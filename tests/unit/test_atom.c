/*
 * tests/unit/test_atom.c
 *
 * The sharded atom table: one string per text, shards that are each their own
 * table, and the weak-key discipline that keeps it from growing forever.
 *
 * The concurrent cases are their own file — this one is the table's behaviour,
 * which is the same whether one thread is using it or several.
 */

#include "vendor/greatest.h"

#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/atom.h"
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

static str literal(char const* text) {
    usize len = 0;
    while (text[len] != '\0') {
        len++;
    }
    str s = {.ptr = (u8 const*)text, .len = len};
    return s;
}

/* "text-<n>", without a libc formatter: the test binary is hosted but the string
 * still has to be built by hand to keep the shape obvious. */
static CvmString* numbered(char const* prefix, u32 n) {
    char buf[64];
    usize len = 0;
    while (prefix[len] != '\0') {
        buf[len] = prefix[len];
        len++;
    }
    char digits[12];
    u32 count = 0;
    if (n == 0u) {
        digits[count++] = '0';
    }
    while (n > 0u) {
        digits[count++] = (char)('0' + (n % 10u));
        n /= 10u;
    }
    while (count > 0u) {
        buf[len++] = digits[--count];
    }
    str text = {.ptr = (u8 const*)buf, .len = len};
    return cvm_string_from_utf8(text);
}

