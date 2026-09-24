/* Tests for sleeping and for running a function later.
 *
 * These all go through the real scheduler on the real clock, which is the
 * opposite of timer_test.c and is the point of them. That file drives a heap of
 * timers with a made up clock and checks the arithmetic. This one checks that a
 * goroutine which asks to be woken in twenty milliseconds is actually woken, by
 * a thread, after twenty milliseconds of real time.
 *
 * So there is a rule here about what is checked. A sleep that was asked for
 * twenty milliseconds and came back after nineteen is a bug and is checked for.
 * One that came back after four hundred is a machine with somebody else's build
 * on it, and is not, because a test that fails when the machine is busy teaches
 * everybody to ignore it. Waiting for something to happen is done by parking on
 * a gate rather than by sleeping for longer than it ought to take, so the only
 * way for these to be wrong about a thing that never happens is to hang, and a
 * suite that hangs is a suite that gets looked at.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/time.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/func.h"
#include "burrow/lock.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/sched.h"
#include "burrow/timer.h"

#include "check.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Twenty milliseconds, long enough that a thread really has to go to sleep for
 * it and short enough that the whole suite still runs in under a second. */
#define WAIT (20 * TIME_MILLISECOND)

/* ----------------------------------------------------------------- the gate
 *
 * One goroutine waits, another lets it through, and neither of them has a
 * timeout in it. This is sched_test.c's gate and timer_test.c's gate, copied
 * again, because a test file that shares its helpers with another test file is a
 * test file you have to read two of. */

typedef struct Gate {
    burrow__Lock lock;
    Goroutine *waiter;
    uint32_t signalled;
} Gate;

static bool gate_unlock(Goroutine *g, void *arg) {
    (void)g;
    burrow__unlock(&((Gate *)arg)->lock);
    return true;
}

static void gate_wait(Gate *gt) {
    burrow__lock(&gt->lock);
    if (gt->signalled != 0) {
        gt->signalled = 0;
        burrow__unlock(&gt->lock);
        return;
    }
    gt->waiter = sched_current();
    sched_park(gate_unlock, gt);
}

static void gate_signal(Gate *gt) {
    burrow__lock(&gt->lock);
    Goroutine *w = gt->waiter;
    if (w == NULL) {
        gt->signalled = 1;
        burrow__unlock(&gt->lock);
        return;
    }
    gt->waiter = NULL;
    burrow__unlock(&gt->lock);
    sched_ready(w);
}

/* --------------------------------------------------------------- off the runtime */

static void TestASleepOnAPlainThreadStillWaits(TestingT *t) {
    /* Nothing has been started, so this thread is not a goroutine and there is
     * nothing to park. The sleep has to happen anyway, because setup code and
     * tests call it before the runtime exists and a call that quietly does
     * nothing there is worse than not having it. */
    int64_t start = burrow__nanotime();
    time_sleep(WAIT);
    CHECK(burrow__nanotime() - start >= WAIT);
}

static void TestASleepOfNothingIsNotASleep(TestingT *t) {
    int64_t start = burrow__nanotime();
    time_sleep(0);
    time_sleep(-5 * TIME_SECOND);
    int64_t took = burrow__nanotime() - start;

    /* The bound is a whole second for two calls that should take nanoseconds,
     * which is not a measurement, it is the difference between coming straight
     * back and sleeping for the five seconds that were asked for. A tighter one
     * would only ever catch a loaded machine. */
    CHECK(took < TIME_SECOND);
}

/* ----------------------------------------------------------------- sleeping */

static int64_t slept;

static void sleep_once(void *env) {
    (void)env;
    int64_t start = burrow__nanotime();
    time_sleep(WAIT);
    slept = burrow__nanotime() - start;
}

static void TestASleepLastsAtLeastAsLongAsItWasAskedTo(TestingT *t) {
    slept = 0;
    (void)runtime_gomaxprocs(1);

    runtime_main(BURROW_FN(Func, sleep_once, NULL));

    CHECK(slept >= WAIT);
}

