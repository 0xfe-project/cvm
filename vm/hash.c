/*
 * vm/hash.c
 *
 * The hash table. See include/cvm/hash.h for the algorithm's provenance, for
 * the two places a collector forces a change from khashl, and for the hash
 * convention the whole runtime shares.
 *
 * The short version of the second half: a bucket array is traced in full, so a
 * slot that is no longer occupied must hold NULL and not a stale key. Every
 * vacating path in this file — delete, clear, rehash — writes NULL for that
 * reason, and for an intern table it is the difference between working and
 * never releasing anything.
 */

#include "cvm/hash.h"

#include <stdatomic.h>

#include "base/assert.h"
#include "base/types.h"
#include "cvm/array.h"
#include "cvm/gc.h"
#include "cvm/str.h"

/* ------------------------------------------------------------------ */
/* the occupancy bitmap                                                */
/* ------------------------------------------------------------------ */

/*
 * One bit per bucket, packed into 32-bit words.
 *
 * A leaf array of scalars: the collector reads the word count to find the end
 * of the object and looks no further, which is right because there is nothing
 * in here that could be a pointer. The word is 32 bits rather than a byte
 * because a table's occupancy is read and written in single words by the probe
 * loop, and because a bitmap that has to be loaded atomically — which is where
 * a concurrent reader would start — wants a machine word.
 */
typedef struct CvmMapBits {
    usize words;
    u32 bits[];
} CvmMapBits;

static CvmTypeDescriptor const cvm_type_bit_word = {
    .size = 4,
    .kind = CVM_TYPE_SCALAR,
};

static CvmTypeDescriptor const cvm_type_bits = {
    .size = offsetof(CvmMapBits, bits),
    .kind = CVM_TYPE_SCALAR_ARRAY,
    .array_length_offset = offsetof(CvmMapBits, words),
    .array_data_offset = offsetof(CvmMapBits, bits),
    .array_elem_type = &cvm_type_bit_word,
};

/* Words needed for `buckets` bits. khashl's minimum of one word keeps a tiny
 * table's bitmap a single allocation. */
CVM_INLINE usize bits_words(usize buckets) {
    return buckets < 32u ? 1u : buckets >> 5;
}

CVM_INLINE u32 const* bit_words(CvmHashMap const* m) {
    return ((CvmMapBits const*)m->used)->bits;
}

CVM_INLINE u32* bit_words_mut(CvmHashMap* m) {
    return ((CvmMapBits*)m->used)->bits;
}

CVM_INLINE bool bit_used(u32 const* words, usize i) {
    return ((words[i >> 5] >> (i & 31u)) & 1u) != 0u;
}

CVM_INLINE void bit_set(u32* words, usize i) {
    words[i >> 5] |= 1u << (i & 31u);
}

CVM_INLINE void bit_clear(u32* words, usize i) {
    words[i >> 5] &= ~(1u << (i & 31u));
}

/* ------------------------------------------------------------------ */
/* the map                                                             */
/* ------------------------------------------------------------------ */

static CvmPtrField const map_fields[] = {
    {.offset = offsetof(CvmHashMap, keys)},
    {.offset = offsetof(CvmHashMap, values)},
    {.offset = offsetof(CvmHashMap, used)},
};

/*
 * Three pointer fields and no more: the operations table points outside the
 * heap (a static const), so a descriptor that traced it would be describing a
 * field the collector has no business following. The scalars after the pointers
 * are not pointers either.
 */
static CvmTypeDescriptor const cvm_type_map = {
    .size = sizeof(CvmHashMap),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 3,
    .num_embed_fields = 0,
    .ptr_fields = map_fields,
    .embed_fields = NULL,
};

/*
 * The load factor: 75%, as khashl's default. Above this a probe chain gets long
 * enough to matter, and below it the table wastes buckets — which for us are
 * GC slots, so they are worth not wasting, but not at the cost of a longer
 * chain on every lookup.
 */
CVM_INLINE usize max_count(usize buckets) {
    return (buckets >> 1) + (buckets >> 2);
}

CVM_INLINE usize map_buckets_of(u32 bits) {
    return (usize)1u << bits;
}

/* khashl's mapping: Fibonacci hashing on the mixed hash, taking the top bits so
 * that a hash function with a weak low half still distributes. */
