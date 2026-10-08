# BENCH.md — comparing cvm's collector against other collectors

This is a specification, not a report. It says what will be measured, how,
and what each measurement will and will not be able to tell you. Numbers go
in `bench/results/`, each with the date, the machine and the flags.

The point of the exercise is not to win. It is to find out where this
collector's design is actually better and where it is actually worse, with
enough discipline that the answer means something.

## 1. The four collectors

| | What it is | Marking | Moving | Reachability |
|---|---|---|---|---|
| **cvm** | this project | stop-the-world, parallel, exact | no | exact |
| **bdwgc** | Boehm-Demers-Weiser 8.2.12 | stop-the-world, exact | no | **conservative** |
| **tgc** | a 459-line tiny GC (`bench/tgc`) | stop-the-world, single-threaded | no | **conservative** |
| **go** | the Go runtime | **concurrent**, write barrier | no | exact |

### Four things that make this comparison unfair, stated up front

Every one of these cuts in someone's favour, and a document that omitted
them would be inviting its own numbers to be misread.

**tgc is a floor, not a competitor.** It inserts every allocation into an
open-addressed hash table and scans roots with `setjmp`. Its per-allocation
cost is a hash probe and its collection cost includes rebuilding a table. It
belongs here as a *lower bound* — what a collector that makes no design
investment achieves — and a cvm that fails to beat it comfortably has a
problem. Reporting it as though it were a peer would flatter us.

**bdwgc is conservative.** It cannot know that an integer is not a pointer,
so it keeps objects alive that a precise collector would reclaim, and it
must scan whole stacks word by word rather than reading a frame's slots.
That is a real cost it pays, and it buys robustness to a frontend that gets
its stack maps wrong. It is also thirty years old and has been tuned by
many people; that maturity is its own advantage.

**Go's collector is a different design with a different goal.** It marks
concurrently with a write barrier, so it pays on every pointer store and
buys short pauses. It also ships its own scheduler and allocator, so a Go
number includes machinery that has nothing to do with collection. Go's
pauses should beat a stop-the-world collector's — that is the design
working, not an accident — and its throughput should suffer for it. Do not
read either as a defect.

**cvm has no evacuation, and neither do the others.** Confirmed by reading:
the Commix reference this project ports from has no object moving at all
(the whole `refs/` tree contains no evacuation, forwarding, or relocation
code), and Go's GC does not move heap objects either. So a fragmentation
scenario measures allocation policy and sweep efficiency, *not* compaction.
It will not show what an evacuating collector would do, because there is no
evacuating collector here to show it.

One more that is cvm's alone, and needs stating because it makes our numbers
look better than they are: **cvm's sweeping is deferred.** The collector
starts the sweep and resumes the program; the sweeping is then paid for by
whichever thread needs memory. A scenario that stops allocating at the end
would hide the sweep's tail entirely. Every scenario therefore forces a
`cvm_collect()` — with the sweep driven to completion — *inside the timed
region*, and `bench/results/` records that this was done.

## 2. Fairness rules

Each rule is checkable, and each says how.

### 2.1 Optimization level

**C and C++ backends compile at `-O1`. Go uses its default.**

`-O1` rather than `-O2` because the C code a frontend generates is not
heavily optimized by hand, and `-O1` is closer to what the generated code
would be built with.

Go has no equivalent knob: `gc` is always fully optimizing. So the honest
statement is "C at `-O1`, Go at its default", not "both at `-O1`". Every
result records the exact flag set for every backend, and a comparison that
depends on the difference is called out as such.

Both are built with debug information off and with the same allocator
alignment assumptions where those are controllable.

### 2.2 Every allocation must actually happen

This is the rule that matters most, and it exists because Go's escape
analysis will silently turn an allocation into a stack slot. A Go
implementation that allocates a million objects the compiler can prove do
not escape will report a million allocations that the GC never saw, and
will look arbitrarily fast.

Two defences, both applied to every backend:

1. **A sink.** Every allocated object's address is written into a global
   that the compiler cannot prove is dead — for Go, a package-level slice
   appended to and never read; for C, a `volatile` global pointer. This
   makes escape analysis unable to remove the allocation.

