/* Derived from Go's src/go/build/build_test.go, read_test.go and
 * syslist_test.go.
 * Go source: go1.27.1.
 *
 * Go reads testdata from the package directory. Here the files are in
 * tests/go_build_test_gen.h, and a test that needs them writes them out to a
 * temporary directory and works from there, so the relative paths are the
 * ones Go uses. The tests that need the go command, or a GOROOT with its
 * sources, find them the way testenv does and are skipped without them.
 * vendor_test.go and deps_test.go check the Go tree, not go/build, and are
 * left out.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/go/build_internal.h"
#include "burrow/burrow.h"
#include "burrow/go/build.h"
#include "burrow/mem/arena.h"
#include "burrow/os/exec.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "go_build_test_gen.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static Arena ar;
static Alloc *a;

static void gbt_free_arena(void *env) {
    arena_free((Arena *)env);
}

/* A fresh arena for the test, freed after its other cleanups. */
static void gbt_begin(TestingT *t) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    testing_t_cleanup(t, BURROW_FN(Func, gbt_free_arena, &ar));
}

static Str gbt_cstr(const char *s) {
    return str_from_cstr(s);
}

static Slice gbt_strs(int n, const char *const *list) {
    Slice s = slice_nil(TYPE_STRING);
    for (int i = 0; i < n; i++) {
        Str v = gbt_cstr(list[i]);
        s = slice_append(a, s, &v, 1);
    }
    return s;
}

static Str gbt_at(Slice s, Int i) {
    return BURROW_AT(Str, s, i);
}

static bool gbt_strs_equal(Slice got, int n, const char *const *want) {
    if (got.len != n)
        return false;
    for (int i = 0; i < n; i++) {
        if (!str_eq(gbt_at(got, i), gbt_cstr(want[i])))
            return false;
    }
    return true;
}

/* got as Go prints a []string. */
static Str gbt_show(Slice got) {
    return fmt_sprintf_v(a, "[%s]", strings_join(a, got, S(" ")));
}

static Map *gbt_tag_map(void) {
    return map_make(a, TYPE_STRING, TYPE_BOOL, 0);
}

/* Whether m, a map of tags to true, holds just the n tags of want. */
static bool gbt_tags_equal(Map *m, int n, const char *const *want) {
    if (map_len(m) != n)
        return false;
    for (int i = 0; i < n; i++) {
        Str k = gbt_cstr(want[i]);
        bool *v = (bool *)map_get(m, &k);
        if (v == NULL || !*v)
            return false;
    }
    return true;
}

static Str gbt_show_tags(Map *m) {
    Slice keys = slice_nil(TYPE_STRING);
    MapIter it = map_iter(m);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v))
        keys = slice_append(a, keys, k, 1);
    sort_strings(keys);
    return gbt_show(keys);
}

static Str gbt_err(Error err) {
    return BURROW_FAILED(err) ? error_text(err) : S("<nil>");
}

static void gbt_remove_tree(void *env) {
    (void)os_remove_all(*(Str *)env);
}

/* t.TempDir. */
static Str gbt_temp_dir(TestingT *t, bool *ok) {
    Error e = BURROW_NO_ERROR;
    Str *d = BURROW_NEW(a, Str);
    *ok = false;
    if (d == NULL)
        return BURROW_STR_EMPTY;
    *d = os_mkdir_temp(a, BURROW_STR_EMPTY, S("burrow-go-build-test-*"), &e);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "MkdirTemp: %s", error_text(e));
        return BURROW_STR_EMPTY;
    }
    testing_t_cleanup(t, BURROW_FN(Func, gbt_remove_tree, d));
    *ok = true;
    return *d;
}

static void gbt_chdir_back(void *env) {
    (void)os_chdir(*(Str *)env);
}

/* The files of testdata written out under a new temporary directory, which
 * becomes the working directory until the test ends. */
static bool gbt_testdata(TestingT *t) {
    bool ok = false;
    Str dir = gbt_temp_dir(t, &ok);
    if (!ok)
        return false;
    for (Int i = 0; i < NELEM(gbt_files); i++) {
        Str name = filepath_from_slash(a, gbt_cstr(gbt_files[i].name));
        Str path = filepath_join_v(a, 2, dir, name);
        Error e = os_mkdir_all(filepath_dir(a, path), 0777);
        if (BURROW_OK(e)) {
            Slice data = slice_from((void *)(uintptr_t)gbt_files[i].data,
                                    gbt_files[i].len, gbt_files[i].len, TYPE_BYTE);
            e = os_write_file(path, data, 0666);
        }
        if (BURROW_FAILED(e)) {
            testing_t_fatalf_v(t, "writing %s: %s", path, error_text(e));
            return false;
        }
    }
    Error e = BURROW_NO_ERROR;
    Str *wd = BURROW_NEW(a, Str);
    if (wd == NULL)
        return false;
    *wd = os_getwd(a, &e);
    if (BURROW_OK(e))
        e = os_chdir(dir);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Chdir %s: %s", dir, error_text(e));
        return false;
    }
    testing_t_cleanup(t, BURROW_FN(Func, gbt_chdir_back, wd));
    return true;
}

typedef struct GbtEnv {
    Str key;
    Str old;
    bool had;
} GbtEnv;

static void gbt_restore_env(void *env) {
    GbtEnv *e = (GbtEnv *)env;
    if (e->had)
        (void)os_setenv(e->key, e->old);
    else
        (void)os_unsetenv(e->key);
}

/* t.Setenv. */
static void gbt_setenv(TestingT *t, Str key, Str value) {
    GbtEnv *e = BURROW_NEW(a, GbtEnv);
    if (e == NULL)
        return;
    e->key = key;
    e->old = os_lookup_env(a, key, &e->had);
    (void)os_setenv(key, value);
    testing_t_cleanup(t, BURROW_FN(Func, gbt_restore_env, e));
}

/* What testenv.GOROOT finds: $GOROOT, or else what go env says, or "" when
 * there is no go command to ask. */
static Str gbt_find_goroot(void) {
    Str goroot = os_getenv(a, S("GOROOT"));
    if (goroot.len > 0)
        return goroot;
    Error e = BURROW_NO_ERROR;
    Str go = exec_look_path(a, S("go"), &e);
    if (BURROW_FAILED(e))
        return BURROW_STR_EMPTY;
    ExecCmd *cmd = exec_command_v(a, go, 2, S("env"), S("GOROOT"));
    if (cmd == NULL)
        return BURROW_STR_EMPTY;
    Slice out = exec_cmd_output(cmd, a, &e);
    if (BURROW_OK(e) && out.len > 0)
        goroot = str_clone(
            a, strings_trim_space(str_from_bytes((const Byte *)out.p, out.len)));
    exec_cmd_free(cmd);
    return goroot;
}

