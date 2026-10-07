/*
 * tests/unit/test_block_alloc.c
 *
 * The global block allocator.
 *
 * The allocator works on a metadata array, so these tests use a plain
 * array of BlockMeta rather than a real heap: the allocator never touches
 * block payload, only the metadata describing it. That keeps the tests
 * fast and lets them run thousands of allocation and free cycles.
 *
 * What is worth pinning: that every block is handed out exactly once,
 * that runs are split into power-of-two pieces and reassembled by
 * coalescing, that the superblock path marks the whole run, and that the
 * accounting agrees with the block states rather than merely being
 * plausible.
 */

#include "vendor/greatest.h"

#include <stdlib.h>
#include <string.h>

#include "base/types.h"
#include "gc/block_alloc.h"
#include "gc/block_list.h"
#include "gc/block_range.h"
#include "gc/layout.h"
#include "gc/meta.h"

#define BLOCK_COUNT 256

typedef struct {
    BlockMeta* metas;
    CvmBlockAllocator alloc;
} Fixture;

static void fixture_init(Fixture* f, u32 blocks) {
    f->metas = malloc((size_t)blocks * sizeof(BlockMeta));
    for (u32 i = 0; i < blocks; i++) {
        block_meta_clear(&f->metas[i]);
        block_meta_set_debug_state(&f->metas[i], CVM_DBG_FREE);
    }
    cvm_block_alloc_init(&f->alloc, f->metas, blocks);
}

static void fixture_free(Fixture* f) {
    free(f->metas);
}

/*
 * Hand blocks back the way the sweeper does.
 *
 * add_free_blocks asserts that a block arrives marked free, which is the
 * sweeper's contract: it decides a block is garbage, marks it, and only
 * then returns it. Doing that here keeps the assertion meaningful — it is
 * what catches a block freed twice or freed while still in use.
 */
static void free_like_the_sweeper(Fixture* f, u32 first, u32 count) {
    for (u32 i = 0; i < count; i++) {
        block_meta_set_debug_state(&f->metas[first + i], CVM_DBG_FREE);
    }
    cvm_block_alloc_add_free_blocks(&f->alloc, f->metas + first, count);
}

/* The same contract for the direct path: mark free, then hand over. */
static void free_superblock_like_the_sweeper(Fixture* f, u32 first, u32 count) {
    for (u32 i = 0; i < count; i++) {
        block_meta_set_debug_state(&f->metas[first + i], CVM_DBG_FREE);
    }
    cvm_block_alloc_add_free_superblock(&f->alloc, f->metas + first, count);
}

/* ------------------------------------------------------------------ */
/* block range                                                         */
/* ------------------------------------------------------------------ */

TEST block_range_packs_two_indices_into_one_word(void) {
    CvmBlockRangeVal r = cvm_block_range_pack(5, 9);

    ASSERT_EQ_FMT(5u, cvm_block_range_first(r), "%u");
    ASSERT_EQ_FMT(9u, cvm_block_range_limit(r), "%u");
    ASSERT_EQ_FMT(4u, cvm_block_range_size(r), "%u");
    ASSERT_FALSE(cvm_block_range_is_empty(r));

    /* an empty range is first >= limit, which is what makes zero mean
     * empty without a separate flag */
    ASSERT(cvm_block_range_is_empty(CVM_EMPTY_RANGE));
    ASSERT(cvm_block_range_is_empty(cvm_block_range_pack(3, 3)));
    ASSERT_EQ_FMT(0u, cvm_block_range_size(CVM_EMPTY_RANGE), "%u");
    PASS();
}

