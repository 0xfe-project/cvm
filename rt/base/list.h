/*
 * rt/base/list.h
 *
 * An intrusive circular doubly-linked list, in the Linux-kernel shape —
 * the same one quickjs carries as list.h, reworded and given the
 * discipline this codebase uses everywhere else.
 *
 * ---------------------------------------------------------------------
 * Why intrusive
 * ---------------------------------------------------------------------
 *
 * The link lives inside the linked structure rather than in a node that
 * wraps a pointer:
 *
 *     typedef struct CvmThreadNode {
 *         CvmList link;    // the link is a member, not a container
 *         ...
 *     } CvmThreadNode;
 *
 * Three consequences, all of them the point:
 *
 *   - No allocation to link: a node that is linked and a node that is not
 *     are the same node, so an insert cannot fail and there is no error
 *     path to write.
 *   - No type erasure: the link is a real member of a real type, and
 *     cvm_list_entry recovers the enclosing structure without a cast
 *     through void* at the call site.
 *   - One link can be in one list at a time, which is a constraint rather
 *     than a limitation: a node that must be in two lists carries two
 *     links, and saying so is clearer than sharing a node between them.
 *
 * ---------------------------------------------------------------------
 * Two states an element can be in, and one an empty head is
 * ---------------------------------------------------------------------
 *
 *   a list head                     prev and next point at the head itself
 *   an element in a list            prev and next point at its neighbours
 *   an element in no list           prev and next are NULL
 *
 * The third state is why this list is safe to use without a separate
 * "linked" flag: cvm_list_del leaves an element detached, so asking
 * cvm_list_is_linked tells the truth about an element whose history the
 * caller may not have tracked. That question comes up for real — a thread
 * detaching from a registry another thread may have already removed it
 * from — and answering it with NULLs costs nothing.
 *
 * Circularity is what makes the head indistinguishable from an element,
 * so add, delete and iterate are one code path each instead of the two
 * (first element, then the rest) a NULL-terminated list needs.
 *
 * ---------------------------------------------------------------------
 * Why the type is a macro parameter
 * ---------------------------------------------------------------------
 *
 * cvm_list_entry needs the enclosing type, and the kernel idiom is to
 * recover it with __typeof__ from the loop variable. That is a GNU
 * extension; passing the type in explicitly costs four characters at the
 * call site and keeps this header standard C, which is the house rule.
 */

#ifndef RT_BASE_LIST_H
#define RT_BASE_LIST_H

#include "base/types.h"

typedef struct CvmList {
    struct CvmList* prev;
    struct CvmList* next;
} CvmList;

/*
 * A head that is empty, usable as a static initialiser:
 *
 *     static CvmList registry = CVM_LIST_HEAD_INIT(registry);
 *
 * `head` must be an lvalue so its address is a constant expression. The
 * self-reference is legal at file scope because the symbol's address is
 * known then; this is not something that can be done to a local.
 */
#define CVM_LIST_HEAD_INIT(head) {&(head), &(head)}

/* Make an existing head empty. */
CVM_INLINE void cvm_list_init(CvmList* head) {
    head->prev = head;
    head->next = head;
}

/*
 * The enclosing structure from a link. The standard container_of, with
 * the pointer arithmetic done in bytes and no assumption about where in
 * the structure the link sits.
 */
#define cvm_list_entry(el, type, member)                                               \
    ((type*)(void*)((u8*)(el) - offsetof(type, member)))

/* Put an element into the detached state: cvm_list_del's result, and the
 * state an element must be in before its first cvm_list_add. */
CVM_INLINE void cvm_list_unlink(CvmList* el) {
    el->prev = NULL;
    el->next = NULL;
}

/* Whether the element is in some list. An empty head answers true, which
 * is why this is asked about elements and cvm_list_empty about heads. */
CVM_INLINE bool cvm_list_is_linked(CvmList const* el) {
    return el->prev != NULL && el->next != NULL;
}

/* Whether this head has no elements. A detached element answers true
 * too, so the two questions are not interchangeable: see above. */
CVM_INLINE bool cvm_list_empty(CvmList const* head) {
    return head->next == head;
}

/* Insert `el` immediately after `head`, which puts it at the front. */
CVM_INLINE void cvm_list_add(CvmList* el, CvmList* head) {
    CvmList* next = head->next;
    next->prev = el;
    el->next = next;
    el->prev = head;
    head->next = el;
}

/* Insert `el` immediately before `head`, which puts it at the back. */
CVM_INLINE void cvm_list_add_tail(CvmList* el, CvmList* head) {
    CvmList* prev = head->prev;
    prev->next = el;
    el->prev = prev;
    el->next = head;
    head->prev = el;
}

/*
 * Unlink `el`, leaving it detached. This is the operation a singly-linked
 * list cannot do in constant time — the predecessor has to be found by
 * walking — and the reason a registry whose nodes are removed while it is
 * in use wants a doubly-linked one.
 *
 * O(1). `el` must be linked; use cvm_list_is_linked when that is not
 * known.
 */
CVM_INLINE void cvm_list_del(CvmList* el) {
    CvmList* prev = el->prev;
    CvmList* next = el->next;
    prev->next = next;
    next->prev = prev;
    cvm_list_unlink(el);
}

/* The first element's link, or NULL when the list is empty. */
CVM_INLINE CvmList* cvm_list_first(CvmList* head) {
    return cvm_list_empty(head) ? NULL : head->next;
}

/* The last element's link, or NULL when the list is empty. */
CVM_INLINE CvmList* cvm_list_last(CvmList* head) {
    return cvm_list_empty(head) ? NULL : head->prev;
}

/* Walk a list of links. `pos` is declared by the caller (it may be a
 * CvmList* or anything else both ends agree on); `tmp` likewise. */
#define cvm_list_for_each(pos, head)                                                   \
    for ((pos) = (head)->next; (pos) != (head); (pos) = (pos)->next)

#define cvm_list_for_each_prev(pos, head)                                              \
    for ((pos) = (head)->prev; (pos) != (head); (pos) = (pos)->prev)

/* The variants that survive deleting `pos` during the walk, by reading
 * the successor before the body runs. */
#define cvm_list_for_each_safe(pos, tmp, head)                                         \
    for ((pos) = (head)->next, (tmp) = (pos)->next; (pos) != (head);                   \
         (pos) = (tmp), (tmp) = (pos)->next)

#define cvm_list_for_each_prev_safe(pos, tmp, head)                                    \
    for ((pos) = (head)->prev, (tmp) = (pos)->prev; (pos) != (head);                   \
         (pos) = (tmp), (tmp) = (pos)->prev)

/*
 * Walk the *enclosing structures*, declaring `pos` itself. The type is a
 * parameter rather than recovered with __typeof__: see the note at the
 * top of the header.
 *
 *     cvm_list_for_each_entry(node, CvmThreadNode, &registry, link) {
 *         ... node->id ...
 *     }
 */
#define cvm_list_for_each_entry(pos, type, head, member)                               \
    for (type* pos = cvm_list_entry((head)->next, type, member);                       \
         &(pos)->member != (head);                                                     \
         pos = cvm_list_entry((pos)->member.next, type, member))

#define cvm_list_for_each_entry_safe(pos, tmp, type, head, member)                     \
    for (type* pos = cvm_list_entry((head)->next, type, member),                       \
               *tmp = cvm_list_entry((pos)->member.next, type, member);                \
         &(pos)->member != (head);                                                     \
         pos = (tmp), tmp = cvm_list_entry((tmp)->member.next, type, member))

#endif /* RT_BASE_LIST_H */
