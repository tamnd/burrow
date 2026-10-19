/* Derived from Go's src/go/types/builtins_test.go. Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_helpers.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/go/ast.h"
#include "burrow/go/types.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"

#define S(lit) BURROW_S(lit)
#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static const struct {
    Str name, src, sig;
} builtin_calls[] = {
    {S_("append"), S_("var s []int; _ = append(s)"), S_("func([]int, ...int) []int")},
    {S_("append"), S_("var s []int; _ = append(s, 0)"),
     S_("func([]int, ...int) []int")},
    {S_("append"), S_("var s []int; _ = (append)(s, 0)"),
     S_("func([]int, ...int) []int")},
    {S_("append"), S_("var s []byte; _ = ((append))(s, 0)"),
     S_("func([]byte, ...byte) []byte")},
    {S_("append"), S_("var s []byte; _ = append(s, \"foo\"...)"),
     S_("func([]byte, string...) []byte")},
    {S_("append"), S_("type T []byte; var s T; var str string; _ = append(s, str...)"),
     S_("func(p.T, string...) p.T")},
    {S_("append"),
     S_("type T []byte; type U string; var s T; var str U; _ = append(s, str...)"),
     S_("func(p.T, p.U...) p.T")},

    {S_("cap"), S_("var s [10]int; _ = cap(s)"), S_("invalid type")},  /* constant */
    {S_("cap"), S_("var s [10]int; _ = cap(&s)"), S_("invalid type")}, /* constant */
    {S_("cap"), S_("var s []int64; _ = cap(s)"), S_("func([]int64) int")},
    {S_("cap"), S_("var c chan<-bool; _ = cap(c)"), S_("func(chan<- bool) int")},
    {S_("cap"), S_("type S []byte; var s S; _ = cap(s)"), S_("func(p.S) int")},
    {S_("cap"), S_("var s P; _ = cap(s)"), S_("func(P) int")},

    {S_("len"), S_("_ = len(\"foo\")"), S_("invalid type")}, /* constant */
    {S_("len"), S_("var s string; _ = len(s)"), S_("func(string) int")},
    {S_("len"), S_("var s [10]int; _ = len(s)"), S_("invalid type")},  /* constant */
    {S_("len"), S_("var s [10]int; _ = len(&s)"), S_("invalid type")}, /* constant */
    {S_("len"), S_("var s []int64; _ = len(s)"), S_("func([]int64) int")},
    {S_("len"), S_("var c chan<-bool; _ = len(c)"), S_("func(chan<- bool) int")},
    {S_("len"), S_("var m map[string]float32; _ = len(m)"),
     S_("func(map[string]float32) int")},
    {S_("len"), S_("type S []byte; var s S; _ = len(s)"), S_("func(p.S) int")},
    {S_("len"), S_("var s P; _ = len(s)"), S_("func(P) int")},

    {S_("clear"), S_("var m map[float64]int; clear(m)"), S_("func(map[float64]int)")},
    {S_("clear"), S_("var s []byte; clear(s)"), S_("func([]byte)")},

    {S_("close"), S_("var c chan int; close(c)"), S_("func(chan int)")},
    {S_("close"), S_("var c chan<- chan string; close(c)"),
     S_("func(chan<- chan string)")},

    {S_("complex"), S_("_ = complex(1, 0)"), S_("invalid type")}, /* constant */
    {S_("complex"), S_("var re float32; _ = complex(re, 1.0)"),
     S_("func(float32, float32) complex64")},
    {S_("complex"), S_("var im float64; _ = complex(1, im)"),
     S_("func(float64, float64) complex128")},
    {S_("complex"), S_("type F32 float32; var re, im F32; _ = complex(re, im)"),
     S_("func(p.F32, p.F32) complex64")},
    {S_("complex"), S_("type F64 float64; var re, im F64; _ = complex(re, im)"),
     S_("func(p.F64, p.F64) complex128")},

    {S_("copy"), S_("var src, dst []byte; copy(dst, src)"),
     S_("func([]byte, []byte) int")},
    {S_("copy"), S_("type T [][]int; var src, dst T; _ = copy(dst, src)"),
     S_("func(p.T, p.T) int")},
    {S_("copy"), S_("var src string; var dst []byte; copy(dst, src)"),
     S_("func([]byte, string) int")},
    {S_("copy"),
     S_("type T string; type U []byte; var src T; var dst U; copy(dst, src)"),
     S_("func(p.U, p.T) int")},
    {S_("copy"), S_("var dst []byte; copy(dst, \"hello\")"),
     S_("func([]byte, string) int")},

    {S_("delete"), S_("var m map[string]bool; delete(m, \"foo\")"),
     S_("func(map[string]bool, string)")},
    {S_("delete"), S_("type (K string; V int); var m map[K]V; delete(m, \"foo\")"),
     S_("func(map[p.K]p.V, p.K)")},

    {S_("imag"), S_("_ = imag(1i)"), S_("invalid type")}, /* constant */
    {S_("imag"), S_("var c complex64; _ = imag(c)"), S_("func(complex64) float32")},
    {S_("imag"), S_("var c complex128; _ = imag(c)"), S_("func(complex128) float64")},
    {S_("imag"), S_("type C64 complex64; var c C64; _ = imag(c)"),
     S_("func(p.C64) float32")},
    {S_("imag"), S_("type C128 complex128; var c C128; _ = imag(c)"),
     S_("func(p.C128) float64")},

    {S_("real"), S_("_ = real(1i)"), S_("invalid type")}, /* constant */
    {S_("real"), S_("var c complex64; _ = real(c)"), S_("func(complex64) float32")},
    {S_("real"), S_("var c complex128; _ = real(c)"), S_("func(complex128) float64")},
    {S_("real"), S_("type C64 complex64; var c C64; _ = real(c)"),
     S_("func(p.C64) float32")},
    {S_("real"), S_("type C128 complex128; var c C128; _ = real(c)"),
     S_("func(p.C128) float64")},

    {S_("make"), S_("_ = make([]int, 10)"), S_("func([]int, int) []int")},
    {S_("make"), S_("type T []byte; _ = make(T, 10, 20)"),
     S_("func(p.T, int, int) p.T")},

    /* go.dev/issue/37349 */
    {S_("make"), S_("              _ = make([]int, 0   )"),
     S_("func([]int, int) []int")},
    {S_("make"), S_("var l    int; _ = make([]int, l   )"),
     S_("func([]int, int) []int")},
    {S_("make"), S_("              _ = make([]int, 0, 0)"),
     S_("func([]int, int, int) []int")},
    {S_("make"), S_("var l    int; _ = make([]int, l, 0)"),
     S_("func([]int, int, int) []int")},
    {S_("make"), S_("var    c int; _ = make([]int, 0, c)"),
     S_("func([]int, int, int) []int")},
    {S_("make"), S_("var l, c int; _ = make([]int, l, c)"),
     S_("func([]int, int, int) []int")},

    /* go.dev/issue/37393 */
    {S_("make"), S_("                _ = make([]int       , 0   )"),
     S_("func([]int, int) []int")},
    {S_("make"), S_("var l    byte ; _ = make([]int8      , l   )"),
     S_("func([]int8, byte) []int8")},
    {S_("make"), S_("                _ = make([]int16     , 0, 0)"),
     S_("func([]int16, int, int) []int16")},
    {S_("make"), S_("var l    int16; _ = make([]string    , l, 0)"),
     S_("func([]string, int16, int) []string")},
    {S_("make"), S_("var    c int32; _ = make([]float64   , 0, c)"),
     S_("func([]float64, int, int32) []float64")},
    {S_("make"), S_("var l, c uint ; _ = make([]complex128, l, c)"),
     S_("func([]complex128, uint, uint) []complex128")},

    /* go.dev/issue/45667 */
    {S_("make"), S_("const l uint = 1; _ = make([]int, l)"),
     S_("func([]int, uint) []int")},

    {S_("max"), S_("               _ = max(0        )"),
     S_("invalid type")}, /* constant */
    {S_("max"), S_("var x int    ; _ = max(x        )"), S_("func(int) int")},
    {S_("max"), S_("var x int    ; _ = max(0, x     )"), S_("func(int, int) int")},
    {S_("max"), S_("var x string ; _ = max(\"a\", x   )"),
     S_("func(string, string) string")},
    {S_("max"), S_("var x float32; _ = max(0, 1.0, x)"),
     S_("func(float32, float32, float32) float32")},

    {S_("min"), S_("               _ = min(0        )"),
     S_("invalid type")}, /* constant */
    {S_("min"), S_("var x int    ; _ = min(x        )"), S_("func(int) int")},
    {S_("min"), S_("var x int    ; _ = min(0, x     )"), S_("func(int, int) int")},
    {S_("min"), S_("var x string ; _ = min(\"a\", x   )"),
     S_("func(string, string) string")},
    {S_("min"), S_("var x float32; _ = min(0, 1.0, x)"),
     S_("func(float32, float32, float32) float32")},

    {S_("new"), S_("_ = new(int)"), S_("func(int) *int")},
    {S_("new"), S_("type T struct{}; _ = new(T)"), S_("func(p.T) *p.T")},

    {S_("panic"), S_("panic(0)"), S_("func(interface{})")},
    {S_("panic"), S_("panic(\"foo\")"), S_("func(interface{})")},

    {S_("print"), S_("print()"), S_("func()")},
    {S_("print"), S_("print(0)"), S_("func(int)")},
    {S_("print"), S_("print(1, 2.0, \"foo\", true)"),
     S_("func(int, float64, string, bool)")},

    {S_("println"), S_("println()"), S_("func()")},
    {S_("println"), S_("println(0)"), S_("func(int)")},
    {S_("println"), S_("println(1, 2.0, \"foo\", true)"),
     S_("func(int, float64, string, bool)")},

    {S_("recover"), S_("recover()"), S_("func() interface{}")},
    {S_("recover"), S_("_ = recover()"), S_("func() interface{}")},

    {S_("Add"), S_("var p unsafe.Pointer; _ = unsafe.Add(p, -1.0)"),
     S_("func(unsafe.Pointer, int) unsafe.Pointer")},
    {S_("Add"), S_("var p unsafe.Pointer; var n uintptr; _ = unsafe.Add(p, n)"),
     S_("func(unsafe.Pointer, uintptr) unsafe.Pointer")},
    {S_("Add"), S_("_ = unsafe.Add(nil, 0)"),
     S_("func(unsafe.Pointer, int) unsafe.Pointer")},

    {S_("Alignof"), S_("_ = unsafe.Alignof(0)"), S_("invalid type")}, /* constant */
    {S_("Alignof"), S_("var x struct{}; _ = unsafe.Alignof(x)"),
     S_("invalid type")}, /* constant */
    {S_("Alignof"), S_("var x P; _ = unsafe.Alignof(x)"), S_("func(P) uintptr")},

    {S_("Offsetof"), S_("var x struct{f bool}; _ = unsafe.Offsetof(x.f)"),
     S_("invalid type")}, /* constant */
    {S_("Offsetof"), S_("var x struct{_ int; f bool}; _ = unsafe.Offsetof((&x).f)"),
     S_("invalid type")}, /* constant */
    {S_("Offsetof"), S_("var x struct{_ int; f P}; _ = unsafe.Offsetof((&x).f)"),
     S_("func(P) uintptr")},

    {S_("Sizeof"), S_("_ = unsafe.Sizeof(0)"), S_("invalid type")}, /* constant */
    {S_("Sizeof"), S_("var x struct{}; _ = unsafe.Sizeof(x)"),
     S_("invalid type")}, /* constant */
    {S_("Sizeof"), S_("var x P; _ = unsafe.Sizeof(x)"), S_("func(P) uintptr")},

    {S_("Slice"), S_("var p *int; _ = unsafe.Slice(p, 1)"),
     S_("func(*int, int) []int")},
    {S_("Slice"), S_("var p *byte; var n uintptr; _ = unsafe.Slice(p, n)"),
     S_("func(*byte, uintptr) []byte")},
    {S_("Slice"), S_("type B *byte; var b B; _ = unsafe.Slice(b, 0)"),
     S_("func(*byte, int) []byte")},

    {S_("SliceData"), S_("var s []int; _ = unsafe.SliceData(s)"),
     S_("func([]int) *int")},
    {S_("SliceData"), S_("type S []int; var s S; _ = unsafe.SliceData(s)"),
     S_("func([]int) *int")},

    {S_("String"), S_("var p *byte; _ = unsafe.String(p, 1)"),
     S_("func(*byte, int) string")},
    {S_("String"), S_("type B *byte; var b B; _ = unsafe.String(b, 0)"),
     S_("func(*byte, int) string")},

    {S_("StringData"), S_("var s string; _ = unsafe.StringData(s)"),
     S_("func(string) *byte")},
    {S_("StringData"), S_("_ = unsafe.StringData(\"foo\")"), S_("func(string) *byte")},

    {S_("assert"), S_("assert(true)"), S_("invalid type")}, /* constant */
    {S_("assert"), S_("type B bool; const pred B = 1 < 2; assert(pred)"),
     S_("invalid type")}, /* constant */

    /* no tests for trace since it produces output as a side-effect */
};

