/*
 * vm/include/cvm/builder.h
 *
 * A string being built.
 *
 * ---------------------------------------------------------------------
 * Why it is not in the heap
 * ---------------------------------------------------------------------
 *
 * The scratch buffer is off-heap — rt_mem_alloc — and the only GC allocation in
 * the whole life of a builder is the string it freezes into at the end. Three
 * things follow, and they are the reason for the choice rather than a side
 * effect of it:
 *
 *   - a builder holds no GC pointers, so it never has to be rooted. Building is
 *     not something that can be interrupted by the collector in a way the caller
 *     has to think about.
 *
 *   - building does not allocate in the heap, so it does not grow the heap, and
 *     it cannot trigger a collection. A program that formats a lot of text does
 *     not pay for it in GC work.
 *
 *   - the result is exact: the buffer is sized to the content, and the frozen
 *     string is one allocation of exactly that size, not a capacity that was
 *     chosen for a growth strategy.
 *
 * A caller that has a GC string to append does not have to root it either:
 * nothing here can collect.
 *
 * ---------------------------------------------------------------------
 * What it is not
 * ---------------------------------------------------------------------
 *
 * Not a formatting language. There is no format string and no varargs, and that
 * is deliberate: a format string is parsed at run time, and the type of each
 * argument has to be recovered from somewhere, which in C means either tags in
 * the string or a macro that dispatches on the argument's type. The frontend
 * knows every type at the point it emits code, so the useful form here is a call
 * per value — put_i64, put_str, put_f64 — which is also what makes it work with
 * no libc for the collector to trip over.
 *
 * Nor is it the bounded formatter in base/format.h. That one writes into a
 * caller's buffer and cannot allocate, because the collector's own logging has
 * to work in the middle of a collection. This one grows and allocates, and is
 * for everything else.
 */

#ifndef CVM_BUILDER_H
#define CVM_BUILDER_H

#include "base/types.h"
#include "cvm/str.h"

/*
 * Field order is not an ABI — nothing but this header and its own .c touch a
 * builder — but `data` is a u8* to the start of the units whatever `wide` says,
 * and the units are u8 or u16 accordingly.
 */
typedef struct CvmStrBuf {
    u8* data;
    usize len; /* code units used */
    usize cap; /* code units allocated */
    bool wide; /* units are 16-bit */
    /*
     * Sticky: set by the first operation that could not be carried out, and
     * cleared only by init. A caller building a long chain checks once at the
     * end, and one that checks every step is not doing anything wrong.
     */
    bool failed;
} CvmStrBuf;

/* An empty builder. Allocates nothing until something is appended. */
void cvm_str_buf_init(CvmStrBuf* b);

/* Release the scratch. The builder is unusable until init again. */
void cvm_str_buf_free(CvmStrBuf* b);

CVM_INLINE usize cvm_str_buf_length(CvmStrBuf const* b) {
    return b->len;
}

CVM_INLINE bool cvm_str_buf_is_wide(CvmStrBuf const* b) {
    return b->wide;
}

/* False once any operation has failed. */
CVM_INLINE bool cvm_str_buf_ok(CvmStrBuf const* b) {
    return !b->failed;
}

/*
 * Make room for `units` more. Not usually needed: every append reserves what it
 * needs. It exists for a caller that knows the size it is heading for and would
 * rather allocate once than four times.
 */
bool cvm_str_buf_reserve(CvmStrBuf* b, usize units);

/*
 * Move the builder to 16-bit units. Contents are preserved; a builder that is
 * already wide does nothing. Called for you by anything that needs it.
 */
bool cvm_str_buf_widen(CvmStrBuf* b);

/*
 * Append one code *unit*, without interpreting it: 0..0xFF in a narrow builder,
 * 0..0xFFFF in a wide one, and a unit above 0xFF widens.
 *
 * This is the operation for text that is already in the string's own terms —
 * including a lone surrogate, which is a unit and not a code point and is
 * exactly what the wide form is for.
 */
bool cvm_str_buf_put_unit(CvmStrBuf* b, u32 unit);

/*
 * Append one code *point*, which is the other half of the distinction: anything
 * above U+FFFF becomes a surrogate pair and forces the wide form.
 *
 * Values above U+10FFFF are a failure, and surrogates are not code points — use
 * put_unit for a lone one.
 */
bool cvm_str_buf_put_rune(CvmStrBuf* b, rune c);

/*
 * Append a byte string, each byte as a code unit. Right for ASCII and for text
 * that is already known to be below U+0100; wrong for anything else, in the
 * specific sense that its UTF-8 bytes would become characters one at a time.
 */
bool cvm_str_buf_put_bytes(CvmStrBuf* b, str text);

/*
 * Append UTF-8, decoded. Malformed input fails — it does not append a
 * replacement character and carry on, because a caller that meant to validate
 * should hear about it, and a caller that did not should not silently get text
 * it did not write.
 */
bool cvm_str_buf_put_utf8(CvmStrBuf* b, str text);

/* Append another string's units, widening this builder if that one is wide. */
bool cvm_str_buf_put_string(CvmStrBuf* b, CvmString const* s);

/* Append `count` copies of a unit. */
bool cvm_str_buf_fill(CvmStrBuf* b, u32 unit, usize count);

/* Numbers, through the conversions in str/dtoa.h: shortest exact form for f64,
 * decimal for the integers, no NUL, no locale. */
bool cvm_str_buf_put_u64(CvmStrBuf* b, u64 value);
bool cvm_str_buf_put_i64(CvmStrBuf* b, i64 value);
bool cvm_str_buf_put_f64(CvmStrBuf* b, f64 value);

/*
 * Freeze: allocate the string, copy the content into it, and release the
 * scratch. The builder is left empty and reusable.
 *
 * Returns NULL when the builder failed at any point. The one allocation in the
 * whole life of a builder is here, and it is exact.
 */
CvmString* cvm_str_buf_finish(CvmStrBuf* b);

#endif /* CVM_BUILDER_H */
