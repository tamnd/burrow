/* Tests for the monitor thread.
 *
 * Read the honest limit of this file first, because it changes what the checks
 * below are worth. The one job sysmon does today is a backstop: it looks for a
 * timer that is already due on a P nobody is holding, and pokes the scheduler
 * when it finds one. Reaching that state means the ordinary wake path has
 * already failed, and nothing in burrow can make it fail on purpose, so there is
 * no test here that watches the backstop save a program. What there is instead
 * is everything around it, which is the part that can be broken by writing it:
 * the thread starting, the decision it makes about when there is nothing left to
 * watch, the handshake that gets it out of that decision, and the shutdown.
 *
 * That last one is why the first test runs the runtime fifty times rather than
 * once. A monitor thread that is not woken at shutdown is a join that never
 * returns, a note freed while somebody is asleep on it is a crash under a
 * sanitiser, and both of those are the kind of thing that happens on the
 * fiftieth run and not the first.
 *
 * The middle test is the only way to reach the state sysmon sleeps indefinitely
 * in while the runtime is up, which is every P idle and not one timer anywhere.
 * A Go program cannot be in that state and be alive, because in Go there is
 * nothing outside the runtime to wake anybody. A burrow program can, because the
 * program burrow is linked into has its own threads, and one of them is allowed
 * to ready a goroutine. So that test is also a statement about what this library
 * is for.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/proc.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/func.h"
#include "burrow/note.h"
#include "burrow/thread.h"
#include "burrow/time.h"

#include "check.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How many times the second test gives the processor away waiting for the main
 * goroutine to park. It gets there in a handful of turns when the scheduler
 * works, so this number only costs anything on a run that is already failing. */
#define YIELD_LIMIT (2 * 1000 * 1000)

/* ------------------------------------------------- starting and stopping */

static uint32_t runs;

static void bump(void *env) {
    (void)env;
    (void)burrow__atomic_add_u32(&runs, 1);
}

static void TestTheRuntimeStartsAndStopsWithAMonitorThreadInIt(TestingT *t) {
    burrow__atomic_store_u32(&runs, 0);

    int old = runtime_gomaxprocs(2);

    for (uint32_t i = 0; i < 50; i++)
        runtime_main(BURROW_FN(Func, bump, NULL));

    (void)runtime_gomaxprocs(old);

    /* Fifty starts and fifty stops, and the interesting half is the stops. A
     * monitor that shutdown does not wake is a join that never comes back, so a
     * broken version of this does not fail here, it stops here. */
    CHECK_INT_EQ((int)burrow__atomic_load_acquire_u32(&runs), 50);
}

/* --------------------------------------------- nothing left to watch at all */

/* Set by the park callback, which runs once the goroutine can no longer be
 * reached from the thread it was running on. That is the point at which handing
 * the pointer to another thread is safe, and doing it any earlier is a race with
 * the switch rather than with anything being tested. */
static uint32_t parked;
static Goroutine *sleeper;
static uint32_t woke;

static bool published(Goroutine *g, void *lock) {
    (void)g;
    (void)lock;
    burrow__atomic_store_release_u32(&parked, 1);
    return true;
}

/* Rouses the goroutine from a thread the runtime knows nothing about.
 *
 * The wait after the park is the point of the whole test and not politeness.
 * With the goroutine parked and no timer anywhere, sysmon works out that there
 * is no event it could be early for and goes to sleep with no deadline on it,
 * and it only gets there if nothing disturbs it first. Fifty milliseconds is far
 * longer than the twenty microseconds its first pass takes and long enough to be
 * sure on a machine that is busy. */
static void outside_waker(void *arg) {
    (void)arg;

    for (long i = 0; i < YIELD_LIMIT; i++) {
        if (burrow__atomic_load_acquire_u32(&parked) != 0)
            break;
        burrow__thread_yield();
    }

    burrow__Note nap;
    if (burrow__note_init(&nap)) {
        (void)burrow__note_sleep_timeout(&nap, 50 * 1000 * 1000);
        burrow__note_free(&nap);
    }

    sched_ready(sleeper);
}

static void park_until_an_outsider_says_so(void *env) {
    (void)env;

    sleeper = sched_current();
    sched_park(published, NULL);

    burrow__atomic_store_release_u32(&woke, 1);
}

static void TestAGoroutineReadiedFromOutsideGetsTheProgramMovingAgain(TestingT *t) {
    burrow__atomic_store_u32(&parked, 0);
    burrow__atomic_store_u32(&woke, 0);
    sleeper = NULL;

    burrow__Thread waker;
    bool started = burrow__thread_start(&waker, outside_waker, NULL, 0);
    CHECK(started);
    if (!started)
        return;

    int old = runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, park_until_an_outsider_says_so, NULL));
    (void)runtime_gomaxprocs(old);

    (void)burrow__thread_join(&waker);

    /* The runtime came back, which it cannot do unless the goroutine was readied
     * and ran to the end. Everything asleep at that moment was asleep with no
     * deadline on it, so a wake that went missing anywhere in here is a test that
     * hangs rather than one that fails. */
    CHECK_INT_EQ((int)burrow__atomic_load_acquire_u32(&woke), 1);
}

/* ------------------------------------------------ timers, still unbothered */

static int64_t slept_for;

static void sleep_and_measure(void *env) {
    (void)env;

    int64_t start = burrow__nanotime();
    time_sleep(30 * TIME_MILLISECOND);
    slept_for = burrow__nanotime() - start;
}

/* The monitor pokes the scheduler through wakep, which is the same call a
 * goroutine becoming runnable makes, so a monitor that decides a timer is
 * overdue slightly too eagerly costs a thread a wakeup and must not cost anybody
 * a wrong answer. This is the check that says so: the sleeps below run on a
 * runtime with the monitor going, at the three widths where its wake times scan
 * has nothing, one thing and several things to look at. */
static void TestASleepIsStillASleepWithTheMonitorRunning(TestingT *t) {
    static const int procs[] = {1, 2, 4};

    for (size_t i = 0; i < sizeof(procs) / sizeof(procs[0]); i++) {
        slept_for = 0;

        int old = runtime_gomaxprocs(procs[i]);
        runtime_main(BURROW_FN(Func, sleep_and_measure, NULL));
        (void)runtime_gomaxprocs(old);

        /* Long enough is a check and short enough is not, because the upper end
         * belongs to the platform. macOS gives a relative condition variable
         * timeout a leeway of half the interval, so a thirty millisecond sleep
         * there comes back at forty five on an idle machine, and a loaded CI box
         * can be later still for reasons that have nothing to do with this. */
        CHECK(slept_for >= 30 * TIME_MILLISECOND);
    }
}

#define TESTS(X)                                                                       \
    X(TestTheRuntimeStartsAndStopsWithAMonitorThreadInIt)                              \
    X(TestAGoroutineReadiedFromOutsideGetsTheProgramMovingAgain)                       \
    X(TestASleepIsStillASleepWithTheMonitorRunning)

TESTING_MAIN_BARE(TESTS)
