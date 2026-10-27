/* Derived from Go's src/go/internal/gcimporter/gcimporter_test.go.
 * Go source: go1.27.1.
 *
 * The tests compile the files in testdata with go tool compile and import what
 * it writes, so each one is skipped when there is no go command, which is
 * what testenv.MustHaveGoBuild does for Go's. The Go tree has to be go1.27,
 * since the testdata and the export data format are go1.27's.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_importer.h"

#include "../src/go/gcimporter.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/go/ast.h"
#include "burrow/go/importer.h"
#include "burrow/go/parser.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os/exec.h"
#include "burrow/path.h"
#include "burrow/strings.h"
#include "burrow/sync.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#define S(lit) BURROW_S(lit)
#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static Arena ar;
static Alloc *a;
/* Not in Go: where the answers of go list are kept. Go keeps them in a
 * package variable, and here each test has its own. */
static Map *exports;

static void gct_free_arena(void *env) {
    arena_free((Arena *)env);
}

/* A fresh arena for the test, freed after its other cleanups. */
static void gct_begin(TestingT *t) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    exports = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    testing_t_cleanup(t, BURROW_FN(Func, gct_free_arena, &ar));
}

/* What a subtest had before gct_scope gave it an arena of its own. */
typedef struct GctScope {
    Arena ar;
    Alloc *a;
    Map *exports;
} GctScope;

static void gct_scope_end(void *env) {
    GctScope *s = (GctScope *)env;
    a = s->a;
    exports = s->exports;
    arena_free(&s->ar);
}

/* Not in Go: the subtest gets an arena of its own, so what it imports is let
 * go once it is done and not kept until the whole test is. */
static void gct_scope(TestingT *t) {
    GctScope *s = BURROW_NEW(a, GctScope);
    s->a = a;
    s->exports = exports;
    arena_init(&s->ar, NULL, 0);
    a = arena_allocator(&s->ar);
    exports = NULL;
    testing_t_cleanup(t, BURROW_FN(Func, gct_scope_end, s));
}

static void gct_chdir_back(void *env) {
    (void)os_chdir(*(Str *)env);
}

static void gct_remove_all(void *env) {
    (void)os_remove_all(*(Str *)env);
}

/* defer os.RemoveAll(dir) */
static void gct_remove_all_later(TestingT *t, Str dir) {
    Str *p = BURROW_NEW(a, Str);
    *p = dir;
    testing_t_cleanup(t, BURROW_FN(Func, gct_remove_all, p));
}

/* TestMain, testenv.MustHaveGoBuild and testenv.GOROOT(t): the Go root, with
 * the test skipped unless it has a go command.
 *
 * Not in Go: TestMain sets build.Default.GOROOT, which build_default reads
 * from $GOROOT, so that is set. Go runs a package's tests in its directory,
 * which is where testdata is, so the test goes there, and back once it is
 * done. */
static Str gct_goroot(TestingT *t) {
    SKIP_WITHOUT_EXEC(t);
    Str goroot = gtt_find_goroot(a);
    if (goroot.len == 0) {
        testing_t_skip_v(t, "no GOROOT and no go command to ask for one");
        return goroot;
    }
    Str version;
    if (!gtt_goroot_is_go127(a, goroot, &version)) {
        testing_t_skipf_v(t, "%s has %q, and these tests are go1.27's", goroot,
                          version);
        return BURROW_STR_EMPTY;
    }
    Error e = BURROW_NO_ERROR;
    (void)exec_look_path(a, filepath_join_v(a, 3, goroot, S("bin"), S("go")), &e);
    if (BURROW_FAILED(e)) {
        testing_t_skipf_v(t, "no go command in %s", goroot);
        return BURROW_STR_EMPTY;
    }
    e = os_setenv(S("GOROOT"), goroot);
    if (BURROW_FAILED(e)) {
        testing_t_fatal_v(t, e);
        return BURROW_STR_EMPTY;
    }
    Str *wd = BURROW_NEW(a, Str);
    *wd = os_getwd(a, &e);
    if (BURROW_FAILED(e)) {
        testing_t_fatal_v(t, e);
        return BURROW_STR_EMPTY;
    }
    e = os_chdir(filepath_join_v(a, 5, goroot, S("src"), S("go"), S("internal"),
                                 S("gcimporter")));
    if (BURROW_FAILED(e)) {
        testing_t_fatal_v(t, e);
        return BURROW_STR_EMPTY;
    }
    testing_t_cleanup(t, BURROW_FN(Func, gct_chdir_back, wd));
    return goroot;
}

/* testenv.GoToolPath */
static Str go_tool_path(void) {
    return filepath_join_v(a, 3, build_default(a).goroot, S("bin"), S("go"));
}

