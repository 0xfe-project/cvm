/*
 * tests/unit/test_meta.c
 *
 * The three levels of GC metadata. These tests pin the state machine and
 * the byte layout, because both are load-bearing: the predicates compare
 * flags for equality, and the metadata tables are walked by address
 * arithmetic that assumes the struct sizes asserted in gc/meta.h.
 */

#include "vendor/greatest.h"

#include <stdlib.h>

#include "base/math.h"
#include "gc/meta.h"

/* ------------------------------------------------------------------ */
/* BlockMeta                                                           */
/* ------------------------------------------------------------------ */

TEST block_meta_layout_is_pinned(void) {
    ASSERT_EQ_FMT(4u, (u32)sizeof(((BlockMeta*)0)->block), "%u");
    ASSERT_EQ_FMT(12u, (u32)sizeof(BlockMeta), "%u");
    PASS();
}

TEST block_meta_starts_free(void) {
    BlockMeta m;
    block_meta_clear(&m);

    ASSERT(block_meta_is_free(&m));
    ASSERT_FALSE(block_meta_is_simple(&m));
    ASSERT_FALSE(block_meta_is_marked(&m));
    ASSERT_FALSE(block_meta_contains_large_objects(&m));
    ASSERT_EQ_FMT(CVM_LAST_HOLE, (int)m.block.simple.first_free_line, "%d");
    ASSERT_EQ_FMT(-1, (int)m.next_block, "%d");
    PASS();
}

TEST block_meta_state_predicates_are_exact(void) {
    BlockMeta m;

    block_meta_set_flag(&m, CVM_BLOCK_FREE);
    ASSERT(block_meta_is_free(&m));

    block_meta_set_flag(&m, CVM_BLOCK_SIMPLE);
    ASSERT(block_meta_is_simple(&m));
    ASSERT_FALSE(block_meta_is_marked(&m));

    /* marked counts as simple: it is a simple block mid-collection */
    block_meta_set_flag(&m, CVM_BLOCK_MARKED);
    ASSERT(block_meta_is_simple(&m));
    ASSERT(block_meta_is_marked(&m));

    block_meta_set_flag(&m, CVM_BLOCK_RESERVED);
    ASSERT(block_meta_is_reserved(&m));
    ASSERT_FALSE(block_meta_is_simple(&m));

    block_meta_set_flag(&m, CVM_BLOCK_SUPERBLOCK_START);
    ASSERT(block_meta_is_superblock_start(&m));
    ASSERT(block_meta_contains_large_objects(&m));

    block_meta_set_flag(&m, CVM_BLOCK_SUPERBLOCK_TAIL);
    ASSERT(block_meta_is_superblock_tail(&m));
    ASSERT(block_meta_contains_large_objects(&m));

    block_meta_set_flag(&m, CVM_BLOCK_SUPERBLOCK_START_ME);
    ASSERT(block_meta_is_superblock_start_me(&m));
    ASSERT_FALSE(block_meta_is_superblock_start(&m));

    block_meta_set_flag(&m, CVM_BLOCK_COALESCE_ME);
    ASSERT(block_meta_is_coalesce_me(&m));
    PASS();
}

TEST block_meta_mark_unmark_round_trip(void) {
    BlockMeta m;
    block_meta_set_flag(&m, CVM_BLOCK_SIMPLE);

    block_meta_mark(&m);
    ASSERT(block_meta_is_marked(&m));

    block_meta_unmark(&m);
    ASSERT(block_meta_is_simple(&m));
    ASSERT_FALSE(block_meta_is_marked(&m));
    PASS();
}

TEST block_meta_first_free_line_round_trip(void) {
    BlockMeta m;
    block_meta_set_flag(&m, CVM_BLOCK_SIMPLE);

    block_meta_set_first_free_line(&m, 0);
    ASSERT_EQ_FMT(0, (int)block_meta_first_free_line(&m), "%d");

    block_meta_set_first_free_line(&m, (i8)(CVM_LINE_COUNT - 1));
    ASSERT_EQ_FMT((int)CVM_LINE_COUNT - 1, (int)block_meta_first_free_line(&m), "%d");

    block_meta_set_first_free_line(&m, CVM_LAST_HOLE);
    ASSERT_EQ_FMT(CVM_LAST_HOLE, (int)block_meta_first_free_line(&m), "%d");
    PASS();
}

