/*
 * Tiny float64 printing and parsing library
 *
 * Copyright (c) 2024 Fabrice Bellard
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
 * vm/str/dtoa.c
 *
 * f64 <-> text. A port of the implementation above (Fabrice Bellard, MIT),
 * which is what quickjs-ng uses in place of libbf. str/dtoa.h has the API;
 * bench/dtoa/README.md has the numbers behind choosing this one.
 *
 * What the port changed, beyond names and types:
 *
 *   - the includes are ours, and the handful of small things upstream reaches
 *     into cutils.h for are re-derived below rather than carrying a 2000-line
 *     header in for them
 *
 *   - the table generators are gone. Upstream keeps them behind #if 0 as a
 *     record of where the static tables came from; the record is the upstream
 *     file, and dead code that mentions printf has no place in a freestanding
 *     translation unit
 *
 *   - cvm_atod reads through `peek`, so a slice with a known length behaves
 *     exactly as the NUL-terminated string upstream assumed, while never
 *     touching a byte past the slice. That is the one place the port changes
 *     control flow, and it is what lets a JSON parser hand us a window into a
 *     buffer it does not own the end of.
 *
 * The algorithm is untouched, deliberately. It is subtle and it is correct,
 * and every line rewritten would be a line to get wrong.
 */

#include "base/assert.h"
#include "base/types.h"
#include "mem.h"

#include "str/dtoa.h"

/* ------------------------------------------------------------------ */
/* what upstream reached into cutils.h for                             */
/* ------------------------------------------------------------------ */

/*
 * Each is the definition this file needs and nothing more. The conversions are
 * done through memcpy because that is the one way to reinterpret bits that
 * does not depend on the compiler noticing anything.
 */

CVM_INLINE f64 u64_as_f64(u64 bits) {
    f64 d;
    memcpy(&d, &bits, sizeof(d));
    return d;
}

CVM_INLINE u64 f64_as_u64(f64 d) {
    u64 bits;
    memcpy(&bits, &d, sizeof(bits));
    return bits;
}

/* NaN without math.h, which a freestanding translation unit does not have. */
CVM_INLINE f64 f64_nan(void) {
    return u64_as_f64(0x7ff8000000000000ull);
}

#if defined(__GNUC__) || defined(__clang__)

CVM_INLINE u32 ctz_u32(u32 v) {
    return v == 0u ? 32u : (u32)__builtin_ctz(v);
}

CVM_INLINE u32 clz_u32(u32 v) {
    return v == 0u ? 32u : (u32)__builtin_clz(v);
}

#else

/* Portable fallbacks, for a compiler without the intrinsics. Not hot: the
 * limb loops dominate everything here. */
CVM_INLINE u32 ctz_u32(u32 v) {
    u32 n = 0;
    while (n < 32u && (v & 1u) == 0u) {
        v >>= 1;
        n++;
    }
    return n;
}

CVM_INLINE u32 clz_u32(u32 v) {
    u32 n = 0;
    while (n < 32u && (v & 0x80000000u) == 0u) {
        v <<= 1;
        n++;
    }
    return n;
}

#endif

CVM_INLINE u32 clz_u64(u64 v) {
    return v == 0ull ? 64u : (u32)__builtin_clzll(v);
}

CVM_INLINE i32 abs_i32(i32 v) {
    return v < 0 ? -v : v;
}

CVM_INLINE i32 max_i32(i32 a, i32 b) {
    return a > b ? a : b;
}

CVM_INLINE i32 min_i32(i32 a, i32 b) {
    return a < b ? a : b;
}

/*
 * The byte at `off` from `p`, or 0 when that is at or past `end`.
 *
 * The scanner below was written for a NUL-terminated string, so "the input
 * ended here" and "this byte is not a digit" were a single test. Reading 0
 * past the end keeps every one of those comparisons meaning what it meant,
 * while guaranteeing not one byte beyond the slice is ever read — the memory
 * after it belongs to whoever the caller borrowed it from.
 */
CVM_INLINE u8 peek(u8 const* p, u8 const* end, usize off) {
    return (end - p) > (isize)off ? p[off] : 0u;
}

/*
 * Whether the text at `p` starts with `prefix`, whose length is passed because
 * nothing in this codebase is NUL-terminated. On a match `*pp` moves past it.
 */
static bool str_startswith(u8 const* p, u8 const* end, u8 const* prefix, usize len,
                           u8 const** pp) {
    if ((usize)(end - p) < len) {
        return false;
    }
    for (usize i = 0; i < len; i++) {
        if (p[i] != prefix[i]) {
            return false;
        }
    }
    *pp = p + len;
    return true;
}

/* ------------------------------------------------------------------ */
/* the ported algorithm                                                */
/* ------------------------------------------------------------------ */

#define USE_POW5_TABLE
/* use fast path to print small integers in free format */
#define USE_FAST_INT

#define LIMB_LOG2_BITS 5

#define LIMB_BITS (1 << LIMB_LOG2_BITS)

typedef i32 slimb_t;
typedef u32 limb_t;
typedef u64 dlimb_t;

#define LIMB_DIGITS 9

#define CVM_RADIX_MAX 36

#define DBIGNUM_LEN_MAX 52 /* ~ 2^(1072+53)*36^100 (dtoa) */
#define MANT_LEN_MAX 18    /* < 36^100 */

typedef isize mp_size_t;

/* the represented number is sum(i, tab[i]*2^(LIMB_BITS * i)) */
typedef struct {
    i32 len; /* >= 1 */
    limb_t tab[];
} mpb_t;

static limb_t mp_add_ui(limb_t* tab, limb_t b, usize n) {
    usize i;
    limb_t k, a;

    k = b;
    for (i = 0; i < n; i++) {
        if (k == 0)
            break;
        a = tab[i] + k;
        k = (a < k);
        tab[i] = a;
    }
    return k;
}

/* tabr[] = taba[] * b + l. Return the high carry */
static limb_t mp_mul1(limb_t* tabr, const limb_t* taba, limb_t n, limb_t b, limb_t l) {
    limb_t i;
    dlimb_t t;

    for (i = 0; i < n; i++) {
        t = (dlimb_t)taba[i] * (dlimb_t)b + l;
        tabr[i] = t;
        l = t >> LIMB_BITS;
    }
    return l;
}

/* WARNING: d must be >= 2^(LIMB_BITS-1) */
static inline limb_t udiv1norm_init(limb_t d) {
    limb_t a0, a1;
    a1 = -d - 1;
    a0 = -1;
    return (((dlimb_t)a1 << LIMB_BITS) | a0) / d;
}

/* return the quotient and the remainder in '*pr'of 'a1*2^LIMB_BITS+a0
   / d' with 0 <= a1 < d. */
