/*
 * vm/gc/layout.h
 *
 * Heap geometry and growth policy, ported from refs/commix/Constants.h and
 * refs/immix_comix/CommonConstants.h.
 *
 * Geometry is expressed in bits first and derived values second, so every
 * power of two is stated once. The _Static_assert block at the bottom pins
 * the relationships; if someone edits a bit count, the build tells them
 * what broke instead of producing a heap that is subtly misaligned.
 *
 * Not here: anything that needs sizeof(BlockMeta). METADATA_PER_BLOCK and
 * friends live in gc/meta.h, otherwise layout.h and meta.h would include
 * each other. Also not here: grey packet sizing (gc/grey_packet.h) and
 * stats buffer sizing (stats/), which belong to their own subsystems.
 */

#ifndef CVM_GC_LAYOUT_H
#define CVM_GC_LAYOUT_H

#include "base/types.h"
#include "cvm/gc.h"

/* ------------------------------------------------------------------ */
/* allocation granularity                                              */
/* ------------------------------------------------------------------ */

/*
 * CVM_ALLOC_ALIGNMENT itself is public (cvm/gc.h): generated code inlines
 * the bump path and embeds the rounding. Here we only need its word count
 * for metadata indexing.
 */
#define CVM_ALLOC_ALIGNMENT_WORDS (CVM_ALLOC_ALIGNMENT / CVM_WORD_SIZE)

/* ------------------------------------------------------------------ */
/* block and line geometry                                             */
/* ------------------------------------------------------------------ */

#define CVM_BLOCK_SIZE_BITS 15u
#define CVM_LINE_SIZE_BITS 8u
#define CVM_BLOCK_COUNT_BITS 24u

#define CVM_BLOCK_SIZE ((usize)1 << CVM_BLOCK_SIZE_BITS) /* 32 KiB */
#define CVM_LINE_SIZE ((usize)1 << CVM_LINE_SIZE_BITS)   /* 256 B */
#define CVM_MAX_BLOCK_COUNT ((usize)1 << CVM_BLOCK_COUNT_BITS)

#define CVM_LINE_COUNT (CVM_BLOCK_SIZE / CVM_LINE_SIZE) /* 128 */

/*
 * Upper bound for a line index, as a wide type.
 *
 * Line indices are stored in an i8 and run from 0 to CVM_LINE_COUNT - 1.
 * Comparing one against CVM_LINE_COUNT requires a wider type: casting the
 * count to i8 gives -128, so `index < (i8)CVM_LINE_COUNT` is false for
 * every legal index and the check silently stops checking. Use this
 * instead, and the mistake cannot be made.
 */
#define CVM_LINE_INDEX_LIMIT ((i32)CVM_LINE_COUNT)
#define CVM_WORDS_IN_LINE (CVM_LINE_SIZE / CVM_WORD_SIZE)   /* 32 */
#define CVM_WORDS_IN_BLOCK (CVM_BLOCK_SIZE / CVM_WORD_SIZE) /* 4096 */

/* Mask off the offset within a block: address -> block start. */
#define CVM_BLOCK_OFFSET_MASK (CVM_BLOCK_SIZE - 1)
#define CVM_BLOCK_ALIGN_MASK (~CVM_BLOCK_OFFSET_MASK)

/* ------------------------------------------------------------------ */
/* large objects                                                       */
/* ------------------------------------------------------------------ */

/*
 * Anything at least this large is handed to the large-object allocator,
 * which deals in whole chunks of this size rather than in lines.
 */
#define CVM_LARGE_OBJECT_MIN_BITS 13u
#define CVM_LARGE_OBJECT_MIN_SIZE ((usize)1 << CVM_LARGE_OBJECT_MIN_BITS) /* 8 KiB */

/*
 * One free list per chunk size between the large-object threshold and a
 * block: 8 KiB, 16 KiB and 24 KiB. A 32 KiB chunk is a whole block and
 * goes back to the block allocator instead, which is why the count is one
 * less than the number of multiples.
 */
