/*
 * vm/include/cvm/str.h
 *
 * The runtime's string: immutable, in the GC heap, eight or sixteen bits per
 * code unit.
 *
 * ---------------------------------------------------------------------
 * Two representations, and what they are not
 * ---------------------------------------------------------------------
 *
 * A string holds either 8-bit or 16-bit code units, and the choice is made
 * once, when it is built:
 *
 *     8-bit units    one byte per code unit, values 0..0xFF
 *     16-bit units   one u16 per code unit, UTF-16, so a code point above
 *                    U+FFFF is a surrogate pair
 *
 * Neither is UTF-8. This is deliberate and it is what makes indexing O(1):
 * with UTF-8 bytes, `s[i]` is a scan, and every string operation that walks a
 * string by position becomes quadratic in the length of the characters it
 * walks over. UTF-8 is a *transport* encoding here — it is what comes in from
 * a file and what goes out to one, converted at the boundary by the functions
 * in base/utf8.h — not an in-memory representation.
 *
 * The 8-bit form is not a character set choice either: it holds code units
 * below 0x100, so text that is all ASCII or all Latin-1 costs half what the
 * 16-bit form would. A code point at or above 0x100 forces 16-bit units, and
 * cvm_string_widen is what makes the move.
 *
 * ---------------------------------------------------------------------
 * Immutability, and what rests on it
 * ---------------------------------------------------------------------
 *
 * Nothing here mutates a built string. That is load-bearing rather than a
 * style preference: a substring can share its parent's bytes, a concatenation
 * can be a tree of pieces, and an interned string can be compared by address,
 * all of which stop being true the moment a byte can change under a reader.
 * Anything that needs to be built up is built in a builder and frozen into a
 * string once.
 *
 * ---------------------------------------------------------------------
 * In the heap
 * ---------------------------------------------------------------------
 *
 * A string is an ordinary GC object with a length-driven tail, described by
 * CVM_TYPE_SCALAR_ARRAY: the collector reads its length to find where it ends
 * and never looks inside, because a string contains no pointers. Its hash is
 * computed on demand and cached in the object.
 */

#ifndef CVM_STR_H
#define CVM_STR_H

#include "base/types.h"
#include "cvm/gc.h"

/* Why a string's flags word exists at all: two bits of state that would
 * otherwise cost a wider field or a separate lookup. */
#define CVM_STRF_WIDE ((u16)1u << 0)   /* 16-bit units rather than 8-bit */
#define CVM_STRF_HASHED ((u16)1u << 1) /* `hash` holds the computed value */

/*
 * Field order is the ABI: the collector reads `length` at
 * array_length_offset, and `data` must be aligned for the widest unit.
 */
typedef struct CvmString {
    /* Number of code units, not bytes and not code points. */
    usize length;
    /* Content hash, meaningful only when CVM_STRF_HASHED is set. */
    u32 hash;
    u16 flags;
    u16 reserved;
    /* The code units: u8 or u16 depending on CVM_STRF_WIDE. */
    u8 data[];
} CvmString;

_Static_assert(offsetof(CvmString, data) == 16, "string data must stay 8-byte aligned");

/* ------------------------------------------------------------------ */
/* construction                                                        */
/* ------------------------------------------------------------------ */

/*
 * From 8-bit code units: the cheap path, and the one to use for text that is
 * already known to be ASCII or Latin-1.
 *
 * The units are copied, so the caller's buffer is not retained.
 */
CvmString* cvm_string_new8(u8 const* units, usize length);

/*
 * From a UTF-8-free ASCII slice. An assertion in debug builds, and the
 * caller's problem in release: the check is a scan of the text, which is
 * exactly the cost the 8-bit path exists to avoid paying twice.
 */
CvmString* cvm_string_new_ascii(str text);

/*
 * From UTF-16 code units, which is the form to use when the text is known to
 * need them. A caller that does not know should build the 8-bit form and widen
 * if it turns out not to be enough.
 */
CvmString* cvm_string_new16(u16 const* units, usize length);

/*
 * The 16-bit form of `s`, or `s` itself when it is already 16-bit.
 *
 * "Or `s` itself" is why the parameter is mutable and the result has to be
 * taken: widening may allocate, so a caller must keep the result rather than
 * assume its own pointer is still the answer. Nothing is mutated either way;
 * the type system has no way to write "one of these two pointers".
 */
CvmString* cvm_string_widen(CvmString* s);

/* ------------------------------------------------------------------ */
/* reading                                                             */
/* ------------------------------------------------------------------ */

CVM_INLINE usize cvm_string_length(CvmString const* s) {
    return s->length;
}

CVM_INLINE bool cvm_string_is_wide(CvmString const* s) {
    return (s->flags & CVM_STRF_WIDE) != 0u;
}

/*
 * The code unit at `i`, widened to u32.
 *
 * A code *unit*, not a code point: on a 16-bit string this can be half of a
 * surrogate pair. Code point access and iteration live above this, where the
 * pairing rules are known.
 *
 * No bounds check: the caller has the length, and the cost of the check is
 * the cost of the operation.
 */
CVM_INLINE u32 cvm_string_unit(CvmString const* s, usize i) {
    cvm_assert(i < s->length);
    return cvm_string_is_wide(s) ? (u32)((u16 const*)s->data)[i] : (u32)s->data[i];
}

/*
 * The content hash, computed on first use and cached in the object.
 *
 * Takes a mutable pointer because the first call fills that cache — nothing
 * about the string's value changes, but the write is real, and a header that
 * claimed otherwise would be lying to a reader about what is safe to share.
 * Two threads hashing one string at once is therefore a benign race and still
 * a race: a caller that shares strings across threads unfenced has to settle
 * that, which is why this is called out rather than hidden.
 *
 * Stability matters: equal strings hash equal, across representations, and the
 * value does not change for the life of the process. Nothing depends on the
 * particular function, so it is an implementation detail rather than something
 * a caller may persist.
 */
u32 cvm_string_hash(CvmString* s);

/* Same code units. */
bool cvm_string_equal(CvmString const* a, CvmString const* b);

/*
 * Content order, as a lexicographic comparison of code units: the difference
 * at the first position where they differ, and shorter first when one is a
 * prefix of the other. A mixed pair compares the units by value, so 8-bit and
 * 16-bit strings of the same text are equal and sort together.
 */
i32 cvm_string_compare(CvmString const* a, CvmString const* b);

#endif /* CVM_STR_H */
