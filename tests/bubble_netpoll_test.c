/* The netpoller, run inside a bubble.
 *
 * Step 9 of the retro-test pass that tests/bubble_test.c is steps 4 through 8,
 * and it is a file of its own because everything in it needs a readiness
 * backend. On Windows, and on the web targets that have no backend at all, this
 * compiles to a main that says so, which is what tests/netpoll_test.c does and
 * for the same reason.
 *
 * There is one question here and it is the one the rest of synctest is built
 * around. A goroutine waiting for a socket is not durably blocked, because the
 * thing that will wake it is the network and the network is not in the bubble.
 * Getting that wrong in the direction that looks harmless, by calling the wait
 * durable because the goroutine is certainly parked, is how a test ends up
 * reading a reply that has not arrived.
 *
 * What is not here is a deadline on a socket inside a bubble, and the reason is
 * worth writing down because the absence looks like an oversight. A deadline
 * armed in a bubble becomes a timer on the bubble's clock, and the clock cannot
 * move while the socket read is keeping the bubble from going idle, so the
 * deadline never fires. Go has the same shape. It is fine as long as the rule
 * is that real network IO does not go in a bubble, and it is the thing to
 * revisit when net lands. docs/design/17-open-questions.md section 12 is where
 * that is tracked. A test for it would be a test that hangs.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/netpoll.h"

#if !defined(BURROW_NETPOLL_READINESS) || defined(BURROW_NETPOLL_NONE)

#include "burrow/testing.h"

static void TestNotAReadinessBackend(TestingT *t) {
    testing_t_skip_v(t, "this platform does not have a readiness backend");
}

#define TESTS(X) X(TestNotAReadinessBackend)

TESTING_MAIN(TESTS)

#else

#include "burrow/synctest.h"

#include "burrow/atomic.h"
#include "burrow/chan.h"
#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include "check.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>

/* ------------------------------------------------------------------- pipes */

typedef struct Pipe {
    int rd;
    int wr;
} Pipe;

static bool nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static bool pipe_open(Pipe *p) {
    int fds[2];
    if (pipe(fds) != 0)
        return false;

    p->rd = fds[0];
    p->wr = fds[1];
    return nonblocking(p->rd) && nonblocking(p->wr);
}

static void pipe_shut(Pipe *p) {
    if (p->rd >= 0)
        (void)close(p->rd);
    if (p->wr >= 0)
        (void)close(p->wr);
    p->rd = -1;
    p->wr = -1;
}

static bool put_byte(int fd) {
    char b = 'x';
    return write(fd, &b, 1) == 1;
}

/* Reads one byte the way a caller of this layer is meant to: try first, and go
 * to the poller only when the kernel says there is nothing there yet. */
static bool get_byte(burrow__PollDesc *pd, int fd) {
    for (;;) {
        char b;
        ssize_t n = read(fd, &b, 1);
        if (n == 1)
            return true;
        if (n == 0)
            return false;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return false;
        if (burrow__poll_wait(pd, BURROW_POLL_READ) != BURROW_POLL_READY)
            return false;
    }
}

static Pipe shared;
static burrow__PollDesc *shared_pd;
static Chan *handshake;
static Chan *inside;

static uint32_t got;
static uint32_t total;
static uint32_t written;

static void reset(void) {
    shared.rd = -1;
    shared.wr = -1;
    shared_pd = NULL;
    handshake = NULL;
    inside = NULL;
    got = 0;
    total = 0;
    written = 0;
}

/* --- a socket read is not a durable wait
 *
 * The child is inside the bubble and is waiting for a byte that only somebody
 * outside can send. That is not a durable wait, and a bubble that thought it
 * was would let synctest_wait return while the byte was still on its way, which
 * is a test that reads its answer before the answer exists.
 *
 * The twenty milliseconds are the same trade tests/bubble_test.c makes for its
 * mutex test. The poller gives no way to ask whether a goroutine has reached
 * the park, so the outsider waits long enough that it certainly has before
 * writing. If the wait were durable, synctest_wait would come back during those
 * twenty milliseconds with nothing read and the value below still zero.
 *
 * The outsider is a goroutine rather than a thread, and it is outside the
 * bubble, so its sleep is on the machine's clock. Nothing here is on the
 * bubble's clock at all, which is the point: a bubble whose goroutines are
 * waiting on the network is a bubble whose clock cannot move. */

