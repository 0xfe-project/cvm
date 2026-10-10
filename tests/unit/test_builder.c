/*
 * tests/unit/test_builder.c
 *
 * The string builder: units against code points, widening, growth, numbers, and
 * freezing.
 *
 * The distinction the file keeps coming back to is units against code points. A
 * string is a run of code units — 8 or 16 bits each — and most of the ways text
 * arrives are already in those terms: bytes, another string, a lone surrogate
 * from an escape. A code point is the other thing, and the only operation that
 * has to translate is put_rune, where a supplementary character becomes two
 * units. Getting the two confused produces strings that look right until someone
 * counts characters.
 */

#include "vendor/greatest.h"

#include <stdio.h>

#include "base/types.h"
#include "base/utf8.h"
#include "context.h"
#include "cvm/builder.h"
#include "cvm/gc.h"
#include "cvm/str.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)16 * 1024 * 1024)

static void fixture_enter(void) {
    (void)cvm_thread_attach();
}

static void fixture_leave(void) {
    cvm_thread_detach();
}

/* The units of a string, as a plain array, for comparing against expectations. */
static usize units_of(CvmString const* s, u32* out, usize cap) {
    usize n = cvm_string_length(s);
    if (n > cap) {
        n = cap;
    }
    for (usize i = 0; i < n; i++) {
        out[i] = cvm_string_unit(s, i);
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* the empty and the trivial                                           */
/* ------------------------------------------------------------------ */

TEST an_empty_builder_freezes_into_an_empty_string(void) {
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);
    ASSERT_EQ_FMT((usize)0, cvm_str_buf_length(&b), "%zu");
    ASSERT(cvm_str_buf_ok(&b));
    /* nothing has been allocated yet */
    ASSERT(b.data == NULL);

    CvmString* s = cvm_str_buf_finish(&b);
    ASSERT(s != NULL);
    ASSERT_EQ_FMT((usize)0, cvm_string_length(s), "%zu");
    ASSERT_FALSE(cvm_string_is_wide(s));

    fixture_leave();
    PASS();
}

TEST the_builder_is_reusable_after_freezing(void) {
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);

    (void)cvm_str_buf_put_bytes(&b, STR_LITERAL("one"));
    CvmString* first = cvm_str_buf_finish(&b);
    ASSERT_EQ_FMT((usize)3, cvm_string_length(first), "%zu");

    /* the builder is empty again, and its scratch is gone */
    ASSERT_EQ_FMT((usize)0, cvm_str_buf_length(&b), "%zu");
    ASSERT(b.data == NULL);

    (void)cvm_str_buf_put_bytes(&b, STR_LITERAL("two"));
    CvmString* second = cvm_str_buf_finish(&b);
    ASSERT_EQ_FMT((usize)3, cvm_string_length(second), "%zu");
    ASSERT_EQ_FMT('t', (int)cvm_string_unit(second, 0), "%c");
    ASSERT(cvm_string_unit(first, 0) == 'o'); /* and the first is untouched */

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* units against code points                                           */
/* ------------------------------------------------------------------ */

TEST bytes_append_as_units(void) {
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);

    /* A byte above 0x7F is one unit whose value is that byte: this is the
     * Latin-1 reading, and it is why put_bytes is the wrong call for UTF-8. */
    static u8 const text[] = {'a', 0xFFu, 0x80u};
    ASSERT(cvm_str_buf_put_bytes(&b, (str){text, sizeof(text)}));

    CvmString* s = cvm_str_buf_finish(&b);
    ASSERT_FALSE(cvm_string_is_wide(s));
    ASSERT_EQ_FMT((usize)3, cvm_string_length(s), "%zu");
    ASSERT_EQ_FMT(0xFFu, cvm_string_unit(s, 1), "%u");
    ASSERT_EQ_FMT(0x80u, cvm_string_unit(s, 2), "%u");

    fixture_leave();
    PASS();
}

