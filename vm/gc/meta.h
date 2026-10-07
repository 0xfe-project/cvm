/*
 * vm/gc/meta.h
 *
 * The three levels of GC metadata, ported from refs/commix/metadata/:
 *
 *   BlockMeta   one per 32 KiB block: what state the block is in
 *   LineMeta    one per 256 B line: marked or not, for line-granular sweep
 *   ObjectMeta  one per 16 B allocation slot: what lives at this address
 *
 * ObjectMeta lives in a separate table (the bytemap, gc/bytemap.h) indexed
 * by address; BlockMeta and LineMeta live in tables indexed by block/line
 * number. All three are walked by the allocator and the sweeper.
 *
 * Deliberate differences from the reference implementation:
 *
 *  - No bitfields. MSVC packs them differently from GCC and Clang, and
 *    this metadata must be byte-identical on every platform. The reference
 *    uses a bitfield under GC_ASSERTIONS and a plain i32 otherwise, which
 *    silently makes the struct layout build-configuration dependent.
 *
 *  - The debug state field is present in every build, for the same reason.
 *    It costs four bytes of padding per block (about 0.2% of the per-block
 *    metadata), which is cheaper than a layout that changes with flags.
 *
 *  - No memset or memcpy. Those are libc symbols; in freestanding code
 *    they would become undefined references. Clearing is written out.
 *
 *  - The flags-plus-size write is done field by field rather than through
 *    a punning 32-bit store. See the note on block_meta_set_flag_and_size.
 */

#ifndef CVM_GC_META_H
#define CVM_GC_META_H

#include "base/assert.h"
#include "base/math.h"
#include "base/types.h"
#include "gc/layout.h"

/* ================================================================== */
/* LineMeta                                                            */
/* ================================================================== */

/*
 * A run of free lines is a linked list threaded through the lines
 * themselves. `next` is a line index within the same block, or
 * CVM_LAST_HOLE at the end; `size` is the run length in lines starting
 * at the line holding this node.
 *
 * `next` is i8 and line indices are 0..127, so the index range fits
 * exactly, with -1 free to serve as the terminator.
 */
typedef struct FreeLineMeta {
    i8 next;
    u8 size;
} FreeLineMeta;

#define CVM_LAST_HOLE (-1)

typedef enum LineFlag {
    CVM_LINE_EMPTY = 0x0,
    CVM_LINE_MARKED = 0x1,
} LineFlag;

typedef u8 LineMeta;

CVM_INLINE bool line_meta_is_marked(LineMeta const* m) {
    return *m == (u8)CVM_LINE_MARKED;
}

CVM_INLINE void line_meta_mark(LineMeta* m) {
    *m = (u8)CVM_LINE_MARKED;
}

CVM_INLINE void line_meta_unmark(LineMeta* m) {
    *m = (u8)CVM_LINE_EMPTY;
}

/* First LineMeta of a block's line table. */
CVM_INLINE LineMeta* line_meta_of_block(LineMeta* line_meta_start, u32 block) {
    return line_meta_start + (usize)block * CVM_LINE_COUNT;
}

CVM_INLINE LineMeta const* line_meta_of_block_const(LineMeta const* start, u32 block) {
    return start + (usize)block * CVM_LINE_COUNT;
}

/* ================================================================== */
/* ObjectMeta                                                          */
/* ================================================================== */

/*
 * One byte per allocation slot, describing what is at that address.
 *
 *   free         nothing lives here
 *   placeholder  this address is the interior of a large object; the
 *                object's own metadata entry is at its first slot
 *   allocated    a live object starts here
 *   marked       a live object starts here and has been reached by this
 *                cycle's marking
 *
 * The states are compared by equality, not by bit tests, so these values
 * are load-bearing.
 */
typedef enum ObjectFlag {
    CVM_OM_FREE = 0x0,
    CVM_OM_PLACEHOLDER = 0x1,
    CVM_OM_ALLOCATED = 0x2,
    CVM_OM_MARKED = 0x4,
} ObjectFlag;

