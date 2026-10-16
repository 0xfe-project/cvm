/*
 * cvm/external.h
 *
 * Objects that own something outside the heap.
 *
 * ---------------------------------------------------------------------
 * The shape
 * ---------------------------------------------------------------------
 *
 * A C library hands back a pointer to a structure it allocated — a FILE*,
 * a sqlite handle, a decoded image. The runtime wraps it so the pointer
 * becomes garbage-collected: when the wrapper dies, the library's
 * destructor runs and the memory is returned.
 *
 * That is what makes the claim "the runtime owns the whole process's
 * memory" true rather than aspirational. Without it, everything a foreign
 * library allocates is invisible and leaks.
 *
 * ---------------------------------------------------------------------
 * Why the descriptor is a scalar
 * ---------------------------------------------------------------------
 *
 * The object holds a C pointer and a function pointer. Neither is a GC
 * reference: the first points outside the heap and the second is code. So
 * the descriptor says "no GC pointers here", which is also what stops the
 * marker from attempting to follow either. A collector that traced a
 * function pointer as though it were an object would follow it into the
 * text segment and find nothing, or worse.
 *
 * ---------------------------------------------------------------------
 * When the finalizer runs
 * ---------------------------------------------------------------------
 *
 * Not during the sweep. A finalizer is user code: it may allocate, take a
 * lock, or block on a C library. Running it inside a collection means any
 * of those happen while the world is stopped or while the sweeper holds
 * heap state, which ranges from a stall to a deadlock.
 *
 * So the sweep does the minimum: it notices that a dying object carries a
 * finalizer, extracts the two words it needs — the C pointer and the
 * function — into a queue, and carries on. A dedicated thread drains the
 * queue and calls the finalizers, and it is a managed thread, so it
 * participates in collections like any other.
 *
 * Nothing can be resurrected by this, because the queue holds the C
 * pointer rather than the object. The object is already dead by the time
 * the finalizer runs; the finalizer cannot reach it even if it wanted to.
 */

#ifndef CVM_GC_EXTERNAL_H
#define CVM_GC_EXTERNAL_H

#include "base/types.h"
#include "cvm/gc.h"

/*
 * Releases a resource. Receives the pointer the object was wrapping, not
 * the object.
 *
 * Called on the finalizer thread with the world running, so it may
 * allocate, block, or call back into the runtime.
 */
/* CvmFinalizerFn is in cvm/gc.h — the type descriptor carries it, so that
 * is where it belongs. */

typedef struct CvmExternal {
    opaque_mut c_ptr;
    CvmFinalizerFn finalizer;
} CvmExternal;

/* The built-in descriptor: a scalar of this size, with the finalizer flag
 * set on objects allocated through cvm_external_new. */
extern CvmTypeDescriptor const cvm_type_external;

/*
 * Wrap a pointer, arranging for `finalizer` to run when the wrapper dies.
 *
 * `finalizer` may be NULL, in which case the object is a plain wrapper
 * with nothing to release. That is useful for holding a pointer for the
 * marker's benefit without owning it.
 */
opaque_mut cvm_external_new(opaque_mut c_ptr, CvmFinalizerFn finalizer);

/* The wrapped pointer, or NULL. */
opaque_mut cvm_external_ptr(opaque_mut object);

/* ---- runtime-internal, not for frontends or FFI glue ---- */

/* ------------------------------------------------------------------ */
/* the finalizer queue                                                 */
/* ------------------------------------------------------------------ */

/*
 * Hand a dead object's finalizer to the queue.
 *
 * Called by the sweeper. Does not allocate on the ordinary path: the
 * queue's nodes come from a pool that the finalizer thread refills, so a
 * collection never has to ask the heap for memory — which it could not
 * do, being in the middle of reclaiming it.
 *
 * Returns false if the queue is full, in which case the resource is
 * leaked rather than lost silently: the leak is counted and reported.
 */
bool cvm_finalizer_enqueue(opaque_mut c_ptr, CvmFinalizerFn finalizer);

/*
 * Prepare the queue. Idempotent.
 *
 * Separate from starting the thread, because a collection must be able to
 * enqueue finalizers whether or not the thread has been started — and
 * tests that want to drain the queue by hand should not have to start a
 * thread to do it.
 *
 * Called by cvm_world_init, so in a running program it is already done.
 */
bool cvm_finalizer_init(void);

/*
 * Prepare the queue if needed, then start the finalizer thread.
 * Idempotent. Returns false if the thread could not be created.
 */
bool cvm_finalizer_start(void);

/*
 * Stop the finalizer thread and drain what is left.
 *
 * Anything still queued has its finalizer run before this returns, so a
 * clean shutdown does not leak the resources the program was holding.
 */
void cvm_finalizer_stop(void);

/* Run every pending finalizer on the calling thread, without waiting. For
 * tests, and for shutdown. */
void cvm_finalizer_drain_now(void);

/* How many finalizers have run, and how many were dropped for want of a
 * queue node. */
u64 cvm_finalizer_run_count(void);
u64 cvm_finalizer_dropped_count(void);

/* Whether the finalizer thread is running. */
bool cvm_finalizer_is_running(void);

#endif /* CVM_GC_EXTERNAL_H */