TEST block_meta_flag_and_size_pack_into_four_bytes(void) {
    BlockMeta m;
    block_meta_clear(&m);

    block_meta_set_flag_and_size(&m, CVM_BLOCK_SUPERBLOCK_START, 7);

    ASSERT_EQ_FMT((u32)CVM_BLOCK_SUPERBLOCK_START, (u32)m.block.superblock.flags, "%u");
    ASSERT_EQ_FMT(7u, block_meta_superblock_size(&m), "%u");

    /* the size occupies the three bytes after the flags byte */
    const u8* raw = (const u8*)&m.block;
    ASSERT_EQ_FMT((u32)CVM_BLOCK_SUPERBLOCK_START, (u32)raw[0], "%u");
    ASSERT_EQ_FMT(7u, (u32)raw[1], "%u");
    ASSERT_EQ_FMT(0u, (u32)raw[2], "%u");
    ASSERT_EQ_FMT(0u, (u32)raw[3], "%u");
    PASS();
}

TEST block_meta_superblock_size_handles_the_full_u24_range(void) {
    BlockMeta m;

    block_meta_set_flag_and_size(&m, CVM_BLOCK_SUPERBLOCK_START, 1);
    ASSERT_EQ_FMT(1u, block_meta_superblock_size(&m), "%u");

    /* the largest superblock a 24-bit field can describe */
    block_meta_set_flag_and_size(&m, CVM_BLOCK_SUPERBLOCK_START, 0xffffff);
    ASSERT_EQ_FMT(0xffffffu, block_meta_superblock_size(&m), "%u");
    PASS();
}

TEST block_meta_superblock_start_walks_back_to_the_head(void) {
    BlockMeta metas[4];
    for (int i = 0; i < 4; i++) {
        block_meta_clear(&metas[i]);
    }

    block_meta_set_flag_and_size(&metas[0], CVM_BLOCK_SUPERBLOCK_START, 3);
    block_meta_set_flag(&metas[1], CVM_BLOCK_SUPERBLOCK_TAIL);
    block_meta_set_flag(&metas[2], CVM_BLOCK_SUPERBLOCK_TAIL);
    block_meta_set_flag(&metas[3], CVM_BLOCK_SIMPLE);

    ASSERT(block_meta_superblock_start(metas, &metas[0]) == &metas[0]);
    ASSERT(block_meta_superblock_start(metas, &metas[1]) == &metas[0]);
    ASSERT(block_meta_superblock_start(metas, &metas[2]) == &metas[0]);
    PASS();
}

TEST block_meta_index_round_trips(void) {
    BlockMeta metas[8];

    for (u32 i = 0; i < 8; i++) {
        BlockMeta* m = block_meta_at(metas, i);
        ASSERT_EQ_FMT(i, block_meta_index_of(metas, m), "%u");
        ASSERT(block_meta_at_const(metas, i) == m);
    }
    PASS();
}

TEST block_address_arithmetic_needs_an_aligned_heap(void) {
    /*
     * block_start_of masks the low 15 bits, so the heap base must itself
     * be block-aligned. Heap creation guarantees this by mapping and then
     * rounding up.
     *
     * The test mimics that path rather than declaring an over-aligned
     * static buffer, because on macOS the linker refuses section
     * alignment above 16 KiB:
     *
     *   ld: reducing alignment of section __DATA,__bss from 0x8000 to
     *       0x4000 because it exceeds segment maximum alignment
     *
     * so a static buffer cannot be relied on to be block-aligned. This is
     * the same reason the real heap cannot be a static array.
     */
    usize raw_size = 3 * CVM_BLOCK_SIZE + CVM_BLOCK_SIZE;
    u8* raw = malloc(raw_size);
    ASSERT(raw != NULL);

    u8* heap = (u8*)cvm_align_up((usize)raw, CVM_BLOCK_SIZE);
    ASSERT_EQ_FMT(0u, (u32)((usize)heap & CVM_BLOCK_OFFSET_MASK), "%u");

    for (u32 i = 0; i < 3; i++) {
        u8* block = block_start_from_index(heap, i);
        ASSERT_EQ_FMT(i, block_index_of(heap, block), "%u");
        ASSERT_EQ_FMT(i, block_index_of(heap, block + 7), "%u");
        ASSERT(block_start_of(block + 7) == block);
        ASSERT(block_start_of_mut(block + 7) == block);
    }

    free(raw);
    PASS();
}

