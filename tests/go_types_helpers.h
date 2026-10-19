/* The helpers from Go's src/go/types/api_test.go that the other go/types test
 * files use: pkgName, mustParse, typecheck, mustTypecheck and testImporter,
 * and pkgFiles and pkgFilenames from self_test.go and stdlib_test.go. Go
 * source: go1.27.1.
 *
 * Not in Go: Go's test files are one package and share these. Each burrow test
 * is its own program, so they live here. tests/go_types_api_test.c keeps its
 * own copies, which are the same.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef GO_TYPES_HELPERS_H
#define GO_TYPES_HELPERS_H

#include "go_types_importer.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/go/ast.h"
#include "burrow/go/build.h"
#include "burrow/go/parser.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/map.h"
#include "burrow/panic.h"
#include "burrow/path/filepath.h"
#include "burrow/slice.h"
#include "burrow/strings.h"

/* pkgName extracts the package name from src, which must contain a package
 * header. */
static inline Str gth_pkg_name(Str src) {
    Str kw = BURROW_S("package ");
    Int i = strings_index(src, kw);
    if (i >= 0) {
        Str after = str_from_bytes(src.p + i + kw.len, src.len - i - kw.len);
        Int n = after.len;
        Int j = strings_index_any(after, BURROW_S("\n\t ;/"));
        if (j >= 0)
            n = j;
        return str_from_bytes(after.p, n);
    }
    panic_str(BURROW_S("missing package header"));
}

static inline AstFile *must_parse(Alloc *a, TokenFileSet *fset, Str src) {
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, fset, gth_pkg_name(src), BURROW_ANY(TYPE_STRING, &src),
                          PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err)); /* so we don't need to pass *testing.T */
    return f;
}

static inline void gth_collect_all(void *env, const TypesError *e) {
    (void)env;
    (void)e;
}

static inline TypesPackage *typecheck(Alloc *a, Str src, TypesConfig *conf,
                                      TypesInfo *info, Error *err) {
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, src);
    TypesConfig conf0 = {0};
    if (conf == NULL) {
        conf0.error = (TypesErrorFunc){gth_collect_all, NULL}; /* collect all errors */
        conf0.importer = default_importer(fset);
        conf = &conf0;
    }
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    return types_config_check(conf, a, f->name->name, fset, files, info, err);
}

static inline TypesPackage *must_typecheck(Alloc *a, Str src, TypesConfig *conf,
                                           TypesInfo *info) {
    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg = typecheck(a, src, conf, info, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err)); /* so we don't need to pass *testing.T */
    return pkg;
}

/* testImporter: the packages it can import, by path. */
static inline TypesPackage *test_importer_import(void *self, Str path, Error *err) {
    Map *m = self;
    TypesPackage **pkg = map_get(m, &path);
    if (pkg != NULL && *pkg != NULL)
        return *pkg;
    *err = fmt_errorf_v("package %q not found", path);
    return NULL;
}

static const TypesImporterVT test_importer_vt = {NULL, test_importer_import, NULL};

static inline Map *new_test_importer(Alloc *a) {
    return map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
}

static inline TypesImporter test_importer(Map *m) {
    return (TypesImporter){&test_importer_vt, m};
}

/* pkgFilenames from stdlib_test.go: the Go files of the package in dir, with
 * its test files if include_test.
 *
 * Not in Go: dir is a directory under GOROOT/src rather than one relative to
 * the test's own. */
static inline Slice pkg_filenames(Alloc *a, Str dir, bool include_test, Error *err) {
    BuildContext ctxt = build_default(a);
    ctxt.goroot = gtt_find_goroot(a);
    ctxt.cgo_enabled = false;
    BuildPackage *pkg = build_context_import_dir(&ctxt, a, dir, 0, err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_STRING);
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

static inline Slice pkg_files(Alloc *a, TokenFileSet *fset, Str path, Error *err) {
    Slice filenames = pkg_filenames(a, path, true, err); /* from stdlib_test.go */
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_AST_FILE_PTR);

    Slice files = slice_nil(TYPE_AST_FILE_PTR);
    for (Int i = 0; i < filenames.len; i++) {
        AstFile *file =
            parser_parse_file(a, fset, BURROW_AT(Str, filenames, i), (Any){NULL, NULL},
                              PARSER_SKIP_OBJECT_RESOLUTION, err);
        if (BURROW_FAILED(*err))
            return slice_nil(TYPE_AST_FILE_PTR);
        files = slice_append(a, files, &file, 1);
    }

    return files;
}

#endif
