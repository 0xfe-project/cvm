/*
 * tests/unit/test_context.c
 *
 * The thread registry.
 *
 * What this layer decides, and therefore what is worth testing: that
 * attach installs a usable context and is idempotent, that detach removes
 * the entry and clears TLS rather than leaving a dangling context behind,
 * that the two directions of the context/node link agree, and that
 * concurrent registration does not corrupt the list.
 *
 * The last one is what ThreadSanitizer is here for; the tests are written
 * so that the registry lock is load-bearing.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <stdatomic.h>
#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

/* The registry is process-wide, so every test that registers must
 * unregister before it returns. This is the baseline other tests measure
 * against. */
static u32 baseline_count(void) {
    return cvm_thread_count();
}

/* ------------------------------------------------------------------ */
/* attach / detach                                                     */
/* ------------------------------------------------------------------ */

TEST attach_installs_a_usable_context(void) {
    u32 before = baseline_count();

    ASSERT(cvm_thread_attach());

    CvmVMContext* ctx = cvm_thread_ctx();
    ASSERT(ctx != NULL);
    ASSERT(cvm_ctx() == ctx); /* the TLS the generated code uses */
    ASSERT_EQ_FMT(before + 1, cvm_thread_count(), "%u");

    /* a fresh context has no frames and no bump region */
    ASSERT(ctx->frame_depth == 0u);
    ASSERT(ctx->bump_ptr == NULL);
    ASSERT(ctx->bump_end == NULL);

    cvm_thread_detach();
    ASSERT_EQ_FMT(before, cvm_thread_count(), "%u");
    PASS();
}

TEST attach_is_idempotent(void) {
    ASSERT(cvm_thread_attach());

    CvmVMContext* first = cvm_thread_ctx();
    u32 count = cvm_thread_count();

    /* a second attach must not allocate a second node and orphan the
     * first, which would leave the original context unreachable while
     * still registered */
    ASSERT(cvm_thread_attach());
    ASSERT(cvm_thread_ctx() == first);
    ASSERT_EQ_FMT(count, cvm_thread_count(), "%u");

    cvm_thread_detach();
    PASS();
}

TEST detach_clears_tls(void) {
    ASSERT(cvm_thread_attach());
    ASSERT(cvm_thread_ctx() != NULL);

    cvm_thread_detach();

    /* a dangling context here would make the thread look registered to
     * everything that reads TLS */
    ASSERT(cvm_thread_ctx() == NULL);
    ASSERT(cvm_ctx() == NULL);
    ASSERT(cvm_thread_node() == NULL);
    PASS();
}

TEST detach_without_attach_is_harmless(void) {
    u32 before = baseline_count();
    cvm_thread_detach();
    ASSERT_EQ_FMT(before, cvm_thread_count(), "%u");
    PASS();
}

TEST context_and_node_point_at_each_other(void) {
    ASSERT(cvm_thread_attach());

    CvmThreadNode* node = cvm_thread_node();
    CvmVMContext* ctx = cvm_thread_ctx();

    ASSERT(node != NULL);
    ASSERT(ctx != NULL);

    /* the node owns the context */
    ASSERT(&node->ctx == ctx);
    /* and the context points back at its container, which is the node */
    ASSERT(node->ctx.owner == (opaque_mut)node);
    ASSERT(ctx->owner == (opaque_mut)node);
    ASSERT((CvmThreadNode*)ctx->owner == node);

    cvm_thread_detach();
    PASS();
}

TEST registered_thread_is_visible_in_the_registry(void) {
    ASSERT(cvm_thread_attach());
    CvmThreadNode* self = cvm_thread_node();

    bool found = false;
    cvm_threads_lock_read();
    CVM_THREADS_FOREACH(node) {
        if (node == self) {
            found = true;
        }
    }
    cvm_threads_unlock_read();

    ASSERT(found);

    cvm_thread_detach();
    PASS();
}

/* ------------------------------------------------------------------ */
/* state                                                               */
/* ------------------------------------------------------------------ */

TEST state_defaults_to_managed(void) {
    ASSERT(cvm_thread_attach());
    ASSERT_EQ_FMT((int)CVM_THREAD_MANAGED, (int)cvm_thread_get_state(), "%d");
    cvm_thread_detach();
    PASS();
}

TEST entering_and_leaving_unmanaged_flips_the_state(void) {
    ASSERT(cvm_thread_attach());

    cvm_thread_enter_unmanaged();
    ASSERT_EQ_FMT((int)CVM_THREAD_UNMANAGED, (int)cvm_thread_get_state(), "%d");

    cvm_thread_leave_unmanaged();
    ASSERT_EQ_FMT((int)CVM_THREAD_MANAGED, (int)cvm_thread_get_state(), "%d");

    cvm_thread_detach();
    PASS();
}

/*
 * Nesting is the ordinary case, not an exotic one: a foreign call that
 * calls back into managed code, which makes a foreign call of its own.
 *
 * The state a collector reads is `unmanaged` exactly while the depth is
 * non-zero, so the first leave must not announce the thread as managed.
 * The version that used a flag did exactly that -- `enter(); enter();
 * leave();` left the thread managed while it was still inside the outer
 * call, and that is the state a collection is entitled to stop. Stopping a
 * thread inside foreign code is the stall this state exists to avoid.
 */
