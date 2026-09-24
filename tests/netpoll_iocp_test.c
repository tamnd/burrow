/* Tests for the completion backend, which today means Windows and IOCP.
 *
 * netpoll_test.c covers everything that is the same on all three backends and
 * it is written against readiness, so it does not run here. What is left for
 * this file is the part that has no counterpart on epoll or kqueue: the caller
 * hands the kernel the read itself, along with a burrow__PollOp, and then parks.
 * Getting that wrong does not look like a poller bug, it looks like a read that
 * never finishes, so it is worth testing on its own.
 *
 * Everything here uses a loopback TCP pair rather than a pipe. An anonymous
 * pipe on Windows is not opened for overlapped I/O and cannot be attached to a
 * completion port, and the named pipe that would work is more setup than a
 * socket. A socket is also what this layer is actually for.
 *
 * The discipline the tests follow is the one net will follow, so it is written
 * out once in recv_one below and the tests use it rather than repeating it:
 * fill in the operation, hand it to the kernel, and park only when the kernel
 * says the work is still going. An operation that finishes immediately still
 * posts to the port, so the park is the normal path and not the slow one.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/netpoll.h"

#if !defined(BURROW_NETPOLL_COMPLETION)

#include "burrow/testing.h"

static void TestNotACompletionBackend(TestingT *t) {
    testing_t_skip_v(t, "this platform does not have a completion backend");
}

#define TESTS(X) X(TestNotACompletionBackend)

TESTING_MAIN(TESTS)

#else

#include "check.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/func.h"
#include "burrow/note.h"
#include "burrow/proc.h"
#include "burrow/sched.h"
#include "burrow/thread.h"
#include "burrow/time.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>

/* ------------------------------------------------------------------ sockets */

typedef struct Pair {
    SOCKET a;
    SOCKET b;
} Pair;

/* A connected pair over the loopback, which is the closest thing Windows has to
 * socketpair. The listener goes away as soon as the connection is made, so
 * nothing here is left listening on a port between tests.
 *
 * Both sockets are made with WSA_FLAG_OVERLAPPED, which is what makes them
 * usable with a completion port. A socket from plain socket() has it too, but
 * asking for it says why it matters. */
static bool pair_open(Pair *p) {
    p->a = INVALID_SOCKET;
    p->b = INVALID_SOCKET;

    SOCKET ln =
        WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (ln == INVALID_SOCKET)
        return false;

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

    int len = (int)sizeof addr;
    bool ok = bind(ln, (struct sockaddr *)&addr, len) == 0 && listen(ln, 1) == 0 &&
              getsockname(ln, (struct sockaddr *)&addr, &len) == 0;

    if (ok) {
        p->a =
            WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
        ok =
            p->a != INVALID_SOCKET && connect(p->a, (struct sockaddr *)&addr, len) == 0;
    }
    if (ok) {
        p->b = accept(ln, NULL, NULL);
        ok = p->b != INVALID_SOCKET;
    }

    (void)closesocket(ln);
    if (!ok) {
        if (p->a != INVALID_SOCKET)
            (void)closesocket(p->a);
        if (p->b != INVALID_SOCKET)
            (void)closesocket(p->b);
        p->a = INVALID_SOCKET;
        p->b = INVALID_SOCKET;
    }
    return ok;
}

static void pair_shut(Pair *p) {
    if (p->a != INVALID_SOCKET)
        (void)closesocket(p->a);
    if (p->b != INVALID_SOCKET)
        (void)closesocket(p->b);
    p->a = INVALID_SOCKET;
    p->b = INVALID_SOCKET;
}

/* ------------------------------------------------------- the whole discipline
 *
 * One read on a completion backend, start to finish. Answers how many bytes
 * arrived, or -1, and puts the reason the wait ended in `*why` for the tests
 * that care which reason it was.
 *
 * Two parts of this are the parts that catch people out.
 *
 * WSARecv answers 0 when the data was already there and SOCKET_ERROR with
 * WSA_IO_PENDING when it was not. Both end up in the same place, because the
 * completion is posted either way, so both park and both are woken by the
 * poller. Only a third answer is a real failure.
 *
 * A wait that does not come back ready leaves the operation with the kernel,
 * and the operation is a local here, so returning without getting it back would
 * hand the kernel a dead stack frame to write into. That is the rule next to
 * burrow__poll_op_init, and the three lines under `if` below are the whole of
 * obeying it. */
