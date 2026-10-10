/*
 * tests/unit/test_format.c
 *
 * The bounded formatter. Two properties matter more than the exact
 * output: it must never write past the buffer, and truncation must be
 * reported rather than silently producing a shorter string that looks
 * complete.
 */

#include "vendor/greatest.h"

#include <inttypes.h>
#include <string.h>

#include "base/format.h"
#include "base/types.h"

/* A buffer with a canary after it, so an overflow is caught by the test
 * rather than by the next stack frame. */
#define GUARD 16
typedef struct {
    u8 buf[64 + GUARD];
} Guarded;

static void guard_fill(Guarded* g) {
    memset(g->buf, 0xAA, sizeof(g->buf));
}

static bool guard_intact(Guarded const* g) {
    for (usize i = 64; i < sizeof(g->buf); i++) {
        if (g->buf[i] != 0xAA)
            return false;
    }
    return true;
}

/* Convenience: format into a guarded buffer, return the produced text as
 * a NUL-terminated C string for comparison in tests only. */
static const char* fmt_cstr(Guarded* g, CvmFmt* out) {
    static char scratch[128];
    usize n = out->len < sizeof(scratch) - 1 ? out->len : sizeof(scratch) - 1;
    memcpy(scratch, g->buf, n);
    scratch[n] = '\0';
    return scratch;
}

TEST format_append_basic(void) {
    Guarded g;
    guard_fill(&g);
    CvmFmt f;
    fmt_init(&f, g.buf, 64);

    fmt_lit(&f, "hello ");
    fmt_lit(&f, "world");

    ASSERT(guard_intact(&g));
    ASSERT_FALSE(f.truncated);
    ASSERT_EQ_FMT(11u, (u32)f.len, "%u");
    ASSERT_STR_EQ("hello world", fmt_cstr(&g, &f));
    PASS();
}

TEST format_unsigned_decimal(void) {
    Guarded g;
    guard_fill(&g);
    CvmFmt f;
    fmt_init(&f, g.buf, 64);

    fmt_u64(&f, 0);
    fmt_lit(&f, ",");
    fmt_u64(&f, 7);
    fmt_lit(&f, ",");
    fmt_u64(&f, (u64)1234567890);
    fmt_lit(&f, ",");
    fmt_u64(&f, (u64)18446744073709551615ull); /* u64 max */

    ASSERT(guard_intact(&g));
    ASSERT_STR_EQ("0,7,1234567890,18446744073709551615", fmt_cstr(&g, &f));
    PASS();
}

TEST format_signed_decimal_including_the_minimum(void) {
    Guarded g;
    guard_fill(&g);
    CvmFmt f;
    fmt_init(&f, g.buf, 64);

    fmt_i64(&f, 0);
    fmt_lit(&f, ",");
    fmt_i64(&f, -1);
    fmt_lit(&f, ",");
    fmt_i64(&f, -9223372036854775807ll - 1); /* i64 min */
    fmt_lit(&f, ",");
    fmt_i64(&f, 9223372036854775807ll); /* i64 max */

    ASSERT(guard_intact(&g));
    /* the minimum must not be mangled: negating it directly would overflow */
    ASSERT_STR_EQ("0,-1,-9223372036854775808,9223372036854775807", fmt_cstr(&g, &f));
    PASS();
}

TEST format_hex(void) {
    Guarded g;
    guard_fill(&g);
    CvmFmt f;
    fmt_init(&f, g.buf, 64);

    fmt_hex(&f, 0);
    fmt_lit(&f, ",");
    fmt_hex(&f, 0xdeadbeefull);
    fmt_lit(&f, ",");
    fmt_hex(&f, 0xffffffffffffffffull);

    ASSERT(guard_intact(&g));
    ASSERT_STR_EQ("0x0,0xdeadbeef,0xffffffffffffffff", fmt_cstr(&g, &f));
    PASS();
}

TEST format_hex_padded_is_fixed_width(void) {
    Guarded g;
    guard_fill(&g);
    CvmFmt f;
    fmt_init(&f, g.buf, 64);

    fmt_hex_padded(&f, 0x1234ull, 8);
    fmt_lit(&f, "|");
    fmt_hex_padded(&f, (u64)0, 8);
    fmt_lit(&f, "|");
    /* digits beyond 16 are clamped rather than reading past the value */
    fmt_hex_padded(&f, 0xabcdull, 99);

    ASSERT(guard_intact(&g));
    ASSERT_STR_EQ("0x00001234|0x00000000|0x000000000000abcd", fmt_cstr(&g, &f));
    PASS();
}

TEST format_truncation_is_flagged_not_silent(void) {
    Guarded g;
    guard_fill(&g);
    CvmFmt f;
    fmt_init(&f, g.buf, 8);

    fmt_lit(&f, "0123456789abcdef");

    ASSERT(guard_intact(&g)); /* nothing past the cap was written */
    ASSERT(f.truncated);
    ASSERT_EQ_FMT(8u, (u32)f.len, "%u");
    ASSERT_STR_EQ("01234567", fmt_cstr(&g, &f));
    PASS();
}

TEST format_truncation_at_exact_capacity(void) {
    Guarded g;
    guard_fill(&g);
    CvmFmt f;
    fmt_init(&f, g.buf, 8);

    fmt_lit(&f, "01234567"); /* exactly fits */

    ASSERT_FALSE(f.truncated);
    ASSERT_EQ_FMT(8u, (u32)f.len, "%u");

    fmt_lit(&f, "x"); /* one byte over */
    ASSERT(f.truncated);
    ASSERT_EQ_FMT(8u, (u32)f.len, "%u");
    ASSERT(guard_intact(&g));
    PASS();
}

