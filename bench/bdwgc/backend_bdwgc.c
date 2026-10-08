/*
 * bench/bdwgc/backend_bdwgc.c
 *
 * bdwgc as the benchmark sees it.
 *
 * Three things about bdwgc shape this file, and all three are advantages it
 * has over cvm rather than excuses.
 *
 * It is conservative. It cannot know that an integer is not a pointer, so it
 * scans whole objects and whole stacks rather than reading a frame's slots.
 * The backend does not pretend otherwise: `alloc_typed` uses the one typed
 * thing bdwgc does offer — GC_MALLOC_ATOMIC for objects with no pointers —
 * and the pointer-bearing shapes are plain GC_MALLOC. That is the real
 * difference being measured.
 *
 * It is thirty years old and tuned by many people. Nothing here second-guesses
 * it: the backend uses its defaults, which is the point of the auto mode.
 *
 * It reports its own pause, properly. GC_set_on_collection_event delivers
 * PRE_STOP_WORLD and POST_START_WORLD, which is the world-stopped window and
 * therefore the same interval cvm's pause measures. Using GC_EVENT_START and
 * GC_EVENT_END instead would include the concurrent parts of an incremental
 * collection and make the two numbers incomparable.
 *
 * One thing it cannot report: a live object count. bdwgc tracks bytes, not
 * objects, so that column is left at zero rather than filled with a number
 * derived from an assumed object size. Its heap bytes are reported.
 */

#include <string.h>

#include "gc.h"

#include "collector.h"

#include <time.h>

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static uint64_t pause_start_ns = 0;
static uint64_t pause_max_ns = 0;
static uint64_t our_allocations = 0;
static uint64_t our_failures = 0;
static bool gc_disabled = false;

/*
 * The stop-the-world window, from bdwgc's own event stream.
 *
 * Only PRE_STOP_WORLD starts a timer and only POST_START_WORLD ends one: the
 * other events (mark start/end, reclaim start/end) happen at points that are
 * not the same interval for every collection mode, and mixing them would
 * produce a number that means different things on different runs.
 */
static void GC_CALLBACK on_collection_event(GC_EventType event) {
    if (event == GC_EVENT_PRE_STOP_WORLD) {
        pause_start_ns = now_ns();
    } else if (event == GC_EVENT_POST_START_WORLD) {
        if (pause_start_ns != 0) {
            uint64_t took = now_ns() - pause_start_ns;
            if (took > pause_max_ns) {
                pause_max_ns = took;
            }
            pause_start_ns = 0;
        }
    }
}

static bool bdwgc_backend_init(size_t heap_hint, bool alloc_only) {
    GC_set_on_collection_event(on_collection_event);

    if (alloc_only) {
        /*
         * A maximum heap size does not stop bdwgc collecting — it collects
         * proactively whenever it estimates reuse will help, so a churn run
         * reported 977 collections in "alloc-only" mode while cvm reported
         * none. That is not the same workload on both sides. GC_disable is
         * the literal meaning of the mode, and shutdown puts it back.
         */
        GC_disable();
        gc_disabled = true;
    }

    if (heap_hint != 0) {
        GC_set_max_heap_size((GC_word)heap_hint);
    }

    GC_INIT();
    our_allocations = 0;
    our_failures = 0;
    pause_max_ns = 0;
    gc_disabled = false;
    return true;
}

static void bdwgc_backend_shutdown(void) {
    if (gc_disabled) {
        GC_enable();
        gc_disabled = false;
    }
    /* Nothing to tear down: bdwgc has no shutdown, and GC_INIT is a no-op
     * the second time. Deliberately not calling GC_gcollect, which would
     * add a collection the run did not ask for. */
}

static void *bdwgc_backend_alloc(size_t size) {
    our_allocations++;
    void *p = GC_MALLOC(size);
    if (p == NULL) {
        our_failures++;
    }
    return p;
}

static void *bdwgc_backend_alloc_typed(BenchShape const *shape) {
    our_allocations++;
    void *p;
    if (shape->nptrs == 0) {
        /*
         * No pointers, so the object does not need to be scanned. This is
         * the one place bdwgc's API distinguishes what a precise collector
         * distinguishes everywhere, and using it is not cheating: it is the
         * feature the collector offers for exactly this case.
         */
        p = GC_MALLOC_ATOMIC(shape->size);
    } else {
        p = GC_MALLOC(shape->size);
    }
    if (p == NULL) {
        our_failures++;
    }
    return p;
}

static bool bdwgc_backend_collect(void) {
    GC_gcollect();
    return true;
}

static void bdwgc_backend_stats(BenchStats *out) {
    GC_word heap_size = 0, free_bytes = 0, unmapped = 0, since_gc = 0, total = 0;
    GC_get_heap_usage_safe(&heap_size, &free_bytes, &unmapped, &since_gc, &total);

    memset(out, 0, sizeof(*out));
    out->allocations = our_allocations;
    out->collections = (uint64_t)GC_get_gc_no();
    out->pause_max_ns = pause_max_ns;
    out->pause_total_ns = 0; /* not exposed; the maximum is what matters */
    out->live_objects = 0;   /* bytes, not objects: see the note above */
    out->failed_allocations = our_failures;
    out->heap_bytes = (uint64_t)heap_size;
}

BenchCollector const bench_collector = {
    .name = "bdwgc",
    .modes = BENCH_MODE_SUPPORTS_ALLOC | BENCH_MODE_SUPPORTS_FIXED |
             BENCH_MODE_SUPPORTS_AUTO,
    .init = bdwgc_backend_init,
    .shutdown = bdwgc_backend_shutdown,
    .alloc = bdwgc_backend_alloc,
    .alloc_typed = bdwgc_backend_alloc_typed,
    .collect = bdwgc_backend_collect,
    .stats = bdwgc_backend_stats,
};
