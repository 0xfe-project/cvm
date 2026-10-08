/*
 * vm/gc/marker.c
 *
 * The marker. See vm/gc/marker.h for what it does differently from the
 * reference and why.
 *
 * Marking is idempotent, which is what lets it run without synchronising
 * on the mark bits: if two threads mark the same object, it stays marked,
 * and the worst case is that one of them traces it twice. The reference
 * makes the same trade and says so; a test-and-set on every object would
 * cost more than the occasional duplicate trace.
 *
 * Grey packets are the only synchronisation: they hand work between
 * threads, and the count of packets not yet accounted for is what says
 * when marking is finished.
 */

#include "gc/marker.h"

#include "base/assert.h"
#include "base/math.h"
#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "gc/bytemap.h"
#include "gc/grey_packet.h"
#include "gc/heap.h"
#include "gc/meta.h"
#include "cvm/roots.h"
#include "gc/pool.h"
#include "log.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* packet plumbing                                                     */
/* ------------------------------------------------------------------ */

/*
 * Take an empty packet, waiting if every one is in flight.
 *
 * Waiting rather than failing: with several markers running it is
 * possible for all packets to be held at once, and the alternative to
 * waiting is to drop work.
 */
CVM_NOINLINE void report_packet_state(CvmHeap* heap, str what);

static CvmGreyPacket* take_empty_packet(CvmHeap* heap) {
    /*
     * Bounded rather than unbounded. Every packet can legitimately be in
     * flight at once — each marker holds one to trace into and one to fill
     * — so this can wait. It cannot legitimately wait forever: packets come
     * back as markers finish with them.
     *
     * If it does wait forever, the cause is a packet that was taken and
     * never returned, and the process wedges with no output at all. A GC
     * that hangs is the worst way for it to fail, so the count is bounded
     * and the state is reported.
     */
    u64 started = rt_time_millis();
    for (u64 spin = 0;; spin++) {
        CvmGreyPacket* packet =
            cvm_grey_list_pop(&heap->mark.empty, heap->mark.packets);
        if (packet != NULL) {
            return packet;
        }

        if ((spin & 0xfffff) == 0 && spin != 0) {
            if (rt_time_millis() - started >= 2000) {
                report_packet_state(heap, STR_LITERAL("no empty grey packet"));
                rt_fatal(STR_LITERAL("grey packets are lost, not busy"));
            }
        }

        rt_thread_yield();
    }
}

static CvmGreyPacket* try_take_full_packet(CvmHeap* heap) {
    return cvm_grey_list_pop(&heap->mark.full, heap->mark.packets);
}

static void give_empty_packet(CvmHeap* heap, CvmGreyPacket* packet) {
    /*
     * Reset the kind on the way out. A packet that served as an array
     * slice comes back with its type still saying so, and a packet whose
     * type says "slice" but whose contents are a list of objects is worse
     * than wrong: it is read as a base pointer and a count, so the objects
     * that were pushed into it are silently not traced. That is a missing
     * mark, and a missing mark is how a live object gets collected.
     *
     * Only the array-split path sets the other type, and it sets it
     * immediately before pushing, so clearing here is enough.
     */
    packet->type = (u16)CVM_GREY_PACKET_REFLIST;
    cvm_grey_list_push(&heap->mark.empty, heap->mark.packets, packet);
}

static void give_full_packet(CvmHeap* heap, CvmGreyPacket* packet) {
    if (cvm_grey_packet_is_empty(packet)) {
        /*
         * Nothing was found for this packet. It goes back to the empty
         * list rather than the full one: a full packet is work, and
         * marking's completion check counts what is on the full list as
         * unfinished. An empty packet there would keep marking spinning
         * forever, and the assertion below is the invariant that says so.
         */
        give_empty_packet(heap, packet);
        return;
    }
    cvm_assert(packet->type == CVM_GREY_PACKET_REFRANGE || packet->size > 0);
    cvm_grey_list_push(&heap->mark.full, heap->mark.packets, packet);
}

