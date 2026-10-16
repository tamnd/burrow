/* Derived from Go's src/go/types/stdlib_test.go. Go source: go1.27.1.
 *
 * This file tests types.Check by using it to typecheck the standard library
 * and tests.
 *
 * Not in Go: TestStdlib checks the packages one at a time, in the order the
 * walk finds them, where Go checks one per CPU in map order. The types keep
 * the allocator they were made with and some of them allocate from it later,
 * and an arena is not safe for two threads at once. The checking runs on a
 * goroutine with a large stack, since each package is checked in the middle
 * of checking the first package that imports it.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_importer.h"

#include "burrow/burrow.h"
#include "burrow/go/ast.h"
#include "burrow/go/build.h"
#include "burrow/go/parser.h"
#include "burrow/go/scanner.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"
#include "burrow/os.h"
#include "burrow/path/filepath.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include <stdbool.h>
#include <stddef.h>

#define S(lit) BURROW_S(lit)

/* Not in Go: how big a stack the checking gets. */
#define TST_STACK ((size_t)256 << 20)

/* What testenv.MustHaveGoBuild and testenv.GOROOT(t) need here: the Go root
 * with its sources, or a skip. */
static Str must_have_goroot(TestingT *t, Alloc *a) {
    SKIP_WITHOUT_EXEC(t);
    Str goroot = gtt_find_goroot(a);
    if (goroot.len == 0) {
        testing_t_skip_v(t, "no GOROOT and no go command to ask for one");
        return goroot;
    }
    Error e = BURROW_NO_ERROR;
    (void)os_stat(a, filepath_join_v(a, 2, goroot, S("src")), &e);
    if (BURROW_FAILED(e))
        testing_t_skipf_v(t, "no Go sources in %s", goroot);
    return goroot;
}

/* Not in Go: build.Default, with the GOROOT the test found. */
static BuildContext build_ctxt(Alloc *a, Str goroot) {
    BuildContext ctxt = build_default(a);
    ctxt.goroot = goroot;
    return ctxt;
}

/* Not in Go: runs fn(env) on a goroutine with a large stack and waits for it. */
typedef struct DeepRun {
    void (*fn)(void *env);
    void *env;
    SyncWaitGroup wg;
} DeepRun;

static void deep_body(void *env) {
    DeepRun *r = env;
    r->fn(r->env);
    sync_wait_group_done(&r->wg);
}

static void run_deep(TestingT *t, void (*fn)(void *env), void *env) {
    DeepRun r = {fn, env, {0}};
    sync_wait_group_add(&r.wg, 1);
    if (!go_stack(BURROW_FN(Func, deep_body, &r), TST_STACK))
        testing_t_fatalf_v(t, "go_stack failed");
    sync_wait_group_wait(&r.wg);
}

static void free_arena(void *env) {
    arena_free(env);
}

/* Package paths of excluded packages. */
static const char *const excluded[] = {
    "builtin",
    "cmd/compile/internal/ssa/_gen",
    "crypto/internal/cryptotest/wycheproof/_schema",
    "crypto/internal/cryptotest/x509limbo/_schema",
    "runtime/_mkmalloc",
    "simd/archsimd/_gen/midway",
    "simd/archsimd/_gen/sgutil",
    "simd/archsimd/_gen/simdgen",
    "simd/archsimd/_gen/simdgen/arm64",
    "simd/archsimd/_gen/tmplgen",
    "simd/archsimd/_gen/unify",
    "simd/archsimd/_gen/wasmgen",
};

static bool is_excluded(Str path) {
    for (size_t i = 0; i < sizeof excluded / sizeof excluded[0]; i++)
        if (str_eq(path, str_from_cstr(excluded[i])))
            return true;
    return false;
}

/* A futurePackage is a future result of type-checking.
 *
 * Not in Go: there is no done channel, since the packages are checked one at
 * a time; done says whether pkg and err are set. */
typedef struct FuturePackage {
    bool done;
    TypesPackage *pkg;
    Error err;
} FuturePackage;

