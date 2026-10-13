/*
 * tests/unit/test_regexp.c
 *
 * Regular expressions: a pattern compiles, a subject matches, and the groups come
 * back where they were found.
 *
 * The offsets are the interesting part. They are code units in the subject, which
 * means the same number for a narrow string and for a wide one, and that is the
 * property these tests hold the matcher to: widening a narrow subject to match it
 * must not move a single offset.
 */

#include "vendor/greatest.h"

#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "cvm/regexp.h"
#include "cvm/str.h"
#include "gc/collect.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#if !defined(_WIN32)

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)32 * 1024 * 1024)

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

static str literal(char const* text) {
    usize len = 0;
    while (text[len] != '\0') {
        len++;
    }
    str s = {.ptr = (u8 const*)text, .len = len};
    return s;
}

static CvmString* string_of(str text) {
    CvmString* s = cvm_string_from_utf8(text);
    return s;
}

/* One match, with room for the groups a test asks about. */
#define CAPTURES 8

TEST a_pattern_matches_and_reports_its_groups(void) {
    fixture_enter();
    FRAME(3);
    f_roots[0] = string_of(literal("a(b+)c"));
    f_roots[1] = string_of(literal("xxabbbcxx"));
    CvmRegExp* re = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    f_roots[2] = re;
    ASSERT(re != NULL);
    ASSERT_EQ(2, cvm_regexp_capture_count(re));

    i32 captures[CAPTURES];
    ASSERT_EQ(1, cvm_regexp_exec(re, (CvmString*)f_roots[1], 0, captures, 0));
    /* group 0 is the whole match: at 2, five units long */
    ASSERT_EQ(2, captures[0]);
    ASSERT_EQ(7, captures[1]);
    /* group 1 is the run of b's inside it */
    ASSERT_EQ(3, captures[2]);
    ASSERT_EQ(6, captures[3]);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_match_can_start_where_the_caller_says(void) {
    fixture_enter();
    FRAME(3);
    f_roots[0] = string_of(literal("a(b+)c"));
    f_roots[1] = string_of(literal("abbbc abbbc"));
    f_roots[2] = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    ASSERT(f_roots[2] != NULL);

    i32 captures[CAPTURES];
    ASSERT_EQ(1, cvm_regexp_exec((CvmRegExp*)f_roots[2], (CvmString*)f_roots[1], 6,
                                 captures, 0));
    ASSERT_EQ(6, captures[0]);
    ASSERT_EQ(11, captures[1]);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_pattern_that_does_not_match_says_so(void) {
    fixture_enter();
    FRAME(3);
    f_roots[0] = string_of(literal("^a+$"));
    f_roots[1] = string_of(literal("ab"));
    f_roots[2] = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    ASSERT(f_roots[2] != NULL);

    i32 captures[CAPTURES];
    ASSERT_EQ(0, cvm_regexp_exec((CvmRegExp*)f_roots[2], (CvmString*)f_roots[1], 0,
                                 captures, 0));
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST flags_are_recorded_and_case_folding_follows_them(void) {
    fixture_enter();
    FRAME(4);
    f_roots[0] = string_of(literal("hello"));
    f_roots[1] = string_of(literal("HeLLo"));
    f_roots[2] = cvm_regexp_compile((CvmString*)f_roots[0], CVM_RE_FLAG_IGNORECASE);
    f_roots[3] = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    ASSERT(f_roots[2] != NULL);
    ASSERT(f_roots[3] != NULL);

    ASSERT(CVM_RE_FLAG_IGNORECASE ==
           (cvm_regexp_flags((CvmRegExp*)f_roots[2]) & CVM_RE_FLAG_IGNORECASE));
    ASSERT(0 == (cvm_regexp_flags((CvmRegExp*)f_roots[3]) & CVM_RE_FLAG_IGNORECASE));

    i32 captures[CAPTURES];
    ASSERT_EQ(1, cvm_regexp_exec((CvmRegExp*)f_roots[2], (CvmString*)f_roots[1], 0,
                                 captures, 0));
    ASSERT_EQ(0, cvm_regexp_exec((CvmRegExp*)f_roots[3], (CvmString*)f_roots[1], 0,
                                 captures, 0));
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST named_groups_are_found_by_name(void) {
    fixture_enter();
    FRAME(4);
    f_roots[0] = string_of(literal("(?<year>\\d{4})-(?<month>\\d{2})"));
    f_roots[1] = string_of(literal("on 2024-05-01"));
    f_roots[2] = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    ASSERT(f_roots[2] != NULL);
    CvmRegExp* re = (CvmRegExp*)f_roots[2];
    ASSERT_EQ(3, cvm_regexp_capture_count(re));

    f_roots[3] = string_of(literal("year"));
    ASSERT_EQ(1, cvm_regexp_group_index(re, (CvmString*)f_roots[3]));
    f_roots[3] = string_of(literal("month"));
    ASSERT_EQ(2, cvm_regexp_group_index(re, (CvmString*)f_roots[3]));
    f_roots[3] = string_of(literal("day"));
    ASSERT_EQ(-1, cvm_regexp_group_index(re, (CvmString*)f_roots[3]));

    i32 captures[CAPTURES];
    ASSERT_EQ(1, cvm_regexp_exec(re, (CvmString*)f_roots[1], 0, captures, 0));
    ASSERT_EQ(3, captures[0]);
    ASSERT_EQ(10, captures[1]);
    ASSERT_EQ(3, captures[2]); /* year */
    ASSERT_EQ(7, captures[3]);
    ASSERT_EQ(8, captures[4]); /* month */
    ASSERT_EQ(10, captures[5]);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_pattern_without_names_has_no_group_to_find(void) {
    fixture_enter();
    FRAME(3);
    f_roots[0] = string_of(literal("(a)(b)"));
    f_roots[1] = string_of(literal("a"));
    f_roots[2] = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    ASSERT(f_roots[2] != NULL);
    ASSERT_EQ(-1,
              cvm_regexp_group_index((CvmRegExp*)f_roots[2], (CvmString*)f_roots[1]));
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_wide_subject_matches_in_its_own_units(void) {
    fixture_enter();
    FRAME(3);
    /* Code points above 0xFF are what makes the representation wide. */
    f_roots[0] = string_of(literal("本"));
    f_roots[1] = string_of(literal("日本語"));
    ASSERT(cvm_string_is_wide((CvmString*)f_roots[1]));
    f_roots[2] = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    ASSERT(f_roots[2] != NULL);

    i32 captures[CAPTURES];
    ASSERT_EQ(1, cvm_regexp_exec((CvmRegExp*)f_roots[2], (CvmString*)f_roots[1], 0,
                                 captures, 0));
    ASSERT_EQ(1, captures[0]);
    ASSERT_EQ(2, captures[1]);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST widening_a_narrow_subject_does_not_move_the_offsets(void) {
    fixture_enter();
    FRAME(4);
    /*
     * U+00E9 is one code point below 0x100, so the string is narrow: one byte of
     * the string is one unit of it. Matched in UTF-16 it is still one unit, and
     * the offsets are the same numbers — which is the property that lets a narrow
     * subject go through the wide path.
     */
    f_roots[0] = string_of(literal("é"));
    f_roots[1] = string_of(literal("xéy"));
    ASSERT(!cvm_string_is_wide((CvmString*)f_roots[1]));
    f_roots[2] = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    ASSERT(f_roots[2] != NULL);

    i32 captures[CAPTURES];
    ASSERT_EQ(1, cvm_regexp_exec((CvmRegExp*)f_roots[2], (CvmString*)f_roots[1], 0,
                                 captures, 0));
    ASSERT_EQ(1, captures[0]);
    ASSERT_EQ(2, captures[1]);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST unicode_classes_follow_the_flag(void) {
    fixture_enter();
    FRAME(4);
    f_roots[0] = string_of(literal("\\p{L}+"));
    f_roots[1] = string_of(literal("123 abc"));
    f_roots[2] = cvm_regexp_compile((CvmString*)f_roots[0], CVM_RE_FLAG_UNICODE);
    ASSERT(f_roots[2] != NULL);

    i32 captures[CAPTURES];
    ASSERT_EQ(1, cvm_regexp_exec((CvmRegExp*)f_roots[2], (CvmString*)f_roots[1], 0,
                                 captures, 0));
    ASSERT_EQ(4, captures[0]);
    ASSERT_EQ(7, captures[1]);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_deadline_stops_a_pattern_that_would_run_long(void) {
    fixture_enter();
    FRAME(3);
    f_roots[0] = string_of(literal("(a+)+b"));
    f_roots[1] = string_of(literal("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    f_roots[2] = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    ASSERT(f_roots[2] != NULL);

    /* A deadline in the past: the matcher must give up rather than backtrack. */
    i32 captures[CAPTURES];
    ASSERT_EQ(CVM_RE_RET_TIMEOUT,
              cvm_regexp_exec((CvmRegExp*)f_roots[2], (CvmString*)f_roots[1], 0,
                              captures, 1u));
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_pattern_that_does_not_compile_is_null(void) {
    fixture_enter();
    FRAME(2);
    f_roots[0] = string_of(literal("(unclosed"));
    f_roots[1] = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    ASSERT(f_roots[1] == NULL);

    f_roots[0] = string_of(literal("a{2,1}"));
    f_roots[1] = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    ASSERT(f_roots[1] == NULL);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_compiled_pattern_survives_a_collection(void) {
    fixture_enter();
    FRAME(3);
    f_roots[0] = string_of(literal("(\\w+)@(\\w+)"));
    f_roots[1] = string_of(literal("write to me@here"));
    f_roots[2] = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    ASSERT(f_roots[2] != NULL);

    collect();

    i32 captures[CAPTURES];
    ASSERT_EQ(1, cvm_regexp_exec((CvmRegExp*)f_roots[2], (CvmString*)f_roots[1], 0,
                                 captures, 0));
    ASSERT_EQ(9, captures[0]);
    ASSERT_EQ(16, captures[1]);
    ASSERT_EQ(9, captures[2]);
    ASSERT_EQ(11, captures[3]);
    ASSERT_EQ(12, captures[4]);
    ASSERT_EQ(16, captures[5]);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST backreference_matches_what_the_group_caught(void) {
    fixture_enter();
    FRAME(4);
    f_roots[0] = cvm_string_from_utf8(literal("(\\w+)\\s\\1"));
    f_roots[1] = cvm_string_from_utf8(literal("hello hello"));
    CvmRegExp* re = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    f_roots[2] = re;
    ASSERT(re != NULL);

    i32 captures[8];
    ASSERT_EQ(1, cvm_regexp_exec(re, (CvmString*)f_roots[1], 0, captures, 0));
    ASSERT_EQ(0, captures[0]); /* the whole "hello hello" */
    ASSERT_EQ(0, captures[2]); /* group 1: the first "hello" */
    ASSERT_EQ(5, captures[3]);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST lookahead_finds_without_consuming(void) {
    fixture_enter();
    FRAME(4);
    f_roots[0] = cvm_string_from_utf8(literal("\\w+(?=;)"));
    f_roots[1] = cvm_string_from_utf8(literal("abc;def"));
    CvmRegExp* re = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    f_roots[2] = re;
    ASSERT(re != NULL);

    i32 captures[8];
    ASSERT_EQ(1, cvm_regexp_exec(re, (CvmString*)f_roots[1], 0, captures, 0));
    /* "abc" matched, the ";" was looked at but not consumed. */
    ASSERT_EQ(0, captures[0]);
    ASSERT_EQ(3, captures[1]);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST negative_lookahead_excludes_its_pattern(void) {
    fixture_enter();
    FRAME(4);
    f_roots[0] = cvm_string_from_utf8(literal("\\d+(?!\\d)"));
    f_roots[1] = cvm_string_from_utf8(literal("12345"));
    CvmRegExp* re = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    f_roots[2] = re;
    ASSERT(re != NULL);

    i32 captures[8];
    /* Greedy \d+ takes "12345"; the lookahead then asks whether a digit
     * follows, and at the end nothing does. The match is the whole string. */
    ASSERT_EQ(1, cvm_regexp_exec(re, (CvmString*)f_roots[1], 0, captures, 0));
    ASSERT_EQ(0, captures[0]);
    ASSERT_EQ(5, captures[1]);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST sticky_requires_the_start_position(void) {
    fixture_enter();
    FRAME(4);
    f_roots[0] = cvm_string_from_utf8(literal("abc"));
    f_roots[1] = cvm_string_from_utf8(literal("xxabc"));
    CvmRegExp* sticky = cvm_regexp_compile((CvmString*)f_roots[0], CVM_RE_FLAG_STICKY);
    CvmRegExp* plain = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    f_roots[2] = sticky;
    f_roots[3] = plain;
    ASSERT(sticky != NULL && plain != NULL);

    i32 captures[8];
    /* Sticky at 0: no match, because "abc" does not start there. */
    ASSERT_EQ(0, cvm_regexp_exec(sticky, (CvmString*)f_roots[1], 0, captures, 0));
    /* Sticky at 2: match, because it does. */
    ASSERT_EQ(1, cvm_regexp_exec(sticky, (CvmString*)f_roots[1], 2, captures, 0));
    /* Plain at 0: match, because it searches. */
    ASSERT_EQ(1, cvm_regexp_exec(plain, (CvmString*)f_roots[1], 0, captures, 0));
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST multiline_anchors_match_at_line_breaks(void) {
    fixture_enter();
    FRAME(4);
    f_roots[0] = cvm_string_from_utf8(literal("^b"));
    f_roots[1] = cvm_string_from_utf8(literal("a\nb"));
    CvmRegExp* ml = cvm_regexp_compile((CvmString*)f_roots[0], CVM_RE_FLAG_MULTILINE);
    f_roots[2] = ml;
    ASSERT(ml != NULL);

    i32 captures[8];
    /* Without multiline, ^ is only at position 0; "a" is there, so no match.
     * With multiline, ^ is also after the newline, so "b" matches. */
    ASSERT_EQ(1, cvm_regexp_exec(ml, (CvmString*)f_roots[1], 0, captures, 0));
    ASSERT_EQ(2, captures[0]); /* after the \n */
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST dotall_lets_the_dot_cross_lines(void) {
    fixture_enter();
    FRAME(4);
    f_roots[0] = cvm_string_from_utf8(literal("a.b"));
    f_roots[1] = cvm_string_from_utf8(literal("a\nb"));
    CvmRegExp* da = cvm_regexp_compile((CvmString*)f_roots[0], CVM_RE_FLAG_DOTALL);
    CvmRegExp* plain = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    f_roots[2] = da;
    f_roots[3] = plain;
    ASSERT(da != NULL && plain != NULL);

    i32 captures[8];
    /* Without dotall, . does not match \n. */
    ASSERT_EQ(0, cvm_regexp_exec(plain, (CvmString*)f_roots[1], 0, captures, 0));
    /* With dotall, it does. */
    ASSERT_EQ(1, cvm_regexp_exec(da, (CvmString*)f_roots[1], 0, captures, 0));
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_word_boundary_is_found(void) {
    fixture_enter();
    FRAME(4);
    f_roots[0] = cvm_string_from_utf8(literal("\\bcat\\b"));
    f_roots[1] = cvm_string_from_utf8(literal("a cat in a catalog"));
    CvmRegExp* re = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    f_roots[2] = re;
    ASSERT(re != NULL);

    i32 captures[8];
    /* "cat" is a word by itself at position 2; "catalog" has no boundary
     * after "cat". */
    ASSERT_EQ(1, cvm_regexp_exec(re, (CvmString*)f_roots[1], 0, captures, 0));
    ASSERT_EQ(2, captures[0]);
    ASSERT_EQ(5, captures[1]);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_group_that_did_not_participate_is_negative_one(void) {
    fixture_enter();
    FRAME(4);
    /* The optional group is not taken when "b" is absent. */
    f_roots[0] = cvm_string_from_utf8(literal("a(b)?c"));
    f_roots[1] = cvm_string_from_utf8(literal("ac"));
    CvmRegExp* re = cvm_regexp_compile((CvmString*)f_roots[0], 0);
    f_roots[2] = re;
    ASSERT(re != NULL);

    i32 captures[8];
    captures[2] = 42; /* poison, to see that it was overwritten */
    ASSERT_EQ(1, cvm_regexp_exec(re, (CvmString*)f_roots[1], 0, captures, 0));
    ASSERT_EQ(-1, captures[2]); /* the group did not take part */
    ASSERT_EQ(-1, captures[3]);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST non_ascii_case_folding_matches_across_scripts(void) {
    fixture_enter();
    FRAME(4);
    /* Pattern: the regexp's \\u escape names U+03A3. Subject: the actual
     * UTF-8 bytes of U+03C3 — a string built from text, not a pattern. */
    f_roots[0] = cvm_string_from_utf8(literal("\\u03A3"));
    f_roots[1] = cvm_string_from_utf8((str){.ptr = (u8 const*)"\xcf\x83", .len = 2});
    CvmRegExp* ci = cvm_regexp_compile((CvmString*)f_roots[0], CVM_RE_FLAG_IGNORECASE);
    f_roots[2] = ci;
    ASSERT(ci != NULL);

    i32 captures[8];
    ASSERT_EQ(1, cvm_regexp_exec(ci, (CvmString*)f_roots[1], 0, captures, 0));
    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(regexp_suite) {
    RUN_TEST(a_pattern_matches_and_reports_its_groups);
    RUN_TEST(a_match_can_start_where_the_caller_says);
    RUN_TEST(a_pattern_that_does_not_match_says_so);
    RUN_TEST(flags_are_recorded_and_case_folding_follows_them);
    RUN_TEST(named_groups_are_found_by_name);
    RUN_TEST(a_pattern_without_names_has_no_group_to_find);
    RUN_TEST(a_wide_subject_matches_in_its_own_units);
    RUN_TEST(widening_a_narrow_subject_does_not_move_the_offsets);
    RUN_TEST(unicode_classes_follow_the_flag);
    RUN_TEST(a_deadline_stops_a_pattern_that_would_run_long);
    RUN_TEST(a_pattern_that_does_not_compile_is_null);
    RUN_TEST(a_compiled_pattern_survives_a_collection);
    RUN_TEST(backreference_matches_what_the_group_caught);
    RUN_TEST(lookahead_finds_without_consuming);
    RUN_TEST(negative_lookahead_excludes_its_pattern);
    RUN_TEST(sticky_requires_the_start_position);
    RUN_TEST(multiline_anchors_match_at_line_breaks);
    RUN_TEST(dotall_lets_the_dot_cross_lines);
    RUN_TEST(a_word_boundary_is_found);
    RUN_TEST(a_group_that_did_not_participate_is_negative_one);
    RUN_TEST(non_ascii_case_folding_matches_across_scripts);
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
    RUN_SUITE(regexp_suite);
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
