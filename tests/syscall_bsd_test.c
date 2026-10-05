/* The parts of syscall Go writes by hand on macOS and FreeBSD: Getwd,
 * Getgroups, Pipe, the times, Getdirentries, ReadDirent and ParseDirent,
 * Kevent, Sysctl, Sendfile, Getfsstat and the rest.
 *
 * TestDirent, TestDirentRepeat, TestGetdirentries and TestGetfsstat are
 * ported from Go's src/syscall/dirent_test.go, getdirentries_test.go and
 * syscall_bsd_test.go. The others are burrow's own, and check against the C
 * library where it has the same call.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/syscall.h"

#if defined(BURROW_OS_DARWIN) || defined(BURROW_OS_FREEBSD)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

/* Go's test has its own, as syscall does not export it. */
#define MNT_NOWAIT_ 2

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

static void write_file(TestingT *t, const char *path, const char *data) {
    Error err = BURROW_NO_ERROR;
    Int fd = syscall_open(
        cstr(path), SYSCALL_O_CREAT | SYSCALL_O_WRONLY | SYSCALL_O_TRUNC, 0644, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Open(%v): %v", path, err);
    /* The mode is the third argument of a variadic open, the one a wrong call
     * loses first. */
    struct stat st;
    memset(&st, 0, sizeof st);
    if (fstat((int)fd, &st) != 0 || (st.st_mode & 0600) != 0600)
        testing_t_fatalf_v(t, "Open(%v) with 0644 made mode %o", path,
                           (Int)(st.st_mode & 07777));
    Int n = (Int)strlen(data);
    if (n > 0 &&
        syscall_write(fd, (Slice){(void *)(uintptr_t)data, n, n, TYPE_BYTE}, &err) != n)
        testing_t_fatalf_v(t, "Write(%v): %v", path, err);
    (void)syscall_close(fd);
}

static int compare_str(const void *x, const void *y) {
    const Str *a = (const Str *)x;
    const Str *b = (const Str *)y;
    Int n = a->len < b->len ? a->len : b->len;
    int c = n > 0 ? memcmp(a->p, b->p, (size_t)n) : 0;
    return c != 0 ? c : (a->len > b->len) - (a->len < b->len);
}

/* --------------------------------------------------------------- dirents */

static void TestDirent(TestingT *t) {
    enum { direntBufSize = 2048, filenameMinSize = 11 };
    ARENA_BEGIN;
    Str d = temp_dir(t, a);
    for (int i = 0; i < 10; i++) {
        char name[32], path[512];
        memset(name, '0' + i, (size_t)(filenameMinSize + i));
        name[filenameMinSize + i] = 0;
        write_file(t, join(path, sizeof path, d, name), "");
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
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "syscall.readdir: %v", err);
        testing_t_logf_v(t, "ReadDirent: read %d bytes", n);
        if (n == 0)
            break;
        Int count = 0;
        Int consumed = syscall_parse_dirent(a, (Slice){buf, n, n, TYPE_BYTE}, -1, names,
                                            &count, &names);
        testing_t_logf_v(t, "ParseDirent: %d new name(s)", count);
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
    /* Small enough that the loop has to go round more than once. FreeBSD
     * wants at least DIRBLKSIZ. */
    Int size = (Int)(N * offsetof(SyscallDirent, name) / 4);
#if defined(BURROW_OS_FREEBSD)
    if (size < 1024)
        size = 1024;
#endif
    Str d = temp_dir(t, a);
    for (int i = 0; i < N; i++) {
        char name[32], path[512];
        snprintf(name, sizeof name, "file%d", i);
        write_file(t, join(path, sizeof path, d, name), "contents");
    }

    Error err = BURROW_NO_ERROR;
    Int fd = syscall_open(d, SYSCALL_O_RDONLY, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "syscall.open: %v", err);
    Slice files = slice_nil(TYPE_STRING);
    for (;;) {
        Byte *buf = (Byte *)mem_alloc(a, (size_t)size, 1);
        Int n = syscall_read_dirent(fd, (Slice){buf, size, size, TYPE_BYTE}, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "syscall.readdir: %v", err);
        if (n == 0)
            break;
        Slice b = {buf, n, n, TYPE_BYTE};
        while (b.len > 0) {
            Int used = syscall_parse_dirent(a, b, -1, files, NULL, &files);
            b.p = (Byte *)b.p + used;
            b.len -= used;
            b.cap -= used;
        }
    }
    (void)syscall_close(fd);

    qsort(files.p, (size_t)files.len, sizeof(Str), compare_str);
    bool ok = files.len == N;
    for (Int i = 0; ok && i < files.len; i++) {
        char want[32];
        snprintf(want, sizeof want, "file%d", (int)i);
        ok = false;
        for (Int j = 0; j < files.len && !ok; j++)
            ok = str_is(((Str *)files.p)[j], want);
    }
    if (!ok)
        testing_t_errorf_v(t, "bad file list: got %d names, want file0 to file%d",
                           files.len, N - 1);
    (void)os_remove_all(d);
    ARENA_END;
}

static void get_direntries(TestingT *t, int count) {
    ARENA_BEGIN;
    Str d = temp_dir(t, a);
    for (int i = 0; i < count; i++) {
        char name[32], path[512];
        snprintf(name, sizeof name, "file%03d", i);
        write_file(t, join(path, sizeof path, d, name), "data");
    }

    Error err = BURROW_NO_ERROR;
    Int fd = syscall_open(d, SYSCALL_O_RDONLY, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Open: %v", err);
    Uintptr base = 0;
    Byte buf[2048];
    Slice names2 = slice_nil(TYPE_STRING);
    for (;;) {
        Int n =
            syscall_getdirentries(fd, (Slice){buf, 2048, 2048, TYPE_BYTE}, &base, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "Getdirentries: %v", err);
        if (n == 0)
            break;
        Int off = 0;
        while (off < n) {
            /* The last one can be shorter than a whole SyscallDirent. */
            SyscallDirent ent;
            memset(&ent, 0, sizeof ent);
            size_t k = (size_t)(n - off) < sizeof ent ? (size_t)(n - off) : sizeof ent;
            memcpy(&ent, buf + off, k);
            off += (Int)ent.reclen;
            Byte *name = (Byte *)mem_alloc(a, ent.namlen, 1);
            for (Int i = 0; i < (Int)ent.namlen; i++)
                name[i] = (Byte)ent.name[i];
            Str s = str_from_bytes(name, (Int)ent.namlen);
            names2 = slice_append(a, names2, &s, 1);
        }
    }
    (void)syscall_close(fd);

    /* Getdirentries gives . and .. as well. */
    qsort(names2.p, (size_t)names2.len, sizeof(Str), compare_str);
    bool ok = names2.len == count + 2 && str_is(((Str *)names2.p)[0], ".") &&
              str_is(((Str *)names2.p)[1], "..");
    for (Int i = 0; ok && i < count; i++) {
        char want[32];
        snprintf(want, sizeof want, "file%03d", (int)i);
        ok = str_is(((Str *)names2.p)[i + 2], want);
    }
    if (!ok)
        testing_t_errorf_v(t, "names don't match: got %d, want %d", names2.len,
                           count + 2);
    (void)os_remove_all(d);
    ARENA_END;
}

static void TestGetdirentries(TestingT *t) {
    get_direntries(t, 10);
    if (testing_short())
        testing_t_skip_v(t, "skipping 1000 files in -short mode");
    get_direntries(t, 1000);
}

/* ------------------------------------------------------------- getfsstat */

static void TestGetfsstat(TestingT *t) {
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    Int n = syscall_getfsstat(slice_nil(TYPE_BYTE), MNT_NOWAIT_, &err);
    testing_t_logf_v(t, "Getfsstat(nil, %d) = (%d, %v)", MNT_NOWAIT_, n, err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    SyscallStatfs_t *data = (SyscallStatfs_t *)mem_alloc_array(
        a, (size_t)n, sizeof *data, _Alignof(SyscallStatfs_t));
    Int n2 = syscall_getfsstat((Slice){data, n, n, NULL}, MNT_NOWAIT_, &err);
    testing_t_logf_v(t, "Getfsstat([]syscall.Statfs_t, %d) = (%d, %v)", MNT_NOWAIT_, n2,
                     err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    if (n != n2)
        testing_t_errorf_v(
            t, "Getfsstat(nil) = %d, but subsequent Getfsstat(slice) = %d", n, n2);
    SyscallStatfs_t zero;
    memset(&zero, 0, sizeof zero);
    for (Int i = 0; i < n; i++)
        if (memcmp(&data[i], &zero, sizeof zero) == 0)
            testing_t_errorf_v(t, "index %v is an empty Statfs_t struct", i);
    ARENA_END;
}

/* ------------------------------------------------- the ones that wrap a call */

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
    ARENA_END;
}

static void TestPipeUtimes(TestingT *t) {
    ARENA_BEGIN;
    Int p[2] = {-1, -1};
    Error err = syscall_pipe((Slice){p, 2, 2, TYPE_INT});
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Pipe: %v", err);
    Byte c = 'x';
    CHECK(syscall_write(p[1], (Slice){&c, 1, 1, TYPE_BYTE}, &err) == 1);
    c = 0;
    CHECK(syscall_read(p[0], (Slice){&c, 1, 1, TYPE_BYTE}, &err) == 1 && c == 'x');
    (void)syscall_close(p[0]);
    (void)syscall_close(p[1]);
    err = syscall_pipe((Slice){p, 1, 1, TYPE_INT});
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "Pipe with one slot = %v, want EINVAL", err);
#if defined(BURROW_OS_FREEBSD)
    err = syscall_pipe2((Slice){p, 2, 2, TYPE_INT}, SYSCALL_O_CLOEXEC);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Pipe2: %v", err);
    (void)syscall_close(p[0]);
    (void)syscall_close(p[1]);
#endif

    Str d = temp_dir(t, a);
    char file[512];
    join(file, sizeof file, d, "f");
    write_file(t, file, "");
    SyscallTimeval tv[2];
    memset(tv, 0, sizeof tv);
    tv[0].sec = 1000000000;
    tv[1].sec = 1200000000;
    tv[1].usec = 5;
    err = syscall_utimes(cstr(file), (Slice){tv, 2, 2, NULL});
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Utimes: %v", err);
    struct stat st;
    CHECK(stat(file, &st) == 0);
    CHECK(st.st_atime == 1000000000 && st.st_mtime == 1200000000);
    CHECK(st.st_mtimespec.tv_nsec == 5000);

    SyscallTimespec ts[2];
    ts[0] = syscall_nsec_to_timespec(1300000000000000007);
    ts[1] = syscall_nsec_to_timespec(1400000000000000009);
    err = syscall_utimes_nano(cstr(file), (Slice){ts, 2, 2, NULL});
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "UtimesNano: %v", err);
    CHECK(stat(file, &st) == 0);
    CHECK(st.st_mtime == 1400000000);
    /* APFS and UFS both keep nanoseconds. */
    CHECK(st.st_mtimespec.tv_nsec == 9);
    err = syscall_utimes(cstr(file), (Slice){tv, 1, 1, NULL});
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "Utimes with one time = %v, want EINVAL", err);
    err = syscall_utimes_nano(cstr(file), (Slice){ts, 3, 3, NULL});
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "UtimesNano with three times = %v, want EINVAL", err);

    Int fd = syscall_open(cstr(file), SYSCALL_O_RDONLY, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Open: %v", err);
    err = syscall_futimes(fd, (Slice){tv, 2, 2, NULL});
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Futimes: %v", err);
    CHECK(errno_of(syscall_futimes(fd, (Slice){tv, 1, 1, NULL})) == SYSCALL_EINVAL);
    (void)syscall_close(fd);
    CHECK(stat(file, &st) == 0);
    CHECK(st.st_mtime == 1200000000);
    (void)os_remove_all(d);
    ARENA_END;
}

