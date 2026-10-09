/* Derived from Go's src/go/build/constraint/expr_test.go and vers_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2020 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/go/build/constraint.h"
#include "burrow/mem/arena.h"

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* The unexported parts the tests reach into, defined in src/go/constraint.c. */
Str burrow__constraint_lex(Alloc *a, Str s, Int *i, Error *err);
ConstraintExpr burrow__constraint_parse_expr(Alloc *a, Str text, Error *err);
ConstraintExpr burrow__constraint_parse_plus_build_expr(Alloc *a, Str text, Error *err);
Error burrow__constraint_err_complex(void);

/* tag, not, and and or from expr.go, for building the expressions the tests
 * want. */
static ConstraintExpr gbc_new(Alloc *a, ConstraintKind kind, Str tag, ConstraintExpr x,
                              ConstraintExpr y) {
    switch ((int)kind) {
    case CONSTRAINT_KIND_TAG: {
        ConstraintTagExpr *e = (ConstraintTagExpr *)mem_alloc(
            a, sizeof(ConstraintTagExpr), _Alignof(ConstraintTagExpr));
        if (e == NULL)
            return NULL;
        e->expr.kind = kind;
        e->tag = tag;
        return &e->expr;
    }
    case CONSTRAINT_KIND_NOT: {
        ConstraintNotExpr *e = (ConstraintNotExpr *)mem_alloc(
            a, sizeof(ConstraintNotExpr), _Alignof(ConstraintNotExpr));
        if (e == NULL)
            return NULL;
        e->expr.kind = kind;
        e->x = x;
        return &e->expr;
    }
    default: {
        ConstraintAndExpr *e = (ConstraintAndExpr *)mem_alloc(
            a, sizeof(ConstraintAndExpr), _Alignof(ConstraintAndExpr));
        if (e == NULL)
            return NULL;
        e->expr.kind = kind;
        e->x = x;
        e->y = y;
        return &e->expr;
    }
    }
}

#define TAG(s) gbc_new(a, CONSTRAINT_KIND_TAG, BURROW_S(s), NULL, NULL)
#define NOT(x) gbc_new(a, CONSTRAINT_KIND_NOT, BURROW_STR_EMPTY, (x), NULL)
#define AND(x, y) gbc_new(a, CONSTRAINT_KIND_AND, BURROW_STR_EMPTY, (x), (y))
#define OR(x, y) gbc_new(a, CONSTRAINT_KIND_OR, BURROW_STR_EMPTY, (x), (y))

static void TestExprString(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const struct {
        ConstraintExpr x;
        Str out;
    } tests[] = {
        {TAG("abc"), S_("abc")},
        {NOT(TAG("abc")), S_("!abc")},
        {NOT(AND(TAG("abc"), TAG("def"))), S_("!(abc && def)")},
        {AND(TAG("abc"), OR(TAG("def"), TAG("ghi"))), S_("abc && (def || ghi)")},
        {OR(AND(TAG("abc"), TAG("def")), TAG("ghi")), S_("(abc && def) || ghi")},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        Str s = constraint_expr_string(tests[i].x, a);
        if (!str_eq(s, tests[i].out))
            testing_t_errorf_v(t, "%d: String() mismatch:\nhave %s\nwant %s", i, s,
                               tests[i].out);
    }
    arena_free(&ar);
}

static const struct {
    Str in;
    Str out;
} lex_tests[] = {
    {S_(""), S_("")},
    {S_("x"), S_("x")},
    {S_("x.y"), S_("x.y")},
    {S_("x_y"), S_("x_y")},
    {S_("\xce\xb1x"), S_("\xce\xb1x")},
    {S_("\xce\xb1x\xc2\xb2"), S_("\xce\xb1x err: invalid syntax at \xc2\xb2")},
    {S_("go1.2"), S_("go1.2")},
    {S_("x y"), S_("x y")},
    {S_("x!y"), S_("x ! y")},
    {S_("&&||!()xy yx "), S_("&& || ! ( ) xy yx")},
    {S_("x~"), S_("x err: invalid syntax at ~")},
    {S_("x ~"), S_("x err: invalid syntax at ~")},
    {S_("x &"), S_("x err: invalid syntax at &")},
    {S_("x &y"), S_("x err: invalid syntax at &")},
};

