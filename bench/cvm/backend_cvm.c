/*
 * bench/cvm/backend_cvm.c
 *
 * cvm as the benchmark sees it.
 *
 * Three things about this file are specific to cvm and worth stating,
 * because they are the ways a backend can accidentally make itself look
 * better.
 *
 * The first is the frame protocol. cvm is precise: roots come from a frame
 * the caller pushes, and nothing else is scanned. A scenario that allocated
 * without pushing a frame would skip the per-entry safepoint poll and the
 * root registration both — so it would measure a collector that never looks
 * at the stack, which is a different collector. See harness/roots.h; the
 * scenarios push frames, and that is enforced by BENCH.md §2.3 rather than
 * by hope.
 *
 * The second is the descriptors. A precise collector needs to know where an
 * object's pointers are, so `alloc(size)` for an arbitrary size has to be
 * turned into a descriptor before it can be allocated. The descriptors here
 * are built from the scenario's declared shape and cached in static storage,
 * because the collector reads them during marking and a stack descriptor
 * would be a dangling pointer by then.
 *
 * The third is that cvm has no allocation counter, deliberately — see the
 * note on CvmGcCounters in vm/stats.h. The count reported here is this
 * backend's own, and the corroboration that the work really happened is the
 * heap's live-object count and size, which cvm does maintain.
 */

#include <string.h>

#include "cvm/gc.h"
#include "cvm/vm.h"

#include "collector.h"

/* ------------------------------------------------------------------ */
/* descriptors                                                         */
/* ------------------------------------------------------------------ */

/*
 * A descriptor for `size` bytes with no pointers.
 *
 * Used by the untyped allocation path, which is what a scenario uses when
 * its object holds no pointers at all — the flat-array control and the blob
 * churn. Marking a leaf is a single visit, which is the point of those
 * scenarios.
 */
static CvmTypeDescriptor raw_desc = {
    .size = 0, /* set per call */
    .kind = CVM_TYPE_SCALAR,
    .num_ptr_fields = 0,
    .num_embed_fields = 0,
    .ptr_fields = NULL,
    .embed_fields = NULL,
};

/*
 * Descriptors built from declared shapes, cached.
 *
 * Bounded rather than grown: the suite declares four shapes, and a fixed
 * table makes the lookup a pointer compare instead of an allocation that
 * would itself perturb the measurement.
 */
#define DESCRIPTOR_CACHE 16

typedef struct CachedDescriptor {
    BenchShape const *shape;
    CvmTypeDescriptor desc;
    CvmPtrField fields[BENCH_SHAPE_MAX_PTRS];
} CachedDescriptor;

static CachedDescriptor descriptors[DESCRIPTOR_CACHE];
static unsigned descriptor_count = 0;

static CvmTypeDescriptor const *descriptor_for(BenchShape const *shape) {
    /*
     * Last-used first. A scenario allocates the same shape millions of times
     * in a row, so the linear scan below almost always finds it on the first
     * comparison — but only if the first entry is the one it wants.
     */
    static CvmTypeDescriptor const *last_shape = NULL;
    static CvmTypeDescriptor const *last_desc = NULL;
    if (shape == last_shape) {
        return last_desc;
    }

    for (unsigned i = 0; i < descriptor_count; i++) {
        if (descriptors[i].shape == shape) {
            last_shape = shape;
            last_desc = &descriptors[i].desc;
            return last_desc;
        }
    }

    if (descriptor_count == DESCRIPTOR_CACHE) {
        return NULL; /* more shapes than the table; a bench bug, not a GC one */
    }

    CachedDescriptor *slot = &descriptors[descriptor_count++];
    slot->shape = shape;
    for (unsigned i = 0; i < shape->nptrs && i < BENCH_SHAPE_MAX_PTRS; i++) {
        slot->fields[i].offset = (u32)shape->ptr_offsets[i];
    }
    slot->desc = (CvmTypeDescriptor){
        .size = (u32)shape->size,
        .kind = (shape->nptrs == 0) ? CVM_TYPE_SCALAR : CVM_TYPE_STRUCT,
        .num_ptr_fields = (u16)shape->nptrs,
        .num_embed_fields = 0,
        .ptr_fields = slot->fields,
        .embed_fields = NULL,
    };
    last_shape = shape;
    last_desc = &slot->desc;
    return last_desc;
}

