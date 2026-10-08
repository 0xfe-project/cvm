/*
 * vm/gc/heap.c
 *
 * Heap layout and growth. See vm/gc/heap.h for the shape of the thing.
 */

#include "gc/heap.h"

#include <stdatomic.h>

#include "base/math.h"
#include "base/types.h"
#include "context.h"
#include "gc/bytemap.h"
#include "gc/layout.h"
#include "gc/meta.h"
#include "log.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* reservations                                                        */
/* ------------------------------------------------------------------ */

bool cvm_region_reserve(CvmRegion* region, usize bytes, usize align) {
    region->base = NULL;
    region->size = 0;
    region->start = NULL;
    region->usable = 0;

    if (bytes == 0) {
        return true; /* an empty region is not a failure */
    }

    /*
     * Ask for an extra `align - 1` bytes so that aligning the start up
     * still leaves the full `bytes` usable. Without this a 32 KiB block
     * alignment could cost a whole block of the reservation.
     */
    usize request = bytes + align - 1;
    u8* base = rt_mem_reserve(request);
    if (base == NULL) {
        return false;
    }
    if (!rt_mem_commit(base, request)) {
        rt_mem_release(base, request);
        return false;
    }

    u8* start = (u8*)cvm_align_up((usize)base, align);

    region->base = base;
    region->size = request;
    region->start = start;
    region->usable = bytes;
    return true;
}

void cvm_region_release(CvmRegion* region) {
    if (region->base != NULL) {
        rt_mem_release(region->base, region->size);
    }
    region->base = NULL;
    region->size = 0;
    region->start = NULL;
    region->usable = 0;
}

/* ------------------------------------------------------------------ */
/* sizing                                                              */
/* ------------------------------------------------------------------ */

/*
 * How much metadata the whole reservation needs, for the maximum block
 * count. Metadata is sized for the maximum and its end pointer moves as
 * the heap grows, so a growing heap never relocates metadata — which
 * matters because the tables are addressed by arithmetic on their base.
 */
static usize block_meta_bytes(u32 max_blocks) {
    return (usize)max_blocks * sizeof(BlockMeta);
}

static usize line_meta_bytes(u32 max_blocks) {
    return (usize)max_blocks * CVM_LINE_COUNT * sizeof(LineMeta);
}

static usize bytemap_bytes(u32 max_blocks) {
    /* one slot per allocation granularity, over the whole payload */
    usize payload = (usize)max_blocks * CVM_BLOCK_SIZE;
    return bytemap_bytes_for(payload);
}

/*
 * How many grey packets the heap gets.
 *
 * A fraction of the heap size, so a bigger heap has more marking
 * parallelism available without the fraction being tuned per workload.
 * The reference uses the same ratio.
 */
static u32 grey_packet_count_for(u32 max_blocks) {
    usize heap_bytes = (usize)max_blocks * CVM_BLOCK_SIZE;
    usize count =
        (usize)((f64)heap_bytes * CVM_GREY_PACKET_RATIO) / CVM_GREY_PACKET_SIZE;
    if (count == 0) {
        count = 1;
    }
    return (u32)count;
}

/*
 * A block's payload plus its share of the metadata is what a block really
 * costs, and that is the figure block counts have to be derived from:
 * sizing from the payload alone would under-reserve the metadata.
 */
static u32 clamp_blocks(usize blocks) {
    if (blocks == 0) {
        blocks = 1;
    }
    if (blocks > (usize)CVM_MAX_BLOCK_COUNT) {
        blocks = (usize)CVM_MAX_BLOCK_COUNT;
    }
    return (u32)blocks;
}

/*
 * The two directions are not symmetric, and getting them the wrong way
 * round is a bug either way:
 *
 *   - the maximum must round down, or the reservation would exceed what
 *     was asked for;
 *   - the initial count must round up, or the heap would start slightly
 *     below the requested minimum. A block is 34956 bytes including
 *     metadata, so 1 MiB is 29.99 blocks: rounding down gives 29 blocks
 *     and a heap 34 KiB short of the minimum.
 */
static u32 blocks_for_max(usize bytes) {
    return clamp_blocks(bytes / cvm_heap_space_per_block());
}

static u32 blocks_for_min(usize bytes) {
    usize per_block = cvm_heap_space_per_block();
    return clamp_blocks(cvm_div_round_up(bytes, per_block));
}

/* ------------------------------------------------------------------ */
/* init                                                                */
/* ------------------------------------------------------------------ */

