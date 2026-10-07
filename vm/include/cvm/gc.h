/*
 * vm/include/cvm/gc.h
 *
 * Public GC ABI for generated code. This is the contract between
 * user frontends (which emit C that includes this header) and the
 * VM runtime. Everything in this file is freestanding: no libc
 * headers, no libc types.
 *
 * Discipline:
 *   - Fixed-width types only (from rt/base/types.h).
 *   - No C strings.
 *   - const by default; mutation is explicit.
 *   - TypeDescriptor is recursive: embedded aggregate fields are
 *     described by pointing at their own descriptor, never flattened.
 */

#ifndef CVM_GC_H
#define CVM_GC_H

/* rt/ is on the -I list, so this resolves without fragile relative
 * traversal. */
#include "base/types.h"

/* ------------------------------------------------------------------ */
/* TypeDescriptor                                                      */
/* ------------------------------------------------------------------ */

typedef struct CvmTypeDescriptor CvmTypeDescriptor;

/* One direct GC-pointer field at a fixed byte offset. */
typedef struct CvmPtrField {
    u16 offset; /* byte offset of the pointer field inside the object */
    u16 _pad;
} CvmPtrField;

/* One embedded aggregate field: an inline struct value (not a pointer)
 * that itself contains GC pointers, described recursively. */
typedef struct CvmEmbedField {
    u16 offset; /* byte offset of the embedded struct */
    u16 _pad;
    CvmTypeDescriptor const* type; /* descriptor of the embedded struct */
} CvmEmbedField;

/*
 * Tag values only. The kind is stored in a u32 field, never in an enum:
 * the width of an enum is implementation-defined, and descriptor layout
 * must be byte-identical across gcc/clang/msvc.
 */
typedef enum CvmTypeKind {
    /* Plain struct: scan ptr_fields, then recurse into embed_fields. */
    CVM_TYPE_STRUCT = 0,

    /* Built-in GC array of pointers. Runtime layout must start with
     * the fields named by array_length_offset / array_data_offset. */
    CVM_TYPE_PTR_ARRAY = 1,

    /* Array of embedded aggregates (stride = element descriptor size).
     * Each element is scanned recursively via array_elem_type. */
    CVM_TYPE_STRUCT_ARRAY = 2,

    /* Leaf: contains no GC pointers at all. Scanner short-circuits. */
    CVM_TYPE_SCALAR = 3,
} CvmTypeKind;

struct CvmTypeDescriptor {
    u32 size; /* physical size of one object, bytes */
    u32 kind; /* CvmTypeKind, fixed width by design */

    /* STRUCT / SCALAR */
    u16 num_ptr_fields;
    u16 num_embed_fields;
    CvmPtrField const* ptr_fields;     /* num_ptr_fields entries, or NULL */
    CvmEmbedField const* embed_fields; /* num_embed_fields entries, or NULL */

    /* *_ARRAY kinds */
    u16 array_length_offset;                  /* offset of usize length */
    u16 array_data_offset;                    /* offset of first element */
    CvmTypeDescriptor const* array_elem_type; /* STRUCT_ARRAY only */
};

/* ------------------------------------------------------------------ */
/* Object header                                                       */
/* ------------------------------------------------------------------ */

/*
 * Header flags are a bitmask over a u32 field: plain macros, not an enum.
 * A fixed-underlying-type enum is C23, and an enum cannot name an OR of
 * two flags anyway.
 */
#define CVM_GC_MARKED ((u32)1u << 0)
#define CVM_GC_PINNED ((u32)1u << 1)

/*
 * Allocation granularity.
 *
 * Every object starts on a CVM_ALLOC_ALIGNMENT boundary, and cvm_alloc
 * rounds the total size up to it. This is part of the ABI rather than an
 * internal detail because generated code inlines the bump fast path and
 * therefore embeds the rounding.
 *
 * The runtime's object metadata table has the same granularity: an
 * address maps to exactly one metadata entry only if object starts are
 * at least this aligned. vm/gc/layout.h asserts the two agree.
 */
#define CVM_ALLOC_ALIGNMENT ((usize)(2 * CVM_WORD_SIZE))

/*
 * Every GC heap object begins with this header. Allocation returns
 * a pointer to the header; user payload follows immediately after.
 */
typedef struct CvmGcHeader {
    CvmTypeDescriptor const* type; /* static, compiler-generated */
    u32 flags;                     /* CVM_GC_* bitmask */
    u32 _pad;                      /* keeps header word-aligned */
} CvmGcHeader;

/* ------------------------------------------------------------------ */
/* GCFrame: the precise root stack                                     */
/* ------------------------------------------------------------------ */

