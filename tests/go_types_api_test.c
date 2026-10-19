/* Derived from Go's src/go/types/api_test.go. Go source: go1.27.1.
 *
 * Not in Go: defaultImporter is the source importer in go_types_importer.h.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_importer.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/importer.h"
#include "burrow/go/parser.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/slices.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"

#define S(lit) BURROW_S(lit)
#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* pkgName extracts the package name from src, which must contain a package
 * header. */
static Str pkg_name(Str src) {
    Str kw = S("package ");
    Int i = strings_index(src, kw);
    if (i >= 0) {
        Str after = str_from_bytes(src.p + i + kw.len, src.len - i - kw.len);
        Int n = after.len;
        Int j = strings_index_any(after, S("\n\t ;/"));
        if (j >= 0)
            n = j;
        return str_from_bytes(after.p, n);
    }
    panic_str(S("missing package header"));
}

static AstFile *must_parse(Alloc *a, TokenFileSet *fset, Str src) {
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, fset, pkg_name(src), BURROW_ANY(TYPE_STRING, &src),
                          PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err)); /* so we don't need to pass *testing.T */
    return f;
}

static void collect_all(void *env, const TypesError *e) {
    (void)env;
    (void)e;
}

static TypesPackage *typecheck(Alloc *a, Str src, TypesConfig *conf, TypesInfo *info,
                               Error *err) {
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, src);
    TypesConfig conf0 = {0};
    if (conf == NULL) {
        conf0.error = (TypesErrorFunc){collect_all, NULL}; /* collect all errors */
        conf0.importer = default_importer(fset);
        conf = &conf0;
    }
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    return types_config_check(conf, a, f->name->name, fset, files, info, err);
}

static TypesPackage *must_typecheck(Alloc *a, Str src, TypesConfig *conf,
                                    TypesInfo *info) {
    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg = typecheck(a, src, conf, info, &err);
    if (BURROW_FAILED(err))
        panic_str(error_text(err)); /* so we don't need to pass *testing.T */
    return pkg;
}

static Map *new_types_map(Alloc *a) {
    return map_make(a, TYPE_UNSAFE_POINTER, TYPE_TYPES_TYPE_AND_VALUE, 0);
}

static Map *new_objects_map(Alloc *a) {
    return map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
}

/* Type.String, made in a. */
static Str type_string(Alloc *a, TypesType t) {
    return types_type_string(a, t, TYPES_NO_QUALIFIER);
}

static const struct {
    Str src;
    Str expr; /* constant expression */
    Str typ;  /* constant type */
    Str val;  /* constant value */
} values_tests[] = {
    {S_("package a0; const _ = false"), S_("false"), S_("untyped bool"), S_("false")},
    {S_("package a1; const _ = 0"), S_("0"), S_("untyped int"), S_("0")},
    {S_("package a2; const _ = 'A'"), S_("'A'"), S_("untyped rune"), S_("65")},
    {S_("package a3; const _ = 0."), S_("0."), S_("untyped float"), S_("0")},
    {S_("package a4; const _ = 0i"), S_("0i"), S_("untyped complex"), S_("(0 + 0i)")},
    {S_("package a5; const _ = \"foo\""), S_("\"foo\""), S_("untyped string"),
     S_("\"foo\"")},

    {S_("package b0; var _ = false"), S_("false"), S_("bool"), S_("false")},
    {S_("package b1; var _ = 0"), S_("0"), S_("int"), S_("0")},
    {S_("package b2; var _ = 'A'"), S_("'A'"), S_("rune"), S_("65")},
    {S_("package b3; var _ = 0."), S_("0."), S_("float64"), S_("0")},
    {S_("package b4; var _ = 0i"), S_("0i"), S_("complex128"), S_("(0 + 0i)")},
    {S_("package b5; var _ = \"foo\""), S_("\"foo\""), S_("string"), S_("\"foo\"")},

    {S_("package c0a; var _ = bool(false)"), S_("false"), S_("bool"), S_("false")},
    {S_("package c0b; var _ = bool(false)"), S_("bool(false)"), S_("bool"),
     S_("false")},
    {S_("package c0c; type T bool; var _ = T(false)"), S_("T(false)"), S_("c0c.T"),
     S_("false")},

    {S_("package c1a; var _ = int(0)"), S_("0"), S_("int"), S_("0")},
    {S_("package c1b; var _ = int(0)"), S_("int(0)"), S_("int"), S_("0")},
    {S_("package c1c; type T int; var _ = T(0)"), S_("T(0)"), S_("c1c.T"), S_("0")},

    {S_("package c2a; var _ = rune('A')"), S_("'A'"), S_("rune"), S_("65")},
    {S_("package c2b; var _ = rune('A')"), S_("rune('A')"), S_("rune"), S_("65")},
    {S_("package c2c; type T rune; var _ = T('A')"), S_("T('A')"), S_("c2c.T"),
     S_("65")},

    {S_("package c3a; var _ = float32(0.)"), S_("0."), S_("float32"), S_("0")},
    {S_("package c3b; var _ = float32(0.)"), S_("float32(0.)"), S_("float32"), S_("0")},
    {S_("package c3c; type T float32; var _ = T(0.)"), S_("T(0.)"), S_("c3c.T"),
     S_("0")},

    {S_("package c4a; var _ = complex64(0i)"), S_("0i"), S_("complex64"),
     S_("(0 + 0i)")},
    {S_("package c4b; var _ = complex64(0i)"), S_("complex64(0i)"), S_("complex64"),
     S_("(0 + 0i)")},
    {S_("package c4c; type T complex64; var _ = T(0i)"), S_("T(0i)"), S_("c4c.T"),
     S_("(0 + 0i)")},

    {S_("package c5a; var _ = string(\"foo\")"), S_("\"foo\""), S_("string"),
     S_("\"foo\"")},
    {S_("package c5b; var _ = string(\"foo\")"), S_("string(\"foo\")"), S_("string"),
     S_("\"foo\"")},
    {S_("package c5c; type T string; var _ = T(\"foo\")"), S_("T(\"foo\")"),
     S_("c5c.T"), S_("\"foo\"")},
    {S_("package c5d; var _ = string(65)"), S_("65"), S_("untyped int"), S_("65")},
    {S_("package c5e; var _ = string('A')"), S_("'A'"), S_("untyped rune"), S_("65")},
    {S_("package c5f; type T string; var _ = T('A')"), S_("'A'"), S_("untyped rune"),
     S_("65")},

    {S_("package d0; var _ = []byte(\"foo\")"), S_("\"foo\""), S_("string"),
     S_("\"foo\"")},
    {S_("package d1; var _ = []byte(string(\"foo\"))"), S_("\"foo\""), S_("string"),
     S_("\"foo\"")},
    {S_("package d2; var _ = []byte(string(\"foo\"))"), S_("string(\"foo\")"),
     S_("string"), S_("\"foo\"")},
    {S_("package d3; type T []byte; var _ = T(\"foo\")"), S_("\"foo\""), S_("string"),
     S_("\"foo\"")},

    {S_("package e0; const _ = float32( 1e-200)"), S_("float32(1e-200)"), S_("float32"),
     S_("0")},
    {S_("package e1; const _ = float32(-1e-200)"), S_("float32(-1e-200)"),
     S_("float32"), S_("0")},
    {S_("package e2; const _ = float64( 1e-2000)"), S_("float64(1e-2000)"),
     S_("float64"), S_("0")},
    {S_("package e3; const _ = float64(-1e-2000)"), S_("float64(-1e-2000)"),
     S_("float64"), S_("0")},
    {S_("package e4; const _ = complex64( 1e-200)"), S_("complex64(1e-200)"),
     S_("complex64"), S_("(0 + 0i)")},
    {S_("package e5; const _ = complex64(-1e-200)"), S_("complex64(-1e-200)"),
     S_("complex64"), S_("(0 + 0i)")},
    {S_("package e6; const _ = complex128( 1e-2000)"), S_("complex128(1e-2000)"),
     S_("complex128"), S_("(0 + 0i)")},
    {S_("package e7; const _ = complex128(-1e-2000)"), S_("complex128(-1e-2000)"),
     S_("complex128"), S_("(0 + 0i)")},

    {S_("package f0 ; var _ float32 =  1e-200"), S_("1e-200"), S_("float32"), S_("0")},
    {S_("package f1 ; var _ float32 = -1e-200"), S_("-1e-200"), S_("float32"), S_("0")},
    {S_("package f2a; var _ float64 =  1e-2000"), S_("1e-2000"), S_("float64"),
     S_("0")},
    {S_("package f3a; var _ float64 = -1e-2000"), S_("-1e-2000"), S_("float64"),
     S_("0")},
    {S_("package f2b; var _         =  1e-2000"), S_("1e-2000"), S_("float64"),
     S_("0")},
    {S_("package f3b; var _         = -1e-2000"), S_("-1e-2000"), S_("float64"),
     S_("0")},
    {S_("package f4 ; var _ complex64  =  1e-200 "), S_("1e-200"), S_("complex64"),
     S_("(0 + 0i)")},
    {S_("package f5 ; var _ complex64  = -1e-200 "), S_("-1e-200"), S_("complex64"),
     S_("(0 + 0i)")},
    {S_("package f6a; var _ complex128 =  1e-2000i"), S_("1e-2000i"), S_("complex128"),
     S_("(0 + 0i)")},
    {S_("package f7a; var _ complex128 = -1e-2000i"), S_("-1e-2000i"), S_("complex128"),
     S_("(0 + 0i)")},
    {S_("package f6b; var _            =  1e-2000i"), S_("1e-2000i"), S_("complex128"),
     S_("(0 + 0i)")},
    {S_("package f7b; var _            = -1e-2000i"), S_("-1e-2000i"), S_("complex128"),
     S_("(0 + 0i)")},

    {S_("package g0; const (a = len([iota]int{}); b; c); const _ = c"), S_("c"),
     S_("int"), S_("2")}, /* go.dev/issue/22341 */
    {S_("package g1; var(j int32; s int; n = 1.0<<s == j)"), S_("1.0"), S_("int32"),
     S_("1")}, /* go.dev/issue/48422 */
};

static void TestValuesInfo(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(values_tests); i++) {
        Str src = values_tests[i].src, expr_str = values_tests[i].expr;
        Str typ = values_tests[i].typ, val = values_tests[i].val;
        TypesInfo info = {0};
        info.types = new_types_map(a);
        Str name = types_package_name(must_typecheck(a, src, NULL, &info));

        /* look for expression */
        AstExpr expr = NULL;
        TypesTypeAndValue tv = {0};
        const void *k;
        void *v;
        for (MapIter it = map_iter(info.types); map_next(&it, &k, &v);) {
            AstExpr e = *(AstExpr const *)k;
            if (str_eq(types_expr_string(a, e), expr_str)) {
                expr = e;
                tv = *(const TypesTypeAndValue *)v;
                break;
            }
        }
        if (expr == NULL) {
            testing_t_errorf_v(t, "package %s: no expression found for %s", name,
                               expr_str);
            continue;
        }

        /* check that type is correct */
        Str got = type_string(a, tv.type);
        if (!str_eq(got, typ)) {
            testing_t_errorf_v(t, "package %s: got type %s; want %s", name, got, typ);
            continue;
        }

        /* if we have a constant, check that value is correct. Not in Go: the
         * zero ConstantValue, of kind unknown, is Go's nil Value. */
        if (constant_value_kind(tv.value) != CONSTANT_UNKNOWN) {
            got = constant_value_exact_string(tv.value, a);
            if (!str_eq(got, val))
                testing_t_errorf_v(t, "package %s: got value %s; want %s", name, got,
                                   val);
        } else if (val.len != 0) {
            testing_t_errorf_v(t, "package %s: no constant found; want %s", name, val);
        }
    }
    arena_free(&ar);
}

/* Test sources that are not expected to typecheck must start with the broken
 * prefix. */
static const Str types_broken = S_("package broken_");

