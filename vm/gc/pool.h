/*
 * vm/gc/pool.h
 *
 * The concurrent GC worker pool, from refs/commix/GCThread.{h,c} and
 * Phase.h.
 *
 * ---------------------------------------------------------------------
 * Shape
 * ---------------------------------------------------------------------
 *
 * A fixed set of threads, created once, that sleep on a semaphore until a
 * phase needs them. Thread 0 is the master: it is the one that finishes a
 * phase off (completes coalescing, closes out the sweep) and decides when
 * to wake more workers. The rest are workers.
 *
 * Waking a thread means incrementing the semaphore and setting the phase.
 * A worker that wakes reads the phase and does that phase's work; when it
 * runs out of work it goes back to sleep on its own, which is what keeps
 * an idle collection from burning a core.
 *
 * ---------------------------------------------------------------------
 * Why a semaphore rather than a condition variable
 * ---------------------------------------------------------------------
 *
 * The wake count matters. `cvm_pool_wake(pool, 3)` means "three more
 * threads should run", not "something changed": a condition variable
 * would need the count kept separately and would wake everyone to have
 * all but three go back to sleep. The reference uses a semaphore for the
 * same reason. It is built on a mutex and a condition variable inside rt
 * (see rt_sem_*), because POSIX named semaphores bring a global namespace
 * and a lifetime that outlives a killed process.
 *
 * ---------------------------------------------------------------------
 * GC workers are not mutators
 * ---------------------------------------------------------------------
 *
 * A worker never allocates and never touches a frame chain, so it does not
 * register with the thread registry and is not stopped by a safepoint.
 * That is deliberate: a thread that stops the world must not itself be
 * waiting to be stopped.
 *
 * What a worker does need is a place to put sweep progress, so that
 * coalescing knows how far it may advance. That is `sweep_cursor_done`
 * here, and cvm_sweeper_min_cursor reads it.
 */

#ifndef CVM_GC_POOL_H
#define CVM_GC_POOL_H

#include <stdatomic.h>

#include "base/types.h"
#include "gc/heap.h"
#include "rt.h"
#include "stats.h"

/* Upper bound on workers, so the array needs no allocation. */
#define CVM_MAX_GC_WORKERS 16

/* Phases a worker can be woken into. */
typedef enum CvmGcPhase {
    CVM_GC_IDLE = 0x0,
    CVM_GC_MARK = 0x1,
    CVM_GC_NULLIFY = 0x2,
    CVM_GC_SWEEP = 0x3,
} CvmGcPhase;

typedef struct CvmGcThread {
    u32 id;
    struct CvmGcPool* pool;

    /* Set while this thread is doing work, cleared when it sleeps. */
    _Atomic bool active;

    /* How far this thread has swept, for the coalescing bound. */
    _Atomic u32 sweep_cursor_done;

    RtThread handle;
    CvmStats stats;
} CvmGcThread;

typedef struct CvmGcPool {
    CvmHeap* heap;
    u32 count;
    CvmGcThread threads[CVM_MAX_GC_WORKERS];

    _Atomic u32 phase;

    /* Counts of threads to run. See the note above. */
    RtSem start_workers;
    RtSem start_master;

    /* Set to make every thread leave its loop and exit. */
    _Atomic bool shutdown;
} CvmGcPool;

/*
 * Bring up `count` threads (clamped to at least 1 and at most
 * CVM_MAX_GC_WORKERS). Thread 0 is the master.
 *
 * The pool is stored in the heap, so the sweeper and the marker can see
 * the workers' progress.
 */
bool cvm_pool_init(CvmHeap* heap, u32 count);

/* Stop and join every thread, and detach the pool from the heap. */
void cvm_pool_destroy(CvmHeap* heap);

/* How many workers should exist, given the machine and the heap size. */
u32 cvm_pool_default_count(CvmHeap const* heap);

/* Set the phase and wake `to_wake` threads (thread 0 among them). */
void cvm_pool_wake(CvmGcPool* pool, u32 to_wake);

void cvm_pool_wake_master(CvmGcPool* pool);
void cvm_pool_wake_workers(CvmGcPool* pool, u32 to_wake);

/* Threads currently doing work. */
u32 cvm_pool_active_count(CvmGcPool* pool);
bool cvm_pool_any_active(CvmGcPool* pool);

/*
 * Wake more workers when the marking backlog justifies it.
 *
 * Marking scales itself: one worker tracing a huge packet while others
 * idle is the worst case, so the number of full packets is the signal.
 */
void cvm_pool_scale_markers(CvmGcPool* pool, u32 remaining_full_packets);

/* Workers whose sweep progress is at or beyond `index` are done. */
bool cvm_pool_all_sweepers_past(CvmGcPool* pool, u32 index);

#endif /* CVM_GC_POOL_H */
