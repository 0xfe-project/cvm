/*
 * vm/stats.h
 *
 * Per-thread event recording, ported from refs/commix/Stats.h.
 *
 * The collector records a timestamped event per phase it performs, into a
 * fixed-size buffer owned by the thread doing the work. The buffer is
 * written out when it fills and again at shutdown, as CSV. Nothing is
 * written during the normal case, so recording is a few stores and a
 * compare.
 *
 * Why per thread rather than one shared log: the alternative is either a
 * lock on the hot path or interleaved lines that cannot be attributed to a
 * thread. One buffer per thread costs a fixed allocation and keeps the
 * recording path lock-free by construction.
 *
 * Why not compile it out: a collector's behaviour is only explicable with
 * this data, and a stats build is not the build you ship, so the numbers
 * you need would be the ones you do not have. The cost is bounded and paid
 * per event, not per allocation. Threads that should not record (a
 * finalizer thread, say) pass a NULL stats pointer, and every entry point
 * checks for it.
 *
 * Freestanding: output goes through an RtChannel, and numbers are
 * formatted with base/format.h. No FILE*, no printf.
 */

#ifndef CVM_STATS_H
#define CVM_STATS_H

#include <stdatomic.h>

#include "base/format.h"
#include "base/types.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* event kinds                                                         */
/* ------------------------------------------------------------------ */

/*
 * The order is part of the output format: the numeric value is what lands
 * in the CSV's first column when a name lookup is not used. Append new
 * kinds at the end rather than reordering.
 */
typedef enum CvmEventType {
    /* Phases on the collecting thread. */
    CVM_EVENT_MARK = 0,
    CVM_EVENT_NULLIFY,
    CVM_EVENT_SWEEP,

    /* The same phases on a worker thread. */
    CVM_EVENT_CONCURRENT_MARK,
    CVM_EVENT_CONCURRENT_NULLIFY,
    CVM_EVENT_CONCURRENT_SWEEP,

    /* Whole collection, from the trigger to the end of sweeping. */
    CVM_EVENT_COLLECTION,

    /* Units of work inside a phase. */
    CVM_EVENT_MARK_BATCH,
    CVM_EVENT_SWEEP_BATCH,
    CVM_EVENT_COALESCE_BATCH,

    /* Time a thread spent with nothing to do, waiting for work. Large
     * values here are the difference between "the collector is slow" and
     * "the collector is unbalanced". */
    CVM_EVENT_MARK_WAITING,
    CVM_EVENT_NULLIFY_WAITING,

    /* Time spent in shared synchronisation. */
    CVM_EVENT_SYNC,

    CVM_EVENT_TYPE_COUNT
} CvmEventType;

/* Stable short name for the CSV. Never NULL, never empty. */
str cvm_event_type_name(CvmEventType type);

/* ------------------------------------------------------------------ */
/* per-thread buffer                                                   */
/* ------------------------------------------------------------------ */

/*
 * Events held before a flush. Sized so a collection of a moderately sized
 * heap fits without an intermediate write, which keeps the file's lines
 * grouped by collection rather than by buffer boundary.
 */
#define CVM_STATS_CAPACITY 2000

/* Thread id used for the mutator thread that triggered a collection. */
#define CVM_STATS_MUTATOR_THREAD_ID ((i8) - 1)

/*
 * Parallel arrays rather than an array of structs: a struct of
 * {u64, u64, u8} pads to 24 bytes, these three arrays cost 17. At the
 * default capacity that is 14 KiB per thread.
 */
typedef struct CvmStats {
    u64 start_ns[CVM_STATS_CAPACITY];
    u64 duration_ns[CVM_STATS_CAPACITY];
    u8 type[CVM_STATS_CAPACITY];

    /* Events currently buffered. */
    u32 buffered;

    /* Events written out so far, for accounting and for tests. */
    u64 written;

    i8 thread_id;
    RtChannel* channel;
} CvmStats;

/*
 * The channel a thread's buffer is opened on, or NULL for none.
 *
 * Set once at startup from CvmOptions.stats_channel. A program that wants
 * per-phase numbers points it at a file; a program that does not leaves it
 * NULL, and then no thread allocates a buffer and every recording call is
 * a null check.
 */
void cvm_stats_set_default_channel(RtChannel* channel);