static void TestLex(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(lex_tests); i++) {
        StringsBuilder out = STRINGS_BUILDER(a);
        Int pos = 0;
        for (;;) {
            Error err = BURROW_NO_ERROR;
            Str tok = burrow__constraint_lex(a, lex_tests[i].in, &pos, &err);
            if (tok.len == 0 && BURROW_OK(err))
                break;
            if (strings_builder_len(&out) > 0)
                (void)strings_builder_write_byte(&out, ' ');
            if (!BURROW_OK(err)) {
                strings_builder_write_string(&out, BURROW_S("err: "), NULL);
                strings_builder_write_string(&out, error_text(err), NULL);
                break;
            }
            strings_builder_write_string(&out, tok, NULL);
        }
        Str have = strings_builder_string(&out);
        if (!str_eq(have, lex_tests[i].out))
            testing_t_errorf_v(t, "lex(%q):\nhave %s\nwant %s", lex_tests[i].in, have,
                               lex_tests[i].out);
    }
    arena_free(&ar);
}

static void TestParseExpr(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const struct {
        Str in;
        ConstraintExpr x;
    } tests[] = {
        {S_("x"), TAG("x")},
        {S_("x&&y"), AND(TAG("x"), TAG("y"))},
        {S_("x||y"), OR(TAG("x"), TAG("y"))},
        {S_("(x)"), TAG("x")},
        {S_("x||y&&z"), OR(TAG("x"), AND(TAG("y"), TAG("z")))},
        {S_("x&&y||z"), OR(AND(TAG("x"), TAG("y")), TAG("z"))},
        {S_("x&&(y||z)"), AND(TAG("x"), OR(TAG("y"), TAG("z")))},
        {S_("(x||y)&&z"), AND(OR(TAG("x"), TAG("y")), TAG("z"))},
        {S_("!(x&&y)"), NOT(AND(TAG("x"), TAG("y")))},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        Error err = BURROW_NO_ERROR;
        ConstraintExpr x = burrow__constraint_parse_expr(a, tests[i].in, &err);
        if (!BURROW_OK(err)) {
            testing_t_errorf_v(t, "%d: %s", i, error_text(err));
            continue;
        }
        Str have = constraint_expr_string(x, a);
        Str want = constraint_expr_string(tests[i].x, a);
        if (!str_eq(have, want))
            testing_t_errorf_v(t, "parseExpr(%q):\nhave %s\nwant %s", tests[i].in, have,
                               want);
    }
    arena_free(&ar);
}

static const struct {
    Str in;
    Int offset;
    Str err;
} parse_expr_error_tests[] = {
    {S_("x && "), 5, S_("unexpected end of expression")},
    {S_("x && ("), 6, S_("missing close paren")},
    {S_("x && ||"), 5, S_("unexpected token ||")},
    {S_("x && !"), 6, S_("unexpected end of expression")},
    {S_("x && !!"), 6, S_("double negation not allowed")},
    {S_("x !"), 2, S_("unexpected token !")},
    {S_("x && (y"), 5, S_("missing close paren")},
};

static void TestParseError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(parse_expr_error_tests); i++) {
        Error err = BURROW_NO_ERROR;
        ConstraintExpr x =
            burrow__constraint_parse_expr(a, parse_expr_error_tests[i].in, &err);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "parseExpr(%q) = %s, want error",
                               parse_expr_error_tests[i].in,
                               constraint_expr_string(x, a));
            continue;
        }
        const ConstraintSyntaxError *e =
            (const ConstraintSyntaxError *)errors_as(err, TYPE_CONSTRAINT_SYNTAX_ERROR);
        if (e == NULL || e->offset != parse_expr_error_tests[i].offset ||
            !str_eq(e->err, parse_expr_error_tests[i].err))
            testing_t_errorf_v(t, "parseExpr(%q): wrong error:\nhave %s\nwant %d: %s",
                               parse_expr_error_tests[i].in, error_text(err),
                               parse_expr_error_tests[i].offset,
                               parse_expr_error_tests[i].err);
    }
    arena_free(&ar);
}

/* The tags Eval asked about, as hasTag records them in Go's map. */
typedef struct GbcSeen {
    Str tags[8];
    Int n;
} GbcSeen;

