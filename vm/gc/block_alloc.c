/*
 * vm/gc/block_alloc.c
 *
 * The global block allocator. See vm/gc/block_alloc.h for the shape and
 * the reasoning behind it.
 *
 * The debug state transitions mirror the reference's GC_ASSERTIONS
 * checks: a block that is handed out must have been free, and a block
 * that is freed must not be freed twice. Those two assertions are what
 * catch the class of bug where the sweeper and the allocator disagree
 * about who owns a block, which otherwise shows up much later as two
 * objects at the same address.
 */

#include "gc/block_alloc.h"

#include <stdatomic.h>

#include "base/assert.h"
#include "base/math.h"
#include "base/types.h"
#include "gc/block_list.h"
#include "gc/block_range.h"
#include "gc/layout.h"
#include "gc/meta.h"
#include "log.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/* Which free list holds runs of `size` blocks. */
static u32 size_to_list_index(u32 size) {
    cvm_assert(size > 0);
    cvm_assert(size <= (1u << (CVM_SUPERBLOCK_LIST_SIZE - 1)));
    return cvm_log2_floor(size);
}

static bool is_concurrent(CvmBlockAllocator* alloc) {
    return atomic_load_explicit(&alloc->concurrent, memory_order_relaxed);
}

static BlockMeta* pop_superblock(CvmBlockAllocator* alloc, u32* index) {
    /* Acquire everything the sweepers published before reading the lists. */
    atomic_thread_fence(memory_order_acquire);

    for (u32 i = *index; i < CVM_SUPERBLOCK_LIST_SIZE; i++) {
        BlockMeta* run = block_list_pop(&alloc->free_superblocks[i], alloc->meta_start);
        if (run != NULL) {
            *index = i;
            return run;
        }
    }
    return NULL;
}

static BlockMeta* pop_superblock_only_thread(CvmBlockAllocator* alloc, u32* index) {
    for (u32 i = *index; i < CVM_SUPERBLOCK_LIST_SIZE; i++) {
        BlockMeta* run =
            block_list_pop_only_thread(&alloc->free_superblocks[i], alloc->meta_start);
        if (run != NULL) {
            *index = i;
            return run;
        }
    }
    return NULL;
}

static BlockMeta* pop_superblock_mode(CvmBlockAllocator* alloc, u32* index) {
    return is_concurrent(alloc) ? pop_superblock(alloc, index)
                                : pop_superblock_only_thread(alloc, index);
}

/*
 * Split a run into power-of-two pieces and add each to its list.
 *
 * The split is by the binary representation of the count: a run of 13 is
 * 8 + 4 + 1, so three lists get one piece each. That is what keeps every
 * list uniform, and it is why the waste from rounding is bounded by the
 * largest power of two below the run length rather than by the run
 * length.
 */
static void split_and_add(CvmBlockAllocator* alloc, BlockMeta* run, u32 count) {
    u32 remaining = count;
    u32 power = 1;
    BlockMeta* current = run;

    while (remaining > 0) {
        if ((power & remaining) != 0) {
            block_list_push(&alloc->free_superblocks[size_to_list_index(power)],
                            alloc->meta_start, current);
            remaining -= power;
            current += power;
        }
        power <<= 1;
    }
}

/* Mark every block in a run free, ready to be handed out. */
static void clear_run(CvmBlockAllocator* alloc, BlockMeta* first, u32 count) {
    for (u32 i = 0; i < count; i++) {
        BlockMeta* meta = first + i;
        cvm_assert_msg(block_meta_debug_state(meta) == (u8)CVM_DBG_FREE,
                       "block freed twice, or freed while in use");
        block_meta_clear(meta);
        block_meta_set_debug_state(meta, CVM_DBG_FREE_IN_COLLECTION);
    }
    (void)alloc;
}

/*
 * Hand a block to a caller, recording that it is now in use.
 *
 * The state it must arrive in is "free, as part of a sweep": the free
 * lists are rebuilt by sweeping, so every block on them came from there.
 * A block that arrives in any other state was put on a free list without
 * being freed, which is the bug this check exists to catch — and a
 * check that only says "not the expected state" is much harder to act on
 * than one that says which block and what it was.
 */
