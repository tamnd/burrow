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
#include "burrow/sync.h"
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
/* syscall.Credential: the user and groups the child runs as. groups is a
 * slice of uint32_t, the supplementary groups, set before the group and the
 * user unless no_set_groups is true. */
typedef struct SyscallCredential {
    uint32_t uid;
    uint32_t gid;
    Slice groups; /* of uint32_t */
    bool no_set_groups;
} SyscallCredential;

/* syscall.SysProcAttr: what the child does between the fork and the exec, in
 * the order the fields are listed, which is Go's order on every system.
 *
 * chroot, when not empty, is the child's new root. credential, when not NULL,
 * is the user it runs as. ptrace has it call ptrace(PTRACE_TRACEME), so that
 * it stops at the exec for the parent to trace.
 *
 * setsid gives it a session of its own. setpgid puts it in process group pgid,
 * or in a new one of its own when pgid is 0. setctty makes descriptor ctty its
 * controlling terminal, which only works with setsid, and ctty is a slot in
 * ProcAttr.files there, so a number in the child. noctty detaches descriptor
 * 0 from its terminal. foreground puts its group in the foreground of the
 * terminal ctty, which is a descriptor of the parent's here, and implies
 * setpgid. setctty and foreground together are an error, as they are in Go.
 *
 * The rest are Linux's, except pdeathsig, which FreeBSD has too, and jail,
 * which is FreeBSD's alone. pdeathsig is the signal the child gets when the
 * thread that started it exits. cloneflags are given to clone, though never
 * CLONE_VM or CLONE_VFORK, since the child is a copy of the parent here as it
 * is after fork, and unshareflags to unshare in the child. uid_mappings and
 * gid_mappings are slices of SyscallSysProcIDMap, written to the child's
 * uid_map and gid_map, and gid_mappings_enable_setgroups is whether a child
 * with gid_mappings may call setgroups. ambient_caps, a slice of Uintptr, are
 * raised in the child, which keeps them through the exec. use_cgroup_fd starts
 * it in the cgroup cgroup_fd is open on. pid_fd, when not NULL, is set to a
 * pidfd for it, or -1 when the system has none to give. */
typedef struct SyscallSysProcAttr {
    Str chroot;
    const SyscallCredential *credential;
    bool ptrace;
    bool setsid;
    bool setpgid;
    bool setctty;
    bool noctty;
    Int ctty;
    bool foreground;
    Int pgid;
#if defined(BURROW_OS_LINUX)
    SyscallSignal pdeathsig;
    Uintptr cloneflags;
    Uintptr unshareflags;
    Slice uid_mappings; /* of SyscallSysProcIDMap */
    Slice gid_mappings; /* of SyscallSysProcIDMap */
    bool gid_mappings_enable_setgroups;
    Slice ambient_caps; /* of Uintptr */
    bool use_cgroup_fd;
    Int cgroup_fd;
    Int *pid_fd;
#elif defined(BURROW_OS_FREEBSD)
    SyscallSignal pdeathsig;
    Int jail;
#endif
} SyscallSysProcAttr;

/* syscall.ProcAttr: dir is the child's working directory, and the empty Str
 * leaves it ours. env is a slice of "key=value" Strs, and nil is no
 * environment at all, not ours, which is Go's rule here and not os's. files
 * is a slice of Uintptr, the parent's descriptors that become the child's 0,
 * 1, 2 and so on, with ~(Uintptr)0 leaving that one closed. sys may be NULL.
 *
 * Descriptors that are not in files and are not close on exec stay open in
 * the child, as they do in Go, which opens everything close on exec. So does
 * burrow, but a C program may not, and os_start_process closes them. */
typedef struct SyscallProcAttr {
    Str dir;
    Slice env;   /* of Str */
    Slice files; /* of Uintptr */
    const SyscallSysProcAttr *sys;
} SyscallProcAttr;

/* syscall.ForkLock. A descriptor that is opened without close on exec and
 * marked after is opened under the read lock, and every fork takes the write
 * lock, syscall_fork_exec's and os_start_process's, so that the child never
 * gets one of those halfway. */
extern SyncRWMutex syscall_fork_lock;

/* syscall.ForkExec: starts argv0 with argv, set up as attr says, which may be
 * NULL, and gives its process id. A failed exec is reported here rather than
 * by the child exiting, because the child sends back its errno over a pipe
 * the exec closes. The error is an Errno, or the text Go gives for a
 * SysProcAttr that cannot work, from error_allocator. */
BURROW_OWNS(err) Int syscall_fork_exec(Str argv0, Slice argv,
                                       const SyscallProcAttr *attr, Error *err);

/* syscall.StartProcess: syscall_fork_exec for package os. handle is always 0
 * here and may be NULL. */
BURROW_OWNS(err) Int syscall_start_process(Str argv0, Slice argv,
                                           const SyscallProcAttr *attr, Uintptr *handle,
                                           Error *err);

/* syscall.Exec: execve, which replaces this process with argv0 and so only
 * returns when it fails. argv and envv are slices of Str, and envv is the
 * whole environment the program gets. */
BURROW_OWNS(ret) Error syscall_exec(Str argv0, Slice argv, Slice envv);

/* syscall.Setgroups: the supplementary groups, a slice of Int. */
BURROW_OWNS(ret) Error syscall_setgroups(Slice gids);

#if defined(BURROW_OS_LINUX)
/* syscall.Setuid and the rest of the family. Each one changes every thread of
 * the process, as POSIX says it should, which is what Go does when it is
 * linked with cgo, and burrow always is: the C library does it. */
BURROW_OWNS(ret) Error syscall_setuid(Int uid);
BURROW_OWNS(ret) Error syscall_setgid(Int gid);
BURROW_OWNS(ret) Error syscall_seteuid(Int euid);
BURROW_OWNS(ret) Error syscall_setegid(Int egid);
BURROW_OWNS(ret) Error syscall_setreuid(Int ruid, Int euid);
BURROW_OWNS(ret) Error syscall_setregid(Int rgid, Int egid);
BURROW_OWNS(ret) Error syscall_setresuid(Int ruid, Int euid, Int suid);
BURROW_OWNS(ret) Error syscall_setresgid(Int rgid, Int egid, Int sgid);

/* syscall.AllThreadsSyscall and AllThreadsSyscall6, which run a system call
 * on every thread. Go refuses them in a program linked with cgo, since it
 * does not know about the C library's threads, and burrow is in the same
 * place: they fail with ENOTSUP and make no call. */
Uintptr syscall_all_threads_syscall(Uintptr trap, Uintptr a1, Uintptr a2, Uintptr a3,
                                    Uintptr *r2, SyscallErrno *err);
Uintptr syscall_all_threads_syscall6(Uintptr trap, Uintptr a1, Uintptr a2, Uintptr a3,
                                     Uintptr a4, Uintptr a5, Uintptr a6, Uintptr *r2,
                                     SyscallErrno *err);
#endif

