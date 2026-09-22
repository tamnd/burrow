/* Tests for notes.
 *
 * A gate that a thread sleeps on is easy to test badly. A test that wakes a
 * thread and then checks it woke up passes just as happily against a sleep that
 * returns immediately and never blocks at all, so most of these are arranged to
 * fail if the sleep does not actually wait: the sleeper sets a flag before it
 * goes in and another one after it comes out, and the checker looks at the
 * second flag while it still has to be clear.
 *
 * The other half is the ordering. Wake before sleep has to work, because the
 * waker cannot know whether the sleeper has arrived, and that is the single
 * property that makes a note usable without a lock wrapped round it.
 *
 * Handles are file scope for the reason thread_test.c gives: a running thread
 * reads its function and argument out of its own handle.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/note.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/thread.h"

#include "harness.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------- the easy directions */

/* Filled with something that is not zero before init, because a note has more
 * than one field now and leaving one of them out of init is a bug that hides.
 * Stack memory is usually zero already, so the note works anyway and every test
 * here passes on every machine anybody runs them on. The memory sanitiser found
 * exactly that on the Linux backend, on CI, after the change had been through
 * six platforms clean.
 *
 * This does not catch every version of the mistake, and it is not meant to. A
 * sleeper count that starts at 0xffffffff still works, because every wake sees
 * a count that is not zero and makes the call it would have made anyway. What
 * it does catch is the gate itself, and it says in one line what init's job is,
 * which is the part somebody adding the next field will read. */
static void fill_with_rubbish(burrow__Note *n) {
    unsigned char *p = (unsigned char *)n;

    for (size_t i = 0; i < sizeof(*n); i++)
        p[i] = 0xff;
}

TEST(a_fresh_note_is_closed_and_a_wake_opens_it) {
    burrow__Note n;

    fill_with_rubbish(&n);

    CHECK(burrow__note_init(&n));
    CHECK(!burrow__note_is_open(&n));

    burrow__note_wake(&n);
    CHECK(burrow__note_is_open(&n));

    /* Waking twice is allowed and changes nothing. */
    burrow__note_wake(&n);
    CHECK(burrow__note_is_open(&n));

    burrow__note_clear(&n);
    CHECK(!burrow__note_is_open(&n));

    burrow__note_free(&n);
}

TEST(sleeping_on_a_note_that_is_already_open_returns_at_once) {
    burrow__Note n;

    CHECK(burrow__note_init(&n));
    burrow__note_wake(&n);

    /* One thread, no second thread coming, so if this blocks the test hangs and
     * the timeout is the failure. That is the only signal available here and it
     * is the right one: there is nothing that could unblock it. */
    burrow__note_sleep(&n);
    burrow__note_sleep(&n);
    CHECK(burrow__note_is_open(&n));

    burrow__note_free(&n);
}

/* ------------------------------------------------------------- one sleeper */

static burrow__Note gate;
static uint32_t entered;
static uint32_t left;
static burrow__Thread sleeper;

static void sleep_on_gate(void *arg) {
    (void)arg;
    burrow__atomic_store_release_u32(&entered, 1);
    burrow__note_sleep(&gate);
    burrow__atomic_store_release_u32(&left, 1);
}

TEST(a_sleeper_waits_until_somebody_else_wakes_it) {
    CHECK(burrow__note_init(&gate));
    entered = 0;
    left = 0;

    CHECK(burrow__thread_start(&sleeper, sleep_on_gate, NULL, 0));

    while (burrow__atomic_load_acquire_u32(&entered) == 0)
        burrow__thread_yield();

    /* The thread is in the sleep or about to be, and nothing has opened the
     * gate, so it cannot have come out the other side. This is the check that
     * fails against a sleep which does not sleep. */
    CHECK(burrow__atomic_load_acquire_u32(&left) == 0);

    burrow__note_wake(&gate);
    CHECK(burrow__thread_join(&sleeper));

    CHECK(burrow__atomic_load_acquire_u32(&left) == 1);
    burrow__note_free(&gate);
}

/* --------------------------------------------------------- wake first, race */

static burrow__Note early;
static uint32_t early_done;
static burrow__Thread early_sleeper;

static void sleep_after_the_wake(void *arg) {
    (void)arg;
    burrow__note_sleep(&early);
    burrow__atomic_store_release_u32(&early_done, 1);
}

