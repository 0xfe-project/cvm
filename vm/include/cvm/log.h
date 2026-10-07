/*
 * vm/log.h
 *
 * Leveled logging for the collector.
 *
 * There is no printf in a freestanding translation unit, so a log call
 * cannot take a format string and varargs. Instead a call site fills a
 * bounded buffer with the formatter from base/format.h:
 *
 *     CVM_LOG_AT(CVM_LOG_DEBUG, m) {
 *         fmt_lit(&m.fmt, "block ");
 *         fmt_u32(&m.fmt, index);
 *         fmt_lit(&m.fmt, " marked");
 *     }
 *
 * That expands to a single loop whose body is the block. The header
 * function runs only when the level is enabled, so a disabled log call
 * costs one comparison and no formatting at all.
 *
 * Why a loop and not a do/while or a statement expression: the body has to
 * run between "we decided to log" and "emit", and statement expressions
 * are a GNU extension that MSVC does not have. A for loop is the portable
 * way to express that.
 *
 * A message is emitted when its level is at least the configured
 * threshold, so setting the level to WARN silences DEBUG and INFO while
 * keeping WARN and ERROR.
 */

#ifndef CVM_LOG_H
#define CVM_LOG_H

#include "base/format.h"
#include "base/types.h"
#include "rt.h"

typedef enum CvmLogLevel {
    CVM_LOG_DEBUG = 0,
    CVM_LOG_INFO = 1,
    CVM_LOG_WARN = 2,
    CVM_LOG_ERROR = 3,
    CVM_LOG_NONE = 4,
} CvmLogLevel;

/* Size of one message. Longer messages are truncated and say so. */
#define CVM_LOG_MSG_CAP 256

/*
 * One in-progress message. Carries its own buffer so that the call-site
 * macro does not need to name one.
 */
typedef struct CvmLogMsg {
    CvmFmt fmt;
    u32 level;
    str file;
    u32 line;
    u8 buf[CVM_LOG_MSG_CAP];
} CvmLogMsg;

/* ------------------------------------------------------------------ */
/* configuration                                                       */
/* ------------------------------------------------------------------ */

/*
 * The level is a plain global, read on every log check. It is written
 * once during initialisation, before any collector thread exists, so no
 * synchronisation is needed. Changing it at runtime is not supported; if
 * that is ever wanted, it becomes atomic and the reasoning changes.
 */
extern u32 cvm_log_level;

void cvm_log_set_level(CvmLogLevel level);
CvmLogLevel cvm_log_get_level(void);

/* Destination. NULL means stderr. */
void cvm_log_set_channel(RtChannel* ch);
RtChannel* cvm_log_get_channel(void);

/*
 * Read the level from the environment (CVM_LOG_LEVEL: debug, info, warn,
 * error, none). Unset or unrecognised leaves the current level alone.
 */
void cvm_log_init_from_env(void);

/*
 * A message is emitted when its level is at least the configured
 * threshold: with the level set to WARN, DEBUG and INFO messages are
 * dropped and WARN and ERROR messages are kept.
 *
 * The comparison direction matters and is easy to get backwards. The
 * levels are ordered by increasing severity (DEBUG = 0 ... ERROR = 3), so
 * "at least the threshold" is `level >= threshold`, not `<=`.
 *
 * CVM_LOG_NONE is a threshold meaning "silence everything"; it is never a
 * level a message carries, so the query is answered false rather than
 * being left to fall out of the arithmetic.
 */
CVM_INLINE bool cvm_log_enabled(CvmLogLevel level) {
    if (level == CVM_LOG_NONE) {
        return false;
    }
    return (u32)level >= cvm_log_level;
}

/* ------------------------------------------------------------------ */
/* emission                                                            */
/* ------------------------------------------------------------------ */

/* Writes a finished message, adding the level tag, file and line. */
void cvm_log_emit(CvmLogLevel level, str file, u32 line, str message);

/* Used by the macro below. begin returns false when the level is off, so
 * the body never runs; end emits and returns false, ending the loop. */
bool cvm_log_begin(CvmLogMsg* m, CvmLogLevel level, str file, u32 line);
bool cvm_log_end(CvmLogMsg* m);

/*
 * The call-site form:
 *
 *     CVM_LOG_AT(CVM_LOG_WARN, m) { fmt_lit(&m.fmt, "..."); }
 *
 * `m` is the name the body uses to reach the formatter.
 *
 * How this works, because the obvious spelling is wrong. A for loop
 * evaluates its increment expression and *discards the result*: only the
 * condition decides whether to iterate. So
 *
 *     for (CvmLogMsg m; cvm_log_begin(&m, ...); cvm_log_end(&m))
 *
 * does not terminate. After the body, cvm_log_end runs and its false
 * return value is thrown away; the condition is then evaluated again,
 * cvm_log_begin returns true again, and the loop spins forever. This was
 * the actual behaviour, confirmed in the generated assembly: the call to
 * cvm_log_end was followed by an unconditional back edge.
 *
 * The fix carries the "we have already run" state in the condition
 * itself, through a pointer that the increment clears:
 *
 *   1. condition: the pointer is live, so ask whether this level is on.
 *      If it is off, begin returns false and the body never runs.
 *   2. body runs once.
 *   3. increment: clear the pointer, then emit.
 *   4. condition: the pointer is NULL, so the loop ends.
 *
 * A `break` inside the body skips the emit, since the increment never
 * runs. That is a reasonable reading of break, and unusual enough to be
 * worth only this note.
 */
#define CVM_LOG_AT(level, m)                                                           \
    for (CvmLogMsg m, *cvm_log_ctx_ = &m;                                              \
         cvm_log_ctx_ != NULL &&                                                       \
         cvm_log_begin(cvm_log_ctx_, (level), STR_LITERAL(__FILE__), (u32)__LINE__);   \
         cvm_log_ctx_ = NULL, cvm_log_end(&m))

#define CVM_LOG_DEBUG(m) CVM_LOG_AT(CVM_LOG_DEBUG, m)
#define CVM_LOG_INFO(m) CVM_LOG_AT(CVM_LOG_INFO, m)
#define CVM_LOG_WARN(m) CVM_LOG_AT(CVM_LOG_WARN, m)
#define CVM_LOG_ERROR(m) CVM_LOG_AT(CVM_LOG_ERROR, m)

#endif /* CVM_LOG_H */