bool cvm_heap_init(CvmHeap* heap, usize min_size, usize max_size) {
    /* Start from a clean slate so a failed init leaves nothing half-set. */
    heap->blocks = (CvmRegion){0};
    heap->grey_packets_region = (CvmRegion){0};
    heap->block_meta_region = (CvmRegion){0};
    heap->line_meta_region = (CvmRegion){0};
    heap->bytemap_region = (CvmRegion){0};
    heap->heap_start = NULL;
    atomic_store(&heap->heap_end, NULL);
    heap->heap_size = 0;
    heap->block_meta_start = NULL;
    heap->block_meta_end = NULL;
    heap->line_meta_start = NULL;
    heap->line_meta_end = NULL;
    heap->bytemap = NULL;
    heap->block_count = 0;
    heap->max_block_count = 0;
    heap->max_heap_size = 0;
    heap->max_mark_time_ratio = CVM_MARK_TIME_RATIO_DEFAULT;
    heap->min_free_ratio = CVM_FREE_RATIO_DEFAULT;
    heap->pool = NULL;
    heap->block_alloc = NULL;

    if (min_size < CVM_MIN_HEAP_SIZE) {
        min_size = CVM_MIN_HEAP_SIZE;
    }

    /* 0 means "unlimited": take as much as the reservation will allow. */
    if (max_size == 0 || max_size == CVM_UNLIMITED_HEAP_SIZE) {
        max_size = CVM_MAX_HEAP_SIZE;
    }
    if (max_size < min_size) {
        return false;
    }

    u32 max_blocks = blocks_for_max(max_size);
    u32 initial_blocks = blocks_for_min(min_size);
    if (initial_blocks == 0) {
        initial_blocks = 1;
    }
    if (max_blocks <= 1) {
        /* A one-block heap cannot hold a superblock, so nothing above the
         * large-object threshold could ever be allocated. */
        return false;
    }

    if (!rt_mutex_init(&heap->grow_lock) ||
        !rt_mutex_init(&heap->sweep.coalesce_lock)) {
        return false;
    }

    /*
     * Reserve every region before committing to any of them, so that a
     * failure part-way through releases everything rather than leaking
     * the earlier reservations.
     */
    bool ok = cvm_region_reserve(&heap->block_meta_region, block_meta_bytes(max_blocks),
                                 _Alignof(BlockMeta)) &&
              cvm_region_reserve(&heap->line_meta_region, line_meta_bytes(max_blocks),
                                 _Alignof(LineMeta)) &&
              cvm_region_reserve(&heap->bytemap_region, bytemap_bytes(max_blocks),
                                 CVM_ALLOC_ALIGNMENT) &&
              cvm_region_reserve(&heap->grey_packets_region,
                                 (usize)grey_packet_count_for(max_blocks) *
                                     sizeof(CvmGreyPacket),
                                 _Alignof(CvmGreyPacket)) &&
              cvm_region_reserve(&heap->blocks, (usize)max_blocks * CVM_BLOCK_SIZE,
                                 CVM_BLOCK_SIZE);

    if (!ok) {
        cvm_heap_destroy(heap);
        return false;
    }

    heap->max_block_count = max_blocks;
    heap->max_heap_size = (usize)max_blocks * CVM_BLOCK_SIZE;

    heap->block_meta_start = (BlockMeta*)heap->block_meta_region.start;
    heap->line_meta_start = (LineMeta*)heap->line_meta_region.start;

    heap->bytemap = bytemap_init(heap->bytemap_region.start, heap->blocks.start,
                                 heap->max_heap_size);

    /*
     * Start with the initial number of blocks usable. The end pointers
     * are what define "in use", so this is just the initial value of the
     * arithmetic that cvm_heap_extend repeats.
     */
    heap->block_count = 0;
    heap->heap_start = heap->blocks.start;
    atomic_store(&heap->heap_end, heap->heap_start);
    heap->block_meta_end = heap->block_meta_start;
    heap->line_meta_end = heap->line_meta_start;
    heap->heap_size = 0;

    u32 first = 0;
    if (!cvm_heap_extend(heap, initial_blocks, &first)) {
        cvm_heap_destroy(heap);
        return false;
    }

    /*
     * Marking state. Every packet starts on the empty list, so the first
     * marker to want one has somewhere to look; the total is what tells
     * marking when everything is accounted for.
     */
    heap->mark.packet_count = grey_packet_count_for(max_blocks);
    heap->mark.packets = (CvmGreyPacket*)heap->grey_packets_region.start;

    for (u32 i = 0; i < heap->mark.packet_count; i++) {
        cvm_grey_packet_init(&heap->mark.packets[i], CVM_GREY_PACKET_REFLIST);
    }

    if (!cvm_grey_list_init(&heap->mark.empty) ||
        !cvm_grey_list_init(&heap->mark.full) ||
        !cvm_grey_list_init(&heap->mark.found_weak_refs)) {
        cvm_heap_destroy(heap);
        return false;
    }

    cvm_grey_list_push_all(&heap->mark.empty, heap->mark.packets, heap->mark.packets,
                           heap->mark.packet_count);

    /*
     * Marking bookkeeping. `has_history` false means the growth decision
     * has no previous collection to read, which is correct for a fresh
     * heap — and setting it explicitly matters because the heap struct may
     * be reused (a test that builds and destroys several heaps in one
     * process), in which case a stale true would let the first collection
     * read another heap's figures.
     */
    heap->mark.current_start_ns = 0;
    heap->mark.current_end_ns = 0;
    heap->mark.last_end_ns = rt_time_nanos();
    heap->mark.last_mark_ns = 0;
    heap->mark.last_period_ns = 0;
    heap->mark.has_history = false;

    CVM_LOG_INFO(m) {
        fmt_lit(&m.fmt, "heap: ");
        fmt_u32(&m.fmt, heap->block_count);
        fmt_lit(&m.fmt, " of ");
        fmt_u32(&m.fmt, heap->max_block_count);
        fmt_lit(&m.fmt, " blocks, block=");
        fmt_u32(&m.fmt, (u32)CVM_BLOCK_SIZE);
        fmt_lit(&m.fmt, " line=");
        fmt_u32(&m.fmt, (u32)CVM_LINE_SIZE);
    }

    return true;
}

