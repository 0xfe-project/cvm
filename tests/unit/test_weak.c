/*
 * tests/unit/test_weak.c
 *
 * Weak references and the nullify phase.
 *
 * The property that matters most is the one that makes weak references
 * useful: they must not keep their target alive. A test that only checks
 * "the target is cleared when it dies" would pass for an implementation
 * that never traced the target and one that traced it and then cleared it
 * anyway; what distinguishes them is whether the target is still marked.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <stddef.h>
#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "gc/heap.h"
#include "gc/marker.h"
#include "gc/weak.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)16 * 1024 * 1024)

/* A node with one outgoing pointer, as a strong holder. */
typedef struct Node {
    opaque_mut next;
} Node;

static CvmPtrField const node_fields[] = {{.offset = offsetof(Node, next)}};

static CvmTypeDescriptor const node_desc = {
    .size = sizeof(Node),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 1,
    .num_embed_fields = 0,
    .ptr_fields = node_fields,
    .embed_fields = NULL,
};

/* A leaf, to be weakly referenced. */
typedef struct Leaf {
    u64 value;
} Leaf;

static CvmTypeDescriptor const leaf_desc = {
    .size = sizeof(Leaf),
    .kind = CVM_TYPE_SCALAR,
};

/* ------------------------------------------------------------------ */
/* fixture                                                             */
/* ------------------------------------------------------------------ */

static CvmHeap* heap(void) {
    return &cvm_world()->heap;
}

static void fixture_enter(void) {
    (void)cvm_thread_attach();
}

static void fixture_leave(void) {
    cvm_thread_detach();
}

/* Start from a state where every packet is on the empty list. */
static void reset_marking(void) {
    CvmGreyPacket* packet;
    while ((packet = cvm_grey_list_pop(&heap()->mark.found_weak_refs,
                                       heap()->mark.packets)) != NULL) {
        cvm_grey_list_push(&heap()->mark.empty, heap()->mark.packets, packet);
    }
}

static void mark_all(void) {
    cvm_mark_roots(heap());
    cvm_mark_until_done(heap());
}

#define FRAME(slots)                                                                   \
    CVM_FRAME_DECL(f, slots);                                                          \
    cvm_frame_push(&f_frame, f_roots, slots);                                          \
    for (u32 i = 0; i < (slots); i++) {                                                \
        f_roots[i] = NULL;                                                             \
    }

#define FRAME_END() cvm_frame_pop(&f_frame)

/* ------------------------------------------------------------------ */
/* the built-in type                                                   */
/* ------------------------------------------------------------------ */

TEST the_weak_ref_type_describes_one_pointer(void) {
    ASSERT_EQ_FMT((u32)sizeof(CvmWeakRef), (u32)cvm_type_weak_ref.size, "%u");
    ASSERT_EQ_FMT((u32)CVM_TYPE_STRUCT, cvm_type_weak_ref.kind, "%u");
    ASSERT_EQ_FMT(1u, (u32)cvm_type_weak_ref.num_ptr_fields, "%u");
    ASSERT_EQ_FMT((u32)offsetof(CvmWeakRef, target),
                  (u32)cvm_type_weak_ref.ptr_fields[0].offset, "%u");
    PASS();
}

