/* Derived from Go's src/path/filepath/path_test.go and match_test.go. Go
 * source: go1.27.1.
 *
 * The tests of the part of filepath that asks the operating system: Abs,
 * EvalSymlinks, Glob, Walk and WalkDir. The lexical part is in
 * filepath_test.c. These run on the host's rules only.
 *
 * Go's TestGlob and TestNonWindowsGlobEscape look for match.go in the
 * directory they run in, and TestBug3486 walks GOROOT/src/unicode. Here each
 * makes the few files it looks for in a temporary directory. TestWalkFileError
 * is left out, because it swaps Walk's os.Lstat for one that fails, and there
 * is no such hook here.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/path/filepath.h"
#include "burrow/sort.h"
#include "burrow/strings.h"
#include "burrow/syscall.h"

#include <string.h>

#define S(lit) BURROW_S(lit)

#if defined(BURROW_OS_WINDOWS)
#define HOST_WIN true
#define SEP "\\"
#else
#define HOST_WIN false
#define SEP "/"
#endif

static Arena ar;
static Alloc *a;

static void remove_tree(void *env) {
    (void)os_remove_all(*(Str *)env);
}

/* t.TempDir: a new directory, removed when the test ends. */
static Str temp_dir(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Str *d = BURROW_NEW(a, Str);
    *d = os_mkdir_temp(a, S(""), S("burrow-filepath-test-*"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "MkdirTemp: %s", error_text(e));
    testing_t_cleanup(t, BURROW_FN(Func, remove_tree, d));
    return *d;
}

static void chdir_back(void *env) {
    (void)os_chdir(*(Str *)env);
}

/* t.Chdir: dir becomes the working directory until the test ends. A cleanup
 * registered after temp_dir runs before the directory is removed, which
 * Windows needs, since it will not remove the working directory. */
static void t_chdir(TestingT *t, Str dir) {
    Error e = BURROW_NO_ERROR;
    Str *wd = BURROW_NEW(a, Str);
    *wd = os_getwd(a, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Getwd: %s", error_text(e));
    e = os_chdir(dir);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Chdir %s: %s", dir, error_text(e));
    testing_t_cleanup(t, BURROW_FN(Func, chdir_back, wd));
}

static Str join(Str dir, Str name) {
    return filepath_join_v(a, 2, dir, name);
}

static Str cat(Str x, Str y) {
    return strings_join(a, slice_from((Str[]){x, y}, 2, 2, TYPE_STRING), S(""));
}

/* simpleJoin: dir and path with a separator between them, and without the
 * cleaning Join does, so that ".." stays. */
static Str simple_join(Str dir, Str path) {
    return cat(cat(dir, S(SEP)), path);
}

static void touch(TestingT *t, Str name) {
    Error e = os_write_file(name, slice_nil(TYPE_BYTE), 0666);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Create %s: %s", name, error_text(e));
}

static void mkdir_all(TestingT *t, Str name) {
    Error e = os_mkdir_all(name, 0755);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "MkdirAll %s: %s", name, error_text(e));
}

/* testenv.MustHaveSymlink. Windows needs a privilege to make links, and wine
 * says it made one when nothing on disk follows it, so the first call makes a
 * link and checks it reads as one and leads to its target. */
#if defined(BURROW_OS_WINDOWS)
static bool is_symlink(OsFileInfo fi) {
    return (fi.vt->mode(fi.data) & OS_MODE_SYMLINK) != 0;
}
#endif

static void must_have_symlink(TestingT *t) {
#if defined(BURROW_OS_WINDOWS)
    static int have; /* 0 not known yet, 1 yes, -1 no */
    if (have == 0) {
        Str d = temp_dir(t);
        touch(t, join(d, S("target")));
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
#else
    (void)t;
#endif
}

static void symlink_or_fatal(TestingT *t, Str target, Str link) {
    Error e = os_symlink(target, link);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Symlink: %s", error_text(e));
}

static bool contains(Slice list, Str s) {
    for (Int i = 0; i < list.len; i++)
        if (str_eq(*(Str *)slice_at(list, i), s))
            return true;
    return false;
}

static bool equal(Slice x, Slice y) {
    if (x.len != y.len)
        return false;
    for (Int i = 0; i < x.len; i++)
        if (!str_eq(*(Str *)slice_at(x, i), *(Str *)slice_at(y, i)))
            return false;
    return true;
}

/* A Slice of Str as Go's %q prints one. */
static Str quote_list(Slice list) {
    Str out = S("[");
    for (Int i = 0; i < list.len; i++)
        out = cat(out,
                  fmt_sprintf_v(a, i == 0 ? "%q" : " %q", *(Str *)slice_at(list, i)));
    return cat(out, S("]"));
}

static Slice str_list(const char *const *s) {
    Slice out = slice_nil(TYPE_STRING);
    for (Int i = 0; s[i] != NULL; i++) {
        Str x = str_from_cstr(s[i]);
        out = slice_append(a, out, &x, 1);
    }
    return out;
}

/* -------------------------------------------------------------------- Glob */

/* The directory Go's TestGlob runs in, src/path/filepath, with match.go in
 * it, made under a temporary directory and made the working directory. */
static void chdir_filepath_dir(TestingT *t) {
    Str root = temp_dir(t);
    Str dir = join(root, S("filepath"));
    mkdir_all(t, dir);
    touch(t, join(dir, S("match.go")));
    t_chdir(t, dir);
}

static void TestGlob(TestingT *t) {
    chdir_filepath_dir(t);
    static const struct {
        const char *pattern, *result;
    } tests[] = {
        {"match.go", "match.go"},
        {"mat?h.go", "match.go"},
        {"*", "match.go"},
        {"../*/match.go", "../filepath/match.go"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str pattern = str_from_cstr(tests[i].pattern);
        Str result = str_from_cstr(tests[i].result);
        if (HOST_WIN) {
            pattern = filepath_clean(a, pattern);
            result = filepath_clean(a, result);
        }
        Error e = BURROW_NO_ERROR;
        Slice matches = filepath_glob(a, pattern, &e);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "Glob error for %q: %s", pattern, error_text(e));
            continue;
        }
        if (!contains(matches, result))
            testing_t_errorf_v(t, "Glob(%s) = %s want %s", pattern, quote_list(matches),
                               result);
    }
    static const char *const none[] = {"no_match", "../*/no_match"};
    for (size_t i = 0; i < 2; i++) {
        Str pattern = str_from_cstr(none[i]);
        Error e = BURROW_NO_ERROR;
        Slice matches = filepath_glob(a, pattern, &e);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "Glob error for %q: %s", pattern, error_text(e));
            continue;
        }
        if (matches.len != 0)
            testing_t_errorf_v(t, "Glob(%s) = %s want []", pattern,
                               quote_list(matches));
    }
}