/*
 * Give back a packet that held weak references. An empty one goes back to
 * the empty list: the weak list is not a place to park empty packets, and
 * marking's completion check counts it as work.
 */
static void give_weak_packet(CvmHeap* heap, CvmGreyPacket* packet) {
    if (!cvm_grey_packet_is_empty(packet)) {
        cvm_grey_list_push(&heap->mark.found_weak_refs, heap->mark.packets, packet);
    } else {
        give_empty_packet(heap, packet);
    }
}

/* Put a value into a packet, taking a fresh one when it is full. */
static void packet_push(CvmHeap* heap, CvmGreyPacket** holder, opaque_mut value) {
    if (!cvm_grey_packet_push(*holder, value)) {
        give_full_packet(heap, *holder);
        *holder = take_empty_packet(heap);
        cvm_grey_packet_push(*holder, value);
    }
}

CvmGreyPacket* cvm_mark_take_packet(CvmHeap* heap) {
    return take_empty_packet(heap);
}

void cvm_mark_give_packet(CvmHeap* heap, CvmGreyPacket* packet, bool weak) {
    if (weak) {
        give_weak_packet(heap, packet);
    } else {
        give_full_packet(heap, packet);
    }
}

/* ------------------------------------------------------------------ */
/* marking                                                             */
/* ------------------------------------------------------------------ */

/*
 * Mark an object's metadata: its own slot, the block, and every line it
 * covers.
 *
 * The line marks are what the sweeper uses to keep a line whose objects
 * survived, so they have to cover the object's whole extent — an object
 * spanning two lines that marked only the first would have its second
 * line swept out from under it.
 *
 * Large objects are exempt from the block and line marks: they are not
 * line-managed at all, and their block metadata is a superblock chain
 * rather than a simple block.
 */
static void mark_metadata(CvmHeap* heap, u8* start, ObjectMeta* meta) {
    object_meta_set_marked(meta);

    BlockMeta* block =
        block_meta_of_word(heap->block_meta_start, heap->heap_start, start);
    if (block_meta_contains_large_objects(block)) {
        return;
    }

    /*
     * Only on the transition.
     *
     * A 32 KiB block holds about 1300 objects of 24 bytes, so marking them
     * all would write this same byte 1300 times — and with several marker
     * threads working through one block, those writes bounce the cache line
     * between cores for no reason. A read and a branch is cheaper than a
     * write that is almost always redundant.
     */
    if (!block_meta_is_marked(block)) {
        block_meta_mark(block);
    }

    usize size = cvm_object_size((CvmGcHeader const*)start);
    u8* last = start + size - 1;

    /*
     * An object never spans blocks: the largest object the line-based
     * allocator produces is smaller than a block. The assertion is here
     * rather than assumed because the line walk below would silently
     * touch the wrong block's line table if it ever became false.
     */
    cvm_assert(block_index_of(heap->heap_start, start) ==
               block_index_of(heap->heap_start, last));

    u32 block_index = block_index_of(heap->heap_start, start);
    LineMeta* lines = cvm_heap_line_meta(heap, block_index);

    u32 first_line = block_line_index_of(block_start_of(start), start);
    u32 last_line = block_line_index_of(block_start_of(last), last);

    /*
     * Same argument as the block mark, one level finer: a 256-byte line holds
     * about ten of these objects, so nine writes in ten would be redundant.
     */
    for (u32 line = first_line; line <= last_line; line++) {
        if (!line_meta_is_marked(&lines[line])) {
            line_meta_mark(&lines[line]);
        }
    }
}

