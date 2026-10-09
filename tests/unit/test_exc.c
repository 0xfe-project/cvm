/*
 * tests/unit/test_exc.c
 *
 * Throwing, catching, and what an unwind has to put back.
 *
 * The mechanism is tests/unit/test_trap.c's neighbour in one respect and its
 * opposite in another: it is also a non-local transfer of control, but the
 * state it has to restore is cvm's, not the platform's. Two pieces of that
 * state are per-thread and both are integers now, which is what makes them
 * restorable at all:
 *
 *   - the frame depth (vm/include/cvm/gc.h), so an unwind drops every frame
 *     the try body pushed and the frames of the throwing activations with
 *     them;
 *   - the unmanaged depth (vm/include/cvm/gc.h), so a try body that entered
 *     a foreign call and never came back does not leave the thread looking
 *     like it is inside one.
 *
 * And the third thing an unwind must not lose is the exception itself. It
 * lives in the handler record, which is on a chain the marker walks, so the
 * value is reachable from the moment cvm_throw stores it until the catch
 * reads it. The tests below check that from the collector's side rather than
 * by reading the field back.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <stddef.h>
#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "cvm/try.h"
#include "cvm/exc.h"
#include "gc/alloc.h"
#include "gc/heap.h"
#include "gc/marker.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)16 * 1024 * 1024)

static CvmHeap* heap(void) {
    return &cvm_world()->heap;
}

/* A leaf with a canary, so a reclaimed-and-reused object is visible. */
typedef struct Leaf {
    u64 canary;
    u64 padding;
} Leaf;

static CvmTypeDescriptor const leaf_desc = {
    .size = sizeof(Leaf),
    .kind = CVM_TYPE_SCALAR,
};

static u64 canary_of(Leaf const* leaf) {
    return (u64)(usize)leaf ^ 0x5a5a5a5a5a5a5a5aull;
}

static void stamp(Leaf* leaf) {
    leaf->canary = canary_of(leaf);
}

static void fixture_enter(void) {
    (void)cvm_thread_attach();
}

static void fixture_leave(void) {
    cvm_thread_detach();
}

/* Mark, then run the marker to completion, without the packet bookkeeping a
 * full collection does. What matters here is which objects are reachable. */
static void mark_now(void) {
    cvm_mark_roots(heap());
    cvm_mark_until_done(heap());
}

/* ------------------------------------------------------------------ */
/* catching                                                            */
/* ------------------------------------------------------------------ */

TEST a_throw_reaches_the_innermost_handler(void) {
    fixture_enter();

    void* outer_value = cvm_alloc(&leaf_desc);
    void* inner_value = cvm_alloc(&leaf_desc);
    ASSERT(outer_value != NULL);
    ASSERT(inner_value != NULL);

    bool outer_ran = false;
    bool inner_ran = false;

    CVM_TRY() {
        CVM_TRY() {
            cvm_throw(inner_value);
        }
        CVM_CATCH(caught) {
            inner_ran = true;
            /* The value that was thrown, not the one the outer try holds. */
            ASSERT(caught == inner_value);
        }
        CVM_ENDTRY();

        cvm_throw(outer_value);
    }
    CVM_CATCH(caught) {
        outer_ran = true;
        ASSERT(caught == outer_value);
    }
    CVM_ENDTRY();

    ASSERT(inner_ran);
    ASSERT(outer_ran);
    ASSERT(cvm_handler_top() == NULL);

    fixture_leave();
    PASS();
}

/*
 * A catch that returns, breaks or falls through leaves nothing behind.
 *
 * The chain is the root set, so a handler that is still linked after its try
 * has finished would keep its exception alive for as long as the thread
 * lives -- a leak with no symptom except a heap that never drops.
 */
