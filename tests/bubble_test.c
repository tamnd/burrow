/* The rest of the runtime, run inside a bubble.
 *
 * docs/design/06-runtime.md asks for this in so many words: build synctest and
 * then go back over everything under it and run it again in there. The reason
 * is that a bubble is not a layer on top of the scheduler, it is a set of
 * counters threaded through the middle of it, and every park and every wake in
 * the runtime has to keep them straight. A place that forgets to count is not a
 * bug in that place. It is a bug in synctest_wait, somewhere else, later, and
 * it looks like a test that reads a value too early once a month.
 *
 * So this file is not a second copy of the channel tests. It is the questions
 * that only have an answer in here. Does a goroutine on a run queue still count
 * as running. Does a stopped timer still drag the clock. Is a select over
 * bubbled channels a durable wait, and is a select with one case that reaches
 * outside not one. Everything a bubble has an opinion about, asked of the code
 * that was written before the bubble existed.
 *
 * Roadmap steps 4, 5 and 6 are here: the scheduler, the timers and the
 * channels. Steps 7, 8 and 9, which are defer and panic, the sync package and
 * the netpoller, are their own file for the same reason they are their own
 * steps.
 *
 * The rules are tests/synctest_test.c's rules, because the difficulty is the
 * same. A broken bubble has to give the wrong answer rather than a slow one, so
 * the waiting is done with chan_try_send on an unbuffered channel, which is
 * true only if a receiver is parked on it at that instant. Nothing here sleeps
 * for real, only goroutines call anything concurrent, and only the test
 * function itself calls CHECK.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/synctest.h"

#include "burrow/atomic.h"
#include "burrow/chan.h"
#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "harness.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where a bubble's clock starts, which is midnight UTC on 2000-01-01 in
 * nanoseconds since the unix epoch. Every reading taken in here is measured
 * against this, so a test that gets an answer of 0 for an elapsed time is one
 * whose goroutine was not in the bubble at all. */
#define BUBBLE_START ((int64_t)946684800000000000)

static Chan *gate;
static Chan *outside_chan;
static Chan *handshake;

static uint32_t parked;
static uint32_t total;
static uint32_t got;
static uint32_t which;
static uint32_t ran;
static int64_t elapsed;

static void reset(void) {
    gate = NULL;
    outside_chan = NULL;
    handshake = NULL;
    parked = 0;
    total = 0;
    got = 0;
    which = 0;
    ran = 0;
    elapsed = 0;
}

/* ------------------------------------------------------- 4. the scheduler */

/* --- a crowd
 *
 * Five hundred goroutines is more than a P's run queue holds, so they spill
 * into the global queue and get stolen back out of it, and every one of those
 * moves is a place the bubble's count of who is running has to survive. Then
 * all of them park on the same channel, which is the other half: five hundred
 * parks and five hundred wakes, concurrently, on one lock.
 *
 * The check is that synctest_wait comes back when every last one of them is on
 * the channel and not before. chan_try_send answers true only if a receiver is
 * parked right now, so five hundred of them succeeding is five hundred parked
 * goroutines and nothing else. A bubble that lost count anywhere in there comes
 * back early and the number is short. */

#define CROWD 500

static void crowd_child(void *env) {
    (void)env;

    Int v;
    if (chan_recv(gate, &v))
        (void)burrow__atomic_add_u32(&total, (uint32_t)v);
}

static void crowd_body(void *env) {
    (void)env;

    gate = chan_make(heap_allocator(), TYPE_INT, 0);
    if (gate == NULL)
        return;

    for (int i = 0; i < CROWD; i++)
        if (!go(BURROW_FN(Func, crowd_child, NULL)))
            return;

    synctest_wait();

    uint32_t sent = 0;
    Int v = 1;
    for (int i = 0; i < CROWD; i++)
        if (chan_try_send(gate, &v))
            sent++;

    burrow__atomic_store_release_u32(&parked, sent);
}

static void crowd_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, crowd_body, NULL)))
        chan_free(gate);
}

TEST(five_hundred_goroutines_in_a_bubble_are_all_waited_for) {
    reset();
    runtime_main(BURROW_FN(Func, crowd_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&parked), CROWD);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&total), CROWD);
}