void cvm_mark_object(CvmHeap* heap, u8* start, CvmGreyPacket** out,
                     CvmGreyPacket** out_weak) {
    ObjectMeta* meta = bytemap_get(heap->bytemap, start);

    mark_metadata(heap, start, meta);

    CvmGcHeader const* header = (CvmGcHeader const*)start;

    if ((header->flags & CVM_GC_WEAK_REF) != 0) {
        /*
         * A weak reference is recorded, not traced. Recording it here is
         * what keeps its referent from being kept alive by it: the
         * referent is only marked if something else reaches it, and the
         * nullify phase clears the ones that did not survive.
         */
        if (!cvm_grey_packet_push(*out_weak, (opaque_mut)start)) {
            give_weak_packet(heap, *out_weak);
            *out_weak = take_empty_packet(heap);
            cvm_grey_packet_push(*out_weak, (opaque_mut)start);
        }
        return;
    }

    packet_push(heap, out, (opaque_mut)start);
}

void cvm_mark_field(CvmHeap* heap, opaque_mut field, CvmGreyPacket** out,
                    CvmGreyPacket** out_weak) {
    if (field == NULL) {
        return;
    }

    /*
     * A field holds a payload pointer; the object it belongs to starts a
     * header earlier. The conversion is not extra work: the marker needs
     * the header anyway to read the type descriptor. See the address
     * convention note in cvm/gc.h.
     */
    CvmGcHeader const* header = cvm_header_of(field);

    if (!cvm_heap_contains(heap, (u8 const*)header)) {
        return;
    }

    ObjectMeta* meta = bytemap_get(heap->bytemap, (u8 const*)header);

    /*
     * One load, not two. The state is a single byte compared by equality, and
     * "allocated and not marked" is exactly "the byte says allocated" — the
     * two states are distinct values, so asking them separately reads the
     * same cache line twice for the same answer. This is the most-executed
     * branch in the marker.
     */
    if (cvm_mark_byte_get(meta) != (u8)CVM_OM_ALLOCATED) {
        return;
    }

    cvm_mark_object(heap, (u8*)header, out, out_weak);
}

/* ------------------------------------------------------------------ */
/* tracing                                                             */
/* ------------------------------------------------------------------ */

/*
 * Walk one object's pointer fields.
 *
 * `base` is the payload, not the object's start: descriptor offsets
 * describe the user's struct, so they are measured from where that struct
 * begins. Passing the header address instead shifts every field by the
 * header size, which reads the header as if it were the first field.
 *
 * For an embedded aggregate, `base` is the payload plus the field's
 * offset. The descriptor is recursive precisely so that this needs no
 * special case: an embedded field is described by its own descriptor and
 * walked the same way.
 */
