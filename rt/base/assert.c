/*
 * rt/base/assert.c
 *
 * The out-of-line half of cvm_assert (see base/assert.h).
 *
 * This lives in the rt layer because reporting a failure means writing to
 * a file descriptor and aborting the process — both platform operations.
 * rt is the one layer allowed to use libc, so this uses stdio for now.
 *
 * The eventual PAL version writes with write(2) and raises SIGABRT
 * directly, which drops the stdio dependency and lets rt itself become
 * freestanding. Nothing outside this file depends on that choice.
 */

#include "base/assert.h"

#include <stdio.h>
#include <stdlib.h>

void rt_assert_fail(str file, u32 line, str expr) {
    /* str is not NUL-terminated: every write is length-delimited. */
    fputs("assertion failed: ", stderr);
    fwrite(expr.ptr, 1, expr.len, stderr);
    fputc('\n', stderr);

    fputs("    at ", stderr);
    fwrite(file.ptr, 1, file.len, stderr);
    fprintf(stderr, ":%u\n", line);

    abort();
}
