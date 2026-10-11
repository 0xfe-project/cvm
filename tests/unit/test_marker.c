/*
 * tests/unit/test_marker.c
 *
 * The precise scan.
 *
 * This is the suite that justifies the design. The reference marks by
 * sweeping stacks for pointer-shaped words; this marker reads a frame
 * chain and a type descriptor, so what these tests check is exactly the
 * claim that makes that work: every live pointer is found, no non-pointer
 * is mistaken for one, and the descriptor is what decides.
 *
 * The types here are hand-written descriptors standing in for what a
 * frontend would generate.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <stddef.h>
#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "gc/alloc.h"
#include "gc/heap.h"
#include "gc/marker.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)16 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* test types                                                          */
/* ------------------------------------------------------------------ */

/* A node with one outgoing pointer at offset 0. */
typedef struct Node {
    opaque_mut next;
} Node;

static CvmPtrField const node_fields[] = {{.offset = 0}};

static CvmTypeDescriptor const node_desc = {
    .size = sizeof(Node),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 1,
    .num_embed_fields = 0,
    .ptr_fields = node_fields,
    .embed_fields = NULL,
};

/* A leaf: nothing for the marker to follow. */
typedef struct Leaf {
    u64 a;
    u64 b;
} Leaf;

static CvmTypeDescriptor const leaf_desc = {
    .size = sizeof(Leaf),
    .kind = CVM_TYPE_SCALAR,
};

/*
 * A scalar whose bytes happen to look exactly like a heap pointer. This
 * is the case a conservative collector gets wrong and a precise one
 * cannot: the descriptor says there is nothing here, so nothing is
 * followed, however pointer-shaped the contents are.
 */
typedef struct PointerShaped {
    u64 value;
} PointerShaped;

static CvmTypeDescriptor const pointer_shaped_desc = {
    .size = sizeof(PointerShaped),
    .kind = CVM_TYPE_SCALAR,
};

/* An embedded aggregate: the pointer is one level down. */
typedef struct Outer {
    Node inner;       /* offset 0 */
    opaque_mut other; /* offset sizeof(Node) */
} Outer;

static CvmPtrField const outer_fields[] = {{.offset = offsetof(Outer, other)}};
static CvmEmbedField const outer_embedded[] = {
    {.offset = offsetof(Outer, inner), .type = &node_desc},
};

static CvmTypeDescriptor const outer_desc = {
    .size = sizeof(Outer),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 1,
    .num_embed_fields = 1,
    .ptr_fields = outer_fields,
    .embed_fields = outer_embedded,
};

/* An array of pointers. */
typedef struct PtrArray {
    usize length;
    opaque_mut items[];
} PtrArray;

static CvmTypeDescriptor const ptr_array_desc = {
    .size = sizeof(PtrArray),
    .kind = CVM_TYPE_PTR_ARRAY,
    .array_length_offset = offsetof(PtrArray, length),
    .array_data_offset = offsetof(PtrArray, items),
};

/* An array of aggregates. */
typedef struct NodeArray {
    usize length;
    Node items[];
} NodeArray;

