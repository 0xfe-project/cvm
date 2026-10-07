/*
 * vm/context.c
 *
 * The thread registry. See vm/context.h for what this deliberately does
 * not do, compared to the reference.
 *
 * The registry is a singly-linked list with prepend on attach and unlink
 * on detach, guarded by a reader-writer lock. Prepend makes the common
 * case — a thread appearing — a single pointer store, and the walk order
 * does not matter because a collection visits every node.
 */

#include "context.h"

#include <stdatomic.h>

#include "base/types.h"
#include "cvm/gc.h"
#include "gc/bytemap.h"
#include "gc/world.h"
#include "log.h"
#include "safepoint.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* registry state                                                      */
/* ------------------------------------------------------------------ */

/* Write side of the registry lock. File-private: nothing outside this
 * file may modify the list. */
static void cvm_threads_lock_write(void);
static void cvm_threads_unlock_write(void);

static CvmThreadNode* registry_head = NULL;
static RtRwlock registry_lock;
static _Atomic u32 registry_count = 0;
static _Atomic bool registry_ready = false;

/*
 * TLS. Declared here because this is where it is installed and cleared;
 * gc.h declares it extern for generated code.
 *
 * initial-exec: one segment-offset access, no runtime call. The cost is
 * that the shared object must be in the initial load set, which is
 * already required (see README).
 */
__thread CvmVMContext* cvm_tls_ctx __attribute__((tls_model("initial-exec"))) = NULL;

void cvm_threads_init(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&registry_ready, &expected, true)) {
        return; /* already initialised */
    }
    if (!rt_rwlock_init(&registry_lock)) {
        rt_fatal(STR_LITERAL("could not create the thread registry lock"));
    }
    registry_head = NULL;
    atomic_store(&registry_count, 0u);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

bool cvm_thread_attach(void) {
    if (!atomic_load(&registry_ready)) {
        /* Registering before the registry exists would be a race against
         * initialisation, so refuse rather than corrupt the list. */
        return false;
    }

    if (!cvm_world_ready()) {
        /*
         * A registered thread is a thread that may allocate, so it needs a
         * heap. Registering without one would put a context with no
         * bytemap into the registry, and the first allocation would write
         * through a null metadata pointer.
         */
        return false;
    }

    /* Idempotent: a second attach from the same thread keeps the first
     * node, so a re-entrant call cannot orphan the original. */
    if (cvm_tls_ctx != NULL) {
        return true;
    }

    CvmThreadNode* node = rt_mem_alloc(sizeof(CvmThreadNode));
    if (node == NULL) {
        return false;
    }

    node->ctx.top_frame = NULL;
    node->ctx.bump_ptr = NULL;
    node->ctx.bump_end = NULL;
    node->ctx.owner = node;

    node->handle = rt_thread_self();
    node->id = rt_thread_id();
    atomic_store(&node->state, (u32)CVM_THREAD_MANAGED);
    atomic_store(&node->parked, false);
    node->next = NULL;

    /* Publish to TLS before the registry, so the thread can use its own
     * context the moment attach returns. */
    cvm_tls_ctx = &node->ctx;

    cvm_threads_lock_write();
    node->next = registry_head;
    registry_head = node;
    cvm_threads_unlock_write();

    atomic_fetch_add(&registry_count, 1u);

    /*
     * A collection may already be running, in which case this thread has
     * to stop before it touches the heap. Polling here is what makes a
     * thread that appears mid-collection participate instead of sailing
     * through a heap that is being marked.
     *
     * Note the order: the registry lock is released before this point, so
     * no thread ever waits on the safepoint mutex while holding the
     * registry lock. See the lock-order note in vm/safepoint.c.
     */
    cvm_safepoint_poll();

    /*
     * Only now that the thread is registered and stopped if a collection
     * is running may it be given allocators pointing into the heap.
     */
    CvmWorld* world = cvm_world();
    cvm_allocator_init(&node->allocator, &world->block_alloc, world->heap.bytemap,
                       &world->heap);
    cvm_large_init(&node->large_allocator, &world->block_alloc, world->heap.bytemap,
                   &world->heap);

    /* The pieces the inlined allocation path needs to mark what it
     * allocates; see CvmVMContext. */
    node->ctx.heap_base = bytemap_base(world->heap.bytemap);
    node->ctx.object_meta = bytemap_slots(world->heap.bytemap);

    CVM_LOG_DEBUG(m) {
        fmt_lit(&m.fmt, "thread ");
        fmt_u64(&m.fmt, node->id);
        fmt_lit(&m.fmt, " registered, ");
        fmt_u32(&m.fmt, cvm_thread_count());
        fmt_lit(&m.fmt, " total");
    }
    return true;
}

