/*
 * vm/gc/alloc.h
 *
 * The per-thread allocator, ported from refs/commix/Allocator.{h,c}.
 *
 * ---------------------------------------------------------------------
 * The fast path is not here
 * ---------------------------------------------------------------------
 *
 * Small-object allocation's hot path is inlined into generated code (see
 * cvm_alloc in cvm/gc.h): compare the bump cursor against the limit,
 * advance it, write the header. Nothing in this file runs unless that
 * comparison fails.
 *
 * What this file provides is everything the fast path falls back on: the
 * next line, the next block, the medium-object overflow path, and the
 * retry loop that collects and grows.
 *
 * ---------------------------------------------------------------------
 * Lines, not blocks
 * ---------------------------------------------------------------------
 *
 * A thread's bump region is a run of contiguous *lines*, not a whole
 * block. Lines are the unit the sweeper reclaims, so a thread that only
 * ever used a few hundred bytes of a block would otherwise hold the whole
 * 32 KiB against the sweeper. Handing out lines keeps the granularity of
 * waste at 256 bytes.
 *
 * Free lines are linked through the line memory itself, using the
 * FreeLineMeta header that gc/meta.h describes: the first bytes of a free
 * line hold the index of the next free line and the length of the run
 * starting here. That is why a free line cannot be used for anything else
 * until it is allocated, and why the line is only written with object data
 * after it has been unlinked.
 *
 * ---------------------------------------------------------------------
 * Three sizes of object
 * ---------------------------------------------------------------------
 *
 *   small   fits in a line: bump-allocated, the common case
 *   medium  fits in a block but not a line: allocated from whole free
 *           blocks via a separate cursor, never from recycled blocks
 *   large   at least CVM_LARGE_OBJECT_MIN_SIZE: whole contiguous blocks,
 *           handed to the large allocator
 *
 * The medium path exists because a line-sized bump region cannot hold an
 * object larger than a line, but such an object is still small enough
 * that giving it a superblock would waste most of it.
 */

#ifndef CVM_GC_ALLOC_H
#define CVM_GC_ALLOC_H

#include <stdatomic.h>

#include "base/types.h"
#include "cvm/gc.h"
#include "gc/block_alloc.h"
#include "gc/block_list.h"
#include "gc/bytemap.h"
#include "gc/heap.h"
#include "gc/meta.h"

typedef struct CvmAllocator {
    /*
     * Field order is deliberate: the ones the fallback path touches
     * first are grouped together, and the atomically updated count is
     * last so it does not share a cache line with anything hot.
     */
    Bytemap* bytemap;
    CvmBlockAllocator* block_alloc;
    CvmHeap* heap;

    /* The block the bump region came from. */
    BlockMeta* block;
    u8* block_start;

    /* Whole free blocks used for medium objects, never recycled ones. */
    BlockMeta* large_block;
    u8* large_block_start;
    u8* large_cursor;
    u8* large_limit;

    /*
     * Blocks the sweeper has finished with and this thread may reuse.
     * Filled by the collector, drained here.
     */
    BlockList recycled_blocks;
    _Atomic u32 recycled_block_count;
} CvmAllocator;

void cvm_allocator_init(CvmAllocator* alloc, CvmBlockAllocator* blocks,
                        Bytemap* bytemap, CvmHeap* heap);

/* Drop every cached region. Called when a collection starts, because the
 * block a thread was bumping in may now be a block the sweeper wants. */
void cvm_allocator_clear(CvmAllocator* alloc);

/*
 * The allocation fallback, called from the inlined fast path.
 *
 * Returns a payload pointer whose header is already written, or does not
 * return: there is no failure path, because an allocation that cannot be
 * satisfied by collecting and growing means the heap is exhausted, and
 * the only honest response to that is to stop.
 */
opaque_mut cvm_alloc_slow(CvmTypeDescriptor const* type, usize size);

/* The large-object path, also reachable through cvm_alloc_slow. */
opaque_mut cvm_alloc_large(CvmTypeDescriptor const* type, usize size);

/* Blocks currently cached by this thread. */
CVM_INLINE u32 cvm_allocator_recycled_count(CvmAllocator const* alloc) {
    return atomic_load_explicit(&alloc->recycled_block_count, memory_order_relaxed);
}

#endif /* CVM_GC_ALLOC_H */
