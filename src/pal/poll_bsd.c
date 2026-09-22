/* Readiness notification on macOS and the BSDs, which is kqueue.
 *
 * This was src/runtime/netpoll_kqueue.c until the platform layer existed. What
 * changed on the way in is that it no longer knows what a goroutine is and no
 * longer throws: a failure comes back as a PalErrno and the caller decides
 * whether the program can go on. What did not change is any of the reasoning.
 *
 * kqueue has one filter per direction rather than one registration covering
 * both, so adding a descriptor arms two kevents instead of one. EV_CLEAR is the
 * edge triggered flag, and it is set for the same reason EPOLLET is set on
 * Linux: a descriptor that stays readable would otherwise be reported on every
 * single wait, by every thread, forever.
 *
 * Removing is the easy half. The kernel drops every kevent on a descriptor when
 * the descriptor closes, so there is no delete to make and no window where a
 * stale registration could fire.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) ||                             \
    defined(BURROW_OS_FREEBSD) || defined(BURROW_OS_OPENBSD) ||                        \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_DRAGONFLY)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/event.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* The udata the wakeup is registered with. A caller's user pointer can never be
 * this, because pal_poll_add refuses that one value and pal.h says so. It has
 * to come from the same space the caller's pointers come from, since the whole
 * point of the user pointer is that there is no table to look anything up in. */
#define BREAK_TAG ((uintptr_t)-1)

/* All three set once, by pal_poll_create, which is the only call allowed to
 * touch them and runs before anything else here can. Read without any
 * synchronisation afterwards, which is safe because nothing writes them again.
 */
static int kq = -1;
static int breakrd = -1;
static int breakwr = -1;

/* Whether a wakeup is already on its way, so that a thousand goroutines readied
 * at once cost one byte down the pipe rather than a thousand. Cleared by
 * whoever drains the pipe, which is a thread that was going to sleep. */
static uint32_t wakesig;

static bool set_pipe_flags(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
        return false;

    flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0)
        return false;

    return true;
}

/* The wakeup is a pipe and a byte down it, on every one of these platforms.
 *
 * Go uses EVFILT_USER where there is one, which is an event with no descriptor
 * behind it, and falls back to a pipe on NetBSD and OpenBSD. It is the nicer
 * mechanism and it is the wrong one here, because a user event has to be
 * registered with EV_CLEAR and EV_CLEAR means the kernel forgets it the moment
 * anybody receives it. A wait that was only looking is anybody. It takes the
 * wakeup, has nowhere to put it, and the thread the wakeup was for stays
 * asleep, which in Go is a thread that gets replaced by a new one and here is a
 * program that stops.
 *
 * A pipe registered without EV_CLEAR is level triggered, so the byte stays
 * there until somebody who is going to sleep takes it out. A wait that was only
 * looking sees it, leaves it, and the next thread to go to sleep comes straight
 * back out. That is one spurious return in exchange for never losing one, and
 * it makes this file the same shape as poll_linux.c rather than two files with
 * different rules. Two descriptors is what it costs. */
int64_t pal_poll_create(PalErrno *err) {
    if (kq >= 0) {
        BURROW_OUT(err, PAL_EBUSY);
        return PAL_INVALID_HANDLE;
    }

    int q = kqueue();
    if (q < 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return PAL_INVALID_HANDLE;
    }

    int flags = fcntl(q, F_GETFD, 0);
    if (flags < 0 || fcntl(q, F_SETFD, flags | FD_CLOEXEC) != 0) {
        PalErrno e = burrow__pal_errno(errno);
        (void)close(q);
        BURROW_OUT(err, e);
        return PAL_INVALID_HANDLE;
    }

    int fds[2];
    if (pipe(fds) != 0) {
        PalErrno e = burrow__pal_errno(errno);
        (void)close(q);
        BURROW_OUT(err, e);
        return PAL_INVALID_HANDLE;
    }

    if (!set_pipe_flags(fds[0]) || !set_pipe_flags(fds[1])) {
        PalErrno e = burrow__pal_errno(errno);
        (void)close(fds[0]);
        (void)close(fds[1]);
        (void)close(q);
        BURROW_OUT(err, e);
        return PAL_INVALID_HANDLE;
    }

    struct kevent ev;
    EV_SET(&ev, (uintptr_t)fds[0], EVFILT_READ, EV_ADD, 0, 0, (void *)BREAK_TAG);
    if (kevent(q, &ev, 1, NULL, 0, NULL) != 0) {
        PalErrno e = burrow__pal_errno(errno);
        (void)close(fds[0]);
        (void)close(fds[1]);
        (void)close(q);
        BURROW_OUT(err, e);
        return PAL_INVALID_HANDLE;
    }

    kq = q;
    breakrd = fds[0];
    breakwr = fds[1];
    BURROW_OUT(err, PAL_OK);
    return (int64_t)q;
}

/* Every call below takes the handle and has to agree it is the poller. A wrong
 * one is a caller bug rather than a system failure, so it is PAL_EINVAL and not
 * a guess at what was meant. */
