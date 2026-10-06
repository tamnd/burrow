/* Derived from Go's src/os/root_test.go.
 * Go source: go1.27.1.
 *
 * Root and OpenInRoot. The table of cases and the tests that run every
 * operation over it are Go's, case for case. Go's consistency tests, which
 * compare Root against the plain os functions on odd names, are not here yet.
 *
 * Copyright 2024 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/os.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/io/fs.h"
#include "burrow/mem/arena.h"
#include "burrow/path.h"
#include "burrow/path/filepath.h"
#include "burrow/sort.h"
#include "burrow/strings.h"
#include "burrow/time.h"

#include "check.h"

#include <string.h>

/* ------------------------------------------------------------- helpers */

#define S(lit) BURROW_S(lit)

static Arena ar;
static Alloc *a;

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Str str_of(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

static void remove_tree(void *env) {
    (void)os_remove_all(*(Str *)env);
}

/* t.TempDir: a new directory, removed when the test ends. */
static Str temp_dir(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Str *d = BURROW_NEW(a, Str);
    *d = os_mkdir_temp(a, S(""), S("burrow-os-root-test-*"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "MkdirTemp: %s", error_text(e));
    testing_t_cleanup(t, BURROW_FN(Func, remove_tree, d));
    return *d;
}

static Str join(Str dir, Str name) {
    return filepath_join_v(a, 2, dir, name);
}

static void write_file(TestingT *t, Str name, Str data, OsFileMode perm) {
    Error e = os_write_file(name, bytes_of(data), perm);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "WriteFile %s: %s", name, error_text(e));
}

static void mkdir_d(TestingT *t, Str name) {
    Error e = os_mkdir(name, 0777);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Mkdir %s: %s", name, error_text(e));
}

static bool is_symlink(OsFileInfo fi) {
    return (fi.vt->mode(fi.data) & OS_MODE_SYMLINK) != 0;
}

/* testenv.MustHaveSymlink. Windows needs a privilege to make links, and
 * wine says it made one when nothing on disk follows it, so the first call
 * makes a link and checks it reads as one and leads to its target. */
static void must_have_symlink(TestingT *t) {
#if defined(BURROW_OS_WINDOWS)
    static int have; /* 0 not known yet, 1 yes, -1 no */
    if (have == 0) {
        Str d = temp_dir(t);
        write_file(t, join(d, S("target")), S("x"), 0666);
        Error e = os_symlink(S("target"), join(d, S("link")));
        OsFileInfo li = {NULL, NULL};
        if (BURROW_OK(e))
            li = os_lstat(a, join(d, S("link")), &e);
        if (BURROW_OK(e) && is_symlink(li))
            (void)os_stat(a, join(d, S("link")), &e);
        have = BURROW_OK(e) && li.vt != NULL && is_symlink(li) ? 1 : -1;
    }
    if (have < 0)
        testing_t_skip_v(t, "symbolic links do not work here");
#elif defined(BURROW_OS_WASI)
    /* Some wasip1 runtimes refuse a link to an absolute path, or one that
     * leads out of the directory they were given, so as in Go, a link that
     * cannot be made is a skip. */
    static int have; /* 0 not known yet, 1 yes, -1 no */
    if (have == 0) {
        Str d = temp_dir(t);
        Str target = join(d, S("testfile.txt"));
        write_file(t, target, S("x"), 0666);
        have = BURROW_OK(os_symlink(target, join(d, S("testlink")))) ? 1 : -1;
    }
    if (have < 0)
        testing_t_skip_v(t, "symbolic links do not work here");
#else
    (void)t;
#endif
}

static bool has_link(const char *const *fs) {
    for (Int i = 0; fs[i] != NULL; i++)
        if (strstr(fs[i], " => ") != NULL)
            return true;
    return false;
}

/* makefs builds a tree from a list of entries and returns the path to it:
 *
 *   - "d/" is the directory d
 *   - "f" is the file f, holding the text "f"
 *   - "a => b" is the symbolic link a, pointing at b
 *
 * The tree's directory is always called ROOT, and $ABS in an entry becomes
 * its absolute path. Parent directories are made as needed. */
static Str makefs(TestingT *t, const char *const *fs) {
    Str root = join(temp_dir(t), S("ROOT"));
    mkdir_d(t, root);
    for (Int i = 0; fs[i] != NULL; i++) {
        Str ent = strings_replace_all(a, str_from_cstr(fs[i]), S("$ABS"), root);
        Str link;
        bool is_link;
        Str base = strings_cut(ent, S(" => "), &link, &is_link);
        if (is_link)
            ent = base;
        Error e = os_mkdir_all(join(root, path_dir(a, base)), 0777);
        if (BURROW_FAILED(e))
            testing_t_fatalf_v(t, "MkdirAll: %s", error_text(e));
        if (is_link) {
            e = os_symlink(link, join(root, base));
            if (BURROW_FAILED(e))
                testing_t_fatalf_v(t, "Symlink: %s", error_text(e));
        } else if (strings_has_suffix(ent, S("/"))) {
            e = os_mkdir_all(join(root, ent), 0777);
            if (BURROW_FAILED(e))
                testing_t_fatalf_v(t, "MkdirAll: %s", error_text(e));
        } else {
            write_file(t, join(root, ent), ent, 0666);
        }
    }
    return root;
}

/* A test case for Root.
 *
 * open is the name to use. target is the file that should be reached once
 * every link is followed or, when the name escapes the root, the file that
 * must not be reached. ltarget is the file reached when every link but the
 * last one is followed, which is what Remove deletes and Lstat looks at, and
 * is NULL when the last part of open is not a link. always_fails marks the
 * cases that would fail even without the root's checks. */
typedef struct RootTest {
    const char *name;
    const char *fs[8];
    const char *open;
    const char *target;
    const char *ltarget;
    bool want_error;
    bool always_fails;
} RootTest;

static const RootTest root_test_cases[] = {
    {.name = "plain path", .open = "target", .target = "target"},
    {.name = "path in directory",
     .fs = {"a/b/c/"},
     .open = "a/b/c/target",
     .target = "a/b/c/target"},
    {.name = "symlink",
     .fs = {"link => target"},
     .open = "link",
     .target = "target",
     .ltarget = "link"},
    {.name = "symlink dotdot slash",
     .fs = {"link => ../"},
     .open = "link",
     .ltarget = "link",
     .want_error = true},
    {.name = "symlink ending in slash",
     .fs = {"dir/", "link => dir/"},
     .open = "link/target",
     .target = "dir/target"},
    {.name = "slash after symlink to file",
     .fs = {"link => ../ROOT/target"},
     .open = "link/",
     .target = "target",
     .want_error = true},
    {.name = "slash after symlink to dir",
     .fs = {"link => ../ROOT/target", "target/"},
     .open = "link/",
     .want_error = true},
    {.name = "symlink dotdot dotdot slash",
     .fs = {"dir/link => ../../"},
     .open = "dir/link",
     .ltarget = "dir/link",
     .want_error = true},
    {.name = "symlink chain",
     .fs = {"link => a/b/c/target", "a/b => e", "a/e => ../f", "f => g/h/i",
            "g/h/i => ..", "g/c/"},
     .open = "link",
     .target = "g/c/target",
     .ltarget = "link"},
    {.name = "path with dot",
     .fs = {"a/b/"},
     .open = "./a/./b/./target",
     .target = "a/b/target"},
    {.name = "path with dotdot",
     .fs = {"a/b/"},
     .open = "a/../a/b/../../a/b/../b/target",
     .target = "a/b/target"},
    {.name = "path with dotdot slash", .open = "../", .want_error = true},
    {.name = "path with dotdot dotdot slash",
     .fs = {"a/"},
     .open = "a/../../",
     .want_error = true},
    {.name = "dotdot no symlink",
     .fs = {"a/"},
     .open = "a/../target",
     .target = "target"},
    {.name = "dotdot after symlink",
     .fs = {"a => b/c", "b/c/"},
     .open = "a/../target",
#if defined(BURROW_OS_WINDOWS)
     /* On Windows the path is cleaned before links are followed. */
     .target = "target"},
#else
     .target = "b/target"},
#endif
    {.name = "dotdot before symlink",
     .fs = {"a => b/c", "b/c/"},
     .open = "b/../a/target",
     .target = "b/c/target"},
    {.name = "symlink ends in dot",
     .fs = {"a => b/.", "b/"},
     .open = "a/target",
     .target = "b/target"},
    {.name = "directory does not exist",
     .open = "a/file",
     .want_error = true,
     .always_fails = true},
    {.name = "empty path", .open = "", .want_error = true, .always_fails = true},
    {.name = "symlink cycle",
     .fs = {"a => a"},
     .open = "a",
     .ltarget = "a",
     .want_error = true,
     .always_fails = true},
    {.name = "path escapes",
     .open = "../ROOT/target",
     .target = "target",
     .want_error = true},
    {.name = "long path escapes",
     .fs = {"a/"},
     .open = "a/../../ROOT/target",
     .target = "target",
     .want_error = true},
    {.name = "absolute symlink",
     .fs = {"link => $ABS/target"},
     .open = "link",
     .target = "target",
     .ltarget = "link",
     .want_error = true},
    {.name = "relative symlink",
     .fs = {"link => ../ROOT/target"},
     .open = "link",
     .target = "target",
     .ltarget = "link",
     .want_error = true},
    {.name = "symlink chain escapes",
     .fs = {"link => a/b/c/target", "a/b => e", "a/e => ../../ROOT", "c/"},
     .open = "link",
     .target = "c/target",
     .ltarget = "link",
     .want_error = true},
};

