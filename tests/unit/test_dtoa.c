/*
 * tests/unit/test_dtoa.c
 *
 * f64 <-> text, and the integer conversions next to it.
 *
 * The unit cases pin spelling and the flag semantics. The property cases are
 * the ones that matter, and they work the way bench/dtoa/README.md argues:
 * `strtod` is used as a correctly-rounded oracle, because an implementation
 * that round-trips through itself can be wrong in a matching way in both
 * directions. That is not a hypothetical here — the benchmark found an
 * implementation that does exactly that on 9% of ordinary JSON numbers.
 *
 * The corpus is smaller than the benchmark's on purpose: this runs in the
 * test suite on every build, in four configurations.
 */

#include "vendor/greatest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/types.h"
#include "str/dtoa.h"

#define FREE (CVM_DTOA_FORMAT_FREE | CVM_DTOA_MINUS_ZERO)

static i32 fmt(f64 d, u8* buf, u32 flags) {
    CvmDtoaTempMem tmp;
    return cvm_dtoa(buf, d, 10, 0, flags, &tmp);
}

/*
 * The text a conversion produced, as a NUL-terminated C string, for comparing
 * against a literal and for handing to strtod.
 *
 * Returns false rather than asserting: greatest's ASSERT macros return from the
 * enclosing function, which a helper returning void cannot do.
 */
static bool fmt_cstr(f64 d, u32 flags, char* out, usize cap) {
    u8 buf[CVM_DTOA_MAX_DIGITS + 24];
    i32 n = fmt(d, buf, flags);
    if (n <= 0 || (usize)n >= cap) {
        return false;
    }
    memcpy(out, buf, (usize)n);
    out[n] = '\0';
    return true;
}

static bool text_is(f64 d, u32 flags, char const* want) {
    char got[CVM_DTOA_MAX_DIGITS + 24];
    if (!fmt_cstr(d, flags, got, sizeof(got))) {
        return false;
    }
    return strcmp(got, want) == 0;
}

