/* Derived from Go's src/go/internal/srcimporter/srcimporter_test.go.
 * Go source: go1.27.1.
 *
 * The tests read the standard library's sources, so each one is skipped when
 * there is no GOROOT with them in it, which is what testenv.MustHaveSource
 * does for Go's.
 *
 * TestForCompiler is from go/importer's importer_test.go, and needs a go
 * command as well.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_importer.h"

#include "../src/go/importer_internal.h"
#include "burrow/burrow.h"
#include "burrow/go/build.h"
#include "burrow/go/importer.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"
#include "burrow/os/exec.h"
#include "burrow/strings.h"

#include <stdbool.h>
#include <stddef.h>

#define S(lit) BURROW_S(lit)
#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static Arena ar;
static Alloc *a;

static void gi_free_arena(void *env) {
    arena_free((Arena *)env);
}

/* A fresh arena for the test, freed after its other cleanups. */
static void gi_begin(TestingT *t) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    testing_t_cleanup(t, BURROW_FN(Func, gi_free_arena, &ar));
}

static void gi_chdir_back(void *env) {
    (void)os_chdir(*(Str *)env);
}

/* testenv.MustHaveSource and testenv.GOROOT(t): the Go root, with the test
 * skipped unless there is one with its sources.
 *
 * Not in Go: Go runs a package's tests in its directory, which matters here,
 * since "." is where imports are resolved from and the vendored packages are
 * only found from inside GOROOT. So the test goes there, and back once it is
 * done. The sources also have to be go1.27's, as go_types_importer.h
 * explains. */
static Str gi_goroot(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    Str goroot = gtt_find_goroot(a);
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
    Str version;
    if (!gtt_goroot_is_go127(a, goroot, &version)) {
        testing_t_skipf_v(t, "%s has %q, and these tests are go1.27's", goroot,
                          version);
        return BURROW_STR_EMPTY;
    }
    Str *wd = BURROW_NEW(a, Str);
    *wd = os_getwd(a, &e);
    if (BURROW_FAILED(e)) {
        testing_t_fatal_v(t, e);
        return BURROW_STR_EMPTY;
    }
    e = os_chdir(filepath_join_v(a, 5, goroot, S("src"), S("go"), S("internal"),
                                 S("srcimporter")));
    if (BURROW_FAILED(e)) {
        testing_t_fatal_v(t, e);
        return BURROW_STR_EMPTY;
    }
    testing_t_cleanup(t, BURROW_FN(Func, gi_chdir_back, wd));
    return goroot;
}

/* build.Default with its GOROOT set, as TestMain sets it. */
static BuildContext *gi_default(Str goroot) {
    BuildContext *c = BURROW_NEW(a, BuildContext);
    *c = build_default(a);
    c->goroot = goroot;
    return c;
}

/* New(&build.Default, token.NewFileSet(), make(map[string]*types.Package)).
 *
 * Not in Go: Go's tests share one importer, and here each test makes its own
 * in its own arena. */
static burrow__SrcImporter *gi_new_importer(BuildContext *ctxt) {
    Map *packages = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    return burrow__srcimporter_new(a, ctxt, token_new_file_set(a), packages);
}

static void do_import(TestingT *t, burrow__SrcImporter *importer, Str path,
                      Str src_dir) {
    Time t0 = time_now();
    Error err = BURROW_NO_ERROR;
    (void)burrow__srcimporter_import_from(importer, path, src_dir, 0, &err);
    if (BURROW_FAILED(err)) {
        /* don't report an error if there's no buildable Go files */
        if (errors_as(err, TYPE_BUILD_NO_GO_ERROR) == NULL)
            testing_t_errorf_v(t, "import %q failed (%v)", path, err);
        return;
    }
    testing_t_logf_v(t, "import %q: %s", path, duration_string(time_since(t0), a));
}

