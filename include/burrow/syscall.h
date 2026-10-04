/* syscall's Errno, the error number a system call failed with.
 *
 * Go's syscall package is mostly the raw system calls, and burrow keeps those
 * behind the PAL. What the rest of the library needs from it is Errno, because
 * os wraps one in every PathError and code tests for it by value:
 *
 *     const SyscallErrno *e = errors_as(err, TYPE_SYSCALL_ERRNO);
 *     if (e != NULL && *e == SYSCALL_ENOENT)
 *
 * The SYSCALL_E constants are the system's own numbers, generated from Go's
 * tables, so SYSCALL_EAGAIN is 11 on Linux and 35 on macOS, and the text of
 * an Errno is the text Go gives on that system. On Windows the constants also
 * include the ERROR_ and WSA codes Go names, and an Errno's text comes from
 * FormatMessage, as it does in Go. The E constants Windows has no code for are
 * numbers Go made up, from APPLICATION_ERROR on, with the Unix text.
 *
 * An Errno is a number. syscall_errno_as_error boxes it into an Error, whose
 * errors_is matches the io/fs errors the way Go's Errno.Is does, and whose
 * type has the Timeout and Temporary methods, so a PathError around it answers
 * fs_path_error_timeout too.
 *
 * Signal is here too, with its names, and WaitStatus and Rusage, which are what
 * os.ProcessState is made of. The rest of syscall's constants and types are
 * generated from Go's tables, in syscall/zconst.h and syscall/ztypes.h, and
 * its system calls from Go's own wrappers, in syscall/zsyscall.h.
 *
 * Derived from Go's src/syscall/syscall_unix.go, syscall_linux.go and
 * syscall_bsd.go, and on Windows syscall_windows.go and types_windows.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package syscall */

#ifndef BURROW_SYSCALL_H
#define BURROW_SYSCALL_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/pal.h"
#include "burrow/platform.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SyscallErrno and SyscallSignal. */
#include "burrow/syscall/base.h"
#include "burrow/syscall/zconst.h"
#include "burrow/syscall/zerrors.h"
#include "burrow/syscall/ztypes.h"

/* The descriptor of Errno, for errors_as. What errors_as hands back points at
 * a SyscallErrno. */
extern const Type *const TYPE_SYSCALL_ERRNO;

/* Errno.Error: the text for e, built in a. On Unix it is Go's table entry, or
 * "errno " and the number for one the table does not have. On Windows it is
 * the table entry for the numbers Go made up, then what FormatMessage says,
 * and "winapi error #" and the number when that fails. A failed allocation
 * gives the empty string. */
BURROW_OWNS(ret) Str syscall_errno_error(SyscallErrno e, Alloc *a);

/* e as an Error, in one allocation from a. Its text is syscall_errno_error's.
 * A failed allocation gives burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error syscall_errno_as_error(SyscallErrno e, Alloc *a);

/* Errno.Is: whether e counts as target, which is one of fs_err_permission,
 * fs_err_exist, fs_err_not_exist or errors_err_unsupported. EACCES and EPERM
 * are permission errors, EEXIST and ENOTEMPTY mean it exists, ENOENT means
 * it does not, and ENOSYS, ENOTSUP and EOPNOTSUPP are unsupported. Windows
 * adds its own ERROR_ codes to each. */
bool syscall_errno_is(SyscallErrno e, Error target);

/* Errno.Timeout: EAGAIN, EWOULDBLOCK and ETIMEDOUT. */
bool syscall_errno_timeout(SyscallErrno e);

/* Errno.Temporary: EINTR, EMFILE, ENFILE everywhere but Windows, and the
 * timeouts. */
bool syscall_errno_temporary(SyscallErrno e);

/* The Errno for a PAL failure. Straight after the PAL call that failed with e
 * it is the system's own code, which on Windows keeps the ERROR_ code Go would
 * show. Later, or for a PalErrno a backend chose itself, it is the E constant
 * for e. PAL_OK gives 0, and so do the PalErrno values with no errno behind
 * them, PAL_EHOSTNOTFOUND, PAL_ETRYAGAIN and PAL_EOTHER, unless they come
 * straight from a failed call with a code of its own. */
SyscallErrno syscall_errno_from_pal(PalErrno e);

/* ------------------------------------------------------------------ signals */

/* Signal.String: Go's name for s on this system, such as "interrupt" or
 * "killed", and "signal " and the number for one it has no name for, built in
 * a. A failed allocation gives the empty string. */
