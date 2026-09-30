/* Tests for the timers that send on a channel: NewTimer, After, NewTicker and
 * Tick, and Stop and Reset on them.
 *
 * Most of these run in a synctest bubble, where the clock only moves when every
 * goroutine in the bubble is blocked and then jumps straight to the next timer.
 * That makes the times exact. A timer asked for three seconds from now sends a
 * value that is three seconds after now to the nanosecond, and the test can say
 * so without any slack for a busy machine.
 *
 * The last two run on the real clock with several threads, because the thing
 * they check is the race Go 1.23 closed: a timer that has been picked to fire,
 * and a Stop or Reset on another thread that lands before the send does. A
 * bubble runs its timers on one goroutine and never has that race.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/time.h"

#include "burrow/chan.h"
#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/sched.h"
#include "burrow/synctest.h"
#include "burrow/testing.h"

#include "check.h"
#include "fatal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Runs body in a bubble with t as its argument. */
static void in_bubble(TestingT *t, void (*body)(void *)) {
    if (!synctest_run(BURROW_FN(Func, body, t)))
        testing_t_fatalf_v(t, "synctest_run could not start the bubble");
}

/* A receive that should not find anything, and says so if it does. */
static bool nothing_waiting(Chan *c) {
    Time v;
    return !chan_try_recv(c, &v, NULL);
}

/* ------------------------------------------------------------------ timers */

static void fires_on_time(void *env) {
    TestingT *t = env;
    Alloc *a = heap_allocator();
    Time start = time_now();

    TimeTimer *tm = time_new_timer(a, 3 * TIME_SECOND);
    CHECK(tm != NULL);
    Chan *c = time_timer_c(tm);

    /* Go 1.23's channel says it holds nothing, before the timer fires and
     * after, even though there is a value in it. */
    CHECK_INT_EQ(chan_len(c), 0);
    CHECK_INT_EQ(chan_cap(c), 0);
    synctest_sleep(5 * TIME_SECOND);
    CHECK_INT_EQ(chan_len(c), 0);
    CHECK_INT_EQ(chan_cap(c), 0);

    Time v;
    CHECK(chan_recv(c, &v));
    CHECK(time_equal(v, time_add(start, 3 * TIME_SECOND)));
    CHECK(nothing_waiting(c));
    time_timer_free(tm);
}

static void TestATimerSendsTheTimeItWasDue(TestingT *t) {
    in_bubble(t, fires_on_time);
}

static void zero_and_negative(void *env) {
    TestingT *t = env;
    Alloc *a = heap_allocator();
    Time start = time_now();

    TimeTimer *zero = time_new_timer(a, 0);
    TimeTimer *neg = time_new_timer(a, -TIME_HOUR);
    Time v;
    CHECK(chan_recv(time_timer_c(zero), &v));
    CHECK(time_equal(v, start));
    CHECK(chan_recv(time_timer_c(neg), &v));
    CHECK(time_equal(v, start));
    CHECK(time_equal(time_now(), start));
    time_timer_free(zero);
    time_timer_free(neg);
}

static void TestAZeroOrNegativeTimerFiresStraightAway(TestingT *t) {
    in_bubble(t, zero_and_negative);
}

static void stop_before(void *env) {
    TestingT *t = env;
    TimeTimer *tm = time_new_timer(heap_allocator(), TIME_SECOND);

    CHECK(time_timer_stop(tm));
    CHECK(!time_timer_stop(tm));
    synctest_sleep(2 * TIME_SECOND);
    CHECK(nothing_waiting(time_timer_c(tm)));
    time_timer_free(tm);
}

static void TestStoppingATimerBeforeItFiresMeansItNeverSends(TestingT *t) {
    in_bubble(t, stop_before);
}

static void stop_unread(void *env) {
    TestingT *t = env;
    TimeTimer *tm = time_new_timer(heap_allocator(), TIME_SECOND);

    /* The timer has fired and its value is sitting in the channel. Go 1.23
     * says that counts as stopping it in time: Stop answers true and the value
     * is gone. */
    synctest_sleep(2 * TIME_SECOND);
    CHECK(time_timer_stop(tm));
    CHECK(nothing_waiting(time_timer_c(tm)));
    CHECK(!time_timer_stop(tm));
    time_timer_free(tm);
}