/* --- a goroutine that is only yielding
 *
 * runtime_gosched puts a goroutine back on a run queue, which means that for a
 * moment it is neither running nor parked. That moment is the one a bubble gets
 * wrong if it counts run queues instead of parks, and the way it gets it wrong
 * is that synctest_wait returns while the goroutine still has work left.
 *
 * So the child yields a thousand times, writing down how far it has got each
 * time, and only then blocks for real. The number the body reads after its wait
 * has to be the last one. Anything less is a wait that returned into the middle
 * of somebody else's loop, which is the exact bug this call exists to not
 * have. */

#define SPINS 1000

static void spin_child(void *env) {
    (void)env;

    for (int i = 0; i < SPINS; i++) {
        burrow__atomic_store_release_u32(&ran, (uint32_t)(i + 1));
        runtime_gosched();
    }

    Int v;
    if (chan_recv(gate, &v))
        burrow__atomic_store_release_u32(&got, (uint32_t)v);
}

static void spin_body(void *env) {
    (void)env;

    gate = chan_make(heap_allocator(), TYPE_INT, 0);
    if (gate == NULL)
        return;

    if (!go(BURROW_FN(Func, spin_child, NULL)))
        return;

    synctest_wait();
    burrow__atomic_store_release_u32(&total, burrow__atomic_load_acquire_u32(&ran));

    Int v = 9;
    if (chan_try_send(gate, &v))
        burrow__atomic_store_release_u32(&parked, 1);
}

static void spin_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, spin_body, NULL)))
        chan_free(gate);
}

TEST(a_goroutine_that_is_only_yielding_still_counts_as_running) {
    reset();
    runtime_main(BURROW_FN(Func, spin_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&total), SPINS);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&parked), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&got), 9);
}

/* --- a goroutine two removes from the body
 *
 * The bubble is inherited, so a goroutine started by a goroutine started by the
 * body is in it too, and so on down. Nothing in the scheduler says that in one
 * place: it falls out of every go taking the bubble of whoever called it, which
 * is the kind of thing that holds until somebody adds a path that starts a
 * goroutine from somewhere else.
 *
 * The great grandchild is the only one that blocks. If it had been born outside
 * the bubble the wait would return without it and the try_send would find
 * nobody there. */

static void deep_third(void *env) {
    (void)env;

    Int v;
    if (chan_recv(gate, &v))
        burrow__atomic_store_release_u32(&got, (uint32_t)v);
}

static void deep_second(void *env) {
    (void)env;
    (void)go(BURROW_FN(Func, deep_third, NULL));
}

static void deep_first(void *env) {
    (void)env;
    (void)go(BURROW_FN(Func, deep_second, NULL));
}

static void deep_body(void *env) {
    (void)env;

    gate = chan_make(heap_allocator(), TYPE_INT, 0);
    if (gate == NULL)
        return;

    if (!go(BURROW_FN(Func, deep_first, NULL)))
        return;

    synctest_wait();

    Int v = 7;
    if (chan_try_send(gate, &v))
        burrow__atomic_store_release_u32(&parked, 1);
}

static void deep_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, deep_body, NULL)))
        chan_free(gate);
}

TEST(a_goroutine_three_removes_from_the_body_is_still_in_the_bubble) {
    reset();
    runtime_main(BURROW_FN(Func, deep_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&parked), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&got), 7);
}

/* ----------------------------------------------------------- 5. the timers */

/* --- a timer that was stopped
 *
 * A stopped timer is not taken out of the heap when it is stopped. It is
 * marked, and the P that owns the heap throws it out the next time it walks
 * one, which is the right thing for a real heap and is a trap for a fake clock.
 * The clock steps from one deadline to the next by asking the heap, and it does
 * that in bigger steps than a real one ever takes.
 *
 * So the bubble here sleeps for two hours, which walks the clock straight over
 * the hour mark where the stopped timer is still sitting. A heap that ran what
 * it found there rather than what was still armed would run the callback on the
 * way past. */

static void never_runs(void *env) {
    (void)env;
    burrow__atomic_store_release_u32(&ran, 1);
}

static void stopped_body(void *env) {
    (void)env;

    TimeTimer *t =
        time_after_func(heap_allocator(), TIME_HOUR, BURROW_FN(Func, never_runs, NULL));
    if (t == NULL)
        return;

    if (time_timer_stop(t))
        burrow__atomic_store_release_u32(&parked, 1);

    int64_t start = burrow_nanotime();
    time_sleep(2 * TIME_HOUR);
    elapsed = burrow_nanotime() - start;

    time_timer_free(t);
}