/* What syscall_fork_exec and os_start_process both do with a SysProcAttr
 * before the fork: Go's two checks against it, with nfiles the length of
 * ProcAttr.files, and the attributes as pal_spawn takes them, in out and
 * flags, with whatever they point at from a. Not the interface. */
bool burrow__syscall_spawn_sys(const SyscallSysProcAttr *sys, Int nfiles, Alloc *a,
                               PalSpawnSys *out, uint32_t *flags, Error *err);
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

/* --------------------------------------------------------------------- time */

/* syscall.Timespec and syscall.Timeval are in ztypes.h, with the system's own
 * field sizes, which are 32 bits on most 32-bit systems. */

/* Timespec.Unix and Timeval.Unix: the seconds, with the nanoseconds in *nsec,
 * which may be NULL. */
int64_t syscall_timespec_unix(const SyscallTimespec *ts, int64_t *nsec);
int64_t syscall_timeval_unix(const SyscallTimeval *tv, int64_t *nsec);

/* Timespec.Nano, Timeval.Nano and TimespecToNsec: the time in nanoseconds. */
int64_t syscall_timespec_nano(const SyscallTimespec *ts);
int64_t syscall_timeval_nano(const SyscallTimeval *tv);
int64_t syscall_timespec_to_nsec(SyscallTimespec ts);

#if defined(BURROW_OS_WINDOWS)
/* Timeval.Nanoseconds, which is Nano under the name Windows has. */
int64_t syscall_timeval_nanoseconds(const SyscallTimeval *tv);
#else
/* TimevalToNsec, which Windows does not have. */
int64_t syscall_timeval_to_nsec(SyscallTimeval tv);
#endif

/* NsecToTimespec and NsecToTimeval. On Unix a negative time gives a positive
 * nsec or usec with one second less, and NsecToTimeval rounds up to the next
 * microsecond. Windows does what Go does there, which is neither: it
 * truncates toward 0. */
SyscallTimespec syscall_nsec_to_timespec(int64_t nsec);
SyscallTimeval syscall_nsec_to_timeval(int64_t nsec);

/* ------------------------------------------------------------- environment */

/* syscall.Getenv: the value of key, from a, and in *found whether it is set at
 * all. On Unix the environment is read once, the first time it is needed, and
 * then kept here, so a setenv(3) another library makes after that is not seen,
 * as in Go. On Windows it is the process's own, each time. A failed
 * allocation gives the empty string with *found true. */
BURROW_OWNS(ret) Str syscall_getenv(Alloc *a, Str key, bool *found);

/* syscall.Setenv: set key to value. An empty key, or one with '=' or a NUL in
 * it, or a value with a NUL, is EINVAL. On Unix the C library's copy is set
 * too, so C code in the same process sees it. */
BURROW_OWNS(ret) Error syscall_setenv(Str key, Str value);

/* syscall.Unsetenv, which is not an error when key is not set. */
BURROW_OWNS(ret) Error syscall_unsetenv(Str key);

/* syscall.Clearenv: unset everything. */
void syscall_clearenv(void);

/* syscall.Environ: a Slice of Str, "key=value" each, all from a. A failed
 * allocation gives a nil slice. */
BURROW_OWNS(ret) Slice syscall_environ(Alloc *a);

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
/* syscall.Rusage is in ztypes.h, with the system's own field sizes, which are
 * 32 bits on most 32-bit systems. It is what wait4 says a child used, and the
 * fields a system does not keep are 0. */

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

/* syscall.StringByteSlice and StringBytePtr, which panic where the others give
 * EINVAL. Go has deprecated them. A failed allocation gives a nil slice. */
BURROW_OWNS(ret) Slice syscall_string_byte_slice(Alloc *a, Str s);
BURROW_OWNS(ret) uint8_t *syscall_string_byte_ptr(Alloc *a, Str s);

/* syscall.Getpagesize: the size of a memory page. */
Int syscall_getpagesize(void);

/* syscall.Exit: end the process now with code, running nothing on the way
 * out, as Go's does. */
BURROW_NORETURN void syscall_exit(Int code);

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

#if !defined(BURROW_OS_WINDOWS)
/* -------------------------------------------------------------------- Unix */

/* syscall.Stdin, Stdout and Stderr: 0, 1 and 2, as variables, since Go has
 * them as variables. */
extern Int syscall_stdin;
extern Int syscall_stdout;
extern Int syscall_stderr;

/* read(2), write(2), pread(2) and pwrite(2) on p's bytes. They give back what
 * the call did, which is -1 with the Errno in *err when it failed. */
Int syscall_read(Int fd, Slice p, Error *err);
Int syscall_write(Int fd, Slice p, Error *err);
Int syscall_pread(Int fd, Slice p, int64_t offset, Error *err);
Int syscall_pwrite(Int fd, Slice p, int64_t offset, Error *err);

/* syscall.SlicePtrFromStrings: the Str values in ss as C strings, in a Slice of
 * uint8_t * with a NULL after the last, for execve. It fails with EINVAL if a
 * string has a NUL in it. The pointers and the bytes are one allocation from
 * a, which syscall_slice_ptr_free gives back. */
BURROW_OWNS(ret) Slice syscall_slice_ptr_from_strings(Alloc *a, Slice ss, Error *err);

/* syscall.StringSlicePtr, which panics where SlicePtrFromStrings gives EINVAL.
 * Go has deprecated it. */
BURROW_OWNS(ret) Slice syscall_string_slice_ptr(Alloc *a, Slice ss);

/* Gives back what syscall_slice_ptr_from_strings or syscall_string_slice_ptr
 * made from a. A nil slice is fine. */
void syscall_slice_ptr_free(Alloc *a, Slice bb);

/* syscall.CloseOnExec: mark fd to be closed in a child exec starts, ignoring
 * any error, as Go does. */
void syscall_close_on_exec(Int fd);

/* syscall.SetNonblock: turn O_NONBLOCK on fd on or off. */
BURROW_OWNS(ret) Error syscall_set_nonblock(Int fd, bool nonblocking);

/* syscall.Mmap: map length bytes of fd from offset, with the system's PROT_
 * and MAP_ flags, and give them back as a Slice of bytes. A length of 0 or
 * less is EINVAL. syscall_munmap undoes it, and only takes the whole slice
 * Mmap gave, as in Go, so anything else is EINVAL. */
BURROW_OWNS(ret) Slice syscall_mmap(Int fd, int64_t offset, Int length, Int prot,
                                    Int flags, Error *err);
BURROW_OWNS(ret) Error syscall_munmap(Slice b);

/* Iovec.SetLen, Msghdr.SetControllen and Cmsghdr.SetLen, which set a length
 * whose width depends on the system. */
void syscall_iovec_set_len(SyscallIovec *iov, Int length);
void syscall_msghdr_set_controllen(SyscallMsghdr *msghdr, Int length);
void syscall_cmsghdr_set_len(SyscallCmsghdr *cmsg, Int length);
#endif

/* ----------------------------------------------------------------- sockets */

