/* Tests for the netpoller.
 *
 * Everything here uses a pipe, because a pipe is the smallest thing epoll and
 * kqueue will both report readiness for and it needs no network to exist. The
 * poller does not know the difference: a pipe and a socket are both a
 * descriptor that goes from not ready to ready when somebody else acts on it,
 * which is the whole of what is being tested.
 *
 * The last test is the one that matters most and it is the one that looks the
 * least like the others. Every other test has a goroutine on one side and a
 * goroutine on the other, so the program always has something runnable and the
 * scheduler never has to sleep. The last one leaves the program with nothing to
 * run at all and has a thread outside the runtime write the byte, which can
 * only work if the last thread went to sleep inside the poller rather than on
 * its note. That is the whole point of the netpoller and nothing else in this
 * file would notice if it were missing.
 *
 * Everything in here is written against a readiness backend, so it is epoll and
 * kqueue only. That is not a gap: readiness is what this file is testing, and a
 * completion backend has a different deal with its caller and a test of its own
 * in netpoll_iocp_test.c. On Windows, and on the web targets that have no
 * backend at all, this compiles to a main that says so.
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

#include "check.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/func.h"
#include "burrow/lock.h"
#include "burrow/note.h"
#include "burrow/proc.h"
#include "burrow/sched.h"
#include "burrow/thread.h"
#include "burrow/time.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
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

static bool socket_pair(int fds[2]) {
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
        return false;
    return nonblocking(fds[0]) && nonblocking(fds[1]);
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

/* Waits for a counter to reach a number, because runtime_main comes back when
 * the main goroutine returns and not when the program is finished. A main that
 * writes the byte and returns is a program that can end before the goroutine
 * waiting for that byte has been scheduled, which is a test that fails once in
 * twenty runs for a reason that has nothing to do with the poller.
 *
 * The limit is there so that a real failure is a test that fails rather than a
 * test that never finishes. Two seconds against sleeps measured in tens of
 * milliseconds, so it is not a bound anything correct can run into. */
static bool wait_for(uint32_t *count, uint32_t want) {
    for (int i = 0; i < 2000; i++) {
        if (burrow__atomic_load_acquire_u32(count) >= want)
            return true;
        time_sleep(TIME_MILLISECOND);
    }
    return false;
}

/* ------------------------------------------------------------ opening and closing */

static bool open_ok;
static int open_err;

static void open_and_close(void *env) {
    (void)env;

    Pipe p;
    if (!pipe_open(&p))
        return;

    burrow__PollDesc *pd = NULL;
    open_err = burrow__poll_open(p.rd, &pd);
    open_ok = open_err == 0 && pd != NULL;

    if (open_ok) {
        burrow__poll_unblock(pd);
        burrow__poll_close(pd);
    }
    pipe_shut(&p);
}

static void TestADescriptorCanBeTakenInAndGivenBack(TestingT *t) {
    open_ok = false;
    open_err = -1;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, open_and_close, NULL));

    CHECK(open_ok);
    CHECK_INT_EQ(open_err, 0);

    /* And the poller is running now, which is the other half of what opening a
     * descriptor is for. Nothing before this test should have started it. */
    CHECK(burrow__netpoll_inited());
}

/* --------------------------------------------------------------- reading */

static Pipe shared;
static burrow__PollDesc *shared_pd;
static bool read_got;
static bool wrote;
static uint32_t done;

static void reader(void *env) {
    (void)env;
    read_got = get_byte(shared_pd, shared.rd);
    burrow__atomic_add_u32(&done, 1);
}

static void wait_then_write(void *env) {
    (void)env;

    /* Long enough that the reader is parked on the descriptor by the time this
     * writes, so the test is about a goroutine being woken by the poller and
     * not about a read that found the byte already there. */
    time_sleep(20 * TIME_MILLISECOND);
    wrote = put_byte(shared.wr);
}

static void one_reader_one_writer(void *env) {
    TestingT *t = env;

    if (!pipe_open(&shared))
        return;
    if (burrow__poll_open(shared.rd, &shared_pd) != 0)
        return;

    CHECK(go(BURROW_FN(Func, reader, NULL)));
    wait_then_write(NULL);
    CHECK(wait_for(&done, 1));
}