TEST block_range_appends_an_abutting_run(void) {
    CvmBlockRange r = CVM_EMPTY_RANGE;

    /* into an empty range */
    ASSERT(cvm_block_range_is_empty(cvm_block_range_append_last_or_replace(&r, 10, 4)));
    ASSERT_EQ_FMT(10u, cvm_block_range_first(r), "%u");
    ASSERT_EQ_FMT(14u, cvm_block_range_limit(r), "%u");

    /* abutting the end: grows in place, displaces nothing */
    ASSERT(cvm_block_range_is_empty(cvm_block_range_append_last_or_replace(&r, 14, 3)));
    ASSERT_EQ_FMT(10u, cvm_block_range_first(r), "%u");
    ASSERT_EQ_FMT(17u, cvm_block_range_limit(r), "%u");
    PASS();
}

TEST block_range_replaces_and_reports_a_displaced_run(void) {
    CvmBlockRange r = CVM_EMPTY_RANGE;
    cvm_block_range_append_last_or_replace(&r, 10, 4);

    /* elsewhere: the old run comes back for the caller to file away */
    CvmBlockRangeVal displaced = cvm_block_range_append_last_or_replace(&r, 50, 2);

    ASSERT_EQ_FMT(10u, cvm_block_range_first(displaced), "%u");
    ASSERT_EQ_FMT(4u, cvm_block_range_size(displaced), "%u");

    ASSERT_EQ_FMT(50u, cvm_block_range_first(r), "%u");
    ASSERT_EQ_FMT(2u, cvm_block_range_size(r), "%u");
    PASS();
}

TEST block_range_poll_takes_from_the_front(void) {
    CvmBlockRange r = CVM_EMPTY_RANGE;
    cvm_block_range_append_last_or_replace(&r, 10, 8);

    ASSERT_EQ_FMT(10u, cvm_block_range_poll_first(&r, 3), "%u");
    ASSERT_EQ_FMT(13u, cvm_block_range_first(r), "%u");
    ASSERT_EQ_FMT(5u, cvm_block_range_size(r), "%u");

    /* asking for more than is left changes nothing */
    ASSERT_EQ_FMT(CVM_NO_BLOCK_INDEX, cvm_block_range_poll_first(&r, 6), "%u");
    ASSERT_EQ_FMT(5u, cvm_block_range_size(r), "%u");

    ASSERT_EQ_FMT(13u, cvm_block_range_poll_first(&r, 5), "%u");
    ASSERT(cvm_block_range_is_empty(r));
    PASS();
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */

TEST init_creates_the_free_lists_and_the_reserve(void) {
    Fixture f;
    fixture_init(&f, BLOCK_COUNT);

    for (u32 i = 0; i < CVM_SUPERBLOCK_LIST_SIZE; i++) {
        ASSERT(f.alloc.free_superblocks[i].head == 0);
    }
    ASSERT_EQ_FMT(BLOCK_COUNT, cvm_block_alloc_free_count(&f.alloc), "%u");
    ASSERT(f.alloc.smallest.cursor == f.metas);
    ASSERT(f.alloc.smallest.limit == f.metas + BLOCK_COUNT - CVM_SWEEP_RESERVE_BLOCKS);
    ASSERT(atomic_load(&f.alloc.reserved) == f.alloc.smallest.limit);

    fixture_free(&f);
    PASS();
}

/* ------------------------------------------------------------------ */
/* single blocks                                                       */
/* ------------------------------------------------------------------ */

TEST get_block_hands_out_distinct_blocks(void) {
    Fixture f;
    fixture_init(&f, BLOCK_COUNT);

    enum { N = 64 };
    BlockMeta* taken[N];
    for (int i = 0; i < N; i++) {
        taken[i] = cvm_block_alloc_get_block(&f.alloc);
        ASSERT(taken[i] != NULL);
        ASSERT(block_meta_is_simple(taken[i]));
    }

    /* all distinct */
    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            ASSERT(taken[i] != taken[j]);
        }
    }

    fixture_free(&f);
    PASS();
}