static void TestCVE202230632(TestingT *t) {
    /* Before CVE-2022-30632 this ran out of stack, given enough separators.
     * Now there is a limit of 10,000. */
    Error e = BURROW_NO_ERROR;
    (void)filepath_glob(a, cat(S("/*"), strings_repeat(a, S("/"), 10001)), &e);
    if (!errors_is(e, filepath_err_bad_pattern) ||
        e.data != filepath_err_bad_pattern.data)
        testing_t_fatalf_v(t, "Glob returned err=%s, want ErrBadPattern",
                           error_text(e));
}

static void TestGlobError(TestingT *t) {
    static const char *const bad[] = {"[]", "nonexist/[]"};
    for (size_t i = 0; i < 2; i++) {
        Error e = BURROW_NO_ERROR;
        (void)filepath_glob(a, str_from_cstr(bad[i]), &e);
        if (e.data != filepath_err_bad_pattern.data)
            testing_t_errorf_v(t, "Glob(%s) returned err=%s, want ErrBadPattern",
                               str_from_cstr(bad[i]), error_text(e));
    }
}

static void TestGlobUNC(TestingT *t) {
    (void)t;
    /* Only that this runs without crashing. See Go issue 15879. */
    Error e = BURROW_NO_ERROR;
    (void)filepath_glob(a, S("\\\\?\\C:\\*"), &e);
}

static void TestGlobSymlink(TestingT *t) {
    must_have_symlink(t);
    static const struct {
        const char *path, *dest;
        bool broken_link;
    } tests[] = {
        {"test1", "link1", false},
        {"test2", "link2", true},
    };
    Str tmp = temp_dir(t);
    for (size_t i = 0; i < 2; i++) {
        Str path = join(tmp, str_from_cstr(tests[i].path));
        Str dest = join(tmp, str_from_cstr(tests[i].dest));
        touch(t, path);
        symlink_or_fatal(t, path, dest);
        if (tests[i].broken_link)
            (void)os_remove(path); /* break the symlink */
        Error e = BURROW_NO_ERROR;
        Slice matches = filepath_glob(a, dest, &e);
        if (BURROW_FAILED(e))
            testing_t_errorf_v(t, "GlobSymlink error for %q: %s", dest, error_text(e));
        if (!contains(matches, dest))
            testing_t_errorf_v(t, "Glob(%s) = %s want %s", dest, quote_list(matches),
                               dest);
    }
}

typedef struct GlobTest {
    const char *pattern;
    const char *matches[4];
} GlobTest;

static Slice build_want(const GlobTest *test, Str root) {
    Slice want = slice_nil(TYPE_STRING);
    for (Int i = 0; test->matches[i] != NULL; i++) {
        Str m = cat(root, filepath_from_slash(a, str_from_cstr(test->matches[i])));
        want = slice_append(a, want, &m, 1);
    }
    sort_strings(want);
    return want;
}

static bool glob_abs(TestingT *t, const GlobTest *test, Str root, Str root_pattern) {
    Str p = filepath_from_slash(
        a, cat(cat(root_pattern, S("\\")), str_from_cstr(test->pattern)));
    Error e = BURROW_NO_ERROR;
    Slice have = filepath_glob(a, p, &e);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "Glob(%q): %s", p, error_text(e));
        return false;
    }
    sort_strings(have);
    Slice want = build_want(test, cat(root, S("\\")));
    if (equal(want, have))
        return true;
    testing_t_errorf_v(t, "Glob(%q) returns %s, but %s expected", p, quote_list(have),
                       quote_list(want));
    return false;
}

static void glob_rel(TestingT *t, const GlobTest *test, Str root) {
    Str p = cat(root, filepath_from_slash(a, str_from_cstr(test->pattern)));
    Error e = BURROW_NO_ERROR;
    Slice have = filepath_glob(a, p, &e);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "Glob(%q): %s", p, error_text(e));
        return;
    }
    sort_strings(have);
    Slice want = build_want(test, root);
    /* The version without the root prefix is fine too. */
    if (equal(want, have) || equal(build_want(test, S("")), have))
        return;
    testing_t_errorf_v(t, "Glob(%q) returns %s, but %s expected", p, quote_list(have),
                       quote_list(want));
}

/* strings.Replace(s, old, new, 1). */
static Str replace1(Str s, Str old, Str new_) {
    return strings_replace(a, s, old, new_, 1);
}

static Str temp_dir_canonical(TestingT *t) {
    Str dir = temp_dir(t);
    Error e = BURROW_NO_ERROR;
    Str cdir = filepath_eval_symlinks(a, dir, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "tempDirCanonical: %s", error_text(e));
    return cdir;
}

static void TestWindowsGlob(TestingT *t) {
    if (!HOST_WIN)
        testing_t_skip_v(t, "skipping windows specific test");

    Str tmp = temp_dir_canonical(t);
    if (tmp.len < 3)
        testing_t_fatalf_v(t, "tmpDir path %q is too short", tmp);
    if (tmp.p[1] != ':')
        testing_t_fatalf_v(t, "tmpDir path %q must have drive letter in it", tmp);

    mkdir_all(t, join(tmp, S("a")));
    mkdir_all(t, join(tmp, S("b")));
    mkdir_all(t, join(tmp, S("dir/d/bin")));
    touch(t, join(tmp, S("dir/d/bin/git.exe")));

    static const GlobTest tests[] = {
        {"a", {"a", NULL}},    {"b", {"b", NULL}},
        {"c", {NULL}},         {"*", {"a", "b", "dir", NULL}},
        {"d*", {"dir", NULL}}, {"*i*", {"dir", NULL}},
        {"*r", {"dir", NULL}}, {"?ir", {"dir", NULL}},
        {"?r", {NULL}},        {"d*/*/bin/git.exe", {"dir/d/bin/git.exe", NULL}},
    };
    Int n = (Int)(sizeof tests / sizeof tests[0]);

    /* Absolute paths. */
    for (Int i = 0; i < n; i++) {
        (void)glob_abs(t, &tests[i], tmp, tmp);
        /* C:\*Documents and Settings\... */
        (void)glob_abs(t, &tests[i], tmp, replace1(tmp, S(":\\"), S(":\\*")));
        /* C:\Documents and Settings*\... */
        Str p = replace1(tmp, S(":\\"), S(":"));
        p = replace1(p, S("\\"), S("*\\"));
        p = replace1(p, S(":"), S(":\\"));
        (void)glob_abs(t, &tests[i], tmp, p);
    }

    /* Relative paths. */
    t_chdir(t, tmp);
    for (Int i = 0; i < n; i++) {
        glob_rel(t, &tests[i], S(""));
        glob_rel(t, &tests[i], S(".\\"));
        glob_rel(t, &tests[i], (Str){tmp.p, 2}); /* C: */
    }
}

