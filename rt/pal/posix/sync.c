/*
 * rt/pal/posix/sync.c
 *
 * Threads, mutexes, condition variables and reader-writer locks, POSIX
 * implementation.
 *
 * The ABI in rt.h presents these as opaque byte blocks so that vm can
 * embed them by value without seeing a pthread type. That means every
 * function here casts a byte block to the real platform type, and the
 * static assertions at the top are what make that cast safe rather than
 * hopeful: if a platform's type grows past the storage, the build fails
 * here instead of corrupting a neighbouring field at runtime.
 *
 * `_POSIX_C_SOURCE` is set because -std=c11 defines __STRICT_ANSI__, which
 * hides the POSIX declarations in the system headers.
 */

#if !defined(_WIN32)

#define _POSIX_C_SOURCE 200809L

#if defined(__APPLE__)
/* Darwin hides sysctlbyname behind its full-namespace guard, and
 * _POSIX_C_SOURCE alone selects the strict POSIX subset. */
#define _DARWIN_C_SOURCE 1
#include <sys/sysctl.h>
#include <sys/types.h>
#endif

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "rt.h"

/* ------------------------------------------------------------------ */
/* storage must fit the platform types                                 */
/* ------------------------------------------------------------------ */

_Static_assert(sizeof(pthread_t) <= RT_THREAD_STORAGE,
               "RT_THREAD_STORAGE is too small for pthread_t");
_Static_assert(sizeof(pthread_mutex_t) <= RT_MUTEX_STORAGE,
               "RT_MUTEX_STORAGE is too small for pthread_mutex_t");
_Static_assert(sizeof(pthread_cond_t) <= RT_COND_STORAGE,
               "RT_COND_STORAGE is too small for pthread_cond_t");
_Static_assert(sizeof(pthread_rwlock_t) <= RT_RWLOCK_STORAGE,
               "RT_RWLOCK_STORAGE is too small for pthread_rwlock_t");

_Static_assert(_Alignof(pthread_t) <= RT_SYNC_ALIGN,
               "RT_SYNC_ALIGN is too small for pthread_t");
_Static_assert(_Alignof(pthread_mutex_t) <= RT_SYNC_ALIGN,
               "RT_SYNC_ALIGN is too small for pthread_mutex_t");
_Static_assert(_Alignof(pthread_cond_t) <= RT_SYNC_ALIGN,
               "RT_SYNC_ALIGN is too small for pthread_cond_t");
_Static_assert(_Alignof(pthread_rwlock_t) <= RT_SYNC_ALIGN,
               "RT_SYNC_ALIGN is too small for pthread_rwlock_t");

/* ------------------------------------------------------------------ */
/* casts                                                               */
/* ------------------------------------------------------------------ */

static pthread_mutex_t* mutex_of(RtMutex* m) {
    return (pthread_mutex_t*)(void*)m->storage;
}

static pthread_cond_t* cond_of(RtCond* c) {
    return (pthread_cond_t*)(void*)c->storage;
}

static pthread_rwlock_t* rwlock_of(RtRwlock* l) {
    return (pthread_rwlock_t*)(void*)l->storage;
}

/* ------------------------------------------------------------------ */
/* threads                                                             */
/* ------------------------------------------------------------------ */

/*
 * pthread_create takes a function returning void*, and so does our ABI.
 * The signatures line up, so no trampoline is needed — unlike the
 * reference, which wraps the routine to register the thread with the
 * collector first. That registration belongs in vm, which will pass a
 * trampoline of its own.
 */
bool rt_thread_create(RtThread* out, RtThreadFn fn, opaque_mut arg) {
    pthread_t* t = (pthread_t*)(void*)out->storage;
    return pthread_create(t, NULL, (void* (*)(void*))fn, arg) == 0;
}

bool rt_thread_join(RtThread thread) {
    pthread_t t = *(pthread_t const*)(void const*)thread.storage;
    return pthread_join(t, NULL) == 0;
}

void rt_thread_detach(RtThread thread) {
    pthread_t t = *(pthread_t const*)(void const*)thread.storage;
    pthread_detach(t);
}