typedef u8 ObjectMeta;

CVM_INLINE bool object_meta_is_free(ObjectMeta const* m) {
    return *m == (u8)CVM_OM_FREE;
}

CVM_INLINE bool object_meta_is_placeholder(ObjectMeta const* m) {
    return *m == (u8)CVM_OM_PLACEHOLDER;
}

CVM_INLINE bool object_meta_is_allocated(ObjectMeta const* m) {
    return *m == (u8)CVM_OM_ALLOCATED;
}

CVM_INLINE bool object_meta_is_marked(ObjectMeta const* m) {
    return *m == (u8)CVM_OM_MARKED;
}

/* True for anything that is not free: allocated, marked, or placeholder. */
CVM_INLINE bool object_meta_is_occupied(ObjectMeta const* m) {
    return *m != (u8)CVM_OM_FREE;
}

CVM_INLINE void object_meta_set_free(ObjectMeta* m) {
    *m = (u8)CVM_OM_FREE;
}

CVM_INLINE void object_meta_set_placeholder(ObjectMeta* m) {
    *m = (u8)CVM_OM_PLACEHOLDER;
}

CVM_INLINE void object_meta_set_allocated(ObjectMeta* m) {
    *m = (u8)CVM_OM_ALLOCATED;
}

CVM_INLINE void object_meta_set_marked(ObjectMeta* m) {
    *m = (u8)CVM_OM_MARKED;
}

/*
 * Sweep decision for one slot: a marked object survives as allocated,
 * anything else becomes free.
 */
CVM_INLINE void object_meta_sweep(ObjectMeta* m) {
    if (object_meta_is_marked(m)) {
        object_meta_set_allocated(m);
    } else {
        object_meta_set_free(m);
    }
}

/* Number of metadata slots covering one line / one block. */
#define CVM_SLOTS_PER_LINE (CVM_LINE_SIZE / CVM_ALLOC_ALIGNMENT)
#define CVM_SLOTS_PER_BLOCK (CVM_BLOCK_SIZE / CVM_ALLOC_ALIGNMENT)

CVM_INLINE void object_meta_clear_line(ObjectMeta* m) {
    for (usize i = 0; i < CVM_SLOTS_PER_LINE; i++) {
        object_meta_set_free(&m[i]);
    }
}

CVM_INLINE void object_meta_clear_block(ObjectMeta* m) {
    for (usize i = 0; i < CVM_SLOTS_PER_BLOCK; i++) {
        object_meta_set_free(&m[i]);
    }
}

CVM_INLINE void object_meta_sweep_line(ObjectMeta* m) {
    for (usize i = 0; i < CVM_SLOTS_PER_LINE; i++) {
        object_meta_sweep(&m[i]);
    }
}

#if defined(CVM_DEBUG)
/* Every slot a fresh allocation covers must be free or placeholder. */
CVM_INLINE void object_meta_assert_valid_allocation(ObjectMeta const* start,
                                                    usize size) {
    usize slots = size / CVM_ALLOC_ALIGNMENT;
    for (usize i = 0; i < slots; i++) {
        cvm_assert(object_meta_is_free(&start[i]) ||
                   object_meta_is_placeholder(&start[i]));
    }
}
#endif

/* ================================================================== */
/* BlockMeta                                                           */
/* ================================================================== */

/*
 * Block states.
 *
 * The low two bits are the base state; the upper bits are flags that only
 * mean anything on a superblock tail. The predicates below compare for
 * exact equality, so these values are load-bearing and must not be
 * renumbered. The two "me" states are used by the block allocator's
 * coalescing pass; see gc/block_alloc.c for what they mark.
 */
