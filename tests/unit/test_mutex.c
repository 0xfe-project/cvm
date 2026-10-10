/*
 * tests/unit/test_mutex.c
 *
 * The lock, and the one thing it has to get right: what a waiting thread tells
 * the collector.
 *
 * A thread blocked on a lock is not executing managed code and will not reach a
 * safepoint, and a collector cannot tell that from the outside — it sees a managed
 * thread that is not parked and waits for it. If the lock is held by a thread that
 * is itself waiting for the collection to end, both wait forever. So the wait is
 * declared unmanaged, which is a statement to the collector: do not wait for me.
 *
 * The tests below observe that declaration from another thread, because it is the
 * only part of this that is invisible from the waiting thread's own side. They are
 * also the regression test for the deadlock, and a deadlock in a test looks like a
 * hang rather than a failure: the collection that must complete is the one that
 * would be waiting for a thread that is waiting for a lock.
 */

#include "vendor/greatest.h"

#include <stdatomic.h>
#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "cvm/mutex.h"
#include "cvm/vm.h"
#include "gc/collect.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#if !defined(_WIN32)

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)32 * 1024 * 1024)

typedef struct Leaf {
    u64 canary;
} Leaf;

static CvmTypeDescriptor const leaf_desc = {
    .size = sizeof(Leaf),
    .kind = CVM_TYPE_SCALAR,
};

static CvmHeap* heap(void) {
    return &cvm_world()->heap;
}

static void collect(void) {
    cvm_gc_collect_and_finish(heap());
}

static void fixture_enter(void) {
    (void)cvm_thread_attach();
}

static void fixture_leave(void) {
    cvm_thread_detach();
}

