/*
 * tests/unit/test_utf8.c
 *
 * UTF-8 decoding, encoding, validation and counting.
 *
 * The cases worth pinning are the ones where a lenient decoder and a
 * strict one disagree: overlong encodings, surrogates, and values past
 * U+10FFFF. A decoder that accepts those is not "robust", it is a
 * different function, and the difference is a security boundary in
 * anything that compares paths or filters input.
 */

#include "vendor/greatest.h"

#include "base/types.h"
#include "base/utf8.h"

/* Build a str over raw bytes, which are not necessarily printable. */
static str bytes_of(u8 const* p, usize n) {
    str s = {.ptr = p, .len = n};
    return s;
}

static CvmUtf8Status status_of_seq(u8 const* p, usize n, rune* out) {
    usize i = 0;
    return cvm_utf8_next(bytes_of(p, n), &i, out);
}

/* ------------------------------------------------------------------ */
/* encoding                                                            */
/* ------------------------------------------------------------------ */

TEST encode_len_boundaries(void) {
    ASSERT_EQ_FMT(1u, cvm_utf8_encode_len(0x00u), "%u");
    ASSERT_EQ_FMT(1u, cvm_utf8_encode_len(0x7Fu), "%u");
    ASSERT_EQ_FMT(2u, cvm_utf8_encode_len(0x80u), "%u");
    ASSERT_EQ_FMT(2u, cvm_utf8_encode_len(0x7FFu), "%u");
    ASSERT_EQ_FMT(3u, cvm_utf8_encode_len(0x800u), "%u");
    ASSERT_EQ_FMT(3u, cvm_utf8_encode_len(0xFFFFu), "%u");
    ASSERT_EQ_FMT(4u, cvm_utf8_encode_len(0x10000u), "%u");
    ASSERT_EQ_FMT(4u, cvm_utf8_encode_len(CVM_RUNE_MAX), "%u");
    PASS();
}

TEST encode_len_rejects_past_the_maximum(void) {
    /* Not a code point, so there is no length: the caller is told by 0
     * rather than given a plausible number for a value that has no
     * encoding. */
    ASSERT_EQ_FMT(0u, cvm_utf8_encode_len(CVM_RUNE_MAX + 1u), "%u");
    ASSERT_EQ_FMT(0u, cvm_utf8_encode_len(0xFFFFFFFFu), "%u");
    PASS();
}

TEST encode_writes_known_sequences(void) {
    u8 buf[CVM_UTF8_MAX_BYTES];

    /* one byte */
    ASSERT_EQ_FMT(1u, cvm_utf8_encode(0x41u, buf), "%u");
    ASSERT_EQ_FMT(0x41u, (u32)buf[0], "0x%02x");

    /* two bytes: the smallest code point that needs them */
    ASSERT_EQ_FMT(2u, cvm_utf8_encode(0x80u, buf), "%u");
    ASSERT_EQ_FMT(0xC2u, (u32)buf[0], "0x%02x");
    ASSERT_EQ_FMT(0x80u, (u32)buf[1], "0x%02x");

    /* three bytes: the euro sign, a familiar non-ASCII character */
    ASSERT_EQ_FMT(3u, cvm_utf8_encode(0x20ACu, buf), "%u");
    ASSERT_EQ_FMT(0xE2u, (u32)buf[0], "0x%02x");
    ASSERT_EQ_FMT(0x82u, (u32)buf[1], "0x%02x");
    ASSERT_EQ_FMT(0xACu, (u32)buf[2], "0x%02x");

    /* four bytes: the largest code point */
    ASSERT_EQ_FMT(4u, cvm_utf8_encode(CVM_RUNE_MAX, buf), "%u");
    ASSERT_EQ_FMT(0xF4u, (u32)buf[0], "0x%02x");
    ASSERT_EQ_FMT(0x8Fu, (u32)buf[1], "0x%02x");
    ASSERT_EQ_FMT(0xBFu, (u32)buf[2], "0x%02x");
    ASSERT_EQ_FMT(0xBFu, (u32)buf[3], "0x%02x");
    PASS();
}

TEST encode_returns_zero_past_the_maximum(void) {
    u8 buf[CVM_UTF8_MAX_BYTES];
    ASSERT_EQ_FMT(0u, cvm_utf8_encode(CVM_RUNE_MAX + 1u, buf), "%u");
    PASS();
}

/* ------------------------------------------------------------------ */
/* decoding                                                            */
/* ------------------------------------------------------------------ */