static void test_builtin_signature(TestingT *t, Alloc *a, Str name, Str src0,
                                   Str want) {
    Str src =
        fmt_sprintf_v(a,
                      "package p; import \"unsafe\"; type _ unsafe.Pointer /* use "
                      "unsafe */; func _[P ~[]byte]() { %s }",
                      src0);

    Map *uses = map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
    Map *types = map_make(a, TYPE_UNSAFE_POINTER, TYPE_TYPES_TYPE_AND_VALUE, 0);
    TypesInfo info = {0};
    info.uses = uses;
    info.types = types;
    must_typecheck(a, src, NULL, &info);

    /* find called function */
    Int n = 0;
    AstExpr fun = NULL;
    const void *k;
    void *v;
    for (MapIter it = map_iter(types); map_next(&it, &k, &v);) {
        AstExpr x = *(AstExpr const *)k;
        if (x->kind == AST_KIND_CALL_EXPR) {
            fun = ((AstCallExpr *)x)->fun;
            n++;
        }
    }
    if (n != 1) {
        testing_t_errorf_v(t, "%s: got %d CallExprs; want 1", src0, n);
        return;
    }

    /* check recorded types for fun and descendents (may be parenthesized) */
    for (;;) {
        /* the recorded type for the built-in must match the wanted signature */
        const TypesTypeAndValue *tv = map_get(types, &fun);
        TypesType typ = tv != NULL ? tv->type : NULL;
        if (typ == NULL) {
            testing_t_errorf_v(t, "%s: no type recorded for %s", src0,
                               types_expr_string(a, fun));
            return;
        }
        Str got = types_type_string(a, typ, TYPES_NO_QUALIFIER);
        if (!str_eq(got, want)) {
            testing_t_errorf_v(t, "%s: got type %s; want %s", src0, got, want);
            return;
        }

        /* called function must be a (possibly parenthesized, qualified)
         * identifier denoting the expected built-in */
        switch (fun->kind) {
        case AST_KIND_IDENT: {
            AstIdent *p = (AstIdent *)fun;
            TypesObject *pobj = map_get(uses, &p);
            TypesObject obj = pobj != NULL ? *pobj : NULL;
            if (obj == NULL) {
                testing_t_errorf_v(t, "%s: no object found for %s", src0, p->name);
                return;
            }
            TypesBuiltin *bin = types_as_builtin(obj);
            if (bin == NULL) {
                testing_t_errorf_v(t, "%s: %s does not denote a built-in", src0,
                                   p->name);
                return;
            }
            if (!str_eq(types_object_name(obj), name)) {
                testing_t_errorf_v(t, "%s: got built-in %s; want %s", src0,
                                   types_object_name(obj), name);
                return;
            }
            return; /* we're done */
        }

        case AST_KIND_PAREN_EXPR:
            fun = ((AstParenExpr *)fun)->x; /* unpack */
            break;

        case AST_KIND_SELECTOR_EXPR:
            /* built-in from package unsafe - ignore details */
            return; /* we're done */

        default:
            testing_t_errorf_v(t, "%s: invalid function call", src0);
            return;
        }
    }
}