static inline limb_t udiv1norm(limb_t* pr, limb_t a1, limb_t a0, limb_t d,
                               limb_t d_inv) {
    limb_t n1m, n_adj, q, r, ah;
    dlimb_t a;
    n1m = ((slimb_t)a0 >> (LIMB_BITS - 1));
    n_adj = a0 + (n1m & d);
    a = (dlimb_t)d_inv * (a1 - n1m) + n_adj;
    q = (a >> LIMB_BITS) + a1;
    /* compute a - q * r and update q so that the remainder is between
       0 and d - 1 */
    a = ((dlimb_t)a1 << LIMB_BITS) | a0;
    a = a - (dlimb_t)q * d - d;
    ah = a >> LIMB_BITS;
    q += 1 + ah;
    r = (limb_t)a + (ah & d);
    *pr = r;
    return q;
}

static limb_t mp_div1(limb_t* tabr, const limb_t* taba, limb_t n, limb_t b, limb_t r) {
    slimb_t i;
    dlimb_t a1;
    for (i = n - 1; i >= 0; i--) {
        a1 = ((dlimb_t)r << LIMB_BITS) | taba[i];
        tabr[i] = a1 / b;
        r = a1 % b;
    }
    return r;
}

/* r = (a + high*B^n) >> shift. Return the remainder r (0 <= r < 2^shift). 
   1 <= shift <= LIMB_BITS - 1 */
static limb_t mp_shr(limb_t* tab_r, const limb_t* tab, mp_size_t n, i32 shift,
                     limb_t high) {
    mp_size_t i;
    limb_t l, a;

    cvm_assert(shift >= 1 && shift < LIMB_BITS);
    l = high;
    for (i = n - 1; i >= 0; i--) {
        a = tab[i];
        tab_r[i] = (a >> shift) | (l << (LIMB_BITS - shift));
        l = a;
    }
    return l & (((limb_t)1 << shift) - 1);
}

/* r = (a << shift) + low. 1 <= shift <= LIMB_BITS - 1, 0 <= low <
   2^shift. */
static limb_t mp_shl(limb_t* tab_r, const limb_t* tab, mp_size_t n, i32 shift,
                     limb_t low) {
    mp_size_t i;
    limb_t l, a;

    cvm_assert(shift >= 1 && shift < LIMB_BITS);
    l = low;
    for (i = 0; i < n; i++) {
        a = tab[i];
        tab_r[i] = (a << shift) | l;
        l = (a >> (LIMB_BITS - shift));
    }
    return l;
}

static CVM_NOINLINE limb_t mp_div1norm(limb_t* tabr, const limb_t* taba, limb_t n,
                                       limb_t b, limb_t r, limb_t b_inv, i32 shift) {
    slimb_t i;

    if (shift != 0) {
        r = (r << shift) | mp_shl(tabr, taba, n, shift, 0);
    }
    for (i = n - 1; i >= 0; i--) {
        tabr[i] = udiv1norm(&r, r, taba[i], b, b_inv);
    }
    r >>= shift;
    return r;
}

static void mpb_renorm(mpb_t* r) {
    while (r->len > 1 && r->tab[r->len - 1] == 0)
        r->len--;
}

#ifdef USE_POW5_TABLE
static const u32 pow5_table[17] = {
    0x00000005, 0x00000019, 0x0000007d, 0x00000271, 0x00000c35, 0x00003d09,
    0x0001312d, 0x0005f5e1, 0x001dcd65, 0x009502f9, 0x02e90edd, 0x0e8d4a51,
    0x48c27395, 0x6bcc41e9, 0x1afd498d, 0x86f26fc1, 0xa2bc2ec5,
};

static const u8 pow5h_table[4] = {
    0x00000001,
    0x00000007,
    0x00000023,
    0x000000b1,
};

static const u32 pow5_inv_table[13] = {
    0x99999999, 0x47ae147a, 0x0624dd2f, 0xa36e2eb1, 0x4f8b588e, 0x0c6f7a0b, 0xad7f29ab,
    0x5798ee23, 0x12e0be82, 0xb7cdfd9d, 0x5fd7fe17, 0x19799812, 0xc25c2684,
};
#endif

/* return a^b */
static u64 pow_ui(u32 a, u32 b) {
    i32 i, n_bits;
    u64 r;
    if (b == 0)
        return 1;
    if (b == 1)
        return a;
#ifdef USE_POW5_TABLE
    if ((a == 5 || a == 10) && b <= 17) {
        r = pow5_table[b - 1];
        if (b >= 14) {
            r |= (u64)pow5h_table[b - 14] << 32;
        }
        if (a == 10)
            r <<= b;
        return r;
    }
#endif
    r = a;
    n_bits = 32 - clz_u32(b);
    for (i = n_bits - 2; i >= 0; i--) {
        r *= r;
        if ((b >> i) & 1)
            r *= a;
    }
    return r;
}

static u32 pow_ui_inv(u32* pr_inv, i32* pshift, u32 a, u32 b) {
    u32 r_inv, r;
    i32 shift;
#ifdef USE_POW5_TABLE
    if (a == 5 && b >= 1 && b <= 13) {
        r = pow5_table[b - 1];
        shift = clz_u32(r);
        r <<= shift;
        r_inv = pow5_inv_table[b - 1];
    } else
#endif
    {
        r = pow_ui(a, b);
        shift = clz_u32(r);
        r <<= shift;
        r_inv = udiv1norm_init(r);
    }
    *pshift = shift;
    *pr_inv = r_inv;
    return r;
}

enum {
    CVM_RNDN,  /* round to nearest, ties to even */
    CVM_RNDNA, /* round to nearest, ties away from zero */
    CVM_RNDZ,
};

static i32 mpb_get_bit(const mpb_t* r, i32 k) {
    i32 l;

    l = (u32)k / LIMB_BITS;
    k = k & (LIMB_BITS - 1);
    if (l >= r->len)
        return 0;
    else
        return (r->tab[l] >> k) & 1;
}

/* compute round(r / 2^shift). 'shift' can be negative */
static void mpb_shr_round(mpb_t* r, i32 shift, i32 rnd_mode) {
    i32 l, i;

    if (shift == 0)
        return;
    if (shift < 0) {
        shift = -shift;
        l = (u32)shift / LIMB_BITS;
        shift = shift & (LIMB_BITS - 1);
        if (shift != 0) {
            r->tab[r->len] = mp_shl(r->tab, r->tab, r->len, shift, 0);
            r->len++;
            mpb_renorm(r);
        }
        if (l > 0) {
            for (i = r->len - 1; i >= 0; i--)
                r->tab[i + l] = r->tab[i];
            for (i = 0; i < l; i++)
                r->tab[i] = 0;
            r->len += l;
        }
    } else {
        limb_t bit1, bit2;
        i32 k, add_one;

        switch (rnd_mode) {
        default:
        case CVM_RNDZ:
            add_one = 0;
            break;
        case CVM_RNDN:
        case CVM_RNDNA:
            bit1 = mpb_get_bit(r, shift - 1);
            if (bit1) {
                if (rnd_mode == CVM_RNDNA) {
                    bit2 = 1;
                } else {
                    /* bit2 = oring of all the bits after bit1 */
                    bit2 = 0;
                    if (shift >= 2) {
                        k = shift - 1;
                        l = (u32)k / LIMB_BITS;
                        k = k & (LIMB_BITS - 1);
                        for (i = 0; i < min_i32(l, r->len); i++)
                            bit2 |= r->tab[i];
                        if (l < r->len)
                            bit2 |= r->tab[l] & (((limb_t)1 << k) - 1);
                    }
                }
                if (bit2) {
                    add_one = 1;
                } else {
                    /* round to even */
                    add_one = mpb_get_bit(r, shift);
                }
            } else {
                add_one = 0;
            }
            break;
        }

        l = (u32)shift / LIMB_BITS;
        shift = shift & (LIMB_BITS - 1);
        if (l >= r->len) {
            r->len = 1;
            r->tab[0] = add_one;
        } else {
            if (l > 0) {
                r->len -= l;
                for (i = 0; i < r->len; i++)
                    r->tab[i] = r->tab[i + l];
            }
            if (shift != 0) {
                mp_shr(r->tab, r->tab, r->len, shift, 0);
                mpb_renorm(r);
            }
            if (add_one) {
                limb_t a;
                a = mp_add_ui(r->tab, 1, r->len);
                if (a)
                    r->tab[r->len++] = a;
            }
        }
    }
}

