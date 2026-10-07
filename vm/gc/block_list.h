/*
 * vm/gc/block_list.h
 *
 * Intrusive singly-linked lists of BlockMeta, ported from
 * refs/commix/datastructures/BlockList.
 *
 * Two shapes:
 *
 *   BlockList       a lock-free stack shared between threads
 *   LocalBlockList  a plain two-pointer list owned by one thread, used to
 *                   accumulate blocks and hand them over in one push
 *
 * Blocks are chained through BlockMeta.next_block as an index, not a
 * pointer; see gc/meta.h.
 */

#ifndef CVM_GC_BLOCK_LIST_H
#define CVM_GC_BLOCK_LIST_H

#include <stdatomic.h>

#include "base/types.h"
#include "gc/meta.h"

/*
 * Shared stack. `head` holds a BlockMeta pointer as an integer, so that
 * the atomic word and the pointer cannot alias, and so the empty state is
 * a plain zero.
 *
 * Concurrent access notes, including the ABA argument this design relies
 * on, are at the top of block_list.c.
 */
typedef struct BlockList {
    _Atomic(usize) head;
} BlockList;

/*
 * Single-threaded list. `last` is only meaningful when `first` is set; it
 * exists so the whole run can be handed to block_list_push_all in one CAS.
 */
typedef struct LocalBlockList {
    BlockMeta* first;
    BlockMeta* last;
} LocalBlockList;

void block_list_init(BlockList* list);
void block_list_clear(BlockList* list);

/* Pop one block, or NULL when empty. Safe to call concurrently. */
BlockMeta* block_list_pop(BlockList* list, BlockMeta* meta_start);

/* Pop one block without atomics. Only valid while other threads are stopped. */
BlockMeta* block_list_pop_only_thread(BlockList* list, BlockMeta* meta_start);

/* Push one block onto the front. Safe to call concurrently. */
void block_list_push(BlockList* list, BlockMeta* meta_start, BlockMeta* m);

/*
 * Push an already-linked run [first..last] onto the front, in one CAS.
 * The run must be linked through next_block with `last` terminated.
 */
void block_list_push_all(BlockList* list, BlockMeta* meta_start, BlockMeta* first,
                         BlockMeta* last);

/* ------------------------------------------------------------------ */
/* LocalBlockList: single-threaded, all inline                         */
/* ------------------------------------------------------------------ */

CVM_INLINE void local_block_list_clear(LocalBlockList* list) {
    list->first = NULL;
    list->last = NULL;
}

CVM_INLINE bool local_block_list_is_empty(LocalBlockList const* list) {
    return list->first == NULL;
}

CVM_INLINE void local_block_list_push(LocalBlockList* list, BlockMeta* meta_start,
                                      BlockMeta* m) {
    BlockMeta* head = list->first;
    if (head == NULL) {
        block_meta_set_next(meta_start, m, NULL);
        list->first = m;
        list->last = m;
    } else {
        block_meta_set_next(meta_start, m, head);
        list->first = m;
    }
}

#endif /* CVM_GC_BLOCK_LIST_H */