static f64 bits_to_f64(u64 bits) {
    f64 d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

static u64 f64_to_bits(f64 d) {
    u64 bits;
    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

static bool finite(f64 d) {
    return (f64_to_bits(d) & 0x7ff0000000000000ull) != 0x7ff0000000000000ull;
}

/* ------------------------------------------------------------------ */
/* spelling                                                            */
/* ------------------------------------------------------------------ */

TEST free_format_spells_the_shortest_exact_form(void) {
    ASSERT(text_is(0.0, FREE, "0"));
    ASSERT(text_is(-0.0, FREE, "-0"));
    ASSERT(text_is(1.0, FREE, "1"));
    ASSERT(text_is(-1.0, FREE, "-1"));
    ASSERT(text_is(0.1, FREE, "0.1"));
    ASSERT(text_is(1.0 / 3.0, FREE, "0.3333333333333333"));
    /* The threshold where the spelling flips to exponent form. */
    ASSERT(text_is(1e21, FREE, "1e+21"));
    ASSERT(text_is(1e-7, FREE, "1e-7"));
    /* The whole range, including both ends of the denormals. */
    ASSERT(text_is(5e-324, FREE, "5e-324"));
    ASSERT(text_is(1.7976931348623157e308, FREE, "1.7976931348623157e+308"));
    /* Exact integers keep their digits rather than going exponential. */
    ASSERT(text_is(9007199254740992.0, FREE, "9007199254740992"));
    PASS();
}

TEST the_minus_zero_flag_is_what_brings_the_sign_back(void) {
    u8 with[CVM_DTOA_MAX_DIGITS];
    u8 without[CVM_DTOA_MAX_DIGITS];
    CvmDtoaTempMem tmp;

    i32 a = cvm_dtoa(with, -0.0, 10, 0, FREE, &tmp);
    i32 b = cvm_dtoa(without, -0.0, 10, 0, CVM_DTOA_FORMAT_FREE, &tmp);

    ASSERT_EQ_FMT(2, a, "%d");
    ASSERT_EQ_FMT(1, b, "%d");
    ASSERT_EQ_FMT('0', (int)without[0], "%c");
    PASS();
}

TEST non_finite_values_use_the_javascript_spelling(void) {
    /* Not a libc "inf"/"nan": these writers are also the ones JSON and the
     * language's own number-to-string go through, and there the spelling is
     * part of the output. */
    ASSERT(text_is(1.0 / 0.0, FREE, "Infinity"));
    ASSERT(text_is(-1.0 / 0.0, FREE, "-Infinity"));
    ASSERT(text_is(0.0 / 0.0, FREE, "NaN"));
    PASS();
}

TEST explicit_formats_and_exponents(void) {
    /* The helper above always passes n_digits = 0, so the formats that use
     * n_digits go through cvm_dtoa directly. */
    u8 buf[CVM_DTOA_MAX_DIGITS + 24];
    CvmDtoaTempMem tmp;

    i32 n = cvm_dtoa(buf, 1.2345, 10, 3, CVM_DTOA_FORMAT_FIXED, &tmp);
    ASSERT_EQ_FMT(4, n, "%d");
    ASSERT(memcmp(buf, "1.23", 4) == 0);

    n = cvm_dtoa(buf, 1.2345, 10, 1, CVM_DTOA_FORMAT_FIXED, &tmp);
    ASSERT(memcmp(buf, "1", (usize)n) == 0);

    /* Exponent forced off: the digits are all written out. */
    ASSERT(text_is(1e21, CVM_DTOA_FORMAT_FREE | CVM_DTOA_EXP_DISABLED,
                   "1000000000000000000000"));
    /* Exponent forced on, even for a value that needs none. */
    ASSERT(text_is(1.0, CVM_DTOA_FORMAT_FREE | CVM_DTOA_EXP_ENABLED, "1e+0"));
    PASS();
}

TEST max_len_bounds_the_write(void) {
    /*
     * The contract is that a caller can size a buffer from max_len alone. Check
     * both halves: the length never exceeds the bound, and the byte just past
     * it is not touched.
     */
    static f64 const cases[] = {
        0.0,
        -0.0,
        1.0,
        0.1,
        1e21,
        1e-7,
        5e-324,
        1.7976931348623157e308,
        9007199254740992.0,
        2.2250738585072014e-308,
        0.0 / 0.0,
        1.0 / 0.0,
    };

    for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CvmDtoaTempMem tmp;
        i32 bound = cvm_dtoa_max_len(cases[i], 10, 0, FREE);
        ASSERT(bound > 0);

        u8 buf[128];
        ASSERT((usize)bound + 1u <= sizeof(buf));
        memset(buf, 0xAA, sizeof(buf));

        i32 n = cvm_dtoa(buf, cases[i], 10, 0, FREE, &tmp);
        ASSERT(n > 0);
        ASSERT(n <= bound);
        ASSERT_EQ_FMT((int)0xAA, (int)buf[(usize)bound], "0x%02x");
    }
    PASS();
}

/* ------------------------------------------------------------------ */
/* integers                                                            */
/* ------------------------------------------------------------------ */

TEST integer_conversions_match_the_libc_ones(void) {
    /* printf is an independent formatter for plain decimal, and this runs
     * hosted, so it can be the oracle. */
    u8 buf[64];
    char want[64];

    static u64 const uvals[] = {0ull,
                                1ull,
                                9ull,
                                10ull,
                                99ull,
                                100ull,
                                4294967295ull,
                                4294967296ull,
                                12345678901234567890ull,
                                18446744073709551615ull};
    for (usize i = 0; i < sizeof(uvals) / sizeof(uvals[0]); i++) {
        usize n = cvm_u64toa(buf, uvals[i]);
        snprintf(want, sizeof(want), "%llu", (unsigned long long)uvals[i]);
        ASSERT_EQ_FMT(strlen(want), n, "%zu");
        ASSERT(memcmp(buf, want, n) == 0);

        n = cvm_u32toa(buf, (u32)uvals[i]);
        snprintf(want, sizeof(want), "%u", (unsigned)(u32)uvals[i]);
        ASSERT_EQ_FMT(strlen(want), n, "%zu");
        ASSERT(memcmp(buf, want, n) == 0);
    }

    static i64 const ivals[] = {0,
                                1,
                                -1,
                                9,
                                -9,
                                2147483647,
                                -2147483648,
                                9223372036854775807ll,
                                -9223372036854775807ll - 1};
    for (usize i = 0; i < sizeof(ivals) / sizeof(ivals[0]); i++) {
        usize n = cvm_i64toa(buf, ivals[i]);
        snprintf(want, sizeof(want), "%lld", (long long)ivals[i]);
        ASSERT_EQ_FMT(strlen(want), n, "%zu");
        ASSERT(memcmp(buf, want, n) == 0);

        n = cvm_i32toa(buf, (i32)ivals[i]);
        snprintf(want, sizeof(want), "%d", (int)(i32)ivals[i]);
        ASSERT_EQ_FMT(strlen(want), n, "%zu");
        ASSERT(memcmp(buf, want, n) == 0);
    }
    PASS();
}

TEST radix_conversions_read_back(void) {
    u8 buf[80];

    struct {
        u64 v;
        u32 radix;
        char const* want;
    } const cases[] = {
        {0ull, 16, "0"},         {255ull, 16, "ff"},
        {255ull, 2, "11111111"}, {255ull, 36, "73"},
        {35ull, 36, "z"},        {16ull, 16, "10"},
        {1ull, 2, "1"},          {4294967295ull, 16, "ffffffff"},
    };

    for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        usize n = cvm_u64toa_radix(buf, cases[i].v, cases[i].radix);
        ASSERT_EQ_FMT(strlen(cases[i].want), n, "%zu");
        ASSERT(memcmp(buf, cases[i].want, n) == 0);

        /* and back through the parser, at the same radix */
        CvmAtodTempMem tmp;
        usize consumed = 0;
        str s = {.ptr = buf, .len = n};
        f64 back = cvm_atod(s, &consumed, (i32)cases[i].radix, 0, &tmp);
        ASSERT_EQ_FMT(n, consumed, "%zu");
        ASSERT_EQ_FMT((f64)cases[i].v, back, "%.0f");
    }

    /* A negative, which carries the sign in front of the digits. */
    usize n = cvm_i64toa_radix(buf, -255, 16);
    ASSERT_EQ_FMT((usize)3, n, "%zu");
    ASSERT(memcmp(buf, "-ff", 3) == 0);
    PASS();
}

