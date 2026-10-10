/*
 * tests/unit/test_stats.c
 *
 * The event recorder. What matters here: recording is a buffer append and
 * nothing else, the buffer flushes exactly at its boundary, and the CSV
 * that comes out is complete and parseable.
 *
 * The process-wide counters are global, so those tests work in deltas
 * rather than absolutes.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/types.h"
#include "rt.h"
#include "stats.h"

#define STATS_TMP "/tmp/cvm_stats_test.csv"

static str path_str(const char* p) {
    return (str){(u8 const*)p, strlen(p)};
}

/* Read the whole file. Returns bytes read, or -1. */
static long slurp(char* out, size_t cap) {
    FILE* f = fopen(STATS_TMP, "rb");
    if (f == NULL)
        return -1;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
    return (long)n;
}

static size_t count_lines(const char* s) {
    size_t n = 0;
    for (; *s; s++) {
        if (*s == '\n')
            n++;
    }
    return n;
}

/* Run `body` against a fresh stats file and hand back its contents. */
static long with_stats(i8 thread_id, void (*body)(CvmStats*), char* out, size_t cap) {
    RtChannel* ch = rt_channel_open_file(path_str(STATS_TMP));
    if (ch == NULL)
        return -1;

    static CvmStats stats;
    cvm_stats_init(&stats, ch, thread_id);
    body(&stats);
    cvm_stats_shutdown(&stats);
    rt_channel_close(ch);

    return slurp(out, cap);
}

/* ------------------------------------------------------------------ */
/* bodies                                                              */
/* ------------------------------------------------------------------ */

static void body_nothing(CvmStats* s) {
    (void)s;
}

static void body_two_events(CvmStats* s) {
    cvm_stats_record(s, CVM_EVENT_MARK, 1000, 1500);
    cvm_stats_record(s, CVM_EVENT_SWEEP, 2000, 2001);
}

static void body_one_event_no_flush(CvmStats* s) {
    cvm_stats_record(s, CVM_EVENT_MARK, 1000, 1500);
    /* no shutdown flush: with_stats calls shutdown, so this body is only
     * used where the tail is expected to be written */
}

static void body_null_stats(CvmStats* s) {
    (void)s;
    /* A thread that does not record must survive every entry point. */
    cvm_stats_record(NULL, CVM_EVENT_MARK, 1, 2);
    cvm_stats_flush(NULL);
    cvm_stats_shutdown(NULL);
    cvm_stats_record(NULL, CVM_EVENT_MARK, 0, 0);
}

static void body_zero_timestamp(CvmStats* s) {
    /* start_ns == 0 means "no timestamp taken"; the event is dropped */
    cvm_stats_record(s, CVM_EVENT_MARK, 0, 0);
    /* and a real one right after must still land */
    cvm_stats_record(s, CVM_EVENT_SWEEP, 500, 600);
}

/* ------------------------------------------------------------------ */
/* tests                                                               */
/* ------------------------------------------------------------------ */

TEST init_writes_the_header_immediately(void) {
    char out[4096];
    long n = with_stats(CVM_STATS_MUTATOR_THREAD_ID, body_nothing, out, sizeof(out));

    ASSERT(n > 0);
    ASSERT_STR_EQ("event_type,gc_thread,start_ns,duration_ns\n", out);
    PASS();
}

TEST record_only_buffers(void) {
    RtChannel* ch = rt_channel_open_file(path_str(STATS_TMP));
    ASSERT(ch != NULL);

    static CvmStats stats;
    cvm_stats_init(&stats, ch, CVM_STATS_MUTATOR_THREAD_ID);

    cvm_stats_record(&stats, CVM_EVENT_MARK, 1000, 1500);
    cvm_stats_record(&stats, CVM_EVENT_SWEEP, 2000, 2500);

    /* nothing written yet beyond the header */
    ASSERT_EQ_FMT(2u, stats.buffered, "%u");
    ASSERT_EQ_FMT((u64)0, (u64)stats.written, "%" PRIu64);

    char out[4096];
    slurp(out, sizeof(out));
    ASSERT_EQ_FMT(1u, (u32)count_lines(out), "%u"); /* header only */

    cvm_stats_shutdown(&stats);
    rt_channel_close(ch);
    PASS();
}

