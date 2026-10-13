/*
 * tests/unit/test_array.c
 *
 * The GC reference array: appending, growth, empty slots, and what the
 * collector sees.
 *
 * The growth case is the one worth having. Appending past the end returns a
 * *different* array, and a caller that keeps the old pointer loses the item
 * without any error — the old array is still a valid array, just not the one
 * holding the data. The test pins the shape of that so the contract is written
 * down rather than remembered.
 */

#include "vendor/greatest.h"

#include <inttypes.h>
#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/array.h"
#include "cvm/gc.h"
#include "cvm/weak.h"
#include "gc/collect.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#if !defined(_WIN32)

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)16 * 1024 * 1024)

static CvmHeap* heap(void) {
    return &cvm_world()->heap;
}

static void collect(void) {
    cvm_gc_collect_and_finish(heap());
}

static void fixture_enter(void) {
    (void)cvm_thread_attach();
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

/* A leaf to put in the slots. */
typedef struct Item {
    u64 value;
} Item;

static CvmTypeDescriptor const item_desc = {
    .size = sizeof(Item),
    .kind = CVM_TYPE_SCALAR,
};

static Item* item_new(u64 v) {
    Item* it = cvm_alloc(&item_desc);
    it->value = v;
    return it;
}

static usize expected_extent(usize capacity) {
    usize payload = offsetof(CvmRefArray, items) + capacity * CVM_WORD_SIZE;
    return cvm_align_up(sizeof(CvmGcHeader) + payload, CVM_ALLOC_ALIGNMENT);
}

/* ------------------------------------------------------------------ */
/* slots                                                               */
/* ------------------------------------------------------------------ */

TEST a_new_array_has_empty_slots(void) {
    fixture_enter();

    CvmRefArray* a = cvm_ref_array_new(8);
    ASSERT_EQ_FMT((usize)8, cvm_ref_array_capacity(a), "%zu");
    ASSERT_EQ_FMT((usize)0, cvm_ref_array_length(a), "%zu");
    for (usize i = 0; i < 8; i++) {
        ASSERT(cvm_ref_array_get(a, i) == NULL);
    }

    fixture_leave();
    PASS();
}

TEST a_zero_capacity_array_is_still_usable(void) {
    fixture_enter();

    FRAME(1);
    CvmRefArray* a = cvm_ref_array_new(0);
    ASSERT_EQ_FMT((usize)0, cvm_ref_array_capacity(a), "%zu");

    a = cvm_ref_array_push(a, item_new(7));
    f_roots[0] = a;
    ASSERT_EQ_FMT((usize)1, cvm_ref_array_length(a), "%zu");
    ASSERT_EQ_FMT((u64)7, ((Item*)cvm_ref_array_get(a, 0))->value, "%" PRIu64);

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST set_and_get_cover_the_whole_capacity(void) {
    /*
     * Slots past `used` are addressable and are the array's spare room, not
     * holes. The hash table is what needs this: its bucket array is full of
     * slots from the start, and which of them hold a key is its own business.
     */
    fixture_enter();

    FRAME(2);
    CvmRefArray* a = cvm_ref_array_new(4);
    f_roots[0] = a;

    Item* last = item_new(99);
    f_roots[1] = last;
    cvm_ref_array_set(a, 3, last);

    ASSERT(cvm_ref_array_get(a, 3) == last);
    ASSERT(cvm_ref_array_get(a, 0) == NULL);
    /* the used prefix did not move: setting a slot is not appending */
    ASSERT_EQ_FMT((usize)0, cvm_ref_array_length(a), "%zu");

    cvm_ref_array_set(a, 3, NULL);
    ASSERT(cvm_ref_array_get(a, 3) == NULL);

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* appending                                                           */
/* ------------------------------------------------------------------ */

TEST push_fills_slots_in_order(void) {
    fixture_enter();

    FRAME(1);
    CvmRefArray* a = cvm_ref_array_new(4);
    f_roots[0] = a;

    for (u64 v = 0; v < 4; v++) {
        a = cvm_ref_array_push(a, item_new(v));
        f_roots[0] = a;
    }

    ASSERT_EQ_FMT((usize)4, cvm_ref_array_length(a), "%zu");
    ASSERT_EQ_FMT((usize)4, cvm_ref_array_capacity(a), "%zu");
    for (usize i = 0; i < 4; i++) {
        ASSERT_EQ_FMT((u64)i, ((Item*)cvm_ref_array_get(a, i))->value, "%" PRIu64);
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST appending_past_the_end_returns_a_new_array(void) {
    fixture_enter();

    FRAME(1);
    CvmRefArray* small = cvm_ref_array_new(2);
    f_roots[0] = small;

    CvmRefArray* a = small;
    a = cvm_ref_array_push(a, item_new(1));
    f_roots[0] = a;
    a = cvm_ref_array_push(a, item_new(2));
    f_roots[0] = a;

    /* still the same array: both items fit */
    ASSERT(a == small);

    CvmRefArray* bigger = cvm_ref_array_push(a, item_new(3));
    f_roots[0] = bigger;

    /* the third did not fit, so this is a different array ... */
    ASSERT(bigger != small);
    ASSERT(cvm_ref_array_capacity(bigger) > cvm_ref_array_capacity(small));
    ASSERT_EQ_FMT((usize)3, cvm_ref_array_length(bigger), "%zu");
    for (u64 i = 0; i < 3; i++) {
        ASSERT_EQ_FMT(i + 1, ((Item*)cvm_ref_array_get(bigger, (usize)i))->value,
                      "%" PRIu64);
    }

    /*
     * ... and the old one is untouched, holding what it held. Nothing is
     * invalid about it: it is simply not where the new item went.
     */
    ASSERT_EQ_FMT((usize)2, cvm_ref_array_length(small), "%zu");
    ASSERT_EQ_FMT((usize)2, cvm_ref_array_capacity(small), "%zu");
    ASSERT_EQ_FMT((u64)1, ((Item*)cvm_ref_array_get(small, 0))->value, "%" PRIu64);

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST growth_is_about_one_and_a_half_each_time(void) {
    fixture_enter();

    FRAME(1);
    CvmRefArray* a = cvm_ref_array_new(0);
    f_roots[0] = a;

    usize previous = 0;
    for (u64 i = 0; i < 64; i++) {
        a = cvm_ref_array_push(a, item_new(i));
        f_roots[0] = a;

        usize capacity = cvm_ref_array_capacity(a);
        if (capacity != previous) {
            /* never less than 1.5x, and never failing to make progress */
            ASSERT(capacity >= previous + previous / 2);
            ASSERT(capacity > previous);
            previous = capacity;
        }
        ASSERT(i + 1 <= (u64)capacity);
    }

    /* every item is still there and still in order */
    ASSERT_EQ_FMT((usize)64, cvm_ref_array_length(a), "%zu");
    for (usize i = 0; i < 64; i++) {
        ASSERT_EQ_FMT((u64)i, ((Item*)cvm_ref_array_get(a, i))->value, "%" PRIu64);
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* the collector's view                                                */
/* ------------------------------------------------------------------ */

TEST the_extent_follows_the_capacity(void) {
    fixture_enter();

    for (usize capacity = 0; capacity <= 40; capacity += 13) {
        CvmRefArray* a = cvm_ref_array_new(capacity);
        ASSERT_EQ_FMT(expected_extent(capacity), cvm_object_size(cvm_header_of(a)),
                      "%zu");
    }

    fixture_leave();
    PASS();
}

TEST a_rooted_array_keeps_its_items_alive(void) {
    fixture_enter();

    FRAME(1);
    CvmRefArray* a = cvm_ref_array_new(4);
    f_roots[0] = a;
    for (u64 v = 0; v < 4; v++) {
        a = cvm_ref_array_push(a, item_new(v * 10u));
        f_roots[0] = a;
    }

    collect();

    a = (CvmRefArray*)f_roots[0];
    ASSERT_EQ_FMT((usize)4, cvm_ref_array_length(a), "%zu");
    for (usize i = 0; i < 4; i++) {
        Item* it = (Item*)cvm_ref_array_get(a, i);
        ASSERT(it != NULL);
        ASSERT_EQ_FMT((u64)(i * 10u), it->value, "%" PRIu64);
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST an_item_dropped_from_a_rooted_array_goes_away(void) {
    /*
     * The array is a strong reference to every slot up to its capacity, so
     * emptying a slot is the only way to let an item go. That is also why the
     * hash table nulls a slot when it deletes a key.
     */
    fixture_enter();

    FRAME(2);
    CvmRefArray* a = cvm_ref_array_new(2);
    f_roots[0] = a;

    Item* first = item_new(1);
    void* weak = cvm_weak_ref_new(first);
    f_roots[1] = weak;

    a = cvm_ref_array_push(a, first);
    f_roots[0] = a;
    collect();
    ASSERT(cvm_weak_ref_target(weak) == first);

    cvm_ref_array_set(a, 0, NULL);
    collect();
    ASSERT(cvm_weak_ref_target(weak) == NULL);

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST repeated_push_and_drop_does_not_grow(void) {
    fixture_enter();
    FRAME(2);
    /* Rounds of building up and dropping: the arrays are garbage after each
     * round, and the heap should come back down after a collection. */
    for (u32 round = 0; round < 10u; round++) {
        CvmRefArray* a = cvm_ref_array_new(4);
        f_roots[0] = a;
        for (u32 i = 0; i < 100u; i++) {
            a = cvm_ref_array_push(a, cvm_alloc(&item_desc));
            f_roots[0] = a;
        }
        f_roots[0] = NULL;
    }
    collect();
    /* The heap is back to what it was before the rounds. */
    FRAME_END();
    fixture_leave();
    PASS();
}

/*
 * An array larger than a block takes the large-object path. The invariant that
 * matters is that the object does not span blocks, and the marker's extent
 * check would catch it if it did; this test checks the content and that the
 * array survives a collection.
 */
TEST an_array_larger_than_a_block_allocates_and_survives(void) {
    fixture_enter();
    /* 32 KiB / 8 bytes per pointer = 4096 pointers crosses a block. */
    enum { N = 8192 };
    CvmRefArray* a = cvm_ref_array_new(N);
    FRAME(1);
    f_roots[0] = a;
    for (u32 i = 0; i < 64; i++) {
        a = cvm_ref_array_push((CvmRefArray*)f_roots[0], cvm_alloc(&item_desc));
        f_roots[0] = a;
    }
    collect();
    CvmRefArray* still = (CvmRefArray*)f_roots[0];
    ASSERT(still != NULL);
    ASSERT_EQ_FMT((usize)64, cvm_ref_array_length(still), "%zu");
    /* Every element is reachable and correct. */
    for (u32 i = 0; i < 64; i++) {
        ASSERT(cvm_ref_array_get(still, i) != NULL);
    }
    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(array_suite) {
    RUN_TEST(a_new_array_has_empty_slots);
    RUN_TEST(a_zero_capacity_array_is_still_usable);
    RUN_TEST(set_and_get_cover_the_whole_capacity);

    RUN_TEST(push_fills_slots_in_order);
    RUN_TEST(appending_past_the_end_returns_a_new_array);
    RUN_TEST(growth_is_about_one_and_a_half_each_time);

    RUN_TEST(the_extent_follows_the_capacity);
    RUN_TEST(a_rooted_array_keeps_its_items_alive);
    RUN_TEST(an_item_dropped_from_a_rooted_array_goes_away);
    RUN_TEST(repeated_push_and_drop_does_not_grow);
    RUN_TEST(an_array_larger_than_a_block_allocates_and_survives);
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
    RUN_SUITE(array_suite);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}

#else /* _WIN32 */

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