RtThread rt_thread_self(void) {
    RtThread out;
    pthread_t self = pthread_self();
    for (usize i = 0; i < sizeof(self) && i < sizeof(out.storage); i++) {
        out.storage[i] = ((u8 const*)&self)[i];
    }
    return out;
}

bool rt_thread_equal(RtThread a, RtThread b) {
    pthread_t pa = *(pthread_t const*)(void const*)a.storage;
    pthread_t pb = *(pthread_t const*)(void const*)b.storage;
    return pthread_equal(pa, pb) != 0;
}

bool rt_thread_is_alive(RtThread thread) {
    /*
     * Signal 0 performs the permission and existence checks without
     * delivering anything, which is the standard way to ask "is this
     * thread still there".
     */
    pthread_t t = *(pthread_t const*)(void const*)thread.storage;
    return pthread_kill(t, 0) == 0;
}

u64 rt_thread_id(void) {
    /* Cast through uintptr_t rather than using pthread_t directly: it is
     * opaque, and on some platforms it is a pointer. Only used for log
     * lines, so the exact value does not matter. */
    return (u64)(uintptr_t)pthread_self();
}

void rt_thread_yield(void) {
    sched_yield();
}

void rt_cpu_relax(void) {
    /*
     * The architecture's spin hint: a few cycles of "I am waiting, give the
     * other hyperthread the execution resources" rather than a system call.
     *
     * This exists because the collector's wait-for-workers loop polls a set
     * of flags, and sched_yield() on every iteration makes that polling a
     * syscall per iteration. A worker finishing a mark batch takes tens of
     * microseconds; the loop can afford to spin for a while before asking
     * the kernel for anything.
     */
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#else
    atomic_thread_fence(memory_order_seq_cst);
#endif
}

u32 rt_hardware_threads(void) {
#if defined(__APPLE__)
    /* sysconf(_SC_NPROCESSORS_ONLN) is not visible under a strict POSIX
     * feature level on Darwin, so ask the sysctl interface instead. */
    int ncpu = 0;
    usize len = sizeof(ncpu);
    if (sysctlbyname("hw.logicalcpu", &ncpu, &len, NULL, 0) == 0 && ncpu > 0) {
        return (u32)ncpu;
    }
    return 1u;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return (n > 0) ? (u32)n : 1u;
#endif
}

u32 rt_process_id(void) {
    return (u32)getpid();
}

/* ------------------------------------------------------------------ */
/* mutex                                                               */
/* ------------------------------------------------------------------ */

static bool mutex_init_with_type(RtMutex* m, int type) {
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        return false;
    }
    if (pthread_mutexattr_settype(&attr, type) != 0) {
        pthread_mutexattr_destroy(&attr);
        return false;
    }
    int rc = pthread_mutex_init(mutex_of(m), &attr);
    pthread_mutexattr_destroy(&attr);
    return rc == 0;
}

bool rt_mutex_init(RtMutex* m) {
    return mutex_init_with_type(m, PTHREAD_MUTEX_NORMAL);
}

bool rt_mutex_init_recursive(RtMutex* m) {
    return mutex_init_with_type(m, PTHREAD_MUTEX_RECURSIVE);
}

void rt_mutex_destroy(RtMutex* m) {
    pthread_mutex_destroy(mutex_of(m));
}

void rt_mutex_lock(RtMutex* m) {
    /* No return value in the ABI: for a well-formed mutex this cannot
     * fail, and the failures pthread reports (EINVAL, EDEADLK) mean the
     * caller has already lost. */
    (void)pthread_mutex_lock(mutex_of(m));
}

bool rt_mutex_trylock(RtMutex* m) {
    return pthread_mutex_trylock(mutex_of(m)) == 0;
}

void rt_mutex_unlock(RtMutex* m) {
    (void)pthread_mutex_unlock(mutex_of(m));
}

/* ------------------------------------------------------------------ */
/* condition variable                                                  */
/* ------------------------------------------------------------------ */

bool rt_cond_init(RtCond* c) {
    return pthread_cond_init(cond_of(c), NULL) == 0;
}

void rt_cond_destroy(RtCond* c) {
    pthread_cond_destroy(cond_of(c));
}