#define ROOT_TEST_CASES ((Int)(sizeof root_test_cases / sizeof root_test_cases[0]))

/* What one operation does with one case. target is the case's target joined
 * onto the root's directory, or empty when the case has none. */
typedef void (*RootTestFn)(TestingT *t, const RootTest *test, Str target, OsRoot *r);

typedef struct RootRun {
    const RootTest *test;
    RootTestFn fn;
} RootRun;

static void root_free_fn(void *env) {
    os_root_free((OsRoot *)env);
}

static void root_run_one(void *env, TestingT *t) {
    const RootRun *run = (const RootRun *)env;
    if (has_link(run->test->fs))
        must_have_symlink(t);
    Str root = makefs(t, run->test->fs);
    Error e = BURROW_NO_ERROR;
    OsRoot *r = os_open_root(a, root, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "OpenRoot: %s", error_text(e));
    testing_t_cleanup(t, BURROW_FN(Func, root_free_fn, r));
    Str target = S("");
    if (run->test->target != NULL)
        target = join(root, str_from_cstr(run->test->target));
    run->fn(t, run->test, target, r);
}

/* rootTest.run over every case, leaving out the one called skip. */
static void run_cases(TestingT *t, RootTestFn fn, const char *skip) {
    for (Int i = 0; i < ROOT_TEST_CASES; i++) {
        const RootTest *test = &root_test_cases[i];
        if (skip != NULL && strcmp(test->name, skip) == 0)
            continue;
        RootRun run = {test, fn};
        testing_t_run(t, str_from_cstr(test->name),
                      BURROW_FN(TestingTFunc, root_run_one, (void *)&run));
    }
}

