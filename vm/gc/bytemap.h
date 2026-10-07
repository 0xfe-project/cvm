/*
 * vm/gc/bytemap.h
 *
 * Address -> ObjectMeta map, ported from refs/commix/datastructures/Bytemap.
 *
 * The bytemap is one byte per allocation slot (CVM_ALLOC_ALIGNMENT bytes of
 * heap), covering the heap from its base upwards. Given an object's start
 * address, bytemap_get() returns the metadata byte describing it; that is
 * how the marker and the sweeper decide whether an address holds a live
 * object without consulting the object itself.
 *
 * Storage: the bytemap is not heap-allocated. It is a flexible-array
 * structure placed inside the heap's metadata region, immediately after
 * the BlockMeta and LineMeta tables. bytemap_bytes_for() reports how much
 * room it needs, and bytemap_init() places it.
 *
 * No .c file: the reference implementation's translation unit contained
 * only a three-assignment initialiser, which does not warrant one. Every
 * operation here is inline, and the hot ones are always_inline.
 */

#ifndef CVM_GC_BYTEMAP_H
#define CVM_GC_BYTEMAP_H

#include "base/assert.h"
#include "base/math.h"
#include "base/types.h"
#include "gc/layout.h"
#include "gc/meta.h"

typedef struct Bytemap {
    /* Address of slot 0: the heap base. */
    u8* first;

    /* Number of slots. Slot i covers [first + i*ALIGN, first + (i+1)*ALIGN). */
    usize count;

    /* One ObjectMeta per slot, laid out contiguously. */
    ObjectMeta slots[];
} Bytemap;

_Static_assert(sizeof(Bytemap) == 2 * sizeof(usize),
               "the bytemap header must be exactly two words, so that the "
               "slot array follows at a known offset");

/* One past the last slot. */
CVM_INLINE ObjectMeta* bytemap_end(Bytemap* b) {
    return b->slots + b->count;
}

CVM_INLINE ObjectMeta const* bytemap_end_const(Bytemap const* b) {
    return b->slots + b->count;
}

/*
 * Bytes of storage a bytemap covering `heap_size` bytes of heap needs.
 * The heap reserves this as part of its metadata region.
 */
CVM_INLINE usize bytemap_bytes_for(usize heap_size) {
    return sizeof(Bytemap) + cvm_div_round_up(heap_size, CVM_ALLOC_ALIGNMENT);
}

/*
 * Place a bytemap in `mem`, covering the heap range
 * [heap_start, heap_start + heap_size).
 *
 * `mem` must be at least bytemap_bytes_for(heap_size) bytes and suitably
 * aligned for ObjectMeta (one byte, so any alignment will do, but the
 * caller is expected to hand out aligned metadata regions).
 *
 * The caller owns clearing the slots; heap creation does that once.
 */
CVM_INLINE Bytemap* bytemap_init(void* mem, u8* heap_start, usize heap_size) {
    Bytemap* b = (Bytemap*)mem;
    b->first = heap_start;
    b->count = cvm_div_round_up(heap_size, CVM_ALLOC_ALIGNMENT);
    return b;
}

/*
 * Slot index for an address.
 *
 * Contract: `addr` must lie inside the heap and must be slot-aligned.
 * An unaligned address would silently describe the wrong slot, so both
 * are asserted in debug builds. Note the alignment is checked on the
 * offset, not on the absolute address: the bytemap only cares that the
 * address is slot-aligned relative to the heap base.
 */
CVM_INLINE usize bytemap_index_of(Bytemap const* b, u8 const* addr) {
    cvm_assert(addr >= b->first);

    usize offset = (usize)(addr - b->first);
    cvm_assert(cvm_is_aligned(offset, CVM_ALLOC_ALIGNMENT));

    usize index = offset / CVM_ALLOC_ALIGNMENT;
    cvm_assert(index < b->count);
    return index;
}

/* Metadata byte for the slot holding `addr`. Mutable flavour. */
CVM_INLINE ObjectMeta* bytemap_get(Bytemap* b, u8 const* addr) {
    return &b->slots[bytemap_index_of(b, addr)];
}

/* Read-only flavour, for the marker. */
CVM_INLINE ObjectMeta const* bytemap_get_const(Bytemap const* b, u8 const* addr) {
    return &b->slots[bytemap_index_of(b, addr)];
}

/* The same slot in the next line. Used when clearing or sweeping line by
 * line: lines are contiguous, so this is a fixed stride. */
CVM_INLINE ObjectMeta* bytemap_next_line(ObjectMeta* cursor) {
    return cursor + CVM_SLOTS_PER_LINE;
}

#endif /* CVM_GC_BYTEMAP_H */
