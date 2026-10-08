/*
 * tests/unit/test_list.c
 *
 * The intrusive list. What is worth pinning here is not that add and del
 * do something, but the states: an empty head points at itself, a
 * detached element is NULLed, and the two questions ("is this head
 * empty", "is this element linked") answer differently for the same
 * memory. The registry in vm/context.c relies on exactly that.
 */

#include "vendor/greatest.h"

#include "base/list.h"
#include "base/types.h"

typedef struct Item {
    u32 id;
    CvmList link;
} Item;

/* Links are the only thing the list sees, so every test goes through the
 * entry macro to get back to the payload. */
static Item* item_of(CvmList* link) {
    return cvm_list_entry(link, Item, link);
}

static void item_init(Item* it, u32 id) {
    it->id = id;
    cvm_list_unlink(&it->link);
}

/* ------------------------------------------------------------------ */
/* the empty head                                                      */
/* ------------------------------------------------------------------ */

TEST a_new_head_is_empty(void) {
    CvmList head = CVM_LIST_HEAD_INIT(head);
    ASSERT(cvm_list_empty(&head));
    ASSERT(cvm_list_first(&head) == NULL);
    ASSERT(cvm_list_last(&head) == NULL);
    PASS();
}

TEST init_makes_an_existing_head_empty_again(void) {
    CvmList head;
    Item a;
    item_init(&a, 1);

    /* Garbage first, so the test would fail if init did not overwrite. */
    head.prev = NULL;
    head.next = NULL;

    cvm_list_init(&head);
    ASSERT(cvm_list_empty(&head));

    cvm_list_add(&a.link, &head);
    ASSERT_FALSE(cvm_list_empty(&head));

    cvm_list_init(&head);
    ASSERT(cvm_list_empty(&head));
    PASS();
}

TEST an_empty_head_answers_linked_which_is_why_they_are_two_questions(void) {
    /* Not a wart: circularity makes a head indistinguishable from an
     * element by its links alone. Pin the contract so nobody "fixes" it
     * into something that breaks detachment. */
    CvmList head = CVM_LIST_HEAD_INIT(head);
    ASSERT(cvm_list_is_linked(&head));
    ASSERT(cvm_list_empty(&head));
    PASS();
}

/* ------------------------------------------------------------------ */
/* add and order                                                       */
/* ------------------------------------------------------------------ */

TEST add_puts_elements_at_the_front(void) {
    CvmList head = CVM_LIST_HEAD_INIT(head);
    Item a, b, c;
    item_init(&a, 1);
    item_init(&b, 2);
    item_init(&c, 3);

    cvm_list_add(&a.link, &head);
    cvm_list_add(&b.link, &head);
    cvm_list_add(&c.link, &head);

    /* last added is first out */
    ASSERT_EQ_FMT(3u, item_of(cvm_list_first(&head))->id, "%u");
    ASSERT_EQ_FMT(1u, item_of(cvm_list_last(&head))->id, "%u");
    PASS();
}

TEST add_tail_puts_elements_at_the_back(void) {
    CvmList head = CVM_LIST_HEAD_INIT(head);
    Item a, b, c;
    item_init(&a, 1);
    item_init(&b, 2);
    item_init(&c, 3);

    cvm_list_add_tail(&a.link, &head);
    cvm_list_add_tail(&b.link, &head);
    cvm_list_add_tail(&c.link, &head);

    ASSERT_EQ_FMT(1u, item_of(cvm_list_first(&head))->id, "%u");
    ASSERT_EQ_FMT(3u, item_of(cvm_list_last(&head))->id, "%u");
    PASS();
}

TEST a_single_element_is_both_ends(void) {
    CvmList head = CVM_LIST_HEAD_INIT(head);
    Item a;
    item_init(&a, 7);
    cvm_list_add_tail(&a.link, &head);

    ASSERT_EQ_FMT(7u, item_of(cvm_list_first(&head))->id, "%u");
    ASSERT_EQ_FMT(7u, item_of(cvm_list_last(&head))->id, "%u");
    ASSERT_FALSE(cvm_list_empty(&head));
    PASS();
}

