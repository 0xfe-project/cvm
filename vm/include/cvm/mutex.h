/*
 * vm/include/cvm/mutex.h
 *
 * A lock, and the one thing a lock has to say in a runtime that stops the world.
 *
 * ---------------------------------------------------------------------
 * The problem
 * ---------------------------------------------------------------------
 *
 * A thread blocked on a mutex is not executing managed code and will not reach a
 * safepoint. A collector cannot tell that from the outside: it sees a managed
 * thread that is not parked, and it waits for it. If the lock is held by a thread
 * that is itself waiting for the collection to end, both wait forever.
 *
 * The runtime already says what to do about it — cvm_thread_enter_unmanaged around
 * a blocking call, the same declaration generated code makes around a foreign call
 * — and the failure is that it has to be *remembered* at every call site. It was
 * forgotten once, while the atom table was being written, and the symptom was a
 * deadlock that the concurrency tests found only because they run under load. So
 * the declaration moves into the lock, where it cannot be left out.
 *
 * ---------------------------------------------------------------------
 * When the wait must be declared, and when declaring it is wrong
 * ---------------------------------------------------------------------
 *
 * The criterion is about the *holder*, not the waiter. A wait needs declaring when
 * the lock can be held across a safepoint, because then the holder can park while
 * holding it, and a waiter that the collector is waiting for is waiting on a
 * thread that is waiting on the collection.
 *
 *   - cvm_mutex_lock: the holder may allocate or park while holding it. The wait
 *     is declared unmanaged, so the collector does not wait for a thread that is
 *     only waiting for another thread.
 *
 *   - cvm_mutex_lock_raw: no declaration. For the collector's own threads, which
 *     have to stay visible to it — a thread parking for a collection is *supposed*
 *     to be waited for, and declaring it unmanaged would tell the collector to go
 *     on without it. It is also the right choice for a lock whose critical section
 *     contains no safepoint: there is no unbounded wait to hide, and declaring one
 *     would let a mutator run through a phase that expects it stopped.
 *
 * So the verb at the call site answers "which world is this thread in", and a
 * reader does not have to reconstruct the argument to find out. A caller that
 * cannot say which of the two it is looking at has found the bug already.
 */

#ifndef CVM_MUTEX_H
#define CVM_MUTEX_H

#include "base/types.h"
#include "rt.h"

typedef struct CvmMutex {
    RtMutex raw;
} CvmMutex;

bool cvm_mutex_init(CvmMutex* m);
void cvm_mutex_destroy(CvmMutex* m);

/*
 * Take the lock, declaring the wait. The thread is managed again by the time this
 * returns: it is about to touch the heap, and it has to be able to reach a
 * safepoint.
 *
 * Asserts, in a debug build, that the calling thread is not one of the collector's
 * own — those must use the raw form, and this is the mistake that would otherwise
 * be silent.
 */
void cvm_mutex_lock(CvmMutex* m);

/* Take the lock if it is free. Returns false without blocking. */
bool cvm_mutex_trylock(CvmMutex* m);

void cvm_mutex_unlock(CvmMutex* m);

/* Take the lock without declaring anything. See the note above for when. */
void cvm_mutex_lock_raw(CvmMutex* m);

#endif /* CVM_MUTEX_H */
