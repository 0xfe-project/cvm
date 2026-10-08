/*
 * vm/include/cvm/unicode.h
 *
 * Unicode: case mapping and classification, over runes and over strings.
 *
 * Ported from refs/quickjs-0.17.0/libunicode.c (Fabrice Bellard, MIT), tables
 * from libunicode-table.h, which carries the Unicode data licence.
 *
 * ---------------------------------------------------------------------
 * What is here, and what is not yet
 * ---------------------------------------------------------------------
 *
 * Here: case conversion (upper, lower, and the canonical form the regex engine
 * folds with), is_cased, is_case_ignorable, is_space, is_id_start,
 * is_id_continue, and the ASCII classification table the fast paths use.
 *
 * Not yet: normalization (NFC/NFD/NFKC/NFKD) and the character-range machinery
 * (\p{...} properties, scripts, general categories). Both need a growable
 * buffer and an allocation contract for it, and the ranges are what the regex
 * engine consumes — so they are one piece of work with the regex port rather
 * than half of this one.
 *
 * ---------------------------------------------------------------------
 * Runes, and one thing that is not one
 * ---------------------------------------------------------------------
 *
 * Everything takes and returns runes. A case mapping is the one operation that
 * is not a function of one rune to one rune: upper-casing U+00DF (ß) is "SS",
 * and lower-casing U+0130 is "i" followed by U+0307. Hence the count in the
 * return value and a caller-provided array, which is the only honest shape for
 * it — a single-rune answer would have to pick which character to lose.
 */

#ifndef CVM_UNICODE_H
#define CVM_UNICODE_H

#include "base/types.h"
#include "cvm/str.h"

/*
 * The most code points one character can map to. Latin sharp s goes to two and
 * the largest case mapping in Unicode is three.
 */
#define CVM_UNICODE_CASE_MAX 3

typedef enum CvmUnicodeCase {
    CVM_UNICODE_CASE_UPPER = 0,
    CVM_UNICODE_CASE_LOWER = 1,
} CvmUnicodeCase;

/*
 * Map one code point, writing the result into `out` and returning how many code
 * points it took (1..CVM_UNICODE_CASE_MAX).
 *
 * For the overwhelming majority of characters the answer is one code point and
 * `out` is untouched beyond it; `c` is returned unchanged when it has no case.
 */
u32 cvm_unicode_case_conv(rune out[CVM_UNICODE_CASE_MAX], rune c, CvmUnicodeCase which);

/*
 * The canonical form used for case-insensitive comparison, which is not the
 * same as lower-casing: it is the regex engine's rule, where for instance U+017F
 * (long s) folds to "s" under full folding and stays itself under simple.
 *
 * `full` is what the regex engine calls "is this a Unicode pattern": with it,
 * non-ASCII characters are folded too; without it, only ASCII is. It also picks
 * the *direction* for ASCII — 'A' folds to 'a' with `full` and 'a' to 'A'
 * without it — which looks odd and is harmless exactly as long as a comparison
 * passes the same flag on both sides, which is what the engine does. A caller
 * that folds one side and not the other gets a comparison that quietly stops
 * matching, so the flag is a property of the comparison rather than of the
 * character.
 */
rune cvm_unicode_canonicalize(rune c, bool full);

/* Whether the character is one that has a case at all. */
bool cvm_unicode_is_cased(rune c);

/* Whether the character may appear inside a case-ignorable sequence, which is
 * what lets a case-insensitive match skip the apostrophe in "L'été". */
bool cvm_unicode_is_case_ignorable(rune c);

/* Unicode White_Space, plus the characters JavaScript's grammar counts as
 * whitespace. Distinct from is_space below, which is the classification the
 * tokenizer's byte table uses. */
bool cvm_unicode_is_white_space(rune c);

bool cvm_unicode_is_id_start(rune c);
bool cvm_unicode_is_id_continue(rune c);

/* The same three questions for one byte, which is the fast path for ASCII and
 * the reason the table is exported rather than hidden in the .c. */
extern u8 const cvm_unicode_ctype_bits[256];

#define CVM_UNICODE_C_SPACE ((u8)1u << 0)
#define CVM_UNICODE_C_DIGIT ((u8)1u << 1)
#define CVM_UNICODE_C_UPPER ((u8)1u << 2)
#define CVM_UNICODE_C_LOWER ((u8)1u << 3)
#define CVM_UNICODE_C_UNDER ((u8)1u << 4)
#define CVM_UNICODE_C_DOLLAR ((u8)1u << 5)
#define CVM_UNICODE_C_XDIGIT ((u8)1u << 6)

/* The non-ASCII half of the space question, runtime-internal: anything below 256
 * is answered by the table, which is one load and is why the table is here. */
bool cvm_unicode_is_space_non_ascii(rune c);

CVM_INLINE bool cvm_unicode_is_space(rune c) {
    if (c < 256u) {
        return (cvm_unicode_ctype_bits[(usize)c] & CVM_UNICODE_C_SPACE) != 0u;
    }
    return cvm_unicode_is_space_non_ascii(c);
}

