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
#include "burrow/thread.h"

#include "harness.h"

#include <stdint.h>

/* ------------------------------------------------------- the easy directions */

TEST(a_fresh_note_is_closed_and_a_wake_opens_it) {
    burrow__Note n;

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

int main(void) {
    RUN(a_fresh_note_is_closed_and_a_wake_opens_it);
    RUN(sleeping_on_a_note_that_is_already_open_returns_at_once);
    RUN(a_sleeper_waits_until_somebody_else_wakes_it);
    RUN(a_wake_that_lands_before_the_sleep_is_not_lost);
    RUN(one_wake_releases_every_sleeper);
    RUN(a_note_can_be_closed_again_and_used_for_the_next_round);
    RUN(two_threads_pass_a_turn_back_and_forth);
    return harness_report("note");
}
