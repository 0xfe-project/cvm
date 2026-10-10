/*
 * vm/gc/root.c
 *
 * The global root table. See include/cvm/roots.h for what it is for and why
 * adding a root is a write barrier.
 */

#include "cvm/roots.h"

#include <stdatomic.h>

#include "base/assert.h"
#include "base/types.h"
#include "cvm/gc.h"
#include "cvm/mutex.h"
#include "gc/grey_packet.h"
#include "gc/heap.h"
#include "gc/marker.h"
#include "gc/world.h"
#include "log.h"
#include "rt.h"
#include "safepoint.h"

/* ------------------------------------------------------------------ */
/* storage                                                             */
/* ------------------------------------------------------------------ */

/*
 * Two parallel arrays rather than converting the slot to a pointer-sized
 * integer to thread a free list through it. The union would be smaller
 * and would also be the kind of punning this codebase avoids; the extra
 * byte per slot buys clarity, and the table is small.
 */
static opaque_mut* root_objects = NULL;
static u8* root_in_use = NULL;

static u32 root_capacity = 0;
static u32 root_count = 0;
static u32 root_next_free = 0;

/* Guards the arrays. A collection reads the table, and an unmanaged
 * thread may write it at any time. */
static CvmMutex root_lock;
static bool root_ready = false;

#define ROOT_INITIAL_CAPACITY 64u

bool cvm_root_init(void) {
    if (root_ready) {
        return true;
    }
    if (!cvm_mutex_init(&root_lock)) {
        return false;
    }

    root_capacity = ROOT_INITIAL_CAPACITY;
    root_objects = rt_mem_alloc((usize)root_capacity * sizeof(opaque_mut));
    root_in_use = rt_mem_alloc((usize)root_capacity);

    if (root_objects == NULL || root_in_use == NULL) {
        rt_mem_free(root_objects);
        rt_mem_free(root_in_use);
        root_objects = NULL;
        root_in_use = NULL;
        root_capacity = 0;
        return false;
    }

    for (u32 i = 0; i < root_capacity; i++) {
        root_objects[i] = NULL;
        root_in_use[i] = 0;
    }
    root_count = 0;
    root_next_free = 0;
    root_ready = true;
    return true;
}

void cvm_root_destroy(void) {
    if (!root_ready) {
        return;
    }
    rt_mem_free(root_objects);
    rt_mem_free(root_in_use);
    root_objects = NULL;
    root_in_use = NULL;
    root_capacity = 0;
    root_count = 0;
    root_ready = false;
}

/* ------------------------------------------------------------------ */
/* growing                                                             */
/* ------------------------------------------------------------------ */

/* Called with the lock held. */
static bool root_grow(void) {
    if (root_capacity == 0 ||
        root_capacity > (u32)(~(u32)0) / (2u * (u32)sizeof(opaque_mut))) {
        return false;
    }

    u32 new_capacity = root_capacity * 2;
    opaque_mut* objects = rt_mem_alloc((usize)new_capacity * sizeof(opaque_mut));
    u8* in_use = rt_mem_alloc((usize)new_capacity);
    if (objects == NULL || in_use == NULL) {
        rt_mem_free(objects);
        rt_mem_free(in_use);
        return false;
    }

    for (u32 i = 0; i < root_capacity; i++) {
        objects[i] = root_objects[i];
        in_use[i] = root_in_use[i];
    }
    for (u32 i = root_capacity; i < new_capacity; i++) {
        objects[i] = NULL;
        in_use[i] = 0;
    }

    rt_mem_free(root_objects);
    rt_mem_free(root_in_use);
    root_objects = objects;
    root_in_use = in_use;
    root_capacity = new_capacity;
    return true;
}

/* ------------------------------------------------------------------ */
/* the write barrier                                                   */
/* ------------------------------------------------------------------ */

/*
 * Mark an object that is about to become a root, if a mark phase is running.
 *
 * This is the whole reason the table has a barrier. An unmanaged thread is not
 * stopped by a collection, so it can register a root while marking is in
 * progress, and marking may already have walked the table. Marking it here
 * closes that window.
 *
 * The mark phase, not the whole stop-the-world window: acting during the sweep
 * re-marks a block the sweeper has finished with, and the live set is decided
 * by then, so there is nothing left to protect. See the declaration of
 * cvm_marking_active in cvm/gc.h.
 */
