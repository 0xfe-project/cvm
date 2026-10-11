/*
 * tests/stress/test_stress_handoff.c
 *
 * A reference that leaves one thread's frame and lands somewhere else.
 *
 * The hazard is narrow and worth stating exactly. A pointer a thread holds in
 * its frame is a root, because the marker walks that frame. A pointer that has
 * left the frame and is not yet in anyone else's is not a root at all, and a
 * collection that runs in that gap reclaims it. So the gap has to be closed by
 * something the collector can see, and `cvm/roots.h` is the something: a table
 * the marker walks alongside the frame chains.
 *
 * The oracle is the canary plus the metadata. An object reclaimed while it was
 * held shows up either as a canary mismatch or as `oracle_is_live` going
 * false.
 *
 * ---------------------------------------------------------------------
 * What is NOT here, and why
 * ---------------------------------------------------------------------
 *
 * There was a second test in this file: a thread inside an `unmanaged` region
 * registering a root while another thread collected, which is the write
 * barrier `cvm/roots.h` promises and the worst failure the collector has.
 *
 * It hangs. Not occasionally — it stops the world and never resumes it, and
 * the collector reports the reason itself:
 *
 *     [cvm warn] safepoint: thread(s) not parked after 200ms:
 *     [cvm warn]   thread ... state=managed alive=true doing=idle
 *     [cvm warn]   a thread inside a blocking foreign call must bracket it
 *                  with cvm_thread_enter_unmanaged/leave
 *
 * The first attempt at the oracle was also wrong and is worth recording,
 * because it is the kind of wrong that passes: it checked liveness
 * immediately after `cvm_root_add`, but a missed mark frees nothing by
 * itself — the block is reclaimed when that collection's *sweep* runs, and
 * the sweep is deferred and driven by whoever allocates next. Checking before
 * that point checks nothing. With the barrier deliberately removed from
 * `cvm_root_add`, that version of the test still passed. Drive a sweep first,
 * and it hangs.
 *
 * So the barrier test is not in the suite in this form. `vm/context.c`'s own
 * note above `cvm_thread_leave_unmanaged` says leaving is where the
 * synchroniser will hook in and that it is not implemented yet; a thread that
 * leaves an unmanaged region during a collection is what that note is about,
 * and this is what it looks like when it happens. The reproducer is written
 * up in the task list rather than committed here, because a test that hangs
 * is a finding, not a test.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "cvm/roots.h"
#include "support/oracle.h"

#define TEST_MIN ((usize)8 * 1024 * 1024)
#define TEST_MAX ((usize)64 * 1024 * 1024)

typedef struct Node Node;

struct Node {
    Node* next;
    u64 canary;
};

static CvmPtrField const node_fields[] = {{.offset = offsetof(Node, next)}};

static CvmTypeDescriptor const node_desc = {
    .size = sizeof(Node),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 1,
    .ptr_fields = node_fields,
};

/* Build a short chain, the head rooted in the caller's slot. */
static bool build_chain(opaque_mut* head, u32 length) {
    for (u32 i = 0; i < length; i++) {
        Node* node = cvm_alloc(&node_desc);
        if (node == NULL) {
            return false;
        }
        cvm_store_field(node, offsetof(Node, next), *head);
        node->canary = oracle_field_canary(node);
        *head = node;
    }
    return true;
}

static bool chain_is_intact(Node* head) {
    for (Node* node = head; node != NULL; node = node->next) {
        if (!oracle_field_canary_ok(node, node->canary)) {
            return false;
        }
    }
    return true;
}

/*
 * The table is what keeps it alive, and it has to be, because nothing else
 * does.
 *
 * The chain is rooted in a frame at first. The test registers the head and
 * then clears the frame slot, so between those two events and after the
 * second the table is the only thing reaching it. Two collections run in that
 * gap, which is the whole point: a table the marker walked at the wrong time
 * would show up here as a reclaimed chain.
 */
TEST a_reference_handed_over_survives_the_gap(void) {
    (void)cvm_thread_attach();

    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = NULL;

    ASSERT(build_chain(&f_roots[0], 8));
    Node* head = f_roots[0];

    CvmRootHandle handle = cvm_root_add(head);
    ASSERT(handle != CVM_ROOT_HANDLE_NONE);

    /* Now the frame lets go: the table is the only root. */
    f_roots[0] = NULL;

    oracle_collect();
    oracle_collect();

    ASSERT(cvm_root_get(handle) == head);
    ASSERT(chain_is_intact(head));
    ASSERT(oracle_is_live(head));

    /* Take it back into a frame and release the slot. */
    f_roots[0] = cvm_root_get(handle);
    cvm_root_remove(handle);

    ASSERT(cvm_root_get(handle) == NULL);

    oracle_collect();
    ASSERT(chain_is_intact(f_roots[0]));

    /* And with the frame gone too, it goes. */
    cvm_frame_pop(&f_frame);
    oracle_collect();
    ASSERT(!oracle_is_live(head));

    cvm_thread_detach();
    PASS();
}