/* testenv.GOROOT(t): the Go root, with the test skipped unless there is one
 * with its sources. */
static Str gbt_goroot(TestingT *t) {
    Str goroot = gbt_find_goroot();
    if (goroot.len == 0) {
        testing_t_skip_v(t, "no GOROOT and no go command to ask for one");
        return goroot;
    }
    Error e = BURROW_NO_ERROR;
    (void)os_stat(a, filepath_join_v(a, 3, goroot, S("src"), S("go")), &e);
    if (BURROW_FAILED(e)) {
        testing_t_skipf_v(t, "no Go sources in %s", goroot);
        return BURROW_STR_EMPTY;
    }
    return goroot;
}

/* testenv.MustHaveGoBuild: the Go root, with the test skipped unless its go
 * command is there to run. */
static Str gbt_must_have_go_build(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    Str goroot = gbt_goroot(t);
    if (goroot.len == 0)
        return goroot;
    Str go = filepath_join_v(a, 3, goroot, S("bin"), S("go"));
    Error e = BURROW_NO_ERROR;
    (void)exec_look_path(a, go, &e);
    if (BURROW_FAILED(e)) {
        testing_t_skipf_v(t, "no go command at %s", go);
        return BURROW_STR_EMPTY;
    }
    return goroot;
}

/* Default with its GOROOT set as TestMain sets it. */
static BuildContext gbt_default(void) {
    BuildContext c = build_default(a);
    c.goroot = gbt_find_goroot();
    return c;
}

/* A Context with nothing set. */
static BuildContext gbt_zero_context(void) {
    BuildContext c;
    memset(&c, 0, sizeof c);
    return c;
}

static Str gbt_cat(Str x, Str y) {
    StringsBuilder b = STRINGS_BUILDER(a);
    (void)strings_builder_write_string(&b, x, NULL);
    (void)strings_builder_write_string(&b, y, NULL);
    return strings_builder_string(&b);
}

/* ------------------------------------------------------------- build_test */

typedef struct GbtMatch {
    TestingT *t;
    BuildContext *ctxt;
    const char *what;
} GbtMatch;

/* match and nomatch of TestMatch, on runtime.GOOS,runtime.GOARCH and the
 * rest of tag. */
static void gbt_match(GbtMatch *mc, bool want_match, const char *rest,
                      const char *extra) {
    Str tag = fmt_sprintf_v(a, "%s,%s%s", gbt_cstr(burrow_os_name()),
                            gbt_cstr(burrow_arch_name()), gbt_cstr(rest));
    const char *want[] = {burrow_os_name(), burrow_arch_name(), extra};
    Map *m = gbt_tag_map();
    if (burrow__build_match_auto(mc->ctxt, a, tag, m) != want_match) {
        if (want_match)
            testing_t_errorf_v(mc->t, "%s context should match %s, does not",
                               gbt_cstr(mc->what), tag);
        else
            testing_t_errorf_v(mc->t, "%s context should NOT match %s, does",
                               gbt_cstr(mc->what), tag);
    }
    int n = extra != NULL ? 3 : 2;
    if (!gbt_tags_equal(m, n, want))
        testing_t_errorf_v(mc->t, "%s tags = %s, want %s", tag, gbt_show_tags(m),
                           gbt_show(gbt_strs(n, want)));
}

static void TestMatch(TestingT *t) {
    gbt_begin(t);
    BuildContext ctxt = gbt_default();
    GbtMatch mc = {t, &ctxt, "default"};

    gbt_match(&mc, true, "", NULL);
    gbt_match(&mc, true, ",!foo", "foo");
    gbt_match(&mc, false, ",foo", "foo");

    mc.what = "modified";
    const char *foo[] = {"foo"};
    ctxt.build_tags = gbt_strs(1, foo);
    gbt_match(&mc, true, "", NULL);
    gbt_match(&mc, true, ",foo", "foo");
    gbt_match(&mc, false, ",!foo", "foo");
    gbt_match(&mc, true, ",!bar", "bar");
    gbt_match(&mc, false, ",bar", "bar");
}

