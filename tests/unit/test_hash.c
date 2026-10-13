/*
 * tests/unit/test_hash.c
 *
 * The hash table: lookup by content, growth, deletion, traversal, and the two
 * things a collector adds to the usual list.
 *
 * The deletion cases carry the weight here. A table that deletes by leaving a
 * hole passes every test that only inserts, and fails the first lookup that
 * walked past the hole; a table that deletes without clearing the key slot
 * passes every functional test and holds a string alive forever. Both are in
 * this file, and the second one is the discipline the intern table depends on.
 */

#include "vendor/greatest.h"

#include <inttypes.h>
#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/array.h"
#include "cvm/gc.h"
#include "cvm/hash.h"
#include "cvm/str.h"
#include "cvm/weak.h"
#include "gc/collect.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"
#include "str/dtoa.h"

#if !defined(_WIN32)

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)32 * 1024 * 1024)

static CvmHeap* heap(void) {
    return &cvm_world()->heap;
}

static void collect(void) {
    cvm_gc_collect_and_finish(heap());
}

static void fixture_enter(void) {
    (void)cvm_thread_attach();
}

static void fixture_leave(void) {
    cvm_thread_detach();
}

#define FRAME(slots)                                                                   \
    CVM_FRAME_DECL(f, slots);                                                          \
    cvm_frame_push(&f_frame, f_roots, slots);                                          \
    for (u32 i = 0; i < (slots); i++) {                                                \
        f_roots[i] = NULL;                                                             \
    }

#define FRAME_END() cvm_frame_pop(&f_frame)

/* A value, since a table's values are GC references like its keys. */
typedef struct Item {
    u64 value;
} Item;

static CvmTypeDescriptor const item_desc = {
    .size = sizeof(Item),
    .kind = CVM_TYPE_SCALAR,
};

static Item* item_new(u64 v) {
    Item* it = cvm_alloc(&item_desc);
    it->value = v;
    return it;
}

/* A key from a C string literal. */
static CvmString* key_new(char const* text) {
    str s = {.ptr = (u8 const*)text, .len = 0};
    while (text[s.len] != '\0') {
        s.len++;
    }
    return cvm_string_new_ascii(s);
}

/* A key from a number, to make many distinct keys without a table of literals. */
static CvmString* key_from_u32(u32 n) {
    u8 buf[16];
    usize len = cvm_u32toa(buf, n);
    return cvm_string_new8(buf, len);
}

/* ------------------------------------------------------------------ */
/* the empty table                                                     */
/* ------------------------------------------------------------------ */