TEST decode_reads_ascii_one_byte_at_a_time(void) {
    str s = STR_LITERAL("abc");
    usize i = 0;
    rune r = 0;

    ASSERT_EQ_FMT(CVM_UTF8_RUNE, (u32)cvm_utf8_next(s, &i, &r), "%u");
    ASSERT_EQ_FMT(0x61u, (u32)r, "0x%02x");
    ASSERT_EQ_FMT((usize)1u, i, "%zu");

    ASSERT_EQ_FMT(CVM_UTF8_RUNE, (u32)cvm_utf8_next(s, &i, &r), "%u");
    ASSERT_EQ_FMT(0x62u, (u32)r, "0x%02x");

    ASSERT_EQ_FMT(CVM_UTF8_RUNE, (u32)cvm_utf8_next(s, &i, &r), "%u");
    ASSERT_EQ_FMT(0x63u, (u32)r, "0x%02x");

    ASSERT_EQ_FMT(CVM_UTF8_END, (u32)cvm_utf8_next(s, &i, &r), "%u");
    ASSERT_EQ_FMT((usize)3u, i, "%zu");
    PASS();
}

TEST decode_of_empty_text_is_end(void) {
    str empty = STR_LITERAL("");
    usize i = 0;
    rune r = 0;
    ASSERT_EQ_FMT(CVM_UTF8_END, (u32)cvm_utf8_next(empty, &i, &r), "%u");
    PASS();
}

TEST decode_reads_the_longest_valid_sequence(void) {
    static u8 const text[] = {0xF4u, 0x8Fu, 0xBFu, 0xBFu};
    usize i = 0;
    rune r = 0;

    ASSERT_EQ_FMT(CVM_UTF8_RUNE,
                  (u32)cvm_utf8_next(bytes_of(text, sizeof(text)), &i, &r), "%u");
    ASSERT_EQ_FMT(CVM_RUNE_MAX, (u32)r, "0x%x");
    ASSERT_EQ_FMT(sizeof(text), i, "%zu");
    PASS();
}

TEST decode_advances_by_the_sequence_length(void) {
    /* e2 82 ac is the euro sign; the 'x' after it still decodes */
    static u8 const text[] = {0xE2u, 0x82u, 0xACu, 0x78u};
    str s = bytes_of(text, sizeof(text));
    usize i = 0;
    rune r = 0;

    ASSERT_EQ_FMT(CVM_UTF8_RUNE, (u32)cvm_utf8_next(s, &i, &r), "%u");
    ASSERT_EQ_FMT((usize)3u, i, "%zu");

    ASSERT_EQ_FMT(CVM_UTF8_RUNE, (u32)cvm_utf8_next(s, &i, &r), "%u");
    ASSERT_EQ_FMT(0x78u, (u32)r, "0x%02x");
    ASSERT_EQ_FMT(sizeof(text), i, "%zu");
    PASS();
}

/* ------------------------------------------------------------------ */
/* what a decoder must refuse                                          */
/* ------------------------------------------------------------------ */

TEST seq_len_rejects_the_impossible_lead_bytes(void) {
    /* A continuation byte cannot lead. */
    ASSERT_EQ_FMT(0u, cvm_utf8_seq_len(0x80u), "%u");
    ASSERT_EQ_FMT(0u, cvm_utf8_seq_len(0xBFu), "%u");
    /* C0 and C1 can only express an overlong two-byte value. */
    ASSERT_EQ_FMT(0u, cvm_utf8_seq_len(0xC0u), "%u");
    ASSERT_EQ_FMT(0u, cvm_utf8_seq_len(0xC1u), "%u");
    /* F5 and up start a value above U+10FFFF. */
    ASSERT_EQ_FMT(0u, cvm_utf8_seq_len(0xF5u), "%u");
    ASSERT_EQ_FMT(0u, cvm_utf8_seq_len(0xFFu), "%u");
    PASS();
}

TEST decode_rejects_a_lone_continuation_byte(void) {
    static u8 const bad[] = {0x80u};
    rune r = 0;
    ASSERT_EQ_FMT(CVM_UTF8_BAD, (u32)status_of_seq(bad, sizeof(bad), &r), "%u");
    PASS();
}