TEST(a_wake_that_lands_before_the_sleep_is_not_lost) {
    CHECK(burrow__note_init(&early));
    early_done = 0;

    /* Opened before the thread that sleeps on it even exists. A note that only
     * releases threads already queued would hang here forever. */
    burrow__note_wake(&early);

    CHECK(burrow__thread_start(&early_sleeper, sleep_after_the_wake, NULL, 0));
    CHECK(burrow__thread_join(&early_sleeper));

    CHECK(burrow__atomic_load_acquire_u32(&early_done) == 1);
    burrow__note_free(&early);
}

/* ------------------------------------------------------------ many sleepers */

#define SLEEPERS 8

static burrow__Note crowd;
static uint32_t arrived;
static uint32_t released;
static burrow__Thread crowd_threads[SLEEPERS];

static void join_the_crowd(void *arg) {
    (void)arg;
    (void)burrow__atomic_add_u32(&arrived, 1);
    burrow__note_sleep(&crowd);
    (void)burrow__atomic_add_u32(&released, 1);
}

TEST(one_wake_releases_every_sleeper) {
    CHECK(burrow__note_init(&crowd));
    arrived = 0;
    released = 0;

    for (size_t i = 0; i < SLEEPERS; i++)
        CHECK(burrow__thread_start(&crowd_threads[i], join_the_crowd, NULL, 0));

    while (burrow__atomic_load_u32(&arrived) < SLEEPERS)
        burrow__thread_yield();

    CHECK(burrow__atomic_load_u32(&released) == 0);

    /* One wake, eight sleepers. This is what a signal instead of a broadcast
     * gets wrong, and it gets it wrong by hanging rather than by failing. */
    burrow__note_wake(&crowd);

    for (size_t i = 0; i < SLEEPERS; i++)
        CHECK(burrow__thread_join(&crowd_threads[i]));

    CHECK(burrow__atomic_load_u32(&released) == SLEEPERS);
    burrow__note_free(&crowd);
}

/* The same crowd, over and over, which is the test for the sleeper count rather
 * than for the gate. The note keeps a count of the threads that are about to
 * sleep so that a wake with nobody waiting can stay out of the kernel, and a
 * count that drifts downwards is a wake that decides there is nobody to tell
 * while eight threads are asleep. That does not fail, it hangs, so this runs
 * the count up to eight and back to zero sixteen times to give it the chance. */
#define CROWD_ROUNDS 16

TEST(a_crowd_can_go_to_sleep_round_after_round) {
    CHECK(burrow__note_init(&crowd));

    for (uint32_t round = 0; round < CROWD_ROUNDS; round++) {
        arrived = 0;
        released = 0;
        burrow__note_clear(&crowd);

        for (size_t i = 0; i < SLEEPERS; i++)
            CHECK(burrow__thread_start(&crowd_threads[i], join_the_crowd, NULL, 0));

        while (burrow__atomic_load_u32(&arrived) < SLEEPERS)
            burrow__thread_yield();

        burrow__note_wake(&crowd);

        for (size_t i = 0; i < SLEEPERS; i++)
            CHECK(burrow__thread_join(&crowd_threads[i]));

        CHECK(burrow__atomic_load_u32(&released) == SLEEPERS);
    }

    burrow__note_free(&crowd);
}

/* ------------------------------------------------------------ clear and reuse */

#define ROUNDS 16

static burrow__Note reused;
static uint32_t rounds_seen;
static burrow__Thread round_thread;

static void one_round(void *arg) {
    (void)arg;
    burrow__note_sleep(&reused);
    (void)burrow__atomic_add_u32(&rounds_seen, 1);
}

TEST(a_note_can_be_closed_again_and_used_for_the_next_round) {
    CHECK(burrow__note_init(&reused));
    rounds_seen = 0;

    for (uint32_t i = 0; i < ROUNDS; i++) {
        /* Cleared while nothing is asleep on it and nothing is about to be,
         * which is the only time the header says this is safe. */
        burrow__note_clear(&reused);
        CHECK(!burrow__note_is_open(&reused));

        CHECK(burrow__thread_start(&round_thread, one_round, NULL, 0));
        burrow__note_wake(&reused);
        CHECK(burrow__thread_join(&round_thread));

        CHECK(burrow__atomic_load_u32(&rounds_seen) == i + 1);
    }

    burrow__note_free(&reused);
}