static void TestDotSlashImport(TestingT *t) {
    gbt_begin(t);
    if (!gbt_testdata(t))
        return;
    Error err = BURROW_NO_ERROR;
    BuildPackage *p = build_import_dir(a, S("testdata/other"), 0, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    if (p->imports.len != 1 || !str_eq(gbt_at(p->imports, 0), S("./file"))) {
        testing_t_fatalf_v(t, "testdata/other: Imports=%s, want [./file]",
                           gbt_show(p->imports));
        return;
    }

    BuildPackage *p1 = build_import(a, S("./file"), S("testdata/other"), 0, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    if (!str_eq(p1->name, S("file"))) {
        testing_t_fatalf_v(t, "./file: Name=%q, want %q", p1->name, S("file"));
        return;
    }
    Str dir = filepath_clean(a, S("testdata/other/file")); /* \ on Windows */
    if (!str_eq(p1->dir, dir))
        testing_t_fatalf_v(t, "./file: Dir=%q, want %q", p1->name, dir);
}

static void TestEmptyImport(TestingT *t) {
    gbt_begin(t);
    Str goroot = gbt_goroot(t);
    if (goroot.len == 0)
        return;
    Error err = BURROW_NO_ERROR;
    BuildPackage *p = build_import(a, BURROW_STR_EMPTY, goroot, BUILD_FIND_ONLY, &err);
    if (BURROW_OK(err)) {
        testing_t_fatal_v(t, "Import(\"\") returned nil error.");
        return;
    }
    if (p == NULL) {
        testing_t_fatal_v(t, "Import(\"\") returned nil package.");
        return;
    }
    if (p->import_path.len != 0)
        testing_t_fatalf_v(t, "ImportPath=%q, want %q.", p->import_path, S(""));
}

static void TestEmptyFolderImport(TestingT *t) {
    gbt_begin(t);
    if (!gbt_testdata(t))
        return;
    Error err = BURROW_NO_ERROR;
    (void)build_import(a, S("."), S("testdata/empty"), 0, &err);
    if (errors_as(err, TYPE_BUILD_NO_GO_ERROR) == NULL)
        testing_t_fatal_v(t, "Import(\"testdata/empty\") did not return NoGoError.");
}

static void TestMultiplePackageImport(TestingT *t) {
    gbt_begin(t);
    if (!gbt_testdata(t))
        return;
    Error err = BURROW_NO_ERROR;
    BuildPackage *pkg = build_import(a, S("."), S("testdata/multi"), 0, &err);

    const BuildMultiplePackageError *mpe =
        errors_as(err, TYPE_BUILD_MULTIPLE_PACKAGE_ERROR);
    if (mpe == NULL) {
        testing_t_fatal_v(
            t, "Import(\"testdata/multi\") did not return MultiplePackageError.");
        return;
    }
    const char *packages[] = {"main", "test_package"};
    const char *files[] = {"file.go", "file_appengine.go"};
    Str dir = filepath_from_slash(a, S("testdata/multi"));
    if (!str_eq(mpe->dir, dir) || !gbt_strs_equal(mpe->packages, 2, packages) ||
        !gbt_strs_equal(mpe->files, 2, files))
        testing_t_errorf_v(t,
                           "err = {Dir:%q Packages:%s Files:%s}; want {Dir:%q "
                           "Packages:[main test_package] Files:[file.go "
                           "file_appengine.go]}",
                           mpe->dir, gbt_show(mpe->packages), gbt_show(mpe->files),
                           dir);

    /* TODO(#45999): Since the name is ambiguous, pkg.Name should be left
     * empty. */
    if (!str_eq(pkg->name, S("main")))
        testing_t_errorf_v(t, "pkg.Name = %q; want %q", pkg->name, S("main"));

    if (!gbt_strs_equal(pkg->go_files, 2, files))
        testing_t_errorf_v(t, "pkg.GoFiles = %s; want %s", gbt_show(pkg->go_files),
                           gbt_show(gbt_strs(2, files)));

    if (!gbt_strs_equal(pkg->invalid_go_files, 1, files + 1))
        testing_t_errorf_v(t, "pkg.InvalidGoFiles = %s; want [file_appengine.go]",
                           gbt_show(pkg->invalid_go_files));
}

/* Go imports its own directory, GOROOT/src/go/build, which here is the one
 * in the Go root the tests find. */
static void TestLocalDirectory(TestingT *t) {
    gbt_begin(t);
    if (strcmp(burrow_os_name(), "ios") == 0) {
        testing_t_skipf_v(t, "skipping on %s/%s, no valid GOROOT",
                          gbt_cstr(burrow_os_name()), gbt_cstr(burrow_arch_name()));
        return;
    }
    Str goroot = gbt_goroot(t);
    if (goroot.len == 0)
        return;
    BuildContext ctxt = gbt_default();
    ctxt.goroot = goroot;
    Str dir = filepath_join_v(a, 4, goroot, S("src"), S("go"), S("build"));

    Error err = BURROW_NO_ERROR;
    BuildPackage *p = build_context_import_dir(&ctxt, a, dir, 0, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    if (!str_eq(p->import_path, S("go/build")))
        testing_t_fatalf_v(t, "ImportPath=%q, want %q", p->import_path, S("go/build"));
}

static void gbt_should_build(void *env, TestingT *t) {
    const GbtShouldBuild *tt = (const GbtShouldBuild *)env;
    BuildContext ctx = gbt_zero_context();
    const char *yes[] = {"yes"};
    ctx.build_tags = gbt_strs(1, yes);
    Map *tags = gbt_tag_map();
    bool binary_only = false;
    Error err = BURROW_NO_ERROR;
    bool should_build = burrow__build_should_build(&ctx, a, gbt_cstr(tt->content), tags,
                                                   &binary_only, &err);
    if (should_build != tt->should_build || binary_only != tt->binary_only ||
        !gbt_tags_equal(tags, tt->ntags, tt->tags) || BURROW_FAILED(err))
        testing_t_errorf_v(t,
                           "mismatch:\n"
                           "have shouldBuild=%v, binaryOnly=%v, tags=%s, err=%s\n"
                           "want shouldBuild=%v, binaryOnly=%v, tags=%s, err=<nil>",
                           should_build, binary_only, gbt_show_tags(tags), gbt_err(err),
                           tt->should_build, tt->binary_only,
                           gbt_show(gbt_strs(tt->ntags, tt->tags)));
}

static void TestShouldBuild(TestingT *t) {
    gbt_begin(t);
    for (Int i = 0; i < NELEM(gbt_should_build_tests); i++) {
        const GbtShouldBuild *tt = &gbt_should_build_tests[i];
        testing_t_run(t, gbt_cstr(tt->name),
                      BURROW_FN(TestingTFunc, gbt_should_build, (void *)(uintptr_t)tt));
    }
}

static void TestGoodOSArchFile(TestingT *t) {
    gbt_begin(t);
    BuildContext ctx = gbt_zero_context();
    const char *linux_tag[] = {"linux"};
    ctx.build_tags = gbt_strs(1, linux_tag);
    ctx.goos = S("darwin");
    Map *m = gbt_tag_map();
    if (!burrow__build_good_os_arch_file(&ctx, a, S("hello_linux.go"), m))
        testing_t_errorf_v(t, "goodOSArchFile(hello_linux.go) = false, want true");
    if (!gbt_tags_equal(m, 1, linux_tag))
        testing_t_errorf_v(t, "goodOSArchFile(hello_linux.go) tags = %s, want [linux]",
                           gbt_show_tags(m));
}

typedef struct GbtMatchFileEnv {
    TestingT *t;
    const GbtMatchFile *tt;
    IoNopCloser rc;
} GbtMatchFileEnv;

static IoReadCloser gbt_match_file_open(void *env, Alloc *al, Str path, Error *err) {
    GbtMatchFileEnv *e = (GbtMatchFileEnv *)env;
    Str want = gbt_cat(S("x+"), gbt_cstr(e->tt->name));
    if (!str_eq(path, want))
        testing_t_fatalf_v(e->t, "OpenFile asked for %q, expected %q", path, want);
    *err = BURROW_NO_ERROR;
    StringsReader *r = strings_new_reader(al, gbt_cstr(e->tt->data));
    if (r == NULL)
        testing_t_fatal_v(e->t, "out of memory");
    e->rc = io_nop_closer(strings_reader_as_io_reader(r));
    return io_nop_closer_as_io_read_closer(&e->rc);
}

static Str gbt_match_file_join(void *env, Alloc *al, Slice elem) {
    (void)env;
    return strings_join(al, elem, S("+"));
}

static void TestMatchFile(TestingT *t) {
    gbt_begin(t);
    for (Int i = 0; i < NELEM(gbt_match_file_tests); i++) {
        const GbtMatchFile *tt = &gbt_match_file_tests[i];
        GbtMatchFileEnv env;
        memset(&env, 0, sizeof env);
        env.t = t;
        env.tt = tt;
        BuildContext ctxt = gbt_zero_context();
        ctxt.goarch = S("arm");
        ctxt.goos = gbt_cstr(tt->ctxt);
        ctxt.open_file = BURROW_FN(BuildOpenFileFunc, gbt_match_file_open, &env);
        ctxt.join_path = BURROW_FN(BuildJoinPathFunc, gbt_match_file_join, NULL);
        Error err = BURROW_NO_ERROR;
        bool match =
            build_context_match_file(&ctxt, a, S("x"), gbt_cstr(tt->name), &err);
        if (match != tt->match || BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "MatchFile(%q) = %v, %s, want %v, nil",
                               gbt_cstr(tt->name), match, gbt_err(err), tt->match);
            return;
        }
    }
}

static void TestImportCmd(TestingT *t) {
    gbt_begin(t);
    if (strcmp(burrow_os_name(), "ios") == 0) {
        testing_t_skipf_v(t, "skipping on %s/%s, no valid GOROOT",
                          gbt_cstr(burrow_os_name()), gbt_cstr(burrow_arch_name()));
        return;
    }
    Str goroot = gbt_goroot(t);
    if (goroot.len == 0)
        return;
    BuildContext ctxt = gbt_default();
    ctxt.goroot = goroot;

    Error err = BURROW_NO_ERROR;
    BuildPackage *p = build_context_import(&ctxt, a, S("cmd/internal/objfile"),
                                           BURROW_STR_EMPTY, 0, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    Str dir = filepath_to_slash(a, p->dir);
    if (!strings_has_suffix(dir, S("src/cmd/internal/objfile")))
        testing_t_fatalf_v(t, "Import cmd/internal/objfile returned Dir=%q, want %q",
                           dir, S(".../src/cmd/internal/objfile"));
}

static void TestExpandSrcDir(TestingT *t) {
    gbt_begin(t);
    Str expand_src_dir_path = filepath_join_v(
        a, 3, filepath_from_slash(a, S("/projects")), S("src"), S("add"));
    for (Int i = 0; i < NELEM(gbt_expand_src_dir_tests); i++) {
        Str input = gbt_cstr(gbt_expand_src_dir_tests[i].input);
        Str expected = gbt_cstr(gbt_expand_src_dir_tests[i].expected);
        bool ok = false;
        Str output = burrow__build_expand_src_dir(a, input, expand_src_dir_path, &ok);
        if (!str_eq(output, expected))
            testing_t_errorf_v(t, "%q expands to %q with SRCDIR=%q when %q is expected",
                               input, output, expand_src_dir_path, expected);
        else
            testing_t_logf_v(t, "%q expands to %q with SRCDIR=%q", input, output,
                             expand_src_dir_path);
    }
}

static void TestShellSafety(TestingT *t) {
    gbt_begin(t);
    for (Int i = 0; i < NELEM(gbt_shell_safety_tests); i++) {
        const GbtShellSafety *test = &gbt_shell_safety_tests[i];
        Str input = gbt_cstr(test->input);
        Str srcdir = gbt_cstr(test->srcdir);
        Str expected = gbt_cstr(test->expected);
        bool ok = false;
        Str output = burrow__build_expand_src_dir(a, input, srcdir, &ok);
        if (ok != test->result)
            testing_t_errorf_v(
                t, "Expected %t while %q expands to %q with SRCDIR=%q; got %t",
                test->result, input, output, srcdir, ok);
        if (!str_eq(output, expected))
            testing_t_errorf_v(t, "Expected %q while %q expands with SRCDIR=%q; got %q",
                               expected, input, srcdir, output);
    }
}

typedef struct GbtNotExist {
    BuildContext *ctxt;
    const char *go111module;
} GbtNotExist;

static void gbt_import_dir_not_exist(void *env, TestingT *t) {
    GbtNotExist *ne = (GbtNotExist *)env;
    static const struct {
        const char *label;
        const char *path;
        bool local;
        BuildImportMode mode;
    } tests[] = {
        {"Import(full, 0)", "go/build/doesnotexist", false, 0},
        {"Import(local, 0)", "./doesnotexist", true, 0},
        {"Import(full, FindOnly)", "go/build/doesnotexist", false, BUILD_FIND_ONLY},
        {"Import(local, FindOnly)", "./doesnotexist", true, BUILD_FIND_ONLY},
    };
    (void)os_setenv(S("GO111MODULE"), gbt_cstr(ne->go111module));

    for (Int i = 0; i < NELEM(tests); i++) {
        Str label = gbt_cstr(tests[i].label);
        Str src_dir = BURROW_STR_EMPTY;
        if (tests[i].local)
            src_dir = filepath_join_v(a, 2, ne->ctxt->goroot, S("src/go/build"));
        Error err = BURROW_NO_ERROR;
        BuildPackage *p = build_context_import(ne->ctxt, a, gbt_cstr(tests[i].path),
                                               src_dir, tests[i].mode, &err);

        bool err_ok = BURROW_FAILED(err) &&
                      strings_has_prefix(error_text(err), S("cannot find package"));
        Str want_err = S("\"cannot find package\" error");
        if (src_dir.len == 0) {
            if (BURROW_FAILED(err) &&
                strings_contains(error_text(err), S("is not in std")))
                err_ok = true;
            want_err = S("\"cannot find package\" or \"is not in std\" error");
        }
        if (!err_ok)
            testing_t_errorf_v(t, "%s got error: %q, want %s", label, gbt_err(err),
                               want_err);
        /* If an error occurs, build.Import is documented to return a non-nil
         * *Package containing partial information. */
        if (p == NULL) {
            testing_t_fatalf_v(t, "%s got nil p, want non-nil *Package", label);
            return;
        }
        /* Verify partial information in p. */
        if (!str_eq(p->import_path, S("go/build/doesnotexist")))
            testing_t_errorf_v(
                t, "%s got p.ImportPath: %q, want \"go/build/doesnotexist\"", label,
                p->import_path);
    }
}

/* Want to get a "cannot find package" error when directory for package does
 * not exist. There should be valid partial information in the returned
 * non-nil *Package. */
static void TestImportDirNotExist(TestingT *t) {
    gbt_begin(t);
    Str goroot = gbt_must_have_go_build(t); /* Need 'go list' internally. */
    if (goroot.len == 0)
        return;
    BuildContext ctxt = gbt_default();
    ctxt.goroot = goroot;

    bool ok = false;
    Str empty_dir = gbt_temp_dir(t, &ok);
    if (!ok)
        return;
    ctxt.gopath = empty_dir;
    ctxt.dir = empty_dir;

    /* The subtests set GO111MODULE, and this puts it back after. */
    gbt_setenv(t, S("GO111MODULE"), os_getenv(a, S("GO111MODULE")));

    const char *values[] = {"off", "on"};
    for (int i = 0; i < 2; i++) {
        GbtNotExist ne = {&ctxt, values[i]};
        testing_t_run(t, gbt_cat(S("GO111MODULE="), gbt_cstr(values[i])),
                      BURROW_FN(TestingTFunc, gbt_import_dir_not_exist, &ne));
    }
}

/* Default with GOPATH at testdata/withvendor of the working directory, and
 * that src/a/b of it. */
static BuildContext gbt_withvendor(TestingT *t, Str *src_dir, bool *ok) {
    BuildContext ctxt = gbt_default();
    *ok = false;
    Error err = BURROW_NO_ERROR;
    Str wd = os_getwd(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return ctxt;
    }
    ctxt.gopath = filepath_join_v(a, 2, wd, S("testdata/withvendor"));
    *src_dir = filepath_join_v(a, 2, ctxt.gopath, S("src/a/b"));
    *ok = true;
    return ctxt;
}

static void TestImportVendor(TestingT *t) {
    gbt_begin(t);
    if (!gbt_testdata(t))
        return;
    gbt_setenv(t, S("GO111MODULE"), S("off"));

    Str src_dir = BURROW_STR_EMPTY;
    bool ok = false;
    BuildContext ctxt = gbt_withvendor(t, &src_dir, &ok);
    if (!ok)
        return;
    Error err = BURROW_NO_ERROR;
    BuildPackage *p = build_context_import(&ctxt, a, S("c/d"), src_dir, 0, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(
            t, "cannot find vendored c/d from testdata src/a/b directory: %s",
            error_text(err));
        return;
    }
    Str want = S("a/vendor/c/d");
    if (!str_eq(p->import_path, want))
        testing_t_fatalf_v(t, "Import succeeded but found %q, want %q", p->import_path,
                           want);
}

static void TestImportVendorFailure(TestingT *t) {
    gbt_begin(t);
    if (!gbt_testdata(t))
        return;
    gbt_setenv(t, S("GO111MODULE"), S("off"));

    Str src_dir = BURROW_STR_EMPTY;
    bool ok = false;
    BuildContext ctxt = gbt_withvendor(t, &src_dir, &ok);
    if (!ok)
        return;
    Error err = BURROW_NO_ERROR;
    BuildPackage *p = build_context_import(&ctxt, a, S("x.com/y/z"), src_dir, 0, &err);
    if (BURROW_OK(err)) {
        testing_t_fatalf_v(t, "found made-up package x.com/y/z in %s", p->dir);
        return;
    }

    Str e = error_text(err);
    if (!strings_contains(e, S(" (vendor tree)")))
        testing_t_fatalf_v(
            t,
            "error on failed import does not mention GOROOT/src/vendor directory:\n%s",
            e);
}

static void TestImportVendorParentFailure(TestingT *t) {
    gbt_begin(t);
    if (!gbt_testdata(t))
        return;
    gbt_setenv(t, S("GO111MODULE"), S("off"));

    Str src_dir = BURROW_STR_EMPTY;
    bool ok = false;
    BuildContext ctxt = gbt_withvendor(t, &src_dir, &ok);
    if (!ok)
        return;
    /* This import should fail because the vendor/c directory has no source
     * code. */
    Error err = BURROW_NO_ERROR;
    BuildPackage *p = build_context_import(&ctxt, a, S("c"), src_dir, 0, &err);
    if (BURROW_OK(err)) {
        testing_t_fatalf_v(t, "found empty parent in %s", p->dir);
        return;
    }
    if (p != NULL && p->dir.len != 0) {
        testing_t_fatalf_v(t, "decided to use %s", p->dir);
        return;
    }
    Str e = error_text(err);
    if (!strings_contains(e, S(" (vendor tree)")))
        testing_t_fatalf_v(
            t,
            "error on failed import does not mention GOROOT/src/vendor directory:\n%s",
            e);
}

/* Check that a package is loaded in module mode if GO111MODULE=on, even when
 * no go.mod file is present. It should fail to resolve packages outside std.
 * Verifies golang.org/issue/34669. */
static void TestImportPackageOutsideModule(TestingT *t) {
    gbt_begin(t);
    Str goroot = gbt_must_have_go_build(t);
    if (goroot.len == 0)
        return;

    /* Disable module fetching for this test so that 'go list' fails quickly
     * without trying to find the latest version of a module. */
    gbt_setenv(t, S("GOPROXY"), S("off"));

    /* Create a GOPATH in a temporary directory. We don't use testdata because
     * it's in GOROOT, which interferes with the module heuristic. */
    bool ok = false;
    Str gopath = gbt_temp_dir(t, &ok);
    if (!ok)
        return;
    Error err =
        os_mkdir_all(filepath_join_v(a, 2, gopath, S("src/example.com/p")), 0777);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    err = os_write_file(filepath_join_v(a, 2, gopath, S("src/example.com/p/p.go")),
                        slice_from((void *)(uintptr_t)"package p", 9, 9, TYPE_BYTE),
                        0666);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }

    gbt_setenv(t, S("GO111MODULE"), S("on"));
    gbt_setenv(t, S("GOPATH"), gopath);
    BuildContext ctxt = gbt_default();
    ctxt.goroot = goroot;
    ctxt.gopath = gopath;
    ctxt.dir = filepath_join_v(a, 2, gopath, S("src/example.com/p"));

    Str want = S("go.mod file not found in current directory or any parent directory");
    (void)build_context_import(&ctxt, a, S("example.com/p"), gopath, BUILD_FIND_ONLY,
                               &err);
    if (BURROW_OK(err))
        testing_t_fatal_v(t, "importing package when no go.mod is present succeeded "
                             "unexpectedly");
    else if (!strings_contains(error_text(err), want))
        testing_t_fatalf_v(
            t,
            "error when importing package when no go.mod is present: got "
            "%q; want %q",
            error_text(err), want);
    else
        testing_t_logf_v(t, "ctxt.Import(\"example.com/p\", _, FindOnly): %s",
                         error_text(err));
}

/* TestIssue23594 prevents go/build from regressing and populating Package.Doc
 * from comments in test files. */
static void TestIssue23594(TestingT *t) {
    gbt_begin(t);
    if (!gbt_testdata(t))
        return;
    /* Package testdata/doc contains regular and external test files with
     * comments attached to their package declarations. The names of the files
     * ensure that we see the comments from the test files first. */
    Error err = BURROW_NO_ERROR;
    BuildPackage *p = build_import_dir(a, S("testdata/doc"), 0, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "could not import testdata: %s", error_text(err));
        return;
    }

    if (!str_eq(p->doc, S("Correct")))
        testing_t_fatalf_v(t, "incorrectly set .Doc to %q", p->doc);
}

/* TestIssue56509 tests that go/build does not add non-go files to
 * InvalidGoFiles when they have unparsable comments. */
static void TestIssue56509(TestingT *t) {
    gbt_begin(t);
    if (!gbt_testdata(t))
        return;
    /* The directory testdata/bads contains a .s file that has an unparsable
     * comment. (go/build parses initial comments in non-go files looking for
     * //go:build or //+go build comments). */
    Error err = BURROW_NO_ERROR;
    BuildPackage *p = build_import_dir(a, S("testdata/bads"), 0, &err);
    if (BURROW_OK(err)) {
        testing_t_fatalf_v(t, "could not import testdata/bads: %s", gbt_err(err));
        return;
    }

    if (p->invalid_go_files.len != 0)
        testing_t_fatal_v(t, "incorrectly added non-go file to InvalidGoFiles");
}

/* TestMissingImportErrorRepetition checks that when an unknown package is
 * imported, the package path is only shown once in the error. Verifies
 * golang.org/issue/34752. */
static void TestMissingImportErrorRepetition(TestingT *t) {
    gbt_begin(t);
    Str goroot = gbt_must_have_go_build(t); /* need 'go list' internally */
    if (goroot.len == 0)
        return;
    bool ok = false;
    Str tmp = gbt_temp_dir(t, &ok);
    if (!ok)
        return;
    Error err =
        os_write_file(filepath_join_v(a, 2, tmp, S("go.mod")),
                      slice_from((void *)(uintptr_t)"module m", 8, 8, TYPE_BYTE), 0666);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    gbt_setenv(t, S("GO111MODULE"), S("on"));
    gbt_setenv(t, S("GOPROXY"), S("off"));
    gbt_setenv(t, S("GONOPROXY"), S("none"));

    BuildContext ctxt = gbt_default();
    ctxt.goroot = goroot;
    ctxt.dir = tmp;

    Str pkg_path = S("example.com/hello");
    (void)build_context_import(&ctxt, a, pkg_path, tmp, BUILD_FIND_ONLY, &err);
    if (BURROW_OK(err)) {
        testing_t_fatal_v(t, "unexpected success");
        return;
    }

    /* Don't count the package path with a URL like https://...?go-get=1. See
     * golang.org/issue/35986. */
    Str err_str = strings_replace_all(
        a, error_text(err), S("://example.com/hello?go-get=1"), S("://...?go-get=1"));

    /* Also don't count instances in suggested "go get" or similar commands
     * (see https://golang.org/issue/41576). The suggested command typically
     * follows a semicolon. */
    bool found = false;
    Str after = BURROW_STR_EMPTY;
    err_str = strings_cut(err_str, S(";"), &after, &found);

    Int n = strings_count(err_str, pkg_path);
    if (n != 1)
        testing_t_fatalf_v(t,
                           "package path %q appears in error %d times; should appear "
                           "once\nerror: %s",
                           pkg_path, n, error_text(err));
}

/* TestCgoImportsIgnored checks that imports in cgo files are not included in
 * the imports list when cgo is disabled. Verifies golang.org/issue/35946. */
static void TestCgoImportsIgnored(TestingT *t) {
    gbt_begin(t);
    if (!gbt_testdata(t))
        return;
    BuildContext ctxt = gbt_default();
    ctxt.cgo_enabled = false;
    Error err = BURROW_NO_ERROR;
    BuildPackage *p =
        build_context_import_dir(&ctxt, a, S("testdata/cgo_disabled"), 0, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    for (Int i = 0; i < p->imports.len; i++) {
        if (str_eq(gbt_at(p->imports, i), S("should/be/ignored")))
            testing_t_errorf_v(t, "found import %q in ignored cgo file",
                               gbt_at(p->imports, i));
    }
}

/* Issue #52053. Check that if there is a file x_GOOS_GOARCH.go that both GOOS
 * and GOARCH show up in the Package.AllTags field. We test both the case where
 * the file matches and where the file does not match. The latter case used to
 * fail, incorrectly omitting GOOS. */
static void TestAllTags(TestingT *t) {
    gbt_begin(t);
    if (!gbt_testdata(t))
        return;
    BuildContext ctxt = gbt_default();
    ctxt.goarch = S("arm");
    ctxt.goos = S("netbsd");
    Error err = BURROW_NO_ERROR;
    BuildPackage *p =
        build_context_import_dir(&ctxt, a, S("testdata/alltags"), 0, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    const char *want[] = {"arm", "netbsd"};
    if (!gbt_strs_equal(p->all_tags, 2, want))
        testing_t_errorf_v(t, "AllTags = %s, want [arm netbsd]", gbt_show(p->all_tags));
    const char *want_files[] = {"alltags.go", "x_netbsd_arm.go"};
    if (!gbt_strs_equal(p->go_files, 2, want_files))
        testing_t_errorf_v(t, "GoFiles = %s, want [alltags.go x_netbsd_arm.go]",
                           gbt_show(p->go_files));

    ctxt.goarch = S("amd64");
    ctxt.goos = S("linux");
    p = build_context_import_dir(&ctxt, a, S("testdata/alltags"), 0, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    if (!gbt_strs_equal(p->all_tags, 2, want))
        testing_t_errorf_v(t, "AllTags = %s, want [arm netbsd]", gbt_show(p->all_tags));
    if (!gbt_strs_equal(p->go_files, 1, want_files))
        testing_t_errorf_v(t, "GoFiles = %s, want [alltags.go]", gbt_show(p->go_files));
}

static void TestAllTagsNonSourceFile(TestingT *t) {
    gbt_begin(t);
    if (!gbt_testdata(t))
        return;
    BuildContext ctxt = gbt_default();
    Error err = BURROW_NO_ERROR;
    BuildPackage *p =
        build_context_import_dir(&ctxt, a, S("testdata/non_source_tags"), 0, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    if (p->all_tags.len > 0)
        testing_t_errorf_v(t, "AllTags = %s, want empty", gbt_show(p->all_tags));
}

/* list as fmt.Sprintf("%q", list) shows a []Directive. */
static Str gbt_show_directives(Slice list) {
    StringsBuilder b = STRINGS_BUILDER(a);
    (void)strings_builder_write_string(&b, S("["), NULL);
    for (Int i = 0; i < list.len; i++) {
        BuildDirective d = BURROW_AT(BuildDirective, list, i);
        Str s = fmt_sprintf_v(a, "%s{%s %s}", i > 0 ? S(" ") : BURROW_STR_EMPTY,
                              strconv_quote(a, d.text),
                              strconv_quote(a, token_position_string(d.pos, a)));
        (void)strings_builder_write_string(&b, s, NULL);
    }
    (void)strings_builder_write_string(&b, S("]"), NULL);
    return strings_builder_string(&b);
}

static void gbt_check_directives(TestingT *t, const char *name, Slice list, Str want) {
    if (strcmp(burrow_os_name(), "windows") == 0)
        want = strings_replace_all(a, want, S("testdata/directives/"),
                                   S("testdata\\\\directives\\\\"));
    Str s = gbt_show_directives(list);
    if (!str_eq(s, want))
        testing_t_errorf_v(t, "%s = %s, want %s", gbt_cstr(name), s, want);
}

static void TestDirectives(TestingT *t) {
    gbt_begin(t);
    if (!gbt_testdata(t))
        return;
    Error err = BURROW_NO_ERROR;
    BuildPackage *p = build_import_dir(a, S("testdata/directives"), 0, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "could not import testdata: %s", error_text(err));
        return;
    }

    gbt_check_directives(t, "Directives", p->directives,
                         S("[{\"//go:main1\" \"testdata/directives/a.go:1:1\"} "
                           "{\"//go:plant\" \"testdata/directives/eve.go:1:1\"}]"));
    gbt_check_directives(t, "TestDirectives", p->test_directives,
                         S("[{\"//go:test1\" \"testdata/directives/a_test.go:1:1\"} "
                           "{\"//go:test2\" \"testdata/directives/b_test.go:1:1\"}]"));
    gbt_check_directives(t, "XTestDirectives", p->x_test_directives,
                         S("[{\"//go:xtest1\" \"testdata/directives/c_test.go:1:1\"} "
                           "{\"//go:xtest2\" \"testdata/directives/d_test.go:1:1\"} "
                           "{\"//go:xtest3\" \"testdata/directives/d_test.go:2:1\"}]"));
}

/* TestContextImportGoWithUseAllFiles ensures that when Context.UseAllFiles is
 * set, that the Go command is not invoked. */
static void TestContextImportGoWithUseAllFiles(TestingT *t) {
    gbt_begin(t);
    BuildContext ctxt = gbt_zero_context();
    ctxt.use_all_files = true;
    BuildPackage *p = BURROW_NEW(a, BuildPackage);
    if (p == NULL) {
        testing_t_fatal_v(t, "out of memory");
        return;
    }
    Error got = burrow__build_import_go(&ctxt, a, p, S("some/package"), S("."), 0);
    if (!errors_is(got, burrow__build_err_no_modules()))
        testing_t_fatalf_v(t, "Error mismatch:\n\tGot:  %s\n\tWant: %s", gbt_err(got),
                           error_text(burrow__build_err_no_modules()));
}

/* -------------------------------------------------------------- read_test */

typedef enum GbtReadFunc {
    GBT_READ_GO_INFO,
    GBT_READ_COMMENTS,
} GbtReadFunc;

static Str gbt_read(GbtReadFunc fn, Str in, Error *err) {
    StringsReader *sr = strings_new_reader(a, in);
    if (sr == NULL)
        return BURROW_STR_EMPTY;
    IoReader r = strings_reader_as_io_reader(sr);
    if (fn == GBT_READ_COMMENTS)
        return burrow__build_read_comments(a, r, err);
    burrow__BuildFileInfo info;
    memset(&info, 0, sizeof info);
    info.parse_err = BURROW_NO_ERROR;
    *err = burrow__build_read_go_info(a, r, &info);
    return info.header;
}

/* testRead. With ignore_failures, the tests want no error but for a NUL, as
 * TestReadFailuresIgnored has it. */
static void gbt_test_read(TestingT *t, const GbtRead *tests, Int n, GbtReadFunc fn,
                          bool ignore_failures) {
    for (Int i = 0; i < n; i++) {
        Str tin = str_from_bytes((const Byte *)tests[i].in, tests[i].len);
        Str tt_err = gbt_cstr(tests[i].err);
        if (ignore_failures && !strings_contains(tt_err, S("NUL")))
            tt_err = BURROW_STR_EMPTY;

        /* The test text ends at a U+2119, and starts at a U+1D53B when
         * there is one, as in Go. */
        bool found = false;
        Str after_p = BURROW_STR_EMPTY;
        Str before_p = strings_cut(tin, S("\342\204\231"), &after_p, &found);
        Str in = gbt_cat(before_p, after_p);
        Str test_out = before_p;

        Str after_d = BURROW_STR_EMPTY;
        Str before_d = strings_cut(before_p, S("\360\235\224\273"), &after_d, &found);
        if (found) {
            in = gbt_cat(gbt_cat(before_d, after_d), after_p);
            test_out = after_d;
        }

        Error err = BURROW_NO_ERROR;
        Str buf = gbt_read(fn, in, &err);
        if (BURROW_FAILED(err)) {
            if (tt_err.len == 0)
                testing_t_errorf_v(t, "#%d: err=%q, expected success (%q)", i,
                                   error_text(err), buf);
            else if (!strings_contains(error_text(err), tt_err))
                testing_t_errorf_v(t, "#%d: err=%q, expected %q", i, error_text(err),
                                   tt_err);
            continue;
        }
        if (tt_err.len != 0) {
            testing_t_errorf_v(t, "#%d: success, expected %q", i, tt_err);
            continue;
        }

        if (!str_eq(buf, test_out))
            testing_t_errorf_v(t, "#%d: wrong output:\nhave %q\nwant %q\n", i, buf,
                               test_out);
    }
}

static void TestReadGoInfo(TestingT *t) {
    gbt_begin(t);
    gbt_test_read(t, gbt_read_go_info_tests, NELEM(gbt_read_go_info_tests),
                  GBT_READ_GO_INFO, false);
}

static void TestReadComments(TestingT *t) {
    gbt_begin(t);
    gbt_test_read(t, gbt_read_comments_tests, NELEM(gbt_read_comments_tests),
                  GBT_READ_COMMENTS, false);
}

/* Syntax errors should not be reported (false arg to readImports). Instead,
 * entire file should be the output and no error. */
static void TestReadFailuresIgnored(TestingT *t) {
    gbt_begin(t);
    gbt_test_read(t, gbt_read_failures_tests, NELEM(gbt_read_failures_tests),
                  GBT_READ_GO_INFO, true);
}

static void TestReadEmbed(TestingT *t) {
    gbt_begin(t);
    TokenFileSet *fset = token_new_file_set(a);
    for (Int i = 0; i < NELEM(gbt_read_embed_tests); i++) {
        burrow__BuildFileInfo info;
        memset(&info, 0, sizeof info);
        info.name = S("test");
        info.fset = fset;
        info.parse_err = BURROW_NO_ERROR;
        StringsReader *r = strings_new_reader(a, gbt_cstr(gbt_read_embed_tests[i].in));
        if (r == NULL) {
            testing_t_fatal_v(t, "out of memory");
            return;
        }
        Error err =
            burrow__build_read_go_info(a, strings_reader_as_io_reader(r), &info);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d: %s", i, error_text(err));
            continue;
        }
        StringsBuilder b = STRINGS_BUILDER(a);
        Str sep = BURROW_STR_EMPTY;
        for (Int j = 0; j < info.embeds.len; j++) {
            burrow__BuildFileEmbed emb =
                BURROW_AT(burrow__BuildFileEmbed, info.embeds, j);
            Str s = fmt_sprintf_v(a, "%s%s:%s", sep, token_position_string(emb.pos, a),
                                  emb.pattern);
            (void)strings_builder_write_string(&b, s, NULL);
            sep = S("\n");
        }
        Str got = strings_builder_string(&b);
        Str want = gbt_cstr(gbt_read_embed_tests[i].out);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "#%d: embeds:\n%s\nwant:\n%s", i, got, want);
    }
}

/* ----------------------------------------------------------- syslist_test */

static void TestGoodOSArch(TestingT *t) {
    gbt_begin(t);
    Str this_os = gbt_cstr(burrow_os_name());
    Str this_arch = gbt_cstr(burrow_arch_name());
    Str other_os = S("linux");
    if (!str_eq(this_os, S("darwin")) && !str_eq(this_os, S("ios")))
        other_os = S("darwin");
    Str other_arch = S("386");
    if (!str_eq(this_arch, S("amd64")))
        other_arch = S("amd64");

    struct {
        Str name;
        bool result;
    } tests[] = {
        {S("file.go"), true},
        {S("file.c"), true},
        {S("file_foo.go"), true},
        {fmt_sprintf_v(a, "file_%s.go", this_arch), true},
        {fmt_sprintf_v(a, "file_%s.go", other_arch), false},
        {fmt_sprintf_v(a, "file_%s.go", this_os), true},
        {fmt_sprintf_v(a, "file_%s.go", other_os), false},
        {fmt_sprintf_v(a, "file_%s_%s.go", this_os, this_arch), true},
        {fmt_sprintf_v(a, "file_%s_%s.go", other_os, this_arch), false},
        {fmt_sprintf_v(a, "file_%s_%s.go", this_os, other_arch), false},
        {fmt_sprintf_v(a, "file_%s_%s.go", other_os, other_arch), false},
        {fmt_sprintf_v(a, "file_foo_%s.go", this_arch), true},
        {fmt_sprintf_v(a, "file_foo_%s.go", other_arch), false},
        {fmt_sprintf_v(a, "file_%s.c", this_os), true},
        {fmt_sprintf_v(a, "file_%s.c", other_os), false},
    };
    BuildContext ctxt = gbt_default();
    for (Int i = 0; i < NELEM(tests); i++) {
        if (burrow__build_good_os_arch_file(&ctxt, a, tests[i].name, gbt_tag_map()) !=
            tests[i].result) {
            testing_t_fatalf_v(t, "goodOSArchFile(%q) != %v", tests[i].name,
                               tests[i].result);
            return;
        }
    }
}

#define TESTS(X)                                                                       \
    X(TestMatch)                                                                       \
    X(TestDotSlashImport)                                                              \
    X(TestEmptyImport)                                                                 \
    X(TestEmptyFolderImport)                                                           \
    X(TestMultiplePackageImport)                                                       \
    X(TestLocalDirectory)                                                              \
    X(TestShouldBuild)                                                                 \
    X(TestGoodOSArchFile)                                                              \
    X(TestMatchFile)                                                                   \
    X(TestImportCmd)                                                                   \
    X(TestExpandSrcDir)                                                                \
    X(TestShellSafety)                                                                 \
    X(TestImportDirNotExist)                                                           \
    X(TestImportVendor)                                                                \
    X(TestImportVendorFailure)                                                         \
    X(TestImportVendorParentFailure)                                                   \
    X(TestImportPackageOutsideModule)                                                  \
    X(TestIssue23594)                                                                  \
    X(TestIssue56509)                                                                  \
    X(TestMissingImportErrorRepetition)                                                \
    X(TestCgoImportsIgnored)                                                           \
    X(TestAllTags)                                                                     \
    X(TestAllTagsNonSourceFile)                                                        \
    X(TestDirectives)                                                                  \
    X(TestContextImportGoWithUseAllFiles)                                              \
    X(TestReadGoInfo)                                                                  \
    X(TestReadComments)                                                                \
    X(TestReadFailuresIgnored)                                                         \
    X(TestReadEmbed)                                                                   \
    X(TestGoodOSArch)

TESTING_MAIN(TESTS)
