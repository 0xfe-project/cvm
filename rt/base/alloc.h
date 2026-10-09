/*
 * rt/base/alloc.h
 *
 * The allocation contract the growing buffers take, and nothing else.
 *
 * Two functions and a context, rather than a realloc: our platform layer offers
 * alloc and free, and a callback with realloc's signature cannot be implemented
 * over those two without being told the old size — which the callback is not. A
 * structure that grows therefore has to know how much it holds, and it does.
 *
 * This is a type, not an implementation: the allocator comes from whoever makes
 * the buffer (the runtime's own is rt_mem_alloc and rt_mem_free), which is what
 * lets a buffer be used by code that must not depend on a particular heap.
 */

#ifndef RT_BASE_ALLOC_H
#define RT_BASE_ALLOC_H

#include "base/types.h"

typedef struct CvmAlloc {
    opaque_mut ctx;
    opaque_mut (*alloc)(opaque_mut ctx, usize bytes);
    void (*free)(opaque_mut ctx, opaque_mut p);
} CvmAlloc;

#endif /* RT_BASE_ALLOC_H */