/* return -1, 0 or 1 */
static i32 mpb_cmp(const mpb_t* a, const mpb_t* b) {
    mp_size_t i;
    if (a->len < b->len)
        return -1;
    else if (a->len > b->len)
        return 1;
    for (i = a->len - 1; i >= 0; i--) {
        if (a->tab[i] != b->tab[i]) {
            if (a->tab[i] < b->tab[i])
                return -1;
            else
                return 1;
        }
    }
    return 0;
}

static void mpb_set_u64(mpb_t* r, u64 m) {
#if LIMB_BITS == 64
    r->tab[0] = m;
    r->len = 1;
#else
    r->tab[0] = m;
    r->tab[1] = m >> LIMB_BITS;
    if (r->tab[1] == 0)
        r->len = 1;
    else
        r->len = 2;
#endif
}

static u64 mpb_get_u64(mpb_t* r) {
#if LIMB_BITS == 64
    return r->tab[0];
#else
    if (r->len == 1) {
        return r->tab[0];
    } else {
        return r->tab[0] | ((u64)r->tab[1] << LIMB_BITS);
    }
#endif
}

/* floor_log2() = position of the first non zero bit or -1 if zero. */
static i32 mpb_floor_log2(mpb_t* a) {
    limb_t v;
    v = a->tab[a->len - 1];
    if (v == 0)
        return -1;
    else
        return a->len * LIMB_BITS - 1 - clz_u32(v);
}

#define MUL_LOG2_RADIX_BASE_LOG2 24

/* round((1 << MUL_LOG2_RADIX_BASE_LOG2)/log2(i + 2)) */
static const u32 mul_log2_radix_table[CVM_RADIX_MAX - 1] = {
    0x000000, 0xa1849d, 0x000000, 0x6e40d2, 0x6308c9, 0x5b3065, 0x000000,
    0x50c24e, 0x4d104d, 0x4a0027, 0x4768ce, 0x452e54, 0x433d00, 0x418677,
    0x000000, 0x3ea16b, 0x3d645a, 0x3c43c2, 0x3b3b9a, 0x3a4899, 0x39680b,
    0x3897b3, 0x37d5af, 0x372069, 0x367686, 0x35d6df, 0x354072, 0x34b261,
    0x342bea, 0x33ac62, 0x000000, 0x32bfd9, 0x3251dd, 0x31e8d6, 0x318465,
};

/* return floor(a / log2(radix)) for -2048 <= a <= 2047 */
static i32 mul_log2_radix(i32 a, i32 radix) {
    i32 radix_bits, mult;

    if ((radix & (radix - 1)) == 0) {
        /* if the radix is a power of two better to do it exactly */
        radix_bits = 31 - clz_u32(radix);
        if (a < 0)
            a -= radix_bits - 1;
        return a / radix_bits;
    } else {
        mult = mul_log2_radix_table[radix - 2];
        return ((i64)a * mult) >> MUL_LOG2_RADIX_BASE_LOG2;
    }
}

#if 0
static void build_mul_log2_radix_table(void)
{
    i32 base, radix, mult, col, base_log2;

    base_log2 = 24;
    base = 1 << base_log2;
    col = 0;
    for(radix = 2; radix <= 36; radix++) {
        if ((radix & (radix - 1)) == 0)
            mult = 0;
        else
            mult = lrint((f64)base / log2(radix));
        printf("0x%06x, ", mult);
        if (++col == 4) {
            printf("\n");
            col = 0;
        }
    }
    printf("\n");
}

static void mul_log2_radix_test(void)
{
    i32 radix, i, ref, r;
    
    for(radix = 2; radix <= 36; radix++) {
        for(i = -2048; i <= 2047; i++) {
            ref = (i32)floor((f64)i / log2(radix));
            r = mul_log2_radix(i, radix);
            if (ref != r) {
                printf("ERROR: radix=%d i=%d r=%d ref=%d\n",
                       radix, i, r, ref);
                exit(1);
            }
        }
    }
    if (0)
        build_mul_log2_radix_table();
}
#endif

static void u32toa_len(u8* buf, u32 n, usize len) {
    i32 digit, i;
    for (i = len - 1; i >= 0; i--) {
        digit = n % 10;
        n = n / 10;
        buf[i] = digit + '0';
    }
}

/* for power of 2 radixes. len >= 1 */
static void u64toa_bin_len(u8* buf, u64 n, u32 radix_bits, i32 len) {
    i32 digit, i;
    u32 mask;

    mask = (1 << radix_bits) - 1;
    for (i = len - 1; i >= 0; i--) {
        digit = n & mask;
        n >>= radix_bits;
        if (digit < 10)
            digit += '0';
        else
            digit += 'a' - 10;
        buf[i] = digit;
    }
}

/* len >= 1. 2 <= radix <= 36 */
static void limb_to_a(u8* buf, limb_t n, u32 radix, i32 len) {
    i32 digit, i;

    if (radix == 10) {
        /* specific case with constant divisor */
#if LIMB_BITS == 32
        u32toa_len(buf, n, len);
#else
        /* XXX: optimize */
        for (i = len - 1; i >= 0; i--) {
            digit = (limb_t)n % 10;
            n = (limb_t)n / 10;
            buf[i] = digit + '0';
        }
#endif
    } else {
        for (i = len - 1; i >= 0; i--) {
            digit = (limb_t)n % radix;
            n = (limb_t)n / radix;
            if (digit < 10)
                digit += '0';
            else
                digit += 'a' - 10;
            buf[i] = digit;
        }
    }
}

usize cvm_u32toa(u8* buf, u32 n) {
    u8 buf1[10], *q;
    usize len;

    q = buf1 + sizeof(buf1);
    do {
        *--q = n % 10 + '0';
        n /= 10;
    } while (n != 0);
    len = buf1 + sizeof(buf1) - q;
    memcpy(buf, q, len);
    return len;
}

