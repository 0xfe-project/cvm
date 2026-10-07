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

TEST every_allocation_slot_holds_a_whole_header(void) {
    /*
     * The object metadata table has one entry per allocation slot. The
     * smallest possible object is a bare header, so a header must fit in
     * a whole number of slots: otherwise an object's metadata entry would
     * straddle a slot boundary and the table would describe the wrong
     * thing. Both quantities are 16 today; the invariant is the relation,
     * not the number.
     */
    ASSERT(CVM_ALLOC_ALIGNMENT >= sizeof(CvmGcHeader));
    ASSERT_EQ_FMT(0u, (u32)(sizeof(CvmGcHeader) % CVM_ALLOC_ALIGNMENT), "%u");
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
    RUN_TEST(every_allocation_slot_holds_a_whole_header);
    RUN_TEST(growth_policy_values_are_sane);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(layout_suite);
    GREATEST_MAIN_END();
}
