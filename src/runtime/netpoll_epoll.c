/* The netpoller on Linux, which is epoll and an eventfd.
 *
 * burrow/netpoll.h says what a backend is for and src/runtime/netpoll.c is
 * everything this does not have to think about. What is left here is one epoll
 * descriptor, one eventfd to interrupt a wait with, and the translation between
 * what epoll reports and what the layer above understands.
 *
 * Registration is edge triggered and covers both directions at once, which is
 * what makes it a single call per connection for the life of the connection.
 * EPOLLRDHUP is in there because without it a half closed connection is not a
 * read event on some kernels, and a goroutine waiting to read one would wait
 * for a close it has already been told about.
 *
 * This calls libc rather than making the system calls itself. Go makes them
 * directly because Go cannot afford to be on a libc thread stack at that
 * moment, which is a constraint burrow does not have: an M here is an ordinary
 * pthread. glibc, musl and bionic all put these behind the same four headers.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/netpoll.h"

#if defined(BURROW_NETPOLL_EPOLL)

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

/* The handle the eventfd is registered with, which is the one value the layer
 * above promises never to hand out. See burrow__netpoll_backend_open there. */
#define BREAK_HANDLE ((uint64_t)(uintptr_t)-1)

/* How many events one call takes off the kernel at a time. Go's number. A
 * kilobyte and a half of stack on the thread that is doing the least work in
 * the program, and enough that a busy server is not going round again for the
 * sake of it. */
#define EVENTS 128

/* Both set once, in the init below, which runs under a lock and runs before
 * anything else here can be called. Read without any of that afterwards. */
static int epfd = -1;
static int breakfd = -1;

/* Whether a wakeup is already on its way, so that a thousand goroutines readied
 * at once cost one write to the eventfd rather than a thousand. Cleared by
 * whoever drains the eventfd, which is the thread that was asleep. */
static uint32_t wakesig;

/* The eventfd is registered level triggered, which is the one place in this
 * file that is not edge triggered and is deliberate. A poll that was only
 * looking can pick a wakeup up, and if taking it off the kernel were enough to
 * consume it then the thread the wakeup was meant for would stay asleep. Level
 * triggered means the count stays on the eventfd until a thread that was going
 * to sleep drains it, so a look costs one spurious return later and never a
 * lost one. See the same paragraph in netpoll_kqueue.c, where the choice costs
 * more. */
void burrow__netpoll_backend_init(void) {
    epfd = epoll_create1(EPOLL_CLOEXEC);
    if (epfd < 0)
        runtime_throw(BURROW_S("netpoll: epoll_create1 failed"));

    breakfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (breakfd < 0)
        runtime_throw(BURROW_S("netpoll: eventfd failed"));

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u64 = BREAK_HANDLE;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, breakfd, &ev) != 0)
        runtime_throw(BURROW_S("netpoll: epoll_ctl on the wakeup descriptor failed"));
}

int burrow__netpoll_backend_open(burrow__PollFd fd, uintptr_t handle) {
    struct epoll_event ev;
    ev.events = (uint32_t)(EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET);
    ev.data.u64 = (uint64_t)handle;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0)
        return errno;
    return 0;
}

int burrow__netpoll_backend_close(burrow__PollFd fd) {
    /* The structure is not read for a delete on kernels since 2.6.9 and is not
     * allowed to be NULL on the ones before that. Passing one costs nothing and
     * is what Go does. */
    struct epoll_event ev;
    ev.events = 0;
    ev.data.u64 = 0;

    if (epoll_ctl(epfd, EPOLL_CTL_DEL, fd, &ev) != 0)
        return errno;
    return 0;
}

void burrow__netpoll_backend_break(void) {
    uint32_t none = 0;
    if (!burrow__atomic_cas_u32(&wakesig, &none, 1))
        return;

    uint64_t one = 1;
    for (;;) {
        ssize_t n = write(breakfd, &one, sizeof one);
        if (n == (ssize_t)sizeof one)
            return;
        if (errno == EINTR)
            continue;

        /* The counter is at its maximum, which means a great many wakeups have
         * been asked for and none delivered. The next poll will come straight
         * back out whatever happens, so there is nothing left to do. */
        if (errno == EAGAIN)
            return;

        runtime_throw(BURROW_S("netpoll: the write that wakes the poller failed"));
    }
}

/* epoll takes milliseconds, so the nanoseconds the scheduler works in have to
 * be rounded, and the rounding has to be upwards. A sleep that is short by a
 * fraction of a millisecond is a thread that wakes up, finds the timer it was
 * waiting for is not due yet, and goes back to sleep, which happens again on
 * every pass and is a loop rather than a delay. */
static int wait_ms(int64_t delay) {
    if (delay < 0)
        return -1;
    if (delay == 0)
        return 0;
    if (delay < 1000000)
        return 1;

    int64_t ms = delay / 1000000;

    /* Eleven and a half days, which is Go's cap and is not a limit on how long
     * a timer can be: a poll that ends with nothing to do goes round again. It
     * is a bound on the argument, so that an absurd deadline cannot turn into a
     * negative millisecond count on the way in. */
    if (ms > 1000000000)
        ms = 1000000000;
    return (int)ms;
}

void burrow__netpoll_backend_wait(int64_t delay, burrow__GQueue *out) {
    int ms = wait_ms(delay);
    struct epoll_event events[EVENTS];

    int n;
    for (;;) {
        n = epoll_wait(epfd, events, EVENTS, ms);
        if (n >= 0)
            break;
        if (errno != EINTR)
            runtime_throw(BURROW_S("netpoll: epoll_wait failed"));

        /* A signal cut the sleep short. There is no way to know how much of the
         * deadline is left, so a timed wait gives up and lets the scheduler
         * work that out again from the timers. An untimed one goes round. */
        if (ms > 0)
            return;
    }

    for (int i = 0; i < n; i++) {
        struct epoll_event *ev = &events[i];
        if (ev->events == 0)
            continue;

        if (ev->data.u64 == BREAK_HANDLE) {
            /* Only drained by a wait that was going to block. A poll that was
             * only looking can pick up somebody else's wakeup, and taking it
             * off the eventfd here would swallow it: the thread it was meant
             * for would go to sleep with the reason for waking it already
             * gone. */
            if (delay != 0) {
                uint64_t seen;
                ssize_t got = read(breakfd, &seen, sizeof seen);
                (void)got;
                burrow__atomic_store_u32(&wakesig, 0);
            }
            continue;
        }

        uint32_t mode = 0;
        if ((ev->events & (uint32_t)(EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0)
            mode |= BURROW_POLL_READ;
        if ((ev->events & (uint32_t)(EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0)
            mode |= BURROW_POLL_WRITE;

        if (mode != 0)
            burrow__netpoll_ready(out, (uintptr_t)ev->data.u64, mode,
                                  ev->events == (uint32_t)EPOLLERR);
    }
}

#endif /* BURROW_NETPOLL_EPOLL */
