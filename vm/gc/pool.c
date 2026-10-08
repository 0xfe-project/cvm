/*
 * vm/gc/pool.c
 *
 * The concurrent GC worker pool. See vm/gc/pool.h for the shape, why the
 * wake-up is a counting semaphore, and why the collecting thread is the
 * master.
 *
 * Every thread runs the same loop: wake, read the phase, do as much of
 * that phase as there is to do, stop. The thread that triggered the
 * collection does the same work and then waits for the others — it is the
 * one that has to notice the phase is finished, because only it knows
 * what "finished" means for this collection.
 */

#include "gc/pool.h"

#include <stdatomic.h>

#include "base/types.h"
#include "cvm/gc.h"
#include "cvm/weak.h"
#include "gc/heap.h"
#include "gc/marker.h"
#include "gc/sweeper.h"
#include "log.h"
#include "rt.h"
#include "stats.h"

/* ------------------------------------------------------------------ */
/* sizing                                                              */
/* ------------------------------------------------------------------ */

u32 cvm_pool_default_count(CvmHeap const* heap) {
    u32 hardware = rt_hardware_threads();

    /*
     * One worker per hardware thread except the one the program is using,
     * since a worker that shares a core with a mutator makes the mutator
     * slower at exactly the wrong moment.
     */
    u32 count = (hardware > 1) ? hardware - 1 : 1;
    if (count > CVM_MAX_GC_WORKERS) {
        count = CVM_MAX_GC_WORKERS;
    }

    /*
     * Also bounded by the packet supply. More markers than packets means
     * some of them find nothing and stop immediately, which is harmless
     * but pointless; the packet count is the amount of work there is to
     * divide.
     */
    u32 by_packets = heap->mark.packet_count / CVM_MARK_MIN_PACKETS_PER_THREAD;
    if (by_packets == 0) {
        by_packets = 1;
    }
    if (count > by_packets) {
        count = by_packets;
    }

    return (count == 0) ? 1 : count;
}

/* ------------------------------------------------------------------ */
/* one thread's loop                                                   */
/* ------------------------------------------------------------------ */

static void do_mark(CvmHeap* heap, CvmStats* stats) {
    /* Take packets until there are none left, then stop. The collector
     * will wake this thread again if the backlog grows. */
    u64 start = cvm_stats_now(stats);
    cvm_mark(heap, false);
    cvm_stats_record(stats, CVM_EVENT_CONCURRENT_MARK, start, cvm_stats_now(stats));
}

static void do_nullify(CvmHeap* heap, CvmStats* stats) {
    /* Nullifying reads marks and writes to weak slots; it is idempotent,
     * so draining whatever is there is enough. */
    u64 start = cvm_stats_now(stats);
    (void)cvm_weak_nullify(heap);
    cvm_stats_record(stats, CVM_EVENT_CONCURRENT_NULLIFY, start, cvm_stats_now(stats));
}

/*
 * Sweep batches until the cursor runs out.
 *
 * Two things this deliberately does not do.
 *
 * It does not finish the sweep. cvm_sweep_try_finish decides that the whole
 * sweep is over — coalescing has run, post-sweep bookkeeping is done — and
 * that is left to the collector and to mutators, because it is a decision
 * about the heap rather than about one batch, and a worker has the least
 * context in which to make it. A worker that exhausted the cursor simply
 * reports how far it got and goes back to waiting.
 *
 * It passes no thread node, so a block this thread sweeps goes to the heap's
 * free list rather than into this thread's bump region. Workers do not
 * allocate, so a block given to one would sit unused until the next
 * collection cleared the region out — a leak with a five-minute lifetime,
 * and the kind that never shows up in a test small enough to notice.
 */
static void do_sweep(CvmGcThread* thread, CvmHeap* heap) {
    while (atomic_load(&heap->sweep.cursor) < heap->sweep.limit) {
        cvm_sweep(heap, &thread->sweep_cursor_done, CVM_SWEEP_BATCH_SIZE, NULL);
    }
}

