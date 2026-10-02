/* Derived from Go's src/testing/fstest/testfs_test.go.
 * Go source: go1.27.1.
 *
 * TestMapFS and the other tests from mapfs_test.go are in fs_test.c with the
 * rest of MapFS. The tests after TestTestFSWrappedErrors are not in Go's file.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/io/fs.h"
#include "burrow/mem/arena.h"
#include "burrow/testing/fstest.h"

#include <stdint.h>
#include <string.h>

static Slice text(const char *s) {
    return slice_from((void *)(uintptr_t)s, (Int)strlen(s), (Int)strlen(s), TYPE_BYTE);
}

static FstestMapFile *map_file(Alloc *a, const char *data, FsFileMode mode) {
    FstestMapFile *f = BURROW_NEW(a, FstestMapFile);
    memset(f, 0, sizeof *f);
    f->data = text(data);
    f->mode = mode;
    return f;
}

/* A directory of its own under the temporary directory, which the test
 * removes. */
static Str temp_dir(TestingT *t, Alloc *a) {
    Error err;
    Str dir = os_mkdir_temp(a, BURROW_STR_EMPTY, BURROW_S("fstest"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
    return dir;
}

static void TestSymlink(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str tmp = temp_dir(t, a);
    Fs tmpfs = os_dir_fs(a, tmp);

    Str hello = path_join_v(a, 2, tmp, BURROW_S("hello"));
    Error err = os_write_file(hello, text("hello, world\n"), 0644);
    if (BURROW_FAILED(err))
        goto fatal;

    /* Go's testenv.MustHaveSymlink: Windows without the privilege cannot
     * make one, and that is a skip rather than a failure. */
    /* Wine says it made the link when nothing on disk follows it, so a link
     * that does not lead to its target counts as no symlinks too. */
    Str link = path_join_v(a, 2, tmp, BURROW_S("hello.link"));
    err = os_symlink(hello, link);
    if (BURROW_OK(err))
        (void)os_stat(a, link, &err);
    if (BURROW_FAILED(err)) {
        (void)os_remove_all(tmp);
        arena_free(&ar);
        testing_t_skip_v(t, "symlinks unavailable:", err);
    }
    err = os_symlink(BURROW_S("hello"),
                     path_join_v(a, 2, tmp, BURROW_S("hello_rel.link")));
    if (BURROW_FAILED(err))
        goto fatal;

    err = fstest_test_fs_v(tmpfs, BURROW_S("hello"), BURROW_S("hello.link"));
    if (BURROW_FAILED(err))
        goto fatal;
    (void)os_remove_all(tmp);
    arena_free(&ar);
    return;

fatal:
    (void)os_remove_all(tmp);
    arena_free(&ar);
    testing_t_fatal_v(t, err);
}

static void TestDash(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FstestMapFS m = fstest_map_fs_make(a);
    fstest_map_fs_set(m, BURROW_S("a-b/a"), map_file(a, "a-b/a", 0));
    Error err = fstest_test_fs_v(fstest_map_fs_as_fs(m), BURROW_S("a-b/a"));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    arena_free(&ar);
}

/* shuffledFS: a MapFS with only Open, whose directories list their entries
 * backwards. Go's shuffledFile embeds the fs.File, so it has Stat, Read and
 * Close but not Seek or ReadAt, and this one forwards the same three. */
typedef struct ShuffledFile {
    FsFile f;
} ShuffledFile;

static Int shuffled_read(void *self, Slice p, Error *err) {
    FsFile f = ((ShuffledFile *)self)->f;
    return f.vt->read_closer.reader.read(f.data, p, err);
}

static Error shuffled_close(void *self) {
    FsFile f = ((ShuffledFile *)self)->f;
    return f.vt->read_closer.closer.close(f.data);
}

static FsFileInfo shuffled_stat(void *self, Alloc *a, Error *err) {
    FsFile f = ((ShuffledFile *)self)->f;
    return f.vt->stat(f.data, a, err);
}

static int reverse_names(void *env, const void *x, const void *y) {
    (void)env;
    const FsDirEntry *dx = (const FsDirEntry *)x, *dy = (const FsDirEntry *)y;
    return (int)strings_compare(dy->vt->name(dy->data), dx->vt->name(dx->data));
}

static Slice shuffled_read_dir(void *self, Alloc *a, Int n, Error *err) {
    FsFile f = ((ShuffledFile *)self)->f;
    /* Shuffle in a deterministic way, all we care about is making sure that
     * the list of directory entries is not in the lexicographic order. */
    Slice dirents = f.vt->read_dir(f.data, a, n, err);
    slices_sort_func(dirents, BURROW_FN(SlicesCmpFunc, reverse_names, NULL));
    return dirents;
}

static const FsFileVT shuffled_file_vt = {
    {{NULL, shuffled_read}, {NULL, shuffled_close}}, shuffled_stat, shuffled_read_dir};

/* The same without read_dir, for what is not a directory. */
static const FsFileVT shuffled_plain_vt = {
    {{NULL, shuffled_read}, {NULL, shuffled_close}}, shuffled_stat, NULL};

static FsFile shuffled_open(void *self, Alloc *a, Str name, Error *err) {
    FstestMapFS m = {(Map *)self};
    FsFile f = fstest_map_fs_open(m, a, name, err);
    if (BURROW_FAILED(*err))
        return f;
    ShuffledFile *s = BURROW_NEW(a, ShuffledFile);
    s->f = f;
    FsFile out = {f.vt->read_dir != NULL ? &shuffled_file_vt : &shuffled_plain_vt, s};
    return out;
}

static const FsVT shuffled_fs_vt = {.open = shuffled_open};

static void TestShuffledFS(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FstestMapFS m = fstest_map_fs_make(a);
    fstest_map_fs_set(m, BURROW_S("tmp/one"), map_file(a, "1", 0));
    fstest_map_fs_set(m, BURROW_S("tmp/two"), map_file(a, "2", 0));
    fstest_map_fs_set(m, BURROW_S("tmp/three"), map_file(a, "3", 0));
    Fs fsys = {&shuffled_fs_vt, m.files};
    Error err = fstest_test_fs_v(fsys, BURROW_S("tmp/one"), BURROW_S("tmp/two"),
                                 BURROW_S("tmp/three"));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);
    arena_free(&ar);
}

/* failPermFS is a filesystem that always fails with fs.ErrPermission. */
static FsFile fail_perm_open(void *self, Alloc *a, Str name, Error *err) {
    (void)self;
    FsFile none = {0};
    if (!fs_valid_path(name))
        *err = fs_path_error_new(a, BURROW_S("open"), name, fs_err_invalid);
    else
        *err = fs_path_error_new(a, BURROW_S("open"), name, fs_err_permission);
    return none;
}

static const FsVT fail_perm_fs_vt = {.open = fail_perm_open};

static void TestTestFSWrappedErrors(TestingT *t) {
    Fs fsys = {&fail_perm_fs_vt, NULL};
    Error err = fstest_test_fs(fsys, slice_nil(TYPE_STRING));
    if (BURROW_OK(err))
        testing_t_fatal_v(t, "error expected");
    testing_t_logf_v(t, "Error (expecting wrapped fs.ErrPermission):\n%v", err);

    if (!errors_is(err, fs_err_permission))
        testing_t_errorf_v(t, "error should be a wrapped ErrPermission: %v", err);

    /* TestFS is expected to return a list of errors. Enforce that the list can
     * be extracted for browsing. Go finds it with errors.As, and here it is
     * the first error down the chain with unwrap_multi. */
    Error e = err;
    while (BURROW_FAILED(e) && e.vt->unwrap_multi == NULL)
        e = errors_unwrap(e);
    if (BURROW_OK(e)) {
        testing_t_errorf_v(
            t, "caller should be able to extract the errors as a list: %v", err);
        return;
    }
    Slice errs = e.vt->unwrap_multi(e.data);
    for (Int i = 0; i < errs.len; i++) {
        Error x = BURROW_AT(Error, errs, i);
        /* ErrPermission is expected but any other error must be reported. */
        if (!errors_is(x, fs_err_permission))
            testing_t_errorf_v(t, "unexpected error: %v", x);
    }
}

/* An FS that passes, as os.DirFS sees it: files, an empty file, a directory
 * inside a directory and an empty directory. */
static void TestDirFS(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str tmp = temp_dir(t, a);
    static const char *const files[][2] = {
        {"a", "one\n"}, {"b", ""}, {"dir/x", "the x file"}, {"dir/sub/y", "why"}};
    Error err =
        os_mkdir_all(path_join_v(a, 3, tmp, BURROW_S("dir"), BURROW_S("sub")), 0755);
    if (BURROW_OK(err))
        err = os_mkdir(path_join_v(a, 2, tmp, BURROW_S("empty")), 0755);
    for (size_t i = 0; i < 4 && BURROW_OK(err); i++)
        err = os_write_file(path_join_v(a, 2, tmp, str_from_cstr(files[i][0])),
                            text(files[i][1]), 0644);
    if (BURROW_OK(err))
        err = fstest_test_fs_v(os_dir_fs(a, tmp), BURROW_S("a"), BURROW_S("b"),
                               BURROW_S("dir/x"), BURROW_S("dir/sub/y"),
                               BURROW_S("empty"));
    (void)os_remove_all(tmp);
    arena_free(&ar);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
}

/* A MapFS whose ReadFile is wrong about what one file holds. TestFS notices
 * it twice through the slot, and once more through fs_read_file, which uses
 * the slot too. */
static Slice wrong_read_file(void *self, Alloc *a, Str name, Error *err) {
    FstestMapFS m = {(Map *)self};
    Slice data = fstest_map_fs_read_file(m, a, name, err);
    if (BURROW_FAILED(*err) || !str_eq(name, BURROW_S("hello")))
        return data;
    return slice_append(a, slice_nil(TYPE_BYTE), "wrong", 5);
}

static void TestTestFSFindsProblems(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FstestMapFS m = fstest_map_fs_make(a);
    fstest_map_fs_set(m, BURROW_S("hello"), map_file(a, "hi", 0));
    FsVT vt = *fstest_map_fs_as_fs(m).vt;
    vt.read_file = wrong_read_file;
    Fs fsys = {&vt, m.files};
    Error err = fstest_test_fs_v(fsys, BURROW_S("hello"), BURROW_S("missing"));
    Str got = error_text(err);
    Str want =
        BURROW_S("TestFS found errors:\n"
                 "hello: ReadAll vs fsys.ReadFile: different data returned\n"
                 "\t\"hi\"\n"
                 "\t\"wrong\"\n"
                 "hello: Readall vs second fsys.ReadFile: different data returned\n"
                 "\t\"hi\"\n"
                 "\t\"wrong\"\n"
                 "hello: ReadAll vs fs.ReadFile: different data returned\n"
                 "\t\"hi\"\n"
                 "\t\"wrong\"\n"
                 "expected but not found: missing");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got:\n%s\nwant:\n%s", got, want);
    arena_free(&ar);
}

/* An empty FS passes with no names, and fails with some. */
static void TestTestFSEmpty(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs fsys = fstest_map_fs_as_fs(fstest_map_fs_make(a));
    Error err = fstest_test_fs(fsys, slice_nil(TYPE_STRING));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "empty: %v", err);
    err = fstest_test_fs_v(fsys, BURROW_S("x"));
    Str want = BURROW_S("TestFS found errors:\nexpected but not found: x");
    if (!str_eq(error_text(err), want))
        testing_t_errorf_v(t, "got %q, want %q", error_text(err), want);

    /* And a non-empty one fails when it is said to be empty. */
    FstestMapFS m = fstest_map_fs_make(a);
    fstest_map_fs_set(m, BURROW_S("d/f"), map_file(a, "", 0));
    err = fstest_test_fs(fstest_map_fs_as_fs(m), slice_nil(TYPE_STRING));
    want = BURROW_S(
        "TestFS found errors:\nexpected empty file system but found files:\nd\nd/f");
    if (!str_eq(error_text(err), want))
        testing_t_errorf_v(t, "got %q, want %q", error_text(err), want);
    arena_free(&ar);
}

