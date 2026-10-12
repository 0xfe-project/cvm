/*
 * vm/mem.c
 *
 * The four symbols the compiler is entitled to call in freestanding C.
 * See vm/mem.h for why they exist and why they are hidden.
 *
 * Byte loops, deliberately. __builtin_memcmp is not always inlinable, and
 * a call it cannot inline is a call to `memcmp` -- which is this function,
 * which is infinite recursion. The loops cannot recurse: the compiler may
 * vectorise them, but it will not turn them into a call to themselves.
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