static uint32_t helper_ran;
static bool helper_ran_first;
static bool helper_started;

static void set_a_flag(void *env) {
    (void)env;
    burrow__atomic_store_u32(&helper_ran, 1);
}

static void sleep_while_another_runs(void *env) {
    (void)env;
    helper_started = go(BURROW_FN(Func, set_a_flag, NULL));
    time_sleep(WAIT);
    helper_ran_first = burrow__atomic_load_acquire_u32(&helper_ran) != 0;
}

static void TestASleepingGoroutineGivesUpItsThread(TestingT *t) {
    helper_ran = 0;
    helper_ran_first = false;
    helper_started = false;

    /* One P, so there is one thread running goroutines and the two of them
     * cannot overlap. If a sleep held on to the thread, the other goroutine
     * could not run until the sleep was over, and the flag would still be
     * down. */
    (void)runtime_gomaxprocs(1);

    runtime_main(BURROW_FN(Func, sleep_while_another_runs, NULL));

    CHECK(helper_started);
    CHECK(helper_ran_first);
}

/* Three sleepers of different lengths on two Ps, which is fewer Ps than
 * sleepers, so at least one of them has to be woken by a thread that was busy
 * with somebody else when its timer came due.
 *
 * What is checked is that all three come back and that each one waited at least
 * as long as it asked for. What is deliberately not checked is the order they
 * come back in. The heap runs the timers in order and there is a test for that
 * in timer_test.c, on a clock the test drives, where the answer does not depend
 * on anything else the machine is doing. Out here a goroutine whose timer has
 * fired is a goroutine on a run queue, and how long it waits there is a question
 * about load rather than about timers. Twenty milliseconds apart was not enough
 * of a gap for that to come out the same way twice on a four core virtual
 * machine, and a bigger gap would only have made a slow test that still fails on
 * a busy one. */
static Gate all_done;
static uint32_t still_asleep;
static int64_t waited[3];
static uint32_t woke_count;

static void wake_up(uint32_t which, int64_t started) {
    waited[which] = burrow__nanotime() - started;
    (void)burrow__atomic_add_u32(&woke_count, 1);
    if (burrow__atomic_add_u32(&still_asleep, 0U - 1U) == 1)
        gate_signal(&all_done);
}

static void sleep_long(void *env) {
    (void)env;
    int64_t at = burrow__nanotime();
    time_sleep(3 * WAIT);
    wake_up(2, at);
}

static void sleep_middling(void *env) {
    (void)env;
    int64_t at = burrow__nanotime();
    time_sleep(2 * WAIT);
    wake_up(1, at);
}

static void sleep_short(void *env) {
    (void)env;
    int64_t at = burrow__nanotime();
    time_sleep(WAIT);
    wake_up(0, at);
}

static void start_three_sleepers(void *env) {
    (void)env;
    all_done = (Gate){0};
    still_asleep = 3;

    /* Started longest first, so that a sleep which somehow did not wait at all
     * would finish out of turn rather than in it. */
    helper_started = go(BURROW_FN(Func, sleep_long, NULL));
    helper_started = go(BURROW_FN(Func, sleep_middling, NULL)) && helper_started;
    helper_started = go(BURROW_FN(Func, sleep_short, NULL)) && helper_started;

    gate_wait(&all_done);
}

static void TestSleepsOfDifferentLengthsEachWaitTheirOwn(TestingT *t) {
    woke_count = 0;
    waited[0] = waited[1] = waited[2] = 0;
    helper_started = false;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, start_three_sleepers, NULL));

    CHECK(helper_started);
    CHECK_INT_EQ(woke_count, 3);
    CHECK(waited[0] >= WAIT);
    CHECK(waited[1] >= 2 * WAIT);
    CHECK(waited[2] >= 3 * WAIT);
}

/* --------------------------------------------------------------- after func */

static Gate callback_done;
static uint32_t callback_runs;
static int env_marker;
static Goroutine *callback_g;
static Goroutine *arming_g;
static void *callback_env;
static TimeTimer *the_timer;
static bool stopped;
static bool pending;
static bool reset_ok;

