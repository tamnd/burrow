/* The clocks on macOS and iOS.
 *
 * mach_absolute_time for the monotonic one, which is what Go uses here. It
 * counts in units the hardware picked rather than in nanoseconds, and
 * mach_timebase_info gives the fraction to multiply by: 1/1 on Intel, where a
 * tick already is a nanosecond, and 125/3 on Apple silicon, where the counter
 * runs at 24 MHz.
 *
 * The fraction is decided at boot and never changes, so it is cached. The cache
 * is not a lock and not a race. The two halves are written with relaxed stores
 * and published with a release store to the denominator, which readers acquire,
 * so two threads arriving at once both ask the kernel, both compute the same
 * number, both write the same bytes, and no reader can see half of one. 32 bit
 * and not 64 because a 64 bit atomic on a 32 bit machine goes through burrow's
 * spin lock table, and taking a lock to read a clock would be a strange thing to
 * do to the hottest call in the timer code.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <mach/mach_time.h>
#include <sys/time.h>
#include <time.h>

static uint32_t timebase_numer;

/* Written last and with a release, so a reader that sees it nonzero also sees
 * the numerator above. Zero means nobody has asked the kernel yet. */
static uint32_t timebase_denom;

int64_t pal_clock_monotonic(void) {
    uint64_t numer;
    uint64_t denom = burrow__atomic_load_acquire_u32(&timebase_denom);

    if (denom != 0) {
        numer = burrow__atomic_load_relaxed_u32(&timebase_numer);
    } else {
        mach_timebase_info_data_t info;

        if (mach_timebase_info(&info) != KERN_SUCCESS || info.denom == 0)
            return 0;

        numer = info.numer;
        denom = info.denom;
        burrow__atomic_store_relaxed_u32(&timebase_numer, info.numer);
        burrow__atomic_store_release_u32(&timebase_denom, info.denom);
    }

    uint64_t ticks = mach_absolute_time();

    /* Split so that the multiply cannot overflow. 24 MHz multiplied by 125
     * overflows 64 bits after a century of uptime, which is not a real worry,
     * and the split costs one divide and removes the question. */
    uint64_t whole = (ticks / denom) * numer;
    uint64_t rest = ((ticks % denom) * numer) / denom;
    return (int64_t)(whole + rest);
}

int64_t pal_clock_realtime(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;

    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

void pal_nanosleep(int64_t ns) {
    if (ns <= 0) {
        struct timespec zero = {0, 0};
        (void)nanosleep(&zero, NULL);
        return;
    }

    struct timespec want;
    want.tv_sec = (time_t)(ns / 1000000000);
    want.tv_nsec = (long)(ns % 1000000000);

    /* A signal cuts a sleep short and hands back what was left, so the loop
     * finishes the wait. See the note in time_posix.c for why that is the right
     * answer at this layer. */
    struct timespec left;
    while (nanosleep(&want, &left) != 0 && errno == EINTR)
        want = left;
}

#endif /* BURROW_OS_DARWIN || BURROW_OS_IOS */
