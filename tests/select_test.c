/* Tests for select.
 *
 * Split the same way tests/chan_test.c is, and for the same reason. A select
 * that finds a case ready on its first pass never touches the scheduler, so it
 * can be tested as an ordinary function call, and a failure there points at the
 * case list and nothing else. A select that has to wait needs goroutines, and
 * those tests follow the rule tests/sched_test.c sets out: only the main
 * goroutine calls CHECK and everything a child finds out it says through an
 * atomic.
 *
 * Three things here are worth more than the rest.
 *
 * The random choice among ready cases is tested by counting, not by asserting
 * that it is random, which no single run can say. Two thousand rounds with two
 * ready cases should land near a thousand each, and the band below is wide
 * enough that a correct implementation will not trip it in the life of this
 * repository and narrow enough that a select which always picks the first case
 * fails on the first run.
 *
 * Two selects on the same two channels in opposite orders is the test for the
 * lock order. Get that wrong and it deadlocks rather than failing, which is why
 * it runs thousands of rounds: a deadlock that only happens sometimes still
 * happens, and a hung test is a failed test.
 *
 * A select that wakes has entries sitting on every queue it did not win on, and
 * those have to be gone before its stack frame is. The test for that frees the
 * channels afterwards, because chan_free is the thing that notices a waiter
 * that was left behind.
 *
 * The one case that used to be untestable is a send arm on a closed channel.
 * That was a fatal error, and a harness that ends the process has failed, so
 * there was nothing to write. It panics today and there is a test for it below.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/chan.h"

#include "burrow/atomic.h"
#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/mem/track.h"
#include "burrow/proc.h"
#include "burrow/sched.h"
#include "burrow/thread.h"

#include "burrow/panic.h"
#include "burrow/runtime.h"

#include "fatal.h"
#include "harness.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------- without a scheduler */

TEST(a_default_arm_answers_when_nothing_is_ready) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 1);
    CHECK(c != NULL);

    Int got = -1;
    SelectCase cases[] = {
        BURROW_RECV(c, &got),
        BURROW_DEFAULT,
    };

    CHECK_INT_EQ(chan_select(cases, 2), 1);
    CHECK_INT_EQ(got, -1);

    chan_free(c);
}

TEST(a_ready_case_beats_the_default) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 1);
    CHECK(c != NULL);

    Int v = 7;
    chan_send(c, &v);

    Int got = -1;
    bool ok = false;
    SelectCase cases[] = {
        BURROW_DEFAULT,
        BURROW_RECV_OK(c, &got, &ok),
    };

    CHECK_INT_EQ(chan_select(cases, 2), 1);
    CHECK_INT_EQ(got, 7);
    CHECK(ok);
    CHECK_INT_EQ(chan_len(c), 0);

    chan_free(c);
}

/* A send arm on a channel with room is ready, and a send arm on a full one is
 * not. Both halves in one test because the second is only interesting next to
 * the first. */
TEST(a_send_arm_is_ready_while_there_is_room) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 2);
    CHECK(c != NULL);

    Int v = 1;
    SelectCase cases[] = {
        BURROW_SEND(c, &v),
        BURROW_DEFAULT,
    };

    CHECK_INT_EQ(chan_select(cases, 2), 0);
    CHECK_INT_EQ(chan_select(cases, 2), 0);
    CHECK_INT_EQ(chan_len(c), 2);

    /* Full now. */
    CHECK_INT_EQ(chan_select(cases, 2), 1);
    CHECK_INT_EQ(chan_len(c), 2);

    chan_free(c);
}

/* The other direction, where the channel being closed is a mistake rather than
 * an answer. A send arm on a closed channel says the same thing a plain send
 * says, because it is the same mistake and somebody searching for the message
 * should not have to know which of the two they wrote.
 *
 * File static for the reason the header gives: a local live across the setjmp
 * inside CHECK_RUNTIME_ERROR is a local gcc warns about. */
