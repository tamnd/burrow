/* The clocks on Windows.
 *
 * QueryPerformanceCounter for the monotonic one, which is what Go uses here. It
 * counts at a rate the machine picks at boot, so the rate has to be asked for
 * and kept. On anything since Windows 8 that rate is ten million, meaning a
 * tick is a hundred nanoseconds and the arithmetic below is exact rather than
 * approximate.
 *
 * The rate is cached the same way the mach timebase is in time_darwin.c, and
 * for the same reasons: two 32 bit halves written with relaxed stores and
 * published with a release store to a flag that readers acquire.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_WINDOWS)

#include "burrow/atomic.h"
#include "burrow/pal.h"

#include "internal.h"

#include <windows.h>

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

int64_t pal_clock_monotonic(void) {
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

/* The number of 100 nanosecond intervals between 1601-01-01, which is where a
 * FILETIME counts from, and 1970-01-01, which is where the world counts from.
 * It is a constant because both dates are. */
#define FILETIME_TO_UNIX 116444736000000000LL

int64_t pal_clock_realtime(void) {
    FILETIME ft;

    /* Precise rather than the plain one, which is quantised to the system timer
     * tick and so answers the same value for about fifteen milliseconds at a
     * time. Go uses the precise one and so does this. It arrived in Windows 8,
     * which is below everything burrow supports. */
    GetSystemTimePreciseAsFileTime(&ft);

    uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    return ((int64_t)ticks - FILETIME_TO_UNIX) * 100;
}

void pal_nanosleep(int64_t ns) {
    if (ns <= 0) {
        /* Zero means give up the rest of this slice to anything ready to run on
         * this processor, which is what Sleep(0) does. */
        Sleep(0);
        return;
    }

    /* A waitable timer rather than Sleep, because Sleep takes milliseconds and
     * rounds up to the system timer resolution, which is about fifteen
     * milliseconds by default. A timer set with a negative due time is a
     * relative wait in 100 nanosecond units, and on Windows 10 1803 and later
     * the high resolution flag gets it down to about half a millisecond.
     *
     * A machine that will not make the timer falls back to Sleep, which is
     * wrong by up to a tick rather than not working at all. */
    HANDLE timer = CreateWaitableTimerExW(
        NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == NULL)
        timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);

    if (timer == NULL) {
        DWORD ms = (DWORD)((ns + 999999) / 1000000);
        Sleep(ms);
        return;
    }

    LARGE_INTEGER due;
    due.QuadPart = -((ns + 99) / 100);

    if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
        (void)WaitForSingleObject(timer, INFINITE);

    CloseHandle(timer);
}

#endif /* BURROW_OS_WINDOWS */
