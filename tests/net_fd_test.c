/* The poll FD under net: a goroutine that waits on a socket parks in the
 * netpoller and is woken by the other end, by a Close, or by a deadline.
 *
 * Everything is over loopback, between goroutines of the one test.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/io.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include "../src/net/internal.h"
#include "check.h"

#include <stdint.h>
#include <string.h>

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_POLL_FD 1
#endif

static void need_poll_fd(TestingT *t) {
#if !defined(HAVE_POLL_FD)
    testing_t_skip_v(t, "the poll FD here is the readiness one only");
#else
    (void)t;
#endif
}

static Slice bytes_of(char *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

static PalSockAddr loopback(void) {
    PalSockAddr a = {0};
    a.family = PAL_AF_INET;
    a.addr[0] = 127;
    a.addr[3] = 1;
    return a;
}

/* A listener on loopback, in the poller, with the address it got. */
static bool listen_fd(TestingT *t, burrow__PollFD *l, PalSockAddr *addr, int32_t type) {
    PalErrno pe = PAL_OK;
    int64_t s = pal_socket(PAL_AF_INET, type, 0, &pe);
    PalSockAddr a = loopback();
    if (s < 0 || !pal_bind(s, &a, &pe) ||
        (type == PAL_SOCK_STREAM && !pal_listen(s, 16, &pe)) ||
        !pal_getsockname(s, addr, &pe)) {
        testing_t_errorf_v(t, "listen: %s", pal_errno_string(pe));
        if (s >= 0)
            pal_socket_close(s, NULL);
        return false;
    }
    Error e = burrow__pfd_init(l, s, type == PAL_SOCK_STREAM, type == PAL_SOCK_STREAM);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "into the poller: %v", e);
        pal_socket_close(s, NULL);
        return false;
    }
    return true;
}

/* Dials addr the way net does: a non blocking connect, a wait to write, and
 * then SO_ERROR for how it went. */
static bool dial_fd(TestingT *t, burrow__PollFD *c, const PalSockAddr *addr) {
    PalErrno pe = PAL_OK;
    int64_t s = pal_socket(PAL_AF_INET, PAL_SOCK_STREAM, 0, &pe);
    if (s < 0) {
        testing_t_errorf_v(t, "socket: %s", pal_errno_string(pe));
        return false;
    }
    Error e = burrow__pfd_init(c, s, true, true);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "into the poller: %v", e);
        pal_socket_close(s, NULL);
        return false;
    }
    if (!pal_connect(s, addr, &pe)) {
        if (pe != PAL_EINPROGRESS) {
            testing_t_errorf_v(t, "connect: %s", pal_errno_string(pe));
            return false;
        }
        e = burrow__pfd_wait_write(c);
        int64_t so = 0;
        if (BURROW_FAILED(e) || !pal_getsockopt(s, PAL_SO_ERROR, &so, &pe) ||
            so != PAL_OK) {
            testing_t_errorf_v(t, "connect did not finish: %v", e);
            return false;
        }
    }
    return true;
}

/* Both ends of one TCP connection, each in the poller, and the listener. */
typedef struct Pair {
    burrow__PollFD l;
    burrow__PollFD c;
    burrow__PollFD s;
} Pair;

static bool pair_open(TestingT *t, Pair *p) {
    PalSockAddr addr;
    if (!listen_fd(t, &p->l, &addr, PAL_SOCK_STREAM))
        return false;
    if (!dial_fd(t, &p->c, &addr))
        return false;
    Error e = BURROW_NO_ERROR;
    int64_t s = burrow__pfd_accept(&p->l, NULL, &e);
    if (s < 0) {
        testing_t_errorf_v(t, "accept: %v", e);
        return false;
    }
    e = burrow__pfd_init(&p->s, s, true, true);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "into the poller: %v", e);
        return false;
    }
    return true;
}

