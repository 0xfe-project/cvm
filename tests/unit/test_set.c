/*
 * tests/unit/test_set.c
 *
 * The set: what the map is with the values left out, and the operations that
 * answer questions about two of them.
 */

#include "vendor/greatest.h"

#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "cvm/hash.h"
#include "cvm/set.h"
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
    return (str){.ptr = (u8 const*)text, .len = len};
}

static CvmString* key(char const* text) {
    return cvm_string_new_ascii(lit(text));
}

/* ------------------------------------------------------------------ */
/* add, has, del, count                                                */
/* ------------------------------------------------------------------ */

TEST adding_a_key_that_was_not_there_says_so(void) {
    fixture_enter();
    FRAME(2);
    CvmHashSet* s = cvm_set_new();
    f_roots[0] = s;
    f_roots[1] = key("alpha");

    ASSERT(cvm_set_add(s, f_roots[1]));
    ASSERT(!cvm_set_add(s, f_roots[1])); /* already there: not new */
    ASSERT_EQ_FMT((usize)1, cvm_set_count(s), "%zu");
    ASSERT(cvm_set_has(s, f_roots[1]));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_key_that_was_never_added_is_not_there(void) {
    fixture_enter();
    FRAME(3);
    CvmHashSet* s = cvm_set_new();
    f_roots[0] = s;
    f_roots[1] = key("in");
    f_roots[2] = key("out");

    (void)cvm_set_add(s, f_roots[1]);
    ASSERT(cvm_set_has(s, f_roots[1]));
    ASSERT(!cvm_set_has(s, f_roots[2]));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST deleting_a_key_takes_it_out(void) {
    fixture_enter();
    FRAME(3);
    CvmHashSet* s = cvm_set_new();
    f_roots[0] = s;
    f_roots[1] = key("gone");
    f_roots[2] = key("kept");

    (void)cvm_set_add(s, f_roots[1]);
    (void)cvm_set_add(s, f_roots[2]);
    ASSERT(cvm_set_del(s, f_roots[1]));
    ASSERT(!cvm_set_has(s, f_roots[1]));
    ASSERT(cvm_set_has(s, f_roots[2]));
    ASSERT(!cvm_set_del(s, f_roots[1])); /* already gone */
    ASSERT_EQ_FMT((usize)1, cvm_set_count(s), "%zu");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST the_same_text_is_one_key_whichever_object_carries_it(void) {
    fixture_enter();
    FRAME(3);
    CvmHashSet* s = cvm_set_new();
    f_roots[0] = s;
    f_roots[1] = key("same");
    f_roots[2] = key("same");

    ASSERT(f_roots[1] != f_roots[2]);    /* two objects */
    ASSERT(cvm_set_add(s, f_roots[1]));  /* new */
    ASSERT(!cvm_set_add(s, f_roots[2])); /* same text: not new */
    ASSERT_EQ_FMT((usize)1, cvm_set_count(s), "%zu");

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* union, intersection, difference                                     */
/* ------------------------------------------------------------------ */

TEST union_holds_every_key_from_both(void) {
    fixture_enter();
    FRAME(4);
    CvmHashSet* a = cvm_set_new();
    CvmHashSet* b = cvm_set_new();
    f_roots[0] = a;
    f_roots[1] = b;

    f_roots[2] = key("a-only");
    (void)cvm_set_add(a, f_roots[2]);
    f_roots[2] = key("shared");
    (void)cvm_set_add(a, f_roots[2]);
    (void)cvm_set_add(b, f_roots[2]);
    f_roots[2] = key("b-only");
    (void)cvm_set_add(b, f_roots[2]);

    CvmHashSet* u = cvm_set_union(a, b);
    f_roots[3] = u;
    ASSERT_EQ_FMT((usize)3, cvm_set_count(u), "%zu");

    f_roots[2] = key("a-only");
    ASSERT(cvm_set_has(u, f_roots[2]));
    f_roots[2] = key("shared");
    ASSERT(cvm_set_has(u, f_roots[2]));
    f_roots[2] = key("b-only");
    ASSERT(cvm_set_has(u, f_roots[2]));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST intersection_holds_what_both_have(void) {
    fixture_enter();
    FRAME(4);
    CvmHashSet* a = cvm_set_new();
    CvmHashSet* b = cvm_set_new();
    f_roots[0] = a;
    f_roots[1] = b;

    f_roots[2] = key("a-only");
    (void)cvm_set_add(a, f_roots[2]);
    f_roots[2] = key("shared");
    (void)cvm_set_add(a, f_roots[2]);
    (void)cvm_set_add(b, f_roots[2]);
    f_roots[2] = key("b-only");
    (void)cvm_set_add(b, f_roots[2]);

    CvmHashSet* i = cvm_set_inter(a, b);
    f_roots[3] = i;
    ASSERT_EQ_FMT((usize)1, cvm_set_count(i), "%zu");

    f_roots[2] = key("shared");
    ASSERT(cvm_set_has(i, f_roots[2]));
    f_roots[2] = key("a-only");
    ASSERT(!cvm_set_has(i, f_roots[2]));
    f_roots[2] = key("b-only");
    ASSERT(!cvm_set_has(i, f_roots[2]));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST difference_holds_what_the_first_has_and_the_second_does_not(void) {
    fixture_enter();
    FRAME(4);
    CvmHashSet* a = cvm_set_new();
    CvmHashSet* b = cvm_set_new();
    f_roots[0] = a;
    f_roots[1] = b;

    f_roots[2] = key("a-only");
    (void)cvm_set_add(a, f_roots[2]);
    f_roots[2] = key("shared");
    (void)cvm_set_add(a, f_roots[2]);
    (void)cvm_set_add(b, f_roots[2]);
    f_roots[2] = key("b-only");
    (void)cvm_set_add(b, f_roots[2]);

    CvmHashSet* d = cvm_set_sub(a, b);
    f_roots[3] = d;
    ASSERT_EQ_FMT((usize)1, cvm_set_count(d), "%zu");

    f_roots[2] = key("a-only");
    ASSERT(cvm_set_has(d, f_roots[2]));
    f_roots[2] = key("shared");
    ASSERT(!cvm_set_has(d, f_roots[2]));
    f_roots[2] = key("b-only");
    ASSERT(!cvm_set_has(d, f_roots[2]));

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* GC and long runs                                                    */
/* ------------------------------------------------------------------ */

TEST the_set_keeps_its_keys_alive(void) {
    fixture_enter();
    FRAME(2);
    CvmHashSet* s = cvm_set_new();
    f_roots[0] = s;

    for (u32 i = 0; i < 50u; i++) {
        char buf[16];
        usize len = (usize)snprintf(buf, sizeof(buf), "key-%u", i);
        f_roots[1] = cvm_string_new8((u8 const*)buf, len);
        (void)cvm_set_add(s, f_roots[1]);
        f_roots[1] = NULL; /* the set is the only holder now */
    }

    collect();

    ASSERT_EQ_FMT((usize)50, cvm_set_count(s), "%zu");
    for (u32 i = 0; i < 50u; i++) {
        char buf[16];
        usize len = (usize)snprintf(buf, sizeof(buf), "key-%u", i);
        f_roots[1] = cvm_string_new8((u8 const*)buf, len);
        ASSERT(cvm_set_has(s, f_roots[1]));
        f_roots[1] = NULL;
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST clear_empties_the_set(void) {
    fixture_enter();
    FRAME(2);
    CvmHashSet* s = cvm_set_new();
    f_roots[0] = s;

    for (u32 i = 0; i < 20u; i++) {
        char buf[16];
        usize len = (usize)snprintf(buf, sizeof(buf), "x-%u", i);
        f_roots[1] = cvm_string_new8((u8 const*)buf, len);
        (void)cvm_set_add(s, f_roots[1]);
    }
    ASSERT_EQ_FMT((usize)20, cvm_set_count(s), "%zu");

    cvm_set_clear(s);
    ASSERT_EQ_FMT((usize)0, cvm_set_count(s), "%zu");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST long_runs_do_not_grow(void) {
    fixture_enter();
    FRAME(2);
    CvmHashSet* s = cvm_set_new();
    f_roots[0] = s;

    for (u32 round = 0; round < 10u; round++) {
        for (u32 i = 0; i < 100u; i++) {
            char buf[16];
            usize len = (usize)snprintf(buf, sizeof(buf), "r-%u", i);
            f_roots[1] = cvm_string_new8((u8 const*)buf, len);
            (void)cvm_set_add(s, f_roots[1]);
        }
        for (u32 i = 0; i < 100u; i++) {
            char buf[16];
            usize len = (usize)snprintf(buf, sizeof(buf), "r-%u", i);
            f_roots[1] = cvm_string_new8((u8 const*)buf, len);
            (void)cvm_set_del(s, f_roots[1]);
        }
        ASSERT_EQ_FMT((usize)0, cvm_set_count(s), "%zu");
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

/* A set with integer keys, built through the same ops the map takes. */
static u32 int_hash(opaque_mut key) {
    return (u32)(((usize)key * 0x9E3779B97F4A7C15ull) >> 32);
}
static bool int_matches(opaque_mut stored, opaque_mut key) {
    return (usize)stored == (usize)key;
}
static bool int_valid(opaque_mut stored) {
    return stored != NULL;
}
static opaque_mut int_store(opaque_mut key) {
    return key;
}
static CvmMapOps const int_ops = {
    .hash = int_hash,
    .hash_stored = int_hash,
    .matches = int_matches,
    .valid = int_valid,
    .store = int_store,
};

TEST a_set_with_integer_keys_works(void) {
    fixture_enter();
    FRAME(1);
    CvmHashSet* s = cvm_set_new_with(&int_ops, 0xABCDu);
    f_roots[0] = s;

    for (u32 i = 1; i <= 20u; i++) {
        ASSERT(cvm_set_add(s, (opaque_mut)(usize)i));
    }
    /* Adding the same integer again is not new. */
    ASSERT(!cvm_set_add(s, (opaque_mut)(usize)5));
    ASSERT_EQ_FMT((usize)20, cvm_set_count(s), "%zu");
    ASSERT(cvm_set_has(s, (opaque_mut)(usize)10));

    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(set_suite) {
    RUN_TEST(adding_a_key_that_was_not_there_says_so);
    RUN_TEST(a_key_that_was_never_added_is_not_there);
    RUN_TEST(deleting_a_key_takes_it_out);
    RUN_TEST(the_same_text_is_one_key_whichever_object_carries_it);
    RUN_TEST(union_holds_every_key_from_both);
    RUN_TEST(intersection_holds_what_both_have);
    RUN_TEST(difference_holds_what_the_first_has_and_the_second_does_not);
    RUN_TEST(the_set_keeps_its_keys_alive);
    RUN_TEST(clear_empties_the_set);
    RUN_TEST(long_runs_do_not_grow);
    RUN_TEST(a_set_with_integer_keys_works);
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
    RUN_SUITE(set_suite);
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
