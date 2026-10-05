/* syscall on Windows: Open and the other file calls, the socket calls and
 * Sockaddrs, ConnectEx, and the SID and token calls.
 *
 * TestOpen, TestComputerName, TestWin32finddata, TestTOKEN_ALL_ACCESS,
 * TestGetwd_DoesNotPanicWhenPathIsLong and TestGetStartupInfo are ported from
 * Go's src/syscall/syscall_windows_test.go. Go source: go1.27.1. The rest are
 * burrow's own, since Go tests these calls through os and net.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/syscall.h"

#if defined(BURROW_OS_WINDOWS)

#include <string.h>

static Arena ar;
static Alloc *a;

static SyscallErrno errno_of(Error err) {
    const SyscallErrno *e = errors_as(err, TYPE_SYSCALL_ERRNO);
    return e == NULL ? 0 : *e;
}

/* t.TempDir. */
static Str temp_dir(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Str d =
        os_mkdir_temp(a, BURROW_S(""), BURROW_S("burrow-syscall-windows-test-*"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "MkdirTemp: %v", e);
    return d;
}

static Str join(Str d, const char *name) {
    return fmt_sprintf_v(a, "%s\\%s", d, str_from_cstr(name));
}

static void create(TestingT *t, Str path) {
    Error e = BURROW_NO_ERROR;
    SyscallHandle h = syscall_open(
        path, SYSCALL_O_RDWR | SYSCALL_O_CREAT | SYSCALL_O_TRUNC, 0666, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "create %s: %v", path, e);
    syscall_close_handle(h);
}

static void TestOpen(TestingT *t) {
    Str dir = temp_dir(t);
    Str file = join(dir, "a");
    create(t, file);

    static const struct {
        bool is_dir;
        Int flag;
        SyscallErrno err;
    } tests[] = {
        {true, SYSCALL_O_RDONLY, 0},
        {true, SYSCALL_O_CREAT, 0},
        {true, SYSCALL_O_RDONLY | SYSCALL_O_CREAT, 0},
        {false, SYSCALL_O_APPEND | SYSCALL_O_WRONLY | SYSCALL_O_CREAT, 0},
        {false, SYSCALL_O_APPEND | SYSCALL_O_WRONLY | SYSCALL_O_CREAT | SYSCALL_O_TRUNC,
         0},
        {false, SYSCALL_O_WRONLY | SYSCALL_O_RDWR, 0},
        {true, SYSCALL_O_WRONLY | SYSCALL_O_RDWR, 0},
        {true, SYSCALL_O_RDONLY | SYSCALL_O_TRUNC, SYSCALL_ERROR_ACCESS_DENIED},
        {true, SYSCALL_O_WRONLY, SYSCALL_EISDIR},
        {true, SYSCALL_O_RDWR, SYSCALL_EISDIR},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        SyscallHandle h =
            syscall_open(tests[i].is_dir ? dir : file, tests[i].flag, 0660, &err);
        if (BURROW_OK(err))
            syscall_close_handle(h);
        if (errno_of(err) != tests[i].err || (tests[i].err == 0) != BURROW_OK(err))
            testing_t_errorf_v(t, "%d: Open got %v, want %d", (Int)i, err,
                               (Int)tests[i].err);
    }

    /* The flags Go has no test for: an empty name, a FILE_FLAG_ that
     * CreateFile is not given, and the directory flag on a file. */
    Error err = BURROW_NO_ERROR;
    syscall_open(BURROW_S(""), SYSCALL_O_RDONLY, 0, &err);
    if (errno_of(err) != SYSCALL_ERROR_FILE_NOT_FOUND)
        testing_t_errorf_v(t, "Open(\"\") = %v, want ERROR_FILE_NOT_FOUND", err);
    syscall_open(file, SYSCALL_O_RDONLY | 0x40000000, 0, &err);
    if (!errors_is(err, fs_err_invalid))
        testing_t_errorf_v(t,
                           "Open with FILE_FLAG_OVERLAPPED's neighbour = %v, want %v",
                           err, fs_err_invalid);
    syscall_open(file, SYSCALL_O_RDONLY | 0x4000, 0, &err);
    if (errno_of(err) != SYSCALL_ENOTDIR)
        testing_t_errorf_v(t, "Open(file, o_DIRECTORY) = %v, want ENOTDIR", err);
    SyscallHandle h = syscall_open(dir, SYSCALL_O_RDONLY | 0x4000, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Open(dir, o_DIRECTORY) = %v", err);
    else
        syscall_close_handle(h);
    os_remove_all(dir);
}

static void TestComputerName(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Str name = syscall_computer_name(a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ComputerName failed: %v", err);
    if (name.len == 0)
        testing_t_errorf_v(t, "ComputerName returned empty string");
}

static void TestWin32finddata(TestingT *t) {
    Str dir = temp_dir(t);
    Str path = join(dir, "long_name.and_extension");
    create(t, path);

    struct {
        SyscallWin32finddata fd;
        uint8_t got;
        uint8_t pad[10]; /* to protect ourselves */
    } x;
    memset(&x, 0, sizeof x);
    uint8_t want = 2; /* it is unlikely to have this character in the filename */
    x.got = want;

    Error err = BURROW_NO_ERROR;
    uint16_t *pathp = syscall_utf16_ptr_from_string(a, path, &err);
    SyscallHandle h = syscall_find_first_file(pathp, &x.fd, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "FindFirstFile failed: %v", err);
    Str got =
        syscall_utf16_to_string(a, (Slice){x.fd.file_name, 259, 259, TYPE_UINT16});
    if (!str_eq(got, BURROW_S("long_name.and_extension")))
        testing_t_errorf_v(t, "FileName = %q", got);
    err = syscall_find_close(h);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "FindClose failed: %v", err);
    if (x.got != want)
        testing_t_fatalf_v(t, "memory corruption: want=%d got=%d", (Int)want,
                           (Int)x.got);

    /* FindNextFile through a directory with two files in it. */
    create(t, join(dir, "b"));
    pathp = syscall_utf16_ptr_from_string(a, join(dir, "*"), &err);
    h = syscall_find_first_file(pathp, &x.fd, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "FindFirstFile(*) failed: %v", err);
    Int n = 1;
    while (BURROW_OK(syscall_find_next_file(h, &x.fd)))
        n++;
    syscall_find_close(h);
    /* ., .., the file and b. */
    if (n != 4)
        testing_t_errorf_v(t, "found %d entries, want 4", n);
    os_remove_all(dir);
}

static void TestTOKEN_ALL_ACCESS(TestingT *t) {
    if (SYSCALL_TOKEN_ALL_ACCESS != 0xF01FF)
        testing_t_errorf_v(t, "TOKEN_ALL_ACCESS = %x, want 0xF01FF",
                           (Int)SYSCALL_TOKEN_ALL_ACCESS);
}

static void TestGetwd_DoesNotPanicWhenPathIsLong(TestingT *t) {
    /* Regression test for https://github.com/golang/go/issues/60051. */
    Error err = BURROW_NO_ERROR;
    Str old = os_getwd(a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Getwd: %v", err);
    Str tmp = temp_dir(t);
    char a200[201];
    memset(a200, 'a', 200);
    a200[200] = 0;
    Str dirname =
        fmt_sprintf_v(a, "%s\\%s\\%s", tmp, str_from_cstr(a200), str_from_cstr(a200));
    err = os_mkdir_all(dirname, 0700);
    if (BURROW_FAILED(err)) {
        os_remove_all(tmp);
        testing_t_skip_v(t, "MkdirAll failed: %v", err);
    }
    err = syscall_chdir(dirname);
    if (BURROW_FAILED(err)) {
        os_remove_all(tmp);
        testing_t_skip_v(t, "Chdir failed: %v", err);
    }
    Str wd = syscall_getwd(a, &err);
    if (BURROW_FAILED(err) || wd.len < 400)
        testing_t_errorf_v(t, "Getwd = %q, %v", wd, err);
    syscall_chdir(old);
    os_remove_all(tmp);
}

static void TestGetStartupInfo(TestingT *t) {
    SyscallStartupInfo si;
    memset(&si, 0, sizeof si);
    Error err = syscall_get_startup_info(&si);
    if (BURROW_FAILED(err)) /* see https://go.dev/issue/31316 */
        testing_t_fatalf_v(t, "GetStartupInfo: got error %v, want nil", err);
}

static void TestReadWriteSeek(TestingT *t) {
    Str dir = temp_dir(t);
    Str file = join(dir, "rw");
    Error err = BURROW_NO_ERROR;
    SyscallHandle h = syscall_open(file, SYSCALL_O_RDWR | SYSCALL_O_CREAT, 0666, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Open: %v", err);
    Int n = syscall_write(
        h, slice_from((void *)(uintptr_t)"hello, world", 12, 12, TYPE_BYTE), &err);
    if (n != 12 || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Write = %d, %v", n, err);
    int64_t off = syscall_seek(h, 7, 0, &err);
    if (off != 7 || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Seek(7, 0) = %d, %v", (Int)off, err);
    char buf[16] = {0};
    n = syscall_read(h, slice_from(buf, 16, 16, TYPE_BYTE), &err);
    if (n != 5 || memcmp(buf, "world", 5) != 0)
        testing_t_errorf_v(t, "Read = %d %q, %v", n, str_from_cstr(buf), err);
    off = syscall_seek(h, -5, 2, &err);
    if (off != 7)
        testing_t_errorf_v(t, "Seek(-5, 2) = %d, %v", (Int)off, err);
    err = syscall_ftruncate(h, 5);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Ftruncate: %v", err);
    off = syscall_seek(h, 0, 2, &err);
    if (off != 5)
        testing_t_errorf_v(t, "size after Ftruncate(5) = %d", (Int)off);
    err = syscall_fsync(h);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Fsync: %v", err);
    syscall_close(h);

    /* O_APPEND writes at the end however the offset moves. */
    h = syscall_open(file, SYSCALL_O_WRONLY | SYSCALL_O_APPEND, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Open(O_APPEND): %v", err);
    syscall_seek(h, 0, 0, &err);
    syscall_write(h, slice_from((void *)(uintptr_t)"!", 1, 1, TYPE_BYTE), &err);
    syscall_close(h);
    h = syscall_open(file, SYSCALL_O_RDONLY, 0, &err);
    memset(buf, 0, sizeof buf);
    n = syscall_read(h, slice_from(buf, 16, 16, TYPE_BYTE), &err);
    if (n != 6 || memcmp(buf, "hello!", 6) != 0)
        testing_t_errorf_v(t, "after the append the file is %q", str_from_cstr(buf));
    syscall_close(h);

    /* O_TRUNC on an open of a file that is there. */
    h = syscall_open(file, SYSCALL_O_WRONLY | SYSCALL_O_TRUNC, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Open(O_TRUNC): %v", err);
    off = syscall_seek(h, 0, 2, &err);
    if (off != 0)
        testing_t_errorf_v(t, "size after O_TRUNC = %d", (Int)off);
    syscall_close(h);
    os_remove_all(dir);
}

static void TestFileOps(TestingT *t) {
    Str dir = temp_dir(t);
    Str sub = join(dir, "sub");
    Error err = syscall_mkdir(sub, 0);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Mkdir: %v", err);
    Str f = join(dir, "f");
    create(t, f);
    Str g = join(dir, "g");
    err = syscall_rename(f, g);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Rename: %v", err);

    /* Chmod toggles the read-only attribute and nothing else. */
    err = syscall_chmod(g, 0444);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Chmod(0444): %v", err);
    syscall_open(g, SYSCALL_O_WRONLY, 0, &err);
    if (errno_of(err) != SYSCALL_ERROR_ACCESS_DENIED)
        testing_t_errorf_v(t, "Open of a read-only file for writing = %v", err);
    err = syscall_chmod(g, 0666);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Chmod(0666): %v", err);

    /* Utimes and UtimesNano set the modification time. */
    SyscallTimeval tv[2] = {{1000000000, 0}, {1000000000, 0}};
    err = syscall_utimes(g, (Slice){tv, 2, 2, TYPE_BYTE});
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Utimes: %v", err);
    uint16_t *gp = syscall_utf16_ptr_from_string(a, g, &err);
    SyscallWin32finddata fd;
    SyscallHandle h = syscall_find_first_file(gp, &fd, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "FindFirstFile: %v", err);
    syscall_find_close(h);
    if (syscall_filetime_nanoseconds(&fd.last_write_time) !=
        1000000000LL * 1000000000LL)
        testing_t_errorf_v(t, "mtime after Utimes = %d",
                           (Int)syscall_filetime_nanoseconds(&fd.last_write_time));
    SyscallTimespec ts[2] = {{0, -1}, {1100000000, 500}};
    err = syscall_utimes_nano(g, (Slice){ts, 2, 2, TYPE_BYTE});
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "UtimesNano: %v", err);
    h = syscall_find_first_file(gp, &fd, &err);
    syscall_find_close(h);
    /* A Filetime counts in 100ns, so the 500ns go. */
    if (syscall_filetime_nanoseconds(&fd.last_write_time) !=
        1100000000LL * 1000000000LL)
        testing_t_errorf_v(t, "mtime after UtimesNano = %d",
                           (Int)syscall_filetime_nanoseconds(&fd.last_write_time));

    /* Fchdir goes to the directory a handle is open on, without the \\?\. */
    Str old = syscall_getwd(a, &err);
    h = syscall_open(sub, SYSCALL_O_RDONLY, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Open(sub): %v", err);
    err = syscall_fchdir(h);
    syscall_close(h);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Fchdir: %v", err);
    Str wd = syscall_getwd(a, &err);
    if (wd.len < 4 || memcmp(wd.p + wd.len - 4, "\\sub", 4) != 0 ||
        memcmp(wd.p, "\\\\?\\", 4) == 0)
        testing_t_errorf_v(t, "Getwd after Fchdir = %q", wd);
    syscall_chdir(old);

    err = syscall_unlink(g);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Unlink: %v", err);
    err = syscall_rmdir(sub);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Rmdir: %v", err);

    /* What Windows does not have. */
    if (errno_of(syscall_link(g, f)) != SYSCALL_EWINDOWS)
        testing_t_errorf_v(t, "Link did not say EWINDOWS");
    if (errno_of(syscall_chown(g, 0, 0)) != SYSCALL_EWINDOWS)
        testing_t_errorf_v(t, "Chown did not say EWINDOWS");
    if (syscall_getuid() != -1 || syscall_getegid() != -1)
        testing_t_errorf_v(t, "Getuid or Getegid is not -1");
    os_remove_all(dir);
}

static void TestPipe(TestingT *t) {
    SyscallHandle p[2];
    Error err = syscall_pipe((Slice){p, 1, 1, TYPE_BYTE});
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "Pipe of 1 = %v, want EINVAL", err);
    err = syscall_pipe((Slice){p, 2, 2, TYPE_BYTE});
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Pipe: %v", err);
    syscall_write(p[1], slice_from((void *)(uintptr_t)"abc", 3, 3, TYPE_BYTE), &err);
    syscall_close(p[1]);
    char buf[8];
    Int n = syscall_read(p[0], slice_from(buf, 8, 8, TYPE_BYTE), &err);
    if (n != 3 || memcmp(buf, "abc", 3) != 0)
        testing_t_errorf_v(t, "Read = %d, %v", n, err);
    /* The end of a pipe is ERROR_BROKEN_PIPE, which Read gives as 0, nil. */
    n = syscall_read(p[0], slice_from(buf, 8, 8, TYPE_BYTE), &err);
    if (n != 0 || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Read at the end = %d, %v", n, err);
    syscall_close(p[0]);
}

static void TestProcess(TestingT *t) {
    if (syscall_getpid() <= 0)
        testing_t_errorf_v(t, "Getpid = %d", syscall_getpid());
    Int ppid = syscall_getppid();
    if (ppid <= 0 || ppid == syscall_getpid())
        testing_t_errorf_v(t, "Getppid = %d", ppid);
    SyscallTimeval tv = {0, 0};
    syscall_gettimeofday(&tv);
    /* After 2020. */
    if (tv.sec < 1577836800)
        testing_t_errorf_v(t, "Gettimeofday = %d", (Int)tv.sec);
    /* The standard handles are the ones GetStdHandle gives. */
    Error err = BURROW_NO_ERROR;
    if (syscall_stderr != syscall_get_std_handle(SYSCALL_STD_ERROR_HANDLE, &err))
        testing_t_errorf_v(t, "Stderr is not STD_ERROR_HANDLE");
}

static void TestSockaddr(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    SyscallSockaddrInet4 sa4;
    memset(&sa4, 0, sizeof sa4);
    sa4.port = 0x10000;
    uint32_t len = 0;
    SyscallSockaddr s = syscall_sockaddr_inet4_as_sockaddr(&sa4);
    s.vt->sockaddr(s.data, &len, &err);
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "port 0x10000 = %v, want EINVAL", err);
    sa4.port = 0x1234;
    memcpy(sa4.addr, "\x7f\x00\x00\x01", 4);
    SyscallRawSockaddrAny *raw = s.vt->sockaddr(s.data, &len, &err);
    if (BURROW_FAILED(err) || len != sizeof(SyscallRawSockaddrInet4))
        testing_t_errorf_v(t, "Inet4 = %d, %v", (Int)len, err);
    SyscallRawSockaddrAny any;
    memset(&any, 0, sizeof any);
    memcpy(&any, raw, len);
    SyscallSockaddr back = syscall_raw_sockaddr_any_sockaddr(&any, a, &err);
    if (BURROW_FAILED(err) || back.vt->self_type != TYPE_SYSCALL_SOCKADDR_INET4 ||
        ((SyscallSockaddrInet4 *)back.data)->port != 0x1234)
        testing_t_errorf_v(t, "Inet4 back = %v", err);
    syscall_sockaddr_free(a, back);

    /* Unix names: a path needs room for its NUL, and an abstract one does
     * not. */
    SyscallSockaddrUnix su;
    memset(&su, 0, sizeof su);
    s = syscall_sockaddr_unix_as_sockaddr(&su);
    char name[108];
    memset(name, 'x', sizeof name);
    su.name = (Str){(const Byte *)name, 108};
    s.vt->sockaddr(s.data, &len, &err);
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "108-byte path = %v, want EINVAL", err);
    name[0] = '@';
    s.vt->sockaddr(s.data, &len, &err);
    if (BURROW_FAILED(err) || len != 110 || su.raw.path[0] != 0)
        testing_t_errorf_v(t, "108-byte abstract name = %d, %v", (Int)len, err);
    su.name = BURROW_S("/tmp/s");
    s.vt->sockaddr(s.data, &len, &err);
    if (len != 2 + 6 + 1)
        testing_t_errorf_v(t, "len of /tmp/s = %d", (Int)len);

    memset(&any, 0, sizeof any);
    any.addr.family = SYSCALL_AF_UNIX;
    memcpy(any.addr.data, "\0abc", 4);
    back = syscall_raw_sockaddr_any_sockaddr(&any, a, &err);
    if (BURROW_FAILED(err) ||
        !str_eq(((SyscallSockaddrUnix *)back.data)->name, BURROW_S("@abc")))
        testing_t_errorf_v(t, "abstract name back = %v", err);
    syscall_sockaddr_free(a, back);
    any.addr.family = 99;
    syscall_raw_sockaddr_any_sockaddr(&any, a, &err);
    if (errno_of(err) != SYSCALL_EAFNOSUPPORT)
        testing_t_errorf_v(t, "family 99 = %v, want EAFNOSUPPORT", err);
}

static void TestSocket(TestingT *t) {
    SyscallWSAData d;
    memset(&d, 0, sizeof d);
    Error err = syscall_wsa_startup(0x202, &d);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "WSAStartup: %v", err);
    SyscallHandle s =
        syscall_socket(SYSCALL_AF_INET, SYSCALL_SOCK_STREAM, SYSCALL_IPPROTO_TCP, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Socket: %v", err);
    SyscallSockaddrInet4 sa;
    memset(&sa, 0, sizeof sa);
    memcpy(sa.addr, "\x7f\x00\x00\x01", 4);
    err = syscall_bind(s, syscall_sockaddr_inet4_as_sockaddr(&sa));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Bind: %v", err);
    err = syscall_listen(s, 1);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Listen: %v", err);
    SyscallSockaddr name = syscall_getsockname(a, s, &err);
    if (BURROW_FAILED(err) || name.vt->self_type != TYPE_SYSCALL_SOCKADDR_INET4)
        testing_t_fatalf_v(t, "Getsockname: %v", err);
    SyscallSockaddrInet4 *got = name.data;
    if (got->port == 0 || memcmp(got->addr, "\x7f\x00\x00\x01", 4) != 0)
        testing_t_errorf_v(t, "Getsockname port %d", got->port);

    err = syscall_setsockopt_int(s, SYSCALL_SOL_SOCKET, SYSCALL_SO_REUSEADDR, 1);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "SetsockoptInt: %v", err);
    Int v = syscall_getsockopt_int(s, SYSCALL_SOL_SOCKET, SYSCALL_SO_REUSEADDR, &err);
    if (BURROW_FAILED(err) || v == 0)
        testing_t_errorf_v(t, "GetsockoptInt = %d, %v", v, err);
    SyscallLinger l = {1, 5};
    err = syscall_setsockopt_linger(s, SYSCALL_SOL_SOCKET, SYSCALL_SO_LINGER, &l);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "SetsockoptLinger: %v", err);

    /* A client through Connect, which Getpeername sees. */
    SyscallHandle c =
        syscall_socket(SYSCALL_AF_INET, SYSCALL_SOCK_STREAM, SYSCALL_IPPROTO_TCP, &err);
    err = syscall_connect(c, name);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Connect: %v", err);
    SyscallSockaddr peer = syscall_getpeername(a, c, &err);
    if (BURROW_FAILED(err) || ((SyscallSockaddrInet4 *)peer.data)->port != got->port)
        testing_t_errorf_v(t, "Getpeername: %v", err);
    syscall_sockaddr_free(a, peer);
    err = syscall_shutdown(c, SYSCALL_SHUT_RDWR);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Shutdown: %v", err);
    syscall_closesocket(c);

    /* What Go leaves unimplemented on Windows. */
    syscall_accept(a, s, NULL, &err);
    if (errno_of(err) != SYSCALL_EWINDOWS)
        testing_t_errorf_v(t, "Accept = %v, want EWINDOWS", err);

    err = syscall_load_connect_ex();
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "LoadConnectEx: %v", err);
    syscall_sockaddr_free(a, name);
    syscall_closesocket(s);

    syscall_socket_disable_ipv6 = true;
    syscall_socket(SYSCALL_AF_INET6, SYSCALL_SOCK_STREAM, 0, &err);
    syscall_socket_disable_ipv6 = false;
    if (errno_of(err) != SYSCALL_EAFNOSUPPORT)
        testing_t_errorf_v(t, "Socket(AF_INET6) with IPv6 off = %v", err);
    syscall_wsa_cleanup();
}

static void TestSID(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    SyscallSID *sid = syscall_string_to_sid(a, BURROW_S("S-1-5-32-544"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "StringToSid: %v", err);
    Str s = syscall_sid_string(sid, a, &err);
    if (BURROW_FAILED(err) || !str_eq(s, BURROW_S("S-1-5-32-544")))
        testing_t_errorf_v(t, "String = %q, %v", s, err);
    /* S-1-5-32-544 has two sub-authorities: 8 bytes and 4 for each. */
    if (syscall_sid_len(sid) != 16)
        testing_t_errorf_v(t, "Len = %d, want 16", syscall_sid_len(sid));
    SyscallSID *c = syscall_sid_copy(sid, a, &err);
    if (BURROW_FAILED(err) || memcmp(c, sid, 16) != 0)
        testing_t_errorf_v(t, "Copy: %v", err);
    syscall_sid_free(a, c);
    Str domain = BURROW_STR_EMPTY;
    uint32_t typ = 0;
    Str acc = syscall_sid_lookup_account(sid, a, BURROW_S(""), &domain, &typ, &err);
    if (BURROW_FAILED(err) || acc.len == 0)
        testing_t_errorf_v(t, "LookupAccount = %q, %q, %v", acc, domain, err);
    else {
        /* And back the other way. */
        Str full = fmt_sprintf_v(a, "%s\\%s", domain, acc);
        SyscallSID *again = syscall_lookup_sid(a, BURROW_S(""), full, NULL, NULL, &err);
        if (BURROW_FAILED(err) || memcmp(again, sid, 16) != 0)
            testing_t_errorf_v(t, "LookupSID(%q): %v", full, err);
        syscall_sid_free(a, again);
    }
    syscall_sid_free(a, sid);

    syscall_lookup_sid(a, BURROW_S(""), BURROW_S(""), NULL, NULL, &err);
    if (errno_of(err) != SYSCALL_EINVAL)
        testing_t_errorf_v(t, "LookupSID of nothing = %v, want EINVAL", err);
    syscall_string_to_sid(a, BURROW_S("not a sid"), &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "StringToSid(not a sid) worked");
}

static void TestToken(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    SyscallToken tok = syscall_open_current_process_token(&err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "OpenCurrentProcessToken: %v", err);
    SyscallTokenuser *u = syscall_token_get_token_user(tok, a, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "GetTokenUser: %v", err);
    Str s = syscall_sid_string(u->user.sid, a, &err);
    if (BURROW_FAILED(err) || s.len < 4 || memcmp(s.p, "S-1-", 4) != 0)
        testing_t_errorf_v(t, "user SID = %q, %v", s, err);
    syscall_token_info_free(a, u);
    SyscallTokenprimarygroup *g = syscall_token_get_token_primary_group(tok, a, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "GetTokenPrimaryGroup: %v", err);
    else
        syscall_token_info_free(a, g);
    Str dir = syscall_token_get_user_profile_directory(tok, a, &err);
    if (BURROW_FAILED(err) || dir.len == 0)
        testing_t_errorf_v(t, "GetUserProfileDirectory = %q, %v", dir, err);
    err = syscall_token_close(tok);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Close: %v", err);
}

static void TestMisc(TestingT *t) {
    /* The OIDs end in a NUL, as Go's do. */
    if (syscall_oid_pkix_kp_server_auth.len != 18 ||
        memcmp(syscall_oid_pkix_kp_server_auth.p, "1.3.6.1.5.5.7.3.1", 18) != 0)
        testing_t_errorf_v(t, "OID_PKIX_KP_SERVER_AUTH is wrong");
    if (syscall_oid_sgc_netscape.len != 22)
        testing_t_errorf_v(t, "OID_SGC_NETSCAPE has %d bytes",
                           syscall_oid_sgc_netscape.len);
    Error err = syscall_load_cancel_io_ex();
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "LoadCancelIoEx: %v", err);
    err = syscall_load_get_addr_info();
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "LoadGetAddrInfo: %v", err);

    /* A completion port with one packet posted to it. */
    SyscallHandle cp =
        syscall_create_io_completion_port(SYSCALL_INVALID_HANDLE, 0, 0, 1, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "CreateIoCompletionPort: %v", err);
    err = syscall_post_queued_completion_status(cp, 7, 42, NULL);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "PostQueuedCompletionStatus: %v", err);
    uint32_t qty = 0, key = 0;
    SyscallOverlapped *ov = NULL;
    err = syscall_get_queued_completion_status(cp, &qty, &key, &ov, 1000);
    if (BURROW_FAILED(err) || qty != 7 || key != 42)
        testing_t_errorf_v(t, "GetQueuedCompletionStatus = %d, %d, %v", (Int)qty,
                           (Int)key, err);
    syscall_close_handle(cp);
}

#define TESTS(X)                                                                       \
    X(TestOpen)                                                                        \
    X(TestComputerName)                                                                \
    X(TestWin32finddata)                                                               \
    X(TestTOKEN_ALL_ACCESS)                                                            \
    X(TestGetwd_DoesNotPanicWhenPathIsLong)                                            \
    X(TestGetStartupInfo)                                                              \
    X(TestReadWriteSeek)                                                               \
    X(TestFileOps)                                                                     \
    X(TestPipe)                                                                        \
    X(TestProcess)                                                                     \
    X(TestSockaddr)                                                                    \
    X(TestSocket)                                                                      \
    X(TestSID)                                                                         \
    X(TestToken)                                                                       \
    X(TestMisc)

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

static int windows_main(TestingM *m) {
    setup();
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

#else

static void TestNothing(TestingT *t) {
    testing_t_skip_v(t, "these calls are Windows only");
}

#define TESTS(X) X(TestNothing)

static int windows_main(TestingM *m) {
    return testing_m_run(m);
}

#endif

TESTING_MAIN_WITH(windows_main, TESTS)