static void note_the_callback(void *env) {
    callback_env = env;
    callback_g = sched_current();
    burrow__atomic_add_u32(&callback_runs, 1);
    gate_signal(&callback_done);
}

static void arm_and_wait(void *env) {
    (void)env;
    callback_done = (Gate){0};
    arming_g = sched_current();

    the_timer = time_after_func(heap_allocator(), WAIT,
                                BURROW_FN(Func, note_the_callback, &env_marker));
    if (the_timer == NULL)
        return;

    gate_wait(&callback_done);
    time_timer_free(the_timer);
    the_timer = NULL;
}

static void TestAfterFuncRunsWhatItWasGiven(TestingT *t) {
    callback_runs = 0;
    callback_env = NULL;
    the_timer = NULL;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, arm_and_wait, NULL));

    CHECK_INT_EQ(callback_runs, 1);
    CHECK(callback_env == &env_marker);
}

static void TestAfterFuncRunsItOnAGoroutineOfItsOwn(TestingT *t) {
    callback_runs = 0;
    callback_g = NULL;
    arming_g = NULL;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, arm_and_wait, NULL));

    CHECK_INT_EQ(callback_runs, 1);

    /* Go promises the callback its own goroutine, and it matters: a callback
     * that ran on the thread which noticed the timer could not block, and half
     * the things people do in one block. The arming goroutine is parked on the
     * gate while this happens, so it is still alive and its pointer is still
     * its own. */
    CHECK(callback_g != NULL);
    CHECK(callback_g != arming_g);
}

static void arm_then_stop(void *env) {
    (void)env;
    callback_done = (Gate){0};

    the_timer = time_after_func(heap_allocator(), WAIT,
                                BURROW_FN(Func, note_the_callback, NULL));
    if (the_timer == NULL)
        return;

    stopped = time_timer_stop(the_timer);

    /* Long enough that a timer which was going to run would have. Nothing here
     * waits on anything, so this is the one place a sleep is standing in for a
     * proof, and the sleep is five times the deadline it is covering. */
    time_sleep(5 * WAIT);

    /* And now that it has certainly fired or certainly not, ask again. */
    pending = time_timer_stop(the_timer);
    time_timer_free(the_timer);
    the_timer = NULL;
}

static void TestAStoppedTimerNeverRuns(TestingT *t) {
    callback_runs = 0;
    stopped = false;
    pending = true;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, arm_then_stop, NULL));

    CHECK_INT_EQ(callback_runs, 0);
    CHECK(stopped);
    CHECK(!pending);
}

static void arm_wait_then_stop(void *env) {
    (void)env;
    callback_done = (Gate){0};

    the_timer = time_after_func(heap_allocator(), WAIT,
                                BURROW_FN(Func, note_the_callback, NULL));
    if (the_timer == NULL)
        return;

    gate_wait(&callback_done);
    stopped = time_timer_stop(the_timer);
    time_timer_free(the_timer);
    the_timer = NULL;
}

static void TestStoppingATimerThatHasAlreadyRunSaysSo(TestingT *t) {
    callback_runs = 0;
    stopped = true;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, arm_wait_then_stop, NULL));

    CHECK_INT_EQ(callback_runs, 1);
    CHECK(!stopped);
}

static void arm_far_out_then_pull_it_in(void *env) {
    (void)env;
    callback_done = (Gate){0};

    the_timer = time_after_func(heap_allocator(), TIME_HOUR,
                                BURROW_FN(Func, note_the_callback, NULL));
    if (the_timer == NULL)
        return;

    reset_ok = time_timer_reset(the_timer, WAIT, &pending);
    gate_wait(&callback_done);
    time_timer_free(the_timer);
    the_timer = NULL;
}

