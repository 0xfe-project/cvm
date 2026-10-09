/*
 * tests/unit/test_buf.c
 *
 * The growable byte buffer: appends, inserts, the integer widths, and the one
 * path that is easy to write and hard to reach — an allocator that fails.
 *
 * The failing allocator is the reason this file has a stub one at all: a buffer
 * that reports an error has to keep its contents and its size intact, and the
 * only way to see that is to make an allocation fail on purpose.
 */

#include "vendor/greatest.h"

#include <stdio.h>
#include <string.h>

#include "base/alloc.h"
#include "base/types.h"
#include "cvm/buf.h"

/* An allocator that refuses, for the error path. */
static opaque_mut refusing_alloc(opaque_mut ctx, usize bytes) {
    (void)ctx;
    (void)bytes;
    return NULL;
}

static void refusing_free(opaque_mut ctx, opaque_mut p) {
    (void)ctx;
    (void)p;
}

/* Returns whether the buffer holds exactly these bytes. A bool rather than
 * assertions inside, because greatest's macros return from their own function. */
static bool holds(CvmBuf* buf, u8 const* want, usize n) {
    return buf->size == n && memcmp(buf->data, want, n) == 0;
}

TEST an_empty_buffer_appends_and_keeps_order(void) {
    CvmBuf buf;
    cvm_buf_init(&buf, cvm_buf_system_alloc());

    ASSERT_EQ_FMT((usize)0, cvm_buf_size(&buf), "%zu");
    ASSERT_FALSE(cvm_buf_error(&buf));

    ASSERT(cvm_buf_putc(&buf, 'a'));
    static u8 const more[] = {'b', 'c', 'd'};
    ASSERT(cvm_buf_put(&buf, more, sizeof(more)));

    static u8 const want[] = {'a', 'b', 'c', 'd'};
    ASSERT(holds(&buf, want, sizeof(want)));

    /* and a free leaves it usable rather than dangling */
    cvm_buf_free(&buf);
    ASSERT_EQ_FMT((usize)0, cvm_buf_size(&buf), "%zu");
    ASSERT(cvm_buf_putc(&buf, 'z'));
    ASSERT_EQ_FMT('z', (int)buf.data[0], "%c");
    cvm_buf_free(&buf);
    PASS();
}

TEST integers_go_in_little_endian(void) {
    CvmBuf buf;
    cvm_buf_init(&buf, cvm_buf_system_alloc());

    ASSERT(cvm_buf_put_u16(&buf, 0x0102u));
    ASSERT(cvm_buf_put_u32(&buf, 0x03040506u));
    ASSERT(cvm_buf_put_u64(&buf, 0x0708090A0B0C0D0Eull));

    static u8 const want[] = {
        0x02, 0x01,                                     /* u16 */
        0x06, 0x05, 0x04, 0x03,                         /* u32 */
        0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, /* u64 */
    };
    ASSERT(holds(&buf, want, sizeof(want)));

    cvm_buf_free(&buf);
    PASS();
}

TEST insert_moves_the_tail_and_may_overlap(void) {
    CvmBuf buf;
    cvm_buf_init(&buf, cvm_buf_system_alloc());

    static u8 const abc[] = {'a', 'b', 'c'};
    ASSERT(cvm_buf_put(&buf, abc, sizeof(abc)));

    /* at the front, in the middle, and at the end */
    static u8 const x[] = {'x'};
    ASSERT(cvm_buf_insert(&buf, 0u, x, 1u));
    static u8 const y[] = {'y'};
    ASSERT(cvm_buf_insert(&buf, 2u, y, 1u));
    static u8 const z[] = {'z'};
    ASSERT(cvm_buf_insert(&buf, cvm_buf_size(&buf), z, 1u));

    static u8 const want[] = {'x', 'a', 'y', 'b', 'c', 'z'};
    ASSERT(holds(&buf, want, sizeof(want)));

    /* an insert of a region of the buffer into itself: the tail has to move out
     * of the way first, which is what the backwards copy is for */
    CvmBuf self;
    cvm_buf_init(&self, cvm_buf_system_alloc());
    static u8 const abcd[] = {'a', 'b', 'c', 'd'};
    ASSERT(cvm_buf_put(&self, abcd, sizeof(abcd)));
    ASSERT(cvm_buf_insert(&self, 1u, self.data, 2u)); /* "ab" into position 1 */
    static u8 const doubled[] = {'a', 'a', 'b', 'b', 'c', 'd'};
    ASSERT(holds(&self, doubled, sizeof(doubled)));

    cvm_buf_free(&self);
    cvm_buf_free(&buf);
    PASS();
}

TEST growing_keeps_what_was_there(void) {
    CvmBuf buf;
    cvm_buf_init(&buf, cvm_buf_system_alloc());

    enum { N = 5000 };
    for (u32 i = 0; i < N; i++) {
        if (!cvm_buf_putc(&buf, (u8)(i & 0xffu))) {
            FAILm("the buffer refused a byte");
        }
    }
    ASSERT_EQ_FMT((usize)N, cvm_buf_size(&buf), "%zu");
    for (u32 i = 0; i < N; i++) {
        if (buf.data[i] != (u8)(i & 0xffu)) {
            FAILm("a byte was lost or moved during growth");
        }
    }

    /* an explicit reserve is honoured rather than ignored */
    usize before = buf.allocated;
    ASSERT(cvm_buf_reserve(&buf, before * 4u));
    ASSERT(buf.allocated >= buf.size + before * 4u);
    ASSERT_EQ_FMT((usize)N, cvm_buf_size(&buf), "%zu");
    ASSERT(buf.data[0] == 0u);

    cvm_buf_free(&buf);
    PASS();
}

TEST a_failing_allocator_is_reported_and_keeps_what_was_there(void) {
    CvmBuf buf;
    CvmAlloc refusing = {.ctx = NULL, .alloc = refusing_alloc, .free = refusing_free};
    cvm_buf_init(&buf, refusing);

    /* nothing has been allocated yet, so the first append is the one that fails */
    ASSERT_FALSE(cvm_buf_putc(&buf, 'a'));
    ASSERT(cvm_buf_error(&buf));
    ASSERT_EQ_FMT((usize)0, cvm_buf_size(&buf), "%zu");

    /* and the failure is sticky: later appends do not retry, and do not pretend
     * they worked */
    ASSERT_FALSE(cvm_buf_put(&buf, "xy", 2u));
    ASSERT_EQ_FMT((usize)0, cvm_buf_size(&buf), "%zu");

    cvm_buf_free(&buf);
    PASS();
}

SUITE(buf_suite) {
    RUN_TEST(an_empty_buffer_appends_and_keeps_order);
    RUN_TEST(integers_go_in_little_endian);
    RUN_TEST(insert_moves_the_tail_and_may_overlap);
    RUN_TEST(growing_keeps_what_was_there);
    RUN_TEST(a_failing_allocator_is_reported_and_keeps_what_was_there);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(buf_suite);
    GREATEST_MAIN_END();
}
