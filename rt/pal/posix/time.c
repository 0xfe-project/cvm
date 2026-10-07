/*
 * rt/pal/posix/time.c
 *
 * Monotonic clock, POSIX implementation.
 *
 * CLOCK_MONOTONIC is the right choice over CLOCK_REALTIME for anything
 * the collector measures: it does not jump when the system clock is
 * adjusted, so a collection duration can never come out negative.
 *
 * Windows would use QueryPerformanceCounter; that branch does not exist
 * yet, and the file is excluded on Windows rather than silently using a
 * clock with the wrong properties.
 */

#if !defined(_WIN32)

/* -std=c11 defines __STRICT_ANSI__, which hides the POSIX declarations in
 * the system headers unless the feature level is requested explicitly. */
#define _POSIX_C_SOURCE 200809L

#include <time.h>

#include "rt.h"

u64 rt_time_nanos(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

u64 rt_time_millis(void) {
    return rt_time_nanos() / 1000000ull;
}

#endif /* !_WIN32 */
