/*
 * bench/scenarios/growth.c
 *
 * The live set grows by about 1.5 each round, and nothing is ever dropped.
 *
 * The scenario for heap policy rather than for mechanism: every backend
 * eventually has to decide how much to grow by, and this is where a
 * twenty-year-old default has an advantage over a policy written recently.
 * BENCH.md §5 predicts bdwgc wins here, and the prediction is recorded so
 * that being wrong is informative.
 *
 * The ratio matters. cvm grows the heap by a factor below the 128 MiB
 * threshold and by √2 above it, so a live set growing at 1.49× crosses that
 * threshold partway through the run and the growth policy changes underneath
 * it. Reporting collections per megabyte of live set on either side of the
 * threshold is the measurement that shows whether the change helped.
 *
 * Nothing is ever freed, so the live set is exactly what has been allocated.
 * That is deliberate: a scenario that also dropped things would measure
 * reclamation, and this one is about what happens when there is nothing to
 * reclaim.
 */

#include <stdio.h>

#include "scenario.h"
#include "roots.h"

#define ROUNDS 8u
#define BASE 4000u
#define GROWTH_NUM 149u /* 1.49, not 1.5: see BENCH.md §5 */
#define GROWTH_DEN 100u

/*
 * A block of objects, kept alive as a unit.
 *
 * The scenario holds one object per round — a small node whose second
 * pointer field chains to the next block — and reaches the rest through a
 * flat array of pointers that is itself allocated in the heap. That is what
 * a growing structure looks like to a collector: a few roots, most of the
 * live set reachable through them.
 */
typedef struct Block {
    void *next;
    void *payload;
} Block;

static void growth(unsigned iteration) {
    (void)iteration;

    BENCH_FRAME_DECL(f, 3);
    BENCH_FRAME_PUSH(f, 3);
    for (unsigned i = 0; i < 3; i++) {
        BENCH_ROOT(f, i) = NULL;
    }

    size_t target = BASE;

    for (unsigned round = 0; round < ROUNDS; round++) {
        /*
         * This round's objects. The previous block is kept: BENCH_ROOT(f,1)
         * chains to it, so the live set accumulates rather than turning
         * over, which is the whole point.
         */
        void *head = NULL;
        for (size_t i = 0; i < target; i++) {
            void *p = bench_alloc_obj(&shape_small_node);
            if (p == NULL) {
                break;
            }
            Block *b = p;
            b->next = head;
            b->payload = NULL;
            head = p;
        }

        void *block = bench_alloc(sizeof(Block));
        if (block != NULL) {
            Block *b = block;
            b->next = BENCH_ROOT(f, 1);
            b->payload = head;
            BENCH_ROOT(f, 1) = block;
        }

        BENCH_ROOT(f, 0) = head;

        /* 1.49x per round. Integer arithmetic so the ratio is exact and
         * reproducible rather than dependent on a floating-point mode. */
        target = (target * GROWTH_NUM) / GROWTH_DEN;
        if (target == 0) {
            target = 1;
        }
    }

    BENCH_FRAME_POP(f);
}

BenchScenario const bench_scenario = {
    .name = "growth-1.5",
    .modes = BENCH_RUN_AUTO,
    .heap_hint_bytes = 512ull * 1024ull * 1024ull,
    .auto_heap_bytes = 32ull * 1024ull * 1024ull,
    .default_cadence = 20000u,
    .run = growth,
};
