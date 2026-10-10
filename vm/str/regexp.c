/*
 * Regular Expression Engine
 *
 * Copyright (c) 2017-2018 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/*
 * vm/str/regexp.c
 *
 * Regular expression compiler and matcher. Ported from
 * refs/quickjs-0.17.0/libregexp.c (Fabrice Bellard, MIT).
 *
 * The port keeps the original structure and names so it can be diffed against
 * upstream. What changed is what it is built on: the project's types, the caller's
 * allocator instead of the C library's, CvmBuf instead of DynBuf, CvmCharRange
 * instead of CharRange, and the ported Unicode tables. libc is gone, which is what
 * lets this live in vm/.
 *
 * The three things upstream expects its embedder to supply -- realloc, a stack
 * check and a timeout -- are the helpers below, and they read the context the
 * caller passed in.
 */

#include "base/assert.h"
#include "base/utf8.h"
#include "base/sort.h"
#include "base/types.h"
#include "cvm/buf.h"
#include "cvm/regexp.h"
#include "cvm/unicode.h"
#include "mem.h"

/* ------------------------------------------------------------------ */
/* what upstream expects the embedder to provide                       */
/* ------------------------------------------------------------------ */

/*
 * Allocation. The context carries the allocator, and a zero size is a free --
 * the convention the engine uses for releasing what it built.
 */
static opaque_mut re_alloc(opaque ctx, opaque_mut p, usize bytes) {
    CvmRegexCtx* c = (CvmRegexCtx*)ctx;
    return c->alloc.realloc(c->alloc.ctx, p, bytes);
}

/*
 * How much C stack is left. There is no portable way to ask, so this is the
 * distance from the mark the caller recorded, and the engine calls it wherever the
 * parser is about to recurse -- which is what makes that distance a bound on the
 * recursion. `alloca_size` is what the caller is about to put on the stack; the
 * parser passes zero.
 */
static bool re_check_stack_overflow(opaque ctx, usize alloca_size) {
    CvmRegexCtx* c = (CvmRegexCtx*)ctx;
    u8 here;
    usize used = (c->stack_base > &here) ? (usize)(c->stack_base - &here)
                                         : (usize)(&here - c->stack_base);
    return used + alloca_size >= c->stack_budget;
}

/* The deadline, if the caller gave one. No clock, no check. */
static i32 re_check_timeout(opaque ctx) {
    CvmRegexCtx* c = (CvmRegexCtx*)ctx;
    if (c->now == NULL || c->deadline == 0u) {
        return 0;
    }
    return c->now(c->clock_ctx) >= c->deadline;
}

/* ------------------------------------------------------------------ */
/* the buffer the bytecode is written into                             */
/* ------------------------------------------------------------------ */

/*
 * Upstream's buffers report failure as a non-zero return; ours report success as
 * true. These keep upstream's sense, so that a call site which checks the result
 * means what it says — the two readings are opposites, and a port that mixes them
 * turns a successful append into an error.
 */
static i32 re_buf_reserve(CvmBuf* b, usize bytes) {
    return cvm_buf_reserve(b, bytes) ? 0 : -1;
}

static i32 re_putc(CvmBuf* b, u8 c) {
    return cvm_buf_putc(b, c) ? 0 : -1;
}

static i32 re_put(CvmBuf* b, opaque data, usize bytes) {
    return cvm_buf_put(b, data, bytes) ? 0 : -1;
}

/* Little-endian appends, as upstream writes its bytecode. */
static i32 re_buf_put_u16(CvmBuf* b, u16 val) {
    u8 tmp[2];
    tmp[0] = (u8)val;
    tmp[1] = (u8)(val >> 8);
    return re_put(b, tmp, 2u);
}

static i32 re_buf_put_u32(CvmBuf* b, u32 val) {
    u8 tmp[4];
    tmp[0] = (u8)val;
    tmp[1] = (u8)(val >> 8);
    tmp[2] = (u8)(val >> 16);
    tmp[3] = (u8)(val >> 24);
    return re_put(b, tmp, 4u);
}

/* The integer helpers upstream takes from cutils.h. */
static i32 re_max_int(i32 a, i32 b) {
    return a > b ? a : b;
}

/* The golden ratio's negative, which spreads the bits: upstream's hash32. */
static u32 re_hash32(u32 a) {
    return a * 0x61c88647u;
}

/* A high and a low surrogate, as one code point. Upstream's from_surrogate. */
static u32 re_from_surrogate(u32 hi, u32 lo) {
    return 0x10000u + ((hi - 0xD800u) << 10) + (lo - 0xDC00u);
}

#define re_countof(a) (sizeof(a) / sizeof((a)[0]))

/* A bounded string copy: at most buf_size - 1 bytes and a terminator. */
static void re_pstrcpy(u8* buf, i32 buf_size, u8 const* str) {
    i32 i = 0;
    while (i < buf_size - 1 && str[i] != 0) {
        buf[i] = str[i];
        i++;
    }
    buf[i] = 0;
}

/* One code point as UTF-8; the number of bytes written. */
static i32 re_utf8_encode(u8* buf, rune c) {
    if (c < 0x80u) {
        buf[0] = (u8)c;
        return 1;
    }
    if (c < 0x800u) {
        buf[0] = (u8)(0xC0u | (c >> 6));
        buf[1] = (u8)(0x80u | (c & 0x3Fu));
        return 2;
    }
    if (c < 0x10000u) {
        buf[0] = (u8)(0xE0u | (c >> 12));
        buf[1] = (u8)(0x80u | ((c >> 6) & 0x3Fu));
        buf[2] = (u8)(0x80u | (c & 0x3Fu));
        return 3;
    }
    buf[0] = (u8)(0xF0u | (c >> 18));
    buf[1] = (u8)(0x80u | ((c >> 12) & 0x3Fu));
    buf[2] = (u8)(0x80u | ((c >> 6) & 0x3Fu));
    buf[3] = (u8)(0x80u | (c & 0x3Fu));
    return 4;
}

/* A word character, for the word-boundary assertion. */
static bool re_is_word_byte(u8 c) {
    return (cvm_unicode_ctype_bits[c] & (CVM_UNICODE_C_UPPER | CVM_UNICODE_C_LOWER |
                                         CVM_UNICODE_C_UNDER | CVM_UNICODE_C_DIGIT)) !=
           0u;
}

/* Upstream's utf8_decode_len: one code point, and how many bytes it took. */
static rune re_utf8_decode_len(u8 const* p, usize max_len, u8 const** pp) {
    rune c = p[0];
    if (c < 0x80u) {
        *pp = p + 1;
        return c;
    }
    u32 n = cvm_utf8_seq_len((u8)c);
    if (n == 0u || (usize)n > max_len) {
        *pp = p + 1;
        return CVM_RUNE_REPLACEMENT;
    }
    rune out = 0;
    usize offset = 0;
    str s = {.ptr = p, .len = max_len};
    if (cvm_utf8_next_mode(s, &offset, &out, CVM_UTF8_STRICT) != CVM_UTF8_RUNE) {
        *pp = p + 1;
        return CVM_RUNE_REPLACEMENT;
    }
    *pp = p + offset;
    return out;
}

static void re_put_u32(u8* tab, u32 val) {
    memcpy(tab, &val, 4u);
}

static void re_put_u16(u8* tab, u16 val) {
    memcpy(tab, &val, 2u);
}

static u16 re_get_u16(u8 const* tab) {
    u16 v;
    memcpy(&v, tab, 2u);
    return v;
}

static u32 re_get_u32(u8 const* tab) {
    u32 v;
    memcpy(&v, tab, 4u);
    return v;
}

/* Byte-wise comparison, for the group-name table. */
static i32 re_strcmp(u8 const* a, u8 const* b) {
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return (i32)*a - (i32)*b;
}

