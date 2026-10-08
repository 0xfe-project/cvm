/*
 * vm/include/cvm/array.h
 *
 * A fixed-length array of GC references that can be appended to.
 *
 * ---------------------------------------------------------------------
 * What it is, and what it is not
 * ---------------------------------------------------------------------
 *
 * It is an `Object[]` — a contiguous run of reference slots, in the heap, with
 * the slot count in the object where the collector reads it. It is not a
 * list object with a collection's worth of behaviour: there is no insert, no
 * remove, no search, no growth in place. Appending past the end is an
 * allocation of a *new, larger array*, and the caller takes the new pointer.
 *
 * That is the whole design, and it is what keeps this usable as a building
 * block: a hash table's bucket array, a rope's pieces, a JSON array being
 * parsed are all the same thing — a run of slots — and none of them wants a
 * container to have opinions about their contents.
 *
 * ---------------------------------------------------------------------
 * Why appending returns a pointer
 * ---------------------------------------------------------------------
 *
 * The collector never moves an object, so an array cannot grow where it is:
 * the slots after it belong to whatever was allocated next. Growth is
 * therefore a second array, and the old one becomes garbage when the last
 * reference to it goes. The cost of that is one rule at every call site:
 *
 *     arr = cvm_ref_array_push(arr, item);   // take the result
 *
 * A caller that keeps its own copy of the old pointer loses the item, and
 * loses it silently, because the old array is still a perfectly valid array.
 *
 * ---------------------------------------------------------------------
 * Slots
 * ---------------------------------------------------------------------
 *
 * A slot holds a GC pointer or NULL. Nothing else: the collector traces every
 * slot up to the capacity, so a tagged integer or a pointer to something
 * outside the heap would be followed as if it were an object — which is why
 * there is one type here and it says references.
 *
 * NULL means empty, and it is the only empty. An array created with
 * cvm_ref_array_new has all of its slots empty, and appending fills them in
 * order; the slots between `used` and `capacity` are the space a caller is
 * not using yet.
 */

#ifndef CVM_ARRAY_H
#define CVM_ARRAY_H

#include "base/types.h"
#include "cvm/gc.h"

/*
 * Field order is the ABI: `capacity` is what the collector reads, at
 * array_length_offset, to find where the object ends.
 */
typedef struct CvmRefArray {
    /* Slots allocated. Every one of them is traced. */
    usize capacity;
    /* Slots in use, a prefix of the array. */
    usize used;
    /* The slots themselves: GC pointers or NULL. */
    opaque_mut items[];
} CvmRefArray;

_Static_assert(offsetof(CvmRefArray, items) == 16, "slots must stay 8-byte aligned");

/* An array of `capacity` slots, all of them empty. A capacity of zero is
 * legal and is what a caller with nothing to store should pass. */
CvmRefArray* cvm_ref_array_new(usize capacity);

CVM_INLINE usize cvm_ref_array_capacity(CvmRefArray const* a) {
    return a->capacity;
}

CVM_INLINE usize cvm_ref_array_length(CvmRefArray const* a) {
    return a->used;
}

/* A slot, empty or not. `i` may be anywhere in the capacity. */
CVM_INLINE opaque_mut cvm_ref_array_get(CvmRefArray const* a, usize i) {
    cvm_assert(i < a->capacity);
    return a->items[i];
}

/*
 * Set a slot. NULL empties it, which is the only way to remove an item —
 * removing does not shift the ones after it, because the users of this type
 * either track their own occupancy or do not care about order.
 */
CVM_INLINE void cvm_ref_array_set(CvmRefArray* a, usize i, opaque_mut item) {
    cvm_assert(i < a->capacity);
    a->items[i] = item;
}

/*
 * Append `item`, returning the array it is in.
 *
 * When there is room the argument comes back and nothing is allocated. When
 * there is not, a new array of about 1.5x the capacity is allocated, the
 * used slots and the new item are copied into it, and *that* is the result.
 *
 * The caller must take the result: see the note at the top of the header.
 */
CvmRefArray* cvm_ref_array_push(CvmRefArray* a, opaque_mut item);

#endif /* CVM_ARRAY_H */