TEST the_chain_is_empty_after_nested_tries(void) {
    fixture_enter();

    for (u32 i = 0; i < 4; i++) {
        CVM_TRY() {
            CVM_TRY() {
                cvm_throw(NULL);
            }
            CVM_CATCH(caught) {
                ASSERT(caught == NULL);
            }
            CVM_ENDTRY();
        }
        CVM_CATCH(caught) {
            ASSERT(caught == NULL);
        }
        CVM_ENDTRY();
    }

    ASSERT(cvm_handler_top() == NULL);
    ASSERT(cvm_thread_ctx()->handlers == NULL);

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* the exception is a root                                             */
/* ------------------------------------------------------------------ */

/*
 * The value is reachable while the catch runs, and it is reachable through
 * the handler chain rather than through a local in the catching function.
 *
 * That distinction is the whole test. A local holding the thrown value would
 * keep it alive too -- but only because this function happens to still be
 * running; what the design promises is that the *runtime* holds it, so a
 * collection during the catch finds it without help from the site. A local
 * would also be indeterminate across the jump, which is why the value is
 * read from the handler rather than handed over in a variable.
 */
TEST a_caught_value_survives_a_collection_during_the_catch(void) {
    fixture_enter();

    Leaf* thrown = cvm_alloc(&leaf_desc);
    ASSERT(thrown != NULL);
    stamp(thrown);
    u64 stamped = thrown->canary;

    bool checked = false;

    CVM_TRY() {
        /*
         * The only reference to the object is the argument to cvm_throw.
         * The frame this would otherwise have been rooted in is gone by the
         * time the catch runs: cvm_throw restores the frame depth, and this
         * function pushed no frame at all.
         */
        cvm_throw(thrown);
    }
    CVM_CATCH(caught) {
        ASSERT(caught == thrown);

        /* Reachable without the site doing anything. */
        mark_now();
        ASSERT(cvm_object_is_marked(heap(), thrown));

        /* And intact, which is what a canary is for. */
        ASSERT_EQ_FMT(stamped, thrown->canary, "%llu");
        checked = true;
    }
    CVM_ENDTRY();

    ASSERT(checked);
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* what only the runtime knows                                         */
/* ------------------------------------------------------------------ */

/*
 * The frame depth an unwind restores.
 *
 * This is the half a landing pad cannot do for itself. The try body pushes
 * frames -- one per call it makes -- and the throw abandons them without a
 * pop running; the depth has to come back to what it was at try entry or the
 * frames of functions that have returned stay in the root set, holding their
 * slots' contents and reaching into storage that has been reused.
 *
 * Measured rather than asserted about: the depth inside the catch has to
 * equal the depth before the try, and the frames that the body pushed have
 * to be gone.
 */
TEST an_unwind_restores_the_frame_depth(void) {
    fixture_enter();

    CVM_FRAME_DECL(root, 1);
    cvm_frame_push(&root_frame, root_roots, 1);
    root_roots[0] = NULL;

    CvmVMContext* ctx = cvm_ctx();
    u32 outer_depth = ctx->frame_depth;

    CVM_TRY() {
        CVM_FRAME_DECL(body, 2);
        cvm_frame_push(&body_frame, body_roots, 2);
        body_roots[0] = NULL;
        body_roots[1] = NULL;

        ASSERT(ctx->frame_depth == outer_depth + 1u);
        cvm_throw(NULL);
    }
    CVM_CATCH(caught) {
        (void)caught;
        ASSERT_EQ_FMT(outer_depth, ctx->frame_depth, "%u");
    }
    CVM_ENDTRY();

    ASSERT_EQ_FMT(outer_depth, ctx->frame_depth, "%u");

    cvm_frame_pop(&root_frame);
    ASSERT_EQ_FMT(outer_depth - 1u, ctx->frame_depth, "%u");

    fixture_leave();
    PASS();
}

/*
 * The unmanaged depth an unwind restores.
 *
 * A try body that enters a foreign call and throws from inside it never runs
 * the leave. Without the restore the thread stays unmanaged -- and a
 * collection would skip a thread that is running managed code again, which
 * is the failure running the other way from the one unmanaged state exists
 * to prevent.
 *
 * Both directions are checked: a body that throws while unmanaged has to
 * come back managed, and one that throws while managed has to stay managed.
 */
TEST an_unwind_restores_the_unmanaged_depth(void) {
    fixture_enter();

    ASSERT_EQ_FMT(0u, cvm_thread_unmanaged_depth(), "%u");

    CVM_TRY() {
        cvm_thread_enter_unmanaged();
        cvm_thread_enter_unmanaged();
        ASSERT_EQ_FMT(2u, cvm_thread_unmanaged_depth(), "%u");
        cvm_throw(NULL);
    }
    CVM_CATCH(caught) {
        (void)caught;
        ASSERT_EQ_FMT(0u, cvm_thread_unmanaged_depth(), "%u");
        ASSERT_EQ_FMT((int)CVM_THREAD_MANAGED, (int)cvm_thread_get_state(), "%d");
    }
    CVM_ENDTRY();

    /* And a try entered while unmanaged comes back to that depth. */
    cvm_thread_enter_unmanaged();

    CVM_TRY() {
        cvm_thread_enter_unmanaged();
        ASSERT_EQ_FMT(2u, cvm_thread_unmanaged_depth(), "%u");
        cvm_throw(NULL);
    }
    CVM_CATCH(caught) {
        (void)caught;
        ASSERT_EQ_FMT(1u, cvm_thread_unmanaged_depth(), "%u");
        ASSERT_EQ_FMT((int)CVM_THREAD_UNMANAGED, (int)cvm_thread_get_state(), "%d");
    }
    CVM_ENDTRY();

    cvm_thread_leave_unmanaged();
    ASSERT_EQ_FMT(0u, cvm_thread_unmanaged_depth(), "%u");
    ASSERT_EQ_FMT((int)CVM_THREAD_MANAGED, (int)cvm_thread_get_state(), "%d");

    fixture_leave();
    PASS();
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
    RUN_TEST(a_throw_reaches_the_innermost_handler);
    RUN_TEST(the_chain_is_empty_after_nested_tries);
    RUN_TEST(a_caught_value_survives_a_collection_during_the_catch);
    RUN_TEST(an_unwind_restores_the_frame_depth);
    RUN_TEST(an_unwind_restores_the_unmanaged_depth);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}

#endif /* !_WIN32 */