/* The value of a hex digit, or -1. Upstream's from_hex. */
static i32 re_from_hex(i32 c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool re_is_hi_surrogate(u32 c) {
    return c >= 0xD800u && c <= 0xDBFFu;
}

static bool re_is_lo_surrogate(u32 c) {
    return c >= 0xDC00u && c <= 0xDFFFu;
}

static usize re_strlen(u8 const* s) {
    usize n = 0;
    while (s[n] != 0u) {
        n++;
    }
    return n;
}

/* A code point is a space: the ASCII table first, the Unicode one after. */
static bool re_is_space(rune c) {
    if (c < 128u) {
        return (cvm_unicode_ctype_bits[c] & CVM_UNICODE_C_SPACE) != 0u;
    }
    return cvm_unicode_is_space_non_ascii(c);
}

/* ASCII identifier tables, used by lre_js_is_ident_first/next in libregexp.h
   and by quickjs.c. */
u32 const cvm_lre_id_start_table_ascii[4] = {/* $ A-Z _ a-z */
                                             0x00000000, 0x00000010, 0x87FFFFFE,
                                             0x07FFFFFE};

u32 const cvm_lre_id_continue_table_ascii[4] = {/* $ 0-9 A-Z _ a-z */
                                                0x00000000, 0x03FF0010, 0x87FFFFFE,
                                                0x07FFFFFE};

/*
  TODO:
  - remove REOP_char_i and REOP_range_i by precomputing the case folding.
  - add specific opcodes for simple unicode property tests so that the
    generated bytecode is smaller.
  - Add a lock step execution mode (=linear time execution guaranteed)
    when the regular expression is "simple" i.e. no backreference nor
    complicated lookahead. The opcodes are designed for this execution
    model.
*/

//#define DUMP_REOP
//#define DUMP_EXEC

typedef enum {
#define DEF(id, size) REOP_##id,
#include "regexp_opcode.h"
#undef DEF
    REOP_COUNT,
} REOPCodeEnum;

#define CAPTURE_COUNT_MAX 255
#define REGISTER_COUNT_MAX 255
#define GROUP_NAME_SCOPE_MAX 255
/* must be large enough to have a negligible runtime cost and small
   enough to call the interrupt callback often. */
#define INTERRUPT_COUNTER_INIT 10000

/* unicode code points */
#define CP_LS 0x2028
#define CP_PS 0x2029

#define TMP_BUF_SIZE 128

typedef struct {
    CvmBuf byte_code;
    const u8* buf_ptr;
    const u8* buf_end;
    const u8* buf_start;
    i32 re_flags;
    bool is_unicode;
    bool unicode_sets; /* if set, is_unicode is also set */
    bool ignore_case;
    bool multi_line;
    bool dotall;
    u8 group_name_scope;
    i32 capture_count;
    i32 total_capture_count; /* -1 = not computed yet */
    i32 has_named_captures;  /* -1 = don't know, 0 = no, 1 = yes */
    void* opaque;
    CvmBuf group_names;
    union {
        u8 error_msg[TMP_BUF_SIZE];
        u8 tmp_buf[TMP_BUF_SIZE];
    } u;
} REParseState;

typedef struct {
#ifdef DUMP_REOP
    u8 const* name;
#endif
    u8 size;
} REOpCode;

static const REOpCode reopcode_info[REOP_COUNT] = {
#ifdef DUMP_REOP
#define DEF(id, size) {#id, size},
#else
#define DEF(id, size) {size},
#endif
#include "regexp_opcode.h"
#undef DEF
};

#define RE_HEADER_FLAGS 0
#define RE_HEADER_CAPTURE_COUNT 2
#define RE_HEADER_REGISTER_COUNT 3
#define RE_HEADER_BYTECODE_LEN 4

#define RE_HEADER_LEN 8

static inline i32 lre_is_digit(i32 c) {
    return c >= '0' && c <= '9';
}

/* insert 'len' bytes at position 'pos'. Return < 0 if error. */
/*
 * Insert `len` bytes of room at `pos`, which is how the bytecode makes space for a
 * back-patched offset. Upstream's CvmBuf names its bytes `buf`; ours says `data`.
 */
static i32 re_buf_insert(CvmBuf* s, i32 pos, i32 len) {
    if (re_buf_reserve(s, (usize)len) != 0) {
        return -1;
    }
    memmove(s->data + pos + len, s->data + pos, s->size - (usize)pos);
    s->size += (usize)len;
    return 0;
}

typedef struct REString {
    struct REString* next;
    u32 hash;
    u32 len;
    u32 buf[];
} REString;

typedef struct {
    /* the string list is the union of 'char_range' and of the strings
       in hash_table[]. The strings in hash_table[] have a length !=
       1. */
    CvmCharRange cr;
    u32 n_strings;
    u32 hash_size;
    i32 hash_bits;
    REString** hash_table;
} REStringList;

static u32 re_string_hash(i32 len, const u32* buf) {
    i32 i;
    u32 h;
    h = 1;
    for (i = 0; i < len; i++)
        h = h * 263 + buf[i];
    return re_hash32(h);
}

static void re_string_list_init(REParseState* s1, REStringList* s) {
    cvm_cr_init(&s->cr, ((CvmRegexCtx*)(s1->opaque))->alloc);
    s->n_strings = 0;
    s->hash_size = 0;
    s->hash_bits = 0;
    s->hash_table = NULL;
}

static void re_string_list_free(REStringList* s) {
    REString *p, *p_next;
    i32 i;
    for (i = 0; i < (i32)s->hash_size; i++) {
        for (p = s->hash_table[i]; p != NULL; p = p_next) {
            p_next = p->next;
            s->cr.alloc.realloc(s->cr.alloc.ctx, p, 0);
        }
    }
    s->cr.alloc.realloc(s->cr.alloc.ctx, s->hash_table, 0);

    cvm_cr_free(&s->cr);
}

#ifdef DUMP_REOP
static void lre_print_char(i32 c, bool is_range) {
    if (c == '\'' || c == '\\' || (is_range && (c == '-' || c == ']'))) {
        printf((u8 const*)"\\%c", c);
    } else if (c >= ' ' && c <= 126) {
        printf((u8 const*)"%c", c);
    } else {
        printf((u8 const*)"\\u{%04x}", c);
    }
}

static __maybe_unused void re_string_list_dump(u8 const* str, const REStringList* s) {
    REString* p;
    const CvmCharRange* cr;
    i32 i, j, k;

    printf((u8 const*)"%s:\n", str);
    printf((u8 const*)"  ranges: [");
    cr = &s->cr;
    for (i = 0; i < cr->len; i += 2) {
        lre_print_char(cr->points[i], true);
        if (cr->points[i] != cr->points[i + 1] - 1) {
            printf((u8 const*)"-");
            lre_print_char(cr->points[i + 1] - 1, true);
        }
    }
    printf((u8 const*)"]\n");

    j = 0;
    for (i = 0; i < (i32)s->hash_size; i++) {
        for (p = s->hash_table[i]; p != NULL; p = p->next) {
            printf((u8 const*)"  %d/%d: '", j, s->n_strings);
            for (k = 0; k < p->len; k++) {
                lre_print_char(p->buf[k], false);
            }
            printf((u8 const*)"'\n");
            j++;
        }
    }
}
#endif /* DUMP_REOP */

/* 'buf' is NULL if 'len' is zero: the empty string is a valid member of a
   string set, e.g. /[\q{}]/v */
static i32 re_string_find2(REStringList* s, i32 len, const u32* buf, u32 h0,
                           bool add_flag) {
    u32 h = 0; /* avoid warning */
    REString* p;
    if (s->n_strings != 0) {
        h = h0 >> (32 - s->hash_bits);
        for (p = s->hash_table[h]; p != NULL; p = p->next) {
            if (p->hash == h0 && (i32)p->len == len &&
                (len == 0 || !memcmp(p->buf, buf, len * sizeof(buf[0])))) {
                return 1;
            }
        }
    }
    /* not found */
    if (!add_flag)
        return 0;
    /* increase the size of the hash table if needed */
    if (CVM_UNLIKELY((s->n_strings + 1) > s->hash_size)) {
        REString **new_hash_table, *p_next;
        i32 new_hash_bits, i;
        u32 new_hash_size;
        new_hash_bits = re_max_int(s->hash_bits + 1, 4);
        new_hash_size = 1 << new_hash_bits;
        new_hash_table = s->cr.alloc.realloc(s->cr.alloc.ctx, NULL,
                                             sizeof(new_hash_table[0]) * new_hash_size);
        if (!new_hash_table)
            return -1;
        memset(new_hash_table, 0, sizeof(new_hash_table[0]) * new_hash_size);
        for (i = 0; i < (i32)s->hash_size; i++) {
            for (p = s->hash_table[i]; p != NULL; p = p_next) {
                p_next = p->next;
                h = p->hash >> (32 - new_hash_bits);
                p->next = new_hash_table[h];
                new_hash_table[h] = p;
            }
        }
        s->cr.alloc.realloc(s->cr.alloc.ctx, s->hash_table, 0);
        s->hash_bits = new_hash_bits;
        s->hash_size = new_hash_size;
        s->hash_table = new_hash_table;
        h = h0 >> (32 - s->hash_bits);
    }

    p = s->cr.alloc.realloc(s->cr.alloc.ctx, NULL,
                            sizeof(REString) + len * sizeof(buf[0]));
    if (!p)
        return -1;
    p->next = s->hash_table[h];
    s->hash_table[h] = p;
    s->n_strings++;
    p->hash = h0;
    p->len = len;
    if (len != 0)
        memcpy(p->buf, buf, sizeof(buf[0]) * len);
    return 1;
}

static i32 re_string_find(REStringList* s, i32 len, const u32* buf, bool add_flag) {
    u32 h0;
    h0 = re_string_hash(len, buf);
    return re_string_find2(s, len, buf, h0, add_flag);
}

/* return -1 if memory error, 0 if OK */
static i32 re_string_add(REStringList* s, i32 len, const u32* buf) {
    if (len == 1) {
        return cvm_cr_union_interval(&s->cr, buf[0], buf[0]);
    }
    if (re_string_find(s, len, buf, true) < 0)
        return -1;
    return 0;
}

/* a = a op b */
static i32 re_string_list_op(REStringList* a, REStringList* b, i32 op) {
    i32 i, ret;
    REString *p, **pp;

    if (cvm_cr_op1(&a->cr, b->cr.points, b->cr.len, op))
        return -1;

    switch (op) {
    case CVM_CR_OP_UNION:
        if (b->n_strings != 0) {
            for (i = 0; i < (i32)b->hash_size; i++) {
                for (p = b->hash_table[i]; p != NULL; p = p->next) {
                    if (re_string_find2(a, p->len, p->buf, p->hash, true) < 0)
                        return -1;
                }
            }
        }
        break;
    case CVM_CR_OP_INTER:
    case CVM_CR_OP_SUB:
        for (i = 0; i < (i32)a->hash_size; i++) {
            pp = &a->hash_table[i];
            for (;;) {
                p = *pp;
                if (p == NULL)
                    break;
                ret = re_string_find2(b, p->len, p->buf, p->hash, false);
                if (op == CVM_CR_OP_SUB)
                    ret = !ret;
                if (!ret) {
                    /* remove it */
                    *pp = p->next;
                    a->n_strings--;
                    a->cr.alloc.realloc(a->cr.alloc.ctx, p, 0);
                } else {
                    /* keep it */
                    pp = &p->next;
                }
            }
        }
        break;
    default:
        cvm_assert(0);
    }
    return 0;
}

static i32 re_string_list_canonicalize(REParseState* s1, REStringList* s,
                                       bool is_unicode) {
    if (cvm_cr_regexp_canonicalize(&s->cr, is_unicode))
        return -1;
    if (s->n_strings != 0) {
        REStringList a_s, *a = &a_s;
        i32 i, j;
        REString* p;

        /* XXX: simplify */
        re_string_list_init(s1, a);

        a->n_strings = s->n_strings;
        a->hash_size = s->hash_size;
        a->hash_bits = s->hash_bits;
        a->hash_table = s->hash_table;

        s->n_strings = 0;
        s->hash_size = 0;
        s->hash_bits = 0;
        s->hash_table = NULL;

        for (i = 0; i < (i32)a->hash_size; i++) {
            for (p = a->hash_table[i]; p != NULL; p = p->next) {
                for (j = 0; j < (i32)p->len; j++) {
                    p->buf[j] = cvm_unicode_canonicalize(p->buf[j], is_unicode);
                }
                if (re_string_add(s, p->len, p->buf)) {
                    re_string_list_free(a);
                    return -1;
                }
            }
        }
        re_string_list_free(a);
    }
    return 0;
}

static const u16 char_range_d[] = {
    1,
    0x0030,
    0x0039 + 1,
};

/* code point ranges for Zs,Zl or Zp property */
static const u16 char_range_s[] = {
    10,
    0x0009,
    0x000D + 1,
    0x0020,
    0x0020 + 1,
    0x00A0,
    0x00A0 + 1,
    0x1680,
    0x1680 + 1,
    0x2000,
    0x200A + 1,
    /* 2028;LINE SEPARATOR;Zl;0;WS;;;;;N;;;;; */
    /* 2029;PARAGRAPH SEPARATOR;Zp;0;B;;;;;N;;;;; */
    0x2028,
    0x2029 + 1,
    0x202F,
    0x202F + 1,
    0x205F,
    0x205F + 1,
    0x3000,
    0x3000 + 1,
    /* FEFF;ZERO WIDTH NO-BREAK SPACE;Cf;0;BN;;;;;N;BYTE ORDER MARK;;;; */
    0xFEFF,
    0xFEFF + 1,
};

static const u16 char_range_w[] = {
    4, 0x0030, 0x0039 + 1, 0x0041, 0x005A + 1, 0x005F, 0x005F + 1, 0x0061, 0x007A + 1,
};

#define CLASS_RANGE_BASE 0x40000000

typedef enum {
    CHAR_RANGE_d,
    CHAR_RANGE_D,
    CHAR_RANGE_s,
    CHAR_RANGE_S,
    CHAR_RANGE_w,
    CHAR_RANGE_W,
} CharRangeEnum;

static const u16* const char_range_table[] = {
    char_range_d,
    char_range_s,
    char_range_w,
};

static i32 cr_init_char_range(REParseState* s, REStringList* cr, u32 c) {
    bool invert;
    const u16* c_pt;
    i32 len, i;

    invert = c & 1;
    c_pt = char_range_table[c >> 1];
    len = *c_pt++;
    re_string_list_init(s, cr);
    for (i = 0; i < len * 2; i++) {
        if (cvm_cr_add_point(&cr->cr, c_pt[i]))
            goto fail;
    }
    if (invert) {
        if (cvm_cr_invert(&cr->cr))
            goto fail;
    }
    return 0;
fail:
    re_string_list_free(cr);
    return -1;
}

#ifdef DUMP_REOP
static __maybe_unused void lre_dump_bytecode(const u8* buf, i32 buf_len) {
    i32 pos, len, opcode, bc_len, re_flags, i;
    u32 val, val2;

    cvm_assert(buf_len >= RE_HEADER_LEN);

    re_flags = cvm_lre_get_flags(buf);
    bc_len = re_get_u32(buf + RE_HEADER_BYTECODE_LEN);
    cvm_assert(bc_len + RE_HEADER_LEN <= buf_len);
    printf((u8 const*)"flags: 0x%x capture_count=%d reg_count=%d\n", re_flags,
           buf[RE_HEADER_CAPTURE_COUNT], buf[RE_HEADER_REGISTER_COUNT]);
    if (re_flags & CVM_RE_FLAG_NAMED_GROUPS) {
        u8 const* p;
        p = (u8*)buf + RE_HEADER_LEN + bc_len;
        printf((u8 const*)"named groups: ");
        for (i = 1; i < buf[RE_HEADER_CAPTURE_COUNT]; i++) {
            if (i != 1)
                printf((u8 const*)",(u8 const*)");
            printf((u8 const*)"<%s>", p);
            p += re_strlen(p) + CVM_RE_GROUP_NAME_TRAILER_LEN;
        }
        printf((u8 const*)"\n");
        cvm_assert(p == (u8*)(buf + buf_len));
    }
    printf((u8 const*)"bytecode_len=%d\n", bc_len);

    buf += RE_HEADER_LEN;
    pos = 0;
    while (pos < bc_len) {
        printf((u8 const*)"%5u: ", pos);
        opcode = buf[pos];
        len = reopcode_info[opcode].size;
        if (opcode >= REOP_COUNT) {
            printf((u8 const*)" invalid opcode=0x%02x\n", opcode);
            break;
        }
        if ((pos + len) > bc_len) {
            printf((u8 const*)" buffer overflow (opcode=0x%02x)\n", opcode);
            break;
        }
        printf((u8 const*)"%s", reopcode_info[opcode].name);
        switch (opcode) {
        case REOP_char:
        case REOP_char_i:
            val = re_get_u16(buf + pos + 1);
            if (val >= ' ' && val <= 126)
                printf((u8 const*)" '%c'", val);
            else
                printf((u8 const*)" 0x%04x", val);
            break;
        case REOP_char32:
        case REOP_char32_i:
            val = re_get_u32(buf + pos + 1);
            if (val >= ' ' && val <= 126)
                printf((u8 const*)" '%c'", val);
            else
                printf((u8 const*)" 0x%08x", val);
            break;
        case REOP_goto:
        case REOP_split_goto_first:
        case REOP_split_next_first:
        case REOP_lookahead:
        case REOP_negative_lookahead:
            val = re_get_u32(buf + pos + 1);
            val += (pos + 5);
            printf((u8 const*)" %u", val);
            break;
        case REOP_loop:
            val2 = buf[pos + 1];
            val = re_get_u32(buf + pos + 2);
            val += (pos + 6);
            printf((u8 const*)" r%u, %u", val2, val);
            break;
        case REOP_loop_split_goto_first:
        case REOP_loop_split_next_first:
        case REOP_loop_check_adv_split_goto_first:
        case REOP_loop_check_adv_split_next_first: {
            u32 limit;
            val2 = buf[pos + 1];
            limit = re_get_u32(buf + pos + 2);
            val = re_get_u32(buf + pos + 6);
            val += (pos + 10);
            printf((u8 const*)" r%u, %u, %u", val2, limit, val);
        } break;
        case REOP_save_start:
        case REOP_save_end:
            printf((u8 const*)" %u", buf[pos + 1]);
            break;
        case REOP_back_reference:
        case REOP_back_reference_i:
        case REOP_backward_back_reference:
        case REOP_backward_back_reference_i: {
            i32 n, i;
            n = buf[pos + 1];
            len += n;
            for (i = 0; i < n; i++) {
                if (i != 0)
                    printf((u8 const*)",(u8 const*)");
                printf((u8 const*)" %u", buf[pos + 2 + i]);
            }
        } break;
        case REOP_save_reset:
            printf((u8 const*)" %u %u", buf[pos + 1], buf[pos + 2]);
            break;
        case REOP_set_i32:
            val = buf[pos + 1];
            val2 = re_get_u32(buf + pos + 2);
            printf((u8 const*)" r%u, %d", val, val2);
            break;
        case REOP_set_char_pos:
        case REOP_check_advance:
            val = buf[pos + 1];
            printf((u8 const*)" r%u", val);
            break;
        case REOP_range:
        case REOP_range_i: {
            i32 n, i;
            n = re_get_u16(buf + pos + 1);
            len += n * 4;
            for (i = 0; i < n * 2; i++) {
                val = re_get_u16(buf + pos + 3 + i * 2);
                printf((u8 const*)" 0x%04x", val);
            }
        } break;
        case REOP_range32:
        case REOP_range32_i: {
            i32 n, i;
            n = re_get_u16(buf + pos + 1);
            len += n * 8;
            for (i = 0; i < n * 2; i++) {
                val = re_get_u32(buf + pos + 3 + i * 4);
                printf((u8 const*)" 0x%08x", val);
            }
        } break;
        default:
            break;
        }
        printf((u8 const*)"\n");
        pos += len;
    }
}
#endif

static void re_emit_op(REParseState* s, i32 op) {
    re_putc(&s->byte_code, op);
}

/* return the offset of the u32 value */
static i32 re_emit_op_u32(REParseState* s, i32 op, u32 val) {
    i32 pos;
    re_putc(&s->byte_code, op);
    pos = s->byte_code.size;
    re_buf_put_u32(&s->byte_code, val);
    return pos;
}

static i32 re_emit_goto(REParseState* s, i32 op, u32 val) {
    i32 pos;
    re_putc(&s->byte_code, op);
    pos = s->byte_code.size;
    re_buf_put_u32(&s->byte_code, val - (pos + 4));
    return pos;
}

static i32 re_emit_goto_u8(REParseState* s, i32 op, u32 arg, u32 val) {
    i32 pos;
    re_putc(&s->byte_code, op);
    re_putc(&s->byte_code, arg);
    pos = s->byte_code.size;
    re_buf_put_u32(&s->byte_code, val - (pos + 4));
    return pos;
}

static i32 re_emit_goto_u8_u32(REParseState* s, i32 op, u32 arg0, u32 arg1, u32 val) {
    i32 pos;
    re_putc(&s->byte_code, op);
    re_putc(&s->byte_code, arg0);
    re_buf_put_u32(&s->byte_code, arg1);
    pos = s->byte_code.size;
    re_buf_put_u32(&s->byte_code, val - (pos + 4));
    return pos;
}

static void re_emit_op_u8(REParseState* s, i32 op, u32 val) {
    re_putc(&s->byte_code, op);
    re_putc(&s->byte_code, val);
}

static void re_emit_op_u16(REParseState* s, i32 op, u32 val) {
    re_putc(&s->byte_code, op);
    re_buf_put_u16(&s->byte_code, val);
}

/*
 * The parser's errors, without a printf.
 *
 * Upstream formats them with the C library's vsnprintf. Every message here is
 * literal text except two that substitute a character, and carrying a format
 * parser for those would be the wrong trade — so the message is copied and the two
 * substitutions are made where they happen.
 */
static i32 re_parse_error(REParseState* s, u8 const* msg) {
    usize n = 0;
    while (msg[n] != 0 && n + 1u < sizeof(s->u.error_msg)) {
        s->u.error_msg[n] = msg[n];
        n++;
    }
    s->u.error_msg[n] = 0;
    return -1;
}

/* `prefix` + the character + `suffix`, for the two messages that name one. */
static i32 re_parse_error_char(REParseState* s, u8 const* prefix, i32 c,
                               u8 const* suffix) {
    u8 buf[64];
    usize n = 0;
    for (usize i = 0; prefix[i] != 0 && n + 1u < sizeof(buf); i++) {
        buf[n++] = prefix[i];
    }
    if (n + 1u < sizeof(buf)) {
        buf[n++] = (u8)c;
    }
    for (usize i = 0; suffix[i] != 0 && n + 1u < sizeof(buf); i++) {
        buf[n++] = suffix[i];
    }
    buf[n] = 0;
    return re_parse_error(s, buf);
}

static i32 re_parse_out_of_memory(REParseState* s) {
    return re_parse_error(s, (u8 const*)"out of memory");
}

/* If allow_overflow is false, return -1 in case of
   overflow. Otherwise return INT32_MAX. */
static i32 parse_digits(const u8** pp, bool allow_overflow) {
    const u8* p;
    u64 v;
    i32 c;

    p = *pp;
    v = 0;
    for (;;) {
        c = *p;
        if (c < '0' || c > '9')
            break;
        v = v * 10 + c - '0';
        if (v >= INT32_MAX) {
            if (allow_overflow)
                v = INT32_MAX;
            else
                return -1;
        }
        p++;
    }
    *pp = p;
    return v;
}

static i32 re_parse_expect(REParseState* s, const u8** pp, i32 c) {
    const u8* p;
    p = *pp;
    if (*p != c)
        return re_parse_error_char(s, (u8 const*)"expecting '", c, (u8 const*)"'");
    p++;
    *pp = p;
    return 0;
}

/* Parse an escape sequence, *pp points after the '\':
   allow_utf16 value:
   0 : no UTF-16 escapes allowed
   1 : UTF-16 escapes allowed
   2 : UTF-16 escapes allowed and escapes of surrogate pairs are
   converted to a unicode character (unicode regexp case).

   Return the unicode char and update *pp if recognized,
   return -1 if malformed escape,
   return -2 otherwise. */
i32 cvm_lre_parse_escape(const u8** pp, i32 allow_utf16) {
    const u8* p;
    u32 c;

    p = *pp;
    c = *p++;
    switch (c) {
    case 'b':
        c = '\b';
        break;
    case 'f':
        c = '\f';
        break;
    case 'n':
        c = '\n';
        break;
    case 'r':
        c = '\r';
        break;
    case 't':
        c = '\t';
        break;
    case 'v':
        c = '\v';
        break;
    case 'x': {
        i32 h0, h1;

        h0 = re_from_hex(*p++);
        if (h0 < 0)
            return -1;
        h1 = re_from_hex(*p++);
        if (h1 < 0)
            return -1;
        c = (h0 << 4) | h1;
    } break;
    case 'u': {
        i32 h, i;
        u32 c1;

        if (*p == '{' && allow_utf16) {
            p++;
            c = 0;
            for (;;) {
                h = re_from_hex(*p++);
                if (h < 0)
                    return -1;
                c = (c << 4) | h;
                if (c > 0x10FFFF)
                    return -1;
                if (*p == '}')
                    break;
            }
            p++;
        } else {
            c = 0;
            for (i = 0; i < 4; i++) {
                h = re_from_hex(*p++);
                if (h < 0) {
                    return -1;
                }
                c = (c << 4) | h;
            }
            if (re_is_hi_surrogate(c) && allow_utf16 == 2 && p[0] == '\\' &&
                p[1] == 'u') {
                /* convert an escaped surrogate pair into a
                       unicode u8*/
                c1 = 0;
                for (i = 0; i < 4; i++) {
                    h = re_from_hex(p[2 + i]);
                    if (h < 0)
                        break;
                    c1 = (c1 << 4) | h;
                }
                if (i == 4 && re_is_lo_surrogate(c1)) {
                    p += 6;
                    c = re_from_surrogate(c, c1);
                }
            }
        }
    } break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
        c -= '0';
        if (allow_utf16 == 2) {
            /* only accept \0 not followed by digit */
            if (c != 0 || lre_is_digit(*p))
                return -1;
        } else {
            /* parse a legacy octal sequence */
            u32 v;
            v = *p - '0';
            if (v > 7)
                break;
            c = (c << 3) | v;
            p++;
            if (c >= 32)
                break;
            v = *p - '0';
            if (v > 7)
                break;
            c = (c << 3) | v;
            p++;
        }
        break;
    default:
        return -2;
    }
    *pp = p;
    return c;
}

/* XXX: we use the same chars for name and value */
static bool is_unicode_char(i32 c) {
    return ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') || (c == '_'));
}

/* XXX: memory error test */
static void seq_prop_cb(void* opaque, const u32* seq, usize seq_len) {
    REStringList* sl = opaque;
    re_string_add(sl, seq_len, seq);
}

static i32 parse_unicode_property(REParseState* s, REStringList* cr, const u8** pp,
                                  bool is_inv, bool allow_sequence_prop) {
    const u8* p;
    u8 name[64], value[64];
    u8* q;
    bool script_ext;
    i32 ret;

    p = *pp;
    if (*p != '{')
        return re_parse_error(s, (u8 const*)"expecting '{' after \\p");
    p++;
    q = name;
    while (is_unicode_char(*p)) {
        if ((usize)(q - name) >= sizeof(name) - 1u)
            goto unknown_property_name;
        *q++ = *p++;
    }
    *q = '\0';
    q = value;
    if (*p == '=') {
        p++;
        while (is_unicode_char(*p)) {
            if ((usize)(q - value) >= sizeof(value) - 1u)
                return re_parse_error(s, (u8 const*)"unknown unicode property value");
            *q++ = *p++;
        }
    }
    *q = '\0';
    if (*p != '}')
        return re_parse_error(s, (u8 const*)"expecting '}'");
    p++;
    //    printf((u8 const*)"name=%s value=%s\n", name, value);

    if (!re_strcmp(name, (u8 const*)"Script") || !re_strcmp(name, (u8 const*)"sc")) {
        script_ext = false;
        goto do_script;
    } else if (!re_strcmp(name, (u8 const*)"Script_Extensions") ||
               !re_strcmp(name, (u8 const*)"scx")) {
        script_ext = true;
do_script:
        re_string_list_init(s, cr);
        ret = cvm_unicode_script(&cr->cr, value, script_ext);
        if (ret) {
            re_string_list_free(cr);
            if (ret == -2)
                return re_parse_error(s, (u8 const*)"unknown unicode script");
            else
                goto out_of_memory;
        }
    } else if (!re_strcmp(name, (u8 const*)"General_Category") ||
               !re_strcmp(name, (u8 const*)"gc")) {
        re_string_list_init(s, cr);
        ret = cvm_unicode_general_category(&cr->cr, value);
        if (ret) {
            re_string_list_free(cr);
            if (ret == -2)
                return re_parse_error(s, (u8 const*)"unknown unicode general category");
            else
                goto out_of_memory;
        }
    } else if (value[0] == '\0') {
        re_string_list_init(s, cr);
        ret = cvm_unicode_general_category(&cr->cr, name);
        if (ret == -1) {
            re_string_list_free(cr);
            goto out_of_memory;
        }
        if (ret < 0) {
            ret = cvm_unicode_prop(&cr->cr, name);
            if (ret == -1) {
                re_string_list_free(cr);
                goto out_of_memory;
            }
        }
        if (ret < 0 && !is_inv && allow_sequence_prop) {
            CvmCharRange cr_tmp;
            cvm_cr_init(&cr_tmp, ((CvmRegexCtx*)(s->opaque))->alloc);
            ret = cvm_unicode_sequence_prop(name, seq_prop_cb, cr, &cr_tmp);
            cvm_cr_free(&cr_tmp);
            if (ret == -1) {
                re_string_list_free(cr);
                goto out_of_memory;
            }
        }
        if (ret < 0)
            goto unknown_property_name;
    } else {
unknown_property_name:
        return re_parse_error(s, (u8 const*)"unknown unicode property name");
    }

    /* the ordering of case folding and inversion  differs with
       unicode_sets. 'unicode_sets' ordering is more consistent */
    /* XXX: the spec seems incorrect, we do it as the other engines
       seem to do it. */
    if (s->ignore_case && s->unicode_sets) {
        if (re_string_list_canonicalize(s, cr, s->is_unicode)) {
            re_string_list_free(cr);
            goto out_of_memory;
        }
    }
    if (is_inv) {
        if (cvm_cr_invert(&cr->cr)) {
            re_string_list_free(cr);
            goto out_of_memory;
        }
    }
    if (s->ignore_case && !s->unicode_sets) {
        if (re_string_list_canonicalize(s, cr, s->is_unicode)) {
            re_string_list_free(cr);
            goto out_of_memory;
        }
    }
    *pp = p;
    return 0;
out_of_memory:
    return re_parse_out_of_memory(s);
}

static i32 get_class_atom(REParseState* s, REStringList* cr, const u8** pp,
                          bool inclass);

static i32 parse_class_string_disjunction(REParseState* s, REStringList* cr,
                                          const u8** pp) {
    const u8* p;
    CvmBuf str;
    i32 c;

    p = *pp;
    if (*p != '{')
        return re_parse_error(s, (u8 const*)"expecting '{' after \\q");

    cvm_buf_init(&str, ((CvmRegexCtx*)(s->opaque))->alloc);
    re_string_list_init(s, cr);

    p++;
    for (;;) {
        str.size = 0;
        while (*p != '}' && *p != '|') {
            c = get_class_atom(s, NULL, &p, true);
            if (c < 0)
                goto fail;
            if (re_buf_put_u32(&str, c)) {
                re_parse_out_of_memory(s);
                goto fail;
            }
        }
        if (re_string_add(cr, str.size / 4, (u32*)str.data)) {
            re_parse_out_of_memory(s);
            goto fail;
        }
        if (*p == '}')
            break;
        p++;
    }
    if (s->ignore_case) {
        if (re_string_list_canonicalize(s, cr, true))
            goto fail;
    }
    p++; /* skip the '}' */
    cvm_buf_free(&str);
    *pp = p;
    return 0;
fail:
    cvm_buf_free(&str);
    re_string_list_free(cr);
    return -1;
}

/* return -1 if error otherwise the character or a class range
   (CLASS_RANGE_BASE) if cr != NULL. In case of class range, 'cr' is
   initialized. Otherwise, it is ignored. */
static i32 get_class_atom(REParseState* s, REStringList* cr, const u8** pp,
                          bool inclass) {
    const u8* p;
    u32 c;
    i32 ret;

    p = *pp;

    c = *p;
    switch (c) {
    case '\\':
        p++;
        if (p >= s->buf_end)
            goto unexpected_end;
        c = *p++;
        switch (c) {
        case 'd':
            c = CHAR_RANGE_d;
            goto class_range;
        case 'D':
            c = CHAR_RANGE_D;
            goto class_range;
        case 's':
            c = CHAR_RANGE_s;
            goto class_range;
        case 'S':
            c = CHAR_RANGE_S;
            goto class_range;
        case 'w':
            c = CHAR_RANGE_w;
            goto class_range;
        case 'W':
            c = CHAR_RANGE_W;
class_range:
            if (!cr)
                goto default_escape;
            if (cr_init_char_range(s, cr, c))
                return -1;
            c += CLASS_RANGE_BASE;
            break;
        case 'c':
            c = *p;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (((c >= '0' && c <= '9') || c == '_') && inclass &&
                 !s->is_unicode)) { /* Annex B.1.4 */
                c &= 0x1f;
                p++;
            } else if (s->is_unicode) {
                goto invalid_escape;
            } else {
                /* otherwise return '\' and 'c' */
                p--;
                c = '\\';
            }
            break;
        case '-':
            if (!inclass && s->is_unicode)
                goto invalid_escape;
            break;
        case '&':
        case '!':
        case '#':
        case '%':
        case ',':
        case ':':
        case ';':
        case '<':
        case '=':
        case '>':
        case '@':
        case '`':
        case '~':
            if (s->is_unicode && (!inclass || !s->unicode_sets))
                /* Only illegal if in unicode mode and not in a class */
                goto invalid_escape;
            break;
        case '^':
        case '$':
        case '\\':
        case '.':
        case '*':
        case '+':
        case '?':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '|':
        case '/':
            /* always valid to escape these characters */
            break;
        case 'p':
        case 'P':
            if (s->is_unicode && cr) {
                if (parse_unicode_property(s, cr, &p, (c == 'P'), s->unicode_sets))
                    return -1;
                c = CLASS_RANGE_BASE;
                break;
            }
            goto default_escape;
        case 'q':
            if (s->unicode_sets && cr && inclass) {
                if (parse_class_string_disjunction(s, cr, &p))
                    return -1;
                c = CLASS_RANGE_BASE;
                break;
            }
            goto default_escape;
        default:
default_escape:
            p--;
            ret = cvm_lre_parse_escape(&p, s->is_unicode * 2);
            if (ret >= 0) {
                c = ret;
            } else {
                if (s->is_unicode) {
invalid_escape:
                    return re_parse_error(
                        s, (u8 const*)"invalid escape sequence in regular expression");
                } else {
                    /* just ignore the '\' */
                    goto normal_char;
                }
            }
            break;
        }
        break;
    case '\0':
        if (p >= s->buf_end) {
unexpected_end:
            return re_parse_error(s, (u8 const*)"unexpected end");
        }
        /* fall thru */
        goto normal_char;

    case '&':
    case '!':
    case '#':
    case '$':
    case '%':
    case '*':
    case '+':
    case ',':
    case '.':
    case ':':
    case ';':
    case '<':
    case '=':
    case '>':
    case '?':
    case '@':
    case '^':
    case '`':
    case '~':
        if (s->unicode_sets && p[1] == c) {
            /* forbidden double characters */
            return re_parse_error(
                s, (u8 const*)"invalid class set operation in regular expression");
        }
        goto normal_char;

    case '(':
    case ')':
    case '[':
    case ']':
    case '{':
    case '}':
    case '/':
    case '-':
    case '|':
        if (s->unicode_sets) {
            /* invalid characters in unicode sets */
            return re_parse_error(
                s, (u8 const*)"invalid character in class in regular expression");
        }
        goto normal_char;

    default:
normal_char:
        /* normal u8*/
        if (c >= 128) {
            c = re_utf8_decode_len(p, CVM_UTF8_MAX_BYTES, &p);
            if ((u32)c > 0xffff && !s->is_unicode) {
                /* XXX: should handle non BMP-1 code points */
                return re_parse_error(s, (u8 const*)"malformed unicode char");
            }
        } else {
            p++;
        }
        break;
    }
    *pp = p;
    return c;
}

