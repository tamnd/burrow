/* Derived from Go's src/os/os_test.go, path_test.go, removeall_test.go,
 * tempfile_test.go and read_test.go.
 * Go source: go1.27.1.
 *
 * Reading directories, MkdirAll, RemoveAll, the temporary files and DirFS.
 * Each test works in a directory from os_mkdir_temp and takes it apart with
 * os_remove_all, so those two are under test everywhere.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"
#include "burrow/syscall.h"

#include "check.h"

#include <string.h>

/* ------------------------------------------------------------- helpers */

#define S(lit) BURROW_S(lit)

static Arena ar;
static Alloc *a;

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

/* t.TempDir. */
static Str temp_dir(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Str d = os_mkdir_temp(a, S(""), S("burrow-os-dir-test-*"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "MkdirTemp: %s", error_text(e));
    return d;
}

static void cleanup(TestingT *t, Str d) {
    Error e = os_remove_all(d);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "RemoveAll %s: %s", d, error_text(e));
    Error se = BURROW_NO_ERROR;
    os_lstat(a, d, &se);
    if (!os_is_not_exist(se))
        testing_t_errorf_v(t, "%s is still there after RemoveAll", d);
}

static Str join(Str d, const char *name) {
    return fmt_sprintf_v(a, "%s%c%s", d, (Int)OS_PATH_SEPARATOR, str_from_cstr(name));
}

static void touch(TestingT *t, Str name, const char *data) {
    Error e = os_write_file(name, bytes_of(str_from_cstr(data)), 0644);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "WriteFile %s: %s", name, error_text(e));
}

static OsFile *open_dir(TestingT *t, Str d) {
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_open(a, d, &e);
    if (f == NULL)
        testing_t_fatalf_v(t, "Open %s: %s", d, error_text(e));
    return f;
}

static SyscallErrno errno_of(Error err) {
    const SyscallErrno *e = (const SyscallErrno *)errors_as(err, TYPE_SYSCALL_ERRNO);
    return e != NULL ? *e : 0;
}

static Str de_name(Slice s, Int i) {
    const FsDirEntry *d = (const FsDirEntry *)slice_at(s, i);
    return d->vt->name(d->data);
}

/* --------------------------------------------------------- reading dirs */

static void TestReadDir(TestingT *t) {
    Str d = temp_dir(t);
    touch(t, join(d, "b.txt"), "bb");
    touch(t, join(d, "a.txt"), "a");
    Error e = os_mkdir(join(d, "sub"), 0755);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Mkdir: %s", error_text(e));

    Slice list = os_read_dir(a, d, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "ReadDir: %s", error_text(e));
    CHECK_INT_EQ(list.len, 3);
    if (list.len == 3) {
        CHECK(str_eq(de_name(list, 0), S("a.txt")));
        CHECK(str_eq(de_name(list, 1), S("b.txt")));
        CHECK(str_eq(de_name(list, 2), S("sub")));
        const FsDirEntry *sub = (const FsDirEntry *)slice_at(list, 2);
        CHECK(sub->vt->is_dir(sub->data));
        CHECK(sub->vt->type(sub->data) == FS_MODE_DIR);
        const FsDirEntry *b = (const FsDirEntry *)slice_at(list, 1);
        CHECK(!b->vt->is_dir(b->data));
        CHECK(b->vt->type(b->data) == 0);
        FsFileInfo fi = b->vt->info(b->data, a, &e);
        CHECK(BURROW_OK(e));
        if (fi.vt != NULL) {
            CHECK(str_eq(fi.vt->name(fi.data), S("b.txt")));
            CHECK_INT_EQ(fi.vt->size(fi.data), 2);
        }
        Str text = fs_format_dir_entry(a, *sub);
        CHECK(str_eq(text, S("d sub/")));
    }

    /* A missing directory fails to open. */
    os_read_dir(a, join(d, "missing"), &e);
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL && str_eq(pe->op, S("open")));
    CHECK(os_is_not_exist(e));
    cleanup(t, d);
}