TEST utf8_appends_code_points_and_widens_when_it_must(void) {
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);

    /* "a" + U+00E9 (2 bytes) stay narrow; U+20AC (3 bytes) needs a wide unit */
    ASSERT(cvm_str_buf_put_utf8(&b, STR_LITERAL("a\xC3\xA9")));
    ASSERT_FALSE(cvm_str_buf_is_wide(&b));
    ASSERT(cvm_str_buf_put_utf8(&b, STR_LITERAL("\xE2\x82\xAC")));
    ASSERT(cvm_str_buf_is_wide(&b));

    CvmString* s = cvm_str_buf_finish(&b);
    ASSERT(cvm_string_is_wide(s));
    ASSERT_EQ_FMT((usize)3, cvm_string_length(s), "%zu");

    u32 units[4];
    ASSERT_EQ_FMT((usize)3, units_of(s, units, 4), "%zu");
    ASSERT_EQ_FMT('a', (int)units[0], "%c");
    ASSERT_EQ_FMT(0xE9u, units[1], "%u");
    ASSERT_EQ_FMT(0x20ACu, units[2], "%u");

    fixture_leave();
    PASS();
}

TEST a_supplementary_code_point_becomes_a_surrogate_pair(void) {
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);

    /* U+1F600: the emoji case, which is two units and not one character */
    ASSERT(cvm_str_buf_put_rune(&b, 0x1F600u));

    CvmString* s = cvm_str_buf_finish(&b);
    ASSERT(cvm_string_is_wide(s));
    ASSERT_EQ_FMT((usize)2, cvm_string_length(s), "%zu");
    ASSERT_EQ_FMT(0xD83Du, cvm_string_unit(s, 0), "%u");
    ASSERT_EQ_FMT(0xDE00u, cvm_string_unit(s, 1), "%u");

    fixture_leave();
    PASS();
}

TEST a_lone_surrogate_is_a_unit_and_is_accepted(void) {
    /* A code point is never a surrogate; a unit can be. An escape such as
     * "\uD800" is exactly this, and a string has to be able to hold it. */
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);
    ASSERT(cvm_str_buf_put_unit(&b, 0xD800u));

    CvmString* s = cvm_str_buf_finish(&b);
    ASSERT(cvm_string_is_wide(s));
    ASSERT_EQ_FMT((usize)1, cvm_string_length(s), "%zu");
    ASSERT_EQ_FMT(0xD800u, cvm_string_unit(s, 0), "%u");

    fixture_leave();
    PASS();
}

TEST put_rune_refuses_a_value_that_is_not_a_code_point(void) {
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);
    ASSERT_FALSE(cvm_str_buf_put_rune(&b, CVM_RUNE_MAX + 1u));
    ASSERT_FALSE(cvm_str_buf_ok(&b));
    /* and the failure is sticky: the result is NULL, not a truncated string */
    ASSERT(cvm_str_buf_finish(&b) == NULL);

    fixture_leave();
    PASS();
}

TEST malformed_utf8_fails_rather_than_substituting(void) {
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);
    (void)cvm_str_buf_put_bytes(&b, STR_LITERAL("ok"));
    ASSERT_FALSE(cvm_str_buf_put_utf8(&b, STR_LITERAL("\x80")));
    ASSERT_FALSE(cvm_str_buf_ok(&b));
    ASSERT(cvm_str_buf_finish(&b) == NULL);

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* widening and growth                                                 */
/* ------------------------------------------------------------------ */

TEST widening_keeps_what_was_already_there(void) {
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);

    str prefix = STR_LITERAL("abcdefghij");
    ASSERT(cvm_str_buf_put_bytes(&b, prefix));
    ASSERT_FALSE(cvm_str_buf_is_wide(&b));

    ASSERT(cvm_str_buf_put_unit(&b, 0x4E2Du)); /* a wide unit forces the move */
    ASSERT(cvm_str_buf_is_wide(&b));

    CvmString* s = cvm_str_buf_finish(&b);
    ASSERT_EQ_FMT(prefix.len + 1u, cvm_string_length(s), "%zu");
    for (usize i = 0; i < prefix.len; i++) {
        ASSERT_EQ_FMT((u32)prefix.ptr[i], cvm_string_unit(s, i), "%u");
    }
    ASSERT_EQ_FMT(0x4E2Du, cvm_string_unit(s, prefix.len), "%u");

    fixture_leave();
    PASS();
}

