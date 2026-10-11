/*
 * vm/include/cvm/retire.h
 *
 * Keeping a replaced object alive for one more collection.
 *
 * ---------------------------------------------------------------------
 * What it is for
 * ---------------------------------------------------------------------
 *
 * A shared structure that gets replaced wholesale — a hash table's resize
 * installs new arrays and drops its reference to the old ones — becomes
 * unreachable the moment it is replaced. The collector's sweep runs
 * concurrently with the program, so it can reclaim that memory while a reader
 * is still walking it. This is not hypothetical: the atom table's lock-free
 * lookup crashed under tsan inside a key comparison, because the array it was
 * walking had been replaced a moment before.
 *
 * The observation that makes a grace period possible: a lock-free reader is a
 * *managed* thread. It allocates nothing and takes no lock, so it cannot be
 * interrupted — but the collector, at the start of the next collection, waits
 * for every managed thread to reach a safepoint, and a reader's safepoint is
 * *after* its read. So once one collection has begun and finished its stop, no
 * managed thread can still be inside a structure that was retired before that
 * collection started.
 *
 * ---------------------------------------------------------------------
 * The contract
 * ---------------------------------------------------------------------
 *
 *   cvm_retire(payload) promises two things about `payload`:
 *
 *     - nothing will *reach* it after this call — the caller dropped the last
 *       reference, and new readers cannot enter it (the replacement is what
 *       triggers the retire);
 *     - readers that entered it before this call are managed threads, and the
 *       memory only has to outlive them.
 *
 *   In return, the object survives the next full collection and is reclaimed
 *   by the one after it.
 *
 * The first promise is what makes a *grey* mark correct rather than a black
 * one: the object is marked and traced like any live object, because a reader
 * walks *through* it — a replaced key array's slots are dereferenced by every
 * probe that walks it — so what it points to has to stay valid too. Retiring
 * with a black mark would keep the array and free its elements out from under
 * the reader.
 *
 * ---------------------------------------------------------------------
 * Who this does not cover
 * ---------------------------------------------------------------------
 *
 * A reader holding a mutex is unmanaged — the collector does not wait for it,
 * by design, because a thread blocked in a wait is exactly what a stop must
 * not wait for. For such a reader the ordering has to come from the lock:
 * retire inside the critical section that also clears whatever lets new
 * readers in, so that when the lock is released, no reader is inside and none
 * can enter. The grace period then only has to bridge the time until the
 * next sweep, which it does with a collection to spare.
 */

#ifndef CVM_RETIRE_H
#define CVM_RETIRE_H

#include "base/types.h"

/*
 * Keep `payload` alive for one more collection, then let it go.
 *
 * `payload` is the object's payload pointer, the same thing every other API
 * hands around. NULL is accepted and ignored, because the callers are
 * replacement paths where "nothing was there before" is a normal case.
 */
void cvm_retire(opaque_mut payload);

#endif /* CVM_RETIRE_H */