static void TestNonWindowsGlobEscape(TestingT *t) {
    if (HOST_WIN)
        testing_t_skip_v(t, "skipping non-windows specific test");
    chdir_filepath_dir(t);
    Str pattern = S("\\match.go");
    Error e = BURROW_NO_ERROR;
    Slice matches = filepath_glob(a, pattern, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Glob error for %q: %s", pattern, error_text(e));
    static const char *const want[] = {"match.go", NULL};
    if (!equal(matches, str_list(want)))
        testing_t_fatalf_v(t, "Glob(%s) = %s want [match.go]", pattern,
                           quote_list(matches));
}

/* -------------------------------------------------------------------- Walk */

typedef struct Node {
    const char *name;
    struct Node *entries; /* NULL for a file */
    Int n;
    int mark;
} Node;

static Node d_z[] = {{"u", NULL, 0, 0}, {"v", NULL, 0, 0}};
static Node d_y[1]; /* an empty directory, of which only the address is used */
static Node d_d[] = {{"x", NULL, 0, 0}, {"y", d_y, 0, 0}, {"z", d_z, 2, 0}};
static Node d_b[1];
static Node d_top[] = {
    {"a", NULL, 0, 0}, {"b", d_b, 0, 0}, {"c", NULL, 0, 0}, {"d", d_d, 3, 0}};
static Node tree = {"testdata", d_top, 4, 0};

typedef void (*NodeFn)(void *env, Str path, Node *n);

static void walk_tree(Node *n, Str path, NodeFn f, void *env) {
    f(env, path, n);
    for (Int i = 0; i < n->n; i++)
        walk_tree(&n->entries[i], join(path, str_from_cstr(n->entries[i].name)), f,
                  env);
}

static void make_node(void *env, Str path, Node *n) {
    TestingT *t = (TestingT *)env;
    if (n->entries == NULL)
        touch(t, path);
    else
        (void)os_mkdir(path, 0770);
}

static void make_tree(TestingT *t) {
    walk_tree(&tree, str_from_cstr(tree.name), make_node, t);
}

static void mark_node(void *env, Str path, Node *n) {
    (void)env;
    (void)path;
    n->mark++;
}

static void mark_tree(Node *n) {
    walk_tree(n, S(""), mark_node, NULL);
}

typedef struct CheckEnv {
    TestingT *t;
    bool report;
} CheckEnv;

static void check_node(void *env, Str path, Node *n) {
    CheckEnv *c = (CheckEnv *)env;
    if (n->mark != 1 && c->report)
        testing_t_errorf_v(c->t, "node %s mark = %d; expected 1", path, n->mark);
    n->mark = 0;
}

static void check_marks(TestingT *t, bool report) {
    CheckEnv c = {t, report};
    walk_tree(&tree, str_from_cstr(tree.name), check_node, &c);
}

typedef struct MarkEnv {
    Str name;
    Slice *errors;
    bool clear;
} MarkEnv;

static void mark_named(void *env, Str path, Node *n) {
    (void)path;
    if (str_eq(str_from_cstr(n->name), *(Str *)env))
        n->mark++;
}

/* mark: each node name is unique, which is good enough for a test. With
 * clear set an incoming error is cleared before returning. The errors are
 * collected either way. */
static Error mark(FsDirEntry d, Error err, MarkEnv *m) {
    Str name = d.vt->name(d.data);
    walk_tree(&tree, str_from_cstr(tree.name), mark_named, &name);
    if (BURROW_FAILED(err)) {
        *m->errors = slice_append(a, *m->errors, &err, 1);
        if (m->clear)
            return BURROW_NO_ERROR;
        return err;
    }
    return BURROW_NO_ERROR;
}

static Error mark_fn(void *env, Str path, FsDirEntry d, Error err) {
    (void)path;
    return mark(d, err, (MarkEnv *)env);
}

/* TestWalk calls Walk with a WalkFunc that turns the info into a DirEntry and
 * calls the same function TestWalkDir uses. */
typedef struct AdaptEnv {
    FsWalkDirFunc fn;
} AdaptEnv;

static Error adapt_fn(void *env, Str path, FsFileInfo info, Error err) {
    AdaptEnv *ad = (AdaptEnv *)env;
    return ad->fn.f(ad->fn.env, path, fs_file_info_to_dir_entry(a, info), err);
}

static Error walk_with_walk(Str root, FsWalkDirFunc fn) {
    AdaptEnv ad = {fn};
    return filepath_walk(a, root, BURROW_FN(FilepathWalkFunc, adapt_fn, &ad));
}

static Error walk_with_walk_dir(Str root, FsWalkDirFunc fn) {
    return filepath_walk_dir(a, root, fn);
}

typedef Error (*WalkerFn)(Str root, FsWalkDirFunc fn);

static WalkerFn perm_walk;
static int perm_err_visit;
static MarkEnv *perm_env;

static Str error_list(Slice errs) {
    Str out = S("[");
    for (Int i = 0; i < errs.len; i++)
        out = cat(cat(out, i == 0 ? S("") : S(" ")),
                  error_text(*(Error *)slice_at(errs, i)));
    return cat(out, S("]"));
}

static void test_walk_perm_err(void *env, TestingT *t) {
    (void)env;
    /* Permission errors can only be made when not root, and only on some
     * file systems, so Go skips this in short mode. */
    if (HOST_WIN)
        testing_t_skip_v(t, "skipping on windows");
    if (os_getuid() == 0)
        testing_t_skip_v(t, "skipping as root");
    if (testing_short())
        testing_t_skip_v(t, "skipping in short mode");

    MarkEnv *m = perm_env;
    FsWalkDirFunc fn = BURROW_FN(FsWalkDirFunc, mark_fn, m);
    Str top = str_from_cstr(tree.name);
    Str e1 = join(top, str_from_cstr(tree.entries[1].name));
    Str e3 = join(top, str_from_cstr(tree.entries[3].name));

    /* Two errors: the top level directories chmod'ed to 0. */
    (void)os_chmod(e1, 0);
    (void)os_chmod(e3, 0);

    /* Collect the errors and expect two. The subtrees that cannot be read
     * are marked by hand, less the visit of the directory itself. */
    mark_tree(&tree.entries[1]);
    mark_tree(&tree.entries[3]);
    tree.entries[1].mark -= perm_err_visit;
    tree.entries[3].mark -= perm_err_visit;
    Error err = perm_walk(top, fn);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "expected no error return from Walk, got %s",
                           error_text(err));
    if (m->errors->len != 2)
        testing_t_errorf_v(t, "expected 2 errors, got %d: %s", m->errors->len,
                           error_list(*m->errors));
    check_marks(t, true);
    m->errors->len = 0;

    /* Collect the errors and stop after the first. */
    mark_tree(&tree.entries[1]);
    mark_tree(&tree.entries[3]);
    tree.entries[1].mark -= perm_err_visit;
    tree.entries[3].mark -= perm_err_visit;
    m->clear = false; /* the error will stop the walk */
    err = perm_walk(top, fn);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "expected error return from Walk");
    if (m->errors->len != 1)
        testing_t_errorf_v(t, "expected 1 error, got %d: %s", m->errors->len,
                           error_list(*m->errors));
    check_marks(t, false);
    m->errors->len = 0;

    (void)os_chmod(e1, 0770);
    (void)os_chmod(e3, 0770);
}

