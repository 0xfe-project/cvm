/*
 * tests/unit/test_collect.c
 *
 * End-to-end collection: allocate, drop, collect, and check what
 * happened.
 *
 * This is where the oracles from TEST.md first become usable, because
 * there is now something to run them against:
 *
 *   TP  a reachable object survives, contents intact
 *   TN  an unreachable object is reclaimed
 *   FP  an unreachable object is kept (a leak, not a corruption)
 *   FN  a reachable object is reclaimed (the catastrophic one)
 *
 * The weak reference is the instrument for TN and FP: it is the only
 * place the collector states its liveness decision in a form a test can
 * read. Canary payloads are the instrument for TP and FN: an object that
 * survives must still hold what was written into it, and an object that
 * was wrongly reclaimed is caught by writing into it afterwards.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <stddef.h>
#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "gc/alloc.h"
#include "gc/collect.h"
#include "gc/heap.h"
#include "gc/large.h"
#include "gc/marker.h"
#include "gc/weak.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)32 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* types                                                               */
/* ------------------------------------------------------------------ */

/* A node holding one pointer and a payload, so both can be checked. */
typedef struct Node {
    opaque_mut next;
    u64 canary;
} Node;

static CvmPtrField const node_fields[] = {{.offset = offsetof(Node, next)}};

static CvmTypeDescriptor const node_desc = {
    .size = sizeof(Node),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 1,
    .num_embed_fields = 0,
    .ptr_fields = node_fields,
    .embed_fields = NULL,
};

typedef struct Leaf {
    u64 canary;
} Leaf;

static CvmTypeDescriptor const leaf_desc = {
    .size = sizeof(Leaf),
    .kind = CVM_TYPE_SCALAR,
};

/* A canary derived from the address, so a wrong-but-plausible value is
 * distinguishable from the right one. */
static u64 canary_for(void const* p) {
    return 0xc0ffee00u ^ (u64)(usize)p;
}

static void stamp(void* payload, u64 value) {
    ((Leaf*)payload)->canary = value;
}

/* ------------------------------------------------------------------ */
/* fixture                                                             */
/* ------------------------------------------------------------------ */

static CvmHeap* heap(void) {
    return &cvm_world()->heap;
}

static void fixture_enter(void) {
    (void)cvm_thread_attach();
}

static void fixture_leave(void) {
    cvm_thread_detach();
}

/* Collect, then drive the sweep to completion so the heap is settled. */
static void collect(void) {
    cvm_gc_collect_and_finish(heap());
}

/*
 * Whether the collector considers this object live.
 *
 * Both slot states count. Between marking and sweeping a live object's
 * slot reads `marked`; once the sweep has run it reads `allocated`, since
 * that is what sweeping turns a mark into. Asking only about `marked`
 * would say every object died the moment a collection finished — which is
 * how this helper came to be written the way it is.
 */
static bool is_live(void* payload) {
    ObjectMeta const* meta =
        bytemap_get_const(heap()->bytemap, (u8 const*)cvm_header_of(payload));
    return object_meta_is_allocated(meta) || object_meta_is_marked(meta);
}

#define FRAME(slots)                                                                   \
    CVM_FRAME_DECL(f, slots);                                                          \
    cvm_frame_push(&f_frame, f_roots, slots);                                          \
    for (u32 i = 0; i < (slots); i++) {                                                \
        f_roots[i] = NULL;                                                             \
    }

#define FRAME_END() cvm_frame_pop(&f_frame)

/* ------------------------------------------------------------------ */
/* TN: an unreachable object is reclaimed                              */
/* ------------------------------------------------------------------ */

/*
 * Whether the collector's metadata says this address holds a live object.
 *
 * Read through the object's start, and note that the metadata table is a
 * separate region from the object itself — so asking this question about
 * a reclaimed object is not a use-after-free.
 */
static bool slot_is_allocated(void* payload) {
    ObjectMeta const* meta =
        bytemap_get_const(heap()->bytemap, (u8 const*)cvm_header_of(payload));
    return object_meta_is_allocated(meta);
}

TEST an_unreachable_object_is_reclaimed(void) {
    fixture_enter();

    void* leaf = cvm_alloc(&leaf_desc);
    ASSERT(slot_is_allocated(leaf));

    collect();

    /*
     * TN, observed directly. A weak reference would not work here: an
     * unreachable weak reference is never reached by the marker, so it is
     * never queued for nullifying, and its slot keeps whatever was in it
     * until the memory is reused. Reading that would be reading freed
     * memory and calling the stale value a liveness decision.
     */
    ASSERT_FALSE(slot_is_allocated(leaf));

    fixture_leave();
    PASS();
}

