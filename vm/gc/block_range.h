/*
 * vm/gc/block_range.h
 *
 * A block run packed into one atomic word, ported from
 * refs/commix/datastructures/BlockRange.h.
 *
 * The sweeper coalesces adjacent free blocks into runs, and the allocator
 * pulls from the run currently being built. Two 32-bit block indices —
 * first and limit — fit in one 64-bit word, which means the whole thing
 * is updated by a single compare-exchange instead of needing a lock or a
 * second field that could disagree with the first.
 *
 * The encoding: low 32 bits are `first`, high 32 bits are `limit`. An
 * empty range has first >= limit, so zero means empty and no separate
 * validity flag is needed.
 */

#ifndef CVM_GC_BLOCK_RANGE_H
#define CVM_GC_BLOCK_RANGE_H

#include <stdatomic.h>

#include "base/types.h"

typedef _Atomic u64 CvmBlockRange;
typedef u64 CvmBlockRangeVal;

/* Returned when a range cannot satisfy a request. */
#define CVM_NO_BLOCK_INDEX ((u32)~0u)

#define CVM_EMPTY_RANGE ((CvmBlockRangeVal)0)

CVM_INLINE CvmBlockRangeVal cvm_block_range_pack(u32 first, u32 limit) {
    return ((CvmBlockRangeVal)limit << 32) | (CvmBlockRangeVal)first;
}

CVM_INLINE u32 cvm_block_range_first(CvmBlockRangeVal r) {
    return (u32)(r & 0xffffffffu);
}

CVM_INLINE u32 cvm_block_range_limit(CvmBlockRangeVal r) {
    return (u32)(r >> 32);
}

CVM_INLINE bool cvm_block_range_is_empty(CvmBlockRangeVal r) {
    return cvm_block_range_first(r) >= cvm_block_range_limit(r);
}

/* Number of blocks covered. Zero for an empty range. */
CVM_INLINE u32 cvm_block_range_size(CvmBlockRangeVal r) {
    if (cvm_block_range_is_empty(r)) {
        return 0;
    }
    return cvm_block_range_limit(r) - cvm_block_range_first(r);
}

CVM_INLINE void cvm_block_range_clear(CvmBlockRange* r) {
    atomic_store(r, CVM_EMPTY_RANGE);
}

/*
 * Extend the range to cover [first, first + count), or replace it.
 *
 * Three cases, and the return value distinguishes them:
 *
 *   - the range was empty: it becomes the new run, returns empty;
 *   - the new run starts exactly where the old one ended: the range grows
 *     in place, returns empty. This is the coalescing case, and it is why
 *     the sweeper walks blocks in ascending order;
 *   - the new run is elsewhere: the range is replaced and the old one is
 *     returned for the caller to hand to the free lists.
 *
 * The whole decision is one compare-exchange, so concurrent sweepers
 * cannot interleave into a range that is internally inconsistent.
 */
CVM_INLINE CvmBlockRangeVal cvm_block_range_append_last_or_replace(CvmBlockRange* r,
                                                                   u32 first,
                                                                   u32 count) {
    CvmBlockRangeVal old = atomic_load(r);
    CvmBlockRangeVal next;
    CvmBlockRangeVal displaced = CVM_EMPTY_RANGE;

    for (;;) {
        if (cvm_block_range_is_empty(old)) {
            next = cvm_block_range_pack(first, first + count);
            displaced = CVM_EMPTY_RANGE;
        } else if (cvm_block_range_limit(old) == first) {
            next = cvm_block_range_pack(cvm_block_range_first(old), first + count);
            displaced = CVM_EMPTY_RANGE;
        } else {
            next = cvm_block_range_pack(first, first + count);
            displaced = old;
        }

        if (atomic_compare_exchange_weak(r, &old, next)) {
            return displaced;
        }
        /* old was refreshed by the failed exchange; re-decide */
    }
}

/*
 * Take the first `count` blocks from the range, if it has that many.
 *
 * Returns the first index taken, or CVM_NO_BLOCK_INDEX. The range is
 * advanced rather than removed, so a caller taking several runs walks
 * forward through it.
 */
CVM_INLINE u32 cvm_block_range_poll_first(CvmBlockRange* r, u32 count) {
    CvmBlockRangeVal old = atomic_load(r);
    u32 first;
    CvmBlockRangeVal next;

    for (;;) {
        first = cvm_block_range_first(old);
        u32 limit = cvm_block_range_limit(old);

        if (cvm_block_range_size(old) < count) {
            return CVM_NO_BLOCK_INDEX;
        }

        next = cvm_block_range_pack(first + count, limit);

        if (atomic_compare_exchange_weak(r, &old, next)) {
            return first;
        }
    }
}

#endif /* CVM_GC_BLOCK_RANGE_H */
