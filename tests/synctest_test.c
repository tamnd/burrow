/* Tests for testing/synctest.
 *
 * A test for a thing that makes tests deterministic has an awkward shape,
 * because the property being tested is "this did not race" and a test that
 * passes because it got lucky looks exactly like a test that passes. So every
 * test here is written so that a broken bubble gives the wrong answer rather
 * than a slow one, and none of them sleep.
 *
 * The trick most of them use is chan_try_send on an unbuffered channel. That
 * answers true only if a receiver is parked on the channel at that instant, so
 * it is a way of asking "is the other goroutine actually blocked" that cannot
 * be satisfied by a goroutine that is merely about to be. An implementation of
 * synctest_wait that returned early would fail it every time.
 *
 * Everything a goroutine finds out it says through an atomic, and only the test
 * function itself calls CHECK, which is the rule tests/sched_test.c sets out:
 * the harness counts its checks in two plain ints and a second thread touching
 * those is a race in the test.
 *
 * The one thing not tested here is the deadlock. A bubble with nothing left
 * that can run stops the program, and it does it from the scheduler's own stack
 * inside a park, which is not somewhere a BURROW_TRY can be put. Go's version
 * of this is a fatal error for the same reason and is untestable in process for
 * the same reason.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/synctest.h"

#include "burrow/atomic.h"
#include "burrow/chan.h"
#include "burrow/clock.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "fatal.h"
#include "harness.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static Chan *bubbled_chan;
static Chan *outside_chan;
static Chan *handshake;
static Chan *release;

static uint32_t body_ran;
static uint32_t children_done;
static uint32_t receiver_was_parked;
static uint32_t wait_returned;
static uint32_t child_had_the_value;
static uint32_t caught;

static int64_t clock_at_start;
static int64_t clock_at_end;
static int64_t fired_at;
static uint32_t order;
static uint32_t ticked;
static uint32_t ticked_seen;
static uint32_t minute_was;
static uint32_t hour_was;
static uint32_t fired;
static uint32_t deadline_hit;

static SyncWaitGroup wg;
static SyncMutex cond_mu;
static SyncCond cond;

static void reset(void) {
    bubbled_chan = NULL;
    outside_chan = NULL;
    handshake = NULL;
    release = NULL;
    body_ran = 0;
    children_done = 0;
    receiver_was_parked = 0;
    wait_returned = 0;
    child_had_the_value = 0;
    caught = 0;
    clock_at_start = 0;
    clock_at_end = 0;
    fired_at = 0;
    order = 0;
    ticked = 0;
    ticked_seen = 0;
    minute_was = 0;
    hour_was = 0;
    fired = 0;
    deadline_hit = 0;
    memset(&wg, 0, sizeof(wg));
    memset(&cond_mu, 0, sizeof(cond_mu));
    memset(&cond, 0, sizeof(cond));
    memset(fatal_caught, 0, sizeof(fatal_caught));
}

/* EXPECT_FATAL's shape with the catch on a goroutine instead of on the test's
 * own thread. The two conditions below that stop the program can only be
 * reached from inside a bubble, so there is nowhere else to put the catch, and
 * what comes back to the test is one atomic and the message. */
#define CAUGHT_HERE(stmt)                                                              \
    do {                                                                               \
        BURROW_TRY {                                                                   \
            stmt;                                                                      \
        }                                                                              \
        BURROW_CATCH(p_) {                                                             \
            fatal_copy(fatal_caught, sizeof(fatal_caught), panic_text(p_));            \
            burrow__atomic_store_release_u32(&caught, 1);                              \
        }                                                                              \
        BURROW_TRY_END;                                                                \
    } while (0)

/* --------------------------------------------------------------- the basics */

static void plain_body(void *env) {
    (void)env;
    burrow__atomic_store_release_u32(&body_ran, 1);
}

static void plain_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, plain_body, NULL)))
        (void)burrow__atomic_add_u32(&children_done, 1);
}

TEST(the_run_calls_the_body) {
    reset();
    runtime_main(BURROW_FN(Func, plain_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&body_ran), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&children_done), 1);
}

