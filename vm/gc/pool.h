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
 * phase needs them. A thread that wakes reads the phase and does that
 * phase's work, then stops as soon as there is none left — which is what
 * keeps an idle collection from burning a core.
 *
 * Every pool thread has the same role. The reference gives thread 0 a
 * separate "master" role, whose job is to notice that a phase is finished
 * and to marshal the others; but the thread that triggered the collection
 * is awake and has to wait for the phase anyway, so it can be the master.
 * That removes a semaphore, a thread role and a branch, and it means the
 * marshalling happens on the thread that has the most context for it.
 *
 * ---------------------------------------------------------------------
 * Why a semaphore rather than a condition variable
 * ---------------------------------------------------------------------
 *
 * The wake count matters. `cvm_pool_wake(pool, 3)` means "three more
 * threads should run", not "something changed": a condition variable
 * would need the count kept separately and would wake everyone to have
 * all but three go back to sleep. It is built on a mutex and a condition
 * variable inside rt, because POSIX named semaphores bring a global
 * namespace and a lifetime that outlives a killed process.
 *
 * ---------------------------------------------------------------------
 * GC workers are not mutators
 * ---------------------------------------------------------------------
 *
 * A worker never allocates and never owns a frame chain, so it does not
 * register with the thread registry and is never stopped by a safepoint.
 * That is deliberate: a thread that stops the world must not itself be
 * waiting to be stopped. It also means workers can run *while* the world
 * is stopped, which is exactly when their work is needed.
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
} CvmGcThread;

typedef struct CvmGcPool {
    CvmHeap* heap;
    u32 count;
    CvmGcThread threads[CVM_MAX_GC_WORKERS];

    _Atomic u32 phase;

    /* Counts of threads to run. See the note above. */
    RtSem start;

    /* Set to make every thread leave its loop and exit. */
    _Atomic bool shutdown;
} CvmGcPool;

/*
 * Bring up `count` threads, or the default when count is zero. Clamped to
 * at least 1 and at most CVM_MAX_GC_WORKERS.
 *
 * The pool is stored in the heap, so the sweeper and the marker can see
 * the workers' progress.
 */
bool cvm_pool_init(CvmHeap* heap, u32 count);

/* Stop and join every thread, and detach the pool from the heap. */
void cvm_pool_destroy(CvmHeap* heap);

/* How many workers should exist, given the machine and the heap. */
u32 cvm_pool_default_count(CvmHeap const* heap);

/*
 * Set the phase and wake up to `to_wake` threads.
 *
 * Returns immediately: the caller does its own share of the work and then
 * waits for the workers to go idle, rather than blocking here.
 */
void cvm_pool_wake(CvmGcPool* pool, CvmGcPhase phase, u32 to_wake);

/* Threads currently doing work. */
u32 cvm_pool_active_count(CvmGcPool* pool);
bool cvm_pool_any_active(CvmGcPool* pool);

/* Wait until every worker has stopped. */
void cvm_pool_wait_idle(CvmGcPool* pool);

/*
 * Wake more workers when the marking backlog justifies it.
 *
 * Marking scales itself: one worker tracing a huge packet while others
 * idle is the worst case, so the number of full packets is the signal.
 * Over-waking costs a check, because a worker with no packet stops.
 */
void cvm_pool_scale_markers(CvmGcPool* pool, u32 remaining_full_packets);

#endif /* CVM_GC_POOL_H */
