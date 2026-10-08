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

/* ------------------------------------------------------------------ */
/* counting semaphore                                                  */
/* ------------------------------------------------------------------ */

/*
 * A counting semaphore: a counter plus a mutex and a condition variable.
 *
 * The reference builds these from POSIX named semaphores, because macOS
 * does not implement unnamed ones (sem_init returns ENOSYS). Named
 * semaphores bring a name in a global namespace and a lifetime that
 * outlives the process if it is killed at the wrong moment. A counter
 * does the same job with neither, and every wake-up the collector needs
 * is a count: "this many more threads should run".
 *
 * `post` is what makes it more than a condition variable — the count
 * survives between waits, so a post that arrives before a wait is not
 * lost.
 */
typedef struct RtSem {
    RtMutex mutex;
    RtCond cond;
    u32 count;
} RtSem;

bool rt_sem_init(RtSem* sem, u32 initial);
void rt_sem_destroy(RtSem* sem);

/* Increment and wake one waiter. */
void rt_sem_post(RtSem* sem);

/* Decrement, blocking while the count is zero. */
void rt_sem_wait(RtSem* sem);

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
/* trap pages                                                          */
/* ------------------------------------------------------------------ */

/*
 * A page that can be made inaccessible, used as a safepoint signal.
 *
 * The trick it exists for: instead of generated code polling a flag and
 * branching, it loads from this page. While the page is armed a load
 * faults, and the fault handler stops the thread. So a safepoint costs one
 * load in the common case, and there is no branch and nothing for the
 * compiler to reorder around.
 *
 * The page's contents are irrelevant; the page itself is the signal. That
 * is why arming and disarming are the only operations.
 *
 * Address-space note: a released page is never unmapped and returned to
 * the system. Generated code is allowed to cache the cell's address in a
 * register or spill it across a call, and a stale address to an unmapped
 * page would fault in a way the handler cannot distinguish from a real
 * one. Disarmed pages are recycled through a free list instead. This
 * matters less than it does in the reference — which has to survive a
 * continuation resuming on a different thread — but the reasoning holds
 * for any hoisted load, and the cost is one page per thread that ever
 * existed only if threads come and go in large numbers.
 */
/*
 * An incomplete type rather than a type-erased pointer: there is exactly
 * one kind of thing this can be, so the compiler can say so.
 */
typedef struct RtTrapPage RtTrapPage;

/* Create a page, disarmed. Returns NULL on failure. */
RtTrapPage* rt_trap_page_create(void);

/* Make loads through the page fault. */
void rt_trap_page_arm(RtTrapPage* page);

/* Make loads through the page succeed. */
void rt_trap_page_disarm(RtTrapPage* page);

/* Return the page for reuse. Never unmaps it; see above. */
void rt_trap_page_release(RtTrapPage* page);

/* The address to poll: loading through this pointer faults when armed. */
opaque_mut rt_trap_page_cell(RtTrapPage* page);

/* ------------------------------------------------------------------ */
/* fault handling                                                      */
/* ------------------------------------------------------------------ */

/*
 * Called when a trap page is read while armed.
 *
 * `context` is the platform's saved machine state for the faulting
 * thread, opaque to the caller. `fault_address` is what was read.
 *
 * Returning true means the fault was handled and the faulting instruction
 * should be retried; returning false passes the fault on, so a genuine
 * segmentation fault is still reported as one.
 */
typedef bool (*RtTrapFaultFn)(opaque_mut context, u64 fault_address, opaque_mut user);

/*
 * Install the handler. Returns false if it could not be installed, in
 * which case trap-based safepoints cannot be used and the caller should
 * fall back to polling.
 */
bool rt_trap_handler_install(RtTrapFaultFn on_fault, opaque_mut user);

/*
 * The faulting instruction's address, and a way to change where execution
 * resumes.
 *
 * This is what makes the mechanism work without doing anything
 * signal-unsafe: the handler does not stop the thread, it repoints the
 * saved instruction pointer at a stub that does, and returns. The stub
 * runs on the faulting thread's own stack in ordinary context, where
 * taking a lock is allowed.
 */
u64 rt_trap_context_pc(opaque_mut context);
void rt_trap_context_set_pc(opaque_mut context, u64 pc);

/*
 * Apple delivers a read of an inaccessible page to task-level Mach
 * exception ports before turning it into a signal, and debuggers and some
 * frameworks install such ports. Clearing them restores the signal path.
 *
 * Called before arming, because a library can install a port after
 * startup. A no-op elsewhere.
 */
void rt_trap_reset_platform_exception_ports(void);

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
