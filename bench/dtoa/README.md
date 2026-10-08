# bench/dtoa/ — which f64 <-> text implementation we port

Two candidates for the number-to-text and text-to-number path, compiled into
**one binary** at the same optimisation level and measured in **one process**,
because they are close enough that numbers taken in two runs on one machine do
not compare:

| | where it comes from | what it is |
|---|---|---|
| **quickjs** | `refs/quickjs-0.17.0/dtoa.c` | Fabrice Bellard's small correctly-rounded implementation, the one quickjs-ng uses instead of libbf |
| **jnum** | `refs/json-5.0.3/jnum.c` | Jing Leng's implementation, the default dtoa for ljson and the reason ljson advertises speed |

The third row, **ours**, is not a candidate: it is `vm/str/dtoa.c`, the port of
the quickjs implementation into our types and onto a `(ptr, len)` parser. It is
compiled in and measured next to its original so that the port can be checked
value for value rather than taken on trust.

Reproduce with `sh bench/dtoa/run.sh` (add a corpus size and a repeat count to
override the defaults: `sh bench/dtoa/run.sh 1000000 4`).

## What is measured, and against what

The interesting failure mode is not "one is slower". It is that a formatter and
a parser can be **wrong in a matching way in both directions**, so that a
round trip through the pair looks perfect while the text itself is not the
value. Measuring only the round trip would hide exactly that. So there are three
counts, and the two halves are judged by something that is not them:

| column | question | judged by |
|---|---|---|
| **fmt bad** | is the text our formatter wrote the value? | `strtod` — libc, correctly rounded |
| **parse bad** | does our parser read *correct* text as the value? | fed text formatted by quickjs |
| **roundtrip** | what a program does: format with ours, parse with ours | the bits |
| **bytes/val** | is the spelling *shortest*? | total emitted length over the corpus |
| **ns fmt / ns parse** | cost | separate loops, `volatile` sink, strings pre-formatted for the parse loop |

`strtod` is not fast and is not what we will ship. It is correctly rounded,
which is the property under test, and that makes it the right oracle.

## Results

Apple M5, Darwin arm64, `-O2`, 200k finite values per corpus, 8 repeats.

**Corpus A — random bit patterns.** The worst case for the shortest-form
search: most of these need the full 17 digits.

| impl | fmt bad | parse bad | roundtrip | len!=str | bytes/val | ns fmt | ns parse |
|---|---|---|---|---|---|---|---|
| quickjs | **0** | **0** | **0** | 0 | 22.43 | 1060 | 309 |
| jnum | 1349 | 1688 | 3932 | 0 | 22.44 | 17 | 16 |
| ours | **0** | **0** | **0** | 0 | 22.43 | 1067 | 309 |

**Corpus B — JSON-like values.** Small integers, three-place decimals, small
magnitudes, and one in eight drawn from random bits. This is what a JSON
document or a source file actually carries.

| impl | fmt bad | parse bad | roundtrip | len!=str | bytes/val | ns fmt | ns parse |
|---|---|---|---|---|---|---|---|
| quickjs | **0** | **0** | **0** | 0 | 16.01 | 221 | 77 |
| jnum | 232 | 7110 | 18475 | 0 | 16.76 | 15 | 14 |
| ours | **0** | **0** | **0** | 0 | 16.01 | 219 | 80 |

Some of what the counters are counting, from the harness's own output:

    jnum parse: "-0" -> 0, want -0
    jnum parse: "1e+23" -> 1.0000000000000001e+23, want 9.9999999999999992e+22
    jnum parse: "-3700974.2309656325" -> -3700974.2309656329, want -3700974.2309656325
    jnum fmt:   -4.9975892919605044e+165 -> "-4.997589291960504e+165" -> -4.9975892919605036e+165

## The decision: quickjs

**jnum is disqualified by correctness, not by speed.**

Its parser is off by about an ulp on **3.6%** of ordinary JSON-like numbers, and
it loses the sign of `-0` outright. Its formatter emits a 16-digit spelling
where 17 were needed for about **0.1%** of values, so its own output does not
round-trip through an independent parser — while `bytes/val` shows it is not
even saving space, 16.76 against quickjs's 16.01.

The numbers also rule out the tempting hybrid — jnum's fast formatter with
quickjs's parser. Both of jnum's halves are wrong on their own, so there is no
half of it to keep.

For a runtime whose JSON printer is expected to round-trip its input, and whose
parser is expected to agree with the spec's correctly-rounded conversion, ~1
value in 100 is not a trade that can be made for 15x on a 221 ns operation.

**What we pay.** Formatting is 221 ns for a JSON-like value and 1060 ns for a
17-digit one — where a random-bit corpus is genuinely worst case, since the
shortest-form search runs to the end of the digit budget. Parsing is 77 ns / 309
ns. Those are acceptable for a text boundary, and Corpus B is the number that
matters in practice.

## The port is checked here, not assumed

`ours` matching `quickjs` in every column — including `bytes/val`, which is a
total over the corpus and would move if a single value were spelled even one
digit differently — is the evidence that rewriting the file into our types and
its parser onto `(ptr, len)` changed no conversion. The 22.43 against 22.43 is
the part that matters; the timings are inside their own noise.

## Follow-up: the integer fast path

Corpus B says where the remaining cost is. A double that is exactly an integer
and fits in 64 bits needs no shortest-form search at all — it needs `u64toa` and
a check for `-`. That covers the overwhelming majority of JSON numbers, and it
is worth doing once the port is in, measured against these same two corpora so
the improvement is visible rather than asserted.