static Chan *shut;
static Int outgoing;

/* Its own function because a braced initialiser has commas in it and a macro
 * argument cannot. */
static void select_send_on_shut(void) {
    SelectCase cases[] = {
        BURROW_SEND(shut, &outgoing),
        BURROW_DEFAULT,
    };

    (void)chan_select(cases, 2);
}

TEST(a_send_arm_on_a_closed_channel_panics) {
    shut = chan_make(heap_allocator(), TYPE_INT, 1);
    CHECK(shut != NULL);
    outgoing = 1;

    chan_close(shut);

    CHECK_RUNTIME_ERROR(select_send_on_shut(), "send on closed channel");

    /* Caught, and the select left nothing of itself on the channel's queues,
     * which is what chan_free notices. */
    chan_free(shut);
    shut = NULL;
}

/* A receive arm on a closed channel is ready at once, hands over whatever is
 * still in the buffer, and then keeps answering with the zero value and a
 * false. That is chan_recv's behaviour and a select arm has to agree with it. */
TEST(a_closed_channel_makes_its_arm_ready_forever) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 2);
    CHECK(c != NULL);

    Int v = 11;
    chan_send(c, &v);
    chan_close(c);

    Int got = -1;
    bool ok = false;
    SelectCase cases[] = {
        BURROW_RECV_OK(c, &got, &ok),
        BURROW_DEFAULT,
    };

    CHECK_INT_EQ(chan_select(cases, 2), 0);
    CHECK_INT_EQ(got, 11);
    CHECK(ok);

    for (int i = 0; i < 3; i++) {
        got = -1;
        ok = true;
        CHECK_INT_EQ(chan_select(cases, 2), 0);
        CHECK_INT_EQ(got, 0);
        CHECK(!ok);
    }

    chan_free(c);
}

/* The idiom for turning an arm off. A NULL channel is never ready, so the arm
 * is there in the list and cannot be chosen, and the case numbering does not
 * shift about as arms come and go. */
TEST(an_arm_on_a_nil_channel_never_fires) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 1);
    CHECK(c != NULL);

    Int v = 3;
    chan_send(c, &v);

    Int got = -1;
    Int unused = -1;
    SelectCase cases[] = {
        BURROW_RECV(NULL, &unused),
        BURROW_SEND(NULL, &v),
        BURROW_RECV(c, &got),
        BURROW_DEFAULT,
    };

    CHECK_INT_EQ(chan_select(cases, 4), 2);
    CHECK_INT_EQ(got, 3);
    CHECK_INT_EQ(unused, -1);

    /* Drained, so now only the default is left. */
    CHECK_INT_EQ(chan_select(cases, 4), 3);

    chan_free(c);
}

/* Every arm nil and a default, which is the shape a select ends up in when
 * every one of its channels has been turned off. It answers the default
 * without touching anything, and it does it without needing a channel to ask
 * for scratch space from, which is the reason the implementation takes this
 * shape first. */
TEST(a_select_with_nothing_but_nil_arms_takes_the_default) {
    Int v = 0;
    SelectCase cases[] = {
        BURROW_RECV(NULL, &v),
        BURROW_RECV(NULL, &v),
        BURROW_DEFAULT,
    };

    CHECK_INT_EQ(chan_select(cases, 3), 2);
}

TEST(a_receive_arm_that_does_not_want_the_value_still_takes_it) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 2);
    CHECK(c != NULL);

    Int a = 1;
    Int b = 2;
    chan_send(c, &a);
    chan_send(c, &b);

    SelectCase cases[] = {
        BURROW_RECV(c, NULL),
        BURROW_DEFAULT,
    };

    CHECK_INT_EQ(chan_select(cases, 2), 0);
    CHECK_INT_EQ(chan_len(c), 1);

    Int got = -1;
    CHECK(chan_recv(c, &got));
    CHECK_INT_EQ(got, 2);

    chan_free(c);
}

