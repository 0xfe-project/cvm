# vm/ — the collector

A precise mark-sweep collector with line-and-block granularity: Immix's
reclamation strategy, Commix's phase structure, and roots that are exact
rather than guessed at.

Everything under this directory is freestanding. `vm/` compiles with
`-ffreestanding -nostdinc`; what it cannot do for itself it asks `rt/` for.
The public ABI that generated code includes is in `include/cvm/`; the
implementation is in `gc/`.

## The algorithm in one page

**Blocks and lines.** The heap is a reservation divided into 32 KiB blocks,
each cut into 128 lines of 256 bytes. A line is the unit of reclamation and a
block is the unit of allocation: a sweep decides which lines survive, and a
block with surviving lines goes back to the mutator pool with a free-line
list describing its holes.

**Objects.** An object is a header — a type descriptor pointer and a flags
word — followed by the payload that generated code sees. A line-allocated
object never spans blocks: the inline path's limit is one line, and the
slow path keeps a medium object inside one block. An object at or above the
large-object threshold does span them when it has to, as a run of whole
blocks that the block allocator hands out as a superblock — see
`gc/large.h`. Allocation is a bump pointer into the current block, which is
three instructions; a block with holes hands out its free lines before it
hands out fresh space.

**Metadata is in side tables, not in objects.** One byte per 16 payload bytes
of bytemap says what each slot is: free, a placeholder for the interior of a
large object, an allocated object's first slot, or a marked one. A line has a
byte saying whether anything in it survived, and a block has one saying
whether it needs sweeping at all. Keeping collector state out of the object
header is what lets a two-word object stay two words.

**Marking is exact.** A thread's roots are the slots of the frames it pushed —
`cvm_frame_push` links them into a chain the collector walks. An object's
pointers are read by following its descriptor, which may be a struct with
pointer fields, an embedded aggregate, or an array. Nothing anywhere scans
raw stack memory, so an integer that happens to look like a pointer cannot
keep an object alive.

## How a collection runs

    stop the world (the first stop)
      drive the previous sweep to its end
      decide whether to grow the heap
      scan the frame chains: the root snapshot
    resume the world
      mark alongside the program, with the worker pool and the write barrier
      (a store during marking marks what it publishes; an allocation
      starts life marked)
    stop the world (the second stop)
      rescan the frame chains: the roots changed while the program ran
      drain whatever the rescan found
      nullify: clear every weak reference whose target did not survive
      prepare the sweep: empty every thread's allocator and the block free lists
      reset the sweep cursors
    resume the world
      hand the sweep to the worker pool, and do not wait for it

Two stops, not one, and neither is the trace. The first is the root snapshot —
a frame chain walk, which is per-thread and small — and the second is the
rescan plus the phases that always needed the world stopped (nullify, sweep
preparation). The marking itself, which is what scaled with the live set, runs
alongside the program. The write barrier is what makes that sound: a store
during marking marks the value it publishes, so the marker cannot miss
something the program made reachable after the marker had already passed.

The sweep is the same as it has always been: a sweep only decides which lines
a dead object occupied — no pointer is followed — so it happens while the
program runs, driven by whoever allocates next and by the pool.

Sweeping is itself batched: a sweeper claims a run of blocks, and a run of
free lines that crosses a batch boundary is held back with a `coalesce` mark
for a later pass to close. That is what lets several threads sweep at once
without two of them describing the same run.

## What this design is good at, and what it pays

**Good at allocation.** A bump pointer, with no free-list walk and no
per-object collector state to maintain. On the benchmark's allocation-only
scenarios it is the fastest of the four collectors measured, and that is the
design claim rather than a tuning result.

**Good at marking, per object.** Exact roots mean the collector reads the
slots a frame declares and nothing else; a conservative collector reads the
whole stack, word by word, on every collection.

**Good at not taxing the program.** There is no read barrier, and the write
barrier's fast path is one load of a shared flag, a branch the predictor gets
right, and the store — it acts only while a mark phase is running, and
marking runs alongside the program, so the mutator's own time is its own
except for that check. Measured on one tree with the barrier on and off,
every benchmark row was within noise, including `write-heavy`, which is
nothing but pointer stores into live objects.

The trade against a stop-the-world collector is not "a barrier or no
barrier": it is that this one pays the barrier on every store, and in return
the pause is the root snapshot and the tail rather than the whole live set.

**The pause does not scale with the live set.** Marking runs alongside the
program; the stop-the-world window is the root snapshot at the start and the
rescan-and-drain at the end. On the benchmark, read-heavy's pause is 0.028ms
against a stop-the-world collector's 2.4ms on the same live set, and the
program's throughput is unchanged: the marking work still happens, it just
overlaps the program instead of stopping it.