/* walkDir imports the all the packages with the given path prefix recursively.
 * It returns the number of packages imported and whether importing was
 * aborted because time has passed endTime. */
static Int walk_dir(TestingT *t, burrow__SrcImporter *importer, Str goroot, Str path,
                    Time end_time, bool *aborted) {
    *aborted = false;
    if (time_after(time_now(), end_time)) {
        testing_t_log_v(t, "testing time used up");
        *aborted = true;
        return 0;
    }

    /* ignore fake packages and testdata directories */
    if (str_eq(path, S("builtin")) || str_eq(path, S("unsafe")) ||
        strings_has_suffix(path, S("testdata")))
        return 0;

    Error err = BURROW_NO_ERROR;
    Slice list = os_read_dir(a, filepath_join_v(a, 3, goroot, S("src"), path), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "walkDir %s failed (%v)", path, err);
        return 0;
    }

    Int nimports = 0;
    bool has_go_files = false;
    for (Int i = 0; i < list.len; i++) {
        FsDirEntry *f = (FsDirEntry *)slice_at(list, i);
        Str name = f->vt->name(f->data);
        if (f->vt->is_dir(f->data)) {
            bool abort = false;
            Int n = walk_dir(t, importer, goroot, filepath_join_v(a, 2, path, name),
                             end_time, &abort);
            nimports += n;
            if (abort) {
                *aborted = true;
                return nimports;
            }
        } else if (strings_has_suffix(name, S(".go"))) {
            has_go_files = true;
        }
    }

    if (has_go_files) {
        do_import(t, importer, path, S(""));
        nimports++;
    }

    return nimports;
}

static void TestImportStdLib(TestingT *t) {
#if defined(BURROW_OS_COSMO)
    /* Not in Go: a Cosmopolitan build reports its GOOS as cosmo, which the Go
     * tree has no port for. No file in internal/goos is built for it, so the
     * runtime package and everything that imports it fail to type-check. */
    testing_t_skip_v(t, "the Go tree has no port for GOOS cosmo");
    return;
#endif
    gi_begin(t);
    Str goroot = gi_goroot(t);
    if (goroot.len == 0)
        return;
    if (testing_short()) {
        testing_t_skip_v(t, "skipping in -short mode");
        return;
    }
    burrow__SrcImporter *importer = gi_new_importer(gi_default(goroot));
    Duration dt = 2 * TIME_SECOND; /* maxTime */
    bool aborted = false;
    /* installed packages */
    Int nimports =
        walk_dir(t, importer, goroot, S(""), time_add(time_now(), dt), &aborted);
    testing_t_logf_v(t, "tested %d imports", nimports);
}

typedef struct ImportedObjectTest {
    Str name;
    Str want;
} ImportedObjectTest;

static const ImportedObjectTest imported_object_tests[] = {
    {S_("flag.Bool"), S_("func Bool(name string, value bool, usage string) *bool")},
    {S_("io.Reader"), S_("type Reader interface{Read(p []byte) (n int, err error)}")},
    /* go/types.gcCompatibilityMode is off => interface not flattened */
    {S_("io.ReadWriter"), S_("type ReadWriter interface{Reader; Writer}")},
    {S_("math.Pi"), S_("const Pi untyped float")},
    {S_("math.Sin"), S_("func Sin(x float64) float64")},
    {S_("math/big.Int"), S_("type Int struct{neg bool; abs nat}")},
    {S_("golang.org/x/text/unicode/norm.MaxSegmentSize"),
     S_("const MaxSegmentSize untyped int")},
};

/* verifyInterfaceMethodRecvs verifies that method receiver types are named if
 * the methods belong to a named interface type. */
