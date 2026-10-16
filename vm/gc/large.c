/*
 * vm/gc/large.c
 *
 * The large-object allocator. See vm/gc/large.h for the shape and why it
 * is chunk-based.
 */

#include "gc/large.h"

#include <stdatomic.h>

#include "base/assert.h"
#include "base/math.h"
#include "base/types.h"
#include "cvm/gc.h"
#include "gc/alloc.h"
#include "gc/block_alloc.h"
#include "context.h"
#include "gc/bytemap.h"
#include "gc/collect.h"
#include "gc/heap.h"
#include "gc/layout.h"
#include "gc/meta.h"
#include "gc/sweeper.h"
#include "log.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* size classes                                                        */
/* ------------------------------------------------------------------ */

/*
 * Which free list holds a chunk of this size.
 *
 * Sizes are rounded up to the large-object granularity first, so this is
 * an exact division: 8 KiB is index 0, 16 KiB index 1, 24 KiB index 2.
 */
static u32 size_to_list_index(usize size) {
    cvm_assert(size >= CVM_LARGE_OBJECT_MIN_SIZE);
    cvm_assert(cvm_is_aligned(size, CVM_LARGE_OBJECT_MIN_SIZE));
    return (u32)(size / CVM_LARGE_OBJECT_MIN_SIZE) - 1u;
}

static usize round_to_chunk(usize size) {
    return cvm_align_up(size, CVM_LARGE_OBJECT_MIN_SIZE);
}

/* ------------------------------------------------------------------ */
/* free lists                                                          */
/* ------------------------------------------------------------------ */

/*
 * Push and pop are compare-exchange loops over one word. The ABA argument
 * is the same one gc/block_list.c documents: a chunk is popped from a
 * given list at most once per phase and cannot return until it has been
 * swept, so the pop-then-push window cannot open. That argument has not
 * been re-verified against our sweeper, which does not exist yet, and it
 * must be checked when the sweeper is ported.
 */
