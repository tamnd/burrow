/* The methods of Timespec and Timeval, and the conversions to and from
 * nanoseconds.
 *
 * Derived from Go's src/syscall/syscall.go, timestruct.go and
 * types_windows.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#include <string.h>

/* Sets f to v. The fields of Timespec and Timeval are as wide as the
 * system's, 32 or 64 bits, so the conversion depends on the platform, as Go's
 * setTimespec and setTimeval do. */
#define TIME_SET(f, v)                                                                 \
    ((f) = _Generic((f), int32_t: (int32_t)(v), int64_t: (int64_t)(v)))

int64_t syscall_timespec_unix(const SyscallTimespec *ts, int64_t *nsec) {
    BURROW_OUT(nsec, (int64_t)ts->nsec);
    return (int64_t)ts->sec;
}

int64_t syscall_timeval_unix(const SyscallTimeval *tv, int64_t *nsec) {
    BURROW_OUT(nsec, (int64_t)tv->usec * 1000);
    return (int64_t)tv->sec;
}

/* Go's arithmetic wraps, so this does it unsigned, where C's does too. */
int64_t syscall_timespec_nano(const SyscallTimespec *ts) {
    return (int64_t)((uint64_t)(int64_t)ts->sec * 1000000000U +
                     (uint64_t)(int64_t)ts->nsec);
}

int64_t syscall_timeval_nano(const SyscallTimeval *tv) {
    return (int64_t)((uint64_t)(int64_t)tv->sec * 1000000000U +
                     (uint64_t)(int64_t)tv->usec * 1000U);
}

int64_t syscall_timespec_to_nsec(SyscallTimespec ts) {
    return syscall_timespec_nano(&ts);
}

#if defined(BURROW_OS_WINDOWS)

int64_t syscall_timeval_nanoseconds(const SyscallTimeval *tv) {
    return (
        int64_t)(((uint64_t)(int64_t)tv->sec * 1000000U + (uint64_t)(int64_t)tv->usec) *
                 1000U);
}

SyscallTimeval syscall_nsec_to_timeval(int64_t nsec) {
    SyscallTimeval tv;
    memset(&tv, 0, sizeof tv);
    tv.sec = (int32_t)(nsec / 1000000000);
    tv.usec = (int32_t)(nsec % 1000000000 / 1000);
    return tv;
}

SyscallTimespec syscall_nsec_to_timespec(int64_t nsec) {
    SyscallTimespec ts;
    memset(&ts, 0, sizeof ts);
    ts.sec = nsec / 1000000000;
    ts.nsec = nsec % 1000000000;
    return ts;
}

#else

int64_t syscall_timeval_to_nsec(SyscallTimeval tv) {
    return syscall_timeval_nano(&tv);
}

SyscallTimespec syscall_nsec_to_timespec(int64_t nsec) {
    int64_t sec = nsec / 1000000000;
    nsec = nsec % 1000000000;
    if (nsec < 0) {
        nsec += 1000000000;
        sec--;
    }
    SyscallTimespec ts;
    memset(&ts, 0, sizeof ts);
    TIME_SET(ts.sec, sec);
    TIME_SET(ts.nsec, nsec);
    return ts;
}

SyscallTimeval syscall_nsec_to_timeval(int64_t nsec) {
    nsec = (int64_t)((uint64_t)nsec + 999U); /* round up to a microsecond */
    int64_t usec = nsec % 1000000000 / 1000;
    int64_t sec = nsec / 1000000000;
    if (usec < 0) {
        usec += 1000000;
        sec--;
    }
    SyscallTimeval tv;
    memset(&tv, 0, sizeof tv);
    TIME_SET(tv.sec, sec);
    TIME_SET(tv.usec, usec);
    return tv;
}

#endif
