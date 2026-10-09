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

void cvm_large_init(CvmLargeAllocator* large, CvmBlockAllocator* blocks,
                    Bytemap* bytemap, CvmHeap* heap) {
    large->bytemap = bytemap;
    large->block_alloc = blocks;
    large->heap = heap;

    for (u32 i = 0; i < CVM_LARGE_FREE_LIST_COUNT; i++) {
        atomic_store(&large->free_lists[i].head, 0);
    }
}

void cvm_large_clear(CvmLargeAllocator* large) {
    for (u32 i = 0; i < CVM_LARGE_FREE_LIST_COUNT; i++) {
        atomic_store(&large->free_lists[i].head, 0);
    }
}

void cvm_large_add_chunk(CvmLargeAllocator* large, CvmChunk* chunk, usize total_size) {
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
    object_meta_set_placeholder(bytemap_get(large->bytemap, (u8 const*)chunk));

    free_list_push(&large->free_lists[size_to_list_index(total_size)], chunk);
}

usize cvm_large_free_bytes(CvmLargeAllocator const* large) {
    usize total = 0;
    for (u32 i = 0; i < CVM_LARGE_FREE_LIST_COUNT; i++) {
        /* Walking without popping: the list is only ever appended to by
         * sweepers between collections, so a snapshot is safe to read. */
        CvmChunk* chunk = (CvmChunk*)atomic_load_explicit(
            &((CvmLargeAllocator*)large)->free_lists[i].head, memory_order_acquire);
        while (chunk != NULL) {
            total += chunk->size;
            chunk = chunk->next;
        }
    }
    return total;
}

u32 cvm_large_free_chunk_count(CvmLargeAllocator const* large) {
    u32 count = 0;
    for (u32 i = 0; i < CVM_LARGE_FREE_LIST_COUNT; i++) {
        CvmChunk* chunk = (CvmChunk*)atomic_load_explicit(
            &((CvmLargeAllocator*)large)->free_lists[i].head, memory_order_acquire);
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
        CvmChunk* chunk = free_list_pop(&large->free_lists[i]);
        if (chunk == NULL) {
            continue;
        }

        /* The size must be read before the chunk is handed out, because
         * allocation overwrites the chunk header. */
        usize chunk_size = chunk->size;
        cvm_assert(chunk_size >= size);

        if (chunk_size > size) {
            CvmChunk* leftover = (CvmChunk*)((u8*)chunk + size);
            cvm_large_add_chunk(large, leftover, chunk_size - size);
        }

        return chunk;
    }

    return NULL;
}

/*
 * Cut a chunk out of a fresh block.
 *
 * The block is one contiguous run; the front becomes the requested chunk
 * and everything after it goes back on the free lists in large-object
 * multiples. Anything left over that is too small to be a chunk is
 * wasted, which is bounded by one granularity unit per block.
 */
static CvmChunk* take_chunk_from_new_block(CvmLargeAllocator* large, usize size) {
    BlockMeta* block = cvm_block_alloc_get_block(large->block_alloc);
    if (block == NULL) {
        return NULL;
    }

    u8* block_start = block_meta_block_start(large->heap->block_meta_start,
                                             large->heap->heap_start, block);
    CvmChunk* chunk = (CvmChunk*)block_start;

    usize remaining = CVM_BLOCK_SIZE - size;
    if (remaining >= CVM_LARGE_OBJECT_MIN_SIZE) {
        usize usable =
            (remaining / CVM_LARGE_OBJECT_MIN_SIZE) * CVM_LARGE_OBJECT_MIN_SIZE;
        CvmChunk* rest = (CvmChunk*)(block_start + size);
        cvm_large_add_chunk(large, rest, usable);
    }

    chunk->size = size;
    return chunk;
}

/* One attempt, without collecting. Returns NULL if there is no room. */
static u8* try_large_alloc(CvmLargeAllocator* large, usize size) {
    CvmChunk* chunk = take_chunk(large, size);

    if (chunk == NULL) {
        /*
         * A chunk of the requested size always fits in one block: the
         * request is below the block size, and rounding up to the
         * granularity keeps it there. So a fresh block can always serve
         * it.
         */
        chunk = take_chunk_from_new_block(large, size);
        if (chunk == NULL) {
            return NULL;
        }
    }

    /*
     * The chunk is the object's start; the payload follows the header.
     *
     * No clearing here, and this is the loop that cost the most: a large
     * object is up to 8 KiB, so this was the one place in the runtime that
     * cleared thousands of bytes per call, and it cleared them a byte at a
     * time -- freestanding, the compiler will not turn that into a memset.
     * The medium-object workload measured 6x slower in a freestanding build
     * than in the hosted one because of this loop and the one publish used
     * to have. Both are gone; a chunk is zero when it is handed out because
     * the sweeper cleared it when it was reclaimed (gc/sweeper.c).
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
    header->flags = 0;
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

        u32 first = 0;
        if (!cvm_heap_extend(heap, pow2, &first) &&
            !cvm_heap_extend(heap, blocks, &first)) {
            rt_fatal(STR_LITERAL("out of heap space for a large object"));
        }

        for (u32 i = 0; i < blocks; i++) {
            BlockMeta* meta = cvm_heap_block_meta(heap, first + i);
            block_meta_set_debug_state(meta, CVM_DBG_FREE);
        }
        cvm_block_alloc_add_free_superblock(large->block_alloc,
                                            cvm_heap_block_meta(heap, first), blocks);
    }
}