static i32 re_emit_range(REParseState* s, const CvmCharRange* cr) {
    i32 len, i;
    u32 high;

    len = (u32)cr->len / 2;
    if (len >= 65535)
        return re_parse_error(s, (u8 const*)"too many ranges");
    if (len == 0) {
        re_emit_op_u32(s, REOP_char32, -1);
    } else {
        high = cr->points[cr->len - 1];
        if (high == UINT32_MAX)
            high = cr->points[cr->len - 2];
        if (high <= 0xffff) {
            /* can use 16 bit ranges with the conversion that 0xffff =
               infinity */
            re_emit_op_u16(s, s->ignore_case ? REOP_range_i : REOP_range, len);
            for (i = 0; i < cr->len; i += 2) {
                re_buf_put_u16(&s->byte_code, cr->points[i]);
                high = cr->points[i + 1] - 1;
                if (high == UINT32_MAX - 1)
                    high = 0xffff;
                re_buf_put_u16(&s->byte_code, high);
            }
        } else {
            re_emit_op_u16(s, s->ignore_case ? REOP_range32_i : REOP_range32, len);
            for (i = 0; i < cr->len; i += 2) {
                re_buf_put_u32(&s->byte_code, cr->points[i]);
                re_buf_put_u32(&s->byte_code, cr->points[i + 1] - 1);
            }
        }
    }
    return 0;
}

