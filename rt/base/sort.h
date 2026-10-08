/*
 * rt/base/sort.h
 *
 * Sorting with a context pointer.
 *
 * There is no qsort here and there will not be one: it is a libc function, and
 * the runtime's freestanding translation units cannot call it. What it also
 * cannot do is take a context, which is the difference that matters when the
 * comparison needs more than the two elements — a sort by a key held elsewhere,
 * a sort that has to count comparisons, a sort of strings with a case-fold flag.
 *
 * Ported from refs/quickjs-0.17.0/cutils.h (Fabrice Bellard, MIT), which is the
 * same bounded-depth quicksort with a heapsort fallback: the recursion depth is
 * capped and the cap is what makes the worst case logarithmic rather than a
 * function of the input's shape.
 *
 * Not stable. A caller that needs stability sorts by a tiebreaker.
 */

#ifndef RT_BASE_SORT_H
#define RT_BASE_SORT_H

#include "base/types.h"

/*
 * Compare two elements. Returns negative, zero or positive.
 *
 * The arguments are the addresses of the elements themselves, not copies, so a
 * comparator reads through them and must not keep them: the array is being
 * rearranged while it runs.
 */
typedef i32 (*CvmSortCmp)(opaque a, opaque b, opaque_mut ctx);

/*
 * Sort `count` elements of `size` bytes each, in place.
 *
 * `size` may be anything: the implementation picks a swap routine from it, and a
 * large element size swaps blocks rather than bytes.
 */
void cvm_rqsort(opaque_mut base, usize count, usize size, CvmSortCmp cmp,
                opaque_mut ctx);

#endif /* RT_BASE_SORT_H */