TEST block_geometry_line_addresses(void) {
    u8 block[CVM_BLOCK_SIZE];

    ASSERT(block_line_address(block, 0) == block);
    ASSERT(block_line_address(block, 1) == block + CVM_LINE_SIZE);
    ASSERT(block_line_address(block, CVM_LINE_COUNT - 1) ==
           block + (CVM_LINE_COUNT - 1) * CVM_LINE_SIZE);
    ASSERT(block_end(block) == block + CVM_BLOCK_SIZE);

    ASSERT_EQ_FMT(0u, block_line_index_of(block, block), "%u");
    ASSERT_EQ_FMT(0u, block_line_index_of(block, block + CVM_LINE_SIZE - 1), "%u");
    ASSERT_EQ_FMT(1u, block_line_index_of(block, block + CVM_LINE_SIZE), "%u");
    ASSERT_EQ_FMT((u32)CVM_LINE_COUNT - 1u,
                  block_line_index_of(block, block + CVM_BLOCK_SIZE - 1), "%u");
    PASS();
}

/* ------------------------------------------------------------------ */
/* LineMeta                                                            */
/* ------------------------------------------------------------------ */

TEST line_meta_mark_and_unmark(void) {
    LineMeta lines[CVM_LINE_COUNT];
    LineMeta* block0 = line_meta_of_block(lines, 0);

    line_meta_unmark(&block0[0]);
    ASSERT_FALSE(line_meta_is_marked(&block0[0]));

    line_meta_mark(&block0[0]);
    ASSERT(line_meta_is_marked(&block0[0]));
    ASSERT_FALSE(line_meta_is_marked(&block0[1]));

    line_meta_unmark(&block0[0]);
    ASSERT_FALSE(line_meta_is_marked(&block0[0]));
    PASS();
}

TEST line_meta_of_block_offsets_by_block(void) {
    LineMeta lines[4 * CVM_LINE_COUNT];

    ASSERT(line_meta_of_block(lines, 0) == lines);
    ASSERT(line_meta_of_block(lines, 1) == lines + CVM_LINE_COUNT);
    ASSERT(line_meta_of_block(lines, 3) == lines + 3 * CVM_LINE_COUNT);
    ASSERT(line_meta_of_block_const(lines, 2) == lines + 2 * CVM_LINE_COUNT);
    PASS();
}

/* ------------------------------------------------------------------ */
/* ObjectMeta                                                          */
/* ------------------------------------------------------------------ */

TEST object_meta_state_predicates(void) {
    ObjectMeta m;

    object_meta_set_free(&m);
    ASSERT(object_meta_is_free(&m));
    ASSERT_FALSE(object_meta_is_occupied(&m));

    object_meta_set_placeholder(&m);
    ASSERT(object_meta_is_placeholder(&m));
    ASSERT(object_meta_is_occupied(&m));

    object_meta_set_allocated(&m);
    ASSERT(object_meta_is_allocated(&m));
    ASSERT(object_meta_is_occupied(&m));

    object_meta_set_marked(&m);
    ASSERT(object_meta_is_marked(&m));
    ASSERT(object_meta_is_occupied(&m));
    /* marked is not the same state as allocated */
    ASSERT_FALSE(object_meta_is_allocated(&m));
    PASS();
}

TEST object_meta_sweep_survives_marked_and_frees_the_rest(void) {
    ObjectMeta m;

    object_meta_set_marked(&m);
    object_meta_sweep(&m);
    ASSERT(object_meta_is_allocated(&m));

    object_meta_set_allocated(&m);
    object_meta_sweep(&m);
    ASSERT(object_meta_is_free(&m));

    object_meta_set_placeholder(&m);
    object_meta_sweep(&m);
    ASSERT(object_meta_is_free(&m));

    object_meta_set_free(&m);
    object_meta_sweep(&m);
    ASSERT(object_meta_is_free(&m));
    PASS();
}