static void trace_type(CvmHeap* heap, u8* base, CvmTypeDescriptor const* type,
                       CvmGreyPacket** out, CvmGreyPacket** out_weak) {
    switch ((CvmTypeKind)type->kind) {
    case CVM_TYPE_SCALAR:
        /* Nothing to do: the descriptor exists to say exactly that. */
        break;

    case CVM_TYPE_STRUCT:
        for (u16 i = 0; i < type->num_ptr_fields; i++) {
            u16 offset = type->ptr_fields[i].offset;
            opaque_mut field = *(opaque_mut*)(base + offset);
            cvm_mark_field(heap, field, out, out_weak);
        }
        for (u16 i = 0; i < type->num_embed_fields; i++) {
            CvmEmbedField const* embedded = &type->embed_fields[i];
            trace_type(heap, base + embedded->offset, embedded->type, out, out_weak);
        }
        break;

    case CVM_TYPE_PTR_ARRAY: {
        usize length = *(usize*)(base + type->array_length_offset);
        opaque_mut* items = (opaque_mut*)(base + type->array_data_offset);

        if (length <= CVM_ARRAY_SPLIT_THRESHOLD) {
            for (usize i = 0; i < length; i++) {
                cvm_mark_field(heap, items[i], out, out_weak);
            }
            break;
        }

        /*
         * A large array is handed to the packet list in slices, so that
         * several markers trace it at once. One thread walking a million
         * elements is a serial bottleneck that no amount of packet
         * shuffling elsewhere can hide.
         *
         * If a slice cannot be had — every packet is in flight — the
         * elements are marked on this thread instead. Dropping them would
         * be a correctness bug, and waiting would be a deadlock when this
         * is the only thread holding a packet.
         */
        usize batch = CVM_ARRAY_SPLIT_BATCH;
        usize done = 0;

        while (length - done >= batch) {
            CvmGreyPacket* slice =
                cvm_grey_list_pop(&heap->mark.empty, heap->mark.packets);
            if (slice == NULL) {
                for (usize i = done; i < done + batch; i++) {
                    cvm_mark_field(heap, items[i], out, out_weak);
                }
            } else {
                slice->type = (u16)CVM_GREY_PACKET_REFRANGE;
                slice->size = (u16)batch;
                slice->items[0] = (opaque_mut)&items[done];
                give_full_packet(heap, slice);
            }
            done += batch;
        }

        /* The remainder is small by construction, so it goes inline. */
        for (usize i = done; i < length; i++) {
            cvm_mark_field(heap, items[i], out, out_weak);
        }
        break;
    }

    case CVM_TYPE_STRUCT_ARRAY: {
        usize length = *(usize*)(base + type->array_length_offset);
        CvmTypeDescriptor const* element = type->array_elem_type;
        u8* items = base + type->array_data_offset;
        usize stride = element->size;

        for (usize i = 0; i < length; i++) {
            trace_type(heap, items + i * stride, element, out, out_weak);
        }
        break;
    }
    }
}

void cvm_trace_object(CvmHeap* heap, u8* start, CvmGreyPacket** out,
                      CvmGreyPacket** out_weak) {
    CvmGcHeader const* header = (CvmGcHeader const*)start;
    u8* payload = (u8*)start + sizeof(CvmGcHeader);
    trace_type(heap, payload, header->type, out, out_weak);
}

/* ------------------------------------------------------------------ */
/* roots                                                               */
/* ------------------------------------------------------------------ */

/*
 * Walk one thread's frame chain.
 *
 * The chain is maintained by the thread itself, newest frame first, and
 * each frame holds exactly the pointers live in that activation. There is
 * no range to bound and no stack to sweep: this is the whole of root
 * finding.
 */
static void mark_frame_chain(CvmHeap* heap, CvmGcFrame* frame, CvmGreyPacket** out,
                             CvmGreyPacket** out_weak) {
    for (; frame != NULL; frame = frame->prev) {
        for (u32 i = 0; i < frame->num_slots; i++) {
            cvm_mark_field(heap, frame->roots[i], out, out_weak);
        }
    }
}

void cvm_mark_roots(CvmHeap* heap) {
    CvmGreyPacket* out = take_empty_packet(heap);
    CvmGreyPacket* out_weak = take_empty_packet(heap);

    /*
     * The read lock keeps a thread from registering or leaving while the
     * chains are walked. Threads that are running are already stopped:
     * this runs with the world held.
     */
    cvm_threads_lock_read();
    CVM_THREADS_FOREACH(node) {
        mark_frame_chain(heap, node->ctx.top_frame, &out, &out_weak);
    }
    cvm_threads_unlock_read();

    /*
     * The global root table, for pointers held by foreign code. It takes
     * its own lock, so it is walked outside the registry lock — the same
     * lock-ordering discipline the rest of the collector follows.
     */
    cvm_root_mark_all(heap, &out, &out_weak);

    give_full_packet(heap, out);
    give_weak_packet(heap, out_weak);
}

/* ------------------------------------------------------------------ */
/* draining                                                            */
/* ------------------------------------------------------------------ */

