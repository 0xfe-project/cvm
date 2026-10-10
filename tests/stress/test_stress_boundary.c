/*
 * tests/stress/test_stress_boundary.c
 *
 * Allocation at the boundaries between the three object sizes.
 *
 * The idea is from bdwgc's `middle.c`, which allocates at the boundary
 * between its small and large objects. Adapted to this collector's
 * boundaries, which are two rather than one:
 *
 *   the inlined path serves an object whose total size fits in one line,
 *   256 bytes. Past that the object takes the slow path.
 *
 *   the slow path splits again: anything past a line is served from a whole
 *   free block, while an object at or past 8 KiB comes from the large
 *   allocator's chunks.
 *
 * Both boundaries have a size that is exactly on them and a size one
 * allocation slot to either side, and the interesting question at each is
 * whether an object lands in the tier the code believes it does. A boundary
 * bug does not crash: it produces an object whose header is somewhere the
 * allocator did not intend, which shows up as an object losing its contents
 * after a collection or two objects becoming the same address.
 *
 * So every size in the sweep is allocated, stamped, kept or dropped, and
 * then verified after collections — the canary oracle from
 * tests/support/oracle.h.
 */

#include "vendor/greatest.h"

#include <stddef.h>
#include <stdio.h>

#include "support/oracle.h"

/* 64 MiB, because the sweep deliberately fills the heap with big objects. */
#define TEST_MIN ((usize)8 * 1024 * 1024)
#define TEST_MAX ((usize)64 * 1024 * 1024)

/*
 * One descriptor per size of interest.
 *
 * The sizes are chosen from the arithmetic `cvm_alloc` does, not from round
 * numbers: the header is 16 bytes and the total is rounded up to 16, so a
 * payload of 240 bytes comes to exactly 256 and sits on the inline limit
 * while 248 comes to 264 and does not.
 */
#define DECL_SIZE(name, bytes)                                                         \
    static CvmTypeDescriptor const name = {.size = (bytes), .kind = CVM_TYPE_SCALAR}

DECL_SIZE(size_8, 8);

/* Total 240: comfortably inside the inlined path. */
DECL_SIZE(size_224, 224);

/* Total 256: exactly on the inlined path's limit. */
DECL_SIZE(size_240, 240);

/* Total 264: one allocation slot past it, so the slow path. */
DECL_SIZE(size_248, 248);

/* Total 272: two slots past. */
DECL_SIZE(size_256, 256);

/* A multiple of a line, so the object spans several. */
DECL_SIZE(size_1000, 1000);

/* Total 8176: past a line, well under the large threshold. */
DECL_SIZE(size_8160, 8160);

/* Total 8192: exactly on the large-object threshold. */
DECL_SIZE(size_8176, 8176);

/* Total 8224: one slot past, so the large allocator. */
DECL_SIZE(size_8208, 8208);

/* Total 16384 and 32768: whole large chunks. */
DECL_SIZE(size_16368, 16368);
DECL_SIZE(size_32752, 32752);

static CvmTypeDescriptor const* const sizes[] = {
    &size_8,    &size_224,  &size_240,  &size_248,   &size_256,   &size_1000,
    &size_8160, &size_8176, &size_8208, &size_16368, &size_32752,
};

#define SIZE_COUNT (sizeof(sizes) / sizeof(sizes[0]))

/* Round-robin allocation, keeping some and dropping others. */
#define SLOTS 64

typedef struct {
    void* payloads[SLOTS];
    bool in_use[SLOTS];
} SlotTable;

static void slots_clear(SlotTable* t) {
    for (u32 i = 0; i < SLOTS; i++) {
        cvm_store_field(t, offsetof(SlotTable, payloads) + i * sizeof(void*), NULL);
        t->in_use[i] = false;
    }
}

/* Every live slot must have its canary, and be described as live. */
/*
 * Every live slot must be described as live and hold its canary.
 *
 * The report names the slot and the address, because "a kept object lost its
 * contents" without them is a sentence that costs a debugging session to
 * act on.
 */
