/*
 * tests/e2e/test_smoke.c
 *
 * A program written the way a frontend's generated code is written.
 *
 * This file is not aimed at any one subsystem — the unit tests do that. It
 * exercises the ABI end to end, in the shape a frontend would actually
 * emit, through the public headers only:
 *
 *   cvm/vm.h        setup and teardown
 *   cvm/gc.h        frames and allocation
 *   cvm/weak.h      weak references
 *   cvm/external.h  resources owned outside the heap
 *   cvm/roots.h     pointers held by foreign code
 *
 * It includes no internal header and never touches the heap directly. If
 * this compiles and runs, the interface a frontend is promised is real.
 *
 * The runtime is brought up once in main and taken down at the end, which
 * is the shape a program has. The tests share it.
 */

#include "vendor/greatest.h"

#include <stddef.h>
#include <stdio.h>

#include "base/types.h"
#include "cvm/external.h"
#include "cvm/gc.h"
#include "cvm/roots.h"
#include "cvm/vm.h"
#include "cvm/weak.h"

/* ------------------------------------------------------------------ */
/* the types a frontend would emit                                     */
/* ------------------------------------------------------------------ */

/*
 * A list cell. A frontend emits this descriptor from the type
 * declaration; here it is written by hand because there is no frontend.
 */
typedef struct Cell {
    opaque_mut next;
    i64 value;
    u64 canary;
} Cell;

static CvmPtrField const cell_ptr_fields[] = {
    {.offset = offsetof(Cell, next)},
};

static CvmTypeDescriptor const cell_type = {
    .size = sizeof(Cell),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 1,
    .num_embed_fields = 0,
    .ptr_fields = cell_ptr_fields,
    .embed_fields = NULL,
};

static CvmTypeDescriptor const blob_type = {
    .size = 128,
    .kind = CVM_TYPE_SCALAR,
};

static u64 canary_for(void const* p) {
    return 0xa5a5a5a5u ^ (u64)(usize)p;
}

/* ------------------------------------------------------------------ */
/* "generated" functions                                               */
/* ------------------------------------------------------------------ */

/*
 * Build a list of `n` cells and return it.
 *
 * This is the shape a generated function has: push a frame sized to the
 * live pointers, keep the slots up to date as it goes, pop on every exit.
 *
 * Note the ordering in the loop. The new cell goes into its slot *before*
 * anything else can allocate, because the next cvm_alloc is what could
 * trigger a collection — and a pointer that exists only in a C local is
 * invisible to the collector.
 */
static opaque_mut build_list(i64 n) {
    CVM_FRAME_DECL(f, 2);
    cvm_frame_push(&f_frame, f_roots, 2);

    f_roots[0] = NULL; /* head */
    f_roots[1] = NULL; /* the cell being built */

    for (i64 i = 0; i < n; i++) {
        f_roots[1] = cvm_alloc(&cell_type);

        Cell* cell = f_roots[1];
        cell->next = f_roots[0];
        cell->value = i;
        cell->canary = canary_for(cell);

        f_roots[0] = f_roots[1];
        f_roots[1] = NULL;
    }

    opaque_mut head = f_roots[0];
    cvm_frame_pop(&f_frame);
    return head;
}

/* Walk the list, checking every cell's payload and the length. */
static bool list_is_intact(opaque_mut head, i64 expected_length) {
    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = head;

    i64 seen = 0;
    while (f_roots[0] != NULL) {
        Cell* cell = f_roots[0];
        if (cell->canary != canary_for(cell)) {
            cvm_frame_pop(&f_frame);
            return false;
        }
        /* The list is built backwards, so the values descend. */
        if (cell->value != expected_length - 1 - seen) {
            cvm_frame_pop(&f_frame);
            return false;
        }
        f_roots[0] = cell->next;
        seen++;
    }

    cvm_frame_pop(&f_frame);
    return seen == expected_length;
}

/* Allocate and drop a lot, the way a program's steady state does. */
static void churn(i64 rounds) {
    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = NULL;

    for (i64 i = 0; i < rounds; i++) {
        f_roots[0] = cvm_alloc(&blob_type);
        f_roots[0] = NULL; /* dropped immediately */
    }

    cvm_frame_pop(&f_frame);
}

/* ------------------------------------------------------------------ */
/* the program                                                         */
/* ------------------------------------------------------------------ */

TEST init_is_idempotent(void) {
    /* main already brought it up; asking again must be harmless. */
    ASSERT(cvm_init(NULL));

    CvmVmStats stats = cvm_vm_stats();
    ASSERT(stats.blocks_total > 0);
    ASSERT(stats.heap_size > 0);
    ASSERT(stats.threads_registered >= 1);
    PASS();
}

