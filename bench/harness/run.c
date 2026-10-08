/*
 * bench/harness/run.c
 *
 * The timing loop. One scenario, one backend, one mode, K repetitions.
 *
 * Everything the harness decides is decided here, so that a scenario is
 * only ever a workload: how many times to repeat, when to force a
 * collection, what to do about the first run, and what counts as a
 * measurement at all.
 *
 *   run <binary> [--mode=alloc|fixed|auto] [--reps=K] [--cadence=N]
 *                [--out=file.csv] [--no-warmup]
 *
 * The scenario supplies its own heap estimate and cadence; see
 * scenarios/scenario.h.
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "collector.h"
#include "report.h"
#include "roots.h"
#include "scenario.h"

#define BENCH_MAX_REPS 64

/* The harness's own inline, so the fixed-cadence check costs a load and a
 * predictable branch in the modes that do not use it. */
#if defined(__GNUC__) || defined(__clang__)
#define CVM_HARNESS_INLINE __attribute__((always_inline)) inline
#else
#define CVM_HARNESS_INLINE inline
#endif

/* ------------------------------------------------------------------ */
/* the mode, and the one hook that implements it                       */
/* ------------------------------------------------------------------ */

typedef enum BenchMode {
    BENCH_MODE_ALLOC = 0,
    BENCH_MODE_FIXED,
    BENCH_MODE_AUTO,
} BenchMode;

static BenchMode mode = BENCH_MODE_AUTO;
static unsigned cadence = 0;
static bool forced_collect_unsupported = false;

/*
 * Fixed-cadence bookkeeping, kept out of every other mode's way.
 *
 * The first version incremented a counter and took it modulo the cadence on
 * every allocation in every mode — an integer division on the allocation
 * path, for a counter nothing else reads. The per-repetition allocation
 * count comes from the backend's own stats, so this exists only to decide
 * when to force a collection, and it is a countdown rather than a modulo so
 * that even in fixed mode there is no division.
 */
static bool fixed_cadence_active = false;
static unsigned fixed_countdown = 0;

static char const *mode_name(BenchMode m) {
    switch (m) {
    case BENCH_MODE_ALLOC:
        return "alloc";
    case BENCH_MODE_FIXED:
        return "fixed";
    case BENCH_MODE_AUTO:
        break;
    }
    return "auto";
}

/*
 * The harness's allocation entry point. Scenarios call this and not the
 * backend, because this is where fixed-cadence mode lives — one
 * implementation, so no scenario can be more or less eager to collect than
 * another.
 */
CVM_HARNESS_INLINE void maybe_collect(void) {
    if (fixed_cadence_active && --fixed_countdown == 0) {
        fixed_countdown = cadence;
        if (!bench_collector.collect()) {
            forced_collect_unsupported = true;
        }
    }
}

void *bench_alloc(size_t size) {
    maybe_collect();
    return bench_collector.alloc(size);
}

void *bench_alloc_obj(BenchShape const *shape) {
    maybe_collect();
    return bench_collector.alloc_typed(shape);
}

/* The opaque sink roots.h needs. Not inlined away: another translation
 * unit, so the compiler cannot see that it does nothing. */
void bench_roots_keep(void *p) {
    static volatile void *sink;
    sink = p;
}

/* ------------------------------------------------------------------ */
/* timing                                                              */
/* ------------------------------------------------------------------ */

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static bool arg_value(char const *arg, char const *name, char const **out) {
    size_t n = strlen(name);
    if (strncmp(arg, name, n) != 0 || arg[n] != '=') {
        return false;
    }
    *out = arg + n + 1;
    return true;
}