static void reader_child(void *env) {
    (void)env;

    if (get_byte(shared_pd, shared.rd))
        burrow__atomic_store_release_u32(&got, 1);
}

static void outside_writer(void *env) {
    (void)env;

    Int v;
    if (!chan_recv(handshake, &v))
        return;

    time_sleep(20 * TIME_MILLISECOND);
    burrow__atomic_store_release_u32(&written, 1);
    (void)put_byte(shared.wr);
}

static void reading_body(void *env) {
    (void)env;

    if (!go(BURROW_FN(Func, reader_child, NULL)))
        return;

    /* Unbuffered, so the outsider has it before this returns and its twenty
     * milliseconds start now rather than whenever it gets scheduled. */
    Int v = 1;
    chan_send(handshake, &v);

    synctest_wait();
    burrow__atomic_store_release_u32(&total, burrow__atomic_load_acquire_u32(&got));
}

static void reading_top(void *env) {
    (void)env;

    if (!pipe_open(&shared))
        return;
    if (burrow__poll_open(shared.rd, &shared_pd) != 0) {
        pipe_shut(&shared);
        return;
    }

    handshake = chan_make(heap_allocator(), TYPE_INT, 0);
    if (handshake != NULL && go(BURROW_FN(Func, outside_writer, NULL)))
        (void)synctest_run(BURROW_FN(Func, reading_body, NULL));

    burrow__poll_unblock(shared_pd);
    burrow__poll_close(shared_pd);
    pipe_shut(&shared);
    chan_free(handshake);
}

static void TestASocketReadInABubbleIsNotADurableWait(TestingT *t) {
    reset();
    runtime_main(BURROW_FN(Func, reading_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&written), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&total), 1);
}

/* --- a socket that is ready already
 *
 * The byte is written before the bubble starts, so the read finds it on the
 * first try and the poller is never reached. Which is the case the fast path
 * exists for, and in a bubble it is also the case where a goroutine gets all
 * the way through a network read without parking once.
 *
 * Worth its own test because the bubble accounting lives on the park, and the
 * failure it is looking for is the mirror of the one above: a count that goes
 * up on the way into a read and down on the way out of a park that never
 * happened. That bubble never goes idle again and synctest_wait never returns.
 * Here it has to return twice.
 *
 * The channel the child parks on is made in the body rather than alongside the
 * pipe, because a channel made outside the bubble is not a durable wait either
 * and this test needs the child's one park to be one synctest_wait can see. */

static void ready_child(void *env) {
    (void)env;

    if (get_byte(shared_pd, shared.rd))
        (void)burrow__atomic_add_u32(&got, 1);

    Int v;
    if (chan_recv(inside, &v))
        (void)burrow__atomic_add_u32(&total, (uint32_t)v);
}

static void ready_body(void *env) {
    (void)env;

    inside = chan_make(heap_allocator(), TYPE_INT, 0);
    if (inside == NULL)
        return;
    if (!go(BURROW_FN(Func, ready_child, NULL)))
        return;

    /* The child has read its byte and is parked on the channel, which is a
     * durable wait and is the only one it has. */
    synctest_wait();

    Int v = 7;
    if (chan_try_send(inside, &v))
        burrow__atomic_store_release_u32(&written, 1);

    synctest_wait();
}

static void ready_top(void *env) {
    (void)env;

    if (!pipe_open(&shared))
        return;
    if (burrow__poll_open(shared.rd, &shared_pd) != 0) {
        pipe_shut(&shared);
        return;
    }

    if (put_byte(shared.wr))
        (void)synctest_run(BURROW_FN(Func, ready_body, NULL));

    burrow__poll_unblock(shared_pd);
    burrow__poll_close(shared_pd);
    pipe_shut(&shared);
    chan_free(inside);
}

static void TestAReadThatFindsItsByteInABubbleNeverParks(TestingT *t) {
    reset();
    runtime_main(BURROW_FN(Func, ready_top, NULL));

    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&got), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&written), 1);
    CHECK_INT_EQ(burrow__atomic_load_acquire_u32(&total), 7);
}

#define TESTS(X)                                                                       \
    X(TestASocketReadInABubbleIsNotADurableWait)                                       \
    X(TestAReadThatFindsItsByteInABubbleNeverParks)

TESTING_MAIN_BARE(TESTS)

#endif /* readiness backend */
