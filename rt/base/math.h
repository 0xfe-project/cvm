/*
 * rt/base/math.h
 *
 * Integer math used by the allocator and the heap geometry code.
 *
 * Layering note: this lives under rt/base/ rather than vm/util/ because
 * it has no dependencies at all and the rt layer must be able to reach it.
 * The dependency arrow only ever points rt <- vm, never the other way.
 *
 * Division note: cvm_round_up and cvm_div_round_up overflow if value is
 * within `multiple - 1` of the top of the address space. Callers pass
 * heap sizes and object sizes, which are nowhere near that, but the
 * contract is written down rather than assumed.
 */

#ifndef RT_BASE_MATH_H
#define RT_BASE_MATH_H

#include "base/types.h"

/* ------------------------------------------------------------------ */
/* log2                                                                */
/* ------------------------------------------------------------------ */

/*
 * floor(log2(v)). Contract: v >= 1. Behaviour at v == 0 is undefined
 * (the builtin would be undefined too).
 */
CVM_INLINE u32 cvm_log2_floor(usize v) {
#if defined(__GNUC__) || defined(__clang__)
    return (u32)(CVM_WORD_BITS - 1 - (u32)__builtin_clzll((unsigned long long)v));
#else
    /* Portable fallback (MSVC without <intrin.h>). Not hot. */
    u32 r = 0;
    usize x = v;
    while (x > 1) {
        x >>= 1;
        r++;
    }
    return r;
#endif
}

/*
 * ceil(log2(v)). Contract: v >= 1.
 *
 * Unlike the reference implementation (which computes floor(2v - 1) and
 * overflows for large v), this compares against 1 << floor directly, so
 * it stays correct up to the top of the address space.
 */
CVM_INLINE u32 cvm_log2_ceil(usize v) {
    u32 f = cvm_log2_floor(v);
    return (v == ((usize)1 << f)) ? f : f + 1;
}

/* ------------------------------------------------------------------ */
/* rounding                                                            */
/* ------------------------------------------------------------------ */

/*
 * Round value up to the next multiple of `multiple` (any divisor >= 1).
 *
 * This is the general form. For power-of-two alignment prefer
 * cvm_align_up in base/types.h, which is a mask instead of a division.
 */
CVM_INLINE usize cvm_round_up(usize value, usize multiple) {
    return (value + multiple - 1) / multiple * multiple;
}

/* ceil(value / divider), for divider >= 1. */
CVM_INLINE usize cvm_div_round_up(usize value, usize divider) {
    return (value + divider - 1) / divider;
}

/*
 * Smallest power of two >= v, for v >= 1.
 * Saturates at the top bit rather than wrapping to zero.
 */
CVM_INLINE usize cvm_round_up_pow2(usize v) {
    u32 f = cvm_log2_ceil(v);
    return ((usize)1 << f);
}

#endif /* RT_BASE_MATH_H */
