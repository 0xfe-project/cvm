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
        node->next = *head;
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
    first->next = NULL;
    first->canary = oracle_field_canary(first);

    Node* second = cvm_alloc(&node_desc);
    ASSERT(second != NULL);
    second->next = NULL;
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
        node->next = NULL;
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
        node->next = NULL;
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

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    if (!oracle_bring_up(TEST_MIN, TEST_MAX, 0)) {
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_TEST(a_reference_handed_over_survives_the_gap);
    RUN_TEST(replacing_a_root_keeps_the_new_object_and_releases_the_old);
    RUN_TEST(slots_are_reused_without_resurrecting_anything);
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