int main(int argc, char **argv) {
    unsigned reps = 7;
    bool warmup = true;
    char const *out_path = NULL;

    BenchScenario const *sc = &bench_scenario;

    for (int i = 1; i < argc; i++) {
        char const *v = NULL;
        if (arg_value(argv[i], "--mode", &v)) {
            if (strcmp(v, "alloc") == 0) {
                mode = BENCH_MODE_ALLOC;
            } else if (strcmp(v, "fixed") == 0) {
                mode = BENCH_MODE_FIXED;
            } else if (strcmp(v, "auto") == 0) {
                mode = BENCH_MODE_AUTO;
            } else {
                fprintf(stderr, "unknown mode: %s\n", v);
                return 2;
            }
        } else if (arg_value(argv[i], "--reps", &v)) {
            reps = (unsigned)strtoul(v, NULL, 10);
            if (reps == 0 || reps > BENCH_MAX_REPS) {
                fprintf(stderr, "reps out of range: %s\n", v);
                return 2;
            }
        } else if (arg_value(argv[i], "--cadence", &v)) {
            cadence = (unsigned)strtoul(v, NULL, 10);
        } else if (arg_value(argv[i], "--out", &v)) {
            out_path = v;
        } else if (strcmp(argv[i], "--no-warmup") == 0) {
            warmup = false;
        } else if (strcmp(argv[i], "--name") == 0) {
            /* The scenario's own name, which is not the same as the file it
             * was built from. The runner uses it to line the C and Go rows
             * up, so that "the same scenario" is one string rather than two
             * spellings that have to agree by hand. */
            printf("%s\n", sc->name);
            return 0;
        } else if (strcmp(argv[i], "--modes") == 0) {
            /*
             * Which modes this scenario declares. Answered here rather than
             * in the runner so that there is one source of truth: the
             * scenario's own table, compiled into this binary.
             */
            if (sc->modes & BENCH_RUN_ALLOC) {
                fputs("alloc ", stdout);
            }
            if (sc->modes & BENCH_RUN_FIXED) {
                fputs("fixed ", stdout);
            }
            if (sc->modes & BENCH_RUN_AUTO) {
                fputs("auto ", stdout);
            }
            fputc('\n', stdout);
            return 0;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }

    if (cadence == 0) {
        cadence = sc->default_cadence;
    }
    fixed_cadence_active = (mode == BENCH_MODE_FIXED) && cadence > 0;

    /*
     * alloc-only mode needs a heap nothing can exhaust, and how big that is
     * depends on the workload — so the scenario says. A backend that ignores
     * the hint is not being unfair, it simply has its own idea, and the
     * allocation counters will show whether it collected anyway.
     */
    /*
     * Refused before anything is measured rather than after. Asking a
     * collector for a mode it does not have is not a failed run, and it is
     * not a run at all — printing "unavailable" after doing the work would
     * waste the work and, worse, would have already run the workload once
     * without a way to report it.
     */
    unsigned want = 0;
    if (mode == BENCH_MODE_ALLOC) {
        want = BENCH_MODE_SUPPORTS_ALLOC;
    } else if (mode == BENCH_MODE_FIXED) {
        want = BENCH_MODE_SUPPORTS_FIXED;
    } else {
        want = BENCH_MODE_SUPPORTS_AUTO;
    }
    if ((bench_collector.modes & want) == 0) {
        printf("%-14s %-8s %-8s  unavailable: this collector has no such mode\n",
               sc->name, bench_collector.name, mode_name(mode));
        return 0;
    }

    bool alloc_only = (mode == BENCH_MODE_ALLOC);
    size_t heap_hint = alloc_only ? sc->heap_hint_bytes : sc->auto_heap_bytes;

    if (!bench_collector.init(heap_hint, alloc_only)) {
        fprintf(stderr, "%s: backend init failed\n", bench_collector.name);
        return 1;
    }

    if (warmup) {
        /* BENCH.md §2.5: one clean run, discarded, so the first measurement
         * is not also the run that faults in the heap. */
        sc->run(0);
    }

    uint64_t wall[BENCH_MAX_REPS], allocs[BENCH_MAX_REPS];
    uint64_t colls[BENCH_MAX_REPS], pauses[BENCH_MAX_REPS];

    for (unsigned r = 0; r < reps; r++) {
        BenchStats before;
        bench_collector.stats(&before);

        fixed_countdown = cadence;
        uint64_t t0 = now_ns();
        sc->run(r + 1);
        uint64_t t1 = now_ns();

        BenchStats after;
        bench_collector.stats(&after);

        wall[r] = t1 - t0;
        allocs[r] = after.allocations - before.allocations;
        colls[r] = after.collections - before.collections;
        pauses[r] = after.pause_max_ns;

        if (alloc_only && r + 1 < reps) {
            /*
             * alloc-only mode resets the collector between repetitions.
             *
             * A workload whose total allocation exceeds its heap cannot run
             * without reuse, and "no collection for the whole run" means per
             * repetition, not per process: bdwgc with collection disabled
             * died of exactly this on mixed-sizes. Re-initializing between
             * repetitions gives each one a fresh heap of the same size,
             * outside the timed window, which is what the mode measures.
             */
            bench_collector.shutdown();
            if (!bench_collector.init(heap_hint, alloc_only)) {
                fprintf(stderr, "%s: re-init failed between repetitions\n",
                        bench_collector.name);
                return 1;
            }
        }
    }

    if (mode == BENCH_MODE_FIXED && forced_collect_unsupported) {
        /*
         * Said plainly rather than measured anyway. A backend that cannot be
         * asked to collect has not run this mode, and a row for it would be a
         * comparison between two different things wearing one label.
         */
        printf("%-14s %-8s %-8s  unavailable: backend cannot collect on demand\n",
               sc->name, bench_collector.name, mode_name(mode));
        bench_collector.shutdown();
        return 0;
    }

    BenchStats final;
    bench_collector.stats(&final);

    bench_print_row(sc->name, bench_collector.name, mode_name(mode), reps, wall,
                    allocs, colls, pauses, final.live_objects, final.heap_bytes);

    if (out_path != NULL) {
        FILE *out = fopen(out_path, "a");
        if (out == NULL) {
            fprintf(stderr, "cannot append to %s\n", out_path);
            bench_collector.shutdown();
            return 1;
        }
        bench_csv_row(out, sc->name, bench_collector.name, mode_name(mode), reps,
                      wall, allocs, colls, pauses, final.live_objects,
                      final.heap_bytes);
        fclose(out);
    }

    bench_collector.shutdown();
    return 0;
}
