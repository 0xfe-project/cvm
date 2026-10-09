/*
 * vm/include/cvm/buf.h
 *
 * A growable run of bytes, over a caller-supplied allocator.
 *
 * This is the buffer the ported text and pattern code needs: one place to append
 * bytes, one place to append an integer of whatever width, and one place to
 * insert into the middle — which is what writing a bytecode stream needs and what
 * a string builder does not.
 *
 * It is not a string: `size` is bytes, not code units, and nothing here knows
 * that text might be inside it. cvm/builder.h is the one that knows that.
 *
 * The allocation contract is base/alloc.h's, and the reason is the same as it was
 * for the normalizer: the caller's allocator, and the buffer tracks its own size
 * because a realloc-shaped callback could not be given it. `error` is sticky, so a
 * caller appending a long stream can check once at the end.
 */

#ifndef CVM_BUF_H
#define CVM_BUF_H

#include "base/alloc.h"
#include "base/types.h"

typedef struct CvmBuf {
    u8* data;
    usize size;      /* bytes used */
    usize allocated; /* bytes allocated */
    bool error;
    CvmAlloc alloc;
} CvmBuf;

void cvm_buf_init(CvmBuf* buf, CvmAlloc alloc);

/* Release the buffer. Safe on an empty buffer; leaves it empty and usable. */
void cvm_buf_free(CvmBuf* buf);

CVM_INLINE bool cvm_buf_error(CvmBuf const* buf) {
    return buf->error;
}

CVM_INLINE usize cvm_buf_size(CvmBuf const* buf) {
    return buf->size;
}

/* Make room for `bytes` more. Every append does this for itself; a caller that
 * knows how large the result will be can say so once. */
bool cvm_buf_reserve(CvmBuf* buf, usize bytes);

bool cvm_buf_put(CvmBuf* buf, opaque data, usize bytes);
bool cvm_buf_putc(CvmBuf* buf, u8 value);

/* Written little-endian, which is the order the ported code reads them back in —
 * it is a byte stream, and the stream's endianness is part of what it means. */
bool cvm_buf_put_u16(CvmBuf* buf, u16 value);
bool cvm_buf_put_u32(CvmBuf* buf, u32 value);
bool cvm_buf_put_u64(CvmBuf* buf, u64 value);

/* Insert `bytes` at `pos`, which must be within the buffer. */
bool cvm_buf_insert(CvmBuf* buf, usize pos, opaque data, usize bytes);

/* The runtime's own allocator: rt_mem_alloc and rt_mem_free. */
CvmAlloc cvm_buf_system_alloc(void);

#endif /* CVM_BUF_H */
