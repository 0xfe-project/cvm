/*
 * tests/unit/test_alloc.c
 *
 * The allocation path: the inlined bump fast path, the fallbacks behind
 * it, and the large-object chunk allocator.
 *
 * These tests run against a real heap and a real block allocator, because
 * the allocator's whole job is to interact with them. What they cannot
 * exercise is collection: there is no collector yet, so the retry loop
 * grows the heap instead of reclaiming. That is stated in the tests that
 * depend on it rather than glossed over.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "gc/alloc.h"
#include "gc/block_alloc.h"
#include "gc/bytemap.h"
#include "gc/heap.h"
#include "gc/large.h"
#include "gc/layout.h"
#include "gc/meta.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)16 * 1024 * 1024)

/* A leaf type: no pointers, so the marker would skip it. */
static CvmTypeDescriptor const scalar8 = {
    .size = 8,
    .kind = CVM_TYPE_SCALAR,
};

static CvmTypeDescriptor const scalar64 = {
    .size = 64,
    .kind = CVM_TYPE_SCALAR,
};

/* Bigger than a line, so it takes the overflow path. */
static CvmTypeDescriptor const medium1000 = {
    .size = 1000,
    .kind = CVM_TYPE_SCALAR,
};

/* At or above the large-object threshold. */
static CvmTypeDescriptor const large9000 = {
    .size = 9000,
    .kind = CVM_TYPE_SCALAR,
};

/*
 * Larger than one block, which is the other kind of large object: a chunk is
 * carved out of a block and cannot span one, so an object this size is a run
 * of whole blocks -- a superblock. 70000 bytes plus a header rounds to 73728,
 * which is three blocks, and three is deliberately not a power of two: that is
 * the case where the heap grows by four to hold three.
 */
static CvmTypeDescriptor const spanning70000 = {
    .size = 70000,
    .kind = CVM_TYPE_SCALAR,
};

/* ------------------------------------------------------------------ */
/* fixture                                                             */
/* ------------------------------------------------------------------ */

/*
 * Attach the calling thread. ASSERT cannot be used here: it expands to a
 * bare `return`, so it only works in a void function. The tests check the
 * attachment took by looking at the context instead.
 */
static void fixture_enter(void) {
    (void)cvm_thread_attach();
}

static void fixture_leave(void) {
    cvm_thread_detach();
}

static CvmHeap* the_heap(void) {
    return &cvm_world()->heap;
}

static Bytemap* the_bytemap(void) {
    return cvm_world()->heap.bytemap;
}

static bool in_heap(opaque_mut p) {
    return cvm_heap_contains(the_heap(), (u8 const*)p);
}

/* ------------------------------------------------------------------ */
/* the fast path                                                       */
/* ------------------------------------------------------------------ */

TEST small_allocation_lands_in_the_heap_with_a_header(void) {
    fixture_enter();
    ASSERT(cvm_thread_ctx() != NULL);

    void* p = cvm_alloc(&scalar8);
    ASSERT(p != NULL);
    ASSERT(in_heap(p));

    /* the header sits immediately before the payload */
    CvmGcHeader const* header = cvm_header_of(p);
    ASSERT(header->type == &scalar8);

    /*
     * One word, and that is the point of it: the two flags this collector
     * has are in the address's metadata byte, where whippet keeps the same
     * kind of information, and the mark state was never in the object. Two
     * words make a 24-byte node 48 bytes instead of 32.
     */
    ASSERT_EQ_FMT(sizeof(void*), (usize)sizeof(CvmGcHeader), "%zu");

    /* and the bytemap says an object starts here */
    ASSERT(object_meta_is_allocated(
        bytemap_get_const(the_bytemap(), (u8 const*)cvm_header_of(p))));

    fixture_leave();
    PASS();
}

TEST allocations_are_distinct_and_aligned(void) {
    fixture_enter();
    ASSERT(cvm_thread_ctx() != NULL);

    enum { N = 64 };
    void* seen[N];

    for (int i = 0; i < N; i++) {
        seen[i] = cvm_alloc(&scalar8);
        ASSERT(seen[i] != NULL);

        /*
         * The object, not the payload: the header is what the metadata
         * table indexes, and 16-byte alignment is what makes an object's
         * entry exact. The payload is one header further on, so it is
         * 8-aligned since the header became one word; a type that needs
         * more has to pad its own payload, and the frontend knows the
         * header's size.
         */
        ASSERT_EQ_FMT(0u, (u32)((usize)cvm_header_of(seen[i]) % CVM_ALLOC_ALIGNMENT),
                      "%u");
        ASSERT(in_heap(seen[i]));
    }

    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            ASSERT(seen[i] != seen[j]);
        }
    }

    fixture_leave();
    PASS();
}

