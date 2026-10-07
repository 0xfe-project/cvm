/*
 * rt/pal/posix/trap.c
 *
 * Trap pages and the fault handler, POSIX implementation.
 *
 * See the note in rt.h for what a trap page is for. This file is the
 * platform half: mapping the page, protecting and unprotecting it, and
 * turning the signal into a call back into the collector.
 *
 * ---------------------------------------------------------------------
 * Why the handler does not stop the thread itself
 * ---------------------------------------------------------------------
 *
 * A signal handler may only call async-signal-safe functions, and parking
 * a thread is not one: it takes a mutex and waits on a condition variable.
 * Doing that from the handler is a deadlock waiting to happen, because the
 * thread that holds the mutex may itself be waiting for this thread to
 * stop.
 *
 * So the handler does the smallest possible thing: it records where the
 * faulting instruction was and repoints the saved instruction pointer at a
 * stub. When the kernel returns from the signal, that stub runs on the
 * faulting thread's own stack, in ordinary context, where taking a lock is
 * allowed. The stub calls back into the collector and then jumps to the
 * recorded address, so the faulting load is retried — by which time the
 * page is readable again.
 *
 * ---------------------------------------------------------------------
 * Two signals, one cause
 * ---------------------------------------------------------------------
 *
 * A read of an inaccessible page is a segmentation fault, but on Apple
 * silicon with certain page states it arrives as SIGBUS. Both are handled,
 * and both are only treated as ours when the address matches a trap page;
 * anything else is passed to the previous handler so a genuine fault is
 * still reported as one.
 */

#if !defined(_WIN32)

#define _POSIX_C_SOURCE 200809L

#if defined(__APPLE__)
/* Darwin needs its full namespace for MAP_ANONYMOUS, and ucontext.h wants
 * a POSIX feature level before it will declare mcontext. Both, in this
 * order, is what makes the file compile. */
#define _DARWIN_C_SOURCE 1
#define _XOPEN_SOURCE 700
#endif

#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif

#include "rt.h"

#if defined(__APPLE__)
/* The Mach exception-port reset. Kept in its own file only because
 * <mach/mach_types.h> defines a `thread_t` that collides with nothing here
 * but would collide with a broader header. */
#include <mach/mach.h>
#endif

/* ------------------------------------------------------------------ */
/* trap pages                                                          */
/* ------------------------------------------------------------------ */

/*
 * Released pages are kept rather than unmapped.
 *
 * Generated code may cache the cell's address in a register or spill it
 * across a call — the compiler is entitled to, and does. A stale address
 * to an unmapped page would fault in a way this handler could not tell
 * from a real one, and the process would die for a reason that looks like
 * memory corruption. Keeping the pages costs one page per peak number of
 * threads, and removes the failure mode entirely.
 */
struct RtTrapPage {
    void* cell;

    /*
     * Two links, and they have to be two. A page is on the live list from
     * the moment it exists — the handler needs to recognise its address
     * forever — while being on the free list only between release and
     * reuse. Sharing one field means releasing a page corrupts the live
     * list, and the symptom is not a crash in this file: the handler stops
     * recognising a trap, the fault reaches the default handler, and the
     * process dies with SIGBUS in whatever code happened to be polling.
     */
    struct RtTrapPage* next_free;
    struct RtTrapPage* next_live;
};

static RtTrapPage* trap_free_list = NULL;

/* Defined with the handler, further down; the page constructor needs it. */
static void live_pages_register(RtTrapPage* page);
static RtMutex trap_free_lock;
static bool trap_free_ready = false;

static void trap_free_list_init(void) {
    if (trap_free_ready) {
        return;
    }
    if (rt_mutex_init(&trap_free_lock)) {
        trap_free_ready = true;
    }
}

