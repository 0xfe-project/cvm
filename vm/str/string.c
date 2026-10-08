/*
 * vm/str/string.c
 *
 * The runtime's string type. See include/cvm/str.h for the shape, for why the
 * units are 8 or 16 bits rather than UTF-8, and for what immutability is
 * load-bearing for.
 *
 * Everything here is one allocation. A string is its own storage — there is no
 * separate byte buffer to point at — and the collector knows how far it runs
 * because the length is in the object and the descriptor says the tail is a
 * leaf array.
 */

#include "cvm/str.h"

#include "base/assert.h"
#include "base/types.h"
#include "base/utf8.h"
#include "cvm/builder.h"
#include "mem.h"

/* ------------------------------------------------------------------ */
/* descriptors                                                         */
/* ------------------------------------------------------------------ */

/*
 * The element descriptors exist for one number: their size, which the
 * collector uses as the stride between units. They say nothing about what the
 * elements are, because there is nothing to say — a scalar is a leaf, and the
 * whole point of describing a string as a leaf array is that nothing inside it
 * is ever traced.
 *
 * The remaining descriptor fields are zero, which in C is what an omitted
 * designated initializer means. For these two that is also the correct value:
 * a scalar has no pointer fields and no embedded fields.
 */
static CvmTypeDescriptor const cvm_type_unit8 = {
    .size = 1,
    .kind = CVM_TYPE_SCALAR,
};

static CvmTypeDescriptor const cvm_type_unit16 = {
    .size = 2,
    .kind = CVM_TYPE_SCALAR,
};

/*
 * Two descriptors, one per representation, because the stride is part of the
 * type: it is how a length becomes an object's extent, and it is why a wide
 * string costs twice what a narrow one does without the collector having to be
 * told anything else.
 *
 * `size` is the fixed part — the header fields, up to but not including the
 * units — and the array bounds describe where the length and the tail are.
 */
static CvmTypeDescriptor const cvm_type_string8 = {
    .size = offsetof(CvmString, data),
    .kind = CVM_TYPE_SCALAR_ARRAY,
    .array_length_offset = offsetof(CvmString, length),
    .array_data_offset = offsetof(CvmString, data),
    .array_elem_type = &cvm_type_unit8,
};

static CvmTypeDescriptor const cvm_type_string16 = {
    .size = offsetof(CvmString, data),
    .kind = CVM_TYPE_SCALAR_ARRAY,
    .array_length_offset = offsetof(CvmString, length),
    .array_data_offset = offsetof(CvmString, data),
    .array_elem_type = &cvm_type_unit16,
};

/* ------------------------------------------------------------------ */
/* allocation                                                          */
/* ------------------------------------------------------------------ */

/*
 * A fresh string of `length` code units, zeroed by the allocator and with its
 * length already written into the object by cvm_alloc_array. The units are the
 * caller's to fill.
 *
 * Note what this does not do: it does not root anything, because it holds no
 * pointer across the allocation. A caller that does is responsible for its own
 * frame, and cvm_string_widen below is the example.
 */
static CvmString* string_alloc(bool wide, usize length) {
    CvmTypeDescriptor const* type = wide ? &cvm_type_string16 : &cvm_type_string8;
    usize stride = wide ? 2u : 1u;

    CvmString* s = cvm_alloc_array(type, length, stride);
    s->flags = wide ? CVM_STRF_WIDE : 0u;
    return s;
}

/* The unit stride of a string, in bytes. */
CVM_INLINE usize string_unit_size(CvmString const* s) {
    return cvm_string_is_wide(s) ? 2u : 1u;
}

/* ------------------------------------------------------------------ */
/* construction                                                        */
/* ------------------------------------------------------------------ */

CvmString* cvm_string_new8(u8 const* units, usize length) {
    cvm_assert(units != NULL || length == 0);

    CvmString* s = string_alloc(false, length);
    if (length != 0) {
        memcpy(s->data, units, length);
    }
    return s;
}

CvmString* cvm_string_new_ascii(str text) {
    CvmString* s = string_alloc(false, text.len);

    /*
     * Copied rather than asserted and then copied anyway: the check *is* the
     * copy on the fast path, and doing it separately would be a second pass
     * over text that is usually a literal.
     */
    for (usize i = 0; i < text.len; i++) {
        cvm_assert(text.ptr[i] < 0x80u);
        s->data[i] = text.ptr[i];
    }
    return s;
}

