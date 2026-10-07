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

static void do_mark(CvmHeap* heap) {
    /* Take packets until there are none left, then stop. The collector
     * will wake this thread again if the backlog grows. */
    cvm_mark(heap, false);
}

static void do_nullify(CvmHeap* heap) {
    /* Nullifying reads marks and writes to weak slots; it is idempotent,
     * so draining whatever is there is enough. */
    (void)cvm_weak_nullify(heap);
}

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

        switch ((CvmGcPhase)atomic_load(&pool->phase)) {
        case CVM_GC_IDLE:
            break;
        case CVM_GC_MARK:
            do_mark(heap);
            break;
        case CVM_GC_NULLIFY:
            do_nullify(heap);
            break;
        case CVM_GC_SWEEP:
            thread->sweep_cursor_done = 0;
            do_sweep(thread, heap);
            thread->sweep_cursor_done = heap->sweep.limit;
            break;
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

    if (!rt_sem_init(&pool->start, 0)) {
        rt_mem_free(pool);
        return false;
    }

    for (u32 i = 0; i < count; i++) {
        CvmGcThread* thread = &pool->threads[i];
        thread->id = i;
        thread->pool = pool;
        atomic_store(&thread->active, false);
        atomic_store(&thread->sweep_cursor_done, 0);
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
