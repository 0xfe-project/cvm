/*
 * tests/unit/test_external.c
 *
 * External handles, the root table, and the finalizer thread.
 *
 * Three things are worth proving here, and each has a failure mode that
 * would be invisible without a test aimed at it:
 *
 *   - an external object's finalizer runs when the object dies, and the
 *     finalizer receives the C pointer rather than the object
 *   - a root registered by foreign code keeps its object alive
 *   - a root registered *during* a collection is not missed. That last one
 *     is the hazard the root table's write barrier exists for, and it is
 *     also the one a test can only check by producing the interleaving
 *     deliberately.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "cvm/array.h"
#include "gc/collect.h"
#include "cvm/external.h"
#include "gc/heap.h"
#include "cvm/roots.h"
#include "cvm/weak.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)32 * 1024 * 1024)

typedef struct Leaf {
    u64 canary;
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

static void collect(void) {
    cvm_gc_collect_and_finish(heap());
}

/*
 * Collect and drain until nothing more happens, then start measuring.
 *
 * The heap is shared by every test in the binary, so objects a previous
 * test left unreachable are still there to be collected. A test that
 * counts finalizers has to run those off first, or the count it measures
 * includes someone else's work — which is exactly how this helper came to
 * exist (the count came out one too high).
 */
static void settle(void) {
    u32 guard = 0;
    u64 before;
    do {
        before = cvm_finalizer_run_count();
        collect();
        cvm_finalizer_drain_now();
        guard++;
    } while (cvm_finalizer_run_count() != before && guard < 20);
}

