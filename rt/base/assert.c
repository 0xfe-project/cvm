/*
 * rt/base/assert.c
 *
 * The out-of-line half of cvm_assert (see base/assert.h).
 *
 * Reporting a failure means formatting a message and writing it to a file
 * descriptor, then terminating the process — all platform operations, so
 * this lives in rt rather than being inlined into vm.
 *
 * The message is built with our own formatter (base/format.h) instead of
 * printf, which keeps the shape of the output under our control and means
 * the failure path does not depend on stdio being in a sane state.
 */

#include "base/assert.h"

#include "base/format.h"
#include "rt.h"

/* Room for the condition text plus the location. Assertions are short;
 * a longer one is truncated rather than costing unbounded stack. */
#define ASSERT_MESSAGE_CAP 512

void rt_assert_fail(str file, u32 line, str expr) {
    u8 buf[ASSERT_MESSAGE_CAP];
    CvmFmt f;

    fmt_init(&f, buf, sizeof(buf));
    fmt_lit(&f, "assertion failed: ");
    fmt_str(&f, expr);
    fmt_lit(&f, "\n    at ");
    fmt_str(&f, file);
    fmt_lit(&f, ":");
    fmt_u32(&f, line);
    fmt_lit(&f, "\n");

    if (f.truncated) {
        /* Say so rather than emitting a plausible-looking half line. */
        static const u8 note[] = "    (message truncated)\n";
        rt_channel_write(rt_channel_stderr(), (span_const){note, sizeof(note) - 1});
    }

    rt_channel_write(rt_channel_stderr(), (span_const){buf, f.len});
    rt_abort();
}
