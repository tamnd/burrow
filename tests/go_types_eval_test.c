/* Derived from Go's src/go/types/eval_test.go. Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"
#include "go_types_helpers.h"
#include "go_types_typestring.h"

#include "burrow/burrow.h"
#include "burrow/fmt.h"
#include "burrow/go/ast.h"
#include "burrow/go/constant.h"
#include "burrow/go/parser.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/map.h"
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

/* split: strings.Cut, with both halves trimmed. */
static void split(Str s, Str sep, Str *before, Str *after) {
    Str b = s, af = S("");
    Int i = strings_index(s, sep);
    if (i >= 0) {
        b = str_from_bytes(s.p, i);
        af = str_from_bytes(s.p + i + sep.len, s.len - i - sep.len);
    }
    *before = strings_trim_space(b);
    *after = strings_trim_space(af);
}

static void TestEvalPos(TestingT *t) {
    /* Not in Go: testenv.MustHaveGoBuild. burrow imports fmt, math and io from
     * source, so it needs Go's sources. */
    if (!have_source_importer())
        testing_t_skip_v(t, "no Go sources to import from");

    /* The contents of block comments are of the form
     *     expr => value, type
     * where value may be the empty string. Each expr is evaluated at the
     * position of the comment and the result is compared with the expected
     * value and type. */
    Str sources[] = {
        S("\n\t\tpackage p\n\t\timport \"fmt\"\n\t\timport m \"math\"\n\t\tconst c = "
          "3.0\n\t\ttype T []int\n\t\tfunc f(a int, s string) float64 "
          "{\n\t\t\tfmt.Println(\"calling f\")\n\t\t\t_ = m.Pi // use package "
          "math\n\t\t\tconst d int = c + 1\n\t\t\tvar x int\n\t\t\tx = a + "
          "len(s)\n\t\t\treturn float64(x)\n\t\t\t/* true => true, untyped bool "
          "*/\n\t\t\t/* fmt.Println => , func(a ...any) (n int, err error) "
          "*/\n\t\t\t/* c => 3, untyped float */\n\t\t\t/* T => , p.T */\n\t\t\t/* a "
          "=> , int */\n\t\t\t/* s => , string */\n\t\t\t/* d => 4, int */\n\t\t\t/* x "
          "=> , int */\n\t\t\t/* d/c => 1, int */\n\t\t\t/* c/2 => 3/2, untyped float "
          "*/\n\t\t\t/* m.Pi < m.E => false, untyped bool */\n\t\t}\n\t\t"),
        S("\n\t\tpackage p\n\t\t/* c => 3, untyped float */\n\t\ttype T1 /* T1 => , "
          "p.T1 */ struct {}\n\t\tvar v1 /* v1 => , int */ = 42\n\t\tfunc /* f1 => , "
          "func(v1 float64) */ f1(v1 float64) {\n\t\t\t/* f1 => , func(v1 float64) "
          "*/\n\t\t\t/* v1 => , float64 */\n\t\t\tvar c /* c => 3, untyped float */ = "
          "\"foo\" /* c => , string */\n\t\t\t{\n\t\t\t\tvar c struct {\n\t\t\t\t\tc "
          "/* c => , string */ int\n\t\t\t\t}\n\t\t\t\t/* c => , struct{c int} "
          "*/\n\t\t\t\t_ = c\n\t\t\t}\n\t\t\t_ = func(a, b, c int /* c => , string */) "
          "/* c => , int */ {\n\t\t\t\t/* c => , int */\n\t\t\t}\n\t\t\t_ = "
          "c\n\t\t\ttype FT /* FT => , p.FT */ interface{}\n\t\t}\n\t\t"),
        S("\n\t\tpackage p\n\t\t/* T => , p.T */\n\t\t"),
        S("\n\t\tpackage p\n\t\timport \"io\"\n\t\ttype R = io.Reader\n\t\tfunc _() "
          "{\n\t\t\t/* interface{R}.Read => , func(_ interface{io.Reader}, p []byte) "
          "(n int, err error) */\n\t\t\t_ = func() {\n\t\t\t\t/* "
          "interface{io.Writer}.Write => , func(_ interface{io.Writer}, p []byte) (n "
          "int, err error) */\n\t\t\t\ttype io interface {} // must not shadow io in "
          "line above\n\t\t\t}\n\t\t\ttype R interface {} // must not shadow R in "
          "first line of this function body\n\t\t}\n\t\t"),
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    Slice files = slice_nil(TYPE_AST_FILE_PTR);
    for (Int i = 0; i < NELEM(sources); i++) {
        Error err = BURROW_NO_ERROR;
        AstFile *file = parser_parse_file(
            a, fset, S("p"), BURROW_ANY(TYPE_STRING, &sources[i]),
            PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "could not parse file %d: %s", i, error_text(err));

        if (strings_contains(sources[i], S("interface{R}.Read")))
            continue;

        files = slice_append(a, files, &file, 1);
    }

    TypesConfig conf = {.importer = default_importer(fset)};
    Error err = BURROW_NO_ERROR;
    TypesPackage *pkg = types_config_check(&conf, a, S("p"), fset, files, NULL, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, error_text(err));

    for (Int i = 0; i < files.len; i++) {
        AstFile *file = *(AstFile **)slice_at(files, i);
        for (Int j = 0; j < file->comments.len; j++) {
            AstCommentGroup *group = *(AstCommentGroup **)slice_at(file->comments, j);
            for (Int k = 0; k < group->list.len; k++) {
                AstComment *comment = *(AstComment **)slice_at(group->list, k);
                Str s = comment->text;
                if (s.len >= 4 && strings_has_prefix(s, S("/*")) &&
                    strings_has_suffix(s, S("*/"))) {
                    Str str, typ, val;
                    split(str_from_bytes(s.p + 2, s.len - 4), S(", "), &str, &typ);
                    split(str, S("=>"), &str, &val);
                    test_eval(t, a, fset, pkg, comment->slash, str, NULL, typ, val);
                }
            }
        }
    }
    arena_free(&ar);
}

/* checkExpr: the object expr denotes at pos, or NULL and *err. */
static TypesObject check_expr(Alloc *a, TokenFileSet *fset, TypesPackage *pkg,
                              TokenPos pos, Str str, Error *err) {
    AstExpr expr = parser_parse_expr_from(a, fset, S("eval"),
                                          BURROW_ANY(TYPE_STRING, &str), 0, err);
    if (BURROW_FAILED(*err))
        return NULL;

    TypesInfo info = {
        .uses = map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0),
        .selections = map_make(a, TYPE_UNSAFE_POINTER, TYPE_UNSAFE_POINTER, 0),
    };
    Error e = BURROW_NO_ERROR;
    if (!types_check_expr(a, fset, pkg, pos, expr, &info, &e)) {
        *err = fmt_errorf_v("CheckExpr(%q) failed: %s", str, error_text(e));
        return NULL;
    }
    switch (expr->kind) {
    case AST_KIND_IDENT: {
        TypesObject *obj = map_get(info.uses, &expr);
        if (obj != NULL)
            return *obj;
        break;
    }
    case AST_KIND_SELECTOR_EXPR: {
        AstSelectorExpr *sel = (AstSelectorExpr *)expr;
        TypesSelection **s = map_get(info.selections, &sel);
        if (s != NULL)
            return types_selection_obj(*s);
        TypesObject *obj = map_get(info.uses, &sel->sel);
        if (obj != NULL)
            return *obj; /* qualified identifier */
        break;
    }
    default:
        break;
    }
    *err = fmt_errorf_v("no object for %s", str);
    return NULL;
}