/* syscall.Sockaddr: a socket address, which is one of the Sockaddr types
 * below. Go keeps the set closed with an unexported method, and here only this
 * package makes the vtables, through syscall_sockaddr_inet4_as_sockaddr and
 * the others. sockaddr fills in the system's form of the address in the
 * struct's raw field and gives back a pointer to it, with its length in *len.
 * The zero SyscallSockaddr, with no vt, is Go's nil.
 *
 * To find out which type one is, as a type switch would, compare
 * sa.vt->self_type with the TYPE_SYSCALL_SOCKADDR_ descriptors, or call
 * iface_assert on BURROW_IFACE(sa). */
typedef struct SyscallSockaddrVT {
    const Type *self_type;
    void *(*sockaddr)(void *self, uint32_t *len, Error *err);
} SyscallSockaddrVT;

typedef struct SyscallSockaddr {
    const SyscallSockaddrVT *vt;
    void *data;
} SyscallSockaddr;

/* syscall.SockaddrUnix: the address of a Unix domain socket, a path, or on
 * Linux an abstract name, which starts with @ or a NUL. raw is Go's
 * unexported field, which sockaddr fills in. */
typedef struct SyscallSockaddrUnix {
    Str name;
    SyscallRawSockaddrUnix raw;
} SyscallSockaddrUnix;

#if !defined(BURROW_OS_WINDOWS)
/* syscall.SocketDisableIPv6: when it is true, syscall_socket fails with
 * EAFNOSUPPORT for AF_INET6, for tests. */
extern bool syscall_socket_disable_ipv6;

/* The descriptors of the Sockaddr types, the self_type of the vtables. */
extern const Type *const TYPE_SYSCALL_SOCKADDR_INET4;
extern const Type *const TYPE_SYSCALL_SOCKADDR_INET6;
extern const Type *const TYPE_SYSCALL_SOCKADDR_UNIX;

/* sa as a Sockaddr. The Sockaddr borrows sa, which has to stay where it is for
 * as long as the Sockaddr is used. */
SyscallSockaddr syscall_sockaddr_inet4_as_sockaddr(SyscallSockaddrInet4 *sa);
SyscallSockaddr syscall_sockaddr_inet6_as_sockaddr(SyscallSockaddrInet6 *sa);
SyscallSockaddr syscall_sockaddr_unix_as_sockaddr(SyscallSockaddrUnix *sa);

/* Gives back to a a Sockaddr that syscall_accept, syscall_getsockname,
 * syscall_getpeername, syscall_recvfrom or syscall_recvmsg made from a. Each
 * one is a single allocation, a SockaddrUnix's name included. The zero
 * Sockaddr is fine. */
void syscall_sockaddr_free(Alloc *a, SyscallSockaddr sa);

/* Socket: socket(2). Socketpair: socketpair(2), with the two descriptors in
 * the result's fd. */
Int syscall_socket(Int domain, Int typ, Int proto, Error *err);

typedef struct SyscallSocketpairRet {
    Int fd[2];
} SyscallSocketpairRet;
SyscallSocketpairRet syscall_socketpair(Int domain, Int typ, Int proto, Error *err);

/* Bind and Connect: bind(2) and connect(2) to sa. An address sa cannot spell,
 * such as a port above 65535 or a path too long for the system, is EINVAL. */
BURROW_OWNS(ret) Error syscall_bind(Int fd, SyscallSockaddr sa);
BURROW_OWNS(ret) Error syscall_connect(Int fd, SyscallSockaddr sa);

/* Accept: accept(2), with the peer's address made from a in *sa. A family
 * this package has no Sockaddr for is EAFNOSUPPORT, and then the new
 * descriptor is closed and 0 comes back, as in Go. On Linux it is
 * Accept4 with no flags. */
Int syscall_accept(Alloc *a, Int fd, SyscallSockaddr *sa, Error *err);

/* Getsockname and Getpeername: the socket's own address and its peer's, made
 * from a. */
BURROW_OWNS(ret) SyscallSockaddr syscall_getsockname(Alloc *a, Int fd, Error *err);
BURROW_OWNS(ret) SyscallSockaddr syscall_getpeername(Alloc *a, Int fd, Error *err);

/* Recvfrom: recvfrom(2) into p. *from is where the data came from, made from
 * a, or the zero Sockaddr when the system did not say, as on a connected
 * socket. from may be NULL. */
Int syscall_recvfrom(Alloc *a, Int fd, Slice p, Int flags, SyscallSockaddr *from,
                     Error *err);

/* Recvmsg: recvmsg(2) into p, with the control messages in oob. It gives back
 * how many bytes of p it filled, and in *oobn how many of oob, in *recvflags
 * the message's flags, and in *from the sender, as Recvfrom does. With oob and
 * no p, it asks for one byte, as Go does, except of a datagram socket on Linux. */
Int syscall_recvmsg(Alloc *a, Int fd, Slice p, Slice oob, Int flags, Int *oobn,
                    Int *recvflags, SyscallSockaddr *from, Error *err);

/* Sendto: sendto(2) of p to to, or with no address when to is the zero
 * Sockaddr. */
BURROW_OWNS(ret) Error syscall_sendto(Int fd, Slice p, Int flags, SyscallSockaddr to);

/* SendmsgN: sendmsg(2) of p with the control messages in oob, to to, or with
 * no address when to is the zero Sockaddr. It gives back how many bytes of p
 * went, which is 0 when p is empty and oob is not, since one byte is sent then
 * to carry oob. Sendmsg is the same without the count. */
Int syscall_sendmsg_n(Int fd, Slice p, Slice oob, SyscallSockaddr to, Int flags,
                      Error *err);
BURROW_OWNS(ret) Error syscall_sendmsg(Int fd, Slice p, Slice oob, SyscallSockaddr to,
                                       Int flags);

/* The Getsockopt functions: getsockopt(2) of a value of each type. Go returns
 * a pointer to the structs, and here they come back by value. */
Int syscall_getsockopt_int(Int fd, Int level, Int opt, Error *err);

typedef struct SyscallGetsockoptInet4AddrRet {
    uint8_t value[4];
} SyscallGetsockoptInet4AddrRet;
SyscallGetsockoptInet4AddrRet syscall_getsockopt_inet4_addr(Int fd, Int level, Int opt,
                                                            Error *err);

SyscallIPMreq syscall_getsockopt_ip_mreq(Int fd, Int level, Int opt, Error *err);
SyscallIPv6Mreq syscall_getsockopt_ipv6_mreq(Int fd, Int level, Int opt, Error *err);
SyscallIPv6MTUInfo syscall_getsockopt_ipv6_mtu_info(Int fd, Int level, Int opt,
                                                    Error *err);
SyscallICMPv6Filter syscall_getsockopt_icmpv6_filter(Int fd, Int level, Int opt,
                                                     Error *err);

/* The Setsockopt functions: setsockopt(2) of a value of each type. An Int
 * goes as 32 bits and a Str as its bytes. */
