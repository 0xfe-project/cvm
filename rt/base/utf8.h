/*
 * rt/base/utf8.h
 *
 * UTF-8 on one side, runes on the other.
 *
 * This is the text transcoding that is not about any particular string
 * representation: bytes in, code points out. Whether those code points
 * then become a narrow string, a wide string or a regex subject is a
 * question for the layers above.
 *
 * ---------------------------------------------------------------------
 * Three outcomes, not two
 * ---------------------------------------------------------------------
 *
 * A decoder that returns a code point has nowhere to put "this is not
 * UTF-8" and "this is the end" at the same time, so it ends up folding
 * one into the other: quickjs's returns U+FFFD and consumes a byte, which
 * makes a malformed sequence indistinguishable from a literal U+FFFD in
 * the text. That is the right trade for a lenient consumer and the wrong
 * one for a validator, and we need both.
 *
 * So the status is explicit and the three outcomes are separate:
 *
 *   CVM_UTF8_RUNE  one rune, `*out` set, offset advanced
 *   CVM_UTF8_END   the offset is at the end of the text
 *   CVM_UTF8_BAD   malformed at the offset, which is left unchanged
 *
 * BAD not advancing is deliberate. It leaves the decision to the caller:
 * a validator stops there, a resynchronising scanner skips one byte and
 * tries again, and a replacement-character policy emits U+FFFD. None of
 * those has to be chosen here, and staying put is what makes them all
 * expressible.
 *
 * ---------------------------------------------------------------------
 * Strict, or WTF-8
 * ---------------------------------------------------------------------
 *
 * CVM_UTF8_STRICT is RFC 3629: surrogates, overlong encodings, truncated
 * sequences and anything above U+10FFFF are all BAD.
 *
 * CVM_UTF8_WTF8 accepts UTF-8-encoded surrogates as well. This is not
 * laxity for its own sake. A string whose code units are 16-bit can hold
 * a lone surrogate — it is what an escape such as "\uD800" means — and
 * if converting that string to UTF-8 rejected the value, the text could
 * not survive a round trip. quickjs makes the same allowance for the same
 * reason.
 *
 * The encoder is total over the code point range and writes surrogates in
 * WTF-8 form, so `encode` and `decode` are inverses in both directions
 * and no caller has to pre-check a value before storing it. Strictness,
 * when it is wanted, is asked for by name: cvm_utf8_valid.
 */

#ifndef RT_BASE_UTF8_H
#define RT_BASE_UTF8_H

#include "base/types.h"

/* Longest UTF-8 encoding of one code point. */
#define CVM_UTF8_MAX_BYTES 4u

/* The largest code point, and the substitution character. */
#define CVM_RUNE_MAX ((rune)0x10FFFFu)
#define CVM_RUNE_REPLACEMENT ((rune)0xFFFDu)

typedef enum CvmUtf8Status {
    CVM_UTF8_RUNE = 0, /* one rune decoded into *out */
    CVM_UTF8_END,      /* offset is at the end of the text */
    CVM_UTF8_BAD,      /* malformed here; offset is unchanged */
} CvmUtf8Status;

typedef enum CvmUtf8Mode {
    /* RFC 3629. Surrogates are malformed. */
    CVM_UTF8_STRICT = 0,
    /* WTF-8: surrogates are accepted, so a 16-bit unit round-trips. */
    CVM_UTF8_WTF8 = 1,
} CvmUtf8Mode;

/*
 * How many bytes the sequence starting at `lead` occupies, or 0 if `lead`
 * cannot start one.
 *
 * Only the lead byte is examined, so this is not validation: a 0xC2 whose
 * continuation byte is missing is a 2 here and BAD from the decoder. What
 * it is for is sizing a destination buffer and rejecting a lead byte
 * before touching memory after it.
 *
 * Note the two gaps, both of them overlong-encoding protection: 0xC0 and
 * 0xC1 could only ever express a value below 0x80, and 0xF5 through 0xFF
 * start a value above U+10FFFF.
 */
CVM_INLINE u32 cvm_utf8_seq_len(u8 lead) {
    if (lead < 0x80u) {
        return 1u;
    }
    if (lead >= 0xC2u && lead <= 0xDFu) {
        return 2u;
    }
    if (lead >= 0xE0u && lead <= 0xEFu) {
        return 3u;
    }
    if (lead >= 0xF0u && lead <= 0xF4u) {
        return 4u;
    }
    return 0u;
}

/*
 * Decode one rune at `*offset`.
 *
 * On CVM_UTF8_RUNE, `*out` holds the code point and `*offset` has moved
 * past it. On CVM_UTF8_END and CVM_UTF8_BAD, neither is written.
 */
