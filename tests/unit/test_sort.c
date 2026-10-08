/*
 * tests/unit/test_sort.c
 *
 * The sort: the order it produces, the context it passes, and the element sizes
 * that pick different swap routines.
 *
 * The property worth testing is not "it sorts" but "it sorts the same thing a
 * sort that is obviously correct would produce" — the random case below compares
 * against an insertion sort over the same input, which is where a quicksort's
 * partition arithmetic gets caught.
 */

#include "vendor/greatest.h"

#include <stdio.h>
#include <string.h>

#include "base/sort.h"
#include "base/types.h"

/* ------------------------------------------------------------------ */
/* comparators                                                         */
/* ------------------------------------------------------------------ */

static i32 cmp_i32(opaque a, opaque b, opaque_mut ctx) {
    i32 x = *(i32 const*)a;
    i32 y = *(i32 const*)b;
    bool descending = (ctx != NULL) && (*(bool const*)ctx);
    i32 d = (x < y) ? -1 : ((x > y) ? 1 : 0);
    return descending ? -d : d;
}

typedef struct Record {
    i32 key;
    u8 payload[300]; /* large enough to swap as a block rather than a word */
} Record;

static i32 cmp_record(opaque a, opaque b, opaque_mut ctx) {
    CVM_UNUSED(ctx);
    i32 x = ((Record const*)a)->key;
    i32 y = ((Record const*)b)->key;
    return (x < y) ? -1 : ((x > y) ? 1 : 0);
}

/* ------------------------------------------------------------------ */
/* ordering                                                            */
/* ------------------------------------------------------------------ */

TEST sorts_in_the_order_the_comparator_asks_for(void) {
    i32 values[] = {5, 1, 4, 2, 8, 3, 9, 0, 7, 6};

    cvm_rqsort(values, 10, sizeof(values[0]), cmp_i32, NULL);
    for (i32 i = 0; i < 10; i++) {
        ASSERT_EQ_FMT(i, values[i], "%d");
    }

    bool descending = true;
    cvm_rqsort(values, 10, sizeof(values[0]), cmp_i32, &descending);
    for (i32 i = 0; i < 10; i++) {
        ASSERT_EQ_FMT(9 - i, values[i], "%d");
    }
    PASS();
}

TEST the_edges_do_nothing_harmful(void) {
    /* nothing to sort */
    cvm_rqsort(NULL, 0, 4, cmp_i32, NULL);

    i32 one[] = {7};
    cvm_rqsort(one, 1, sizeof(one[0]), cmp_i32, NULL);
    ASSERT_EQ_FMT(7, one[0], "%d");

    i32 two_unordered[] = {2, 1};
    cvm_rqsort(two_unordered, 2, sizeof(two_unordered[0]), cmp_i32, NULL);
    ASSERT_EQ_FMT(1, two_unordered[0], "%d");
    ASSERT_EQ_FMT(2, two_unordered[1], "%d");

    i32 two_equal[] = {3, 3};
    cvm_rqsort(two_equal, 2, sizeof(two_equal[0]), cmp_i32, NULL);
    ASSERT_EQ_FMT(3, two_equal[0], "%d");
    ASSERT_EQ_FMT(3, two_equal[1], "%d");
    PASS();
}

TEST large_elements_are_swapped_whole(void) {
    /*
     * An element bigger than the block threshold takes a different swap routine,
     * and getting it wrong moves part of a record — which shows up as a payload
     * that no longer matches its key, so each record carries a payload derived
     * from its key and the test checks every one.
     */
    enum { N = 20 };
    Record records[N];

    /* a deterministic shuffle, so the test is the same on every run */
    u32 state = 12345u;
    for (i32 i = 0; i < N; i++) {
        state = state * 1103515245u + 12345u;
        records[i].key = (i32)((state >> 16) % (u32)N);
        memset(records[i].payload, (int)(u8)records[i].key, sizeof(records[i].payload));
    }

    cvm_rqsort(records, N, sizeof(records[0]), cmp_record, NULL);

    for (i32 i = 0; i < N; i++) {
        if (i > 0) {
            ASSERT(records[i - 1].key <= records[i].key);
        }
        for (usize j = 0; j < sizeof(records[0].payload); j++) {
            if (records[i].payload[j] != (u8)records[i].key) {
                FAILm("a record's payload does not belong to its key");
            }
        }
    }
    PASS();
}

/* ------------------------------------------------------------------ */
/* against something obviously correct                                 */
/* ------------------------------------------------------------------ */

static void insertion_sort(i32* values, usize n) {
    for (usize i = 1; i < n; i++) {
        i32 v = values[i];
        usize j = i;
        while (j > 0u && values[j - 1u] > v) {
            values[j] = values[j - 1u];
            j--;
        }
        values[j] = v;
    }
}

TEST matches_an_insertion_sort_on_random_input(void) {
    enum { N = 500 };
    i32 mine[N];
    i32 reference[N];

    u32 state = 987654321u;
    for (usize i = 0; i < N; i++) {
        state = state * 1103515245u + 12345u;
        /* a small range on purpose: duplicates and near-sorted runs are where a
         * partition scheme goes wrong */
        mine[i] = (i32)((state >> 16) % 37u) - 18;
        reference[i] = mine[i];
    }

    cvm_rqsort(mine, N, sizeof(mine[0]), cmp_i32, NULL);
    insertion_sort(reference, N);

    for (usize i = 0; i < N; i++) {
        if (mine[i] != reference[i]) {
            fprintf(stderr, "position %zu: %d, wanted %d\n", i, mine[i], reference[i]);
            FAILm("the sort disagrees with an insertion sort");
        }
    }
    PASS();
}

SUITE(sort_suite) {
    RUN_TEST(sorts_in_the_order_the_comparator_asks_for);
    RUN_TEST(the_edges_do_nothing_harmful);
    RUN_TEST(large_elements_are_swapped_whole);
    RUN_TEST(matches_an_insertion_sort_on_random_input);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(sort_suite);
    GREATEST_MAIN_END();
}
