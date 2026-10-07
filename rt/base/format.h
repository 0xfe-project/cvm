/*
 * rt/base/format.h
 *
 * A tiny bounded formatter, shared by rt and vm.
 *
 * There is no printf here and there will not be one: vm/ is freestanding,
 * and printf is a libc symbol. Formatting is done by appending to a
 * caller-provided buffer, which also means the caller decides the upper
 * bound on cost and stack use.
 *
 * Truncation is recorded rather than silent. Text is `str`, never
 * NUL-terminated, so a message that does not fit is simply cut short and
 * `truncated` is set; the caller can then say so instead of emitting a
 * plausible-looking half line.
 */

#ifndef RT_BASE_FORMAT_H
#define RT_BASE_FORMAT_H

#include "base/types.h"

typedef struct CvmFmt {
    u8* buf;
    usize cap;
    usize len;
    bool truncated;
} CvmFmt;

CVM_INLINE void fmt_init(CvmFmt* f, u8* buf, usize cap) {
    f->buf = buf;
    f->cap = cap;
    f->len = 0;
    f->truncated = false;
}

/* One byte. Sets `truncated` instead of overflowing. */
CVM_INLINE void fmt_byte(CvmFmt* f, u8 c) {
    if (f->len < f->cap) {
        f->buf[f->len++] = c;
    } else {
        f->truncated = true;
    }
}

/* Bytes, cut to whatever still fits. */
CVM_INLINE void fmt_bytes(CvmFmt* f, span_const s) {
    for (usize i = 0; i < s.len; i++) {
        fmt_byte(f, s.ptr[i]);
    }
}

/* Text. */
CVM_INLINE void fmt_str(CvmFmt* f, str s) {
    for (usize i = 0; i < s.len; i++) {
        fmt_byte(f, s.ptr[i]);
    }
}

/*
 * A literal that is known at compile time. Prefer this over fmt_str with
 * STR_LITERAL: it skips a strlen the compiler already knows the answer to,
 * and it works on `char[]` without a cast.
 */
#define fmt_lit(f, lit) fmt_bytes((f), (span_const){(u8 const*)(lit), sizeof(lit) - 1})

/* Unsigned decimal, no padding. */
CVM_INLINE void fmt_u64(CvmFmt* f, u64 v) {
    u8 tmp[20]; /* 2^64-1 has 20 digits */
    usize n = 0;
    do {
        tmp[n++] = (u8)('0' + (u8)(v % 10u));
        v /= 10u;
    } while (v != 0);
    while (n > 0) {
        fmt_byte(f, tmp[--n]);
    }
}

CVM_INLINE void fmt_u32(CvmFmt* f, u32 v) {
    fmt_u64(f, (u64)v);
}

/* Signed decimal. Handles the most negative value without overflowing. */
CVM_INLINE void fmt_i64(CvmFmt* f, i64 v) {
    if (v < 0) {
        fmt_byte(f, (u8)'-');
        /* Negating i64 directly overflows at the minimum value; shifting
         * the range by one first keeps it representable. */
        fmt_u64(f, (u64)(-(v + 1)) + 1u);
    } else {
        fmt_u64(f, (u64)v);
    }
}

CVM_INLINE void fmt_i32(CvmFmt* f, i32 v) {
    fmt_i64(f, (i64)v);
}

/* Lowercase hex with a 0x prefix. Zero prints as "0x0". */
CVM_INLINE void fmt_hex(CvmFmt* f, u64 v) {
    fmt_lit(f, "0x");
    u8 tmp[16];
    usize n = 0;
    do {
        u8 d = (u8)(v & 0xfu);
        tmp[n++] = (u8)(d < 10u ? (u8)('0' + d) : (u8)('a' + (d - 10u)));
        v >>= 4;
    } while (v != 0);
    while (n > 0) {
        fmt_byte(f, tmp[--n]);
    }
}

/* Fixed-width lowercase hex, for addresses that should line up in a
 * column. `digits` is clamped to 16. */
CVM_INLINE void fmt_hex_padded(CvmFmt* f, u64 v, u32 digits) {
    if (digits > 16u)
        digits = 16u;
    fmt_lit(f, "0x");
    for (u32 i = digits; i > 0; i--) {
        u8 d = (u8)((v >> (4u * (i - 1u))) & 0xfu);
        fmt_byte(f, (u8)(d < 10u ? (u8)('0' + d) : (u8)('a' + (d - 10u))));
    }
}

CVM_INLINE void fmt_bool(CvmFmt* f, bool b) {
    if (b) {
        fmt_lit(f, "true");
    } else {
        fmt_lit(f, "false");
    }
}

/* The formatted bytes. Never NUL-terminated; check `truncated` first. */
CVM_INLINE str fmt_result(CvmFmt const* f) {
    return (str){.ptr = f->buf, .len = f->len};
}

#endif /* RT_BASE_FORMAT_H */
