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
#include "base/swar.h"
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

            /* The flag rides in the slot byte, which this loop is already
             * holding: see the note on CvmGcHeader. */
            if (object_meta_has_finalizer(slot)) {
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

            if (object_meta_has_finalizer(slot)) {
                CvmExternal* ext = (CvmExternal*)(addr + sizeof(CvmGcHeader));
                (void)cvm_finalizer_enqueue(ext->c_ptr, ext->finalizer);
            }
            object_meta_set_free(slot);
        } else if (object_meta_is_marked(slot)) {
            object_meta_set_allocated(slot);
        } else if (object_meta_is_end(slot)) {
            /*
             * The end of an object that reached into this line, and the line
             * survived because of it: it stays, because the next sweep reads
             * extents out of these. A dead object's end marker inside a kept
             * line is left where it is -- the allocator never bumps into a
             * kept line, so nothing can be allocated over it, and the line's
             * slots are cleared in bulk when the line itself is freed.
             */
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
/*
 * Sweep one simple block.
 *
 * The heap, not a thread, owns the metadata this walks: the bytemap and the
 * block table are heap-level objects, and a thread's allocator only caches a
 * pointer to them. Taking them from the thread was harmless while every
 * sweeper was a mutator, and broke the moment a GC worker started sweeping
 * without one — it read a bytemap pointer out of a NULL node. So `target` is
 * now only about *ownership of the result*: which thread's recycled list the
 * partially-free block lands on. It may be NULL, and the block is swept
 * anyway. Skipping the sweep for want of an owner was the other shape of the
 * same mistake: the block stays described as in use, and the symptom is a
 * collection that reclaims nothing.
 */
/*
 * How many slots the object starting at `index` covers.
 *
 * Read out of the metadata table, which is the point: the marker does not
 * need it any more (it marks the object and its block and nothing else) and
 * the sweeper must not read the object -- it runs while the program does, and
 * the object and its descriptor are the coldest lines it could touch. Whippet
 * records the same thing in its mark byte for the same reason: "finding holes
 * in a block can just read the mark table and can avoid looking at object
 * memory" (doc/collector-mmc.md).
 *
 * The fallback is the descriptor, for an object whose end marker is not
 * there. That should not happen -- every line-managed allocation writes one
 * -- but a wrong extent here frees a live object's tail, so the slow path is
 * preferred to a guess. The scan is bounded by the block because an object
 * never spans one.
 */
static u32 object_slots_at(u8* block_start, ObjectMeta* slots, u32 index) {
    for (u32 i = index + 1; i < CVM_SLOTS_PER_BLOCK; i++) {
        if (object_meta_is_end(&slots[i])) {
            return i - index + 1;
        }
    }

    u8* addr = block_start + (usize)index * CVM_ALLOC_ALIGNMENT;
    return (u32)(cvm_object_size((CvmGcHeader const*)addr) / CVM_ALLOC_ALIGNMENT);
}

/*
 * Which lines the survivors in this block reach.
 *
 * This is what the marker used to do, and moving it here is the point of the
 * exercise: it is a pass over the block's metadata, which the sweep has time
 * for and the pause does not. A line is kept because some object that
 * survived covers part of it -- including the lines after the first, which is
 * why the object's end has to be readable from the table rather than assumed
 * from a mark on its first line.
 */
/*
 * Which slots this pass does work at.
 *
 * A live object's start is MARKED (0x3) or, for a large object, PLACEHOLDER
 * (0x1) -- both carry bit 0. A dead object's start is ALLOCATED (0x2), which
 * does not, and an END marker (0x4) does not either. So bit 0 is "the start
 * of something that survives", and a scan for it skips everything else --
 * the dead, and the interior slots of every object, live or dead -- eight at
 * a time.
 *
 * This is whippet's usage of the same table: finding object boundaries and
 * live slots by bits in the mark table, not an emptiness test. The first
 * version of this pass tried the emptiness test and got it wrong (the
 * any-byte-zero idiom reads a group holding one mark and seven empties as
 * empty), and the second tried skipping empty groups by alignment and
 * measured nothing, because a dense block has no empty groups to skip. The
 * third scanned from every slot and lost 20% on the dense-all-live block,
 * because the scan's fixed work is several times a byte compare and a live
 * object has nothing to skip past. This one scans only out of dead runs,
 * where there is something to skip, and walks live objects byte-wise.
 */
#define CVM_SLOT_LIVE_START ((u8)0x1)

static void mark_survivors_lines(u8* block_start, ObjectMeta* slots,
                                 LineMeta* line_metas) {
    u32 i = 0;
    while (i < CVM_SLOTS_PER_BLOCK) {
        u8 state = object_meta_state(&slots[i]);

        if (state == (u8)CVM_OM_MARKED) {
            u32 count = object_slots_at(block_start, slots, i);
            cvm_assert(count > 0);
            cvm_assert(i + count <= CVM_SLOTS_PER_BLOCK);

            for (u32 slot = i; slot < i + count; slot++) {
                LineMeta* line = &line_metas[slot / CVM_SLOTS_PER_LINE];
                if (!line_meta_is_marked(line)) {
                    line_meta_mark(line);
                }
            }
            i += count;
        } else if (state == (u8)CVM_OM_PLACEHOLDER) {
            /* The interior of a large object: its extent is the chunk's, at
             * large-object granularity, and nothing here is line-managed. */
            i += CVM_LARGE_OBJECT_MIN_SIZE / CVM_ALLOC_ALIGNMENT;
        } else if (state == (u8)CVM_OM_END) {
            /*
             * The last slot of an object. What follows it is the next slot in
             * the block -- a start, or free -- and one byte of walking is
             * cheaper than asking the scan to skip it: the second version of
             * this pass scanned from here too, and a dense-all-live block
             * (read-heavy, an END under every survivor) paid the scan's fixed
             * work two hundred thousand times a collection for a one-byte
             * answer.
             */
            i++;
        } else {
            /*
             * Dead territory: the start of an object that did not survive
             * (ALLOCATED), or a free slot. The next slot that is the start of
             * anything live has bit 0, and the scan finds it eight bytes at a
             * time -- which is the whole point of doing this at all. A block
             * that is mostly dead -- the shape a heap about to be
             * half-reclaimed has -- walks its dead runs at a load per eight
             * slots instead of a load per slot.
             *
             * A dense block of survivors never takes this branch: live starts
             * are handled above and ENDs one byte at a time above that, so
             * nothing pays for the scan existing except the runs it exists to
             * skip.
             *
             * The scan cannot return 0 from here: this slot has no bit 0, so
             * byte i never matches, and progress is guaranteed.
             */
            usize left = CVM_SLOTS_PER_BLOCK - (usize)i;
            i += (u32)rt_swar_scan_for_byte_with_bits((u8 const*)&slots[i], left,
                                                      CVM_SLOT_LIVE_START);
        }
    }
}

static u32 cvm_sweep_simple_block(CvmHeap* heap, CvmThreadNode* target,
                                  BlockMeta* block_meta, u8* block_start,
                                  LineMeta* line_metas, CvmSweepResult* result) {
    Bytemap* bytemap = heap->bytemap;
    cvm_assert(block_meta_debug_state(block_meta) == (u8)CVM_DBG_MUST_SWEEP);

    if (!block_meta_is_marked(block_meta)) {
        /*
         * Nothing survived, so the whole block is free — but every object
         * in it died here, and any of them may own something outside the
         * heap. This bulk path is the reason the block needs a finalizer
         * flag at all: it clears the metadata in one go and would never
         * look at an individual object.
         */
        extract_finalizers_if_flagged(bytemap, block_meta, block_start,
                                      block_end(block_start));

        block_meta_clear(block_meta);
        block_meta_set_debug_state(block_meta, CVM_DBG_FREE);

        /*
         * The whole block is dead, so the whole block is cleared, once.
         * This is the clearing that used to happen per object on the
         * allocation path; here it is one call per 32 KiB block, and the
         * mutator that later adopts the block finds it already zero.
         *
         * After the finalizers, because extracting one reads the object it
         * belongs to, and before the metadata is dropped, so that a reader
         * of the bytemap cannot observe a block that is free but still
         * full of a dead program's data.
         */
        rt_mem_zero((opaque_mut)block_start, CVM_BLOCK_SIZE);

        object_meta_clear_block(bytemap_get(bytemap, block_start));
        return 1;
    }

    cvm_assert(block_meta_is_marked(block_meta));
    block_meta_unmark(block_meta);

    mark_survivors_lines(block_start, bytemap_get(bytemap, block_start), line_metas);

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

            /*
             * Clear the run, then write its node back on top.
             *
             * One call for the whole run rather than one per line, and it
             * has to be here rather than at first-touch: `last_recyclable`
             * is the run's first line, the node lives in that line, and a
             * node written before the clear would be cleared away.
             */
            rt_mem_zero((opaque_mut)last_recyclable, (usize)size * CVM_LINE_SIZE);

            last_recyclable->size = size;
        }
    }

    if (last_recyclable != NULL) {
        /* Some lines are free, so the block is worth keeping. */
        last_recyclable->next = CVM_LAST_HOLE;

        cvm_assert(block_meta_first_free_line(block_meta) >= 0);
        cvm_assert((i32)block_meta_first_free_line(block_meta) < CVM_LINE_INDEX_LIMIT);

        /*
         * The count belongs to whoever will allocate from this block. With no
         * owner there is nothing to count for — the block still goes on the
         * batch's list, and apply_result decides where it lands.
         */
        if (target != NULL) {
            atomic_fetch_add_explicit(&target->allocator.recycled_block_count, 1,
                                      memory_order_relaxed);
        }
        block_meta_set_debug_state(block_meta, CVM_DBG_PARTIAL_FREE);

        /*
         * The mutator that takes this block reads the free-line list it
         * just had written into it, so the writes must be visible before
         * the block is. The release fence orders them; the acquire on the
         * reader's side is in the allocator's new-block path.
         */
        atomic_thread_fence(memory_order_release);
        local_block_list_push(&result->recycled_blocks, heap->block_meta_start,
                              block_meta);
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
static u32 cvm_sweep_superblock(CvmHeap* heap, BlockMeta* block_meta, u8* block_start,
                                BlockMeta* batch_limit) {
    u32 superblock_size = block_meta_superblock_size(block_meta);
    u8* block_end = block_start + (usize)superblock_size * CVM_BLOCK_SIZE;

    ObjectMeta* first_object = bytemap_get(heap->bytemap, block_start);
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

        /*
         * Those blocks are the body of a dead large object, and they are
         * cleared the way a wholly dead simple block is: once for the run,
         * because they are contiguous and none of them is live. The last
         * block of the superblock is a different matter -- something may
         * have survived in it -- and is not touched here.
         */
        rt_mem_zero((opaque_mut)block_start, (usize)free_count * CVM_BLOCK_SIZE);
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
    ObjectMeta* current_meta = bytemap_get(heap->bytemap, current);

    while (current < block_end) {
        if (chunk_start == NULL) {
            if (object_meta_is_allocated(current_meta) ||
                object_meta_is_placeholder(current_meta)) {
                chunk_start = current;
            }
        } else if (object_meta_is_marked(current_meta)) {
            /* A survivor begins here, so the run ends just before it. */
            usize dead = (usize)(current - chunk_start);
            rt_mem_zero((opaque_mut)chunk_start, dead);
            cvm_large_add_chunk(heap, (CvmChunk*)chunk_start, dead);
            chunk_start = NULL;
        }
        object_meta_sweep(current_meta);

        current += CVM_LARGE_OBJECT_MIN_SIZE;
        current_meta += CVM_LARGE_OBJECT_MIN_SIZE / CVM_ALLOC_ALIGNMENT;
    }

    if (chunk_start == last_block_start) {
        /*
         * The whole last block is one dead chunk, so it is a free block.
         *
         * This is the common case for a superblock that is one block long,
         * which is what a single large object gets, and it is a reclaim
         * site like any other: the block goes on the free list, so its
         * payload has to be zero before it does. The front blocks above are
         * cleared only when the object spanned more than one, so this
         * branch is not covered by that one.
         */
        free_count += 1;
        rt_mem_zero((opaque_mut)last_block_start, CVM_BLOCK_SIZE);
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
            object_meta_set_placeholder(bytemap_get(heap->bytemap, last_block_start));
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
            /* Cleared before it is filed: the free-list link is written
             * into the chunk itself, so a clear afterwards would erase it. */
            usize dead = (usize)(current - chunk_start);
            rt_mem_zero((opaque_mut)chunk_start, dead);
            cvm_large_add_chunk(heap, (CvmChunk*)chunk_start, dead);
        }
    }

    return free_count;
}

