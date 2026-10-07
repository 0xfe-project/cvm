/*
 * vm/gc/collect.c
 *
 * The collection driver. See vm/gc/collect.h for the shape.
 *
 * ---------------------------------------------------------------------
 * Where the stop-the-world ends
 * ---------------------------------------------------------------------
 *
 * Marking and nullifying happen with the world stopped. Sweeping does not:
 * the collector starts it and resumes the world, and the sweeping is
 * carried on by whoever needs memory.
 *
 * That is the point of the batch machinery and the coalesce_me flag. A
 * sweep that had to finish before anyone could allocate again would stall
 * every thread for as long as it takes to walk the whole heap, and the
 * program would spend that time doing nothing. Instead the sweep proceeds
 * alongside the program, and a thread that runs out of memory sweeps a
 * batch itself rather than waiting.
 *
 * So the stop-the-world window is mark plus nullify, and the sweep is
 * paid for by the allocation that needed it.
 *
 * ---------------------------------------------------------------------
 * Only one collection at a time
 * ---------------------------------------------------------------------
 *
 * If a previous sweep is still unfinished when a new collection starts,
 * the collector drives it to completion first rather than waiting for
 * someone else to. Waiting would be a deadlock in the case where every
 * mutator is happily allocating from the reserve and nobody has any
 * reason to sweep; driving it is the same work, done by a thread that
 * already has the world stopped.
 */

#include "gc/collect.h"

#include <stdatomic.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "gc/alloc.h"
#include "gc/block_alloc.h"
#include "gc/bytemap.h"
#include "gc/heap.h"
#include "gc/large.h"
#include "gc/layout.h"
#include "gc/marker.h"
#include "gc/meta.h"
#include "gc/pool.h"
#include "gc/sweeper.h"
#include "gc/weak.h"
#include "log.h"
#include "rt.h"
#include "safepoint.h"
#include "stats.h"

static CvmCollectionStats last_collection;

CvmCollectionStats cvm_gc_last_collection(void) {
    return last_collection;
}

/* ------------------------------------------------------------------ */
/* counting, for the stats and the tests                               */
/* ------------------------------------------------------------------ */