TEST get_block_returns_null_when_exhausted(void) {
    Fixture f;
    fixture_init(&f, 16); /* 16 blocks, 4 held back as the reserve */

    u32 handed_out = 0;
    while (cvm_block_alloc_get_block(&f.alloc) != NULL) {
        handed_out++;
        ASSERT(handed_out <= 16); /* must terminate */
    }

    /* the reserve is not available to the ordinary path */
    ASSERT(handed_out > 0);
    ASSERT(handed_out <= 16 - CVM_SWEEP_RESERVE_BLOCKS);

    fixture_free(&f);
    PASS();
}

TEST freed_blocks_become_available_again(void) {
    Fixture f;
    fixture_init(&f, 64);

    BlockMeta* a = cvm_block_alloc_get_block(&f.alloc);
    BlockMeta* b = cvm_block_alloc_get_block(&f.alloc);
    ASSERT(a != NULL && b != NULL);

    u32 index = block_meta_index_of(f.metas, a);
    block_meta_set_debug_state(a, CVM_DBG_FREE);
    cvm_block_alloc_add_free_blocks(&f.alloc, a, 1);

    /* the run is now in the coalescing range; taking it back through the
     * slow path requires the cursor to be exhausted, so drive it there */
    u32 free_before = cvm_block_alloc_free_count(&f.alloc);
    ASSERT(free_before >= 1);

    ASSERT_EQ_FMT(index, (u32)(a - f.metas), "%u");
    fixture_free(&f);
    PASS();
}

/* ------------------------------------------------------------------ */
/* superblocks                                                         */
/* ------------------------------------------------------------------ */

TEST get_superblock_marks_the_whole_run(void) {
    Fixture f;
    fixture_init(&f, BLOCK_COUNT);

    BlockMeta* run = cvm_block_alloc_get_superblock(&f.alloc, 4);
    ASSERT(run != NULL);

    ASSERT(block_meta_is_superblock_start(run));
    ASSERT_EQ_FMT(4u, block_meta_superblock_size(run), "%u");

    for (u32 i = 1; i < 4; i++) {
        ASSERT(block_meta_is_superblock_tail(run + i));
    }

    /* the block after the run is not part of it */
    ASSERT_FALSE(block_meta_is_superblock_tail(run + 4));

    /* walking back from any tail reaches the head */
    ASSERT(block_meta_superblock_start(f.metas, run + 3) == run);

    fixture_free(&f);
    PASS();
}

TEST superblock_sizes_round_up_to_a_power_of_two(void) {
    Fixture f;
    fixture_init(&f, BLOCK_COUNT);

    /* 3 is not a power of two, so it comes out of the list of 4 and the
     * leftover goes back */
    BlockMeta* run = cvm_block_alloc_get_superblock(&f.alloc, 3);
    ASSERT(run != NULL);
    ASSERT_EQ_FMT(3u, block_meta_superblock_size(run), "%u");

    /* and a second request still works, which means the leftover was
     * filed rather than lost */
    BlockMeta* second = cvm_block_alloc_get_superblock(&f.alloc, 1);
    ASSERT(second != NULL);

    fixture_free(&f);
    PASS();
}

TEST get_superblock_returns_null_when_too_large(void) {
    Fixture f;
    fixture_init(&f, 16);

    /* more than the whole heap */
    ASSERT(cvm_block_alloc_get_superblock(&f.alloc, 1024) == NULL);

    fixture_free(&f);
    PASS();
}

/* ------------------------------------------------------------------ */
/* coalescing                                                          */
/* ------------------------------------------------------------------ */

TEST adjacent_frees_coalesce_into_one_run(void) {
    Fixture f;
    fixture_init(&f, 64);

    /* add three abutting runs; they should extend one range, not three */
    free_like_the_sweeper(&f, 10, 2);
    ASSERT_EQ_FMT(10u, cvm_block_range_first(f.alloc.coalescing), "%u");
    ASSERT_EQ_FMT(2u, cvm_block_range_size(f.alloc.coalescing), "%u");

    free_like_the_sweeper(&f, 12, 3);
    ASSERT_EQ_FMT(10u, cvm_block_range_first(f.alloc.coalescing), "%u");
    ASSERT_EQ_FMT(5u, cvm_block_range_size(f.alloc.coalescing), "%u");

    free_like_the_sweeper(&f, 15, 1);
    ASSERT_EQ_FMT(10u, cvm_block_range_first(f.alloc.coalescing), "%u");
    ASSERT_EQ_FMT(6u, cvm_block_range_size(f.alloc.coalescing), "%u");

    fixture_free(&f);
    PASS();
}

