/* The parts of syscall Go writes by hand: the environment, the Timespec and
 * Timeval conversions, Mmap, the string slices exec takes, and on Linux the
 * path calls, Getwd, Getgroups, the dirents and the rest.
 *
 * TestEnv, TestGettimeofday, TestMmap, TestFaccessat, TestFchmodat,
 * TestDirent, TestDirentRepeat and TestPrlimitSelf are ported from Go's
 * src/syscall/syscall_test.go, mmap_unix_test.go, syscall_linux_test.go and
 * dirent_test.go. The others are burrow's own, and check against the C
 * library where it has the same call.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/syscall.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(BURROW_OS_WINDOWS)
#include <fcntl.h>
#include <unistd.h>
#endif
#if defined(BURROW_OS_LINUX)
#include <grp.h>
#include <sys/resource.h>
#include <sys/stat.h>
#endif

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

static Str cstr(const char *s) {
    return str_from_bytes((const Byte *)s, (Int)strlen(s));
}

static bool str_is(Str s, const char *want) {
    return str_eq(s, cstr(want));
}

/* The Errno err holds, or 0 if it holds none. */
static SyscallErrno errno_of(Error err) {
    const SyscallErrno *e = errors_as(err, TYPE_SYSCALL_ERRNO);
    return e == NULL ? 0 : *e;
}

/* ------------------------------------------------------------ everywhere */

static void set_get_env(TestingT *t, Alloc *a, const char *key, const char *value) {
    Error err = syscall_setenv(cstr(key), cstr(value));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Setenv failed to set %q: %v", value, err);
    bool found = false;
    Str got = syscall_getenv(a, cstr(key), &found);
    if (!found)
        testing_t_errorf_v(t, "Getenv failed to find %v variable (want value %q)", key,
                           value);
    if (!str_is(got, value))
        testing_t_errorf_v(t, "Getenv(%v) = %q; want %q", key, got, value);
}

static void TestEnv(TestingT *t) {
    ARENA_BEGIN;
    set_get_env(t, a, "TESTENV", "AVALUE");
    /* make sure TESTENV gets set to "", not deleted */
    set_get_env(t, a, "TESTENV", "");

    bool seen = false;
    Slice env = syscall_environ(a);
    for (Int i = 0; i < env.len; i++)
        if (str_is(((Str *)env.p)[i], "TESTENV="))
            seen = true;
    if (!seen)
        testing_t_errorf_v(t, "Environ does not have TESTENV=");

    Error err = syscall_unsetenv(BURROW_S("TESTENV"));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Unsetenv: %v", err);
    bool found = true;
    (void)syscall_getenv(a, BURROW_S("TESTENV"), &found);
    if (found)
        testing_t_errorf_v(t, "Getenv found TESTENV after Unsetenv");

#if defined(BURROW_OS_WINDOWS)
    /* SetEnvironmentVariable says ERROR_INVALID_PARAMETER, and Go passes it on. */
    const SyscallErrno bad_key = 87;
#else
    const SyscallErrno bad_key = SYSCALL_EINVAL;
#endif
    err = syscall_setenv(BURROW_S(""), BURROW_S("x"));
    if (errno_of(err) != bad_key)
        testing_t_errorf_v(t, "Setenv with no key = %v, want errno %d", err,
                           (int)bad_key);
    err = syscall_setenv(BURROW_S("A=B"), BURROW_S("x"));
    if (errno_of(err) != bad_key)
        testing_t_errorf_v(t, "Setenv with = in the key = %v, want errno %d", err,
                           (int)bad_key);
    ARENA_END;
}

