/*
 * vm/gc/heap.h
 *
 * Heap layout, ported from refs/commix/Heap.{h,c}.
 *
 * ---------------------------------------------------------------------
 * Layout
 * ---------------------------------------------------------------------
 *
 * The heap is a set of separate reservations, not one:
 *
 *   block metadata    one BlockMeta per block, for the maximum block count
 *   line metadata     one LineMeta per line, likewise
 *   bytemap           one ObjectMeta per allocation slot, likewise
 *   the heap itself   block-aligned, so that an address maps to a block by
 *                     masking rather than by division
 *
 * The metadata regions are sized for the maximum and grow by moving their
 * end pointers, so growing the heap never has to relocate metadata. That
 * is worth the address space: the block metadata tables are addressed by
 * arithmetic on the metadata base, and a relocation would invalidate
 * every index.
 *
 * The heap reservation is block-aligned. This is not cosmetic: the
 * address-to-block conversion in gc/meta.h is a mask, so a base that is
 * not aligned would map addresses outside the heap onto a block below it.
 * Aligning up inside the reservation is why the reservation asks for one
 * extra block.
 *
 * ---------------------------------------------------------------------
 * Growth
 * ---------------------------------------------------------------------
 *
 * cvm_heap_extend moves the end pointers and clears the new metadata. It
 * does not decide policy: whether to grow, and by how much, belongs to
 * whoever needs the space. The reference keeps that policy in
 * Heap_GrowIfNeeded, driven by the ratio of marking time to mutator time
 * and by the fraction of free blocks; the same shape is reproduced here
 * in cvm_heap_should_grow.
 */

#ifndef CVM_GC_HEAP_H
#define CVM_GC_HEAP_H

#include <stdatomic.h>

#include "base/types.h"
#include "gc/bytemap.h"
#include "gc/block_alloc.h"
#include "gc/block_range.h"
#include "gc/grey_packet.h"
#include "gc/layout.h"
#include "gc/meta.h"
#include "gc/worklist.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* a reservation                                                       */
/* ------------------------------------------------------------------ */

/*
 * An aligned window inside a reservation.
 *
 * `base` and `size` are what rt_mem_reserve returned and must be handed
 * back verbatim on release; `start` is the aligned address the heap
 * actually uses, and `usable` is how much fits from there.
 */
typedef struct CvmRegion {
    u8* base;
    usize size;
    u8* start;
    usize usable;
} CvmRegion;

/* Reserve `bytes` and align the usable start up to `align`. */
bool cvm_region_reserve(CvmRegion* region, usize bytes, usize align);

void cvm_region_release(CvmRegion* region);

/* ------------------------------------------------------------------ */
/* the heap                                                            */
/* ------------------------------------------------------------------ */

/* The worker pool, defined in gc/pool.h. Held by pointer so that heap.h
 * does not depend on the pool's internals; NULL when there are no
 * concurrent workers, which is the single-threaded case. */
struct CvmGcPool;

/*
 * A chunk is the unit the large-object allocator hands out: whole multiples
 * of the large-object granularity, smaller than a block. While it is free it
 * carries its own header, so the free lists cost no memory of their own.
 *
 * Declared here because the heap owns the lists -- see large_free_lists
 * below -- and because both gc/large.h and this header need the type without
 * one including the other.
 */
typedef struct CvmChunk {
    usize size;            /* total bytes in this chunk */
    struct CvmChunk* next; /* next free chunk of the same size class */
} CvmChunk;

/* One size class of free chunks. A compare-exchange stack, because the
 * threads that push are not the threads that pop. */
typedef struct CvmFreeList {
    _Atomic(usize) head; /* CvmChunk*, as an integer so zero means empty */
} CvmFreeList;