static void TestAGoroutineParkedOnAReadIsWokenByAWrite(TestingT *t) {
    shared = (Pipe){-1, -1};
    shared_pd = NULL;
    read_got = false;
    wrote = false;
    done = 0;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, one_reader_one_writer, t));

    CHECK(wrote);
    CHECK(read_got);

    if (shared_pd != NULL) {
        burrow__poll_unblock(shared_pd);
        burrow__poll_close(shared_pd);
    }
    pipe_shut(&shared);
}

/* --------------------------------------------------------------- writing */

static burrow__PollStatus write_status;

static void wait_to_write(void *env) {
    (void)env;

    if (!pipe_open(&shared))
        return;
    if (burrow__poll_open(shared.wr, &shared_pd) != 0)
        return;

    /* An empty pipe is writable, so this is the case where the answer comes
     * back without the goroutine ever parking. It still has to go through the
     * poller, because a descriptor that was never reported ready starts out
     * not ready as far as this layer is concerned, and the first wait is what
     * asks the kernel. */
    write_status = burrow__poll_wait(shared_pd, BURROW_POLL_WRITE);
}

static void TestAWaitToWriteOnAnEmptyPipeComesStraightBack(TestingT *t) {
    shared = (Pipe){-1, -1};
    shared_pd = NULL;
    write_status = BURROW_POLL_UNPOLLABLE;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, wait_to_write, NULL));

    CHECK_INT_EQ(write_status, BURROW_POLL_READY);

    if (shared_pd != NULL) {
        burrow__poll_unblock(shared_pd);
        burrow__poll_close(shared_pd);
    }
    pipe_shut(&shared);
}

/* --------------------------------------------------- both ends of one descriptor */

static int sock[2];
static burrow__PollDesc *sock_pd;
static bool both_read;
static bool both_wrote;

static void read_the_socket(void *env) {
    (void)env;
    both_read = get_byte(sock_pd, sock[0]);
    burrow__atomic_add_u32(&done, 1);
}

static void write_the_socket(void *env) {
    (void)env;
    if (burrow__poll_wait(sock_pd, BURROW_POLL_WRITE) == BURROW_POLL_READY)
        both_wrote = write(sock[0], "y", 1) == 1;
    burrow__atomic_add_u32(&done, 1);
}

static void two_directions(void *env) {
    TestingT *t = env;

    /* A socket pair rather than a pipe, because a pipe end is one direction
     * only and this is about a reader and a writer on the same descriptor
     * without either one seeing the other's readiness. */
    if (!socket_pair(sock))
        return;
    if (burrow__poll_open(sock[0], &sock_pd) != 0)
        return;

    CHECK(go(BURROW_FN(Func, read_the_socket, NULL)));
    CHECK(go(BURROW_FN(Func, write_the_socket, NULL)));

    time_sleep(20 * TIME_MILLISECOND);
    CHECK(put_byte(sock[1]));
    CHECK(wait_for(&done, 2));
}

static void TestAReaderAndAWriterShareOneDescriptor(TestingT *t) {
    sock[0] = -1;
    sock[1] = -1;
    sock_pd = NULL;
    both_read = false;
    both_wrote = false;
    done = 0;
    (void)runtime_gomaxprocs(4);

    runtime_main(BURROW_FN(Func, two_directions, t));

    CHECK(both_wrote);
    CHECK(both_read);

    if (sock_pd != NULL) {
        burrow__poll_unblock(sock_pd);
        burrow__poll_close(sock_pd);
    }
    if (sock[0] >= 0)
        (void)close(sock[0]);
    if (sock[1] >= 0)
        (void)close(sock[1]);
}

/* ------------------------------------------------------------------ unblock */

static burrow__PollStatus closed_status;
static burrow__PollStatus after_status;

static void park_forever(void *env) {
    (void)env;
    closed_status = burrow__poll_wait(shared_pd, BURROW_POLL_READ);
    burrow__atomic_add_u32(&done, 1);
}

static void unblock_it(void *env) {
    TestingT *t = env;

    if (!pipe_open(&shared))
        return;
    if (burrow__poll_open(shared.rd, &shared_pd) != 0)
        return;

    CHECK(go(BURROW_FN(Func, park_forever, NULL)));

    /* Nothing is ever written to this pipe, so the goroutine above is parked
     * with no event coming for it. Only the unblock can end that wait. */
    time_sleep(20 * TIME_MILLISECOND);
    burrow__poll_unblock(shared_pd);
    CHECK(wait_for(&done, 1));

    after_status = burrow__poll_wait(shared_pd, BURROW_POLL_READ);
}

