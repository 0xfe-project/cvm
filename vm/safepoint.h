/*
 * vm/safepoint.h
 *
 * Stop-the-world coordination, replacing the polling half of
 * refs/commix/Synchronizer.c.
 *
 * ---------------------------------------------------------------------
 * The protocol
 * ---------------------------------------------------------------------
 *
 * Generated code calls cvm_safepoint_poll() at function entries and loop
 * back edges. The poll is an atomic load and a branch; when the flag is
 * clear it costs nothing else.
 *
 * To collect, a thread calls cvm_safepoint_acquire(), which:
 *
 *   1. takes the stop flag, or waits for the thread that already holds it
 *      and returns false, meaning "someone else just collected, retry";
 *   2. waits until every other registered thread is parked, skipping any
 *      thread that is unmanaged (inside a foreign call, so it cannot be
 *      touching the heap) or dead;
 *   3. returns true, and the caller owns the world until it calls
 *      cvm_safepoint_release().
 *
 * A thread that observes the flag parks: it records that it is parked,
 * wakes the collector, and waits for the flag to clear.
 *
 * ---------------------------------------------------------------------
 * What this does not do yet
 * ---------------------------------------------------------------------
 *
 * No trap mode. The reference has a second mechanism where each thread
 * owns a page that the collector protects, so a safepoint poll is a load
 * that faults rather than a load that branches. It removes the need for
 * the frontend to insert polls in the right places, at the cost of signal
 * handling on every platform plus a per-thread page. That is a later
 * step; this file is the portable baseline the trap mode would replace
 * the poll body of, not the whole design.
 *
 * No deadlock recovery. A thread that never reaches a safepoint is
 * reported (see cvm_safepoint_stuck_reports) and then waited for
 * indefinitely, because the alternatives are worse: proceeding would mean
 * scanning a heap that is being mutated.
 */

#ifndef CVM_SAFEPOINT_H
#define CVM_SAFEPOINT_H

#include "base/types.h"

/* One-time setup. Safe to call more than once. */
void cvm_safepoint_init(void);

/*
 * Take the stop-the-world and wait for every other thread to park.
 *
 * Returns true if this thread now owns the collection, false if another
 * thread was already collecting — in which case the heap has just been
 * collected, so the caller's move is to retry what it was doing.
 */
bool cvm_safepoint_acquire(void);

/* Give the world back. Wakes every parked thread. */
void cvm_safepoint_release(void);

bool cvm_safepoint_in_progress(void);

/* Out-of-line slow path of the poll. Called by cvm_safepoint_poll(). */
void cvm_safepoint_park(void);

/* Whether the calling thread is parked right now. */
bool cvm_safepoint_is_parked(void);

/*
 * Wake a collector that may be waiting for a thread which is going away.
 *
 * Called when the registry changes. Without it a collector that had
 * already noticed a thread would sit out its full timeout before
 * re-checking, so a thread exiting during a collection would stall it for
 * as long as the diagnostic interval.
 */
void cvm_safepoint_notify_thread_changed(void);

/*
 * How many times the collector gave up waiting and reported which threads
 * were not parked. Non-zero means a thread is not reaching safepoints:
 * either it is stuck in foreign code without having entered unmanaged
 * state, or the generated code is missing a poll.
 */
u32 cvm_safepoint_stuck_reports(void);

/*
 * How many threads are currently waiting for another collector to finish.
 *
 * A diagnostic rather than a mechanism: a value that stays above zero
 * means collections are being serialised, and each waiter is a mutator
 * that is stalled. Also what a test uses to know, without guessing at
 * timings, that a thread has actually entered the wait.
 */
u32 cvm_safepoint_acquire_waiters(void);

#endif /* CVM_SAFEPOINT_H */