static void TestKevent(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Int kq = syscall_kqueue(&err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Kqueue: %v", err);
    Int p[2] = {-1, -1};
    if (BURROW_FAILED(syscall_pipe((Slice){p, 2, 2, TYPE_INT})))
        testing_t_fatal_v(t, "Pipe failed");

    SyscallKevent_t change;
    memset(&change, 0, sizeof change);
    syscall_set_kevent(&change, p[0], SYSCALL_EVFILT_READ, SYSCALL_EV_ADD);
    CHECK((Int)change.ident == p[0]);
    CHECK(change.filter == SYSCALL_EVFILT_READ && change.flags == SYSCALL_EV_ADD);
    Int n =
        syscall_kevent(kq, (Slice){&change, 1, 1, NULL}, slice_nil(NULL), NULL, &err);
    if (BURROW_FAILED(err) || n != 0)
        testing_t_fatalf_v(t, "Kevent to add = %d, %v", n, err);

    /* Nothing to read yet, so a zero timeout gives nothing. */
    SyscallKevent_t ev[2];
    memset(ev, 0, sizeof ev);
    SyscallTimespec zero = syscall_nsec_to_timespec(0);
    n = syscall_kevent(kq, slice_nil(NULL), (Slice){ev, 2, 2, NULL}, &zero, &err);
    if (BURROW_FAILED(err) || n != 0)
        testing_t_errorf_v(t, "Kevent with nothing ready = %d, %v, want 0", n, err);

    Byte c[3] = {'a', 'b', 'c'};
    CHECK(syscall_write(p[1], (Slice){c, 3, 3, TYPE_BYTE}, &err) == 3);
    SyscallTimespec second = syscall_nsec_to_timespec(1000000000);
    n = syscall_kevent(kq, slice_nil(NULL), (Slice){ev, 2, 2, NULL}, &second, &err);
    if (BURROW_FAILED(err) || n != 1)
        testing_t_fatalf_v(t, "Kevent after a write = %d, %v, want 1", n, err);
    CHECK((Int)ev[0].ident == p[0]);
    CHECK(ev[0].filter == SYSCALL_EVFILT_READ);
    CHECK(ev[0].data == 3);
    (void)syscall_close(p[0]);
    (void)syscall_close(p[1]);
    (void)syscall_close(kq);
}

static void TestSysctl(TestingT *t) {
    ARENA_BEGIN;
    Error err = BURROW_NO_ERROR;
    Str ostype = syscall_sysctl(a, BURROW_S("kern.ostype"), &err);
#if defined(BURROW_OS_FREEBSD)
    const char *want = "FreeBSD";
#else
    const char *want = "Darwin";
#endif
    if (BURROW_FAILED(err) || !str_is(ostype, want))
        testing_t_errorf_v(t, "Sysctl(kern.ostype) = %q, %v, want %q", ostype, err,
                           want);

    char host[256];
    size_t hlen = sizeof host;
    CHECK(sysctlbyname("kern.hostname", host, &hlen, NULL, 0) == 0);
    Str h = syscall_sysctl(a, BURROW_S("kern.hostname"), &err);
    if (BURROW_FAILED(err) || !str_is(h, host))
        testing_t_errorf_v(t, "Sysctl(kern.hostname) = %q, %v, want %q", h, err, host);

    int maxproc = 0;
    size_t mlen = sizeof maxproc;
    CHECK(sysctlbyname("kern.maxproc", &maxproc, &mlen, NULL, 0) == 0);
    uint32_t got = syscall_sysctl_uint32(BURROW_S("kern.maxproc"), &err);
    if (BURROW_FAILED(err) || got != (uint32_t)maxproc)
        testing_t_errorf_v(t, "SysctlUint32(kern.maxproc) = %d, %v, want %d", (Int)got,
                           err, (Int)maxproc);

    /* kern.ostype is a string longer than four bytes. */
    got = syscall_sysctl_uint32(BURROW_S("kern.ostype"), &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "SysctlUint32(kern.ostype) = %d, want an error",
                           (Int)got);

    (void)syscall_sysctl(a, BURROW_S("kern.no_such_thing"), &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "Sysctl of a name that is not there worked");
    (void)syscall_sysctl(a, (Str){(const Byte *)"kern\0ostype", 11}, &err);
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "Sysctl of a name with a NUL = %v, want EINVAL", err);
    ARENA_END;
}

