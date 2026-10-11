/*
 * bench/scenarios/write_heavy.c
 *
 * Repeatedly mutate a dense graph. Few traversals, many stores.
 *
 * The other half of the read/write pair, and the one where the trade runs
 * the other way. cvm's pointer stores are plain stores because marking is
 * stop-the-world; Go's go through a write barrier because marking is
 * concurrent. So a cvm win here is Go paying for short pauses, not Go being
 * slow — BENCH.md §4 says this out loud so that neither number gets quoted
 * without the other.
 *
 * The objects are wide_node: eight pointers in ninety-six bytes, so almost
 * every store is a pointer store and a barrier has almost nothing to miss.
 */

#include "scenario.h"
#include "roots.h"

#define NODES 20000u
#define ROUNDS 400u

typedef struct Node {
    void *slots[8];
    unsigned tag;
} Node;

static void write_heavy(unsigned iteration) {
    (void)iteration;

    BENCH_FRAME_DECL(f, 4);
    BENCH_FRAME_PUSH(f, 4);
    for (unsigned i = 0; i < 4; i++) {
        BENCH_ROOT(f, i) = NULL;
    }

    /*
     * A ring of nodes, held by slot 0 as the head and slot 1 as the
     * previous one while it is being linked.
     */
    for (unsigned i = 0; i < NODES; i++) {
        void *p = bench_alloc_obj(&shape_wide_node);
        if (p == NULL) {
            break;
        }
        Node *n = p;
        for (unsigned s = 0; s < 8; s++) {
            BENCH_STORE(n, offsetof(Node, slots) + s * sizeof(void *), NULL);
        }
        n->tag = i;
        if (BENCH_ROOT(f, 0) == NULL) {
            BENCH_ROOT(f, 0) = p; /* head */
        } else {
            Node *prev = BENCH_ROOT(f, 1);
            BENCH_STORE(prev, offsetof(Node, slots), p);
        }
        BENCH_ROOT(f, 1) = p;
    }

    /* Close the ring so the traversal never ends early. */
    if (BENCH_ROOT(f, 1) != NULL) {
        Node *last = BENCH_ROOT(f, 1);
        BENCH_STORE(last, offsetof(Node, slots), BENCH_ROOT(f, 0));
    }

    /*
     * The mutation loop: walk the ring and write pointers into every slot.
     * This is the pointer-store tax under test, so the writes are the work —
     * they must not be optimized away, hence the volatile sink below.
     */
    Node *n = BENCH_ROOT(f, 0);
    for (unsigned round = 0; round < ROUNDS && n != NULL; round++) {
        /*
         * Read the next node before overwriting this one's slots. The first
         * version wrote slots[0] = n and then followed slots[0], which turns
         * the ring into a self-loop after one step and measures a single
         * node four hundred times — a bug that produces a fast, believable,
         * meaningless number, which is the worst kind.
         */
        Node *next = n->slots[0];
        for (unsigned s = 0; s < 8; s++) {
            BENCH_STORE(n, offsetof(Node, slots) + s * sizeof(void *), next);
        }
        n->tag = round;
        n = next;
    }

    volatile unsigned sink = (n != NULL) ? n->tag : 0;
    (void)sink;

    BENCH_FRAME_POP(f);
}

BenchScenario const bench_scenario = {
    .name = "write-heavy",
    .modes = BENCH_RUN_AUTO,
    .heap_hint_bytes = 256ull * 1024ull * 1024ull,
    .auto_heap_bytes = 16ull * 1024ull * 1024ull,
    .default_cadence = 50000u,
    .run = write_heavy,
};