static void stopped_top(void *env) {
    (void)env;
    (void)synctest_run(BURROW_FN(Func, stopped_body, NULL));
}

TEST(a_stopped_timer_in_a_bubble_does_not_run_when_the_clock_goes_past_it) {
    reset();
    runtime_main(BURROW_FN(Func, stopped_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&parked), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&ran), 0);
    CHECK_INT_EQ(elapsed, 2 * TIME_HOUR);
}

/* --- a timer that was moved
 *
 * Reset takes a timer out of the heap and puts it back somewhere else, and on
 * the fake clock "somewhere else" is measured from the bubble's reading rather
 * than the machine's. A reset that used the real clock would arm the timer at a
 * point twenty five years past the bubble's now, and the sleep below would end
 * with the callback still waiting.
 *
 * The callback writes down the bubble's time when it ran, so the check is the
 * new deadline to the nanosecond rather than the fact that it happened. */

static int64_t fired_at;

static void note_the_time(void *env) {
    (void)env;

    fired_at = burrow_nanotime();
    burrow__atomic_store_release_u32(&ran, 1);
}

static void reset_body(void *env) {
    (void)env;

    TimeTimer *t = time_after_func(heap_allocator(), TIME_HOUR,
                                   BURROW_FN(Func, note_the_time, NULL));
    if (t == NULL)
        return;

    bool pending = false;
    if (time_timer_reset(t, TIME_SECOND, &pending) && pending)
        burrow__atomic_store_release_u32(&parked, 1);

    time_sleep(2 * TIME_SECOND);
    time_timer_free(t);
}

static void reset_top(void *env) {
    (void)env;
    (void)synctest_run(BURROW_FN(Func, reset_body, NULL));
}

TEST(a_timer_reset_in_a_bubble_lands_on_the_bubble_clock) {
    reset();
    fired_at = 0;
    runtime_main(BURROW_FN(Func, reset_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&parked), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&ran), 1);
    CHECK_INT_EQ(fired_at, BUBBLE_START + TIME_SECOND);
}

/* --- a timer that arms a timer
 *
 * Every link in the chain is armed from a callback, which runs on a goroutine
 * the bubble started for it, on whatever thread happened to notice. So this is
 * the bubble arming its own clock from inside itself three times over, and each
 * arm has to land a second after the one that armed it rather than a second
 * after whenever the machine got round to running it.
 *
 * The times come out exact, which is the whole point of a fake clock. On the
 * real one this test would be a tolerance and a shrug. */

#define LINKS 3

static TimeTimer *chain[LINKS];
static int64_t chain_at[LINKS];
static uint32_t links;

static void chain_step(void *env) {
    (void)env;

    uint32_t i = burrow__atomic_add_u32(&links, 1);
    if (i >= LINKS)
        return;

    chain_at[i] = burrow_nanotime();
    if (i + 1 < LINKS)
        chain[i + 1] = time_after_func(heap_allocator(), TIME_SECOND,
                                       BURROW_FN(Func, chain_step, NULL));
}

static void chain_body(void *env) {
    (void)env;

    chain[0] = time_after_func(heap_allocator(), TIME_SECOND,
                               BURROW_FN(Func, chain_step, NULL));
    if (chain[0] == NULL)
        return;

    /* Longer than the chain, so the chain finishes first and the bubble always
     * has a timer left to move the clock on to. */
    time_sleep(TIME_MINUTE);
    elapsed = burrow_nanotime();
}

static void chain_top(void *env) {
    (void)env;

    (void)synctest_run(BURROW_FN(Func, chain_body, NULL));

    for (int i = 0; i < LINKS; i++)
        time_timer_free(chain[i]);
}

TEST(a_timer_that_arms_a_timer_in_a_bubble_stays_on_the_bubble_clock) {
    reset();
    for (int i = 0; i < LINKS; i++) {
        chain[i] = NULL;
        chain_at[i] = 0;
    }
    links = 0;

    runtime_main(BURROW_FN(Func, chain_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&links), LINKS);
    for (int i = 0; i < LINKS; i++)
        CHECK_INT_EQ(chain_at[i], BUBBLE_START + (int64_t)(i + 1) * TIME_SECOND);
    CHECK_INT_EQ(elapsed, BUBBLE_START + TIME_MINUTE);
}

