/*
 * tests/stress/test_stress_graph.c
 *
 * Object graphs: deep nesting, wide traversal, and mutation between
 * collections.
 *
 * The deep-nesting case is the one worth starting with, because it is where
 * this collector's design differs most from the reference it ports. The
 * reference walks a flat array of field offsets attached to a runtime type
 * record. This one walks a *recursive* descriptor: an embedded aggregate is
 * described by pointing at its own type's descriptor, and the marker
 * recurses. Nothing about that is hard, but it means depth is a code path,
 * and depth was only ever tested one level deep before this file.
 *
 * The graph cases are the read-heavy and write-heavy patterns from TEST.md:
 * a wide structure traversed repeatedly, and the same structure rewired
 * between collections so that what is reachable changes each time.
 */

#include "vendor/greatest.h"

#include <stddef.h>
#include <stdio.h>

#include "support/oracle.h"

#define TEST_MIN ((usize)8 * 1024 * 1024)
#define TEST_MAX ((usize)64 * 1024 * 1024)

/* ------------------------------------------------------------------ */
/* a chain of embedded aggregates                                      */
/* ------------------------------------------------------------------ */

/*
 * Eight levels, each a struct containing the next by value.
 *
 * The innermost holds a pointer; everything above it is embedding. The
 * marker walks this by recursing once per level, so eight levels means
 * eight frames of `trace_type`, and one level — which is all that was
 * tested before — would not have exercised the recursion at all.
 *
 * Written out rather than generated, because a descriptor is a static
 * initialiser and a chain of them can only be built by naming each one.
 */
typedef struct Nest7 {
    opaque_mut leaf;
} Nest7;

typedef struct Nest6 {
    Nest7 inner;
} Nest6;

typedef struct Nest5 {
    Nest6 inner;
} Nest5;

typedef struct Nest4 {
    Nest5 inner;
} Nest4;

typedef struct Nest3 {
    Nest4 inner;
} Nest3;

typedef struct Nest2 {
    Nest3 inner;
} Nest2;

typedef struct Nest1 {
    Nest2 inner;
} Nest1;

typedef struct Nest0 {
    Nest1 inner;
} Nest0;

static CvmPtrField const nest7_fields[] = {
    {.offset = offsetof(Nest7, leaf)},
};

static CvmTypeDescriptor const nest7_desc = {
    .size = sizeof(Nest7),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 1,
    .num_embed_fields = 0,
    .ptr_fields = nest7_fields,
    .embed_fields = NULL,
};

/*
 * The rest are the same shape, so the pattern is stated once: an embedded
 * field at offset zero, described by the type it embeds.
 *
 * The struct parameter is named `struct_type` rather than `type` because the
 * initialiser below uses `.type` as a field name, and a macro parameter
 * named `type` would be substituted into it — turning `.type = ...` into
 * `.Nest6 = ...`, and producing a compile error naming a field that does
 * not exist.
 */
#define EMBEDDING_DESC(name, struct_type, inner)                                       \
    static CvmEmbedField const name##_embed[] = {                                      \
        {.offset = 0, .type = &inner},                                                 \
    };                                                                                 \
    static CvmTypeDescriptor const name##_desc = {                                     \
        .size = sizeof(struct_type),                                                   \
        .kind = CVM_TYPE_STRUCT,                                                       \
        .num_ptr_fields = 0,                                                           \
        .num_embed_fields = 1,                                                         \
        .ptr_fields = NULL,                                                            \
        .embed_fields = name##_embed,                                                  \
    }

EMBEDDING_DESC(nest6, Nest6, nest7_desc);
EMBEDDING_DESC(nest5, Nest5, nest6_desc);
EMBEDDING_DESC(nest4, Nest4, nest5_desc);
EMBEDDING_DESC(nest3, Nest3, nest4_desc);
EMBEDDING_DESC(nest2, Nest2, nest3_desc);
EMBEDDING_DESC(nest1, Nest1, nest2_desc);
EMBEDDING_DESC(nest0, Nest0, nest1_desc);

/* A leaf to sit at the bottom of the chain. */
typedef struct Leaf {
    u64 canary;
} Leaf;

static CvmTypeDescriptor const leaf_desc = {
    .size = sizeof(Leaf),
    .kind = CVM_TYPE_SCALAR,
};

/* Reach the innermost pointer through the whole chain. */
static opaque_mut nest_leaf(Nest0* n) {
    return n->inner.inner.inner.inner.inner.inner.inner.leaf;
}

