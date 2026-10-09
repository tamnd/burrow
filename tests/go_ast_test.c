/* Derived from Go's src/go/ast/ast_test.go, directive_test.go and
 * print_test.go. The rest of Go's tests start from go/parser, which is not
 * here yet, so those below build their trees by hand and check what comes out
 * against what Go's go/ast prints for the same trees.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/go/ast.h"
#include "burrow/go/token.h"
#include "burrow/mem/arena.h"

#include <stddef.h>
#include <string.h>

#define S_(lit) BURROW_S_INIT(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* ----------------------------------------------------------------- helpers */

static void *gat_new(Alloc *a, AstKind kind) {
    AstNode n = ast_node_new(a, kind);
    if (n == NULL) {
        panic_str(BURROW_S("go_ast_test: out of memory"));
    }
    return n;
}

static AstIdent *gat_id(Alloc *a, Str name) {
    AstIdent *id = ast_new_ident(a, name);
    if (id == NULL) {
        panic_str(BURROW_S("go_ast_test: out of memory"));
    }
    return id;
}

/* A Slice of elem holding the n pointers in p. */
static Slice gat_list(Alloc *a, const Type *elem, void *const *p, Int n) {
    Slice s = slice_make(a, elem, n, n);
    if (s.p == NULL) {
        panic_str(BURROW_S("go_ast_test: out of memory"));
    }
    memcpy(s.p, p, (size_t)n * sizeof(void *));
    return s;
}

#define GAT_LIST(elem, ...)                                                            \
    gat_list(a, (elem), (void *const[]){__VA_ARGS__},                                  \
             (Int)(sizeof((void *const[]){__VA_ARGS__}) / sizeof(void *)))
#define ID(lit) gat_id(a, BURROW_S(lit))

static Str gat_bool(bool b) {
    return b ? BURROW_S("true") : BURROW_S("false");
}

static Str gat_pos(Alloc *a, TokenFileSet *fset, TokenPos p) {
    return token_position_string(token_file_set_position(fset, p), a);
}

/* The text of the first comment in g. */
static Str gat_text(AstCommentGroup *g) {
    return ((AstComment **)g->list.p)[0]->text;
}

static AstComment *gat_comment(Alloc *a, TokenPos slash, Str text) {
    AstComment *c = gat_new(a, AST_KIND_COMMENT);
    c->slash = slash;
    c->text = text;
    return c;
}

static AstCommentGroup *gat_group(Alloc *a, Slice list) {
    AstCommentGroup *g = gat_new(a, AST_KIND_COMMENT_GROUP);
    g->list = list;
    return g;
}

static AstBasicLit *gat_lit(Alloc *a, Token kind, Str value) {
    AstBasicLit *x = gat_new(a, AST_KIND_BASIC_LIT);
    x->kind = kind;
    x->value = value;
    return x;
}

static AstFieldList *gat_field_list(Alloc *a, Slice list) {
    AstFieldList *l = gat_new(a, AST_KIND_FIELD_LIST);
    l->list = list;
    return l;
}

static AstField *gat_field(Alloc *a, AstExpr type, Slice names) {
    AstField *f = gat_new(a, AST_KIND_FIELD);
    f->type = type;
    f->names = names;
    return f;
}

/* &ast.FuncType{Params: &ast.FieldList{}} */
static AstFuncType *gat_func_type(Alloc *a) {
    AstFuncType *t = gat_new(a, AST_KIND_FUNC_TYPE);
    t->params = gat_field_list(a, (Slice){0});
    return t;
}

static AstFuncDecl *gat_func(Alloc *a, AstIdent *name, AstFuncType *type) {
    AstFuncDecl *d = gat_new(a, AST_KIND_FUNC_DECL);
    d->name = name;
    d->type = type;
    return d;
}

static AstGenDecl *gat_gen(Alloc *a, Token tok, Slice specs) {
    AstGenDecl *d = gat_new(a, AST_KIND_GEN_DECL);
    d->tok = tok;
    d->specs = specs;
    return d;
}

static AstValueSpec *gat_value(Alloc *a, Slice names, AstExpr type, Slice values) {
    AstValueSpec *s = gat_new(a, AST_KIND_VALUE_SPEC);
    s->names = names;
    s->type = type;
    s->values = values;
    return s;
}

static AstTypeSpec *gat_type_spec(Alloc *a, AstIdent *name, AstExpr type) {
    AstTypeSpec *s = gat_new(a, AST_KIND_TYPE_SPEC);
    s->name = name;
    s->type = type;
    return s;
}

static bool gat_check(TestingT *t, const char *what, Str got, const char *want) {
    Str w = str_from_cstr(want);
    if (str_eq(got, w)) {
        return true;
    }
    testing_t_errorf_v(t, "%s: got\n%s\nwant\n%s", what, got, w);
    return false;
}

/* -------------------------------------------------------- what Go printed */

static const char gat_want_sort[] = "sort: 5 specs, 5 imports, 12 lines\n"
                                    "  \"a\" - s.go:4:2 end s.go:4:5\n"
                                    "  \"b\" - s.go:4:7 end s.go:4:10\n"
                                    "  \"c\" - s.go:7:2 end s.go:7:5\n"
                                    "  \"y\" x@s.go:8:2 s.go:8:2 end s.go:8:7\n"
                                    "  \"z\" - s.go:9:2 end s.go:9:5\n"
                                    "  comment \"// ca\" s.go:4:5\n"
                                    "  rparen s.go:10:1\n";

static const char gat_want_filter[] =
    "exports: true\n"
    "  const [ A ]\n"
    "  var [ D ] [ V ] lit 1 true\n"
    "  type T{3 fields true 1 0 0} I{1 methods true}\n"
    "  func F\n"
    "filter: true\n"
    "  type T{1 fields true 1} I{1 methods true}\n"
    "  func g\n"
    "none: false 0\n";

/* Printed by go1.26.5, except for the start positions. Go 1.26 bumped its file
 * counter before the i == 0 test, so FileStart stayed 0. Go 1.27 fixed that and
 * takes the smallest FileStart, which is a.go's 90. */
static const char gat_want_merge[] = "merge 0: package 300 start 90 end 600 name p\n"
                                     "  \"// A\" \"//\" \"// B1\" \"// B2\"\n"
                                     "  F (recv)M+doc G H F+doc (recv)M H F\n"
                                     "  \"os\" \"io\" \"fmt\" \"os\"\n"
                                     "  comments 3 nil false imports-nil false\n"
                                     "merge 5: package 300 start 90 end 600 name p\n"
                                     "  \"// A\" \"//\" \"// B1\" \"// B2\"\n"
                                     "  (recv)M+doc G F+doc H\n"
                                     "  \"os\" \"io\" \"fmt\"\n"
                                     "  comments 3 nil false imports-nil false\n"
                                     "merge 2: package 300 start 90 end 600 name p\n"
                                     "  \"// A\" \"//\" \"// B1\" \"// B2\"\n"
                                     "  F (recv)M+doc G H F+doc (recv)M H F\n"
                                     "  \"os\" \"io\" \"fmt\" \"os\"\n"
                                     "  comments 0 nil true imports-nil false\n";

static const char gat_want_fprint_nn[] = "     0  *ast.BinaryExpr {\n"
                                         "     1  .  X: *ast.Ident {\n"
                                         "     2  .  .  NamePos: 0\n"
                                         "     3  .  .  Name: \"s\"\n"
                                         "     4  .  }\n"
                                         "     5  .  OpPos: 0\n"
                                         "     6  .  Op: +\n"
                                         "     7  .  Y: *ast.CallExpr {\n"
                                         "     8  .  .  Fun: *ast.Ident {\n"
                                         "     9  .  .  .  NamePos: 0\n"
                                         "    10  .  .  .  Name: \"f\"\n"
                                         "    11  .  .  }\n"
                                         "    12  .  .  Lparen: 0\n"
                                         "    13  .  .  Args: []ast.Expr (len = 2) {\n"
                                         "    14  .  .  .  0: *(obj @ 1)\n"
                                         "    15  .  .  .  1: *ast.BasicLit {\n"
                                         "    16  .  .  .  .  ValuePos: 0\n"
                                         "    17  .  .  .  .  ValueEnd: 0\n"
                                         "    18  .  .  .  .  Kind: INT\n"
                                         "    19  .  .  .  .  Value: \"1\"\n"
                                         "    20  .  .  .  }\n"
                                         "    21  .  .  }\n"
                                         "    22  .  .  Ellipsis: 0\n"
                                         "    23  .  .  Rparen: 0\n"
                                         "    24  .  }\n"
                                         "    25  }\n";

