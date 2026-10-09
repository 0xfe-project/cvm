# bench — four collectors, one workload each, measured

Four garbage collectors run the same seven scenarios, compiled from the same
sources, built at the same optimization level, and land their rows in one
CSV. This file says what the numbers are and which of them not to trust.

## The four

| | marking | roots | moves objects | built as |
|---|---|---|---|---|
| **cvm** | stop-the-world, parallel | exact (frame chain) | no | the project itself |
| **bdwgc** | stop-the-world, conservative | conservative (stack scan) | no | 8.2.12, `-O1` |
| **tgc** | stop-the-world, conservative | conservative (`setjmp`) | no | 459 lines, `-O1`, three metrics added |
| **go** | concurrent, write barrier | exact | no | its own toolchain and optimizer |

Nobody moves objects — not cvm, not bdwgc, not Go, and not the Commix
reference cvm ports from. So these scenarios measure allocation, marking and
sweeping. They cannot show what a compacting or evacuating collector would do.

## What it measures

Median wall time in milliseconds. **One table per run mode, because a mode is
an experimental condition and rows from two of them are not comparable.**
alloc-only never collects, so it measures the allocator; auto lets each
collector choose, so it measures the policy.

**alloc-only**

| scenario | cvm | go | bdwgc | tgc |
|---|---|---|---|---|
| churn-unary | 18.20 | 30.15 | 31.07 | 627.65 |
| tree-keep | 44.95 | 50.04 | 51.53 | 3762.96 |
| mixed-sizes | 41.17 | 93.47 | — | 223.42 |
| flat-fp | 13.07 | 14.49 | 13.86 | 13.93 |

**auto**

| scenario | cvm | go | bdwgc | tgc |
|---|---|---|---|---|
| tree-keep | 58.56 | 54.95 | 54.26 | 1797.72 |
| mixed-sizes | 14.09 | 72.55 | 88.10 | 230.12 |
| flat-fp | 13.11 | 12.87 | 13.11 | 13.39 |
| read-heavy | 28.42 | 12.97 | 19.33 | — |
| write-heavy | 0.17 | 1.02 | 0.65 | 5.12 |
| growth-1.5 | 1.36 | 3.94 | 2.71 | 35.61 |

The full comparison is in `results/`, and the figures below are generated from
it by `pnpm run bench:svg`.

**The cvm column is the cvm library, compiled the way it is shipped.** It was
not, until it was: the runtime was built hosted at -O1, so it got the
compiler's `memset` for loops the library keeps as loops under
`-ffreestanding`, and it polled for safepoints where the library faults on a
protected page. Both changed the numbers by more than the differences they
were measuring -- 6x on the medium-object workload, 9% on tree-keep's
allocator -- and both were in the flattering direction. The rows before this
one are therefore a different collector and are not comparable with it; the
commit that changed the flags says which rows moved and by how much.

What the two tables say now, and it is not one story:

  - **allocation** is cvm's, and by a margin: 18.20 against 30.15 for Go and
    31.07 for bdwgc on churn-unary, 41.17 against 93.47 for Go on
    mixed-sizes. That is the bump pointer doing what the design says.
  - **medium objects** moved the most of anything here: mixed-sizes in auto
    mode is 14.09 against Go's 72.55 and bdwgc's 88.10. Part of that is the
    clearing leaving the allocation path, which is the change with the
    largest measured effect in this repository so far.
  - **tree-keep** is still bdwgc's and Go's: 58.56 against 54.26 and 54.95.
    Marking a large live set is what this collector pays for, and the
    difference is the per-object cost of its metadata, not its throughput.
  - **read-heavy** is still the worst row: 28.42 against bdwgc's 19.33 and
    Go's 12.97. That row marks a 200,000-node chain, which cannot be
    parallelised at all, so it is a per-object cost with no other thread to
    hide behind -- and it is where the remaining work is.

## The figures

Each is one experimental condition. They are generated, so they cannot drift
from the data.

![Wall time, alloc-only](wall-alloc.svg)

![Wall time, auto](wall-auto.svg)

![Longest stop-the-world pause, auto](pause-auto.svg)

![How much these numbers move between runs, auto](spread-auto.svg)

The last one is worth reading before any of the others. Run-to-run dispersion
across these rows ranges from under 1% to over 500%, so a table of medians
presents a row that swings by a factor of six with the same confidence as one
that barely moves. In the figures, the length of a row's line is that row's
spread — which is why most rows have no visible line and a few have a long
one.

## What is missing, and why

| row | why there is no number |
|---|---|
| `read-heavy × tgc` | tgc's marker recurses once per pointer field and read-heavy's graph is a 200,000-node chain, so marking nests 200,000 frames deep. Stack overflow, in the collector this row exists to compare against. |
| `mixed-sizes × bdwgc × alloc` | a conservative collector cannot reclaim between repetitions — the scenario drops its pointers but stale copies remain on the stack — so the allocations accumulate past any heap worth reserving. |

Both are listed in `scripts/bench.mjs` under `KNOWN_GAPS` and reported as
known gaps rather than failures, because a limitation and a broken harness are
different things and a reader should not have to work out which they are
looking at.

## Running it

```
pnpm run bench:build     build every backend and binary
pnpm run bench           measure, seven repetitions
pnpm run bench:svg       redraw the figures from the latest results
pnpm run bench:all       run, then redraw
pnpm run bench:quick     three repetitions — unreliable on the small
                         scenarios; see results/README.md
```

## Where things are

| path | what |
|---|---|
| `harness/` | the interface every backend implements, the timing loop, the CSV |
| `scenarios/` | the seven workloads, shared by all four collectors |
| `cvm/` `bdwgc/` `tgc/` `go/` | one collector each, with its backend and a short README |
| `results/` | the run the figures are drawn from |

Each collector's folder README says what that backend can and cannot report —
they are not interchangeable, and a column one of them leaves at zero is
listed there.