static void test_walk(TestingT *t, WalkerFn walk, int err_visit) {
    t_chdir(t, temp_dir(t));
    make_tree(t);

    Slice errors = slice_make(a, TYPE_ERROR, 0, 10);
    MarkEnv m = {BURROW_STR_EMPTY, &errors, true};
    FsWalkDirFunc fn = BURROW_FN(FsWalkDirFunc, mark_fn, &m);

    /* Expect no errors. */
    Error err = walk(str_from_cstr(tree.name), fn);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "no error expected, found: %s", error_text(err));
    if (errors.len != 0)
        testing_t_fatalf_v(t, "unexpected errors: %s", error_list(errors));
    check_marks(t, true);
    errors.len = 0;

    perm_walk = walk;
    perm_err_visit = err_visit;
    perm_env = &m;
    (void)testing_t_run(t, S("PermErr"),
                        BURROW_FN(TestingTFunc, test_walk_perm_err, NULL));
}

static void TestWalk(TestingT *t) {
    test_walk(t, walk_with_walk, 1);
}

static void TestWalkDir(TestingT *t) {
    test_walk(t, walk_with_walk_dir, 2);
}

/* What TestWalkSkipDirOnFile and TestWalkSkipAllOnFile's walkers see. */
typedef struct SkipEnv {
    bool saw;
} SkipEnv;

static Error skip_dir_walker(SkipEnv *s, Str path) {
    if (strings_has_suffix(path, S("foo2")))
        s->saw = true;
    if (strings_has_suffix(path, S("foo1")))
        return filepath_skip_dir;
    return BURROW_NO_ERROR;
}

static Error skip_dir_walk_fn(void *env, Str path, FsFileInfo info, Error err) {
    (void)info;
    (void)err;
    return skip_dir_walker((SkipEnv *)env, path);
}

static Error skip_dir_walk_dir_fn(void *env, Str path, FsDirEntry d, Error err) {
    (void)d;
    (void)err;
    return skip_dir_walker((SkipEnv *)env, path);
}

static void TestWalkSkipDirOnFile(TestingT *t) {
    Str td = temp_dir(t);
    mkdir_all(t, join(td, S("dir")));
    touch(t, join(td, S("dir/foo1")));
    touch(t, join(td, S("dir/foo2")));

    /* Go's version passes td as the root both times, whatever root it was
     * given, so the second call of each is the same as the first. */
    for (int walk_dir = 0; walk_dir < 2; walk_dir++) {
        for (int i = 0; i < 2; i++) {
            SkipEnv s = {false};
            Error err =
                walk_dir
                    ? filepath_walk_dir(
                          a, td, BURROW_FN(FsWalkDirFunc, skip_dir_walk_dir_fn, &s))
                    : filepath_walk(a, td,
                                    BURROW_FN(FilepathWalkFunc, skip_dir_walk_fn, &s));
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s", error_text(err));
            if (s.saw)
                testing_t_errorf_v(
                    t, "%s: SkipDir on file foo1 did not block processing of foo2",
                    walk_dir ? S("WalkDir") : S("Walk"));
        }
    }
}

typedef struct SkipAllEnv {
    bool remaining_were_skipped;
} SkipAllEnv;

static Error skip_all_walker(SkipAllEnv *s, Str path) {
    if (strings_has_suffix(path, S("foo2")))
        return filepath_skip_all;
    if (strings_has_suffix(path, S("foo3")) || strings_has_suffix(path, S("foo4")) ||
        strings_has_suffix(path, S("bar")) || strings_has_suffix(path, S("last")))
        s->remaining_were_skipped = false;
    return BURROW_NO_ERROR;
}

static Error skip_all_walk_fn(void *env, Str path, FsFileInfo info, Error err) {
    (void)info;
    (void)err;
    return skip_all_walker((SkipAllEnv *)env, path);
}

static Error skip_all_walk_dir_fn(void *env, Str path, FsDirEntry d, Error err) {
    (void)d;
    (void)err;
    return skip_all_walker((SkipAllEnv *)env, path);
}

static void TestWalkSkipAllOnFile(TestingT *t) {
    Str td = temp_dir(t);
    mkdir_all(t, join(td, S("dir/subdir")));
    mkdir_all(t, join(td, S("dir2")));
    touch(t, join(td, S("dir/foo1")));
    touch(t, join(td, S("dir/foo2")));
    touch(t, join(td, S("dir/subdir/foo3")));
    touch(t, join(td, S("dir/foo4")));
    touch(t, join(td, S("dir2/bar")));
    touch(t, join(td, S("last")));

    for (int walk_dir = 0; walk_dir < 2; walk_dir++) {
        for (int i = 0; i < 2; i++) {
            SkipAllEnv s = {true};
            Error err =
                walk_dir
                    ? filepath_walk_dir(
                          a, td, BURROW_FN(FsWalkDirFunc, skip_all_walk_dir_fn, &s))
                    : filepath_walk(a, td,
                                    BURROW_FN(FilepathWalkFunc, skip_all_walk_fn, &s));
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%s", error_text(err));
            if (!s.remaining_were_skipped)
                testing_t_errorf_v(
                    t,
                    "%s: SkipAll on file foo2 did not block processing of "
                    "remaining files and directories",
                    walk_dir ? S("WalkDir") : S("Walk"));
        }
    }
}

typedef struct CollectEnv {
    TestingT *t;
    Slice walked;
} CollectEnv;

static Error collect_fn(void *env, Str path, FsFileInfo info, Error err) {
    (void)info;
    CollectEnv *c = (CollectEnv *)env;
    if (BURROW_FAILED(err))
        return err;
    Str p = filepath_clean(a, path);
    c->walked = slice_append(a, c->walked, &p, 1);
    return BURROW_NO_ERROR;
}