static i32 re_string_cmp_len(const void* a, const void* b, void* arg) {
    CVM_UNUSED(arg);
    REString* p1 = *(REString**)a;
    REString* p2 = *(REString**)b;
    return (p1->len < p2->len) - (p1->len > p2->len);
}

static void re_emit_char(REParseState* s, i32 c) {
    if (c <= 0xffff)
        re_emit_op_u16(s, s->ignore_case ? REOP_char_i : REOP_char, c);
    else
        re_emit_op_u32(s, s->ignore_case ? REOP_char32_i : REOP_char32, c);
}

static i32 re_emit_string_list(REParseState* s, const REStringList* sl) {
    REString **tab, *p;
    i32 i, j, split_pos, last_match_pos, n;
    bool has_empty_string, is_last;

    //    re_string_list_dump((u8 const*)"sl", sl);
    if (sl->n_strings == 0) {
        /* simple case: only characters */
        if (re_emit_range(s, &sl->cr))
            return -1;
    } else {
        /* at least one string list is present : match the longest ones first */
        /* XXX: add a new op_switch opcode to compile as a trie */
        tab = re_alloc(s->opaque, NULL, sizeof(tab[0]) * sl->n_strings);
        if (!tab) {
            re_parse_out_of_memory(s);
            return -1;
        }
        has_empty_string = false;
        n = 0;
        for (i = 0; i < (i32)sl->hash_size; i++) {
            for (p = sl->hash_table[i]; p != NULL; p = p->next) {
                if (p->len == 0) {
                    has_empty_string = true;
                } else {
                    tab[n++] = p;
                }
            }
        }
        cvm_assert((u32)n <= sl->n_strings);

        cvm_rqsort(tab, n, sizeof(tab[0]), re_string_cmp_len, NULL);

        last_match_pos = -1;
        for (i = 0; i < n; i++) {
            p = tab[i];
            is_last = !has_empty_string && sl->cr.len == 0 && i == (n - 1);
            if (!is_last)
                split_pos = re_emit_op_u32(s, REOP_split_next_first, 0);
            else
                split_pos = 0;
            for (j = 0; j < (i32)p->len; j++) {
                re_emit_char(s, p->buf[j]);
            }
            if (!is_last) {
                last_match_pos = re_emit_op_u32(s, REOP_goto, last_match_pos);
                /* the positions to patch are only valid if all the byte code
                   could be emitted */
                if (cvm_buf_error(&s->byte_code))
                    goto out_of_memory;
                re_put_u32(s->byte_code.data + split_pos,
                           s->byte_code.size - (split_pos + 4));
            }
        }

        if (sl->cr.len != 0) {
            /* char range */
            is_last = !has_empty_string;
            if (!is_last)
                split_pos = re_emit_op_u32(s, REOP_split_next_first, 0);
            else
                split_pos = 0; /* not used */
            if (re_emit_range(s, &sl->cr))
                goto fail;
            if (!is_last) {
                if (cvm_buf_error(&s->byte_code))
                    goto out_of_memory;
                re_put_u32(s->byte_code.data + split_pos,
                           s->byte_code.size - (split_pos + 4));
            }
        }

        /* patch the 'goto match' */
        if (cvm_buf_error(&s->byte_code))
            goto out_of_memory;
        while (last_match_pos != -1) {
            i32 next_pos = re_get_u32(s->byte_code.data + last_match_pos);
            re_put_u32(s->byte_code.data + last_match_pos,
                       s->byte_code.size - (last_match_pos + 4));
            last_match_pos = next_pos;
        }

        re_alloc(s->opaque, tab, 0);
    }
    return 0;
out_of_memory:
    re_parse_out_of_memory(s);
fail:
    re_alloc(s->opaque, tab, 0);
    return -1;
}

static i32 re_parse_nested_class(REParseState* s, REStringList* cr, const u8** pp);

static i32 re_parse_class_set_operand(REParseState* s, REStringList* cr,
                                      const u8** pp) {
    i32 c1;
    const u8* p = *pp;

    if (*p == '[') {
        if (re_parse_nested_class(s, cr, pp))
            return -1;
    } else {
        c1 = get_class_atom(s, cr, pp, true);
        if (c1 < 0)
            return -1;
        if (c1 < CLASS_RANGE_BASE) {
            /* create a range with a single character */
            re_string_list_init(s, cr);
            if (s->ignore_case)
                c1 = cvm_unicode_canonicalize(c1, s->is_unicode);
            if (cvm_cr_union_interval(&cr->cr, c1, c1)) {
                re_string_list_free(cr);
                return -1;
            }
        }
    }
    return 0;
}

static i32 re_parse_nested_class(REParseState* s, REStringList* cr, const u8** pp) {
    const u8* p;
    u32 c1, c2;
    i32 ret;
    REStringList cr1_s, *cr1 = &cr1_s;
    bool invert, is_first;

    if (re_check_stack_overflow(s->opaque, 0))
        return re_parse_error(s, (u8 const*)"stack overflow");

    re_string_list_init(s, cr);
    p = *pp;
    p++; /* skip '[' */

    invert = false;
    if (*p == '^') {
        p++;
        invert = true;
    }

    /* handle unions */
    is_first = true;
    for (;;) {
        if (*p == ']')
            break;
        if (*p == '[' && s->unicode_sets) {
            if (re_parse_nested_class(s, cr1, &p))
                goto fail;
            goto class_union;
        } else {
            c1 = get_class_atom(s, cr1, &p, true);
            if ((i32)c1 < 0)
                goto fail;
            if (*p == '-' && p[1] != ']') {
                const u8* p0 = p + 1;
                if (p[1] == '-' && s->unicode_sets && is_first)
                    goto class_atom; /* first character class followed by '--' */
                if (c1 >= CLASS_RANGE_BASE) {
                    if (s->is_unicode) {
                        re_string_list_free(cr1);
                        goto invalid_class_range;
                    }
                    /* Annex B: match '-' character */
                    goto class_atom;
                }
                c2 = get_class_atom(s, cr1, &p0, true);
                if ((i32)c2 < 0)
                    goto fail;
                if (c2 >= CLASS_RANGE_BASE) {
                    re_string_list_free(cr1);
                    if (s->is_unicode) {
                        goto invalid_class_range;
                    }
                    /* Annex B: match '-' character */
                    goto class_atom;
                }
                p = p0;
                if (c2 < c1) {
invalid_class_range:
                    re_parse_error(s, (u8 const*)"invalid class range");
                    goto fail;
                }
                if (s->ignore_case) {
                    CvmCharRange cr2_s, *cr2 = &cr2_s;
                    cvm_cr_init(cr2, ((CvmRegexCtx*)(s->opaque))->alloc);
                    if (cvm_cr_add_interval(cr2, c1, c2 + 1) ||
                        cvm_cr_regexp_canonicalize(cr2, s->is_unicode) ||
                        cvm_cr_op1(&cr->cr, cr2->points, cr2->len, CVM_CR_OP_UNION)) {
                        cvm_cr_free(cr2);
                        goto memory_error;
                    }
                    cvm_cr_free(cr2);
                } else {
                    if (cvm_cr_union_interval(&cr->cr, c1, c2))
                        goto memory_error;
                }
                is_first = false; /* union operation */
            } else {
class_atom:
                if (c1 >= CLASS_RANGE_BASE) {
class_union:
                    ret = re_string_list_op(cr, cr1, CVM_CR_OP_UNION);
                    re_string_list_free(cr1);
                    if (ret)
                        goto memory_error;
                } else {
                    if (s->ignore_case)
                        c1 = cvm_unicode_canonicalize(c1, s->is_unicode);
                    if (cvm_cr_union_interval(&cr->cr, c1, c1))
                        goto memory_error;
                }
            }
        }
        if (s->unicode_sets && is_first) {
            if (*p == '&' && p[1] == '&' && p[2] != '&') {
                /* handle '&&' */
                for (;;) {
                    if (*p == ']') {
                        break;
                    } else if (*p == '&' && p[1] == '&' && p[2] != '&') {
                        p += 2;
                    } else {
                        goto invalid_operation;
                    }
                    if (re_parse_class_set_operand(s, cr1, &p))
                        goto fail;
                    ret = re_string_list_op(cr, cr1, CVM_CR_OP_INTER);
                    re_string_list_free(cr1);
                    if (ret)
                        goto memory_error;
                }
            } else if (*p == '-' && p[1] == '-') {
                /* handle '--' */
                for (;;) {
                    if (*p == ']') {
                        break;
                    } else if (*p == '-' && p[1] == '-') {
                        p += 2;
                    } else {
invalid_operation:
                        re_parse_error(
                            s, (u8 const*)"invalid operation in regular expression");
                        goto fail;
                    }
                    if (re_parse_class_set_operand(s, cr1, &p))
                        goto fail;
                    ret = re_string_list_op(cr, cr1, CVM_CR_OP_SUB);
                    re_string_list_free(cr1);
                    if (ret)
                        goto memory_error;
                }
            }
        }
        is_first = false;
    }

    p++; /* skip ']' */
    *pp = p;
    if (invert) {
        /* XXX: add may_contain_string syntax check to be fully
           compliant. The test here accepts more input than the
           spec. */
        if (cr->n_strings != 0) {
            re_parse_error(s, (u8 const*)"negated character class with strings in "
                                         "regular expression debugger eval code");
            goto fail;
        }
        if (cvm_cr_invert(&cr->cr))
            goto memory_error;
    }
    return 0;
memory_error:
    re_parse_out_of_memory(s);
fail:
    re_string_list_free(cr);
    return -1;
}

static i32 re_parse_char_class(REParseState* s, const u8** pp) {
    REStringList cr_s, *cr = &cr_s;

    if (re_parse_nested_class(s, cr, pp))
        return -1;
    if (re_emit_string_list(s, cr))
        goto fail;
    re_string_list_free(cr);
    return 0;
fail:
    re_string_list_free(cr);
    return -1;
}

/* need_check_adv: false if the opcodes always advance the char pointer
   need_capture_init: true if all the captures in the atom are not set
*/
static bool re_need_check_adv_and_capture_init(bool* pneed_capture_init,
                                               const u8* bc_buf, i32 bc_buf_len) {
    i32 pos, opcode, len;
    u32 val;
    bool need_check_adv, need_capture_init;

    need_check_adv = true;
    need_capture_init = false;
    pos = 0;
    while (pos < bc_buf_len) {
        opcode = bc_buf[pos];
        len = reopcode_info[opcode].size;
        switch (opcode) {
        case REOP_range:
        case REOP_range_i:
            val = re_get_u16(bc_buf + pos + 1);
            len += val * 4;
            need_check_adv = false;
            break;
        case REOP_range32:
        case REOP_range32_i:
            val = re_get_u16(bc_buf + pos + 1);
            len += val * 8;
            need_check_adv = false;
            break;
        case REOP_char:
        case REOP_char_i:
        case REOP_char32:
        case REOP_char32_i:
        case REOP_dot:
        case REOP_any:
        case REOP_space:
        case REOP_not_space:
            need_check_adv = false;
            break;
        case REOP_line_start:
        case REOP_line_start_m:
        case REOP_line_end:
        case REOP_line_end_m:
        case REOP_set_i32:
        case REOP_set_char_pos:
        case REOP_word_boundary:
        case REOP_word_boundary_i:
        case REOP_not_word_boundary:
        case REOP_not_word_boundary_i:
        case REOP_prev:
            /* no effect */
            break;
        case REOP_save_start:
        case REOP_save_end:
        case REOP_save_reset:
            break;
        case REOP_back_reference:
        case REOP_back_reference_i:
        case REOP_backward_back_reference:
        case REOP_backward_back_reference_i:
            val = bc_buf[pos + 1];
            len += val;
            need_capture_init = true;
            break;
        default:
            /* safe behavior: we cannot predict the outcome */
            need_capture_init = true;
            goto done;
        }
        pos += len;
    }
done:
    *pneed_capture_init = need_capture_init;
    return need_check_adv;
}

/* '*pp' is the first char after '<' */
static i32 re_parse_group_name(u8* buf, i32 buf_size, const u8** pp) {
    const u8 *p, *p1;
    u32 c, d;
    u8* q;

    p = *pp;
    q = buf;
    for (;;) {
        c = *p;
        if (c == '\\') {
            p++;
            if (*p != 'u')
                return -1;
            c = cvm_lre_parse_escape(&p, 2); // accept surrogate pairs
        } else if (c == '>') {
            break;
        } else if (c >= 128) {
            c = re_utf8_decode_len(p, CVM_UTF8_MAX_BYTES, &p);
            if (re_is_hi_surrogate(c)) {
                d = re_utf8_decode_len(p, CVM_UTF8_MAX_BYTES, &p1);
                if (re_is_lo_surrogate(d)) {
                    c = re_from_surrogate(c, d);
                    p = p1;
                }
            }
        } else {
            p++;
        }
        if (c > 0x10FFFF)
            return -1;
        if (q == buf) {
            if (!cvm_lre_js_is_ident_first(c))
                return -1;
        } else {
            if (!cvm_lre_js_is_ident_next(c))
                return -1;
        }
        if ((q - buf + CVM_UTF8_MAX_BYTES + 1) > buf_size)
            return -1;
        if (c < 128) {
            *q++ = c;
        } else {
            q += re_utf8_encode((u8*)q, c);
        }
    }
    if (q == buf)
        return -1;
    *q = '\0';
    p++;
    *pp = p;
    return 0;
}

/* if capture_name = NULL: return the number of captures + 1.
   Otherwise, return the number of matching capture groups  */