/* --- the run waits for what the body started
 *
 * Every child blocks on a channel made in the bubble, the body sends each of
 * them a value and returns straight afterwards without waiting for anything.
 * The bubble is not over until the last of them has exited, so by the time
 * synctest_run answers, all of them have counted themselves. A run that
 * returned when the body did would find the count short. */

#define CHILDREN 32

static void counting_child(void *env) {
    (void)env;

    Int v;
    if (chan_recv(bubbled_chan, &v))
        (void)burrow__atomic_add_u32(&children_done, 1);
}

static void counting_body(void *env) {
    (void)env;

    bubbled_chan = chan_make(heap_allocator(), TYPE_INT, 0);
    if (bubbled_chan == NULL)
        return;

    for (int i = 0; i < CHILDREN; i++)
        (void)go(BURROW_FN(Func, counting_child, NULL));

    for (Int i = 0; i < CHILDREN; i++)
        chan_send(bubbled_chan, &i);
}

static void counting_top(void *env) {
    (void)env;

    if (!synctest_run(BURROW_FN(Func, counting_body, NULL)))
        return;

    /* Read here rather than after runtime_main, so that what is being checked
     * is the moment synctest_run returned and not the moment the runtime shut
     * down, which would have waited for the children anyway. */
    burrow__atomic_store_release_u32(&wait_returned,
                                     burrow__atomic_load_acquire_u32(&children_done));
    chan_free(bubbled_chan);
}

TEST(the_run_waits_for_the_goroutines_the_body_started) {
    reset();
    runtime_main(BURROW_FN(Func, counting_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&wait_returned), CHILDREN);
}

/* ---------------------------------------------------------------- the wait */

static void parked_receiver(void *env) {
    (void)env;

    Int v;
    if (chan_recv(bubbled_chan, &v))
        burrow__atomic_store_release_u32(&child_had_the_value, (uint32_t)v);
}

static void wait_body(void *env) {
    (void)env;

    bubbled_chan = chan_make(heap_allocator(), TYPE_INT, 0);
    if (bubbled_chan == NULL)
        return;

    (void)go(BURROW_FN(Func, parked_receiver, NULL));
    synctest_wait();

    /* An unbuffered send that does not wait succeeds only if a receiver is
     * parked on the channel right now. That is the whole claim synctest_wait
     * makes, in one call and with nothing timing dependent about it. */
    Int v = 11;
    if (chan_try_send(bubbled_chan, &v))
        burrow__atomic_store_release_u32(&receiver_was_parked, 1);
}

static void wait_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, wait_body, NULL)))
        chan_free(bubbled_chan);
}

TEST(the_wait_returns_once_the_others_are_blocked) {
    reset();
    runtime_main(BURROW_FN(Func, wait_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&receiver_was_parked), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&child_had_the_value), 11);
}

static void lonely_body(void *env) {
    (void)env;

    /* A bubble of one has nothing to wait for, and twice to make sure the
     * second one is not looking at anything the first one left behind. */
    synctest_wait();
    synctest_wait();
    burrow__atomic_store_release_u32(&body_ran, 1);
}

static void lonely_top(void *env) {
    (void)env;
    (void)synctest_run(BURROW_FN(Func, lonely_body, NULL));
}

TEST(a_wait_with_nothing_else_in_the_bubble_returns) {
    reset();
    runtime_main(BURROW_FN(Func, lonely_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&body_ran), 1);
}

/* --- a channel from outside the bubble is not a durable wait
 *
 * The child blocks on a channel made outside the bubble, which somebody out
 * there is holding the other end of, so the bubble must not count it as stuck.
 * synctest_wait therefore has to keep waiting until the outsider has sent and
 * the child has finished with it.
 *
 * A bubble that got this wrong would decide the child was durably blocked, the
 * wait would return before the outsider had done anything, and the value the
 * body reads back would be the one the child had not been given yet. So the
 * failure is a wrong answer rather than a hang, which is what makes this worth
 * having. */

