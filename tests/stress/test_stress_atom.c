/*
 * tests/stress/test_stress_atom.c
 *
 * The atom table under threads.
 *
 * The unit tests hold it to one string per text while one thread uses it. What is
 * left is the property the table exists for: several threads asking for the same
 * text at the same time must end up with the same string, and the table must still
 * be a table while a collection and a prune are running underneath them.
 *
 * Every worker interns a *fresh* string for each text — a different object with
 * the same content — so what is being asserted is not that a pointer was passed
 * around but that the table made one string out of many. The expected answer is
 * the string the main thread interned before any worker started, so a worker that
 * gets anything else got a second entry for one text.
 *
 * The lock-free lookup is exercised on the same texts. It is the read path that
 * has no lock to hide behind, and a collection running while it runs is the case
 * it has to survive: the argument is that a lookup allocates nothing, so it
 * contains no safepoint, so the arrays it is walking cannot be collected
 * underneath it.
 */

#include "vendor/greatest.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/atom.h"
#include "cvm/gc.h"
#include "cvm/str.h"
#include "cvm/vm.h"
#include "gc/collect.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#if !defined(_WIN32)

#define TEST_MIN ((usize)8 * 1024 * 1024)
#define TEST_MAX ((usize)64 * 1024 * 1024)

/* Texts every worker asks for, and the threads asking for them. */
#define SHARED 24
#define WORKERS 4

/* Texts per round that nobody keeps: the weak-key path, under load. */
#define EPHEMERAL 8

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

/* "<prefix><n>", built by hand: the test binary is hosted but the strings it
 * makes are the runtime's, and building them through the runtime's own
 * constructor is what keeps the rest of the test honest. */
