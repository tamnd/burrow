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
 * Roadmap steps 4 through 8 are here: the scheduler, the timers, the channels,
 * defer and panic, and the sync package. Step 9 is the netpoller and it is in
 * tests/bubble_netpoll_test.c, because everything in it needs a readiness
 * backend and half the platforms burrow builds for do not have one.
 *
 * The rules are tests/synctest_test.c's rules, because the difficulty is the
 * same. A broken bubble has to give the wrong answer rather than a slow one, so
 * the waiting is done with chan_try_send on an unbuffered channel, which is
 * true only if a receiver is parked on it at that instant. Only goroutines call
 * anything concurrent, and only the test function itself calls CHECK.
 *
 * One test sleeps for real and says why where it is. It is the one about a
 * mutex, which is the only wait in the runtime that a test cannot ask about,
 * and twenty milliseconds of somebody else's time is what it costs to have the
 * test at all.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/synctest.h"

#include "burrow/atomic.h"
#include "burrow/chan.h"
#include "burrow/defer.h"
#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "check.h"
#include "fatal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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

static void TestFiveHundredGoroutinesInABubbleAreAllWaitedFor(TestingT *t) {
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

static void TestAGoroutineThatIsOnlyYieldingStillCountsAsRunning(TestingT *t) {
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

static void TestAGoroutineThreeRemovesFromTheBodyIsStillInTheBubble(TestingT *t) {
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

static void TestAStoppedTimerInABubbleDoesNotRunWhenTheClockGoesPastIt(TestingT *t) {
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

static void TestATimerResetInABubbleLandsOnTheBubbleClock(TestingT *t) {
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

static void TestATimerThatArmsATimerInABubbleStaysOnTheBubbleClock(TestingT *t) {
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

static void TestASelectOverBubbledChannelsIsADurableWait(TestingT *t) {
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

static void TestASelectWithACaseFromOutsideTheBubbleIsNotDurable(TestingT *t) {
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

static void TestASelectWithADefaultNeverParksInABubble(TestingT *t) {
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

static void TestAPipelineOfTwoThousandHandoffsRunsThroughABubble(TestingT *t) {
    reset();
    stage_one = NULL;
    stage_two = NULL;
    runtime_main(BURROW_FN(Func, pipeline_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&got), ITEMS);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&total), ITEMS * (ITEMS + 1));
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&ran), 1);
}

/* ------------------------------------------- 7. defer, panic and recover */

/* --- a panic with a park on each side of it
 *
 * A panic unwinds a goroutine's stack by jumping back to a frame it has already
 * left, and a bubble's count of that goroutine lives in the scheduler rather
 * than on the stack, so the two have to not know about each other. The way to
 * find out that they do is to park, panic, recover, and park again, and see
 * whether the bubble still knows the goroutine is there for the second one.
 *
 * The first park matters as much as the panic. It puts the goroutine through
 * the scheduler and onto another thread before anything unwinds, so what the
 * recover is picking up is a stack that has been moved rather than one that has
 * only ever run. */

static void raise_it(void *env) {
    (void)env;
    panic_str(BURROW_S("a panic in a bubble"));
}

static void panic_child(void *env) {
    (void)env;

    Int v;
    if (!chan_recv(gate, &v))
        return;

    EXPECT_PANIC(raise_it(NULL));

    if (chan_recv(other, &v))
        burrow__atomic_store_release_u32(&got, (uint32_t)v);
}

static void panic_body(void *env) {
    (void)env;

    gate = chan_make(heap_allocator(), TYPE_INT, 0);
    other = chan_make(heap_allocator(), TYPE_INT, 0);
    if (gate == NULL || other == NULL)
        return;

    if (!go(BURROW_FN(Func, panic_child, NULL)))
        return;

    synctest_wait();

    Int v = 1;
    if (chan_try_send(gate, &v))
        (void)burrow__atomic_add_u32(&parked, 1);

    /* Comes back with the child parked on the second channel, which it only
     * reaches by way of the panic and the recover. */
    synctest_wait();

    v = 5;
    if (chan_try_send(other, &v))
        (void)burrow__atomic_add_u32(&parked, 1);
}

static void panic_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, panic_body, NULL))) {
        chan_free(gate);
        chan_free(other);
    }
}

static void TestAPanicAndARecoverInABubbleLeaveTheGoroutineCounted(TestingT *t) {
    reset();
    other = NULL;
    runtime_main(BURROW_FN(Func, panic_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&parked), 2);
    CHECK(fatal_did_catch);
    CHECK_STR_EQ(fatal_caught, "a panic in a bubble");
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&got), 5);
}

