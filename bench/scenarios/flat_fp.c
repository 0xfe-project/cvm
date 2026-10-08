/*
 * bench/scenarios/flat_fp.c
 *
 * An n-body kernel over a flat array of doubles. Almost no pointers.
 *
 * The control that makes everything else interpretable. A tracing collector
 * should do nearly nothing here: there is one object and it holds no
 * pointers, so marking is a single visit and sweeping has one block to
 * consider. Whatever a backend costs on this scenario, it costs in its
 * allocator or its safepoint mechanism — not in its marking.
 *
 * Without this row, a backend that is slow elsewhere cannot be attributed:
 * "slow at tracing" and "slow at everything" produce the same number.
 *
 * The kernel is the classic O(n^2) pairwise gravity, run for a fixed number
 * of steps so the floating-point work is identical for every backend. It
 * exists to give the CPU something to do that the collector cannot
 * influence, which is the whole point of a control.
 */

#include "scenario.h"
#include "roots.h"

#define BODIES 512u
#define STEPS 40u

static void flat_fp(unsigned iteration) {
    (void)iteration;

    BENCH_FRAME_DECL(f, 1);
    BENCH_FRAME_PUSH(f, 1);
    BENCH_ROOT(f, 0) = NULL;

    /* One allocation for the whole run: the array. Sizes are set here so the
     * work is deterministic rather than dependent on a random seed. */
    double *x = bench_alloc(BODIES * sizeof(double));
    double *y = bench_alloc(BODIES * sizeof(double));
    double *vx = bench_alloc(BODIES * sizeof(double));
    double *vy = bench_alloc(BODIES * sizeof(double));
    if (x == NULL || y == NULL || vx == NULL || vy == NULL) {
        BENCH_FRAME_POP(f);
        return;
    }

    /* The array is reachable through the frame, so a collection during the
     * run does not reclaim the thing being computed on. */
    BENCH_ROOT(f, 0) = x;

    for (unsigned i = 0; i < BODIES; i++) {
        x[i] = (double)(i % 32) - 16.0;
        y[i] = (double)(i / 32) - 8.0;
        vx[i] = 0.0;
        vy[i] = 0.0;
    }

    for (unsigned step = 0; step < STEPS; step++) {
        for (unsigned i = 0; i < BODIES; i++) {
            for (unsigned j = i + 1; j < BODIES; j++) {
                double dx = x[j] - x[i];
                double dy = y[j] - y[i];
                double d2 = (dx * dx) + (dy * dy) + 1e-9;
                double inv = 1.0 / (d2 * d2 * d2);
                vx[i] += dx * inv;
                vy[i] += dy * inv;
                vx[j] -= dx * inv;
                vy[j] -= dy * inv;
            }
        }
        for (unsigned i = 0; i < BODIES; i++) {
            x[i] += vx[i] * 0.01;
            y[i] += vy[i] * 0.01;
        }
    }

    /* Consume the result so the loop cannot be optimized away. */
    volatile double sink = x[0] + y[BODIES - 1];
    (void)sink;

    BENCH_FRAME_POP(f);
}

BenchScenario const bench_scenario = {
    .name = "flat-fp",
    .modes = BENCH_RUN_ALLOC | BENCH_RUN_AUTO,
    .heap_hint_bytes = 64ull * 1024ull * 1024ull,
    .auto_heap_bytes = 16ull * 1024ull * 1024ull,
    .default_cadence = 1u, /* no meaningful cadence: this scenario allocates
                            * four times. Fixed-cadence mode is not run for
                            * it; see BENCH.md §6. */
    .run = flat_fp,
};
