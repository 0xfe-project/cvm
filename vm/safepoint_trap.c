/*
 * vm/safepoint_trap.c
 *
 * Trap-based safepoints: the collector's half of the mechanism.
 *
 * See vm/safepoint_trap.S for the trampoline, and
 * rt/pal/posix/trap.c for the page and the signal handler. This file is
 * what sits between them: it decides whether a fault was a trap, and it
 * runs the parking that the handler deliberately does not.
 *
 * ---------------------------------------------------------------------
 * The two-step handoff
 * ---------------------------------------------------------------------
 *
 * The signal handler runs in async-signal context, where taking a lock is
 * forbidden. So it does the only thing it can: it records where the
 * faulting instruction was and repoints the saved instruction pointer at
 * the trampoline. The trampoline then runs on the faulting thread's own
 * stack, in ordinary context, and calls cvm_safepoint_stub_park — which is
 * ordinary collector code and may block.
 *
 * When parking is over, the trampoline asks for the resume address and
 * jumps there. That address is the faulting load itself, not the
 * instruction after it: by the time it runs again the page is readable,
 * so the load succeeds and the program continues as though nothing
 * happened.
 *
 * ---------------------------------------------------------------------
 * Which fault is ours
 * ---------------------------------------------------------------------
 *
 * A fault is treated as a trap only when its address is a registered trap
 * page. Anything else is passed to the previous handler, so a program with
 * a real segmentation fault still dies with the right signal and a core
 * dump rather than being swallowed by the collector.
 */

#include "safepoint.h"

#include "base/types.h"
#include "context.h"
#include "cvm/gc.h"
#include "log.h"
#include "rt.h"

#if defined(CVM_SAFEPOINT_TRAPS)

/*
 * The cell to poll, in its own TLS slot.
 *
 * Its own slot rather than a field of the context, because the poll reads
 * this on every function entry: one segment-offset load beats two loads,
 * and the second would be a heap access.
 */
__thread opaque_mut cvm_tls_trap_cell = NULL;

static bool trap_installed = false;

/* Called by the trampoline, in ordinary context. */
void cvm_safepoint_stub_park(void);
u64 cvm_safepoint_stub_resume_pc(void);

/* The trampoline itself, in safepoint_trap.S. */
void cvm_safepoint_stub(void);

bool cvm_safepoint_trap_usable(void) {
    return trap_installed;
}

/* ------------------------------------------------------------------ */
/* the fault handler                                                   */
/* ------------------------------------------------------------------ */

static bool on_trap_fault(opaque_mut context, u64 fault_address, opaque_mut user) {
    (void)user;

    CvmThreadNode* node = cvm_thread_node();
    if (node == NULL || node->trap == NULL) {
        return false;
    }

    /*
     * Only a read of this thread's own cell is a trap. Comparing against
     * the registered pages is what lets a genuine fault through, and it is
     * why the page tables are never unmapped.
     */
    if (fault_address != (u64)(usize)rt_trap_page_cell(node->trap)) {
        return false;
    }

    /*
     * Record where we were and point execution at the trampoline. The
     * address recorded is the faulting instruction, not the one after it,
     * so that the retry re-executes the load that faulted.
     */
    node->safepoint_resume_pc = rt_trap_context_pc(context);
    rt_trap_context_set_pc(context, (u64)(usize)&cvm_safepoint_stub);
    return true;
}

/* ------------------------------------------------------------------ */
/* setup                                                               */
/* ------------------------------------------------------------------ */

bool cvm_safepoint_trap_init(void) {
    if (trap_installed) {
        return true;
    }

    if (!rt_trap_handler_install(on_trap_fault, NULL)) {
        CVM_LOG_WARN(m) {
            fmt_lit(&m.fmt, "could not install the safepoint trap handler; "
                            "falling back to polling");
        }
        return false;
    }

    trap_installed = true;
    return true;
}