/*
 * Open a buffer on the default channel, or return NULL when there is none.
 *
 * A pointer rather than an embedded struct, because the buffer is 34 KiB —
 * 2000 events of 17 bytes — and a program with a hundred threads should not
 * pay 3.4 MiB for statistics it did not ask for. NULL is the normal case.
 *
 * The thread id is assigned here, from a counter, so that no caller has to
 * manage one. It only labels the CSV.
 */
CvmStats* cvm_stats_open(void);

/* Flush and release a buffer from cvm_stats_open. NULL is accepted. */
void cvm_stats_close(CvmStats* stats);

/*
 * The calling thread's buffer, or NULL. The convenience the collector
 * actually calls; a thread that has none gets NULL and records nothing.
 */
CvmStats* cvm_stats_this_thread(void);

/*
 * Prepare a buffer. Writes the CSV header immediately, so a stats file
 * that exists is always parseable even if the process dies mid-collection.
 */
void cvm_stats_init(CvmStats* stats, RtChannel* channel, i8 thread_id);

/* Write out whatever is buffered. Safe to call with nothing buffered. */
void cvm_stats_flush(CvmStats* stats);

/* Flush, then report a summary line to the log. Does not close the
 * channel; the caller that opened it owns it. */
void cvm_stats_shutdown(CvmStats* stats);

/*
 * Timestamp for a stats-enabled thread, or 0 when stats is NULL.
 *
 * Call sites pair this with cvm_stats_record, so a thread without stats
 * does not even pay for the clock:
 *
 *     u64 t0 = cvm_stats_now(stats);
 *     ... work ...
 *     cvm_stats_record(stats, CVM_EVENT_MARK, t0, cvm_stats_now(stats));
 */
u64 cvm_stats_now(CvmStats const* stats);

/*
 * Record one event. Inline because it sits on the phase boundaries and is
 * three stores in the common case; the flush it may trigger is out of
 * line.
 *
 * start_ns == 0 means "no timestamp was taken", which is how a
 * stats-disabled thread's `cvm_stats_now` result arrives here. Such an
 * event is dropped rather than recorded as a zero-length event at time
 * zero, which would be indistinguishable from a real one.
 */
CVM_INLINE void cvm_stats_record(CvmStats* stats, CvmEventType type, u64 start_ns,
                                 u64 end_ns) {
    if (stats == NULL || start_ns == 0) {
        return;
    }
    if (stats->buffered == CVM_STATS_CAPACITY) {
        cvm_stats_flush(stats);
    }

    u32 i = stats->buffered++;
    stats->type[i] = (u8)type;
    stats->start_ns[i] = start_ns;
    stats->duration_ns[i] = end_ns - start_ns;
}

/* ------------------------------------------------------------------ */
/* process-wide counters                                               */
/* ------------------------------------------------------------------ */

/*
 * The handful of numbers worth reading without parsing a CSV: how many
 * collections have happened and how long they took in total. Atomic
 * because any thread may finish a collection.
 *
 * Deliberately absent: a bytes-allocated counter. It would have to be
 * incremented on the bump fast path, which is the one place in the
 * collector that must stay a few instructions. The same information is
 * derivable from heap size and block counts, which are only touched on the
 * slow path.
 */
typedef struct CvmGcCounters {
    _Atomic u64 collections;
    _Atomic u64 collection_ns;

    /*
     * The longest single collection so far.
     *
     * Kept because a total and a maximum answer different questions and only
     * one of them is about pauses: a collector that stops the world for 40 ms
     * once and 1 ms forty times has the same total as one that stops it for
     * 5 ms eight times, and only the first is a problem for a program with a
     * frame deadline. Updated with a compare-exchange loop because two
     * threads can finish collections concurrently.
     */
    _Atomic u64 pause_max_ns;
} CvmGcCounters;

void cvm_gc_counters_record_collection(u64 start_ns, u64 end_ns);

/* Count a collection and add its stop-the-world time to the total. */
void cvm_gc_counters_record_span(u64 pause_ns);

/* Record one stop-the-world window, without counting a collection. */
void cvm_gc_counters_record_pause(u64 start_ns, u64 end_ns);

/* Convenience for tests and for the exit path. */
u64 cvm_gc_counters_collections(void);
u64 cvm_gc_counters_collection_ns(void);
u64 cvm_gc_counters_pause_max_ns(void);

#endif /* CVM_STATS_H */
