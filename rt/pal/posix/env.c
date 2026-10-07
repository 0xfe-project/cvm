/*
 * rt/pal/posix/env.c
 *
 * Environment lookup, POSIX implementation.
 */

#if !defined(_WIN32)

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include "rt.h"

/* Names are short by nature; a longer one is not a variable we set. */
#define RT_ENV_NAME_MAX 128

str rt_env_get(str name) {
    static char buf[RT_ENV_NAME_MAX];

    if (name.len == 0 || name.len >= RT_ENV_NAME_MAX) {
        return (str){NULL, 0};
    }
    for (usize i = 0; i < name.len; i++) {
        buf[i] = (char)name.ptr[i];
    }
    buf[name.len] = '\0';

    const char* value = getenv(buf);
    if (value == NULL) {
        return (str){NULL, 0};
    }

    return (str){(u8 const*)value, strlen(value)};
}

#endif /* !_WIN32 */
