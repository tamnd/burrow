/* Readiness notification with poll(2), on Cosmopolitan and wasip1.
 *
 * Neither has a poller of the kind the other files here wrap. Cosmopolitan has
 * epoll on Linux and Windows and nothing like it on macOS or the BSDs, and one
 * binary has to run on all of them. wasip1 has poll_oneoff, which wasi-libc's
 * poll is written on. poll is what both have, and it is what Go uses on AIX,
 * and in effect on wasip1 too.
 *
 * poll keeps no set of its own and is level triggered, which is the opposite
 * of what pal.h promises. So the set is kept here, in a table indexed by the
 * descriptor, and a descriptor is only in a wait for the directions somebody
 * has armed with pal_poll_arm. A direction that is reported is disarmed, so it
 * is reported once. To the layer above that looks like edge triggering, as
 * long as it arms before every wait, which burrow__poll_wait does here the way
 * Go's poll_runtime_pollWait calls netpollarm on AIX and wasip1.
 *
 * The table is static, since nothing in the PAL allocates, so a descriptor
 * from TABLE upwards can't be added. Go's own table on AIX grows, and its
 * waits go through every descriptor the same way this does.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_COSMO) || defined(BURROW_OS_WASI)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#if !defined(BURROW_OS_WASI)
#include <pthread.h>
#endif

#define BREAK_TAG ((uintptr_t)-1)

/* How many descriptors the table holds, which is FD_SETSIZE on most systems.
 * A wait puts a struct pollfd for each one on its stack, 8 KiB of them. */
#define TABLE 1024

/* What is known about one descriptor. armed is the directions somebody is
 * waiting for, as PAL_POLL_READ and PAL_POLL_WRITE, and is what goes into the
 * next wait. */
typedef struct PollEntry {
    void *user;
    uint32_t used;
    uint32_t armed;
} PollEntry;

/* The table, and one more than the highest descriptor in it, so that a wait
 * looks only as far as it has to. Both under table_mu. */
static PollEntry table[TABLE];
static int table_hi;

/* Whether pal_poll_create has run, and the handle it gave back. */
static int64_t pollset_handle = PAL_INVALID_HANDLE;

#if defined(BURROW_OS_WASI)

/* One thread, so nothing to lock, and nobody to wake: whoever would call
 * pal_poll_break or pal_poll_arm is the thread that would be asleep. There is
 * no descriptor for the poller either, and this stands in for one. */
#define WASI_POLLER ((int64_t)0x7fffffff)

static void table_lock(void) {}

static void table_unlock(void) {}

#else

/* Cosmopolitan's PTHREAD_MUTEX_INITIALIZER leaves fields out, which GCC warns
 * about. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static pthread_mutex_t table_mu = PTHREAD_MUTEX_INITIALIZER;
#pragma GCC diagnostic pop

static void table_lock(void) {
    (void)pthread_mutex_lock(&table_mu);
}

static void table_unlock(void) {
    (void)pthread_mutex_unlock(&table_mu);
}

/* The wakeup, a pipe with a byte down it as in poll_bsd.c, and for the same
 * reasons. Both ends set once by pal_poll_create. */
static int breakrd = -1;
static int breakwr = -1;

/* Whether a wakeup is already on its way. Cleared by whoever drains the pipe,
 * which is a thread that was going to sleep. */
static uint32_t wakesig;

/* How many waits are asleep in poll, under table_mu. pal_poll_arm wakes them,
 * since a set they copied before it was armed won't have it in. */
static int sleepers;

static bool set_pipe_flags(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | (int)O_NONBLOCK) != 0)
        return false;

    flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0)
        return false;

    return true;
}

static bool wake(PalErrno *err) {
    uint32_t none = 0;
    if (!burrow__atomic_cas_u32(&wakesig, &none, 1))
        return true;

    char b = 0;
    for (;;) {
        ssize_t n = write(breakwr, &b, 1);
        if (n == 1)
            return true;
        if (errno == EINTR)
            continue;

        /* The pipe is full, so the next wait comes straight back out anyway. */
        if (errno == EAGAIN)
            return true;

        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }
}

