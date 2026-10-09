/*
 * tests/support/oracle.h
 *
 * The liveness oracles from TEST.md, factored out.
 *
 * They were written inline in test_collect and test_smoke, which was right
 * while there were two suites that wanted them. The stress suite is the
 * moment TEST.md predicted would want them shared, and this is it.
 *
 * What is here, and what it is for:
 *
 *   canary payloads   an address-derived value written into an object and
 *                     checked after a collection. This is the instrument
 *                     for TP (a live object survived intact) and for FN (a
 *                     live object was collected and its memory reused).
 *
 *   is_live           whether the collector's own metadata says an address
 *                     holds a live object. This is the instrument for TN
 *                     (the collector reclaimed it) and it is safe to ask
 *                     about a reclaimed object, because the metadata table
 *                     is a separate region from the object.
 *
 *   collect           a collection followed by driving the sweep to
 *                     completion, so that a caller inspects a settled heap.
 *
 * Why not the weak reference, which TEST.md calls the primary instrument
 * for TN: a weak reference on an *unreachable* object is never reached by
 * the marker, so it is never queued for nullifying, and its slot keeps
 * whatever was there until the memory is reused. Reading it would be
 * reading freed memory and calling the stale value a liveness decision.
 * The weak probe is sound only when the weak reference itself is reachable,
 * and the stress suite mostly wants the other question.
 */

#ifndef CVM_TEST_ORACLE_H
#define CVM_TEST_ORACLE_H

/*
 * Every helper here is a `static` function in a header, so a test that
 * includes the header and does not call one of them would be compiled with
 * an unused function and fail on -Werror. The attribute says outright that
 * the set is offered rather than required, which is what a shared header is.
 */
#if defined(__GNUC__) || defined(__clang__)
#define ORACLE_UNUSED __attribute__((unused))
#else
#define ORACLE_UNUSED
#endif

#include <stddef.h>
#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "cvm/vm.h"
#include "gc/bytemap.h"
#include "gc/collect.h"
#include "gc/heap.h"
#include "gc/meta.h"
#include "gc/pool.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

/* ------------------------------------------------------------------ */
/* bring-up                                                            */
/* ------------------------------------------------------------------ */

/*
 * Bring the runtime up for a test binary.
 *
 * `workers` is how many GC worker threads to start, or zero for none. A
 * stress test wants to be run both ways — workers change who does the work
 * and therefore which interleavings happen — so it is a parameter rather
 * than a constant.
 */
static ORACLE_UNUSED bool oracle_bring_up(usize min_heap, usize max_heap, u32 workers) {
    cvm_threads_init();
    cvm_safepoint_init();

    if (!cvm_world_init(min_heap, max_heap)) {
        fprintf(stderr, "oracle: could not reserve the heap\n");
        return false;
    }

    if (workers > 0 && !cvm_pool_init(&cvm_world()->heap, workers)) {
        fprintf(stderr, "oracle: could not start the worker pool\n");
        return false;
    }

    if (!cvm_thread_attach()) {
        fprintf(stderr, "oracle: could not register the calling thread\n");
        return false;
    }
    return true;
}

static ORACLE_UNUSED CvmHeap* oracle_heap(void) {
    return &cvm_world()->heap;
}

/* ------------------------------------------------------------------ */
/* the canary oracle                                                   */
/* ------------------------------------------------------------------ */

/*
 * A value derived from the object's address, so that a stale-but-plausible
 * value is distinguishable from the right one. Two objects cannot carry
 * each other's canary by accident.
 */
static ORACLE_UNUSED u64 oracle_canary(void const* p) {
    u64 x = (u64)(usize)p;
    /* A cheap mix, so that neighbouring addresses do not produce
     * neighbouring canaries. */
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    return x;
}

/*
 * Write a canary into the first word of a payload, and check it.
 *
 * Scope, because this pair is narrower than it reads and the mistake it
 * invites was made twice while writing these tests:
 *
 * The canary lives in the payload's *first word*. That is fine for a scalar
 * payload and wrong for any type whose first field is a pointer — for a
 * two-pointer node the stamp lands on `left`, and the check then reads
 * `left` and asks whether it equals the canary. It will not, for any reason
 * or none, and the test reports the collector as corrupting an object it
 * never touched.
 *
 * A type with pointer fields must carry its canary in a field of its own:
 * store `oracle_canary(address)` there when building, and compare against
 * it when checking. `oracle_canary` is the mixing function; this pair is
 * only the shorthand for a payload that is a plain word.
 */
static ORACLE_UNUSED void oracle_stamp(void* payload) {
    *(u64*)payload = oracle_canary(payload);
}

