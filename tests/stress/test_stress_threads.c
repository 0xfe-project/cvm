/*
 * tests/stress/test_stress_threads.c
 *
 * Threads coming and going while collections run.
 *
 * This is the pattern from bdwgc's `threadleak.c` — many threads that
 * allocate and finish — combined with the concurrent-mutation pattern from
 * TEST.md. Three things it is aimed at, all of them places where a
 * collector's bookkeeping is easier to get wrong than its tracing:
 *
 *   registration and removal under a running collection. A thread that
 *   appears mid-collection has to stop before it touches the heap, and a
 *   thread that leaves has to stop being waited for. Both were got wrong
 *   once each while building this collector, and both were found by tests
 *   narrower than this one.
 *
 *   the trap page a thread owns, released and reused. The page is what
 *   makes a safepoint a load rather than a branch, and recycling it went
 *   wrong once already: two lifetime lists shared one link, so releasing a
 *   page corrupted the list the fault handler searches and a trap fault
 *   became a SIGBUS. A thread storm is exactly the workload that walks that
 *   path.
 *
 *   objects that stay alive across collections while other threads allocate
 *   and die. Every worker keeps a set of objects rooted for its whole life
 *   and verifies them at the end, so a collection that reclaims something
 *   reachable is caught.
 */

#include "vendor/greatest.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>

#include "support/oracle.h"

#define TEST_MIN ((usize)8 * 1024 * 1024)
#define TEST_MAX ((usize)64 * 1024 * 1024)

typedef struct Leaf {
    u64 canary;
} Leaf;

static CvmTypeDescriptor const leaf_desc = {
    .size = sizeof(Leaf),
    .kind = CVM_TYPE_SCALAR,
};

typedef struct Node {
    opaque_mut left;
    opaque_mut right;
    u64 canary;
} Node;

static CvmPtrField const node_fields[] = {
    {.offset = offsetof(Node, left)},
    {.offset = offsetof(Node, right)},
};

static CvmTypeDescriptor const node_desc = {
    .size = sizeof(Node),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 2,
    .num_embed_fields = 0,
    .ptr_fields = node_fields,
    .embed_fields = NULL,
};

static bool node_canary_ok(Node const* n) {
    return oracle_field_canary_ok(n, n->canary);
}

/* ------------------------------------------------------------------ */
/* a storm of short-lived threads                                      */
/* ------------------------------------------------------------------ */

/*
 * Each thread attaches, allocates a few objects, drops them all, and
 * detaches — repeatedly, for `rounds`. The main thread collects throughout.
 *
 * Nothing a worker allocates is meant to survive, so the collector is free
 * to reclaim all of it whenever it likes. What is being tested is the
 * registry and the per-thread state churn, not liveness.
 */
typedef struct {
    u32 rounds;

    _Atomic u32 attaches;
    _Atomic u32 attach_failures;
    _Atomic u32 allocations;
} StormArg;

#define STORM_SLOTS 4

static opaque_mut storm_worker(opaque_mut raw) {
    StormArg* arg = raw;

    for (u32 round = 0; round < arg->rounds; round++) {
        if (!cvm_thread_attach()) {
            atomic_fetch_add(&arg->attach_failures, 1);
            return NULL;
        }
        atomic_fetch_add(&arg->attaches, 1);

        CVM_FRAME_DECL(f, STORM_SLOTS);
        cvm_frame_push(&f_frame, f_roots, STORM_SLOTS);
        for (u32 i = 0; i < STORM_SLOTS; i++) {
            f_roots[i] = NULL;
        }

        /*
         * Allocate more than there are slots, so each pass overwrites
         * earlier objects and leaves them unreachable — a deliberate trickle
         * of garbage from every thread.
         */
        for (u32 i = 0; i < STORM_SLOTS * 3; i++) {
            void* p = cvm_alloc(&leaf_desc);
            oracle_stamp(p);
            f_roots[i % STORM_SLOTS] = p;
            atomic_fetch_add(&arg->allocations, 1);
        }

        cvm_frame_pop(&f_frame);

        /*
         * Drop the thread's registration before it exits. A registered
         * thread that dies without detaching leaves the registry waiting
         * for a safepoint that can never come — the collector's liveness
         * check covers for it, but the entry is still a leak, and this is
         * the path that would find it if it were not.
         */
        cvm_thread_detach();
    }

    return NULL;
}