BURROW_OWNS(ret) Str syscall_signal_string(SyscallSignal s, Alloc *a);

/* Signal.Signal, which does nothing. In Go it is the method that makes a
 * Signal an os.Signal. */
void syscall_signal_signal(SyscallSignal s);

/* The descriptor of Signal, which os_signal_from_syscall's OsSignal has as its
 * self_type. */
extern const Type *const TYPE_SYSCALL_SIGNAL;

/* The descriptor TYPE_SYSCALL_SIGNAL points at, for a table in another file
 * that needs it as a constant. Use TYPE_SYSCALL_SIGNAL. */
extern const Type burrow__syscall_signal_desc;

#if !defined(BURROW_OS_WINDOWS)
/* kill(2): send sig to pid, which reaches a process group when it is 0 or
 * negative, as it does in C. The error is the Errno, from error_allocator. */
BURROW_OWNS(ret) Error syscall_kill(Int pid, SyscallSignal sig);
#endif

/* ------------------------------------------------------------ process attr */

#if defined(BURROW_OS_WINDOWS)
/* syscall.SysProcAttr on Windows, with the one field so far. creation_flags
 * is added to the flags CreateProcess is given, so CREATE_NEW_PROCESS_GROUP
 * works as it does in Go. */
typedef struct SyscallSysProcAttr {
    uint32_t creation_flags;
} SyscallSysProcAttr;
#else
/* syscall.SysProcAttr, with the fields burrow can do so far. setsid gives the
 * child a session of its own. setpgid puts it in a new process group, and
 * pgid has to be 0, which is the new group, since joining another one is not
 * done yet and gives "not supported". */
typedef struct SyscallSysProcAttr {
    bool setsid;
    bool setpgid;
    Int pgid;
} SyscallSysProcAttr;
#endif

/* ------------------------------------------------------------- wait status */

#if defined(BURROW_OS_WINDOWS)
/* syscall.WaitStatus on Windows: the exit code GetExitCodeProcess gives. */
typedef struct SyscallWaitStatus {
    uint32_t exit_code;
} SyscallWaitStatus;
#else
/* syscall.WaitStatus: the status wait4 gives, as it gives it. */
typedef uint32_t SyscallWaitStatus;
#endif

/* The WaitStatus methods. A Windows process always exited, with no signal, so
 * there Exited is true, the Signal ones are -1 and the rest are false. */
bool syscall_wait_status_exited(SyscallWaitStatus w);
Int syscall_wait_status_exit_status(SyscallWaitStatus w); /* -1 unless Exited */
bool syscall_wait_status_signaled(SyscallWaitStatus w);
SyscallSignal syscall_wait_status_signal(SyscallWaitStatus w); /* -1 unless Signaled */
bool syscall_wait_status_core_dump(SyscallWaitStatus w);
bool syscall_wait_status_stopped(SyscallWaitStatus w);
bool syscall_wait_status_continued(SyscallWaitStatus w);
SyscallSignal
syscall_wait_status_stop_signal(SyscallWaitStatus w); /* -1 unless Stopped */
/* The ptrace event of a process stopped at a SIGTRAP, on Linux. -1 elsewhere. */
Int syscall_wait_status_trap_cause(SyscallWaitStatus w);

/* -------------------------------------------------------- resource usage */

#if defined(BURROW_OS_WINDOWS)
/* syscall.Filetime, in ztypes.h, is 100 nanosecond intervals since 1601, in
 * two halves. */

/* Filetime.Nanoseconds: the time since the Unix epoch in nanoseconds. */
int64_t syscall_filetime_nanoseconds(const SyscallFiletime *ft);

/* NsecToFiletime: the other way. */
SyscallFiletime syscall_nsec_to_filetime(int64_t nsec);

/* syscall.Rusage on Windows, in ztypes.h, is what GetProcessTimes says. The
 * creation and exit times are dates, and the kernel and user times are
 * amounts. */
#else
/* syscall.Timeval and syscall.Rusage are in ztypes.h, with the system's own
 * field sizes, which are 32 bits on most 32-bit systems. Rusage is what wait4
 * says a child used, and the fields a system does not keep are 0. */

/* Timeval.Nano and TimevalToNsec: tv in nanoseconds. */
int64_t syscall_timeval_nano(const SyscallTimeval *tv);
int64_t syscall_timeval_to_nsec(SyscallTimeval tv);