static bool gbc_has_tag(void *env, Str tag) {
    GbcSeen *seen = (GbcSeen *)env;
    bool dup = false;
    for (Int i = 0; i < seen->n; i++)
        dup = dup || str_eq(seen->tags[i], tag);
    if (!dup && seen->n < NELEM(seen->tags))
        seen->tags[seen->n++] = tag;
    return str_eq(tag, BURROW_S("yes"));
}

static const struct {
    Str in;
    bool ok;
    Str tags;
} expr_eval_tests[] = {
    {S_("x"), false, S_("x")},           {S_("x && y"), false, S_("x y")},
    {S_("x || y"), false, S_("x y")},    {S_("!x && yes"), true, S_("x yes")},
    {S_("yes || y"), true, S_("y yes")},
};

static void TestExprEval(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(expr_eval_tests); i++) {
        Error err = BURROW_NO_ERROR;
        ConstraintExpr x =
            burrow__constraint_parse_expr(a, expr_eval_tests[i].in, &err);
        if (!BURROW_OK(err)) {
            testing_t_errorf_v(t, "%d: %s", i, error_text(err));
            continue;
        }
        GbcSeen seen = {0};
        bool ok =
            constraint_expr_eval(x, BURROW_FN(ConstraintTagFunc, gbc_has_tag, &seen));

        /* The two sets match when they are the same size and every wanted tag
         * was seen, since seen has no duplicates. */
        Slice want = strings_fields(a, expr_eval_tests[i].tags);
        bool same = want.len == seen.n;
        for (Int j = 0; same && j < want.len; j++) {
            bool found = false;
            for (Int k = 0; k < seen.n; k++)
                found = found || str_eq(seen.tags[k], ((const Str *)want.p)[j]);
            same = found;
        }
        if (ok != expr_eval_tests[i].ok || !same)
            testing_t_errorf_v(t, "Eval(%s):\nhave ok=%v, %d tags\nwant ok=%v, tags=%s",
                               expr_eval_tests[i].in, ok, seen.n, expr_eval_tests[i].ok,
                               expr_eval_tests[i].tags);
    }
    arena_free(&ar);
}

static void TestParsePlusBuildExpr(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const struct {
        Str in;
        ConstraintExpr x;
    } tests[] = {
        {S_("x"), TAG("x")},
        {S_("x,y"), AND(TAG("x"), TAG("y"))},
        {S_("x y"), OR(TAG("x"), TAG("y"))},
        {S_("x y,z"), OR(TAG("x"), AND(TAG("y"), TAG("z")))},
        {S_("x,y z"), OR(AND(TAG("x"), TAG("y")), TAG("z"))},
        {S_("x,!y !z"), OR(AND(TAG("x"), NOT(TAG("y"))), NOT(TAG("z")))},
        {S_("!! x"), OR(TAG("ignore"), TAG("x"))},
        {S_("!!x"), TAG("ignore")},
        {S_("!x"), NOT(TAG("x"))},
        {S_("!"), TAG("ignore")},
        {S_(""), TAG("ignore")},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        Error err = BURROW_NO_ERROR;
        ConstraintExpr x =
            burrow__constraint_parse_plus_build_expr(a, tests[i].in, &err);
        Str have = x == NULL ? BURROW_STR_EMPTY : constraint_expr_string(x, a);
        Str want = constraint_expr_string(tests[i].x, a);
        if (!str_eq(have, want))
            testing_t_errorf_v(t, "parsePlusBuildExpr(%q):\nhave %s\nwant %s",
                               tests[i].in, have, want);
    }
    arena_free(&ar);
}

