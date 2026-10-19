/* Derived from Go's src/go/types/eval_test.go. Go source: go1.27.1.
 *
 * Not here yet: TestEvalPos and TestCheckExpr, which import fmt, math and io
 * through the default importer, which burrow does not have yet.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_typestring.h"

#include "burrow/burrow.h"
#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/parser.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"

#define S(lit) BURROW_S(lit)
#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static void test_eval(TestingT *t, Alloc *a, TokenFileSet *fset, TypesPackage *pkg,
                      TokenPos pos, Str expr, TypesType typ, Str typ_str, Str val_str) {
    Error err = BURROW_NO_ERROR;
    TypesTypeAndValue got_tv = types_eval(a, fset, pkg, pos, expr, &err);
    if (BURROW_FAILED(err)) {
        Str msg = error_text(err);
        testing_t_errorf_v(t, "Eval(%q) failed: %s", expr, msg);
        return;
    }
    if (got_tv.type == NULL) {
        testing_t_errorf_v(t, "Eval(%q) got nil type but no error", expr);
        return;
    }

    /* compare types */
    if (typ != NULL) {
        /* we have a type, check identity */
        if (!types_identical(got_tv.type, typ)) {
            Str got = types_type_string(a, got_tv.type, TYPES_NO_QUALIFIER);
            Str want = types_type_string(a, typ, TYPES_NO_QUALIFIER);
            testing_t_errorf_v(t, "Eval(%q) got type %s, want %s", expr, got, want);
            return;
        }
    } else {
        /* we have a string, compare type string */
        Str got_str = types_type_string(a, got_tv.type, TYPES_NO_QUALIFIER);
        if (!str_eq(got_str, typ_str)) {
            testing_t_errorf_v(t, "Eval(%q) got type %s, want %s", expr, got_str,
                               typ_str);
            return;
        }
    }

    /* compare values */
    Str got_str = S("");
    /* Not in Go: the zero ConstantValue, of kind unknown, is Go's nil Value. */
    if (constant_value_kind(got_tv.value) != CONSTANT_UNKNOWN)
        got_str = constant_value_exact_string(got_tv.value, a);
    if (!str_eq(got_str, val_str))
        testing_t_errorf_v(t, "Eval(%q) got value %s, want %s", expr, got_str, val_str);
}

static void TestEvalBasic(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    for (Int k = TYPES_BOOL; k <= TYPES_STRING; k++) {
        TypesBasic *typ = types_typ[k];
        test_eval(t, a, fset, NULL, TOKEN_NO_POS, types_basic_name(typ),
                  types_type(typ), S(""), S(""));
    }
    arena_free(&ar);
}

static void TestEvalComposite(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    for (Int i = 0; i < NELEM(independent_test_types); i++) {
        const TestEntry *test = &independent_test_types[i];
        test_eval(t, a, fset, NULL, TOKEN_NO_POS, test->src, NULL, test->str, S(""));
    }
    arena_free(&ar);
}

static void TestEvalArith(TestingT *t) {
    static const Str tests[] = {
        S_("true"),
        S_("false == false"),
        S_("12345678 + 87654321 == 99999999"),
        S_("10 * 20 == 200"),
        S_("(1<<500)*2 >> 100 == 2<<400"),
        S_("\"foo\" + \"bar\" == \"foobar\""),
        S_("\"abc\" <= \"bcd\""),
        S_("len([10]struct{}{}) == 2*5"),
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    for (Int i = 0; i < NELEM(tests); i++)
        test_eval(t, a, fset, NULL, TOKEN_NO_POS, tests[i],
                  types_type(types_typ[TYPES_UNTYPED_BOOL]), S(""), S("true"));
    arena_free(&ar);
}

static void TestIssue65898(TestingT *t) {
    Str src = S("\npackage p\nfunc _[A any](A) {}\n");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, fset, S("p"), BURROW_ANY(TYPE_STRING, &src),
                          PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    TypesConfig conf = {0};
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    TypesPackage *pkg = types_config_check(&conf, a, S("p"), fset, files, NULL, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    for (Int i = 0; i < f->decls.len; i++) {
        AstDecl d = ((AstDecl *)f->decls.p)[i];
        if (d->kind != AST_KIND_FUNC_DECL)
            continue;
        AstFuncDecl *fun = (AstFuncDecl *)(void *)d;
        AstExpr typ = &fun->type->node;
        /* type parameter A is not found at the start of the function type */
        bool ok = types_check_expr(a, fset, pkg, ast_node_pos(typ), typ, NULL, &err);
        if (ok || !strings_contains(error_text(err), S("undefined"))) {
            Str got = ok ? S("<nil>") : error_text(err);
            testing_t_fatalf_v(t, "got %s, want undefined error", got);
        }
        /* type parameter A must be found at the end of the function type */
        if (!types_check_expr(a, fset, pkg, ast_node_end(typ), typ, NULL, &err))
            testing_t_fatalf_v(t, "%s", error_text(err));
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestEvalBasic)                                                                   \
    X(TestEvalComposite)                                                               \
    X(TestEvalArith)                                                                   \
    X(TestIssue65898)

TESTING_MAIN(TESTS)
