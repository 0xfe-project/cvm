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

#define UNICODE_DECOMP_LEN_MAX 18

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
