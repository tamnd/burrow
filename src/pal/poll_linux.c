/* Readiness notification on Linux, which is epoll and an eventfd.
 *
 * This was src/runtime/netpoll_epoll.c until the platform layer existed. What
 * changed on the way in is that it no longer knows what a goroutine is and no
 * longer throws: a failure comes back as a PalErrno and the caller decides
 * whether the program can go on. What did not change is any of the reasoning,
 * which is still the interesting part of the file.
 *
 * Registration is edge triggered and covers both directions at once, which is
 * what makes it one system call per connection for the life of the connection.
 * EPOLLRDHUP is in there because without it a half closed connection is not a
 * read event on some kernels, and a goroutine waiting to read one would wait
 * for a close it has already been told about.
 *
 * This calls libc rather than making the system calls itself, which bends PAL
 * rule 2. Go makes them directly because Go cannot afford to be on a libc
 * thread stack at that moment, which is a constraint burrow does not have: an M
 * here is an ordinary pthread. glibc, musl and bionic all put these behind the
 * same four headers.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_LINUX)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

/* The data the eventfd is registered with. A caller's user pointer can never be
 * this, because pal_poll_add refuses that one value and pal.h says so. It has
 * to come from the same space the caller's pointers come from, since the whole
 * point of the user pointer is that there is no table to look anything up in. */
#define BREAK_TAG ((uint64_t)(uintptr_t)-1)

/* All three set once, by pal_poll_create, which is the only call allowed to
 * touch them and runs before anything else here can. Read without any
 * synchronisation afterwards, which is safe because nothing writes them again.
 */
static int epfd = -1;
static int breakfd = -1;

/* Whether a wakeup is already on its way, so that a thousand goroutines readied
 * at once cost one write to the eventfd rather than a thousand. Cleared by
 * whoever drains the eventfd, which is a thread that was going to sleep. */
static uint32_t wakesig;

/* The eventfd is registered level triggered, which is the one thing in this
 * file that is not edge triggered and is deliberate. A poll that was only
 * looking can pick a wakeup up, and if taking it off the kernel were enough to
 * consume it then the thread the wakeup was meant for would stay asleep. Level
 * triggered means the count stays on the eventfd until a thread that is going
 * to sleep drains it, so a look costs one spurious return later and never a
 * lost one. See the same paragraph in poll_bsd.c, where the choice costs more.
 */
int64_t pal_poll_create(PalErrno *err) {
    if (epfd >= 0) {
        BURROW_OUT(err, PAL_EBUSY);
        return PAL_INVALID_HANDLE;
    }

    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep < 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return PAL_INVALID_HANDLE;
    }

    int bf = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (bf < 0) {
        PalErrno e = burrow__pal_errno(errno);
        (void)close(ep);
        BURROW_OUT(err, e);
        return PAL_INVALID_HANDLE;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.u64 = BREAK_TAG;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, bf, &ev) != 0) {
        PalErrno e = burrow__pal_errno(errno);
        (void)close(bf);
        (void)close(ep);
        BURROW_OUT(err, e);
        return PAL_INVALID_HANDLE;
    }

    epfd = ep;
    breakfd = bf;
    BURROW_OUT(err, PAL_OK);
    return (int64_t)ep;
}

/* Every call below takes the handle and has to agree it is the poller. A wrong
 * one is a caller bug rather than a system failure, so it is PAL_EINVAL and not
 * a guess at what was meant. */
static bool is_poller(int64_t poll) {
    return epfd >= 0 && poll == (int64_t)epfd;
}

