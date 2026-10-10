/*
 * vm/atom.c
 *
 * The sharded atom table. See include/cvm/atom.h for what it is for and for the
 * argument that a lock-free lookup is safe.
 *
 * There is no new table kernel here. A shard is a whole intern table, so the
 * sharding is a wrapper around cvm/intern.h and nothing else: the kernel is
 * untouched, and everything this file does is decide which shard a string belongs
 * to and hold the right lock while writing to it.
 */

#include "cvm/atom.h"

#include "base/assert.h"
#include "base/types.h"
#include "cvm/array.h"
#include "cvm/gc.h"
#include "cvm/hash.h"
#include "cvm/intern.h"
#include "cvm/mutex.h"
#include "cvm/weak.h"
#include "mem.h"
#include "rt.h"

/* One mutex per shard, in a scalar array: the collector has no pointers to trace
 * in it, and it is a GC object so the memory comes back with the table. */
typedef struct CvmAtomLocks {
    usize count;
    CvmMutex locks[];
} CvmAtomLocks;

static CvmTypeDescriptor const cvm_type_mutex = {
    .size = sizeof(CvmMutex),
    .kind = CVM_TYPE_SCALAR,
};

static CvmTypeDescriptor const cvm_type_atom_locks = {
    .size = offsetof(CvmAtomLocks, locks),
    .kind = CVM_TYPE_SCALAR_ARRAY,
    .array_length_offset = offsetof(CvmAtomLocks, count),
    .array_data_offset = offsetof(CvmAtomLocks, locks),
    .array_elem_type = &cvm_type_mutex,
};

struct CvmAtomTable {
    /* The shards: one intern table each. */
    CvmRefArray* maps;
    /* The shards' mutexes, parallel to `maps` by index. */
    CvmAtomLocks* locks;
    usize shards;
    u32 mask;
};

static CvmPtrField const atom_fields[] = {
    {.offset = offsetof(CvmAtomTable, maps)},
    {.offset = offsetof(CvmAtomTable, locks)},
};

static CvmTypeDescriptor const cvm_type_atom = {
    .size = sizeof(CvmAtomTable),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 2,
    .num_embed_fields = 0,
    .ptr_fields = atom_fields,
    .embed_fields = NULL,
};

/*
 * Which shard a string belongs to.
 *
 * The *low* bits of the content hash, and not the high ones. The bucket inside a
 * shard already comes from the high bits of the multiplied hash, so driving both
 * from the same bits would put every string that shares them in one shard and in
 * one part of that shard's array — which is the correlation the sharding exists to
 * avoid.
 */
static usize shard_of(CvmAtomTable const* t, CvmString* text) {
    return (usize)(cvm_string_hash(text) & t->mask);
}

static CvmHashMap* shard_map(CvmAtomTable const* t, usize shard) {
    return (CvmHashMap*)cvm_ref_array_get(t->maps, shard);
}

static CvmMutex* shard_lock(CvmAtomTable const* t, usize shard) {
    return &t->locks->locks[shard];
}

CvmAtomTable* cvm_atom_table_new(usize shards) {
    cvm_assert(shards > 0u);
    cvm_assert((shards & (shards - 1u)) == 0u);

    CVM_FRAME_DECL(f, 2);
    cvm_frame_push(&f_frame, f_roots, 2);

    CvmAtomTable* t = cvm_alloc(&cvm_type_atom);
    f_roots[0] = t;
    t->shards = shards;
    t->mask = (u32)(shards - 1u);
    t->maps = cvm_ref_array_new(shards);
    f_roots[1] = t->maps;
    t->locks = cvm_alloc_array(&cvm_type_atom_locks, shards, sizeof(CvmMutex));

    for (usize i = 0; i < shards; i++) {
        cvm_ref_array_set(t->maps, i, cvm_intern_new());
        if (!cvm_mutex_init(&t->locks->locks[i])) {
            cvm_frame_pop(&f_frame);
            return NULL;
        }
    }
    cvm_frame_pop(&f_frame);
    return t;
}

CvmString* cvm_atom_lookup(CvmAtomTable const* t, CvmString* text) {
    cvm_assert(text != NULL);
    usize shard = shard_of(t, text);
    CvmHashMap* m = shard_map(t, shard);

    /*
     * Under the shard's lock, and not yet lock-free.
     *
     * A lock-free lookup walks the shard's key array, and a resize or a
     * compaction replaces that array and leaves the old one unreachable. The
     * collector's sweep runs concurrently with mutators, so it can free that
     * array while a reader is still walking it — which is a use-after-free, and
     * it was one: the concurrency test crashed inside a key comparison.
     *
     * The argument for the lock-free read used to be "a lookup allocates nothing,
     * so it contains no safepoint, so no collection can begin inside it". That is
     * true and it is not enough: it keeps a collection from *starting*, and says
     * nothing about a sweep that is already running.
     *
     * Making it safe again needs the replaced arrays to be kept alive until no
     * reader can still be inside them, which is a grace period, and there are two
     * shapes for it: retire the arrays in the map and drop them in the next
     * mutator phase after a collection (every reader has passed a safepoint by
     * then, since the collector waited for it), or take the lock for the lookup
     * and be done. Until one of them is in, the lock is the answer.
     */
    cvm_mutex_lock(shard_lock(t, shard));
    usize slot = cvm_map_slot(m, text);
    CvmString* canonical = NULL;
    if (slot != CVM_MAP_NO_SLOT) {
        canonical = (CvmString*)cvm_weak_ref_target(cvm_map_key_at(m, slot));
    }
    cvm_mutex_unlock(shard_lock(t, shard));
    return canonical;
}

CvmString* cvm_atom_intern(CvmAtomTable* t, CvmString* text) {
    cvm_assert(text != NULL);
    usize shard = shard_of(t, text);
    CvmHashMap* m = shard_map(t, shard);

    /*
     * The lock is what makes this the only place an entry for this text can
     * appear: a lookup that missed a concurrent insert and a lookup that found
     * nothing both end up here, and here they look again.
     */
    cvm_mutex_lock(shard_lock(t, shard));
    CvmString* canonical = cvm_intern(m, text);
    cvm_mutex_unlock(shard_lock(t, shard));
    return canonical;
}

usize cvm_atom_prune(CvmAtomTable* t) {
    usize dropped = 0;
    for (usize i = 0; i < t->shards; i++) {
        cvm_mutex_lock(shard_lock(t, i));
        dropped += cvm_intern_prune(shard_map(t, i));
        cvm_mutex_unlock(shard_lock(t, i));
    }
    return dropped;
}

usize cvm_atom_count(CvmAtomTable* t) {
    usize total = 0;
    for (usize i = 0; i < t->shards; i++) {
        cvm_mutex_lock(shard_lock(t, i));
        total += cvm_map_count(shard_map(t, i));
        cvm_mutex_unlock(shard_lock(t, i));
    }
    return total;
}

usize cvm_atom_shards(CvmAtomTable const* t) {
    return t->shards;
}

usize cvm_atom_buckets(CvmAtomTable* t) {
    usize total = 0;
    for (usize i = 0; i < t->shards; i++) {
        cvm_mutex_lock(shard_lock(t, i));
        total += cvm_map_buckets(shard_map(t, i));
        cvm_mutex_unlock(shard_lock(t, i));
    }
    return total;
}
