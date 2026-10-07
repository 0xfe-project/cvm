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
/* memory                                                              */
/* ------------------------------------------------------------------ */

/*
 * General-purpose allocation for runtime bookkeeping: thread registry
 * nodes, per-thread buffers, that sort of thing.
 *
 * Not for GC heap objects. The heap is one large reservation, carved up
 * by the collector itself; allocating objects here would put them outside
 * the heap and invisible to the bytemap.
 */
opaque_mut rt_mem_alloc(usize bytes);

/* Release memory from rt_mem_alloc. NULL is accepted and ignored. */
void rt_mem_free(opaque_mut p);

/*
 * Reserve a range of address space, and commit it.
 *
 * This is what the GC heap is built on, and the distinction from
 * rt_mem_alloc is the whole point: a heap whose blocks are individually
 * allocated cannot be described by address arithmetic, because there is
 * no relationship between one block's address and the next.
 *
 * reserve returns NULL on failure. The memory is not usable until
 * rt_mem_commit succeeds for the range. On systems that over-commit
 * (every POSIX), reserve already gives usable memory and commit is a
 * no-op; on Windows the two are genuinely separate, which is why the
 * split exists in the ABI rather than being folded into one call.
 */
opaque_mut rt_mem_reserve(usize bytes);
bool rt_mem_commit(opaque_mut base, usize bytes);

/* Return the whole reservation. The size must match the reservation. */
void rt_mem_release(opaque_mut base, usize bytes);

/*
 * Granularity of the smallest independently usable mapping. Used to size
 * a reservation so that it can be aligned up without wasting a whole
 * extra page.
 */
usize rt_mem_page_size(void);

/* ------------------------------------------------------------------ */
/* threads and synchronisation                                         */
/* ------------------------------------------------------------------ */

/*
 * These are opaque storage, not platform types.
 *
 * The collector embeds its locks by value in its own structures, so the
 * storage has to be a complete type with a known size. But it must not be
 * a pthread_mutex_t: that would put a libc type into a freestanding
 * header and make the ABI platform-specific.
 *
 * So each one is a fixed-size, suitably aligned byte block, and the
 * implementation static-asserts that the real platform type fits inside.
 * The sizes below are generous for every platform we target; the asserts
 * are what keeps that honest rather than hopeful.
 */
#define RT_THREAD_STORAGE 16
#define RT_MUTEX_STORAGE 64
#define RT_COND_STORAGE 64
#define RT_RWLOCK_STORAGE 208

/* All platform primitives we use are 8-byte aligned; 16 is free here
 * because every storage size above is a multiple of it. */
#define RT_SYNC_ALIGN 16

typedef struct RtThread {
    _Alignas(RT_SYNC_ALIGN) u8 storage[RT_THREAD_STORAGE];
} RtThread;

typedef struct RtMutex {
    _Alignas(RT_SYNC_ALIGN) u8 storage[RT_MUTEX_STORAGE];
} RtMutex;

typedef struct RtCond {
    _Alignas(RT_SYNC_ALIGN) u8 storage[RT_COND_STORAGE];
} RtCond;

typedef struct RtRwlock {
    _Alignas(RT_SYNC_ALIGN) u8 storage[RT_RWLOCK_STORAGE];
} RtRwlock;

/* ------------------------------------------------------------------ */
/* threads                                                             */
/* ------------------------------------------------------------------ */

typedef opaque_mut (*RtThreadFn)(opaque_mut);

/* Start a thread. Returns false if the thread could not be created. */
bool rt_thread_create(RtThread* out, RtThreadFn fn, opaque_mut arg);

/* Wait for a thread to finish. */
bool rt_thread_join(RtThread thread);

/* Release our handle without waiting. The thread keeps running. */
void rt_thread_detach(RtThread thread);

RtThread rt_thread_self(void);
bool rt_thread_equal(RtThread a, RtThread b);

/* Numeric identity, for log lines. Not necessarily unique across time: a
 * finished thread's id may be reused. */
u64 rt_thread_id(void);

/*
 * Whether a thread is still running.
 *
 * Used to detect a thread that died without detaching from the collector,
 * which would otherwise leave the registry waiting forever for a safepoint
 * that can never arrive.
 */
bool rt_thread_is_alive(RtThread thread);

/* Give up the rest of the time slice. */
void rt_thread_yield(void);

/* Usable parallelism, at least 1. */
u32 rt_hardware_threads(void);

u32 rt_process_id(void);

/* ------------------------------------------------------------------ */
/* mutex                                                               */
/* ------------------------------------------------------------------ */

/*
 * A plain mutex: not recursive.
 *
 * The reference implementation makes every mutex recursive, which hides
 * the mistake it is most likely to be used for — locking a lock the
 * current thread already holds — by turning a deadlock into silent
 * success. The default here is non-recursive, and recursion is opt-in
 * through rt_mutex_init_recursive so that a call site needing it says so.
 */
bool rt_mutex_init(RtMutex* m);

/* For the few places that genuinely re-enter, such as a lock taken by a
 * helper that is also called with the lock held. */
bool rt_mutex_init_recursive(RtMutex* m);

void rt_mutex_destroy(RtMutex* m);
void rt_mutex_lock(RtMutex* m);

/* false if the lock is held by someone else. */
bool rt_mutex_trylock(RtMutex* m);

void rt_mutex_unlock(RtMutex* m);

/* ------------------------------------------------------------------ */
/* condition variable                                                  */
/* ------------------------------------------------------------------ */

bool rt_cond_init(RtCond* c);
void rt_cond_destroy(RtCond* c);

/* Releases `m` while waiting and reacquires it before returning. */
void rt_cond_wait(RtCond* c, RtMutex* m);

/*
 * Wait until signalled, or until `timeout_ns` has passed.
 *
 * Returns true if it was signalled, false on timeout. The distinction
 * matters for the collector: a thread that never reaches a safepoint
 * would otherwise be waited for forever, with nothing to report.
 *
 * The timeout is relative rather than an absolute deadline, because the
 * clock a condition variable times against is not the same everywhere:
 * pthread_cond_timedwait defaults to CLOCK_REALTIME, and macOS has no way
 * to change that, so a monotonic deadline could not be expressed
 * portably.
 */
bool rt_cond_wait_timeout(RtCond* c, RtMutex* m, u64 timeout_ns);

void rt_cond_signal(RtCond* c);
void rt_cond_broadcast(RtCond* c);

/*
 * No semaphore type.
 *
 * The reference builds them from POSIX named semaphores, because macOS
 * does not implement unnamed ones (sem_init returns ENOSYS). Named
 * semaphores bring a name in a global namespace and a lifetime that
 * outlives the process if it is killed at the wrong moment. A counter plus
 * a mutex and a condition variable does the same job with none of that,
 * and every wake-up the collector needs is expressible that way.
 */

/* ------------------------------------------------------------------ */
/* reader-writer lock                                                  */
/* ------------------------------------------------------------------ */

/*
 * Used for the thread registry: readers walk the list while a writer adds
 * or removes a thread. Writers are given priority, so a stream of readers
 * cannot starve a registration.
 */
bool rt_rwlock_init(RtRwlock* l);
void rt_rwlock_destroy(RtRwlock* l);
void rt_rwlock_lock_read(RtRwlock* l);
void rt_rwlock_lock_write(RtRwlock* l);
void rt_rwlock_unlock_read(RtRwlock* l);
void rt_rwlock_unlock_write(RtRwlock* l);

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
