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
 * On a platform with no backend yet this file compiles to a main that says so.
 * Windows is that platform today.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/netpoll.h"

#include "harness.h"

#if defined(BURROW_NETPOLL_NONE)

int main(void) {
    printf("ok\tnetpoll\t0 checks (no backend on this platform)\n");
    return 0;
}

#else

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

TEST(a_descriptor_can_be_taken_in_and_given_back) {
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
    (void)env;

    if (!pipe_open(&shared))
        return;
    if (burrow__poll_open(shared.rd, &shared_pd) != 0)
        return;

    CHECK(go(BURROW_FN(Func, reader, NULL)));
    wait_then_write(NULL);
    CHECK(wait_for(&done, 1));
}

TEST(a_goroutine_parked_on_a_read_is_woken_by_a_write) {
    shared = (Pipe){-1, -1};
    shared_pd = NULL;
    read_got = false;
    wrote = false;
    done = 0;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, one_reader_one_writer, NULL));

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

TEST(a_wait_to_write_on_an_empty_pipe_comes_straight_back) {
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
    (void)env;

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

TEST(a_reader_and_a_writer_share_one_descriptor) {
    sock[0] = -1;
    sock[1] = -1;
    sock_pd = NULL;
    both_read = false;
    both_wrote = false;
    done = 0;
    (void)runtime_gomaxprocs(4);

    runtime_main(BURROW_FN(Func, two_directions, NULL));

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
    (void)env;

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

TEST(unblocking_wakes_the_waiter_and_stays_closed) {
    shared = (Pipe){-1, -1};
    shared_pd = NULL;
    closed_status = BURROW_POLL_UNPOLLABLE;
    after_status = BURROW_POLL_UNPOLLABLE;
    done = 0;
    (void)runtime_gomaxprocs(2);

    runtime_main(BURROW_FN(Func, unblock_it, NULL));

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
    (void)env;

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

TEST(many_descriptors_and_many_waiters_at_once) {
    many_read = 0;
    many_opened = 0;
    (void)runtime_gomaxprocs(4);

    runtime_main(BURROW_FN(Func, many_readers, NULL));

    CHECK_INT_EQ(many_opened, MANY);
    CHECK_INT_EQ(many_read, many_opened);

    for (uint32_t i = 0; i < many_opened; i++) {
        burrow__poll_unblock(many_pd[i]);
        burrow__poll_close(many_pd[i]);
        pipe_shut(&many_pipe[i]);
    }
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

TEST(the_last_thread_sleeps_inside_the_poller) {
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
TEST(the_last_thread_sleeps_inside_the_poller_again) {
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

int main(void) {
    RUN(a_descriptor_can_be_taken_in_and_given_back);
    RUN(a_goroutine_parked_on_a_read_is_woken_by_a_write);
    RUN(a_wait_to_write_on_an_empty_pipe_comes_straight_back);
    RUN(a_reader_and_a_writer_share_one_descriptor);
    RUN(unblocking_wakes_the_waiter_and_stays_closed);
    RUN(many_descriptors_and_many_waiters_at_once);
    RUN(the_last_thread_sleeps_inside_the_poller);
    RUN(the_last_thread_sleeps_inside_the_poller_again);
    return harness_report("netpoll");
}

#endif /* BURROW_NETPOLL_NONE */
