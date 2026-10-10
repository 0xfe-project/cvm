/*
 * tests/unit/test_rt_channel.c
 *
 * Output channels. These are the collector's only way to say anything, so
 * the properties that matter are: a write lands whole, a write to a bad
 * channel fails rather than crashing, and the standard channels are not
 * accidentally closed by a caller that meant to close its own.
 *
 * POSIX-only, like the implementation it exercises.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "base/types.h"
#include "rt.h"

#define TMP_PATH "/tmp/cvm_rt_channel_test.txt"

static const char* TMP_PATH_CSTR = TMP_PATH;

static str path_str(const char* p) {
    return (str){(u8 const*)p, strlen(p)};
}

/* Read a file back as a NUL-terminated buffer. Returns bytes read, or -1. */
static long read_back(char* out, size_t cap) {
    FILE* f = fopen(TMP_PATH_CSTR, "rb");
    if (f == NULL)
        return -1;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
    return (long)n;
}

TEST standard_channels_exist_and_are_distinct(void) {
    RtChannel* err = rt_channel_stderr();
    RtChannel* out = rt_channel_stdout();

    ASSERT(err != NULL);
    ASSERT(out != NULL);
    ASSERT(err != out);
    PASS();
}

TEST write_to_a_file_and_read_it_back(void) {
    RtChannel* ch = rt_channel_open_file(path_str(TMP_PATH));
    ASSERT(ch != NULL);

    static const u8 msg[] = "first line\nsecond line\n";
    ASSERT(rt_channel_write(ch, (span_const){msg, sizeof(msg) - 1}));
    rt_channel_flush(ch);
    rt_channel_close(ch);

    char got[256];
    ASSERT(read_back(got, sizeof(got)) > 0);
    ASSERT_STR_EQ("first line\nsecond line\n", got);
    PASS();
}

TEST opening_truncates_an_existing_file(void) {
    /* First write. */
    RtChannel* ch = rt_channel_open_file(path_str(TMP_PATH));
    ASSERT(ch != NULL);
    static const u8 longer[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    rt_channel_write(ch, (span_const){longer, sizeof(longer) - 1});
    rt_channel_close(ch);

    /* Reopen with a shorter payload: the old tail must be gone. */
    ch = rt_channel_open_file(path_str(TMP_PATH));
    ASSERT(ch != NULL);
    static const u8 shorter[] = "bb";
    rt_channel_write(ch, (span_const){shorter, sizeof(shorter) - 1});
    rt_channel_close(ch);

    char got[256];
    ASSERT(read_back(got, sizeof(got)) > 0);
    ASSERT_STR_EQ("bb", got);
    PASS();
}

TEST opening_a_bad_path_fails_cleanly(void) {
    RtChannel* ch =
        rt_channel_open_file(path_str("/definitely/not/a/directory/anywhere/file.txt"));
    ASSERT(ch == NULL);
    PASS();
}

TEST opening_an_empty_path_fails_cleanly(void) {
    RtChannel* ch = rt_channel_open_file((str){NULL, 0});
    ASSERT(ch == NULL);
    PASS();
}

TEST write_of_nothing_succeeds(void) {
    RtChannel* ch = rt_channel_stderr();
    ASSERT(rt_channel_write(ch, (span_const){NULL, 0}));
    PASS();
}

TEST closing_a_standard_channel_is_a_no_op(void) {
    /* A caller that closes its own channel must not be able to take out
     * stderr as a side effect, and a subsequent write must still work. */
    RtChannel* err = rt_channel_stderr();
    rt_channel_close(err);
    rt_channel_close(NULL);

    static const u8 msg[] = "still writable\n";
    ASSERT(rt_channel_write(err, (span_const){msg, sizeof(msg) - 1}));
    PASS();
}

TEST large_write_is_not_truncated(void) {
    /* Exercises the partial-write loop: a payload well past any single
     * write(2) the kernel will accept in one go. */
    enum { N = 300000 };
    static u8 big[N];
    memset(big, 'x', sizeof(big));

    RtChannel* ch = rt_channel_open_file(path_str(TMP_PATH));
    ASSERT(ch != NULL);
    ASSERT(rt_channel_write(ch, (span_const){big, sizeof(big)}));
    rt_channel_close(ch);

    FILE* f = fopen(TMP_PATH_CSTR, "rb");
    ASSERT(f != NULL);
    long total = 0;
    u8 chunk[8192];
    size_t n;
    bool all_x = true;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        for (size_t i = 0; i < n; i++) {
            if (chunk[i] != 'x')
                all_x = false;
        }
        total += (long)n;
    }
    fclose(f);

    ASSERT_EQ_FMT((long)N, total, "%ld");
    ASSERT(all_x);
    PASS();
}

TEST time_is_monotonic(void) {
    u64 a = rt_time_nanos();
    u64 b = rt_time_nanos();
    u64 c = rt_time_nanos();

    /* A monotonic clock never goes backwards. Equal values are allowed on
     * coarse clocks, so the assertions are non-strict. */
    ASSERT(a <= b);
    ASSERT(b <= c);

    u64 ms = rt_time_millis();
    /* milliseconds must be in the same ballpark as nanos/1e6, which also
     * catches a unit mix-up in either function */
    ASSERT(ms <= (c / (u64)1000000) + (u64)1000);
    ASSERT(ms + (u64)1000 >= (c / (u64)1000000));
    PASS();
}

TEST abi_version_symbol_is_reachable(void) {
    ASSERT(rt_abi_version_ptr() != NULL);
    ASSERT_EQ_FMT(RT_ABI_VERSION, *rt_abi_version_ptr(), "%u");
    ASSERT_EQ_FMT(RT_ABI_VERSION, rt_abi_version, "%u");
    PASS();
}

SUITE(rt_channel_suite) {
    RUN_TEST(standard_channels_exist_and_are_distinct);
    RUN_TEST(write_to_a_file_and_read_it_back);
    RUN_TEST(opening_truncates_an_existing_file);
    RUN_TEST(opening_a_bad_path_fails_cleanly);
    RUN_TEST(opening_an_empty_path_fails_cleanly);
    RUN_TEST(write_of_nothing_succeeds);
    RUN_TEST(closing_a_standard_channel_is_a_no_op);
    RUN_TEST(large_write_is_not_truncated);
    RUN_TEST(time_is_monotonic);
    RUN_TEST(abi_version_symbol_is_reachable);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(rt_channel_suite);
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
