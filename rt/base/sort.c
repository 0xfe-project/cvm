/*
 * rt/base/sort.c
 *
 * The sort. See rt/base/sort.h for why it is here and what it is not.
 */

#include "base/sort.h"

#include "base/types.h"

typedef void (*CvmSortSwap)(opaque_mut a, opaque_mut b, usize size);

/*---- sorting with ctx argument ----*/

typedef void (*CvmSortSwap)(void* a, void* b, usize size);

static void exchange_bytes(void* a, void* b, usize size) {
    u8* ap = (u8*)a;
    u8* bp = (u8*)b;

    while (size-- != 0) {
        u8 t = *ap;
        *ap++ = *bp;
        *bp++ = t;
    }
}

static void exchange_one_byte(void* a, void* b, usize size) {
    CVM_UNUSED(size);
    /* The size is in the signature to match the other swappers; one byte needs
     * no loop. */
    CVM_UNUSED(size);
    u8* ap = (u8*)a;
    u8* bp = (u8*)b;
    u8 t = *ap;
    *ap = *bp;
    *bp = t;
}

static void exchange_int16s(void* a, void* b, usize size) {
    u16* ap = (u16*)a;
    u16* bp = (u16*)b;

    for (size /= sizeof(u16); size-- != 0;) {
        u16 t = *ap;
        *ap++ = *bp;
        *bp++ = t;
    }
}

static void exchange_one_int16(void* a, void* b, usize size) {
    CVM_UNUSED(size);
    u16* ap = (u16*)a;
    u16* bp = (u16*)b;
    u16 t = *ap;
    *ap = *bp;
    *bp = t;
}

static void exchange_int32s(void* a, void* b, usize size) {
    u32* ap = (u32*)a;
    u32* bp = (u32*)b;

    for (size /= sizeof(u32); size-- != 0;) {
        u32 t = *ap;
        *ap++ = *bp;
        *bp++ = t;
    }
}

static void exchange_one_int32(void* a, void* b, usize size) {
    CVM_UNUSED(size);
    u32* ap = (u32*)a;
    u32* bp = (u32*)b;
    u32 t = *ap;
    *ap = *bp;
    *bp = t;
}

static void exchange_int64s(void* a, void* b, usize size) {
    u64* ap = (u64*)a;
    u64* bp = (u64*)b;

    for (size /= sizeof(u64); size-- != 0;) {
        u64 t = *ap;
        *ap++ = *bp;
        *bp++ = t;
    }
}

static void exchange_one_int64(void* a, void* b, usize size) {
    CVM_UNUSED(size);
    u64* ap = (u64*)a;
    u64* bp = (u64*)b;
    u64 t = *ap;
    *ap = *bp;
    *bp = t;
}

static void exchange_int128s(void* a, void* b, usize size) {
    u64* ap = (u64*)a;
    u64* bp = (u64*)b;

    for (size /= sizeof(u64) * 2; size-- != 0; ap += 2, bp += 2) {
        u64 t = ap[0];
        u64 u = ap[1];
        ap[0] = bp[0];
        ap[1] = bp[1];
        bp[0] = t;
        bp[1] = u;
    }
}

static void exchange_one_int128(void* a, void* b, usize size) {
    CVM_UNUSED(size);
    u64* ap = (u64*)a;
    u64* bp = (u64*)b;
    u64 t = ap[0];
    u64 u = ap[1];
    ap[0] = bp[0];
    ap[1] = bp[1];
    bp[0] = t;
    bp[1] = u;
}

static inline CvmSortSwap exchange_func(const void* base, usize size) {
    switch (((uintptr_t)base | (uintptr_t)size) & 15) {
    case 0:
        if (size == sizeof(u64) * 2)
            return exchange_one_int128;
        else
            return exchange_int128s;
    case 8:
        if (size == sizeof(u64))
            return exchange_one_int64;
        else
            return exchange_int64s;
    case 4:
    case 12:
        if (size == sizeof(u32))
            return exchange_one_int32;
        else
            return exchange_int32s;
    case 2:
    case 6:
    case 10:
    case 14:
        if (size == sizeof(u16))
            return exchange_one_int16;
        else
            return exchange_int16s;
    default:
        if (size == 1)
            return exchange_one_byte;
        else
            return exchange_bytes;
    }
}

static void heapsortx(void* base, usize nmemb, usize size, CvmSortCmp cmp,
                      opaque_mut ctx) {
    u8* basep = (u8*)base;
    usize i, n, c, r;
    CvmSortSwap swap = exchange_func(base, size);

    if (nmemb > 1) {
        i = (nmemb / 2) * size;
        n = nmemb * size;

        while (i > 0) {
            i -= size;
            for (r = i; (c = r * 2 + size) < n; r = c) {
                if (c < n - size && cmp(basep + c, basep + c + size, ctx) <= 0)
                    c += size;
                if (cmp(basep + r, basep + c, ctx) > 0)
                    break;
                swap(basep + r, basep + c, size);
            }
        }
        for (i = n - size; i > 0; i -= size) {
            swap(basep, basep + i, size);

            for (r = 0; (c = r * 2 + size) < i; r = c) {
                if (c < i - size && cmp(basep + c, basep + c + size, ctx) <= 0)
                    c += size;
                if (cmp(basep + r, basep + c, ctx) > 0)
                    break;
                swap(basep + r, basep + c, size);
            }
        }
    }
}

