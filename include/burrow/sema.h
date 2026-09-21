/* The sleeping and waking underneath sync, and the one piece of it that is not
 * a lock.
 *
 * A semaphore here is a plain uint32_t that somebody else owns, and this file
 * is the queue of waiters that goes with it. That is the shape Go uses and it
 * is the reason a sync.Mutex is eight bytes: the mutex holds a counter, the
 * waiters live in a table keyed on the counter's address, and a mutex nobody is
 * waiting on has no queue anywhere.
 *
 *     uint32_t sema;              // yours, wherever you like
 *     burrow__sema_acquire(&sema, false);
 *     ...
 *     burrow__sema_release(&sema, false);
 *
 * The counter is the number of wakeups owing. Acquire takes one, waiting if
 * there is none, and release adds one and hands it to somebody waiting. So a
 * counter that starts at zero is a gate that stays shut until somebody opens
 * it, which is what every user of this in sync wants, and a counter that starts
 * at n is a semaphore with n permits, which nothing here wants yet and which
 * works anyway.
 *
 * Goroutines and threads both. A goroutine parks and costs no thread, which is
 * the whole point of the scheduler. A thread that is not running a goroutine
 * has nothing to park, so it sleeps on a note and costs itself. Go can only be
 * in the first situation. burrow is a library inside somebody else's program,
 * so it has to be able to be in both, and the same sync.Mutex has to work
 * either way.
 *
 * Internal. Nothing outside burrow should reach for this: it has no bounds
 * checking, no way to report failure, and the contract is that you never
 * release a semaphore whose waiters you have not thought about. sync.Mutex,
 * sync.WaitGroup and the rest are what this is for.
 *
 * Derived from Go's src/runtime/sema.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SEMA_H
#define BURROW_SEMA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Takes one wakeup from the counter, waiting until there is one to take.
 *
 * `lifo` puts this waiter at the front of the queue for that address rather
 * than the back. Only a mutex in starvation mode asks for it, and it asks
 * because a goroutine that has just been handed the lock and immediately lost
 * it again is the one that has been waiting longest, so putting it at the back
 * a second time is how a queue turns into a lottery.
 *
 * Everything else asks for false and gets first in, first out. */
void burrow__sema_acquire(uint32_t *addr, bool lifo);

/* Adds one wakeup to the counter and gives it to a waiter if there is one.
 *
 * `handoff` asks for the wakeup to be given to that waiter rather than left on
 * the counter for whoever gets there first, and for this caller to give up its
 * turn straight afterwards so the waiter actually runs. That is what makes a
 * starving mutex fair: without it the waiter wakes up, finds the lock already
 * taken by somebody who never queued, and goes back to sleep.
 *
 * Nothing else wants it. A handoff costs a scheduling round trip on every
 * unlock, which is exactly the cost the ordinary path exists to avoid. */
void burrow__sema_release(uint32_t *addr, bool handoff);

/* ---------------------------------------------------------------- spinning
 *
 * The other half of waiting, and the half that is usually right.
 *
 * A mutex is held for a few dozen instructions most of the time, so a waiter
 * that goes to sleep pays a scheduling round trip to avoid a wait that was
 * going to be shorter than the round trip. Spinning a handful of times first is
 * what turns that back into the right trade.
 *
 * `iter` is how many times this caller has already spun, counting from zero.
 * The answer turns to false after four, which is Go's number, and it turns to
 * false earlier when the machine or the scheduler says spinning cannot pay.
 * Then the caller stops asking and goes to sleep. */
bool burrow__sync_can_spin(int32_t iter);

/* One turn of a spin. Tells the processor what this is, so that a core running
 * two hardware threads gives the other one the pipeline, and so that the
 * memory ordering machinery does not have to unwind a speculated load. */
void burrow__sync_do_spin(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SEMA_H */
