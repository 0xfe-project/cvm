/*
 * vm/gc/retire.c
 *
 * The retire list's mechanics. The contract and the argument for why a grace
 * period can work at all are in cvm/retire.h; this file is the three things
 * the collector does with the list and the one thing a mutator does.
 */

#include "cvm/retire.h"

#include <stdatomic.h>

#include "base/types.h"
#include "cvm/gc.h"
#include "gc/grey_packet.h"
#include "gc/heap.h"
#include "gc/marker.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

/*
 * Room for the first several retires without an allocation. Resizes are the
 * ordinary caller and a handful per collection is a busy one; a workload that
 * retires more than this grows once and carries the larger arrays, which is
 * the right trade for a mechanism whose whole point is that it is rarely used.
 */
#define RETIRE_INITIAL_CAPACITY 8

static bool retire_push(opaque_mut** list, u32* count, u32* cap, opaque_mut payload) {
    if (*count == *cap) {
        u32 next_cap = (*cap == 0) ? RETIRE_INITIAL_CAPACITY : *cap * 2u;
        opaque_mut* bigger = rt_mem_alloc((usize)next_cap * sizeof(opaque_mut));
        if (bigger == NULL) {
            return false;
        }
        for (u32 i = 0; i < *count; i++) {
            bigger[i] = (*list)[i];
        }
        rt_mem_free((opaque_mut)*list);
        *list = bigger;
        *cap = next_cap;
    }
    (*list)[(*count)++] = payload;
    return true;
}

void cvm_retire(opaque_mut payload) {
    if (payload == NULL) {
        return;
    }
    if (!cvm_world_ready()) {
        return; /* nothing to keep alive in, and shutdown races are not ours */
    }
    CvmHeap* heap = &cvm_world()->heap;

    rt_mutex_lock(&heap->retired.lock);
    bool ok = retire_push(&heap->retired.current, &heap->retired.current_count,
                          &heap->retired.current_cap, payload);
    rt_mutex_unlock(&heap->retired.lock);

    /*
     * A retire that happens while a mark phase is running cannot wait for the
     * next marking to be kept alive: this collection's sweep is already
     * entitled to reclaim it, because its marking may have walked past the
     * structure before the replacement happened. So mark it now, through the
     * same path a store would -- grey, not black, because a reader walks
     * through this object's fields and what they point to has to stay valid
     * too. See the note in cvm/retire.h.
     */
    if (ok && atomic_load_explicit(&cvm_marking_active, memory_order_acquire)) {
        cvm_store_barrier(payload);
    }
}

/*
 * Move the current list to previous, dropping what previous held.
 *
 * Called from reset_marking, which runs under the first stop of a collection
 * in both collector modes: managed threads are parked, so the only concurrent
 * retires are from unmanaged threads, which the lock serialises.
 *
 * What this drop means: everything on `previous` was marked by the collection
 * that is ending -- kept alive through its sweep -- and the marking that is
 * about to run will not mark it again, because it is unreachable from the
 * graph and no longer on any list. The sweep of the collection now starting
 * reclaims it. That is the promised grace: alive through one full collection,
 * gone in the next.
 */
void cvm_retire_begin_collection(CvmHeap* heap) {
    rt_mutex_lock(&heap->retired.lock);

    rt_mem_free((opaque_mut)heap->retired.previous);
    heap->retired.previous = heap->retired.current;
    heap->retired.previous_count = heap->retired.current_count;
    heap->retired.previous_cap = heap->retired.current_cap;
    heap->retired.current = NULL;
    heap->retired.current_count = 0;
    heap->retired.current_cap = 0;

    rt_mutex_unlock(&heap->retired.lock);
}

/*
 * Mark everything on both lists, grey.
 *
 * `previous` is what keeps this collection's sweep from reclaiming the
 * objects retired before it started. `current` was already marked at retire
 * time when it was retired mid-marking; marking it again is idempotent, and
 * covers the retires that happened between collections, which nothing else
 * has marked.
 */
void cvm_retire_mark_all(CvmHeap* heap, CvmGreyPacket** out, CvmGreyPacket** out_weak) {
    rt_mutex_lock(&heap->retired.lock);

    for (u32 i = 0; i < heap->retired.previous_count; i++) {
        cvm_mark_field(heap, heap->retired.previous[i], out, out_weak);
    }
    for (u32 i = 0; i < heap->retired.current_count; i++) {
        cvm_mark_field(heap, heap->retired.current[i], out, out_weak);
    }

    rt_mutex_unlock(&heap->retired.lock);
}