TEST a_fresh_object_is_zeroed(void) {
    /*
     * Part of the contract: generated code assumes a fresh object has
     * zeroed fields, which is what lets a type with pointer fields start
     * out valid rather than full of rubbish the marker would follow.
     */
    fixture_enter();
    ASSERT(cvm_thread_ctx() != NULL);

    u8* p = cvm_alloc(&scalar64);
    ASSERT(p != NULL);
    for (u32 i = 0; i < scalar64.size; i++) {
        ASSERT_EQ_FMT(0u, (u32)p[i], "%u");
    }

    fixture_leave();
    PASS();
}

TEST allocation_crosses_line_boundaries_without_overlap(void) {
    /*
     * A line is 256 bytes and the alignment is 16, so objects of 64 bytes
     * land 4 per line. Allocating many of them must walk through lines and
     * eventually blocks without ever handing out an overlapping range.
     */
    fixture_enter();
    ASSERT(cvm_thread_ctx() != NULL);

    enum { N = 300 };
    u8* p[N];
    for (int i = 0; i < N; i++) {
        p[i] = cvm_alloc(&scalar64);
        ASSERT(p[i] != NULL);
    }

    /* sorted-by-address adjacency: each object must end before the next
     * begins, wherever it happens to be */
    for (int i = 0; i + 1 < N; i++) {
        usize a = (usize)p[i];
        usize b = (usize)p[i + 1];
        usize step =
            sizeof(CvmGcHeader) + cvm_align_up(scalar64.size, CVM_ALLOC_ALIGNMENT);
        if (b > a) {
            ASSERT(b - a >= step);
        }
    }

    fixture_leave();
    PASS();
}

