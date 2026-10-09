/*
 * vm/buf.c
 *
 * The growable byte buffer. See cvm/buf.h for why it is this and not a realloc
 * callback, and why it is not a string builder.
 */

#include "cvm/buf.h"

#include "base/assert.h"
#include "base/types.h"
#include "mem.h"
#include "rt.h"

/*
 * Where `data` sits inside this buffer, or CVM_BUF_NOT_INSIDE.
 *
 * Growing a buffer can move it, so a source that points into it would be read
 * from a freed block — and the caller has no way to know whether the append it
 * asked for will grow. Rather than document a rule that is easy to break, the
 * append finds out: the offset survives the move, the pointer does not.
 */
#define CVM_BUF_NOT_INSIDE ((usize)~0u)

static usize interior_offset(CvmBuf const* buf, opaque data) {
    u8 const* p = data;
    if (buf->data == NULL || p < buf->data || p >= buf->data + buf->size) {
        return CVM_BUF_NOT_INSIDE;
    }
    return (usize)(p - buf->data);
}

/* Copy a source that points into the buffer out of the way first, and return it
 * (or the original when it was already outside). The caller frees what comes
 * back if it differs from `data`. */
static opaque_mut detach_source(CvmBuf* buf, opaque data, usize bytes) {
    usize inside = interior_offset(buf, data);
    if (inside == CVM_BUF_NOT_INSIDE || bytes == 0u) {
        /* Nothing to detach: the source stays where it is, and the cast is only
         * because the helper hands back something the caller may free. */
        return (opaque_mut)data;
    }
    u8* temp = buf->alloc.alloc(buf->alloc.ctx, bytes);
    if (temp == NULL) {
        buf->error = true;
        return NULL;
    }
    memcpy(temp, buf->data + inside, bytes);
    return temp;
}

/* Not zero: an empty buffer is a valid one, and a caller that asks for room for
 * nothing should not get an allocation it then has to reason about. */
CVM_INLINE usize grown_capacity(usize allocated) {
    usize want = allocated == 0u ? 64u : allocated;
    return want + want / 2u;
}

bool cvm_buf_reserve(CvmBuf* buf, usize bytes) {
    if (buf->error) {
        return false;
    }
    if (bytes <= buf->allocated - buf->size) {
        return true;
    }

    usize want = buf->allocated;
    while (want - buf->size < bytes) {
        want = grown_capacity(want);
    }

    u8* fresh = buf->alloc.alloc(buf->alloc.ctx, want);
    if (fresh == NULL) {
        buf->error = true;
        return false;
    }
    if (buf->size != 0u) {
        memcpy(fresh, buf->data, buf->size);
    }
    if (buf->data != NULL) {
        buf->alloc.free(buf->alloc.ctx, buf->data);
    }
    buf->data = fresh;
    buf->allocated = want;
    return true;
}

void cvm_buf_init(CvmBuf* buf, CvmAlloc alloc) {
    buf->data = NULL;
    buf->size = 0;
    buf->allocated = 0;
    buf->error = false;
    buf->alloc = alloc;
}

void cvm_buf_free(CvmBuf* buf) {
    if (buf->data != NULL) {
        buf->alloc.free(buf->alloc.ctx, buf->data);
    }
    buf->data = NULL;
    buf->size = 0;
    buf->allocated = 0;
}

bool cvm_buf_put(CvmBuf* buf, opaque data, usize bytes) {
    opaque_mut source = detach_source(buf, data, bytes);
    if (source == NULL) {
        return false;
    }

    if (!cvm_buf_reserve(buf, bytes)) {
        if (source != data) {
            buf->alloc.free(buf->alloc.ctx, source);
        }
        return false;
    }
    if (bytes != 0u) {
        memcpy(buf->data + buf->size, source, bytes);
        buf->size += bytes;
    }
    if (source != data) {
        buf->alloc.free(buf->alloc.ctx, source);
    }
    return true;
}

bool cvm_buf_putc(CvmBuf* buf, u8 value) {
    return cvm_buf_put(buf, &value, 1u);
}

bool cvm_buf_put_u16(CvmBuf* buf, u16 value) {
    u8 bytes[2];
    bytes[0] = (u8)(value & 0xffu);
    bytes[1] = (u8)((value >> 8) & 0xffu);
    return cvm_buf_put(buf, bytes, 2u);
}

bool cvm_buf_put_u32(CvmBuf* buf, u32 value) {
    u8 bytes[4];
    bytes[0] = (u8)(value & 0xffu);
    bytes[1] = (u8)((value >> 8) & 0xffu);
    bytes[2] = (u8)((value >> 16) & 0xffu);
    bytes[3] = (u8)((value >> 24) & 0xffu);
    return cvm_buf_put(buf, bytes, 4u);
}

bool cvm_buf_put_u64(CvmBuf* buf, u64 value) {
    u8 bytes[8];
    for (u32 i = 0; i < 8u; i++) {
        bytes[i] = (u8)((value >> (8u * i)) & 0xffu);
    }
    return cvm_buf_put(buf, bytes, 8u);
}

bool cvm_buf_insert(CvmBuf* buf, usize pos, opaque data, usize bytes) {
    cvm_assert(pos <= buf->size);

    opaque_mut source = detach_source(buf, data, bytes);
    if (source == NULL) {
        return false;
    }

    if (!cvm_buf_reserve(buf, bytes)) {
        if (source != data) {
            buf->alloc.free(buf->alloc.ctx, source);
        }
        return false;
    }
    if (bytes == 0u) {
        if (source != data) {
            buf->alloc.free(buf->alloc.ctx, source);
        }
        return true;
    }

    /* Backwards from the end of what is there, so the source and the destination
     * may overlap — which the caller cannot always know they do not. */
    usize moved = buf->size - pos;
    for (usize i = moved; i > 0u; i--) {
        buf->data[pos + bytes + i - 1u] = buf->data[pos + i - 1u];
    }
    memcpy(buf->data + pos, source, bytes);
    buf->size += bytes;
    if (source != data) {
        buf->alloc.free(buf->alloc.ctx, source);
    }
    return true;
}

static opaque_mut system_alloc(opaque_mut ctx, usize bytes) {
    (void)ctx;
    return rt_mem_alloc(bytes);
}

static opaque_mut system_realloc(opaque_mut ctx, opaque_mut p, usize bytes) {
    (void)ctx;
    return rt_mem_realloc(p, bytes);
}

static void system_free(opaque_mut ctx, opaque_mut p) {
    (void)ctx;
    rt_mem_free(p);
}

CvmAlloc cvm_buf_system_alloc(void) {
    CvmAlloc a = {.ctx = NULL,
                  .alloc = system_alloc,
                  .realloc = system_realloc,
                  .free = system_free};
    return a;
}