static Str gct_str(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

static Slice gct_bytes(Str s) {
    return slice_from_str(a, s);
}

/* An entry of the packageFiles map that compile and WriteImportcfg take. */
typedef struct GctPackageFile {
    Str path;
    Str file;
} GctPackageFile;

/* testenv.WriteImportcfg */
static void write_importcfg(TestingT *t, Str dst_path,
                            const GctPackageFile *package_files, Int npackage_files,
                            Slice pkgs) {
    StringsBuilder icfg = STRINGS_BUILDER(a);
    (void)strings_builder_write_string(&icfg, S("# import config\n"), NULL);
    for (Int i = 0; i < npackage_files; i++)
        (void)strings_builder_write_string(&icfg,
                                           fmt_sprintf_v(a, "packagefile %s=%s\n",
                                                         package_files[i].path,
                                                         package_files[i].file),
                                           NULL);

    if (pkgs.len > 0) {
        /* Use 'go list' to resolve any missing packages and rewrite the import
         * map. */
        Slice args = slice_nil(TYPE_STRING);
        static const Str list_args[] = {
            S_("list"),
            S_("-export"),
            S_("-deps"),
            S_("-f"),
            S_("{{if ne .ImportPath \"command-line-arguments\"}}{{if "
               ".Export}}{{.ImportPath}}={{.Export}}{{end}}{{end}}"),
        };
        args = slice_append(a, args, list_args, NELEM(list_args));
        args = slice_append(a, args, pkgs.p, pkgs.len);
        ExecCmd *cmd = exec_command(a, go_tool_path(), args);
        StringsBuilder stderr_b = STRINGS_BUILDER(a);
        cmd->stderr_ = strings_builder_as_io_writer(&stderr_b);
        Error err = BURROW_NO_ERROR;
        Slice out = exec_cmd_output(cmd, a, &err);
        Str cmd_string = exec_cmd_string(cmd, a);
        exec_cmd_free(cmd);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "%v: %v\n%s", cmd_string, err,
                               strings_builder_string(&stderr_b));
            return;
        }

        Slice lines = strings_split(a, gct_str(out), S("\n"));
        for (Int i = 0; i < lines.len; i++) {
            Str line = BURROW_AT(Str, lines, i);
            if (line.len == 0)
                continue;
            Str export_;
            bool ok;
            Str import_path = strings_cut(line, S("="), &export_, &ok);
            if (!ok) {
                testing_t_fatalf_v(t, "invalid line in output from %v:\n%s", cmd_string,
                                   line);
                return;
            }
            bool listed = false;
            for (Int j = 0; j < npackage_files; j++)
                if (str_eq(package_files[j].path, import_path) &&
                    package_files[j].file.len > 0)
                    listed = true;
            if (!listed)
                (void)strings_builder_write_string(
                    &icfg,
                    fmt_sprintf_v(a, "packagefile %s=%s\n", import_path, export_),
                    NULL);
        }
    }

    Error err = os_write_file(dst_path, gct_bytes(strings_builder_string(&icfg)), 0666);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);
}

/* compile runs the compiler on filename, with dirname as the working
 * directory, and writes the output file to outdirname. compile gives the
 * resulting package a packagepath of testdata/<filebasename>. */
static Str compile(TestingT *t, Str dirname, Str filename, Str outdirname,
                   const GctPackageFile *package_files, Int npackage_files,
                   Slice pkg_imports) {
    /* filename must end with ".go" */
    bool ok;
    Str basename = strings_cut_suffix(filepath_base(filename), S(".go"), &ok);
    if (!ok) {
        testing_t_fatalf_v(t, "filename doesn't end in .go: %s", filename);
        return BURROW_STR_EMPTY;
    }
    Str objname = fmt_sprintf_v(a, "%s.o", basename);
    Str outname = filepath_join_v(a, 2, outdirname, objname);

    Str importcfgfile = S(OS_DEV_NULL);
    if (npackage_files > 0 || pkg_imports.len > 0) {
        importcfgfile = fmt_sprintf_v(a, "%s.importcfg",
                                      filepath_join_v(a, 2, outdirname, basename));
        write_importcfg(t, importcfgfile, package_files, npackage_files, pkg_imports);
    }

    Str pkgpath = path_join_v(a, 2, S("testdata"), basename);
    ExecCmd *cmd = exec_command_v(
        a, go_tool_path(), 11, S("tool"), S("compile"), S("-p"), pkgpath, S("-D"),
        S("testdata"), S("-importcfg"), importcfgfile, S("-o"), outname, filename);
    cmd->dir = dirname;
    Error err = BURROW_NO_ERROR;
    Slice out = exec_cmd_combined_output(cmd, a, &err);
    exec_cmd_free(cmd);
    if (BURROW_FAILED(err)) {
        testing_t_logf_v(t, "%s", gct_str(out));
        testing_t_fatalf_v(t, "go tool compile %s failed: %s", filename, err);
        return BURROW_STR_EMPTY;
    }
    return outname;
}

/* compile with no packageFiles and the pkgImports given. */
static Str compile_v(TestingT *t, Str dirname, Str filename, Str outdirname, int n,
                     ...) {
    Slice pkg_imports = slice_nil(TYPE_STRING);
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) {
        Str s = va_arg(ap, Str);
        pkg_imports = slice_append(a, pkg_imports, &s, 1);
    }
    va_end(ap);
    return compile(t, dirname, filename, outdirname, NULL, 0, pkg_imports);
}

static Map *new_packages(void) {
    return map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
}

/* Import from the gcimporter package. */
static TypesPackage *gct_import(TokenFileSet *fset, Map *packages, Str path,
                                Str src_dir, Error *err) {
    return burrow__gcimporter_import(a, fset, packages, path, src_dir,
                                     (ImporterLookup){0}, exports, err);
}

static TypesPackage *test_path(TestingT *t, Str path, Str src_dir) {
    Time t0 = time_now();
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg = gct_import(fset, new_packages(), path, src_dir, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "testPath(%s): %s", path, err);
        return NULL;
    }
    testing_t_logf_v(t, "testPath(%s): %s", path, duration_string(time_since(t0), a));
    return pkg;
}

static Str mktmpdir(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Str tmpdir = os_mkdir_temp(a, S(""), S("gcimporter_test"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, "mktmpdir:", err);
        return BURROW_STR_EMPTY;
    }
    err = os_mkdir(filepath_join_v(a, 2, tmpdir, S("testdata")), 0700);
    if (BURROW_FAILED(err)) {
        (void)os_remove_all(tmpdir);
        testing_t_fatal_v(t, "mktmpdir:", err);
        return BURROW_STR_EMPTY;
    }
    return tmpdir;
}

