/*
 * vm/log.c
 *
 * Leveled logging. See vm/include/cvm/log.h for the call-site form.
 *
 * Output goes through an RtChannel, so this file never sees a FILE* and
 * the destination can be swapped without touching the collector. Messages
 * are written with a single channel write, which is what keeps concurrent
 * logging from interleaving mid-line.
 */

#include "cvm/log.h"

#include "base/format.h"
#include "rt.h"

/*
 * Read on every log check, written once during initialisation before any
 * collector thread exists. See the header for why this is not atomic.
 */
u32 cvm_log_level = (u32)CVM_LOG_WARN;

static RtChannel* log_channel = NULL;

void cvm_log_set_level(CvmLogLevel level) {
    cvm_log_level = (u32)level;
}

CvmLogLevel cvm_log_get_level(void) {
    return (CvmLogLevel)cvm_log_level;
}

void cvm_log_set_channel(RtChannel* ch) {
    log_channel = ch;
}

RtChannel* cvm_log_get_channel(void) {
    return log_channel;
}

/* ------------------------------------------------------------------ */
/* level names and parsing                                             */
/* ------------------------------------------------------------------ */

static str level_tag(CvmLogLevel level) {
    switch (level) {
    case CVM_LOG_DEBUG:
        return STR_LITERAL("debug");
    case CVM_LOG_INFO:
        return STR_LITERAL("info");
    case CVM_LOG_WARN:
        return STR_LITERAL("warn");
    case CVM_LOG_ERROR:
        return STR_LITERAL("error");
    case CVM_LOG_NONE:
        break;
    }
    return STR_LITERAL("none");
}

/* Case-insensitive comparison, since the variable is typed by hand. */
static bool str_eq_ci(str a, str b) {
    if (a.len != b.len) {
        return false;
    }
    for (usize i = 0; i < a.len; i++) {
        u8 ca = a.ptr[i];
        u8 cb = b.ptr[i];
        if (ca >= 'A' && ca <= 'Z')
            ca = (u8)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (u8)(cb - 'A' + 'a');
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

static bool parse_level(str text, CvmLogLevel* out) {
    static const struct {
        str name;
        CvmLogLevel level;
    } table[] = {
        {STR_LITERAL("debug"), CVM_LOG_DEBUG}, {STR_LITERAL("info"), CVM_LOG_INFO},
        {STR_LITERAL("warn"), CVM_LOG_WARN},   {STR_LITERAL("error"), CVM_LOG_ERROR},
        {STR_LITERAL("none"), CVM_LOG_NONE},
    };

    for (usize i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (str_eq_ci(text, table[i].name)) {
            *out = table[i].level;
            return true;
        }
    }
    return false;
}

void cvm_log_init_from_env(void) {
    str value = rt_env_get(STR_LITERAL("CVM_LOG_LEVEL"));
    if (value.len == 0) {
        return;
    }

    CvmLogLevel parsed;
    if (parse_level(value, &parsed)) {
        cvm_log_set_level(parsed);
    }
    /* An unrecognised value is ignored rather than fatal: a typo in an
     * environment variable should not take down the process. */
}

/* ------------------------------------------------------------------ */
/* emission                                                            */
/* ------------------------------------------------------------------ */

/* Longer than one message, because the prefix and file:line are added. */
#define EMIT_CAP (CVM_LOG_MSG_CAP + 128)

void cvm_log_emit(CvmLogLevel level, str file, u32 line, str message) {
    u8 buf[EMIT_CAP];
    CvmFmt f;
    fmt_init(&f, buf, sizeof(buf));

    fmt_lit(&f, "[cvm ");
    fmt_str(&f, level_tag(level));
    fmt_lit(&f, "] ");

    /* Debug lines carry their origin, because that is the level used to
     * trace collector internals and the call site is the first thing
     * anyone wants. Warnings and errors are few enough to be found by
     * their text. */
    if (level == CVM_LOG_DEBUG) {
        fmt_str(&f, file);
        fmt_lit(&f, ":");
        fmt_u32(&f, line);
        fmt_lit(&f, " ");
    }

    fmt_str(&f, message);

    if (message.len >= CVM_LOG_MSG_CAP) {
        fmt_lit(&f, " (truncated)");
    }
    fmt_lit(&f, "\n");

    RtChannel* ch = (log_channel != NULL) ? log_channel : rt_channel_stderr();
    rt_channel_write(ch, (span_const){buf, f.len});
}

bool cvm_log_begin(CvmLogMsg* m, CvmLogLevel level, str file, u32 line) {
    if (!cvm_log_enabled(level)) {
        return false;
    }

    m->level = (u32)level;
    m->file = file;
    m->line = line;
    fmt_init(&m->fmt, m->buf, CVM_LOG_MSG_CAP);
    return true;
}

bool cvm_log_end(CvmLogMsg* m) {
    cvm_log_emit((CvmLogLevel)m->level, m->file, m->line, fmt_result(&m->fmt));
    return false; /* ends the loop that the macro opened */
}