BURROW_OWNS(ret) Error syscall_setsockopt_byte(Int fd, Int level, Int opt,
                                               uint8_t value);
BURROW_OWNS(ret) Error syscall_setsockopt_int(Int fd, Int level, Int opt, Int value);
BURROW_OWNS(ret) Error syscall_setsockopt_inet4_addr(Int fd, Int level, Int opt,
                                                     const uint8_t value[4]);
BURROW_OWNS(ret) Error syscall_setsockopt_ip_mreq(Int fd, Int level, Int opt,
                                                  SyscallIPMreq *mreq);
BURROW_OWNS(ret) Error syscall_setsockopt_ipv6_mreq(Int fd, Int level, Int opt,
                                                    SyscallIPv6Mreq *mreq);
BURROW_OWNS(ret) Error syscall_setsockopt_icmpv6_filter(Int fd, Int level, Int opt,
                                                        SyscallICMPv6Filter *filter);
BURROW_OWNS(ret) Error syscall_setsockopt_linger(Int fd, Int level, Int opt,
                                                 SyscallLinger *l);
BURROW_OWNS(ret) Error syscall_setsockopt_string(Int fd, Int level, Int opt, Str s);
BURROW_OWNS(ret) Error syscall_setsockopt_timeval(Int fd, Int level, Int opt,
                                                  SyscallTimeval *tv);

/* ----------------------------------------------------- control messages */

/* syscall.SocketControlMessage: one control message from
 * syscall_parse_socket_control_message. data borrows the bytes it was parsed
 * from. */
typedef struct SyscallSocketControlMessage {
    SyscallCmsghdr header;
    Slice data;
} SyscallSocketControlMessage;

extern const Type *const TYPE_SYSCALL_SOCKET_CONTROL_MESSAGE;

/* CmsgLen: the length a control message's header says it is, for datalen
 * bytes of data. CmsgSpace: the room it takes in a buffer, padding included. */
Int syscall_cmsg_len(Int datalen);
Int syscall_cmsg_space(Int datalen);

/* ParseSocketControlMessage: the control messages in b, in a Slice of
 * SyscallSocketControlMessage from a. A header whose length runs past b is
 * EINVAL. */
BURROW_OWNS(ret) Slice syscall_parse_socket_control_message(Alloc *a, Slice b,
                                                            Error *err);

/* UnixRights: a control message that passes the descriptors in fds, a Slice of
 * Int, to another process, in bytes from a. A failed allocation gives a nil
 * slice. ParseUnixRights: the descriptors in an SCM_RIGHTS message, as a Slice
 * of Int from a. Any other message is EINVAL. */
BURROW_OWNS(ret) Slice syscall_unix_rights(Alloc *a, Slice fds);
BURROW_OWNS(ret) Slice syscall_parse_unix_rights(Alloc *a,
                                                 SyscallSocketControlMessage *m,
                                                 Error *err);
#endif

#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) || defined(BURROW_OS_WASI) || \
    defined(BURROW_OS_FREEBSD)
/* Accept4: accept4(2), which takes SOCK_NONBLOCK and SOCK_CLOEXEC in flags,
 * and is otherwise Accept. */
Int syscall_accept4(Alloc *a, Int fd, Int flags, SyscallSockaddr *sa, Error *err);

/* GetsockoptIPMreqn and SetsockoptIPMreqn. */
SyscallIPMreqn syscall_getsockopt_ip_mreqn(Int fd, Int level, Int opt, Error *err);
BURROW_OWNS(ret) Error syscall_setsockopt_ip_mreqn(Int fd, Int level, Int opt,
                                                   SyscallIPMreqn *mreq);
#endif

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || defined(BURROW_OS_FREEBSD)
/* The descriptor of SockaddrDatalink, a link layer address on the BSDs, and
 * one as a Sockaddr, which borrows sa. */
extern const Type *const TYPE_SYSCALL_SOCKADDR_DATALINK;
SyscallSockaddr syscall_sockaddr_datalink_as_sockaddr(SyscallSockaddrDatalink *sa);

/* GetsockoptByte: getsockopt(2) of one byte. */
uint8_t syscall_getsockopt_byte(Int fd, Int level, Int opt, Error *err);
#endif

#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) || defined(BURROW_OS_WASI)
/* The descriptors of SockaddrLinklayer, an AF_PACKET address, and
 * SockaddrNetlink, and either as a Sockaddr, which borrows sa. */
extern const Type *const TYPE_SYSCALL_SOCKADDR_LINKLAYER;
extern const Type *const TYPE_SYSCALL_SOCKADDR_NETLINK;
SyscallSockaddr syscall_sockaddr_linklayer_as_sockaddr(SyscallSockaddrLinklayer *sa);
SyscallSockaddr syscall_sockaddr_netlink_as_sockaddr(SyscallSockaddrNetlink *sa);

/* GetsockoptUcred: the credentials of the peer of a Unix domain socket, with
 * SO_PEERCRED. */
SyscallUcred syscall_getsockopt_ucred(Int fd, Int level, Int opt, Error *err);

/* BindToDevice: SO_BINDTODEVICE, which ties the socket to the network
 * interface called device. */
BURROW_OWNS(ret) Error syscall_bind_to_device(Int fd, Str device);

/* UnixCredentials: an SCM_CREDENTIALS control message carrying ucred, in
 * bytes from a. ParseUnixCredentials: the Ucred in one, which the receiving
 * socket has to have SO_PASSCRED set to get. Any other message is EINVAL. */
BURROW_OWNS(ret) Slice syscall_unix_credentials(Alloc *a, SyscallUcred *ucred);
SyscallUcred syscall_parse_unix_credentials(SyscallSocketControlMessage *m, Error *err);

/* syscall.NetlinkMessage and NetlinkRouteAttr: a netlink message and a route
 * attribute in one. data and value borrow the bytes they were parsed from. */
typedef struct SyscallNetlinkMessage {
    SyscallNlMsghdr header;
    Slice data;
} SyscallNetlinkMessage;

typedef struct SyscallNetlinkRouteAttr {
    SyscallRtAttr attr;
    Slice value;
} SyscallNetlinkRouteAttr;

extern const Type *const TYPE_SYSCALL_NETLINK_MESSAGE;
extern const Type *const TYPE_SYSCALL_NETLINK_ROUTE_ATTR;

/* NetlinkRIB: what the kernel says about its links, addresses or routes, all
 * of the replies to one NETLINK_ROUTE dump request for proto, such as
 * RTM_GETLINK, and family, in bytes from a. */
BURROW_OWNS(ret) Slice syscall_netlink_rib(Alloc *a, Int proto, Int family, Error *err);

/* ParseNetlinkMessage: the netlink messages in b, as a Slice of
 * SyscallNetlinkMessage from a. ParseNetlinkRouteAttr: the route attributes
 * in m, a link, address or route message, as a Slice of
 * SyscallNetlinkRouteAttr. A length that runs past the bytes is EINVAL, and so
 * is a message of another type. */