static const struct {
    Str src;
    Str expr; /* expression */
    Str typ;  /* value type */
} types_tests[] = {
    /* single-valued expressions of untyped constants */
    {S_("package b0; var x interface{} = false"), S_("false"), S_("bool")},
    {S_("package b1; var x interface{} = 0"), S_("0"), S_("int")},
    {S_("package b2; var x interface{} = 0."), S_("0."), S_("float64")},
    {S_("package b3; var x interface{} = 0i"), S_("0i"), S_("complex128")},
    {S_("package b4; var x interface{} = \"foo\""), S_("\"foo\""), S_("string")},

    /* uses of nil */
    {S_("package n0; var _ *int = nil"), S_("nil"), S_("untyped nil")},
    {S_("package n1; var _ func() = nil"), S_("nil"), S_("untyped nil")},
    {S_("package n2; var _ []byte = nil"), S_("nil"), S_("untyped nil")},
    {S_("package n3; var _ map[int]int = nil"), S_("nil"), S_("untyped nil")},
    {S_("package n4; var _ chan int = nil"), S_("nil"), S_("untyped nil")},
    {S_("package n5; var _ interface{} = nil"), S_("nil"), S_("untyped nil")},
    {S_("package n6; import \"unsafe\"; var _ unsafe.Pointer = nil"), S_("nil"),
     S_("untyped nil")},

    {S_("package n10; var (x *int; _ = x == nil)"), S_("nil"), S_("untyped nil")},
    {S_("package n11; var (x func(); _ = x == nil)"), S_("nil"), S_("untyped nil")},
    {S_("package n12; var (x []byte; _ = x == nil)"), S_("nil"), S_("untyped nil")},
    {S_("package n13; var (x map[int]int; _ = x == nil)"), S_("nil"),
     S_("untyped nil")},
    {S_("package n14; var (x chan int; _ = x == nil)"), S_("nil"), S_("untyped nil")},
    {S_("package n15; var (x interface{}; _ = x == nil)"), S_("nil"),
     S_("untyped nil")},
    {S_("package n15; import \"unsafe\"; var (x unsafe.Pointer; _ = x == nil)"),
     S_("nil"), S_("untyped nil")},

    {S_("package n20; var _ = (*int)(nil)"), S_("nil"), S_("untyped nil")},
    {S_("package n21; var _ = (func())(nil)"), S_("nil"), S_("untyped nil")},
    {S_("package n22; var _ = ([]byte)(nil)"), S_("nil"), S_("untyped nil")},
    {S_("package n23; var _ = (map[int]int)(nil)"), S_("nil"), S_("untyped nil")},
    {S_("package n24; var _ = (chan int)(nil)"), S_("nil"), S_("untyped nil")},
    {S_("package n25; var _ = (interface{})(nil)"), S_("nil"), S_("untyped nil")},
    {S_("package n26; import \"unsafe\"; var _ = unsafe.Pointer(nil)"), S_("nil"),
     S_("untyped nil")},

    {S_("package n30; func f(*int) { f(nil) }"), S_("nil"), S_("untyped nil")},
    {S_("package n31; func f(func()) { f(nil) }"), S_("nil"), S_("untyped nil")},
    {S_("package n32; func f([]byte) { f(nil) }"), S_("nil"), S_("untyped nil")},
    {S_("package n33; func f(map[int]int) { f(nil) }"), S_("nil"), S_("untyped nil")},
    {S_("package n34; func f(chan int) { f(nil) }"), S_("nil"), S_("untyped nil")},
    {S_("package n35; func f(interface{}) { f(nil) }"), S_("nil"), S_("untyped nil")},
    {S_("package n35; import \"unsafe\"; func f(unsafe.Pointer) { f(nil) }"), S_("nil"),
     S_("untyped nil")},

    /* comma-ok expressions */
    {
        S_("package p0; var x interface{}; var _, _ = x.(int)"),
        S_("x.(int)"),
        S_("(int, bool)"),
    },
    {
        S_("package p1; var x interface{}; func _() { _, _ = x.(int) }"),
        S_("x.(int)"),
        S_("(int, bool)"),
    },
    {
        S_("package p2a; type mybool bool; var m map[string]complex128; var b mybool; "
           "func _() { _, b = m[\"foo\"] }"),
        S_("m[\"foo\"]"),
        S_("(complex128, p2a.mybool)"),
    },
    {
        S_("package p2b; var m map[string]complex128; var b bool; func _() { _, b = "
           "m[\"foo\"] }"),
        S_("m[\"foo\"]"),
        S_("(complex128, bool)"),
    },
    {
        S_("package p3; var c chan string; var _, _ = <-c"),
        S_("<-c"),
        S_("(string, bool)"),
    },

    /* go.dev/issue/6796 */
    {
        S_("package issue6796_a; var x interface{}; var _, _ = (x.(int))"),
        S_("x.(int)"),
        S_("(int, bool)"),
    },
    {
        S_("package issue6796_b; var c chan string; var _, _ = (<-c)"),
        S_("(<-c)"),
        S_("(string, bool)"),
    },
    {
        S_("package issue6796_c; var c chan string; var _, _ = (<-c)"),
        S_("<-c"),
        S_("(string, bool)"),
    },
    {
        S_("package issue6796_d; var c chan string; var _, _ = ((<-c))"),
        S_("(<-c)"),
        S_("(string, bool)"),
    },
    {
        S_("package issue6796_e; func f(c chan string) { _, _ = ((<-c)) }"),
        S_("(<-c)"),
        S_("(string, bool)"),
    },

    /* go.dev/issue/7060 */
    {
        S_("package issue7060_a; var ( m map[int]string; x, ok = m[0] )"),
        S_("m[0]"),
        S_("(string, bool)"),
    },
    {
        S_("package issue7060_b; var ( m map[int]string; x, ok interface{} = m[0] )"),
        S_("m[0]"),
        S_("(string, bool)"),
    },
    {
        S_("package issue7060_c; func f(x interface{}, ok bool, m map[int]string) { x, "
           "ok = m[0] }"),
        S_("m[0]"),
        S_("(string, bool)"),
    },
    {
        S_("package issue7060_d; var ( ch chan string; x, ok = <-ch )"),
        S_("<-ch"),
        S_("(string, bool)"),
    },
    {
        S_("package issue7060_e; var ( ch chan string; x, ok interface{} = <-ch )"),
        S_("<-ch"),
        S_("(string, bool)"),
    },
    {
        S_("package issue7060_f; func f(x interface{}, ok bool, ch chan string) { x, "
           "ok = <-ch }"),
        S_("<-ch"),
        S_("(string, bool)"),
    },

    /* go.dev/issue/28277 */
    {
        S_("package issue28277_a; func f(...int)"),
        S_("...int"),
        S_("[]int"),
    },
    {
        S_("package issue28277_b; func f(a, b int, c ...[]struct{})"),
        S_("...[]struct{}"),
        S_("[][]struct{}"),
    },

    /* go.dev/issue/47243 */
    {S_("package issue47243_a; var x int32; var _ = x << 3"), S_("3"),
     S_("untyped int")},
    {S_("package issue47243_b; var x int32; var _ = x << 3."), S_("3."),
     S_("untyped float")},
    {S_("package issue47243_c; var x int32; var _ = 1 << x"), S_("1 << x"), S_("int")},
    {S_("package issue47243_d; var x int32; var _ = 1 << x"), S_("1"), S_("int")},
    {S_("package issue47243_e; var x int32; var _ = 1 << 2"), S_("1"),
     S_("untyped int")},
    {S_("package issue47243_f; var x int32; var _ = 1 << 2"), S_("2"),
     S_("untyped int")},
    {S_("package issue47243_g; var x int32; var _ = int(1) << 2"), S_("2"),
     S_("untyped int")},
    {S_("package issue47243_h; var x int32; var _ = 1 << (2 << x)"), S_("1"),
     S_("int")},
    {S_("package issue47243_i; var x int32; var _ = 1 << (2 << x)"), S_("(2 << x)"),
     S_("untyped int")},
    {S_("package issue47243_j; var x int32; var _ = 1 << (2 << x)"), S_("2"),
     S_("untyped int")},

    /* tests for broken code that doesn't type-check */
    {S_("package broken_x0; func _() { var x struct {f string}; x.f := 0 }"), S_("x.f"),
     S_("string")},
    {S_("package broken_x1; func _() { var z string; type x struct {f string}; y := "
        "&x{q: z}}"),
     S_("z"), S_("string")},
    {S_("package broken_x2; func _() { var a, b string; type x struct {f string}; z := "
        "&x{f: a, f: b,}}"),
     S_("b"), S_("string")},
    {S_("package broken_x3; var x = panic(\"\");"), S_("panic"),
     S_("func(interface{})")},
    {S_("package x4; func _() { panic(\"\") }"), S_("panic"), S_("func(interface{})")},
    {S_("package broken_x5; func _() { var x map[string][...]int; x = "
        "map[string][...]int{\"\": {1,2,3}} }"),
     S_("x"), S_("map[string]invalid type")},

    /* parameterized functions */
    {S_("package p0; func f[T any](T) {}; var _ = f[int]"), S_("f"),
     S_("func[T any](T)")},
    {S_("package p1; func f[T any](T) {}; var _ = f[int]"), S_("f[int]"),
     S_("func(int)")},
    {S_("package p2; func f[T any](T) {}; func _() { f(42) }"), S_("f"),
     S_("func(int)")},
    {S_("package p3; func f[T any](T) {}; func _() { f[int](42) }"), S_("f[int]"),
     S_("func(int)")},
    {S_("package p4; func f[T any](T) {}; func _() { f[int](42) }"), S_("f"),
     S_("func[T any](T)")},
    {S_("package p5; func f[T any](T) {}; func _() { f(42) }"), S_("f(42)"), S_("()")},

    /* type parameters */
    {S_("package t0; type t[] int; var _ t"), S_("t"),
     S_("t0.t")}, /* t[] is a syntax error that is ignored in this test in favor of t */
    {S_("package t1; type t[P any] int; var _ t[int]"), S_("t"), S_("t1.t[P any]")},
    {S_("package t2; type t[P interface{}] int; var _ t[int]"), S_("t"),
     S_("t2.t[P interface{}]")},
    {S_("package t3; type t[P, Q interface{}] int; var _ t[int, int]"), S_("t"),
     S_("t3.t[P, Q interface{}]")},
    {S_("package broken_t4; type t[P, Q interface{ m() }] int; var _ t[int, int]"),
     S_("t"), S_("broken_t4.t[P, Q interface{m()}]")},

    /* instantiated types must be sanitized */
    {S_("package g0; type t[P any] int; var x struct{ f t[int] }; var _ = x.f"),
     S_("x.f"), S_("g0.t[int]")},

    /* go.dev/issue/45096 */
    {S_("package issue45096; func _[T interface{ ~int8 | ~int16 | ~int32  }](x T) { _ "
        "= x < 0 }"),
     S_("0"), S_("T")},

    /* go.dev/issue/47895 */
    {S_("package p; import \"unsafe\"; type S struct { f int }; var s S; var _ = "
        "unsafe.Offsetof(s.f)"),
     S_("s.f"), S_("int")},

    /* go.dev/issue/74303. Note that interface field types are synthetic, so */
    /* even though `func()` doesn't appear in the source, it appears in the */
    /* syntax tree. */
    {S_("package p; type T interface { M(int) }"), S_("func(int)"), S_("func(int)")},

    /* go.dev/issue/50093 */
    {S_("package u0a; func _[_ interface{int}]() {}"), S_("int"), S_("int")},
    {S_("package u1a; func _[_ interface{~int}]() {}"), S_("~int"), S_("~int")},
    {S_("package u2a; func _[_ interface{int | string}]() {}"), S_("int | string"),
     S_("int | string")},
    {S_("package u3a; func _[_ interface{int | string | ~bool}]() {}"),
     S_("int | string | ~bool"), S_("int | string | ~bool")},
    {S_("package u3a; func _[_ interface{int | string | ~bool}]() {}"),
     S_("int | string"), S_("int | string")},
    {S_("package u3a; func _[_ interface{int | string | ~bool}]() {}"), S_("~bool"),
     S_("~bool")},
    {S_("package u3a; func _[_ interface{int | string | ~float64|~bool}]() {}"),
     S_("int | string | ~float64"), S_("int | string | ~float64")},

    {S_("package u0b; func _[_ int]() {}"), S_("int"), S_("int")},
    {S_("package u1b; func _[_ ~int]() {}"), S_("~int"), S_("~int")},
    {S_("package u2b; func _[_ int | string]() {}"), S_("int | string"),
     S_("int | string")},
    {S_("package u3b; func _[_ int | string | ~bool]() {}"), S_("int | string | ~bool"),
     S_("int | string | ~bool")},
    {S_("package u3b; func _[_ int | string | ~bool]() {}"), S_("int | string"),
     S_("int | string")},
    {S_("package u3b; func _[_ int | string | ~bool]() {}"), S_("~bool"), S_("~bool")},
    {S_("package u3b; func _[_ int | string | ~float64|~bool]() {}"),
     S_("int | string | ~float64"), S_("int | string | ~float64")},

    {S_("package u0c; type _ interface{int}"), S_("int"), S_("int")},
    {S_("package u1c; type _ interface{~int}"), S_("~int"), S_("~int")},
    {S_("package u2c; type _ interface{int | string}"), S_("int | string"),
     S_("int | string")},
    {S_("package u3c; type _ interface{int | string | ~bool}"),
     S_("int | string | ~bool"), S_("int | string | ~bool")},
    {S_("package u3c; type _ interface{int | string | ~bool}"), S_("int | string"),
     S_("int | string")},
    {S_("package u3c; type _ interface{int | string | ~bool}"), S_("~bool"),
     S_("~bool")},
    {S_("package u3c; type _ interface{int | string | ~float64|~bool}"),
     S_("int | string | ~float64"), S_("int | string | ~float64")},

    /* reverse type inference */
    {S_("package r1; var _ func(int) = g; func g[P any](P) {}"), S_("g"),
     S_("func(int)")},
    {S_("package r2; var _ func(int) = g[int]; func g[P any](P) {}"), S_("g"),
     S_("func[P any](P)")}, /* go.dev/issues/60212 */
    {S_("package r3; var _ func(int) = g[int]; func g[P any](P) {}"), S_("g[int]"),
     S_("func(int)")},
    {S_("package r4; var _ func(int, string) = g; func g[P, Q any](P, Q) {}"), S_("g"),
     S_("func(int, string)")},
    {S_("package r5; var _ func(int, string) = g[int]; func g[P, Q any](P, Q) {}"),
     S_("g"), S_("func[P, Q any](P, Q)")}, /* go.dev/issues/60212 */
    {S_("package r6; var _ func(int, string) = g[int]; func g[P, Q any](P, Q) {}"),
     S_("g[int]"), S_("func(int, string)")},

    {S_("package s1; func _() { f(g) }; func f(func(int)) {}; func g[P any](P) {}"),
     S_("g"), S_("func(int)")},
    {S_("package s2; func _() { f(g[int]) }; func f(func(int)) {}; func g[P any](P) "
        "{}"),
     S_("g"), S_("func[P any](P)")}, /* go.dev/issues/60212 */
    {S_("package s3; func _() { f(g[int]) }; func f(func(int)) {}; func g[P any](P) "
        "{}"),
     S_("g[int]"), S_("func(int)")},
    {S_("package s4; func _() { f(g) }; func f(func(int, string)) {}; func g[P, Q "
        "any](P, Q) {}"),
     S_("g"), S_("func(int, string)")},
    {S_("package s5; func _() { f(g[int]) }; func f(func(int, string)) {}; func g[P, Q "
        "any](P, Q) {}"),
     S_("g"), S_("func[P, Q any](P, Q)")}, /* go.dev/issues/60212 */
    {S_("package s6; func _() { f(g[int]) }; func f(func(int, string)) {}; func g[P, Q "
        "any](P, Q) {}"),
     S_("g[int]"), S_("func(int, string)")},

    {S_("package s7; func _() { f(g, h) }; func f[P any](func(int, P), func(P, "
        "string)) {}; func g[P any](P, P) {}; func h[P, Q any](P, Q) {}"),
     S_("g"), S_("func(int, int)")},
    {S_("package s8; func _() { f(g, h) }; func f[P any](func(int, P), func(P, "
        "string)) {}; func g[P any](P, P) {}; func h[P, Q any](P, Q) {}"),
     S_("h"), S_("func(int, string)")},
    {S_("package s9; func _() { f(g, h[int]) }; func f[P any](func(int, P), func(P, "
        "string)) {}; func g[P any](P, P) {}; func h[P, Q any](P, Q) {}"),
     S_("h"), S_("func[P, Q any](P, Q)")}, /* go.dev/issues/60212 */
    {S_("package s10; func _() { f(g, h[int]) }; func f[P any](func(int, P), func(P, "
        "string)) {}; func g[P any](P, P) {}; func h[P, Q any](P, Q) {}"),
     S_("h[int]"), S_("func(int, string)")},

    /* go.dev/issue/68639 */
    /* parenthesized and pointer type expressions in various positions */
    /* - as variable type, not generic */
    {S_("package qa1; type T int; var x T"), S_("T"), S_("qa1.T")},
    {S_("package qa2; type T int; var x (T)"), S_("T"), S_("qa2.T")},
    {S_("package qa3; type T int; var x (T)"), S_("(T)"), S_("qa3.T")},
    {S_("package qa4; type T int; var x ((T))"), S_("T"), S_("qa4.T")},
    {S_("package qa5; type T int; var x ((T))"), S_("(T)"), S_("qa5.T")},
    {S_("package qa6; type T int; var x ((T))"), S_("((T))"), S_("qa6.T")},
    {S_("package qa7; type T int; var x *T"), S_("T"), S_("qa7.T")},
    {S_("package qa8; type T int; var x *T"), S_("*T"), S_("*qa8.T")},
    {S_("package qa9; type T int; var x (*T)"), S_("T"), S_("qa9.T")},
    {S_("package qa10; type T int; var x (*T)"), S_("*T"), S_("*qa10.T")},
    {S_("package qa11; type T int; var x *(T)"), S_("T"), S_("qa11.T")},
    {S_("package qa12; type T int; var x *(T)"), S_("(T)"), S_("qa12.T")},
    {S_("package qa13; type T int; var x *(T)"), S_("*(T)"), S_("*qa13.T")},
    {S_("package qa14; type T int; var x (*(T))"), S_("(T)"), S_("qa14.T")},
    {S_("package qa15; type T int; var x (*(T))"), S_("*(T)"), S_("*qa15.T")},
    {S_("package qa16; type T int; var x (*(T))"), S_("(*(T))"), S_("*qa16.T")},

    /* - as ordinary function parameter, not generic */
    {S_("package qb1; type T int; func _(T)"), S_("T"), S_("qb1.T")},
    {S_("package qb2; type T int; func _((T))"), S_("T"), S_("qb2.T")},
    {S_("package qb3; type T int; func _((T))"), S_("(T)"), S_("qb3.T")},
    {S_("package qb4; type T int; func _(((T)))"), S_("T"), S_("qb4.T")},
    {S_("package qb5; type T int; func _(((T)))"), S_("(T)"), S_("qb5.T")},
    {S_("package qb6; type T int; func _(((T)))"), S_("((T))"), S_("qb6.T")},
    {S_("package qb7; type T int; func _(*T)"), S_("T"), S_("qb7.T")},
    {S_("package qb8; type T int; func _(*T)"), S_("*T"), S_("*qb8.T")},
    {S_("package qb9; type T int; func _((*T))"), S_("T"), S_("qb9.T")},
    {S_("package qb10; type T int; func _((*T))"), S_("*T"), S_("*qb10.T")},
    {S_("package qb11; type T int; func _(*(T))"), S_("T"), S_("qb11.T")},
    {S_("package qb12; type T int; func _(*(T))"), S_("(T)"), S_("qb12.T")},
    {S_("package qb13; type T int; func _(*(T))"), S_("*(T)"), S_("*qb13.T")},
    {S_("package qb14; type T int; func _((*(T)))"), S_("(T)"), S_("qb14.T")},
    {S_("package qb15; type T int; func _((*(T)))"), S_("*(T)"), S_("*qb15.T")},
    {S_("package qb16; type T int; func _((*(T)))"), S_("(*(T))"), S_("*qb16.T")},

    /* - as method receiver, not generic */
    {S_("package qc1; type T int; func (T) _() {}"), S_("T"), S_("qc1.T")},
    {S_("package qc2; type T int; func ((T)) _() {}"), S_("T"), S_("qc2.T")},
    {S_("package qc3; type T int; func ((T)) _() {}"), S_("(T)"), S_("qc3.T")},
    {S_("package qc4; type T int; func (((T))) _() {}"), S_("T"), S_("qc4.T")},
    {S_("package qc5; type T int; func (((T))) _() {}"), S_("(T)"), S_("qc5.T")},
    {S_("package qc6; type T int; func (((T))) _() {}"), S_("((T))"), S_("qc6.T")},
    {S_("package qc7; type T int; func (*T) _() {}"), S_("T"), S_("qc7.T")},
    {S_("package qc8; type T int; func (*T) _() {}"), S_("*T"), S_("*qc8.T")},
    {S_("package qc9; type T int; func ((*T)) _() {}"), S_("T"), S_("qc9.T")},
    {S_("package qc10; type T int; func ((*T)) _() {}"), S_("*T"), S_("*qc10.T")},
    {S_("package qc11; type T int; func (*(T)) _() {}"), S_("T"), S_("qc11.T")},
    {S_("package qc12; type T int; func (*(T)) _() {}"), S_("(T)"), S_("qc12.T")},
    {S_("package qc13; type T int; func (*(T)) _() {}"), S_("*(T)"), S_("*qc13.T")},
    {S_("package qc14; type T int; func ((*(T))) _() {}"), S_("(T)"), S_("qc14.T")},
    {S_("package qc15; type T int; func ((*(T))) _() {}"), S_("*(T)"), S_("*qc15.T")},
    {S_("package qc16; type T int; func ((*(T))) _() {}"), S_("(*(T))"), S_("*qc16.T")},

    /* - as variable type, generic */
    {S_("package qd1; type T[_ any] int; var x T[int]"), S_("T"), S_("qd1.T[_ any]")},
    {S_("package qd2; type T[_ any] int; var x (T[int])"), S_("T[int]"),
     S_("qd2.T[int]")},
    {S_("package qd3; type T[_ any] int; var x (T[int])"), S_("(T[int])"),
     S_("qd3.T[int]")},
    {S_("package qd4; type T[_ any] int; var x ((T[int]))"), S_("T"),
     S_("qd4.T[_ any]")},
    {S_("package qd5; type T[_ any] int; var x ((T[int]))"), S_("(T[int])"),
     S_("qd5.T[int]")},
    {S_("package qd6; type T[_ any] int; var x ((T[int]))"), S_("((T[int]))"),
     S_("qd6.T[int]")},
    {S_("package qd7; type T[_ any] int; var x *T[int]"), S_("T"), S_("qd7.T[_ any]")},
    {S_("package qd8; type T[_ any] int; var x *T[int]"), S_("*T[int]"),
     S_("*qd8.T[int]")},
    {S_("package qd9; type T[_ any] int; var x (*T[int])"), S_("T"),
     S_("qd9.T[_ any]")},
    {S_("package qd10; type T[_ any] int; var x (*T[int])"), S_("*T[int]"),
     S_("*qd10.T[int]")},
    {S_("package qd11; type T[_ any] int; var x *(T[int])"), S_("T[int]"),
     S_("qd11.T[int]")},
    {S_("package qd12; type T[_ any] int; var x *(T[int])"), S_("(T[int])"),
     S_("qd12.T[int]")},
    {S_("package qd13; type T[_ any] int; var x *(T[int])"), S_("*(T[int])"),
     S_("*qd13.T[int]")},
    {S_("package qd14; type T[_ any] int; var x (*(T[int]))"), S_("(T[int])"),
     S_("qd14.T[int]")},
    {S_("package qd15; type T[_ any] int; var x (*(T[int]))"), S_("*(T[int])"),
     S_("*qd15.T[int]")},
    {S_("package qd16; type T[_ any] int; var x (*(T[int]))"), S_("(*(T[int]))"),
     S_("*qd16.T[int]")},

    /* - as ordinary function parameter, generic */
    {S_("package qe1; type T[_ any] int; func _(T[int])"), S_("T"), S_("qe1.T[_ any]")},
    {S_("package qe2; type T[_ any] int; func _((T[int]))"), S_("T[int]"),
     S_("qe2.T[int]")},
    {S_("package qe3; type T[_ any] int; func _((T[int]))"), S_("(T[int])"),
     S_("qe3.T[int]")},
    {S_("package qe4; type T[_ any] int; func _(((T[int])))"), S_("T"),
     S_("qe4.T[_ any]")},
    {S_("package qe5; type T[_ any] int; func _(((T[int])))"), S_("(T[int])"),
     S_("qe5.T[int]")},
    {S_("package qe6; type T[_ any] int; func _(((T[int])))"), S_("((T[int]))"),
     S_("qe6.T[int]")},
    {S_("package qe7; type T[_ any] int; func _(*T[int])"), S_("T"),
     S_("qe7.T[_ any]")},
    {S_("package qe8; type T[_ any] int; func _(*T[int])"), S_("*T[int]"),
     S_("*qe8.T[int]")},
    {S_("package qe9; type T[_ any] int; func _((*T[int]))"), S_("T"),
     S_("qe9.T[_ any]")},
    {S_("package qe10; type T[_ any] int; func _((*T[int]))"), S_("*T[int]"),
     S_("*qe10.T[int]")},
    {S_("package qe11; type T[_ any] int; func _(*(T[int]))"), S_("T[int]"),
     S_("qe11.T[int]")},
    {S_("package qe12; type T[_ any] int; func _(*(T[int]))"), S_("(T[int])"),
     S_("qe12.T[int]")},
    {S_("package qe13; type T[_ any] int; func _(*(T[int]))"), S_("*(T[int])"),
     S_("*qe13.T[int]")},
    {S_("package qe14; type T[_ any] int; func _((*(T[int])))"), S_("(T[int])"),
     S_("qe14.T[int]")},
    {S_("package qe15; type T[_ any] int; func _((*(T[int])))"), S_("*(T[int])"),
     S_("*qe15.T[int]")},
    {S_("package qe16; type T[_ any] int; func _((*(T[int])))"), S_("(*(T[int]))"),
     S_("*qe16.T[int]")},

    /* - as method receiver, generic */
    {S_("package qf1; type T[_ any] int; func (T[_]) _() {}"), S_("T"),
     S_("qf1.T[_ any]")},
    {S_("package qf2; type T[_ any] int; func ((T[_])) _() {}"), S_("T[_]"),
     S_("qf2.T[_]")},
    {S_("package qf3; type T[_ any] int; func ((T[_])) _() {}"), S_("(T[_])"),
     S_("qf3.T[_]")},
    {S_("package qf4; type T[_ any] int; func (((T[_]))) _() {}"), S_("T"),
     S_("qf4.T[_ any]")},
    {S_("package qf5; type T[_ any] int; func (((T[_]))) _() {}"), S_("(T[_])"),
     S_("qf5.T[_]")},
    {S_("package qf6; type T[_ any] int; func (((T[_]))) _() {}"), S_("((T[_]))"),
     S_("qf6.T[_]")},
    {S_("package qf7; type T[_ any] int; func (*T[_]) _() {}"), S_("T"),
     S_("qf7.T[_ any]")},
    {S_("package qf8; type T[_ any] int; func (*T[_]) _() {}"), S_("*T[_]"),
     S_("*qf8.T[_]")},
    {S_("package qf9; type T[_ any] int; func ((*T[_])) _() {}"), S_("T"),
     S_("qf9.T[_ any]")},
    {S_("package qf10; type T[_ any] int; func ((*T[_])) _() {}"), S_("*T[_]"),
     S_("*qf10.T[_]")},
    {S_("package qf11; type T[_ any] int; func (*(T[_])) _() {}"), S_("T[_]"),
     S_("qf11.T[_]")},
    {S_("package qf12; type T[_ any] int; func (*(T[_])) _() {}"), S_("(T[_])"),
     S_("qf12.T[_]")},
    {S_("package qf13; type T[_ any] int; func (*(T[_])) _() {}"), S_("*(T[_])"),
     S_("*qf13.T[_]")},
    {S_("package qf14; type T[_ any] int; func ((*(T[_]))) _() {}"), S_("(T[_])"),
     S_("qf14.T[_]")},
    {S_("package qf15; type T[_ any] int; func ((*(T[_]))) _() {}"), S_("*(T[_])"),
     S_("*qf15.T[_]")},
    {S_("package qf16; type T[_ any] int; func ((*(T[_]))) _() {}"), S_("(*(T[_]))"),
     S_("*qf16.T[_]")},

    /* For historic reasons, type parameters in receiver type expressions */
    /* are considered both definitions and uses and thus also show up in */
    /* the Info.Types map (see go.dev/issue/68670). */
    {S_("package t1; type T[_ any] int; func (T[P]) _() {}"), S_("P"), S_("P")},
    {S_("package t2; type T[_, _ any] int; func (T[P, Q]) _() {}"), S_("P"), S_("P")},
    {S_("package t3; type T[_, _ any] int; func (T[P, Q]) _() {}"), S_("Q"), S_("Q")},
};

