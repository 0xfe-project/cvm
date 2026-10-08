/*
 * bench/harness/collector.h
 *
 * What the harness needs from a collector, and nothing else.
 *
 * The shape of this file is the whole fairness argument: every scenario is
 * written once and linked against each backend, so a workload cannot drift
 * between them, and the only thing that differs is the four functions below.
 * A backend that cannot do something the interface asks for says so rather
 * than approximating it — see `collect`.
 */

#ifndef BENCH_COLLECTOR_H
#define BENCH_COLLECTOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The widest object shape in the suite has 8 pointers. */
#define BENCH_SHAPE_MAX_PTRS 8

/*
 * An object shape: how many bytes, and where the pointers are.
 *
 * A precise collector reads this; a conservative one scans the whole object
 * and finds the same pointers. Both are doing their job, which is why the
 * shape is data rather than a comment.
 */
typedef struct BenchShape {
    char const *name;
    size_t size;
    unsigned nptrs;
    size_t ptr_offsets[BENCH_SHAPE_MAX_PTRS];
} BenchShape;

/*
 * What a backend reports about itself when asked.
 *
 * `allocations` is the cross-check required by BENCH.md §2.2: a run whose
 * own count of allocations is zero did not do the work, whatever its wall
 * clock says, and the harness refuses to report such a run as a measurement.
 *
 * `pause_max_ns` is the longest single collection the backend performed,
 * measured by the backend because only it knows when its own collections
 * start and stop. The harness cannot time what happens inside `alloc`.
 */
typedef struct BenchStats {
    uint64_t allocations;
    uint64_t collections;
    uint64_t pause_max_ns;
    uint64_t pause_total_ns;
    /*
     * Objects the collector believes are live, and the heap it is holding.
     *
     * A count rather than a byte total because that is what the three C
     * collectors can actually report: bdwgc's heap size, tgc's live slot
     * count, cvm's live-object count. Deriving bytes from a count by
     * assuming an average object size would be a made-up number in a table
     * of measured ones.
     */
    uint64_t live_objects;
    uint64_t heap_bytes;

    /*
     * Allocations the backend could not satisfy.
     *
     * Zero in every run that is a measurement. A workload that kept
     * allocating after the heap ran out produces a row with the right
     * allocation count and wrong everything else — bdwgc in alloc-only mode
     * on mixed-sizes failed 669,601 allocations this way and the row read as
     * a clean run. The harness refuses a row with any failure in it, because
     * the alternative is a number that looks like a result.
     */
    uint64_t failed_allocations;
} BenchStats;

/*
 * Which run modes this collector can be measured in.
 *
 * Separate from the scenario's own modes, and both have to agree before a
 * row is produced. A scenario may not want a mode; a collector may not be
 * able to provide one. tgc is the case that matters: it has no automatic
 * collection policy at all — the program is expected to call tgc_run when
 * it sees fit — so measuring it in auto mode would be measuring the absence
 * of a policy and calling it a result.
 */
#define BENCH_MODE_SUPPORTS_ALLOC 1u
#define BENCH_MODE_SUPPORTS_FIXED 2u
#define BENCH_MODE_SUPPORTS_AUTO 4u

typedef struct BenchCollector {
    char const *name;

    /* Bits from BENCH_MODE_SUPPORTS_*. */
    unsigned modes;

    /*
     * One-time setup.
     *
     * `heap_hint` is the scenario's estimate of what it needs, passed in
     * every mode: a backend may use it to bound growth as well as to avoid
     * it. `alloc_only` says the run must not collect, which is a stronger
     * statement than a large hint and lets a backend that can disable
     * collection say so rather than hope the heap is big enough.
     *
     * A backend may ignore both.
     */
    bool (*init)(size_t heap_hint, bool alloc_only);
    void (*shutdown)(void);

    /* Allocate `size` zeroed bytes, or NULL. */
    void *(*alloc)(size_t size);

    /* Allocate an object of a declared shape. A conservative backend may
     * implement this as `alloc(shape->size)`; the distinction is the point
     * of the comparison, so it must be a real choice rather than a default
     * nobody looked at. */
    void *(*alloc_typed)(BenchShape const *shape);

    /* Force a collection. Returns false if this backend has no way to be
     * asked, in which case the harness reports the mode as unavailable
     * rather than substituting a different one. */
    bool (*collect)(void);

    void (*stats)(BenchStats *out);
} BenchCollector;

/* Each backend defines exactly one of these. */
extern BenchCollector const bench_collector;

#endif /* BENCH_COLLECTOR_H */