/* stdlibChecker implements type-checking of the packages defined by dirFiles,
 * which must define a closed set of packages (such as GOROOT/src).
 *
 * Not in Go: dirs keeps the order the walk found the directories in, and a is
 * where everything comes from. */
typedef struct StdlibChecker {
    Map *dir_files; /* non-test files per directory, Slice * of Str; must be
                       pre-populated */
    Slice dirs;     /* Str */
    Map *pkgs;      /* future cache of type-checking results, FuturePackage * */
    BuildContext ctxt;
    Alloc *a;
    TestingT *t;
} StdlibChecker;

static TypesPackage *get_dir_package(StdlibChecker *c, Str dir, Error *err);

static TypesPackage *stdlib_import(void *self, Str path, Error *err) {
    (void)self;
    (void)path;
    (void)err;
    runtime_panic(S("unimplemented: use ImportFrom"));
    return NULL;
}

static TypesPackage *stdlib_import_from(void *self, Str path, Str dir,
                                        TypesImportMode mode, Error *err) {
    (void)mode;
    StdlibChecker *c = self;
    if (str_eq(path, S("unsafe"))) {
        /* unsafe cannot be type checked normally. */
        return types_unsafe();
    }

    Error e = BURROW_NO_ERROR;
    BuildPackage *p =
        build_context_import(&c->ctxt, c->a, path, dir, BUILD_FIND_ONLY, &e);
    if (BURROW_FAILED(e)) {
        *err = e;
        return NULL;
    }

    TypesPackage *pkg = get_dir_package(c, p->dir, &e);
    if (pkg != NULL) {
        /* As long as pkg is non-nil, avoid redundant errors related to failed
         * imports. TestStdlib will collect errors once for each package. */
        return pkg;
    }
    *err = e;
    return NULL;
}

static const TypesImporterVT stdlib_checker_vt = {NULL, stdlib_import,
                                                  stdlib_import_from};

/* ErrList collects the errors the checker reports. */
typedef struct ErrList {
    Alloc *a;
    Slice errs; /* Error */
} ErrList;

static void collect_err(void *env, const TypesError *err) {
    ErrList *l = env;
    Error e = types_error_as_error(err, l->a);
    l->errs = slice_append(l->a, l->errs, &e, 1);
}

/* typecheckFiles typechecks the given package files.
 *
 * Not in Go: a is where the files and the package come from. */
static TypesPackage *typecheck_files(Alloc *a, Str path, Slice filenames,
                                     TypesImporter importer, Error *err) {
    TokenFileSet *fset = token_new_file_set(a);

    /* Parse package files. */
    Slice files = slice_nil(TYPE_AST_FILE_PTR);
    for (Int i = 0; i < filenames.len; i++) {
        Str filename = BURROW_AT(Str, filenames, i);
        Error e = BURROW_NO_ERROR;
        AstFile *file =
            parser_parse_file(a, fset, filename, BURROW_ANY(NULL, NULL),
                              PARSER_ALL_ERRORS | PARSER_SKIP_OBJECT_RESOLUTION, &e);
        if (BURROW_FAILED(e)) {
            *err = e;
            return NULL;
        }

        files = slice_append(a, files, &file, 1);
    }

    if (testing_verbose()) {
        fmt_println_v("package", BURROW_AT(AstFile *, files, 0)->name->name);
        for (Int i = 0; i < filenames.len; i++)
            fmt_println_v("\t", BURROW_AT(Str, filenames, i));
    }

    /* Typecheck package files. */
    ErrList errs = {a, slice_nil(TYPE_ERROR)};
    TypesConfig conf = {0};
    conf.error = (TypesErrorFunc){collect_err, &errs};
    conf.importer = importer;
    TypesInfo info = {0};
    info.uses = map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
    Error cerr = BURROW_NO_ERROR;
    TypesPackage *pkg = types_config_check(&conf, a, path, fset, files, &info, &cerr);
    if (errs.errs.len > 0) {
        *err = errors_join(a, errs.errs);
        return pkg;
    }

    /* Perform checks of API invariants. */

    /* All Objects have a package, except predeclared ones. */
    TypesScope *universe = types_universe();
    TypesInterface *error_iface = types_as_interface(types_type_underlying(
        types_object_type(types_scope_lookup(universe, S("error")))));
    TypesObject error_error = types_object(
        types_interface_explicit_method(error_iface, 0)); /* (error).Error */
    const void *k;
    void *v;
    for (MapIter it = map_iter(info.uses); map_next(&it, &k, &v);) {
        AstIdent *id = *(AstIdent *const *)k;
        TypesObject obj = *(TypesObject *)v;
        bool predeclared =
            obj == types_scope_lookup(universe, types_object_name(obj)) ||
            obj == error_error;
        if (predeclared == (types_object_pkg(obj) != NULL)) {
            Str posn =
                token_position_string(token_file_set_position(fset, id->name_pos), a);
            Str s = types_object_string(a, obj, (TypesQualifier){0});
            if (predeclared)
                *err = fmt_errorf_v("%s: predeclared object with package: %s", posn, s);
            else
                *err = fmt_errorf_v("%s: user-defined object without package: %s", posn,
                                    s);
            return NULL;
        }
    }

    return pkg;
}