static void TestBuiltinSignatures(TestingT *t) {
    types_def_predeclared_test_funcs();

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Map *seen = map_make(a, TYPE_STRING, TYPE_BOOL, 0);
    bool yes = true;
    Str trace = S("trace");
    map_set(seen, &trace, &yes); /* no test for trace built-in; add it manually */
    for (Int i = 0; i < NELEM(builtin_calls); i++) {
        test_builtin_signature(t, a, builtin_calls[i].name, builtin_calls[i].src,
                               builtin_calls[i].sig);
        map_set(seen, &builtin_calls[i].name, &yes);
    }

    /* make sure we didn't miss one */
    Slice names = types_scope_names(a, types_universe());
    for (Int i = 0; i < names.len; i++) {
        Str name = ((Str *)names.p)[i];
        if (types_as_builtin(types_scope_lookup(types_universe(), name)) != NULL &&
            map_get(seen, &name) == NULL)
            testing_t_errorf_v(t, "missing test for %s", name);
    }
    TypesScope *unsafe_scope = types_package_scope(types_unsafe());
    names = types_scope_names(a, unsafe_scope);
    for (Int i = 0; i < names.len; i++) {
        Str name = ((Str *)names.p)[i];
        if (types_as_builtin(types_scope_lookup(unsafe_scope, name)) != NULL &&
            map_get(seen, &name) == NULL)
            testing_t_errorf_v(t, "missing test for unsafe.%s", name);
    }
    arena_free(&ar);
}

#define TESTS(X) X(TestBuiltinSignatures)

TESTING_MAIN(TESTS)
