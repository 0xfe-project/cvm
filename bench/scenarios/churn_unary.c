/*
 * bench/scenarios/churn_unary.c
 *
 * Allocate and drop. Nothing survives, so nothing is ever marked.
 *
 * This is the allocator's ceiling and cvm's core claim: a bump pointer
 * against a free list against a hash-table insert. The live set is empty, so
 * the only thing measured is what an allocation costs and what a collection
 * costs when it has nothing to trace.
 *
 * One object is kept alive so that a collector cannot decide the whole heap
 * is garbage and hand back a heap it never scanned.
 */

#include "scenario.h"
#include "roots.h"

#define CHURN_OBJECTS 4000000u
#define KEEP_INTERVAL 64u

static void churn(unsigned iteration) {
    (void)iteration;

    BENCH_FRAME_DECL(f, 2);
    BENCH_FRAME_PUSH(f, 2);
    BENCH_ROOT(f, 0) = NULL;
    BENCH_ROOT(f, 1) = NULL;

    for (unsigned i = 0; i < CHURN_OBJECTS; i++) {
        void *p = bench_alloc_obj(&shape_small_node);
        if (p == NULL) {
            break;
        }
        if ((i % KEEP_INTERVAL) == 0) {
            /* Overwritten, not accumulated: this keeps one object alive at a
             * time, which is enough to make the heap look inhabited. */
            BENCH_ROOT(f, 0) = p;
        }
    }

    BENCH_FRAME_POP(f);
}

BenchScenario const bench_scenario = {
    .name = "churn-unary",
    .modes = BENCH_RUN_ALLOC | BENCH_RUN_FIXED,
    .heap_hint_bytes = 1024ull * 1024ull * 1024ull,
    .auto_heap_bytes = 64ull * 1024ull * 1024ull,
    .default_cadence = 100000u,
    .run = churn,
};
