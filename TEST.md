# Testing cvm's collector

This document records how the collector is tested and why. It is written
before most of the collector exists, so that the harness is built to fit
the oracles rather than the other way round.

## Why GC testing is unlike other testing

Three properties make a collector hostile to ordinary testing:

1. **The failure is usually not a crash.** A wrong liveness decision
   produces a program that runs and returns wrong answers, or one that
   leaks until it dies, hours later. There is no assert to trip at the
   point of the mistake.
2. **The bugs are timing-dependent.** Most of them need a specific
   interleaving of mutator threads, GC worker threads and safepoints, so
   they reproduce rarely and on other people's machines.
3. **The code under test is its own judge.** If the marker and the
   sweeper share a misunderstanding about object layout, they will agree
   with each other and be wrong together. Tests must therefore compare
   against something outside the collector: a payload the test wrote, a
   weak reference, or a poisoned page.

So the strategy is: make the wrong answer *visible*, with oracles that do
not share the collector's assumptions, then run those oracles under
concurrency and under sanitizers.

## The four liveness oracles

For a given object, after a collection there are four possible outcomes.
Two are correct, two are bugs, and they are not equally bad.

| | Object is reachable | Object is unreachable |
|---|---|---|
| **Kept alive** | **TP** — correct | **FP** — floating garbage |
| **Reclaimed** | **FN** — catastrophic | **TN** — correct |

- **TP (true positive, correct).** A reachable object survives, and its
  contents are intact. Detected by writing a canary payload into the
  object before the collection and verifying it afterwards. A collector
  that traces the wrong offsets — or the wrong type descriptor — corrupts
  the payload or drops the object.
- **TN (true negative, correct).** An unreachable object is reclaimed.
  Detected with a weak reference: the collector itself reports the
  decision by clearing the weak slot. Also observable as heap usage
  falling, or as a block returning to the free list.
- **FP (false positive: floating garbage).** An unreachable object is
  kept. Not a correctness bug — nothing reads a dead object — but a space
  leak, and the classic symptom of a conservative scan mistaking a
  pointer-shaped integer for a root. Detected the same way as TN, by the
  weak reference *not* being cleared.
- **FN (false negative: a live object is reclaimed).** The fatal one. The
  mutator later reads or writes freed memory. This is what the whole
  apparatus below exists to catch, because in production it looks like
  memory corruption somewhere else entirely.

TP and FP are cheap to test. FN is the hard one, and it needs a mechanism
that turns a silent wrong answer into a loud failure — see poisoning.

### Weak references are the collector's own confession

A weak reference is the only place where the collector states its
liveness decision in a way a test can read. Everything else about a
collection is inferred. This makes weak references the primary oracle for
TN and FP, and is a reason to implement them early rather than as a
late feature: they are test infrastructure as much as a language feature.

### Canary payloads

Every test object gets filled with a value derived from its address and a
per-test seed. After a collection, each surviving object is checked
against its own expected pattern. This catches:

- the object being moved without updating a reference
- the object being scanned with the wrong descriptor, so a pointer field
  is treated as a scalar or vice versa
- partial overwrites from a neighbouring object's sweep

A canary is also the cheapest way to notice that the marker wrote into
the payload region because it mis-computed where the payload starts.

## Turning silent failures into loud ones

### Poisoning reclaimed regions

The single most valuable trick. AddressSanitizer does not understand a
custom heap — it only knows about malloc — but it exposes manual control:

```c
#include <sanitizer/asan_interface.h>

/* at sweep, when an object is reclaimed */
__asan_poison_memory_region(obj, size);

/* at allocation, when the slot is handed out again */
__asan_unpoison_memory_region(obj, size);
```

With this, any stale access to a reclaimed object faults immediately with
a stack trace pointing at the accessor. An FN stops being a corrupted
value three hours later and becomes a test failure at the moment of the
mistake. This is strictly better than valgrind here, because valgrind has
the same blind spot about our heap.

### Internal consistency checks

Debug builds carry a checker that validates the collector's own
invariants after each phase — Commix has this as `GC_ASSERTIONS` plus
`Sweeper_AssertIsConsistent`, and it is worth porting:

- every allocated slot in the bytemap is inside a block marked in use
- no line is marked free while holding allocated objects
- the free-line lists are acyclic and stay inside their block
- the sum of free blocks plus in-use blocks equals the block count
- a marked block's marked lines are exactly the lines the marker touched

These catch the class of bug where the marker and the sweeper agree with
each other but disagree with the heap. They are also the cheapest bug
detector we have, because they run in the same process with full
knowledge of the data structures.

### Negative controls

Any check that can pass silently must be demonstrated to fail at least
once, or it is decoration. We have already applied this to the harness
itself:

- TSan's silence about the block lists was only trusted after an
  unsynchronised counter was shown to produce a report
- `cvm_assert` was confirmed to fire (it caught a real bug on its first
  day) and to compile out in release
- `scripts/ci.mjs` was confirmed to stop and exit non-zero on a
  deliberately unformatted file
