/* The netpoller on Windows, which is an I/O completion port.
 *
 * burrow/netpoll.h says what a backend is for and src/runtime/netpoll.c is
 * everything this does not have to think about. What is left here is one
 * completion port, a way to interrupt a wait, and the translation between what
 * the port reports and what the layer above understands.
 *
 * This is the odd one of the three and the difference is worth stating plainly.
 * epoll and kqueue watch descriptors and say when one is worth trying. A
 * completion port watches operations: somebody submits a read with an
 * OVERLAPPED attached, the kernel does the read, and the port hands the
 * OVERLAPPED back when it is finished. So there is nothing to arm here and
 * nothing to re-arm, no edge and no level for a connection, and the only thing
 * open does is attach the descriptor to the port so that its completions have
 * somewhere to arrive.
 *
 * That also means the handle the layer above wants back does not come from the
 * kernel. It comes from the caller, in the burrow__PollOp it submitted, whose
 * first member is laid out to be an OVERLAPPED so that the pointer the kernel
 * returns is the pointer to the operation. Go does the same thing with
 * internal/poll.operation and net_op. burrow carries its generation checked
 * handle in there rather than a pointer to the descriptor, which is the one
 * improvement on Go's arrangement: a completion for an operation on a
 * connection that was closed while it was in flight names a slot that has since
 * been reused, and the layer above drops it instead of waking whoever holds the
 * slot now.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/netpoll.h"

#if defined(BURROW_NETPOLL_IOCP)

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <windows.h>

/* How many completions one call takes off the kernel at a time. Go's number for
 * this backend, and it is smaller than the epoll one because each entry is four
 * words rather than two and because a completion is a whole operation finishing
 * rather than a hint. */
#define EVENTS 64

/* The port. Made once, in the init below, which runs under a lock and runs
 * before anything else here can be called. Read without any of that after. */
static HANDLE iocp = NULL;

/* Whether a wakeup is already on its way, so that a thousand goroutines readied
 * at once cost one posted completion rather than a thousand. Cleared by
 * whoever takes the posted completion off the port. */
static uint32_t wakesig;

/* The operation structure in the header has to be an OVERLAPPED where the
 * kernel looks, and there is no way to check that by reading it. These are the
 * check. Size and alignment catch a field of the wrong width, and the offsets
 * catch two fields in the wrong order, which is the mistake that would
 * otherwise show up as a kernel writing a byte count into the wrong place. */
_Static_assert(sizeof(burrow__PollOverlapped) == sizeof(OVERLAPPED),
               "burrow__PollOverlapped is not the size of an OVERLAPPED");
_Static_assert(_Alignof(burrow__PollOverlapped) == _Alignof(OVERLAPPED),
               "burrow__PollOverlapped is not aligned like an OVERLAPPED");
_Static_assert(offsetof(burrow__PollOverlapped, internal) ==
                   offsetof(OVERLAPPED, Internal),
               "burrow__PollOverlapped.internal is in the wrong place");
_Static_assert(offsetof(burrow__PollOverlapped, internal_high) ==
                   offsetof(OVERLAPPED, InternalHigh),
               "burrow__PollOverlapped.internal_high is in the wrong place");
_Static_assert(offsetof(burrow__PollOverlapped, event) == offsetof(OVERLAPPED, hEvent),
               "burrow__PollOverlapped.event is in the wrong place");

/* The operation is handed to the kernel as an OVERLAPPED and comes back as one,
 * and it is the first member, so this is the cast in both directions. */
_Static_assert(offsetof(burrow__PollOp, o) == 0,
               "the overlapped has to be first in a burrow__PollOp");

void burrow__netpoll_backend_init(void) {
    iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (iocp == NULL)
        runtime_throw(BURROW_S("netpoll: CreateIoCompletionPort failed"));
}