/* t.TempDir() */
static Str gct_temp_dir(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, S(""), S("gcimporter_test"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return BURROW_STR_EMPTY;
    }
    gct_remove_all_later(t, dir);
    return dir;
}

/* fmt.Sprint(pkg.Imports()) */
static Str gct_imports_string(TypesPackage *pkg) {
    Slice imports = types_package_imports(pkg);
    StringsBuilder b = STRINGS_BUILDER(a);
    (void)strings_builder_write_string(&b, S("["), NULL);
    for (Int i = 0; i < imports.len; i++) {
        if (i > 0)
            (void)strings_builder_write_string(&b, S(" "), NULL);
        (void)strings_builder_write_string(
            &b, types_package_string(a, BURROW_AT(TypesPackage *, imports, i)), NULL);
    }
    (void)strings_builder_write_string(&b, S("]"), NULL);
    return strings_builder_string(&b);
}

static void TestImportTestdata(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    static const Str exports_imports[] = {S_("go/ast")};
    static const struct {
        Str testfile;
        const Str *want_imports;
        Int nwant_imports;
    } testfiles[] = {
        {S_("exports.go"), exports_imports, NELEM(exports_imports)},
        {S_("generics.go"), NULL, 0},
    };

    for (Int k = 0; k < NELEM(testfiles); k++) {
        Str testfile = testfiles[k].testfile;
        Slice want_imports = slice_nil(TYPE_STRING);
        want_imports = slice_append(a, want_imports, testfiles[k].want_imports,
                                    testfiles[k].nwant_imports);

        Str tmpdir = mktmpdir(t);
        gct_remove_all_later(t, tmpdir);

        compile(t, S("testdata"), testfile,
                filepath_join_v(a, 2, tmpdir, S("testdata")), NULL, 0, want_imports);
        Str path =
            fmt_sprintf_v(a, "./testdata/%s", strings_trim_suffix(testfile, S(".go")));

        TypesPackage *pkg = test_path(t, path, tmpdir);
        if (pkg != NULL) {
            /* The package's Imports list must include all packages explicitly
             * imported by testfile, plus all packages referenced indirectly
             * via exported objects in testfile. */
            Str got = gct_imports_string(pkg);
            for (Int i = 0; i < want_imports.len; i++) {
                Str want = BURROW_AT(Str, want_imports, i);
                if (!strings_contains(got, want))
                    testing_t_errorf_v(
                        t, "Package(\"exports\").Imports() = %s, does not contain %s",
                        got, want);
            }
        }
    }
}

static TypesPackage *import_pkg(TestingT *t, Str path, Str src_dir);
static TypesPackage *check_file(TestingT *t, Str filename, Slice src);

/* sanitizeObjectString removes type parameter debugging markers from an
 * object string, to normalize it for comparison.
 * TODO(rfindley): this should not be necessary. */
static Str sanitize_object_string(Str s) {
    StringsBuilder b = STRINGS_BUILDER(a);
    Int start = 0;
    for (Int i = 0; i < s.len; i++) {
        /* '₀' through '₉' are U+2080 through U+2089. */
        if (i + 2 < s.len && s.p[i] == 0xE2 && s.p[i + 1] == 0x82 &&
            s.p[i + 2] >= 0x80 && s.p[i + 2] <= 0x89) {
            /* trim type parameter subscripts */
            (void)strings_builder_write_string(
                &b, str_from_bytes(s.p + start, i - start), NULL);
            i += 2;
            start = i + 1;
        }
    }
    (void)strings_builder_write_string(&b, str_from_bytes(s.p + start, s.len - start),
                                       NULL);
    return strings_builder_string(&b);
}

typedef struct TypeparamCase {
    Str root_dir;
    Str name;
    Str tmpdir;
} TypeparamCase;

static void typeparam_case(void *env, TestingT *t) {
    const TypeparamCase *c = (const TypeparamCase *)env;
    gct_scope(t);
    Str filename = filepath_join_v(a, 2, c->root_dir, c->name);
    Error err = BURROW_NO_ERROR;
    Slice src = os_read_file(a, filename, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }
    if (!bytes_has_prefix(src, gct_bytes(S("// run"))) &&
        !bytes_has_prefix(src, gct_bytes(S("// compile")))) {
        /* We're bypassing the logic of run.go here, so be conservative about
         * the files we consider in an attempt to make this test more robust to
         * changes in test/typeparams. */
        testing_t_skipf_v(t, "not detected as a run test");
        return;
    }

    /* Compile and import, and compare the resulting package with the package
     * that was type-checked directly. */
    compile_v(t, c->root_dir, c->name, filepath_join_v(a, 2, c->tmpdir, S("testdata")),
              1, filename);
    Str pkg_name = strings_trim_suffix(c->name, S(".go"));
    TypesPackage *imported =
        import_pkg(t, fmt_sprintf_v(a, "./testdata/%s", pkg_name), c->tmpdir);
    TypesPackage *checked = check_file(t, filename, src);

    Map *seen = map_make(a, TYPE_STRING, TYPE_BOOL, 0);
    Slice names = types_scope_names(a, types_package_scope(imported));
    for (Int i = 0; i < names.len; i++) {
        Str name = BURROW_AT(Str, names, i);
        if (!token_is_exported(name))
            continue; /* ignore synthetic names like .inittask and .dict.* */
        bool yes = true;
        map_set(seen, &name, &yes);

        TypesObject imported_obj =
            types_scope_lookup(types_package_scope(imported), name);
        Str got = types_object_string(a, imported_obj, types_relative_to(imported));
        got = sanitize_object_string(got);

        TypesObject checked_obj =
            types_scope_lookup(types_package_scope(checked), name);
        if (checked_obj == NULL) {
            testing_t_fatalf_v(t, "imported object %q was not type-checked", name);
            return;
        }
        Str want = types_object_string(a, checked_obj, types_relative_to(checked));
        want = sanitize_object_string(want);

        if (!str_eq(got, want))
            testing_t_errorf_v(t, "imported %q as %q, want %q", name, got, want);
    }

    names = types_scope_names(a, types_package_scope(checked));
    for (Int i = 0; i < names.len; i++) {
        Str name = BURROW_AT(Str, names, i);
        if (!token_is_exported(name) || map_get(seen, &name) != NULL)
            continue;
        testing_t_errorf_v(t, "did not import object %q", name);
    }
}

