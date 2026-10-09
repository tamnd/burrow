/* internal/poll.FD for sockets, on a readiness netpoller. See internal.h.
 *
 * Derived from Go's src/internal/poll/fd_unix.go, fd_posix.go,
 * fd_poll_runtime.go and fd_mutex.go.
 * Go source: go1.27.1.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/clock.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/sema.h"
#include "burrow/syscall.h"

#include "../os/internal.h"

#include <stdint.h>

BURROW_SENTINEL_ERROR(burrow__net_err_not_pollable, "not pollable");

/* Go's maxRW: one read or write on a stream asks for at most a gigabyte, since
 * some systems refuse more than that in one call. */
#define PFD_MAX_RW ((Int)1 << 30)

/* ------------------------------------------------------- the poller's answers */

/* convertErr, for a socket, which is never a file. */
static Error pfd_status(burrow__PollStatus st) {
    switch (st) {
    case BURROW_POLL_READY:
        return BURROW_NO_ERROR;
    case BURROW_POLL_CLOSED:
        return net_err_closed;
    case BURROW_POLL_TIMEOUT:
        return os_err_deadline_exceeded;
    case BURROW_POLL_UNPOLLABLE:
    default:
        return burrow__net_err_not_pollable;
    }
}

static Error pfd_prepare(burrow__PollFD *fd, uint32_t mode) {
    return pfd_status(burrow__poll_reset(fd->pd, mode));
}

static Error pfd_wait(burrow__PollFD *fd, uint32_t mode) {
    return pfd_status(burrow__poll_wait(fd->pd, mode));
}

/* ---------------------------------------------------------------- the locks */

/* poll.FD.destroy: the last user is gone after a close, so the descriptor
 * goes back to the system and Close can stop waiting. */
static Error pfd_destroy(burrow__PollFD *fd) {
    burrow__poll_close(fd->pd);
    fd->pd = NULL;
    PalErrno pe = PAL_OK;
    Error e = BURROW_NO_ERROR;
    if (!pal_socket_close(fd->sysfd, &pe))
        e = burrow__os_errno(pe);
    fd->sysfd = -1;
    burrow__sema_release(&fd->csema, false);
    return e;
}

Error burrow__pfd_incref(burrow__PollFD *fd) {
    if (!burrow__fdmu_incref(&fd->mu))
        return net_err_closed;
    return BURROW_NO_ERROR;
}

Error burrow__pfd_decref(burrow__PollFD *fd) {
    if (burrow__fdmu_decref(&fd->mu))
        return pfd_destroy(fd);
    return BURROW_NO_ERROR;
}

static bool pfd_lock(burrow__PollFD *fd, bool read) {
    return burrow__fdmu_rwlock(&fd->mu, read, true);
}

static void pfd_unlock(burrow__PollFD *fd, bool read) {
    if (burrow__fdmu_rwunlock(&fd->mu, read))
        (void)pfd_destroy(fd);
}

/* ------------------------------------------------------------ opening, closing */

Error burrow__pfd_init(burrow__PollFD *fd, int64_t sysfd, bool stream,
                       bool zero_read_is_eof) {
    *fd = (burrow__PollFD){0};
    fd->sysfd = sysfd;
    fd->stream = stream;
    fd->zero_read_is_eof = zero_read_is_eof;
#if defined(BURROW_NETPOLL_READINESS)
    if (sysfd < 0 || sysfd > INT32_MAX)
        return burrow__os_errno(PAL_EBADF);
    int e = burrow__poll_open((burrow__PollFd)sysfd, &fd->pd);
    if (e != 0)
        return burrow__os_errno_value((SyscallErrno)e);
    return BURROW_NO_ERROR;
#else
    return burrow__os_errno(PAL_ENOSYS);
#endif
}

