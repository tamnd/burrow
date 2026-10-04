/* syscall's functions, the ones generated from Go's zsyscall files and the raw
 * calls under them.
 *
 * Go tests these through os and the rest of the library rather than one by
 * one, so these are burrow's own. They make calls whose answer the C library
 * also gives, and check the two agree, and they check the errors come back as
 * the Errno Go would give.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/syscall.h"

#if !defined(BURROW_OS_WINDOWS) && !defined(BURROW_OS_WASI) && !defined(BURROW_OS_COSMO)
#include <unistd.h>
#if defined(BURROW_OS_LINUX)
#include <sys/utsname.h>
#endif
#define HAVE_CALLS 1
#endif

#if defined(HAVE_CALLS)

/* The Errno err holds, or 0 if it holds none. */
static SyscallErrno errno_of(Error err) {
    const SyscallErrno *e = errors_as(err, TYPE_SYSCALL_ERRNO);
    return e == NULL ? 0 : *e;
}

static void TestGetpid(TestingT *t) {
    Int pid = syscall_getpid();
    if (pid != (Int)getpid())
        testing_t_errorf_v(t, "Getpid() = %d, want %d", pid, (Int)getpid());
    Int ppid = syscall_getppid();
    if (ppid != (Int)getppid())
        testing_t_errorf_v(t, "Getppid() = %d, want %d", ppid, (Int)getppid());
    Int uid = syscall_getuid();
    if (uid != (Int)getuid())
        testing_t_errorf_v(t, "Getuid() = %d, want %d", uid, (Int)getuid());
}

#if !defined(BURROW_OS_DARWIN) && !defined(BURROW_OS_IOS)
/* syscall.Syscall by number. macOS has deprecated syscall(2), so it is left
 * out there. */
static void TestSyscall(TestingT *t) {
    SyscallErrno e = 1;
    Uintptr r2 = 1;
    Uintptr pid = syscall_syscall(SYSCALL_SYS_GETPID, 0, 0, 0, &r2, &e);
    if (e != 0 || (Int)pid != (Int)getpid() || r2 != 0)
        testing_t_errorf_v(t, "Syscall(SYS_GETPID) = %d, %d, %d, want %d, 0, 0",
                           (Int)pid, (Int)r2, (Int)e, (Int)getpid());
    Uintptr r = syscall_syscall(SYSCALL_SYS_CLOSE, (Uintptr)-1, 0, 0, NULL, &e);
    if (e != SYSCALL_EBADF || r != ~(Uintptr)0)
        testing_t_errorf_v(t, "Syscall(SYS_CLOSE, -1) = %d, %d, want -1, EBADF", (Int)r,
                           (Int)e);
}
#endif

static void TestDupClose(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Int fd = syscall_dup(1, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "Dup(1): %v", err);
    }
    if (fd <= 2)
        testing_t_errorf_v(t, "Dup(1) = %d, want more than 2", fd);
    err = syscall_close(fd);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Close(%d): %v", fd, err);
    err = syscall_close(fd);
    if (errno_of(err) != SYSCALL_EBADF)
        testing_t_errorf_v(t, "Close(%d) again = %v, want EBADF", fd, err);
}

/* A path goes to the system as a C string, on the stack when it is short and
 * on the heap when it is not, and one with a NUL in it is EINVAL. */
static void TestPaths(TestingT *t) {
    Error err = syscall_chdir(BURROW_S("/nonexistent-burrow-dir"));
    if (errno_of(err) != SYSCALL_ENOENT)
        testing_t_errorf_v(t, "Chdir(missing) = %v, want ENOENT", err);

    char long_path[600];
    size_t n = 0;
    memcpy(long_path, "/nonexistent-burrow-dir", 23);
    n = 23;
    while (n + 10 < sizeof long_path) {
        memcpy(long_path + n, "/abcdefghi", 10);
        n += 10;
    }
    err = syscall_chdir(str_from_bytes((const Byte *)long_path, (Int)n));
    if (errno_of(err) != SYSCALL_ENOENT)
        testing_t_errorf_v(t, "Chdir(long missing) = %v, want ENOENT", err);

    static const Byte nul[] = {'/', 't', 0, 'p'};
    err = syscall_chdir(str_from_bytes(nul, 4));
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "Chdir with a NUL = %v, want EINVAL", err);

    err = syscall_chdir(BURROW_S("/"));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Chdir(/): %v", err);
}

#if defined(BURROW_OS_LINUX)
/* A function that fills in a struct, and one that takes a []byte. */
static void TestUnameGetcwd(TestingT *t) {
    SyscallUtsname u;
    memset(&u, 0, sizeof u);
    Error err = syscall_uname(&u);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Uname: %v", err);
    struct utsname want;
    uname(&want);
    if (memcmp(u.sysname, want.sysname, strlen(want.sysname) + 1) != 0)
        testing_t_errorf_v(t, "Uname sysname = %q, want %q",
                           str_from_cstr((const char *)u.sysname),
                           str_from_cstr(want.sysname));

    char cwd[4096];
    if (getcwd(cwd, sizeof cwd) == NULL)
        testing_t_fatal_v(t, "getcwd failed");
    Byte buf[4096];
    Slice b = {buf, sizeof buf, sizeof buf, TYPE_BYTE};
    Int got = syscall_getcwd(b, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Getcwd: %v", err);
    /* The kernel counts the NUL. */
    Int n = (Int)strlen(cwd) + 1;
    if (got != n || memcmp(buf, cwd, (size_t)n) != 0)
        testing_t_errorf_v(t, "Getcwd = %d, want %d", got, n);
}
#endif

static void TestBytePtrFromString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    uint8_t *p = syscall_byte_ptr_from_string(a, BURROW_S("abc"), &err);
    if (BURROW_FAILED(err) || p == NULL || memcmp(p, "abc", 4) != 0)
        testing_t_errorf_v(t, "BytePtrFromString(abc): %v", err);
    Slice b = syscall_byte_slice_from_string(a, BURROW_S("ab"), &err);
    if (BURROW_FAILED(err) || b.len != 3 || memcmp(b.p, "ab", 3) != 0)
        testing_t_errorf_v(t, "ByteSliceFromString(ab) = %d bytes, %v", b.len, err);
    static const Byte nul[] = {'a', 0};
    p = syscall_byte_ptr_from_string(a, str_from_bytes(nul, 2), &err);
    if (p != NULL || errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "BytePtrFromString with a NUL = %v, want EINVAL", err);
    arena_free(&ar);
}

#if defined(BURROW_OS_LINUX)
#define PLATFORM_TESTS(X) X(TestSyscall) X(TestUnameGetcwd)
#elif defined(BURROW_OS_DARWIN) || defined(BURROW_OS_IOS)
#define PLATFORM_TESTS(X)
#else
#define PLATFORM_TESTS(X) X(TestSyscall)
#endif

#define TESTS(X)                                                                       \
    X(TestGetpid)                                                                      \
    X(TestDupClose)                                                                    \
    X(TestPaths)                                                                       \
    X(TestBytePtrFromString)                                                           \
    PLATFORM_TESTS(X)

#else

static void TestNothing(TestingT *t) {
    testing_t_skip_v(t, "no system calls here");
}

#define TESTS(X) X(TestNothing)

#endif

TESTING_MAIN(TESTS)