/* getDirPackage gets the package defined in dir from the future cache.
 *
 * If this is the first request for the package, getDirPackage type-checks. */
static TypesPackage *get_dir_package(StdlibChecker *c, Str dir, Error *err) {
    FuturePackage **found = map_get(c->pkgs, &dir);
    FuturePackage *fut = found != NULL ? *found : NULL;
    if (fut == NULL) {
        /* First request for this package dir; type check. */
        fut = BURROW_NEW(c->a, FuturePackage);
        if (fut == NULL)
            runtime_panic(S("out of memory"));
        dir = str_clone(c->a, dir);
        map_set(c->pkgs, &dir, &fut);
        Slice **files = map_get(c->dir_files, &dir);
        if (files == NULL) {
            fut->err = fmt_errorf_v("no files for %s", dir);
        } else {
            /* Using dir as the package path here may be inconsistent with the
             * behavior of a normal importer, but is sufficient as dir is by
             * construction unique to this package. */
            fut->pkg = typecheck_files(
                c->a, dir, **files, (TypesImporter){&stdlib_checker_vt, c}, &fut->err);
        }
        fut->done = true;
    } else if (!fut->done) {
        /* Not in Go: Go would wait for the goroutine checking the package,
         * and with one goroutine that is a cycle, which GOROOT/src has none
         * of. */
        *err = fmt_errorf_v("import cycle through %s", dir);
        return NULL;
    }
    *err = fut->err;
    return fut->pkg;
}

/* pkgFilenames returns the list of package filenames for the given directory. */
static Slice pkg_filenames(Alloc *a, Str goroot, Str dir, bool include_test,
                           Error *err) {
    BuildContext ctxt = build_ctxt(a, goroot);
    ctxt.cgo_enabled = false;
    Error e = BURROW_NO_ERROR;
    BuildPackage *pkg = build_context_import_dir(&ctxt, a, dir, 0, &e);
    if (BURROW_FAILED(e)) {
        if (errors_as(e, TYPE_BUILD_NO_GO_ERROR) != NULL)
            return slice_nil(TYPE_STRING); /* no *.go files, not an error */
        *err = e;
        return slice_nil(TYPE_STRING);
    }
    if (is_excluded(pkg->import_path))
        return slice_nil(TYPE_STRING);
    Slice parts = strings_split(a, pkg->import_path, S("/"));
    for (Int i = 0; i < parts.len; i++) {
        if (str_eq(BURROW_AT(Str, parts, i), S("_asm"))) {
            /* Submodules where not all dependencies are available.
             * See go.dev/issue/46027. */
            return slice_nil(TYPE_STRING);
        }
    }
    Slice filenames = slice_nil(TYPE_STRING);
    for (Int i = 0; i < pkg->go_files.len; i++) {
        Str name = filepath_join_v(a, 2, pkg->dir, BURROW_AT(Str, pkg->go_files, i));
        filenames = slice_append(a, filenames, &name, 1);
    }
    if (include_test) {
        for (Int i = 0; i < pkg->test_go_files.len; i++) {
            Str name =
                filepath_join_v(a, 2, pkg->dir, BURROW_AT(Str, pkg->test_go_files, i));
            filenames = slice_append(a, filenames, &name, 1);
        }
    }
    return filenames;
}

