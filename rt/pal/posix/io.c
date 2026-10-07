/*
 * rt/pal/posix/io.c
 *
 * Output channels, POSIX implementation.
 *
 * Deliberately built on open/write/close rather than stdio. stdio would
 * bring buffering that the collector does not want (log lines must appear
 * even if the process is killed mid-collection) and a FILE* in the
 * interface, which the ABI forbids.
 *
 * A message is written with a single write(2) call. That is what makes
 * concurrent logging from several threads come out as whole lines: writes
 * below PIPE_BUF are atomic with respect to one another on a regular file
 * or a pipe.
 */

#if !defined(_WIN32)

#define _POSIX_C_SOURCE 200809L

#include <fcntl.h>
#include <unistd.h>

#include "rt.h"

struct RtChannel {
    i32 fd;
    bool owns_fd;
};

static RtChannel rt_stderr_channel = {.fd = 2, .owns_fd = false};
static RtChannel rt_stdout_channel = {.fd = 1, .owns_fd = false};

RtChannel* rt_channel_stderr(void) {
    return &rt_stderr_channel;
}

RtChannel* rt_channel_stdout(void) {
    return &rt_stdout_channel;
}

/*
 * Convert a path to a NUL-terminated buffer for open(2).
 *
 * The ABI passes `str` precisely so that callers never need to own a
 * terminated buffer; the conversion happens here, at the boundary, in a
 * fixed-size stack buffer. Paths longer than this are rejected rather
 * than truncated — a silently truncated path would open the wrong file.
 */
#define RT_PATH_MAX 4096

RtChannel* rt_channel_open_file(str path) {
    static RtChannel channel;
    char buf[RT_PATH_MAX];

    if (path.len == 0 || path.len >= RT_PATH_MAX) {
        return NULL;
    }
    for (usize i = 0; i < path.len; i++) {
        buf[i] = (char)path.ptr[i];
    }
    buf[path.len] = '\0';

    int fd = open(buf, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return NULL;
    }

    channel.fd = (i32)fd;
    channel.owns_fd = true;
    return &channel;
}

bool rt_channel_write(RtChannel* ch, span_const bytes) {
    if (ch == NULL || bytes.len == 0) {
        return bytes.len == 0;
    }

    usize written = 0;
    while (written < bytes.len) {
        ssize_t n = write(ch->fd, bytes.ptr + written, bytes.len - written);
        if (n < 0) {
            return false;
        }
        written += (usize)n;
    }
    return true;
}

void rt_channel_flush(RtChannel* ch) {
    /* Unbuffered by construction: there is nothing to flush. Kept in the
     * ABI because a buffered implementation would need it, and callers
     * should not have to change when that happens. */
    (void)ch;
}

void rt_channel_close(RtChannel* ch) {
    if (ch == NULL || !ch->owns_fd) {
        return;
    }
    close(ch->fd);
    ch->fd = -1;
    ch->owns_fd = false;
}

#endif /* !_WIN32 */
