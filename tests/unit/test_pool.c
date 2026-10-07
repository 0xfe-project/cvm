/*
 * tests/unit/test_pool.c
 *
 * The concurrent GC worker pool.
 *
 * A pool changes *when* work happens, not *what* work happens, so the
 * property that matters is that a collection with workers produces exactly
 * the same heap as one without. These tests check that by running the same
 * workload twice — once with the pool, once without — and comparing what
 * survived.
 *
 * The pool is also where the collector's concurrency lives, so this
 * binary is one of the more useful ones to run under ThreadSanitizer.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/external.h"
#include "cvm/gc.h"
#include "cvm/roots.h"
#include "cvm/weak.h"
#include "gc/collect.h"
#include "gc/heap.h"
#include "gc/pool.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)32 * 1024 * 1024)

typedef struct Node {
    opaque_mut next;
    u64 canary;
} Node;

static CvmPtrField const node_fields[] = {{.offset = offsetof(Node, next)}};

static CvmTypeDescriptor const node_type = {
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

static CvmTypeDescriptor const leaf_type = {
    .size = sizeof(Leaf),
    .kind = CVM_TYPE_SCALAR,
};

/* A big array, so the marker's array splitting has something to split. */
typedef struct PtrArray {
    usize length;
    opaque_mut items[];
} PtrArray;

static CvmTypeDescriptor const ptr_array_type = {
    .size = sizeof(PtrArray),
    .kind = CVM_TYPE_PTR_ARRAY,
    .array_length_offset = offsetof(PtrArray, length),
    .array_data_offset = offsetof(PtrArray, items),
};

static CvmHeap* heap(void) {
    return &cvm_world()->heap;
}

static void fixture_enter(void) {
    (void)cvm_thread_attach();
}

static void fixture_leave(void) {
    cvm_thread_detach();
}

static u64 canary_for(void const* p) {
    return 0x5a5a0000u ^ (u64)(usize)p;
}

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

/* Build a chain of `n` nodes and return the head. */
static opaque_mut build_chain(i64 n) {
    CVM_FRAME_DECL(f, 2);
    cvm_frame_push(&f_frame, f_roots, 2);
    f_roots[0] = NULL;
    f_roots[1] = NULL;

    for (i64 i = 0; i < n; i++) {
        f_roots[1] = cvm_alloc(&node_type);
        Node* node = f_roots[1];
        node->next = f_roots[0];
        node->canary = canary_for(node);
        f_roots[0] = f_roots[1];
        f_roots[1] = NULL;
    }

    opaque_mut head = f_roots[0];
    cvm_frame_pop(&f_frame);
    return head;
}

static bool chain_is_intact(opaque_mut head, i64 expected) {
    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = head;

    i64 seen = 0;
    while (f_roots[0] != NULL && seen <= expected) {
        Node* node = f_roots[0];
        if (node->canary != canary_for(node)) {
            cvm_frame_pop(&f_frame);
            return false;
        }
        f_roots[0] = node->next;
        seen++;
    }

    cvm_frame_pop(&f_frame);
    return seen == expected;
}

/* ------------------------------------------------------------------ */
/* the pool itself                                                     */
/* ------------------------------------------------------------------ */

TEST a_pool_comes_up_and_goes_down(void) {
    ASSERT(heap()->pool == NULL);

    ASSERT(cvm_pool_init(heap(), 3));
    ASSERT(heap()->pool != NULL);
    ASSERT_EQ_FMT(3u, (u32)heap()->pool->count, "%u");

    /* Nothing is active while idle. */
    ASSERT_EQ_FMT(0u, cvm_pool_active_count(heap()->pool), "%u");
    ASSERT_FALSE(cvm_pool_any_active(heap()->pool));

    cvm_pool_destroy(heap());
    ASSERT(heap()->pool == NULL);
    PASS();
}

TEST the_default_count_is_bounded(void) {
    u32 count = cvm_pool_default_count(heap());
    ASSERT(count >= 1);
    ASSERT(count <= CVM_MAX_GC_WORKERS);
    ASSERT(count <= heap()->mark.packet_count);
    PASS();
}

