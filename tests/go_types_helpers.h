/* The helpers from Go's src/go/types/api_test.go that the other go/types test
 * files use: pkgName, mustParse, typecheck and mustTypecheck. Go source:
 * go1.27.1.
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
#include "burrow/go/ast.h"
#include "burrow/go/parser.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/panic.h"
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

#endif
