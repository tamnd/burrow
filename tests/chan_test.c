/* Tests for channels.
 *
 * In two halves, because channels are in two halves.
 *
 * The first half does not start the runtime at all. Every channel operation
 * that does not have to wait is an ordinary function call on an ordinary data
 * structure, and testing the ring buffer, the indices, the close rules and the
 * zeroing without a scheduler underneath means a failure in any of them points
 * at the queue rather than at the queue or the scheduler or the timing.
 *
 * The second half starts the runtime and uses real goroutines, because the
 * waiting is the interesting part and there is no way to test waiting without
 * something to wait for. Those tests follow the rule tests/sched_test.c sets
 * out: only the main goroutine calls CHECK, and everything a child goroutine
 * finds out it says through an atomic, because the harness counts checks in
 * two plain ints and a second thread touching those is a race in the test.
 *
 * There is a third thing in here which Go has no equivalent of, which is a host
 * thread that is not running a goroutine blocking on a channel. burrow is a
 * library inside somebody else's program and that program has its own threads,
 * so a channel has to work from one. It costs the thread, which a goroutine
 * parking does not, and that is the only difference the caller can see.
 *
 * The three mistakes that used to be untestable are tested here now. A send on
 * a closed channel, a double close and a close of NULL were fatal errors, and a
 * harness that ends the process has failed, so there was nothing to write. They
 * panic today, so the test is a BURROW_TRY around each one and a look at what
 * came out.
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
#include <string.h>

/* ----------------------------------------------------- without a scheduler */

TEST(a_buffered_channel_is_a_queue) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 4);
    CHECK(c != NULL);

    CHECK_INT_EQ(chan_cap(c), 4);
    CHECK_INT_EQ(chan_len(c), 0);
    CHECK(chan_elem(c) == TYPE_INT);

    for (Int i = 0; i < 4; i++)
        chan_send(c, &i);

    CHECK_INT_EQ(chan_len(c), 4);

    for (Int i = 0; i < 4; i++) {
        Int got = -1;
        CHECK(chan_recv(c, &got));
        CHECK_INT_EQ(got, i);
    }

    CHECK_INT_EQ(chan_len(c), 0);
    chan_free(c);
}

/* Sends and receives alternately for long enough that both indices go round
 * several times, which is the arithmetic that a queue written with one spare
 * slot instead of a count gets wrong. */
TEST(the_ring_wraps_without_losing_its_place) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 3);
    CHECK(c != NULL);

    Int next_in = 0;
    Int next_out = 0;
    bool ok = true;

    /* Two in, one out, twenty times, so the buffer fills and the indices pass
     * the end of it repeatedly. */
    for (int round = 0; round < 20; round++) {
        while (chan_len(c) < 2 && next_in < 100) {
            chan_send(c, &next_in);
            next_in++;
        }

        Int got = -1;
        ok = ok && chan_try_recv(c, &got, NULL);
        ok = ok && got == next_out;
        next_out++;
    }

    CHECK(ok);
    CHECK_INT_EQ(chan_len(c), next_in - next_out);
    chan_free(c);
}

TEST(an_unbuffered_channel_holds_nothing) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 0);
    CHECK(c != NULL);

    CHECK_INT_EQ(chan_cap(c), 0);
    CHECK_INT_EQ(chan_len(c), 0);

    /* Nobody is waiting, so neither half of the rendezvous can happen and both
     * non-blocking calls have to say so. */
    Int v = 7;
    CHECK(!chan_try_send(c, &v));
    CHECK(!chan_try_recv(c, &v, NULL));

    chan_free(c);
}

TEST(the_non_blocking_calls_answer_instead_of_waiting) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 2);
    CHECK(c != NULL);

    Int got = -1;
    bool ok = true;

    /* Empty and open: nothing to take, and ok is left alone because nothing
     * happened at all. */
    CHECK(!chan_try_recv(c, &got, &ok));

    Int v = 1;
    CHECK(chan_try_send(c, &v));
    v = 2;
    CHECK(chan_try_send(c, &v));

    /* Full. */
    v = 3;
    CHECK(!chan_try_send(c, &v));
    CHECK_INT_EQ(chan_len(c), 2);

    CHECK(chan_try_recv(c, &got, &ok));
    CHECK(ok);
    CHECK_INT_EQ(got, 1);

    /* Room again. */
    CHECK(chan_try_send(c, &v));
    CHECK_INT_EQ(chan_len(c), 2);

    chan_free(c);
}