static void verify_interface_method_recvs(TestingT *t, TypesNamed *named, Int level) {
    /* avoid endless recursion in case of an embedding bug that lead to a cycle */
    if (level > 10) {
        testing_t_errorf_v(t, "%s: embeds itself",
                           types_type_string(a, types_type(named), TYPES_NO_QUALIFIER));
        return;
    }

    TypesInterface *iface =
        types_as_interface(types_type_underlying(types_type(named)));
    if (iface == NULL)
        return; /* not an interface */

    /* check explicitly declared methods */
    for (Int i = 0; i < types_interface_num_explicit_methods(iface); i++) {
        TypesFunc *m = types_interface_explicit_method(iface, i);
        Str ms = types_object_string(a, types_object(m), TYPES_NO_QUALIFIER);
        TypesVar *recv = types_signature_recv(types_func_signature(m));
        if (recv == NULL) {
            testing_t_errorf_v(t, "%s: missing receiver type", ms);
            continue;
        }
        TypesType rt = types_object_type(types_object(recv));
        if (rt != types_type(named))
            testing_t_errorf_v(
                t, "%s: got recv type %s; want %s", ms,
                types_type_string(a, rt, TYPES_NO_QUALIFIER),
                types_type_string(a, types_type(named), TYPES_NO_QUALIFIER));
    }

    /* check embedded interfaces (they are named, too) */
    for (Int i = 0; i < types_interface_num_embeddeds(iface); i++) {
        /* embedding of interfaces cannot have cycles; recursion will terminate */
        verify_interface_method_recvs(t, types_interface_embedded(iface, i), level + 1);
    }
}

static void TestImportedTypes(TestingT *t) {
#if defined(BURROW_OS_COSMO)
    /* Not in Go: a Cosmopolitan build reports its GOOS as cosmo, which the Go
     * tree has no port for. No file in internal/goos is built for it, so the
     * runtime package and everything that imports it fail to type-check. */
    testing_t_skip_v(t, "the Go tree has no port for GOOS cosmo");
    return;
#endif
    gi_begin(t);
    Str goroot = gi_goroot(t);
    if (goroot.len == 0)
        return;
    burrow__SrcImporter *importer = gi_new_importer(gi_default(goroot));

    for (Int k = 0; k < NELEM(imported_object_tests); k++) {
        const ImportedObjectTest *test = &imported_object_tests[k];
        Int i = strings_last_index(test->name, S("."));
        if (i < 0) {
            testing_t_fatal_v(t, "invalid test data format");
            return;
        }
        Str import_path = {test->name.p, i};
        Str obj_name = {test->name.p + i + 1, test->name.len - i - 1};

        Error err = BURROW_NO_ERROR;
        TypesPackage *pkg =
            burrow__srcimporter_import_from(importer, import_path, S("."), 0, &err);
        if (BURROW_FAILED(err)) {
            testing_t_error_v(t, err);
            continue;
        }

        TypesObject obj = types_scope_lookup(types_package_scope(pkg), obj_name);
        if (obj == NULL) {
            testing_t_errorf_v(t, "%s: object not found", test->name);
            continue;
        }

        Str got = types_object_string(a, obj, types_relative_to(pkg));
        if (!str_eq(got, test->want))
            testing_t_errorf_v(t, "%s: got %q; want %q", test->name, got, test->want);

        TypesNamed *named = types_as_named(types_object_type(obj));
        if (named != NULL)
            verify_interface_method_recvs(t, named, 0);
    }
}

static void TestReimport(TestingT *t) {
    gi_begin(t);
    Str goroot = gi_goroot(t);
    if (goroot.len == 0)
        return;

    /* Reimporting a partially imported (incomplete) package is not supported
     * (see issue #19337). Make sure we recognize the situation and report an
     * error. */
    TypesPackage *math_pkg =
        types_new_package(a, S("math"), S("math")); /* incomplete */
    Map *packages = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    Str path = types_package_path(math_pkg);
    map_set(packages, &path, &math_pkg);
    burrow__SrcImporter *importer =
        burrow__srcimporter_new(a, gi_default(goroot), token_new_file_set(a), packages);
    Error err = BURROW_NO_ERROR;
    (void)burrow__srcimporter_import_from(importer, S("math"), S("."), 0, &err);
    if (BURROW_OK(err) || !strings_has_prefix(error_text(err), S("reimport")))
        testing_t_errorf_v(t, "got %v; want reimport error", err);
}

