/* Derived from Go's src/os/os_test.go.
 * Go source: go1.27.1.
 *
 * The tests here are the ones for what burrow has of os so far: File, Stat,
 * the path functions and the error predicates. They make their files under a
 * directory of their own in the system's temporary directory, since burrow has
 * no MkdirTemp yet, and take it apart again with os_remove.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/strings.h"
#include "burrow/syscall.h"

#include "check.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- helpers */

#define S(lit) BURROW_S(lit)

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Arena ar;
static Alloc *a;
static char dir_buf[1024];
static Str dir;
static int dir_seq;

/* A fresh empty directory for one test. */
static void make_dir(TestingT *t) {
    char tmp[900];
    PalErrno pe = PAL_OK;
    int64_t n = pal_temp_dir(tmp, (int64_t)sizeof tmp, &pe);
    if (n < 0)
        testing_t_fatalf_v(t, "temp dir: %s", str_from_cstr(pal_errno_string(pe)));
    snprintf(dir_buf, sizeof dir_buf, "%s%cburrow-os-test-%lld-%d", tmp,
             OS_PATH_SEPARATOR, (long long)pal_getpid(), dir_seq++);
    dir = str_from_cstr(dir_buf);
    Error e = os_mkdir(dir, 0700);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "%s", error_text(e));
}

static Str path_in(const char *name) {
    return fmt_sprintf_v(a, "%s%c%s", dir, (Int)OS_PATH_SEPARATOR, str_from_cstr(name));
}

/* Removes what the test made, one level deep, which is all any of them
 * makes. */
static void remove_dir(TestingT *t, const char *const *names, int n) {
    for (int i = 0; i < n; i++)
        os_remove(path_in(names[i]));
    Error e = os_remove(dir);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "removing the test directory: %s", error_text(e));
}

static void write_file(TestingT *t, Str name, const char *data) {
    Error e = os_write_file(name, bytes_of(str_from_cstr(data)), 0644);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "WriteFile %s: %s", name, error_text(e));
}

static Str read_file(TestingT *t, Str name) {
    Error e = BURROW_NO_ERROR;
    Slice b = os_read_file(a, name, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "ReadFile %s: %s", name, error_text(e));
    return str_from_bytes((const Byte *)b.p, b.len);
}

static void check_ok(TestingT *t, const char *what, Error e) {
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "%s: %s", str_from_cstr(what), error_text(e));
}

/* The SyscallErrno inside err, or 0. */
static SyscallErrno errno_of(Error err) {
    const SyscallErrno *e = (const SyscallErrno *)errors_as(err, TYPE_SYSCALL_ERRNO);
    return e != NULL ? *e : 0;
}

static OsFile *create(TestingT *t, Str name) {
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_create(a, name, &e);
    if (f == NULL)
        testing_t_fatalf_v(t, "Create %s: %s", name, error_text(e));
    return f;
}

static void close_free(OsFile *f) {
    os_file_close(f);
    os_file_free(f);
}

/* --------------------------------------------------------------- tests */

static void TestStatError(TestingT *t) {
    make_dir(t);
    Str path = path_in("no-such-file");
    Error e = BURROW_NO_ERROR;
    OsFileInfo fi = os_stat(a, path, &e);
    CHECK(fi.vt == NULL);
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    if (pe == NULL) {
        testing_t_errorf_v(t, "os_stat of a missing file: %s, want a PathError",
                           error_text(e));
    } else {
        CHECK(str_eq(pe->op, S("stat")));
        CHECK(str_eq(pe->path, path));
    }
    CHECK(os_is_not_exist(e));
    CHECK(errors_is(e, os_err_not_exist));

    os_lstat(a, path, &e);
    pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL && str_eq(pe->op, S("lstat")));
    remove_dir(t, NULL, 0);
}