static i32 re_parse_captures(REParseState* s, i32* phas_named_captures,
                             u8 const* capture_name, bool emit_group_index) {
    const u8* p;
    i32 capture_index, n;
    u8 name[TMP_BUF_SIZE];

    capture_index = 1;
    n = 0;
    *phas_named_captures = 0;
    for (p = s->buf_start; p < s->buf_end; p++) {
        switch (*p) {
        case '(':
            if (p[1] == '?') {
                if (p[2] == '<' && p[3] != '=' && p[3] != '!') {
                    *phas_named_captures = 1;
                    /* potential named capture */
                    if (capture_name) {
                        p += 3;
                        if (re_parse_group_name(name, sizeof(name), &p) == 0) {
                            if (!re_strcmp(name, capture_name)) {
                                if (emit_group_index)
                                    re_putc(&s->byte_code, capture_index);
                                n++;
                            }
                        }
                    }
                    capture_index++;
                    if (capture_index >= CAPTURE_COUNT_MAX)
                        goto done;
                }
            } else {
                capture_index++;
                if (capture_index >= CAPTURE_COUNT_MAX)
                    goto done;
            }
            break;
        case '\\':
            p++;
            break;
        case '[':
            for (p += 1 + (*p == ']'); p < s->buf_end && *p != ']'; p++) {
                if (*p == '\\')
                    p++;
            }
            break;
        }
    }
done:
    if (capture_name) {
        return n;
    } else {
        return capture_index;
    }
}

static i32 re_count_captures(REParseState* s) {
    if (s->total_capture_count < 0) {
        s->total_capture_count =
            re_parse_captures(s, &s->has_named_captures, NULL, false);
    }
    return s->total_capture_count;
}

static bool re_has_named_captures(REParseState* s) {
    if (s->has_named_captures < 0)
        re_count_captures(s);
    return s->has_named_captures;
}

static i32 find_group_name(REParseState* s, u8 const* name, bool emit_group_index) {
    u8 const *p, *buf_end;
    usize len, name_len;
    i32 capture_index, n;

    p = (u8*)s->group_names.data;
    if (!p)
        return 0;
    buf_end = (u8*)s->group_names.data + s->group_names.size;
    name_len = re_strlen(name);
    capture_index = 1;
    n = 0;
    while (p < buf_end) {
        len = re_strlen(p);
        if (len == name_len && memcmp(name, p, name_len) == 0) {
            if (emit_group_index)
                re_putc(&s->byte_code, capture_index);
            n++;
        }
        p += len + CVM_RE_GROUP_NAME_TRAILER_LEN;
        capture_index++;
    }
    return n;
}

static bool is_duplicate_group_name(REParseState* s, u8 const* name, i32 scope) {
    u8 const *p, *buf_end;
    usize len, name_len;
    i32 scope1;

    p = (u8*)s->group_names.data;
    if (!p)
        return 0;
    buf_end = (u8*)s->group_names.data + s->group_names.size;
    name_len = re_strlen(name);
    while (p < buf_end) {
        len = re_strlen(p);
        if (len == name_len && memcmp(name, p, name_len) == 0) {
            scope1 = (u8)p[len + 1];
            if (scope == scope1)
                return true;
        }
        p += len + CVM_RE_GROUP_NAME_TRAILER_LEN;
    }
    return false;
}

static i32 re_parse_disjunction(REParseState* s, bool is_backward_dir);

static i32 re_parse_modifiers(REParseState* s, const u8** pp) {
    const u8* p = *pp;
    i32 mask = 0;
    i32 val;

    for (;;) {
        if (*p == 'i') {
            val = CVM_RE_FLAG_IGNORECASE;
        } else if (*p == 'm') {
            val = CVM_RE_FLAG_MULTILINE;
        } else if (*p == 's') {
            val = CVM_RE_FLAG_DOTALL;
        } else {
            break;
        }
        if (mask & val)
            return re_parse_error_char(s, (u8 const*)"duplicate modifier: '", *p,
                                       (u8 const*)"'");
        mask |= val;
        p++;
    }
    *pp = p;
    return mask;
}

static bool update_modifier(bool val, i32 add_mask, i32 remove_mask, i32 mask) {
    if (add_mask & mask)
        val = true;
    if (remove_mask & mask)
        val = false;
    return val;
}

static i32 re_parse_term(REParseState* s, bool is_backward_dir) {
    const u8* p;
    i32 c, last_atom_start, quant_min, quant_max, last_capture_count;
    bool greedy, is_neg, is_backward_lookahead;
    REStringList cr_s, *cr = &cr_s;

    /* after a failed allocation the byte code is incomplete and 'buf' may
       still be NULL: stop before computing positions inside it */
    if (cvm_buf_error(&s->byte_code))
        return re_parse_out_of_memory(s);

    last_atom_start = -1;
    last_capture_count = 0;
    p = s->buf_ptr;
    c = *p;
    switch (c) {
    case '^':
        p++;
        re_emit_op(s, s->multi_line ? REOP_line_start_m : REOP_line_start);
        break;
    case '$':
        p++;
        re_emit_op(s, s->multi_line ? REOP_line_end_m : REOP_line_end);
        break;
    case '.':
        p++;
        last_atom_start = s->byte_code.size;
        last_capture_count = s->capture_count;
        if (is_backward_dir)
            re_emit_op(s, REOP_prev);
        re_emit_op(s, s->dotall ? REOP_any : REOP_dot);
        if (is_backward_dir)
            re_emit_op(s, REOP_prev);
        break;
    case '{':
        if (s->is_unicode) {
            return re_parse_error(s, (u8 const*)"syntax error");
        } else if (!lre_is_digit(p[1])) {
            /* Annex B: we accept '{' not followed by digits as a
               normal atom */
            goto parse_class_atom;
        } else {
            const u8* p1 = p + 1;
            /* Annex B: error if it is like a repetition count */
            parse_digits(&p1, true);
            if (*p1 == ',') {
                p1++;
                if (lre_is_digit(*p1)) {
                    parse_digits(&p1, true);
                }
            }
            if (*p1 != '}') {
                goto parse_class_atom;
            }
        }
        /* fall thru */
    case '*':
    case '+':
    case '?':
        return re_parse_error(s, (u8 const*)"nothing to repeat");
    case '(':
        if (p[1] == '?') {
            if (p[2] == ':') {
                p += 3;
                last_atom_start = s->byte_code.size;
                last_capture_count = s->capture_count;
                s->buf_ptr = p;
                if (re_parse_disjunction(s, is_backward_dir))
                    return -1;
                p = s->buf_ptr;
                if (re_parse_expect(s, &p, ')'))
                    return -1;
            } else if (p[2] == 'i' || p[2] == 'm' || p[2] == 's' || p[2] == '-') {
                bool saved_ignore_case, saved_multi_line, saved_dotall;
                i32 add_mask, remove_mask;
                p += 2;
                remove_mask = 0;
                add_mask = re_parse_modifiers(s, &p);
                if (add_mask < 0)
                    return -1;
                if (*p == '-') {
                    p++;
                    remove_mask = re_parse_modifiers(s, &p);
                    if (remove_mask < 0)
                        return -1;
                }
                if ((add_mask == 0 && remove_mask == 0) ||
                    (add_mask & remove_mask) != 0) {
                    return re_parse_error(s, (u8 const*)"invalid modifiers");
                }
                if (re_parse_expect(s, &p, ':'))
                    return -1;
                saved_ignore_case = s->ignore_case;
                saved_multi_line = s->multi_line;
                saved_dotall = s->dotall;
                s->ignore_case = update_modifier(s->ignore_case, add_mask, remove_mask,
                                                 CVM_RE_FLAG_IGNORECASE);
                s->multi_line = update_modifier(s->multi_line, add_mask, remove_mask,
                                                CVM_RE_FLAG_MULTILINE);
                s->dotall = update_modifier(s->dotall, add_mask, remove_mask,
                                            CVM_RE_FLAG_DOTALL);

                last_atom_start = s->byte_code.size;
                last_capture_count = s->capture_count;
                s->buf_ptr = p;
                if (re_parse_disjunction(s, is_backward_dir))
                    return -1;
                p = s->buf_ptr;
                if (re_parse_expect(s, &p, ')'))
                    return -1;
                s->ignore_case = saved_ignore_case;
                s->multi_line = saved_multi_line;
                s->dotall = saved_dotall;
            } else if ((p[2] == '=' || p[2] == '!')) {
                is_neg = (p[2] == '!');
                is_backward_lookahead = false;
                p += 3;
                goto lookahead;
            } else if (p[2] == '<' && (p[3] == '=' || p[3] == '!')) {
                i32 pos;
                is_neg = (p[3] == '!');
                is_backward_lookahead = true;
                p += 4;
                /* lookahead */
lookahead:
                /* Annex B allows lookahead to be used as an atom for
                   the quantifiers */
                if (!s->is_unicode && !is_backward_lookahead) {
                    last_atom_start = s->byte_code.size;
                    last_capture_count = s->capture_count;
                }
                pos = re_emit_op_u32(s, REOP_lookahead + is_neg, 0);
                s->buf_ptr = p;
                if (re_parse_disjunction(s, is_backward_lookahead))
                    return -1;
                p = s->buf_ptr;
                if (re_parse_expect(s, &p, ')'))
                    return -1;
                re_emit_op(s, REOP_lookahead_match + is_neg);
                /* jump after the 'match' after the lookahead is successful */
                if (cvm_buf_error(&s->byte_code))
                    return re_parse_out_of_memory(s);
                re_put_u32(s->byte_code.data + pos, s->byte_code.size - (pos + 4));
            } else if (p[2] == '<') {
                p += 3;
                if (re_parse_group_name(s->u.tmp_buf, sizeof(s->u.tmp_buf), &p)) {
                    return re_parse_error(s, (u8 const*)"invalid group name");
                }
                /* poor's man method to test duplicate group
                   names. */
                /* XXX: this method does not catch all the errors*/
                if (is_duplicate_group_name(s, s->u.tmp_buf, s->group_name_scope)) {
                    return re_parse_error(s, (u8 const*)"duplicate group name");
                }
                /* group name with a trailing zero */
                re_put(&s->group_names, (u8*)s->u.tmp_buf, re_strlen(s->u.tmp_buf) + 1);
                re_putc(&s->group_names, s->group_name_scope);
                s->has_named_captures = 1;
                goto parse_capture;
            } else {
                return re_parse_error(s, (u8 const*)"invalid group");
            }
        } else {
            i32 capture_index;
            p++;
            /* capture without group name */
            re_putc(&s->group_names, 0);
            re_putc(&s->group_names, 0);
parse_capture:
            if (s->capture_count >= CAPTURE_COUNT_MAX)
                return re_parse_error(s, (u8 const*)"too many captures");
            last_atom_start = s->byte_code.size;
            last_capture_count = s->capture_count;
            capture_index = s->capture_count++;
            re_emit_op_u8(s, REOP_save_start + is_backward_dir, capture_index);

            s->buf_ptr = p;
            if (re_parse_disjunction(s, is_backward_dir))
                return -1;
            p = s->buf_ptr;

            re_emit_op_u8(s, REOP_save_start + 1 - is_backward_dir, capture_index);

            if (re_parse_expect(s, &p, ')'))
                return -1;
        }
        break;
    case '\\':
        switch (p[1]) {
        case 'b':
        case 'B':
            if (p[1] != 'b') {
                re_emit_op(s, s->ignore_case && s->is_unicode ? REOP_not_word_boundary_i
                                                              : REOP_not_word_boundary);
            } else {
                re_emit_op(s, s->ignore_case && s->is_unicode ? REOP_word_boundary_i
                                                              : REOP_word_boundary);
            }
            p += 2;
            break;
        case 'k': {
            const u8* p1;
            i32 dummy_res, n;
            bool is_forward;

            p1 = p;
            if (p1[2] != '<') {
                /* annex B: we tolerate invalid group names in non
                       unicode mode if there is no named capture
                       definition */
                if (s->is_unicode || re_has_named_captures(s))
                    return re_parse_error(s, (u8 const*)"expecting group name");
                else
                    goto parse_class_atom;
            }
            p1 += 3;
            if (re_parse_group_name(s->u.tmp_buf, sizeof(s->u.tmp_buf), &p1)) {
                if (s->is_unicode || re_has_named_captures(s))
                    return re_parse_error(s, (u8 const*)"invalid group name");
                else
                    goto parse_class_atom;
            }
            is_forward = false;
            n = find_group_name(s, s->u.tmp_buf, false);
            if (n == 0) {
                /* no capture name parsed before, try to look
                       after (inefficient, but hopefully not common */
                n = re_parse_captures(s, &dummy_res, s->u.tmp_buf, false);
                if (n == 0) {
                    if (s->is_unicode || re_has_named_captures(s))
                        return re_parse_error(s, (u8 const*)"group name not defined");
                    else
                        goto parse_class_atom;
                }
                is_forward = true;
            }
            last_atom_start = s->byte_code.size;
            last_capture_count = s->capture_count;

            /* emit back references to all the captures indexes matching the group name */
            re_emit_op_u8(s, REOP_back_reference + 2 * is_backward_dir + s->ignore_case,
                          n);
            if (is_forward) {
                re_parse_captures(s, &dummy_res, s->u.tmp_buf, true);
            } else {
                find_group_name(s, s->u.tmp_buf, true);
            }
            p = p1;
        } break;
        case '0':
            p += 2;
            c = 0;
            if (s->is_unicode) {
                if (lre_is_digit(*p)) {
                    return re_parse_error(
                        s, (u8 const*)"invalid decimal escape in regular expression");
                }
            } else {
                /* Annex B.1.4: accept legacy octal */
                if (*p >= '0' && *p <= '7') {
                    c = *p++ - '0';
                    if (*p >= '0' && *p <= '7') {
                        c = (c << 3) + *p++ - '0';
                    }
                }
            }
            goto normal_char;
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9': {
            const u8* q = ++p;

            c = parse_digits(&p, false);
            if (c < 0 || (c >= s->capture_count && c >= re_count_captures(s))) {
                if (!s->is_unicode) {
                    /* Annex B.1.4: accept legacy octal */
                    p = q;
                    if (*p <= '7') {
                        c = 0;
                        if (*p <= '3')
                            c = *p++ - '0';
                        if (*p >= '0' && *p <= '7') {
                            c = (c << 3) + *p++ - '0';
                            if (*p >= '0' && *p <= '7') {
                                c = (c << 3) + *p++ - '0';
                            }
                        }
                    } else {
                        c = *p++;
                    }
                    goto normal_char;
                }
                return re_parse_error(
                    s, (u8 const*)"back reference out of range in regular expression");
            }
            last_atom_start = s->byte_code.size;
            last_capture_count = s->capture_count;

            re_emit_op_u8(s, REOP_back_reference + 2 * is_backward_dir + s->ignore_case,
                          1);
            re_putc(&s->byte_code, c);
        } break;
        default:
            goto parse_class_atom;
        }
        break;
    case '[':
        last_atom_start = s->byte_code.size;
        last_capture_count = s->capture_count;
        if (is_backward_dir)
            re_emit_op(s, REOP_prev);
        if (re_parse_char_class(s, &p))
            return -1;
        if (is_backward_dir)
            re_emit_op(s, REOP_prev);
        break;
    case ']':
    case '}':
        if (s->is_unicode)
            return re_parse_error(s, (u8 const*)"syntax error");
        goto parse_class_atom;
    default:
parse_class_atom:
        c = get_class_atom(s, cr, &p, false);
        if ((i32)c < 0)
            return -1;
normal_char:
        last_atom_start = s->byte_code.size;
        last_capture_count = s->capture_count;
        if (is_backward_dir)
            re_emit_op(s, REOP_prev);
        if (c >= CLASS_RANGE_BASE) {
            i32 ret = 0;
            /* optimize the common 'space' tests */
            if (c == (CLASS_RANGE_BASE + CHAR_RANGE_s)) {
                re_emit_op(s, REOP_space);
            } else if (c == (CLASS_RANGE_BASE + CHAR_RANGE_S)) {
                re_emit_op(s, REOP_not_space);
            } else {
                ret = re_emit_string_list(s, cr);
            }
            re_string_list_free(cr);
            if (ret)
                return -1;
        } else {
            if (s->ignore_case)
                c = cvm_unicode_canonicalize(c, s->is_unicode);
            re_emit_char(s, c);
        }
        if (is_backward_dir)
            re_emit_op(s, REOP_prev);
        break;
    }

    /* quantifier */
    if (last_atom_start >= 0) {
        c = *p;
        switch (c) {
        case '*':
            p++;
            quant_min = 0;
            quant_max = INT32_MAX;
            goto quantifier;
        case '+':
            p++;
            quant_min = 1;
            quant_max = INT32_MAX;
            goto quantifier;
        case '?':
            p++;
            quant_min = 0;
            quant_max = 1;
            goto quantifier;
        case '{': {
            const u8* p1 = p;
            /* As an extension (see ES6 annex B), we accept '{' not
                   followed by digits as a normal atom */
            if (!lre_is_digit(p[1])) {
                if (s->is_unicode)
                    goto invalid_quant_count;
                break;
            }
            p++;
            quant_min = parse_digits(&p, true);
            quant_max = quant_min;
            if (*p == ',') {
                p++;
                if (lre_is_digit(*p)) {
                    quant_max = parse_digits(&p, true);
                    if (quant_max < quant_min) {
invalid_quant_count:
                        return re_parse_error(s, (u8 const*)"invalid repetition count");
                    }
                } else {
                    quant_max = INT32_MAX; /* infinity */
                }
            }
            if (*p != '}' && !s->is_unicode) {
                /* Annex B: normal atom if invalid '{' syntax */
                p = p1;
                break;
            }
            if (re_parse_expect(s, &p, '}'))
                return -1;
        }
quantifier:
            greedy = true;
            if (*p == '?') {
                p++;
                greedy = false;
            }
            if (last_atom_start < 0) {
                return re_parse_error(s, (u8 const*)"nothing to repeat");
            }
            {
                bool need_capture_init, add_zero_advance_check;
                i32 len, pos;

                /* the spec tells that if there is no advance when
                   running the atom after the first quant_min times,
                   then there is no match. We remove this test when we
                   are sure the atom always advances the position. */
                add_zero_advance_check = re_need_check_adv_and_capture_init(
                    &need_capture_init, s->byte_code.data + last_atom_start,
                    s->byte_code.size - last_atom_start);

                /* general case: need to reset the capture at each
                   iteration. We don't do it if there are no captures
                   in the atom or if we are sure all captures are
                   initialized in the atom. If quant_min = 0, we still
                   need to reset once the captures in case the atom
                   does not match. */
                if (need_capture_init && last_capture_count != s->capture_count) {
                    if (re_buf_insert(&s->byte_code, last_atom_start, 3))
                        goto out_of_memory;
                    i32 pos = last_atom_start;
                    s->byte_code.data[pos++] = REOP_save_reset;
                    s->byte_code.data[pos++] = last_capture_count;
                    s->byte_code.data[pos++] = s->capture_count - 1;
                }

                len = s->byte_code.size - last_atom_start;
                if (quant_min == 0) {
                    /* need to reset the capture in case the atom is
                       not executed */
                    if (!need_capture_init && last_capture_count != s->capture_count) {
                        if (re_buf_insert(&s->byte_code, last_atom_start, 3))
                            goto out_of_memory;
                        s->byte_code.data[last_atom_start++] = REOP_save_reset;
                        s->byte_code.data[last_atom_start++] = last_capture_count;
                        s->byte_code.data[last_atom_start++] = s->capture_count - 1;
                    }
                    if (quant_max == 0) {
                        s->byte_code.size = last_atom_start;
                    } else if (quant_max == 1 || quant_max == INT32_MAX) {
                        bool has_goto = (quant_max == INT32_MAX);
                        if (re_buf_insert(&s->byte_code, last_atom_start,
                                          5 + add_zero_advance_check * 2))
                            goto out_of_memory;
                        s->byte_code.data[last_atom_start] =
                            REOP_split_goto_first + greedy;
                        re_put_u32(s->byte_code.data + last_atom_start + 1,
                                   len + 5 * has_goto + add_zero_advance_check * 2 * 2);
                        if (add_zero_advance_check) {
                            s->byte_code.data[last_atom_start + 1 + 4] =
                                REOP_set_char_pos;
                            s->byte_code.data[last_atom_start + 1 + 4 + 1] = 0;
                            re_emit_op_u8(s, REOP_check_advance, 0);
                        }
                        if (has_goto)
                            re_emit_goto(s, REOP_goto, last_atom_start);
                    } else {
                        if (re_buf_insert(&s->byte_code, last_atom_start,
                                          11 + add_zero_advance_check * 2))
                            goto out_of_memory;
                        pos = last_atom_start;
                        s->byte_code.data[pos++] = REOP_split_goto_first + greedy;
                        re_put_u32(s->byte_code.data + pos,
                                   6 + add_zero_advance_check * 2 + len + 10);
                        pos += 4;

                        s->byte_code.data[pos++] = REOP_set_i32;
                        s->byte_code.data[pos++] = 0;
                        re_put_u32(s->byte_code.data + pos, quant_max);
                        pos += 4;
                        last_atom_start = pos;
                        if (add_zero_advance_check) {
                            s->byte_code.data[pos++] = REOP_set_char_pos;
                            s->byte_code.data[pos++] = 0;
                        }
                        re_emit_goto_u8_u32(s,
                                            (add_zero_advance_check
                                                 ? REOP_loop_check_adv_split_next_first
                                                 : REOP_loop_split_next_first) -
                                                greedy,
                                            0, quant_max, last_atom_start);
                    }
                } else if (quant_min == 1 && quant_max == INT32_MAX &&
                           !add_zero_advance_check) {
                    re_emit_goto(s, REOP_split_next_first - greedy, last_atom_start);
                } else {
                    if (quant_min == quant_max)
                        add_zero_advance_check = false;
                    if (re_buf_insert(&s->byte_code, last_atom_start,
                                      6 + add_zero_advance_check * 2))
                        goto out_of_memory;
                    /* Note: we assume the string length is < INT32_MAX */
                    pos = last_atom_start;
                    s->byte_code.data[pos++] = REOP_set_i32;
                    s->byte_code.data[pos++] = 0;
                    re_put_u32(s->byte_code.data + pos, quant_max);
                    pos += 4;
                    last_atom_start = pos;
                    if (add_zero_advance_check) {
                        s->byte_code.data[pos++] = REOP_set_char_pos;
                        s->byte_code.data[pos++] = 0;
                    }
                    if (quant_min == quant_max) {
                        /* a simple loop is enough */
                        re_emit_goto_u8(s, REOP_loop, 0, last_atom_start);
                    } else {
                        re_emit_goto_u8_u32(s,
                                            (add_zero_advance_check
                                                 ? REOP_loop_check_adv_split_next_first
                                                 : REOP_loop_split_next_first) -
                                                greedy,
                                            0, quant_max - quant_min, last_atom_start);
                    }
                }
                last_atom_start = -1;
            }
            break;
        default:
            break;
        }
    }
    s->buf_ptr = p;
    return 0;
