/*
 * bench/scenarios/shapes.c
 *
 * The object shapes, in one place.
 *
 * Declared once so that `small_node` in one scenario is the same object as
 * `small_node` in another, and so that the pointer offsets are data rather
 * than a comment: a precise collector reads them, a conservative one scans
 * the whole object and finds the same pointers.
 *
 * Sizes are multiples of 16 so that every shape is aligned the way generated
 * code would align it, and so that a shape cannot accidentally measure an
 * allocator's rounding rather than its own cost.
 */

#include "scenario.h"

/*
 * 24 bytes, 2 pointers. The common case.
 *
 * Pointer count and bytes both small, which is what most real objects are.
 */
BenchShape const shape_small_node = {
    .name = "small_node",
    .size = 24,
    .nptrs = 2,
    .ptr_offsets = {0, 8},
};

/*
 * 136 bytes, the same 2 pointers. The controlled pair with small_node.
 *
 * Identical marking work per object, five times the bytes. A collector that
 * counts objects shows no difference between the two; one that cares about
 * bytes does. The ratio between them is the cleanest statement in the suite
 * about whether line-and-block granularity is doing anything.
 */
BenchShape const shape_fat_node = {
    .name = "fat_node",
    .size = 136,
    .nptrs = 2,
    .ptr_offsets = {0, 8},
};

/* 96 bytes, 8 pointers spread through the object. Every 8-byte slot is a
 * pointer, so a write barrier taxes almost every store. */
BenchShape const shape_wide_node = {
    .name = "wide_node",
    .size = 96,
    .nptrs = 8,
    .ptr_offsets = {0, 8, 16, 24, 32, 40, 48, 56},
};

/* 128 bytes, no pointers. The control: a tracing collector should do almost
 * nothing here, so a backend that is slow on this scenario is slow in its
 * allocator or its safepoint mechanism rather than in its marking. */
BenchShape const shape_leaf_blob = {
    .name = "leaf_blob",
    .size = 128,
    .nptrs = 0,
    .ptr_offsets = {0},
};
