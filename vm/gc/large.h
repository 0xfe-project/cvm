/*
 * vm/gc/large.h
 *
 * The large-object allocator, ported from refs/commix/LargeAllocator.{h,c}.
 *
 * Objects at least CVM_LARGE_OBJECT_MIN_SIZE (8 KiB) are not line-managed.
 * Up to one block they are cut from whole free blocks in 8 KiB multiples,
 * and the pieces are reused through segregated free lists rather than being
 * returned to the block allocator and re-cut every time. Above one block
 * they are a run of whole blocks -- a superblock, asked for as one from the
 * block allocator -- because a chunk is defined by fitting inside a block.
 *
 * ---------------------------------------------------------------------
 * Objects larger than a block
 * ---------------------------------------------------------------------
 *
 * The superblock is the shape the rest of the collector already reads for a
 * large object: the marker skips line marking for a block marked as a
 * superblock head or tail, and cvm_sweep_superblock walks the head slot and
 * the last block, which is where an object that bodies the front blocks and
 * ends in the last one is described. A spanning object is one object per
 * superblock: the tail of its last block is not cut into chunks for other
 * objects, so up to a block per spanning object is left inside the run and
 * reclaimed with it. Filling that tail is what the chunk path in
 * cvm_sweep_superblock is written for and is not implemented, which is why
 * the sweep's clears there have no workload that reaches them.
 *
 * ---------------------------------------------------------------------
 * Why chunks rather than blocks
 * ---------------------------------------------------------------------
 *
 * A 10 KiB object needs two blocks if blocks are the unit, and 22 KiB of
 * the second block is wasted. Cutting blocks into 8 KiB chunks wastes at
 * most 8 KiB, and the leftover chunk goes on a free list for the next
 * object that needs one rather than being lost.
 *
 * A chunk carries its own header in its first bytes while it is free, so
 * the free lists cost no memory beyond what the chunks already occupy.
 * That header is overwritten by the object header when the chunk is
 * handed out, which is why a chunk's size must be read before it is
 * allocated and never after.
 *
 * ---------------------------------------------------------------------
 * Interaction with the bytemap
 * ---------------------------------------------------------------------
 *
 * A free chunk's first slot is marked as a placeholder, which tells the
 * collector "something starts here but it is not a live object". When the
 * chunk is handed out the slot becomes allocated. The interior slots of a
 * large object are left alone, so an interior pointer — which a precise
 * collector never produces — maps to a free slot and is ignored rather
 * than being mistaken for an object.
 *
 * ---------------------------------------------------------------------
 * What happens to chunks on sweep
 * ---------------------------------------------------------------------
 *
 * The sweeper finds a dead large object, reads its size from the block
 * metadata, and hands the chunk back here. Until the sweeper exists, the
 * only source of free chunks is the initial split of a superblock, which
 * is enough for the allocator to be correct and testable.
 */

#ifndef CVM_GC_LARGE_H
#define CVM_GC_LARGE_H

#include <stdatomic.h>

#include "base/types.h"
#include "cvm/gc.h"
#include "gc/block_alloc.h"
#include "gc/bytemap.h"
#include "gc/heap.h"

/*
 * A free chunk. Lives in the chunk's own memory while the chunk is free.
 *
 * `size` must be read before the chunk is allocated, because allocation
 * overwrites this header with the object header.
 *
 * CvmChunk and CvmFreeList are declared in gc/heap.h, because the free lists
 * belong to the heap rather than to a mutator: a chunk comes back from
 * whichever thread swept the block it lived in, and a GC worker sweeping has
 * no mutator to hand it to.
 */
typedef struct CvmLargeAllocator {
    Bytemap* bytemap;
    CvmBlockAllocator* block_alloc;
    CvmHeap* heap;
} CvmLargeAllocator;

void cvm_large_init(CvmLargeAllocator* large, CvmBlockAllocator* blocks,
                    Bytemap* bytemap, CvmHeap* heap);

/* Drop every free list on the heap. Called when a collection starts. */
void cvm_large_clear(CvmHeap* heap);

/*
 * Return a chunk to the heap's free lists.
 *
 * `total_size` is the chunk's size in bytes, which the caller knows from
 * the block metadata. It must be a multiple of the large-object granularity
 * and smaller than a block; a whole block goes back to the block allocator
 * instead.
 *
 * Takes the heap rather than an allocator because the caller may be a GC
 * worker, which has no allocator of its own. Passing one anyway -- or passing
 * NULL, which is what the sweep used to do -- is how a worker sweeping a
 * block with a dead chunk in it came to dereference a null allocator.
 */
void cvm_large_add_chunk(CvmHeap* heap, CvmChunk* chunk, usize total_size);

/* Bytes currently available across the free lists. */
usize cvm_large_free_bytes(CvmHeap const* heap);

/* Chunks currently on the free lists. */
u32 cvm_large_free_chunk_count(CvmHeap const* heap);

#endif /* CVM_GC_LARGE_H */