/* ---------------------------------------------------------------- ping pong */

/* What the scheduler will actually do with these: two threads handing a turn
 * back and forth, each one asleep while it is not its turn. It is also the
 * shape that finds a lost wakeup, because a single one anywhere in a thousand
 * rounds stops the whole thing dead.
 *
 * The counter is not atomic on purpose. Only one thread touches it at a time,
 * and the note is what makes that true, so a note that does not order its two
 * sides properly shows up here as a data race under the sanitiser rather than
 * as a wrong number. */
#define VOLLEYS 1000

static burrow__Note to_worker;
static burrow__Note to_main;
static uint32_t turn_counter;
static burrow__Thread ponger;

static void pong(void *arg) {
    (void)arg;
    for (int i = 0; i < VOLLEYS; i++) {
        burrow__note_sleep(&to_worker);
        burrow__note_clear(&to_worker);
        turn_counter++;
        burrow__note_wake(&to_main);
    }
}

TEST(two_threads_pass_a_turn_back_and_forth) {
    CHECK(burrow__note_init(&to_worker));
    CHECK(burrow__note_init(&to_main));
    turn_counter = 0;

    CHECK(burrow__thread_start(&ponger, pong, NULL, 0));

    for (int i = 0; i < VOLLEYS; i++) {
        /* Closed before the ball is sent, so the sleep below waits for the
         * reply to this volley rather than seeing the reply to the last one. */
        burrow__note_clear(&to_main);
        burrow__note_wake(&to_worker);

        burrow__note_sleep(&to_main);
        turn_counter++;
    }

    CHECK(burrow__thread_join(&ponger));
    CHECK(turn_counter == VOLLEYS * 2);

    burrow__note_free(&to_main);
    burrow__note_free(&to_worker);
}

/* -------------------------------------------------------- the transient note */

/* A note that lives in a stack frame and is freed the instant the sleep on it
 * returns, which is what a channel does with the waiter of a thread that is not
 * a goroutine.
 *
 * A wake opens the gate first and then has a little more to do, and by then the
 * sleeper is running again and the frame the note was in has been returned to
 * whatever wants it next. On the backend with a mutex in the note, the wake is
 * about to lock one that has been destroyed. That is what
 * burrow__note_init_transient is for, and this is the test that fails without
 * it.
 *
 * It does not fail by giving a wrong answer, which is worth saying because an
 * ordinary run of it passes either way. Run it under a thread sanitiser and the
 * unfixed version reports a race between the free and the wake inside a round or
 * two, and under an address sanitiser with stack use after return turned on it
 * reports that instead. Without a sanitiser it is still a liveness test, because
 * a lost wakeup here hangs. */
#define HANDOFFS 2000

/* Plain rather than atomic, because the two gates below are what order it: it is
 * written before the go gate is opened and read after the sleep on that gate has
 * returned. */
static burrow__Note *handoff_note;
static burrow__Note handoff_go;
static uint32_t handoffs_seen;
static burrow__Thread handoff_thread;

static void handoff_waker(void *arg) {
    (void)arg;

    for (int i = 0; i < HANDOFFS; i++) {
        burrow__note_sleep(&handoff_go);
        /* Closed before the reply goes out, so the next round's wake is not
         * thrown away by a clear that arrives after it. */
        burrow__note_clear(&handoff_go);

        burrow__note_wake(handoff_note);
        (void)burrow__atomic_add_u32(&handoffs_seen, 1);
    }
}

/* One round, in its own frame so that the frame really does go away. */
static bool one_handoff(void) {
    burrow__Note n;

    fill_with_rubbish(&n);

    if (!burrow__note_init_transient(&n))
        return false;

    handoff_note = &n;
    burrow__note_wake(&handoff_go);

    burrow__note_sleep(&n);
    burrow__note_free(&n);
    return true;
}

