/*
 * tests/stress/test_stress_sweep_race.c
 *
 * Sweeping, while the program is running.
 *
 * The sweep is the one part of this collector that runs concurrently with the
 * program, and it rests on an assumption worth stating exactly: **the blocks a
 * sweep walks are not handed to a mutator while it walks them.** The sweep
 * decides which lines of a block are free by reading the object metadata in
 * them; a mutator that allocated into the same block while that was happening
 * would be writing objects the sweep is about to declare free.
 *
 * The design has one mechanism for this, and it is a range: the reserve run is
 * excluded from sweeping by address, so a mutator resuming immediately
 * allocates out of the reserve and not out of anything the sweep is walking.
 * Whether that is *enough* is what this test is for — it makes the overlap as
 * likely as it can and checks the result.
 *
 * A second reason to have it: this is the assumption that eager block freeing
 * failed against. That change freed empty blocks before the sweep reached
 * them, a mutator took one immediately, and the sweep walked it afterwards —
 * correct in neither direction. If this test goes red when that work is
 * retried, the assumption is what is being broken.
 *
 * The oracle is the canary. Each worker builds a chain of nodes whose contents
 * are a value derived from the node's own address, allocates and drops heavily
 * around it to drive collections and their sweeps, and walks the chain after
 * every round. An object reclaimed while it was still reachable shows up as a
 * canary mismatch rather than as a corrupt graph three hours later.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>

#include "support/oracle.h"

#define TEST_MIN ((usize)8 * 1024 * 1024)
#define TEST_MAX ((usize)64 * 1024 * 1024)

/* Four workers, so the sweep is parallel as well as concurrent. */
#define SWEEP_WORKERS 4
#define SWEEP_ROUNDS 4000
#define SWEEP_CHURN 24
#define CHAIN_LENGTH 8

typedef struct Chain Chain;

struct Chain {
    Chain* next;
    u64 canary;
};

static CvmPtrField const chain_fields[] = {{.offset = offsetof(Chain, next)}};

static CvmTypeDescriptor const chain_desc = {
    .size = sizeof(Chain),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 1,
    .ptr_fields = chain_fields,
};

typedef struct {
    u32 rounds;

    _Atomic u32 corrupt;
    _Atomic u32 allocations;
    _Atomic u32 attach_failures;
} SweepArg;

/* Extend the chain by one node, keeping the head in the caller's slot. */
static bool extend(opaque_mut* head) {
    Chain* node = cvm_alloc(&chain_desc);
    if (node == NULL) {
        return false;
    }
    node->next = *head;
    node->canary = oracle_field_canary(node);
    *head = node;
    return true;
}

/* Walk the chain, checking every link and every canary. */
static bool walk_is_intact(Chain* head) {
    for (Chain* node = head; node != NULL; node = node->next) {
        if (!oracle_field_canary_ok(node, node->canary)) {
            return false;
        }
    }
    return true;
}

/*
 * One worker's whole life.
 *
 * The churn is the point: allocating without keeping is what exhausts the
 * heap, which triggers a collection, which leaves a sweep behind to be driven
 * by the *next* allocation. So the loop that allocates is the loop that
 * sweeps, and every round puts an allocation on top of a sweep that is in
 * flight.
 */
static opaque_mut sweep_worker(opaque_mut raw) {
    SweepArg* arg = raw;

    if (!cvm_thread_attach()) {
        atomic_fetch_add(&arg->attach_failures, 1);
        return NULL;
    }

    /*
     * Two slots for the whole thread: the standing chain in the first, and a
     * scratch object in the second that is overwritten every few rounds. The
     * overwritten ones are garbage the sweep has to reclaim, which is what
     * keeps it moving.
     */
    CVM_FRAME_DECL(f, 2);
    cvm_frame_push(&f_frame, f_roots, 2);
    f_roots[0] = NULL;
    f_roots[1] = NULL;

    for (u32 i = 0; i < CHAIN_LENGTH; i++) {
        if (!extend(&f_roots[0])) {
            atomic_fetch_add(&arg->corrupt, 1);
            break;
        }
    }

    for (u32 round = 0; round < arg->rounds; round++) {
        for (u32 k = 0; k < SWEEP_CHURN; k++) {
            if (cvm_alloc(&chain_desc) != NULL) {
                atomic_fetch_add(&arg->allocations, 1);
            }
        }

        if (!walk_is_intact(f_roots[0])) {
            atomic_fetch_add(&arg->corrupt, 1);
            break;
        }

        /* Replace the scratch object, which drops the previous one. */
        if ((round % 5) == 0) {
            f_roots[1] = cvm_alloc(&chain_desc);
            if (f_roots[1] != NULL) {
                ((Chain*)f_roots[1])->next = NULL;
                ((Chain*)f_roots[1])->canary = oracle_field_canary(f_roots[1]);
            }
        }
    }

    cvm_frame_pop(&f_frame);
    cvm_thread_detach();
    return NULL;
}

