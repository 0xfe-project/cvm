/*
 * tests/unit/test_safepoint.c
 *
 * Stop-the-world coordination.
 *
 * The protocol has three observable properties worth pinning: a collector
 * waits for a polling thread to park, a collector does not wait for an
 * unmanaged thread, and a thread that never polls is reported rather than
 * silently stalling the world. The last one is the difference between a
 * hang and a diagnosis, so it gets its own test even though it costs the
 * report interval in wall-clock time.
 *
 * Every thread in these tests attaches before it touches anything, and
 * detaches before it exits, because a registry entry for a dead thread
 * would make the next acquire wait for something that can never park.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <stdatomic.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "rt.h"
#include "safepoint.h"

/*
 * Start a test from a world that is definitely running.
 *
 * A test that fails between acquire and release leaves the stop flag set,
 * and every later test's threads then park inside attach and fail for a
 * reason that has nothing to do with what they are testing. One test
 * failing should not cascade. Releasing a stop-the-world that is not held
 * is harmless by construction.
 */
static void ensure_world_running(void) {
    cvm_safepoint_release();
}

/* A bounded spin that deliberately does NOT poll, used to model a thread
 * that is not reaching safepoints. */
static void spin_without_polling(u64 nanos) {
    u64 deadline = rt_time_nanos() + nanos;
    volatile u64 sink = 0;
    while (rt_time_nanos() < deadline) {
        sink++;
    }
}

/* ------------------------------------------------------------------ */
/* basic protocol                                                      */
/* ------------------------------------------------------------------ */

TEST init_is_idempotent(void) {
    ensure_world_running();

    cvm_safepoint_init();
    cvm_safepoint_init();
    cvm_safepoint_init();
    PASS();
}

TEST acquire_and_release_toggle_in_progress(void) {
    ensure_world_running();

    ASSERT(cvm_thread_attach());

    ASSERT_FALSE(cvm_safepoint_in_progress());
    ASSERT(cvm_safepoint_acquire());
    ASSERT(cvm_safepoint_in_progress());
    cvm_safepoint_release();
    ASSERT_FALSE(cvm_safepoint_in_progress());

    /* and it can be taken again */
    ASSERT(cvm_safepoint_acquire());
    cvm_safepoint_release();

    cvm_thread_detach();
    PASS();
}

TEST release_without_acquire_is_harmless(void) {
    ensure_world_running();

    ASSERT(cvm_thread_attach());
    cvm_safepoint_release();
    ASSERT_FALSE(cvm_safepoint_in_progress());
    cvm_thread_detach();
    PASS();
}

/* ------------------------------------------------------------------ */
/* a polling thread parks                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    _Atomic bool stop;
    _Atomic bool attached;
    _Atomic(CvmThreadNode*) node;
    _Atomic u32 polls;
} PollerArg;

static opaque_mut polls_until_stopped(opaque_mut arg) {
    PollerArg* a = arg;
    if (!cvm_thread_attach()) {
        return NULL;
    }
    atomic_store(&a->node, cvm_thread_node());
    atomic_store(&a->attached, true);

    while (!atomic_load(&a->stop)) {
        cvm_safepoint_poll();
        atomic_fetch_add(&a->polls, 1u);
        rt_thread_yield();
    }

    cvm_thread_detach();
    return NULL;
}

TEST acquire_waits_for_a_polling_thread(void) {
    ensure_world_running();

    ASSERT(cvm_thread_attach());

    PollerArg arg = {.stop = false, .attached = false, .node = NULL, .polls = 0};
    RtThread poller;
    ASSERT(rt_thread_create(&poller, polls_until_stopped, &arg));

    /* wait for the poller to register, bounded */
    u64 deadline = rt_time_nanos() + 2000000000ull;
    while (!atomic_load(&arg.attached) && rt_time_nanos() < deadline) {
        rt_thread_yield();
    }
    ASSERT(atomic_load(&arg.attached));

    u32 polls_before = atomic_load(&arg.polls);

    /* The acquire can only return once the poller has parked. */
    ASSERT(cvm_safepoint_acquire());

    CvmThreadNode* node = atomic_load(&arg.node);
    ASSERT(node != NULL);
    ASSERT(atomic_load(&node->parked));
    ASSERT_EQ_FMT((u32)CVM_THREAD_MANAGED, (u32)atomic_load(&node->state), "%u");

    /*
     * A parked thread is not running, so it makes no further progress.
     * Sampling twice after the acquire is the right shape: comparing
     * against a count taken before the acquire would be racy, because the
     * poller can legitimately complete another poll between that sample
     * and the moment the flag is set.
     */
    u32 parked_polls = atomic_load(&arg.polls);
    for (int i = 0; i < 100; i++) {
        rt_thread_yield();
    }
    ASSERT_EQ_FMT(parked_polls, atomic_load(&arg.polls), "%u");

    cvm_safepoint_release();

    /* and it resumes */
    deadline = rt_time_nanos() + 2000000000ull;
    while (atomic_load(&arg.polls) == polls_before && rt_time_nanos() < deadline) {
        rt_thread_yield();
    }
    ASSERT(atomic_load(&arg.polls) > polls_before);

    atomic_store(&arg.stop, true);
    ASSERT(rt_thread_join(poller));

    cvm_thread_detach();
    PASS();
}

