/*
 * vm/context.c
 *
 * The thread registry. See vm/context.h for what this deliberately does
 * not do, compared to the reference.
 *
 * The registry is an intrusive doubly-linked list — see base/list.h for
 * the shape — guarded by a reader-writer lock. Attach prepends, so a
 * thread appearing costs a constant number of pointer stores, and detach
 * unlinks in constant time. The doubly-linked form is what buys the
 * second of those: a singly-linked registry has to walk to find the
 * predecessor of the node being removed, and threads come and go for the
 * whole life of the process.
 *
 * The walk order carries no meaning: a collection visits every node.
 */

#include "context.h"

#include <stdatomic.h>

#include "base/types.h"
#include "cvm/gc.h"
#include "gc/bytemap.h"
#include "gc/world.h"
#include "log.h"
#include "stats.h"
#include "safepoint.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* registry state                                                      */
/* ------------------------------------------------------------------ */

/* Write side of the registry lock. File-private: nothing outside this
 * file may modify the list. */
static void cvm_threads_lock_write(void);
static void cvm_threads_unlock_write(void);

/*
 * The registry. `registry` is a sentinel head, not a node: it is never
 * handed out and never visited by iteration.
 */
static CvmList registry = CVM_LIST_HEAD_INIT(registry);
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
    cvm_list_init(&registry);
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

    node->ctx.frames = NULL;
    node->ctx.frame_depth = 0u;
    node->ctx.frame_capacity = 0u;
    node->ctx.bump_ptr = NULL;
    node->ctx.bump_end = NULL;
    node->ctx.owner = node;

    node->handle = rt_thread_self();
    node->id = rt_thread_id();
    node->role = (u32)CVM_THREAD_ROLE_MUTATOR;
    atomic_store(&node->state, (u32)CVM_THREAD_MANAGED);
    atomic_store(&node->unmanaged_depth, 0u);
    atomic_store(&node->parked, false);
    cvm_list_unlink(&node->link);

    /*
     * Every field, including the ones only one build configuration uses.
     * The node comes from rt_mem_alloc, so anything not written here is
     * whatever the allocator last had — which is how `trap` came to be a
     * non-null pointer to nothing in the configuration that does not use
     * it. Nothing read it there, but an uninitialised field is a bug
     * waiting for a build that does.
     */
    node->trap = NULL;
    node->safepoint_resume_pc = 0;
    atomic_store(&node->sweep_cursor_done, 0);
    atomic_store(&node->sweeping, false);
    atomic_store(&node->activity, (u32)CVM_ACT_IDLE);

    /*
     * An event buffer, if the program asked for stats. NULL otherwise, and
     * every recording call is a null check in that case. See
     * cvm_stats_open for why this is a pointer rather than embedded.
     */
    node->stats = cvm_stats_open();

    /* Publish to TLS before the registry, so the thread can use its own
     * context the moment attach returns. */
    cvm_tls_ctx = &node->ctx;

    /*
     * The trap page has to exist before anything polls, because in trap
     * mode the poll *is* a load through this thread's cell — and would
     * fault on a null one. It is created here rather than later precisely
     * so that the poll below has somewhere to read. (It did not, once, and
     * the result was a crash inside the registration path.)
     */
    cvm_safepoint_trap_attach(node);

    /*
     * Everything a collector reads through this node has to be valid
     * *before* the node is in the registry, not after.
     *
     * The registry is what makes this node visible to a collector, and a
     * collector reads three fields of the allocator: `heap`, and
     * `recycled_blocks` — onto which the sweep pushes blocks it has just
     * freed — and `recycled_block_count`, which the growth decision sums
     * into its free-block figure. The node comes from rt_mem_alloc, so
     * between the insert and an initialisation further down those three
     * read as whatever the allocator last held. A garbage `recycled_blocks`
     * head is a sweep pushing onto a list that points nowhere in
     * particular; a garbage count is a growth decision made on a number
     * nobody wrote. Valgrind found the second one, from a test that was
     * already in the suite.
     *
     * Initialising here rather than after the poll below does not weaken
     * the poll's job. A thread cannot allocate until attach returns, and
     * attach does not return until the poll has stopped it if a collection
     * is running — so the allocators are both valid and unused for the
     * whole window.
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

    cvm_threads_lock_write();
    cvm_list_add(&node->link, &registry);
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
    cvm_safepoint_arm_page_if_collecting();
    cvm_safepoint_poll();

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

    /*
     * Detach may race with something that already removed the node, in
     * which case its link is NULLed and there is nothing left to unlink.
     * The free below is still correct either way, because this thread owns
     * the node.
     */
    if (cvm_list_is_linked(&node->link)) {
        cvm_list_del(&node->link);
    }

    cvm_threads_unlock_write();

    atomic_fetch_sub(&registry_count, 1u);

    cvm_safepoint_trap_detach(node);

    /* A collector waiting for this thread would otherwise sit out its
     * whole timeout before re-checking the registry. */
    cvm_safepoint_notify_thread_changed();

    /*
     * The frame array goes with the node, and only after the node is out of
     * the registry: while it is still visible, a collection walks through
     * this thread's context and would read frames that had just been freed.
     */
    rt_mem_free(node->ctx.frames);
    node->ctx.frames = NULL;
    node->ctx.frame_depth = 0u;
    node->ctx.frame_capacity = 0u;

    /* Clear TLS before freeing: leaving a dangling context installed
     * would make any later use of the thread look registered. */
    cvm_tls_ctx = NULL;

    /* Flushed rather than dropped: a thread's last events are the ones that
     * say how it finished. */
    cvm_stats_close(node->stats);
    node->stats = NULL;

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

    /*
     * A thread that becomes unmanaged is one a collector no longer has to
     * wait for, and it should not have to wait out its whole diagnostic
     * interval to notice. The check keeps this off the hot path: when no
     * collection is running there is nothing to notify, and a relaxed load
     * of a flag is all this costs.
     */
    if (state == CVM_THREAD_UNMANAGED && cvm_safepoint_in_progress()) {
        cvm_safepoint_notify_thread_changed();
    }
}