Error burrow__pfd_close(burrow__PollFD *fd) {
    if (!burrow__fdmu_incref_and_close(&fd->mu))
        return net_err_closed;

    /* Wakes whoever is parked in a read or a write, so that they see the close
     * and give their references back, which is what lets the last decref
     * below or theirs close the descriptor. */
    burrow__poll_unblock(fd->pd);
    Error e = burrow__pfd_decref(fd);

    /* The descriptor is closed by whichever of the users went last, and Close
     * does not return until that has happened, so that a Close followed by a
     * Listen on the same port finds the port free. */
    burrow__sema_acquire(&fd->csema, false, false);
    return e;
}

/* ------------------------------------------------------------- reading */

/* eofError: nothing read and nothing wrong is the end, on a stream. */
static Error pfd_eof(const burrow__PollFD *fd, Int n, Error e) {
    if (n == 0 && !BURROW_FAILED(e) && fd->zero_read_is_eof)
        return io_eof;
    return e;
}

static Int pfd_recv(burrow__PollFD *fd, Slice p, PalSockAddr *from, Error *err) {
    if (!pfd_lock(fd, true)) {
        BURROW_OUT(err, net_err_closed);
        return 0;
    }
    Int n = 0;
    Error e = pfd_prepare(fd, BURROW_POLL_READ);
    Int len = p.len;
    if (fd->stream && len > PFD_MAX_RW)
        len = PFD_MAX_RW;
    while (!BURROW_FAILED(e)) {
        PalErrno pe = PAL_OK;
        int64_t r = pal_recvfrom(fd->sysfd, p.p, len, from, &pe);
        if (r >= 0) {
            n = (Int)r;
            e = pfd_eof(fd, n, BURROW_NO_ERROR);
            break;
        }
        if (pe == PAL_EINTR)
            continue;
        if (pe == PAL_EAGAIN) {
            e = pfd_wait(fd, BURROW_POLL_READ);
            continue;
        }
        e = burrow__os_errno(pe);
    }
    pfd_unlock(fd, true);
    BURROW_OUT(err, e);
    return n;
}

Int burrow__pfd_read(burrow__PollFD *fd, Slice p, Error *err) {
    /* Go answers an empty read at once, with the lock taken and nothing else,
     * so that a Read of nothing on a closed connection is still an error. */
    if (p.len == 0) {
        if (!pfd_lock(fd, true)) {
            BURROW_OUT(err, net_err_closed);
            return 0;
        }
        pfd_unlock(fd, true);
        BURROW_OUT(err, BURROW_NO_ERROR);
        return 0;
    }
    return pfd_recv(fd, p, NULL, err);
}

Int burrow__pfd_read_from(burrow__PollFD *fd, Slice p, PalSockAddr *from, Error *err) {
    return pfd_recv(fd, p, from, err);
}

/* poll.FD.ReadMsg. Unlike a read, an empty p still goes to the system,
 * since the control messages may be all there is. */
Int burrow__pfd_read_msg(burrow__PollFD *fd, Slice p, Slice oob, Int *oobn, Int *flags,
                         PalSockAddr *from, Error *err) {
    *oobn = 0;
    *flags = 0;
    if (!pfd_lock(fd, true)) {
        BURROW_OUT(err, net_err_closed);
        return 0;
    }
    Int n = 0;
    Error e = pfd_prepare(fd, BURROW_POLL_READ);
    while (!BURROW_FAILED(e)) {
        PalErrno pe = PAL_OK;
        int64_t on = 0;
        int32_t fl = 0;
        int64_t r = pal_recvmsg(fd->sysfd, p.p, p.len, oob.p, oob.len, &on, &fl, from, &pe);
        if (r >= 0) {
            n = (Int)r;
            *oobn = (Int)on;
            *flags = (Int)fl;
            e = pfd_eof(fd, n, BURROW_NO_ERROR);
            break;
        }
        if (pe == PAL_EINTR)
            continue;
        if (pe == PAL_EAGAIN) {
            e = pfd_wait(fd, BURROW_POLL_READ);
            continue;
        }
        e = burrow__os_errno(pe);
    }
    pfd_unlock(fd, true);
    BURROW_OUT(err, e);
    return n;
}

