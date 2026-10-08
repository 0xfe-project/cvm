/*
 * vm/gc/block_alloc.h
 *
 * The global block allocator, ported from refs/commix/BlockAllocator.{h,c}.
 *
 * Blocks are handed out one at a time (for small objects, which the
 * per-thread allocator then carves into lines) or as a run of contiguous
 * blocks (a superblock, for objects too large for one block).
 *
 * ---------------------------------------------------------------------
 * Power-of-two free lists
 * ---------------------------------------------------------------------
 *
 * Free runs are kept in one list per power of two, indexed by the
 * exponent: list 0 holds single blocks, list 1 holds pairs, list 5 holds
 * runs of 32, and so on. A run that is not a power of two long is split
 * into powers of two when it is freed, so a request for 2^n blocks can be
 * served from the first list at or above n.
 *
 * This is why the lists are the shape they are. A single list sorted by
 * size would need a search; a single unsorted list would need a scan to
 * find a run big enough. The power-of-two split means every list is
 * uniform, so finding a run is an index computation and taking the first
 * element, and the only waste is the rounding up to the next power of two.
 *
 * ---------------------------------------------------------------------
 * Two data structures, two purposes
 * ---------------------------------------------------------------------
 *
 * `smallest` is a cursor into a run of free blocks, used one block at a
 * time. It is the fast path for small-object allocation, and it is only
 * ever touched under the allocator lock.
 *
 * `coalescing` is the run currently being assembled by the sweeper. The
 * sweeper frees blocks in ascending address order, so consecutive frees
 * usually extend one run rather than creating many small ones. When the
 * run is displaced — because the sweeper reached a block that does not
 * abut it — the displaced run is split into power-of-two pieces and added
 * to the free lists.
 *
 * ---------------------------------------------------------------------
 * Concurrent and single-threaded modes
 * ---------------------------------------------------------------------
 *
 * During sweeping, mutators are running and blocks are being freed, so
 * the free lists are manipulated atomically. Outside that window only one
 * thread is in the allocator, and the atomic operations are pure
 * overhead, so `concurrent` selects the plain versions. Getting the flag
 * wrong is a correctness bug in one direction and a slowdown in the
 * other, which is why it is set at exactly two points: cleared when
 * sweeping starts, cleared again when coalescing finishes.
 */

#ifndef CVM_GC_BLOCK_ALLOC_H
#define CVM_GC_BLOCK_ALLOC_H

#include <stdatomic.h>

#include "base/types.h"
#include "gc/block_list.h"
#include "gc/block_range.h"
#include "gc/layout.h"
#include "gc/meta.h"
#include "rt.h"

/* One list per power of two, from 1 block up to a whole heap. */
#define CVM_SUPERBLOCK_LIST_SIZE (CVM_BLOCK_COUNT_BITS + 1)

typedef struct CvmBlockAllocator {
    /*
     * Cursor into a run of free blocks, consumed one block at a time.
     * Only touched under the allocator lock.
     */
    struct {
        BlockMeta* cursor;
        BlockMeta* limit;
    } smallest;

    _Atomic u32 free_block_count;

    /* The run the sweeper is currently extending. */
    CvmBlockRange coalescing;

    BlockMeta* meta_start;

    /*
     * Whether a sweep is in progress. Informational only.
     *
     * It used to select between atomic and non-atomic pops, which is safe
     * only if it is exact — and it cannot be, because a sweeper can still be
     * inside a batch when another thread concludes the sweep is over. The
     * pops are always atomic now, so nothing reads this to make a decision.
     * It is kept because it costs a byte and says something true about the
     * heap's state to anyone reading a core dump.
     */
    _Atomic bool concurrent;

    BlockList free_superblocks[CVM_SUPERBLOCK_LIST_SIZE];

    /*
     * Partially free blocks swept by a thread with no allocator of its own.
     *
     * A GC worker sweeping a batch has no recycled_blocks list, and handing
     * its blocks to a mutator means touching that mutator's node after the
     * registry's read lock was released — the window in which the thread
     * leaves and its node is freed underneath the write. So worker-swept
     * blocks go here instead: one lock-free stack, and any mutator whose own
     * list is empty takes from it. The free-line lists live in the blocks'
     * own metadata, so a block popped from here is as usable as one from a
     * per-thread list.
     */
    BlockList recycled_shared;
    _Atomic u32 recycled_shared_count;

    /* A run held back so the sweeper always has somewhere to work. */
    _Atomic(BlockMeta*) reserved;

    RtMutex allocation_lock;
} CvmBlockAllocator;