void rt_cond_wait(RtCond* c, RtMutex* m) {
    (void)pthread_cond_wait(cond_of(c), mutex_of(m));
}

bool rt_cond_wait_timeout(RtCond* c, RtMutex* m, u64 timeout_ns) {
    /*
     * pthread_cond_timedwait takes an absolute deadline, and it times
     * against CLOCK_REALTIME unless the condition variable was created
     * with a monotonic clock attribute. glibc supports that attribute;
     * macOS does not, so the portable spelling is a realtime deadline
     * computed from the current realtime clock.
     *
     * A clock jump during the wait can therefore shorten or lengthen it.
     * That is acceptable here: this wait exists to notice a thread that is
     * not reaching a safepoint, so an imprecise timeout costs a
     * mis-timed diagnostic, not a correctness problem.
     */
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
        return false;
    }

    u64 secs = timeout_ns / 1000000000ull;
    u64 nanos = timeout_ns % 1000000000ull;

    deadline.tv_sec += (time_t)secs;
    deadline.tv_nsec += (long)nanos;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_nsec -= 1000000000L;
        deadline.tv_sec += 1;
    }

    int rc = pthread_cond_timedwait(cond_of(c), mutex_of(m), &deadline);
    return rc == 0;
}

void rt_cond_signal(RtCond* c) {
    (void)pthread_cond_signal(cond_of(c));
}

void rt_cond_broadcast(RtCond* c) {
    (void)pthread_cond_broadcast(cond_of(c));
}

/* ------------------------------------------------------------------ */
/* reader-writer lock                                                  */
/* ------------------------------------------------------------------ */

bool rt_rwlock_init(RtRwlock* l) {
    pthread_rwlockattr_t attr;
    if (pthread_rwlockattr_init(&attr) != 0) {
        return false;
    }

    /*
     * Writers first. Without this a steady stream of readers can keep a
     * registering thread out indefinitely, which for a thread registry
     * means a new thread never becomes visible to a collection.
     *
     * Setting the preference explicitly is a glibc extension. macOS has no
     * such attribute because its implementation is already
     * writer-preferring, so the default is what we want there.
     */
#if defined(__linux__)
    (void)pthread_rwlockattr_setkind_np(&attr,
                                        PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif

    int rc = pthread_rwlock_init(rwlock_of(l), &attr);
    pthread_rwlockattr_destroy(&attr);
    return rc == 0;
}

void rt_rwlock_destroy(RtRwlock* l) {
    pthread_rwlock_destroy(rwlock_of(l));
}

void rt_rwlock_lock_read(RtRwlock* l) {
    (void)pthread_rwlock_rdlock(rwlock_of(l));
}

void rt_rwlock_lock_write(RtRwlock* l) {
    (void)pthread_rwlock_wrlock(rwlock_of(l));
}

void rt_rwlock_unlock_read(RtRwlock* l) {
    (void)pthread_rwlock_unlock(rwlock_of(l));
}

void rt_rwlock_unlock_write(RtRwlock* l) {
    (void)pthread_rwlock_unlock(rwlock_of(l));
}

/* ------------------------------------------------------------------ */
/* counting semaphore                                                  */
/* ------------------------------------------------------------------ */

bool rt_sem_init(RtSem* sem, u32 initial) {
    if (!rt_mutex_init(&sem->mutex) || !rt_cond_init(&sem->cond)) {
        return false;
    }
    sem->count = initial;
    return true;
}

void rt_sem_destroy(RtSem* sem) {
    rt_cond_destroy(&sem->cond);
    rt_mutex_destroy(&sem->mutex);
}

void rt_sem_post(RtSem* sem) {
    rt_mutex_lock(&sem->mutex);
    sem->count++;
    rt_cond_signal(&sem->cond);
    rt_mutex_unlock(&sem->mutex);
}

void rt_sem_wait(RtSem* sem) {
    rt_mutex_lock(&sem->mutex);
    while (sem->count == 0) {
        rt_cond_wait(&sem->cond, &sem->mutex);
    }
    sem->count--;
    rt_mutex_unlock(&sem->mutex);
}

#endif /* !_WIN32 */