/* ------------------------------------------------------------------ */
/* parsing                                                             */
/* ------------------------------------------------------------------ */

static f64 parse_cstr(char const* text, i32 radix, u32 flags, usize* consumed) {
    CvmAtodTempMem tmp;
    usize len = strlen(text);
    str s = {.ptr = (u8 const*)text, .len = len};
    return cvm_atod(s, consumed, radix, flags, &tmp);
}

TEST parse_takes_the_number_at_the_front(void) {
    usize consumed = 0;

    ASSERT_EQ_FMT(123.0, parse_cstr("123abc", 10, 0, &consumed), "%.0f");
    ASSERT_EQ_FMT((usize)3, consumed, "%zu");

    ASSERT_EQ_FMT(1500.0, parse_cstr("1.5e3", 10, 0, &consumed), "%.0f");
    ASSERT_EQ_FMT((usize)5, consumed, "%zu");

    ASSERT_EQ_FMT(-0.0, parse_cstr("-0", 10, 0, &consumed), "%.0f");
    ASSERT_EQ_FMT((usize)2, consumed, "%zu");
    /* the sign survives, which is the half of the round trip a sign-losing
     * parser gets wrong */
    ASSERT(f64_to_bits(parse_cstr("-0", 10, 0, &consumed)) == f64_to_bits(-0.0));

    ASSERT_EQ_FMT(0.0, parse_cstr("0.0", 10, 0, &consumed), "%.0f");
    ASSERT_EQ_FMT(0.5, parse_cstr(".5", 10, 0, &consumed), "%.1f");
    ASSERT_EQ_FMT(5.0, parse_cstr("5.", 10, 0, &consumed), "%.0f");
    PASS();
}

TEST parse_reports_where_a_number_ends_and_where_it_never_started(void) {
    usize consumed = 0;

    /* Not a number at all: nothing consumed, so a caller can say so. */
    (void)parse_cstr("abc", 10, 0, &consumed);
    ASSERT_EQ_FMT((usize)0, consumed, "%zu");

    /* Leading whitespace is not skipped: that belongs to the grammar above,
     * which knows where a token may begin. */
    (void)parse_cstr(" 12", 10, 0, &consumed);
    ASSERT_EQ_FMT((usize)0, consumed, "%zu");

    /* Started like a number and then ran out: consumed is the part that
     * matched, and the value is NaN. */
    f64 v = parse_cstr("1e", 10, 0, &consumed);
    ASSERT_EQ_FMT((usize)2, consumed, "%zu");
    ASSERT(v != v);

    v = parse_cstr("1e+", 10, 0, &consumed);
    ASSERT_EQ_FMT((usize)3, consumed, "%zu");
    ASSERT(v != v);
    PASS();
}

TEST parse_radix_detection_and_separators(void) {
    usize consumed = 0;
    u32 flags = CVM_ATOD_ACCEPT_BIN_OCT | CVM_ATOD_ACCEPT_UNDERSCORES;

    /* radix 0 means detect */
    ASSERT_EQ_FMT(31.0, parse_cstr("0x1f", 0, flags, &consumed), "%.0f");
    ASSERT_EQ_FMT((usize)4, consumed, "%zu");
    ASSERT_EQ_FMT(8.0, parse_cstr("0o10", 0, flags, &consumed), "%.0f");
    ASSERT_EQ_FMT(2.0, parse_cstr("0b10", 0, flags, &consumed), "%.0f");
    ASSERT_EQ_FMT(10.0, parse_cstr("10", 0, flags, &consumed), "%.0f");

    /* underscores are accepted only when asked for */
    ASSERT_EQ_FMT(1000.0, parse_cstr("1_000", 0, flags, &consumed), "%.0f");
    ASSERT_EQ_FMT((usize)5, consumed, "%zu");

    /* and a radix-16 spelling reads back at radix 16 without a prefix */
    ASSERT_EQ_FMT(255.0, parse_cstr("ff", 16, 0, &consumed), "%.0f");
    PASS();
}

