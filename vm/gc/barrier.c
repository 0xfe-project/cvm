/*
 * vm/gc/barrier.c
 *
 * The slow path of the write barrier: what to do about a pointer store that
 * may have opened a window for the marker to miss something.
 *
 * The other half of this is the inline load in cvm/gc.h, because noticing is
 * on every pointer store the program makes and doing something about it is
 * rare. See the contract on cvm_store_field there.
 */

#include "cvm/gc.h"

#include "gc/grey_packet.h"
#include "gc/heap.h"
#include "gc/marker.h"
#include "gc/world.h"
#include "safepoint.h"

/*
 * Set by the collector for the duration of a mark phase, and read by the
 * inline barrier in cvm/gc.h on every pointer store the program makes. See
 * the declaration there for why it is not the safepoint flag.
 */
_Atomic bool cvm_marking_active = false;

/*
 * Mark a value that a running thread has just published.
 *
 * Called only while a collection is in progress -- the caller has already
 * loaded the flag that says so -- and only for a non-NULL value.
 *
 * This is the same shape as the root table's barrier in gc/root.c, and for
 * the same reason: both exist for threads a collection does not stop. The
 * difference is what the pointer came from. That one is a root being
 * registered; this one is a field being written. A third case, a frame slot
 * being written, is not covered here and cannot be: a frame slot is a root,
 * not a field, and the marker reaches it through the frame chain rather than
 * through a descriptor. What covers that one is the final root re-scan at
 * the end of a concurrent mark.
 */
void cvm_store_barrier_slow(opaque_mut value) {
    /*
     * Before the runtime is up there is no heap to mark into, and after it is
     * down there is nothing to keep alive. Both are ordinary during startup
     * and teardown, where a store can still be made.
     */
    if (!cvm_world_ready()) {
        return;
    }

    CvmHeap* heap = &cvm_world()->heap;

    /*
     * The caller has no packet of its own: it is a mutator in the middle of a
     * store, not a marker. Taking and returning one is cheaper than giving
     * every thread a holder it mostly does not need, and the packet lists are
     * lock-free, so this is safe from any thread.
     *
     * The value is usually already marked -- a program mostly stores pointers
     * to things that were reachable anyway -- and cvm_mark_field returns on the
     * first load when it is. The packets are still taken and given back, which
     * is two lock-free pushes.
     */
    CvmGreyPacket* out = cvm_mark_take_packet(heap);
    CvmGreyPacket* out_weak = cvm_mark_take_packet(heap);

    cvm_mark_field(heap, value, &out, &out_weak);

    cvm_mark_give_packet(heap, out, false);
    cvm_mark_give_packet(heap, out_weak, true);
}
