# bench/scenarios/

Seven workloads and the shapes they allocate, shared by all four collectors.
One `.c` file per scenario; each defines exactly one `bench_scenario`.

## The seven

| scenario | axes | modes | what it is for |
|---|---|---|---|
| `churn_unary.c` | small, churn, uniform | alloc, fixed | allocate and drop; nothing survives, so nothing is marked. The allocator's ceiling, and cvm's core claim |
| `tree_keep.c` | small, tree, uniform | all | build a tree, drop the previous one. The only shape that exercises marking and sweeping together with a live set worth the name |
| `mixed_sizes.c` | all, churn, mixed | all | sizes drawn across every allocation path, lifetimes random. The fragmentation scenario |
| `flat_fp.c` | flat | alloc, auto | n-body over flat arrays, almost no pointers. **The control** — a backend slow here is slow in its allocator or its safepoint mechanism, not its marking |
| `read_heavy.c` | small, tree, read | auto | build a graph once, traverse it many times with about one write per hundred reads |
| `write_heavy.c` | wide, tree, write | auto | mutate a dense pointer graph. Where cvm's free stores meet Go's write barrier |
| `growth.c` | small+blob, growing, mixed | auto | live set ×1.49 per round. Heap policy rather than mechanism |

## What a scenario declares

| field | meaning |
|---|---|
| `name` | what the row is called. Not the file name — the C binaries are named after their files, and the runner asks each one for its name |
| `modes` | which of the three modes make sense for it |
| `heap_hint_bytes` | what alloc-only mode needs so that no collection happens: the total allocation |
| `auto_heap_bytes` | the ceiling for auto and fixed, sized to a few times the live set |
| `default_cadence` | allocations between forced collections in fixed mode |

**Two heap numbers, not one.** alloc-only is sized to the total allocation;
auto and fixed are sized to the working set. The first version passed one
number to both, and cvm's growth policy — uncapped, and happy to grow until
collections are rare — made the collector half of the comparison invisible.

## The shapes (`shapes.c`)

| shape | bytes | pointers | role |
|---|---|---|---|
| `small_node` | 24 | 2 | the common case |
| `fat_node` | 136 | 2 | **controlled pair with `small_node`** — identical pointer count, five times the bytes |
| `wide_node` | 96 | 8 | the write-heavy shape |
| `leaf_blob` | 128 | 0 | the no-pointer control |

Declared once so that `small_node` in one scenario is the same object as
`small_node` in another, and so the pointer offsets are data rather than a
comment: a precise collector reads them, a conservative one scans the whole
object and finds the same pointers.

## Two things a reader should know

**The predictions are in `BENCH.md` §6**, recorded so that being wrong is
informative.

**`tree_keep`'s build order is post-order** — it recurses before allocating
the node — while marking is pre-order, so pointer chasing has poor locality
by construction. That is the workload's shape, not something tuned for or
against any collector, but it is why marking there is slower per byte than a
workload whose allocation order matches its traversal order.