static void nest_set_leaf(Nest0* n, opaque_mut leaf) {
    n->inner.inner.inner.inner.inner.inner.inner.leaf = leaf;
}

/* ------------------------------------------------------------------ */
/* a binary tree, with a recursive builder                             */
/* ------------------------------------------------------------------ */

typedef struct Node {
    opaque_mut left;
    opaque_mut right;
    u64 canary;
} Node;

static CvmPtrField const node_fields[] = {
    {.offset = offsetof(Node, left)},
    {.offset = offsetof(Node, right)},
};

static CvmTypeDescriptor const node_desc = {
    .size = sizeof(Node),
    .kind = CVM_TYPE_STRUCT,
    .num_ptr_fields = 2,
    .num_embed_fields = 0,
    .ptr_fields = node_fields,
    .embed_fields = NULL,
};

/*
 * The node's canary lives in a named field, not at the payload's start: the
 * first field is a pointer, so the shorthand canary would land on it and the
 * check would read the pointer. See oracle.h for the full reasoning.
 */
static bool node_canary_ok(Node const* n) {
    return oracle_field_canary_ok(n, n->canary);
}

/*
 * Build a balanced tree of the given depth, recursively.
 *
 * Each level pushes its own frame and keeps the node it just allocated in a
 * slot across the two recursive calls that follow. That is exactly what
 * generated code does, and it means this exercises the frame chain at
 * depth as well as the descriptor recursion: a marker that lost the chain
 * part way up would find the leaves unreachable.
 */
static opaque_mut build_tree_recursive(u32 depth) {
    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = NULL;

    f_roots[0] = cvm_alloc(&node_desc);

    Node* node = f_roots[0];
    cvm_store_field(node, offsetof(Node, left), NULL);
    cvm_store_field(node, offsetof(Node, right), NULL);
    node->canary = oracle_field_canary(node);

    if (depth > 0) {
        /*
         * The children are built while this node sits in a root slot, which
         * is what keeps it alive across allocations that may collect.
         */
        cvm_store_field(node, offsetof(Node, left), build_tree_recursive(depth - 1));
        cvm_store_field(node, offsetof(Node, right), build_tree_recursive(depth - 1));
    }

    opaque_mut result = f_roots[0];
    cvm_frame_pop(&f_frame);
    return result;
}

/*
 * Walk the tree, checking every canary and counting the nodes.
 *
 * Counting as well as checking is what makes the check tight: a collector
 * that lost a subtree would leave the canaries of the nodes it lost
 * untouched, and a walk that only asked "are the reachable nodes intact"
 * would report success on a tree that had silently lost half of itself.
 * The count is compared against what the builder was asked to make.
 *
 * A frame per level, following the same protocol the builder does.
 */
static bool tree_check_recursive(opaque_mut root, u32* count) {
    if (root == NULL) {
        return true;
    }

    CVM_FRAME_DECL(f, 1);
    cvm_frame_push(&f_frame, f_roots, 1);
    f_roots[0] = root;

    Node* node = f_roots[0];
    if (!node_canary_ok(node)) {
        /* Do not recurse into a node whose contents are already wrong: its
         * child pointers may not be pointers, and following them would be
         * a wild read. */
        cvm_frame_pop(&f_frame);
        return false;
    }
    (*count)++;

    bool left_ok = tree_check_recursive(node->left, count);
    bool right_ok = tree_check_recursive(node->right, count);

    cvm_frame_pop(&f_frame);
    return left_ok && right_ok;
}

/* Check the tree and require exactly `expected` nodes. */
static bool tree_is_intact(opaque_mut root, u32 expected, char const* where) {
    u32 count = 0;
    if (!tree_check_recursive(root, &count)) {
        fprintf(stderr, "graph: a node lost its contents (%s)\n", where);
        return false;
    }
    if (count != expected) {
        fprintf(stderr, "graph: %u nodes, expected %u (%s)\n", count, expected, where);
        return false;
    }
    return true;
}

/* Nodes in a balanced tree of the given depth. */
static u32 tree_nodes(u32 depth) {
    return (depth == 0) ? 1u : (1u + 2u * tree_nodes(depth - 1));
}

/* ------------------------------------------------------------------ */
/* deep nesting                                                        */
/* ------------------------------------------------------------------ */

