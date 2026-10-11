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
#include "base/spin.h"
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
    /*
     * The collector marks with a deque, and that is deliberate rather than
     * incidental. Its other option is to hand the packets it fills to the
     * global list, and the first version of this change did exactly that --
     * with a consequence that took a while to see. The collector takes from
     * that same list, one line above where it pushes, so a packet it had
     * just pushed it took straight back. The list never grew, the scaling
     * decision that reads it never fired, and read-heavy -- a chain whose
     * only width is its forward edges -- ran on one thread while the workers
     * slept. Fourteen percent slower, and the profile said every mark event
     * was on the collecting thread.
     *
     * With a deque the collector's packets wait where it does not look first:
     * it pops its own newest, and what it does not get to waits behind it,
     * counted by backlog(), and the workers steal it. Both halves are
     * load-bearing -- the waiting must be visible, and the collector must
     * not consume it itself.
     */
    cvm_tls_worklist = &heap->mark.work;
    cvm_mark_until_done(heap);
    cvm_tls_worklist = NULL;
}

/* Clear every weak reference whose target did not survive. */
static void nullify_all(CvmHeap* heap) {
    u64 spins = 0;
    while (!cvm_weak_is_done(heap)) {
        cvm_weak_nullify(heap);
        if (!cvm_weak_is_done(heap)) {
            rt_spin_backoff(spins++);
        } else {
            spins = 0;
        }
    }
}

/*
 * Reset the marking state so a collection starts from a known place.
 *
 * Every packet goes back on the empty list. That is the invariant the
 * completion check reads: marking is done when the packets on the empty and
 * weak lists account for all of them, so a packet left elsewhere would make
 * marking either finish early or never.
 *
 * This drains the global full list, every worker's deque, and the collector's
 * deque. Packets on the weak list are pushed back onto empty by the completion
 * checks for nullifying and sweeping, which both test against "all packets on
 * the empty list".
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

    /* Drain the workers' deques. */
    CvmGcPool* pool = heap->pool;
    if (pool != NULL) {
        for (u32 i = 0; i < pool->count; i++) {
            while ((packet = cvm_worklist_pop(&pool->threads[i].work)) != NULL) {
                cvm_grey_list_push(&heap->mark.empty, heap->mark.packets, packet);
            }
        }
    }

    /* Drain the collector's own deque. */
    while ((packet = cvm_worklist_pop(&heap->mark.work)) != NULL) {
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
        /*
         * Each node's *own* context, which is the whole point: the bump
         * cursor lives there, and clearing the caller's instead leaves every
         * parked thread allocating into a block the sweep is about to reuse.
         */
        cvm_allocator_clear(&node->allocator, &node->ctx);
    }
    cvm_threads_unlock_read();

    /*
     * The chunk free lists are the heap's, so they are dropped once, not once
     * per thread -- and dropping them per thread would be wrong as well as
     * pointless: a chunk filed by a GC worker's sweep belongs to a list every
     * mutator reads, and clearing it from each attach would throw away
     * whatever the previous one had filed.
     */
    cvm_large_clear(heap);

#if defined(CVM_DEBUG)
    /*
     * The invariant the above is supposed to establish, checked rather than
     * assumed: no thread's bump region points anywhere any more. This is the
     * check that would have caught the bug in one run instead of in a stress
     * test.
     */
    cvm_threads_lock_read();
    CVM_THREADS_FOREACH(node) {
        cvm_assert(node->ctx.bump_ptr == NULL);
        cvm_assert(node->ctx.bump_end == NULL);
    }
    cvm_threads_unlock_read();
#endif

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