TEST flush_writes_the_buffered_events(void) {
    char out[4096];
    long n = with_stats(CVM_STATS_MUTATOR_THREAD_ID, body_two_events, out, sizeof(out));
    ASSERT(n > 0);

    /* header plus two events */
    ASSERT_EQ_FMT(3u, (u32)count_lines(out), "%u");
    ASSERT(strstr(out, "mark,-1,1000,500") != NULL);
    ASSERT(strstr(out, "sweep,-1,2000,1") != NULL);
    PASS();
}

TEST duration_is_end_minus_start(void) {
    char out[4096];
    with_stats(CVM_STATS_MUTATOR_THREAD_ID, body_two_events, out, sizeof(out));

    /* first event: 1500 - 1000 = 500 */
    ASSERT(strstr(out, ",1000,500\n") != NULL);
    /* second: 2001 - 2000 = 1 */
    ASSERT(strstr(out, ",2000,1\n") != NULL);
    PASS();
}

TEST thread_id_is_recorded(void) {
    char out[4096];
    with_stats(3, body_two_events, out, sizeof(out));

    ASSERT(strstr(out, "mark,3,") != NULL);
    ASSERT(strstr(out, "sweep,3,") != NULL);
    PASS();
}

TEST null_stats_is_harmless_everywhere(void) {
    char out[4096];
    long n = with_stats(CVM_STATS_MUTATOR_THREAD_ID, body_null_stats, out, sizeof(out));

    /* only the header: nothing was recorded */
    ASSERT(n > 0);
    ASSERT_EQ_FMT(1u, (u32)count_lines(out), "%u");
    PASS();
}

TEST zero_timestamp_is_dropped_not_recorded(void) {
    char out[4096];
    with_stats(CVM_STATS_MUTATOR_THREAD_ID, body_zero_timestamp, out, sizeof(out));

    /* the dropped event leaves no trace */
    ASSERT(strstr(out, "mark") == NULL);
    /* the real one survives */
    ASSERT(strstr(out, "sweep,-1,500,100") != NULL);
    ASSERT_EQ_FMT(2u, (u32)count_lines(out), "%u");
    PASS();
}

TEST buffer_flushes_exactly_at_capacity(void) {
    RtChannel* ch = rt_channel_open_file(path_str(STATS_TMP));
    ASSERT(ch != NULL);

    static CvmStats stats;
    cvm_stats_init(&stats, ch, CVM_STATS_MUTATOR_THREAD_ID);

    /* fill to exactly capacity: still buffered, no flush yet */
    for (u32 i = 0; i < CVM_STATS_CAPACITY; i++) {
        cvm_stats_record(&stats, CVM_EVENT_MARK_BATCH, 1, 2);
    }
    ASSERT_EQ_FMT(CVM_STATS_CAPACITY, stats.buffered, "%u");
    ASSERT_EQ_FMT((u64)0, (u64)stats.written, "%" PRIu64);

    /* one more event forces the flush, then buffers itself */
    cvm_stats_record(&stats, CVM_EVENT_SYNC, 1, 2);
    ASSERT_EQ_FMT(1u, stats.buffered, "%u");
    ASSERT_EQ_FMT((u64)CVM_STATS_CAPACITY, (u64)stats.written, "%" PRIu64);

    cvm_stats_shutdown(&stats);
    rt_channel_close(ch);
    PASS();
}

TEST repeated_flushes_account_for_every_event(void) {
    RtChannel* ch = rt_channel_open_file(path_str(STATS_TMP));
    ASSERT(ch != NULL);

    static CvmStats stats;
    cvm_stats_init(&stats, ch, CVM_STATS_MUTATOR_THREAD_ID);

    const u32 total = CVM_STATS_CAPACITY * 2 + 5;
    for (u32 i = 0; i < total; i++) {
        cvm_stats_record(&stats, CVM_EVENT_MARK, 1, 2);
    }
    cvm_stats_shutdown(&stats);

    ASSERT_EQ_FMT((u64)total, (u64)stats.written, "%" PRIu64);

    rt_channel_close(ch);

    char out[64 * 1024];
    long n = slurp(out, sizeof(out));
    ASSERT(n > 0);
    /* header plus every event */
    ASSERT_EQ_FMT(total + 1, (u32)count_lines(out), "%u");
    PASS();
}

