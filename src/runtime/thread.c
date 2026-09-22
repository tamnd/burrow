/* Starting an OS thread.
 *
 * This file used to be two implementations of six functions, one for pthreads
 * and one for Windows, with the system headers included right here. All of that
 * is in src/pal/thread_posix.c and src/pal/thread_windows.c now, and what is
 * left is the handle bookkeeping: whether a thread was started, and whether it
 * has already been joined or detached. That part was never per platform and it
 * reads better without the other part around it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/thread.h"

#include "burrow/pal.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool burrow__thread_start(burrow__Thread *t, burrow__ThreadFn fn, void *arg,
                          size_t stack_bytes) {
    if (t == NULL || fn == NULL)
        return false;

    t->handle = PAL_INVALID_HANDLE;
    t->started = false;

    /* The platform layer takes a signed count and a size_t is unsigned, so a
     * request past the top is clamped rather than handed over as a negative,
     * which the layer would refuse. Nothing asks for a stack that big, and a
     * request that does is going to be refused by the system in any case. */
    int64_t want = stack_bytes > (size_t)INT64_MAX ? INT64_MAX : (int64_t)stack_bytes;

    int64_t h = pal_thread_create(fn, arg, want, NULL);
    if (h == PAL_INVALID_HANDLE)
        return false;

    t->handle = h;
    t->started = true;
    return true;
}

bool burrow__thread_join(burrow__Thread *t) {
    if (t == NULL || !t->started)
        return false;
    if (!pal_thread_join(t->handle, NULL))
        return false;

    t->handle = PAL_INVALID_HANDLE;
    t->started = false;
    return true;
}

bool burrow__thread_detach(burrow__Thread *t) {
    if (t == NULL || !t->started)
        return false;
    if (!pal_thread_detach(t->handle, NULL))
        return false;

    t->handle = PAL_INVALID_HANDLE;
    t->started = false;
    return true;
}

uint64_t burrow__thread_self(void) {
    return (uint64_t)pal_thread_self();
}

void burrow__thread_yield(void) {
    pal_thread_yield();
}

bool burrow__thread_stack_bounds(void **lo, void **hi) {
    return pal_thread_stack_bounds(lo, hi);
}

int burrow__thread_ncpu(void) {
    int64_t n = pal_cpu_count();

    if (n < 1)
        return 1;
    if (n > INT_MAX)
        return INT_MAX;
    return (int)n;
}