/* The same channel in several arms, which Go allows and which the address
 * ordered locking has to survive: it locks each distinct channel once, and a
 * second lock of a channel it already holds would stop the program here. */
TEST(the_same_channel_can_appear_in_several_arms) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 1);
    CHECK(c != NULL);

    Int out = 5;
    Int got = -1;
    SelectCase cases[] = {
        BURROW_RECV(c, &got),
        BURROW_SEND(c, &out),
        BURROW_DEFAULT,
    };

    /* Empty, so only the send can run. */
    CHECK_INT_EQ(chan_select(cases, 3), 1);
    CHECK_INT_EQ(chan_len(c), 1);

    /* Full, so only the receive can. */
    CHECK_INT_EQ(chan_select(cases, 3), 0);
    CHECK_INT_EQ(got, 5);

    chan_free(c);
}

/* --- the random choice
 *
 * Two channels that are always ready, refilled after every round, so the only
 * thing deciding the answer is the shuffle. A select that walks its cases in
 * order would come out two thousand to nothing.
 *
 * The band is three hundred wide either side of a thousand. A fair coin misses
 * that by more than twenty standard deviations, which is to say never, and an
 * unfair one has to be very nearly fair to get inside it. */
#define FAIRNESS_ROUNDS 2000

TEST(the_ready_arm_that_runs_is_chosen_at_random) {
    Alloc *a = heap_allocator();
    Chan *c1 = chan_make(a, TYPE_INT, 1);
    Chan *c2 = chan_make(a, TYPE_INT, 1);
    CHECK(c1 != NULL);
    CHECK(c2 != NULL);

    Int one = 1;
    Int two = 2;
    chan_send(c1, &one);
    chan_send(c2, &two);

    Int got = -1;
    SelectCase cases[] = {
        BURROW_RECV(c1, &got),
        BURROW_RECV(c2, &got),
    };

    int wins[2] = {0, 0};
    bool values_right = true;

    for (int i = 0; i < FAIRNESS_ROUNDS; i++) {
        Int which = chan_select(cases, 2);
        if (which == 0) {
            wins[0]++;
            if (got != 1)
                values_right = false;
            chan_send(c1, &one);
        } else if (which == 1) {
            wins[1]++;
            if (got != 2)
                values_right = false;
            chan_send(c2, &two);
        } else {
            values_right = false;
        }
    }

    CHECK(values_right);
    CHECK_INT_EQ(wins[0] + wins[1], FAIRNESS_ROUNDS);
    CHECK(wins[0] > FAIRNESS_ROUNDS / 2 - 300);
    CHECK(wins[0] < FAIRNESS_ROUNDS / 2 + 300);

    chan_free(c1);
    chan_free(c2);
}

/* --- more arms than fit in the frame
 *
 * A select with up to sixteen arms works out of its own stack frame and one
 * with more makes a single allocation. Forty arms with the ready one buried in
 * the middle exercises the second path, and a tracking allocator underneath
 * says the allocation came back. */
#define MANY 40

TEST(a_select_with_more_arms_than_fit_still_works_and_gives_the_memory_back) {
    Track tr;
    track_init(&tr, heap_allocator());
    track_set_quarantine(&tr, 0);
    Alloc *a = track_allocator(&tr);

    Chan *chans[MANY];
    for (int i = 0; i < MANY; i++) {
        chans[i] = chan_make(a, TYPE_INT, 1);
        CHECK(chans[i] != NULL);
    }

    Int got = -1;
    SelectCase cases[MANY + 1];
    for (int i = 0; i < MANY; i++)
        cases[i] = BURROW_RECV(chans[i], &got);
    cases[MANY] = BURROW_DEFAULT;

    /* Nothing in any of them. */
    CHECK_INT_EQ(chan_select(cases, MANY + 1), MANY);

    /* One value, in an arm that is neither the first nor the last. */
    Int v = 99;
    chan_send(chans[23], &v);
    CHECK_INT_EQ(chan_select(cases, MANY + 1), 23);
    CHECK_INT_EQ(got, 99);

    for (int i = 0; i < MANY; i++)
        chan_free(chans[i]);

    CHECK_INT_EQ((Int)track_live(&tr), 0);
    track_free(&tr);
}

