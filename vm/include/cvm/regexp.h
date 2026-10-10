/*
 * vm/include/cvm/regexp.h
 *
 * Regular expressions: the bytecode compiler and the matcher, ported from
 * refs/quickjs-0.17.0/libregexp.c (Fabrice Bellard, MIT) — see vm/str/regexp.c.
 *
 * The engine is self-contained: it takes a context, a pattern and a subject, and
 * it allocates nothing else. The context carries the caller's allocator and the
 * two budgets the parser and the matcher are not allowed to exceed — how much C
 * stack they may use, and when they must give up. Both are the caller's policy,
 * which is why neither is a constant here.
 */

#ifndef CVM_REGEXP_H
#define CVM_REGEXP_H

#include "base/alloc.h"
#include "base/types.h"
#include "cvm/unicode.h"

/* Compile flags, as recorded in the bytecode and reported by cvm_lre_get_flags. */
#define CVM_RE_FLAG_GLOBAL (1 << 0)
#define CVM_RE_FLAG_IGNORECASE (1 << 1)
#define CVM_RE_FLAG_MULTILINE (1 << 2)
#define CVM_RE_FLAG_DOTALL (1 << 3)
#define CVM_RE_FLAG_UNICODE (1 << 4)
#define CVM_RE_FLAG_STICKY (1 << 5)
#define CVM_RE_FLAG_INDICES (1 << 6)      /* unused by the engine, just recorded */
#define CVM_RE_FLAG_NAMED_GROUPS (1 << 7) /* the pattern has named groups */
#define CVM_RE_FLAG_UNICODE_SETS (1 << 8)

#define CVM_RE_RET_MEMORY_ERROR (-1)
#define CVM_RE_RET_TIMEOUT (-2)
#define CVM_RE_RET_BYTECODE_ERROR (-3)

/* Length of the trailer after a group name, the terminating zero included. */
#define CVM_RE_GROUP_NAME_TRAILER_LEN 2

/* The two subject widths the matcher understands. */
#define CVM_RE_CBUF_8 0
#define CVM_RE_CBUF_16 1

/*
 * What the engine needs from its caller.
 *
 * `now` may be null, in which case the deadline is not checked; when it is given
 * it must return a monotonic count in whatever unit `deadline` is in, and the
 * matcher gives up once it reaches it. A freestanding runtime has no clock, so the
 * clock is the caller's too.
 */
typedef struct CvmRegexCtx {
    CvmAlloc alloc;
    u8 const* stack_base; /* the mark the C stack stood at when this began */
    usize stack_budget;   /* bytes of C stack the engine may use from there */
    u64 (*now)(opaque clock_ctx);
    opaque clock_ctx;
    u64 deadline; /* 0 = no deadline */
} CvmRegexCtx;

/*
 * Compile. Returns the bytecode, or null with `error_msg` filled in. The bytecode
 * is allocated with the context's allocator and belongs to the caller, who frees
 * it the same way: a zero-size reallocation through the same context.
 */
u8* cvm_lre_compile(i32* plen, u8* error_msg, i32 error_msg_size, u8 const* buf,
                    usize buf_len, i32 re_flags, opaque_mut ctx);

i32 cvm_lre_get_capture_count(u8 const* bc_buf);

/*
 * How large a capture buffer the executor needs: twice the group count plus its
 * temporary registers, which it keeps in the tail of the same buffer. Sizing the
 * buffer by the group count alone overflows on any pattern that uses a register.
 */
i32 cvm_lre_get_alloc_count(u8 const* bc_buf);
i32 cvm_lre_get_flags(u8 const* bc_buf);
u8 const* cvm_lre_get_groupnames(u8 const* bc_buf);

/*
 * Match `cbuf` from `cindex`, `clen` code units long (`cbuf_type` picks the width).
 * `capture` holds 2 * capture_count indices and is filled in on a match; a capture
 * that did not participate comes back as -1. Returns 1 for a match, 0 for none, or
 * one of the CVM_RE_RET_* codes.
 */
i32 cvm_lre_exec(u8** capture, u8 const* bc_buf, u8 const* cbuf, i32 cindex, i32 clen,
                 i32 cbuf_type, opaque_mut ctx);

i32 cvm_lre_parse_escape(u8 const** pp, i32 allow_utf16);

/* ASCII identifier tables, and the two tests built on them. */
extern u32 const cvm_lre_id_start_table_ascii[4];
extern u32 const cvm_lre_id_continue_table_ascii[4];

CVM_INLINE bool cvm_lre_js_is_ident_first(i32 c) {
    if ((u32)c < 128u) {
        return ((cvm_lre_id_start_table_ascii[(u32)c >> 5] >> ((u32)c & 31u)) & 1u) !=
               0u;
    }
    return cvm_unicode_is_id_start((rune)c);
}

CVM_INLINE bool cvm_lre_js_is_ident_next(i32 c) {
    if ((u32)c < 128u) {
        return ((cvm_lre_id_continue_table_ascii[(u32)c >> 5] >> ((u32)c & 31u)) &
                1u) != 0u;
    }
    /* ZWNJ and ZWJ are accepted in identifiers. */
    return cvm_unicode_is_id_continue((rune)c) || (u32)c == 0x200Cu ||
           (u32)c == 0x200Du;
}

/* ------------------------------------------------------------------ */
/* a compiled pattern, as a runtime value                              */
/* ------------------------------------------------------------------ */

/*
 * The engine above is self-contained and knows nothing about the collector; this
 * is the value a program holds. A compiled pattern is one GC object: the bytecode
 * is its tail, so there is nothing off the heap to finalize and nothing to free
 * by hand.
 *
 * Compiling needs scratch memory, and that comes from the platform layer, which is
 * the one place allowed to ask the operating system. It is released before the
 * call returns.
 */

typedef struct CvmRegExp CvmRegExp;

/*
 * How much C stack a compile or a match may use, counted from where it was
 * entered. The matcher recurses, so a pattern of nested groups can run a program
 * out of stack; this is the bound that turns that into an error instead. A caller
 * that knows its own stack can be lower.
 */
#define CVM_REGEXP_STACK_BUDGET ((usize)256u * 1024u)

/* Compile `pattern`. NULL when it does not compile, or when there is no room. */
CvmRegExp* cvm_regexp_compile(CvmString const* pattern, i32 flags);

i32 cvm_regexp_capture_count(CvmRegExp const* re);
i32 cvm_regexp_flags(CvmRegExp const* re);

/*
 * Match `subject` from code unit `start`.
 *
 * `captures` holds 2 * capture_count entries and is filled with code unit offsets,
 * or -1 for a group that did not take part. (The buffer the executor itself needs
 * is larger — cvm_lre_get_alloc_count says how large — and this file keeps that
 * one to itself.) `deadline_ms` is an rt_time_millis() reading to stop at, or 0
 * for no deadline: a pattern can backtrack for longer than a program is willing to
 * wait, and this is where it says so.
 *
 * Returns 1 for a match, 0 for none, or a CVM_RE_RET_* code.
 */
i32 cvm_regexp_exec(CvmRegExp const* re, CvmString const* subject, usize start,
                    i32* captures, u64 deadline_ms);

/* The capture index a group name stands for, or -1. */
i32 cvm_regexp_group_index(CvmRegExp const* re, CvmString const* name);

#endif /* CVM_REGEXP_H */
