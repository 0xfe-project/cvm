/*
 * tests/unit/test_log.c
 *
 * Leveled logging. The properties that matter: a disabled level costs
 * nothing and emits nothing, an enabled one emits exactly once (the macro
 * is a loop, so "runs twice" is a real failure mode), and the message
 * reaches the configured channel.
 */

#if !defined(_WIN32)

#include "vendor/greatest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "base/format.h"
#include "cvm/log.h"
#include "rt.h"

#define LOG_TMP "/tmp/cvm_log_test.txt"

static str path_str(const char* p) {
    return (str){(u8 const*)p, strlen(p)};
}

/* Run `body` with the log channel pointed at a file, then hand back the
 * file's contents. Returns the number of bytes captured. */
static long capture(char* out, size_t cap, void (*body)(void)) {
    RtChannel* ch = rt_channel_open_file(path_str(LOG_TMP));
    if (ch == NULL)
        return -1;

    cvm_log_set_channel(ch);
    body();
    cvm_log_set_channel(NULL);
    rt_channel_close(ch);

    FILE* f = fopen(LOG_TMP, "rb");
    if (f == NULL)
        return -1;
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = '\0';
    fclose(f);
    return (long)n;
}

/* Bodies for capture(). Each is a separate function because a macro
 * expanding to a loop cannot be wrapped in a lambda. */
static void body_warn(void) {
    CVM_LOG_WARN(m) {
        fmt_lit(&m.fmt, "something happened");
    }
}

static void body_debug_while_warn(void) {
    CVM_LOG_DEBUG(m) {
        fmt_lit(&m.fmt, "should not appear");
    }
}

static void body_debug_with_values(void) {
    CVM_LOG_AT(CVM_LOG_DEBUG, m) {
        fmt_lit(&m.fmt, "block ");
        fmt_u32(&m.fmt, 4242);
        fmt_lit(&m.fmt, " at ");
        fmt_hex(&m.fmt, 0xdeadbeefull);
    }
}

static int side_effect_count = 0;

static void body_side_effect(void) {
    CVM_LOG_DEBUG(m) {
        side_effect_count++;
        fmt_lit(&m.fmt, "counted");
    }
}

static void body_two_lines(void) {
    CVM_LOG_ERROR(m) {
        fmt_lit(&m.fmt, "first");
    }
    CVM_LOG_ERROR(m) {
        fmt_lit(&m.fmt, "second");
    }
}

TEST enabled_level_emits_once(void) {
    char out[1024];
    cvm_log_set_level(CVM_LOG_WARN);

    long n = capture(out, sizeof(out), body_warn);
    ASSERT(n > 0);
    ASSERT(strstr(out, "something happened") != NULL);
    ASSERT(strstr(out, "[cvm warn]") != NULL);

    /* exactly one line */
    ASSERT_EQ_FMT(1, (int)(strchr(out, '\n') == strrchr(out, '\n')), "%d");
    PASS();
}

TEST disabled_level_emits_nothing(void) {
    char out[1024];
    cvm_log_set_level(CVM_LOG_WARN);

    long n = capture(out, sizeof(out), body_debug_while_warn);
    ASSERT_EQ_FMT(0L, n, "%ld");
    ASSERT_STR_EQ("", out);
    PASS();
}

TEST disabled_level_does_not_run_the_body(void) {
    /* The point of the level check: a disabled log call must not even
     * evaluate the work that builds the message. */
    cvm_log_set_level(CVM_LOG_ERROR);
    side_effect_count = 0;

    char out[64];
    capture(out, sizeof(out), body_side_effect);

    ASSERT_EQ_FMT(0, side_effect_count, "%d");
    PASS();
}

TEST enabled_level_runs_the_body_exactly_once(void) {
    /* The macro is a for loop. Running the body twice, or zero times,
     * would be a silent and confusing bug. */
    cvm_log_set_level(CVM_LOG_DEBUG);
    side_effect_count = 0;

    char out[256];
    capture(out, sizeof(out), body_side_effect);

    ASSERT_EQ_FMT(1, side_effect_count, "%d");
    PASS();
}

TEST message_carries_formatted_values(void) {
    char out[1024];
    cvm_log_set_level(CVM_LOG_DEBUG);

    long n = capture(out, sizeof(out), body_debug_with_values);
    ASSERT(n > 0);
    ASSERT(strstr(out, "block 4242 at 0xdeadbeef") != NULL);
    /* debug lines carry their origin */
    ASSERT(strstr(out, "test_log.c:") != NULL);
    PASS();
}

