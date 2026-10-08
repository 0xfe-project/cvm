/*
 * vm/gc/sweeper.c
 *
 * The sweeper. See vm/gc/sweeper.h for the shape, why sweeping is batched,
 * and what the coalesce_me flag is for.
 *
 * The two functions that do the actual work are cvm_sweep_simple_block and
 * cvm_sweep_superblock. Everything else is bookkeeping around them: which
 * batch to take, where free runs go, and how far coalescing may advance.
 */

#include "gc/sweeper.h"

#include <stdatomic.h>

#include "base/assert.h"
#include "base/math.h"
#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "gc/block_alloc.h"
#include "gc/block_list.h"
#include "gc/bytemap.h"
#include "cvm/external.h"
#include "gc/heap.h"
#include "gc/large.h"
#include "gc/layout.h"
#include "gc/meta.h"
#include "gc/pool.h"
#include "log.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* sweep results                                                       */
/* ------------------------------------------------------------------ */

void cvm_sweep_result_clear(CvmSweepResult* result) {
    local_block_list_clear(&result->recycled_blocks);
    for (u32 i = 0; i < CVM_SUPERBLOCK_LOCAL_LIST_SIZE; i++) {
        local_block_list_clear(&result->free_superblocks[i]);
    }
}

void cvm_sweep_result_init(CvmSweepResult* result) {
    cvm_sweep_result_clear(result);
}

/* ------------------------------------------------------------------ */
/* simple blocks                                                       */
/* ------------------------------------------------------------------ */
/*
 * Hand the finalizer of every dead object in a range to the queue.
 *
 * This has to run over *every* path that reclaims objects, and the reason
 * is not obvious until it bites: the sweeper has bulk paths that clear a
 * whole line or a whole block without ever looking at an individual
 * object. A block that held nothing but waste takes one of those paths,
 * and a finalizer inside it would be lost — which is the common case for
 * an external handle, since the whole point of one is to be the only thing
 * keeping a resource alive.
 *
 * A slot reading "allocated" at this point means the object is dead:
 * marking wrote "marked" on everything that survived, so anything still
 * reading "allocated" did not. That is why this can be a single pass and
 * needs no separate liveness question.
 *
 * The walk steps by object size when it finds an object and by one
 * allocation slot when it does not, so a block costs one iteration per
 * object plus one per gap rather than one per slot.
 */
static void extract_finalizers(Bytemap* bytemap, u8* start, u8* limit) {
    u8* p = start;

    while (p < limit) {
        ObjectMeta* slot = bytemap_get(bytemap, p);

        if (object_meta_is_allocated(slot)) {
            CvmGcHeader* header = (CvmGcHeader*)p;

            if ((header->flags & CVM_GC_HAS_FINALIZER) != 0) {
                CvmExternal* ext = (CvmExternal*)(p + sizeof(CvmGcHeader));
                /*
                 * A failure means the queue is full, which is counted and
                 * read as cvm_finalizer_dropped_count rather than logged
                 * per object: a sweep can produce thousands, and a line
                 * each would bury the collection's own output.
                 */
                (void)cvm_finalizer_enqueue(ext->c_ptr, ext->finalizer);
            }
            p += cvm_object_size(header);
        } else {
            p += CVM_ALLOC_ALIGNMENT;
        }
    }
}

/* The same, for a block that says it might be worth doing. */
static void extract_finalizers_if_flagged(Bytemap* bytemap, BlockMeta* block, u8* start,
                                          u8* limit) {
    if (!block_meta_has_finalizers(block)) {
        return;
    }
    extract_finalizers(bytemap, start, limit);
}

/*
 * Sweep one line's objects, handing any finalizer to the queue first.
 *
 * Slot by slot rather than through the bulk line sweep, because a line can
 * hold both survivors and dead objects and each dead one may carry a
 * finalizer.
 */
static void sweep_line_with_finalizers(ObjectMeta* line_cursor, u8* line_start) {
    for (usize i = 0; i < CVM_SLOTS_PER_LINE; i++) {
        ObjectMeta* slot = &line_cursor[i];

        if (object_meta_is_allocated(slot)) {
            u8* addr = line_start + i * CVM_ALLOC_ALIGNMENT;
            CvmGcHeader* header = (CvmGcHeader*)addr;

            if ((header->flags & CVM_GC_HAS_FINALIZER) != 0) {
                CvmExternal* ext = (CvmExternal*)(addr + sizeof(CvmGcHeader));
                (void)cvm_finalizer_enqueue(ext->c_ptr, ext->finalizer);
            }
            object_meta_set_free(slot);
        } else if (object_meta_is_marked(slot)) {
            object_meta_set_allocated(slot);
        } else {
            /* Already free, or a placeholder for a large object's
             * interior. Both become free, which is what
             * object_meta_sweep does. */
            object_meta_set_free(slot);
        }
    }
}