CvmString* cvm_string_new16(u16 const* units, usize length) {
    cvm_assert(units != NULL || length == 0);

    CvmString* s = string_alloc(true, length);
    if (length != 0) {
        memcpy(s->data, units, length * 2u);
    }
    return s;
}

CvmString* cvm_string_widen(CvmString* s) {
    if (cvm_string_is_wide(s)) {
        return s;
    }

    /*
     * This is the case the frame protocol exists for: the source is a live
     * string held in a local, the allocation below can collect, and the
     * collector does not scan the machine stack. Without the frame, a
     * collection triggered right here would free the very units being copied.
     */
    CVM_FRAME_DECL(f, 2);
    cvm_frame_push(&f_frame, f_roots, 2);
    f_roots[0] = s;

    CvmString* wide = string_alloc(true, s->length);
    f_roots[1] = wide;

    u8 const* src = ((CvmString*)f_roots[0])->data;
    u16* dst = (u16*)wide->data;
    usize length = ((CvmString*)f_roots[0])->length;
    for (usize i = 0; i < length; i++) {
        dst[i] = (u16)src[i];
    }

    cvm_frame_pop(&f_frame);
    return wide;
}

/* ------------------------------------------------------------------ */
/* hashing                                                             */
/* ------------------------------------------------------------------ */

/*
 * Units are hashed by *value*, so a text in either representation hashes the
 * same. That is not an optimisation: equal strings are equal across the two
 * representations, so they have to land in the same hash bucket, and a hash
 * over raw bytes would put them in different ones.
 */
CVM_INLINE u32 hash_step(u32 h, u32 unit) {
    return (h ^ unit) * 16777619u; /* FNV-1a */
}

u32 cvm_string_hash(CvmString* s) {
    if ((s->flags & CVM_STRF_HASHED) != 0u) {
        return s->hash;
    }

    /*
     * The seed is FNV's offset basis rather than zero so that an empty string
     * and a string of zero bytes do not both start from a zero state.
     */
    u32 h = 2166136261u;
    for (usize i = 0; i < s->length; i++) {
        h = hash_step(h, cvm_string_unit(s, i));
    }

    /*
     * An avalanche to finish. FNV alone leaves the low-entropy pattern of
     * short inputs in the high bits, and the hash table maps a bucket with a
     * Fibonacci multiply that reads exactly the high bits — so a table keyed
     * by short strings would cluster without this.
     */
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;

    s->hash = h;
    s->flags |= CVM_STRF_HASHED;
    return h;
}

/* ------------------------------------------------------------------ */
/* comparison                                                          */
/* ------------------------------------------------------------------ */

bool cvm_string_equal(CvmString const* a, CvmString const* b) {
    if (a == b) {
        return true;
    }
    if (a->length != b->length) {
        return false;
    }

    usize bytes = a->length * string_unit_size(a);
    if (cvm_string_is_wide(a) == cvm_string_is_wide(b)) {
        /* Same representation: the units are contiguous and compared as
         * bytes, which for equal-width units is the same as comparing them as
         * units. */
        return memcmp(a->data, b->data, bytes) == 0;
    }

    /* Mixed: the byte compare would compare a u8 against half of a u16, so
     * this goes unit by unit. It is the same question, asked in the only way
     * that is meaningful when the widths differ. */
    for (usize i = 0; i < a->length; i++) {
        if (cvm_string_unit(a, i) != cvm_string_unit(b, i)) {
            return false;
        }
    }
    return true;
}

i32 cvm_string_compare(CvmString const* a, CvmString const* b) {
    if (a == b) {
        return 0;
    }

    usize shorter = a->length < b->length ? a->length : b->length;

    /*
     * Two narrow strings compare as bytes: one byte per unit, in the platform's
     * order, so memory order is unit order.
     *
     * Two *wide* strings do not, and this is the trap worth naming: a u16 in
     * memory is two bytes in platform order, so memcmp on little-endian
     * compares the low byte first and would order 0x0100 below 0x0001. Wide
     * and mixed pairs therefore go through the units.
     */
    if (!cvm_string_is_wide(a) && !cvm_string_is_wide(b)) {
        i32 c = memcmp(a->data, b->data, shorter);
        if (c != 0) {
            return c < 0 ? -1 : 1;
        }
    } else {
        for (usize i = 0; i < shorter; i++) {
            u32 x = cvm_string_unit(a, i);
            u32 y = cvm_string_unit(b, i);
            if (x != y) {
                return x < y ? -1 : 1;
            }
        }
    }

    /* One is a prefix of the other, and the shorter sorts first. */
    if (a->length == b->length) {
        return 0;
    }
    return a->length < b->length ? -1 : 1;
}