static void drain(void) {
    char buf[64];
    while (read(breakrd, buf, sizeof buf) > 0) {
    }
    burrow__atomic_store_u32(&wakesig, 0);
}

#endif

int64_t pal_poll_create(PalErrno *err) {
    if (pollset_handle != PAL_INVALID_HANDLE) {
        BURROW_OUT(err, PAL_EBUSY);
        return PAL_INVALID_HANDLE;
    }

#if defined(BURROW_OS_WASI)
    pollset_handle = WASI_POLLER;
#else
    int fds[2];
    if (pipe(fds) != 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return PAL_INVALID_HANDLE;
    }
    if (!set_pipe_flags(fds[0]) || !set_pipe_flags(fds[1])) {
        PalErrno e = burrow__pal_errno(errno);
        (void)close(fds[0]);
        (void)close(fds[1]);
        BURROW_OUT(err, e);
        return PAL_INVALID_HANDLE;
    }
    breakrd = fds[0];
    breakwr = fds[1];
    pollset_handle = (int64_t)fds[0];
#endif

    BURROW_OUT(err, PAL_OK);
    return pollset_handle;
}

static bool is_poller(int64_t handle) {
    return pollset_handle != PAL_INVALID_HANDLE && handle == pollset_handle;
}

bool pal_poll_add(int64_t handle, int64_t fd, void *user, PalErrno *err) {
    if (!is_poller(handle) || (uintptr_t)user == BREAK_TAG || fd < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }
    if (fd >= TABLE) {
        BURROW_OUT(err, PAL_EMFILE);
        return false;
    }

    table_lock();
    PollEntry *e = &table[fd];
    if (e->used) {
        table_unlock();
        BURROW_OUT(err, PAL_EEXIST);
        return false;
    }
    e->user = user;
    e->used = 1;
    e->armed = 0;
    if (table_hi <= (int)fd)
        table_hi = (int)fd + 1;
    table_unlock();

    BURROW_OUT(err, PAL_OK);
    return true;
}

bool pal_poll_del(int64_t handle, int64_t fd, PalErrno *err) {
    if (!is_poller(handle) || fd < 0 || fd >= TABLE) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    table_lock();
    PollEntry *e = &table[fd];
    if (!e->used) {
        table_unlock();
        BURROW_OUT(err, PAL_ENOENT);
        return false;
    }
    e->user = NULL;
    e->used = 0;
    e->armed = 0;
    while (table_hi > 0 && !table[table_hi - 1].used)
        table_hi--;
    table_unlock();

    BURROW_OUT(err, PAL_OK);
    return true;
}