usize cvm_i32toa(u8* buf, i32 n) {
    if (n >= 0) {
        return cvm_u32toa(buf, n);
    } else {
        buf[0] = '-';
        return cvm_u32toa(buf + 1, -(u32)n) + 1;
    }
}

#ifdef USE_FAST_INT
usize cvm_u64toa(u8* buf, u64 n) {
    if (n < 0x100000000) {
        return cvm_u32toa(buf, n);
    } else {
        u64 n1;
        u8* q = buf;
        u32 n2;

        n1 = n / 1000000000;
        n %= 1000000000;
        if (n1 >= 0x100000000) {
            n2 = n1 / 1000000000;
            n1 = n1 % 1000000000;
            /* at most two digits */
            if (n2 >= 10) {
                *q++ = n2 / 10 + '0';
                n2 %= 10;
            }
            *q++ = n2 + '0';
            u32toa_len(q, n1, 9);
            q += 9;
        } else {
            q += cvm_u32toa(q, n1);
        }
        u32toa_len(q, n, 9);
        q += 9;
        return q - buf;
    }
}

usize cvm_i64toa(u8* buf, i64 n) {
    if (n >= 0) {
        return cvm_u64toa(buf, n);
    } else {
        buf[0] = '-';
        return cvm_u64toa(buf + 1, -(u64)n) + 1;
    }
}

/* XXX: only tested for 1 <= n < 2^53 */
usize cvm_u64toa_radix(u8* buf, u64 n, u32 radix) {
    i32 radix_bits, l;
    if (CVM_LIKELY(radix == 10))
        return cvm_u64toa(buf, n);
    if ((radix & (radix - 1)) == 0) {
        radix_bits = 31 - clz_u32(radix);
        if (n == 0)
            l = 1;
        else
            l = (64 - clz_u64(n) + radix_bits - 1) / radix_bits;
        u64toa_bin_len(buf, n, radix_bits, l);
        return l;
    } else {
        u8 buf1[41], *q; /* maximum length for radix = 3 */
        usize len;
        i32 digit;
        q = buf1 + sizeof(buf1);
        do {
            digit = n % radix;
            n /= radix;
            if (digit < 10)
                digit += '0';
            else
                digit += 'a' - 10;
            *--q = digit;
        } while (n != 0);
        len = buf1 + sizeof(buf1) - q;
        memcpy(buf, q, len);
        return len;
    }
}

usize cvm_i64toa_radix(u8* buf, i64 n, u32 radix) {
    if (n >= 0) {
        return cvm_u64toa_radix(buf, n, radix);
    } else {
        buf[0] = '-';
        return cvm_u64toa_radix(buf + 1, -(u64)n, radix) + 1;
    }
}
#endif /* USE_FAST_INT */

static const u8 digits_per_limb_table[CVM_RADIX_MAX - 1] = {
#if LIMB_BITS == 32
    32, 20, 16, 13, 12, 11, 10, 10, 9, 9, 8, 8, 8, 8, 8, 7, 7, 7,
    7,  7,  7,  7,  6,  6,  6,  6,  6, 6, 6, 6, 6, 6, 6, 6, 6,
#else
    64, 40, 32, 27, 24, 22, 21, 20, 19, 18, 17, 17, 16, 16, 16, 15, 15, 15,
    14, 14, 14, 14, 13, 13, 13, 13, 13, 13, 13, 12, 12, 12, 12, 12, 12,
#endif
};

static const u32 radix_base_table[CVM_RADIX_MAX - 1] = {
    0x00000000, 0xcfd41b91, 0x00000000, 0x48c27395, 0x81bf1000, 0x75db9c97, 0x40000000,
    0xcfd41b91, 0x3b9aca00, 0x8c8b6d2b, 0x19a10000, 0x309f1021, 0x57f6c100, 0x98c29b81,
    0x00000000, 0x18754571, 0x247dbc80, 0x3547667b, 0x4c4b4000, 0x6b5a6e1d, 0x94ace180,
    0xcaf18367, 0x0b640000, 0x0e8d4a51, 0x1269ae40, 0x17179149, 0x1cb91000, 0x23744899,
    0x2b73a840, 0x34e63b41, 0x40000000, 0x4cfa3cc1, 0x5c13d840, 0x6d91b519, 0x81bf1000,
};

/* XXX: remove the table ? */
static u8 dtoa_max_digits_table[CVM_RADIX_MAX - 1] = {
    54, 35, 28, 24, 22, 20, 19, 18, 17, 17, 16, 16, 15, 15, 15, 14, 14, 14,
    14, 14, 13, 13, 13, 13, 13, 13, 13, 12, 12, 12, 12, 12, 12, 12, 12,
};

/* we limit the maximum number of significant digits for atod to about
   128 bits of precision for non power of two bases. The only
   requirement for Javascript is at least 20 digits in base 10. For
   power of two bases, we do an exact rounding in all the cases. */
static u8 atod_max_digits_table[CVM_RADIX_MAX - 1] = {
    64, 80, 32, 55, 49, 45, 21, 40, 38, 37, 35, 34, 33, 32, 16, 31, 30, 30,
    29, 29, 28, 28, 27, 27, 27, 26, 26, 26, 26, 25, 12, 25, 25, 24, 24,
};

/* if abs_i32(d) >= B^max_exponent, it is an overflow */
static const i16 max_exponent[CVM_RADIX_MAX - 1] = {
    1024, 647, 512, 442, 397, 365, 342, 324, 309, 297, 286, 277,
    269,  263, 256, 251, 246, 242, 237, 234, 230, 227, 224, 221,
    218,  216, 214, 211, 209, 207, 205, 203, 202, 200, 199,
};

/* if abs_i32(d) <= B^min_exponent, it is an underflow */
static const i16 min_exponent[CVM_RADIX_MAX - 1] = {
    -1075, -679, -538, -463, -416, -383, -359, -340, -324, -311, -300, -291,
    -283,  -276, -269, -263, -258, -254, -249, -245, -242, -238, -235, -232,
    -229,  -227, -224, -222, -220, -217, -215, -214, -212, -210, -208,
};