static void TestTimeConversions(TestingT *t) {
    SyscallTimespec ts = syscall_nsec_to_timespec(1500000001);
    CHECK((int64_t)ts.sec == 1 && (int64_t)ts.nsec == 500000001);
    CHECK(syscall_timespec_nano(&ts) == 1500000001);
    CHECK(syscall_timespec_to_nsec(ts) == 1500000001);
    int64_t nsec = 0;
    CHECK(syscall_timespec_unix(&ts, &nsec) == 1 && nsec == 500000001);

    SyscallTimeval tv;
    memset(&tv, 0, sizeof tv);
    tv.sec = 2;
    tv.usec = 250;
    CHECK(syscall_timeval_nano(&tv) == 2000250000);
    CHECK(syscall_timeval_unix(&tv, &nsec) == 2 && nsec == 250000);

#if defined(BURROW_OS_WINDOWS)
    /* Windows truncates, and leaves a negative time negative in both fields. */
    CHECK(syscall_timeval_nanoseconds(&tv) == 2000250000);
    tv = syscall_nsec_to_timeval(1999);
    CHECK(tv.sec == 0 && tv.usec == 1);
    ts = syscall_nsec_to_timespec(-1);
    CHECK((int64_t)ts.sec == 0 && (int64_t)ts.nsec == -1);
#else
    CHECK(syscall_timeval_to_nsec(tv) == 2000250000);
    /* NsecToTimeval rounds up to the next microsecond. */
    tv = syscall_nsec_to_timeval(1001);
    CHECK((int64_t)tv.sec == 0 && (int64_t)tv.usec == 2);
    tv = syscall_nsec_to_timeval(-1);
    CHECK((int64_t)tv.sec == 0 && (int64_t)tv.usec == 0);
    tv = syscall_nsec_to_timeval(-1000999);
    CHECK((int64_t)tv.sec == -1 && (int64_t)tv.usec == 999000);
    /* and NsecToTimespec keeps Nsec in [0, 1e9). */
    ts = syscall_nsec_to_timespec(-1);
    CHECK((int64_t)ts.sec == -1 && (int64_t)ts.nsec == 999999999);
#endif
}

static void TestGetpagesize(TestingT *t) {
    Int n = syscall_getpagesize();
    if (n < 4096 || (n & (n - 1)) != 0)
        testing_t_errorf_v(t, "Getpagesize() = %d, want a power of two of 4096 or more",
                           n);
#if !defined(BURROW_OS_WINDOWS) && !defined(BURROW_OS_WASI)
    CHECK(n == (Int)sysconf(_SC_PAGESIZE));
#endif
}

static void TestStringByteSlice(TestingT *t) {
    ARENA_BEGIN;
    Slice b = syscall_string_byte_slice(a, BURROW_S("ab"));
    CHECK(b.len == 3 && memcmp(b.p, "ab", 3) == 0);
    uint8_t *p = syscall_string_byte_ptr(a, BURROW_S("xyz"));
    CHECK(p != NULL && strcmp((const char *)p, "xyz") == 0);
    ARENA_END;
}

#if !defined(BURROW_OS_WINDOWS)
/* ------------------------------------------------------------- every Unix */

/* Cosmopolitan has no way to make a raw system call, so everything that goes
 * through one gives ENOSYS there. syscall_call_test.c leaves it out for the
 * same reason. wasip1 has no system calls of its own at all, and no mmap or
 * pipe in its C library, so these give ENOSYS there too. Go builds none of
 * these tests for wasip1, which is not unix to it. */
static void skip_without_raw_calls(TestingT *t) {
#if defined(BURROW_OS_COSMO)
    testing_t_skip_v(t, "Cosmopolitan has no raw system calls");
#elif defined(BURROW_OS_WASI)
    testing_t_skip_v(t, "wasip1 has no mmap and no pipe");
#else
    (void)t;
#endif
}

