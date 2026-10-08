/*
 * tests/unit/test_unicode.c
 *
 * Unicode case mapping and classification.
 *
 * The expectations are the Unicode standard's own: the mappings that are not one
 * character to one character (sharp s to "SS", dotted I to "i" plus a combining
 * dot) are the reason the case API returns a count, and the classifications
 * below 0x100 are answered by the byte table while the ones above it are not.
 */

#include "vendor/greatest.h"

#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/builder.h"
#include "cvm/gc.h"
#include "cvm/str.h"
#include "cvm/unicode.h"
#include "gc/collect.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#if !defined(_WIN32)

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)16 * 1024 * 1024)

static CvmHeap* heap(void) {
    return &cvm_world()->heap;
}

static void collect(void) {
    cvm_gc_collect_and_finish(heap());
}

static void fixture_enter(void) {
    (void)cvm_thread_attach();
}

static void fixture_leave(void) {
    cvm_thread_detach();
}

#define FRAME(slots)                                                                   \
    CVM_FRAME_DECL(f, slots);                                                          \
    cvm_frame_push(&f_frame, f_roots, slots);                                          \
    for (u32 i = 0; i < (slots); i++) {                                                \
        f_roots[i] = NULL;                                                             \
    }

#define FRAME_END() cvm_frame_pop(&f_frame)

static rune upper_of(rune c) {
    rune out[CVM_UNICODE_CASE_MAX];
    return cvm_unicode_case_conv(out, c, CVM_UNICODE_CASE_UPPER) == 1u ? out[0]
                                                                       : 0xFFFFFFFFu;
}

static rune lower_of(rune c) {
    rune out[CVM_UNICODE_CASE_MAX];
    return cvm_unicode_case_conv(out, c, CVM_UNICODE_CASE_LOWER) == 1u ? out[0]
                                                                       : 0xFFFFFFFFu;
}

