/*
 * tests/unit/test_math.c
 *
 * Integer math used by allocator and heap geometry. The interesting
 * cases are the boundaries: powers of two exactly, one below, one above,
 * and the top of the word.
 */

#include "vendor/greatest.h"

#include "base/math.h"
#include "base/types.h"

#define U(x) ((unsigned long long)(x))

TEST log2_floor_exact_powers(void) {
    ASSERT_EQ_FMT(U(0), U(cvm_log2_floor(1)), "%llu");
    ASSERT_EQ_FMT(U(1), U(cvm_log2_floor(2)), "%llu");
    ASSERT_EQ_FMT(U(2), U(cvm_log2_floor(4)), "%llu");
    ASSERT_EQ_FMT(U(7), U(cvm_log2_floor(128)), "%llu");
    ASSERT_EQ_FMT(U(8), U(cvm_log2_floor(256)), "%llu");
    ASSERT_EQ_FMT(U(32), U(cvm_log2_floor((usize)1 << 32)), "%llu");
    ASSERT_EQ_FMT(U(63), U(cvm_log2_floor((usize)1 << 63)), "%llu");
    PASS();
}

TEST log2_floor_between_powers(void) {
    ASSERT_EQ_FMT(U(1), U(cvm_log2_floor(3)), "%llu");
    ASSERT_EQ_FMT(U(7), U(cvm_log2_floor(255)), "%llu");
    ASSERT_EQ_FMT(U(8), U(cvm_log2_floor(257)), "%llu");
    ASSERT_EQ_FMT(U(31), U(cvm_log2_floor(((usize)1 << 32) - 1)), "%llu");
    ASSERT_EQ_FMT(U(63), U(cvm_log2_floor((usize)-1)), "%llu");
    PASS();
}

TEST log2_ceil_boundaries(void) {
    ASSERT_EQ_FMT(U(0), U(cvm_log2_ceil(1)), "%llu");
    ASSERT_EQ_FMT(U(1), U(cvm_log2_ceil(2)), "%llu");
    /* one above a power of two must round up to the next exponent */
    ASSERT_EQ_FMT(U(2), U(cvm_log2_ceil(3)), "%llu");
    ASSERT_EQ_FMT(U(2), U(cvm_log2_ceil(4)), "%llu");
    ASSERT_EQ_FMT(U(3), U(cvm_log2_ceil(5)), "%llu");
    ASSERT_EQ_FMT(U(8), U(cvm_log2_ceil(255)), "%llu");
    ASSERT_EQ_FMT(U(8), U(cvm_log2_ceil(256)), "%llu");
    ASSERT_EQ_FMT(U(9), U(cvm_log2_ceil(257)), "%llu");
    PASS();
}

TEST log2_ceil_does_not_overflow_at_the_top(void) {
    /* the reference implementation (floor(2v - 1)) overflows here */
    ASSERT_EQ_FMT(U(63), U(cvm_log2_ceil((usize)1 << 63)), "%llu");
    ASSERT_EQ_FMT(U(64), U(cvm_log2_ceil((usize)-1)), "%llu");
    PASS();
}

TEST round_up_and_div_round_up(void) {
    ASSERT_EQ_FMT(U(0), U(cvm_round_up(0, 16)), "%llu");
    ASSERT_EQ_FMT(U(16), U(cvm_round_up(1, 16)), "%llu");
    ASSERT_EQ_FMT(U(16), U(cvm_round_up(16, 16)), "%llu");
    ASSERT_EQ_FMT(U(32), U(cvm_round_up(17, 16)), "%llu");

    ASSERT_EQ_FMT(U(0), U(cvm_div_round_up(0, 16)), "%llu");
    ASSERT_EQ_FMT(U(1), U(cvm_div_round_up(1, 16)), "%llu");
    ASSERT_EQ_FMT(U(1), U(cvm_div_round_up(16, 16)), "%llu");
    ASSERT_EQ_FMT(U(2), U(cvm_div_round_up(17, 16)), "%llu");
    PASS();
}

TEST round_up_pow2(void) {
    ASSERT_EQ_FMT(U(1), U(cvm_round_up_pow2(1)), "%llu");
    ASSERT_EQ_FMT(U(2), U(cvm_round_up_pow2(2)), "%llu");
    ASSERT_EQ_FMT(U(4), U(cvm_round_up_pow2(3)), "%llu");
    ASSERT_EQ_FMT(U(8), U(cvm_round_up_pow2(5)), "%llu");
    ASSERT_EQ_FMT(U(8), U(cvm_round_up_pow2(8)), "%llu");
    ASSERT_EQ_FMT(U((unsigned long long)1 << 32),
                  U(cvm_round_up_pow2(((usize)1 << 32) - 1)), "%llu");
    PASS();
}

TEST align_helpers_are_the_power_of_two_fast_path(void) {
    /* base/types.h: mask-based, power-of-two only */
    ASSERT_EQ_FMT(U(0), U(cvm_align_up(0, 16)), "%llu");
    ASSERT_EQ_FMT(U(16), U(cvm_align_up(1, 16)), "%llu");
    ASSERT_EQ_FMT(U(16), U(cvm_align_up(16, 16)), "%llu");
    ASSERT_EQ_FMT(U(32), U(cvm_align_up(17, 16)), "%llu");

    ASSERT(cvm_is_aligned(0, 16));
    ASSERT(cvm_is_aligned(16, 16));
    ASSERT_FALSE(cvm_is_aligned(17, 16));
    PASS();
}

TEST round_up_agrees_with_align_up_on_powers_of_two(void) {
    for (usize align = 1; align <= 256; align <<= 1) {
        for (usize v = 0; v <= 512; v++) {
            ASSERT_EQ_FMT(U(cvm_align_up(v, align)), U(cvm_round_up(v, align)), "%llu");
        }
    }
    PASS();
}

SUITE(math_suite) {
    RUN_TEST(log2_floor_exact_powers);
    RUN_TEST(log2_floor_between_powers);
    RUN_TEST(log2_ceil_boundaries);
    RUN_TEST(log2_ceil_does_not_overflow_at_the_top);
    RUN_TEST(round_up_and_div_round_up);
    RUN_TEST(round_up_pow2);
    RUN_TEST(align_helpers_are_the_power_of_two_fast_path);
    RUN_TEST(round_up_agrees_with_align_up_on_powers_of_two);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(math_suite);
    GREATEST_MAIN_END();
}
