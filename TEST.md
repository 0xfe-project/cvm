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

**Not implemented.** This is the technique the design most wants and the
suite does not yet have, so it is written down here as a plan rather than
described in the present tense.

The idea: AddressSanitizer does not understand a custom heap — it only
knows about malloc — but it exposes manual control:

```c
#include <sanitizer/asan_interface.h>

/* at sweep, when an object is reclaimed */
__asan_poison_memory_region(obj, size);

/* at allocation, when the slot is handed out again */
__asan_unpoison_memory_region(obj, size);
```

With this, a stale access to a reclaimed object faults immediately with a
stack trace pointing at the accessor. An FN stops being a corrupted value
three hours later and becomes a test failure at the moment of the mistake.
That is strictly better than valgrind here, because valgrind has the same
blind spot about our heap.

Where it would go, and the one wrinkle: `vm/` may not include a sanitizer
header, because that would break the rule that `vm/` is libc-free and the
freestanding audit would say so. It has to be reached through `rt/` — two
functions, `rt_mem_poison` and `rt_mem_unpoison`, which are no-ops in an
ordinary build and call the sanitizer's API when built with it. The
sweeper calls one and the allocator calls the other, and nothing else
changes. That is a small piece of work, and it is the piece that would
make the FN oracle as loud as it can be made.

### Internal consistency checks

**Partly implemented**, and what is there is narrower than the plan below.

What exists is `cvm_sweep_assert_consistent`, called once per collection,
before marking, in a debug build. It validates that the *previous* sweep
left the heap in a state the allocator can use:

- no block is still marked
- no line is still marked
- no object slot is still marked
- no `coalesce_me` block survived the sweep
- no `superblock_start_me` block survived it
- no superblock tail is reached directly, which would mean its head was
  never swept

That is a real check and it has fired: it was what caught the very first
sweep leaving `coalesce_me` blocks behind, when coalescing ran ahead of
sweeping. A mark leaking past a sweep is a bug that otherwise shows up as
a stale object being handed out twice, much later and much less obviously.

There is one more, narrower check, and its scope is worth stating because
it reads as broader than it is: `object_meta_assert_valid_allocation`, in
the allocator's slow path, asserts that every slot a new object covers was
free. It fires if the free-line list hands out a line still in use. It does
*not* cover the inlined fast path, which is most allocations, because that
path lives in a freestanding header that deliberately does not include the
metadata header. Confirmed to fire by marking a slot allocated that the
next allocation would cover.

What the plan above also asked for, and what is not there:

- that every allocated slot lies inside a block marked in use
- that the free-line lists are acyclic and stay inside their block
- that free plus in-use blocks accounts for the block count
- that a marked block's marked lines are exactly the lines the marker touched

The first two are the interesting ones: a cyclic free-line list is an
infinite loop in the allocator, and a slot outside a live block is a
use-after-free waiting for the next allocation. Both are cheap to check and
neither is checked. They belong with the stress suite, because a single
hand-built heap is unlikely to contain either.

The reference has this as `GC_ASSERTIONS` plus `Sweeper_AssertIsConsistent`,
which is a wider net than what is here.

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
- TSan was confirmed live by feeding it an unsynchronised counter before
  its silence about the block lists was trusted
- the per-binary timeout was confirmed by writing a test that never
  returns, and watching the runner report it as a failure rather than
  hanging
- the stuck-thread diagnostic fires, and one test provokes it on purpose;
  that warning therefore appears in every run, labelled

One more, from the other direction: the lost-packet diagnostic earned its
keep before it was ever needed as a demonstration. It printed "packet 11
is on no list" and turned a one-run-in-five hang with no output into a
findable bug in the grey lists. A diagnostic that only helps in theory is
worth less than one that has already paid.

And one suppression, which is a demonstration of a different kind.
`tests/tsan.supp` silences a pattern that is inherent to a recycling
collector: memory is handed from one thread to another, and the previous
owner performed no synchronisation to give it up — nothing gave it up; the
collector decided the object was unreachable. So there is no
happens-before edge from its last write to the next owner's first write,
and ThreadSanitizer reports the pair. The memory is unreachable, nobody
reads the old value, and every tracing collector has this; bdwgc ships a
suppression file of its own.

A suppression is a claim that the tool is wrong, and the only way to make
that claim honestly is to check that the tool still works elsewhere. The
file is scoped to the test functions that churn allocations, and that
scoping was verified rather than asserted: a deliberately unsynchronised
write injected into `vm/` is still reported, with the file in effect, as a
race in the injected function. The verification was removed afterwards.

