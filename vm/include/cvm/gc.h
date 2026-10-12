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
#include <stdatomic.h>

#include "base/assert.h"
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

    /*
     * Array of leaf elements (stride = element descriptor size). The object
     * has a length-driven tail that contains no pointers anywhere, so the
     * scanner skips it wholesale while the object's extent still follows the
     * length.
     *
     * This is what a string is, and what any other opaque blob is.
     * STRUCT_ARRAY expresses the same layout and would visit every element to
     * discover that none of them is a pointer — a loop per byte of every
     * string in the heap, at every collection.
     */
    CVM_TYPE_SCALAR_ARRAY = 4,
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
 * A per-object flag is not in the object.
 *
 * Both of the flags this runtime has are properties of an *object* rather
 * than of its type -- which is why they are not in the descriptor -- but they
 * are read by the collector while it is already holding the address's
 * metadata byte, and that byte is where whippet keeps the same kind of
 * information (its allocation kind sits beside the mark state in one byte per
 * granule). So they live there, in gc/meta.h, and the object header is one
 * word.
 *
 * That word is the difference between this collector's objects and both
 * references': a header of two words makes a 24-byte node 48 bytes where
 * commix's one-word header and whippet's one-word tag make it 32, and 48
 * bytes is past the point where an object stops fitting a cache line with
 * its neighbour.
 *
 * The two bits the header used to carry:
 *
 *   CVM_OM_FLAG_WEAK       the object does not hold its referent alive
 *   CVM_OM_FLAG_FINALIZER  the object owns something outside the heap
 *
 * A third, CVM_GC_MARKED, is gone rather than moved: marking has been
 * recorded in the metadata byte since this collector was written -- a mark
 * bit in the object is what a conservative collector needs, because it
 * cannot always find the metadata for an address it guessed at, and this one
 * always can -- so the bit had no reader and no writer anywhere in the
 * repository.
 */

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
 *
 * The *payload* is aligned to the header's size and no more, which is one
 * word: 8 bytes. A type whose fields need more than that has to pad its own
 * payload, and it can, because the header's size is this header's business
 * and the frontend reads it here. Saying so is the point -- the payload was
 * 16-aligned for as long as the header was two words, and nothing said that
 * was a promise rather than a coincidence.
 */
#define CVM_ALLOC_ALIGNMENT ((usize)(2 * CVM_WORD_SIZE))

/*
 * Largest object the inlined bump path will serve.
 *
 * The inlined path bumps within a run of lines, so it can only produce an
 * object that fits inside one line. Bigger objects take the slow path,
 * which knows about whole-block and chunk allocation. The threshold is
 * part of the ABI because generated code embeds the comparison; gc/layout.h
 * asserts that it matches the line size.
 */
#define CVM_INLINE_ALLOC_MAX ((usize)256)

/*
 * The object metadata state the inlined path writes: "an object starts
 * here". The full state machine is in gc/meta.h; that header asserts these
 * values agree, so the duplication cannot drift.
 */
#define CVM_OBJECT_SLOT_ALLOCATED ((u8)0x2)

/*
 * The metadata state the inlined path writes on the object's *last* slot:
 * "an object of this many slots ends here". See the note on CVM_OM_END in
 * gc/meta.h for what reads it and why it is in the table rather than in the
 * object.
 */
#define CVM_OBJECT_SLOT_END ((u8)0x4)

/*
 * Every GC heap object begins with this header. Allocation returns a
 * pointer to the header; user payload follows immediately after.
 */
typedef struct CvmGcHeader {
    CvmTypeDescriptor const* type; /* static, compiler-generated */
} CvmGcHeader;

/* ------------------------------------------------------------------ */
/* Thread state                                                        */
/* ------------------------------------------------------------------ */

/*
 * Whether a thread is executing managed code or is inside a foreign call.
 *
 * An unmanaged thread cannot be touching the heap, so a collection does
 * not have to wait for it: it is treated as already at a safepoint. This
 * is what makes a blocking FFI call harmless rather than a stop-the-world
 * stall.
 *
 * Generated code must bracket every call that can block or re-enter:
 *
 *     cvm_thread_enter_unmanaged();
 *     ... foreign call ...
 *     cvm_thread_leave_unmanaged();
 *
 * The two are counted, so nesting composes. That is the ordinary case
 * rather than an exotic one: a foreign call that calls back into managed
 * code, which makes a foreign call of its own, nests. Only the outermost
 * leave makes the thread managed again.
 *
 * Leaving polls before returning to generated code, which is what makes
 * the return safe when a collection started while the thread was away.
 */