typedef struct CvmHeap {
    /* --- the block-aligned heap window --- */
    CvmRegion blocks;
    u8* heap_start;
    /*
     * One past the last byte in use.
     *
     * Atomic because the two accessors disagree about locking. The grow path
     * writes it under grow_lock, and cvm_heap_contains reads it on the
     * allocation path with no lock — it is a debug assertion, and taking a
     * lock there would put the collector's bookkeeping on the fast path it is
     * supposed to stay off. The field only ever grows, so a reader that sees
     * a slightly old value just answers "not in the heap" for a block that is
     * not yet published, which is the safe direction. Making it atomic is
     * what tells the race checker the arrangement is intentional rather than
     * an accident.
     */
    _Atomic(u8*) heap_end;
    usize heap_size;

    /* --- metadata, sized for the maximum --- */
    CvmRegion block_meta_region;
    CvmRegion line_meta_region;
    CvmRegion bytemap_region;

    BlockMeta* block_meta_start;
    BlockMeta* block_meta_end;
    LineMeta* line_meta_start;
    LineMeta* line_meta_end;

    Bytemap* bytemap;

    /* --- sizes --- */
    u32 block_count;     /* in use */
    u32 max_block_count; /* reserved */
    usize max_heap_size;

    /* --- growth policy --- */
    f64 max_mark_time_ratio;
    f64 min_free_ratio;

    /* --- growth serialisation --- */
    RtMutex grow_lock;

    /* --- marking --- */
    CvmRegion grey_packets_region;

    struct {
        /*
         * Packets with room, packets to trace, and weak references found
         * along the way. The weak list is separate because a weak
         * reference must not keep its referent alive: it is recorded
         * rather than traced, and the nullify phase decides what to do
         * with it once marking knows what survived.
         */
        CvmGreyList empty;
        CvmGreyList full;
        CvmGreyList found_weak_refs;

        /* How many packets exist in total. Marking is finished when the
         * number on the empty list plus the number on the weak list
         * accounts for all of them. */
        u32 packet_count;
        CvmGreyPacket* packets;

        /*
         * The deque for the thread that is collecting without a pool -- the
         * single-threaded case, where marking runs on the caller. A worker has
         * its own in CvmGcThread; the collecting thread has no CvmGcThread,
         * and it still fills and drains packets like a marker, so it needs
         * somewhere to stack them.
         */
        CvmWorklist work;

        /*
         * Whether this heap collects with concurrent marking, and whether a
         * collection is in progress right now. See CvmOptions.concurrent_mark
         * for the first; the second exists because a concurrent collection
         * releases the stop-the-world window before it finishes, so the flag
         * that guards the window no longer says "a collection is running" --
         * a thread that arrives mid-marking has to find that out some other
         * way, or it would start a second collection into the first.
         */
        _Atomic bool busy;

        u64 current_start_ns;
        u64 current_end_ns;
        u64 last_end_ns;

        /*
         * The previous collection's figures, kept because the growth
         * decision is made at the start of the next one.
         *
         * That timing is not arbitrary. The free-block count is only
         * meaningful once a sweep has finished and mutators have been
         * allocating against it; asking earlier, while the block allocator
         * has just been emptied for the coming sweep, always answers "no
         * free blocks" and grows the heap on every collection. (That is
         * exactly what happened: a churn test that should have grown
         * nothing doubled the heap three times in a row.)
         */
        u64 last_mark_ns;
        u64 last_period_ns;
        bool has_history;
    } mark;

    /*
     * Objects kept alive for one more collection after their replacement.
     * See cvm/retire.h for the contract and the argument.
     *
     * Two lists rather than a per-entry generation: `current` collects what is
     * retired while a collection runs or between collections, and moves to
     * `previous` at the next collection's reset -- by which time one full
     * marking has kept every entry alive, and the marking after that is what
     * reclaims them. The lock is for the concurrent window, where an
     * unmanaged thread can retire (a resize under a container's lock) while
     * the collector marks; it is a leaf, taken by nothing else.
     */
    struct {
        RtMutex lock;
        opaque_mut* previous;
        u32 previous_count;
        u32 previous_cap;
        opaque_mut* current;
        u32 current_count;
        u32 current_cap;
    } retired;

    /* --- sweeping --- */
    struct {
        /*
         * Next block to sweep. Atomic because sweepers claim batches by
         * advancing it, so several can work at once without a lock.
         */
        _Atomic u32 cursor;
        u32 limit;

        /*
         * Coalescing has been done up to here. Trails the sweep cursor:
         * a run of free blocks can only be merged once every sweeper that
         * touches it has finished, which is what the minimum cursor of
         * all sweepers tracks.
         */
        _Atomic u32 coalesce_done;

        /* Set when the sweep, including coalescing, is finished. */
        _Atomic bool post_sweep_done;

        /*
         * Claimed by whichever thread does the finishing work — putting
         * the sweeper's reserve back and leaving concurrent mode. Without
         * it two threads reaching the end at once would each take a
         * reserve, and the second would take blocks the first had just
         * set aside.
         */
        _Atomic bool finishing;

        /*
         * Held for the duration of a coalescing pass.
         *
         * Any thread that cannot allocate sweeps a batch and then
         * coalesces, so several can arrive at the coalescing pass at once.
         * The pass reads a start point, walks forward adding runs to the
         * block allocator, and only then advances the start point — so two
         * threads entering together walk the same range and add the same
         * runs, and the second add finds blocks that are already free. That
         * is the assertion in the allocator's clear_run, and it fired
         * before this lock existed.
         *
         * A pass is bounded work that is already rate-limited by the sweep
         * cursor, so serialising it costs nothing worth measuring.
         */
        RtMutex coalesce_lock;

        /*
         * Who holds the coalescing lock, or zero. A diagnostic, not a
         * mechanism: a collector waiting for a thread that is itself
         * waiting for a lock cannot be understood from the outside, and
         * "which lock, held by whom" is the first question worth answering.
         */
        _Atomic(usize) coalesce_owner;
    } sweep;

    /*
     * Concurrent GC workers, or NULL. Their per-thread sweep progress is
     * what bounds coalescing, so the sweeper needs to see them.
     */
    struct CvmGcPool* pool;

    /*
     * The block allocator over this heap. Held here rather than passed
     * around because the sweeper, the collector and the allocator all need
     * it and it is a property of the heap: there is exactly one per heap,
     * and it is created with it.
     *
     * Named block_alloc rather than blocks, because `blocks` is the
     * reservation holding the heap's own payload.
     */
    CvmBlockAllocator* block_alloc;

    /*
     * Free chunks, by size class.
     *
     * The heap's, not a mutator's, and that is the point: a chunk is returned
     * by whichever thread sweeps the block it lived in, and a GC worker
     * sweeping has no mutator to return it to. The lists are compare-exchange
     * stacks, so any thread can push or pop one, which is what the comment on
     * cvm_large_add_chunk has claimed since it was written.
     *
     * They are dropped at the start of every collection and rebuilt by the
     * sweep, so nothing here outlives a cycle and there is no ownership to
     * track.
     */
    CvmFreeList large_free_lists[CVM_LARGE_FREE_LIST_COUNT];

} CvmHeap;

