/* Derived from Go's src/io/fs/fs_test.go, format_test.go, glob_test.go,
 * readdir_test.go, readfile_test.go, readlink_test.go, stat_test.go,
 * sub_test.go, walk_test.go and example_test.go, and from
 * src/testing/fstest/mapfs_test.go.
 * Go source: go1.27.1.
 *
 * Go's tests reach for os.DirFS in a few places, to glob the package's own
 * directory and to make a directory that cannot be read. burrow has no os yet,
 * so those use a MapFS laid out the same way, and TestIssue51617 uses an FS
 * that fails to read the one directory.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/io/fs.h"
#include "burrow/mem/arena.h"
#include "burrow/testing/fstest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static bool str_is(Str s, const char *want) {
    return (size_t)s.len == strlen(want) &&
           (s.len == 0 || memcmp(s.p, want, (size_t)s.len) == 0);
}

/* CHECK_STR_EQ for a Str, which need not end in a NUL. */
#define CHECK_IS(got, want)                                                            \
    do {                                                                               \
        Str g_ = (got);                                                                \
        if (!str_is(g_, want))                                                         \
            testing_t_errorf_v(t, "got %q, want %q", g_, want);                        \
    } while (0)

static Str cstr(const char *s) {
    return str_from_bytes((const Byte *)s, (Int)strlen(s));
}

static Slice text(const char *s) {
    return slice_from((void *)(uintptr_t)s, (Int)strlen(s), (Int)strlen(s), TYPE_BYTE);
}

static bool bytes_are(Slice b, const char *want) {
    return (size_t)b.len == strlen(want) &&
           (b.len == 0 || memcmp(b.p, want, (size_t)b.len) == 0);
}

static Str mode_str(Alloc *a, FsFileMode m) {
    return fs_file_mode_string(m, a);
}

/* The names of a Slice of FsDirEntry, joined with spaces, for messages. */
static Str entry_names(Alloc *a, Slice dirs) {
    Str out = BURROW_STR_EMPTY;
    for (Int i = 0; i < dirs.len; i++) {
        FsDirEntry *d = (FsDirEntry *)slice_at(dirs, i);
        Str n = d->vt->name(d->data);
        Str parts[3] = {out, i > 0 ? BURROW_S(" ") : BURROW_STR_EMPTY, n};
        Slice all = slice_nil(TYPE_STRING);
        all = slice_append(a, all, parts, 3);
        out = strings_join(a, all, BURROW_STR_EMPTY);
    }
    return out;
}

/* ---------------------------------------------------------------- MapFS */

typedef struct FileSpec {
    const char *name;
    const char *data;
    FsFileMode mode;
} FileSpec;

static int sys_value;

/* A MapFS from a table, every file with the time now and sys_value. */
static FstestMapFS map_of(Alloc *a, const FileSpec *spec, int n) {
    FstestMapFS fsys = fstest_map_fs_make(a);
    Time now = time_now();
    for (int i = 0; i < n; i++) {
        FstestMapFile *f =
            (FstestMapFile *)mem_alloc(a, sizeof *f, _Alignof(FstestMapFile));
        f->data = spec[i].data == NULL ? slice_nil(TYPE_BYTE) : text(spec[i].data);
        f->mode = spec[i].mode;
        f->mod_time = now;
        f->sys = BURROW_ANY(TYPE_INT, &sys_value);
        fstest_map_fs_set(fsys, cstr(spec[i].name), f);
    }
    return fsys;
}

static const FileSpec test_fsys_spec[] = {
    {"hello.txt", "hello, world", 0456},
    {"sub/goodbye.txt", "goodbye, world", 0456},
};

static Fs test_fsys(Alloc *a) {
    return fstest_map_fs_as_fs(map_of(a, test_fsys_spec, 2));
}

/* The FS types Go's tests make by embedding one interface in a struct: Open
 * that always fails with ErrNotExist, and one method of the MapFS. */
static FstestMapFS as_map(void *self) {
    FstestMapFS m = {(Map *)self};
    return m;
}

static FsFile open_not_exist(void *self, Alloc *a, Str name, Error *err) {
    (void)self;
    (void)a;
    (void)name;
    *err = fs_err_not_exist;
    return (FsFile){NULL, NULL};
}

static FsFile map_open(void *self, Alloc *a, Str name, Error *err) {
    return fstest_map_fs_open(as_map(self), a, name, err);
}

static Slice map_read_dir(void *self, Alloc *a, Str name, Error *err) {
    return fstest_map_fs_read_dir(as_map(self), a, name, err);
}

static Slice map_read_file(void *self, Alloc *a, Str name, Error *err) {
    return fstest_map_fs_read_file(as_map(self), a, name, err);
}

static FsFileInfo map_stat(void *self, Alloc *a, Str name, Error *err) {
    return fstest_map_fs_stat(as_map(self), a, name, err);
}

static Fs map_sub(void *self, Alloc *a, Str dir, Error *err) {
    return fstest_map_fs_sub(as_map(self), a, dir, err);
}

static Slice map_glob(void *self, Alloc *a, Str pattern, Error *err) {
    return fstest_map_fs_glob(as_map(self), a, pattern, err);
}

static const FsVT open_only_vt = {NULL, map_open, NULL, NULL, NULL,
                                  NULL, NULL,     NULL, NULL};
static const FsVT read_dir_only_vt = {NULL, open_not_exist, map_read_dir, NULL, NULL,
                                      NULL, NULL,           NULL,         NULL};
static const FsVT read_file_only_vt = {NULL, open_not_exist, NULL, map_read_file, NULL,
                                       NULL, NULL,           NULL, NULL};
static const FsVT stat_only_vt = {NULL, open_not_exist, NULL, NULL, map_stat,
                                  NULL, NULL,           NULL, NULL};
static const FsVT sub_only_vt = {NULL,    open_not_exist, NULL, NULL, NULL,
                                 map_sub, NULL,           NULL, NULL};
static const FsVT glob_only_vt = {NULL, open_not_exist, NULL, NULL, NULL,
                                  NULL, map_glob,       NULL, NULL};

static Fs wrap(const FsVT *vt, Fs fsys) {
    return (Fs){vt, fsys.data};
}

/* errors.AsType[*PathError](err)'s Path, or "". */
static Str error_path(Error err) {
    const FsPathError *pe = (const FsPathError *)errors_as(err, TYPE_FS_PATH_ERROR);
    return pe == NULL ? BURROW_STR_EMPTY : pe->path;
}

/* ------------------------------------------------------------ ValidPath */

static const struct {
    const char *name;
    bool ok;
} is_valid_path_tests[] = {
    {".", true},     {"x", true},     {"x/y", true},

    {"", false},     {"..", false},   {"/", false},      {"x/", false},
    {"/x", false},   {"x/y/", false}, {"/x/y", false},   {"./", false},
    {"./x", false},  {"x/.", false},  {"x/./y", false},  {"../", false},
    {"../x", false}, {"x/..", false}, {"x/../y", false}, {"x//y", false},
    {"x\\", true},   {"x\\y", true},  {"x:y", true},     {"\\x", true},
};

static void TestValidPath(TestingT *t) {
    for (size_t i = 0; i < sizeof is_valid_path_tests / sizeof is_valid_path_tests[0];
         i++) {
        bool ok = fs_valid_path(cstr(is_valid_path_tests[i].name));
        if (ok != is_valid_path_tests[i].ok)
            testing_t_errorf_v(t, "ValidPath(%q) = %v, want %v",
                               is_valid_path_tests[i].name, ok,
                               is_valid_path_tests[i].ok);
    }
}

/* Not in Go's table, which never has anything but ASCII in it. */
static void TestValidPathUTF8(TestingT *t) {
    CHECK(fs_valid_path(BURROW_S("gr\xc3\xbc\xc3\x9f"
                                 "e/x")));
    CHECK(!fs_valid_path(BURROW_S("bad\xff"
                                  "name")));
}