/* ------------------------------------------------------------------ */
/* views                                                               */
/* ------------------------------------------------------------------ */

bool cvm_string_as_str(CvmString const* s, str* out) {
    if (cvm_string_is_wide(s)) {
        return false;
    }
    out->ptr = s->data;
    out->len = s->length;
    return true;
}

/* ------------------------------------------------------------------ */
/* conversion                                                          */
/* ------------------------------------------------------------------ */

CvmString* cvm_string_from_utf8(str text) {
    /*
     * Two passes, and the first one is not avoidable: whether the result is
     * 8-bit or 16-bit, and how many units it needs, are both decided by the
     * whole text. Building into a growable buffer instead would mean a copy of
     * the copy — the buffer is off-heap, the string is in the heap — for a scan
     * that costs a few instructions per byte.
     */
    usize i = 0;
    usize units = 0;
    bool wide = false;

    while (i < text.len) {
        rune r = 0;
        if (cvm_utf8_next_mode(text, &i, &r, CVM_UTF8_WTF8) != CVM_UTF8_RUNE) {
            return NULL;
        }
        if (r >= (rune)0x100u) {
            wide = true;
        }
        units += (r >= (rune)0x10000u) ? 2u : 1u;
    }

    CvmString* s = string_alloc(wide, units);

    i = 0;
    usize out = 0;
    while (i < text.len) {
        rune r = 0;
        (void)cvm_utf8_next_mode(text, &i, &r, CVM_UTF8_WTF8);

        if (wide) {
            u16* dst = (u16*)s->data;
            if (r >= (rune)0x10000u) {
                u32 v = (u32)r - 0x10000u;
                dst[out++] = (u16)(0xD800u + (v >> 10));
                dst[out++] = (u16)(0xDC00u + (v & 0x3FFu));
            } else {
                dst[out++] = (u16)r;
            }
        } else {
            s->data[out++] = (u8)r;
        }
    }
    return s;
}

usize cvm_string_utf8_length(CvmString const* s) {
    usize bytes = 0;
    usize i = 0;
    while (i < s->length) {
        usize next = 0;
        bytes += cvm_utf8_encode_len(cvm_string_code_point_at(s, i, &next));
        i = next;
    }
    return bytes;
}

usize cvm_string_write_utf8(CvmString const* s, u8* out, usize cap) {
    /*
     * Measured first, so that a buffer that cannot hold the result is refused
     * rather than half-filled. A caller that gets CVM_STRING_NO_ROOM has its
     * buffer as it was, which is the only behaviour that makes retrying with a
     * bigger one straightforward.
     */
    usize needed = cvm_string_utf8_length(s);
    if (cap < needed) {
        return CVM_STRING_NO_ROOM;
    }

    usize written = 0;
    usize i = 0;
    while (i < s->length) {
        usize next = 0;
        rune r = cvm_string_code_point_at(s, i, &next);
        written += cvm_utf8_encode(r, out + written);
        i = next;
    }
    return written;
}

usize cvm_string_write_utf8_nul(CvmString const* s, u8* out, usize cap) {
    usize needed = cvm_string_utf8_length(s);
    if (cap < needed + 1u) {
        return CVM_STRING_NO_ROOM;
    }
    usize written = cvm_string_write_utf8(s, out, cap);
    if (written != needed) {
        return CVM_STRING_NO_ROOM;
    }
    out[written] = 0u;
    return written;
}

usize cvm_string_utf16_length(CvmString const* s) {
    /*
     * The length in code units, in either representation: 8-bit units are all
     * below 0x100 and stay one unit, and a code point above U+FFFF is a pair in
     * the source as well as in the destination.
     */
    return s->length;
}

usize cvm_string_write_utf16(CvmString const* s, u16* out, usize cap) {
    usize needed = cvm_string_utf16_length(s);
    if (cap < needed) {
        return CVM_STRING_NO_ROOM;
    }
    for (usize i = 0; i < needed; i++) {
        out[i] = (u16)cvm_string_unit(s, i);
    }
    return needed;
}

/* ------------------------------------------------------------------ */
/* code points                                                         */
/* ------------------------------------------------------------------ */

