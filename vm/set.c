/*
 * vm/set.c
 *
 * See include/cvm/set.h for what this is: the map with the values left out,
 * and the set operations built on the cursor and `has`.
 */

#include "cvm/set.h"

#include "base/types.h"
#include "cvm/hash.h"

/* The set's key operations are the map's — the map decides what the keys are,
 * and a set does not change that. */

CvmHashSet* cvm_set_new(void) {
    /* A map with no value array: the value pointer starts NULL, and every path
     * that would create or touch it checks for NULL first. */
    CvmHashSet* s = cvm_map_new();
    s->values = NULL; /* it already is, from the zeroing allocation */
    return s;
}

CvmHashSet* cvm_set_new_with(CvmMapOps const* ops, u32 seed) {
    CvmHashSet* s = cvm_map_new_with(ops, seed);
    return s;
}

bool cvm_set_add(CvmHashSet* s, opaque_mut key) {
    bool absent = false;
    (void)cvm_map_put(s, key, NULL, &absent);
    return absent;
}

bool cvm_set_has(CvmHashSet const* s, opaque_mut key) {
    return cvm_map_has((CvmHashMap*)s, key);
}

bool cvm_set_del(CvmHashSet* s, opaque_mut key) {
    return cvm_map_del(s, key);
}

/* ------------------------------------------------------------------ */
/* set operations                                                      */
/* ------------------------------------------------------------------ */

/*
 * Each operation is the same shape: walk one set with the cursor, ask the
 * other, and put the answer in a new set. The cursor walks occupied slots, and
 * `has` answers for the other set, so neither operation touches a value array.
 *
 * The result is a new set, not a mutation of either input: a set operation is
 * a question about two sets, and the answer is a third.
 */

CvmHashSet* cvm_set_union(CvmHashSet const* a, CvmHashSet const* b) {
    CvmHashSet* out = cvm_set_new();

    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = out;

    for (usize slot = cvm_map_next((CvmHashMap*)a, 0); slot != CVM_MAP_NO_SLOT;
         slot = cvm_map_next((CvmHashMap*)a, slot + 1u)) {
        (void)cvm_set_add((CvmHashSet*)f_roots[0],
                          cvm_map_key_at((CvmHashMap*)a, slot));
    }
    for (usize slot = cvm_map_next((CvmHashMap*)b, 0); slot != CVM_MAP_NO_SLOT;
         slot = cvm_map_next((CvmHashMap*)b, slot + 1u)) {
        (void)cvm_set_add((CvmHashSet*)f_roots[0],
                          cvm_map_key_at((CvmHashMap*)b, slot));
    }

    cvm_frame_pop(&f_frame);
    return out;
}

CvmHashSet* cvm_set_inter(CvmHashSet const* a, CvmHashSet const* b) {
    CvmHashSet* out = cvm_set_new();

    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = out;

    for (usize slot = cvm_map_next((CvmHashMap*)a, 0); slot != CVM_MAP_NO_SLOT;
         slot = cvm_map_next((CvmHashMap*)a, slot + 1u)) {
        opaque_mut key = cvm_map_key_at((CvmHashMap*)a, slot);
        if (cvm_set_has(b, key)) {
            (void)cvm_set_add((CvmHashSet*)f_roots[0], key);
        }
    }

    cvm_frame_pop(&f_frame);
    return out;
}

CvmHashSet* cvm_set_sub(CvmHashSet const* a, CvmHashSet const* b) {
    CvmHashSet* out = cvm_set_new();

    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = out;

    for (usize slot = cvm_map_next((CvmHashMap*)a, 0); slot != CVM_MAP_NO_SLOT;
         slot = cvm_map_next((CvmHashMap*)a, slot + 1u)) {
        opaque_mut key = cvm_map_key_at((CvmHashMap*)a, slot);
        if (!cvm_set_has(b, key)) {
            (void)cvm_set_add((CvmHashSet*)f_roots[0], key);
        }
    }

    cvm_frame_pop(&f_frame);
    return out;
}