/* ----------------------------------------------------------------- Glob */

/* The files os.DirFS(".") has in Go's io/fs directory, near enough, and
 * os.DirFS("..") seen from io. */
static const FileSpec dot_spec[] = {
    {"example_test.go", "", 0644}, {"format.go", "", 0644},    {"fs.go", "", 0644},
    {"glob.go", "", 0644},         {"glob_test.go", "", 0644}, {"readdir.go", "", 0644},
    {"sub.go", "", 0644},          {"walk.go", "", 0644},
};

static const FileSpec dotdot_spec[] = {
    {"fs/glob.go", "", 0644},
    {"fs/fs.go", "", 0644},
    {"io.go", "", 0644},
    {"pipe.go", "", 0644},
};

static bool contains(Slice names, const char *want) {
    for (Int i = 0; i < names.len; i++)
        if (str_is(*(Str *)slice_at(names, i), want))
            return true;
    return false;
}

static void TestGlob(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs dot = fstest_map_fs_as_fs(map_of(a, dot_spec, 8));
    Fs dotdot = fstest_map_fs_as_fs(map_of(a, dotdot_spec, 4));
    const struct {
        Fs fs;
        const char *pattern, *result;
    } tests[] = {
        {dot, "glob.go", "glob.go"},         {dot, "gl?b.go", "glob.go"},
        {dot, "gl\\ob.go", "glob.go"},       {dot, "*", "glob.go"},
        {dotdot, "*/glob.go", "fs/glob.go"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err;
        Slice matches = fs_glob(a, tests[i].fs, cstr(tests[i].pattern), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Glob error for %q: %v", tests[i].pattern, err);
            continue;
        }
        if (!contains(matches, tests[i].result))
            testing_t_errorf_v(t, "Glob(%#q) = %v want %v", tests[i].pattern, matches,
                               tests[i].result);
    }
    const char *none[] = {"no_match", "../*/no_match", "\\*"};
    for (size_t i = 0; i < 3; i++) {
        Error err;
        Slice matches = fs_glob(a, dot, cstr(none[i]), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Glob error for %q: %v", none[i], err);
            continue;
        }
        if (matches.len != 0)
            testing_t_errorf_v(t, "Glob(%#q) = %v want []", none[i], matches);
    }
    arena_free(&ar);
}

static void TestGlobError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs dot = fstest_map_fs_as_fs(map_of(a, dot_spec, 8));
    const char *bad[] = {"[]", "nonexist/[]"};
    for (size_t i = 0; i < 2; i++) {
        Error err;
        (void)fs_glob(a, dot, cstr(bad[i]), &err);
        if (!same_error(err, path_err_bad_pattern))
            testing_t_errorf_v(t,
                               "Glob(fs, %#q) returned err=%v, want path.ErrBadPattern",
                               bad[i], err);
    }
    arena_free(&ar);
}

static void TestCVE202230630(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs dot = fstest_map_fs_as_fs(map_of(a, dot_spec, 8));
    Int n = 2 + 10001;
    Byte *p = (Byte *)mem_alloc(a, (size_t)n, 1);
    p[0] = '/';
    p[1] = '*';
    memset(p + 2, '/', 10001);
    Error err;
    (void)fs_glob(a, dot, str_from_bytes(p, n), &err);
    if (!same_error(err, path_err_bad_pattern))
        testing_t_errorf_v(t, "Glob returned err=%v, want %v", err,
                           path_err_bad_pattern);
    arena_free(&ar);
}

static void check_glob(TestingT *t, const char *desc, Slice names, Error err) {
    if (BURROW_FAILED(err) || names.len != 1 ||
        !str_is(*(Str *)slice_at(names, 0), "hello.txt"))
        testing_t_errorf_v(t, "Glob(%s) = %v, %v, want [hello.txt], nil", desc, names,
                           err);
}

static void TestGlobMethod(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs fsys = test_fsys(a);
    Error err;

    /* Test that Glob uses the method when present. */
    Slice names = fs_glob(a, wrap(&glob_only_vt, fsys), BURROW_S("*.txt"), &err);
    check_glob(t, "readDirOnly", names, err);

    /* Test that Glob uses Open when the method is not present. */
    names = fs_glob(a, wrap(&open_only_vt, fsys), BURROW_S("*.txt"), &err);
    check_glob(t, "openOnly", names, err);
    arena_free(&ar);
}

/* -------------------------------------------------------------- ReadDir */

static void check_read_dir(TestingT *t, Alloc *a, const char *desc, Slice dirs,
                           Error err) {
    if (BURROW_FAILED(err) || dirs.len != 2 ||
        !str_is(((FsDirEntry *)slice_at(dirs, 0))
                    ->vt->name(((FsDirEntry *)slice_at(dirs, 0))->data),
                "hello.txt") ||
        !str_is(((FsDirEntry *)slice_at(dirs, 1))
                    ->vt->name(((FsDirEntry *)slice_at(dirs, 1))->data),
                "sub"))
        testing_t_errorf_v(t, "ReadDir(%s) = [%s], %v, want [hello.txt sub], nil", desc,
                           entry_names(a, dirs), err);
}

static void TestReadDir(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs fsys = test_fsys(a);
    Error err;

    /* Test that ReadDir uses the method when present. */
    Slice dirs = fs_read_dir(a, wrap(&read_dir_only_vt, fsys), BURROW_S("."), &err);
    check_read_dir(t, a, "readDirOnly", dirs, err);

    /* Test that ReadDir uses Open when the method is not present. */
    dirs = fs_read_dir(a, wrap(&open_only_vt, fsys), BURROW_S("."), &err);
    check_read_dir(t, a, "openOnly", dirs, err);

    /* Test that ReadDir on Sub of . works (sub_test checks non-trivial subs). */
    Fs sub = fs_sub(a, fsys, BURROW_S("."), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        arena_free(&ar);
        return;
    }
    dirs = fs_read_dir(a, sub, BURROW_S("."), &err);
    check_read_dir(t, a, "sub(.)", dirs, err);
    arena_free(&ar);
}

static void TestFileInfoToDirEntry(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const FileSpec spec[] = {
        {"notadir.txt", "hello, world", 0},
        {"adir", NULL, FS_MODE_DIR},
    };
    Fs test_fs = fstest_map_fs_as_fs(map_of(a, spec, 2));
    const struct {
        const char *path;
        FsFileMode want_mode;
        bool want_dir;
    } tests[] = {
        {"notadir.txt", 0, false},
        {"adir", FS_MODE_DIR, true},
    };
    for (size_t i = 0; i < 2; i++) {
        Error err;
        FsFileInfo fi = fs_stat(a, test_fs, cstr(tests[i].path), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: %v", tests[i].path, err);
            continue;
        }
        FsDirEntry d = fs_file_info_to_dir_entry(a, fi);
        FsFileMode g = d.vt->type(d.data);
        if (g != tests[i].want_mode)
            testing_t_errorf_v(t, "%s: FileMode mismatch: got=%s, want=%s",
                               tests[i].path, mode_str(a, g),
                               mode_str(a, tests[i].want_mode));
        if (!str_is(d.vt->name(d.data), tests[i].path))
            testing_t_errorf_v(t, "%s: Name mismatch: got=%s, want=%s", tests[i].path,
                               d.vt->name(d.data), tests[i].path);
        if (d.vt->is_dir(d.data) != tests[i].want_dir)
            testing_t_errorf_v(t, "%s: IsDir mismatch: got=%v, want=%v", tests[i].path,
                               d.vt->is_dir(d.data), tests[i].want_dir);
        FsFileInfo back = d.vt->info(d.data, a, &err);
        CHECK(BURROW_OK(err) && back.data == fi.data);
    }
    FsFileInfo nil_info = {NULL, NULL};
    CHECK(fs_file_info_to_dir_entry(a, nil_info).vt == NULL);
    arena_free(&ar);
}