/*
 * Set up the heap.
 *
 * `min_size` and `max_size` are in bytes of block payload. `max_size` is
 * what gets reserved; `min_size` is what starts out usable. Sizes below
 * CVM_MIN_HEAP_SIZE are raised to it, and a max of 0 means "as much as
 * the machine will give".
 *
 * Returns false if a reservation failed, in which case nothing is
 * allocated and the heap must not be used.
 */
bool cvm_heap_init(CvmHeap* heap, usize min_size, usize max_size);

/* The retire list's collector-side halves. See gc/retire.c and cvm/retire.h. */
void cvm_retire_begin_collection(CvmHeap* heap);
void cvm_retire_mark_all(CvmHeap* heap, CvmGreyPacket** out, CvmGreyPacket** out_weak);

void cvm_heap_destroy(CvmHeap* heap);

/*
 * Extend the heap by `blocks`, clearing the new metadata.
 *
 * Reports the index of the first new block through `out_first_block`, so
 * that the caller can hand the range to the block allocator. The heap
 * itself does not know about free lists; that is gc/block_alloc.h.
 *
 * Returns false if the request would exceed the reservation.
 */
bool cvm_heap_extend(CvmHeap* heap, u32 blocks, u32* out_first_block);

/*
 * Grow if the previous collection's figures say to.
 *
 * Called at the start of a collection, after any unfinished sweep has been
 * driven to completion, because that is when the free-block count reflects
 * the heap as the program is actually using it. See the note on
 * `has_history` in CvmHeap for why the timing matters.
 */
void cvm_heap_grow_if_needed(CvmHeap* heap);

/*
 * Whether to grow, given how long the last collection spent marking, how
 * long it has been since the one before, and how many blocks are free.
 *
 * `period_ns` is the time *between* collections, not the duration of this
 * one. That distinction is the whole point of the mark-time condition: it
 * is asking what fraction of the program's running time marking is eating,
 * and dividing by the collection's own duration instead answers a
 * different and useless question — the ratio comes out near one, so a heap
 * that is barely being used grows anyway on timing noise. (Getting this
 * wrong is how a churn test that should not have grown the heap at all
 * did exactly that.)
 *
 * The two conditions are independent and either suffices: marking eating
 * too much of the program's time means the heap is too small for the
 * allocation rate, and few free blocks means the next allocation is at
 * risk. Growing only on the second would let a program sit at 100% mark
 * time indefinitely.
 */
bool cvm_heap_should_grow(CvmHeap* heap, u64 mark_time_ns, u64 period_ns,
                          u32 free_blocks);

/*
 * How many blocks to add next. Doubling while the heap is small avoids a
 * long ramp of small grows; the smaller golden-ratio factor later keeps
 * the over-reservation bounded.
 */
u32 cvm_heap_growth_increment(CvmHeap const* heap);

/* Bytes of address space one block costs, including its metadata. */
CVM_INLINE usize cvm_heap_space_per_block(void) {
    return CVM_SPACE_USED_PER_BLOCK;
}

/* Address of block `index`, or NULL if it is beyond the current heap. */
CVM_INLINE u8* cvm_heap_block_address(CvmHeap const* heap, u32 index) {
    if (index >= heap->block_count) {
        return NULL;
    }
    return heap->heap_start + (usize)index * CVM_BLOCK_SIZE;
}

/* Metadata entry for block `index`. */
CVM_INLINE BlockMeta* cvm_heap_block_meta(CvmHeap const* heap, u32 index) {
    return heap->block_meta_start + index;
}

CVM_INLINE LineMeta* cvm_heap_line_meta(CvmHeap const* heap, u32 block) {
    return heap->line_meta_start + (usize)block * CVM_LINE_COUNT;
}

/* Is this address inside the heap's usable window? */
CVM_INLINE bool cvm_heap_contains(CvmHeap const* heap, u8 const* p) {
    /*
     * Relaxed, and it matters. This runs once per pointer field of every
     * object the marker visits — the hottest read in the collector — and an
     * acquire load on AArch64 is `ldar`, which is several times the cost of a
     * plain one. Nothing here needs the ordering: the bound only grows, so a
     * stale value is conservative, and the marking that calls this runs with
     * the world stopped, when no growth can be in flight.
     */
    u8* end = atomic_load_explicit(&heap->heap_end, memory_order_relaxed);
    return p >= heap->heap_start && p < end;
}

#endif /* CVM_GC_HEAP_H */