/* Set up the allocator over `block_count` blocks starting at meta_start. */
void cvm_block_alloc_init(CvmBlockAllocator* alloc, BlockMeta* meta_start,
                          u32 block_count);

/* Take one block. Returns NULL when there is nothing free. */
BlockMeta* cvm_block_alloc_get_block(CvmBlockAllocator* alloc);

/* Take `size` contiguous blocks, or NULL. */
BlockMeta* cvm_block_alloc_get_superblock(CvmBlockAllocator* alloc, u32 size);

/*
 * Give a run of blocks back.
 *
 * add_free_blocks is the sweeper's path: it feeds the coalescing run, so
 * that a sweep freeing adjacent blocks produces one large run instead of
 * many small ones. add_free_superblock is the direct path: the run is
 * cleared and split into power-of-two pieces immediately.
 *
 * Contract: the caller marks each block free (block_meta_set_debug_state
 * to CVM_DBG_FREE) before handing it over. That is what the sweeper does
 * when it decides a block is garbage, and the assertion in the debug
 * build is what catches a block freed twice or freed while still in use —
 * a bug that otherwise surfaces much later as two objects at the same
 * address. In a release build the assertion compiles out and the contract
 * is not enforced.
 */
void cvm_block_alloc_add_free_blocks(CvmBlockAllocator* alloc, BlockMeta* first,
                                     u32 count);

void cvm_block_alloc_add_free_superblock(CvmBlockAllocator* alloc, BlockMeta* first,
                                         u32 count);

/*
 * The same, but staging the pieces on local lists instead of the shared
 * ones.
 *
 * A sweeper produces many small free runs, and pushing each straight to
 * the shared lists means a compare-exchange loop per run, contending with
 * every other sweeper. Staging them locally and pushing the whole batch
 * once at the end of the sweep is what the local lists are for.
 *
 * The run is cleared the same way as the shared path. That is not
 * optional: forgetting it is how blocks came to reach a free list still
 * marked "needs sweeping", which the allocator's assertion catches.
 */
void cvm_block_alloc_add_free_superblock_local(CvmBlockAllocator* alloc,
                                               LocalBlockList* lists, BlockMeta* first,
                                               u32 count);

/* Reset for a new collection: empty lists, no cursor, no run. */
void cvm_block_alloc_clear(CvmBlockAllocator* alloc);

/* Take a run to hold in reserve for the sweeper. */
void cvm_block_alloc_reserve_blocks(CvmBlockAllocator* alloc);

/* Put the reserved run back into the free lists. */
void cvm_block_alloc_use_reserve(CvmBlockAllocator* alloc);

/* Leave concurrent mode. */
void cvm_block_alloc_finish_coalescing(CvmBlockAllocator* alloc);

void cvm_block_alloc_acquire(CvmBlockAllocator* alloc);
void cvm_block_alloc_release(CvmBlockAllocator* alloc);

CVM_INLINE u32 cvm_block_alloc_free_count(CvmBlockAllocator const* alloc) {
    return atomic_load_explicit(&alloc->free_block_count, memory_order_relaxed);
}

/* Partially free blocks available in the shared stack (worker-swept). */
CVM_INLINE u32 cvm_block_alloc_recycled_shared_count(CvmBlockAllocator const* alloc) {
    return atomic_load_explicit(&alloc->recycled_shared_count, memory_order_relaxed);
}

#endif /* CVM_GC_BLOCK_ALLOC_H */
