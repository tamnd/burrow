/* Waiting on an address on Linux, which is the futex the other two backends
 * imitate.
 *
 * The whole thing is one 32 bit word of the caller's own memory. The kernel
 * only learns the word exists when a thread actually has to sleep on it, so an
 * uncontended wait costs a load and an uncontended wake costs nothing at all.
 * That is why this file exists rather than letting Linux use futex_posix.c.
 *
 * This was src/runtime/note.c's linux half until the platform layer existed,
 * and the reasoning in the comments below came with it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/platform.h"

#if defined(BURROW_OS_LINUX)

#include "burrow/pal.h"

#include "internal.h"

#include <errno.h>
#include <sys/syscall.h>

/* FUTEX_WAIT and FUTEX_WAKE are 0 and 1, and the PRIVATE flag is 128, which
 * says the futex is never shared between processes and lets the kernel skip
 * looking the page up in the shared mapping table.
 *
 * <linux/futex.h> has these, and pulling a kernel header into a userspace build
 * is the kind of thing that works until the day somebody builds against a
 * different kernel's headers. The three numbers are part of the system call
 * interface and cannot change. */
#define FUTEX_WAIT_PRIVATE 128
#define FUTEX_WAKE_PRIVATE 129

/* Which system call number to use, and what shape the timeout that goes with it
 * has. Architectures added after 2019 have no 32 bit time_t and so have only
 * the time64 variant, while everything older has both and the plain one is what
 * its libc uses.
 *
 * The timespec is written out here rather than taken from <time.h> because the
 * two do not have to agree. SYS_futex wants the kernel's old timespec, whose
 * fields are both a long, and a 32 bit build with _TIME_BITS=64 has a libc
 * timespec whose seconds field is eight bytes wide. Handing the second to the
 * first is a struct the kernel reads the wrong way, and it is the sort of
 * mistake that only appears on the one platform nobody builds for. Writing the
 * layout that goes with the chosen system call number keeps the two together. */
#if defined(SYS_futex)
#define PAL_SYS_FUTEX SYS_futex
typedef long FutexTime;
#elif defined(SYS_futex_time64)
#define PAL_SYS_FUTEX SYS_futex_time64
typedef int64_t FutexTime;
#else
#error "no futex system call number on this Linux architecture"
#endif

typedef struct FutexTimespec {
    FutexTime tv_sec;
    FutexTime tv_nsec;
} FutexTimespec;

/* Declared here rather than taken from <unistd.h>, because both glibc and musl
 * hide syscall behind _GNU_SOURCE and burrow is built as strict C11. The
 * prototype is the one both of them use and it is fixed by the ABI, so writing
 * it out is not a guess. */
extern long syscall(long number, ...);

/* The longest any single wait may be, a thousand seconds. It costs one extra
 * system call every quarter of an hour, and in exchange the conversion from
 * nanoseconds into the kernel's timespec is something that cannot overflow
 * rather than something that usually does not. */
#define FUTEX_MAX_WAIT_NS 1000000000000LL

bool pal_futex_wait(uint32_t *addr, uint32_t expect, int64_t timeout_ns,
                    PalErrno *err) {
    if (addr == NULL) {
        BURROW_OUT(err, PAL_EINVAL);
        return false;
    }

    FutexTimespec ts;
    FutexTimespec *tp = NULL;

    if (timeout_ns >= 0) {
        int64_t ns = timeout_ns > FUTEX_MAX_WAIT_NS ? FUTEX_MAX_WAIT_NS : timeout_ns;

        /* The kernel reads this as a duration and measures it on
         * CLOCK_MONOTONIC, which is the clock pal_clock_monotonic reads, so the
         * two agree about how long a second is without any conversion. */
        ts.tv_sec = (FutexTime)(ns / 1000000000);
        ts.tv_nsec = (FutexTime)(ns % 1000000000);
        tp = &ts;
    }

    if (syscall(PAL_SYS_FUTEX, addr, FUTEX_WAIT_PRIVATE, expect, tp, NULL, 0) == 0) {
        BURROW_OUT(err, PAL_OK);
        return true;
    }

    switch (errno) {
    case EAGAIN:
        /* The word already differed, which is not a failure. See pal.h. */
        BURROW_OUT(err, PAL_OK);
        return true;
    case ETIMEDOUT:
        BURROW_OUT(err, PAL_ETIMEDOUT);
        return false;
    case EINTR:
        BURROW_OUT(err, PAL_EINTR);
        return false;
    default:
        BURROW_OUT(err, burrow__pal_errno(errno));
        return false;
    }
}

int64_t pal_futex_wake(uint32_t *addr, int64_t n, PalErrno *err) {
    if (addr == NULL || n < 0) {
        BURROW_OUT(err, PAL_EINVAL);
        return -1;
    }

    /* The kernel takes an int, and everything past INT32_MAX means the same
     * thing as INT32_MAX does: all of them. */
    int32_t want = n > INT32_MAX ? INT32_MAX : (int32_t)n;

    long woken = syscall(PAL_SYS_FUTEX, addr, FUTEX_WAKE_PRIVATE, want, NULL, NULL, 0);
    if (woken < 0) {
        BURROW_OUT(err, burrow__pal_errno(errno));
        return -1;
    }

    BURROW_OUT(err, PAL_OK);
    return (int64_t)woken;
}

#endif /* BURROW_OS_LINUX */