CVM_INLINE usize map_bucket(u32 hash, u32 salt, u32 bits) {
    cvm_assert(bits >= 2u && bits < 32u);
    return (usize)(((hash ^ salt) * 2654435769u) >> (32u - bits));
}

/*
 * A per-table salt, from a counter rather than from a random source: it only
 * has to differ between tables, and a reproducible value is worth more here
 * than an unpredictable one. A table that wants a specific salt — a test
 * predicting a bucket index — passes one.
 */
static u32 next_salt(void) {
    static _Atomic u32 counter = 1u;
    u32 n = atomic_fetch_add_explicit(&counter, 0x9e3779b9u, memory_order_relaxed);
    /* splitmix32's finalizer, so that consecutive salts are not consecutive. */
    n ^= n >> 16;
    n *= 0x21f0aaadu;
    n ^= n >> 15;
    n *= 0x735a2d97u;
    return n ^ (n >> 15);
}

/* ------------------------------------------------------------------ */
/* string keys                                                         */
/* ------------------------------------------------------------------ */

/*
 * The string table's operations.
 *
 * Content hash and content equality, and for a table that stores its keys
 * directly the two hashing entry points are one function.
 */
static u32 string_key_hash(opaque_mut key) {
    return cvm_string_hash((CvmString*)key);
}

static bool string_key_matches(opaque_mut stored_key, opaque_mut key) {
    return cvm_string_equal((CvmString const*)stored_key, (CvmString const*)key);
}

static opaque_mut string_key_store(opaque_mut key) {
    return key;
}

static bool string_key_valid(opaque_mut stored_key) {
    (void)stored_key;
    return true; /* a string table's entries live as long as the table does */
}

static CvmMapOps const string_key_ops = {
    .hash = string_key_hash,
    .hash_stored = string_key_hash,
    .matches = string_key_matches,
    .valid = string_key_valid,
    .store = string_key_store,
};

static CvmMapOps const* ops_for_strings(void) {
    return &string_key_ops;
}

CvmHashMap* cvm_map_new(void) {
    return cvm_map_new_with(ops_for_strings(), next_salt());
}

/* ------------------------------------------------------------------ */
/* construction                                                        */
/* ------------------------------------------------------------------ */

static CvmHashMap* map_alloc(CvmMapOps const* ops, u32 salt) {
    CvmHashMap* m = cvm_alloc(&cvm_type_map);
    m->ops = ops;
    m->salt = salt;
    /* count, bits and the arrays are zero, which is the empty table: no
     * buckets, nothing to trace. */
    return m;
}

CvmHashMap* cvm_map_new_with(CvmMapOps const* ops, u32 seed) {
    cvm_assert(ops != NULL);
    return map_alloc(ops, seed);
}

CvmHashMap* cvm_map_new_seeded(u32 seed) {
    return map_alloc(ops_for_strings(), seed);
}

/* ------------------------------------------------------------------ */
/* probing                                                             */
/* ------------------------------------------------------------------ */

/*
 * The bucket holding `key`, or CVM_MAP_NO_SLOT.
 *
 * Linear probing from the Fibonacci landing spot, stopping at the first unused
 * bucket. That is correct — the table is never left with a hole in a chain,
 * because deletion closes the chain rather than leaving a tombstone, and a
 * rehash re-inserts everything.
 */
static usize map_find(CvmHashMap const* m, u32 hash, opaque_mut key) {
    usize buckets = cvm_map_buckets(m);
    if (buckets == 0u) {
        return CVM_MAP_NO_SLOT;
    }

    u32 const* used = bit_words(m);
    usize mask = buckets - 1u;
    usize i = map_bucket(hash, m->salt, m->bits);

    while (bit_used(used, i)) {
        if (m->ops->matches(m->keys->items[i], key)) {
            return i;
        }
        i = (i + 1u) & mask;
    }
    return CVM_MAP_NO_SLOT;
}

/*
 * The bucket holding `key`, or the bucket a new key would go in. The two
 * questions are one probe, which is what makes an insert one pass rather than
 * two.
 */
