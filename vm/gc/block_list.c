/*
 * vm/gc/block_list.c
 *
 * Lock-free block stacks, ported from refs/commix/datastructures/BlockList.c.
 *
 * ---------------------------------------------------------------------
 * ABA, and why this is believed safe
 * ---------------------------------------------------------------------
 *
 * The shared list is a Treiber stack: pop and push are compare-exchange
 * loops over one atomic word holding a BlockMeta pointer. That data
 * structure is not ABA-safe in general. A thread that reads head = X and
 * X.next = Y can have its exchange succeed after X was popped and pushed
 * back in the meantime, splicing Y onto a list X is no longer on.
 *
 * What is claimed to make it safe here is a usage invariant, not the data
 * structure. The reference implementation states it as:
 *
 *     This could suffer from the ABA problem. However, during a single
 *     phase each BlockMeta is removed no more than once. It would need to
 *     be swept before re-use.
 *
 * In other words: within one GC phase a given block is popped from a given
 * list at most once, and it cannot be pushed back until it has been swept,
 * so the X-popped-and-returned window cannot open.
 *
 * Status: this argument is inherited from the reference and has NOT been
 * re-verified against our sweeper, which is not written yet. It must be
 * checked when the sweeper is ported (see gc/sweeper.c), because it is the
 * only thing standing between this file and a rare, timing-dependent
 * corruption. If the invariant does not hold there, the fix is a tagged
 * head — a counter packed alongside the pointer — or hazard pointers; it
 * is not something to paper over at the call site.
 *
 * ---------------------------------------------------------------------
 * Memory ordering
 * ---------------------------------------------------------------------
 *
 * push is a release: a block's next_block write must be visible to
 * whichever thread acquires the head that now points at it.
 *
 * pop acquires, including on failure: the newly read head's next_block
 * must be visible before it is dereferenced. The initial load is acquire
 * for the same reason.
 *
 * Everything is weakly ordered on purpose; there is no seq_cst here.
 */

#include "gc/block_list.h"

#include <stdatomic.h>

#include "base/types.h"
#include "gc/meta.h"

void block_list_init(BlockList* list) {
    atomic_store_explicit(&list->head, 0, memory_order_relaxed);
}

void block_list_clear(BlockList* list) {
    atomic_store_explicit(&list->head, 0, memory_order_relaxed);
}

BlockMeta* block_list_pop(BlockList* list, BlockMeta* meta_start) {
    usize head = atomic_load_explicit(&list->head, memory_order_acquire);

    for (;;) {
        if (head == 0) {
            return NULL;
        }

        BlockMeta* m = (BlockMeta*)head;
        usize next = (usize)block_meta_next(meta_start, m);

        if (atomic_compare_exchange_weak_explicit(
                &list->head, &head, next, memory_order_acquire, memory_order_acquire)) {
            return m;
        }

        /* The exchange failed and updated `head` with the value that beat
         * us; loop and retry against it. */
    }
}

BlockMeta* block_list_pop_only_thread(BlockList* list, BlockMeta* meta_start) {
    usize head = atomic_load_explicit(&list->head, memory_order_relaxed);
    if (head == 0) {
        return NULL;
    }

    BlockMeta* m = (BlockMeta*)head;
    atomic_store_explicit(&list->head, (usize)block_meta_next(meta_start, m),
                          memory_order_relaxed);
    return m;
}

void block_list_push(BlockList* list, BlockMeta* meta_start, BlockMeta* m) {
    usize head = atomic_load_explicit(&list->head, memory_order_relaxed);

    for (;;) {
        block_meta_set_next(meta_start, m, (BlockMeta*)head);

        if (atomic_compare_exchange_weak_explicit(&list->head, &head, (usize)m,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
            return;
        }

        /* `head` was refreshed; relink m against the new head and retry. */
    }
}

void block_list_push_all(BlockList* list, BlockMeta* meta_start, BlockMeta* first,
                         BlockMeta* last) {
    usize head = atomic_load_explicit(&list->head, memory_order_relaxed);

    for (;;) {
        block_meta_set_next(meta_start, last, (BlockMeta*)head);

        if (atomic_compare_exchange_weak_explicit(&list->head, &head, (usize)first,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
            return;
        }
    }
}
