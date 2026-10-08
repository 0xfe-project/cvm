/*
 * vm/intern.c
 *
 * The intern table. See include/cvm/intern.h for what it is for, for the
 * difference from quickjs's atom table (which never releases anything), and for
 * the four rules that keep a weak-keyed table from leaking.
 *
 * There is no table object here: an intern table *is* a hash map with a
 * particular set of key operations, and inventing a wrapper type would only add
 * a level of indirection between a caller and the operations it needs to see.
 * The map's usual API still works on it — a caller that wants to walk it uses
 * the cursor — which is the point of the kernel being parameterised by its keys
 * rather than by its contents.
 */

#include "cvm/intern.h"

#include "base/assert.h"
#include "base/types.h"
#include "cvm/hash.h"
#include "cvm/str.h"
#include "cvm/weak.h"

/* ------------------------------------------------------------------ */
/* keys that are held weakly                                           */
/* ------------------------------------------------------------------ */

/*
 * A bucket holds a CvmWeakRef whose referent is the interned string, and the
 * value is unused (NULL).
 *
 * The wrapper is the whole mechanism: it is a strong reference to an object that
 * holds a weak reference, so the bucket array's tracing keeps the wrapper and
 * the wrapper does not keep the string.
 */

/* A lookup arrives holding the string itself. */
static u32 weak_key_hash(opaque_mut key) {
    return cvm_string_hash((CvmString*)key);
}

/*
 * A bucket holds the wrapper. When its referent has died the wrapper is still
 * there — its target is NULL — and the entry is a candidate for pruning. Hashing
 * it cannot use the dead target, and gives a value that only has to be harmless:
 * a dead entry is never matched, and the one place a hash of a stored key is
 * used is the backward shift while deleting, which only needs *some* bucket to
 * decide whether an entry has to move. Any value is correct there, because a
 * dead entry never holds a probe chain's continuation.
 */
static u32 weak_key_hash_stored(opaque_mut stored) {
    CvmString* target = (CvmString*)cvm_weak_ref_target(stored);
    return target == NULL ? 0u : cvm_string_hash(target);
}

static bool weak_key_matches(opaque_mut stored, opaque_mut key) {
    CvmString* target = (CvmString*)cvm_weak_ref_target(stored);
    return target != NULL && cvm_string_equal(target, (CvmString const*)key);
}

/*
 * Whether the entry still stands for anything. This is the predicate that makes
 * an entry removable: once the referent is gone, the text is gone with it, so
 * nothing can match it and nothing can say what it was.
 */
static bool weak_key_valid(opaque_mut stored) {
    return cvm_weak_ref_target(stored) != NULL;
}

/*
 * Wrapping the key allocates, which is fine: the map roots the key across it,
 * and the argument is a caller's string that the caller is holding in any case.
 */
static opaque_mut weak_key_store(opaque_mut key) {
    return cvm_weak_ref_new(key);
}

static CvmMapOps const weak_string_ops = {
    .hash = weak_key_hash,
    .hash_stored = weak_key_hash_stored,
    .matches = weak_key_matches,
    .valid = weak_key_valid,
    .store = weak_key_store,
};

/* ------------------------------------------------------------------ */
/* the table                                                           */
/* ------------------------------------------------------------------ */

CvmHashMap* cvm_intern_new(void) {
    /* A salt of its own, so that an intern table and a map built over the same
     * strings do not lay out identically — not a correctness property, just one
     * less coincidence to debug. */
    return cvm_map_new_with(&weak_string_ops, 0x9e3779b9u);
}

CvmString* cvm_intern_lookup(CvmHashMap* t, CvmString* text) {
    cvm_assert(text != NULL);

    usize slot = cvm_map_slot(t, text);
    if (slot == CVM_MAP_NO_SLOT) {
        return NULL;
    }
    /*
     * The canonical string is reachable only through the wrapper in the bucket,
     * which is exactly why cvm_map_slot is exposed: the map's value is NULL by
     * design here, so asking for it would answer the wrong question.
     */
    return (CvmString*)cvm_weak_ref_target(cvm_map_key_at(t, slot));
}

CvmString* cvm_intern(CvmHashMap* t, CvmString* text) {
    cvm_assert(text != NULL);

    usize slot = cvm_map_slot(t, text);
    if (slot != CVM_MAP_NO_SLOT) {
        CvmString* canonical = (CvmString*)cvm_weak_ref_target(cvm_map_key_at(t, slot));
        if (canonical != NULL) {
            return canonical;
        }
        /*
         * The entry is there but its string is not: it died since the last
         * prune and this is a new string with the same text. Replace the entry
         * rather than inserting a second one for the same text — the bucket is
         * occupied, so the probe would have to continue past it anyway, and two
         * entries for one text is the one thing an intern table must not have.
         */
        cvm_map_del_slot(t, slot);
    }

    bool absent = false;
    (void)cvm_map_put(t, text, NULL, &absent);
    return text;
}

usize cvm_intern_prune(CvmHashMap* t) {
    /*
     * One call, because the table has to be rebuilt either way: see the note in
     * the header. The dead entries are dropped, the survivors are re-inserted,
     * and the arrays come back sized for them.
     */
    return cvm_map_compact(t);
}

usize cvm_intern_count(CvmHashMap* t) {
    return cvm_map_count(t);
}