static void TestImportTypeparamTests(TestingT *t) {
    gct_begin(t);
    if (testing_short()) {
        testing_t_skipf_v(t,
                          "in short mode, skipping test that requires export data for "
                          "all of std");
        return;
    }
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    /* cmd/distpack removes the GOROOT/test directory, so skip if it isn't
     * there. cmd/distpack also requires the presence of GOROOT/VERSION, so use
     * that to avoid false-positive skips. */
    Str goroot_test = filepath_join_v(a, 2, goroot, S("test"));
    Error err = BURROW_NO_ERROR;
    (void)os_stat(a, goroot_test, &err);
    if (os_is_not_exist(err)) {
        Error verr = BURROW_NO_ERROR;
        (void)os_stat(a, filepath_join_v(a, 2, goroot, S("VERSION")), &verr);
        if (BURROW_OK(verr)) {
            testing_t_skipf_v(t, "skipping: GOROOT/test not present");
            return;
        }
    }

    Str tmpdir = mktmpdir(t);
    gct_remove_all_later(t, tmpdir);

    /* Check go files in test/typeparam, except those that fail for a known
     * reason. */
    Str root_dir = filepath_join_v(a, 2, goroot_test, S("typeparam"));
    err = BURROW_NO_ERROR;
    Slice list = os_read_dir(a, root_dir, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }

    for (Int i = 0; i < list.len; i++) {
        FsDirEntry *entry = (FsDirEntry *)slice_at(list, i);
        Str name = entry->vt->name(entry->data);
        if (entry->vt->is_dir(entry->data) || !strings_has_suffix(name, S(".go")))
            /* For now, only consider standalone go files. */
            continue;

        TypeparamCase *c = BURROW_NEW(a, TypeparamCase);
        *c = (TypeparamCase){root_dir, name, tmpdir};
        testing_t_run(t, name, BURROW_FN(TestingTFunc, typeparam_case, c));
    }
}

static TypesPackage *check_file(TestingT *t, Str filename, Slice src) {
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    Str s = gct_str(src);
    AstFile *f = parser_parse_file(a, fset, filename, BURROW_ANY(TYPE_STRING, &s),
                                   PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return NULL;
    }
    TypesConfig config = {0};
    config.importer = importer_default(a);
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    TypesPackage *pkg = types_config_check(&config, a, S(""), fset, files, NULL, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return NULL;
    }
    return pkg;
}

static void TestVersionHandling(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    Str dir = S("./testdata/versions");
    Error err = BURROW_NO_ERROR;
    Slice list = os_read_dir(a, dir, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }

    Str tmpdir = mktmpdir(t);
    gct_remove_all_later(t, tmpdir);
    Str corruptdir = filepath_join_v(a, 3, tmpdir, S("testdata"), S("versions"));
    err = os_mkdir(corruptdir, 0700);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }

    TokenFileSet *fset = token_new_file_set(a);

    for (Int k = 0; k < list.len; k++) {
        FsDirEntry *f = (FsDirEntry *)slice_at(list, k);
        Str name = f->vt->name(f->data);
        if (!strings_has_suffix(name, S(".a")))
            continue; /* not a package file */
        if (strings_contains(name, S("corrupted")))
            continue; /* don't process a leftover corrupted file */
        Str pkgpath = fmt_sprintf_v(a, "./%s", str_from_bytes(name.p, name.len - 2));

        if (testing_verbose())
            testing_t_logf_v(t, "importing %s", name);

        /* test that export data can be imported */
        err = BURROW_NO_ERROR;
        (void)gct_import(fset, new_packages(), pkgpath, dir, &err);
        if (BURROW_FAILED(err)) {
            Str msg = error_text(err);
            /* ok to fail if it fails with a 'not the start of an archive file'
             * error for select files */
            if (strings_contains(msg, S("not the start of an archive file"))) {
                if (str_eq(name, S("test_go1.8_4.a")) ||
                    str_eq(name, S("test_go1.8_5.a")))
                    continue;
                /* fall through */
            }
            /* ok to fail if it fails with a 'no longer supported' error for
             * select files */
            if (strings_contains(msg, S("no longer supported"))) {
                static const Str ok_names[] = {
                    S_("test_go1.7_0.a"),     S_("test_go1.7_1.a"),
                    S_("test_go1.8_4.a"),     S_("test_go1.8_5.a"),
                    S_("test_go1.11_0i.a"),   S_("test_go1.11_6b.a"),
                    S_("test_go1.11_999b.a"), S_("test_go1.11_999i.a"),
                };
                bool ok = false;
                for (Int i = 0; i < NELEM(ok_names); i++)
                    if (str_eq(name, ok_names[i]))
                        ok = true;
                if (ok)
                    continue;
                /* fall through */
            }
            /* ok to fail if it fails with a 'newer version' error for select
             * files */
            if (strings_contains(msg, S("newer version"))) {
                if (str_eq(name, S("test_go1.11_999i.a")))
                    continue;
                /* fall through */
            }
            testing_t_errorf_v(t, "import %q failed: %v", pkgpath, err);
            continue;
        }

        /* create file with corrupted export data */
        /* 1) read file */
        err = BURROW_NO_ERROR;
        Slice data = os_read_file(a, filepath_join_v(a, 2, dir, name), &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatal_v(t, err);
            return;
        }
        Byte *d = (Byte *)data.p;
        /* 2) find export data */
        Int i = bytes_index(data, gct_bytes(S("\n$$B\n"))) + 5;
        /* Export data can contain "\n$$\n" in string constants, however,
         * searching for the next end of section marker "\n$$\n" is good enough
         * for tests. */
        Int j = -1;
        if (i >= 0 && i <= data.len)
            j = bytes_index(slice_from(d + i, data.len - i, data.len - i, TYPE_BYTE),
                            gct_bytes(S("\n$$\n"))) +
                i;
        if (i < 0 || j < 0 || i > j) {
            testing_t_fatalf_v(t, "export data section not found (i = %d, j = %d)", i,
                               j);
            return;
        }
        /* 3) corrupt the data (increment every 7th byte) */
        for (Int k2 = j - 13; k2 >= i; k2 -= 7)
            d[k2]++;
        /* 4) write the file */
        pkgpath = fmt_sprintf_v(a, "%s_corrupted", pkgpath);
        Str filename =
            fmt_sprintf_v(a, "%s.a", filepath_join_v(a, 2, corruptdir, pkgpath));
        (void)os_write_file(filename, data, 0666);

        /* test that importing the corrupted file results in an error */
        err = BURROW_NO_ERROR;
        (void)gct_import(fset, new_packages(), pkgpath, corruptdir, &err);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "import corrupted %q succeeded", pkgpath);
        } else {
            Str msg = error_text(err);
            if (!strings_contains(msg, S("version skew")))
                testing_t_errorf_v(t, "import %q error incorrect (%s)", pkgpath, msg);
        }
    }
}

