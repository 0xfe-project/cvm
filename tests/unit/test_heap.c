/*
 * tests/unit/test_heap.c
 *
 * Heap layout and growth.
 *
 * The properties that matter: the reservations are sized from the maximum
 * and aligned as the address arithmetic in gc/meta.h requires, growth
 * moves the end pointers consistently, and the new metadata is in a known
 * state rather than whatever the reservation happened to contain.
 *
 * The heap is a real reservation, so these tests map and unmap real
 * address space. Sizes are kept to a few megabytes.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include "base/math.h"
#include "base/types.h"
#include "gc/bytemap.h"
#include "gc/heap.h"
#include "gc/layout.h"
#include "gc/meta.h"
#include "rt.h"

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)16 * 1024 * 1024)

static CvmHeap* heap_new(usize min_size, usize max_size) {
    static CvmHeap heap;
    if (!cvm_heap_init(&heap, min_size, max_size)) {
        return NULL;
    }
    return &heap;
}

/* ------------------------------------------------------------------ */
/* reservations                                                        */
/* ------------------------------------------------------------------ */

TEST region_reserve_aligns_the_start(void) {
    CvmRegion r;
    ASSERT(cvm_region_reserve(&r, 3 * CVM_BLOCK_SIZE, CVM_BLOCK_SIZE));

    ASSERT(r.base != NULL);
    ASSERT(r.start != NULL);
    ASSERT_EQ_FMT(0u, (u32)((usize)r.start % CVM_BLOCK_SIZE), "%u");
    /* the whole requested size is usable despite the alignment */
    ASSERT(r.usable == 3 * CVM_BLOCK_SIZE);
    /* and the aligned start is inside the reservation */
    ASSERT(r.start >= r.base);
    ASSERT(r.start + r.usable <= r.base + r.size);

    cvm_region_release(&r);
    ASSERT(r.base == NULL);
    PASS();
}