/* --- the same thing with the arms in the worst order
 *
 * The lock order is built by sorting the arms by the address of their channel,
 * so the case that costs the sort the most is a list that is already in the
 * wrong order. This one is in descending address order with a channel listed
 * twice, a nil arm and a default mixed in, which is every kind of arm the sort
 * has to leave out or leave alone in one list.
 *
 * What it can check from one goroutine is that the right arm still wins and the
 * right value still arrives. That a list in this order takes its locks in the
 * same order as a list in any other is what the pair of selects in opposite
 * orders further down is for. */
static int by_address_descending(const void *x, const void *y) {
    Uintptr a = (Uintptr) * (Chan *const *)x;
    Uintptr b = (Uintptr) * (Chan *const *)y;
    if (a > b)
        return -1;
    return a < b ? 1 : 0;
}

TEST(a_wide_select_with_its_arms_in_descending_order_still_works) {
    Chan *chans[MANY];
    for (int i = 0; i < MANY; i++) {
        chans[i] = chan_make(heap_allocator(), TYPE_INT, 1);
        CHECK(chans[i] != NULL);
    }

    qsort(chans, MANY, sizeof chans[0], by_address_descending);

    Int got = -1;
    SelectCase cases[MANY + 3];
    for (int i = 0; i < MANY; i++)
        cases[i] = BURROW_RECV(chans[i], &got);
    cases[MANY] = BURROW_RECV(chans[7], &got); /* the same channel again */
    cases[MANY + 1] = BURROW_RECV(NULL, &got);
    cases[MANY + 2] = BURROW_DEFAULT;

    CHECK_INT_EQ(chan_select(cases, MANY + 3), MANY + 2);

    /* On the channel that is listed twice, so either of the two arms is a right
     * answer and the value says which. */
    Int v = 77;
    chan_send(chans[7], &v);
    Int won = chan_select(cases, MANY + 3);
    CHECK(won == 7 || won == MANY);
    CHECK_INT_EQ(got, 77);

    /* On one that is listed once, where the answer is exact. */
    v = 88;
    chan_send(chans[MANY - 1], &v);
    CHECK_INT_EQ(chan_select(cases, MANY + 3), MANY - 1);
    CHECK_INT_EQ(got, 88);

    for (int i = 0; i < MANY; i++)
        chan_free(chans[i]);
}

/* ------------------------------------------------------- with a scheduler
 *
 * File static shared state, because a goroutine's argument is one pointer and
 * a struct per test would say less than the names do. */

static Chan *c1;
static Chan *c2;
static Chan *fin;
static uint32_t took;
static uint32_t value_ok;
static uint32_t closed_seen;
static uint32_t rounds_done;
static uint32_t child_done;

static void reset(void) {
    c1 = NULL;
    c2 = NULL;
    fin = NULL;
    took = 0;
    value_ok = 0;
    closed_seen = 0;
    rounds_done = 0;
    child_done = 0;
}

/* --- waiting on two channels
 *
 * On one P there is no parallelism to hide behind, so the child can only run
 * if the select parked. A value arriving on the second channel is therefore a
 * statement about the waiting and not just about the two of them meeting. */

static void waits_child(void *arg) {
    (void)arg;
    Int v = 42;
    chan_send(c2, &v);
}