typedef struct StdLibCase {
    Str pkg;
    Str goroot;
    Int *nimports;
} StdLibCase;

static void std_lib_case(void *env, TestingT *t) {
    const StdLibCase *c = (const StdLibCase *)env;
    gct_scope(t);
    if (test_path(t, c->pkg,
                  filepath_join_v(a, 3, c->goroot, S("src"), path_dir(a, c->pkg))) !=
        NULL)
        (*c->nimports)++;
}

static void TestImportStdLib(TestingT *t) {
    gct_begin(t);
    if (testing_short()) {
        testing_t_skip_v(t, "the imports can be expensive, and this test is especially "
                            "slow when the build cache is empty");
        return;
    }
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    /* Get list of packages in stdlib. Filter out test-only packages with
     * {{if .GoFiles}} check. */
    StringsBuilder stderr_b = STRINGS_BUILDER(a);
    ExecCmd *cmd = exec_command_v(a, S("go"), 4, S("list"), S("-f"),
                                  S("{{if .GoFiles}}{{.ImportPath}}{{end}}"), S("std"));
    cmd->stderr_ = strings_builder_as_io_writer(&stderr_b);
    Error err = BURROW_NO_ERROR;
    Slice out = exec_cmd_output(cmd, a, &err);
    exec_cmd_free(cmd);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(
            t, "failed to run go list to determine stdlib packages: %v\nstderr:\n%v",
            err, strings_builder_string(&stderr_b));
        return;
    }
    Slice pkgs = strings_fields(a, gct_str(out));

    Int nimports = 0;
    for (Int i = 0; i < pkgs.len; i++) {
        StdLibCase *c = BURROW_NEW(a, StdLibCase);
        *c = (StdLibCase){BURROW_AT(Str, pkgs, i), goroot, &nimports};
        testing_t_run(t, c->pkg, BURROW_FN(TestingTFunc, std_lib_case, c));
    }
    /* 'GOOS=plan9 go1.18 list std | wc -l' reports 228; most other platforms
     * have more. */
    const Int min_pkgs = 225;
    if (pkgs.len < min_pkgs) {
        testing_t_fatalf_v(t, "too few packages (%d) were imported", nimports);
        return;
    }

    testing_t_logf_v(t, "tested %d imports", nimports);
}

typedef struct ImportedObjectTest {
    Str name;
    Str want;
} ImportedObjectTest;