static void TestIssue20855(TestingT *t) {
    gi_begin(t);
    Str goroot = gi_goroot(t);
    if (goroot.len == 0)
        return;
    burrow__SrcImporter *importer = gi_new_importer(gi_default(goroot));

    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg = burrow__srcimporter_import_from(
        importer, S("go/internal/srcimporter/testdata/issue20855"), S("."), 0, &err);
    if (BURROW_OK(err) ||
        !strings_contains(error_text(err), S("func init must have a body"))) {
        testing_t_fatalf_v(t, "got unexpected or no error: %v", err);
        return;
    }
    if (pkg == NULL)
        testing_t_error_v(t, "got no package despite no hard errors");
}

static void test_import_path(TestingT *t, burrow__SrcImporter *importer, Str pkg_path) {
    Str pkg_name = path_base(pkg_path);

    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg = burrow__srcimporter_import(importer, pkg_path, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }

    if (!str_eq(types_package_name(pkg), pkg_name))
        testing_t_errorf_v(t, "got %q; want %q", types_package_name(pkg), pkg_name);

    if (!str_eq(types_package_path(pkg), pkg_path))
        testing_t_errorf_v(t, "got %q; want %q", types_package_path(pkg), pkg_path);
}

/* TestIssue23092 tests relative imports. */
static void TestIssue23092(TestingT *t) {
    gi_begin(t);
    Str goroot = gi_goroot(t);
    if (goroot.len == 0)
        return;
    test_import_path(t, gi_new_importer(gi_default(goroot)),
                     S("./testdata/issue23092"));
}

/* TestIssue24392 tests imports against a path containing 'testdata'. */
static void TestIssue24392(TestingT *t) {
    gi_begin(t);
    Str goroot = gi_goroot(t);
    if (goroot.len == 0)
        return;
    test_import_path(t, gi_new_importer(gi_default(goroot)),
                     S("go/internal/srcimporter/testdata/issue24392"));
}

