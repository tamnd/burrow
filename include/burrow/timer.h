/* Timers, which are what a runtime uses to make something happen later.
 *
 * This is Go's runtime/time.go, which is the layer underneath time.Sleep,
 * time.After, time.AfterFunc, time.Ticker, context deadlines and every network
 * read timeout. None of those exist yet. What exists here is the thing they are
 * all built out of: a set of timers per P, ordered so that the earliest one is
 * cheap to find, and a call the scheduler makes to run whatever is due.
 *
 * Per P rather than one global set, because the alternative is every goroutine
 * that sets a deadline taking one lock. Go started with a single heap under a
 * single lock and moved to this in 1.9 for exactly that reason, and a program
 * that does nothing but set and clear deadlines is a normal kind of server.
 *
 * The heap has four children per node rather than two. It is still a heap and
 * every operation is still logarithmic, but a four way one is a third shallower,
 * which means a third fewer comparisons on the way down and a third fewer cache
 * misses, and the four entries a node compares against sit next to each other in
 * one or two cache lines. Go's number, and Go's reason.
 *
 * The part that is not obvious is that a timer cannot be taken out of the heap
 * by whoever stops it. Stopping happens on whatever thread the program was on,
 * and the heap belongs to a P that some other thread may be holding, so a stop
 * marks the timer and walks away. That is what the three state bits below are
 * for, and it is why a timer that is stopped and started again before the owning
 * P gets round to it never leaves the heap at all. That case is a read deadline
 * on a busy connection, and it is why this shape is worth its complexity.
 *
 * A set does not have to belong to a P. A synctest bubble has one of its own, so
 * that a timer armed inside the bubble runs on the bubble's clock rather than on
 * the machine's, and `fake` below is the one thing that set does differently.
 * burrow/synctest.h is what that is for.
 *
 * What Go has here that this does not, yet. Timer channels, which need channels,
 * and the sequence numbers that go with them. And the netpoller wakeup, which is
 * how Go tells a sleeping thread that its deadline moved. A thread with nothing
 * to do here is asleep on its own note with a deadline on it, and
 * burrow__timers_wake below is what cuts that short.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TIMER_H
#define BURROW_TIMER_H

#include "burrow/atomic.h"
#include "burrow/lock.h"
#include "burrow/own.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct burrow__Timer burrow__Timer;
typedef struct burrow__Timers burrow__Timers;

/* What a timer does when it is due.
 *
 * `delay` is how late it is, in nanoseconds: the reading the scheduler took
 * minus the time the timer asked for. Normally it is a few microseconds and
 * nobody cares, and the reason it is passed at all is that the things built on
 * top of this do care. A ticker that is fed lazily can be arbitrarily late, and
 * package time subtracts the delay back out so that the value it delivers says
 * when the tick was due rather than when somebody got round to it.
 *
 * This runs on the scheduler's own stack with no P's timer lock held, and it
 * has to be quick. Readying a goroutine is the intended amount of work. Doing
 * anything that blocks is not: the thread running it is a thread that is not
 * running goroutines, and every other timer due at the same moment is waiting
 * behind it. */
typedef void (*burrow__TimerFn)(void *arg, int64_t delay);

/* In some P's heap. t->ts is that P's set and is only meaningful with this set. */
#define BURROW__TIMER_HEAPED 1U

/* t->when has moved since the heap recorded it. Only ever set together with
 * HEAPED, and cleared by the P that owns the heap when it puts the timer back
 * where the new time says it belongs. */
#define BURROW__TIMER_MODIFIED 2U

/* Stopped, but still in a heap that belongs to somebody else. Only ever set
 * together with HEAPED. The owning P drops it on its next pass. */
#define BURROW__TIMER_ZOMBIE 4U

/* One timer.
 *
 * Owned by one goroutine at a time as far as starting and stopping goes, and
 * touched concurrently by the P whose heap it is in, which is what `mu` is for.
 * `astate` is a copy of the state bits published every time the lock is
 * released, so that the P walking its heap can decide whether a timer needs
 * attention without taking a lock per timer. That read can be stale, and every
 * place that acts on it takes the lock and looks again. */
struct burrow__Timer {
    /* Covers every field below except `f` and `arg`, which are written by
     * burrow__timer_init before anybody else can see the timer. */
    burrow__Lock mu;

    /* The state bits as of the last unlock, readable without the lock. */
    uint32_t astate;

    /* The state bits. Under mu. */
    uint32_t state;

    /* When this is due, as a burrow__nanotime reading, and how often it repeats.
     * A period of zero is a timer that fires once. Zero `when` means disarmed.
     * Under mu. */
    int64_t when;
    int64_t period;

