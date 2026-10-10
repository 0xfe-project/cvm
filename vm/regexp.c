/*
 * vm/regexp.c
 *
 * A compiled pattern, as a value the runtime can hold: the layer between
 * vm/str/regexp.c (the engine, which knows nothing about the collector) and a
 * program that wants to match text.
 *
 * The engine is built to be embedded, so the three things it needs from its host
 * are arguments: an allocator for its scratch memory, a stack budget, and a
 * deadline. They are filled in here from the platform layer and from the caller.
 *
 * A subject is matched in UTF-16, always. A wide string's units are already
 * UTF-16, so they are matched where they lie; a narrow string's units are code
 * points below 0x100 held one to a byte, so widening them is value-preserving and
 * gives back the same indices. That is why the offsets this file returns are code
 * units in the subject, whichever representation it happens to have.
 */

#include "cvm/regexp.h"

#include "base/assert.h"
#include "cvm/buf.h"
#include "base/types.h"
#include "cvm/gc.h"
#include "cvm/str.h"
#include "mem.h"
#include "rt.h"

typedef struct CvmRegExp {
    i32 flags;
    i32 capture_count;
    /* The executor's capture buffer size: twice the groups plus its registers. */
    i32 alloc_count;
    /*
     * The length of the bytecode tail. A `usize`, because that is the width the
     * collector reads an array's length at: a narrower field would leave the bytes
     * beside it holding half of a length.
     */
    usize bytecode_len;
    /* The engine's bytecode, trailer and all: a scalar array of bytes. */
    u8 bytecode[];
} CvmRegExp;

static CvmTypeDescriptor const cvm_type_byte = {
    .size = 1,
    .kind = CVM_TYPE_SCALAR,
};

static CvmTypeDescriptor const cvm_type_regexp = {
    .size = offsetof(CvmRegExp, bytecode),
    .kind = CVM_TYPE_SCALAR_ARRAY,
    .array_length_offset = offsetof(CvmRegExp, bytecode_len),
    .array_data_offset = offsetof(CvmRegExp, bytecode),
    .array_elem_type = &cvm_type_byte,
};

/* The deadline the engine reads, in the unit the caller gave it. */
static u64 regexp_now(opaque clock_ctx) {
    CVM_UNUSED(clock_ctx);
    return rt_time_millis();
}

/*
 * The context a compile or a match runs under. `mark` is the caller's own stack
 * mark: the engine measures from it, so it has to be a local of the frame that
 * calls in.
 */
static CvmRegexCtx regexp_ctx(u8 const* mark, u64 deadline_ms) {
    CvmRegexCtx ctx;
    ctx.alloc = cvm_buf_system_alloc();
    ctx.stack_base = mark;
    ctx.stack_budget = CVM_REGEXP_STACK_BUDGET;
    ctx.now = regexp_now;
    ctx.clock_ctx = NULL;
    ctx.deadline = deadline_ms;
    return ctx;
}

/* ------------------------------------------------------------------ */
/* compiling                                                           */
/* ------------------------------------------------------------------ */

CvmRegExp* cvm_regexp_compile(CvmString const* pattern, i32 flags) {
    /* The stack mark is this frame's own address; its value is never read. */
    u8 mark = 0;
    CvmRegexCtx ctx = regexp_ctx(&mark, 0u);

    /* The engine reads UTF-8, and a narrow string's units are not UTF-8. */
    usize text_len = cvm_string_utf8_length(pattern);
    CvmBuf text;
    cvm_buf_init(&text, ctx.alloc);
    if (!cvm_buf_reserve(&text, text_len + 1u)) {
        cvm_buf_free(&text);
        return NULL;
    }
    usize written = cvm_string_write_utf8(pattern, text.data, text_len + 1u);
    if (written == CVM_STRING_NO_ROOM) {
        cvm_buf_free(&text);
        return NULL;
    }
    text.size = written;
    if (!cvm_buf_putc(&text, 0)) { /* the engine wants a terminator */
        cvm_buf_free(&text);
        return NULL;
    }

    i32 len = 0;
    u8 error_msg[64];
    error_msg[0] = 0;
    u8* bytecode = cvm_lre_compile(&len, error_msg, (i32)sizeof(error_msg), text.data,
                                   text.size - 1u, flags, &ctx);
    cvm_buf_free(&text);
    if (bytecode == NULL || len <= 0) {
        return NULL;
    }

    /*
     * The bytecode is scratch until here, and it is what the value is: copy it
     * into the object and hand the scratch back. Nothing allocates between the
     * copy and the return, so the new object needs no root of its own.
     */
    CvmRegExp* re = cvm_alloc_array(&cvm_type_regexp, (usize)len, 1u);
    memcpy(re->bytecode, bytecode, (usize)len);
    re->bytecode_len = len;
    re->flags = cvm_lre_get_flags(bytecode);
    re->capture_count = cvm_lre_get_capture_count(bytecode);
    re->alloc_count = cvm_lre_get_alloc_count(bytecode);
    ctx.alloc.realloc(ctx.alloc.ctx, bytecode, 0);
    return re;
}

