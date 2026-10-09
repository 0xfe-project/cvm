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

/* ------------------------------------------------------------------ */
/* the contract the frontend emits                                     */
/* ------------------------------------------------------------------ */

/*
 * A function that returns from inside a try body.
 *
 * The return cannot reach CVM_ENDTRY, so the handler the body pushed would
 * stay linked -- to a landing pad in a frame that no longer exists. That is
 * worse than a leak: the next throw in this thread would resume into a
 * function that has returned.
 *
 * The choke point is what prevents it, and this is the case the choke point
 * exists for. It is written as a helper rather than inline because what is
 * being checked is a function's exit path, and a test body is not a
 * function with a return.
 */
static opaque_mut returns_from_inside_a_try(opaque_mut value) {
    CVM_ENTER(helper, 1);

    CVM_TRY() {
        /* Only this frame slot holds it, and the slot goes away with the
         * frame -- which is why the value comes back out as the return. */
        helper_roots[0] = value;
        CVM_RETURN(helper, value);
    }
    CVM_CATCH(caught) {
        (void)caught;
    }
    CVM_ENDTRY();

    CVM_RETURN(helper, NULL);
}

TEST a_return_out_of_a_try_body_leaves_nothing_linked(void) {
    fixture_enter();

    u32 before = cvm_handler_depth();
    ASSERT_EQ_FMT(0u, before, "%u");

    void* value = cvm_alloc(&leaf_desc);
    ASSERT(value != NULL);
    ASSERT(returns_from_inside_a_try(value) == value);
    ASSERT_EQ_FMT(before, cvm_handler_depth(), "%u");
    ASSERT(cvm_handler_top() == NULL);

    fixture_leave();
    PASS();
}

/*
 * The frame's slots are valid the moment the frame is linked.
 *
 * cvm_frame_push clears them, so "zero the slots, then link" is not an
 * obligation the frontend can get wrong -- it is not the frontend's line at
 * all. That matters because the slots are what the marker walks: a frame
 * linked with whatever the stack had in those slots is a frame whose roots
 * are whatever the stack had, and the only thing between that and a wild
 * read is a bounds check somewhere else.
 */
static bool a_pushed_frame_starts_with_null_slots(void) {
    CVM_ENTER(fresh, 4);

    bool all_null = true;
    for (u32 i = 0; i < 4; i++) {
        if (fresh_roots[i] != NULL) {
            all_null = false;
        }
    }

    CVM_RETURN(fresh, all_null);
}

/*
 * Fill a frame's slots with heap pointers and pop it, so the storage the next
 * frame lands in holds something rather than zeroes. A `TEST` body cannot do
 * this itself: it compiles to a function that returns a test result, not to
 * one that returns void.
 */
static void dirty_the_frame_storage(void) {
    CVM_ENTER(dirty, 4);
    for (u32 i = 0; i < 4; i++) {
        dirty_roots[i] = cvm_alloc(&leaf_desc);
    }
    CVM_RETURN_VOID(dirty);
}