static void TestWalkSymlinkRoot(TestingT *t) {
    must_have_symlink(t);
    Str td = temp_dir(t);
    Str dir = join(td, S("dir"));
    mkdir_all(t, dir);
    touch(t, join(dir, S("foo")));
    Str link = join(td, S("link"));
    symlink_or_fatal(t, S("dir"), link);
    Str abslink = join(td, S("abslink"));
    symlink_or_fatal(t, dir, abslink);
    Str linklink = join(td, S("linklink"));
    symlink_or_fatal(t, S("link"), linklink);

    /* POSIX says a path that ends in a slash is only resolved if what comes
     * before the slash is a directory. Walk does not follow links itself, so
     * a root without a slash is the link and a root with one is what it
     * points to, which gets walked. */
    struct {
        const char *desc;
        Str root;
        Str want[2];
        Int nwant;
        bool buggy_darwin; /* Go issue 59586 */
    } tests[] = {
        {"no slash", link, {link}, 1, false},
        {"slash", cat(link, S(SEP)), {link, join(link, S("foo"))}, 2, false},
        {"abs no slash", abslink, {abslink}, 1, false},
        {"abs with slash",
         cat(abslink, S(SEP)),
         {abslink, join(abslink, S("foo"))},
         2,
         false},
        {"double link no slash", linklink, {linklink}, 1, false},
        {"double link with slash",
         cat(linklink, S(SEP)),
         {linklink, join(linklink, S("foo"))},
         2,
         true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        CollectEnv c = {t, slice_nil(TYPE_STRING)};
        Error err = filepath_walk(a, tests[i].root,
                                  BURROW_FN(FilepathWalkFunc, collect_fn, &c));
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: %s", str_from_cstr(tests[i].desc),
                               error_text(err));
            continue;
        }
        Slice want =
            slice_from(tests[i].want, tests[i].nwant, tests[i].nwant, TYPE_STRING);
        if (!equal(c.walked, want)) {
            testing_t_logf_v(t, "%s: Walk(%q) visited %s; want %s",
                             str_from_cstr(tests[i].desc), tests[i].root,
                             quote_list(c.walked), quote_list(want));
#if defined(BURROW_OS_DARWIN)
            if (tests[i].buggy_darwin) {
                testing_t_logf_v(t, "(ignoring known bug on darwin)");
                continue;
            }
#endif
            testing_t_fail(t);
        }
    }
}

/* ------------------------------------------------------------ EvalSymlinks */

typedef struct EvalSymlinksTest {
    /* With dest empty, path is made as a directory. Otherwise path is made as
     * a link to dest. */
    const char *path, *dest;
} EvalSymlinksTest;

static const EvalSymlinksTest eval_symlinks_test_dirs[] = {
    {"test", ""},
    {"test/dir", ""},
    {"test/dir/link3", "../../"},
    {"test/link1", "../test"},
    {"test/link2", "dir"},
    {"test/linkabs", "/"},
    {"test/link4", "../test2"},
    {"test2", "test/dir"},
    /* Go issue 23444. */
    {"src", ""},
    {"src/pool", ""},
    {"src/pool/test", ""},
    {"src/versions", ""},
    {"src/versions/current", "../../version"},
    {"src/versions/v1", ""},
    {"src/versions/v1/modules", ""},
    {"src/versions/v1/modules/test", "../../../pool/test"},
    {"version", "src/versions/v1"},
};

static const EvalSymlinksTest eval_symlinks_tests[] = {
    {"test", "test"},
    {"test/dir", "test/dir"},
    {"test/dir/../..", "."},
    {"test/link1", "test"},
    {"test/link2", "test/dir"},
    {"test/link1/dir", "test/dir"},
    {"test/link2/..", "test"},
    {"test/dir/link3", "."},
    {"test/link2/link3/test", "test"},
    {"test/linkabs", "/"},
    {"test/link4/..", "test"},
    {"src/versions/current/modules/test", "src/pool/test"},
};

static void test_eval_symlinks(TestingT *t, Str path, Str want) {
    Error e = BURROW_NO_ERROR;
    Str have = filepath_eval_symlinks(a, path, &e);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "EvalSymlinks(%q) error: %s", path, error_text(e));
        return;
    }
    if (!str_eq(filepath_clean(a, have), filepath_clean(a, want)))
        testing_t_errorf_v(t, "EvalSymlinks(%q) returns %q, want %q", path, have, want);
}

/* Go's t.Chdir is undone when the test ends, and testEvalSymlinksAfterChdir
 * calls it once per case, so the working directory only goes back at the
 * end. Here it goes back after each call, which ends up the same. */
static void test_eval_symlinks_after_chdir(TestingT *t, Str wd, Str path, Str want) {
    Error e = BURROW_NO_ERROR;
    Str back = os_getwd(a, &e);
    if (BURROW_OK(e))
        e = os_chdir(wd);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "chdir %s: %s", wd, error_text(e));
    Str have = filepath_eval_symlinks(a, path, &e);
    (void)os_chdir(back);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "EvalSymlinks(%q) in %q directory error: %s", path, wd,
                           error_text(e));
        return;
    }
    if (!str_eq(filepath_clean(a, have), filepath_clean(a, want)))
        testing_t_errorf_v(t, "EvalSymlinks(%q) in %q directory returns %q, want %q",
                           path, wd, have, want);
}

static void TestEvalSymlinks(TestingT *t) {
    must_have_symlink(t);
    Str tmp = temp_dir(t);

    /* /tmp may be a symlink itself. That is avoided here, at the cost of
     * trusting the thing under test. */
    Error e = BURROW_NO_ERROR;
    tmp = filepath_eval_symlinks(a, tmp, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "eval symlink for tmp dir: %s", error_text(e));

    /* The symlink farm, made with relative paths. */
    for (size_t i = 0;
         i < sizeof eval_symlinks_test_dirs / sizeof eval_symlinks_test_dirs[0]; i++) {
        const EvalSymlinksTest *d = &eval_symlinks_test_dirs[i];
        Str path = simple_join(tmp, str_from_cstr(d->path));
        Error err = d->dest[0] == 0 ? os_mkdir(path, 0755)
                                    : os_symlink(str_from_cstr(d->dest), path);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
    }

    for (size_t i = 0; i < sizeof eval_symlinks_tests / sizeof eval_symlinks_tests[0];
         i++) {
        Str tpath = str_from_cstr(eval_symlinks_tests[i].path);
        Str tdest = str_from_cstr(eval_symlinks_tests[i].dest);
        bool dest_abs =
            filepath_is_abs(tdest) || filepath_is_path_separator(tdest.p[0]);

        Str path = simple_join(tmp, tpath);
        Str dest = dest_abs ? tdest : simple_join(tmp, tdest);
        test_eval_symlinks(t, path, dest);

        /* EvalSymlinks(".") */
        test_eval_symlinks_after_chdir(t, path, S("."), S("."));

        /* EvalSymlinks("C:.") on Windows */
        if (HOST_WIN) {
            Str vol_dot = cat(filepath_volume_name(a, tmp), S("."));
            test_eval_symlinks_after_chdir(t, path, vol_dot, vol_dot);
        }

        /* EvalSymlinks(".." + path) */
        Str dotdot_path = dest_abs ? tdest : simple_join(S(".."), tdest);
        test_eval_symlinks_after_chdir(t, simple_join(tmp, S("test")),
                                       simple_join(S(".."), tpath), dotdot_path);

        /* EvalSymlinks(p) for a relative p */
        test_eval_symlinks_after_chdir(t, tmp, tpath, tdest);
    }
}