/* ------------------------------------------------------------------ */
/* an unmanaged thread is not waited for                               */
/* ------------------------------------------------------------------ */

typedef struct {
    _Atomic bool ready;
    _Atomic bool go;
    _Atomic bool done;
} UnmanagedArg;

static opaque_mut sits_in_foreign_code(opaque_mut arg) {
    UnmanagedArg* a = arg;
    if (!cvm_thread_attach()) {
        return NULL;
    }

    cvm_thread_enter_unmanaged();
    atomic_store(&a->ready, true);

    /* Spin inside "foreign code": no polling, because a thread in this
     * state is not supposed to be looking at the heap at all. */
    while (!atomic_load(&a->go)) {
        rt_thread_yield();
    }

    cvm_thread_leave_unmanaged();
    atomic_store(&a->done, true);
    cvm_thread_detach();
    return NULL;
}

TEST unmanaged_thread_is_not_waited_for(void) {
    ensure_world_running();

    ASSERT(cvm_thread_attach());

    UnmanagedArg arg = {.ready = false, .go = false, .done = false};
    RtThread foreign;
    ASSERT(rt_thread_create(&foreign, sits_in_foreign_code, &arg));

    u64 deadline = rt_time_nanos() + 2000000000ull;
    while (!atomic_load(&arg.ready) && rt_time_nanos() < deadline) {
        rt_thread_yield();
    }
    ASSERT(atomic_load(&arg.ready));

    u32 reports_before = cvm_safepoint_stuck_reports();

    /* The foreign thread is unmanaged, so this must not wait for it. */
    ASSERT(cvm_safepoint_acquire());

    /* The proof that it did not wait: no diagnostic was emitted, so the
     * collector never considered the thread stuck. */
    ASSERT_EQ_FMT(reports_before, cvm_safepoint_stuck_reports(), "%u");

    /* and the thread really is unmanaged and not parked */
    cvm_threads_lock_read();
    bool saw_unmanaged = false;
    CVM_THREADS_FOREACH(node) {
        if ((u32)atomic_load(&node->state) == (u32)CVM_THREAD_UNMANAGED) {
            saw_unmanaged = true;
            ASSERT_FALSE(atomic_load(&node->parked));
        }
    }
    cvm_threads_unlock_read();
    ASSERT(saw_unmanaged);

    cvm_safepoint_release();

    atomic_store(&arg.go, true);
    ASSERT(rt_thread_join(foreign));

    cvm_thread_detach();
    PASS();
}

/* ------------------------------------------------------------------ */
/* a thread that never polls is reported                               */
/* ------------------------------------------------------------------ */

typedef struct {
    _Atomic bool acquired;
    _Atomic bool released;
} CollectorArg;

static opaque_mut collects_once(opaque_mut arg) {
    CollectorArg* a = arg;
    if (!cvm_thread_attach()) {
        return NULL;
    }
    atomic_store(&a->acquired, cvm_safepoint_acquire());
    cvm_safepoint_release();
    atomic_store(&a->released, true);
    cvm_thread_detach();
    return NULL;
}