TEST an_eight_level_chain_survives_a_collection(void) {
    oracle_reset_frames();
    ORACLE_FRAME(1);

    Leaf* leaf = cvm_alloc(&leaf_desc);
    oracle_stamp(leaf);

    Nest0* nest = cvm_alloc(&nest0_desc);
    nest_set_leaf(nest, leaf);

    oracle_f_roots[0] = nest;

    oracle_collect();

    ASSERT(oracle_is_live(nest));
    /* The leaf is seven levels down. If the recursion stops early, this is
     * what notices. */
    ASSERT(oracle_is_live(nest_leaf(nest)));
    ASSERT(nest_leaf(nest) == leaf);
    ASSERT(oracle_canary_intact(leaf));

    ORACLE_FRAME_END();
    PASS();
}

TEST a_leaf_reached_only_through_the_chain_dies_without_it(void) {
    /*
     * The negative half: if nothing points at the chain, the leaf seven
     * levels down must go too. A marker that stopped recursing early would
     * keep the leaf alive by accident, and this is what says it did not.
     */
    oracle_reset_frames();

    Leaf* leaf = cvm_alloc(&leaf_desc);
    Nest0* nest = cvm_alloc(&nest0_desc);
    nest_set_leaf(nest, leaf);

    /* Neither is a root. */
    oracle_collect();

    ASSERT_FALSE(oracle_is_live(nest));
    ASSERT_FALSE(oracle_is_live(leaf));
    PASS();
}

/* ------------------------------------------------------------------ */
/* read-heavy                                                          */
/* ------------------------------------------------------------------ */

TEST a_wide_tree_survives_repeated_collections(void) {
    oracle_reset_frames();
    ORACLE_FRAME(1);

    const u32 depth = 7;
    u32 expected = tree_nodes(depth);

    void* root = build_tree_recursive(depth);
    ASSERT(root != NULL);
    oracle_f_roots[0] = root;

    /* Check once before collecting, so a tree the builder got wrong fails
     * here rather than being blamed on the collector. */
    if (!tree_is_intact(root, expected, "before any collection")) {
        FAIL();
    }

    for (u32 round = 0; round < 10; round++) {
        oracle_collect();

        char where[64];
        snprintf(where, sizeof(where), "after round %u", round);
        if (!tree_is_intact(root, expected, where)) {
            FAIL();
        }
    }

    ORACLE_FRAME_END();
    PASS();
}

/* ------------------------------------------------------------------ */
/* write-heavy                                                         */
/* ------------------------------------------------------------------ */

TEST rewiring_a_root_between_collections_kills_the_old_subtree(void) {
    /*
     * The mutation pattern: a reference handed from one subtree to the next,
     * with a collection each time. What the reference no longer reaches must
     * die; what it now reaches must live and be intact.
     *
     * Two slots, and a fresh subtree each round, because an object does not
     * come back from being collected. Two earlier versions of this test got
     * that wrong in different ways — one built a subtree into a C local that
     * was not a root and then expected it alive after a collection, the other
     * dropped a tree and then expected it alive again when it was made a root
     * once more. Both failed for reasons that had nothing to do with the
     * collector, which is worth recording so the next person writing this
     * shape does not repeat it.
     */
    oracle_reset_frames();
    ORACLE_FRAME(2);

    const u32 expected = tree_nodes(4);

    void* current = build_tree_recursive(4);
    oracle_f_roots[0] = current;

    for (u32 round = 0; round < 6; round++) {
        /*
         * Build the replacement while the incumbent is still a root, then
         * hand over: the new one becomes reachable in slot 1 before the old
         * one stops being reachable in slot 0. Doing it the other way round
         * would leave a window with neither, and a collection in that window
         * would take both.
         */
        void* next = build_tree_recursive(4);
        oracle_f_roots[1] = next;
        oracle_f_roots[0] = NULL;

        oracle_collect();

        if (!oracle_is_live(next)) {
            fprintf(stderr, "graph: the new subtree is dead at round %u\n", round);
            FAIL();
        }
        if (oracle_is_live(current)) {
            fprintf(stderr, "graph: the old subtree survived at round %u\n", round);
            FAIL();
        }
        if (!tree_is_intact(next, expected, "the new subtree")) {
            FAIL();
        }

        /* Make room for the next round's handover. */
        oracle_f_roots[0] = next;
        oracle_f_roots[1] = NULL;
        current = next;
    }

    ORACLE_FRAME_END();
    PASS();
}

