/*
 * vm/gc/marker.h
 *
 * The marker, ported from refs/commix/Marker.c.
 *
 * ---------------------------------------------------------------------
 * What is different from the reference, and why it matters
 * ---------------------------------------------------------------------
 *
 * The reference scans stacks conservatively: it walks a thread's stack
 * word by word and treats anything that looks like a heap pointer as a
 * root, then walks backwards from interior pointers to find the object
 * they are inside (Object_getInnerPointer, Object_GetUnmarkedObject).
 *
 * This marker does none of that. Roots come from each thread's frame
 * chain, which the thread maintains itself and which holds exactly the
 * live pointers. Object fields are walked through the type descriptor,
 * which says exactly which offsets hold pointers.
 *
 * The consequences are worth stating:
 *
 *   - No interior-pointer search. In a precise collector a reference is
 *     always to an object's start, so a pointer either names an object or
 *     is not a heap pointer at all. The whole backwards-walk machinery
 *     has no purpose here.
 *
 *   - No false roots. An integer that happens to look like an address
 *     cannot keep an object alive, so there is no floating garbage from
 *     that source.
 *
 *   - Marking is not a function of stack contents, so a thread that is
 *     stopped mid-expression marks the same things as one stopped
 *     anywhere else. The roots are the frames, not the stack.
 *
 * What remains is the reference's shape: mark an object's metadata, put
 * it in a packet, and trace its fields when the packet comes up.
 */

#ifndef CVM_GC_MARKER_H
#define CVM_GC_MARKER_H

#include "base/types.h"
#include "gc/grey_packet.h"
#include "gc/heap.h"

/*
 * Mark everything reachable from the roots.
 *
 * Roots are every registered thread's frame chain. Must be called with
 * the world stopped: a thread that is running can be adding frames and
 * changing slots, and the walk would see a half-updated chain.
 */
void cvm_mark_roots(CvmHeap* heap);

/*
 * Drain the full packet list, tracing objects until nothing is left to
 * trace. Safe to call from more than one thread; the packet lists are
 * lock-free.
 */
void cvm_mark_until_done(CvmHeap* heap);

/*
 * Whether marking has nothing left to do.
 *
 * True when every packet is accounted for: on the empty list or on the
 * weak-reference list. A packet on the full list means there is work.
 */
bool cvm_mark_is_done(CvmHeap* heap);

/* Mark one object and enqueue it for tracing. */
void cvm_mark_object(CvmHeap* heap, u8* start, CvmGreyPacket** out,
                     CvmGreyPacket** out_weak);

/* Mark a pointer value, if it names an object in the heap. */
void cvm_mark_field(CvmHeap* heap, opaque_mut field, CvmGreyPacket** out,
                    CvmGreyPacket** out_weak);

/*
 * A packet to mark into, for a caller outside the marking loop.
 *
 * The root table's write barrier needs to mark a single object from an
 * arbitrary thread while a collection is running, and that thread has no
 * packet of its own. Taking and returning one is cheaper than making every
 * marker keep a holder it mostly does not need, and the packet lists are
 * lock-free so it is safe from any thread.
 */
CvmGreyPacket* cvm_mark_take_packet(CvmHeap* heap);

/* Give a packet back. An empty one goes to the empty list; a non-empty
 * one to the full list, or to the weak list when `weak` is set. */
void cvm_mark_give_packet(CvmHeap* heap, CvmGreyPacket* packet, bool weak);

/* Trace one object's fields through its type descriptor. */
void cvm_trace_object(CvmHeap* heap, u8* start, CvmGreyPacket** out,
                      CvmGreyPacket** out_weak);

/*
 * Whether an address names a live object that has not been marked yet,
 * and mark it if so. Exposed because the nullify phase needs the same
 * question answered without marking anything.
 */
bool cvm_object_is_marked(CvmHeap* heap, opaque_mut payload);

#endif /* CVM_GC_MARKER_H */
