/*
 * vm/gc/worklist.c
 *
 * Allocation for the work-stealing deque. The algorithm is in
 * gc/worklist.h, all of it inline, because push and pop are on the path of
 * every packet a marker handles.
 */

#include "gc/worklist.h"

#include "rt.h"

__thread CvmWorklist* cvm_tls_worklist = NULL;

static u64 next_power_of_two(u64 v) {
    u64 p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

bool cvm_worklist_init(CvmWorklist* w, u32 capacity) {
    u64 cap = next_power_of_two(capacity == 0 ? 1 : capacity);

    w->slots = (CvmGreyPacket * _Atomic*)rt_mem_alloc((usize)cap * sizeof(*w->slots));
    if (w->slots == NULL) {
        return false;
    }

    /*
     * Zeroed, because rt_mem_alloc does not promise it. Nothing reads a slot
     * that was never written -- a thief only reads below `bottom`, and the
     * owner only below `bottom` too -- so this is not load-bearing for
     * correctness. It is for the sanitizers: a ring of indeterminate pointers
     * is a ring of things asan and valgrind are right to complain about the
     * first time a debugging session dumps it.
     */
    rt_mem_zero((opaque_mut)w->slots, (usize)cap * sizeof(*w->slots));

    w->mask = (i64)cap - 1;
    w->cached_top = 0;
    atomic_store_explicit(&w->top, 0, memory_order_relaxed);
    atomic_store_explicit(&w->bottom, 0, memory_order_relaxed);
    return true;
}

void cvm_worklist_destroy(CvmWorklist* w) {
    if (w->slots != NULL) {
        rt_mem_free((opaque_mut)w->slots);
        w->slots = NULL;
    }
    w->mask = 0;
    w->cached_top = 0;
}