static void outsider(void *env) {
    (void)env;

    Int v;
    if (!chan_recv(handshake, &v))
        return;

    v = 23;
    chan_send(outside_chan, &v);
}

static void outside_waiter(void *env) {
    (void)env;

    Int v;
    if (chan_recv(outside_chan, &v))
        burrow__atomic_store_release_u32(&child_had_the_value, (uint32_t)v);
}

static void outside_body(void *env) {
    (void)env;

    (void)go(BURROW_FN(Func, outside_waiter, NULL));

    /* Unbuffered, so this returns only once the outsider has the value, which
     * puts the outsider on its way to the send before the wait starts. */
    Int v = 1;
    chan_send(handshake, &v);

    synctest_wait();
    burrow__atomic_store_release_u32(
        &wait_returned, burrow__atomic_load_acquire_u32(&child_had_the_value));
}

static void outside_top(void *env) {
    (void)env;

    handshake = chan_make(heap_allocator(), TYPE_INT, 0);
    outside_chan = chan_make(heap_allocator(), TYPE_INT, 0);
    if (handshake == NULL || outside_chan == NULL)
        return;

    (void)go(BURROW_FN(Func, outsider, NULL));
    (void)synctest_run(BURROW_FN(Func, outside_body, NULL));

    chan_free(handshake);
    chan_free(outside_chan);
}

TEST(a_wait_on_a_channel_from_outside_the_bubble_is_not_durable) {
    reset();
    runtime_main(BURROW_FN(Func, outside_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&wait_returned), 23);
}

/* --------------------------------------------------- the rest of sync
 *
 * A WaitGroup whose work was all added from inside the bubble, and a Cond,
 * both of which are durable waits, so synctest_wait has to come back while a
 * goroutine is sitting in one.
 *
 * Both tests hang rather than fail if the durability is lost, which is the one
 * shape a missing durability can have: the bubble never goes idle and the wait
 * in the body never returns. There is no arrangement that turns it into a wrong
 * answer, because the whole claim is about a wait that has to end.
 *
 * There is no test here for a mutex, which is the other way round and is not
 * durable. A bubble whose goroutines are all waiting for a mutex has to keep
 * running, and every way of writing that down needs the test to know when the
 * waiter has parked, which is the thing a mutex gives no way to ask. There is
 * one in tests/bubble_test.c that buys the answer with a real sleep from
 * outside the bubble, which is the only way to get it. */

static void wg_worker(void *env) {
    (void)env;

    Int v;
    (void)chan_recv(bubbled_chan, &v);
    sync_wait_group_done(&wg);
}

static void wg_waiter(void *env) {
    (void)env;

    sync_wait_group_wait(&wg);
    burrow__atomic_store_release_u32(&wait_returned, 1);
}

static void wg_body(void *env) {
    (void)env;

    bubbled_chan = chan_make(heap_allocator(), TYPE_INT, 0);
    if (bubbled_chan == NULL)
        return;

    /* Added from in here, which is what makes the group this bubble's and the
     * Wait below a durable one. */
    sync_wait_group_add(&wg, 1);
    (void)go(BURROW_FN(Func, wg_worker, NULL));
    (void)go(BURROW_FN(Func, wg_waiter, NULL));

    /* Comes back only when both of them are durably blocked: the worker on a
     * bubbled channel and the waiter on the group. */
    synctest_wait();

    if (burrow__atomic_load_acquire_u32(&wait_returned) == 0)
        burrow__atomic_store_release_u32(&child_had_the_value, 1);

    Int v = 5;
    if (chan_try_send(bubbled_chan, &v))
        burrow__atomic_store_release_u32(&receiver_was_parked, 1);
}

static void wg_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, wg_body, NULL)))
        chan_free(bubbled_chan);
}

TEST(a_wait_group_wait_inside_the_bubble_is_durable) {
    reset();
    runtime_main(BURROW_FN(Func, wg_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&receiver_was_parked), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&child_had_the_value), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&wait_returned), 1);
}

