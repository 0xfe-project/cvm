/*
 * vm/gc/grey_packet.c
 *
 * Grey packets and lists. See vm/gc/grey_packet.h for why the lists are
 * mutex-guarded rather than lock-free.
 *
 * The encoding is the reference's and is worth stating once, because the
 * link word carries more than a link:
 *
 *   packet indices are stored offset by two, so 0 can mean "no packets at
 *   all" and 1 can mean "the list ends here";
 *
 *   a packet's link is either 0, meaning "the next packet is the adjacent
 *   one" — which is how a contiguous run handed over in one push is
 *   represented with no per-packet writes — or a whole head word, meaning
 *   the rest of the list from there on;
 *
 *   the head word also carries the count of packets from the head onwards,
 *   so a pop can restore the list's size without walking it. That is why
 *   the count lives in the word rather than in a field of its own: it has
 *   to travel with the link.
 */

#include "gc/grey_packet.h"

#include "base/assert.h"
#include "base/types.h"
#include "cvm/gc.h"
#include "rt.h"

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

    /* Written out rather than memcpy: memcpy is a libc symbol and this is
     * freestanding code. */
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

bool cvm_grey_list_init(CvmGreyList* list) {
    if (!rt_mutex_init(&list->lock)) {
        return false;
    }
    list->head = cvm_grey_list_empty();
    list->ready = true;
    return true;
}

u32 cvm_grey_list_size(CvmGreyList const* list) {
    CvmGreyList* mutable_list = (CvmGreyList*)list;
    if (!mutable_list->ready) {
        return 0;
    }

    rt_mutex_lock(&mutable_list->lock);
    CvmGreyPacketRef ref;
    ref.atom = mutable_list->head;
    u32 count = u24_to_u32(ref.sep.size);
    rt_mutex_unlock(&mutable_list->lock);
    return count;
}

void cvm_grey_list_push(CvmGreyList* list, CvmGreyPacket* packets_start,
                        CvmGreyPacket* packet) {
    u32 index = packet_index_of(packets_start, packet);

    rt_mutex_lock(&list->lock);

    CvmGreyPacketRef head;
    head.atom = list->head;

    CvmGreyPacketRef next;
    next.sep.idx = u24_from_u32(index);
    next.sep.size = u24_add(head.sep.size, 1);
    next.sep._reserved = 0;

    /*
     * If the list was empty, this packet is the whole list and is its own
     * end. Otherwise it points at what was the head, carrying that head's
     * own link and count along with it.
     */
    if (u24_to_u32(head.sep.idx) == CVM_GREYLIST_LAST) {
        packet->next.atom = cvm_grey_list_empty();
    } else {
        packet->next.atom = head.atom;
    }

    list->head = next.atom;
    rt_mutex_unlock(&list->lock);
}

void cvm_grey_list_push_all(CvmGreyList* list, CvmGreyPacket* packets_start,
                            CvmGreyPacket* first, u32 count) {
    cvm_assert(count > 0);

    u32 index = packet_index_of(packets_start, first);
    CvmGreyPacket* last = first + (count - 1);

    rt_mutex_lock(&list->lock);

    CvmGreyPacketRef head;
    head.atom = list->head;

    CvmGreyPacketRef next;
    next.sep.idx = u24_from_u32(index);
    next.sep.size = u24_add(head.sep.size, (i32)count);
    next.sep._reserved = 0;

    /*
     * Only the last packet of the run needs a link: the rest are found by
     * adding one to the index, which is why a run has to be contiguous in
     * the packet array.
     */
    if (u24_to_u32(head.sep.idx) == CVM_GREYLIST_LAST) {
        last->next.atom = cvm_grey_list_empty();
    } else {
        last->next.atom = head.atom;
    }

    list->head = next.atom;
    rt_mutex_unlock(&list->lock);
}

CvmGreyPacket* cvm_grey_list_pop(CvmGreyList* list, CvmGreyPacket* packets_start) {
    rt_mutex_lock(&list->lock);

    CvmGreyPacketRef head;
    head.atom = list->head;

    u32 index = u24_to_u32(head.sep.idx);

    /* An empty list and the end sentinel are different states; only the
     * first means "nothing here". */
    cvm_assert(index != CVM_GREYLIST_NEXT);
    if (index == CVM_GREYLIST_LAST) {
        rt_mutex_unlock(&list->lock);
        return NULL;
    }

    CvmGreyPacket* packet = packet_from_index(packets_start, index);
    u64 next = packet->next.atom;

    if (next != 0) {
        /* The packet holds the rest of the list. */
        list->head = next;
    } else {
        /* No link means the next packet is the adjacent one, and the count
         * drops by one. */
        CvmGreyPacketRef n;
        n.sep.idx = u24_add(head.sep.idx, 1);
        n.sep.size = u24_add(head.sep.size, -1);
        n.sep._reserved = 0;
        list->head = n.atom;
    }

    rt_mutex_unlock(&list->lock);
    return packet;
}