TEST(a_closed_channel_drains_and_then_answers_false) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 4);
    CHECK(c != NULL);

    for (Int i = 10; i < 13; i++)
        chan_send(c, &i);

    chan_close(c);

    /* The values that were already in it are still there, which is the whole
     * reason close is not the same as discard. */
    for (Int i = 10; i < 13; i++) {
        Int got = -1;
        CHECK(chan_recv(c, &got));
        CHECK_INT_EQ(got, i);
    }

    /* Drained. Every receive from here answers false and writes the zero value,
     * for ever, without blocking. */
    for (int i = 0; i < 3; i++) {
        Int got = 999;
        CHECK(!chan_recv(c, &got));
        CHECK_INT_EQ(got, 0);
    }

    bool ok = true;
    Int got = 999;
    CHECK(chan_try_recv(c, &got, &ok));
    CHECK(!ok);
    CHECK_INT_EQ(got, 0);

    chan_free(c);
}

TEST(a_closed_unbuffered_channel_answers_a_receive_at_once) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 0);
    CHECK(c != NULL);

    chan_close(c);

    Int got = 5;
    CHECK(!chan_recv(c, &got));
    CHECK_INT_EQ(got, 0);

    chan_free(c);
}

TEST(a_receive_that_does_not_want_the_value_still_takes_it) {
    Chan *c = chan_make(heap_allocator(), TYPE_INT, 2);
    CHECK(c != NULL);

    Int v = 1;
    chan_send(c, &v);
    v = 2;
    chan_send(c, &v);

    CHECK(chan_recv(c, NULL));
    CHECK_INT_EQ(chan_len(c), 1);

    Int got = -1;
    CHECK(chan_recv(c, &got));
    CHECK_INT_EQ(got, 2);

    chan_free(c);
}

/* A Str is two words and the type descriptor knows how to copy one, which is
 * why the channel calls type_copy rather than memcpy. This is the test that
 * would notice if it stopped. */
TEST(a_channel_carries_whatever_its_type_says) {
    Chan *c = chan_make(heap_allocator(), TYPE_STRING, 2);
    CHECK(c != NULL);

    Str in = BURROW_S("hello");
    chan_send(c, &in);

    Str got = BURROW_STR_EMPTY;
    CHECK(chan_recv(c, &got));
    CHECK_INT_EQ(got.len, 5);
    CHECK(got.p != NULL && memcmp(got.p, "hello", 5) == 0);

    /* And close zeroes it, rather than leaving a pointer to somebody else's
     * bytes with a length that no longer means anything. */
    chan_close(c);
    CHECK(!chan_recv(c, &got));
    CHECK_INT_EQ(got.len, 0);
    CHECK(got.p == NULL);

    chan_free(c);
}

TEST(a_nil_channel_is_empty_and_never_ready) {
    CHECK_INT_EQ(chan_len(NULL), 0);
    CHECK_INT_EQ(chan_cap(NULL), 0);
    CHECK(chan_elem(NULL) == NULL);

    Int v = 1;
    CHECK(!chan_try_send(NULL, &v));
    CHECK(!chan_try_recv(NULL, &v, NULL));

    /* And freeing it does nothing, which is the rule every free in this library
     * follows. */
    chan_free(NULL);
}

/* The header and the buffer are one allocation and chan_free gives all of it
 * back. A Track over the heap is the cheapest way to say so. */
TEST(a_channel_gives_back_everything_it_took) {
    Track tr;
    track_init(&tr, heap_allocator());
    track_set_quarantine(&tr, 0);
    Alloc *a = track_allocator(&tr);

    for (Int cap = 0; cap < 5; cap++) {
        Chan *c = chan_make(a, TYPE_INT, cap);
        CHECK(c != NULL);

        for (Int i = 0; i < cap; i++)
            chan_send(c, &i);

        chan_free(c);
    }

    CHECK_INT_EQ((Int)track_live(&tr), 0);
    track_free(&tr);
}

/* --------------------------------------------------------- the three mistakes
 *
 * Go panics on each of these and so does burrow, so each one is catchable and
 * the process carries on. The channel is file static because everything else a
 * BURROW_TRY touches in here would be a local live across a setjmp. */