TEST a_long_build_keeps_every_unit_in_order(void) {
    /* Growth is several reallocations; nothing may be lost or reordered at any
     * of them. */
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);

    enum { N = 5000 };
    for (u32 i = 0; i < N; i++) {
        if (!cvm_str_buf_put_unit(&b, 0x100u + i)) {
            FAILm("the builder refused a unit");
        }
    }
    ASSERT(cvm_str_buf_is_wide(&b));
    ASSERT_EQ_FMT((usize)N, cvm_str_buf_length(&b), "%zu");

    CvmString* s = cvm_str_buf_finish(&b);
    ASSERT_EQ_FMT((usize)N, cvm_string_length(s), "%zu");
    for (u32 i = 0; i < N; i++) {
        if (cvm_string_unit(s, i) != 0x100u + i) {
            FAILm("a unit was lost or moved during growth");
        }
    }

    fixture_leave();
    PASS();
}

TEST an_explicit_reserve_is_honoured(void) {
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);
    ASSERT(cvm_str_buf_reserve(&b, 1000));
    usize cap = b.cap;
    ASSERT(cap >= 1000);

    /* appending inside the reservation does not reallocate */
    for (u32 i = 0; i < 1000; i++) {
        (void)cvm_str_buf_put_unit(&b, 'x');
    }
    ASSERT_EQ_FMT(cap, b.cap, "%zu");
    ASSERT_EQ_FMT((usize)1000, cvm_str_buf_length(&b), "%zu");
    cvm_str_buf_free(&b);

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* strings, fills and numbers                                          */
/* ------------------------------------------------------------------ */

TEST another_string_appends_with_its_units(void) {
    fixture_enter();

    CvmString* wide_source = cvm_string_new16((u16 const[]){0x41u, 0x4E2Du}, 2);

    CvmStrBuf b;
    cvm_str_buf_init(&b);
    ASSERT(cvm_str_buf_put_bytes(&b, STR_LITERAL("x")));
    ASSERT(cvm_str_buf_put_string(&b, wide_source));

    /* a wide source widens the builder rather than dropping anything */
    ASSERT(cvm_str_buf_is_wide(&b));
    CvmString* s = cvm_str_buf_finish(&b);
    ASSERT_EQ_FMT((usize)3, cvm_string_length(s), "%zu");
    ASSERT_EQ_FMT('x', (int)cvm_string_unit(s, 0), "%c");
    ASSERT_EQ_FMT(0x41u, cvm_string_unit(s, 1), "%u");
    ASSERT_EQ_FMT(0x4E2Du, cvm_string_unit(s, 2), "%u");

    fixture_leave();
    PASS();
}

TEST fill_repeats_a_unit(void) {
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);
    ASSERT(cvm_str_buf_fill(&b, 0x00u, 3));
    ASSERT(cvm_str_buf_fill(&b, 'z', 2));

    CvmString* s = cvm_str_buf_finish(&b);
    ASSERT_EQ_FMT((usize)5, cvm_string_length(s), "%zu");
    ASSERT_EQ_FMT(0x00u, cvm_string_unit(s, 0), "%u");
    ASSERT_EQ_FMT('z', (int)cvm_string_unit(s, 4), "%c");

    fixture_leave();
    PASS();
}