BURROW_OWNS(ret) Slice syscall_parse_netlink_message(Alloc *a, Slice b, Error *err);
BURROW_OWNS(ret) Slice syscall_parse_netlink_route_attr(Alloc *a,
                                                        SyscallNetlinkMessage *m,
                                                        Error *err);

/* The Linux socket filter functions, which Go has deprecated in favour of
 * golang.org/x/net/bpf. LsfStmt and LsfJump build one instruction, which comes
 * back by value where Go returns a pointer. LsfSocket opens an AF_PACKET
 * socket for proto bound to the interface ifindex. SetLsfPromisc turns
 * promiscuous mode on the interface called name on or off. AttachLsf and
 * DetachLsf put a program, a Slice of SyscallSockFilter, on a socket and take
 * it off. */
SyscallSockFilter syscall_lsf_stmt(Int code, Int k);
SyscallSockFilter syscall_lsf_jump(Int code, Int k, Int jt, Int jf);
Int syscall_lsf_socket(Int ifindex, Int proto, Error *err);
BURROW_OWNS(ret) Error syscall_set_lsf_promisc(Str name, bool m);
BURROW_OWNS(ret) Error syscall_attach_lsf(Int fd, Slice i);
BURROW_OWNS(ret) Error syscall_detach_lsf(Int fd);
#endif

#if (defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) ||                           \
     defined(BURROW_OS_WASI)) &&                                                       \
    (defined(BURROW_ARCH_386) || defined(BURROW_ARCH_S390X))
/* Listen and Shutdown, which these two have through socketcall(2) and the
 * others have generated. */
BURROW_OWNS(ret) Error syscall_listen(Int s, Int n);
BURROW_OWNS(ret) Error syscall_shutdown(Int s, Int how);
#endif

#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_COSMO) || defined(BURROW_OS_WASI)
/* ------------------------------------------------------------------- Linux */

/* The path calls, each the at form with AT_FDCWD as Go does it. Open and
 * Openat add O_LARGEFILE, which is 0 on 64-bit systems. Creat is Open with
 * O_CREAT, O_WRONLY and O_TRUNC, Rmdir is unlinkat with AT_REMOVEDIR, and
 * Mkfifo is Mknod with S_IFIFO. */
BURROW_OWNS(ret) Error syscall_access(Str path, uint32_t mode);
BURROW_OWNS(ret) Error syscall_chmod(Str path, uint32_t mode);
BURROW_OWNS(ret) Error syscall_chown(Str path, Int uid, Int gid);
Int syscall_creat(Str path, uint32_t mode, Error *err);
BURROW_OWNS(ret) Error syscall_link(Str oldpath, Str newpath);
BURROW_OWNS(ret) Error syscall_mkdir(Str path, uint32_t mode);
BURROW_OWNS(ret) Error syscall_mknod(Str path, uint32_t mode, Int dev);
BURROW_OWNS(ret) Error syscall_mkfifo(Str path, uint32_t mode);
Int syscall_open(Str path, Int mode, uint32_t perm, Error *err);
Int syscall_openat(Int dirfd, Str path, Int flags, uint32_t mode, Error *err);
Int syscall_readlink(Str path, Slice buf, Error *err);
BURROW_OWNS(ret) Error syscall_rename(Str oldpath, Str newpath);
BURROW_OWNS(ret) Error syscall_rmdir(Str path);
BURROW_OWNS(ret) Error syscall_symlink(Str oldpath, Str newpath);
BURROW_OWNS(ret) Error syscall_unlink(Str path);
BURROW_OWNS(ret) Error syscall_unlinkat(Int dirfd, Str path);

/* Faccessat: faccessat2 when there are flags, and when the kernel does not
 * have it, the checks the C library makes, from the file's mode, owner and
 * group, as Go does. Only AT_SYMLINK_NOFOLLOW and AT_EACCESS are known. */
BURROW_OWNS(ret) Error syscall_faccessat(Int dirfd, Str path, uint32_t mode, Int flags);

/* Fchmodat: fchmodat2 when there are flags. A kernel without it gives
 * EOPNOTSUPP for AT_SYMLINK_NOFOLLOW and AT_EMPTY_PATH, and EINVAL for any
 * other flag. */
BURROW_OWNS(ret) Error syscall_fchmodat(Int dirfd, Str path, uint32_t mode, Int flags);

/* EpollCreate: EpollCreate1(0), and EINVAL for a size of 0 or less. */
Int syscall_epoll_create(Int size, Error *err);

/* Pipe and Pipe2: the two ends in p, a Slice of Int that has to have a
 * length of 2, or it is EINVAL. */
BURROW_OWNS(ret) Error syscall_pipe(Slice p);
BURROW_OWNS(ret) Error syscall_pipe2(Slice p, Int flags);

/* Utimes, Futimesat and Futimes take a Slice of two SyscallTimeval, and
 * UtimesNano two SyscallTimespec, the access time and then the modification
 * time. Any other length is EINVAL. Futimes goes through /proc/self/fd, as the
 * C library does. */
BURROW_OWNS(ret) Error syscall_utimes(Str path, Slice tv);
BURROW_OWNS(ret) Error syscall_utimes_nano(Str path, Slice ts);
BURROW_OWNS(ret) Error syscall_futimesat(Int dirfd, Str path, Slice tv);
BURROW_OWNS(ret) Error syscall_futimes(Int fd, Slice tv);

/* Getwd: the working directory, from a. A path that is not absolute, which
 * Linux gives as "(unreachable)" and the rest, is ENOENT. */
BURROW_OWNS(ret) Str syscall_getwd(Alloc *a, Error *err);

/* Getgroups: the supplementary group ids, a Slice of Int from a, nil if there
 * are none. */
BURROW_OWNS(ret) Slice syscall_getgroups(Alloc *a, Error *err);

/* Mount: mount(2), with data passed as NULL when it is empty. */
BURROW_OWNS(ret) Error syscall_mount(Str source, Str target, Str fstype, Uintptr flags,
                                     Str data);

/* Reboot: reboot(2) with the two magic numbers. cmd is a LINUX_REBOOT_CMD_
 * constant. */
BURROW_OWNS(ret) Error syscall_reboot(Int cmd);

/* Getpgrp: Getpgid(0). */
Int syscall_getpgrp(void);

/* Getrlimit and Setrlimit, through prlimit64 on this process. */
BURROW_OWNS(ret) Error syscall_getrlimit(Int resource, SyscallRlimit *rlim);
BURROW_OWNS(ret) Error syscall_setrlimit(Int resource, SyscallRlimit *rlim);

/* Sendfile: sendfile(2), from offset when it is not NULL. */
Int syscall_sendfile(Int outfd, Int infd, int64_t *offset, Int count, Error *err);

/* FcntlFlock: fcntl with F_GETLK, F_SETLK or F_SETLKW, and fcntl64 on 32-bit
 * systems. */
