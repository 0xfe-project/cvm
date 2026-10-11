/*
 * vm/include/cvm/hash.h
 *
 * A hash table whose bucket array lives in the GC heap.
 *
 * ---------------------------------------------------------------------
 * The algorithm, and where it came from
 * ---------------------------------------------------------------------
 *
 * The kernel is khashl's (Attractive Chaos, MIT, refs/khashl-main): open
 * addressing over a power-of-two bucket count, with
 *
 *   - one *bit* per bucket for occupancy, rather than a byte or a tombstone,
 *     which is both smaller and the thing the collector must not see as a
 *     pointer;
 *
 *   - Fibonacci hashing to map a hash onto a bucket, so that a poor hash
 *     function's high bits decide the bucket and the low bits do not matter;
 *
 *   - backward-shift deletion. A hole left in a probe chain breaks every
 *     lookup that walked past it, and the usual tombstone fixes that by making
 *     the hole look occupied forever. Backward shift instead moves the
 *     following entries back to close the chain, so the table never fills with
 *     garbage that only a rehash can clear.
 *
 * One place we deviate, and why: khashl's resize moves entries between the old
 * and new arrays with a kick-out dance, which exists to avoid allocating a
 * second key array. We allocate a fresh bucket array anyway — it has to be a GC
 * object — so the rehash is a plain scan and re-insert, which is shorter and
 * obviously correct. The kick-out would save an allocation we are already
 * making.
 *
 * What is *not* from khashl, and could not be: khashl never has to think about
 * a collector. Two consequences run through this file:
 *
 *   - a deleted or vacated key slot is set to NULL. The bucket array is traced
 *     in full, so a stale key pointer in it is a reference that keeps a string
 *     alive forever. For an intern table that is not a leak in the table, it is
 *     a leak in the program.
 *
 *   - every allocation can collect, so anything held across one is rooted. See
 *     the note in cvm_map_resize.
 *
 * ---------------------------------------------------------------------
 * Keys, and the hash convention
 * ---------------------------------------------------------------------
 *
 * Which keys a table takes is decided by the operations it is built with:
 *
 *   - a *string* key hashes by content. Two distinct CvmString objects holding
 *     the same text are the same key, which is the point of an intern table.
 *
 *   - every other GC object hashes by identity — its address. It does not move,
 *     so the address is stable for its lifetime.
 *
 *   - an integer or a rune hashes by value.
 *
 * That is the convention the whole runtime shares, and it is why a string's
 * hash is cached in the string: it is a property of the content, computed once,
 * and the same value is what a map and an intern table insert under.
 *
 * A table that hashes by content must also compare by content: the hash decides
 * the bucket, and the comparison decides whether two keys in that bucket are
 * the same key. Getting one of the two right and the other wrong produces a
 * table that is merely wrong in a way that looks like data corruption.
 */

#ifndef CVM_HASH_H
#define CVM_HASH_H

#include <stdatomic.h>

#include "base/types.h"
#include "cvm/array.h"
#include "cvm/gc.h"
#include "cvm/str.h"

/* No bucket. Returned by the cursor when the table has no more entries. */
#define CVM_MAP_NO_SLOT ((usize)~0u)

/*
 * How a table hashes and compares its keys.
 *
 * Two hashing entry points rather than one, because what a lookup arrives with
 * and what a bucket holds are not the same shape in general: a weak-keyed table
 * stores a reference wrapper while lookups arrive holding the string itself.
 * `hash` is for the former and `hash_stored` for the latter, and for a table
 * where the two shapes coincide they are the same function.
 *
 * `matches` takes a stored key and a lookup key for the same reason. The hash
 * decides the bucket and this decides whether the occupant is the key; a table
 * that gets one of the two right and the other wrong is wrong in a way that
 * looks like data corruption rather than a bug in the table.
 *
 * `store` turns a lookup key into what goes into a slot, and is the identity
 * for a table that stores keys directly. A caller-supplied `store` that
 * allocates is safe: the kernel roots the map and the key around it.
 */