- the freestanding audit was confirmed to catch both a libc include
  (blocked by `-nostdinc`) and a hand-declared libc prototype (caught by
  the object scan)

New oracles and new checks should come with the same demonstration.

## Test tiers

```
tests/unit/       pure functions and data structures; no collector needed
tests/gc_oracle/  the four oracles above; needs a working collector
tests/stress/     the patterns below; parameterised by threads and sizes
tests/e2e/        a toy frontend compiled through the real pipeline
```

Tests are **hosted**: they link libc and may use pthreads and stdio
freely. The code under test is **freestanding**. Keeping the split sharp
means the test harness can never accidentally become a dependency of the
runtime, and the freestanding audit stays meaningful.

## Stress patterns

Each is a separate binary, parameterised by thread count, object sizes and
survival rate, so a single pattern can be swept across configurations.

| Pattern | Shape | What it targets |
|---|---|---|
| **Allocation churn** | many short-lived objects, high rate | bump fast path, slow path, lazy sweep keeping up |
| **Read-heavy** | large long-lived graph, frequent traversals | marker throughput, cache behaviour of tracing |
| **Write-heavy** | high mutation rate on an existing graph | root slot updates; later, write-barrier cost |
| **Fragmentation** | random sizes, random lifetimes | line-level holes, evacuation trigger heuristics |
| **Large objects** | allocations above the 8 KiB threshold | superblock allocation, split, and return |
| **Mixed, multithreaded** | several mutators running the above concurrently | safepoint coordination, grey packet handoff |
| **Thread storm** | threads registering and unregistering rapidly | thread registry races, blocks held by dead threads |
| **FFI boundary** | frequent managed/unmanaged transitions during collections | frames marked in-FFI, pinning, external handles |

Two of these deserve a note:

- **Fragmentation** is the one that finds Immix-specific bugs. It needs
  sizes chosen so that allocations land on line boundaries in awkward
  ways, and lifetimes chosen so that holes accumulate without ever
  becoming a full free block.
- **FFI boundary** is where pinning is exercised. A test that pins an
  object, forces a collection, and then verifies the object did not move
  is the only way to know evacuation honours the pin.

## Borrowing from other collectors

Nobody should design a GC test suite from scratch. These are worth
reading before writing one, in rough order of usefulness for us:

| Project | Where | What to take |
|---|---|---|
| **MPS** (Ravenbrook) | <https://github.com/Ravenbrook/mps> | The closest thing to a reference GC test suite in C: random object graphs with exact roots, verified for corruption. Apache 2.0 |
| **bdwgc** (Boehm) | <https://github.com/ivmai/bdwgc> | Thread registration/unregistration storms and repeated init/collect cycles; GCBench lives in its `tests/` |
| **Scala Native** | <https://github.com/scala-native/scala-native> | The collector we are porting from, so its unit tests express the same semantics; useful as a diff of intent |
| **MMTk** | <https://github.com/mmtk/mmtk-core> | Test architecture for a pluggable collector: how to test a GC independent of its host |
| **CRuby** | <https://github.com/ruby/ruby> | `test/ruby/test_gc.rb`: finalizer ordering, weak map behaviour, stress-mode edge cases |
| **Go** | <https://github.com/golang/go> | Mutation patterns during concurrent marking; write-barrier stress |

GCBench is in the bdwgc tree rather than at its own URL. The DaCapo and
Immix paper benchmarks are Java/C and not directly usable, but their
*shapes* — allocation-heavy, pointer-chasing, and multi-threaded phases —
are worth copying even when the code is not.

## Harness

[greatest](https://github.com/silentbicycle/greatest), vendored at
`tests/vendor/greatest.h`. Single header, ISC licensed, no build system,
readable output, per-test filtering via `scripts/run-tests.mjs`.

## Running things

```
pnpm run test          # debug
pnpm run test:asan
pnpm run test:tsan
pnpm run test:release
pnpm run ci            # all of the above plus the format check
node scripts/run-tests.mjs bytemap      # one binary by substring
```

The sanitizer configurations are not redundant. asan catches
out-of-bounds and use-after-free; tsan catches data races, and the
concurrent tests mean nothing without it. Both are run in debug and
release, because a bug that only appears with optimisations on is a real
category.

`valgrind` is available on the development machine and can serve as a
second opinion on metadata allocations and on the test harness itself,
but it does not understand the GC heap — the same blind spot as ASan,
without the manual poisoning that makes ASan useful here.

## Status

Batch 1 is complete. What exists:

- `tests/unit/`: 6 binaries, 61 tests, about 9k assertions, green under
  debug, release, asan and tsan
- the freestanding audit, running in every configuration
- the CI matrix driver
- `cvm_assert`, which caught a real overflow bug on its first day

What does not exist yet, and is the reason this document is mostly
forward-looking: any collector. The oracles need a marker and a sweeper;
the stress patterns need an allocator. The order they get built in is the
order in the porting plan, and each one brings its tests with it.
