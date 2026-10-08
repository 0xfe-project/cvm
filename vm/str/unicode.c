/*
 * Unicode utilities
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
 * vm/str/unicode.c
 *
 * Unicode case mapping and classification. Ported from the corresponding parts
 * of refs/quickjs-0.17.0/libunicode.c (Fabrice Bellard, MIT); the tables come
 * from libunicode-table.h, which carries the Unicode data licence.
 *
 * Here: case conversion (upper, lower, and the canonical form the regex engine
 * folds with), is_cased, is_case_ignorable, is_white_space, is_space,
 * is_id_start, is_id_continue, and the ASCII classification table the byte fast
 * paths use.
 *
 * Not here, and deliberately not half here: normalization and the character
 * range machinery. Both need a growable buffer and an allocation contract for
 * it, and the ranges are what the regex engine consumes — so they are one piece
 * of work with the regex port. See cvm/unicode.h.
 */

#include "cvm/unicode.h"

#include "base/assert.h"
#include "base/types.h"
#include "cvm/builder.h"
#include "cvm/str.h"
#include "mem.h"
#include "rt.h"

/* The table file is generated and asks for this before anything else here has a
 * chance to define it. */
#define countof(x) (sizeof(x) / sizeof((x)[0]))

#include "str/unicode-table.h"

/* ------------------------------------------------------------------ */
/* the ported code                                                     */
/* ------------------------------------------------------------------ */

// note: stored as 4 bit tag, not much room left
enum {
    RUN_TYPE_U,
    RUN_TYPE_L,
    RUN_TYPE_UF,
    RUN_TYPE_LF,
    RUN_TYPE_UL,
    RUN_TYPE_LSU,
    RUN_TYPE_U2L_399_EXT2,
    RUN_TYPE_UF_D20,
    RUN_TYPE_UF_D1_EXT,
    RUN_TYPE_U_EXT,
    RUN_TYPE_LF_EXT,
    RUN_TYPE_UF_EXT2,
    RUN_TYPE_LF_EXT2,
    RUN_TYPE_UF_EXT3,
};

static i32 case_conv1(u32 c, i32 conv_type) {
    u32 res[CVM_UNICODE_CASE_MAX];
    cvm_unicode_case_conv(res, c, conv_type);
    return res[0];
}

/* case conversion using the table entry 'idx' with value 'v' */
static i32 case_conv_entry(u32* res, u32 c, i32 conv_type, u32 idx, u32 v) {
    u32 code, data, type, a, is_lower;
    is_lower = (conv_type != 0);
    type = (v >> (32 - 17 - 7 - 4)) & 0xf;
    data = ((v & 0xf) << 8) | case_conv_table2[idx];
    code = v >> (32 - 17);
    switch (type) {
    case RUN_TYPE_U:
    case RUN_TYPE_L:
    case RUN_TYPE_UF:
    case RUN_TYPE_LF:
        if (conv_type == (type & 1) || (type >= RUN_TYPE_UF && conv_type == 2)) {
            c = c - code + (case_conv_table1[data] >> (32 - 17));
        }
        break;
    case RUN_TYPE_UL:
        a = c - code;
        if ((a & 1) != (1 - is_lower))
            break;
        c = (a ^ 1) + code;
        break;
    case RUN_TYPE_LSU:
        a = c - code;
        if (a == 1) {
            c += 2 * is_lower - 1;
        } else if (a == (1 - is_lower) * 2) {
            c += (2 * is_lower - 1) * 2;
        }
        break;
    case RUN_TYPE_U2L_399_EXT2:
        if (!is_lower) {
            res[0] = c - code + case_conv_ext[data >> 6];
            res[1] = 0x399;
            return 2;
        } else {
            c = c - code + case_conv_ext[data & 0x3f];
        }
        break;
    case RUN_TYPE_UF_D20:
        if (conv_type == 1)
            break;
        c = data + (conv_type == 2) * 0x20;
        break;
    case RUN_TYPE_UF_D1_EXT:
        if (conv_type == 1)
            break;
        c = case_conv_ext[data] + (conv_type == 2);
        break;
    case RUN_TYPE_U_EXT:
    case RUN_TYPE_LF_EXT:
        if (is_lower != (type - RUN_TYPE_U_EXT))
            break;
        c = case_conv_ext[data];
        break;
    case RUN_TYPE_LF_EXT2:
        if (!is_lower)
            break;
        res[0] = c - code + case_conv_ext[data >> 6];
        res[1] = case_conv_ext[data & 0x3f];
        return 2;
    case RUN_TYPE_UF_EXT2:
        if (conv_type == 1)
            break;
        res[0] = c - code + case_conv_ext[data >> 6];
        res[1] = case_conv_ext[data & 0x3f];
        if (conv_type == 2) {
            /* convert to lower */
            res[0] = case_conv1(res[0], 1);
            res[1] = case_conv1(res[1], 1);
        }
        return 2;
    default:
    case RUN_TYPE_UF_EXT3:
        if (conv_type == 1)
            break;
        res[0] = case_conv_ext[data >> 8];
        res[1] = case_conv_ext[(data >> 4) & 0xf];
        res[2] = case_conv_ext[data & 0xf];
        if (conv_type == 2) {
            /* convert to lower */
            res[0] = case_conv1(res[0], 1);
            res[1] = case_conv1(res[1], 1);
            res[2] = case_conv1(res[2], 1);
        }
        return 3;
    }
    res[0] = c;
    return 1;
}

