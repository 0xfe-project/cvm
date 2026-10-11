/*
 * rt/base/spin.h
 *
 * How a thread that is waiting for another thread to finish something
 * should wait.
 *
 * Transcribed from refs/whippet/src/spin.h, which is 18 lines and whose
 * comment gives its own reason. The three tiers, and what each is for:
 *
 *   the first 10 tries   the architecture's spin hint. Not a syscall, not
 *                         even an instruction the pipeline treats as work:
 *                         "I am waiting, give the sibling thread the
 *                         resources." A worker finishing a mark batch takes
 *                         tens of microseconds, so most waits end here.
 *   the next 10          sched_yield. The wait has outlasted a spin, so ask
 *                         the scheduler to run whoever is being waited for.
 *   after that           sleep, in growing slices: none at first, then 2us,
 *                         4us, 6us and so on, doubling in effect every power
 *                         of two of the spin count. A wait this long is a
 *                         stall, and polling it hot helps nobody.
 *
 * The growth is __builtin_clz arithmetic rather than a table, and it is the
 * one line here that looks clever: clz(20) - clz(n) is the index of the
 * highest bit n has above 20's, so the sleep doubles each time the count
 * crosses a power of two. Keep it as the reference has it.
 *
 * whippet's version calls __builtin_ia32_pause, which is x86's hint; ours is
 * rt_cpu_relax(), which is the same hint per architecture (yield on arm,
 * pause on x86) and already exists for exactly this purpose.
 */

#ifndef RT_BASE_SPIN_H
#define RT_BASE_SPIN_H

#include "base/types.h"
#include "rt.h"

/*
 * Back off for the `spin`'th consecutive failed attempt, counting from 0 at
 * the caller. The count is the caller's to keep: this is a pure function of
 * it, so a caller that resets its counter on success gets the full sequence
 * again next time.
 */
CVM_INLINE void rt_spin_backoff(u64 spin) {
    if (spin < 10) {
        rt_cpu_relax();
    } else if (spin < 20) {
        rt_thread_yield();
    } else {
        rt_thread_sleep_us((u32)((__builtin_clzll(20) - __builtin_clzll(spin)) << 1));
    }
}

#endif /* RT_BASE_SPIN_H */