TEST unmanaged_state_nests(void) {
    ASSERT(cvm_thread_attach());

    cvm_thread_enter_unmanaged();
    cvm_thread_enter_unmanaged();
    ASSERT_EQ_FMT((int)CVM_THREAD_UNMANAGED, (int)cvm_thread_get_state(), "%d");

    cvm_thread_leave_unmanaged();
    ASSERT_EQ_FMT((int)CVM_THREAD_UNMANAGED, (int)cvm_thread_get_state(), "%d");

    cvm_thread_leave_unmanaged();
    ASSERT_EQ_FMT((int)CVM_THREAD_MANAGED, (int)cvm_thread_get_state(), "%d");

    cvm_thread_detach();
    PASS();
}

/*
 * A leave with no matching enter restores the invariant instead of driving
 * the depth below zero.
 *
 * The depth is what the next enter compares against, so a counter that went
 * negative would make the *first* enter of the next foreign call look like
 * a nested one -- and the thread would then never announce itself managed
 * again, which is a collection waiting for a thread that has come back.
 */
TEST a_leave_without_an_enter_leaves_the_thread_managed(void) {
    ASSERT(cvm_thread_attach());

    cvm_thread_leave_unmanaged();
    ASSERT_EQ_FMT((int)CVM_THREAD_MANAGED, (int)cvm_thread_get_state(), "%d");

    /* The next enter is the first, not a nested one. */
    cvm_thread_enter_unmanaged();
    ASSERT_EQ_FMT((int)CVM_THREAD_UNMANAGED, (int)cvm_thread_get_state(), "%d");
    cvm_thread_leave_unmanaged();
    ASSERT_EQ_FMT((int)CVM_THREAD_MANAGED, (int)cvm_thread_get_state(), "%d");

    cvm_thread_detach();
    PASS();
}

TEST state_of_an_unregistered_thread_reads_as_managed(void) {
    /* Nothing registered, so there is nothing to be unmanaged relative to.
     * Reporting unmanaged would make a collection skip a thread that is
     * actually running. */
    ASSERT(cvm_thread_ctx() == NULL);
    ASSERT_EQ_FMT((int)CVM_THREAD_MANAGED, (int)cvm_thread_get_state(), "%d");

    /* and setting it must not crash */
    cvm_thread_set_state(CVM_THREAD_UNMANAGED);
    ASSERT_EQ_FMT((int)CVM_THREAD_MANAGED, (int)cvm_thread_get_state(), "%d");
    PASS();
}

TEST a_running_thread_is_alive(void) {
    ASSERT(cvm_thread_attach());
    ASSERT(cvm_thread_node_is_alive(cvm_thread_node()));
    cvm_thread_detach();
    PASS();
}

/* ------------------------------------------------------------------ */
/* many threads                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    _Atomic u32* registered_seen;
    _Atomic bool* attach_failed;
    int iterations;
} RegisterArg;

static opaque_mut registers_and_leaves(opaque_mut arg) {
    RegisterArg* a = arg;
    for (int i = 0; i < a->iterations; i++) {
        if (!cvm_thread_attach()) {
            atomic_store(a->attach_failed, true);
            return NULL;
        }
        /* the thread's own node must be findable while it is registered */
        if (cvm_thread_node() != NULL) {
            atomic_fetch_add(a->registered_seen, 1u);
        }
        cvm_thread_detach();
    }
    return NULL;
}

TEST many_threads_register_and_unregister(void) {
    enum { THREADS = 8, ITERATIONS = 200 };

    u32 before = baseline_count();

    _Atomic u32 registered_seen = 0;
    _Atomic bool attach_failed = false;
    RegisterArg a = {.registered_seen = &registered_seen,
                     .attach_failed = &attach_failed,
                     .iterations = ITERATIONS};

    RtThread threads[THREADS];
    for (int i = 0; i < THREADS; i++) {
        ASSERT(rt_thread_create(&threads[i], registers_and_leaves, &a));
    }
    for (int i = 0; i < THREADS; i++) {
        ASSERT(rt_thread_join(threads[i]));
    }

    ASSERT_FALSE(atomic_load(&attach_failed));
    ASSERT_EQ_FMT((u32)(THREADS * ITERATIONS), atomic_load(&registered_seen), "%u");
    /* every thread unregistered on the way out */
    ASSERT_EQ_FMT(before, baseline_count(), "%u");
    PASS();
}

typedef struct {
    _Atomic u32* concurrent_count;
    _Atomic u32* max_concurrent;
    int iterations;
} ConcurrentArg;

static void note_max_u32(_Atomic u32* max, u32 value) {
    u32 prev = atomic_load(max);
    while (value > prev && !atomic_compare_exchange_weak(max, &prev, value)) {
        /* prev refreshed; retry */
    }
}