typedef enum BlockFlag {
    CVM_BLOCK_FREE = 0x00,
    CVM_BLOCK_SIMPLE = 0x01,
    CVM_BLOCK_SUPERBLOCK_START = 0x02,
    CVM_BLOCK_SUPERBLOCK_TAIL = 0x03,
    CVM_BLOCK_RESERVED = 0x04,
    CVM_BLOCK_MARKED = 0x05,              /* RESERVED | SIMPLE */
    CVM_BLOCK_SUPERBLOCK_START_ME = 0x0b, /* SUPERBLOCK_TAIL | 0x08 */
    CVM_BLOCK_COALESCE_ME = 0x13,         /* SUPERBLOCK_TAIL | 0x10 */
} BlockFlag;

/*
 * Debug-only block state, used by the sweeper's consistency checks.
 * Always present in the struct; see the file header.
 */
typedef enum BlockDebugState {
    CVM_DBG_MUST_SWEEP = 0x0,
    CVM_DBG_FREE = 0x1,
    CVM_DBG_PARTIAL_FREE = 0x2,
    CVM_DBG_NOT_FREE = 0x3,
    CVM_DBG_FREE_IN_COLLECTION = 0x4,
    CVM_DBG_IN_USE = 0x5,
} BlockDebugState;

typedef struct BlockMeta {
    /*
     * The two views share the leading flags byte. `simple` blocks track
     * the head of their free-line list; `superblock` blocks track how
     * many blocks the large object spans.
     */
    union {
        struct {
            u8 flags;
            i8 first_free_line;
        } simple;
        struct {
            u8 flags;
            u24 size; /* in blocks */
        } superblock;
    } block;

    /* Index of the next block in a free list, or -1 for the end. */
    i32 next_block;

    u8 debug_state;
    u8 reserved[3]; /* explicit, so the layout does not depend on the
                     * compiler's padding decisions */
} BlockMeta;

_Static_assert(sizeof(((BlockMeta*)0)->block) == 4,
               "the flags byte plus its payload must be four bytes");
_Static_assert(sizeof(BlockMeta) == 12,
               "BlockMeta layout is part of the on-disk-free metadata "
               "contract; changing it changes metadata memory use");

/* ------------------------------------------------------------------ */
/* BlockMeta: state predicates                                         */
/* ------------------------------------------------------------------ */

CVM_INLINE bool block_meta_is_free(BlockMeta const* m) {
    return m->block.simple.flags == (u8)CVM_BLOCK_FREE;
}

CVM_INLINE bool block_meta_is_simple(BlockMeta const* m) {
    u8 flags = m->block.simple.flags;
    return flags == (u8)CVM_BLOCK_SIMPLE || flags == (u8)CVM_BLOCK_MARKED;
}

CVM_INLINE bool block_meta_is_marked(BlockMeta const* m) {
    return m->block.simple.flags == (u8)CVM_BLOCK_MARKED;
}

CVM_INLINE bool block_meta_is_reserved(BlockMeta const* m) {
    return m->block.simple.flags == (u8)CVM_BLOCK_RESERVED;
}

CVM_INLINE bool block_meta_is_superblock_start(BlockMeta const* m) {
    return m->block.simple.flags == (u8)CVM_BLOCK_SUPERBLOCK_START;
}

CVM_INLINE bool block_meta_is_superblock_tail(BlockMeta const* m) {
    return m->block.simple.flags == (u8)CVM_BLOCK_SUPERBLOCK_TAIL;
}

CVM_INLINE bool block_meta_is_superblock_start_me(BlockMeta const* m) {
    return m->block.simple.flags == (u8)CVM_BLOCK_SUPERBLOCK_START_ME;
}

CVM_INLINE bool block_meta_is_coalesce_me(BlockMeta const* m) {
    return m->block.simple.flags == (u8)CVM_BLOCK_COALESCE_ME;
}

/* True for both the head and the continuation blocks of a large object. */
CVM_INLINE bool block_meta_contains_large_objects(BlockMeta const* m) {
    return block_meta_is_superblock_start(m) || block_meta_is_superblock_tail(m);
}

/* ------------------------------------------------------------------ */
/* BlockMeta: state transitions                                        */
/* ------------------------------------------------------------------ */

CVM_INLINE void block_meta_set_flag(BlockMeta* m, BlockFlag flag) {
    m->block.simple.flags = (u8)flag;
}

