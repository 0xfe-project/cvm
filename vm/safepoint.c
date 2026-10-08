/*
 * vm/safepoint.c
 *
 * Stop-the-world coordination. See vm/safepoint.h for the protocol.
 *
 * ---------------------------------------------------------------------
 * Lock order
 * ---------------------------------------------------------------------
 *
 * There are two locks in play: the registry's reader-writer lock
 * (vm/context.h) and this file's `stw_mutex`.
 *
 * The rule is: never take the registry lock while holding `stw_mutex`
 * from a path that also takes `stw_mutex` while holding the registry
 * lock. The collector takes stw_mutex and then the registry read lock,
 * which is the only nesting that happens. A registering thread takes the
 * registry write lock, releases it, and only then polls — so it never
 * holds the registry lock while waiting on stw_mutex.
 *
 * Getting this wrong would deadlock exactly when a new thread appears
 * during a collection, which is rare and unpleasant to reproduce.
 */

#include "safepoint.h"

#include <stdatomic.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "gc/heap.h"
#include "gc/world.h"
#include "log.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

/*
 * The flag generated code polls. Declared extern in gc.h because the poll
 * is inlined into generated code.
 */
_Atomic bool cvm_safepoint_requested = false;

static RtMutex stw_mutex;
static RtCond stw_cond;
static _Atomic bool stw_ready = false;
static _Atomic u32 stuck_reports = 0;

/* Identity of the thread that currently holds the world, or zero. */
static _Atomic(usize) stw_owner = 0;

/* Threads waiting for another collector to finish. */
static _Atomic u32 acquire_waiters = 0;

/*
 * How long the collector waits before reporting. Long enough that a
 * thread merely slow to reach a poll does not produce noise; short enough
 * that a genuine stall is reported while someone is still watching.
 */
#define STUCK_REPORT_INTERVAL_NS (200ull * 1000 * 1000)

void cvm_safepoint_init(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&stw_ready, &expected, true)) {
        return;
    }
    if (!rt_mutex_init(&stw_mutex) || !rt_cond_init(&stw_cond)) {
        rt_fatal(STR_LITERAL("could not create the safepoint synchroniser"));
    }
    atomic_store(&cvm_safepoint_requested, false);

    /*
     * Try to install the trap handler. A refusal is not fatal: polling
     * works everywhere, so a platform that cannot do traps collects with a
     * branch in the poll rather than not collecting.
     */
    cvm_safepoint_trap_init();
}

static bool ensure_ready(void) {
    if (atomic_load(&stw_ready)) {
        return true;
    }
    cvm_safepoint_init();
    return atomic_load(&stw_ready);
}

bool cvm_safepoint_in_progress(void) {
    return atomic_load_explicit(&cvm_safepoint_requested, memory_order_acquire);
}

/* ------------------------------------------------------------------ */
/* the collector's side                                                */
/* ------------------------------------------------------------------ */

/*
 * Whether every thread other than `self` is parked or can be ignored.
 *
 * A thread is ignorable when it is unmanaged (it cannot be touching the
 * heap) or dead (it will never park, and a collection that waited for it
 * would hang forever). Liveness is checked last because it costs a
 * syscall.
 *
 * Called with stw_mutex held.
 */
static bool all_others_parked(CvmThreadNode* self) {
    bool all = true;

    cvm_threads_lock_read();
    CVM_THREADS_FOREACH(node) {
        if (node == self) {
            continue;
        }
        if (atomic_load_explicit(&node->state, memory_order_acquire) ==
            (u32)CVM_THREAD_UNMANAGED) {
            continue;
        }
        if (atomic_load_explicit(&node->parked, memory_order_acquire)) {
            continue;
        }
        if (!cvm_thread_node_is_alive(node)) {
            continue;
        }
        all = false;
        break;
    }
    cvm_threads_unlock_read();

    return all;
}

/*
 * Say which threads are holding things up.
 *
 * Called with stw_mutex held. The report is the whole point of the
 * timeout: without it, a thread stuck in foreign code without having
 * entered unmanaged state shows up as a collection that never finishes,
 * with nothing in the log to say why.
 */