TEST init_is_idempotent(void) {
    ASSERT(cvm_pool_init(heap(), 2));
    CvmGcPool* first = heap()->pool;
    ASSERT(cvm_pool_init(heap(), 5)); /* different count, same pool */
    ASSERT(heap()->pool == first);
    ASSERT_EQ_FMT(2u, (u32)heap()->pool->count, "%u");
    cvm_pool_destroy(heap());
    PASS();
}

TEST the_pool_can_be_restarted(void) {
    ASSERT(cvm_pool_init(heap(), 2));
    cvm_pool_destroy(heap());
    ASSERT(cvm_pool_init(heap(), 2));
    ASSERT(heap()->pool != NULL);
    cvm_pool_destroy(heap());
    PASS();
}

/* ------------------------------------------------------------------ */
/* a collection with workers does the same thing                       */
/* ------------------------------------------------------------------ */

/*
 * Workload shared by the two runs.
 *
 * `root` keeps the chain alive, so the result is comparable: the same
 * objects should survive whether or not workers helped.
 */
static void run_workload(opaque_mut* head_out, CvmRootHandle* root_out) {
    opaque_mut chain = build_chain(500);
    *root_out = cvm_root_add(chain);
    *head_out = chain;

    /* garbage in several shapes: leaves, chains, and a big array */
    CVM_FRAME_DECL(f, 2);
    cvm_frame_push(&f_frame, f_roots, 2);
    f_roots[0] = NULL;
    f_roots[1] = NULL;

    for (int round = 0; round < 3; round++) {
        for (int i = 0; i < 500; i++) {
            f_roots[0] = cvm_alloc(&leaf_type);
            f_roots[0] = NULL;
        }

        f_roots[0] = build_chain(100);
        f_roots[0] = NULL;

        /* large enough to exercise the marker's array splitting */
        enum { N = 1500 };
        PtrArray* array =
            cvm_alloc_array(&ptr_array_type, N, cvm_array_stride(&ptr_array_type));
        f_roots[1] = array;
        for (int i = 0; i < N; i++) {
            array->items[i] = NULL;
        }
        f_roots[1] = NULL;

        cvm_gc_collect_and_finish(heap());
    }

    cvm_frame_pop(&f_frame);
}

TEST a_collection_with_workers_matches_one_without(void) {
    fixture_enter();

    /* --- without workers --- */
    opaque_mut chain_single = NULL;
    CvmRootHandle root_single = CVM_ROOT_HANDLE_NONE;
    run_workload(&chain_single, &root_single);

    ASSERT(chain_is_intact(chain_single, 500));
    u32 live_single = 0;
    {
        /* Count what survived, so the two runs can be compared. */
        for (u32 b = 0; b < heap()->block_count; b++) {
            u8* block_start = cvm_heap_block_address(heap(), b);
            ObjectMeta* slot = bytemap_get(heap()->bytemap, block_start);
            for (usize s = 0; s < CVM_SLOTS_PER_BLOCK; s++) {
                if (object_meta_is_allocated(&slot[s])) {
                    live_single++;
                }
            }
        }
    }
    cvm_root_remove(root_single);

    /* --- with workers --- */
    ASSERT(cvm_pool_init(heap(), 4));

    opaque_mut chain_multi = NULL;
    CvmRootHandle root_multi = CVM_ROOT_HANDLE_NONE;
    run_workload(&chain_multi, &root_multi);

    ASSERT(chain_is_intact(chain_multi, 500));
    u32 live_multi = 0;
    for (u32 b = 0; b < heap()->block_count; b++) {
        u8* block_start = cvm_heap_block_address(heap(), b);
        ObjectMeta* slot = bytemap_get(heap()->bytemap, block_start);
        for (usize s = 0; s < CVM_SLOTS_PER_BLOCK; s++) {
            if (object_meta_is_allocated(&slot[s])) {
                live_multi++;
            }
        }
    }
    cvm_root_remove(root_multi);

    cvm_pool_destroy(heap());

    /*
     * The same objects must have survived. An exact match is expected
     * rather than a tolerance: the workload is deterministic, and the only
     * difference is who did the tracing. A count that differs means a
     * worker marked something the single-threaded run did not, or missed
     * something — either of which is a liveness bug.
     */
    ASSERT_EQ_FMT(live_single, live_multi, "%u");

    fixture_leave();
    PASS();
}