static void cond_waiter(void *env) {
    (void)env;

    sync_mutex_lock(&cond_mu);
    while (burrow__atomic_load_acquire_u32(&child_had_the_value) == 0)
        sync_cond_wait(&cond);
    sync_mutex_unlock(&cond_mu);

    burrow__atomic_store_release_u32(&wait_returned, 1);
}

static void cond_body(void *env) {
    (void)env;

    cond = SYNC_COND(sync_mutex_locker(&cond_mu));
    (void)go(BURROW_FN(Func, cond_waiter, NULL));

    synctest_wait();

    if (burrow__atomic_load_acquire_u32(&wait_returned) == 0)
        burrow__atomic_store_release_u32(&receiver_was_parked, 1);

    sync_mutex_lock(&cond_mu);
    burrow__atomic_store_release_u32(&child_had_the_value, 1);
    sync_cond_broadcast(&cond);
    sync_mutex_unlock(&cond_mu);
}

static void cond_top(void *env) {
    (void)env;
    (void)synctest_run(BURROW_FN(Func, cond_body, NULL));
}

TEST(a_cond_wait_inside_the_bubble_is_durable) {
    reset();
    runtime_main(BURROW_FN(Func, cond_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&receiver_was_parked), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&wait_returned), 1);
}

/* ------------------------------------------------------------ the refusals */

static void nested_inner(void *env) {
    (void)env;
}

static void nested_body(void *env) {
    (void)env;
    CAUGHT_HERE((void)synctest_run(BURROW_FN(Func, nested_inner, NULL)));
}

static void nested_top(void *env) {
    (void)env;
    (void)synctest_run(BURROW_FN(Func, nested_body, NULL));
}

TEST(bubbles_do_not_nest) {
    reset();
    runtime_set_fatal_handler(fatal_handler);
    runtime_main(BURROW_FN(Func, nested_top, NULL));
    runtime_set_fatal_handler(NULL);

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&caught), 1);
    CHECK_STR_EQ(fatal_caught, "synctest_run: bubbles do not nest");
}

/* --- a channel made in a bubble belongs to it
 *
 * The body publishes a channel it made and then waits, on a channel from
 * outside the bubble so that the bubble does not go idle, while a goroutine
 * that is in no bubble at all tries to send on the published one. That is the
 * one thing Go makes fatal about bubbled channels, and it is fatal here for the
 * same reason: the send would be a value arriving in a test from somewhere the
 * test cannot see. */

static void misuser(void *env) {
    (void)env;

    Int v;
    if (!chan_recv(handshake, &v))
        return;

    Int x = 1;
    CAUGHT_HERE((void)chan_try_send(bubbled_chan, &x));

    v = 1;
    chan_send(release, &v);
}

static void misuse_body(void *env) {
    (void)env;

    bubbled_chan = chan_make(heap_allocator(), TYPE_INT, 0);
    if (bubbled_chan == NULL)
        return;

    Int v = 1;
    chan_send(handshake, &v);
    (void)chan_recv(release, &v);

    chan_free(bubbled_chan);
}

static void misuse_top(void *env) {
    (void)env;

    handshake = chan_make(heap_allocator(), TYPE_INT, 0);
    release = chan_make(heap_allocator(), TYPE_INT, 0);
    if (handshake == NULL || release == NULL)
        return;

    (void)go(BURROW_FN(Func, misuser, NULL));
    (void)synctest_run(BURROW_FN(Func, misuse_body, NULL));

    chan_free(handshake);
    chan_free(release);
}

TEST(a_channel_made_in_a_bubble_cannot_be_used_from_outside_it) {
    reset();
    runtime_set_fatal_handler(fatal_handler);
    runtime_main(BURROW_FN(Func, misuse_top, NULL));
    runtime_set_fatal_handler(NULL);

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&caught), 1);
    CHECK_STR_EQ(fatal_caught, "chan: a channel made inside a synctest bubble was "
                               "used from outside it");
}