static CvmString* text_n(char const* prefix, u32 n) {
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

/* "<prefix><a>-<b>", for texts that are unique to a round. */
static CvmString* text_ab(char const* prefix, u32 a, u32 b) {
    char buf[64];
    usize len = 0;
    while (prefix[len] != '\0') {
        buf[len] = prefix[len];
        len++;
    }
    u32 parts[2] = {a, b};
    for (u32 p = 0; p < 2u; p++) {
        if (p != 0u) {
            buf[len++] = '-';
        }
        char digits[12];
        u32 count = 0;
        u32 v = parts[p];
        if (v == 0u) {
            digits[count++] = '0';
        }
        while (v > 0u) {
            digits[count++] = (char)('0' + (v % 10u));
            v /= 10u;
        }
        while (count > 0u) {
            buf[len++] = digits[--count];
        }
    }
    str text = {.ptr = (u8 const*)buf, .len = len};
    return cvm_string_from_utf8(text);
}

typedef struct AtomArg {
    CvmAtomTable* table;
    /* What the main thread interned first: the only correct answer. */
    CvmString* expected[SHARED];
    u32 rounds;
    /* Also intern texts nobody holds, which is what a prune is for. */
    bool ephemeral;

    _Atomic u32 misses;
    _Atomic u32 interns;
    _Atomic u32 lookups;
    _Atomic u32 done;
} AtomArg;

static opaque_mut atom_worker(opaque_mut raw) {
    AtomArg* arg = raw;
    if (!cvm_thread_attach()) {
        atomic_fetch_add(&arg->done, 1);
        return NULL;
    }

    CVM_FRAME_DECL(f, 2);
    cvm_frame_push(&f_frame, f_roots, 2);
    f_roots[0] = NULL;
    f_roots[1] = NULL;

    for (u32 round = 0; round < arg->rounds; round++) {
        for (u32 i = 0; i < SHARED; i++) {
            /*
             * A string of this thread's own, with text the table already knows.
             * It goes in a root slot because interning allocates, and the
             * candidate has to be reachable across that.
             */
            f_roots[0] = text_n("shared-", i);
            if (f_roots[0] == NULL) {
                continue;
            }
            CvmString* canonical = cvm_atom_intern(arg->table, (CvmString*)f_roots[0]);
            atomic_fetch_add(&arg->interns, 1);
            if (canonical != arg->expected[i]) {
                atomic_fetch_add(&arg->misses, 1);
            }

            /* And the read path with no lock under it, on the same text. */
            CvmString* found = cvm_atom_lookup(arg->table, (CvmString*)f_roots[0]);
            atomic_fetch_add(&arg->lookups, 1);
            if (found != arg->expected[i]) {
                atomic_fetch_add(&arg->misses, 1);
            }
        }

        if (arg->ephemeral) {
            for (u32 i = 0; i < EPHEMERAL; i++) {
                f_roots[1] = text_ab("eph-", round, i);
                if (f_roots[1] == NULL) {
                    continue;
                }
                (void)cvm_atom_intern(arg->table, (CvmString*)f_roots[1]);
                /* Dropped here: the table's entry for it is the only thing left,
                 * and that one is weak. */
                f_roots[1] = NULL;
            }
        }
    }

    cvm_frame_pop(&f_frame);
    cvm_thread_detach();
    atomic_fetch_add(&arg->done, 1);
    return NULL;
}

/*
 * Intern the shared batch, so that every worker has one right answer to compare
 * against, and hand the strings to the caller's root slots.
 */
static bool intern_shared(CvmAtomTable* t, opaque_mut* roots, u32 base) {
    for (u32 i = 0; i < SHARED; i++) {
        roots[base + 1u] = text_n("shared-", i);
        if (roots[base + 1u] == NULL) {
            return false;
        }
        roots[base + 2u + i] = cvm_atom_intern(t, (CvmString*)roots[base + 1u]);
        if (roots[base + 2u + i] == NULL) {
            return false;
        }
    }
    roots[base + 1u] = NULL;
    return true;
}

static bool start_workers(AtomArg* arg, RtThread* threads) {
    for (u32 w = 0; w < WORKERS; w++) {
        if (!rt_thread_create(&threads[w], atom_worker, arg)) {
            return false;
        }
    }
    return true;
}

static bool join_workers(RtThread* threads) {
    for (u32 w = 0; w < WORKERS; w++) {
        if (!cvm_thread_join(threads[w])) {
            return false;
        }
    }
    return true;
}

TEST threads_intern_the_same_text_and_get_one_string(void) {
    fixture_enter();
    FRAME(SHARED + 3);
    CvmAtomTable* t = cvm_atom_table_new(CVM_ATOM_SHARDS_DEFAULT);
    f_roots[0] = t;
    ASSERT(t != NULL);
    ASSERT(intern_shared(t, f_roots, 0));

    AtomArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.table = t;
    arg.rounds = 60;
    arg.ephemeral = false;
    for (u32 i = 0; i < SHARED; i++) {
        arg.expected[i] = (CvmString*)f_roots[2 + i];
    }

    RtThread threads[WORKERS];
    ASSERT(start_workers(&arg, threads));
    ASSERT(join_workers(threads));

    ASSERT_EQ_FMT(0u, atomic_load(&arg.misses), "%u");
    ASSERT(atomic_load(&arg.interns) >= (u32)WORKERS * 60u);
    ASSERT_EQ_FMT((usize)SHARED, cvm_atom_count(t), "%zu");
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_collection_during_the_storm_leaves_the_table_consistent(void) {
    fixture_enter();
    FRAME(SHARED + 3);
    CvmAtomTable* t = cvm_atom_table_new(CVM_ATOM_SHARDS_DEFAULT);
    f_roots[0] = t;
    ASSERT(intern_shared(t, f_roots, 0));

    AtomArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.table = t;
    arg.rounds = 60;
    for (u32 i = 0; i < SHARED; i++) {
        arg.expected[i] = (CvmString*)f_roots[2 + i];
    }

    RtThread threads[WORKERS];
    ASSERT(start_workers(&arg, threads));

    /* Collect underneath them for as long as they run. Every one of the shared
     * strings is rooted here, so a collection that takes one is a bug. */
    while (atomic_load(&arg.done) < WORKERS) {
        collect();
    }
    ASSERT(join_workers(threads));

    ASSERT_EQ_FMT(0u, atomic_load(&arg.misses), "%u");
    ASSERT_EQ_FMT((usize)SHARED, cvm_atom_count(t), "%zu");
    /* And the table still answers after all of that. */
    f_roots[1] = text_n("shared-", 0);
    ASSERT(cvm_atom_lookup(t, (CvmString*)f_roots[1]) == (CvmString*)f_roots[2]);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST pruning_under_workers_does_not_break_the_table(void) {
    fixture_enter();
    FRAME(SHARED + 3);
    CvmAtomTable* t = cvm_atom_table_new(CVM_ATOM_SHARDS_DEFAULT);
    f_roots[0] = t;
    ASSERT(intern_shared(t, f_roots, 0));

    AtomArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.table = t;
    arg.rounds = 40;
    arg.ephemeral = true;
    for (u32 i = 0; i < SHARED; i++) {
        arg.expected[i] = (CvmString*)f_roots[2 + i];
    }

    RtThread threads[WORKERS];
    ASSERT(start_workers(&arg, threads));

    /* Collect and prune underneath them: the prune rebuilds bucket arrays while
     * workers insert into them and readers walk them. */
    while (atomic_load(&arg.done) < WORKERS) {
        collect();
        (void)cvm_atom_prune(t);
    }
    ASSERT(join_workers(threads));

    ASSERT_EQ_FMT(0u, atomic_load(&arg.misses), "%u");

    /* Everything the workers made and dropped is dead now; the shared batch is
     * not, because this thread is holding it. */
    collect();
    (void)cvm_atom_prune(t);
    ASSERT_EQ_FMT((usize)SHARED, cvm_atom_count(t), "%zu");
    for (u32 i = 0; i < SHARED; i++) {
        f_roots[1] = text_n("shared-", i);
        ASSERT(cvm_atom_lookup(t, (CvmString*)f_roots[1]) == arg.expected[i]);
    }
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST churn_returns_the_capacity_to_where_it_started(void) {
    fixture_enter();
    FRAME(SHARED + 3);
    CvmAtomTable* t = cvm_atom_table_new(CVM_ATOM_SHARDS_DEFAULT);
    f_roots[0] = t;
    ASSERT(intern_shared(t, f_roots, 0));

    /*
     * The baseline is the size the table settles at for these entries: the arrays
     * grew to take them and a prune is what sizes them to fit. Measured before any
     * churn, and it has to be the same number after every round — an array that has
     * just grown is *larger* than the entries need, so a comparison against that
     * would be asserting the wrong thing.
     */
    (void)cvm_atom_prune(t);
    usize baseline = cvm_atom_buckets(t);
    ASSERT(baseline > 0u);

    /*
     * Rounds of texts that nobody keeps. Each round grows the shards' arrays and
     * leaves nothing behind, and the prune is what has to give the room back —
     * without it the table ends up the size of its busiest moment, which is a leak
     * with a shape that an exit-time check cannot see.
     */
    for (u32 round = 0; round < 12u; round++) {
        for (u32 i = 0; i < 64u; i++) {
            f_roots[1] = text_ab("churn-", round, i);
            (void)cvm_atom_intern(t, (CvmString*)f_roots[1]);
            f_roots[1] = NULL;
        }
        ASSERT(cvm_atom_buckets(t) >= baseline);
        collect();
        (void)cvm_atom_prune(t);
        ASSERT_EQ_FMT((usize)SHARED, cvm_atom_count(t), "%zu");
        ASSERT_EQ_FMT(baseline, cvm_atom_buckets(t), "%zu");
    }
    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(stress_atom_suite) {
    RUN_TEST(threads_intern_the_same_text_and_get_one_string);
    RUN_TEST(a_collection_during_the_storm_leaves_the_table_consistent);
    RUN_TEST(pruning_under_workers_does_not_break_the_table);
    RUN_TEST(churn_returns_the_capacity_to_where_it_started);
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
    RUN_SUITE(stress_atom_suite);
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