static void TestReadDirPath(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs fsys = test_fsys(a);
    Error err1, err2;
    (void)fs_read_dir(a, fsys, BURROW_S("non-existent"), &err1);
    (void)fs_read_dir(a, wrap(&open_only_vt, fsys), BURROW_S("non-existent"), &err2);
    Str s1 = error_path(err1), s2 = error_path(err2);
    if (!str_eq(s1, s2) || !str_is(s1, "non-existent"))
        testing_t_fatalf_v(t, "s1: %s != s2: %s", s1, s2);
    arena_free(&ar);
}

/* ------------------------------------------------------------- ReadFile */

static void TestReadFile(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs fsys = test_fsys(a);
    Error err;

    /* Test that ReadFile uses the method when present. */
    Slice data =
        fs_read_file(a, wrap(&read_file_only_vt, fsys), BURROW_S("hello.txt"), &err);
    if (!bytes_are(data, "hello, world") || BURROW_FAILED(err))
        testing_t_errorf_v(
            t, "ReadFile(readFileOnly, \"hello.txt\") = %q, %v, want %q, nil", data,
            err, "hello, world");

    /* Test that ReadFile uses Open when the method is not present. */
    data = fs_read_file(a, wrap(&open_only_vt, fsys), BURROW_S("hello.txt"), &err);
    if (!bytes_are(data, "hello, world") || BURROW_FAILED(err))
        testing_t_errorf_v(t,
                           "ReadFile(openOnly, \"hello.txt\") = %q, %v, want %q, nil",
                           data, err, "hello, world");

    /* Test that ReadFile on Sub of . works (sub_test checks non-trivial subs). */
    Fs sub = fs_sub(a, fsys, BURROW_S("."), &err);
    CHECK(BURROW_OK(err));
    data = fs_read_file(a, sub, BURROW_S("hello.txt"), &err);
    if (!bytes_are(data, "hello, world") || BURROW_FAILED(err))
        testing_t_errorf_v(t, "ReadFile(sub(.), \"hello.txt\") = %q, %v, want %q, nil",
                           data, err, "hello, world");
    arena_free(&ar);
}

static void TestReadFilePath(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs fsys = test_fsys(a);
    Error err1, err2;
    (void)fs_read_file(a, fsys, BURROW_S("non-existent"), &err1);
    (void)fs_read_file(a, wrap(&open_only_vt, fsys), BURROW_S("non-existent"), &err2);
    Str s1 = error_path(err1), s2 = error_path(err2);
    if (!str_eq(s1, s2) || !str_is(s1, "non-existent"))
        testing_t_fatalf_v(t, "s1: %s != s2: %s", s1, s2);
    arena_free(&ar);
}

/* A file whose stat says it is empty and whose reads give n bytes, a few at a
 * time, so that fs_read_file has to grow its buffer. */
typedef struct Liar {
    Int left;
    Int total;
} Liar;

static Int liar_read(void *self, Slice p, Error *err) {
    Liar *l = (Liar *)self;
    *err = BURROW_NO_ERROR;
    if (l->left == 0) {
        *err = io_eof;
        return 0;
    }
    Int n = p.len < 7 ? p.len : 7;
    if (n > l->left)
        n = l->left;
    for (Int i = 0; i < n; i++)
        ((Byte *)p.p)[i] = (Byte)('a' + (l->total - l->left + i) % 26);
    l->left -= n;
    return n;
}

static Error liar_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static int64_t liar_size(void *self) {
    (void)self;
    return 0;
}

static FsFileMode liar_mode(void *self) {
    (void)self;
    return 0444;
}

static Str liar_name(void *self) {
    (void)self;
    return BURROW_S("liar");
}

static Time liar_time(void *self) {
    (void)self;
    Time zero = {0};
    return zero;
}

static bool liar_is_dir(void *self) {
    (void)self;
    return false;
}

static Any liar_sys(void *self) {
    (void)self;
    Any nil = {NULL, NULL};
    return nil;
}

static const FsFileInfoVT liar_info_vt = {NULL,      liar_name,   liar_size, liar_mode,
                                          liar_time, liar_is_dir, liar_sys};

static FsFileInfo liar_stat(void *self, Alloc *a, Error *err) {
    (void)a;
    *err = BURROW_NO_ERROR;
    return (FsFileInfo){&liar_info_vt, self};
}

static const FsFileVT liar_file_vt = {
    {{NULL, liar_read}, {NULL, liar_close}},
    liar_stat,
    NULL,
};

static FsFile liar_open(void *self, Alloc *a, Str name, Error *err) {
    (void)a;
    (void)name;
    *err = BURROW_NO_ERROR;
    return (FsFile){&liar_file_vt, self};
}

static const FsVT liar_fs_vt = {NULL, liar_open, NULL, NULL, NULL,
                                NULL, NULL,      NULL, NULL};

static void TestReadFileGrows(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Liar l = {10000, 10000};
    Error err;
    Slice data = fs_read_file(a, (Fs){&liar_fs_vt, &l}, BURROW_S("liar"), &err);
    CHECK(BURROW_OK(err));
    CHECK_INT_EQ(data.len, 10000);
    bool ok = true;
    for (Int i = 0; i < data.len && ok; i++)
        ok = ((Byte *)data.p)[i] == (Byte)('a' + i % 26);
    CHECK(ok);

    /* And the Stat that falls back to Open gives the file's own. */
    FsFileInfo fi = fs_stat(a, (Fs){&liar_fs_vt, &l}, BURROW_S("liar"), &err);
    CHECK(BURROW_OK(err) && str_is(fi.vt->name(fi.data), "liar"));

    /* A file with no read_dir is not a directory ReadDir can read. */
    Slice dirs = fs_read_dir(a, (Fs){&liar_fs_vt, &l}, BURROW_S("liar"), &err);
    CHECK(dirs.len == 0);
    CHECK_IS(error_text(err), "readdir liar: not implemented");
    arena_free(&ar);
}

/* ------------------------------------------------------------- ReadLink */

static const FileSpec link_spec[] = {
    {"foo", "bar", FS_MODE_SYMLINK | 0777},
    {"bar", "Hello, World!\n", 0644},
    {"dir/parentlink", "../bar", FS_MODE_SYMLINK | 0777},
    {"dir/link", "file", FS_MODE_SYMLINK | 0777},
    {"dir/file", "Hello, World!\n", 0644},
};

static void check_link(TestingT *t, Alloc *a, Fs fsys, const char *name,
                       const char *want) {
    Error err;
    Str got = fs_read_link(a, fsys, cstr(name), &err);
    if (!str_is(got, want) || BURROW_FAILED(err))
        testing_t_errorf_v(t, "ReadLink(%q) = %q, %v; want %q, <nil>", name, got, err,
                           want);
}

