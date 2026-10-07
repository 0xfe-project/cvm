/*
 * tests/unit/test_block_list.c
 *
 * Block stacks. The single-threaded tests pin the list semantics; the
 * concurrent ones exercise the compare-exchange loops and the
 * acquire/release pairing between a pusher's next_block write and a
 * popper's read of it.
 *
 * The concurrent tests deliberately respect the usage invariant that
 * block_list.c documents: every block is pushed exactly once and popped
 * exactly once, so a block is never re-pushed while another thread holds a
 * stale head. That is the invariant the ABA argument rests on. A test that
 * violated it would be testing behaviour the design does not claim.
 */

#include "vendor/greatest.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "base/types.h"
#include "gc/block_list.h"
#include "gc/meta.h"

/* ------------------------------------------------------------------ */
/* single-threaded                                                     */
/* ------------------------------------------------------------------ */

static BlockMeta* metas_alloc(u32 count) {
    BlockMeta* m = malloc(count * sizeof(BlockMeta));
    for (u32 i = 0; i < count; i++) {
        block_meta_clear(&m[i]);
    }
    return m;
}

TEST empty_list_pops_null(void) {
    BlockMeta* metas = metas_alloc(4);
    BlockList list;
    block_list_init(&list);

    ASSERT(block_list_pop(&list, metas) == NULL);
    ASSERT(block_list_pop_only_thread(&list, metas) == NULL);

    free(metas);
    PASS();
}

TEST push_then_pop_is_lifo(void) {
    BlockMeta* metas = metas_alloc(4);
    BlockList list;
    block_list_init(&list);

    for (u32 i = 0; i < 4; i++) {
        block_list_push(&list, metas, &metas[i]);
    }

    /* last pushed comes back first */
    for (u32 i = 4; i > 0; i--) {
        BlockMeta* got = block_list_pop(&list, metas);
        ASSERT(got == &metas[i - 1]);
    }
    ASSERT(block_list_pop(&list, metas) == NULL);

    free(metas);
    PASS();
}

TEST push_all_links_a_run_in_one_step(void) {
    BlockMeta* metas = metas_alloc(8);
    BlockList list;
    block_list_init(&list);

    /* build a run 0 -> 1 -> 2 -> 3 locally */
    for (u32 i = 0; i < 3; i++) {
        block_meta_set_next(metas, &metas[i], &metas[i + 1]);
    }
    block_meta_set_next(metas, &metas[3], NULL);

    block_list_push_all(&list, metas, &metas[0], &metas[3]);

    /* popping walks the run in order: 0, 1, 2, 3 */
    for (u32 i = 0; i < 4; i++) {
        ASSERT(block_list_pop(&list, metas) == &metas[i]);
    }
    ASSERT(block_list_pop(&list, metas) == NULL);

    free(metas);
    PASS();
}

TEST pop_only_thread_matches_concurrent_pop_semantics(void) {
    BlockMeta* metas = metas_alloc(4);
    BlockList list;
    block_list_init(&list);

    block_list_push(&list, metas, &metas[1]);
    block_list_push(&list, metas, &metas[2]);

    ASSERT(block_list_pop_only_thread(&list, metas) == &metas[2]);
    ASSERT(block_list_pop_only_thread(&list, metas) == &metas[1]);
    ASSERT(block_list_pop_only_thread(&list, metas) == NULL);

    free(metas);
    PASS();
}

TEST local_block_list_accumulates_then_hands_over(void) {
    BlockMeta* metas = metas_alloc(8);
    LocalBlockList local;
    local_block_list_clear(&local);

    ASSERT(local_block_list_is_empty(&local));

    for (u32 i = 0; i < 4; i++) {
        local_block_list_push(&local, metas, &metas[i]);
    }
    ASSERT_FALSE(local_block_list_is_empty(&local));
    /* last pushed is the head of the local run */
    ASSERT(local.first == &metas[3]);
    ASSERT(local.last == &metas[0]);

    BlockList shared;
    block_list_init(&shared);
    block_list_push_all(&shared, metas, local.first, local.last);

    /* handover preserves the local order: 3, 2, 1, 0 */
    for (u32 i = 4; i > 0; i--) {
        ASSERT(block_list_pop(&shared, metas) == &metas[i - 1]);
    }
    ASSERT(block_list_pop(&shared, metas) == NULL);

    free(metas);
    PASS();
}

TEST local_block_list_single_push_sets_both_ends(void) {
    BlockMeta* metas = metas_alloc(2);
    LocalBlockList local;
    local_block_list_clear(&local);

    local_block_list_push(&local, metas, &metas[1]);
    ASSERT(local.first == &metas[1]);
    ASSERT(local.last == &metas[1]);
    ASSERT(block_meta_next(metas, &metas[1]) == NULL);

    free(metas);
    PASS();
}

TEST clear_empties_the_list(void) {
    BlockMeta* metas = metas_alloc(4);
    BlockList list;
    block_list_init(&list);

    block_list_push(&list, metas, &metas[0]);
    block_list_clear(&list);
    ASSERT(block_list_pop(&list, metas) == NULL);

    free(metas);
    PASS();
}

/* ------------------------------------------------------------------ */
/* concurrent                                                          */
/* ------------------------------------------------------------------ */

#define N_PRODUCERS 2
#define N_CONSUMERS 2
#define PER_PRODUCER 512
#define TOTAL_BLOCKS (N_PRODUCERS * PER_PRODUCER)