/*
 * Grow the thread's frame array so that one more frame fits.
 *
 * Out of line because it is the cold half of push: the array doubles, so a
 * thread that reaches depth N pays for log2(N) reallocations in total.
 *
 * Fatal on failure rather than returning. A thread that cannot record its
 * frames stops being a root set, and a collection that runs after that
 * reclaims objects the thread still holds -- a wrong answer delivered
 * quietly. There is no recovery to offer the caller either: generated code
 * cannot do anything useful about it and has no way to report it.
 */
void cvm_frame_grow(CvmVMContext* ctx) {
    u32 capacity = (ctx->frame_capacity == 0u) ? 8u : (ctx->frame_capacity * 2u);

    CvmGcFrame* grown = rt_mem_alloc((usize)capacity * sizeof(CvmGcFrame));
    if (grown == NULL) {
        rt_fatal(STR_LITERAL("out of memory growing the frame stack"));
    }

    for (u32 i = 0; i < ctx->frame_depth; i++) {
        grown[i] = ctx->frames[i];
    }

    rt_mem_free(ctx->frames);
    ctx->frames = grown;
    ctx->frame_capacity = capacity;
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
 * Leaving is where the synchroniser hooks in: if a collection started while
 * this thread was inside the foreign call, the thread must wait for it to
 * finish before touching the heap again, and it does that by polling after
 * it has announced itself managed again.
 *
 * Both are counted rather than flagged. Nesting is the ordinary case -- a
 * foreign call that itself calls back into managed code that makes another
 * foreign call -- and a flag cannot express it: `enter(); enter(); leave();`
 * would report the thread managed while it is still inside the outer call.
 * Only the transition to depth zero changes the state a collector reads.
 */
void cvm_thread_enter_unmanaged(void) {
    CvmThreadNode* node = cvm_thread_node();
    if (node == NULL) {
        /* Nothing registered: there is no state to be unmanaged relative
         * to, and a collector is not waiting for this thread. */
        return;
    }

    u32 depth =
        atomic_fetch_add_explicit(&node->unmanaged_depth, 1u, memory_order_acq_rel) +
        1u;
    if (depth == 1u) {
        /* First entry: publish the state a collector reads, and wake one
         * that is waiting out its diagnostic interval for this thread. A
         * collector no longer has to wait for an unmanaged thread, and it
         * should not have to wait to notice. */
        atomic_store_explicit(&node->state, (u32)CVM_THREAD_UNMANAGED,
                              memory_order_release);
        if (cvm_safepoint_in_progress()) {
            cvm_safepoint_notify_thread_changed();
        }
    }
}

void cvm_thread_leave_unmanaged(void) {
    CvmThreadNode* node = cvm_thread_node();
    if (node == NULL) {
        return;
    }

    u32 depth = atomic_load_explicit(&node->unmanaged_depth, memory_order_relaxed);
    for (;;) {
        if (depth == 0) {
            /* Leaving without a matching enter. Restore the invariant
             * rather than leaving a running thread looking unmanaged, but
             * do not skip the poll: a foreign call ended either way. */
            cvm_thread_set_state(CVM_THREAD_MANAGED);
            cvm_safepoint_poll();
            return;
        }
        if (atomic_compare_exchange_weak_explicit(&node->unmanaged_depth, &depth,
                                                  depth - 1, memory_order_acq_rel,
                                                  memory_order_relaxed)) {
            break;
        }
    }

    if (depth != 1u) {
        /* Still nested. This thread remains unmanaged and a collection is
         * still entitled to skip it. */
        return;
    }

    /*
     * Now managed, so a collection must be able to stop this thread -- and
     * one may have started while it was inside the foreign call. Polling
     * here is what makes returning from foreign code safe.
     *
     * The order matters and is not interchangeable. Announcing the managed
     * state first means a collector that observes it will wait for this
     * thread to park, which the poll then does. Polling first would leave
     * a window where the thread looks unmanaged and is skipped, but is
     * about to touch the heap.
     */
    cvm_thread_set_state(CVM_THREAD_MANAGED);
    cvm_safepoint_poll();
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
    CvmList* first = cvm_list_first(&registry);
    return first == NULL ? NULL : cvm_list_entry(first, CvmThreadNode, link);
}

CvmThreadNode* cvm_threads_next(CvmThreadNode* node) {
    CvmList* next = node->link.next;
    /* The sentinel is the end of the list, and is not a node. */
    return next == &registry ? NULL : cvm_list_entry(next, CvmThreadNode, link);
}

/* ------------------------------------------------------------------ */
/* the activity breadcrumb                                             */
/* ------------------------------------------------------------------ */

static str const role_names[CVM_THREAD_ROLE_COUNT] = {
    [CVM_THREAD_ROLE_MUTATOR] = {.ptr = (u8 const*)"mutator", .len = 7},
    [CVM_THREAD_ROLE_FINALIZER] = {.ptr = (u8 const*)"finalizer", .len = 9},
};

str cvm_thread_role_name(u32 role) {
    if (role >= (u32)CVM_THREAD_ROLE_COUNT) {
        return (str){.ptr = (u8 const*)"?", .len = 1};
    }
    return role_names[role];
}

void cvm_thread_set_role(CvmThreadRole role) {
    CvmThreadNode* node = cvm_thread_node();
    if (node == NULL) {
        return;
    }
    node->role = (u32)role;
}

static str const activity_names[CVM_ACT_TYPE_COUNT] = {
    [CVM_ACT_IDLE] = {.ptr = (u8 const*)"idle", .len = 4},
    [CVM_ACT_ALLOCATING] = {.ptr = (u8 const*)"allocating", .len = 10},
    [CVM_ACT_SWEEPING] = {.ptr = (u8 const*)"sweeping", .len = 8},
    [CVM_ACT_COALESCING] = {.ptr = (u8 const*)"coalescing", .len = 10},
    [CVM_ACT_COLLECTING] = {.ptr = (u8 const*)"collecting", .len = 10},
    [CVM_ACT_MARKING] = {.ptr = (u8 const*)"marking", .len = 7},
    [CVM_ACT_NULLIFYING] = {.ptr = (u8 const*)"nullifying", .len = 10},
    [CVM_ACT_WAITING_FOR_COLLECTION] = {.ptr = (u8 const*)"waiting-for-collection",
                                        .len = 22},
    [CVM_ACT_ATTACHING] = {.ptr = (u8 const*)"attaching", .len = 9},
    [CVM_ACT_DETACHING] = {.ptr = (u8 const*)"detaching", .len = 9},
    [CVM_ACT_FINALIZING] = {.ptr = (u8 const*)"finalizing", .len = 10},
    [CVM_ACT_JOINING] = {.ptr = (u8 const*)"joining", .len = 7},
    [CVM_ACT_PARKING] = {.ptr = (u8 const*)"parking", .len = 7},
};

str cvm_thread_activity_name(CvmThreadActivity activity) {
    if ((u32)activity >= (u32)CVM_ACT_TYPE_COUNT) {
        return STR_LITERAL("unknown");
    }
    return activity_names[activity];
}

void cvm_thread_set_activity(CvmThreadActivity activity) {
    CvmThreadNode* node = cvm_thread_node();
    if (node != NULL) {
        atomic_store_explicit(&node->activity, (u32)activity, memory_order_relaxed);
    }
}

CvmThreadActivity cvm_thread_get_activity(CvmThreadNode const* node) {
    return (CvmThreadActivity)atomic_load_explicit(&node->activity,
                                                   memory_order_relaxed);
}