static void waits_body(void *arg) {
    (void)arg;
    c1 = chan_make(heap_allocator(), TYPE_INT, 0);
    c2 = chan_make(heap_allocator(), TYPE_INT, 0);
    if (c1 == NULL || c2 == NULL)
        return;

    if (!go(BURROW_FN(Func, waits_child, NULL)))
        return;

    Int got = -1;
    bool ok = false;
    SelectCase cases[] = {
        BURROW_RECV(c1, &got),
        BURROW_RECV_OK(c2, &got, &ok),
    };

    burrow__atomic_store_u32(&took, (uint32_t)(chan_select(cases, 2) + 1));
    if (got == 42 && ok)
        burrow__atomic_store_u32(&value_ok, 1);
}

TEST(a_select_with_no_default_waits_for_one_of_its_channels) {
    reset();
    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, waits_body, NULL));

    /* Stored one higher so that zero means the select never answered. */
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&took), 2);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&value_ok), 1);

    chan_free(c1);
    chan_free(c2);
    (void)runtime_gomaxprocs(0);
}

/* --- a close wakes it
 *
 * And the arm that did not win has to be off its queue by the time the select
 * returns, which is what the chan_free below is really testing: freeing a
 * channel somebody is still queued on stops the program. */

static void closes_child(void *arg) {
    (void)arg;
    chan_close(c2);
}

static void closes_body(void *arg) {
    (void)arg;
    c1 = chan_make(heap_allocator(), TYPE_INT, 0);
    c2 = chan_make(heap_allocator(), TYPE_INT, 0);
    if (c1 == NULL || c2 == NULL)
        return;

    if (!go(BURROW_FN(Func, closes_child, NULL)))
        return;

    Int got = -1;
    bool ok = true;
    SelectCase cases[] = {
        BURROW_RECV(c1, &got),
        BURROW_RECV_OK(c2, &got, &ok),
    };

    Int which = chan_select(cases, 2);
    if (which == 1 && !ok && got == 0)
        burrow__atomic_store_u32(&closed_seen, 1);
}

TEST(a_close_wakes_a_waiting_select_and_says_so) {
    reset();
    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, closes_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&closed_seen), 1);

    /* Neither channel may still have the select queued on it. chan_free is
     * what notices, by stopping the program, so reaching the next line is the
     * check. */
    chan_free(c1);
    chan_free(c2);
    (void)runtime_gomaxprocs(0);
}

/* --- a send arm that has to wait
 *
 * The mirror of the receive case, and the one that finds a select that only
 * ever queues itself on receive queues. */

static void sendarm_child(void *arg) {
    (void)arg;
    Int got = -1;
    if (chan_recv(c2, &got) && got == 8)
        burrow__atomic_store_u32(&value_ok, 1);
}

static void sendarm_body(void *arg) {
    (void)arg;
    c1 = chan_make(heap_allocator(), TYPE_INT, 0);
    c2 = chan_make(heap_allocator(), TYPE_INT, 0);
    if (c1 == NULL || c2 == NULL)
        return;

    if (!go(BURROW_FN(Func, sendarm_child, NULL)))
        return;

    Int out = 8;
    Int got = -1;
    SelectCase cases[] = {
        BURROW_RECV(c1, &got),
        BURROW_SEND(c2, &out),
    };

    burrow__atomic_store_u32(&took, (uint32_t)(chan_select(cases, 2) + 1));
}

TEST(a_send_arm_waits_for_a_receiver_and_then_hands_the_value_over) {
    reset();
    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, sendarm_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&took), 2);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&value_ok), 1);

    chan_free(c1);
    chan_free(c2);
    (void)runtime_gomaxprocs(0);
}

/* --- two selects on the same two channels, the other way round
 *
 * This is the lock order test and the claim test at once.
 *
 * One goroutine selects on a receive from c1 and a receive from c2, listing
 * them in one order. A second goroutine selects on a send to c2 and a send to
 * c1, listing them in the other. If the locking followed the case order they
 * would take one channel each and wait for the other forever, and if a claim
 * could be taken twice one of the two would come back with a value nobody
 * sent. Thousands of rounds because a deadlock that happens sometimes is still
 * a deadlock, and a hung test is a failed test. */