TEST a_built_structure_survives_collections(void) {
    CvmVmStats start = cvm_vm_stats();

    opaque_mut list = build_list(200);
    ASSERT(list_is_intact(list, 200));

    /* Keep it alive across collections: nothing in the C stack points at
     * it once build_list has returned. */
    CvmRootHandle root = cvm_root_add(list);
    ASSERT(root != CVM_ROOT_HANDLE_NONE);

    for (int round = 0; round < 5; round++) {
        churn(2000);
        cvm_collect();
        ASSERT(list_is_intact(list, 200));
    }

    CvmVmStats after = cvm_vm_stats();
    ASSERT(after.collections > start.collections);

    /*
     * The churn allocated far more than the list holds, so the heap should
     * now be mostly free. Asserting on the free block count rather than on
     * the heap size says what actually matters — the memory came back —
     * and does not depend on whether the growth heuristic happened to fire.
     */
    ASSERT(after.blocks_free > after.blocks_total / 2);

    cvm_root_remove(root);
    PASS();
}

TEST garbage_is_reclaimed(void) {
    churn(5000);
    cvm_collect();

    CvmVmStats stats = cvm_vm_stats();
    /*
     * The heap holds several megabytes and each blob is 128 bytes, so a
     * heap that reclaimed nothing would report tens of thousands of live
     * objects. A settled one is far below that.
     */
    ASSERT(stats.objects_live < 1000);
    PASS();
}

TEST a_weak_reference_observes_the_collector(void) {
    CVM_FRAME_DECL(f, 2);
    cvm_frame_push(&f_frame, f_roots, 2);
    f_roots[0] = NULL; /* the weak reference */
    f_roots[1] = NULL; /* nothing strongly reaches the target */

    void* target = cvm_alloc(&blob_type);
    f_roots[0] = cvm_weak_ref_new(target);

    cvm_collect();

    /* The collector says so by clearing the reference. */
    ASSERT(cvm_weak_ref_target(f_roots[0]) == NULL);

    cvm_frame_pop(&f_frame);
    PASS();
}

/* ------------------------------------------------------------------ */
/* finalizers on a live runtime                                        */
/* ------------------------------------------------------------------ */

static u64 finalized = 0;

static void count_finalizer(opaque_mut c_ptr) {
    (void)c_ptr;
    finalized++;
}

TEST external_handles_are_released_by_the_finalizer_thread(void) {
    u64 before = cvm_vm_stats().finalizers_run;

    enum { N = 100 };
    for (int i = 0; i < N; i++) {
        (void)cvm_external_new(NULL, count_finalizer);
    }

    /*
     * The runtime's own thread should run these without being asked, but
     * collection is what makes them die, so this collects in a bounded
     * loop rather than assuming a single pass is enough.
     */
    for (int attempt = 0; attempt < 200; attempt++) {
        cvm_collect();
        if (cvm_vm_stats().finalizers_run >= before + N) {
            break;
        }
    }

    ASSERT(cvm_vm_stats().finalizers_run >= before + N);
    ASSERT_EQ_FMT(0ull, (unsigned long long)cvm_vm_stats().finalizers_dropped, "%llu");
    PASS();
}

TEST a_root_registered_by_foreign_code_keeps_an_object(void) {
    void* blob = cvm_alloc(&blob_type);
    CvmRootHandle handle = cvm_root_add(blob);

    churn(2000);
    cvm_collect();

    /* Still reachable, through nothing but the root table. */
    ASSERT(cvm_root_get(handle) == blob);

    cvm_root_remove(handle);
    PASS();
}

SUITE(smoke_suite) {
    RUN_TEST(init_is_idempotent);
    RUN_TEST(a_built_structure_survives_collections);
    RUN_TEST(garbage_is_reclaimed);
    RUN_TEST(a_weak_reference_observes_the_collector);
    RUN_TEST(external_handles_are_released_by_the_finalizer_thread);
    RUN_TEST(a_root_registered_by_foreign_code_keeps_an_object);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    CvmOptions options = {
        .min_heap_size = 4 * 1024 * 1024,
        .max_heap_size = 32 * 1024 * 1024,
        /*
         * Let the runtime size the pool. The smoke test is meant to run the
         * configuration a real program would, and a real program wants
         * workers.
         */
        .gc_workers = CVM_GC_WORKERS_DEFAULT,
        .finalizer_thread = true,
        .stats_channel = NULL,
        .log_level = {NULL, 0},
    };

    if (!cvm_init(&options)) {
        fprintf(stderr, "the runtime could not start\n");
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(smoke_suite);
    GREATEST_MAIN_END();

    cvm_shutdown();
}
