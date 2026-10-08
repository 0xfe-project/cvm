/*
 * tests/unit/test_str_methods.c
 *
 * The string's operations: views, conversion in both directions, code points
 * against code units, search, and pieces.
 *
 * The distinction under most of this file is the one the type is built on — a
 * string is a run of code units, and a code point is a different thing that
 * sometimes costs two of them. Every place the two can be confused is a place
 * text gets lost or a length comes out wrong, so each is a case: a lone
 * surrogate (a unit that is not a point), a surrogate pair (a point that is two
 * units), and a conversion that has to survive both.
 */

#include "vendor/greatest.h"

#include <stdio.h>

#include "base/types.h"
#include "base/utf8.h"
#include "context.h"
#include "cvm/builder.h"
#include "cvm/gc.h"
#include "cvm/str.h"
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

/* A string from raw UTF-8 bytes, which may not be printable. */
static CvmString* from_bytes(u8 const* p, usize n) {
    str s = {.ptr = p, .len = n};
    return cvm_string_from_utf8(s);
}

/* ------------------------------------------------------------------ */
/* views                                                               */
/* ------------------------------------------------------------------ */

TEST a_view_is_the_strings_own_units(void) {
    fixture_enter();

    CvmString* s = cvm_string_new_ascii(STR_LITERAL("hello"));
    CvmStringView v = cvm_string_view(s);

    ASSERT_EQ_FMT((usize)5, v.length, "%zu");
    ASSERT_FALSE(v.wide);
    ASSERT((u8 const*)s->data == v.units); /* no copy: the same bytes */

    CvmString* wide = cvm_string_new16((u16 const[]){0x4E2Du}, 1);
    CvmStringView w = cvm_string_view(wide);
    ASSERT(w.wide);
    ASSERT_EQ_FMT((usize)1, w.length, "%zu");

    fixture_leave();
    PASS();
}