static void TestTypesInfo(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(types_tests); i++) {
        Str src = types_tests[i].src, expr = types_tests[i].expr;
        Str want = types_tests[i].typ;
        TypesInfo info = {0};
        info.types = new_types_map(a);
        Str name = S("");
        if (strings_has_prefix(src, types_broken)) {
            Error err = BURROW_NO_ERROR;
            TypesPackage *pkg = typecheck(a, src, NULL, &info, &err);
            if (BURROW_OK(err)) {
                testing_t_errorf_v(t, "package %s: expected to fail but passed",
                                   types_package_name(pkg));
                continue;
            }
            if (pkg != NULL)
                name = types_package_name(pkg);
        } else {
            name = types_package_name(must_typecheck(a, src, NULL, &info));
        }

        /* look for expression type */
        TypesType typ = NULL;
        const void *k;
        void *v;
        for (MapIter it = map_iter(info.types); map_next(&it, &k, &v);) {
            if (str_eq(types_expr_string(a, *(AstExpr const *)k), expr)) {
                typ = ((const TypesTypeAndValue *)v)->type;
                break;
            }
        }
        if (typ == NULL) {
            testing_t_errorf_v(t, "package %s: no type found for %s", name, expr);
            continue;
        }

        /* check that type is correct */
        Str got = type_string(a, typ);
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "package %s: expr = %s: got %s; want %s", name, expr,
                               got, want);
    }
    arena_free(&ar);
}

/* testImporter: the packages it can import, by path. */
static TypesPackage *test_importer_import(void *self, Str path, Error *err) {
    Map *m = self;
    TypesPackage *pkg = NULL;
    if (map_get2(m, &path, &pkg) && pkg != NULL)
        return pkg;
    *err = fmt_errorf_v("package %q not found", path);
    return NULL;
}

static const TypesImporterVT test_importer_vt = {NULL, test_importer_import, NULL};

static Map *new_test_importer(Alloc *a) {
    return map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
}

static TypesImporter test_importer(Map *m) {
    return (TypesImporter){&test_importer_vt, m};
}

enum { MAX_INSTS = 3, MAX_TARGS = 3 };

typedef struct {
    Str name;
    Str targs[MAX_TARGS];
    Str typ;
} TestInst;

static const Str instance_lib =
    S_("package lib\n\nfunc F[P any](P) {}\n\ntype T[P any] []P\n");