/*
 * Wait out a collection that is in its concurrent window, helping it finish.
 *
 * The collection in progress has released the stop-the-world window and is
 * marking alongside the program. A thread that arrives here -- typically one
 * whose allocation failed and called the collector -- has nothing to gain
 * from starting a second collection into the first, and everything to gain
 * from the first ending: it helps drain the grey set, and it parks when the
 * final stop asks it to.
 *
 * Helping is safe for the same reasons concurrent marking is: marking is
 * idempotent and the packets are lock-free. The poll between rounds is not
 * decoration -- without it, this thread would be a managed thread that never
 * parks, and the final stop, which waits for every managed thread to park,
 * would wait forever. That is a deadlock this function would create all by
 * itself.
 */
static void help_and_wait(CvmHeap* heap) {
    u64 spins = 0;
    while (atomic_load_explicit(&heap->mark.busy, memory_order_acquire)) {
        cvm_safepoint_poll();
        cvm_mark(heap, false);
        rt_spin_backoff(spins++);
    }
}

/*
 * A collection that marks alongside the program. See CvmOptions.concurrent_mark.
 *
 * Two stops, and what each is for:
 *
 *   the first  snapshot the roots with the world stopped, then let it run.
 *               Roots are the frame chains, and a frame chain is not safe to
 *               walk while its owner is pushing and popping frames -- so this
 *               much has to happen under the stop, and only this much.
 *   the window  marking drains the grey set with the program running. The
 *               write barrier marks what stores publish, allocation starts
 *               life marked, and workers and the collecting thread share the
 *               work through the deques and the packet lists.
 *   the second  the frames changed while the program ran, so the roots are
 *               rescanned; whatever that finds is drained with the world
 *               stopped -- nothing new can arrive but a barrier from a thread
 *               the stop does not wait for -- and then the phases that always
 *               needed the stop run: nullify, sweep preparation.
 *
 * The pause a program feels is each stop, so each is recorded as one; the
 * collection's span is recorded separately, because "how long was the world
 * stopped" and "how long did the collection take" have different answers now.
 */