/* errEndsTest: fails the test when err does not match want_error, and
 * reports whether the case is over because the expected error came. */
static bool err_ends_test(TestingT *t, Error err, bool want_error, const char *op,
                          const RootTest *test) {
    if (want_error) {
        if (BURROW_OK(err))
            testing_t_fatalf_v(t, "%s(%q) = nil; want error", str_from_cstr(op),
                               str_from_cstr(test->open));
        return true;
    }
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s(%q) = %s; want success", str_from_cstr(op),
                           str_from_cstr(test->open), error_text(err));
    return false;
}

static Str open_of(const RootTest *test) {
    return str_from_cstr(test->open);
}

static Str ltarget_path(OsRoot *r, const RootTest *test) {
    return join(os_root_name(r), str_from_cstr(test->ltarget));
}

static void want_not_exist(TestingT *t, Str path, const char *op,
                           const RootTest *test) {
    Error e = BURROW_NO_ERROR;
    (void)os_lstat(a, path, &e);
    if (!errors_is(e, os_err_not_exist))
        testing_t_fatalf_v(t, "stat file removed with %s(%q): %s, want ErrNotExist",
                           str_from_cstr(op), str_from_cstr(test->open), error_text(e));
}

/* ----------------------------------------------------- one op, all cases */

static void root_open_file(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    if (target.len != 0)
        write_file(t, target, S("target"), 0666);
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_root_open(r, a, open_of(test), &e);
    if (err_ends_test(t, e, test->want_error, "root.Open", test))
        return;
    Slice got = io_read_all(a, os_file_as_io_reader(f), &e);
    if (BURROW_FAILED(e) || !str_eq(str_of(got), S("target")))
        testing_t_errorf_v(t, "Root.Open(%q): read content %q, %s; want \"target\"",
                           open_of(test), str_of(got), error_text(e));
    os_file_free(f);
}

static void TestRootOpen_File(TestingT *t) {
    run_cases(t, root_open_file, NULL);
}

static void root_open_directory(TestingT *t, const RootTest *test, Str target,
                                OsRoot *r) {
    if (target.len != 0) {
        mkdir_d(t, target);
        write_file(t, join(target, S("found")), S(""), 0666);
    }
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_root_open(r, a, open_of(test), &e);
    if (err_ends_test(t, e, test->want_error, "root.Open", test))
        return;
    Slice got = os_file_readdirnames(f, a, -1, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "Root.Open(%q).Readdirnames: %s", open_of(test),
                           error_text(e));
    if (got.len != 1 || !str_eq(((const Str *)got.p)[0], S("found")))
        testing_t_errorf_v(t, "Root.Open(%q).Readdirnames: %d names, want [found]",
                           open_of(test), got.len);
    os_file_free(f);
}

static void TestRootOpen_Directory(TestingT *t) {
    run_cases(t, root_open_directory, NULL);
}

