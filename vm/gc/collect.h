/*
 * vm/gc/collect.h
 *
 * The collection driver: the one entry point that runs a whole collection.
 *
 * ---------------------------------------------------------------------
 * What a collection is
 * ---------------------------------------------------------------------
 *
 *   1. stop the world
 *   2. reset the marking state
 *   3. mark everything reachable from the roots
 *   4. nullify weak references whose targets did not survive
 *   5. sweep, returning dead memory to the allocators
 *   6. decide whether the heap should grow
 *   7. resume the world
 *
 * Steps 2 to 5 are phases, and the phase a collection is in is visible in
 * the heap so that a worker thread woken mid-collection knows what to do.
 *
 * ---------------------------------------------------------------------
 * Why the collecting thread is a mutator
 * ---------------------------------------------------------------------
 *
 * Collection is triggered by an allocation that could not be satisfied,
 * so the thread asking for memory is the one that collects. There is no
 * dedicated collector thread to hand the work to, and inventing one would
 * mean a thread that has to be kept alive and scheduled for a job that
 * happens only when some other thread is already blocked.
 *
 * ---------------------------------------------------------------------
 * Only one collector at a time
 * ---------------------------------------------------------------------
 *
 * If two threads run out of memory at once, one collects and the other
 * waits and is told to retry: by the time it retries, the heap has just
 * been collected, so its allocation is likely to succeed. That is what
 * the false return from cvm_gc_collect means, and why the allocation
 * retry loop treats it as "try again" rather than as a failure.
 */

#ifndef CVM_GC_COLLECT_H
#define CVM_GC_COLLECT_H

#include "base/types.h"
#include "gc/heap.h"

/*
 * Run a collection.
 *
 * Returns true if this call ran one, false if another thread was already
 * collecting — in which case the heap has just been collected, so the
 * caller should retry rather than collect again.
 */
bool cvm_gc_collect(CvmHeap* heap);

/*
 * What the last collection did.
 *
 * Note what is absent: a count of live objects. Counting them means
 * walking the whole bytemap, which for a heap of any size costs more than
 * the collection it is measuring — hundreds of microseconds for a few
 * megabytes, and seconds for a gigabyte. It also distorts the very ratio
 * the growth heuristic reads, since the period between collections then
 * includes the previous one's counting.
 *
 * A caller that wants the live count asks cvm_vm_stats, which counts on
 * demand. That is the right place for an expensive question.
 */
typedef struct CvmCollectionStats {
    u64 start_ns;
    u64 end_ns;
    u64 mark_ns;
    u32 weak_cleared;
    u32 blocks_before;
    u32 blocks_after;
    bool grew;
} CvmCollectionStats;

CvmCollectionStats cvm_gc_last_collection(void);

/*
 * Collect, then drive the sweep to completion.
 *
 * The production path deliberately leaves the sweep running alongside the
 * program; this is the settled shape, which is what a test inspecting the
 * heap wants and what a program wants at exit.
 */
void cvm_gc_collect_and_finish(CvmHeap* heap);

#endif /* CVM_GC_COLLECT_H */
