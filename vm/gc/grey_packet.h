/*
 * vm/gc/grey_packet.h
 *
 * Grey packets and grey lists, ported from
 * refs/commix/datastructures/GreyPacket.{h,c}.
 *
 * ---------------------------------------------------------------------
 * What a grey packet is
 * ---------------------------------------------------------------------
 *
 * A fixed-size array of pointers to objects that have been marked but
 * whose fields have not been traced yet. Marking threads take packets off
 * the full list, trace what is in them, and put the newly discovered
 * objects into packets of their own; when a packet fills it goes on the
 * full list and the thread takes an empty one.
 *
 * Packets are allocated once, up front, in one array. Nothing about them
 * is heap-allocated during a collection, which is what lets marking run
 * without ever needing to allocate — important, because allocating during
 * a collection would be circular.
 *
 * ---------------------------------------------------------------------
 * Why the list head is one word
 * ---------------------------------------------------------------------
 *
 * A grey list needs to know two things: which packet is at the head, and
 * how many packets it holds in total. Marking is finished when the count
 * of packets in flight reaches zero, so that count has to be accurate at
 * every instant a marking thread might read it.
 *
 * Keeping the count in a separate field does not work. A push does two
 * things — link the packet, then increment the count — and between them
 * the list is longer than the count says, or the count is larger than the
 * list. A thread reading the count in that window can conclude that
 * marking is done when it is not, and stop early. The reference states
 * this in a comment on the size field, and it is the reason for the shape
 * below.
 *
 * So the head index and the count live in one 64-bit word and are updated
 * by a single compare-exchange. A reader sees both, or neither. The
 * remaining 16 bits are an ABA counter on the head packet: without it, a
 * packet popped and pushed back during another thread's exchange would let
 * that exchange succeed against a stale expectation.
 *
 * ---------------------------------------------------------------------
 * Sentinels and indices
 * ---------------------------------------------------------------------
 *
 * Packet indices are stored offset by two, so that 0 and 1 are free to
 * mean something else:
 *
 *   0  the list is empty
 *   1  the list ends here (a packet's `next` when it is the last one)
 *
 * A packet's `next` is either 0, meaning "the next packet is at index + 1"
 * — which is how a contiguous run pushed by push_all is represented, with
 * no per-packet writes — or a whole head word, meaning the rest of the
 * list.
 */

#ifndef CVM_GC_GREY_PACKET_H
#define CVM_GC_GREY_PACKET_H

#include <stdatomic.h>

#include "base/assert.h"
#include "base/types.h"
#include "cvm/gc.h"
#include "rt.h"

/* Bytes per packet, and the pointers that fit in one. */
#define CVM_GREY_PACKET_SIZE 2048
#define CVM_GREY_PACKET_ITEMS ((CVM_GREY_PACKET_SIZE / CVM_WORD_SIZE) - 2)

/* List sentinels, in the offset-by-two index space. */
#define CVM_GREYLIST_NEXT ((u32)0)
#define CVM_GREYLIST_LAST ((u32)1)

/* The offset applied to a packet index. */
#define CVM_GREY_INDEX_BIAS 2u

/*
 * The packed list head.
 *
 * The three fields are laid out to fill eight bytes exactly with no
 * padding: u24 is three bytes and is only one-byte aligned, so the u16
 * lands at offset six. The static assertion below is what keeps that
 * true, because the union's second member reinterprets the whole thing as
 * one word and a compiler that inserted padding would make the two views
 * disagree.
 *
 * No packed attribute is needed, which is deliberate: `packed` is a
 * GCC/Clang extension and MSVC spells it differently, and the layout
 * already comes out right.
 */
typedef union CvmGreyPacketRef {
    struct {
        u24 idx;
        u24 size;

        /*
         * Padding, not a counter. The reference used this for an ABA
         * counter on the head packet; the lists are mutex-guarded now, so
         * there is nothing to count. It stays because the two u24 fields
         * only come to six bytes, and the word has to be eight for the
         * whole-head load and store to be one operation.
         */
        u16 _reserved;
    } sep;
    u64 atom;
} CvmGreyPacketRef;

_Static_assert(sizeof(CvmGreyPacketRef) == 8,
               "the packed head must be exactly one word, or the atomic "
               "view and the field view disagree");
_Static_assert(sizeof(((CvmGreyPacketRef*)0)->sep) == 8,
               "the packed head's fields must fill the word exactly");

typedef enum CvmGreyPacketType {
    /* A list of object pointers. */
    CVM_GREY_PACKET_REFLIST = 0x0,
    /* A range of an array, described by its base and a count. */
    CVM_GREY_PACKET_REFRANGE = 0x1,
} CvmGreyPacketType;