static Chan *doomed;

TEST(a_send_on_a_closed_channel_panics) {
    doomed = chan_make(heap_allocator(), TYPE_INT, 1);
    CHECK(doomed != NULL);

    chan_close(doomed);

    CHECK_RUNTIME_ERROR(
        {
            Int v = 1;
            chan_send(doomed, &v);
        },
        "send on closed channel");

    /* Recovering from it leaves the channel usable, which is the part worth
     * checking: the send took the lock and let go of it before panicking, so a
     * caught panic here is not a deadlock later. */
    CHECK_INT_EQ(chan_len(doomed), 0);
    CHECK_INT_EQ(chan_cap(doomed), 1);

    chan_free(doomed);
    doomed = NULL;
}

TEST(closing_twice_panics) {
    doomed = chan_make(heap_allocator(), TYPE_INT, 0);
    CHECK(doomed != NULL);

    chan_close(doomed);
    CHECK_RUNTIME_ERROR(chan_close(doomed), "close of closed channel");
    CHECK_RUNTIME_ERROR(chan_close(doomed), "close of closed channel");

    chan_free(doomed);
    doomed = NULL;
}

TEST(closing_nothing_panics) {
    CHECK_RUNTIME_ERROR(chan_close(NULL), "close of nil channel");
}

TEST(an_absurd_capacity_panics) {
    CHECK_RUNTIME_ERROR((void)chan_make(heap_allocator(), TYPE_INT, -1),
                        "makechan: size out of range");
}

/* ------------------------------------------------------- with a scheduler
 *
 * Everything below runs inside runtime_main. The shared state is file static
 * because a goroutine's argument is one pointer and a struct per test would
 * say less than the names do. */

#define VALUES 500

static Chan *shared;
static uint32_t received;
static uint32_t sent;
static uint32_t sum;
static uint32_t woke;
static uint32_t started;
static uint32_t order_ok;

static void reset(void) {
    shared = NULL;
    received = 0;
    sent = 0;
    sum = 0;
    woke = 0;
    started = 0;
    order_ok = 0;
}

/* --- a send waits for a receiver
 *
 * On one P there is no parallelism to hide behind. The child receives, and the
 * only way it can run at all is if the send parked the main goroutine, so a
 * value arriving is a statement about the handoff and not just about the two
 * of them eventually meeting. */

static void handoff_child(void *arg) {
    (void)arg;
    Int got = -1;
    if (chan_recv(shared, &got) && got == 42)
        burrow__atomic_store_u32(&received, 1);
}

static void handoff_body(void *arg) {
    (void)arg;
    shared = chan_make(heap_allocator(), TYPE_INT, 0);
    if (shared == NULL)
        return;

    if (!go(BURROW_FN(Func, handoff_child, NULL)))
        return;

    Int v = 42;
    chan_send(shared, &v);

    /* The send has returned, so the receiver already has the value. That is the
     * unbuffered guarantee and it is checkable here without waiting for
     * anything. */
    burrow__atomic_store_u32(&sent, burrow__atomic_load_acquire_u32(&received));
}

TEST(an_unbuffered_send_does_not_return_until_somebody_has_the_value) {
    reset();
    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, handoff_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&received), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&sent), 1);

    chan_free(shared);
    (void)runtime_gomaxprocs(0);
}

/* --- one producer, one consumer
 *
 * The values come out in the order they went in and none of them are missing,
 * on a channel small enough that the producer blocks on it constantly. */

static void producer(void *arg) {
    (void)arg;
    for (Int i = 0; i < VALUES; i++)
        chan_send(shared, &i);
    chan_close(shared);
}

static void pipeline_body(void *arg) {
    (void)arg;
    shared = chan_make(heap_allocator(), TYPE_INT, 4);
    if (shared == NULL)
        return;

    if (!go(BURROW_FN(Func, producer, NULL)))
        return;

    /* Go's `for v := range c`, which is this loop. */
    Int want = 0;
    bool ordered = true;
    Int got = -1;
    while (chan_recv(shared, &got)) {
        ordered = ordered && got == want;
        want++;
    }

    burrow__atomic_store_u32(&received, (uint32_t)want);
    burrow__atomic_store_u32(&order_ok, ordered ? 1u : 0u);
}