/* NsecToTimeval, which rounds up to the next microsecond, as Go's does. */
SyscallTimeval syscall_nsec_to_timeval(int64_t nsec);

/* wait4(2): wait for pid and give back its id. wstatus and rusage may be
 * NULL, and options are the system's own wait4 flags. A signal that arrives
 * first is EINTR, which Go returns rather than retries, and so does this. */
Int syscall_wait4(Int pid, SyscallWaitStatus *wstatus, Int options,
                  SyscallRusage *rusage, Error *err);
#endif

/* The Rusage for what pal_wait4 filled in, as syscall_errno_from_pal is the
 * Errno for a PalErrno. */
SyscallRusage syscall_rusage_from_pal(const PalRusage *ru);

/* ------------------------------------------------------------- RawConn */

/* The callbacks syscall.RawConn takes: Control's gets the descriptor, or the
 * HANDLE on Windows, and Read's and Write's say whether they are done. */
BURROW_FUNC(SyscallFdFunc, void, Uintptr fd);
BURROW_FUNC(SyscallFdDoneFunc, bool, Uintptr fd);

/* syscall.RawConn: access to the descriptor under a file or connection, held
 * open for as long as a callback runs and no longer. */
typedef struct SyscallRawConnVT {
    const Type *self_type;
    Error (*control)(void *self, SyscallFdFunc f);
    Error (*read)(void *self, SyscallFdDoneFunc f);
    Error (*write)(void *self, SyscallFdDoneFunc f);
} SyscallRawConnVT;

typedef struct SyscallRawConn {
    const SyscallRawConnVT *vt;
    void *data;
} SyscallRawConn;

/* ----------------------------------------------------------- raw calls */

/* syscall.BytePtrFromString: s with a NUL after it, from a, which is s.len + 1
 * bytes to free. ByteSliceFromString is the same as a Slice. Either fails with
 * EINVAL if s has a NUL in it already. */
BURROW_OWNS(ret) uint8_t *syscall_byte_ptr_from_string(Alloc *a, Str s, Error *err);
BURROW_OWNS(ret) Slice syscall_byte_slice_from_string(Alloc *a, Str s, Error *err);

#if !defined(BURROW_OS_WINDOWS)
/* syscall.Syscall, Syscall6 and the Raw ones: the system call trap, one of
 * the SYSCALL_SYS_ constants, with the arguments as integers. They return
 * what the call returned, all ones if it failed, with the Errno in *err, or 0
 * if it worked. *r2 is always 0, since the C library has nowhere to put a
 * second result register. Either may be NULL.
 *
 * burrow has no goroutines that a blocked call would hold up, so the Raw ones
 * are the same as the others. On macOS these go through syscall(2), which
 * Apple has deprecated, and the functions in syscall/zsyscall.h go through
 * libSystem the way Go's do, which is the better choice there. On Cosmopolitan
 * and wasip1 every call fails with ENOSYS. */
Uintptr syscall_syscall(Uintptr trap, Uintptr a1, Uintptr a2, Uintptr a3, Uintptr *r2,
                        SyscallErrno *err);
Uintptr syscall_syscall6(Uintptr trap, Uintptr a1, Uintptr a2, Uintptr a3, Uintptr a4,
                         Uintptr a5, Uintptr a6, Uintptr *r2, SyscallErrno *err);
Uintptr syscall_raw_syscall(Uintptr trap, Uintptr a1, Uintptr a2, Uintptr a3,
                            Uintptr *r2, SyscallErrno *err);
Uintptr syscall_raw_syscall6(Uintptr trap, Uintptr a1, Uintptr a2, Uintptr a3,
                             Uintptr a4, Uintptr a5, Uintptr a6, Uintptr *r2,
                             SyscallErrno *err);
#if !defined(BURROW_OS_LINUX) && !defined(BURROW_OS_COSMO) && !defined(BURROW_OS_WASI)
/* syscall.Syscall9, which the BSDs and macOS have and Linux does not. */
Uintptr syscall_syscall9(Uintptr trap, Uintptr a1, Uintptr a2, Uintptr a3, Uintptr a4,
                         Uintptr a5, Uintptr a6, Uintptr a7, Uintptr a8, Uintptr a9,
                         Uintptr *r2, SyscallErrno *err);
#endif
#endif

/* The rest of the system calls, one function each. */
#include "burrow/syscall/zsyscall.h"

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SYSCALL_H */