/* --- a WaitGroup belongs to the bubble that added to it
 *
 * Same shape as the channel above. The body claims the group by adding to it
 * from inside the bubble, then holds still on a channel from outside while a
 * goroutine in no bubble at all adds to the same group. Go stops the program
 * for this and so does burrow, because a Wait that counted as durable while
 * somebody outside could still call Done would be a synctest_wait answering a
 * question it cannot answer. */

static void wg_misuser(void *env) {
    (void)env;

    Int v;
    if (!chan_recv(handshake, &v))
        return;

    CAUGHT_HERE(sync_wait_group_add(&wg, 1));

    v = 1;
    chan_send(release, &v);
}

static void wg_misuse_body(void *env) {
    (void)env;

    sync_wait_group_add(&wg, 1);

    Int v = 1;
    chan_send(handshake, &v);
    (void)chan_recv(release, &v);

    sync_wait_group_done(&wg);
}

static void wg_misuse_top(void *env) {
    (void)env;

    handshake = chan_make(heap_allocator(), TYPE_INT, 0);
    release = chan_make(heap_allocator(), TYPE_INT, 0);
    if (handshake == NULL || release == NULL)
        return;

    (void)go(BURROW_FN(Func, wg_misuser, NULL));
    (void)synctest_run(BURROW_FN(Func, wg_misuse_body, NULL));

    chan_free(handshake);
    chan_free(release);
}

TEST(a_wait_group_cannot_be_added_to_from_inside_and_outside_a_bubble) {
    reset();
    runtime_set_fatal_handler(fatal_handler);
    runtime_main(BURROW_FN(Func, wg_misuse_top, NULL));
    runtime_set_fatal_handler(NULL);

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&caught), 1);
    CHECK_STR_EQ(fatal_caught, "sync: WaitGroup.Add called from inside and outside "
                               "a synctest bubble");
}

/* ------------------------------------------------------------------ the clock
 *
 * A bubble has a clock of its own, and the rule it follows is the same rule the
 * rest of synctest follows: it moves only when nothing in the bubble can move,
 * and then it jumps straight to the next timer that is due.
 *
 * So a sleep of an hour in here is free, and the tests below are the three
 * things that has to mean. The sleep has to come back, having taken no real
 * time. Two sleeps have to come back in the order their durations say, not in
 * the order they were started. And anything else built on timers, AfterFunc
 * being the one that exists, has to be on the same clock.
 *
 * The reading at the top of a bubble is a fixed number rather than whatever the
 * machine says, which is checked here because it is a promise burrow/time.h
 * makes and a test that prints an elapsed time depends on it. */

#define BUBBLE_START ((int64_t)946684800000000000)

/* --- a sleep in a bubble is free */

static void sleep_body(void *env) {
    (void)env;

    clock_at_start = burrow_nanotime();
    time_sleep(TIME_HOUR);
    clock_at_end = burrow_nanotime();
}

static void sleep_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, sleep_body, NULL)))
        (void)burrow__atomic_add_u32(&children_done, 1);
}

TEST(a_sleep_inside_a_bubble_costs_no_real_time) {
    reset();

    int64_t before = burrow__nanotime();
    runtime_main(BURROW_FN(Func, sleep_top, NULL));
    int64_t really_took = burrow__nanotime() - before;

    CHECK_INT_EQ((Int)burrow__atomic_load_acquire_u32(&children_done), 1);
    CHECK(clock_at_start == BUBBLE_START);
    CHECK(clock_at_end - clock_at_start == TIME_HOUR);

    /* Five seconds rather than something tight, because the number that has to
     * be wrong for this to be a real sleep is an hour. Anything in this range
     * is the cost of starting a runtime. */
    CHECK(really_took < 5 * TIME_SECOND);
}

/* --- two sleeps come back in the order their durations say
 *
 * The hour is started first and has to finish last, which is the whole of what
 * a fake clock has to get right and is the thing a clock that simply returned
 * from every sleep at once would fail. */

static void hour_sleeper(void *env) {
    (void)env;

    time_sleep(TIME_HOUR);
    hour_was = burrow__atomic_add_u32(&order, 1);
    sync_wait_group_done(&wg);
}

