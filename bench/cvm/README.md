# bench/cvm/

cvm as the benchmark measures it. `backend_cvm.c` is the whole folder.

## What it is

| | |
|---|---|
| collector | cvm itself, the project this repository is |
| marking | stop-the-world, parallel (worker pool) |
| roots | exact — a frame chain the caller pushes |
| moves objects | no |
| built | from `vm/` and `rt/` sources, `-O1`, `-DNDEBUG` |

## What it has to do that the others do not

**Push frames.** cvm is precise: roots come from a frame the caller pushes,
and nothing else is scanned. A scenario that allocated without pushing one
would skip both the per-entry safepoint poll and the root registration, and
would then be measuring a collector that never looks at the stack. That is
the requirement in `BENCH.md` §2.3, and `harness/roots.h` is where it is
expressed — the same scenario source pushes a real frame here and does
nothing for the conservative collectors.

**Build descriptors.** A precise collector needs to know where an object's
pointers are, so `alloc(size)` for an arbitrary size has to become a
descriptor before it can be allocated. The descriptors are built from the
scenario's declared shape and cached in static storage, because the collector
reads them during marking and a stack descriptor would be dangling by then.

## What it reports

| column | source | note |
|---|---|---|
| `allocations` | this backend's own count | cvm has no allocation counter of its own, by design — one would have to sit on the bump fast path. The corroboration that work happened is `live_objects` and the heap size |
| `collections` | `cvm_vm_stats()` | |
| `pause_max_ns` | the collector's own counter | mark + nullify + sweep preparation. Not the sweep: that continues after the world resumes |
| `live_objects` | `cvm_vm_stats()` | counted on demand by walking the bytemap, not tracked |
| `heap_bytes` | `cvm_vm_stats()` | the heap's size, not the live set |

## Modes

| mode | how |
|---|---|
| `alloc` | `min_heap_size` raised to the scenario's hint, so no collection can happen; the collector is re-initialised between repetitions, because no collection means no memory comes back and a workload allocating more than its heap could not otherwise run twice |
| `fixed` | the harness forces a collection every N allocations; the heap is capped at the scenario's `auto_heap_bytes` |
| `auto` | cvm's own growth policy, capped at the same |

The cap exists because cvm's policy is otherwise uncapped and, on an
allocation-heavy workload, grows until collections are rare — which makes the
collector half of the comparison invisible. See `scenarios/scenario.h`.