TEST numbers_are_appended_in_their_converted_form(void) {
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);
    ASSERT(cvm_str_buf_put_i64(&b, -42));
    ASSERT(cvm_str_buf_put_bytes(&b, STR_LITERAL("/")));
    ASSERT(cvm_str_buf_put_u64(&b, (u64)18446744073709551615ull));
    ASSERT(cvm_str_buf_put_bytes(&b, STR_LITERAL("/")));
    ASSERT(cvm_str_buf_put_f64(&b, 0.1));
    ASSERT(cvm_str_buf_put_bytes(&b, STR_LITERAL("/")));
    ASSERT(cvm_str_buf_put_f64(&b, -0.0));
    ASSERT(cvm_str_buf_put_bytes(&b, STR_LITERAL("/")));
    ASSERT(cvm_str_buf_put_f64(&b, 1e21));

    CvmString* s = cvm_str_buf_finish(&b);
    static char const* want = "-42/18446744073709551615/0.1/-0/1e+21";

    usize n = 0;
    while (want[n] != '\0') {
        n++;
    }
    ASSERT_EQ_FMT(n, cvm_string_length(s), "%zu");
    for (usize i = 0; i < n; i++) {
        if (cvm_string_unit(s, i) != (u32)(u8)want[i]) {
            fprintf(stderr, "position %zu\n", i);
            FAILm("a number was not spelled as expected");
        }
    }

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* freezing                                                            */
/* ------------------------------------------------------------------ */

TEST freezing_allocates_exactly_what_was_built(void) {
    /*
     * The point of building off-heap: the result is one allocation of exactly
     * the content's size, not a capacity chosen for growth. The collector's own
     * size computation has to agree with that, or the object is mis-sized.
     */
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);
    (void)cvm_str_buf_reserve(&b, 1000); /* a spare capacity of 1000 units */
    (void)cvm_str_buf_put_bytes(&b, STR_LITERAL("twelve bytes"));

    CvmString* s = cvm_str_buf_finish(&b);
    usize payload = offsetof(CvmString, data) + 12u;
    ASSERT_EQ_FMT(cvm_align_up(sizeof(CvmGcHeader) + payload, CVM_ALLOC_ALIGNMENT),
                  cvm_object_size(cvm_header_of(s)), "%zu");

    fixture_leave();
    PASS();
}

TEST a_frozen_string_is_ordinary(void) {
    /* It goes through the same constructors as any other string, so it has the
     * same properties: hash, comparison, equality with a differently-built
     * string of the same text. */
    fixture_enter();

    CvmStrBuf b;
    cvm_str_buf_init(&b);
    (void)cvm_str_buf_put_utf8(&b, STR_LITERAL("hello"));
    CvmString* built = cvm_str_buf_finish(&b);

    CvmString* direct = cvm_string_new_ascii(STR_LITERAL("hello"));
    ASSERT(cvm_string_equal(built, direct));
    ASSERT_EQ_FMT(cvm_string_hash(direct), cvm_string_hash(built), "%u");
    ASSERT_EQ_FMT(0, cvm_string_compare(built, direct), "%d");

    fixture_leave();
    PASS();
}

SUITE(builder_suite) {
    RUN_TEST(an_empty_builder_freezes_into_an_empty_string);
    RUN_TEST(the_builder_is_reusable_after_freezing);

    RUN_TEST(bytes_append_as_units);
    RUN_TEST(utf8_appends_code_points_and_widens_when_it_must);
    RUN_TEST(a_supplementary_code_point_becomes_a_surrogate_pair);
    RUN_TEST(a_lone_surrogate_is_a_unit_and_is_accepted);
    RUN_TEST(put_rune_refuses_a_value_that_is_not_a_code_point);
    RUN_TEST(malformed_utf8_fails_rather_than_substituting);

    RUN_TEST(widening_keeps_what_was_already_there);
    RUN_TEST(a_long_build_keeps_every_unit_in_order);
    RUN_TEST(an_explicit_reserve_is_honoured);

    RUN_TEST(another_string_appends_with_its_units);
    RUN_TEST(fill_repeats_a_unit);
    RUN_TEST(numbers_are_appended_in_their_converted_form);

    RUN_TEST(freezing_allocates_exactly_what_was_built);
    RUN_TEST(a_frozen_string_is_ordinary);
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
    RUN_SUITE(builder_suite);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}