void cvm_heap_destroy(CvmHeap* heap) {
    cvm_region_release(&heap->blocks);
    cvm_region_release(&heap->grey_packets_region);
    cvm_region_release(&heap->block_meta_region);
    cvm_region_release(&heap->line_meta_region);
    cvm_region_release(&heap->bytemap_region);

    heap->heap_start = NULL;
    atomic_store(&heap->heap_end, NULL);
    heap->block_meta_start = NULL;
    heap->block_meta_end = NULL;
    heap->line_meta_start = NULL;
    heap->line_meta_end = NULL;
    heap->bytemap = NULL;
    heap->block_count = 0;
    heap->mark.packets = NULL;
    heap->mark.packet_count = 0;
}

/* ------------------------------------------------------------------ */
/* growth                                                              */
/* ------------------------------------------------------------------ */

bool cvm_heap_extend(CvmHeap* heap, u32 blocks, u32* out_first_block) {
    if (blocks == 0) {
        return true;
    }

    rt_mutex_lock(&heap->grow_lock);

    if (heap->block_count + blocks > heap->max_block_count) {
        rt_mutex_unlock(&heap->grow_lock);
        return false;
    }

    u32 first = heap->block_count;

    /*
     * A block's metadata is not zeroed by the reservation if the region
     * was reused, and a BlockMeta full of rubbish would look like a block
     * in some arbitrary state. Clear it explicitly.
     */
    for (u32 i = 0; i < blocks; i++) {
        BlockMeta* meta = heap->block_meta_start + (usize)(first + i);
        block_meta_clear(meta);
    }

    for (u32 i = 0; i < blocks; i++) {
        LineMeta* lines = heap->line_meta_start + (usize)(first + i) * CVM_LINE_COUNT;
        for (u32 l = 0; l < CVM_LINE_COUNT; l++) {
            line_meta_unmark(&lines[l]);
        }
    }

    /*
     * The bytemap slots for the new blocks are already free: the
     * reservation starts zeroed and CVM_OM_FREE is zero. That is relied
     * on rather than re-cleared, so the encoding of "free" must stay 0 —
     * which gc/meta.h asserts by pinning the enum values.
     */
    _Static_assert((int)CVM_OM_FREE == 0,
                   "the bytemap relies on a fresh reservation reading as "
                   "free, so CVM_OM_FREE must be zero");

    heap->block_count += blocks;
    atomic_store_explicit(&heap->heap_end,
                          heap->heap_start + (usize)heap->block_count * CVM_BLOCK_SIZE,
                          memory_order_release);
    heap->block_meta_end = heap->block_meta_start + heap->block_count;
    heap->line_meta_end =
        heap->line_meta_start + (usize)heap->block_count * CVM_LINE_COUNT;
    heap->heap_size = (usize)heap->block_count * CVM_SPACE_USED_PER_BLOCK;

    if (out_first_block != NULL) {
        *out_first_block = first;
    }

    rt_mutex_unlock(&heap->grow_lock);

    CVM_LOG_DEBUG(m) {
        fmt_lit(&m.fmt, "heap grown by ");
        fmt_u32(&m.fmt, blocks);
        fmt_lit(&m.fmt, " blocks, now ");
        fmt_u32(&m.fmt, heap->block_count);
    }

    return true;
}