/* ------------------------------------------------------------------ */
/* traversal                                                           */
/* ------------------------------------------------------------------ */

TEST for_each_entry_visits_in_order(void) {
    CvmList head = CVM_LIST_HEAD_INIT(head);
    Item items[4];
    u32 expected[4] = {10u, 20u, 30u, 40u};
    u32 seen[4] = {0u, 0u, 0u, 0u};
    usize n = 0;

    for (usize i = 0; i < 4; i++) {
        item_init(&items[i], 10u * (u32)(i + 1u));
        cvm_list_add_tail(&items[i].link, &head);
    }

    cvm_list_for_each_entry(it, Item, &head, link) {
        ASSERT(n < 4);
        seen[n++] = it->id;
    }

    ASSERT_EQ_FMT((u32)4u, (u32)n, "%u");
    for (usize i = 0; i < 4; i++) {
        ASSERT_EQ_FMT(expected[i], seen[i], "%u");
    }
    PASS();
}

TEST for_each_prev_walks_backwards(void) {
    CvmList head = CVM_LIST_HEAD_INIT(head);
    Item items[3];
    u32 expected[3] = {30u, 20u, 10u};
    usize n = 0;

    for (usize i = 0; i < 3; i++) {
        item_init(&items[i], 10u * (u32)(i + 1u));
        cvm_list_add_tail(&items[i].link, &head);
    }

    CvmList* link = NULL;
    cvm_list_for_each_prev(link, &head) {
        ASSERT(n < 3);
        ASSERT_EQ_FMT(expected[n], item_of(link)->id, "%u");
        n++;
    }
    ASSERT_EQ_FMT((u32)3u, (u32)n, "%u");
    PASS();
}

TEST for_each_safe_survives_deleting_every_element(void) {
    /* This is the shape the registry uses: walk it and unlink as you go,
     * which an unsafe walk cannot do because it reads the successor of a
     * link that has just been NULLed. */
    CvmList head = CVM_LIST_HEAD_INIT(head);
    Item items[4];

    for (usize i = 0; i < 4; i++) {
        item_init(&items[i], (u32)(i + 1u));
        cvm_list_add_tail(&items[i].link, &head);
    }

    CvmList* link = NULL;
    CvmList* next = NULL;
    cvm_list_for_each_safe(link, next, &head) {
        cvm_list_del(link);
    }

    ASSERT(cvm_list_empty(&head));
    for (usize i = 0; i < 4; i++) {
        ASSERT_FALSE(cvm_list_is_linked(&items[i].link));
    }
    PASS();
}

/* ------------------------------------------------------------------ */
/* removal                                                             */
/* ------------------------------------------------------------------ */

TEST del_removes_from_the_middle(void) {
    CvmList head = CVM_LIST_HEAD_INIT(head);
    Item a, b, c;
    item_init(&a, 1);
    item_init(&b, 2);
    item_init(&c, 3);
    cvm_list_add_tail(&a.link, &head);
    cvm_list_add_tail(&b.link, &head);
    cvm_list_add_tail(&c.link, &head);

    cvm_list_del(&b.link);

    ASSERT_EQ_FMT(1u, item_of(cvm_list_first(&head))->id, "%u");
    ASSERT_EQ_FMT(3u, item_of(cvm_list_last(&head))->id, "%u");
    /* the survivors still point at each other, not at b */
    ASSERT(cvm_list_first(&head)->next == cvm_list_last(&head));
    ASSERT(cvm_list_last(&head)->prev == cvm_list_first(&head));
    PASS();
}