/* --- a deferred call that blocks
 *
 * Deferred calls run on the way out of a scope, which is a moment when the
 * goroutine is finished as far as the code reads but is very much still there
 * as far as the scheduler is concerned. A bubble that let go of a goroutine at
 * the end of its function rather than at the end of its last defer would decide
 * everybody had stopped while one of them was still inside a cleanup, and the
 * test would read its result while the cleanup was still writing it.
 *
 * So the middle of the three defers parks. The body has to wait three times:
 * once for the child to reach the end of its scope, once for it to reach the
 * park inside the second defer, and once for it to finish. The order it writes
 * down is the other half of the check, because a defer that ran on a different
 * goroutine or got skipped in the unwinding would still leave the count right
 * and the order wrong. */

static int order_log[4];
static int order_len;

static void note_one(void *env) {
    (void)env;
    order_log[order_len++] = 1;
}

static void note_two(void *env) {
    (void)env;

    Int v;
    (void)chan_recv(other, &v);
    order_log[order_len++] = 2;
}

static void note_three(void *env) {
    (void)env;
    order_log[order_len++] = 3;
}

static void defer_child(void *env) {
    (void)env;

    BURROW_SCOPE {
        BURROW_DEFER(note_one, NULL);
        BURROW_DEFER(note_two, NULL);
        BURROW_DEFER(note_three, NULL);

        Int v;
        (void)chan_recv(gate, &v);
    }
    BURROW_SCOPE_END;

    burrow__atomic_store_release_u32(&ran, 1);
}

static void defer_body(void *env) {
    (void)env;

    gate = chan_make(heap_allocator(), TYPE_INT, 0);
    other = chan_make(heap_allocator(), TYPE_INT, 0);
    if (gate == NULL || other == NULL)
        return;

    if (!go(BURROW_FN(Func, defer_child, NULL)))
        return;

    synctest_wait();

    Int v = 1;
    if (chan_try_send(gate, &v))
        (void)burrow__atomic_add_u32(&parked, 1);

    synctest_wait();

    if (chan_try_send(other, &v))
        (void)burrow__atomic_add_u32(&parked, 1);

    /* Nothing left to be blocked, so this comes back once the child has run its
     * last defer and gone. */
    synctest_wait();
    burrow__atomic_store_release_u32(&total, burrow__atomic_load_acquire_u32(&ran));
}

static void defer_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, defer_body, NULL))) {
        chan_free(gate);
        chan_free(other);
    }
}

static void TestADeferredCallThatParksInABubbleIsStillWaitedFor(TestingT *t) {
    reset();
    other = NULL;
    order_len = 0;
    for (int i = 0; i < 4; i++)
        order_log[i] = 0;

    runtime_main(BURROW_FN(Func, defer_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&parked), 2);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&total), 1);
    CHECK_INT_EQ(order_len, 3);
    CHECK_INT_EQ(order_log[0], 3);
    CHECK_INT_EQ(order_log[1], 2);
    CHECK_INT_EQ(order_log[2], 1);
}

/* --------------------------------------------------- 8. the sync package */

#define WORKERS 64

static SyncWaitGroup wg;
static SyncMutex mu;
static SyncRWMutex rw;
static SyncOnce once;
static SyncMutex cond_mu;
static SyncCond cond;

/* Under mu and under rw respectively, so neither is an atomic and that is the
 * point: a lock that let two goroutines in at once would show up here as a
 * count that is short rather than as anything the sanitisers would have to be
 * running to catch. */
static int guarded;
static int shared;

/* --- a mutex under contention
 *
 * Sixty four goroutines taking the same lock a hundred times each, which is six
 * thousand four hundred trips through the semaphore, most of them contended.
 * None of those waits is durable, so for the whole of this the bubble has
 * goroutines that are blocked and has to keep saying it is not idle. A bubble
 * that called a lock wait durable would decide everybody had stopped and report
 * a deadlock in a program that was making perfectly good progress.
 *
 * The Wait at the end is durable, because the Add that put the work in happened
 * in here. So this is both answers in one test: the lock is not a durable wait
 * and the group is. */

static void locker_child(void *env) {
    (void)env;

    for (int i = 0; i < 100; i++) {
        sync_mutex_lock(&mu);
        guarded++;
        sync_mutex_unlock(&mu);
    }

    sync_wait_group_done(&wg);
}