/* --------------------------------------------------------- 6. the channels */

/* --- a select where everything is in the bubble
 *
 * A select parks once on behalf of several channels, so its answer to the
 * durability question is one answer for the whole list rather than one per
 * case. Every case here is on a channel made in the bubble, which makes the
 * whole select durable: the only goroutines that can reach those channels are
 * in here with it.
 *
 * The try_send goes to the second channel to make sure the select really is
 * waiting on all of its cases and not just on the first one it looked at. */

static Chan *other;

static void select_child(void *env) {
    (void)env;

    Int v = 0;
    SelectCase cases[] = {BURROW_RECV(gate, &v), BURROW_RECV(other, &v)};

    Int i = chan_select(cases, 2);
    burrow__atomic_store_release_u32(&which, (uint32_t)i);
    burrow__atomic_store_release_u32(&got, (uint32_t)v);
}

static void select_body(void *env) {
    (void)env;

    gate = chan_make(heap_allocator(), TYPE_INT, 0);
    other = chan_make(heap_allocator(), TYPE_INT, 0);
    if (gate == NULL || other == NULL)
        return;

    if (!go(BURROW_FN(Func, select_child, NULL)))
        return;

    synctest_wait();

    Int v = 11;
    if (chan_try_send(other, &v))
        burrow__atomic_store_release_u32(&parked, 1);
}

static void select_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, select_body, NULL))) {
        chan_free(gate);
        chan_free(other);
    }
}

TEST(a_select_over_bubbled_channels_is_a_durable_wait) {
    reset();
    other = NULL;
    runtime_main(BURROW_FN(Func, select_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&parked), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&which), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&got), 11);
}

/* --- a select with one foot outside
 *
 * One case on a channel from outside the bubble is enough to make the whole
 * select not durable, because that case can be run by somebody the bubble has
 * never heard of and so the goroutine is not stuck. This is the rule that a
 * per case answer would get wrong in the direction that hurts: a select that
 * was called durable because most of it was would let synctest_wait return
 * while the outsider was still on its way.
 *
 * Which is what this measures. The value the body reads after its wait is the
 * one the outsider sent, and a bubble that got the rule wrong reads zero. */

static void outsider(void *env) {
    (void)env;

    Int v;
    if (!chan_recv(handshake, &v))
        return;

    v = 23;
    chan_send(outside_chan, &v);
}

static void mixed_child(void *env) {
    (void)env;

    Int v = 0;
    SelectCase cases[] = {BURROW_RECV(gate, &v), BURROW_RECV(outside_chan, &v)};

    (void)chan_select(cases, 2);
    burrow__atomic_store_release_u32(&got, (uint32_t)v);
}

static void mixed_body(void *env) {
    (void)env;

    gate = chan_make(heap_allocator(), TYPE_INT, 0);
    if (gate == NULL)
        return;

    if (!go(BURROW_FN(Func, mixed_child, NULL)))
        return;

    /* Unbuffered, so this returns only once the outsider has taken it, which
     * puts the outsider on its way to the send before the wait below starts. */
    Int v = 1;
    chan_send(handshake, &v);

    synctest_wait();
    burrow__atomic_store_release_u32(&total, burrow__atomic_load_acquire_u32(&got));
}

static void mixed_top(void *env) {
    (void)env;

    handshake = chan_make(heap_allocator(), TYPE_INT, 0);
    outside_chan = chan_make(heap_allocator(), TYPE_INT, 0);
    if (handshake == NULL || outside_chan == NULL)
        return;

    if (!go(BURROW_FN(Func, outsider, NULL)))
        return;

    if (synctest_run(BURROW_FN(Func, mixed_body, NULL)))
        chan_free(gate);

    chan_free(handshake);
    chan_free(outside_chan);
}

TEST(a_select_with_a_case_from_outside_the_bubble_is_not_durable) {
    reset();
    runtime_main(BURROW_FN(Func, mixed_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&total), 23);
}

/* --- a select that does not wait at all
 *
 * A default arm is the one shape of select that never parks, so a bubble should
 * see nothing of it. Worth a check because the bubble accounting sits on the
 * park path, and a version of it that had crept up into the select itself would
 * count a goroutine as having blocked when it had not. That is the failure
 * where the bubble decides everybody is stuck while somebody is in a loop with
 * a default arm in it, and it is a deadlock report for a program that was
 * running perfectly well. */