TEST decode_rejects_overlong_encodings(void) {
    /* Each of these encodes a value that a shorter sequence could have
     * expressed, which is the classic way a filter and its consumer are
     * made to disagree about what the bytes say. */
    static u8 const two[] = {0xC0u, 0x80u};                /* overlong 0x00 */
    static u8 const three[] = {0xE0u, 0x80u, 0x80u};       /* overlong 0x00 */
    static u8 const four[] = {0xF0u, 0x80u, 0x80u, 0x80u}; /* overlong 0x00 */
    /* 0xE0 0x9F is below U+0800; 0xF0 0x8F is below U+10000. */
    static u8 const three_hi[] = {0xE0u, 0x9Fu, 0xBFu};
    static u8 const four_hi[] = {0xF0u, 0x8Fu, 0xBFu, 0xBFu};
    rune r = 0;

    ASSERT_EQ_FMT(CVM_UTF8_BAD, (u32)status_of_seq(two, sizeof(two), &r), "%u");
    ASSERT_EQ_FMT(CVM_UTF8_BAD, (u32)status_of_seq(three, sizeof(three), &r), "%u");
    ASSERT_EQ_FMT(CVM_UTF8_BAD, (u32)status_of_seq(four, sizeof(four), &r), "%u");
    ASSERT_EQ_FMT(CVM_UTF8_BAD, (u32)status_of_seq(three_hi, sizeof(three_hi), &r),
                  "%u");
    ASSERT_EQ_FMT(CVM_UTF8_BAD, (u32)status_of_seq(four_hi, sizeof(four_hi), &r), "%u");
    PASS();
}

TEST decode_rejects_sequences_past_the_maximum(void) {
    static u8 const bad[] = {0xF4u, 0x90u, 0x80u, 0x80u}; /* U+110000 */
    rune r = 0;
    ASSERT_EQ_FMT(CVM_UTF8_BAD, (u32)status_of_seq(bad, sizeof(bad), &r), "%u");
    PASS();
}

TEST decode_rejects_truncated_sequences(void) {
    /* The lead byte promises bytes the text does not have. With a length
     * in hand there is nothing to wait for, so this is malformed rather
     * than incomplete. */
    static u8 const two_short[] = {0xC2u};
    static u8 const three_short[] = {0xE2u, 0x82u};
    static u8 const four_short[] = {0xF0u, 0x90u, 0x80u};
    rune r = 0;

    ASSERT_EQ_FMT(CVM_UTF8_BAD, (u32)status_of_seq(two_short, sizeof(two_short), &r),
                  "%u");
    ASSERT_EQ_FMT(CVM_UTF8_BAD,
                  (u32)status_of_seq(three_short, sizeof(three_short), &r), "%u");
    ASSERT_EQ_FMT(CVM_UTF8_BAD, (u32)status_of_seq(four_short, sizeof(four_short), &r),
                  "%u");
    PASS();
}

TEST decode_rejects_a_bad_continuation_byte(void) {
    /* C2 must be followed by a continuation, not by another lead. */
    static u8 const bad[] = {0xC2u, 0xC2u};
    rune r = 0;
    ASSERT_EQ_FMT(CVM_UTF8_BAD, (u32)status_of_seq(bad, sizeof(bad), &r), "%u");
    PASS();
}

TEST decode_leaves_the_offset_alone_on_a_bad_sequence(void) {
    /* The caller resynchronises, so the decoder must not decide how many
     * bytes a malformed sequence "used". */
    static u8 const text[] = {0x80u, 0x41u};
    usize i = 0;
    rune r = 0;

    ASSERT_EQ_FMT(CVM_UTF8_BAD,
                  (u32)cvm_utf8_next(bytes_of(text, sizeof(text)), &i, &r), "%u");
    ASSERT_EQ_FMT((usize)0u, i, "%zu");
    PASS();
}

/* ------------------------------------------------------------------ */
/* surrogates: strict against WTF-8                                    */
/* ------------------------------------------------------------------ */

TEST surrogates_are_strictly_malformed(void) {
    static u8 const d800[] = {0xEDu, 0xA0u, 0x80u}; /* U+D800 */
    static u8 const dfff[] = {0xEDu, 0xBFu, 0xBFu}; /* U+DFFF */
    rune r = 0;

    ASSERT_EQ_FMT(CVM_UTF8_BAD, (u32)status_of_seq(d800, sizeof(d800), &r), "%u");
    ASSERT_EQ_FMT(CVM_UTF8_BAD, (u32)status_of_seq(dfff, sizeof(dfff), &r), "%u");
    PASS();
}