static void TestReaddirNValues(TestingT *t) {
    Str d = temp_dir(t);
    for (int i = 1; i <= 105; i++) {
        Str name = fmt_sprintf_v(a, "%s%c%d", d, (Int)OS_PATH_SEPARATOR, (Int)i);
        Error e = os_write_file(name, bytes_of(strings_repeat(a, S("X"), i)), 0644);
        if (BURROW_FAILED(e))
            testing_t_fatalf_v(t, "WriteFile: %s", error_text(e));
    }

    /* The three readers in turn, as Go's test runs them. */
    for (int mode = 0; mode < 3; mode++) {
        struct {
            Int n, want;
            bool eof;
            int reopen; /* open a fresh file before this step */
        } steps[] = {
            {0, 105, false, 1}, {0, 0, false, 0},     {-1, 105, false, 1},
            {-2, 0, false, 0},  {0, 0, false, 0},     {1, 1, false, 1},
            {2, 2, false, 0},   {105, 102, false, 0}, {3, 0, true, 0},
        };
        OsFile *f = NULL;
        for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
            if (steps[i].reopen) {
                if (f != NULL) {
                    os_file_close(f);
                    os_file_free(f);
                }
                f = open_dir(t, d);
            }
            Error e = BURROW_NO_ERROR;
            Slice got;
            if (mode == 0)
                got = os_file_readdir(f, a, steps[i].n, &e);
            else if (mode == 1)
                got = os_file_readdirnames(f, a, steps[i].n, &e);
            else
                got = os_file_read_dir(f, a, steps[i].n, &e);
            bool eof = errors_is(e, io_eof);
            if (eof != steps[i].eof || (BURROW_FAILED(e) && !eof))
                testing_t_fatalf_v(t, "mode %d step %d: n=%d gave error %s", (Int)mode,
                                   (Int)i, steps[i].n,
                                   BURROW_FAILED(e) ? error_text(e) : S("none"));
            if (got.len != steps[i].want)
                testing_t_errorf_v(t, "mode %d step %d: n=%d gave %d entries, want %d",
                                   (Int)mode, (Int)i, steps[i].n, got.len,
                                   steps[i].want);
        }
        os_file_close(f);
        os_file_free(f);
    }

    /* Readdir's FileInfos are lstats, sizes and all. */
    OsFile *f = open_dir(t, d);
    Error e = BURROW_NO_ERROR;
    Slice infos = os_file_readdir(f, a, -1, &e);
    int64_t total = 0;
    for (Int i = 0; i < infos.len; i++) {
        const FsFileInfo *fi = (const FsFileInfo *)slice_at(infos, i);
        total += fi->vt->size(fi->data);
    }
    CHECK_INT_EQ(total, 105 * 106 / 2);
    os_file_close(f);
    os_file_free(f);
    cleanup(t, d);
}

static void TestReaddirSmallSeek(TestingT *t) {
    Str d = temp_dir(t);
    touch(t, join(d, "entry1"), "");
    touch(t, join(d, "entry2"), "");
    touch(t, join(d, "entry3"), "");
    OsFile *f = open_dir(t, d);
    Error e = BURROW_NO_ERROR;
    Slice names1 = os_file_readdirnames(f, a, 1, &e);
    CHECK(BURROW_OK(e));
    CHECK_INT_EQ(names1.len, 1);
    os_file_seek(f, 0, OS_SEEK_SET, &e);
    CHECK(BURROW_OK(e));
    Slice names2 = os_file_readdirnames(f, a, 0, &e);
    CHECK(BURROW_OK(e));
    CHECK_INT_EQ(names2.len, 3);
    os_file_close(f);
    os_file_free(f);
    cleanup(t, d);
}

static void TestReaddirOfFile(TestingT *t) {
    Str d = temp_dir(t);
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_create_temp(a, d, S("_Go_ReaddirOfFile"), &e);
    if (f == NULL)
        testing_t_fatalf_v(t, "CreateTemp: %s", error_text(e));
    os_file_write_string(f, S("foo"), &e);
    os_file_close(f);
    OsFile *reg = open_dir(t, os_file_name(f));
    Slice names = os_file_readdirnames(reg, a, -1, &e);
    CHECK(BURROW_FAILED(e));
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    if (pe == NULL || !str_eq(pe->path, os_file_name(f)))
        testing_t_errorf_v(t, "Readdirnames returned %s; want a PathError with path %s",
                           BURROW_FAILED(e) ? error_text(e) : S("nil"),
                           os_file_name(f));
    CHECK_INT_EQ(names.len, 0);
    os_file_close(reg);
    os_file_free(reg);
    os_file_free(f);
    cleanup(t, d);
}

static void TestReaddirClosed(TestingT *t) {
    Str d = temp_dir(t);
    OsFile *f = open_dir(t, d);
    os_file_close(f);
    Error e = BURROW_NO_ERROR;
    os_file_read_dir(f, a, -1, &e);
    CHECK(BURROW_FAILED(e));
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL);
    if (pe != NULL)
        CHECK(str_eq(error_text(pe->err), S("use of closed file")));
    os_file_free(f);

    os_file_read_dir(NULL, a, -1, &e);
    CHECK(errors_is(e, os_err_invalid));
    cleanup(t, d);
}