static void root_create(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_root_create(r, a, open_of(test), &e);
    if (err_ends_test(t, e, test->want_error, "root.Create", test))
        return;
    (void)os_file_write(f, bytes_of(S("target")), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Write: %s", error_text(e));
    os_file_free(f);
    Slice got = os_read_file(a, target, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "reading file created with root.Create(%q): %s",
                           open_of(test), error_text(e));
    if (!str_eq(str_of(got), S("target")))
        testing_t_fatalf_v(t, "reading file created with root.Create(%q): got %q",
                           open_of(test), str_of(got));
}

static void TestRootCreate(TestingT *t) {
    run_cases(t, root_create, NULL);
}

static void root_chmod(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    /* A file nobody may read or write, so Chmod has to work without
     * opening it. */
    if (target.len != 0)
        write_file(t, target, S(""), 0000);
    Error e = BURROW_NO_ERROR;
#if defined(BURROW_OS_WINDOWS)
    /* On Windows Chmod on a link changes the link, not what it points at.
     * See https://go.dev/issue/71492. */
    OsFileInfo li = os_root_lstat(r, a, open_of(test), &e);
    if (BURROW_OK(e) && !os_file_mode_is_regular(li.vt->mode(li.data)))
        testing_t_skip_v(t, "https://go.dev/issue/71492");
#endif
    OsFileMode want = 0666;
    e = os_root_chmod(r, open_of(test), want);
    if (err_ends_test(t, e, test->want_error, "root.Chmod", test))
        return;
    OsFileInfo fi = os_stat(a, target, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "os.Stat(%q) = %s", target, error_text(e));
    if (fi.vt->mode(fi.data) != want)
        testing_t_errorf_v(t, "after root.Chmod(%q, 0666): file mode = %#o, want 0666",
                           open_of(test), (Int)fi.vt->mode(fi.data));
}

static void TestRootChmod(TestingT *t) {
#if defined(BURROW_OS_WASI)
    testing_t_skip_v(t, "Chmod not supported on wasip1");
#endif
    run_cases(t, root_chmod, NULL);
}

static void root_chtimes(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    if (target.len != 0)
        write_file(t, target, S(""), 0666);
    /* Windows keeps times in 100ns steps, so the times here are whole
     * microseconds. Go also checks the access times where the file system
     * keeps them. This only checks modification times. */
    Time now = time_truncate(time_now(), TIME_MICROSECOND);
    Time zero = {0};
    struct {
        Time atime, mtime;
    } times[] = {
        {time_add(now, -TIME_MINUTE), time_add(now, -TIME_MINUTE)},
        {time_add(now, TIME_MINUTE), time_add(now, TIME_MINUTE)},
        {zero, now},
        {now, zero},
    };
    for (size_t i = 0; i < sizeof times / sizeof times[0]; i++) {
        Error e = os_root_chtimes(r, open_of(test), times[i].atime, times[i].mtime);
        if (err_ends_test(t, e, test->want_error, "root.Chtimes", test))
            return;
        OsFileInfo fi = os_stat(a, target, &e);
        if (BURROW_FAILED(e))
            testing_t_fatalf_v(t, "os.Stat(%q) = %s", target, error_text(e));
        Time got = fi.vt->mod_time(fi.data);
        if (!time_is_zero(times[i].mtime) && !time_equal(got, times[i].mtime))
            testing_t_errorf_v(
                t, "after root.Chtimes(%q) #%d: got mtime %d.%09d, want %d.%09d",
                open_of(test), (Int)i, time_unix(got), (Int)time_nanosecond(got),
                time_unix(times[i].mtime), (Int)time_nanosecond(times[i].mtime));
    }
}

static void TestRootChtimes(TestingT *t) {
    run_cases(t, root_chtimes, NULL);
}

static void check_made_dir(TestingT *t, Str target, const char *op,
                           const RootTest *test) {
    Error e = BURROW_NO_ERROR;
    OsFileInfo fi = os_lstat(a, target, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "stat file created with %s(%q): %s", str_from_cstr(op),
                           open_of(test), error_text(e));
    if (!fi.vt->is_dir(fi.data))
        testing_t_fatalf_v(t, "stat file created with %s(%q): not a directory",
                           str_from_cstr(op), open_of(test));
    /* Go issue 73559: the umask changes the bits, but some must be set. */
    if ((fi.vt->mode(fi.data) & 0777) == 0)
        testing_t_fatalf_v(t, "stat file created with %s(%q): mode=%#o, want non-zero",
                           str_from_cstr(op), open_of(test), (Int)fi.vt->mode(fi.data));
}

static void root_mkdir(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    /* Mkdir on a link is an error, though not an escape. */
    bool want_error = test->want_error || test->ltarget != NULL;
    Error e = os_root_mkdir(r, open_of(test), 0777);
    if (err_ends_test(t, e, want_error, "root.Mkdir", test))
        return;
    check_made_dir(t, target, "Root.Mkdir", test);
}

static void TestRootMkdir(TestingT *t) {
    run_cases(t, root_mkdir, NULL);
}

static void root_mkdir_all(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    bool want_error = test->want_error || test->ltarget != NULL;
    Error e = os_root_mkdir_all(r, open_of(test), 0777);
    if (err_ends_test(t, e, want_error, "root.MkdirAll", test))
        return;
    check_made_dir(t, target, "Root.MkdirAll", test);
}

static void TestRootMkdirAll(TestingT *t) {
    /* That case wants an error, and MkdirAll makes the missing directory. */
    run_cases(t, root_mkdir_all, "directory does not exist");
}

static void root_open_root(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    if (target.len != 0) {
        mkdir_d(t, target);
        write_file(t, join(target, S("f")), S(""), 0666);
    }
    Error e = BURROW_NO_ERROR;
    OsRoot *rr = os_root_open_root(r, a, open_of(test), &e);
    if (err_ends_test(t, e, test->want_error, "root.OpenRoot", test))
        return;
    OsFile *f = os_root_open(rr, a, S("f"), &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "root.OpenRoot(%q).Open(\"f\") = %s", open_of(test),
                           error_text(e));
    else
        os_file_free(f);
    os_root_free(rr);
}

static void TestRootOpenRoot(TestingT *t) {
    run_cases(t, root_open_root, NULL);
}

static void root_remove_file(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    bool want_error = test->want_error;
    if (test->ltarget != NULL) {
        /* Remove does not follow a link in the last part, so it removes
         * the link. */
        want_error = false;
        target = ltarget_path(r, test);
    } else if (target.len != 0) {
        write_file(t, target, S(""), 0666);
    }
    Error e = os_root_remove(r, open_of(test));
    if (err_ends_test(t, e, want_error, "root.Remove", test))
        return;
    want_not_exist(t, target, "Root.Remove", test);
}

static void TestRootRemoveFile(TestingT *t) {
    run_cases(t, root_remove_file, NULL);
}

static void root_remove_directory(TestingT *t, const RootTest *test, Str target,
                                  OsRoot *r) {
    bool want_error = test->want_error;
    if (test->ltarget != NULL) {
        want_error = false;
        target = ltarget_path(r, test);
    } else if (target.len != 0) {
        mkdir_d(t, target);
    }
    Error e = os_root_remove(r, open_of(test));
    if (err_ends_test(t, e, want_error, "root.Remove", test))
        return;
    want_not_exist(t, target, "Root.Remove", test);
}

static void TestRootRemoveDirectory(TestingT *t) {
    run_cases(t, root_remove_directory, NULL);
}

static void root_remove_all(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    const char *ltarget = test->ltarget;
    Error e = BURROW_NO_ERROR;
    if (strings_has_suffix(open_of(test), S("/"))) {
        /* RemoveAll ignores trailing slashes, so a link named with one is
         * removed as a link. */
        OsFileInfo li = os_lstat(a, join(os_root_name(r), open_of(test)), &e);
        if (BURROW_OK(e) && os_file_mode_type(li.vt->mode(li.data)) == OS_MODE_SYMLINK)
            ltarget = test->open;
    }
    bool want_error = test->want_error;
    if (ltarget != NULL) {
        want_error = false;
        target = join(os_root_name(r), str_from_cstr(ltarget));
    } else if (target.len != 0) {
        mkdir_d(t, target);
        write_file(t, join(target, S("file")), S(""), 0666);
    }
    /* RemoveAll of something that is not there succeeds. */
    bool target_exists = true;
    e = BURROW_NO_ERROR;
    (void)os_root_lstat(r, a, open_of(test), &e);
    if (errors_is(e, os_err_not_exist)) {
        target_exists = false;
        want_error = false;
    }
    e = os_root_remove_all(r, open_of(test));
    if (err_ends_test(t, e, want_error, "root.RemoveAll", test))
        return;
    if (target_exists)
        want_not_exist(t, target, "Root.RemoveAll", test);
}

static void TestRootRemoveAll(TestingT *t) {
    run_cases(t, root_remove_all, NULL);
}

static void root_stat(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    if (target.len != 0)
        write_file(t, target, S("content"), 0666);
    Error e = BURROW_NO_ERROR;
    OsFileInfo fi = os_root_stat(r, a, open_of(test), &e);
    if (err_ends_test(t, e, test->want_error, "root.Stat", test))
        return;
    Str want = filepath_base(open_of(test));
    if (!str_eq(fi.vt->name(fi.data), want))
        testing_t_errorf_v(t, "root.Stat(%q).Name() = %q, want %q", open_of(test),
                           fi.vt->name(fi.data), want);
    if (fi.vt->size(fi.data) != 7)
        testing_t_errorf_v(t, "root.Stat(%q).Size() = %d, want 7", open_of(test),
                           fi.vt->size(fi.data));
}

static void TestRootStat(TestingT *t) {
    run_cases(t, root_stat, NULL);
}

static void root_lstat(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    bool want_error = test->want_error;
    if (test->ltarget != NULL)
        want_error = false; /* Lstat looks at the last link itself. */
    else if (target.len != 0)
        write_file(t, target, S("content"), 0666);
    Error e = BURROW_NO_ERROR;
    OsFileInfo fi = os_root_lstat(r, a, open_of(test), &e);
    if (err_ends_test(t, e, want_error, "root.Lstat", test))
        return;
    Str want = filepath_base(open_of(test));
    if (!str_eq(fi.vt->name(fi.data), want))
        testing_t_errorf_v(t, "root.Lstat(%q).Name() = %q, want %q", open_of(test),
                           fi.vt->name(fi.data), want);
    if (test->ltarget == NULL) {
        if (is_symlink(fi))
            testing_t_errorf_v(t, "root.Lstat(%q).Mode() is a link, want non-link",
                               open_of(test));
        if (fi.vt->size(fi.data) != 7)
            testing_t_errorf_v(t, "root.Lstat(%q).Size() = %d, want 7", open_of(test),
                               fi.vt->size(fi.data));
    } else if (!is_symlink(fi)) {
        testing_t_errorf_v(t, "root.Lstat(%q).Mode() is not a link, want a link",
                           open_of(test));
    }
}

static void TestRootLstat(TestingT *t) {
    run_cases(t, root_lstat, NULL);
}

static void root_readlink(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    (void)target;
    /* Readlink reads the last link rather than following it, and fails on
     * anything that is not a link. */
    bool want_error = test->ltarget == NULL;
    Error e = BURROW_NO_ERROR;
    Str got = os_root_readlink(r, a, open_of(test), &e);
    if (err_ends_test(t, e, want_error, "root.Readlink", test))
        return;
    Str want = os_readlink(a, ltarget_path(r, test), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "os.Readlink(%q) = %s, want success",
                           str_from_cstr(test->ltarget), error_text(e));
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "root.Readlink(%q) = %q, want %q", open_of(test), got,
                           want);
}

