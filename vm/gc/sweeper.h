/*
 * vm/gc/sweeper.h
 *
 * The sweeper, ported from refs/commix/Sweeper.c.
 *
 * ---------------------------------------------------------------------
 * What sweeping has to do
 * ---------------------------------------------------------------------
 *
 * Marking leaves every live object marked: the object's metadata byte is
 * CVM_OM_MARKED, and the block and lines it covers are marked too. Sweeping
 * turns that into a usable heap:
 *
 *   - each object slot: marked becomes allocated, anything else becomes
 *     free
 *   - each line: if it held a live object it is unmarked and kept; if it
 *     held nothing, its objects are cleared and it joins the block's free
 *     line list
 *   - each block: if nothing in it survived, the whole block goes back to
 *     the block allocator; if part of it survived, the block becomes
 *     available to a mutator thread as a recycled block; if all of it
 *     survived, it stays in use
 *   - each large object: if it died, its chunks go back on the large
 *     allocator's free lists
 *
 * ---------------------------------------------------------------------
 * Why sweeping is done in batches, and what that costs
 * ---------------------------------------------------------------------
 *
 * Sweeping the whole heap in one pass would mean no block became
 * available until the very end, so a program that fills the heap and then
 * drops most of it would stall for the whole sweep. Instead sweepers claim
 * batches and blocks become available as they go.
 *
 * That is what the `coalesce_me` block flag is for. A run of free blocks
 * that touches a batch boundary cannot be turned into a superblock yet,
 * because the neighbouring batch may extend it. So the run is marked
 * coalesce_me and left alone, and the coalescing pass merges it once every
 * sweeper that could contribute has finished. Runs in the middle of a
 * batch are already bounded and go straight to the block allocator.
 *
 * Coalescing therefore trails the sweep cursor, and can only advance as
 * far as the *minimum* progress of every sweeper still running — which is
 * what cvm_sweeper_min_cursor computes, including the lazy sweeper on a
 * mutator thread.
 *
 * ---------------------------------------------------------------------
 * The lazy sweeper
 * ---------------------------------------------------------------------
 *
 * A mutator thread that cannot allocate sweeps a batch itself instead of
 * waiting for a collector. That is the "help sweep" step in the
 * allocation retry loop: doing a little work beats blocking, and it means
 * an allocation-heavy thread speeds up the sweep that is holding it back.
 *
 * ---------------------------------------------------------------------
 * Not here yet
 * ---------------------------------------------------------------------
 *
 * No concurrent sweepers. cvm_sweep is written to be safe when called
 * from several threads — batches are claimed by an atomic cursor, and the
 * results are applied through lists that tolerate concurrent pushes — but
 * nothing calls it from more than one thread until the worker pool
 * exists.
 *
 * Evacuation is not here either. The reference does not move objects
 * during sweeping; it reclaims lines and blocks, and the fragmentation
 * that leaves is what its opportunistic evacuation would address. That is
 * a separate step.
 */

#ifndef CVM_GC_SWEEPER_H
#define CVM_GC_SWEEPER_H

#include <stdatomic.h>

#include "base/types.h"
#include "context.h"
#include "gc/block_alloc.h"
#include "gc/heap.h"
#include "stats.h"

/* Blocks one sweeper claims at a time. */
#define CVM_SWEEP_BATCH_SIZE 32

/* Blocks a mutator sweeps when it cannot allocate. */
/*
 * 16, matching the reference's LAZY_SWEEP_MIN_BATCH.
 *
 * It was 1 for most of this project's life, and the number was load-bearing
 * in the worst way: every allocation failure drove the sweep forward by one
 * 32 KiB block, and each of those single blocks also paid for a coalescing
 * pass and a finish attempt. Against a deferred sweep that is exactly what
 * makes the sweep fall behind — the collector completes a collection, hands
 * the sweep to whoever allocates next, and that thread's one-block-at-a-time
 * driving never catches up before the next collection starts. The visible
 * numbers were a heap that grew without bound and live-object counts seven
 * trees high on a one-tree workload.
 */
#define CVM_LAZY_SWEEP_MIN_BATCH 16

/*
 * Free runs shorter than this are accumulated locally and pushed in one
 * go; longer ones go straight to the block allocator.
 *
 * The threshold is where the local list tops out: a run of at least
 * 2^(SIZE-1) blocks cannot fit in a local list of power-of-two pieces, so
 * there is no point staging it.
 */
#define CVM_SUPERBLOCK_LOCAL_LIST_SIZE 4
#define CVM_SUPERBLOCK_LOCAL_LIST_MAX ((1u << CVM_SUPERBLOCK_LOCAL_LIST_SIZE) - 1u)