static void TestMmap(TestingT *t) {
    skip_without_raw_calls(t);
    Error err = BURROW_NO_ERROR;
    Slice b = syscall_mmap(-1, 0, syscall_getpagesize(), SYSCALL_PROT_NONE,
                           SYSCALL_MAP_ANON | SYSCALL_MAP_PRIVATE, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Mmap: %v", err);
    err = syscall_munmap(b);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Munmap: %v", err);
}

/* Munmap only takes back a whole mapping Mmap gave, and only once. */
static void TestMunmapChecks(TestingT *t) {
    skip_without_raw_calls(t);
    Int page = syscall_getpagesize();
    Error err = BURROW_NO_ERROR;
    Slice b = syscall_mmap(-1, 0, 2 * page, SYSCALL_PROT_READ | SYSCALL_PROT_WRITE,
                           SYSCALL_MAP_ANON | SYSCALL_MAP_PRIVATE, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Mmap: %v", err);
    ((uint8_t *)b.p)[2 * page - 1] = 7;

    Slice front = {b.p, page, page, b.elem};
    err = syscall_munmap(front);
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "Munmap of the first page = %v, want EINVAL", err);
    Slice back = {(uint8_t *)b.p + page, page, page, b.elem};
    err = syscall_munmap(back);
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "Munmap of the last page = %v, want EINVAL", err);
    err = syscall_munmap(slice_nil(TYPE_BYTE));
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "Munmap(nil) = %v, want EINVAL", err);

    err = syscall_munmap(b);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Munmap: %v", err);
    err = syscall_munmap(b);
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "second Munmap = %v, want EINVAL", err);

    err = BURROW_NO_ERROR;
    (void)syscall_mmap(-1, 0, 0, SYSCALL_PROT_READ,
                       SYSCALL_MAP_ANON | SYSCALL_MAP_PRIVATE, &err);
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "Mmap of 0 bytes = %v, want EINVAL", err);
}

static void TestSlicePtrFromStrings(TestingT *t) {
    Alloc *a = heap_allocator();
    Str in[] = {BURROW_S("a"), BURROW_S(""), BURROW_S("bc")};
    Slice ss = {in, 3, 3, TYPE_STRING};
    Error err = BURROW_NO_ERROR;
    Slice bb = syscall_slice_ptr_from_strings(a, ss, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SlicePtrFromStrings: %v", err);
    char **p = (char **)bb.p;
    CHECK(bb.len == 4);
    CHECK(strcmp(p[0], "a") == 0 && strcmp(p[1], "") == 0 && strcmp(p[2], "bc") == 0);
    CHECK(p[3] == NULL);
    syscall_slice_ptr_free(a, bb);

    static const Byte nul[] = {'x', 0, 'y'};
    in[1] = str_from_bytes(nul, 3);
    bb = syscall_slice_ptr_from_strings(a, ss, &err);
    if (errno_of(err) != SYSCALL_EINVAL || bb.p != NULL)
        testing_t_errorf_v(t, "SlicePtrFromStrings with a NUL = %v, want EINVAL", err);
}

static void TestSetNonblock(TestingT *t) {
    skip_without_raw_calls(t);
    int fds[2];
    if (pipe(fds) != 0)
        testing_t_fatalf_v(t, "pipe failed");
    Error err = syscall_set_nonblock(fds[0], true);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "SetNonblock(true): %v", err);
    /* Checked through the C library first, so that a SetNonblock that did
     * nothing fails here rather than leaving the read below to wait forever. */
    int fl = fcntl(fds[0], F_GETFL);
    if (fl == -1 || (fl & (int)O_NONBLOCK) == 0)
        testing_t_fatalf_v(t, "flags after SetNonblock(true) = %#x, want O_NONBLOCK",
                           (Int)fl);
    Byte buf[1];
    err = BURROW_NO_ERROR;
    (void)syscall_read(fds[0], (Slice){buf, 1, 1, TYPE_BYTE}, &err);
    if (errno_of(err) != SYSCALL_EAGAIN)
        testing_t_errorf_v(t, "Read of an empty nonblocking pipe = %v, want EAGAIN",
                           err);
    err = syscall_set_nonblock(fds[0], false);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "SetNonblock(false): %v", err);

    err = BURROW_NO_ERROR;
    Byte z[1] = {'z'};
    Int n = syscall_write(fds[1], (Slice){z, 1, 1, TYPE_BYTE}, &err);
    CHECK(n == 1 && !BURROW_FAILED(err));
    n = syscall_read(fds[0], (Slice){buf, 1, 1, TYPE_BYTE}, &err);
    CHECK(n == 1 && buf[0] == 'z');

    syscall_close_on_exec(fds[1]);
    CHECK((fcntl(fds[1], F_GETFD) & FD_CLOEXEC) != 0);
    close(fds[0]);
    close(fds[1]);

    err = syscall_set_nonblock(-1, true);
    if (errno_of(err) != SYSCALL_EBADF)
        testing_t_errorf_v(t, "SetNonblock(-1) = %v, want EBADF", err);
}