static const struct {
    Str src;
    TestInst instances[MAX_INSTS]; /* recorded instances in source order */
} instance_tests[] = {
    {
        S_("package p0; func f[T any](T) {}; func _() { f(42) }"),
        {{S_("f"), {S_("int")}, S_("func(int)")}},
    },
    {
        S_("package p1; func f[T any](T) T { panic(0) }; func _() { f('@') }"),
        {{S_("f"), {S_("rune")}, S_("func(rune) rune")}},
    },
    {
        S_("package p2; func f[T any](...T) T { panic(0) }; func _() { f(0i) }"),
        {{S_("f"), {S_("complex128")}, S_("func(...complex128) complex128")}},
    },
    {
        S_("package p3; func f[A, B, C any](A, *B, []C) {}; func _() { f(1.2, "
           "new(string), []byte{}) }"),
        {{S_("f"),
          {S_("float64"), S_("string"), S_("byte")},
          S_("func(float64, *string, []byte)")}},
    },
    {
        S_("package p4; func f[A, B any](A, *B, ...[]B) {}; func _() { f(1.2, "
           "new(byte)) }"),
        {{S_("f"), {S_("float64"), S_("byte")}, S_("func(float64, *byte, ...[]byte)")}},
    },

    {
        S_("package s1; func f[T any, P interface{*T}](x T) {}; func _(x string) { "
           "f(x) }"),
        {{S_("f"), {S_("string"), S_("*string")}, S_("func(x string)")}},
    },
    {
        S_("package s2; func f[T any, P interface{*T}](x []T) {}; func _(x []int) { "
           "f(x) }"),
        {{S_("f"), {S_("int"), S_("*int")}, S_("func(x []int)")}},
    },
    {
        S_("package s3; type C[T any] interface{chan<- T}; func f[T any, P C[T]](x "
           "[]T) {}; func _(x []int) { f(x) }"),
        {
            {S_("C"), {S_("T")}, S_("interface{chan<- T}")},
            {S_("f"), {S_("int"), S_("chan<- int")}, S_("func(x []int)")},
        },
    },
    {
        S_("package s4; type C[T any] interface{chan<- T}; func f[T any, P C[T], Q "
           "C[[]*P]](x []T) {}; func _(x []int) { f(x) }"),
        {
            {S_("C"), {S_("T")}, S_("interface{chan<- T}")},
            {S_("C"), {S_("[]*P")}, S_("interface{chan<- []*P}")},
            {S_("f"),
             {S_("int"), S_("chan<- int"), S_("chan<- []*chan<- int")},
             S_("func(x []int)")},
        },
    },

    {
        S_("package t1; func f[T any, P interface{*T}]() T { panic(0) }; func _() { _ "
           "= f[string] }"),
        {{S_("f"), {S_("string"), S_("*string")}, S_("func() string")}},
    },
    {
        S_("package t2; func f[T any, P interface{*T}]() T { panic(0) }; func _() { _ "
           "= (f[string]) }"),
        {{S_("f"), {S_("string"), S_("*string")}, S_("func() string")}},
    },
    {
        S_("package t3; type C[T any] interface{chan<- T}; func f[T any, P C[T], Q "
           "C[[]*P]]() []T { return nil }; func _() { _ = f[int] }"),
        {
            {S_("C"), {S_("T")}, S_("interface{chan<- T}")},
            {S_("C"), {S_("[]*P")}, S_("interface{chan<- []*P}")},
            {S_("f"),
             {S_("int"), S_("chan<- int"), S_("chan<- []*chan<- int")},
             S_("func() []int")},
        },
    },
    {
        S_("package t4; type C[T any] interface{chan<- T}; func f[T any, P C[T], Q "
           "C[[]*P]]() []T { return nil }; func _() { _ = (f[int]) }"),
        {
            {S_("C"), {S_("T")}, S_("interface{chan<- T}")},
            {S_("C"), {S_("[]*P")}, S_("interface{chan<- []*P}")},
            {S_("f"),
             {S_("int"), S_("chan<- int"), S_("chan<- []*chan<- int")},
             S_("func() []int")},
        },
    },
    {
        S_("package i0; import \"lib\"; func _() { lib.F(42) }"),
        {{S_("F"), {S_("int")}, S_("func(int)")}},
    },

    {
        S_("package duplfunc0; func f[T any](T) {}; func _() { f(42); f(\"foo\"); "
           "f[int](3) }"),
        {
            {S_("f"), {S_("int")}, S_("func(int)")},
            {S_("f"), {S_("string")}, S_("func(string)")},
            {S_("f"), {S_("int")}, S_("func(int)")},
        },
    },
    {
        S_("package duplfunc1; import \"lib\"; func _() { lib.F(42); lib.F(\"foo\"); "
           "lib.F(3) }"),
        {
            {S_("F"), {S_("int")}, S_("func(int)")},
            {S_("F"), {S_("string")}, S_("func(string)")},
            {S_("F"), {S_("int")}, S_("func(int)")},
        },
    },

    {
        S_("package type0; type T[P interface{~int}] struct{ x P }; var _ T[int]"),
        {{S_("T"), {S_("int")}, S_("struct{x int}")}},
    },
    {
        S_("package type1; type T[P interface{~int}] struct{ x P }; var _ (T[int])"),
        {{S_("T"), {S_("int")}, S_("struct{x int}")}},
    },
    {
        S_("package type2; type T[P interface{~int}] struct{ x P }; var _ T[(int)]"),
        {{S_("T"), {S_("int")}, S_("struct{x int}")}},
    },
    {
        S_("package type3; type T[P1 interface{~[]P2}, P2 any] struct{ x P1; y P2 }; "
           "var _ T[[]int, int]"),
        {{S_("T"), {S_("[]int"), S_("int")}, S_("struct{x []int; y int}")}},
    },
    {
        S_("package type4; import \"lib\"; var _ lib.T[int]"),
        {{S_("T"), {S_("int")}, S_("[]int")}},
    },

    {
        S_("package dupltype0; type T[P interface{~int}] struct{ x P }; var x T[int]; "
           "var y T[int]"),
        {
            {S_("T"), {S_("int")}, S_("struct{x int}")},
            {S_("T"), {S_("int")}, S_("struct{x int}")},
        },
    },
    {
        S_("package dupltype1; type T[P ~int] struct{ x P }; func (r *T[Q]) add(z "
           "T[Q]) { r.x += z.x }"),
        {
            {S_("T"), {S_("Q")}, S_("struct{x Q}")},
            {S_("T"), {S_("Q")}, S_("struct{x Q}")},
        },
    },
    {
        S_("package dupltype1; import \"lib\"; var x lib.T[int]; var y lib.T[int]; var "
           "z lib.T[string]"),
        {
            {S_("T"), {S_("int")}, S_("[]int")},
            {S_("T"), {S_("int")}, S_("[]int")},
            {S_("T"), {S_("string")}, S_("[]string")},
        },
    },
    {
        S_("package issue51803; func foo[T any](T) {}; func _() { foo[int]( /* leave "
           "arg away on purpose */ ) }"),
        {{S_("foo"), {S_("int")}, S_("func(int)")}},
    },

    /* reverse type inference */
    {
        S_("package reverse1a; var f func(int) = g; func g[P any](P) {}"),
        {{S_("g"), {S_("int")}, S_("func(int)")}},
    },
    {
        S_("package reverse1b; func f(func(int)) {}; func g[P any](P) {}; func _() { "
           "f(g) }"),
        {{S_("g"), {S_("int")}, S_("func(int)")}},
    },
    {
        S_("package reverse2a; var f func(int, string) = g; func g[P, Q any](P, Q) {}"),
        {{S_("g"), {S_("int"), S_("string")}, S_("func(int, string)")}},
    },
    {
        S_("package reverse2b; func f(func(int, string)) {}; func g[P, Q any](P, Q) "
           "{}; func _() { f(g) }"),
        {{S_("g"), {S_("int"), S_("string")}, S_("func(int, string)")}},
    },
    {
        S_("package reverse2c; func f(func(int, string)) {}; func g[P, Q any](P, Q) "
           "{}; func _() { f(g[int]) }"),
        {{S_("g"), {S_("int"), S_("string")}, S_("func(int, string)")}},
    },
    /* reverse3a not possible (cannot assign to generic function outside of argument passing) */
    {
        S_("package reverse3b; func f[R any](func(int) R) {}; func g[P any](P) string "
           "{ return \"\" }; func _() { f(g) }"),
        {
            {S_("f"), {S_("string")}, S_("func(func(int) string)")},
            {S_("g"), {S_("int")}, S_("func(int) string")},
        },
    },
    {
        S_("package reverse4a; var _, _ func([]int, *float32) = g, h; func g[P, Q "
           "any]([]P, *Q) {}; func h[R any]([]R, *float32) {}"),
        {
            {S_("g"), {S_("int"), S_("float32")}, S_("func([]int, *float32)")},
            {S_("h"), {S_("int")}, S_("func([]int, *float32)")},
        },
    },
    {
        S_("package reverse4b; func f(_, _ func([]int, *float32)) {}; func g[P, Q "
           "any]([]P, *Q) {}; func h[R any]([]R, *float32) {}; func _() { f(g, h) }"),
        {
            {S_("g"), {S_("int"), S_("float32")}, S_("func([]int, *float32)")},
            {S_("h"), {S_("int")}, S_("func([]int, *float32)")},
        },
    },
    {
        S_("package issue59956; func f(func(int), func(string), func(bool)) {}; func "
           "g[P any](P) {}; func _() { f(g, g, g) }"),
        {
            {S_("g"), {S_("int")}, S_("func(int)")},
            {S_("g"), {S_("string")}, S_("func(string)")},
            {S_("g"), {S_("bool")}, S_("func(bool)")},
        },
    },
};

typedef struct {
    AstIdent *ident;
    TypesInstance inst;
} RecordedInstance;

static int cmp_recorded_instance(void *env, const void *x, const void *y) {
    (void)env;
    TokenPos p = (*(RecordedInstance *const *)x)->ident->name_pos;
    TokenPos q = (*(RecordedInstance *const *)y)->ident->name_pos;
    return p < q ? -1 : p > q;
}

/* Not in Go: the instances are a []*recordedInstance. */
static Slice sorted_instances(Alloc *a, Map *m) {
    Slice instances = slice_nil(TYPE_UNSAFE_POINTER);
    const void *k;
    void *v;
    for (MapIter it = map_iter(m); map_next(&it, &k, &v);) {
        RecordedInstance *r = BURROW_NEW(a, RecordedInstance);
        r->ident = *(AstIdent *const *)k;
        r->inst = *(const TypesInstance *)v;
        instances = slice_append(a, instances, &r, 1);
    }
    slices_sort_func(instances, BURROW_FN(SlicesCmpFunc, cmp_recorded_instance, NULL));
    return instances;
}

typedef struct {
    Alloc *a;
    TypesPackage *pkg;
    Map *inst_map;
    Map *use_map;
    Int test;
} InstanceEnv;

static void instance_info_run(void *env, TestingT *t) {
    InstanceEnv *e = env;
    Alloc *a = e->a;
    const TestInst *want = instance_tests[e->test].instances;
    Int nwant = 0;
    while (nwant < MAX_INSTS && want[nwant].name.len != 0)
        nwant++;

    /* Sort instances in source order for stability. */
    Slice instances = sorted_instances(a, e->inst_map);
    if (instances.len != nwant)
        testing_t_fatalf_v(t, "got %d instances, want %d", instances.len, nwant);

    /* Pairwise compare with the expected instances. */
    for (Int ii = 0; ii < instances.len; ii++) {
        const RecordedInstance *inst = BURROW_AT(RecordedInstance *, instances, ii);
        Slice targs = slice_nil(TYPE_UNSAFE_POINTER);
        for (Int i = 0; i < types_type_list_len(inst->inst.type_args); i++) {
            TypesType targ = types_type_list_at(inst->inst.type_args, i);
            targs = slice_append(a, targs, &targ, 1);
        }
        TypesType typ = inst->inst.type;

        const TestInst *test_inst = &want[ii];
        Int ntargs = 0;
        while (ntargs < MAX_TARGS && test_inst->targs[ntargs].len != 0)
            ntargs++;
        if (!str_eq(inst->ident->name, test_inst->name))
            testing_t_fatalf_v(t, "got name %s, want %s", inst->ident->name,
                               test_inst->name);
        if (targs.len != ntargs)
            testing_t_fatalf_v(t, "got %d type arguments; want %d", targs.len, ntargs);
        for (Int i = 0; i < targs.len; i++) {
            Str got = type_string(a, BURROW_AT(TypesType, targs, i));
            if (!str_eq(got, test_inst->targs[i]))
                testing_t_errorf_v(t, "type argument %d: got %s; want %s", i, got,
                                   test_inst->targs[i]);
        }
        Str got = type_string(a, types_type_underlying(typ));
        if (!str_eq(got, test_inst->typ))
            testing_t_errorf_v(t, "package %s: got %s; want %s",
                               types_package_name(e->pkg), got, test_inst->typ);

        /* Verify the invariant that re-instantiating the corresponding generic
         * type with TypeArgs results in an identical instance. */
        TypesObject obj = NULL;
        (void)map_get2(e->use_map, &inst->ident, &obj);
        TypesType ptype = types_object_type(obj);
        TypesTypeParamList *tparams = NULL;
        if (types_as_named(ptype) != NULL)
            tparams = types_named_type_params(types_as_named(ptype));
        else if (types_as_signature(ptype) != NULL)
            tparams = types_signature_type_params(types_as_signature(ptype));
        else if (types_as_alias(ptype) != NULL)
            tparams = types_alias_type_params(types_as_alias(ptype));
        if (tparams == NULL || types_type_param_list_len(tparams) == 0)
            testing_t_fatalf_v(t, "info.Types[%s] = %s, want parameterized type",
                               inst->ident->name, type_string(a, ptype));
        Error err = BURROW_NO_ERROR;
        TypesType inst2 = types_instantiate(a, NULL, ptype, targs, true, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Instantiate(%s, %d type arguments) failed: %v",
                               type_string(a, ptype), targs.len, err);
        if (!types_identical(inst->inst.type, inst2))
            testing_t_errorf_v(t, "%s and %s are not identical",
                               type_string(a, inst->inst.type), type_string(a, inst2));
    }
}

static void TestInstanceInfo(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(instance_tests); i++) {
        Map *imports = new_test_importer(a);
        TypesConfig conf = {0};
        conf.importer = test_importer(imports);
        TypesInfo info = {0};
        info.instances = map_make(a, TYPE_UNSAFE_POINTER, TYPE_TYPES_INSTANCE, 0);
        info.uses = new_objects_map(a);
        Str srcs[2] = {instance_lib, instance_tests[i].src};
        TypesPackage *pkg = NULL;
        for (int j = 0; j < 2; j++) {
            Error err = BURROW_NO_ERROR;
            pkg = typecheck(a, srcs[j], &conf, &info, &err);
            /* allow error for issue51803 */
            if (BURROW_FAILED(err) &&
                (pkg == NULL || !str_eq(types_package_name(pkg), S("issue51803")))) {
                testing_t_error_v(t, error_text(err));
                arena_free(&ar);
                return;
            }
            Str pname = types_package_name(pkg);
            map_set(imports, &pname, &pkg);
        }

        InstanceEnv env = {a, pkg, info.instances, info.uses, i};
        testing_t_run(t, types_package_name(pkg),
                      BURROW_FN(TestingTFunc, instance_info_run, &env));
    }
    arena_free(&ar);
}

static const struct {
    Str src;
    Str obj;
    Str want;
} defs_tests[] = {
    {S_("package p0; const x = 42"), S_("x"), S_("const p0.x untyped int")},
    {S_("package p1; const x int = 42"), S_("x"), S_("const p1.x int")},
    {S_("package p2; var x int"), S_("x"), S_("var p2.x int")},
    {S_("package p3; type x int"), S_("x"), S_("type p3.x int")},
    {S_("package p4; func f()"), S_("f"), S_("func p4.f()")},
    {S_("package p5; func f() int { x, _ := 1, 2; return x }"), S_("_"),
     S_("var _ int")},

    /* Tests using generics. */
    {S_("package g0; type x[T any] int"), S_("x"), S_("type g0.x[T any] int")},
    {S_("package g1; func f[T any]() {}"), S_("f"), S_("func g1.f[T any]()")},
    {S_("package g2; type x[T any] int; func (*x[_]) m() {}"), S_("m"),
     S_("func (*g2.x[_]).m()")},

    /* Type parameters in receiver type expressions are definitions. */
    {S_("package r0; type T[_ any] int; func (T[P]) _() {}"), S_("P"),
     S_("type parameter P any")},
    {S_("package r1; type T[_, _ any] int; func (T[P, Q]) _() {}"), S_("P"),
     S_("type parameter P any")},
    {S_("package r2; type T[_, _ any] int; func (T[P, Q]) _() {}"), S_("Q"),
     S_("type parameter Q any")},
};

/* object_string: Object.String, made in a. */
static Str object_string(Alloc *a, TypesObject obj) {
    return types_object_string(a, obj, TYPES_NO_QUALIFIER);
}

static void TestDefsInfo(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(defs_tests); i++) {
        TypesInfo info = {0};
        info.defs = new_objects_map(a);
        Str name =
            types_package_name(must_typecheck(a, defs_tests[i].src, NULL, &info));

        /* find object */
        TypesObject def = NULL;
        const void *k;
        void *v;
        for (MapIter it = map_iter(info.defs); map_next(&it, &k, &v);) {
            const AstIdent *id = *(AstIdent *const *)k;
            if (str_eq(id->name, defs_tests[i].obj)) {
                def = *(TypesObject *)v;
                break;
            }
        }
        if (def == NULL) {
            testing_t_errorf_v(t, "package %s: %s not found", name, defs_tests[i].obj);
            continue;
        }

        Str got = object_string(a, def);
        if (!str_eq(got, defs_tests[i].want))
            testing_t_errorf_v(t, "package %s: got %s; want %s", name, got,
                               defs_tests[i].want);
    }
    arena_free(&ar);
}

static const struct {
    Str src;
    Str obj;
    Str want;
} uses_tests[] = {
    {S_("package p0; func _() { _ = x }; const x = 42"), S_("x"),
     S_("const p0.x untyped int")},
    {S_("package p1; func _() { _ = x }; const x int = 42"), S_("x"),
     S_("const p1.x int")},
    {S_("package p2; func _() { _ = x }; var x int"), S_("x"), S_("var p2.x int")},
    {S_("package p3; func _() { type _ x }; type x int"), S_("x"), S_("type p3.x int")},
    {S_("package p4; func _() { _ = f }; func f()"), S_("f"), S_("func p4.f()")},

    /* Tests using generics. */
    {S_("package g0; func _[T any]() { _ = x }; const x = 42"), S_("x"),
     S_("const g0.x untyped int")},
    {S_("package g1; func _[T any](x T) { }"), S_("T"), S_("type parameter T any")},
    {S_("package g2; type N[A any] int; var _ N[int]"), S_("N"),
     S_("type g2.N[A any] int")},
    {S_("package g3; type N[A any] int; func (N[_]) m() {}"), S_("N"),
     S_("type g3.N[A any] int")},

    /* Uses of fields are instantiated. */
    {S_("package s1; type N[A any] struct{ a A }; var f = N[int]{}.a"), S_("a"),
     S_("field a int")},
    {S_("package s2; type N[A any] struct{ a A }; func (r N[B]) m(b B) { r.a = b }"),
     S_("a"), S_("field a B")},

    /* Uses of methods are uses of the instantiated method. */
    {S_("package m0; type N[A any] int; func (r N[B]) m() { r.n() }; func (N[C]) n() "
        "{}"),
     S_("n"), S_("func (m0.N[B]).n()")},
    {S_("package m1; type N[A any] int; func (r N[B]) m() { }; var f = N[int].m"),
     S_("m"), S_("func (m1.N[int]).m()")},
    {S_("package m2; func _[A any](v interface{ m() A }) { v.m() }"), S_("m"),
     S_("func (interface).m() A")},
    {S_("package m3; func f[A any]() interface{ m() A } { return nil }; var _ = "
        "f[int]().m()"),
     S_("m"), S_("func (interface).m() int")},
    {S_("package m4; type T[A any] func() interface{ m() A }; var x T[int]; var y = "
        "x().m"),
     S_("m"), S_("func (interface).m() int")},
    {S_("package m5; type T[A any] interface{ m() A }; func _[B any](t T[B]) { t.m() "
        "}"),
     S_("m"), S_("func (m5.T[B]).m() B")},
    {S_("package m6; type T[A any] interface{ m() }; func _[B any](t T[B]) { t.m() }"),
     S_("m"), S_("func (m6.T[B]).m()")},
    {S_("package m7; type T[A any] interface{ m() A }; func _(t T[int]) { t.m() }"),
     S_("m"), S_("func (m7.T[int]).m() int")},
    {S_("package m8; type T[A any] interface{ m() }; func _(t T[int]) { t.m() }"),
     S_("m"), S_("func (m8.T[int]).m()")},
    {S_("package m9; type T[A any] interface{ m() }; func _(t T[int]) { _ = t.m }"),
     S_("m"), S_("func (m9.T[int]).m()")},
    {
        S_("package m10; type E[A any] interface{ m() }; type T[B any] interface{ "
           "E[B]; n() }; func _(t T[int]) { t.m() }"),
        S_("m"),
        S_("func (m10.E[int]).m()"),
    },
    {S_("package m11; type T[A any] interface{ m(); n() }; func _(t1 T[int], t2 "
        "T[string]) { t1.m(); t2.n() }"),
     S_("m"), S_("func (m11.T[int]).m()")},
    {S_("package m12; type T[A any] interface{ m(); n() }; func _(t1 T[int], t2 "
        "T[string]) { t1.m(); t2.n() }"),
     S_("n"), S_("func (m12.T[string]).n()")},

    /* For historic reasons, type parameters in receiver type expressions */
    /* are considered both definitions and uses (see go.dev/issue/68670). */
    {S_("package r0; type T[_ any] int; func (T[P]) _() {}"), S_("P"),
     S_("type parameter P any")},
    {S_("package r1; type T[_, _ any] int; func (T[P, Q]) _() {}"), S_("P"),
     S_("type parameter P any")},
    {S_("package r2; type T[_, _ any] int; func (T[P, Q]) _() {}"), S_("Q"),
     S_("type parameter Q any")},
};