typedef enum CvmThreadState {
    CVM_THREAD_MANAGED = 0,
    CVM_THREAD_UNMANAGED = 1,
} CvmThreadState;

void cvm_thread_enter_unmanaged(void);
void cvm_thread_leave_unmanaged(void);

/*
 * How deep the unmanaged nesting is, and how to put it back.
 *
 * Public because a frontend that suspends an activation has to save it: the
 * depth is per-thread state, and a coroutine that suspends inside a foreign
 * call and later resumes has to restore its own. The same pair is what an
 * unwind uses -- see cvm_throw in vm/exc.c.
 *
 * Restoring also restores the state a collector reads, which is why this is
 * the only correct way to put the depth back: assigning the depth alone
 * would leave a thread that looks managed while it is inside a foreign call.
 */
u32 cvm_thread_unmanaged_depth(void);
void cvm_thread_restore_unmanaged_depth(u32 depth);

/* ------------------------------------------------------------------ */
/* Safepoints                                                          */
/* ------------------------------------------------------------------ */

/*
 * Set by a thread that is collecting. Generated code polls it at
 * function entries and loop back edges.
 */
extern _Atomic bool cvm_safepoint_requested;

/*
 * Whether a mark phase is running, which is what the write barrier asks.
 *
 * Not the same question as cvm_safepoint_requested, and the difference is a
 * bug this was changed to fix. The safepoint flag is set for the whole
 * stop-the-world window, which includes the sweep; a barrier that acts during
 * the sweep re-marks a block the sweeper has already finished with, and the
 * sweeper's consistency check catches it as "a block is still marked after
 * sweeping".
 *
 * Acting during the sweep would also be pointless. What the barrier prevents
 * is the marker missing an object, and by the time the sweep runs the live set
 * is already decided: a store into a live object during the sweep either
 * stores a live object, which needs nothing, or stores one that was already
 * unreachable at mark time, which is the program holding a pointer the
 * collector could not see.
 */
extern _Atomic bool cvm_marking_active;

/* Out-of-line slow path: park until the collection finishes. */
void cvm_safepoint_park(void);

/*
 * The safepoint poll, inlined into generated code.
 *
 * Two mechanisms, chosen at build time. Nothing outside this function and
 * the collector's arming code knows which is in use.
 *
 * Poll placement is a frontend obligation for loop back edges — a loop with
 * no poll can keep a collection waiting indefinitely, which is what
 * cvm_safepoint_stuck_reports exists to diagnose. Function entries are
 * covered already, because cvm_frame_push polls.
 */
#if defined(CVM_SAFEPOINT_TRAPS) && !defined(_WIN32)

/*
 * The cell to poll, in its own TLS slot. The poll reads it on every
 * function entry, so one segment-offset load beats a load through the
 * context, which would be a second access and a heap one.
 */
extern __thread opaque_mut cvm_tls_trap_cell;

CVM_INLINE void cvm_safepoint_poll(void) {
    /*
     * Load from the trap cell. While the collector has the page armed this
     * faults, and the fault handler redirects execution to a stub that
     * parks the thread. The value read is irrelevant — the page is the
     * signal — which is why it is discarded.
     *
     * One load and no branch. There is also nothing for the compiler to
     * reorder around: the load cannot be hoisted past a collection,
     * because the collection is what makes it fault.
     *
     * A thread that has not registered cannot be running generated code,
     * so the cell is valid wherever this is reached. The attach path makes
     * that true by refusing to register a thread it cannot give a page to.
     */
    (void)*(volatile u8*)cvm_tls_trap_cell;
}

#else

CVM_INLINE void cvm_safepoint_poll(void) {
    /*
     * One relaxed-in-practice atomic load and a well-predicted branch. The
     * acquire ordering is what makes everything the collecting thread did
     * before setting the flag visible to this thread, which matters because
     * the flag is also the handshake for "stop touching the heap".
     */
    if (CVM_UNLIKELY(
            atomic_load_explicit(&cvm_safepoint_requested, memory_order_acquire))) {
        cvm_safepoint_park();
    }
}

#endif