static void TestSetLen(TestingT *t) {
    SyscallIovec iov;
    memset(&iov, 0, sizeof iov);
    syscall_iovec_set_len(&iov, 42);
    CHECK((int64_t)iov.len == 42);
    SyscallMsghdr msg;
    memset(&msg, 0, sizeof msg);
    syscall_msghdr_set_controllen(&msg, 24);
    CHECK((int64_t)msg.controllen == 24);
    SyscallCmsghdr cmsg;
    memset(&cmsg, 0, sizeof cmsg);
    syscall_cmsghdr_set_len(&cmsg, 20);
    CHECK((int64_t)cmsg.len == 20);
}

#define UNIX_TESTS(X)                                                                  \
    X(TestMmap)                                                                        \
    X(TestMunmapChecks)                                                                \
    X(TestSlicePtrFromStrings)                                                         \
    X(TestSetNonblock)                                                                 \
    X(TestSetLen)
#else
#define UNIX_TESTS(X)
#endif

#if defined(BURROW_OS_LINUX)
/* ------------------------------------------------------------------ Linux */

#define AT_FDCWD_ (-0x64)
#define AT_SYMLINK_NOFOLLOW_ 0x100
#define AT_EACCESS_ 0x200
#define F_OK_ 0
#define R_OK_ 4