TEST surrogates_decode_in_wtf8_mode(void) {
    static u8 const d800[] = {0xEDu, 0xA0u, 0x80u};
    static u8 const dfff[] = {0xEDu, 0xBFu, 0xBFu};
    usize i = 0;
    rune r = 0;

    i = 0;
    ASSERT_EQ_FMT(
        CVM_UTF8_RUNE,
        (u32)cvm_utf8_next_mode(bytes_of(d800, sizeof(d800)), &i, &r, CVM_UTF8_WTF8),
        "%u");
    ASSERT_EQ_FMT(0xD800u, (u32)r, "0x%x");

    i = 0;
    ASSERT_EQ_FMT(
        CVM_UTF8_RUNE,
        (u32)cvm_utf8_next_mode(bytes_of(dfff, sizeof(dfff)), &i, &r, CVM_UTF8_WTF8),
        "%u");
    ASSERT_EQ_FMT(0xDFFFu, (u32)r, "0x%x");
    PASS();
}

TEST wtf8_does_not_admit_anything_else(void) {
    /* The relaxation is exactly surrogates: an overlong form is still
     * malformed, so the mode cannot be used to smuggle one past. */
    static u8 const overlong[] = {0xE0u, 0x80u, 0x80u};
    usize i = 0;
    rune r = 0;

    ASSERT_EQ_FMT(CVM_UTF8_BAD,
                  (u32)cvm_utf8_next_mode(bytes_of(overlong, sizeof(overlong)), &i, &r,
                                          CVM_UTF8_WTF8),
                  "%u");
    PASS();
}

/* ------------------------------------------------------------------ */
/* round trip                                                          */
/* ------------------------------------------------------------------ */

TEST every_encoded_rune_decodes_back(void) {
    /* The boundaries where the encoding length changes, plus a stride
     * through the BMP and the supplementary planes. */
    static rune const cases[] = {
        0x0000u, 0x0001u, 0x007Fu, 0x0080u, 0x00FFu,  0x07FFu,  0x0800u,   0x20ACu,
        0xD7FFu, 0xE000u, 0xFFFDu, 0xFFFFu, 0x10000u, 0x1F600u, 0x10FFFFu,
    };

    for (usize k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        u8 buf[CVM_UTF8_MAX_BYTES];
        u32 n = cvm_utf8_encode(cases[k], buf);
        usize i = 0;
        rune back = 0;

        ASSERT_EQ_FMT(cvm_utf8_encode_len(cases[k]), n, "%u");
        ASSERT(cvm_utf8_encode_len(cases[k]) != 0u);
        ASSERT_EQ_FMT(CVM_UTF8_RUNE,
                      (u32)cvm_utf8_next(bytes_of(buf, (usize)n), &i, &back), "%u");
        ASSERT_EQ_FMT(cases[k], (u32)back, "0x%x");
        ASSERT_EQ_FMT((usize)n, i, "%zu");
    }
    PASS();
}

TEST surrogate_round_trips_through_wtf8(void) {
    /* The reason the encoder is total: a lone surrogate that came from a
     * 16-bit string has to survive a trip through bytes. */
    static rune const cases[] = {0xD800u, 0xDBFFu, 0xDC00u, 0xDFFFu};

    for (usize k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        u8 buf[CVM_UTF8_MAX_BYTES];
        u32 n = cvm_utf8_encode(cases[k], buf);
        usize i = 0;
        rune back = 0;

        ASSERT_EQ_FMT(3u, n, "%u");
        ASSERT_EQ_FMT(
            CVM_UTF8_RUNE,
            (u32)cvm_utf8_next_mode(bytes_of(buf, (usize)n), &i, &back, CVM_UTF8_WTF8),
            "%u");
        ASSERT_EQ_FMT(cases[k], (u32)back, "0x%x");
    }
    PASS();
}

/* ------------------------------------------------------------------ */
/* whole-text predicates                                               */
/* ------------------------------------------------------------------ */

TEST valid_accepts_well_formed_text(void) {
    ASSERT(cvm_utf8_valid(STR_LITERAL("")));
    ASSERT(cvm_utf8_valid(STR_LITERAL("plain ascii")));
    /* U+00E9 U+20AC U+1F600 */
    static u8 const mixed[] = {0xC3u, 0xA9u, 0xE2u, 0x82u, 0xACu,
                               0xF0u, 0x9Fu, 0x98u, 0x80u};
    ASSERT(cvm_utf8_valid(bytes_of(mixed, sizeof(mixed))));
    PASS();
}