TEST a_new_weak_ref_carries_the_flag(void) {
    fixture_enter();

    Leaf* leaf = cvm_alloc(&leaf_desc);
    void* weak = cvm_weak_ref_new(leaf);

    ASSERT(cvm_weak_ref_target(weak) == leaf);
    /* the flag is what makes the marker record rather than trace it */
    ASSERT((cvm_header_of(weak)->flags & CVM_GC_WEAK_REF) != 0);

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* a weak reference does not keep its target alive                     */
/* ------------------------------------------------------------------ */

TEST a_weak_ref_does_not_keep_its_target_alive(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    Leaf* leaf = cvm_alloc(&leaf_desc);
    void* weak = cvm_weak_ref_new(leaf);
    f_roots[0] = weak; /* the weak reference is reachable, the leaf is not */

    mark_all();

    /* the reference itself survives */
    ASSERT(cvm_object_is_marked(heap(), weak));
    /* but its target does not: marking must not have followed it */
    ASSERT_FALSE(cvm_object_is_marked(heap(), leaf));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST nullify_clears_a_dead_target(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    Leaf* leaf = cvm_alloc(&leaf_desc);
    void* weak = cvm_weak_ref_new(leaf);
    f_roots[0] = weak;

    mark_all();

    u32 cleared = cvm_weak_nullify(heap());
    ASSERT_EQ_FMT(1u, cleared, "%u");
    ASSERT(cvm_weak_ref_target(weak) == NULL);
    /* the reference object itself is untouched */
    ASSERT(cvm_object_is_marked(heap(), weak));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_strongly_reachable_target_survives(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    Leaf* leaf = cvm_alloc(&leaf_desc);
    Node* holder = cvm_alloc(&node_desc);
    holder->next = leaf; /* a strong path to the same object */

    void* weak = cvm_weak_ref_new(leaf);

    /* both are roots: the holder strongly, the weak reference weakly */
    CVM_FRAME_DECL(f2, 2);
    cvm_frame_push(&f2_frame, f2_roots, 2);
    f2_roots[0] = holder;
    f2_roots[1] = weak;

    mark_all();

    ASSERT(cvm_object_is_marked(heap(), leaf));

    u32 cleared = cvm_weak_nullify(heap());
    ASSERT_EQ_FMT(0u, cleared, "%u");
    /* the target survived because something else reached it */
    ASSERT(cvm_weak_ref_target(weak) == leaf);

    cvm_frame_pop(&f2_frame);
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_null_target_needs_no_action(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    void* weak = cvm_weak_ref_new(NULL);
    f_roots[0] = weak;

    mark_all();

    u32 cleared = cvm_weak_nullify(heap());
    ASSERT_EQ_FMT(0u, cleared, "%u");
    ASSERT(cvm_weak_ref_target(weak) == NULL);

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST many_weak_refs_are_all_handled(void) {
    fixture_enter();
    reset_marking();

    enum { N = 20 };

    CVM_FRAME_DECL(f, N);
    cvm_frame_push(&f_frame, f_roots, N);
    for (u32 i = 0; i < N; i++) {
        f_roots[i] = NULL;
    }

    void* weak[N];
    void* alive_target = cvm_alloc(&leaf_desc);

    for (int i = 0; i < N; i++) {
        Leaf* leaf = cvm_alloc(&leaf_desc);
        weak[i] = cvm_weak_ref_new(leaf);
        f_roots[i] = weak[i];
    }

    /* one extra weak reference whose target is also strongly reachable */
    Node* holder = cvm_alloc(&node_desc);
    holder->next = alive_target;
    void* survivor = cvm_weak_ref_new(alive_target);

    /* a second frame for the holder and the survivor */
    CVM_FRAME_DECL(f2, 2);
    cvm_frame_push(&f2_frame, f2_roots, 2);
    f2_roots[0] = holder;
    f2_roots[1] = survivor;

    mark_all();

    u32 cleared = cvm_weak_nullify(heap());

    /* every dead target was cleared */
    ASSERT_EQ_FMT((u32)N, cleared, "%u");
    for (int i = 0; i < N; i++) {
        ASSERT(cvm_weak_ref_target(weak[i]) == NULL);
        ASSERT(cvm_object_is_marked(heap(), weak[i]));
    }
    /* the one with a strong path was not */
    ASSERT(cvm_weak_ref_target(survivor) == alive_target);

    cvm_frame_pop(&f2_frame);
    cvm_frame_pop(&f_frame);
    fixture_leave();
    PASS();
}

TEST nullify_returns_every_packet_to_the_empty_list(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    for (int i = 0; i < 30; i++) {
        f_roots[0] = cvm_weak_ref_new(cvm_alloc(&leaf_desc));
    }

    mark_all();
    ASSERT_FALSE(cvm_weak_is_done(heap())); /* weak packets are outstanding */

    cvm_weak_nullify(heap());

    ASSERT(cvm_weak_is_done(heap()));
    ASSERT_EQ_FMT(0u, cvm_grey_list_size(&heap()->mark.found_weak_refs), "%u");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST nullify_with_nothing_to_do_is_harmless(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);
    mark_all();

    ASSERT_EQ_FMT(0u, cvm_weak_nullify(heap()), "%u");
    ASSERT_EQ_FMT(0u, cvm_weak_nullify(heap()), "%u");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_weak_reference_can_be_retargeted(void) {
    fixture_enter();

    Leaf* first = cvm_alloc(&leaf_desc);
    Leaf* second = cvm_alloc(&leaf_desc);

    void* weak = cvm_weak_ref_new(first);
    ASSERT(cvm_weak_ref_target(weak) == first);

    cvm_weak_ref_set_target(weak, second);
    ASSERT(cvm_weak_ref_target(weak) == second);

    cvm_weak_ref_set_target(weak, NULL);
    ASSERT(cvm_weak_ref_target(weak) == NULL);

    fixture_leave();
    PASS();
}

SUITE(weak_suite) {
    RUN_TEST(the_weak_ref_type_describes_one_pointer);
    RUN_TEST(a_new_weak_ref_carries_the_flag);
    RUN_TEST(a_weak_ref_does_not_keep_its_target_alive);
    RUN_TEST(nullify_clears_a_dead_target);
    RUN_TEST(a_strongly_reachable_target_survives);
    RUN_TEST(a_null_target_needs_no_action);
    RUN_TEST(many_weak_refs_are_all_handled);
    RUN_TEST(nullify_returns_every_packet_to_the_empty_list);
    RUN_TEST(nullify_with_nothing_to_do_is_harmless);
    RUN_TEST(a_weak_reference_can_be_retargeted);
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
    RUN_SUITE(weak_suite);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}

#endif /* !_WIN32 */