#ifndef CROSS_ROUNDS
#define CROSS_ROUNDS 5000
#endif

static void cross_child(void *arg) {
    (void)arg;

    for (int i = 0; i < CROSS_ROUNDS; i++) {
        Int out = 1;
        SelectCase cases[] = {
            BURROW_SEND(c2, &out),
            BURROW_SEND(c1, &out),
        };
        (void)chan_select(cases, 2);
    }

    burrow__atomic_store_u32(&child_done, 1);

    /* Says so out loud, because the last round leaves this goroutine runnable
     * rather than run. The value that ends the loop above goes straight to the
     * other one, which then has three instructions left before it returns and
     * the whole runtime comes down, and nothing in between says this one has to
     * be given a turn first. Without this the store above lands after the test
     * has read it, one run in a hundred, and the leftover entry this select
     * still has on the other channel is still there when chan_free looks. */
    Int over = 1;
    chan_send(fin, &over);
}

static void cross_body(void *arg) {
    (void)arg;
    c1 = chan_make(heap_allocator(), TYPE_INT, 0);
    c2 = chan_make(heap_allocator(), TYPE_INT, 0);
    fin = chan_make(heap_allocator(), TYPE_INT, 0);
    if (c1 == NULL || c2 == NULL || fin == NULL)
        return;

    if (!go(BURROW_FN(Func, cross_child, NULL)))
        return;

    uint32_t good = 0;
    for (int i = 0; i < CROSS_ROUNDS; i++) {
        Int got = -1;
        SelectCase cases[] = {
            BURROW_RECV(c1, &got),
            BURROW_RECV(c2, &got),
        };
        (void)chan_select(cases, 2);
        if (got == 1)
            good++;
    }

    burrow__atomic_store_u32(&rounds_done, good);

    Int over = 0;
    (void)chan_recv(fin, &over);
}

TEST(two_selects_listing_the_same_channels_the_other_way_round_agree) {
    reset();
    (void)runtime_gomaxprocs(2);
    runtime_main(BURROW_FN(Func, cross_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&rounds_done), CROSS_ROUNDS);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&child_done), 1);

    chan_free(c1);
    chan_free(c2);
    chan_free(fin);
    (void)runtime_gomaxprocs(0);
}

/* --- everything arrives, whichever arm it comes down
 *
 * Two producers, one on each channel, and a select loop draining both until
 * both have closed. Every value has to arrive exactly once, which the sum
 * checks, and the loop has to notice both closes, which is what turns the arms
 * off one at a time by setting the channel variable to NULL. That idiom is the
 * reason a nil arm never fires. */

#define VALUES 300

static void drain_producer_one(void *arg) {
    (void)arg;
    for (Int i = 1; i <= VALUES; i++)
        chan_send(c1, &i);
    chan_close(c1);
}

static void drain_producer_two(void *arg) {
    (void)arg;
    for (Int i = 1; i <= VALUES; i++)
        chan_send(c2, &i);
    chan_close(c2);
}

static void drain_body(void *arg) {
    (void)arg;
    c1 = chan_make(heap_allocator(), TYPE_INT, 4);
    c2 = chan_make(heap_allocator(), TYPE_INT, 0);
    if (c1 == NULL || c2 == NULL)
        return;

    if (!go(BURROW_FN(Func, drain_producer_one, NULL)))
        return;
    if (!go(BURROW_FN(Func, drain_producer_two, NULL)))
        return;

    Chan *a = c1;
    Chan *b = c2;
    uint32_t total = 0;
    uint32_t count = 0;

    while (a != NULL || b != NULL) {
        Int got = -1;
        bool ok = false;
        SelectCase cases[] = {
            BURROW_RECV_OK(a, &got, &ok),
            BURROW_RECV_OK(b, &got, &ok),
        };

        Int which = chan_select(cases, 2);
        if (ok) {
            total += (uint32_t)got;
            count++;
            continue;
        }

        /* Closed and drained. Turn that arm off and keep the numbering. */
        if (which == 0)
            a = NULL;
        else
            b = NULL;
    }

    burrow__atomic_store_u32(&took, count);
    burrow__atomic_store_u32(&value_ok, total);
}