BURROW_OWNS(ret) Error syscall_fcntl_flock(Uintptr fd, Int cmd, SyscallFlock_t *lk);

/* ReadDirent: Getdents. ParseDirent: the names in the dirents at the start of
 * buf, up to max of them, or all of them when max is -1, leaving out . and
 * .., appended to names from a and given back in *newnames, with how many in
 * *count. It returns how many bytes of buf it used. */
Int syscall_read_dirent(Int fd, Slice buf, Error *err);
Int syscall_parse_dirent(Alloc *a, Slice buf, Int max, Slice names, Int *count,
                         Slice *newnames);

/* The ptrace helpers. Peek and Poke read and write the tracee's memory a word
 * at a time, so addr need not be aligned, and give back how many bytes they
 * did. GetRegs and SetRegs use PTRACE_GETREGSET and PTRACE_SETREGSET. */
Int syscall_ptrace_peek_text(Int pid, Uintptr addr, Slice out, Error *err);
Int syscall_ptrace_peek_data(Int pid, Uintptr addr, Slice out, Error *err);
Int syscall_ptrace_poke_text(Int pid, Uintptr addr, Slice data, Error *err);
Int syscall_ptrace_poke_data(Int pid, Uintptr addr, Slice data, Error *err);
BURROW_OWNS(ret) Error syscall_ptrace_get_regs(Int pid, SyscallPtraceRegs *regsout);
BURROW_OWNS(ret) Error syscall_ptrace_set_regs(Int pid, SyscallPtraceRegs *regs);
BURROW_OWNS(ret) Error syscall_ptrace_set_options(Int pid, Int options);
Uint syscall_ptrace_get_event_msg(Int pid, Error *err);
BURROW_OWNS(ret) Error syscall_ptrace_cont(Int pid, Int signal);
BURROW_OWNS(ret) Error syscall_ptrace_syscall(Int pid, Int signal);
BURROW_OWNS(ret) Error syscall_ptrace_single_step(Int pid);
BURROW_OWNS(ret) Error syscall_ptrace_attach(Int pid);
BURROW_OWNS(ret) Error syscall_ptrace_detach(Int pid);

#if defined(BURROW_ARCH_LOONG64)
/* PtraceRegs.GetEra and SetEra, which loong64 has in place of PC. */
uint64_t syscall_ptrace_regs_get_era(const SyscallPtraceRegs *r);
void syscall_ptrace_regs_set_era(SyscallPtraceRegs *r, uint64_t era);
#else
/* PtraceRegs.PC and SetPC: the program counter, whatever the architecture
 * calls it. */
uint64_t syscall_ptrace_regs_pc(const SyscallPtraceRegs *r);
void syscall_ptrace_regs_set_pc(SyscallPtraceRegs *r, uint64_t pc);
#endif

/* The ones Go writes by hand on some architectures and generates on the rest,
 * so syscall/zsyscall.h has them where they are generated. */
#if defined(BURROW_ARCH_AMD64)
BURROW_OWNS(ret) Error syscall_gettimeofday(SyscallTimeval *tv);
#endif
#if !defined(BURROW_ARCH_386) && !defined(BURROW_ARCH_ARM) &&                          \
    !defined(BURROW_ARCH_PPC64)
/* Time: the seconds Gettimeofday gives, also in *t when it is not NULL. */
SyscallTime_t syscall_time(SyscallTime_t *t, Error *err);
#endif
#if !defined(BURROW_ARCH_PPC64) && !defined(BURROW_ARCH_S390X) &&                      \
    !defined(BURROW_ARCH_LOONG64) && !defined(BURROW_ARCH_MIPS64)
BURROW_OWNS(ret) Error syscall_stat(Str path, SyscallStat_t *stat);
BURROW_OWNS(ret) Error syscall_lstat(Str path, SyscallStat_t *stat);
#endif
#if defined(BURROW_ARCH_ARM64) || defined(BURROW_ARCH_RISCV64)
BURROW_OWNS(ret) Error syscall_fstatat(Int fd, Str path, SyscallStat_t *stat,
                                       Int flags);
#endif
#if !defined(BURROW_ARCH_MIPS64) && !defined(BURROW_ARCH_PPC64) &&                     \
    !defined(BURROW_ARCH_S390X)
BURROW_OWNS(ret) Error syscall_lchown(Str path, Int uid, Int gid);
#endif
#if defined(BURROW_ARCH_RISCV64)
BURROW_OWNS(ret) Error syscall_renameat(Int olddirfd, Str oldpath, Int newdirfd,
                                        Str newpath);
#endif
#if defined(BURROW_ARCH_386) || defined(BURROW_ARCH_ARM)
int64_t syscall_seek(Int fd, int64_t offset, Int whence, Error *err);
BURROW_OWNS(ret) Error syscall_fstatfs(Int fd, SyscallStatfs_t *buf);
BURROW_OWNS(ret) Error syscall_statfs(Str path, SyscallStatfs_t *buf);
#endif
#if defined(BURROW_ARCH_ARM64) || defined(BURROW_ARCH_RISCV64) ||                      \
    defined(BURROW_ARCH_LOONG64) || defined(BURROW_ARCH_MIPS64)
Int syscall_select(Int nfd, SyscallFdSet *r, SyscallFdSet *w, SyscallFdSet *e,
                   SyscallTimeval *timeout, Error *err);
#endif
#if defined(BURROW_ARCH_ARM64) || defined(BURROW_ARCH_RISCV64) ||                      \
    defined(BURROW_ARCH_LOONG64)
BURROW_OWNS(ret) Error syscall_utime(Str path, SyscallUtimbuf *buf);
Int syscall_inotify_init(Error *err);
BURROW_OWNS(ret) Error syscall_pause(void);
#endif
#if defined(BURROW_ARCH_PPC64)
BURROW_OWNS(ret) Error syscall_sync_file_range(Int fd, int64_t off, int64_t n,
                                               Int flags);
#endif
#endif

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS) || defined(BURROW_OS_FREEBSD)
/* --------------------------------------------------------------------- BSD */

/* Getwd: the working directory, from a, up to its first NUL. An empty one is
 * EINVAL. */
BURROW_OWNS(ret) Str syscall_getwd(Alloc *a, Error *err);

/* Getgroups: the supplementary group ids, a Slice of Int from a, nil if there
 * are none. More than 1000 is EINVAL. */
BURROW_OWNS(ret) Slice syscall_getgroups(Alloc *a, Error *err);

/* Pipe: the two ends in p, a Slice of Int that has to have a length of 2, or
 * it is EINVAL. FreeBSD has Pipe2 too, which Pipe is with no flags. */
BURROW_OWNS(ret) Error syscall_pipe(Slice p);
#if defined(BURROW_OS_FREEBSD)
BURROW_OWNS(ret) Error syscall_pipe2(Slice p, Int flags);
#endif

/* Utimes and Futimes take a Slice of two SyscallTimeval, and UtimesNano two
 * SyscallTimespec, the access time and then the modification time. Any other
 * length is EINVAL. UtimesNano uses utimensat, and utimes to the microsecond
 * only when that is ENOSYS. */