/* ------------------------------------------------------------------ */
/* the backend                                                         */
/* ------------------------------------------------------------------ */

static uint64_t our_allocations = 0;
static uint64_t our_failures = 0;
static bool init_ok = false;

static bool cvm_backend_init(size_t heap_hint, bool alloc_only) {
    CvmOptions opts = {0};
    opts.gc_workers = CVM_GC_WORKERS_DEFAULT;
    opts.max_heap_size = heap_hint; /* in auto/fixed this is auto_heap_bytes */

    /*
     * The hint is used differently by the two kinds of mode, and both are
     * necessary.
     *
     * In alloc-only mode the minimum is raised to it, so that no collection
     * happens and the row measures the allocator.
     *
     * In every other mode it bounds the maximum instead. Without that, cvm's
     * growth policy — which doubles the heap when free blocks are low — runs
     * away on a workload whose live set keeps growing: the tree scenario in
     * fixed-cadence mode reached a 270 GiB reservation and spent thirteen
     * seconds clearing the metadata for it, which is a measurement of the
     * growth policy's worst case rather than of the collector. Bounding the
     * heap to what the scenario says it needs is what the scenario's hint is
     * for.
     */
    if (alloc_only) {
        /*
         * alloc-only: reserve enough that no collection can happen. The
         * minimum, not the maximum — a maximum would still let the heap
         * start small and grow through the timed window, paying page faults
         * on the way. This hint is the scenario's total allocation.
         */
        opts.min_heap_size = heap_hint;
        opts.max_heap_size = heap_hint * 2;
    }
    /*
     * auto/fixed: the ceiling is already set from auto_heap_bytes above.
     * See the note in scenario.h for why an uncapped heap is not a
     * measurement.
     */

    /*
     * No stats channel and no finalizer thread: both would put work inside
     * the measured window that the other backends are not paying for. The
     * event log is available for a run that wants to explain a result rather
     * than produce one.
     */
    opts.stats_channel = NULL;
    opts.finalizer_thread = false;

    init_ok = cvm_init(&opts);
    our_allocations = 0;
    our_failures = 0;
    return init_ok;
}

static void cvm_backend_shutdown(void) {
    if (init_ok) {
        cvm_shutdown();
        init_ok = false;
    }
}

static void *cvm_backend_alloc(size_t size) {
    raw_desc.size = (u32)size;
    our_allocations++;
    void *p = cvm_alloc(&raw_desc);
    if (p == NULL) {
        our_failures++;
    }
    return p;
}

static void *cvm_backend_alloc_typed(BenchShape const *shape) {
    CvmTypeDescriptor const *desc = descriptor_for(shape);
    if (desc == NULL) {
        return NULL;
    }
    our_allocations++;
    void *p = cvm_alloc(desc);
    if (p == NULL) {
        our_failures++;
    }
    return p;
}

static bool cvm_backend_collect(void) {
    cvm_collect();
    return true;
}

static void cvm_backend_stats(BenchStats *out) {
    CvmVmStats vm = cvm_vm_stats();
    memset(out, 0, sizeof(*out));
    out->allocations = our_allocations;
    out->collections = vm.collections;
    out->pause_max_ns = vm.pause_max_ns;
    out->pause_total_ns = vm.collection_ns;
    out->live_objects = vm.objects_live;
    out->heap_bytes = vm.heap_size;
    out->failed_allocations = our_failures;
}

BenchCollector const bench_collector = {
    .name = "cvm",
    .modes = BENCH_MODE_SUPPORTS_ALLOC | BENCH_MODE_SUPPORTS_FIXED |
             BENCH_MODE_SUPPORTS_AUTO,
    .init = cvm_backend_init,
    .shutdown = cvm_backend_shutdown,
    .alloc = cvm_backend_alloc,
    .alloc_typed = cvm_backend_alloc_typed,
    .collect = cvm_backend_collect,
    .stats = cvm_backend_stats,
};