/* ------------------------------------------------------------------ */
/* GCFrame: the precise root stack                                     */
/* ------------------------------------------------------------------ */

/*
 * One per managed activation record. Frames live in an array owned by the
 * thread, and one of them holds every GC pointer that is live in its
 * activation. Generated code is responsible for keeping slots in sync with
 * source-level variables.
 *
 * A frame does not know its own position. The position *is* the frame's
 * identity: the thread's context counts how many are live, and only the
 * prefix below that count is reached by anything. That is what makes
 * abandoning frames safe -- see the note above cvm_frame_push.
 */
typedef struct CvmGcFrame {
    u32 num_slots;

    /*
     * The caller's array of live pointers, not storage inside the frame.
     *
     * A flexible array member here would be the obvious spelling and does
     * not work: a struct containing one cannot be embedded in another
     * struct except at the end, so generated code could not declare a
     * frame alongside its own slots. A pointer costs one indirection, on
     * the root walk only, which is a cold path.
     */
    opaque_mut* roots; /* num_slots entries */
} CvmGcFrame;

/*
 * What generated code holds for the duration of an activation.
 *
 * An index rather than a `CvmGcFrame*`, and that is a requirement rather
 * than a preference: the array grows by reallocation, so a pointer taken at
 * push time would dangle the first time the array moved. An index survives
 * the move, and it can be saved and restored by a value -- which is the
 * whole point of the design.
 */
typedef struct CvmFrameHandle {
    u32 index;
} CvmFrameHandle;

/*
 * No frame flags.
 *
 * An earlier version carried CVM_FRAME_IN_FFI here, meaning "the thread that
 * owns this frame is inside a foreign call, so do not wait for it". It was
 * never set and never read, and the reason is that the fact it describes is
 * already recorded one level up: the *thread* is either managed or
 * unmanaged, and that is what a collector consults when deciding whether to
 * wait. A per-frame copy would be a second source of truth for the same
 * fact, and the first time the two disagreed the collector would be wrong
 * in a way no test would name.
 *
 * The frame is therefore the frame: where the previous one is, how many
 * slots, and the slots.
 */
/* ------------------------------------------------------------------ */
/* VMContext: per-thread state, reached via TLS                        */
/* ------------------------------------------------------------------ */

/*
 * Per-thread state, reached through TLS.
 *
 * The first three fields are what generated code touches, and they are
 * grouped at the front so the hot ones share a cache line. Everything
 * after them is runtime bookkeeping that generated code never reads.
 */
/*
 * The runtime's record for one `try` (vm/exc.h). Named here because the
 * thread context holds the chain, and the context is what a collector reads.
 * The definition is not here: what a handler holds is the runtime's
 * business, and a header that describes the root set only needs to say that
 * the chain exists.
 */
struct CvmHandler;
typedef struct CvmHandler CvmHandler;