bool cvm_heap_should_grow(CvmHeap* heap, u64 mark_time_ns, u64 period_ns,
                          u32 free_blocks) {
    /*
     * Two independent reasons, either sufficient. The reference uses the
     * same pair:
     *
     *   - marking is eating too large a share of the program's time, so
     *     the heap is too small for the allocation rate;
     *   - free blocks are running out, so the next allocation is at risk.
     *
     * Growing only on the second would let a program sit at 100% mark
     * time indefinitely, which is a heap that never stops collecting.
     */
    if (period_ns > 0) {
        f64 ratio = (f64)mark_time_ns / (f64)period_ns;
        if (ratio >= heap->max_mark_time_ratio) {
            return true;
        }
    }

    f64 free_ratio = (f64)free_blocks / (f64)heap->block_count;
    return free_ratio < heap->min_free_ratio;
}

void cvm_heap_grow_if_needed(CvmHeap* heap) {
    /*
     * "Free" for the growth decision is free *plus* recycled: partially free
     * blocks are available memory too, and not counting them is how the
     * check used to conclude the heap was starved right after a sweep. The
     * reference (commix Heap_shouldGrow) counts freeBlockCount +
     * recycledBlockCount for exactly this reason; for most of this project's
     * life we counted only fully free blocks, and the heap grew without
     * bound in reply.
     */
    u32 free_blocks = cvm_block_alloc_free_count(heap->block_alloc);

    free_blocks += cvm_block_alloc_recycled_shared_count(heap->block_alloc);

    cvm_threads_lock_read();
    CVM_THREADS_FOREACH(node) {
        free_blocks += atomic_load_explicit(&node->allocator.recycled_block_count,
                                            memory_order_acquire);
    }
    cvm_threads_unlock_read();

    if (!cvm_heap_should_grow(heap, heap->mark.last_mark_ns, heap->mark.last_period_ns,
                              free_blocks)) {
        return;
    }

    u32 increment = cvm_heap_growth_increment(heap);
    if (increment == 0) {
        return;
    }

    u32 first = 0;
    if (!cvm_heap_extend(heap, increment, &first)) {
        return;
    }

    /*
     * The new blocks are free and their metadata is cleared, so they go
     * straight to the allocator as one run. Their debug state is set the
     * way the sweeper would leave a free block, which is what the
     * allocator expects to find.
     */
    for (u32 i = 0; i < increment; i++) {
        block_meta_set_debug_state(cvm_heap_block_meta(heap, first + i), CVM_DBG_FREE);
    }

    /*
     * Under the allocator's lock: after a sweep finishes the allocator
     * leaves concurrent mode, so its free lists are manipulated non-
     * atomically from then on. A lock-free push from here would race with
     * a pop that is not expecting one.
     */
    cvm_block_alloc_acquire(heap->block_alloc);
    cvm_block_alloc_add_free_superblock(heap->block_alloc,
                                        cvm_heap_block_meta(heap, first), increment);
    cvm_block_alloc_release(heap->block_alloc);

    CVM_LOG_DEBUG(m) {
        fmt_lit(&m.fmt, "heap grew by ");
        fmt_u32(&m.fmt, increment);
        fmt_lit(&m.fmt, " blocks, now ");
        fmt_u32(&m.fmt, heap->block_count);
    }
}

u32 cvm_heap_growth_increment(CvmHeap const* heap) {
    f64 rate = (heap->heap_size < CVM_EARLY_GROWTH_THRESHOLD) ? CVM_EARLY_GROWTH_RATE
                                                              : CVM_GROWTH_RATE;

    u32 extra = (u32)((f64)heap->block_count * (rate - 1.0));
    if (extra == 0) {
        extra = 1; /* always make progress */
    }

    u32 remaining = heap->max_block_count - heap->block_count;
    return (extra > remaining) ? remaining : extra;
}
