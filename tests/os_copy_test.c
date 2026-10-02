/* Derived from Go's src/os/os_test.go, readfrom_linux_test.go,
 * writeto_linux_test.go and rawconn_test.go.
 * Go source: go1.27.1.
 *
 * CopyFS, File.ReadFrom and File.WriteTo, and File.SyscallConn. Go's CopyFS
 * tests copy out of a fstest.MapFS, which is not here yet, so these copy out
 * of a DirFS over a tree the test writes first.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"
#include "burrow/syscall.h"

#include "check.h"

#include <string.h>

/* ------------------------------------------------------------- helpers */

#define S(lit) BURROW_S(lit)

#define CHECK_S(got, want)                                                             \
    do {                                                                               \
        Str got_ = (got), want_ = (want);                                              \
        if (!str_eq(got_, want_))                                                      \
            testing_t_errorf_v(t, "%s:%d: got %q, want %q", str_from_cstr(__FILE__),   \
                               (Int)__LINE__, got_, want_);                            \
    } while (0)

static Arena ar;
static Alloc *a;

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Str str_of(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

static Str temp_dir(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Str d = os_mkdir_temp(a, S(""), S("burrow-os-copy-test-*"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "MkdirTemp: %s", error_text(e));
    return d;
}

static void cleanup(TestingT *t, Str d) {
    Error e = os_remove_all(d);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "RemoveAll %s: %s", d, error_text(e));
}

static Str join(Str d, const char *name) {
    return fmt_sprintf_v(a, "%s%c%s", d, (Int)OS_PATH_SEPARATOR, str_from_cstr(name));
}

static void write_file(TestingT *t, Str name, const char *data, OsFileMode perm) {
    Error e = os_write_file(name, bytes_of(str_from_cstr(data)), perm);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "WriteFile %s: %s", name, error_text(e));
}

static Str read_file(TestingT *t, Str name) {
    Error e = BURROW_NO_ERROR;
    Slice b = os_read_file(a, name, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "ReadFile %s: %s", name, error_text(e));
    return str_of(b);
}

static void mkdir_d(TestingT *t, Str name) {
    Error e = os_mkdir(name, 0755);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Mkdir %s: %s", name, error_text(e));
}

/* --------------------------------------------------------------- CopyFS */

static void TestCopyFS(TestingT *t) {
    Str src = temp_dir(t);
    mkdir_d(t, join(src, "dir"));
    mkdir_d(t, join(src, "dir/empty"));
    write_file(t, join(src, "hello.txt"), "hello, world\n", 0644);
    write_file(t, join(src, "dir/run.sh"), "#!/bin/sh\n", 0755);
    write_file(t, join(src, "dir/nothing"), "", 0644);

    Str dst = join(temp_dir(t), "copy");
    Error e = os_copy_fs(dst, os_dir_fs(a, src));
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "CopyFS: %s", error_text(e));

    CHECK_S(read_file(t, join(dst, "hello.txt")), S("hello, world\n"));
    CHECK_S(read_file(t, join(dst, "dir/run.sh")), S("#!/bin/sh\n"));
    CHECK_S(read_file(t, join(dst, "dir/nothing")), S(""));
    OsFileInfo fi = os_stat(a, join(dst, "dir/empty"), &e);
    CHECK(BURROW_OK(e) && fi.vt->is_dir(fi.data));
#if !defined(BURROW_OS_WINDOWS)
    /* 0666 | mode&0777, so the execute bits come across. */
    fi = os_stat(a, join(dst, "dir/run.sh"), &e);
    CHECK(BURROW_OK(e) && (fi.vt->mode(fi.data) & 0100) != 0);
    fi = os_stat(a, join(dst, "hello.txt"), &e);
    CHECK(BURROW_OK(e) && (fi.vt->mode(fi.data) & 0111) == 0);
#endif

    /* A second copy over the first fails, since files are created with
     * O_EXCL, and nothing already there changes. */
    write_file(t, join(src, "hello.txt"), "changed\n", 0644);
    e = os_copy_fs(dst, os_dir_fs(a, src));
    if (!os_is_exist(e))
        testing_t_errorf_v(t, "second CopyFS: got %s, want a file exists error",
                           error_text(e));
    CHECK_S(read_file(t, join(dst, "hello.txt")), S("hello, world\n"));

    cleanup(t, src);
    cleanup(t, str_from_bytes(dst.p, dst.len - 5));
}