out_of_memory:
    return re_parse_out_of_memory(s);
}

static i32 re_parse_alternative(REParseState* s, bool is_backward_dir) {
    const u8* p;
    i32 ret;
    usize start, term_start, end, term_size;

    start = s->byte_code.size;
    for (;;) {
        p = s->buf_ptr;
        if (p >= s->buf_end)
            break;
        if (*p == '|' || *p == ')')
            break;
        term_start = s->byte_code.size;
        ret = re_parse_term(s, is_backward_dir);
        if (ret)
            return ret;
        if (is_backward_dir) {
            /* reverse the order of the terms (XXX: inefficient, but
               speed is not really critical here) */
            end = s->byte_code.size;
            term_size = end - term_start;
            if (re_buf_reserve(&s->byte_code, term_size))
                return re_parse_out_of_memory(s);
            memmove(s->byte_code.data + start + term_size, s->byte_code.data + start,
                    end - start);
            memcpy(s->byte_code.data + start, s->byte_code.data + end, term_size);
        }
    }
    return 0;
}

static i32 re_parse_disjunction(REParseState* s, bool is_backward_dir) {
    i32 start, len, pos;

    if (re_check_stack_overflow(s->opaque, 0))
        return re_parse_error(s, (u8 const*)"stack overflow");

    start = s->byte_code.size;
    if (re_parse_alternative(s, is_backward_dir))
        return -1;
    while (*s->buf_ptr == '|') {
        s->buf_ptr++;

        len = s->byte_code.size - start;

        /* insert a split before the first alternative */
        if (re_buf_insert(&s->byte_code, start, 5)) {
            return re_parse_out_of_memory(s);
        }
        s->byte_code.data[start] = REOP_split_next_first;
        re_put_u32(s->byte_code.data + start + 1, len + 5);

        pos = re_emit_op_u32(s, REOP_goto, 0);

        if (re_has_named_captures(s) && s->group_name_scope == GROUP_NAME_SCOPE_MAX)
            return re_parse_error(s, (u8 const*)"too many named groups");

        s->group_name_scope++;

        if (re_parse_alternative(s, is_backward_dir))
            return -1;

        /* patch the goto ('pos' is only valid if the placeholder for the
           offset could be emitted) */
        if (cvm_buf_error(&s->byte_code))
            return re_parse_out_of_memory(s);
        len = s->byte_code.size - (pos + 4);
        re_put_u32(s->byte_code.data + pos, len);
    }
    return 0;
}

/* Allocate the registers as a stack. The control flow is recursive so
   the analysis can be linear. */
static i32 compute_register_count(u8* bc_buf, i32 bc_buf_len) {
    i32 stack_size, stack_size_max, pos, opcode, len;
    u32 val;

    stack_size = 0;
    stack_size_max = 0;
    bc_buf += RE_HEADER_LEN;
    bc_buf_len -= RE_HEADER_LEN;
    pos = 0;
    while (pos < bc_buf_len) {
        opcode = bc_buf[pos];
        len = reopcode_info[opcode].size;
        cvm_assert(opcode < REOP_COUNT);
        cvm_assert((pos + len) <= bc_buf_len);
        switch (opcode) {
        case REOP_set_i32:
        case REOP_set_char_pos:
            bc_buf[pos + 1] = stack_size;
            stack_size++;
            if (stack_size > stack_size_max) {
                if (stack_size > REGISTER_COUNT_MAX)
                    return -1;
                stack_size_max = stack_size;
            }
            break;
        case REOP_check_advance:
        case REOP_loop:
        case REOP_loop_split_goto_first:
        case REOP_loop_split_next_first:
            cvm_assert(stack_size > 0);
            stack_size--;
            bc_buf[pos + 1] = stack_size;
            break;
        case REOP_loop_check_adv_split_goto_first:
        case REOP_loop_check_adv_split_next_first:
            cvm_assert(stack_size >= 2);
            stack_size -= 2;
            bc_buf[pos + 1] = stack_size;
            break;
        case REOP_range:
        case REOP_range_i:
            val = re_get_u16(bc_buf + pos + 1);
            len += val * 4;
            break;
        case REOP_range32:
        case REOP_range32_i:
            val = re_get_u16(bc_buf + pos + 1);
            len += val * 8;
            break;
        case REOP_back_reference:
        case REOP_back_reference_i:
        case REOP_backward_back_reference:
        case REOP_backward_back_reference_i:
            val = bc_buf[pos + 1];
            len += val;
            break;
        }
        pos += len;
    }
    return stack_size_max;
}

/*
 * The bytecode cannot be larger than 2G, and the offsets inside it are 32-bit, so
 * upstream hands the buffer a realloc that refuses to grow past it. Ours is the
 * same refusal, expressed as the allocator our CvmBuf takes.
 */
static bool re_bytecode_fits(usize bytes) {
    return bytes <= (usize)0x7FFFFFFFu;
}

static opaque_mut re_bytecode_alloc(opaque_mut ctx, usize bytes) {
    return re_bytecode_fits(bytes) ? re_alloc(ctx, NULL, bytes) : NULL;
}

static opaque_mut re_bytecode_realloc(opaque_mut ctx, opaque_mut p, usize bytes) {
    return re_bytecode_fits(bytes) ? re_alloc(ctx, p, bytes) : NULL;
}

static void re_bytecode_free(opaque_mut ctx, opaque_mut p) {
    re_alloc(ctx, p, 0);
}

static CvmAlloc re_bytecode_allocator(opaque_mut ctx) {
    CvmAlloc a;
    a.ctx = ctx;
    a.alloc = re_bytecode_alloc;
    a.realloc = re_bytecode_realloc;
    a.free = re_bytecode_free;
    return a;
}

