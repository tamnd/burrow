/* Derived from Go's src/go/types/api_test.go. Go source: go1.27.1.
 *
 * Only the tests that need no importer are here so far.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/go/ast.h"
#include "burrow/go/parser.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"
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

/* Not in Go: typecheck with no importer, since burrow has no default importer
 * yet. The sources here import nothing. */
static TypesPackage *typecheck(Alloc *a, Str src, TypesConfig *conf, TypesInfo *info,
                               Error *err) {
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, src);
    TypesConfig conf0 = {0};
    if (conf == NULL) {
        conf0.error = (TypesErrorFunc){collect_all, NULL}; /* collect all errors */
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

#define TESTS(X)                                                                       \
    X(TestInitOrderInfo)                                                               \
    X(TestMultiFileInitOrder)                                                          \
    X(TestFiles)

TESTING_MAIN(TESTS)