static void TestUsesInfo(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(uses_tests); i++) {
        TypesInfo info = {0};
        info.uses = new_objects_map(a);
        Str name =
            types_package_name(must_typecheck(a, uses_tests[i].src, NULL, &info));

        /* find object */
        TypesObject use = NULL;
        const void *k;
        void *v;
        for (MapIter it = map_iter(info.uses); map_next(&it, &k, &v);) {
            const AstIdent *id = *(AstIdent *const *)k;
            if (str_eq(id->name, uses_tests[i].obj)) {
                if (use != NULL)
                    panic_str(fmt_sprintf_v(a, "multiple uses of %q", id->name));
                use = *(TypesObject *)v;
            }
        }
        if (use == NULL) {
            testing_t_errorf_v(t, "package %s: %s not found", name, uses_tests[i].obj);
            continue;
        }

        Str got = object_string(a, use);
        if (!str_eq(got, uses_tests[i].want))
            testing_t_errorf_v(t, "package %s: got %s; want %s", name, got,
                               uses_tests[i].want);
    }
    arena_free(&ar);
}

/* must_func: obj.(*Func), which panics if obj is something else. */
static TypesFunc *must_func(TypesObject obj) {
    TypesFunc *f = obj != NULL ? types_as_func(obj) : NULL;
    if (f == NULL)
        panic_str(S("interface conversion: object is not *types.Func"));
    return f;
}

static TypesObject lookup_object(Map *m, const void *key) {
    TypesObject obj = NULL;
    (void)map_get2(m, &key, &obj);
    return obj;
}

typedef struct {
    TestingT *t;
    Alloc *a;
    TypesInfo *info;
    TypesFunc *dmm, *dmn; /* the methods used in the body of m */
} GenericMethodEnv;

static bool generic_method_inspect(void *penv, AstNode n) {
    GenericMethodEnv *env = penv;
    if (n != NULL && n->kind == AST_KIND_CALL_EXPR) {
        AstNode fun = ((AstCallExpr *)n)->fun;
        if (fun->kind != AST_KIND_SELECTOR_EXPR)
            panic_str(S("interface conversion: ast.Expr is not *ast.SelectorExpr"));
        AstSelectorExpr *sel = (AstSelectorExpr *)fun;
        TypesFunc *use = must_func(lookup_object(env->info->uses, sel->sel));
        TypesSelection *selection = NULL;
        (void)map_get2(env->info->selections, &sel, &selection);
        if (types_selection_kind(selection) != TYPES_METHOD_VAL)
            testing_t_errorf_v(env->t, "Selection kind = %v, want %v",
                               (Int)types_selection_kind(selection),
                               (Int)TYPES_METHOD_VAL);
        if (types_selection_obj(selection) != types_object(use))
            testing_t_errorf_v(env->t, "info.Selections contains %v, want %v",
                               object_string(env->a, types_selection_obj(selection)),
                               object_string(env->a, types_object(use)));
        if (str_eq(sel->sel->name, S("m")))
            env->dmm = use;
        else if (str_eq(sel->sel->name, S("n")))
            env->dmn = use;
    }
    return true;
}

static Str func_string(Alloc *a, TypesFunc *f) {
    return f != NULL ? object_string(a, types_object(f)) : S("<nil>");
}

static void TestGenericMethodInfo(TestingT *t) {
    Str src = S("package p\n"
                "\n"
                "type N[A any] int\n"
                "\n"
                "func (r N[B]) m() { r.m(); r.n() }\n"
                "\n"
                "func (r *N[C]) n() {  }\n");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, src);
    TypesInfo info = {0};
    info.defs = new_objects_map(a);
    info.uses = new_objects_map(a);
    info.selections = map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);
    TypesConfig conf = {0};
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg = types_config_check(&conf, a, S("p"), fset, files, &info, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    TypesNamed *n = types_as_named(
        types_object_type(types_scope_lookup(types_package_scope(pkg), S("N"))));

    /* Find the generic methods stored on N. */
    TypesFunc *gm = types_named_method(n, 0), *gn = types_named_method(n, 1);
    if (str_eq(types_object_name(types_object(gm)), S("n"))) {
        TypesFunc *tmp = gm;
        gm = gn;
        gn = tmp;
    }

    /* Collect objects from info. */
    TypesFunc *dm = NULL, *dn = NULL; /* the declared methods */
    GenericMethodEnv env = {t, a, &info, NULL, NULL};
    for (Int i = 0; i < f->decls.len; i++) {
        AstDecl decl = ((AstDecl *)f->decls.p)[i];
        if (decl->kind != AST_KIND_FUNC_DECL)
            continue;
        AstFuncDecl *fdecl = (AstFuncDecl *)decl;
        TypesFunc *def = must_func(lookup_object(info.defs, fdecl->name));
        if (str_eq(fdecl->name->name, S("m"))) {
            dm = def;
            ast_inspect(&fdecl->body->node,
                        BURROW_FN(AstInspectFunc, generic_method_inspect, &env));
        } else if (str_eq(fdecl->name->name, S("n"))) {
            dn = def;
        }
    }

    if (gm != dm)
        testing_t_errorf_v(t,
                           "N.Method(...) returns %v for \"m\", but Info.Defs has %v",
                           func_string(a, gm), func_string(a, dm));
    if (gn != dn)
        testing_t_errorf_v(t,
                           "N.Method(...) returns %v for \"m\", but Info.Defs has %v",
                           func_string(a, gm), func_string(a, dm));
    if (env.dmm == dm)
        testing_t_errorf_v(t, "Inside \"m\", r.m uses %v, want a func distinct from %v",
                           func_string(a, env.dmm), func_string(a, dm));
    if (env.dmn == dn)
        testing_t_errorf_v(t, "Inside \"m\", r.n uses %v, want a func distinct from %v",
                           func_string(a, env.dmn), func_string(a, dn));
    arena_free(&ar);
}

static const struct {
    Str src;
    Str want;
} implicits_tests[] = {
    {S_("package p2; import . \"fmt\"; var _ = Println"),
     S_("")}, /* no Implicits entry */
    {S_("package p0; import local \"fmt\"; var _ = local.Println"),
     S_("")}, /* no Implicits entry */
    {S_("package p1; import \"fmt\"; var _ = fmt.Println"),
     S_("importSpec: package fmt")},

    {S_("package p3; func f(x interface{}) { switch x.(type) { case int: } }"),
     S_("")}, /* no Implicits entry */
    {S_("package p4; func f(x interface{}) { switch t := x.(type) { case int: _ = t } "
        "}"),
     S_("caseClause: var t int")},
    {S_("package p5; func f(x interface{}) { switch t := x.(type) { case int, uint: _ "
        "= t } }"),
     S_("caseClause: var t interface{}")},
    {S_("package p6; func f(x interface{}) { switch t := x.(type) { default: _ = t } "
        "}"),
     S_("caseClause: var t interface{}")},

    {S_("package p7; func f(x int) {}"), S_("")}, /* no Implicits entry */
    {S_("package p8; func f(int) {}"), S_("field: var  int")},
    {S_("package p9; func f() (complex64) { return 0 }"), S_("field: var  complex64")},
    {S_("package p10; type T struct{}; func (*T) f() {}"), S_("field: var  *p10.T")},

    /* Tests using generics. */
    {S_("package f0; func f[T any](x int) {}"), S_("")}, /* no Implicits entry */
    {S_("package f1; func f[T any](int) {}"), S_("field: var  int")},
    {S_("package f2; func f[T any](T) {}"), S_("field: var  T")},
    {S_("package f3; func f[T any]() (complex64) { return 0 }"),
     S_("field: var  complex64")},
    {S_("package f4; func f[T any](t T) (T) { return t }"), S_("field: var  T")},
    {S_("package t0; type T[A any] struct{}; func (*T[_]) f() {}"),
     S_("field: var  *t0.T[_]")},
    {S_("package t1; type T[A any] struct{}; func _(x interface{}) { switch t := "
        "x.(type) { case T[int]: _ = t } }"),
     S_("caseClause: var t t1.T[int]")},
    {S_("package t2; type T[A any] struct{}; func _[P any](x interface{}) { switch t "
        ":= x.(type) { case T[P]: _ = t } }"),
     S_("caseClause: var t t2.T[P]")},
    {S_("package t3; func _[P any](x interface{}) { switch t := x.(type) { case P: _ = "
        "t } }"),
     S_("caseClause: var t P")},
};

static void TestImplicitsInfo(TestingT *t) {
    if (!have_source_importer())
        testing_t_skip_v(t, "no go1.27 sources to import fmt from");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(implicits_tests); i++) {
        TypesInfo info = {0};
        info.implicits = new_objects_map(a);
        Str name =
            types_package_name(must_typecheck(a, implicits_tests[i].src, NULL, &info));

        /* the test cases expect at most one Implicits entry */
        if (map_len(info.implicits) > 1) {
            testing_t_errorf_v(t, "package %s: %d Implicits entries found", name,
                               map_len(info.implicits));
            continue;
        }

        /* extract Implicits entry, if any */
        Str got = S("");
        const void *k;
        void *v;
        for (MapIter it = map_iter(info.implicits); map_next(&it, &k, &v);) {
            AstNode n = *(AstNode const *)k;
            switch (n->kind) {
            case AST_KIND_IMPORT_SPEC:
                got = S("importSpec");
                break;
            case AST_KIND_CASE_CLAUSE:
                got = S("caseClause");
                break;
            case AST_KIND_FIELD:
                got = S("field");
                break;
            default:
                testing_t_fatalf_v(t, "package %s: unexpected kind %d", name,
                                   (Int)n->kind);
            }
            got = fmt_sprintf_v(a, "%s: %s", got, object_string(a, *(TypesObject *)v));
        }

        /* verify entry */
        if (!str_eq(got, implicits_tests[i].want))
            testing_t_errorf_v(t, "package %s: got %q; want %q", name, got,
                               implicits_tests[i].want);
    }
    arena_free(&ar);
}

static const Str pkg_name_of_src =
    S_("\npackage p\n\nimport (\n\t. \"os\"\n\t_ "
       "\"io\"\n\t\"math\"\n\t\"path/filepath\"\n\tsnort \"sort\"\n)\n\n// avoid "
       "imported and not used errors\nvar (\n\t_ = Open // os.Open\n\t_ = "
       "math.Sin\n\t_ = filepath.Abs\n\t_ = snort.Ints\n)\n");

static const struct {
    Str path; /* path string enclosed in "'s */
    Str want;
} pkg_name_of_tests[] = {
    {S_("\"os\""), S_(".")},       {S_("\"io\""), S_("_")},
    {S_("\"math\""), S_("math")},  {S_("\"path/filepath\""), S_("filepath")},
    {S_("\"sort\""), S_("snort")},
};

static void TestPkgNameOf(TestingT *t) {
    if (!have_source_importer())
        testing_t_skip_v(t, "no go1.27 sources to import os from");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, pkg_name_of_src);
    TypesInfo info = {0};
    info.defs = new_objects_map(a);
    info.implicits = new_objects_map(a);
    TypesConfig conf = {0};
    conf.importer = default_importer(fset);
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    Error err = BURROW_NO_ERROR;
    (void)types_config_check(&conf, a, S("p"), fset, files, &info, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    /* map import paths to importDecl */
    Map *imports = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    AstGenDecl *gen = (AstGenDecl *)((AstDecl *)f->decls.p)[0];
    for (Int i = 0; i < gen->specs.len; i++) {
        AstSpec s = ((AstSpec *)gen->specs.p)[i];
        if (s->kind == AST_KIND_IMPORT_SPEC) {
            AstImportSpec *imp = (AstImportSpec *)s;
            map_set(imports, &imp->path->value, &imp);
        }
    }

    for (Int i = 0; i < NELEM(pkg_name_of_tests); i++) {
        Str path = pkg_name_of_tests[i].path, want = pkg_name_of_tests[i].want;
        AstImportSpec *imp = NULL;
        (void)map_get2(imports, &path, &imp);
        if (imp == NULL)
            testing_t_fatalf_v(t, "invalid test case: import path %s not found", path);
        TypesPkgName *got = types_info_pkg_name_of(&info, imp);
        if (got == NULL)
            testing_t_fatalf_v(t, "import %s: package name not found", path);
        Str got_name = types_object_name(types_object(got));
        if (!str_eq(got_name, want))
            testing_t_errorf_v(t, "import %s: got %s; want %s", path, got_name, want);
    }

    /* test non-existing importDecl */
    AstImportSpec *none = (AstImportSpec *)ast_node_new(a, AST_KIND_IMPORT_SPEC);
    TypesPkgName *got = types_info_pkg_name_of(&info, none);
    if (got != NULL)
        testing_t_errorf_v(t, "got %s for non-existing import declaration",
                           types_object_name(types_object(got)));
    arena_free(&ar);
}

/* pred_string: the predicates tv satisfies. Not in Go: the zero
 * ConstantValue, of kind unknown, is Go's nil Value. */
static Str pred_string(Alloc *a, TypesTypeAndValue tv) {
    bool is_const = constant_value_kind(tv.value) != CONSTANT_UNKNOWN;
    const struct {
        bool b;
        Str s;
    } preds[] = {
        {types_type_and_value_is_void(tv), S("void")},
        {types_type_and_value_is_type(tv), S("type")},
        {types_type_and_value_is_builtin(tv), S("builtin")},
        {types_type_and_value_is_value(tv) && is_const, S("const")},
        {types_type_and_value_is_value(tv) && !is_const, S("value")},
        {types_type_and_value_is_nil(tv), S("nil")},
        {types_type_and_value_addressable(tv), S("addressable")},
        {types_type_and_value_assignable(tv), S("assignable")},
        {types_type_and_value_has_ok(tv), S("hasOk")},
    };
    Slice parts = slice_nil(TYPE_STRING);
    for (Int i = 0; i < NELEM(preds); i++) {
        if (preds[i].b)
            parts = slice_append(a, parts, &preds[i].s, 1);
    }
    if (parts.len == 0)
        return S("invalid");
    return strings_join(a, parts, S(", "));
}