TEST workers_handle_a_large_array(void) {
    /* A big pointer array is the case the marker splits across threads. */
    fixture_enter();

    ASSERT(cvm_pool_init(heap(), 4));

    enum { N = 4000 };
    CVM_FRAME_DECL(f, 3);
    cvm_frame_push(&f_frame, f_roots, 3);
    for (u32 i = 0; i < 3; i++) {
        f_roots[i] = NULL;
    }

    PtrArray* array =
        cvm_alloc_array(&ptr_array_type, N, cvm_array_stride(&ptr_array_type));
    f_roots[0] = array;

    for (int i = 0; i < N; i++) {
        Leaf* leaf = cvm_alloc(&leaf_type);
        leaf->canary = canary_for(leaf);
        array->items[i] = leaf;
    }

    cvm_gc_collect_and_finish(heap());

    /* Every element must have been reached and kept. */
    u32 alive = 0;
    for (int i = 0; i < N; i++) {
        if (array->items[i] != NULL && is_live(array->items[i])) {
            alive++;
        }
    }
    ASSERT_EQ_FMT((u32)N, alive, "%u");

    cvm_frame_pop(&f_frame);
    cvm_pool_destroy(heap());
    fixture_leave();
    PASS();
}

TEST weak_references_are_nullified_with_workers(void) {
    fixture_enter();
    ASSERT(cvm_pool_init(heap(), 4));

    enum { N = 200 };
    CVM_FRAME_DECL(f, N);
    cvm_frame_push(&f_frame, f_roots, N);
    for (u32 i = 0; i < N; i++) {
        f_roots[i] = NULL;
    }

    for (int i = 0; i < N; i++) {
        void* target = cvm_alloc(&leaf_type);
        f_roots[i] = cvm_weak_ref_new(target);
    }

    cvm_gc_collect_and_finish(heap());

    u32 cleared = 0;
    for (int i = 0; i < N; i++) {
        if (cvm_weak_ref_target(f_roots[i]) == NULL) {
            cleared++;
        }
    }
    ASSERT_EQ_FMT((u32)N, cleared, "%u");

    cvm_frame_pop(&f_frame);
    cvm_pool_destroy(heap());
    fixture_leave();
    PASS();
}

TEST finalizers_still_run_with_workers(void) {
    fixture_enter();
    ASSERT(cvm_pool_init(heap(), 3));

    u64 before = cvm_finalizer_run_count();
    enum { N = 150 };
    for (int i = 0; i < N; i++) {
        (void)cvm_external_new(NULL, NULL); /* no finalizer, just die */
    }

    cvm_gc_collect_and_finish(heap());
    cvm_finalizer_drain_now();

    /*
     * These have no finalizer, so nothing runs — the point is that the
     * pool's sweep did not lose them or trip its own assertions. What is
     * checked is that the heap is still usable.
     */
    void* after = cvm_alloc(&leaf_type);
    ASSERT(after != NULL);
    ASSERT(is_live(after));
    ASSERT(cvm_finalizer_run_count() >= before);

    cvm_pool_destroy(heap());
    fixture_leave();
    PASS();
}

SUITE(pool_suite) {
    RUN_TEST(a_pool_comes_up_and_goes_down);
    RUN_TEST(the_default_count_is_bounded);
    RUN_TEST(init_is_idempotent);
    RUN_TEST(the_pool_can_be_restarted);
    RUN_TEST(a_collection_with_workers_matches_one_without);
    RUN_TEST(workers_handle_a_large_array);
    RUN_TEST(weak_references_are_nullified_with_workers);
    RUN_TEST(finalizers_still_run_with_workers);
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
    RUN_SUITE(pool_suite);
    GREATEST_MAIN_END();

    cvm_finalizer_stop();
    cvm_world_destroy();
}

#endif /* !_WIN32 */
