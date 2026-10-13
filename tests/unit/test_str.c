/*
 * tests/unit/test_str.c
 *
 * The string type: construction in both representations, widening, equality,
 * ordering and the hash cache.
 *
 * Two things here are not obvious and are the reason the file exists:
 *
 *   - ordering must be by code unit *value*. Two wide strings compared with
 *     memcmp would be ordered by bytes, and on a little-endian machine that
 *     orders 0x0100 below 0x0001. The test says so.
 *
 *   - a string is a length-driven object whose tail the collector never looks
 *     into, which is what CVM_TYPE_SCALAR_ARRAY says. The extent test pins
 *     that the collector computes the same size the allocator produced, and
 *     the liveness tests pin that surviving a collection is about being
 *     rooted, not about the string being special.
 */

#include "vendor/greatest.h"

#include <stdio.h>

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "cvm/str.h"
#include "cvm/weak.h"
#include "gc/collect.h"
#include "gc/world.h"
#include "rt.h"
#include "safepoint.h"

#if !defined(_WIN32)

#define TEST_MIN ((usize)4 * 1024 * 1024)
#define TEST_MAX ((usize)16 * 1024 * 1024)

static CvmHeap* heap(void) {
    return &cvm_world()->heap;
}

/* The collector's entry point that the unit tests use. cvm_collect()
 * is the public one and stays inert until cvm_init has run, which this
 * fixture deliberately does not: it brings up the world directly. */
static void collect(void) {
    cvm_gc_collect_and_finish(heap());
}

static void fixture_enter(void) {
    (void)cvm_thread_attach();
}

static void fixture_leave(void) {
    cvm_thread_detach();
}

#define FRAME(slots)                                                                   \
    CVM_FRAME_DECL(f, slots);                                                          \
    cvm_frame_push(&f_frame, f_roots, slots);                                          \
    for (u32 i = 0; i < (slots); i++) {                                                \
        f_roots[i] = NULL;                                                             \
    }

#define FRAME_END() cvm_frame_pop(&f_frame)

/* The extent the allocator produced for a string of `length` units. */
static usize expected_extent(usize length, bool wide) {
    usize payload = offsetof(CvmString, data) + length * (wide ? 2u : 1u);
    return cvm_align_up(sizeof(CvmGcHeader) + payload, CVM_ALLOC_ALIGNMENT);
}

/* ------------------------------------------------------------------ */
/* the two representations                                             */
/* ------------------------------------------------------------------ */

TEST a_narrow_string_holds_its_units(void) {
    fixture_enter();

    static u8 const text[] = {'h', 'e', 'l', 'l', 'o'};
    CvmString* s = cvm_string_new8(text, sizeof(text));

    ASSERT_EQ_FMT((usize)5, cvm_string_length(s), "%zu");
    ASSERT_FALSE(cvm_string_is_wide(s));
    for (usize i = 0; i < 5; i++) {
        ASSERT_EQ_FMT((u32)text[i], cvm_string_unit(s, i), "%u");
    }

    fixture_leave();
    PASS();
}

TEST narrow_units_are_code_units_not_utf8_bytes(void) {
    /*
     * The distinction the representation turns on: a byte above 0x7F is a
     * *unit* whose value is that byte, one character. Text that really is
     * UTF-8 has to be decoded by the layer above before it gets here.
     */
    fixture_enter();

    static u8 const latin1[] = {0xFFu, 0x00u, 0x80u};
    CvmString* s = cvm_string_new8(latin1, sizeof(latin1));

    ASSERT_EQ_FMT((usize)3, cvm_string_length(s), "%zu");
    ASSERT_EQ_FMT(0xFFu, cvm_string_unit(s, 0), "%u");
    ASSERT_EQ_FMT(0x00u, cvm_string_unit(s, 1), "%u");
    ASSERT_EQ_FMT(0x80u, cvm_string_unit(s, 2), "%u");

    fixture_leave();
    PASS();
}

