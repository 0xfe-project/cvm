# bench/json — three JSON paths, one corpus

`json-bench.c` compiles all three implementations into one binary and runs them in
one process on the same bytes, so the columns are comparable without a control run.

```sh
sh bench/json/run.sh 7        # 7 rounds, averaged
```

| impl | what it is |
| --- | --- |
| `ours` | `vm/json.c` — every node a GC object, every string a `CvmString` |
| `ljson` | `refs/json-5.0.3` — a block pool, `json_fast_parse_str`, strings reused in place |
| `yyjson` | `refs/yyjson-0.13.0` — a read-only tape, SIMD scanning |

## The columns

`parse` builds the document. `navigate` walks it and touches every node.
`materialize` turns **every string the program can read — member names included**
into an interned `CvmString`; ours reads 0 because parse already made them, which
is the whole point of that column. `print` serializes back to text.

Averaged over rounds, not best-of: the collector is live, the previous round's
document is garbage by now, and a collection that lands inside a parse is a cost a
real program pays. Each round re-copies the corpus, because both fast modes write
into their input (ljson's reuse mode and yyjson's in-situ mode both want a writable
buffer — a `const str` from our runtime is not one, and that copy is not timed).

## The numbers

arm64 macOS, `-O2`, 7 rounds, one process. µs.

| corpus | bytes | impl | parse | navigate | materialize | print |
| --- | --- | --- | --- | --- | --- | --- |
| numbers (50k ints) | 334126 | ours | 2109 | **85** | 0 | 610 |
| | | ljson | 345 | 153 | 147 | 193 |
| | | yyjson | **152** | 111 | 67 | **80** |
| records (5k × 4 fields) | 318671 | ours | 2180 | 146 | 0 | 1258 |
| | | ljson | 345 | 276 | 818 | 142 |
| | | yyjson | **114** | **63** | 591 | **97** |
| strings (2k × 108 B) | 216001 | ours | 672 | 112 | 0 | 672 |
| | | ljson | 162 | 127 | 432 | **52** |
| | | yyjson | **43** | 109 | 812 | 50 |

## What they say

**The gap is per-node allocation, not scanning.** Our parse is 4–6x ljson's and
4–15x yyjson's, and it is worst exactly where nodes are cheap and numerous (50k
numbers, 0 strings). A number costs us a GC object. The scanner is not the problem.

**Our navigate is already competitive, and best on flat arrays** (85µs vs 153/111).
A JSON array is a `CvmRefArray`, so a walk is one pointer per element. ljson's
276µs on `records` is its `json_get_items` materializing a flat item array per
container — 5001 calls for 5000 objects.

**The boundary is ours, and it is not free.** Materialize is ~33ns per short string
and ~2–4ns per byte on long ones, and it is *our* code: UTF-8 validation, a GC
allocation, a hash, a probe. No vendored parser can take that away, because the
strings have to become `CvmString`s before the language can use them. That is the
number that bounds how much a special case can buy:

| corpus | ours to a usable doc | ljson + materialize | yyjson + materialize |
| --- | --- | --- | --- |
| numbers | 2109 | 492 | 219 |
| records | 2180 | 1163 | 705 |
| strings | 672 | **594** | 855 |

On a string-heavy document a vendored parse buys almost nothing. On a scalar-heavy
one it buys 2–6x.

**Our printer is 3–13x slower and that is plain work to do.** 12ns per number
against ljson's 4. It goes through the builder and re-escapes; neither has to cost
what it costs.

## Where that leaves the design

1. Keep `CvmJson` as the editable value model — its navigation is competitive, and a
   document you can already read needs no materialize step.
2. Fix the two measured weaknesses inside our own design: the one-object-per-node
   representation (the largest single win, and it also shrinks GC pressure during
   parse) and the printer. Do not adopt a second value model to avoid fixing ours.
3. If a vendored fast path is added, it is a **parse-only** path producing a borrowed
   document, materialized on first touch. The measured ceiling on that is the
   `+materialize` column above, not the `parse` column — the tempting 15x on `parse`
   alone is not a number anyone gets to keep.

## Two traps that cost time

- **`yyjson_obj_iter_next` returns the *key*.** The value is `yyjson_obj_iter_get_val(key)`.
  Iterating values means calling that; a walker that treats the iterator's result as
  a value silently visits member names instead of contents, and both its navigate
  and materialize columns come out wrong.
- **`cvm_intern` allocates, so the candidate must be in a root slot across the call.**
  A growing map builds a bigger key array, and that allocation may collect. A
  harness that holds the new string in a C local crashed at 7 rounds and did not
  crash under a debugger — it looked exactly like a GC bug, and was a missing frame.