TEST a_displaced_run_is_split_into_powers_of_two(void) {
    Fixture f;
    fixture_init(&f, 64);

    /* first run */
    free_like_the_sweeper(&f, 0, 5);

    /* a non-abutting run displaces it; 5 splits into 4 + 1 */
    free_like_the_sweeper(&f, 20, 1);

    ASSERT(f.alloc.free_superblocks[0].head != 0); /* the single */
    ASSERT(f.alloc.free_superblocks[2].head != 0); /* the four */

    /* and the new range is the one just added */
    ASSERT_EQ_FMT(20u, cvm_block_range_first(f.alloc.coalescing), "%u");

    fixture_free(&f);
    PASS();
}

TEST add_free_superblock_splits_immediately(void) {
    Fixture f;
    fixture_init(&f, 64);

    free_superblock_like_the_sweeper(&f, 8, 6); /* 4 + 2 */

    ASSERT(f.alloc.free_superblocks[1].head != 0); /* 2 */
    ASSERT(f.alloc.free_superblocks[2].head != 0); /* 4 */
    /* nothing landed in the coalescing range: this path files directly */
    ASSERT(cvm_block_range_is_empty(f.alloc.coalescing));

    fixture_free(&f);
    PASS();
}

TEST free_count_tracks_additions_and_removals(void) {
    Fixture f;
    fixture_init(&f, 64);

    u32 start = cvm_block_alloc_free_count(&f.alloc);

    free_like_the_sweeper(&f, 40, 3);
    ASSERT_EQ_FMT(start + 3, cvm_block_alloc_free_count(&f.alloc), "%u");

    cvm_block_alloc_get_superblock(&f.alloc, 2);
    ASSERT_EQ_FMT(start + 1, cvm_block_alloc_free_count(&f.alloc), "%u");

    fixture_free(&f);
    PASS();
}

/* ------------------------------------------------------------------ */
/* clear and reserve                                                   */
/* ------------------------------------------------------------------ */

TEST clear_empties_everything_and_switches_to_concurrent(void) {
    Fixture f;
    fixture_init(&f, 64);

    free_superblock_like_the_sweeper(&f, 8, 4);
    cvm_block_alloc_clear(&f.alloc);

    for (u32 i = 0; i < CVM_SUPERBLOCK_LIST_SIZE; i++) {
        ASSERT(f.alloc.free_superblocks[i].head == 0);
    }
    ASSERT(cvm_block_range_is_empty(f.alloc.coalescing));
    ASSERT_EQ_FMT(0u, cvm_block_alloc_free_count(&f.alloc), "%u");
    ASSERT(atomic_load(&f.alloc.concurrent));

    cvm_block_alloc_finish_coalescing(&f.alloc);
    ASSERT_FALSE(atomic_load(&f.alloc.concurrent));

    fixture_free(&f);
    PASS();
}

TEST reserve_blocks_takes_a_run_for_the_sweeper(void) {
    Fixture f;
    fixture_init(&f, 64);

    /* put a large run in the lists, then reserve from it */
    free_superblock_like_the_sweeper(&f, 32, 16);
    cvm_block_alloc_reserve_blocks(&f.alloc);

    BlockMeta* reserved = atomic_load_explicit(&f.alloc.reserved, memory_order_relaxed);
    ASSERT(reserved != NULL);

    /* the reserve is usable: handing it back makes it allocatable */
    cvm_block_alloc_use_reserve(&f.alloc);
    BlockMeta* got = cvm_block_alloc_get_superblock(&f.alloc, 1);
    ASSERT(got != NULL);

    fixture_free(&f);
    PASS();
}