int burrow__netpoll_backend_open(burrow__PollFd fd, uintptr_t handle) {
    /* Nothing is registered for a direction and nothing asks for the handle
     * back, so the handle is not wanted here. It reaches this file in the
     * operation the caller submits. */
    (void)handle;

    if (CreateIoCompletionPort((HANDLE)fd, iocp, 0, 0) == NULL)
        return (int)GetLastError();

    /* An overlapped operation on a handle signals the handle's own event when it
     * finishes, on top of posting to the port. Nothing here waits on that event,
     * so this turns it off, which saves a kernel object touch on every read and
     * write. It is allowed to fail: a socket from a layered provider that is not
     * an installable file system does not support it, and the only cost of not
     * having it is the work it would have saved. Go ignores the same failure. */
    (void)SetFileCompletionNotificationModes((HANDLE)fd, FILE_SKIP_SET_EVENT_ON_HANDLE);
    return 0;
}

int burrow__netpoll_backend_close(burrow__PollFd fd) {
    /* Nothing to undo, and nothing that could undo it. There is no call that
     * takes a handle off a completion port; closing the handle is how it leaves.
     * The caller closes it right after this returns. */
    (void)fd;
    return 0;
}

void burrow__netpoll_backend_break(void) {
    uint32_t none = 0;
    if (!burrow__atomic_cas_u32(&wakesig, &none, 1))
        return;

    /* A completion with no OVERLAPPED behind it, which is a shape a real
     * operation can never have, so the wait below can tell them apart without a
     * value of its own. */
    if (!PostQueuedCompletionStatus(iocp, 0, 0, NULL))
        runtime_throw(BURROW_S("netpoll: PostQueuedCompletionStatus failed"));
}

void burrow__netpoll_backend_wait(int64_t delay, burrow__GQueue *out) {
    DWORD timeout = INFINITE;
    if (delay == 0) {
        timeout = 0;
    } else if (delay > 0) {
        /* Milliseconds, rounded up, because a wait that is short by less than a
         * millisecond comes straight back and goes round, and rounding down is
         * how a timer deadline turns into a spin. INFINITE is a value rather
         * than a very long wait, so a delay that lands on it has to be moved. */
        int64_t ms = (delay + 999999) / 1000000;
        if (ms >= (int64_t)INFINITE)
            ms = (int64_t)INFINITE - 1;
        timeout = (DWORD)ms;
    }

    OVERLAPPED_ENTRY entries[EVENTS];
    ULONG n = 0;
    if (!GetQueuedCompletionStatusEx(iocp, entries, EVENTS, &n, timeout, FALSE)) {
        DWORD err = GetLastError();
        if (err == WAIT_TIMEOUT)
            return;
        runtime_throw(BURROW_S("netpoll: GetQueuedCompletionStatusEx failed"));
    }

    for (ULONG i = 0; i < n; i++) {
        OVERLAPPED_ENTRY *e = &entries[i];

        if (e->lpOverlapped == NULL) {
            /* The wakeup. A wait that was going to block takes it, which is
             * what it was posted for. A wait that was only looking puts it back,
             * because the thread it was meant for is still asleep in here and a
             * lost wakeup is a program that stops: burrow runs one thread per P
             * and there is no spare thread to notice.
             *
             * Putting it back rather than leaving it there is the difference
             * between this file and the other two, where the wakeup is a
             * descriptor that can be left readable. A completion is taken off
             * the port by whoever receives it and there is no leaving it. */
            burrow__atomic_store_u32(&wakesig, 0);
            if (delay == 0)
                burrow__netpoll_backend_break();
            continue;
        }

        burrow__PollOp *op = (burrow__PollOp *)e->lpOverlapped;

        /* Not a check on the kernel, which does not get this wrong. It is a
         * check on the caller: an operation that was freed before its completion
         * arrived lands here as whatever the memory says now, and this is the
         * closest thing to a message about it. See the one rule in the header. */
        if (op->mode != BURROW_POLL_READ && op->mode != BURROW_POLL_WRITE)
            runtime_throw(BURROW_S("netpoll: a completion for neither reading nor "
                                   "writing, which is an operation freed too soon"));

        /* What the operation produced, straight off the completion, which is
         * why this backend makes no system call per event. The status word is
         * the kernel's own and burrow__PollOp says why it is not translated. */
        op->qty = (uint32_t)e->dwNumberOfBytesTransferred;
        op->status = (int32_t)(uint32_t)e->Internal;

        burrow__netpoll_ready(out, op->desc, op->mode, op->status != 0);
    }
}

#endif /* BURROW_NETPOLL_IOCP */