static usize map_slot_for(CvmHashMap const* m, u32 hash, opaque_mut key, bool* found) {
    u32 const* used = bit_words(m);
    usize mask = cvm_map_buckets(m) - 1u;
    usize i = map_bucket(hash, m->salt, m->bits);

    while (bit_used(used, i)) {
        if (m->ops->matches(m->keys->items[i], key)) {
            *found = true;
            return i;
        }
        i = (i + 1u) & mask;
    }
    *found = false;
    return i;
}

/* ------------------------------------------------------------------ */
/* resize                                                              */
/* ------------------------------------------------------------------ */

/*
 * Rehash into `buckets` buckets.
 *
 * The vectors are allocated before anything is moved, and both the map and the
 * two new arrays are rooted in a frame across the allocation: the collector
 * does not scan the machine stack, so a local pointer held over an allocation
 * is an object that can be freed and then written into.
 *
 * This is the deviation from khashl worth naming. Its resize reuses the old key
 * array as scratch and moves entries with a kick-out dance, which exists to
 * avoid allocating a second array. Ours has to be a GC object either way, so
 * the dance would buy nothing: scan the old buckets, re-insert into the new
 * ones, drop the old.
 */
static bool map_rehash(CvmHashMap* m, usize buckets) {
    if (buckets < 4u) {
        buckets = 4u;
    }
    /* Round up to a power of two, which the masking arithmetic requires. */
    u32 bits = 2u;
    while ((map_buckets_of(bits) < buckets) && (bits < 31u)) {
        bits++;
    }
    buckets = map_buckets_of(bits);

    if (bits == m->bits && m->keys != NULL) {
        return true;
    }
    if (m->count > max_count(buckets)) {
        /* Too small for what is in it. Leave the table as it was. */
        return false;
    }

    CVM_FRAME_DECL(f, 3);
    cvm_frame_push(&f_frame, f_roots, 3);
    f_roots[0] = m;

    CvmRefArray* keys = cvm_ref_array_new(buckets);
    CvmMapBits* used = cvm_alloc_array(&cvm_type_bits, bits_words(buckets), 4u);
    CvmRefArray* values = cvm_ref_array_new(buckets);
    f_roots[1] = keys;
    f_roots[2] = values;

    /*
     * The bucket array is fully initialised for a table: every slot is either a
     * key or NULL, so `used` is the whole capacity. That is what the array's
     * length means, and it keeps the invariant "slots below `used` are the
     * meaningful ones" true for readers of the array itself.
     */
    keys->used = buckets;
    values->used = buckets;

    /* The map does not move, so `m` stays the right pointer; the frame slot is
     * what keeps it alive across the allocation above. */
    usize old_buckets = cvm_map_buckets(m);
    if (old_buckets != 0u) {
        u32 const* old_used = bit_words(m);

        for (usize i = 0; i < old_buckets; i++) {
            if (!bit_used(old_used, i)) {
                continue;
            }
            opaque_mut key = m->keys->items[i];
            u32 hash = m->ops->hash_stored(key);

            usize slot = map_bucket(hash, m->salt, bits);
            while (bit_used(used->bits, slot)) {
                slot = (slot + 1u) & (buckets - 1u);
            }
            bit_set(used->bits, slot);
            keys->items[slot] = key;
            values->items[slot] = m->values->items[i];
        }
    }

    m->keys = keys;
    m->values = values;
    m->used = used;
    m->bits = bits;

    cvm_frame_pop(&f_frame);
    return true;
}

bool cvm_map_resize(CvmHashMap* m, usize buckets) {
    return map_rehash(m, buckets);
}

/* ------------------------------------------------------------------ */
/* compaction                                                          */
/* ------------------------------------------------------------------ */

/* The smallest power of two whose load factor holds `live` entries. */
static u32 bits_for(usize live) {
    u32 bits = 2u;
    while (bits < 31u && max_count(map_buckets_of(bits)) < live) {
        bits++;
    }
    return bits;
}