TEST mutating_a_live_graph_in_place_is_safe(void) {
    /*
     * Rewrite pointers *within* a reachable structure, keeping the
     * structure reachable. The old subtrees become unreachable as the
     * rewiring proceeds, and each new subtree is built while the node that
     * will point at it is already reachable — so a collection can happen in
     * the middle of the rewiring.
     */
    oracle_reset_frames();
    ORACLE_FRAME(1);

    Node* root = cvm_alloc(&node_desc);
    cvm_store_field(root, offsetof(Node, left), NULL);
    cvm_store_field(root, offsetof(Node, right), NULL);
    root->canary = oracle_field_canary(root);
    oracle_f_roots[0] = root;

    for (u32 round = 0; round < 20; round++) {
        /* Build a new subtree and attach it, dropping what was there. */
        void* fresh = build_tree_recursive(3);

        if (round % 2 == 0) {
            cvm_store_field(root, offsetof(Node, left), fresh);
        } else {
            cvm_store_field(root, offsetof(Node, right), fresh);
        }

        oracle_collect();

        ASSERT(oracle_is_live(root));
        /* The root is still the root: its address must not have changed,
         * which is what says nothing moved. */
        ASSERT(oracle_f_roots[0] == root);
        ASSERT(node_canary_ok(root));

        /*
         * One side holds the subtree built this round and the other holds
         * the one from two rounds ago, so both are non-NULL from round 2
         * onwards. Before that one side is legitimately empty, and a count
         * of zero is correct for it.
         */
        if (root->left != NULL &&
            !tree_is_intact(root->left, tree_nodes(3), "left subtree")) {
            FAIL();
        }
        if (root->right != NULL &&
            !tree_is_intact(root->right, tree_nodes(3), "right subtree")) {
            FAIL();
        }
    }

    ORACLE_FRAME_END();
    PASS();
}

/* ------------------------------------------------------------------ */
/* cycles                                                              */
/* ------------------------------------------------------------------ */

TEST a_cycle_is_marked_and_a_detached_cycle_is_not(void) {
    oracle_reset_frames();
    ORACLE_FRAME(2);

    /* A three-node ring, reachable from a root. */
    Node* a = cvm_alloc(&node_desc);
    Node* b = cvm_alloc(&node_desc);
    Node* c = cvm_alloc(&node_desc);
    cvm_store_field(a, offsetof(Node, left), b);
    cvm_store_field(b, offsetof(Node, left), c);
    cvm_store_field(c, offsetof(Node, left), a);
    cvm_store_field(a, offsetof(Node, right), b->right = c->right = NULL);
    a->canary = oracle_field_canary(a);
    b->canary = oracle_field_canary(b);
    c->canary = oracle_field_canary(c);
    oracle_f_roots[0] = a;

    /* A second ring, reachable from nothing. */
    Node* x = cvm_alloc(&node_desc);
    Node* y = cvm_alloc(&node_desc);
    cvm_store_field(x, offsetof(Node, left), y);
    cvm_store_field(y, offsetof(Node, left), x);
    cvm_store_field(x, offsetof(Node, right), y->right = NULL);

    oracle_collect();

    ASSERT(oracle_is_live(a));
    ASSERT(oracle_is_live(b));
    ASSERT(oracle_is_live(c));
    /* A cycle with no external reference is garbage, and a collector that
     * counts references rather than tracing would leak it. */
    ASSERT_FALSE(oracle_is_live(x));
    ASSERT_FALSE(oracle_is_live(y));

    /* Break the reachable ring: the whole thing must now die. */
    oracle_f_roots[0] = NULL;
    oracle_collect();

    ASSERT_FALSE(oracle_is_live(a));
    ASSERT_FALSE(oracle_is_live(b));
    ASSERT_FALSE(oracle_is_live(c));

    ORACLE_FRAME_END();
    PASS();
}

SUITE(graph_suite) {
    RUN_TEST(an_eight_level_chain_survives_a_collection);
    RUN_TEST(a_leaf_reached_only_through_the_chain_dies_without_it);
    RUN_TEST(a_wide_tree_survives_repeated_collections);
    RUN_TEST(rewiring_a_root_between_collections_kills_the_old_subtree);
    RUN_TEST(mutating_a_live_graph_in_place_is_safe);
    RUN_TEST(a_cycle_is_marked_and_a_detached_cycle_is_not);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    if (!oracle_bring_up(TEST_MIN, TEST_MAX, 2)) {
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(graph_suite);
    GREATEST_MAIN_END();
}