static const char gat_want_fprint[] = "     0  *ast.BinaryExpr {\n"
                                      "     1  .  X: *ast.Ident {\n"
                                      "     2  .  .  NamePos: 0\n"
                                      "     3  .  .  Name: \"s\"\n"
                                      "     4  .  .  Obj: nil\n"
                                      "     5  .  }\n"
                                      "     6  .  OpPos: 0\n"
                                      "     7  .  Op: +\n"
                                      "     8  .  Y: *ast.CallExpr {\n"
                                      "     9  .  .  Fun: *ast.Ident {\n"
                                      "    10  .  .  .  NamePos: 0\n"
                                      "    11  .  .  .  Name: \"f\"\n"
                                      "    12  .  .  .  Obj: nil\n"
                                      "    13  .  .  }\n"
                                      "    14  .  .  Lparen: 0\n"
                                      "    15  .  .  Args: []ast.Expr (len = 2) {\n"
                                      "    16  .  .  .  0: *(obj @ 1)\n"
                                      "    17  .  .  .  1: *ast.BasicLit {\n"
                                      "    18  .  .  .  .  ValuePos: 0\n"
                                      "    19  .  .  .  .  ValueEnd: 0\n"
                                      "    20  .  .  .  .  Kind: INT\n"
                                      "    21  .  .  .  .  Value: \"1\"\n"
                                      "    22  .  .  .  }\n"
                                      "    23  .  .  }\n"
                                      "    24  .  .  Ellipsis: 0\n"
                                      "    25  .  .  Rparen: 0\n"
                                      "    26  .  }\n"
                                      "    27  }\n";

static const char gat_want_print[] =
    "     0  *ast.File {\n"
    "     1  .  Package: p.go:1:1\n"
    "     2  .  Name: *ast.Ident {\n"
    "     3  .  .  NamePos: p.go:1:9\n"
    "     4  .  .  Name: \"p\"\n"
    "     5  .  }\n"
    "     6  .  Decls: []ast.Decl (len = 1) {\n"
    "     7  .  .  0: *ast.GenDecl {\n"
    "     8  .  .  .  TokPos: p.go:3:1\n"
    "     9  .  .  .  Tok: var\n"
    "    10  .  .  .  Lparen: -\n"
    "    11  .  .  .  Specs: []ast.Spec (len = 1) {\n"
    "    12  .  .  .  .  0: *ast.ValueSpec {\n"
    "    13  .  .  .  .  .  Names: []*ast.Ident (len = 1) {\n"
    "    14  .  .  .  .  .  .  0: *ast.Ident {\n"
    "    15  .  .  .  .  .  .  .  NamePos: p.go:3:5\n"
    "    16  .  .  .  .  .  .  .  Name: \"x\"\n"
    "    17  .  .  .  .  .  .  }\n"
    "    18  .  .  .  .  .  }\n"
    "    19  .  .  .  .  .  Values: []ast.Expr (len = 1) {\n"
    "    20  .  .  .  .  .  .  0: *ast.BasicLit {\n"
    "    21  .  .  .  .  .  .  .  ValuePos: p.go:3:9\n"
    "    22  .  .  .  .  .  .  .  ValueEnd: -\n"
    "    23  .  .  .  .  .  .  .  Kind: INT\n"
    "    24  .  .  .  .  .  .  .  Value: \"1\"\n"
    "    25  .  .  .  .  .  .  }\n"
    "    26  .  .  .  .  .  }\n"
    "    27  .  .  .  .  }\n"
    "    28  .  .  .  }\n"
    "    29  .  .  .  Rparen: -\n"
    "    30  .  .  }\n"
    "    31  .  }\n"
    "    32  .  FileStart: -\n"
    "    33  .  FileEnd: -\n"
    "    34  .  GoVersion: \"\"\n"
    "    35  }\n";

static const char gat_want_walk[] =
    "inspect: File Ident ) GenDecl ValueSpec Ident ) BasicLit ) ) ValueSpec "
    "Ident ) BasicLit ) ) ) GenDecl ValueSpec Ident ) Ident ) Ident ) ) "
    "ValueSpec Ident ) CompositeLit Ident ) KeyValueExpr Ident ) BasicLit ) "
    ") KeyValueExpr Ident ) BasicLit ) ) ) ) ) GenDecl TypeSpec Ident ) "
    "StructType FieldList Field Field Field Field Field ) ) ) TypeSpec Ident "
    ") Ident ) ) TypeSpec Ident ) InterfaceType FieldList Field Field ) ) ) "
    ") FuncDecl Ident ) FuncType FieldList ) ) ) FuncDecl Ident ) FuncType "
    "FieldList ) ) ) )\n"
    "preorder: p A b c D int\n"
    "stack: p/1 A/3 b/3 c/3 D/3 int/3 V/3 T/4 X/5 y/5 T/3 X/6 y/6 int/6 E/7 "
    "f/7 G/7 h/6 z/6 int/6 u/3 int/3 I/3 M/6 n/6\n";

static const char gat_want_cmap[] =
    "cmap: 4\n"
    "  *ast.File \"// end\"\n"
    "  *ast.FuncDecl \"// doc F\" \"// after F\"\n"
    "  *ast.FuncDecl \"// floating\"\n"
    "  *ast.BlockStmt \"// in G\"\n"
    "filter G: 2\n"
    "comments: 5 \"// doc F\" \"// after F\" \"// floating\" \"// in G\" \"// end\"\n"
    "update: 3\n"
    "  *ast.File \"// end\"\n"
    "  *ast.FuncDecl \"// doc F\" \"// after F\" \"// floating\"\n"
    "  *ast.BlockStmt \"// in G\"\n"
    "nil: true\n";

/* ------------------------------------------------------------ comment text */

typedef struct GatCommentCase {
    Str list[8];
    Int n;
    Str text;
} GatCommentCase;

static const GatCommentCase gat_comments[] = {
    {{S_("//")}, 1, S_("")},
    {{S_("//   ")}, 1, S_("")},
    {{S_("//"), S_("//"), S_("//   ")}, 3, S_("")},
    {{S_("// foo   ")}, 1, S_("foo\n")},
    {{S_("//"), S_("//"), S_("// foo")}, 3, S_("foo\n")},
    {{S_("// foo  bar  ")}, 1, S_("foo  bar\n")},
    {{S_("// foo"), S_("// bar")}, 2, S_("foo\nbar\n")},
    {{S_("// foo"), S_("//"), S_("//"), S_("//"), S_("// bar")}, 5, S_("foo\n\nbar\n")},
    {{S_("// foo"), S_("/* bar */")}, 2, S_("foo\n bar\n")},
    {{S_("//"), S_("//"), S_("//"), S_("// foo"), S_("//"), S_("//"), S_("//")},
     7,
     S_("foo\n")},

    {{S_("/**/")}, 1, S_("")},
    {{S_("/*   */")}, 1, S_("")},
    {{S_("/**/"), S_("/**/"), S_("/*   */")}, 3, S_("")},
    {{S_("/* Foo   */")}, 1, S_(" Foo\n")},
    {{S_("/* Foo  Bar  */")}, 1, S_(" Foo  Bar\n")},
    {{S_("/* Foo*/"), S_("/* Bar*/")}, 2, S_(" Foo\n Bar\n")},
    {{S_("/* Foo*/"), S_("/**/"), S_("/**/"), S_("/**/"), S_("// Bar")},
     5,
     S_(" Foo\n\nBar\n")},
    {{S_("/* Foo*/"), S_("/*\n*/"), S_("//"), S_("/*\n*/"), S_("// Bar")},
     5,
     S_(" Foo\n\nBar\n")},
    {{S_("/* Foo*/"), S_("// Bar")}, 2, S_(" Foo\nBar\n")},
    {{S_("/* Foo\n Bar*/")}, 1, S_(" Foo\n Bar\n")},

    {{S_("// foo"), S_("//go:noinline"), S_("// bar"), S_("//:baz")},
     4,
     S_("foo\nbar\n:baz\n")},
    {{S_("// foo"), S_("//lint123:ignore"), S_("// bar")}, 3, S_("foo\nbar\n")},
};