TEST int_only_refuses_the_fractional_and_exponential_parts(void) {
    usize consumed = 0;

    ASSERT_EQ_FMT(12.0, parse_cstr("12.5", 10, CVM_ATOD_INT_ONLY, &consumed), "%.0f");
    ASSERT_EQ_FMT((usize)2, consumed, "%zu");

    ASSERT_EQ_FMT(12.0, parse_cstr("12e3", 10, CVM_ATOD_INT_ONLY, &consumed), "%.0f");
    ASSERT_EQ_FMT((usize)2, consumed, "%zu");
    PASS();
}

/* ------------------------------------------------------------------ */
/* the property that matters                                           */
/* ------------------------------------------------------------------ */

static u64 rng_state = 0x243f6a8885a308d3ull;

static u64 splitmix64(void) {
    u64 z = (rng_state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/*
 * Both directions, judged by strtod.
 *
 * A formatter and a parser that agree with each other prove nothing, so:
 *
 *   - the text our formatter writes must read back, through *strtod*, as the
 *     same bits
 *   - text written by *snprintf*, which is not our code, must parse through
 *     our parser to the same bits
 *   - and the round trip through ours is checked too, because that is what a
 *     program does
 */
TEST every_value_survives_both_directions(void) {
    enum { N = 6000 };
    char text[CVM_DTOA_MAX_DIGITS + 24];
    u8 buf[CVM_DTOA_MAX_DIGITS + 24];
    CvmDtoaTempMem dtmp;

    usize checked = 0;

    for (usize i = 0; i < N; i++) {
        f64 d = (i < 24) ? bits_to_f64(0x0010000000000001ull * (u64)i)
                         : bits_to_f64(splitmix64());
        if (!finite(d)) {
            continue;
        }
        checked++;

        i32 n = cvm_dtoa(buf, d, 10, 0, FREE, &dtmp);
        ASSERT(n > 0);
        memcpy(text, buf, (usize)n);
        text[n] = '\0';

        /* (1) our text, independently parsed */
        f64 via_libc = strtod(text, NULL);
        if (f64_to_bits(via_libc) != f64_to_bits(d)) {
            fprintf(stderr, "fmt: %.17g -> \"%s\" -> %.17g\n", d, text, via_libc);
            FAILm("our text did not read back through strtod");
        }

        /* (2) our parser, on text we did not write */
        char from_libc[CVM_DTOA_MAX_DIGITS + 24];
        snprintf(from_libc, sizeof(from_libc), "%.17g", d);
        usize consumed = 0;
        CvmAtodTempMem atmp;
        str s = {.ptr = (u8 const*)from_libc, .len = strlen(from_libc)};
        f64 parsed = cvm_atod(s, &consumed, 10, 0, &atmp);
        if (f64_to_bits(parsed) != f64_to_bits(d)) {
            fprintf(stderr, "parse: \"%s\" -> %.17g, want %.17g\n", from_libc, parsed,
                    d);
            FAILm("our parser did not read what it was given");
        }
        if (consumed != s.len) {
            fprintf(stderr, "parse: \"%s\" consumed %zu of %zu\n", from_libc, consumed,
                    s.len);
            FAILm("our parser stopped early");
        }

        /* (3) the round trip a program performs */
        f64 back = parse_cstr(text, 10, 0, &consumed);
        if (consumed != (usize)n || f64_to_bits(back) != f64_to_bits(d)) {
            fprintf(stderr, "roundtrip: %.17g -> \"%s\" -> %.17g\n", d, text, back);
            FAILm("the pair did not round trip");
        }
    }

    ASSERT(checked > N / 2);
    PASS();
}

SUITE(dtoa_suite) {
    RUN_TEST(free_format_spells_the_shortest_exact_form);
    RUN_TEST(the_minus_zero_flag_is_what_brings_the_sign_back);
    RUN_TEST(non_finite_values_use_the_javascript_spelling);
    RUN_TEST(explicit_formats_and_exponents);
    RUN_TEST(max_len_bounds_the_write);

    RUN_TEST(integer_conversions_match_the_libc_ones);
    RUN_TEST(radix_conversions_read_back);

    RUN_TEST(parse_takes_the_number_at_the_front);
    RUN_TEST(parse_reports_where_a_number_ends_and_where_it_never_started);
    RUN_TEST(parse_radix_detection_and_separators);
    RUN_TEST(int_only_refuses_the_fractional_and_exponential_parts);

    RUN_TEST(every_value_survives_both_directions);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(dtoa_suite);
    GREATEST_MAIN_END();
}