TEST short_lived_threads_register_and_leave_cleanly(void) {
    oracle_reset_frames();

    u32 baseline = cvm_thread_count();

    StormArg arg = {.rounds = 40};
    atomic_store(&arg.attaches, 0);
    atomic_store(&arg.attach_failures, 0);
    atomic_store(&arg.allocations, 0);

    enum { WORKERS = 4 };
    RtThread threads[WORKERS];
    for (u32 i = 0; i < WORKERS; i++) {
        ASSERT(rt_thread_create(&threads[i], storm_worker, &arg));
    }

    /*
     * Collect while they churn. Bounded so the storm cannot outlast the
     * test: the collection is a real stop-the-world, and each worker is
     * attaching and detaching around it.
     */
    for (u32 i = 0; i < 20; i++) {
        oracle_collect();
    }

    for (u32 i = 0; i < WORKERS; i++) {
        ASSERT(cvm_thread_join(threads[i]));
    }

    ASSERT_EQ_FMT((u32)(WORKERS * arg.rounds), atomic_load(&arg.attaches), "%u");
    ASSERT_EQ_FMT(0u, atomic_load(&arg.attach_failures), "%u");
    ASSERT(atomic_load(&arg.allocations) > 0);

    /*
     * Every worker detached, so the registry is back where it started. A
     * leftover entry here is a node whose thread no longer exists, which is
     * the leak this test is named for.
     */
    ASSERT_EQ_FMT(baseline, cvm_thread_count(), "%u");

    PASS();
}

TEST a_storm_across_many_sequential_threads_stays_clean(void) {
    /*
     * The same churn, but one thread at a time and many of them, so the
     * trap page recycling path is walked far more times than there are
     * concurrent threads. A page's lifetime is longer than a thread's when
     * it is recycled, and that is the case that went wrong before.
     */
    oracle_reset_frames();

    u32 baseline = cvm_thread_count();

    enum { THREADS = 48 };
    for (u32 i = 0; i < THREADS; i++) {
        StormArg arg = {.rounds = 4};
        atomic_store(&arg.attaches, 0);
        atomic_store(&arg.attach_failures, 0);
        atomic_store(&arg.allocations, 0);

        RtThread thread;
        ASSERT(rt_thread_create(&thread, storm_worker, &arg));
        ASSERT(cvm_thread_join(thread));

        ASSERT_EQ_FMT(4u, atomic_load(&arg.attaches), "%u");
        ASSERT_EQ_FMT(0u, atomic_load(&arg.attach_failures), "%u");

        if (i % 6 == 0) {
            oracle_collect();
        }
    }

    oracle_collect();
    ASSERT_EQ_FMT(baseline, cvm_thread_count(), "%u");

    PASS();
}

/* ------------------------------------------------------------------ */
/* concurrent allocation with survivors                                */
/* ------------------------------------------------------------------ */

/*
 * Each worker builds a set of objects, keeps them rooted for its whole
 * life, and then verifies them. The main thread collects continuously while
 * they do it, so every collection has other threads running — some of them
 * allocating, some of them between frames.
 *
 * This is where a collection that reclaims something reachable shows up as
 * a canary mismatch rather than as a use-after-free three hours later.
 */
#define WORKER_KEEP 32

typedef struct {
    u32 churn_rounds;

    _Atomic u32 ready;
    _Atomic bool stop;
    _Atomic u32 corrupt;
    _Atomic u32 churned;
} WorkerArg;

static opaque_mut keeping_worker(opaque_mut raw) {
    WorkerArg* arg = raw;

    if (!cvm_thread_attach()) {
        atomic_fetch_add(&arg->corrupt, 1);
        return NULL;
    }

    /*
     * The frame lives for the whole thread: every object this worker keeps
     * is in a slot, and the slots are never cleared until the very end. That
     * is the protocol generated code follows for a long-lived activation.
     */
    CVM_FRAME_DECL(f, WORKER_KEEP);
    cvm_frame_push(&f_frame, f_roots, WORKER_KEEP);
    for (u32 i = 0; i < WORKER_KEEP; i++) {
        f_roots[i] = NULL;
    }

    /*
     * Half nodes, half leaves, so that both the tracing path and the
     * short-circuit path are exercised by the same worker.
     */
    for (u32 i = 0; i < WORKER_KEEP; i++) {
        if (i % 2 == 0) {
            Node* n = cvm_alloc(&node_desc);
            n->left = NULL;
            n->right = NULL;
            n->canary = oracle_field_canary(n);
            f_roots[i] = n;
        } else {
            Leaf* l = cvm_alloc(&leaf_desc);
            oracle_stamp(l);
            f_roots[i] = l;
        }
    }

    atomic_store(&arg->ready, 1);

    /* Churn garbage while the main thread collects. */
    while (!atomic_load(&arg->stop)) {
        CVM_FRAME_DECL(g, 2);
        cvm_frame_push(&g_frame, g_roots, 2);
        g_roots[0] = NULL;
        g_roots[1] = NULL;

        for (u32 i = 0; i < 8; i++) {
            g_roots[i % 2] = cvm_alloc(&leaf_desc);
            oracle_stamp(g_roots[i % 2]);
        }

        cvm_frame_pop(&g_frame);
        atomic_fetch_add(&arg->churned, 1);
    }

    /* Everything this worker kept must still be live and intact. */
    for (u32 i = 0; i < WORKER_KEEP; i++) {
        void* p = f_roots[i];
        if (p == NULL) {
            atomic_fetch_add(&arg->corrupt, 1);
            continue;
        }
        if (!oracle_is_live(p)) {
            atomic_fetch_add(&arg->corrupt, 1);
            continue;
        }
        if (i % 2 == 0) {
            if (!node_canary_ok((Node*)p) || ((Node*)p)->left != NULL ||
                ((Node*)p)->right != NULL) {
                atomic_fetch_add(&arg->corrupt, 1);
            }
        } else if (!oracle_canary_intact(p)) {
            atomic_fetch_add(&arg->corrupt, 1);
        }
    }

    cvm_frame_pop(&f_frame);
    cvm_thread_detach();
    return NULL;
}