/* conv_type:
   0 = to upper
   1 = to lower
   2 = case folding (= to lower with modifications)
*/
u32 cvm_unicode_case_conv(rune out[CVM_UNICODE_CASE_MAX], rune c,
                          CvmUnicodeCase which) {
    i32 conv_type = (i32)which;
    if (c < 128) {
        if (conv_type) {
            if (c >= 'A' && c <= 'Z') {
                c = c - 'A' + 'a';
            }
        } else {
            if (c >= 'a' && c <= 'z') {
                c = c - 'a' + 'A';
            }
        }
    } else {
        u32 v, code, len;
        i32 idx, idx_min, idx_max;

        idx_min = 0;
        idx_max = countof(case_conv_table1) - 1;
        while (idx_min <= idx_max) {
            idx = (u32)(idx_max + idx_min) / 2;
            v = case_conv_table1[idx];
            code = v >> (32 - 17);
            len = (v >> (32 - 17 - 7)) & 0x7f;
            if (c < code) {
                idx_max = idx - 1;
            } else if (c >= code + len) {
                idx_min = idx + 1;
            } else {
                return case_conv_entry(out, c, conv_type, idx, v);
            }
        }
    }
    out[0] = c;
    return 1;
}

static i32 case_folding_entry(u32 c, u32 idx, u32 v, bool is_unicode) {
    u32 res[CVM_UNICODE_CASE_MAX];
    i32 len;

    if (is_unicode) {
        len = case_conv_entry(res, c, 2, idx, v);
        if (len == 1) {
            c = res[0];
        } else {
            /* handle the few specific multi-character cases (see
               unicode_gen.c:dump_case_folding_special_cases()) */
            if (c == 0xfb06) {
                c = 0xfb05;
            } else if (c == 0x01fd3) {
                c = 0x390;
            } else if (c == 0x01fe3) {
                c = 0x3b0;
            }
        }
    } else {
        if (CVM_LIKELY(c < 128)) {
            if (c >= 'a' && c <= 'z')
                c = c - 'a' + 'A';
        } else {
            /* legacy regexp: to upper case if single u8 >= 128 */
            len = case_conv_entry(res, c, false, idx, v);
            if (len == 1 && res[0] >= 128)
                c = res[0];
        }
    }
    return c;
}

/* JS regexp specific rules for case folding */
rune cvm_unicode_canonicalize(rune c, bool full) {
    if (c < 128) {
        /* fast case */
        if (full) {
            if (c >= 'A' && c <= 'Z') {
                c = c - 'A' + 'a';
            }
        } else {
            if (c >= 'a' && c <= 'z') {
                c = c - 'a' + 'A';
            }
        }
    } else {
        u32 v, code, len;
        i32 idx, idx_min, idx_max;

        idx_min = 0;
        idx_max = countof(case_conv_table1) - 1;
        while (idx_min <= idx_max) {
            idx = (u32)(idx_max + idx_min) / 2;
            v = case_conv_table1[idx];
            code = v >> (32 - 17);
            len = (v >> (32 - 17 - 7)) & 0x7f;
            if (c < code) {
                idx_max = idx - 1;
            } else if (c >= code + len) {
                idx_min = idx + 1;
            } else {
                return case_folding_entry(c, idx, v, full);
            }
        }
    }
    return c;
}

static u32 get_le24(const u8* ptr) {
    return ptr[0] | (ptr[1] << 8) | (ptr[2] << 16);
}

#define UNICODE_INDEX_BLOCK_LEN 32

/* return -1 if not in table, otherwise the offset in the block */
static i32 get_index_pos(u32* pcode, u32 c, const u8* index_table,
                         i32 index_table_len) {
    u32 code, v;
    i32 idx_min, idx_max, idx;

    idx_min = 0;
    v = get_le24(index_table);
    code = v & ((1 << 21) - 1);
    if (c < code) {
        *pcode = 0;
        return 0;
    }
    idx_max = index_table_len - 1;
    code = get_le24(index_table + idx_max * 3);
    if (c >= code)
        return -1;
    /* invariant: tab[idx_min] <= c < tab2[idx_max] */
    while ((idx_max - idx_min) > 1) {
        idx = (idx_max + idx_min) / 2;
        v = get_le24(index_table + idx * 3);
        code = v & ((1 << 21) - 1);
        if (c < code) {
            idx_max = idx;
        } else {
            idx_min = idx;
        }
    }
    v = get_le24(index_table + idx_min * 3);
    *pcode = v & ((1 << 21) - 1);
    return (idx_min + 1) * UNICODE_INDEX_BLOCK_LEN + (v >> 21);
}