static const ImportedObjectTest imported_object_tests[] = {
    /* non-interfaces */
    {S_("crypto.Hash"), S_("type Hash uint")},
    {S_("go/ast.ObjKind"), S_("type ObjKind int")},
    {S_("go/types.Qualifier"), S_("type Qualifier func(*Package) string")},
    {S_("go/types.Comparable"), S_("func Comparable(T Type) bool")},
    {S_("math.Pi"), S_("const Pi untyped float")},
    {S_("math.Sin"), S_("func Sin(x float64) float64")},
    {S_("go/ast.NotNilFilter"),
     S_("func NotNilFilter(_ string, v reflect.Value) bool")},
    {S_("internal/exportdata.FindPkg"),
     S_("func FindPkg(path string, srcDir string) (filename string, id string, err "
        "error)")},

    /* interfaces */
    {S_("context.Context"), S_("type Context interface{Deadline() (deadline time.Time, "
                               "ok bool); Done() <-chan "
                               "struct{}; Err() error; Value(key any) any}")},
    {S_("crypto.Decrypter"),
     S_("type Decrypter interface{Decrypt(rand io.Reader, msg []byte, opts "
        "DecrypterOpts) (plaintext []byte, err error); Public() PublicKey}")},
    {S_("encoding.BinaryMarshaler"),
     S_("type BinaryMarshaler interface{MarshalBinary() (data []byte, err error)}")},
    {S_("io.Reader"), S_("type Reader interface{Read(p []byte) (n int, err error)}")},
    {S_("io.ReadWriter"), S_("type ReadWriter interface{Reader; Writer}")},
    {S_("go/ast.Node"),
     S_("type Node interface{End() go/token.Pos; Pos() go/token.Pos}")},
    {S_("go/types.Type"),
     S_("type Type interface{String() string; Underlying() Type}")},
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

    /* check embedded interfaces (if they are named, too) */
    for (Int i = 0; i < types_interface_num_embeddeds(iface); i++) {
        /* embedding of interfaces cannot have cycles; recursion will terminate */
        TypesNamed *etype = types_as_named(types_interface_embedded_type(iface, i));
        if (etype != NULL)
            verify_interface_method_recvs(t, etype, level + 1);
    }
}