typedef struct CvmMapOps {
    u32 (*hash)(opaque_mut key);
    u32 (*hash_stored)(opaque_mut stored_key);
    bool (*matches)(opaque_mut stored_key, opaque_mut key);
    /*
     * Whether a stored key still stands for something.
     *
     * True for a table whose entries live as long as the table does. False for a
     * weak-keyed table once the key has died — its text is gone with it, so
     * nothing can match it and nothing can tell what it was, and the only thing
     * left to do with the entry is remove it.
     */
    bool (*valid)(opaque_mut stored_key);
    opaque_mut (*store)(opaque_mut key);

    /*
     * Whether this table is read without a lock while it is being written.
     *
     * A table that is asks the map to keep its replaced arrays alive for one
     * more collection (cvm/retire.h), because a reader that snapshotted the
     * old arrays is still walking them after the swap. A table that is not --
     * the ordinary case, a table used from one thread or under a lock taken
     * for every access -- pays nothing and reclaims exactly when it did,
     * which is what the tests that clear and prune pin down.
     */
    bool lock_free_readers;
} CvmMapOps;

/*
 * Field order is the ABI: the three arrays are what the collector traces, and
 * the scalars after them are not pointers and are not traced.
 *
 * The scalar fields are not part of any array's extent — they are the map's own
 * header — which is why they sit after the pointers rather than in front of
 * them.
 */
typedef struct CvmHashMap {
    /* Bucket array: one key per bucket, or NULL. */
    CvmRefArray* keys;
    /* Values, parallel to `keys` by bucket index. */
    CvmRefArray* values;
    /* Occupancy: one bit per bucket, packed into words. */
    opaque_mut used;
    /* Static, outside the heap, and deliberately not traced. */
    CvmMapOps const* ops;
    /* Live entries. */
    usize count;
    /* Bucket count is 1 << bits. */
    u32 bits;
    /* Per-table hash salt, so two tables do not collide in lockstep. */
    u32 salt;
} CvmHashMap;

/* ------------------------------------------------------------------ */
/* construction                                                        */
/* ------------------------------------------------------------------ */

/* A table keyed by string content. Empty: no bucket array until the first
 * insert. */
CvmHashMap* cvm_map_new(void);

/*
 * The same, with a chosen salt. Two tables with the same salt and the same keys
 * have the same layout, which is what a test that wants to predict a bucket
 * index needs.
 */
CvmHashMap* cvm_map_new_seeded(u32 seed);

/* A table with whichever key operations are given. The map does not copy them,
 * so `ops` must outlive the map — a static const, in practice. */
CvmHashMap* cvm_map_new_with(CvmMapOps const* ops, u32 seed);

/* ------------------------------------------------------------------ */
/* lookup and update                                                   */
/* ------------------------------------------------------------------ */

/*
 * The value under `key`, or NULL if it is absent. A NULL value cannot be
 * stored, so there is no ambiguity: use cvm_map_has for the question "is this
 * key there".
 */
opaque_mut cvm_map_get(CvmHashMap const* m, opaque_mut key);

bool cvm_map_has(CvmHashMap const* m, opaque_mut key);

/*
 * Insert or replace, returning the value that is now stored (the argument for a
 * new key, the previous value for an existing one).
 *
 * `absent`, when not NULL, is set to true if the key was not there — the
 * question an intern table asks, because it is the difference between "here is
 * the canonical string" and "you just made a new one".
 */
opaque_mut cvm_map_put(CvmHashMap* m, opaque_mut key, opaque_mut value, bool* absent);

/* Remove `key`. False if it was not there. */
bool cvm_map_del(CvmHashMap* m, opaque_mut key);

/*
 * The slot holding `key`, or CVM_MAP_NO_SLOT.
 *
 * For a table that has to look at *what is stored* rather than at a value it put
 * there. An intern table is the case: its entries hold their keys weakly, so the
 * canonical string is reachable only through the stored key's wrapper, and
 * asking for a value would ask the wrong question.
 */
bool cvm_map_find_key(CvmHashMap const* m, opaque_mut key, opaque_mut* stored_out);

usize cvm_map_slot(CvmHashMap const* m, opaque_mut key);