TEST stuck_thread_is_reported(void) {
    ensure_world_running();

    ASSERT(cvm_thread_attach());

    CollectorArg arg = {.acquired = false, .released = false};
    RtThread collector;
    ASSERT(rt_thread_create(&collector, collects_once, &arg));

    u32 reports_before = cvm_safepoint_stuck_reports();

    /* Deliberately do not poll. The collector is waiting for us, and after
     * its report interval it must say so instead of stalling silently. */
    u64 deadline = rt_time_nanos() + 5000000000ull;
    while (cvm_safepoint_stuck_reports() == reports_before &&
           rt_time_nanos() < deadline) {
        spin_without_polling(1000000ull); /* 1ms at a time */
    }

    ASSERT(cvm_safepoint_stuck_reports() > reports_before);

    /* Now cooperate, so the collector can finish and the test can end. */
    while (!atomic_load(&arg.released)) {
        cvm_safepoint_poll();
        rt_thread_yield();
    }

    ASSERT(atomic_load(&arg.acquired));
    ASSERT(rt_thread_join(collector));

    cvm_thread_detach();
    PASS();
}

/* ------------------------------------------------------------------ */
/* a second collector is told to retry                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    _Atomic bool ready;
    _Atomic bool go;
    _Atomic bool got_it;
    _Atomic bool returned;
} SecondArg;

static opaque_mut tries_to_collect(opaque_mut arg) {
    SecondArg* a = arg;
    if (!cvm_thread_attach()) {
        return NULL;
    }

    /* Unmanaged while waiting, so the real collector does not wait for
     * this thread to park. */
    cvm_thread_enter_unmanaged();
    atomic_store(&a->ready, true);
    while (!atomic_load(&a->go)) {
        rt_thread_yield();
    }

    /* The collector holds the stop-the-world at this point, so this call
     * must wait for it to finish and then report that it did not collect:
     * the heap has just been collected, and retrying is the right move. */
    bool acquired = cvm_safepoint_acquire();
    atomic_store(&a->got_it, acquired);
    if (acquired) {
        cvm_safepoint_release();
    }
    atomic_store(&a->returned, true);

    cvm_thread_detach();
    return NULL;
}

TEST second_acquire_reports_false_while_held(void) {
    ensure_world_running();

    ASSERT(cvm_thread_attach());

    SecondArg arg = {.ready = false, .go = false, .got_it = false, .returned = false};
    RtThread second;
    ASSERT(rt_thread_create(&second, tries_to_collect, &arg));

    u64 deadline = rt_time_nanos() + 2000000000ull;
    while (!atomic_load(&arg.ready) && rt_time_nanos() < deadline) {
        rt_thread_yield();
    }
    ASSERT(atomic_load(&arg.ready));

    /* take it while the other thread is unmanaged, so this returns now */
    ASSERT(cvm_safepoint_acquire());
    ASSERT(cvm_safepoint_in_progress());

    /*
     * Let the other thread try, and wait until it is demonstrably inside
     * acquire's wait before releasing. Releasing first would let it become
     * the collector instead, which is correct behaviour but not the path
     * under test — and it would then wait for *this* thread to park, which
     * this thread never does.
     */
    atomic_store(&arg.go, true);

    u64 wait_deadline = rt_time_nanos() + 2000000000ull;
    while (cvm_safepoint_acquire_waiters() == 0 && rt_time_nanos() < wait_deadline) {
        rt_thread_yield();
    }
    ASSERT(cvm_safepoint_acquire_waiters() > 0);

    cvm_safepoint_release();

    deadline = rt_time_nanos() + 2000000000ull;
    while (!atomic_load(&arg.returned) && rt_time_nanos() < deadline) {
        rt_thread_yield();
    }
    ASSERT(atomic_load(&arg.returned));
    ASSERT_FALSE(atomic_load(&arg.got_it));

    ASSERT(rt_thread_join(second));

    cvm_thread_detach();
    PASS();
}

SUITE(safepoint_suite) {
    RUN_TEST(init_is_idempotent);
    RUN_TEST(acquire_and_release_toggle_in_progress);
    RUN_TEST(release_without_acquire_is_harmless);
    RUN_TEST(acquire_waits_for_a_polling_thread);
    RUN_TEST(unmanaged_thread_is_not_waited_for);
    RUN_TEST(stuck_thread_is_reported);
    RUN_TEST(second_acquire_reports_false_while_held);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    cvm_threads_init();
    cvm_safepoint_init();

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(safepoint_suite);
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
