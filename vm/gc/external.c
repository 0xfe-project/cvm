/*
 * vm/gc/external.c
 *
 * External handles and the finalizer thread. See include/cvm/external.h for the
 * shape and why the finalizers run on their own thread rather than in the
 * sweep.
 */

#include "cvm/external.h"

#include <stdatomic.h>

#include "base/assert.h"
#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "gc/heap.h"
#include "gc/meta.h"
#include "gc/world.h"
#include "log.h"
#include "rt.h"
#include "safepoint.h"
#include "stats.h"

/* ------------------------------------------------------------------ */
/* the built-in type                                                   */
/* ------------------------------------------------------------------ */

/*
 * A scalar: the object holds a C pointer and a function pointer, and
 * neither is a GC reference. Saying so is also what stops the marker from
 * following either of them.
 */
CvmTypeDescriptor const cvm_type_external = {
    .size = sizeof(CvmExternal),
    .kind = CVM_TYPE_SCALAR,
};

/*
 * The external type has NO type-level finalizer. Its finalizers are
 * per-instance — chosen at construction, stored in the object's payload —
 * and the sweep reads them at sweep time, because the object's bytes are
 * about to stop being its.
 *
 * The metadata flag is still set, because the sweep still has to notice the
 * object. What the flag says is "this object needs attention when it dies",
 * not "the descriptor knows what to do".
 */
static CvmTypeDescriptor const cvm_type_external_with_finalizer = {
    .size = sizeof(CvmExternal),
    .kind = CVM_TYPE_SCALAR,
    /* .finalizer stays NULL: the sweep reads the payload instead. */
};

opaque_mut cvm_external_new(opaque_mut c_ptr, CvmFinalizerFn finalizer) {
    CvmTypeDescriptor const* type =
        finalizer != NULL ? &cvm_type_external_with_finalizer : &cvm_type_external;

    CvmExternal* ext = cvm_alloc(type);
    ext->c_ptr = c_ptr;
    ext->finalizer = finalizer;

    /*
     * The flag is set here rather than by the allocation path, because the
     * external type's finalizer is per-instance: the descriptor says NULL,
     * and the allocation path only flags what the descriptor names. The
     * object still needs the flag, or the sweep never looks at it.
     */
    if (finalizer != NULL) {
        CvmGcHeader* header = cvm_header_of_mut(ext);
        cvm_note_finalizer(header);
    }

    return ext;
}

opaque_mut cvm_external_ptr(opaque_mut object) {
    if (object == NULL) {
        return NULL;
    }
    return ((CvmExternal*)object)->c_ptr;
}

/* ------------------------------------------------------------------ */
/* the queue                                                           */
/* ------------------------------------------------------------------ */

/*
 * Queue nodes come from a fixed pool that exists from startup, never from
 * the GC heap.
 *
 * The reason is a specific hazard rather than tidiness. Enqueuing happens
 * during a sweep, and a sweep can run with the world stopped — the
 * collector drives an unfinished sweep to completion before starting a new
 * collection, and that happens under the stop. Allocating from the GC heap
 * there would mean asking the heap for memory while holding the heap, and
 * allocating from libc there would mean taking libc's lock while the
 * thread that holds it is stopped. A pool built at startup avoids both.
 *
 * The pool is drained and refilled by the finalizer thread: nodes go
 * pending, then back to free. Steady state needs no allocation at all. A
 * burst larger than the pool is reported rather than silently losing
 * resources — see cvm_finalizer_dropped_count.
 */
#define CVM_FINALIZER_POOL_SIZE 4096u

typedef struct FinalizerNode {
    struct FinalizerNode* next;
    opaque_mut c_ptr;
    CvmFinalizerFn finalizer;
} FinalizerNode;

static FinalizerNode pool[CVM_FINALIZER_POOL_SIZE];

/* Free nodes. Producers pop, the finalizer thread pushes. */
static _Atomic(usize) free_head = 0;

/* Pending work. Many producers, one consumer. */
static _Atomic(usize) pending_head = 0;

static RtSem pending_sem;

static _Atomic bool finalizer_ready = false;
static _Atomic bool shutdown = false;
static RtThread finalizer_thread;
static _Atomic bool thread_running = false;

static _Atomic u64 run_count = 0;
static _Atomic u64 dropped_count = 0;

static void push_atomic(_Atomic(usize)* head, FinalizerNode* node) {
    usize old = atomic_load_explicit(head, memory_order_relaxed);
    for (;;) {
        node->next = (FinalizerNode*)old;
        if (atomic_compare_exchange_weak_explicit(
                head, &old, (usize)node, memory_order_release, memory_order_relaxed)) {
            return;
        }
    }
}

static FinalizerNode* pop_atomic(_Atomic(usize)* head) {
    usize old = atomic_load_explicit(head, memory_order_acquire);
    for (;;) {
        if (old == 0) {
            return NULL;
        }
        FinalizerNode* node = (FinalizerNode*)old;
        usize next = (usize)node->next;
        if (atomic_compare_exchange_weak_explicit(
                head, &old, next, memory_order_acquire, memory_order_acquire)) {
            return node;
        }
    }
}