static void TestStat(TestingT *t) {
    make_dir(t);
    Str path = path_in("hello.txt");
    write_file(t, path, "hello, world\n");
    Error e = BURROW_NO_ERROR;
    OsFileInfo fi = os_stat(a, path, &e);
    check_ok(t, "Stat", e);
    if (fi.vt != NULL) {
        CHECK_STR_EQ(str_to_cstr(a, fi.vt->name(fi.data)), "hello.txt");
        CHECK_INT_EQ(fi.vt->size(fi.data), 13);
        CHECK(!fi.vt->is_dir(fi.data));
        CHECK((fi.vt->mode(fi.data) & FS_MODE_TYPE) == 0);
    }

    OsFileInfo di = os_stat(a, dir, &e);
    check_ok(t, "Stat dir", e);
    if (di.vt != NULL) {
        CHECK(di.vt->is_dir(di.data));
        CHECK((di.vt->mode(di.data) & FS_MODE_DIR) != 0);
    }

    /* A trailing separator does not change the name. */
    Str slashed = fmt_sprintf_v(a, "%s%c", dir, (Int)OS_PATH_SEPARATOR);
    OsFileInfo si = os_stat(a, slashed, &e);
    check_ok(t, "Stat dir/", e);
    if (si.vt != NULL && di.vt != NULL)
        CHECK(str_eq(si.vt->name(si.data), di.vt->name(di.data)));

    const char *names[] = {"hello.txt"};
    remove_dir(t, names, 1);
}