BURROW_OWNS(ret) Error syscall_utimes(Str path, Slice tv);
BURROW_OWNS(ret) Error syscall_utimes_nano(Str path, Slice ts);
BURROW_OWNS(ret) Error syscall_futimes(Int fd, Slice tv);

/* Getdirentries: the dirents of directory fd into buf, and how many bytes they
 * took. FreeBSD keeps where it got to in *basep, and gives EIO when that does
 * not fit a 32-bit Uintptr. macOS has no such call for programs, so it is
 * built from readdir_r as in Go, keeps how many entries it has given back as
 * fd's offset, and does not use basep. ReadDirent is Getdirentries with a base
 * of its own. ParseDirent is as on Linux, and leaves out entries whose inode
 * is 0. */
Int syscall_getdirentries(Int fd, Slice buf, Uintptr *basep, Error *err);
Int syscall_read_dirent(Int fd, Slice buf, Error *err);
Int syscall_parse_dirent(Alloc *a, Slice buf, Int max, Slice names, Int *count,
                         Slice *newnames);

/* Kevent: kevent(2) with the changes and events Slices of SyscallKevent_t,
 * either of which may be empty, and NULL timeout to wait for ever. SetKevent
 * fills in what it is for: fd, the filter and the flags. */
Int syscall_kevent(Int kq, Slice changes, Slice events, SyscallTimespec *timeout,
                   Error *err);
void syscall_set_kevent(SyscallKevent_t *k, Int fd, Int mode, Int flags);

/* Sysctl: the value of a sysctl by name, such as "kern.ostype", from a,
 * without its NUL. SysctlUint32 reads one that is four bytes, and anything
 * else is EIO. */
BURROW_OWNS(ret) Str syscall_sysctl(Alloc *a, Str name, Error *err);
uint32_t syscall_sysctl_uint32(Str name, Error *err);

/* Setrlimit: setrlimit(2). */
BURROW_OWNS(ret) Error syscall_setrlimit(Int resource, SyscallRlimit *rlim);

/* FcntlFlock: fcntl with F_GETLK, F_SETLK or F_SETLKW. */
BURROW_OWNS(ret) Error syscall_fcntl_flock(Uintptr fd, Int cmd, SyscallFlock_t *lk);

/* Sendfile: up to count bytes of infd from *offset to the socket outfd, and
 * how many it sent, which can be more than 0 when it fails too. offset may not
 * be NULL, as it may not be in Go, and is not moved. */
Int syscall_sendfile(Int outfd, Int infd, int64_t *offset, Int count, Error *err);

/* Getfsstat: what is mounted, into buf, a Slice of SyscallStatfs_t, and how
 * many there are. An empty buf only counts them. flags is MNT_WAIT or
 * MNT_NOWAIT. */
Int syscall_getfsstat(Slice buf, Int flags, Error *err);

#if defined(BURROW_OS_FREEBSD)
/* Stat and Lstat: Fstatat with AT_FDCWD, and AT_SYMLINK_NOFOLLOW for Lstat.
 * Mknod: mknodat with AT_FDCWD. */
BURROW_OWNS(ret) Error syscall_stat(Str path, SyscallStat_t *st);
BURROW_OWNS(ret) Error syscall_lstat(Str path, SyscallStat_t *st);
BURROW_OWNS(ret) Error syscall_mknod(Str path, uint32_t mode, uint64_t dev);
#else
/* PtraceAttach and PtraceDetach: ptrace(2) with PT_ATTACH and PT_DETACH. */
BURROW_OWNS(ret) Error syscall_ptrace_attach(Int pid);
BURROW_OWNS(ret) Error syscall_ptrace_detach(Int pid);
#endif
#endif

#if defined(BURROW_OS_WINDOWS)
/* ------------------------------------------------------------ Windows DLLs */

/* syscall.Syscall and the others with a number after them, which on Windows
 * call the function at address trap with the first nargs of the arguments, as
 * SyscallN does. Go has deprecated them for SyscallN. */
Uintptr syscall_syscall(Uintptr trap, Uintptr nargs, Uintptr a1, Uintptr a2, Uintptr a3,
                        Uintptr *r2, SyscallErrno *err);
Uintptr syscall_syscall6(Uintptr trap, Uintptr nargs, Uintptr a1, Uintptr a2,
                         Uintptr a3, Uintptr a4, Uintptr a5, Uintptr a6, Uintptr *r2,
                         SyscallErrno *err);
Uintptr syscall_syscall9(Uintptr trap, Uintptr nargs, Uintptr a1, Uintptr a2,
                         Uintptr a3, Uintptr a4, Uintptr a5, Uintptr a6, Uintptr a7,
                         Uintptr a8, Uintptr a9, Uintptr *r2, SyscallErrno *err);
Uintptr syscall_syscall12(Uintptr trap, Uintptr nargs, Uintptr a1, Uintptr a2,
                          Uintptr a3, Uintptr a4, Uintptr a5, Uintptr a6, Uintptr a7,
                          Uintptr a8, Uintptr a9, Uintptr a10, Uintptr a11, Uintptr a12,
                          Uintptr *r2, SyscallErrno *err);
Uintptr syscall_syscall15(Uintptr trap, Uintptr nargs, Uintptr a1, Uintptr a2,
                          Uintptr a3, Uintptr a4, Uintptr a5, Uintptr a6, Uintptr a7,
                          Uintptr a8, Uintptr a9, Uintptr a10, Uintptr a11, Uintptr a12,
                          Uintptr a13, Uintptr a14, Uintptr a15, Uintptr *r2,
                          SyscallErrno *err);
Uintptr syscall_syscall18(Uintptr trap, Uintptr nargs, Uintptr a1, Uintptr a2,
                          Uintptr a3, Uintptr a4, Uintptr a5, Uintptr a6, Uintptr a7,
                          Uintptr a8, Uintptr a9, Uintptr a10, Uintptr a11, Uintptr a12,
                          Uintptr a13, Uintptr a14, Uintptr a15, Uintptr a16,
                          Uintptr a17, Uintptr a18, Uintptr *r2, SyscallErrno *err);

/* syscall.SyscallN: calls the function at p with the Uintptrs in args, at
 * most 42 of them, and returns what it returned. *err is what GetLastError
 * said afterwards, which is only worth looking at when the result says the
 * call failed, since a call that works may leave it set. *r2 is always 0.
 * Go puts the floating point result register there on amd64, which a call
 * through a C function pointer can't read. Either may be NULL.
 * More than 42 arguments is a panic, as in Go. */
Uintptr syscall_syscall_n(Uintptr p, Slice args, Uintptr *r2, SyscallErrno *err);

/* syscall.DLLError: a DLL that would not load or a procedure it does not
 * have. err is the Errno, obj_name the name of the DLL or procedure, and msg
 * the whole message. errors_as with TYPE_SYSCALL_DLL_ERROR finds it. */