#if 0
void build_tables(void)
{
    i32 r, j, radix, n, col, i;
    
    /* radix_base_table */
    for(radix = 2; radix <= 36; radix++) {
        r = 1;
        for(j = 0; j < digits_per_limb_table[radix - 2]; j++) {
            r *= radix;
        }
        printf(" 0x%08x,", r);
        if ((radix % 4) == 1)
            printf("\n");
    }
    printf("\n");

    /* dtoa_max_digits_table */
    for(radix = 2; radix <= 36; radix++) {
        /* Note: over estimated when the radix is a power of two */
        printf(" %d,", 1 + (i32)ceil(53.0 / log2(radix)));
    }
    printf("\n");

    /* atod_max_digits_table */
    for(radix = 2; radix <= 36; radix++) {
        if ((radix & (radix - 1)) == 0) {
            /* 64 bits is more than enough */
            n = (i32)floor(64.0 / log2(radix));
        } else {
            n = (i32)floor(128.0 / log2(radix));
        }
        printf(" %d,", n);
    }
    printf("\n");

    printf("static const i16 max_exponent[CVM_RADIX_MAX - 1] = {\n");
    col = 0;
    for(radix = 2; radix <= 36; radix++) {
        printf("%5d, ", (i32)ceil(1024 / log2(radix)));
        if (++col == 8) {
            col = 0;
            printf("\n");
        }
    }
    printf("\n};\n\n");

    printf("static const i16 min_exponent[CVM_RADIX_MAX - 1] = {\n");
    col = 0; 
    for(radix = 2; radix <= 36; radix++) {
        printf("%5d, ", (i32)floor(-1075 / log2(radix)));
        if (++col == 8) {
            col = 0;
            printf("\n");
        }
    }
    printf("\n};\n\n");

    printf("static const u32 pow5_table[16] = {\n");
    col = 0; 
    for(i = 2; i <= 17; i++) {
        r = 1;
        for(j = 0; j < i; j++) {
            r *= 5;
        }
        printf("0x%08x, ", r);
        if (++col == 4) {
            col = 0;
            printf("\n");
        }
    }
    printf("\n};\n\n");

    /* high part */
    printf("static const u8 pow5h_table[4] = {\n");
    col = 0; 
    for(i = 14; i <= 17; i++) {
        u64 r1;
        r1 = 1;
        for(j = 0; j < i; j++) {
            r1 *= 5;
        }
        printf("0x%08x, ", (u32)(r1 >> 32));
        if (++col == 4) {
            col = 0;
            printf("\n");
        }
    }
    printf("\n};\n\n");
}
#endif

/* n_digits >= 1. 0 <= dot_pos <= n_digits. If dot_pos == n_digits,
   the dot is not displayed. 'a' is modified. */
static i32 output_digits(u8* buf, mpb_t* a, i32 radix, i32 n_digits1, i32 dot_pos) {
    i32 n_digits, digits_per_limb, radix_bits, n, len;

    n_digits = n_digits1;
    if ((radix & (radix - 1)) == 0) {
        /* radix = 2^radix_bits */
        radix_bits = 31 - clz_u32(radix);
    } else {
        radix_bits = 0;
    }
    digits_per_limb = digits_per_limb_table[radix - 2];
    if (radix_bits != 0) {
        for (;;) {
            n = min_i32(n_digits, digits_per_limb);
            n_digits -= n;
            u64toa_bin_len(buf + n_digits, a->tab[0], radix_bits, n);
            if (n_digits == 0)
                break;
            mpb_shr_round(a, digits_per_limb * radix_bits, CVM_RNDZ);
        }
    } else {
        limb_t r;
        while (n_digits != 0) {
            n = min_i32(n_digits, digits_per_limb);
            n_digits -= n;
            r = mp_div1(a->tab, a->tab, a->len, radix_base_table[radix - 2], 0);
            mpb_renorm(a);
            limb_to_a(buf + n_digits, r, radix, n);
        }
    }

    /* add the dot */
    len = n_digits1;
    if (dot_pos != n_digits1) {
        memmove(buf + dot_pos + 1, buf + dot_pos, n_digits1 - dot_pos);
        buf[dot_pos] = '.';
        len++;
    }
    return len;
}

/* return (a, e_offset) such that a = a * (radix1*2^radix_shift)^f *
   2^-e_offset. 'f' can be negative. */
static i32 mul_pow(mpb_t* a, i32 radix1, i32 radix_shift, i32 f, bool is_int, i32 e) {
    i32 e_offset, d, n, n0;

    e_offset = -f * radix_shift;
    if (radix1 != 1) {
        d = digits_per_limb_table[radix1 - 2];
        if (f >= 0) {
            limb_t h, b;

            b = 0;
            n0 = 0;
            while (f != 0) {
                n = min_i32(f, d);
                if (n != n0) {
                    b = pow_ui(radix1, n);
                    n0 = n;
                }
                h = mp_mul1(a->tab, a->tab, a->len, b, 0);
                if (h != 0) {
                    a->tab[a->len++] = h;
                }
                f -= n;
            }
        } else {
            i32 extra_bits, l, shift;
            limb_t r, rem, b, b_inv;

            f = -f;
            l = (f + d - 1) /
                d; /* high bound for the number of limbs (XXX: make it better) */
            e_offset += l * LIMB_BITS;
            if (!is_int) {
                /* at least 'e' bits are needed in the final result for rounding */
                extra_bits = max_i32(e - mpb_floor_log2(a), 0);
            } else {
                /* at least two extra bits are needed in the final result
                   for rounding */
                extra_bits = max_i32(2 + e - e_offset, 0);
            }
            e_offset += extra_bits;
            mpb_shr_round(a, -(l * LIMB_BITS + extra_bits), CVM_RNDZ);

            b = 0;
            b_inv = 0;
            shift = 0;
            n0 = 0;
            rem = 0;
            while (f != 0) {
                n = min_i32(f, d);
                if (n != n0) {
                    b = pow_ui_inv(&b_inv, &shift, radix1, n);
                    n0 = n;
                }
                r = mp_div1norm(a->tab, a->tab, a->len, b, 0, b_inv, shift);
                rem |= r;
                mpb_renorm(a);
                f -= n;
            }
            /* if the remainder is non zero, use it for rounding */
            a->tab[0] |= (rem != 0);
        }
    }
    return e_offset;
}

/* tmp1 = round(m*2^e*radix^f). 'tmp0' is a temporary storage */
static void mul_pow_round(mpb_t* tmp1, u64 m, i32 e, i32 radix1, i32 radix_shift, i32 f,
                          i32 rnd_mode) {
    i32 e_offset;

    mpb_set_u64(tmp1, m);
    e_offset = mul_pow(tmp1, radix1, radix_shift, f, true, e);
    mpb_shr_round(tmp1, -e + e_offset, rnd_mode);
}

/* return round(a*2^e_offset) rounded as a float64. 'a' is modified */
static u64 round_to_d(i32* pe, mpb_t* a, i32 e_offset, i32 rnd_mode) {
    i32 e;
    u64 m;

    if (a->tab[0] == 0 && a->len == 1) {
        /* zero result */
        m = 0;
        e = 0; /* don't care */
    } else {
        i32 prec, prec1, e_min;
        e = mpb_floor_log2(a) + 1 - e_offset;
        prec1 = 53;
        e_min = -1021;
        if (e < e_min) {
            /* subnormal result or zero */
            prec = prec1 - (e_min - e);
        } else {
            prec = prec1;
        }
        mpb_shr_round(a, e + e_offset - prec, rnd_mode);
        m = mpb_get_u64(a);
        m <<= (53 - prec);
        /* mantissa overflow due to rounding */
        if (m >= (u64)1 << 53) {
            m >>= 1;
            e++;
        }
    }
    *pe = e;
    return m;
}

/* return (m, e) such that m*2^(e-53) = round(a * radix^f) with 2^52
   <= m < 2^53 or m = 0.
   'a' is modified. */