static opaque_mut gc_thread_main(opaque_mut arg) {
    CvmGcThread* thread = arg;
    CvmGcPool* pool = thread->pool;
    CvmHeap* heap = pool->heap;

    for (;;) {
        atomic_store_explicit(&thread->active, false, memory_order_release);

        /*
         * Write down what was recorded before sleeping, by this thread and not
         * by whoever is curious enough to ask: the ring belongs to its thread,
         * and flushing it from another one is a data race on that thread's
         * buffer index.
         *
         * It also makes the output usable. A buffer holds 2000 events, so
         * without this the concurrent phases appear in a stats file only after
         * a long run, and a short one — the kind you write while checking that
         * the recording works at all — shows a collection with no worker
         * activity in it.
         */
        cvm_stats_flush(thread->stats);

        rt_sem_wait(&pool->start);

        if (atomic_load(&pool->shutdown)) {
            break;
        }

        atomic_store_explicit(&thread->active, true, memory_order_release);

        /*
         * A full fence before reading the phase, and another after the
         * work. Without them a thread could see a phase change before the
         * data that phase depends on — the sweep cursor, the packet lists,
         * the heap's end pointers.
         */
        atomic_thread_fence(memory_order_seq_cst);

        u32 phase = atomic_load_explicit(&pool->phase, memory_order_acquire);

        /*
         * A sweep handoff is announced before it is retired, and the order is
         * the whole point. "Is a sweep in flight" has two halves — a handoff
         * not yet consumed, or a thread inside a batch — and retiring first
         * opens a window where a consumed post is counted as neither: the
         * collector says "no sweep in flight", begins a new round, and this
         * thread goes on to sweep that round while still carrying the previous
         * round's progress. cvm_sweep's bound assertion is exactly that claim.
         * Announcing first means the two counters never read clear together.
         */
        if ((CvmGcPhase)phase == CVM_GC_SWEEP) {
            atomic_store_explicit(&thread->sweeping, true, memory_order_release);
        }

        /*
         * Retire the handoff that woke this thread, here rather than in
         * whichever case it goes on to run. The posts are not typed, so this
         * is the one place that can say "a post was consumed" truthfully, and
         * putting it in a phase-specific case is how the count gets stranded: a
         * thread woken for a sweep that a collection has since overtaken runs
         * the marking case instead, and a wake retired only by the sweeping
         * case would never be retired at all. A count that can strand above
         * zero turns "is the sweep over" into "never", which is a hang rather
         * than an inaccuracy.
         */
        atomic_fetch_sub_explicit(&pool->wake_pends, 1u, memory_order_release);

        switch ((CvmGcPhase)phase) {
        case CVM_GC_IDLE:
            break;
        case CVM_GC_COALESCE: {
            u64 start = cvm_stats_now(thread->stats);
            /* A run is waiting to be closed. One thread does it; the wake
             * is sized at one because the pass is not parallel-safe. */
            cvm_sweep_lazy_coalesce(heap);
            cvm_stats_record(thread->stats, CVM_EVENT_COALESCE_BATCH, start,
                             cvm_stats_now(thread->stats));
            break;
        }
        case CVM_GC_MARK:
            do_mark(heap, thread->stats);
            break;
        case CVM_GC_NULLIFY:
            do_nullify(heap, thread->stats);
            break;
        case CVM_GC_SWEEP: {
            u64 start = cvm_stats_now(thread->stats);

            /* Already announced, above, before the handoff was retired. */
            do_sweep(thread, heap);

            /*
             * Past the end, meaning "I hold nothing below here".
             *
             * The bound is published before the withdrawal, in that order:
             * a thread that stopped counting itself while still short of its
             * last claim would let coalescing advance past work still in
             * flight. Together the two stores say "this thread is done, and
             * here is how far done reaches", and neither half is useful
             * without the other.
             *
             * The zero these replace was put there by cvm_pool_wake rather
             * than here, so that no worker is ever observable as sweeping with
             * a bound left over from the previous round.
             */
            atomic_store_explicit(&thread->sweep_cursor_done, heap->sweep.limit,
                                  memory_order_release);
            atomic_store_explicit(&thread->sweeping, false, memory_order_release);

            cvm_stats_record(thread->stats, CVM_EVENT_CONCURRENT_SWEEP, start,
                             cvm_stats_now(thread->stats));
            break;
        }
        }

        atomic_thread_fence(memory_order_seq_cst);
    }

    atomic_store_explicit(&thread->active, false, memory_order_release);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

bool cvm_pool_init(CvmHeap* heap, u32 count) {
    if (heap->pool != NULL) {
        return true; /* already up */
    }

    if (count == 0) {
        count = cvm_pool_default_count(heap);
    }
    if (count > CVM_MAX_GC_WORKERS) {
        count = CVM_MAX_GC_WORKERS;
    }
    if (count == 0) {
        count = 1;
    }

    CvmGcPool* pool = rt_mem_alloc(sizeof(CvmGcPool));
    if (pool == NULL) {
        return false;
    }

    pool->heap = heap;
    pool->count = count;
    atomic_store(&pool->phase, (u32)CVM_GC_IDLE);
    atomic_store(&pool->shutdown, false);
    atomic_store(&pool->wake_pends, 0);

    if (!rt_sem_init(&pool->start, 0)) {
        rt_mem_free(pool);
        return false;
    }

    for (u32 i = 0; i < count; i++) {
        CvmGcThread* thread = &pool->threads[i];
        thread->id = i;
        thread->pool = pool;
        atomic_store(&thread->active, false);
        atomic_store(&thread->sweeping, false);
        atomic_store(&thread->sweep_cursor_done, 0);
        thread->stats = cvm_stats_open();
    }

    /*
     * Publish the pool before the threads exist. A worker that woke early
     * would look for heap->pool and find NULL. Done before the threads
     * start, and undone if none of them do.
     */
    heap->pool = pool;

    u32 started = 0;
    for (u32 i = 0; i < count; i++) {
        if (!rt_thread_create(&pool->threads[i].handle, gc_thread_main,
                              &pool->threads[i])) {
            CVM_LOG_ERROR(m) {
                fmt_lit(&m.fmt, "could not start GC worker ");
                fmt_u32(&m.fmt, i);
            }
            break;
        }
        started++;
    }

    if (started == 0) {
        heap->pool = NULL;
        rt_sem_destroy(&pool->start);
        rt_mem_free(pool);
        return false;
    }
    pool->count = started;

    CVM_LOG_INFO(m) {
        fmt_lit(&m.fmt, "GC pool: ");
        fmt_u32(&m.fmt, started);
        fmt_lit(&m.fmt, " worker thread(s)");
    }
    return true;
}

void cvm_pool_destroy(CvmHeap* heap) {
    CvmGcPool* pool = heap->pool;
    if (pool == NULL) {
        return;
    }

    atomic_store(&pool->shutdown, true);

    /* One post each, so every thread wakes to see the flag. */
    for (u32 i = 0; i < pool->count; i++) {
        rt_sem_post(&pool->start);
    }

    for (u32 i = 0; i < pool->count; i++) {
        rt_thread_join(pool->threads[i].handle);
    }

    for (u32 i = 0; i < pool->count; i++) {
        cvm_stats_close(pool->threads[i].stats);
        pool->threads[i].stats = NULL;
    }

    heap->pool = NULL;

    rt_sem_destroy(&pool->start);
    rt_mem_free(pool);
}

/* ------------------------------------------------------------------ */
/* waking                                                              */
/* ------------------------------------------------------------------ */

void cvm_pool_wake(CvmGcPool* pool, CvmGcPhase phase, u32 to_wake) {
    if (pool == NULL || to_wake == 0) {
        return;
    }

    if (to_wake > pool->count) {
        to_wake = pool->count;
    }

    /*
     * The phase is set before the wake, so a thread that wakes sees the
     * phase it was woken for. The release ordering pairs with the acquire
     * fence in the worker loop.
     */
    atomic_store_explicit(&pool->phase, (u32)phase, memory_order_release);

    /*
     * A sweep starts every woken thread's progress at zero.
     *
     * It has to happen here rather than in the worker, because a thread
     * becomes `active` before it reads the phase, and coalescing takes its
     * bound from active threads: a worker woken for a new sweep and still
     * carrying last round's end-of-heap number would be seen as having
     * swept everything, which lets coalescing close a run the sweep has not
     * reached yet. Setting it before the post means no observer can see the
     * pair in the wrong order.
     */
    atomic_fetch_add_explicit(&pool->wake_pends, to_wake, memory_order_release);

    if (phase == CVM_GC_SWEEP) {
        /*
         * Every thread, not the first `to_wake`. A counting semaphore does
         * not choose who wakes: posting twice can release the same thread
         * twice while another sleeps on, which is the same behaviour that
         * made a worker record nothing across three collections in
         * test_stats_wiring. Choosing the first `to_wake` slots would reset
         * threads that stay asleep and leave the one that actually woke
         * carrying last round's end-of-heap bound — and cvm_sweep asserts
         * that a thread's bound is at or behind its claim, so the first
         * block of the new round trips it.
         *
         * Resetting a thread that is sweeping right now is safe in the
         * wrong-direction-only sense: it lowers that thread's bound, and a
         * low bound makes coalescing hold back rather than run ahead.
         */
        for (u32 i = 0; i < pool->count; i++) {
            atomic_store_explicit(&pool->threads[i].sweep_cursor_done, 0,
                                  memory_order_relaxed);
        }
    }

    for (u32 i = 0; i < to_wake; i++) {
        rt_sem_post(&pool->start);
    }
}

u32 cvm_pool_active_count(CvmGcPool* pool) {
    if (pool == NULL) {
        return 0;
    }
    u32 count = 0;
    for (u32 i = 0; i < pool->count; i++) {
        if (atomic_load_explicit(&pool->threads[i].active, memory_order_acquire)) {
            count++;
        }
    }
    return count;
}

bool cvm_pool_any_active(CvmGcPool* pool) {
    return cvm_pool_active_count(pool) > 0;
}

void cvm_pool_wait_idle(CvmGcPool* pool) {
    if (pool == NULL) {
        return;
    }
    /*
     * Waiting for idle rather than for a count to drain: a worker that has
     * finished its share clears `active` on its way back to the semaphore,
     * so this is the point at which no worker is touching the heap.
     * Proceeding earlier would mean a phase change landing in the middle
     * of a worker's batch.
     */
    while (cvm_pool_any_active(pool)) {
        rt_thread_yield();
    }
}

void cvm_pool_scale_markers(CvmGcPool* pool, u32 remaining_full_packets) {
    if (pool == NULL || remaining_full_packets <= CVM_MARK_SPAWN_THREADS_MIN_PACKETS) {
        return;
    }

    u32 wanted = remaining_full_packets / CVM_MARK_MIN_PACKETS_PER_THREAD;
    if (wanted > pool->count) {
        wanted = pool->count;
    }

    u32 active = cvm_pool_active_count(pool);
    if (wanted > active) {
        cvm_pool_wake(pool, (CvmGcPhase)atomic_load(&pool->phase), wanted - active);
    }
}