static void TestCommentText(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(gat_comments); i++) {
        const GatCommentCase *c = &gat_comments[i];
        void *list[8];
        for (Int j = 0; j < c->n; j++) {
            list[j] = gat_comment(a, 0, c->list[j]);
        }
        AstCommentGroup *g =
            gat_group(a, gat_list(a, TYPE_AST_COMMENT_PTR, list, c->n));
        Str text = ast_comment_group_text(g, a);
        if (!str_eq(text, c->text)) {
            testing_t_errorf_v(t, "case %d: got %q; expected %q", i, text, c->text);
        }
    }
    CHECK(ast_comment_group_text(NULL, a).len == 0);
    arena_free(&ar);
}

/* ------------------------------------------------------------- directives */

typedef struct GatDirectiveCase {
    Str in;
    bool ok;
} GatDirectiveCase;

static const GatDirectiveCase gat_is_directive_tests[] = {
    {S_("abc"), false},       {S_("go:inline"), true},  {S_("Go:inline"), false},
    {S_("go:Inline"), false}, {S_(":inline"), false},   {S_("lint:ignore"), true},
    {S_("lint:1234"), true},  {S_("1234:lint"), true},  {S_("go: inline"), false},
    {S_("go:"), false},       {S_("go:*"), false},      {S_("go:x*"), true},
    {S_("export foo"), true}, {S_("extern foo"), true}, {S_("expert foo"), false},
};

/* The unexported isDirective, which src/go/ast.c lets the tests at. */
bool burrow__ast_is_directive(Str c);

static void TestIsDirective(TestingT *t) {
    for (Int i = 0; i < NELEM(gat_is_directive_tests); i++) {
        const GatDirectiveCase *tt = &gat_is_directive_tests[i];
        bool ok = burrow__ast_is_directive(tt->in);
        if (ok != tt->ok) {
            testing_t_errorf_v(t, "isDirective(%q) = %v, want %v", tt->in, ok, tt->ok);
        }
    }
}

static void TestParseDirectiveMatchesIsDirective(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(gat_is_directive_tests); i++) {
        const GatDirectiveCase *tt = &gat_is_directive_tests[i];
        bool want = tt->ok;
        if (strings_has_prefix(tt->in, BURROW_S("extern ")) ||
            strings_has_prefix(tt->in, BURROW_S("export "))) {
            /* ParseDirective does NOT support extern or export, unlike
             * isDirective. */
            want = false;
        }
        bool ok = false;
        Str c = fmt_sprintf_v(a, "//%s", tt->in);
        (void)ast_parse_directive(0, c, &ok);
        if (ok != want) {
            testing_t_errorf_v(t, "ParseDirective(0, %q) = %v, want %v", c, ok, want);
        }
    }
    arena_free(&ar);
}

typedef struct GatParseDirectiveCase {
    Str name;
    Str in;
    TokenPos pos;
    AstDirective want;
    bool want_ok;
} GatParseDirectiveCase;

#define GAT_LEN(lit) ((TokenPos)(sizeof(lit) - 1))
#define GAT_NO_DIRECTIVE {{NULL, 0}, {NULL, 0}, {NULL, 0}, 0, 0}

static const GatParseDirectiveCase gat_parse_directive_tests[] = {
    {S_("valid"),
     S_("//go:generate stringer -type Op -trimprefix Op"),
     10,
     {S_("go"), S_("generate"), S_("stringer -type Op -trimprefix Op"), 10,
      10 + GAT_LEN("//go:generate ")},
     true},
    {S_("no args"),
     S_("//go:build ignore"),
     20,
     {S_("go"), S_("build"), S_("ignore"), 20, 20 + GAT_LEN("//go:build ")},
     true},
    {S_("not a directive"), S_("// not a directive"), 30, GAT_NO_DIRECTIVE, false},
    {S_("not a comment"), S_("go:generate"), 40, GAT_NO_DIRECTIVE, false},
    {S_("empty"), S_(""), 50, GAT_NO_DIRECTIVE, false},
    {S_("just slashes"), S_("//"), 60, GAT_NO_DIRECTIVE, false},
    {S_("no name"), S_("//go:"), 70, GAT_NO_DIRECTIVE, false},
    {S_("no tool"), S_("//:generate"), 80, GAT_NO_DIRECTIVE, false},
    {S_("multiple spaces"),
     S_("//go:build  foo bar"),
     90,
     {S_("go"), S_("build"), S_("foo bar"), 90, 90 + GAT_LEN("//go:build  ")},
     true},
    {S_("trailing space"),
     S_("//go:build foo "),
     100,
     {S_("go"), S_("build"), S_("foo"), 100, 100 + GAT_LEN("//go:build ")},
     true},
};

static void TestParseDirective(TestingT *t) {
    for (Int i = 0; i < NELEM(gat_parse_directive_tests); i++) {
        const GatParseDirectiveCase *test = &gat_parse_directive_tests[i];
        bool ok = false;
        AstDirective got = ast_parse_directive(test->pos, test->in, &ok);
        if (ok != test->want_ok) {
            testing_t_errorf_v(t, "%s: ParseDirective(%q) ok = %v, want %v", test->name,
                               test->in, ok, test->want_ok);
            continue;
        }
        const AstDirective *w = &test->want;
        if (!str_eq(got.tool, w->tool) || !str_eq(got.name, w->name) ||
            !str_eq(got.args, w->args) || got.slash != w->slash ||
            got.args_pos != w->args_pos) {
            testing_t_errorf_v(t,
                               "%s: ParseDirective(%q) = {%q %q %q %d %d}, want "
                               "{%q %q %q %d %d}",
                               test->name, test->in, got.tool, got.name, got.args,
                               got.slash, got.args_pos, w->tool, w->name, w->args,
                               w->slash, w->args_pos);
        }
    }
}

typedef struct GatParseArgsCase {
    Str name;
    Str args;
    TokenPos args_pos;
    AstDirectiveArg want[3];
    Int nwant;
    bool want_err;
} GatParseArgsCase;

static const GatParseArgsCase gat_parse_args_tests[] = {
    {S_("simple"),
     S_("stringer -type Op"),
     10,
     {{S_("stringer"), 10},
      {S_("-type"), 10 + GAT_LEN("stringer ")},
      {S_("Op"), 10 + GAT_LEN("stringer -type ")}},
     3,
     false},
    {S_("quoted"),
     S_("\"foo bar\" baz"),
     10,
     {{S_("foo bar"), 10}, {S_("baz"), 10 + GAT_LEN("\"foo bar\" ")}},
     2,
     false},
    {S_("raw quoted"),
     S_("`foo bar` baz"),
     10,
     {{S_("foo bar"), 10}, {S_("baz"), 10 + GAT_LEN("`foo bar` ")}},
     2,
     false},
    {S_("escapes"),
     S_("\"foo\\U0001F60Abar\" `a\\tb`"),
     10,
     {{S_("foo\xF0\x9F\x98\x8A"
          "bar"),
       10},
      {S_("a\\tb"), 10 + GAT_LEN("\"foo\\U0001F60Abar\" ")}},
     2,
     false},
    {S_("empty args"), S_(""), 10, {{{NULL, 0}, 0}}, 0, false},
    {S_("spaces"),
     S_("  foo   bar  "),
     10,
     {{S_("foo"), 10 + GAT_LEN("  ")}, {S_("bar"), 10 + GAT_LEN("  foo   ")}},
     2,
     false},
    {S_("unterminated quote"), S_("`foo"), 0, {{{NULL, 0}, 0}}, 0, true},
    {S_("no space after quote"), S_("\"foo\"bar"), 0, {{{NULL, 0}, 0}}, 0, true},
};