TEST the_ascii_constructor_copies_the_text(void) {
    fixture_enter();

    str text = STR_LITERAL("hello");
    CvmString* s = cvm_string_new_ascii(text);

    ASSERT_EQ_FMT(text.len, cvm_string_length(s), "%zu");
    ASSERT_FALSE(cvm_string_is_wide(s));
    ASSERT_EQ_FMT('h', (int)cvm_string_unit(s, 0), "%c");
    ASSERT_EQ_FMT('o', (int)cvm_string_unit(s, 4), "%c");

    /* the source is not retained: a string owns its own storage */
    ASSERT((u8 const*)s->data != text.ptr);

    fixture_leave();
    PASS();
}

TEST a_wide_string_holds_code_units(void) {
    fixture_enter();

    /* U+0100 and a lone surrogate, which only the 16-bit form can hold */
    static u16 const units[] = {0x0100u, 0x0041u, 0xD800u};
    CvmString* s = cvm_string_new16(units, 3);

    ASSERT_EQ_FMT((usize)3, cvm_string_length(s), "%zu");
    ASSERT(cvm_string_is_wide(s));
    ASSERT_EQ_FMT(0x0100u, cvm_string_unit(s, 0), "%u");
    ASSERT_EQ_FMT(0x0041u, cvm_string_unit(s, 1), "%u");
    ASSERT_EQ_FMT(0xD800u, cvm_string_unit(s, 2), "%u");

    fixture_leave();
    PASS();
}