/* The same churn on the test's own thread, so the sweep is driven from two
 * places at once rather than from one worker at a time. */
static void churn_here(u32 rounds) {
    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = NULL;

    for (u32 round = 0; round < rounds; round++) {
        for (u32 k = 0; k < SWEEP_CHURN; k++) {
            (void)cvm_alloc(&chain_desc);
        }
    }

    cvm_frame_pop(&f_frame);
}

TEST allocation_overlaps_a_sweep_in_flight(void) {
    SweepArg args[SWEEP_WORKERS];
    RtThread threads[SWEEP_WORKERS];

    for (u32 i = 0; i < SWEEP_WORKERS; i++) {
        args[i] = (SweepArg){.rounds = SWEEP_ROUNDS};
        atomic_init(&args[i].corrupt, 0);
        atomic_init(&args[i].allocations, 0);
        atomic_init(&args[i].attach_failures, 0);

        ASSERT(rt_thread_create(&threads[i], sweep_worker, &args[i]));
    }

    churn_here(SWEEP_ROUNDS / 4);

    u32 allocations = 0;
    for (u32 i = 0; i < SWEEP_WORKERS; i++) {
        ASSERT(cvm_thread_join(threads[i]));
        ASSERT_EQ_FMT(0u, atomic_load(&args[i].attach_failures), "%u");
        ASSERT_EQ_FMT(0u, atomic_load(&args[i].corrupt), "%u");
        allocations += atomic_load(&args[i].allocations);
    }

    /*
     * A run where nothing was allocated proves nothing about sweeping: the
     * collections never happened, so the sweeps never did either.
     */
    ASSERT(allocations > 0);

    PASS();
}

/*
 * The same overlap, but with the collections forced rather than left to the
 * allocation policy — and forced *while* the workers are allocating, so the
 * window between "the body is over" and "the sweep has finished" is as wide as
 * this collector can make it.
 */
TEST a_forced_collection_does_not_disturb_the_chain(void) {
    SweepArg args[SWEEP_WORKERS];
    RtThread threads[SWEEP_WORKERS];

    for (u32 i = 0; i < SWEEP_WORKERS; i++) {
        args[i] = (SweepArg){.rounds = SWEEP_ROUNDS / 4};
        atomic_init(&args[i].corrupt, 0);
        atomic_init(&args[i].allocations, 0);
        atomic_init(&args[i].attach_failures, 0);

        ASSERT(rt_thread_create(&threads[i], sweep_worker, &args[i]));
    }

    for (u32 i = 0; i < 32; i++) {
        cvm_collect();
        churn_here(32);
    }

    for (u32 i = 0; i < SWEEP_WORKERS; i++) {
        ASSERT(cvm_thread_join(threads[i]));
        ASSERT_EQ_FMT(0u, atomic_load(&args[i].corrupt), "%u");
    }

    PASS();
}

/*
 * After all of that, the heap must still be a heap: a collection finishes, the
 * reachable set is what it should be, and the collector's own consistency
 * check — which runs in debug builds — finds nothing wrong.
 */
TEST the_heap_is_consistent_after_the_overlap(void) {
    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = NULL;

    ASSERT(extend(&f_roots[0]));
    Chain* kept = f_roots[0];

    oracle_collect();

    ASSERT(walk_is_intact(kept));
    ASSERT(oracle_is_live(kept));

    cvm_frame_pop(&f_frame);
    oracle_collect();

    /* Nothing roots it now, so the collection should have taken it. */
    ASSERT(!oracle_is_live(kept));

    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    if (!oracle_bring_up(TEST_MIN, TEST_MAX, SWEEP_WORKERS)) {
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_TEST(allocation_overlaps_a_sweep_in_flight);
    RUN_TEST(a_forced_collection_does_not_disturb_the_chain);
    RUN_TEST(the_heap_is_consistent_after_the_overlap);
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