static void TestCopyFSMissing(TestingT *t) {
    Str src = temp_dir(t);
    Str dst = temp_dir(t);
    Error e = os_copy_fs(dst, os_dir_fs(a, join(src, "nope")));
    if (!os_is_not_exist(e))
        testing_t_errorf_v(t, "CopyFS of a missing dir: got %s, want not exist",
                           error_text(e));
    cleanup(t, src);
    cleanup(t, dst);
}

static void TestCopyFSWithSymlinks(TestingT *t) {
#if defined(BURROW_OS_WINDOWS)
    testing_t_skip_v(t, "symlinks need privileges on Windows");
#else
    Str src = temp_dir(t);
    write_file(t, join(src, "file"), "data", 0644);
    Error e = os_symlink(S("file"), join(src, "link"));
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Symlink: %s", error_text(e));
    e = os_symlink(S("../outside"), join(src, "out"));
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Symlink: %s", error_text(e));

    Str dst = join(temp_dir(t), "copy");
    e = os_copy_fs(dst, os_dir_fs(a, src));
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "CopyFS: %s", error_text(e));
    /* The links are copied as links, with their targets as they were. */
    Str got = os_readlink(a, join(dst, "link"), &e);
    CHECK(BURROW_OK(e));
    CHECK_S(got, S("file"));
    got = os_readlink(a, join(dst, "out"), &e);
    CHECK(BURROW_OK(e));
    CHECK_S(got, S("../outside"));
    CHECK_S(read_file(t, join(dst, "link")), S("data"));

    cleanup(t, src);
    cleanup(t, str_from_bytes(dst.p, dst.len - 5));
#endif
}

/* ------------------------------------------------- ReadFrom and WriteTo */

static OsFile *create(TestingT *t, Str name) {
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_create(a, name, &e);
    if (f == NULL)
        testing_t_fatalf_v(t, "Create %s: %s", name, error_text(e));
    return f;
}

static OsFile *open_f(TestingT *t, Str name) {
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_open(a, name, &e);
    if (f == NULL)
        testing_t_fatalf_v(t, "Open %s: %s", name, error_text(e));
    return f;
}

static void TestReadFrom(TestingT *t) {
    Str d = temp_dir(t);
    OsFile *f = create(t, join(d, "out"));
    StringsReader r;
    strings_reader_reset(&r, S("some text to copy in"));
    Error e = BURROW_NO_ERROR;
    int64_t n = os_file_read_from(f, strings_reader_as_io_reader(&r), &e);
    CHECK(BURROW_OK(e));
    CHECK_INT_EQ(n, 20);

    /* It writes at the file's offset, after whatever came before. */
    strings_reader_reset(&r, S(", and more"));
    n = os_file_read_from(f, strings_reader_as_io_reader(&r), &e);
    CHECK(BURROW_OK(e));
    CHECK_INT_EQ(n, 10);
    CHECK(BURROW_OK(os_file_close(f)));
    CHECK_S(read_file(t, join(d, "out")), S("some text to copy in, and more"));

    /* On a closed file the write inside fails, unwrapped by ReadFrom. */
    strings_reader_reset(&r, S("late"));
    n = os_file_read_from(f, strings_reader_as_io_reader(&r), &e);
    CHECK_INT_EQ(n, 0);
    CHECK(errors_is(e, os_err_closed));
    os_file_free(f);
    cleanup(t, d);
}

static void TestWriteTo(TestingT *t) {
    Str d = temp_dir(t);
    write_file(t, join(d, "in"), "0123456789", 0644);
    OsFile *f = open_f(t, join(d, "in"));
    Error e = BURROW_NO_ERROR;
    os_file_seek(f, 4, OS_SEEK_SET, &e);
    CHECK(BURROW_OK(e));

    BytesBuffer b = BYTES_BUFFER(a);
    int64_t n = os_file_write_to(f, bytes_buffer_as_io_writer(&b), &e);
    CHECK(BURROW_OK(e));
    CHECK_INT_EQ(n, 6);
    CHECK_S(bytes_buffer_string(&b, a), S("456789"));

    /* At the end there is nothing left, and that is not an error. */
    n = os_file_write_to(f, bytes_buffer_as_io_writer(&b), &e);
    CHECK(BURROW_OK(e));
    CHECK_INT_EQ(n, 0);
    bytes_buffer_free(&b);
    CHECK(BURROW_OK(os_file_close(f)));
    os_file_free(f);
    cleanup(t, d);
}

