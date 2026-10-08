/*
 * bench/scenarios/scenario.h
 *
 * What a scenario is, and the shapes they are built from.
 *
 * A scenario is a workload and two numbers about itself. The numbers matter:
 * `heap_hint_bytes` is what alloc-only mode needs so that no collection
 * happens for the whole run, and `default_cadence` is how many allocations
 * apart fixed-cadence mode forces a collection. Both belong to the workload
 * rather than to the harness, because only the workload knows how much it
 * will allocate and how long a run is worth measuring over.
 *
 * The shapes are declared here rather than inside each scenario so that
 * `small_node` in one scenario is the same object as `small_node` in
 * another. BENCH.md §4 explains why each one exists; the two that are a
 * controlled pair (small_node and fat_node: identical pointers, five times
 * the bytes) are the most informative thing in the suite.
 */

#ifndef BENCH_SCENARIO_H
#define BENCH_SCENARIO_H

#include <stddef.h>

#include "collector.h"

/*
 * Which run modes make sense for a scenario.
 *
 * Declared by the scenario rather than decided by the runner, because only
 * the workload knows. Forcing a collection every N allocations in a scenario
 * that allocates four times measures nothing; the flat-array control says so
 * and is not asked for that mode.
 */
#define BENCH_RUN_ALLOC 1u
#define BENCH_RUN_FIXED 2u
#define BENCH_RUN_AUTO 4u
#define BENCH_RUN_ALL_MODES (BENCH_RUN_ALLOC | BENCH_RUN_FIXED | BENCH_RUN_AUTO)

typedef struct BenchScenario {
    char const *name;

    /* Bits from BENCH_RUN_*. A mode not named here is not run, and the
     * results file has no row for it rather than a row measuring nothing. */
    unsigned modes;

    /*
     * Two different heap sizes, because the two kinds of mode measure
     * different things.
     *
     * `heap_hint_bytes` is what alloc-only mode needs so that no collection
     * happens for the whole run: it is sized to the total allocation.
     *
     * `auto_heap_bytes` is the ceiling for auto and fixed-cadence modes,
     * sized to a few times the live set. It exists because cvm's growth
     * policy is uncapped by default and, on an allocation-heavy workload,
     * grows until collections are rare — which makes the "collector" half of
     * the comparison invisible (0 collections, heap hundreds of times the
     * live set). Capping it puts every backend in the same memory envelope,
     * which is the only comparison that means anything. The policy is still
     * cvm's own up to the cap; it just cannot run away.
     */
    size_t heap_hint_bytes;
    size_t auto_heap_bytes;

    /* Allocations between forced collections in fixed-cadence mode. */
    unsigned default_cadence;

    /* One repetition. `iteration` starts at 1 for the first measured run;
     * 0 is the warm-up. A scenario may ignore it, and must not depend on
     * it for correctness — every repetition does the same work. */
    void (*run)(unsigned iteration);
} BenchScenario;

extern BenchScenario const bench_scenario;

/* ------------------------------------------------------------------ */
/* the shapes                                                          */
/* ------------------------------------------------------------------ */

/* 24 bytes, 2 pointers. The common case. */
extern BenchShape const shape_small_node;

/* 136 bytes, 2 pointers. The controlled pair with small_node: same marking
 * work per object, five times the bytes. */
extern BenchShape const shape_fat_node;

/* 96 bytes, 8 pointers. The write-heavy shape. */
extern BenchShape const shape_wide_node;

/* 128 bytes, no pointers. The control that makes the others interpretable:
 * a tracing collector should do almost nothing here. */
extern BenchShape const shape_leaf_blob;

/* ------------------------------------------------------------------ */
/* what a scenario calls                                               */
/* ------------------------------------------------------------------ */

void *bench_alloc(size_t size);
void *bench_alloc_obj(BenchShape const *shape);

/* Defined in run.c. Keeps a stack array alive for the conservative
 * collectors; see harness/roots.h. */
void bench_roots_keep(void *p);

#endif /* BENCH_SCENARIO_H */
