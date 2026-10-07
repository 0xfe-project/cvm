/*
 * tests/unit/test_bytemap.c
 *
 * Address -> ObjectMeta mapping. The bytemap is indexed by address
 * arithmetic, so the cases that matter are the slot boundaries: the first
 * slot, the last slot, and the exact edges between them.
 */

#include "vendor/greatest.h"

#include <stdlib.h>

#include "base/math.h"
#include "gc/bytemap.h"
#include "gc/layout.h"
#include "gc/meta.h"

/* A heap image plus a bytemap placed after it, mimicking the real layout
 * where both live in one mapping. */
typedef struct Fixture {
    u8* raw;
    u8* heap;
    usize heap_size;
    Bytemap* map;
} Fixture;

static Fixture fixture_make(usize heap_size) {
    /* room for the heap and the bytemap that describes it */
    usize raw_size = heap_size + bytemap_bytes_for(heap_size) + CVM_ALLOC_ALIGNMENT;
    u8* raw = malloc(raw_size);
    if (raw == NULL) {
        return (Fixture){NULL, NULL, 0, NULL};
    }

    u8* heap = (u8*)cvm_align_up((usize)raw, CVM_ALLOC_ALIGNMENT);
    u8* meta = heap + heap_size;

    Fixture f;
    f.raw = raw;
    f.heap = heap;
    f.heap_size = heap_size;
    f.map = bytemap_init(meta, heap, heap_size);

    /* bytemap_init does not clear the slots; the heap does that once at
     * startup, and so must the fixture, or tests would read whatever the
     * allocation happened to contain. */
    for (usize i = 0; i < f.map->count; i++) {
        object_meta_set_free(&f.map->slots[i]);
    }
    return f;
}

static void fixture_free(Fixture* f) {
    free(f->raw);
}

TEST bytemap_header_is_two_words(void) {
    ASSERT_EQ_FMT((u32)(2 * sizeof(void*)), (u32)sizeof(Bytemap), "%u");
    PASS();
}

TEST bytemap_size_accounts_for_header_and_slots(void) {
    usize heap_size = 4 * CVM_BLOCK_SIZE;
    usize expected_slots = heap_size / CVM_ALLOC_ALIGNMENT;

    ASSERT_EQ_FMT((u32)expected_slots,
                  (u32)bytemap_bytes_for(heap_size) - (u32)sizeof(Bytemap), "%u");
    PASS();
}

TEST bytemap_init_sets_base_and_count(void) {
    Fixture f = fixture_make(4 * CVM_BLOCK_SIZE);
    ASSERT(f.raw != NULL);

    ASSERT(f.map->first == f.heap);
    ASSERT_EQ_FMT((u32)(f.heap_size / CVM_ALLOC_ALIGNMENT), (u32)f.map->count, "%u");
    ASSERT(bytemap_end(f.map) == f.map->slots + f.map->count);
    ASSERT(bytemap_end_const(f.map) == bytemap_end(f.map));

    fixture_free(&f);
    PASS();
}

TEST bytemap_maps_the_first_slot(void) {
    Fixture f = fixture_make(4 * CVM_BLOCK_SIZE);
    ASSERT(f.raw != NULL);

    ASSERT_EQ_FMT(0u, (u32)bytemap_index_of(f.map, f.heap), "%u");
    ASSERT(bytemap_get(f.map, f.heap) == f.map->slots);

    fixture_free(&f);
    PASS();
}

TEST bytemap_maps_every_slot_boundary(void) {
    Fixture f = fixture_make(2 * CVM_BLOCK_SIZE);
    ASSERT(f.raw != NULL);

    /* walk the first and last few slots exactly on their boundaries */
    const usize probes[] = {
        0, 1, 2, 3, 15, 16, 17, f.map->count - 3, f.map->count - 2, f.map->count - 1};

    for (usize i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        usize index = probes[i];
        u8* addr = f.heap + index * CVM_ALLOC_ALIGNMENT;

        ASSERT_EQ_FMT((u32)index, (u32)bytemap_index_of(f.map, addr), "%u");
        ASSERT(bytemap_get(f.map, addr) == &f.map->slots[index]);
    }

    fixture_free(&f);
    PASS();
}

TEST bytemap_is_byte_granular_at_slot_edges(void) {
    Fixture f = fixture_make(CVM_BLOCK_SIZE);
    ASSERT(f.raw != NULL);

    /* two adjacent slots one byte inside the boundary */
    u8* a = f.heap + 3 * CVM_ALLOC_ALIGNMENT;
    u8* b = a + CVM_ALLOC_ALIGNMENT;

    ASSERT_EQ_FMT(3u, (u32)bytemap_index_of(f.map, a), "%u");
    ASSERT_EQ_FMT(4u, (u32)bytemap_index_of(f.map, b), "%u");
    /* the last byte of slot 3 still belongs to slot 3 */
    ASSERT_EQ_FMT(3u, (u32)bytemap_index_of(f.map, b - CVM_ALLOC_ALIGNMENT), "%u");

    fixture_free(&f);
    PASS();
}

