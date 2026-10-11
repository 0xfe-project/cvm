/*
 * vm/stats.c
 *
 * The out-of-line half of the event recorder: names, the CSV writer, and
 * the process-wide counters.
 */

#include "stats.h"

#include "context.h"

#include "base/format.h"
#include "base/types.h"
#include "log.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* event names                                                         */
/* ------------------------------------------------------------------ */

/*
 * Names are short and lower-case with no separators, because they are the
 * first CSV column and are meant to be greppable: `grep concsweep` should
 * find exactly one kind.
 */
static str const event_names[CVM_EVENT_TYPE_COUNT] = {
    [CVM_EVENT_MARK] = {.ptr = (u8 const*)"mark", .len = 4},
    [CVM_EVENT_NULLIFY] = {.ptr = (u8 const*)"nullify", .len = 7},
    [CVM_EVENT_SWEEP] = {.ptr = (u8 const*)"sweep", .len = 5},
    [CVM_EVENT_CONCURRENT_MARK] = {.ptr = (u8 const*)"concmark", .len = 8},
    [CVM_EVENT_CONCURRENT_NULLIFY] = {.ptr = (u8 const*)"concnullify", .len = 11},
    [CVM_EVENT_CONCURRENT_SWEEP] = {.ptr = (u8 const*)"concsweep", .len = 9},
    [CVM_EVENT_COLLECTION] = {.ptr = (u8 const*)"collection", .len = 10},
    [CVM_EVENT_MARK_BATCH] = {.ptr = (u8 const*)"markbatch", .len = 9},
    [CVM_EVENT_SWEEP_BATCH] = {.ptr = (u8 const*)"sweepbatch", .len = 10},
    [CVM_EVENT_COALESCE_BATCH] = {.ptr = (u8 const*)"coalescebatch", .len = 13},
    [CVM_EVENT_MARK_WAITING] = {.ptr = (u8 const*)"markwaiting", .len = 11},
    [CVM_EVENT_NULLIFY_WAITING] = {.ptr = (u8 const*)"nullifywaiting", .len = 14},
    [CVM_EVENT_SYNC] = {.ptr = (u8 const*)"sync", .len = 4},
};

str cvm_event_type_name(CvmEventType type) {
    if ((u32)type >= (u32)CVM_EVENT_TYPE_COUNT) {
        /* An out-of-range type is a bug in the caller, but a stats
         * function is the wrong place to abort: the output would be lost,
         * which is exactly what someone debugging needs. Name it and
         * carry on. */
        return STR_LITERAL("unknown");
    }
    return event_names[type];
}

/* ------------------------------------------------------------------ */
/* CSV                                                                 */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* per-thread buffers                                                  */
/* ------------------------------------------------------------------ */

static RtChannel* default_channel = NULL;
static i8 next_thread_id = 0;

void cvm_stats_set_default_channel(RtChannel* channel) {
    default_channel = channel;
}

CvmStats* cvm_stats_open(void) {
    if (default_channel == NULL) {
        return NULL;
    }

    CvmStats* stats = rt_mem_alloc(sizeof(CvmStats));
    if (stats == NULL) {
        return NULL;
    }

    /*
     * Ids are labels, not identifiers: 0..126 for threads, -1 reserved for
     * the mutator role. Wrapping after 127 threads means two threads can
     * share a label in a long run, which the CSV's other columns
     * disambiguate by time.
     */
    i8 id = (next_thread_id >= 127) ? 0 : next_thread_id++;
    cvm_stats_init(stats, default_channel, id);
    return stats;
}

void cvm_stats_close(CvmStats* stats) {
    if (stats == NULL) {
        return;
    }
    cvm_stats_shutdown(stats);
    rt_mem_free(stats);
}

CvmStats* cvm_stats_this_thread(void) {
    CvmThreadNode* node = cvm_thread_node();
    return (node != NULL) ? node->stats : NULL;
}

static void write_header(CvmStats* stats) {
    static const u8 header[] = "event_type,gc_thread,start_ns,duration_ns\n";
    rt_channel_write(stats->channel, (span_const){header, sizeof(header) - 1});
}

void cvm_stats_init(CvmStats* stats, RtChannel* channel, i8 thread_id) {
    stats->buffered = 0;
    stats->written = 0;
    stats->thread_id = thread_id;
    stats->channel = channel;
    write_header(stats);
}