TEST(everything_sent_arrives_in_order_and_the_close_ends_the_loop) {
    reset();
    (void)runtime_gomaxprocs(2);
    runtime_main(BURROW_FN(Func, pipeline_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&received), VALUES);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&order_ok), 1);

    chan_free(shared);
    (void)runtime_gomaxprocs(0);
}

/* --- many of each
 *
 * Four senders and four receivers on an unbuffered channel, which is the shape
 * that finds a lost wakeup. Order means nothing with several senders, so the
 * check is that every value arrived exactly once, which the sum says.
 *
 * The receivers stop when the channel closes, and the channel is closed by the
 * main goroutine once every sender has finished, because a sender must never
 * close a channel other senders are still using. */

#define WORKERS 4
#define PER_WORKER 200

static void many_sender(void *arg) {
    Int base = (Int)(intptr_t)arg * PER_WORKER;
    for (Int i = 0; i < PER_WORKER; i++) {
        Int v = base + i;
        chan_send(shared, &v);
    }
    (void)burrow__atomic_add_u32(&started, 1);
}

static void many_receiver(void *arg) {
    (void)arg;
    Int got = -1;
    while (chan_recv(shared, &got)) {
        (void)burrow__atomic_add_u32(&sum, (uint32_t)got);
        (void)burrow__atomic_add_u32(&received, 1);
    }
    (void)burrow__atomic_add_u32(&woke, 1);
}

static void many_body(void *arg) {
    (void)arg;
    shared = chan_make(heap_allocator(), TYPE_INT, 0);
    if (shared == NULL)
        return;

    for (Int i = 0; i < WORKERS; i++) {
        if (!go(BURROW_FN(Func, many_sender, (void *)(intptr_t)i)))
            return;
        if (!go(BURROW_FN(Func, many_receiver, NULL)))
            return;
    }

    /* Wait for the senders by yielding rather than by another channel, so that
     * this test depends on one channel and not on two. */
    while (burrow__atomic_load_acquire_u32(&started) < WORKERS)
        runtime_gosched();

    chan_close(shared);

    while (burrow__atomic_load_acquire_u32(&woke) < WORKERS)
        runtime_gosched();

    burrow__atomic_store_u32(&sent, 1);
}

TEST(several_senders_and_several_receivers_move_every_value_once) {
    reset();
    (void)runtime_gomaxprocs(4);
    runtime_main(BURROW_FN(Func, many_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&sent), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&received), WORKERS * PER_WORKER);

    /* 0 + 1 + ... + 799, which only comes out right if every value arrived and
     * none of them arrived twice. */
    uint32_t n = WORKERS * PER_WORKER;
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&sum), n * (n - 1) / 2);

    /* Every receiver saw the close. */
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&woke), WORKERS);

    chan_free(shared);
    (void)runtime_gomaxprocs(0);
}

/* --- close is a broadcast
 *
 * Several goroutines blocked on an empty channel, one close, and all of them
 * come back with false. A condition variable signalled once instead of
 * broadcast would leave all but one of them parked and this test would hang
 * rather than fail, which is why the harness bounds the wait. */

static void sleeper(void *arg) {
    (void)arg;
    (void)burrow__atomic_add_u32(&started, 1);

    Int got = 7;
    bool ok = chan_recv(shared, &got);
    if (!ok && got == 0)
        (void)burrow__atomic_add_u32(&woke, 1);
}

static void broadcast_body(void *arg) {
    (void)arg;
    shared = chan_make(heap_allocator(), TYPE_INT, 0);
    if (shared == NULL)
        return;

    for (int i = 0; i < WORKERS; i++) {
        if (!go(BURROW_FN(Func, sleeper, NULL)))
            return;
    }

    /* Every one of them has at least started. That is not the same as every one
     * of them being parked, and it does not need to be: a receiver that has not
     * reached the channel yet finds it closed when it gets there, and a
     * receiver that is parked gets woken. Both count. */
    while (burrow__atomic_load_acquire_u32(&started) < WORKERS)
        runtime_gosched();

    chan_close(shared);

    while (burrow__atomic_load_acquire_u32(&woke) < WORKERS)
        runtime_gosched();
}

TEST(closing_wakes_every_receiver_that_is_waiting) {
    reset();
    (void)runtime_gomaxprocs(2);
    runtime_main(BURROW_FN(Func, broadcast_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&woke), WORKERS);

    chan_free(shared);
    (void)runtime_gomaxprocs(0);
}