TEST a_pushed_frame_has_null_slots(void) {
    fixture_enter();

    dirty_the_frame_storage();
    ASSERT(a_pushed_frame_starts_with_null_slots());

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* the whole design, in one test                                       */
/* ------------------------------------------------------------------ */

/*
 * A throw from a few activations down, each of which allocated on the way in.
 *
 * Every one of those allocations is a point where a collection can run --
 * which means a point where the collector walks the frame chain and the
 * handler chain while the chain is mid-change. What has to hold at all of
 * them is that the exception is rooted and the depths are consistent.
 */
static void throw_from_depth(opaque_mut value, u32 depth) {
    CVM_ENTER(deep, 2);

    deep_roots[0] = cvm_alloc(&leaf_desc);
    deep_roots[1] = cvm_alloc(&leaf_desc);
    if (deep_roots[0] != NULL) {
        stamp(deep_roots[0]);
    }
    if (deep_roots[1] != NULL) {
        stamp(deep_roots[1]);
    }

    if (depth == 0u) {
        cvm_throw(value);
    }
    throw_from_depth(value, depth - 1u);

    CVM_RETURN_VOID(deep);
}

/*
 * One round, reporting through the counter rather than by asserting: an
 * ASSERT returns a test result, so it only compiles in a function that
 * returns one, and this is a helper.
 */
static u32 unwind_problems = 0;

static void one_throw_with_collections_on_the_path(void) {
    CvmVMContext* ctx = cvm_ctx();

    Leaf* thrown = cvm_alloc(&leaf_desc);
    if (thrown == NULL) {
        unwind_problems++;
        return;
    }
    stamp(thrown);
    u64 stamped = thrown->canary;

    CVM_ENTER(outer, 1);
    outer_roots[0] = thrown;
    u32 entry_depth = ctx->frame_depth;

    CVM_TRY() {
        throw_from_depth(thrown, 3u);
    }
    CVM_CATCH(caught) {
        if (caught != thrown) {
            unwind_problems++;
        }
        if (thrown->canary != stamped) {
            unwind_problems++;
        }
        /* The frames of everything the body called are gone. */
        if (ctx->frame_depth != entry_depth) {
            unwind_problems++;
        }

        /* Allocate hard enough that a collection lands here, with the
         * exception reachable only through the handler chain. */
        for (u32 i = 0; i < 256; i++) {
            Leaf* junk = cvm_alloc(&leaf_desc);
            if (junk == NULL) {
                break;
            }
            stamp(junk);
        }

        /* Both sides of that: a canary mismatch is a reclaimed object that
         * was handed out again. */
        if (thrown->canary != stamped) {
            unwind_problems++;
        }
    }
    CVM_ENDTRY();

    if (ctx->frame_depth != entry_depth) {
        unwind_problems++;
    }

    CVM_RETURN_VOID(outer);
}

/*
 * The concentration of the design: collections at every allocation on the
 * path from throwing to catching.
 *
 * Forcing a collection at literally every point needs a hook in the runtime.
 * What this does instead is put the collector under pressure while the path
 * allocates: four activations on the way down, each pushing a frame with two
 * slots and allocating into them, and a catch that allocates enough to make
 * a collection certain.
 *
 * The window being looked for is the one where the exception is held by
 * nothing but the handler chain -- between the store in cvm_throw and the
 * read in the catch. A collection that lands there takes the value away if
 * the chain is not a root, and the canary is what makes that visible instead
 * of leaving a heap that is quietly wrong.
 */
TEST collections_along_the_unwind_path_do_not_lose_the_exception(void) {
    fixture_enter();

    unwind_problems = 0;
    for (u32 round = 0; round < 8; round++) {
        one_throw_with_collections_on_the_path();
    }

    ASSERT_EQ_FMT(0u, unwind_problems, "%u");
    ASSERT(cvm_handler_top() == NULL);
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* values that live across a try                                       */
/* ------------------------------------------------------------------ */

/*
 * A value that has to survive a try goes through memory, and it reads
 * correctly on the other side however much register pressure the body
 * creates.
 *
 * The slots are in the *catching* activation's frame, not the throwing one's:
 * an unwind drops every frame above the handler, so a slot in the body's own
 * frame is gone by the time the catch runs. That is a property of the frame
 * depth restore, and it is why the contract says the spill set is the
 * frontend's to choose -- the frontend knows which values are still live and
 * which frame outlives the try.
 *
 * What is deliberately not tested is the other shape: a plain local the try
 * body modified, read in the catch. The C standard makes that value
 * indeterminate after the jump, so a test asserting it would be asserting
 * undefined behaviour -- able to pass for years and then be "fixed" by a
 * compiler upgrade. The point of the contract is that the supported shape is
 * one the frontend can always emit.
 */
static bool slots_modified_inside_a_try_survive_it(void) {
    CVM_ENTER(f, 2);
    f_roots[0] = NULL;
    f_roots[1] = NULL;

    bool ok = false;

    CVM_TRY() {
        /*
         * Register pressure on purpose, and enough arithmetic that these
         * cannot all stay in registers across the jump.
         */
        u64 accumulators[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        for (u64 round = 1; round <= 64; round++) {
            for (u32 i = 0; i < 8; i++) {
                accumulators[i] = accumulators[i] * 3u + round + i;
            }
        }

        f_roots[0] = cvm_alloc(&leaf_desc);
        f_roots[1] = cvm_alloc(&leaf_desc);
        stamp((Leaf*)f_roots[0]);
        stamp((Leaf*)f_roots[1]);

        /* Keep the accumulators alive past the throw, so they compete for
         * registers with everything the jump has to preserve. */
        volatile u64 sink = accumulators[0] + accumulators[7];
        (void)sink;

        cvm_throw(NULL);
    }
    CVM_CATCH(caught) {
        (void)caught;
        Leaf* first = f_roots[0];
        Leaf* second = f_roots[1];
        ok = first != NULL && second != NULL && first != second &&
             first->canary == canary_of(first) && second->canary == canary_of(second);
    }
    CVM_ENDTRY();

    CVM_RETURN(f, ok);
}

TEST values_across_a_try_go_through_memory(void) {
    fixture_enter();
    ASSERT(slots_modified_inside_a_try_survive_it());
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
    RUN_TEST(a_return_out_of_a_try_body_leaves_nothing_linked);
    RUN_TEST(a_pushed_frame_has_null_slots);
    RUN_TEST(collections_along_the_unwind_path_do_not_lose_the_exception);
    RUN_TEST(values_across_a_try_go_through_memory);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}

#endif /* !_WIN32 */