TEST(a_select_loop_drains_two_producers_and_notices_both_closes) {
    reset();
    (void)runtime_gomaxprocs(2);
    runtime_main(BURROW_FN(Func, drain_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&took), 2 * VALUES);

    /* 1 + 2 + ... + VALUES, twice. */
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&value_ok),
                 (uint32_t)(VALUES * (VALUES + 1)));

    chan_free(c1);
    chan_free(c2);
    (void)runtime_gomaxprocs(0);
}

/* --- from a thread that is not a goroutine
 *
 * burrow is a library inside somebody else's program and that program's own
 * threads are allowed to select. The thread sleeps on a note instead of
 * parking, which costs the thread and is the only difference it can see. */

static void host_thread(void *arg) {
    (void)arg;

    Int got = -1;
    bool ok = false;
    SelectCase cases[] = {
        BURROW_RECV(c1, &got),
        BURROW_RECV_OK(c2, &got, &ok),
    };

    Int which = chan_select(cases, 2);
    if (which == 1 && ok && got == 77)
        burrow__atomic_store_u32(&value_ok, 1);

    burrow__atomic_store_u32(&child_done, 1);
}

static void host_body(void *arg) {
    (void)arg;

    /* Wait for the thread to be blocked in the select before sending, so that
     * the value goes to a select that is genuinely asleep rather than to one
     * that has not started yet. A queue of one on c2 would let the send finish
     * without the thread ever waiting, which is why c2 is unbuffered. */
    Int v = 77;
    chan_send(c2, &v);
}

TEST(a_thread_that_is_not_a_goroutine_can_select) {
    reset();
    (void)runtime_gomaxprocs(2);

    c1 = chan_make(heap_allocator(), TYPE_INT, 0);
    c2 = chan_make(heap_allocator(), TYPE_INT, 0);
    CHECK(c1 != NULL);
    CHECK(c2 != NULL);

    burrow__Thread t;
    CHECK(burrow__thread_start(&t, host_thread, NULL, 0));

    runtime_main(BURROW_FN(Func, host_body, NULL));
    CHECK(burrow__thread_join(&t));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&value_ok), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&child_done), 1);

    chan_free(c1);
    chan_free(c2);
    (void)runtime_gomaxprocs(0);
}

int main(void) {
    RUN(a_default_arm_answers_when_nothing_is_ready);
    RUN(a_ready_case_beats_the_default);
    RUN(a_send_arm_is_ready_while_there_is_room);
    RUN(a_send_arm_on_a_closed_channel_panics);
    RUN(a_closed_channel_makes_its_arm_ready_forever);
    RUN(an_arm_on_a_nil_channel_never_fires);
    RUN(a_select_with_nothing_but_nil_arms_takes_the_default);
    RUN(a_receive_arm_that_does_not_want_the_value_still_takes_it);
    RUN(the_same_channel_can_appear_in_several_arms);
    RUN(the_ready_arm_that_runs_is_chosen_at_random);
    RUN(a_select_with_more_arms_than_fit_still_works_and_gives_the_memory_back);
    RUN(a_wide_select_with_its_arms_in_descending_order_still_works);

    RUN(a_select_with_no_default_waits_for_one_of_its_channels);
    RUN(a_close_wakes_a_waiting_select_and_says_so);
    RUN(a_send_arm_waits_for_a_receiver_and_then_hands_the_value_over);
    RUN(two_selects_listing_the_same_channels_the_other_way_round_agree);
    RUN(a_select_loop_drains_two_producers_and_notices_both_closes);
    RUN(a_thread_that_is_not_a_goroutine_can_select);

    return harness_report("select");
}