void cvm_mark_batch(CvmHeap* heap, CvmGreyPacket* in, CvmGreyPacket** out,
                    CvmGreyPacket** out_weak) {
    /*
     * The caller's holders may start empty. Taking here rather than
     * requiring the caller to have taken first makes "out is non-NULL once
     * this returns" an invariant rather than a hope — and the marking loop
     * relies on it when it reuses the out packet as its next input.
     */
    if (*out == NULL) {
        *out = take_empty_packet(heap);
    }
    if (*out_weak == NULL) {
        *out_weak = take_empty_packet(heap);
    }

    while (!cvm_grey_packet_is_empty(in)) {
        if (in->type == (u16)CVM_GREY_PACKET_REFRANGE) {
            /* A slice of a large array: the packet holds where to start
             * and how many words follow. */
            opaque_mut* base = (opaque_mut*)in->items[0];
            u16 count = in->size;
            in->size = 0;
            for (u16 i = 0; i < count; i++) {
                cvm_mark_field(heap, base[i], out, out_weak);
            }
        } else {
            u8* start = cvm_grey_packet_pop(in);
            cvm_trace_object(heap, start, out, out_weak);
        }
    }
}

/*
 * Marking's driving loop.
 *
 * The packet churn is the part worth reading twice. When there is more
 * work on the full list, the exhausted in packet goes back and a new one
 * is taken. When there is not, but this trace produced objects, the out
 * packet *becomes* the in packet and the old in packet becomes the new
 * out packet. That avoids returning a packet only to take another
 * immediately, which under a single marker is most of what it would be
 * doing.
 */
void cvm_mark(CvmHeap* heap, bool scale) {
    CvmGreyPacket* in = try_take_full_packet(heap);
    CvmGreyPacket* out = NULL;
    CvmGreyPacket* out_weak = NULL;

    while (in != NULL) {
        cvm_mark_batch(heap, in, &out, &out_weak);

        cvm_assert(out != NULL);
        cvm_assert(cvm_grey_packet_is_empty(in));

        CvmGreyPacket* next = try_take_full_packet(heap);

        if (next != NULL) {
            give_empty_packet(heap, in);
            if (scale) {
                /*
                 * A backlog means marking is falling behind, so more
                 * workers are worth waking. They stop themselves when the
                 * list runs dry, so over-waking costs a check rather than
                 * a busy thread.
                 */
                cvm_pool_scale_markers(heap->pool,
                                       cvm_grey_list_size(&heap->mark.full));
            }
        } else if (!cvm_grey_packet_is_empty(out)) {
            /*
             * Nothing left to take, but this trace produced more. Reuse
             * the packets already in hand rather than returning them and
             * immediately taking others: the in packet becomes the out
             * packet and vice versa.
             */
            next = out;
            out = in;
        } else {
            /* Nothing left and nothing produced: this marker is done. */
            give_empty_packet(heap, in);
            give_full_packet(heap, out);
            give_weak_packet(heap, out_weak);
            return;
        }

        in = next;
    }

    /*
     * The loop is entered with `in == NULL` when there was nothing to do
     * at all, in which case `out` is still NULL and there is nothing to
     * hand back. Otherwise the packets were returned above.
     */
}

/*
 * Say how the packets are distributed, and name any that are on no list.
 *
 * Marking cannot finish while a packet is unaccounted for, so a packet that
 * was taken and never returned wedges it. Naming the index is the
 * difference between a diagnosable bug and "marking stopped working
 * sometimes": the index locates it in the packet array, and from there in
 * the code that handed it out.
 *
 * Builds the set of reachable packets by walking each list, which makes it
 * quadratic. It runs only on the failure path, where the alternative is no
 * information at all — and it does consume the lists, which is acceptable
 * only because the caller is about to end the process.
 */
