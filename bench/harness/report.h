/*
 * bench/harness/report.h
 *
 * Turning K runs of one measurement into something a reader can use.
 *
 * Two decisions are made here rather than in a spreadsheet later.
 *
 * The first is that the *median* is reported, not the mean or the best. A
 * collector's wall time is not normally distributed — a run that happens to
 * collect once more than another is a long way off — and the minimum
 * systematically flatters whichever run collected least. The median of an
 * odd number of runs is a real observed run rather than an interpolation.
 *
 * The second is that the spread is reported next to it. A median with no
 * dispersion is a number pretending to be a measurement: two collectors can
 * have the same median and completely different behaviour, one steady and
 * one bimodal. So every row carries the min, the median and the max, and
 * the summary line says how many runs there were.
 */

#ifndef BENCH_REPORT_H
#define BENCH_REPORT_H

#include <stdint.h>
#include <stdio.h>

/* Sorted in place. `values` must hold `n` entries, n >= 1. */
uint64_t bench_median(uint64_t *values, unsigned n);
uint64_t bench_percentile(uint64_t *values, unsigned n, unsigned pct);
uint64_t bench_min(uint64_t *values, unsigned n);
uint64_t bench_max(uint64_t *values, unsigned n);

/* The CSV columns, written once per results file. */
void bench_csv_header(FILE *out);

/*
 * One measurement, one row.
 *
 * `wall_*`, `alloc_*` and `pause_*` are per-run arrays of length `runs` and
 * are sorted in place by this call. `live` and `heap` are the backend's own
 * view at the end of the last run and are not summarised.
 */
void bench_csv_row(FILE *out, char const *scenario, char const *backend,
                   char const *mode, unsigned runs, uint64_t *wall_ns,
                   uint64_t *allocations, uint64_t *collections,
                   uint64_t *pause_max_ns, uint64_t *pause_total_ns,
                   uint64_t live_objects, uint64_t heap_bytes);

/* The same numbers, for a human, on stdout. */
void bench_print_row(char const *scenario, char const *backend, char const *mode,
                     unsigned runs, uint64_t *wall_ns, uint64_t *allocations,
                     uint64_t *collections, uint64_t *pause_max_ns,
                     uint64_t *pause_total_ns, uint64_t live_objects,
                     uint64_t heap_bytes);

#endif /* BENCH_REPORT_H */