typedef struct CvmGreyPacket {
    /*
     * Link to the rest of the list. Carries both the index of the next
     * packet and the count of packets from there on, which is what lets a
     * pop restore the list's size without walking it.
     */
    CvmGreyPacketRef next;

    u16 size;
    u16 type;

    /*
     * Object pointers, or — for a refrange — a base pointer followed by a
     * count. A range is how a very large array is marked without copying
     * every element into a packet.
     */
    opaque_mut items[CVM_GREY_PACKET_ITEMS];
} CvmGreyPacket;

/*
 * A grey list: a stack of packets, guarded by a mutex.
 *
 * The reference makes this lock-free: a Treiber stack over one word holding
 * the head index and the count, with a 16-bit counter on the head packet
 * as an ABA guard. That guard is not complete — when the head comes from a
 * contiguous run of packets (which is how the list starts, and how a batch
 * of packets is handed over) the counter is not carried, so the guard does
 * not apply to exactly the case that happens first. The consequence is not
 * a slow path: a pop whose exchange succeeds against a stale head can hand
 * two threads the same packet, and a packet that is owned twice is a packet
 * that comes back once. Marking cannot finish while a packet is
 * unaccounted for, so the process wedges at the next collection with
 * nothing in the log to explain it. (That is exactly what happened, and it
 * took a lost-packet diagnostic to see it: one packet on no list.)
 *
 * A mutex removes the whole class of problem, and the cost is nothing that
 * matters: a packet holds two kilobytes' worth of objects to trace, so the
 * lock is taken once per two kilobytes of marking work. Trading an
 * unverifiable lock-free argument for a lock whose correctness is obvious
 * is the right side of that trade.
 *
 * The head word still packs the index and the count together, because the
 * count has to travel with the link anyway: a pop has to know how many
 * packets remain behind the one it takes.
 */
typedef struct CvmGreyList {
    RtMutex lock;
    /*
     * Atomic so the size can be read without the lock. Every write happens
     * under `lock` (relaxed stores; the mutex orders writers against each
     * other), and the readers that matter are advisory: marking's completion
     * check asks "are all packets accounted for", and a stale answer only
     * makes it look once more -- the guarantee it relies on comes from the
     * workers being idle, which is waited on separately. Before this field
     * was atomic, that check took this lock twice a round, against the same
     * lock every push and pop was already contending for.
     */
    _Atomic u64 head; /* a CvmGreyPacketRef */
    bool ready;
} CvmGreyList;

/* ------------------------------------------------------------------ */
/* packets                                                             */
/* ------------------------------------------------------------------ */

/* Append. Returns false when the packet is full. */
bool cvm_grey_packet_push(CvmGreyPacket* packet, opaque_mut value);

/* Remove the last item. The packet must not be empty. */
opaque_mut cvm_grey_packet_pop(CvmGreyPacket* packet);

bool cvm_grey_packet_is_empty(CvmGreyPacket const* packet);

CVM_INLINE bool cvm_grey_packet_is_full(CvmGreyPacket const* packet) {
    return packet->size >= CVM_GREY_PACKET_ITEMS;
}

/* Move `count` items from the end of src to the end of dst. */
void cvm_grey_packet_move_items(CvmGreyPacket* src, CvmGreyPacket* dst, u32 count);

CVM_INLINE void cvm_grey_packet_init(CvmGreyPacket* packet, CvmGreyPacketType type) {
    packet->size = 0;
    packet->type = (u16)type;
    packet->next.atom = 0;
}

/* ------------------------------------------------------------------ */
/* lists                                                               */
/* ------------------------------------------------------------------ */

/* Prepare the list. Returns false if the lock could not be created. */
bool cvm_grey_list_init(CvmGreyList* list);

/* Packets currently on the list. Read atomically with the head. */
u32 cvm_grey_list_size(CvmGreyList const* list);

/* Push one packet, or a contiguous run of them. */
void cvm_grey_list_push(CvmGreyList* list, CvmGreyPacket* packets_start,
                        CvmGreyPacket* packet);

void cvm_grey_list_push_all(CvmGreyList* list, CvmGreyPacket* packets_start,
                            CvmGreyPacket* first, u32 count);

/* Pop one packet, or NULL when the list is empty. */
CvmGreyPacket* cvm_grey_list_pop(CvmGreyList* list, CvmGreyPacket* packets_start);

/* The head word as an initialised empty list. */
CVM_INLINE u64 cvm_grey_list_empty(void) {
    CvmGreyPacketRef r;
    r.sep.idx = u24_from_u32(CVM_GREYLIST_LAST);
    r.sep.size = u24_from_u32(0);
    r.sep._reserved = 0;
    return r.atom;
}

#endif /* CVM_GC_GREY_PACKET_H */