TEST as_str_is_bytes_or_nothing(void) {
    fixture_enter();

    CvmString* narrow = cvm_string_new_ascii(STR_LITERAL("bytes"));
    str out = {0};
    ASSERT(cvm_string_as_str(narrow, &out));
    ASSERT_EQ_FMT((usize)5, out.len, "%zu");
    ASSERT_EQ_FMT('b', (int)out.ptr[0], "%c");

    /* A 16-bit string's units are not bytes, and saying so is the point. */
    CvmString* wide = cvm_string_new16((u16 const[]){0x4E2Du}, 1);
    ASSERT_FALSE(cvm_string_as_str(wide, &out));

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* from UTF-8                                                          */
/* ------------------------------------------------------------------ */

TEST from_utf8_picks_the_smaller_representation(void) {
    fixture_enter();

    CvmString* ascii = cvm_string_from_utf8(STR_LITERAL("plain"));
    ASSERT_FALSE(cvm_string_is_wide(ascii));
    ASSERT_EQ_FMT((usize)5, cvm_string_length(ascii), "%zu");

    /* U+00E9 in two bytes is still one 8-bit unit */
    CvmString* latin1 = from_bytes((u8 const*)"caf\xC3\xA9", 5);
    ASSERT(latin1 != NULL);
    ASSERT_FALSE(cvm_string_is_wide(latin1));
    ASSERT_EQ_FMT((usize)4, cvm_string_length(latin1), "%zu");
    ASSERT_EQ_FMT(0xE9u, cvm_string_unit(latin1, 3), "%u");

    /* U+20AC is above 0x100 and forces the wide form */
    CvmString* euro = from_bytes((u8 const*)"\xE2\x82\xAC", 3);
    ASSERT(euro != NULL);
    ASSERT(cvm_string_is_wide(euro));
    ASSERT_EQ_FMT((usize)1, cvm_string_length(euro), "%zu");
    ASSERT_EQ_FMT(0x20ACu, cvm_string_unit(euro, 0), "%u");

    /* U+1F600 is two units: the one place the unit count is not the character
     * count of the input */
    CvmString* emoji = from_bytes((u8 const*)"\xF0\x9F\x98\x80", 4);
    ASSERT(emoji != NULL);
    ASSERT(cvm_string_is_wide(emoji));
    ASSERT_EQ_FMT((usize)2, cvm_string_length(emoji), "%zu");
    ASSERT_EQ_FMT(0xD83Du, cvm_string_unit(emoji, 0), "%u");
    ASSERT_EQ_FMT(0xDE00u, cvm_string_unit(emoji, 1), "%u");

    fixture_leave();
    PASS();
}

TEST from_utf8_keeps_a_lone_surrogate_but_refuses_malformed_text(void) {
    fixture_enter();

    /* WTF-8 for U+D800: a string has to be able to hold what an escape asked
     * for, and this is what that looks like in bytes. */
    CvmString* lone = from_bytes((u8 const*)"\xED\xA0\x80", 3);
    ASSERT(lone != NULL);
    ASSERT(cvm_string_is_wide(lone));
    ASSERT_EQ_FMT((usize)1, cvm_string_length(lone), "%zu");
    ASSERT_EQ_FMT(0xD800u, cvm_string_unit(lone, 0), "%u");

    /* The rest is not text under any reading. */
    ASSERT(from_bytes((u8 const*)"\x80", 1) == NULL);     /* lone continuation */
    ASSERT(from_bytes((u8 const*)"\xC0\x80", 2) == NULL); /* overlong */
    ASSERT(from_bytes((u8 const*)"\xF0\x80\x80\x80", 4) == NULL); /* overlong */
    ASSERT(from_bytes((u8 const*)"\xE2\x82", 2) == NULL);         /* truncated */
    ASSERT(from_bytes((u8 const*)"\xF4\x90\x80\x80", 4) == NULL); /* past U+10FFFF */

    fixture_leave();
    PASS();
}

TEST utf8_written_back_is_the_text_that_came_in(void) {
    fixture_enter();

    static u8 const cases[] = {
        'a',    '\xC3', '\xA9',         /* a, e-acute */
        '\xE2', '\x82', '\xAC',         /* euro */
        '\xF0', '\x9F', '\x98', '\x80', /* emoji */
        '\xED', '\xA0', '\x80',         /* a lone surrogate */
    };

    for (usize start = 0; start < sizeof(cases);) {
        /* The lead byte, then every continuation after it. */
        usize end = start + 1u;
        while (end < sizeof(cases) && (cases[end] & 0xC0u) == 0x80u) {
            end++;
        }

        CvmString* s = from_bytes(cases + start, end - start);
        ASSERT(s != NULL);

        usize need = cvm_string_utf8_length(s);
        ASSERT_EQ_FMT(end - start, need, "%zu");

        u8 out[16];
        ASSERT_EQ_FMT(need, cvm_string_write_utf8(s, out, sizeof(out)), "%zu");
        for (usize i = 0; i < need; i++) {
            ASSERT_EQ_FMT((u32)cases[start + i], (u32)out[i], "0x%02x");
        }
        start = end;
    }

    fixture_leave();
    PASS();
}

TEST utf8_writing_refuses_a_buffer_that_is_too_small(void) {
    fixture_enter();

    CvmString* s = from_bytes((u8 const*)"\xE2\x82\xAC", 3); /* three bytes */
    usize need = cvm_string_utf8_length(s);
    ASSERT_EQ_FMT((usize)3, need, "%zu");

    u8 out[8];
    for (usize i = 0; i < sizeof(out); i++) {
        out[i] = 0xAAu;
    }

    ASSERT_EQ_FMT(CVM_STRING_NO_ROOM, cvm_string_write_utf8(s, out, 2), "%zu");
    /* and the buffer is untouched, so a retry with a bigger one is enough */
    ASSERT_EQ_FMT(0xAAu, (u32)out[0], "0x%02x");

    ASSERT_EQ_FMT(CVM_STRING_NO_ROOM, cvm_string_write_utf8_nul(s, out, 3), "%zu");
    ASSERT_EQ_FMT((usize)3, cvm_string_write_utf8_nul(s, out, 4), "%zu");
    ASSERT_EQ_FMT(0x00u, (u32)out[3], "0x%02x");

    fixture_leave();
    PASS();
}

TEST utf16_is_the_units_in_order(void) {
    fixture_enter();

    CvmString* narrow = cvm_string_new_ascii(STR_LITERAL("ab"));
    ASSERT_EQ_FMT((usize)2, cvm_string_utf16_length(narrow), "%zu");

    u16 out[4];
    ASSERT_EQ_FMT((usize)2, cvm_string_write_utf16(narrow, out, 4), "%zu");
    ASSERT_EQ_FMT('a', (int)out[0], "%c");
    ASSERT_EQ_FMT('b', (int)out[1], "%c");
    ASSERT_EQ_FMT(CVM_STRING_NO_ROOM, cvm_string_write_utf16(narrow, out, 1), "%zu");

    /* a wide string with a pair comes out as its two units */
    CvmString* wide = from_bytes((u8 const*)"\xF0\x9F\x98\x80", 4);
    ASSERT_EQ_FMT((usize)2, cvm_string_utf16_length(wide), "%zu");
    ASSERT_EQ_FMT((usize)2, cvm_string_write_utf16(wide, out, 4), "%zu");
    ASSERT_EQ_FMT(0xD83Du, (u32)out[0], "%u");
    ASSERT_EQ_FMT(0xDE00u, (u32)out[1], "%u");

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* code points                                                         */
/* ------------------------------------------------------------------ */

TEST code_points_pair_surrogates_and_keep_lone_ones(void) {
    fixture_enter();

    /* "a" U+00E9 U+4E2D U+1F600 */
    CvmString* s = from_bytes((u8 const*)"a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80", 10);
    ASSERT(s != NULL);

    usize next = 0;
    ASSERT_EQ_FMT('a', (int)cvm_string_code_point_at(s, 0, &next), "%c");
    ASSERT_EQ_FMT((usize)1, next, "%zu");

    ASSERT_EQ_FMT(0xE9u, cvm_string_code_point_at(s, 1, &next), "%u");
    ASSERT_EQ_FMT((usize)2, next, "%zu");

    ASSERT_EQ_FMT(0x4E2Du, cvm_string_code_point_at(s, 2, &next), "%u");
    ASSERT_EQ_FMT((usize)3, next, "%zu");

    /* the pair: one code point, two units */
    ASSERT_EQ_FMT(0x1F600u, cvm_string_code_point_at(s, 3, &next), "%u");
    ASSERT_EQ_FMT((usize)5, next, "%zu");

    ASSERT_EQ_FMT((usize)4, cvm_string_code_point_count(s), "%zu");

    /* A lone high surrogate: returned as itself, one unit. Nothing else would
     * be lossless. */
    CvmString* lone_high = cvm_string_new16((u16 const[]){0xD83Du}, 1);
    ASSERT_EQ_FMT(0xD83Du, cvm_string_code_point_at(lone_high, 0, &next), "%u");
    ASSERT_EQ_FMT((usize)1, next, "%zu");
    ASSERT_EQ_FMT((usize)1, cvm_string_code_point_count(lone_high), "%zu");

    /* A high surrogate at the end of a string has no partner, so it stays a
     * unit: the same rule, and the one a naive pair-decoder gets wrong by
     * reading past the end. */
    CvmString* high_then_ascii = cvm_string_new16((u16 const[]){0xD83Du, 'x'}, 2);
    ASSERT_EQ_FMT(0xD83Du, cvm_string_code_point_at(high_then_ascii, 0, &next), "%u");
    ASSERT_EQ_FMT((usize)1, next, "%zu");
    ASSERT_EQ_FMT((usize)2, cvm_string_code_point_count(high_then_ascii), "%zu");

    CvmString* lone_low = cvm_string_new16((u16 const[]){0xDE00u}, 1);
    ASSERT_EQ_FMT(0xDE00u, cvm_string_code_point_at(lone_low, 0, &next), "%u");

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* search                                                              */
/* ------------------------------------------------------------------ */

TEST index_of_finds_positions_and_refuses_absent_ones(void) {
    fixture_enter();

    CvmString* hay = cvm_string_new_ascii(STR_LITERAL("the quick brown fox"));
    CvmString* brown = cvm_string_new_ascii(STR_LITERAL("brown"));
    CvmString* fox = cvm_string_new_ascii(STR_LITERAL("fox"));
    CvmString* cat = cvm_string_new_ascii(STR_LITERAL("cat"));

    ASSERT_EQ_FMT((isize)10, cvm_string_index_of(hay, brown, 0), "%zd");
    ASSERT_EQ_FMT((isize)16, cvm_string_index_of(hay, fox, 0), "%zd");
    ASSERT_EQ_FMT((isize)-1, cvm_string_index_of(hay, cat, 0), "%zd");
    /* starting past the match */
    ASSERT_EQ_FMT((isize)-1, cvm_string_index_of(hay, brown, 11), "%zd");
    /* a needle longer than what is left */
    ASSERT_EQ_FMT((isize)-1, cvm_string_index_of(hay, fox, 17), "%zd");

    /* an empty needle is at `from`, clamped to the length */
    CvmString* empty = cvm_string_new8(NULL, 0);
    ASSERT_EQ_FMT((isize)4, cvm_string_index_of(hay, empty, 4), "%zd");
    ASSERT_EQ_FMT((isize)19, cvm_string_index_of(hay, empty, 100), "%zd");

    /* the needle's representation need not match the haystack's */
    CvmString* wide_brown = cvm_string_new16((u16 const[]){'b', 'r', 'o', 'w', 'n'}, 5);
    ASSERT_EQ_FMT((isize)10, cvm_string_index_of(hay, wide_brown, 0), "%zd");

    ASSERT(cvm_string_includes(hay, fox));
    ASSERT_FALSE(cvm_string_includes(hay, cat));

    fixture_leave();
    PASS();
}

TEST last_index_of_finds_the_last_one(void) {
    fixture_enter();

    CvmString* hay = cvm_string_new_ascii(STR_LITERAL("abcabcabc"));
    CvmString* abc = cvm_string_new_ascii(STR_LITERAL("abc"));
    CvmString* cab = cvm_string_new_ascii(STR_LITERAL("cab"));

    ASSERT_EQ_FMT((isize)6, cvm_string_last_index_of(hay, abc), "%zd");
    ASSERT_EQ_FMT((isize)5, cvm_string_last_index_of(hay, cab), "%zd");
    ASSERT_EQ_FMT(
        (isize)-1,
        cvm_string_last_index_of(hay, cvm_string_new_ascii(STR_LITERAL("zz"))), "%zd");
    ASSERT_EQ_FMT((isize)9, cvm_string_last_index_of(hay, cvm_string_new8(NULL, 0)),
                  "%zd");

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* pieces                                                              */
/* ------------------------------------------------------------------ */

TEST substring_takes_the_units_it_is_given(void) {
    fixture_enter();

    CvmString* s = cvm_string_new_ascii(STR_LITERAL("abcdef"));

    CvmString* middle = cvm_string_substring(s, 2, 4);
    ASSERT_EQ_FMT((usize)2, cvm_string_length(middle), "%zu");
    ASSERT_EQ_FMT('c', (int)cvm_string_unit(middle, 0), "%c");
    ASSERT_EQ_FMT('d', (int)cvm_string_unit(middle, 1), "%c");

    /* 0..length is the whole string, and an empty range is a string too */
    CvmString* all = cvm_string_substring(s, 0, 6);
    ASSERT(cvm_string_equal(all, s));
    /* a copy, not the same storage — the slice form will change this, and this
     * line is what has to change with it */
    ASSERT((u8 const*)all->data != (u8 const*)s->data);

    CvmString* empty = cvm_string_substring(s, 3, 3);
    ASSERT_EQ_FMT((usize)0, cvm_string_length(empty), "%zu");
    ASSERT_FALSE(cvm_string_is_wide(empty));

    /* a wide source stays wide */
    CvmString* wide = cvm_string_new16((u16 const[]){0x41u, 0x4E2Du, 0x42u}, 3);
    CvmString* piece = cvm_string_substring(wide, 1, 2);
    ASSERT(cvm_string_is_wide(piece));
    ASSERT_EQ_FMT((usize)1, cvm_string_length(piece), "%zu");
    ASSERT_EQ_FMT(0x4E2Du, cvm_string_unit(piece, 0), "%u");

    fixture_leave();
    PASS();
}

TEST concat_puts_the_units_end_to_end(void) {
    fixture_enter();

    CvmString* a = cvm_string_new_ascii(STR_LITERAL("one"));
    CvmString* b = cvm_string_new_ascii(STR_LITERAL("two"));

    CvmString* both = cvm_string_concat(a, b);
    ASSERT_EQ_FMT((usize)6, cvm_string_length(both), "%zu");
    ASSERT_FALSE(cvm_string_is_wide(both));
    ASSERT(cvm_string_equal(both, cvm_string_new_ascii(STR_LITERAL("onetwo"))));

    /* a wide half widens the result rather than dropping anything */
    CvmString* wide = cvm_string_new16((u16 const[]){0x4E2Du}, 1);
    CvmString* mixed = cvm_string_concat(a, wide);
    ASSERT(cvm_string_is_wide(mixed));
    ASSERT_EQ_FMT((usize)4, cvm_string_length(mixed), "%zu");
    ASSERT_EQ_FMT(0x4E2Du, cvm_string_unit(mixed, 3), "%u");

    /* and the empty string is an identity on both sides */
    CvmString* empty = cvm_string_new8(NULL, 0);
    ASSERT(cvm_string_equal(cvm_string_concat(a, empty), a));
    ASSERT(cvm_string_equal(cvm_string_concat(empty, a), a));
    ASSERT_EQ_FMT((usize)0, cvm_string_length(cvm_string_concat(empty, empty)), "%zu");

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* the collector, once more                                            */
/* ------------------------------------------------------------------ */

TEST results_survive_a_collection(void) {
    /*
     * Substring and concat hold their sources across an allocation, so both are
     * places where a missing frame would be invisible until a collection landed
     * in the window. This runs a collection after the fact and checks the
     * results, which is what a frame is for.
     */
    fixture_enter();

    FRAME(3);
    CvmString* a = cvm_string_new_ascii(STR_LITERAL("alpha"));
    f_roots[0] = a;
    CvmString* b = cvm_string_new_ascii(STR_LITERAL("beta"));
    f_roots[1] = b;

    CvmString* joined = cvm_string_concat(a, b);
    CvmString* piece = cvm_string_substring(joined, 3, 6);
    f_roots[2] = piece;

    collect();

    CvmString* still_joined = (CvmString*)f_roots[2];
    /* "alpha" + "beta" = "alphabeta"; [3, 6) is "hab" */
    ASSERT_EQ_FMT((usize)3, cvm_string_length(still_joined), "%zu");
    ASSERT_EQ_FMT('h', (int)cvm_string_unit(still_joined, 0), "%c");
    ASSERT_EQ_FMT('a', (int)cvm_string_unit(still_joined, 1), "%c");
    ASSERT_EQ_FMT('b', (int)cvm_string_unit(still_joined, 2), "%c");

    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(str_methods_suite) {
    RUN_TEST(a_view_is_the_strings_own_units);
    RUN_TEST(as_str_is_bytes_or_nothing);

    RUN_TEST(from_utf8_picks_the_smaller_representation);
    RUN_TEST(from_utf8_keeps_a_lone_surrogate_but_refuses_malformed_text);
    RUN_TEST(utf8_written_back_is_the_text_that_came_in);
    RUN_TEST(utf8_writing_refuses_a_buffer_that_is_too_small);
    RUN_TEST(utf16_is_the_units_in_order);

    RUN_TEST(code_points_pair_surrogates_and_keep_lone_ones);

    RUN_TEST(index_of_finds_positions_and_refuses_absent_ones);
    RUN_TEST(last_index_of_finds_the_last_one);

    RUN_TEST(substring_takes_the_units_it_is_given);
    RUN_TEST(concat_puts_the_units_end_to_end);

    RUN_TEST(results_survive_a_collection);
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
    RUN_SUITE(str_methods_suite);
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