TEST del_from_head_and_tail(void) {
    CvmList head = CVM_LIST_HEAD_INIT(head);
    Item a, b, c;
    item_init(&a, 1);
    item_init(&b, 2);
    item_init(&c, 3);
    cvm_list_add_tail(&a.link, &head);
    cvm_list_add_tail(&b.link, &head);
    cvm_list_add_tail(&c.link, &head);

    cvm_list_del(&a.link);
    ASSERT_EQ_FMT(2u, item_of(cvm_list_first(&head))->id, "%u");

    cvm_list_del(&c.link);
    ASSERT_EQ_FMT(2u, item_of(cvm_list_last(&head))->id, "%u");

    cvm_list_del(&b.link);
    ASSERT(cvm_list_empty(&head));
    PASS();
}

TEST a_detached_element_is_not_linked(void) {
    CvmList head = CVM_LIST_HEAD_INIT(head);
    Item a;
    item_init(&a, 1);

    /* never added */
    ASSERT_FALSE(cvm_list_is_linked(&a.link));

    cvm_list_add_tail(&a.link, &head);
    ASSERT(cvm_list_is_linked(&a.link));

    cvm_list_del(&a.link);
    ASSERT_FALSE(cvm_list_is_linked(&a.link));
    /* and del NULLs both ends, so a stray read cannot follow into a list
     * the element is no longer part of */
    ASSERT(a.link.prev == NULL);
    ASSERT(a.link.next == NULL);
    PASS();
}

TEST is_linked_tells_the_truth_after_a_remove_that_already_happened(void) {
    /* The thread registry's detach path: another thread may have removed
     * the node already, and the answer has to be "not linked" rather than
     * a second unlink that corrupts the neighbours. */
    CvmList head = CVM_LIST_HEAD_INIT(head);
    Item a, b;
    item_init(&a, 1);
    item_init(&b, 2);
    cvm_list_add_tail(&a.link, &head);
    cvm_list_add_tail(&b.link, &head);

    cvm_list_del(&a.link);
    ASSERT_FALSE(cvm_list_is_linked(&a.link));

    /* The guard the registry uses, so a repeated detach is a no-op. */
    if (cvm_list_is_linked(&a.link)) {
        cvm_list_del(&a.link);
    }

    /* b is untouched and still alone on the list */
    ASSERT_EQ_FMT(2u, item_of(cvm_list_first(&head))->id, "%u");
    ASSERT(cvm_list_first(&head) == cvm_list_last(&head));
    PASS();
}

TEST an_element_can_be_moved_between_lists(void) {
    CvmList one = CVM_LIST_HEAD_INIT(one);
    CvmList two = CVM_LIST_HEAD_INIT(two);
    Item a;
    item_init(&a, 1);

    cvm_list_add_tail(&a.link, &one);
    cvm_list_del(&a.link);
    cvm_list_add_tail(&a.link, &two);

    ASSERT(cvm_list_empty(&one));
    ASSERT_FALSE(cvm_list_empty(&two));
    ASSERT_EQ_FMT(1u, item_of(cvm_list_first(&two))->id, "%u");
    PASS();
}

SUITE(list_suite) {
    RUN_TEST(a_new_head_is_empty);
    RUN_TEST(init_makes_an_existing_head_empty_again);
    RUN_TEST(an_empty_head_answers_linked_which_is_why_they_are_two_questions);

    RUN_TEST(add_puts_elements_at_the_front);
    RUN_TEST(add_tail_puts_elements_at_the_back);
    RUN_TEST(a_single_element_is_both_ends);

    RUN_TEST(for_each_entry_visits_in_order);
    RUN_TEST(for_each_prev_walks_backwards);
    RUN_TEST(for_each_safe_survives_deleting_every_element);

    RUN_TEST(del_removes_from_the_middle);
    RUN_TEST(del_from_head_and_tail);
    RUN_TEST(a_detached_element_is_not_linked);
    RUN_TEST(is_linked_tells_the_truth_after_a_remove_that_already_happened);
    RUN_TEST(an_element_can_be_moved_between_lists);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(list_suite);
    GREATEST_MAIN_END();
}