/* ------------------------------------------------------------- writing */

Int burrow__pfd_write(burrow__PollFD *fd, Slice p, Error *err) {
    if (!pfd_lock(fd, false)) {
        BURROW_OUT(err, net_err_closed);
        return 0;
    }
    Int nn = 0;
    Error e = pfd_prepare(fd, BURROW_POLL_WRITE);
    while (!BURROW_FAILED(e)) {
        Int max = p.len;
        if (fd->stream && max - nn > PFD_MAX_RW)
            max = nn + PFD_MAX_RW;
        PalErrno pe = PAL_OK;
        int64_t r = -1;
        do {
            r = pal_sendto(fd->sysfd, (const Byte *)p.p + nn, max - nn, NULL, &pe);
        } while (r < 0 && pe == PAL_EINTR);
        if (r > 0)
            nn += (Int)r;
        if (nn == p.len) {
            if (r < 0)
                e = burrow__os_errno(pe);
            break;
        }
        if (r < 0 && pe == PAL_EAGAIN) {
            e = pfd_wait(fd, BURROW_POLL_WRITE);
            continue;
        }
        if (r < 0) {
            e = burrow__os_errno(pe);
            break;
        }
        if (r == 0) {
            e = io_err_unexpected_eof;
            break;
        }
    }
    pfd_unlock(fd, false);
    BURROW_OUT(err, e);
    return nn;
}

Int burrow__pfd_write_to(burrow__PollFD *fd, Slice p, const PalSockAddr *to,
                         Error *err) {
    if (!pfd_lock(fd, false)) {
        BURROW_OUT(err, net_err_closed);
        return 0;
    }
    Int n = 0;
    Error e = pfd_prepare(fd, BURROW_POLL_WRITE);
    while (!BURROW_FAILED(e)) {
        PalErrno pe = PAL_OK;
        int64_t r = pal_sendto(fd->sysfd, p.p, p.len, to, &pe);
        if (r >= 0) {
            /* A datagram goes whole or not at all, so Go reports all of p. */
            n = p.len;
            break;
        }
        if (pe == PAL_EINTR)
            continue;
        if (pe == PAL_EAGAIN) {
            e = pfd_wait(fd, BURROW_POLL_WRITE);
            continue;
        }
        e = burrow__os_errno(pe);
    }
    pfd_unlock(fd, false);
    BURROW_OUT(err, e);
    return n;
}

/* poll.FD.WriteMsg, which answers all of oob as sent when the message
 * went. */
Int burrow__pfd_write_msg(burrow__PollFD *fd, Slice p, Slice oob, const PalSockAddr *to,
                          Int *oobn, Error *err) {
    *oobn = 0;
    if (!pfd_lock(fd, false)) {
        BURROW_OUT(err, net_err_closed);
        return 0;
    }
    Int n = 0;
    Error e = pfd_prepare(fd, BURROW_POLL_WRITE);
    while (!BURROW_FAILED(e)) {
        PalErrno pe = PAL_OK;
        int64_t r = pal_sendmsg(fd->sysfd, p.p, p.len, oob.p, oob.len, to, &pe);
        if (r >= 0) {
            n = (Int)r;
            *oobn = oob.len;
            break;
        }
        if (pe == PAL_EINTR)
            continue;
        if (pe == PAL_EAGAIN) {
            e = pfd_wait(fd, BURROW_POLL_WRITE);
            continue;
        }
        e = burrow__os_errno(pe);
    }
    pfd_unlock(fd, false);
    BURROW_OUT(err, e);
    return n;
}

/* ------------------------------------------------------------ the rest */