static void TestUnblockingWakesTheWaiterAndStaysClosed(TestingT *t) {
    shared = (Pipe){-1, -1};
    shared_pd = NULL;
    closed_status = BURROW_POLL_UNPOLLABLE;
    after_status = BURROW_POLL_UNPOLLABLE;
    done = 0;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, unblock_it, t));

    CHECK_INT_EQ(closed_status, BURROW_POLL_CLOSED);
    CHECK_INT_EQ(after_status, BURROW_POLL_CLOSED);

    if (shared_pd != NULL)
        burrow__poll_close(shared_pd);
    pipe_shut(&shared);
}

/* --------------------------------------------------------------- many at once */

#define MANY 64

static Pipe many_pipe[MANY];
static burrow__PollDesc *many_pd[MANY];
static uint32_t many_read;
static uint32_t many_opened;

static void read_one(void *env) {
    int i = (int)(intptr_t)env;
    if (get_byte(many_pd[i], many_pipe[i].rd))
        burrow__atomic_add_u32(&many_read, 1);
}

static void many_readers(void *env) {
    TestingT *t = env;

    for (int i = 0; i < MANY; i++) {
        many_pipe[i] = (Pipe){-1, -1};
        if (!pipe_open(&many_pipe[i]))
            break;
        if (burrow__poll_open(many_pipe[i].rd, &many_pd[i]) != 0)
            break;
        many_opened++;
    }

    for (uint32_t i = 0; i < many_opened; i++)
        CHECK(go(BURROW_FN(Func, read_one, (void *)(intptr_t)i)));

    /* Every reader is parked by now, and then every one of them becomes ready
     * inside the same poll. That is the case where one wakeup has to turn into
     * a batch of goroutines rather than one. */
    time_sleep(30 * TIME_MILLISECOND);
    for (uint32_t i = 0; i < many_opened; i++)
        CHECK(put_byte(many_pipe[i].wr));

    CHECK(wait_for(&many_read, many_opened));
}

static void TestManyDescriptorsAndManyWaitersAtOnce(TestingT *t) {
    many_read = 0;
    many_opened = 0;
    (void)runtime_gomaxprocs(4);

    runtime_main(BURROW_FN(Func, many_readers, t));

    CHECK_INT_EQ(many_opened, MANY);
    CHECK_INT_EQ(many_read, many_opened);

    for (uint32_t i = 0; i < many_opened; i++) {
        burrow__poll_unblock(many_pd[i]);
        burrow__poll_close(many_pd[i]);
        pipe_shut(&many_pipe[i]);
    }
}

/* ------------------------------------------------------------- the deadlines */

/* Writes until the kernel says the send buffer is full, so that the next write
 * on this socket is one that has to wait. There is no portable size for that
 * buffer, so the loop is bounded by something far larger than any of them and a
 * failure to fill is a failure of the test rather than a silent pass. */
