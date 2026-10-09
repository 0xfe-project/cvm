/*
 * rt/base/alloc.h
 *
 * The allocation contract the growing buffers take, and nothing else.
 *
 * Three functions and a context. The realloc is in it because every growable
 * structure in the runtime wants exactly that operation, and the alternative —
 * allocate, copy, free, once per structure — is the same code written several
 * times with a size each of them had to keep anyway.
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
    /*
     * Grow or shrink in place where the implementation can. A structure that
     * grows asks for this rather than allocating and copying, which is the same
     * operation with the size bookkeeping left to whoever knows it — and for a
     * buffer that grows by a little at a time, a platform that can extend in
     * place does no copy at all.
     */
    opaque_mut (*realloc)(opaque_mut ctx, opaque_mut p, usize bytes);
    void (*free)(opaque_mut ctx, opaque_mut p);
} CvmAlloc;

#endif /* RT_BASE_ALLOC_H */