static void TestRlimitFlock(TestingT *t) {
    ARENA_BEGIN;
    SyscallRlimit lim;
    memset(&lim, 0, sizeof lim);
    Error err = syscall_getrlimit(SYSCALL_RLIMIT_NOFILE, &lim);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Getrlimit: %v", err);
    struct rlimit c;
    CHECK(getrlimit(RLIMIT_NOFILE, &c) == 0);
    CHECK((uint64_t)lim.cur == (uint64_t)c.rlim_cur);
    err = syscall_setrlimit(SYSCALL_RLIMIT_NOFILE, &lim);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Setrlimit with the same limit: %v", err);

    Str d = temp_dir(t, a);
    char file[512];
    join(file, sizeof file, d, "lock");
    write_file(t, file, "x");
    Int fd = syscall_open(cstr(file), SYSCALL_O_RDWR, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Open: %v", err);
    SyscallFlock_t lk;
    memset(&lk, 0, sizeof lk);
    lk.type = SYSCALL_F_WRLCK;
    err = syscall_fcntl_flock((Uintptr)fd, SYSCALL_F_SETLK, &lk);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "FcntlFlock F_SETLK: %v", err);
    /* A process's own lock never gets in its way. */
    memset(&lk, 0, sizeof lk);
    lk.type = SYSCALL_F_WRLCK;
    err = syscall_fcntl_flock((Uintptr)fd, SYSCALL_F_GETLK, &lk);
    if (BURROW_FAILED(err) || lk.type != SYSCALL_F_UNLCK)
        testing_t_errorf_v(t, "FcntlFlock F_GETLK = type %d, %v, want F_UNLCK",
                           (Int)lk.type, err);
    (void)syscall_close(fd);
    (void)os_remove_all(d);
    ARENA_END;
}