static void default_body(void *env) {
    (void)env;

    gate = chan_make(heap_allocator(), TYPE_INT, 0);
    if (gate == NULL)
        return;

    Int v = 0;
    SelectCase cases[] = {BURROW_RECV(gate, &v), BURROW_DEFAULT};

    for (int i = 0; i < 1000; i++)
        burrow__atomic_store_release_u32(&which, (uint32_t)chan_select(cases, 2));

    burrow__atomic_store_release_u32(&ran, 1);
}

static void default_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, default_body, NULL)))
        chan_free(gate);
}

TEST(a_select_with_a_default_never_parks_in_a_bubble) {
    reset();
    runtime_main(BURROW_FN(Func, default_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&ran), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&which), 1);
}

/* --- a pipeline
 *
 * Two thousand handoffs between three goroutines, with buffered channels so
 * that both sides really do run at the same time and both sides really do block
 * when the buffer runs out or runs dry. Every one of those blocks and wakes is
 * an adjustment to the bubble's count of who can still move.
 *
 * This one is a stress test rather than a question, and it fails in the loudest
 * way there is. An adjustment lost in one direction and the bubble decides
 * every goroutine is blocked while a thousand values are still in flight, which
 * stops the program with a deadlock report. Lost in the other and the run never
 * ends. The sum is there so that a run which somehow survives both still has to
 * prove it carried the values. */

#define ITEMS 1000

static Chan *stage_one;
static Chan *stage_two;

static void producer(void *env) {
    (void)env;

    for (Int i = 1; i <= ITEMS; i++)
        chan_send(stage_one, &i);

    chan_close(stage_one);
}

static void doubler(void *env) {
    (void)env;

    Int v;
    while (chan_recv(stage_one, &v)) {
        Int out = v * 2;
        chan_send(stage_two, &out);
    }

    chan_close(stage_two);
}

static void pipeline_body(void *env) {
    (void)env;

    stage_one = chan_make(heap_allocator(), TYPE_INT, 8);
    stage_two = chan_make(heap_allocator(), TYPE_INT, 8);
    if (stage_one == NULL || stage_two == NULL)
        return;

    if (!go(BURROW_FN(Func, producer, NULL)) || !go(BURROW_FN(Func, doubler, NULL)))
        return;

    uint32_t sum = 0;
    uint32_t seen = 0;
    Int v;
    while (chan_recv(stage_two, &v)) {
        sum += (uint32_t)v;
        seen++;
    }

    burrow__atomic_store_release_u32(&total, sum);
    burrow__atomic_store_release_u32(&got, seen);

    /* Both of them have run off the end of their loops by now, so there is
     * nobody left to wait for and this returns without parking. It is here
     * because a bubble whose count drifted upwards during the run has to
     * answer for it somewhere, and this is the call that asks. */
    synctest_wait();
    burrow__atomic_store_release_u32(&ran, 1);
}

static void pipeline_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, pipeline_body, NULL))) {
        chan_free(stage_one);
        chan_free(stage_two);
    }
}

TEST(a_pipeline_of_two_thousand_handoffs_runs_through_a_bubble) {
    reset();
    stage_one = NULL;
    stage_two = NULL;
    runtime_main(BURROW_FN(Func, pipeline_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&got), ITEMS);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&total), ITEMS * (ITEMS + 1));
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&ran), 1);
}

int main(void) {
    RUN(five_hundred_goroutines_in_a_bubble_are_all_waited_for);
    RUN(a_goroutine_that_is_only_yielding_still_counts_as_running);
    RUN(a_goroutine_three_removes_from_the_body_is_still_in_the_bubble);
    RUN(a_stopped_timer_in_a_bubble_does_not_run_when_the_clock_goes_past_it);
    RUN(a_timer_reset_in_a_bubble_lands_on_the_bubble_clock);
    RUN(a_timer_that_arms_a_timer_in_a_bubble_stays_on_the_bubble_clock);
    RUN(a_select_over_bubbled_channels_is_a_durable_wait);
    RUN(a_select_with_a_case_from_outside_the_bubble_is_not_durable);
    RUN(a_select_with_a_default_never_parks_in_a_bubble);
    RUN(a_pipeline_of_two_thousand_handoffs_runs_through_a_bubble);

    return harness_report("bubble");
}
