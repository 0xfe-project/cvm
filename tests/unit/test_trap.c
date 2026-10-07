/*
 * tests/unit/test_trap.c
 *
 * Trap-based safepoints.
 *
 * The mechanism is worth testing at its own level rather than only through
 * a collection, because when it goes wrong it goes wrong quietly: a thread
 * that is not stopped keeps touching a heap that is being marked, and the
 * symptom appears somewhere else entirely.
 *
 * The mechanism is chosen at build time, so this file is written to say
 * which mode it ran in rather than to fail when trap mode is off. The
 * sanitizer configurations run with polling, and the plain ones with traps,
 * so both modes are exercised by the suite as a whole.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <stdatomic.h>
#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"
#include "stats.h"

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)32 * 1024 * 1024)

#if defined(CVM_SAFEPOINT_TRAPS)
/* Only the allocation-loop test allocates, and it is trap-mode only. */
typedef struct Leaf {
    u64 canary;
} Leaf;

static CvmTypeDescriptor const leaf_type = {
    .size = sizeof(Leaf),
    .kind = CVM_TYPE_SCALAR,
};
#endif

#if defined(CVM_SAFEPOINT_TRAPS)
/* State shared with the helper threads, which have no closure to carry it
 * in: a thread entry point takes one void pointer and returns one. */
static _Atomic(usize) captured_cell = 0;
static _Atomic bool stop_flag = false;
static _Atomic u32 loop_count = 0;

static opaque_mut capture_cell(opaque_mut arg);
static opaque_mut spin_polling(opaque_mut arg);
static opaque_mut allocate_in_a_loop(opaque_mut arg);
#endif

static void fixture_enter(void) {
    (void)cvm_thread_attach();
}

static void fixture_leave(void) {
    cvm_thread_detach();
}

/* ------------------------------------------------------------------ */
/* which mode is live                                                  */
/* ------------------------------------------------------------------ */