static void TestStoppingATimerWhoseValueNobodyReadThrowsItAway(TestingT *t) {
    in_bubble(t, stop_unread);
}

static void stop_read(void *env) {
    TestingT *t = env;
    TimeTimer *tm = time_new_timer(heap_allocator(), TIME_SECOND);

    Time v;
    CHECK(chan_recv(time_timer_c(tm), &v));
    CHECK(!time_timer_stop(tm));

    bool pending = true;
    CHECK(time_timer_reset(tm, TIME_SECOND, &pending));
    CHECK(!pending);
    CHECK(chan_recv(time_timer_c(tm), &v));
    time_timer_free(tm);
}

static void TestATimerThatHasBeenReadIsNotPending(TestingT *t) {
    in_bubble(t, stop_read);
}

static void reset_unread(void *env) {
    TestingT *t = env;
    TimeTimer *tm = time_new_timer(heap_allocator(), TIME_SECOND);

    synctest_sleep(2 * TIME_SECOND);
    Time at = time_now();
    bool pending = false;
    CHECK(time_timer_reset(tm, TIME_SECOND, &pending));
    CHECK(pending);

    /* The value from the first firing is gone, and the one that arrives is for
     * the time the reset asked for. */
    CHECK(nothing_waiting(time_timer_c(tm)));
    Time v;
    CHECK(chan_recv(time_timer_c(tm), &v));
    CHECK(time_equal(v, time_add(at, TIME_SECOND)));
    time_timer_free(tm);
}

static void TestResettingATimerNeverDeliversTheOldValue(TestingT *t) {
    in_bubble(t, reset_unread);
}

static void reset_earlier(void *env) {
    TestingT *t = env;
    Time start = time_now();
    TimeTimer *tm = time_new_timer(heap_allocator(), TIME_HOUR);

    bool pending = false;
    CHECK(time_timer_reset(tm, TIME_MINUTE, &pending));
    CHECK(pending);
    Time v;
    CHECK(chan_recv(time_timer_c(tm), &v));
    CHECK(time_equal(v, time_add(start, TIME_MINUTE)));

    /* And nothing more when the hour comes round. */
    synctest_sleep(2 * TIME_HOUR);
    CHECK(nothing_waiting(time_timer_c(tm)));
    time_timer_free(tm);
}

static void TestATimerCanBeMovedEarlier(TestingT *t) {
    in_bubble(t, reset_earlier);
}

static void noop(void *env) {
    (void)env;
}

static void after_func_has_no_channel(void *env) {
    TestingT *t = env;
    TimeTimer *tm =
        time_after_func(heap_allocator(), TIME_SECOND, BURROW_FN(Func, noop, NULL));

    CHECK(time_timer_c(tm) == NULL);
    time_timer_free(tm);
}

static void TestAnAfterFuncTimerHasNoChannel(TestingT *t) {
    in_bubble(t, after_func_has_no_channel);
}

static void deadline_select(void *env) {
    TestingT *t = env;
    Alloc *a = heap_allocator();
    Time start = time_now();

    /* The usual shape: wait for some work, or give up after a while. The work
     * never comes. */
    Chan *work = chan_make(a, TYPE_INT, 0);
    TimeTimer *tm = time_new_timer(a, 10 * TIME_SECOND);
    Int got = 0;
    Time v;
    SelectCase cases[2] = {BURROW_RECV(work, &got), BURROW_RECV(time_timer_c(tm), &v)};

    CHECK_INT_EQ(chan_select(cases, 2), 1);
    CHECK(time_equal(v, time_add(start, 10 * TIME_SECOND)));
    time_timer_free(tm);
    chan_free(work);
}

static void TestATimerPutsADeadlineOnASelect(TestingT *t) {
    in_bubble(t, deadline_select);
}

/* ------------------------------------------------------------------- after */