static int recv_one(burrow__PollDesc *pd, SOCKET s, char *buf, uint32_t n,
                    burrow__PollStatus *why) {
    burrow__PollOp op;
    burrow__poll_op_init(&op, pd, BURROW_POLL_READ);

    WSABUF b;
    b.buf = buf;
    b.len = n;

    DWORD got = 0;
    DWORD flags = 0;
    if (WSARecv(s, &b, 1, &got, &flags, (OVERLAPPED *)&op, NULL) != 0 &&
        WSAGetLastError() != WSA_IO_PENDING) {
        *why = BURROW_POLL_UNPOLLABLE;
        return -1;
    }

    *why = burrow__poll_wait(pd, BURROW_POLL_READ);
    if (*why != BURROW_POLL_READY) {
        (void)CancelIoEx((HANDLE)s, (OVERLAPPED *)&op);
        burrow__poll_wait_canceled(pd, BURROW_POLL_READ);
        return -1;
    }

    if (op.status != 0)
        return -1;
    return (int)op.qty;
}

static bool send_all(SOCKET s, const char *buf, int n) {
    while (n > 0) {
        int sent = send(s, buf, n, 0);
        if (sent <= 0)
            return false;
        buf += sent;
        n -= sent;
    }
    return true;
}

/* The same counter wait netpoll_test.c uses, and for the same reason: a main
 * that returns before the goroutine it was waiting on has run is a test that
 * fails for a reason unrelated to what it tests. */
static bool wait_for(uint32_t *count, uint32_t want) {
    for (int i = 0; i < 2000; i++) {
        if (burrow__atomic_load_acquire_u32(count) >= want)
            return true;
        time_sleep(TIME_MILLISECOND);
    }
    return false;
}

/* ------------------------------------------------------- opening and closing */

static Pair shared;
static burrow__PollDesc *pd_a;

/* Where recv_one puts the reason its wait ended, so that a test which is about
 * the reason can check it without every test having to pass a local in. */
static burrow__PollStatus why;

static bool open_ok;
static int open_err;

static void open_and_close(void *env) {
    (void)env;

    Pair p;
    if (!pair_open(&p))
        return;

    burrow__PollDesc *pd = NULL;
    open_err = burrow__poll_open(p.a, &pd);
    open_ok = open_err == 0 && pd != NULL;

    if (open_ok) {
        burrow__poll_unblock(pd);
        burrow__poll_close(pd);
    }
    pair_shut(&p);
}

static void TestASocketCanBeAttachedToThePortAndGivenBack(TestingT *t) {
    open_ok = false;
    open_err = -1;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, open_and_close, NULL));

    CHECK(open_ok);
    CHECK_INT_EQ(open_err, 0);

    /* And the port exists now, which is the other half of what attaching a
     * socket is for. */
    CHECK(burrow__netpoll_inited());
}

/* --------------------------------------------------------- one read, one write */

static int got_n;
static char got_buf[8];
static uint32_t done;

static void read_side(void *env) {
    (void)env;
    got_n = recv_one(pd_a, shared.a, got_buf, (uint32_t)sizeof got_buf, &why);
    burrow__atomic_add_u32(&done, 1);
}

static void write_side(void *env) {
    (void)env;

    /* Long enough that the read is submitted and parked by the time the bytes
     * turn up, which is the ordering worth testing. The other ordering is the
     * test after this one. */
    time_sleep(50 * TIME_MILLISECOND);
    (void)send_all(shared.b, "burrow", 6);
    burrow__atomic_add_u32(&done, 1);
}

static void one_each_way(void *env) {
    TestingT *t = env;

    if (burrow__poll_open(shared.a, &pd_a) != 0)
        return;
    CHECK(go(BURROW_FN(Func, read_side, NULL)));
    write_side(NULL);
    (void)wait_for(&done, 2);
}

