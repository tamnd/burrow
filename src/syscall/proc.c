/* syscall.Signal, WaitStatus and Rusage. See include/burrow/syscall.h.
 *
 * Derived from Go's src/syscall/syscall_unix.go, syscall_linux.go,
 * syscall_bsd.go, timestruct.go and syscall.go.
 * Go source: go1.27.1.
 *
 * The Windows forms are from syscall_windows.go and types_windows.go in the
 * same release.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/syscall.h"

#include "internal.h"

#include <string.h>

/* ------------------------------------------------------------------ signals */

Str syscall_signal_string(SyscallSignal s, Alloc *a) {
    char buf[32];
    const char *name = NULL;
    if (s >= 0 && s < burrow__syscall_nsignals)
        name = burrow__syscall_signals[s];
    Int n;
    if (name != NULL && name[0] != 0) {
        n = (Int)strlen(name);
    } else {
        /* "signal " and strconv.Itoa(int(s)). */
        memcpy(buf, "signal ", 7);
        n = 7;
        uint64_t u = s < 0 ? 0 - (uint64_t)s : (uint64_t)s;
        char digits[20];
        int nd = 0;
        do {
            digits[nd++] = (char)('0' + u % 10);
            u /= 10;
        } while (u != 0);
        if (s < 0)
            buf[n++] = '-';
        while (nd > 0)
            buf[n++] = digits[--nd];
        name = buf;
    }
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n + 1, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, name, (size_t)n);
    p[n] = 0;
    return str_from_bytes(p, n);
}

void syscall_signal_signal(SyscallSignal s) {
    (void)s;
}

#if !defined(BURROW_OS_WINDOWS)
Error syscall_kill(Int pid, SyscallSignal sig) {
    PalErrno e = PAL_OK;
    if (pid < INT32_MIN || pid > INT32_MAX || sig < 0 || sig > INT32_MAX)
        return syscall_errno_as_error(SYSCALL_EINVAL, error_allocator());
    if (pal_kill_native((int64_t)pid, (int32_t)sig, &e))
        return BURROW_NO_ERROR;
    return syscall_errno_as_error(syscall_errno_from_pal(e), error_allocator());
}
#endif

/* ------------------------------------------------------------- wait status */

#if defined(BURROW_OS_WINDOWS)

bool syscall_wait_status_exited(SyscallWaitStatus w) {
    (void)w;
    return true;
}

Int syscall_wait_status_exit_status(SyscallWaitStatus w) {
    return (Int)w.exit_code;
}

bool syscall_wait_status_signaled(SyscallWaitStatus w) {
    (void)w;
    return false;
}

SyscallSignal syscall_wait_status_signal(SyscallWaitStatus w) {
    (void)w;
    return -1;
}

bool syscall_wait_status_core_dump(SyscallWaitStatus w) {
    (void)w;
    return false;
}

bool syscall_wait_status_stopped(SyscallWaitStatus w) {
    (void)w;
    return false;
}

bool syscall_wait_status_continued(SyscallWaitStatus w) {
    (void)w;
    return false;
}

SyscallSignal syscall_wait_status_stop_signal(SyscallWaitStatus w) {
    (void)w;
    return -1;
}

Int syscall_wait_status_trap_cause(SyscallWaitStatus w) {
    (void)w;
    return -1;
}

#else

#define WAIT_MASK 0x7Fu
#define WAIT_CORE 0x80u
#define WAIT_EXITED 0x00u
#define WAIT_STOPPED 0x7Fu
#define WAIT_SHIFT 8

bool syscall_wait_status_exited(SyscallWaitStatus w) {
    return (w & WAIT_MASK) == WAIT_EXITED;
}

bool syscall_wait_status_signaled(SyscallWaitStatus w) {
    return (w & WAIT_MASK) != WAIT_STOPPED && (w & WAIT_MASK) != WAIT_EXITED;
}

bool syscall_wait_status_core_dump(SyscallWaitStatus w) {
    return syscall_wait_status_signaled(w) && (w & WAIT_CORE) != 0;
}

SyscallSignal syscall_wait_status_signal(SyscallWaitStatus w) {
    if (!syscall_wait_status_signaled(w))
        return -1;
    return (SyscallSignal)(w & WAIT_MASK);
}

#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_ANDROID) || defined(BURROW_OS_COSMO)

/* syscall_linux.go. */

Int syscall_wait_status_exit_status(SyscallWaitStatus w) {
    if (!syscall_wait_status_exited(w))
        return -1;
    return (Int)((w >> WAIT_SHIFT) & 0xFFu);
}

bool syscall_wait_status_stopped(SyscallWaitStatus w) {
    return (w & 0xFFu) == WAIT_STOPPED;
}

bool syscall_wait_status_continued(SyscallWaitStatus w) {
    return w == 0xFFFFu;
}

SyscallSignal syscall_wait_status_stop_signal(SyscallWaitStatus w) {
    if (!syscall_wait_status_stopped(w))
        return -1;
    return (SyscallSignal)((w >> WAIT_SHIFT) & 0xFFu);
}

Int syscall_wait_status_trap_cause(SyscallWaitStatus w) {
    if (syscall_wait_status_stop_signal(w) != SYSCALL_SIGTRAP)
        return -1;
    return (Int)((w >> WAIT_SHIFT) >> 8);
}

#else

/* syscall_bsd.go, which macOS uses too, and Solaris has the same rules. */