    /* What to run and what to hand it. Set by burrow__timer_init and by a reset
     * that supplies new ones. */
    burrow__TimerFn f;
    void *arg;

    /* The heap this is in, or NULL. Borrowed: a P owns its own timer set. Under
     * mu, and only meaningful while HEAPED is set. */
    BURROW_BORROWS(1) burrow__Timers *ts;
};

/* A timer and a copy of its `when`, which is what the heap is made of.
 *
 * The copy is not redundant. Reading t->when needs the timer's lock, and a sift
 * down compares a few dozen of them, so a heap of bare pointers would take and
 * release a lock per comparison. The copy is under the heap's lock instead, and
 * the MODIFIED bit is what says the copy has fallen behind. */
typedef struct burrow__TimerWhen {
    BURROW_BORROWS(1) burrow__Timer *t;
    int64_t when;
} burrow__TimerWhen;

/* The set of timers belonging to one P.
 *
 * Locked rather than owned outright, because a thread that has found no work of
 * its own goes looking at other Ps' timers before it parks, the same way it goes
 * looking at their run queues. */
struct burrow__Timers {
    burrow__Lock mu;

    /* Set for the one set that belongs to a synctest bubble rather than to a P.
     *
     * Only one thing turns on it. A timer added to a P's heap has to be able to
     * cut short a thread that is asleep waiting for a later one, and that is
     * burrow__timers_wake. A bubble's timers are run by the goroutine sitting in
     * synctest_run and by nobody else, so there is no sleeping thread to cut
     * short and the wake would be a thread woken for nothing.
     *
     * Written once, before the set is reachable from anywhere else. */
    bool fake;

    /* The heap. Under mu. */
    BURROW_OWNS(1) burrow__TimerWhen *heap;
    uint32_t len;
    uint32_t cap;

    /* A copy of `len` published every time the lock is released, for the threads
     * that want to know whether this set is worth a trip through the lock.
     * Published at unlock rather than at every change, so that a pass which
     * takes the only timer out and puts it back reads as 1 throughout rather
     * than dipping through 0. */
    uint32_t alen;

    /* How many timers in the heap are stopped and waiting to be thrown out.
     * Atomic. A set with more zombies than live timers gets cleaned out on the
     * owning P's next pass. */
    uint32_t zombies;

    /* When the earliest timer in the heap is due, or 0 for none. Atomic, and it
     * is what makes asking another P whether it has anything due a single load.
     * This is heap[0].when, so it goes stale in the same way a heap entry does,
     * and the field below is the correction. */
    uint64_t min_when_heap;

    /* A lower bound on `when` across the timers that have the MODIFIED bit set,
     * or 0 when there are none. Atomic.
     *
     * Two fields rather than one because a timer can be moved earlier by a
     * thread that does not hold this lock, and that thread has to be able to
     * publish the new time without waiting for the lock. So it lowers this one
     * and leaves the heap alone. Anybody asking when the next timer is due takes
     * the smaller of the two, which is never later than the truth, which is the
     * direction that is safe to be wrong in. */
    uint64_t min_when_modified;
};

/* Prepares a timer that has never been used. `f` and `arg` may be NULL and be
 * supplied later by burrow__timer_reset. */
void burrow__timer_init(burrow__Timer *t, burrow__TimerFn f, void *arg);

/* Arms a timer, or moves one that is already armed.
 *
 * `when` is a burrow__nanotime reading and has to be positive. `period` is zero
 * for a timer that fires once and the gap between firings for one that repeats.
 * `f` and `arg` replace what the timer was going to run, unless `f` is NULL, in
 * which case both are left alone.
 *
 * `pending`, when it is not NULL, is set to whether the timer was armed and had
 * not yet fired, which is what time.Timer.Reset reports.
 *
 * Answers false only when the P's heap needed to grow and the allocator said no,
 * in which case the timer is not armed and will never fire. Go cannot fail here
 * because Go's heap grows by panicking if it cannot. burrow does not have that
 * option and does not pretend to: the caller gets told. */
bool burrow__timer_reset(burrow__Timer *t, int64_t when, int64_t period,
                         burrow__TimerFn f, void *arg, bool *pending);

/* The same, into a set the caller names rather than the calling thread's.
 *
 * Only matters for a timer that is not in a heap yet, since one that is stays
 * where it is. What wants this is the tests, which drive a set with no scheduler
 * anywhere near it, and code that has a particular set in hand. */
bool burrow__timer_reset_on(burrow__Timers *ts, burrow__Timer *t, int64_t when,
                            int64_t period, burrow__TimerFn f, void *arg,
                            bool *pending);

