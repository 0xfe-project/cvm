/*
 * tests/unit/test_rope.c
 *
 * The rope and slice representations, and the properties that make them safe to
 * use as strings rather than as a second-class form.
 *
 * Two things are load-bearing, and they are what this file holds:
 *
 *   - a rope or a slice is equal to the flat string of the same text: equal,
 *     same hash, same order. Anything that holds a pointer and compares by
 *     content — an intern table, a hash map, a sort — must not care which shape
 *     the string is.
 *
 *   - a slice keeps its parent alive, and a rope keeps its children alive,
 *     through a collection. The GC descriptors trace the pointers in a rope's
 *     and a slice's tail, which is the whole reason those descriptors are
 *     CVM_TYPE_STRUCT rather than CVM_TYPE_SCALAR_ARRAY.
 */

#include "vendor/greatest.h"

#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
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

static str lit(char const* text) {
    usize len = 0;
    while (text[len] != '\0') {
        len++;
    }
    str s = {.ptr = (u8 const*)text, .len = len};
    return s;
}

/* ------------------------------------------------------------------ */
/* equality across shapes                                              */
/* ------------------------------------------------------------------ */

TEST a_rope_and_a_flat_string_of_the_same_text_are_equal(void) {
    fixture_enter();
    FRAME(3);
    f_roots[0] = cvm_string_from_utf8(lit("ab"));
    f_roots[1] = cvm_string_concat(cvm_string_from_utf8(lit("a")),
                                   cvm_string_from_utf8(lit("b")));
    ASSERT(f_roots[1] != NULL);

    ASSERT(cvm_string_equal((CvmString*)f_roots[0], (CvmString*)f_roots[1]));
    ASSERT(cvm_string_equal((CvmString*)f_roots[1], (CvmString*)f_roots[0]));

    /* Same hash: a table keyed by content cannot tell them apart. */
    ASSERT_EQ_FMT(cvm_string_hash((CvmString*)f_roots[0]),
                  cvm_string_hash((CvmString*)f_roots[1]), "%u");

    /* Same order. */
    ASSERT_EQ_FMT(0, cvm_string_compare((CvmString*)f_roots[0], (CvmString*)f_roots[1]),
                  "%d");
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_slice_and_a_flat_string_of_the_same_text_are_equal(void) {
    fixture_enter();
    FRAME(3);
    f_roots[0] = cvm_string_from_utf8(lit("hello world"));
    /* "world" is a slice of "hello world" at offset 6 */
    f_roots[1] = cvm_string_substring((CvmString*)f_roots[0], 6u, 11u);
    f_roots[2] = cvm_string_from_utf8(lit("world"));

    ASSERT(cvm_string_equal((CvmString*)f_roots[1], (CvmString*)f_roots[2]));
    ASSERT_EQ_FMT(cvm_string_hash((CvmString*)f_roots[1]),
                  cvm_string_hash((CvmString*)f_roots[2]), "%u");
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_rope_indexes_like_the_flat_string(void) {
    fixture_enter();
    FRAME(2);
    f_roots[0] = cvm_string_from_utf8(lit("hello world"));
    f_roots[1] = cvm_string_concat(cvm_string_from_utf8(lit("hello ")),
                                   cvm_string_from_utf8(lit("world")));

    CvmString* flat = (CvmString*)f_roots[0];
    CvmString* rope = (CvmString*)f_roots[1];
    ASSERT_EQ_FMT(flat->length, rope->length, "%zu");
    for (usize i = 0; i < flat->length; i++) {
        ASSERT_EQ_FMT(cvm_string_unit(flat, i), cvm_string_unit(rope, i), "%u");
    }
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_slice_indexes_like_the_flat_string(void) {
    fixture_enter();
    FRAME(2);
    f_roots[0] = cvm_string_from_utf8(lit("abcdef"));
    /* "cde" is a slice at offset 2, length 3 */
    f_roots[1] = cvm_string_substring((CvmString*)f_roots[0], 2u, 5u);

    CvmString* slice = (CvmString*)f_roots[1];
    ASSERT_EQ_FMT((usize)3u, slice->length, "%zu");
    ASSERT_EQ_FMT((u32)'c', cvm_string_unit(slice, 0u), "%u");
    ASSERT_EQ_FMT((u32)'d', cvm_string_unit(slice, 1u), "%u");
    ASSERT_EQ_FMT((u32)'e', cvm_string_unit(slice, 2u), "%u");
    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* flatten                                                             */
/* ------------------------------------------------------------------ */

TEST flatten_returns_a_flat_string_and_is_idempotent(void) {
    fixture_enter();
    FRAME(3);
    f_roots[0] = cvm_string_concat(cvm_string_from_utf8(lit("foo")),
                                   cvm_string_from_utf8(lit("bar")));
    ASSERT(cvm_string_kind((CvmString*)f_roots[0]) != CVM_STRF_FLAT);

    CvmString* flat = cvm_string_flatten((CvmString*)f_roots[0]);
    f_roots[1] = flat;
    ASSERT_EQ_FMT(CVM_STRF_FLAT, cvm_string_kind(flat), "%u");
    ASSERT(cvm_string_equal(flat, (CvmString*)f_roots[0]));

    /* Flatten of a flat string is itself. */
    CvmString* again = cvm_string_flatten(flat);
    f_roots[2] = again;
    ASSERT(again == flat);

    /* And flatten of a slice works. */
    f_roots[0] = cvm_string_substring(cvm_string_from_utf8(lit("abcdef")), 1u, 4u);
    flat = cvm_string_flatten((CvmString*)f_roots[0]);
    f_roots[1] = flat;
    ASSERT_EQ_FMT(CVM_STRF_FLAT, cvm_string_kind(flat), "%u");
    ASSERT_EQ_FMT((usize)3u, flat->length, "%zu");
    ASSERT_EQ_FMT((u32)'b', cvm_string_unit(flat, 0u), "%u");
    ASSERT_EQ_FMT((u32)'c', cvm_string_unit(flat, 1u), "%u");
    ASSERT_EQ_FMT((u32)'d', cvm_string_unit(flat, 2u), "%u");
    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* GC: a rope's children and a slice's parent survive a collection     */
/* ------------------------------------------------------------------ */

TEST a_rope_survives_a_collection(void) {
    fixture_enter();
    FRAME(1);
    f_roots[0] = cvm_string_concat(cvm_string_from_utf8(lit("left ")),
                                   cvm_string_from_utf8(lit("right")));

    collect();

    /* The rope's content is intact: the children were traced. */
    CvmString* rope = (CvmString*)f_roots[0];
    ASSERT_EQ_FMT((usize)10u, rope->length, "%zu");
    ASSERT_EQ_FMT((u32)'l', cvm_string_unit(rope, 0u), "%u");
    ASSERT_EQ_FMT((u32)'r', cvm_string_unit(rope, 5u), "%u");
    ASSERT_EQ_FMT((u32)'t', cvm_string_unit(rope, 9u), "%u");

    /* And it still equals the flat string of the same text. */
    f_roots[0] = cvm_string_from_utf8(lit("left right"));
    ASSERT(cvm_string_equal(rope, (CvmString*)f_roots[0]));
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_slice_keeps_its_parent_alive(void) {
    fixture_enter();
    FRAME(2);
    /*
     * The parent is held only by the slice — the frame slot is overwritten with
     * the slice after the substring, so the only thing keeping the parent alive
     * is the slice's GC descriptor tracing it.
     */
    CvmString* parent = cvm_string_from_utf8(lit("the parent string"));
    f_roots[1] = cvm_string_substring(parent, 4u, 10u);
    f_roots[0] = NULL; /* drop the only direct reference to the parent */

    collect();

    /* The slice's content is intact: the parent survived because the slice
     * traces it. */
    CvmString* slice = (CvmString*)f_roots[1];
    ASSERT_EQ_FMT((usize)6u, slice->length, "%zu");
    ASSERT_EQ_FMT((u32)'p', cvm_string_unit(slice, 0u), "%u");
    ASSERT_EQ_FMT((u32)'r', cvm_string_unit(slice, 2u), "%u");

    /* And it equals the flat string of the same text. */
    f_roots[0] = cvm_string_from_utf8(lit("parent"));
    ASSERT(cvm_string_equal(slice, (CvmString*)f_roots[0]));
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_slice_of_a_slice_points_at_the_grandparent(void) {
    fixture_enter();
    FRAME(2);
    f_roots[0] = cvm_string_from_utf8(lit("abcdefgh"));
    /* "cdef" at offset 2 */
    f_roots[1] = cvm_string_substring((CvmString*)f_roots[0], 2u, 6u);
    /* "de" from the slice, at offset 1 within it */
    CvmString* inner = cvm_string_substring((CvmString*)f_roots[1], 1u, 3u);
    f_roots[1] = inner;

    ASSERT_EQ_FMT((usize)2u, inner->length, "%zu");
    ASSERT_EQ_FMT((u32)'d', cvm_string_unit(inner, 0u), "%u");
    ASSERT_EQ_FMT((u32)'e', cvm_string_unit(inner, 1u), "%u");

    /* And the inner slice equals the flat string of the same text. */
    f_roots[0] = cvm_string_from_utf8(lit("de"));
    ASSERT(cvm_string_equal(inner, (CvmString*)f_roots[0]));
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST concat_past_the_depth_cap_flattens(void) {
    fixture_enter();
    FRAME(2);
    /*
     * Concatenate one character at a time, past CVM_ROPE_MAX_DEPTH. The cap
     * does not guarantee the final result is flat — the concat after a flatten
     * starts a new rope — only that the depth never grows past it.
     */
    f_roots[1] = cvm_string_from_utf8(lit("x"));
    f_roots[0] = cvm_string_from_utf8(lit(""));
    for (u32 i = 0; i <= CVM_ROPE_MAX_DEPTH + 2u; i++) {
        f_roots[0] = cvm_string_concat((CvmString*)f_roots[0], (CvmString*)f_roots[1]);
        CvmString* result = (CvmString*)f_roots[0];
        if (cvm_string_kind(result) != CVM_STRF_FLAT) {
            ASSERT((u32)result->reserved <= CVM_ROPE_MAX_DEPTH);
        }
    }
    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(rope_suite) {
    RUN_TEST(a_rope_and_a_flat_string_of_the_same_text_are_equal);
    RUN_TEST(a_slice_and_a_flat_string_of_the_same_text_are_equal);
    RUN_TEST(a_rope_indexes_like_the_flat_string);
    RUN_TEST(a_slice_indexes_like_the_flat_string);
    RUN_TEST(flatten_returns_a_flat_string_and_is_idempotent);
    RUN_TEST(a_rope_survives_a_collection);
    RUN_TEST(a_slice_keeps_its_parent_alive);
    RUN_TEST(a_slice_of_a_slice_points_at_the_grandparent);
    RUN_TEST(concat_past_the_depth_cap_flattens);
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
    RUN_SUITE(rope_suite);
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
