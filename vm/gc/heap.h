/*
 * vm/gc/heap.h
 *
 * Heap layout, ported from refs/commix/Heap.{h,c}.
 *
 * ---------------------------------------------------------------------
 * Layout
 * ---------------------------------------------------------------------
 *
 * The heap is a set of separate reservations, not one:
 *
 *   block metadata    one BlockMeta per block, for the maximum block count
 *   line metadata     one LineMeta per line, likewise
 *   bytemap           one ObjectMeta per allocation slot, likewise
 *   the heap itself   block-aligned, so that an address maps to a block by
 *                     masking rather than by division
 *
 * The metadata regions are sized for the maximum and grow by moving their
 * end pointers, so growing the heap never has to relocate metadata. That
 * is worth the address space: the block metadata tables are addressed by
 * arithmetic on the metadata base, and a relocation would invalidate
 * every index.
 *
 * The heap reservation is block-aligned. This is not cosmetic: the
 * address-to-block conversion in gc/meta.h is a mask, so a base that is
 * not aligned would map addresses outside the heap onto a block below it.
 * Aligning up inside the reservation is why the reservation asks for one
 * extra block.
 *
 * ---------------------------------------------------------------------
 * Growth
 * ---------------------------------------------------------------------
 *
 * cvm_heap_extend moves the end pointers and clears the new metadata. It
 * does not decide policy: whether to grow, and by how much, belongs to
 * whoever needs the space. The reference keeps that policy in
 * Heap_GrowIfNeeded, driven by the ratio of marking time to mutator time
 * and by the fraction of free blocks; the same shape is reproduced here
 * in cvm_heap_should_grow.
 */

#ifndef CVM_GC_HEAP_H
#define CVM_GC_HEAP_H

#include <stdatomic.h>

#include "base/types.h"
#include "gc/bytemap.h"
#include "gc/layout.h"
#include "gc/meta.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* a reservation                                                       */
/* ------------------------------------------------------------------ */

/*
 * An aligned window inside a reservation.
 *
 * `base` and `size` are what rt_mem_reserve returned and must be handed
 * back verbatim on release; `start` is the aligned address the heap
 * actually uses, and `usable` is how much fits from there.
 */
typedef struct CvmRegion {
    u8* base;
    usize size;
    u8* start;
    usize usable;
} CvmRegion;

/* Reserve `bytes` and align the usable start up to `align`. */
bool cvm_region_reserve(CvmRegion* region, usize bytes, usize align);

void cvm_region_release(CvmRegion* region);

/* ------------------------------------------------------------------ */
/* the heap                                                            */
/* ------------------------------------------------------------------ */

typedef struct CvmHeap {
    /* --- the block-aligned heap window --- */
    CvmRegion blocks;
    u8* heap_start;
    u8* heap_end; /* one past the last byte in use */
    usize heap_size;

    /* --- metadata, sized for the maximum --- */
    CvmRegion block_meta_region;
    CvmRegion line_meta_region;
    CvmRegion bytemap_region;

    BlockMeta* block_meta_start;
    BlockMeta* block_meta_end;
    LineMeta* line_meta_start;
    LineMeta* line_meta_end;

    Bytemap* bytemap;

    /* --- sizes --- */
    u32 block_count;     /* in use */
    u32 max_block_count; /* reserved */
    usize max_heap_size;

    /* --- growth policy --- */
    f64 max_mark_time_ratio;
    f64 min_free_ratio;

    /* --- growth serialisation --- */
    RtMutex grow_lock;
} CvmHeap;

/*
 * Set up the heap.
 *
 * `min_size` and `max_size` are in bytes of block payload. `max_size` is
 * what gets reserved; `min_size` is what starts out usable. Sizes below
 * CVM_MIN_HEAP_SIZE are raised to it, and a max of 0 means "as much as
 * the machine will give".
 *
 * Returns false if a reservation failed, in which case nothing is
 * allocated and the heap must not be used.
 */
bool cvm_heap_init(CvmHeap* heap, usize min_size, usize max_size);

void cvm_heap_destroy(CvmHeap* heap);

/*
 * Extend the heap by `blocks`, clearing the new metadata.
 *
 * Reports the index of the first new block through `out_first_block`, so
 * that the caller can hand the range to the block allocator. The heap
 * itself does not know about free lists; that is gc/block_alloc.h.
 *
 * Returns false if the request would exceed the reservation.
 */
bool cvm_heap_extend(CvmHeap* heap, u32 blocks, u32* out_first_block);

/*
 * Whether to grow, given how long the last collection spent marking and
 * how many blocks are free.
 *
 * Both conditions come from the reference and both matter: a heap that is
 * mostly marking is too small for the program's allocation rate, and a
 * heap with few free blocks is about to fail an allocation. Growing only
 * on the second would let a program sit at 100% mark time.
 */
bool cvm_heap_should_grow(CvmHeap* heap, u64 mark_time_ns, u64 total_time_ns,
                          u32 free_blocks);

/*
 * How many blocks to add next. Doubling while the heap is small avoids a
 * long ramp of small grows; the smaller golden-ratio factor later keeps
 * the over-reservation bounded.
 */
u32 cvm_heap_growth_increment(CvmHeap const* heap);

/* Bytes of address space one block costs, including its metadata. */
CVM_INLINE usize cvm_heap_space_per_block(void) {
    return CVM_SPACE_USED_PER_BLOCK;
}

/* Address of block `index`, or NULL if it is beyond the current heap. */
CVM_INLINE u8* cvm_heap_block_address(CvmHeap const* heap, u32 index) {
    if (index >= heap->block_count) {
        return NULL;
    }
    return heap->heap_start + (usize)index * CVM_BLOCK_SIZE;
}

/* Metadata entry for block `index`. */
CVM_INLINE BlockMeta* cvm_heap_block_meta(CvmHeap const* heap, u32 index) {
    return heap->block_meta_start + index;
}

CVM_INLINE LineMeta* cvm_heap_line_meta(CvmHeap const* heap, u32 block) {
    return heap->line_meta_start + (usize)block * CVM_LINE_COUNT;
}

/* Is this address inside the heap's usable window? */
CVM_INLINE bool cvm_heap_contains(CvmHeap const* heap, u8 const* p) {
    return p >= heap->heap_start && p < heap->heap_end;
}

#endif /* CVM_GC_HEAP_H */
