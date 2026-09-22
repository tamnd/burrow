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
#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
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
    RUN(a_channel_made_in_a_bubble_cannot_be_used_from_outside_it);
    RUN(a_wait_outside_a_bubble_stops_the_program);
    RUN(a_run_outside_a_goroutine_stops_the_program);

    return harness_report("synctest");
}