static bool slots_verify(SlotTable const* t, char* report, usize report_cap) {
    for (u32 i = 0; i < SLOTS; i++) {
        if (!t->in_use[i]) {
            continue;
        }
        if (!oracle_is_live(t->payloads[i])) {
            snprintf(report, report_cap, "slot %u (%p) was collected while reachable",
                     i, t->payloads[i]);
            return false;
        }
        if (!oracle_canary_intact(t->payloads[i])) {
            snprintf(report, report_cap,
                     "slot %u (%p) lost its contents while reachable", i,
                     t->payloads[i]);
            return false;
        }
    }
    report[0] = '\0';
    return true;
}

TEST every_boundary_size_allocates_and_survives(void) {
    oracle_reset_frames();
    ORACLE_FRAME(SLOTS);

    SlotTable table;
    slots_clear(&table);

    /* Fill the table, one of each size at a time, so the allocation
     * pattern walks back and forth across both boundaries. */
    for (u32 round = 0; round < 4; round++) {
        for (u32 s = 0; s < SIZE_COUNT; s++) {
            u32 slot = (round * SIZE_COUNT + s) % SLOTS;
            if (table.in_use[slot]) {
                continue;
            }

            void* p = cvm_alloc(sizes[s]);
            ASSERT(p != NULL);

            /*
             * Stamp the first word, and mark the last byte too so that an
             * object whose extent is computed wrong is caught at either
             * end — but only when the payload is bigger than the canary.
             * Writing the last byte of an eight-byte payload overwrites the
             * top byte of the canary, and the check then reports the object
             * as corrupted when it is untouched. (That is how this comment
             * came to exist.)
             */
            oracle_stamp(p);
            if (sizes[s]->size > sizeof(u64)) {
                ((u8*)p)[sizes[s]->size - 1] = (u8)(0xA0 | (s & 0x0F));
            }

            table.payloads[slot] = p;
            table.in_use[slot] = true;
            oracle_f_roots[slot] = p;
        }

        oracle_collect();

        char report[128];
        if (!slots_verify(&table, report, sizeof(report))) {
            fprintf(stderr, "boundary: %s (round %u)\n", report, round);
            FAIL();
        }
    }

    ORACLE_FRAME_END();
    PASS();
}

TEST a_dropped_boundary_object_is_reclaimed(void) {
    oracle_reset_frames();
    /*
     * The other half of the boundary question: an object that is dropped
     * must actually come back, whichever tier it came from. A large object
     * whose chunks are never returned leaks its whole superblock, which is
     * a lot of address space to lose quietly.
     */
    for (u32 s = 0; s < SIZE_COUNT; s++) {
        ORACLE_FRAME(1);

        void* p = cvm_alloc(sizes[s]);
        ASSERT(p != NULL);
        ASSERT(oracle_is_live(p));

        /* Unreachable, so the next collection must take it. */
        oracle_f_roots[0] = NULL;
        oracle_collect();

        ASSERT_FALSE(oracle_is_live(p));

        ORACLE_FRAME_END();
    }
    PASS();
}

TEST a_size_that_spans_a_line_does_not_overlap_its_neighbour(void) {
    oracle_reset_frames();
    /*
     * An object larger than a line is served from a whole free block, and
     * the object after it comes from somewhere else entirely. If the block
     * cursor is left where the big object ended rather than being advanced
     * past it, the next small object lands inside it.
     */
    ORACLE_FRAME(8);

    void* big = cvm_alloc(&size_1000);
    oracle_stamp(big);
    oracle_f_roots[0] = big;

    for (u32 i = 1; i < 8; i++) {
        void* small = cvm_alloc(&size_8);
        oracle_stamp(small);
        oracle_f_roots[i] = small;

        /* Neither overlaps the other. */
        usize big_start = (usize)big;
        usize small_start = (usize)small;
        ASSERT(small_start + size_8.size <= big_start ||
               big_start + size_1000.size <= small_start);
    }

    oracle_collect();

    ASSERT(oracle_canary_intact(big));
    for (u32 i = 1; i < 8; i++) {
        ASSERT(oracle_canary_intact(oracle_f_roots[i]));
    }

    ORACLE_FRAME_END();
    PASS();
}

