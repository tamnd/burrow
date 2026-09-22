/* The netpoller's backend on a platform that reports completions, over the PAL.
 *
 * This file used to be src/runtime/netpoll_iocp.c. Everything that was a call
 * to Windows went into src/pal/poll_windows.c, and what is left here is the
 * part that was never about Windows: taking what an operation produced out of
 * the event and readying the goroutine that submitted it.
 *
 * It is a separate file from netpoll_readiness.c rather than an #if inside it,
 * because the difference between the two is the one difference the PAL
 * deliberately does not hide. A readiness backend says a descriptor is worth
 * trying and a completion backend says an operation has finished, and the code
 * that sits on top of each of them is a different shape for a real reason.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/netpoll.h"

#if defined(BURROW_NETPOLL_COMPLETION)

#include "burrow/core.h"
#include "burrow/pal.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"

#include <stddef.h>

/* The operation is handed to the kernel as an OVERLAPPED and comes back as one,
 * and it is the first member, which is what makes the PAL's user pointer and
 * the operation's address the same address. */
_Static_assert(offsetof(burrow__PollOp, o) == 0,
               "the overlapped has to be first in a burrow__PollOp");

/* And it has to be the same structure the PAL declared, which is the half of
 * the chain this side can check. src/pal/poll_windows.c checks the other half,
 * that PalOverlapped is what Windows means by OVERLAPPED, and neither file can
 * check both because one of them must not include windows.h. */
_Static_assert(sizeof(burrow__PollOverlapped) == sizeof(PalOverlapped),
               "burrow__PollOverlapped and PalOverlapped are different sizes");
_Static_assert(_Alignof(burrow__PollOverlapped) == _Alignof(PalOverlapped),
               "burrow__PollOverlapped and PalOverlapped are aligned differently");
_Static_assert(offsetof(burrow__PollOverlapped, internal) ==
                   offsetof(PalOverlapped, internal),
               "burrow__PollOverlapped.internal is in the wrong place");
_Static_assert(offsetof(burrow__PollOverlapped, internal_high) ==
                   offsetof(PalOverlapped, internal_high),
               "burrow__PollOverlapped.internal_high is in the wrong place");
_Static_assert(offsetof(burrow__PollOverlapped, event) ==
                   offsetof(PalOverlapped, event),
               "burrow__PollOverlapped.event is in the wrong place");

/* The port. Set once, by the init below, which runs under a lock and runs
 * before anything else here can be called. */
static int64_t poller = PAL_INVALID_HANDLE;

/* How many completions one call takes off the kernel at a time. The PAL caps
 * this at whatever fits on its own stack. */
#define EVENTS 64

void burrow__netpoll_backend_init(void) {
    PalErrno err = PAL_OK;

    poller = pal_poll_create(&err);
    if (poller == PAL_INVALID_HANDLE)
        runtime_throw(BURROW_S("netpoll: the completion port could not be created"));
}

int burrow__netpoll_backend_open(burrow__PollFd fd, uintptr_t handle) {
    PalErrno err = PAL_OK;

    /* Nothing is registered for a direction and nothing asks for the handle
     * back, so the handle is not wanted here. It reaches this file in the
     * operation the caller submits. */
    (void)handle;

    if (!pal_poll_add(poller, (int64_t)fd, NULL, &err))
        return (int)err;
    return 0;
}

int burrow__netpoll_backend_close(burrow__PollFd fd) {
    PalErrno err = PAL_OK;

    if (!pal_poll_del(poller, (int64_t)fd, &err))
        return (int)err;
    return 0;
}

void burrow__netpoll_backend_break(void) {
    PalErrno err = PAL_OK;

    if (!pal_poll_break(poller, &err))
        runtime_throw(BURROW_S("netpoll: the wakeup could not be delivered"));
}

void burrow__netpoll_backend_wait(int64_t delay, burrow__GQueue *out) {
    PalPollEvent events[EVENTS];
    PalErrno err = PAL_OK;

    int64_t n = pal_poll_wait(poller, events, EVENTS, delay, &err);
    if (n < 0)
        runtime_throw(BURROW_S("netpoll: the wait failed"));

    for (int64_t i = 0; i < n; i++) {
        burrow__PollOp *op = (burrow__PollOp *)events[i].user;

        /* Not a check on the kernel, which does not get this wrong. It is a
         * check on the caller: an operation that was freed before its
         * completion arrived lands here as whatever the memory says now, and
         * this is the closest thing to a message about it. See the one rule in
         * the header. */
        if (op->mode != BURROW_POLL_READ && op->mode != BURROW_POLL_WRITE)
            runtime_throw(BURROW_S("netpoll: a completion for neither reading nor "
                                   "writing, which is an operation freed too soon"));

        /* What the operation produced, straight off the completion, which is
         * why this backend makes no system call per event. The status word is
         * the kernel's own and burrow__PollOp says why it is not translated. */
        op->qty = events[i].bytes;
        op->status = events[i].status;

        burrow__netpoll_ready(out, op->desc, op->mode, op->status != 0);
    }
}

#endif /* BURROW_NETPOLL_COMPLETION */