static void mutex_body(void *env) {
    (void)env;

    sync_wait_group_add(&wg, WORKERS);
    for (int i = 0; i < WORKERS; i++)
        if (!go(BURROW_FN(Func, locker_child, NULL))) {
            sync_wait_group_add(&wg, -(WORKERS - i));
            break;
        }

    sync_wait_group_wait(&wg);
    burrow__atomic_store_release_u32(&total, (uint32_t)guarded);

    synctest_wait();
    burrow__atomic_store_release_u32(&ran, 1);
}

static void mutex_top(void *env) {
    (void)env;
    (void)synctest_run(BURROW_FN(Func, mutex_body, NULL));
}

static void TestSixtyFourGoroutinesCanContendForAMutexInABubble(TestingT *t) {
    reset();
    guarded = 0;
    memset(&wg, 0, sizeof(wg));
    memset(&mu, 0, sizeof(mu));

    runtime_main(BURROW_FN(Func, mutex_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&total), WORKERS * 100);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&ran), 1);
}

/* --- a mutex wait is not durable
 *
 * tests/synctest_test.c says there is no test for this and says why: every way
 * of writing it down needs the test to know when the waiter has parked, and a
 * mutex is the one wait in the runtime that gives no way to ask. A channel has
 * chan_try_send, a WaitGroup has its counter, a Cond has the flag its waiters
 * are looking at. A mutex has nothing.
 *
 * So this one buys the answer with twenty milliseconds of real time, from a
 * goroutine outside the bubble where the clock is the machine's. The body holds
 * the lock, a goroutine in the bubble blocks on it, and the outsider waits long
 * enough for that to have certainly happened before letting go. If the wait
 * were durable, synctest_wait would come back during those twenty milliseconds
 * with the child still stuck and the value it writes still zero.
 *
 * Unlocking from a goroutine other than the one that locked is allowed, here
 * and in Go. A mutex is not owned by a goroutine, and a handoff like this one is
 * why. */

static uint32_t released;

static void outside_unlocker(void *env) {
    (void)env;

    Int v;
    if (!chan_recv(handshake, &v))
        return;

    time_sleep(20 * TIME_MILLISECOND);
    burrow__atomic_store_release_u32(&released, 1);
    sync_mutex_unlock(&mu);
}

static void mutex_waiter(void *env) {
    (void)env;

    sync_mutex_lock(&mu);
    burrow__atomic_store_release_u32(&got, 1);
    sync_mutex_unlock(&mu);
}

static void not_durable_body(void *env) {
    (void)env;

    sync_mutex_lock(&mu);
    if (!go(BURROW_FN(Func, mutex_waiter, NULL))) {
        sync_mutex_unlock(&mu);
        return;
    }

    /* Unbuffered, so the outsider has it before this returns and its twenty
     * milliseconds start now rather than whenever it gets scheduled. */
    Int v = 1;
    chan_send(handshake, &v);

    synctest_wait();
    burrow__atomic_store_release_u32(&total, burrow__atomic_load_acquire_u32(&got));
}

static void not_durable_top(void *env) {
    (void)env;

    handshake = chan_make(heap_allocator(), TYPE_INT, 0);
    if (handshake == NULL)
        return;

    if (go(BURROW_FN(Func, outside_unlocker, NULL)))
        (void)synctest_run(BURROW_FN(Func, not_durable_body, NULL));

    chan_free(handshake);
}

static void TestAMutexWaitInABubbleIsNotDurable(TestingT *t) {
    reset();
    released = 0;
    memset(&mu, 0, sizeof(mu));

    runtime_main(BURROW_FN(Func, not_durable_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&released), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&total), 1);
}

/* --- readers and writers
 *
 * Eight readers and two writers on the same lock, which is the other shape of
 * semaphore wait: a writer queues behind readers and readers queue behind a
 * writer, and both of those are parks the bubble has to count as running.
 *
 * The invariant is that the shared number is always a multiple of seven,
 * because that is the only thing a writer ever adds to it and it does so under
 * the write lock. A reader that got in while a writer was halfway through would
 * see a number that is not, and there is nowhere else for such a number to come
 * from. */

#define READERS 8
#define WRITERS 2
#define ROUNDS 50

