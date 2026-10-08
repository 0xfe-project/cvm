/*
 * bench/scenarios/mixed_sizes.c
 *
 * Sizes drawn at random across cvm's two boundaries, with random lifetimes.
 *
 * This walks every allocation path a collector has — inline, medium, large —
 * and it is the fragmentation scenario: sizes chosen so that allocations
 * land on line boundaries in awkward ways and lifetimes chosen so that holes
 * accumulate without ever becoming a full free block.
 *
 * The seed is fixed and printed, so a surprising result can be reproduced
 * exactly. A random scenario with an unrecorded seed is an anecdote.
 */

#include <stdio.h>

#include "scenario.h"
#include "roots.h"

#define ROUNDS 400000u
#define SLOTS 64u
#define SEED 0x9e3779b9u

/*
 * Sizes chosen around cvm's two boundaries: the 256-byte inline limit and
 * the 8 KiB large-object threshold. Landing on the boundaries rather than
 * around them is deliberate — that is where an off-by-one in a size class
 * shows up.
 */
static size_t pick_size(unsigned *state) {
    *state = (*state * 1103515245u) + 12345u;
    unsigned r = (*state >> 16) & 0xffu;

    if (r < 64) {
        return 16 + (r % 48); /* well inside a line */
    }
    if (r < 128) {
        return 240 + (r % 32); /* straddling the inline limit */
    }
    if (r < 192) {
        return 512 + (r % 1536); /* medium: several lines */
    }
    return 7000 + (r % 2500); /* straddling the large-object threshold */
}

static void mixed(unsigned iteration) {
    (void)iteration;

    BENCH_FRAME_DECL(f, SLOTS);
    BENCH_FRAME_PUSH(f, SLOTS);
    for (unsigned i = 0; i < SLOTS; i++) {
        BENCH_ROOT(f, i) = NULL;
    }

    unsigned state = SEED;

    for (unsigned round = 0; round < ROUNDS; round++) {
        unsigned slot = (state >> 8) % SLOTS;
        state = (state * 1103515245u) + 12345u;

        /* Half the time drop whatever was there, half the time replace it.
         * That is what makes lifetimes random rather than uniform: some
         * slots turn over constantly and some hold for a long time. */
        if ((state & 1u) != 0) {
            BENCH_ROOT(f, slot) = NULL;
        } else {
            BENCH_ROOT(f, slot) = bench_alloc(pick_size(&state));
        }
    }

    BENCH_FRAME_POP(f);
}

BenchScenario const bench_scenario = {
    .name = "mixed-sizes",
    .modes = BENCH_RUN_ALL_MODES,
    .heap_hint_bytes = 2ull * 1024ull * 1024ull * 1024ull,
    .auto_heap_bytes = 256ull * 1024ull * 1024ull,
    .default_cadence = 20000u,
    .run = mixed,
};