static void TestCgo(TestingT *t) {
    gi_begin(t);
    /* testenv.MustHaveGoBuild */
    Str goroot = gi_goroot(t);
    if (goroot.len == 0)
        return;
    Error err = BURROW_NO_ERROR;
    (void)exec_look_path(a, filepath_join_v(a, 3, goroot, S("bin"), S("go")), &err);
    if (BURROW_FAILED(err)) {
        testing_t_skipf_v(t, "no go command in %s", goroot);
        return;
    }
    BuildContext *build_ctx = gi_default(goroot);
    /* testenv.MustHaveCGO */
    if (!build_ctx->cgo_enabled) {
        testing_t_skip_v(t, "test requires cgo");
        return;
    }

    burrow__SrcImporter *importer = gi_new_importer(build_ctx);
    (void)burrow__srcimporter_import_from(importer, S("cmd/cgo/internal/test"),
                                          build_ctx->dir, 0, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Import failed: %v", err);
}

typedef struct ForCompilerCase {
    TokenFileSet *fset;
    Str compiler;
    Str goroot;
} ForCompilerCase;

static const Str the_package = S_("math/big");

static void for_compiler_lookup_default(void *env, TestingT *t) {
    const ForCompilerCase *c = (const ForCompilerCase *)env;
    TypesImporter imp =
        importer_for_compiler(a, c->fset, c->compiler, (ImporterLookup){0});
    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg = imp.vt->import_(imp.data, the_package, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    if (!str_eq(types_package_path(pkg), the_package)) {
        testing_t_fatalf_v(t, "Path() = %q, want %q", types_package_path(pkg),
                           the_package);
        return;
    }

    /* Check that the fileset positions are accurate.
     * https://github.com/golang/go#28995 */
    TypesObject math_big_int = types_scope_lookup(types_package_scope(pkg), S("Int"));
    /* "$GOROOT/src/math/big/int.go:25:1" */
    TokenPosition posn =
        token_file_set_position(c->fset, types_object_pos(math_big_int));
    Str filename = strings_replace(a, posn.filename, S("$GOROOT"), c->goroot, 1);
    Slice data = os_read_file(a, filename, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(
            t, "can't read file containing declaration of math/big.Int: %v", err);
        return;
    }
    Slice lines =
        strings_split(a, str_from_bytes((const Byte *)data.p, data.len), S("\n"));
    if (posn.line > lines.len ||
        !strings_has_prefix(BURROW_AT(Str, lines, posn.line - 1), S("type Int")))
        testing_t_fatalf_v(t, "Object %v position %s does not contain its declaration",
                           types_object_string(a, math_big_int, TYPES_NO_QUALIFIER),
                           token_position_string(posn, a));
}

static void for_compiler_lookup_custom(void *env, TestingT *t) {
    (void)env;
    /* TODO(mdempsky): Decide whether to remove this test, or to fix support for
     * it in unified IR. It's not clear that we actually need to support
     * importing "math/big" as "math/bigger", for example. cmd/link no longer
     * supports that.
     *
     * Not in Go: the rest of Go's test comes after this skip and never runs,
     * so it is not here. */
    testing_t_skip_v(t, "not supported by GOEXPERIMENT=unified; see go.dev/cl/406319");
}

static void TestForCompiler(TestingT *t) {
    gi_begin(t);
    /* testenv.MustHaveGoBuild */
    Str goroot = gi_goroot(t);
    if (goroot.len == 0)
        return;
    Str go = filepath_join_v(a, 3, goroot, S("bin"), S("go"));
    Error err = BURROW_NO_ERROR;
    (void)exec_look_path(a, go, &err);
    if (BURROW_FAILED(err)) {
        testing_t_skipf_v(t, "no go command in %s", goroot);
        return;
    }
    /* TestMain sets build.Default.GOROOT, which build_default reads from
     * $GOROOT. */
    err = os_setenv(S("GOROOT"), goroot);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }

    ExecCmd *cmd =
        exec_command_v(a, go, 4, S("list"), S("-export"),
                       S("-f={{context.Compiler}}:{{.Export}}"), the_package);
    Slice out = exec_cmd_combined_output(cmd, a, &err);
    exec_cmd_free(cmd);
    Str outs = str_from_bytes((const Byte *)out.p, out.len);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "go list %s: %v\n%s", the_package, err, outs);
        return;
    }
    Str export_ = strings_trim_space(outs);
    Str target;
    bool found;
    Str compiler = strings_cut(export_, S(":"), &target, &found);

    if (str_eq(compiler, S("gccgo"))) {
        testing_t_skip_v(t, "golang.org/issue/22500");
        return;
    }

    ForCompilerCase *c = BURROW_NEW(a, ForCompilerCase);
    *c = (ForCompilerCase){token_new_file_set(a), compiler, goroot};

    testing_t_run(t, S("LookupDefault"),
                  BURROW_FN(TestingTFunc, for_compiler_lookup_default, c));
    testing_t_run(t, S("LookupCustom"),
                  BURROW_FN(TestingTFunc, for_compiler_lookup_custom, c));
}

#define TESTS(X)                                                                       \
    X(TestImportStdLib)                                                                \
    X(TestImportedTypes)                                                               \
    X(TestReimport)                                                                    \
    X(TestIssue20855)                                                                  \
    X(TestIssue23092)                                                                  \
    X(TestIssue24392)                                                                  \
    X(TestCgo)                                                                         \
    X(TestForCompiler)

TESTING_MAIN(TESTS)