/*
 * Remove the entry in `slot`, which must be occupied.
 *
 * Runtime-internal rather than part of the table's surface: a caller with a slot
 * index got it from a traversal or from cvm_map_slot, and the index stops being
 * meaningful at the next insert or delete. It exists because a caller that has
 * already found what it wants to remove should not have to hash the key again to
 * say which entry it meant.
 */
void cvm_map_del_slot(CvmHashMap* m, usize slot);

/*
 * Drop every entry, keeping the bucket array.
 *
 * The arrays are emptied slot by slot rather than dropped, because they are
 * what the next insert reuses; a caller that wants the memory back calls
 * cvm_map_resize with a smaller bucket count.
 */
void cvm_map_clear(CvmHashMap* m);

CVM_INLINE usize cvm_map_count(CvmHashMap const* m) {
    return m->count;
}

/*
 * Read as the writer published them, because this is reached from a lookup that
 * takes no lock — an assertion inside one, for instance. The two fields are read
 * with acquire so that a reader that sees the new key array also sees the bucket
 * count that goes with it.
 */
CVM_INLINE usize cvm_map_buckets(CvmHashMap const* m) {
    CvmRefArray* keys = (CvmRefArray*)atomic_load_explicit(
        (CvmRefArray* const _Atomic*)&m->keys, memory_order_acquire);
    if (keys == NULL) {
        return 0u;
    }
    return (usize)1u << atomic_load_explicit((u32 const _Atomic*)&m->bits,
                                             memory_order_acquire);
}

/*
 * Change the bucket count, growing or shrinking. Returns false if the table
 * could not hold its entries at that size, in which case it is unchanged.
 *
 * Shrinking matters for a table whose entries come and go: without it, the
 * bucket array only ever grows, and a long-running program's table becomes a
 * large empty object that never comes back. The intern table is the case that
 * makes it necessary rather than nice.
 */
bool cvm_map_resize(CvmHashMap* m, usize buckets);

/*
 * Rebuild the bucket arrays, keeping only entries whose key is still valid, in
 * an array sized for what remains. Returns how many entries were dropped.
 *
 * This is the table's one maintenance operation, and it exists because dropping
 * entries *in place* cannot be made to work. Closing a deleted entry's probe
 * chain moves later entries backwards, and a chain that wraps past the end of
 * the array moves an entry that a downward scan has not reached yet into a
 * bucket it has already passed — so a walk-and-delete leaves dead entries
 * behind, in a pattern that depends on where the chains happen to wrap.
 *
 * Rebuilding has neither problem: the old arrays are untouched while the new
 * ones are filled, so nothing moves under the scan, and the result is both
 * compact in entries and sized to fit them.
 *
 * It allocates when it changes anything, and allocates nothing when it does not.
 * That is what makes it safe to call from the mutator after a collection and
 * unsafe to call from the nullify phase, which is the same sentence as "this is
 * why the cleanup is deferred".
 */
usize cvm_map_compact(CvmHashMap* m);

/* ------------------------------------------------------------------ */
/* traversal                                                           */
/* ------------------------------------------------------------------ */

/*
 * A cursor over occupied buckets: the next occupied bucket at or after `from`,
 * or CVM_MAP_NO_SLOT.
 *
 *     for (usize s = cvm_map_next(m, 0); s != CVM_MAP_NO_SLOT; s = cvm_map_next(m, s + 1))
 *
 * There is no callback form on purpose. A callback is invoked with the table
 * mid-traversal, and if it allocates — which is the first thing anything
 * interesting does — the table can move under it as the collector runs. An
 * index is inert, and a caller that wants to hold onto one can say so: the
 * index stays valid until the next insert or delete, and is invalidated by
 * both, which is the same rule the arrays themselves follow.
 */
usize cvm_map_next(CvmHashMap const* m, usize from);

/* The key stored in `slot`. Read through the map's publication, since a writer
 * may be replacing the array it lives in. */
opaque_mut cvm_map_key_at(CvmHashMap const* m, usize slot);

/* The value stored in `slot`. */
opaque_mut cvm_map_value_at(CvmHashMap const* m, usize slot);

#endif /* CVM_HASH_H */
