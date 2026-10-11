/*
 * bench/harness/report.c
 */

#include "report.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* order statistics                                                    */
/* ------------------------------------------------------------------ */

static void sort(uint64_t *v, unsigned n) {
    /* Insertion sort. n is the number of repetitions — seven, or three in a
     * quick run — and a library qsort call would be more code than this. */
    for (unsigned i = 1; i < n; i++) {
        uint64_t key = v[i];
        unsigned j = i;
        while (j > 0 && v[j - 1] > key) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = key;
    }
}

uint64_t bench_median(uint64_t *values, unsigned n) {
    if (n == 0) {
        return 0;
    }
    sort(values, n);
    return values[n / 2];
}

uint64_t bench_percentile(uint64_t *values, unsigned n, unsigned pct) {
    if (n == 0) {
        return 0;
    }
    sort(values, n);
    /*
     * Nearest-rank, with the rank rounded up so that p99 of seven runs is
     * the maximum rather than an interpolation between two of them. With
     * this few samples an interpolated percentile would be a number no run
     * ever produced, which is worse than a conservative one.
     */
    unsigned rank = (unsigned)(((uint64_t)pct * n + 99) / 100);
    if (rank == 0) {
        rank = 1;
    }
    if (rank > n) {
        rank = n;
    }
    return values[rank - 1];
}

uint64_t bench_min(uint64_t *values, unsigned n) {
    if (n == 0) {
        return 0;
    }
    sort(values, n);
    return values[0];
}

uint64_t bench_max(uint64_t *values, unsigned n) {
    if (n == 0) {
        return 0;
    }
    sort(values, n);
    return values[n - 1];
}

/* ------------------------------------------------------------------ */
/* output                                                              */
/* ------------------------------------------------------------------ */

void bench_csv_header(FILE *out) {
    fprintf(out,
            "scenario,backend,mode,runs,"
            "wall_ns_min,wall_ns_median,wall_ns_max,"
            "allocations_median,allocations_min,"
            "collections_median,"
            "pause_max_ns_median,pause_max_ns_max,"
            "pause_total_ns_median,"
            "live_objects,heap_bytes\n");
}

/*
 * Every row is written from the same summary, so the CSV and the printed
 * line cannot disagree about what was measured.
 */
static void summarise(unsigned runs, uint64_t *wall_ns, uint64_t *allocations,
                      uint64_t *collections, uint64_t *pause_max_ns,
                      uint64_t *pause_total_ns, uint64_t *wall_med,
                      uint64_t *wall_min, uint64_t *wall_max,
                      uint64_t *alloc_med, uint64_t *alloc_min,
                      uint64_t *coll_med, uint64_t *pause_med,
                      uint64_t *pause_max, uint64_t *pause_total_med) {
    /*
     * Copied before sorting: the caller's arrays are reused for the next
     * mode, and a function that silently reorders its input is a trap for
     * whoever calls it next.
     */
    uint64_t wall[64], alloc[64], coll[64], pause[64], pause_total[64];
    if (runs > 64) {
        runs = 64;
    }
    memcpy(wall, wall_ns, runs * sizeof(uint64_t));
    memcpy(alloc, allocations, runs * sizeof(uint64_t));
    memcpy(coll, collections, runs * sizeof(uint64_t));
    memcpy(pause, pause_max_ns, runs * sizeof(uint64_t));
    memcpy(pause_total, pause_total_ns, runs * sizeof(uint64_t));

    *wall_min = bench_min(wall, runs);
    *wall_med = bench_median(wall, runs);
    *wall_max = bench_max(wall, runs);
    *alloc_med = bench_median(alloc, runs);
    *alloc_min = bench_min(alloc, runs);
    *coll_med = bench_median(coll, runs);
    *pause_med = bench_median(pause, runs);
    *pause_max = bench_max(pause, runs);
    *pause_total_med = bench_median(pause_total, runs);
}

void bench_csv_row(FILE *out, char const *scenario, char const *backend,
                   char const *mode, unsigned runs, uint64_t *wall_ns,
                   uint64_t *allocations, uint64_t *collections,
                   uint64_t *pause_max_ns, uint64_t *pause_total_ns,
                   uint64_t live_objects, uint64_t heap_bytes) {
    uint64_t wmin, wmed, wmax, amin, amed, cmed, pmed, pmax, ptot;
    summarise(runs, wall_ns, allocations, collections, pause_max_ns,
              pause_total_ns, &wmed, &wmin, &wmax, &amed, &amin, &cmed, &pmed,
              &pmax, &ptot);

    fprintf(out, "%s,%s,%s,%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
            scenario, backend, mode, runs, (unsigned long long)wmin,
            (unsigned long long)wmed, (unsigned long long)wmax,
            (unsigned long long)amed, (unsigned long long)amin,
            (unsigned long long)cmed, (unsigned long long)pmed,
            (unsigned long long)pmax, (unsigned long long)ptot,
            (unsigned long long)live_objects, (unsigned long long)heap_bytes);
}

void bench_print_row(char const *scenario, char const *backend, char const *mode,
                     unsigned runs, uint64_t *wall_ns, uint64_t *allocations,
                     uint64_t *collections, uint64_t *pause_max_ns,
                     uint64_t *pause_total_ns, uint64_t live_objects,
                     uint64_t heap_bytes) {
    uint64_t wmin, wmed, wmax, amin, amed, cmed, pmed, pmax, ptot;
    summarise(runs, wall_ns, allocations, collections, pause_max_ns,
              pause_total_ns, &wmed, &wmin, &wmax, &amed, &amin, &cmed, &pmed,
              &pmax, &ptot);

    /*
     * Wall time in milliseconds with the spread in the same units, because
     * that is the number a reader compares; the raw nanoseconds are in the
     * CSV for anyone who wants them.
     */
    /*
     * pause_all is the stop-the-world share of the run's wall time, said as
     * a percentage so the column is comparable across rows whose walls
     * differ by three orders of magnitude. cvm can also report a total of
     * zero (its mode chose no stop-the-world collections); zero prints as
     * 0.0% rather than dividing by it.
     */
    printf("%-14s %-8s %-8s  wall %8.2f ms [%.2f..%.2f]  alloc %10llu"
           "  gc %6llu  pause_max %8.3f ms  pause_all %5.1f%%  live %9llu"
           "  heap %7.1f MiB\n",
           scenario, backend, mode, (double)wmed / 1e6, (double)wmin / 1e6,
           (double)wmax / 1e6, (unsigned long long)amed,
           (unsigned long long)cmed, (double)pmax / 1e6,
           wmed > 0 ? 100.0 * (double)ptot / (double)wmed : 0.0,
           (unsigned long long)live_objects,
           (double)heap_bytes / (1024.0 * 1024.0));
}
