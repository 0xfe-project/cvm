/*
 * tests/unit/test_u24.c
 *
 * u24 exists because block metadata must be byte-identical across
 * compilers (MSVC packs bitfields differently). These tests pin the
 * contract: three bytes, little-endian, round-trip safe.
 */

#include "vendor/greatest.h"

#include "base/types.h"

TEST u24_is_three_bytes(void) {
    ASSERT_EQ_FMT(3u, (u32)sizeof(u24), "%u");
    PASS();
}

TEST u24_round_trips(void) {
    /* boundary values plus a few interior ones */
    const u32 cases[] = {
        0x000000u, 0x000001u, 0x0000ffu, 0x000100u,
        0x00ffffu, 0x010000u, 0x123456u, 0xffffffu,
    };

    for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        u32 v = cases[i];
        ASSERT_EQ_FMT(v, u24_to_u32(u24_from_u32(v)), "0x%06x");
    }
    PASS();
}

TEST u24_is_little_endian_on_the_wire(void) {
    /* bytes[0] is the least significant byte, on every host */
    u24 v = u24_from_u32(0x123456u);
    ASSERT_EQ_FMT(0x56u, (u32)v.bytes[0], "0x%02x");
    ASSERT_EQ_FMT(0x34u, (u32)v.bytes[1], "0x%02x");
    ASSERT_EQ_FMT(0x12u, (u32)v.bytes[2], "0x%02x");
    PASS();
}

TEST u24_truncates_above_24_bits(void) {
    /* only the low three bytes survive; this is the documented behaviour */
    ASSERT_EQ_FMT(0x000000u, u24_to_u32(u24_from_u32(0xff000000u)), "0x%06x");
    ASSERT_EQ_FMT(0x345678u, u24_to_u32(u24_from_u32(0xaa345678u)), "0x%06x");
    PASS();
}

TEST u24_add_wraps_within_24_bits(void) {
    ASSERT_EQ_FMT(0x00000au, u24_to_u32(u24_add(u24_from_u32(5), 5)), "0x%06x");
    /* 0xffffff + 1 wraps to 0, matching the three-byte storage */
    ASSERT_EQ_FMT(0x000000u, u24_to_u32(u24_add(u24_from_u32(0xffffffu), 1)), "0x%06x");
    ASSERT_EQ_FMT(0xfffffeu, u24_to_u32(u24_add(u24_from_u32(0xffffffu), -1)),
                  "0x%06x");
    PASS();
}

TEST u24_eq_compares_all_three_bytes(void) {
    ASSERT(u24_eq(u24_from_u32(0x123456u), u24_from_u32(0x123456u)));
    ASSERT_FALSE(u24_eq(u24_from_u32(0x123456u), u24_from_u32(0x123457u)));
    /* a difference only in the top byte must still be seen */
    ASSERT_FALSE(u24_eq(u24_from_u32(0x123456u), u24_from_u32(0x023456u)));
    PASS();
}

TEST u24_overlaid_with_u8_flags_has_stable_layout(void) {
    /*
     * The shape the GC actually uses: a flags byte followed by a u24.
     * A 4-byte store over this struct must land flags in byte 0 and the
     * little-endian u24 in bytes 1..3, on every platform.
     */
    struct combined {
        u8 flags;
        u24 size;
    } c;

    _Static_assert(sizeof(struct combined) == 4, "flags + u24 must be 4 bytes");

    c.flags = 0x07;
    c.size = u24_from_u32(0x00abcd);

    const u8* raw = (const u8*)&c;
    ASSERT_EQ_FMT(0x07u, (u32)raw[0], "0x%02x");
    ASSERT_EQ_FMT(0xcdu, (u32)raw[1], "0x%02x");
    ASSERT_EQ_FMT(0xabu, (u32)raw[2], "0x%02x");
    ASSERT_EQ_FMT(0x00u, (u32)raw[3], "0x%02x");
    PASS();
}

SUITE(u24_suite) {
    RUN_TEST(u24_is_three_bytes);
    RUN_TEST(u24_round_trips);
    RUN_TEST(u24_is_little_endian_on_the_wire);
    RUN_TEST(u24_truncates_above_24_bits);
    RUN_TEST(u24_add_wraps_within_24_bits);
    RUN_TEST(u24_eq_compares_all_three_bytes);
    RUN_TEST(u24_overlaid_with_u8_flags_has_stable_layout);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(u24_suite);
    GREATEST_MAIN_END();
}