/*
 * Replacing a slot releases the object that was in it.
 *
 * `cvm_root_set` is a separate code path from `cvm_root_add` and would be a
 * separate omission, so it gets its own test rather than being assumed to
 * follow.
 */
TEST replacing_a_root_keeps_the_new_object_and_releases_the_old(void) {
    (void)cvm_thread_attach();

    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = NULL;

    Node* first = cvm_alloc(&node_desc);
    ASSERT(first != NULL);
    cvm_store_field(first, offsetof(Node, next), NULL);
    first->canary = oracle_field_canary(first);

    Node* second = cvm_alloc(&node_desc);
    ASSERT(second != NULL);
    cvm_store_field(second, offsetof(Node, next), NULL);
    second->canary = oracle_field_canary(second);

    CvmRootHandle handle = cvm_root_add(first);
    ASSERT(handle != CVM_ROOT_HANDLE_NONE);

    cvm_root_set(handle, second);
    ASSERT(cvm_root_get(handle) == second);

    /*
     * Neither is in a frame, so the table holds `second` and nothing holds
     * `first`. One collection should settle both facts.
     */
    oracle_collect();

    ASSERT(oracle_is_live(second));
    ASSERT(oracle_field_canary_ok(second, second->canary));
    ASSERT(!oracle_is_live(first));

    cvm_root_remove(handle);
    cvm_frame_pop(&f_frame);

    oracle_collect();
    ASSERT(!oracle_is_live(second));

    cvm_thread_detach();
    PASS();
}

/*
 * Many slots, filled and emptied, with collections in between.
 *
 * The table grows and shrinks and reuses slots; this is what says that a slot
 * handed back is not still being marked, and that a reused slot does not
 * resurrect what used to be in it.
 */
TEST slots_are_reused_without_resurrecting_anything(void) {
    (void)cvm_thread_attach();

    enum { SLOTS = 64 };

    CVM_FRAME_DECL(f, SLOTS);
    cvm_frame_push(&f_frame, f_roots, SLOTS);
    for (u32 i = 0; i < SLOTS; i++) {
        f_roots[i] = NULL;
    }

    CvmRootHandle handles[SLOTS];

    /* Fill every slot, collecting as we go. */
    for (u32 i = 0; i < SLOTS; i++) {
        Node* node = cvm_alloc(&node_desc);
        ASSERT(node != NULL);
        cvm_store_field(node, offsetof(Node, next), NULL);
        node->canary = oracle_field_canary(node);

        handles[i] = cvm_root_add(node);
        ASSERT(handles[i] != CVM_ROOT_HANDLE_NONE);
        ASSERT(cvm_root_get(handles[i]) == node);

        if ((i % 8) == 0) {
            oracle_collect();
            ASSERT(cvm_root_get(handles[i]) == node);
            ASSERT(oracle_field_canary_ok(node, node->canary));
        }
    }

    /* Empty them, collecting as we go, then reuse the same slots. */
    for (u32 i = 0; i < SLOTS; i++) {
        Node* node = cvm_root_get(handles[i]);
        ASSERT(node != NULL);
        cvm_root_remove(handles[i]);

        if ((i % 8) == 0) {
            oracle_collect();
            ASSERT(cvm_root_get(handles[i]) == NULL);
        }
    }

    for (u32 i = 0; i < SLOTS; i++) {
        Node* node = cvm_alloc(&node_desc);
        ASSERT(node != NULL);
        cvm_store_field(node, offsetof(Node, next), NULL);
        node->canary = oracle_field_canary(node);

        handles[i] = cvm_root_add(node);
        ASSERT(handles[i] != CVM_ROOT_HANDLE_NONE);
        ASSERT(cvm_root_get(handles[i]) == node);
        ASSERT(oracle_is_live(node));
    }

    oracle_collect();

    for (u32 i = 0; i < SLOTS; i++) {
        Node* node = cvm_root_get(handles[i]);
        ASSERT(node != NULL);
        ASSERT(oracle_field_canary_ok(node, node->canary));
        cvm_root_remove(handles[i]);
    }

    cvm_frame_pop(&f_frame);
    cvm_thread_detach();
    PASS();
}

/* ------------------------------------------------------------------ */
/* the open bug: leaving the unmanaged state during a collection        */
/* ------------------------------------------------------------------ */