TEST the_mode_is_reported(void) {
#if defined(CVM_SAFEPOINT_TRAPS)
    /*
     * Trap mode was compiled in, so a page had to be obtainable and the
     * handler installed. If either failed the build would have fallen back
     * and cvm_thread_attach would have refused to register a thread.
     */
    ASSERT(cvm_safepoint_trap_usable());
    fprintf(stderr, "test_trap: running with trap-based safepoints\n");
#else
    ASSERT_FALSE(cvm_safepoint_trap_usable());
    fprintf(stderr, "test_trap: running with polling safepoints\n");
#endif

    /* Either way the world can be taken. */
    fixture_enter();
    ASSERT(cvm_safepoint_acquire());
    cvm_safepoint_release();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* the page itself                                                     */
/* ------------------------------------------------------------------ */

TEST an_attached_thread_has_its_own_page(void) {
    fixture_enter();

    CvmThreadNode* node = cvm_thread_node();
    ASSERT(node != NULL);

#if defined(CVM_SAFEPOINT_TRAPS)
    /*
     * A page of its own, not a shared one: arming has to be per thread, or
     * stopping one thread would stop every thread.
     */
    ASSERT(node->trap != NULL);
    ASSERT(rt_trap_page_cell(node->trap) != NULL);
    ASSERT(cvm_tls_trap_cell == rt_trap_page_cell(node->trap));
#else
    ASSERT(node->trap == NULL);
#endif

    fixture_leave();
    PASS();
}

TEST two_threads_have_different_pages(void) {
#if defined(CVM_SAFEPOINT_TRAPS)
    fixture_enter();
    void* main_cell = cvm_tls_trap_cell;

    RtThread thread;
    ASSERT(rt_thread_create(&thread, capture_cell, NULL));
    ASSERT(rt_thread_join(thread));

    ASSERT(main_cell != NULL);
    ASSERT(atomic_load(&captured_cell) != 0);
    ASSERT((void*)atomic_load(&captured_cell) != main_cell);

    fixture_leave();
#else
    ASSERT(true);
#endif
    PASS();
}

/* ------------------------------------------------------------------ */
/* arming, and the fault it causes                                     */
/* ------------------------------------------------------------------ */

#if defined(CVM_SAFEPOINT_TRAPS)

static _Atomic bool spinner_attached = false;
static _Atomic u32 spinner_polls = 0;

opaque_mut capture_cell(opaque_mut arg) {
    (void)arg;
    if (cvm_thread_attach()) {
        /* Published to a file-scope slot because the thread's own storage
         * is gone by the time it is joined. */
        atomic_store(&captured_cell, (usize)cvm_tls_trap_cell);
        cvm_thread_detach();
    }
    return NULL;
}

opaque_mut spin_polling(opaque_mut arg) {
    (void)arg;
    if (!cvm_thread_attach()) {
        return NULL;
    }
    atomic_store(&spinner_attached, true);

    while (!atomic_load(&stop_flag)) {
        /* The poll itself: a load through this thread's trap cell, which
         * faults while a collection has the page armed. */
        cvm_safepoint_poll();
        atomic_fetch_add(&spinner_polls, 1);
        rt_thread_yield();
    }

    cvm_thread_detach();
    return NULL;
}

TEST a_polling_thread_is_stopped_by_arming_its_page(void) {
    fixture_enter();

    atomic_store(&stop_flag, false);
    atomic_store(&spinner_attached, false);
    atomic_store(&spinner_polls, 0);

    RtThread thread;
    ASSERT(rt_thread_create(&thread, spin_polling, NULL));

    /* Wait for it to register, bounded. */
    u64 deadline = rt_time_nanos() + 2000000000ull;
    while (!atomic_load(&spinner_attached) && rt_time_nanos() < deadline) {
        rt_thread_yield();
    }
    ASSERT(atomic_load(&spinner_attached));

    /*
     * Taking the world arms every registered thread's page. The spinner is
     * in a poll loop, so it faults, the handler redirects it to the
     * trampoline, and it parks. This call returns only once it has.
     */
    ASSERT(cvm_safepoint_acquire());

    u32 parked_polls = atomic_load(&spinner_polls);
    for (int i = 0; i < 200; i++) {
        rt_thread_yield();
    }
    /* A parked thread makes no progress. */
    ASSERT_EQ_FMT(parked_polls, atomic_load(&spinner_polls), "%u");

    cvm_safepoint_release();

    /* And it resumes, which is what says the trampoline found its way back
     * to the load that faulted. */
    deadline = rt_time_nanos() + 2000000000ull;
    while (atomic_load(&spinner_polls) == parked_polls && rt_time_nanos() < deadline) {
        rt_thread_yield();
    }
    ASSERT(atomic_load(&spinner_polls) > parked_polls);

    atomic_store(&stop_flag, true);
    ASSERT(rt_thread_join(thread));

    fixture_leave();
    PASS();
}

TEST a_thread_faults_many_times_without_losing_its_place(void) {
    /*
     * The trampoline's job is to resume exactly where the fault happened.
     * Doing it once might work by luck; doing it repeatedly while the
     * thread is in the middle of real work is what says the saved program
     * counter is right.
     */
    fixture_enter();

    atomic_store(&stop_flag, false);
    atomic_store(&spinner_attached, false);
    atomic_store(&spinner_polls, 0);

    RtThread thread;
    ASSERT(rt_thread_create(&thread, spin_polling, NULL));

    u64 deadline = rt_time_nanos() + 2000000000ull;
    while (!atomic_load(&spinner_attached) && rt_time_nanos() < deadline) {
        rt_thread_yield();
    }
    ASSERT(atomic_load(&spinner_attached));

    for (int round = 0; round < 20; round++) {
        ASSERT(cvm_safepoint_acquire());
        cvm_safepoint_release();
    }

    atomic_store(&stop_flag, true);
    ASSERT(rt_thread_join(thread));

    /* It ran, it was stopped twenty times, and it never went off the rails:
     * a wrong resume address would have crashed it long before this. */
    ASSERT(atomic_load(&spinner_polls) > 0);

    fixture_leave();
    PASS();
}

TEST arming_stops_a_thread_that_is_allocating(void) {
    /* The mechanism is meant to stop real work, not just a poll loop. */
    fixture_enter();

    atomic_store(&stop_flag, false);
    atomic_store(&loop_count, 0);

    RtThread thread;
    ASSERT(rt_thread_create(&thread, allocate_in_a_loop, NULL));

    /* Let it get going. */
    for (int i = 0; i < 200; i++) {
        rt_thread_yield();
    }

    ASSERT(cvm_safepoint_acquire());
    u32 at_take = atomic_load(&loop_count);
    for (int i = 0; i < 200; i++) {
        rt_thread_yield();
    }
    ASSERT_EQ_FMT(at_take, atomic_load(&loop_count), "%u");
    cvm_safepoint_release();

    atomic_store(&stop_flag, true);
    ASSERT(rt_thread_join(thread));

    fixture_leave();
    PASS();
}

opaque_mut allocate_in_a_loop(opaque_mut arg) {
    (void)arg;
    if (!cvm_thread_attach()) {
        return NULL;
    }

    while (!atomic_load(&stop_flag)) {
        /* Frames poll, so this thread reaches safepoints the way generated
         * code does. */
        CVM_FRAME_DECL(f, 1);
        cvm_frame_push(&f_frame, f_roots, 1);
        f_roots[0] = NULL;

        for (int i = 0; i < 20; i++) {
            f_roots[0] = cvm_alloc(&leaf_type);
        }

        cvm_frame_pop(&f_frame);
        atomic_fetch_add(&loop_count, 1);
        rt_thread_yield();
    }

    cvm_thread_detach();
    return NULL;
}

#endif /* CVM_SAFEPOINT_TRAPS */

SUITE(trap_suite) {
    RUN_TEST(the_mode_is_reported);
    RUN_TEST(an_attached_thread_has_its_own_page);
    RUN_TEST(two_threads_have_different_pages);
#if defined(CVM_SAFEPOINT_TRAPS)
    RUN_TEST(a_polling_thread_is_stopped_by_arming_its_page);
    RUN_TEST(a_thread_faults_many_times_without_losing_its_place);
    RUN_TEST(arming_stops_a_thread_that_is_allocating);
#endif
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    cvm_threads_init();
    cvm_safepoint_init();
    if (!cvm_world_init(TEST_MIN, TEST_MAX)) {
        fprintf(stderr, "could not bring up the heap\n");
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(trap_suite);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}

#endif /* !_WIN32 */