static void TestRootReadlink(TestingT *t) {
    run_cases(t, root_readlink, NULL);
}

/* testRootMoveFrom: rename or link the case's name to a name known to be
 * fine. */
static void root_move_from(TestingT *t, const RootTest *test, Str target, OsRoot *r,
                           bool rename) {
    if (target.len != 0)
        write_file(t, target, S("target"), 0666);
    bool want_error = test->want_error;
    Str link_target = S("");
    Error e = BURROW_NO_ERROR;
    if (test->ltarget != NULL) {
        /* Rename moves the link, not the file it points at. */
        want_error = false;
        link_target = os_root_readlink(r, a, str_from_cstr(test->ltarget), &e);
        if (BURROW_FAILED(e))
            testing_t_fatalf_v(t, "root.Readlink(%q) = %s, want success",
                               str_from_cstr(test->ltarget), error_text(e));
#if defined(BURROW_OS_WINDOWS)
        /* Windows makes hard links to file links but not to directory
         * links. The link was made with os_symlink, which makes a directory
         * link when the target is a directory, so a Stat is enough here. */
        if (!rename) {
            OsFileInfo st = os_stat(a, ltarget_path(r, test), &e);
            if (BURROW_OK(e) && st.vt->is_dir(st.data))
                want_error = true;
        }
#endif
    }
    Str dst = S("destination");
    const char *op = rename ? "root.Rename" : "root.Link";
    e = rename ? os_root_rename(r, open_of(test), dst)
               : os_root_link(r, open_of(test), dst);
    if (err_ends_test(t, e, want_error, op, test))
        return;

    Str orig = test->ltarget != NULL ? ltarget_path(r, test) : target;
    e = BURROW_NO_ERROR;
    (void)os_lstat(a, orig, &e);
    if (rename && !errors_is(e, os_err_not_exist))
        testing_t_errorf_v(t, "after renaming file, Lstat(%q) = %s, want ErrNotExist",
                           orig, error_text(e));
    if (!rename && BURROW_FAILED(e))
        testing_t_errorf_v(t, "after linking file, error accessing original: %s",
                           error_text(e));

    Str full = join(os_root_name(r), dst);
    if (test->ltarget != NULL) {
        Str got = os_readlink(a, full, &e);
        if (BURROW_FAILED(e) || !str_eq(got, link_target))
            testing_t_errorf_v(t, "os.Readlink(%q) = %q, %s, want %q", full, got,
                               error_text(e), link_target);
    } else {
        Slice got = os_read_file(a, full, &e);
        if (BURROW_FAILED(e) || !str_eq(str_of(got), S("target")))
            testing_t_errorf_v(t,
                               "os.ReadFile(%q): read content %q, %s; want \"target\"",
                               full, str_of(got), error_text(e));
        OsFileInfo st = os_lstat(a, full, &e);
        if (BURROW_FAILED(e) || is_symlink(st))
            testing_t_errorf_v(t, "os.Lstat(%q) = %s; want non-link", full,
                               error_text(e));
    }
}