static void TestReadLink(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs test_fs = fstest_map_fs_as_fs(map_of(a, link_spec, 5));

    check_link(t, a, test_fs, "foo", "bar");
    check_link(t, a, test_fs, "dir/parentlink", "../bar");
    check_link(t, a, test_fs, "dir/link", "file");

    /* Test that ReadLink on Sub works. */
    Error err;
    Fs sub = fs_sub(a, test_fs, BURROW_S("dir"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        arena_free(&ar);
        return;
    }
    check_link(t, a, sub, "link", "file");
    check_link(t, a, sub, "parentlink", "../bar");

    /* Not in Go's test: what the failures look like. */
    (void)fs_read_link(a, test_fs, BURROW_S("bar"), &err);
    CHECK_IS(error_text(err), "readlink bar: invalid argument");
    CHECK(errors_is(err, fs_err_invalid));
    (void)fs_read_link(a, test_fs, BURROW_S("nope"), &err);
    CHECK(errors_is(err, fs_err_not_exist));
    (void)fs_read_link(a, wrap(&open_only_vt, test_fs), BURROW_S("foo"), &err);
    CHECK_IS(error_text(err), "readlink foo: invalid argument");
    (void)fs_read_link(a, sub, BURROW_S("nope"), &err);
    CHECK_IS(error_text(err), "readlink nope: file does not exist");
    arena_free(&ar);
}

static void check_lstat(TestingT *t, Alloc *a, Fs fsys, const char *name,
                        FsFileMode want) {
    Error err;
    FsFileInfo info = fs_lstat(a, fsys, cstr(name), &err);
    FsFileMode got = 0;
    if (BURROW_OK(err))
        got = info.vt->mode(info.data);
    if (got != want || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Lstat(%q) = %s, %v; want %s, <nil>", name,
                           mode_str(a, got), err, mode_str(a, want));
}

static void TestLstat(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs test_fs = fstest_map_fs_as_fs(map_of(a, link_spec, 5));

    check_lstat(t, a, test_fs, "foo", FS_MODE_SYMLINK | 0777);
    check_lstat(t, a, test_fs, "bar", 0644);

    /* Test that Lstat on Sub works. */
    Error err;
    Fs sub = fs_sub(a, test_fs, BURROW_S("dir"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        arena_free(&ar);
        return;
    }
    check_lstat(t, a, sub, "link", FS_MODE_SYMLINK | 0777);

    /* Without the method, Lstat is Stat, which follows the link. */
    check_lstat(t, a, wrap(&open_only_vt, test_fs), "foo", 0644);
    arena_free(&ar);
}

/* ----------------------------------------------------------------- Stat */

static void check_stat(TestingT *t, const char *desc, FsFileInfo info, Error err) {
    if (BURROW_FAILED(err) || info.vt == NULL || info.vt->mode(info.data) != 0456)
        testing_t_fatalf_v(t, "Stat(%s) = %v, want Mode:0456, nil", desc, err);
}

static void TestStat(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs fsys = test_fsys(a);
    Error err;

    /* Test that Stat uses the method when present. */
    FsFileInfo info =
        fs_stat(a, wrap(&stat_only_vt, fsys), BURROW_S("hello.txt"), &err);
    check_stat(t, "statOnly", info, err);

    /* Test that Stat uses Open when the method is not present. */
    info = fs_stat(a, wrap(&open_only_vt, fsys), BURROW_S("hello.txt"), &err);
    check_stat(t, "openOnly", info, err);
    arena_free(&ar);
}

/* ------------------------------------------------------------------ Sub */

static void check_sub(TestingT *t, Alloc *a, const char *desc, Fs sub, Error err) {
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Sub(sub): %v", err);
        return;
    }
    Slice data = fs_read_file(a, sub, BURROW_S("goodbye.txt"), &err);
    if (!bytes_are(data, "goodbye, world") || BURROW_FAILED(err))
        testing_t_errorf_v(t, "ReadFile(%s, \"goodbye.txt\" = %q, %v, want %q, nil",
                           desc, data, err, "goodbye, world");

    Slice dirs = fs_read_dir(a, sub, BURROW_S("."), &err);
    if (BURROW_FAILED(err) || dirs.len != 1 ||
        !str_is(((FsDirEntry *)slice_at(dirs, 0))
                    ->vt->name(((FsDirEntry *)slice_at(dirs, 0))->data),
                "goodbye.txt"))
        testing_t_errorf_v(t, "ReadDir(%s, \".\") = [%s], %v, want [goodbye.txt], nil",
                           desc, entry_names(a, dirs), err);
}

static void TestSub(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs fsys = test_fsys(a);
    Error err;

    /* Test that Sub uses the method when present. */
    Fs sub = fs_sub(a, wrap(&sub_only_vt, fsys), BURROW_S("sub"), &err);
    check_sub(t, a, "subOnly", sub, err);

    /* Test that Sub uses Open when the method is not present. */
    sub = fs_sub(a, wrap(&open_only_vt, fsys), BURROW_S("sub"), &err);
    check_sub(t, a, "openOnly", sub, err);

    (void)sub.vt->open(sub.data, a, BURROW_S("nonexist"), &err);
    if (BURROW_OK(err)) {
        testing_t_fatalf_v(t, "Open(nonexist): succeeded");
        arena_free(&ar);
        return;
    }
    const FsPathError *pe = (const FsPathError *)errors_as(err, TYPE_FS_PATH_ERROR);
    if (pe == NULL) {
        testing_t_fatalf_v(t, "Open(nonexist): error is %v, want *PathError", err);
        arena_free(&ar);
        return;
    }
    if (!str_is(pe->path, "nonexist"))
        testing_t_fatalf_v(t, "Open(nonexist): err.Path = %q, want %q", pe->path,
                           "nonexist");

    (void)sub.vt->open(sub.data, a, BURROW_S("./"), &err);
    if (!errors_is(err, fs_err_invalid))
        testing_t_fatalf_v(t, "Open(./): error is %v, want %v", err, fs_err_invalid);
    arena_free(&ar);
}

/* Not in Go's tests: the rest of the subFS methods, and a Sub of a Sub. */
static void TestSubMethods(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const FileSpec spec[] = {
        {"a/b/c.txt", "c", 0644},
        {"a/b/d.go", "d", 0644},
        {"a/e.txt", "e", 0644},
    };
    Fs fsys = wrap(&open_only_vt, fstest_map_fs_as_fs(map_of(a, spec, 3)));
    Error err;
    Fs sub = fs_sub(a, fsys, BURROW_S("a"), &err);
    CHECK(BURROW_OK(err));

    Slice names = fs_glob(a, sub, BURROW_S("*/*.txt"), &err);
    if (BURROW_FAILED(err) || names.len != 1 ||
        !str_is(*(Str *)slice_at(names, 0), "b/c.txt"))
        testing_t_errorf_v(t, "Glob(sub, \"*/*.txt\") = %v, %v, want [b/c.txt]", names,
                           err);
    names = fs_glob(a, sub, BURROW_S("."), &err);
    CHECK(BURROW_OK(err) && names.len == 1 && str_is(*(Str *)slice_at(names, 0), "."));
    (void)fs_glob(a, sub, BURROW_S("["), &err);
    CHECK(same_error(err, path_err_bad_pattern));

    Fs sub2 = fs_sub(a, sub, BURROW_S("b"), &err);
    CHECK(BURROW_OK(err));
    Slice data = fs_read_file(a, sub2, BURROW_S("d.go"), &err);
    CHECK(BURROW_OK(err) && bytes_are(data, "d"));
    Fs same = fs_sub(a, sub2, BURROW_S("."), &err);
    CHECK(BURROW_OK(err) && same.data == sub2.data);
    (void)fs_sub(a, sub, BURROW_S("../x"), &err);
    CHECK_IS(error_text(err), "sub ../x: invalid argument");
    (void)fs_sub(a, sub2, BURROW_S("/x"), &err);
    CHECK_IS(error_text(err), "sub /x: invalid argument");

    (void)fs_read_file(a, sub2, BURROW_S("zz"), &err);
    CHECK_IS(error_text(err), "open zz: file does not exist");
    (void)fs_read_dir(a, sub2, BURROW_S("x/"), &err);
    CHECK_IS(error_text(err), "read x/: invalid argument");

    /* The inner FS has no ReadLink, so the error is the one fs_read_link
     * makes, with its path made relative again. */
    (void)fs_read_link(a, sub2, BURROW_S("d.go"), &err);
    CHECK_IS(error_text(err), "readlink d.go: invalid argument");
    FsFileInfo fi = fs_lstat(a, sub2, BURROW_S("c.txt"), &err);
    CHECK(BURROW_OK(err) && str_is(fi.vt->name(fi.data), "c.txt"));
    arena_free(&ar);
}

/* ----------------------------------------------------------------- Walk */

typedef struct Node {
    const char *name;
    const struct Node *entries; /* NULL for a file */
    int n;
    int *mark;
} Node;

static int marks[11];

static const Node z_entries[] = {{"u", NULL, 0, &marks[9]}, {"v", NULL, 0, &marks[10]}};
static const Node empty_dir[1] = {{NULL, NULL, 0, NULL}};
static const Node d_entries[] = {
    {"x", NULL, 0, &marks[5]},
    {"y", empty_dir, 0, &marks[6]},
    {"z", z_entries, 2, &marks[7]},
};
static const Node testdata_entries[] = {
    {"a", NULL, 0, &marks[1]},
    {"b", empty_dir, 0, &marks[2]},
    {"c", NULL, 0, &marks[3]},
    {"d", d_entries, 3, &marks[4]},
};
static const Node tree = {"testdata", testdata_entries, 4, &marks[0]};

typedef void (*WalkTreeFn)(void *env, Str path, const Node *n);

static void walk_tree(Alloc *a, const Node *n, Str path, WalkTreeFn f, void *env) {
    f(env, path, n);
    for (int i = 0; i < n->n; i++) {
        const Node *e = &n->entries[i];
        walk_tree(a, e, path_join_v(a, 2, path, cstr(e->name)), f, env);
    }
}

static void add_node(void *env, Str path, const Node *n) {
    FstestMapFS *fsys = (FstestMapFS *)env;
    Alloc *a = burrow__map_allocator(fsys->files);
    FstestMapFile *f =
        (FstestMapFile *)mem_alloc(a, sizeof *f, _Alignof(FstestMapFile));
    if (n->entries != NULL)
        f->mode = FS_MODE_DIR;
    fstest_map_fs_set(*fsys, path, f);
}

static Fs make_tree(Alloc *a) {
    FstestMapFS fsys = fstest_map_fs_make(a);
    walk_tree(a, &tree, cstr(tree.name), add_node, &fsys);
    return fstest_map_fs_as_fs(fsys);
}

typedef struct MarkEnv {
    Alloc *a;
    Slice errors;
    bool clear;
} MarkEnv;

static void mark_name(void *env, Str path, const Node *n) {
    (void)path;
    if (str_is(*(Str *)env, n->name))
        (*n->mark)++;
}

static Error mark(void *env, Str path, FsDirEntry entry, Error err) {
    (void)path;
    MarkEnv *m = (MarkEnv *)env;
    Str name = entry.vt->name(entry.data);
    walk_tree(m->a, &tree, cstr(tree.name), mark_name, &name);
    if (BURROW_FAILED(err)) {
        m->errors = slice_append(m->a, m->errors, &err, 1);
        if (m->clear)
            return BURROW_NO_ERROR;
        return err;
    }
    return BURROW_NO_ERROR;
}

typedef struct CheckEnv {
    TestingT *t;
} CheckEnv;

static void check_mark(void *env, Str path, const Node *n) {
    TestingT *t = ((CheckEnv *)env)->t;
    if (*n->mark != 1)
        testing_t_errorf_v(t, "node %s mark = %d; expected 1", path, *n->mark);
    *n->mark = 0;
}

static void TestWalkDir(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs fsys = make_tree(a);
    MarkEnv env = {a, slice_nil(TYPE_ERROR), true};

    /* Expect no errors. */
    Error err =
        fs_walk_dir(a, fsys, BURROW_S("."), BURROW_FN(FsWalkDirFunc, mark, &env));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "no error expected, found: %v", err);
        arena_free(&ar);
        return;
    }
    if (env.errors.len != 0) {
        testing_t_fatalf_v(t, "unexpected errors: %v", env.errors);
        arena_free(&ar);
        return;
    }
    CheckEnv check = {t};
    walk_tree(a, &tree, cstr(tree.name), check_mark, &check);
    arena_free(&ar);
}