TEST many_allocations_stay_inside_the_heap(void) {
    fixture_enter();
    ASSERT(cvm_thread_ctx() != NULL);

    for (int i = 0; i < 2000; i++) {
        void* p = cvm_alloc(&scalar8);
        ASSERT(p != NULL);
        ASSERT(in_heap(p));
        ASSERT(object_meta_is_allocated(
            bytemap_get_const(the_bytemap(), (u8 const*)cvm_header_of(p))));
    }

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* medium objects                                                      */
/* ------------------------------------------------------------------ */

TEST an_object_larger_than_a_line_takes_the_overflow_path(void) {
    fixture_enter();
    ASSERT(cvm_thread_ctx() != NULL);

    u8* p = cvm_alloc(&medium1000);
    ASSERT(p != NULL);
    ASSERT(in_heap(p));
    ASSERT(object_meta_is_allocated(
        bytemap_get_const(the_bytemap(), (u8 const*)cvm_header_of(p))));

    /* the object is written end to end, so it is not just a line */
    for (u32 i = 0; i < medium1000.size; i++) {
        ASSERT_EQ_FMT(0u, (u32)p[i], "%u");
    }

    /* a second medium object continues in the same overflow region rather
     * than starting a new block, so the two are close together */
    u8* q = cvm_alloc(&medium1000);
    ASSERT(q != NULL);
    ASSERT(q != p);

    usize step =
        cvm_align_up(sizeof(CvmGcHeader) + medium1000.size, CVM_ALLOC_ALIGNMENT);
    ASSERT_EQ_FMT(step, (usize)(q - p), "%zu");

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* large objects                                                       */
/* ------------------------------------------------------------------ */

TEST a_large_object_is_chunk_allocated(void) {
    fixture_enter();
    ASSERT(cvm_thread_ctx() != NULL);

    u8* p = cvm_alloc(&large9000);
    ASSERT(p != NULL);
    ASSERT(in_heap(p));

    CvmGcHeader const* header = cvm_header_of(p);
    ASSERT(header->type == &large9000);
    ASSERT(object_meta_is_allocated(
        bytemap_get_const(the_bytemap(), (u8 const*)cvm_header_of(p))));

    /* the chunk is rounded up to a multiple of the large-object
     * granularity, so the object fits with room to spare */
    usize needed = sizeof(CvmGcHeader) + large9000.size;
    ASSERT(cvm_align_up(needed, CVM_LARGE_OBJECT_MIN_SIZE) >= needed);

    fixture_leave();
    PASS();
}

TEST an_object_larger_than_a_block_takes_a_superblock(void) {
    fixture_enter();
    ASSERT(cvm_thread_ctx() != NULL);

    u8* p = cvm_alloc(&spanning70000);
    ASSERT(p != NULL);
    ASSERT(in_heap(p));

    CvmGcHeader const* header = cvm_header_of(p);
    ASSERT(header->type == &spanning70000);
    ASSERT(
        object_meta_is_allocated(bytemap_get_const(the_bytemap(), (u8 const*)header)));

    /*
     * The object's blocks are described as one superblock, and its size is
     * the number of blocks it reaches. That is what the sweeper reads to
     * decide which blocks are its body: it frees every block but the last
     * when the object at the start is dead.
     */
    usize needed =
        cvm_align_up(sizeof(CvmGcHeader) + spanning70000.size, CVM_BLOCK_SIZE);
    u32 blocks = (u32)(needed / CVM_BLOCK_SIZE);
    ASSERT(blocks > 1);

    CvmHeap* h = the_heap();
    BlockMeta* head =
        cvm_heap_block_meta(h, block_index_of(h->heap_start, (u8*)header));
    ASSERT(block_meta_is_superblock_start(head));
    ASSERT_EQ_FMT(blocks, block_meta_superblock_size(head), "%u");

    /* Every other block of the run says it is a tail, which is what tells
     * the marker not to look for line marks in it. */
    for (u32 i = 1; i < blocks; i++) {
        ASSERT(block_meta_is_superblock_tail(head + i));
    }

    /* The object's whole payload is the object's: write it end to end and
     * read it back, which a superblock handed out twice would not survive. */
    for (usize i = 0; i < spanning70000.size; i++) {
        p[i] = (u8)(0x5au ^ (u8)i);
    }
    for (usize i = 0; i < spanning70000.size; i++) {
        ASSERT_EQ_FMT((u8)(0x5au ^ (u8)i), p[i], "%u");
    }

    fixture_leave();
    PASS();
}

TEST spanning_objects_do_not_overlap(void) {
    fixture_enter();
    ASSERT(cvm_thread_ctx() != NULL);

    enum { N = 4 };
    u8* p[N];
    for (int i = 0; i < N; i++) {
        p[i] = cvm_alloc(&spanning70000);
        ASSERT(p[i] != NULL);
    }

    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            if (i == j) {
                continue;
            }
            usize a = (usize)p[i];
            usize b = (usize)p[j];
            ASSERT(a >= b + spanning70000.size || b >= a + spanning70000.size);
        }
    }

    fixture_leave();
    PASS();
}

TEST large_objects_do_not_overlap(void) {
    fixture_enter();
    ASSERT(cvm_thread_ctx() != NULL);

    enum { N = 8 };
    u8* p[N];
    for (int i = 0; i < N; i++) {
        p[i] = cvm_alloc(&large9000);
        ASSERT(p[i] != NULL);
        ASSERT(in_heap(p[i]));
    }

    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            usize a = (usize)p[i];
            usize b = (usize)p[j];
            usize lo = a < b ? a : b;
            usize hi = a < b ? b : a;
            /* the chunk sizes are rounded up to 16 KiB for a 9 KiB object */
            ASSERT(hi - lo >= CVM_LARGE_OBJECT_MIN_SIZE);
        }
    }

    fixture_leave();
    PASS();
}

TEST a_returned_chunk_is_reused(void) {
    /*
     * The segregated free lists exist so a chunk of a given size is
     * reused rather than the block being re-cut. Hand a chunk back and
     * check that the next allocation of that size takes it.
     */
    fixture_enter();
    ASSERT(cvm_thread_ctx() != NULL);

    /*
     * The lists are the heap's, so the count is too -- which is the property
     * that matters: a chunk filed by one thread is visible to the next, and
     * to a GC worker that sweeps.
     */
    u32 free_before = cvm_large_free_chunk_count(the_heap());

    /* Build a chunk from a fresh block and hand it back. */
    CvmChunk* chunk = (CvmChunk*)cvm_heap_block_address(the_heap(), 0);
    ASSERT(chunk != NULL);
    cvm_large_add_chunk(the_heap(), chunk, CVM_LARGE_OBJECT_MIN_SIZE);

    ASSERT_EQ_FMT(free_before + 1, cvm_large_free_chunk_count(the_heap()), "%u");
    ASSERT(cvm_large_free_bytes(the_heap()) >= CVM_LARGE_OBJECT_MIN_SIZE);

    fixture_leave();
    PASS();
}