/* --- a buffered channel with a sender queued behind it
 *
 * The case with the wrinkle in it. The buffer is full and a sender is parked,
 * so a receive has to hand over the value at the head of the buffer and then
 * put the parked sender's value in at the tail, which is the same slot. Getting
 * that backwards gives a channel that returns values out of order only when it
 * is under pressure, which is the worst kind of bug to find later. */

static void backlog_producer(void *arg) {
    (void)arg;
    for (Int i = 0; i < VALUES; i++)
        chan_send(shared, &i);
    chan_close(shared);
}

static void backlog_body(void *arg) {
    (void)arg;
    shared = chan_make(heap_allocator(), TYPE_INT, 2);
    if (shared == NULL)
        return;

    if (!go(BURROW_FN(Func, backlog_producer, NULL)))
        return;

    /* Let the producer fill the buffer and then block, so that every receive
     * from here on takes the slot path with a queued sender behind it. */
    while (chan_len(shared) < 2)
        runtime_gosched();
    for (int i = 0; i < 100; i++)
        runtime_gosched();

    Int want = 0;
    bool ordered = true;
    Int got = -1;
    while (chan_recv(shared, &got)) {
        ordered = ordered && got == want;
        want++;
    }

    burrow__atomic_store_u32(&received, (uint32_t)want);
    burrow__atomic_store_u32(&order_ok, ordered ? 1u : 0u);
}

TEST(a_full_buffer_with_a_sender_behind_it_still_comes_out_in_order) {
    reset();
    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, backlog_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&received), VALUES);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&order_ok), 1);

    chan_free(shared);
    (void)runtime_gomaxprocs(0);
}

/* --- a thread that is not a goroutine
 *
 * The host's own thread sends on an unbuffered channel and a goroutine
 * receives. The thread has nothing to park, so it sleeps, and it is the only
 * user of the note path in the implementation.
 *
 * GOMAXPROCS is one on purpose. The runtime has one thread of its own and the
 * sender is a second one that the runtime knows nothing about, so if the two
 * sides did not really synchronise, this would deadlock rather than pass. */

#define OUTSIDE 50

static void outside_sender(void *arg) {
    (void)arg;
    for (Int i = 0; i < OUTSIDE; i++)
        chan_send(shared, &i);
    chan_close(shared);
}

static void outside_body(void *arg) {
    (void)arg;
    shared = chan_make(heap_allocator(), TYPE_INT, 0);
    if (shared == NULL)
        return;

    burrow__Thread t;
    if (!burrow__thread_start(&t, outside_sender, NULL, 0))
        return;

    Int want = 0;
    bool ordered = true;
    Int got = -1;
    while (chan_recv(shared, &got)) {
        ordered = ordered && got == want;
        want++;
    }

    (void)burrow__thread_join(&t);

    burrow__atomic_store_u32(&received, (uint32_t)want);
    burrow__atomic_store_u32(&order_ok, ordered ? 1u : 0u);
}

TEST(a_thread_that_is_not_a_goroutine_can_block_on_a_channel) {
    reset();
    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, outside_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&received), OUTSIDE);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&order_ok), 1);

    chan_free(shared);
    (void)runtime_gomaxprocs(0);
}

/* --- and the other direction
 *
 * A goroutine sends and the host thread receives, so that the note is on the
 * receiving side and the direct copy into a waiting receiver's variable is the
 * one crossing the boundary. */

static void outside_receiver(void *arg) {
    (void)arg;
    Int want = 0;
    bool ordered = true;
    Int got = -1;
    while (chan_recv(shared, &got)) {
        ordered = ordered && got == want;
        want++;
    }
    burrow__atomic_store_u32(&received, (uint32_t)want);
    burrow__atomic_store_u32(&order_ok, ordered ? 1u : 0u);
}

static void outside_recv_body(void *arg) {
    (void)arg;
    shared = chan_make(heap_allocator(), TYPE_INT, 0);
    if (shared == NULL)
        return;

    burrow__Thread t;
    if (!burrow__thread_start(&t, outside_receiver, NULL, 0))
        return;

    for (Int i = 0; i < OUTSIDE; i++)
        chan_send(shared, &i);
    chan_close(shared);

    (void)burrow__thread_join(&t);
}