typedef struct {
    BlockList* list;
    BlockMeta* metas;
    u32 base;
    u32 count;
} Producer;

static void* producer_main(void* arg) {
    Producer* p = arg;
    for (u32 i = 0; i < p->count; i++) {
        block_list_push(p->list, p->metas, &p->metas[p->base + i]);
    }
    return NULL;
}

typedef struct {
    BlockList* list;
    BlockMeta* metas;
    _Atomic(u32)* remaining;
    _Atomic(u32)* seen;
    u32 popped;
} Consumer;

static void* consumer_main(void* arg) {
    Consumer* c = arg;
    for (;;) {
        BlockMeta* m = block_list_pop(c->list, c->metas);
        if (m == NULL) {
            if (atomic_load_explicit(c->remaining, memory_order_acquire) == 0) {
                return NULL;
            }
            continue; /* producers still have blocks to hand over */
        }

        u32 index = block_meta_index_of(c->metas, m);
        atomic_fetch_add_explicit(&c->seen[index], 1u, memory_order_relaxed);
        atomic_fetch_sub_explicit(c->remaining, 1u, memory_order_release);
        c->popped++;
    }
}

TEST concurrent_producers_and_consumers_lose_nothing(void) {
    BlockMeta* metas = metas_alloc(TOTAL_BLOCKS);
    BlockList list;
    block_list_init(&list);

    _Atomic(u32) remaining = TOTAL_BLOCKS;
    _Atomic(u32)* seen = calloc(TOTAL_BLOCKS, sizeof(_Atomic(u32)));

    Producer producers[N_PRODUCERS];
    Consumer consumers[N_CONSUMERS];
    pthread_t ptid[N_PRODUCERS];
    pthread_t ctid[N_CONSUMERS];

    for (u32 i = 0; i < N_PRODUCERS; i++) {
        producers[i] = (Producer){&list, metas, i * PER_PRODUCER, PER_PRODUCER};
        pthread_create(&ptid[i], NULL, producer_main, &producers[i]);
    }
    for (u32 i = 0; i < N_CONSUMERS; i++) {
        consumers[i] = (Consumer){&list, metas, &remaining, seen, 0};
        pthread_create(&ctid[i], NULL, consumer_main, &consumers[i]);
    }

    for (u32 i = 0; i < N_PRODUCERS; i++)
        pthread_join(ptid[i], NULL);
    for (u32 i = 0; i < N_CONSUMERS; i++)
        pthread_join(ctid[i], NULL);

    ASSERT_EQ_FMT(0u, (u32)atomic_load(&remaining), "%u");

    u32 total = 0;
    u32 duplicated = 0;
    u32 missed = 0;
    for (u32 i = 0; i < TOTAL_BLOCKS; i++) {
        u32 n = atomic_load(&seen[i]);
        total += n;
        if (n > 1)
            duplicated++;
        if (n == 0)
            missed++;
    }

    ASSERT_EQ_FMT(TOTAL_BLOCKS, total, "%u");
    ASSERT_EQ_FMT(0u, duplicated, "%u");
    ASSERT_EQ_FMT(0u, missed, "%u");

    free(seen);
    free(metas);
    PASS();
}

TEST concurrent_pushes_are_all_visible_after_join(void) {
    /* Phase-separated: concurrent pushes, then concurrent pops. Exercises
     * the release/acquire pairing across a thread boundary. */
    BlockMeta* metas = metas_alloc(TOTAL_BLOCKS);
    BlockList list;
    block_list_init(&list);

    Producer producers[N_PRODUCERS];
    pthread_t ptid[N_PRODUCERS];

    for (u32 i = 0; i < N_PRODUCERS; i++) {
        producers[i] = (Producer){&list, metas, i * PER_PRODUCER, PER_PRODUCER};
        pthread_create(&ptid[i], NULL, producer_main, &producers[i]);
    }
    for (u32 i = 0; i < N_PRODUCERS; i++)
        pthread_join(ptid[i], NULL);

    _Atomic(u32)* seen = calloc(TOTAL_BLOCKS, sizeof(_Atomic(u32)));
    u32 popped = 0;
    BlockMeta* m;
    while ((m = block_list_pop(&list, metas)) != NULL) {
        u32 index = block_meta_index_of(metas, m);
        atomic_fetch_add(&seen[index], 1u);
        popped++;
    }

    ASSERT_EQ_FMT(TOTAL_BLOCKS, popped, "%u");

    u32 missed = 0;
    for (u32 i = 0; i < TOTAL_BLOCKS; i++) {
        if (atomic_load(&seen[i]) != 1u)
            missed++;
    }
    ASSERT_EQ_FMT(0u, missed, "%u");

    free(seen);
    free(metas);
    PASS();
}

SUITE(block_list_suite) {
    RUN_TEST(empty_list_pops_null);
    RUN_TEST(push_then_pop_is_lifo);
    RUN_TEST(push_all_links_a_run_in_one_step);
    RUN_TEST(pop_only_thread_matches_concurrent_pop_semantics);
    RUN_TEST(local_block_list_accumulates_then_hands_over);
    RUN_TEST(local_block_list_single_push_sets_both_ends);
    RUN_TEST(clear_empties_the_list);
    RUN_TEST(concurrent_producers_and_consumers_lose_nothing);
    RUN_TEST(concurrent_pushes_are_all_visible_after_join);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(block_list_suite);
    GREATEST_MAIN_END();
}