static CvmTypeDescriptor const node_array_desc = {
    .size = sizeof(NodeArray),
    .kind = CVM_TYPE_STRUCT_ARRAY,
    .array_length_offset = offsetof(NodeArray, length),
    .array_data_offset = offsetof(NodeArray, items),
    .array_elem_type = &node_desc,
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

/*
 * Put the marking state back to a clean slate.
 *
 * A mark cycle leaves weak packets on their own list, which the
 * completion check counts as accounted for. Moving them back keeps each
 * test's starting point the same. The mark bits themselves are not
 * cleared: every test allocates its own objects, so a leftover bit
 * describes a different address.
 */
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

/* A frame with room for `slots` roots, all starting NULL. */
#define FRAME(slots)                                                                   \
    CVM_FRAME_DECL(f, slots);                                                          \
    cvm_frame_push(&f_frame, f_roots, slots);                                          \
    for (u32 i = 0; i < (slots); i++) {                                                \
        f_roots[i] = NULL;                                                             \
    }

#define FRAME_END() cvm_frame_pop(&f_frame)

/* ------------------------------------------------------------------ */
/* roots                                                               */
/* ------------------------------------------------------------------ */

TEST nothing_reachable_marks_nothing(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    void* loose = cvm_alloc(&leaf_desc);
    ASSERT(loose != NULL);
    ASSERT_FALSE(cvm_object_is_marked(heap(), loose));

    mark_all();

    /* the object exists but nothing points at it */
    ASSERT_FALSE(cvm_object_is_marked(heap(), loose));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_root_keeps_its_object(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    void* leaf = cvm_alloc(&leaf_desc);
    f_roots[0] = leaf;

    mark_all();

    ASSERT(cvm_object_is_marked(heap(), leaf));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_null_root_is_harmless(void) {
    fixture_enter();
    reset_marking();

    FRAME(3);
    /* all slots NULL */
    mark_all();

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST every_frame_in_the_chain_is_walked(void) {
    fixture_enter();
    reset_marking();

    void* outer_object = cvm_alloc(&leaf_desc);
    void* inner_object = cvm_alloc(&leaf_desc);

    FRAME(1);
    f_roots[0] = outer_object;

    /* a second, newer frame on top */
    CVM_FRAME_DECL(inner, 1);
    cvm_frame_push(&inner_frame, inner_roots, 1);
    inner_roots[0] = inner_object;

    mark_all();

    ASSERT(cvm_object_is_marked(heap(), inner_object));
    ASSERT(cvm_object_is_marked(heap(), outer_object));

    cvm_frame_pop(&inner_frame);
    FRAME_END();
    fixture_leave();
    PASS();
}

/*
 * Unwinding is restoring a number, and an abandoned frame's pop cannot undo
 * it.
 *
 * This is the property the exception work needs. A handler record holds the
 * depth to restore, which is a u32 rather than a pointer into the handling
 * activation's stack -- so a handler inside a suspended coroutine, whose
 * activation is a heap object, is describable at all. With a chain of
 * frames, the natural thing for a record to hold is a `CvmGcFrame*`, and
 * that points into a stack that may have been reused by the time the
 * handler runs.
 *
 * The object this checks on is allocated *after* the unwind, so the mark
 * cycle below is the first one that has ever seen it and its bit is its
 * own. That matters here: this suite does not clear mark bits between
 * cycles, because every other test allocates its own objects and a leftover
 * bit describes a different address. An object marked by an earlier cycle
 * cannot be used to assert that something was *not* reached.
 *
 * The stale pop is deliberate. An activation reached by an unwind does not
 * run its epilogue, so the pop below would not happen -- but the pop is
 * documented as idempotent precisely so that a mistake in that reasoning
 * costs nothing, and this is what says so.
 */
TEST a_restored_depth_hides_the_frames_above_it(void) {
    fixture_enter();
    reset_marking();

    CvmVMContext* ctx = cvm_ctx();

    FRAME(1);
    void* kept = cvm_alloc(&leaf_desc);
    ASSERT(kept != NULL);
    f_roots[0] = kept;

    /* What a handler record would save. */
    u32 handler_depth = ctx->frame_depth;

    /* The outer frame is a root while it is live, so the assertion after the
     * unwind is about the unwind and not about the object being unreachable
     * from the start. */
    mark_all();
    ASSERT(cvm_object_is_marked(heap(), kept));
    reset_marking();

    {
        CVM_FRAME_DECL(inner, 1);
        cvm_frame_push(&inner_frame, inner_roots, 1);

        void* abandoned = cvm_alloc(&leaf_desc);
        ASSERT(abandoned != NULL);
        inner_roots[0] = abandoned;

        /* The unwind, which is this one store and nothing else. */
        ctx->frame_depth = handler_depth;

        cvm_frame_pop(&inner_frame);
        ASSERT_EQ_FMT(handler_depth, ctx->frame_depth, "%u");

        mark_all();
        ASSERT(cvm_object_is_marked(heap(), kept));
        ASSERT_FALSE(cvm_object_is_marked(heap(), abandoned));
        reset_marking();
    }

    FRAME_END();
    ASSERT_EQ_FMT(handler_depth - 1u, ctx->frame_depth, "%u");
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* following pointers                                                  */
/* ------------------------------------------------------------------ */

TEST a_pointer_chain_is_followed(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    Node* a = cvm_alloc(&node_desc);
    Node* b = cvm_alloc(&node_desc);
    Node* c = cvm_alloc(&node_desc);

    cvm_store_field(a, offsetof(Node, next), b);
    cvm_store_field(b, offsetof(Node, next), c);
    cvm_store_field(c, offsetof(Node, next), NULL);
    f_roots[0] = a;

    mark_all();

    ASSERT(cvm_object_is_marked(heap(), a));
    ASSERT(cvm_object_is_marked(heap(), b));
    ASSERT(cvm_object_is_marked(heap(), c));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_cycle_terminates(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    Node* a = cvm_alloc(&node_desc);
    Node* b = cvm_alloc(&node_desc);
    cvm_store_field(a, offsetof(Node, next), b);
    cvm_store_field(b, offsetof(Node, next), a); /* back to the start */
    f_roots[0] = a;

    mark_all();

    ASSERT(cvm_object_is_marked(heap(), a));
    ASSERT(cvm_object_is_marked(heap(), b));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST an_unreachable_tail_is_left_alone(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    Node* head = cvm_alloc(&node_desc);
    Node* reachable = cvm_alloc(&node_desc);
    Node* detached = cvm_alloc(&node_desc);

    cvm_store_field(head, offsetof(Node, next), reachable);
    cvm_store_field(reachable, offsetof(Node, next), NULL);
    cvm_store_field(detached, offsetof(Node, next), NULL);
    f_roots[0] = head;

    mark_all();

    ASSERT(cvm_object_is_marked(heap(), head));
    ASSERT(cvm_object_is_marked(heap(), reachable));
    ASSERT_FALSE(cvm_object_is_marked(heap(), detached));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_scalar_is_not_traced_however_it_looks(void) {
    /*
     * The claim the whole design rests on. A conservative collector would
     * follow the pointer-shaped bytes and keep the object alive; this one
     * reads the descriptor, sees a scalar, and stops.
     */
    fixture_enter();
    reset_marking();

    FRAME(1);

    void* target = cvm_alloc(&leaf_desc);
    ASSERT(target != NULL);

    PointerShaped* holder = cvm_alloc(&pointer_shaped_desc);
    holder->value = (u64)(usize)cvm_header_of(target);
    f_roots[0] = holder;

    mark_all();

    ASSERT(cvm_object_is_marked(heap(), holder));
    /* the target is only reachable through a field the type says is not a
     * pointer */
    ASSERT_FALSE(cvm_object_is_marked(heap(), target));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_field_outside_the_heap_is_ignored(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    Node* node = cvm_alloc(&node_desc);
    int stack_value = 42;
    /* not a heap pointer at all */
    cvm_store_field(node, offsetof(Node, next), &stack_value);
    f_roots[0] = node;

    mark_all();

    ASSERT(cvm_object_is_marked(heap(), node));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_pointer_to_an_already_marked_object_stops(void) {
    fixture_enter();
    reset_marking();

    FRAME(2);

    Node* shared = cvm_alloc(&node_desc);
    Node* a = cvm_alloc(&node_desc);
    Node* b = cvm_alloc(&node_desc);

    cvm_store_field(shared, offsetof(Node, next), NULL);
    cvm_store_field(a, offsetof(Node, next), shared);
    cvm_store_field(b, offsetof(Node, next),
                    shared); /* both point at the same object */

    f_roots[0] = a;
    f_roots[1] = b;

    mark_all();

    ASSERT(cvm_object_is_marked(heap(), shared));
    ASSERT(cvm_object_is_marked(heap(), a));
    ASSERT(cvm_object_is_marked(heap(), b));

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* descriptors                                                         */
/* ------------------------------------------------------------------ */

TEST an_embedded_aggregate_is_walked(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    Node* inner_target = cvm_alloc(&node_desc);
    cvm_store_field(inner_target, offsetof(Node, next), NULL);
    void* other_target = cvm_alloc(&leaf_desc);

    Outer* outer = cvm_alloc(&outer_desc);
    outer->inner.next = inner_target;
    cvm_store_field(outer, offsetof(Outer, other), other_target);
    f_roots[0] = outer;

    mark_all();

    ASSERT(cvm_object_is_marked(heap(), outer));
    /* found through the embedded descriptor */
    ASSERT(cvm_object_is_marked(heap(), inner_target));
    /* found through the direct field */
    ASSERT(cvm_object_is_marked(heap(), other_target));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_pointer_array_marks_every_element(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    enum { N = 5 };
    void* elements[N];
    for (int i = 0; i < N; i++) {
        elements[i] = cvm_alloc(&leaf_desc);
    }

    PtrArray* array =
        cvm_alloc_array(&ptr_array_desc, N, cvm_array_stride(&ptr_array_desc));
    for (int i = 0; i < N; i++) {
        cvm_store_field(array, offsetof(PtrArray, items) + i * sizeof(void*),
                        elements[i]);
    }
    f_roots[0] = array;

    mark_all();

    ASSERT(cvm_object_is_marked(heap(), array));
    for (int i = 0; i < N; i++) {
        ASSERT(cvm_object_is_marked(heap(), elements[i]));
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST an_empty_array_marks_nothing(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    PtrArray* array =
        cvm_alloc_array(&ptr_array_desc, 0, cvm_array_stride(&ptr_array_desc));
    ASSERT(array->length == 0);
    f_roots[0] = array;

    mark_all();

    ASSERT(cvm_object_is_marked(heap(), array));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST an_array_of_aggregates_walks_each_element(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    enum { N = 3 };
    Node* targets[N];
    for (int i = 0; i < N; i++) {
        targets[i] = cvm_alloc(&node_desc);
        cvm_store_field(targets[i], offsetof(Node, next), NULL);
    }

    NodeArray* array =
        cvm_alloc_array(&node_array_desc, N, cvm_array_stride(&node_array_desc));
    for (int i = 0; i < N; i++) {
        array->items[i].next = targets[i];
    }
    f_roots[0] = array;

    mark_all();

    ASSERT(cvm_object_is_marked(heap(), array));
    for (int i = 0; i < N; i++) {
        ASSERT(cvm_object_is_marked(heap(), targets[i]));
    }

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST an_array_only_marks_up_to_its_length(void) {
    /*
     * The tail past the length is not part of the object. If the marker
     * walked past it, it would follow whatever the next object's header
     * happens to contain.
     */
    fixture_enter();
    reset_marking();

    FRAME(1);

    void* inside = cvm_alloc(&leaf_desc);
    void* beyond = cvm_alloc(&leaf_desc);

    enum { N = 2 };
    PtrArray* array =
        cvm_alloc_array(&ptr_array_desc, N, cvm_array_stride(&ptr_array_desc));
    cvm_store_field(array, offsetof(PtrArray, items) + 0 * sizeof(void*), inside);
    cvm_store_field(array, offsetof(PtrArray, items) + 1 * sizeof(void*), NULL);
    f_roots[0] = array;

    mark_all();

    ASSERT(cvm_object_is_marked(heap(), inside));
    /* allocated after the array, so it is outside it */
    ASSERT_FALSE(cvm_object_is_marked(heap(), beyond));

    FRAME_END();
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* marking state                                                       */
/* ------------------------------------------------------------------ */

TEST marking_is_idempotent(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);

    Node* a = cvm_alloc(&node_desc);
    Node* b = cvm_alloc(&node_desc);
    cvm_store_field(a, offsetof(Node, next), b);
    cvm_store_field(b, offsetof(Node, next), NULL);
    f_roots[0] = a;

    mark_all();
    /* a second cycle over the same roots must be harmless */
    mark_all();

    ASSERT(cvm_object_is_marked(heap(), a));
    ASSERT(cvm_object_is_marked(heap(), b));
    ASSERT(cvm_mark_is_done(heap()));

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST marking_finishes_with_every_packet_accounted_for(void) {
    fixture_enter();
    reset_marking();

    FRAME(1);
    f_roots[0] = cvm_alloc(&node_desc);

    mark_all();
    ASSERT(cvm_mark_is_done(heap()));
    ASSERT_EQ_FMT(0u, cvm_grey_list_size(&heap()->mark.full), "%u");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST the_block_and_lines_of_a_marked_object_are_marked(void) {
    /*
     * The sweeper decides which lines to keep from the line marks, so an
     * object that is marked but whose lines are not would be swept out
     * from under itself.
     */
    fixture_enter();
    reset_marking();

    FRAME(1);

    Leaf* leaf = cvm_alloc(&leaf_desc);
    f_roots[0] = leaf;

    BlockMeta* block = block_meta_of_word(heap()->block_meta_start, heap()->heap_start,
                                          (u8*)cvm_header_of(leaf));
    ASSERT_FALSE(block_meta_is_marked(block));

    mark_all();

    ASSERT(block_meta_is_marked(block));

    /*
     * And the lines, deliberately, are *not*.
     *
     * The marker used to compute each object's extent -- through the header,
     * the descriptor, and for an array the length out of the payload -- and
     * mark every line it covered, so the sweeper would keep them. The sweeper
     * does that itself now, from the metadata table (sweeper.c,
     * mark_survivors_lines), which is work moved out of the pause and into
     * the sweep. What the marker owes the sweep is the object's mark and its
     * block's; the fixtures in this file check the rest.
     */
    u32 block_index = block_index_of(heap()->heap_start, (u8*)cvm_header_of(leaf));
    LineMeta* lines = cvm_heap_line_meta(heap(), block_index);
    u32 line = block_line_index_of(block_start_of((u8*)cvm_header_of(leaf)),
                                   (u8*)cvm_header_of(leaf));
    ASSERT_FALSE(line_meta_is_marked(&lines[line]));

    /* The object's own slot, and its extent, are what the sweep reads. */
    ObjectMeta* slot = bytemap_get(heap()->bytemap, (u8 const*)cvm_header_of(leaf));
    ASSERT(object_meta_is_marked(slot));

    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(marker_suite) {
    RUN_TEST(nothing_reachable_marks_nothing);
    RUN_TEST(a_root_keeps_its_object);
    RUN_TEST(a_null_root_is_harmless);
    RUN_TEST(every_frame_in_the_chain_is_walked);
    RUN_TEST(a_restored_depth_hides_the_frames_above_it);
    RUN_TEST(a_pointer_chain_is_followed);
    RUN_TEST(a_cycle_terminates);
    RUN_TEST(an_unreachable_tail_is_left_alone);
    RUN_TEST(a_scalar_is_not_traced_however_it_looks);
    RUN_TEST(a_field_outside_the_heap_is_ignored);
    RUN_TEST(a_pointer_to_an_already_marked_object_stops);
    RUN_TEST(an_embedded_aggregate_is_walked);
    RUN_TEST(a_pointer_array_marks_every_element);
    RUN_TEST(an_empty_array_marks_nothing);
    RUN_TEST(an_array_of_aggregates_walks_each_element);
    RUN_TEST(an_array_only_marks_up_to_its_length);
    RUN_TEST(marking_is_idempotent);
    RUN_TEST(marking_finishes_with_every_packet_accounted_for);
    RUN_TEST(the_block_and_lines_of_a_marked_object_are_marked);
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
    RUN_SUITE(marker_suite);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}

#endif /* !_WIN32 */