usize cvm_map_compact(CvmHashMap* m) {
    usize buckets = cvm_map_buckets(m);
    if (buckets == 0u) {
        return 0;
    }

    u32 const* old_used = bit_words(m);
    usize occupied = 0;
    usize live = 0;
    for (usize i = 0; i < buckets; i++) {
        if (!bit_used(old_used, i)) {
            continue;
        }
        occupied++;
        if (m->ops->valid(m->keys->items[i])) {
            live++;
        }
    }

    u32 new_bits = bits_for(live);
    if (live == occupied && new_bits == m->bits) {
        /* Nothing to drop and nothing to give back. Not even an allocation. */
        return 0;
    }

    CVM_FRAME_DECL(f, 3);
    cvm_frame_push(&f_frame, f_roots, 3);
    f_roots[0] = m;

    if (live == 0u) {
        /* Everything died: the table goes back to being a fresh one, arrays and
         * all, rather than keeping a large empty object alive. */
        m->keys = NULL;
        m->values = NULL;
        m->used = NULL;
        m->bits = 0;
        m->count = 0;
        cvm_frame_pop(&f_frame);
        return occupied;
    }

    usize new_buckets = map_buckets_of(new_bits);
    CvmRefArray* keys = cvm_ref_array_new(new_buckets);
    CvmRefArray* values = cvm_ref_array_new(new_buckets);
    CvmMapBits* used = cvm_alloc_array(&cvm_type_bits, bits_words(new_buckets), 4u);
    f_roots[1] = keys;
    f_roots[2] = values;

    keys->used = new_buckets;
    values->used = new_buckets;

    /*
     * Re-insert from the old arrays, which still hold everything and are still
     * what the map points at: every key stays reachable for the whole of this,
     * including across the allocations above.
     */
    usize mask = new_buckets - 1u;
    for (usize i = 0; i < buckets; i++) {
        if (!bit_used(old_used, i)) {
            continue;
        }
        opaque_mut key = m->keys->items[i];
        if (!m->ops->valid(key)) {
            continue; /* dropped */
        }

        usize slot = map_bucket(m->ops->hash_stored(key), m->salt, new_bits);
        while (bit_used(used->bits, slot)) {
            slot = (slot + 1u) & mask;
        }
        bit_set(used->bits, slot);
        keys->items[slot] = key;
        values->items[slot] = m->values->items[i];
    }

    m->keys = keys;
    m->values = values;
    m->used = used;
    m->bits = new_bits;
    m->count = live; /* authoritative, whatever the old count had drifted to */

    cvm_frame_pop(&f_frame);
    return occupied - live;
}

/* ------------------------------------------------------------------ */
/* lookup and update                                                   */
/* ------------------------------------------------------------------ */

opaque_mut cvm_map_get(CvmHashMap const* m, opaque_mut key) {
    cvm_assert(key != NULL);

    usize slot = map_find(m, m->ops->hash(key), key);
    return slot == CVM_MAP_NO_SLOT ? NULL : m->values->items[slot];
}

bool cvm_map_has(CvmHashMap const* m, opaque_mut key) {
    cvm_assert(key != NULL);
    return map_find(m, m->ops->hash(key), key) != CVM_MAP_NO_SLOT;
}

opaque_mut cvm_map_put(CvmHashMap* m, opaque_mut key, opaque_mut value, bool* absent) {
    cvm_assert(key != NULL);

    /*
     * The map, the key and the value are rooted across the whole call: growing
     * allocates, and `store` may allocate as well (a weak-keyed table wraps its
     * keys). Rooting a value that is not a heap pointer is harmless — the
     * marker checks containment before it does anything with a field.
     */
    CVM_FRAME_DECL(f, 3);
    cvm_frame_push(&f_frame, f_roots, 3);
    f_roots[0] = m;
    f_roots[1] = key;
    f_roots[2] = value;

    CvmHashMap* map = (CvmHashMap*)f_roots[0];
    u32 hash = map->ops->hash(f_roots[1]);

    if (map->count + 1u > max_count(cvm_map_buckets(map))) {
        usize wanted = cvm_map_buckets(map) == 0u ? 8u : cvm_map_buckets(map) * 2u;
        if (!map_rehash(map, wanted)) {
            cvm_frame_pop(&f_frame);
            return NULL;
        }
    }

    bool found = false;
    usize slot = map_slot_for(map, hash, f_roots[1], &found);

    if (found) {
        opaque_mut previous = map->values->items[slot];
        map->values->items[slot] = f_roots[2];
        if (absent != NULL) {
            *absent = false;
        }
        cvm_frame_pop(&f_frame);
        return previous;
    }

    map->keys->items[slot] = map->ops->store(f_roots[1]);
    map->values->items[slot] = f_roots[2];
    bit_set(bit_words_mut(map), slot);
    map->count++;

    if (absent != NULL) {
        *absent = true;
    }
    cvm_frame_pop(&f_frame);
    return f_roots[2];
}