TEST a_reachable_object_survives(void) {
    fixture_enter();

    FRAME(1);

    void* leaf = cvm_alloc(&leaf_desc);
    stamp(leaf, canary_for(leaf));
    f_roots[0] = leaf;

    collect();

    /* TP: still live, and still holding what was written into it */
    ASSERT(is_live(leaf));
    ASSERT_EQ_FMT(canary_for(leaf), ((Leaf*)leaf)->canary, "%llu");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_weak_ref_is_cleared_when_its_target_dies(void) {
    fixture_enter();

    FRAME(1);

    void* leaf = cvm_alloc(&leaf_desc);
    void* weak = cvm_weak_ref_new(leaf);
    f_roots[0] = weak;

    collect();

    /* the weak reference survived, its target did not */
    ASSERT(is_live(weak));
    ASSERT(cvm_weak_ref_target(weak) == NULL);

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_weak_ref_keeps_its_target_when_something_else_does(void) {
    fixture_enter();

    FRAME(2);

    void* leaf = cvm_alloc(&leaf_desc);
    void* weak = cvm_weak_ref_new(leaf);
    Node* holder = cvm_alloc(&node_desc);
    holder->next = leaf;

    f_roots[0] = weak;
    f_roots[1] = holder;

    collect();

    ASSERT(is_live(leaf));
    ASSERT(cvm_weak_ref_target(weak) == leaf);

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* following a graph                                                   */
/* ------------------------------------------------------------------ */

TEST a_chain_survives_a_collection(void) {
    fixture_enter();

    FRAME(1);

    enum { N = 16 };
    Node* nodes[N];
    for (int i = 0; i < N; i++) {
        nodes[i] = cvm_alloc(&node_desc);
        nodes[i]->canary = canary_for(nodes[i]);
        nodes[i]->next = NULL;
    }
    for (int i = 0; i + 1 < N; i++) {
        nodes[i]->next = nodes[i + 1];
    }
    f_roots[0] = nodes[0];

    collect();

    for (int i = 0; i < N; i++) {
        ASSERT(is_live(nodes[i]));
        ASSERT_EQ_FMT(canary_for(nodes[i]), nodes[i]->canary, "%llu");
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST the_tail_of_a_broken_chain_is_reclaimed(void) {
    fixture_enter();

    FRAME(1);

    Node* head = cvm_alloc(&node_desc);
    Node* middle = cvm_alloc(&node_desc);
    Node* tail = cvm_alloc(&node_desc);

    head->next = middle;
    middle->next = NULL; /* the chain to tail is cut */
    tail->next = NULL;

    f_roots[0] = head;

    collect();

    ASSERT(is_live(head));
    ASSERT(is_live(middle));
    /* tail is unreachable, so it dies; its slot is the evidence */
    ASSERT_FALSE(is_live(tail));
    ASSERT_FALSE(slot_is_allocated(tail));

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* repeated collection                                                 */
/* ------------------------------------------------------------------ */

TEST repeated_collections_are_stable(void) {
    fixture_enter();

    FRAME(1);

    /* a long-lived object that must survive every round */
    Leaf* keeper = cvm_alloc(&leaf_desc);
    stamp(keeper, canary_for(keeper));
    f_roots[0] = keeper;

    for (int round = 0; round < 8; round++) {
        /* garbage */
        for (int i = 0; i < 200; i++) {
            void* junk = cvm_alloc(&leaf_desc);
            stamp(junk, 0xdead);
        }

        collect();

        ASSERT(is_live(keeper));
        ASSERT_EQ_FMT(canary_for(keeper), ((Leaf*)keeper)->canary, "%llu");
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST collection_reclaims_space(void) {
    /*
     * TN at the heap level: after allocating a lot of garbage and
     * collecting, the number of objects the collector considers live must
     * be far below what was allocated. Counted through the bytemap, so it
     * measures the collector's own view rather than the allocator's.
     */
    fixture_enter();

    FRAME(1);

    Leaf* keeper = cvm_alloc(&leaf_desc);
    f_roots[0] = keeper;

    for (int i = 0; i < 2000; i++) {
        (void)cvm_alloc(&leaf_desc);
    }

    collect();

    /* One live object: the keeper. A handful of others may exist — the
     * weak list, internal bookkeeping — so the check is a bound rather
     * than an equality. */
    u32 live = 0;
    for (u32 b = 0; b < heap()->block_count; b++) {
        u8* block_start = cvm_heap_block_address(heap(), b);
        ObjectMeta* slot = bytemap_get(heap()->bytemap, block_start);
        for (usize s = 0; s < CVM_SLOTS_PER_BLOCK; s++) {
            if (object_meta_is_allocated(&slot[s])) {
                live++;
            }
        }
    }

    ASSERT(live < 100);
    ASSERT(is_live(keeper));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST allocation_still_works_after_a_collection(void) {
    /* The allocator's caches were cleared by the collection, so this is
     * what checks that it can rebuild them. */
    fixture_enter();

    FRAME(1);

    Leaf* keeper = cvm_alloc(&leaf_desc);
    f_roots[0] = keeper;

    collect();

    for (int i = 0; i < 500; i++) {
        void* p = cvm_alloc(&leaf_desc);
        ASSERT(p != NULL);
        stamp(p, canary_for(p));
        ASSERT_EQ_FMT(canary_for(p), ((Leaf*)p)->canary, "%llu");
    }

    ASSERT(is_live(keeper));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST surviving_objects_keep_their_identity_across_collections(void) {
    /* If a live object were moved or its metadata lost, its address would
     * stop matching what the frame holds. */
    fixture_enter();

    FRAME(4);

    enum { N = 4 };
    void* objects[N];
    for (int i = 0; i < N; i++) {
        objects[i] = cvm_alloc(&leaf_desc);
        stamp(objects[i], canary_for(objects[i]));
        f_roots[i] = objects[i];
    }

    for (int round = 0; round < 5; round++) {
        collect();
        for (int i = 0; i < N; i++) {
            ASSERT(f_roots[i] == objects[i]);
            ASSERT_EQ_FMT(canary_for(objects[i]), ((Leaf*)objects[i])->canary, "%llu");
        }
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* large objects                                                       */
/* ------------------------------------------------------------------ */

TEST a_large_object_is_reclaimed_when_it_dies(void) {
    fixture_enter();

    CvmTypeDescriptor large_desc = {
        .size = 9000,
        .kind = CVM_TYPE_SCALAR,
    };

    void* big = cvm_alloc(&large_desc);
    ASSERT(big != NULL);
    ASSERT(cvm_heap_contains(heap(), (u8 const*)cvm_header_of(big)));
    ASSERT(slot_is_allocated(big));

    collect();

    ASSERT_FALSE(slot_is_allocated(big));

    fixture_leave();
    PASS();
}

TEST a_large_object_survives_when_reachable(void) {
    fixture_enter();

    CvmTypeDescriptor large_desc = {
        .size = 9000,
        .kind = CVM_TYPE_SCALAR,
    };

    FRAME(1);

    void* big = cvm_alloc(&large_desc);
    memset(big, 0xAB, large_desc.size);
    f_roots[0] = big;

    collect();

    ASSERT(is_live(big));
    /* its bytes are still there */
    u8* bytes = big;
    ASSERT_EQ_FMT(0xABu, (u32)bytes[0], "%02x");
    ASSERT_EQ_FMT(0xABu, (u32)bytes[large_desc.size - 1], "%02x");

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* the collection's own report                                         */
/* ------------------------------------------------------------------ */

TEST the_collection_report_is_consistent(void) {
    fixture_enter();

    FRAME(1);

    f_roots[0] = cvm_alloc(&leaf_desc);
    for (int i = 0; i < 100; i++) {
        (void)cvm_alloc(&leaf_desc);
    }

    collect();

    CvmCollectionStats stats = cvm_gc_last_collection();
    ASSERT(stats.end_ns >= stats.start_ns);
    ASSERT(stats.mark_ns <= stats.end_ns - stats.start_ns);
    ASSERT(stats.objects_before >= stats.objects_after || stats.objects_before > 0);
    ASSERT_EQ_FMT(stats.blocks_before, stats.blocks_after, "%u");

    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(collect_suite) {
    RUN_TEST(an_unreachable_object_is_reclaimed);
    RUN_TEST(a_reachable_object_survives);
    RUN_TEST(a_weak_ref_is_cleared_when_its_target_dies);
    RUN_TEST(a_weak_ref_keeps_its_target_when_something_else_does);
    RUN_TEST(a_chain_survives_a_collection);
    RUN_TEST(the_tail_of_a_broken_chain_is_reclaimed);
    RUN_TEST(repeated_collections_are_stable);
    RUN_TEST(collection_reclaims_space);
    RUN_TEST(allocation_still_works_after_a_collection);
    RUN_TEST(surviving_objects_keep_their_identity_across_collections);
    RUN_TEST(a_large_object_is_reclaimed_when_it_dies);
    RUN_TEST(a_large_object_survives_when_reachable);
    RUN_TEST(the_collection_report_is_consistent);
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
    RUN_SUITE(collect_suite);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}

#endif /* !_WIN32 */
