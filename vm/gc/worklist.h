/*
 * vm/gc/worklist.h
 *
 * A work-stealing deque of grey packets, one per marker.
 *
 * ---------------------------------------------------------------------
 * Where it comes from
 * ---------------------------------------------------------------------
 *
 * The Chase-Lev deque -- "Dynamic Circular Work-Stealing Deque" (Chase and
 * Lev, SPAA'05) -- with the memory orders from "Correct and Efficient
 * Work-Stealing for Weak Memory Models" (Lê et al., PPoPP'13). The code is a
 * transcription of UnboundedWSQ in refs/chase-lev.cpp (from Taskflow), which
 * is the same algorithm whippet's shared-worklist.h implements. push, pop
 * and steal are that file's push, pop and steal, line for line, including
 * every fence and every memory order. Change them only with the paper open.
 *
 * ---------------------------------------------------------------------
 * The one thing that is not transcribed, and why
 * ---------------------------------------------------------------------
 *
 * UnboundedWSQ grows: when the ring fills, it allocates one twice the size,
 * copies the live range across, and keeps the old ring on a garbage list,
 * because a thief may still be reading it. None of that is here, and it is
 * not a simplification of the algorithm but a consequence of what is being
 * queued.
 *
 * The elements are grey packets, and there is a fixed number of them: the
 * heap reserves its packet pool once, at startup, and a packet is in exactly
 * one place at a time -- on a list, in one deque, or in one marker's hands.
 * So no deque can ever hold more than `packet_count` elements, and a ring of
 * the next power of two above that can never fill. The resize branch is
 * unreachable by construction, and unreachable code that frees memory a
 * thief might be reading is a liability rather than a feature.
 *
 * The capacity check is kept, as an assertion: if it ever fires, the claim
 * above is false, and that is worth knowing at the moment it becomes false.
 *
 * ---------------------------------------------------------------------
 * Who calls what
 * ---------------------------------------------------------------------
 *
 *   push, pop   only the owner -- the marker this deque belongs to
 *   steal       any other marker
 *   empty       anyone; an answer, not a promise
 *
 * Mutators never touch a deque. A mutator's write barrier has no deque of its
 * own and hands its packet to the global full list, which is what that list
 * is now for (see gc/marker.c).
 */

#ifndef CVM_GC_WORKLIST_H
#define CVM_GC_WORKLIST_H

#include <stdatomic.h>

#include "base/assert.h"
#include "base/types.h"
#include "gc/grey_packet.h"

/*
 * A cache line, so that `top` (written by thieves) and `bottom` (written by the
 * owner) do not share one. refs/chase-lev.cpp aligns both to
 * WSQ_CACHELINE_SIZE for the same reason; 128 rather than 64 because Apple's
 * performance cores prefetch adjacent pairs of lines.
 */
#define CVM_WORKLIST_ALIGN 128

typedef struct CvmWorklist {
    _Alignas(CVM_WORKLIST_ALIGN) _Atomic i64 top;
    _Alignas(CVM_WORKLIST_ALIGN) _Atomic i64 bottom;

    /*
     * The ring and its mask. Never replaced -- see the note at the top of
     * this file -- so it needs none of UnboundedWSQ's atomic pointer or
     * garbage list. On its own line so a thief loading it after `bottom`
     * does not false-share with the owner's stores.
     */
    _Alignas(CVM_WORKLIST_ALIGN) CvmGreyPacket* _Atomic* slots;
    i64 mask;

    /*
     * Owner-private cached upper bound on `top`, never read by a thief.
     * UnboundedWSQ's `_cached_top`: `top` only grows, so occupancy computed
     * from a stale value can only be an overestimate, which is the safe
     * direction for a capacity check.
     */
    i64 cached_top;
} CvmWorklist;

/*
 * The deque the marker running on this thread is filling, or NULL.
 *
 * TLS because the packet a marker fills is handed back deep inside the trace
 * path -- cvm_mark_field, called from every field walk, refills its output
 * packet when it fills up -- and threading a worklist through every one of
 * those signatures would be a parameter that is the same for a whole phase.
 *
 * NULL is the ordinary case for a mutator: a write barrier has no deque of
 * its own and hands its packet to the global full list, which is what that
 * list is for. So the fallback is not an error path, it is the mutator's
 * path.
 */
