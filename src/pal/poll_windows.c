/* Completion notification on Windows, which is an I/O completion port.
 *
 * This was src/runtime/netpoll_iocp.c until the platform layer existed. What
 * changed on the way in is that it no longer knows what a goroutine or an
 * operation is and no longer throws: a failure comes back as a PalErrno and the
 * caller decides whether the program can go on.
 *
 * This is the odd one of the three and the difference is worth stating plainly.
 * epoll and kqueue watch descriptors and say when one is worth trying. A
 * completion port watches operations: somebody submits a read with an
 * OVERLAPPED attached, the kernel does the read, and the port hands the
 * OVERLAPPED back when it is finished. So there is nothing to arm here and
 * nothing to re-arm, no edge and no level for a connection, and the only thing
 * pal_poll_add does is attach the descriptor to the port so that its
 * completions have somewhere to arrive.
 *
 * That also means the user pointer does not come from pal_poll_add. It comes
 * from the caller, in the OVERLAPPED it submitted the operation with, and it is
 * what comes back in PalPollEvent.user. Go does the same thing with
 * internal/poll.operation and net_op.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <stddef.h>

#include <windows.h>

/* PalOverlapped has to be an OVERLAPPED where the kernel looks, and there is no
 * way to check that by reading it. These are the check. Size and alignment
 * catch a field of the wrong width, and the offsets catch two fields in the
 * wrong order, which is the mistake that would otherwise show up as a kernel
 * writing a byte count into the wrong place. */
_Static_assert(sizeof(PalOverlapped) == sizeof(OVERLAPPED),
               "PalOverlapped is not the size of an OVERLAPPED");
_Static_assert(_Alignof(PalOverlapped) == _Alignof(OVERLAPPED),
               "PalOverlapped is not aligned like an OVERLAPPED");
_Static_assert(offsetof(PalOverlapped, internal) == offsetof(OVERLAPPED, Internal),
               "PalOverlapped.internal is in the wrong place");
_Static_assert(offsetof(PalOverlapped, internal_high) ==
                   offsetof(OVERLAPPED, InternalHigh),
               "PalOverlapped.internal_high is in the wrong place");
_Static_assert(offsetof(PalOverlapped, event) == offsetof(OVERLAPPED, hEvent),
               "PalOverlapped.event is in the wrong place");

/* The port. Made once, by pal_poll_create, which is the only call allowed to
 * touch it and runs before anything else here can. Read without any
 * synchronisation afterwards, which is safe because nothing writes it again. */
static HANDLE iocp = NULL;

/* Whether a wakeup is already on its way, so that a thousand goroutines readied
 * at once cost one posted completion rather than a thousand. Cleared by whoever
 * takes the posted completion off the port. */
static uint32_t wakesig;

int64_t pal_poll_create(PalErrno *err) {
    if (iocp != NULL) {
        BURROW_OUT(err, PAL_EBUSY);
        return PAL_INVALID_HANDLE;
    }

    HANDLE h = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (h == NULL) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return PAL_INVALID_HANDLE;
    }

    iocp = h;
    BURROW_OUT(err, PAL_OK);
    return (int64_t)(intptr_t)h;
}

static bool is_poller(int64_t poll) {
    return iocp != NULL && poll == (int64_t)(intptr_t)iocp;
}

bool pal_poll_add(int64_t poll, int64_t fd, void *user, PalErrno *err) {
    /* Nothing is registered for a direction and nothing asks for a user pointer
     * back, so the pointer is not wanted here. It reaches this file in the
     * OVERLAPPED the caller submits. */
    (void)user;

    if (!is_poller(poll) || fd == PAL_INVALID_HANDLE) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    HANDLE h = (HANDLE)(intptr_t)fd;
    if (CreateIoCompletionPort(h, iocp, 0, 0) == NULL) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }

    /* An overlapped operation on a handle signals the handle's own event when
     * it finishes, on top of posting to the port. Nothing here waits on that
     * event, so this turns it off, which saves a kernel object touch on every
     * read and write. It is allowed to fail: a socket from a layered provider
     * that is not an installable file system does not support it, and the only
     * cost of not having it is the work it would have saved. Go ignores the
     * same failure. */
    (void)SetFileCompletionNotificationModes(h, FILE_SKIP_SET_EVENT_ON_HANDLE);

    BURROW_OUT(err, PAL_OK);
    return true;
}