static bool is_in_table(u32 c, const u8* table, const u8* index_table,
                        i32 index_table_len) {
    u32 code, b, bit;
    i32 pos;
    const u8* p;

    pos = get_index_pos(&code, c, index_table, index_table_len);
    if (pos < 0)
        return false; /* outside the table */
    p = table + pos;
    bit = 0;
    for (;;) {
        b = *p++;
        if (b < 64) {
            code += (b >> 3) + 1;
            if (c < code)
                return bit;
            bit ^= 1;
            code += (b & 7) + 1;
        } else if (b >= 0x80) {
            code += b - 0x80 + 1;
        } else if (b < 0x60) {
            code += (((b - 0x40) << 8) | p[0]) + 1;
            p++;
        } else {
            code += (((b - 0x60) << 16) | (p[0] << 8) | p[1]) + 1;
            p += 2;
        }
        if (c < code)
            return bit;
        bit ^= 1;
    }
}

bool cvm_unicode_is_cased(u32 c) {
    u32 v, code, len;
    i32 idx, idx_min, idx_max;

    idx_min = 0;
    idx_max = countof(case_conv_table1) - 1;
    while (idx_min <= idx_max) {
        idx = (u32)(idx_max + idx_min) / 2;
        v = case_conv_table1[idx];
        code = v >> (32 - 17);
        len = (v >> (32 - 17 - 7)) & 0x7f;
        if (c < code) {
            idx_max = idx - 1;
        } else if (c >= code + len) {
            idx_min = idx + 1;
        } else {
            return true;
        }
    }
    return is_in_table(c, unicode_prop_Cased1_table, unicode_prop_Cased1_index,
                       sizeof(unicode_prop_Cased1_index) / 3);
}

bool cvm_unicode_is_case_ignorable(u32 c) {
    return is_in_table(c, unicode_prop_Case_Ignorable_table,
                       unicode_prop_Case_Ignorable_index,
                       sizeof(unicode_prop_Case_Ignorable_index) / 3);
}

bool cvm_unicode_is_id_start(u32 c) {
    return is_in_table(c, unicode_prop_ID_Start_table, unicode_prop_ID_Start_index,
                       sizeof(unicode_prop_ID_Start_index) / 3);
}

bool cvm_unicode_is_id_continue(u32 c) {
    return cvm_unicode_is_id_start(c) ||
           is_in_table(c, unicode_prop_ID_Continue1_table,
                       unicode_prop_ID_Continue1_index,
                       sizeof(unicode_prop_ID_Continue1_index) / 3);
}

bool cvm_unicode_is_white_space(u32 c) {
    return is_in_table(c, unicode_prop_White_Space_table,
                       unicode_prop_White_Space_index,
                       sizeof(unicode_prop_White_Space_index) / 3);
}

/*---- lre codepoint categorizing functions ----*/

#define S CVM_UNICODE_C_SPACE
#define D CVM_UNICODE_C_DIGIT
#define X CVM_UNICODE_C_XDIGIT
#define U CVM_UNICODE_C_UPPER
#define L CVM_UNICODE_C_LOWER
#define _ CVM_UNICODE_C_UNDER
#define d CVM_UNICODE_C_DOLLAR

u8 const cvm_unicode_ctype_bits[256] = {
    0,     0,     0,     0,     0,     0,     0,     0,     0,     S,     S,
    S,     S,     S,     0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,     0,     0,     0,

    S,     0,     0,     0,     d,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0,     X | D, X | D, X | D, X | D, X | D, X | D,
    X | D, X | D, X | D, X | D, 0,     0,     0,     0,     0,     0,

    0,     X | U, X | U, X | U, X | U, X | U, X | U, U,     U,     U,     U,
    U,     U,     U,     U,     U,     U,     U,     U,     U,     U,     U,
    U,     U,     U,     U,     U,     0,     0,     0,     0,     _,

    0,     X | L, X | L, X | L, X | L, X | L, X | L, L,     L,     L,     L,
    L,     L,     L,     L,     L,     L,     L,     L,     L,     L,     L,
    L,     L,     L,     L,     L,     0,     0,     0,     0,     0,

    0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,     0,     0,     0,

    S,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,     0,     0,     0,

    0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,     0,     0,     0,

    0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
    0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
};

#undef S
#undef D
#undef X
#undef U
#undef L
#undef _
#undef d