i32 cvm_regexp_capture_count(CvmRegExp const* re) {
    return re->capture_count;
}

i32 cvm_regexp_flags(CvmRegExp const* re) {
    return re->flags;
}

/* ------------------------------------------------------------------ */
/* matching                                                            */
/* ------------------------------------------------------------------ */

/*
 * The subject as UTF-16 code units, and the number of them.
 *
 * A wide string is its own buffer. A narrow one is widened into `scratch`, which
 * the caller provides and which is only allocated when it is needed.
 */
static u16 const* subject_units(CvmString const* subject, CvmBuf* scratch,
                                usize* count) {
    *count = subject->length;
    if (cvm_string_is_wide(subject)) {
        return (u16 const*)subject->data;
    }
    if (!cvm_buf_reserve(scratch, subject->length * 2u)) {
        return NULL;
    }
    u16* out = (u16*)scratch->data;
    for (usize i = 0; i < subject->length; i++) {
        out[i] = (u16)subject->data[i];
    }
    scratch->size = subject->length * 2u;
    return out;
}

i32 cvm_regexp_exec(CvmRegExp const* re, CvmString const* subject, usize start,
                    i32* captures, u64 deadline_ms) {
    cvm_assert(re != NULL);
    cvm_assert(subject != NULL);
    if (start > subject->length) {
        return CVM_RE_RET_BYTECODE_ERROR;
    }

    u8 mark = 0;
    CvmRegexCtx ctx = regexp_ctx(&mark, deadline_ms);

    usize count = 0;
    CvmBuf scratch;
    cvm_buf_init(&scratch, ctx.alloc);
    u16 const* units = subject_units(subject, &scratch, &count);
    if (units == NULL) {
        cvm_buf_free(&scratch);
        return CVM_RE_RET_MEMORY_ERROR;
    }

    /*
     * The engine hands back pointers into the subject, one pair per group; the
     * caller asked for offsets, and a group that did not take part is a null. The
     * array is the engine's size, not the group count: its registers live in the
     * tail of it.
     */
    i32 groups = re->capture_count;
    u8** where =
        (u8**)ctx.alloc.alloc(ctx.alloc.ctx, (usize)re->alloc_count * sizeof(u8*));
    if (where == NULL) {
        cvm_buf_free(&scratch);
        return CVM_RE_RET_MEMORY_ERROR;
    }
    memset(where, 0, (usize)re->alloc_count * sizeof(u8*));

    i32 ret = cvm_lre_exec(where, re->bytecode, (u8 const*)units, (i32)start,
                           (i32)count, CVM_RE_CBUF_16, &ctx);
    if (ret == 1) {
        u8 const* base = (u8 const*)units;
        for (i32 i = 0; i < groups * 2; i++) {
            captures[i] =
                where[i] == NULL ? -1 : (i32)((where[i] - base) / (i32)sizeof(u16));
        }
    }
    ctx.alloc.free(ctx.alloc.ctx, where);
    cvm_buf_free(&scratch);
    return ret;
}

/* ------------------------------------------------------------------ */
/* named groups                                                        */
/* ------------------------------------------------------------------ */

/*
 * The trailer after the bytecode holds one entry per capture group: the name as
 * UTF-8 with a terminator, then one byte of scope. An unnamed group is two zero
 * bytes, which is why a zero-length name is skipped rather than compared. The
 * capture index is the entry's position, counting from one.
 */
i32 cvm_regexp_group_index(CvmRegExp const* re, CvmString const* name) {
    u8 const* p = cvm_lre_get_groupnames(re->bytecode);
    if (p == NULL) {
        return -1;
    }
    u8 const* end = re->bytecode + re->bytecode_len;

    usize name_len = cvm_string_utf8_length(name);
    CvmBuf text;
    cvm_buf_init(&text, cvm_buf_system_alloc());
    if (!cvm_buf_reserve(&text, name_len + 1u)) {
        cvm_buf_free(&text);
        return -1;
    }
    if (cvm_string_write_utf8(name, text.data, name_len + 1u) == CVM_STRING_NO_ROOM) {
        cvm_buf_free(&text);
        return -1;
    }
    text.size = name_len;
    text.data[name_len] = 0;

    i32 index = 1;
    while (p + CVM_RE_GROUP_NAME_TRAILER_LEN <= end) {
        usize len = 0;
        while (p[len] != 0) {
            len++;
        }
        if (len == name_len && len != 0u && memcmp(p, text.data, len) == 0) {
            cvm_buf_free(&text);
            return index;
        }
        p += len + CVM_RE_GROUP_NAME_TRAILER_LEN;
        index++;
    }
    cvm_buf_free(&text);
    return -1;
}
