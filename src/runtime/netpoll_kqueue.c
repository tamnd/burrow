/* The netpoller on macOS and the BSDs, which is kqueue.
 *
 * burrow/netpoll.h says what a backend is for and src/runtime/netpoll.c is
 * everything this does not have to think about. What is left here is one kqueue
 * descriptor, a way to interrupt a wait, and the translation between what
 * kqueue reports and what the layer above understands.
 *
 * kqueue has one filter per direction rather than one registration covering
 * both, so opening a descriptor arms two kevents instead of one. EV_CLEAR is
 * the edge triggered flag, and it is set for the same reason EPOLLET is set on
 * Linux: a descriptor that stays readable would otherwise be reported on every
 * single wait, by every thread, forever.
 *
 * Closing is the easy half. The kernel drops every kevent on a descriptor when
 * the descriptor closes, so there is no delete to make and no window where a
 * stale registration could fire.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/netpoll.h"

#if defined(BURROW_NETPOLL_KQUEUE)

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/event.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* The udata the wakeup is registered with, which is the one value the layer
 * above promises never to hand out. See burrow__netpoll_backend_open there. */
#define BREAK_HANDLE ((uintptr_t)-1)

/* How many events one call takes off the kernel at a time. Go's number, and
 * half of what the epoll backend uses, for no reason beyond that being what
 * each of them has always had. */
#define EVENTS 64

/* All three set once, in the init below, which runs under a lock and runs
 * before anything else here can be called. Read without any of that after. */
static int kq = -1;
static int breakrd = -1;
static int breakwr = -1;

/* Whether a wakeup is already on its way, so that a thousand goroutines readied
 * at once cost one byte down the pipe rather than a thousand. Cleared by
 * whoever drains the pipe, which is a thread that was going to sleep. */
static uint32_t wakesig;

static void set_pipe_flags(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
        runtime_throw(BURROW_S("netpoll: could not make the wakeup pipe non-blocking"));

    flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0)
        runtime_throw(
            BURROW_S("netpoll: could not set close-on-exec on the wakeup pipe"));
}

/* The wakeup is a pipe and a byte down it, on every one of these platforms.
 *
 * Go uses EVFILT_USER where there is one, which is an event with no descriptor
 * behind it, and falls back to a pipe on NetBSD and OpenBSD. It is the nicer
 * mechanism and it is the wrong one here, because a user event has to be
 * registered with EV_CLEAR and EV_CLEAR means the kernel forgets it the moment
 * anybody receives it. A poll that was only looking is anybody. It takes the
 * wakeup, has nowhere to put it, and the thread the wakeup was for stays
 * asleep, which in Go is a thread that gets replaced by a new one and here is a
 * program that stops.
 *
 * A pipe registered without EV_CLEAR is level triggered, so the byte stays
 * there until somebody who is going to sleep takes it out. A poll that was only
 * looking sees it, leaves it, and the next thread to go to sleep comes straight
 * back out. That is one spurious return in exchange for never losing one, and
 * it makes this file the same shape as the epoll one rather than two files with
 * different rules. Two descriptors is what it costs. */
void burrow__netpoll_backend_init(void) {
    kq = kqueue();
    if (kq < 0)
        runtime_throw(BURROW_S("netpoll: kqueue failed"));

    int flags = fcntl(kq, F_GETFD, 0);
    if (flags < 0 || fcntl(kq, F_SETFD, flags | FD_CLOEXEC) != 0)
        runtime_throw(BURROW_S("netpoll: could not set close-on-exec on the kqueue"));

    int fds[2];
    if (pipe(fds) != 0)
        runtime_throw(BURROW_S("netpoll: pipe failed"));
    breakrd = fds[0];
    breakwr = fds[1];
    set_pipe_flags(breakrd);
    set_pipe_flags(breakwr);

    struct kevent ev;
    EV_SET(&ev, (uintptr_t)breakrd, EVFILT_READ, EV_ADD, 0, 0, (void *)BREAK_HANDLE);
    if (kevent(kq, &ev, 1, NULL, 0, NULL) != 0)
        runtime_throw(BURROW_S("netpoll: registering the wakeup event failed"));
}