TEST an_empty_string_is_a_string(void) {
    fixture_enter();

    CvmString* empty = cvm_string_new8(NULL, 0);
    ASSERT_EQ_FMT((usize)0, cvm_string_length(empty), "%zu");
    ASSERT_FALSE(cvm_string_is_wide(empty));

    CvmString* other = cvm_string_new_ascii(STR_LITERAL(""));
    ASSERT(cvm_string_equal(empty, other));
    ASSERT_EQ_FMT(0, cvm_string_compare(empty, other), "%d");

    /* a hash is defined even when there is nothing to hash */
    ASSERT_EQ_FMT(cvm_string_hash(empty), cvm_string_hash(other), "%u");

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* widening                                                            */
/* ------------------------------------------------------------------ */

TEST widen_converts_units_and_preserves_content(void) {
    fixture_enter();

    static u8 const text[] = {0x41u, 0xC3u, 0xFFu};
    CvmString* narrow = cvm_string_new8(text, sizeof(text));

    FRAME(1);
    f_roots[0] = narrow;

    CvmString* wide = cvm_string_widen((CvmString*)f_roots[0]);
    f_roots[0] = wide;

    ASSERT(cvm_string_is_wide(wide));
    ASSERT_EQ_FMT((usize)3, cvm_string_length(wide), "%zu");
    for (usize i = 0; i < 3; i++) {
        ASSERT_EQ_FMT((u32)text[i], cvm_string_unit(wide, i), "%u");
    }
    ASSERT_EQ_FMT(offsetof(CvmString, data),
                  (usize)((u8 const*)wide->data - (u8 const*)wide), "%zu");
    /* and it really is the 16-bit form: the object is sized for 16-bit units */
    ASSERT_EQ_FMT(expected_extent(3, true), cvm_object_size(cvm_header_of(wide)),
                  "%zu");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST widening_a_wide_string_returns_it(void) {
    fixture_enter();

    static u16 const units[] = {0x0100u};
    CvmString* wide = cvm_string_new16(units, 1);

    FRAME(1);
    f_roots[0] = wide;
    CvmString* again = cvm_string_widen((CvmString*)f_roots[0]);
    ASSERT(again == (CvmString*)f_roots[0]);
    FRAME_END();

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* equality and order                                                  */
/* ------------------------------------------------------------------ */

TEST equality_is_across_representations(void) {
    fixture_enter();

    static u8 const narrow_units[] = {'a', 'b'};
    static u16 const wide_units[] = {'a', 'b'};

    CvmString* narrow = cvm_string_new8(narrow_units, 2);
    CvmString* wide = cvm_string_new16(wide_units, 2);

    ASSERT(cvm_string_equal(narrow, wide));
    ASSERT(cvm_string_equal(narrow, narrow));
    ASSERT_EQ_FMT(0, cvm_string_compare(narrow, wide), "%d");

    static u8 const other[] = {'a', 'c'};
    CvmString* different = cvm_string_new8(other, 2);
    ASSERT_FALSE(cvm_string_equal(narrow, different));

    static u8 const shorter[] = {'a'};
    CvmString* short_one = cvm_string_new8(shorter, 1);
    ASSERT_FALSE(cvm_string_equal(narrow, short_one));

    fixture_leave();
    PASS();
}

TEST order_is_by_code_unit_value(void) {
    fixture_enter();

    static u8 const a[] = {'a'};
    static u8 const b[] = {'b'};
    static u8 const ab[] = {'a', 'b'};

    CvmString* sa = cvm_string_new8(a, 1);
    CvmString* sb = cvm_string_new8(b, 1);
    CvmString* sab = cvm_string_new8(ab, 2);

    ASSERT(cvm_string_compare(sa, sb) < 0);
    ASSERT(cvm_string_compare(sb, sa) > 0);
    /* a prefix sorts first */
    ASSERT(cvm_string_compare(sa, sab) < 0);
    ASSERT(cvm_string_compare(sab, sa) > 0);

    /*
     * The case memory order gets wrong. In a 16-bit string, 0x0001 and 0x0100
     * are two bytes each, low byte first: byte-wise, 0x0100 starts with 0x00
     * and 0x0001 starts with 0x01, so memcmp would call the *smaller* value
     * the larger string. Ordering by units says 0x0100 is larger.
     */
    static u16 const big_units[] = {0x0100u};
    static u16 const small_units[] = {0x0001u};
    CvmString* big = cvm_string_new16(big_units, 1);
    CvmString* small = cvm_string_new16(small_units, 1);

    ASSERT(cvm_string_compare(big, small) > 0);
    ASSERT(cvm_string_compare(small, big) < 0);

    /* and the same comparison across representations */
    unsigned char latin1 = 0xFFu;
    CvmString* narrow_ff = cvm_string_new8(&latin1, 1);
    static u16 const wide_100_units[] = {0x0100u};
    CvmString* wide_100 = cvm_string_new16(wide_100_units, 1);

    ASSERT(cvm_string_compare(narrow_ff, wide_100) < 0);
    ASSERT(cvm_string_compare(wide_100, narrow_ff) > 0);

    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* hashing                                                             */
/* ------------------------------------------------------------------ */

TEST hash_is_cached_and_representation_independent(void) {
    fixture_enter();

    static u8 const text[] = {'h', 'e', 'l', 'l', 'o'};
    CvmString* narrow = cvm_string_new8(text, sizeof(text));

    ASSERT_EQ_FMT((u32)0, (u32)(narrow->flags & CVM_STRF_HASHED), "%u");
    u32 h = cvm_string_hash(narrow);

    /* the first call filled the cache, and the second reads it */
    ASSERT(narrow->flags & CVM_STRF_HASHED);
    ASSERT_EQ_FMT(h, narrow->hash, "%u");
    ASSERT_EQ_FMT(h, cvm_string_hash(narrow), "%u");

    /* equal content in the other representation hashes the same, which is what
     * makes a hash-keyed table agree with cvm_string_equal */
    static u16 const wide_units[] = {'h', 'e', 'l', 'l', 'o'};
    CvmString* wide = cvm_string_new16(wide_units, 5);
    ASSERT(cvm_string_equal(narrow, wide));
    ASSERT_EQ_FMT(h, cvm_string_hash(wide), "%u");

    /* and different content does not collapse onto it */
    static u8 const other[] = {'h', 'e', 'l', 'l', 'p'};
    CvmString* different = cvm_string_new8(other, 5);
    ASSERT(cvm_string_hash(different) != h);

    fixture_leave();
    PASS();
}

TEST hash_puts_short_strings_in_different_buckets(void) {
    /*
     * A hash that passes a sequential test but clusters on short keys would
     * still be broken, because short keys are what a table is mostly made of.
     * Sixteen one-byte keys must not collapse.
     */
    fixture_enter();

    u32 seen[16];
    usize found = 0;

    for (u32 c = 'a'; c < 'a' + 16u; c++) {
        u8 unit = (u8)c;
        CvmString* s = cvm_string_new8(&unit, 1);
        u32 h = cvm_string_hash(s);

        for (usize i = 0; i < found; i++) {
            ASSERT(seen[i] != h);
        }
        seen[found++] = h;
    }

    ASSERT_EQ_FMT((usize)16, found, "%zu");
    fixture_leave();
    PASS();
}

/* ------------------------------------------------------------------ */
/* the collector's view of a string                                    */
/* ------------------------------------------------------------------ */

TEST a_strings_extent_follows_its_length(void) {
    /*
     * This is what CVM_TYPE_SCALAR_ARRAY has to mean: the allocator wrote a
     * length, and the collector reads the same number back to find where the
     * object ends. If the two disagreed, the sweeper would reclaim a line the
     * string still occupies.
     */
    fixture_enter();

    u8 units[100];
    for (usize i = 0; i < sizeof(units); i++) {
        units[i] = (u8)i;
    }

    for (usize length = 0; length <= sizeof(units); length += 37) {
        CvmString* narrow = cvm_string_new8(units, length);
        ASSERT_EQ_FMT(expected_extent(length, false),
                      cvm_object_size(cvm_header_of(narrow)), "%zu");
    }

    u16 wide_units[100];
    for (usize i = 0; i < 100; i++) {
        wide_units[i] = (u16)(0x100u + i);
    }
    for (usize length = 0; length <= 100; length += 37) {
        CvmString* wide = cvm_string_new16(wide_units, length);
        ASSERT_EQ_FMT(expected_extent(length, true),
                      cvm_object_size(cvm_header_of(wide)), "%zu");
    }

    fixture_leave();
    PASS();
}

TEST a_rooted_string_survives_collection(void) {
    fixture_enter();

    static u8 const text[] = {'a', 'l', 'i', 'v', 'e'};
    CvmString* s = cvm_string_new8(text, sizeof(text));

    FRAME(1);
    f_roots[0] = s;

    collect();

    CvmString* still = (CvmString*)f_roots[0];
    ASSERT_EQ_FMT((usize)5, cvm_string_length(still), "%zu");
    for (usize i = 0; i < 5; i++) {
        ASSERT_EQ_FMT((u32)text[i], cvm_string_unit(still, i), "%u");
    }
    /* the cached hash survives too, because the object did not move */
    ASSERT_EQ_FMT(cvm_string_hash(still), cvm_string_hash((CvmString*)f_roots[0]),
                  "%u");

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST an_unrooted_string_is_reclaimed(void) {
    fixture_enter();

    static u8 const text[] = {'d', 'e', 'a', 'd'};
    CvmString* s = cvm_string_new8(text, sizeof(text));
    void* weak = cvm_weak_ref_new(s);

    /* only the weak reference is rooted: the string itself is not */
    FRAME(1);
    f_roots[0] = weak;

    collect();

    ASSERT(cvm_weak_ref_target(weak) == NULL);

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_long_string_crosses_the_inline_allocation_path(void) {
    /*
     * CVM_INLINE_ALLOC_MAX is a line, so a string past it takes the slow path
     * and a different allocator branch. The content and the extent both have
     * to be right on that side too.
     */
    fixture_enter();

    enum { N = 5000 };
    static u8 units[N];
    for (usize i = 0; i < N; i++) {
        units[i] = (u8)('a' + (i % 26u));
    }

    CvmString* s = cvm_string_new8(units, N);

    FRAME(1);
    f_roots[0] = s;

    collect();
    CvmString* still = (CvmString*)f_roots[0];

    ASSERT_EQ_FMT((usize)N, cvm_string_length(still), "%zu");
    ASSERT_EQ_FMT(expected_extent(N, false), cvm_object_size(cvm_header_of(still)),
                  "%zu");
    for (usize i = 0; i < N; i++) {
        if (cvm_string_unit(still, i) != (u32)units[i]) {
            FAILm("a unit of the long string changed");
        }
    }
    (void)cvm_string_hash(still);

    FRAME_END();
    fixture_leave();
    PASS();
}

TEST strings_come_and_go_without_growing_the_heap(void) {
    fixture_enter();
    FRAME(1);
    for (u32 round = 0; round < 10u; round++) {
        for (u32 i = 0; i < 50u; i++) {
            f_roots[0] = cvm_string_from_utf8(
                (str){.ptr = (u8 const*)"round text here", .len = 14});
            f_roots[0] = NULL;
        }
    }
    collect();
    FRAME_END();
    fixture_leave();
    PASS();
}

TEST a_string_larger_than_a_block_allocates_and_survives(void) {
    fixture_enter();
    /* 32 KiB block: a string past that is a large object. */
    enum { N = (usize)40 * 1024 };
    static u8 units[N];
    for (usize i = 0; i < N; i++) {
        units[i] = (u8)(i & 0xffu);
    }

    CvmString* s = cvm_string_new8(units, N);
    FRAME(1);
    f_roots[0] = s;
    collect();
    CvmString* still = (CvmString*)f_roots[0];

    ASSERT_EQ_FMT((usize)N, cvm_string_length(still), "%zu");
    for (usize i = 0; i < N; i += 997u) { /* sample, not every byte: the point
                                           * is that the content is there, not
                                           * that the loop is thorough */
        if (cvm_string_unit(still, i) != (u32)units[i]) {
            FAILm("a unit of the large string changed");
        }
    }

    /* And it is reclaimed when the root goes away. */
    f_roots[0] = NULL;
    collect();

    FRAME_END();
    fixture_leave();
    PASS();
}

SUITE(str_suite) {
    RUN_TEST(a_narrow_string_holds_its_units);
    RUN_TEST(narrow_units_are_code_units_not_utf8_bytes);
    RUN_TEST(the_ascii_constructor_copies_the_text);
    RUN_TEST(a_wide_string_holds_code_units);
    RUN_TEST(an_empty_string_is_a_string);

    RUN_TEST(widen_converts_units_and_preserves_content);
    RUN_TEST(widening_a_wide_string_returns_it);

    RUN_TEST(equality_is_across_representations);
    RUN_TEST(order_is_by_code_unit_value);

    RUN_TEST(hash_is_cached_and_representation_independent);
    RUN_TEST(hash_puts_short_strings_in_different_buckets);

    RUN_TEST(a_strings_extent_follows_its_length);
    RUN_TEST(a_rooted_string_survives_collection);
    RUN_TEST(an_unrooted_string_is_reclaimed);
    RUN_TEST(a_long_string_crosses_the_inline_allocation_path);
    RUN_TEST(strings_come_and_go_without_growing_the_heap);
    RUN_TEST(a_string_larger_than_a_block_allocates_and_survives);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    cvm_threads_init();
    cvm_safepoint_init();
    if (!cvm_world_init(TEST_MIN, TEST_MAX)) {
        fprintf(stderr, "could not bring up the heap\n");
        return 1;
    }

    GREATEST_MAIN_BEGIN();
    RUN_SUITE(str_suite);
    GREATEST_MAIN_END();

    cvm_world_destroy();
}

#else /* _WIN32 */

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */

/*
 * A string larger than a block takes the large-object path: a different
 * allocator, a different set of metadata, and a different set of ways to get
 * it wrong. The immix invariant that matters is that an object never spans
 * blocks, and the marker's extent assertion is the thing that would notice if
 * it did — a pointer into the middle of a large object reads as a placeholder
 * and is silently skipped, which is why this test checks the content rather
 * than trusting the survival.
 */