typedef struct SymlinkEnv {
    TestingT *t;
    Alloc *a;
    Map *marks;
} SymlinkEnv;

static const struct {
    const char *path;
    FsFileMode mode;
} want_types[] = {
    {"link", FS_MODE_DIR},       {"link/a", 0},
    {"link/b", FS_MODE_DIR},     {"link/b/c", 0},
    {"link/d", FS_MODE_SYMLINK},
};

static Error symlink_walk(void *env, Str path, FsDirEntry entry, Error err) {
    SymlinkEnv *e = (SymlinkEnv *)env;
    TestingT *t = e->t;
    Int *n = (Int *)map_get(e->marks, &path);
    if (n != NULL) {
        (*n)++;
    } else {
        Int one = 1;
        map_set(e->marks, &path, &one);
    }
    bool found = false;
    for (size_t i = 0; i < sizeof want_types / sizeof want_types[0]; i++) {
        if (!str_is(path, want_types[i].path))
            continue;
        found = true;
        FsFileMode got = entry.vt->type(entry.data);
        if (got != want_types[i].mode)
            testing_t_errorf_v(t, "%s entry type = %s; want %s", path,
                               mode_str(e->a, got), mode_str(e->a, want_types[i].mode));
    }
    if (!found)
        testing_t_errorf_v(t, "Unexpected path %q in walk", path);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Walking %s: %v", path, err);
    return BURROW_NO_ERROR;
}

static void TestWalkDirSymlink(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const FileSpec spec[] = {
        {"link", "dir", FS_MODE_SYMLINK},
        {"dir/a", NULL, 0},
        {"dir/b/c", NULL, 0},
        {"dir/d", "b", FS_MODE_SYMLINK},
    };
    Fs fsys = fstest_map_fs_as_fs(map_of(a, spec, 4));
    SymlinkEnv env = {t, a, map_make(a, TYPE_STRING, TYPE_INT, 0)};

    /* Expect no errors. */
    Error err = fs_walk_dir(a, fsys, BURROW_S("link"),
                            BURROW_FN(FsWalkDirFunc, symlink_walk, &env));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "no error expected, found: %v", err);
        arena_free(&ar);
        return;
    }
    for (size_t i = 0; i < sizeof want_types / sizeof want_types[0]; i++) {
        Str path = cstr(want_types[i].path);
        Int *got = (Int *)map_get(env.marks, &path);
        if (got == NULL || *got != 1)
            testing_t_errorf_v(t, "%s visited %d times; expected 1", path,
                               got == NULL ? (Int)0 : *got);
    }
    arena_free(&ar);
}

/* os.DirFS with a directory chmod'ed to 0 in Go. Here the FS itself refuses
 * to open "a/bad". */
static FsFile bad_open(void *self, Alloc *a, Str name, Error *err) {
    if (str_is(name, "a/bad")) {
        *err = fs_path_error_new(a, BURROW_S("open"), name, fs_err_permission);
        return (FsFile){NULL, NULL};
    }
    return fstest_map_fs_open(as_map(self), a, name, err);
}

static FsFileInfo bad_stat(void *self, Alloc *a, Str name, Error *err) {
    return fstest_map_fs_stat(as_map(self), a, name, err);
}

static const FsVT bad_vt = {NULL, bad_open, NULL, NULL, bad_stat,
                            NULL, NULL,     NULL, NULL};

typedef struct SawEnv {
    Alloc *a;
    Slice saw;
} SawEnv;

static Error saw_dirs(void *env, Str path, FsDirEntry d, Error err) {
    SawEnv *s = (SawEnv *)env;
    if (BURROW_FAILED(err))
        return fs_skip_dir;
    if (d.vt->is_dir(d.data))
        s->saw = slice_append(s->a, s->saw, &path, 1);
    return BURROW_NO_ERROR;
}