static void report_stuck(CvmThreadNode* self) {
    atomic_fetch_add(&stuck_reports, 1u);

    CVM_LOG_WARN(m) {
        fmt_lit(&m.fmt, "safepoint: thread(s) not parked after ");
        fmt_u64(&m.fmt, STUCK_REPORT_INTERVAL_NS / (1000ull * 1000ull));
        fmt_lit(&m.fmt, "ms:");
    }

    cvm_threads_lock_read();
    CVM_THREADS_FOREACH(node) {
        if (node == self) {
            continue;
        }
        if (atomic_load_explicit(&node->parked, memory_order_acquire)) {
            continue;
        }

        bool alive = cvm_thread_node_is_alive(node);
        u32 state = atomic_load_explicit(&node->state, memory_order_acquire);

        CVM_LOG_WARN(m) {
            fmt_lit(&m.fmt, "  thread ");
            fmt_u64(&m.fmt, node->id);
            fmt_lit(&m.fmt, " role=");
            fmt_str(&m.fmt, cvm_thread_role_name(node->role));
            fmt_lit(&m.fmt, " state=");
            fmt_lit(&m.fmt, state == (u32)CVM_THREAD_MANAGED ? "managed" : "unmanaged");
            fmt_lit(&m.fmt, " alive=");
            fmt_bool(&m.fmt, alive);
            fmt_lit(&m.fmt, " doing=");
            fmt_str(&m.fmt, cvm_thread_activity_name(cvm_thread_get_activity(node)));
            fmt_lit(&m.fmt, " sweeping=");
            fmt_bool(&m.fmt,
                     atomic_load_explicit(&node->sweeping, memory_order_acquire));
            fmt_lit(&m.fmt, " sweep_done=");
            fmt_u32(&m.fmt, atomic_load_explicit(&node->sweep_cursor_done,
                                                 memory_order_acquire));
        }
    }
    cvm_threads_unlock_read();

    /*
     * Where the sweep had got to, and who was holding what. A collector
     * waiting for a thread that is itself waiting for a lock cannot be
     * understood from the outside, and "which lock, held by whom" is the
     * first question worth answering. This is also why the owner of the
     * coalescing lock is recorded at all.
     */
    if (cvm_world_ready()) {
        CvmHeap* heap = &cvm_world()->heap;
        CVM_LOG_WARN(m) {
            fmt_lit(&m.fmt, "  sweep: cursor=");
            fmt_u32(&m.fmt,
                    atomic_load_explicit(&heap->sweep.cursor, memory_order_acquire));
            fmt_lit(&m.fmt, " limit=");
            fmt_u32(&m.fmt, heap->sweep.limit);
            fmt_lit(&m.fmt, " coalesce=");
            fmt_u32(&m.fmt, atomic_load_explicit(&heap->sweep.coalesce_done,
                                                 memory_order_acquire));
        }
        CVM_LOG_WARN(m) {
            fmt_lit(&m.fmt, "  coalesce_owner=");
            fmt_u64(&m.fmt, (u64)atomic_load(&heap->sweep.coalesce_owner));
            fmt_lit(&m.fmt, " acquire_waiters=");
            fmt_u32(&m.fmt, atomic_load(&acquire_waiters));
            fmt_lit(&m.fmt, " finishing=");
            fmt_bool(&m.fmt, atomic_load(&heap->sweep.finishing));
        }
    }

    CVM_LOG_WARN(m) {
        fmt_lit(&m.fmt, "  a thread inside a blocking foreign call must bracket it "
                        "with cvm_thread_enter_unmanaged/leave");
    }
}

bool cvm_safepoint_acquire(void) {
    if (!ensure_ready()) {
        return false;
    }

    CvmThreadNode* self = cvm_thread_node();

    rt_mutex_lock(&stw_mutex);

    if (atomic_load_explicit(&cvm_safepoint_requested, memory_order_acquire)) {
        /*
         * Another thread is collecting. Wait for it to finish and report
         * that we did not collect: by the time this returns the heap has
         * just been collected, so the caller should retry rather than
         * collect again.
         *
         * Waiting for another collection to end is indistinguishable, from
         * the collector's point of view, from being stopped by it: this
         * thread is not touching the heap and will not until the flag
         * clears. So it declares itself parked for the duration.
         *
         * Not declaring it is a deadlock, and a stress test with several
         * threads allocating found it. The incumbent collector waits for
         * every managed thread to park; this thread is not parked, it is
         * waiting; and it is waiting for the incumbent to finish. Both wait
         * forever, and the only sign is the collector's periodic "thread(s)
         * not parked" warning — which is what that diagnostic is for.
         */
        if (self != NULL) {
            atomic_store_explicit(&self->parked, true, memory_order_release);
        }
        cvm_thread_set_activity(CVM_ACT_WAITING_FOR_COLLECTION);
        atomic_fetch_add(&acquire_waiters, 1u);
        while (atomic_load_explicit(&cvm_safepoint_requested, memory_order_acquire)) {
            rt_cond_wait(&stw_cond, &stw_mutex);
        }
        atomic_fetch_sub(&acquire_waiters, 1u);
        if (self != NULL) {
            atomic_store_explicit(&self->parked, false, memory_order_release);
        }
        cvm_thread_set_activity(CVM_ACT_IDLE);

        rt_mutex_unlock(&stw_mutex);
        return false;
    }

    atomic_store_explicit(&cvm_safepoint_requested, true, memory_order_release);

    /*
     * With trap mode the flag alone is not enough: a thread that is polling
     * its trap cell never looks at it. Arming the pages is what makes those
     * polls fault. Both are done because a thread that has no trap page
     * still polls the flag.
     */
    cvm_safepoint_trap_arm_all();

    /* Wait for the others. cond_wait releases the mutex, which is what
     * lets a thread park while we are waiting. */
    while (!all_others_parked(self)) {
        if (!rt_cond_wait_timeout(&stw_cond, &stw_mutex, STUCK_REPORT_INTERVAL_NS)) {
            /*
             * Re-check before reporting. A thread that went unmanaged or
             * parked just as the interval expired would otherwise be
             * reported as stuck while it is in fact on its way — and a
             * diagnostic that races its own timeout is worse than none,
             * because it teaches the reader to ignore it.
             */
            if (!all_others_parked(self)) {
                report_stuck(self);
            }
        }
    }

    atomic_store_explicit(&stw_owner, (usize)self->id, memory_order_release);

    rt_mutex_unlock(&stw_mutex);
    return true;
}

