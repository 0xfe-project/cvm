/*
 * bench/scenarios/read_heavy.c
 *
 * Build a large graph once, then traverse it many times with almost no
 * writes.
 *
 * Separates mark throughput and cache behaviour from mutation cost. The
 * graph is built once, so allocation is a small part of the run, and the
 * traversal does about one write per hundred reads, so a write barrier has
 * almost nothing to tax.
 *
 * That makes this the scenario where a stop-the-world marker over a static
 * graph should look best against a concurrent one: the concurrent collector
 * pays its barrier everywhere and gets nothing back here.
 */

#include "scenario.h"
#include "roots.h"

#define NODES 200000u
#define TRAVERSALS 12u

typedef struct Node {
    struct Node *next;
    struct Node *other;
    unsigned value;
} Node;

static void read_heavy(unsigned iteration) {
    (void)iteration;

    BENCH_FRAME_DECL(f, 3);
    BENCH_FRAME_PUSH(f, 3);
    for (unsigned i = 0; i < 3; i++) {
        BENCH_ROOT(f, i) = NULL;
    }

    /* Build a chain, each node also pointing at a node further along so the
     * traversal is not purely sequential. */
    for (unsigned i = 0; i < NODES; i++) {
        void *p = bench_alloc_obj(&shape_small_node);
        if (p == NULL) {
            break;
        }
        Node *n = p;
        n->next = BENCH_ROOT(f, 0);
        n->other = BENCH_ROOT(f, 1);
        n->value = i;
        BENCH_ROOT(f, 1) = BENCH_ROOT(f, 0);
        BENCH_ROOT(f, 0) = p;
    }

    /*
     * Traverse. Summing forces the loads to happen; the result is consumed
     * through a volatile so the loop cannot be removed.
     *
     * A collection is asked for at the start of each traversal. Without one,
     * the graph is small enough that cvm's default policy never collects and
     * the scenario measures a traversal with no marking in it at all — which
     * would be a measurement of cache behaviour wearing the label of mark
     * throughput. Asking for the collection is what the mode is for.
     */
    unsigned long long sum = 0;
    for (unsigned t = 0; t < TRAVERSALS; t++) {
        bench_collector.collect();
        Node *n = BENCH_ROOT(f, 0);
        unsigned steps = 0;
        while (n != NULL && steps < NODES) {
            sum += n->value;
            n = n->next;
            steps++;
            /* One write per hundred reads: enough that a barrier is not
             * free, few enough that it is not the subject. */
            if ((steps % 100) == 0 && n != NULL) {
                n->value = (unsigned)steps;
            }
        }
    }

    volatile unsigned long long sink = sum;
    (void)sink;

    BENCH_FRAME_POP(f);
}

BenchScenario const bench_scenario = {
    .name = "read-heavy",
    .modes = BENCH_RUN_AUTO,
    .heap_hint_bytes = 512ull * 1024ull * 1024ull,
    .auto_heap_bytes = 16ull * 1024ull * 1024ull,
    .default_cadence = 100000u,
    .run = read_heavy,
};