static void pair_close(Pair *p) {
    (void)burrow__pfd_close(&p->s);
    (void)burrow__pfd_close(&p->c);
    (void)burrow__pfd_close(&p->l);
}

/* ------------------------------------------------------------------ reading */

typedef struct ReadJob {
    burrow__PollFD *fd;
    char buf[64];
    Int n;
    Error err;
} ReadJob;

static void read_job(void *env) {
    ReadJob *j = env;
    j->n = burrow__pfd_read(j->fd, bytes_of(j->buf, (Int)sizeof j->buf), &j->err);
}

static void TestAReadWaitsForTheOtherEndToWrite(TestingT *t) {
    need_poll_fd(t);
    Pair p;
    if (!pair_open(t, &p))
        return;
    ReadJob j = {.fd = &p.s};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, read_job, &j));

    /* Long enough that the reader is parked by the time this writes, so the
     * test is of a wakeup and not of a read that found the bytes there. */
    time_sleep(20 * TIME_MILLISECOND);
    Error e = BURROW_NO_ERROR;
    char hi[] = "hello";
    CHECK_INT_EQ(burrow__pfd_write(&p.c, bytes_of(hi, 5), &e), 5);
    CHECK(!BURROW_FAILED(e));
    sync_wait_group_wait(&wg);
    CHECK_INT_EQ(j.n, 5);
    CHECK(!BURROW_FAILED(j.err));
    CHECK(memcmp(j.buf, "hello", 5) == 0);
    pair_close(&p);
}

static void TestAReadOfNothingAtAllIsTheEnd(TestingT *t) {
    need_poll_fd(t);
    Pair p;
    if (!pair_open(t, &p))
        return;
    CHECK(!BURROW_FAILED(burrow__pfd_shutdown(&p.c, PAL_SHUT_WR)));
    Error e = BURROW_NO_ERROR;
    char buf[8];
    CHECK_INT_EQ(burrow__pfd_read(&p.s, bytes_of(buf, 8), &e), 0);
    CHECK(errors_is(e, io_eof));

    /* An empty buffer reads nothing and says nothing, end or not. */
    CHECK_INT_EQ(burrow__pfd_read(&p.s, bytes_of(buf, 0), &e), 0);
    CHECK(!BURROW_FAILED(e));
    pair_close(&p);
}

static void TestACloseWakesAReadThatIsWaiting(TestingT *t) {
    need_poll_fd(t);
    Pair p;
    if (!pair_open(t, &p))
        return;
    ReadJob j = {.fd = &p.s};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, read_job, &j));
    time_sleep(20 * TIME_MILLISECOND);

    CHECK(!BURROW_FAILED(burrow__pfd_close(&p.s)));
    sync_wait_group_wait(&wg);
    CHECK_INT_EQ(j.n, 0);
    if (!errors_is(j.err, net_err_closed))
        testing_t_errorf_v(t, "the read woke with %v, want %v", j.err, net_err_closed);
    (void)burrow__pfd_close(&p.c);
    (void)burrow__pfd_close(&p.l);
}

static void TestEveryCallOnAClosedFDSaysItIsClosed(TestingT *t) {
    need_poll_fd(t);
    Pair p;
    if (!pair_open(t, &p))
        return;
    CHECK(!BURROW_FAILED(burrow__pfd_close(&p.c)));
    Error e = BURROW_NO_ERROR;
    char buf[4] = "abc";
    CHECK_INT_EQ(burrow__pfd_read(&p.c, bytes_of(buf, 3), &e), 0);
    CHECK(errors_is(e, net_err_closed));
    CHECK_INT_EQ(burrow__pfd_read(&p.c, bytes_of(buf, 0), &e), 0);
    CHECK(errors_is(e, net_err_closed));
    CHECK_INT_EQ(burrow__pfd_write(&p.c, bytes_of(buf, 3), &e), 0);
    CHECK(errors_is(e, net_err_closed));
    CHECK(errors_is(burrow__pfd_shutdown(&p.c, PAL_SHUT_RDWR), net_err_closed));
    CHECK(errors_is(burrow__pfd_set_deadline(&p.c, time_now(), BURROW_POLL_READ),
                    net_err_closed));
    CHECK(errors_is(burrow__pfd_close(&p.c), net_err_closed));

    /* The other end sees the close as the end of the stream. */
    CHECK_INT_EQ(burrow__pfd_read(&p.s, bytes_of(buf, 3), &e), 0);
    CHECK(errors_is(e, io_eof));
    (void)burrow__pfd_close(&p.s);
    (void)burrow__pfd_close(&p.l);
}