static void TestEvalSymlinksIsNotExist(TestingT *t) {
    must_have_symlink(t);
    t_chdir(t, temp_dir(t));

    Error e = BURROW_NO_ERROR;
    (void)filepath_eval_symlinks(a, S("notexist"), &e);
    if (!os_is_not_exist(e))
        testing_t_errorf_v(t, "expected the file is not found, got %s", error_text(e));

    symlink_or_fatal(t, S("notexist"), S("link"));
    e = BURROW_NO_ERROR;
    (void)filepath_eval_symlinks(a, S("link"), &e);
    if (!os_is_not_exist(e))
        testing_t_errorf_v(t, "expected the file is not found, got %s", error_text(e));
    (void)os_remove(S("link"));
}

static void TestIssue13582(TestingT *t) {
    must_have_symlink(t);
    Str tmp = temp_dir(t);

    Str dir = join(tmp, S("dir"));
    Error e = os_mkdir(dir, 0755);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "%s", error_text(e));
    Str link_to_dir = join(tmp, S("link_to_dir"));
    symlink_or_fatal(t, dir, link_to_dir);
    Str file = join(link_to_dir, S("file"));
    touch(t, file);
    Str link1 = join(link_to_dir, S("link1"));
    symlink_or_fatal(t, file, link1);
    Str link2 = join(link_to_dir, S("link2"));
    symlink_or_fatal(t, link1, link2);

    /* /tmp may be a symlink itself. */
    Str real_tmp = filepath_eval_symlinks(a, tmp, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "%s", error_text(e));
    Str real_dir = join(real_tmp, S("dir"));
    Str real_file = join(real_dir, S("file"));

    Str tests[][2] = {
        {dir, real_dir},    {link_to_dir, real_dir}, {file, real_file},
        {link1, real_file}, {link2, real_file},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str have = filepath_eval_symlinks(a, tests[i][0], &e);
        if (BURROW_FAILED(e))
            testing_t_fatalf_v(t, "%s", error_text(e));
        if (!str_eq(have, tests[i][1]))
            testing_t_errorf_v(t, "test#%d: EvalSymlinks(%q) returns %q, want %q",
                               (int)i, tests[i][0], have, tests[i][1]);
    }
}

/* Go issue 57905. */
static void TestRelativeSymlinkToAbsolute(TestingT *t) {
    must_have_symlink(t);
    Str tmp = temp_dir(t);
    t_chdir(t, tmp);

    /* "link" in the working directory, pointing at an absolute path. On
     * macOS that path likely starts with a symlink itself, /var or /tmp. */
    symlink_or_fatal(t, tmp, S("link"));
    testing_t_logf_v(t, "os.Symlink(%q, \"link\")", tmp);

    Error e = BURROW_NO_ERROR;
    Str p = filepath_eval_symlinks(a, S("link"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "EvalSymlinks(\"link\"): %s", error_text(e));
    Str want = filepath_eval_symlinks(a, tmp, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "EvalSymlinks(%q): %s", tmp, error_text(e));
    if (!str_eq(p, want))
        testing_t_errorf_v(t, "EvalSymlinks(\"link\") = %q; want %q", p, want);
    testing_t_logf_v(t, "EvalSymlinks(\"link\") = %q", p);
}

static void TestDriveLetterInEvalSymlinks(TestingT *t) {
    if (!HOST_WIN)
        return;
    Error e = BURROW_NO_ERROR;
    Str wd = os_getwd(a, &e);
    if (wd.len < 3)
        testing_t_errorf_v(t, "Current directory path %q is too short", wd);
    Str lp = strings_to_lower(a, wd);
    Str up = strings_to_upper(a, wd);
    Str flp = filepath_eval_symlinks(a, lp, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "EvalSymlinks(%q) failed: %s", lp, error_text(e));
    Str fup = filepath_eval_symlinks(a, up, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "EvalSymlinks(%q) failed: %s", up, error_text(e));
    if (!str_eq(flp, fup))
        testing_t_errorf_v(t, "Results of EvalSymlinks do not match: %q and %q", flp,
                           fup);
}

static void TestIssue29372(TestingT *t) {
    Str tmp = temp_dir(t);
    Str path = join(tmp, S("file.txt"));
    touch(t, path);

    Str sep1 = S(SEP), sep2 = S(SEP SEP);
    Str tests[] = {
        cat(path, sep1),
        cat(path, sep2),
        cat(cat(path, sep1), S(".")),
        cat(cat(path, sep2), S(".")),
        cat(cat(path, sep1), S("..")),
        cat(cat(path, sep2), S("..")),
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error e = BURROW_NO_ERROR;
        (void)filepath_eval_symlinks(a, tests[i], &e);
        const SyscallErrno *n = errors_as(e, TYPE_SYSCALL_ERRNO);
        if (n == NULL || *n != SYSCALL_ENOTDIR || errors_unwrap(e).vt != NULL)
            testing_t_fatalf_v(t, "test#%d: want %q, got %q", (int)i,
                               syscall_errno_error(SYSCALL_ENOTDIR, a), error_text(e));
    }
}

/* Go issue 30520, part 1. */
static void TestEvalSymlinksAboveRoot(TestingT *t) {
    must_have_symlink(t);
    Str tmp = temp_dir(t);
    Error e = BURROW_NO_ERROR;
    Str eval_tmp = filepath_eval_symlinks(a, tmp, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "%s", error_text(e));

    e = os_mkdir(join(eval_tmp, S("a")), 0777);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "%s", error_text(e));
    symlink_or_fatal(t, join(eval_tmp, S("a")), join(eval_tmp, S("b")));
    touch(t, filepath_join_v(a, 3, eval_tmp, S("a"), S("file")));

    /* The number of ".." it takes to get to the root. */
    Str vol = filepath_volume_name(a, eval_tmp);
    Int c = strings_count((Str){eval_tmp.p + vol.len, eval_tmp.len - vol.len}, S(SEP));
    Str want_suffix = cat(cat(S("a"), S(SEP)), S("file"));

    /* Different numbers of "..". */
    for (Int k = c; k <= c + 2; k++) {
        Str dd = S("");
        for (Int i = 0; i < k; i++)
            dd = i == 0 ? S("..") : cat(cat(dd, S(SEP)), S(".."));
        Str rest = (Str){eval_tmp.p + vol.len + 1, eval_tmp.len - vol.len - 1};
        Str check = simple_join(
            simple_join(simple_join(simple_join(eval_tmp, dd), rest), S("b")),
            S("file"));
        Str resolved = filepath_eval_symlinks(a, check, &e);
        if (BURROW_FAILED(e))
            testing_t_errorf_v(t, "EvalSymlinks(%q) failed: %s", check, error_text(e));
        else if (!strings_has_suffix(resolved, want_suffix))
            testing_t_errorf_v(t, "EvalSymlinks(%q) = %q does not end with %q", check,
                               resolved, want_suffix);
        else
            testing_t_logf_v(t, "EvalSymlinks(%q) = %q", check, resolved);
    }
}