/* A MapFS whose Sub gives an empty FS, which only the retest TestFS does on
 * the first directory it is given can notice. */
static Fs empty_sub(void *self, Alloc *a, Str dir, Error *err) {
    (void)self;
    (void)dir;
    *err = BURROW_NO_ERROR;
    return fstest_map_fs_as_fs(fstest_map_fs_make(a));
}

static void TestTestFSSub(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FstestMapFS m = fstest_map_fs_make(a);
    fstest_map_fs_set(m, BURROW_S("d/f"), map_file(a, "f", 0));
    Error err = fstest_test_fs_v(fstest_map_fs_as_fs(m), BURROW_S("d/f"));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);

    FsVT vt = *fstest_map_fs_as_fs(m).vt;
    vt.sub = empty_sub;
    Fs fsys = {&vt, m.files};
    err = fstest_test_fs_v(fsys, BURROW_S("d/f"));
    Str want = BURROW_S("testing fs.Sub(fsys, d): TestFS found errors:\n"
                        "expected but not found: f");
    if (!str_eq(error_text(err), want))
        testing_t_errorf_v(t, "got %q, want %q", error_text(err), want);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestSymlink)                                                                     \
    X(TestDash)                                                                        \
    X(TestShuffledFS)                                                                  \
    X(TestTestFSWrappedErrors)                                                         \
    X(TestDirFS)                                                                       \
    X(TestTestFSFindsProblems)                                                         \
    X(TestTestFSEmpty)                                                                 \
    X(TestTestFSSub)
TESTING_MAIN(TESTS)