static bool fill_socket(int fd) {
    static char buf[4096];

    for (int i = 0; i < 100000; i++) {
        ssize_t n = write(fd, buf, sizeof buf);
        if (n < 0)
            return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    return false;
}

static int64_t dl_elapsed;
static burrow__PollStatus dl_first;
static burrow__PollStatus dl_second;
static burrow__PollStatus dl_third;
static bool dl_set;

static void read_deadline_passes(void *env) {
    TestingT *t = env;

    if (!pipe_open(&shared))
        return;
    if (burrow__poll_open(shared.rd, &shared_pd) != 0)
        return;

    dl_set = burrow__poll_set_deadline(
        shared_pd, burrow__nanotime() + 40 * TIME_MILLISECOND, BURROW_POLL_READ);

    int64_t start = burrow__nanotime();
    dl_first = burrow__poll_wait(shared_pd, BURROW_POLL_READ);
    dl_elapsed = burrow__nanotime() - start;

    /* A deadline that has gone by stays gone by. This is the part that makes a
     * timed out connection stay timed out rather than quietly working again on
     * the next read. */
    dl_second = burrow__poll_wait(shared_pd, BURROW_POLL_READ);

    /* And clearing it puts the descriptor back to work. */
    (void)burrow__poll_set_deadline(shared_pd, 0, BURROW_POLL_READ);
    CHECK(put_byte(shared.wr));
    dl_third = burrow__poll_wait(shared_pd, BURROW_POLL_READ);
}

static void TestAReadDeadlineThatPassesWakesTheWaiter(TestingT *t) {
    shared = (Pipe){-1, -1};
    shared_pd = NULL;
    dl_set = false;
    dl_elapsed = 0;
    dl_first = BURROW_POLL_UNPOLLABLE;
    dl_second = BURROW_POLL_UNPOLLABLE;
    dl_third = BURROW_POLL_UNPOLLABLE;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, read_deadline_passes, t));

    CHECK(dl_set);
    CHECK_INT_EQ(dl_first, BURROW_POLL_TIMEOUT);
    CHECK_INT_EQ(dl_second, BURROW_POLL_TIMEOUT);
    CHECK_INT_EQ(dl_third, BURROW_POLL_READY);

    /* It waited, rather than answering at once. The bound is well under the
     * forty milliseconds asked for, because what is being checked is that a
     * timer ran and not how good the timer is at being on time. */
    CHECK(dl_elapsed > 20 * TIME_MILLISECOND);

    if (shared_pd != NULL) {
        burrow__poll_unblock(shared_pd);
        burrow__poll_close(shared_pd);
    }
    pipe_shut(&shared);
}

/* A socket rather than a pipe, because the second half of this asks about the
 * write direction and the read end of a pipe is never writable. */
static void deadline_already_past(void *env) {
    (void)env;

    if (!socket_pair(sock))
        return;
    if (burrow__poll_open(sock[0], &sock_pd) != 0)
        return;

    dl_set = burrow__poll_set_deadline(sock_pd, burrow__nanotime() - TIME_SECOND,
                                       BURROW_POLL_READ);

    int64_t start = burrow__nanotime();
    dl_first = burrow__poll_wait(sock_pd, BURROW_POLL_READ);
    dl_elapsed = burrow__nanotime() - start;

    /* The other direction was not asked about and is not affected by this one,
     * which is the reason the two are separate bits rather than one. */
    dl_second = burrow__poll_wait(sock_pd, BURROW_POLL_WRITE);
}

static void TestADeadlineAlreadyPastTimesOutWithoutParking(TestingT *t) {
    sock[0] = -1;
    sock[1] = -1;
    sock_pd = NULL;
    dl_set = false;
    dl_elapsed = 0;
    dl_first = BURROW_POLL_UNPOLLABLE;
    dl_second = BURROW_POLL_UNPOLLABLE;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, deadline_already_past, NULL));

    CHECK(dl_set);
    CHECK_INT_EQ(dl_first, BURROW_POLL_TIMEOUT);
    CHECK(dl_elapsed < 10 * TIME_MILLISECOND);
    CHECK_INT_EQ(dl_second, BURROW_POLL_READY);

    if (sock_pd != NULL) {
        burrow__poll_unblock(sock_pd);
        burrow__poll_close(sock_pd);
    }
    if (sock[0] >= 0)
        (void)close(sock[0]);
    if (sock[1] >= 0)
        (void)close(sock[1]);
}

static void write_after_a_pause(void *env) {
    (void)env;
    time_sleep(20 * TIME_MILLISECOND);
    wrote = put_byte(shared.wr);
    burrow__atomic_add_u32(&done, 1);
}

static void deadline_moves(void *env) {
    TestingT *t = env;

    if (!pipe_open(&shared))
        return;
    if (burrow__poll_open(shared.rd, &shared_pd) != 0)
        return;

    /* Armed close enough to be believable, then pushed far enough out that a
     * timer which fired on the first one would be seen. This is the case a
     * sequence number exists for in Go and that the clock reading covers here. */
    CHECK(burrow__poll_set_deadline(
        shared_pd, burrow__nanotime() + 20 * TIME_MILLISECOND, BURROW_POLL_READ));
    time_sleep(10 * TIME_MILLISECOND);
    CHECK(burrow__poll_set_deadline(shared_pd, burrow__nanotime() + 5 * TIME_SECOND,
                                    BURROW_POLL_READ));

    CHECK(go(BURROW_FN(Func, write_after_a_pause, NULL)));

    int64_t start = burrow__nanotime();
    dl_first = burrow__poll_wait(shared_pd, BURROW_POLL_READ);
    dl_elapsed = burrow__nanotime() - start;
    CHECK(wait_for(&done, 1));
}

