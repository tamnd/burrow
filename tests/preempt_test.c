/* Tests for preemption.
 *
 * What is being tested here is one sentence: a goroutine that never blocks does
 * not keep its processor for ever. Everything below is a different way of
 * asking that, and every one of them runs with a single P, because a second P
 * would let the goroutine that is supposed to be starved run somewhere else and
 * the test would pass without the feature.
 *
 * The shape is always the same. Start a helper, then go round a loop that does
 * not block and does not call anything that could block, and see whether the
 * helper ever got a turn. Without preemption the helper sits in the run queue
 * until the spinner finishes, and since the spinner only finishes when it sees
 * the helper's flag, that is a hang. So every loop in this file has a deadline
 * in it: a broken build fails these tests after a few seconds rather than
 * stopping the suite, which matters because a hang gives nobody a stack to read.
 *
 * The one thing deliberately not tested is the compromise. A loop that passes
 * no safe point is not preempted, and there is no way to check that which is
 * not a hang on the build where it is wrong. docs/design/06-runtime.md section
 * 9 states it, include/burrow/proc.h repeats it on runtime_preempt_point, and
 * it stays a statement rather than a test.
 *
 * These are wall clock tests, which is the thing tests/synctest_test.c exists to
 * avoid, and there is no way round it. The question being asked is whether the
 * monitor thread notices in time, and the monitor thread watches the real clock.
 * A bubble would stop that clock and with it the only thing under test. The
 * deadlines are therefore set hundreds of times longer than the ten milliseconds
 * the answer should take, so that a machine under load is slow here rather than
 * red.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/proc.h"

#include "burrow/atomic.h"
#include "burrow/chan.h"
#include "burrow/clock.h"
#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/type.h"

#include "harness.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How long a spinner waits for the helper before deciding it is never coming.
 *
 * sysmon asks a goroutine to give way once it has been on its processor for ten
 * milliseconds, so the real answer is two orders of magnitude inside this. The
 * gap is for the machines these run on: a laptop with a build going and a CI
 * box with eight jobs on four cores both make the monitor thread late, and a
 * test that goes red when the machine is busy is worse than no test. */
#define DEADLINE_NS (5 * 1000 * 1000 * 1000LL)

/* The flag the helper sets, the answer the spinner reached, and how long it
 * took. All three are file statics rather than locals because the goroutine
 * bodies and the test function are different frames, and a goroutine is not
 * allowed to call CHECK. */
static uint32_t helper_ran;
static uint32_t spinner_saw;
static int64_t spinner_took;

static void helper(void *env) {
    (void)env;
    burrow__atomic_store_release_u32(&helper_ran, 1);
}

/* True when the helper has had a turn, false when the deadline went past first.
 * The loop body is passed in, and it is the only thing that differs between the
 * tests: each one is a different candidate safe point. */
static bool spin_until_helped(void (*safe_point)(void)) {
    int64_t start = burrow__nanotime();

    for (;;) {
        if (burrow__atomic_load_acquire_u32(&helper_ran) != 0) {
            spinner_took = burrow__nanotime() - start;
            return true;
        }
        if (burrow__nanotime() - start > DEADLINE_NS) {
            spinner_took = burrow__nanotime() - start;
            return false;
        }
        safe_point();
    }
}

static void reset(void) {
    burrow__atomic_store_u32(&helper_ran, 0);
    burrow__atomic_store_u32(&spinner_saw, 0);
    spinner_took = 0;
}

/* ------------------------------------------------- the explicit safe point
 *
 * The one a program written against this library reaches for. runtime_gosched
 * would also get the helper running, and it would do it by going through the
 * scheduler on every single turn, which is the cost this call exists to avoid.
 * So this test is about runtime_preempt_point being enough, not about it being
 * the only thing that works. */

static void explicit_body(void *env) {
    (void)env;

    if (!go(BURROW_FN(Func, helper, NULL)))
        return;

    if (spin_until_helped(runtime_preempt_point))
        burrow__atomic_store_release_u32(&spinner_saw, 1);
}

TEST(a_loop_with_an_explicit_safe_point_in_it_gives_way) {
    reset();

    int old = runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, explicit_body, NULL));
    (void)runtime_gomaxprocs(old);

    /* The helper is on the same P as the spinner and there is nowhere else for
     * it to go, so the spinner seeing the flag at all means the spinner was
     * asked to stop and did. Checking helper_ran here instead would prove
     * nothing: the helper always runs eventually, because the spinner gives up
     * at the deadline and then it is next in the queue. What is being asked is
     * whether the helper ran while the spinner was still going. */
    CHECK(burrow__atomic_load_acquire_u32(&spinner_saw) == 1);
    CHECK(spinner_took < DEADLINE_NS);
}

/* ------------------------------------------------------- channel operations
 *
 * The safe point a program gets without asking for one.
 *
 * chan_try_recv on an empty open channel answers false without taking a lock
 * and without parking, so a loop of them is a loop that never blocks: exactly
 * the shape a poll written by hand takes, and exactly the shape that used to
 * hold a thread for ever. The channel is made outside any bubble and stays
 * empty, so nothing in here can accidentally park and pass the test for the
 * wrong reason. */

static Chan *empty;

static void try_recv_once(void) {
    Int v = 0;
    (void)chan_try_recv(empty, &v, NULL);
}

static void chan_body(void *env) {
    (void)env;

    if (!go(BURROW_FN(Func, helper, NULL)))
        return;

    if (spin_until_helped(try_recv_once))
        burrow__atomic_store_release_u32(&spinner_saw, 1);
}