/* code point ranges for Zs,Zl or Zp property */
static const u16 char_range_space[] = {
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

bool cvm_unicode_is_space_non_ascii(rune c) {
    usize i, n;

    n = countof(char_range_space);
    for (i = 5; i < n; i += 2) {
        u32 low = char_range_space[i];
        u32 high = char_range_space[i + 1];
        if (c < low)
            return false;
        if (c < high)
            return true;
    }
    return false;
}

#define CVM_UNICODE_DECOMP_MAX 18

typedef enum {
    DECOMP_TYPE_C1, /* 16 bit u8 */
    DECOMP_TYPE_L1, /* 16 bit u8 table */
    DECOMP_TYPE_L2,
    DECOMP_TYPE_L3,
    DECOMP_TYPE_L4,
    DECOMP_TYPE_L5,  /* XXX: not used */
    DECOMP_TYPE_L6,  /* XXX: could remove */
    DECOMP_TYPE_L7,  /* XXX: could remove */
    DECOMP_TYPE_LL1, /* 18 bit u8 table */
    DECOMP_TYPE_LL2,
    DECOMP_TYPE_S1, /* 8 bit u8 table */
    DECOMP_TYPE_S2,
    DECOMP_TYPE_S3,
    DECOMP_TYPE_S4,
    DECOMP_TYPE_S5,
    DECOMP_TYPE_I1, /* increment 16 bit u8 value */
    DECOMP_TYPE_I2_0,
    DECOMP_TYPE_I2_1,
    DECOMP_TYPE_I3_1,
    DECOMP_TYPE_I3_2,
    DECOMP_TYPE_I4_1,
    DECOMP_TYPE_I4_2,
    DECOMP_TYPE_B1, /* 16 bit base + 8 bit offset */
    DECOMP_TYPE_B2,
    DECOMP_TYPE_B3,
    DECOMP_TYPE_B4,
    DECOMP_TYPE_B5,
    DECOMP_TYPE_B6,
    DECOMP_TYPE_B7,
    DECOMP_TYPE_B8,
    DECOMP_TYPE_B18,
    DECOMP_TYPE_LS2,
    DECOMP_TYPE_PAT3,
    DECOMP_TYPE_S2_UL,
    DECOMP_TYPE_LS2_UL,
} DecompTypeEnum;

/* ------------------------------------------------------------------ */
/* the buffer normalization needs                                      */
/* ------------------------------------------------------------------ */

/*
 * A growable run of bytes over the caller's allocator.
 *
 * The ported code wants the shape of cutils' DynBuf, and this is that shape over
 * what our platform layer actually offers. libc's realloc is not it — rt has
 * alloc and free — so the buffer tracks how much it has and copies on growth
 * itself. That is the reason this type exists at all: a callback with realloc's
 * signature cannot be implemented over alloc and free without being told the old
 * size, and the thing that knows it is the buffer.
 */
typedef struct UniBuf {
    u8* data;
    usize size;      /* bytes used */
    usize allocated; /* bytes allocated */
    bool error;
    CvmUnicodeAlloc alloc;
} UniBuf;

static void unibuf_init(UniBuf* buf, CvmUnicodeAlloc alloc) {
    buf->data = NULL;
    buf->size = 0;
    buf->allocated = 0;
    buf->error = false;
    buf->alloc = alloc;
}

static bool unibuf_reserve(UniBuf* buf, usize bytes) {
    if (buf->error) {
        return false;
    }
    if (bytes <= buf->allocated) {
        return true;
    }

    usize want = buf->allocated == 0u ? 64u : buf->allocated;
    while (want < bytes) {
        want *= 2u;
    }

    u8* fresh = buf->alloc.alloc(buf->alloc.ctx, want);
    if (fresh == NULL) {
        buf->error = true;
        return false;
    }
    for (usize i = 0; i < buf->size; i++) {
        fresh[i] = buf->data[i];
    }
    if (buf->data != NULL) {
        buf->alloc.free(buf->alloc.ctx, buf->data);
    }
    buf->data = fresh;
    buf->allocated = want;
    return true;
}

static void unibuf_put_u32(UniBuf* buf, u32 value) {
    if (!unibuf_reserve(buf, buf->size + sizeof(u32))) {
        return;
    }
    buf->data[buf->size + 0u] = (u8)(value & 0xffu);
    buf->data[buf->size + 1u] = (u8)((value >> 8) & 0xffu);
    buf->data[buf->size + 2u] = (u8)((value >> 16) & 0xffu);
    buf->data[buf->size + 3u] = (u8)((value >> 24) & 0xffu);
    buf->size += sizeof(u32);
}

/* Hand the buffer over: the caller frees it with the same allocator, and the
 * buffer forgets it so nothing frees it twice. */
static u8* unibuf_release(UniBuf* buf) {
    u8* data = buf->data;
    buf->data = NULL;
    buf->allocated = 0;
    buf->size = 0;
    return data;
}

/* ------------------------------------------------------------------ */
/* character ranges                                                    */
/* ------------------------------------------------------------------ */

void cvm_cr_init(CvmCharRange* cr, CvmUnicodeAlloc alloc) {
    cr->len = 0;
    cr->size = 0;
    cr->points = NULL;
    cr->alloc = alloc;
}

void cvm_cr_free(CvmCharRange* cr) {
    if (cr->points != NULL) {
        cr->alloc.free(cr->alloc.ctx, cr->points);
    }
    cr->points = NULL;
    cr->len = 0;
    cr->size = 0;
}

i32 cvm_cr_realloc(CvmCharRange* cr, i32 size) {
    /*
     * Grow by allocating and copying: the allocator has no realloc, and the old
     * length is `cr->size`, so this is the whole of what a realloc would do.
     */
    u32* fresh = cr->alloc.alloc(cr->alloc.ctx, (usize)size * sizeof(u32));
    if (fresh == NULL) {
        return -1;
    }
    for (i32 i = 0; i < cr->len; i++) {
        fresh[i] = cr->points[i];
    }
    if (cr->points != NULL) {
        cr->alloc.free(cr->alloc.ctx, cr->points);
    }
    cr->points = fresh;
    cr->size = size;
    return 0;
}

i32 cvm_cr_copy(CvmCharRange* cr, const CvmCharRange* cr1) {
    if (cvm_cr_realloc(cr, cr1->len))
        return -1;
    memcpy(cr->points, cr1->points, sizeof(cr->points[0]) * cr1->len);
    cr->len = cr1->len;
    return 0;
}

/* merge consecutive intervals and remove empty intervals */
static void cr_compress(CvmCharRange* cr) {
    i32 i, j, k, len;
    u32* pt;

    pt = cr->points;
    len = cr->len;
    i = 0;
    j = 0;
    k = 0;
    while ((i + 1) < len) {
        if (pt[i] == pt[i + 1]) {
            /* empty interval */
            i += 2;
        } else {
            j = i;
            while ((j + 3) < len && pt[j + 1] == pt[j + 2])
                j += 2;
            /* just copy */
            pt[k] = pt[i];
            pt[k + 1] = pt[j + 1];
            k += 2;
            i = j + 2;
        }
    }
    cr->len = k;
}

/* union or intersection */
i32 cvm_cr_op(CvmCharRange* cr, const u32* a_pt, i32 a_len, const u32* b_pt, i32 b_len,
              i32 op) {
    i32 a_idx, b_idx, is_in;
    u32 v;

    a_idx = 0;
    b_idx = 0;
    for (;;) {
        /* get one more point from a or b in increasing order */
        if (a_idx < a_len && b_idx < b_len) {
            if (a_pt[a_idx] < b_pt[b_idx]) {
                goto a_add;
            } else if (a_pt[a_idx] == b_pt[b_idx]) {
                v = a_pt[a_idx];
                a_idx++;
                b_idx++;
            } else {
                goto b_add;
            }
        } else if (a_idx < a_len) {
a_add:
            v = a_pt[a_idx++];
        } else if (b_idx < b_len) {
b_add:
            v = b_pt[b_idx++];
        } else {
            break;
        }
        /* add the point if the in/out status changes */
        switch (op) {
        case CVM_CR_OP_UNION:
            is_in = (a_idx & 1) | (b_idx & 1);
            break;
        case CVM_CR_OP_INTER:
            is_in = (a_idx & 1) & (b_idx & 1);
            break;
        case CVM_CR_OP_XOR:
            is_in = (a_idx & 1) ^ (b_idx & 1);
            break;
        case CVM_CR_OP_SUB:
            is_in = (a_idx & 1) & ((b_idx & 1) ^ 1);
            break;
        default:
            /* Not reachable: the ported code's own invariant, and a runtime
             * that stops is better than one that writes past the end. */
            rt_abort();
        }
        if (is_in != (cr->len & 1)) {
            if (cvm_cr_add_point(cr, v))
                return -1;
        }
    }
    cr_compress(cr);
    return 0;
}

i32 cvm_cr_op1(CvmCharRange* cr, const u32* b_pt, i32 b_len, i32 op) {
    CvmCharRange a = *cr;
    i32 ret;
    cr->len = 0;
    cr->size = 0;
    cr->points = NULL;
    ret = cvm_cr_op(cr, a.points, a.len, b_pt, b_len, op);
    cvm_cr_free(&a);
    return ret;
}

i32 cvm_cr_invert(CvmCharRange* cr) {
    i32 len;
    len = cr->len;
    if (cvm_cr_realloc(cr, len + 2))
        return -1;
    memmove(cr->points + 1, cr->points, len * sizeof(cr->points[0]));
    cr->points[0] = 0;
    cr->points[len + 1] = UINT32_MAX;
    cr->len = len + 2;
    cr_compress(cr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* normalization                                                       */
/* ------------------------------------------------------------------ */

static u32 unicode_get_short_code(u32 c) {
    static const u16 unicode_short_table[2] = {0x2044, 0x2215};

    if (c < 0x80)
        return c;
    else if (c < 0x80 + 0x50)
        return c - 0x80 + 0x300;
    else
        return unicode_short_table[c - 0x80 - 0x50];
}

static u32 unicode_get_lower_simple(u32 c) {
    if (c < 0x100 || (c >= 0x410 && c <= 0x42f))
        c += 0x20;
    else
        c++;
    return c;
}

static u16 unicode_get16(const u8* p) {
    return p[0] | (p[1] << 8);
}

static i32 unicode_decomp_entry(u32* res, u32 c, i32 idx, u32 code, u32 len, u32 type) {
    u32 c1;
    i32 l, i, p;
    const u8* d;

    if (type == DECOMP_TYPE_C1) {
        res[0] = unicode_decomp_table2[idx];
        return 1;
    } else {
        d = unicode_decomp_data + unicode_decomp_table2[idx];
        switch (type) {
        case DECOMP_TYPE_L1:
        case DECOMP_TYPE_L2:
        case DECOMP_TYPE_L3:
        case DECOMP_TYPE_L4:
        case DECOMP_TYPE_L5:
        case DECOMP_TYPE_L6:
        case DECOMP_TYPE_L7:
            l = type - DECOMP_TYPE_L1 + 1;
            d += (c - code) * l * 2;
            for (i = 0; i < l; i++) {
                if ((res[i] = unicode_get16(d + 2 * i)) == 0)
                    return 0;
            }
            return l;
        case DECOMP_TYPE_LL1:
        case DECOMP_TYPE_LL2: {
            u32 k, p;
            l = type - DECOMP_TYPE_LL1 + 1;
            k = (c - code) * l;
            p = len * l * 2;
            for (i = 0; i < l; i++) {
                c1 = unicode_get16(d + 2 * k) |
                     (((d[p + (k / 4)] >> ((k % 4) * 2)) & 3) << 16);
                if (!c1)
                    return 0;
                res[i] = c1;
                k++;
            }
        }
            return l;
        case DECOMP_TYPE_S1:
        case DECOMP_TYPE_S2:
        case DECOMP_TYPE_S3:
        case DECOMP_TYPE_S4:
        case DECOMP_TYPE_S5:
            l = type - DECOMP_TYPE_S1 + 1;
            d += (c - code) * l;
            for (i = 0; i < l; i++) {
                if ((res[i] = unicode_get_short_code(d[i])) == 0)
                    return 0;
            }
            return l;
        case DECOMP_TYPE_I1:
            l = 1;
            p = 0;
            goto decomp_type_i;
        case DECOMP_TYPE_I2_0:
        case DECOMP_TYPE_I2_1:
        case DECOMP_TYPE_I3_1:
        case DECOMP_TYPE_I3_2:
        case DECOMP_TYPE_I4_1:
        case DECOMP_TYPE_I4_2:
            l = 2 + ((type - DECOMP_TYPE_I2_0) >> 1);
            p = ((type - DECOMP_TYPE_I2_0) & 1) + (l > 2);
decomp_type_i:
            for (i = 0; i < l; i++) {
                c1 = unicode_get16(d + 2 * i);
                if (i == p)
                    c1 += c - code;
                res[i] = c1;
            }
            return l;
        case DECOMP_TYPE_B18:
            l = 18;
            goto decomp_type_b;
        case DECOMP_TYPE_B1:
        case DECOMP_TYPE_B2:
        case DECOMP_TYPE_B3:
        case DECOMP_TYPE_B4:
        case DECOMP_TYPE_B5:
        case DECOMP_TYPE_B6:
        case DECOMP_TYPE_B7:
        case DECOMP_TYPE_B8:
            l = type - DECOMP_TYPE_B1 + 1;
decomp_type_b:
            {
                u32 c_min;
                c_min = unicode_get16(d);
                d += 2 + (c - code) * l;
                for (i = 0; i < l; i++) {
                    c1 = d[i];
                    if (c1 == 0xff)
                        c1 = 0x20;
                    else
                        c1 += c_min;
                    res[i] = c1;
                }
            }
            return l;
        case DECOMP_TYPE_LS2:
            d += (c - code) * 3;
            if (!(res[0] = unicode_get16(d)))
                return 0;
            res[1] = unicode_get_short_code(d[2]);
            return 2;
        case DECOMP_TYPE_PAT3:
            res[0] = unicode_get16(d);
            res[2] = unicode_get16(d + 2);
            d += 4 + (c - code) * 2;
            res[1] = unicode_get16(d);
            return 3;
        case DECOMP_TYPE_S2_UL:
        case DECOMP_TYPE_LS2_UL:
            c1 = c - code;
            if (type == DECOMP_TYPE_S2_UL) {
                d += c1 & ~1;
                c = unicode_get_short_code(*d);
                d++;
            } else {
                d += (c1 >> 1) * 3;
                c = unicode_get16(d);
                d += 2;
            }
            if (c1 & 1)
                c = unicode_get_lower_simple(c);
            res[0] = c;
            res[1] = unicode_get_short_code(*d);
            return 2;
        }
    }
    return 0;
}

/* return the length of the decomposition (length <=
   CVM_UNICODE_DECOMP_MAX) or 0 if no decomposition */
static i32 unicode_decomp_char(u32* res, u32 c, bool is_compat1) {
    u32 v, type, is_compat, code, len;
    i32 idx_min, idx_max, idx;

    idx_min = 0;
    idx_max = countof(unicode_decomp_table1) - 1;
    while (idx_min <= idx_max) {
        idx = (idx_max + idx_min) / 2;
        v = unicode_decomp_table1[idx];
        code = v >> (32 - 18);
        len = (v >> (32 - 18 - 7)) & 0x7f;
        //        printf("idx=%d code=%05x len=%d\n", idx, code, len);
        if (c < code) {
            idx_max = idx - 1;
        } else if (c >= code + len) {
            idx_min = idx + 1;
        } else {
            is_compat = v & 1;
            if (is_compat1 < is_compat)
                break;
            type = (v >> (32 - 18 - 7 - 6)) & 0x3f;
            return unicode_decomp_entry(res, c, idx, code, len, type);
        }
    }
    return 0;
}

/* return 0 if no pair found */
static i32 unicode_compose_pair(u32 c0, u32 c1) {
    u32 code, len, type, v, idx1, d_idx, d_offset, ch;
    i32 idx_min, idx_max, idx, d;
    u32 pair[2];

    idx_min = 0;
    idx_max = countof(unicode_comp_table) - 1;
    while (idx_min <= idx_max) {
        idx = (idx_max + idx_min) / 2;
        idx1 = unicode_comp_table[idx];

        /* idx1 represent an entry of the decomposition table */
        d_idx = idx1 >> 6;
        d_offset = idx1 & 0x3f;
        v = unicode_decomp_table1[d_idx];
        code = v >> (32 - 18);
        len = (v >> (32 - 18 - 7)) & 0x7f;
        type = (v >> (32 - 18 - 7 - 6)) & 0x3f;
        ch = code + d_offset;
        unicode_decomp_entry(pair, ch, d_idx, code, len, type);
        d = c0 - pair[0];
        if (d == 0)
            d = c1 - pair[1];
        if (d < 0) {
            idx_max = idx - 1;
        } else if (d > 0) {
            idx_min = idx + 1;
        } else {
            return ch;
        }
    }
    return 0;
}

/* return the combining class of character c (between 0 and 255) */
static i32 unicode_get_cc(u32 c) {
    u32 code, n, type, cc, c1, b;
    i32 pos;
    const u8* p;

    pos = get_index_pos(&code, c, unicode_cc_index, sizeof(unicode_cc_index) / 3);
    if (pos < 0)
        return 0;
    p = unicode_cc_table + pos;
    for (;;) {
        b = *p++;
        type = b >> 6;
        n = b & 0x3f;
        if (n < 48) {
        } else if (n < 56) {
            n = (n - 48) << 8;
            n |= *p++;
            n += 48;
        } else {
            n = (n - 56) << 8;
            n |= *p++ << 8;
            n |= *p++;
            n += 48 + (1 << 11);
        }
        if (type <= 1)
            p++;
        c1 = code + n + 1;
        if (c < c1) {
            switch (type) {
            case 0:
                cc = p[-1];
                break;
            case 1:
                cc = p[-1] + c - code;
                break;
            case 2:
                cc = 0;
                break;
            default:
            case 3:
                cc = 230;
                break;
            }
            return cc;
        }
        code = c1;
    }
}

static void sort_cc(i32* buf, i32 len) {
    i32 i, j, k, cc, cc1, start, ch1;

    for (i = 0; i < len; i++) {
        cc = unicode_get_cc(buf[i]);
        if (cc != 0) {
            start = i;
            j = i + 1;
            while (j < len) {
                ch1 = buf[j];
                cc1 = unicode_get_cc(ch1);
                if (cc1 == 0)
                    break;
                k = j - 1;
                while (k >= start) {
                    if (unicode_get_cc(buf[k]) <= cc1)
                        break;
                    buf[k + 1] = buf[k];
                    k--;
                }
                buf[k + 1] = ch1;
                j++;
            }
            i = j;
        }
    }
}

static void to_nfd_rec(UniBuf* dbuf, const i32* src, i32 src_len, i32 is_compat) {
    u32 c, v;
    i32 i, l;
    u32 res[CVM_UNICODE_DECOMP_MAX];

    for (i = 0; i < src_len; i++) {
        c = src[i];
        if (c >= 0xac00 && c < 0xd7a4) {
            /* Hangul decomposition */
            c -= 0xac00;
            unibuf_put_u32(dbuf, 0x1100 + c / 588);
            unibuf_put_u32(dbuf, 0x1161 + (c % 588) / 28);
            v = c % 28;
            if (v != 0)
                unibuf_put_u32(dbuf, 0x11a7 + v);
        } else {
            l = unicode_decomp_char(res, c, is_compat);
            if (l) {
                to_nfd_rec(dbuf, (i32*)res, l, is_compat);
            } else {
                unibuf_put_u32(dbuf, c);
            }
        }
    }
}

/* return 0 if not found */
static i32 compose_pair(u32 c0, u32 c1) {
    /* Hangul composition */
    if (c0 >= 0x1100 && c0 < 0x1100 + 19 && c1 >= 0x1161 && c1 < 0x1161 + 21) {
        return 0xac00 + (c0 - 0x1100) * 588 + (c1 - 0x1161) * 28;
    } else if (c0 >= 0xac00 && c0 < 0xac00 + 11172 && (c0 - 0xac00) % 28 == 0 &&
               c1 >= 0x11a7 && c1 < 0x11a7 + 28) {
        return c0 + c1 - 0x11a7;
    } else {
        return unicode_compose_pair(c0, c1);
    }
}

i32 cvm_unicode_normalize(rune** pdst, rune const* src, usize src_len,
                          CvmUnicodeNormalization kind, CvmUnicodeAlloc alloc) {
    i32* buf;
    usize buf_len;
    i32 i, p, starter_pos, cc, last_cc;
    usize out_len;
    bool is_compat;
    UniBuf dbuf_s, *dbuf = &dbuf_s;

    if (src_len == 0) {
        *pdst = NULL;
        return 0;
    }

    is_compat = kind >= CVM_UNICODE_NFKC;

    unibuf_init(dbuf, alloc);
    if (!unibuf_reserve(dbuf, sizeof(i32) * src_len))
        goto fail;

    /* common case: latin1 is unaffected by NFC */
    if (kind == CVM_UNICODE_NFC) {
        for (i = 0; i < (i32)src_len; i++) {
            if (src[i] >= 0x100)
                goto not_latin1;
        }
        buf = (i32*)dbuf->data;
        memcpy(buf, src, src_len * sizeof(i32));
        *pdst = (rune*)unibuf_release(dbuf);
        return (i32)src_len;
not_latin1:;
    }

    to_nfd_rec(dbuf, (i32 const*)src, src_len, is_compat);
    if (dbuf->error) {
fail:
        *pdst = NULL;
        return -1;
    }
    buf = (i32*)dbuf->data;
    buf_len = dbuf->size / sizeof(i32);

    sort_cc(buf, buf_len);

    if (buf_len <= 1u || kind == CVM_UNICODE_NFD || kind == CVM_UNICODE_NFKD) {
        /* NFD / NFKD: decomposition only, no composition back */
        *pdst = (rune*)unibuf_release(dbuf);
        return (i32)buf_len;
    }

    i = 1;
    out_len = 1u;
    while (i < (i32)buf_len) {
        /* find the starter character and test if it is blocked from
           the character at 'i' */
        last_cc = unicode_get_cc((u32)buf[i]);
        starter_pos = (i32)out_len - 1;
        while (starter_pos >= 0) {
            cc = unicode_get_cc((u32)buf[starter_pos]);
            if (cc == 0)
                break;
            if (cc >= last_cc)
                goto next;
            last_cc = 256;
            starter_pos--;
        }
        if (starter_pos >= 0 && (p = compose_pair(buf[starter_pos], buf[i])) != 0) {
            buf[starter_pos] = p;
            i++;
        } else {
next:
            buf[out_len++] = buf[i++];
        }
    }
    *pdst = (rune*)unibuf_release(dbuf);
    return (i32)out_len;
}

/* ------------------------------------------------------------------ */
/* over strings                                                        */
/* ------------------------------------------------------------------ */

/*
 * Through the builder, which is off-heap: the only heap allocation is the
 * result, and the only rooting needed is for the source across it. The builder
 * widens on its own when a replacement needs 16 bits, which is what makes sharp
 * s and dotted I come out right with no special path here.
 */
static CvmString* case_map(CvmString const* s, CvmUnicodeCase which) {
    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = (opaque_mut)(CvmString*)s;

    CvmStrBuf out;
    cvm_str_buf_init(&out);

    CvmString const* src = (CvmString const*)f_roots[0];
    usize i = 0;
    bool ok = true;
    while (i < src->length && ok) {
        usize next = 0;
        rune c = cvm_string_code_point_at(src, i, &next);
        i = next;

        rune mapped[CVM_UNICODE_CASE_MAX];
        u32 n = cvm_unicode_case_conv(mapped, c, which);
        for (u32 k = 0; k < n && ok; k++) {
            ok = cvm_str_buf_put_rune(&out, mapped[k]);
        }
    }

    CvmString* result = ok ? cvm_str_buf_finish(&out) : NULL;
    cvm_str_buf_free(&out);
    cvm_frame_pop(&f_frame);
    return result;
}

CvmString* cvm_unicode_upper(CvmString const* s) {
    return case_map(s, CVM_UNICODE_CASE_UPPER);
}

CvmString* cvm_unicode_lower(CvmString const* s) {
    return case_map(s, CVM_UNICODE_CASE_LOWER);
}

/* ------------------------------------------------------------------ */
/* normalization, over a string                                        */
/* ------------------------------------------------------------------ */

static opaque_mut system_alloc(opaque_mut ctx, usize bytes) {
    (void)ctx;
    return rt_mem_alloc(bytes);
}

static void system_free(opaque_mut ctx, opaque_mut p) {
    (void)ctx;
    rt_mem_free(p);
}

CvmUnicodeAlloc cvm_unicode_system_alloc(void) {
    CvmUnicodeAlloc a = {.ctx = NULL, .alloc = system_alloc, .free = system_free};
    return a;
}

CvmString* cvm_unicode_normalize_string(CvmString const* s,
                                        CvmUnicodeNormalization kind) {
    CvmUnicodeAlloc alloc = cvm_unicode_system_alloc();

    /* The source becomes an off-heap run of code points first: the normalizer
     * wants a plain array, and a string's units are not one. */
    usize length = cvm_string_length(s);
    rune* input = alloc.alloc(alloc.ctx, (length == 0u ? 1u : length) * sizeof(rune));
    if (input == NULL) {
        return NULL;
    }
    for (usize i = 0; i < length; i++) {
        input[i] = cvm_string_unit(s, i);
    }

    rune* normalized = NULL;
    i32 out_len = cvm_unicode_normalize(&normalized, input, length, kind, alloc);
    alloc.free(alloc.ctx, input);
    if (out_len < 0) {
        return NULL;
    }

    /*
     * Built off-heap and frozen once, so the only heap allocation is the string
     * itself — and the source is rooted across it, because that allocation can
     * collect.
     */
    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = (opaque_mut)(CvmString*)s;

    CvmStrBuf out;
    cvm_str_buf_init(&out);
    bool ok = true;
    for (i32 i = 0; i < out_len && ok; i++) {
        ok = cvm_str_buf_put_rune(&out, normalized[i]);
    }
    CvmString* result = ok ? cvm_str_buf_finish(&out) : NULL;
    cvm_str_buf_free(&out);
    cvm_frame_pop(&f_frame);

    if (normalized != NULL) {
        alloc.free(alloc.ctx, normalized);
    }
    return result;
}
