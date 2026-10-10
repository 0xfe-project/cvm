/*
 * vm/array.c
 *
 * The GC reference array. See include/cvm/array.h for the shape, for why
 * growth is a new array rather than a longer one, and for the one rule that
 * puts on callers.
 */

#include "cvm/array.h"

#include "base/assert.h"
#include "base/types.h"

/* ------------------------------------------------------------------ */
/* the descriptor                                                      */
/* ------------------------------------------------------------------ */

/*
 * There is no element descriptor: a pointer array's stride is the word size,
 * and the kind says so. That is the difference between this and a string, which
 * needs an element size to know whether its units are one byte or two.
 */
static CvmTypeDescriptor const cvm_type_ref_array = {
    .size = offsetof(CvmRefArray, items),
    .kind = CVM_TYPE_PTR_ARRAY,
    .array_length_offset = offsetof(CvmRefArray, capacity),
    .array_data_offset = offsetof(CvmRefArray, items),
};

/* ------------------------------------------------------------------ */
/* construction                                                        */
/* ------------------------------------------------------------------ */

CvmRefArray* cvm_ref_array_new(usize capacity) {
    CvmRefArray* a = cvm_alloc_array(&cvm_type_ref_array, capacity, CVM_WORD_SIZE);

    /*
     * Every slot starts NULL, which the allocator already guaranteed by zeroing
     * the object. Nothing to write, and writing it anyway would be the kind of
     * belt-and-braces that stops being true when the allocator changes.
     */
    a->used = 0;
    return a;
}

/* ------------------------------------------------------------------ */
/* growth                                                              */
/* ------------------------------------------------------------------ */

/*
 * The capacity to grow to.
 *
 * 1.5x rather than 2x. The textbook argument is that the old array can be
 * reused for a later allocation; here the reason is that this collector
 * allocates by size class, and a doubling sequence asks for a brand new size
 * every time while a 1.5x sequence keeps returning to sizes whose free blocks
 * are already lying around after a sweep.
 *
 * The floors matter more than the ratio: from a capacity of one, 1.5x is one,
 * and the array would never grow at all. So the result is at least
 * `capacity + 1`, and at least 4 so that a young array is not reallocated on
 * every other append.
 */
CVM_INLINE usize grown_capacity(usize capacity) {
    usize next = capacity + capacity / 2u;
    if (next < capacity + 1u) {
        next = capacity + 1u;
    }
    if (next < 4u) {
        next = 4u;
    }
    return next;
}

CvmRefArray* cvm_ref_array_push(CvmRefArray* a, opaque_mut item) {
    if (a->used < a->capacity) {
        /* A slot of a live array, so the store goes through the barrier. */
        cvm_store_ptr(&a->items[a->used], item);
        a->used++;
        return a;
    }

    /*
     * The allocation below can collect, so the array being copied out of and
     * the item going into the new one are both held in a frame: the collector
     * does not scan the machine stack, and a local variable is invisible to it.
     *
     * The item is rooted even though it is usually a caller's value that the
     * caller has rooted too. Cheap insurance against the case where it is a
     * temporary, and the difference between the two cases is not something this
     * function can see.
     */
    CVM_FRAME_DECL(f, 2);
    cvm_frame_push(&f_frame, f_roots, 2);
    f_roots[0] = a;
    f_roots[1] = item;

    usize capacity = grown_capacity(a->capacity);
    CvmRefArray* bigger = cvm_ref_array_new(capacity);

    /* Both arrays are rooted now; copy the live prefix across. */
    CvmRefArray* from = (CvmRefArray*)f_roots[0];
    /*
     * The copies below are the exemption in the barrier's contract, and this
     * is a real instance of it rather than a hypothetical one: `bigger` is
     * fresh, so nothing can reach it while it is being filled, so the marker
     * cannot see it and there is nothing for it to miss. The objects copied
     * into it stay reachable from `from`, which is in a frame slot. The store
     * that publishes the array is the caller's, when it puts the result
     * somewhere live.
     */
    for (usize i = 0; i < from->used; i++) {
        bigger->items[i] = from->items[i];
    }
    bigger->used = from->used;
    bigger->items[bigger->used++] = f_roots[1];

    cvm_frame_pop(&f_frame);
    return bigger;
}
