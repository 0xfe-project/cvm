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

typedef struct CvmHeap {
    /* --- the block-aligned heap window --- */
    CvmRegion blocks;
    u8* heap_start;
    u8* heap_end; /* one past the last byte in use */
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

/* Marking state, for the marker and the collector driver. */
CVM_INLINE CvmGreyList* cvm_heap_grey_empty(CvmHeap* heap) {
    return &heap->mark.empty;
}
CVM_INLINE CvmGreyList* cvm_heap_grey_full(CvmHeap* heap) {
    return &heap->mark.full;
}
CVM_INLINE CvmGreyList* cvm_heap_grey_weak(CvmHeap* heap) {
    return &heap->mark.found_weak_refs;
}
CVM_INLINE CvmGreyPacket* cvm_heap_packets(CvmHeap* heap) {
    return heap->mark.packets;
}

/* Is this address inside the heap's usable window? */
CVM_INLINE bool cvm_heap_contains(CvmHeap const* heap, u8 const* p) {
    return p >= heap->heap_start && p < heap->heap_end;
}

#endif /* CVM_GC_HEAP_H */