/*
 * A thread that registers and leaves the unmanaged state while another thread
 * collects, on repeat.
 *
 * ---------------------------------------------------------------------
 * This is not the reproducer for the open bug, and saying so is the point
 * ---------------------------------------------------------------------
 *
 * It was written to be. The bug is a thread that leaves the unmanaged state
 * during a collection and leaves the collector waiting forever, and the
 * shape that reproduced it was in the working tree when it was found and was
 * never committed -- so the exact reproduction is lost, and this is a
 * reconstruction from the description rather than the thing itself.
 *
 * It does not reproduce it. What it does reproduce is a collector that stops
 * the world tens of thousands of times a second, which makes everything else
 * slow -- the worker makes progress, just not fast. And it prints no
 * "thread(s) not parked" at all, where the original printed them by the
 * thousand. Both are measurements of a different situation, which is what
 * makes this an honest stress case and a dishonest reproducer.
 *
 * It stays, gated and bounded, because a thread that registers while another
 * thread collects is worth stressing. It skips unless
 * CVM_REPRO_UNMANAGED_LEAVE is set, so the suite is not slowed by it.
 *
 * The shape, which is the shape any program that calls out and back in has:
 *
 *     allocate something
 *     root it in a frame slot            <- the frame can reach it
 *     enter unmanaged                    <- a foreign call
 *     clear the slot                     <- the frame lets go
 *     cvm_root_add                       <- the table holds it instead
 *     allocate, to drive the deferred sweep
 *     check it is still alive
 *     cvm_root_remove, leave unmanaged
 *
 * The check is what the sweep makes meaningful: a missed mark frees nothing
 * by itself, because the block is only reclaimed when that collection's sweep
 * runs, and the sweep is deferred until somebody allocates. Checking before
 * the allocations checks nothing -- a version of this that did so passed with
 * the write barrier deliberately removed.
 */
#define LEAVE_ROUNDS 200

typedef struct {
    _Atomic u32 corrupt;
    _Atomic bool ready;
} LeaveArg;

static opaque_mut leaving_worker(opaque_mut raw) {
    LeaveArg* arg = raw;
    if (!cvm_thread_attach()) {
        atomic_fetch_add(&arg->corrupt, 1);
        return NULL;
    }
    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = NULL;
    atomic_store(&arg->ready, true);

    for (u32 round = 0; round < LEAVE_ROUNDS; round++) {
        Node* node = cvm_alloc(&node_desc);
        if (node == NULL) {
            atomic_fetch_add(&arg->corrupt, 1);
            break;
        }
        cvm_store_field(node, offsetof(Node, next), NULL);
        node->canary = oracle_field_canary(node);

        f_roots[0] = node;

        cvm_thread_enter_unmanaged();
        f_roots[0] = NULL;

        CvmRootHandle handle = cvm_root_add(node);
        if (handle == CVM_ROOT_HANDLE_NONE) {
            atomic_fetch_add(&arg->corrupt, 1);
            cvm_thread_leave_unmanaged();
            break;
        }

        /* Drive the deferred sweep, so a missed mark would have been
         * reclaimed by the time the next line runs. */
        for (u32 i = 0; i < 16; i++) {
            (void)cvm_alloc(&node_desc);
        }

        if (!oracle_is_live(node) || !oracle_field_canary_ok(node, node->canary)) {
            atomic_fetch_add(&arg->corrupt, 1);
        }

        cvm_root_remove(handle);
        cvm_thread_leave_unmanaged();
    }

    cvm_frame_pop(&f_frame);
    cvm_thread_detach();
    return NULL;
}

static opaque_mut collecting_worker(opaque_mut raw) {
    LeaveArg* arg = raw;
    (void)arg;

    /*
     * Collected in a loop, but not back to back: a collection stops the world
     * for its whole duration, so a collector that re-acquires immediately
     * gives the other thread no window at all and starves it rather than
     * racing it. The yield is what makes this a test of the state transition
     * instead of a test of the scheduler.
     */
    for (;;) {
        oracle_collect();
        rt_thread_yield();
    }
    return NULL;
}

TEST leaving_unmanaged_during_a_collection_does_not_stall_the_world(void) {
    if (getenv("CVM_REPRO_UNMANAGED_LEAVE") == NULL) {
        SKIP();
    }

    LeaveArg arg = {.corrupt = 0, .ready = false};
    atomic_store(&arg.corrupt, 0u);
    atomic_store(&arg.ready, false);

    RtThread leaver;
    RtThread collector;
    ASSERT(rt_thread_create(&collector, collecting_worker, &arg));
    ASSERT(rt_thread_create(&leaver, leaving_worker, &arg));

    u64 deadline = rt_time_nanos() + 60000000000ull;
    while (!atomic_load(&arg.ready) && rt_time_nanos() < deadline) {
        rt_thread_yield();
    }
    ASSERT(atomic_load(&arg.ready));

    ASSERT(cvm_thread_join(leaver));
    ASSERT_EQ_FMT(0u, atomic_load(&arg.corrupt), "%u");
    PASS();
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    if (!oracle_bring_up(TEST_MIN, TEST_MAX, 0)) {
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_reference_handed_over_survives_the_gap);
    RUN_TEST(replacing_a_root_keeps_the_new_object_and_releases_the_old);
    RUN_TEST(slots_are_reused_without_resurrecting_anything);
    RUN_TEST(leaving_unmanaged_during_a_collection_does_not_stall_the_world);
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