static void TestATimerCanBeMovedEarlier(TestingT *t) {
    callback_runs = 0;
    reset_ok = false;
    pending = false;
    (void)runtime_gomaxprocs(2);

    /* An hour away, then twenty milliseconds away. If the move did not take,
     * this test does not fail, it hangs for an hour, which is the honest
     * failure for a wake that never comes. */
    runtime_main(BURROW_FN(Func, arm_far_out_then_pull_it_in, NULL));

    CHECK(reset_ok);
    CHECK(pending);
    CHECK_INT_EQ(callback_runs, 1);
}

static void arm_wait_and_arm_again(void *env) {
    (void)env;
    callback_done = (Gate){0};

    the_timer = time_after_func(heap_allocator(), WAIT,
                                BURROW_FN(Func, note_the_callback, NULL));
    if (the_timer == NULL)
        return;

    gate_wait(&callback_done);

    /* It has run, so it is in no heap at all any more and starting it again is
     * a fresh add rather than a move. That is the path that can fail, and it is
     * why the reset reports whether it worked. */
    reset_ok = time_timer_reset(the_timer, WAIT, &pending);
    gate_wait(&callback_done);

    time_timer_free(the_timer);
    the_timer = NULL;
}

static void TestATimerThatHasRunCanBeStartedAgain(TestingT *t) {
    callback_runs = 0;
    reset_ok = false;
    pending = true;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, arm_wait_and_arm_again, NULL));

    CHECK(reset_ok);
    CHECK(!pending);
    CHECK_INT_EQ(callback_runs, 2);
}

/* ------------------------------------------------------------------- freeing */

static uint32_t len_before;
static uint32_t len_after;

static void arm_one_and_free_it(void *env) {
    (void)env;
    callback_done = (Gate){0};

    /* An hour out, so it is certainly still sitting in this P's heap when it is
     * freed. Freeing it has to take it out of that heap, and the check is the
     * length of the heap before and after. */
    the_timer = time_after_func(heap_allocator(), TIME_HOUR,
                                BURROW_FN(Func, note_the_callback, NULL));
    if (the_timer == NULL)
        return;

    len_before = burrow__timers_len(burrow__timers_local());
    time_timer_free(the_timer);
    the_timer = NULL;
    len_after = burrow__timers_len(burrow__timers_local());

    /* And the set still works afterwards, which is the part that a heap left in
     * a bad state would fail. */
    the_timer = time_after_func(heap_allocator(), WAIT,
                                BURROW_FN(Func, note_the_callback, NULL));
    if (the_timer == NULL)
        return;
    gate_wait(&callback_done);
    time_timer_free(the_timer);
    the_timer = NULL;
}

static void TestFreeingATimerTakesItOutOfTheHeap(TestingT *t) {
    callback_runs = 0;
    len_before = 0;
    len_after = 99;
    (void)runtime_gomaxprocs(1);

    runtime_main(BURROW_FN(Func, arm_one_and_free_it, NULL));

    CHECK_INT_EQ(len_before, 1);
    CHECK_INT_EQ(len_after, 0);
    CHECK_INT_EQ(callback_runs, 1);
}

#define TESTS(X)                                                                       \
    X(TestASleepOnAPlainThreadStillWaits)                                              \
    X(TestASleepOfNothingIsNotASleep)                                                  \
    X(TestASleepLastsAtLeastAsLongAsItWasAskedTo)                                      \
    X(TestASleepingGoroutineGivesUpItsThread)                                          \
    X(TestSleepsOfDifferentLengthsEachWaitTheirOwn)                                    \
    X(TestAfterFuncRunsWhatItWasGiven)                                                 \
    X(TestAfterFuncRunsItOnAGoroutineOfItsOwn)                                         \
    X(TestAStoppedTimerNeverRuns)                                                      \
    X(TestStoppingATimerThatHasAlreadyRunSaysSo)                                       \
    X(TestATimerCanBeMovedEarlier)                                                     \
    X(TestATimerThatHasRunCanBeStartedAgain)                                           \
    X(TestFreeingATimerTakesItOutOfTheHeap)

TESTING_MAIN_BARE(TESTS)