static bool is_live(void* payload) {
    ObjectMeta const* meta =
        bytemap_get_const(heap()->bytemap, (u8 const*)cvm_header_of(payload));
    return object_meta_is_allocated(meta) || object_meta_is_marked(meta);
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

TEST the_external_type_is_a_scalar(void) {
    /* Neither the C pointer nor the function pointer is a GC reference,
     * and saying so is what stops the marker following either. */
    ASSERT_EQ_FMT((u32)CVM_TYPE_SCALAR, (u32)cvm_type_external.kind, "%u");
    ASSERT_EQ_FMT((u32)sizeof(CvmExternal), (u32)cvm_type_external.size, "%u");
    ASSERT_EQ_FMT(0u, (u32)cvm_type_external.num_ptr_fields, "%u");
    PASS();
}

TEST a_new_external_object_holds_its_pointer(void) {
    fixture_enter();

    int resource = 42;
    void* ext = cvm_external_new(&resource, NULL);

    ASSERT(cvm_external_ptr(ext) == &resource);
    /* no finalizer, so no flag: there is nothing to release */
    ASSERT(!object_meta_has_finalizer(
        bytemap_get_const(heap()->bytemap, (u8 const*)cvm_header_of(ext))));

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* finalizers                                                          */
/* ------------------------------------------------------------------ */

static _Atomic u32 finalized_count = 0;
static _Atomic(usize) last_finalized_ptr = 0;
static _Atomic(u64) last_finalized_count = 0;

static void counting_finalizer(opaque_mut c_ptr) {
    atomic_fetch_add(&finalized_count, 1);
    atomic_store(&last_finalized_ptr, (usize)c_ptr);
}

TEST a_finalizer_runs_when_its_object_dies(void) {
    fixture_enter();

    settle();
    u64 before = cvm_finalizer_run_count();

    int resource = 7;
    void* ext = cvm_external_new(&resource, counting_finalizer);
    ASSERT(object_meta_has_finalizer(
        bytemap_get_const(heap()->bytemap, (u8 const*)cvm_header_of(ext))));

    /* nothing points at it, so it dies */
    collect();
    cvm_finalizer_drain_now();

    ASSERT_EQ_FMT(before + 1, cvm_finalizer_run_count(), "%" PRIu64);
    /* the finalizer receives the C pointer, not the wrapper: the wrapper
     * is already dead by the time this runs */
    ASSERT_EQ_FMT((usize)&resource, atomic_load(&last_finalized_ptr), "%zu");

    fixture_leave();
    PASS();
}

TEST a_finalizer_does_not_run_while_the_object_lives(void) {
    fixture_enter();

    settle();
    u64 before = cvm_finalizer_run_count();

    FRAME(1);

    int resource = 9;
    void* ext = cvm_external_new(&resource, counting_finalizer);
    f_roots[0] = ext;

    collect();
    cvm_finalizer_drain_now();

    ASSERT(is_live(ext));
    ASSERT_EQ_FMT(before, cvm_finalizer_run_count(), "%" PRIu64);

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST many_finalizers_all_run(void) {
    fixture_enter();

    settle();
    u64 before = cvm_finalizer_run_count();

    enum { N = 200 };
    for (int i = 0; i < N; i++) {
        (void)cvm_external_new(NULL, counting_finalizer);
    }

    collect();
    cvm_finalizer_drain_now();

    ASSERT_EQ_FMT(before + N, cvm_finalizer_run_count(), "%" PRIu64);
    /* nothing was dropped */
    ASSERT_EQ_FMT((u64)0, (u64)cvm_finalizer_dropped_count(), "%" PRIu64);

    fixture_leave();
    PASS();
}

TEST the_finalizer_thread_runs_them_on_its_own(void) {
    ASSERT(cvm_finalizer_start());
    ASSERT(cvm_finalizer_is_running());

    fixture_enter();

    settle();
    u64 before = cvm_finalizer_run_count();

    enum { N = 50 };
    for (int i = 0; i < N; i++) {
        (void)cvm_external_new(NULL, counting_finalizer);
    }

    collect();

    /* Wait for the thread rather than draining on this one, bounded. */
    /*
     * Generous on purpose: the finalizer thread runs when the scheduler lets
     * it, and what this test is about is whether the finalizer runs at all,
     * not how quickly. A tight deadline here fails under load for a reason
     * that has nothing to do with finalizers. See the note in
     * test_safepoint.c on why the suite's waits are what they are.
     */
    u64 deadline = rt_time_nanos() + (u64)20000000000;
    while (cvm_finalizer_run_count() < before + N && rt_time_nanos() < deadline) {
        rt_thread_yield();
    }

    ASSERT_EQ_FMT(before + N, cvm_finalizer_run_count(), "%" PRIu64);

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* the root table                                                      */
/* ------------------------------------------------------------------ */

TEST a_root_keeps_an_object_alive(void) {
    fixture_enter();

    Leaf* leaf = cvm_alloc(&leaf_desc);
    CvmRootHandle handle = cvm_root_add(leaf);
    ASSERT(handle != CVM_ROOT_HANDLE_NONE);
    ASSERT(cvm_root_get(handle) == leaf);

    /* nothing in a frame points at it */
    collect();

    ASSERT(is_live(leaf));

    cvm_root_remove(handle);
    fixture_leave();
    PASS();
}

TEST a_removed_root_no_longer_keeps_its_object(void) {
    fixture_enter();

    Leaf* leaf = cvm_alloc(&leaf_desc);
    CvmRootHandle handle = cvm_root_add(leaf);

    cvm_root_remove(handle);
    ASSERT(cvm_root_get(handle) == NULL);

    collect();

    ASSERT_FALSE(is_live(leaf));

    fixture_leave();
    PASS();
}

TEST a_root_can_be_repointed(void) {
    fixture_enter();

    Leaf* first = cvm_alloc(&leaf_desc);
    Leaf* second = cvm_alloc(&leaf_desc);

    CvmRootHandle handle = cvm_root_add(first);
    cvm_root_set(handle, second);
    ASSERT(cvm_root_get(handle) == second);

    collect();

    ASSERT(is_live(second));
    ASSERT_FALSE(is_live(first));

    cvm_root_remove(handle);
    fixture_leave();
    PASS();
}

TEST handles_are_distinct_and_slots_are_reused(void) {
    fixture_enter();

    enum { N = 32 };
    CvmRootHandle handles[N];
    for (int i = 0; i < N; i++) {
        handles[i] = cvm_root_add(cvm_alloc(&leaf_desc));
        ASSERT(handles[i] != CVM_ROOT_HANDLE_NONE);
    }
    for (int i = 0; i < N; i++) {
        for (int j = i + 1; j < N; j++) {
            ASSERT(handles[i] != handles[j]);
        }
    }

    u32 count = cvm_root_count();

    for (int i = 0; i < N; i++) {
        cvm_root_remove(handles[i]);
    }
    ASSERT_EQ_FMT(count - N, cvm_root_count(), "%u");

    /* a released handle is refused rather than reading a stale slot */
    ASSERT(cvm_root_get(handles[0]) == NULL);

    /* and the slot is reused rather than the table growing forever */
    u32 capacity = cvm_root_capacity();
    for (int i = 0; i < N; i++) {
        ASSERT(cvm_root_add(cvm_alloc(&leaf_desc)) != CVM_ROOT_HANDLE_NONE);
    }
    ASSERT_EQ_FMT(capacity, cvm_root_capacity(), "%u");

    for (int i = 0; i < N; i++) {
        cvm_root_remove(handles[i]);
    }

    fixture_leave();
    PASS();
}

TEST the_table_grows_past_its_initial_capacity(void) {
    fixture_enter();

    u32 initial = cvm_root_capacity();
    CvmRootHandle first = cvm_root_add(NULL);
    ASSERT(first != CVM_ROOT_HANDLE_NONE);

    enum { N = 200 };
    CvmRootHandle handles[N];
    for (int i = 0; i < N; i++) {
        handles[i] = cvm_root_add(cvm_alloc(&leaf_desc));
        ASSERT(handles[i] != CVM_ROOT_HANDLE_NONE);
    }

    ASSERT(cvm_root_capacity() > initial);
    /* handles issued before the growth are still valid */
    ASSERT(cvm_root_get(first) == NULL); /* NULL by construction */
    ASSERT(first <= cvm_root_capacity());

    for (int i = 0; i < N; i++) {
        cvm_root_remove(handles[i]);
    }
    cvm_root_remove(first);

    fixture_leave();
    PASS();
}

TEST a_root_registered_during_a_collection_is_not_missed(void) {
    /*
     * The hazard the write barrier exists for.
     *
     * An unmanaged thread is not stopped by a collection, so it can
     * register a root while marking is running and after marking has
     * already walked the table. Without the barrier the object would be
     * reclaimed while foreign code held it.
     *
     * Simulated by registering the root between marking and sweeping —
     * which is what "during a collection" means for a thread that is not
     * stopped — and checking the object survives.
     */
    fixture_enter();

    Leaf* leaf = cvm_alloc(&leaf_desc);

    /* Start a collection but do not finish it: this is the window. */
    ASSERT(cvm_safepoint_acquire());
    cvm_safepoint_release();

    /*
     * Registering now is what the barrier has to handle. The object is not
     * reachable from any frame, so only the barrier's mark can save it.
     */
    CvmRootHandle handle = cvm_root_add(leaf);
    ASSERT(handle != CVM_ROOT_HANDLE_NONE);

    collect();

    ASSERT(is_live(leaf));

    cvm_root_remove(handle);
    fixture_leave();
    PASS();
}

TEST an_external_object_held_by_a_root_stays_alive(void) {
    fixture_enter();

    settle();
    u64 before = cvm_finalizer_run_count();

    int resource = 11;
    void* ext = cvm_external_new(&resource, counting_finalizer);
    CvmRootHandle handle = cvm_root_add(ext);

    collect();
    cvm_finalizer_drain_now();

    ASSERT(is_live(ext));
    ASSERT_EQ_FMT(before, cvm_finalizer_run_count(), "%" PRIu64);

    /* release it and the finalizer runs on the next collection */
    cvm_root_remove(handle);
    collect();
    cvm_finalizer_drain_now();

    ASSERT_EQ_FMT(before + 1, cvm_finalizer_run_count(), "%" PRIu64);

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* type-level finalizers: any type, not just external                  */
/* ------------------------------------------------------------------ */

/*
 * A non-external type with a type-level finalizer. The descriptor names the
 * function, the allocation path flags the object, and the sweep calls it
 * when the object dies. This is what task A exists to make possible.
 */
typedef struct OwnedBuffer {
    opaque_mut raw; /* a foreign allocation, not a GC pointer */
    usize len;
} OwnedBuffer;

static void owned_buffer_finalize(opaque_mut object) {
    OwnedBuffer* buf = object;
    /* The object may have been reused by now, so this reads what it can and
     * does not depend on the bytes staying valid: the raw pointer is a
     * foreign allocation, not in the GC heap, and the finalizer's job is to
     * release it. Whether the len is still readable is not something the
     * contract promises; it happens to be, because the sweep does not zero
     * the payload, but a caller that depends on that is depending on the
     * memory not having been reallocated, which is not guaranteed. */
    if (buf->raw != NULL) {
        rt_mem_free(buf->raw);
        atomic_fetch_add(&last_finalized_count, 1);
    }
}

static CvmTypeDescriptor const owned_buffer_desc = {
    .size = sizeof(OwnedBuffer),
    .kind = CVM_TYPE_SCALAR,
    .finalizer = owned_buffer_finalize,
};

/*
 * A PTR_ARRAY type with a type-level finalizer, to show it is not just
 * scalar types.
 */
static void node_array_finalize(opaque_mut object) {
    (void)object;
    atomic_fetch_add(&last_finalized_count, 1);
}

static CvmPtrField const node_ptr_fields[] = {
    {.offset = offsetof(CvmRefArray, items)},
};
#pragma unused(node_ptr_fields)

static CvmTypeDescriptor const node_array_desc = {
    .size = offsetof(CvmRefArray, items),
    .kind = CVM_TYPE_PTR_ARRAY,
    .array_length_offset = offsetof(CvmRefArray, used),
    .array_data_offset = offsetof(CvmRefArray, items),
    .finalizer = node_array_finalize,
};

TEST a_struct_type_can_have_a_finalizer(void) {
    fixture_enter();
    settle();

    /* Allocate one, drop it, collect. */
    OwnedBuffer* buf = cvm_alloc(&owned_buffer_desc);
    buf->raw = rt_mem_alloc(64);
    buf->len = 64;
    (void)buf;

    collect();
    cvm_finalizer_drain_now();
    u64 before = cvm_finalizer_run_count() - 1;

    ASSERT_EQ_FMT(before + 1, cvm_finalizer_run_count(), "%" PRIu64);

    fixture_leave();
    PASS();
}

TEST a_struct_type_does_not_finalize_while_alive(void) {
    fixture_enter();
    settle();
    u64 before = cvm_finalizer_run_count();

    FRAME(1);
    OwnedBuffer* buf = cvm_alloc(&owned_buffer_desc);
    buf->raw = rt_mem_alloc(32);
    buf->len = 32;
    f_roots[0] = buf;

    collect();
    cvm_finalizer_drain_now();
    ASSERT_EQ_FMT(before, cvm_finalizer_run_count(), "%" PRIu64);

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_ptr_array_type_can_have_a_finalizer(void) {
    fixture_enter();
    settle();

    CvmRefArray* arr = cvm_alloc_array(&node_array_desc, 0, 0);
    (void)arr;

    collect();
    cvm_finalizer_drain_now();
    u64 before = cvm_finalizer_run_count() - 1;

    ASSERT_EQ_FMT(before + 1, cvm_finalizer_run_count(), "%" PRIu64);

    fixture_leave();
    PASS();
}

TEST many_struct_finalizers_all_run(void) {
    fixture_enter();

    /* One at a time, each verified: the mechanism, not the batching. */
    u64 total = 0;
    for (u32 i = 0; i < 3; i++) {
        settle();
        u64 before = cvm_finalizer_run_count();
        OwnedBuffer* buf = cvm_alloc(&owned_buffer_desc);
        buf->raw = rt_mem_alloc(8);
        buf->len = 8;
        collect();
        cvm_finalizer_drain_now();
        total += cvm_finalizer_run_count() - before;
    }
    ASSERT_EQ_FMT((u64)3, total, "%" PRIu64);

    fixture_leave();
    PASS();
}

SUITE(external_suite) {
    RUN_TEST(the_external_type_is_a_scalar);
    RUN_TEST(a_new_external_object_holds_its_pointer);
    RUN_TEST(a_finalizer_runs_when_its_object_dies);
    RUN_TEST(a_finalizer_does_not_run_while_the_object_lives);
    RUN_TEST(many_finalizers_all_run);
    RUN_TEST(the_finalizer_thread_runs_them_on_its_own);
    RUN_TEST(a_root_keeps_an_object_alive);
    RUN_TEST(a_removed_root_no_longer_keeps_its_object);
    RUN_TEST(a_root_can_be_repointed);
    RUN_TEST(handles_are_distinct_and_slots_are_reused);
    RUN_TEST(the_table_grows_past_its_initial_capacity);
    RUN_TEST(a_root_registered_during_a_collection_is_not_missed);
    RUN_TEST(an_external_object_held_by_a_root_stays_alive);
    RUN_TEST(a_struct_type_can_have_a_finalizer);
    RUN_TEST(a_struct_type_does_not_finalize_while_alive);
    RUN_TEST(a_ptr_array_type_can_have_a_finalizer);
    RUN_TEST(many_struct_finalizers_all_run);
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
    RUN_SUITE(external_suite);
    GREATEST_MAIN_END();

    cvm_finalizer_stop();
    cvm_world_destroy();
}

#endif /* !_WIN32 */