static inline void* med3(void* a, void* b, void* c, CvmSortCmp cmp, opaque_mut ctx) {
    return cmp(a, b, ctx) < 0 ? (cmp(b, c, ctx) < 0 ? b : (cmp(a, c, ctx) < 0 ? c : a))
                              : (cmp(b, c, ctx) > 0 ? b : (cmp(a, c, ctx) < 0 ? a : c));
}

/* pointer based version with local stack and insertion sort threshhold */
void cvm_rqsort(void* base, usize nmemb, usize size, CvmSortCmp cmp, opaque_mut ctx) {
    struct {
        u8* base;
        usize count;
        i32 depth;
    } stack[50], *sp = stack;
    u8 *ptr, *pi, *pj, *plt, *pgt, *top, *m;
    usize m4, i, lt, gt, span, span2;
    i32 c, depth;
    CvmSortSwap swap = exchange_func(base, size);
    CvmSortSwap swap_block = exchange_func(base, size | 128);

    if (nmemb < 2 || size <= 0)
        return;

    sp->base = (u8*)base;
    sp->count = nmemb;
    sp->depth = 0;
    sp++;

    while (sp > stack) {
        sp--;
        ptr = sp->base;
        nmemb = sp->count;
        depth = sp->depth;

        while (nmemb > 6) {
            if (++depth > 50) {
                /* depth check to ensure worst case logarithmic time */
                heapsortx(ptr, nmemb, size, cmp, ctx);
                nmemb = 0;
                break;
            }
            /* select median of 3 from 1/4, 1/2, 3/4 positions */
            /* should use median of 5 or 9? */
            m4 = (nmemb >> 2) * size;
            m = med3(ptr + m4, ptr + 2 * m4, ptr + 3 * m4, cmp, ctx);
            swap(ptr, m, size); /* move the pivot to the start or the array */
            i = lt = 1;
            pi = plt = ptr + size;
            gt = nmemb;
            pj = pgt = top = ptr + nmemb * size;
            for (;;) {
                while (pi < pj && (c = cmp(ptr, pi, ctx)) >= 0) {
                    if (c == 0) {
                        swap(plt, pi, size);
                        lt++;
                        plt += size;
                    }
                    i++;
                    pi += size;
                }
                while (pi < (pj -= size) && (c = cmp(ptr, pj, ctx)) <= 0) {
                    if (c == 0) {
                        gt--;
                        pgt -= size;
                        swap(pgt, pj, size);
                    }
                }
                if (pi >= pj)
                    break;
                swap(pi, pj, size);
                i++;
                pi += size;
            }
            /* array has 4 parts:
             * from 0 to lt excluded: elements identical to pivot
             * from lt to pi excluded: elements smaller than pivot
             * from pi to gt excluded: elements greater than pivot
             * from gt to n excluded: elements identical to pivot
             */
            /* move elements identical to pivot in the middle of the array: */
            /* swap values in ranges [0..lt[ and [i-lt..i[
               swapping the smallest span between lt and i-lt is sufficient
             */
            span = plt - ptr;
            span2 = pi - plt;
            lt = i - lt;
            if (span > span2)
                span = span2;
            swap_block(ptr, pi - span, span);
            /* swap values in ranges [gt..top[ and [i..top-(top-gt)[
               swapping the smallest span between top-gt and gt-i is sufficient
             */
            span = top - pgt;
            span2 = pgt - pi;
            pgt = top - span2;
            gt = nmemb - (gt - i);
            if (span > span2)
                span = span2;
            swap_block(pi, top - span, span);

            /* now array has 3 parts:
             * from 0 to lt excluded: elements smaller than pivot
             * from lt to gt excluded: elements identical to pivot
             * from gt to n excluded: elements greater than pivot
             */
            /* stack the larger segment and keep processing the smaller one
               to minimize stack use for pathological distributions */
            if (lt > nmemb - gt) {
                sp->base = ptr;
                sp->count = lt;
                sp->depth = depth;
                sp++;
                ptr = pgt;
                nmemb -= gt;
            } else {
                sp->base = pgt;
                sp->count = nmemb - gt;
                sp->depth = depth;
                sp++;
                nmemb = lt;
            }
        }
        /* Use insertion sort for small fragments */
        for (pi = ptr + size, top = ptr + nmemb * size; pi < top; pi += size) {
            for (pj = pi; pj > ptr && cmp(pj - size, pj, ctx) > 0; pj -= size)
                swap(pj, pj - size, size);
        }
    }
}