static void TestIssue51617(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const FileSpec spec[] = {
        {"a", NULL, FS_MODE_DIR | 0755},
        {"a/bad", NULL, FS_MODE_DIR},
        {"a/next", NULL, FS_MODE_DIR | 0755},
    };
    Fs fsys = {&bad_vt, map_of(a, spec, 3).files};
    SawEnv env = {a, slice_nil(TYPE_STRING)};
    Error err =
        fs_walk_dir(a, fsys, BURROW_S("."), BURROW_FN(FsWalkDirFunc, saw_dirs, &env));
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        arena_free(&ar);
        return;
    }
    const char *want[] = {".", "a", "a/bad", "a/next"};
    bool ok = env.saw.len == 4;
    for (Int i = 0; ok && i < 4; i++)
        ok = str_is(*(Str *)slice_at(env.saw, i), want[i]);
    if (!ok)
        testing_t_errorf_v(t, "got directories %v, want [. a a/bad a/next]", env.saw);
    arena_free(&ar);
}

/* Not in Go's tests: SkipDir from a file, SkipAll, a stop with an error of
 * the function's own, and a root that is not there. */
typedef struct SkipEnv {
    Alloc *a;
    Slice seen;
    const char *skip_dir_at;
    const char *skip_all_at;
    const char *fail_at;
    Error fail;
    Error root_err;
    bool root_nil;
} SkipEnv;

static Error skip_walk(void *env, Str path, FsDirEntry d, Error err) {
    SkipEnv *s = (SkipEnv *)env;
    if (BURROW_FAILED(err)) {
        s->root_err = err;
        s->root_nil = d.vt == NULL;
        return err;
    }
    s->seen = slice_append(s->a, s->seen, &path, 1);
    if (s->skip_dir_at != NULL && str_is(path, s->skip_dir_at))
        return fs_skip_dir;
    if (s->skip_all_at != NULL && str_is(path, s->skip_all_at))
        return fs_skip_all;
    if (s->fail_at != NULL && str_is(path, s->fail_at))
        return s->fail;
    return BURROW_NO_ERROR;
}

static Str joined(Alloc *a, Slice names) {
    return strings_join(a, names, BURROW_S(" "));
}

static void TestWalkDirSkip(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Fs fsys = make_tree(a);
    FsWalkDirFunc fn;

    SkipEnv s = {a, slice_nil(TYPE_STRING), "testdata/d", NULL, NULL, {0}, {0}, false};
    fn = BURROW_FN(FsWalkDirFunc, skip_walk, &s);
    CHECK(BURROW_OK(fs_walk_dir(a, fsys, BURROW_S("testdata"), fn)));
    CHECK_IS(joined(a, s.seen), "testdata testdata/a testdata/b testdata/c testdata/d");

    /* SkipDir from a file skips what is left of its directory. */
    SkipEnv f = {a,    slice_nil(TYPE_STRING), "testdata/d/x", NULL, NULL, {0}, {0},
                 false};
    fn = BURROW_FN(FsWalkDirFunc, skip_walk, &f);
    CHECK(BURROW_OK(fs_walk_dir(a, fsys, BURROW_S("testdata/d"), fn)));
    CHECK_IS(joined(a, f.seen), "testdata/d testdata/d/x");

    SkipEnv all = {a,    slice_nil(TYPE_STRING), NULL, "testdata/c", NULL, {0}, {0},
                   false};
    fn = BURROW_FN(FsWalkDirFunc, skip_walk, &all);
    CHECK(BURROW_OK(fs_walk_dir(a, fsys, BURROW_S("testdata"), fn)));
    CHECK_IS(joined(a, all.seen), "testdata testdata/a testdata/b testdata/c");

    Error mine = errors_new(a, BURROW_S("mine"));
    SkipEnv fail = {a,    slice_nil(TYPE_STRING), NULL, NULL, "testdata/b", mine, {0},
                    false};
    fn = BURROW_FN(FsWalkDirFunc, skip_walk, &fail);
    CHECK(same_error(fs_walk_dir(a, fsys, BURROW_S("testdata"), fn), mine));

    SkipEnv root = {a, slice_nil(TYPE_STRING), NULL, NULL, NULL, {0}, {0}, false};
    fn = BURROW_FN(FsWalkDirFunc, skip_walk, &root);
    Error err = fs_walk_dir(a, fsys, BURROW_S("nothere"), fn);
    CHECK(errors_is(err, fs_err_not_exist) && root.root_nil);
    CHECK_IS(error_text(err), "open nothere: file does not exist");
    arena_free(&ar);
}

/* --------------------------------------------------------------- format */

typedef struct FormatTest {
    const char *name;
    int64_t size;
    FsFileMode mode;
    Time mod_time;
    bool is_dir;
} FormatTest;

static Str ft_name(void *self) {
    return cstr(((FormatTest *)self)->name);
}

static int64_t ft_size(void *self) {
    return ((FormatTest *)self)->size;
}

static FsFileMode ft_mode(void *self) {
    return ((FormatTest *)self)->mode;
}

static Time ft_mod_time(void *self) {
    return ((FormatTest *)self)->mod_time;
}

static bool ft_is_dir(void *self) {
    return ((FormatTest *)self)->is_dir;
}

static Any ft_sys(void *self) {
    (void)self;
    Any nil = {NULL, NULL};
    return nil;
}

static FsFileMode ft_type(void *self) {
    return fs_file_mode_type(((FormatTest *)self)->mode);
}

static const FsFileInfoVT ft_info_vt = {NULL,        ft_name,   ft_size, ft_mode,
                                        ft_mod_time, ft_is_dir, ft_sys};

static FsFileInfo ft_info(void *self, Alloc *a, Error *err) {
    (void)a;
    *err = BURROW_NO_ERROR;
    return (FsFileInfo){&ft_info_vt, self};
}

static const FsDirEntryVT ft_entry_vt = {NULL, ft_name, ft_is_dir, ft_type, ft_info};

static const struct {
    const char *name;
    int64_t size;
    FsFileMode mode;
    bool is_dir;
    const char *want_file_info;
    const char *want_dir_entry;
} format_tests[] = {
    {"hello.go", 100, 0644, false, "-rw-r--r-- 100 1970-01-01 12:00:00 hello.go",
     "- hello.go"},
    {"home/gopher", 0, FS_MODE_DIR | 0755, true,
     "drwxr-xr-x 0 1970-01-01 12:00:00 home/gopher/", "d home/gopher/"},
    {"big", INT64_MAX, FS_MODE_IRREGULAR | 0644, false,
     "?rw-r--r-- 9223372036854775807 1970-01-01 12:00:00 big", "? big"},
    {"small", INT64_MIN, FS_MODE_SOCKET | FS_MODE_SETUID | 0644, false,
     "Surw-r--r-- -9223372036854775808 1970-01-01 12:00:00 small", "S small"},
};

static FormatTest format_input(int i) {
    FormatTest ft = {format_tests[i].name, format_tests[i].size, format_tests[i].mode,
                     time_date(1970, TIME_JANUARY, 1, 12, 0, 0, 0, time_utc_loc),
                     format_tests[i].is_dir};
    return ft;
}

static void TestFormatFileInfo(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < 4; i++) {
        FormatTest input = format_input(i);
        Str got = fs_format_file_info(a, (FsFileInfo){&ft_info_vt, &input});
        if (!str_is(got, format_tests[i].want_file_info))
            testing_t_errorf_v(t, "%d: FormatFileInfo(%s) = %q, want %q", i, input.name,
                               got, format_tests[i].want_file_info);
    }
    arena_free(&ar);
}

static void TestFormatDirEntry(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < 4; i++) {
        FormatTest input = format_input(i);
        Str got = fs_format_dir_entry(a, (FsDirEntry){&ft_entry_vt, &input});
        if (!str_is(got, format_tests[i].want_dir_entry))
            testing_t_errorf_v(t, "%d: FormatDirEntry(%s) = %q, want %q", i, input.name,
                               got, format_tests[i].want_dir_entry);
    }
    arena_free(&ar);
}