/* testRootMoveTo: rename or link a name known to be fine to the case's
 * name. */
static void root_move_to(TestingT *t, const RootTest *test, Str target, OsRoot *r,
                         bool rename) {
    (void)target;
    Str src = S("source");
    write_file(t, join(os_root_name(r), src), S("target"), 0666);
    const char *ltarget = test->ltarget;
    Error e = BURROW_NO_ERROR;
#if defined(BURROW_OS_WINDOWS)
    /* Windows ignores trailing slashes on the new name. */
    if (strings_has_suffix(open_of(test), S("/"))) {
        Str p = strings_trim_suffix(open_of(test), S("/"));
        OsFileInfo st = os_root_lstat(r, a, p, &e);
        if (BURROW_OK(e) && os_file_mode_type(st.vt->mode(st.data)) == OS_MODE_SYMLINK)
            ltarget = str_to_cstr(a, p);
    }
#endif
    const char *want_name = test->target;
    bool want_error = test->want_error;
    if (ltarget != NULL) {
        /* Rename replaces the last link rather than following it. */
        want_name = ltarget;
        want_error = false;
    }
    const char *op = rename ? "root.Rename" : "root.Link";
    e = rename ? os_root_rename(r, src, open_of(test))
               : os_root_link(r, src, open_of(test));
    if (err_ends_test(t, e, want_error, op, test))
        return;

    e = BURROW_NO_ERROR;
    (void)os_lstat(a, join(os_root_name(r), src), &e);
    if (rename && !errors_is(e, os_err_not_exist))
        testing_t_errorf_v(t, "after renaming file, Lstat(%q) = %s, want ErrNotExist",
                           src, error_text(e));
    if (!rename && BURROW_FAILED(e))
        testing_t_errorf_v(t, "after linking file, error accessing original: %s",
                           error_text(e));

    Str name = str_from_cstr(want_name != NULL ? want_name : "");
    Slice got = os_read_file(a, join(os_root_name(r), name), &e);
    if (BURROW_FAILED(e) || !str_eq(str_of(got), S("target")))
        testing_t_errorf_v(t, "os.ReadFile(%q): read content %q, %s; want \"target\"",
                           name, str_of(got), error_text(e));
}

static void root_rename_from(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    root_move_from(t, test, target, r, true);
}

static void root_link_from(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    root_move_from(t, test, target, r, false);
}

static void root_rename_to(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    root_move_to(t, test, target, r, true);
}

/* Go's TestRootLinkTo calls testRootMoveTo with rename set to true, so it
 * renames as well. This does the same. */
static void TestRootRenameFrom(TestingT *t) {
    run_cases(t, root_rename_from, NULL);
}

static void TestRootLinkFrom(TestingT *t) {
    run_cases(t, root_link_from, NULL);
}

static void TestRootRenameTo(TestingT *t) {
    run_cases(t, root_rename_to, NULL);
}

static void TestRootLinkTo(TestingT *t) {
    run_cases(t, root_rename_to, NULL);
}

static void root_symlink(TestingT *t, const RootTest *test, Str target, OsRoot *r) {
    /* A link cannot be made over a link that is already there. */
    bool want_error = test->want_error || test->ltarget != NULL;
    Error e = os_root_symlink(r, S("linktarget"), open_of(test));
    if (err_ends_test(t, e, want_error, "root.Symlink", test))
        return;
    Str got = os_readlink(a, target, &e);
    if (BURROW_FAILED(e) || !str_eq(got, S("linktarget")))
        testing_t_fatalf_v(t, "ReadLink(%q) = %q, %s; want \"linktarget\", nil", target,
                           got, error_text(e));
}

static void TestRootSymlink(TestingT *t) {
    must_have_symlink(t);
    run_cases(t, root_symlink, NULL);
}

/* --------------------------------------------------------- one at a time */

static void TestRootOpenFileAsRoot(TestingT *t) {
    Str dir = temp_dir(t);
    Str target = join(dir, S("target"));
    write_file(t, target, S(""), 0666);
    Error e = BURROW_NO_ERROR;
    OsRoot *r = os_open_root(a, target, &e);
    if (BURROW_OK(e)) {
        os_root_free(r);
        testing_t_fatalf_v(t, "os.OpenRoot(file) succeeded; want failure");
    }
    r = os_open_root(a, dir, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "OpenRoot: %s", error_text(e));
    OsRoot *rr = os_root_open_root(r, a, S("target"), &e);
    if (BURROW_OK(e)) {
        os_root_free(rr);
        os_root_free(r);
        testing_t_fatalf_v(t, "Root.OpenRoot(file) succeeded; want failure");
    }
    os_root_free(r);
}