TEST(a_note_on_a_stack_can_be_freed_the_moment_the_sleep_returns) {
    CHECK(burrow__note_init(&handoff_go));
    burrow__note_clear(&handoff_go);
    handoffs_seen = 0;

    CHECK(burrow__thread_start(&handoff_thread, handoff_waker, NULL, 0));

    bool all = true;
    for (int i = 0; i < HANDOFFS; i++)
        all = one_handoff() && all;

    CHECK(all);
    CHECK(burrow__thread_join(&handoff_thread));
    CHECK(burrow__atomic_load_u32(&handoffs_seen) == HANDOFFS);

    burrow__note_free(&handoff_go);
}

/* ---------------------------------------------------------- the timed sleep */

/* Half a millisecond, which is long enough that the measurement below is not
 * all overhead and short enough that a test which does it a few hundred times
 * still finishes quickly. */
#define SHORT_NS 500000

TEST(a_timed_sleep_on_an_open_note_returns_true_without_waiting) {
    burrow__Note n;

    CHECK(burrow__note_init(&n));
    burrow__note_wake(&n);

    int64_t start = burrow__nanotime();
    CHECK(burrow__note_sleep_timeout(&n, 10000000000LL));

    /* Ten seconds asked for and it had better not have waited any of them. A
     * tenth of a second of slack, which is a loaded machine and not a wait. */
    CHECK(burrow__nanotime() - start < 100000000);

    burrow__note_free(&n);
}

TEST(a_timed_sleep_with_no_time_left_is_a_poll) {
    burrow__Note n;

    CHECK(burrow__note_init(&n));

    /* Zero and negative both mean do not wait, so these answer the note's
     * state and nothing else. */
    CHECK(!burrow__note_sleep_timeout(&n, 0));
    CHECK(!burrow__note_sleep_timeout(&n, -1));

    burrow__note_wake(&n);

    CHECK(burrow__note_sleep_timeout(&n, 0));
    CHECK(burrow__note_sleep_timeout(&n, -1));

    burrow__note_free(&n);
}

TEST(a_timed_sleep_that_nobody_wakes_times_out_and_says_so) {
    burrow__Note n;

    CHECK(burrow__note_init(&n));

    int64_t start = burrow__nanotime();
    CHECK(!burrow__note_sleep_timeout(&n, SHORT_NS));
    int64_t taken = burrow__nanotime() - start;

    /* Early is a bug and late is the operating system. The lower bound is the
     * real check here: a timed sleep that returns straight away passes a test
     * which only looks at the false. */
    CHECK(taken >= SHORT_NS);
    CHECK(!burrow__note_is_open(&n));

    burrow__note_free(&n);
}

#define TIMEOUT_ROUNDS 20

TEST(the_same_short_timeout_used_again_does_not_grow) {
    burrow__Note n;

    CHECK(burrow__note_init(&n));

    int64_t start = burrow__nanotime();

    for (int i = 0; i < TIMEOUT_ROUNDS; i++)
        CHECK(!burrow__note_sleep_timeout(&n, SHORT_NS));

    int64_t taken = burrow__nanotime() - start;

    /* A hundred milliseconds allowed for each half millisecond asked for, which
     * is two hundred times the slack and is deliberate. Windows rounds every
     * wait up to a timer tick, which is about sixteen milliseconds by default,
     * so a bound tight enough to be interesting on Linux fails there for a
     * reason that has nothing to do with this code. What is left after that is
     * still worth having: a backend that took nanoseconds for microseconds, or
     * milliseconds for nanoseconds, is out by a factor of a thousand and lands
     * well outside even this. */
    CHECK(taken >= TIMEOUT_ROUNDS * (int64_t)SHORT_NS);
    CHECK(taken < TIMEOUT_ROUNDS * 100000000LL);

    burrow__note_free(&n);
}

static burrow__Note timed;
static burrow__Thread timed_waker;

static void wake_after_a_moment(void *arg) {
    (void)arg;

    /* Spun rather than slept, because the thing being tested is the only sleep
     * this library has. */
    int64_t start = burrow__nanotime();
    while (burrow__nanotime() - start < SHORT_NS) {
    }

    burrow__note_wake(&timed);
}