static void TestCheckExpr(TestingT *t) {
    /* Not in Go: testenv.MustHaveGoBuild. burrow imports fmt from source, so it
     * needs Go's sources. */
    if (!have_source_importer())
        testing_t_skip_v(t, "no Go sources to import from");

    /* Each block comment has the form expr => object: expr is an identifier or
     * selector expression that is passed to CheckExpr at the position of the
     * comment, and object is the string form of the object it denotes. */
    Str src = S("\n"
                "package p\n"
                "\n"
                "import \"fmt\"\n"
                "\n"
                "const c = 3.0\n"
                "type T []int\n"
                "type S struct{ X int }\n"
                "\n"
                "func f(a int, s string) S {\n"
                "\t/* fmt.Println => func fmt.Println(a ...any) (n int, err error) */\n"
                "\t/* fmt.Stringer.String => func (fmt.Stringer).String() string */\n"
                "\tfmt.Println(\"calling f\")\n"
                "\n"
                "\tvar fmt struct{ Println int }\n"
                "\t/* fmt => var fmt struct{Println int} */\n"
                "\t/* fmt.Println => field Println int */\n"
                "\t/* f(1, \"\").X => field X int */\n"
                "\tfmt.Println = 1\n"
                "\n"
                "\t/* append => builtin append */\n"
                "\n"
                "\t/* new(S).X => field X int */\n"
                "\n"
                "\treturn S{}\n"
                "}");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *f = must_parse(a, fset, src);

    TypesConfig conf = {.importer = default_importer(fset)};
    Error err = BURROW_NO_ERROR;
    Slice files = slice_append(a, slice_nil(TYPE_AST_FILE_PTR), &f, 1);
    TypesPackage *pkg = types_config_check(&conf, a, S("p"), fset, files, NULL, &err);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, error_text(err));

    for (Int i = 0; i < f->comments.len; i++) {
        AstCommentGroup *group = *(AstCommentGroup **)slice_at(f->comments, i);
        for (Int j = 0; j < group->list.len; j++) {
            AstComment *comment = *(AstComment **)slice_at(group->list, j);
            Str s = comment->text;
            if (s.len >= 4 && strings_has_prefix(s, S("/*")) &&
                strings_has_suffix(s, S("*/"))) {
                TokenPos pos = comment->slash;
                Str expr, want_obj;
                split(str_from_bytes(s.p + 2, s.len - 4), S("=>"), &expr, &want_obj);
                Error e = BURROW_NO_ERROR;
                TypesObject obj = check_expr(a, fset, pkg, pos, expr, &e);
                Str where =
                    token_position_string(token_file_set_position(fset, pos), a);
                if (obj == NULL) {
                    testing_t_errorf_v(t, "%s: %s", where, error_text(e));
                    continue;
                }
                Str got = types_object_string(a, obj, TYPES_NO_QUALIFIER);
                if (!str_eq(got, want_obj))
                    testing_t_errorf_v(t, "%s: checkExpr(%s) = %s, want %s", where,
                                       expr, got, want_obj);
            }
        }
    }
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
    X(TestEvalPos)                                                                     \
    X(TestCheckExpr)                                                                   \
    X(TestIssue65898)

TESTING_MAIN(TESTS)