static void take_for_use(BlockMeta* meta_start, BlockMeta* meta) {
    u8 state = block_meta_debug_state(meta);

    if (!block_meta_is_free(meta) || state != (u8)CVM_DBG_FREE_IN_COLLECTION) {
        CVM_LOG_ERROR(m) {
            fmt_lit(&m.fmt, "block ");
            fmt_u32(&m.fmt, block_meta_index_of(meta_start, meta));
            fmt_lit(&m.fmt, " handed out in state ");
            fmt_u32(&m.fmt, (u32)state);
            fmt_lit(&m.fmt, ", free=");
            fmt_bool(&m.fmt, block_meta_is_free(meta));
            fmt_lit(&m.fmt, " (expected free, state ");
            fmt_u32(&m.fmt, (u32)CVM_DBG_FREE_IN_COLLECTION);
            fmt_lit(&m.fmt, ")");
        }
        cvm_assert_msg(block_meta_is_free(meta),
                       "a block on a free list is not marked free");
        cvm_assert_msg(state == (u8)CVM_DBG_FREE_IN_COLLECTION,
                       "a block reached a free list without being freed");
    }

    block_meta_set_debug_state(meta, CVM_DBG_IN_USE);
    block_meta_set_flag(meta, CVM_BLOCK_SIMPLE);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

void cvm_block_alloc_acquire(CvmBlockAllocator* alloc) {
    rt_mutex_lock(&alloc->allocation_lock);
    atomic_thread_fence(memory_order_acquire);
}

void cvm_block_alloc_release(CvmBlockAllocator* alloc) {
    atomic_thread_fence(memory_order_release);
    rt_mutex_unlock(&alloc->allocation_lock);
}

void cvm_block_alloc_init(CvmBlockAllocator* alloc, BlockMeta* meta_start,
                          u32 block_count) {
    for (u32 i = 0; i < CVM_SUPERBLOCK_LIST_SIZE; i++) {
        block_list_init(&alloc->free_superblocks[i]);
    }

    alloc->meta_start = meta_start;

    /*
     * The reserve has to come out of the block count, and the assertion
     * below is the reference's: a heap with no room beyond the reserve
     * could never satisfy a request, because every block would be held
     * back for the sweeper.
     */
    cvm_assert(block_count > CVM_SWEEP_RESERVE_BLOCKS);

    alloc->smallest.cursor = meta_start;
    alloc->smallest.limit = meta_start + block_count - CVM_SWEEP_RESERVE_BLOCKS;
    atomic_store(&alloc->reserved, meta_start + block_count - CVM_SWEEP_RESERVE_BLOCKS);

    atomic_store(&alloc->concurrent, false);
    atomic_store(&alloc->free_block_count, block_count);
    cvm_block_range_clear(&alloc->coalescing);

    /*
     * The heap starts entirely free, and the debug state has to say so in
     * the form the hand-out path expects: a block that is free as part of
     * a collection. Without this the first allocation trips the assertion
     * in take_for_use, which is exactly what happened when this was
     * missed.
     */
    for (u32 i = 0; i < block_count; i++) {
        block_meta_set_debug_state(&meta_start[i], CVM_DBG_FREE_IN_COLLECTION);
    }

    if (!rt_mutex_init(&alloc->allocation_lock)) {
        rt_fatal(STR_LITERAL("could not create the block allocator lock"));
    }
}

void cvm_block_alloc_clear(CvmBlockAllocator* alloc) {
    for (u32 i = 0; i < CVM_SUPERBLOCK_LIST_SIZE; i++) {
        block_list_clear(&alloc->free_superblocks[i]);
    }

    /*
     * Sweeping is about to start, which means blocks will be freed while
     * mutators are running, so the free lists have to be manipulated
     * atomically from here until coalescing finishes.
     */
    atomic_store(&alloc->concurrent, true);
    atomic_store(&alloc->free_block_count, 0);
    alloc->smallest.cursor = NULL;
    alloc->smallest.limit = NULL;
    cvm_block_range_clear(&alloc->coalescing);
}

void cvm_block_alloc_finish_coalescing(CvmBlockAllocator* alloc) {
    atomic_store(&alloc->concurrent, false);
}

/* ------------------------------------------------------------------ */
/* allocation                                                          */
/* ------------------------------------------------------------------ */

/*
 * Slow path: the cursor run is exhausted, so look in the free lists and
 * then in the run being coalesced.
 */
static BlockMeta* get_free_block_slow(CvmBlockAllocator* alloc) {
    u32 index = 0;
    BlockMeta* run = pop_superblock_mode(alloc, &index);

    if (run != NULL) {
        /* The rest of the run becomes the new cursor. */
        alloc->smallest.cursor = run + 1;
        alloc->smallest.limit = run + (1u << index);

        take_for_use(alloc->meta_start, run);
        atomic_fetch_sub_explicit(&alloc->free_block_count, 1, memory_order_relaxed);
        return run;
    }

    /*
     * Last resort: the run the sweeper is still building. A mutator that
     * gets here is taking blocks out from under the sweeper, which is
     * fine — the sweeper only ever extends a run forward, so a block
     * taken from its front is already accounted for.
     */
    BlockMeta* block = NULL;
    if (is_concurrent(alloc)) {
        u32 idx = cvm_block_range_poll_first(&alloc->coalescing, 1);
        if (idx != CVM_NO_BLOCK_INDEX) {
            block = block_meta_at(alloc->meta_start, idx);
        }
    } else {
        CvmBlockRangeVal range = atomic_load(&alloc->coalescing);
        atomic_store(&alloc->coalescing, CVM_EMPTY_RANGE);
        u32 size = cvm_block_range_size(range);
        if (size > 0) {
            u32 idx = cvm_block_range_first(range);
            block = block_meta_at(alloc->meta_start, idx);
            alloc->smallest.cursor = block + 1;
            alloc->smallest.limit = block + size;
        }
    }

    if (block != NULL) {
        take_for_use(alloc->meta_start, block);
        atomic_fetch_sub_explicit(&alloc->free_block_count, 1, memory_order_relaxed);
    }
    return block;
}

BlockMeta* cvm_block_alloc_get_block(CvmBlockAllocator* alloc) {
    cvm_block_alloc_acquire(alloc);

    if (alloc->smallest.cursor == NULL ||
        alloc->smallest.cursor >= alloc->smallest.limit) {
        BlockMeta* block = get_free_block_slow(alloc);
        cvm_block_alloc_release(alloc);
        return block;
    }

    BlockMeta* block = alloc->smallest.cursor;
    take_for_use(alloc->meta_start, block);
    alloc->smallest.cursor++;

    cvm_block_alloc_release(alloc);

    /* The count is not decremented on this path in the reference either:
     * it is only read after a sweep, when the cursor has been reset. */
    return block;
}

BlockMeta* cvm_block_alloc_get_superblock(CvmBlockAllocator* alloc, u32 size) {
    cvm_assert(size > 0);

    cvm_block_alloc_acquire(alloc);

    BlockMeta* run = NULL;
    BlockMeta* cursor = alloc->smallest.cursor;
    BlockMeta* limit = alloc->smallest.limit;

    if (cursor != NULL && (u32)(limit - cursor) >= size) {
        /* The cursor run is big enough; carve the front off it. */
        alloc->smallest.cursor += size;
        run = cursor;
    } else {
        u32 index = cvm_log2_ceil(size);
        run = pop_superblock_mode(alloc, &index);
        u32 received = 1u << index;

        if (run != NULL && received > size) {
            /* Too big by a factor: give the tail back. */
            split_and_add(alloc, run + size, received - size);
        } else if (run == NULL) {
            u32 idx = cvm_block_range_poll_first(&alloc->coalescing, size);
            if (idx != CVM_NO_BLOCK_INDEX) {
                run = block_meta_at(alloc->meta_start, idx);
            }
        }

        if (run == NULL) {
            cvm_block_alloc_release(alloc);
            return NULL;
        }
    }

    cvm_assert(block_meta_is_free(run));
    block_meta_set_debug_state(run, CVM_DBG_IN_USE);
    block_meta_set_flag_and_size(run, CVM_BLOCK_SUPERBLOCK_START, (i32)size);

    for (u32 i = 1; i < size; i++) {
        BlockMeta* tail = run + i;
        cvm_assert(block_meta_is_free(tail));
        block_meta_set_debug_state(tail, CVM_DBG_IN_USE);
        block_meta_set_flag(tail, CVM_BLOCK_SUPERBLOCK_TAIL);
    }

    cvm_block_alloc_release(alloc);

    atomic_fetch_sub_explicit(&alloc->free_block_count, size, memory_order_relaxed);
    return run;
}

/* ------------------------------------------------------------------ */
/* freeing                                                             */
/* ------------------------------------------------------------------ */

void cvm_block_alloc_add_free_blocks(CvmBlockAllocator* alloc, BlockMeta* first,
                                     u32 count) {
    cvm_assert(count > 0);

    clear_run(alloc, first, count);

    /* Make the sweep's changes visible before the run is published. */
    atomic_thread_fence(memory_order_release);

    u32 first_index = block_meta_index_of(alloc->meta_start, first);
    CvmBlockRangeVal displaced =
        cvm_block_range_append_last_or_replace(&alloc->coalescing, first_index, count);

    u32 displaced_size = cvm_block_range_size(displaced);
    if (displaced_size > 0) {
        BlockMeta* displaced_first =
            block_meta_at(alloc->meta_start, cvm_block_range_first(displaced));
        split_and_add(alloc, displaced_first, displaced_size);
    }

    atomic_fetch_add_explicit(&alloc->free_block_count, count, memory_order_relaxed);
}

void cvm_block_alloc_add_free_superblock(CvmBlockAllocator* alloc, BlockMeta* first,
                                         u32 count) {
    clear_run(alloc, first, count);
    split_and_add(alloc, first, count);
    atomic_fetch_add_explicit(&alloc->free_block_count, count, memory_order_relaxed);
}

/* Split a run into power-of-two pieces, staged on local lists. */
static void split_and_add_local(CvmBlockAllocator* alloc, LocalBlockList* lists,
                                BlockMeta* run, u32 count) {
    u32 remaining = count;
    u32 power = 1;
    BlockMeta* current = run;

    while (remaining > 0) {
        if ((power & remaining) != 0) {
            local_block_list_push(&lists[size_to_list_index(power)], alloc->meta_start,
                                  current);
            remaining -= power;
            current += power;
        }
        power <<= 1;
    }
}

void cvm_block_alloc_add_free_superblock_local(CvmBlockAllocator* alloc,
                                               LocalBlockList* lists, BlockMeta* first,
                                               u32 count) {
    cvm_assert(count > 0);

    clear_run(alloc, first, count);
    split_and_add_local(alloc, lists, first, count);
    atomic_fetch_add_explicit(&alloc->free_block_count, count, memory_order_relaxed);
}

/* ------------------------------------------------------------------ */
/* the sweeper's reserve                                               */
/* ------------------------------------------------------------------ */

void cvm_block_alloc_reserve_blocks(CvmBlockAllocator* alloc) {
    cvm_block_alloc_acquire(alloc);

    u32 index = cvm_log2_ceil(CVM_SWEEP_RESERVE_BLOCKS);
    BlockMeta* run = pop_superblock(alloc, &index);
    u32 received = 1u << index;

    if (run != NULL && received > CVM_SWEEP_RESERVE_BLOCKS) {
        split_and_add(alloc, run + CVM_SWEEP_RESERVE_BLOCKS,
                      received - CVM_SWEEP_RESERVE_BLOCKS);
    } else if (run == NULL) {
        u32 idx =
            cvm_block_range_poll_first(&alloc->coalescing, CVM_SWEEP_RESERVE_BLOCKS);
        if (idx != CVM_NO_BLOCK_INDEX) {
            run = block_meta_at(alloc->meta_start, idx);
        }
    }

    atomic_store(&alloc->reserved, run);
    cvm_block_alloc_release(alloc);
}

void cvm_block_alloc_use_reserve(CvmBlockAllocator* alloc) {
    cvm_block_alloc_acquire(alloc);

    BlockMeta* reserved = atomic_load_explicit(&alloc->reserved, memory_order_relaxed);
    if (reserved != NULL) {
        split_and_add(alloc, reserved, CVM_SWEEP_RESERVE_BLOCKS);
    }

    cvm_block_alloc_release(alloc);
}
