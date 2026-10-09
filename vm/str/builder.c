/*
 * vm/str/builder.c
 *
 * The string builder. See include/cvm/builder.h for why its scratch is off the
 * heap, and what it deliberately is not.
 *
 * The growth and widening rules are the same shape as the reference
 * implementation's (quickjs's string_buffer_*), which is where the design comes
 * from: start narrow, keep one contiguous run of units, widen in place the first
 * time a unit needs 16 bits, and grow by a factor rather than by a fixed step.
 * What is ours is the storage, and the consequence that nothing here ever needs
 * a frame.
 */

#include "cvm/builder.h"

#include "base/assert.h"
#include "base/types.h"
#include "base/utf8.h"
#include "rt.h"
#include "str/dtoa.h"

/* ------------------------------------------------------------------ */
/* storage                                                             */
/* ------------------------------------------------------------------ */

CVM_INLINE usize unit_bytes(CvmStrBuf const* b) {
    return b->wide ? 2u : 1u;
}

CVM_INLINE u16* units16(CvmStrBuf const* b) {
    return (u16*)b->data;
}

CVM_INLINE u8* units8(CvmStrBuf const* b) {
    return b->data;
}

/*
 * The growth rule, and the two floors it needs.
 *
 * 1.5x, because this allocation comes from the platform allocator and not from
 * the collector: a doubling sequence is the usual choice for a malloc'd buffer,
 * and 1.5x trades a few more copies for smaller peak waste on the large
 * intermediate strings a builder tends to produce.
 *
 * From a capacity of one, 1.5x is one, so the result is at least capacity + 1.
 * And a young builder is not reallocated every other append: anything under 64
 * units jumps straight to 64, which is one allocation instead of four for the
 * common short-string case.
 */
CVM_INLINE usize grown_capacity(usize cap) {
    usize next = cap + cap / 2u;
    if (next < cap + 1u) {
        next = cap + 1u;
    }
    if (next < 64u) {
        next = 64u;
    }
    return next;
}

static bool reserve(CvmStrBuf* b, usize extra) {
    if (b->failed) {
        return false;
    }
    if (b->len + extra <= b->cap) {
        return true;
    }

    usize want = b->cap;
    while (want < b->len + extra) {
        want = grown_capacity(want);
    }

    usize bytes = want * unit_bytes(b);
    /*
     * The units are a byte prefix either way — the width of each unit is not the
     * buffer's business — so this is one realloc, not an alloc-copy-free. A
     * platform that can extend in place does no copy at all.
     */
    u8* fresh = rt_mem_realloc(b->data, bytes);
    if (fresh == NULL) {
        b->failed = true;
        return false;
    }

    b->data = fresh;
    b->cap = want;
    return true;
}

bool cvm_str_buf_reserve(CvmStrBuf* b, usize units) {
    return reserve(b, units);
}

/* ------------------------------------------------------------------ */
/* lifetime                                                            */
/* ------------------------------------------------------------------ */

void cvm_str_buf_init(CvmStrBuf* b) {
    b->data = NULL;
    b->len = 0;
    b->cap = 0;
    b->wide = false;
    b->failed = false;
}

void cvm_str_buf_free(CvmStrBuf* b) {
    if (b->data != NULL) {
        rt_mem_free(b->data);
    }
    cvm_str_buf_init(b);
}

/* ------------------------------------------------------------------ */
/* widening                                                            */
/* ------------------------------------------------------------------ */

bool cvm_str_buf_widen(CvmStrBuf* b) {
    if (b->failed) {
        return false;
    }
    if (b->wide) {
        return true;
    }

    /*
     * A builder that has not allocated yet has no capacity to double, and
     * rt_mem_alloc(0) is a failure rather than a zero-byte buffer — so a builder
     * whose first unit needs 16 bits has to start somewhere.
     */
    usize want = b->cap == 0u ? 64u : b->cap;
    usize bytes = want * 2u;
    /*
     * Grow the buffer, then re-lay it out in place: the wide form is not the
     * narrow form's prefix — each unit becomes two bytes — but the narrow bytes
     * are all below the positions they end up at, so reading backwards over them
     * is safe. It is the reason memmove exists, done by hand because the element
     * size changes as well as the direction.
     */
    u8* fresh = rt_mem_realloc(b->data, bytes);
    if (fresh == NULL) {
        b->failed = true;
        return false;
    }

    u8* src = fresh;
    u16* dst = (u16*)fresh;
    for (usize i = b->len; i > 0; i--) {
        dst[i - 1] = src[i - 1];
    }

    b->data = fresh;
    b->cap = want;
    b->wide = true;
    return true;
}

/* ------------------------------------------------------------------ */
/* appending                                                           */
/* ------------------------------------------------------------------ */

bool cvm_str_buf_put_unit(CvmStrBuf* b, u32 unit) {
    if (b->failed) {
        return false;
    }
    if (unit > 0xFFu && !b->wide && !cvm_str_buf_widen(b)) {
        return false;
    }
    if (b->wide && unit > 0xFFFFu) {
        /* A code unit is 16 bits by definition. Asking for more is a caller bug,
         * and reporting it is better than truncating it: the builder fails, the
         * failure is sticky, and freezing it gives NULL rather than a string
         * with a character quietly missing. */
        b->failed = true;
        return false;
    }
    if (!reserve(b, 1)) {
        return false;
    }

    if (b->wide) {
        units16(b)[b->len] = (u16)unit;
    } else {
        units8(b)[b->len] = (u8)unit;
    }
    b->len++;
    return true;
}

