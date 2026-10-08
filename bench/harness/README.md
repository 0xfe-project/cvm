# bench/harness/

The part that is the same for every collector: the interface, the roots
protocol, the timing loop, and the CSV.

## Files

| file | what |
|---|---|
| `collector.h` | the interface every backend implements: five functions and a stats struct |
| `roots.h` | how a scenario tells the collector what is alive — a frame for cvm, an escaping stack array for the conservative ones |
| `run.c` | `main`: the three modes, K repetitions, timing, the printed row |
| `report.c` `report.h` | order statistics and the CSV row |

## The interface

| function | contract |
|---|---|
| `init(heap_hint, alloc_only)` | one-time setup. The hint is passed in every mode — a backend may use it to bound growth as well as to avoid it |
| `alloc(size)` | zeroed bytes, or NULL |
| `alloc_typed(shape)` | an object of a declared shape. A conservative backend implements this as `alloc(shape->size)`, and that is a real choice rather than a default nobody looked at |
| `collect()` | force a collection, or return false if this backend has no way to be asked |
| `stats(&out)` | allocations, collections, pause, live objects, heap bytes |

A backend also declares which modes it has (`BENCH_MODE_SUPPORTS_*`), and a
scenario declares which modes it wants. Both have to agree before a row is
produced — a backend that cannot be asked to collect is reported as unable to
run that mode rather than approximated.

## The three modes

| mode | how it is driven | what it isolates |
|---|---|---|
| `alloc` | a heap large enough that no collection happens; the collector is re-initialised between repetitions | the allocator: bump pointer versus free list versus hash table |
| `fixed` | every backend forced to collect at the same allocation count | the collector's mechanism, with the triggering policy removed |
| `auto` | each backend's own policy | the policy |

## What it enforces

**The allocation counts must be nonzero.** A run that did not do the work is
not a measurement, whatever its wall clock says, and each backend reports its
own count for that reason.

**One scenario source, linked four ways.** The scenario cannot drift between
collectors; if two binaries for one scenario differ, it is only in which
collector they link.

**The mode check happens before anything is measured.** Asking a collector
for a mode it does not have is not a failed run and not a run at all.

**Nothing on the allocation path that does not have to be.** The
fixed-cadence check was an integer division per allocation in every mode
before it was a countdown in one mode; the per-repetition count comes from
each backend's own stats rather than from a counter the harness keeps.
