/* The netpoller's backend on a platform that reports readiness, over the PAL.
 *
 * This file used to be two, src/runtime/netpoll_epoll.c and
 * src/runtime/netpoll_kqueue.c, and it is one now because everything that was
 * different between them was the kernel's spelling. That went into
 * src/pal/poll_linux.c and src/pal/poll_bsd.c, and what is left here is the
 * part that was always the same: turning an event into a readied goroutine.
 *
 * There is nothing platform specific below and nothing here includes a system
 * header, which is the point. burrow/netpoll.h says what a backend owes the
 * layer above and burrow/pal.h says what the layer below promises, and this is
 * the fifty lines between them.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/netpoll.h"

#if defined(BURROW_NETPOLL_READINESS)

#include "burrow/core.h"
#include "burrow/pal.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"

/* The poller. Set once, by the init below, which runs under a lock and runs
 * before anything else here can be called. */
static int64_t poller = PAL_INVALID_HANDLE;

/* How many events one call takes off the kernel at a time. The PAL caps this
 * at whatever fits on its own stack, so asking for more than it will give is
 * harmless and asking for fewer would mean going round again for no reason. */
#define EVENTS 128

void burrow__netpoll_backend_init(void) {
    PalErrno err = PAL_OK;

    poller = pal_poll_create(&err);
    if (poller == PAL_INVALID_HANDLE)
        runtime_throw(BURROW_S("netpoll: the poller could not be created"));
}

int burrow__netpoll_backend_open(burrow__PollFd fd, uintptr_t handle) {
    PalErrno err = PAL_OK;

    if (!pal_poll_add(poller, (int64_t)fd, (void *)handle, &err))
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
        PalPollEvent *ev = &events[i];

        uint32_t mode = 0;
        if ((ev->ready & PAL_POLL_READY_READ) != 0)
            mode |= BURROW_POLL_READ;
        if ((ev->ready & PAL_POLL_READY_WRITE) != 0)
            mode |= BURROW_POLL_WRITE;

        if (mode == 0)
            continue;

        /* The error bit means the only thing the kernel reported was a
         * failure, which is what the layer above wants to know in order to tell
         * a descriptor with data on it from one that has broken. */
        burrow__netpoll_ready(out, (uintptr_t)ev->user, mode,
                              (ev->ready & PAL_POLL_READY_ERROR) != 0);
    }
}

#endif /* BURROW_NETPOLL_READINESS */