rune cvm_string_code_point_at(CvmString const* s, usize index, usize* next) {
    cvm_assert(index < s->length);

    u32 lead = cvm_string_unit(s, index);

    /* A high surrogate followed by a low one is one code point. A high
     * surrogate followed by anything else is a code unit on its own, and so is a
     * low one with nothing in front of it — both of which a 16-bit string can
     * hold and neither of which is a character. */
    if (lead >= 0xD800u && lead <= 0xDBFFu && index + 1u < s->length) {
        u32 trail = cvm_string_unit(s, index + 1u);
        if (trail >= 0xDC00u && trail <= 0xDFFFu) {
            *next = index + 2u;
            return (rune)(0x10000u + ((lead - 0xD800u) << 10) + (trail - 0xDC00u));
        }
    }

    *next = index + 1u;
    return (rune)lead;
}

usize cvm_string_code_point_count(CvmString const* s) {
    usize count = 0;
    usize i = 0;
    while (i < s->length) {
        usize next = 0;
        (void)cvm_string_code_point_at(s, i, &next);
        i = next;
        count++;
    }
    return count;
}

/* ------------------------------------------------------------------ */
/* search                                                              */
/* ------------------------------------------------------------------ */

isize cvm_string_index_of(CvmString const* haystack, CvmString const* needle,
                          usize from) {
    usize h = haystack->length;
    usize n = needle->length;

    if (from > h) {
        from = h;
    }
    if (n == 0u) {
        /* Every position is a match for nothing, starting at `from`. */
        return (isize)from;
    }
    if (n > h) {
        return -1;
    }

    for (usize i = from; i + n <= h; i++) {
        usize k = 0;
        while (k < n &&
               cvm_string_unit(haystack, i + k) == cvm_string_unit(needle, k)) {
            k++;
        }
        if (k == n) {
            return (isize)i;
        }
    }
    return -1;
}

isize cvm_string_last_index_of(CvmString const* haystack, CvmString const* needle) {
    usize h = haystack->length;
    usize n = needle->length;

    if (n == 0u) {
        return (isize)h;
    }
    if (n > h) {
        return -1;
    }

    /* Downwards, and the loop counts down from `h - n + 1` so that the index
     * never has to go below zero in an unsigned type. */
    for (usize i = h - n + 1u; i > 0u; i--) {
        usize start = i - 1u;
        usize k = 0;
        while (k < n &&
               cvm_string_unit(haystack, start + k) == cvm_string_unit(needle, k)) {
            k++;
        }
        if (k == n) {
            return (isize)start;
        }
    }
    return -1;
}

bool cvm_string_includes(CvmString const* haystack, CvmString const* needle) {
    return cvm_string_index_of(haystack, needle, 0) >= 0;
}

/* ------------------------------------------------------------------ */
/* pieces                                                              */
/* ------------------------------------------------------------------ */

CvmString* cvm_string_substring(CvmString const* s, usize from, usize to) {
    cvm_assert(from <= to);
    cvm_assert(to <= s->length);

    usize n = to - from;
    bool wide = cvm_string_is_wide(s);

    /*
     * The result is allocated before the source's units are read, so the source
     * is held across an allocation and goes in a frame. Everything else here
     * would be fine without one — the copy is plain arithmetic — which is
     * exactly the kind of exception that is easy to miss.
     */
    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = (opaque_mut)(CvmString*)s;

    CvmString* out = string_alloc(wide, n);

    CvmString* src = (CvmString*)f_roots[0];
    for (usize i = 0; i < n; i++) {
        u32 unit = cvm_string_unit(src, from + i);
        if (wide) {
            ((u16*)out->data)[i] = (u16)unit;
        } else {
            out->data[i] = (u8)unit;
        }
    }

    cvm_frame_pop(&f_frame);
    return out;
}

CvmString* cvm_string_concat(CvmString const* a, CvmString const* b) {
    /*
     * Through the builder, which is off-heap: the scratch never enters the heap,
     * so the only allocation is the result and the only rooting needed is for the
     * two sources across it.
     */
    CVM_FRAME_DECL(f, 2);
    cvm_frame_push(&f_frame, f_roots, 2);
    f_roots[0] = (opaque_mut)(CvmString*)a;
    f_roots[1] = (opaque_mut)(CvmString*)b;

    CvmStrBuf buf;
    cvm_str_buf_init(&buf);
    bool ok = cvm_str_buf_put_string(&buf, (CvmString*)f_roots[0]);
    ok = ok && cvm_str_buf_put_string(&buf, (CvmString*)f_roots[1]);

    CvmString* out = ok ? cvm_str_buf_finish(&buf) : NULL;
    cvm_str_buf_free(&buf);

    cvm_frame_pop(&f_frame);
    return out;
}