/* Not in Go's tests: every letter of FileMode.String, and the predicates. */
static void TestFileModeString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CHECK(str_is(mode_str(a, 0), "----------"));
    CHECK(str_is(mode_str(a, 0777), "-rwxrwxrwx"));
    CHECK(str_is(mode_str(a, 0xfff80000U | 0751), "dalTLDpSugct?rwxr-x--x"));
    CHECK(str_is(mode_str(a, FS_MODE_DIR | 0555), "dr-xr-xr-x"));
    CHECK(str_is(mode_str(a, FS_MODE_SYMLINK), "L---------"));
    CHECK(fs_file_mode_is_dir(FS_MODE_DIR | 0755));
    CHECK(!fs_file_mode_is_dir(0755));
    CHECK(fs_file_mode_is_regular(FS_MODE_SETUID | FS_MODE_APPEND | 0755));
    CHECK(!fs_file_mode_is_regular(FS_MODE_NAMED_PIPE));
    CHECK_INT_EQ(fs_file_mode_perm(FS_MODE_DIR | FS_MODE_STICKY | 01777), 0777);
    CHECK_INT_EQ(fs_file_mode_type(FS_MODE_DIR | FS_MODE_STICKY | 0777), FS_MODE_DIR);
    CHECK_INT_EQ(FS_MODE_TYPE, 0x8f280000U);
    arena_free(&ar);
}

/* ------------------------------------------------------------ PathError */

static void TestPathError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = fs_path_error_new(a, BURROW_S("open"), BURROW_S("x/y"), fs_err_exist);
    CHECK_IS(error_text(err), "open x/y: file already exists");
    CHECK(errors_is(err, fs_err_exist));
    CHECK(!errors_is(err, fs_err_not_exist));
    CHECK(same_error(errors_unwrap(err), fs_err_exist));
    const FsPathError *pe = (const FsPathError *)errors_as(err, TYPE_FS_PATH_ERROR);
    CHECK(pe != NULL);
    if (pe == NULL) {
        arena_free(&ar);
        return;
    }
    CHECK(str_is(pe->op, "open") && str_is(pe->path, "x/y"));
    CHECK(same_error(fs_path_error_unwrap(pe), fs_err_exist));
    CHECK(str_is(fs_path_error_error(pe, a), "open x/y: file already exists"));
    CHECK(!fs_path_error_timeout(pe));

    FsPathError mine = {BURROW_S("stat"), BURROW_S("z"), fs_err_closed};
    Error e2 = fs_path_error_as_error(&mine, a);
    CHECK_IS(error_text(e2), "stat z: file already closed");

    /* error_retain keeps it a PathError. */
    Arena other;
    arena_init(&other, NULL, 0);
    Error kept = error_retain(arena_allocator(&other), err);
    arena_free(&ar);
    CHECK(errors_as(kept, TYPE_FS_PATH_ERROR) != NULL);
    CHECK(errors_is(kept, fs_err_exist));
    CHECK(str_is(error_text(kept), "open x/y: file already exists"));
    arena_free(&other);

    CHECK(str_is(error_text(fs_err_invalid), "invalid argument"));
    CHECK(str_is(error_text(fs_err_permission), "permission denied"));
    CHECK(str_is(error_text(fs_err_not_exist), "file does not exist"));
    CHECK(str_is(error_text(fs_skip_dir), "skip this directory"));
    CHECK(str_is(error_text(fs_skip_all), "skip everything and stop the walk"));
}

/* -------------------------------------------------------------- examples */

static void TestExampleGlob(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const FileSpec spec[] = {
        {"file.txt", NULL, 0},        {"file.go", NULL, 0},
        {"dir/file.txt", NULL, 0},    {"dir/file.go", NULL, 0},
        {"dir/subdir/x.go", NULL, 0},
    };
    Fs fsys = fstest_map_fs_as_fs(map_of(a, spec, 5));
    const char *patterns[] = {"*.txt", "*.go", "dir/*.go", "dir/*/x.go"};
    const char *want[] = {"file.txt", "file.go", "dir/file.go", "dir/subdir/x.go"};
    for (int i = 0; i < 4; i++) {
        Error err;
        Slice matches = fs_glob(a, fsys, cstr(patterns[i]), &err);
        if (BURROW_FAILED(err) || matches.len != 1 ||
            !str_is(*(Str *)slice_at(matches, 0), want[i]))
            testing_t_errorf_v(t, "%q matches: %v, %v; want [%s]", patterns[i], matches,
                               err, want[i]);
    }
    arena_free(&ar);
}

static void TestExampleReadFile(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const FileSpec spec[] = {{"hello.txt", "Hello, World!\n", 0}};
    Error err;
    Slice data = fs_read_file(a, fstest_map_fs_as_fs(map_of(a, spec, 1)),
                              BURROW_S("hello.txt"), &err);
    CHECK(BURROW_OK(err) && bytes_are(data, "Hello, World!\n"));
    arena_free(&ar);
}

/* ----------------------------------------------------------------- MapFS */

typedef struct ModesEnv {
    Alloc *a;
    Slice lines;
} ModesEnv;

static Error modes_walk(void *env, Str path, FsDirEntry d, Error err) {
    (void)err;
    ModesEnv *m = (ModesEnv *)env;
    FsFileInfo fi = d.vt->info(d.data, m->a, &err);
    if (BURROW_FAILED(err))
        return err;
    Str parts[3] = {path, BURROW_S(": "),
                    fs_file_mode_string(fi.vt->mode(fi.data), m->a)};
    Slice ps = slice_append(m->a, slice_nil(TYPE_STRING), parts, 3);
    Str line = strings_join(m->a, ps, BURROW_STR_EMPTY);
    m->lines = slice_append(m->a, m->lines, &line, 1);
    return BURROW_NO_ERROR;
}

static void TestMapFS(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const FileSpec spec[] = {
        {"hello", "hello, world\n", 0},
        {"fortune/k/ken.txt", "If a program is too slow, it must have a loop.\n", 0},
    };
    Fs m = fstest_map_fs_as_fs(map_of(a, spec, 2));
    Error err = fstest_test_fs_v(m, BURROW_S("hello"), BURROW_S("fortune"),
                                 BURROW_S("fortune/k"), BURROW_S("fortune/k/ken.txt"));
    arena_free(&ar);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
}

static void TestMapFSChmodDot(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const FileSpec spec[] = {
        {"a/b.txt", NULL, 0666},
        {".", NULL, 0777 | FS_MODE_DIR},
    };
    Fs m = fstest_map_fs_as_fs(map_of(a, spec, 2));
    ModesEnv env = {a, slice_nil(TYPE_STRING)};
    (void)fs_walk_dir(a, m, BURROW_S("."), BURROW_FN(FsWalkDirFunc, modes_walk, &env));
    Str got = strings_join(a, env.lines, BURROW_S("\n"));
    const char *want = ".: drwxrwxrwx\na: dr-xr-xr-x\na/b.txt: -rw-rw-rw-";
    if (!str_is(got, want))
        testing_t_errorf_v(t, "MapFS modes want:\n%s\ngot:\n%s\n", want, got);
    arena_free(&ar);
}

static void TestMapFSFileInfoName(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const FileSpec spec[] = {{"path/to/b.txt", NULL, 0}};
    FstestMapFS m = map_of(a, spec, 1);
    Error err;
    FsFileInfo info = fstest_map_fs_stat(m, a, BURROW_S("path/to/b.txt"), &err);
    CHECK(BURROW_OK(err));
    if (BURROW_OK(err) && !str_is(info.vt->name(info.data), "b.txt"))
        testing_t_errorf_v(t, "MapFS FileInfo.Name want:\nb.txt\ngot:\n%s\n",
                           info.vt->name(info.data));
    arena_free(&ar);
}

static void check_info(TestingT *t, Alloc *a, const char *what, FsFileInfo info,
                       Error err, const char *want_name, FsFileMode want_mode) {
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s = _, %v; want _, <nil>", what, err);
        return;
    }
    if (!str_is(info.vt->name(info.data), want_name))
        testing_t_errorf_v(t, "%s.Name() = %q; want %q", what, info.vt->name(info.data),
                           want_name);
    if (info.vt->mode(info.data) != want_mode)
        testing_t_errorf_v(t, "%s.Mode() = %s; want %s", what,
                           mode_str(a, info.vt->mode(info.data)),
                           mode_str(a, want_mode));
}