CVM_NOINLINE void report_packet_state(CvmHeap* heap, str what) {
    u32 total = heap->mark.packet_count;

    CVM_LOG_ERROR(m) {
        fmt_str(&m.fmt, what);
        fmt_lit(&m.fmt, ": empty=");
        fmt_u32(&m.fmt, cvm_grey_list_size(&heap->mark.empty));
        fmt_lit(&m.fmt, " full=");
        fmt_u32(&m.fmt, cvm_grey_list_size(&heap->mark.full));
        fmt_lit(&m.fmt, " weak=");
        fmt_u32(&m.fmt, cvm_grey_list_size(&heap->mark.found_weak_refs));
        fmt_lit(&m.fmt, " total=");
        fmt_u32(&m.fmt, total);
    }

#if defined(CVM_DEBUG)
    u8 on_a_list[1024];
    u32 reportable = (total < sizeof(on_a_list)) ? total : 0;
    if (reportable == 0) {
        return;
    }

    for (u32 i = 0; i < reportable; i++) {
        on_a_list[i] = 0;
    }

    for (u32 which = 0; which < 3; which++) {
        CvmGreyList* grey = (which == 0)   ? &heap->mark.empty
                            : (which == 1) ? &heap->mark.full
                                           : &heap->mark.found_weak_refs;
        for (;;) {
            CvmGreyPacket* packet = cvm_grey_list_pop(grey, heap->mark.packets);
            if (packet == NULL) {
                break;
            }
            u32 index = (u32)(packet - heap->mark.packets);
            if (index < reportable) {
                on_a_list[index] = 1;
            }
        }
    }

    for (u32 i = 0; i < reportable; i++) {
        if (!on_a_list[i]) {
            CVM_LOG_ERROR(m) {
                fmt_lit(&m.fmt, "  packet ");
                fmt_u32(&m.fmt, i);
                fmt_lit(&m.fmt, " is on no list (type=");
                fmt_u32(&m.fmt, (u32)heap->mark.packets[i].type);
                fmt_lit(&m.fmt, " size=");
                fmt_u32(&m.fmt, (u32)heap->mark.packets[i].size);
                fmt_lit(&m.fmt, ")");
            }
        }
    }
#endif
}

bool cvm_mark_is_done(CvmHeap* heap) {
    u32 empty = cvm_grey_list_size(&heap->mark.empty);
    u32 weak = cvm_grey_list_size(&heap->mark.found_weak_refs);
    return empty + weak == heap->mark.packet_count;
}

void cvm_mark_until_done(CvmHeap* heap) {
    /*
     * Bounded by time rather than by iteration count: a marker that can
     * never conclude is worse than one that says why, and the number of
     * yields it takes to get there depends on how slow the machine is —
     * under a sanitizer, millions of yields can pass in a second.
     */
    u64 last_progress_at = rt_time_millis();
    u32 last_accounted = 0;

    while (!cvm_mark_is_done(heap)) {
        cvm_mark(heap, true);

        u32 accounted = cvm_grey_list_size(&heap->mark.empty) +
                        cvm_grey_list_size(&heap->mark.found_weak_refs);
        if (accounted != last_accounted) {
            last_accounted = accounted;
            last_progress_at = rt_time_millis();
        } else if (rt_time_millis() - last_progress_at >= 2000) {
            report_packet_state(heap, STR_LITERAL("marking made no progress"));
            rt_fatal(STR_LITERAL("marking cannot complete"));
        }

        if (!cvm_mark_is_done(heap)) {
            /* Other markers have the work in flight; let them finish it
             * rather than spinning on the packet list. */
            rt_thread_yield();
        }
    }
}

/* ------------------------------------------------------------------ */
/* queries for the nullify phase                                       */
/* ------------------------------------------------------------------ */

bool cvm_object_is_marked(CvmHeap* heap, opaque_mut payload) {
    if (payload == NULL) {
        return false;
    }

    CvmGcHeader const* header = cvm_header_of(payload);
    if (!cvm_heap_contains(heap, (u8 const*)header)) {
        return false;
    }

    ObjectMeta const* meta = bytemap_get_const(heap->bytemap, (u8 const*)header);
    return object_meta_is_marked(meta);
}