void cvm_stats_flush(CvmStats* stats) {
    if (stats == NULL || stats->buffered == 0) {
        return;
    }

    u8 line[128];

    for (u32 i = 0; i < stats->buffered; i++) {
        CvmFmt f;
        fmt_init(&f, line, sizeof(line));

        fmt_str(&f, cvm_event_type_name((CvmEventType)stats->type[i]));
        fmt_lit(&f, ",");
        fmt_i32(&f, (i32)stats->thread_id);
        fmt_lit(&f, ",");
        fmt_u64(&f, stats->start_ns[i]);
        fmt_lit(&f, ",");
        fmt_u64(&f, stats->duration_ns[i]);
        fmt_lit(&f, "\n");

        rt_channel_write(stats->channel, (span_const){line, f.len});
    }

    stats->written += stats->buffered;
    stats->buffered = 0;
}

void cvm_stats_shutdown(CvmStats* stats) {
    if (stats == NULL) {
        return;
    }
    cvm_stats_flush(stats);
}

/* ------------------------------------------------------------------ */
/* timestamps                                                          */
/* ------------------------------------------------------------------ */

u64 cvm_stats_now(CvmStats const* stats) {
    /* The clock read is skipped entirely for a thread without stats; the
     * zero it returns is what cvm_stats_record uses to skip recording. */
    return (stats != NULL) ? rt_time_nanos() : 0;
}

/* ------------------------------------------------------------------ */
/* process-wide counters                                               */
/* ------------------------------------------------------------------ */

static CvmGcCounters gc_counters = {.collections = 0, .collection_ns = 0};

void cvm_gc_counters_record_collection(u64 start_ns, u64 end_ns) {
    if (start_ns == 0) {
        return;
    }
    u64 duration = end_ns - start_ns;

    atomic_fetch_add_explicit(&gc_counters.collections, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&gc_counters.collection_ns, duration,
                              memory_order_relaxed);

    /*
     * Monotonic maximum, by compare-exchange rather than a plain store: two
     * collections can end at once, and the loser of that race must not
     * overwrite a longer pause with a shorter one. A store here would make
     * the reported maximum depend on which thread finished last, which is
     * exactly the kind of number that looks like a measurement and is not.
     */
    u64 seen = atomic_load_explicit(&gc_counters.pause_max_ns, memory_order_relaxed);
    while (duration > seen && !atomic_compare_exchange_weak_explicit(
                                  &gc_counters.pause_max_ns, &seen, duration,
                                  memory_order_relaxed, memory_order_relaxed)) {
    }
}

/*
 * Record one stop-the-world window's length, without counting a collection.
 *
 * Concurrent marking stops the world twice per collection, and the pause a
 * program feels is each window, not the span between the first stop and the
 * second -- that span is marking time, spent alongside the program. The
 * collection counter and the total keep their meaning; only the maximum is
 * maintained here.
 */
void cvm_gc_counters_record_pause(u64 start_ns, u64 end_ns) {
    if (start_ns == 0) {
        return;
    }
    u64 duration = end_ns - start_ns;
    u64 seen = atomic_load_explicit(&gc_counters.pause_max_ns, memory_order_relaxed);
    while (duration > seen && !atomic_compare_exchange_weak_explicit(
                                  &gc_counters.pause_max_ns, &seen, duration,
                                  memory_order_relaxed, memory_order_relaxed)) {
    }
}

/* Count a collection and add its span to the total, without touching the
 * pause maximum -- for a collection whose stop-the-world windows are recorded
 * separately, one per window. */
void cvm_gc_counters_record_span(u64 start_ns, u64 end_ns) {
    if (start_ns == 0) {
        return;
    }
    atomic_fetch_add_explicit(&gc_counters.collections, 1u, memory_order_relaxed);
    atomic_fetch_add_explicit(&gc_counters.collection_ns, end_ns - start_ns,
                              memory_order_relaxed);
}

u64 cvm_gc_counters_collections(void) {
    return atomic_load_explicit(&gc_counters.collections, memory_order_relaxed);
}

u64 cvm_gc_counters_collection_ns(void) {
    return atomic_load_explicit(&gc_counters.collection_ns, memory_order_relaxed);
}

u64 cvm_gc_counters_pause_max_ns(void) {
    return atomic_load_explicit(&gc_counters.pause_max_ns, memory_order_relaxed);
}
