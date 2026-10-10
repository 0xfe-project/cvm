/*
 * vm/gc/weak.c
 *
 * Weak references and the nullify phase. See include/cvm/weak.h for the shape
 * and why it is two phases.
 */

#include "cvm/weak.h"

#include "base/assert.h"
#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "gc/bytemap.h"
#include "gc/grey_packet.h"
#include "gc/heap.h"
#include "gc/marker.h"
#include "gc/world.h"
#include "gc/meta.h"
#include "log.h"
#include "rt.h"

/* ------------------------------------------------------------------ */
/* the built-in type                                                   */
/* ------------------------------------------------------------------ */

static CvmPtrField const weak_ref_fields[] = {
    {.offset = offsetof(CvmWeakRef, target)},
};

CvmTypeDescriptor const cvm_type_weak_ref = {
    .size = sizeof(CvmWeakRef),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 1,
    .num_embed_fields = 0,
    .ptr_fields = weak_ref_fields,
    .embed_fields = NULL,
};

/* ------------------------------------------------------------------ */
/* construction                                                        */
/* ------------------------------------------------------------------ */

opaque_mut cvm_weak_ref_new(opaque_mut target) {
    CvmWeakRef* weak = cvm_alloc(&cvm_type_weak_ref);
    weak->target = target;

    /* The flag is what makes the marker record rather than trace it. */
    ObjectMeta* meta =
        bytemap_get(cvm_world()->heap.bytemap, (u8 const*)cvm_header_of(weak));
    object_meta_set_weak(meta);
    return weak;
}

opaque_mut cvm_weak_ref_target(opaque_mut weak) {
    if (weak == NULL) {
        return NULL;
    }
    return ((CvmWeakRef*)weak)->target;
}

void cvm_weak_ref_set_target(opaque_mut weak, opaque_mut target) {
    cvm_assert(weak != NULL);
    ((CvmWeakRef*)weak)->target = target;
}

/* ------------------------------------------------------------------ */
/* nullify                                                             */
/* ------------------------------------------------------------------ */

/*
 * Clear one packet's worth of weak references.
 *
 * A reference is cleared when its target is not marked. Three cases look
 * the same to this code and are all "not marked":
 *
 *   - the target is NULL, which needs no action;
 *   - the target is outside the heap, which cannot be a live object;
 *   - the target is in the heap but its slot is not marked, so it is
 *     about to be swept.
 *
 * The check is a read of the mark bit rather than a mutation, so two
 * threads nullifying different packets never interfere; and nullifying
 * the same packet twice is harmless, which is what lets this run without
 * synchronising.
 */
static u32 nullify_packet(CvmHeap* heap, CvmGreyPacket* packet) {
    u32 cleared = 0;

    while (!cvm_grey_packet_is_empty(packet)) {
        /*
         * Packets hold object starts, not payloads — the same convention
         * the marker's other packets use. The payload is one header
         * further on, and getting this wrong reads the header as if it
         * were the referent.
         */
        u8* start = cvm_grey_packet_pop(packet);

        cvm_assert(
            object_meta_is_weak(bytemap_get_const(heap->bytemap, (u8 const*)start)));

        CvmWeakRef* weak = (CvmWeakRef*)(start + sizeof(CvmGcHeader));
        opaque_mut target = weak->target;
        if (target != NULL && !cvm_object_is_marked(heap, target)) {
            weak->target = NULL;
            cleared++;
        }
    }

    return cleared;
}

u32 cvm_weak_nullify(CvmHeap* heap) {
    u32 cleared = 0;

    for (;;) {
        CvmGreyPacket* packet =
            cvm_grey_list_pop(&heap->mark.found_weak_refs, heap->mark.packets);
        if (packet == NULL) {
            break;
        }

        cleared += nullify_packet(heap, packet);

        /* Back to the empty list, because the packet itself is now
         * available again and the completion check counts it as such. */
        cvm_grey_list_push(&heap->mark.empty, heap->mark.packets, packet);
    }

    return cleared;
}

bool cvm_weak_is_done(CvmHeap* heap) {
    u32 empty = cvm_grey_list_size(&heap->mark.empty);
    return empty == heap->mark.packet_count;
}