2. **A runtime cross-check.** Each backend reads its collector's own
   allocation counter before and after the timed region, and asserts the
   count is within a few percent of what the scenario asked for:

   | backend | counter |
   |---|---|
   | cvm | `cvm_vm_stats().objects_live`, plus collections from `cvm_gc_last_collection` |
   | bdwgc | `GC_get_heap_size()`, plus `GC_gc_no` |
   | tgc | `gc.nitems` and `gc.nslots` |
   | go | `runtime.MemStats.Mallocs`, plus `NumGC` |

   A run whose counter disagrees with the scenario is **invalid**, not slow.
   The harness prints the counters for every run so this is auditable
   rather than trusted.

### 2.3 cvm's workloads follow the code-generation protocol

A cvm workload that allocated without pushing frames would skip the
safepoint load that generated code performs at every function entry, and
the per-call frame push. It would be measuring a runtime that does not
exist.

So every cvm workload pushes a frame sized to its live pointers, keeps
those slots current across every allocation, and pops on the way out —
exactly the shape `tests/e2e/test_smoke.c` uses. The traversal and building
loops are otherwise identical, token for token, to the other backends'.

Root registration is the one place the backends legitimately differ, and it
is part of what is being measured: cvm uses frames, bdwgc relies on its own
stack scan plus a registered static root array, tgc sets `TGC_ROOT`, Go
holds a slice. Writing them the same way would be measuring a protocol
nobody uses.

### 2.4 Same algorithmic work

For each scenario the traversal, the object sizes, the graph shape and the
number of allocations are identical across backends. Differences are
confined to allocation calls and root registration. The scenarios live in
one file per scenario, with the backend-specific parts behind a small
interface, so the loops can be diffed by eye.

### 2.5 Each backend gets one clean run first

Every backend runs a warm-up pass that is not timed, so a first-collection
heap-growth spike does not land in the measurement, and every timed result
is the median of K runs rather than the best.

## 3. Three run modes

A collector's throughput depends more on *when* it collects than on how fast
it collects. Measuring only the default policy compares tuning, not design.
So every scenario runs in three modes.

| Mode | How it is driven | What it isolates |
|---|---|---|
| **alloc-only** | heap reserved large enough that no collection happens for the whole run | the allocator: bump pointer versus free list versus hash table. This is where cvm's design should win, and the size of the win is the interesting number. |
| **fixed-cadence** | every backend is forced to collect at the *same allocation count*: every N allocations, call the backend's collect. N is a scenario parameter. | the collector's mechanism — mark, sweep, root scan — with triggering policy removed. |
| **auto** | each backend uses its own default policy | the policy: heap growth decisions, when to collect, how much to wait for. This is where a twenty-year-old default will beat a fresh one. |

A backend that cannot be forced to collect on demand is reported as
unable to run that mode rather than approximated.

## 4. Object shapes

Chosen by axis, not by picking famous benchmarks, so that a result can be
attributed to one property rather than explained away.

### Axis A — pointer density

| shape | bytes | pointers | role |
|---|---|---|---|
| `small_node` | 24 | 2 | the common case |
| `fat_node` | 136 | 2 | **controlled pair with `small_node`** |
| `wide_node` | 96 | 8 | the write-heavy shape |
| `leaf_blob` | 128 | 0 | the no-pointer control |

`small_node` and `fat_node` are the most informative pair in the suite:
identical pointer count and identical marking work per object, five times
the bytes. The ratio between them says directly whether line-and-block
granularity is doing anything. A collector that only cares about object
counts will show no difference; one that cares about bytes will.

### Axis B — lifetime structure

| shape | description | live set |
|---|---|---|
| `churn` | allocate, drop immediately | ~0 |
| `tree` | GCBench shape: build a tree, drop the previous one, keep one | O(2^d) |
| `chain` | one long singly-linked list | the whole list |
| `growing` | the live set multiplied by ~1.5 each round |

`tree` is the primary scenario: it is the only shape that exercises marking
and sweeping at the same time with a live set worth the name. `chain` is
there for locality — pointer chasing is where cache behaviour shows.