static void TestFstat(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    write_file(t, path, "0123456789");
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_open(a, path, &e);
    if (f == NULL)
        testing_t_fatalf_v(t, "Open: %s", error_text(e));
    OsFileInfo fi = os_file_stat(f, a, &e);
    check_ok(t, "File.Stat", e);
    OsFileInfo di = os_stat(a, path, &e);
    check_ok(t, "Stat", e);
    if (fi.vt != NULL && di.vt != NULL) {
        CHECK(str_eq(fi.vt->name(fi.data), di.vt->name(di.data)));
        CHECK_INT_EQ(fi.vt->size(fi.data), 10);
        CHECK(fi.vt->mode(fi.data) == di.vt->mode(di.data));
        CHECK(os_same_file(fi, di));
    }
    close_free(f);
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestSameFile(TestingT *t) {
    make_dir(t);
    Str a1 = path_in("a"), b1 = path_in("b");
    write_file(t, a1, "a");
    write_file(t, b1, "b");
    Error e = BURROW_NO_ERROR;
    OsFileInfo ia1 = os_stat(a, a1, &e);
    OsFileInfo ia2 = os_stat(a, a1, &e);
    OsFileInfo ib = os_stat(a, b1, &e);
    check_ok(t, "Stat", e);
    CHECK(os_same_file(ia1, ia2));
    CHECK(!os_same_file(ia1, ib));
    const char *names[] = {"a", "b"};
    remove_dir(t, names, 2);
}

static void TestRead0(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    write_file(t, path, "x");
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_open(a, path, &e);
    if (f == NULL)
        testing_t_fatalf_v(t, "Open: %s", error_text(e));
    Byte b[100];
    Int n = os_file_read(f, slice_from(b, 0, 0, TYPE_BYTE), &e);
    if (n != 0 || BURROW_FAILED(e))
        testing_t_errorf_v(t, "Read(0) = %d, %s, want 0, nil", n, error_text(e));
    n = os_file_read(f, slice_from(b, 100, 100, TYPE_BYTE), &e);
    if (n <= 0 || BURROW_FAILED(e))
        testing_t_errorf_v(t, "Read(100) = %d, %s, want >0, nil", n, error_text(e));
    n = os_file_read(f, slice_from(b, 100, 100, TYPE_BYTE), &e);
    if (n != 0 || !errors_is(e, io_eof))
        testing_t_errorf_v(t, "Read at the end = %d, %s, want 0, EOF", n,
                           error_text(e));
    close_free(f);
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestReadAt(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    OsFile *f = create(t, path);
    Error e = BURROW_NO_ERROR;
    os_file_write_string(f, S("hello, world\n"), &e);
    check_ok(t, "WriteString", e);

    Byte b[5];
    Int n = os_file_read_at(f, slice_from(b, 5, 5, TYPE_BYTE), 7, &e);
    if (BURROW_FAILED(e) || n != 5 || memcmp(b, "world", 5) != 0)
        testing_t_errorf_v(t, "ReadAt 7: %d, %s, %q", n, error_text(e),
                           str_from_bytes(b, n < 0 ? 0 : n));

    /* ReadAt does not move the offset Read uses. */
    os_file_seek(f, 0, OS_SEEK_SET, &e);
    os_file_read_at(f, slice_from(b, 5, 5, TYPE_BYTE), 7, &e);
    n = os_file_read(f, slice_from(b, 5, 5, TYPE_BYTE), &e);
    if (BURROW_FAILED(e) || n != 5 || memcmp(b, "hello", 5) != 0)
        testing_t_errorf_v(t, "Read after ReadAt: %d, %s, %q", n, error_text(e),
                           str_from_bytes(b, n < 0 ? 0 : n));

    /* Past the end is a short read and EOF. */
    n = os_file_read_at(f, slice_from(b, 5, 5, TYPE_BYTE), 10, &e);
    if (n != 3 || !errors_is(e, io_eof))
        testing_t_errorf_v(t, "ReadAt 10 = %d, %s, want 3, EOF", n, error_text(e));
    close_free(f);
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestReadAtNegativeOffset(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    OsFile *f = create(t, path);
    Error e = BURROW_NO_ERROR;
    Byte b[5];
    Int n = os_file_read_at(f, slice_from(b, 5, 5, TYPE_BYTE), -10, &e);
    Str want = S("negative offset");
    if (n != 0 || !strings_contains(error_text(e), want))
        testing_t_errorf_v(t, "ReadAt(-10) = %d, %s, want 0, ...%s...", n,
                           error_text(e), want);
    close_free(f);
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestWriteAt(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    OsFile *f = create(t, path);
    Error e = BURROW_NO_ERROR;
    os_file_write_string(f, S("hello, world\n"), &e);
    Int n = os_file_write_at(f, bytes_of(S("WORLD")), 7, &e);
    if (n != 5 || BURROW_FAILED(e))
        testing_t_fatalf_v(t, "WriteAt 7: %d, %s", n, error_text(e));
    close_free(f);
    CHECK_STR_EQ(str_to_cstr(a, read_file(t, path)), "hello, WORLD\n");
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestWriteAtNegativeOffset(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    OsFile *f = create(t, path);
    Error e = BURROW_NO_ERROR;
    Int n = os_file_write_at(f, bytes_of(S("WORLD")), -10, &e);
    Str want = S("negative offset");
    if (n != 0 || !strings_contains(error_text(e), want))
        testing_t_errorf_v(t, "WriteAt(-10) = %d, %s, want 0, ...%s...", n,
                           error_text(e), want);
    close_free(f);
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestWriteAtInAppendMode(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_open_file(a, path, OS_O_APPEND | OS_O_CREATE, 0666, &e);
    if (f == NULL)
        testing_t_fatalf_v(t, "OpenFile: %s", error_text(e));
    os_file_write_at(f, bytes_of(S("")), 1, &e);
    CHECK_STR_EQ(str_to_cstr(a, error_text(e)),
                 "os: invalid use of WriteAt on file opened with O_APPEND");
    close_free(f);
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestSeek(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    OsFile *f = create(t, path);
    Error e = BURROW_NO_ERROR;
    os_file_write_string(f, S("hello, world\n"), &e);
    check_ok(t, "WriteString", e);

    static const struct {
        int64_t in;
        int whence;
        int64_t out;
    } tests[] = {
        {0, OS_SEEK_CUR, 13},
        {0, OS_SEEK_SET, 0},
        {5, OS_SEEK_SET, 5},
        {0, OS_SEEK_END, 13},
        {0, OS_SEEK_SET, 0},
        {-1, OS_SEEK_END, 12},
        {(int64_t)1 << 33, OS_SEEK_SET, (int64_t)1 << 33},
        {(int64_t)1 << 33, OS_SEEK_END, ((int64_t)1 << 33) + 13},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int64_t off = os_file_seek(f, tests[i].in, tests[i].whence, &e);
        if (BURROW_FAILED(e) || off != tests[i].out)
            testing_t_errorf_v(t, "#%d: Seek(%d, %d) = %d, %s, want %d", (Int)i,
                               tests[i].in, (Int)tests[i].whence, off, error_text(e),
                               tests[i].out);
    }
    close_free(f);
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void check_size(TestingT *t, OsFile *f, int64_t want) {
    Arena ar2;
    arena_init(&ar2, NULL, 0);
    Error e = BURROW_NO_ERROR;
    OsFileInfo fi = os_file_stat(f, arena_allocator(&ar2), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Stat %s: %s", os_file_name(f), error_text(e));
    if (fi.vt->size(fi.data) != want)
        testing_t_errorf_v(t, "Stat %q: size %d want %d", os_file_name(f),
                           fi.vt->size(fi.data), want);
    arena_free(&ar2);
}

static void TestFTruncate(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    OsFile *f = create(t, path);
    Error e = BURROW_NO_ERROR;
    check_size(t, f, 0);
    os_file_write(f, bytes_of(S("hello, world\n")), &e);
    check_size(t, f, 13);
    check_ok(t, "Truncate", os_file_truncate(f, 10));
    check_size(t, f, 10);
    check_ok(t, "Truncate", os_file_truncate(f, 1024));
    check_size(t, f, 1024);
    check_ok(t, "Truncate", os_file_truncate(f, 0));
    check_size(t, f, 0);
    os_file_write(f, bytes_of(S("surprise!")), &e);
    check_size(t, f, 13 + 9); /* wrote at offset past where hello, world was. */
    close_free(f);
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestTruncate(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    write_file(t, path, "hello, world\n");
    check_ok(t, "Truncate", os_truncate(path, 5));
    CHECK_STR_EQ(str_to_cstr(a, read_file(t, path)), "hello");
    check_ok(t, "Truncate", os_truncate(path, 0));
    CHECK_INT_EQ(read_file(t, path).len, 0);
    Error e = os_truncate(path_in("missing"), 0);
    CHECK(os_is_not_exist(e));
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestReadWriteFile(TestingT *t) {
    make_dir(t);
    Str path = path_in("big");
    /* Bigger than the 512 byte first buffer, to make the read grow. */
    Str data = strings_repeat(a, S("0123456789abcdef"), 1000);
    check_ok(t, "WriteFile", os_write_file(path, bytes_of(data), 0644));
    Str got = read_file(t, path);
    CHECK(str_eq(got, data));

    /* WriteFile truncates what was there. */
    check_ok(t, "WriteFile", os_write_file(path, bytes_of(S("short")), 0644));
    CHECK_STR_EQ(str_to_cstr(a, read_file(t, path)), "short");

    Error e = BURROW_NO_ERROR;
    os_read_file(a, path_in("missing"), &e);
    CHECK(os_is_not_exist(e));
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL && str_eq(pe->op, S("open")));
    const char *names[] = {"big"};
    remove_dir(t, names, 1);
}

static void TestCloseTwice(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    OsFile *f = create(t, path);
    check_ok(t, "Close", os_file_close(f));
    Error e = os_file_close(f);
    CHECK(errors_is(e, os_err_closed));
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL && str_eq(pe->op, S("close")));

    Byte b[4];
    os_file_read(f, slice_from(b, 4, 4, TYPE_BYTE), &e);
    CHECK(errors_is(e, os_err_closed));
    os_file_write(f, slice_from(b, 4, 4, TYPE_BYTE), &e);
    CHECK(errors_is(e, os_err_closed));
    CHECK_STR_EQ(str_to_cstr(a, error_text(os_file_set_deadline(f, (Time){0}))),
                 "use of closed file");
    os_file_free(f);
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestNoDeadline(TestingT *t) {
    make_dir(t);
    OsFile *f = create(t, path_in("f"));
    CHECK(errors_is(os_file_set_deadline(f, (Time){0}), os_err_no_deadline));
    CHECK(errors_is(os_file_set_read_deadline(f, (Time){0}), os_err_no_deadline));
    CHECK(errors_is(os_file_set_write_deadline(f, (Time){0}), os_err_no_deadline));
    close_free(f);
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestOpenFileExcl(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    write_file(t, path, "x");
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_open_file(a, path, OS_O_RDWR | OS_O_CREATE | OS_O_EXCL, 0666, &e);
    CHECK(f == NULL);
    CHECK(os_is_exist(e));
    CHECK(errors_is(e, os_err_exist));
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestRemove(TestingT *t) {
    make_dir(t);
    Str file = path_in("f");
    Str sub = path_in("sub");
    write_file(t, file, "x");
    check_ok(t, "Mkdir", os_mkdir(sub, 0755));
    check_ok(t, "Remove file", os_remove(file));
    check_ok(t, "Remove dir", os_remove(sub));
    Error e = os_remove(file);
    CHECK(os_is_not_exist(e));
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL && str_eq(pe->op, S("remove")));

    /* A directory with something in it stays. */
    check_ok(t, "Mkdir", os_mkdir(sub, 0755));
    write_file(t, path_in("sub/x"), "x");
    e = os_remove(sub);
    CHECK(BURROW_FAILED(e));
    CHECK(os_is_exist(e));
    check_ok(t, "Remove sub/x", os_remove(path_in("sub/x")));
    const char *names[] = {"sub"};
    remove_dir(t, names, 1);
}

static void TestMkdirExists(TestingT *t) {
    make_dir(t);
    Error e = os_mkdir(dir, 0755);
    CHECK(os_is_exist(e));
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL && str_eq(pe->op, S("mkdir")));
    remove_dir(t, NULL, 0);
}

static void TestRename(TestingT *t) {
    make_dir(t);
    Str from = path_in("renamefrom"), to = path_in("renameto");
    write_file(t, from, "x");
    check_ok(t, "Rename", os_rename(from, to));
    Error e = BURROW_NO_ERROR;
    os_stat(a, to, &e);
    check_ok(t, "Stat", e);
    os_stat(a, from, &e);
    CHECK(os_is_not_exist(e));
    const char *names[] = {"renameto"};
    remove_dir(t, names, 1);
}

static void TestRenameOverwriteDest(TestingT *t) {
    make_dir(t);
    Str from = path_in("renamefrom"), to = path_in("renameto");
    write_file(t, from, "from");
    write_file(t, to, "to");
    check_ok(t, "Rename", os_rename(from, to));
    CHECK_STR_EQ(str_to_cstr(a, read_file(t, to)), "from");
    const char *names[] = {"renameto"};
    remove_dir(t, names, 1);
}

static void TestRenameFailed(TestingT *t) {
    make_dir(t);
    Str from = path_in("from"), to = path_in("to");
    Error e = os_rename(from, to);
    const OsLinkError *le = (const OsLinkError *)errors_as(e, TYPE_OS_LINK_ERROR);
    if (le == NULL) {
        testing_t_errorf_v(t, "rename %q, %q: %s, want a LinkError", from, to,
                           error_text(e));
    } else {
        CHECK(str_eq(le->op, S("rename")));
        CHECK(str_eq(le->old, from));
        CHECK(str_eq(le->new_, to));
    }
    CHECK(os_is_not_exist(e));
    remove_dir(t, NULL, 0);
}

static void TestRenameToDirFailed(TestingT *t) {
    make_dir(t);
    Str from = path_in("from"), to = path_in("to");
    check_ok(t, "Mkdir", os_mkdir(from, 0777));
    check_ok(t, "Mkdir", os_mkdir(to, 0777));
    Error e = os_rename(from, to);
    const OsLinkError *le = (const OsLinkError *)errors_as(e, TYPE_OS_LINK_ERROR);
    if (le == NULL)
        testing_t_errorf_v(t, "rename %q, %q: %s, want a LinkError", from, to,
                           error_text(e));
    else
        CHECK(str_eq(le->op, S("rename")));
    const char *names[] = {"from", "to"};
    remove_dir(t, names, 2);
}

static void TestHardLink(TestingT *t) {
    make_dir(t);
    Str from = path_in("hardlinktestfrom"), to = path_in("hardlinktestto");
    write_file(t, to, "x");
    check_ok(t, "Link", os_link(to, from));
    Error e = BURROW_NO_ERROR;
    OsFileInfo tostat = os_stat(a, to, &e);
    OsFileInfo fromstat = os_stat(a, from, &e);
    check_ok(t, "Stat", e);
    CHECK(os_same_file(tostat, fromstat));

    /* Linking over something that exists is a LinkError with EEXIST. */
    e = os_link(to, from);
    const OsLinkError *le = (const OsLinkError *)errors_as(e, TYPE_OS_LINK_ERROR);
    if (le == NULL)
        testing_t_errorf_v(t, "link %q, %q: %s, want a LinkError", to, from,
                           error_text(e));
    else
        CHECK(str_eq(le->op, S("link")));
    CHECK(os_is_exist(e));
    const char *names[] = {"hardlinktestfrom", "hardlinktestto"};
    remove_dir(t, names, 2);
}

static void TestSymlink(TestingT *t) {
#if defined(BURROW_OS_WINDOWS)
    testing_t_skip_v(t, "making a symbolic link may need privileges on Windows");
#endif
    make_dir(t);
    Str from = path_in("symlinktestfrom"), to = path_in("symlinktestto");
    write_file(t, to, "x");
    check_ok(t, "Symlink", os_symlink(to, from));
    Error e = BURROW_NO_ERROR;
    OsFileInfo fi = os_lstat(a, from, &e);
    check_ok(t, "Lstat", e);
    if (fi.vt != NULL)
        CHECK((fi.vt->mode(fi.data) & FS_MODE_SYMLINK) != 0);
    fi = os_stat(a, from, &e);
    check_ok(t, "Stat", e);
    if (fi.vt != NULL)
        CHECK((fi.vt->mode(fi.data) & FS_MODE_SYMLINK) == 0);
    Str s = os_readlink(a, from, &e);
    check_ok(t, "Readlink", e);
    CHECK(str_eq(s, to));

    os_readlink(a, to, &e);
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL && str_eq(pe->op, S("readlink")));
    const char *names[] = {"symlinktestfrom", "symlinktestto"};
    remove_dir(t, names, 2);
}

static void TestLongSymlink(TestingT *t) {
#if defined(BURROW_OS_WINDOWS)
    testing_t_skip_v(t, "making a symbolic link may need privileges on Windows");
#endif
    make_dir(t);
    /* Longer than Readlink's first 128 byte buffer. */
    Str target = strings_repeat(a, S("x/"), 300);
    Str link = path_in("longlinktest");
    check_ok(t, "Symlink", os_symlink(target, link));
    Error e = BURROW_NO_ERROR;
    Str r = os_readlink(a, link, &e);
    check_ok(t, "Readlink", e);
    CHECK(str_eq(r, target));
    const char *names[] = {"longlinktest"};
    remove_dir(t, names, 1);
}

static void TestChmod(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    write_file(t, path, "x");
#if defined(BURROW_OS_WINDOWS)
    /* Windows has one bit, read only, and Go maps it to 0444 or 0666. */
    OsFileMode fm = 0444;
#else
    OsFileMode fm = 0456;
#endif
    check_ok(t, "Chmod", os_chmod(path, fm));
    Error e = BURROW_NO_ERROR;
    OsFileInfo fi = os_stat(a, path, &e);
    check_ok(t, "Stat", e);
    if (fi.vt != NULL && fi.vt->mode(fi.data) != fm)
        testing_t_errorf_v(t, "stat %q: mode %#o want %#o", path,
                           (Int)fi.vt->mode(fi.data), (Int)fm);
    check_ok(t, "Chmod", os_chmod(path, 0644));
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestChtimes(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    write_file(t, path, "x");
    Time at = time_from_unix(1000000000, 0);
    Time mt = time_from_unix(1200000000, 500);
    check_ok(t, "Chtimes", os_chtimes(path, at, mt));
    Error e = BURROW_NO_ERROR;
    OsFileInfo fi = os_stat(a, path, &e);
    check_ok(t, "Stat", e);
    /* Windows keeps times in 100ns units, so only the seconds are checked. */
    if (fi.vt != NULL)
        CHECK_INT_EQ(time_unix(fi.vt->mod_time(fi.data)), 1200000000);

    /* A zero time leaves that time alone. */
    check_ok(t, "Chtimes", os_chtimes(path, at, (Time){0}));
    fi = os_stat(a, path, &e);
    if (fi.vt != NULL)
        CHECK_INT_EQ(time_unix(fi.vt->mod_time(fi.data)), 1200000000);

    e = os_chtimes(path_in("missing"), at, mt);
    CHECK(os_is_not_exist(e));
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestChown(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    write_file(t, path, "x");
#if defined(BURROW_OS_WINDOWS)
    Error e = os_chown(path, 0, 0);
    CHECK(errno_of(e) == SYSCALL_EWINDOWS);
#else
    /* -1 for both leaves the owner as it is, which anyone may do. */
    check_ok(t, "Chown", os_chown(path, -1, -1));
#endif
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void TestNulInName(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_open(a, str_from_bytes((const Byte *)"a\0b", 3), &e);
    CHECK(f == NULL);
    /* EINVAL, which is not os_err_invalid, since Errno.Is does not match
     * ErrInvalid. */
    CHECK(errno_of(e) == SYSCALL_EINVAL);
    CHECK(!errors_is(e, os_err_invalid));
}

static void TestStdio(TestingT *t) {
    OsFile *in = os_stdin;
    OsFile *out = os_stdout;
    OsFile *err = os_stderr;
    if (in != NULL)
        CHECK_STR_EQ(str_to_cstr(a, os_file_name(in)), "/dev/stdin");
    if (out != NULL)
        CHECK_STR_EQ(str_to_cstr(a, os_file_name(out)), "/dev/stdout");
    if (err != NULL)
        CHECK_STR_EQ(str_to_cstr(a, os_file_name(err)), "/dev/stderr");
    CHECK(os_stdout == out);
    /* Freeing one of the three does nothing. */
    os_file_free(out);
}

static void TestNewFileInvalid(TestingT *t) {
    CHECK(os_new_file(a, ~(Uintptr)0, S("bad")) == NULL);
}

static void TestErrIsExist(TestingT *t) {
    Error exist = syscall_errno_as_error(SYSCALL_EEXIST, a);
    Error noent = syscall_errno_as_error(SYSCALL_ENOENT, a);
    Error perm = syscall_errno_as_error(SYSCALL_EACCES, a);
    Error pe = fs_path_error_new(a, S("open"), S("x"), exist);
    Error le = os_link_error_new(a, S("link"), S("x"), S("y"), exist);
    Error se = os_new_syscall_error(a, S("open"), exist);

    CHECK(os_is_exist(exist));
    CHECK(os_is_exist(pe));
    CHECK(os_is_exist(le));
    CHECK(os_is_exist(se));
    CHECK(os_is_exist(os_err_exist));
    CHECK(!os_is_exist(noent));
    CHECK(!os_is_exist(BURROW_NO_ERROR));

    CHECK(os_is_not_exist(noent));
    CHECK(os_is_not_exist(fs_path_error_new(a, S("open"), S("x"), noent)));
    CHECK(os_is_not_exist(os_err_not_exist));
    CHECK(!os_is_not_exist(exist));

    CHECK(os_is_permission(perm));
    CHECK(os_is_permission(os_err_permission));
    CHECK(!os_is_permission(noent));

    /* Only one level comes off, as in Go. */
    Error twice = fs_path_error_new(a, S("open"), S("x"), pe);
    CHECK(!os_is_exist(twice));
}

static void TestErrorText(TestingT *t) {
    Error exist = syscall_errno_as_error(SYSCALL_EEXIST, a);
    Error le = os_link_error_new(a, S("link"), S("x"), S("y"), exist);
    Str want = fmt_sprintf_v(a, "link x y: %s", error_text(exist));
    CHECK(str_eq(error_text(le), want));
    Error se = os_new_syscall_error(a, S("open"), exist);
    want = fmt_sprintf_v(a, "open: %s", error_text(exist));
    CHECK(str_eq(error_text(se), want));
    CHECK(BURROW_OK(os_new_syscall_error(a, S("open"), BURROW_NO_ERROR)));
    CHECK(os_is_timeout(os_err_deadline_exceeded));
    CHECK_STR_EQ(str_to_cstr(a, error_text(os_err_deadline_exceeded)), "i/o timeout");
    CHECK(!os_is_timeout(exist));
}

static void TestIsPathSeparator(TestingT *t) {
    CHECK(os_is_path_separator('/'));
#if defined(BURROW_OS_WINDOWS)
    CHECK(os_is_path_separator('\\'));
#else
    CHECK(!os_is_path_separator('\\'));
#endif
    CHECK(!os_is_path_separator('a'));
}

static void TestAdapters(TestingT *t) {
    make_dir(t);
    Str path = path_in("f");
    OsFile *f = create(t, path);
    Error e = BURROW_NO_ERROR;
    IoWriter w = os_file_as_io_writer(f);
    w.vt->write(w.data, bytes_of(S("through io")), &e);
    check_ok(t, "Write", e);
    IoSeeker s = os_file_as_io_seeker(f);
    s.vt->seek(s.data, 0, OS_SEEK_SET, &e);
    Byte b[32];
    IoReader r = os_file_as_io_reader(f);
    Int n = r.vt->read(r.data, slice_from(b, 32, 32, TYPE_BYTE), &e);
    CHECK(n == 10 && memcmp(b, "through io", 10) == 0);
    FsFile ff = os_file_as_fs_file(f);
    FsFileInfo fi = ff.vt->stat(ff.data, a, &e);
    check_ok(t, "Stat", e);
    if (fi.vt != NULL)
        CHECK_INT_EQ(fi.vt->size(fi.data), 10);
    IoCloser c = os_file_as_io_closer(f);
    check_ok(t, "Close", c.vt->close(c.data));
    os_file_free(f);
    const char *names[] = {"f"};
    remove_dir(t, names, 1);
}

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

#define TESTS(X)                                                                       \
    X(TestStatError)                                                                   \
    X(TestStat)                                                                        \
    X(TestFstat)                                                                       \
    X(TestSameFile)                                                                    \
    X(TestRead0)                                                                       \
    X(TestReadAt)                                                                      \
    X(TestReadAtNegativeOffset)                                                        \
    X(TestWriteAt)                                                                     \
    X(TestWriteAtNegativeOffset)                                                       \
    X(TestWriteAtInAppendMode)                                                         \
    X(TestSeek)                                                                        \
    X(TestFTruncate)                                                                   \
    X(TestTruncate)                                                                    \
    X(TestReadWriteFile)                                                               \
    X(TestCloseTwice)                                                                  \
    X(TestNoDeadline)                                                                  \
    X(TestOpenFileExcl)                                                                \
    X(TestRemove)                                                                      \
    X(TestMkdirExists)                                                                 \
    X(TestRename)                                                                      \
    X(TestRenameOverwriteDest)                                                         \
    X(TestRenameFailed)                                                                \
    X(TestRenameToDirFailed)                                                           \
    X(TestHardLink)                                                                    \
    X(TestSymlink)                                                                     \
    X(TestLongSymlink)                                                                 \
    X(TestChmod)                                                                       \
    X(TestChtimes)                                                                     \
    X(TestChown)                                                                       \
    X(TestNulInName)                                                                   \
    X(TestStdio)                                                                       \
    X(TestNewFileInvalid)                                                              \
    X(TestErrIsExist)                                                                  \
    X(TestErrorText)                                                                   \
    X(TestIsPathSeparator)                                                             \
    X(TestAdapters)

static int os_main(TestingM *m) {
    setup();
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(os_main, TESTS)
