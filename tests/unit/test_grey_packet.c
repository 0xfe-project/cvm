/*
 * tests/unit/test_grey_packet.c
 *
 * Grey packets and grey lists.
 *
 * The property worth the most attention is the one the packed head exists
 * for: the count must never be larger than the number of packets actually
 * on the list. Marking decides it is finished when the count reaches zero,
 * so a count that is briefly too high is a correctness bug, not a
 * rounding error. Several tests check the pair rather than the parts.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "base/types.h"
#include "gc/grey_packet.h"

/* ------------------------------------------------------------------ */
/* packets                                                             */
/* ------------------------------------------------------------------ */

TEST a_packet_starts_empty(void) {
    CvmGreyPacket p;
    cvm_grey_packet_init(&p, CVM_GREY_PACKET_REFLIST);

    ASSERT(cvm_grey_packet_is_empty(&p));
    ASSERT_FALSE(cvm_grey_packet_is_full(&p));
    ASSERT_EQ_FMT((u32)CVM_GREY_PACKET_REFLIST, (u32)p.type, "%u");
    PASS();
}

TEST packet_push_and_pop_are_lifo(void) {
    CvmGreyPacket p;
    cvm_grey_packet_init(&p, CVM_GREY_PACKET_REFLIST);

    int a = 1, b = 2, c = 3;
    ASSERT(cvm_grey_packet_push(&p, &a));
    ASSERT(cvm_grey_packet_push(&p, &b));
    ASSERT(cvm_grey_packet_push(&p, &c));
    ASSERT_EQ_FMT(3u, (u32)p.size, "%u");

    ASSERT(cvm_grey_packet_pop(&p) == &c);
    ASSERT(cvm_grey_packet_pop(&p) == &b);
    ASSERT(cvm_grey_packet_pop(&p) == &a);
    ASSERT(cvm_grey_packet_is_empty(&p));
    PASS();
}

TEST a_packet_refuses_to_overflow(void) {
    CvmGreyPacket p;
    cvm_grey_packet_init(&p, CVM_GREY_PACKET_REFLIST);

    static int values[CVM_GREY_PACKET_ITEMS + 8];

    for (u32 i = 0; i < CVM_GREY_PACKET_ITEMS; i++) {
        ASSERT(cvm_grey_packet_push(&p, &values[i]));
    }
    ASSERT(cvm_grey_packet_is_full(&p));

    /* one more must be refused, not written past the end */
    ASSERT_FALSE(cvm_grey_packet_push(&p, &values[CVM_GREY_PACKET_ITEMS]));
    ASSERT_EQ_FMT((u32)CVM_GREY_PACKET_ITEMS, (u32)p.size, "%u");
    PASS();
}

TEST moving_items_takes_from_the_end_of_the_source(void) {
    CvmGreyPacket src;
    CvmGreyPacket dst;
    cvm_grey_packet_init(&src, CVM_GREY_PACKET_REFLIST);
    cvm_grey_packet_init(&dst, CVM_GREY_PACKET_REFLIST);

    static int values[10];
    for (int i = 0; i < 6; i++) {
        cvm_grey_packet_push(&src, &values[i]);
    }

    cvm_grey_packet_move_items(&src, &dst, 2);

    ASSERT_EQ_FMT(4u, (u32)src.size, "%u");
    ASSERT_EQ_FMT(2u, (u32)dst.size, "%u");
    /* the last two of the source, in order */
    ASSERT(dst.items[0] == &values[4]);
    ASSERT(dst.items[1] == &values[5]);
    PASS();
}

/* ------------------------------------------------------------------ */
/* lists                                                               */
/* ------------------------------------------------------------------ */

/* A small packet array, as a collection would allocate once up front. */
#define PACKET_COUNT 8

typedef struct {
    CvmGreyPacket packets[PACKET_COUNT];
    CvmGreyList list;
} Pool;

static void pool_init(Pool* pool) {
    for (u32 i = 0; i < PACKET_COUNT; i++) {
        cvm_grey_packet_init(&pool->packets[i], CVM_GREY_PACKET_REFLIST);
    }
    cvm_grey_list_init(&pool->list);
}

