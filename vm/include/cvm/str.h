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

/*
 * The flags word: two bits of state and two for what the string is made of.
 */
#define CVM_STRF_WIDE ((u16)1u << 0)
#define CVM_STRF_HASHED ((u16)1u << 1)

/* What the string is made of: flat (units in data[]), rope (data[] is two
 * pointers), or slice (data[] is a parent pointer and an offset). */
#define CVM_STRF_KIND_MASK ((u16)0x000Cu)
#define CVM_STRF_FLAT ((u16)0u)
#define CVM_STRF_ROPE ((u16)1u << 2)
#define CVM_STRF_SLICE ((u16)1u << 3)

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

CVM_INLINE u16 cvm_string_kind(CvmString const* s) {
    return (u16)(s->flags & CVM_STRF_KIND_MASK);
}

/* The shape-holding half of cvm_string_unit. Not for calling directly. */
u32 cvm_string_unit_slow(CvmString const* s, usize i);

/* The same string as a flat one: s itself when flat, a flat copy otherwise. */
CvmString* cvm_string_flatten(CvmString* s);

/*
 * The code unit at `i`, whichever shape the string is.
 *
 * The flat path is inline; a rope or slice goes through cvm_string_unit_slow,
 * which walks to the leaf. The depth is bounded (see cvm_string_concat), so an
 * index cannot become a scan.
 */