TEST the_same_text_interned_twice_is_one_string(void) {
    fixture_enter();
    FRAME(4);
    CvmAtomTable* t = cvm_atom_table_new(CVM_ATOM_SHARDS_DEFAULT);
    f_roots[0] = t;
    ASSERT(t != NULL);

    f_roots[1] = cvm_string_from_utf8(literal("alpha"));
    f_roots[2] = cvm_string_from_utf8(literal("alpha"));
    ASSERT(f_roots[1] != f_roots[2]);

    CvmString* first = cvm_atom_intern(t, (CvmString*)f_roots[1]);
    f_roots[3] = first;
    CvmString* second = cvm_atom_intern(t, (CvmString*)f_roots[2]);
    ASSERT(first == second);
    ASSERT_EQ_FMT((usize)1, cvm_atom_count(t), "%zu");
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST lookup_finds_what_was_interned_and_nothing_else(void) {
    fixture_enter();
    FRAME(4);
    CvmAtomTable* t = cvm_atom_table_new(CVM_ATOM_SHARDS_DEFAULT);
    f_roots[0] = t;
    f_roots[1] = cvm_string_from_utf8(literal("beta"));
    CvmString* canonical = cvm_atom_intern(t, (CvmString*)f_roots[1]);
    f_roots[2] = canonical;

    f_roots[3] = cvm_string_from_utf8(literal("beta"));
    ASSERT(cvm_atom_lookup(t, (CvmString*)f_roots[3]) == canonical);

    f_roots[3] = cvm_string_from_utf8(literal("gamma"));
    ASSERT(cvm_atom_lookup(t, (CvmString*)f_roots[3]) == NULL);
    /* Looking is not inserting. */
    ASSERT_EQ_FMT((usize)1, cvm_atom_count(t), "%zu");
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST one_shard_is_a_table_too(void) {
    fixture_enter();
    FRAME(3);
    CvmAtomTable* t = cvm_atom_table_new(1u);
    f_roots[0] = t;
    ASSERT_EQ_FMT((usize)1, cvm_atom_shards(t), "%zu");

    f_roots[1] = cvm_string_from_utf8(literal("one"));
    f_roots[2] = cvm_string_from_utf8(literal("two"));
    CvmString* a = cvm_atom_intern(t, (CvmString*)f_roots[1]);
    CvmString* b = cvm_atom_intern(t, (CvmString*)f_roots[2]);
    ASSERT(a != b);
    ASSERT_EQ_FMT((usize)2, cvm_atom_count(t), "%zu");
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST text_spreads_across_the_shards(void) {
    fixture_enter();
    FRAME(2);
    CvmAtomTable* t = cvm_atom_table_new(CVM_ATOM_SHARDS_DEFAULT);
    f_roots[0] = t;

    /*
     * Two hundred texts, each held in the frame slot in turn, so that only the
     * table's own weak entries are left. Every one of them must be findable, which
     * is the property that a shard choice has to preserve: a string goes in the
     * shard its hash picks and is looked up in that same shard.
     */
    for (u32 i = 0; i < 200u; i++) {
        f_roots[1] = numbered("text-", i);
        CvmString* canonical = cvm_atom_intern(t, (CvmString*)f_roots[1]);
        ASSERT(canonical != NULL);
    }
    ASSERT_EQ_FMT((usize)200, cvm_atom_count(t), "%zu");

    for (u32 i = 0; i < 200u; i++) {
        f_roots[1] = numbered("text-", i);
        ASSERT(cvm_atom_lookup(t, (CvmString*)f_roots[1]) != NULL);
    }
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST text_nobody_holds_is_dropped_by_prune(void) {
    fixture_enter();
    FRAME(2);
    CvmAtomTable* t = cvm_atom_table_new(CVM_ATOM_SHARDS_DEFAULT);
    f_roots[0] = t;

    f_roots[1] = cvm_string_from_utf8(literal("ephemeral"));
    (void)cvm_atom_intern(t, (CvmString*)f_roots[1]);
    ASSERT_EQ_FMT((usize)1, cvm_atom_count(t), "%zu");

    /* Drop the only strong reference there is: the table's entry is weak. */
    f_roots[1] = NULL;
    collect();
    ASSERT_EQ_FMT((usize)1, cvm_atom_prune(t), "%zu");
    ASSERT_EQ_FMT((usize)0, cvm_atom_count(t), "%zu");
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST text_someone_holds_survives_a_collection_and_a_prune(void) {
    fixture_enter();
    FRAME(3);
    CvmAtomTable* t = cvm_atom_table_new(CVM_ATOM_SHARDS_DEFAULT);
    f_roots[0] = t;
    f_roots[1] = cvm_string_from_utf8(literal("kept"));
    CvmString* canonical = cvm_atom_intern(t, (CvmString*)f_roots[1]);
    f_roots[2] = canonical;

    collect();
    ASSERT_EQ_FMT((usize)0, cvm_atom_prune(t), "%zu");
    ASSERT_EQ_FMT((usize)1, cvm_atom_count(t), "%zu");

    f_roots[2] = cvm_string_from_utf8(literal("kept"));
    ASSERT(cvm_atom_lookup(t, (CvmString*)f_roots[2]) == canonical);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_dead_entry_is_invisible_and_the_prune_leaves_one(void) {
    fixture_enter();
    FRAME(3);
    CvmAtomTable* t = cvm_atom_table_new(CVM_ATOM_SHARDS_DEFAULT);
    f_roots[0] = t;

    f_roots[1] = cvm_string_from_utf8(literal("recycled"));
    (void)cvm_atom_intern(t, (CvmString*)f_roots[1]);
    f_roots[1] = NULL;
    collect();

    /*
     * The entry is still in its bucket, with a key that no longer stands for
     * anything — and a key that stands for nothing matches nothing, so this
     * second intern makes a new entry rather than taking that one over. What
     * matters is what a lookup sees: the live string, never the dead one.
     */
    f_roots[2] = cvm_string_from_utf8(literal("recycled"));
    CvmString* again = cvm_atom_intern(t, (CvmString*)f_roots[2]);
    ASSERT(again == f_roots[2]);
    f_roots[1] = again;
    ASSERT(cvm_atom_lookup(t, (CvmString*)f_roots[1]) == again);

    /* And the prune is what takes the dead one out of the bucket it still holds:
     * one entry dropped, one left, and the survivor is the live string. */
    ASSERT_EQ_FMT((usize)1, cvm_atom_prune(t), "%zu");
    ASSERT_EQ_FMT((usize)1, cvm_atom_count(t), "%zu");
    ASSERT(cvm_atom_lookup(t, (CvmString*)f_roots[1]) == again);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_long_run_does_not_grow(void) {
    fixture_enter();
    FRAME(2);
    CvmAtomTable* t = cvm_atom_table_new(CVM_ATOM_SHARDS_DEFAULT);
    f_roots[0] = t;

    /* Intern a batch, drop it, collect and prune; repeat. Neither the entries nor
     * the capacity may keep what the first round used. */
    for (u32 round = 0; round < 8u; round++) {
        for (u32 i = 0; i < 64u; i++) {
            f_roots[1] = numbered("round-", i);
            ASSERT(cvm_atom_intern(t, (CvmString*)f_roots[1]) != NULL);
        }
        ASSERT_EQ_FMT((usize)64, cvm_atom_count(t), "%zu");
        f_roots[1] = NULL;
        collect();
        ASSERT_EQ_FMT((usize)64, cvm_atom_prune(t), "%zu");
        ASSERT_EQ_FMT((usize)0, cvm_atom_count(t), "%zu");
    }
    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(atom_suite) {
    RUN_TEST(the_same_text_interned_twice_is_one_string);
    RUN_TEST(lookup_finds_what_was_interned_and_nothing_else);
    RUN_TEST(one_shard_is_a_table_too);
    RUN_TEST(text_spreads_across_the_shards);
    RUN_TEST(text_nobody_holds_is_dropped_by_prune);
    RUN_TEST(text_someone_holds_survives_a_collection_and_a_prune);
    RUN_TEST(a_dead_entry_is_invisible_and_the_prune_leaves_one);
    RUN_TEST(a_long_run_does_not_grow);
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
    RUN_SUITE(atom_suite);
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