/* ---------------------------------------------------------------- deadlines */

static void TestAReadPastItsDeadlineTimesOutAndStaysTimedOut(TestingT *t) {
    need_poll_fd(t);
    Pair p;
    if (!pair_open(t, &p))
        return;
    Time start = time_now();
    CHECK(!BURROW_FAILED(burrow__pfd_set_deadline(
        &p.s, time_add(start, 30 * TIME_MILLISECOND), BURROW_POLL_READ)));
    Error e = BURROW_NO_ERROR;
    char buf[8];
    CHECK_INT_EQ(burrow__pfd_read(&p.s, bytes_of(buf, 8), &e), 0);
    CHECK(errors_is(e, os_err_deadline_exceeded));
    CHECK(net_error_timeout(e));
    Duration took = time_since(start);
    if (took < 25 * TIME_MILLISECOND)
        testing_t_errorf_v(t, "the read gave up after %d ns, before its deadline",
                           (Int)took);

    /* Bytes that arrive after the deadline do not undo it, which is Go's
     * rule: a timed out connection stays timed out until it is given a new
     * deadline. */
    char x[] = "x";
    CHECK_INT_EQ(burrow__pfd_write(&p.c, bytes_of(x, 1), &e), 1);
    time_sleep(10 * TIME_MILLISECOND);
    CHECK_INT_EQ(burrow__pfd_read(&p.s, bytes_of(buf, 8), &e), 0);
    CHECK(errors_is(e, os_err_deadline_exceeded));

    /* The write direction has a deadline of its own and it has not passed. */
    CHECK_INT_EQ(burrow__pfd_write(&p.s, bytes_of(x, 1), &e), 1);
    CHECK(!BURROW_FAILED(e));

    Time zero = {0};
    CHECK(!BURROW_FAILED(burrow__pfd_set_deadline(&p.s, zero, BURROW_POLL_READ)));
    CHECK_INT_EQ(burrow__pfd_read(&p.s, bytes_of(buf, 8), &e), 1);
    CHECK(!BURROW_FAILED(e));
    pair_close(&p);
}

static void TestADeadlineSetWhileAReadWaitsWakesIt(TestingT *t) {
    need_poll_fd(t);
    Pair p;
    if (!pair_open(t, &p))
        return;
    ReadJob j = {.fd = &p.s};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, read_job, &j));
    time_sleep(20 * TIME_MILLISECOND);

    /* A deadline already in the past, for both directions at once. */
    CHECK(!BURROW_FAILED(
        burrow__pfd_set_deadline(&p.s, time_add(time_now(), -TIME_SECOND),
                                 BURROW_POLL_READ | BURROW_POLL_WRITE)));
    sync_wait_group_wait(&wg);
    CHECK(errors_is(j.err, os_err_deadline_exceeded));
    pair_close(&p);
}

/* A write that fills the socket buffers waits for room, and gives up at its
 * deadline with what it got through. */