Int syscall_wait_status_exit_status(SyscallWaitStatus w) {
    if ((w & WAIT_MASK) != WAIT_EXITED)
        return -1;
    return (Int)(w >> WAIT_SHIFT);
}

bool syscall_wait_status_stopped(SyscallWaitStatus w) {
    return (w & WAIT_MASK) == WAIT_STOPPED &&
           (SyscallSignal)(w >> WAIT_SHIFT) != SYSCALL_SIGSTOP;
}

bool syscall_wait_status_continued(SyscallWaitStatus w) {
    return (w & WAIT_MASK) == WAIT_STOPPED &&
           (SyscallSignal)(w >> WAIT_SHIFT) == SYSCALL_SIGSTOP;
}

SyscallSignal syscall_wait_status_stop_signal(SyscallWaitStatus w) {
    if (!syscall_wait_status_stopped(w))
        return -1;
    return (SyscallSignal)((w >> WAIT_SHIFT) & 0xFFu);
}

Int syscall_wait_status_trap_cause(SyscallWaitStatus w) {
    (void)w;
    return -1;
}

#endif
#endif

/* -------------------------------------------------------- resource usage */

#if defined(BURROW_OS_WINDOWS)

/* 100 nanosecond intervals from 1601 to 1970. */
#define FILETIME_EPOCH 116444736000000000LL

static SyscallFiletime filetime_of(int64_t v) {
    SyscallFiletime ft;
    ft.low_date_time = (uint32_t)((uint64_t)v & 0xffffffffu);
    ft.high_date_time = (uint32_t)(((uint64_t)v >> 32) & 0xffffffffu);
    return ft;
}

int64_t syscall_filetime_nanoseconds(const SyscallFiletime *ft) {
    uint64_t n = ((uint64_t)ft->high_date_time << 32) + (uint64_t)ft->low_date_time;
    n -= (uint64_t)FILETIME_EPOCH;
    return (int64_t)(n * 100u);
}

SyscallFiletime syscall_nsec_to_filetime(int64_t nsec) {
    nsec /= 100;
    return filetime_of((int64_t)((uint64_t)nsec + (uint64_t)FILETIME_EPOCH));
}

SyscallRusage syscall_rusage_from_pal(const PalRusage *ru) {
    SyscallRusage r;
    r.creation_time = filetime_of(ru->creation_time);
    r.exit_time = filetime_of(ru->exit_time);
    r.kernel_time = filetime_of(ru->kernel_time);
    r.user_time = filetime_of(ru->user_time);
    return r;
}

#else

int64_t syscall_timeval_nano(const SyscallTimeval *tv) {
    return (int64_t)((uint64_t)tv->sec * 1000000000u + (uint64_t)tv->usec * 1000u);
}

int64_t syscall_timeval_to_nsec(SyscallTimeval tv) {
    return syscall_timeval_nano(&tv);
}

SyscallTimeval syscall_nsec_to_timeval(int64_t nsec) {
    nsec = (int64_t)((uint64_t)nsec + 999u); /* round up to a microsecond */
    int64_t usec = nsec % 1000000000 / 1000;
    int64_t sec = nsec / 1000000000;
    if (usec < 0) {
        usec += 1000000;
        sec--;
    }
    SyscallTimeval tv = {sec, usec};
    return tv;
}

SyscallRusage syscall_rusage_from_pal(const PalRusage *ru) {
    SyscallRusage r;
    r.utime.sec = ru->utime_sec;
    r.utime.usec = ru->utime_usec;
    r.stime.sec = ru->stime_sec;
    r.stime.usec = ru->stime_usec;
    r.maxrss = ru->maxrss;
    r.ixrss = ru->ixrss;
    r.idrss = ru->idrss;
    r.isrss = ru->isrss;
    r.minflt = ru->minflt;
    r.majflt = ru->majflt;
    r.nswap = ru->nswap;
    r.inblock = ru->inblock;
    r.oublock = ru->oublock;
    r.msgsnd = ru->msgsnd;
    r.msgrcv = ru->msgrcv;
    r.nsignals = ru->nsignals;
    r.nvcsw = ru->nvcsw;
    r.nivcsw = ru->nivcsw;
    return r;
}

Int syscall_wait4(Int pid, SyscallWaitStatus *wstatus, Int options,
                  SyscallRusage *rusage, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (pid < INT32_MIN || pid > INT32_MAX || options < INT32_MIN ||
        options > INT32_MAX) {
        BURROW_OUT(err, syscall_errno_as_error(SYSCALL_EINVAL, error_allocator()));
        return -1;
    }
    PalErrno e = PAL_OK;
    uint32_t st = 0;
    PalRusage ru;
    int64_t got = pal_wait4((int64_t)pid, &st, (int32_t)options, &ru, &e);
    if (got < 0) {
        BURROW_OUT(
            err, syscall_errno_as_error(syscall_errno_from_pal(e), error_allocator()));
        return -1;
    }
    BURROW_OUT(wstatus, st);
    if (rusage != NULL)
        *rusage = syscall_rusage_from_pal(&ru);
    return (Int)got;
}

#endif

/* -------------------------------------------------------------- the type */

const Type burrow__syscall_signal_desc = {
    {(const Byte *)"Signal", 6},
    {(const Byte *)"syscall", 7},
    KIND_INT,
    (uint32_t)sizeof(SyscallSignal),
    (uint16_t)_Alignof(SyscallSignal),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x73797367U, /* "sysg" */
    NULL,
};

const Type *const TYPE_SYSCALL_SIGNAL = &burrow__syscall_signal_desc;