static bool cvm_gc_collect_concurrent(CvmHeap* heap) {
    cvm_thread_set_activity(CVM_ACT_COLLECTING);
    CvmStats* stats = cvm_stats_this_thread();

    /*
     * A collection in its concurrent window has released the stop, so the
     * stop's flag no longer means "a collection is running". Without this
     * check, a second collector would start into the first: it would reset
     * the marking state mid-mark, hand the sweeper a half-marked heap, and
     * the result would be the collector's own worst failure -- a live object
     * reclaimed. The check runs twice because it can pass, and then the
     * acquire can succeed, in the moment between another collection's first
     * stop and its release.
     */
    if (atomic_load_explicit(&heap->mark.busy, memory_order_acquire)) {
        help_and_wait(heap);
        return false;
    }
    if (!cvm_safepoint_acquire()) {
        return false;
    }
    if (atomic_load_explicit(&heap->mark.busy, memory_order_acquire)) {
        cvm_safepoint_release();
        help_and_wait(heap);
        return false;
    }
    atomic_store_explicit(&heap->mark.busy, true, memory_order_release);

    u64 event_start = cvm_stats_now(stats);

    /* ---- the first stop: root snapshot, and nothing else that can wait -- */
    u64 pause1_start = rt_time_nanos();

    /*
     * A previous sweep may still be running; drive it to completion here for
     * the same reason the stop-the-world collector does: the growth decision
     * below reads the free-block count, which is only meaningful once the
     * sweep has finished.
     */
    cvm_sweep_finish(heap, STR_LITERAL("before marking"));

#if defined(CVM_DEBUG)
    /* The previous sweep's outcome, checked before anything marks again:
     * every flag it set and every mark it saw must have been resolved. In
     * the concurrent collector this is the only moment the check is true --
     * once the window opens, allocate-black and the markers leave marks
     * that belong to this round and are not the sweep's to have cleared. */
    cvm_sweep_assert_consistent(heap);
#endif

    if (heap->mark.has_history) {
        cvm_heap_grow_if_needed(heap);
    }

    u32 worker_count = (heap->pool != NULL) ? heap->pool->count : 0;
    u32 initial_workers = (worker_count > CVM_MARK_INITIAL_WORKERS)
                              ? CVM_MARK_INITIAL_WORKERS
                              : worker_count;

    u64 start_ns = rt_time_nanos();
    u32 blocks_before = heap->block_count;

    reset_marking(heap);

    /* --- mark, concurrently from here --- */
    cvm_thread_set_activity(CVM_ACT_MARKING);
    u64 mark_start = rt_time_nanos();

    /*
     * The barrier goes live before the roots are scanned, and the ordering is
     * the one the stop-the-world collector uses: roots scanned under the stop
     * publish into the packet lists, and anything a store publishes after
     * this point is the barrier's to mark.
     */
    atomic_store_explicit(&cvm_marking_active, true, memory_order_release);
    cvm_mark_roots(heap);

    /*
     * Hand marking to the workers and let the world run. Not phase_run,
     * because phase_run waits for the workers to go idle, and waiting here
     * would be the whole stop-the-world collector again with extra steps.
     */
    if (heap->pool != NULL) {
        atomic_store_explicit(&heap->pool->phase, (u32)CVM_GC_MARK,
                              memory_order_release);
        cvm_pool_wake(heap->pool, CVM_GC_MARK, initial_workers);
    }

    cvm_safepoint_release();
    cvm_gc_counters_record_pause(pause1_start, rt_time_nanos());

    /* ---- the window: marking runs alongside the program ---- */
    cvm_tls_worklist = &heap->mark.work;
    cvm_mark_until_done(heap);
    cvm_tls_worklist = NULL;

    /* ---- the second stop: rescan, drain, and the phases that need it ---- */
    /*
     * The acquire can fail only to a transient holder -- a thread that took
     * the stop between ours, found `busy` set, and let go -- so retry with
     * the backoff rather than concluding anything from it.
     */
    u64 spins = 0;
    while (!cvm_safepoint_acquire()) {
        rt_spin_backoff(spins++);
    }
    u64 pause2_start = rt_time_nanos();

    /*
     * The frames changed while the program ran: slots were stored, frames
     * pushed and popped, handlers thrown past. None of that went through the
     * field barrier, because a frame slot is a root rather than a field, so
     * the roots are scanned again, here, under the stop. What the rescan
     * finds is drained next, and nothing new can arrive but a barrier from a
     * thread the stop does not wait for.
     */
    cvm_mark_roots(heap);

    /*
     * Wait for the workers before draining, not after: a worker mid-batch
     * holds packets that are on no list, so the drain could conclude "done"
     * with work still in flight. Waiting first puts every packet back on a
     * list, and then phase_run drains them and waits again for the ones it
     * wakes.
     */
    if (heap->pool != NULL) {
        cvm_pool_wait_idle(heap->pool);
    }
    cvm_tls_worklist = &heap->mark.work;
    phase_run(heap, CVM_GC_MARK, initial_workers, mark_all);
    cvm_tls_worklist = NULL;

    atomic_store_explicit(&cvm_marking_active, false, memory_order_release);
    u64 mark_end = rt_time_nanos();
    cvm_stats_record(stats, CVM_EVENT_MARK, mark_start, mark_end);

    /* --- nullify weak references --- */
    cvm_thread_set_activity(CVM_ACT_NULLIFYING);
    u64 nullify_start = rt_time_nanos();
    u32 weak_cleared = 0;
    phase_run(heap, CVM_GC_NULLIFY, initial_workers, nullify_all);
    cvm_stats_record(stats, CVM_EVENT_NULLIFY, nullify_start, rt_time_nanos());

    /* --- start the sweep --- */
    /*
     * The debug block states are prepared here rather than at the first stop,
     * and the concurrent window is why. They say "every block is about to be
     * swept" -- which is true here, and was true for the whole collection
     * when there was one stop. Setting them at the first stop marked the
     * blocks sitting on the allocator's free lists as not-yet-swept, and then
     * let the world run: a mutator taking one of those blocks found it in a
     * state the free lists never hand out, and the check that exists to catch
     * exactly that caught it.
     */
#if defined(CVM_DEBUG)
    cvm_sweep_clear_debug_state(heap);
#endif

    u64 sweep_start = rt_time_nanos();
    prepare_for_sweep(heap);
    cvm_stats_record(stats, CVM_EVENT_SWEEP, sweep_start, rt_time_nanos());

    if (heap->pool != NULL) {
        atomic_store_explicit(&heap->pool->phase, (u32)CVM_GC_IDLE,
                              memory_order_release);
    }

    heap->mark.last_mark_ns = mark_end - mark_start;
    heap->mark.last_period_ns = mark_end - heap->mark.last_end_ns;
    heap->mark.last_end_ns = mark_end;
    heap->mark.current_start_ns = mark_start;
    heap->mark.current_end_ns = mark_end;
    heap->mark.has_history = true;

    cvm_safepoint_release();
    u64 end_ns = rt_time_nanos();
    cvm_gc_counters_record_pause(pause2_start, end_ns);

    /* Hand the sweep to the pool, exactly as the stop-the-world collector
     * does: after the release, sized by the work, not through phase_run. */
    if (heap->pool != NULL) {
        u32 swept = atomic_load(&heap->sweep.cursor);
        u32 left = (heap->sweep.limit > swept) ? heap->sweep.limit - swept : 0;
        u32 batches = left / CVM_SWEEP_BATCH_SIZE;

        if (batches >= 2) {
            cvm_pool_wake(heap->pool, CVM_GC_SWEEP,
                          (batches < worker_count) ? batches : worker_count);
        }
    }

    last_collection = (CvmCollectionStats){
        .start_ns = start_ns,
        .end_ns = end_ns,
        .mark_ns = mark_end - mark_start,
        .weak_cleared = weak_cleared,
        .blocks_before = blocks_before,
        .blocks_after = heap->block_count,
        .grew = false,
    };

    /* One collection, one span; the pauses were recorded per window. */
    cvm_stats_record(stats, CVM_EVENT_COLLECTION, event_start, end_ns);
    cvm_gc_counters_record_span(start_ns, end_ns);

    CVM_LOG_INFO(m) {
        fmt_lit(&m.fmt, "collected (concurrent): weak cleared ");
        fmt_u32(&m.fmt, weak_cleared);
        fmt_lit(&m.fmt, ", mark ");
        fmt_u64(&m.fmt, (mark_end - mark_start) / 1000);
        fmt_lit(&m.fmt, "us, pauses ");
        fmt_u64(&m.fmt, (mark_start - pause1_start) / 1000);
        fmt_lit(&m.fmt, "+");
        fmt_u64(&m.fmt, (end_ns - pause2_start) / 1000);
        fmt_lit(&m.fmt, "us, sweep started");
    }

    atomic_store_explicit(&heap->mark.busy, false, memory_order_release);

    cvm_thread_set_activity(CVM_ACT_IDLE);
    return true;
}