static void TestAWriteNobodyReadsTimesOut(TestingT *t) {
    need_poll_fd(t);
    Pair p;
    if (!pair_open(t, &p))
        return;
    Int n = 64 << 20;
    char *big = mem_alloc(heap_allocator(), (size_t)n, 1);
    if (big == NULL) {
        testing_t_fatalf_v(t, "out of memory");
        return;
    }
    memset(big, 'w', (size_t)n);
    CHECK(!BURROW_FAILED(burrow__pfd_set_deadline(
        &p.c, time_add(time_now(), 50 * TIME_MILLISECOND), BURROW_POLL_WRITE)));
    Error e = BURROW_NO_ERROR;
    Int wrote = burrow__pfd_write(&p.c, bytes_of(big, n), &e);
    CHECK(errors_is(e, os_err_deadline_exceeded));
    if (wrote <= 0 || wrote >= n)
        testing_t_errorf_v(t, "wrote %d of %d bytes before the deadline", wrote, n);
    mem_free(heap_allocator(), big, (size_t)n, 1);
    pair_close(&p);
}

/* ------------------------------------------------------------- a lot of data */

typedef struct Drain {
    burrow__PollFD *fd;
    Int got;
    bool in_order;
    Error err;
} Drain;

static void drain_job(void *env) {
    Drain *d = env;
    char buf[4096];
    d->in_order = true;
    for (;;) {
        Error e = BURROW_NO_ERROR;
        Int n = burrow__pfd_read(d->fd, bytes_of(buf, (Int)sizeof buf), &e);
        for (Int i = 0; i < n; i++)
            if (buf[i] != (char)('a' + (d->got + i) % 26))
                d->in_order = false;
        d->got += n;
        if (BURROW_FAILED(e)) {
            if (!errors_is(e, io_eof))
                d->err = e;
            return;
        }
    }
}

static void TestABigWriteWaitsForTheReaderToMakeRoom(TestingT *t) {
    need_poll_fd(t);
    Pair p;
    if (!pair_open(t, &p))
        return;
    Int n = 16 << 20;
    char *big = mem_alloc(heap_allocator(), (size_t)n, 1);
    if (big == NULL) {
        testing_t_fatalf_v(t, "out of memory");
        return;
    }
    for (Int i = 0; i < n; i++)
        big[i] = (char)('a' + i % 26);
    Drain d = {.fd = &p.s};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, drain_job, &d));
    Error e = BURROW_NO_ERROR;
    CHECK_INT_EQ(burrow__pfd_write(&p.c, bytes_of(big, n), &e), n);
    CHECK(!BURROW_FAILED(e));
    CHECK(!BURROW_FAILED(burrow__pfd_shutdown(&p.c, PAL_SHUT_WR)));
    sync_wait_group_wait(&wg);
    CHECK_INT_EQ(d.got, n);
    CHECK(d.in_order);
    CHECK(!BURROW_FAILED(d.err));
    mem_free(heap_allocator(), big, (size_t)n, 1);
    pair_close(&p);
}

/* ------------------------------------------------------------------ accepting */

typedef struct AcceptJob {
    burrow__PollFD *l;
    int64_t s;
    PalSockAddr peer;
    Error err;
} AcceptJob;

static void accept_job(void *env) {
    AcceptJob *j = env;
    j->s = burrow__pfd_accept(j->l, &j->peer, &j->err);
}

static void TestAnAcceptWaitsForAConnection(TestingT *t) {
    need_poll_fd(t);
    burrow__PollFD l;
    PalSockAddr addr;
    if (!listen_fd(t, &l, &addr, PAL_SOCK_STREAM))
        return;
    AcceptJob j = {.l = &l};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, accept_job, &j));
    time_sleep(20 * TIME_MILLISECOND);

    burrow__PollFD c;
    if (!dial_fd(t, &c, &addr)) {
        /* The close is what lets the accept, and so the test, finish. */
        (void)burrow__pfd_close(&l);
        sync_wait_group_wait(&wg);
        return;
    }
    sync_wait_group_wait(&wg);
    CHECK(j.s >= 0);
    CHECK(!BURROW_FAILED(j.err));
    PalSockAddr mine;
    CHECK(pal_getsockname(c.sysfd, &mine, NULL));
    CHECK_INT_EQ(j.peer.port, mine.port);
    if (j.s >= 0)
        CHECK(pal_socket_close(j.s, NULL));
    (void)burrow__pfd_close(&c);
    (void)burrow__pfd_close(&l);
}