typedef struct SyscallDLLError {
    Error err;
    Str obj_name;
    Str msg;
} SyscallDLLError;

extern const Type *const TYPE_SYSCALL_DLL_ERROR;

/* DLLError.Error, which is msg, and DLLError.Unwrap, which is err. */
BURROW_BORROWS(ret, e) Str syscall_dll_error_error(const SyscallDLLError *e);
BURROW_BORROWS(ret, e) Error syscall_dll_error_unwrap(const SyscallDLLError *e);

/* syscall.DLL: a loaded DLL. */
typedef struct SyscallDLL {
    Str name;
    SyscallHandle handle;
} SyscallDLL;

/* syscall.Proc: a procedure in a DLL. addr is unexported in Go, and is what
 * syscall_proc_addr returns. */
typedef struct SyscallProc {
    SyscallDLL *dll;
    Str name;
    Uintptr addr;
} SyscallProc;

/* syscall.LoadDLL: loads the DLL called name. One of the DLLs syscall itself
 * uses, such as kernel32.dll, is only looked for in the system directory, as
 * in Go, and any other the way LoadLibrary looks. The DLL comes from a, with
 * its name copied, and syscall_dll_free gives it back. A failure is a
 * SyscallDLLError from error_allocator, or EINVAL if name has a NUL in it. */
BURROW_OWNS(ret) SyscallDLL *syscall_load_dll(Alloc *a, Str name, Error *err);

/* syscall.MustLoadDLL: syscall_load_dll, which panics with the error if the
 * DLL will not load. */
BURROW_OWNS(ret) SyscallDLL *syscall_must_load_dll(Alloc *a, Str name);

/* DLL.FindProc: the procedure called name, from a, which syscall_proc_free
 * gives back. A failure is a SyscallDLLError, or EINVAL if name has a NUL in
 * it. */
BURROW_OWNS(ret) SyscallProc *syscall_dll_find_proc(SyscallDLL *d, Alloc *a, Str name,
                                                    Error *err);

/* DLL.MustFindProc: syscall_dll_find_proc, which panics if it fails. */
BURROW_OWNS(ret) SyscallProc *syscall_dll_must_find_proc(SyscallDLL *d, Alloc *a,
                                                         Str name);

/* DLL.Release: FreeLibrary on the handle. d itself stays until
 * syscall_dll_free. */
BURROW_OWNS(ret) Error syscall_dll_release(SyscallDLL *d);

/* Gives back what syscall_load_dll and syscall_dll_find_proc took from a,
 * without releasing the DLL. Either may be NULL. */
void syscall_dll_free(Alloc *a, SyscallDLL *d);
void syscall_proc_free(Alloc *a, SyscallProc *p);

/* Proc.Addr: the address of the procedure. */
Uintptr syscall_proc_addr(const SyscallProc *p);

/* Proc.Call: syscall_syscall_n on the procedure. The error is never nil, as
 * in Go: it is the Errno GetLastError gave, from error_allocator, and only
 * means something when the result says the call failed. */
Uintptr syscall_proc_call(const SyscallProc *p, Slice args, Uintptr *r2, Error *err);

/* syscall.LazyDLL: a DLL loaded the first time something needs it. A zero
 * LazyDLL with name set is ready to use, so one can be a static:
 *
 *     static SyscallLazyDLL kernel32 = {.name = {(const Byte *)"kernel32.dll", 12}};
 *
 * dll is set once it is loaded, and comes from the heap allocator, since it
 * lives as long as the LazyDLL does. */
typedef struct SyscallLazyDLL {
    SyncMutex mu;
    SyscallDLL *dll;
    Str name;
} SyscallLazyDLL;

/* syscall.LazyProc: a procedure in a LazyDLL, looked up the first time it is
 * needed, the same way. */
typedef struct SyscallLazyProc {
    SyncMutex mu;
    Str name;
    SyscallLazyDLL *l;
    SyscallProc *proc;
} SyscallLazyProc;

/* syscall.NewLazyDLL: a LazyDLL for name, from a, which nothing gives back
 * while the DLL is in use. name is borrowed, and has to last as long. */
BURROW_OWNS(ret) SyscallLazyDLL *syscall_new_lazy_dll(Alloc *a, Str name);

/* LazyDLL.Load: loads the DLL if it is not loaded yet. Safe to call from any
 * thread. */
BURROW_OWNS(ret) Error syscall_lazy_dll_load(SyscallLazyDLL *d);

/* LazyDLL.Handle: the DLL's handle, loading it first, and a panic if it will
 * not load. */
Uintptr syscall_lazy_dll_handle(SyscallLazyDLL *d);

/* LazyDLL.NewProc: a LazyProc for name in d, from a. name is borrowed. */
BURROW_OWNS(ret) SyscallLazyProc *syscall_lazy_dll_new_proc(SyscallLazyDLL *d, Alloc *a,
                                                            Str name);

/* LazyProc.Find: looks the procedure up if that has not been done yet,
 * loading the DLL first. */
BURROW_OWNS(ret) Error syscall_lazy_proc_find(SyscallLazyProc *p);

/* LazyProc.Addr: the procedure's address, and a panic if it cannot be
 * found. */
Uintptr syscall_lazy_proc_addr(SyscallLazyProc *p);

/* LazyProc.Call: syscall_proc_call, after finding the procedure, and a panic
 * if it cannot be found. */
Uintptr syscall_lazy_proc_call(SyscallLazyProc *p, Slice args, Uintptr *r2, Error *err);

/* ---------------------------------------------------------------- UTF-16 */

/* syscall.UTF16FromString: s as UTF-16, from a, with a 0 after it, as a Slice
 * of uint16_t whose length counts the 0. An unpaired surrogate written in
 * WTF-8 comes back as itself, which is how Windows can be given a name it
 * gave out. EINVAL if s has a NUL in it. */
BURROW_OWNS(ret) Slice syscall_utf16_from_string(Alloc *a, Str s, Error *err);

/* syscall.UTF16PtrFromString: the same, as a pointer to the first unit. */
BURROW_OWNS(ret) uint16_t *syscall_utf16_ptr_from_string(Alloc *a, Str s, Error *err);

/* syscall.StringToUTF16 and StringToUTF16Ptr, which panic where the others
 * give EINVAL. Go has deprecated them. */
BURROW_OWNS(ret) Slice syscall_string_to_utf16(Alloc *a, Str s);
BURROW_OWNS(ret) uint16_t *syscall_string_to_utf16_ptr(Alloc *a, Str s);

/* syscall.UTF16ToString: the uint16_t units in s as UTF-8, from a, up to the
 * first 0 if there is one. An unpaired surrogate becomes its WTF-8 bytes
 * rather than U+FFFD, so it goes back to Windows as it came. */
BURROW_OWNS(ret) Str syscall_utf16_to_string(Alloc *a, Slice s);
#endif

/* The rest of the system calls, one function each. */
#include "burrow/syscall/zsyscall.h"

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SYSCALL_H */
