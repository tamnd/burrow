/* Derived from Go's src/go/types/eval_test.go, with independentTestTypes from
 * typestring_test.go. Go source: go1.27.1.
 *
 * Not here yet: TestEvalPos and TestCheckExpr, which import fmt, math and io
 * through the default importer, which burrow does not have yet.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

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

typedef struct {
    Str src, str;
} TestEntry;

/* types that don't depend on any other type declarations */
static const TestEntry independent_test_types[] = {
    /* basic types */
    {S_("int"), S_("int")},
    {S_("float32"), S_("float32")},
    {S_("string"), S_("string")},

    /* arrays */
    {S_("[10]int"), S_("[10]int")},

    /* slices */
    {S_("[]int"), S_("[]int")},
    {S_("[][]int"), S_("[][]int")},

    /* structs */
    {S_("struct{}"), S_("struct{}")},
    {S_("struct{x int}"), S_("struct{x int}")},
    {S_("struct {\n\t\tx, y int\n\t\tz float32 \"foo\"\n\t}"),
     S_("struct{x int; y int; z float32 \"foo\"}")},
    {S_("struct {\n\t\tstring\n\t\telems []complex128\n\t}"),
     S_("struct{string; elems []complex128}")},

    /* pointers */
    {S_("*int"), S_("*int")},
    {S_("***struct{}"), S_("***struct{}")},
    {S_("*struct{a int; b float32}"), S_("*struct{a int; b float32}")},

    /* functions */
    {S_("func()"), S_("func()")},
    {S_("func(x int)"), S_("func(x int)")},
    {S_("func(x, y int)"), S_("func(x int, y int)")},
    {S_("func(x, y int, z string)"), S_("func(x int, y int, z string)")},
    {S_("func(int)"), S_("func(int)")},
    {S_("func(int, string, byte)"), S_("func(int, string, byte)")},

    {S_("func() int"), S_("func() int")},
    {S_("func() (string)"), S_("func() string")},
    {S_("func() (u int)"), S_("func() (u int)")},
    {S_("func() (u, v int, w string)"), S_("func() (u int, v int, w string)")},

    {S_("func(int) string"), S_("func(int) string")},
    {S_("func(x int) string"), S_("func(x int) string")},
    {S_("func(x int) (u string)"), S_("func(x int) (u string)")},
    {S_("func(x, y int) (u string)"), S_("func(x int, y int) (u string)")},

    {S_("func(...int) string"), S_("func(...int) string")},
    {S_("func(x ...int) string"), S_("func(x ...int) string")},
    {S_("func(x ...int) (u string)"), S_("func(x ...int) (u string)")},
    {S_("func(x int, y ...int) (u string)"), S_("func(x int, y ...int) (u string)")},

    /* interfaces */
    {S_("interface{}"), S_("interface{}")},
    {S_("interface{m()}"), S_("interface{m()}")},
    {S_("interface{String() string; m(int) float32}"),
     S_("interface{String() string; m(int) float32}")},
    {S_("interface{int | float32 | complex128}"),
     S_("interface{int | float32 | complex128}")},
    {S_("interface{int | ~float32 | ~complex128}"),
     S_("interface{int | ~float32 | ~complex128}")},
    {S_("interface{comparable}"), S_("interface{comparable}")},
    /* TODO(gri) adjust test for EvalCompositeTest
     * {"error", "interface{Error() string}"},
     * {"any", "interface{}"},
     * {"comparable", "interface{comparable}"}, */

    /* maps */
    {S_("map[string]int"), S_("map[string]int")},
    {S_("map[struct{x, y int}][]byte"), S_("map[struct{x int; y int}][]byte")},

    /* channels */
    {S_("chan<- chan int"), S_("chan<- chan int")},
    {S_("chan<- <-chan int"), S_("chan<- <-chan int")},
    {S_("<-chan <-chan int"), S_("<-chan <-chan int")},
    {S_("chan (<-chan int)"), S_("chan (<-chan int)")},
    {S_("chan<- func()"), S_("chan<- func()")},
    {S_("<-chan []func() int"), S_("<-chan []func() int")},
};

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
    Slice files = {0};
    files = slice_append(a, files, &f, 1);
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
