/*
 * vm/str/dtoa.h
 *
 * f64 to text, and text to f64.
 *
 * Ported from refs/quickjs-0.17.0/dtoa.c (Fabrice Bellard, MIT) — the
 * implementation quickjs-ng uses in place of libbf. Two things are
 * deliberately not as they are upstream:
 *
 *   - Names carry our prefix and the types are ours (i32/f64/usize), because
 *     nothing under vm/ may depend on a raw C scalar or on a libc type.
 *
 *   - cvm_atod takes `str`, not a NUL-terminated pointer. This codebase
 *     assumes no terminator anywhere, and a caller holding a slice of a larger
 *     buffer — a JSON parser, say — has none to give.
 *
 * Why this implementation and not a faster one: see bench/dtoa/README.md. The
 * short of it is that the fastest alternative we had parses about 3.6% of
 * ordinary JSON numbers to the wrong side of a ulp and loses the sign of -0,
 * while this one agrees with correctly-rounded `strtod` on every value in the
 * corpus, in both directions.
 *
 * Both directions are correctly rounded (nearest, ties to even), and the
 * formatting is *shortest*: the fewest digits that still read back as the same
 * value.
 */

#ifndef CVM_STR_DTOA_H
#define CVM_STR_DTOA_H

#include "base/types.h"

/* Largest number of digits either direction will produce. */
#define CVM_DTOA_MAX_DIGITS 101

/* Radix 2..36. */
#define CVM_RADIX_MAX 36

/* --- formatting flags ------------------------------------------------- */

/* Radix other than 10 only supports CVM_DTOA_FORMAT_FREE. */

/* As many digits as the value needs (the shortest exact form). */
#define CVM_DTOA_FORMAT_FREE ((u32)0 << 0)
/* Exactly n_digits significant digits: 1 <= n_digits <= MAX_DIGITS. */
#define CVM_DTOA_FORMAT_FIXED ((u32)1 << 0)
/* Fractional form, n_digits digits after the point: 0 <= n_digits <= ... */
#define CVM_DTOA_FORMAT_FRAC ((u32)2 << 0)
#define CVM_DTOA_FORMAT_MASK ((u32)3 << 0)

/* Whether an exponent is used: auto decides from the magnitude. */
#define CVM_DTOA_EXP_AUTO ((u32)0 << 2)
#define CVM_DTOA_EXP_ENABLED ((u32)1 << 2)
#define CVM_DTOA_EXP_DISABLED ((u32)2 << 2)
#define CVM_DTOA_EXP_MASK ((u32)3 << 2)

/*
 * Print the sign of a negative zero.
 *
 * On by default upstream is off, which loses -0 on the way through text. A
 * language that distinguishes the two (and JavaScript does) has to ask for the
 * sign back.
 */
#define CVM_DTOA_MINUS_ZERO ((u32)1 << 4)

/* --- parsing flags ---------------------------------------------------- */

/* Accept only an integer: no dot, no exponent. */
#define CVM_ATOD_INT_ONLY ((u32)1 << 0)
/* With radix 0, also accept the 0o and 0b prefixes. */
#define CVM_ATOD_ACCEPT_BIN_OCT ((u32)1 << 1)
/* With radix 0, accept a leading 0 as octal (Annex B). */
#define CVM_ATOD_ACCEPT_LEGACY_OCTAL ((u32)1 << 2)
/* Accept '_' as a separator between digits. */
#define CVM_ATOD_ACCEPT_UNDERSCORES ((u32)1 << 3)

/* ------------------------------------------------------------------ */
/* scratch space                                                       */
/* ------------------------------------------------------------------ */

/*
 * Both directions need multiprecision scratch, and both take it from the
 * caller rather than allocating: a conversion must not be able to fail, and it
 * must be usable with no allocator in reach at all.
 *
 * These are sized for the worst case (a 1072-bit significand scaled by the
 * largest radix exponent) and are meant to sit on the stack at the call site.
 */
typedef struct CvmDtoaTempMem {
    u64 mem[37];
} CvmDtoaTempMem;

typedef struct CvmAtodTempMem {
    u64 mem[27];
} CvmAtodTempMem;

/* ------------------------------------------------------------------ */
/* conversion                                                          */
/* ------------------------------------------------------------------ */

/*
 * An upper bound on the length cvm_dtoa will write, so a caller can size a
 * buffer without knowing the value's digits. `buf` must have room for this
 * plus one if the caller wants to append a NUL itself.
 */
i32 cvm_dtoa_max_len(f64 d, i32 radix, i32 n_digits, u32 flags);

/*
 * Format `d` into `buf` and return the bytes written. No NUL is appended; the
 * return value is the length, as everywhere else here.
 *
 * `radix` is 2..36, and 10 is the only one that supports formats other than
 * FREE. `n_digits` is used by FIXED and FRAC and ignored by FREE.
 */
i32 cvm_dtoa(u8* buf, f64 d, i32 radix, i32 n_digits, u32 flags,
             CvmDtoaTempMem* tmp_mem);

/*
 * Parse a number from the front of `s`.
 *
 * `*consumed`, which may be NULL, receives how many bytes of `s` the number
 * occupied. Zero means nothing number-like was at the front. On a malformed
 * number the result is NaN and `consumed` is as far as the scan got, which is
 * what lets a caller tell "not a number" from "a number that ran out of
 * digits" — a distinction a parser reporting a syntax error needs.
 *
 * `radix` of 0 means "detect": a 0x/0o/0b prefix selects the radix, otherwise
 * 10.
 */
f64 cvm_atod(str s, usize* consumed, i32 radix, u32 flags, CvmAtodTempMem* tmp_mem);

/* ------------------------------------------------------------------ */
/* integers                                                            */
/* ------------------------------------------------------------------ */

/*
 * Integer to text, which is not the same operation as f64 to text and should
 * not go through it: an integer has an exact decimal form and needs no digit
 * search. These write no NUL and return the length.
 *
 * The radix forms take 2..36 and use lowercase for digits above 9.
 */
usize cvm_u32toa(u8* buf, u32 n);
usize cvm_i32toa(u8* buf, i32 n);
usize cvm_u64toa(u8* buf, u64 n);
usize cvm_i64toa(u8* buf, i64 n);
usize cvm_u64toa_radix(u8* buf, u64 n, u32 radix);
usize cvm_i64toa_radix(u8* buf, i64 n, u32 radix);

#endif /* CVM_STR_DTOA_H */