/* ------------------------------------------------------------------ */
/* removal                                                             */
/* ------------------------------------------------------------------ */

/*
 * Empty the bucket at `i`, closing the probe chain behind it.
 *
 * The hole has to be closed rather than marked: a tombstone would answer "not
 * here" correctly but leave the bucket looking occupied forever, so a table
 * whose entries come and go fills up with tombstones and can only be repaired
 * by a rehash. This is khashl's backward shift, and the condition is the usual
 * one for "this entry's probe chain has to pass through the hole being closed",
 * in the two cases of whether the chain wraps past the end of the array.
 *
 * Two things are ours and not khashl's: the last vacated bucket gets NULL in
 * both arrays, because the collector traces every slot and a stale key there is
 * a reference that never dies; and the values move with the keys, or the two
 * arrays would disagree about which value belongs to which key.
 */
static void map_vacate(CvmHashMap* m, usize i) {
    u32* used = bit_words_mut(m);
    usize mask = cvm_map_buckets(m) - 1u;
    usize j = i;

    for (;;) {
        j = (j + 1u) & mask;
        /* j == i only when the table is completely full, which the load factor
         * prevents; the check is here so that a corrupted table cannot spin. */
        if (j == i || !bit_used(used, j)) {
            break;
        }

        usize home =
            map_bucket(m->ops->hash_stored(m->keys->items[j]), m->salt, m->bits);
        bool movable = (j > i) ? (home <= i || home > j) : (home <= i && home > j);
        if (movable) {
            m->keys->items[i] = m->keys->items[j];
            m->values->items[i] = m->values->items[j];
            i = j;
        }
    }

    bit_clear(used, i);
    m->keys->items[i] = NULL;
    m->values->items[i] = NULL;
    m->count--;
}

usize cvm_map_slot(CvmHashMap const* m, opaque_mut key) {
    cvm_assert(key != NULL);
    return map_find(m, m->ops->hash(key), key);
}

void cvm_map_del_slot(CvmHashMap* m, usize slot) {
    cvm_assert(slot < cvm_map_buckets(m));
    cvm_assert(bit_used(bit_words(m), slot));
    map_vacate(m, slot);
}

bool cvm_map_del(CvmHashMap* m, opaque_mut key) {
    cvm_assert(key != NULL);

    usize slot = map_find(m, m->ops->hash(key), key);
    if (slot == CVM_MAP_NO_SLOT) {
        return false;
    }
    map_vacate(m, slot);
    return true;
}

void cvm_map_clear(CvmHashMap* m) {
    usize buckets = cvm_map_buckets(m);
    if (buckets == 0u) {
        return;
    }

    /*
     * Slot by slot rather than by dropping the arrays: the arrays are what the
     * next insert reuses, and a table that is cleared and refilled should not
     * reallocate twice. The memset is over the bitmap, whose words are the
     * whole of what "empty" means.
     */
    for (usize i = 0; i < buckets; i++) {
        m->keys->items[i] = NULL;
        m->values->items[i] = NULL;
    }
    u32* used = bit_words_mut(m);
    for (usize w = 0; w < bits_words(buckets); w++) {
        used[w] = 0u;
    }
    m->count = 0;
}

/* ------------------------------------------------------------------ */
/* traversal                                                           */
/* ------------------------------------------------------------------ */

usize cvm_map_next(CvmHashMap const* m, usize from) {
    usize buckets = cvm_map_buckets(m);
    if (buckets == 0u || from >= buckets) {
        return CVM_MAP_NO_SLOT;
    }

    u32 const* used = bit_words(m);
    for (usize i = from; i < buckets; i++) {
        if (bit_used(used, i)) {
            return i;
        }
    }
    return CVM_MAP_NO_SLOT;
}

opaque_mut cvm_map_key_at(CvmHashMap const* m, usize slot) {
    cvm_assert(slot < cvm_map_buckets(m));
    return m->keys->items[slot];
}

opaque_mut cvm_map_value_at(CvmHashMap const* m, usize slot) {
    cvm_assert(slot < cvm_map_buckets(m));
    return m->values->items[slot];
}