extern __thread CvmWorklist* cvm_tls_worklist;

/*
 * Set up a deque that can hold `capacity` packets. Rounded up to a power of
 * two. Returns false if the ring could not be allocated.
 */
bool cvm_worklist_init(CvmWorklist* w, u32 capacity);

void cvm_worklist_destroy(CvmWorklist* w);

/* Owner only. Never fails: the capacity is the packet pool's size. */
CVM_INLINE void cvm_worklist_push(CvmWorklist* w, CvmGreyPacket* packet) {
    i64 b = atomic_load_explicit(&w->bottom, memory_order_relaxed);

    /*
     * UnboundedWSQ refreshes its cached top here and grows the ring if it is
     * still full. The ring cannot be full -- a packet is in one place at a
     * time, and there are no more packets than slots -- so the refresh is all
     * that is left, and the growth is an assertion.
     */
    if (CVM_UNLIKELY(b - w->cached_top + 1 > w->mask + 1)) {
        w->cached_top = atomic_load_explicit(&w->top, memory_order_acquire);
        cvm_assert(b - w->cached_top + 1 <= w->mask + 1);
    }

    atomic_store_explicit(&w->slots[b & w->mask], packet, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&w->bottom, b + 1, memory_order_release);
}

/* Owner only. NULL when empty, or when a thief took the last packet. */
CVM_INLINE CvmGreyPacket* cvm_worklist_pop(CvmWorklist* w) {
    i64 b = atomic_load_explicit(&w->bottom, memory_order_relaxed) - 1;
    atomic_store_explicit(&w->bottom, b, memory_order_relaxed);
    atomic_thread_fence(memory_order_seq_cst);

    i64 t = atomic_load_explicit(&w->top, memory_order_relaxed);

    CvmGreyPacket* packet = NULL;

    if (t <= b) {
        packet = atomic_load_explicit(&w->slots[b & w->mask], memory_order_relaxed);
        if (t == b) {
            /* The last item: race the thieves for it. */
            if (!atomic_compare_exchange_strong_explicit(
                    &w->top, &t, t + 1, memory_order_seq_cst, memory_order_relaxed)) {
                packet = NULL;
            }
            atomic_store_explicit(&w->bottom, b + 1, memory_order_relaxed);
        }
    } else {
        atomic_store_explicit(&w->bottom, b + 1, memory_order_relaxed);
    }

    return packet;
}

/* Any thread but the owner. NULL when empty or when the race was lost. */
CVM_INLINE CvmGreyPacket* cvm_worklist_steal(CvmWorklist* w) {
    i64 t = atomic_load_explicit(&w->top, memory_order_acquire);
    atomic_thread_fence(memory_order_seq_cst);
    i64 b = atomic_load_explicit(&w->bottom, memory_order_acquire);

    CvmGreyPacket* packet = NULL;

    if (t < b) {
        /*
         * UnboundedWSQ loads the array pointer with memory_order_consume here,
         * because the array can be replaced. This one never is, so there is no
         * pointer to load and nothing for consume to order.
         */
        packet = atomic_load_explicit(&w->slots[t & w->mask], memory_order_relaxed);
        if (!atomic_compare_exchange_strong_explicit(
                &w->top, &t, t + 1, memory_order_seq_cst, memory_order_relaxed)) {
            return NULL;
        }
    }

    return packet;
}

/* Anyone. Racy by nature: a thief or the owner may change it immediately. */
CVM_INLINE bool cvm_worklist_empty(CvmWorklist const* w) {
    i64 t = atomic_load_explicit(&((CvmWorklist*)w)->top, memory_order_relaxed);
    i64 b = atomic_load_explicit(&((CvmWorklist*)w)->bottom, memory_order_relaxed);
    return b <= t;
}

/* Anyone. An upper bound on occupancy, for the same reason as `empty`. */
CVM_INLINE i64 cvm_worklist_size(CvmWorklist const* w) {
    i64 t = atomic_load_explicit(&((CvmWorklist*)w)->top, memory_order_relaxed);
    i64 b = atomic_load_explicit(&((CvmWorklist*)w)->bottom, memory_order_relaxed);
    return (b >= t) ? (b - t) : 0;
}

#endif /* CVM_GC_WORKLIST_H */