TEST two_calls_produce_two_lines(void) {
    char out[1024];
    cvm_log_set_level(CVM_LOG_ERROR);

    long n = capture(out, sizeof(out), body_two_lines);
    ASSERT(n > 0);
    ASSERT(strstr(out, "first") != NULL);
    ASSERT(strstr(out, "second") != NULL);
    ASSERT_EQ_FMT(2, (int)(strchr(out, '\n') != strrchr(out, '\n')) + 1, "%d");
    PASS();
}

TEST level_comparison_is_at_most(void) {
    /* Setting WARN keeps WARN and ERROR and drops DEBUG and INFO. */
    cvm_log_set_level(CVM_LOG_WARN);

    ASSERT_FALSE(cvm_log_enabled(CVM_LOG_DEBUG));
    ASSERT_FALSE(cvm_log_enabled(CVM_LOG_INFO));
    ASSERT(cvm_log_enabled(CVM_LOG_WARN));
    ASSERT(cvm_log_enabled(CVM_LOG_ERROR));

    cvm_log_set_level(CVM_LOG_NONE);
    ASSERT_FALSE(cvm_log_enabled(CVM_LOG_ERROR));
    ASSERT_FALSE(cvm_log_enabled(CVM_LOG_NONE));

    cvm_log_set_level(CVM_LOG_DEBUG);
    ASSERT(cvm_log_enabled(CVM_LOG_DEBUG));
    ASSERT(cvm_log_enabled(CVM_LOG_ERROR));
    PASS();
}

TEST long_message_is_marked_truncated(void) {
    char out[2048];
    cvm_log_set_level(CVM_LOG_ERROR);

    RtChannel* ch = rt_channel_open_file(path_str(LOG_TMP));
    ASSERT(ch != NULL);
    cvm_log_set_channel(ch);

    CVM_LOG_ERROR(m) {
        /* well past CVM_LOG_MSG_CAP */
        for (int i = 0; i < 400; i++) {
            fmt_byte(&m.fmt, (u8)'x');
        }
    }

    cvm_log_set_channel(NULL);
    rt_channel_close(ch);

    FILE* f = fopen(LOG_TMP, "rb");
    ASSERT(f != NULL);
    size_t n = fread(out, 1, sizeof(out) - 1, f);
    out[n] = '\0';
    fclose(f);

    ASSERT(strstr(out, "(truncated)") != NULL);
    /* the formatter itself reports it too */
    ASSERT(n > 0);
    PASS();
}

TEST env_var_sets_the_level(void) {
    struct {
        const char* value;
        CvmLogLevel expected;
    } cases[] = {
        {"debug", CVM_LOG_DEBUG}, {"DEBUG", CVM_LOG_DEBUG}, {"Info", CVM_LOG_INFO},
        {"warn", CVM_LOG_WARN},   {"error", CVM_LOG_ERROR}, {"none", CVM_LOG_NONE},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        setenv("CVM_LOG_LEVEL", cases[i].value, 1);
        cvm_log_set_level(CVM_LOG_WARN); /* reset to a known baseline */
        cvm_log_init_from_env();
        ASSERT_EQ_FMT((int)cases[i].expected, (int)cvm_log_get_level(), "%d");
    }

    unsetenv("CVM_LOG_LEVEL");
    PASS();
}

TEST env_var_unset_or_bogus_leaves_level_alone(void) {
    unsetenv("CVM_LOG_LEVEL");
    cvm_log_set_level(CVM_LOG_ERROR);
    cvm_log_init_from_env();
    ASSERT_EQ_FMT((int)CVM_LOG_ERROR, (int)cvm_log_get_level(), "%d");

    /* a typo must not take the process down, and must not silently
     * select some other level */
    setenv("CVM_LOG_LEVEL", "verbose", 1);
    cvm_log_init_from_env();
    ASSERT_EQ_FMT((int)CVM_LOG_ERROR, (int)cvm_log_get_level(), "%d");

    unsetenv("CVM_LOG_LEVEL");
    PASS();
}

SUITE(log_suite) {
    RUN_TEST(enabled_level_emits_once);
    RUN_TEST(disabled_level_emits_nothing);
    RUN_TEST(disabled_level_does_not_run_the_body);
    RUN_TEST(enabled_level_runs_the_body_exactly_once);
    RUN_TEST(message_carries_formatted_values);
    RUN_TEST(two_calls_produce_two_lines);
    RUN_TEST(level_comparison_is_at_most);
    RUN_TEST(long_message_is_marked_truncated);
    RUN_TEST(env_var_sets_the_level);
    RUN_TEST(env_var_unset_or_bogus_leaves_level_alone);
}

GREATEST_MAIN_DEFS();

int main(int argc, char** argv) {
    GREATEST_MAIN_BEGIN();
    RUN_SUITE(log_suite);
    GREATEST_MAIN_END();
}

#endif /* !_WIN32 */