static void TestADeadlineMovedOutDoesNotFireOnTheOldOne(TestingT *t) {
    shared = (Pipe){-1, -1};
    shared_pd = NULL;
    wrote = false;
    done = 0;
    dl_elapsed = 0;
    dl_first = BURROW_POLL_UNPOLLABLE;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, deadline_moves, t));

    CHECK(wrote);
    CHECK_INT_EQ(dl_first, BURROW_POLL_READY);
    CHECK(dl_elapsed < TIME_SECOND);

    if (shared_pd != NULL) {
        burrow__poll_unblock(shared_pd);
        burrow__poll_close(shared_pd);
    }
    pipe_shut(&shared);
}

static void write_deadline_passes(void *env) {
    (void)env;

    if (!socket_pair(sock))
        return;
    if (burrow__poll_open(sock[0], &sock_pd) != 0)
        return;
    if (!fill_socket(sock[0]))
        return;

    dl_set = burrow__poll_set_deadline(
        sock_pd, burrow__nanotime() + 40 * TIME_MILLISECOND, BURROW_POLL_WRITE);

    int64_t start = burrow__nanotime();
    dl_first = burrow__poll_wait(sock_pd, BURROW_POLL_WRITE);
    dl_elapsed = burrow__nanotime() - start;
}

static void TestAWriteDeadlineThatPassesWakesTheWaiter(TestingT *t) {
    sock[0] = -1;
    sock[1] = -1;
    sock_pd = NULL;
    dl_set = false;
    dl_elapsed = 0;
    dl_first = BURROW_POLL_UNPOLLABLE;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, write_deadline_passes, NULL));

    CHECK(dl_set);
    CHECK_INT_EQ(dl_first, BURROW_POLL_TIMEOUT);
    CHECK(dl_elapsed > 20 * TIME_MILLISECOND);

    if (sock_pd != NULL) {
        burrow__poll_unblock(sock_pd);
        burrow__poll_close(sock_pd);
    }
    if (sock[0] >= 0)
        (void)close(sock[0]);
    if (sock[1] >= 0)
        (void)close(sock[1]);
}

static void both_directions_time_out(void *env) {
    (void)env;

    dl_third = burrow__poll_wait(sock_pd, BURROW_POLL_READ);
    burrow__atomic_add_u32(&done, 1);
}

static void one_deadline_both_ways(void *env) {
    TestingT *t = env;

    if (!socket_pair(sock))
        return;
    if (burrow__poll_open(sock[0], &sock_pd) != 0)
        return;
    if (!fill_socket(sock[0]))
        return;

    /* Both directions to the same instant, which is what SetDeadline does and
     * is the case that arms one timer rather than two. */
    dl_set =
        burrow__poll_set_deadline(sock_pd, burrow__nanotime() + 40 * TIME_MILLISECOND,
                                  BURROW_POLL_READ | BURROW_POLL_WRITE);

    CHECK(go(BURROW_FN(Func, both_directions_time_out, NULL)));
    dl_first = burrow__poll_wait(sock_pd, BURROW_POLL_WRITE);
    CHECK(wait_for(&done, 1));

    /* Moving one of them on its own splits the pair back into two timers, and
     * the one that was not moved has to stay where it was. */
    (void)burrow__poll_set_deadline(sock_pd, 0, BURROW_POLL_READ);
    dl_second = burrow__poll_wait(sock_pd, BURROW_POLL_WRITE);
}

static void TestOneDeadlineCoversBothDirections(TestingT *t) {
    sock[0] = -1;
    sock[1] = -1;
    sock_pd = NULL;
    done = 0;
    dl_set = false;
    dl_first = BURROW_POLL_UNPOLLABLE;
    dl_second = BURROW_POLL_UNPOLLABLE;
    dl_third = BURROW_POLL_UNPOLLABLE;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, one_deadline_both_ways, t));

    CHECK(dl_set);
    CHECK_INT_EQ(dl_first, BURROW_POLL_TIMEOUT);
    CHECK_INT_EQ(dl_third, BURROW_POLL_TIMEOUT);
    CHECK_INT_EQ(dl_second, BURROW_POLL_TIMEOUT);

    if (sock_pd != NULL) {
        burrow__poll_unblock(sock_pd);
        burrow__poll_close(sock_pd);
    }
    if (sock[0] >= 0)
        (void)close(sock[0]);
    if (sock[1] >= 0)
        (void)close(sock[1]);
}