static void TestASubmittedReadIsFinishedByAWriteFromTheOtherSide(TestingT *t) {
    why = BURROW_POLL_CLOSED;
    got_n = -2;
    done = 0;
    pd_a = NULL;
    CHECK(pair_open(&shared));
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, one_each_way, t));

    CHECK_INT_EQ(why, BURROW_POLL_READY);
    CHECK_INT_EQ(got_n, 6);
    CHECK(memcmp(got_buf, "burrow", 6) == 0);

    if (pd_a != NULL) {
        burrow__poll_unblock(pd_a);
        burrow__poll_close(pd_a);
    }
    pair_shut(&shared);
}

/* The other ordering: the bytes are already there when the read is submitted.
 * WSARecv answers straight away rather than pending, and the completion is
 * still posted, so the goroutine still parks and is still woken. A backend that
 * got this wrong would hang here and pass the test above. */
static void read_what_is_already_there(void *env) {
    (void)env;

    if (burrow__poll_open(shared.a, &pd_a) != 0)
        return;
    if (!send_all(shared.b, "burrow", 6))
        return;

    /* Give the bytes time to cross the loopback, so that the submission below
     * really is the second of the two and the test is testing what it says. */
    time_sleep(50 * TIME_MILLISECOND);

    got_n = recv_one(pd_a, shared.a, got_buf, (uint32_t)sizeof got_buf, &why);
    burrow__atomic_add_u32(&done, 1);
}

static void TestAReadSubmittedAfterTheBytesArrivedFinishesToo(TestingT *t) {
    why = BURROW_POLL_CLOSED;
    got_n = -2;
    done = 0;
    pd_a = NULL;
    CHECK(pair_open(&shared));
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, read_what_is_already_there, NULL));

    CHECK_INT_EQ(why, BURROW_POLL_READY);
    CHECK_INT_EQ(got_n, 6);
    CHECK(memcmp(got_buf, "burrow", 6) == 0);

    if (pd_a != NULL) {
        burrow__poll_unblock(pd_a);
        burrow__poll_close(pd_a);
    }
    pair_shut(&shared);
}

/* --------------------------------------------------------------- end of file */

static void read_until_the_far_end_goes(void *env) {
    (void)env;

    if (burrow__poll_open(shared.a, &pd_a) != 0)
        return;

    (void)closesocket(shared.b);
    shared.b = INVALID_SOCKET;

    /* A receive on a connection the other side has closed finishes with zero
     * bytes and no error, which is how the end of a stream arrives here. It is
     * not a failure and it must not come back as one, because a reader that
     * treats it as a failure loses the difference between a connection that
     * ended and a connection that broke. */
    got_n = recv_one(pd_a, shared.a, got_buf, (uint32_t)sizeof got_buf, &why);
    burrow__atomic_add_u32(&done, 1);
}

static void TestAReadOnAClosedConnectionFinishesWithNothing(TestingT *t) {
    why = BURROW_POLL_CLOSED;
    got_n = -2;
    done = 0;
    pd_a = NULL;
    CHECK(pair_open(&shared));
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, read_until_the_far_end_goes, NULL));

    CHECK_INT_EQ(why, BURROW_POLL_READY);
    CHECK_INT_EQ(got_n, 0);

    if (pd_a != NULL) {
        burrow__poll_unblock(pd_a);
        burrow__poll_close(pd_a);
    }
    pair_shut(&shared);
}

/* ------------------------------------------------------------------ deadlines
 *
 * The deadline machinery is in the generic half, so this is not testing it
 * again. What it is testing is that a deadline still reaches a goroutine parked
 * on a completion backend, where nothing arrives unasked and the only other way
 * out of the park is an operation finishing. A read submitted on a quiet
 * connection is exactly that park. */

static void read_with_a_deadline(void *env) {
    (void)env;

    if (burrow__poll_open(shared.a, &pd_a) != 0)
        return;
    if (!burrow__poll_set_deadline(pd_a, burrow__nanotime() + 50 * TIME_MILLISECOND,
                                   BURROW_POLL_READ))
        return;

    /* Nothing is ever sent on this connection, so the only way this comes back
     * is the deadline. recv_one does the cancel and the wait for the completion
     * that has to follow, which is the half of a timed out read that a readiness
     * backend has no equivalent of. */
    got_n = recv_one(pd_a, shared.a, got_buf, (uint32_t)sizeof got_buf, &why);
    burrow__atomic_add_u32(&done, 1);
}