/* Objects with an allocated or marked slot, across the blocks in use. */
static u32 count_objects(CvmHeap* heap) {
    u32 count = 0;
    for (u32 i = 0; i < heap->block_count; i++) {
        u8* block_start = cvm_heap_block_address(heap, i);
        ObjectMeta* slot = bytemap_get(heap->bytemap, block_start);
        for (usize s = 0; s < CVM_SLOTS_PER_BLOCK; s++) {
            if (object_meta_is_allocated(&slot[s]) || object_meta_is_marked(&slot[s])) {
                count++;
            }
        }
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* phases                                                              */
/* ------------------------------------------------------------------ */

static void phase_set(CvmHeap* heap, CvmGcPhase phase) {
    if (heap->pool != NULL) {
        atomic_store_explicit(&heap->pool->phase, (u32)phase, memory_order_release);
    }
}

/*
 * Reset the marking state so a collection starts from a known place.
 *
 * Every packet goes back on the empty list. That is the invariant the
 * completion check reads: marking is done when the packets on the empty
 * and weak lists account for all of them, so a packet left elsewhere
 * would make marking either finish early or never.
 */
static void reset_marking(CvmHeap* heap) {
    CvmGreyPacket* packet;
    while ((packet = cvm_grey_list_pop(&heap->mark.found_weak_refs,
                                       heap->mark.packets)) != NULL) {
        cvm_grey_list_push(&heap->mark.empty, heap->mark.packets, packet);
    }
    while ((packet = cvm_grey_list_pop(&heap->mark.full, heap->mark.packets)) != NULL) {
        cvm_grey_list_push(&heap->mark.empty, heap->mark.packets, packet);
    }
}

/*
 * Drop every thread's cached allocation state, and the block allocator's.
 *
 * This has to happen before sweeping, and the reason is worth stating: a
 * thread's bump region points into a block, and its recycled list holds
 * blocks the previous sweep produced. Both describe blocks whose contents
 * this sweep is about to re-derive, and leaving them would let a thread
 * allocate into a block the sweeper has just handed to someone else.
 *
 * Nothing is lost. Everything a thread was holding is still described by
 * the heap metadata, and the sweep re-derives which parts of it are free.
 */
static void prepare_for_sweep(CvmHeap* heap) {
    cvm_threads_lock_read();
    CVM_THREADS_FOREACH(node) {
        cvm_allocator_clear(&node->allocator);
        cvm_large_clear(&node->large_allocator);
    }
    cvm_threads_unlock_read();

    /*
     * The block allocator's free lists are emptied too, for the same
     * reason: a block that is free now has nothing marked in it, so the
     * sweep returns it as free. Keeping the old lists would double-count
     * blocks the sweep also returns.
     *
     * The reserve goes back first, so a mutator resuming immediately has
     * somewhere to allocate from without sweeping.
     */
    cvm_block_alloc_clear(heap->block_alloc);
    cvm_block_alloc_use_reserve(heap->block_alloc);

    cvm_sweep_begin(heap);
}

/* ------------------------------------------------------------------ */
/* growth                                                              */
/* ------------------------------------------------------------------ */

/*
 * Whether to grow, and by how much.
 *
 * Growing here rather than on the allocation path is what makes the
 * decision informed: this is the one moment the collector knows how much
 * survived and how long marking took.
 */
static bool maybe_grow(CvmHeap* heap, u64 mark_time_ns, u64 total_time_ns) {
    u32 free_blocks = cvm_block_alloc_free_count(heap->block_alloc);
    if (!cvm_heap_should_grow(heap, mark_time_ns, total_time_ns, free_blocks)) {
        return false;
    }

    u32 increment = cvm_heap_growth_increment(heap);
    if (increment == 0) {
        return false;
    }

    u32 first = 0;
    if (!cvm_heap_extend(heap, increment, &first)) {
        return false;
    }

    /*
     * The new blocks are free and their metadata is cleared, so they go
     * straight to the allocator. They arrive as one run, and their debug
     * state is set the way the sweeper would leave it.
     */
    for (u32 i = 0; i < increment; i++) {
        block_meta_set_debug_state(cvm_heap_block_meta(heap, first + i), CVM_DBG_FREE);
    }
    cvm_block_alloc_add_free_superblock(heap->block_alloc,
                                        cvm_heap_block_meta(heap, first), increment);

    CVM_LOG_DEBUG(m) {
        fmt_lit(&m.fmt, "heap grew by ");
        fmt_u32(&m.fmt, increment);
        fmt_lit(&m.fmt, " blocks");
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* the driver                                                          */
/* ------------------------------------------------------------------ */

bool cvm_gc_collect(CvmHeap* heap) {
    if (!cvm_safepoint_acquire()) {
        /* Another thread is collecting, and has just finished. The
         * caller's next move is to retry its allocation. */
        return false;
    }

    /*
     * A previous sweep may still be in progress: it runs alongside the
     * program by design. Finish it here rather than waiting, because
     * waiting would hang if no mutator had any reason to sweep. The world
     * is stopped, so driving it is as cheap as it can be.
     */
    while (!cvm_sweep_is_done(heap)) {
        u32 cursor_before = atomic_load(&heap->sweep.cursor);
        u32 coalesce_before = atomic_load(&heap->sweep.coalesce_done);

        cvm_sweep_drive(heap, cvm_thread_node(), CVM_SWEEP_BATCH_SIZE);

        if (atomic_load(&heap->sweep.cursor) == cursor_before &&
            atomic_load(&heap->sweep.coalesce_done) == coalesce_before) {
            /*
             * No progress is possible from here. Break rather than spin,
             * and let the sweep that is left be finished by the mutators
             * that are about to resume.
             */
            break;
        }
    }

    u64 start_ns = rt_time_nanos();
    u32 objects_before = count_objects(heap);
    u32 blocks_before = heap->block_count;

#if defined(CVM_DEBUG)
    /*
     * Every block is about to be swept, and the sweeper asserts that it
     * finds them in that state. The states blocks carry while the program
     * runs — in use, partially free, free — describe the previous cycle,
     * not this one.
     */
    cvm_sweep_clear_debug_state(heap);
    cvm_sweep_assert_consistent(heap);
#endif

    reset_marking(heap);

    /* --- mark --- */
    u64 mark_start = rt_time_nanos();
    phase_set(heap, CVM_GC_MARK);
    cvm_mark_roots(heap);
    cvm_mark_until_done(heap);
    u64 mark_end = rt_time_nanos();

    /* --- nullify weak references --- */
    phase_set(heap, CVM_GC_NULLIFY);
    u32 weak_cleared = cvm_weak_nullify(heap);

    /* --- start the sweep, which continues after the world resumes --- */
    phase_set(heap, CVM_GC_SWEEP);
    prepare_for_sweep(heap);

    u64 end_ns = rt_time_nanos();
    bool grew = maybe_grow(heap, mark_end - mark_start, end_ns - mark_start);

    phase_set(heap, CVM_GC_IDLE);

    last_collection = (CvmCollectionStats){
        .start_ns = start_ns,
        .end_ns = end_ns,
        .mark_ns = mark_end - mark_start,
        .objects_before = objects_before,
        .objects_after = objects_before, /* not known until the sweep ends */
        .weak_cleared = weak_cleared,
        .blocks_before = blocks_before,
        .blocks_after = heap->block_count,
        .grew = grew,
    };

    cvm_gc_counters_record_collection(start_ns, end_ns);

    CVM_LOG_INFO(m) {
        fmt_lit(&m.fmt, "collected: objects ");
        fmt_u32(&m.fmt, objects_before);
        fmt_lit(&m.fmt, ", weak cleared ");
        fmt_u32(&m.fmt, weak_cleared);
        fmt_lit(&m.fmt, ", mark ");
        fmt_u64(&m.fmt, (mark_end - mark_start) / 1000);
        fmt_lit(&m.fmt, "us, sweep ");
        fmt_lit(&m.fmt, grew ? "started (heap grew)" : "started");
    }

    cvm_safepoint_release();
    return true;
}

/* ------------------------------------------------------------------ */
/* helpers for tests                                                   */
/* ------------------------------------------------------------------ */

/*
 * Run a collection and then drive the sweep to completion, so that a test
 * can inspect the heap in a settled state.
 *
 * The production path deliberately leaves the sweep unfinished; this is
 * the shape a test wants, and it is also what a program wants at exit.
 */
void cvm_gc_collect_and_finish(CvmHeap* heap) {
    cvm_gc_collect(heap);

    /*
     * Drive the sweep to the end. cvm_sweep_drive's return value says
     * whether the sweep is finished, not whether this call made progress,
     * so the loop tests the state and breaks only on genuine lack of
     * progress — which cannot happen here, since this thread is the only
     * one sweeping, but is worth guarding against rather than spinning.
     */
    while (!cvm_sweep_is_done(heap)) {
        u32 cursor_before = atomic_load(&heap->sweep.cursor);
        u32 coalesce_before = atomic_load(&heap->sweep.coalesce_done);

        cvm_sweep_drive(heap, cvm_thread_node(), CVM_SWEEP_BATCH_SIZE);

        if (atomic_load(&heap->sweep.cursor) == cursor_before &&
            atomic_load(&heap->sweep.coalesce_done) == coalesce_before) {
            break;
        }
    }
}
