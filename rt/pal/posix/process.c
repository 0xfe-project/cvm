/*
 * rt/pal/posix/process.c
 *
 * Process termination, POSIX implementation.
 */

#if !defined(_WIN32)

#define _POSIX_C_SOURCE 200809L

#include <signal.h>
#include <unistd.h>

#include "rt.h"

CVM_NORETURN void rt_abort(void) {
    /* SIGABRT rather than exit(3): exit runs atexit handlers, which for a
     * collector failure means running arbitrary user code in a state the
     * collector has already declared unrecoverable. A core dump is also
     * more useful than an orderly shutdown. */
    raise(SIGABRT);
    _exit(134); /* unreachable unless SIGABRT is blocked or handled */
}

CVM_NORETURN void rt_fatal(str message) {
    /* Written directly rather than through the logging layer: this path
     * runs before any channel or log level exists. */
    static const u8 prefix[] = "cvm: fatal: ";
    (void)write(2, prefix, sizeof(prefix) - 1);
    (void)write(2, message.ptr, message.len);
    (void)write(2, "\n", 1);
    rt_abort();
}

#endif /* !_WIN32 */