bool cvm_str_buf_put_rune(CvmStrBuf* b, rune c) {
    if (b->failed) {
        return false;
    }
    if (c > CVM_RUNE_MAX) {
        b->failed = true;
        return false;
    }
    if (c < (rune)0x10000u) {
        return cvm_str_buf_put_unit(b, (u32)c);
    }

    /*
     * A supplementary code point is a surrogate pair, which is the one place a
     * code point becomes two units. The arithmetic is the UTF-16 encoding rule
     * and is worth stating: subtract 0x10000 to get a 20-bit value, and split it
     * into two 10-bit halves biased into the surrogate ranges.
     */
    if (!cvm_str_buf_widen(b)) {
        return false;
    }
    u32 v = (u32)c - 0x10000u;
    if (!cvm_str_buf_put_unit(b, 0xD800u + (v >> 10))) {
        return false;
    }
    return cvm_str_buf_put_unit(b, 0xDC00u + (v & 0x3FFu));
}

bool cvm_str_buf_put_bytes(CvmStrBuf* b, str text) {
    if (b->failed) {
        return false;
    }
    if (!reserve(b, text.len)) {
        return false;
    }
    for (usize i = 0; i < text.len; i++) {
        if (b->wide) {
            units16(b)[b->len] = text.ptr[i];
        } else {
            units8(b)[b->len] = text.ptr[i];
        }
        b->len++;
    }
    return true;
}

bool cvm_str_buf_put_utf8(CvmStrBuf* b, str text) {
    if (b->failed) {
        return false;
    }

    usize i = 0;
    while (i < text.len) {
        rune r = 0;
        if (cvm_utf8_next(text, &i, &r) != CVM_UTF8_RUNE) {
            b->failed = true;
            return false;
        }
        if (!cvm_str_buf_put_rune(b, r)) {
            return false;
        }
    }
    return true;
}

bool cvm_str_buf_put_string(CvmStrBuf* b, CvmString const* s) {
    if (b->failed) {
        return false;
    }

    usize length = cvm_string_length(s);
    if (cvm_string_is_wide(s) && !b->wide && !cvm_str_buf_widen(b)) {
        return false;
    }
    if (!reserve(b, length)) {
        return false;
    }

    for (usize i = 0; i < length; i++) {
        if (b->wide) {
            units16(b)[b->len] = (u16)cvm_string_unit(s, i);
        } else {
            units8(b)[b->len] = (u8)cvm_string_unit(s, i);
        }
        b->len++;
    }
    return true;
}

bool cvm_str_buf_fill(CvmStrBuf* b, u32 unit, usize count) {
    if (b->failed) {
        return false;
    }
    if (unit > 0xFFu && !b->wide && !cvm_str_buf_widen(b)) {
        return false;
    }
    if (!reserve(b, count)) {
        return false;
    }

    for (usize i = 0; i < count; i++) {
        if (b->wide) {
            units16(b)[b->len] = (u16)unit;
        } else {
            units8(b)[b->len] = (u8)unit;
        }
        b->len++;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* numbers                                                             */
/* ------------------------------------------------------------------ */

/*
 * A number never needs more than this: the longest f64 spelling, plus room for a
 * sign and a terminator that the conversions do not write.
 */
#define NUMBER_CAP 32

bool cvm_str_buf_put_u64(CvmStrBuf* b, u64 value) {
    u8 buf[NUMBER_CAP];
    return cvm_str_buf_put_bytes(b, (str){buf, cvm_u64toa(buf, value)});
}

bool cvm_str_buf_put_i64(CvmStrBuf* b, i64 value) {
    u8 buf[NUMBER_CAP];
    return cvm_str_buf_put_bytes(b, (str){buf, cvm_i64toa(buf, value)});
}

bool cvm_str_buf_put_f64(CvmStrBuf* b, f64 value) {
    u8 buf[NUMBER_CAP];
    CvmDtoaTempMem tmp;
    i32 n =
        cvm_dtoa(buf, value, 10, 0, CVM_DTOA_FORMAT_FREE | CVM_DTOA_MINUS_ZERO, &tmp);
    if (n <= 0) {
        b->failed = true;
        return false;
    }
    return cvm_str_buf_put_bytes(b, (str){buf, (usize)n});
}

/* ------------------------------------------------------------------ */
/* freezing                                                            */
/* ------------------------------------------------------------------ */

CvmString* cvm_str_buf_finish(CvmStrBuf* b) {
    if (b->failed) {
        cvm_str_buf_free(b);
        return NULL;
    }

    /*
     * The one heap allocation in a builder's life, and the only place a caller
     * has to think about the collector: the content is off-heap and stays valid
     * across it, and there is nothing else held.
     */
    CvmString* s = b->wide ? cvm_string_new16((u16 const*)b->data, b->len)
                           : cvm_string_new8(b->data, b->len);

    cvm_str_buf_free(b);
    return s;
}
