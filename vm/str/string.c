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
