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

#include "burrow/lock.h"

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
 * Everything else asks for false and gets first in, first out.
 *
 * `durable` says that the only thing which can release this semaphore is
 * another goroutine in the same synctest bubble, so a bubble where everybody is
 * waiting like this has stopped. See burrow/synctest.h. Only the caller knows
 * the answer: a WaitGroup can say yes when every Add came from inside the
 * bubble, and a Mutex always says no, because a goroutine waiting for a mutex
 * is waiting for a goroutine that is running. */
void burrow__sema_acquire(uint32_t *addr, bool lifo, bool durable);

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

/* ------------------------------------------------------------ notify lists
 *
 * The other kind of waiting, and the one a sync.Cond is built out of.
 *
 * A semaphore wakes one waiter at a time and has no opinion about which one. A
 * Cond has to wake every waiter that was already waiting when Broadcast was
 * called, and none of the ones that arrive afterwards, and it has to do that
 * while the caller is holding a lock that the waiters themselves need. So it
 * gets a queue of its own rather than a call into the one above.
 *
 * The thing that makes it work is a ticket, taken in two steps:
 *
 *     uint32_t t = burrow__notify_list_add(&l);
 *     sync_locker_unlock(c->l);
 *     burrow__notify_list_wait(&l, t);
 *     sync_locker_lock(c->l);
 *
 * The ticket is taken while the caller still holds its own lock, so it is
 * ordered against whatever the waiter just observed. The wait happens after
 * that lock is dropped, which is the window where a notify can land, and the
 * ticket is what closes it: the list remembers how far it has notified, and a
 * wait whose ticket has already been passed returns immediately instead of
 * sleeping through a wakeup that has been and gone. Without the two steps this
 * is the classic lost wakeup and a Cond built on it would hang.
 *
 * The zero value is an empty list, like everything else here. Internal, for the
 * same reasons the semaphore is.
 *
 * Derived from Go's notifyList in src/runtime/sema.go. */
typedef struct burrow__NotifyList {
    /* The next ticket to hand out. Atomic, and read without the lock, because
     * the fast path of a notify with nobody waiting is a comparison of these
     * two numbers and nothing else. */
    uint32_t wait;

    /* The next ticket to be notified. Everything before it has been. Written
     * under the lock and read atomically outside it. */
    uint32_t notify;

    /* Guards the list below. The runtime's lock rather than a sync.Mutex,
     * because a sync.Mutex parks and this is the thing parking is built on. */
    burrow__Lock lock;

    /* The waiters, oldest first. Tickets increase along it, which is what lets
     * a notify of one ticket stop as soon as it passes that ticket. */
    struct burrow__SemaWaiter *head;
    struct burrow__SemaWaiter *tail;
} burrow__NotifyList;

/* Takes a ticket. Call this while holding whatever lock protects the condition
 * being waited on, then drop that lock, then wait on the ticket. */
uint32_t burrow__notify_list_add(burrow__NotifyList *l);

/* Waits until this ticket is notified, or returns straight away if it already
 * has been. The lock the ticket was taken under must not be held.
 *
 * `durable` has the meaning it has on the semaphore above. Go's answer for a
 * sync.Cond is always yes, without asking which bubble the Cond belongs to,
 * because a Cond cannot be signalled by anything except a Signal or a Broadcast
 * and a bubble where every goroutine is waiting for one of those has nobody
 * left to send it. */
void burrow__notify_list_wait(burrow__NotifyList *l, uint32_t t, bool durable);

/* Wakes the oldest waiter that has not been woken yet, if there is one. */
void burrow__notify_list_notify_one(burrow__NotifyList *l);

/* Wakes everybody who has a ticket so far, and nobody who takes one after. */
void burrow__notify_list_notify_all(burrow__NotifyList *l);

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