bool pal_poll_add(int64_t poll, int64_t fd, void *user, PalErrno *err) {
    if (!is_poller(poll) || (uintptr_t)user == BREAK_TAG || fd < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    struct epoll_event ev;
    ev.events = (uint32_t)(EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET);
    ev.data.u64 = (uint64_t)(uintptr_t)user;

    if (epoll_ctl(epfd, EPOLL_CTL_ADD, (int)fd, &ev) != 0) {
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

    /* The structure is not read for a delete on kernels since 2.6.9 and is not
     * allowed to be NULL on the ones before that. Passing one costs nothing and
     * is what Go does. */
    struct epoll_event ev;
    ev.events = 0;
    ev.data.u64 = 0;

    if (epoll_ctl(epfd, EPOLL_CTL_DEL, (int)fd, &ev) != 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }

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

    uint64_t one = 1;
    for (;;) {
        ssize_t n = write(breakfd, &one, sizeof one);
        if (n == (ssize_t)sizeof one)
            return true;
        if (errno == EINTR)
            continue;

        /* The counter is at its maximum, which means a great many wakeups have
         * been asked for and none delivered. The next wait comes straight back
         * out whatever happens, so the wakeup this call owed has been paid. */
        if (errno == EAGAIN)
            return true;

        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }
}

/* epoll takes milliseconds, so the nanoseconds the caller works in have to be
 * rounded, and the rounding has to be upwards. A sleep that is short by a
 * fraction of a millisecond is a thread that wakes up, finds the timer it was
 * waiting for is not due yet, and goes back to sleep, which happens again on
 * every pass and is a loop rather than a delay. */
static int wait_ms(int64_t timeout_ns) {
    if (timeout_ns < 0)
        return -1;
    if (timeout_ns == 0)
        return 0;
    if (timeout_ns < 1000000)
        return 1;

    int64_t ms = timeout_ns / 1000000;

    /* Eleven and a half days, which is Go's cap and is not a limit on how long
     * a timer can be: a wait that ends with nothing to do goes round again. It
     * is a bound on the argument, so that an absurd deadline cannot turn into a
     * negative millisecond count on the way in. */
    if (ms > 1000000000)
        ms = 1000000000;
    return (int)ms;
}

int64_t pal_poll_wait(int64_t poll, PalPollEvent *out, int64_t cap, int64_t timeout_ns,
                      PalErrno *err) {
    if (!is_poller(poll) || out == NULL || cap <= 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }

    /* One kernel call takes at most as many events as the caller has room for,
     * and at most what fits on this stack. 128 is Go's number: a kilobyte and a
     * half on the thread doing the least work in the program, and enough that a
     * busy server is not going round again for the sake of it. */
    struct epoll_event events[128];
    int want = cap < (int64_t)(sizeof events / sizeof events[0])
                   ? (int)cap
                   : (int)(sizeof events / sizeof events[0]);
    int ms = wait_ms(timeout_ns);

    int n;
    for (;;) {
        n = epoll_wait(epfd, events, want, ms);
        if (n >= 0)
            break;
        if (errno != EINTR) {
            BURROW_OUT(err, burrow__pal_errno(errno));
            return -1;
        }

        /* A signal cut the sleep short. There is no way to know how much of the
         * deadline is left, so a timed wait gives up and lets the caller work
         * that out again from its own timers. An untimed one goes round. */
        if (ms > 0) {
            BURROW_OUT(err, PAL_OK);
            return 0;
        }
    }

    BURROW_OUT(err, PAL_OK);

    int64_t got = 0;
    for (int i = 0; i < n; i++) {
        struct epoll_event *ev = &events[i];
        if (ev->events == 0)
            continue;

        if (ev->data.u64 == BREAK_TAG) {
            /* Only drained by a wait that was going to block. A wait that was
             * only looking can pick up somebody else's wakeup, and taking it
             * off the eventfd here would swallow it: the thread it was meant
             * for would go to sleep with the reason for waking it already
             * gone. */
            if (timeout_ns != 0) {
                uint64_t seen;
                ssize_t drained = read(breakfd, &seen, sizeof seen);
                (void)drained;
                burrow__atomic_store_u32(&wakesig, 0);
            }
            continue;
        }

        uint32_t ready = 0;
        if ((ev->events & (uint32_t)(EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR)) != 0)
            ready |= PAL_POLL_READY_READ;
        if ((ev->events & (uint32_t)(EPOLLOUT | EPOLLHUP | EPOLLERR)) != 0)
            ready |= PAL_POLL_READY_WRITE;

        /* An error and nothing else. Both directions are woken above, and this
         * bit is what says the wakeup is a failure to report rather than
         * something worth reading. */
        if (ev->events == (uint32_t)EPOLLERR)
            ready |= PAL_POLL_READY_ERROR;

        if (ready == 0)
            continue;

        out[got].user = (void *)(uintptr_t)ev->data.u64;
        out[got].ready = ready;
        out[got].bytes = 0;
        out[got].status = 0;
        got++;
    }

    return got;
}

#endif /* linux epoll */
