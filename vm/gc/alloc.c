/*
 * vm/gc/alloc.c
 *
 * The allocation fallback path. See vm/gc/alloc.h for the shape.
 *
 * The structure follows the reference closely, because the sequence it
 * encodes is not arbitrary:
 *
 *   1. try the current bump region (a line, or the tail of one)
 *   2. try a recycled block the sweeper gave us
 *   3. try the global free lists
 *   4. if sweeping is still in progress, help sweep, then retry
 *   5. collect, then retry
 *   6. grow the heap, then retry
 *
 * The order matters for latency. Steps 2 and 3 are cheap and often
 * succeed; step 4 does work rather than waiting; only then does the
 * thread stop the world. A thread that jumped straight to step 5 would
 * collect constantly under a load that a little sweeping would have
 * absorbed.
 */

#include "gc/alloc.h"

#include <stdatomic.h>

#include "base/assert.h"
#include "base/math.h"
#include "base/types.h"
#include "cvm/gc.h"
#include "gc/block_alloc.h"
#include "gc/block_list.h"
#include "gc/bytemap.h"
#include "gc/collect.h"
#include "gc/sweeper.h"
#include "gc/heap.h"
#include "gc/meta.h"
#include "context.h"
#include "log.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* setup                                                               */
/* ------------------------------------------------------------------ */

void cvm_allocator_init(CvmAllocator* alloc, CvmBlockAllocator* blocks,
                        Bytemap* bytemap, CvmHeap* heap) {
    alloc->bytemap = bytemap;
    alloc->block_alloc = blocks;
    alloc->heap = heap;

    block_list_init(&alloc->recycled_blocks);
    atomic_store(&alloc->recycled_block_count, 0);

    cvm_allocator_clear(alloc, cvm_ctx());
}