static void TestImportedTypes(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    TokenFileSet *fset = token_new_file_set(a);
    for (Int k = 0; k < NELEM(imported_object_tests); k++) {
        const ImportedObjectTest *test = &imported_object_tests[k];
        Slice s = strings_split(a, test->name, S("."));
        if (s.len != 2) {
            testing_t_fatal_v(t, "inconsistent test data");
            return;
        }
        Str import_path = BURROW_AT(Str, s, 0);
        Str obj_name = BURROW_AT(Str, s, 1);

        Error err = BURROW_NO_ERROR;
        TypesPackage *pkg = gct_import(fset, new_packages(), import_path, S("."), &err);
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

static void TestIssue5815(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    TypesPackage *pkg = import_pkg(t, S("strings"), S("."));

    TypesScope *scope = types_package_scope(pkg);
    Slice names = types_scope_names(a, scope);
    for (Int i = 0; i < names.len; i++) {
        TypesObject obj = types_scope_lookup(scope, BURROW_AT(Str, names, i));
        if (types_object_pkg(obj) == NULL)
            testing_t_errorf_v(t, "no pkg for %s",
                               types_object_string(a, obj, TYPES_NO_QUALIFIER));
        TypesTypeName *tname = types_as_type_name(obj);
        if (tname != NULL) {
            TypesNamed *named = types_as_named(types_object_type(types_object(tname)));
            for (Int j = 0; j < types_named_num_methods(named); j++) {
                TypesFunc *m = types_named_method(named, j);
                if (types_object_pkg(types_object(m)) == NULL)
                    testing_t_errorf_v(
                        t, "no pkg for %s",
                        types_object_string(a, types_object(m), TYPES_NO_QUALIFIER));
            }
        }
    }
}

/* Smoke test to ensure that imported methods get the correct package. */
static void TestCorrectMethodPackage(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    Map *imports = new_packages();
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    (void)gct_import(fset, imports, S("net/http"), S("."), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }

    Str sync_path = S("sync");
    TypesPackage **sync_pkg = map_get(imports, &sync_path);
    TypesType mutex = types_object_type(types_object(types_as_type_name(
        types_scope_lookup(types_package_scope(*sync_pkg), S("Mutex")))));
    /* methods of *sync.Mutex */
    TypesMethodSet *mset =
        types_new_method_set(a, types_type(types_new_pointer(a, mutex)));
    TypesSelection *sel = types_method_set_lookup(mset, NULL, S("Lock"));
    TypesFunc *lock = types_as_func(types_selection_obj(sel));
    Str got = types_package_path(types_object_pkg(types_object(lock)));
    Str want = S("sync");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got package path %q; want %q", got, want);
}

static void TestIssue13566(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    Str tmpdir = mktmpdir(t);
    gct_remove_all_later(t, tmpdir);
    Str testoutdir = filepath_join_v(a, 2, tmpdir, S("testdata"));

    /* b.go needs to be compiled from the output directory so that the compiler
     * can find the compiled package a. We pass the full path to compile() so
     * that we don't have to copy the file to that directory. */
    Error err = BURROW_NO_ERROR;
    Str bpath = filepath_abs(a, filepath_join_v(a, 2, S("testdata"), S("b.go")), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }

    compile_v(t, S("testdata"), S("a.go"), testoutdir, 1, S("encoding/json"));
    GctPackageFile files[] = {
        {S("testdata/a"), filepath_join_v(a, 2, testoutdir, S("a.o"))},
    };
    Slice json = slice_nil(TYPE_STRING);
    Str json_path = S("encoding/json");
    json = slice_append(a, json, &json_path, 1);
    compile(t, testoutdir, bpath, testoutdir, files, NELEM(files), json);

    /* import must succeed (test for issue at hand) */
    TypesPackage *pkg = import_pkg(t, S("./testdata/b"), tmpdir);

    /* make sure all indirectly imported packages have names */
    Slice imports = types_package_imports(pkg);
    for (Int i = 0; i < imports.len; i++) {
        TypesPackage *imp = BURROW_AT(TypesPackage *, imports, i);
        if (types_package_name(imp).len == 0)
            testing_t_errorf_v(t, "no name for %s package", types_package_path(imp));
    }
}

static void TestTypeNamingOrder(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    Str tmpdir = mktmpdir(t);
    gct_remove_all_later(t, tmpdir);
    Str testoutdir = filepath_join_v(a, 2, tmpdir, S("testdata"));

    compile_v(t, S("testdata"), S("g.go"), testoutdir, 0);

    /* import must succeed (test for issue at hand) */
    (void)import_pkg(t, S("./testdata/g"), tmpdir);
}

static TypesObject lookup_obj(TestingT *t, TypesScope *scope, Str name);

static void TestIssue13898(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    /* import go/internal/gcimporter which imports go/types partially */
    TokenFileSet *fset = token_new_file_set(a);
    Map *imports = new_packages();
    Error err = BURROW_NO_ERROR;
    (void)gct_import(fset, imports, S("go/internal/gcimporter"), S("."), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }

    /* look for go/types package */
    Str types_path = S("go/types");
    TypesPackage **found = map_get(imports, &types_path);
    TypesPackage *go_types_pkg = found != NULL ? *found : NULL;
    if (go_types_pkg == NULL) {
        testing_t_fatal_v(t, "go/types not found");
        return;
    }

    /* look for go/types.Object type */
    TypesObject obj = lookup_obj(t, types_package_scope(go_types_pkg), S("Object"));
    TypesNamed *typ = types_as_named(types_object_type(obj));
    if (typ == NULL) {
        testing_t_fatalf_v(
            t, "go/types.Object type is %v; wanted named type",
            types_type_string(a, types_object_type(obj), TYPES_NO_QUALIFIER));
        return;
    }

    /* lookup go/types.Object.Pkg method */
    TypesSelection *sel =
        types_lookup_selection(a, types_type(typ), false, NULL, S("Pkg"));
    if (sel == NULL) {
        testing_t_fatalf_v(t, "go/types.Object.Pkg not found");
        return;
    }

    /* the method must belong to go/types */
    TypesPackage *pkg = types_object_pkg(types_selection_obj(sel));
    if (!str_eq(types_package_path(pkg), S("go/types")))
        testing_t_fatalf_v(t, "found %v; want go/types", types_package_string(a, pkg));
}

static void TestIssue15517(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    Str tmpdir = mktmpdir(t);
    gct_remove_all_later(t, tmpdir);

    compile_v(t, S("testdata"), S("p.go"), filepath_join_v(a, 2, tmpdir, S("testdata")),
              0);

    /* Multiple imports of p must succeed without redeclaration errors. We use
     * an import path that's not cleaned up so that the eventual file path for
     * the package is different from the package path; this will expose the
     * error if it is present.
     *
     * (Issue: Both the textual and the binary importer used the file path of
     * the package to be imported as key into the shared packages map.
     * However, the binary importer then used the package path to identify the
     * imported package to mark it as complete; effectively marking the wrong
     * package as complete. By using an "unclean" package path, the file and
     * package path are different, exposing the problem if present. The same
     * issue occurs with vendoring.) */
    Map *imports = new_packages();
    TokenFileSet *fset = token_new_file_set(a);
    for (Int i = 0; i < 3; i++) {
        Error err = BURROW_NO_ERROR;
        (void)gct_import(fset, imports, S("./././testdata/p"), tmpdir, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatal_v(t, err);
            return;
        }
    }
}

static TypesPackage *compile_and_import_pkg(TestingT *t, Str name);

static void TestIssue15920(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    (void)compile_and_import_pkg(t, S("issue15920"));
}

static void TestIssue20046(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    /* "./issue20046".V.M must exist */
    TypesPackage *pkg = compile_and_import_pkg(t, S("issue20046"));
    TypesObject obj = lookup_obj(t, types_package_scope(pkg), S("V"));
    if (types_lookup_selection(a, types_object_type(obj), false, NULL, S("M")) == NULL)
        testing_t_fatalf_v(t, "V.M not found");
}

static void TestIssue25301(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    (void)compile_and_import_pkg(t, S("issue25301"));
}

static void TestIssue25596(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    (void)compile_and_import_pkg(t, S("issue25596"));
}

static void TestIssue57015(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    (void)compile_and_import_pkg(t, S("issue57015"));
}

static TypesPackage *import_pkg(TestingT *t, Str path, Str src_dir) {
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg = gct_import(fset, new_packages(), path, src_dir, &err);
    if (BURROW_FAILED(err)) {
        testing_t_helper(t);
        testing_t_fatal_v(t, err);
        return NULL;
    }
    return pkg;
}

static TypesPackage *compile_and_import_pkg(TestingT *t, Str name) {
    testing_t_helper(t);
    Str tmpdir = mktmpdir(t);
    gct_remove_all_later(t, tmpdir);
    compile_v(t, S("testdata"), fmt_sprintf_v(a, "%s.go", name),
              filepath_join_v(a, 2, tmpdir, S("testdata")), 0);
    return import_pkg(t, fmt_sprintf_v(a, "./testdata/%s", name), tmpdir);
}

static TypesObject lookup_obj(TestingT *t, TypesScope *scope, Str name) {
    TypesObject obj = types_scope_lookup(scope, name);
    if (obj != NULL)
        return obj;
    testing_t_helper(t);
    testing_t_fatalf_v(t, "%s not found", name);
    return NULL;
}

/* importMap implements the types.Importer interface. */
static TypesPackage *import_map_import(void *self, Str path, Error *err) {
    (void)err;
    TypesPackage **p = map_get((Map *)self, &path);
    return p != NULL ? *p : NULL;
}

static const TypesImporterVT import_map_vt = {NULL, import_map_import, NULL};

/* The check closure of TestIssue69912 and TestGenMeth.
 *
 * Not in Go: the package is checked in an allocator of the caller's, so that
 * each thread can use its own. */
static TypesPackage *gct_check(Alloc *ca, TokenFileSet *fset, Str filename, Str pkgname,
                               Str src, Map *imports, Error *err) {
    AstFile *f = parser_parse_file(ca, fset, filename, BURROW_ANY(TYPE_STRING, &src),
                                   PARSER_SKIP_OBJECT_RESOLUTION, err);
    if (BURROW_FAILED(*err))
        return NULL;
    TypesConfig config = {0};
    config.importer = (TypesImporter){&import_map_vt, imports};
    Slice files = slice_append(ca, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    return types_config_check(&config, ca, pkgname, fset, files, NULL, err);
}

/* Use the resulting package concurrently, via dot-imports, to exercise the
 * race of issue #69912. */
static const Str issue69912_p_src = S_("package p\n"
                                       "\n"
                                       "import . \"issue69912\"\n"
                                       "\n"
                                       "type S struct {\n"
                                       "\tf T\n"
                                       "}\n");

typedef struct Issue69912Job {
    TestingT *t;
    TokenFileSet *fset;
    Map *importer;
} Issue69912Job;

static void issue69912_check(void *env) {
    const Issue69912Job *job = (const Issue69912Job *)env;
    Arena car;
    arena_init(&car, NULL, 0);
    Error err = BURROW_NO_ERROR;
    (void)gct_check(arena_allocator(&car), job->fset, S("a.go"), S("p"),
                    issue69912_p_src, job->importer, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(job->t, "Check failed: %v", err);
    arena_free(&car);
}

static void gct_free_file_set(void *env) {
    token_file_set_free((TokenFileSet *)env);
}

static void TestIssue69912(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    Str tmpdir = gct_temp_dir(t);
    Str testoutdir = filepath_join_v(a, 2, tmpdir, S("testdata"));
    Error err = os_mkdir(testoutdir, 0700);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "making output dir: %v", err);
        return;
    }

    compile_v(t, S("testdata"), S("issue69912.go"), testoutdir, 0);

    /* Not in Go: the threads add files to the set, so it comes from the heap,
     * which they can all use, and not from the test's arena. */
    TokenFileSet *fset = token_new_file_set(heap_allocator());
    testing_t_cleanup(t, BURROW_FN(Func, gct_free_file_set, fset));

    err = BURROW_NO_ERROR;
    TypesPackage *issue69912 =
        gct_import(fset, new_packages(), S("./testdata/issue69912"), tmpdir, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }

    Map *importer = new_packages();
    Str name = S("issue69912");
    map_set(importer, &name, &issue69912);
    Issue69912Job job = {t, fset, importer};
    SyncWaitGroup wg = {0};
    for (int i = 0; i < 10; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, issue69912_check, &job));
    sync_wait_group_wait(&wg);
}

static void TestGenMeth(TestingT *t) {
    gct_begin(t);
    Str goroot = gct_goroot(t);
    if (goroot.len == 0)
        return;

    Str tmpdir = gct_temp_dir(t);
    Str testoutdir = filepath_join_v(a, 2, tmpdir, S("testdata"));
    Error err = os_mkdir(testoutdir, 0700);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "making output dir: %v", err);
        return;
    }

    compile_v(t, S("testdata"), S("genmeth.go"), testoutdir, 0);

    TokenFileSet *fset = token_new_file_set(a);

    err = BURROW_NO_ERROR;
    TypesPackage *genmeth =
        gct_import(fset, new_packages(), S("./testdata/genmeth"), tmpdir, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatal_v(t, err);
        return;
    }

    static const Str p_src =
        S_("package p\n"
           "\n"
           "import \"genmeth\"\n"
           "\n"
           "func _() {\n"
           "\tvar ex func(int) genmeth.List[int]\n"
           "\tvar fl func(genmeth.List[int]) genmeth.List[int]\n"
           "\n"
           "\tvar l genmeth.List[int]\n"
           "\tl = l.Map(ex).FlatMap(fl)\n"
           "\n"
           "\tvar bl genmeth.BiList[int, any]\n"
           "\tbl = bl.MapKeys(ex).Flip().FlatMapValues(fl).Flip()\n"
           "\n"
           "\tvar id func(int) int\n"
           "\n"
           "\tvar op genmeth.Option[int]\n"
           "\tvar _ int = op.MapIfPresent(id).Get()\n"
           "\n"
           "\tvar ol genmeth.OrderedList[int]\n"
           "\tvar _ int = ol.Min().Get()\n"
           "\n"
           "\tvar b genmeth.Box[int]\n"
           "\tb.Set(42)\n"
           "\tvar _ int = b.Get()\n"
           "}\n");

    Map *importer = new_packages();
    Str name = S("genmeth");
    map_set(importer, &name, &genmeth);
    err = BURROW_NO_ERROR;
    (void)gct_check(a, fset, S("genmeth.go"), S("p"), p_src, importer, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Check failed: %v", err);
}

#define TESTS(X)                                                                       \
    X(TestImportTestdata)                                                              \
    X(TestImportTypeparamTests)                                                        \
    X(TestVersionHandling)                                                             \
    X(TestImportStdLib)                                                                \
    X(TestImportedTypes)                                                               \
    X(TestIssue5815)                                                                   \
    X(TestCorrectMethodPackage)                                                        \
    X(TestIssue13566)                                                                  \
    X(TestTypeNamingOrder)                                                             \
    X(TestIssue13898)                                                                  \
    X(TestIssue15517)                                                                  \
    X(TestIssue15920)                                                                  \
    X(TestIssue20046)                                                                  \
    X(TestIssue25301)                                                                  \
    X(TestIssue25596)                                                                  \
    X(TestIssue57015)                                                                  \
    X(TestIssue69912)                                                                  \
    X(TestGenMeth)

TESTING_MAIN(TESTS)