/*
 * One per managed activation record. Frames live in the thread's
 * bump arena, form a singly-linked list via prev, and hold every
 * GC pointer that is live in that activation. Generated code is
 * responsible for keeping slots in sync with source-level variables.
 */
typedef struct CvmGcFrame {
    struct CvmGcFrame* prev;
    u32 num_slots;
    u32 flags;          /* e.g. CVM_FRAME_IN_FFI */
    opaque_mut roots[]; /* num_slots entries */
} CvmGcFrame;

/* Thread is inside a blocking FFI call; GC may treat this frame as a safe
 * root without waiting for the thread to yield. */
#define CVM_FRAME_IN_FFI ((u32)1u << 0)

/* ------------------------------------------------------------------ */
/* VMContext: per-thread state, reached via TLS                        */
/* ------------------------------------------------------------------ */

typedef struct CvmVMContext {
    CvmGcFrame* top_frame; /* head of the precise root stack */
    u8* bump_ptr;          /* fast-path allocation cursor */
    u8* bump_end;          /* end of the current bump region */

    /* Owning OS thread identity is an rt concern; vm keeps only an
     * opaque handle handed down at thread registration. */
    opaque_mut rt_thread;
} CvmVMContext;

/*
 * TLS via initial-exec __thread: one segment-offset instruction per
 * access, no runtime calls, no libc involvement after load.
 * Hard requirement: the VM shared object must be in the initial load
 * set; dlopen of it is unsupported.
 */
extern __thread CvmVMContext* cvm_tls_ctx __attribute__((tls_model("initial-exec")));
#define cvm_ctx() (cvm_tls_ctx)

/* ------------------------------------------------------------------ */
/* Frame management: inline fast path, called by generated code        */
/* ------------------------------------------------------------------ */

/*
 * Frame memory comes from the same bump region as objects: pushing a
 * frame is one addition, popping is one store. The bump region must
 * always have room for the maximum frame size of the function being
 * entered; the slow path guarantees this (out of line, in vm).
 */
CVM_INLINE CvmGcFrame* cvm_frame_push(u32 num_slots) {
    CvmVMContext* ctx = cvm_ctx();
    usize bytes = sizeof(CvmGcFrame) + CVM_WORD_SIZE * num_slots;
    CvmGcFrame* frame = (CvmGcFrame*)ctx->bump_ptr;

    ctx->bump_ptr += bytes;
    frame->prev = ctx->top_frame;
    frame->num_slots = num_slots;
    frame->flags = 0;
    ctx->top_frame = frame;
    return frame;
}

CVM_INLINE void cvm_frame_pop(CvmGcFrame* frame) {
    CvmVMContext* ctx = cvm_ctx();
    ctx->top_frame = frame->prev;
    ctx->bump_ptr = (u8*)frame;
}

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

/* Slow paths live in vm (out of line). */
opaque_mut cvm_alloc_slow(CvmTypeDescriptor const* type, usize size);
opaque_mut cvm_alloc_large(CvmTypeDescriptor const* type, usize size);

/*
 * Fast path: bump within the current region. The limit check and
 * slow-path call are the only branches. Payload is zero-initialized
 * by the runtime contract.
 */
CVM_INLINE opaque_mut cvm_alloc(CvmTypeDescriptor const* type) {
    CvmVMContext* ctx = cvm_ctx();
    usize size = cvm_align_up(sizeof(CvmGcHeader) + type->size, CVM_ALLOC_ALIGNMENT);
    u8* p = ctx->bump_ptr;

    if (CVM_UNLIKELY(p + size > ctx->bump_end)) {
        return cvm_alloc_slow(type, size);
    }
    ctx->bump_ptr = p + size;
    CVM_PREFETCH(p + 64);

    CvmGcHeader* hdr = (CvmGcHeader*)p;
    hdr->type = type;
    hdr->flags = 0;
    return (opaque_mut)(hdr + 1); /* payload follows the header */
}

/* Payload pointer <-> header helpers.
 * Read flavor keeps const; write flavor is the explicit exception
 * used only by the collector when it must mutate header flags. */
CVM_INLINE CvmGcHeader const* cvm_header_of(opaque payload) {
    return ((CvmGcHeader const*)payload) - 1;
}
CVM_INLINE CvmGcHeader* cvm_header_of_mut(opaque_mut payload) {
    return ((CvmGcHeader*)payload) - 1;
}

/* ------------------------------------------------------------------ */
/* Root scanning entry point (called by the collector, not by          */
/* generated code)                                                     */
/* ------------------------------------------------------------------ */

struct CvmHeap;
void cvm_gc_scan_frame(struct CvmHeap* heap, CvmGcFrame const* frame);
void cvm_gc_scan_object(struct CvmHeap* heap, CvmGcHeader* obj);

#endif /* CVM_GC_H */