void cvm_safepoint_release(void) {
    if (!atomic_load(&stw_ready)) {
        return;
    }

    atomic_store_explicit(&stw_owner, 0, memory_order_release);

    rt_mutex_lock(&stw_mutex);

    /* Disarm before clearing the flag: a thread that faulted and has not
     * yet parked must find its page readable when it retries the load. */
    cvm_safepoint_trap_disarm_all();

    atomic_store_explicit(&cvm_safepoint_requested, false, memory_order_release);
    rt_cond_broadcast(&stw_cond);
    rt_mutex_unlock(&stw_mutex);
}

void cvm_safepoint_arm_page_if_collecting(void) {
    if (!atomic_load(&stw_ready)) {
        return;
    }

    CvmThreadNode* node = cvm_thread_node();
    if (node == NULL || node->trap == NULL) {
        return;
    }

    rt_mutex_lock(&stw_mutex);
    if (atomic_load_explicit(&cvm_safepoint_requested, memory_order_acquire)) {
        rt_trap_page_arm(node->trap);
    }
    rt_mutex_unlock(&stw_mutex);
}

/* ------------------------------------------------------------------ */
/* the mutator's side                                                  */
/* ------------------------------------------------------------------ */

/*
 * Stop this thread for the collection that is running.
 *
 * Sets the parked flag, which is what the collecting thread waits on, and
 * is also what a thread waiting for someone else's collection sets — see
 * cvm_safepoint_acquire. The flag means "this thread is not touching the
 * heap and will not until the collection ends", which covers both.
 */
void cvm_safepoint_park(void) {
    CvmThreadNode* node = cvm_thread_node();
    if (node == NULL || !atomic_load(&stw_ready)) {
        return;
    }

    /*
     * The thread holding the world must never wait for the world to be
     * given back. In polling mode it cannot reach here, because the poll
     * is what it is doing; in trap mode it could, if it happened to touch
     * an armed page — its own, or one it had just armed. Returning
     * immediately is both the correct answer and the one that keeps a
     * mistake from becoming a deadlock with no output.
     */
    if (atomic_load_explicit(&stw_owner, memory_order_acquire) == (usize)node->id) {
        return;
    }

    rt_mutex_lock(&stw_mutex);

    /* Re-check under the lock: the collection may have ended between the
     * poll's load and getting here, and parking then would mean waiting
     * for a release that already happened. */
    if (!atomic_load_explicit(&cvm_safepoint_requested, memory_order_acquire)) {
        rt_mutex_unlock(&stw_mutex);
        return;
    }

    atomic_store_explicit(&node->parked, true, memory_order_release);
    rt_cond_broadcast(&stw_cond); /* wake the collector, which is waiting */

    while (atomic_load_explicit(&cvm_safepoint_requested, memory_order_acquire)) {
        rt_cond_wait(&stw_cond, &stw_mutex);
    }

    atomic_store_explicit(&node->parked, false, memory_order_release);
    rt_mutex_unlock(&stw_mutex);
}

void cvm_safepoint_notify_thread_changed(void) {
    if (!atomic_load(&stw_ready)) {
        return;
    }
    rt_mutex_lock(&stw_mutex);
    rt_cond_broadcast(&stw_cond);
    rt_mutex_unlock(&stw_mutex);
}

u32 cvm_safepoint_stuck_reports(void) {
    return atomic_load(&stuck_reports);
}

u32 cvm_safepoint_acquire_waiters(void) {
    return atomic_load(&acquire_waiters);
}
