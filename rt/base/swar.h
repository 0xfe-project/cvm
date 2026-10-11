/*
 * rt/base/swar.h
 *
 * SWAR: SIMD within a register. Eight bytes at a time, in a word.
 *
 * Transcribed from refs/whippet/src/swar.h, which carries its own
 * explanation: the operations are the ones a marker and a sweeper do to a
 * per-slot byte map -- "does any of these eight slots match", "how far to the
 * next one that does" -- and doing them a word at a time is the difference
 * between a pass over the metadata costing a load per slot and costing a load
 * per eight slots.
 *
 * Freestanding: no libc, so memcpy is __builtin_memcpy and that is the only
 * dependency. Everything else is __builtin_*, which the compiler either
 * lowers to one instruction or to the obvious short sequence.
 *
 * Little-endian throughout, like the reference: byte i of the conceptual
 * array is bits [8i, 8i+8) of the word, so count_zero_bytes returns an index
 * into the array. A big-endian target byte-swaps on load and store, as the
 * reference does; we do not have one, and the assert is that we do not.
 */

#ifndef RT_BASE_SWAR_H
#define RT_BASE_SWAR_H

#include "base/assert.h"
#include "base/types.h"

/* How many bytes precede the lowest set bit, or 8 if none. */
CVM_INLINE u8 rt_swar_count_zero_bytes(u64 bytes) {
    return bytes ? (u8)(__builtin_ctzll(bytes) / 8) : (u8)sizeof(bytes);
}

/* One byte, in all eight lanes. */
CVM_INLINE u64 rt_swar_broadcast_byte(u8 byte) {
    return (u64)byte * 0x0101010101010101ULL;
}

/* Eight bytes from an aligned address, as one word. */
CVM_INLINE u64 rt_swar_load_eight_aligned_bytes(u8 const* ptr) {
    cvm_assert(((usize)ptr & 7) == 0);
    u64 word;
    __builtin_memcpy(&word, ptr, sizeof(word));
    return word;
}

/*
 * The bytes that have all of `mask`'s bits set.
 *
 * Not equality: a byte with mask's bits plus others still matches. That is
 * the right question for a state byte whose upper bits are flags -- "find
 * the next slot whose state is MARKED" has to match MARKED-with-weak and
 * MARKED-with-finalizer too, or those objects are dropped on the floor
 * silently.
 */
CVM_INLINE u64 rt_swar_match_bytes_against_bits(u64 bytes, u8 mask) {
    return bytes & rt_swar_broadcast_byte(mask);
}

/*
 * The first byte among `limit` that has all of `mask`'s bits set, or
 * `limit` if none does.
 *
 * The unaligned head is handled by loading the aligned group it sits in and
 * shifting: eight bytes' work for one byte's answer, and no special case in
 * the caller.
 */
CVM_INLINE usize rt_swar_scan_for_byte_with_bits(u8 const* ptr, usize limit, u8 mask) {
    usize n = 0;
    usize unaligned = (usize)ptr & 7;
    if (unaligned != 0) {
        u64 bytes =
            rt_swar_load_eight_aligned_bytes(ptr - unaligned) >> (unaligned * 8);
        u64 match = rt_swar_match_bytes_against_bits(bytes, mask);
        if (match != 0) {
            return rt_swar_count_zero_bytes(match);
        }
        n += 8 - unaligned;
    }

    for (; n < limit; n += 8) {
        u64 bytes = rt_swar_load_eight_aligned_bytes(ptr + n);
        u64 match = rt_swar_match_bytes_against_bits(bytes, mask);
        if (match != 0) {
            return n + rt_swar_count_zero_bytes(match);
        }
    }

    return limit;
}

#endif /* RT_BASE_SWAR_H */
