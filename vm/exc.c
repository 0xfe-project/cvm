/*
 * vm/exc.c -- the handler chain, and throwing.
 *
 * See vm/exc.h for the shape and why the record is the runtime's.
 */

#include "cvm/exc.h"

#include "context.h"
#include "log.h"
#include "rt.h"

/*
 * Take a record off the free list, or get a new one.
 *
 * A free list rather than an allocation per try, because a `try` inside a
 * loop is ordinary and a malloc on that path is a cost nobody chose. The
 * link and the chain link are the same field: a free record is not on the
 * chain, so `prev` has nothing else to mean.
 *
 * Fatal on allocation failure. There is no recovery to offer -- the site is
 * between establishing a handler and running its body, and the alternative
 * to having a handler is that a throw later has nowhere to go.
 */
static CvmHandler* handler_alloc(CvmVMContext* ctx) {
    CvmHandler* handler = ctx->free_handlers;
    if (handler != NULL) {
        ctx->free_handlers = handler->prev;
        return handler;
    }

    handler = rt_mem_alloc(sizeof(CvmHandler));
    if (handler == NULL) {
        rt_fatal(STR_LITERAL("out of memory for a handler record"));
    }
    return handler;
}

RtJumpBuf* cvm_handler_push(void) {
    CvmVMContext* ctx = cvm_ctx();
    if (ctx == NULL) {
        rt_fatal(STR_LITERAL("try from a thread that never attached"));
    }

    /*
     * The storage constant is checked once, here rather than at some
     * unrelated startup point, because this is the moment it starts to
     * matter: it is checked before the first setjmp that could overrun it,
     * and a wrong size is otherwise a stack smash that reads as anything but
     * a wrong size.
     */
    static _Atomic bool storage_checked = false;
    if (!atomic_load_explicit(&storage_checked, memory_order_acquire)) {
        if (!rt_jump_storage_is_sufficient()) {
            rt_fatal(STR_LITERAL("RtJumpBuf is too small for this platform's jmp_buf"));
        }
        atomic_store_explicit(&storage_checked, true, memory_order_release);
    }

    CvmHandler* handler = handler_alloc(ctx);

    handler->prev = ctx->handlers;
    handler->exception = NULL;
    handler->caught = false;

    /*
     * The state to come back to. The frame depth is what makes an unwind
     * drop everything the try body pushed; the unmanaged depth is the same
     * for a foreign call the body entered and never left. Reading them here,
     * at try entry, is the only moment they are the right values.
     */
    handler->frame_depth = ctx->frame_depth;
    handler->unmanaged_depth = cvm_thread_unmanaged_depth();

    ctx->handlers = handler;
    ctx->handler_depth++;
    return &handler->jump;
}

void cvm_handler_pop(void) {
    CvmVMContext* ctx = cvm_ctx();
    if (ctx == NULL || ctx->handlers == NULL) {
        return;
    }

    CvmHandler* handler = ctx->handlers;
    ctx->handlers = handler->prev;
    if (ctx->handler_depth > 0u) {
        ctx->handler_depth--;
    }

    /* Dropped rather than cleared: the record is unreachable from the chain
     * now, so nothing walks the exception it still holds, and push clears it
     * before the record is used again. */
    handler->prev = ctx->free_handlers;
    ctx->free_handlers = handler;
}

CvmHandler* cvm_handler_top(void) {
    CvmVMContext* ctx = cvm_ctx();
    return (ctx != NULL) ? ctx->handlers : NULL;
}

u32 cvm_handler_depth(void) {
    CvmVMContext* ctx = cvm_ctx();
    return (ctx != NULL) ? ctx->handler_depth : 0u;
}

void cvm_handler_pop_to(u32 depth) {
    CvmVMContext* ctx = cvm_ctx();
    if (ctx == NULL) {
        return;
    }
    while (ctx->handler_depth > depth) {
        cvm_handler_pop();
    }
}

opaque_mut cvm_handler_begin_catch(void) {
    CvmHandler* handler = cvm_handler_top();
    if (handler == NULL) {
        return NULL;
    }
    handler->caught = true;
    return handler->exception;
}

void cvm_throw(opaque_mut exception) {
    CvmVMContext* ctx = cvm_ctx();
    CvmHandler* handler = (ctx != NULL) ? ctx->handlers : NULL;

    /*
     * Skip the catch bodies being unwound out of. Their handlers are still
     * on the chain -- that is what roots the value each of them is holding --
     * so an unqualified "top handler" would mean a re-throw from inside a
     * catch lands in the same catch.
     */
    while (handler != NULL && handler->caught) {
        handler = handler->prev;
    }
    if (handler == NULL) {
        rt_fatal(STR_LITERAL("uncaught exception: no handler is registered"));
    }

    /*
     * And take those skipped records off the chain, because their
     * CVM_ENDTRY will never run: the catch body that would have reached it
     * is being abandoned right now. Waiting for a pop that cannot come would
     * leave them linked, so the next try would find its throw going to a
     * handler whose frame is gone.
     */
    while (ctx->handlers != handler) {
        cvm_handler_pop();
    }

    /*
     * The value is stored first, and the order is the whole of the safety
     * argument rather than a preference.
     *
     * `cvm_mark_roots` walks this chain and marks `exception`, so the moment
     * this store lands the value is reachable to a collection. Restoring the
     * depths first would open a window where the throwing activations are
     * already gone from the frame set while the value is still in nobody's
     * root: a collection there reclaims the exception, and the catch reads
     * freed memory. There is no poll in that window today, which is what
     * makes the bug latent rather than absent -- the reason to hold the
     * order is that the window stops being harmless the first time anything
     * is added between the two stores.
     */
    handler->exception = exception;

    /*
     * What a landing pad cannot know: how much of cvm's own state belonged
     * to the activations being abandoned. Restoring the frame depth drops
     * every frame the try body pushed, and the frames of the throwing
     * activations with them; the unmanaged depth does the same for foreign
     * calls. Both are integers, and nothing here points into stack that is
     * about to be left behind.
     */
    ctx->frame_depth = handler->frame_depth;
    cvm_thread_restore_unmanaged_depth(handler->unmanaged_depth);

    rt_jump_to(&handler->jump);
}