### Axis C — access pattern

Two scenarios requested specifically, because they separate costs that
`tree` blends together.

| scenario | shape | what it isolates |
|---|---|---|
| `read-heavy` | build a large graph once, then traverse it many times with about one write per hundred reads | mark throughput and cache behaviour, with mutation cost nearly removed |
| `write-heavy` | repeatedly mutate a dense graph (`wide_node`), with few traversals | pointer-store cost: for cvm and bdwgc this is a plain store, for Go it goes through the write barrier. **A cvm win here is Go paying for concurrency, not Go being slow.** |

`write-heavy` measures the boundary of the design difference: our marking
is stop-the-world, so our stores are free and our pauses are long; Go's
stores are taxed and its pauses are short. Reporting one without the other
would be reporting half a trade.

### Axis D — size distribution

| shape | description |
|---|---|
| `uniform-small` | one size, well inside a line |
| `uniform-large` | one size past the large-object threshold |
| `mixed` | sizes drawn at random **across cvm's two boundaries** (256-byte inline limit, 8 KiB large-object threshold), with random lifetimes |

`mixed` is the fragmentation scenario, and it is also the one that walks
every allocation path cvm has. Its seed is fixed and printed, so a result
that surprises can be reproduced exactly.

### Axis E — controls

| scenario | description | why |
|---|---|---|
| `flat-fp` | an n-body kernel over a flat array, almost no pointers | the control that makes everything else interpretable. A pointer-tracing collector should do nearly nothing here. **If a backend is slow here, the cause is its allocator or its safepoint mechanism, not its marking** — and without this, a slow number elsewhere cannot be attributed. |
| `blob-churn` | variable-size byte blocks, no pointers, sizes crossing the medium and large paths | isolates the size-class machinery from the tracing machinery |

## 5. The growth scenario

Requested specifically: what happens when the live set grows at a rate near
1.5.

`growing` multiplies the live set by **1.49** per round and runs until a
configurable ceiling. It is not a throughput scenario — it is a *policy*
scenario, and it has a prediction attached, which is what makes it worth
running:

> cvm's heap growth policy doubles below 128 MiB and multiplies by √2
> (≈ 1.414) above it. A live set growing at 1.49 therefore **outpaces the
> heap once past the early threshold**, and cvm should collect more often
> per unit of work above 128 MiB than below it. If it does not, either the
> policy is not firing as written or the free-block condition is masking
> the mark-time condition.

The scenario reports, per round: live set, heap size, cumulative
allocations, collections so far, and time spent collecting. From those the
interesting derived figures fall out:

- **collections per megabyte allocated**, below and above the threshold
- **peak RSS ÷ live set** — the space the policy wastes, which is the cost
  it pays for collecting less often
- **time in collection ÷ total time** — what the program actually loses

The same figures are produced for bdwgc and Go with their own defaults, so
the comparison is between policies rather than between implementations. Go's
`GOGC` is recorded in `bench/results/`; a Go number without it is not
comparable to anything.

## 6. Scenarios for the first pass

Seven, ordered by how much they will tell us rather than by how flattering
they are.

| # | scenario | axes | modes | expected to favour |
|---|---|---|---|---|
| 1 | `churn-unary` | A:small, B:churn, D:uniform-small | alloc-only, fixed | cvm — bump allocation is the design's core claim, and this is its ceiling |
| 2 | `tree-keep` | A:small, B:tree, D:uniform-small | all three | unresolved; this is the main event |
| 3 | `mixed-sizes` | A:all, B:churn, D:mixed | all three | unresolved; exercises every path and the fragmentation behaviour |
| 4 | `flat-fp` | E:flat | alloc-only, auto | cvm — almost no pointers means almost no marking, and per-object allocation cost dominates |
| 5 | `read-heavy` | A:small, B:tree, C:read | auto | cvm — a single-threaded stop-the-world marker over a static graph, against a concurrent one paying barrier costs |
| 6 | `write-heavy` | A:wide, B:tree, C:write | auto | go — this is where our stop-the-world free store meets Go's barrier tax |
| 7 | `growth-1.5` | A:small+blob, B:growing, D:mixed | auto | bdwgc — thirty years of tuning against a policy written last week |