bool pal_poll_arm(int64_t handle, int64_t fd, uint32_t mode, PalErrno *err) {
    if (!is_poller(handle) || fd < 0 || fd >= TABLE ||
        (mode & ~(uint32_t)(PAL_POLL_READ | PAL_POLL_WRITE)) != 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    table_lock();
    PollEntry *e = &table[fd];
    if (!e->used) {
        table_unlock();
        BURROW_OUT(err, PAL_ENOENT);
        return false;
    }
    bool news = (e->armed & mode) != mode;
    e->armed |= mode;
#if defined(BURROW_OS_WASI)
    (void)news;
    table_unlock();
#else
    bool asleep = news && sleepers > 0;
    table_unlock();
    if (asleep && !wake(err))
        return false;
#endif

    BURROW_OUT(err, PAL_OK);
    return true;
}

bool pal_poll_break(int64_t handle, PalErrno *err) {
    if (!is_poller(handle)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    BURROW_OUT(err, PAL_OK);
#if defined(BURROW_OS_WASI)
    return true;
#else
    return wake(err);
#endif
}

/* poll takes milliseconds, rounded up for the reason poll_linux.c gives. */
static int wait_ms(int64_t timeout_ns) {
    if (timeout_ns < 0)
        return -1;
    if (timeout_ns == 0)
        return 0;
    if (timeout_ns < 1000000)
        return 1;

    int64_t ms = timeout_ns / 1000000;
    if (ms > 1000000000)
        ms = 1000000000;
    return (int)ms;
}

int64_t pal_poll_wait(int64_t handle, PalPollEvent *out, int64_t cap,
                      int64_t timeout_ns, PalErrno *err) {
    if (!is_poller(handle) || out == NULL || cap <= 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }

    /* The wakeup's descriptor first, and every armed one after it. */
    struct pollfd fds[TABLE + 1];
    nfds_t n = 0;
#if !defined(BURROW_OS_WASI)
    fds[n].fd = breakrd;
    fds[n].events = POLLIN;
    fds[n].revents = 0;
    n++;
#endif

    table_lock();
    for (int fd = 0; fd < table_hi; fd++) {
        uint32_t armed = table[fd].used ? table[fd].armed : 0;
        if (armed == 0)
            continue;
        short events = 0;
        if ((armed & PAL_POLL_READ) != 0)
            events |= POLLIN;
        if ((armed & PAL_POLL_WRITE) != 0)
            events |= POLLOUT;
        fds[n].fd = fd;
        fds[n].events = events;
        fds[n].revents = 0;
        n++;
    }
#if !defined(BURROW_OS_WASI)
    bool sleeping = timeout_ns != 0;
    if (sleeping)
        sleepers++;
#endif
    table_unlock();

#if defined(BURROW_OS_WASI)
    /* With nothing armed there is nothing to wait for but the time, and poll
     * with no descriptors is a sleep. With no time either, a wait that would
     * never end is one that can't be woken, so like a futex wait here it fails
     * rather than hangs. */
    if (n == 0 && timeout_ns < 0) {
        BURROW_OUT(err, PAL_EDEADLK);
        return -1;
    }
#endif

    int ms = wait_ms(timeout_ns);
    int got_n = poll(fds, n, ms);
    int perr = errno;

    table_lock();
#if !defined(BURROW_OS_WASI)
    if (sleeping)
        sleepers--;
#endif
    if (got_n < 0) {
        table_unlock();
        /* A signal cut it short, which is no events rather than a failure. */
        if (perr == EINTR) {
            BURROW_OUT(err, PAL_OK);
            return 0;
        }
        BURROW_OUT(err, burrow__pal_errno(perr));
        return -1;
    }

    int64_t got = 0;
    for (nfds_t i = 0; i < n && got < cap; i++) {
        short rev = fds[i].revents;
        if (rev == 0)
            continue;
#if !defined(BURROW_OS_WASI)
        if (fds[i].fd == breakrd) {
            /* Only drained by a wait that was going to block, as in
             * poll_linux.c. */
            if (sleeping)
                drain();
            continue;
        }
#endif

        /* The descriptor may have been removed while this was asleep, or
         * removed and added again, in which case what was armed is what
         * counts now. */
        PollEntry *e = &table[fds[i].fd];
        if (!e->used)
            continue;

        uint32_t ready = 0;
        if ((rev & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) != 0 &&
            (e->armed & PAL_POLL_READ) != 0)
            ready |= PAL_POLL_READY_READ;
        if ((rev & (POLLOUT | POLLHUP | POLLERR | POLLNVAL)) != 0 &&
            (e->armed & PAL_POLL_WRITE) != 0)
            ready |= PAL_POLL_READY_WRITE;
        if (ready == 0)
            continue;
        if ((rev & (POLLIN | POLLOUT | POLLHUP)) == 0)
            ready |= PAL_POLL_READY_ERROR;

        /* Reported once, which is what makes this edge triggered. */
        if ((ready & PAL_POLL_READY_READ) != 0)
            e->armed &= ~(uint32_t)PAL_POLL_READ;
        if ((ready & PAL_POLL_READY_WRITE) != 0)
            e->armed &= ~(uint32_t)PAL_POLL_WRITE;

        out[got].user = e->user;
        out[got].ready = ready;
        out[got].bytes = 0;
        out[got].status = 0;
        got++;
    }
    table_unlock();

    BURROW_OUT(err, PAL_OK);
    return got;
}

#endif /* Cosmopolitan and wasip1 */
