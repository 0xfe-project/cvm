/*
 * tests/unit/test_worklist.c
 *
 * The work-stealing deque, on its own.
 *
 * The algorithm is a transcription of refs/chase-lev.cpp (see the note in
 * vm/gc/worklist.h for what is not transcribed and why), and these tests pin
 * the properties that transcription has to preserve: push then pop returns
 * the same packet, steal takes from the other end, and the last packet is
 * handed out exactly once however the owner and the thieves race for it.
 *
 * What these tests do not do is prove the memory orders. No test can, short
 * of a model checker; that is what the line-for-line transcription is for.
 */

#include "vendor/greatest.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "base/types.h"
#include "gc/grey_packet.h"
#include "gc/worklist.h"
#include "rt.h"

/*
 * Packets to queue. Real ones are owned by the heap; these are just
 * addresses with distinguishable identities, which is all the deque sees.
 */
static CvmGreyPacket packets[64];

static void wl_setup(CvmWorklist* w) {
    memset(packets, 0, sizeof(packets));
    if (!cvm_worklist_init(w, 8)) {
        /* A failed init is a failed test; greatest has no assert that is
         * legal outside a TEST body, so this is what that looks like. */
        fprintf(stderr, "worklist init failed\n");
        exit(1);
    }
}

static void wl_teardown(CvmWorklist* w) {
    cvm_worklist_destroy(w);
}

TEST push_then_pop_returns_the_same_packet(void) {
    CvmWorklist w;
    wl_setup(&w);

    cvm_worklist_push(&w, &packets[3]);
    ASSERT_EQ(&packets[3], cvm_worklist_pop(&w));
    ASSERT(NULL == cvm_worklist_pop(&w));

    wl_teardown(&w);
    PASS();
}

TEST push_then_steal_takes_it(void) {
    CvmWorklist w;
    wl_setup(&w);

    cvm_worklist_push(&w, &packets[1]);
    ASSERT_EQ(&packets[1], cvm_worklist_steal(&w));
    ASSERT(NULL == cvm_worklist_steal(&w));

    wl_teardown(&w);
    PASS();
}

TEST pop_takes_the_newest_and_steal_the_oldest(void) {
    CvmWorklist w;
    wl_setup(&w);

    cvm_worklist_push(&w, &packets[1]);
    cvm_worklist_push(&w, &packets[2]);
    cvm_worklist_push(&w, &packets[3]);

    /* LIFO from the owner's end... */
    ASSERT_EQ(&packets[3], cvm_worklist_pop(&w));
    /* ...FIFO from the thieves'. */
    ASSERT_EQ(&packets[1], cvm_worklist_steal(&w));
    /* And what is left is the middle. */
    ASSERT_EQ(&packets[2], cvm_worklist_pop(&w));
    ASSERT(NULL == cvm_worklist_pop(&w));
    ASSERT(NULL == cvm_worklist_steal(&w));

    wl_teardown(&w);
    PASS();
}

TEST draining_then_refilling_works(void) {
    CvmWorklist w;
    wl_setup(&w);

    for (int round = 0; round < 100; round++) {
        for (u32 i = 0; i < 8; i++) {
            cvm_worklist_push(&w, &packets[i]);
        }
        for (u32 i = 0; i < 8; i++) {
            ASSERT(NULL != cvm_worklist_pop(&w));
        }
        ASSERT(NULL == cvm_worklist_pop(&w));
    }

    wl_teardown(&w);
    PASS();
}

TEST filling_to_capacity_does_not_resize_or_lose(void) {
    CvmWorklist w;
    wl_setup(&w);

    /* Capacity 8, and eight packets: the ring is exactly full. The claim in
     * worklist.h is that it can never need to grow, and this is the
     * boundary of that claim. */
    for (u32 i = 0; i < 8; i++) {
        cvm_worklist_push(&w, &packets[i]);
    }
    u32 seen = 0;
    for (u32 i = 0; i < 8; i++) {
        if (cvm_worklist_pop(&w) != NULL) {
            seen++;
        }
    }
    ASSERT_EQ(8u, seen);

    wl_teardown(&w);
    PASS();
}

/*
 * A thief and an owner taking the last packet at once. Exactly one of them
 * gets it, and the count that comes out is the count that went in.
 */
typedef struct {
    CvmWorklist* w;
    _Atomic u32 got;
    _Atomic bool stop;
} RaceArg;

static opaque_mut thief_loop(opaque_mut raw) {
    RaceArg* arg = raw;
    while (!atomic_load(&arg->stop)) {
        if (cvm_worklist_steal(arg->w) != NULL) {
            atomic_fetch_add(&arg->got, 1u);
        } else {
            rt_thread_yield();
        }
    }
    return NULL;
}

TEST the_last_packet_is_taken_exactly_once(void) {
    CvmWorklist w;
    /* Capacity must cover the pool it serves: a packet is in one place at a
     * time, so a deque never holds more than the pool, and the pool here is
     * `many`. Initialized by hand rather than wl_setup because wl_setup's
     * capacity is 8 and this test's pool is 64. */
    memset(packets, 0, sizeof(packets));
    if (!cvm_worklist_init(&w, 64)) {
        fprintf(stderr, "worklist init failed\n");
        exit(1);
    }

    enum { N = 2000, CAP = 64 };
    static CvmGreyPacket many[CAP];
    memset(many, 0, sizeof(many));

    RaceArg arg = {.w = &w, .got = 0, .stop = false};
    RtThread thief;
    ASSERT(rt_thread_create(&thief, thief_loop, &arg));

    u32 owner_got = 0;
    u32 pushed = 0;
    while (owner_got + atomic_load(&arg.got) < N) {
        /*
         * Refill below half capacity, and never re-push an address that may
         * still be in the ring: the pool has CAP packets and the deque holds
         * fewer than that, so pushed % CAP is free exactly when the deque has
         * drained past it. Keeping occupancy under CAP/2 makes that certain
         * rather than likely: the owner pops in order, so by the time
         * (pushed % CAP) wraps, the slot it names was popped long before.
         */
        while (pushed < N && cvm_worklist_size(&w) < CAP / 2) {
            cvm_worklist_push(&w, &many[pushed % CAP]);
            pushed++;
        }
        if (cvm_worklist_pop(&w) != NULL) {
            owner_got++;
        }
    }

    ASSERT_EQ((u32)N, owner_got + atomic_load(&arg.got));
    atomic_store(&arg.stop, true);
    rt_thread_join(thief);

    cvm_worklist_destroy(&w);
    PASS();
}

SUITE(worklist_suite) {
    RUN_TEST(push_then_pop_returns_the_same_packet);
    RUN_TEST(push_then_steal_takes_it);
    RUN_TEST(pop_takes_the_newest_and_steal_the_oldest);
    RUN_TEST(draining_then_refilling_works);
    RUN_TEST(filling_to_capacity_does_not_resize_or_lose);
    RUN_TEST(the_last_packet_is_taken_exactly_once);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(worklist_suite);
    GREATEST_MAIN_END();
}