static void mark_if_collecting(opaque_mut obj) {
    if (obj == NULL ||
        !atomic_load_explicit(&cvm_marking_active, memory_order_acquire)) {
        return;
    }

    if (!cvm_world_ready()) {
        return;
    }
    CvmHeap* heap = &cvm_world()->heap;

    /*
     * Marking is idempotent and the packet lists are lock-free, so
     * marking from this thread is safe even with markers running. The
     * packet is taken, used and returned; the alternative is for every
     * root addition to allocate its own holder, which for a rare path is
     * not worth it.
     */
    CvmGreyPacket* out = cvm_mark_take_packet(heap);
    CvmGreyPacket* out_weak = cvm_mark_take_packet(heap);

    cvm_mark_field(heap, obj, &out, &out_weak);

    cvm_mark_give_packet(heap, out, false);
    cvm_mark_give_packet(heap, out_weak, true);
}

/* ------------------------------------------------------------------ */
/* the table                                                           */
/* ------------------------------------------------------------------ */

CvmRootHandle cvm_root_add(opaque_mut obj) {
    if (!root_ready) {
        return CVM_ROOT_HANDLE_NONE;
    }

    cvm_mutex_lock(&root_lock);

    /* Find a free slot, starting from where the last search stopped so
     * that filling and emptying alternates in O(1) rather than scanning
     * from zero every time. */
    u32 slot = root_capacity;
    for (u32 tried = 0; tried < root_capacity; tried++) {
        u32 candidate = (root_next_free + tried) % root_capacity;
        if (!root_in_use[candidate]) {
            slot = candidate;
            root_next_free = (candidate + 1) % root_capacity;
            break;
        }
    }

    if (slot == root_capacity) {
        if (!root_grow()) {
            cvm_mutex_unlock(&root_lock);
            CVM_LOG_ERROR(m) {
                fmt_lit(&m.fmt, "root table could not grow past ");
                fmt_u32(&m.fmt, root_capacity);
                fmt_lit(&m.fmt, " slots");
            }
            return CVM_ROOT_HANDLE_NONE;
        }
        slot = root_capacity / 2; /* the first slot the growth added */
        root_next_free = (slot + 1) % root_capacity;
    }

    root_objects[slot] = obj;
    root_in_use[slot] = 1;
    root_count++;

    cvm_mutex_unlock(&root_lock);

    /* Outside the lock: marking takes locks of its own. */
    mark_if_collecting(obj);

    return (CvmRootHandle)(slot + 1);
}

opaque_mut cvm_root_get(CvmRootHandle handle) {
    if (!root_ready || handle == CVM_ROOT_HANDLE_NONE) {
        return NULL;
    }

    u32 slot = handle - 1;
    cvm_mutex_lock(&root_lock);
    opaque_mut obj = NULL;
    if (slot < root_capacity && root_in_use[slot]) {
        obj = root_objects[slot];
    }
    cvm_mutex_unlock(&root_lock);
    return obj;
}

void cvm_root_set(CvmRootHandle handle, opaque_mut obj) {
    if (!root_ready || handle == CVM_ROOT_HANDLE_NONE) {
        return;
    }

    u32 slot = handle - 1;
    cvm_mutex_lock(&root_lock);
    if (slot < root_capacity && root_in_use[slot]) {
        root_objects[slot] = obj;
    }
    cvm_mutex_unlock(&root_lock);

    mark_if_collecting(obj);
}

void cvm_root_remove(CvmRootHandle handle) {
    if (!root_ready || handle == CVM_ROOT_HANDLE_NONE) {
        return;
    }

    u32 slot = handle - 1;
    cvm_mutex_lock(&root_lock);
    if (slot < root_capacity && root_in_use[slot]) {
        root_objects[slot] = NULL;
        root_in_use[slot] = 0;
        root_count--;
        if (slot < root_next_free) {
            root_next_free = slot;
        }
    }
    cvm_mutex_unlock(&root_lock);
}

u32 cvm_root_count(void) {
    if (!root_ready) {
        return 0;
    }
    cvm_mutex_lock(&root_lock);
    u32 n = root_count;
    cvm_mutex_unlock(&root_lock);
    return n;
}

u32 cvm_root_capacity(void) {
    if (!root_ready) {
        return 0;
    }
    cvm_mutex_lock(&root_lock);
    u32 n = root_capacity;
    cvm_mutex_unlock(&root_lock);
    return n;
}

void cvm_root_mark_all(CvmHeap* heap, CvmGreyPacket** out, CvmGreyPacket** out_weak) {
    if (!root_ready) {
        return;
    }

    /*
     * Raw: this runs on the collecting thread, which must stay visible to the
     * collector. Declaring the wait here would tell the collection this thread is
     * running not to wait for it.
     */
    cvm_mutex_lock_raw(&root_lock);
    for (u32 i = 0; i < root_capacity; i++) {
        if (root_in_use[i] && root_objects[i] != NULL) {
            cvm_mark_field(heap, root_objects[i], out, out_weak);
        }
    }
    cvm_mutex_unlock(&root_lock);
}