/* No byte-sized variant of the space question: cvm_unicode_is_space already
 * answers from the table for anything below 256, which is the only reason a byte
 * variant would exist, and a second entry point that nothing calls is a
 * declaration with no caller — something this build rejects on purpose. */

/*
 * Identifiers, as the language defines them rather than as Unicode does.
 *
 * ID_Start is a Unicode property and does not include `_` or `$`; a programming
 * language's identifier is that property plus the two punctuation marks its
 * grammar picked, so is_id_start('_') being false is correct and not the
 * question a tokenizer is asking. These are the questions it is asking, and they
 * are the same ones the regex engine's identifier escapes need — the ASCII half
 * is the byte table, which is one load, and the rest is the property.
 *
 * ZWNJ and ZWJ are accepted inside an identifier, which is ECMAScript's rule and
 * a Unicode recommendation for scripts that need a joiner to be spelled.
 */
CVM_INLINE bool cvm_unicode_is_ident_start(rune c) {
    if (c < 128u) {
        return (cvm_unicode_ctype_bits[(usize)c] &
                (CVM_UNICODE_C_UPPER | CVM_UNICODE_C_LOWER | CVM_UNICODE_C_UNDER |
                 CVM_UNICODE_C_DOLLAR)) != 0u;
    }
    return cvm_unicode_is_id_start(c);
}

CVM_INLINE bool cvm_unicode_is_ident_continue(rune c) {
    if (c < 128u) {
        return (cvm_unicode_ctype_bits[(usize)c] &
                (CVM_UNICODE_C_UPPER | CVM_UNICODE_C_LOWER | CVM_UNICODE_C_UNDER |
                 CVM_UNICODE_C_DOLLAR | CVM_UNICODE_C_DIGIT)) != 0u;
    }
    return cvm_unicode_is_id_continue(c) || c == 0x200Cu || c == 0x200Du;
}

/*
 * Where scratch memory comes from.
 *
 * The ported code wants to grow a buffer and hand it to the caller, which in the
 * original is a libc realloc callback. Our platform layer offers alloc and free
 * and no realloc, so the contract is those two and whatever grows keeps track of
 * its own size — see the note in vm/str/unicode.c. Both the normalizer and the
 * character ranges take one of these, which is why it is here rather than next to
 * either of them.
 */
typedef struct CvmUnicodeAlloc {
    opaque_mut ctx;
    opaque_mut (*alloc)(opaque_mut ctx, usize bytes);
    void (*free)(opaque_mut ctx, opaque_mut p);
} CvmUnicodeAlloc;

/* ------------------------------------------------------------------ */
/* character ranges                                                    */
/* ------------------------------------------------------------------ */

/*
 * A set of code points, as a sorted list of half-open intervals: `points[2i]` is
 * the first code point of interval i and `points[2i+1]` is one past its last. An
 * empty interval has the two equal, so a run is always [first, last).
 *
 * This is what `\p{...}` produces and what a matcher tests a character against,
 * and the set operations are what make it possible to answer a property with
 * something a matcher can use directly. Sorted and non-overlapping is the
 * invariant every operation here maintains.
 */
typedef struct CvmCharRange {
    i32 len; /* code points, always even */
    i32 size;
    rune* points;
    CvmUnicodeAlloc alloc;
} CvmCharRange;

typedef enum CvmCharRangeOp {
    CVM_CR_OP_UNION,
    CVM_CR_OP_INTER,
    CVM_CR_OP_XOR,
    CVM_CR_OP_SUB,
} CvmCharRangeOp;

void cvm_cr_init(CvmCharRange* cr, CvmUnicodeAlloc alloc);
void cvm_cr_free(CvmCharRange* cr);
i32 cvm_cr_realloc(CvmCharRange* cr, i32 size);
i32 cvm_cr_copy(CvmCharRange* cr, CvmCharRange const* from);

CVM_INLINE i32 cvm_cr_add_point(CvmCharRange* cr, rune v) {
    if (cr->len >= cr->size && cvm_cr_realloc(cr, cr->len + 1) < 0) {
        return -1;
    }
    cr->points[cr->len++] = v;
    return 0;
}

CVM_INLINE i32 cvm_cr_add_interval(CvmCharRange* cr, rune first, rune last) {
    if (cr->len + 2 > cr->size && cvm_cr_realloc(cr, cr->len + 2) < 0) {
        return -1;
    }
    cr->points[cr->len++] = first;
    cr->points[cr->len++] = last;
    return 0;
}

/* Combine the range with one interval, without needing a second range. */
i32 cvm_cr_op1(CvmCharRange* cr, rune const* other, i32 other_len, i32 op);

CVM_INLINE i32 cvm_cr_union_interval(CvmCharRange* cr, rune first, rune last) {
    rune other[2];
    other[0] = first;
    other[1] = last + 1u;
    return cvm_cr_op1(cr, other, 2, CVM_CR_OP_UNION);
}

/* `cr` becomes the result of `cr op (a, b)`. */
i32 cvm_cr_op(CvmCharRange* cr, rune const* a, i32 a_len, rune const* b, i32 b_len,
              i32 op);

