/* How a thread goes to sleep. See burrow/note.h for what a note is and what the
 * rules for using one are.
 *
 * This used to be three files pretending to be one, a futex on Linux, a manual
 * reset event on Windows and a mutex with a condition variable everywhere else.
 * It is one now. pal_futex_wait and pal_futex_wake are the same three things
 * behind one declaration, so what is left here is only the part that was never
 * about any platform: the gate, the counts, and the rules that make a wake that
 * arrives early not get lost.
 *
 * The shape is worth saying once. The open flag is written with a release store
 * and read with an acquire load, so that everything the waker did before it
 * opened the gate is visible to the sleeper once the sleeper sees it open. The
 * sleeper always checks the flag before it waits and again every time it wakes,
 * so a wake that arrives before the sleep is not lost and a wakeup nobody asked
 * for is not mistaken for one. That loop is the whole of the correctness
 * argument.
 *
 * The timed sleep adds one thing to that shape. It works out what is left of
 * the timeout from burrow__nanotime every time round the loop rather than
 * waiting the original amount again, so a sleep that is interrupted nine times
 * still waits the length it was asked for.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/note.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/pal.h"
#include "burrow/thread.h"

#include <stdbool.h>
#include <stdint.h>

/* When a wait of ns nanoseconds starting now runs out.
 *
 * The addition needs the guard because the duration comes from a timer and a
 * timer can be set for the end of the clock. A context made with a deadline of
 * INT64_MAX is the obvious way to get one, and it is a reasonable thing to
 * write: it means this has a deadline in the sense the type system cares about
 * and no deadline in the sense the caller cares about. The scheduler then asks
 * to be woken in INT64_MAX minus now nanoseconds, and now plus that is not a
 * number. Signed overflow is undefined behaviour, so this is not a wrong answer
 * that gets clamped later, it is a program the compiler may do anything with,
 * and the undefined behaviour sanitizer says so.
 *
 * Landing on INT64_MAX is the right answer as well as a defined one. The PAL
 * caps each individual wait anyway, so a deadline at the end of the clock is a
 * loop that goes back to sleep, which is what a wait with no end should look
 * like. */
static int64_t note_deadline(int64_t ns) {
    int64_t now = burrow__nanotime();

    if (ns > INT64_MAX - now)
        return INT64_MAX;
    return now + ns;
}

/* The sleeper count, which is read before anything goes near the kernel.
 *
 * Waking a note used to go into the kernel every single time, on the reasoning
 * that there is no way to know whether anybody is queued without asking, and
 * that asking costs what telling costs. That is true of the kernel and it is not
 * true of a word the sleepers write to on their way past. A wake can read that
 * word, find nobody, and stay in user space, which is the common case by a long
 * way: a scheduler opens gates nobody is standing at all day long. Pinned on an
 * EPYC, a wake and a walk through the gate went from 352 nanoseconds to single
 * digits. Go's runtime does not do this for its own notes, and Go's
 * sync.WaitGroup does exactly this in the word next to its counter.
 *
 * Nothing is lost by doing it, and the argument is worth writing out once
 * because it is the only subtle thing in this file.
 *
 * A sleeper joins the count and then reads the flag. A waker sets the flag and
 * then reads the count. All four of those are sequentially consistent, so they
 * all appear in one order that every thread agrees on, and in any such order the
 * two writes cannot both come after the two reads. Whichever write goes first is
 * seen by the read that follows it. So either the sleeper sees an open gate and
 * does not sleep, or the waker sees a sleeper and calls it, and possibly both.
 * Never neither. This is Dekker's algorithm with the two flags being the gate
 * and the count, and it is the only place in burrow that needs a sequentially
 * consistent ordering rather than an acquire and release pair.
 *
 * The count is a second word rather than spare bits in the flag, which costs
 * four bytes and is worth them. The flag is what a futex compares against, and a
 * futex that finds a different value than the caller expected returns rather
 * than sleeping. Sharing the word would mean every thread arriving at the gate
 * changed the value every thread already asleep was waiting on, so a crowd of
 * sixty four threads would wake each other up for no reason a few thousand
 * times on the way in. A flag that only ever holds nought or one cannot do
 * that. */

/* The waker count, which is the other half of the same idea and is there for a
 * different reason.
 *
 * Read the argument above again and look at what it forces. The waker has to set
 * the flag before it reads the count, or the two writes could both come after
 * the two reads and a sleeper would be missed. So the flag is set first, and the
 * flag is what releases the sleeper, which means every line of a wake after that
 * store is running against a sleeper that is already awake and already gone. If
 * the note was a local in the frame that sleeper just returned from, the wake is
 * reading memory that is not there any more.
 *
 * That is not theoretical and it is not rare. A channel puts its waiter on the
 * stack of whoever is blocking, the way Go's sudog does, and the last thing the
 * blocking side does is free the note and return. A thread sanitiser finds it in
 * seconds.
 *
 * So a wake can join a count on the way in and leave it on the way out, and a
 * free waits for that count to reach nought before it touches anything. The join
 * is before the flag is set, so a free that sees nought can be sure no wake is
 * holding the note and none is about to, because any wake that had not joined
 * yet had not set the flag yet either, and a sleeper that has returned was
 * released by a flag somebody had already set.
 *
 * Can rather than does, because it is not free. Two atomic additions on an
 * otherwise untouched cache line took a wake and a walk through the gate from
 * eleven nanoseconds to twenty three on an EPYC, which is not a price a
 * scheduler should pay on every park. So a note says at init time whether it is
 * the kind that can be freed out from under a wake, and only those count. The
 * flag is read before the gate is opened, which is the last moment the note is
 * certainly still there.
 *
 * One thing got easier when this moved onto the PAL and it is worth recording.
 * The window this count protects used to end in a call that touched the note's
 * own mutex, and destroying a mutex somebody is about to lock is a crash rather
 * than a wrong answer. pal_futex_wake compares the address and never reads
 * through it, so the worst a late wake can now do is walk a list that does not
 * name it. The count still earns its keep, because the store to the flag above
 * it is a write to the caller's memory either way, but the sharp edge is gone
 * on every platform rather than only on Linux. */