/* walker: Go's walkPkgDirs with pkgh filling in the checker and errh
 * t.Error. */
static void walk(StdlibChecker *c, Str goroot, Str dir) {
    Error err = BURROW_NO_ERROR;
    Slice files = os_read_dir(c->a, dir, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(c->t, error_text(err));
        return;
    }

    /* apply pkgh to the files in directory dir */

    /* Don't get test files as these packages are imported. */
    Slice pkg_files = pkg_filenames(c->a, goroot, dir, false, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(c->t, error_text(err));
        return;
    }
    if (pkg_files.len > 0) {
        Slice *v = BURROW_NEW(c->a, Slice);
        if (v == NULL)
            runtime_panic(S("out of memory"));
        *v = pkg_files;
        map_set(c->dir_files, &dir, &v);
        c->dirs = slice_append(c->a, c->dirs, &dir, 1);
    }

    /* traverse subdirectories, but don't walk into testdata */
    for (Int i = 0; i < files.len; i++) {
        FsDirEntry *f = (FsDirEntry *)slice_at(files, i);
        Str name = f->vt->name(f->data);
        if (f->vt->is_dir(f->data) && !str_eq(name, S("testdata")))
            walk(c, goroot, filepath_join_v(c->a, 2, dir, name));
    }
}

static void stdlib_body(void *env) {
    StdlibChecker *c = env;
    Time start = time_now();

    for (Int i = 0; i < c->dirs.len; i++) {
        Str dir = BURROW_AT(Str, c->dirs, i);
        Error err = BURROW_NO_ERROR;
        (void)get_dir_package(c, dir, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(c->t, "error checking %s: %v", dir, err);
    }

    if (testing_verbose())
        fmt_println_v(c->dirs.len, "packages typechecked in",
                      duration_string(time_since(start), c->a));
}

static void TestStdlib(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping in short mode");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    testing_t_cleanup(t, BURROW_FN(Func, free_arena, &ar));
    Str goroot = must_have_goroot(t, a);

    /* Collect non-test files. */
    StdlibChecker c = {0};
    c.dir_files = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    c.dirs = slice_nil(TYPE_STRING);
    c.pkgs = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    c.ctxt = build_ctxt(a, goroot);
    c.a = a;
    c.t = t;
    walk(&c, goroot, filepath_join_v(a, 2, goroot, S("src")));

    run_deep(t, stdlib_body, &c);
}

/* firstComment returns the contents of the first non-empty comment in the
 * given file, "skip", or the empty string. No matter the present comments, if
 * any of them contains a build tag, the result is always "skip". Only comments
 * before the "package" token and within the first 4K of the file are
 * considered. */
static Str first_comment(Alloc *a, TokenFileSet *fset, Str filename) {
    Error err = BURROW_NO_ERROR;
    OsFile *f = os_open(a, filename, &err);
    if (BURROW_FAILED(err))
        return S("");

    Slice src = slice_make(a, TYPE_BYTE, 4 << 10, 4 << 10); /* read at most 4KB */
    Error rerr = BURROW_NO_ERROR;
    Int n = os_file_read(f, src, &rerr);
    (void)os_file_close(f);
    os_file_free(f);
    if (n < 0)
        n = 0;
    src.len = n;

    Str first = S("");
    GoScanner s;
    go_scanner_init(
        &s, a, token_file_set_add_file(fset, S(""), token_file_set_base(fset), n), src,
        (GoScannerErrorHandler){0} /* ignore errors */, GO_SCANNER_SCAN_COMMENTS);
    for (;;) {
        Token tok;
        Str lit;
        (void)go_scanner_scan(&s, &tok, &lit);
        switch (tok) {
        case TOKEN_COMMENT: {
            /* remove trailing * / of multi-line comment */
            if (lit.p[1] == '*')
                lit.len -= 2;
            Str contents = strings_trim_space((Str){lit.p + 2, lit.len - 2});
            if (strings_has_prefix(contents, S("go:build ")))
                return S("skip");
            if (first.len == 0)
                first = contents; /* contents may be "" but that's ok */
            /* continue as we may still see build tags */
            break;
        }
        case TOKEN_PACKAGE:
        case TOKEN_EOF:
            return first;
        default:
            break;
        }
    }
}

typedef struct TestDir {
    TestingT *t;
    Alloc *a;
    Slice files; /* FsDirEntry */
    Str path;
    const char *const *ignore;
    size_t nignore;
} TestDir;

static bool ignored(const TestDir *d, Str name) {
    for (size_t i = 0; i < d->nignore; i++)
        if (str_eq(name, str_from_cstr(d->ignore[i])))
            return true;
    return false;
}

static void test_dir_body(void *env) {
    TestDir *d = env;
    Alloc *a = d->a;
    TokenFileSet *fset = token_new_file_set(a);
    for (Int i = 0; i < d->files.len; i++) {
        FsDirEntry *f = (FsDirEntry *)slice_at(d->files, i);
        Str name = f->vt->name(f->data);
        /* filter directory contents */
        if (f->vt->is_dir(f->data) || !strings_has_suffix(name, S(".go")) ||
            ignored(d, name))
            continue;

        /* get per-file instructions */
        bool expect_errors = false;
        Str filename = filepath_join_v(a, 2, d->path, name);
        Str go_version = S("");
        Str comment = first_comment(a, fset, filename);
        if (comment.len > 0) {
            if (strings_contains(comment, S("-goexperiment")))
                continue; /* ignore this file */
            Slice fields = strings_fields(a, comment);
            Str f0 = BURROW_AT(Str, fields, 0);
            if (str_eq(f0, S("skip")) || str_eq(f0, S("compiledir")))
                continue; /* ignore this file */
            if (str_eq(f0, S("errorcheck"))) {
                expect_errors = true;
                for (Int k = 1; k < fields.len; k++) {
                    Str arg = BURROW_AT(Str, fields, k);
                    if (str_eq(arg, S("-0")) || str_eq(arg, S("-+")) ||
                        str_eq(arg, S("-std"))) {
                        /* Marked explicitly as not expecting errors (-0), or
                         * marked as compiling runtime/stdlib, which is only
                         * done to trigger runtime/stdlib-only error output. In
                         * both cases, the code should typecheck. */
                        expect_errors = false;
                        break;
                    }
                    Str prefix = S("-lang=");
                    if (strings_has_prefix(arg, prefix))
                        go_version = (Str){arg.p + prefix.len, arg.len - prefix.len};
                }
            }
        }

        /* parse and type-check file */
        Error err = BURROW_NO_ERROR;
        AstFile *file = parser_parse_file(a, fset, filename, BURROW_ANY(NULL, NULL),
                                          PARSER_SKIP_OBJECT_RESOLUTION, &err);
        if (BURROW_OK(err)) {
            TypesConfig conf = {0};
            conf.go_version = go_version;
            /* Not in Go: stdLibImporter is the shared source importer from
             * go_types_importer.h. */
            conf.importer = default_importer(NULL);
            Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &file, 1);
            (void)types_config_check(&conf, a, filename, fset, files, NULL, &err);
        }

        if (expect_errors) {
            if (BURROW_OK(err))
                testing_t_errorf_v(d->t, "expected errors but found none in %s",
                                   filename);
        } else {
            if (BURROW_FAILED(err))
                testing_t_error_v(d->t, error_text(err));
        }
    }
}