static const struct {
    Str src;
    Str expr;
    Str pred;
} predicates_tests[] = {
    {S_("package n0; func f() { f() }"), S_("f()"), S_("void")},

    /* types */
    {S_("package t0; type _ int"), S_("int"), S_("type")},
    {S_("package t1; type _ []int"), S_("[]int"), S_("type")},
    {S_("package t2; type _ func()"), S_("func()"), S_("type")},
    {S_("package t3; type _ func(int)"), S_("int"), S_("type")},
    {S_("package t3; type _ func(...int)"), S_("...int"), S_("type")},

    /* built-ins */
    {S_("package b0; var _ = len(\"\")"), S_("len"), S_("builtin")},
    {S_("package b1; var _ = (len)(\"\")"), S_("(len)"), S_("builtin")},

    /* constants */
    {S_("package c0; var _ = 42"), S_("42"), S_("const")},
    {S_("package c1; var _ = \"foo\" + \"bar\""), S_("\"foo\" + \"bar\""), S_("const")},
    {S_("package c2; const (i = 1i; _ = i)"), S_("i"), S_("const")},

    /* values */
    {S_("package v0; var (a, b int; _ = a + b)"), S_("a + b"), S_("value")},
    {S_("package v1; var _ = &[]int{1}"), S_("[]int{\342\200\246}"), S_("value")},
    {S_("package v2; var _ = func(){}"), S_("(func() literal)"), S_("value")},
    {S_("package v4; func f() { _ = f }"), S_("f"), S_("value")},
    {S_("package v3; var _ *int = nil"), S_("nil"), S_("value, nil")},
    {S_("package v3; var _ *int = (nil)"), S_("(nil)"), S_("value, nil")},

    /* addressable (and thus assignable) operands */
    {S_("package a0; var (x int; _ = x)"), S_("x"),
     S_("value, addressable, assignable")},
    {S_("package a1; var (p *int; _ = *p)"), S_("*p"),
     S_("value, addressable, assignable")},
    {S_("package a2; var (s []int; _ = s[0])"), S_("s[0]"),
     S_("value, addressable, assignable")},
    {S_("package a3; var (s struct{f int}; _ = s.f)"), S_("s.f"),
     S_("value, addressable, assignable")},
    {S_("package a4; var (a [10]int; _ = a[0])"), S_("a[0]"),
     S_("value, addressable, assignable")},
    {S_("package a5; func _(x int) { _ = x }"), S_("x"),
     S_("value, addressable, assignable")},
    {S_("package a6; func _()(x int) { _ = x; return }"), S_("x"),
     S_("value, addressable, assignable")},
    {S_("package a7; type T int; func (x T) _() { _ = x }"), S_("x"),
     S_("value, addressable, assignable")},
    /* composite literals are not addressable */

    /* assignable but not addressable values */
    {S_("package s0; var (m map[int]int; _ = m[0])"), S_("m[0]"),
     S_("value, assignable, hasOk")},
    {S_("package s1; var (m map[int]int; _, _ = m[0])"), S_("m[0]"),
     S_("value, assignable, hasOk")},

    /* hasOk expressions */
    {S_("package k0; var (ch chan int; _ = <-ch)"), S_("<-ch"), S_("value, hasOk")},
    {S_("package k1; var (ch chan int; _, _ = <-ch)"), S_("<-ch"), S_("value, hasOk")},

    /* missing entries */
    /* - package names are collected in the Uses map */
    /* - identifiers being declared are collected in the Defs map */
    {S_("package m0; import \"os\"; func _() { _ = os.Stdout }"), S_("os"),
     S_("<missing>")},
    {S_("package m1; import p \"os\"; func _() { _ = p.Stdout }"), S_("p"),
     S_("<missing>")},
    {S_("package m2; const c = 0"), S_("c"), S_("<missing>")},
    {S_("package m3; type T int"), S_("T"), S_("<missing>")},
    {S_("package m4; var v int"), S_("v"), S_("<missing>")},
    {S_("package m5; func f() {}"), S_("f"), S_("<missing>")},
    {S_("package m6; func _(x int) {}"), S_("x"), S_("<missing>")},
    {S_("package m6; func _()(x int) { return }"), S_("x"), S_("<missing>")},
    {S_("package m6; type T int; func (x T) _() {}"), S_("x"), S_("<missing>")},
};

static void TestPredicatesInfo(TestingT *t) {
    if (!have_source_importer())
        testing_t_skip_v(t, "no go1.27 sources to import os from");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(predicates_tests); i++) {
        TypesInfo info = {0};
        info.types = new_types_map(a);
        Str name =
            types_package_name(must_typecheck(a, predicates_tests[i].src, NULL, &info));

        /* look for expression predicates */
        Str got = S("<missing>");
        const void *k;
        void *v;
        for (MapIter it = map_iter(info.types); map_next(&it, &k, &v);) {
            if (str_eq(types_expr_string(a, *(AstExpr const *)k),
                       predicates_tests[i].expr)) {
                got = pred_string(a, *(const TypesTypeAndValue *)v);
                break;
            }
        }

        if (!str_eq(got, predicates_tests[i].pred))
            testing_t_errorf_v(t, "package %s: got %s; want %s", name, got,
                               predicates_tests[i].pred);
    }
    arena_free(&ar);
}

enum { MAX_SCOPES = 5 };

static const struct {
    Str src;
    Str scopes[MAX_SCOPES]; /* list of scope descriptors of the form kind:varlist */
} scopes_tests[] = {
    {S_("package p0"),
     {
         S_("file:"),
     }},
    {S_("package p1; import ( \"fmt\"; m \"math\"; _ \"os\" ); var ( _ = fmt.Println; "
        "_ = m.Pi )"),
     {
         S_("file:fmt m"),
     }},
    {S_("package p2; func _() {}"),
     {
         S_("file:"),
         S_("func:"),
     }},
    {S_("package p3; func _(x, y int) {}"),
     {
         S_("file:"),
         S_("func:x y"),
     }},
    {S_("package p4; func _(x, y int) { x, z := 1, 2; _ = z }"),
     {
         S_("file:"), S_("func:x y z"), /* redeclaration of x */
     }},
    {S_("package p5; func _(x, y int) (u, _ int) { return }"),
     {
         S_("file:"),
         S_("func:u x y"),
     }},
    {S_("package p6; func _() { { var x int; _ = x } }"),
     {
         S_("file:"),
         S_("func:"),
         S_("block:x"),
     }},
    {S_("package p7; func _() { if true {} }"),
     {
         S_("file:"),
         S_("func:"),
         S_("if:"),
         S_("block:"),
     }},
    {S_("package p8; func _() { if x := 0; x < 0 { y := x; _ = y } }"),
     {
         S_("file:"),
         S_("func:"),
         S_("if:x"),
         S_("block:y"),
     }},
    {S_("package p9; func _() { switch x := 0; x {} }"),
     {
         S_("file:"),
         S_("func:"),
         S_("switch:x"),
     }},
    {S_("package p10; func _() { switch x := 0; x { case 1: y := x; _ = y; default: "
        "}}"),
     {
         S_("file:"),
         S_("func:"),
         S_("switch:x"),
         S_("case:y"),
         S_("case:"),
     }},
    {S_("package p11; func _(t interface{}) { switch t.(type) {} }"),
     {
         S_("file:"),
         S_("func:t"),
         S_("type switch:"),
     }},
    {S_("package p12; func _(t interface{}) { switch t := t; t.(type) {} }"),
     {
         S_("file:"),
         S_("func:t"),
         S_("type switch:t"),
     }},
    {S_("package p13; func _(t interface{}) { switch x := t.(type) { case int: _ = x } "
        "}"),
     {
         S_("file:"), S_("func:t"), S_("type switch:"),
         S_("case:x"), /* x implicitly declared */
     }},
    {S_("package p14; func _() { select{} }"),
     {
         S_("file:"),
         S_("func:"),
     }},
    {S_("package p15; func _(c chan int) { select{ case <-c: } }"),
     {
         S_("file:"),
         S_("func:c"),
         S_("comm:"),
     }},
    {S_("package p16; func _(c chan int) { select{ case i := <-c: x := i; _ = x} }"),
     {
         S_("file:"),
         S_("func:c"),
         S_("comm:i x"),
     }},
    {S_("package p17; func _() { for{} }"),
     {
         S_("file:"),
         S_("func:"),
         S_("for:"),
         S_("block:"),
     }},
    {S_("package p18; func _(n int) { for i := 0; i < n; i++ { _ = i } }"),
     {
         S_("file:"),
         S_("func:n"),
         S_("for:i"),
         S_("block:"),
     }},
    {S_("package p19; func _(a []int) { for i := range a { _ = i} }"),
     {
         S_("file:"),
         S_("func:a"),
         S_("range:i"),
         S_("block:"),
     }},
    {S_("package p20; var s int; func _(a []int) { for i, x := range a { s += x; _ = i "
        "} }"),
     {
         S_("file:"),
         S_("func:a"),
         S_("range:i x"),
         S_("block:"),
     }},
};

static void TestScopesInfo(TestingT *t) {
    if (!have_source_importer())
        testing_t_skip_v(t, "no go1.27 sources to import fmt from");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(scopes_tests); i++) {
        const Str *scopes = scopes_tests[i].scopes;
        Int nscopes = 0;
        while (nscopes < MAX_SCOPES && scopes[nscopes].len != 0)
            nscopes++;

        TypesInfo info = {0};
        info.scopes = new_objects_map(a);
        Str name =
            types_package_name(must_typecheck(a, scopes_tests[i].src, NULL, &info));

        /* number of scopes must match */
        if (map_len(info.scopes) != nscopes)
            testing_t_errorf_v(t, "package %s: got %d scopes; want %d", name,
                               map_len(info.scopes), nscopes);

        /* scope descriptions must match */
        const void *k;
        void *v;
        for (MapIter it = map_iter(info.scopes); map_next(&it, &k, &v);) {
            Str kind = S("<unknown node kind>");
            switch ((*(AstNode const *)k)->kind) {
            case AST_KIND_FILE:
                kind = S("file");
                break;
            case AST_KIND_FUNC_TYPE:
                kind = S("func");
                break;
            case AST_KIND_BLOCK_STMT:
                kind = S("block");
                break;
            case AST_KIND_IF_STMT:
                kind = S("if");
                break;
            case AST_KIND_SWITCH_STMT:
                kind = S("switch");
                break;
            case AST_KIND_TYPE_SWITCH_STMT:
                kind = S("type switch");
                break;
            case AST_KIND_CASE_CLAUSE:
                kind = S("case");
                break;
            case AST_KIND_COMM_CLAUSE:
                kind = S("comm");
                break;
            case AST_KIND_FOR_STMT:
                kind = S("for");
                break;
            case AST_KIND_RANGE_STMT:
                kind = S("range");
                break;
            default:
                break;
            }

            /* look for matching scope description */
            TypesScope *scope = *(TypesScope **)v;
            Str desc = fmt_sprintf_v(
                a, "%s:%s", kind, strings_join(a, types_scope_names(a, scope), S(" ")));
            bool found = false;
            for (Int j = 0; j < nscopes; j++)
                found = found || str_eq(scopes[j], desc);
            if (!found)
                testing_t_errorf_v(t, "package %s: no matching scope found for %s",
                                   name, desc);
        }
    }
    arena_free(&ar);
}

static const Str selection_lib_src = S_("\npackage lib\ntype T float64\nconst C T = "
                                        "3\nvar V T\nfunc F() {}\nfunc (T) M() {}\n");
static const Str selection_main_src =
    S_("\npackage main\nimport \"lib\"\n\ntype A struct {\n\t*B\n\tC\n}\n\ntype B "
       "struct {\n\tb int\n}\n\nfunc (B) f(int)\n\ntype C struct {\n\tc int\n}\n\ntype "
       "G[P any] struct {\n\tp P\n}\n\nfunc (G[P]) m(P) {}\n\nvar Inst G[int]\n\nfunc "
       "(C) g()\nfunc (*C) h()\n\nfunc main() {\n\t// qualified identifiers\n\tvar _ "
       "lib.T\n\t_ = lib.C\n\t_ = lib.F\n\t_ = lib.V\n\t_ = lib.T.M\n\n\t// "
       "fields\n\t_ = A{}.B\n\t_ = new(A).B\n\n\t_ = A{}.C\n\t_ = new(A).C\n\n\t_ = "
       "A{}.b\n\t_ = new(A).b\n\n\t_ = A{}.c\n\t_ = new(A).c\n\n\t_ = Inst.p\n\t_ = "
       "G[string]{}.p\n\n\t// methods\n\t_ = A{}.f\n\t_ = new(A).f\n\t_ = A{}.g\n\t_ = "
       "new(A).g\n\t_ = new(A).h\n\n\t_ = B{}.f\n\t_ = new(B).f\n\n\t_ = C{}.g\n\t_ = "
       "new(C).g\n\t_ = new(C).h\n\t_ = Inst.m\n\n\t// method expressions\n\t_ = "
       "A.f\n\t_ = (*A).f\n\t_ = B.f\n\t_ = (*B).f\n\t_ = G[string].m\n}");

static const struct {
    Str syntax;
    Str want[2];
} selection_want_out[] = {
    {S_("lib.T.M"), {S_("method expr (lib.T) M(lib.T)"), S_(".[0]")}},

    {S_("A{}.B"), {S_("field (main.A) B *main.B"), S_(".[0]")}},
    {S_("new(A).B"), {S_("field (*main.A) B *main.B"), S_("->[0]")}},
    {S_("A{}.C"), {S_("field (main.A) C main.C"), S_(".[1]")}},
    {S_("new(A).C"), {S_("field (*main.A) C main.C"), S_("->[1]")}},
    {S_("A{}.b"), {S_("field (main.A) b int"), S_("->[0 0]")}},
    {S_("new(A).b"), {S_("field (*main.A) b int"), S_("->[0 0]")}},
    {S_("A{}.c"), {S_("field (main.A) c int"), S_(".[1 0]")}},
    {S_("new(A).c"), {S_("field (*main.A) c int"), S_("->[1 0]")}},
    {S_("Inst.p"), {S_("field (main.G[int]) p int"), S_(".[0]")}},

    {S_("A{}.f"), {S_("method (main.A) f(int)"), S_("->[0 0]")}},
    {S_("new(A).f"), {S_("method (*main.A) f(int)"), S_("->[0 0]")}},
    {S_("A{}.g"), {S_("method (main.A) g()"), S_(".[1 0]")}},
    {S_("new(A).g"), {S_("method (*main.A) g()"), S_("->[1 0]")}},
    {S_("new(A).h"),
     {S_("method (*main.A) h()"),
      S_("->[1 1]")}}, /* TODO(gri) should this report .[1 1] ? */
    {S_("B{}.f"), {S_("method (main.B) f(int)"), S_(".[0]")}},
    {S_("new(B).f"), {S_("method (*main.B) f(int)"), S_("->[0]")}},
    {S_("C{}.g"), {S_("method (main.C) g()"), S_(".[0]")}},
    {S_("new(C).g"), {S_("method (*main.C) g()"), S_("->[0]")}},
    {S_("new(C).h"),
     {S_("method (*main.C) h()"),
      S_("->[1]")}}, /* TODO(gri) should this report .[1] ? */
    {S_("Inst.m"), {S_("method (main.G[int]) m(int)"), S_(".[0]")}},

    {S_("A.f"), {S_("method expr (main.A) f(main.A, int)"), S_("->[0 0]")}},
    {S_("(*A).f"), {S_("method expr (*main.A) f(*main.A, int)"), S_("->[0 0]")}},
    {S_("B.f"), {S_("method expr (main.B) f(main.B, int)"), S_(".[0]")}},
    {S_("(*B).f"), {S_("method expr (*main.B) f(*main.B, int)"), S_("->[0]")}},
    {S_("G[string].m"),
     {S_("method expr (main.G[string]) m(main.G[string], string)"), S_(".[0]")}},
    {S_("G[string]{}.p"), {S_("field (main.G[string]) p string"), S_(".[0]")}},
};

/* "[1 0]", the way %v prints an []int */
static Str index_string(Alloc *a, Slice index) {
    Slice parts = slice_nil(TYPE_STRING);
    for (Int i = 0; i < index.len; i++) {
        Str s = strconv_itoa(a, BURROW_AT(Int, index, i));
        parts = slice_append(a, parts, &s, 1);
    }
    return fmt_sprintf_v(a, "[%s]", strings_join(a, parts, S(" ")));
}

/* makePkg for the tests that check one package after another, with imports
 * importing what came before. */
