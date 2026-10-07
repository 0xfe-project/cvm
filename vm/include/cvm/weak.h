/*
 * cvm/weak.h
 *
 * Weak references, from refs/commix/WeakReferences.c.
 *
 * ---------------------------------------------------------------------
 * The two-phase shape
 * ---------------------------------------------------------------------
 *
 * A weak reference must not keep its referent alive, so marking cannot
 * trace through it. But it also cannot be cleared during marking, because
 * whether the referent survives is not known until marking finishes.
 *
 * So marking records weak references it encounters on a list of their own,
 * and does not follow their referents. When marking is done, the nullify
 * phase walks that list and clears the referent of every weak reference
 * whose target did not survive.
 *
 * The consequence for the completion check: a packet on the weak list
 * counts as accounted for, because there is nothing more to trace through
 * it. That is why cvm_mark_is_done adds the weak list's size to the empty
 * list's.
 *
 * ---------------------------------------------------------------------
 * Why a header flag rather than a type
 * ---------------------------------------------------------------------
 *
 * Two objects of the same type can differ in whether they hold their
 * referent strongly — a weak map's key slot and a strong map's key slot
 * have the same shape. A type descriptor is shared between all instances
 * of a type, so the property cannot live there. It lives in the object's
 * header flags, and the marker consults it per object.
 *
 * ---------------------------------------------------------------------
 * No resurrection
 * ---------------------------------------------------------------------
 *
 * Clearing is a write to a field of an object that is itself alive; it
 * does not revive anything. A weak reference whose target is dead simply
 * has its target set to NULL. There is no finalizer here and no queue:
 * the only thing that happens is that the slot stops pointing at
 * something that is about to be swept.
 */

#ifndef CVM_GC_WEAK_H
#define CVM_GC_WEAK_H

#include "base/types.h"
#include "cvm/gc.h"
#include "gc/heap.h"

/*
 * The runtime's weak reference layout: one pointer, the referent.
 *
 * A frontend that wants weak references allocates this type and sets
 * CVM_GC_WEAK_REF in the header. The runtime owns the descriptor because
 * the nullify phase has to know where the referent is.
 */
typedef struct CvmWeakRef {
    opaque_mut target;
} CvmWeakRef;

extern CvmTypeDescriptor const cvm_type_weak_ref;

/*
 * Clear the referent of every weak reference whose target did not
 * survive marking.
 *
 * Must run after marking and before sweeping: once sweeping starts, a
 * dead target's metadata is gone and the question can no longer be
 * answered.
 *
 * Returns the number of references cleared, which is what a test and a
 * caller interested in "did anything actually die" both want to know.
 */
/* ---- runtime-internal, not for frontends or FFI glue ---- */

u32 cvm_weak_nullify(CvmHeap* heap);

/*
 * Whether the nullify phase has nothing left to do. True when every
 * packet is back on the empty list.
 */
bool cvm_weak_is_done(CvmHeap* heap);

/*
 * Allocate a weak reference whose referent is `target`.
 *
 * Convenience for callers and tests: it is an ordinary allocation of the
 * built-in type with the flag set.
 */
opaque_mut cvm_weak_ref_new(opaque_mut target);

/* The referent, or NULL if it has been cleared or was never set. */
opaque_mut cvm_weak_ref_target(opaque_mut weak);

/* Set the referent. */
void cvm_weak_ref_set_target(opaque_mut weak, opaque_mut target);

#endif /* CVM_GC_WEAK_H */