CVM_INLINE CvmUtf8Status cvm_utf8_next_mode(str s, usize* offset, rune* out,
                                            CvmUtf8Mode mode) {
    usize i = *offset;
    if (i >= s.len) {
        return CVM_UTF8_END;
    }

    u8 lead = s.ptr[i];
    if (lead < 0x80u) {
        *out = (rune)lead;
        *offset = i + 1;
        return CVM_UTF8_RUNE;
    }

    u32 seq = cvm_utf8_seq_len(lead);
    if (seq == 0u) {
        return CVM_UTF8_BAD;
    }

    /*
     * Total bytes are `seq`, of which one has been read; if the text does
     * not hold the rest, the sequence is truncated. It cannot be salvaged
     * by waiting, because there is nothing to wait for — str has a length
     * and this is the end of it.
     */
    u32 rest = seq - 1u;
    if (i + (usize)rest >= s.len) {
        return CVM_UTF8_BAD;
    }

    /*
     * The first continuation byte carries the constraints that rule out
     * the short forms of a longer sequence: an overlong encoding (0xE0
     * followed by 0x80..0x9F would be a two-byte value) or a value above
     * the maximum (0xF4 followed by 0x90.. would exceed U+10FFFF).
     */
    u8 lo = 0x80u;
    u8 hi = 0xBFu;
    if (lead == 0xE0u) {
        lo = 0xA0u;
    } else if (lead == 0xEDu && mode == CVM_UTF8_STRICT) {
        /* 0xED 0xA0.. is exactly the surrogate range. */
        hi = 0x9Fu;
    } else if (lead == 0xF0u) {
        lo = 0x90u;
    } else if (lead == 0xF4u) {
        hi = 0x8Fu;
    }

    u8 c1 = s.ptr[i + 1];
    if (c1 < lo || c1 > hi) {
        return CVM_UTF8_BAD;
    }

    rune c = (rune)(lead & (u8)(0xFFu >> (seq + 1u)));
    c = (c << 6) | (rune)(c1 & 0x3Fu);
    for (u32 k = 2u; k < seq; k++) {
        u8 ck = s.ptr[i + (usize)k];
        if (ck < 0x80u || ck > 0xBFu) {
            return CVM_UTF8_BAD;
        }
        c = (c << 6) | (rune)(ck & 0x3Fu);
    }

    *out = c;
    *offset = i + (usize)seq;
    return CVM_UTF8_RUNE;
}

/* Strict, which is the right default for text that came from outside. */
CVM_INLINE CvmUtf8Status cvm_utf8_next(str s, usize* offset, rune* out) {
    return cvm_utf8_next_mode(s, offset, out, CVM_UTF8_STRICT);
}

/*
 * Bytes needed to encode `r`, or 0 if `r` is above U+10FFFF and is
 * therefore not a code point at all.
 *
 * Surrogates are given the 3 bytes they need: see the note at the top on
 * why the encoder is total over the range.
 */
CVM_INLINE u32 cvm_utf8_encode_len(rune r) {
    if (r < (rune)0x80u) {
        return 1u;
    }
    if (r < (rune)0x800u) {
        return 2u;
    }
    if (r < (rune)0x10000u) {
        return 3u;
    }
    if (r <= CVM_RUNE_MAX) {
        return 4u;
    }
    return 0u;
}

/*
 * Encode `r` at `out`, which must have room for CVM_UTF8_MAX_BYTES.
 * Returns the bytes written, or 0 if `r` is above U+10FFFF.
 *
 * No NUL is appended: the length is the return value, as everywhere else
 * in this codebase.
 */
CVM_INLINE u32 cvm_utf8_encode(rune r, u8* out) {
    u32 n = cvm_utf8_encode_len(r);
    switch (n) {
    case 1u:
        out[0] = (u8)r;
        break;
    case 2u:
        out[0] = (u8)(0xC0u | (r >> 6));
        out[1] = (u8)(0x80u | (r & 0x3Fu));
        break;
    case 3u:
        out[0] = (u8)(0xE0u | (r >> 12));
        out[1] = (u8)(0x80u | ((r >> 6) & 0x3Fu));
        out[2] = (u8)(0x80u | (r & 0x3Fu));
        break;
    case 4u:
        out[0] = (u8)(0xF0u | (r >> 18));
        out[1] = (u8)(0x80u | ((r >> 12) & 0x3Fu));
        out[2] = (u8)(0x80u | ((r >> 6) & 0x3Fu));
        out[3] = (u8)(0x80u | (r & 0x3Fu));
        break;
    default:
        break;
    }
    return n;
}

/*
 * Whether the whole of `s` is well-formed UTF-8 under the strict rules.
 *
 * A UTF-8 BOM is a code point like any other here: this asks whether the
 * bytes decode, not whether the text means what the sender intended.
 */
CVM_INLINE bool cvm_utf8_valid(str s) {
    usize i = 0;
    rune r = 0;
    while (i < s.len) {
        if (cvm_utf8_next(s, &i, &r) != CVM_UTF8_RUNE) {
            return false;
        }
    }
    return true;
}

/*
 * Number of runes in `s`. That is not `s.len` unless the text is ASCII:
 * the whole reason to decode is that a code point can cost up to four
 * bytes.
 *
 * `*valid`, when not NULL, reports whether the text was well-formed; a
 * malformed sequence is skipped one byte at a time and counted as one
 * rune each, so the count stays a count of what a lenient consumer would
 * see.
 */
CVM_INLINE usize cvm_utf8_count(str s, bool* valid) {
    usize i = 0;
    usize count = 0;
    bool ok = true;
    rune r = 0;

    while (i < s.len) {
        CvmUtf8Status st = cvm_utf8_next(s, &i, &r);
        if (st == CVM_UTF8_RUNE) {
            count++;
        } else {
            ok = false;
            i++; /* resynchronise: one byte, one replacement */
            count++;
        }
    }

    if (valid != NULL) {
        *valid = ok;
    }
    return count;
}

#endif /* RT_BASE_UTF8_H */