static void test_test_dir(TestingT *t, Str path, const char *const *ignore,
                          size_t nignore) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    testing_t_cleanup(t, BURROW_FN(Func, free_arena, &ar));
    Str goroot = must_have_goroot(t, a);
    path = filepath_join_v(a, 2, goroot, filepath_from_slash(a, path));

    Error err = BURROW_NO_ERROR;
    Slice files = os_read_dir(a, path, &err);
    if (BURROW_FAILED(err)) {
        /* cmd/distpack deletes GOROOT/test, so skip the test if it isn't
         * present. cmd/distpack also requires GOROOT/VERSION to exist, so use
         * that to suppress false-positive skips. */
        Error e = BURROW_NO_ERROR;
        (void)os_stat(a, filepath_join_v(a, 2, goroot, S("test")), &e);
        if (errors_is(e, os_err_not_exist)) {
            e = BURROW_NO_ERROR;
            (void)os_stat(a, filepath_join_v(a, 2, goroot, S("VERSION")), &e);
            if (BURROW_OK(e))
                testing_t_skipf_v(t, "skipping: GOROOT/test not present");
        }
        testing_t_fatal_v(t, error_text(err));
    }

    TestDir d = {t, a, files, path, ignore, nignore};
    run_deep(t, test_dir_body, &d);
}