TEST a_new_list_is_empty(void) {
    Pool pool;
    pool_init(&pool);

    ASSERT_EQ_FMT(0u, cvm_grey_list_size(&pool.list), "%u");
    ASSERT(cvm_grey_list_pop(&pool.list, pool.packets) == NULL);
    PASS();
}

TEST pushing_one_packet_makes_the_count_one(void) {
    Pool pool;
    pool_init(&pool);

    cvm_grey_list_push(&pool.list, pool.packets, &pool.packets[3]);
    ASSERT_EQ_FMT(1u, cvm_grey_list_size(&pool.list), "%u");

    CvmGreyPacket* got = cvm_grey_list_pop(&pool.list, pool.packets);
    ASSERT(got == &pool.packets[3]);
    ASSERT_EQ_FMT(0u, cvm_grey_list_size(&pool.list), "%u");
    PASS();
}

TEST pushing_several_packets_pops_them_in_reverse(void) {
    Pool pool;
    pool_init(&pool);

    for (u32 i = 0; i < PACKET_COUNT; i++) {
        cvm_grey_list_push(&pool.list, pool.packets, &pool.packets[i]);
        ASSERT_EQ_FMT(i + 1, cvm_grey_list_size(&pool.list), "%u");
    }

    for (u32 i = PACKET_COUNT; i > 0; i--) {
        CvmGreyPacket* got = cvm_grey_list_pop(&pool.list, pool.packets);
        ASSERT(got == &pool.packets[i - 1]);
        ASSERT_EQ_FMT(i - 1, cvm_grey_list_size(&pool.list), "%u");
    }
    ASSERT(cvm_grey_list_pop(&pool.list, pool.packets) == NULL);
    PASS();
}

TEST push_all_links_a_run_without_touching_each_packet(void) {
    /*
     * A contiguous run is represented by a single link on its last packet:
     * the rest are found by adding one to the index. This is what makes
     * handing back a batch of empty packets cheap.
     */
    Pool pool;
    pool_init(&pool);

    cvm_grey_list_push_all(&pool.list, pool.packets, &pool.packets[2], 4);
    ASSERT_EQ_FMT(4u, cvm_grey_list_size(&pool.list), "%u");

    /* popped in ascending order, because the run is followed forward */
    ASSERT(cvm_grey_list_pop(&pool.list, pool.packets) == &pool.packets[2]);
    ASSERT(cvm_grey_list_pop(&pool.list, pool.packets) == &pool.packets[3]);
    ASSERT(cvm_grey_list_pop(&pool.list, pool.packets) == &pool.packets[4]);
    ASSERT_EQ_FMT(1u, cvm_grey_list_size(&pool.list), "%u");
    ASSERT(cvm_grey_list_pop(&pool.list, pool.packets) == &pool.packets[5]);
    ASSERT(cvm_grey_list_pop(&pool.list, pool.packets) == NULL);
    PASS();
}

TEST a_run_can_be_pushed_on_top_of_another(void) {
    Pool pool;
    pool_init(&pool);

    cvm_grey_list_push_all(&pool.list, pool.packets, &pool.packets[0], 2);
    cvm_grey_list_push_all(&pool.list, pool.packets, &pool.packets[4], 2);

    ASSERT_EQ_FMT(4u, cvm_grey_list_size(&pool.list), "%u");

    /* the second run is at the head, so it comes out first */
    ASSERT(cvm_grey_list_pop(&pool.list, pool.packets) == &pool.packets[4]);
    ASSERT(cvm_grey_list_pop(&pool.list, pool.packets) == &pool.packets[5]);
    ASSERT(cvm_grey_list_pop(&pool.list, pool.packets) == &pool.packets[0]);
    ASSERT(cvm_grey_list_pop(&pool.list, pool.packets) == &pool.packets[1]);
    ASSERT(cvm_grey_list_pop(&pool.list, pool.packets) == NULL);
    PASS();
}

