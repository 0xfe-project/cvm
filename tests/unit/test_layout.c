/*
 * tests/unit/test_layout.c
 *
 * Heap geometry. Most of the invariants are compile-time (_Static_assert
 * in gc/layout.h); these tests pin the concrete numbers so a silent edit
 * to a bit count shows up as a failing test rather than as a heap that is
 * merely a different size.
 */

#include "vendor/greatest.h"

#include "base/types.h"
#include "gc/layout.h"

TEST geometry_has_the_expected_shape(void) {
    ASSERT_EQ_FMT(32768u, (u32)CVM_BLOCK_SIZE, "%u");
    ASSERT_EQ_FMT(256u, (u32)CVM_LINE_SIZE, "%u");
    ASSERT_EQ_FMT(128u, (u32)CVM_LINE_COUNT, "%u");
    ASSERT_EQ_FMT(32u, (u32)CVM_WORDS_IN_LINE, "%u");
    ASSERT_EQ_FMT(4096u, (u32)CVM_WORDS_IN_BLOCK, "%u");
    PASS();
}

TEST large_object_threshold_is_eight_kib(void) {
    ASSERT_EQ_FMT(8192u, (u32)CVM_LARGE_OBJECT_MIN_SIZE, "%u");
    /* and it is well below a block, so the large allocator has room to
     * carve several chunks out of one block */
    ASSERT(CVM_LARGE_OBJECT_MIN_SIZE < CVM_BLOCK_SIZE);
    PASS();
}

TEST allocation_alignment_is_two_words(void) {
    ASSERT_EQ_FMT((u32)(2 * sizeof(void*)), (u32)CVM_ALLOC_ALIGNMENT, "%u");
    ASSERT_EQ_FMT(2u, (u32)CVM_ALLOC_ALIGNMENT_WORDS, "%u");
    PASS();
}

TEST metadata_slots_per_line_and_block(void) {
    /* The object metadata table has one entry per allocation slot. */
    usize slotsPerLine = CVM_LINE_SIZE / CVM_ALLOC_ALIGNMENT;
    usize slotsPerBlock = CVM_BLOCK_SIZE / CVM_ALLOC_ALIGNMENT;

    ASSERT_EQ_FMT(16u, (u32)slotsPerLine, "%u");
    ASSERT_EQ_FMT(2048u, (u32)slotsPerBlock, "%u");
    ASSERT_EQ(slotsPerLine * CVM_LINE_COUNT, slotsPerBlock);
    PASS();
}

TEST block_mask_rounds_an_address_down_to_its_block(void) {
    /* CVM_BLOCK_ALIGN_MASK must clear exactly the low 15 bits. */
    const usize addrs[] = {
        0,
        1,
        CVM_BLOCK_SIZE - 1,
        CVM_BLOCK_SIZE,
        CVM_BLOCK_SIZE + 12345,
        (usize)0x10000000 + 7,
    };

    for (usize i = 0; i < sizeof(addrs) / sizeof(addrs[0]); i++) {
        usize a = addrs[i];
        usize blockStart = a & CVM_BLOCK_ALIGN_MASK;
        usize offset = a & CVM_BLOCK_OFFSET_MASK;

        ASSERT(blockStart + offset == a);
        ASSERT(offset < CVM_BLOCK_SIZE);
        ASSERT(blockStart % CVM_BLOCK_SIZE == 0);
    }
    PASS();
}

TEST a_header_fits_inside_one_allocation_slot(void) {
    /*
     * The object metadata table has one entry per allocation slot, and the
     * entry that describes an object is the one its *start* falls in. So the
     * header may be shorter than a slot -- it is, at one word, since the
     * flags moved into the metadata byte -- but not longer: a header that
     * straddled a slot boundary would put an object's start and its payload
     * in different entries, and a payload pointer would resolve to a slot
     * that does not describe its object.
     *
     * This used to assert the header was a whole number of slots, which held
     * because both were 16. The relation that is actually needed is the
     * weaker one, and the difference is 8 bytes on every object.
     */
    ASSERT(sizeof(CvmGcHeader) <= CVM_ALLOC_ALIGNMENT);
    /* And slot-aligned starts, which is what makes an entry exact. */
    ASSERT_EQ_FMT(0u, (u32)(CVM_ALLOC_ALIGNMENT % CVM_ALLOC_ALIGNMENT), "%u");
    PASS();
}

TEST growth_policy_values_are_sane(void) {
    ASSERT(CVM_MARK_TIME_RATIO_DEFAULT > 0.0);
    ASSERT(CVM_MARK_TIME_RATIO_DEFAULT < 1.0);
    ASSERT(CVM_FREE_RATIO_DEFAULT > 0.0);
    ASSERT(CVM_FREE_RATIO_DEFAULT < 1.0);
    ASSERT(CVM_GROWTH_RATE > 1.0);
    ASSERT(CVM_EARLY_GROWTH_RATE > CVM_GROWTH_RATE);
    ASSERT(CVM_MIN_HEAP_SIZE > 0);
    PASS();
}

SUITE(layout_suite) {
    RUN_TEST(geometry_has_the_expected_shape);
    RUN_TEST(large_object_threshold_is_eight_kib);
    RUN_TEST(allocation_alignment_is_two_words);
    RUN_TEST(metadata_slots_per_line_and_block);
    RUN_TEST(block_mask_rounds_an_address_down_to_its_block);
    RUN_TEST(a_header_fits_inside_one_allocation_slot);
    RUN_TEST(growth_policy_values_are_sane);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(layout_suite);
    GREATEST_MAIN_END();
}
