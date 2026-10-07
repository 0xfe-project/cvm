/*
 * vm/gc/grey_packet.c
 *
 * Grey packets and lists. See vm/gc/grey_packet.h for the shape and, in
 * particular, why the list head is one word.
 *
 * The compare-exchange loops here follow the reference. The subtle part
 * is not the loop — it is that every mutation of the list must update the
 * head index and the total count in the same exchange, because a reader
 * that sees a consistent pair is entitled to conclude that marking is
 * finished when the count reaches zero.
 */

#include "gc/grey_packet.h"

#include <stdatomic.h>

#include "base/assert.h"
#include "base/types.h"
#include "cvm/gc.h"

/* ------------------------------------------------------------------ */
/* packets                                                             */
/* ------------------------------------------------------------------ */

bool cvm_grey_packet_push(CvmGreyPacket* packet, opaque_mut value) {
    cvm_assert(value != NULL);

    if (packet->size >= CVM_GREY_PACKET_ITEMS) {
        return false;
    }
    packet->items[packet->size++] = value;
    return true;
}

opaque_mut cvm_grey_packet_pop(CvmGreyPacket* packet) {
    cvm_assert(packet->size > 0);
    return packet->items[--packet->size];
}

bool cvm_grey_packet_is_empty(CvmGreyPacket const* packet) {
    return packet->size == 0;
}

void cvm_grey_packet_move_items(CvmGreyPacket* src, CvmGreyPacket* dst, u32 count) {
    cvm_assert(dst->size + count < CVM_GREY_PACKET_ITEMS);
    cvm_assert(src->size >= count);

    /* Copy by hand rather than memcpy: memcpy is a libc symbol and this
     * is freestanding code. The compiler turns the loop into the same
     * thing for a known count, and the count is bounded by the packet
     * size either way. */
    u32 dst_start = dst->size;
    u32 src_start = src->size - count;
    for (u32 i = 0; i < count; i++) {
        dst->items[dst_start + i] = src->items[src_start + i];
    }
    dst->size += (u16)count;
    src->size -= (u16)count;
}

/* ------------------------------------------------------------------ */
/* index arithmetic                                                    */
/* ------------------------------------------------------------------ */

static u32 packet_index_of(CvmGreyPacket* packets_start, CvmGreyPacket* packet) {
    cvm_assert(packet != NULL);
    cvm_assert(packet >= packets_start);
    return (u32)(packet - packets_start) + CVM_GREY_INDEX_BIAS;
}

static CvmGreyPacket* packet_from_index(CvmGreyPacket* packets_start, u32 index) {
    cvm_assert(index >= CVM_GREY_INDEX_BIAS);
    return packets_start + (index - CVM_GREY_INDEX_BIAS);
}

/* ------------------------------------------------------------------ */
/* lists                                                               */
/* ------------------------------------------------------------------ */

void cvm_grey_list_init(CvmGreyList* list) {
    atomic_store_explicit(&list->head, cvm_grey_list_empty(), memory_order_relaxed);
}

u32 cvm_grey_list_size(CvmGreyList const* list) {
    u64 head = atomic_load_explicit(&list->head, memory_order_relaxed);
    CvmGreyPacketRef ref;
    ref.atom = head;
    return u24_to_u32(ref.sep.size);
}

void cvm_grey_list_push(CvmGreyList* list, CvmGreyPacket* packets_start,
                        CvmGreyPacket* packet) {
    u32 index = packet_index_of(packets_start, packet);
    u32 times_popped =
        atomic_load_explicit(&packet->times_popped, memory_order_relaxed);

    u64 head = atomic_load_explicit(&list->head, memory_order_relaxed);
    u64 next_value;

    for (;;) {
        CvmGreyPacketRef h;
        h.atom = head;

        CvmGreyPacketRef n;
        n.sep.idx = u24_from_u32(index);
        n.sep.size = u24_add(h.sep.size, 1);
        n.sep.times_popped = (u16)times_popped;

        /* If the list was empty, this packet becomes the whole list and
         * is its own end. Otherwise it points at what was the head, with
         * that head's own link and count carried along. */
        if (u24_to_u32(h.sep.idx) == CVM_GREYLIST_LAST) {
            packet->next.atom = cvm_grey_list_empty();
        } else {
            packet->next.atom = head;
        }

        next_value = n.atom;
        if (atomic_compare_exchange_weak_explicit(&list->head, &head, next_value,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
            return;
        }
        /* head was refreshed by the failed exchange; rebuild and retry */
    }
}

void cvm_grey_list_push_all(CvmGreyList* list, CvmGreyPacket* packets_start,
                            CvmGreyPacket* first, u32 count) {
    cvm_assert(count > 0);

    u32 index = packet_index_of(packets_start, first);
    u32 times_popped = atomic_load_explicit(&first->times_popped, memory_order_relaxed);
    CvmGreyPacket* last = first + (count - 1);

    u64 head = atomic_load_explicit(&list->head, memory_order_relaxed);
    u64 next_value;

    for (;;) {
        CvmGreyPacketRef h;
        h.atom = head;

        CvmGreyPacketRef n;
        n.sep.idx = u24_from_u32(index);
        n.sep.size = u24_add(h.sep.size, (i32)count);
        n.sep.times_popped = (u16)times_popped;

        /* Only the last packet of the run needs a link: the rest are
         * found by adding one to the index, which is why a run has to be
         * contiguous in the packet array. */
        if (u24_to_u32(h.sep.idx) == CVM_GREYLIST_LAST) {
            last->next.atom = cvm_grey_list_empty();
        } else {
            last->next.atom = head;
        }

        next_value = n.atom;
        if (atomic_compare_exchange_weak_explicit(&list->head, &head, next_value,
                                                  memory_order_release,
                                                  memory_order_relaxed)) {
            return;
        }
    }
}

CvmGreyPacket* cvm_grey_list_pop(CvmGreyList* list, CvmGreyPacket* packets_start) {
    u64 head = atomic_load_explicit(&list->head, memory_order_acquire);

    for (;;) {
        CvmGreyPacketRef h;
        h.atom = head;

        u32 index = u24_to_u32(h.sep.idx);

        /* An empty list and the end sentinel are different states; only
         * the first means "nothing here". */
        cvm_assert(index != CVM_GREYLIST_NEXT);
        if (index == CVM_GREYLIST_LAST) {
            return NULL;
        }

        CvmGreyPacket* packet = packet_from_index(packets_start, index);

        u64 next = packet->next.atom;
        u64 next_value;

        if (next != 0) {
            /* The packet holds the rest of the list. */
            next_value = next;
        } else {
            /* No link means the next packet is the adjacent one, and the
             * count drops by one. */
            CvmGreyPacketRef n;
            n.sep.idx = u24_add(h.sep.idx, 1);
            n.sep.size = u24_add(h.sep.size, -1);
            n.sep.times_popped = h.sep.times_popped;
            next_value = n.atom;
        }

        if (atomic_compare_exchange_weak_explicit(&list->head, &head, next_value,
                                                  memory_order_acquire,
                                                  memory_order_acquire)) {
            atomic_fetch_add_explicit(&packet->times_popped, 1, memory_order_relaxed);
            return packet;
        }
    }
}
