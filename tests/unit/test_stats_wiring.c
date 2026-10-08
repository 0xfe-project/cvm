/*
 * tests/unit/test_stats_wiring.c
 *
 * That a collection actually produces event rows.
 *
 * vm/stats.c has been tested as a component since it was written: the ring
 * buffer, the flush boundary, the CSV's shape, the null-safety of every
 * entry point. None of that tests the reason it exists. A recorder with no
 * producer passes every test you can write about a recorder, and for most of
 * this project's life that is exactly what it was — fifteen passing tests
 * and zero events, because nothing in the collector called it.
 *
 * So these tests read the file back. They ask which rows are present, not
 * how long each phase took, because timings depend on the machine and the
 * presence of a row does not.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <stdio.h>
#include <string.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "gc/alloc.h"
#include "gc/collect.h"
#include "gc/heap.h"
#include "gc/pool.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"
#include "stats.h"

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)32 * 1024 * 1024)

#define STATS_TMP "/tmp/cvm_stats_wiring.csv"

/* The one channel, opened for the whole run, as a program would. */
static RtChannel* channel = NULL;

/* The only shape the churn needs: a word, no pointers, so tracing one is a
 * single visit. */
static CvmTypeDescriptor const word_desc = {
    .size = sizeof(u64),
    .kind = CVM_TYPE_SCALAR,
};

static str path_str(char const* p) {
    return (str){(u8 const*)p, strlen(p)};
}

static CvmHeap* heap(void) {
    return &cvm_world()->heap;
}

/* Read the stats file. Returns the byte count, or -1 if it is not there. */
static long slurp(char* out, size_t cap) {
    FILE* f = fopen(STATS_TMP, "rb");
    if (f == NULL)
        return -1;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
    return (long)n;
}

/*
 * Whether the file contains a row for `event`, and if so the fields of the
 * first such row.
 *
 * Parsing rather than substring searching, because "mark" is a substring of
 * "markbatch" and of "nullifywaiting", and a test that confuses them would
 * pass for the wrong reason.
 */
static bool find_row(char const* csv, char const* event, i32* thread_id, u64* start_ns,
                     u64* duration_ns) {
    size_t elen = strlen(event);

    for (char const* p = csv; *p != '\0';) {
        char const* eol = strchr(p, '\n');
        size_t llen = (eol != NULL) ? (size_t)(eol - p) : strlen(p);

        if (llen > elen && strncmp(p, event, elen) == 0 && p[elen] == ',') {
            long id = 0, start = 0, dur = 0;
            if (sscanf(p + elen + 1, "%ld,%ld,%ld", &id, &start, &dur) == 3) {
                if (thread_id != NULL)
                    *thread_id = (i32)id;
                if (start_ns != NULL)
                    *start_ns = (u64)start;
                if (duration_ns != NULL)
                    *duration_ns = (u64)dur;
                return true;
            }
        }

        p += llen + ((eol != NULL) ? 1 : 0);
    }
    return false;
}

/* Allocate `n` short-lived objects; the first one stays reachable. */
static void allocate_churn(u32 n, void** keep) {
    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = NULL;

    for (u32 i = 0; i < n; i++) {
        u64* junk = cvm_alloc(&word_desc);
        if (junk != NULL) {
            *junk = i;
        }
        if (keep != NULL && i == 0) {
            f_roots[0] = junk;
        }
    }

    if (keep != NULL) {
        *keep = f_roots[0];
    }
    cvm_frame_pop(&f_frame);
}

/*
 * Flush what the current thread recorded and read the file back.
 *
 * Detaching is what flushes a thread's ring buffer into the channel, so it
 * comes first; flushing the channel is what puts the bytes where fopen can
 * find them.
 */
static long recorded(char* out, size_t cap) {
    cvm_thread_detach();
    rt_channel_flush(channel);
    return slurp(out, cap);
}

/* ------------------------------------------------------------------ */