static void TestParseArgs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(gat_parse_args_tests); i++) {
        const GatParseArgsCase *test = &gat_parse_args_tests[i];
        AstDirective d = {BURROW_S("go"), BURROW_S("generate"), test->args, 0,
                          test->args_pos};
        Error err = BURROW_NO_ERROR;
        Slice got = ast_directive_parse_args(&d, a, &err);
        if (!BURROW_OK(err)) {
            if (!test->want_err) {
                testing_t_errorf_v(t, "%s: ParseArgs(%q) = error %v", test->name,
                                   test->args, err);
            }
            continue;
        }
        if (test->want_err) {
            testing_t_errorf_v(t, "%s: ParseArgs(%q) = %d args; want error", test->name,
                               test->args, got.len);
            continue;
        }
        /* Go's DeepEqual tells an empty slice from nil, and wants the empty one. */
        if (slice_is_nil(got) || got.len != test->nwant) {
            testing_t_errorf_v(t, "%s: ParseArgs(%q) = %d args (nil %v); want %d",
                               test->name, test->args, got.len, slice_is_nil(got),
                               test->nwant);
            continue;
        }
        const AstDirectiveArg *args = (const AstDirectiveArg *)got.p;
        for (Int j = 0; j < got.len; j++) {
            const AstDirectiveArg *w = &test->want[j];
            if (!str_eq(args[j].arg, w->arg) || args[j].pos != w->pos) {
                testing_t_errorf_v(t, "%s: arg %d = {%q %d}, want {%q %d}", test->name,
                                   j, args[j].arg, args[j].pos, w->arg, w->pos);
            }
        }
    }
    arena_free(&ar);
}

/* ---------------------------------------------------------------- printing */

/* The unnamed types of print_test.go's table. */
BURROW_MAP_TYPE(GatExprStrMap, AstExpr, Str);
BURROW_MAP_TYPE(GatStrIntMap, Str, Int);
BURROW_PTR_TYPE(GatIntPtr, Int);
BURROW_ARRAY_TYPE(GatInt3, Int, 3);
BURROW_ARRAY_TYPE(GatInt1, Int, 1);
BURROW_SLICE_TYPE(GatInts, Int);

/* [0]int, which C cannot declare the way the macro does. */
static const Type gat_int0 = {
    {NULL, 0}, {NULL, 0}, KIND_ARRAY, 0,    (uint16_t)_Alignof(Int),
    0,         0,         NULL,       NULL, TYPE_INT,
    NULL,      0,         0,          NULL,
};

typedef struct GatX {
    Int x;
} GatX;

typedef struct GatXY {
    Int x, y;
} GatXY;

static const Field gat_x_fields[] = {{S_("x"), S_(""), TYPE_INT, offsetof(GatX, x)}};
static const Field gat_xy_fields[] = {
    {S_("X"), S_(""), TYPE_INT, offsetof(GatXY, x)},
    {S_("y"), S_(""), TYPE_INT, offsetof(GatXY, y)},
};
static const Field gat_xY_fields[] = {
    {S_("X"), S_(""), TYPE_INT, offsetof(GatXY, x)},
    {S_("Y"), S_(""), TYPE_INT, offsetof(GatXY, y)},
};

static const Type gat_empty_struct = {
    {NULL, 0}, {NULL, 0}, KIND_STRUCT, 0, 1, 0, 0, NULL, NULL, NULL, NULL, 0, 0, NULL,
};
static const Type gat_x_struct = {
    {NULL, 0},
    {NULL, 0},
    KIND_STRUCT,
    (uint32_t)sizeof(GatX),
    (uint16_t)_Alignof(GatX),
    1,
    0,
    gat_x_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};
static const Type gat_xy_struct = {
    {NULL, 0},
    {NULL, 0},
    KIND_STRUCT,
    (uint32_t)sizeof(GatXY),
    (uint16_t)_Alignof(GatXY),
    2,
    0,
    gat_xy_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};