typedef struct CvmVMContext {
    u8* bump_ptr; /* fast-path allocation cursor */
    u8* bump_end; /* end of the current bump region */

    /*
     * Heap base and the object metadata table, so the inlined path can
     * mark its own allocation without a call. The table holds one byte per
     * allocation slot; the indexing rule mirrors gc/bytemap.h, which is
     * where the authoritative version lives.
     *
     * These are needed here because the fast path must mark what it
     * allocates: an object the metadata does not describe is invisible to
     * the collector, which is the bug this field was added to fix.
     */
    u8* heap_base;
    u8* object_meta;

    /*
     * The thread's live frames, as a prefix of an array the runtime owns:
     * `frames[0..frame_depth)` are the frames, and everything at or above
     * `frame_depth` is storage whose lifetime has ended and which nothing
     * reads. `frames` is reallocated as the depth grows, which is why the
     * handle generated code holds is an index.
     *
     * A count and a prefix, rather than a chain through the callers'
     * stacks. Unwinding then means restoring an integer, and the frames on
     * abandoned call paths are unreachable by construction rather than by
     * discipline -- where a chain would leave their `prev` links pointing
     * into C stack that has been reused, which is a crash in the collector
     * rather than a leak.
     */
    CvmGcFrame* frames;
    u32 frame_depth;
    u32 frame_capacity;

    /*
     * The handler chain, innermost first, and a free list of records that
     * are not on it. Both per-thread, and both made of records the runtime
     * allocated -- which keeps them out of the class of problem the frame
     * array was built to leave behind: no chain here points into a C stack,
     * so an abandoned activation cannot leave one for the collector to
     * follow.
     *
     * Only `handlers` is walked by cvm_mark_roots. A record on the free list
     * is not a live handler, and it still holds the exception it was popped
     * with; marking that would retain an object nothing can reach.
     */
    CvmHandler* handlers;
    CvmHandler* free_handlers;
    u32 handler_depth;

    /*
     * The runtime's own record for this thread (vm/context.h). A context
     * is embedded in its thread node, so this points back at the
     * container. It is an explicit field rather than a reliance on the
     * embedding offset, because that coupling would be invisible at the
     * point where someone breaks it.
     */
    opaque_mut owner;
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
 * Frame storage belongs to the runtime, and generated code holds an index.
 *
 * Generated code declares the slots as a local and a handle beside them:
 *
 *     CVM_FRAME_DECL(f, 2);
 *     cvm_frame_push(&f_frame, f_roots, 2);
 *     f_roots[0] = cvm_alloc(&some_type);
 *     ...
 *     cvm_frame_pop(&f_frame);
 *
 * The slots stay on the machine stack -- they would have been stack locals
 * anyway -- and the frame that describes them goes into an array the
 * runtime owns, one per thread. What the activation holds is `f_frame`,
 * which is an index.
 *
 * ---------------------------------------------------------------------
 * Why an array and not a chain through the callers' stacks
 * ---------------------------------------------------------------------
 *
 * The obvious shape is for the frame to be a local too, linked to its
 * caller's frame by a `prev` pointer, and that is what this was. It is
 * correct as long as the chain is unwound by popping. It is wrong the
 * moment anything unwinds by other means -- and unwinding by other means is
 * exactly what exceptions need:
 *
 *     frame A is pushed, allocates, calls B;
 *     B throws; the handler is in A; A's and B's frames are abandoned
 *       without either pop running;
 *     A's chain still points at B's frame, which lives in C stack that has
 *       been reused;
 *     the next collection walks A's chain into that storage.
 *
 * The failure is not a leak. The collector dereferences a `num_slots` and a
 * `roots` pointer read out of whatever the stack now holds, so it is a wild
 * read from the marker -- a crash, or a silent retention of a number the
 * collector invented.
 *
 * Counting instead of linking removes the failure rather than guarding it.
 * The live frames are the first `frame_depth` entries, so a frame that was
 * abandoned is not *skipped*: it is not reachable by construction, and there
 * is no pointer from a live frame into a dead one for anything to follow.
 * Unwinding is then one store of an integer, which is also the thing a
 * frontend can save and restore.
 *
 * The cost is an indirection on the root walk, which is cold, and a bounds
 * check that cannot be skipped.
 *
 * ---------------------------------------------------------------------
 * Why not carve frames out of the bump region
 * ---------------------------------------------------------------------
 *
 * Storing frames in the same arena as objects, and rewinding the bump
 * pointer on pop, is tempting and does not work. Rewinding reclaims
 * everything allocated since the frame was pushed, including any object the
 * function is about to return:
 *
 *     frame->roots[0] = cvm_alloc(&t);
 *     cvm_frame_pop(frame);        // bump pointer rewinds past it
 *     return frame->roots[0];      // dangling
 *
 * Nothing short of escape analysis could tell the two cases apart, and the
 * collector is not the place for that.
 *
 * The collector still never scans the machine stack. Roots come from these
 * frames, which hold exactly the live pointers, and walking them is what
 * root finding is.
 */
#define CVM_FRAME_DECL(name, slots)                                                    \
    opaque_mut name##_roots[(slots)];                                                  \
    CvmFrameHandle name##_frame

/* Out of line, in vm/context.c. Fatal if the array cannot grow: a thread
 * with nowhere to put its frames cannot be allowed to keep running, because
 * the roots it would stop recording are the difference between a collection
 * that is correct and one that reclaims live objects. */
void cvm_frame_grow(CvmVMContext* ctx);

CVM_INLINE void cvm_frame_push(CvmFrameHandle* frame, opaque_mut* roots,
                               u32 num_slots) {
    /*
     * A function entry is a safepoint, and this is where generated code
     * enters a function -- so the poll lives here rather than being left to
     * the frontend to remember.
     *
     * That narrows the frontend's obligation to loop back edges, which are
     * the places it has to think about anyway. The alternative is a
     * protocol with two obligations, one of which is easy to forget
     * precisely because it looks like boilerplate.
     *
     * The cost is one relaxed load and a well-predicted branch per call.
     * The compiler can merge consecutive ones, and it is the same load the
     * frontend would have emitted.
     */
    cvm_safepoint_poll();

    CvmVMContext* ctx = cvm_ctx();
    if (ctx->frame_depth == ctx->frame_capacity) {
        cvm_frame_grow(ctx);
    }

    u32 index = ctx->frame_depth;
    ctx->frames[index].roots = roots;
    ctx->frames[index].num_slots = num_slots;

    /*
     * Zeroed here rather than left to the caller.
     *
     * A frame whose slots hold whatever the stack had is a frame whose roots
     * are whatever the stack had: the marker would walk them, and the only
     * thing standing between that and a wild read is the bounds check
     * (assumption A2). Doing it here makes "the slots are valid when the
     * frame is linked" the only order, instead of an obligation that looks
     * like boilerplate and is therefore the one that gets forgotten -- the
     * same argument this file already makes about the safepoint poll being
     * inside the push.
     */
    for (u32 slot = 0; slot < num_slots; slot++) {
        roots[slot] = NULL;
    }

    ctx->frame_depth = index + 1u;
    frame->index = index;
}

/*
 * Idempotent, and that is deliberate rather than convenient.
 *
 * Only the frame that is on top takes the depth down. A second pop finds the
 * depth already at or below its own index and does nothing, and so does a
 * pop that runs after an unwind has already restored a smaller depth. The
 * alternative -- assert that the frame is the top, then follow it -- makes
 * every such mistake fatal at the moment it is least convenient to debug,
 * and buys nothing: the state after a redundant pop is exactly the state
 * wanted.
 *
 * What it does not cover is a stale pop landing after a *different*
 * activation has taken the same index. That cannot happen here: a push is
 * paired with exactly one pop on that activation's own path, and an
 * activation reached by an unwind never resumes to run its epilogue.
 */
CVM_INLINE void cvm_frame_pop(CvmFrameHandle* frame) {
    CvmVMContext* ctx = cvm_ctx();
    if (frame->index + 1u == ctx->frame_depth) {
        ctx->frame_depth = frame->index;
    }
}

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

/* Slow paths live in vm (out of line). */
opaque_mut cvm_alloc_slow(CvmTypeDescriptor const* type, usize size);
opaque_mut cvm_alloc_large(CvmTypeDescriptor const* type, usize size);

/* Mark a just-allocated object as surviving, while marking runs. */
void cvm_alloc_black(opaque_mut payload);

/*
 * Fast path: bump within the current region. The limit check and
 * slow-path call are the only branches.
 */
/*
 * The inlined allocation path, with an explicit payload size.
 *
 * cvm_alloc passes the descriptor's own size; an array passes the fixed
 * part plus its tail, which is only known at run time.
 *
 * A fresh object's payload is zeroed, and generated code depends on that:
 * a type with pointer fields has to start out valid rather than full of
 * rubbish for the marker to follow. But that is now a property of where the
 * memory came from rather than work done here. Every byte this function
 * hands out already reads as zero, because the collector clears a range
 * when it reclaims it and the platform hands the heap back zeroed -- see
 * the note on rt_mem_zero and the clearing sites in gc/sweeper.c. What is
 * left to write here is the header, which is the collector's own state.
 *
 * Clearing here instead was measured at 40-58% of three of the benchmark's
 * scenarios, and it was mostly wasted: memory the heap has never used is
 * already zero, and memory it has reclaimed is cleared once per run rather
 * than once per object.
 */
CVM_INLINE opaque_mut cvm_alloc_sized(CvmTypeDescriptor const* type,
                                      usize payload_size) {
    CvmVMContext* ctx = cvm_ctx();
    usize size = cvm_align_up(sizeof(CvmGcHeader) + payload_size, CVM_ALLOC_ALIGNMENT);

    /*
     * At least two slots, so that an object's start and its end are different
     * slots.
     *
     * The end of an object is a slot state, and a slot holds one state: an
     * object that fitted in a single slot could not say both "an object
     * starts here" and "it ends here". Whippet has the same problem and
     * solves it with a bit per property, so a one-granule object writes
     * `begin | end` in one byte; our states are values compared as a whole,
     * which is what makes them cheap to read and what makes them unable to
     * overlap.
     *
     * So a payload of one word or less costs one slot more than it needs. It
     * is the smallest possible object -- the header is one word and the
     * payload rounding is to 16 -- and the alternative is a second state
     * meaning "starts and ends here", compared alongside the other four
     * everywhere a state is read.
     */
    if (size == CVM_ALLOC_ALIGNMENT) {
        size += CVM_ALLOC_ALIGNMENT;
    }
    u8* p = ctx->bump_ptr;

    if (CVM_UNLIKELY(size > CVM_INLINE_ALLOC_MAX || p + size > ctx->bump_end)) {
        return cvm_alloc_slow(type, size);
    }
    ctx->bump_ptr = p + size;

    CvmGcHeader* hdr = (CvmGcHeader*)p;
    hdr->type = type;

    /*
     * Mark the slot. Without this the object exists but the collector
     * cannot see it, which is silent and fatal.
     *
     * Relaxed and atomic rather than a plain store, because the collector
     * reads these bytes atomically and mixing the two kinds of access to
     * one address is a data race whichever way round it is. A relaxed
     * atomic byte store is the same single instruction.
     */
    usize slot = (usize)(p - ctx->heap_base) / CVM_ALLOC_ALIGNMENT;
    atomic_store_explicit((_Atomic u8*)&ctx->object_meta[slot],
                          CVM_OBJECT_SLOT_ALLOCATED, memory_order_relaxed);

    /*
     * And the object's extent: one slot past its first, minus one. `size` is
     * a multiple of the alignment and at least two slots, because the header
     * is one word and a payload is at least one more, so the end is never the
     * same slot as the start.
     */
    atomic_store_explicit(
        (_Atomic u8*)&ctx->object_meta[slot + size / CVM_ALLOC_ALIGNMENT - 1],
        CVM_OBJECT_SLOT_END, memory_order_relaxed);

    /*
     * Allocate black while a mark phase is running.
     *
     * An object born during concurrent marking is unreachable when the
     * sweeper comes to decide what survived -- nothing can point to it yet,
     * so no marker has a reason to visit it -- and without this it would be
     * reclaimed while its owner is still filling it. Marking it here costs
     * the sweeper one cycle of floating garbage at worst, and it means the
     * object survives on its own, without needing a frame slot or a root,
     * which is the same mitigation the frame discipline note in this header
     * describes for a pointer held only in a C local.
     *
     * Black, not grey: the object is not traced, because the write barrier
     * marks every value stored into it and nothing else reaches it. Out of
     * line because the marking it triggers -- the slot's byte, the block's
     * mark, the lines' marks -- touches three tables the fast path has no
     * other reason to visit, and this branch is taken only while marking.
     */
    if (CVM_UNLIKELY(atomic_load_explicit(&cvm_marking_active, memory_order_acquire))) {
        cvm_alloc_black((opaque_mut)(hdr + 1));
    }

    return (opaque_mut)(hdr + 1); /* payload follows the header */
}

CVM_INLINE opaque_mut cvm_alloc(CvmTypeDescriptor const* type) {
    return cvm_alloc_sized(type, type->size);
}

/*
 * Allocate a variable-length array.
 *
 * `stride` is the element size: the word size for a pointer array, the
 * element descriptor's size for an array of aggregates. The length is
 * written into the object, because that is where both the collector and
 * the generated code read it from.
 */
CVM_INLINE opaque_mut cvm_alloc_array(CvmTypeDescriptor const* type, usize length,
                                      usize stride) {
    usize payload_size = type->size + length * stride;
    u8* payload = (u8*)cvm_alloc_sized(type, payload_size);
    *(usize*)(payload + type->array_length_offset) = length;
    return (opaque_mut)payload;
}

/*
 * Address convention.
 *
 * An object *starts* at its header. cvm_alloc returns a pointer to the
 * payload, which is sizeof(CvmGcHeader) past the start, because that is
 * what generated code and user types see.
 *
 * The bytemap and every other piece of collector bookkeeping index the
 * object by its start, not by its payload. So a collector that has a
 * payload pointer converts first:
 *
 *     CvmGcHeader const *h = cvm_header_of(payload);
 *     ObjectMeta *meta = bytemap_get(bytemap, (u8 const *)h);
 *
 * The conversion is not extra work: the collector needs the header anyway
 * to read the type descriptor. Getting this wrong is invisible until
 * something looks up a slot and finds it free — which is exactly how the
 * bug that established this convention was found.
 */

/* Payload pointer <-> header helpers.
 * Read flavor keeps const; write flavor is the explicit exception
 * used only by the collector when it must mutate header flags. */
CVM_INLINE CvmGcHeader const* cvm_header_of(opaque payload) {
    return ((CvmGcHeader const*)payload) - 1;
}
CVM_INLINE CvmGcHeader* cvm_header_of_mut(opaque_mut payload) {
    return ((CvmGcHeader*)payload) - 1;
}

/*
 * The object's extent in bytes, measured from its start.
 *
 * The header and the payload are rounded up together, so an object whose
 * type size is not a multiple of the alignment does not leave the next
 * object half a slot away.
 */
/* Bytes per element of an array type. */
CVM_INLINE usize cvm_array_stride(CvmTypeDescriptor const* type) {
    return (type->kind == CVM_TYPE_PTR_ARRAY) ? CVM_WORD_SIZE
                                              : type->array_elem_type->size;
}

CVM_INLINE bool cvm_type_is_array(CvmTypeDescriptor const* type) {
    return type->kind == CVM_TYPE_PTR_ARRAY || type->kind == CVM_TYPE_STRUCT_ARRAY ||
           type->kind == CVM_TYPE_SCALAR_ARRAY;
}

/*
 * The object's extent in bytes, measured from its start.
 *
 * For an array this has to be read from the object rather than from the
 * descriptor: the descriptor describes the fixed part, and how much tail
 * follows depends on a length only known at run time. Getting this wrong
 * under-marks the object's lines, and the sweeper then reclaims a line
 * that still holds live data.
 *
 * The header and the payload are rounded up together, so an object whose
 * size is not a multiple of the alignment does not leave the next object
 * half a slot away.
 */
CVM_INLINE usize cvm_object_size(CvmGcHeader const* header) {
    CvmTypeDescriptor const* type = header->type;
    usize size = sizeof(CvmGcHeader) + type->size;

    if (cvm_type_is_array(type)) {
        u8 const* payload = (u8 const*)(header + 1);
        usize length = *(usize const*)(payload + type->array_length_offset);
        size += length * cvm_array_stride(type);
    }

    return cvm_align_up(size, CVM_ALLOC_ALIGNMENT);
}

/* ------------------------------------------------------------------ */
/* The write barrier                                                   */
/* ------------------------------------------------------------------ */

/*
 * Slow path: what to do about a store that may have opened a window. Out of
 * line, because the fast path is on every pointer store the program makes.
 */
void cvm_store_barrier_slow(opaque_mut value);

/*
 * Store a pointer into an object's field.
 *
 * This is the only way to write a pointer into an object. Generated code
 * emits it for every pointer field store -- assignment, property write,
 * array write, closure capture, initialisation -- and the runtime uses it
 * for its own containers.
 *
 * ---------------------------------------------------------------------
 * Why it exists
 * ---------------------------------------------------------------------
 *
 * Marking is a graph traversal, and a traversal needs the graph to hold
 * still. Stopping the world is what makes it hold still today: no thread
 * that could change the graph is running while the marker walks it.
 *
 * When marking runs alongside the program instead, the graph does not hold
 * still. The failure is specific and silent. The marker marks an object,
 * traces its fields, and moves on -- an object it has finished with is
 * never looked at again. If the program then stores a pointer into that
 * object, the object it now points to was never marked, is collected, and
 * the field is left pointing at freed memory. Nothing reports it: the
 * store was legal, the object was live, and the symptom appears later
 * somewhere else, as a read of whatever the collector put there.
 *
 * A store is what has to close the window, because it is the only moment
 * the program can make an unmarked object reachable from a marked one. So
 * the store marks what it is about to publish. Marking is idempotent and
 * the packet lists are lock-free, so this is safe from any thread,
 * including one the collector is not waiting for.
 *
 * ---------------------------------------------------------------------
 * What it costs, and when
 * ---------------------------------------------------------------------
 *
 * One load of a flag that is shared and in cache, plus a branch the
 * predictor gets right. The load is skipped entirely when the value is
 * NULL, because emptying a field cannot make anything reachable.
 *
 * Outside marking the branch is not taken and the store is the store it
 * would have been. The flag is set only for the mark phase itself -- not for
 * the whole stop-the-world window, which would include the sweep -- so under
 * the stop-the-world marking this runtime does today, the barrier fires only
 * for the threads a collection does not stop, which are the unmanaged ones
 * (inside a foreign call, or blocked in a wait). That is the same window the
 * root table's barrier exists for.
 *
 * ---------------------------------------------------------------------
 * The contract
 * ---------------------------------------------------------------------
 *
 * Writing a pointer field directly -- `obj->field = value` -- is correct
 * only while marking is stop-the-world. It is not correct with concurrent
 * marking, and nothing will catch it: see the failure above.
 *
 * Two cases are exempt, and they are worth naming because they are most of
 * the stores in allocation-heavy code:
 *
 *   - Filling an object that nothing can reach yet. A fresh object is
 *     unreachable while it is being filled, so the marker cannot see it and
 *     there is nothing to miss. The store that *publishes* it -- into a live
 *     object, or into a frame slot -- is the one that needs the barrier.
 *   - Bulk copying pointers into a fresh object, for the same reason. This is
 *     the case with no entry point here: a range copy is not a field store,
 *     and the runtime has no copy into an existing object today. If one
 *     appears it needs a range form, because a per-field barrier cannot see
 *     a memcpy.
 */
/*
 * The barrier itself: mark a value that a running thread has just published.
 *
 * Call it after the store, not before. A marker that reads the slot between
 * the two sees the new value unmarked and marks it a moment later, so the
 * object is still traced. The other order has a window in which the field is
 * not yet published and nothing marks the value if the marker has already
 * finished with the object holding it.
 *
 * Separate from the two functions below rather than fused with them, because
 * not every pointer store is a plain store. A container whose slots a lock-free
 * reader loads atomically has to store them atomically too -- mixing atomic and
 * non-atomic access to one address is a data race whichever way round it is --
 * and such a store has to keep its own ordering and then call this.
 */
CVM_INLINE void cvm_store_barrier(opaque_mut value) {
    if (CVM_UNLIKELY(value != NULL &&
                     atomic_load_explicit(&cvm_marking_active, memory_order_acquire))) {
        cvm_store_barrier_slow(value);
    }
}

/*
 * Store a pointer into a slot, with the barrier.
 *
 * The form for a container that owns the slot: `&a->items[i]`, say.
 */
CVM_INLINE void cvm_store_ptr(opaque_mut* slot, opaque_mut value) {
    /*
     * A relaxed atomic store, for the same reason the marker's field reads
     * are relaxed atomic loads: in the concurrent window, the marker may be
     * reading this slot while the store happens, and mixing atomic and
     * non-atomic access to one address is a data race by definition.
     *
     * Relaxed is sufficient because the ordering that correctness needs is
     * carried by the barrier below, not by this store: if the marker sees
     * the new value it will be marked (the barrier ran first), and if it
     * sees the old one that value is still being kept alive. A relaxed
     * atomic store is the same instruction as a plain store on every
     * target this runtime builds for.
     */
    atomic_store_explicit((opaque_mut _Atomic*)slot, value, memory_order_relaxed);
    cvm_store_barrier(value);
}

/*
 * Store a pointer into a field of `obj`, `offset` bytes from the payload.
 *
 * The offset form, which is what generated code has: a descriptor names its
 * pointer fields by offset (see CvmPtrField), and those offsets are from the
 * payload -- the pointer the program holds -- not from the object header.
 */
CVM_INLINE void cvm_store_field(opaque_mut obj, usize offset, opaque_mut value) {
    cvm_store_ptr((opaque_mut*)((u8*)obj + offset), value);
}

/*
 * Nothing here for the collector to call.
 *
 * An earlier version of this header declared cvm_gc_scan_frame and
 * cvm_gc_scan_object, anticipating an entry point for the marker. They were
 * never defined, which is worse than not declaring them: a frontend reading
 * this header would have written a call to a function that does not exist,
 * and found out at link time. The marker's real entry points are internal
 * (gc/marker.h), because generated code does not drive marking — the
 * collector does.
 *
 * The build now checks for this class of mistake; see the public-ABI audit
 * in scripts/lib/verify.mjs.
 */

#endif /* CVM_GC_H */