/* A new directory to work in, from a. */
static Str temp_dir(TestingT *t, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    Str d = os_mkdir_temp(a, BURROW_S(""), BURROW_S("burrow-syscall-*"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "MkdirTemp: %v", err);
    return d;
}

/* dir/name, as a C string in buf. */
static const char *join(char *buf, size_t size, Str dir, const char *name) {
    snprintf(buf, size, "%.*s/%s", (int)dir.len, (const char *)dir.p, name);
    return buf;
}

static void touch(TestingT *t, const char *path) {
    Error err = BURROW_NO_ERROR;
    Int fd = syscall_creat(cstr(path), 0644, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Creat(%v): %v", path, err);
    (void)syscall_close(fd);
}

static void TestFaccessat(TestingT *t) {
    ARENA_BEGIN;
    Str d = temp_dir(t, a);
    char file1[512], symlink1[512];
    join(file1, sizeof file1, d, "file1");
    join(symlink1, sizeof symlink1, d, "symlink1");
    touch(t, file1);

    Error err = syscall_faccessat(AT_FDCWD_, cstr(file1), R_OK_, 0);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Faccessat: unexpected error: %v", err);

    err = syscall_faccessat(AT_FDCWD_, cstr(file1), R_OK_, 2);
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "Faccessat: unexpected error: %v, want EINVAL", err);

    err = syscall_faccessat(AT_FDCWD_, cstr(file1), R_OK_, AT_EACCESS_);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Faccessat: unexpected error: %v", err);

    err = syscall_symlink(BURROW_S("file1"), cstr(symlink1));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Symlink: %v", err);

    err = syscall_faccessat(AT_FDCWD_, cstr(symlink1), R_OK_, AT_SYMLINK_NOFOLLOW_);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Faccessat SYMLINK_NOFOLLOW: unexpected error %v", err);

    err = syscall_fchmodat(AT_FDCWD_, cstr(file1), 0, 0);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Fchmodat: unexpected error %v", err);

    err = syscall_faccessat(AT_FDCWD_, cstr(file1), F_OK_, AT_SYMLINK_NOFOLLOW_);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Faccessat: unexpected error: %v", err);

    err = syscall_faccessat(AT_FDCWD_, cstr(file1), R_OK_, AT_SYMLINK_NOFOLLOW_);
    if (errno_of(err) != SYSCALL_EACCES && syscall_getuid() != 0)
        testing_t_errorf_v(t, "Faccessat: unexpected error: %v, want EACCES", err);

    (void)os_remove_all(d);
    ARENA_END;
}

static void TestFchmodat(TestingT *t) {
    ARENA_BEGIN;
    Str d = temp_dir(t, a);
    char file1[512], symlink1[512];
    join(file1, sizeof file1, d, "file1");
    join(symlink1, sizeof symlink1, d, "symlink1");
    touch(t, file1);
    (void)syscall_symlink(BURROW_S("file1"), cstr(symlink1));

    Error err = syscall_fchmodat(AT_FDCWD_, cstr(symlink1), 0444, 0);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Fchmodat: unexpected error: %v", err);

    struct stat st;
    if (stat(file1, &st) != 0)
        testing_t_fatalf_v(t, "stat failed");
    if ((st.st_mode & 0777) != 0444)
        testing_t_errorf_v(t, "Fchmodat: failed to change mode: expected %o, got %o",
                           0444, (Int)(st.st_mode & 0777));

    err = syscall_fchmodat(AT_FDCWD_, cstr(symlink1), 0444, AT_SYMLINK_NOFOLLOW_);
    if (errno_of(err) != SYSCALL_EOPNOTSUPP)
        testing_t_fatalf_v(t, "Fchmodat: unexpected error: %v, expected EOPNOTSUPP",
                           err);

    (void)os_remove_all(d);
    ARENA_END;
}

static int compare_str(const void *x, const void *y) {
    const Str *a = (const Str *)x, *b = (const Str *)y;
    Int n = a->len < b->len ? a->len : b->len;
    int c = n > 0 ? memcmp(a->p, b->p, (size_t)n) : 0;
    return c != 0 ? c : (a->len > b->len) - (a->len < b->len);
}

static void TestDirent(TestingT *t) {
    enum { direntBufSize = 2048, filenameMinSize = 11 };
    ARENA_BEGIN;
    Str d = temp_dir(t, a);
    for (int i = 0; i < 10; i++) {
        char name[32], path[512];
        memset(name, '0' + i, (size_t)(filenameMinSize + i));
        name[filenameMinSize + i] = 0;
        touch(t, join(path, sizeof path, d, name));
    }

    Slice names = slice_make(a, TYPE_STRING, 0, 10);
    Error err = BURROW_NO_ERROR;
    Int fd = syscall_open(d, SYSCALL_O_RDONLY, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "syscall.open: %v", err);

    Int size = direntBufSize;
    Byte *buf = (Byte *)mem_alloc(a, (size_t)size, 1);
    memset(buf, 0xCD, (size_t)size);
    for (;;) {
        Int n = syscall_read_dirent(fd, (Slice){buf, size, size, TYPE_BYTE}, &err);
        if (errno_of(err) == SYSCALL_EINVAL) {
            /* getdents64 says EINVAL when the buffer is too small. */
            size *= 2;
            buf = (Byte *)mem_alloc(a, (size_t)size, 1);
            memset(buf, 0xCD, (size_t)size);
            continue;
        }
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "syscall.readdir: %v", err);
        if (n == 0)
            break;
        Int count = 0;
        Int consumed = syscall_parse_dirent(a, (Slice){buf, n, n, TYPE_BYTE}, -1, names,
                                            &count, &names);
        if (consumed != n)
            testing_t_fatalf_v(t, "ParseDirent: consumed %d bytes; expected %d",
                               consumed, n);
    }
    (void)syscall_close(fd);

    qsort(names.p, (size_t)names.len, sizeof(Str), compare_str);
    if (names.len != 10)
        testing_t_errorf_v(t, "got %d names; expected 10", names.len);
    for (Int i = 0; i < names.len; i++) {
        Str name = ((Str *)names.p)[i];
        int ord = name.len > 0 ? name.p[0] - '0' : -1;
        bool ok = ord >= 0 && ord <= 9 && name.len == filenameMinSize + ord;
        for (Int j = 0; ok && j < name.len; j++)
            ok = name.p[j] == name.p[0];
        if (!ok)
            testing_t_errorf_v(t, "names[%d] is %q (len %d)", i, name, name.len);
    }
    (void)os_remove_all(d);
    ARENA_END;
}

