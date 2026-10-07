/*
 * vm/mem.c
 *
 * The four primitives. See vm/mem.h for why they exist and why they are
 * hidden.
 *
 * Written as plain byte loops. A compiler recognises this shape and turns
 * it back into whatever instruction sequence it wanted to emit in the
 * first place, so there is no need to hand-optimise; and a hand-written
 * word-at-a-time version would need to handle alignment and tails, which
 * is exactly the kind of code that is easy to get subtly wrong in a
 * runtime where a subtle mistake is invisible.
 */

#include "mem.h"

CVM_HIDDEN void* memcpy(void* restrict dst, void const* restrict src, usize n) {
    u8* d = dst;
    u8 const* s = src;
    for (usize i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

CVM_HIDDEN void* memmove(void* dst, void const* src, usize n) {
    u8* d = dst;
    u8 const* s = src;

    if (d == s || n == 0) {
        return dst;
    }

    /* Overlapping and moving up: copy backwards. */
    if (d < s) {
        for (usize i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (usize i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

CVM_HIDDEN void* memset(void* dst, int value, usize n) {
    u8* d = dst;
    u8 v = (u8)value;
    for (usize i = 0; i < n; i++) {
        d[i] = v;
    }
    return dst;
}

CVM_HIDDEN int memcmp(void const* a, void const* b, usize n) {
    u8 const* x = a;
    u8 const* y = b;
    for (usize i = 0; i < n; i++) {
        if (x[i] != y[i]) {
            return (x[i] < y[i]) ? -1 : 1;
        }
    }
    return 0;
}