static void after_chan(void *env) {
    TestingT *t = env;
    Alloc *a = heap_allocator();
    Time start = time_now();

    Chan *c = time_after_chan(a, 2 * TIME_SECOND);
    CHECK(c != NULL);
    Time v;
    CHECK(chan_recv(c, &v));
    CHECK(time_equal(v, time_add(start, 2 * TIME_SECOND)));
    chan_free(c);

    /* Freed before it fires, which takes the timer out of the bubble's heap.
     * If it did not, the bubble's clock would jump an hour at the end and send
     * on a channel that is gone. */
    c = time_after_chan(a, TIME_HOUR);
    chan_free(c);
}

static void TestAfterSendsOnceAndChanFreeTakesTheTimerWithIt(TestingT *t) {
    in_bubble(t, after_chan);
}

/* ------------------------------------------------------------------ tickers */

static void ticks(void *env) {
    TestingT *t = env;
    Alloc *a = heap_allocator();
    Time start = time_now();

    TimeTicker *tk = time_new_ticker(a, TIME_SECOND);
    CHECK(tk != NULL);
    Chan *c = time_ticker_c(tk);
    CHECK_INT_EQ(chan_len(c), 0);
    CHECK_INT_EQ(chan_cap(c), 0);

    Time v;
    for (int k = 1; k <= 3; k++) {
        CHECK(chan_recv(c, &v));
        CHECK(time_equal(v, time_add(start, (Duration)k * TIME_SECOND)));
    }

    /* Nobody reads for three and a half seconds. The tick at four is kept and
     * the ones at five and six are dropped, and the next one after that is at
     * seven, on the original schedule. */
    synctest_sleep(3 * TIME_SECOND + TIME_SECOND / 2);
    CHECK(chan_recv(c, &v));
    CHECK(time_equal(v, time_add(start, 4 * TIME_SECOND)));
    CHECK(chan_recv(c, &v));
    CHECK(time_equal(v, time_add(start, 7 * TIME_SECOND)));

    /* Stopped, with a tick left unread, which goes too. */
    synctest_sleep(TIME_SECOND);
    time_ticker_stop(tk);
    CHECK(nothing_waiting(c));
    synctest_sleep(5 * TIME_SECOND);
    CHECK(nothing_waiting(c));

    /* Reset starts it again on a new period, counted from now. */
    Time at = time_now();
    CHECK(time_ticker_reset(tk, 2 * TIME_SECOND));
    CHECK(chan_recv(c, &v));
    CHECK(time_equal(v, time_add(at, 2 * TIME_SECOND)));
    CHECK(chan_recv(c, &v));
    CHECK(time_equal(v, time_add(at, 4 * TIME_SECOND)));

    time_ticker_free(tk);
}

static void TestATickerTicksOnScheduleAndDropsWhatNobodyReads(TestingT *t) {
    in_bubble(t, ticks);
}

static void tick_chan(void *env) {
    TestingT *t = env;
    Alloc *a = heap_allocator();
    Time start = time_now();

    CHECK(time_tick(a, 0) == NULL);
    CHECK(time_tick(a, -TIME_SECOND) == NULL);

    Chan *c = time_tick(a, TIME_MINUTE);
    Time v;
    for (int k = 1; k <= 3; k++) {
        CHECK(chan_recv(c, &v));
        CHECK(time_equal(v, time_add(start, (Duration)k * TIME_MINUTE)));
    }
    chan_free(c);
}

static void TestTickIsATickerYouOnlySeeTheChannelOf(TestingT *t) {
    in_bubble(t, tick_chan);
}

static void bad_intervals(void *env) {
    TestingT *t = env;
    Alloc *a = heap_allocator();

    /* Plain string panics in Go, not runtime errors, and the same here. */
    EXPECT_PANIC((void)time_new_ticker(a, 0));
    CHECK(fatal_did_catch);
    CHECK_STR_EQ(fatal_caught, "non-positive interval for NewTicker");
    CHECK(!fatal_was_runtime_error);

    TimeTicker *tk = time_new_ticker(a, TIME_SECOND);
    CHECK_PANIC((void)time_ticker_reset(tk, -TIME_SECOND),
                "non-positive interval for Ticker.Reset");
    CHECK_PANIC((void)time_ticker_reset(NULL, TIME_SECOND),
                "time: Reset called on uninitialized Ticker");
    time_ticker_free(tk);

    time_ticker_stop(NULL);
    time_ticker_free(NULL);
    time_timer_free(NULL);
}