/* Go issue 30520, part 2. */
static void TestEvalSymlinksAboveRootChdir(TestingT *t) {
    must_have_symlink(t);
    t_chdir(t, temp_dir(t));

    Str subdir = join(S("a"), S("b"));
    mkdir_all(t, subdir);
    symlink_or_fatal(t, subdir, S("c"));
    touch(t, join(subdir, S("file")));

    subdir = filepath_join_v(a, 3, S("d"), S("e"), S("f"));
    mkdir_all(t, subdir);
    Error e = os_chdir(subdir);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "%s", error_text(e));

    Str check = filepath_join_v(a, 5, S(".."), S(".."), S(".."), S("c"), S("file"));
    Str want_suffix = filepath_join_v(a, 3, S("a"), S("b"), S("file"));
    Str resolved = filepath_eval_symlinks(a, check, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "EvalSymlinks(%q) failed: %s", check, error_text(e));
    else if (!strings_has_suffix(resolved, want_suffix))
        testing_t_errorf_v(t, "EvalSymlinks(%q) = %q does not end with %q", check,
                           resolved, want_suffix);
    else
        testing_t_logf_v(t, "EvalSymlinks(%q) = %q", check, resolved);
}

static void TestEvalSymlinksTooManyLinks(TestingT *t) {
    must_have_symlink(t);
    Str dir = join(temp_dir(t), S("dir"));
    symlink_or_fatal(t, dir, dir);
    Error e = BURROW_NO_ERROR;
    (void)filepath_eval_symlinks(a, dir, &e);
    if (BURROW_OK(e))
        testing_t_fatalf_v(t, "expected error, got nil");
}

/* --------------------------------------------------------------------- Abs */

/* Directories under the temporary directory. The tests run in the first. */
static const char *const abs_test_dirs[] = {"a", "a/b", "a/b/c"};

/* Paths under the temporary directory, run in abs_test_dirs[0]. $ is the
 * temporary directory. */
static const char *const abs_tests[] = {
    ".",
    "b",
    "b/",
    "../a",
    "../a/b",
    "../a/b/./c/../../.././a",
    "../a/b/./c/../../.././a/",
    "$",
    "$/.",
    "$/a/../a/b",
    "$/a/b/c/../../.././a",
    "$/a/b/c/../../.././a/",
};

static void check_abs(TestingT *t, Str path) {
    Error e = BURROW_NO_ERROR;
    OsFileInfo info = os_stat(a, path, &e);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "%s: %s", path, error_text(e));
        return;
    }
    Str abspath = filepath_abs(a, path, &e);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "Abs(%q) error: %s", path, error_text(e));
        return;
    }
    OsFileInfo absinfo = os_stat(a, abspath, &e);
    if (BURROW_FAILED(e) || !os_same_file(absinfo, info))
        testing_t_errorf_v(t, "Abs(%q)=%q, not the same file", path, abspath);
    if (!filepath_is_abs(abspath))
        testing_t_errorf_v(t, "Abs(%q)=%q, not an absolute path", path, abspath);
    if (filepath_is_abs(abspath) && !str_eq(abspath, filepath_clean(a, abspath)))
        testing_t_errorf_v(t, "Abs(%q)=%q, isn't clean", path, abspath);
}

static void TestAbs(TestingT *t) {
    Str root = temp_dir(t);
    t_chdir(t, root);
    for (size_t i = 0; i < 3; i++) {
        Error e = os_mkdir(str_from_cstr(abs_test_dirs[i]), 0777);
        if (BURROW_FAILED(e))
            testing_t_fatalf_v(t, "Mkdir failed: %s", error_text(e));
    }

    Slice tests = slice_nil(TYPE_STRING);
    for (size_t i = 0; i < sizeof abs_tests / sizeof abs_tests[0]; i++) {
        Str p = str_from_cstr(abs_tests[i]);
        tests = slice_append(a, tests, &p, 1);
    }
    if (HOST_WIN) {
        Str vol = filepath_volume_name(a, root);
        for (size_t i = 0; i < sizeof abs_tests / sizeof abs_tests[0]; i++) {
            Str p = str_from_cstr(abs_tests[i]);
            if (strings_index_byte(p, '$') >= 0)
                continue;
            p = cat(vol, p);
            tests = slice_append(a, tests, &p, 1);
        }
    }

    Error e = os_chdir(str_from_cstr(abs_test_dirs[0]));
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "chdir failed: %s", error_text(e));

    for (Int i = 0; i < tests.len; i++)
        check_abs(t, strings_replace_all(a, *(Str *)slice_at(tests, i), S("$"), root));
}

/* The empty path needs a case of its own on Windows. See Go issue 24441. It
 * is tested apart from the others because os_stat does not take it. */
static void TestAbsEmptyString(TestingT *t) {
    Str root = temp_dir(t);
    t_chdir(t, root);

    Error e = BURROW_NO_ERROR;
    OsFileInfo info = os_stat(a, root, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "%s: %s", root, error_text(e));
    Str abspath = filepath_abs(a, S(""), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "Abs(\"\") error: %s", error_text(e));
    OsFileInfo absinfo = os_stat(a, abspath, &e);
    if (BURROW_FAILED(e) || !os_same_file(absinfo, info))
        testing_t_errorf_v(t, "Abs(\"\")=%q, not the same file", abspath);
    if (!filepath_is_abs(abspath))
        testing_t_errorf_v(t, "Abs(\"\")=%q, not an absolute path", abspath);
    if (filepath_is_abs(abspath) && !str_eq(abspath, filepath_clean(a, abspath)))
        testing_t_errorf_v(t, "Abs(\"\")=%q, isn't clean", abspath);
}

/* --------------------------------------------------------------- the rest */

typedef struct Bug3486Env {
    TestingT *t;
    Str utf16, utf8;
    bool seen_utf16, seen_utf8;
} Bug3486Env;

static Error bug3486_fn(void *env, Str path, FsFileInfo info, Error err) {
    (void)info;
    Bug3486Env *b = (Bug3486Env *)env;
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(b->t, "%s", error_text(err));
    if (str_eq(path, b->utf16)) {
        b->seen_utf16 = true;
        return filepath_skip_dir;
    }
    if (str_eq(path, b->utf8)) {
        if (!b->seen_utf16)
            testing_t_fatalf_v(b->t, "filepath.Walk out of order - utf8 before utf16");
        b->seen_utf8 = true;
    }
    return BURROW_NO_ERROR;
}

/* Go issue 3486. Go walks GOROOT/src/unicode, and this walks a copy of the
 * part of it the test looks at. */