static void TestADeadlineWakesAGoroutineParkedOnAnOperation(TestingT *t) {
    why = BURROW_POLL_READY;
    got_n = -2;
    done = 0;
    pd_a = NULL;
    CHECK(pair_open(&shared));
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, read_with_a_deadline, NULL));

    CHECK_INT_EQ(why, BURROW_POLL_TIMEOUT);
    CHECK_INT_EQ(got_n, -1);

    if (pd_a != NULL) {
        burrow__poll_unblock(pd_a);
        burrow__poll_close(pd_a);
    }
    pair_shut(&shared);
}

/* ------------------------------------------------- the thread asleep in the port
 *
 * The test that matters most and the one that looks least like the others, the
 * same as in netpoll_test.c. One P, so one thread. The main goroutine submits a
 * read and parks, which leaves the program with nothing runnable at all, and a
 * thread outside the runtime sends the bytes. That can only work if the last
 * thread went to sleep inside GetQueuedCompletionStatusEx rather than on its
 * note, and nothing else in this file would notice if it had not. */

static bool outsider_started;
static bool woken_from_outside;
static burrow__Thread outsider;

static void send_from_outside(void *arg) {
    (void)arg;

    /* Not a goroutine and not an M, so time_sleep is not available here and a
     * gate nobody ever opens is what stands in for it. By the time this sends,
     * the program has one goroutine, it is parked on an operation, and every
     * thread has stopped. */
    burrow__Note gate;
    if (burrow__note_init(&gate)) {
        (void)burrow__note_sleep_timeout(&gate, 50 * 1000 * 1000);
        burrow__note_free(&gate);
    }
    (void)send_all(shared.b, "burrow", 6);
}

static void sleep_in_the_port(void *env) {
    (void)env;

    if (burrow__poll_open(shared.a, &pd_a) != 0)
        return;

    outsider_started = burrow__thread_start(&outsider, send_from_outside, NULL, 0);
    if (!outsider_started)
        return;

    got_n = recv_one(pd_a, shared.a, got_buf, (uint32_t)sizeof got_buf, &why);
    woken_from_outside = got_n == 6;
    (void)burrow__thread_join(&outsider);
}

static void TestTheLastThreadSleepsInsideThePort(TestingT *t) {
    got_n = -2;
    pd_a = NULL;
    outsider_started = false;
    woken_from_outside = false;
    CHECK(pair_open(&shared));
    (void)runtime_gomaxprocs(1);

    runtime_main(BURROW_FN(Func, sleep_in_the_port, NULL));

    CHECK(outsider_started);
    CHECK(woken_from_outside);

    if (pd_a != NULL) {
        burrow__poll_unblock(pd_a);
        burrow__poll_close(pd_a);
    }
    pair_shut(&shared);
}

/* Again, because the interesting bugs in this area are in teardown and they
 * only show up on the run after the one that made them. */
static void TestTheLastThreadSleepsInsideThePortAgain(TestingT *t) {
    got_n = -2;
    pd_a = NULL;
    outsider_started = false;
    woken_from_outside = false;
    CHECK(pair_open(&shared));
    (void)runtime_gomaxprocs(1);

    runtime_main(BURROW_FN(Func, sleep_in_the_port, NULL));

    CHECK(outsider_started);
    CHECK(woken_from_outside);

    if (pd_a != NULL) {
        burrow__poll_unblock(pd_a);
        burrow__poll_close(pd_a);
    }
    pair_shut(&shared);
}

#define TESTS(X)                                                                       \
    X(TestASocketCanBeAttachedToThePortAndGivenBack)                                   \
    X(TestASubmittedReadIsFinishedByAWriteFromTheOtherSide)                            \
    X(TestAReadSubmittedAfterTheBytesArrivedFinishesToo)                               \
    X(TestAReadOnAClosedConnectionFinishesWithNothing)                                 \
    X(TestADeadlineWakesAGoroutineParkedOnAnOperation)                                 \
    X(TestTheLastThreadSleepsInsideThePort)                                            \
    X(TestTheLastThreadSleepsInsideThePortAgain)

static int TestMain(TestingM *m) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 1;
    }
    int code = testing_m_run(m);
    (void)WSACleanup();
    return code;
}

TESTING_MAIN_BARE_WITH(TestMain, TESTS)

#endif /* BURROW_NETPOLL_COMPLETION */