static bool is_poller(int64_t poll) {
    return kq >= 0 && poll == (int64_t)kq;
}

bool pal_poll_add(int64_t poll, int64_t fd, void *user, PalErrno *err) {
    if (!is_poller(poll) || (uintptr_t)user == BREAK_TAG || fd < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    struct kevent ev[2];
    EV_SET(&ev[0], (uintptr_t)fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, user);
    EV_SET(&ev[1], (uintptr_t)fd, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, user);

    if (kevent(kq, ev, 2, NULL, 0, NULL) != 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }

    BURROW_OUT(err, PAL_OK);
    return true;
}

bool pal_poll_del(int64_t poll, int64_t fd, PalErrno *err) {
    if (!is_poller(poll) || fd < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    /* Nothing to undo. The kernel takes the kevents off when the descriptor
     * closes, and the caller closes it right after this returns. */
    BURROW_OUT(err, PAL_OK);
    return true;
}

bool pal_poll_break(int64_t poll, PalErrno *err) {
    if (!is_poller(poll)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    BURROW_OUT(err, PAL_OK);

    uint32_t none = 0;
    if (!burrow__atomic_cas_u32(&wakesig, &none, 1))
        return true;

    char byte = 0;
    for (;;) {
        ssize_t n = write(breakwr, &byte, 1);
        if (n == 1)
            return true;
        if (errno == EINTR)
            continue;

        /* The pipe is full, which means a great many wakeups have been asked
         * for and none drained. The next wait comes straight back out whatever
         * happens, so the wakeup this call owed has been paid. */
        if (errno == EAGAIN)
            return true;

        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }
}

int64_t pal_poll_wait(int64_t poll, PalPollEvent *out, int64_t cap, int64_t timeout_ns,
                      PalErrno *err) {
    if (!is_poller(poll) || out == NULL || cap <= 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }

    struct timespec ts;
    struct timespec *timeout = NULL;
    if (timeout_ns >= 0) {
        ts.tv_sec = (time_t)(timeout_ns / 1000000000);
        ts.tv_nsec = (long)(timeout_ns % 1000000000);
        timeout = &ts;
    }

    /* 64 rather than the 128 in poll_linux.c, which is Go's number for each of
     * them and not a considered difference between the two kernels. */
    struct kevent events[64];
    int want = cap < (int64_t)(sizeof events / sizeof events[0])
                   ? (int)cap
                   : (int)(sizeof events / sizeof events[0]);

    int n;
    for (;;) {
        n = kevent(kq, NULL, 0, events, want, timeout);
        if (n >= 0)
            break;

        /* ETIMEDOUT is not documented as a kevent error and DragonFly returns
         * it anyway. It means the same thing as a return of zero. */
        if (errno == ETIMEDOUT) {
            BURROW_OUT(err, PAL_OK);
            return 0;
        }
        if (errno != EINTR) {
            BURROW_OUT(err, burrow__pal_errno(errno));
            return -1;
        }

        /* A signal cut the sleep short. There is no way to know how much of the
         * deadline is left, so a timed wait gives up and lets the caller work
         * that out again from its own timers. An untimed one goes round. */
        if (timeout != NULL) {
            BURROW_OUT(err, PAL_OK);
            return 0;
        }
    }

    BURROW_OUT(err, PAL_OK);

    int64_t got = 0;
    for (int i = 0; i < n; i++) {
        struct kevent *ev = &events[i];

        if ((uintptr_t)ev->udata == BREAK_TAG) {
            /* Only drained by a wait that was going to block, which is the
             * whole reason the pipe is level triggered. A wait that was only
             * looking leaves the byte where it is, so the wakeup is still there
             * for the thread it was meant for. */
            if (timeout_ns != 0) {
                char drain[16];
                for (;;) {
                    ssize_t drained = read(breakrd, drain, sizeof drain);
                    if (drained < (ssize_t)sizeof drain)
                        break;
                }
                burrow__atomic_store_u32(&wakesig, 0);
            }
            continue;
        }

        uint32_t ready = 0;
        if (ev->filter == EVFILT_READ)
            ready |= PAL_POLL_READY_READ;
        if (ev->filter == EVFILT_WRITE)
            ready |= PAL_POLL_READY_WRITE;

        /* End of file on the read side is also the end of the write side, and a
         * goroutine blocked writing to a socket the far end has gone away from
         * has to find that out. Without this it waits for a write event the
         * kernel has no reason left to send. */
        if (ready == PAL_POLL_READY_READ && (ev->flags & EV_EOF) != 0)
            ready |= PAL_POLL_READY_WRITE;

        /* An error and nothing else, which is what EV_ERROR on its own means. */
        if (ev->flags == EV_ERROR)
            ready |= PAL_POLL_READY_ERROR;

        if (ready == 0)
            continue;

        out[got].user = ev->udata;
        out[got].ready = ready;
        out[got].bytes = 0;
        out[got].status = 0;
        got++;
    }

    return got;
}

#endif /* bsd kqueue */
