/* Three monotonic clocks, one per platform. See burrow/clock.h for what the
 * caller is promised.
 *
 * _POSIX_C_SOURCE goes before the first include, because clock_gettime and
 * CLOCK_MONOTONIC are POSIX rather than C, and a strict C11 build of glibc or
 * musl does not declare them without being asked. 199309L is the release that
 * added them and is the smallest thing that works, which is the version to ask
 * for: a larger number turns on more than this file needs.
 *
 * Two of the three platforms need a conversion factor that the machine decides
 * at boot and never changes after, so both cache it. Neither cache is a lock and
 * neither is a race. The factor is held in 32 bit halves, written with relaxed
 * stores and published with a release store to a flag that readers acquire, so
 * two threads arriving at once both compute the same number and write the same
 * bytes and no reader can see half of one. 32 bit and not 64 because a 64 bit
 * atomic on a 32 bit machine goes through burrow's spin lock table, and taking a
 * lock to read a clock would be a strange thing to do to the hottest call in the
 * timer code.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if !defined(_WIN32) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 199309L
#endif

#include "burrow/clock.h"

#include "burrow/atomic.h"
#include "burrow/platform.h"

#include <stdint.h>

#if defined(BURROW_OS_WINDOWS)
#include <windows.h>
#elif defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

#if defined(BURROW_OS_WINDOWS)

/* ------------------------------------------------------------------ windows
 *
 * QueryPerformanceCounter, which is what Go uses here. It counts at a rate the
 * machine picks at boot, so the rate has to be asked for and kept. On anything
 * since Windows 8 that rate is ten million, meaning a tick is a hundred
 * nanoseconds and the arithmetic below is exact rather than approximate. */

/* Zero until the two halves below hold the tick rate. */
static uint32_t qpc_ready;
static uint32_t qpc_lo;
static uint32_t qpc_hi;

/* Ticks per second, or zero if the system will not say. */
static int64_t qpc_frequency(void) {
    if (burrow__atomic_load_acquire_u32(&qpc_ready) != 0) {
        uint64_t lo = burrow__atomic_load_relaxed_u32(&qpc_lo);
        uint64_t hi = burrow__atomic_load_relaxed_u32(&qpc_hi);
        return (int64_t)(lo | (hi << 32));
    }

    LARGE_INTEGER freq;

    /* Cannot fail on anything this library builds for, since XP was the last
     * release where it could. A zero would divide by zero below, so it is
     * turned into a useless clock rather than a crash. */
    if (!QueryPerformanceFrequency(&freq) || freq.QuadPart <= 0)
        return 0;

    uint64_t hz = (uint64_t)freq.QuadPart;
    burrow__atomic_store_relaxed_u32(&qpc_lo, (uint32_t)(hz & 0xffffffffu));
    burrow__atomic_store_relaxed_u32(&qpc_hi, (uint32_t)(hz >> 32));
    burrow__atomic_store_release_u32(&qpc_ready, 1);

    return freq.QuadPart;
}

int64_t burrow__nanotime(void) {
    int64_t hz = qpc_frequency();
    if (hz <= 0)
        return 0;

    LARGE_INTEGER now;
    if (!QueryPerformanceCounter(&now))
        return 0;

    /* Divided before it is multiplied, because the counter is a count of
     * nanoseconds in disguise and multiplying it by a billion first overflows a
     * signed 64 bit integer after about nine seconds of uptime. The whole
     * seconds and the remainder are converted separately and neither can. */
    int64_t secs = now.QuadPart / hz;
    int64_t rest = now.QuadPart % hz;
    return secs * 1000000000 + (rest * 1000000000) / hz;
}

#elif defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)

/* ------------------------------------------------------------------- darwin
 *
 * mach_absolute_time, which is what Go uses here. It counts in units the
 * hardware picked rather than in nanoseconds, and mach_timebase_info gives the
 * fraction to multiply by: 1/1 on Intel, where a tick already is a nanosecond,
 * and 125/3 on Apple silicon, where the counter runs at 24 MHz. */

static uint32_t timebase_numer;

/* Written last and with a release, so a reader that sees it nonzero also sees
 * the numerator above. Zero means nobody has asked the kernel yet. */
static uint32_t timebase_denom;

int64_t burrow__nanotime(void) {
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

    /* Split for the same reason as the Windows path. 24 MHz multiplied by 125
     * overflows 64 bits after a century of uptime, which is not a real worry,
     * and the split costs one divide and removes the question. */
    uint64_t whole = (ticks / denom) * numer;
    uint64_t rest = ((ticks % denom) * numer) / denom;
    return (int64_t)(whole + rest);
}

#else

/* -------------------------------------------------------------------- posix
 *
 * CLOCK_MONOTONIC, which covers Linux, the BSDs, illumos and everything else
 * with a POSIX clock. On Linux it reads through the vdso rather than entering
 * the kernel, so it is a few nanoseconds and not a system call.
 *
 * CLOCK_MONOTONIC_RAW exists on Linux and is deliberately not used. It is the
 * hardware counter with no ntp discipline on it, which sounds more correct and
 * is not: a clock nobody may discipline drifts against every other machine, and
 * ntp's correction to a monotonic clock changes its rate rather than jumping it,
 * so it still cannot go backwards. Raw is also slower, because it is not in the
 * vdso on every kernel. Go uses CLOCK_MONOTONIC and so does this. */

int64_t burrow__nanotime(void) {
    struct timespec ts;

    /* Cannot fail for a clock the system defines, and there is nothing useful
     * to return if it somehow does. Zero is a reading like any other, since the
     * only thing a caller may do with these is subtract two of them. */
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;

    return (int64_t)ts.tv_sec * 1000000000 + (int64_t)ts.tv_nsec;
}

#endif