static u64 mul_pow_round_to_d(i32* pe, mpb_t* a, i32 radix1, i32 radix_shift, i32 f,
                              i32 rnd_mode) {
    i32 e_offset;

    e_offset = mul_pow(a, radix1, radix_shift, f, false, 55);
    return round_to_d(pe, a, e_offset, rnd_mode);
}

/* return a maximum bound of the string length. The bound depends on
   'd' only if format = CVM_DTOA_FORMAT_FRAC or if CVM_DTOA_EXP_DISABLED
   is enabled. */
i32 cvm_dtoa_max_len(f64 d, i32 radix, i32 n_digits, u32 flags) {
    i32 fmt = flags & CVM_DTOA_FORMAT_MASK;
    i32 n, e;
    u64 a;

    if (fmt != CVM_DTOA_FORMAT_FRAC) {
        if (fmt == CVM_DTOA_FORMAT_FREE) {
            n = dtoa_max_digits_table[radix - 2];
        } else {
            n = n_digits;
        }
        if ((flags & CVM_DTOA_EXP_MASK) == CVM_DTOA_EXP_DISABLED) {
            /* no exponential */
            a = f64_as_u64(d);
            e = (a >> 52) & 0x7ff;
            if (e == 0x7ff) {
                /* NaN, Infinity */
                n = 0;
            } else {
                e -= 1023;
                /* XXX: adjust */
                n += 10 + abs_i32(mul_log2_radix(e - 1, radix));
            }
        } else {
            /* extra: sign, 1 dot and exponent "e-1000" */
            n += 1 + 1 + 6;
        }
    } else {
        a = f64_as_u64(d);
        e = (a >> 52) & 0x7ff;
        if (e == 0x7ff) {
            /* NaN, Infinity */
            n = 0;
        } else {
            /* high bound for the integer part */
            e -= 1023;
            /* x < 2^(e + 1) */
            if (e < 0) {
                n = 1;
            } else {
                n = 2 + mul_log2_radix(e - 1, radix);
            }
            /* sign, extra digit, 1 dot */
            n += 1 + 1 + 1 + n_digits;
        }
    }
    return max_i32(n, 9); /* also include NaN and [-]Infinity */
}

#if defined(__SANITIZE_ADDRESS__) && 0
static void* dtoa_malloc(u64** pptr, usize size) {
    return malloc(size);
}
static void dtoa_free(void* ptr) {
    free(ptr);
}
#else
static void* dtoa_malloc(u64** pptr, usize size) {
    void* ret;
    ret = *pptr;
    *pptr += (size + 7) / 8;
    return ret;
}

static void dtoa_free(void* ptr) {
    /* The scratch is the caller's, so there is nothing to release. The
     * parameter exists because the matching allocator has one. */
    CVM_UNUSED(ptr);
}
#endif

/* return the length */
i32 cvm_dtoa(u8* buf, f64 d, i32 radix, i32 n_digits, u32 flags,
             CvmDtoaTempMem* tmp_mem) {
    u64 a, m, *mptr = tmp_mem->mem;
    i32 e, sgn, l, E, P, i, E_max, radix1, radix_shift;
    u8* q;
    mpb_t *tmp1, *mant_max;
    i32 fmt = flags & CVM_DTOA_FORMAT_MASK;

    tmp1 = dtoa_malloc(&mptr, sizeof(mpb_t) + sizeof(limb_t) * DBIGNUM_LEN_MAX);
    mant_max = dtoa_malloc(&mptr, sizeof(mpb_t) + sizeof(limb_t) * MANT_LEN_MAX);
    cvm_assert((usize)(mptr - tmp_mem->mem) <=
               sizeof(CvmDtoaTempMem) / sizeof(mptr[0]));

    radix_shift = ctz_u32(radix);
    radix1 = radix >> radix_shift;
    a = f64_as_u64(d);
    sgn = a >> 63;
    e = (a >> 52) & 0x7ff;
    m = a & (((u64)1 << 52) - 1);
    q = buf;
    if (e == 0x7ff) {
        if (m == 0) {
            if (sgn)
                *q++ = '-';
            memcpy(q, "Infinity", 8);
            q += 8;
        } else {
            memcpy(q, "NaN", 3);
            q += 3;
        }
        goto done;
    } else if (e == 0) {
        if (m == 0) {
            tmp1->len = 1;
            tmp1->tab[0] = 0;
            E = 1;
            if (fmt == CVM_DTOA_FORMAT_FREE)
                P = 1;
            else if (fmt == CVM_DTOA_FORMAT_FRAC)
                P = n_digits + 1;
            else
                P = n_digits;
            /* "-0" is displayed as "0" if CVM_DTOA_MINUS_ZERO is not present */
            if (sgn && (flags & CVM_DTOA_MINUS_ZERO))
                *q++ = '-';
            goto output;
        }
        /* denormal number: convert to a normal number */
        l = clz_u64(m) - 11;
        e -= l - 1;
        m <<= l;
    } else {
        m |= (u64)1 << 52;
    }
    if (sgn)
        *q++ = '-';
    /* remove the bias */
    e -= 1022;
    /* d = 2^(e-53)*m */
    //    printf("m=0x%016" PRIx64 " e=%d\n", m, e);
#ifdef USE_FAST_INT
    if (fmt == CVM_DTOA_FORMAT_FREE && e >= 1 && e <= 53 &&
        (m & (((u64)1 << (53 - e)) - 1)) == 0 &&
        (flags & CVM_DTOA_EXP_MASK) != CVM_DTOA_EXP_ENABLED) {
        m >>= 53 - e;
        /* 'm' is never zero */
        q += cvm_u64toa_radix(q, m, radix);
        goto done;
    }
#endif

    /* this choice of E implies F=round(x*B^(P-E) is such as: 
       B^(P-1) <= F < 2.B^P. */
    E = 1 + mul_log2_radix(e - 1, radix);

    if (fmt == CVM_DTOA_FORMAT_FREE) {
        i32 P_max, E0, e1, E_found, P_found;
        u64 m1, mant_found, mant, mant_max1;
        /* P_max is guarranteed to work by construction */
        P_max = dtoa_max_digits_table[radix - 2];
        E0 = E;
        E_found = 0;
        P_found = 0;
        mant_found = 0;
        /* find the minimum number of digits by successive tries */
        P = P_max; /* P_max is guarateed to work */
        for (;;) {
            /* mant_max always fits on 64 bits */
            mant_max1 = pow_ui(radix, P);
            /* compute the mantissa in base B */
            E = E0;
            for (;;) {
                /* XXX: add inexact flag */
                mul_pow_round(tmp1, m, e - 53, radix1, radix_shift, P - E, CVM_RNDN);
                mant = mpb_get_u64(tmp1);
                if (mant < mant_max1)
                    break;
                E++; /* at most one iteration is possible */
            }
            /* remove useless trailing zero digits */
            while ((mant % radix) == 0) {
                mant /= radix;
                P--;
            }
            /* garanteed to work for P = P_max */
            if (P_found == 0)
                goto prec_found;
            /* convert back to base 2 */
            mpb_set_u64(tmp1, mant);
            m1 = mul_pow_round_to_d(&e1, tmp1, radix1, radix_shift, E - P, CVM_RNDN);
            //            printf("P=%2d: m=0x%016" PRIx64 " e=%d m1=0x%016" PRIx64 " e1=%d\n", P, m, e, m1, e1);
            /* Note: (m, e) is never zero here, so the exponent for m1
               = 0 does not matter */
            if (m1 == m && e1 == e) {
prec_found:
                P_found = P;
                E_found = E;
                mant_found = mant;
                if (P == 1)
                    break;
                P--; /* try lower exponent */
            } else {
                break;
            }
        }
        P = P_found;
        E = E_found;
        mpb_set_u64(tmp1, mant_found);
    } else if (fmt == CVM_DTOA_FORMAT_FRAC) {
        i32 len;

        cvm_assert(n_digits >= 0 && n_digits <= CVM_DTOA_MAX_DIGITS);
        /* P = max_i32(E, 1) + n_digits; */
        /* frac is rounded using RNDNA */
        mul_pow_round(tmp1, m, e - 53, radix1, radix_shift, n_digits, CVM_RNDNA);

        /* we add one extra digit on the left and remove it if needed
           to avoid testing if the result is < radix^P */
        len = output_digits(q, tmp1, radix, max_i32(E + 1, 1) + n_digits,
                            max_i32(E + 1, 1));
        if (q[0] == '0' && len >= 2 && q[1] != '.') {
            len--;
            memmove(q, q + 1, len);
        }
        q += len;
        goto done;
    } else {
        i32 pow_shift;
        cvm_assert(n_digits >= 1 && n_digits <= CVM_DTOA_MAX_DIGITS);
        P = n_digits;
        /* mant_max = radix^P */
        mant_max->len = 1;
        mant_max->tab[0] = 1;
        pow_shift = mul_pow(mant_max, radix1, radix_shift, P, false, 0);
        mpb_shr_round(mant_max, pow_shift, CVM_RNDZ);

        for (;;) {
            /* fixed and frac are rounded using RNDNA */
            mul_pow_round(tmp1, m, e - 53, radix1, radix_shift, P - E, CVM_RNDNA);
            if (mpb_cmp(tmp1, mant_max) < 0)
                break;
            E++; /* at most one iteration is possible */
        }
    }
output:
    if (fmt == CVM_DTOA_FORMAT_FIXED)
        E_max = n_digits;
    else
        E_max = dtoa_max_digits_table[radix - 2] + 4;
    if ((flags & CVM_DTOA_EXP_MASK) == CVM_DTOA_EXP_ENABLED ||
        ((flags & CVM_DTOA_EXP_MASK) == CVM_DTOA_EXP_AUTO && (E <= -6 || E > E_max))) {
        q += output_digits(q, tmp1, radix, P, 1);
        E--;
        if (radix == 10) {
            *q++ = 'e';
        } else if (radix1 == 1 && radix_shift <= 4) {
            E *= radix_shift;
            *q++ = 'p';
        } else {
            *q++ = '@';
        }
        if (E < 0) {
            *q++ = '-';
            E = -E;
        } else {
            *q++ = '+';
        }
        q += cvm_u32toa(q, E);
    } else if (E <= 0) {
        *q++ = '0';
        *q++ = '.';
        for (i = 0; i < -E; i++)
            *q++ = '0';
        q += output_digits(q, tmp1, radix, P, P);
    } else {
        q += output_digits(q, tmp1, radix, P, min_i32(P, E));
        for (i = 0; i < E - P; i++)
            *q++ = '0';
    }
done:
    *q = '\0';
    dtoa_free(mant_max);
    dtoa_free(tmp1);
    return q - buf;
}

