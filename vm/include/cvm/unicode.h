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