static void minute_sleeper(void *env) {
    (void)env;

    time_sleep(TIME_MINUTE);
    minute_was = burrow__atomic_add_u32(&order, 1);
    sync_wait_group_done(&wg);
}

static void order_body(void *env) {
    (void)env;

    clock_at_start = burrow_nanotime();

    sync_wait_group_add(&wg, 2);
    (void)go(BURROW_FN(Func, hour_sleeper, NULL));
    (void)go(BURROW_FN(Func, minute_sleeper, NULL));
    sync_wait_group_wait(&wg);

    clock_at_end = burrow_nanotime();
}

static void order_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, order_body, NULL)))
        (void)burrow__atomic_add_u32(&children_done, 1);
}

TEST(sleeps_in_a_bubble_finish_in_the_order_their_durations_say) {
    reset();
    runtime_main(BURROW_FN(Func, order_top, NULL));

    CHECK_INT_EQ((Int)burrow__atomic_load_acquire_u32(&children_done), 1);
    CHECK_INT_EQ((Int)minute_was, 0);
    CHECK_INT_EQ((Int)hour_was, 1);
    CHECK(clock_at_end - clock_at_start == TIME_HOUR);
}

/* --- synctest_sleep lets the others finish what they do at the same instant
 *
 * The worker and the body both sleep for a minute. With time_sleep alone the
 * body could look before the worker has counted, and with synctest_sleep it
 * cannot, because the wait after the sleep holds the body until the worker has
 * got as far as it can. The worker yields a few times before it counts, which
 * is a window the body walks straight through without the wait. */

static void ticker(void *env) {
    (void)env;

    time_sleep(TIME_MINUTE);
    for (int i = 0; i < 10; i++)
        runtime_gosched();
    (void)burrow__atomic_add_u32(&ticked, 1);
}

static void settle_body(void *env) {
    (void)env;

    clock_at_start = burrow_nanotime();
    (void)go(BURROW_FN(Func, ticker, NULL));
    synctest_sleep(TIME_MINUTE);
    ticked_seen = burrow__atomic_load_acquire_u32(&ticked);
    clock_at_end = burrow_nanotime();
}

static void settle_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, settle_body, NULL)))
        (void)burrow__atomic_add_u32(&children_done, 1);
}

TEST(a_synctest_sleep_returns_after_the_others_have_settled) {
    reset();
    runtime_main(BURROW_FN(Func, settle_top, NULL));

    CHECK_INT_EQ((Int)burrow__atomic_load_acquire_u32(&children_done), 1);
    CHECK_INT_EQ((Int)ticked_seen, 1);
    CHECK(clock_at_end - clock_at_start == TIME_MINUTE);
}

/* --- AfterFunc is on the bubble's clock too
 *
 * The body arms a callback for fifty milliseconds and then sleeps for a second,
 * so the clock has to stop at the callback on its way. By the time the sleep
 * returns the callback has not only fired but finished, because the clock could
 * not have moved on to the second while the goroutine it started was still
 * running. That is a thing a real clock cannot promise at all. */

static void after_fires(void *env) {
    (void)env;

    fired_at = burrow_nanotime();
    burrow__atomic_store_release_u32(&fired, 1);
}

static void after_body(void *env) {
    (void)env;

    clock_at_start = burrow_nanotime();

    TimeTimer *t = time_after_func(heap_allocator(), 50 * TIME_MILLISECOND,
                                   BURROW_FN(Func, after_fires, NULL));
    if (t == NULL)
        return;

    time_sleep(TIME_SECOND);
    clock_at_end = burrow_nanotime();

    time_timer_free(t);
}

static void after_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, after_body, NULL)))
        (void)burrow__atomic_add_u32(&children_done, 1);
}

TEST(an_after_func_in_a_bubble_runs_on_the_bubble_clock) {
    reset();
    runtime_main(BURROW_FN(Func, after_top, NULL));

    CHECK_INT_EQ((Int)burrow__atomic_load_acquire_u32(&children_done), 1);
    CHECK_INT_EQ((Int)burrow__atomic_load_acquire_u32(&fired), 1);
    CHECK(clock_at_start == BUBBLE_START);
    CHECK(fired_at == BUBBLE_START + 50 * TIME_MILLISECOND);
    CHECK(clock_at_end == BUBBLE_START + TIME_SECOND);
}

