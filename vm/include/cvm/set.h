/*
 * vm/include/cvm/set.h
 *
 * A set: the map's kernel with the values left out.
 *
 * A set is a map whose values are the question rather than the answer — what
 * matters is "is this key here", not "what goes with it" — and the map's kernel
 * already answers that: the keys, the occupancy bitmap, and the probing are the
 * whole of it. The only thing a map has that a set does not is the value array,
 * and that is one pointer per bucket of memory a set would allocate and never
 * read.
 *
 * So this is not a new type. It is the map, with the value array never created,
 * and the operations that read and write values skipping the array that is not
 * there. The typedef is for the reader: a variable called CvmHashSet says what
 * it holds in a way that CvmHashMap with a null value array does not.
 *
 * The set operations — union, intersection, difference — are built on the cursor
 * and `has`, not on a new kernel, because two sets' union is "everything in
 * either" and the cursor walks one of them while `has` asks the other.
 */

#ifndef CVM_SET_H
#define CVM_SET_H

#include "base/types.h"
#include "cvm/hash.h"

/* Not a new type: the map is the set, with the value array absent. */
typedef CvmHashMap CvmHashSet;

/* A set of strings, keyed by content. */
CvmHashSet* cvm_set_new(void);

/* A set with whichever key operations the caller provides. */
CvmHashSet* cvm_set_new_with(CvmMapOps const* ops, u32 seed);

/*
 * Put `key` in. True when it was not there — the difference between "you just
 * added it" and "it was already here" — because a set's add is idempotent and
 * the answer is the only thing it can tell the caller.
 */
bool cvm_set_add(CvmHashSet* s, opaque_mut key);

bool cvm_set_has(CvmHashSet const* s, opaque_mut key);

/* Take `key` out. False if it was not there. */
bool cvm_set_del(CvmHashSet* s, opaque_mut key);

CVM_INLINE usize cvm_set_count(CvmHashSet const* s) {
    return cvm_map_count(s);
}

CVM_INLINE void cvm_set_clear(CvmHashSet* s) {
    cvm_map_clear(s);
}

/* ------------------------------------------------------------------ */
/* set operations                                                      */
/* ------------------------------------------------------------------ */

/* Everything in either: a new set holding the keys of `a` and `b`. */
CvmHashSet* cvm_set_union(CvmHashSet const* a, CvmHashSet const* b);

/* The keys that are in both. */
CvmHashSet* cvm_set_inter(CvmHashSet const* a, CvmHashSet const* b);

/* The keys of `a` that are not in `b`. */
CvmHashSet* cvm_set_sub(CvmHashSet const* a, CvmHashSet const* b);

#endif /* CVM_SET_H */