New oracles and new checks should come with the same demonstration.

## Test tiers

The layout as built, which is not quite the one planned above:

```
tests/unit/    everything except the smoke test
               pure functions, data structures, and the collector itself
tests/e2e/     one smoke test, run through the public headers only
```

Two differences from the plan worth recording, because a plan that no
longer matches the tree is worse than no plan:

- **There is no `tests/gc_oracle/`.** The oracles did not turn out to need
  a directory of their own: a canary check is a helper and a couple of
  assertions, and it lives wherever it is used — `test_collect` and
  `test_smoke` mostly. Pulling them out would have bought indirection and
  nothing else, so they stayed inline. If the stress suite above gets
  built, it will want the helpers factored out and that is the moment to
  create the directory.

- **`tests/e2e/` is not a compiled toy frontend.** It is a hand-written
  program in the *shape* generated code has — frames pushed and popped,
  slots kept up to date around every allocation. That tests the ABI and
  the protocol. It does not test a frontend, because there is no frontend
  yet; the first real one will want its own tier, and it will find the
  smoke test's role taken by something better.

Three tiers, then, rather than four. What matters is unchanged: tests are
**hosted** — they link libc and may use pthreads and stdio freely — while
the code under test is **freestanding**. Keeping that split sharp means the
harness can never accidentally become a dependency of the runtime, and the
freestanding audit stays meaningful.

## Stress patterns

**Three of the eight are built**, in `tests/stress/`, and they found four
real defects in the collector within a day of existing — which is the
argument for the other five.

Built:

- **Allocation churn, at the size boundaries** (`test_stress_boundary.c`,
  from bdwgc's `middle.c`). Both of this collector's boundaries have a size
  exactly on them and a size one allocation slot either side, and the sizes
  come from the arithmetic `cvm_alloc` does rather than from round numbers.
  Includes the random-sizes/random-lifetimes pattern with a fixed seed.
- **Object graphs** (`test_stress_graph.c`, from Go's `TestGcDeepNesting`
  and its rescan/array tests). Eight levels of embedded aggregate with a
  pointer at the bottom, which is where this collector differs most from
  the reference it ports; plus a wide tree traversed after ten collections
  with its node count verified, subtree handover, in-place mutation, and
  cycles reachable and detached.
- **Threads** (`test_stress_threads.c`, from bdwgc's `threadleak.c`). Short
  and long-lived threads registering and leaving while collections run, and
  workers that keep objects rooted across collections taken under load.

Not built: read-heavy as a distinct pattern (the graph test covers
traversal), fragmentation as a distinct pattern (the boundary test covers
random sizes and lifetimes), large objects as a distinct pattern (covered
by the boundary sweep at both ends of the threshold), and the FFI-boundary
pattern, which has no target until evacuation exists.

Each is meant to be parameterised by thread count, object sizes and
survival rate so that a single pattern can be swept across configurations.
What is built so far hard-codes those; parameterising them is the next step
and not a prerequisite for anything.

### What the stress suite found

Four defects, none of which the unit tests could have produced:

- **A deadlock between two collectors.** A thread waiting for another
  collection to finish was not "parked" as far as the incumbent collector
  could tell, so the incumbent waited for it while it waited for the
  incumbent. Both stopped forever, and the only sign was the collector's
  periodic "thread(s) not parked" warning.
- **Two threads coalescing at once.** Any thread that cannot allocate
  sweeps a batch and then coalesces, so several arrive at the coalescing
  pass together; the pass files results before advancing its start point,
  so they walked the same range and added the same runs twice. The block
  allocator's double-free assertion is what caught it.
- **A straggler sweeping into a finished sweep.** A thread still inside a
  batch when the sweep was declared over filed its blocks afterwards, and
  the next sweep then re-swept blocks that were in use.
- **The collector clearing the wrong thread's allocation cursor.**
  `cvm_allocator_clear` looked up the *calling* thread's context, but it is
  called for every parked thread in turn, so every other thread kept
  allocating from a block the sweep was about to hand to someone else. The
  debug assertion that now checks the post-condition — no thread's bump
  region points anywhere — is what would have caught it in one run.

The last one is worth dwelling on: it was found by asking why ThreadSanitizer
reported two threads writing into the same object memory, and the answer
came from a purpose-built probe (`tests/stress/` was not enough on its own)
that looked for two threads receiving the same address. The first version of
that probe was wrong — it counted sequential reuse as a collision — and
learning that was itself useful: a correct version, using allocations large
enough to take the validated slow path, found nothing.

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
concurrent tests mean nothing without it.

What the matrix actually runs, since the shape matters:

    tests: debug      plain build, trap safepoints
tests: release    plain build, trap safepoints
tests: asan       debug, polling safepoints
tests: tsan       debug, polling safepoints

So asan and tsan each run once, and it is the plain configurations that
run twice. A bug that only appears with optimisations on is a real
category, which is why release is there; running a sanitizer in release as
well would be better still and is not done, because a sanitizer's own
overhead makes optimised code harder to reason about, not easier.

Every binary runs under a deadline (`CVM_TEST_TIMEOUT_MS`, default a
minute) and a hang is reported as a failure with the binary's name. That
was added the day a logging macro turned out not to terminate: a suite
that can hang forever is worse than one that fails, because the failure
is the only one that tells you anything.

The test runner reads `build/config.json` to learn which sanitizer the
build used, because one check is only relevant under ThreadSanitizer and a
binary's name does not say. That check is the suppressions file below.

`valgrind` is available on the development machine and can serve as a
second opinion on metadata allocations and on the test harness itself,
but it does not understand the GC heap — the same blind spot as ASan,
without the manual poisoning that makes ASan useful here.

## Status

There is a collector, and it collects. The oracles described above are no
longer a plan: the collection tests are written against them, which is how
the design's central claim is checked rather than asserted.

Which oracle caught which bug is not worth tallying, partly because most
of the bugs found were not liveness bugs at all — they were in the
allocator, the sweeper's bookkeeping, the growth heuristic and the grey
lists. What the oracles did do is make it safe to change the collector:
after any change to marking or sweeping, a canary check and a metadata
check say whether liveness still works, without anyone having to reason
about it again.

What exists, by tier:

- `tests/unit/`: 23 binaries, nearly all of the suite's tests, green under
  debug, release, asan and tsan
- `tests/e2e/`: one smoke test that runs a whole program through the
  public headers only, no internal header at all
- the freestanding audit, in every configuration
- the CI matrix driver
- `cvm_assert`, which caught a real overflow bug on its first day

The count depends on the configuration, which is worth knowing before
reading a number off a run: 24 binaries and 280 tests in the default
build, three fewer under a sanitizer, because the trap-mode-only tests are
compiled out when the sanitizer's own `SIGSEGV` handling rules traps out.
So the suite does not have "a" test count; it has one per configuration,
and both are exercised.

How the oracles are used today:

- **TP** — address-derived canary payloads. Written into an object before a
  collection, verified after, in `test_collect` and `test_smoke`. This is
  what catches a wrongly-traced descriptor.
- **TN** — the collector's own metadata slot. A test asks whether the slot
  says "allocated", which is a question the collector answers about itself.
- **FP** — not directly tested. Nothing asserts "this object was *not*
  collected when it should have been", because a precise collector has no
  mechanism that produces floating garbage beyond a bug in tracing, and a
  tracing bug shows up as FN instead.
- **FN** — caught by canary verification, and by asan through the whole
  suite. This is the one the design exists to make impossible, and the
  canary is the cheapest instrument for it.

A note on the weak-reference probe, which the plan above leans on more than
the tests do: a weak reference on an *unreachable* object is never reached
by the marker, so it is never queued for nullifying, and its slot keeps
whatever was there until the memory is reused. Reading it would be reading
freed memory and calling the stale value a liveness decision. The weak
probe is sound only when the weak reference itself is reachable, which is
how `test_weak` and the reachable cases in `test_collect` use it.

What does not exist yet:

- **the stress patterns**. The table above is still a plan. Each needs a
  harness that allocates and mutates under a policy, and the collector now
  has enough in place to run them — this is the next thing to build, not a
  prerequisite for anything else.
- **evacuation**, which is why the fragmentation pattern has no target yet.
  Sweeping reclaims lines and blocks and leaves the fragmentation that
  opportunistic evacuation would address.
- **Windows**, so the FFI-boundary pattern's pinning half is untested. The
  pinning flag exists and evacuation would honour it; there is no
  evacuation to honour it.

The order the rest gets built in is the order in the porting plan, and each
one brings its tests with it. That is how the collector got here: 25 tasks,
each with its tests, each committed only once they passed.