/*
 * The four intervals the collector owns must appear.
 *
 * This is the point of the file. Each row's presence says a specific call in
 * collect.c ran; a non-zero duration says the clock moved, which is what
 * separates "recorded" from "recorded with a timestamp of zero".
 */
TEST a_collection_writes_its_phases(void) {
    (void)cvm_thread_attach();

    void* keep = NULL;
    allocate_churn(2000, &keep);
    cvm_gc_collect_and_finish(heap());

    static char csv[1 << 16];
    ASSERTm("the stats file was never written", recorded(csv, sizeof(csv)) > 0);

    i32 collector_id = -1;
    u64 collection_start = 0;
    u64 duration = 0;

    ASSERTm("no collection row: the collector records nothing",
            find_row(csv, "collection", &collector_id, &collection_start, &duration));
    ASSERTm("a collection cannot take no time at all", duration > 0);
    ASSERTm("the row should be stamped with a real clock value", collection_start > 0);

    /*
     * The phases inside it. A mark of zero duration is legitimate — with
     * little to trace there is nothing to spend time on — so these check
     * presence, and that a phase sits inside its own collection on one
     * thread, not a minimum duration.
     */
    i32 phase_thread = -1;
    u64 phase_start = 0;

    ASSERTm("no mark row", find_row(csv, "mark", &phase_thread, &phase_start, NULL));
    ASSERTm("marking happened before the collection started",
            phase_start >= collection_start);
    ASSERTm("a phase belongs to the thread that ran the collection",
            phase_thread == collector_id);

    ASSERTm("no nullify row",
            find_row(csv, "nullify", &phase_thread, &phase_start, NULL));
    ASSERTm("no sweep row", find_row(csv, "sweep", &phase_thread, &phase_start, NULL));

    PASS();
}

/*
 * Sweeping reports per-batch, because it has no whole-phase interval.
 *
 * The sweep is deferred: after the collector resumes the world, the thread
 * that needs memory next is the one that sweeps. So the sweep's cost can
 * only be reported a batch at a time, and if those rows are missing the
 * sweep is invisible in the output — which is the failure mode worth
 * catching here, since the `sweep` row is present and looks like coverage.
 */
TEST sweeping_reports_its_batches(void) {
    (void)cvm_thread_attach();

    void* keep = NULL;
    allocate_churn(4000, &keep);
    cvm_gc_collect_and_finish(heap());

    static char csv[1 << 16];
    ASSERTm("the stats file was never written", recorded(csv, sizeof(csv)) > 0);

    /*
     * Either kind counts as "the sweep reported itself". Which one appears
     * depends on whether the swept blocks lined up into whole runs, and
     * that is a property of the heap's shape rather than of the recording.
     */
    bool batched = find_row(csv, "sweepbatch", NULL, NULL, NULL) ||
                   find_row(csv, "coalescebatch", NULL, NULL, NULL);
    ASSERTm("the sweep ran but reported no unit of work", batched);

    PASS();
}

/*
 * Workers carry their own buffer.
 *
 * They are not registered threads, so they cannot reach a buffer through the
 * thread-local lookup; cvm_pool_init has to give each one explicitly. That
 * is the kind of thing that silently does nothing.
 *
 * Whether a worker recorded anything is not checkable, and the reason is
 * worth writing down because it says what the pool does. A collection wakes
 * every worker, but a worker that wakes to find the phase already over does
 * no work and reports none — the collector finishes marking on its own for a
 * heap this small, and the worker arrives late. So a `concmark` row appears
 * when a worker got there first, which is a timing property, not an
 * invariant. Asserting it would make this test flaky in a way that means
 * nothing.
 *
 * What is checkable is that a row which does appear belongs to a worker: a
 * concurrent mark stamped with the collector's own id would show parallel
 * marking that never happened, on a thread that was not there.
 *
 * Observed while writing this, and worth recording because it will matter to
 * the benchmark: across three collections with two workers, one worker did
 * all four units of work and the other recorded nothing. Waking N threads
 * with a counting semaphore does not guarantee N threads take part — the
 * first one back can consume a second post before the second finishes
 * waking. That is not incorrect (no work is lost, and no thread is missed
 * permanently), but "two GC threads" is not the same promise as "marking is
 * split across two threads", and a measurement that assumed the latter would
 * be reading its own label rather than the machine.
 */