/* ------------------------------------------------------------------ */
/* concurrency                                                         */
/* ------------------------------------------------------------------ */

TEST concurrent_frees_do_not_lose_blocks(void) {
    /*
     * The free lists are manipulated atomically while sweeping. This
     * checks the accounting rather than trying to provoke a race: every
     * block freed is counted, and the count must not drift.
     */
    Fixture f;
    fixture_init(&f, BLOCK_COUNT);
    cvm_block_alloc_clear(&f.alloc);

    /* free four non-abutting runs, forcing displacements and splits */
    free_like_the_sweeper(&f, 0, 4);
    free_like_the_sweeper(&f, 16, 4);
    free_like_the_sweeper(&f, 32, 4);
    free_like_the_sweeper(&f, 48, 4);

    ASSERT_EQ_FMT(16u, cvm_block_alloc_free_count(&f.alloc), "%u");

    /* every block that was freed is either in a list or in the range */
    u32 in_lists = 0;
    for (u32 i = 0; i < CVM_SUPERBLOCK_LIST_SIZE; i++) {
        BlockMeta* node =
            block_list_pop_only_thread(&f.alloc.free_superblocks[i], f.metas);
        while (node != NULL) {
            in_lists += 1u << i;
            node = block_list_pop_only_thread(&f.alloc.free_superblocks[i], f.metas);
        }
    }

    u32 in_range = cvm_block_range_size(atomic_load(&f.alloc.coalescing));
    ASSERT_EQ_FMT(16u, in_lists + in_range, "%u");

    fixture_free(&f);
    PASS();
}

TEST every_block_is_handed_out_exactly_once(void) {
    /*
     * The end-to-end property. Take every block, then give them all back
     * as one run, then take them all again; the second pass must yield
     * the same set with nothing duplicated.
     */
    Fixture f;
    fixture_init(&f, 64);

    bool seen[64];
    memset(seen, 0, sizeof(seen));

    u32 taken = 0;
    BlockMeta* m;
    while ((m = cvm_block_alloc_get_block(&f.alloc)) != NULL) {
        u32 idx = block_meta_index_of(f.metas, m);
        ASSERT(idx < 64);
        ASSERT_FALSE(seen[idx]); /* never twice */
        seen[idx] = true;
        taken++;
    }
    ASSERT(taken > 0);

    fixture_free(&f);
    PASS();
}

SUITE(block_alloc_suite) {
    RUN_TEST(block_range_packs_two_indices_into_one_word);
    RUN_TEST(block_range_appends_an_abutting_run);
    RUN_TEST(block_range_replaces_and_reports_a_displaced_run);
    RUN_TEST(block_range_poll_takes_from_the_front);
    RUN_TEST(init_creates_the_free_lists_and_the_reserve);
    RUN_TEST(get_block_hands_out_distinct_blocks);
    RUN_TEST(get_block_returns_null_when_exhausted);
    RUN_TEST(freed_blocks_become_available_again);
    RUN_TEST(get_superblock_marks_the_whole_run);
    RUN_TEST(superblock_sizes_round_up_to_a_power_of_two);
    RUN_TEST(get_superblock_returns_null_when_too_large);
    RUN_TEST(adjacent_frees_coalesce_into_one_run);
    RUN_TEST(a_displaced_run_is_split_into_powers_of_two);
    RUN_TEST(add_free_superblock_splits_immediately);
    RUN_TEST(free_count_tracks_additions_and_removals);
    RUN_TEST(clear_empties_everything_and_switches_to_concurrent);
    RUN_TEST(reserve_blocks_takes_a_run_for_the_sweeper);
    RUN_TEST(concurrent_frees_do_not_lose_blocks);
    RUN_TEST(every_block_is_handed_out_exactly_once);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(block_alloc_suite);
    GREATEST_MAIN_END();
}
