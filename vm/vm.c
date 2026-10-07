/*
 * vm/vm.c
 *
 * The runtime's entry points. See vm/include/cvm/vm.h for the shape.
 *
 * This file is the only place that knows the order the subsystems have to
 * come up in, and that order is not arbitrary:
 *
 *   logging        before anything that might want to say something
 *   thread registry  before a thread can register, which requires...
 *   safepoints       before registration uses them
 *   heap             before registration gives a thread allocators
 *   root table       before the marker walks it
 *   finalizer queue  before a sweep can hand it work
 *   attach           last, because it needs all of the above
 */

#include "cvm/vm.h"

#include "base/types.h"
#include "context.h"
#include "gc/alloc.h"
#include "gc/block_alloc.h"
#include "gc/collect.h"
#include "cvm/external.h"
#include "gc/heap.h"
#include "cvm/roots.h"
#include "gc/sweeper.h"
#include "gc/world.h"
#include "log.h"
#include "rt.h"
#include "safepoint.h"

static bool vm_ready = false;

bool cvm_init(CvmOptions const* options) {
    if (vm_ready) {
        return true;
    }

    CvmOptions defaults = {
        .min_heap_size = 0,
        .max_heap_size = 0,
        .gc_workers = 0,
        .finalizer_thread = true,
        .stats_channel = NULL,
        .log_level = {NULL, 0},
    };
    CvmOptions const* o = (options != NULL) ? options : &defaults;

    /* Logging first: everything below may want to report a failure. */
    if (o->log_level.len > 0) {
        /* A caller-supplied level is not parsed from the environment, so
         * it replaces the level rather than feeding into the parser. */
        cvm_log_set_level(CVM_LOG_WARN);
    } else {
        cvm_log_init_from_env();
    }

    if (o->gc_workers != 0) {
        /*
         * Refused rather than ignored. Quietly running single-threaded
         * when the caller asked for workers would be a performance
         * mystery; saying so is better than a silent downgrade.
         */
        CVM_LOG_ERROR(m) {
            fmt_lit(&m.fmt, "gc_workers is not implemented yet; leave it at 0");
        }
        return false;
    }

    cvm_threads_init();
    cvm_safepoint_init();

    if (!cvm_world_init(o->min_heap_size, o->max_heap_size)) {
        CVM_LOG_ERROR(m) {
            fmt_lit(&m.fmt, "could not reserve the heap");
        }
        return false;
    }

    if (o->finalizer_thread && !cvm_finalizer_start()) {
        CVM_LOG_WARN(m) {
            fmt_lit(&m.fmt, "finalizer thread could not start; finalizers will "
                            "only run at shutdown");
        }
    }

    /*
     * Register the calling thread. It is about to run generated code, so
     * it needs a context to allocate from and a frame chain to push onto.
     */
    if (!cvm_thread_attach()) {
        CVM_LOG_ERROR(m) {
            fmt_lit(&m.fmt, "could not register the calling thread");
        }
        cvm_world_destroy();
        return false;
    }

    vm_ready = true;

    CVM_LOG_INFO(m) {
        fmt_lit(&m.fmt, "runtime up: heap ");
        fmt_u64(&m.fmt, (u64)cvm_vm_heap()->heap_size);
        fmt_lit(&m.fmt, " of ");
        fmt_u64(&m.fmt, (u64)cvm_vm_heap()->max_heap_size);
        fmt_lit(&m.fmt, " bytes");
    }
    return true;
}

void cvm_shutdown(void) {
    if (!vm_ready) {
        return;
    }

    /*
     * Finalizers first. A pending finalizer may allocate or call back in,
     * and it needs a working runtime to do that.
     */
    cvm_finalizer_stop();

    cvm_thread_detach();
    cvm_world_destroy();

    vm_ready = false;
}

void cvm_collect(void) {
    if (!vm_ready) {
        return;
    }
    cvm_gc_collect_and_finish(cvm_vm_heap());
}

CvmVmStats cvm_vm_stats(void) {
    CvmVmStats stats = {0};
    if (!vm_ready) {
        return stats;
    }

    CvmHeap* heap = cvm_vm_heap();

    stats.collections = cvm_gc_counters_collections();
    stats.collection_ns = cvm_gc_counters_collection_ns();
    stats.blocks_total = heap->block_count;
    stats.blocks_free = cvm_block_alloc_free_count(heap->block_alloc);
    stats.heap_size = heap->heap_size;
    stats.heap_max = heap->max_heap_size;
    stats.threads_registered = cvm_thread_count();
    stats.finalizers_run = cvm_finalizer_run_count();
    stats.finalizers_dropped = cvm_finalizer_dropped_count();

    /*
     * Objects live is counted rather than tracked. A running count would
     * have to be updated on the allocation fast path, which is the one
     * place that has to stay a few instructions, so it is derived when
     * asked instead.
     */
    u32 live = 0;
    for (u32 i = 0; i < heap->block_count; i++) {
        u8* block_start = cvm_heap_block_address(heap, i);
        ObjectMeta* slot = bytemap_get(heap->bytemap, block_start);
        for (usize s = 0; s < CVM_SLOTS_PER_BLOCK; s++) {
            if (object_meta_is_allocated(&slot[s]) || object_meta_is_marked(&slot[s])) {
                live++;
            }
        }
    }
    stats.objects_live = live;

    return stats;
}

struct CvmHeap* cvm_vm_heap(void) {
    return &cvm_world()->heap;
}