static void TestDirentRepeat(TestingT *t) {
    enum { N = 100 };
    ARENA_BEGIN;
    Str d = temp_dir(t, a);
    for (int i = 0; i < N; i++) {
        char name[32], path[512];
        snprintf(name, sizeof name, "file%d", i);
        touch(t, join(path, sizeof path, d, name));
    }
    /* small enough that the loop has to go round more than once */
    Int size = (Int)(N * offsetof(SyscallDirent, name) / 4);

    Error err = BURROW_NO_ERROR;
    Int fd = syscall_open(d, SYSCALL_O_RDONLY, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "syscall.open: %v", err);
    Slice files = slice_nil(TYPE_STRING);
    Int rounds = 0;
    for (;;) {
        Byte *buf = (Byte *)mem_alloc(a, (size_t)size, 1);
        Int n = syscall_read_dirent(fd, (Slice){buf, size, size, TYPE_BYTE}, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "syscall.readdir: %v", err);
        if (n == 0)
            break;
        rounds++;
        Slice b = {buf, n, n, TYPE_BYTE};
        while (b.len > 0) {
            Int used = syscall_parse_dirent(a, b, 1, files, NULL, &files);
            b.p = (Byte *)b.p + used;
            b.len -= used;
            b.cap -= used;
        }
    }
    (void)syscall_close(fd);
    if (files.len != N)
        testing_t_errorf_v(t, "got %d files, want %d", files.len, N);
    if (rounds < 2)
        testing_t_errorf_v(t, "ReadDirent filled the buffer %d times, want 2 or more",
                           rounds);
    (void)os_remove_all(d);
    ARENA_END;
}

static void TestPrlimitSelf(TestingT *t) {
    SyscallRlimit lim;
    memset(&lim, 0, sizeof lim);
    Error err = syscall_getrlimit(SYSCALL_RLIMIT_NOFILE, &lim);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Getrlimit: %v", err);
    struct rlimit c;
    if (getrlimit(RLIMIT_NOFILE, &c) != 0)
        testing_t_fatalf_v(t, "getrlimit failed");
    CHECK((uint64_t)lim.cur == (uint64_t)c.rlim_cur);
    CHECK((uint64_t)lim.max_ == (uint64_t)c.rlim_max);
    err = syscall_setrlimit(SYSCALL_RLIMIT_NOFILE, &lim);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Setrlimit with the same limit: %v", err);
}

static void TestGettimeofday(TestingT *t) {
    SyscallTimeval tv;
    memset(&tv, 0, sizeof tv);
    Error err = syscall_gettimeofday(&tv);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Gettimeofday: %v", err);
    if (tv.sec == 0 && tv.usec == 0)
        testing_t_fatalf_v(t, "Sec and Usec both zero");
#if !defined(BURROW_ARCH_386) && !defined(BURROW_ARCH_ARM) &&                          \
    !defined(BURROW_ARCH_PPC64)
    SyscallTime_t got = 0;
    SyscallTime_t now = syscall_time(&got, &err);
    if (BURROW_FAILED(err) || now != got || (int64_t)now < (int64_t)tv.sec)
        testing_t_errorf_v(t, "Time() = %d, %v", (Int)now, err);
#endif
}

