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
#include <sys/mman.h>
#include <unistd.h>

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

/* ------------------------------------------------------------------ */
/* address space                                                       */
/* ------------------------------------------------------------------ */

opaque_mut rt_mem_reserve(usize bytes) {
    if (bytes == 0) {
        return NULL;
    }

    /*
     * MAP_NORESERVE: the heap reserves its maximum size up front and only
     * ever touches the part it is using. Without this the kernel would
     * account for the whole reservation against swap, so a process asking
     * for a large maximum heap would fail to start on a machine with
     * plenty of memory.
     *
     * Anonymous and private: nobody else sees it, and it starts zeroed,
     * which the heap relies on for its metadata.
     */
    void* p = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
}

bool rt_mem_commit(opaque_mut base, usize bytes) {
    /* Already writable: reserve mapped it read-write. The call exists so
     * that the ABI is the same shape on a platform where reserving and
     * committing are separate operations. */
    (void)base;
    (void)bytes;
    return true;
}

void rt_mem_release(opaque_mut base, usize bytes) {
    if (base == NULL || bytes == 0) {
        return;
    }
    munmap(base, bytes);
}

#endif /* !_WIN32 */