/* --- a context deadline is on the bubble's clock as well
 *
 * Nothing in burrow/context.h knows what a bubble is. It arms a timer, the
 * timer goes into whichever set the goroutine arming it belongs to, and inside
 * a bubble that is the bubble's set. This is the test that the rest of the
 * library gets the clock for free, and it is the shape a real test for a
 * timeout wants: thirty seconds of deadline and no thirty second test.
 *
 * The wait is on the context's own channel and not a sleep of the same length.
 * Two timers due at the same instant both run, but the goroutine one of them
 * wakes is free to start on another thread before the other has run, so a sleep
 * that ended exactly at the deadline would be a race. That is true in Go too,
 * and the answer in both is to wait for the thing rather than for the clock. */

static void deadline_body(void *env) {
    (void)env;

    ContextCancelFunc cancel;
    Context ctx = context_with_timeout(heap_allocator(), context_background(),
                                       30 * TIME_SECOND, &cancel);

    clock_at_start = burrow_nanotime();

    Int v;
    (void)chan_recv(context_done(ctx), &v);
    clock_at_end = burrow_nanotime();

    if (errors_is(context_err(ctx), context_deadline_exceeded))
        burrow__atomic_store_release_u32(&deadline_hit, 1);

    BURROW_CALLF0(cancel);
    context_release(ctx);
}

static void deadline_top(void *env) {
    (void)env;

    if (synctest_run(BURROW_FN(Func, deadline_body, NULL)))
        (void)burrow__atomic_add_u32(&children_done, 1);
}

TEST(a_context_deadline_in_a_bubble_is_on_the_bubble_clock) {
    reset();
    runtime_main(BURROW_FN(Func, deadline_top, NULL));

    CHECK_INT_EQ((Int)burrow__atomic_load_acquire_u32(&children_done), 1);
    CHECK_INT_EQ((Int)burrow__atomic_load_acquire_u32(&deadline_hit), 1);
    CHECK(clock_at_end - clock_at_start == 30 * TIME_SECOND);
}

/* --- the two that need no runtime at all */

TEST(a_wait_outside_a_bubble_stops_the_program) {
    reset();
    CHECK_FATAL(synctest_wait(), "synctest_wait: not inside a bubble");
}

TEST(a_run_outside_a_goroutine_stops_the_program) {
    reset();
    CHECK_FATAL((void)synctest_run(BURROW_FN(Func, plain_body, NULL)),
                "synctest_run: not on a goroutine");
}

int main(void) {
    RUN(the_run_calls_the_body);
    RUN(the_run_waits_for_the_goroutines_the_body_started);
    RUN(the_wait_returns_once_the_others_are_blocked);
    RUN(a_wait_with_nothing_else_in_the_bubble_returns);
    RUN(a_wait_on_a_channel_from_outside_the_bubble_is_not_durable);
    RUN(bubbles_do_not_nest);
    RUN(a_wait_group_wait_inside_the_bubble_is_durable);
    RUN(a_cond_wait_inside_the_bubble_is_durable);
    RUN(a_channel_made_in_a_bubble_cannot_be_used_from_outside_it);
    RUN(a_wait_group_cannot_be_added_to_from_inside_and_outside_a_bubble);
    RUN(a_sleep_inside_a_bubble_costs_no_real_time);
    RUN(sleeps_in_a_bubble_finish_in_the_order_their_durations_say);
    RUN(a_synctest_sleep_returns_after_the_others_have_settled);
    RUN(an_after_func_in_a_bubble_runs_on_the_bubble_clock);
    RUN(a_context_deadline_in_a_bubble_is_on_the_bubble_clock);
    RUN(a_wait_outside_a_bubble_stops_the_program);
    RUN(a_run_outside_a_goroutine_stops_the_program);

    return harness_report("synctest");
}
