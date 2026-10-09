/*
 * rt/rt_jump.h -- the setjmp half of non-local exit.
 *
 * This is a separate header, and that is forced rather than chosen.
 *
 * `setjmp` has to execute in the function that will be jumped to. What it
 * saves is that function's stack pointer and its callee-saved registers; a
 * function that called it would save the callee's context instead, and the
 * jump would land in a frame that has already returned. So this cannot be
 * something the runtime calls -- it has to be a macro that expands at the
 * site, which puts the platform declaration into the translation unit that
 * uses it, and that unit needs <setjmp.h>.
 *
 * rt.h is the boundary that keeps libc out of everything above it, so the
 * macro goes here. Code that emits a `try` includes this header and nothing
 * else does. vm/ is built freestanding and cannot see it at all, which is
 * why cvm_throw (vm/exc.c) only ever calls the other half of this pair.
 *
 * sigsetjmp(env, 0) rather than setjmp. They are the same machine operation
 * when the signal mask is not saved, but setjmp's behaviour is
 * implementation-defined and some libcs make `setjmp` save the mask, which
 * is a sigprocmask syscall per try entry. Asking for "do not save the mask"
 * explicitly costs nothing and says which one is meant.
 */
#if !defined(CVM_RT_JUMP_H)
#define CVM_RT_JUMP_H

#include <setjmp.h>

#include "rt.h"

/*
 * The platform's buffer, seen through the opaque storage in RtJumpBuf. A
 * cast rather than a field because jmp_buf is an array type, and because
 * keeping it out of the header is the point of the storage being opaque.
 */
CVM_INLINE sigjmp_buf* rt_jump_native(RtJumpBuf* env) {
    return (sigjmp_buf*)(void*)env->storage;
}

/* 0 on the way in, non-zero when rt_jump_to returns here. */
#define rt_jump_set(env) (sigsetjmp(*rt_jump_native(env), 0))

#endif /* CVM_RT_JUMP_H */