static TypesPackage *make_pkg(Alloc *a, TypesConfig *conf, TokenFileSet *fset,
                              TypesInfo *info, Map *imports, Str path, Str src,
                              Error *err) {
    AstFile *f = must_parse(a, fset, src);
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    TypesPackage *pkg = types_config_check(conf, a, path, fset, files, info, err);
    map_set(imports, &path, &pkg);
    return pkg;
}

static void TestSelection(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Map *selections = map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0);

    /* We need a specific fileset in this test below for positions.
     * Cannot use typecheck helper. */
    TokenFileSet *fset = token_new_file_set(a);
    Map *imports = new_test_importer(a);
    TypesConfig conf = {0};
    conf.importer = test_importer(imports);
    Str paths[2] = {S("lib"), S("main")};
    Str srcs[2] = {selection_lib_src, selection_main_src};
    for (Int i = 0; i < 2; i++) {
        TypesInfo info = {0};
        info.selections = selections;
        Error err = BURROW_NO_ERROR;
        (void)make_pkg(a, &conf, fset, &info, imports, paths[i], srcs[i], &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
    }

    bool used[NELEM(selection_want_out)] = {0};
    const void *k;
    void *v;
    for (MapIter it = map_iter(selections); map_next(&it, &k, &v);) {
        AstNode e = *(AstNode const *)k;
        TypesSelection *sel = *(TypesSelection **)v;
        (void)types_selection_string(a, sel, TYPES_NO_QUALIFIER); /* must not panic */

        Int start = token_file_set_position(fset, ast_node_pos(e)).offset;
        Int end = token_file_set_position(fset, ast_node_end(e)).offset;
        /* (all SelectorExprs are in main, not lib) */
        Str syntax = str_from_bytes(selection_main_src.p + start, end - start);

        Str direct = types_selection_indirect(sel) ? S("->") : S(".");
        Str got[2] = {
            types_selection_string(a, sel, TYPES_NO_QUALIFIER),
            fmt_sprintf_v(a, "%s%s", direct,
                          index_string(a, types_selection_index(sel))),
        };
        Str want[2] = {S(""), S("")};
        for (Int i = 0; i < NELEM(selection_want_out); i++) {
            if (!used[i] && str_eq(selection_want_out[i].syntax, syntax)) {
                want[0] = selection_want_out[i].want[0];
                want[1] = selection_want_out[i].want[1];
                used[i] = true; /* delete(wantOut, syntax) */
                break;
            }
        }
        if (!str_eq(want[0], got[0]) || !str_eq(want[1], got[1]))
            testing_t_errorf_v(t, "%s: got [%q %q]; want [%q %q]", syntax, got[0],
                               got[1], want[0], want[1]);

        /* We must explicitly assert properties of the
         * Signature's receiver since it doesn't participate
         * in Identical() or String(). */
        TypesSignature *sig = types_as_signature(types_selection_type(a, sel));
        if (types_selection_kind(sel) == TYPES_METHOD_VAL) {
            TypesType got_recv =
                types_object_type(types_object(types_signature_recv(sig)));
            TypesType want_recv = types_selection_recv(sel);
            if (!types_identical(got_recv, want_recv))
                testing_t_errorf_v(t, "%s: Recv() = %s, want %s", syntax,
                                   type_string(a, got_recv), type_string(a, want_recv));
        } else if (sig != NULL && types_signature_recv(sig) != NULL) {
            testing_t_errorf_v(
                t, "%s: signature has receiver %s", type_string(a, types_type(sig)),
                type_string(
                    a, types_object_type(types_object(types_signature_recv(sig)))));
        }
    }
    /* Assert that all wantOut entries were used exactly once. */
    for (Int i = 0; i < NELEM(selection_want_out); i++) {
        if (!used[i])
            testing_t_errorf_v(t, "no ast.Selection found with syntax %q",
                               selection_want_out[i].syntax);
    }
    arena_free(&ar);
}

static void log_error(void *env, const TypesError *e) {
    TestingT *t = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    testing_t_log_v(t, types_error_error(e, arena_allocator(&ar)));
    arena_free(&ar);
}

/* check_in_order: makePkg for each of paths and srcs in turn, with errors
 * logged through log_error. */
static void check_in_order(TestingT *t, const Str *paths, const Str *srcs, Int n) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    Map *imports = new_test_importer(a);
    TypesConfig conf = {0};
    conf.error = (TypesErrorFunc){log_error, t}; /* don't exit after first error */
    conf.importer = test_importer(imports);
    for (Int i = 0; i < n; i++) {
        Error err = BURROW_NO_ERROR; /* errors logged via conf.Error */
        (void)make_pkg(a, &conf, fset, NULL, imports, paths[i], srcs[i], &err);
    }
    arena_free(&ar);
}

static void TestIssue8518(TestingT *t) {
    const Str paths[] = {S("a"), S("main")};
    const Str srcs[] = {
        S_("\npackage a\nimport \"missing\"\nconst C1 = foo\nconst C2 = missing.C\n"),
        S_("\npackage main\nimport \"a\"\nvar _ = a.C1\nvar _ = a.C2\n"),
    };
    check_in_order(t, paths, srcs, 2); /* don't crash when type-checking main */
}

static void TestIssue59603(TestingT *t) {
    const Str paths[] = {S("a"), S("main")};
    const Str srcs[] = {
        S_("\npackage a\nconst C = foo\n"),
        S_("\npackage main\nimport \"a\"\nconst _ = a.C\n"),
    };
    check_in_order(t, paths, srcs, 2); /* don't crash when type-checking main */
}

/* Test for go.dev/issue/52715 */
static void TestLookupFieldOrMethod_RecursiveGeneric(TestingT *t) {
    (void)t;
    Str src = S("\npackage pkg\n\ntype Tree[T any] struct {\n\t*Node[T]\n}\n\nfunc "
                "(*Tree[R]) N(r R) R { return r }\n\ntype Node[T any] struct "
                "{\n\t*Tree[T]\n}\n\ntype Instance = *Tree[int]\n");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, src);
    TypesPackage *pkg = types_new_package(a, S("pkg"), f->name->name);
    TypesChecker *check = types_new_checker(a, NULL, fset, pkg, NULL);
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    Error err = BURROW_NO_ERROR;
    if (!types_checker_files(check, files, &err))
        panic_str(error_text(err));

    TypesType T =
        types_object_type(types_scope_lookup(types_package_scope(pkg), S("Instance")));
    /* verify that LookupFieldOrMethod terminates */
    (void)types_lookup_field_or_method(a, T, false, pkg, S("M"), NULL, NULL);
    arena_free(&ar);
}

static const struct {
    Str src;
    bool want;
} identical_tests[] = {
    {S_("var X int; var Y int"), true},
    {S_("var X int; var Y string"), false},

    /* TODO: add more tests for complex types. */

    /* Named types. */
    {S_("type X int; type Y int"), false},

    /* Aliases. */
    {S_("type X = int; type Y = int"), true},

    /* Functions. */
    {S_("func X(int) string { return \"\" }; func Y(int) string { return \"\" }"),
     true},
    {S_("func X() string { return \"\" }; func Y(int) string { return \"\" }"), false},
    {S_("func X(int) string { return \"\" }; func Y(int) {}"), false},

    /* Generic functions. Type parameters should be considered identical modulo */
    /* renaming. See also go.dev/issue/49722. */
    {S_("func X[P ~int](){}; func Y[Q ~int]() {}"), true},
    {S_("func X[P1 any, P2 ~*P1](){}; func Y[Q1 any, Q2 ~*Q1]() {}"), true},
    {S_("func X[P1 any, P2 ~[]P1](){}; func Y[Q1 any, Q2 ~*Q1]() {}"), false},
    {S_("func X[P ~int](P){}; func Y[Q ~int](Q) {}"), true},
    {S_("func X[P ~string](P){}; func Y[Q ~int](Q) {}"), false},
    {S_("func X[P ~int]([]P){}; func Y[Q ~int]([]Q) {}"), true},
};

static void TestIdentical(TestingT *t) {
    /* For each test, we compare the types of objects X and Y in the source. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(identical_tests); i++) {
        TypesPackage *pkg = must_typecheck(
            a, fmt_sprintf_v(a, "package p;%s", identical_tests[i].src), NULL, NULL);
        TypesObject X = types_scope_lookup(types_package_scope(pkg), S("X"));
        TypesObject Y = types_scope_lookup(types_package_scope(pkg), S("Y"));
        if (X == NULL || Y == NULL)
            testing_t_fatal_v(t, "test must declare both X and Y");
        bool got = types_identical(types_object_type(X), types_object_type(Y));
        if (got != identical_tests[i].want)
            testing_t_errorf_v(t, "Identical(%s, %s) = %t, want %t",
                               type_string(a, types_object_type(X)),
                               type_string(a, types_object_type(Y)), got,
                               identical_tests[i].want);
    }
    arena_free(&ar);
}

/* Type.String for %v, with <nil> for NULL. */
static Str type_string_or_nil(Alloc *a, TypesType t) {
    return t != NULL ? type_string(a, t) : S("<nil>");
}

static void TestIdentical_issue15173(TestingT *t) {
    /* Identical should allow nil arguments and be symmetric. */
    TypesType int_ = types_type(types_typ[TYPES_INT]);
    const struct {
        TypesType x, y;
        bool want;
    } tests[] = {
        {int_, int_, true},
        {int_, NULL, false},
        {NULL, int_, false},
        {NULL, NULL, true},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        bool got = types_identical(tests[i].x, tests[i].y);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "Identical(%v, %v) = %t",
                               type_string_or_nil(a, tests[i].x),
                               type_string_or_nil(a, tests[i].y), got);
    }
    arena_free(&ar);
}

static TypesTuple *one_param(Alloc *a, TypesBasicKind kind) {
    TypesVar *v =
        types_new_param(a, TOKEN_NO_POS, NULL, S(""), types_type(types_typ[kind]));
    return types_new_tuple(a, slice_append(a, slice_nil(TYPE_UNSAFE_POINTER), &v, 1));
}

static void TestIssue61737(TestingT *t) {
    /* This test verifies that it is possible to construct invalid interfaces
     * containing duplicate methods using the go/types API.
     *
     * It must be possible for importers to construct such invalid interfaces.
     * Previously, this panicked. */
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice none = slice_nil(TYPE_UNSAFE_POINTER);
    TypesSignature *sig1 = types_new_signature_type(
        a, NULL, none, none, one_param(a, TYPES_INT), NULL, false);
    TypesSignature *sig2 = types_new_signature_type(
        a, NULL, none, none, one_param(a, TYPES_STRING), NULL, false);

    TypesFunc *methods[] = {
        types_new_func(a, TOKEN_NO_POS, NULL, S("M"), sig1),
        types_new_func(a, TOKEN_NO_POS, NULL, S("M"), sig2),
    };
    TypesFunc *embedded_methods[] = {
        types_new_func(a, TOKEN_NO_POS, NULL, S("M"), sig2),
    };
    TypesInterface *embedded =
        types_new_interface_type(a, slice_append(a, none, embedded_methods, 1), none);
    TypesType embeddeds[] = {types_type(embedded)};
    TypesInterface *iface = types_new_interface_type(
        a, slice_append(a, none, methods, 2), slice_append(a, none, embeddeds, 1));
    (void)types_interface_complete(iface);
    arena_free(&ar);
}

static void TestNewAlias_Issue65455(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TypesTypeName *obj = types_new_type_name(a, TOKEN_NO_POS, NULL, S("A"), NULL);
    TypesAlias *alias = types_new_alias(a, obj, types_type(types_typ[TYPES_INT]));
    (void)types_type_underlying(types_type(alias)); /* must not panic */
    arena_free(&ar);
}

static void TestIssue15305(TestingT *t) {
    Str src = S("package p; func f() int16; var _ = f(undef)");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, src);
    TypesConfig conf = {0};
    conf.error = (TypesErrorFunc){collect_all, NULL}; /* allow errors */
    TypesInfo info = {0};
    info.types = new_types_map(a);
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    Error err = BURROW_NO_ERROR;
    (void)types_config_check(&conf, a, S("p"), fset, files, &info,
                             &err); /* ignore result */
    const void *k;
    void *v;
    for (MapIter it = map_iter(info.types); map_next(&it, &k, &v);) {
        if ((*(AstExpr const *)k)->kind == AST_KIND_CALL_EXPR) {
            TypesType typ = ((const TypesTypeAndValue *)v)->type;
            if (typ != types_type(types_typ[TYPES_INT16]))
                testing_t_errorf_v(t, "CallExpr has type %v, want int16",
                                   type_string_or_nil(a, typ));
            arena_free(&ar);
            return;
        }
    }
    testing_t_errorf_v(t, "CallExpr has no type");
    arena_free(&ar);
}

static const struct {
    Str lit, typ;
} composite_lit_tests[] = {
    {S_("[16]byte{}"), S_("[16]byte")},
    {S_("[...]byte{}"), S_("[0]byte")},                /* test for go.dev/issue/14092 */
    {S_("[...]int{1, 2, 3}"), S_("[3]int")},           /* test for go.dev/issue/14092 */
    {S_("[...]int{90: 0, 98: 1, 2}"), S_("[100]int")}, /* test for go.dev/issue/14092 */
    {S_("[]int{}"), S_("[]int")},
    {S_("map[string]bool{\"foo\": true}"), S_("map[string]bool")},
    {S_("struct{}{}"), S_("struct{}")},
    {S_("struct{x, y int; z complex128}{}"), S_("struct{x int; y int; z complex128}")},
};

static void cmptype(TestingT *t, Alloc *a, Map *types, Str lit, AstExpr x, Str want) {
    TypesTypeAndValue tv;
    if (!map_get2(types, &x, &tv)) {
        testing_t_errorf_v(t, "%s: no Types entry found", lit);
        return;
    }
    if (tv.type == NULL) {
        testing_t_errorf_v(t, "%s: type is nil", lit);
        return;
    }
    Str got = type_string(a, tv.type);
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "%s: got %v, want %s", lit, got, want);
}

/* TestCompositeLitTypes verifies that Info.Types registers the correct
 * types for composite literal expressions and composite literal type
 * expressions. */
static void TestCompositeLitTypes(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(composite_lit_tests); i++) {
        Str lit = composite_lit_tests[i].lit;
        TokenFileSet *fset = token_new_file_set(a);
        AstFile *f =
            must_parse(a, fset, fmt_sprintf_v(a, "package p%d; var _ = %s", i, lit));
        Map *types = new_types_map(a);
        TypesInfo info = {0};
        info.types = types;
        TypesConfig conf = {0};
        Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
        Error err = BURROW_NO_ERROR;
        (void)types_config_check(&conf, a, S("p"), fset, files, &info, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s: %v", lit, error_text(err));

        /* test type of composite literal expression */
        AstGenDecl *gen = (AstGenDecl *)BURROW_AT(AstDecl, f->decls, 0);
        AstValueSpec *spec = (AstValueSpec *)BURROW_AT(AstSpec, gen->specs, 0);
        AstExpr rhs = BURROW_AT(AstExpr, spec->values, 0);
        cmptype(t, a, types, lit, rhs, composite_lit_tests[i].typ);

        /* test type of composite literal type expression */
        cmptype(t, a, types, lit, ((AstCompositeLit *)rhs)->type,
                composite_lit_tests[i].typ);
    }
    arena_free(&ar);
}

/* TestObjectParents verifies that objects have parent scopes or not
 * as specified by the Object interface. */