/* 'buf' must be a zero terminated UTF-8 string of length buf_len.
   Return NULL if error and allocate an error message in *perror_msg,
   otherwise the compiled bytecode and its length in plen.
*/
u8* cvm_lre_compile(i32* plen, u8* error_msg, i32 error_msg_size, u8 const* buf,
                    usize buf_len, i32 re_flags, void* opaque) {
    REParseState s_s, *s = &s_s;
    i32 register_count;
    bool is_sticky;

    memset(s, 0, sizeof(*s));
    s->opaque = opaque;
    s->buf_ptr = (const u8*)buf;
    s->buf_end = s->buf_ptr + buf_len;
    s->buf_start = s->buf_ptr;
    s->re_flags = re_flags;
    s->is_unicode =
        ((re_flags & (CVM_RE_FLAG_UNICODE | CVM_RE_FLAG_UNICODE_SETS)) != 0);
    is_sticky = ((re_flags & CVM_RE_FLAG_STICKY) != 0);
    s->ignore_case = ((re_flags & CVM_RE_FLAG_IGNORECASE) != 0);
    s->multi_line = ((re_flags & CVM_RE_FLAG_MULTILINE) != 0);
    s->dotall = ((re_flags & CVM_RE_FLAG_DOTALL) != 0);
    s->unicode_sets = ((re_flags & CVM_RE_FLAG_UNICODE_SETS) != 0);
    s->capture_count = 1;
    s->total_capture_count = -1;
    s->has_named_captures = -1;

    cvm_buf_init(&s->byte_code, re_bytecode_allocator(opaque));
    cvm_buf_init(&s->group_names, ((CvmRegexCtx*)(opaque))->alloc);

    re_buf_put_u16(&s->byte_code, re_flags); /* first element is the flags */
    re_putc(&s->byte_code, 0);        /* second element is the number of captures */
    re_putc(&s->byte_code, 0);        /* stack size */
    re_buf_put_u32(&s->byte_code, 0); /* bytecode length */

    if (!is_sticky) {
        /* iterate thru all positions (about the same as .*?( ... ) )
           .  We do it without an explicit loop so that lock step
           thread execution will be possible in an optimized
           implementation */
        re_emit_op_u32(s, REOP_split_goto_first, 1 + 5);
        re_emit_op(s, REOP_any);
        re_emit_op_u32(s, REOP_goto, -(5 + 1 + 5));
    }
    re_emit_op_u8(s, REOP_save_start, 0);

    if (re_parse_disjunction(s, false)) {
error:
        cvm_buf_free(&s->byte_code);
        cvm_buf_free(&s->group_names);
        re_pstrcpy(error_msg, error_msg_size, s->u.error_msg);
        *plen = 0;
        return NULL;
    }

    re_emit_op_u8(s, REOP_save_end, 0);

    re_emit_op(s, REOP_match);

    if (*s->buf_ptr != '\0') {
        re_parse_error(s, (u8 const*)"extraneous characters at the end");
        goto error;
    }

    if (cvm_buf_error(&s->byte_code)) {
        re_parse_out_of_memory(s);
        goto error;
    }

    register_count = compute_register_count(s->byte_code.data, s->byte_code.size);
    if (register_count < 0) {
        re_parse_error(s, (u8 const*)"too many imbricated quantifiers");
        goto error;
    }

    s->byte_code.data[RE_HEADER_CAPTURE_COUNT] = s->capture_count;
    s->byte_code.data[RE_HEADER_REGISTER_COUNT] = register_count;
    re_put_u32(s->byte_code.data + RE_HEADER_BYTECODE_LEN,
               s->byte_code.size - RE_HEADER_LEN);

    /* add the named groups if needed */
    if (s->group_names.size >
        (usize)((s->capture_count - 1) * CVM_RE_GROUP_NAME_TRAILER_LEN)) {
        if (re_put(&s->byte_code, s->group_names.data, s->group_names.size)) {
            re_parse_out_of_memory(s);
            goto error;
        }
        re_put_u16(s->byte_code.data + RE_HEADER_FLAGS,
                   cvm_lre_get_flags(s->byte_code.data) | CVM_RE_FLAG_NAMED_GROUPS);
    }
    cvm_buf_free(&s->group_names);

#ifdef DUMP_REOP
    lre_dump_bytecode(s->byte_code.data, s->byte_code.size);
#endif

    error_msg[0] = '\0';
    *plen = s->byte_code.size;
    return s->byte_code.data;
}

static bool is_line_terminator(u32 c) {
    return (c == '\n' || c == '\r' || c == CP_LS || c == CP_PS);
}

#define GET_CHAR(c, cptr, cbuf_end, cbuf_type)                                         \
    do {                                                                               \
        if (cbuf_type == 0) {                                                          \
            c = *cptr++;                                                               \
        } else {                                                                       \
            const u16* _p = (const u16*)cptr;                                          \
            const u16* _end = (const u16*)cbuf_end;                                    \
            c = *_p++;                                                                 \
            if (re_is_hi_surrogate(c) && cbuf_type == 2) {                             \
                if (_p < _end && re_is_lo_surrogate(*_p)) {                            \
                    c = re_from_surrogate(c, *_p++);                                   \
                }                                                                      \
            }                                                                          \
            cptr = (const void*)_p;                                                    \
        }                                                                              \
    } while (0)

#define PEEK_CHAR(c, cptr, cbuf_end, cbuf_type)                                        \
    do {                                                                               \
        if (cbuf_type == 0) {                                                          \
            c = cptr[0];                                                               \
        } else {                                                                       \
            const u16* _p = (const u16*)cptr;                                          \
            const u16* _end = (const u16*)cbuf_end;                                    \
            c = *_p++;                                                                 \
            if (re_is_hi_surrogate(c) && cbuf_type == 2) {                             \
                if (_p < _end && re_is_lo_surrogate(*_p)) {                            \
                    c = re_from_surrogate(c, *_p);                                     \
                }                                                                      \
            }                                                                          \
        }                                                                              \
    } while (0)

#define PEEK_PREV_CHAR(c, cptr, cbuf_start, cbuf_type)                                 \
    do {                                                                               \
        if (cbuf_type == 0) {                                                          \
            c = cptr[-1];                                                              \
        } else {                                                                       \
            const u16* _p = (const u16*)cptr - 1;                                      \
            const u16* _start = (const u16*)cbuf_start;                                \
            c = *_p;                                                                   \
            if (re_is_lo_surrogate(c) && cbuf_type == 2) {                             \
                if (_p > _start && re_is_hi_surrogate(_p[-1])) {                       \
                    c = re_from_surrogate(*--_p, c);                                   \
                }                                                                      \
            }                                                                          \
        }                                                                              \
    } while (0)

#define GET_PREV_CHAR(c, cptr, cbuf_start, cbuf_type)                                  \
    do {                                                                               \
        if (cbuf_type == 0) {                                                          \
            cptr--;                                                                    \
            c = cptr[0];                                                               \
        } else {                                                                       \
            const u16* _p = (const u16*)cptr - 1;                                      \
            const u16* _start = (const u16*)cbuf_start;                                \
            c = *_p;                                                                   \
            if (re_is_lo_surrogate(c) && cbuf_type == 2) {                             \
                if (_p > _start && re_is_hi_surrogate(_p[-1])) {                       \
                    c = re_from_surrogate(*--_p, c);                                   \
                }                                                                      \
            }                                                                          \
            cptr = (const void*)_p;                                                    \
        }                                                                              \
    } while (0)

#define PREV_CHAR(cptr, cbuf_start, cbuf_type)                                         \
    do {                                                                               \
        if (cbuf_type == 0) {                                                          \
            cptr--;                                                                    \
        } else {                                                                       \
            const u16* _p = (const u16*)cptr - 1;                                      \
            const u16* _start = (const u16*)cbuf_start;                                \
            if (re_is_lo_surrogate(*_p) && cbuf_type == 2) {                           \
                if (_p > _start && re_is_hi_surrogate(_p[-1])) {                       \
                    --_p;                                                              \
                }                                                                      \
            }                                                                          \
            cptr = (const void*)_p;                                                    \
        }                                                                              \
    } while (0)

typedef enum {
    RE_EXEC_STATE_SPLIT,
    RE_EXEC_STATE_LOOKAHEAD,
    RE_EXEC_STATE_NEGATIVE_LOOKAHEAD,
} REExecStateEnum;

#if INTPTR_MAX >= INT64_MAX
#define BP_TYPE_BITS 3
#else
#define BP_TYPE_BITS 2
#endif

typedef union {
    u8* ptr;
    intptr_t val; /* for bp, the low BP_SHIFT bits store REExecStateEnum */
    struct {
        usize val : sizeof(usize) * 8 - BP_TYPE_BITS;
        usize type : BP_TYPE_BITS;
    } bp;
} StackElem;

typedef struct {
    const u8* cbuf;
    const u8* cbuf_end;
    /* 0 = 8 bit chars, 1 = 16 bit chars, 2 = 16 bit chars, UTF-16 */
    i32 cbuf_type;
    i32 capture_count;
    bool is_unicode;
    i32 interrupt_counter;
    void* opaque; /* used for stack overflow check */

    StackElem* stack_buf;
    usize stack_size;
    StackElem static_stack_buf[32]; /* static stack to avoid allocation in most cases */
} REExecContext;

static i32 lre_poll_timeout(REExecContext* s) {
    if (CVM_UNLIKELY(--s->interrupt_counter <= 0)) {
        s->interrupt_counter = INTERRUPT_COUNTER_INIT;
        if (re_check_timeout(s->opaque))
            return CVM_RE_RET_TIMEOUT;
    }
    return 0;
}

static CVM_NOINLINE i32 stack_realloc(REExecContext* s, usize n) {
    StackElem* new_stack;
    usize new_size;
    new_size = s->stack_size * 3 / 2;
    if (new_size < n)
        new_size = n;
    if (s->stack_buf == s->static_stack_buf) {
        new_stack = re_alloc(s->opaque, NULL, new_size * sizeof(StackElem));
        if (!new_stack)
            return -1;
        /* XXX: could use correct size */
        memcpy(new_stack, s->stack_buf, s->stack_size * sizeof(StackElem));
    } else {
        new_stack = re_alloc(s->opaque, s->stack_buf, new_size * sizeof(StackElem));
        if (!new_stack)
            return -1;
    }
    s->stack_size = new_size;
    s->stack_buf = new_stack;
    return 0;
}

