/*
 * cvm/roots.h
 *
 * The global root table, replacing the address-range list in
 * refs/commix/immix_comix/GCRoots.{h,c}.
 *
 * ---------------------------------------------------------------------
 * What it is for
 * ---------------------------------------------------------------------
 *
 * A frame chain is a thread's roots and lives only as long as the
 * activation. Foreign code has no activation: a C callback registry, an
 * event loop's user data, a library holding a handle it was given. Those
 * need a root that outlives any particular call, so the runtime keeps a
 * table of them, and the marker walks it alongside the frame chains.
 *
 * The reference instead keeps a list of address *ranges* and sweeps them
 * conservatively. With a precise collector there is nothing to sweep: a
 * root is a pointer to an object, so the table is an array of pointers
 * and marking one is a direct call.
 *
 * ---------------------------------------------------------------------
 * Adding a root during a collection
 * ---------------------------------------------------------------------
 *
 * This is the part worth getting right, and it is not obvious.
 *
 * A thread inside a foreign call is unmanaged, so a stop-the-world does
 * not stop it — that is the whole point of the unmanaged state. But such
 * a thread can still call cvm_root_add, and if it does so while marking is
 * running, marking may have already passed the table and would miss the
 * new root. The object would then be reclaimed while foreign code holds
 * it, which is the worst failure the collector has.
 *
 * So adding a root is a write barrier: if a collection is in progress, the
 * object is marked before the call returns. Marking is idempotent and the
 * packet lists are lock-free, so doing it from another thread is safe, and
 * by the time the caller has the handle the object is already known to be
 * live.
 *
 * The cost is one relaxed load on the add path when nothing is happening,
 * and a mark when something is. Both are rare — roots are registered at
 * setup, not in a loop — so this is not a hot path being pessimised.
 */

#ifndef CVM_GC_ROOT_H
#define CVM_GC_ROOT_H

#include "base/types.h"
#include "gc/grey_packet.h"
#include "gc/heap.h"

/* An opaque handle to one slot. Zero is never a valid handle. */
typedef u32 CvmRootHandle;

#define CVM_ROOT_HANDLE_NONE ((CvmRootHandle)0)

/*
 * Set up the table. Called once, from the same place the heap is set up.
 * Returns false if the initial allocation failed.
 */
bool cvm_root_init(void);

void cvm_root_destroy(void);

/*
 * Register an object as a root and hand back a handle.
 *
 * Returns CVM_ROOT_HANDLE_NONE if the table could not grow. If a
 * collection is in progress the object is marked first; see the note
 * above.
 */
CvmRootHandle cvm_root_add(opaque_mut obj);

/* The object, or NULL for an invalid or cleared handle. */
opaque_mut cvm_root_get(CvmRootHandle handle);

/* Replace the object in a slot. Same write-barrier treatment as adding. */
void cvm_root_set(CvmRootHandle handle, opaque_mut obj);

/* Release a slot. Invalid handles are ignored. */
void cvm_root_remove(CvmRootHandle handle);

/* Slots in use. */
u32 cvm_root_count(void);

/* Slots currently allocated, for tests and diagnostics. */
u32 cvm_root_capacity(void);

/* ---- runtime-internal ---- */

/*
 * Mark everything in the table. Called by the marker as part of root
 * finding.
 */
void cvm_root_mark_all(CvmHeap* heap, CvmGreyPacket** out, CvmGreyPacket** out_weak);

#endif /* CVM_GC_ROOT_H */
