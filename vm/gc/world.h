/*
 * vm/gc/world.h
 *
 * The collector's process-global state, replacing refs/commix/State.h.
 *
 * There is exactly one heap and one block allocator per process. They live
 * here rather than being threaded through every call because the
 * alternative is a context parameter on every allocation path, including
 * the one inlined into generated code.
 *
 * The reference puts these in globals declared by State.h; this is the
 * same thing with a defined initialisation order and a way to tear down,
 * which matters for tests that want to build and destroy several heaps in
 * one process.
 */

#ifndef CVM_GC_WORLD_H
#define CVM_GC_WORLD_H

#include "base/types.h"
#include "gc/block_alloc.h"
#include "gc/heap.h"

typedef struct CvmWorld {
    CvmHeap heap;
    CvmBlockAllocator block_alloc;
    bool ready;
} CvmWorld;

/* The one world. Never NULL, but not usable until cvm_world_init succeeds. */
CvmWorld* cvm_world(void);

/*
 * Bring up the heap and the block allocator over it.
 *
 * `min_size` and `max_size` are heap sizes in bytes; see cvm_heap_init.
 * Returns false if a reservation failed, in which case the world is not
 * ready and must not be used.
 */
bool cvm_world_init(usize min_size, usize max_size);

/* Release everything. Safe to call when not initialised. */
void cvm_world_destroy(void);

CVM_INLINE bool cvm_world_ready(void) {
    return cvm_world()->ready;
}

#endif /* CVM_GC_WORLD_H */