static void TestRootNonPermissionMode(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    OsRoot *r = os_open_root(a, temp_dir(t), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "OpenRoot: %s", error_text(e));
    OsFile *f = os_root_open_file(r, a, S("file"), OS_O_RDWR | OS_O_CREATE, 01777, &e);
    if (BURROW_OK(e)) {
        os_file_free(f);
        testing_t_errorf_v(
            t, "r.OpenFile(file, O_RDWR|O_CREATE, 0o1777) succeeded; want error");
    }
    if (BURROW_OK(os_root_mkdir(r, S("file"), 01777)))
        testing_t_errorf_v(t, "r.Mkdir(file, 0o1777) succeeded; want error");
    os_root_free(r);
}

static void TestRootUseAfterClose(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    OsRoot *r = os_open_root(a, temp_dir(t), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "OpenRoot: %s", error_text(e));
    (void)os_root_close(r);
    static const char *const names[] = {"Open", "Create", "OpenFile", "OpenRoot",
                                        "Mkdir"};
    for (int i = 0; i < 5; i++) {
        e = BURROW_NO_ERROR;
        switch (i) {
        case 0:
            (void)os_root_open(r, a, S("target"), &e);
            break;
        case 1:
            (void)os_root_create(r, a, S("target"), &e);
            break;
        case 2:
            (void)os_root_open_file(r, a, S("target"), OS_O_RDWR, 0666, &e);
            break;
        case 3:
            (void)os_root_open_root(r, a, S("target"), &e);
            break;
        default:
            e = os_root_mkdir(r, S("target"), 0777);
            break;
        }
        const FsPathError *pe = (const FsPathError *)errors_as(e, TYPE_FS_PATH_ERROR);
        if (pe == NULL || !str_eq(pe->path, S("target")) ||
            pe->err.data != os_err_closed.data || pe->err.vt != os_err_closed.vt)
            testing_t_errorf_v(
                t, "r.%s = %s; want &PathError{Path: \"target\", Err: ErrClosed}",
                str_from_cstr(names[i]), error_text(e));
    }
    os_root_free(r);
}