static inline i32 to_digit(i32 c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    else if (c >= 'A' && c <= 'Z')
        return c - 'A' + 10;
    else if (c >= 'a' && c <= 'z')
        return c - 'a' + 10;
    else
        return 36;
}

/* r = r * radix_base + a. radix_base = 0 means radix_base = 2^32 */
static void mpb_mul1_base(mpb_t* r, limb_t radix_base, limb_t a) {
    i32 i;
    if (r->tab[0] == 0 && r->len == 1) {
        r->tab[0] = a;
    } else {
        if (radix_base == 0) {
            for (i = r->len; i >= 0; i--) {
                r->tab[i + 1] = r->tab[i];
            }
            r->tab[0] = a;
        } else {
            r->tab[r->len] = mp_mul1(r->tab, r->tab, r->len, radix_base, a);
        }
        r->len++;
        mpb_renorm(r);
    }
}

/* XXX: add fast path for small integers */
f64 cvm_atod(str s, usize* consumed, i32 radix, u32 flags, CvmAtodTempMem* tmp_mem) {
    u64* mptr = tmp_mem->mem;
    u8 const *p, *p_start;
    u8 const* end = s.ptr + s.len;
    limb_t cur_limb, radix_base, extra_digits;
    i32 is_neg, digit_count, limb_digit_count, digits_per_limb, sep, radix1,
        radix_shift;
    i32 radix_bits, expn, e, max_digits, expn_offset, dot_pos, sig_pos, pos;
    mpb_t* tmp0;
    f64 dval;
    bool is_bin_exp, is_zero, expn_overflow;
    u64 m, a;

    tmp0 = dtoa_malloc(&mptr, sizeof(mpb_t) + sizeof(limb_t) * DBIGNUM_LEN_MAX);
    cvm_assert((usize)(mptr - tmp_mem->mem) <=
               sizeof(CvmAtodTempMem) / sizeof(mptr[0]));
    /* optional separator between digits */
    sep = (flags & CVM_ATOD_ACCEPT_UNDERSCORES) ? '_' : 256;

    p = s.ptr;
    is_neg = 0;
    if (peek(p, end, 0) == '+') {
        p++;
        p_start = p;
    } else if (peek(p, end, 0) == '-') {
        is_neg = 1;
        p++;
        p_start = p;
    } else {
        p_start = p;
    }

    if (peek(p, end, 0) == '0') {
        if ((peek(p, end, 1) == 'x' || peek(p, end, 1) == 'X') &&
            (radix == 0 || radix == 16)) {
            p += 2;
            radix = 16;
        } else if ((peek(p, end, 1) == 'o' || peek(p, end, 1) == 'O') && radix == 0 &&
                   (flags & CVM_ATOD_ACCEPT_BIN_OCT)) {
            p += 2;
            radix = 8;
        } else if ((peek(p, end, 1) == 'b' || peek(p, end, 1) == 'B') && radix == 0 &&
                   (flags & CVM_ATOD_ACCEPT_BIN_OCT)) {
            p += 2;
            radix = 2;
        } else if ((peek(p, end, 1) >= '0' && peek(p, end, 1) <= '9') && radix == 0 &&
                   (flags & CVM_ATOD_ACCEPT_LEGACY_OCTAL)) {
            i32 i;
            sep = 256;
            for (i = 1;
                 (peek(p, end, (usize)i) >= '0' && peek(p, end, (usize)i) <= '7'); i++)
                continue;
            if (peek(p, end, (usize)i) == '8' || peek(p, end, (usize)i) == '9')
                goto no_prefix;
            p += 1;
            radix = 8;
        } else {
            goto no_prefix;
        }
        /* there must be a digit after the prefix */
        if (to_digit((u8)peek(p, end, 0)) >= radix)
            goto fail;
no_prefix:;
    } else {
        if (!(flags & CVM_ATOD_INT_ONLY) &&
            str_startswith(p, end, (u8 const*)"Infinity", 8, &p))
            goto overflow;
    }
    if (radix == 0)
        radix = 10;

    cur_limb = 0;
    expn_offset = 0;
    digit_count = 0;
    limb_digit_count = 0;
    max_digits = atod_max_digits_table[radix - 2];
    digits_per_limb = digits_per_limb_table[radix - 2];
    radix_base = radix_base_table[radix - 2];
    radix_shift = ctz_u32(radix);
    radix1 = radix >> radix_shift;
    if (radix1 == 1) {
        /* radix = 2^radix_bits */
        radix_bits = radix_shift;
    } else {
        radix_bits = 0;
    }
    tmp0->len = 1;
    tmp0->tab[0] = 0;
    extra_digits = 0;
    pos = 0;
    dot_pos = -1;
    /* skip leading zeros */
    for (;;) {
        if (peek(p, end, 0) == '.' &&
            (p > p_start || to_digit(peek(p, end, 1)) < radix) &&
            !(flags & CVM_ATOD_INT_ONLY)) {
            if (peek(p, end, 0) == sep)
                goto fail;
            if (dot_pos >= 0)
                break;
            dot_pos = pos;
            p++;
        }
        if (peek(p, end, 0) == sep && p > p_start && peek(p, end, 1) == '0')
            p++;
        if (peek(p, end, 0) != '0')
            break;
        p++;
        pos++;
    }

    sig_pos = pos;
    for (;;) {
        i32 c;
        if (peek(p, end, 0) == '.' &&
            (p > p_start || to_digit(peek(p, end, 1)) < radix) &&
            !(flags & CVM_ATOD_INT_ONLY)) {
            if (peek(p, end, 0) == sep)
                goto fail;
            if (dot_pos >= 0)
                break;
            dot_pos = pos;
            p++;
        }
        if (peek(p, end, 0) == sep && p > p_start && to_digit(peek(p, end, 1)) < radix)
            p++;
        c = to_digit(peek(p, end, 0));
        if (c >= radix)
            break;
        p++;
        pos++;
        if (digit_count < max_digits) {
            /* XXX: could be faster when radix_bits != 0 */
            cur_limb = cur_limb * radix + c;
            limb_digit_count++;
            if (limb_digit_count == digits_per_limb) {
                mpb_mul1_base(tmp0, radix_base, cur_limb);
                cur_limb = 0;
                limb_digit_count = 0;
            }
            digit_count++;
        } else {
            extra_digits |= c;
        }
    }
    if (limb_digit_count != 0) {
        mpb_mul1_base(tmp0, pow_ui(radix, limb_digit_count), cur_limb);
    }
    if (digit_count == 0) {
        is_zero = true;
        expn_offset = 0;
    } else {
        is_zero = false;
        if (dot_pos < 0)
            dot_pos = pos;
        expn_offset = sig_pos + digit_count - dot_pos;
    }

    /* Use the extra digits for rounding if the base is a power of
       two. Otherwise they are just truncated. */
    if (radix_bits != 0 && extra_digits != 0) {
        tmp0->tab[0] |= 1;
    }

    /* parse the exponent, if any */
    expn = 0;
    expn_overflow = false;
    is_bin_exp = false;
    if (!(flags & CVM_ATOD_INT_ONLY) &&
        ((radix == 10 && (peek(p, end, 0) == 'e' || peek(p, end, 0) == 'E')) ||
         (radix != 10 && (peek(p, end, 0) == '@' ||
                          (radix_bits >= 1 && radix_bits <= 4 &&
                           (peek(p, end, 0) == 'p' || peek(p, end, 0) == 'P'))))) &&
        p > p_start) {
        bool exp_is_neg;
        i32 c;
        is_bin_exp = (peek(p, end, 0) == 'p' || peek(p, end, 0) == 'P');
        p++;
        exp_is_neg = false;
        if (peek(p, end, 0) == '+') {
            p++;
        } else if (peek(p, end, 0) == '-') {
            exp_is_neg = true;
            p++;
        }
        c = to_digit(peek(p, end, 0));
        if (c >= 10)
            goto fail; /* XXX: could stop before the exponent part */
        expn = c;
        p++;
        for (;;) {
            if (peek(p, end, 0) == sep && to_digit(peek(p, end, 1)) < 10)
                p++;
            c = to_digit(peek(p, end, 0));
            if (c >= 10)
                break;
            if (!expn_overflow) {
                if (CVM_UNLIKELY(expn > ((((i32)0x7fffffff) - 2 - 9) / 10))) {
                    expn_overflow = true;
                } else {
                    expn = expn * 10 + c;
                }
            }
            p++;
        }
        if (exp_is_neg)
            expn = -expn;
        /* if zero result, the exponent can be arbitrarily large */
        if (!is_zero && expn_overflow) {
            if (exp_is_neg)
                a = 0;
            else
                a = (u64)0x7ff << 52; /* infinity */
            goto done;
        }
    }

    if (p == p_start)
        goto fail;

    if (is_zero) {
        a = 0;
    } else {
        i32 expn1;
        if (radix_bits != 0) {
            if (!is_bin_exp)
                expn *= radix_bits;
            expn -= expn_offset * radix_bits;
            expn1 = expn + digit_count * radix_bits;
            if (expn1 >= 1024 + radix_bits)
                goto overflow;
            else if (expn1 <= -1075)
                goto underflow;
            m = round_to_d(&e, tmp0, -expn, CVM_RNDN);
        } else {
            expn -= expn_offset;
            expn1 = expn + digit_count;
            if (expn1 >= max_exponent[radix - 2] + 1)
                goto overflow;
            else if (expn1 <= min_exponent[radix - 2])
                goto underflow;
            m = mul_pow_round_to_d(&e, tmp0, radix1, radix_shift, expn, CVM_RNDN);
        }
        if (m == 0) {
underflow:
            a = 0;
        } else if (e > 1024) {
overflow:
            /* overflow */
            a = (u64)0x7ff << 52;
        } else if (e < -1073) {
            /* underflow */
            /* XXX: check rounding */
            a = 0;
        } else if (e < -1021) {
            /* subnormal */
            a = m >> (-e - 1021);
        } else {
            a = ((u64)(e + 1022) << 52) | (m & (((u64)1 << 52) - 1));
        }
    }
done:
    a |= (u64)is_neg << 63;
    dval = u64_as_f64(a);
done1:
    if (consumed != NULL)
        *consumed = (usize)(p - s.ptr);
    dtoa_free(tmp0);
    return dval;
fail:
    dval = f64_nan();
    goto done1;
}