static void reader_child(void *env) {
    (void)env;

    for (int i = 0; i < ROUNDS; i++) {
        sync_rw_mutex_r_lock(&rw);
        if (shared % 7 != 0)
            burrow__atomic_store_release_u32(&which, 1);
        sync_rw_mutex_r_unlock(&rw);
    }

    sync_wait_group_done(&wg);
}

static void writer_child(void *env) {
    (void)env;

    for (int i = 0; i < ROUNDS; i++) {
        sync_rw_mutex_lock(&rw);
        shared += 7;
        sync_rw_mutex_unlock(&rw);
    }

    sync_wait_group_done(&wg);
}

static void rw_body(void *env) {
    (void)env;

    sync_wait_group_add(&wg, READERS + WRITERS);

    for (int i = 0; i < READERS; i++)
        if (!go(BURROW_FN(Func, reader_child, NULL)))
            return;
    for (int i = 0; i < WRITERS; i++)
        if (!go(BURROW_FN(Func, writer_child, NULL)))
            return;

    sync_wait_group_wait(&wg);
    burrow__atomic_store_release_u32(&total, (uint32_t)shared);
}

static void rw_top(void *env) {
    (void)env;
    (void)synctest_run(BURROW_FN(Func, rw_body, NULL));
}

static void TestReadersAndWritersShareALockInABubble(TestingT *t) {
    reset();
    shared = 0;
    memset(&wg, 0, sizeof(wg));
    memset(&rw, 0, sizeof(rw));

    runtime_main(BURROW_FN(Func, rw_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&which), 0);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&total), WRITERS * ROUNDS * 7);
}

/* --- a Once
 *
 * Sixty four goroutines and one function, and the sixty three that lose the
 * race block until the winner is finished. That wait is the interesting part
 * here rather than the counting: it is a park in the middle of a call that
 * looks like it does not have one, and every one of those is a place the
 * bubble's count can go missing. */

static void once_fn(void *env) {
    (void)env;
    (void)burrow__atomic_add_u32(&ran, 1);
}

static void once_child(void *env) {
    (void)env;

    sync_once_do(&once, BURROW_FN(Func, once_fn, NULL));
    (void)burrow__atomic_add_u32(&total, 1);
    sync_wait_group_done(&wg);
}

static void once_body(void *env) {
    (void)env;

    sync_wait_group_add(&wg, WORKERS);
    for (int i = 0; i < WORKERS; i++)
        if (!go(BURROW_FN(Func, once_child, NULL))) {
            sync_wait_group_add(&wg, -(WORKERS - i));
            break;
        }

    sync_wait_group_wait(&wg);
}

static void once_top(void *env) {
    (void)env;
    (void)synctest_run(BURROW_FN(Func, once_body, NULL));
}

static void TestAOnceInABubbleRunsItsFunctionOnce(TestingT *t) {
    reset();
    memset(&wg, 0, sizeof(wg));
    memset(&once, 0, sizeof(once));

    runtime_main(BURROW_FN(Func, once_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&ran), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&total), WORKERS);
}

/* --- a broadcast
 *
 * tests/synctest_test.c has one goroutine in a Cond wait and checks that the
 * bubble calls it durably blocked. This is the same question asked of eight of
 * them at once, which is worth asking separately because a broadcast wakes them
 * all from inside the notify list and the bubble has to count eight goroutines
 * coming back rather than one.
 *
 * The first wait is the claim that all eight are really in there. Nobody has
 * incremented anything at that point, and a bubble that came back early would
 * be coming back with goroutines that had not reached the Cond yet, which is
 * the case where the broadcast below goes out to an empty list and the run
 * never ends. */

#define SLEEPERS 8

static void cond_child(void *env) {
    (void)env;

    sync_mutex_lock(&cond_mu);
    while (burrow__atomic_load_acquire_u32(&which) == 0)
        sync_cond_wait(&cond);
    sync_mutex_unlock(&cond_mu);

    (void)burrow__atomic_add_u32(&total, 1);
}

static void cond_body(void *env) {
    (void)env;

    cond = SYNC_COND(sync_mutex_locker(&cond_mu));

    for (int i = 0; i < SLEEPERS; i++)
        if (!go(BURROW_FN(Func, cond_child, NULL)))
            return;

    synctest_wait();
    if (burrow__atomic_load_acquire_u32(&total) == 0)
        burrow__atomic_store_release_u32(&parked, 1);

    sync_mutex_lock(&cond_mu);
    burrow__atomic_store_release_u32(&which, 1);
    sync_cond_broadcast(&cond);
    sync_mutex_unlock(&cond_mu);

    synctest_wait();
    burrow__atomic_store_release_u32(&got, burrow__atomic_load_acquire_u32(&total));
}

