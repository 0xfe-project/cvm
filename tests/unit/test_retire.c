/*
 * tests/unit/test_retire.c
 *
 * The retire list's grace period, on its own.
 *
 * The observable is the sweeper's clearing: memory it reclaims is zeroed
 * before it can be handed out again, so an object's canary going from intact
 * to zero is the moment its memory was reclaimed. That makes both halves of
 * the promise falsifiable:
 *
 *   the object survives the collection after its retire
 *   (canary intact -- had nothing kept it, that collection's sweep would
 *   have taken it);
 *   it does not survive the one after that
 *   (canary zeroed -- the grace period ended and nothing else was holding it).
 *
 * The second half is also what distinguishes this from a leak: a mechanism
 * that kept everything forever would pass every survival test and fail this
 * one.
 */

#include "vendor/greatest.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "cvm/retire.h"
#include "cvm/vm.h"
#include "gc/collect.h"
#include "gc/world.h"
#include "safepoint.h"

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)32 * 1024 * 1024)

typedef struct Leaf {
    struct Leaf* next;
    u64 canary;
} Leaf;

static CvmPtrField const leaf_fields[] = {
    {.offset = offsetof(Leaf, next)},
};

static CvmTypeDescriptor const leaf_desc = {
    .size = sizeof(Leaf),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 1,
    .ptr_fields = leaf_fields,
};

static void fixture_enter(void) {
    (void)cvm_thread_attach();
}

static CvmHeap* heap(void) {
    return &cvm_world()->heap;
}

static void collect(void) {
    cvm_gc_collect_and_finish(heap());
}

static void fixture_leave(void) {
    cvm_thread_detach();
}

#define FRAME(slots)                                                                   \
    CVM_FRAME_DECL(f, slots);                                                          \
    cvm_frame_push(&f_frame, f_roots, slots);                                          \
    for (u32 i = 0; i < (slots); i++) {                                                \
        f_roots[i] = NULL;                                                             \
    }

#define FRAME_END() cvm_frame_pop(&f_frame)

static void collect_until_stable(void) {
    /* Two collections: the grace period is one full collection, so what a
     * retire kept alive is reclaimed by the second. */
    collect();
    collect();
}

TEST a_retired_object_survives_the_next_collection_and_not_the_one_after(void) {
    fixture_enter();
    FRAME(2);

    Leaf* leaf = cvm_alloc(&leaf_desc);
    leaf->next = NULL;
    leaf->canary = 0xC0FFEE;
    f_roots[0] = leaf;

    /* Drop the only reference and retire it, as a replaced array would be. */
    f_roots[0] = NULL;
    cvm_retire(leaf);

    /* One collection: the grace period holds. */
    collect();
    ASSERT_EQ_FMT((u64)0xC0FFEE, leaf->canary, "%llu");

    /* And the one after: nothing holds it, and the memory is reclaimed --
     * zeroed on its way back out, which is what makes this a window rather
     * than a hope. */
    collect();
    ASSERT_EQ_FMT((u64)0, leaf->canary, "%llu");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST retiring_twice_collects_nothing_extra_and_still_ends(void) {
    fixture_enter();
    FRAME(2);

    Leaf* a = cvm_alloc(&leaf_desc);
    a->next = NULL;
    a->canary = 0xAAAA;
    Leaf* b = cvm_alloc(&leaf_desc);
    b->next = NULL;
    b->canary = 0xBBBB;

    cvm_retire(a);
    cvm_retire(a); /* the same object twice: the list is a list, not a set */
    cvm_retire(b);

    collect();
    ASSERT_EQ_FMT((u64)0xAAAA, a->canary, "%llu");
    ASSERT_EQ_FMT((u64)0xBBBB, b->canary, "%llu");

    collect_until_stable();
    ASSERT_EQ_FMT((u64)0, a->canary, "%llu");
    ASSERT_EQ_FMT((u64)0, b->canary, "%llu");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_retired_object_still_reaches_what_it_points_to(void) {
    /* The grey half of the promise: what the retired object references has to
     * stay valid too, because a lock-free reader walks *through* it. */
    fixture_enter();
    FRAME(2);

    Leaf* inner = cvm_alloc(&leaf_desc);
    inner->next = NULL;
    inner->canary = 0x1234;
    Leaf* outer = cvm_alloc(&leaf_desc);
    outer->next = inner;
    outer->canary = 0x5678;

    /* The only reference to inner is through outer. Retire outer -- as the
     * map retires a key array whose slots point at wrappers. */
    cvm_retire(outer);

    collect();
    ASSERT_EQ_FMT((u64)0x5678, outer->canary, "%llu");
    /* inner was reachable only from a retired object, and the grace period
     * kept the whole subgraph: a reader walking outer's fields finds inner
     * intact. */
    ASSERT(outer->next == inner);
    ASSERT_EQ_FMT((u64)0x1234, inner->canary, "%llu");

    collect_until_stable();
    ASSERT_EQ_FMT((u64)0, outer->canary, "%llu");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST retire_accepts_null_and_an_unready_world(void) {
    /* NULL is the callers' normal "nothing was there before" case, and it
     * must not disturb the lists. */
    cvm_retire(NULL);
    PASS();
}

TEST retirement_across_many_collections_does_not_accumulate(void) {
    /* One retire per round, many rounds: the lists must hand their entries
     * over and drop them, or the heap fills with grace periods that never
     * end. */
    fixture_enter();
    FRAME(2);

    for (u32 round = 0; round < 64; round++) {
        Leaf* leaf = cvm_alloc(&leaf_desc);
        leaf->next = NULL;
        leaf->canary = 0xDEAD0000 + round;
        cvm_retire(leaf);

        collect();
        /* Two rounds in, the round-before-last's retire must be gone. */
        if (round >= 2) {
            collect(); /* this collection reclaims round-2's retire */
        }
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(retire_suite) {
    RUN_TEST(a_retired_object_survives_the_next_collection_and_not_the_one_after);
    RUN_TEST(retiring_twice_collects_nothing_extra_and_still_ends);
    RUN_TEST(a_retired_object_still_reaches_what_it_points_to);
    RUN_TEST(retire_accepts_null_and_an_unready_world);
    RUN_TEST(retirement_across_many_collections_does_not_accumulate);
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
    RUN_SUITE(retire_suite);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}