TEST interleaved_pushes_and_pops_keep_the_count_honest(void) {
    /*
     * The count must never exceed the number of packets actually on the
     * list. Checked after every single operation, which is the tightest a
     * single-threaded test can make it.
     *
     * A packet may only be pushed when it is not already on the list: a
     * packet holds one link, so pushing it twice would build a cycle and
     * the list would never end. An earlier version of this test pushed
     * from a pool round-robin without tracking that, and hung.
     */
    Pool pool;
    pool_init(&pool);

    bool on_the_list[PACKET_COUNT] = {false};
    u32 on_list = 0;
    u32 next_free = 0;

    for (u32 round = 0; round < 200; round++) {
        /* push whatever is free, up to two */
        for (u32 i = 0; i < 2; i++) {
            /* find a packet that is not on the list */
            u32 tries = 0;
            while (tries < PACKET_COUNT && on_the_list[next_free % PACKET_COUNT]) {
                next_free++;
                tries++;
            }
            if (tries == PACKET_COUNT) {
                break; /* everything is on the list */
            }
            u32 index = next_free % PACKET_COUNT;
            next_free++;

            cvm_grey_list_push(&pool.list, pool.packets, &pool.packets[index]);
            on_the_list[index] = true;
            on_list++;
            ASSERT_EQ_FMT(on_list, cvm_grey_list_size(&pool.list), "%u");
        }

        /* pop one, if there is one */
        CvmGreyPacket* got = cvm_grey_list_pop(&pool.list, pool.packets);
        if (got != NULL) {
            ASSERT(on_the_list[got - pool.packets]);
            on_the_list[got - pool.packets] = false;
            on_list--;
            ASSERT_EQ_FMT(on_list, cvm_grey_list_size(&pool.list), "%u");
        }
    }

    /* drain */
    CvmGreyPacket* got;
    while ((got = cvm_grey_list_pop(&pool.list, pool.packets)) != NULL) {
        ASSERT(on_the_list[got - pool.packets]);
        on_the_list[got - pool.packets] = false;
        on_list--;
        ASSERT_EQ_FMT(on_list, cvm_grey_list_size(&pool.list), "%u");
    }
    ASSERT_EQ_FMT(0u, on_list, "%u");
    ASSERT_EQ_FMT(0u, cvm_grey_list_size(&pool.list), "%u");
    PASS();
}

TEST the_packed_head_is_exactly_one_word(void) {
    ASSERT_EQ_FMT(8u, (u32)sizeof(CvmGreyPacketRef), "%u");
    ASSERT_EQ_FMT(8u, (u32)sizeof(((CvmGreyPacketRef*)0)->sep), "%u");
    /* the atomic view must cover the whole thing */
    ASSERT_EQ_FMT(8u, (u32)sizeof(((CvmGreyPacketRef*)0)->atom), "%u");
    PASS();
}

TEST the_empty_list_encoding_is_distinct_from_the_end_sentinel(void) {
    /* 0 means "no packets at all"; 1 means "the list ends here". A packet
     * whose link is 1 is the last one, which is not the same as an empty
     * list, and the pop path has to tell them apart. */
    Pool pool;
    pool_init(&pool);

    u64 empty = cvm_grey_list_empty();
    CvmGreyPacketRef r;
    r.atom = empty;
    ASSERT_EQ_FMT(CVM_GREYLIST_LAST, u24_to_u32(r.sep.idx), "%u");
    ASSERT_EQ_FMT(0u, u24_to_u32(r.sep.size), "%u");
    ASSERT(empty != 0);
    PASS();
}

/* ------------------------------------------------------------------ */
/* concurrency                                                         */
/* ------------------------------------------------------------------ */

/*
 * Producers push packets, consumers pop them, and nothing may be lost or
 * seen twice. Each producer owns a disjoint slice of the packet array, so
 * no packet is pushed twice — which is the invariant the ABA counter
 * assumes, the same way gc/block_list.c does.
 */
#define CONC_PACKETS 256
#define CONC_PRODUCERS 4
#define CONC_PER_PRODUCER (CONC_PACKETS / CONC_PRODUCERS)

typedef struct {
    CvmGreyPacket* packets;
    CvmGreyList* list;
    u32 first;
    u32 count;
} ProducerArg;

static void* producer_main(void* raw) {
    ProducerArg* a = raw;
    for (u32 i = 0; i < a->count; i++) {
        cvm_grey_list_push(a->list, a->packets, &a->packets[a->first + i]);
    }
    return NULL;
}

