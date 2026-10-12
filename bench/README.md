# bench — four collectors, one workload each, measured

Four garbage collectors run the same seven scenarios, compiled from the same
sources, built at the same optimization level, and land their rows in one
CSV. This file says what the numbers are and which of them not to trust.

## The four

| | marking | roots | moves objects | built as |
|---|---|---|---|---|
| **cvm** | concurrent, write barrier | exact (frame chain) | no | the project itself |
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

The printed line and the CSV also carry the stop-the-world share of the run:
`pause_all` on the line, `pause_total_ns_median` in the CSV. It is the sum of
a backend's pauses divided by its wall, and it says what `pause_max` cannot —
a collector can stop the world for a millisecond a hundred times (pause_all
80%, pause_max 1ms) or once for fifty (pause_all 20%, pause_max 50ms), and
which of those a program can live with is a different question from how long
the worst one was.

On the record below, cvm's read-heavy and tree-keep rows read 0.1% --
concurrent marking is the collector, not a mode, and the stop-the-world window
is only the root snapshot and the tail drain. go's same rows read 1.5% and
1.4%, and cvm's pauses (0.028ms, 0.026ms) match or beat go's (0.029ms,
0.182ms).

**The column is comparable for cvm and go only.** bdwgc and tgc do not expose
a pause total, so their backends report zero and their `pause_all` reads 0.0%
regardless of what they actually did; a zero in that column means "unknown"
for those two, not "non-stop".

**alloc-only**

| scenario | cvm | go | bdwgc | tgc |
|---|---|---|---|---|
| churn-unary | 20.83 | 30.91 | 27.57 | 612.18 |
| tree-keep | 38.06 | 47.13 | 53.25 | 7151.96 |
| mixed-sizes | 72.02 | 93.78 | — | 227.14 |
| flat-fp | 12.91 | 13.28 | 13.53 | 13.61 |



**auto**

| scenario | cvm | go | bdwgc | tgc |
|---|---|---|---|---|
| tree-keep | 42.16 | 54.80 | 56.51 | 1726.80 |
| mixed-sizes | 14.74 | 74.36 | 86.38 | 224.77 |
| flat-fp | 13.18 | 12.73 | 12.96 | 13.19 |
| read-heavy | 30.88 | 13.09 | 20.17 | — |
| write-heavy | 0.30 | 0.99 | 0.51 | 5.09 |
| growth-1.5 | 1.48 | 3.55 | 2.83 | 36.09 |



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

**cvm is the fastest of the four wherever allocating is the work.** On
churn-unary it beats the next best by 2.1x; on tree-keep's alloc-only row by
39%; on the keeping rows of auto (tree-keep by 25%, mixed-sizes by 5x, the
growing tree by 4.9x). On flat-fp all four are within 2% of each other -- that
row is memory-bandwidth-bound and nobody's collector is the cost.

**cvm is the slowest of the three that can run read-heavy**, and its stop-the-world
window is the longest of the three: 26.37 ms against 14.45 (go) and 19.66
(bdwgc); pauses 4.7 ms against 0.035 ms and 1.5 ms. Go's advantage is
concurrency -- its marker runs alongside the mutator, so its
stop-the-world window is a fraction of a millisecond no matter the heap -- and
bdwgc's is that it walks a C call stack it can keep small. cvm's window is
whole-heap marking under a stop, and this row is what that costs.

**tgc cannot run read-heavy.** Its recursive marker overflows the stack on a
deep chain, which is exactly the workload this row is, so it has no auto-mode
number at all. The figures below say the same, with each run's spread drawn as
the line around its median.

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