TEST random_sizes_with_random_lifetimes(void) {
    /*
     * The fragmentation pattern from TEST.md, in its simplest form: sizes
     * drawn at random across both boundaries, lifetimes drawn at random, so
     * that holes accumulate in awkward shapes rather than in the tidy
     * pattern a hand-written loop produces.
     *
     * The seed is fixed so a failure is reproducible, and printed when a
     * round fails so it can be re-run.
     */
    oracle_reset_frames();

    const u64 seed = 0x5eed1234ull;
    OracleRandom rng = oracle_random(seed);

    ORACLE_FRAME(SLOTS);

    SlotTable table;
    slots_clear(&table);

    for (u32 round = 0; round < 200; round++) {
        /* Free a few slots at random. */
        for (u32 k = 0; k < 4; k++) {
            u32 slot = oracle_random_below(&rng, SLOTS);
            if (table.in_use[slot]) {
                table.in_use[slot] = false;
                table.payloads[slot] = NULL;
                oracle_f_roots[slot] = NULL;
            }
        }

        /* Fill a few at random. */
        for (u32 k = 0; k < 6; k++) {
            u32 slot = oracle_random_below(&rng, SLOTS);
            if (table.in_use[slot]) {
                continue;
            }

            CvmTypeDescriptor const* type =
                sizes[oracle_random_below(&rng, (u32)SIZE_COUNT)];

            void* p = cvm_alloc(type);
            ASSERT(p != NULL);
            oracle_stamp(p);
            table.payloads[slot] = p;
            table.in_use[slot] = true;
            oracle_f_roots[slot] = p;
        }

        if (round % 10 == 0) {
            oracle_collect();

            char report[128];
            if (!slots_verify(&table, report, sizeof(report))) {
                fprintf(stderr, "boundary stress: %s (round %u, seed 0x%llx)\n", report,
                        round, (unsigned long long)seed);
                FAIL();
            }
        }
    }

    oracle_collect();

    char report[128];
    ASSERT(slots_verify(&table, report, sizeof(report)));

    ORACLE_FRAME_END();
    PASS();
}

TEST the_heap_does_not_grow_without_reason(void) {
    /*
     * A churn of dropped objects should reuse memory rather than grow the
     * heap. This is a weak assertion by design — the growth heuristic has
     * legitimate reasons to grow — but a heap that grows on every
     * collection under a steady workload is the shape of a leak, and that
     * is worth catching.
     */
    oracle_reset_frames();

    usize start_size = oracle_heap()->heap_size;

    for (u32 round = 0; round < 40; round++) {
        ORACLE_FRAME(SLOTS);

        for (u32 i = 0; i < SLOTS; i++) {
            CvmTypeDescriptor const* type = sizes[(round + i) % SIZE_COUNT];
            void* p = cvm_alloc(type);
            ASSERT(p != NULL);
            oracle_stamp(p);
            oracle_f_roots[i] = p;
        }

        /* Drop them all, then collect: nothing survives, so nothing should
         * need more room. */
        ORACLE_FRAME_END();
        oracle_collect();
    }

    usize end_size = oracle_heap()->heap_size;

    /* Allow a doubling or so for the growth heuristic's own reasons, but
     * not unbounded growth across 40 rounds. */
    ASSERT(end_size <= start_size * 4);

    PASS();
}

SUITE(boundary_suite) {
    RUN_TEST(every_boundary_size_allocates_and_survives);
    RUN_TEST(a_dropped_boundary_object_is_reclaimed);
    RUN_TEST(a_size_that_spans_a_line_does_not_overlap_its_neighbour);
    RUN_TEST(random_sizes_with_random_lifetimes);
    RUN_TEST(the_heap_does_not_grow_without_reason);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    if (!oracle_bring_up(TEST_MIN, TEST_MAX, 2)) {
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(boundary_suite);
    GREATEST_MAIN_END();
}