typedef struct {
    CvmGreyPacket* packets;
    CvmGreyList* list;
    _Atomic(u32)* remaining;
    _Atomic(u32)* seen;
} ConsumerArg;

static void* consumer_main(void* raw) {
    ConsumerArg* a = raw;
    for (;;) {
        CvmGreyPacket* p = cvm_grey_list_pop(a->list, a->packets);
        if (p == NULL) {
            if (atomic_load_explicit(a->remaining, memory_order_acquire) == 0) {
                return NULL;
            }
            continue;
        }
        u32 index = (u32)(p - a->packets);
        atomic_fetch_add_explicit(&a->seen[index], 1u, memory_order_relaxed);
        atomic_fetch_sub_explicit(a->remaining, 1u, memory_order_release);
    }
}

TEST concurrent_pushes_and_pops_lose_nothing(void) {
    CvmGreyPacket* packets = malloc(CONC_PACKETS * sizeof(CvmGreyPacket));
    for (u32 i = 0; i < CONC_PACKETS; i++) {
        cvm_grey_packet_init(&packets[i], CVM_GREY_PACKET_REFLIST);
    }

    CvmGreyList list;
    cvm_grey_list_init(&list);

    _Atomic(u32) remaining = CONC_PACKETS;
    _Atomic(u32)* seen = calloc(CONC_PACKETS, sizeof(_Atomic(u32)));

    ProducerArg producers[CONC_PRODUCERS];
    ConsumerArg consumers[CONC_PRODUCERS];
    pthread_t ptid[CONC_PRODUCERS];
    pthread_t ctid[CONC_PRODUCERS];

    for (u32 i = 0; i < CONC_PRODUCERS; i++) {
        producers[i] =
            (ProducerArg){packets, &list, i * CONC_PER_PRODUCER, CONC_PER_PRODUCER};
        pthread_create(&ptid[i], NULL, producer_main, &producers[i]);
    }
    for (u32 i = 0; i < CONC_PRODUCERS; i++) {
        consumers[i] = (ConsumerArg){packets, &list, &remaining, seen};
        pthread_create(&ctid[i], NULL, consumer_main, &consumers[i]);
    }

    for (u32 i = 0; i < CONC_PRODUCERS; i++)
        pthread_join(ptid[i], NULL);
    for (u32 i = 0; i < CONC_PRODUCERS; i++)
        pthread_join(ctid[i], NULL);

    ASSERT_EQ_FMT(0u, (u32)atomic_load(&remaining), "%u");
    ASSERT_EQ_FMT(0u, cvm_grey_list_size(&list), "%u");

    u32 total = 0;
    u32 duplicated = 0;
    u32 missed = 0;
    for (u32 i = 0; i < CONC_PACKETS; i++) {
        u32 n = atomic_load(&seen[i]);
        total += n;
        if (n > 1)
            duplicated++;
        if (n == 0)
            missed++;
    }

    ASSERT_EQ_FMT((u32)CONC_PACKETS, total, "%u");
    ASSERT_EQ_FMT(0u, duplicated, "%u");
    ASSERT_EQ_FMT(0u, missed, "%u");

    free(seen);
    free(packets);
    PASS();
}

SUITE(grey_packet_suite) {
    RUN_TEST(a_packet_starts_empty);
    RUN_TEST(packet_push_and_pop_are_lifo);
    RUN_TEST(a_packet_refuses_to_overflow);
    RUN_TEST(moving_items_takes_from_the_end_of_the_source);
    RUN_TEST(a_new_list_is_empty);
    RUN_TEST(pushing_one_packet_makes_the_count_one);
    RUN_TEST(pushing_several_packets_pops_them_in_reverse);
    RUN_TEST(push_all_links_a_run_without_touching_each_packet);
    RUN_TEST(a_run_can_be_pushed_on_top_of_another);
    RUN_TEST(interleaved_pushes_and_pops_keep_the_count_honest);
    RUN_TEST(the_packed_head_is_exactly_one_word);
    RUN_TEST(the_empty_list_encoding_is_distinct_from_the_end_sentinel);
    RUN_TEST(concurrent_pushes_and_pops_lose_nothing);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(grey_packet_suite);
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