bool pal_poll_del(int64_t poll, int64_t fd, PalErrno *err) {
    (void)fd;

    if (!is_poller(poll)) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    /* Nothing to undo, and nothing that could undo it. There is no call that
     * takes a handle off a completion port; closing the handle is how it
     * leaves. The caller closes it right after this returns. */
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

    /* A completion with no OVERLAPPED behind it, which is a shape a real
     * operation can never have, so the wait below can tell them apart without a
     * value of its own. */
    if (!PostQueuedCompletionStatus(iocp, 0, 0, NULL)) {
        BURROW_OUT(err, burrow__pal_errno_win(GetLastError()));
        return false;
    }

    return true;
}

int64_t pal_poll_wait(int64_t poll, PalPollEvent *out, int64_t cap, int64_t timeout_ns,
                      PalErrno *err) {
    if (!is_poller(poll) || out == NULL || cap <= 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }

    DWORD timeout = INFINITE;
    if (timeout_ns == 0) {
        timeout = 0;
    } else if (timeout_ns > 0) {
        /* Milliseconds, rounded up, because a wait that is short by less than a
         * millisecond comes straight back and goes round, and rounding down is
         * how a timer deadline turns into a spin. INFINITE is a value rather
         * than a very long wait, so a delay that lands on it has to be moved. */
        int64_t ms = (timeout_ns + 999999) / 1000000;
        if (ms >= (int64_t)INFINITE)
            ms = (int64_t)INFINITE - 1;
        timeout = (DWORD)ms;
    }

    /* 64 rather than the 128 in poll_linux.c, which is Go's number for this
     * backend. Each entry is four words rather than two, and a completion is a
     * whole operation finishing rather than a hint. */
    OVERLAPPED_ENTRY entries[64];
    ULONG want = cap < (int64_t)(sizeof entries / sizeof entries[0])
                     ? (ULONG)cap
                     : (ULONG)(sizeof entries / sizeof entries[0]);

    ULONG n = 0;
    if (!GetQueuedCompletionStatusEx(iocp, entries, want, &n, timeout, FALSE)) {
        DWORD e = GetLastError();
        if (e == WAIT_TIMEOUT) {
            BURROW_OUT(err, PAL_OK);
            return 0;
        }
        BURROW_OUT(err, burrow__pal_errno_win(e));
        return -1;
    }

    BURROW_OUT(err, PAL_OK);

    int64_t got = 0;
    for (ULONG i = 0; i < n; i++) {
        OVERLAPPED_ENTRY *e = &entries[i];

        if (e->lpOverlapped == NULL) {
            /* The wakeup. A wait that was going to block takes it, which is
             * what it was posted for. A wait that was only looking puts it
             * back, because the thread it was meant for is still asleep in here
             * and a lost wakeup is a program that stops: burrow runs one thread
             * per P and there is no spare thread to notice.
             *
             * Putting it back rather than leaving it there is the difference
             * between this file and the other two, where the wakeup is a
             * descriptor that can be left readable. A completion is taken off
             * the port by whoever receives it and there is no leaving it. */
            burrow__atomic_store_u32(&wakesig, 0);
            if (timeout_ns == 0)
                (void)pal_poll_break(poll, NULL);
            continue;
        }

        out[got].user = e->lpOverlapped;
        out[got].bytes = (uint32_t)e->dwNumberOfBytesTransferred;
        out[got].status = (int32_t)(uint32_t)e->Internal;

        /* A completion says an operation finished, not which direction it was,
         * because the direction is the caller's and travels in the operation.
         * The only thing this layer can say is whether it failed. */
        out[got].ready = out[got].status != 0 ? PAL_POLL_READY_ERROR : 0;
        got++;
    }

    return got;
}

#endif /* BURROW_OS_WINDOWS */