static void cond_top(void *env) {
    (void)env;
    (void)synctest_run(BURROW_FN(Func, cond_body, NULL));
}

static void TestABroadcastInABubbleWakesEveryWaiter(TestingT *t) {
    reset();
    memset(&cond_mu, 0, sizeof(cond_mu));
    memset(&cond, 0, sizeof(cond));

    runtime_main(BURROW_FN(Func, cond_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&parked), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&got), SLEEPERS);
}

/* --- a Map
 *
 * Sixteen goroutines writing sixteen keys each into the same map, which is the
 * one thing in sync that is not built on the semaphore. It has a lock of its
 * own for the slow path and reclamation underneath it that is tied to the
 * thread rather than to the goroutine, and a bubble moves goroutines between
 * threads more than an ordinary program does, because everything in one stops
 * and starts together.
 *
 * So this is a test of the reclamation as much as of the map. The count at the
 * end is what says every write landed. */

#define POSTERS 16
#define PER_POSTER 16

static SyncMap cache;

static void map_child(void *env) {
    Int base = (Int)(intptr_t)env;

    for (Int i = 0; i < PER_POSTER; i++) {
        Int k = base * PER_POSTER + i;
        Int v = k * 2;

        if (sync_map_store(&cache, &k, &v))
            (void)burrow__atomic_add_u32(&total, 1);
    }

    sync_wait_group_done(&wg);
}

static void map_body(void *env) {
    (void)env;

    cache = SYNC_MAP(heap_allocator(), TYPE_INT, TYPE_INT);
    sync_wait_group_add(&wg, POSTERS);

    for (Int i = 0; i < POSTERS; i++)
        if (!go(BURROW_FN(Func, map_child, (void *)(intptr_t)i))) {
            sync_wait_group_add(&wg, -(POSTERS - (int)i));
            break;
        }

    sync_wait_group_wait(&wg);

    uint32_t found = 0;
    for (Int k = 0; k < POSTERS * PER_POSTER; k++) {
        Int v = 0;
        if (sync_map_load(&cache, &k, &v) && v == k * 2)
            found++;
    }

    burrow__atomic_store_release_u32(&got, found);
    sync_map_free(&cache);
}

static void map_top(void *env) {
    (void)env;
    (void)synctest_run(BURROW_FN(Func, map_body, NULL));
}

static void TestASyncMapInABubbleKeepsEveryWrite(TestingT *t) {
    reset();
    memset(&wg, 0, sizeof(wg));

    runtime_main(BURROW_FN(Func, map_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&total), POSTERS * PER_POSTER);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&got), POSTERS * PER_POSTER);
}

#define TESTS(X)                                                                       \
    X(TestFiveHundredGoroutinesInABubbleAreAllWaitedFor)                               \
    X(TestAGoroutineThatIsOnlyYieldingStillCountsAsRunning)                            \
    X(TestAGoroutineThreeRemovesFromTheBodyIsStillInTheBubble)                         \
    X(TestAStoppedTimerInABubbleDoesNotRunWhenTheClockGoesPastIt)                      \
    X(TestATimerResetInABubbleLandsOnTheBubbleClock)                                   \
    X(TestATimerThatArmsATimerInABubbleStaysOnTheBubbleClock)                          \
    X(TestASelectOverBubbledChannelsIsADurableWait)                                    \
    X(TestASelectWithACaseFromOutsideTheBubbleIsNotDurable)                            \
    X(TestASelectWithADefaultNeverParksInABubble)                                      \
    X(TestAPipelineOfTwoThousandHandoffsRunsThroughABubble)                            \
    X(TestAPanicAndARecoverInABubbleLeaveTheGoroutineCounted)                          \
    X(TestADeferredCallThatParksInABubbleIsStillWaitedFor)                             \
    X(TestSixtyFourGoroutinesCanContendForAMutexInABubble)                             \
    X(TestAMutexWaitInABubbleIsNotDurable)                                             \
    X(TestReadersAndWritersShareALockInABubble)                                        \
    X(TestAOnceInABubbleRunsItsFunctionOnce)                                           \
    X(TestABroadcastInABubbleWakesEveryWaiter)                                         \
    X(TestASyncMapInABubbleKeepsEveryWrite)

TESTING_MAIN_BARE(TESTS)
