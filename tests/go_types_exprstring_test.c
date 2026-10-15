/* Derived from Go's src/go/types/exprstring_test.go, with testEntry and dup
 * from typestring_test.go. Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/go/ast.h"
#include "burrow/go/parser.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

typedef struct TestEntry {
    const char *src;
    const char *str;
} TestEntry;

/* dup returns a testEntry where both src and str are the same. */
#define DUP(s) {s, s}

static const TestEntry test_exprs[] = {
    /* basic type literals */
    DUP("x"),
    DUP("true"),
    DUP("42"),
    DUP("3.1415"),
    DUP("2.71828i"),
    DUP("'a'"),
    DUP("\"foo\""),
    DUP("`bar`"),
    DUP("any"),

    /* func and composite literals */
    {"func(){}", "(func() literal)"},
    {"func(x int) complex128 {}", "(func(x int) complex128 literal)"},
    {"[]int{1, 2, 3}", "[]int{\xe2\x80\xa6}"},

    /* type expressions */
    DUP("[1 << 10]byte"),
    DUP("[]int"),
    DUP("*int"),
    DUP("struct{x int}"),
    DUP("func()"),
    DUP("func(int, float32) string"),
    DUP("interface{m()}"),
    DUP("interface{m() string; n(x int)}"),
    DUP("interface{~int}"),

    DUP("map[string]int"),
    DUP("chan E"),
    DUP("<-chan E"),
    DUP("chan<- E"),

    /* new interfaces */
    DUP("interface{int}"),
    DUP("interface{~int}"),

    /* generic constraints */
    DUP("interface{~a | ~b | ~c; ~int | ~string; float64; m()}"),
    DUP("interface{int | string}"),
    DUP("interface{~int | ~string; float64; m()}"),
    DUP("interface{~T[int, string] | string}"),

    /* generic types */
    DUP("x[T]"),
    DUP("x[N | A | S]"),
    DUP("x[N, A]"),

    /* non-type expressions */
    DUP("(x)"),
    DUP("x.f"),
    DUP("a[i]"),

    DUP("s[:]"),
    DUP("s[i:]"),
    DUP("s[:j]"),
    DUP("s[i:j]"),
    DUP("s[:j:k]"),
    DUP("s[i:j:k]"),

    DUP("x.(T)"),

    DUP("x.([10]int)"),
    DUP("x.([...]int)"),

    DUP("x.(struct{})"),
    DUP("x.(struct{x int; y, z float32; E})"),

    DUP("x.(func())"),
    DUP("x.(func(x int))"),
    DUP("x.(func() int)"),
    DUP("x.(func(x, y int, z float32) (r int))"),
    DUP("x.(func(a, b, c int))"),
    DUP("x.(func(x ...T))"),

    DUP("x.(interface{})"),
    DUP("x.(interface{m(); n(x int); E})"),
    DUP("x.(interface{m(); n(x int) T; E; F})"),

    DUP("x.(map[K]V)"),

    DUP("x.(chan E)"),
    DUP("x.(<-chan E)"),
    DUP("x.(chan<- chan int)"),
    DUP("x.(chan<- <-chan int)"),
    DUP("x.(<-chan chan int)"),
    DUP("x.(chan (<-chan int))"),

    DUP("f()"),
    DUP("f(x)"),
    DUP("int(x)"),
    DUP("f(x, x + y)"),
    DUP("f(s...)"),
    DUP("f(a, s...)"),

    /* generic functions */
    DUP("f[T]()"),
    DUP("f[T](T)"),
    DUP("f[T, T1]()"),
    DUP("f[T, T1](T, T1)"),

    DUP("*x"),
    DUP("&x"),
    DUP("x + y"),
    DUP("x + y << (2 * s)"),
};

static void TestExprString(TestingT *t) {
    for (Int i = 0; i < NELEM(test_exprs); i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        const char *src = test_exprs[i].src;
        Error err = BURROW_NO_ERROR;
        AstExpr x = parser_parse_expr(a, str_from_cstr(src), &err);
        if (!BURROW_OK(err)) {
            testing_t_errorf_v(t, "%s: %s", src, error_text(err));
            arena_free(&ar);
            continue;
        }
        Str got = types_expr_string(a, x);
        if (!str_eq(got, str_from_cstr(test_exprs[i].str)))
            testing_t_errorf_v(t, "%s: got %s, want %s", src, got, test_exprs[i].str);
        arena_free(&ar);
    }
}

/* Not in Go: the nodes ExprString has no form for, which it names the way
 * Go's %T does, and WriteExpr adding to what a buffer already holds. */
static void check_str(TestingT *t, Str got, Str want) {
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "got %q, want %q", got, want);
}

static void TestExprStringOther(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    check_str(t, types_expr_string(a, NULL), S("(ast: <nil>)"));
    AstExpr bad = ast_node_new(a, AST_KIND_BAD_EXPR);
    check_str(t, types_expr_string(a, bad), S("(ast: *ast.BadExpr)"));

    Error err = BURROW_NO_ERROR;
    AstExpr lit = parser_parse_expr(a, S("T{k: v}"), &err);
    CHECK(BURROW_OK(err));
    AstCompositeLit *c = (AstCompositeLit *)(void *)lit;
    CHECK(c->elts.len == 1);
    AstExpr kv = ((AstExpr *)c->elts.p)[0];
    check_str(t, types_expr_string(a, kv), S("(ast: *ast.KeyValueExpr)"));
    check_str(t, types_expr_string(a, lit), S("T{\xe2\x80\xa6}"));

    BytesBuffer buf = BYTES_BUFFER(a);
    bytes_buffer_write_string(&buf, S("x = "), NULL);
    types_write_expr(&buf, parser_parse_expr(a, S("f(a, b...)"), &err));
    CHECK(BURROW_OK(err));
    check_str(t, bytes_buffer_string(&buf, a), S("x = f(a, b...)"));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestExprString)                                                                  \
    X(TestExprStringOther)

TESTING_MAIN(TESTS)