#define CVM_LARGE_FREE_LIST_COUNT                                                      \
    ((1u << (CVM_BLOCK_SIZE_BITS - CVM_LARGE_OBJECT_MIN_BITS)) - 1u)

/*
 * Blocks held back from the free lists so the sweeper always has
 * somewhere to work. Without this, a heap that fills completely leaves
 * the sweeper unable to allocate the space it needs to finish, which is a
 * deadlock rather than a slow collection.
 */
#define CVM_SWEEP_RESERVE_BLOCKS 4u

/*
 * Grey packets as a fraction of the heap size. Marking parallelism comes
 * from having enough packets to hand out; the fraction is the reference's
 * and is a starting point rather than a tuned value.
 */
#define CVM_GREY_PACKET_RATIO 0.01

/* ------------------------------------------------------------------ */
/* heap growth policy                                                  */
/* ------------------------------------------------------------------ */

/*
 * Growth is triggered when either the fraction of time spent marking, or
 * the fraction of free blocks, crosses these ratios.
 */
#define CVM_MARK_TIME_RATIO_DEFAULT 0.05
#define CVM_FREE_RATIO_DEFAULT 0.5
#define CVM_MAX_UNAVAILABLE_RATIO 0.25
#define CVM_MIN_FREE_RATIO_TO_DEFER_GROWTH 0.65

/*
 * Below EARLY_GROWTH_THRESHOLD the heap doubles; above it, it grows by
 * the golden-ratio factor. Doubling early avoids a long ramp of small
 * grows; the smaller factor later keeps the over-reservation bounded.
 */
#define CVM_EARLY_GROWTH_THRESHOLD ((usize)128 * 1024 * 1024)
#define CVM_EARLY_GROWTH_RATE 2.0
#define CVM_GROWTH_RATE 1.414213562

#define CVM_MIN_HEAP_SIZE ((usize)1 * 1024 * 1024)
#define CVM_UNLIMITED_HEAP_SIZE (~(usize)0)

/* ------------------------------------------------------------------ */
/* invariants                                                          */
/* ------------------------------------------------------------------ */

_Static_assert((CVM_BLOCK_SIZE & (CVM_BLOCK_SIZE - 1)) == 0,
               "block size must be a power of two");
_Static_assert((CVM_LINE_SIZE & (CVM_LINE_SIZE - 1)) == 0,
               "line size must be a power of two");
_Static_assert(CVM_LINE_COUNT* CVM_LINE_SIZE == CVM_BLOCK_SIZE,
               "lines must tile a block exactly, with no gaps");
_Static_assert(CVM_LINE_SIZE % CVM_WORD_SIZE == 0,
               "line size must be a whole number of words");
_Static_assert(CVM_BLOCK_SIZE % CVM_LINE_SIZE == 0,
               "block size must be a whole number of lines");
_Static_assert(CVM_ALLOC_ALIGNMENT % CVM_WORD_SIZE == 0,
               "allocation alignment must be a whole number of words");
_Static_assert(CVM_INLINE_ALLOC_MAX == CVM_LINE_SIZE,
               "the inlined allocation path bumps within a run of lines, so "
               "its size limit must be exactly the line size");
_Static_assert(CVM_LINE_SIZE % CVM_ALLOC_ALIGNMENT == 0,
               "a line must hold a whole number of allocation slots, "
               "otherwise object metadata would straddle line boundaries");
_Static_assert(CVM_ALLOC_ALIGNMENT >= sizeof(CvmGcHeader),
               "an object must be able to hold at least its own header");
_Static_assert(CVM_BLOCK_SIZE % CVM_LARGE_OBJECT_MIN_SIZE == 0,
               "large-object granularity must divide the block size, "
               "so a large allocation is a whole number of blocks");

#endif /* CVM_GC_LAYOUT_H */
