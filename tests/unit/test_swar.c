/*
 * tests/unit/test_swar.c
 *
 * The SWAR predicates, on their own.
 *
 * These are eight-at-a-time operations on a byte map whose answers decide
 * which objects a sweep keeps, so the tests pin the boundary cases rather
 * than sample the behaviour: the all-zero word, the one-hot byte at each
 * position, the byte with extra bits set (a state plus its flags), and the
 * scan's head, tail and miss cases.
 *
 * One of these is here because it was gotten wrong before, in this file's
 * sibling pass: the "any byte zero" idiom was used to answer "are all bytes
 * zero", which reads a group holding one mark and seven empties as empty. A
 * predicate that takes a group and answers the wrong question is not caught
 * by measuring; it is caught by asking it the question directly.
 */

#include "vendor/greatest.h"

#include <stddef.h>
#include <string.h>

#include "base/types.h"
#include "base/swar.h"

TEST count_zero_bytes_of_an_empty_word_is_eight(void) {
    ASSERT_EQ_FMT((u8)8, rt_swar_count_zero_bytes(0), "%u");
    PASS();
}

TEST count_zero_bytes_finds_each_lane(void) {
    for (u32 lane = 0; lane < 8; lane++) {
        u64 one_bit = (u64)1 << (lane * 8);
        ASSERT_EQ_FMT((u8)lane, rt_swar_count_zero_bytes(one_bit), "%u");
    }
    PASS();
}

TEST broadcast_puts_the_byte_in_every_lane(void) {
    u64 word = rt_swar_broadcast_byte(0x3);
    for (u32 lane = 0; lane < 8; lane++) {
        ASSERT_EQ_FMT((u64)0x3, (word >> (lane * 8)) & 0xFF, "%llu");
    }
    PASS();
}

TEST match_is_bits_not_equality(void) {
    /*
     * Matching is by bits, not equality -- and the sweeper's scan depends on
     * exactly this shape. A live object's start is MARKED (0x03) or
     * PLACEHOLDER (0x01), which share bit 0; a dead object's start is
     * ALLOCATED (0x02), which has bit 1 and not bit 0. So mask 0x1 selects
     * the starts of survivors and of large objects, and skips the dead, in
     * one test -- and it must also select a survivor carrying its flags
     * (MARKED|WEAK = 0x0B, MARKED|FINALIZER = 0x13), or every weak and
     * finalizable object is invisible to the scan.
     *
     * Lane 0: MARKED|WEAK. Lane 7: MARKED|FINALIZER. Lane 1: ALLOCATED.
     */
    u64 bytes = (u64)0x0B | ((u64)0x13 << 56) | ((u64)0x02 << 8);
    u64 match = rt_swar_match_bytes_against_bits(bytes, 0x1);
    ASSERT_EQ_FMT((u64)0x1, match & 0xFF, "%llu");
    ASSERT_EQ_FMT((u64)0x1, (match >> 56) & 0xFF, "%llu");
    ASSERT_EQ_FMT((u64)0, (match >> 8) & 0xFF, "%llu");
    PASS();
}

TEST scan_finds_a_single_mark_in_a_group_of_zeroes(void) {
    /* The case the earlier hand-rolled predicate got wrong: one live slot
     * and seven empty ones must not read as empty. */
    u8 slots[8] = {0};
    slots[4] = 0x3; /* MARKED */
    ASSERT_EQ_FMT((usize)4, rt_swar_scan_for_byte_with_bits(slots, 8, 0x3), "%zu");
    PASS();
}

TEST scan_returns_limit_when_nothing_matches(void) {
    u8 slots[16];
    memset(slots, 0x2, sizeof(slots)); /* ALLOCATED: no bit 0 */
    ASSERT_EQ_FMT((usize)16, rt_swar_scan_for_byte_with_bits(slots, 16, 0x1), "%zu");
    PASS();
}

TEST scan_handles_the_unaligned_head(void) {
    u8 slots[24];
    memset(slots, 0, sizeof(slots));
    /*
     * One PLACEHOLDER just past an unaligned start: the pointer is slots+3,
     * so offset 1 is slots[4]. The head loads the group holding slots[0..7]
     * and shifts out the three bytes behind the pointer -- a mark in those
     * must not be seen, and this one must.
     */
    slots[4] = 0x1;
    ASSERT_EQ_FMT((usize)1, rt_swar_scan_for_byte_with_bits(slots + 3, 21, 0x1), "%zu");
    /* And the byte the head shifted out is genuinely invisible. */
    memset(slots, 0, sizeof(slots));
    slots[1] = 0x1; /* behind the pointer */
    ASSERT_EQ_FMT((usize)21, rt_swar_scan_for_byte_with_bits(slots + 3, 21, 0x1),
                  "%zu");
    PASS();
}

TEST scan_finds_the_last_byte_of_the_range(void) {
    u8 slots[8] = {0};
    slots[7] = 0x4; /* END, alone at the end */
    ASSERT_EQ_FMT((usize)7, rt_swar_scan_for_byte_with_bits(slots, 8, 0x4), "%zu");
    PASS();
}

TEST scan_does_not_read_past_limit(void) {
    u8 storage[24];
    memset(storage, 0, sizeof(storage));
    storage[20] = 0x3; /* past the limit we will pass */
    storage[5] = 0x3;  /* inside it */
    ASSERT_EQ_FMT((usize)5, rt_swar_scan_for_byte_with_bits(storage, 8, 0x3), "%zu");
    /* And a limit that stops before the mark finds nothing. */
    ASSERT_EQ_FMT((usize)4, rt_swar_scan_for_byte_with_bits(storage + 1, 4, 0x3),
                  "%zu");
    PASS();
}

SUITE(swar_suite) {
    RUN_TEST(count_zero_bytes_of_an_empty_word_is_eight);
    RUN_TEST(count_zero_bytes_finds_each_lane);
    RUN_TEST(broadcast_puts_the_byte_in_every_lane);
    RUN_TEST(match_is_bits_not_equality);
    RUN_TEST(scan_finds_a_single_mark_in_a_group_of_zeroes);
    RUN_TEST(scan_returns_limit_when_nothing_matches);
    RUN_TEST(scan_handles_the_unaligned_head);
    RUN_TEST(scan_finds_the_last_byte_of_the_range);
    RUN_TEST(scan_does_not_read_past_limit);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(swar_suite);
    GREATEST_MAIN_END();
}