/* io_copy finds WriteTo on the source file, and ReadFrom on the destination
 * when the source has no WriteTo of its own. */
static void TestCopyBetweenFiles(TestingT *t) {
    Str d = temp_dir(t);
    char big[100000];
    for (size_t i = 0; i < sizeof big - 1; i++)
        big[i] = (char)('a' + i % 26);
    big[sizeof big - 1] = 0;
    write_file(t, join(d, "src"), big, 0644);

    OsFile *src = open_f(t, join(d, "src"));
    OsFile *dst = create(t, join(d, "dst"));
    Error e = BURROW_NO_ERROR;
    int64_t n = io_copy(a, os_file_as_io_writer(dst), os_file_as_io_reader(src), &e);
    CHECK(BURROW_OK(e));
    CHECK_INT_EQ(n, (int64_t)(sizeof big - 1));
    CHECK(BURROW_OK(os_file_close(dst)));
    CHECK(BURROW_OK(os_file_close(src)));
    os_file_free(dst);
    os_file_free(src);
    CHECK_S(read_file(t, join(d, "dst")), str_from_cstr(big));
    cleanup(t, d);
}

static void TestNilFile(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    StringsReader r;
    strings_reader_reset(&r, S("x"));
    CHECK_INT_EQ(os_file_read_from(NULL, strings_reader_as_io_reader(&r), &e), 0);
    CHECK(errors_is(e, os_err_invalid));
    BytesBuffer b = BYTES_BUFFER(a);
    CHECK_INT_EQ(os_file_write_to(NULL, bytes_buffer_as_io_writer(&b), &e), 0);
    CHECK(errors_is(e, os_err_invalid));
    SyscallRawConn c = os_file_syscall_conn(NULL, &e);
    CHECK(c.vt == NULL);
    CHECK(errors_is(e, os_err_invalid));
}

/* ----------------------------------------------------------- SyscallConn */

static void save_fd(void *env, Uintptr fd) {
    *(Uintptr *)env = fd;
}

static bool done_fd(void *env, Uintptr fd) {
    *(Uintptr *)env = fd;
    return true;
}

static bool not_ready(void *env, Uintptr fd) {
    (void)env;
    (void)fd;
    return false;
}

static void TestRawConn(TestingT *t) {
    Str d = temp_dir(t);
    OsFile *f = create(t, join(d, "f"));
    Error e = BURROW_NO_ERROR;
    SyscallRawConn c = os_file_syscall_conn(f, &e);
    CHECK(BURROW_OK(e));

    Uintptr got = 0;
    e = c.vt->control(c.data, (SyscallFdFunc){save_fd, &got});
    CHECK(BURROW_OK(e));
    CHECK(got == os_file_fd(f));

    got = 0;
    e = c.vt->read(c.data, (SyscallFdDoneFunc){done_fd, &got});
    CHECK(BURROW_OK(e));
    CHECK(got == os_file_fd(f));
    got = 0;
    e = c.vt->write(c.data, (SyscallFdDoneFunc){done_fd, &got});
    CHECK(BURROW_OK(e));
    CHECK(got == os_file_fd(f));

    /* A disk file cannot be waited on, so a callback that wants to wait
     * gets an error back rather than a hang. */
    e = c.vt->read(c.data, (SyscallFdDoneFunc){not_ready, NULL});
    CHECK(BURROW_FAILED(e));
    e = c.vt->write(c.data, (SyscallFdDoneFunc){not_ready, NULL});
    CHECK(BURROW_FAILED(e));

    CHECK(BURROW_OK(os_file_close(f)));
    e = c.vt->control(c.data, (SyscallFdFunc){save_fd, &got});
    CHECK(BURROW_FAILED(e));
    CHECK_S(error_text(e), S("use of closed file"));
    e = c.vt->read(c.data, (SyscallFdDoneFunc){done_fd, &got});
    CHECK_S(error_text(e), S("use of closed file"));
    os_file_free(f);
    cleanup(t, d);
}

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

#define TESTS(X)                                                                       \
    X(TestCopyFS)                                                                      \
    X(TestCopyFSMissing)                                                               \
    X(TestCopyFSWithSymlinks)                                                          \
    X(TestReadFrom)                                                                    \
    X(TestWriteTo)                                                                     \
    X(TestCopyBetweenFiles)                                                            \
    X(TestNilFile)                                                                     \
    X(TestRawConn)

static int os_copy_main(TestingM *m) {
    setup();
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(os_copy_main, TESTS)