static void TestParse(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const struct {
        Str in;
        ConstraintExpr x;
        Str err;
    } tests[] = {
        {S_("//+build !"), TAG("ignore"), S_("")},
        {S_("//+build"), TAG("ignore"), S_("")},
        {S_("//+build x y"), OR(TAG("x"), TAG("y")), S_("")},
        {S_("// +build x y \n"), OR(TAG("x"), TAG("y")), S_("")},
        {S_("// +build x y \n "), NULL, S_("not a build constraint")},
        {S_("// +build x y \nmore"), NULL, S_("not a build constraint")},
        {S_(" //+build x y"), NULL, S_("not a build constraint")},

        {S_("//go:build x && y"), AND(TAG("x"), TAG("y")), S_("")},
        {S_("//go:build x && y\n"), AND(TAG("x"), TAG("y")), S_("")},
        {S_("//go:build x && y\n "), NULL, S_("not a build constraint")},
        {S_("//go:build x && y\nmore"), NULL, S_("not a build constraint")},
        {S_(" //go:build x && y"), NULL, S_("not a build constraint")},
        {S_("//go:build\n"), NULL, S_("unexpected end of expression")},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        Error err = BURROW_NO_ERROR;
        ConstraintExpr x = constraint_parse(a, tests[i].in, &err);
        if (!BURROW_OK(err)) {
            if (tests[i].err.len == 0)
                testing_t_errorf_v(t, "Constraint(%q): unexpected error: %s",
                                   tests[i].in, error_text(err));
            else if (!strings_contains(error_text(err), tests[i].err))
                testing_t_errorf_v(t, "Constraint(%q): error %s, want %s", tests[i].in,
                                   error_text(err), tests[i].err);
            continue;
        }
        Str have = constraint_expr_string(x, a);
        if (tests[i].err.len > 0) {
            testing_t_errorf_v(t, "Constraint(%q) = %s, want error %s", tests[i].in,
                               have, tests[i].err);
            continue;
        }
        Str want = constraint_expr_string(tests[i].x, a);
        if (!str_eq(have, want))
            testing_t_errorf_v(t, "Constraint(%q):\nhave %s\nwant %s", tests[i].in,
                               have, want);
    }
    arena_free(&ar);
}

/* Up to two lines, without the "// +build " the test puts in front, and
 * err_complex for the one that wants errComplex. */
static const struct {
    Str in;
    Int n;
    Str out[2];
    bool err_complex;
} plus_build_lines_tests[] = {
    {S_("x"), 1, {S_("x")}, false},
    {S_("x && !y"), 1, {S_("x,!y")}, false},
    {S_("x || y"), 1, {S_("x y")}, false},
    {S_("x && (y || z)"), 2, {S_("x"), S_("y z")}, false},
    {S_("!(x && y)"), 1, {S_("!x !y")}, false},
    {S_("x || (y && z)"), 1, {S_("x y,z")}, false},
    {S_("w && (x || (y && z))"), 2, {S_("w"), S_("x y,z")}, false},
    {S_("v || (w && (x || (y && z)))"), 0, {S_(""), S_("")}, true},
};

static void TestPlusBuildLines(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(plus_build_lines_tests); i++) {
        Str in = plus_build_lines_tests[i].in;
        Error err = BURROW_NO_ERROR;
        ConstraintExpr x = burrow__constraint_parse_expr(a, in, &err);
        if (!BURROW_OK(err)) {
            testing_t_errorf_v(t, "%d: %s", i, error_text(err));
            continue;
        }
        Slice lines = constraint_plus_build_lines(a, x, &err);
        if (!BURROW_OK(err)) {
            if (!plus_build_lines_tests[i].err_complex)
                testing_t_errorf_v(t, "PlusBuildLines(%q): unexpected error: %s", in,
                                   error_text(err));
            else if (!errors_is(err, burrow__constraint_err_complex()))
                testing_t_errorf_v(t, "PlusBuildLines(%q): error %s, want %s", in,
                                   error_text(err),
                                   error_text(burrow__constraint_err_complex()));
            continue;
        }
        if (plus_build_lines_tests[i].err_complex) {
            testing_t_errorf_v(t, "PlusBuildLines(%q) = %d lines, want error %s", in,
                               lines.len, error_text(burrow__constraint_err_complex()));
            continue;
        }
        bool same = lines.len == plus_build_lines_tests[i].n;
        for (Int j = 0; same && j < lines.len; j++) {
            Str want =
                fmt_sprintf_v(a, "// +build %s", plus_build_lines_tests[i].out[j]);
            same = str_eq(((const Str *)lines.p)[j], want);
        }
        if (!same)
            testing_t_errorf_v(t,
                               "PlusBuildLines(%q): have %d lines, want %d starting %q",
                               in, lines.len, plus_build_lines_tests[i].n,
                               plus_build_lines_tests[i].out[0]);
    }
    arena_free(&ar);
}

