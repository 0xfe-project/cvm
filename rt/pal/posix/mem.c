/*
 * rt/pal/posix/mem.c
 *
 * General allocation, POSIX implementation.
 *
 * Backed by libc malloc. That is deliberate and unremarkable: rt is the
 * one layer allowed to use libc, and this is for runtime bookkeeping, not
 * for GC heap objects. The heap itself gets one large reservation through
 * a separate call, because a heap whose blocks are individually malloc'd
 * cannot be described by address arithmetic.
 */

#if !defined(_WIN32)

#include <stdlib.h>

#include "rt.h"

opaque_mut rt_mem_alloc(usize bytes) {
    if (bytes == 0) {
        return NULL;
    }
    return malloc(bytes);
}

void rt_mem_free(opaque_mut p) {
    free(p);
}

#endif /* !_WIN32 */