static void pool_init(void) {
    atomic_store(&free_head, 0);
    atomic_store(&pending_head, 0);

    /* Fill the free list. Built in reverse so the list comes out in
     * address order, which makes a dump of it readable. */
    for (u32 i = CVM_FINALIZER_POOL_SIZE; i > 0; i--) {
        FinalizerNode* node = &pool[i - 1];
        node->next = NULL;
        node->c_ptr = NULL;
        node->finalizer = NULL;
        push_atomic(&free_head, node);
    }
}

bool cvm_finalizer_enqueue(opaque_mut c_ptr, CvmFinalizerFn finalizer) {
    if (finalizer == NULL || !atomic_load(&finalizer_ready)) {
        return false;
    }

    FinalizerNode* node = pop_atomic(&free_head);
    if (node == NULL) {
        atomic_fetch_add(&dropped_count, 1);
        return false;
    }

    node->c_ptr = c_ptr;
    node->finalizer = finalizer;
    push_atomic(&pending_head, node);

    rt_sem_post(&pending_sem);
    return true;
}

/* ------------------------------------------------------------------ */
/* the finalizer thread                                                */
/* ------------------------------------------------------------------ */

static void run_one(FinalizerNode* node) {
    CvmFinalizerFn fn = node->finalizer;
    opaque_mut c_ptr = node->c_ptr;

    node->c_ptr = NULL;
    node->finalizer = NULL;

    /*
     * The node goes back to the pool before the finalizer runs, so a
     * finalizer that allocates and collects cannot find the queue empty on
     * its own account.
     */
    push_atomic(&free_head, node);

    /*
     * Released from the managed state: the finalizer may allocate, block,
     * or call back in, and a collection must be able to stop this thread
     * while it runs.
     */
    fn(c_ptr);

    atomic_fetch_add(&run_count, 1);
}

static opaque_mut finalizer_main(opaque_mut arg) {
    (void)arg;

    if (!cvm_thread_attach()) {
        CVM_LOG_ERROR(m) {
            fmt_lit(&m.fmt, "finalizer thread could not register");
        }
        return NULL;
    }

    /*
     * Two things for the stuck-thread report, both of them because this thread
     * is the one a collection is most likely to be waiting for while it is not
     * obviously doing anything: it runs user code that may not allocate, and so
     * may not reach a safepoint poll.
     *
     * The role names it. The activity says what it is doing — without it this
     * thread reports as "idle" throughout, which describes every thread in the
     * process equally well.
     */
    cvm_thread_set_role(CVM_THREAD_ROLE_FINALIZER);

    for (;;) {
        /*
         * Waiting here must not look like a thread that is running: an
         * unregistered block would make every collection wait for a
         * safepoint that cannot come. Entering the unmanaged state is
         * exactly the declaration "I am inside foreign code", and a
         * collector skips such a thread.
         */
        cvm_thread_enter_unmanaged();
        rt_sem_wait(&pending_sem);
        cvm_thread_leave_unmanaged();

        if (atomic_load(&shutdown)) {
            break;
        }

        FinalizerNode* node = pop_atomic(&pending_head);
        if (node != NULL) {
            cvm_thread_set_activity(CVM_ACT_FINALIZING);
            run_one(node);
            cvm_thread_set_activity(CVM_ACT_IDLE);
        }
    }

    cvm_thread_detach();
    return NULL;
}

bool cvm_finalizer_init(void) {
    bool expected = false;
    if (!atomic_compare_exchange_strong(&finalizer_ready, &expected, true)) {
        return true; /* already initialised */
    }

    pool_init();
    if (!rt_sem_init(&pending_sem, 0)) {
        atomic_store(&finalizer_ready, false);
        return false;
    }
    return true;
}

bool cvm_finalizer_start(void) {
    if (!cvm_finalizer_init()) {
        return false;
    }
    if (atomic_load(&thread_running)) {
        return true;
    }

    atomic_store(&shutdown, false);
    if (!rt_thread_create(&finalizer_thread, finalizer_main, NULL)) {
        return false;
    }

    atomic_store(&thread_running, true);
    return true;
}

void cvm_finalizer_stop(void) {
    if (!atomic_load(&finalizer_ready)) {
        return;
    }

    if (atomic_load(&thread_running)) {
        atomic_store(&shutdown, true);
        rt_sem_post(&pending_sem);
        rt_thread_join(finalizer_thread);
        atomic_store(&thread_running, false);
    }

    /* Whatever is left runs here, so shutdown does not leak the resources
     * the program was holding. */
    cvm_finalizer_drain_now();

    rt_sem_destroy(&pending_sem);
    atomic_store(&finalizer_ready, false);
}

void cvm_finalizer_drain_now(void) {
    if (!atomic_load(&finalizer_ready)) {
        return;
    }
    FinalizerNode* node;
    while ((node = pop_atomic(&pending_head)) != NULL) {
        run_one(node);
    }
}

u64 cvm_finalizer_run_count(void) {
    return atomic_load(&run_count);
}

u64 cvm_finalizer_dropped_count(void) {
    return atomic_load(&dropped_count);
}

bool cvm_finalizer_is_running(void) {
    return atomic_load(&thread_running);
}