/* Disarms a timer. Answers whether it was armed and had not yet fired, which is
 * what time.Timer.Stop reports.
 *
 * Does not take the timer out of any heap, because the heap may belong to a P
 * another thread is holding. It marks it, and the owner throws it out later. A
 * stopped timer can be armed again with burrow__timer_reset and is cheaper to
 * arm while it is still in the heap. */
bool burrow__timer_stop(burrow__Timer *t);

/* Disarms a timer and takes it out of whatever heap it is in, so that the memory
 * underneath it can be given back.
 *
 * This is the one thing Go does not need and burrow does. Go's timer stays alive
 * as long as a heap points at it and the collector deals with the rest, so a
 * stop can be a mark and nothing more. Here the memory belongs to somebody, and
 * a free that leaves the pointer in a P's heap is a use after free the next time
 * that P walks it.
 *
 * Costs a walk of the heap the timer is in, because the heap entries carry no
 * back index and paying for one on every sift would slow the hot path down to
 * make this rare call faster. A timer that has already fired, or that was never
 * armed, is not in a heap and costs nothing.
 *
 * The timer is left the way burrow__timer_init leaves one, so it can be armed
 * again rather than freed if that is what the caller wants. */
void burrow__timer_drop(burrow__Timer *t);

/* Prepares an empty set. A zeroed burrow__Timers is already a valid empty one,
 * so this exists for symmetry with the free and for the day it needs to do
 * something. */
void burrow__timers_init(burrow__Timers *ts);

/* Gives the heap array back. Every timer still in it is marked as being in no
 * heap, so a timer that outlives its P is disarmed rather than dangling. */
void burrow__timers_free(burrow__Timers *ts);

/* When the earliest timer in this set is due, or 0 if there is nothing in it.
 *
 * Takes no lock, so the answer can be earlier than the truth and never later.
 * That is the direction the scheduler can absorb: a thread that wakes for a
 * timer which turns out not to be due yet works out that there is nothing to do
 * and goes back to sleep, and a thread that does not wake at all sleeps through
 * the deadline. */
int64_t burrow__timers_wake_time(burrow__Timers *ts);

/* Whether burrow__timers_wake_time would say anything other than zero, inline,
 * because the scheduler asks on every pass and a P with no timers is the
 * common case. The loads are in the same order and for the same reason. */
static inline bool burrow__timers_any(burrow__Timers *ts) {
    return burrow__atomic_load_u64(&ts->min_when_modified) != 0 ||
           burrow__atomic_load_u64(&ts->min_when_heap) != 0;
}

/* Roughly how many timers are in the set, from the copy published at the last
 * unlock. For deciding whether a set is worth looking at. */
uint32_t burrow__timers_len(burrow__Timers *ts);

/* Runs everything in `ts` that is due, and tidies up what has been stopped.
 *
 * `now` is a burrow__nanotime reading, or 0 to take one. The reading actually
 * used is the answer, so that a caller walking several Ps takes one reading and
 * passes it along rather than asking the clock once per P.
 *
 * `next`, when it is not NULL, is set to when the earliest remaining timer is
 * due, or 0 if there is none. It is always later than the reading that was used.
 * `ran`, when it is not NULL, is set to whether anything actually ran, which
 * tells the caller that a goroutine may have become runnable.
 *
 * Runs the timer functions with no lock held. Call it on the scheduler's own
 * stack, which in practice means from the scheduler. */
int64_t burrow__timers_check(burrow__Timers *ts, int64_t now, int64_t *next, bool *ran);

/* Moves every timer in `src` into `ts` and leaves `src` empty, for a P going out
 * of service. Answers false if `ts` could not grow to hold them, in which case
 * nothing has moved.
 *
 * Neither set may be in use by another thread. Go calls this with the world
 * stopped and so does this. */
bool burrow__timers_take(burrow__Timers *ts, burrow__Timers *src);

/* Says that a timer is now due earlier than whoever is asleep was told.
 *
 * Supplied by the scheduler, called from here. A thread with nothing to run is
 * asleep with a deadline worked out from the timers that existed when it went
 * to sleep, and a timer added or moved after that has to cut the sleep short.
 * That thread may be asleep on a note or inside the netpoller, and `when` is
 * the new timer's firing time so that the scheduler can tell whether it beats
 * the deadline the poll went in with. */
void burrow__timers_wake(int64_t when);

/* The calling thread's own set, or NULL on a thread that is not running a
 * goroutine.
 *
 * Supplied by the scheduler, called from here. A timer that is being armed for
 * the first time goes into the set of whichever P the goroutine arming it is
 * running on, which is Go's rule and is what keeps a program that sets a
 * deadline per request off a single shared lock. */
BURROW_STATIC(ret) burrow__Timers *burrow__timers_local(void);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TIMER_H */