/* Waits until no wake is holding the note. Every free starts with this, and on a
 * note that is not transient it is one load that always finds nought. */
static void note_drain_wakers(burrow__Note *n) {
    for (int spins = 0; burrow__atomic_load_acquire_u32(&n->wakers) != 0; spins++) {
        /* Spin first, because the thread being waited for is running and has a
         * handful of instructions left. Yield after that, because on a machine
         * with one core or an oversubscribed one it is holding a timeslice this
         * thread could give it. */
        if (spins < 64)
            burrow__atomic_spin_hint();
        else
            burrow__thread_yield();
    }
}

bool burrow__note_init(burrow__Note *n) {
    n->state = 0;
    n->waiters = 0;
    n->wakers = 0;
    n->transient = false;
    return true;
}

bool burrow__note_init_transient(burrow__Note *n) {
    if (!burrow__note_init(n))
        return false;

    /* Written before the note is shared with anybody, and never written again,
     * so a wake can read it without an atomic. */
    n->transient = true;
    return true;
}

void burrow__note_free(burrow__Note *n) {
    /* Nothing was ever allocated, but a wake that has already released its
     * sleeper may still be reading these words. See the top of the file. */
    note_drain_wakers(n);

    /* The stores are here so that a use after free finds a closed gate with
     * nobody at it and hangs where a debugger can see it, rather than finding
     * whatever the memory is reused for and carrying on. */
    n->state = 0;
    n->waiters = 0;
}

void burrow__note_clear(burrow__Note *n) {
    burrow__atomic_store_u32(&n->state, 0);
}

void burrow__note_wake(burrow__Note *n) {
    /* Joined before the flag is set, so that a free cannot run to completion
     * underneath the lines below. See the top of the file. */
    const bool pin = n->transient;
    if (pin)
        (void)burrow__atomic_add_u32(&n->wakers, 1);

    burrow__atomic_store_u32(&n->state, 1);

    /* Only if somebody said they were going to sleep. See the top of the file
     * for why reading the count here cannot miss a sleeper on its way in.
     *
     * Everybody, because a note releases everybody. Waking one would leave every
     * sleeper but one waiting for a second wake that is never coming. */
    if (burrow__atomic_load_u32(&n->waiters) != 0)
        (void)pal_futex_wake(&n->state, INT64_MAX, NULL);

    if (pin)
        (void)burrow__atomic_add_u32(&n->wakers, 0U - 1U);
}

void burrow__note_sleep(burrow__Note *n) {
    if (burrow__atomic_load_acquire_u32(&n->state) != 0)
        return;

    (void)burrow__atomic_add_u32(&n->waiters, 1);

    while (burrow__atomic_load_u32(&n->state) == 0) {
        /* The race between the load above and this call is what the expected
         * value argument is for: the wait rechecks the word under whatever lock
         * the platform uses and returns straight away if the waker got there in
         * between. A negative timeout is a wait with no end. */
        (void)pal_futex_wait(&n->state, 0, -1, NULL);
    }

    (void)burrow__atomic_add_u32(&n->waiters, 0U - 1U);
}

bool burrow__note_sleep_timeout(burrow__Note *n, int64_t ns) {
    /* The note is read before the clock is. The caller here is a thread that
     * has just failed to find work and is parking with a deadline on it, and by
     * then the wake it was racing has usually already landed, so the common
     * case returns without ever asking what time it is. Asking first put a vdso
     * call in front of every one of those, which is what the note_timeout_hit
     * row in burrow-bench is there to catch. */
    if (burrow__atomic_load_acquire_u32(&n->state) != 0)
        return true;

    if (ns <= 0)
        return false;

    int64_t deadline = note_deadline(ns);

    (void)burrow__atomic_add_u32(&n->waiters, 1);

    while (burrow__atomic_load_u32(&n->state) == 0) {
        int64_t left = deadline - burrow__nanotime();
        if (left <= 0)
            break;

        (void)pal_futex_wait(&n->state, 0, left, NULL);
    }

    (void)burrow__atomic_add_u32(&n->waiters, 0U - 1U);

    /* Read again rather than returning what the loop decided, because the wake
     * can land between the last check and the deadline passing, and a note that
     * is open is open however late the thread noticed. */
    return burrow__atomic_load_acquire_u32(&n->state) != 0;
}

bool burrow__note_is_open(const burrow__Note *n) {
    return burrow__atomic_load_acquire_u32(&n->state) != 0;
}