static void TestRootSymlinkToRoot(TestingT *t) {
    must_have_symlink(t);
    static const char *const fs[] = {"d/d => ..", NULL};
    Str dir = makefs(t, fs);
    Error e = BURROW_NO_ERROR;
    OsRoot *r = os_open_root(a, dir, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "OpenRoot: %s", error_text(e));
    testing_t_cleanup(t, BURROW_FN(Func, root_free_fn, r));
    e = os_root_mkdir(r, S("d/d/new"), 0777);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Mkdir: %s", error_text(e));
    OsFile *f = os_root_open(r, a, S("d/d"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Open: %s", error_text(e));
    Slice names = os_file_readdirnames(f, a, -1, &e);
    os_file_free(f);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Readdirnames: %s", error_text(e));
    sort_strings(names);
    const Str *n = (const Str *)names.p;
    if (names.len != 2 || !str_eq(n[0], S("d")) || !str_eq(n[1], S("new")))
        testing_t_errorf_v(t, "root contains %d names, want [d new]", names.len);
}

static void TestOpenInRoot(TestingT *t) {
    must_have_symlink(t);
    static const char *const fs[] = {"file", "link => ../ROOT/file", NULL};
    Str dir = makefs(t, fs);
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_open_in_root(a, dir, S("file"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "OpenInRoot(`file`) = %s, want success", error_text(e));
    os_file_free(f);
    Str names[] = {S("link"), S("../ROOT/file"), join(dir, S("file"))};
    for (int i = 0; i < 3; i++) {
        e = BURROW_NO_ERROR;
        f = os_open_in_root(a, dir, names[i], &e);
        if (BURROW_OK(e)) {
            os_file_free(f);
            testing_t_fatalf_v(t, "OpenInRoot(%q) = nil, want error", names[i]);
        }
    }
}

static void TestRootRemoveDot(TestingT *t) {
    Str dir = temp_dir(t);
    Error e = BURROW_NO_ERROR;
    OsRoot *r = os_open_root(a, dir, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "OpenRoot: %s", error_text(e));
    if (BURROW_OK(os_root_remove(r, S("."))))
        testing_t_errorf_v(t, "root.Remove(\".\") = nil, want error");
    if (BURROW_OK(os_root_remove_all(r, S("."))))
        testing_t_errorf_v(t, "root.RemoveAll(\".\") = nil, want error");
    (void)os_stat(a, dir, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "root.Remove(All)?(\".\") removed the root");
    os_root_free(r);
}

static void TestRootWriteReadFile(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    OsRoot *r = os_open_root(a, temp_dir(t), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "OpenRoot: %s", error_text(e));
    testing_t_cleanup(t, BURROW_FN(Func, root_free_fn, r));
    Str want = S("file contents");
    e = os_root_write_file(r, S("filename"), bytes_of(want), 0666);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "root.WriteFile(\"filename\") = %s; want nil",
                           error_text(e));
    Slice got = os_root_read_file(r, a, S("filename"), &e);
    if (BURROW_FAILED(e) || !str_eq(str_of(got), want))
        testing_t_fatalf_v(t, "root.ReadFile(\"filename\") = %q, %s; want %q",
                           str_of(got), error_text(e), want);
}

static void TestRootName(TestingT *t) {
    Str dir = temp_dir(t);
    Error e = BURROW_NO_ERROR;
    OsRoot *r = os_open_root(a, dir, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "OpenRoot: %s", error_text(e));
    testing_t_cleanup(t, BURROW_FN(Func, root_free_fn, r));
    if (!str_eq(os_root_name(r), dir))
        testing_t_errorf_v(t, "root.Name() = %q, want %q", os_root_name(r), dir);

    OsFile *f = os_root_create(r, a, S("file"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Create: %s", error_text(e));
    Str want = join(dir, S("file"));
    if (!str_eq(os_file_name(f), want))
        testing_t_errorf_v(t, "root.Create(\"file\").Name() = %q, want %q",
                           os_file_name(f), want);
    os_file_free(f);

    e = os_root_mkdir(r, S("dir"), 0777);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Mkdir: %s", error_text(e));
    OsRoot *sub = os_root_open_root(r, a, S("dir"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "OpenRoot: %s", error_text(e));
    want = join(dir, S("dir"));
    if (!str_eq(os_root_name(sub), want))
        testing_t_errorf_v(t, "root.OpenRoot(\"dir\").Name() = %q, want %q",
                           os_root_name(sub), want);
    os_root_free(sub);
}

/* ----------------------------------------------------------------- Root.FS */

static void TestRootFS(TestingT *t) {
    must_have_symlink(t);
    static const char *const fs[] = {"b", "a/", "a/x", "c/d/", "link => b", NULL};
    Str dir = makefs(t, fs);
    Error e = BURROW_NO_ERROR;
    OsRoot *r = os_open_root(a, dir, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "OpenRoot: %s", error_text(e));
    testing_t_cleanup(t, BURROW_FN(Func, root_free_fn, r));
    Fs fsys = os_root_fs(r);

    e = fstest_test_fs_v(fsys, S("b"), S("a/x"), S("c/d"), S("link"));
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "TestFS: %v", e);

    Slice b = fs_read_file(a, fsys, S("a/x"), &e);
    if (BURROW_FAILED(e) || !str_eq(str_of(b), S("a/x")))
        testing_t_errorf_v(t, "ReadFile(a/x) = %q, %s", str_of(b), error_text(e));

    /* ReadDir comes back sorted by name. */
    Slice ents = fs_read_dir(a, fsys, S("."), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "ReadDir: %s", error_text(e));
    static const char *const want[] = {"a", "b", "c", "link"};
    if (ents.len != 4) {
        testing_t_errorf_v(t, "ReadDir(.) gave %d entries, want 4", ents.len);
    } else {
        const FsDirEntry *de = (const FsDirEntry *)ents.p;
        for (Int i = 0; i < 4; i++) {
            Str name = de[i].vt->name(de[i].data);
            if (!str_eq(name, str_from_cstr(want[i])))
                testing_t_errorf_v(t, "ReadDir(.)[%d] = %q, want %s", i, name,
                                   str_from_cstr(want[i]));
        }
    }

    Str l = fs_read_link(a, fsys, S("link"), &e);
    if (BURROW_FAILED(e) || !str_eq(l, S("b")))
        testing_t_errorf_v(t, "ReadLink(link) = %q, %s", l, error_text(e));
    FsFileInfo fi = fs_lstat(a, fsys, S("link"), &e);
    if (BURROW_FAILED(e) || (fi.vt->mode(fi.data) & FS_MODE_SYMLINK) == 0)
        testing_t_errorf_v(t, "Lstat(link) is not a link: %s", error_text(e));
    fi = fs_stat(a, fsys, S("link"), &e);
    if (BURROW_FAILED(e) || fi.vt->size(fi.data) != 1)
        testing_t_errorf_v(t, "Stat(link): %s", error_text(e));

    /* Names an FS does not take: rooted, with dot-dot, or empty. */
    Str bad[] = {S("/b"), S("../ROOT/b"), S(""), S("a/../b")};
    for (int i = 0; i < 4; i++) {
        e = BURROW_NO_ERROR;
        (void)fs_read_file(a, fsys, bad[i], &e);
        if (!errors_is(e, fs_err_invalid))
            testing_t_errorf_v(t, "ReadFile(%q) = %s, want ErrInvalid", bad[i],
                               error_text(e));
    }
}

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

#define TESTS(X)                                                                       \
    X(TestRootOpen_File)                                                               \
    X(TestRootOpen_Directory)                                                          \
    X(TestRootCreate)                                                                  \
    X(TestRootChmod)                                                                   \
    X(TestRootChtimes)                                                                 \
    X(TestRootMkdir)                                                                   \
    X(TestRootMkdirAll)                                                                \
    X(TestRootOpenRoot)                                                                \
    X(TestRootRemoveFile)                                                              \
    X(TestRootRemoveDirectory)                                                         \
    X(TestRootRemoveAll)                                                               \
    X(TestRootOpenFileAsRoot)                                                          \
    X(TestRootStat)                                                                    \
    X(TestRootLstat)                                                                   \
    X(TestRootReadlink)                                                                \
    X(TestRootRenameFrom)                                                              \
    X(TestRootLinkFrom)                                                                \
    X(TestRootRenameTo)                                                                \
    X(TestRootLinkTo)                                                                  \
    X(TestRootSymlink)                                                                 \
    X(TestRootNonPermissionMode)                                                       \
    X(TestRootUseAfterClose)                                                           \
    X(TestRootSymlinkToRoot)                                                           \
    X(TestOpenInRoot)                                                                  \
    X(TestRootRemoveDot)                                                               \
    X(TestRootWriteReadFile)                                                           \
    X(TestRootName)                                                                    \
    X(TestRootFS)

static int os_root_main(TestingM *m) {
    setup();
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(os_root_main, TESTS)