void cvm_thread_detach(void) {
    CvmThreadNode* node = cvm_thread_node();
    if (node == NULL) {
        return; /* never attached, or already detached */
    }

    cvm_threads_lock_write();

    if (registry_head == node) {
        registry_head = node->next;
    } else {
        CvmThreadNode* prev = registry_head;
        while (prev != NULL && prev->next != node) {
            prev = prev->next;
        }
        if (prev != NULL) {
            prev->next = node->next;
        }
        /* If prev is NULL the node was not in the list, which means
         * detach raced with something that already removed it. Nothing to
         * unlink, and the free below is still correct because we own the
         * node. */
    }

    cvm_threads_unlock_write();

    atomic_fetch_sub(&registry_count, 1u);

    /* A collector waiting for this thread would otherwise sit out its
     * whole timeout before re-checking the registry. */
    cvm_safepoint_notify_thread_changed();

    /* Clear TLS before freeing: leaving a dangling context installed
     * would make any later use of the thread look registered. */
    cvm_tls_ctx = NULL;

    CVM_LOG_DEBUG(m) {
        fmt_lit(&m.fmt, "thread ");
        fmt_u64(&m.fmt, node->id);
        fmt_lit(&m.fmt, " unregistered, ");
        fmt_u32(&m.fmt, cvm_thread_count());
        fmt_lit(&m.fmt, " left");
    }

    rt_mem_free(node);
}

CvmThreadNode* cvm_thread_node(void) {
    CvmVMContext* ctx = cvm_tls_ctx;
    return (ctx != NULL) ? (CvmThreadNode*)ctx->owner : NULL;
}

CvmVMContext* cvm_thread_ctx(void) {
    return cvm_tls_ctx;
}

u32 cvm_thread_count(void) {
    return atomic_load(&registry_count);
}

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

CvmThreadState cvm_thread_get_state(void) {
    CvmThreadNode* node = cvm_thread_node();
    if (node == NULL) {
        return CVM_THREAD_MANAGED;
    }
    return (CvmThreadState)atomic_load_explicit(&node->state, memory_order_acquire);
}

void cvm_thread_set_state(CvmThreadState state) {
    CvmThreadNode* node = cvm_thread_node();
    if (node == NULL) {
        return;
    }
    atomic_store_explicit(&node->state, (u32)state, memory_order_release);
}

bool cvm_thread_node_is_alive(CvmThreadNode const* node) {
    return rt_thread_is_alive(node->handle);
}

/* ------------------------------------------------------------------ */
/* the FFI boundary                                                    */
/* ------------------------------------------------------------------ */

/*
 * Entering unmanaged state is a plain store, with release ordering so a
 * collector that observes the state also observes everything the thread
 * did before it.
 *
 * Leaving is where the synchroniser will hook in: if a collection started
 * while this thread was inside the foreign call, the thread must wait for
 * it to finish before touching the heap again. That is not implemented
 * yet, so leaving is currently just the state change.
 */
void cvm_thread_enter_unmanaged(void) {
    cvm_thread_set_state(CVM_THREAD_UNMANAGED);
}

void cvm_thread_leave_unmanaged(void) {
    cvm_thread_set_state(CVM_THREAD_MANAGED);
}

/* ------------------------------------------------------------------ */
/* iteration                                                           */
/* ------------------------------------------------------------------ */

static void cvm_threads_lock_write(void) {
    rt_rwlock_lock_write(&registry_lock);
}

static void cvm_threads_unlock_write(void) {
    rt_rwlock_unlock_write(&registry_lock);
}

void cvm_threads_lock_read(void) {
    rt_rwlock_lock_read(&registry_lock);
}

void cvm_threads_unlock_read(void) {
    rt_rwlock_unlock_read(&registry_lock);
}

CvmThreadNode* cvm_threads_head(void) {
    return registry_head;
}