static void TestObjectParents(TestingT *t) {
    Str src = S("\npackage p\n\nconst C = 0\n\ntype T1 struct {\n\ta, b "
                "int\n\tT2\n}\n\ntype T2 interface {\n\tim1()\n\tim2()\n}\n\nfunc (T1) "
                "m1() {}\nfunc (*T1) m2() {}\n\nfunc f(x int) { y := x; print(y) }\n");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, src);
    TypesInfo info = {0};
    info.defs = new_objects_map(a);
    TypesConfig conf = {0};
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    Error err = BURROW_NO_ERROR;
    (void)types_config_check(&conf, a, S("p"), fset, files, &info, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    const void *k;
    void *v;
    for (MapIter it = map_iter(info.defs); map_next(&it, &k, &v);) {
        const AstIdent *ident = *(AstIdent *const *)k;
        TypesObject obj = *(TypesObject *)v;
        if (obj == NULL) {
            /* only package names and implicit vars have a nil object
             * (in this test we only need to handle the package name) */
            if (!str_eq(ident->name, S("p")))
                testing_t_errorf_v(t, "%v has nil object", ident->name);
            continue;
        }

        /* struct fields, type-associated and interface methods
         * have no parent scope */
        bool want_parent = true;
        if (types_as_var(obj) != NULL) {
            if (types_var_is_field(types_as_var(obj)))
                want_parent = false;
        } else if (types_as_func(obj) != NULL) {
            if (types_signature_recv(types_func_signature(types_as_func(obj))) != NULL)
                want_parent = false; /* method */
        }

        bool got_parent = types_object_parent(obj) != NULL;
        if (got_parent && !want_parent)
            testing_t_errorf_v(t, "%v: want no parent, got %s", ident->name,
                               types_scope_string(a, types_object_parent(obj)));
        else if (!got_parent && want_parent)
            testing_t_errorf_v(t, "%v: no parent found", ident->name);
    }
    arena_free(&ar);
}

typedef struct {
    TestingT *t;
    Str compiler;
    Int errcount;
} FailedImportEnv;

static void failed_import_error(void *penv, const TypesError *e) {
    FailedImportEnv *env = penv;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Str msg = types_error_error(e, arena_allocator(&ar));
    /* we should only see the import error */
    if (env->errcount > 0 || !strings_contains(msg, S("could not import")))
        testing_t_errorf_v(env->t, "for %s importer, got unexpected error: %v",
                           env->compiler, msg);
    env->errcount++;
    arena_free(&ar);
}

/* TestFailedImport tests that we don't get follow-on errors
 * elsewhere in a package due to failing to import a package.
 *
 * Not in Go: importer_for has no gc or gccgo importer yet and gives the zero
 * TypesImporter for them, so for those two the import fails because no
 * importer is installed rather than because the package is missing. */
static void TestFailedImport(TestingT *t) {
    if (!have_source_importer())
        testing_t_skip_v(t, "no go1.27 sources for the source importer");

    Str src = S("\npackage p\n\nimport foo "
                "\"go/types/thisdirectorymustnotexistotherwisethistestmayfail/foo\" // "
                "should only see an error here\n\nconst c = foo.C\ntype T = foo.T\nvar "
                "v T = c\nfunc f(x T) T { return foo.F(x) }\n");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, src);
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);

    /* type-check using all possible importers */
    const Str compilers[] = {S("gc"), S("gccgo"), S("source")};
    for (Int i = 0; i < NELEM(compilers); i++) {
        Str compiler = compilers[i];
        FailedImportEnv env = {t, compiler, 0};
        TypesConfig conf = {0};
        conf.error = (TypesErrorFunc){failed_import_error, &env};
        conf.importer = importer_for(a, compiler, (ImporterLookup){0});

        TypesInfo info = {0};
        info.uses = new_objects_map(a);
        Error err = BURROW_NO_ERROR;
        TypesPackage *pkg =
            types_config_check(&conf, a, S("p"), fset, files, &info, &err);
        if (pkg == NULL) {
            testing_t_errorf_v(
                t, "for %s importer, type-checking failed to return a package",
                compiler);
            continue;
        }

        Slice imports = types_package_imports(pkg);
        if (imports.len != 1) {
            testing_t_errorf_v(t, "for %s importer, got %d imports, want 1", compiler,
                               imports.len);
            continue;
        }
        TypesPackage *imp = BURROW_AT(TypesPackage *, imports, 0);
        if (!str_eq(types_package_name(imp), S("foo"))) {
            testing_t_errorf_v(t, "for %s importer, got %q, want \"foo\"", compiler,
                               types_package_name(imp));
            continue;
        }

        /* verify that all uses of foo refer to the imported package foo (imp) */
        const void *k;
        void *v;
        for (MapIter it = map_iter(info.uses); map_next(&it, &k, &v);) {
            const AstIdent *ident = *(AstIdent *const *)k;
            if (!str_eq(ident->name, S("foo")))
                continue;
            TypesObject obj = *(TypesObject *)v;
            TypesPkgName *pn = obj != NULL ? types_as_pkg_name(obj) : NULL;
            if (pn != NULL) {
                if (types_pkg_name_imported(pn) != imp)
                    testing_t_errorf_v(
                        t, "%s resolved to %v; want %v", ident->name,
                        types_package_string(a, types_pkg_name_imported(pn)),
                        types_package_string(a, imp));
            } else {
                testing_t_errorf_v(t, "%s resolved to %v; want package name",
                                   ident->name,
                                   obj != NULL ? object_string(a, obj) : S("<nil>"));
            }
        }
    }
    arena_free(&ar);
}

/* fmt.Sprint of a list of strings, a Slice of Str. */
static Str sprint_list(Alloc *a, Slice list) {
    Str parts[3] = {S("["), strings_join(a, list, S(" ")), S("]")};
    return strings_join(a, slice_append(a, slice_nil(TYPE_STRING), parts, 3), S(""));
}

/* fmt.Sprint(info.InitOrder). */
static Str init_order_string(Alloc *a, Slice init_order) {
    Slice list = slice_nil(TYPE_STRING);
    for (Int i = 0; i < init_order.len; i++) {
        const TypesInitializer *init = ((TypesInitializer **)init_order.p)[i];
        Str s = types_initializer_string(a, init);
        list = slice_append(a, list, &s, 1);
    }
    return sprint_list(a, list);
}

enum { MAX_INITS = 7 };

static const struct {
    Str src;
    Str inits[MAX_INITS];
} init_order_tests[] = {
    {S_("package p0; var (x = 1; y = x)"), {S_("x = 1"), S_("y = x")}},
    {S_("package p1; var (a = 1; b = 2; c = 3)"),
     {S_("a = 1"), S_("b = 2"), S_("c = 3")}},
    {S_("package p2; var (a, b, c = 1, 2, 3)"),
     {S_("a = 1"), S_("b = 2"), S_("c = 3")}},
    {S_("package p3; var _ = f(); func f() int { return 1 }"),
     {S_("_ = f()") /* blank var */}},
    {S_("package p4; var (a = 0; x = y; y = z; z = 0)"),
     {S_("a = 0"), S_("z = 0"), S_("y = z"), S_("x = y")}},
    {S_("package p5; var (a, _ = m[0]; m map[int]string)"),
     {S_("a, _ = m[0]") /* blank var */}},
    {S_("package p6; var a, b = f(); func f() (_, _ int) { return z, z }; var z = 0"),
     {S_("z = 0"), S_("a, b = f()")}},
    {S_("package p7; var (a = func() int { return b }(); b = 1)"),
     {S_("b = 1"), S_("a = (func() int literal)()")}},
    {S_("package p8; var (a, b = func() (_, _ int) { return c, c }(); c = 1)"),
     {S_("c = 1"), S_("a, b = (func() (_, _ int) literal)()")}},
    {S_("package p9; type T struct{}; func (T) m() int { _ = y; return 0 }; var x, y = "
        "T.m, 1"),
     {S_("y = 1"), S_("x = T.m")}},
    {S_("package p10; var (d = c + b; a = 0; b = 0; c = 0)"),
     {S_("a = 0"), S_("b = 0"), S_("c = 0"), S_("d = c + b")}},
    {S_("package p11; var (a = e + c; b = d + c; c = 0; d = 0; e = 0)"),
     {S_("c = 0"), S_("d = 0"), S_("b = d + c"), S_("e = 0"), S_("a = e + c")}},
    /* emit an initializer for n:1 initializations only once (not for each node
     * on the lhs which may appear in different order in the dependency graph) */
    {S_("package p12; var (a = x; b = 0; x, y = m[0]; m map[int]int)"),
     {S_("b = 0"), S_("x, y = m[0]"), S_("a = x")}},
    /* test case from spec section on package initialization */
    {S_("package p12\n\n\t\tvar (\n\t\t\ta = c + b\n\t\t\tb = f()\n\t\t\tc = "
        "f()\n\t\t\td = 3\n\t\t)\n\n\t\tfunc f() int {\n\t\t\td++\n\t\t\treturn "
        "d\n\t\t}"),
     {S_("d = 3"), S_("b = f()"), S_("c = f()"), S_("a = c + b")}},
    /* test case for go.dev/issue/7131 */
    {S_("package main\n\n\t\tvar counter int\n\t\tfunc next() int { counter++; return "
        "counter }\n\n\t\tvar _ = makeOrder()\n\t\tfunc makeOrder() []int { return "
        "[]int{f, b, d, e, c, a} }\n\n\t\tvar a       = next()\n\t\tvar b, c    = "
        "next(), next()\n\t\tvar d, e, f = next(), next(), next()\n\t\t"),
     {S_("a = next()"), S_("b = next()"), S_("c = next()"), S_("d = next()"),
      S_("e = next()"), S_("f = next()"), S_("_ = makeOrder()")}},
    /* test case for go.dev/issue/10709 */
    {S_("package p13\n\n\t\tvar (\n\t\t    v = t.m()\n\t\t    t = "
        "makeT(0)\n\t\t)\n\n\t\ttype T struct{}\n\n\t\tfunc (T) m() int { return 0 "
        "}\n\n\t\tfunc makeT(n int) T {\n\t\t    if n > 0 {\n\t\t        return "
        "makeT(n-1)\n\t\t    }\n\t\t    return T{}\n\t\t}"),
     {S_("t = makeT(0)"), S_("v = t.m()")}},
    /* test case for go.dev/issue/10709: same as test before, but variable decls
     * swapped */
    {S_("package p14\n\n\t\tvar (\n\t\t    t = makeT(0)\n\t\t    v = "
        "t.m()\n\t\t)\n\n\t\ttype T struct{}\n\n\t\tfunc (T) m() int { return 0 "
        "}\n\n\t\tfunc makeT(n int) T {\n\t\t    if n > 0 {\n\t\t        return "
        "makeT(n-1)\n\t\t    }\n\t\t    return T{}\n\t\t}"),
     {S_("t = makeT(0)"), S_("v = t.m()")}},
    /* another candidate possibly causing problems with go.dev/issue/10709 */
    {S_("package p15\n\n\t\tvar y1 = f1()\n\n\t\tfunc f1() int { return g1() "
        "}\n\t\tfunc g1() int { f1(); return x1 }\n\n\t\tvar x1 = 0\n\n\t\tvar y2 = "
        "f2()\n\n\t\tfunc f2() int { return g2() }\n\t\tfunc g2() int { return x2 "
        "}\n\n\t\tvar x2 = 0"),
     {S_("x1 = 0"), S_("y1 = f1()"), S_("x2 = 0"), S_("y2 = f2()")}},
};

static void TestInitOrderInfo(TestingT *t) {
    for (Int k = 0; k < NELEM(init_order_tests); k++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Int ninits = 0;
        while (ninits < MAX_INITS && init_order_tests[k].inits[ninits].len != 0)
            ninits++;

        TypesInfo info = {0};
        Str name =
            types_package_name(must_typecheck(a, init_order_tests[k].src, NULL, &info));

        /* number of initializers must match */
        if (info.init_order.len != ninits) {
            testing_t_errorf_v(t, "package %s: got %d initializers; want %d", name,
                               info.init_order.len, ninits);
            arena_free(&ar);
            continue;
        }

        /* initializers must match */
        for (Int i = 0; i < ninits; i++) {
            Str want = init_order_tests[k].inits[i];
            Str got = types_initializer_string(
                a, ((TypesInitializer **)info.init_order.p)[i]);
            if (!str_eq(got, want)) {
                testing_t_errorf_v(t, "package %s, init %d: got %s; want %s", name, i,
                                   got, want);
                continue;
            }
        }
        arena_free(&ar);
    }
}

static void TestMultiFileInitOrder(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *file_a = must_parse(a, fset, S("package main; var a = 1"));
    AstFile *file_b = must_parse(a, fset, S("package main; var b = 2"));

    /* The initialization order must not depend on the parse order of the
     * files, only on the presentation order to the type-checker. */
    struct {
        AstFile *files[2];
        Str want;
    } tests[] = {
        {{file_a, file_b}, S("[a = 1 b = 2]")},
        {{file_b, file_a}, S("[b = 2 a = 1]")},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        TypesInfo info = {0};
        TypesConfig conf = {0};
        Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), tests[i].files, 2);
        Error err = BURROW_NO_ERROR;
        (void)types_config_check(&conf, a, S("main"), fset, files, &info, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "%s", error_text(err));
        Str got = init_order_string(a, info.init_order);
        if (!str_eq(got, tests[i].want))
            testing_t_fatalf_v(t, "got %s; want %s", got, tests[i].want);
    }
    arena_free(&ar);
}

static void TestFiles(TestingT *t) {
    static const Str sources[] = {
        S_("package p; type T struct{}; func (T) m1() {}"),
        S_("package p; func (T) m2() {}; var x interface{ m1(); m2() } = T{}"),
        S_("package p; func (T) m3() {}; var y interface{ m1(); m2(); m3() } = T{}"),
        S_("package p"),
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TypesConfig conf = {0};
    TokenFileSet *fset = token_new_file_set(a);
    TypesPackage *pkg = types_new_package(a, S("p"), S("p"));
    TypesInfo info = {0};
    TypesChecker *check = types_new_checker(a, &conf, fset, pkg, &info);

    for (Int i = 0; i < NELEM(sources); i++) {
        AstFile *f = must_parse(a, fset, sources[i]);
        Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
        Error err = BURROW_NO_ERROR;
        if (!types_checker_files(check, files, &err))
            testing_t_errorf_v(t, "%s", error_text(err));
    }

    /* check InitOrder is [x y] */
    Slice vars = slice_nil(TYPE_STRING);
    for (Int i = 0; i < info.init_order.len; i++) {
        const TypesInitializer *init = ((TypesInitializer **)info.init_order.p)[i];
        for (Int j = 0; j < init->lhs.len; j++) {
            TypesVar *v = ((TypesVar **)init->lhs.p)[j];
            Str name = types_object_name(types_object(v));
            vars = slice_append(a, vars, &name, 1);
        }
    }
    Str got = sprint_list(a, vars);
    Str want = S("[x y]");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "InitOrder == %s, want %s", got, want);
    arena_free(&ar);
}

/* Not in Go: with no error handler, the first error ends the check, and
 * burrow ends it with a panic. The duplicate method is reported while the
 * interface's type set is being computed, which keeps scratch memory of its
 * own, and the leak checker in the sanitizer builds sees whether the panic
 * frees it. */
static void TestFirstErrorInTypeSet(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TypesConfig conf = {0};
    Error err = BURROW_NO_ERROR;
    (void)typecheck(a, S("package p; type I interface{ m(); m() }"), &conf, NULL, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "no error");
    else if (!strings_contains(error_text(err), S("duplicate method m")))
        testing_t_errorf_v(t, "got %s; want duplicate method m", error_text(err));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestValuesInfo)                                                                  \
    X(TestTypesInfo)                                                                   \
    X(TestInstanceInfo)                                                                \
    X(TestDefsInfo)                                                                    \
    X(TestUsesInfo)                                                                    \
    X(TestGenericMethodInfo)                                                           \
    X(TestImplicitsInfo)                                                               \
    X(TestPkgNameOf)                                                                   \
    X(TestPredicatesInfo)                                                              \
    X(TestScopesInfo)                                                                  \
    X(TestInitOrderInfo)                                                               \
    X(TestMultiFileInitOrder)                                                          \
    X(TestFiles)                                                                       \
    X(TestSelection)                                                                   \
    X(TestIssue8518)                                                                   \
    X(TestIssue59603)                                                                  \
    X(TestLookupFieldOrMethod_RecursiveGeneric)                                        \
    X(TestIdentical)                                                                   \
    X(TestIdentical_issue15173)                                                        \
    X(TestIssue61737)                                                                  \
    X(TestNewAlias_Issue65455)                                                         \
    X(TestIssue15305)                                                                  \
    X(TestCompositeLitTypes)                                                           \
    X(TestObjectParents)                                                               \
    X(TestFailedImport)                                                                \
    X(TestFirstErrorInTypeSet)

TESTING_MAIN(TESTS)