/* Not in Go: testenv.Builder() is "", since burrow's tests never run on Go's
 * builders. */
static void TestStdTest(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping in short mode");

    static const char *const ignore[] = {
        "cmplxdivide.go", /* also needs file cmplxdivide1.go - ignore */
        "directive.go", /* tests compiler rejection of bad directive placement - ignore */
        "directive2.go", /* tests compiler rejection of bad directive placement - ignore */
        "embedfunc.go", /* tests //go:embed */
        "embedvers.go", /* tests //go:embed */
        "linkname2.go", /* go/types doesn't check validity of //go:xxx directives */
        "linkname3.go", /* go/types doesn't check validity of //go:xxx directives */
    };
    test_test_dir(t, S("test"), ignore, sizeof ignore / sizeof ignore[0]);
}

static void TestStdFixed(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping in short mode");

    static const char *const ignore[] = {
        "bug248.go",
        "bug302.go",
        "bug369.go",     /* complex test instructions - ignore */
        "bug398.go",     /* go/types doesn't check for anonymous interface cycles
                             (go.dev/issue/56103) */
        "issue6889.go",  /* gc-specific test */
        "issue11362.go", /* canonical import path check */
        "issue16369.go", /* go/types handles this correctly - not an issue */
        "issue18459.go", /* go/types doesn't check validity of //go:xxx directives */
        "issue18882.go", /* go/types doesn't check validity of //go:xxx directives */
        "issue20027.go", /* go/types does not have constraints on channel element size */
        "issue20529.go",  /* go/types does not have constraints on stack size */
        "issue22200.go",  /* go/types does not have constraints on stack size */
        "issue22200b.go", /* go/types does not have constraints on stack size */
        "issue25507.go",  /* go/types does not have constraints on stack size */
        "issue20780.go",  /* go/types does not have constraints on stack size */
        "bug251.go",      /* go.dev/issue/34333 which was exposed with fix for
                             go.dev/issue/34151 */
        "issue42058a.go", /* go/types does not have constraints on channel element size */
        "issue42058b.go", /* go/types does not have constraints on channel element size */
        "issue48097.go", /* go/types doesn't check validity of //go:xxx directives, and
                             non-init bodyless function */
        "issue48230.go", /* go/types doesn't check validity of //go:xxx directives */
        "issue49767.go", /* go/types does not have constraints on channel element size */
        "issue49814.go", /* go/types does not have constraints on array size */
        "issue78355.go", /* go/types does not have constraints on map element size */
        "issue56103.go", /* anonymous interface cycles; will be a type checker error in
                             1.22 */
        "issue52697.go", /* go/types does not have constraints on stack size */

        /* These tests requires runtime/cgo.Incomplete, which is only available on
         * some platforms. However, go/types does not know about build
         * constraints. */
        "bug514.go",
        "issue40954.go",
        "issue42032.go",
        "issue42076.go",
        "issue46903.go",
        "issue51733.go",
        "notinheap2.go",
        "notinheap3.go",
    };
    test_test_dir(t, S("test/fixedbugs"), ignore, sizeof ignore / sizeof ignore[0]);
}

static void TestStdKen(TestingT *t) {
    test_test_dir(t, S("test/ken"), NULL, 0);
}

#define TESTS(X)                                                                       \
    X(TestStdlib)                                                                      \
    X(TestStdTest)                                                                     \
    X(TestStdFixed)                                                                    \
    X(TestStdKen)

TESTING_MAIN(TESTS)