TEST region_reserve_of_zero_is_empty_not_failed(void) {
    CvmRegion r;
    ASSERT(cvm_region_reserve(&r, 0, 64));
    ASSERT(r.base == NULL);
    cvm_region_release(&r);
    PASS();
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */

TEST init_establishes_a_consistent_layout(void) {
    CvmHeap* heap = heap_new(TEST_MIN, TEST_MAX);
    ASSERT(heap != NULL);

    ASSERT(heap->heap_start != NULL);
    ASSERT(heap->block_count > 0);
    ASSERT(heap->max_block_count >= heap->block_count);

    /* the heap window is block-aligned, which the mask-based
     * address-to-block conversion depends on */
    ASSERT_EQ_FMT(0u, (u32)((usize)heap->heap_start % CVM_BLOCK_SIZE), "%u");

    /* end pointers agree with the block count */
    ASSERT(heap->heap_end ==
           heap->heap_start + (usize)heap->block_count * CVM_BLOCK_SIZE);
    ASSERT(heap->block_meta_end == heap->block_meta_start + heap->block_count);
    ASSERT(heap->line_meta_end ==
           heap->line_meta_start + (usize)heap->block_count * CVM_LINE_COUNT);

    ASSERT_EQ_FMT((u32)(heap->block_count * CVM_SPACE_USED_PER_BLOCK),
                  (u32)heap->heap_size, "%u");

    /* metadata regions are sized for the maximum, not the initial count */
    ASSERT(heap->block_meta_region.usable >=
           (usize)heap->max_block_count * sizeof(BlockMeta));
    ASSERT(heap->line_meta_region.usable >=
           (usize)heap->max_block_count * CVM_LINE_COUNT * sizeof(LineMeta));

    cvm_heap_destroy(heap);
    PASS();
}

TEST init_raises_a_too_small_minimum(void) {
    CvmHeap* heap = heap_new(1, TEST_MAX); /* 1 byte */
    ASSERT(heap != NULL);

    /* the minimum is raised, so at least one block of payload exists */
    ASSERT(heap->heap_size >= CVM_MIN_HEAP_SIZE);
    ASSERT(heap->block_count > 0);

    cvm_heap_destroy(heap);
    PASS();
}

TEST init_rejects_a_max_below_the_minimum(void) {
    static CvmHeap heap;
    ASSERT_FALSE(cvm_heap_init(&heap, 8 * 1024 * 1024, 4 * 1024 * 1024));
    PASS();
}

TEST init_rejects_a_heap_too_small_for_a_superblock(void) {
    /* One block cannot hold a superblock, so nothing above the
     * large-object threshold could ever be allocated. Better to refuse
     * than to run a heap that cannot serve its allocator. */
    static CvmHeap heap;
    ASSERT_FALSE(cvm_heap_init(&heap, CVM_MIN_HEAP_SIZE, CVM_MIN_HEAP_SIZE - 1));
    PASS();
}

TEST init_then_destroy_then_init_again_works(void) {
    static CvmHeap heap;

    ASSERT(cvm_heap_init(&heap, TEST_MIN, TEST_MAX));
    u32 first_count = heap.block_count;
    cvm_heap_destroy(&heap);

    ASSERT(cvm_heap_init(&heap, TEST_MIN, TEST_MAX));
    ASSERT_EQ_FMT(first_count, heap.block_count, "%u");
    cvm_heap_destroy(&heap);
    PASS();
}

/* ------------------------------------------------------------------ */
/* address arithmetic                                                  */
/* ------------------------------------------------------------------ */

TEST block_addresses_are_contiguous_and_in_range(void) {
    CvmHeap* heap = heap_new(TEST_MIN, TEST_MAX);
    ASSERT(heap != NULL);

    for (u32 i = 0; i < heap->block_count; i++) {
        u8* addr = cvm_heap_block_address(heap, i);
        ASSERT(addr == heap->heap_start + (usize)i * CVM_BLOCK_SIZE);

        /* the block index computed from the address must agree, which is
         * the property the whole mask-based scheme rests on */
        ASSERT_EQ_FMT(i, block_index_of(heap->heap_start, addr), "%u");
        ASSERT_EQ_FMT(i, block_index_of(heap->heap_start, addr + 7), "%u");

        ASSERT(cvm_heap_contains(heap, addr));
        ASSERT(cvm_heap_contains(heap, addr + CVM_BLOCK_SIZE - 1));
    }

    /* one past the end is not in the heap */
    ASSERT(cvm_heap_block_address(heap, heap->block_count) == NULL);
    ASSERT_FALSE(cvm_heap_contains(heap, heap->heap_end));

    cvm_heap_destroy(heap);
    PASS();
}

TEST metadata_accessors_line_up_with_blocks(void) {
    CvmHeap* heap = heap_new(TEST_MIN, TEST_MAX);
    ASSERT(heap != NULL);

    for (u32 i = 0; i < 3 && i < heap->block_count; i++) {
        ASSERT(cvm_heap_block_meta(heap, i) == heap->block_meta_start + i);
        ASSERT(cvm_heap_line_meta(heap, i) ==
               heap->line_meta_start + (usize)i * CVM_LINE_COUNT);
    }

    /* every line's metadata lives inside the reserved region */
    ASSERT((u8*)(heap->line_meta_start + (usize)heap->block_count * CVM_LINE_COUNT) <=
           heap->line_meta_region.start + heap->line_meta_region.usable);

    cvm_heap_destroy(heap);
    PASS();
}

/* ------------------------------------------------------------------ */
/* growth                                                              */
/* ------------------------------------------------------------------ */

TEST extend_moves_the_ends_and_reports_the_new_range(void) {
    CvmHeap* heap = heap_new(TEST_MIN, TEST_MAX);
    ASSERT(heap != NULL);

    u32 before_blocks = heap->block_count;
    u8* before_end = heap->heap_end;
    BlockMeta* before_meta_end = heap->block_meta_end;

    u32 first = 0;
    ASSERT(cvm_heap_extend(heap, 4, &first));

    ASSERT_EQ_FMT(before_blocks, first, "%u");
    ASSERT_EQ_FMT(before_blocks + 4, heap->block_count, "%u");
    ASSERT(heap->heap_end == before_end + 4 * CVM_BLOCK_SIZE);
    ASSERT(heap->block_meta_end == before_meta_end + 4);

    /* the newly added blocks are addressable */
    ASSERT(cvm_heap_block_address(heap, before_blocks) != NULL);
    ASSERT(cvm_heap_block_address(heap, before_blocks + 3) != NULL);
    ASSERT(cvm_heap_block_address(heap, before_blocks + 4) == NULL);

    cvm_heap_destroy(heap);
    PASS();
}

TEST extend_clears_the_new_metadata(void) {
    CvmHeap* heap = heap_new(TEST_MIN, TEST_MAX);
    ASSERT(heap != NULL);

    u32 first = 0;
    ASSERT(cvm_heap_extend(heap, 3, &first));

    for (u32 i = first; i < first + 3; i++) {
        BlockMeta* meta = cvm_heap_block_meta(heap, i);
        ASSERT(block_meta_is_free(meta));
        ASSERT_EQ_FMT(-1, (int)meta->next_block, "%d");

        LineMeta* lines = cvm_heap_line_meta(heap, i);
        for (u32 l = 0; l < CVM_LINE_COUNT; l++) {
            ASSERT_FALSE(line_meta_is_marked(&lines[l]));
        }
    }

    cvm_heap_destroy(heap);
    PASS();
}

TEST extend_beyond_the_reservation_fails(void) {
    CvmHeap* heap = heap_new(TEST_MIN, TEST_MAX);
    ASSERT(heap != NULL);

    u32 remaining = heap->max_block_count - heap->block_count;

    /* exactly the remaining amount is allowed */
    ASSERT(cvm_heap_extend(heap, remaining, NULL));
    ASSERT_EQ_FMT(heap->max_block_count, heap->block_count, "%u");

    /* one more is not */
    ASSERT_FALSE(cvm_heap_extend(heap, 1, NULL));
    ASSERT_EQ_FMT(heap->max_block_count, heap->block_count, "%u");

    cvm_heap_destroy(heap);
    PASS();
}

TEST extend_by_zero_succeeds_without_changing_anything(void) {
    CvmHeap* heap = heap_new(TEST_MIN, TEST_MAX);
    ASSERT(heap != NULL);

    u32 before = heap->block_count;
    ASSERT(cvm_heap_extend(heap, 0, NULL));
    ASSERT_EQ_FMT(before, heap->block_count, "%u");

    cvm_heap_destroy(heap);
    PASS();
}

TEST the_bytemap_covers_the_grown_heap(void) {
    CvmHeap* heap = heap_new(TEST_MIN, TEST_MAX);
    ASSERT(heap != NULL);

    ASSERT(cvm_heap_extend(heap, 8, NULL));

    /* every allocation slot in the heap maps into the bytemap, including
     * the slots of the blocks just added */
    u8* last_block = cvm_heap_block_address(heap, heap->block_count - 1);
    ObjectMeta* slot = bytemap_get(heap->bytemap, last_block);
    ASSERT(slot >= heap->bytemap->slots);
    ASSERT(slot < bytemap_end(heap->bytemap));
    ASSERT(object_meta_is_free(slot));

    cvm_heap_destroy(heap);
    PASS();
}

/* ------------------------------------------------------------------ */
/* growth policy                                                       */
/* ------------------------------------------------------------------ */

TEST growth_increment_always_makes_progress(void) {
    CvmHeap* heap = heap_new(TEST_MIN, TEST_MAX);
    ASSERT(heap != NULL);

    u32 inc = cvm_heap_growth_increment(heap);
    ASSERT(inc >= 1);
    ASSERT(inc <= heap->max_block_count - heap->block_count);

    cvm_heap_destroy(heap);
    PASS();
}

TEST growth_increment_is_clamped_to_the_remaining_room(void) {
    CvmHeap* heap = heap_new(TEST_MIN, TEST_MAX);
    ASSERT(heap != NULL);

    /* fill it up so only one block of room is left */
    u32 remaining = heap->max_block_count - heap->block_count;
    ASSERT(cvm_heap_extend(heap, remaining - 1, NULL));

    u32 inc = cvm_heap_growth_increment(heap);
    ASSERT_EQ_FMT(1u, inc, "%u");

    cvm_heap_destroy(heap);
    PASS();
}

TEST should_grow_on_mark_time_ratio(void) {
    CvmHeap* heap = heap_new(TEST_MIN, TEST_MAX);
    ASSERT(heap != NULL);

    /* plenty of free blocks, but marking is eating the program's time */
    u32 free_blocks = heap->block_count;
    ASSERT(cvm_heap_should_grow(heap, 1000, 10000, free_blocks)); /* 10% > 5% */

    /* and not, when marking is cheap and blocks are plentiful */
    ASSERT_FALSE(cvm_heap_should_grow(heap, 1, 10000, free_blocks));

    cvm_heap_destroy(heap);
    PASS();
}

TEST should_grow_on_free_block_ratio(void) {
    CvmHeap* heap = heap_new(TEST_MIN, TEST_MAX);
    ASSERT(heap != NULL);

    /* marking is cheap, but almost nothing is free */
    ASSERT(cvm_heap_should_grow(heap, 1, 1000000, 1));

    /* Half the blocks free is the default threshold. Tested with a value
     * safely above it rather than exactly at it: with an odd block count,
     * `block_count / 2` truncates to a ratio just under 0.5, which would
     * trip the threshold and make this assertion about integer division
     * rather than about the policy. */
    ASSERT_FALSE(cvm_heap_should_grow(heap, 1, 1000000, heap->block_count - 1));

    cvm_heap_destroy(heap);
    PASS();
}

TEST should_grow_ignores_a_zero_total_time(void) {
    /* A collection that has not been timed yet must not be read as an
     * infinite mark ratio. */
    CvmHeap* heap = heap_new(TEST_MIN, TEST_MAX);
    ASSERT(heap != NULL);

    ASSERT_FALSE(cvm_heap_should_grow(heap, 0, 0, heap->block_count));

    cvm_heap_destroy(heap);
    PASS();
}

SUITE(heap_suite) {
    RUN_TEST(region_reserve_aligns_the_start);
    RUN_TEST(region_reserve_of_zero_is_empty_not_failed);
    RUN_TEST(init_establishes_a_consistent_layout);
    RUN_TEST(init_raises_a_too_small_minimum);
    RUN_TEST(init_rejects_a_max_below_the_minimum);
    RUN_TEST(init_rejects_a_heap_too_small_for_a_superblock);
    RUN_TEST(init_then_destroy_then_init_again_works);
    RUN_TEST(block_addresses_are_contiguous_and_in_range);
    RUN_TEST(metadata_accessors_line_up_with_blocks);
    RUN_TEST(extend_moves_the_ends_and_reports_the_new_range);
    RUN_TEST(extend_clears_the_new_metadata);
    RUN_TEST(extend_beyond_the_reservation_fails);
    RUN_TEST(extend_by_zero_succeeds_without_changing_anything);
    RUN_TEST(the_bytemap_covers_the_grown_heap);
    RUN_TEST(growth_increment_always_makes_progress);
    RUN_TEST(growth_increment_is_clamped_to_the_remaining_room);
    RUN_TEST(should_grow_on_mark_time_ratio);
    RUN_TEST(should_grow_on_free_block_ratio);
    RUN_TEST(should_grow_ignores_a_zero_total_time);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(heap_suite);
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
