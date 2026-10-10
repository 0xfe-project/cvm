/*
 * bench/scenarios/tree_keep.c
 *
 * The main event. Build a binary tree, drop the previous one, keep the last.
 *
 * The only shape in the suite that exercises marking and sweeping at the
 * same time with a live set worth the name, which is why BENCH.md §6 calls
 * it unresolved rather than predicting a winner: it is the one measurement
 * that is about the collector rather than about one of its parts.
 *
 * A full tree of depth d holds 2^(d+1)-1 nodes, so d=17 is about 260k
 * objects — big enough that the mark phase is real work, small enough that
 * seven repetitions stay quick.
 */

#include "scenario.h"
#include "roots.h"

#define TREE_DEPTH 17u
#define ROUNDS 24u

typedef struct Node {
    struct Node *left;
    struct Node *right;
    unsigned tag;
} Node;

/*
 * Built with the frame's slot 0 pointing at the root for the whole build.
 * That is what generated code looks like: the node being worked on is in a
 * frame slot, so a collection in the middle of the build finds it.
 */
static void *build(unsigned depth) {
    BENCH_FRAME_DECL(f, 3);
    BENCH_FRAME_PUSH(f, 3);
    for (unsigned i = 0; i < 3; i++) {
        BENCH_ROOT(f, i) = NULL;
    }

    if (depth == 0) {
        BENCH_ROOT(f, 0) = bench_alloc_obj(&shape_small_node);
    } else {
        /*
         * Both children are held in frame slots, not C locals, for the whole
         * time the other is being built. A precise collector cannot see a C
         * local, so a child left in one would be reclaimed while the parent
         * was still working on its sibling — and the failure would look like
         * a corrupted tree rather than like a missing root. This is the
         * protocol BENCH.md §2.3 requires, and writing it wrong is the
         * easiest way to produce a number that means nothing.
         */
        BENCH_ROOT(f, 0) = build(depth - 1);
        BENCH_ROOT(f, 1) = build(depth - 1);

        void *node = bench_alloc_obj(&shape_small_node);
        BENCH_ROOT(f, 2) = node;
        if (node != NULL) {
            Node *n = node;
            BENCH_STORE(n, offsetof(Node, left), BENCH_ROOT(f, 0));
            BENCH_STORE(n, offsetof(Node, right), BENCH_ROOT(f, 1));
            n->tag = depth;
        }
        BENCH_ROOT(f, 0) = node;
    }

    void *root = BENCH_ROOT(f, 0);
    BENCH_FRAME_POP(f);
    return root;
}

static void tree(unsigned iteration) {
    (void)iteration;

    BENCH_FRAME_DECL(f, 1);
    BENCH_FRAME_PUSH(f, 1);
    BENCH_ROOT(f, 0) = NULL;

    for (unsigned round = 0; round < ROUNDS; round++) {
        /* The previous tree is dropped by overwriting the only slot that
         * reaches it. Everything below it becomes garbage at once, which is
         * the allocation pattern a real program has after a phase change. */
        BENCH_ROOT(f, 0) = build(TREE_DEPTH);
    }

    BENCH_FRAME_POP(f);
}

BenchScenario const bench_scenario = {
    .name = "tree-keep",
    .modes = BENCH_RUN_ALL_MODES,
    .heap_hint_bytes = 2ull * 1024ull * 1024ull * 1024ull,
    .auto_heap_bytes = 48ull * 1024ull * 1024ull,
    .default_cadence = 500000u,
    .run = tree,
};