TEST(a_wake_that_arrives_before_the_timeout_wins) {
    CHECK(burrow__note_init(&timed));

    CHECK(burrow__thread_start(&timed_waker, wake_after_a_moment, NULL, 0));

    /* Ten seconds against a wake half a millisecond away. Either this comes
     * back true quickly or the wake is being lost, and the elapsed check is
     * what tells those two apart from a test that just sat there. */
    int64_t start = burrow__nanotime();
    bool got = burrow__note_sleep_timeout(&timed, 10000000000LL);
    int64_t taken = burrow__nanotime() - start;

    CHECK(got);
    CHECK(taken < 5000000000LL);

    CHECK(burrow__thread_join(&timed_waker));
    CHECK(burrow__note_is_open(&timed));

    burrow__note_free(&timed);
}

static burrow__Note forever;
static burrow__Thread forever_waker;

static void wake_forever(void *arg) {
    (void)arg;

    int64_t start = burrow__nanotime();
    while (burrow__nanotime() - start < SHORT_NS) {
    }

    burrow__note_wake(&forever);
}

TEST(a_timeout_at_the_end_of_the_clock_is_a_sleep_with_no_end) {
    CHECK(burrow__note_init(&forever));

    CHECK(burrow__thread_start(&forever_waker, wake_forever, NULL, 0));

    /* The largest duration there is, which is what the scheduler asks for when
     * the next timer is set for the end of the clock. Adding it to the current
     * time is signed overflow written the obvious way, so the only thing this
     * test really wants is for the sanitizer builds to have nothing to say
     * about the line that does the adding. The wake is here so that the test
     * finishes. */
    bool got = burrow__note_sleep_timeout(&forever, INT64_MAX);

    CHECK(got);
    CHECK(burrow__thread_join(&forever_waker));

    burrow__note_free(&forever);
}

/* Every sleeper released by one wake, the same as the untimed case, and with
 * timeouts long enough that a lost wake shows up as a failure rather than as a
 * hung test. */
static burrow__Note timed_crowd;
static uint32_t timed_released;
static uint32_t timed_timedout;
static burrow__Thread timed_threads[SLEEPERS];

static void wait_with_a_deadline(void *arg) {
    (void)arg;

    (void)burrow__atomic_add_u32(&arrived, 1);

    if (burrow__note_sleep_timeout(&timed_crowd, 10000000000LL))
        (void)burrow__atomic_add_u32(&timed_released, 1);
    else
        (void)burrow__atomic_add_u32(&timed_timedout, 1);
}

TEST(one_wake_releases_every_timed_sleeper_too) {
    CHECK(burrow__note_init(&timed_crowd));
    arrived = 0;
    timed_released = 0;
    timed_timedout = 0;

    for (size_t i = 0; i < SLEEPERS; i++)
        CHECK(burrow__thread_start(&timed_threads[i], wait_with_a_deadline, NULL, 0));

    while (burrow__atomic_load_u32(&arrived) < SLEEPERS)
        burrow__thread_yield();

    burrow__note_wake(&timed_crowd);

    for (size_t i = 0; i < SLEEPERS; i++)
        CHECK(burrow__thread_join(&timed_threads[i]));

    CHECK(burrow__atomic_load_u32(&timed_released) == SLEEPERS);
    CHECK(burrow__atomic_load_u32(&timed_timedout) == 0);

    burrow__note_free(&timed_crowd);
}

int main(void) {
    RUN(a_fresh_note_is_closed_and_a_wake_opens_it);
    RUN(sleeping_on_a_note_that_is_already_open_returns_at_once);
    RUN(a_sleeper_waits_until_somebody_else_wakes_it);
    RUN(a_wake_that_lands_before_the_sleep_is_not_lost);
    RUN(one_wake_releases_every_sleeper);
    RUN(a_crowd_can_go_to_sleep_round_after_round);
    RUN(a_note_can_be_closed_again_and_used_for_the_next_round);
    RUN(two_threads_pass_a_turn_back_and_forth);
    RUN(a_note_on_a_stack_can_be_freed_the_moment_the_sleep_returns);
    RUN(a_timed_sleep_on_an_open_note_returns_true_without_waiting);
    RUN(a_timed_sleep_with_no_time_left_is_a_poll);
    RUN(a_timed_sleep_that_nobody_wakes_times_out_and_says_so);
    RUN(the_same_short_timeout_used_again_does_not_grow);
    RUN(a_wake_that_arrives_before_the_timeout_wins);
    RUN(a_timeout_at_the_end_of_the_clock_is_a_sleep_with_no_end);
    RUN(one_wake_releases_every_timed_sleeper_too);
    return harness_report("note");
}