TEST(a_goroutine_can_hand_a_value_to_a_thread_that_is_not_one) {
    reset();
    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, outside_recv_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&received), OUTSIDE);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&order_ok), 1);

    chan_free(shared);
    (void)runtime_gomaxprocs(0);
}

/* --- the same thing again, many times over
 *
 * Every round here parks both sides. The goroutine parks waiting for the next
 * value, which leaves the runtime's one thread with nothing to run, and the
 * host thread parks waiting for the answer. So the runtime's thread goes to
 * sleep and is woken by a thread it knows nothing about, over and over, which
 * is the handover the two tests above do fifty times and this one does enough
 * times to catch it being wrong once in a while.
 *
 * It is here because it found a real one. A thread on its way to sleep gives up
 * its P, takes one more look for work, and only then joins the list of threads
 * that can be handed a P. Work arriving in the gap between the look and the
 * list was work nobody ever came back for, and the whole program stopped. Fifty
 * rounds almost never lands in that gap. Twenty thousand does.
 *
 * The same twenty thousand under a thread sanitizer, which took a fix in
 * src/runtime/context.c to be possible and is worth keeping an eye on: this is
 * the test that goes first when the sanitizer stops being told about goroutine
 * switches, and what it does then is die inside the sanitizer rather than fail
 * anything here. It costs about five seconds. */

#ifndef ROUNDS
#define ROUNDS 20000
#endif

static Chan *pong;
static uint32_t rounds_done;

static void pingpong_host(void *arg) {
    (void)arg;
    for (Int i = 0; i < ROUNDS; i++) {
        Int back = -1;
        chan_send(shared, &i);
        if (!chan_recv(pong, &back) || back != i + 1)
            return;
        burrow__atomic_store_u32(&rounds_done, (uint32_t)(i + 1));
    }
    chan_close(shared);
}

static void pingpong_body(void *arg) {
    (void)arg;
    shared = chan_make(heap_allocator(), TYPE_INT, 0);
    pong = chan_make(heap_allocator(), TYPE_INT, 0);
    if (shared == NULL || pong == NULL)
        return;

    burrow__Thread t;
    if (!burrow__thread_start(&t, pingpong_host, NULL, 0))
        return;

    Int got = -1;
    while (chan_recv(shared, &got)) {
        Int answer = got + 1;
        chan_send(pong, &answer);
    }

    (void)burrow__thread_join(&t);
}

TEST(a_thread_and_a_goroutine_can_keep_handing_a_value_back_and_forth) {
    reset();
    (void)runtime_gomaxprocs(1);
    runtime_main(BURROW_FN(Func, pingpong_body, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&rounds_done), ROUNDS);

    chan_free(shared);
    chan_free(pong);
    (void)runtime_gomaxprocs(0);
}

int main(void) {
    RUN(a_buffered_channel_is_a_queue);
    RUN(the_ring_wraps_without_losing_its_place);
    RUN(an_unbuffered_channel_holds_nothing);
    RUN(the_non_blocking_calls_answer_instead_of_waiting);
    RUN(a_closed_channel_drains_and_then_answers_false);
    RUN(a_closed_unbuffered_channel_answers_a_receive_at_once);
    RUN(a_receive_that_does_not_want_the_value_still_takes_it);
    RUN(a_channel_carries_whatever_its_type_says);
    RUN(a_nil_channel_is_empty_and_never_ready);
    RUN(a_channel_gives_back_everything_it_took);

    RUN(a_send_on_a_closed_channel_panics);
    RUN(closing_twice_panics);
    RUN(closing_nothing_panics);
    RUN(an_absurd_capacity_panics);

    RUN(an_unbuffered_send_does_not_return_until_somebody_has_the_value);
    RUN(everything_sent_arrives_in_order_and_the_close_ends_the_loop);
    RUN(several_senders_and_several_receivers_move_every_value_once);
    RUN(closing_wakes_every_receiver_that_is_waiting);
    RUN(a_full_buffer_with_a_sender_behind_it_still_comes_out_in_order);
    RUN(a_thread_that_is_not_a_goroutine_can_block_on_a_channel);
    RUN(a_goroutine_can_hand_a_value_to_a_thread_that_is_not_one);
    RUN(a_thread_and_a_goroutine_can_keep_handing_a_value_back_and_forth);

    return harness_report("chan");
}
