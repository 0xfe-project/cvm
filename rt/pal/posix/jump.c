/*
 * rt/pal/posix/jump.c
 *
 * The jump half of non-local exit. The setjmp half is a macro in
 * rt/rt_jump.h rather than a function here, for the reason recorded there:
 * it has to run in the function that will be jumped to.
 *
 * Windows would use its own longjmp, but this file is excluded there rather
 * than assumed, the way the rest of the platform layer is.
 */

#if !defined(_WIN32)

#include "rt.h"
#include "rt_jump.h"

void rt_jump_to(RtJumpBuf* env) {
    siglongjmp(*rt_jump_native(env), 1);
}

bool rt_jump_storage_is_sufficient(void) {
    return sizeof(sigjmp_buf) <= (usize)RT_JUMP_STORAGE &&
           _Alignof(sigjmp_buf) <= (usize)RT_JUMP_ALIGN;
}

#endif /* !_WIN32 */