CVM_INLINE void block_meta_mark(BlockMeta* m) {
    block_meta_set_flag(m, CVM_BLOCK_MARKED);
}

CVM_INLINE void block_meta_unmark(BlockMeta* m) {
    block_meta_set_flag(m, CVM_BLOCK_SIMPLE);
}

/*
 * Write the flags byte and the superblock size together.
 *
 * The reference implementation does this with a punning 32-bit store.
 * We write the two fields individually: the pun is strict-aliasing UB,
 * and a compiler is within its rights to miscompile it.
 *
 * Consequence worth knowing: this is two stores, not one, so a reader
 * running concurrently could observe the new flags with the old size.
 * Every call site today is either stop-the-world or holds the block
 * allocator lock. If that ever stops being true, this needs a single
 * store, which means a small helper that packs the bytes explicitly
 * rather than punning.
 */
CVM_INLINE void block_meta_set_flag_and_size(BlockMeta* m, BlockFlag flag, i32 size) {
    cvm_assert(flag != CVM_BLOCK_SIMPLE);
    cvm_assert(flag != CVM_BLOCK_SUPERBLOCK_START || size > 0);
    cvm_assert(flag != CVM_BLOCK_COALESCE_ME || size > 0);

    m->block.superblock.flags = (u8)flag;
    m->block.superblock.size = u24_from_u32((u32)size);
}

CVM_INLINE u32 block_meta_superblock_size(BlockMeta const* m) {
    return u24_to_u32(m->block.superblock.size);
}

CVM_INLINE void block_meta_set_first_free_line(BlockMeta* m, i8 line) {
    cvm_assert(block_meta_is_simple(m));
    /* Compare in i32: (i8)CVM_LINE_COUNT would overflow to -128, since
     * a line index of 127 is valid and 128 is not representable in i8. */
    cvm_assert(line == CVM_LAST_HOLE ||
               ((i32)line >= 0 && (i32)line < (i32)CVM_LINE_COUNT));
    m->block.simple.first_free_line = line;
}

CVM_INLINE i8 block_meta_first_free_line(BlockMeta const* m) {
    cvm_assert(block_meta_is_simple(m));
    return m->block.simple.first_free_line;
}

CVM_INLINE void block_meta_set_debug_state(BlockMeta* m, BlockDebugState s) {
    m->debug_state = (u8)s;
}

CVM_INLINE u8 block_meta_debug_state(BlockMeta const* m) {
    return m->debug_state;
}

/* Reset to the free state. Written out rather than memset; see the file
 * header for why. */
CVM_INLINE void block_meta_clear(BlockMeta* m) {
    m->block.simple.flags = (u8)CVM_BLOCK_FREE;
    m->block.simple.first_free_line = CVM_LAST_HOLE;
    m->next_block = -1;
    m->debug_state = (u8)CVM_DBG_MUST_SWEEP;
    m->reserved[0] = 0;
    m->reserved[1] = 0;
    m->reserved[2] = 0;
}

/* ------------------------------------------------------------------ */
/* Block geometry: address arithmetic                                  */
/* ------------------------------------------------------------------ */

/*
 * Everything below relies on the heap base being block-aligned: the
 * address-to-block conversion is a mask, not a division, so a base that
 * is not 32 KiB aligned would map addresses outside the heap to a block
 * below it. Heap creation is responsible for that alignment (the heap is
 * mapped and then rounded up; see gc/heap.c).
 */

/* Address of line `index` inside a block. */
CVM_INLINE u8* block_line_address(u8* block_start, u32 index) {
    cvm_assert(index < CVM_LINE_COUNT);
    return block_start + (usize)index * CVM_LINE_SIZE;
}

CVM_INLINE u8 const* block_line_address_const(u8 const* block_start, u32 index) {
    cvm_assert(index < CVM_LINE_COUNT);
    return block_start + (usize)index * CVM_LINE_SIZE;
}