static void free_list_push(CvmFreeList* list, CvmChunk* chunk) {
    usize head = atomic_load_explicit(&list->head, memory_order_relaxed);

    for (;;) {
        chunk->next = (CvmChunk*)head;
        if (atomic_compare_exchange_weak_explicit(&list->head, &head, (usize)chunk,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
            return;
        }
    }
}

static CvmChunk* free_list_pop(CvmFreeList* list) {
    usize head = atomic_load_explicit(&list->head, memory_order_acquire);

    for (;;) {
        if (head == 0) {
            return NULL;
        }
        CvmChunk* chunk = (CvmChunk*)head;
        usize next = (usize)chunk->next;

        if (atomic_compare_exchange_weak_explicit(
                &list->head, &head, next, memory_order_acquire, memory_order_acquire)) {
            return chunk;
        }
    }
}

/* ------------------------------------------------------------------ */
/* setup                                                               */
/* ------------------------------------------------------------------ */

/*
 * Point an allocator at the heap it serves.
 *
 * Nothing to initialize but the three pointers: the free lists belong to the
 * heap and are set up with it, because a mutator that is attaching is not
 * the only thing that touches them -- a GC worker sweeping a block files a
 * chunk without ever attaching. Initializing them here would make a thread's
 * attach wipe whatever another thread had already filed.
 */
void cvm_large_init(CvmLargeAllocator* large, CvmBlockAllocator* blocks,
                    Bytemap* bytemap, CvmHeap* heap) {
    large->bytemap = bytemap;
    large->block_alloc = blocks;
    large->heap = heap;
}

void cvm_large_clear(CvmHeap* heap) {
    for (u32 i = 0; i < CVM_LARGE_FREE_LIST_COUNT; i++) {
        atomic_store(&heap->large_free_lists[i].head, 0);
    }
}

void cvm_large_add_chunk(CvmHeap* heap, CvmChunk* chunk, usize total_size) {
    cvm_assert(total_size >= CVM_LARGE_OBJECT_MIN_SIZE);
    cvm_assert(total_size < CVM_BLOCK_SIZE);
    cvm_assert(cvm_is_aligned(total_size, CVM_LARGE_OBJECT_MIN_SIZE));

    chunk->size = total_size;

    /*
     * The chunk's first slot becomes a placeholder: something starts here,
     * but it is not a live object. That is what keeps the marker from
     * treating a free chunk as an object, and what makes the chunk's
     * interior invisible rather than looking like a crowd of objects.
     */
    object_meta_set_placeholder(bytemap_get(heap->bytemap, (u8 const*)chunk));

    free_list_push(&heap->large_free_lists[size_to_list_index(total_size)], chunk);
}

usize cvm_large_free_bytes(CvmHeap const* heap) {
    usize total = 0;
    for (u32 i = 0; i < CVM_LARGE_FREE_LIST_COUNT; i++) {
        /* Walking without popping: the list is only ever appended to by
         * sweepers between collections, so a snapshot is safe to read. */
        CvmChunk* chunk = (CvmChunk*)atomic_load_explicit(
            &((CvmHeap*)heap)->large_free_lists[i].head, memory_order_acquire);
        while (chunk != NULL) {
            total += chunk->size;
            chunk = chunk->next;
        }
    }
    return total;
}

u32 cvm_large_free_chunk_count(CvmHeap const* heap) {
    u32 count = 0;
    for (u32 i = 0; i < CVM_LARGE_FREE_LIST_COUNT; i++) {
        CvmChunk* chunk = (CvmChunk*)atomic_load_explicit(
            &((CvmHeap*)heap)->large_free_lists[i].head, memory_order_acquire);
        while (chunk != NULL) {
            count++;
            chunk = chunk->next;
        }
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* allocation                                                          */
/* ------------------------------------------------------------------ */

/*
 * Find a chunk of at least `size`, splitting it if it is too big.
 *
 * The search starts at the size class for `size` and goes up, because a
 * bigger chunk can be split but a smaller one cannot be grown. This is
 * the whole point of the segregated lists: the common case finds a chunk
 * in the first list it looks at.
 */
static CvmChunk* take_chunk(CvmLargeAllocator* large, usize size) {
    u32 start = size_to_list_index(size);

    /*
     * Always atomic: chunks are pushed onto these lists by sweepers without
     * the allocator's lock, so a non-atomic pop can lose one of their pushes
     * and hand the same chunk out twice.
     */
    for (u32 i = start; i < CVM_LARGE_FREE_LIST_COUNT; i++) {
        CvmChunk* chunk = free_list_pop(&large->heap->large_free_lists[i]);
        if (chunk == NULL) {
            continue;
        }

        /* The size must be read before the chunk is handed out, because
         * allocation overwrites the chunk header. */
        usize chunk_size = chunk->size;
        cvm_assert(chunk_size >= size);

        if (chunk_size > size) {
            CvmChunk* leftover = (CvmChunk*)((u8*)chunk + size);
            cvm_large_add_chunk(large->heap, leftover, chunk_size - size);
        }

        return chunk;
    }

    return NULL;
}

/*
 * Cut a chunk out of a freshly grown superblock.
 *
 * Every large object comes from one of these, a single block long for
 * anything that fits in a block -- and that is the point rather than an
 * implementation detail. A block obtained this way is marked as a superblock
 * by the block allocator, and both the marker and the sweeper read that mark:
 * the marker knows not to look for line marks in it, and the sweeper walks it
 * as chunks rather than as lines. Obtaining it as an ordinary block instead
 * -- which is what this did, and the reference does not -- leaves the sweeper
 * line-walking a block full of large objects, and leaves the chunk path in
 * cvm_sweep_superblock with no workload that reaches it.
 *
 * Whatever is left of the superblock after the object is cut off goes back on
 * the free lists, which is how one block comes to hold several large objects.
 * The remainder is always smaller than a block: the count was rounded up to
 * hold the object, so the object is more than blocks - 1 blocks long.
 */
static CvmChunk* take_chunk_from_superblock(CvmLargeAllocator* large, usize size) {
    u32 blocks = (u32)cvm_div_round_up(size, CVM_BLOCK_SIZE);

    BlockMeta* run = cvm_block_alloc_get_superblock(large->block_alloc, blocks);
    if (run == NULL) {
        return NULL;
    }

    u8* start = block_meta_block_start(large->heap->block_meta_start,
                                       large->heap->heap_start, run);
    CvmChunk* chunk = (CvmChunk*)start;

    usize total = (usize)blocks * CVM_BLOCK_SIZE;
    cvm_assert(total >= size);
    if (total - size >= CVM_LARGE_OBJECT_MIN_SIZE) {
        cvm_large_add_chunk(large->heap, (CvmChunk*)(start + size), total - size);
    }

    chunk->size = size;
    return chunk;
}

/* One attempt at a large object, without collecting. */
static u8* try_large_alloc(CvmLargeAllocator* large, usize size) {
    CvmChunk* chunk = NULL;

    /*
     * The free lists only hold chunks smaller than a block, because that is
     * what a chunk is; a block or more comes from a superblock. Trying the
     * lists for a bigger request would return a chunk that cannot hold it.
     */
    if (size < CVM_BLOCK_SIZE) {
        chunk = take_chunk(large, size);
    }

    if (chunk == NULL) {
        chunk = take_chunk_from_superblock(large, size);
        if (chunk == NULL) {
            return NULL;
        }
    }

    /*
     * The chunk is the object's start; the payload follows the header.
     *
     * No clearing here. A chunk is zero when it is handed out because the
     * sweeper cleared it when it was reclaimed (gc/sweeper.c), which is
     * what lets a large object's payload be promised zeroed without
     * clearing up to 8 KiB a byte at a time on the allocation path.
     */
    u8* start = (u8*)chunk;

    ObjectMeta* meta = bytemap_get(large->bytemap, start);
    object_meta_set_allocated(meta);

    return (opaque_mut)((CvmGcHeader*)start + 1);
}

/*
 * Write the header of a chunk that has just been taken, and hand back the
 * payload. Every successful attempt above ends here.
 */
static opaque_mut adopt_large_chunk(u8* payload, CvmTypeDescriptor const* type) {
    CvmGcHeader* header = cvm_header_of_mut(payload);
    header->type = type;

    /* A large object's finalizer is as real as a small one's. */
    if (type->finalizer != NULL) {
        cvm_note_finalizer(header);
    }
    return payload;
}

opaque_mut cvm_alloc_large(CvmTypeDescriptor const* type, usize size) {
    CvmThreadNode* node = cvm_thread_node();
    if (node == NULL) {
        rt_fatal(STR_LITERAL("large allocation from an unattached thread"));
    }

    CvmLargeAllocator* large = &node->large_allocator;
    CvmHeap* heap = large->heap;

    usize chunk_size = round_to_chunk(size);

    for (;;) {
        u8* payload = try_large_alloc(large, chunk_size);
        if (payload != NULL) {
            return adopt_large_chunk(payload, type);
        }

        /*
         * Reclaim before growing, the sequence gc/alloc.c runs and for the
         * same reason.
         *
         * This loop used to go straight to the growth step, with a note
         * saying where the collection should go. The consequence was not a
         * slow path but a fatal one: a large allocation that could not be
         * satisfied grew the heap, and when the heap was at its maximum it
         * called rt_fatal, with every byte of the garbage it had been asked
         * to allocate still sitting unreclaimed. A 9 KiB object against a
         * 32 MiB heap, all of it garbage, was enough to kill the process.
         */
        if (!cvm_sweep_is_done(heap)) {
            cvm_sweep_finish(heap, STR_LITERAL("large allocation failed"));
            payload = try_large_alloc(large, chunk_size);
            if (payload != NULL) {
                return adopt_large_chunk(payload, type);
            }
        }

        /* A false return means another thread collected instead, in which
         * case the heap has just been collected and retrying is right. */
        if (!cvm_gc_collect(heap)) {
            continue;
        }

        payload = try_large_alloc(large, chunk_size);
        if (payload != NULL) {
            return adopt_large_chunk(payload, type);
        }

        if (!cvm_sweep_is_done(heap)) {
            cvm_sweep_finish(heap, STR_LITERAL("large allocation failed"));
            payload = try_large_alloc(large, chunk_size);
            if (payload != NULL) {
                return adopt_large_chunk(payload, type);
            }
        }

        /*
         * Grow by enough whole blocks to hold the object, rounded up to a
         * power of two. The reference does the same, and the power-of-two
         * rounding is what keeps a sequence of growing large allocations
         * from producing a heap that is a long list of odd-sized runs.
         */
        u32 blocks = (u32)cvm_div_round_up(chunk_size, CVM_BLOCK_SIZE);
        u32 pow2 = 1u << cvm_log2_ceil(blocks);

        /*
         * How many blocks the heap actually takes, which is not always the
         * power of two that was asked for: the first call fails on a
         * reservation with less than pow2 left and blocks is what lands.
         * Filing fewer than were extended leaves the rest inside the heap
         * and on no free list, where the next sweep finds a block that is
         * neither free nor in use -- so the count filed has to be the count
         * added.
         */
        u32 grown = pow2;
        u32 first = 0;
        if (!cvm_heap_extend(heap, pow2, &first)) {
            grown = blocks;
            if (!cvm_heap_extend(heap, blocks, &first)) {
                rt_fatal(STR_LITERAL("out of heap space for a large object"));
            }
        }

        for (u32 i = 0; i < grown; i++) {
            BlockMeta* meta = cvm_heap_block_meta(heap, first + i);
            block_meta_set_debug_state(meta, CVM_DBG_FREE);
        }
        cvm_block_alloc_add_free_superblock(large->block_alloc,
                                            cvm_heap_block_meta(heap, first), grown);
    }
}