/*
 * Complement, over the whole u32 range rather than over the code points: the
 * inversion is interval arithmetic, and the last interval ends at 0xFFFFFFFF.
 * A caller that cares about code points clamps — the ported code's convention,
 * kept rather than quietly changed.
 */
i32 cvm_cr_invert(CvmCharRange* cr);

/* ------------------------------------------------------------------ */
/* properties, by name                                                 */
/* ------------------------------------------------------------------ */

/*
 * The `\p{...}` surface: a name goes in, a character range comes out.
 *
 * Three of them, because that is what the standards have: a general category
 * (Lu, Lt, Nd, and the groups like L and C that are unions of them), a script
 * (Latin, Greek, Han), and — with the ext flag — the script extensions, which is
 * the set a character is shared between. The fourth, `\p{...}` proper with its
 * binary properties, is built on the same range machinery and is not in yet.
 *
 * `cr` must be initialized and empty. The return is 0, -1 on an allocation
 * failure, or -2 when the name is not one this knows — a distinction the caller
 * needs, because a typo in a pattern is a different problem from a full heap.
 */
i32 cvm_unicode_script(CvmCharRange* cr, u8 const* script_name, bool is_ext);
i32 cvm_unicode_general_category(CvmCharRange* cr, u8 const* gc_name);

/*
 * `\p{...}` proper: the binary properties — Alphabetic, ASCII, Math, Cased,
 * Changes_When_Lowercased, the ID_Start family, and the rest of the table. The
 * names are the standard's, and the ones that are a union of a general category
 * with something else are built with the same range operations a caller could
 * have used.
 *
 * Not here: the *properties of strings* — `\p{RGI_Emoji}` and the other names a
 * v-flag pattern can use, which describe sequences rather than characters and so
 * are not a range at all. Upstream has them behind a separate entry point, and so
 * will this, when the regex engine that needs them lands.
 */
i32 cvm_unicode_prop(CvmCharRange* cr, u8 const* prop_name);

/*
 * The *properties of strings*: `\p{RGI_Emoji}` and the other names only a
 * v-flag pattern can use. They are not a range because what they describe is a
 * list of code point *sequences* — an emoji drawn with three code points is one
 * member — so instead of filling a range they call `cb` once per sequence.
 *
 * `cr` is scratch space that the caller provides and must have initialized; it is
 * empty when the call returns. The callback's array is the implementation's and
 * is only valid for the duration of the call.
 *
 * Returns 0, or -2 when the name is not one of these.
 */
typedef void (*CvmUnicodeSequencePropCb)(opaque_mut ctx, rune const* sequence,
                                         usize length);

i32 cvm_unicode_sequence_prop(u8 const* prop_name, CvmUnicodeSequencePropCb cb,
                              opaque_mut ctx, CvmCharRange* cr);

/*
 * Case-fold a character set, for the regex engine's case-insensitive matching:
 * every character in the set gains the characters it folds to and from. `full`
 * is the same flag cvm_unicode_canonicalize takes, and has to be the same value
 * on the pattern and the subject for a match to mean anything.
 */
i32 cvm_cr_regexp_canonicalize(CvmCharRange* cr, bool full);

/* ------------------------------------------------------------------ */
/* normalization                                                       */
/* ------------------------------------------------------------------ */

/* The four forms. The enum's order is the ported code's: the compat forms are
 * the two at or above NFKC, and the two NFD forms are the ones that stop after
 * decomposing. */
typedef enum CvmUnicodeNormalization {
    CVM_UNICODE_NFC = 0,
    CVM_UNICODE_NFD = 1,
    CVM_UNICODE_NFKC = 2,
    CVM_UNICODE_NFKD = 3,
} CvmUnicodeNormalization;

/* The runtime's own: rt_mem_alloc and rt_mem_free. */
CvmUnicodeAlloc cvm_unicode_system_alloc(void);

/*
 * Normalize `src` (a run of code points) and hand back a newly allocated run in
 * `*dst`, which the caller releases with `alloc.free`. Returns how many code
 * points the result has, or -1 if the scratch could not be allocated.
 *
 * A zero-length input gives a NULL result and a length of zero, which is the one
 * case where "no buffer" is the answer rather than an error.
 */
i32 cvm_unicode_normalize(rune** dst, rune const* src, usize length,
                          CvmUnicodeNormalization kind, CvmUnicodeAlloc alloc);

/*
 * The same thing over a string: the useful form of it, since the caller has a
 * string and wants a string.
 *
 * NULL only if the scratch could not be allocated.
 */
CvmString* cvm_unicode_normalize_string(CvmString const* s,
                                        CvmUnicodeNormalization kind);

/* ------------------------------------------------------------------ */
/* over strings                                                        */
/* ------------------------------------------------------------------ */

/*
 * Case-map a whole string, through a builder, widening if a replacement needs
 * more room than the character it came from.
 *
 * NULL only if the scratch could not be allocated. The source is not modified
 * and is not kept alive by the result.
 */
CvmString* cvm_unicode_upper(CvmString const* s);
CvmString* cvm_unicode_lower(CvmString const* s);

#endif /* CVM_UNICODE_H */