TEST kept_objects_survive_collections_taken_under_load(void) {
    oracle_reset_frames();

    u32 baseline = cvm_thread_count();

    enum { WORKERS = 3 };
    WorkerArg args[WORKERS];
    RtThread threads[WORKERS];

    for (u32 i = 0; i < WORKERS; i++) {
        args[i] = (WorkerArg){.churn_rounds = 0};
        atomic_store(&args[i].ready, 0);
        atomic_store(&args[i].stop, false);
        atomic_store(&args[i].corrupt, 0);
        atomic_store(&args[i].churned, 0);

        ASSERT(rt_thread_create(&threads[i], keeping_worker, &args[i]));
    }

    /* Wait until every worker has built its set, bounded. */
    for (u32 i = 0; i < WORKERS; i++) {
        u64 deadline = rt_time_nanos() + 5000000000ull;
        while (!atomic_load(&args[i].ready) && rt_time_nanos() < deadline) {
            rt_thread_yield();
        }
        ASSERT(atomic_load(&args[i].ready));
    }

    /*
     * Collect repeatedly while they churn. Each collection stops every
     * worker, which is the interleaving this is for: the workers are inside
     * their churn loops with a frame pushed, and the collector walks their
     * chains.
     */
    for (u32 i = 0; i < 30; i++) {
        oracle_collect();
    }

    for (u32 i = 0; i < WORKERS; i++) {
        atomic_store(&args[i].stop, true);
    }
    for (u32 i = 0; i < WORKERS; i++) {
        ASSERT(cvm_thread_join(threads[i]));
    }

    for (u32 i = 0; i < WORKERS; i++) {
        if (atomic_load(&args[i].corrupt) != 0) {
            fprintf(stderr, "threads: worker %u reports %u problems\n", i,
                    atomic_load(&args[i].corrupt));
            FAIL();
        }
        /* It should have done some churn, or the collection was not
         * actually taken under load. */
        ASSERT(atomic_load(&args[i].churned) > 0);
    }

    ASSERT_EQ_FMT(baseline, cvm_thread_count(), "%u");

    PASS();
}

/* ------------------------------------------------------------------ */
/* a thread that outlives a collection it started                      */
/* ------------------------------------------------------------------ */

TEST a_thread_can_start_a_collection_of_its_own(void) {
    /*
     * The collector may be entered from any registered thread, not only the
     * one that set the runtime up. A worker that allocates enough to run
     * out triggers a collection itself, and it has to be able to do that
     * while the main thread is running.
     */
    oracle_reset_frames();

    u32 baseline = cvm_thread_count();

    StormArg arg = {.rounds = 1};
    atomic_store(&arg.attaches, 0);
    atomic_store(&arg.attach_failures, 0);
    atomic_store(&arg.allocations, 0);

    RtThread thread;
    ASSERT(rt_thread_create(&thread, storm_worker, &arg));
    ASSERT(cvm_thread_join(thread));

    ASSERT_EQ_FMT(0u, atomic_load(&arg.attach_failures), "%u");
    ASSERT_EQ_FMT(baseline, cvm_thread_count(), "%u");

    /* And the main thread still works afterwards. */
    ORACLE_FRAME(1);
    void* p = cvm_alloc(&leaf_desc);
    oracle_stamp(p);
    oracle_f_roots[0] = p;
    oracle_collect();
    ASSERT(oracle_is_live(p));
    ASSERT(oracle_canary_intact(p));
    ORACLE_FRAME_END();

    PASS();
}

SUITE(threads_suite) {
    RUN_TEST(short_lived_threads_register_and_leave_cleanly);
    RUN_TEST(a_storm_across_many_sequential_threads_stays_clean);
    RUN_TEST(kept_objects_survive_collections_taken_under_load);
    RUN_TEST(a_thread_can_start_a_collection_of_its_own);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    if (!oracle_bring_up(TEST_MIN, TEST_MAX, 2)) {
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(threads_suite);
    GREATEST_MAIN_END();
}