TEST valid_rejects_anything_the_decoder_refuses(void) {
    static u8 const lone_cont[] = {0x80u};
    static u8 const overlong[] = {0xC0u, 0x80u};
    static u8 const surrogate[] = {0xEDu, 0xA0u, 0x80u};
    static u8 const too_high[] = {0xF4u, 0x90u, 0x80u, 0x80u};
    static u8 const truncated[] = {0xE2u, 0x82u};
    /* valid prefix, then a bad byte: validity is about the whole text */
    static u8 const tail[] = {0x41u, 0x42u, 0xFFu};

    ASSERT_FALSE(cvm_utf8_valid(bytes_of(lone_cont, sizeof(lone_cont))));
    ASSERT_FALSE(cvm_utf8_valid(bytes_of(overlong, sizeof(overlong))));
    ASSERT_FALSE(cvm_utf8_valid(bytes_of(surrogate, sizeof(surrogate))));
    ASSERT_FALSE(cvm_utf8_valid(bytes_of(too_high, sizeof(too_high))));
    ASSERT_FALSE(cvm_utf8_valid(bytes_of(truncated, sizeof(truncated))));
    ASSERT_FALSE(cvm_utf8_valid(bytes_of(tail, sizeof(tail))));
    PASS();
}

TEST count_counts_runes_not_bytes(void) {
    static u8 const text[] = {0x41u, 0xC3u, 0xA9u, 0xE2u, 0x82u, 0xACu,
                              0xF0u, 0x9Fu, 0x98u, 0x80u}; /* A, e-acute, euro, emoji */
    bool valid = false;
    usize n = cvm_utf8_count(bytes_of(text, sizeof(text)), &valid);

    ASSERT(valid);
    ASSERT_EQ_FMT((usize)4u, n, "%zu");
    ASSERT_EQ_FMT(10u, (u32)sizeof(text), "%u");
    PASS();
}

TEST count_of_empty_and_ascii(void) {
    bool valid = true;
    ASSERT_EQ_FMT((usize)0u, cvm_utf8_count(STR_LITERAL(""), &valid), "%zu");
    ASSERT(valid);

    valid = false;
    ASSERT_EQ_FMT((usize)5u, cvm_utf8_count(STR_LITERAL("abcde"), &valid), "%zu");
    ASSERT(valid);
    PASS();
}

TEST count_reports_malformed_text_without_lying_about_the_count(void) {
    /* One bad byte counts as one rune, and the caller is told. */
    static u8 const text[] = {0x41u, 0x80u, 0x42u};
    bool valid = true;
    ASSERT_EQ_FMT((usize)3u, cvm_utf8_count(bytes_of(text, sizeof(text)), &valid),
                  "%zu");
    ASSERT_FALSE(valid);
    PASS();
}

SUITE(utf8_suite) {
    RUN_TEST(encode_len_boundaries);
    RUN_TEST(encode_len_rejects_past_the_maximum);
    RUN_TEST(encode_writes_known_sequences);
    RUN_TEST(encode_returns_zero_past_the_maximum);

    RUN_TEST(decode_reads_ascii_one_byte_at_a_time);
    RUN_TEST(decode_of_empty_text_is_end);
    RUN_TEST(decode_reads_the_longest_valid_sequence);
    RUN_TEST(decode_advances_by_the_sequence_length);

    RUN_TEST(seq_len_rejects_the_impossible_lead_bytes);
    RUN_TEST(decode_rejects_a_lone_continuation_byte);
    RUN_TEST(decode_rejects_overlong_encodings);
    RUN_TEST(decode_rejects_sequences_past_the_maximum);
    RUN_TEST(decode_rejects_truncated_sequences);
    RUN_TEST(decode_rejects_a_bad_continuation_byte);
    RUN_TEST(decode_leaves_the_offset_alone_on_a_bad_sequence);

    RUN_TEST(surrogates_are_strictly_malformed);
    RUN_TEST(surrogates_decode_in_wtf8_mode);
    RUN_TEST(wtf8_does_not_admit_anything_else);

    RUN_TEST(every_encoded_rune_decodes_back);
    RUN_TEST(surrogate_round_trips_through_wtf8);

    RUN_TEST(valid_accepts_well_formed_text);
    RUN_TEST(valid_rejects_anything_the_decoder_refuses);
    RUN_TEST(count_counts_runes_not_bytes);
    RUN_TEST(count_of_empty_and_ascii);
    RUN_TEST(count_reports_malformed_text_without_lying_about_the_count);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(utf8_suite);
    GREATEST_MAIN_END();
}