/* How many threads are in the unmanaged state right now. */
static u32 unmanaged_threads(void) {
    u32 n = 0;
    CVM_THREADS_FOREACH(node) {
        if ((CvmThreadState)atomic_load_explicit(&node->state, memory_order_acquire) ==
            CVM_THREAD_UNMANAGED) {
            n++;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ */
/* a holder and a waiter, arranged so the waiter blocks                */
/* ------------------------------------------------------------------ */

typedef struct HoldArg {
    CvmMutex* m;
    bool raw_waiter;

    _Atomic bool held;
    _Atomic bool release;
    _Atomic u32 allocations;
    _Atomic bool waiter_started;
    _Atomic bool waiter_got;
} HoldArg;

/*
 * Holds the lock and keeps allocating, which is what makes it a holder the
 * collector can park while it holds it. That is the whole reason the wait needs
 * declaring: this thread may park while holding the lock.
 */
static opaque_mut holder_worker(opaque_mut raw) {
    HoldArg* arg = raw;
    if (!cvm_thread_attach()) {
        return NULL;
    }

    cvm_mutex_lock(arg->m);
    atomic_store(&arg->held, true);

    while (!atomic_load(&arg->release)) {
        void* p = cvm_alloc(&leaf_desc);
        (void)p;
        atomic_fetch_add(&arg->allocations, 1);
    }

    cvm_mutex_unlock(arg->m);
    cvm_thread_detach();
    return NULL;
}

static opaque_mut waiter_worker(opaque_mut raw) {
    HoldArg* arg = raw;
    if (!cvm_thread_attach()) {
        return NULL;
    }

    atomic_store(&arg->waiter_started, true);
    if (arg->raw_waiter) {
        cvm_mutex_lock_raw(arg->m);
    } else {
        cvm_mutex_lock(arg->m);
    }
    atomic_store(&arg->waiter_got, true);
    cvm_mutex_unlock(arg->m);
    cvm_thread_detach();
    return NULL;
}

/* Wait for a flag, with a bound so a broken build fails instead of hanging here. */
static bool wait_for(_Atomic bool* flag, u32 spins) {
    for (u32 i = 0; i < spins; i++) {
        if (atomic_load(flag)) {
            return true;
        }
        /* Allocating is also how this thread reaches a safepoint, which a
         * collection running in another thread needs it to do. */
        (void)cvm_alloc(&leaf_desc);
    }
    return atomic_load(flag);
}

static bool start_holder_and_waiter(HoldArg* arg, RtThread* holder, RtThread* waiter) {
    if (!rt_thread_create(holder, holder_worker, arg)) {
        return false;
    }
    if (!wait_for(&arg->held, 200000u)) {
        return false;
    }
    if (!rt_thread_create(waiter, waiter_worker, arg)) {
        return false;
    }
    return wait_for(&arg->waiter_started, 200000u);
}

static bool finish(HoldArg* arg, RtThread holder, RtThread waiter) {
    atomic_store(&arg->release, true);
    return cvm_thread_join(holder) && cvm_thread_join(waiter);
}

/* ------------------------------------------------------------------ */
/* the lock itself                                                     */
/* ------------------------------------------------------------------ */

TEST a_lock_excludes_and_a_trylock_does_not_wait(void) {
    CvmMutex m;
    ASSERT(cvm_mutex_init(&m));

    ASSERT(cvm_mutex_trylock(&m));
    /* Held by this thread, so a second attempt fails rather than blocking. */
    ASSERT(!cvm_mutex_trylock(&m));
    cvm_mutex_unlock(&m);
    ASSERT(cvm_mutex_trylock(&m));
    cvm_mutex_unlock(&m);

    cvm_mutex_destroy(&m);
    PASS();
}

TEST a_declared_wait_reads_as_unmanaged(void) {
    fixture_enter();
    CvmMutex m;
    ASSERT(cvm_mutex_init(&m));

    HoldArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.m = &m;
    arg.raw_waiter = false;

    RtThread holder, waiter;
    ASSERT(start_holder_and_waiter(&arg, &holder, &waiter));

    /*
     * The waiter is blocked on the lock now, and it has said so: exactly one
     * thread in the process is unmanaged, and it is the one that is waiting.
     */
    bool seen = false;
    for (u32 i = 0; i < 200000u && !atomic_load(&arg.waiter_got); i++) {
        if (unmanaged_threads() == 1u) {
            seen = true;
            break;
        }
        (void)cvm_alloc(&leaf_desc);
    }
    ASSERT(seen);
    ASSERT(!atomic_load(&arg.waiter_got));

    ASSERT(finish(&arg, holder, waiter));
    ASSERT(atomic_load(&arg.waiter_got));
    /* And once it is done, it is managed again. */
    ASSERT_EQ_FMT(0u, unmanaged_threads(), "%u");

    cvm_mutex_destroy(&m);
    fixture_leave();
    PASS();
}

TEST a_raw_wait_stays_visible_to_the_collector(void) {
    fixture_enter();
    CvmMutex m;
    ASSERT(cvm_mutex_init(&m));

    HoldArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.m = &m;
    arg.raw_waiter = true;

    RtThread holder, waiter;
    ASSERT(start_holder_and_waiter(&arg, &holder, &waiter));

    /*
     * The waiter is blocked too, and it has said nothing. That is the difference
     * the two forms make, and it is the reason the raw form is only for threads
     * the collector already knows about.
     */
    for (u32 i = 0; i < 20000u && !atomic_load(&arg.waiter_got); i++) {
        ASSERT_EQ_FMT(0u, unmanaged_threads(), "%u");
        (void)cvm_alloc(&leaf_desc);
    }
    ASSERT(!atomic_load(&arg.waiter_got));

    ASSERT(finish(&arg, holder, waiter));
    cvm_mutex_destroy(&m);
    fixture_leave();
    PASS();
}

TEST a_collection_completes_while_a_thread_waits_for_the_lock(void) {
    fixture_enter();
    CvmMutex m;
    ASSERT(cvm_mutex_init(&m));

    HoldArg arg;
    memset(&arg, 0, sizeof(arg));
    arg.m = &m;
    arg.raw_waiter = false;

    RtThread holder, waiter;
    ASSERT(start_holder_and_waiter(&arg, &holder, &waiter));

    /*
     * This is the whole point. The holder is managed and allocating, so it parks
     * when the collection asks it to; the waiter is blocked and has declared
     * itself unmanaged, so the collector does not wait for it. Without that
     * declaration the collector would wait for the waiter, the waiter would wait
     * for the holder, and the holder would be parked waiting for the collection.
     *
     * A regression here does not fail, it hangs.
     */
    for (u32 i = 0; i < 3u; i++) {
        collect();
    }
    ASSERT(atomic_load(&arg.allocations) > 0u);

    ASSERT(finish(&arg, holder, waiter));
    ASSERT(atomic_load(&arg.waiter_got));

    /* And the heap is still a heap. */
    collect();

    cvm_mutex_destroy(&m);
    fixture_leave();
    PASS();
}

SUITE(mutex_suite) {
    RUN_TEST(a_lock_excludes_and_a_trylock_does_not_wait);
    RUN_TEST(a_declared_wait_reads_as_unmanaged);
    RUN_TEST(a_raw_wait_stays_visible_to_the_collector);
    RUN_TEST(a_collection_completes_while_a_thread_waits_for_the_lock);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    cvm_threads_init();
    cvm_safepoint_init();
    if (!cvm_world_init(TEST_MIN, TEST_MAX)) {
        fprintf(stderr, "could not bring up the heap\n");
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(mutex_suite);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}

#else /* _WIN32 */

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