static void TestReadDirAsFsFile(TestingT *t) {
    Str d = temp_dir(t);
    touch(t, join(d, "x"), "");
    OsFile *f = open_dir(t, d);
    FsFile ff = os_file_as_fs_file(f);
    CHECK(ff.vt->read_dir != NULL);
    Error e = BURROW_NO_ERROR;
    Slice list = ff.vt->read_dir(ff.data, a, 5, &e);
    CHECK(BURROW_OK(e));
    CHECK_INT_EQ(list.len, 1);
    list = ff.vt->read_dir(ff.data, a, 5, &e);
    CHECK(errors_is(e, io_eof));
    CHECK_INT_EQ(list.len, 0);
    os_file_close(f);
    os_file_free(f);
    cleanup(t, d);
}

/* -------------------------------------------------------- MkdirAll, etc */

static void TestMkdirAll(TestingT *t) {
    Str d = temp_dir(t);
    Str path = fmt_sprintf_v(a, "%s%c_TestMkdirAll_%cdir%c.%cdir2", d,
                             (Int)OS_PATH_SEPARATOR, (Int)OS_PATH_SEPARATOR,
                             (Int)OS_PATH_SEPARATOR, (Int)OS_PATH_SEPARATOR);
    Error e = os_mkdir_all(path, 0777);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "MkdirAll %s: %s", path, error_text(e));

    /* Already there is fine. */
    e = os_mkdir_all(path, 0777);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "MkdirAll %s (second time): %s", path, error_text(e));

    Str fpath = join(path, "file");
    touch(t, fpath, "");

    /* A directory cannot be made where a file is. */
    e = os_mkdir_all(fpath, 0777);
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    if (pe == NULL)
        testing_t_fatalf_v(t, "MkdirAll %s: no PathError", fpath);
    CHECK(str_eq(pe->path, fpath));
    CHECK(errno_of(e) == SYSCALL_ENOTDIR);

    /* Nor under one. */
    Str ffpath = join(fpath, "subdir");
    e = os_mkdir_all(ffpath, 0777);
    pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    if (pe == NULL)
        testing_t_fatalf_v(t, "MkdirAll %s: no PathError", ffpath);
    CHECK(str_eq(pe->path, fpath));

#if defined(BURROW_OS_WINDOWS)
    Str wpath = fmt_sprintf_v(a, "%s\\_TestMkdirAll_\\dir\\.\\dir2\\", d);
    e = os_mkdir_all(wpath, 0777);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "MkdirAll %s: %s", wpath, error_text(e));
#endif
    cleanup(t, d);
}

static void TestRemoveAll(TestingT *t) {
    Str d = temp_dir(t);
    Str path = join(d, "_TestRemoveAll_");
    Str fpath = join(path, "file");
    Str dpath = join(path, "dir");

    /* An empty name and a missing one are both fine. */
    CHECK(BURROW_OK(os_remove_all(S(""))));
    CHECK(BURROW_OK(os_remove_all(path)));

    /* A single file. */
    touch(t, path, "");
    Error e = os_remove_all(path);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "RemoveAll %s (first): %s", path, error_text(e));
    Error se = BURROW_NO_ERROR;
    os_lstat(a, path, &se);
    CHECK(os_is_not_exist(se));

    /* A directory with a file and an empty directory and a full one. */
    CHECK(BURROW_OK(os_mkdir_all(join(dpath, "deeper"), 0777)));
    touch(t, fpath, "");
    touch(t, join(dpath, "file"), "");
    touch(t, join(join(dpath, "deeper"), "file"), "");
    e = os_remove_all(path);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "RemoveAll %s (second): %s", path, error_text(e));
    os_lstat(a, path, &se);
    CHECK(os_is_not_exist(se));

    /* Lots of entries, more than one batch of names. */
    CHECK(BURROW_OK(os_mkdir(path, 0777)));
    for (int i = 0; i < 1100; i++)
        touch(t, fmt_sprintf_v(a, "%s%cf%d", path, (Int)OS_PATH_SEPARATOR, (Int)i), "");
    e = os_remove_all(path);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "RemoveAll %s (big): %s", path, error_text(e));
    os_lstat(a, path, &se);
    CHECK(os_is_not_exist(se));

    /* A trailing separator after a file still removes it. */
    touch(t, path, "");
    e = os_remove_all(fmt_sprintf_v(a, "%s%c", path, (Int)OS_PATH_SEPARATOR));
    CHECK(BURROW_OK(e));
    cleanup(t, d);
}

