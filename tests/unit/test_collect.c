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
#include "gc/block_alloc.h"
#include "gc/heap.h"
#include "gc/large.h"
#include "gc/marker.h"
#include "cvm/weak.h"
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

/*
 * Bytes the collector has released that are not zero.
 *
 * The object-level check further down is the claim the contract makes, but on
 * its own it is not enough to test it: the allocator is free to serve the
 * second round from blocks it has never used, and the check then passes
 * without having looked at a single reclaimed byte. That is not hypothetical
 * -- it is what this test did at first, and removing a clear did not fail it.
 *
 * So the pool is checked rather than the allocation, and by state rather than
 * by guesswork:
 *
 *   free or reserved            the whole block must be zero
 *   a superblock with no live object at its start
 *                               the whole run must be zero
 *   simple                      every free-line run must be zero, except the
 *                               first 16 bytes of each run, which is where
 *                               the free-line node lives until the allocator
 *                               clears it on the way in
 *   anything with a live object nothing can be said, so nothing is checked
 *
 * A block being "handed back" in a state other than free is why the states
 * are enumerated: a swept large object's block reaches the allocator described
 * as a one-block superblock, not as free, and a check that only looked at free
 * blocks would step straight over it.
 */
static u32 released_bytes_that_are_not_zero(void) {
    u32 offenders = 0;
    CvmHeap* h = heap();

    for (u32 b = 0; b < h->block_count; b++) {
        BlockMeta* meta = cvm_heap_block_meta(h, b);
        u8* start = cvm_heap_block_address(h, b);
        usize run = 1;

        if (block_meta_is_superblock_start(meta)) {
            run = block_meta_superblock_size(meta);
            ASSERT(run > 0 && b + run <= h->block_count);

            /*
             * A superblock holds large objects at granularity strides, and
             * any of them may have died while the object at the start
             * survived -- which is the case the chunk walk in the sweeper
             * exists for, and the only one where that walk's clear has
             * something to clear that is not already zero. A free chunk says
             * so in the bytemap: its first slot is a placeholder.
             *
             * The chunk's own header is the free list's link and holds
             * neither zeroes nor the object, so it is skipped; the size it
             * carries is what says how far the chunk reaches.
             */
            for (usize off = 0; off < run * CVM_BLOCK_SIZE;
                 off += CVM_LARGE_OBJECT_MIN_SIZE) {
                u8* at = start + off;
                if (!object_meta_is_placeholder(bytemap_get(h->bytemap, at))) {
                    continue;
                }
                usize bytes = *(usize*)at;
                if (bytes < CVM_LARGE_OBJECT_MIN_SIZE || bytes > CVM_BLOCK_SIZE) {
                    continue; /* not a chunk header we can make sense of */
                }
                for (usize i = 16; i < bytes; i++) {
                    if (at[i] != 0) {
                        offenders++;
                        break;
                    }
                }
            }

            b += run - 1;
            continue;
        } else if (block_meta_is_simple(meta)) {
            for (i8 line = block_meta_first_free_line(meta); line != CVM_LAST_HOLE;) {
                FreeLineMeta* node =
                    (FreeLineMeta*)(start + (usize)line * CVM_LINE_SIZE);
                usize bytes = (usize)node->size * CVM_LINE_SIZE;
                for (usize i = 16; i < bytes; i++) {
                    if (((u8*)node)[i] != 0) {
                        offenders++;
                        break;
                    }
                }
                line = node->next;
            }
            continue;
        }

        /* Free, reserved, or a dead superblock whose metadata has been swept:
         * every byte of it is memory the collector has released. */
        for (usize i = 0; i < run * CVM_BLOCK_SIZE; i++) {
            if (start[i] != 0) {
                offenders++;
                break;
            }
        }
    }

    return offenders;
}