CVM_INLINE u32 cvm_string_unit(CvmString const* s, usize i) {
    cvm_assert(i < s->length);
    if (CVM_LIKELY(cvm_string_kind(s) == CVM_STRF_FLAT)) {
        return cvm_string_is_wide(s) ? (u32)((u16 const*)s->data)[i] : (u32)s->data[i];
    }
    return cvm_string_unit_slow(s, i);
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

/* ------------------------------------------------------------------ */
/* views                                                               */
/* ------------------------------------------------------------------ */

/*
 * A borrowed view of a string's units: no copy, nothing owned, and nothing to
 * outlive the string it came from.
 *
 * This is a flat string's units, which is every string today. When the rope and
 * slice forms land, this is where their cost shows up: a rope has no contiguous
 * units to hand out, so its view has to be built and the caller has to decide
 * whether to pay for it, while a slice's view is its parent's units at an
 * offset. The declaration will not change; a note about when it can allocate
 * will be added when there is something for it to describe.
 */
typedef struct CvmStringView {
    u8 const* units;
    usize length; /* code units */
    bool wide;    /* units are 16-bit */
} CvmStringView;

CVM_INLINE CvmStringView cvm_string_view(CvmString const* s) {
    cvm_assert(cvm_string_kind(s) == CVM_STRF_FLAT &&
               "view requires a flat string; flatten first");
    CvmStringView v = {
        .units = s->data, .length = s->length, .wide = cvm_string_is_wide(s)};
    return v;
}

/*
 * The string as a `str`, when its units are bytes.
 *
 * False for a 16-bit string, and that is the honest answer rather than a
 * conversion done on the caller's behalf: a wide string's units are not bytes,
 * and the two things a caller wants from bytes — raw storage, or UTF-8 — are
 * answered by different calls below. Turning a wide string into bytes quietly
 * would make every later operation on the view a lie about its width.
 */
bool cvm_string_as_str(CvmString const* s, str* out);

/* ------------------------------------------------------------------ */
/* conversion                                                          */
/* ------------------------------------------------------------------ */

/*
 * From UTF-8, choosing the representation: 8-bit units when every code point
 * fits in one, 16-bit otherwise.
 *
 * NULL when the text is not well-formed. A lone surrogate is accepted — it is a
 * code unit, it survives the round trip, and refusing it would mean a string
 * could not hold what an escape sequence asked for. An overlong form, a
 * truncated sequence and a value past U+10FFFF are refused.
 */
CvmString* cvm_string_from_utf8(str text);

/* Bytes that cvm_string_write_utf8 needs. */
usize cvm_string_utf8_length(CvmString const* s);

/* Returned when the buffer is too small. Not zero: an empty string writes zero
 * bytes, and that is a success. */
#define CVM_STRING_NO_ROOM ((usize)~0u)

/*
 * Write the string as UTF-8 and return the bytes written, or CVM_STRING_NO_ROOM
 * if `cap` cannot hold them.
 *
 * A code unit that is a lone surrogate comes out as WTF-8, which is what
 * cvm_utf8_encode does with it: three bytes that decode back to the same unit,
 * so the conversion loses nothing in either direction.
 */
usize cvm_string_write_utf8(CvmString const* s, u8* out, usize cap);

/*
 * The same, with a NUL after it, for the libc calls that want one. The return is
 * the bytes written *not* counting the NUL, so the buffer needs
 * cvm_string_utf8_length(s) + 1.
 */
usize cvm_string_write_utf8_nul(CvmString const* s, u8* out, usize cap);

/*
 * Code units in a UTF-16 rendering: the string's own length in either
 * representation, since 8-bit units are all below 0x100 and a code point above
 * U+FFFF is two units either way.
 */
usize cvm_string_utf16_length(CvmString const* s);

/* Write UTF-16 code units. Returns the units written, or CVM_STRING_NO_ROOM. */
usize cvm_string_write_utf16(CvmString const* s, u16* out, usize cap);

/* ------------------------------------------------------------------ */
/* code points, as opposed to units                                    */
/* ------------------------------------------------------------------ */

/*
 * The code point beginning at code unit `index`, with `*next` set to where the
 * following one begins.
 *
 * A surrogate pair decodes into the one code point it stands for. A *lone*
 * surrogate is returned as its own value and costs one unit: it is a code unit
 * and not a code point, and doing anything else with it — dropping it, or
 * substituting a replacement character — would lose text the string holds.
 */
rune cvm_string_code_point_at(CvmString const* s, usize index, usize* next);

/* How many code points, which is not the length: a surrogate pair is two units
 * and one code point. */
usize cvm_string_code_point_count(CvmString const* s);

/* ------------------------------------------------------------------ */
/* search                                                              */
/* ------------------------------------------------------------------ */

/*
 * The first occurrence of `needle` at or after code unit `from`, or -1.
 *
 * An empty needle is found at `from` (or at the length, if `from` is past it),
 * which is what every string search does and what makes `includes` true for it.
 *
 * Naive search, deliberately: the strings a runtime searches are short, the step
 * is a unit comparison, and a two-way or Boyer-Moore implementation earns its
 * proof once a profile says so.
 */
isize cvm_string_index_of(CvmString const* haystack, CvmString const* needle,
                          usize from);

/* The last occurrence, or -1. */
isize cvm_string_last_index_of(CvmString const* haystack, CvmString const* needle);

bool cvm_string_includes(CvmString const* haystack, CvmString const* needle);

/* ------------------------------------------------------------------ */
/* pieces                                                              */
/* ------------------------------------------------------------------ */

/*
 * The code units in [from, to), as a string of its own.
 *
 * A slice: the parent's units at an offset, not a copy. A slice of a slice is
 * adjusted to point at the grandparent, so a chain of substring calls does
 * not stack up indirections.
 *
 * Units rather than text: a substring of a wide string stays wide, and splitting
 * a surrogate pair is the caller's business, as it is in every other
 * code-unit-indexed operation here.
 */
CvmString* cvm_string_substring(CvmString const* s, usize from, usize to);

/* The two strings' units, one after the other. A rope: a node holding both,
 * not a copy. Past CVM_ROPE_MAX_DEPTH the concatenation flattens rather than
 * build a tree that would make an index a scan. */
CvmString* cvm_string_concat(CvmString const* a, CvmString const* b);

#define CVM_ROPE_MAX_DEPTH ((u32)32)

#endif /* CVM_STR_H */