TEST shutdown_flushes_the_tail(void) {
    char out[4096];
    with_stats(CVM_STATS_MUTATOR_THREAD_ID, body_one_event_no_flush, out, sizeof(out));

    /* the single event was still buffered when shutdown ran */
    ASSERT(strstr(out, "mark,-1,1000,500") != NULL);
    ASSERT_EQ_FMT(2u, (u32)count_lines(out), "%u");
    PASS();
}

TEST every_event_type_has_a_distinct_name(void) {
    for (u32 i = 0; i < (u32)CVM_EVENT_TYPE_COUNT; i++) {
        str a = cvm_event_type_name((CvmEventType)i);
        ASSERT(a.len > 0);
        ASSERT(a.ptr != NULL);

        for (u32 j = i + 1; j < (u32)CVM_EVENT_TYPE_COUNT; j++) {
            str b = cvm_event_type_name((CvmEventType)j);
            bool same = (a.len == b.len) && (memcmp(a.ptr, b.ptr, a.len) == 0);
            ASSERT_FALSE(same);
        }
    }
    PASS();
}

TEST out_of_range_event_type_is_named_not_fatal(void) {
    str s = cvm_event_type_name((CvmEventType)CVM_EVENT_TYPE_COUNT);
    ASSERT(s.len > 0);
    ASSERT_EQ_FMT(7u, (u32)s.len, "%u");
    ASSERT(memcmp(s.ptr, "unknown", 7) == 0);
    PASS();
}

TEST now_is_zero_without_stats_and_real_with(void) {
    ASSERT_EQ_FMT((u64)0, (u64)cvm_stats_now(NULL), "%" PRIu64);

    static CvmStats stats;
    /* now() only reads the pointer, so an uninitialised buffer is fine
     * for this one check */
    u64 t = cvm_stats_now(&stats);
    ASSERT(t > 0);
    PASS();
}

TEST collection_counters_accumulate(void) {
    u64 c0 = cvm_gc_counters_collections();
    u64 d0 = cvm_gc_counters_collection_ns();

    cvm_gc_counters_record_collection(1000, 1500);
    cvm_gc_counters_record_collection(2000, 2200);

    ASSERT_EQ_FMT((u64)(c0 + 2), (u64)cvm_gc_counters_collections(), "%" PRIu64);
    ASSERT_EQ_FMT((u64)(d0 + 700), (u64)cvm_gc_counters_collection_ns(), "%" PRIu64);
    PASS();
}

TEST collection_counter_ignores_a_missing_start(void) {
    u64 c0 = cvm_gc_counters_collections();
    u64 d0 = cvm_gc_counters_collection_ns();

    cvm_gc_counters_record_collection(0, 5000);

    ASSERT_EQ_FMT((u64)c0, (u64)cvm_gc_counters_collections(), "%" PRIu64);
    ASSERT_EQ_FMT((u64)d0, (u64)cvm_gc_counters_collection_ns(), "%" PRIu64);
    PASS();
}

SUITE(stats_suite) {
    RUN_TEST(init_writes_the_header_immediately);
    RUN_TEST(record_only_buffers);
    RUN_TEST(flush_writes_the_buffered_events);
    RUN_TEST(duration_is_end_minus_start);
    RUN_TEST(thread_id_is_recorded);
    RUN_TEST(null_stats_is_harmless_everywhere);
    RUN_TEST(zero_timestamp_is_dropped_not_recorded);
    RUN_TEST(buffer_flushes_exactly_at_capacity);
    RUN_TEST(repeated_flushes_account_for_every_event);
    RUN_TEST(shutdown_flushes_the_tail);
    RUN_TEST(every_event_type_has_a_distinct_name);
    RUN_TEST(out_of_range_event_type_is_named_not_fatal);
    RUN_TEST(now_is_zero_without_stats_and_real_with);
    RUN_TEST(collection_counters_accumulate);
    RUN_TEST(collection_counter_ignores_a_missing_start);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(stats_suite);
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