static opaque_mut holds_registration_while_others_register(opaque_mut arg) {
    ConcurrentArg* a = arg;
    for (int i = 0; i < a->iterations; i++) {
        if (!cvm_thread_attach()) {
            return NULL;
        }

        u32 n = atomic_fetch_add(a->concurrent_count, 1u) + 1;
        note_max_u32(a->max_concurrent, n);

        rt_thread_yield();

        atomic_fetch_sub(a->concurrent_count, 1u);
        cvm_thread_detach();
    }
    return NULL;
}

TEST registry_survives_concurrent_registration(void) {
    /* Several threads register, are visible together, and leave. If the
     * registry lock were missing or wrong, this is where it would show up
     * under ThreadSanitizer. */
    enum { THREADS = 6, ITERATIONS = 100 };

    u32 before = baseline_count();

    _Atomic u32 concurrent_count = 0;
    _Atomic u32 max_concurrent = 0;
    ConcurrentArg a = {.concurrent_count = &concurrent_count,
                       .max_concurrent = &max_concurrent,
                       .iterations = ITERATIONS};

    RtThread threads[THREADS];
    for (int i = 0; i < THREADS; i++) {
        ASSERT(rt_thread_create(&threads[i], holds_registration_while_others_register,
                                &a));
    }
    for (int i = 0; i < THREADS; i++) {
        ASSERT(rt_thread_join(threads[i]));
    }

    /* more than one thread was registered at the same time */
    ASSERT(atomic_load(&max_concurrent) >= 2);
    ASSERT_EQ_FMT(0u, atomic_load(&concurrent_count), "%u");
    ASSERT_EQ_FMT(before, baseline_count(), "%u");
    PASS();
}

/*
 * The role is what the stuck-thread report names a thread by, and the report is
 * the only thing that can say which thread a collection was waiting for. So the
 * properties worth pinning are the two the report relies on: every attached
 * thread has a role, and the runtime's own thread can change its own.
 */
TEST a_thread_attaches_as_a_mutator(void) {
    ASSERT(cvm_thread_attach());

    CvmThreadNode* self = cvm_thread_node();
    ASSERT(self != NULL);
    ASSERT_EQ_FMT((u32)CVM_THREAD_ROLE_MUTATOR, self->role, "%u");

    str name = cvm_thread_role_name(self->role);
    ASSERT_EQ_FMT((usize)7, name.len, "%zu");
    ASSERT(memcmp(name.ptr, "mutator", 7) == 0);

    cvm_thread_detach();
    PASS();
}

TEST a_thread_can_declare_that_it_is_not_a_mutator(void) {
    ASSERT(cvm_thread_attach());

    cvm_thread_set_role(CVM_THREAD_ROLE_FINALIZER);
    CvmThreadNode* self = cvm_thread_node();
    ASSERT_EQ_FMT((u32)CVM_THREAD_ROLE_FINALIZER, self->role, "%u");

    str name = cvm_thread_role_name(self->role);
    ASSERT_EQ_FMT((usize)9, name.len, "%zu");
    ASSERT(memcmp(name.ptr, "finalizer", 9) == 0);

    /* A role from outside the enum still has to produce something printable:
     * the report must never be the thing that crashes. */
    str unknown = cvm_thread_role_name((u32)CVM_THREAD_ROLE_COUNT);
    ASSERT_EQ_FMT((usize)1, unknown.len, "%zu");

    cvm_thread_detach();
    PASS();
}

TEST role_changes_of_an_unregistered_thread_do_nothing(void) {
    /* Nothing to attach to, so nothing happens — and nothing crashes, which
     * matters because this runs on the teardown paths. */
    cvm_thread_set_role(CVM_THREAD_ROLE_FINALIZER);
    PASS();
}

SUITE(context_suite) {
    RUN_TEST(attach_installs_a_usable_context);
    RUN_TEST(attach_is_idempotent);
    RUN_TEST(detach_clears_tls);
    RUN_TEST(detach_without_attach_is_harmless);
    RUN_TEST(context_and_node_point_at_each_other);
    RUN_TEST(registered_thread_is_visible_in_the_registry);
    RUN_TEST(state_defaults_to_managed);
    RUN_TEST(entering_and_leaving_unmanaged_flips_the_state);
    RUN_TEST(unmanaged_state_nests);
    RUN_TEST(a_leave_without_an_enter_leaves_the_thread_managed);
    RUN_TEST(state_of_an_unregistered_thread_reads_as_managed);
    RUN_TEST(a_running_thread_is_alive);
    RUN_TEST(many_threads_register_and_unregister);
    RUN_TEST(registry_survives_concurrent_registration);
    RUN_TEST(a_thread_attaches_as_a_mutator);
    RUN_TEST(a_thread_can_declare_that_it_is_not_a_mutator);
    RUN_TEST(role_changes_of_an_unregistered_thread_do_nothing);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    cvm_threads_init();
    cvm_safepoint_init();

    /* Registration requires a heap: a registered thread may allocate. */
    if (!cvm_world_init(4 * 1024 * 1024, 16 * 1024 * 1024)) {
        fprintf(stderr, "could not bring up the heap\n");
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(context_suite);
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