static void TestACloseWakesAnAcceptThatIsWaiting(TestingT *t) {
    need_poll_fd(t);
    burrow__PollFD l;
    PalSockAddr addr;
    if (!listen_fd(t, &l, &addr, PAL_SOCK_STREAM))
        return;
    AcceptJob j = {.l = &l};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, accept_job, &j));
    time_sleep(20 * TIME_MILLISECOND);
    CHECK(!BURROW_FAILED(burrow__pfd_close(&l)));
    sync_wait_group_wait(&wg);
    CHECK_INT_EQ(j.s, -1);
    CHECK(errors_is(j.err, net_err_closed));
}

/* ---------------------------------------------------------------- datagrams */

typedef struct FromJob {
    burrow__PollFD *fd;
    char buf[16];
    Int n;
    PalSockAddr from;
    Error err;
} FromJob;

static void from_job(void *env) {
    FromJob *j = env;
    j->n = burrow__pfd_read_from(j->fd, bytes_of(j->buf, (Int)sizeof j->buf), &j->from,
                                 &j->err);
}

static void TestADatagramWaitsAndSaysWhoSentIt(TestingT *t) {
    need_poll_fd(t);
    burrow__PollFD u1;
    burrow__PollFD u2;
    PalSockAddr a1;
    PalSockAddr a2;
    if (!listen_fd(t, &u1, &a1, PAL_SOCK_DGRAM))
        return;
    if (!listen_fd(t, &u2, &a2, PAL_SOCK_DGRAM))
        return;
    FromJob j = {.fd = &u2};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, from_job, &j));
    time_sleep(20 * TIME_MILLISECOND);

    Error e = BURROW_NO_ERROR;
    char ping[] = "ping";
    CHECK_INT_EQ(burrow__pfd_write_to(&u1, bytes_of(ping, 4), &a2, &e), 4);
    CHECK(!BURROW_FAILED(e));
    sync_wait_group_wait(&wg);
    CHECK_INT_EQ(j.n, 4);
    CHECK(!BURROW_FAILED(j.err));
    CHECK(memcmp(j.buf, "ping", 4) == 0);
    CHECK_INT_EQ(j.from.port, a1.port);

    /* An empty datagram is a datagram, and not the end of anything. */
    CHECK_INT_EQ(burrow__pfd_write_to(&u1, bytes_of(ping, 0), &a2, &e), 0);
    CHECK(!BURROW_FAILED(e));
    CHECK_INT_EQ(burrow__pfd_read_from(&u2, bytes_of(j.buf, 16), NULL, &e), 0);
    CHECK(!BURROW_FAILED(e));
    (void)burrow__pfd_close(&u1);
    (void)burrow__pfd_close(&u2);
}

#define TESTS(X)                                                                       \
    X(TestAReadWaitsForTheOtherEndToWrite)                                             \
    X(TestAReadOfNothingAtAllIsTheEnd)                                                 \
    X(TestACloseWakesAReadThatIsWaiting)                                               \
    X(TestEveryCallOnAClosedFDSaysItIsClosed)                                          \
    X(TestAReadPastItsDeadlineTimesOutAndStaysTimedOut)                                \
    X(TestADeadlineSetWhileAReadWaitsWakesIt)                                          \
    X(TestAWriteNobodyReadsTimesOut)                                                   \
    X(TestABigWriteWaitsForTheReaderToMakeRoom)                                        \
    X(TestAnAcceptWaitsForAConnection)                                                 \
    X(TestACloseWakesAnAcceptThatIsWaiting)                                            \
    X(TestADatagramWaitsAndSaysWhoSentIt)

TESTING_MAIN(TESTS)