TEST format_zero_capacity_writes_nothing(void) {
    Guarded g;
    guard_fill(&g);
    CvmFmt f;
    fmt_init(&f, g.buf, 0);

    fmt_lit(&f, "anything");
    fmt_u64(&f, 12345);

    ASSERT(guard_intact(&g));
    ASSERT(f.truncated);
    ASSERT_EQ_FMT(0u, (u32)f.len, "%u");
    PASS();
}

TEST format_str_handles_non_terminated_text(void) {
    Guarded g;
    guard_fill(&g);
    CvmFmt f;
    fmt_init(&f, g.buf, 64);

    /* text with no NUL anywhere, as `str` guarantees */
    static const u8 raw[] = {'a', 'b', 'c'};
    fmt_str(&f, (str){raw, sizeof(raw)});

    ASSERT_STR_EQ("abc", fmt_cstr(&g, &f));
    ASSERT(guard_intact(&g));
    PASS();
}

TEST format_bytes_and_bool(void) {
    Guarded g;
    guard_fill(&g);
    CvmFmt f;
    fmt_init(&f, g.buf, 64);

    fmt_bool(&f, true);
    fmt_lit(&f, ",");
    fmt_bool(&f, false);
    fmt_lit(&f, ",");
    static const u8 raw[] = {0x41, 0x42};
    fmt_bytes(&f, (span_const){raw, sizeof(raw)});

    ASSERT_STR_EQ("true,false,AB", fmt_cstr(&g, &f));
    ASSERT(guard_intact(&g));
    PASS();
}

TEST format_a_realistic_log_line_fits(void) {
    /* The shape the collector actually emits, to keep the sizing honest. */
    Guarded g;
    guard_fill(&g);
    CvmFmt f;
    fmt_init(&f, g.buf, 64);

    fmt_lit(&f, "block ");
    fmt_u32(&f, 12345);
    fmt_lit(&f, " at ");
    fmt_hex_padded(&f, 0x7f0000001234ull, 12);
    fmt_lit(&f, " marked");

    ASSERT(guard_intact(&g));
    ASSERT_FALSE(f.truncated);
    ASSERT_STR_EQ("block 12345 at 0x7f0000001234 marked", fmt_cstr(&g, &f));
    PASS();
}

TEST fmt_u64_matches_printf_over_a_wide_range(void) {
    /*
     * The table-based path is compared against the library's own formatting, over
     * the boundaries where a two-digits-at-a-time loop goes wrong: the ends of a
     * pair, the first value with an odd number of digits, and the top of the
     * range. The library is the reference, and it is available because this test
     * runs hosted.
     */
    static u64 const boundaries[] = {
        (u64)0,
        (u64)1,
        (u64)9,
        (u64)10,
        (u64)11,
        (u64)99,
        (u64)100,
        (u64)101,
        (u64)999,
        (u64)1000,
        (u64)12345,
        (u64)1000000,
        (u64)4294967295,
        (u64)4294967296,
        (u64)9999999999999999999ull,
        (u64)18446744073709551615ull,
    };

    for (usize i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
        char buf[32];
        CvmFmt f;
        fmt_init(&f, (u8*)buf, sizeof(buf));
        fmt_u64(&f, boundaries[i]);

        char want[32];
        snprintf(want, sizeof(want), "%" PRIu64, (u64)boundaries[i]);

        usize n = 0;
        while (want[n] != '\0') {
            n++;
        }
        str got = fmt_result(&f);
        if (got.len != n || memcmp(got.ptr, want, n) != 0) {
            fprintf(stderr, "%" PRIu64 ": got %.*s, wanted %s\n", (u64)boundaries[i],
                    (int)got.len, (char*)got.ptr, want);
            FAILm("the conversion disagrees with printf");
        }
    }

    /* and a sweep that hits both even and odd digit counts, plus the carries */
    u64 v = (u64)1;
    for (u32 i = 0; i < 2000u; i++) {
        v = v * (u64)6364136223846793005ull + (u64)1442695040888963407ull;
        char buf[32];
        CvmFmt f;
        fmt_init(&f, (u8*)buf, sizeof(buf));
        fmt_u64(&f, v);

        char want[32];
        snprintf(want, sizeof(want), "%" PRIu64, (u64)v);
        str got = fmt_result(&f);
        usize n = 0;
        while (want[n] != '\0') {
            n++;
        }
        if (got.len != n || memcmp(got.ptr, want, n) != 0) {
            fprintf(stderr, "%" PRIu64 ": a sweep value disagreed\n", (u64)v);
            FAILm("the conversion disagrees with printf on a random value");
        }
    }
    PASS();
}

SUITE(format_suite) {
    RUN_TEST(fmt_u64_matches_printf_over_a_wide_range);
    RUN_TEST(format_append_basic);
    RUN_TEST(format_unsigned_decimal);
    RUN_TEST(format_signed_decimal_including_the_minimum);
    RUN_TEST(format_hex);
    RUN_TEST(format_hex_padded_is_fixed_width);
    RUN_TEST(format_truncation_is_flagged_not_silent);
    RUN_TEST(format_truncation_at_exact_capacity);
    RUN_TEST(format_zero_capacity_writes_nothing);
    RUN_TEST(format_str_handles_non_terminated_text);
    RUN_TEST(format_bytes_and_bool);
    RUN_TEST(format_a_realistic_log_line_fits);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(format_suite);
    GREATEST_MAIN_END();
}