/* One past the last byte of a block. */
CVM_INLINE u8* block_end(u8* block_start) {
    return block_start + CVM_BLOCK_SIZE;
}

/* Which line of the block contains `p`. */
CVM_INLINE u32 block_line_index_of(u8 const* block_start, u8 const* p) {
    return (u32)((usize)(p - block_start) >> CVM_LINE_SIZE_BITS);
}

/* Start address of the block containing `p`. */
CVM_INLINE u8 const* block_start_of(u8 const* p) {
    return (u8 const*)((usize)p & CVM_BLOCK_ALIGN_MASK);
}

CVM_INLINE u8* block_start_of_mut(u8* p) {
    return (u8*)((usize)p & CVM_BLOCK_ALIGN_MASK);
}

/* Block index of the block containing `p`. */
CVM_INLINE u32 block_index_of(u8 const* heap_start, u8 const* p) {
    return (u32)((usize)(block_start_of(p) - heap_start) / CVM_BLOCK_SIZE);
}

CVM_INLINE u8* block_start_from_index(u8* heap_start, u32 index) {
    return heap_start + (usize)index * CVM_BLOCK_SIZE;
}

/* ------------------------------------------------------------------ */
/* BlockMeta <-> BlockMeta table                                       */
/* ------------------------------------------------------------------ */

CVM_INLINE BlockMeta* block_meta_at(BlockMeta* meta_start, u32 index) {
    return meta_start + index;
}

CVM_INLINE BlockMeta const* block_meta_at_const(BlockMeta const* meta_start,
                                                u32 index) {
    return meta_start + index;
}

CVM_INLINE u32 block_meta_index_of(BlockMeta const* meta_start, BlockMeta const* m) {
    return (u32)(m - meta_start);
}

/* Address of the block described by this metadata. */
CVM_INLINE u8* block_meta_block_start(BlockMeta* meta_start, u8* heap_start,
                                      BlockMeta* m) {
    return block_start_from_index(heap_start, block_meta_index_of(meta_start, m));
}

/* Metadata entry describing the block containing `p`. */
CVM_INLINE BlockMeta* block_meta_of_word(BlockMeta* meta_start, u8* heap_start,
                                         u8 const* p) {
    return block_meta_at(meta_start, block_index_of(heap_start, p));
}

/*
 * Walk backwards from a superblock tail to the head of its superblock.
 * Blocks in a superblock are contiguous and the head is at or after
 * meta_start, so the loop terminates.
 */
CVM_INLINE BlockMeta* block_meta_superblock_start(BlockMeta* meta_start, BlockMeta* m) {
    BlockMeta* cur = m;
    while (block_meta_is_superblock_tail(cur)) {
        cur--;
        cvm_assert(cur >= meta_start);
    }
    cvm_assert(block_meta_is_superblock_start(cur));
    return cur;
}

/* ================================================================== */
/* Per-block metadata cost                                             */
/* ================================================================== */

/*
 * Bytes of metadata per block: the BlockMeta itself, one LineMeta byte
 * per line, and one ObjectMeta byte per allocation slot.
 */
#define CVM_METADATA_PER_BLOCK                                                         \
    (sizeof(BlockMeta) + CVM_LINE_COUNT * sizeof(LineMeta) + CVM_SLOTS_PER_BLOCK)

/* Address space consumed per block: payload plus its metadata. */
#define CVM_SPACE_USED_PER_BLOCK (CVM_BLOCK_SIZE + CVM_METADATA_PER_BLOCK)

#define CVM_MAX_HEAP_SIZE ((usize)CVM_SPACE_USED_PER_BLOCK * CVM_MAX_BLOCK_COUNT)
#define CVM_DEFAULT_MIN_HEAP_SIZE ((usize)128 * CVM_SPACE_USED_PER_BLOCK)

_Static_assert(CVM_SLOTS_PER_LINE == CVM_SLOTS_PER_BLOCK / CVM_LINE_COUNT,
               "a block's metadata slots must tile its lines exactly");

#endif /* CVM_GC_META_H */