/* ------------------------------------------------- the one that needs the poller */

static burrow__Thread outsider;
static bool outsider_started;
static bool woken_from_outside;

static void write_from_outside(void *arg) {
    (void)arg;

    /* Not a goroutine and not an M, so time_sleep is not available here and a
     * gate nobody ever opens is what stands in for it. By the time this writes,
     * the program has one goroutine and it is parked on a descriptor, so the
     * scheduler has nothing runnable anywhere and every thread has stopped. */
    burrow__Note gate;
    if (burrow__note_init(&gate)) {
        (void)burrow__note_sleep_timeout(&gate, 50 * 1000 * 1000);
        burrow__note_free(&gate);
    }
    (void)put_byte(shared.wr);
}

static void sleep_in_the_poller(void *env) {
    (void)env;

    if (!pipe_open(&shared))
        return;
    if (burrow__poll_open(shared.rd, &shared_pd) != 0)
        return;

    outsider_started = burrow__thread_start(&outsider, write_from_outside, NULL, 0);
    if (!outsider_started)
        return;

    woken_from_outside = get_byte(shared_pd, shared.rd);
    (void)burrow__thread_join(&outsider);
}

static void TestTheLastThreadSleepsInsideThePoller(TestingT *t) {
    shared = (Pipe){-1, -1};
    shared_pd = NULL;
    outsider_started = false;
    woken_from_outside = false;

    /* One P, so there is exactly one thread and no second one to stumble over
     * the event on its way past. With more than one this could pass for the
     * wrong reason. */
    (void)runtime_gomaxprocs(1);

    runtime_main(BURROW_FN(Func, sleep_in_the_poller, NULL));

    CHECK(outsider_started);
    CHECK(woken_from_outside);

    if (shared_pd != NULL) {
        burrow__poll_unblock(shared_pd);
        burrow__poll_close(shared_pd);
    }
    pipe_shut(&shared);
}

/* Again, because the interesting bugs in this area are in teardown and they
 * only show up on the run after the one that made them. */
static void TestTheLastThreadSleepsInsideThePollerAgain(TestingT *t) {
    shared = (Pipe){-1, -1};
    shared_pd = NULL;
    outsider_started = false;
    woken_from_outside = false;
    (void)runtime_gomaxprocs(1);

    runtime_main(BURROW_FN(Func, sleep_in_the_poller, NULL));

    CHECK(outsider_started);
    CHECK(woken_from_outside);

    if (shared_pd != NULL) {
        burrow__poll_unblock(shared_pd);
        burrow__poll_close(shared_pd);
    }
    pipe_shut(&shared);
}

#define TESTS(X)                                                                       \
    X(TestADescriptorCanBeTakenInAndGivenBack)                                         \
    X(TestAGoroutineParkedOnAReadIsWokenByAWrite)                                      \
    X(TestAWaitToWriteOnAnEmptyPipeComesStraightBack)                                  \
    X(TestAReaderAndAWriterShareOneDescriptor)                                         \
    X(TestUnblockingWakesTheWaiterAndStaysClosed)                                      \
    X(TestManyDescriptorsAndManyWaitersAtOnce)                                         \
    X(TestAReadDeadlineThatPassesWakesTheWaiter)                                       \
    X(TestADeadlineAlreadyPastTimesOutWithoutParking)                                  \
    X(TestADeadlineMovedOutDoesNotFireOnTheOldOne)                                     \
    X(TestAWriteDeadlineThatPassesWakesTheWaiter)                                      \
    X(TestOneDeadlineCoversBothDirections)                                             \
    X(TestTheLastThreadSleepsInsideThePoller)                                          \
    X(TestTheLastThreadSleepsInsideThePollerAgain)

TESTING_MAIN_BARE(TESTS)

#endif /* BURROW_NETPOLL_READINESS */
