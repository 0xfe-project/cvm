/*
 * vm/context.h
 *
 * The thread registry, replacing refs/commix/MutatorThread.{c,h} and
 * State.h.
 *
 * Every thread that can touch the heap registers here before it does so,
 * and unregisters before it exits. A collection walks the registry to
 * find every thread's frame chain.
 *
 * ---------------------------------------------------------------------
 * What the reference carries that we do not
 * ---------------------------------------------------------------------
 *
 * MutatorThread has stackBottom, an atomic stackTop, a register capture
 * buffer, and an "approximate stack top" helper that takes the address of
 * a local. All of that exists to support conservative stack scanning: to
 * know which address range to sweep for pointer-shaped values, and to
 * save the registers that hold live pointers when a thread is stopped.
 *
 * A precise collector needs none of it. Roots come from the frame chain,
 * which the thread maintains itself, so there is no address range to
 * bound and no register to spill — the frames are already in the heap and
 * already reachable from the context. Dropping this is the single largest
 * simplification our design buys, and it removes the class of bug where a
 * stack bound is computed wrong and a live pointer is missed.
 *
 * What remains is what the registry genuinely needs: who is registered,
 * what state each thread is in, and a handle for liveness checks.
 */

#ifndef CVM_CONTEXT_H
#define CVM_CONTEXT_H

#include <stdatomic.h>

#include "base/types.h"
#include "cvm/gc.h"
#include "rt.h"

/*
 * One registered thread.
 *
 * The context is embedded rather than pointed to, so a thread needs a
 * single allocation and there is no lifetime question about the two
 * halves. `ctx.owner` points back here; the link is explicit on both
 * sides so neither direction depends on the embedding offset.
 */
typedef struct CvmThreadNode {
    struct CvmThreadNode* next; /* registry link */

    CvmVMContext ctx; /* embedded; ctx.owner points back at this node */

    RtThread handle;
    u64 id;

    /*
     * CvmThreadState. Atomic because a collection reads it while the
     * thread it describes is still running.
     */
    _Atomic u32 state;

    /*
     * Set by the thread itself when it is stopped at a safepoint, cleared
     * when it resumes. Read by the collecting thread while the parked
     * thread is blocked, so it has to be atomic.
     */
    _Atomic bool parked;
} CvmThreadNode;

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

/*
 * Prepare the registry. Must be called once, before any thread attaches.
 * Calling it again is a no-op.
 */
void cvm_threads_init(void);

/*
 * Register the calling thread and install its context in TLS.
 *
 * Returns false if the node could not be allocated, in which case the
 * thread must not touch the heap. Idempotent: a thread that is already
 * registered gets its existing node back and a true return.
 */
bool cvm_thread_attach(void);

/*
 * Unregister the calling thread and clear its TLS.
 *
 * Safe to call from a thread that never attached. Must be called before
 * the thread exits: a registry entry for a dead thread makes a collection
 * wait forever for a safepoint that cannot come. (A collection also
 * checks liveness, so a missing detach degrades rather than hangs, but
 * the entry is still a leak.)
 */
void cvm_thread_detach(void);

/* The calling thread's node, or NULL if it has not attached. */
CvmThreadNode* cvm_thread_node(void);

/* The calling thread's context, or NULL. Same as cvm_ctx() for a
 * registered thread; this spelling makes the requirement explicit. */
CvmVMContext* cvm_thread_ctx(void);

/* Registered threads. Atomic, so it is safe to read without the lock. */
u32 cvm_thread_count(void);

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

CvmThreadState cvm_thread_get_state(void);
void cvm_thread_set_state(CvmThreadState state);

/*
 * Whether a registered thread is still running.
 *
 * A thread that died without detaching leaves an entry behind; a
 * collection uses this to skip it rather than waiting for it.
 */
bool cvm_thread_node_is_alive(CvmThreadNode const* node);

/* ------------------------------------------------------------------ */
/* iteration                                                           */
/* ------------------------------------------------------------------ */

/*
 * Walk the registry. The caller must hold the read lock for the whole
 * walk, otherwise a thread registering mid-iteration invalidates the
 * traversal.
 *
 * Writers are given priority (see rt_rwlock_init), so a steady stream of
 * collectors cannot keep a registering thread out.
 */
void cvm_threads_lock_read(void);
void cvm_threads_unlock_read(void);

CvmThreadNode* cvm_threads_head(void);

#define CVM_THREADS_FOREACH(node)                                                      \
    for (CvmThreadNode* node = cvm_threads_head(); node != NULL; node = node->next)

#endif /* CVM_CONTEXT_H */