static void TestRemoveAllDot(TestingT *t) {
    Str d = temp_dir(t);
    Str cases[] = {S("."), join(d, ".")};
    for (int i = 0; i < 2; i++) {
        Error e = os_remove_all(cases[i]);
        const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
        if (pe == NULL) {
            testing_t_errorf_v(t, "RemoveAll %s: want a PathError", cases[i]);
            continue;
        }
        CHECK(str_eq(pe->op, S("RemoveAll")));
        CHECK(errno_of(e) == SYSCALL_EINVAL);
    }
    /* Nothing went. */
    Error se = BURROW_NO_ERROR;
    os_stat(a, d, &se);
    CHECK(BURROW_OK(se));
    cleanup(t, d);
}

/* ----------------------------------------------------------- temp files */

static void TestTempDir(TestingT *t) {
    Str td = os_temp_dir(a);
    CHECK(td.len > 0);
#if defined(BURROW_OS_WINDOWS)
    /* Go trims the backslash GetTempPath2 ends with. */
    if (td.len > 3)
        CHECK(!os_is_path_separator(td.p[td.len - 1]));
#else
    /* Go hands $TMPDIR back as it is, so on macOS it ends in a slash. */
    bool found = false;
    Str env = os_lookup_env(a, S("TMPDIR"), &found);
    if (found && env.len > 0)
        CHECK(str_eq(td, env));
#endif
    Error e = BURROW_NO_ERROR;
    OsFileInfo fi = os_stat(a, td, &e);
    CHECK(BURROW_OK(e) && fi.vt->is_dir(fi.data));
}

static void TestCreateTemp(TestingT *t) {
    Str d = temp_dir(t);
    Str prefix = join(d, "foo");
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_create_temp(a, d, S("foo*.txt"), &e);
    if (f == NULL)
        testing_t_fatalf_v(t, "CreateTemp: %s", error_text(e));
    Str name = os_file_name(f);
    CHECK(strings_has_prefix(name, prefix));
    CHECK(strings_has_suffix(name, S(".txt")));
    CHECK(name.len > prefix.len + 4);
    for (Int i = prefix.len; i < name.len - 4; i++)
        CHECK(name.p[i] >= '0' && name.p[i] <= '9');
    OsFileInfo fi = os_file_stat(f, a, &e);
#if !defined(BURROW_OS_WINDOWS)
    CHECK(fi.vt != NULL && (fi.vt->mode(fi.data) & FS_MODE_PERM) == 0600);
#else
    CHECK(fi.vt != NULL);
#endif
    /* No "*" puts the number at the end. */
    OsFile *g = os_create_temp(a, d, S("bar"), &e);
    CHECK(g != NULL);
    if (g != NULL) {
        Str gname = os_file_name(g);
        Str gprefix = join(d, "bar");
        CHECK(strings_has_prefix(gname, gprefix) && gname.len > gprefix.len);
        os_file_free(g);
    }
    os_file_free(f);

    /* A separator in the pattern is refused. */
    Str bad = fmt_sprintf_v(a, "ioutil%ctest*", (Int)OS_PATH_SEPARATOR);
    OsFile *h = os_create_temp(a, d, bad, &e);
    CHECK(h == NULL);
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL && str_eq(pe->op, S("createtemp")) && str_eq(pe->path, bad));
    CHECK(str_eq(error_text(pe != NULL ? pe->err : e),
                 S("pattern contains path separator")));
    cleanup(t, d);
}

static void TestMkdirTemp(TestingT *t) {
    Str d = temp_dir(t);
    Error e = BURROW_NO_ERROR;
    Str sub = os_mkdir_temp(a, d, S("x*y"), &e);
    CHECK(BURROW_OK(e));
    OsFileInfo fi = os_stat(a, sub, &e);
    CHECK(BURROW_OK(e) && fi.vt->is_dir(fi.data));
    Str base = fi.vt->name(fi.data);
    CHECK(strings_has_prefix(base, S("x")) && strings_has_suffix(base, S("y")));
    CHECK(base.len > 2);

    /* A missing dir says so with a stat of it. */
    Str missing = join(d, "missing");
    Str none = os_mkdir_temp(a, missing, S("z"), &e);
    CHECK(none.len == 0);
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL && str_eq(pe->op, S("stat")) && str_eq(pe->path, missing));
    CHECK(os_is_not_exist(e));

    Str bad = fmt_sprintf_v(a, "a%cb", (Int)OS_PATH_SEPARATOR);
    os_mkdir_temp(a, d, bad, &e);
    pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL && str_eq(pe->op, S("mkdirtemp")));
    cleanup(t, d);
}

