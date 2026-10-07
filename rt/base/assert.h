/*
 * rt/base/assert.h
 *
 * The runtime's only assertion facility.
 *
 * Design notes:
 *
 * - Compiled out unless CVM_DEBUG is defined, so the argument must not
 *   have side effects. The macro is written so that the expression is not
 *   even parsed in release builds.
 *
 * - The failure path is out of line: printing needs a write syscall and
 *   aborting needs a signal, both of which are rt concerns. Keeping them
 *   behind a call also keeps hot paths small.
 *
 * - Messages are `str`, not C strings, in keeping with the rest of the
 *   codebase. STR_LITERAL works on __FILE__ and on the stringified
 *   condition because both are string literals.
 */

#ifndef RT_BASE_ASSERT_H
#define RT_BASE_ASSERT_H

#include "base/types.h"

/*
 * Provided by the rt layer. Does not return.
 * `file` and `expr` are not NUL-terminated.
 */
CVM_NORETURN void rt_assert_fail(str file, u32 line, str expr);

#if defined(CVM_DEBUG)

#define cvm_assert(cond)                                                               \
    ((cond)                                                                            \
         ? (void)0                                                                     \
         : rt_assert_fail(STR_LITERAL(__FILE__), (u32)__LINE__, STR_LITERAL(#cond)))

/*
 * An assertion whose message is a custom explanation rather than the
 * condition itself. Same rules: no side effects, debug only.
 */
#define cvm_assert_msg(cond, msg)                                                      \
    ((cond) ? (void)0                                                                  \
            : rt_assert_fail(STR_LITERAL(__FILE__), (u32)__LINE__, STR_LITERAL(msg)))

#else

#define cvm_assert(cond) ((void)0)
#define cvm_assert_msg(cond, msg) ((void)0)

#endif /* CVM_DEBUG */

#endif /* RT_BASE_ASSERT_H */
