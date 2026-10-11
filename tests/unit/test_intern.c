/*
 * tests/unit/test_intern.c
 *
 * The intern table: dedup by content, and the four ways a weakly-keyed table of
 * interned strings leaks. Three of the four are about what happens *after* a
 * string dies, which is why most of this file is a loop rather than a case.
 */

#include "vendor/greatest.h"

#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "cvm/hash.h"
#include "cvm/intern.h"
#include "cvm/str.h"
#include "cvm/weak.h"
#include "gc/collect.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"
#include "str/dtoa.h"

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

static CvmString* key_from_u32(u32 n) {
    u8 buf[16];
    usize len = cvm_u32toa(buf, n);
    return cvm_string_new8(buf, len);
}

/* ------------------------------------------------------------------ */
/* dedup                                                               */
/* ------------------------------------------------------------------ */

TEST the_same_text_interned_twice_is_one_string(void) {
    fixture_enter();

    FRAME(1);
    CvmHashMap* t = cvm_intern_new();
    f_roots[0] = t;

    CvmString* first = key_from_u32(1234);
    CvmString* canonical = cvm_intern(t, first);
    ASSERT(canonical == first);
    ASSERT_EQ_FMT((usize)1, cvm_intern_count(t), "%zu");

    /* a second object holding the same text is recognized as the same string */
    CvmString* second = key_from_u32(1234);
    ASSERT(second != first);
    ASSERT(cvm_string_equal(first, second));

    CvmString* again = cvm_intern(t, second);
    ASSERT(again == first);
    ASSERT_EQ_FMT((usize)1, cvm_intern_count(t), "%zu");

    /* and the lookup path agrees without inserting */
    ASSERT(cvm_intern_lookup(t, key_from_u32(1234)) == first);
    ASSERT_EQ_FMT((usize)1, cvm_intern_count(t), "%zu");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST lookup_of_absent_text_inserts_nothing(void) {
    fixture_enter();

    FRAME(1);
    CvmHashMap* t = cvm_intern_new();
    f_roots[0] = t;

    ASSERT(cvm_intern_lookup(t, key_from_u32(7)) == NULL);
    ASSERT_EQ_FMT((usize)0, cvm_intern_count(t), "%zu");
    ASSERT_EQ_FMT((usize)0, cvm_map_buckets(t), "%zu");

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* the string is held weakly                                           */
/* ------------------------------------------------------------------ */

TEST a_string_nobody_holds_does_not_stay_interned(void) {
    fixture_enter();

    FRAME(1);
    CvmHashMap* t = cvm_intern_new();
    f_roots[0] = t;

    CvmString* s = key_from_u32(99);
    (void)cvm_intern(t, s);
    ASSERT_EQ_FMT((usize)1, cvm_intern_count(t), "%zu");

    /* nothing roots the string now: the table holds it weakly by design */
    collect();

    /* the entry is still there until someone cleans up ... */
    ASSERT_EQ_FMT((usize)1, cvm_intern_count(t), "%zu");
    ASSERT(cvm_intern_lookup(t, key_from_u32(99)) == NULL);

    /* ... and cleaning up is a mutator-phase call, because deleting an entry
     * can resize the table and the nullify phase may not allocate. */
    ASSERT_EQ_FMT((usize)1, cvm_intern_prune(t), "%zu");
    ASSERT_EQ_FMT((usize)0, cvm_intern_count(t), "%zu");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_string_someone_holds_stays_interned(void) {
    fixture_enter();

    FRAME(2);
    CvmHashMap* t = cvm_intern_new();
    f_roots[0] = t;

    CvmString* s = key_from_u32(5);
    CvmString* canonical = cvm_intern(t, s);
    f_roots[1] = canonical; /* the caller keeps it, which is what keeps it */

    collect();
    ASSERT_EQ_FMT((usize)0, cvm_intern_prune(t), "%zu");
    ASSERT_EQ_FMT((usize)1, cvm_intern_count(t), "%zu");
    ASSERT(cvm_intern_lookup(t, key_from_u32(5)) == canonical);

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST the_wrapper_object_is_released_with_the_entry(void) {
    /*
     * Rule 2. Each entry holds a wrapper object, and the bucket array is what
     * keeps it alive. Pruning has to actually remove it from the array, or every
     * string that was ever interned costs a wrapper forever — a leak that no
     * test of lookups would show.
     */
    fixture_enter();

    FRAME(2);
    CvmHashMap* t = cvm_intern_new();
    f_roots[0] = t;

    (void)cvm_intern(t, key_from_u32(42));

    usize slot = cvm_map_slot(t, key_from_u32(42));
    ASSERT(slot != CVM_MAP_NO_SLOT);

    void* wrapper = cvm_map_key_at(t, slot);
    ASSERT(wrapper != NULL);
    void* weak_wrapper = cvm_weak_ref_new(wrapper);
    f_roots[1] = weak_wrapper;

    collect();
    /* the wrapper is still reachable, from the bucket array */
    ASSERT(cvm_weak_ref_target(weak_wrapper) == wrapper);

    ASSERT_EQ_FMT((usize)1, cvm_intern_prune(t), "%zu");
    collect();
    /*
     * Not yet, and this is the retire list, not a leak. A prune rebuilds the
     * table, and this table is read without a lock -- so the map keeps the
     * arrays it replaced alive for one more collection, because a reader that
     * snapshotted them is still entitled to be walking them (see
     * cvm/retire.h). The wrapper is in those arrays, so it outlives the prune
     * by exactly that grace period.
     */
    ASSERT(cvm_weak_ref_target(weak_wrapper) == wrapper);
    collect();
    /* and now the grace period is over and nothing points at it */
    ASSERT(cvm_weak_ref_target(weak_wrapper) == NULL);

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST an_entry_whose_string_died_is_pruned_not_reused(void) {
    /*
     * A dead entry cannot be recognised as holding the same text: the text is
     * what died. So interning a fresh string with the same text inserts beside
     * it, and the dead one is removed by the next prune rather than by the
     * insert.
     *
     * This is the case that says pruning is not optional. Skip it and a program
     * that interns transient strings keeps one dead entry per distinct text it
     * ever saw — which is exactly the unbounded set that interning weakly was
     * meant to avoid.
     */
    fixture_enter();

    FRAME(1);
    CvmHashMap* t = cvm_intern_new();
    f_roots[0] = t;

    (void)cvm_intern(t, key_from_u32(11));
    collect();
    ASSERT(cvm_intern_lookup(t, key_from_u32(11)) == NULL);

    CvmString* fresh = key_from_u32(11);
    CvmString* canonical = cvm_intern(t, fresh);
    ASSERT(canonical == fresh);
    /* the dead entry is still counted, beside the live one */
    ASSERT_EQ_FMT((usize)2, cvm_intern_count(t), "%zu");
    ASSERT(cvm_intern_lookup(t, key_from_u32(11)) == fresh);

    /* and pruning takes exactly the dead one */
    ASSERT_EQ_FMT((usize)1, cvm_intern_prune(t), "%zu");
    ASSERT_EQ_FMT((usize)1, cvm_intern_count(t), "%zu");
    ASSERT(cvm_intern_lookup(t, key_from_u32(11)) == fresh);

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* pruning                                                             */
/* ------------------------------------------------------------------ */

TEST prune_removes_only_what_died(void) {
    fixture_enter();

    FRAME(3);
    CvmHashMap* t = cvm_intern_new();
    f_roots[0] = t;

    /* one that stays, held by the caller */
    CvmString* kept = key_from_u32(1);
    f_roots[1] = cvm_intern(t, kept);

    /* and twenty that do not */
    for (u32 i = 100; i < 120; i++) {
        (void)cvm_intern(t, key_from_u32(i));
    }
    ASSERT_EQ_FMT((usize)21, cvm_intern_count(t), "%zu");

    collect();
    ASSERT_EQ_FMT((usize)20, cvm_intern_prune(t), "%zu");
    ASSERT_EQ_FMT((usize)1, cvm_intern_count(t), "%zu");
    ASSERT(cvm_intern_lookup(t, key_from_u32(1)) == (CvmString*)f_roots[1]);
    ASSERT(cvm_intern_lookup(t, key_from_u32(100)) == NULL);

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST pruning_gives_the_capacity_back(void) {
    /*
     * Rule 3. Pruning empties buckets; a table that kept the array its busiest
     * moment needed would have traded an entry leak for a memory leak. The
     * rebuild is also where the sizing happens, so the array comes back to fit
     * what is left in the same pass.
     */
    fixture_enter();

    FRAME(2);
    CvmHashMap* t = cvm_intern_new();
    f_roots[0] = t;

    CvmString* kept = key_from_u32(1);
    f_roots[1] = cvm_intern(t, kept);

    for (u32 i = 0; i < 500; i++) {
        (void)cvm_intern(t, key_from_u32(1000u + i));
    }
    usize peak = cvm_map_buckets(t);
    ASSERT(peak > 32);

    collect();
    ASSERT_EQ_FMT((usize)500, cvm_intern_prune(t), "%zu");
    ASSERT_EQ_FMT((usize)1, cvm_intern_count(t), "%zu");
    ASSERT(cvm_map_buckets(t) < peak);
    ASSERT_EQ_FMT((usize)4, cvm_map_buckets(t), "%zu");

    /* the survivor is still findable afterwards */
    ASSERT(cvm_intern_lookup(t, key_from_u32(1)) == (CvmString*)f_roots[1]);

    /* a second prune has nothing to do, and does not allocate to find that out */
    ASSERT_EQ_FMT((usize)0, cvm_intern_prune(t), "%zu");
    ASSERT_EQ_FMT((usize)4, cvm_map_buckets(t), "%zu");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST prune_empties_a_table_that_is_entirely_dead(void) {
    /*
     * Everything in the table has died, so a prune must remove exactly as many
     * entries as were inserted — no more and no less — and leave nothing
     * occupying a bucket.
     *
     * This is the shape that found the bug the current implementation is
     * written around: removing an entry in place closes its probe chain by
     * moving the entries behind it backwards, and a chain that wraps past the
     * end of the array pulls an entry from the front into a bucket a downward
     * scan has already passed, so a walk-and-delete leaves dead entries behind.
     * Several hundred entries matter here — the long-running case at the bottom
     * of this file is what reproduced it, and this is the same failure stated
     * directly.
     */
    fixture_enter();

    FRAME(1);
    CvmHashMap* t = cvm_intern_new();
    f_roots[0] = t;

    enum { N = 400 };
    for (u32 i = 0; i < N; i++) {
        (void)cvm_intern(t, key_from_u32(i));
    }
    ASSERT_EQ_FMT((usize)N, cvm_intern_count(t), "%zu");

    collect();
    ASSERT_EQ_FMT((usize)N, cvm_intern_prune(t), "%zu");
    ASSERT_EQ_FMT((usize)0, cvm_intern_count(t), "%zu");
    /* and nothing is left occupying a bucket */
    ASSERT(cvm_map_next(t, 0) == CVM_MAP_NO_SLOT);

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_long_run_does_not_grow(void) {
    /*
     * The plan's "长跑不增长" case, and the only way to see any of the four leaks
     * from outside: run the table hard, release everything each round, and check
     * that the entry count *and the bucket count* come back to where they
     * started. A table that leaks an entry, a wrapper, or its capacity shows up
     * here and nowhere else.
     */
    fixture_enter();

    FRAME(2);
    CvmHashMap* t = cvm_intern_new();
    f_roots[0] = t;

    CvmString* resident = key_from_u32(123456);
    f_roots[1] = cvm_intern(t, resident);

    usize baseline_count = cvm_intern_count(t);

    for (u32 round = 0; round < 30; round++) {
        for (u32 i = 0; i < 100; i++) {
            (void)cvm_intern(t, key_from_u32(round * 1000u + i));
        }
        ASSERT_EQ_FMT(baseline_count + 100, cvm_intern_count(t), "%zu");

        /* drop every reference to this round's strings, then collect */
        collect();
        usize pruned = cvm_intern_prune(t);
        ASSERT_EQ_FMT((usize)100, pruned, "%zu");

        ASSERT_EQ_FMT(baseline_count, cvm_intern_count(t), "%zu");
        ASSERT_EQ_FMT((usize)4, cvm_map_buckets(t), "%zu");
    }

    /* the resident string came through all of it */
    ASSERT(cvm_intern_lookup(t, key_from_u32(123456)) == (CvmString*)f_roots[1]);

    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(intern_suite) {
    RUN_TEST(the_same_text_interned_twice_is_one_string);
    RUN_TEST(lookup_of_absent_text_inserts_nothing);

    RUN_TEST(a_string_nobody_holds_does_not_stay_interned);
    RUN_TEST(a_string_someone_holds_stays_interned);
    RUN_TEST(the_wrapper_object_is_released_with_the_entry);
    RUN_TEST(an_entry_whose_string_died_is_pruned_not_reused);

    RUN_TEST(prune_removes_only_what_died);
    RUN_TEST(pruning_gives_the_capacity_back);

    RUN_TEST(prune_empties_a_table_that_is_entirely_dead);
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
    RUN_SUITE(intern_suite);
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