/* ------------------------------------------------------------------ */
/* choosing where recycled blocks go                                   */

/* ------------------------------------------------------------------ */
/* applying a batch's results                                          */
/* ------------------------------------------------------------------ */

static void apply_result(CvmSweepResult* result, CvmBlockAllocator* blocks,
                         CvmThreadNode* node) {
    /*
     * Who takes the partially free blocks.
     *
     * A mutator sweeps for itself and keeps them in its own list. A worker
     * has no list, so its blocks go to the shared stack instead — that is
     * what the stack is for, and handing them to a mutator by round-robin
     * would mean touching that mutator's node after the registry's read
     * lock was released, which is the window in which the thread leaves and
     * its node is freed underneath the write.
     *
     * Before this, a worker's swept blocks were simply dropped: not handed
     * to anyone, not counted as free, not swept to completion. Every
     * collection leaked every partially free block a worker touched, the
     * growth check then read a free ratio that stayed low, and the heap
     * grew without bound — 529 MiB for a 6 MB live set was the visible
     * number. That was not "a leak of address space, not of correctness";
     * it was the heap policy being fed a lie.
     */
    BlockMeta* first = result->recycled_blocks.first;
    if (first != NULL) {
        if (node != NULL) {
            block_list_push_all(&node->allocator.recycled_blocks,
                                node->allocator.heap->block_meta_start, first,
                                result->recycled_blocks.last);
        } else {
            u32 count = 0;
            for (BlockMeta* b = first; b != NULL;
                 b = block_meta_next(blocks->meta_start, b)) {
                count++;
            }
            block_list_push_all(&blocks->recycled_shared, blocks->meta_start, first,
                                result->recycled_blocks.last);
            atomic_fetch_add_explicit(&blocks->recycled_shared_count, count,
                                      memory_order_release);
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

/* ------------------------------------------------------------------ */
/* sweeping a batch                                                    */
/* ------------------------------------------------------------------ */

void cvm_sweep_begin(CvmHeap* heap) {
    /*
     * A sweep only starts once the previous one is finished, and finished
     * means no thread is still inside a batch — see
     * cvm_sweep_any_in_flight. Resetting the cursor while a straggler
     * held a batch would let it hand blocks to a mutator that the new sweep
     * is about to describe again.
     */
    cvm_assert(!cvm_sweep_any_in_flight(heap));

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
            free_count = cvm_sweep_simple_block(
                heap, node, current, current_block_start, line_metas, &result);
        } else if (block_meta_is_superblock_start(current)) {
            size = block_meta_superblock_size(current);
            cvm_assert(size > 0);

            free_count =
                cvm_sweep_superblock(heap, current, current_block_start, limit);
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

        /* See below: the marker and the bound, or nobody takes this run in. */
        atomic_thread_fence(memory_order_release);
    }

    apply_result(&result, blocks, node);

    cvm_stats_record(cvm_stats_this_thread(), CVM_EVENT_SWEEP_BATCH, sweep_started,
                     rt_time_nanos());

    /* The coalescing pass reads these marks, so publish them first. */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(cursor_done, limit_index, memory_order_release);

    if (last_free_block_start != NULL) {
        /*
         * The run is marked and the bound is published. Now wake a thread to
         * do the coalescing, because nobody else is going to: the mutators
         * that would sweep on their next allocation may not allocate for a
         * while, and a run that sits unclosed is what the consistency check
         * reports as a coalesce_me block surviving the sweep.
         *
         * Posted rather than run here because the call site is the bottom of
         * a sweep, which is already holding no other lock — and coalescing
         * takes one.
         */
        if (heap->pool != NULL) {
            cvm_pool_wake(heap->pool, CVM_GC_COALESCE, 1);
        } else if (cvm_thread_node() != NULL) {
            /* No pool: this thread coalesces its own run before moving on. */
            cvm_sweep_lazy_coalesce(heap);
        }
    }
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
            if (!atomic_load_explicit(&heap->pool->threads[i].sweeping,
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
bool cvm_sweep_any_in_flight(CvmHeap* heap) {
    bool busy = false;

    cvm_threads_lock_read();
    CVM_THREADS_FOREACH(node) {
        if (atomic_load_explicit(&node->sweeping, memory_order_acquire)) {
            busy = true;
            break;
        }
    }
    cvm_threads_unlock_read();

    if (busy) {
        return true;
    }

    /*
     * Workers are not registered threads, so the loop above cannot see them,
     * and they sweep: the collector hands them the sweep after resuming the
     * world. Asking only the registry would let the last mutator declare the
     * sweep finished while a worker is halfway through a batch — and then the
     * next collection's cvm_sweep_begin would reset the cursor under that
     * batch, which is how a block gets handed to a mutator and described
     * again in the same round.
     *
     * Workers carry a flag of their own rather than reusing `active`, which
     * says "doing some phase" and would answer yes for a worker tracing a
     * grey packet. Getting this wrong is not subtle: the first version used
     * `active`, and cvm_sweep_begin's assertion below fired within a handful
     * of runs of test_pool.
     */
    if (heap->pool != NULL) {
        for (u32 i = 0; i < heap->pool->count; i++) {
            if (atomic_load_explicit(&heap->pool->threads[i].sweeping,
                                     memory_order_acquire)) {
                return true;
            }
        }
    }

    return false;
}

bool cvm_sweep_handoff_pending(CvmHeap* heap) {
    /*
     * A sweep handoff posted and not yet consumed. Separate from
     * cvm_sweep_any_in_flight because the two questions have different
     * answers, and using one predicate for both was wrong in both directions:
     *
     *   - Beginning a new sweep does not have to wait for an arrival. A worker
     *     that gets here late looks at the cursor, finds the sweep already
     *     over, and does nothing — that is the correct response to arriving
     *     late, and making it wait would stall every collection on a handoff
     *     that is about to turn into nothing.
     *
     *   - Declaring a sweep finished does have to wait for one, and the
     *     failure is not subtle. Without this test the collector finished a
     *     sweep with a run still held back for coalescing, the next collection
     *     began, and the consistency check reported the leftover as a
     *     coalesce_me block surviving a sweep.
     *
     * Unqualified by phase: every post is retired by whoever consumes it,
     * whatever it then does, so this always falls to zero and cannot wedge the
     * question at "never".
     */
    return heap->pool != NULL &&
           atomic_load_explicit(&heap->pool->wake_pends, memory_order_acquire) > 0;
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
    if (cvm_sweep_any_in_flight(heap) || cvm_sweep_handoff_pending(heap)) {
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

    /*
     * Clear this thread's published bound now that it is no longer sweeping.
     *
     * `cvm_sweep_begin` says each thread clears its own progress when it stops,
     * and this path was the one that did not — so the number left behind was the
     * end of the last batch of the *previous* sweep. The next sweep starts its
     * cursor at zero, and the claim assertion then compared a fresh
     * start_index against a bound from a sweep that was over.
     *
     * After the withdrawal, deliberately: a reader that filters on `sweeping`
     * cannot see this value, so it cannot be mistaken for progress. A zero
     * published while the flag was still set would instead hold coalescing back,
     * which is the safe direction but not free.
     */
    atomic_store(&self->sweep_cursor_done, 0);
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

/*
 * Drive the sweep to its end, or to the point where this thread can do no
 * more.
 *
 * Used from inside a collection, where the world is stopped, from just
 * after one, where it is not — which is why "am I the only sweeper?" cannot
 * be assumed and "no progress" cannot simply end the loop. A worker holding
 * the remaining work makes the cursor stand still while it works, and a
 * handoff that has been posted but not consumed stands the cursor still
 * before that. Breaking on either would abandon a sweep that is perfectly
 * alive: the visible symptom was a collection that reported most of its heap
 * still in use, because the blocks had not been swept rather than because
 * they held live objects.
 *
 * So the escape hatch is conditioned on there being nobody who could still
 * be working. When there is, yielding is not hope: a sweep batch is a bounded
 * loop and a handoff is consumed by a thread already blocked on a semaphore.
 */
void cvm_sweep_finish(CvmHeap* heap, str const where) {
    while (!cvm_sweep_is_done(heap)) {
        u32 cursor_before = atomic_load(&heap->sweep.cursor);
        u32 coalesce_before = atomic_load(&heap->sweep.coalesce_done);

        cvm_sweep_drive(heap, cvm_thread_node(), CVM_SWEEP_BATCH_SIZE);

        if (atomic_load(&heap->sweep.cursor) == cursor_before &&
            atomic_load(&heap->sweep.coalesce_done) == coalesce_before) {
            if (cvm_sweep_any_in_flight(heap) || cvm_sweep_handoff_pending(heap)) {
                rt_thread_yield();
                continue;
            }

            CVM_LOG_DEBUG(m) {
                fmt_lit(&m.fmt, "sweep left unfinished ");
                fmt_str(&m.fmt, where);
                fmt_lit(&m.fmt, ": cursor ");
                fmt_u32(&m.fmt, atomic_load(&heap->sweep.cursor));
                fmt_lit(&m.fmt, "/");
                fmt_u32(&m.fmt, heap->sweep.limit);
            }
            break;
        }
    }
}