static void TestMapFSSymlink(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const char *file_content = "If a program is too slow, it must have a loop.\n";
    const FileSpec spec[] = {
        {"fortune/k/ken.txt", file_content, 0},
        {"dirlink", "fortune/k", FS_MODE_SYMLINK},
        {"linklink", "dirlink", FS_MODE_SYMLINK},
        {"ken.txt", "dirlink/ken.txt", FS_MODE_SYMLINK},
    };
    Fs m = fstest_map_fs_as_fs(map_of(a, spec, 4));
    Error err = fstest_test_fs_v(m, BURROW_S("fortune/k/ken.txt"), BURROW_S("dirlink"),
                                 BURROW_S("ken.txt"), BURROW_S("linklink"));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%v", err);

    Slice got_data = fs_read_file(a, m, BURROW_S("ken.txt"), &err);
    if (!bytes_are(got_data, file_content) || BURROW_FAILED(err))
        testing_t_errorf_v(t, "fs.ReadFile(m, \"ken.txt\") = %q, %v; want %q, <nil>",
                           got_data, err, file_content);
    Str got_link = fs_read_link(a, m, BURROW_S("dirlink"), &err);
    if (!str_is(got_link, "fortune/k") || BURROW_FAILED(err))
        testing_t_errorf_v(t, "fs.ReadLink(m, \"dirlink\") = %q, %v; want %q, <nil>",
                           got_link, err, "fortune/k");

    FsFileInfo info = fs_lstat(a, m, BURROW_S("dirlink"), &err);
    check_info(t, a, "fs.Lstat(m, \"dirlink\")", info, err, "dirlink", FS_MODE_SYMLINK);
    info = fs_stat(a, m, BURROW_S("dirlink"), &err);
    check_info(t, a, "fs.Stat(m, \"dirlink\")", info, err, "dirlink",
               FS_MODE_DIR | 0555);
    info = fs_lstat(a, m, BURROW_S("linklink"), &err);
    check_info(t, a, "fs.Lstat(m, \"linklink\")", info, err, "linklink",
               FS_MODE_SYMLINK);
    info = fs_stat(a, m, BURROW_S("linklink"), &err);
    check_info(t, a, "fs.Stat(m, \"linklink\")", info, err, "linklink",
               FS_MODE_DIR | 0555);
    arena_free(&ar);
}

/* Not in Go's tests: a directory read a few entries at a time, what a read of
 * a directory gives, a link loop, and a link to an absolute path. */
static void TestMapFSDirectory(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const FileSpec spec[] = {
        {"d/1", "one", 0},
        {"d/2", "two", 0},
        {"d/3/x", "x", 0},
        {"loop", "loop2", FS_MODE_SYMLINK},
        {"loop2", "loop", FS_MODE_SYMLINK},
        {"abs", "/etc/passwd", FS_MODE_SYMLINK},
    };
    FstestMapFS m = map_of(a, spec, 6);
    Error err;
    FsFile f = fstest_map_fs_open(m, a, BURROW_S("d"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        arena_free(&ar);
        return;
    }
    CHECK(f.vt->read_dir != NULL);
    Slice first = f.vt->read_dir(f.data, a, 2, &err);
    CHECK(BURROW_OK(err) && first.len == 2);
    CHECK_IS(entry_names(a, first), "1 2");
    Slice rest = f.vt->read_dir(f.data, a, 2, &err);
    CHECK(BURROW_OK(err) && rest.len == 1);
    FsDirEntry *three = (FsDirEntry *)slice_at(rest, 0);
    CHECK(three->vt->is_dir(three->data));
    CHECK_IS(fs_format_dir_entry(a, *three), "d 3/");
    Slice end = f.vt->read_dir(f.data, a, 2, &err);
    CHECK(end.len == 0 && same_error(err, io_eof));
    end = f.vt->read_dir(f.data, a, -1, &err);
    CHECK(end.len == 0 && BURROW_OK(err));

    Byte buf[4];
    Int n =
        f.vt->read_closer.reader.read(f.data, slice_from(buf, 4, 4, TYPE_BYTE), &err);
    CHECK(n == 0);
    CHECK_IS(error_text(err), "read d: invalid argument");
    FsFileInfo fi = f.vt->stat(f.data, a, &err);
    CHECK(BURROW_OK(err) &&
          str_is(fs_format_file_info(a, fi), "dr-xr-xr-x 0 0001-01-01 00:00:00 d/"));
    CHECK(BURROW_OK(f.vt->read_closer.closer.close(f.data)));

    (void)fstest_map_fs_open(m, a, BURROW_S("loop"), &err);
    CHECK_IS(error_text(err), "open loop: file does not exist");
    (void)fstest_map_fs_open(m, a, BURROW_S("abs"), &err);
    CHECK(errors_is(err, fs_err_not_exist));
    (void)fstest_map_fs_open(m, a, BURROW_S("d/../d"), &err);
    CHECK_IS(error_text(err), "open d/../d: file does not exist");
    (void)fstest_map_fs_lstat(m, a, BURROW_S("nope/x"), &err);
    CHECK_IS(error_text(err), "lstat nope/x: file does not exist");

    /* A file opened and read through the io.Reader it is. */
    f = fstest_map_fs_open(m, a, BURROW_S("d/2"), &err);
    CHECK(BURROW_OK(err) && f.vt->read_dir == NULL);
    Slice all = io_read_all(a, fs_file_as_io_reader(f), &err);
    CHECK(BURROW_OK(err) && bytes_are(all, "two"));
    fi = f.vt->stat(f.data, a, &err);
    CHECK(BURROW_OK(err) && fi.vt->size(fi.data) == 3 && !fi.vt->is_dir(fi.data));
    Any sys = fi.vt->sys(fi.data);
    CHECK(sys.t == TYPE_INT && sys.data == &sys_value);

    /* An empty MapFS still has ".". */
    FstestMapFS empty = fstest_map_fs_make(a);
    Slice dirs = fs_read_dir(a, fstest_map_fs_as_fs(empty), BURROW_S("."), &err);
    CHECK(BURROW_OK(err) && dirs.len == 0);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestValidPath)                                                                   \
    X(TestValidPathUTF8)                                                               \
    X(TestGlob)                                                                        \
    X(TestGlobError)                                                                   \
    X(TestCVE202230630)                                                                \
    X(TestGlobMethod)                                                                  \
    X(TestReadDir)                                                                     \
    X(TestFileInfoToDirEntry)                                                          \
    X(TestReadDirPath)                                                                 \
    X(TestReadFile)                                                                    \
    X(TestReadFilePath)                                                                \
    X(TestReadFileGrows)                                                               \
    X(TestReadLink)                                                                    \
    X(TestLstat)                                                                       \
    X(TestStat)                                                                        \
    X(TestSub)                                                                         \
    X(TestSubMethods)                                                                  \
    X(TestWalkDir)                                                                     \
    X(TestWalkDirSymlink)                                                              \
    X(TestIssue51617)                                                                  \
    X(TestWalkDirSkip)                                                                 \
    X(TestFormatFileInfo)                                                              \
    X(TestFormatDirEntry)                                                              \
    X(TestFileModeString)                                                              \
    X(TestPathError)                                                                   \
    X(TestExampleGlob)                                                                 \
    X(TestExampleReadFile)                                                             \
    X(TestMapFS)                                                                       \
    X(TestMapFSChmodDot)                                                               \
    X(TestMapFSFileInfoName)                                                           \
    X(TestMapFSSymlink)                                                                \
    X(TestMapFSDirectory)

TESTING_MAIN(TESTS)