static const Type gat_xY_struct = {
    {NULL, 0},
    {NULL, 0},
    KIND_STRUCT,
    (uint32_t)sizeof(GatXY),
    (uint16_t)_Alignof(GatXY),
    2,
    0,
    gat_xY_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static void gat_print_case(TestingT *t, Alloc *a, Any x, const char *want) {
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err = ast_fprint(a, strings_builder_as_io_writer(&b), NULL, x,
                           (AstFieldFilter){NULL, NULL});
    if (!BURROW_OK(err)) {
        testing_t_errorf_v(t, "Fprint failed: %v", err);
    }
    gat_check(t, "Fprint", strings_builder_string(&b), want);
}

static void TestPrint(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* basic types */
    gat_print_case(t, a, (Any){NULL, NULL}, "     0  nil\n");
    bool yes = true;
    gat_print_case(t, a, BURROW_ANY(TYPE_BOOL, &yes), "     0  true\n");
    Int n42 = 42;
    gat_print_case(t, a, BURROW_ANY(TYPE_INT, &n42), "     0  42\n");
    double pi = 3.14;
    gat_print_case(t, a, BURROW_ANY(TYPE_FLOAT64, &pi), "     0  3.14\n");
    Complex128 c = {1, 2.718};
    gat_print_case(t, a, BURROW_ANY(TYPE_COMPLEX128, &c), "     0  (1+2.718i)\n");
    Str foobar = BURROW_S("foobar");
    gat_print_case(t, a, BURROW_ANY(TYPE_STRING, &foobar), "     0  \"foobar\"\n");

    /* maps */
    Map *m0 = map_make(a, TYPE_AST_EXPR, TYPE_STRING, 0);
    Map *m1 = map_make(a, TYPE_STRING, TYPE_INT, 1);
    Str ka = BURROW_S("a");
    Int one = 1;
    CHECK(m0 != NULL && m1 != NULL && map_set(m1, &ka, &one));
    gat_print_case(t, a, BURROW_ANY(TYPE_OF(GatExprStrMap), &m0),
                   "     0  map[ast.Expr]string (len = 0) {}\n");
    gat_print_case(t, a, BURROW_ANY(TYPE_OF(GatStrIntMap), &m1),
                   "     0  map[string]int (len = 1) {\n"
                   "     1  .  \"a\": 1\n"
                   "     2  }\n");

    /* pointers */
    Int zero = 0;
    GatIntPtr pz = &zero;
    gat_print_case(t, a, BURROW_ANY(TYPE_OF(GatIntPtr), &pz), "     0  *0\n");

    /* arrays */
    Int none = 0;
    gat_print_case(t, a, BURROW_ANY(&gat_int0, &none), "     0  [0]int {}\n");
    GatInt3 a3 = {{1, 2, 3}};
    gat_print_case(t, a, BURROW_ANY(TYPE_OF(GatInt3), &a3),
                   "     0  [3]int {\n"
                   "     1  .  0: 1\n"
                   "     2  .  1: 2\n"
                   "     3  .  2: 3\n"
                   "     4  }\n");
    GatInt1 a1 = {{42}};
    gat_print_case(t, a, BURROW_ANY(TYPE_OF(GatInt1), &a1),
                   "     0  [1]int {\n"
                   "     1  .  0: 42\n"
                   "     2  }\n");

    /* slices */
    GatInts s0 = slice_make(a, TYPE_INT, 0, 0);
    gat_print_case(t, a, BURROW_ANY(TYPE_OF(GatInts), &s0),
                   "     0  []int (len = 0) {}\n");
    GatInts s3 = slice_from(a3.v, 3, 3, TYPE_INT);
    gat_print_case(t, a, BURROW_ANY(TYPE_OF(GatInts), &s3),
                   "     0  []int (len = 3) {\n"
                   "     1  .  0: 1\n"
                   "     2  .  1: 2\n"
                   "     3  .  2: 3\n"
                   "     4  }\n");

    /* structs */
    gat_print_case(t, a, BURROW_ANY(&gat_empty_struct, &none),
                   "     0  struct {} {}\n");
    GatX x = {007};
    gat_print_case(t, a, BURROW_ANY(&gat_x_struct, &x),
                   "     0  struct { x int } {}\n");
    GatXY xy = {42, 991};
    gat_print_case(t, a, BURROW_ANY(&gat_xy_struct, &xy),
                   "     0  struct { X int; y int } {\n"
                   "     1  .  X: 42\n"
                   "     2  }\n");
    gat_print_case(t, a, BURROW_ANY(&gat_xY_struct, &xy),
                   "     0  struct { X int; Y int } {\n"
                   "     1  .  X: 42\n"
                   "     2  .  Y: 991\n"
                   "     3  }\n");
    arena_free(&ar);
}

static bool gat_not_nil(void *env, Str name, Any v) {
    (void)env;
    return ast_not_nil_filter(name, v);
}

/* A shared node is printed once and then referred to by line, and a
 * TokenPos is a position when there is a file set. */
static void TestFprintTree(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    IoWriter w = strings_builder_as_io_writer(&b);

    AstIdent *shared = ID("s");
    AstCallExpr *call = gat_new(a, AST_KIND_CALL_EXPR);
    call->fun = &ID("f")->node;
    call->args = GAT_LIST(TYPE_AST_EXPR, shared, gat_lit(a, TOKEN_INT, BURROW_S("1")));
    AstBinaryExpr *x = gat_new(a, AST_KIND_BINARY_EXPR);
    x->x = &shared->node;
    x->op = TOKEN_ADD;
    x->y = &call->node;
    Any ax = BURROW_ANY(TYPE_OF(AstBinaryExprPtr), &x);
    CHECK(BURROW_OK(ast_fprint(a, w, NULL, ax, (AstFieldFilter){gat_not_nil, NULL})));
    gat_check(t, "Fprint with NotNilFilter", strings_builder_string(&b),
              gat_want_fprint_nn);
    strings_builder_reset(&b);
    CHECK(BURROW_OK(ast_fprint(a, w, NULL, ax, (AstFieldFilter){NULL, NULL})));
    gat_check(t, "Fprint", strings_builder_string(&b), gat_want_fprint);

    Str src = BURROW_S("package p\n\nvar x = 1\n");
    TokenFileSet *fset = token_new_file_set(a);
    TokenFile *f = token_file_set_add_file(fset, BURROW_S("p.go"), -1, src.len);
    token_file_set_lines_for_content(f, slice_from_str(a, src));
    AstIdent *vx = ID("x");
    vx->name_pos = token_file_pos(f, 15);
    AstBasicLit *v1 = gat_lit(a, TOKEN_INT, BURROW_S("1"));
    v1->value_pos = token_file_pos(f, 19);
    AstValueSpec *vs = gat_value(a, GAT_LIST(TYPE_AST_IDENT_PTR, vx), NULL,
                                 GAT_LIST(TYPE_AST_EXPR, v1));
    AstGenDecl *d = gat_gen(a, TOKEN_VAR, GAT_LIST(TYPE_AST_SPEC, vs));
    d->tok_pos = token_file_pos(f, 11);
    AstFile *file = gat_new(a, AST_KIND_FILE);
    file->package = token_file_pos(f, 0);
    file->name = ID("p");
    file->name->name_pos = token_file_pos(f, 8);
    file->decls = GAT_LIST(TYPE_AST_DECL, d);
    strings_builder_reset(&b);
    CHECK(BURROW_OK(ast_fprint(a, w, fset, BURROW_ANY(TYPE_OF(AstFilePtr), &file),
                               (AstFieldFilter){gat_not_nil, NULL})));
    gat_check(t, "Print", strings_builder_string(&b), gat_want_print);

    /* Go's NotNilFilter: nil interfaces, pointers and slices are left out, and
     * everything else is kept. */
    AstIdent *nil_id = NULL;
    Slice nil_slice = {0};
    Int n = 0;
    CHECK(!ast_not_nil_filter(BURROW_S("X"), BURROW_ANY(TYPE_AST_IDENT_PTR, &nil_id)));
    CHECK(!ast_not_nil_filter(BURROW_S("X"), BURROW_ANY(TYPE_OF(GatInts), &nil_slice)));
    CHECK(ast_not_nil_filter(BURROW_S("X"), BURROW_ANY(TYPE_OF(AstBinaryExprPtr), &x)));
    CHECK(ast_not_nil_filter(BURROW_S("X"), BURROW_ANY(TYPE_INT, &n)));
    arena_free(&ar);
}

/* ------------------------------------------------------------ sort imports */

typedef struct GatCursor {
    Str src;
    Int at;
    TokenFile *f;
} GatCursor;

/* Where s is next in the source, from where the last one ended. */
static TokenPos gat_next(GatCursor *c, Str s) {
    Int i =
        c->at + strings_index(str_from_bytes(c->src.p + c->at, c->src.len - c->at), s);
    c->at = i + s.len;
    return token_file_pos(c->f, i);
}

static AstImportSpec *gat_import(Alloc *a, GatCursor *c, AstFile *file, Str name,
                                 Str path, Str comment) {
    AstImportSpec *s = gat_new(a, AST_KIND_IMPORT_SPEC);
    if (name.len > 0) {
        s->name = gat_id(a, name);
        s->name->name_pos = gat_next(c, name);
    }
    s->path = gat_lit(a, TOKEN_STRING, path);
    s->path->value_pos = gat_next(c, path);
    if (comment.len > 0) {
        AstComment *k = gat_comment(a, gat_next(c, comment), comment);
        s->comment = gat_group(a, GAT_LIST(TYPE_AST_COMMENT_PTR, k));
        file->comments = slice_append(a, file->comments, &s->comment, 1);
    }
    return s;
}

static void TestSortImports(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    IoWriter w = strings_builder_as_io_writer(&b);

    Str src = BURROW_S(
        "package p\n\nimport (\n\t\"b\"\n\t\"a\" // ca\n\t\"b\"\n\n\t\"z\"\n\tx "
        "\"y\"\n\t\"c\"\n)\n\nvar v int\n");
    TokenFileSet *fset = token_new_file_set(a);
    TokenFile *f = token_file_set_add_file(fset, BURROW_S("s.go"), -1, src.len);
    token_file_set_lines_for_content(f, slice_from_str(a, src));
    GatCursor c = {src, 0, f};
    AstFile *file = gat_new(a, AST_KIND_FILE);
    file->comments = slice_make(a, TYPE_AST_COMMENT_GROUP_PTR, 0, 0);
    file->package = gat_next(&c, BURROW_S("package"));
    file->name = ID("p");
    file->name->name_pos = gat_next(&c, BURROW_S("p"));
    AstGenDecl *d = gat_gen(a, TOKEN_IMPORT, (Slice){0});
    d->tok_pos = gat_next(&c, BURROW_S("import"));
    d->lparen = gat_next(&c, BURROW_S("("));
    Str none = BURROW_STR_EMPTY;
    AstImportSpec *s1 = gat_import(a, &c, file, none, BURROW_S("\"b\""), none);
    AstImportSpec *s2 =
        gat_import(a, &c, file, none, BURROW_S("\"a\""), BURROW_S("// ca"));
    AstImportSpec *s3 = gat_import(a, &c, file, none, BURROW_S("\"b\""), none);
    AstImportSpec *s4 = gat_import(a, &c, file, none, BURROW_S("\"z\""), none);
    AstImportSpec *s5 = gat_import(a, &c, file, BURROW_S("x"), BURROW_S("\"y\""), none);
    AstImportSpec *s6 = gat_import(a, &c, file, none, BURROW_S("\"c\""), none);
    d->specs = GAT_LIST(TYPE_AST_SPEC, s1, s2, s3, s4, s5, s6);
    d->rparen = gat_next(&c, BURROW_S(")"));
    AstGenDecl *v = gat_gen(a, TOKEN_VAR, (Slice){0});
    v->tok_pos = gat_next(&c, BURROW_S("var"));
    file->decls = GAT_LIST(TYPE_AST_DECL, d, v);
    file->imports = GAT_LIST(TYPE_AST_IMPORT_SPEC_PTR, s1, s2, s3, s4, s5, s6);

    ast_sort_imports(a, fset, file);
    fmt_fprintf_v(w, "sort: %d specs, %d imports, %d lines\n", d->specs.len,
                  file->imports.len, token_file_line_count(f));
    for (Int i = 0; i < d->specs.len; i++) {
        AstImportSpec *s = ((AstImportSpec **)d->specs.p)[i];
        Str name = BURROW_S("-");
        if (s->name != NULL) {
            name = fmt_sprintf_v(a, "%s@%s", s->name->name,
                                 gat_pos(a, fset, s->name->name_pos));
        }
        fmt_fprintf_v(w, "  %s %s %s end %s\n", s->path->value, name,
                      gat_pos(a, fset, s->path->value_pos),
                      gat_pos(a, fset, ast_import_spec_end(s)));
    }
    for (Int i = 0; i < file->comments.len; i++) {
        AstCommentGroup *g = ((AstCommentGroup **)file->comments.p)[i];
        fmt_fprintf_v(w, "  comment %q %s\n", gat_text(g),
                      gat_pos(a, fset, ast_comment_group_pos(g)));
    }
    fmt_fprintf_v(w, "  rparen %s\n", gat_pos(a, fset, d->rparen));
    gat_check(t, "SortImports", strings_builder_string(&b), gat_want_sort);
    arena_free(&ar);
}

/* ----------------------------------------------------------------- filters */

static AstFile *gat_filter_tree(Alloc *a) {
    AstStarExpr *star = gat_new(a, AST_KIND_STAR_EXPR);
    star->x = &ID("E")->node;
    AstSelectorExpr *sel = gat_new(a, AST_KIND_SELECTOR_EXPR);
    sel->x = &ID("f")->node;
    sel->sel = ID("G");
    AstStructType *st = gat_new(a, AST_KIND_STRUCT_TYPE);
    st->fields = gat_field_list(
        a,
        GAT_LIST(
            TYPE_AST_FIELD_PTR,
            gat_field(a, &ID("int")->node,
                      GAT_LIST(TYPE_AST_IDENT_PTR, ID("X"), ID("y"))),
            gat_field(a, &star->node, (Slice){0}), gat_field(a, &sel->node, (Slice){0}),
            gat_field(a, &ID("h")->node, (Slice){0}),
            gat_field(a, &ID("int")->node, GAT_LIST(TYPE_AST_IDENT_PTR, ID("z")))));
    AstInterfaceType *it = gat_new(a, AST_KIND_INTERFACE_TYPE);
    it->methods =
        gat_field_list(a, GAT_LIST(TYPE_AST_FIELD_PTR,
                                   gat_field(a, &gat_func_type(a)->node,
                                             GAT_LIST(TYPE_AST_IDENT_PTR, ID("M"))),
                                   gat_field(a, &gat_func_type(a)->node,
                                             GAT_LIST(TYPE_AST_IDENT_PTR, ID("n")))));
    AstKeyValueExpr *kx = gat_new(a, AST_KIND_KEY_VALUE_EXPR);
    kx->key = &ID("X")->node;
    kx->value = &gat_lit(a, TOKEN_INT, BURROW_S("1"))->node;
    AstKeyValueExpr *ky = gat_new(a, AST_KIND_KEY_VALUE_EXPR);
    ky->key = &ID("y")->node;
    ky->value = &gat_lit(a, TOKEN_INT, BURROW_S("2"))->node;
    AstCompositeLit *cl = gat_new(a, AST_KIND_COMPOSITE_LIT);
    cl->type = &ID("T")->node;
    cl->elts = GAT_LIST(TYPE_AST_EXPR, kx, ky);

    AstFile *f = gat_new(a, AST_KIND_FILE);
    f->name = ID("p");
    f->decls = GAT_LIST(
        TYPE_AST_DECL,
        gat_gen(a, TOKEN_CONST,
                GAT_LIST(TYPE_AST_SPEC,
                         gat_value(a, GAT_LIST(TYPE_AST_IDENT_PTR, ID("A")), NULL,
                                   GAT_LIST(TYPE_AST_EXPR,
                                            gat_lit(a, TOKEN_INT, BURROW_S("1")))),
                         gat_value(a, GAT_LIST(TYPE_AST_IDENT_PTR, ID("b")), NULL,
                                   GAT_LIST(TYPE_AST_EXPR,
                                            gat_lit(a, TOKEN_INT, BURROW_S("2")))))),
        gat_gen(a, TOKEN_VAR,
                GAT_LIST(TYPE_AST_SPEC,
                         gat_value(a, GAT_LIST(TYPE_AST_IDENT_PTR, ID("c"), ID("D")),
                                   &ID("int")->node, (Slice){0}),
                         gat_value(a, GAT_LIST(TYPE_AST_IDENT_PTR, ID("V")), NULL,
                                   GAT_LIST(TYPE_AST_EXPR, cl)))),
        gat_gen(a, TOKEN_TYPE_,
                GAT_LIST(TYPE_AST_SPEC, gat_type_spec(a, ID("T"), &st->node),
                         gat_type_spec(a, ID("u"), &ID("int")->node),
                         gat_type_spec(a, ID("I"), &it->node))),
        gat_func(a, ID("F"), gat_func_type(a)), gat_func(a, ID("g"), gat_func_type(a)));
    return f;
}

static void gat_describe(IoWriter w, Alloc *a, AstFile *file) {
    for (Int i = 0; i < file->decls.len; i++) {
        AstDecl d = ((AstDecl *)file->decls.p)[i];
        if (d->kind == AST_KIND_FUNC_DECL) {
            fmt_fprintf_v(w, "  func %s\n", ((AstFuncDecl *)d)->name->name);
            continue;
        }
        AstGenDecl *g = (AstGenDecl *)d;
        fmt_fprintf_v(w, "  %s", token_string(g->tok, a));
        for (Int j = 0; j < g->specs.len; j++) {
            AstSpec s = ((AstSpec *)g->specs.p)[j];
            if (s->kind == AST_KIND_VALUE_SPEC) {
                AstValueSpec *vs = (AstValueSpec *)s;
                fmt_fprintf_v(w, " [");
                for (Int k = 0; k < vs->names.len; k++) {
                    fmt_fprintf_v(w, " %s", ((AstIdent **)vs->names.p)[k]->name);
                }
                fmt_fprintf_v(w, " ]");
                for (Int k = 0; k < vs->values.len; k++) {
                    AstExpr v = ((AstExpr *)vs->values.p)[k];
                    if (v->kind == AST_KIND_COMPOSITE_LIT) {
                        AstCompositeLit *cl = (AstCompositeLit *)v;
                        fmt_fprintf_v(w, " lit %d %s", cl->elts.len,
                                      gat_bool(cl->incomplete));
                    }
                }
            } else if (s->kind == AST_KIND_TYPE_SPEC) {
                AstTypeSpec *ts = (AstTypeSpec *)s;
                fmt_fprintf_v(w, " %s", ts->name->name);
                if (ts->type->kind == AST_KIND_STRUCT_TYPE) {
                    AstStructType *st = (AstStructType *)ts->type;
                    fmt_fprintf_v(w, "{%d fields %s", st->fields->list.len,
                                  gat_bool(st->incomplete));
                    for (Int k = 0; k < st->fields->list.len; k++) {
                        AstField *f = ((AstField **)st->fields->list.p)[k];
                        fmt_fprintf_v(w, " %d", f->names.len);
                    }
                    fmt_fprintf_v(w, "}");
                } else if (ts->type->kind == AST_KIND_INTERFACE_TYPE) {
                    AstInterfaceType *it = (AstInterfaceType *)ts->type;
                    fmt_fprintf_v(w, "{%d methods %s}", it->methods->list.len,
                                  gat_bool(it->incomplete));
                }
            }
        }
        fmt_fprintf_v(w, "\n");
    }
}

static bool gat_keep(void *env, Str name) {
    (void)env;
    return str_eq(name, BURROW_S("X")) || str_eq(name, BURROW_S("n")) ||
           str_eq(name, BURROW_S("g"));
}

static bool gat_none(void *env, Str name) {
    (void)env;
    (void)name;
    return false;
}

static void TestFilter(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    IoWriter w = strings_builder_as_io_writer(&b);

    AstFile *f = gat_filter_tree(a);
    fmt_fprintf_v(w, "exports: %s\n", gat_bool(ast_file_exports(f)));
    gat_describe(w, a, f);
    f = gat_filter_tree(a);
    fmt_fprintf_v(w, "filter: %s\n",
                  gat_bool(ast_filter_file(f, (AstFilter){gat_keep, NULL})));
    gat_describe(w, a, f);
    f = gat_filter_tree(a);
    bool kept = ast_filter_file(f, (AstFilter){gat_none, NULL});
    fmt_fprintf_v(w, "none: %s %d\n", gat_bool(kept), f->decls.len);
    gat_check(t, "filters", strings_builder_string(&b), gat_want_filter);
    arena_free(&ar);
}

/* ----------------------------------------------------------------- merging */

static AstFuncDecl *gat_merge_func(Alloc *a, Str recv, Str name, bool doc) {
    AstFuncDecl *d = gat_func(a, gat_id(a, name), gat_new(a, AST_KIND_FUNC_TYPE));
    if (recv.len > 0) {
        AstExpr type = &gat_id(a, strings_trim_prefix(recv, BURROW_S("*")))->node;
        if (strings_has_prefix(recv, BURROW_S("*"))) {
            AstStarExpr *star = gat_new(a, AST_KIND_STAR_EXPR);
            star->x = type;
            type = &star->node;
        }
        d->recv = gat_field_list(
            a, GAT_LIST(TYPE_AST_FIELD_PTR, gat_field(a, type, (Slice){0})));
    }
    if (doc) {
        Str text = fmt_sprintf_v(a, "// %s", name);
        d->doc = gat_group(a, GAT_LIST(TYPE_AST_COMMENT_PTR, gat_comment(a, 0, text)));
    }
    return d;
}

typedef struct GatMergeFile {
    Str name;
    Str docs[2];
    Int ndocs; /* -1 for no doc comment at all */
    TokenPos package, start, end;
    Str imports[2];
    Int nimports;
    Int ncomments;
} GatMergeFile;

static const GatMergeFile gat_merge_files[] = {
    {S_("b.go"),
     {S_("// B1"), S_("// B2")},
     2,
     300,
     290,
     400,
     {S_("\"fmt\""), S_("\"os\"")},
     2,
     2},
    {S_("a.go"), {S_("// A")}, 1, 100, 90, 200, {S_("\"os\""), S_("\"io\"")}, 2, 1},
    {S_("c.go"), {{NULL, 0}}, -1, 500, 490, 600, {{NULL, 0}}, 0, 0},
};

static AstFile *gat_merge_file(Alloc *a, const GatMergeFile *mf, Slice funcs) {
    AstFile *f = gat_new(a, AST_KIND_FILE);
    f->package = mf->package;
    f->name = ID("p");
    f->file_start = mf->start;
    f->file_end = mf->end;
    if (mf->ndocs >= 0) {
        f->doc = gat_group(a, slice_make(a, TYPE_AST_COMMENT_PTR, 0, 0));
        for (Int i = 0; i < mf->ndocs; i++) {
            AstComment *c = gat_comment(a, 0, mf->docs[i]);
            f->doc->list = slice_append(a, f->doc->list, &c, 1);
        }
    }
    f->imports = (Slice){NULL, 0, 0, TYPE_AST_IMPORT_SPEC_PTR};
    for (Int i = 0; i < mf->nimports; i++) {
        AstImportSpec *s = gat_new(a, AST_KIND_IMPORT_SPEC);
        s->path = gat_lit(a, TOKEN_STRING, mf->imports[i]);
        f->imports = slice_append(a, f->imports, &s, 1);
    }
    f->comments = (Slice){NULL, 0, 0, TYPE_AST_COMMENT_GROUP_PTR};
    for (Int i = 0; i < mf->ncomments; i++) {
        AstCommentGroup *g = gat_group(a, (Slice){0});
        f->comments = slice_append(a, f->comments, &g, 1);
    }
    f->decls = funcs;
    return f;
}

static void gat_merge_run(IoWriter w, Alloc *a, AstMergeMode mode) {
    Str none = BURROW_STR_EMPTY;
    Slice funcs[3] = {
        GAT_LIST(TYPE_AST_DECL, gat_merge_func(a, none, BURROW_S("F"), true),
                 gat_merge_func(a, BURROW_S("*T"), BURROW_S("M"), false),
                 gat_merge_func(a, none, BURROW_S("H"), false)),
        GAT_LIST(TYPE_AST_DECL, gat_merge_func(a, none, BURROW_S("F"), false),
                 gat_merge_func(a, BURROW_S("T"), BURROW_S("M"), true),
                 gat_merge_func(a, none, BURROW_S("G"), false),
                 gat_merge_func(a, none, BURROW_S("H"), false)),
        GAT_LIST(TYPE_AST_DECL, gat_merge_func(a, none, BURROW_S("F"), false)),
    };
    AstPackage *pkg = gat_new(a, AST_KIND_PACKAGE);
    pkg->name = BURROW_S("p");
    pkg->files = map_make(a, TYPE_STRING, TYPE_AST_FILE_PTR, 3);
    for (Int i = 0; i < NELEM(gat_merge_files); i++) {
        AstFile *f = gat_merge_file(a, &gat_merge_files[i], funcs[i]);
        if (!map_set(pkg->files, &gat_merge_files[i].name, &f)) {
            panic_str(BURROW_S("go_ast_test: out of memory"));
        }
    }
    AstFile *m = ast_merge_package_files(a, pkg, mode);
    fmt_fprintf_v(w, "merge %d: package %d start %d end %d name %s\n", mode, m->package,
                  m->file_start, m->file_end, m->name->name);
    fmt_fprintf_v(w, " ");
    for (Int i = 0; i < m->doc->list.len; i++) {
        fmt_fprintf_v(w, " %q", ((AstComment **)m->doc->list.p)[i]->text);
    }
    fmt_fprintf_v(w, "\n ");
    for (Int i = 0; i < m->decls.len; i++) {
        AstFuncDecl *f = ((AstFuncDecl **)m->decls.p)[i];
        fmt_fprintf_v(w, " %s%s%s", f->recv != NULL ? BURROW_S("(recv)") : none,
                      f->name->name, f->doc != NULL ? BURROW_S("+doc") : none);
    }
    fmt_fprintf_v(w, "\n ");
    for (Int i = 0; i < m->imports.len; i++) {
        fmt_fprintf_v(w, " %s", ((AstImportSpec **)m->imports.p)[i]->path->value);
    }
    fmt_fprintf_v(w, "\n  comments %d nil %s imports-nil %s\n", m->comments.len,
                  gat_bool(slice_is_nil(m->comments)),
                  gat_bool(slice_is_nil(m->imports)));
}

static void TestMergePackageFiles(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    IoWriter w = strings_builder_as_io_writer(&b);
    gat_merge_run(w, a, 0);
    gat_merge_run(w, a, AST_FILTER_FUNC_DUPLICATES | AST_FILTER_IMPORT_DUPLICATES);
    gat_merge_run(w, a, AST_FILTER_UNASSOCIATED_COMMENTS);
    gat_check(t, "MergePackageFiles", strings_builder_string(&b), gat_want_merge);
    arena_free(&ar);
}

/* ----------------------------------------------------------------- walking */

typedef struct GatSeq {
    IoWriter w;
    Alloc *a;
    Int n;
} GatSeq;

static void gat_seq_add(GatSeq *s, Str word) {
    if (s->n > 0) {
        fmt_fprintf_v(s->w, " ");
    }
    fmt_fprintf_v(s->w, "%s", word);
    s->n++;
}

static bool gat_inspect(void *env, AstNode n) {
    GatSeq *s = env;
    if (n == NULL) {
        gat_seq_add(s, BURROW_S(")"));
        return false;
    }
    gat_seq_add(s, ast_node_type(n)->name);
    return n->kind != AST_KIND_FIELD;
}

static bool gat_preorder(void *env, const void *v) {
    GatSeq *s = env;
    AstNode n = *(const AstNode *)v;
    if (n->kind == AST_KIND_IDENT) {
        gat_seq_add(s, ((AstIdent *)n)->name);
    }
    return s->n != 6;
}

static bool gat_preorder_stack(void *env, AstNode n, Slice stack) {
    GatSeq *s = env;
    if (n->kind == AST_KIND_IDENT) {
        gat_seq_add(s, fmt_sprintf_v(s->a, "%s/%d", ((AstIdent *)n)->name, stack.len));
    }
    return n->kind != AST_KIND_FUNC_DECL;
}

static void TestWalk(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    IoWriter w = strings_builder_as_io_writer(&b);
    AstFile *f = gat_filter_tree(a);

    GatSeq s = {w, a, 0};
    fmt_fprintf_v(w, "inspect: ");
    ast_inspect(&f->node, (AstInspectFunc){gat_inspect, &s});
    fmt_fprintf_v(w, "\npreorder: ");
    s.n = 0;
    IterSeq seq = ast_preorder(&f->node);
    seq.f(seq.env, (IterYield){gat_preorder, &s});
    fmt_fprintf_v(w, "\nstack: ");
    s.n = 0;
    ast_preorder_stack(a, &f->node, (Slice){NULL, 0, 0, TYPE_AST_NODE},
                       (AstPreorderStackFunc){gat_preorder_stack, &s});
    fmt_fprintf_v(w, "\n");
    gat_check(t, "walking", strings_builder_string(&b), gat_want_walk);
    arena_free(&ar);
}

/* ------------------------------------------------------------ comment maps */

typedef struct GatCmap {
    AstNode nodes[10];
} GatCmap;

static void gat_show(IoWriter w, AstCommentMap cmap, const GatCmap *c) {
    for (int i = 0; i < 10; i++) {
        AstNode n = c->nodes[i];
        Slice *list = map_get(cmap, &n);
        if (list == NULL || list->len == 0) {
            continue;
        }
        fmt_fprintf_v(w, "  *ast.%s", ast_node_type(n)->name);
        for (Int j = 0; j < list->len; j++) {
            fmt_fprintf_v(w, " %q", gat_text(((AstCommentGroup **)list->p)[j]));
        }
        fmt_fprintf_v(w, "\n");
    }
}

static AstCommentGroup *gat_cm_group(Alloc *a, GatCursor *c, Str text) {
    AstComment *k = gat_comment(a, gat_next(c, text), text);
    return gat_group(a, GAT_LIST(TYPE_AST_COMMENT_PTR, k));
}

static AstFuncDecl *gat_cm_func(Alloc *a, GatCursor *c, Str name) {
    AstFuncType *type = gat_new(a, AST_KIND_FUNC_TYPE);
    type->func = gat_next(c, BURROW_S("func"));
    AstFuncDecl *d = gat_func(a, gat_id(a, name), type);
    d->name->name_pos = gat_next(c, name);
    type->params = gat_field_list(a, (Slice){0});
    type->params->opening = gat_next(c, BURROW_S("("));
    type->params->closing = gat_next(c, BURROW_S(")"));
    d->body = gat_new(a, AST_KIND_BLOCK_STMT);
    d->body->lbrace = gat_next(c, BURROW_S("{"));
    return d;
}

static void TestCommentMap(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder b = STRINGS_BUILDER(a);
    IoWriter w = strings_builder_as_io_writer(&b);

    Str src =
        BURROW_S("package p\n\n// doc F\nfunc F() {} // after F\n\n// floating\n\n"
                 "func G() {\n\t// in G\n}\n// end\n");
    TokenFileSet *fset = token_new_file_set(a);
    TokenFile *tf = token_file_set_add_file(fset, BURROW_S("c.go"), -1, src.len);
    token_file_set_lines_for_content(tf, slice_from_str(a, src));
    GatCursor c = {src, 0, tf};
    AstFile *file = gat_new(a, AST_KIND_FILE);
    file->package = gat_next(&c, BURROW_S("package"));
    file->name = ID("p");
    file->name->name_pos = gat_next(&c, BURROW_S("p"));
    file->file_start = token_file_pos(tf, 0);
    file->file_end = token_file_pos(tf, src.len);
    AstCommentGroup *doc_f = gat_cm_group(a, &c, BURROW_S("// doc F"));
    AstFuncDecl *fF = gat_cm_func(a, &c, BURROW_S("F"));
    fF->doc = doc_f;
    fF->body->rbrace = gat_next(&c, BURROW_S("}"));
    AstCommentGroup *after = gat_cm_group(a, &c, BURROW_S("// after F"));
    AstCommentGroup *floating = gat_cm_group(a, &c, BURROW_S("// floating"));
    AstFuncDecl *fG = gat_cm_func(a, &c, BURROW_S("G"));
    AstCommentGroup *in_g = gat_cm_group(a, &c, BURROW_S("// in G"));
    fG->body->rbrace = gat_next(&c, BURROW_S("}"));
    AstCommentGroup *end = gat_cm_group(a, &c, BURROW_S("// end"));
    file->decls = GAT_LIST(TYPE_AST_DECL, fF, fG);
    file->comments =
        GAT_LIST(TYPE_AST_COMMENT_GROUP_PTR, doc_f, after, floating, in_g, end);
    GatCmap nodes = {{&file->node, &file->name->node, &fF->node, &fF->name->node,
                      &fF->type->node, &fF->body->node, &fG->node, &fG->name->node,
                      &fG->type->node, &fG->body->node}};

    AstCommentMap cmap = ast_new_comment_map(a, fset, &file->node, file->comments);
    fmt_fprintf_v(w, "cmap: %d\n", map_len(cmap));
    gat_show(w, cmap, &nodes);
    fmt_fprintf_v(w, "filter G: %d\n",
                  map_len(ast_comment_map_filter(cmap, a, &fG->node)));
    Slice cs = ast_comment_map_comments(cmap, a);
    fmt_fprintf_v(w, "comments: %d", cs.len);
    for (Int i = 0; i < cs.len; i++) {
        fmt_fprintf_v(w, " %q", gat_text(((AstCommentGroup **)cs.p)[i]));
    }
    fmt_fprintf_v(w, "\n");
    ast_comment_map_update(cmap, a, &fG->node, &fF->node);
    fmt_fprintf_v(w, "update: %d\n", map_len(cmap));
    gat_show(w, cmap, &nodes);
    AstCommentMap empty = ast_new_comment_map(a, fset, &file->node, (Slice){0});
    fmt_fprintf_v(w, "nil: %s\n", gat_bool(empty == NULL));
    gat_check(t, "CommentMap", strings_builder_string(&b), gat_want_cmap);
    arena_free(&ar);
}

/* ------------------------------------------------------------------ scopes */

static void TestScope(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const Str kinds[] = {S_("bad"), S_("package"), S_("const"), S_("type"),
                                S_("var"), S_("func"),    S_("label")};
    for (AstObjKind k = AST_BAD; k <= AST_LBL; k++) {
        CHECK(str_eq(ast_obj_kind_string(k), kinds[k]));
    }

    AstScope *outer = ast_new_scope(a, NULL);
    AstScope *s = ast_new_scope(a, outer);
    CHECK(s != NULL && s->outer == outer);
    Str empty = ast_scope_string(s, a);
    CHECK(strings_has_prefix(empty, BURROW_S("scope 0x")));
    CHECK(strings_has_suffix(empty, BURROW_S(" {}\n")));

    AstObject *x = ast_new_obj(a, AST_VAR, BURROW_S("x"));
    AstObject *x2 = ast_new_obj(a, AST_CON, BURROW_S("x"));
    CHECK(ast_scope_insert(s, x) == NULL);
    CHECK(ast_scope_insert(s, x2) == x);
    CHECK(ast_scope_lookup(s, BURROW_S("x")) == x);
    /* Lookup does not look in the outer scope. */
    AstObject *y = ast_new_obj(a, AST_FUN, BURROW_S("y"));
    CHECK(ast_scope_insert(outer, y) == NULL);
    CHECK(ast_scope_lookup(s, BURROW_S("y")) == NULL);
    CHECK(ast_scope_lookup(outer, BURROW_S("y")) == y);
    Str one = ast_scope_string(s, a);
    CHECK(strings_has_suffix(one, BURROW_S(" {\n\tvar x\n}\n")));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestCommentText)                                                                 \
    X(TestIsDirective)                                                                 \
    X(TestParseDirectiveMatchesIsDirective)                                            \
    X(TestParseDirective)                                                              \
    X(TestParseArgs)                                                                   \
    X(TestPrint)                                                                       \
    X(TestFprintTree)                                                                  \
    X(TestSortImports)                                                                 \
    X(TestFilter)                                                                      \
    X(TestMergePackageFiles)                                                           \
    X(TestWalk)                                                                        \
    X(TestCommentMap)                                                                  \
    X(TestScope)

TESTING_MAIN(TESTS)