TEST object_meta_clear_line_and_block(void) {
    ObjectMeta line[CVM_SLOTS_PER_LINE];
    for (usize i = 0; i < CVM_SLOTS_PER_LINE; i++) {
        object_meta_set_allocated(&line[i]);
    }
    object_meta_clear_line(line);
    for (usize i = 0; i < CVM_SLOTS_PER_LINE; i++) {
        ASSERT(object_meta_is_free(&line[i]));
    }

    ObjectMeta block[CVM_SLOTS_PER_BLOCK];
    for (usize i = 0; i < CVM_SLOTS_PER_BLOCK; i++) {
        object_meta_set_allocated(&block[i]);
    }
    object_meta_clear_block(block);
    for (usize i = 0; i < CVM_SLOTS_PER_BLOCK; i++) {
        ASSERT(object_meta_is_free(&block[i]));
    }
    PASS();
}

TEST object_meta_sweep_line_keeps_marked_slots(void) {
    ObjectMeta line[CVM_SLOTS_PER_LINE];
    object_meta_clear_line(line);

    object_meta_set_marked(&line[3]);
    object_meta_set_allocated(&line[5]);

    object_meta_sweep_line(line);

    ASSERT(object_meta_is_allocated(&line[3]));
    ASSERT(object_meta_is_free(&line[5]));
    ASSERT(object_meta_is_free(&line[0]));
    ASSERT(object_meta_is_free(&line[CVM_SLOTS_PER_LINE - 1]));
    PASS();
}

/* ------------------------------------------------------------------ */
/* per-block accounting                                                */
/* ------------------------------------------------------------------ */

TEST metadata_cost_per_block_is_pinned(void) {
    /* 12 + 128 * 1 + 2048 = 2188 */
    ASSERT_EQ_FMT(2188u, (u32)CVM_METADATA_PER_BLOCK, "%u");
    ASSERT_EQ_FMT(32768u + 2188u, (u32)CVM_SPACE_USED_PER_BLOCK, "%u");
    PASS();
}

TEST metadata_slots_tile_lines_exactly(void) {
    ASSERT_EQ_FMT((u32)(CVM_SLOTS_PER_BLOCK / CVM_LINE_COUNT), (u32)CVM_SLOTS_PER_LINE,
                  "%u");
    ASSERT_EQ(CVM_SLOTS_PER_LINE * CVM_LINE_COUNT, CVM_SLOTS_PER_BLOCK);
    PASS();
}

SUITE(meta_suite) {
    RUN_TEST(block_meta_layout_is_pinned);
    RUN_TEST(block_meta_starts_free);
    RUN_TEST(block_meta_state_predicates_are_exact);
    RUN_TEST(block_meta_mark_unmark_round_trip);
    RUN_TEST(block_meta_first_free_line_round_trip);
    RUN_TEST(block_meta_flag_and_size_pack_into_four_bytes);
    RUN_TEST(block_meta_superblock_size_handles_the_full_u24_range);
    RUN_TEST(block_meta_superblock_start_walks_back_to_the_head);
    RUN_TEST(block_meta_index_round_trips);
    RUN_TEST(block_address_arithmetic_needs_an_aligned_heap);
    RUN_TEST(block_geometry_line_addresses);
    RUN_TEST(line_meta_mark_and_unmark);
    RUN_TEST(line_meta_of_block_offsets_by_block);
    RUN_TEST(object_meta_state_predicates);
    RUN_TEST(object_meta_sweep_survives_marked_and_frees_the_rest);
    RUN_TEST(object_meta_clear_line_and_block);
    RUN_TEST(object_meta_sweep_line_keeps_marked_slots);
    RUN_TEST(metadata_cost_per_block_is_pinned);
    RUN_TEST(metadata_slots_tile_lines_exactly);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(meta_suite);
    GREATEST_MAIN_END();
}