/* maxSize in expr.go. */
enum { GBC_MAX_SIZE = 1000 };

static void TestSizeLimits(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const struct {
        Str name;
        Str rep;
    } tests[] = {
        {S_("go:build or limit"), S_("a || ")},
        {S_("go:build and limit"), S_("a && ")},
        {S_("go:build and depth limit"), S_("(a &&")},
        {S_("go:build or depth limit"), S_("(a ||")},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        Str expr = fmt_sprintf_v(a, "//go:build %s",
                                 strings_repeat(a, tests[i].rep, GBC_MAX_SIZE + 2));
        Error err = BURROW_NO_ERROR;
        (void)constraint_parse(a, expr, &err);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "%s: expression did not trigger limit",
                               tests[i].name);
            continue;
        }
        const ConstraintSyntaxError *e =
            (const ConstraintSyntaxError *)errors_as(err, TYPE_CONSTRAINT_SYNTAX_ERROR);
        if (e == NULL)
            testing_t_errorf_v(t, "%s: unexpected error: %s", tests[i].name,
                               error_text(err));
        else if (!str_eq(e->err, BURROW_S("build expression too large")))
            testing_t_errorf_v(t, "%s: unexpected syntax error: %s", tests[i].name,
                               e->err);
    }
    arena_free(&ar);
}

static void TestPlusSizeLimits(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Int max_old_size = 100;
    static const struct {
        Str name;
        Str rep;
    } tests[] = {
        {S_("+build or limit"), S_("a ")},
        {S_("+build and limit"), S_("a,")},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        Str expr = fmt_sprintf_v(a, "// +build %s",
                                 strings_repeat(a, tests[i].rep, max_old_size + 2));
        Error err = BURROW_NO_ERROR;
        (void)constraint_parse(a, expr, &err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "%s: expression did not trigger limit",
                               tests[i].name);
        else if (!errors_is(err, burrow__constraint_err_complex()))
            testing_t_errorf_v(t, "%s: unexpected error: got %q, want %q",
                               tests[i].name, error_text(err),
                               error_text(burrow__constraint_err_complex()));
    }
    arena_free(&ar);
}

static const struct {
    Str in;
    Int out;
} go_version_tests[] = {
    {S_("//go:build linux && go1.60"), 60},
    {S_("//go:build ignore && go1.60"), 60},
    {S_("//go:build ignore || go1.60"), -1},
    {S_("//go:build go1.50 || (ignore && go1.60)"), 50},
    {S_("// +build go1.60,linux"), 60},
    {S_("// +build go1.60 linux"), -1},
    {S_("//go:build go1.50 && !go1.60"), 50},
    {S_("//go:build !go1.60"), -1},
    {S_("//go:build linux && go1.50 || darwin && go1.60"), 50},
    {S_("//go:build linux && go1.50 || !(!darwin || !go1.60)"), 50},
};

static void TestGoVersion(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(go_version_tests); i++) {
        Error err = BURROW_NO_ERROR;
        ConstraintExpr x = constraint_parse(a, go_version_tests[i].in, &err);
        if (!BURROW_OK(err)) {
            testing_t_errorf_v(t, "%s", error_text(err));
            continue;
        }
        Str v = constraint_go_version(a, x);
        Str want = BURROW_STR_EMPTY;
        if (go_version_tests[i].out == 0)
            want = BURROW_S("go1");
        else if (go_version_tests[i].out > 0)
            want = fmt_sprintf_v(a, "go1.%d", go_version_tests[i].out);
        if (!str_eq(v, want))
            testing_t_errorf_v(t, "GoVersion(%q) = %q, want %q, nil",
                               go_version_tests[i].in, v, want);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestExprString)                                                                  \
    X(TestLex)                                                                         \
    X(TestParseExpr)                                                                   \
    X(TestParseError)                                                                  \
    X(TestExprEval)                                                                    \
    X(TestParsePlusBuildExpr)                                                          \
    X(TestParse)                                                                       \
    X(TestPlusBuildLines)                                                              \
    X(TestSizeLimits)                                                                  \
    X(TestPlusSizeLimits)                                                              \
    X(TestGoVersion)

TESTING_MAIN(TESTS)
