/*
 * vm/mutex.c
 *
 * See include/cvm/mutex.h for what the two forms are for and for the criterion
 * that decides between them.
 */

#include "cvm/mutex.h"

#include "base/assert.h"
#include "base/types.h"
#include "context.h"
#include "rt.h"

bool cvm_mutex_init(CvmMutex* m) {
    return rt_mutex_init(&m->raw);
}

void cvm_mutex_destroy(CvmMutex* m) {
    rt_mutex_destroy(&m->raw);
}

/*
 * Whether this thread is doing the collector's work.
 *
 * A thread that is collecting, marking, sweeping or finalizing must not declare
 * itself outside the managed world: the collector's whole job is to know where
 * those threads are, and telling it not to wait for one is telling it to walk a
 * heap that is still being changed.
 *
 * Only in a debug build, because that is when there is an assertion to answer: a
 * release build has no use for the question, and an unused static function is an
 * error here.
 */
#if defined(CVM_DEBUG)
static bool caller_is_collector(CvmThreadNode* node) {
    if (node == NULL) {
        return false;
    }
    switch ((CvmThreadActivity)atomic_load_explicit(&node->activity,
                                                    memory_order_relaxed)) {
    case CVM_ACT_COLLECTING:
    case CVM_ACT_MARKING:
    case CVM_ACT_SWEEPING:
    case CVM_ACT_COALESCING:
    case CVM_ACT_NULLIFYING:
    case CVM_ACT_FINALIZING:
        return true;
    default:
        return false;
    }
}

#endif /* CVM_DEBUG */

void cvm_mutex_lock(CvmMutex* m) {
#if defined(CVM_DEBUG)
    cvm_assert(!caller_is_collector(cvm_thread_node()));
#endif

    cvm_thread_enter_unmanaged();
    rt_mutex_lock(&m->raw);
    cvm_thread_leave_unmanaged();
}

bool cvm_mutex_trylock(CvmMutex* m) {
    /* Nothing is waited for, so nothing has to be declared. */
    return rt_mutex_trylock(&m->raw);
}

void cvm_mutex_unlock(CvmMutex* m) {
    rt_mutex_unlock(&m->raw);
}

void cvm_mutex_lock_raw(CvmMutex* m) {
    rt_mutex_lock(&m->raw);
}