int burrow__netpoll_backend_open(burrow__PollFd fd, uintptr_t handle) {
    struct kevent ev[2];
    EV_SET(&ev[0], (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, (void *)handle);
    EV_SET(&ev[1], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0,
           (void *)handle);

    if (kevent(kq, ev, 2, NULL, 0, NULL) != 0)
        return errno;
    return 0;
}

int burrow__netpoll_backend_close(burrow__PollFd fd) {
    /* Nothing to undo. The kernel takes the kevents off when the descriptor
     * closes, and the caller closes it right after this returns. */
    (void)fd;
    return 0;
}

void burrow__netpoll_backend_break(void) {
    uint32_t none = 0;
    if (!burrow__atomic_cas_u32(&wakesig, &none, 1))
        return;

    char byte = 0;
    for (;;) {
        ssize_t n = write(breakwr, &byte, 1);
        if (n == 1)
            return;
        if (errno == EINTR)
            continue;

        /* The pipe is full, which means a great many wakeups have been asked
         * for and none drained. The next poll will come straight back out
         * whatever happens, so there is nothing left to do. */
        if (errno == EAGAIN)
            return;

        runtime_throw(BURROW_S("netpoll: the write that wakes the poller failed"));
    }
}

void burrow__netpoll_backend_wait(int64_t delay, burrow__GQueue *out) {
    struct timespec ts;
    struct timespec *timeout = NULL;

    if (delay >= 0) {
        ts.tv_sec = (time_t)(delay / 1000000000);
        ts.tv_nsec = (long)(delay % 1000000000);
        timeout = &ts;
    }

    struct kevent events[EVENTS];
    int n;
    for (;;) {
        n = kevent(kq, NULL, 0, events, EVENTS, timeout);
        if (n >= 0)
            break;

        /* ETIMEDOUT is not documented as a kevent error and DragonFly returns
         * it anyway. It means the same thing as a return of zero. */
        if (errno == ETIMEDOUT)
            return;
        if (errno != EINTR)
            runtime_throw(BURROW_S("netpoll: kevent failed"));

        /* A signal cut the sleep short. There is no way to know how much of the
         * deadline is left, so a timed wait gives up and lets the scheduler
         * work that out again from the timers. An untimed one goes round. */
        if (timeout != NULL)
            return;
    }

    for (int i = 0; i < n; i++) {
        struct kevent *ev = &events[i];

        if ((uintptr_t)ev->udata == BREAK_HANDLE) {
            /* Only drained by a wait that was going to block, which is the
             * whole reason the pipe is level triggered. A poll that was only
             * looking leaves the byte where it is, so the wakeup is still there
             * for the thread it was meant for. */
            if (delay != 0) {
                char drain[16];
                for (;;) {
                    ssize_t got = read(breakrd, drain, sizeof drain);
                    if (got < (ssize_t)sizeof drain)
                        break;
                }
                burrow__atomic_store_u32(&wakesig, 0);
            }
            continue;
        }

        uint32_t mode = 0;
        if (ev->filter == EVFILT_READ)
            mode |= BURROW_POLL_READ;
        if (ev->filter == EVFILT_WRITE)
            mode |= BURROW_POLL_WRITE;

        /* End of file on the read side is also the end of the write side, and
         * a goroutine blocked writing to a socket the far end has gone away
         * from has to find that out. Without this it waits for a write event
         * the kernel has no reason left to send. */
        if (mode == BURROW_POLL_READ && (ev->flags & EV_EOF) != 0)
            mode |= BURROW_POLL_WRITE;

        if (mode != 0)
            burrow__netpoll_ready(out, (uintptr_t)ev->udata, mode,
                                  ev->flags == EV_ERROR);
    }
}

#endif /* BURROW_NETPOLL_KQUEUE */