int64_t burrow__pfd_accept(burrow__PollFD *fd, PalSockAddr *peer, Error *err) {
    if (!pfd_lock(fd, true)) {
        BURROW_OUT(err, net_err_closed);
        return -1;
    }
    int64_t s = -1;
    Error e = pfd_prepare(fd, BURROW_POLL_READ);
    while (!BURROW_FAILED(e)) {
        PalErrno pe = PAL_OK;
        s = pal_accept(fd->sysfd, peer, &pe);
        if (s >= 0)
            break;
        if (pe == PAL_EINTR || pe == PAL_ECONNABORTED)
            continue;
        if (pe == PAL_EAGAIN) {
            e = pfd_wait(fd, BURROW_POLL_READ);
            continue;
        }
        e = burrow__os_errno(pe);
    }
    pfd_unlock(fd, true);
    BURROW_OUT(err, e);
    return s;
}

Error burrow__pfd_shutdown(burrow__PollFD *fd, int32_t how) {
    Error e = burrow__pfd_incref(fd);
    if (BURROW_FAILED(e))
        return e;
    PalErrno pe = PAL_OK;
    if (!pal_shutdown(fd->sysfd, how, &pe))
        e = burrow__os_errno(pe);
    Error d = burrow__pfd_decref(fd);
    return BURROW_FAILED(e) ? e : d;
}

Error burrow__pfd_set_deadline(burrow__PollFD *fd, Time t, uint32_t mode) {
    /* setDeadlineImpl: a deadline of now is not the same as no deadline, and
     * the runtime then turns the duration into a point on its own clock. */
    int64_t when = 0;
    if (!time_is_zero(t)) {
        Duration d = time_until(t);
        if (d <= 0)
            when = -1;
        else if (d > INT64_MAX - burrow__nanotime())
            when = INT64_MAX;
        else
            when = burrow__nanotime() + d;
    }
    Error e = burrow__pfd_incref(fd);
    if (BURROW_FAILED(e))
        return e;
    bool armed = burrow__poll_set_deadline(fd->pd, when, mode);
    Error d = burrow__pfd_decref(fd);
    if (!armed)
        return burrow_err_out_of_memory;
    return d;
}

Error burrow__pfd_wait_write(burrow__PollFD *fd) {
    return pfd_wait(fd, BURROW_POLL_WRITE);
}

/* ------------------------------------------------------------ the raw calls */

BURROW_SENTINEL_ERROR(burrow__net_err_unsupported_wait,
                      "waiting for unsupported file type");

/* pollDesc.prepare and wait for a descriptor that may not be in the poller
 * yet, which Go lets through and then refuses to wait for. */
static Error pfd_raw_prepare(burrow__PollFD *fd, uint32_t mode) {
    if (fd->pd == NULL)
        return BURROW_NO_ERROR;
    return pfd_prepare(fd, mode);
}

static Error pfd_raw_wait(burrow__PollFD *fd, uint32_t mode) {
    if (fd->pd == NULL)
        return burrow__net_err_unsupported_wait;
    return pfd_wait(fd, mode);
}

Error burrow__pfd_raw_control(burrow__PollFD *fd, SyscallFdFunc f) {
    Error e = burrow__pfd_incref(fd);
    if (BURROW_FAILED(e))
        return e;
    f.f(f.env, (Uintptr)fd->sysfd);
    (void)burrow__pfd_decref(fd);
    return BURROW_NO_ERROR;
}

static Error pfd_raw_io(burrow__PollFD *fd, SyscallFdDoneFunc f, bool read) {
    uint32_t mode = read ? BURROW_POLL_READ : BURROW_POLL_WRITE;
    if (!pfd_lock(fd, read))
        return net_err_closed;
    Error e = pfd_raw_prepare(fd, mode);
    while (!BURROW_FAILED(e)) {
        if (f.f(f.env, (Uintptr)fd->sysfd))
            break;
        e = pfd_raw_wait(fd, mode);
    }
    pfd_unlock(fd, read);
    return e;
}

Error burrow__pfd_raw_read(burrow__PollFD *fd, SyscallFdDoneFunc f) {
    return pfd_raw_io(fd, f, true);
}

Error burrow__pfd_raw_write(burrow__PollFD *fd, SyscallFdDoneFunc f) {
    return pfd_raw_io(fd, f, false);
}
