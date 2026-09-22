/* The clocks on anything with a POSIX clock, which is Linux, the BSDs and
 * illumos. macOS has one too and does not use this file, because it reads the
 * mach counter directly the way Go does.
 *
 * _POSIX_C_SOURCE goes before the first include, because clock_gettime and
 * CLOCK_MONOTONIC are POSIX rather than C, and a strict C11 build of glibc or
 * musl does not declare them without being asked. 199309L is the release that
 * added them and is the smallest thing that works, which is the version to ask
 * for: a larger number turns on more than this file needs.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* Before the first include of anything, because glibc reads the feature test
 * macros when its first header is included and ignores one that arrives later.
 * The test is on the compiler's own macros rather than on BURROW_OS_*, since
 * knowing those would mean including a header first, which is the thing this
 * has to come before. */
#if !defined(_WIN32) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 199309L
#endif

#include "burrow/platform.h"

#if !defined(BURROW_OS_WINDOWS) && !defined(BURROW_OS_DARWIN) && !defined(BURROW_OS_IOS)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <time.h>

/* CLOCK_MONOTONIC_RAW exists on Linux and is deliberately not used. It is the
 * hardware counter with no ntp discipline on it, which sounds more correct and
 * is not: a clock nobody may discipline drifts against every other machine, and
 * ntp's correction to a monotonic clock changes its rate rather than jumping it,
 * so it still cannot go backwards. Raw is also slower, because it is not in the
 * vdso on every kernel. Go uses CLOCK_MONOTONIC and so does this.
 *
 * On Linux this reads through the vdso rather than entering the kernel, so it
 * is a few nanoseconds and not a system call. */
int64_t pal_clock_monotonic(void) {
    struct timespec ts;

    /* Cannot fail for a clock the system defines, and there is nothing useful
     * to return if it somehow does. Zero is a reading like any other, since the
     * only thing a caller may do with these is subtract two of them. */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

int64_t pal_clock_realtime(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return 0;

    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

void pal_nanosleep(int64_t ns) {
    if (ns <= 0) {
        /* A zero sleep still goes to the kernel, because the point of asking
         * for one is to let something else run. nanosleep with a zero timespec
         * is what sched_yield is on a system that has no sched_yield in
         * POSIX.1, and it is defined everywhere this builds. */
        struct timespec zero = {0, 0};
        (void)nanosleep(&zero, NULL);
        return;
    }

    struct timespec want;
    want.tv_sec = (time_t)(ns / 1000000000);
    want.tv_nsec = (long)(ns % 1000000000);

    /* A signal cuts a sleep short and hands back what was left, so the loop
     * finishes the wait rather than returning early. That is what the caller
     * asked for: a PAL sleep of one second means one second, and a caller that
     * wants to be woken by a signal is a caller that should be parking on a
     * futex instead. */
    struct timespec left;
    while (nanosleep(&want, &left) != 0 && errno == EINTR)
        want = left;
}

#endif /* posix clocks */