TEST bytemap_mutable_and_const_views_agree(void) {
    Fixture f = fixture_make(CVM_BLOCK_SIZE);
    ASSERT(f.raw != NULL);

    u8* addr = f.heap + 7 * CVM_ALLOC_ALIGNMENT;

    object_meta_set_allocated(bytemap_get(f.map, addr));
    ASSERT(object_meta_is_allocated(bytemap_get_const(f.map, addr)));
    ASSERT_FALSE(object_meta_is_free(bytemap_get_const(f.map, addr)));

    object_meta_set_marked(bytemap_get(f.map, addr));
    ASSERT(object_meta_is_marked(bytemap_get_const(f.map, addr)));

    fixture_free(&f);
    PASS();
}

TEST bytemap_slots_do_not_alias(void) {
    Fixture f = fixture_make(CVM_BLOCK_SIZE);
    ASSERT(f.raw != NULL);

    /* mark slot 5 only, and check nothing else changed */
    object_meta_set_allocated(bytemap_get(f.map, f.heap + 5 * CVM_ALLOC_ALIGNMENT));

    ASSERT(object_meta_is_allocated(
        bytemap_get_const(f.map, f.heap + 5 * CVM_ALLOC_ALIGNMENT)));
    ASSERT(object_meta_is_free(
        bytemap_get_const(f.map, f.heap + 4 * CVM_ALLOC_ALIGNMENT)));
    ASSERT(object_meta_is_free(
        bytemap_get_const(f.map, f.heap + 6 * CVM_ALLOC_ALIGNMENT)));

    fixture_free(&f);
    PASS();
}

TEST bytemap_next_line_steps_by_a_line(void) {
    Fixture f = fixture_make(CVM_BLOCK_SIZE);
    ASSERT(f.raw != NULL);

    ObjectMeta* cursor = f.map->slots;
    for (u32 line = 0; line < 3; line++) {
        ASSERT(cursor == bytemap_get(f.map, f.heap + line * CVM_LINE_SIZE));
        cursor = bytemap_next_line(cursor);
    }
    /* three steps lands exactly on line 3 */
    ASSERT(cursor == bytemap_get(f.map, f.heap + 3 * CVM_LINE_SIZE));

    fixture_free(&f);
    PASS();
}

TEST bytemap_line_walk_covers_a_whole_block(void) {
    Fixture f = fixture_make(CVM_BLOCK_SIZE);
    ASSERT(f.raw != NULL);

    /* walking CVM_LINE_COUNT lines from the block base must land exactly
     * at the end of the block's metadata */
    ObjectMeta* cursor = f.map->slots;
    for (u32 i = 0; i < CVM_LINE_COUNT; i++) {
        cursor = bytemap_next_line(cursor);
    }
    ASSERT(cursor == f.map->slots + CVM_SLOTS_PER_BLOCK);

    fixture_free(&f);
    PASS();
}

TEST bytemap_clear_block_covers_exactly_one_block(void) {
    Fixture f = fixture_make(2 * CVM_BLOCK_SIZE);
    ASSERT(f.raw != NULL);

    /* dirty the whole second block's slots */
    for (usize i = 0; i < CVM_SLOTS_PER_BLOCK; i++) {
        object_meta_set_allocated(&f.map->slots[CVM_SLOTS_PER_BLOCK + i]);
    }

    object_meta_clear_block(&f.map->slots[CVM_SLOTS_PER_BLOCK]);

    for (usize i = 0; i < CVM_SLOTS_PER_BLOCK; i++) {
        ASSERT(object_meta_is_free(&f.map->slots[CVM_SLOTS_PER_BLOCK + i]));
    }
    /* the first block was never touched */
    ASSERT_EQ_FMT(0u, (u32)bytemap_index_of(f.map, f.heap), "%u");

    fixture_free(&f);
    PASS();
}

SUITE(bytemap_suite) {
    RUN_TEST(bytemap_header_is_two_words);
    RUN_TEST(bytemap_size_accounts_for_header_and_slots);
    RUN_TEST(bytemap_init_sets_base_and_count);
    RUN_TEST(bytemap_maps_the_first_slot);
    RUN_TEST(bytemap_maps_every_slot_boundary);
    RUN_TEST(bytemap_is_byte_granular_at_slot_edges);
    RUN_TEST(bytemap_mutable_and_const_views_agree);
    RUN_TEST(bytemap_slots_do_not_alias);
    RUN_TEST(bytemap_next_line_steps_by_a_line);
    RUN_TEST(bytemap_line_walk_covers_a_whole_block);
    RUN_TEST(bytemap_clear_block_covers_exactly_one_block);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(bytemap_suite);
    GREATEST_MAIN_END();
}