void cvm_safepoint_trap_attach(CvmThreadNode* node) {
    if (!trap_installed) {
        /*
         * Fatal, not a silent skip. In trap mode the poll does not consult
         * the flag at all, so a thread without a page would be reached by
         * a null load — and a null load is a fault the handler cannot
         * recognise, so the process dies in whatever code was polling.
         * Saying so here names the actual problem: safepoints were not
         * initialised before a thread tried to register.
         */
        rt_fatal(STR_LITERAL("a thread registered before the safepoint "
                             "synchroniser was initialised"));
    }

    node->trap = rt_trap_page_create();
    if (node->trap == NULL) {
        /*
         * Fatal rather than a warning, and not a fallback to the flag: in
         * trap mode the poll does not look at the flag, so a thread
         * without a page could never be stopped. A page is one mmap, so
         * failing to get one means the process is out of address space
         * and has nothing to continue with.
         */
        rt_fatal(STR_LITERAL("could not create a safepoint trap page"));
    }

    rt_trap_page_disarm(node->trap);

    /*
     * Installed into the TLS slot so the poll can reach it in one load.
     * The context also points at it, for the handler.
     */
    cvm_tls_trap_cell = rt_trap_page_cell(node->trap);

    /*
     * Deliberately nothing here about arming for a collection that may be
     * running. The caller does that after registering this thread, under
     * the stop-the-world mutex, because arming before registering is how a
     * page ends up armed with no collection running.
     */
}

void cvm_safepoint_trap_detach(CvmThreadNode* node) {
    if (node->trap == NULL) {
        return;
    }
    rt_trap_page_release(node->trap);
    node->trap = NULL;
    cvm_tls_trap_cell = NULL;
}

/* ------------------------------------------------------------------ */
/* arming and disarming                                                */
/* ------------------------------------------------------------------ */

void cvm_safepoint_trap_arm_all(void) {
    if (!trap_installed) {
        return;
    }

    /*
     * Cleared before every arming, not only at startup: a library loaded
     * later can install a task-level Mach exception handler, and it would
     * receive the fault instead of the signal handler.
     */
    rt_trap_reset_platform_exception_ports();

    cvm_threads_lock_read();
    CVM_THREADS_FOREACH(node) {
        if (node->trap != NULL) {
            rt_trap_page_arm(node->trap);
        }
    }
    cvm_threads_unlock_read();
}

void cvm_safepoint_trap_disarm_all(void) {
    if (!trap_installed) {
        return;
    }

    cvm_threads_lock_read();
    CVM_THREADS_FOREACH(node) {
        if (node->trap != NULL) {
            rt_trap_page_disarm(node->trap);
        }
    }
    cvm_threads_unlock_read();
}

/* ------------------------------------------------------------------ */
/* the trampoline's C side                                             */
/* ------------------------------------------------------------------ */

/*
 * Park this thread until the collection is over.
 *
 * Reached through the trampoline rather than from the signal handler, so
 * this is ordinary code and may take locks. Marked noinline because it must
 * not be inlined into anything: the trampoline calls it by name, and it
 * has to return to the trampoline rather than to a caller that does not
 * exist.
 */
CVM_NOINLINE void cvm_safepoint_stub_park(void) {
    cvm_safepoint_park();
}

/*
 * Where the trampoline should resume: the instruction that faulted.
 *
 * Read after parking, not before, because the value is only meaningful for
 * the thread's own trap and the thread may be resumed by a different
 * collection on another thread's behalf. It is not cleared, so a
 * spurious call would resume somewhere plausible rather than at address
 * zero.
 */
CVM_NOINLINE u64 cvm_safepoint_stub_resume_pc(void) {
    CvmThreadNode* node = cvm_thread_node();
    return (node != NULL) ? node->safepoint_resume_pc : 0;
}

#else /* !CVM_SAFEPOINT_TRAPS */

/*
 * Trap mode is off, so the poll uses the flag and none of the above exists.
 * The collector still calls these, because arming is unconditional — one
 * mechanism or the other has to stop the world — so they are present and
 * do nothing.
 *
 * `cvm_safepoint_trap_usable` is the one that means something: it is how a
 * caller finds out which mechanism is live.
 */

bool cvm_safepoint_trap_usable(void) {
    return false;
}

bool cvm_safepoint_trap_init(void) {
    return false;
}

void cvm_safepoint_trap_attach(CvmThreadNode* node) {
    (void)node;
}

void cvm_safepoint_trap_detach(CvmThreadNode* node) {
    (void)node;
}

void cvm_safepoint_trap_arm_all(void) {}

void cvm_safepoint_trap_disarm_all(void) {}

#endif /* CVM_SAFEPOINT_TRAPS */
