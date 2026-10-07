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
#include "cvm/weak.h"
#include "log.h"
#include "rt.h"
#include "safepoint.h"
#include "stats.h"

static CvmCollectionStats last_collection;

CvmCollectionStats cvm_gc_last_collection(void) {
    return last_collection;
}

/* ------------------------------------------------------------------ */
/* phases                                                              */
/* ------------------------------------------------------------------ */

/*
 * Run a phase with whatever help is available.
 *
 * `work` is this thread's own share, which is the same work the workers
 * do: with no pool it is the whole phase, and with one it is the share the
 * thread that then has to notice the phase is over must do itself.
 *
 * The order is: set the phase, do our share, wake the workers, wait for
 * them. Doing our share first matters for marking — the root scan has to
 * produce packets before a worker that wakes finds anything, or it finds
 * an empty list, concludes there is nothing to do, and stops.
 */
static void phase_run(CvmHeap* heap, CvmGcPhase phase, u32 workers,
                      void (*work)(CvmHeap*)) {
    CvmGcPool* pool = heap->pool;

    if (pool == NULL) {
        work(heap);
        return;
    }

    atomic_store_explicit(&pool->phase, (u32)phase, memory_order_release);
    work(heap);
    cvm_pool_wake(pool, phase, workers);
    cvm_pool_wait_idle(pool);
}

/* Mark until nothing is left to trace. */
static void mark_all(CvmHeap* heap) {
    cvm_mark_until_done(heap);
}

/* Clear every weak reference whose target did not survive. */
static void nullify_all(CvmHeap* heap) {
    while (!cvm_weak_is_done(heap)) {
        cvm_weak_nullify(heap);
        if (!cvm_weak_is_done(heap)) {
            rt_thread_yield();
        }
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

    /*
     * Now that any unfinished sweep has been driven to completion, the
     * free-block count is meaningful, so this is where the growth decision
     * belongs.
     */
    if (heap->mark.has_history) {
        cvm_heap_grow_if_needed(heap);
    }

    /*
     * How many workers to wake for a phase. All of them: a worker with
     * nothing to do stops immediately, so waking too many costs a check
     * rather than a busy thread.
     */
    u32 worker_count = (heap->pool != NULL) ? heap->pool->count : 0;

    u64 start_ns = rt_time_nanos();
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
    cvm_mark_roots(heap);
    phase_run(heap, CVM_GC_MARK, worker_count, mark_all);
    u64 mark_end = rt_time_nanos();

    /* --- nullify weak references --- */
    u32 weak_cleared = 0;
    phase_run(heap, CVM_GC_NULLIFY, worker_count, nullify_all);

    /*
     * --- start the sweep ---
     *
     * The sweep deliberately continues after the world resumes, so only the
     * preparation happens here. Workers are not woken for it either: the
     * preparation empties the block allocator, so there is nothing to sweep
     * until a mutator needs memory, and that mutator drives it.
     */
    if (heap->pool != NULL) {
        atomic_store_explicit(&heap->pool->phase, (u32)CVM_GC_IDLE,
                              memory_order_release);
    }
    prepare_for_sweep(heap);

    u64 end_ns = rt_time_nanos();

    /*
     * Record this collection's figures for the next one's growth
     * decision. The period is the time between this collection's marking
     * and the previous one's — the program's running time, not the
     * collection's duration.
     */
    heap->mark.last_mark_ns = mark_end - mark_start;
    heap->mark.last_period_ns = mark_end - heap->mark.last_end_ns;
    heap->mark.last_end_ns = mark_end;
    heap->mark.current_start_ns = mark_start;
    heap->mark.current_end_ns = mark_end;
    heap->mark.has_history = true;

    bool grew = false; /* decided at the start of the next collection */

    if (heap->pool != NULL) {
        atomic_store_explicit(&heap->pool->phase, (u32)CVM_GC_IDLE,
                              memory_order_release);
    }

    last_collection = (CvmCollectionStats){
        .start_ns = start_ns,
        .end_ns = end_ns,
        .mark_ns = mark_end - mark_start,
        .weak_cleared = weak_cleared,
        .blocks_before = blocks_before,
        .blocks_after = heap->block_count,
        .grew = grew,
    };

    cvm_gc_counters_record_collection(start_ns, end_ns);

    CVM_LOG_INFO(m) {
        fmt_lit(&m.fmt, "collected: weak cleared ");
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