static void TestSendfile(TestingT *t) {
    ARENA_BEGIN;
    Str d = temp_dir(t, a);
    char file[512];
    join(file, sizeof file, d, "data");
    write_file(t, file, "hello, sendfile world");
    Error err = BURROW_NO_ERROR;
    Int fd = syscall_open(cstr(file), SYSCALL_O_RDONLY, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Open: %v", err);
    SyscallSocketpairRet sv =
        syscall_socketpair(SYSCALL_AF_UNIX, SYSCALL_SOCK_STREAM, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Socketpair: %v", err);
    int64_t off = 7;
    Int n = syscall_sendfile(sv.fd[0], fd, &off, 8, &err);
    if (BURROW_FAILED(err) || n != 8)
        testing_t_fatalf_v(t, "Sendfile = %d, %v, want 8", n, err);
    CHECK(off == 7);
    Byte buf[16];
    Int got = 0;
    while (got < 8) {
        Int r = syscall_read(sv.fd[1],
                             (Slice){buf + got, 16 - got, 16 - got, TYPE_BYTE}, &err);
        if (r <= 0)
            break;
        got += r;
    }
    if (got != 8 || memcmp(buf, "sendfile", 8) != 0)
        testing_t_errorf_v(t, "read %d bytes after Sendfile, want \"sendfile\"", got);
    (void)syscall_close(sv.fd[0]);
    (void)syscall_close(sv.fd[1]);
    (void)syscall_close(fd);
    (void)os_remove_all(d);
    ARENA_END;
}

#if defined(BURROW_OS_FREEBSD)
static void TestStatMknod(TestingT *t) {
    ARENA_BEGIN;
    Str d = temp_dir(t, a);
    char file[512], link[512], fifo[512];
    join(file, sizeof file, d, "f");
    join(link, sizeof link, d, "l");
    join(fifo, sizeof fifo, d, "p");
    write_file(t, file, "four");
    CHECK(BURROW_OK(syscall_symlink(cstr("f"), cstr(link))));
    SyscallStat_t st;
    memset(&st, 0, sizeof st);
    Error err = syscall_stat(cstr(link), &st);
    if (BURROW_FAILED(err) || st.size != 4)
        testing_t_errorf_v(t, "Stat of the link = size %d, %v, want 4", (Int)st.size,
                           err);
    err = syscall_lstat(cstr(link), &st);
    if (BURROW_FAILED(err) || (st.mode & SYSCALL_S_IFMT) != SYSCALL_S_IFLNK)
        testing_t_errorf_v(t, "Lstat of the link = mode %o, %v", (Int)st.mode, err);
    err = syscall_mknod(cstr(fifo), SYSCALL_S_IFIFO | 0600, 0);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Mknod: %v", err);
    err = syscall_lstat(cstr(fifo), &st);
    CHECK(BURROW_OK(err) && (st.mode & SYSCALL_S_IFMT) == SYSCALL_S_IFIFO);
    (void)os_remove_all(d);
    ARENA_END;
}
#define OS_TESTS(X) X(TestStatMknod)
#else
static void TestPtrace(TestingT *t) {
    /* Nobody has this pid, and attaching to nobody fails. */
    Error err = syscall_ptrace_attach(99999999);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "PtraceAttach of a pid nobody has worked");
    testing_t_logf_v(t, "PtraceAttach: %v", err);
    err = syscall_ptrace_detach(99999999);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "PtraceDetach of a pid nobody has worked");
}
#define OS_TESTS(X) X(TestPtrace)
#endif

#define TESTS(X)                                                                       \
    X(TestDirent)                                                                      \
    X(TestDirentRepeat)                                                                \
    X(TestGetdirentries)                                                               \
    X(TestGetfsstat)                                                                   \
    X(TestGetwdGetgroups)                                                              \
    X(TestPipeUtimes)                                                                  \
    X(TestKevent)                                                                      \
    X(TestSysctl)                                                                      \
    X(TestRlimitFlock)                                                                 \
    X(TestSendfile)                                                                    \
    OS_TESTS(X)

#else

static void TestNothing(TestingT *t) {
    testing_t_skip_v(t, "these are the macOS and FreeBSD functions");
}

#define TESTS(X) X(TestNothing)

#endif

TESTING_MAIN(TESTS)