TEST a_worker_has_a_buffer_of_its_own(void) {
    CvmGcPool* pool = heap()->pool;
    ASSERTm("the fixture did not start a worker pool", pool != NULL);
    ASSERTm("the pool started with no threads", pool->count > 0);

    for (u32 i = 0; i < pool->count; i++) {
        ASSERTm("a worker was given no event buffer", pool->threads[i].stats != NULL);
    }

    (void)cvm_thread_attach();
    void* keep = NULL;
    allocate_churn(4000, &keep);
    cvm_gc_collect_and_finish(heap());

    /*
     * Workers flush their own buffers before sleeping, so their rows are in
     * the file already. The test must not flush them: reading another thread's
     * ring is a data race on that thread's buffer index, and ThreadSanitizer
     * says so.
     */

    static char csv[1 << 16];
    ASSERTm("the stats file was never written", recorded(csv, sizeof(csv)) > 0);

    i32 collector_id = -1;
    ASSERT(find_row(csv, "collection", &collector_id, NULL, NULL));

    i32 worker_id = -1;
    if (find_row(csv, "concmark", &worker_id, NULL, NULL)) {
        ASSERTm("a concurrent mark is stamped with the collector's own id",
                worker_id != collector_id);
    }

    PASS();
}

/*
 * Asking for no statistics must not cost a buffer, and must not stop the
 * collector from collecting.
 *
 * The first half is the reason cvm_stats_open returns a pointer: 34 KiB per
 * thread is a price only a program that asked for stats should pay. The
 * second half is the reason every recording call accepts a possibly-NULL
 * buffer: a null check that is forgotten turns "no statistics" into "no
 * garbage collection".
 */
TEST without_a_channel_nothing_is_allocated_and_gc_still_runs(void) {
    u64 before = cvm_gc_counters_collections();

    cvm_stats_set_default_channel(NULL);

    (void)cvm_thread_attach();
    ASSERTm("a thread allocated an event buffer it did not ask for",
            cvm_stats_this_thread() == NULL);

    void* keep = NULL;
    allocate_churn(2000, &keep);
    cvm_gc_collect_and_finish(heap());
    cvm_thread_detach();

    cvm_stats_set_default_channel(channel);

    ASSERTm("a collection did not happen, or was not counted",
            cvm_gc_counters_collections() > before);
    ASSERTm("the object that should have survived did not", keep != NULL);

    PASS();
}

GREATEST_MAIN_DEFS();

static void stats_suite(void) {
    RUN_TEST(a_collection_writes_its_phases);
    RUN_TEST(sweeping_reports_its_batches);
    RUN_TEST(a_worker_has_a_buffer_of_its_own);
    RUN_TEST(without_a_channel_nothing_is_allocated_and_gc_still_runs);
}

int main(int argc, char** argv) {
    /*
     * The channel is set before the world and the pool come up, because a
     * buffer is opened when a thread registers. Setting it later would mean
     * the pool's workers record nothing — which is the ordering cvm_init
     * already gets right for the same reason.
     */
    channel = rt_channel_open_file(path_str(STATS_TMP));
    if (channel == NULL) {
        fprintf(stderr, "could not open %s\n", STATS_TMP);
        return 1;
    }
    cvm_stats_set_default_channel(channel);

    cvm_threads_init();
    cvm_safepoint_init();
    if (!cvm_world_init(TEST_MIN, TEST_MAX)) {
        fprintf(stderr, "could not bring up the heap\n");
        return 1;
    }
    if (!cvm_pool_init(heap(), 2)) {
        fprintf(stderr, "could not start the worker pool\n");
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(stats_suite);
    GREATEST_MAIN_END();

    cvm_world_destroy();
    rt_channel_close(channel);
}

#endif /* !_WIN32 */