static ORACLE_UNUSED bool oracle_canary_intact(void const* payload) {
    return *(u64 const*)payload == oracle_canary(payload);
}

/*
 * The canary for a type that keeps it in a named field, which is the safe
 * form: the value is derived from the object's address, so it identifies
 * that one object and cannot be satisfied by a neighbour's contents.
 */
static ORACLE_UNUSED u64 oracle_field_canary(void const* object) {
    return oracle_canary(object);
}

static ORACLE_UNUSED bool oracle_field_canary_ok(void const* object, u64 stored) {
    return stored == oracle_canary(object);
}

/* ------------------------------------------------------------------ */
/* the metadata oracle                                                 */
/* ------------------------------------------------------------------ */

/*
 * Whether the collector considers this address to hold a live object.
 *
 * Both slot states count: between marking and sweeping a live object reads
 * `marked`, and once the sweep has run it reads `allocated`, because that
 * is what sweeping turns a mark into. Asking only about `marked` would say
 * every object died the moment a collection finished.
 *
 * The question is asked about the object's *start*, not its payload, since
 * that is what every piece of collector bookkeeping indexes.
 */
static ORACLE_UNUSED bool oracle_is_live(void const* payload) {
    if (payload == NULL) {
        return false;
    }
    ObjectMeta const* meta =
        bytemap_get_const(oracle_heap()->bytemap, (u8 const*)cvm_header_of(payload));
    return object_meta_is_allocated(meta) || object_meta_is_marked(meta);
}

/* ------------------------------------------------------------------ */
/* collecting                                                          */
/* ------------------------------------------------------------------ */

/*
 * Collect and drive the sweep to completion.
 *
 * The production path deliberately leaves the sweep running alongside the
 * program; this is the settled shape, which is what a test inspecting the
 * heap wants.
 */
static ORACLE_UNUSED void oracle_collect(void) {
    cvm_gc_collect_and_finish(oracle_heap());
}

/* ------------------------------------------------------------------ */
/* frames                                                              */
/* ------------------------------------------------------------------ */

/*
 * Abandon whatever frames the calling thread has.
 *
 * Every test in this suite pushes a frame that lives for the whole test
 * body, and an assertion that fires returns from the test function without
 * popping it. What is left behind is a depth that counts frames belonging to
 * an activation that has ended, so the next test's collection walks slots
 * whose storage is gone.
 *
 * Note that this is now a count rather than a pointer, which is why the
 * comment is shorter than it used to be. Abandoning frames cannot leave a
 * dangling pointer to follow -- the frames above the depth are simply not
 * reached -- so the failure this guards against is a stale depth, not a wild
 * read. Calling it at the top of each test still keeps one failure from
 * becoming two.
 */
static ORACLE_UNUSED void oracle_reset_frames(void) {
    CvmVMContext* ctx = cvm_thread_ctx();
    if (ctx != NULL) {
        ctx->frame_depth = 0u;
    }
}

/*
 * A frame with `n` slots, all NULL, pushed and ready.
 *
 * The slots must be updated by hand as locals come and go; that is the
 * protocol generated code follows and there is no way to make a test
 * honest without following it too.
 */
#define ORACLE_FRAME(n)                                                                \
    CVM_FRAME_DECL(oracle_f, n);                                                       \
    cvm_frame_push(&oracle_f_frame, oracle_f_roots, n);                                \
    for (u32 oracle_i = 0; oracle_i < (n); oracle_i++) {                               \
        oracle_f_roots[oracle_i] = NULL;                                               \
    }

#define ORACLE_FRAME_END() cvm_frame_pop(&oracle_f_frame)

/* ------------------------------------------------------------------ */
/* a deterministic random source                                       */
/* ------------------------------------------------------------------ */

/*
 * xorshift64*, written out rather than using libc rand.
 *
 * The seed is fixed and printed by a failing test, so a failure is
 * reproducible: a stress test whose input changes between runs is a stress
 * test whose failures cannot be chased.
 */
typedef struct OracleRandom {
    u64 state;
} OracleRandom;

static ORACLE_UNUSED OracleRandom oracle_random(u64 seed) {
    /* Zero is a fixed point of xorshift, so it must not be the seed. */
    return (OracleRandom){.state = (seed != 0) ? seed : 0x9e3779b97f4a7c15ull};
}

static ORACLE_UNUSED u64 oracle_random_next(OracleRandom* r) {
    u64 x = r->state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->state = x;
    return x * 0x2545f4914f6cdd1dull;
}

/* [0, bound). */
static ORACLE_UNUSED u32 oracle_random_below(OracleRandom* r, u32 bound) {
    return (bound == 0) ? 0 : (u32)(oracle_random_next(r) % bound);
}

#endif /* CVM_TEST_ORACLE_H */