void cvm_allocator_clear(CvmAllocator* alloc, CvmVMContext* ctx) {
    block_list_clear(&alloc->recycled_blocks);
    atomic_store(&alloc->recycled_block_count, 0);

    alloc->block = NULL;
    alloc->block_start = NULL;
    alloc->large_block = NULL;
    alloc->large_block_start = NULL;
    alloc->large_cursor = NULL;
    alloc->large_limit = NULL;

    /*
     * The bump cursor lives in the context, not in this struct, so clearing
     * one without the other leaves the thread allocating into a block the
     * sweeper is about to hand to somebody else. The context arrives as a
     * parameter because this runs for other threads; see gc/alloc.h.
     */
    if (ctx != NULL) {
        ctx->bump_ptr = NULL;
        ctx->bump_end = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* finishing an allocation                                             */
/* ------------------------------------------------------------------ */

/*
 * Publish an object: zero it, write its header, mark its metadata.
 *
 * `start` is the object's start, which is where the header goes — not the
 * payload. See the address convention note in cvm/gc.h. Treating the two
 * as interchangeable is how the header ended up written outside its own
 * object, and how the bytemap came to describe a slot one header away
 * from the real one.
 *
 * Zeroing is part of the contract: generated code assumes a fresh object
 * has zeroed fields, which is what lets a type with pointer fields start
 * out valid rather than full of rubbish that the marker would follow.
 */
static opaque_mut publish(CvmAllocator* alloc, u8* start, usize size,
                          CvmTypeDescriptor const* type) {
    cvm_assert(cvm_heap_contains(alloc->heap, start));
    cvm_assert(cvm_is_aligned((usize)start, CVM_ALLOC_ALIGNMENT));

    for (usize i = 0; i < size; i++) {
        start[i] = 0;
    }

#if defined(CVM_DEBUG)
    /*
     * Every slot this allocation covers must have been free. If one was
     * not, either the free-line list handed out a line that is still in
     * use, or two threads are bumping into the same region — both of which
     * corrupt silently and show up much later as an object whose contents
     * changed underneath it.
     *
     * Not applied to large objects. Their interior slots are deliberately
     * left free — a precise collector never produces an interior pointer,
     * so an interior slot that reads free is not a claim that the memory
     * is available. Large objects are tracked by the large allocator's free
     * lists instead.
     *
     * Scope, because this reads as though it covered every allocation and
     * does not: it runs on the slow path only. The inlined path in cvm/gc.h
     * moves the bump cursor and writes the header itself, and cvm/gc.h does
     * not include gc/meta.h — a deliberate piece of layering, since gc.h is
     * what generated code pulls in. So most allocations, being fast-path,
     * are not validated here. Extending the check to them means making the
     * slot predicate reachable from the freestanding header, which is a
     * coupling worth making only if a bug calls for it.
     *
     * Confirmed to fire: marking a slot as allocated that the next
     * allocation will cover trips it with the slot's index. It only covers
     * the slow path, and it says so here rather than reading as more.
     */
    object_meta_assert_valid_allocation(bytemap_get(alloc->bytemap, start), size);
#endif

    CvmGcHeader* header = (CvmGcHeader*)start;
    header->type = type;
    header->flags = 0;

    ObjectMeta* meta = bytemap_get(alloc->bytemap, start);
    object_meta_set_allocated(meta);

    return (opaque_mut)(header + 1); /* the payload */
}

/* ------------------------------------------------------------------ */
/* lines                                                               */
/* ------------------------------------------------------------------ */

/*
 * Point the bump region at the next free line.
 *
 * Returns false only when there is no space at all: an exhausted block is
 * not a failure, it is a reason to fetch another one. Returning false
 * there was a bug — the allocator would declare the heap full while the
 * free lists were untouched, and the retry loop would grow the heap until
 * the reservation ran out.
 *
 * Note the mutual recursion with allocator_new_block, which is the same
 * shape the reference has: each knows how to fall back to the other, and
 * new_block never calls back into here.
 */
static bool allocator_next_line(CvmAllocator* alloc);

static bool allocator_new_block(CvmAllocator* alloc) {
    BlockMeta* block;

    /*
     * Always atomic, and the reason is not caution.
     *
     * This list is filled by *other* threads: the sweeper pushes whole runs
     * of partially free blocks into whichever mutator it chose to give them
     * to, and it does so without the block allocator's lock. So a pop here
     * can run concurrently with a push from a thread that holds nothing.
     *
     * This used to branch on one heap-wide flag saying whether sweeping was
     * in progress, and pop non-atomically when it was not. That is safe only
     * if the flag is exact, and it is not: a sweeper can still be inside a
     * batch when another thread concludes the sweep is over and clears the
     * flag. A non-atomic pop racing an atomic push loses an update, and the
     * same block is then handed to two threads — which a multi-threaded
     * stress test caught, as two threads writing into overlapping object
     * memory.
     *
     * The cost of always being atomic is one compare-exchange per block
     * adopted, which is once per 32 KiB of allocation.
     */
    block = block_list_pop(&alloc->recycled_blocks, alloc->heap->block_meta_start);

    CvmVMContext* ctx = cvm_ctx();
    u8* block_start;

    if (block == NULL) {
        /*
         * Own list empty: fall back to the shared stack of worker-swept
         * blocks. Same pop discipline as above — the stack is filled by
         * threads that hold nothing, so this is always an atomic pop.
         */
        block = block_list_pop(&alloc->block_alloc->recycled_shared,
                               alloc->heap->block_meta_start);
        if (block != NULL) {
            atomic_fetch_sub_explicit(&alloc->block_alloc->recycled_shared_count, 1,
                                      memory_order_relaxed);
        }
    }

    if (block != NULL) {
        atomic_fetch_sub_explicit(&alloc->recycled_block_count, 1,
                                  memory_order_relaxed);
        /* Take in everything the sweeper wrote into this block. */
        atomic_thread_fence(memory_order_acquire);

        cvm_assert(block_meta_is_simple(block));

        /*
         * A block reaches a mutator's recycled list only through the
         * sweeper, which marks it partially free on the way. So that is the
         * state it must still be in: anything else means it was handed out
         * already, and two threads are about to bump their way through the
         * same memory.
         */
        cvm_assert_msg(block_meta_debug_state(block) == (u8)CVM_DBG_PARTIAL_FREE,
                       "a recycled block was handed to a second owner");

        block_meta_set_debug_state(block, CVM_DBG_IN_USE);

        block_start = block_meta_block_start(alloc->heap->block_meta_start,
                                             alloc->heap->heap_start, block);

        /* A recycled block has holes, so it comes with a free-line list
         * and the bump region starts at its first free line rather than
         * at the block start. */
        i8 line_index = block_meta_first_free_line(block);
        cvm_assert((i32)line_index >= 0 && (i32)line_index < CVM_LINE_INDEX_LIMIT);

        u8* line = block_line_address(block_start, (u32)line_index);
        FreeLineMeta* meta = (FreeLineMeta*)line;
        cvm_assert(meta->size > 0);

        block_meta_set_first_free_line(block, meta->next);
        ctx->bump_ptr = line;
        ctx->bump_end = line + (usize)meta->size * CVM_LINE_SIZE;
        cvm_assert(ctx->bump_end <= block_end(block_start));
    } else {
        block = cvm_block_alloc_get_block(alloc->block_alloc);
        if (block == NULL) {
            return false;
        }

        block_start = block_meta_block_start(alloc->heap->block_meta_start,
                                             alloc->heap->heap_start, block);

        /* A fresh block is entirely free: the whole thing becomes the
         * bump region, and the free-line list is empty. */
        ctx->bump_ptr = block_start;
        ctx->bump_end = block_end(block_start);
        block_meta_set_first_free_line(block, CVM_LAST_HOLE);
    }

    alloc->block = block;
    alloc->block_start = block_start;
    return true;
}

static bool allocator_next_line(CvmAllocator* alloc) {
    BlockMeta* block = alloc->block;
    if (block == NULL) {
        return allocator_new_block(alloc);
    }

    i8 line_index = block_meta_first_free_line(block);
    if (line_index == CVM_LAST_HOLE) {
        /* This block is used up; the next one may not be. */
        return allocator_new_block(alloc);
    }

    u8* line = block_line_address(alloc->block_start, (u32)line_index);
    FreeLineMeta* meta = (FreeLineMeta*)line;

    if (meta->size == 0) {
        return allocator_new_block(alloc);
    }

    block_meta_set_first_free_line(block, meta->next);

    CvmVMContext* ctx = cvm_ctx();
    ctx->bump_ptr = line;
    ctx->bump_end = line + (usize)meta->size * CVM_LINE_SIZE;

    cvm_assert(ctx->bump_end <= block_end(alloc->block_start));
    return true;
}

/* ------------------------------------------------------------------ */
/* medium objects                                                      */
/* ------------------------------------------------------------------ */

/*
 * A whole free block for an object that is bigger than a line but
 * smaller than the large-object threshold.
 *
 * Only free blocks are used, never recycled ones: a recycled block has
 * holes, and the object needs contiguity.
 */
static bool allocator_new_overflow_block(CvmAllocator* alloc) {
    BlockMeta* block = cvm_block_alloc_get_block(alloc->block_alloc);
    if (block == NULL) {
        return false;
    }

    u8* block_start = block_meta_block_start(alloc->heap->block_meta_start,
                                             alloc->heap->heap_start, block);

    alloc->large_block = block;
    alloc->large_block_start = block_start;
    alloc->large_cursor = block_start;
    alloc->large_limit = block_end(block_start);
    return true;
}

static u8* allocator_overflow_alloc(CvmAllocator* alloc, usize size) {
    u8* start = alloc->large_cursor;

    if (start == NULL || (usize)(alloc->large_limit - start) < size) {
        if (!allocator_new_overflow_block(alloc)) {
            return NULL;
        }
        return allocator_overflow_alloc(alloc, size);
    }

    alloc->large_cursor = start + size;
    return start;
}

/* ------------------------------------------------------------------ */
/* the retry loop                                                      */
/* ------------------------------------------------------------------ */

/*
 * Try to satisfy an allocation without collecting. Returns NULL if it
 * cannot.
 */
static u8* try_alloc(CvmAllocator* alloc, usize size) {
    CvmVMContext* ctx = cvm_ctx();

    if (ctx->bump_ptr == NULL) {
        if (!allocator_new_block(alloc)) {
            return NULL;
        }
    }

    u8* start = ctx->bump_ptr;
    u8* end = start + size;

    if (end <= ctx->bump_end) {
        ctx->bump_ptr = end;
        return start;
    }

    if (size > CVM_LINE_SIZE) {
        /*
         * Too big for a line, so a new line will not help. Use a whole
         * free block instead.
         */
        return allocator_overflow_alloc(alloc, size);
    }

    if (allocator_next_line(alloc)) {
        return try_alloc(alloc, size);
    }
    return NULL;
}

/*
 * The allocation retry loop.
 *
 * The sequence, and why it is this order:
 *
 *   1. try to allocate
 *   2. if a sweep is in progress, sweep a batch and try again
 *   3. collect, then try again
 *   4. grow the heap
 *
 * Step 2 is what makes sweeping concurrent pay off. A thread that cannot
 * allocate does a little of the sweeping that is holding it back, rather
 * than blocking until someone else finishes; and because it sweeps the
 * same blocks the sweep is working through, an allocation-heavy thread
 * speeds up its own progress. Jumping straight to step 3 would collect
 * constantly under a load that a batch or two of sweeping would have
 * absorbed.
 *
 * Step 4 is the last resort: a collection ran and there is still no room,
 * so the heap is genuinely too small. One block is always enough for a
 * small object — it is at most a line, and a block holds 128 lines — so
 * growing by one is always sufficient to make progress.
 */
opaque_mut cvm_alloc_slow(CvmTypeDescriptor const* type, usize size) {
    /*
     * Objects at or above the large-object threshold are chunk-allocated,
     * not line-allocated. Dispatched here rather than in the inlined path
     * because a large object always fails the bump check anyway, so the
     * branch costs nothing on the hot path.
     */
    if (size >= CVM_LARGE_OBJECT_MIN_SIZE) {
        return cvm_alloc_large(type, size);
    }

    CvmThreadNode* node = cvm_thread_node();
    if (node == NULL) {
        rt_fatal(STR_LITERAL("allocation from a thread that never attached"));
    }

    CvmAllocator* alloc = &node->allocator;
    CvmHeap* heap = alloc->heap;

    cvm_thread_set_activity(CVM_ACT_ALLOCATING);

    for (;;) {
        u8* payload = try_alloc(alloc, size);
        if (payload != NULL) {
            return publish(alloc, payload, size, type);
        }

        /* (2) Help sweep, if a sweep is in progress.
         *
         * To completion, not one batch. commix waits for the sweep here too:
         * a thread that cannot allocate has nothing better to do than finish
         * the sweep that is holding it back, and sweeping one batch and then
         * marching on to "grow the heap" is exactly how a heap with plenty of
         * reclaimable memory reports itself out of space. */
        if (!cvm_sweep_is_done(heap)) {
            cvm_sweep_finish(heap, STR_LITERAL("allocation failed"));
            payload = try_alloc(alloc, size);
            if (payload != NULL) {
                return publish(alloc, payload, size, type);
            }
        }

        /* (3) Collect. A false return means someone else did, in which
         * case the heap has just been collected and retrying is right. */
        if (!cvm_gc_collect(heap)) {
            continue;
        }

        payload = try_alloc(alloc, size);
        if (payload != NULL) {
            return publish(alloc, payload, size, type);
        }

        if (!cvm_sweep_is_done(heap)) {
            cvm_sweep_finish(heap, STR_LITERAL("allocation failed"));
            payload = try_alloc(alloc, size);
            if (payload != NULL) {
                return publish(alloc, payload, size, type);
            }
        }

        /* (4) Grow. */
        u32 first = 0;
        if (!cvm_heap_extend(heap, 1, &first)) {
            rt_fatal(STR_LITERAL("out of heap space"));
        }

        BlockMeta* fresh = cvm_heap_block_meta(heap, first);
        block_meta_set_debug_state(fresh, CVM_DBG_FREE);
        cvm_block_alloc_add_free_superblock(alloc->block_alloc, fresh, 1);
    }
}
