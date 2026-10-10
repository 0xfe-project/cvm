/*
 * bench/tgc/backend_tgc.c
 *
 * tgc as the benchmark sees it.
 *
 * tgc is a floor, not a competitor. It inserts every allocation into an
 * open-addressed hash table, scans roots by walking the stack from a setjmp
 * point, and sweeps by rebuilding its table. A collector that makes no design
 * investment is supposed to be slow, and a cvm that fails to beat it
 * comfortably has a problem. Reporting it as a peer would flatter us.
 *
 * Two consequences of its design that the backend has to respect:
 *
 *   - It has no typed allocation. Every object is scanned conservatively,
 *     so `alloc_typed` is the same call as `alloc`. That is not a shortcut:
 *     it is what the collector does, and the difference between this and
 *     cvm's descriptor-driven allocation is part of what is being measured.
 *
 *   - It finds roots by scanning the stack, so the scenarios' frame macros
 *     do nothing here. The roots only have to be on the stack, which they
 *     are, because harness/roots.h declares a real array for every
 *     non-cvm backend.
 *
 * The metrics it reports are not upstream's: upstream has only table
 * occupancy. See the note in gc/tgc.h for what was added and why.
 */

#include <string.h>

#include "tgc.h"

#include "collector.h"

static tgc_t gc;
static uint64_t our_failures = 0;

static bool tgc_backend_init(size_t heap_hint, bool alloc_only) {
    (void)alloc_only;
    /*
     * tgc has no heap sizing: it calls malloc per object and grows a hash
     * table. The hint is ignored rather than approximated, and alloc-only
     * mode for tgc therefore does not mean "never collects" — it means "not
     * forced to collect". The collection counter in the row says which
     * happened.
     */
    (void)heap_hint;
    our_failures = 0;
    memset(&gc, 0, sizeof(gc));

    /*
     * The stack bottom has to be a real stack address. tgc finds roots by
     * walking memory from a local in its own marker up to this address, so a
     * static here — which lives in .bss, nowhere near the stack — makes it
     * walk off the end of the stack and fault. It only uses the address as a
     * bound, so the variable going out of scope afterwards is fine: the
     * scenario's own frames are deeper than this one and therefore inside the
     * range.
     */
    int stack_bottom = 0;
    tgc_start(&gc, &stack_bottom);
    return true;
}

static void tgc_backend_shutdown(void) {
    tgc_stop(&gc);
}

static void *tgc_backend_alloc(size_t size) {
    void *p = tgc_alloc(&gc, size);
    if (p == NULL) {
        our_failures++;
    }
    return p;
}

static void *tgc_backend_alloc_typed(BenchShape const *shape) {
    /* Conservative: the shape's pointer offsets are not used, because tgc
     * does not have a way to use them. */
    (void)shape;
    void *p = tgc_alloc(&gc, shape->size);
    if (p == NULL) {
        our_failures++;
    }
    return p;
}

static bool tgc_backend_collect(void) {
    tgc_run(&gc);
    return true;
}

static void tgc_backend_stats(BenchStats *out) {
    memset(out, 0, sizeof(*out));
    out->allocations = gc.bench_allocations;
    out->collections = gc.bench_collections;
    out->pause_max_ns = gc.bench_pause_max_ns;
    out->pause_total_ns = 0; /* not tracked: the maximum is what matters */
    out->live_objects = gc.nitems;
    out->heap_bytes = gc.nslots * sizeof(tgc_ptr_t);
    out->failed_allocations = our_failures;
}

BenchCollector const bench_collector = {
    .name = "tgc",
    /*
     * All three modes, and auto mode is real rather than nominal: tgc_add
     * calls tgc_run itself once the item count passes a threshold that the
     * last sweep set, so tgc collects without being asked. An earlier version
     * of this file claimed tgc had no automatic policy and reported auto mode
     * as unavailable, which was wrong — the call is at tgc.c:369 and it is
     * the reason the churn rows show more than a million collections.
     */
    .modes = BENCH_MODE_SUPPORTS_ALLOC | BENCH_MODE_SUPPORTS_FIXED |
             BENCH_MODE_SUPPORTS_AUTO,
    .init = tgc_backend_init,
    .shutdown = tgc_backend_shutdown,
    .alloc = tgc_backend_alloc,
    .alloc_typed = tgc_backend_alloc_typed,
    .collect = tgc_backend_collect,
    .stats = tgc_backend_stats,
};