TEST reclaimed_memory_is_zeroed_before_it_is_reused(void) {
    /*
     * The object contract says a fresh object's payload is zeroed, and the
     * allocator no longer does that -- the collector does it when it
     * reclaims the memory, and the platform does it when the heap is first
     * mapped. So the case that matters is not the first allocation from a
     * fresh heap, which is covered in test_alloc.c, but the one that reuses
     * memory a dead program wrote into.
     *
     * Every size class is walked because each has a reclaim site of its own:
     * a wholly dead block, a run of free lines inside a block that still has
     * survivors, the blocks that body a multi-block large object, the single
     * block one large object gets to itself, and a large object's chunk.
     *
     * A pattern rather than a canary: one byte in the right place is not
     * enough to catch a clear that covered the wrong range, and every one of
     * these sites is about a range.
     */
    /*
     * Sizes that between them walk every reclaim path, and enough of them
     * that the heap runs out of room: a round has to be larger than half the
     * heap's maximum, or the second round is served from memory the heap has
     * never used and the test passes without testing anything. That is not
     * hypothetical -- with 300 per size the heap grew, the second round took
     * fresh blocks, and removing the clear from the single-block superblock
     * path did not fail the test.
     */
    static usize const sizes[] = {16, 48, 64, 512, 2000, 9000};
    static unsigned const waves[] = {600, 600, 600, 600, 600, 2200};
    enum { KINDS = sizeof(sizes) / sizeof(sizes[0]) };

    fixture_enter();
    FRAME(1);

    for (unsigned k = 0; k < KINDS; k++) {
        CvmTypeDescriptor desc = {.size = (u32)sizes[k], .kind = CVM_TYPE_SCALAR};
        for (unsigned i = 0; i < waves[k]; i++) {
            u8* p = cvm_alloc(&desc);
            ASSERT(p != NULL);
            for (usize b = 0; b < sizes[k]; b++) {
                p[b] = (u8)(0xa5u ^ (u8)b);
            }
        }
    }

    collect();

    ASSERT_EQ_FMT(0u, released_bytes_that_are_not_zero(), "%u");

    for (unsigned k = 0; k < KINDS; k++) {
        CvmTypeDescriptor desc = {.size = (u32)sizes[k], .kind = CVM_TYPE_SCALAR};
        for (unsigned i = 0; i < waves[k]; i++) {
            u8* p = cvm_alloc(&desc);
            ASSERT(p != NULL);
            for (usize b = 0; b < sizes[k]; b++) {
                if (p[b] != 0) {
                    FAILm("reclaimed memory was handed out without being cleared");
                }
            }
        }
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_partly_free_block_is_zeroed_where_it_is_free(void) {
    /*
     * The other half of the same claim.
     *
     * A collection in which nothing survives leaves whole blocks free, and
     * those are cleared in one call each. A collection in which *something*
     * survives leaves a block with holes -- free runs of lines beside lines
     * that still hold live objects -- and a free run is cleared as a run,
     * with the free-line node written back into its first line afterwards.
     * A workload with no survivor never produces a hole, so it cannot see
     * that path at all: removing the run clear left the other test passing.
     *
     * The keeper is the instrument. It is what makes a block partly free,
     * and checking that it is still live at the end is what proves the test
     * was in the state it claims to be in.
     */
    enum { GARBAGE = 4000 };

    fixture_enter();
    FRAME(1);

    for (unsigned i = 0; i < GARBAGE; i++) {
        u8* p = cvm_alloc(&leaf_desc);
        ASSERT(p != NULL);
        stamp(p, 0xa5a5a5a5a5a5a5a5ull);
        if (i == GARBAGE / 2) {
            f_roots[0] = p;
        }
    }

    collect();

    ASSERT(f_roots[0] != NULL);
    ASSERT(is_live(f_roots[0]));
    ASSERT_EQ_FMT(0u, released_bytes_that_are_not_zero(), "%u");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_spanning_object_survives_and_is_reclaimed(void) {
    /*
     * A large object bigger than a block is a superblock: a run of whole
     * blocks with the object at the front of it. Nothing used to allocate
     * one, so cvm_sweep_superblock had no workload that reached it, and the
     * clearing it does when it reclaims one -- the front blocks, the last
     * block, the dead chunks in a block that still has a survivor -- was
     * unreachable code with a test that could not fail.
     *
     * This is the workload. A spanning object is allocated, held across a
     * collection, checked byte for byte, dropped, and its memory allocated
     * again and checked for zeroes, which is the same claim the two tests
     * above make about a block and about a free run.
     */
    enum { SIZE = 70000 };
    CvmTypeDescriptor desc = {.size = SIZE, .kind = CVM_TYPE_SCALAR};

    fixture_enter();
    FRAME(1);

    u8* first = cvm_alloc(&desc);
    ASSERT(first != NULL);
    for (usize i = 0; i < SIZE; i++) {
        first[i] = (u8)(0x3cu ^ (u8)i);
    }
    f_roots[0] = first;

    collect();

    /* Live, so nothing was cleared under it and nothing moved it. */
    ASSERT(is_live(f_roots[0]));
    for (usize i = 0; i < SIZE; i++) {
        if (first[i] != (u8)(0x3cu ^ (u8)i)) {
            FAILm("a spanning object's contents did not survive the collection");
        }
    }

    f_roots[0] = NULL;
    collect();

    ASSERT_EQ_FMT(0u, released_bytes_that_are_not_zero(), "%u");

    u8* again = cvm_alloc(&desc);
    ASSERT(again != NULL);
    for (usize i = 0; i < SIZE; i++) {
        if (again[i] != 0) {
            FAILm("a reclaimed superblock was handed out without being cleared");
        }
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_superblock_clears_the_chunk_beside_a_survivor(void) {
    /*
     * A block holding two large objects, one of which survives.
     *
     * This is the case cvm_sweep_superblock's chunk walk exists for, and the
     * only one that can show whether its clear works. A chunk that is free
     * is already zero and clearing it again changes nothing; a chunk that
     * held a live object which then died is full of that object's bytes, and
     * only then does the clear have anything to do.
     *
     * Two objects of 8 KiB come to 16 KiB each with the header rounding,
     * which is exactly half a block, so the second one lands in what is left
     * of the first one's superblock. Rooting the first and dropping the
     * second is what separates them; the pool check afterwards is what reads
     * the result, and it does not care where the allocator puts the next
     * object.
     */
    enum { SIZE = 8192 };
    CvmTypeDescriptor desc = {.size = SIZE, .kind = CVM_TYPE_SCALAR};

    fixture_enter();
    FRAME(1);

    /*
     * Start from an empty chunk free list, or the two objects below need not
     * land in one block: a chunk left over from an earlier test in this
     * binary is a chunk of the same size class, and the first allocation can
     * pop that instead of a fresh superblock. A collection with nothing
     * rooted frees every block whole, so nothing is filed as a chunk and the
     * list comes back empty.
     */
    f_roots[0] = NULL;
    collect();

    u8* survivor = cvm_alloc(&desc);
    u8* doomed = cvm_alloc(&desc);
    ASSERT(survivor != NULL);
    ASSERT(doomed != NULL);

    /*
     * Same block, or this test is not in the state it claims to be in. Worth
     * asserting rather than assuming: packing two large objects into one
     * block is the allocator's business, and a change to it would otherwise
     * turn this into a test of nothing.
     */
    CvmHeap* h = heap();
    u32 survivor_block = block_index_of(h->heap_start, (u8*)cvm_header_of(survivor));
    u32 doomed_block = block_index_of(h->heap_start, (u8*)cvm_header_of(doomed));
    ASSERT_EQ_FMT(survivor_block, doomed_block, "%u");

    for (usize i = 0; i < SIZE; i++) {
        survivor[i] = (u8)(0x77u ^ (u8)i);
        doomed[i] = (u8)(0x11u ^ (u8)i);
    }
    f_roots[0] = survivor;

    collect();

    ASSERT(is_live(f_roots[0]));
    for (usize i = 0; i < SIZE; i++) {
        if (survivor[i] != (u8)(0x77u ^ (u8)i)) {
            FAILm("the survivor's contents did not survive");
        }
    }

    ASSERT_EQ_FMT(0u, released_bytes_that_are_not_zero(), "%u");

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
    ASSERT_EQ_FMT(stats.blocks_before, stats.blocks_after, "%u");

    /* Most of the heap should be free after collecting 100 pieces of
     * garbage while keeping one object. */
    ASSERT(cvm_block_alloc_free_count(heap()->block_alloc) > 0);

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
    RUN_TEST(reclaimed_memory_is_zeroed_before_it_is_reused);
    RUN_TEST(a_partly_free_block_is_zeroed_where_it_is_free);
    RUN_TEST(a_spanning_object_survives_and_is_reclaimed);
    RUN_TEST(a_superblock_clears_the_chunk_beside_a_survivor);
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