TEST(a_loop_of_channel_operations_gives_way) {
    reset();

    empty = chan_make(heap_allocator(), TYPE_INT, 1);
    CHECK(empty != NULL);

    int old = runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, chan_body, NULL));
    (void)runtime_gomaxprocs(old);

    CHECK(burrow__atomic_load_acquire_u32(&spinner_saw) == 1);
    CHECK(spinner_took < DEADLINE_NS);

    CHECK_INT_EQ((int)chan_len(empty), 0);
    chan_free(empty);
    empty = NULL;
}

/* ------------------------------------------------------- select with a default
 *
 * The other shape, and the one with the strongest claim on a safe point. A
 * select with a default arm is the only construct in the language that is built
 * to be called in a loop and guaranteed never to wait, so a version of this
 * library that preempted everywhere except here would still hang on the code
 * most likely to need it. */

static void select_once(void) {
    Int v = 0;
    SelectCase cases[] = {BURROW_RECV(empty, &v), BURROW_DEFAULT};
    (void)chan_select(cases, 2);
}

static void select_body(void *env) {
    (void)env;

    if (!go(BURROW_FN(Func, helper, NULL)))
        return;

    if (spin_until_helped(select_once))
        burrow__atomic_store_release_u32(&spinner_saw, 1);
}

TEST(a_loop_of_selects_with_a_default_gives_way) {
    reset();

    empty = chan_make(heap_allocator(), TYPE_INT, 1);
    CHECK(empty != NULL);

    int old = runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, select_body, NULL));
    (void)runtime_gomaxprocs(old);

    CHECK(burrow__atomic_load_acquire_u32(&spinner_saw) == 1);
    CHECK(spinner_took < DEADLINE_NS);

    chan_free(empty);
    empty = NULL;
}

/* --------------------------------------------------------------- fair shares
 *
 * Everything above proves one goroutine gets a turn. This proves the turns keep
 * coming, which is a different claim and the one that makes preemption worth
 * having.
 *
 * Four spinners on one processor, none of which ever blocks, and every one of
 * them has to record that it ran. A build with no preemption gets one: the
 * first one to start holds the processor until the deadline and the other three
 * come out of the run queue with the deadline already behind them.
 *
 * They all stop as soon as the last one has checked in, rather than each
 * running a fixed window. The window version was a guess at how long four turns
 * take, and that is not a thing to guess at: sysmon asks after ten milliseconds
 * of a quiet machine and considerably more of a busy one, so a window short
 * enough to be a quick test is short enough to go red on a loaded build box.
 * This way the test takes as long as it takes, and the deadline is not a
 * measurement any more, it is only there so that a broken build fails rather
 * than hanging. */

#define SPINNERS 4

static int64_t share_deadline;
static uint32_t laps[SPINNERS];
static uint32_t claimed;
static uint32_t checked_in;

static bool everybody_ran(void) {
    return burrow__atomic_load_acquire_u32(&checked_in) == SPINNERS;
}

static void share_body(void *env) {
    (void)env;

    /* Each spinner takes the next slot on the way in rather than being handed
     * one, because go takes a void pointer and an index is not a pointer. The
     * add returns the old value, which is this goroutine's slot. */
    uint32_t slot = burrow__atomic_add_u32(&claimed, 1);
    if (slot >= SPINNERS)
        return;

    laps[slot]++;
    (void)burrow__atomic_add_u32(&checked_in, 1);

    while (!everybody_ran() && burrow__nanotime() < share_deadline) {
        laps[slot]++;
        runtime_preempt_point();
    }
}

static void share_top(void *env) {
    (void)env;

    share_deadline = burrow__nanotime() + DEADLINE_NS;

    for (int i = 0; i < SPINNERS; i++) {
        if (!go(BURROW_FN(Func, share_body, NULL)))
            return;
    }

    /* The top goroutine joins in rather than waiting for them, because a wait
     * would park it and a parked goroutine is not competing for the processor
     * that is under test. It has no slot, so its laps go nowhere. */
    while (!everybody_ran() && burrow__nanotime() < share_deadline)
        runtime_preempt_point();
}

TEST(several_spinners_on_one_processor_all_make_progress) {
    burrow__atomic_store_u32(&claimed, 0);
    burrow__atomic_store_u32(&checked_in, 0);
    for (int i = 0; i < SPINNERS; i++)
        laps[i] = 0;

    int old = runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, share_top, NULL));
    (void)runtime_gomaxprocs(old);

    CHECK_INT_EQ((int)burrow__atomic_load_acquire_u32(&claimed), SPINNERS);

    /* Every one of them ran while the others were still going, which is the
     * check. Reading laps after runtime_main has returned is safe: it joins
     * every thread it started, so nothing is left to write them. */
    for (int i = 0; i < SPINNERS; i++)
        CHECK(laps[i] > 0);
}

/* ------------------------------------------------------- outside a goroutine
 *
 * A safe point on a plain thread is nothing at all. This matters because it is
 * what lets a function that is sometimes called from a goroutine and sometimes
 * from the program's own thread put one in its loop without first working out
 * which it is. The alternative would be an assertion, and an assertion here
 * would make the call unusable in exactly the library code that wants it. */

TEST(a_safe_point_off_a_goroutine_does_nothing) {
    for (int i = 0; i < 1000; i++)
        runtime_preempt_point();

    /* Reaching this line is the check. Nothing else can be asserted: the call
     * has no return value and no visible effect, and having no visible effect
     * is the property. */
    CHECK(true);
}

int main(void) {
    RUN(a_loop_with_an_explicit_safe_point_in_it_gives_way);
    RUN(a_loop_of_channel_operations_gives_way);
    RUN(a_loop_of_selects_with_a_default_gives_way);
    RUN(several_spinners_on_one_processor_all_make_progress);
    RUN(a_safe_point_off_a_goroutine_does_nothing);

    return harness_report("preempt");
}