static void TestGetwdGetgroups(TestingT *t) {
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    Str wd = syscall_getwd(a, &err);
    char buf[4096];
    if (BURROW_FAILED(err) || getcwd(buf, sizeof buf) == NULL || !str_is(wd, buf))
        testing_t_errorf_v(t, "Getwd() = %q, %v, want %q", wd, err, buf);

    Slice gids = syscall_getgroups(a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Getgroups: %v", err);
    gid_t list[256];
    int n = getgroups(256, list);
    if (n >= 0) {
        CHECK(gids.len == n);
        for (Int i = 0; i < gids.len && i < n; i++)
            CHECK(((Int *)gids.p)[i] == (Int)list[i]);
    }
    CHECK(syscall_getpgrp() == (Int)getpgrp());
    ARENA_END;
}

static void TestPipeUtimes(TestingT *t) {
    ARENA_BEGIN;
    Int p[2] = {-1, -1};
    Error err = syscall_pipe2((Slice){p, 2, 2, TYPE_INT}, SYSCALL_O_CLOEXEC);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Pipe2: %v", err);
    CHECK((fcntl((int)p[0], F_GETFD) & FD_CLOEXEC) != 0);
    (void)syscall_close(p[0]);
    (void)syscall_close(p[1]);
    err = syscall_pipe((Slice){p, 1, 1, TYPE_INT});
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "Pipe with one slot = %v, want EINVAL", err);

    Str d = temp_dir(t, a);
    char file[512];
    join(file, sizeof file, d, "f");
    touch(t, file);
    SyscallTimeval tv[2];
    memset(tv, 0, sizeof tv);
    tv[0].sec = 1000000000;
    tv[1].sec = 1200000000;
    tv[1].usec = 5;
    err = syscall_utimes(cstr(file), (Slice){tv, 2, 2, TYPE_BYTE});
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Utimes: %v", err);
    struct stat st;
    CHECK(stat(file, &st) == 0);
    CHECK(st.st_atime == 1000000000 && st.st_mtime == 1200000000);
    CHECK(st.st_mtim.tv_nsec == 5000);

    SyscallTimespec ts[2];
    ts[0] = syscall_nsec_to_timespec(1300000000000000007);
    ts[1] = syscall_nsec_to_timespec(1400000000000000009);
    err = syscall_utimes_nano(cstr(file), (Slice){ts, 2, 2, TYPE_BYTE});
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "UtimesNano: %v", err);
    CHECK(stat(file, &st) == 0);
    CHECK(st.st_mtime == 1400000000 && st.st_mtim.tv_nsec == 9);
    err = syscall_utimes(cstr(file), (Slice){tv, 1, 1, TYPE_BYTE});
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "Utimes with one time = %v, want EINVAL", err);

    Int fd = syscall_open(cstr(file), SYSCALL_O_RDONLY, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Open: %v", err);
    err = syscall_futimes(fd, (Slice){tv, 2, 2, TYPE_BYTE});
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Futimes: %v", err);
    (void)syscall_close(fd);
    CHECK(stat(file, &st) == 0);
    CHECK(st.st_mtime == 1200000000);

    char dst[512];
    join(dst, sizeof dst, d, "g");
    CHECK(!BURROW_FAILED(syscall_rename(cstr(file), cstr(dst))));
    CHECK(!BURROW_FAILED(syscall_link(cstr(dst), cstr(file))));
    Byte rl[64];
    char sub[512];
    join(sub, sizeof sub, d, "s");
    CHECK(!BURROW_FAILED(syscall_symlink(BURROW_S("g"), cstr(sub))));
    Int n = syscall_readlink(cstr(sub), (Slice){rl, 64, 64, TYPE_BYTE}, &err);
    CHECK(n == 1 && rl[0] == 'g');
    CHECK(!BURROW_FAILED(syscall_unlink(cstr(sub))));
    join(sub, sizeof sub, d, "sub");
    CHECK(!BURROW_FAILED(syscall_mkdir(cstr(sub), 0755)));
    CHECK(!BURROW_FAILED(syscall_rmdir(cstr(sub))));
    CHECK(errno_of(syscall_rmdir(cstr(sub))) == SYSCALL_ENOENT);
    (void)os_remove_all(d);
    ARENA_END;
}

#define LINUX_TESTS(X)                                                                 \
    X(TestFaccessat)                                                                   \
    X(TestFchmodat)                                                                    \
    X(TestDirent)                                                                      \
    X(TestDirentRepeat)                                                                \
    X(TestPrlimitSelf)                                                                 \
    X(TestGettimeofday)                                                                \
    X(TestGetwdGetgroups)                                                              \
    X(TestPipeUtimes)
#else
#define LINUX_TESTS(X)
#endif

#define TESTS(X)                                                                       \
    X(TestEnv)                                                                         \
    X(TestTimeConversions)                                                             \
    X(TestGetpagesize)                                                                 \
    X(TestStringByteSlice)                                                             \
    UNIX_TESTS(X)                                                                      \
    LINUX_TESTS(X)

TESTING_MAIN(TESTS)