TEST an_empty_table_has_no_buckets_and_answers_nothing(void) {
    fixture_enter();

    FRAME(1);
    CvmHashMap* m = cvm_map_new();
    f_roots[0] = m;

    ASSERT_EQ_FMT((usize)0, cvm_map_count(m), "%zu");
    ASSERT_EQ_FMT((usize)0, cvm_map_buckets(m), "%zu");
    ASSERT(cvm_map_get(m, key_new("x")) == NULL);
    ASSERT_FALSE(cvm_map_has(m, key_new("x")));
    ASSERT_FALSE(cvm_map_del(m, key_new("x")));

    /* the bucket array appears on the first insert, not before */
    (void)cvm_map_put(m, key_new("x"), item_new(1), NULL);
    ASSERT(cvm_map_buckets(m) > 0);

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* content-addressed lookup                                            */
/* ------------------------------------------------------------------ */

TEST equal_text_is_the_same_key(void) {
    /*
     * The property the whole design is for: two strings that were built
     * separately, at different addresses, are one key. Anything that hashes or
     * compares by address would pass a single-key test and fail this one.
     */
    fixture_enter();

    FRAME(1);
    CvmHashMap* m = cvm_map_new();
    f_roots[0] = m;

    Item* value = item_new(42);
    (void)cvm_map_put(m, key_new("hello"), value, NULL);

    CvmString* same_text = key_new("hello");
    ASSERT(cvm_map_has(m, same_text));
    ASSERT(cvm_map_get(m, same_text) == value);

    CvmString* different = key_new("hellp");
    ASSERT_FALSE(cvm_map_has(m, different));

    /* and a prefix is not the key */
    ASSERT_FALSE(cvm_map_has(m, key_new("hell")));

    ASSERT_EQ_FMT((usize)1, cvm_map_count(m), "%zu");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST put_replaces_and_says_whether_the_key_was_new(void) {
    fixture_enter();

    FRAME(1);
    CvmHashMap* m = cvm_map_new();
    f_roots[0] = m;

    bool absent = false;
    Item* first = item_new(1);
    Item* second = item_new(2);

    /* put returns the value that is now stored: the argument, for a new key */
    ASSERT(cvm_map_put(m, key_new("k"), first, &absent) == first);
    ASSERT(absent);
    ASSERT_EQ_FMT((usize)1, cvm_map_count(m), "%zu");

    /* the second put reports the previous value and does not add an entry */
    Item* previous = (Item*)cvm_map_put(m, key_new("k"), second, &absent);
    ASSERT(previous == first);
    ASSERT_FALSE(absent);
    ASSERT_EQ_FMT((usize)1, cvm_map_count(m), "%zu");
    ASSERT(cvm_map_get(m, key_new("k")) == second);

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* deletion                                                            */
/* ------------------------------------------------------------------ */

TEST deleting_a_key_leaves_the_others_reachable(void) {
    /*
     * The backward-shift case. Deleting leaves a hole in a probe chain unless
     * the entries behind it are moved up, so this inserts enough keys into a
     * small table that the chains are long, deletes every other one, and then
     * checks that every survivor is still findable.
     *
     * A table that deleted by clearing a bit alone fails here and nowhere else.
     */
    fixture_enter();

    FRAME(1);
    CvmHashMap* m = cvm_map_new_seeded(0x1234u);
    f_roots[0] = m;

    enum { N = 64 };
    for (u32 i = 0; i < N; i++) {
        (void)cvm_map_put(m, key_from_u32(i * 7u), item_new(i), NULL);
    }
    ASSERT_EQ_FMT((usize)N, cvm_map_count(m), "%zu");

    for (u32 i = 0; i < N; i += 2) {
        ASSERT(cvm_map_del(m, key_from_u32(i * 7u)));
    }
    ASSERT_EQ_FMT((usize)N / 2, cvm_map_count(m), "%zu");

    for (u32 i = 0; i < N; i++) {
        CvmString* k = key_from_u32(i * 7u);
        if ((i % 2u) == 0u) {
            ASSERT_FALSE(cvm_map_has(m, k));
        } else {
            if (!cvm_map_has(m, k)) {
                FAILm("a key behind a deleted one became unreachable");
            }
            Item* it = (Item*)cvm_map_get(m, k);
            ASSERT_EQ_FMT((u64)i, it->value, "%" PRIu64);
        }
    }

    /* and a key that was never there is still not there */
    ASSERT_FALSE(cvm_map_has(m, key_from_u32(100000u)));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_deleted_key_is_no_longer_referenced(void) {
    /*
     * The discipline the intern table rests on. The bucket array is traced in
     * full, so a slot left holding a deleted key keeps that string alive for
     * good — in an intern table, that is a leak in the program rather than in
     * the table.
     */
    fixture_enter();

    FRAME(2);
    CvmHashMap* m = cvm_map_new();
    f_roots[0] = m;

    CvmString* key = key_new("gone");
    void* weak = cvm_weak_ref_new(key);
    f_roots[1] = weak;

    (void)cvm_map_put(m, key, item_new(1), NULL);

    collect();
    ASSERT(cvm_weak_ref_target(weak) == key); /* the table holds it */

    ASSERT(cvm_map_del(m, key));
    collect();
    ASSERT(cvm_weak_ref_target(weak) == NULL); /* and now it does not */

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST clear_drops_every_entry_and_frees_the_keys(void) {
    fixture_enter();

    FRAME(3);
    CvmHashMap* m = cvm_map_new();
    f_roots[0] = m;

    CvmString* watched = key_new("watched");
    void* weak = cvm_weak_ref_new(watched);
    f_roots[1] = weak;
    f_roots[2] = watched;

    (void)cvm_map_put(m, watched, item_new(1), NULL);
    for (u32 i = 0; i < 20; i++) {
        (void)cvm_map_put(m, key_from_u32(i), item_new(i), NULL);
    }

    usize buckets = cvm_map_buckets(m);
    cvm_map_clear(m);

    ASSERT_EQ_FMT((usize)0, cvm_map_count(m), "%zu");
    /* the array is kept for reuse, not thrown away */
    ASSERT_EQ_FMT(buckets, cvm_map_buckets(m), "%zu");
    ASSERT_FALSE(cvm_map_has(m, key_new("watched")));
    ASSERT(cvm_map_next(m, 0) == CVM_MAP_NO_SLOT);

    /* the key is no longer referenced, even though the array is still there */
    f_roots[2] = NULL;
    collect();
    ASSERT(cvm_weak_ref_target(weak) == NULL);

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* growth and shrinking                                                */
/* ------------------------------------------------------------------ */

TEST growth_keeps_every_entry_findable(void) {
    fixture_enter();

    FRAME(1);
    CvmHashMap* m = cvm_map_new();
    f_roots[0] = m;

    enum { N = 500 };
    for (u32 i = 0; i < N; i++) {
        (void)cvm_map_put(m, key_from_u32(i), item_new(i), NULL);
    }

    ASSERT_EQ_FMT((usize)N, cvm_map_count(m), "%zu");
    /* 75% load factor: the array must be bigger than the entry count */
    ASSERT(cvm_map_buckets(m) > (usize)N);
    ASSERT(cvm_map_buckets(m) * 3u / 4u >= (usize)N);

    for (u32 i = 0; i < N; i++) {
        Item* it = (Item*)cvm_map_get(m, key_from_u32(i));
        if (it == NULL) {
            FAILm("an entry was lost across a growth");
        }
        ASSERT_EQ_FMT((u64)i, it->value, "%" PRIu64);
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_table_can_shrink_back(void) {
    /*
     * Without this, a table whose entries come and go keeps the array its
     * busiest moment needed, for the life of the process. The intern table is
     * the case that makes it necessary: nothing about its contents says how
     * large it once was.
     */
    fixture_enter();

    FRAME(1);
    CvmHashMap* m = cvm_map_new();
    f_roots[0] = m;

    for (u32 i = 0; i < 500; i++) {
        (void)cvm_map_put(m, key_from_u32(i), item_new(i), NULL);
    }
    usize big = cvm_map_buckets(m);

    for (u32 i = 1; i < 500; i++) {
        ASSERT(cvm_map_del(m, key_from_u32(i)));
    }
    ASSERT_EQ_FMT((usize)1, cvm_map_count(m), "%zu");

    ASSERT(cvm_map_resize(m, 8));
    ASSERT(cvm_map_buckets(m) < big);
    ASSERT_EQ_FMT((usize)8, cvm_map_buckets(m), "%zu");

    /* the survivor is still there, with its value */
    Item* it = (Item*)cvm_map_get(m, key_from_u32(0));
    ASSERT(it != NULL);
    ASSERT_EQ_FMT((u64)0, it->value, "%" PRIu64);

    /* a resize that cannot fit the contents is refused, and changes nothing */
    for (u32 i = 0; i < 50; i++) {
        (void)cvm_map_put(m, key_from_u32(1000u + i), item_new(i), NULL);
    }
    usize before = cvm_map_buckets(m);
    ASSERT_FALSE(cvm_map_resize(m, 4));
    ASSERT_EQ_FMT(before, cvm_map_buckets(m), "%zu");
    ASSERT_EQ_FMT((usize)51, cvm_map_count(m), "%zu");

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* traversal                                                           */
/* ------------------------------------------------------------------ */

TEST the_cursor_visits_every_entry_exactly_once(void) {
    fixture_enter();

    FRAME(1);
    CvmHashMap* m = cvm_map_new();
    f_roots[0] = m;

    enum { N = 100 };
    for (u32 i = 0; i < N; i++) {
        (void)cvm_map_put(m, key_from_u32(i), item_new(i), NULL);
    }
    /* and delete some, so the traversal has to skip empty buckets */
    usize deleted = 0;
    for (u32 i = 0; i < N; i += 3) {
        ASSERT(cvm_map_del(m, key_from_u32(i)));
        deleted++;
    }

    bool seen[N];
    for (usize i = 0; i < N; i++) {
        seen[i] = false;
    }

    usize visited = 0;
    for (usize s = cvm_map_next(m, 0); s != CVM_MAP_NO_SLOT;
         s = cvm_map_next(m, s + 1)) {
        Item* it = (Item*)cvm_map_value_at(m, s);
        ASSERT(it != NULL);
        ASSERT(it->value < (u64)N);
        ASSERT_FALSE(seen[it->value]);
        seen[it->value] = true;
        visited++;

        /* the key in the slot is what the value belongs to */
        CvmString* k = (CvmString*)cvm_map_key_at(m, s);
        ASSERT(cvm_map_get(m, k) == it);
    }

    ASSERT_EQ_FMT((usize)N - deleted, visited, "%zu");
    for (usize i = 0; i < N; i++) {
        ASSERT_EQ_FMT(((i % 3u) == 0u) ? false : true, seen[i], "%d");
    }
    ASSERT_EQ_FMT((usize)N - deleted, cvm_map_count(m), "%zu");

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* the collector's view                                                */
/* ------------------------------------------------------------------ */

TEST a_table_keeps_its_keys_and_values_alive(void) {
    fixture_enter();

    FRAME(1);
    CvmHashMap* m = cvm_map_new();
    f_roots[0] = m;

    for (u32 i = 0; i < 30; i++) {
        (void)cvm_map_put(m, key_from_u32(i), item_new(i * 100u), NULL);
    }

    collect();

    m = (CvmHashMap*)f_roots[0];
    ASSERT_EQ_FMT((usize)30, cvm_map_count(m), "%zu");
    for (u32 i = 0; i < 30; i++) {
        Item* it = (Item*)cvm_map_get(m, key_from_u32(i));
        if (it == NULL) {
            FAILm("an entry did not survive the collection");
        }
        ASSERT_EQ_FMT((u64)i * 100u, it->value, "%" PRIu64);
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST entries_can_be_deleted_while_the_collector_runs(void) {
    /*
     * A table that is used and swept repeatedly, which is the shape an intern
     * table has for the whole life of a process. Each round inserts, deletes
     * most of what it inserted, and collects: the entry count must come back to
     * where it started rather than creeping up.
     */
    fixture_enter();

    FRAME(1);
    CvmHashMap* m = cvm_map_new();
    f_roots[0] = m;

    (void)cvm_map_put(m, key_new("resident"), item_new(1), NULL);

    for (u32 round = 0; round < 20; round++) {
        for (u32 i = 0; i < 50; i++) {
            (void)cvm_map_put(m, key_from_u32(round * 1000u + i), item_new(i), NULL);
        }
        ASSERT_EQ_FMT((usize)51, cvm_map_count(m), "%zu");

        for (u32 i = 0; i < 50; i++) {
            ASSERT(cvm_map_del(m, key_from_u32(round * 1000u + i)));
        }
        collect();
        ASSERT_EQ_FMT((usize)1, cvm_map_count(m), "%zu");
    }

    /* the one key that stayed is untouched by all of it */
    Item* it = (Item*)cvm_map_get(m, key_new("resident"));
    ASSERT(it != NULL);
    ASSERT_EQ_FMT((u64)1, it->value, "%" PRIu64);

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* keys that are not strings                                           */
/* ------------------------------------------------------------------ */

/*
 * Identity-hash keys: a GC object is its own key, hashed by address. The
 * hash convention says every non-string GC object works this way, and the
 * tests here are the first time that promise has been exercised.
 */
static u32 identity_hash(opaque_mut key) {
    /* The address itself, mixed: a straight pointer has high bits that cluster
     * when the heap allocates sequentially, and the bucket takes the high bits
     * of the hash. */
    usize addr = (usize)key;
    addr ^= addr >> 15;
    return (u32)(addr ^ (addr >> 32));
}

static bool identity_matches(opaque_mut stored, opaque_mut key) {
    return stored == key;
}

static bool identity_valid(opaque_mut stored) {
    return stored != NULL;
}

static opaque_mut identity_store(opaque_mut key) {
    return key;
}

static CvmMapOps const identity_ops = {
    .hash = identity_hash,
    .hash_stored = identity_hash,
    .matches = identity_matches,
    .valid = identity_valid,
    .store = identity_store,
};

/*
 * Integer keys: the value is the key. A pointer-sized integer, which is how
 * every C runtime puts a small value where a pointer is expected.
 */
static u32 integer_hash(opaque_mut key) {
    /* Knuth multiplicative: the low bits of the key are not used by the
     * Fibonacci bucket map, so spread them up. */
    return (u32)(((usize)key * 0x9E3779B97F4A7C15ull) >> 32);
}

static bool integer_matches(opaque_mut stored, opaque_mut key) {
    return (usize)stored == (usize)key;
}

static bool integer_valid(opaque_mut stored) {
    return stored != NULL;
}

static opaque_mut integer_store(opaque_mut key) {
    return key;
}

static CvmMapOps const integer_ops = {
    .hash = integer_hash,
    .hash_stored = integer_hash,
    .matches = integer_matches,
    .valid = integer_valid,
    .store = integer_store,
};

/* A scalar object to use as an identity key. */
static CvmTypeDescriptor const scratch_desc = {
    .size = sizeof(u64),
    .kind = CVM_TYPE_SCALAR,
};

TEST identity_keys_find_the_object_that_went_in(void) {
    fixture_enter();
    FRAME(6);
    CvmHashMap* m = cvm_map_new_with(&identity_ops, 0x1234u);
    f_roots[0] = m;

    /* Three objects, each its own key and its own value. */
    for (u32 i = 0; i < 3; i++) {
        f_roots[1 + i] = cvm_alloc(&scratch_desc);
    }

    for (u32 i = 0; i < 3; i++) {
        (void)cvm_map_put(m, f_roots[1 + i], f_roots[1 + i], NULL);
    }
    ASSERT_EQ_FMT((usize)3, cvm_map_count(m), "%zu");

    /* Each is found, and nothing else is. */
    for (u32 i = 0; i < 3; i++) {
        ASSERT(cvm_map_has(m, f_roots[1 + i]));
        ASSERT(cvm_map_get(m, f_roots[1 + i]) == f_roots[1 + i]);
    }

    /* A fourth object that was never inserted is not found. */
    f_roots[4] = cvm_alloc(&scratch_desc);
    ASSERT(!cvm_map_has(m, f_roots[4]));

    /* Deleting one leaves the other two reachable. */
    ASSERT(cvm_map_del(m, f_roots[1]));
    ASSERT(!cvm_map_has(m, f_roots[1]));
    ASSERT(cvm_map_has(m, f_roots[2]));
    ASSERT(cvm_map_has(m, f_roots[3]));
    ASSERT_EQ_FMT((usize)2, cvm_map_count(m), "%zu");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST integer_keys_find_the_value_that_went_in(void) {
    fixture_enter();
    FRAME(2);
    CvmHashMap* m = cvm_map_new_with(&integer_ops, 0x5678u);
    f_roots[0] = m;

    /*
     * Integers 1..100, each its own key. Key 0 is not representable: the map
     * uses a NULL key to mean absent, and 0 as a pointer is NULL, so the
     * integer keys start at 1 and that is a property of the convention rather
     * than a choice this test made.
     */
    for (u32 i = 1; i <= 100u; i++) {
        (void)cvm_map_put(m, (opaque_mut)(usize)i, (opaque_mut)(usize)(i + 100u), NULL);
    }
    ASSERT_EQ_FMT((usize)100, cvm_map_count(m), "%zu");

    for (u32 i = 1; i <= 100u; i++) {
        ASSERT((usize)cvm_map_get(m, (opaque_mut)(usize)i) == (usize)(i + 100u));
    }

    /* Delete the even ones. */
    for (u32 i = 2; i <= 100u; i += 2u) {
        ASSERT(cvm_map_del(m, (opaque_mut)(usize)i));
    }
    ASSERT_EQ_FMT((usize)50, cvm_map_count(m), "%zu");

    /* The odd ones survived, the even ones are gone. */
    for (u32 i = 1; i <= 100u; i++) {
        if (i % 2u == 0u) {
            ASSERT(!cvm_map_has(m, (opaque_mut)(usize)i));
        } else {
            ASSERT(cvm_map_has(m, (opaque_mut)(usize)i));
        }
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* deletion clusters and probe wraparound                              */
/* ------------------------------------------------------------------ */

/*
 * Delete a contiguous run of keys and verify the rest are still findable.
 *
 * This is the case that made prune a whole-table rehash rather than an
 * in-place delete: backward-shift deletion moves entries back to close the
 * chain, and a chain that wraps past the end of the array moves an entry that
 * a scan has not reached yet. The in-place delete leaves dead entries behind;
 * the rehash does not. This test exercises the same wraparound through the
 * public API, which goes through cvm_map_del_slot and therefore through the
 * same backward shift.
 */
TEST deleting_a_cluster_leaves_the_rest_reachable(void) {
    fixture_enter();
    FRAME(2);
    /* A fixed seed so the bucket layout is reproducible. */
    CvmHashMap* m = cvm_map_new_seeded(0xABCDu);
    f_roots[0] = m;

    /* Enough keys to fill the table past its first growth. */
    u32 const N = 300;
    for (u32 i = 0; i < N; i++) {
        f_roots[1] = key_from_u32(i);
        (void)cvm_map_put(m, f_roots[1], f_roots[1], NULL);
        f_roots[1] = NULL;
    }
    ASSERT_EQ_FMT((usize)N, cvm_map_count(m), "%zu");

    /* Delete a contiguous band in the middle, where the keys' hashes cluster. */
    u32 const LO = N / 3;
    u32 const HI = N / 3 + N / 6;
    for (u32 i = LO; i < HI; i++) {
        f_roots[1] = key_from_u32(i);
        ASSERT(cvm_map_del(m, f_roots[1]));
        f_roots[1] = NULL;
    }
    ASSERT_EQ_FMT((usize)(N - (HI - LO)), cvm_map_count(m), "%zu");

    /* Everything before the band. */
    for (u32 i = 0; i < LO; i++) {
        f_roots[1] = key_from_u32(i);
        ASSERT(cvm_map_has(m, f_roots[1]));
        f_roots[1] = NULL;
    }
    /* Everything after the band. */
    for (u32 i = HI; i < N; i++) {
        f_roots[1] = key_from_u32(i);
        ASSERT(cvm_map_has(m, f_roots[1]));
        f_roots[1] = NULL;
    }
    /* And nothing inside the band. */
    for (u32 i = LO; i < HI; i++) {
        f_roots[1] = key_from_u32(i);
        ASSERT(!cvm_map_has(m, f_roots[1]));
        f_roots[1] = NULL;
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

/*
 * Delete every other key in a table that has grown past several sizes, so the
 * probe chains have been rebuilt at different bucket counts and some wrap.
 */
TEST delete_interleaved_across_growth(void) {
    fixture_enter();
    FRAME(2);
    CvmHashMap* m = cvm_map_new();
    f_roots[0] = m;

    u32 const N = 500;
    for (u32 i = 0; i < N; i++) {
        f_roots[1] = key_from_u32(i * 7u);
        (void)cvm_map_put(m, f_roots[1], f_roots[1], NULL);
        f_roots[1] = NULL;
    }
    ASSERT_EQ_FMT((usize)N, cvm_map_count(m), "%zu");

    /* Delete every other one. */
    for (u32 i = 0; i < N; i += 2u) {
        f_roots[1] = key_from_u32(i * 7u);
        ASSERT(cvm_map_del(m, f_roots[1]));
        f_roots[1] = NULL;
    }
    ASSERT_EQ_FMT((usize)(N / 2), cvm_map_count(m), "%zu");

    /* Every survivor is findable; every deleted one is not. */
    for (u32 i = 0; i < N; i++) {
        f_roots[1] = key_from_u32(i * 7u);
        if (i % 2u == 0u) {
            ASSERT(!cvm_map_has(m, f_roots[1]));
        } else {
            ASSERT(cvm_map_has(m, f_roots[1]));
        }
        f_roots[1] = NULL;
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

/*
 * A table that fills, empties, and fills again: the chains are rebuilt at
 * every size, and the second fill must not find ghosts from the first.
 */
TEST fill_empty_refill_finds_no_ghosts(void) {
    fixture_enter();
    FRAME(2);
    CvmHashMap* m = cvm_map_new();
    f_roots[0] = m;

    for (u32 round = 0; round < 3u; round++) {
        for (u32 i = 0; i < 200u; i++) {
            f_roots[1] = key_from_u32(i);
            (void)cvm_map_put(m, f_roots[1], f_roots[1], NULL);
            f_roots[1] = NULL;
        }
        for (u32 i = 0; i < 200u; i++) {
            f_roots[1] = key_from_u32(i);
            ASSERT(cvm_map_del(m, f_roots[1]));
            f_roots[1] = NULL;
        }
        ASSERT_EQ_FMT((usize)0, cvm_map_count(m), "%zu");
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(hash_suite) {
    RUN_TEST(identity_keys_find_the_object_that_went_in);
    RUN_TEST(integer_keys_find_the_value_that_went_in);
    RUN_TEST(deleting_a_cluster_leaves_the_rest_reachable);
    RUN_TEST(delete_interleaved_across_growth);
    RUN_TEST(fill_empty_refill_finds_no_ghosts);
    RUN_TEST(an_empty_table_has_no_buckets_and_answers_nothing);
    RUN_TEST(equal_text_is_the_same_key);
    RUN_TEST(put_replaces_and_says_whether_the_key_was_new);

    RUN_TEST(deleting_a_key_leaves_the_others_reachable);
    RUN_TEST(a_deleted_key_is_no_longer_referenced);
    RUN_TEST(clear_drops_every_entry_and_frees_the_keys);

    RUN_TEST(growth_keeps_every_entry_findable);
    RUN_TEST(a_table_can_shrink_back);

    RUN_TEST(the_cursor_visits_every_entry_exactly_once);

    RUN_TEST(a_table_keeps_its_keys_and_values_alive);
    RUN_TEST(entries_can_be_deleted_while_the_collector_runs);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    cvm_threads_init();
    cvm_safepoint_init();
    if (!cvm_world_init(TEST_MIN, TEST_MAX)) {
        fprintf(stderr, "could not bring up the heap\n");
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(hash_suite);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}

#else /* _WIN32 */

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