RtTrapPage* rt_trap_page_create(void) {
    trap_free_list_init();

    if (trap_free_ready) {
        rt_mutex_lock(&trap_free_lock);
        RtTrapPage* reused = trap_free_list;
        if (reused != NULL) {
            trap_free_list = reused->next_free;
        }
        rt_mutex_unlock(&trap_free_lock);

        if (reused != NULL) {
            /* Disarmed already: release disarms before recycling. */
            return reused;
        }
    }

    RtTrapPage* page = rt_mem_alloc(sizeof(RtTrapPage));
    if (page == NULL) {
        return NULL;
    }

    void* cell = mmap(NULL, sizeof(void*), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (cell == MAP_FAILED) {
        rt_mem_free(page);
        return NULL;
    }

    page->cell = cell;
    page->next_free = NULL;
    page->next_live = NULL;

    /*
     * Every page that has ever existed is kept on the live list, because
     * the handler has to be able to tell "a thread read its trap cell"
     * from "the program has a real segmentation fault", and the only way
     * to tell is to compare the fault address against the pages that
     * exist.
     */
    live_pages_register(page);
    return page;
}

void rt_trap_page_arm(RtTrapPage* page) {
    if (page == NULL) {
        return;
    }
    if (mprotect(page->cell, sizeof(void*), PROT_NONE) != 0) {
        rt_fatal(STR_LITERAL("could not arm a safepoint trap page"));
    }
}

void rt_trap_page_disarm(RtTrapPage* page) {
    if (page == NULL) {
        return;
    }
    if (mprotect(page->cell, sizeof(void*), PROT_READ | PROT_WRITE) != 0) {
        rt_fatal(STR_LITERAL("could not disarm a safepoint trap page"));
    }
}

void rt_trap_page_release(RtTrapPage* page) {
    if (page == NULL) {
        return;
    }

    /* Disarm first, so a stale pointer that reaches this page reads
     * successfully rather than faulting on a page whose new owner may have
     * armed it for a different thread. */
    rt_trap_page_disarm(page);

    trap_free_list_init();
    if (!trap_free_ready) {
        return; /* leaked rather than unsafely freed */
    }

    rt_mutex_lock(&trap_free_lock);
    page->next_free = trap_free_list;
    trap_free_list = page;
    rt_mutex_unlock(&trap_free_lock);
}

opaque_mut rt_trap_page_cell(RtTrapPage* page) {
    return (page != NULL) ? page->cell : NULL;
}

/* ------------------------------------------------------------------ */
/* the handler                                                         */
/* ------------------------------------------------------------------ */

static RtTrapFaultFn trap_on_fault = NULL;
static opaque_mut trap_user = NULL;

static struct sigaction previous_sigsegv;
#if defined(__APPLE__)
static struct sigaction previous_sigbus;
#endif

static bool is_our_signal(int signal) {
#if defined(__APPLE__)
    return signal == SIGSEGV || signal == SIGBUS;
#else
    return signal == SIGSEGV;
#endif
}

/* Every live trap page, so a fault address can be matched to one. Pages
 * are never unmapped, so this list only ever grows to the peak thread
 * count. */
static RtTrapPage* live_pages = NULL;
static RtMutex live_pages_lock;
static bool live_pages_ready = false;

static void live_pages_register(RtTrapPage* page) {
    if (!live_pages_ready) {
        if (!rt_mutex_init(&live_pages_lock)) {
            return;
        }
        live_pages_ready = true;
    }
    rt_mutex_lock(&live_pages_lock);
    page->next_live = live_pages;
    live_pages = page;
    rt_mutex_unlock(&live_pages_lock);
}

static bool is_trap_address(void* address) {
    if (!live_pages_ready) {
        return false;
    }

    bool found = false;
    rt_mutex_lock(&live_pages_lock);
    for (RtTrapPage* p = live_pages; p != NULL; p = p->next_live) {
        if (p->cell == address) {
            found = true;
            break;
        }
    }
    rt_mutex_unlock(&live_pages_lock);
    return found;
}

static void trap_handler(int signum, siginfo_t* info, void* context) {
    /*
     * A trap page is never recycled while it is reachable, so a fault on
     * one is always ours. A fault anywhere else is not, and goes to
     * whatever handler was installed before us — including the default,
     * which is what makes a genuine segmentation fault still look like
     * one.
     */
    if (is_our_signal(signum) && info != NULL && is_trap_address(info->si_addr) &&
        trap_on_fault != NULL) {
        if (trap_on_fault(context, (u64)(usize)info->si_addr, trap_user)) {
            return;
        }
    }

    struct sigaction* previous = &previous_sigsegv;
#if defined(__APPLE__)
    if (signum == SIGBUS) {
        previous = &previous_sigbus;
    }
#endif

    if (previous->sa_handler != SIG_DFL && previous->sa_handler != SIG_IGN &&
        previous->sa_handler != SIG_ERR) {
        if ((previous->sa_flags & SA_SIGINFO) != 0) {
            previous->sa_sigaction(signum, info, context);
            return;
        }
        previous->sa_handler(signum);
        return;
    }

    /* Nothing else wants it: restore the default and re-raise, so the
     * process dies with the right signal and a core dump. */
    signal(signum, SIG_DFL);
    raise(signum);
}

bool rt_trap_handler_install(RtTrapFaultFn on_fault, opaque_mut user) {
    trap_on_fault = on_fault;
    trap_user = user;

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = trap_handler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;

    if (sigaction(SIGSEGV, &action, &previous_sigsegv) != 0) {
        return false;
    }

#if defined(__APPLE__)
    if (sigaction(SIGBUS, &action, &previous_sigbus) != 0) {
        return false;
    }
#endif

    return true;
}

/* ------------------------------------------------------------------ */
/* reading and rewriting the faulting context                          */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__)
#if defined(__APPLE__)
#define UC_PC(uc) ((uc)->uc_mcontext->__ss.__rip)
#define UC_SET_PC(uc, value) ((uc)->uc_mcontext->__ss.__rip = (value))
#else
#define UC_PC(uc) ((uc)->uc_mcontext.gregs[REG_RIP])
#define UC_SET_PC(uc, value) ((uc)->uc_mcontext.gregs[REG_RIP] = (value))
#endif
#elif defined(__aarch64__)
#if defined(__APPLE__)
#define UC_PC(uc) ((uc)->uc_mcontext->__ss.__pc)
#define UC_SET_PC(uc, value) ((uc)->uc_mcontext->__ss.__pc = (value))
#else
#define UC_PC(uc) ((uc)->uc_mcontext.pc)
#define UC_SET_PC(uc, value) ((uc)->uc_mcontext.pc = (value))
#endif
#else
#error "trap-based safepoints need a PC accessor for this architecture"
#endif

u64 rt_trap_context_pc(opaque_mut context) {
    ucontext_t* uc = (ucontext_t*)context;
    return (u64)UC_PC(uc);
}

void rt_trap_context_set_pc(opaque_mut context, u64 pc) {
    ucontext_t* uc = (ucontext_t*)context;
    UC_SET_PC(uc, pc);
}

/* ------------------------------------------------------------------ */
/* Apple's Mach exception ports                                        */
/* ------------------------------------------------------------------ */

void rt_trap_reset_platform_exception_ports(void) {
#if defined(__APPLE__)
    /*
     * A task-level Mach handler for EXC_BAD_ACCESS receives the fault
     * before the kernel translates it into SIGBUS, so a thread that faults
     * on a trap page would never reach our signal handler. Debuggers and
     * some frameworks install such handlers, so this is cleared before
     * every arming rather than only at startup.
     */
    (void)task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS,
                                   MACH_PORT_NULL, EXCEPTION_STATE_IDENTITY,
                                   MACHINE_THREAD_STATE);
#endif
}

#endif /* !_WIN32 */