/* return 1 if match, 0 if not match or < 0 if error. */
static intptr_t lre_exec_backtrack(REExecContext* s, u8** capture, const u8* pc,
                                   const u8* cptr) {
    i32 opcode;
    i32 cbuf_type;
    u32 val, c, idx;
    const u8* cbuf_end;
    StackElem *sp, *bp, *stack_end;
#ifdef DUMP_EXEC
    const u8* pc_start = pc; /* TEST */
#endif
    cbuf_type = s->cbuf_type;
    cbuf_end = s->cbuf_end;

    sp = s->stack_buf;
    bp = s->stack_buf;
    stack_end = s->stack_buf + s->stack_size;

#define CHECK_STACK_SPACE(n)                                                           \
    if (CVM_UNLIKELY((stack_end - sp) < (n))) {                                        \
        usize saved_sp = sp - s->stack_buf;                                            \
        usize saved_bp = bp - s->stack_buf;                                            \
        if (stack_realloc(s, sp - s->stack_buf + (n)))                                 \
            return CVM_RE_RET_MEMORY_ERROR;                                            \
        stack_end = s->stack_buf + s->stack_size;                                      \
        sp = s->stack_buf + saved_sp;                                                  \
        bp = s->stack_buf + saved_bp;                                                  \
    }

    /* XXX: could test if the value was saved to reduce the stack size
       but slower */
#define SAVE_CAPTURE(idx, value)                                                       \
    {                                                                                  \
        CHECK_STACK_SPACE(2);                                                          \
        sp[0].val = idx;                                                               \
        sp[1].ptr = capture[idx];                                                      \
        sp += 2;                                                                       \
        capture[idx] = (value);                                                        \
    }

    /* avoid saving the previous value if already saved */
#define SAVE_CAPTURE_CHECK(idx, value)                                                 \
    {                                                                                  \
        StackElem* sp1;                                                                \
        sp1 = sp;                                                                      \
        for (;;) {                                                                     \
            if (sp1 > bp) {                                                            \
                if (sp1[-2].val == idx)                                                \
                    break;                                                             \
                sp1 -= 2;                                                              \
            } else {                                                                   \
                CHECK_STACK_SPACE(2);                                                  \
                sp[0].val = idx;                                                       \
                sp[1].ptr = capture[idx];                                              \
                sp += 2;                                                               \
                break;                                                                 \
            }                                                                          \
        }                                                                              \
        capture[idx] = (value);                                                        \
    }

#ifdef DUMP_EXEC
    printf((u8 const*)"%5s %5s %5s %5s %s\n", (u8 const*)"PC", (u8 const*)"CP",
           (u8 const*)"BP", (u8 const*)"SP", (u8 const*)"OPCODE");
#endif
    for (;;) {
        opcode = *pc++;
#ifdef DUMP_EXEC
        printf((u8 const*)"%5ld %5ld %5ld %5ld %s\n", pc - 1 - pc_start,
               cbuf_type == 0 ? cptr - s->cbuf : (cptr - s->cbuf) / 2,
               bp - s->stack_buf, sp - s->stack_buf, reopcode_info[opcode].name);
#endif
        switch (opcode) {
        case REOP_match:
            return 1;
no_match:
            for (;;) {
                REExecStateEnum type;
                if (bp == s->stack_buf)
                    return 0;
                /* undo the modifications to capture[] */
                while (sp > bp) {
                    capture[sp[-2].val] = sp[-1].ptr;
                    sp -= 2;
                }

                pc = sp[-3].ptr;
                cptr = sp[-2].ptr;
                type = sp[-1].bp.type;
                bp = s->stack_buf + sp[-1].bp.val;
                sp -= 3;
                if (type != RE_EXEC_STATE_LOOKAHEAD)
                    break;
            }
            if (lre_poll_timeout(s))
                return CVM_RE_RET_TIMEOUT;
            break;
        case REOP_lookahead_match:
            /* pop all the saved states until reaching the start of
               the lookahead and keep the updated captures and
               variables and the corresponding undo info. */
            {
                StackElem *sp1, *sp_top, *next_sp;
                REExecStateEnum type;

                sp_top = sp;
                for (;;) {
                    sp1 = sp;
                    sp = bp;
                    pc = sp[-3].ptr;
                    cptr = sp[-2].ptr;
                    type = sp[-1].bp.type;
                    bp = s->stack_buf + sp[-1].bp.val;
                    sp[-1].ptr = (void*)sp1; /* save the next value for the copy step */
                    sp -= 3;
                    if (type == RE_EXEC_STATE_LOOKAHEAD)
                        break;
                }
                if (sp != s->stack_buf) {
                    /* keep the undo info if there is a saved state */
                    sp1 = sp;
                    while (sp1 < sp_top) {
                        next_sp = (void*)sp1[2].ptr;
                        sp1 += 3;
                        while (sp1 < next_sp)
                            *sp++ = *sp1++;
                    }
                }
            }
            break;
        case REOP_negative_lookahead_match:
            /* pop all the saved states until reaching start of the negative lookahead */
            for (;;) {
                REExecStateEnum type;
                type = bp[-1].bp.type;
                /* undo the modifications to capture[] */
                while (sp > bp) {
                    capture[sp[-2].val] = sp[-1].ptr;
                    sp -= 2;
                }
                pc = sp[-3].ptr;
                cptr = sp[-2].ptr;
                type = sp[-1].bp.type;
                bp = s->stack_buf + sp[-1].bp.val;
                sp -= 3;
                if (type == RE_EXEC_STATE_NEGATIVE_LOOKAHEAD)
                    break;
            }
            goto no_match;
        case REOP_char32:
        case REOP_char32_i:
            val = re_get_u32(pc);
            pc += 4;
            goto test_char;
        case REOP_char:
        case REOP_char_i:
            val = re_get_u16(pc);
            pc += 2;
test_char:
            if (cptr >= cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (opcode == REOP_char_i || opcode == REOP_char32_i) {
                c = cvm_unicode_canonicalize(c, s->is_unicode);
            }
            if (val != c)
                goto no_match;
            break;
        case REOP_split_goto_first:
        case REOP_split_next_first: {
            const u8* pc1;

            val = re_get_u32(pc);
            pc += 4;
            if (opcode == REOP_split_next_first) {
                pc1 = pc + (i32)val;
            } else {
                pc1 = pc;
                pc = pc + (i32)val;
            }
            CHECK_STACK_SPACE(3);
            sp[0].ptr = (u8*)pc1;
            sp[1].ptr = (u8*)cptr;
            sp[2].bp.val = bp - s->stack_buf;
            sp[2].bp.type = RE_EXEC_STATE_SPLIT;
            sp += 3;
            bp = sp;
        } break;
        case REOP_lookahead:
        case REOP_negative_lookahead:
            val = re_get_u32(pc);
            pc += 4;
            CHECK_STACK_SPACE(3);
            sp[0].ptr = (u8*)(pc + (i32)val);
            sp[1].ptr = (u8*)cptr;
            sp[2].bp.val = bp - s->stack_buf;
            sp[2].bp.type = RE_EXEC_STATE_LOOKAHEAD + opcode - REOP_lookahead;
            sp += 3;
            bp = sp;
            break;
        case REOP_goto:
            val = re_get_u32(pc);
            pc += 4 + (i32)val;
            if (lre_poll_timeout(s))
                return CVM_RE_RET_TIMEOUT;
            break;
        case REOP_line_start:
        case REOP_line_start_m:
            if (cptr == s->cbuf)
                break;
            if (opcode == REOP_line_start)
                goto no_match;
            PEEK_PREV_CHAR(c, cptr, s->cbuf, cbuf_type);
            if (!is_line_terminator(c))
                goto no_match;
            break;
        case REOP_line_end:
        case REOP_line_end_m:
            if (cptr == cbuf_end)
                break;
            if (opcode == REOP_line_end)
                goto no_match;
            PEEK_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (!is_line_terminator(c))
                goto no_match;
            break;
        case REOP_dot:
            if (cptr == cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (is_line_terminator(c))
                goto no_match;
            break;
        case REOP_any:
            if (cptr == cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            break;
        case REOP_space:
            if (cptr == cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (!re_is_space(c))
                goto no_match;
            break;
        case REOP_not_space:
            if (cptr == cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (re_is_space(c))
                goto no_match;
            break;
        case REOP_save_start:
        case REOP_save_end:
            val = *pc++;
            if (val >= (u32)s->capture_count)
                return CVM_RE_RET_BYTECODE_ERROR;
            idx = 2 * val + opcode - REOP_save_start;
            SAVE_CAPTURE(idx, (u8*)cptr);
            break;
        case REOP_save_reset: {
            u32 val2;
            val = pc[0];
            val2 = pc[1];
            pc += 2;
            if (val2 >= (u32)s->capture_count)
                return CVM_RE_RET_BYTECODE_ERROR;
            CHECK_STACK_SPACE(2 * (val2 - val + 1));
            while (val <= val2) {
                idx = 2 * val;
                SAVE_CAPTURE(idx, NULL);
                idx = 2 * val + 1;
                SAVE_CAPTURE(idx, NULL);
                val++;
            }
        } break;
        case REOP_set_i32:
            idx = 2 * s->capture_count + pc[0];
            val = re_get_u32(pc + 1);
            pc += 5;
            SAVE_CAPTURE_CHECK(idx, (void*)(usize)val);
            break;
        case REOP_loop: {
            u32 val2;
            idx = 2 * s->capture_count + pc[0];
            val = re_get_u32(pc + 1);
            pc += 5;

            val2 = (usize)capture[idx] - 1;
            SAVE_CAPTURE_CHECK(idx, (void*)(usize)val2);
            if (val2 != 0) {
                pc += (i32)val;
                if (lre_poll_timeout(s))
                    return CVM_RE_RET_TIMEOUT;
            }
        } break;
        case REOP_loop_split_goto_first:
        case REOP_loop_split_next_first:
        case REOP_loop_check_adv_split_goto_first:
        case REOP_loop_check_adv_split_next_first: {
            const u8* pc1;
            u32 val2, limit;
            idx = 2 * s->capture_count + pc[0];
            limit = re_get_u32(pc + 1);
            val = re_get_u32(pc + 5);
            pc += 9;

            /* decrement the counter */
            val2 = (usize)capture[idx] - 1;
            SAVE_CAPTURE_CHECK(idx, (void*)(usize)val2);

            if (val2 > limit) {
                /* normal loop if counter > limit */
                pc += (i32)val;
                if (lre_poll_timeout(s))
                    return CVM_RE_RET_TIMEOUT;
            } else {
                /* check advance */
                if ((opcode == REOP_loop_check_adv_split_goto_first ||
                     opcode == REOP_loop_check_adv_split_next_first) &&
                    capture[idx + 1] == cptr && val2 != limit) {
                    goto no_match;
                }

                /* otherwise conditional split */
                if (val2 != 0) {
                    if (opcode == REOP_loop_split_next_first ||
                        opcode == REOP_loop_check_adv_split_next_first) {
                        pc1 = pc + (i32)val;
                    } else {
                        pc1 = pc;
                        pc = pc + (i32)val;
                    }
                    CHECK_STACK_SPACE(3);
                    sp[0].ptr = (u8*)pc1;
                    sp[1].ptr = (u8*)cptr;
                    sp[2].bp.val = bp - s->stack_buf;
                    sp[2].bp.type = RE_EXEC_STATE_SPLIT;
                    sp += 3;
                    bp = sp;
                }
            }
        } break;
        case REOP_set_char_pos:
            idx = 2 * s->capture_count + pc[0];
            pc++;
            SAVE_CAPTURE_CHECK(idx, (u8*)cptr);
            break;
        case REOP_check_advance:
            idx = 2 * s->capture_count + pc[0];
            pc++;
            if (capture[idx] == cptr)
                goto no_match;
            break;
        case REOP_word_boundary:
        case REOP_word_boundary_i:
        case REOP_not_word_boundary:
        case REOP_not_word_boundary_i: {
            bool v1, v2;
            i32 ignore_case =
                (opcode == REOP_word_boundary_i || opcode == REOP_not_word_boundary_i);
            bool is_boundary =
                (opcode == REOP_word_boundary || opcode == REOP_word_boundary_i);
            /* char before */
            if (cptr == s->cbuf) {
                v1 = false;
            } else {
                PEEK_PREV_CHAR(c, cptr, s->cbuf, cbuf_type);
                if (c < 256) {
                    v1 = (re_is_word_byte(c) != 0);
                } else {
                    v1 = ignore_case && (c == 0x017f || c == 0x212a);
                }
            }
            /* current u8*/
            if (cptr >= cbuf_end) {
                v2 = false;
            } else {
                PEEK_CHAR(c, cptr, cbuf_end, cbuf_type);
                if (c < 256) {
                    v2 = (re_is_word_byte(c) != 0);
                } else {
                    v2 = ignore_case && (c == 0x017f || c == 0x212a);
                }
            }
            if (v1 ^ v2 ^ is_boundary)
                goto no_match;
        } break;
        case REOP_back_reference:
        case REOP_back_reference_i:
        case REOP_backward_back_reference:
        case REOP_backward_back_reference_i: {
            const u8 *cptr1, *cptr1_end, *cptr1_start;
            const u8* pc1;
            u32 c1, c2;
            i32 i, n;

            n = *pc++;
            pc1 = pc;
            pc += n;

            for (i = 0; i < n; i++) {
                val = pc1[i];
                if ((i32)val >= s->capture_count)
                    goto no_match;
                cptr1_start = capture[2 * val];
                cptr1_end = capture[2 * val + 1];
                /* test the first not empty capture */
                if (cptr1_start && cptr1_end) {
                    if (opcode == REOP_back_reference ||
                        opcode == REOP_back_reference_i) {
                        cptr1 = cptr1_start;
                        while (cptr1 < cptr1_end) {
                            if (cptr >= cbuf_end)
                                goto no_match;
                            GET_CHAR(c1, cptr1, cptr1_end, cbuf_type);
                            GET_CHAR(c2, cptr, cbuf_end, cbuf_type);
                            if (opcode == REOP_back_reference_i) {
                                c1 = cvm_unicode_canonicalize(c1, s->is_unicode);
                                c2 = cvm_unicode_canonicalize(c2, s->is_unicode);
                            }
                            if (c1 != c2)
                                goto no_match;
                        }
                    } else {
                        cptr1 = cptr1_end;
                        while (cptr1 > cptr1_start) {
                            if (cptr == s->cbuf)
                                goto no_match;
                            GET_PREV_CHAR(c1, cptr1, cptr1_start, cbuf_type);
                            GET_PREV_CHAR(c2, cptr, s->cbuf, cbuf_type);
                            if (opcode == REOP_backward_back_reference_i) {
                                c1 = cvm_unicode_canonicalize(c1, s->is_unicode);
                                c2 = cvm_unicode_canonicalize(c2, s->is_unicode);
                            }
                            if (c1 != c2)
                                goto no_match;
                        }
                    }
                    break;
                }
            }
        } break;
        case REOP_range:
        case REOP_range_i: {
            i32 n;
            u32 low, high, idx_min, idx_max, idx;

            n = re_get_u16(pc); /* n must be >= 1 */
            pc += 2;
            if (cptr >= cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (opcode == REOP_range_i) {
                c = cvm_unicode_canonicalize(c, s->is_unicode);
            }
            idx_min = 0;
            low = re_get_u16(pc + 0 * 4);
            if (c < low)
                goto no_match;
            idx_max = n - 1;
            high = re_get_u16(pc + idx_max * 4 + 2);
            /* 0xffff in for last value means +infinity */
            if (CVM_UNLIKELY(c >= 0xffff) && high == 0xffff)
                goto range_match;
            if (c > high)
                goto no_match;
            while (idx_min <= idx_max) {
                idx = (idx_min + idx_max) / 2;
                low = re_get_u16(pc + idx * 4);
                high = re_get_u16(pc + idx * 4 + 2);
                if (c < low)
                    idx_max = idx - 1;
                else if (c > high)
                    idx_min = idx + 1;
                else
                    goto range_match;
            }
            goto no_match;
range_match:
            pc += 4 * n;
        } break;
        case REOP_range32:
        case REOP_range32_i: {
            i32 n;
            u32 low, high, idx_min, idx_max, idx;

            n = re_get_u16(pc); /* n must be >= 1 */
            pc += 2;
            if (cptr >= cbuf_end)
                goto no_match;
            GET_CHAR(c, cptr, cbuf_end, cbuf_type);
            if (opcode == REOP_range32_i) {
                c = cvm_unicode_canonicalize(c, s->is_unicode);
            }
            idx_min = 0;
            low = re_get_u32(pc + 0 * 8);
            if (c < low)
                goto no_match;
            idx_max = n - 1;
            high = re_get_u32(pc + idx_max * 8 + 4);
            if (c > high)
                goto no_match;
            while (idx_min <= idx_max) {
                idx = (idx_min + idx_max) / 2;
                low = re_get_u32(pc + idx * 8);
                high = re_get_u32(pc + idx * 8 + 4);
                if (c < low)
                    idx_max = idx - 1;
                else if (c > high)
                    idx_min = idx + 1;
                else
                    goto range32_match;
            }
            goto no_match;
range32_match:
            pc += 8 * n;
        } break;
        case REOP_prev:
            /* go to the previous u8*/
            if (cptr == s->cbuf)
                goto no_match;
            PREV_CHAR(cptr, s->cbuf, cbuf_type);
            break;
        default:
#ifdef DUMP_EXEC
            printf((u8 const*)"unknown opcode pc=%ld\n", pc - 1 - pc_start);
#endif
            cvm_assert(0);
        }
    }
}

/* Return 1 if match, 0 if not match or < 0 if error (see CVM_RE_RET_x). cindex is the
   starting position of the match and must be such as 0 <= cindex <=
   clen. */
i32 cvm_lre_exec(u8** capture, const u8* bc_buf, const u8* cbuf, i32 cindex, i32 clen,
                 i32 cbuf_type, void* opaque) {
    REExecContext s_s, *s = &s_s;
    i32 re_flags, i, ret;
    const u8* cptr;

    re_flags = cvm_lre_get_flags(bc_buf);
    s->is_unicode = (re_flags & (CVM_RE_FLAG_UNICODE | CVM_RE_FLAG_UNICODE_SETS)) != 0;
    s->capture_count = bc_buf[RE_HEADER_CAPTURE_COUNT];
    s->cbuf = cbuf;
    s->cbuf_end = cbuf + (clen << cbuf_type);
    s->cbuf_type = cbuf_type;
    if (s->cbuf_type == 1 && s->is_unicode)
        s->cbuf_type = 2;
    s->interrupt_counter = INTERRUPT_COUNTER_INIT;
    s->opaque = opaque;

    s->stack_buf = s->static_stack_buf;
    s->stack_size = re_countof(s->static_stack_buf);

    for (i = 0; i < s->capture_count * 2; i++)
        capture[i] = NULL;

    cptr = cbuf + (cindex << cbuf_type);
    if (0 < cindex && cindex < clen && s->cbuf_type == 2) {
        const u16* p = (const u16*)cptr;
        if (re_is_lo_surrogate(*p) && re_is_hi_surrogate(p[-1])) {
            cptr = (const u8*)(p - 1);
        }
    }

    ret = lre_exec_backtrack(s, capture, bc_buf + RE_HEADER_LEN, cptr);

    if (s->stack_buf != s->static_stack_buf)
        re_alloc(s->opaque, s->stack_buf, 0);
    return ret;
}

/* Structurally validate serialized regexp bytecode (e.g. from JS_ReadObject)
   before it is executed: a present header and a body length that fits within
   the buffer. Returns 0 on success, -1 if the bytecode is malformed. */
/*
 * The capture buffer's size, not the number of groups: the executor keeps its
 * temporary registers in the tail of the same buffer, so a buffer sized by the
 * group count alone overflows on any pattern that uses one.
 */
i32 cvm_lre_get_alloc_count(const u8* bc_buf) {
    return (i32)bc_buf[RE_HEADER_CAPTURE_COUNT] * 2 +
           (i32)bc_buf[RE_HEADER_REGISTER_COUNT];
}

i32 cvm_lre_get_capture_count(const u8* bc_buf) {
    return bc_buf[RE_HEADER_CAPTURE_COUNT];
}

i32 cvm_lre_get_flags(const u8* bc_buf) {
    return re_get_u16(bc_buf + RE_HEADER_FLAGS);
}

/* Return NULL if no group names. Otherwise, return a pointer to
   'capture_count - 1' zero terminated UTF-8 strings. */
u8 const* cvm_lre_get_groupnames(const u8* bc_buf) {
    u32 re_bytecode_len;
    if ((cvm_lre_get_flags(bc_buf) & CVM_RE_FLAG_NAMED_GROUPS) == 0)
        return NULL;
    re_bytecode_len = re_get_u32(bc_buf + RE_HEADER_BYTECODE_LEN);
    return (u8 const*)(bc_buf + RE_HEADER_LEN + re_bytecode_len);
}
