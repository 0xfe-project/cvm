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

## What it measures today

Median wall time over seven runs, milliseconds. The current full run is in
`results/`; the environment block at the top of that file is part of the
number.

| scenario | cvm | go | bdwgc | tgc | |
|---|---|---|---|---|---|
| churn-unary | **22.5** | 28.9 | 25.6 | 597 | allocate and drop, nothing survives |
| write-heavy | **0.16** | 0.98 | 0.22 | 3.5 | mutate a dense pointer graph |
| mixed-sizes | **28.6** | 75.0 | 84.1 | 213 | sizes across every allocation path |
| growth-1.5 | **1.18** | 3.78 | 1.36 | 35.7 | live set ×1.49 per round |
| flat-fp | **12.9** | 14.1 | 13.0 | 13.0 | n-body, almost no pointers (control) |
| tree-keep | 47.8 | 47.1 | 45.7 | 1748 | build a tree, drop the last one |
| read-heavy | 27.4 | **14.1** | 18.1 | — | traverse a static graph |

cvm wins five, levels with two, loses read-heavy.

**read-heavy is lost for a reason worth knowing.** Its graph is a chain — each
node reaches only the one before it — so marking discovers one new object per
pop however many threads are looking. It is serial by construction, and work
stealing cannot help it. tree-keep, which is a tree, is the scenario that
tests parallel marking.

**tgc is a floor, not a competitor.** It inserts every allocation into an
open-addressed hash table and finds roots with `setjmp`. It is here as a lower
bound. It also crashes on read-heavy, in its own conservative stack walk.

## Running it

```
pnpm run bench:build     build every backend and binary
pnpm run bench           measure, seven repetitions
pnpm run bench:quick     three repetitions — unreliable on the small
                         scenarios, see the note in results/README.md
```

## Where things are

| path | what |
|---|---|
| `BENCH.md` | the specification: fairness rules, scenarios, what these numbers cannot tell you |
| `harness/` | the interface every backend implements, the timing loop, the CSV |
| `scenarios/` | the seven workloads, shared by all four collectors |
| `cvm/` `bdwgc/` `tgc/` `go/` | one collector each, with its backend and a short README |
| `results/` | dated runs |

Each collector's folder README says what that backend can and cannot report —
they are not interchangeable, and a column one of them leaves at zero is
listed there.