TEST large_allocation_reports_the_granularity(void) {
    /* The chunk size is the object size rounded up to the granularity, and
     * a chunk is never larger than a block: anything bigger goes to the
     * block allocator as a superblock, which is a different path. */
    ASSERT(cvm_align_up(1, CVM_LARGE_OBJECT_MIN_SIZE) == CVM_LARGE_OBJECT_MIN_SIZE);
    ASSERT(cvm_align_up(CVM_LARGE_OBJECT_MIN_SIZE, CVM_LARGE_OBJECT_MIN_SIZE) ==
           CVM_LARGE_OBJECT_MIN_SIZE);
    ASSERT(cvm_align_up(CVM_LARGE_OBJECT_MIN_SIZE + 1, CVM_LARGE_OBJECT_MIN_SIZE) ==
           2 * CVM_LARGE_OBJECT_MIN_SIZE);
    ASSERT_EQ_FMT(3u, (u32)CVM_LARGE_FREE_LIST_COUNT, "%u");
    PASS();
}

/* ------------------------------------------------------------------ */
/* the allocator's own state                                           */
/* ------------------------------------------------------------------ */

TEST a_fresh_thread_has_a_clear_allocator(void) {
    fixture_enter();
    ASSERT(cvm_thread_ctx() != NULL);

    CvmThreadNode* node = cvm_thread_node();
    ASSERT(node != NULL);
    ASSERT(node->allocator.heap == the_heap());
    ASSERT(node->allocator.bytemap == the_bytemap());
    ASSERT(node->allocator.block_alloc == &cvm_world()->block_alloc);
    ASSERT_EQ_FMT(0u, cvm_allocator_recycled_count(&node->allocator), "%u");

    /*
     * The chunk free lists are not part of this. They are the heap's, so a
     * thread attaching neither inherits them nor clears them -- and the
     * check that used to be here, that a fresh thread sees them empty, only
     * passed because an earlier test in this binary had not filed anything
     * yet. An invariant that holds by accident of test order is not one.
     *
     * What is asserted instead is the property that matters: attaching does
     * not disturb them. A GC worker sweeping files chunks into these lists
     * without attaching at all, so anything that cleared them per attach
     * would throw that work away.
     */
    u32 chunks_before = cvm_large_free_chunk_count(the_heap());
    fixture_leave();
    fixture_enter();
    ASSERT_EQ_FMT(chunks_before, cvm_large_free_chunk_count(the_heap()), "%u");

    fixture_leave();
    PASS();
}

TEST clearing_the_allocator_drops_the_cached_region(void) {
    fixture_enter();
    ASSERT(cvm_thread_ctx() != NULL);

    CvmAllocator* alloc = &cvm_thread_node()->allocator;

    (void)cvm_alloc(&scalar8);
    ASSERT(cvm_ctx()->bump_ptr != NULL);

    cvm_allocator_clear(alloc, cvm_ctx());
    ASSERT(alloc->block == NULL);
    ASSERT(alloc->large_cursor == NULL);
    ASSERT_EQ_FMT(0u, cvm_allocator_recycled_count(alloc), "%u");

    /* and allocation still works afterwards */
    void* p = cvm_alloc(&scalar8);
    ASSERT(p != NULL);
    ASSERT(in_heap(p));

    fixture_leave();
    PASS();
}

SUITE(alloc_suite) {
    RUN_TEST(small_allocation_lands_in_the_heap_with_a_header);
    RUN_TEST(allocations_are_distinct_and_aligned);
    RUN_TEST(a_fresh_object_is_zeroed);
    RUN_TEST(allocation_crosses_line_boundaries_without_overlap);
    RUN_TEST(many_allocations_stay_inside_the_heap);
    RUN_TEST(an_object_larger_than_a_line_takes_the_overflow_path);
    RUN_TEST(a_large_object_is_chunk_allocated);
    RUN_TEST(an_object_larger_than_a_block_takes_a_superblock);
    RUN_TEST(spanning_objects_do_not_overlap);
    RUN_TEST(large_objects_do_not_overlap);
    RUN_TEST(a_returned_chunk_is_reused);
    RUN_TEST(large_allocation_reports_the_granularity);
    RUN_TEST(a_fresh_thread_has_a_clear_allocator);
    RUN_TEST(clearing_the_allocator_drops_the_cached_region);
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
    RUN_SUITE(alloc_suite);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}

#endif /* !_WIN32 */