/*
 * Sweep one ordinary block.
 *
 * Returns the number of free blocks it produced, which is 1 when nothing
 * in it survived and 0 when it did. The caller accumulates those into
 * free runs.
 *
 * Two cases, and the first is much cheaper:
 *
 *   - the block is not marked, so nothing in it survived. The whole block
 *     goes back: its metadata is reset and its object slots are cleared
 *     in one pass. Its line metadata is not touched, because the
 *     allocator ignores line metadata for a block it has been handed in
 *     full.
 *
 *   - the block is marked, so something in it survived. Then it is line
 *     by line: a marked line is unmarked and its objects are swept; an
 *     unmarked line has nothing live in it, so its objects are cleared
 *     and it joins the block's free-line list. Consecutive free lines are
 *     merged into one run, because that is the unit the allocator hands
 *     out.
 *
 * The free-line list is what makes a partially free block usable: the
 * allocator follows it to find the next run of free lines, so a block
 * with a few surviving objects still contributes its holes.
 */
static u32 cvm_sweep_simple_block(CvmThreadNode* node, BlockMeta* block_meta,
                                  u8* block_start, LineMeta* line_metas,
                                  CvmSweepResult* result) {
    cvm_assert(block_meta_debug_state(block_meta) == (u8)CVM_DBG_MUST_SWEEP);

    if (!block_meta_is_marked(block_meta)) {
        /*
         * Nothing survived, so the whole block is free — but every object
         * in it died here, and any of them may own something outside the
         * heap. This bulk path is the reason the block needs a finalizer
         * flag at all: it clears the metadata in one go and would never
         * look at an individual object.
         */
        extract_finalizers_if_flagged(node->allocator.bytemap, block_meta, block_start,
                                      block_end(block_start));

        block_meta_clear(block_meta);
        block_meta_set_debug_state(block_meta, CVM_DBG_FREE);

        object_meta_clear_block(bytemap_get(node->allocator.bytemap, block_start));
        return 1;
    }

    cvm_assert(block_meta_is_marked(block_meta));
    block_meta_unmark(block_meta);

    Bytemap* bytemap = node->allocator.bytemap;

    /* Three cursors advancing together: line index, line metadata, and the
     * bytemap entry for the line's first slot. */
    u32 line_index = 0;
    LineMeta* line_meta = line_metas;
    u8* line_start = block_start;
    ObjectMeta* bytemap_cursor = bytemap_get(bytemap, line_start);

    FreeLineMeta* last_recyclable = NULL;

    while (line_index < CVM_LINE_COUNT) {
        if (line_meta_is_marked(line_meta)) {
            /* Live lines are kept; their objects lose the mark bit and
             * become ordinary allocated objects. */
            line_meta_unmark(line_meta);
            sweep_line_with_finalizers(bytemap_cursor, line_start);

            line_index++;
            line_meta++;
            line_start += CVM_LINE_SIZE;
            bytemap_cursor = bytemap_next_line(bytemap_cursor);
        } else {
            /*
             * A free line. If it is the first of a run, the block's head
             * points at it; otherwise the previous run's last node links
             * to it. Either way the run is threaded through the line
             * memory itself, so it costs nothing but the line.
             */
            if (last_recyclable == NULL) {
                block_meta_set_first_free_line(block_meta, (i8)line_index);
            } else {
                last_recyclable->next = (i8)line_index;
            }

            /* An unmarked line inside a marked block: everything in it is
             * dead, and this path clears the line in bulk. */
            extract_finalizers_if_flagged(bytemap, block_meta, line_start,
                                          line_start + CVM_LINE_SIZE);

            object_meta_clear_line(bytemap_cursor);
            last_recyclable = (FreeLineMeta*)line_start;

            line_index++;
            line_meta++;
            line_start += CVM_LINE_SIZE;
            bytemap_cursor = bytemap_next_line(bytemap_cursor);

            /* Extend the run over every following unmarked line. */
            u8 size = 1;
            while (line_index < CVM_LINE_COUNT && !line_meta_is_marked(line_meta)) {
                extract_finalizers_if_flagged(bytemap, block_meta, line_start,
                                              line_start + CVM_LINE_SIZE);
                object_meta_clear_line(bytemap_cursor);
                size++;

                line_index++;
                line_meta++;
                line_start += CVM_LINE_SIZE;
                bytemap_cursor = bytemap_next_line(bytemap_cursor);
            }
            last_recyclable->size = size;
        }
    }

    if (last_recyclable != NULL) {
        /* Some lines are free, so the block is worth keeping. */
        last_recyclable->next = CVM_LAST_HOLE;

        cvm_assert(block_meta_first_free_line(block_meta) >= 0);
        cvm_assert((i32)block_meta_first_free_line(block_meta) < CVM_LINE_INDEX_LIMIT);

        atomic_fetch_add_explicit(&node->allocator.recycled_block_count, 1,
                                  memory_order_relaxed);
        block_meta_set_debug_state(block_meta, CVM_DBG_PARTIAL_FREE);

        /*
         * The mutator that takes this block reads the free-line list it
         * just had written into it, so the writes must be visible before
         * the block is. The release fence orders them; the acquire on the
         * reader's side is in the allocator's new-block path.
         */
        atomic_thread_fence(memory_order_release);
        local_block_list_push(&result->recycled_blocks,
                              node->allocator.heap->block_meta_start, block_meta);
    } else {
        /* Every line held something live: the block stays in use. */
        block_meta_set_debug_state(block_meta, CVM_DBG_NOT_FREE);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* superblocks                                                         */
/* ------------------------------------------------------------------ */

/*
 * Sweep one large object's superblock.
 *
 * Returns how many whole blocks at the front are now free.
 *
 * The layout that makes this tractable: large objects are allocated from
 * the start of a superblock, and any remainder is filled with more large
 * objects. So an object can only begin at the superblock's first slot, or
 * somewhere in its *last* block. Everything in between is either the body
 * of the first object or the bodies of later ones. That means the sweep
 * only has to examine the first slot and then step through the last block
 * at the large-object granularity.
 *
 * `batch_limit` is the end of the current batch. A free block that
 * reaches past it cannot be published as a superblock yet, because the
 * sweeper handling the next batch may extend it; it is marked
 * superblock_start_me and left for the coalescing pass.
 */
static u32 cvm_sweep_superblock(CvmLargeAllocator* large, BlockMeta* block_meta,
                                u8* block_start, BlockMeta* batch_limit) {
    u32 superblock_size = block_meta_superblock_size(block_meta);
    u8* block_end = block_start + (usize)superblock_size * CVM_BLOCK_SIZE;

    ObjectMeta* first_object = bytemap_get(large->bytemap, block_start);
    cvm_assert(!object_meta_is_free(first_object));

    BlockMeta* last_block = block_meta + superblock_size - 1;
    u32 free_count = 0;

    if (superblock_size > 1 && !object_meta_is_marked(first_object)) {
        /*
         * The object at the start is dead and it spanned more than one
         * block, so every block but the last is entirely its body — and
         * therefore entirely free.
         */
        free_count = superblock_size - 1;

        /*
         * Those blocks are now free, and the block allocator's free path
         * asserts that a block arriving there is in the free state. This
         * is where a freed superblock's body blocks get it: without this
         * they would reach the allocator still marked as needing a sweep,
         * and the assertion that catches a double free would fire on a
         * block that was never freed twice.
         */
        for (u32 i = 0; i < free_count; i++) {
            block_meta_set_debug_state(block_meta + i, CVM_DBG_FREE);
        }
    } else {
        /* Still in use: the object at the start survived, so the body
         * blocks it spans are not free. */
        for (u32 i = 0; i + 1 < superblock_size; i++) {
            block_meta_set_debug_state(block_meta + i, CVM_DBG_NOT_FREE);
        }
    }

    u8* last_block_start = block_end - CVM_BLOCK_SIZE;
    u8* chunk_start = NULL;

    if (!object_meta_is_marked(first_object)) {
        /* Its tail occupies the start of the last block. */
        chunk_start = last_block_start;
    }
    object_meta_sweep(first_object);

    /*
     * Walk the last block at the large-object granularity, collecting
     * runs of dead chunks. A chunk is free from where one dead object
     * starts until the next live one.
     */
    u8* current = last_block_start + CVM_LARGE_OBJECT_MIN_SIZE;
    ObjectMeta* current_meta = bytemap_get(large->bytemap, current);

    while (current < block_end) {
        if (chunk_start == NULL) {
            if (object_meta_is_allocated(current_meta) ||
                object_meta_is_placeholder(current_meta)) {
                chunk_start = current;
            }
        } else if (object_meta_is_marked(current_meta)) {
            /* A survivor begins here, so the run ends just before it. */
            cvm_large_add_chunk(large, (CvmChunk*)chunk_start,
                                (usize)(current - chunk_start));
            chunk_start = NULL;
        }
        object_meta_sweep(current_meta);

        current += CVM_LARGE_OBJECT_MIN_SIZE;
        current_meta += CVM_LARGE_OBJECT_MIN_SIZE / CVM_ALLOC_ALIGNMENT;
    }

    if (chunk_start == last_block_start) {
        /* The whole last block is one dead chunk, so it is a free block. */
        free_count += 1;
        block_meta_set_debug_state(last_block, CVM_DBG_FREE);
    } else {
        block_meta_set_debug_state(last_block, CVM_DBG_NOT_FREE);
        if (object_meta_is_free(first_object)) {
            /*
             * The first object died but the last block is not wholly free,
             * so something else lives in it. The slot where the first
             * object's tail ended becomes a placeholder: something starts
             * there, but it is not a live object.
             */
            object_meta_set_placeholder(bytemap_get(large->bytemap, last_block_start));
        }

        if (free_count > 0) {
            if (last_block < batch_limit) {
                /* Within this batch, so it can be published now. */
                block_meta_set_flag_and_size(last_block, CVM_BLOCK_SUPERBLOCK_START, 1);
            } else {
                /*
                 * It reaches into the next batch. Marking it
                 * superblock_start_me rather than start keeps a sweeper
                 * working on the next batch from mistaking it for a
                 * superblock it should skip, and leaves the decision to
                 * coalescing.
                 */
                block_meta_set_flag(last_block, CVM_BLOCK_SUPERBLOCK_START_ME);
            }
        }

        if (chunk_start != NULL) {
            cvm_large_add_chunk(large, (CvmChunk*)chunk_start,
                                (usize)(current - chunk_start));
        }
    }

    return free_count;
}

/* ------------------------------------------------------------------ */
/* choosing where recycled blocks go                                   */
/* ------------------------------------------------------------------ */

/*
 * Which mutator thread receives the next recycled block.
 *
 * Round-robin over the registered threads, so a long sweep does not pile
 * every partially free block onto one thread's allocator. A GC worker has
 * no allocator of its own, so it always asks for a thread this way.
 */
static CvmThreadNode* pick_mutator_round_robin(void) {
    static _Atomic u32 counter = 0;

    cvm_threads_lock_read();
    CvmThreadNode* head = cvm_threads_head();

    u32 count = cvm_thread_count();
    if (head == NULL || count == 0) {
        cvm_threads_unlock_read();
        return NULL;
    }

    u32 which = atomic_fetch_add(&counter, 1) % count;
    CvmThreadNode* node = head;
    for (u32 i = 0; i < which && node->next != NULL; i++) {
        node = node->next;
    }

    cvm_threads_unlock_read();
    return node;
}

/* ------------------------------------------------------------------ */
/* applying a batch's results                                          */
/* ------------------------------------------------------------------ */

static void apply_result(CvmSweepResult* result, CvmBlockAllocator* blocks,
                         CvmThreadNode* node) {
    CvmThreadNode* target = node;
    if (target == NULL) {
        target = pick_mutator_round_robin();
    }

    /*
     * Partially free blocks go to a mutator thread's own recycled list
     * rather than to the shared free list. A thread that is about to
     * allocate is the right owner for a block with holes in it, and it
     * avoids a round trip through the allocator's lock.
     *
     * If there is no registered thread — during teardown, or a test that
     * never attached — the blocks cannot be given away. Returning them to
     * the shared list would lose their free-line lists, which only the
     * per-thread allocator knows how to use, so they are left out of the
     * sweep. That is a leak of address space, not of correctness, and it
     * cannot happen while any thread is allocating.
     */
    if (target != NULL) {
        BlockMeta* first = result->recycled_blocks.first;
        if (first != NULL) {
            block_list_push_all(&target->allocator.recycled_blocks,
                                target->allocator.heap->block_meta_start, first,
                                result->recycled_blocks.last);
        }
    }

    for (u32 i = 0; i < CVM_SUPERBLOCK_LOCAL_LIST_SIZE; i++) {
        LocalBlockList item = result->free_superblocks[i];
        if (item.first != NULL) {
            block_list_push_all(&blocks->free_superblocks[i], blocks->meta_start,
                                item.first, item.last);
        }
    }

    cvm_sweep_result_clear(result);
}

/* Defined with the finishing logic, below; a sweep cannot start while a
 * straggler from the previous one is still filing results. */
static bool any_other_sweeper_is_in_flight(void);

/* ------------------------------------------------------------------ */
/* sweeping a batch                                                    */
/* ------------------------------------------------------------------ */

void cvm_sweep_begin(CvmHeap* heap) {
    /*
     * A sweep only starts once the previous one is finished, and finished
     * means no thread is still inside a batch — see
     * any_other_sweeper_is_in_flight. Resetting the cursor while a straggler
     * held a batch would let it hand blocks to a mutator that the new sweep
     * is about to describe again.
     */
    cvm_assert(!any_other_sweeper_is_in_flight());

    atomic_store(&heap->sweep.cursor, 0);
    heap->sweep.limit = heap->block_count;
    atomic_store(&heap->sweep.coalesce_done, 0);
    atomic_store(&heap->sweep.post_sweep_done, false);
    atomic_store(&heap->sweep.finishing, false);

    /*
     * Per-thread sweep progress is deliberately not reset here. A thread
     * that has a claim in flight owns its own number, and clearing it from
     * another thread would tell coalescing that work had been abandoned.
     * Each thread clears its own when it stops sweeping.
     */
}

void cvm_sweep(CvmHeap* heap, _Atomic u32* cursor_done, u32 max_count,
               CvmThreadNode* node) {
    /*
     * Timed before the claim, so a batch that waits for a contended claim
     * counts as waiting rather than as sweeping. That is the honest split:
     * a sweep batch that costs 2 microseconds of work and 200 of lock
     * contention should not be reported as slow sweeping.
     */
    u64 sweep_started = rt_time_nanos();
    CvmBlockAllocator* blocks = heap->block_alloc;
    CvmSweepResult result;
    cvm_sweep_result_init(&result);

    /*
     * Claim a batch. The exchange is what keeps two sweepers from taking
     * the same blocks, so it happens before anything is read.
     */
    u32 start_index = heap->sweep.limit;
    u32 cursor = atomic_load(&heap->sweep.cursor);
    if (cursor < heap->sweep.limit) {
        start_index = atomic_fetch_add(&heap->sweep.cursor, max_count);
    }

    u32 limit_index = start_index + max_count;
    if (limit_index > heap->sweep.limit) {
        limit_index = heap->sweep.limit;
    }

    cvm_assert(atomic_load(cursor_done) <= start_index);
    if (start_index >= limit_index) {
        /* Another sweeper took the tail; nothing to do. */
        atomic_store_explicit(cursor_done, limit_index, memory_order_release);
        return;
    }

    BlockMeta* first = block_meta_at(heap->block_meta_start, start_index);
    BlockMeta* limit = block_meta_at(heap->block_meta_start, limit_index);

    BlockMeta* reserve_first =
        atomic_load_explicit(&blocks->reserved, memory_order_relaxed);
    BlockMeta* reserve_limit =
        (reserve_first != NULL) ? reserve_first + CVM_SWEEP_RESERVE_BLOCKS : NULL;

    /*
     * Skip blocks whose sweep belongs to an earlier batch: the
     * continuation of a superblock, a run held back for coalescing, or
     * the tail of a run whose head was in the previous batch.
     */
    while (first < limit &&
           (block_meta_is_superblock_start_me(first) ||
            block_meta_is_superblock_tail(first) || block_meta_is_coalesce_me(first))) {
        start_index++;
        first++;
    }

    BlockMeta* current = first;
    u8* current_block_start = heap->heap_start + (usize)start_index * CVM_BLOCK_SIZE;
    LineMeta* line_metas = heap->line_meta_start + (usize)start_index * CVM_LINE_COUNT;

    /* Head of the free run being accumulated, if any. */
    BlockMeta* last_free_block_start = NULL;

    /*
     * Whether recycled blocks are spread over threads by the caller
     * (round-robin) or all given to one. A mutator sweeping its own batch
     * keeps them; a worker spreads them.
     */
    bool spread = (node == NULL);

    while (current < limit) {
        u32 size = 1;
        u32 free_count = 0;

        cvm_assert(!block_meta_is_coalesce_me(current));

        bool in_reserve = (reserve_first != NULL && current >= reserve_first &&
                           current < reserve_limit);

        if (block_meta_is_reserved(current) || in_reserve) {
            /*
             * The reserve is held back from sweeping so the sweeper always
             * has somewhere to work. A free run that runs into it has to
             * stop there.
             */
            if (block_meta_is_reserved(current) && last_free_block_start != NULL) {
                u32 total = (u32)(current - last_free_block_start);
                if (last_free_block_start == first || current >= limit) {
                    block_meta_set_flag_and_size(last_free_block_start,
                                                 CVM_BLOCK_COALESCE_ME, (i32)total);
                } else if (total >= CVM_SUPERBLOCK_LOCAL_LIST_MAX) {
                    cvm_block_alloc_add_free_superblock(blocks, last_free_block_start,
                                                        total);
                } else {
                    cvm_block_alloc_add_free_superblock_local(
                        blocks, result.free_superblocks, last_free_block_start, total);
                }
                last_free_block_start = NULL;
            }
        } else if (block_meta_is_simple(current)) {
            CvmThreadNode* target = node;
            if (spread) {
                target = pick_mutator_round_robin();
            }
            if (target != NULL) {
                free_count = cvm_sweep_simple_block(
                    target, current, current_block_start, line_metas, &result);
            }
        } else if (block_meta_is_superblock_start(current)) {
            size = block_meta_superblock_size(current);
            cvm_assert(size > 0);

            CvmThreadNode* target = node;
            if (spread) {
                target = pick_mutator_round_robin();
            }
            if (target != NULL) {
                free_count = cvm_sweep_superblock(&target->large_allocator, current,
                                                  current_block_start, limit);
            }
        } else {
            /* Already free from an earlier phase. */
            cvm_assert(block_meta_is_free(current));
            cvm_assert(block_meta_debug_state(current) == (u8)CVM_DBG_MUST_SWEEP);
            free_count = 1;
            block_meta_set_debug_state(current, CVM_DBG_FREE);
        }

        cvm_assert(size > 0);
        cvm_assert(free_count <= size);

        /* Start a run, or close one when a block survived. */
        if (last_free_block_start == NULL) {
            if (free_count > 0) {
                last_free_block_start = current;
            }
        }
        if (last_free_block_start != NULL && free_count < size) {
            BlockMeta* free_limit = current + free_count;
            u32 total = (u32)(free_limit - last_free_block_start);

            if (last_free_block_start == first || free_limit >= limit) {
                /*
                 * The run touches a batch boundary, so a neighbouring
                 * batch may extend it. Hold it for coalescing.
                 */
                block_meta_set_flag_and_size(last_free_block_start,
                                             CVM_BLOCK_COALESCE_ME, (i32)total);
            } else if (total >= CVM_SUPERBLOCK_LOCAL_LIST_MAX) {
                cvm_block_alloc_add_free_superblock(blocks, last_free_block_start,
                                                    total);
            } else {
                cvm_block_alloc_add_free_superblock_local(
                    blocks, result.free_superblocks, last_free_block_start, total);
            }
            last_free_block_start = NULL;
        }

        current += size;
        current_block_start += (usize)size * CVM_BLOCK_SIZE;
        line_metas += (usize)size * CVM_LINE_COUNT;
    }

    /* A run still open at the end of the batch also crosses the boundary. */
    if (last_free_block_start != NULL) {
        u32 total = (u32)(current - last_free_block_start);
        cvm_assert(total > 0);
        block_meta_set_flag_and_size(last_free_block_start, CVM_BLOCK_COALESCE_ME,
                                     (i32)total);
    }

    apply_result(&result, blocks, node);

    cvm_stats_record(cvm_stats_this_thread(), CVM_EVENT_SWEEP_BATCH, sweep_started,
                     rt_time_nanos());

    /* The coalescing pass reads these marks, so publish them first. */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(cursor_done, limit_index, memory_order_release);
}

/* ------------------------------------------------------------------ */
/* coalescing                                                          */
/* ------------------------------------------------------------------ */

/*
 * The earliest block that is not yet safely swept.
 *
 * A run held for coalescing can only be closed off once every sweeper
 * that might still extend it has finished its batch. A sweeper's
 * cursor_done is the end of its last completed batch, so the bound is the
 * smallest cursor_done among the sweepers that are still working.
 *
 * A sweeper that has stopped imposes no bound — it is not going to extend
 * anything — and when none is working at all, coalescing stays where it
 * is. That is the conservative direction: it makes no progress rather
 * than wrong progress, and the next thread to drive the sweep marks
 * itself active and carries it forward.
 */
u32 cvm_sweeper_min_cursor(CvmHeap* heap) {
    if (cvm_sweep_is_done(heap)) {
        return heap->sweep.limit;
    }

    u32 min = heap->sweep.limit;
    bool any_active = false;

    /*
     * Every mutator thread that is sweeping right now, each with its own
     * progress. This was one shared field per heap, which is what a
     * multi-threaded stress test broke: several threads sweep a batch each
     * when they cannot allocate, and a shared field lets them overwrite
     * one another. True progress is monotonic, a shared field is not, and
     * a bound computed from two threads' numbers can land past a run that
     * was never closed — after which the sweep is declared finished with a
     * coalesce_me block still in the heap.
     */
    cvm_threads_lock_read();
    CVM_THREADS_FOREACH(node) {
        if (!atomic_load_explicit(&node->sweeping, memory_order_acquire)) {
            continue;
        }
        any_active = true;
        u32 done = atomic_load_explicit(&node->sweep_cursor_done, memory_order_acquire);
        if (done < min) {
            min = done;
        }
    }
    cvm_threads_unlock_read();

    if (heap->pool != NULL) {
        for (u32 i = 0; i < heap->pool->count; i++) {
            if (!atomic_load_explicit(&heap->pool->threads[i].active,
                                      memory_order_acquire)) {
                continue;
            }
            any_active = true;
            u32 done = atomic_load_explicit(&heap->pool->threads[i].sweep_cursor_done,
                                            memory_order_acquire);
            if (done < min) {
                min = done;
            }
        }
    }

    if (!any_active) {
        return atomic_load(&heap->sweep.coalesce_done);
    }

    return min;
}

void cvm_sweep_lazy_coalesce(CvmHeap* heap) {
    u64 coalesce_started = rt_time_nanos();

    /*
     * One pass at a time. The pass is not idempotent in the direction that
     * matters: it adds runs to the block allocator's free lists before
     * advancing its start point, so two concurrent passes add the same runs
     * twice and the second finds blocks that are already free. See the note
     * on sweep.coalesce_lock.
     */
    rt_mutex_lock(&heap->sweep.coalesce_lock);
    {
        CvmThreadNode* holder = cvm_thread_node();
        atomic_store(&heap->sweep.coalesce_owner,
                     (holder != NULL) ? (usize)holder->id : 1u);
    }

    CvmBlockAllocator* blocks = heap->block_alloc;
    u32 start_index = atomic_load(&heap->sweep.coalesce_done);
    u32 limit_index = cvm_sweeper_min_cursor(heap);
    if (start_index >= limit_index) {
        atomic_store(&heap->sweep.coalesce_owner, 0);
        rt_mutex_unlock(&heap->sweep.coalesce_lock);
        return;
    }

    /* Take in the coalesce_me marks the sweepers published. */
    atomic_thread_fence(memory_order_acquire);

    BlockMeta* current = block_meta_at(heap->block_meta_start, start_index);
    BlockMeta* limit = block_meta_at(heap->block_meta_start, limit_index);
    BlockMeta* last_free_block_start = NULL;

    while (current < limit) {
        if (last_free_block_start == NULL) {
            if (block_meta_is_coalesce_me(current)) {
                last_free_block_start = current;
            }
        } else if (!block_meta_is_coalesce_me(current)) {
            /* The run ends here: it is bounded on both sides now. */
            u32 total = (u32)(current - last_free_block_start);
            cvm_block_alloc_add_free_blocks(blocks, last_free_block_start, total);
            last_free_block_start = NULL;
        }

        /* Step over whatever this block is, using its own size. */
        u32 size = 1;
        if (block_meta_is_coalesce_me(current)) {
            size = block_meta_superblock_size(current);
        } else if (block_meta_is_superblock_start(current)) {
            size = block_meta_superblock_size(current);
        } else if (block_meta_is_superblock_start_me(current)) {
            /*
             * A superblock whose free part reached into a later batch and
             * whose last block was not free after all. It is a one-block
             * superblock now; finishing it here is what the reference
             * does, and the alternative is leaving it in a state no
             * allocator understands.
             */
            block_meta_set_flag_and_size(current, CVM_BLOCK_SUPERBLOCK_START, 1);
        }

        current += size;
    }

    if (last_free_block_start != NULL) {
        /*
         * The run reaches the coalescing limit. If it ends exactly at the
         * limit it is bounded and can be published; if a superblock
         * crossed the limit, only the part inside is published and the
         * remainder is marked for the next pass.
         */
        if (current == limit) {
            u32 total = (u32)(limit - last_free_block_start);
            cvm_block_alloc_add_free_blocks(blocks, last_free_block_start, total);
        } else {
            u32 total = (u32)(limit - last_free_block_start);
            u32 remaining = (u32)(current - limit);
            cvm_assert(remaining > 0);

            block_meta_set_flag_and_size(limit, CVM_BLOCK_COALESCE_ME, (i32)remaining);
            /* Other sweepers must see that before it is acted on. */
            atomic_thread_fence(memory_order_seq_cst);

            if (total > 0) {
                cvm_block_alloc_add_free_blocks(blocks, last_free_block_start, total);
            }

            /*
             * Try to move the sweep cursor past the superblock, so no
             * sweeper wastes a batch on its interior.
             */
            u32 advance_to = block_meta_index_of(heap->block_meta_start, current);
            u32 cursor = atomic_load(&heap->sweep.cursor);
            while (cursor < advance_to) {
                if (atomic_compare_exchange_weak(&heap->sweep.cursor, &cursor,
                                                 advance_to)) {
                    break;
                }
            }
        }
    }

    atomic_store(&heap->sweep.coalesce_done, limit_index);

    cvm_stats_record(cvm_stats_this_thread(), CVM_EVENT_COALESCE_BATCH,
                     coalesce_started, rt_time_nanos());

    atomic_store(&heap->sweep.coalesce_owner, 0);
    rt_mutex_unlock(&heap->sweep.coalesce_lock);
}

bool cvm_sweep_is_done(CvmHeap* heap) {
    return atomic_load_explicit(&heap->sweep.post_sweep_done, memory_order_acquire);
}

/*
 * Whether any thread other than the caller is inside a sweep batch.
 *
 * A batch is not atomic: it claims a range, walks it, and only then files
 * its results. A thread that finished a collection while another was still
 * inside a batch would let that straggler file blocks into a mutator's
 * recycled list *after* the sweep was declared over — and the next sweep
 * then re-sweeps those blocks while they are in use, so the same block can
 * be handed to two threads. A multi-threaded stress test caught exactly
 * that, as two threads writing into overlapping object memory.
 */
static bool any_other_sweeper_is_in_flight(void) {
    bool busy = false;

    cvm_threads_lock_read();
    CVM_THREADS_FOREACH(node) {
        if (atomic_load_explicit(&node->sweeping, memory_order_acquire)) {
            busy = true;
            break;
        }
    }
    cvm_threads_unlock_read();

    return busy;
}

bool cvm_sweep_try_finish(CvmHeap* heap) {
    if (atomic_load(&heap->sweep.cursor) < heap->sweep.limit ||
        atomic_load(&heap->sweep.coalesce_done) < heap->sweep.limit) {
        return false;
    }

    /*
     * Not while somebody is still filing a batch's results. The caller has
     * already cleared its own flag, so this asks about everyone else.
     */
    if (any_other_sweeper_is_in_flight()) {
        return false;
    }

    /*
     * Claim the finishing work. Two threads reaching the end at once must
     * not both take a reserve: the second would hand back blocks the first
     * had just set aside for the sweeper.
     */
    bool expected = false;
    if (!atomic_compare_exchange_strong(&heap->sweep.finishing, &expected, true)) {
        return false;
    }

    /*
     * Take a run to hold back, then leave concurrent mode. The reserve
     * matters because the free lists may be empty at this instant: a
     * mutator resuming with nothing free needs somewhere to start without
     * sweeping first.
     */
    cvm_block_alloc_reserve_blocks(heap->block_alloc);
    cvm_block_alloc_finish_coalescing(heap->block_alloc);

    /* Published last, so a thread that sees it also sees the reserve. */
    atomic_store_explicit(&heap->sweep.post_sweep_done, true, memory_order_release);
    return true;
}

bool cvm_sweep_drive(CvmHeap* heap, CvmThreadNode* node, u32 max_count) {
    if (cvm_sweep_is_done(heap)) {
        return true;
    }

    /*
     * This thread's own sweep record, published for the whole call,
     * coalescing included. Counting it is what lets the final batch finish
     * the sweep: coalescing is bounded by the smallest progress among the
     * sweepers still working, and a thread that did not count itself during
     * its own coalescing call would leave no bound at all, so coalescing
     * would be held at the last block forever.
     */
    CvmThreadNode* self = (node != NULL) ? node : cvm_thread_node();
    if (self == NULL) {
        /*
         * Not a registered thread: a GC worker, or teardown. Workers track
         * their own progress in the pool, and nothing else should be
         * driving the sweep from here.
         */
        cvm_sweep_lazy_coalesce(heap);
        return cvm_sweep_try_finish(heap);
    }

    /*
     * The claim's starting point, not the finished one. Everything before
     * the unclaimed cursor has been swept by somebody, so publishing that
     * as the bound is correct even before this batch finishes — and unlike
     * the end of the batch, it cannot land ahead of a run that is still
     * open.
     */
    u32 claim = atomic_load(&heap->sweep.cursor);
    if (claim > heap->sweep.limit) {
        claim = heap->sweep.limit;
    }
    atomic_store(&self->sweep_cursor_done, claim);
    atomic_store(&self->sweeping, true);
    cvm_thread_set_activity(CVM_ACT_SWEEPING);

    if (claim < heap->sweep.limit) {
        cvm_sweep(heap, &self->sweep_cursor_done, max_count, self);
    }

    cvm_thread_set_activity(CVM_ACT_COALESCING);
    cvm_sweep_lazy_coalesce(heap);

    atomic_store(&self->sweeping, false);
    cvm_thread_set_activity(CVM_ACT_IDLE);

    return cvm_sweep_try_finish(heap);
}

/* ------------------------------------------------------------------ */
/* consistency checks                                                  */
/* ------------------------------------------------------------------ */

#if defined(CVM_DEBUG)

void cvm_sweep_clear_debug_state(CvmHeap* heap) {
    CvmBlockAllocator* blocks = heap->block_alloc;
    BlockMeta* reserve_first =
        atomic_load_explicit(&blocks->reserved, memory_order_relaxed);
    BlockMeta* reserve_limit =
        (reserve_first != NULL) ? reserve_first + CVM_SWEEP_RESERVE_BLOCKS : NULL;

    for (u32 i = 0; i < heap->block_count; i++) {
        BlockMeta* meta = block_meta_at(heap->block_meta_start, i);
        if (reserve_first != NULL && meta >= reserve_first && meta < reserve_limit) {
            continue;
        }
        block_meta_set_debug_state(meta, CVM_DBG_MUST_SWEEP);
    }
}

void cvm_sweep_assert_consistent(CvmHeap* heap) {
    /*
     * On finding a block in a state the sweep should have resolved, say
     * where and in what circumstances before giving up. "A coalesce_me block
     * survived the sweep" without the index, the cursor and the coalesce
     * cursor is a sentence that costs an hour to act on; with them it names
     * the run that was not closed off and how far each pass had got.
     */
#define CVM_SWEEP_REPORT_AND_FAIL(cond, message)                                       \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            CVM_LOG_ERROR(m) {                                                         \
                fmt_lit(&m.fmt, message " at block ");                                 \
                fmt_u32(&m.fmt, i);                                                    \
                fmt_lit(&m.fmt, " (flags=0x");                                         \
                fmt_hex(&m.fmt, (u64)meta->block.simple.flags);                        \
                fmt_lit(&m.fmt, " size=");                                             \
                fmt_u32(&m.fmt, block_meta_superblock_size(meta));                     \
                fmt_lit(&m.fmt, ") cursor=");                                          \
                fmt_u32(&m.fmt, atomic_load(&heap->sweep.cursor));                     \
                fmt_lit(&m.fmt, " coalesce=");                                         \
                fmt_u32(&m.fmt, atomic_load(&heap->sweep.coalesce_done));              \
                fmt_lit(&m.fmt, " limit=");                                            \
                fmt_u32(&m.fmt, heap->sweep.limit);                                    \
                fmt_lit(&m.fmt, " done=");                                             \
                fmt_bool(&m.fmt, cvm_sweep_is_done(heap));                             \
            }                                                                          \
            cvm_assert_msg(cond, message);                                             \
        }                                                                              \
    } while (0)

    for (u32 i = 0; i < heap->block_count;) {
        BlockMeta* meta = block_meta_at(heap->block_meta_start, i);

        CVM_SWEEP_REPORT_AND_FAIL(!block_meta_is_coalesce_me(meta),
                                  "a coalesce_me block survived the sweep");
        CVM_SWEEP_REPORT_AND_FAIL(!block_meta_is_superblock_start_me(meta),
                                  "a superblock_start_me block survived the sweep");
        CVM_SWEEP_REPORT_AND_FAIL(!block_meta_is_superblock_tail(meta),
                                  "a superblock tail was reached directly, so "
                                  "its head was not swept");
        CVM_SWEEP_REPORT_AND_FAIL(!block_meta_is_marked(meta),
                                  "a block is still marked after sweeping");

        u32 size = 1;
        if (block_meta_is_superblock_start(meta)) {
            size = block_meta_superblock_size(meta);
        }

        for (u32 b = 0; b < size; b++) {
            LineMeta* lines = cvm_heap_line_meta(heap, i + b);
            for (u32 l = 0; l < CVM_LINE_COUNT; l++) {
                if (line_meta_is_marked(&lines[l])) {
                    CVM_LOG_ERROR(m) {
                        fmt_lit(&m.fmt, "a line is still marked after sweeping: "
                                        "block ");
                        fmt_u32(&m.fmt, i + b);
                        fmt_lit(&m.fmt, " line ");
                        fmt_u32(&m.fmt, l);
                    }
                    cvm_assert_msg(false, "a line is still marked after sweeping");
                }
            }
        }

        for (u32 b = 0; b < size; b++) {
            u8* block_start = cvm_heap_block_address(heap, i + b);
            ObjectMeta* slot = bytemap_get(heap->bytemap, block_start);
            for (usize s = 0; s < CVM_SLOTS_PER_BLOCK; s++) {
                if (object_meta_is_marked(&slot[s])) {
                    CVM_LOG_ERROR(m) {
                        fmt_lit(&m.fmt, "an object is still marked after sweeping: "
                                        "block ");
                        fmt_u32(&m.fmt, i + b);
                        fmt_lit(&m.fmt, " slot ");
                        fmt_u32(&m.fmt, (u32)s);
                    }
                    cvm_assert_msg(false, "an object is still marked after sweeping");
                }
            }
        }

        i += size;
    }
}

#endif /* CVM_DEBUG */
