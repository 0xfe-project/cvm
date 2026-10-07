/*
 * rt/rt.h
 *
 * The runtime's platform layer: the ABI that vm/ calls down into.
 *
 * Everything here is implemented per platform under rt/pal/. The ABI is
 * deliberately narrow and uses only types from rt/base/, so that:
 *
 *   - vm/ stays freestanding (it includes this header, not any libc one)
 *   - the platform implementation can change without touching vm/
 *   - libc types (FILE, pthread_t, size_t from a libc header) never leak
 *     upward
 *
 * rt/ is the one layer allowed to use libc. The eventual freestanding PAL
 * would replace the implementations with raw syscalls; the ABI would not
 * change.
 *
 * Note on str: file paths and log payloads are `str`, a pointer plus a
 * length, not a NUL-terminated C string. Implementations that must hand a
 * path to a libc call do the conversion internally.
 */

#ifndef RT_RT_H
#define RT_RT_H

#include "base/types.h"

/* ------------------------------------------------------------------ */
/* ABI version                                                         */
/* ------------------------------------------------------------------ */

/*
 * Bumped whenever the ABI below changes incompatibly. vm checks this at
 * init so a mismatched pair fails loudly instead of misbehaving. The
 * version is also exposed as a symbol so the check is not optimised into
 * a compile-time constant (which would defeat its purpose when the two
 * objects are built separately).
 */
#define RT_ABI_VERSION 1u

extern const u32 rt_abi_version;
const u32* rt_abi_version_ptr(void);

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */

/*
 * Monotonic clock, unaffected by system time adjustments. This is the
 * only clock the collector uses for measuring durations; wall-clock time
 * appears nowhere in a collection.
 *
 * Origin is unspecified; only differences are meaningful. The resolution
 * is whatever the platform offers, typically nanoseconds.
 */
u64 rt_time_nanos(void);

u64 rt_time_millis(void);

/* ------------------------------------------------------------------ */
/* logging and byte output                                             */
/* ------------------------------------------------------------------ */

/*
 * An output channel: a file descriptor or a file, wrapped so that callers
 * never see a FILE*.
 *
 * Not thread-safe at this level beyond what a single write(2) call gives
 * you; the logging layer above serialises per message.
 */
typedef struct RtChannel RtChannel;

/* Standard error. Always available; never fails. */
RtChannel* rt_channel_stderr(void);

/* Standard output. */
RtChannel* rt_channel_stdout(void);

/*
 * Open (creating or truncating) a file for writing. Returns NULL on
 * failure. The path is not NUL-terminated.
 */
RtChannel* rt_channel_open_file(str path);

/*
 * Append `bytes`. Returns false if the write did not complete. A single
 * call maps to a single write, so messages from different threads do not
 * interleave mid-line as long as callers pass whole lines.
 */
bool rt_channel_write(RtChannel* ch, span_const bytes);

void rt_channel_flush(RtChannel* ch);

/* Close a channel opened with rt_channel_open_file. The standard
 * channels are not closed by this and may be passed safely. */
void rt_channel_close(RtChannel* ch);

/* ------------------------------------------------------------------ */
/* process                                                             */
/* ------------------------------------------------------------------ */

/*
 * Environment lookup. Returns an empty str when the variable is unset,
 * so callers never deal with NULL.
 *
 * The returned bytes are owned by the environment and must not be
 * written. They are not NUL-terminated, like every other str.
 */
str rt_env_get(str name);

/* Terminate without running atexit handlers. Used for unrecoverable
 * collector states, where running user cleanup could deadlock. */
CVM_NORETURN void rt_abort(void);

/* Write a message to stderr and terminate. For failures that happen
 * before any channel exists. */
CVM_NORETURN void rt_fatal(str message);

#endif /* RT_RT_H */