static void TestBug3486(TestingT *t) {
    Str root = join(temp_dir(t), S("unicode"));
    mkdir_all(t, join(root, S("utf16")));
    mkdir_all(t, join(root, S("utf8")));
    touch(t, join(root, S("utf16/utf16.go")));
    touch(t, join(root, S("utf8/utf8.go")));
    touch(t, join(root, S("tables.go")));

    Bug3486Env b = {t, join(root, S("utf16")), join(root, S("utf8")), false, false};
    Error err = filepath_walk(a, root, BURROW_FN(FilepathWalkFunc, bug3486_fn, &b));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!b.seen_utf8)
        testing_t_fatalf_v(t, "%q not seen", b.utf8);
}

typedef struct RelEnv {
    TestingT *t;
    Str root;
    Slice visited;
} RelEnv;

static Error visit_rel_fn(void *env, Str path, FsFileInfo info, Error err) {
    (void)info;
    RelEnv *r = (RelEnv *)env;
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(r->t, "%s", error_text(err));
    Str rel = filepath_rel(a, r->root, path, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(r->t, "%s", error_text(err));
    r->visited = slice_append(a, r->visited, &rel, 1);
    return BURROW_NO_ERROR;
}

static void TestWalkSymlink(TestingT *t) {
    must_have_symlink(t);
    Str tmp = temp_dir(t);
    t_chdir(t, tmp);
    symlink_or_fatal(t, tmp, S("link"));

    RelEnv r = {t, tmp, slice_nil(TYPE_STRING)};
    Error err = filepath_walk(a, tmp, BURROW_FN(FilepathWalkFunc, visit_rel_fn, &r));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    sort_strings(r.visited);
    static const char *const want[] = {".", "link", NULL};
    if (!equal(r.visited, str_list(want)))
        testing_t_errorf_v(t, "unexpected paths visited %s, want %s",
                           quote_list(r.visited), quote_list(str_list(want)));
}

typedef struct DirsEnv {
    TestingT *t;
    Str root;
    Slice saw;
} DirsEnv;

static Error saw_dirs_fn(void *env, Str path, FsDirEntry d, Error err) {
    DirsEnv *s = (DirsEnv *)env;
    if (BURROW_FAILED(err))
        return filepath_skip_dir;
    if (d.vt->is_dir(d.data)) {
        Str rel = filepath_rel(a, s->root, path, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(s->t, "%s", error_text(err));
        s->saw = slice_append(a, s->saw, &rel, 1);
    }
    return BURROW_NO_ERROR;
}

static void TestIssue51617(TestingT *t) {
    Str dir = temp_dir(t);
    Str subs[] = {S("a"), join(S("a"), S("bad")), join(S("a"), S("next"))};
    for (size_t i = 0; i < 3; i++) {
        Error e = os_mkdir(join(dir, subs[i]), 0755);
        if (BURROW_FAILED(e))
            testing_t_fatalf_v(t, "%s", error_text(e));
    }
    Str bad = filepath_join_v(a, 3, dir, S("a"), S("bad"));
    Error e = os_chmod(bad, 0);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "%s", error_text(e));
    DirsEnv s = {t, dir, slice_nil(TYPE_STRING)};
    Error err = filepath_walk_dir(a, dir, BURROW_FN(FsWalkDirFunc, saw_dirs_fn, &s));
    (void)os_chmod(bad, 0700); /* so the cleanup can remove it */
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Str want[] = {S("."), S("a"), join(S("a"), S("bad")), join(S("a"), S("next"))};
    Slice w = slice_from(want, 4, 4, TYPE_STRING);
    if (!equal(s.saw, w))
        testing_t_errorf_v(t, "got directories %s, want %s", quote_list(s.saw),
                           quote_list(w));
}

static void TestEscaping(TestingT *t) {
    Str dir = temp_dir(t);
    t_chdir(t, temp_dir(t));
    Str p = join(dir, S("x"));
    if (!filepath_is_local(p))
        return;
    Error e = BURROW_NO_ERROR;
    OsFile *f = os_create(a, p, &e);
    os_file_free(f);
    Slice ents = os_read_dir(a, dir, &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "%s", error_text(e));
    for (Int i = 0; i < ents.len; i++) {
        FsDirEntry *d = (FsDirEntry *)slice_at(ents, i);
        testing_t_fatalf_v(t, "found: %s", d->vt->name(d->data));
    }
}

/* Not from Go: what filepath_skip_dir and filepath_skip_all are, and that the
 * results of the functions here come from the allocator they are given. */
static void TestSkipAliases(TestingT *t) {
    CHECK(filepath_skip_dir.data == fs_skip_dir.data);
    CHECK(filepath_skip_all.data == fs_skip_all.data);
}

static void TestAbsIsJoinedOntoGetwd(TestingT *t) {
    if (HOST_WIN)
        testing_t_skip_v(t, "Abs is GetFullPathNameW on Windows");
    Str root = temp_dir(t);
    t_chdir(t, root);
    Error e = BURROW_NO_ERROR;
    Str wd = os_getwd(a, &e);
    Str got = filepath_abs(a, S("x/../y"), &e);
    if (BURROW_FAILED(e))
        testing_t_fatalf_v(t, "%s", error_text(e));
    CHECK(str_eq(got, join(wd, S("y"))));
    got = filepath_abs(a, S("/a/./b/.."), &e);
    CHECK(BURROW_OK(e) && str_eq(got, S("/a")));
}

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

#define TESTS(X)                                                                       \
    X(TestGlob)                                                                        \
    X(TestCVE202230632)                                                                \
    X(TestGlobError)                                                                   \
    X(TestGlobUNC)                                                                     \
    X(TestGlobSymlink)                                                                 \
    X(TestWindowsGlob)                                                                 \
    X(TestNonWindowsGlobEscape)                                                        \
    X(TestWalk)                                                                        \
    X(TestWalkDir)                                                                     \
    X(TestWalkSkipDirOnFile)                                                           \
    X(TestWalkSkipAllOnFile)                                                           \
    X(TestWalkSymlinkRoot)                                                             \
    X(TestEvalSymlinks)                                                                \
    X(TestEvalSymlinksIsNotExist)                                                      \
    X(TestIssue13582)                                                                  \
    X(TestRelativeSymlinkToAbsolute)                                                   \
    X(TestAbs)                                                                         \
    X(TestAbsEmptyString)                                                              \
    X(TestDriveLetterInEvalSymlinks)                                                   \
    X(TestBug3486)                                                                     \
    X(TestWalkSymlink)                                                                 \
    X(TestIssue29372)                                                                  \
    X(TestEvalSymlinksAboveRoot)                                                       \
    X(TestEvalSymlinksAboveRootChdir)                                                  \
    X(TestIssue51617)                                                                  \
    X(TestEscaping)                                                                    \
    X(TestEvalSymlinksTooManyLinks)                                                    \
    X(TestSkipAliases)                                                                 \
    X(TestAbsIsJoinedOntoGetwd)

static int filepath_os_main(TestingM *m) {
    setup();
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(filepath_os_main, TESTS)
