/*
 * vm/gc/world.c
 *
 * The process-global collector state.
 */

#include "gc/world.h"

#include "base/types.h"
#include "gc/block_alloc.h"
#include "gc/heap.h"
#include "log.h"
#include "rt.h"

static CvmWorld the_world = {.ready = false};

CvmWorld* cvm_world(void) {
    return &the_world;
}

bool cvm_world_init(usize min_size, usize max_size) {
    CvmWorld* w = &the_world;

    if (w->ready) {
        return true;
    }

    if (!cvm_heap_init(&w->heap, min_size, max_size)) {
        CVM_LOG_ERROR(m) {
            fmt_lit(&m.fmt, "heap reservation failed (min=");
            fmt_u64(&m.fmt, (u64)min_size);
            fmt_lit(&m.fmt, " max=");
            fmt_u64(&m.fmt, (u64)max_size);
            fmt_lit(&m.fmt, " bytes)");
        }
        return false;
    }

    /*
     * The block allocator covers the blocks the heap has actually brought
     * into use, not the whole reservation. Handing it max_block_count
     * would let it allocate blocks past heap_end, whose metadata has not
     * been cleared and whose addresses are not part of the heap yet.
     *
     * Growing is a separate step: cvm_heap_extend reports the range it
     * added, and the caller that needed the space files it. That keeps the
     * allocator's view and the heap's view from drifting apart.
     */
    cvm_block_alloc_init(&w->block_alloc, w->heap.block_meta_start,
                         w->heap.block_count);
    w->heap.block_alloc = &w->block_alloc;

    w->ready = true;
    return true;
}

void cvm_world_destroy(void) {
    CvmWorld* w = &the_world;
    if (!w->ready) {
        return;
    }
    cvm_heap_destroy(&w->heap);
    w->ready = false;
}