static void TestNonPositiveTickerIntervalsPanic(TestingT *t) {
    in_bubble(t, bad_intervals);
}

/* --------------------------------------------------------- the real clock */

static void TestAfterWaitsOnTheRealClock(TestingT *t) {
    Duration d = 20 * TIME_MILLISECOND;
    Time start = time_now();
    Chan *c = time_after_chan(heap_allocator(), d);
    Time v;

    CHECK(chan_recv(c, &v));
    CHECK(time_since(start) >= d);
    CHECK(!time_before(v, time_add(start, d)));
    chan_free(c);
}

/* Stop against a timer that is due right now, on another thread, many times
 * over. Whichever way each race goes, once Stop has returned the channel has
 * nothing in it and never gets anything. */
static void TestNoValueArrivesAfterStopReturns(TestingT *t) {
    Alloc *a = heap_allocator();
    TimeTimer *tm = time_new_timer(a, TIME_HOUR);
    Chan *c = time_timer_c(tm);
    Int stale = 0;

    for (int i = 0; i < 5000; i++) {
        CHECK(time_timer_reset(tm, (Duration)(i % 4) * TIME_MICROSECOND, NULL));
        if (i % 3 == 0)
            runtime_gosched();
        (void)time_timer_stop(tm);
        if (!nothing_waiting(c))
            stale++;
    }
    time_sleep(TIME_MILLISECOND);
    if (!nothing_waiting(c))
        stale++;

    CHECK_INT_EQ(stale, 0);
    time_timer_free(tm);
}

/* Reset against the same race. The value that arrives after a Reset is always
 * one the Reset armed, so it is never earlier than the moment Reset was
 * called. */
static void TestTheValueAfterResetIsNeverAStaleOne(TestingT *t) {
    Alloc *a = heap_allocator();
    TimeTimer *tm = time_new_timer(a, 0);
    Chan *c = time_timer_c(tm);
    Int stale = 0;

    for (int i = 0; i < 5000; i++) {
        if (i % 3 == 0)
            runtime_gosched();
        Time before = time_now();
        CHECK(time_timer_reset(tm, 0, NULL));
        Time v;
        CHECK(chan_recv(c, &v));
        if (time_before(v, before))
            stale++;
    }

    CHECK_INT_EQ(stale, 0);
    time_timer_free(tm);
}

#define TESTS(X)                                                                       \
    X(TestATimerSendsTheTimeItWasDue)                                                  \
    X(TestAZeroOrNegativeTimerFiresStraightAway)                                       \
    X(TestStoppingATimerBeforeItFiresMeansItNeverSends)                                \
    X(TestStoppingATimerWhoseValueNobodyReadThrowsItAway)                              \
    X(TestATimerThatHasBeenReadIsNotPending)                                           \
    X(TestResettingATimerNeverDeliversTheOldValue)                                     \
    X(TestATimerCanBeMovedEarlier)                                                     \
    X(TestAnAfterFuncTimerHasNoChannel)                                                \
    X(TestATimerPutsADeadlineOnASelect)                                                \
    X(TestAfterSendsOnceAndChanFreeTakesTheTimerWithIt)                                \
    X(TestATickerTicksOnScheduleAndDropsWhatNobodyReads)                               \
    X(TestTickIsATickerYouOnlySeeTheChannelOf)                                         \
    X(TestNonPositiveTickerIntervalsPanic)                                             \
    X(TestAfterWaitsOnTheRealClock)                                                    \
    X(TestNoValueArrivesAfterStopReturns)                                              \
    X(TestTheValueAfterResetIsNeverAStaleOne)

TESTING_MAIN(TESTS)