/*
 * Where a sweep's results go.
 *
 * Kept local to the sweeper and applied at the end of the batch, so the
 * shared structures are touched once per batch rather than once per
 * block. The reference does the same and gives the same reason: taking
 * the block allocator's lock for every block would serialise the
 * sweepers against each other.
 */
typedef struct CvmSweepResult {
    /* Blocks with free lines, to be handed to mutator threads. */
    LocalBlockList recycled_blocks;

    /* Free runs small enough to stage locally. */
    LocalBlockList free_superblocks[CVM_SUPERBLOCK_LOCAL_LIST_SIZE];
} CvmSweepResult;

void cvm_sweep_result_init(CvmSweepResult* result);
void cvm_sweep_result_clear(CvmSweepResult* result);

/*
 * Prepare for a sweep: every block from 0 to the current block count is
 * to be swept, nothing is coalesced yet, and the sweep is not done.
 */
void cvm_sweep_begin(CvmHeap* heap);

/*
 * Sweep up to `max_count` blocks.
 *
 * `cursor_done` is this sweeper's own progress, updated when the batch
 * finishes. `node` is the mutator thread that should receive recycled
 * blocks, or NULL to spread them round-robin over the registered threads
 * (which is what a GC worker wants, since it has no allocator of its own).
 */
void cvm_sweep(CvmHeap* heap, _Atomic u32* cursor_done, u32 max_count,
               CvmThreadNode* node);

/*
 * Merge free runs that were held back at batch boundaries.
 *
 * Called after each batch. Does nothing until some sweeper's progress
 * allows a run to be closed off, so calling it more often than necessary
 * is harmless.
 */
void cvm_sweep_lazy_coalesce(CvmHeap* heap);

/*
 * The earliest block index that is not yet safely swept by every sweeper.
 *
 * Coalescing cannot go past this: a run that touches a block a sweeper is
 * still working on may still grow.
 */
u32 cvm_sweeper_min_cursor(CvmHeap* heap);

/* Whether the sweep has finished, coalescing included. */
bool cvm_sweep_is_done(CvmHeap* heap);

/*
 * Sweep one batch, coalesce, and finish the sweep if it is now complete.
 *
 * This is the whole driving loop in one call, so that a mutator which
 * needs memory and a collector that wants to start a new collection both
 * make progress the same way. Returns true if the sweep is now finished.
 *
 * Safe to call from several threads: batches are claimed by an atomic
 * cursor, and the finishing work is claimed by a compare-exchange so only
 * one thread does it.
 */
bool cvm_sweep_drive(CvmHeap* heap, CvmThreadNode* node, u32 max_count);

/*
 * Finish the sweep if it is complete: put the reserve back and leave
 * concurrent mode. Does nothing if it is not complete, or if another
 * thread is already finishing.
 */
bool cvm_sweep_try_finish(CvmHeap* heap);

/*
 * Whether any sweep is in flight, over every kind of sweeper: a registered
 * thread inside a batch, a worker inside a batch, or a worker that has been
 * handed the sweep and has not arrived yet.
 *
 * One function because there is one question, and three places that have to
 * agree on the answer: before finishing a sweep, before beginning a new one,
 * and before giving up on finishing the current one. They disagreed once, and
 * the symptom was a block described twice in the same round.
 */
bool cvm_sweep_any_in_flight(CvmHeap* heap);

/*
 * Drive the sweep to its end, or to the point where this thread can do no
 * more. This is what an allocator that cannot allocate and cannot grow must
 * do instead of dying: commix waits for the sweep the same way, and it is
 * the difference between "the heap is full" and "the heap is full right now".
 */
void cvm_sweep_finish(CvmHeap* heap, str const where);

/*
 * Whether a sweep handoff has been posted and not yet consumed — a worker on
 * its way, which blocks *finishing* a sweep and does not block beginning one.
 * See the comment on the definition for why those are two questions.
 */
bool cvm_sweep_handoff_pending(CvmHeap* heap);

#if defined(CVM_DEBUG)
/*
 * Reset every block's debug state to "must be swept", which is what the
 * sweeper expects to find and what the consistency check verifies.
 */
void cvm_sweep_clear_debug_state(CvmHeap* heap);

/*
 * Check that sweeping left the heap in a state the allocator can use:
 * no block still marked, no line still marked, no object still marked,
 * and no half-finished superblock left over.
 */
void cvm_sweep_assert_consistent(CvmHeap* heap);
#endif

#endif /* CVM_GC_SWEEPER_H */
