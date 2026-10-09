/*
 * vm/exc.h -- handlers, and throwing.
 *
 * The shape is taken from exceptions4c, and two of its decisions are worth
 * naming because they are load-bearing rather than stylistic.
 *
 * 1. The handler record is allocated by the runtime, not by the site and not
 *    from a fixed per-thread array. A fixed array has a capacity, and a
 *    program that runs out of it has to be told so at the one moment it
 *    cannot do anything about it. A record per try has no such limit; the
 *    free list below is what keeps that from being an allocation per try.
 *
 * 2. The thrown value lives *in the record*, and it is written before
 *    anything else happens. So "root the exception, then restore the state"
 *    is not an ordering a caller has to remember -- by the time the state
 *    moves, the value is already somewhere the collector walks (see
 *    mark_handler_chain in vm/gc/marker.c).
 *
 * What e4c cannot know, and this can, is cvm's own unwinding state: the
 * frame depth (vm/include/cvm/gc.h) and the unmanaged depth (vm/context.c)
 * of the activation the handler was pushed in. Both are integers, so
 * restoring them is a store each, and nothing points into a stack that is
 * going away.
 *
 * The landing pad itself -- whether control gets back to the catch by
 * longjmp, by a return status, or by a table the frontend indexes -- is not
 * part of this contract. It is part of the frontend's, and rt_jump.h is the
 * one place a mechanism is chosen.
 */
#if !defined(CVM_EXC_H)
#define CVM_EXC_H

#include "cvm/gc.h"
#include "rt.h"

/*
 * One `try`. Named by no caller: the runtime owns it, links it into the
 * thread's chain, and hands back only the jump target.
 */
struct CvmHandler {
    struct CvmHandler* prev;

    /*
     * Where to resume. Embedded rather than pointed to, so a site has no
     * storage to own and no local to keep valid across the jump -- which
     * also sidesteps the rule that a non-volatile local modified between
     * setjmp and longjmp has an indeterminate value.
     */
    RtJumpBuf jump;

    /* What was thrown. NULL when the runtime raised without a value. */
    opaque_mut exception;

    /*
     * Set while this handler's catch body is running, and cleared when the
     * record is reused.
     *
     * A handler stays on the chain for the whole catch, because that is what
     * roots the value the catch is working with -- a local in the catching
     * function is not a root, and reading the value back from a record that
     * had been unlinked would be reading it from nothing.
     *
     * The flag is what keeps that from meaning "catch your own re-throw": a
     * throw skips handlers whose catch is already running, and the ones it
     * skips are exactly the catch bodies it is unwinding out of.
     */
    bool caught;

    /* cvm's state at the point the try was entered. See above. */
    u32 frame_depth;
    u32 unmanaged_depth;
};

/* Link a new handler into this thread's chain and return its jump target. */
RtJumpBuf* cvm_handler_push(void);

/* Unlink the innermost handler. The chain is LIFO and balanced, so a pop is
 * always the handler that is on top. */
void cvm_handler_pop(void);

CvmHandler* cvm_handler_top(void);

/*
 * How many handlers are linked, and how to take the chain back to a depth.
 *
 * These exist for one case, and it is the case that makes a `return` inside a
 * try body safe. `CVM_ENDTRY` is what pops a handler, and a `return` out of
 * the body never reaches it: the record would stay linked to a function that
 * has returned, and the next throw in that thread would resume at its
 * landing pad -- a jump into a frame that no longer exists.
 *
 * So a function that can run a try saves the depth at entry and restores it
 * on the way out, exactly as it does for frames. The count is what makes
 * that a store rather than a walk.
 */
u32 cvm_handler_depth(void);
void cvm_handler_pop_to(u32 depth);

/*
 * Mark the innermost handler as catching and hand back what it holds. Called
 * where a catch body begins, and only there.
 */
opaque_mut cvm_handler_begin_catch(void);

/*
 * Hand `exception` to the innermost handler and resume there. Never returns.
 * With no handler registered the process is terminated, because there is
 * nowhere to put the value and nothing that could act on it.
 */
CVM_NORETURN void cvm_throw(opaque_mut exception);

#endif /* CVM_EXC_H */