Predictions are recorded so that being wrong is informative. A result that
matches a prediction is weak evidence; one that contradicts it is worth a
paragraph.

## 7. Metrics

One number is not a result. Each scenario reports:

**Throughput** — wall time for a fixed amount of work, median of K runs.
Reported alongside the allocation counters (§2.2) so a run that did not do
the work is visible.

**Pause behaviour** — maximum single-collection time and p99 across the
run. This is where cvm and Go differ by design, so it is reported for both
without implying either is wrong.

For cvm, the pause is the stop-the-world window: **mark, nullify, and the
sweep's setup — but not the sweep itself.** Read from the collector: the
measured collection duration ends after `prepare_for_sweep` (which empties
the block allocator and resets the sweep cursors) and the world is released
just after that, so the sweeping proper happens with the program running.
That distinction has to be in the output, or cvm's pauses will look
artificially good next to collectors that sweep inside the stop.

That inaccuracy was real and is fixed: the world used to be released a few
statements *after* the measured duration ended, so the counter update and the
log line happened while the world was still stopped and were not counted.
The gap grew with the log level — a debug-level run wrote more, stopping the
world longer while reporting the same collection time. The release now
happens before the recording, and the recording happens with the world
running.

**Space** — heap size, peak RSS, and peak RSS ÷ live set.

**Collector's own accounting** — collections, total time in collection, and
whatever each backend can report: cvm's per-phase event log
(`CVM_LOG_LEVEL=info`, or the stats channel, which the collector writes to
now), bdwgc's `GC_get_gc_no` and heap statistics, Go's `MemStats`, tgc's slot
counts. Each backend's folder README lists what that one can and cannot
report; bdwgc's live-object count, for instance, is zero, because it tracks
bytes.

Every figure gets the same treatment: the harness prints raw per-run values
and the derived summary, so a surprising summary can be checked against the
runs that produced it.

## 8. Layout

```
bench/
├── BENCH.md              this document
├── harness/
│   ├── collector.h       the interface every backend implements
│   ├── run.c             timing loop, modes, counters, reporting
│   └── report.c          median, percentile, CSV out
├── backends/
│   ├── backend_cvm.c
│   ├── backend_bdwgc.c
│   ├── backend_tgc.c
│   └── backend_go/       a Go module; same scenarios, same counters
├── scenarios/
│   ├── scenario.h
│   ├── churn_unary.c
│   ├── tree_keep.c
│   ├── mixed_sizes.c
│   ├── flat_fp.c
│   ├── read_heavy.c
│   ├── write_heavy.c
│   └── growth.c
├── bdwgc/                the release tree (ignored) plus our build glue
├── tgc/gc/               vendored tiny GC
└── results/              dated CSVs and a short note per run
```

The C side is one binary per scenario, linked three times with a different
backend — the same source, so the workload cannot drift between backends.
The Go module repeats the scenarios in Go; there is no way to share the
source across languages, so the Go copies are checked line by line against
the C ones and any difference is written in a comment where it occurs.

Build and run through the same `scripts/` tooling as the rest of the
project — `pnpm run bench` — so there is no second build system to drift.

## 9. What these numbers will not tell you

Worth reading before reading the results.

- **Nothing about a real workload.** Every scenario here is synthetic and
  allocation-heavy. A program whose GC cost is dominated by a few very
  large graphs, or by FFI, or by coroutine churn, is not represented.
- **Nothing about tail latency under memory pressure.** All scenarios run
  with the heap free to grow. What happens when it cannot is a different
  measurement, and a more interesting one.
- **Nothing about correctness.** Speed on a broken collector is
  meaningless; the correctness evidence is `TEST.md`.
- **Little about the frontends' costs.** Generated code carries frame
  pushes, safepoint loads and slot updates that a hand-written benchmark
  approximates but does not reproduce. cvm's numbers are optimistic for
  that reason, and the workloads are written the way they are (§2.3) to
  keep the optimism small and known.
- **Nothing about Windows**, which has no backend yet, and nothing about
  evacuation, which no collector here implements.