static bool string_is(CvmString* s, char const* want) {
    usize n = 0;
    while (want[n] != '\0') {
        n++;
    }
    if (cvm_string_length(s) != n) {
        return false;
    }
    for (usize i = 0; i < n; i++) {
        if (cvm_string_unit(s, i) != (u32)(u8)want[i]) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* case mapping                                                        */
/* ------------------------------------------------------------------ */

TEST ascii_case_mapping(void) {
    ASSERT_EQ_FMT((u32)'A', upper_of('a'), "%u");
    ASSERT_EQ_FMT((u32)'a', lower_of('A'), "%u");
    /* already in the target case: unchanged, and one code point */
    ASSERT_EQ_FMT((u32)'A', upper_of('A'), "%u");
    ASSERT_EQ_FMT((u32)'a', lower_of('a'), "%u");
    /* and a character with no case at all */
    ASSERT_EQ_FMT((u32)'1', upper_of('1'), "%u");
    ASSERT_EQ_FMT((u32)' ', lower_of(' '), "%u");
    PASS();
}

TEST case_mapping_beyond_ascii(void) {
    /* U+00E9 LATIN SMALL LETTER E WITH ACUTE and its capital */
    ASSERT_EQ_FMT(0x00C9u, upper_of(0x00E9u), "%u");
    ASSERT_EQ_FMT(0x00E9u, lower_of(0x00C9u), "%u");
    /* U+00FF -> U+0178, which is the case that needs 16-bit units */
    ASSERT_EQ_FMT(0x0178u, upper_of(0x00FFu), "%u");
    /* U+03C3 GREEK SMALL LETTER SIGMA and its capital */
    ASSERT_EQ_FMT(0x03A3u, upper_of(0x03C3u), "%u");
    ASSERT_EQ_FMT(0x03C3u, lower_of(0x03A3u), "%u");
    /* a code point with no case above 0x100 stays itself */
    ASSERT_EQ_FMT(0x4E2Du, upper_of(0x4E2Du), "%u");
    PASS();
}

TEST a_mapping_can_be_more_than_one_code_point(void) {
    rune out[CVM_UNICODE_CASE_MAX];

    /* U+00DF LATIN SMALL LETTER SHARP S upper-cases to "SS" */
    u32 n = cvm_unicode_case_conv(out, 0x00DFu, CVM_UNICODE_CASE_UPPER);
    ASSERT_EQ_FMT((u32)2, n, "%u");
    ASSERT_EQ_FMT((u32)'S', out[0], "%u");
    ASSERT_EQ_FMT((u32)'S', out[1], "%u");

    /* U+0130 LATIN CAPITAL LETTER I WITH DOT ABOVE lower-cases to "i" plus
     * U+0307 COMBINING DOT ABOVE */
    n = cvm_unicode_case_conv(out, 0x0130u, CVM_UNICODE_CASE_LOWER);
    ASSERT_EQ_FMT((u32)2, n, "%u");
    ASSERT_EQ_FMT((u32)'i', out[0], "%u");
    ASSERT_EQ_FMT(0x0307u, out[1], "%u");

    /* and the result array is only written as far as the count says */
    ASSERT_EQ_FMT((u32)1, cvm_unicode_case_conv(out, 'a', CVM_UNICODE_CASE_UPPER),
                  "%u");
    PASS();
}

TEST canonicalize_is_one_form_per_flag(void) {
    /*
     * The regex engine's fold, not a case conversion: for ASCII the flag also
     * picks the direction, and that is fine as long as a comparison uses one
     * flag on both sides — which is what the engine does.
     */
    ASSERT_EQ_FMT((u32)'a', cvm_unicode_canonicalize('A', true), "%u");
    ASSERT_EQ_FMT((u32)'A', cvm_unicode_canonicalize('a', false), "%u");
    /* beyond ASCII the flag says whether to fold at all: with it, U+00C9 folds
     * to U+00E9; without it, a non-ASCII character is left alone */
    ASSERT_EQ_FMT(0x00E9u, cvm_unicode_canonicalize(0x00C9u, true), "%u");
    ASSERT_EQ_FMT(0x00C9u, cvm_unicode_canonicalize(0x00C9u, false), "%u");
    PASS();
}

/* ------------------------------------------------------------------ */
/* classification                                                      */
/* ------------------------------------------------------------------ */

TEST the_predicates_answer_for_ascii_and_beyond(void) {
    /* is_cased */
    ASSERT(cvm_unicode_is_cased('a'));
    ASSERT(cvm_unicode_is_cased('A'));
    ASSERT_FALSE(cvm_unicode_is_cased('1'));
    ASSERT_FALSE(cvm_unicode_is_cased(' '));
    ASSERT(cvm_unicode_is_cased(0x00DFu)); /* sharp s is a cased letter */

    /* is_case_ignorable: what a case-insensitive match may skip over */
    ASSERT(cvm_unicode_is_case_ignorable('\''));
    ASSERT(cvm_unicode_is_case_ignorable(0x00ADu)); /* soft hyphen */
    ASSERT_FALSE(cvm_unicode_is_case_ignorable('A'));

    /* white space, of which the byte table knows four and the table beyond it
     * knows more */
    ASSERT(cvm_unicode_is_white_space(' '));
    ASSERT(cvm_unicode_is_white_space('\t'));
    ASSERT(cvm_unicode_is_white_space('\n'));
    ASSERT(cvm_unicode_is_white_space(0x00A0u)); /* no-break space */
    ASSERT(cvm_unicode_is_white_space(0x2028u)); /* line separator */
    ASSERT_FALSE(cvm_unicode_is_white_space('a'));
    ASSERT_FALSE(cvm_unicode_is_white_space(0x4E2Du));

    /* space, as the tokenizer's byte table classifies it */
    ASSERT(cvm_unicode_is_space(' '));
    ASSERT(cvm_unicode_is_space(0x00A0u));
    ASSERT_FALSE(cvm_unicode_is_space('a'));

    /* ID_Start is a Unicode property: letters, and not the underscore a
     * language adds to it. Both answers are correct, and knowing which question
     * is being asked is the whole point. */
    ASSERT(cvm_unicode_is_id_start('a'));
    ASSERT(cvm_unicode_is_id_start('Z'));
    ASSERT(cvm_unicode_is_id_start(0x00E9u));
    ASSERT_FALSE(cvm_unicode_is_id_start('_'));
    ASSERT_FALSE(cvm_unicode_is_id_start('$'));
    ASSERT_FALSE(cvm_unicode_is_id_start('1'));
    ASSERT_FALSE(cvm_unicode_is_id_start('-'));

    /* The identifier rule a tokenizer wants: the property, plus `_` and `$` */
    ASSERT(cvm_unicode_is_ident_start('a'));
    ASSERT(cvm_unicode_is_ident_start('_'));
    ASSERT(cvm_unicode_is_ident_start('$'));
    ASSERT(cvm_unicode_is_ident_start(0x00E9u));
    ASSERT_FALSE(cvm_unicode_is_ident_start('1'));
    ASSERT_FALSE(cvm_unicode_is_ident_start('-'));

    ASSERT(cvm_unicode_is_ident_continue('a'));
    ASSERT(cvm_unicode_is_ident_continue('1'));
    ASSERT(cvm_unicode_is_ident_continue('_'));
    ASSERT(cvm_unicode_is_ident_continue(0x200Cu)); /* ZWNJ */
    ASSERT_FALSE(cvm_unicode_is_ident_continue('-'));
    ASSERT_FALSE(cvm_unicode_is_ident_continue(' '));
    PASS();
}

/* ------------------------------------------------------------------ */
/* over strings                                                        */
/* ------------------------------------------------------------------ */

TEST case_mapping_over_a_string(void) {
    fixture_enter();

    FRAME(3);
    CvmString* hello = cvm_string_new_ascii(STR_LITERAL("hello"));
    f_roots[0] = hello;

    CvmString* upper = cvm_unicode_upper(hello);
    f_roots[1] = upper;
    ASSERT(upper != NULL);
    ASSERT(string_is(upper, "HELLO"));
    ASSERT_FALSE(cvm_string_is_wide(upper));

    CvmString* lower = cvm_unicode_lower(upper);
    f_roots[2] = lower;
    ASSERT(string_is(lower, "hello"));

    /* "straße" upper-cases to "STRASSE": one character becomes two, and the
     * result is still narrow */
    CvmString* street = cvm_string_from_utf8(STR_LITERAL("stra\xC3\x9F"
                                                         "e"));
    f_roots[0] = street;
    ASSERT(street != NULL);
    CvmString* shout = cvm_unicode_upper(street);
    f_roots[1] = shout;
    ASSERT(shout != NULL);
    ASSERT(string_is(shout, "STRASSE"));
    ASSERT_FALSE(cvm_string_is_wide(shout));

    /* and a mapping above 0xFF widens: U+00FF -> U+0178 */
    CvmString* yumlaut = cvm_string_from_utf8(STR_LITERAL("\xC3\xBF"));
    f_roots[0] = yumlaut;
    CvmString* wide = cvm_unicode_upper(yumlaut);
    f_roots[1] = wide;
    ASSERT(wide != NULL);
    ASSERT(cvm_string_is_wide(wide));
    ASSERT_EQ_FMT((usize)1, cvm_string_length(wide), "%zu");
    ASSERT_EQ_FMT(0x0178u, cvm_string_unit(wide, 0), "%u");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_mapped_string_survives_a_collection(void) {
    fixture_enter();

    FRAME(1);
    CvmString* src = cvm_string_from_utf8(STR_LITERAL("caf\xC3\xA9"));
    ASSERT(src != NULL);

    f_roots[0] = cvm_unicode_upper(src);
    collect();

    CvmString* after = (CvmString*)f_roots[0];
    ASSERT_EQ_FMT((usize)4, cvm_string_length(after), "%zu");
    ASSERT_EQ_FMT((u32)'C', cvm_string_unit(after, 0), "%u");
    ASSERT_EQ_FMT(0x00C9u, cvm_string_unit(after, 3), "%u");

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* normalization                                                       */
/* ------------------------------------------------------------------ */

/* Normalize a run of code points, and compare the result to what is wanted.
 * The buffer belongs to the allocator, so it is released here. */
static bool normalizes(rune const* src, usize n, CvmUnicodeNormalization kind,
                       rune const* want, usize want_n) {
    CvmUnicodeAlloc alloc = cvm_unicode_system_alloc();
    rune* out = NULL;
    i32 len = cvm_unicode_normalize(&out, src, n, kind, alloc);
    if (len < 0) {
        return false;
    }
    bool ok = (usize)len == want_n;
    for (usize i = 0; ok && i < want_n; i++) {
        ok = out[i] == want[i];
    }
    if (out != NULL) {
        alloc.free(alloc.ctx, out);
    }
    return ok;
}

TEST composition_and_decomposition(void) {
    /* "e" followed by a combining acute, and the one code point they compose
     * into */
    static rune const decomposed[] = {'e', 0x0301u};
    static rune const composed[] = {0x00E9u};

    ASSERT(normalizes(decomposed, 2, CVM_UNICODE_NFC, composed, 1));
    ASSERT(normalizes(composed, 1, CVM_UNICODE_NFD, decomposed, 2));
    /* already in the target form: unchanged */
    ASSERT(normalizes(composed, 1, CVM_UNICODE_NFC, composed, 1));
    ASSERT(normalizes(decomposed, 2, CVM_UNICODE_NFD, decomposed, 2));
    /* ASCII is unaffected, which is the fast path the ported code keeps */
    static rune const ascii[] = {'a', 'b', 'c'};
    ASSERT(normalizes(ascii, 3, CVM_UNICODE_NFC, ascii, 3));
    ASSERT(normalizes(ascii, 3, CVM_UNICODE_NFKC, ascii, 3));
    PASS();
}

TEST compatibility_forms_are_the_kind_with_no_composition(void) {
    /* U+FB01 LATIN SMALL LIGATURE FI decomposes under compatibility */
    static rune const ligature[] = {0xFB01u};
    static rune const fi[] = {'f', 'i'};
    ASSERT(normalizes(ligature, 1, CVM_UNICODE_NFKD, fi, 2));
    ASSERT(normalizes(ligature, 1, CVM_UNICODE_NFKC, fi, 2));
    /* and the canonical forms leave it alone */
    ASSERT(normalizes(ligature, 1, CVM_UNICODE_NFC, ligature, 1));
    ASSERT(normalizes(ligature, 1, CVM_UNICODE_NFD, ligature, 1));

    /* U+00BD VULGAR FRACTION ONE HALF expands to three code points, one of which
     * comes from the short-code table rather than from a decomposition */
    static rune const half[] = {0x00BDu};
    static rune const one_third[] = {'1', 0x2044u, '2'};
    ASSERT(normalizes(half, 1, CVM_UNICODE_NFKC, one_third, 3));
    PASS();
}

TEST hangul_composes_and_decomposes(void) {
    /* U+AC00 HANGUL SYLLABLE GA is U+1100 + U+1161, and composes back */
    static rune const syllable[] = {0xAC00u};
    static rune const jamo[] = {0x1100u, 0x1161u};
    ASSERT(normalizes(syllable, 1, CVM_UNICODE_NFD, jamo, 2));
    ASSERT(normalizes(jamo, 2, CVM_UNICODE_NFC, syllable, 1));

    /* with a trailing consonant, which is the third jamo */
    static rune const syllable_with_tail[] = {0xAC01u};
    static rune const jamo_with_tail[] = {0x1100u, 0x1161u, 0x11A8u};
    ASSERT(normalizes(syllable_with_tail, 1, CVM_UNICODE_NFD, jamo_with_tail, 3));
    ASSERT(normalizes(jamo_with_tail, 3, CVM_UNICODE_NFC, syllable_with_tail, 1));
    PASS();
}

TEST normalizing_a_string(void) {
    fixture_enter();

    FRAME(1);
    /* "cafe" + U+0301, which composes to "café" */
    CvmString* source = cvm_string_from_utf8(STR_LITERAL("cafe\xCC\x81"));
    ASSERT(source != NULL);
    f_roots[0] = source;

    CvmString* nfc = cvm_unicode_normalize_string(source, CVM_UNICODE_NFC);
    f_roots[0] = nfc;
    ASSERT(nfc != NULL);
    ASSERT_EQ_FMT((usize)4, cvm_string_length(nfc), "%zu");
    ASSERT_EQ_FMT(0x00E9u, cvm_string_unit(nfc, 3), "%u");

    /* and back apart */
    CvmString* nfd = cvm_unicode_normalize_string(nfc, CVM_UNICODE_NFD);
    f_roots[0] = nfd;
    ASSERT(nfd != NULL);
    ASSERT_EQ_FMT((usize)5, cvm_string_length(nfd), "%zu");
    ASSERT_EQ_FMT((u32)'e', cvm_string_unit(nfd, 3), "%u");
    ASSERT_EQ_FMT(0x0301u, cvm_string_unit(nfd, 4), "%u");

    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(unicode_suite) {
    RUN_TEST(ascii_case_mapping);
    RUN_TEST(case_mapping_beyond_ascii);
    RUN_TEST(a_mapping_can_be_more_than_one_code_point);
    RUN_TEST(canonicalize_is_one_form_per_flag);
    RUN_TEST(the_predicates_answer_for_ascii_and_beyond);
    RUN_TEST(case_mapping_over_a_string);
    RUN_TEST(a_mapped_string_survives_a_collection);
    RUN_TEST(composition_and_decomposition);
    RUN_TEST(compatibility_forms_are_the_kind_with_no_composition);
    RUN_TEST(hangul_composes_and_decomposes);
    RUN_TEST(normalizing_a_string);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    cvm_threads_init();
    cvm_safepoint_init();
    if (!cvm_world_init(TEST_MIN, TEST_MAX)) {
        fprintf(stderr, "could not bring up the heap\n");
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(unicode_suite);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}

#else /* _WIN32 */

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