/* ----------------------------------------------------------------- DirFS */

static void TestDirFS(TestingT *t) {
    Str d = temp_dir(t);
    CHECK(BURROW_OK(os_mkdir_all(join(join(d, "sub"), "deeper"), 0755)));
    touch(t, join(d, "top.txt"), "top");
    touch(t, join(join(d, "sub"), "b"), "bee");
    touch(t, join(join(d, "sub"), "a"), "ay");

    Fs fsys = os_dir_fs(a, d);
    CHECK(fsys.vt != NULL);
    Error e = BURROW_NO_ERROR;

    e = fstest_test_fs_v(fsys, S("top.txt"), S("sub/a"), S("sub/b"), S("sub/deeper"));
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "TestFS: %v", e);

    Slice data = fs_read_file(a, fsys, S("sub/b"), &e);
    CHECK(BURROW_OK(e));
    CHECK(str_eq(str_from_bytes((const Byte *)data.p, data.len), S("bee")));

    Slice list = fs_read_dir(a, fsys, S("sub"), &e);
    CHECK(BURROW_OK(e));
    CHECK_INT_EQ(list.len, 3);
    if (list.len == 3) {
        CHECK(str_eq(de_name(list, 0), S("a")));
        CHECK(str_eq(de_name(list, 1), S("b")));
        CHECK(str_eq(de_name(list, 2), S("deeper")));
    }

    FsFileInfo fi = fs_stat(a, fsys, S("top.txt"), &e);
    CHECK(BURROW_OK(e) && fi.vt->size(fi.data) == 3);

    /* Open gives a file that reads and lists. */
    FsFile f = fsys.vt->open(fsys.data, a, S("sub"), &e);
    CHECK(BURROW_OK(e) && f.vt != NULL);
    if (f.vt != NULL) {
        Slice part = f.vt->read_dir(f.data, a, -1, &e);
        CHECK(BURROW_OK(e) && part.len == 3);
        f.vt->read_closer.closer.close(f.data);
    }

    /* The error names the FS's path, not the system's. */
    fs_stat(a, fsys, S("sub/missing"), &e);
    const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL && str_eq(pe->op, S("stat")) &&
          str_eq(pe->path, S("sub/missing")));
    CHECK(os_is_not_exist(e));

    /* A name io/fs would not take is invalid. */
    const char *bad[] = {"/top.txt", "../x", "sub/../top.txt", "./top.txt"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        Str name = str_from_cstr(bad[i]);
        fs_read_file(a, fsys, name, &e);
        pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
        if (pe == NULL || !errors_is(e, os_err_invalid) || !str_eq(pe->path, name))
            testing_t_errorf_v(t, "ReadFile %s: %s, want an invalid PathError", name,
                               BURROW_FAILED(e) ? error_text(e) : S("nil"));
    }

    /* An empty root says so. */
    Fs empty = os_dir_fs(a, S(""));
    fs_stat(a, empty, S("x"), &e);
    pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL && str_eq(error_text(pe->err), S("os: DirFS with empty root")));

#if !defined(BURROW_OS_WINDOWS)
    /* Symbolic links through ReadLink and Lstat. */
    CHECK(BURROW_OK(os_symlink(S("top.txt"), join(d, "link"))));
    Str target = fs_read_link(a, fsys, S("link"), &e);
    CHECK(BURROW_OK(e) && str_eq(target, S("top.txt")));
    fi = fs_lstat(a, fsys, S("link"), &e);
    CHECK(BURROW_OK(e) && (fi.vt->mode(fi.data) & FS_MODE_SYMLINK) != 0);
#endif
    cleanup(t, d);
}

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

#define TESTS(X)                                                                       \
    X(TestReadDir)                                                                     \
    X(TestReaddirNValues)                                                              \
    X(TestReaddirSmallSeek)                                                            \
    X(TestReaddirOfFile)                                                               \
    X(TestReaddirClosed)                                                               \
    X(TestReadDirAsFsFile)                                                             \
    X(TestMkdirAll)                                                                    \
    X(TestRemoveAll)                                                                   \
    X(TestRemoveAllDot)                                                                \
    X(TestTempDir)                                                                     \
    X(TestCreateTemp)                                                                  \
    X(TestMkdirTemp)                                                                   \
    X(TestDirFS)

static int os_dir_main(TestingM *m) {
    setup();
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(os_dir_main, TESTS)