bool cvm_gc_collect(CvmHeap* heap) {
    if (heap->mark.concurrent_enabled) {
        return cvm_gc_collect_concurrent(heap);
    }
    cvm_thread_set_activity(CVM_ACT_COLLECTING);

    /*
     * The thread's event buffer, or NULL. Recording is a few stores, and
     * the whole-collection event below is what a caller reads to see how
     * long the world was stopped; without it the pause figures the
     * benchmark reports would have no per-phase breakdown behind them.
     */
    CvmStats* stats = cvm_stats_this_thread();
    u64 event_start = cvm_stats_now(stats);

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
    cvm_sweep_finish(heap, STR_LITERAL("before marking"));

    /*
     * Now that any unfinished sweep has been driven to completion, the
     * free-block count is meaningful, so this is where the growth decision
     * belongs.
     */
    if (heap->mark.has_history) {
        cvm_heap_grow_if_needed(heap);
    }

    /*
     * How many workers to wake *up front*.
     *
     * Not all of them, and the reasoning that said to wake all of them was
     * wrong. It read "a worker with nothing to do stops immediately, so
     * waking too many costs a check rather than a busy thread" — but a
     * worker does not wake for free. It costs a semaphore post, a context
     * switch, and a scheduling round trip, tens of microseconds each, and
     * that is paid before the worker can discover it has nothing to do.
     *
     * Measured on tree-keep, with the same collection points throughout:
     * waking the whole pool of nine gives the same marking time as waking
     * three, and on the small collections at the start of a run it is a net
     * loss — 0.60x, slower than doing the work on one thread.
     *
     * Worse, waking everyone up front disabled the scaling that exists for
     * exactly this reason. cvm_pool_scale_markers adds workers when the full
     * packet list says marking is falling behind, but it cannot add anyone
     * who is already awake: `wanted > active` is never true when active is
     * the whole pool. It was dead code.
     *
     * So: start with a couple, and let the backlog bring the rest.
     */
    u32 worker_count = (heap->pool != NULL) ? heap->pool->count : 0;
    u32 initial_workers = (worker_count > CVM_MARK_INITIAL_WORKERS)
                              ? CVM_MARK_INITIAL_WORKERS
                              : worker_count;

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
    cvm_thread_set_activity(CVM_ACT_MARKING);
    u64 mark_start = rt_time_nanos();

    /*
     * From here to the end of the mark phase, the write barrier is live: a
     * store from a thread this collection is not waiting for has to mark what
     * it publishes, or the marker can miss it.
     *
     * Cleared before the sweep begins, and that is the part that is not
     * obvious. The safepoint flag stays set for the whole window, and using it
     * here re-marked blocks the sweeper had already finished with, which its
     * consistency check reported as "a block is still marked after sweeping".
     */
    atomic_store_explicit(&cvm_marking_active, true, memory_order_release);
    cvm_mark_roots(heap);
    phase_run(heap, CVM_GC_MARK, initial_workers, mark_all);
    atomic_store_explicit(&cvm_marking_active, false, memory_order_release);

    u64 mark_end = rt_time_nanos();
    cvm_stats_record(stats, CVM_EVENT_MARK, mark_start, mark_end);

    /* --- nullify weak references --- */
    cvm_thread_set_activity(CVM_ACT_NULLIFYING);
    u64 nullify_start = rt_time_nanos();
    u32 weak_cleared = 0;
    phase_run(heap, CVM_GC_NULLIFY, initial_workers, nullify_all);
    cvm_stats_record(stats, CVM_EVENT_NULLIFY, nullify_start, rt_time_nanos());

    /*
     * --- start the sweep ---
     *
     * The sweep deliberately continues after the world resumes, so only the
     * preparation happens here. Workers are not woken for it either: the
     * preparation empties the block allocator, so there is nothing to sweep
     * until a mutator needs memory, and that mutator drives it.
     */
    u64 sweep_start = rt_time_nanos();
    prepare_for_sweep(heap);
    /*
     * What this measures is the sweep's preparation, not the sweep. The
     * sweep itself is deferred and driven by whoever allocates next, so it
     * has no single interval to report; its cost appears as `sweepbatch`
     * and `coalescebatch` rows, one per unit of work. Naming that here keeps
     * a reader from adding this row to those and reading a phase twice.
     */
    cvm_stats_record(stats, CVM_EVENT_SWEEP, sweep_start, rt_time_nanos());

    if (heap->pool != NULL) {
        atomic_store_explicit(&heap->pool->phase, (u32)CVM_GC_IDLE,
                              memory_order_release);
    }

    /*
     * Record this collection's figures for the next collection's growth
     * decision. The period is the time between this collection's marking and
     * the previous one's — the program's running time, not the collection's
     * duration.
     *
     * These are a handful of stores and belong inside the stop, which is
     * where they are: they are not why the recording below moved.
     */
    heap->mark.last_mark_ns = mark_end - mark_start;
    heap->mark.last_period_ns = mark_end - heap->mark.last_end_ns;
    heap->mark.last_end_ns = mark_end;
    heap->mark.current_start_ns = mark_start;
    heap->mark.current_end_ns = mark_end;
    heap->mark.has_history = true;

    /*
     * The stop ends here, and it is measured here.
     *
     * The ordering is the whole of the fix, and it was wrong in a way that
     * flattered the collector. The release is what ends the stop, so a
     * timestamp taken before it understates the pause — and worse, the
     * counter update, the summary struct and the log line all used to run
     * *before* the release, which put them inside the stop but outside the
     * measurement. The error grew with the log level: a debug-level run
     * writes a line per collection through a syscall, stopping the world
     * longer while reporting the same collection time.
     *
     * Everything after this point touches only locals and atomics that no
     * mutator reads for correctness, so it is safe with the world running.
     */
    cvm_safepoint_release();
    u64 end_ns = rt_time_nanos();

    /*
     * Hand the sweep to the pool.
     *
     * After the release, deliberately, and not through phase_run: the point
     * of a deferred sweep is that nobody waits for it, and phase_run's
     * contract is to wait for every worker to go idle. Waking them inside the
     * stop would put the sweep's cost into the pause, which is the one thing
     * this design exists to avoid.
     *
     * The number woken is bounded by the work rather than by the pool. Each
     * handoff costs a semaphore post and a thread wakeup, which is real
     * money next to a batch of 32 blocks, so waking seven threads to look at
     * two batches makes the sweep slower for everyone, including the mutators
     * that would have swept it themselves on the way back from an allocation.
     *
     * Workers do not finish the sweep — only its batches. Deciding that the
     * sweep is over means the coalescing pass has run and the post-sweep
     * bookkeeping is done, and that is the collector's or a mutator's job,
     * because a worker that decided it would be a thread with the least
     * information about the heap making the call with the widest consequences.
     */
    if (heap->pool != NULL) {
        u32 swept = atomic_load(&heap->sweep.cursor);
        u32 left = (heap->sweep.limit > swept) ? heap->sweep.limit - swept : 0;
        u32 batches = left / CVM_SWEEP_BATCH_SIZE;

        if (batches >= 2) {
            cvm_pool_wake(heap->pool, CVM_GC_SWEEP,
                          (batches < worker_count) ? batches : worker_count);
        }
    }

    last_collection = (CvmCollectionStats){
        .start_ns = start_ns,
        .end_ns = end_ns,
        .mark_ns = mark_end - mark_start,
        .weak_cleared = weak_cleared,
        .blocks_before = blocks_before,
        .blocks_after = heap->block_count,
        .grew = false, /* decided at the start of the next collection */
    };

    cvm_stats_record(stats, CVM_EVENT_COLLECTION, event_start, end_ns);
    cvm_gc_counters_record_collection(start_ns, end_ns);

    CVM_LOG_INFO(m) {
        fmt_lit(&m.fmt, "collected: weak cleared ");
        fmt_u32(&m.fmt, weak_cleared);
        fmt_lit(&m.fmt, ", mark ");
        fmt_u64(&m.fmt, (mark_end - mark_start) / 1000);
        fmt_lit(&m.fmt, "us, pause ");
        fmt_u64(&m.fmt, (end_ns - start_ns) / 1000);
        fmt_lit(&m.fmt, "us, sweep started");
    }

    cvm_thread_set_activity(CVM_ACT_IDLE);
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
    cvm_sweep_finish(heap, STR_LITERAL("after the collection"));
}
