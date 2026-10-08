# bench/bdwgc/

PLEASE download [gc-8.2.12](https://github.com/bdwgc/bdwgc/releases/download/v8.2.12/gc-8.2.12.tar.gz), and unzip it here

`backend_bdwgc.c` is ours; the release tree is not (it is gitignored, and
`scripts/lib/bench/thirdparty.mjs` configures and builds it into
`bench/build/bdwgc/`).

## What it is

| | |
|---|---|
| collector | Boehm-Demers-Weiser 8.2.12 |
| marking | stop-the-world, **conservative** — it cannot know an integer is not a pointer |
| roots | conservative — it scans the stack word by word |
| moves objects | no |
| built | `--enable-threads=posix --disable-shared --enable-static`, `-O1` |

## What it has to do that the others do not

**Two allocation entry points.** `GC_MALLOC` for objects with pointers,
`GC_MALLOC_ATOMIC` for objects without. Using the atomic call for a leaf is
not a shortcut around the interface; it is the one place bdwgc's API
distinguishes what a precise collector distinguishes everywhere, and taking
advantage of it is what the collector offers.

**Disable collection for alloc-only mode.** A large maximum heap does not
stop bdwgc collecting: it collects proactively whenever it estimates reuse
will help, so a churn run reported 977 collections in "alloc-only" mode while
cvm reported none. `GC_disable` is the literal meaning of the mode.

**Measure its own pause.** `GC_set_on_collection_event` delivers
`PRE_STOP_WORLD` and `POST_START_WORLD`, which is the world-stopped window
and therefore the same interval cvm's pause measures. Using `GC_EVENT_START`
and `GC_EVENT_END` instead would include the concurrent parts of an
incremental collection and make the two numbers incomparable.

## What it reports

| column | source | note |
|---|---|---|
| `allocations` | this backend's own count | bdwgc has no per-allocation counter |
| `collections` | `GC_get_gc_no()` | |
| `pause_max_ns` | the stop-world event pair | |
| `live_objects` | — | **left at zero.** bdwgc tracks bytes, not objects, and deriving a count from an assumed object size would be a made-up number in a table of measured ones. Its heap size is reported instead |
| `heap_bytes` | `GC_get_heap_usage_safe()` | |

## Modes

All three. alloc-only disables the collector; fixed and auto use bdwgc's own
policy with the scenario's heap hint as a maximum.

## Known gap

`mixed-sizes × alloc` fails intermittently **under the runner** and passes
standalone. It is the one row this backend does not always produce, and it is
listed rather than hidden.