**It pays for its side tables in cache footprint.** Per object the marker
touches the object, its bytemap slot, its line metadata and its block
metadata, where a collector keeping mark bits in the object header touches
one cache line. The benchmark measures this: marking is latency-bound on this
collector, not instruction-bound, and the instruction-level changes that were
tried against it all measured as noise.

**It does not move objects.** Nothing is evacuated, so the fragmentation that
sweeping cannot reach stays where it is. Pinning is therefore unobservable
and the flag for it does not exist.

## Using it

```c
#include "cvm/vm.h"
#include "cvm/gc.h"

static CvmPtrField const node_fields[] = {{.offset = 0}, {.offset = 8}};
static CvmTypeDescriptor const node_type = {
    .size = 24, .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 2, .ptr_fields = node_fields,
};

void walk(void) {
    CVM_FRAME_DECL(f, 2);           /* two root slots, on this stack frame */
    cvm_frame_push(&f_frame, f_roots, 2);
    f_roots[0] = cvm_alloc(&node_type);
    f_roots[1] = cvm_alloc(&node_type);

    /* A pointer into an object's field. */
    cvm_store_field(f_roots[0], node_fields[0].offset, f_roots[1]);

    cvm_frame_pop(&f_frame);
}

int main(void) {
    if (!cvm_init(NULL)) return 1;   /* NULL means the defaults */
    walk();
    cvm_shutdown();
}
```

Four things to get right, and none of them is optional:

- **Every function that can allocate pushes a frame first**, and pops it
  before returning. The push is also where the safepoint poll lives, so a
  function that skips it is not merely missing roots — it has opted out of
  being stopped.
- **A pointer store goes through `cvm_store_field`**, not through a bare
  `obj->field = value`. Marking is a graph traversal and a traversal needs the
  graph to hold still: the marker marks an object, traces its fields and never
  looks at it again, so a store into an object it has already finished with
  would leave the object it now points at unmarked, collected, and the field
  pointing at freed memory — with nothing reporting it. The store is the only
  moment a program can make an unmarked object reachable from a marked one, so
  the store is where the barrier lives. See the contract on `cvm_store_field`
  in `cvm/gc.h` for what it costs and for the one case that is exempt (filling
  an object nothing can reach yet).
- **Descriptor offsets are relative to the payload**, not to the object's
  start. `cvm_header_of` converts between the two, and the header is one
  step back from what generated code holds.
- **A long array is one object.** `cvm_alloc_array` writes the length where
  the collector reads it, so the descriptor and the allocation agree about
  how far the object reaches.

`cvm_init`'s options cover the heap's minimum and maximum, how many GC
workers to run, whether finalizers get their own thread, where the event log goes,
and the log level.

A container that replaces a structure a lock-free reader may still be
walking retires it rather than dropping it: `cvm_retire` keeps the old object
alive through one more collection, which is long enough that every reader
who entered before the replacement has passed a safepoint. See
`cvm/retire.h` for the contract and the argument; the hash table and the
atom table are the users. `cvm_vm_stats` reports collections, time spent,
live objects and heap size; `cvm_collect` forces a collection, which is what
the benchmark's fixed-cadence mode calls.

## Reading the code

| file | what |
|---|---|
| `include/cvm/gc.h` | the ABI: header, frame, context, allocation fast path, descriptors |
| `gc/layout.h` | block and line geometry, growth policy, the constants above |
| `gc/heap.[ch]` | the reservation, the metadata regions, growth |
| `gc/alloc.c` | the slow path: bump, adopt a recycled block, sweep a little, collect, grow |
| `gc/collect.c` | the driver: the phases, in the order this README lists them |
| `gc/marker.c` | root scan, the trace, `trace_type` |
| `gc/sweeper.c` | batches, coalescing, the finish, the consistency check |
| `gc/block_alloc.[ch]` | free runs, the reserve, the recycled blocks |
| `gc/pool.[ch]` | the worker pool and its phases |
| `context.[ch]` | the thread registry, the frame chains, the TLS context |
| `safepoint.[ch]` | stopping the world. A build chooses one of two mechanisms at compile time: a protected page per thread whose fault parks it, or a poll gated by a counter. The trap is one load and no branch; the poll is chosen where a signal handler or a mappable page is not available |

`gc/collect.h` and `gc/sweeper.h` carry the contracts worth reading before
changing anything: what the phases guarantee each other, and why the sweep
batch boundary exists.
