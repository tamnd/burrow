/* Derived from Go's src/go/parser/parser_test.go.
 * Go source: go1.27.1.
 *
 * Go's tests parse the package's own files and the ones in testdata, in the
 * directory the test runs in. Here they come from tests/go_parser_test_gen.h,
 * and the tests that read a directory get a temporary one with the files
 * written out. TestParseDir's has every .go file of src/go/parser in it, and a
 * parser.go.orig too, which its filter lets through and which ParseDir has to
 * leave out.
 *
 * Go's stack grows and a C stack does not, so TestParseDepthLimit and
 * TestScopeDepthLimit parse on a goroutine with a stack big enough for the
 * nesting they test. Like Go, TestParseDepthLimit is skipped with
 * -test.short, since it needs a good deal of memory.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/go/parser.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/os.h"
#include "burrow/path/filepath.h"
#include "burrow/proc.h"
#include "burrow/strings.h"
#include "burrow/sync.h"

#include <stdint.h>
#include <string.h>

#define GPT_WANT_SOURCES
#define GPT_WANT_TESTDATA
#define GPT_WANT_SHORT
/* The files are cut into pieces of up to 30000 bytes, longer than the 4095
 * bytes C99 promises a string literal can hold. Every compiler burrow builds
 * with takes them. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "go_parser_test_gen.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* The guillemets the depth test formats use, as UTF-8. */
#define LQ "\xc2\xab"
#define RQ "\xc2\xbb"

/* parser.go's maxNestLev and resolver.go's maxScopeDepth. */
enum { MAX_NEST_LEV = 100000, MAX_SCOPE_DEPTH = 1000 };

static Any nil_src(void) {
    return (Any){NULL, NULL};
}

static Any str_src(Str *s) {
    return BURROW_ANY(TYPE_STRING, s);
}

static Str cstr(const char *s) {
    return str_from_bytes(s, (Int)strlen(s));
}

/* The bytes of a generated file, its pieces joined up. */
static Str file_bytes(Alloc *a, const GptFile *f) {
    if (f->npieces == 1)
        return str_from_bytes(f->pieces[0].p, (Int)f->pieces[0].n);
    Int n = 0;
    for (int i = 0; i < f->npieces; i++)
        n += (Int)f->pieces[i].n;
    char *buf = (char *)mem_alloc(a, (size_t)n, 1);
    if (buf == NULL)
        panic_str(S("out of memory"));
    Int off = 0;
    for (int i = 0; i < f->npieces; i++) {
        memcpy(buf + off, f->pieces[i].p, (size_t)f->pieces[i].n);
        off += (Int)f->pieces[i].n;
    }
    return str_from_bytes(buf, n);
}

static const GptFile *find_source(const char *name) {
    for (Int i = 0; i < NELEM(gpt_sources); i++)
        if (strcmp(gpt_sources[i].name, name) == 0)
            return &gpt_sources[i];
    return NULL;
}

static void remove_tree(void *env) {
    Str *d = (Str *)env;
    (void)os_remove_all(*d);
    Alloc *h = heap_allocator();
    mem_free(h, (void *)(uintptr_t)d->p, (size_t)d->len, 1);
    mem_free(h, d, sizeof *d, _Alignof(Str));
}

/* t.TempDir: a new directory, removed when the test ends. The name lives on
 * the heap, since the cleanup runs after the test has freed its arena. The
 * copy returned is in a. */
static Str temp_dir(TestingT *t, Alloc *a) {
    Error e = BURROW_NO_ERROR;
    Alloc *h = heap_allocator();
    Str dir = os_mkdir_temp(a, S(""), S("burrow-go-parser-test-*"), &e);
    if (!BURROW_OK(e))
        testing_t_fatalf_v(t, "MkdirTemp: %s", error_text(e));
    Str *d = BURROW_NEW(h, Str);
    char *p = d != NULL ? (char *)mem_alloc(h, (size_t)dir.len, 1) : NULL;
    if (p == NULL) {
        (void)os_remove_all(dir);
        if (d != NULL)
            mem_free(h, d, sizeof *d, _Alignof(Str));
        testing_t_fatalf_v(t, "out of memory");
        return dir;
    }
    memcpy(p, dir.p, (size_t)dir.len);
    *d = str_from_bytes(p, dir.len);
    testing_t_cleanup(t, BURROW_FN(Func, remove_tree, d));
    return dir;
}

/* Writes data to dir/rel, where rel is slash separated, making the
 * directories on the way. */
static void write_file(TestingT *t, Alloc *a, Str dir, Str rel, Str data) {
    Str path = filepath_join_v(a, 2, dir, filepath_from_slash(a, rel));
    Error e = os_mkdir_all(filepath_dir(a, path), 0755);
    if (BURROW_OK(e))
        e = os_write_file(
            path, slice_from((void *)(uintptr_t)data.p, data.len, data.len, TYPE_BYTE),
            0644);
    if (!BURROW_OK(e))
        testing_t_fatalf_v(t, "writing %s: %s", path, error_text(e));
}

/* A directory with the files of src/go/parser, and parser.go.orig. */
static Str source_dir(TestingT *t, Alloc *a) {
    Str dir = temp_dir(t, a);
    for (Int i = 0; i < NELEM(gpt_sources); i++)
        write_file(t, a, dir, cstr(gpt_sources[i].name),
                   file_bytes(a, &gpt_sources[i]));
    write_file(t, a, dir, S("parser.go.orig"), file_bytes(a, find_source("parser.go")));
    return dir;
}

/* A directory with the files of src/go/parser/testdata. */
static Str testdata_dir(TestingT *t, Alloc *a) {
    Str dir = temp_dir(t, a);
    for (Int i = 0; i < NELEM(gpt_testdata); i++)
        write_file(t, a, dir, cstr(gpt_testdata[i].name),
                   file_bytes(a, &gpt_testdata[i]));
    return dir;
}

static const char *const valid_files[] = {
    "parser.go",
    "parser_test.go",
    "error_test.go",
    "short_test.go",
};

static void TestParse(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str dir = source_dir(t, a);
    for (Int i = 0; i < NELEM(valid_files); i++) {
        Str filename = filepath_join_v(a, 2, dir, cstr(valid_files[i]));
        Error err = BURROW_NO_ERROR;
        (void)parser_parse_file(a, token_new_file_set(a), filename, nil_src(),
                                PARSER_DECLARATION_ERRORS, &err);
        if (!BURROW_OK(err))
            testing_t_fatalf_v(t, "ParseFile(%s): %s", filename, error_text(err));
    }
    arena_free(&ar);
}

static bool name_filter(Str filename) {
    if (str_eq(filename, S("parser.go")) || str_eq(filename, S("interface.go")) ||
        str_eq(filename, S("parser_test.go")))
        return true;
    if (str_eq(filename, S("parser.go.orig")))
        return true; /* permit but should be ignored by ParseDir */
    return false;
}

static bool dir_filter(void *env, FsFileInfo f) {
    (void)env;
    return name_filter(f.vt->name(f.data));
}

static void TestParseFile(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src =
        S("package p\nvar _=s[::]+\ns[::]+\ns[::]+\ns[::]+\ns[::]+\ns[::]+\ns[::]+"
          "\ns[::]+\ns[::]+\ns[::]+\ns[::]+\ns[::]");
    Error err = BURROW_NO_ERROR;
    (void)parser_parse_file(a, token_new_file_set(a), BURROW_STR_EMPTY, str_src(&src),
                            0, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "ParseFile(%s) succeeded unexpectedly", src);
    arena_free(&ar);
}

static void TestParseExprFrom(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("s[::]+\ns[::]+\ns[::]+\ns[::]+\ns[::]+\ns[::]+\ns[::]+\ns[::]+\ns[::]+"
                "\ns[::]+\ns[::]+\ns[::]");
    Error err = BURROW_NO_ERROR;
    (void)parser_parse_expr_from(a, token_new_file_set(a), BURROW_STR_EMPTY,
                                 str_src(&src), 0, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "ParseExprFrom(%s) succeeded unexpectedly", src);
    arena_free(&ar);
}

static void TestParseDir(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str path = source_dir(t, a);
    Error err = BURROW_NO_ERROR;
    Map *pkgs =
        parser_parse_dir(a, token_new_file_set(a), path,
                         BURROW_FN(ParserFileFilter, dir_filter, NULL), 0, &err);
    if (!BURROW_OK(err))
        testing_t_fatalf_v(t, "ParseDir(%s): %s", path, error_text(err));
    Int n = map_len(pkgs);
    if (n != 1)
        testing_t_errorf_v(t, "got %d packages; want 1", n);
    Str name = S("parser");
    AstPackage **pkg = (AstPackage **)map_get(pkgs, &name);
    if (pkg == NULL || *pkg == NULL) {
        testing_t_errorf_v(t, "package \"parser\" not found");
        arena_free(&ar);
        return;
    }
    n = map_len((*pkg)->files);
    if (n != 3)
        testing_t_errorf_v(t, "got %d package files; want 3", n);
    MapIter it = map_iter((*pkg)->files);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v)) {
        /* Go's are relative to ".", and these to the temporary directory. */
        Str filename = *(const Str *)k;
        if (!name_filter(filepath_base(filename)))
            testing_t_errorf_v(t, "unexpected package file: %s", filename);
    }
    arena_free(&ar);
}

static void TestIssue42951(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str path = filepath_join_v(a, 2, testdata_dir(t, a), S("issue42951"));
    Error err = BURROW_NO_ERROR;
    (void)parser_parse_dir(a, token_new_file_set(a), path,
                           (ParserFileFilter){NULL, NULL}, 0, &err);
    if (!BURROW_OK(err))
        testing_t_errorf_v(t, "ParseDir(%s): %s", path, error_text(err));
    arena_free(&ar);
}

static void TestParseExpr(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    /* just kicking the tires: */
    /* a valid arithmetic expression */
    Str src = S("a + b");
    AstExpr x = parser_parse_expr(a, src, &err);
    if (!BURROW_OK(err))
        testing_t_errorf_v(t, "ParseExpr(%q): %s", src, error_text(err));
    /* sanity check */
    if (x == NULL || x->kind != AST_KIND_BINARY_EXPR)
        testing_t_errorf_v(t, "ParseExpr(%q): got %p, want *ast.BinaryExpr", src,
                           (void *)x);

    /* a valid type expression */
    src = S("struct{x *int}");
    x = parser_parse_expr(a, src, &err);
    if (!BURROW_OK(err))
        testing_t_errorf_v(t, "ParseExpr(%q): %s", src, error_text(err));
    /* sanity check */
    if (x == NULL || x->kind != AST_KIND_STRUCT_TYPE)
        testing_t_errorf_v(t, "ParseExpr(%q): got %p, want *ast.StructType", src,
                           (void *)x);

    /* an invalid expression */
    src = S("a + *");
    x = parser_parse_expr(a, src, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "ParseExpr(%q): got no error", src);
    if (x == NULL)
        testing_t_errorf_v(t, "ParseExpr(%q): got no (partial) result", src);
    if (x == NULL || x->kind != AST_KIND_BINARY_EXPR)
        testing_t_errorf_v(t, "ParseExpr(%q): got %p, want *ast.BinaryExpr", src,
                           (void *)x);

    /* a valid expression followed by extra tokens is invalid */
    src = S("a[i] := x");
    (void)parser_parse_expr(a, src, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "ParseExpr(%q): got no error", src);

    /* a semicolon is not permitted unless automatically inserted */
    src = S("a + b\n");
    (void)parser_parse_expr(a, src, &err);
    if (!BURROW_OK(err))
        testing_t_errorf_v(t, "ParseExpr(%q): got error %s", src, error_text(err));
    src = S("a + b;");
    (void)parser_parse_expr(a, src, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "ParseExpr(%q): got no error", src);

    /* various other stuff following a valid expression */
    static const char followers[] = "!)]};,";
    for (Int i = 0; followers[i] != 0; i++) {
        src = fmt_sprintf_v(a, "a + b%cdh3*#D)#_", (int32_t)followers[i]);
        (void)parser_parse_expr(a, src, &err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "ParseExpr(%q): got no error", src);
    }

    /* ParseExpr must not crash */
    for (Int i = 0; i < NELEM(gpt_valids); i++)
        (void)parser_parse_expr(
            a, str_from_bytes(gpt_valids[i].p, (Int)gpt_valids[i].n), &err);
    arena_free(&ar);
}

static AstFuncDecl *first_func(AstFile *f) {
    return (AstFuncDecl *)BURROW_AT(AstDecl, f->decls, 0);
}

static void TestColonEqualsScope(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package p; func f() { x, y, z := x, y, z }");
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, token_new_file_set(a), BURROW_STR_EMPTY,
                                   str_src(&src), 0, &err);
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, error_text(err));

    /* RHS refers to undefined globals; LHS does not. */
    AstAssignStmt *as =
        (AstAssignStmt *)BURROW_AT(AstStmt, first_func(f)->body->list, 0);
    for (Int i = 0; i < as->rhs.len; i++) {
        AstIdent *id = (AstIdent *)BURROW_AT(AstExpr, as->rhs, i);
        if (id->obj != NULL)
            testing_t_errorf_v(t, "rhs %s has Obj, should not", id->name);
    }
    for (Int i = 0; i < as->lhs.len; i++) {
        AstIdent *id = (AstIdent *)BURROW_AT(AstExpr, as->lhs, i);
        if (id->obj == NULL)
            testing_t_errorf_v(t, "lhs %s does not have Obj, should", id->name);
    }
    arena_free(&ar);
}

static void TestVarScope(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package p; func f() { var x, y, z = x, y, z }");
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, token_new_file_set(a), BURROW_STR_EMPTY,
                                   str_src(&src), 0, &err);
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, error_text(err));

    /* RHS refers to undefined globals; LHS does not. */
    AstDeclStmt *ds = (AstDeclStmt *)BURROW_AT(AstStmt, first_func(f)->body->list, 0);
    AstGenDecl *gd = (AstGenDecl *)ds->decl;
    AstValueSpec *as = (AstValueSpec *)BURROW_AT(AstSpec, gd->specs, 0);
    for (Int i = 0; i < as->values.len; i++) {
        AstIdent *id = (AstIdent *)BURROW_AT(AstExpr, as->values, i);
        if (id->obj != NULL)
            testing_t_errorf_v(t, "rhs %s has Obj, should not", id->name);
    }
    for (Int i = 0; i < as->names.len; i++) {
        AstIdent *id = BURROW_AT(AstIdent *, as->names, i);
        if (id->obj == NULL)
            testing_t_errorf_v(t, "lhs %s does not have Obj, should", id->name);
    }
    arena_free(&ar);
}

typedef struct ObjWant {
    const char *name;
    AstObjKind kind;
} ObjWant;

static const ObjWant objects_want[] = {
    {"p", AST_BAD},   /* not in a scope */
    {"fmt", AST_BAD}, /* not resolved yet */
    {"pi", AST_CON},  {"T", AST_TYP},
    {"x", AST_VAR},   {"int", AST_BAD}, /* not resolved yet */
    {"f", AST_FUN},   {"L", AST_LBL},
};

/* objects[name], with Go's zero value, Bad, for a name not there. */
static AstObjKind object_kind(Str name) {
    for (Int i = 0; i < NELEM(objects_want); i++)
        if (str_eq(cstr(objects_want[i].name), name))
            return objects_want[i].kind;
    return AST_BAD;
}

static bool check_object(void *env, AstNode n) {
    TestingT *t = (TestingT *)env;
    if (n == NULL || n->kind != AST_KIND_IDENT)
        return true;
    AstIdent *ident = (AstIdent *)n;
    AstObject *obj = ident->obj;
    if (obj == NULL) {
        if (object_kind(ident->name) != AST_BAD)
            testing_t_errorf_v(t, "no object for %s", ident->name);
        return true;
    }
    if (!str_eq(obj->name, ident->name))
        testing_t_errorf_v(t, "names don't match: obj.Name = %s, ident.Name = %s",
                           obj->name, ident->name);
    AstObjKind kind = object_kind(ident->name);
    if (obj->kind != kind)
        testing_t_errorf_v(t, "%s: obj.Kind = %s; want %s", ident->name,
                           ast_obj_kind_string(obj->kind), ast_obj_kind_string(kind));
    return true;
}

static void TestObjects(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("\npackage p\nimport fmt \"fmt\"\nconst pi = 3.14\ntype T struct{}\n"
                "var x int\nfunc f() { L: }\n");
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, token_new_file_set(a), BURROW_STR_EMPTY,
                                   str_src(&src), 0, &err);
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, error_text(err));
    ast_inspect(&f->node, BURROW_FN(AstInspectFunc, check_object, t));
    arena_free(&ar);
}

static void TestUnresolved(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("\npackage p\n//\nfunc f1a(int)\nfunc f2a(byte, int, float)\n"
                "func f3a(a, b int, c float)\nfunc f4a(...complex)\n"
                "func f5a(a s1a, b ...complex)\n//\nfunc f1b(*int)\n"
                "func f2b([]byte, (int), *float)\nfunc f3b(a, b *int, c []float)\n"
                "func f4b(...*complex)\nfunc f5b(a s1a, b ...[]complex)\n//\n"
                "type s1a struct { int }\ntype s2a struct { byte; int; s1a }\n"
                "type s3a struct { a, b int; c float }\n//\ntype s1b struct { *int }\n"
                "type s2b struct { byte; int; *float }\n"
                "type s3b struct { a, b *s3b; c []float }\n");
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, token_new_file_set(a), BURROW_STR_EMPTY,
                                   str_src(&src), 0, &err);
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, error_text(err));

    Str want = S("int "            /* f1a */
                 "byte int float " /* f2a */
                 "int float "      /* f3a */
                 "complex "        /* f4a */
                 "complex "        /* f5a */
                 "int "            /* f1b */
                 "byte int float " /* f2b */
                 "int float "      /* f3b */
                 "complex "        /* f4b */
                 "complex "        /* f5b */
                 "int "            /* s1a */
                 "byte int "       /* s2a */
                 "int float "      /* s3a */
                 "int "            /* s1a */
                 "byte int float " /* s2a */
                 "float ");        /* s3a */

    /* collect unresolved identifiers */
    StringsBuilder buf = STRINGS_BUILDER(a);
    for (Int i = 0; i < f->unresolved.len; i++) {
        AstIdent *u = BURROW_AT(AstIdent *, f->unresolved, i);
        (void)strings_builder_write_string(&buf, u->name, NULL);
        (void)strings_builder_write_byte(&buf, ' ');
    }
    Str got = strings_builder_string(&buf);

    if (!str_eq(got, want))
        testing_t_errorf_v(t, "\ngot:  %s\nwant: %s", got, want);
    arena_free(&ar);
}

static Str comment_text_at(AstCommentGroup *g, Int j) {
    if (g == NULL || j >= g->list.len)
        return BURROW_STR_EMPTY;
    return BURROW_AT(AstComment *, g->list, j)->text;
}

static void TestCommentGroups(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src =
        S("\npackage p /* 1a */ /* 1b */      /* 1c */ // 1d\n/* 2a\n*/\n// 2b\n"
          "const pi = 3.1415\n/* 3a */ // 3b\n/* 3c */ const e = 2.7182\n\n"
          "// Example from go.dev/issue/3139\nfunc ExampleCount() {\n"
          "\tfmt.Println(strings.Count(\"cheese\", \"e\"))\n"
          "\tfmt.Println(strings.Count(\"five\", \"\")) // before & after each rune\n"
          "\t// Output:\n\t// 3\n\t// 5\n}\n");
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, token_new_file_set(a), BURROW_STR_EMPTY,
                                   str_src(&src), PARSER_PARSE_COMMENTS, &err);
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, error_text(err));
    static const char *const expected[][4] = {
        {"/* 1a */", "/* 1b */", "/* 1c */", "// 1d"},
        {"/* 2a\n*/", "// 2b", NULL, NULL},
        {"/* 3a */", "// 3b", "/* 3c */", NULL},
        {"// Example from go.dev/issue/3139", NULL, NULL, NULL},
        {"// before & after each rune", NULL, NULL, NULL},
        {"// Output:", "// 3", "// 5", NULL},
    };
    if (f->comments.len != NELEM(expected))
        testing_t_fatalf_v(t, "got %d comment groups; expected %d", f->comments.len,
                           NELEM(expected));
    for (Int i = 0; i < NELEM(expected); i++) {
        AstCommentGroup *g = BURROW_AT(AstCommentGroup *, f->comments, i);
        Int n = 0;
        while (n < 4 && expected[i][n] != NULL)
            n++;
        if (g->list.len != n) {
            testing_t_errorf_v(t, "got %d comments in group %d; expected %d",
                               g->list.len, i, n);
            continue;
        }
        for (Int j = 0; j < n; j++) {
            Str got = comment_text_at(g, j);
            Str exp = cstr(expected[i][j]);
            if (!str_eq(got, exp))
                testing_t_errorf_v(t, "got %q in group %d; expected %q", got, i, exp);
        }
    }
    arena_free(&ar);
}

static AstField *get_field(AstFile *file, Str type_name, Str field_name) {
    for (Int i = 0; i < file->decls.len; i++) {
        AstDecl d = BURROW_AT(AstDecl, file->decls, i);
        if (d->kind != AST_KIND_GEN_DECL || ((AstGenDecl *)d)->tok != TOKEN_TYPE_)
            continue;
        AstGenDecl *gd = (AstGenDecl *)d;
        for (Int j = 0; j < gd->specs.len; j++) {
            AstSpec s = BURROW_AT(AstSpec, gd->specs, j);
            if (s->kind != AST_KIND_TYPE_SPEC)
                continue;
            AstTypeSpec *ts = (AstTypeSpec *)s;
            if (!str_eq(ts->name->name, type_name) ||
                ts->type->kind != AST_KIND_STRUCT_TYPE)
                continue;
            AstFieldList *fields = ((AstStructType *)ts->type)->fields;
            for (Int k = 0; k < fields->list.len; k++) {
                AstField *f = BURROW_AT(AstField *, fields->list, k);
                for (Int m = 0; m < f->names.len; m++)
                    if (str_eq(BURROW_AT(AstIdent *, f->names, m)->name, field_name))
                        return f;
            }
        }
    }
    return NULL;
}

/* Don't use ast_comment_group_text - we want to see exact comment text. */
static Str comment_text(Alloc *a, AstCommentGroup *c) {
    StringsBuilder buf = STRINGS_BUILDER(a);
    if (c != NULL)
        for (Int i = 0; i < c->list.len; i++)
            (void)strings_builder_write_string(&buf, comment_text_at(c, i), NULL);
    return strings_builder_string(&buf);
}

static void check_field_comments(TestingT *t, Alloc *a, AstFile *file, Str type_name,
                                 Str field_name, Str lead, Str line) {
    AstField *f = get_field(file, type_name, field_name);
    if (f == NULL) {
        testing_t_fatalf_v(t, "field not found: %s.%s", type_name, field_name);
        return;
    }
    Str got = comment_text(a, f->doc);
    if (!str_eq(got, lead))
        testing_t_errorf_v(t, "got lead comment %q; expected %q", got, lead);
    got = comment_text(a, f->comment);
    if (!str_eq(got, line))
        testing_t_errorf_v(t, "got line comment %q; expected %q", got, line);
}

static void TestLeadAndLineComments(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src =
        S("\npackage p\ntype T struct {\n\t/* F1 lead comment */\n\t//\n"
          "\tF1 int  /* F1 */ // line comment\n\t// F2 lead\n\t// comment\n"
          "\tF2 int  // F2 line comment\n\t// f3 lead comment\n"
          "\tf3 int  // f3 line comment\n\n\tf4 int   /* not a line comment */ ;\n"
          "        f5 int ; // f5 line comment\n"
          "\tf6 int ; /* f6 line comment */\n\tf7 int ; /*f7a*/ /*f7b*/ //f7c\n}\n");
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, token_new_file_set(a), BURROW_STR_EMPTY,
                                   str_src(&src), PARSER_PARSE_COMMENTS, &err);
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, error_text(err));
    check_field_comments(t, a, f, S("T"), S("F1"), S("/* F1 lead comment *///"),
                         S("/* F1 */// line comment"));
    check_field_comments(t, a, f, S("T"), S("F2"), S("// F2 lead// comment"),
                         S("// F2 line comment"));
    check_field_comments(t, a, f, S("T"), S("f3"), S("// f3 lead comment"),
                         S("// f3 line comment"));
    check_field_comments(t, a, f, S("T"), S("f4"), S(""), S(""));
    check_field_comments(t, a, f, S("T"), S("f5"), S(""), S("// f5 line comment"));
    check_field_comments(t, a, f, S("T"), S("f6"), S(""), S("/* f6 line comment */"));
    check_field_comments(t, a, f, S("T"), S("f7"), S(""), S("/*f7a*//*f7b*///f7c"));

    (void)ast_file_exports(f);
    check_field_comments(t, a, f, S("T"), S("F1"), S("/* F1 lead comment *///"),
                         S("/* F1 */// line comment"));
    check_field_comments(t, a, f, S("T"), S("F2"), S("// F2 lead// comment"),
                         S("// F2 line comment"));
    if (get_field(f, S("T"), S("f3")) != NULL)
        testing_t_error_v(t, "not expected to find T.f3");
    arena_free(&ar);
}

typedef struct Issue9979 {
    TestingT *t;
    TokenFileSet *fset;
    Str src;
    TokenPos pos, end;
} Issue9979;

static bool check_issue9979(void *env, AstNode x) {
    Issue9979 *c = (Issue9979 *)env;
    if (x == NULL)
        return true;
    switch ((int)x->kind) {
    case AST_KIND_BLOCK_STMT:
        c->pos = ast_node_pos(x) + 1; /* exclude "{", "}" */
        c->end = ast_node_end(x) - 1;
        break;
    case AST_KIND_LABELED_STMT:
        c->pos = ast_node_pos(x) + 2; /* exclude "L:" */
        c->end = ast_node_end(x);
        break;
    case AST_KIND_EMPTY_STMT: {
        AstEmptyStmt *s = (AstEmptyStmt *)x;
        TokenPos spos = ast_node_pos(x), send = ast_node_end(x);
        /* check containment */
        if (spos < c->pos || send > c->end)
            testing_t_errorf_v(c->t, "%s: *ast.EmptyStmt[%d, %d] not inside [%d, %d]",
                               c->src, spos, send, c->pos, c->end);
        /* check semicolon */
        Int offs = token_file_set_position(c->fset, spos).offset;
        char ch = (char)c->src.p[offs];
        if ((ch != ';') != s->implicit) {
            Str want = S("want ';'");
            if (s->implicit)
                want = S("but ';' is implicit");
            testing_t_errorf_v(c->t, "%s: found %q at offset %d; %s", c->src,
                               (int32_t)ch, offs, want);
        }
        break;
    }
    default:
        break;
    }
    return true;
}

/* TestIssue9979 verifies that empty statements are contained within their
 * enclosing blocks. */
static void TestIssue9979(TestingT *t) {
    static const char *const srcs[] = {
        "package p; func f() {;}",      "package p; func f() {L:}",
        "package p; func f() {L:;}",    "package p; func f() {L:\n}",
        "package p; func f() {L:\n;}",  "package p; func f() { ; }",
        "package p; func f() { L: }",   "package p; func f() { L: ; }",
        "package p; func f() { L: \n}", "package p; func f() { L: \n; }",
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(srcs); i++) {
        Str src = cstr(srcs[i]);
        TokenFileSet *fset = token_new_file_set(a);
        Error err = BURROW_NO_ERROR;
        AstFile *f =
            parser_parse_file(a, fset, BURROW_STR_EMPTY, str_src(&src), 0, &err);
        if (!BURROW_OK(err))
            testing_t_fatal_v(t, error_text(err));

        Issue9979 c = {t, fset, src, 0, 0};
        ast_inspect(&f->node, BURROW_FN(AstInspectFunc, check_issue9979, &c));
    }
    arena_free(&ar);
}

static void TestFileStartEndPos(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src =
        S("// Copyright\n\n//+build tag\n\n// Package p doc comment.\npackage p\n\n"
          "var lastDecl int\n\n/* end of file */\n");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, fset, S("file.go"), str_src(&src), 0, &err);
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, error_text(err));

    /* File{Start,End} spans the entire file, not just the declarations. */
    Str got = token_position_string(token_file_set_position(fset, f->file_start), a);
    if (!str_eq(got, S("file.go:1:1")))
        testing_t_errorf_v(t, "for File.FileStart, got %s, want %s", got,
                           S("file.go:1:1"));
    /* The end position is the newline at the end of the / * end of file * /
     * line. */
    got = token_position_string(token_file_set_position(fset, f->file_end), a);
    if (!str_eq(got, S("file.go:10:19")))
        testing_t_errorf_v(t, "for File.FileEnd, got %s, want %s", got,
                           S("file.go:10:19"));
    arena_free(&ar);
}

static bool find_selector(void *env, AstNode n) {
    if (n != NULL && n->kind == AST_KIND_SELECTOR_EXPR)
        *(AstSelectorExpr **)env = (AstSelectorExpr *)n;
    return true;
}

/* fmt.Sprint of a SelectorExpr whose X is an identifier, the only kind this
 * test meets: &{X Sel}, with each Ident printed as its name. */
static Str sprint_selector(Alloc *a, AstSelectorExpr *sel) {
    Str x = sel->x != NULL && sel->x->kind == AST_KIND_IDENT
                ? ((AstIdent *)sel->x)->name
                : S("?");
    return fmt_sprintf_v(a, "&{%s %s}", x,
                         sel->sel != NULL ? sel->sel->name : S("<nil>"));
}

/* TestIncompleteSelection ensures that an incomplete selector expression is
 * parsed as a (blank) *ast.SelectorExpr, not a *ast.BadExpr. */
static void TestIncompleteSelection(TestingT *t) {
    static const char *const srcs[] = {
        "package p; var _ = fmt.",             /* at EOF */
        "package p; var _ = fmt.\ntype X int", /* not at EOF */
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(srcs); i++) {
        Str src = cstr(srcs[i]);
        Error err = BURROW_NO_ERROR;
        AstFile *f = parser_parse_file(a, token_new_file_set(a), BURROW_STR_EMPTY,
                                       str_src(&src), 0, &err);
        if (BURROW_OK(err)) {
            testing_t_errorf_v(t, "ParseFile(%s) succeeded unexpectedly", src);
            continue;
        }

        Str want_err = S("expected selector or type assertion");
        if (!strings_contains(error_text(err), want_err))
            testing_t_errorf_v(t, "ParseFile returned wrong error %q, want %q",
                               error_text(err), want_err);

        AstSelectorExpr *sel = NULL;
        ast_inspect(&f->node, BURROW_FN(AstInspectFunc, find_selector, &sel));
        if (sel == NULL) {
            testing_t_error_v(t, "found no *ast.SelectorExpr");
            continue;
        }
        Str want_sel = S("&{fmt _}");
        Str got = sprint_selector(a, sel);
        if (!str_eq(got, want_sel)) {
            testing_t_errorf_v(t, "found selector %s, want %s", got, want_sel);
            continue;
        }
    }
    arena_free(&ar);
}

static AstTypeSpec *first_type_spec(AstFile *f) {
    AstGenDecl *gd = (AstGenDecl *)BURROW_AT(AstDecl, f->decls, 0);
    return (AstTypeSpec *)BURROW_AT(AstSpec, gd->specs, 0);
}

static void TestLastLineComment(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package main\ntype x int // comment\n");
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, token_new_file_set(a), BURROW_STR_EMPTY,
                                   str_src(&src), PARSER_PARSE_COMMENTS, &err);
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, error_text(err));
    Str comment = comment_text_at(first_type_spec(f)->comment, 0);
    if (!str_eq(comment, S("// comment")))
        testing_t_errorf_v(t, "got %q, want %q", comment, S("// comment"));
    arena_free(&ar);
}

typedef struct DepthTest {
    const char *name;
    const char *format;
    /* parse_multiplier is used when a single statement may result in more
     * than one change in the depth level, for instance "1+(..." produces a
     * BinaryExpr followed by a UnaryExpr, which increments the depth twice.
     * The test case comment explains which nodes are triggering the multiple
     * depth changes. */
    int parse_multiplier;
    /* scope is true if we should also test the statement for the resolver
     * scope depth limit. */
    bool scope;
    /* scope_multiplier does the same as parse_multiplier, but for the scope
     * depths. */
    int scope_multiplier;
} DepthTest;

/* The format expands the part inside « » many times. A second set of brackets
 * nested inside the first stops the repetition, so that for example «(«1»)»
 * expands to (((...((((1))))...))). */
static const DepthTest parse_depth_tests[] = {
    {"array", "package main; var x " LQ "[1]" RQ "int", 0, false, 0},
    {"slice", "package main; var x " LQ "[]" RQ "int", 0, false, 0},
    {"struct", "package main; var x " LQ "struct { X " LQ "int" RQ " }" RQ, 0, true, 0},
    {"pointer", "package main; var x " LQ "*" RQ "int", 0, false, 0},
    {"func", "package main; var x " LQ "func()" RQ "int", 0, true, 0},
    {"chan", "package main; var x " LQ "chan " RQ "int", 0, false, 0},
    {"chan2", "package main; var x " LQ "<-chan " RQ "int", 0, false, 0},
    /* Scopes: InterfaceType, FuncType */
    {"interface", "package main; var x " LQ "interface { M() " LQ "int" RQ " }" RQ, 0,
     true, 2},
    {"map", "package main; var x " LQ "map[int]" RQ "int", 0, false, 0},
    /* Parser nodes: UnaryExpr, CompositeLit */
    {"slicelit", "package main; var x = []any{" LQ "[]any{" LQ RQ "}" RQ "}", 3, false,
     0},
    /* Parser nodes: UnaryExpr, CompositeLit */
    {"arraylit", "package main; var x = " LQ "[1]any{" LQ "nil" RQ "}" RQ, 3, false, 0},
    /* Parser nodes: UnaryExpr, CompositeLit */
    {"structlit", "package main; var x = " LQ "struct{x any}{" LQ "nil" RQ "}" RQ, 3,
     false, 0},
    /* Parser nodes: CompositeLit, KeyValueExpr */
    {"maplit", "package main; var x = " LQ "map[int]any{1:" LQ "nil" RQ "}" RQ, 3,
     false, 0},
    {"element", "package main; var x = struct{x any}{x: " LQ "{" LQ RQ "}" RQ "}", 0,
     false, 0},
    {"dot", "package main; var x = " LQ "x." RQ "x", 0, false, 0},
    {"index", "package main; var x = x" LQ "[1]" RQ, 0, false, 0},
    {"slice", "package main; var x = x" LQ "[1:2]" RQ, 0, false, 0},
    {"slice3", "package main; var x = x" LQ "[1:2:3]" RQ, 0, false, 0},
    {"dottype", "package main; var x = x" LQ ".(any)" RQ, 0, false, 0},
    {"callseq", "package main; var x = x" LQ "()" RQ, 0, false, 0},
    /* Parser nodes: SelectorExpr, CallExpr */
    {"methseq", "package main; var x = x" LQ ".m()" RQ, 2, false, 0},
    {"binary", "package main; var x = " LQ "1+" RQ "1", 0, false, 0},
    /* Parser nodes: BinaryExpr, ParenExpr */
    {"binaryparen", "package main; var x = " LQ "1+(" LQ "1" RQ ")" RQ, 2, false, 0},
    {"unary", "package main; var x = " LQ "^" RQ "1", 0, false, 0},
    {"addr", "package main; var x = " LQ "& " RQ "x", 0, false, 0},
    {"star", "package main; var x = " LQ "*" RQ "x", 0, false, 0},
    {"recv", "package main; var x = " LQ "<-" RQ "x", 0, false, 0},
    /* Parser nodes: Ident, CallExpr */
    {"call", "package main; var x = " LQ "f(" LQ "1" RQ ")" RQ, 2, false, 0},
    /* Parser nodes: ParenExpr, CallExpr */
    {"conv", "package main; var x = " LQ "(*T)(" LQ "1" RQ ")" RQ, 2, false, 0},
    {"label", "package main; func main() { " LQ "Label:" RQ " }", 0, false, 0},
    /* Parser nodes: IfStmt, BlockStmt. Scopes: IfStmt, BlockStmt */
    {"if", "package main; func main() { " LQ "if true { " LQ RQ " }" RQ "}", 2, true,
     2},
    {"ifelse", "package main; func main() { " LQ "if true {} else " RQ " {} }", 0, true,
     0},
    /* Scopes: TypeSwitchStmt, CaseClause */
    {"switch", "package main; func main() { " LQ "switch { default: " LQ RQ " }" RQ "}",
     0, true, 2},
    /* Scopes: TypeSwitchStmt, CaseClause */
    {"typeswitch",
     "package main; func main() { " LQ "switch x.(type) { default: " LQ RQ " }" RQ " }",
     0, true, 2},
    /* Scopes: ForStmt, BlockStmt */
    {"for0", "package main; func main() { " LQ "for { " LQ RQ " }" RQ " }", 0, true, 2},
    /* Scopes: ForStmt, BlockStmt */
    {"for1", "package main; func main() { " LQ "for x { " LQ RQ " }" RQ " }", 0, true,
     2},
    /* Scopes: ForStmt, BlockStmt */
    {"for3",
     "package main; func main() { " LQ "for f(); g(); h() { " LQ RQ " }" RQ " }", 0,
     true, 2},
    /* Scopes: RangeStmt, BlockStmt */
    {"forrange0", "package main; func main() { " LQ "for range x { " LQ RQ " }" RQ " }",
     0, true, 2},
    /* Scopes: RangeStmt, BlockStmt */
    {"forrange1",
     "package main; func main() { " LQ "for x = range z { " LQ RQ " }" RQ " }", 0, true,
     2},
    /* Scopes: RangeStmt, BlockStmt */
    {"forrange2",
     "package main; func main() { " LQ "for x, y = range z { " LQ RQ " }" RQ " }", 0,
     true, 2},
    /* Parser nodes: GoStmt, FuncLit */
    {"go", "package main; func main() { " LQ "go func() { " LQ RQ " }()" RQ " }", 2,
     true, 0},
    /* Parser nodes: DeferStmt, FuncLit */
    {"defer", "package main; func main() { " LQ "defer func() { " LQ RQ " }()" RQ " }",
     2, true, 0},
    {"select",
     "package main; func main() { " LQ "select { default: " LQ RQ " }" RQ " }", 0, true,
     0},
    {"block", "package main; func main() { " LQ "{" LQ RQ "}" RQ " }", 0, true, 0},
};

/* split splits pre«mid»post into pre, mid, post. If the string does not have
 * that form, split returns x, "", "". */
static void split(Str x, Str *pre, Str *mid, Str *post) {
    Int start = strings_index(x, S(LQ)), end = strings_last_index(x, S(RQ));
    if (start < 0 || end < 0) {
        *pre = x;
        *mid = BURROW_STR_EMPTY;
        *post = BURROW_STR_EMPTY;
        return;
    }
    Int l = (Int)strlen(LQ), r = (Int)strlen(RQ);
    *pre = str_from_bytes(x.p, start);
    *mid = str_from_bytes(x.p + start + l, end - start - l);
    *post = str_from_bytes(x.p + end + r, x.len - end - r);
}

/* The input of a depth test with the part inside « » repeated n times. */
static Str depth_input(Alloc *a, const DepthTest *tt, Int n) {
    Str pre, mid, post;
    split(cstr(tt->format), &pre, &mid, &post);
    if (strings_contains(mid, S(LQ))) {
        Str left, base, right;
        split(mid, &left, &base, &right);
        mid = fmt_sprintf_v(a, "%s%s%s", strings_repeat(a, left, n), base,
                            strings_repeat(a, right, n));
    } else {
        mid = strings_repeat(a, mid, n);
    }
    return fmt_sprintf_v(a, "%s%s%s", pre, mid, post);
}

/* One run of a depth test, on a goroutine with a stack of its own. */
typedef struct DepthRun {
    TestingT *t;
    const DepthTest *tt;
    bool small;
    bool scope; /* TestScopeDepthLimit, and not TestParseDepthLimit */
    SyncWaitGroup wg;
} DepthRun;

static void depth_body(void *env) {
    DepthRun *r = (DepthRun *)env;
    const DepthTest *tt = r->tt;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Int n = (r->scope ? MAX_SCOPE_DEPTH : MAX_NEST_LEV) + 1;
    int mult = r->scope ? tt->scope_multiplier : tt->parse_multiplier;
    if (mult > 0)
        n /= mult;
    if (r->small) {
        /* Decrease the number of statements by 10, in order to check that we
         * do not fail when under the limit. 10 is used to provide some wiggle
         * room for cases where the surrounding scaffolding syntax adds some
         * noise to the depth that changes on a per testcase basis. */
        n -= 10;
    }
    Str input = depth_input(a, tt, n);

    ParserMode mode = r->scope ? PARSER_DECLARATION_ERRORS
                               : PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION;
    Error err = BURROW_NO_ERROR;
    (void)parser_parse_file(a, token_new_file_set(a), BURROW_STR_EMPTY, str_src(&input),
                            mode, &err);
    if (r->small) {
        if (!BURROW_OK(err))
            testing_t_errorf_v(r->t, "ParseFile(...): %s (want success)",
                               error_text(err));
    } else {
        Str expected = r->scope ? S("exceeded max scope depth during object resolution")
                                : S("exceeded max nesting depth");
        if (BURROW_OK(err) || !strings_has_suffix(error_text(err), expected))
            testing_t_errorf_v(r->t, "ParseFile(...) = _, %s, want %q",
                               BURROW_OK(err) ? S("<nil>") : error_text(err), expected);
    }
    arena_free(&ar);
    sync_wait_group_done(&r->wg);
}

static void run_depth(TestingT *t, const DepthTest *tt, bool small, bool scope) {
    DepthRun r;
    memset(&r, 0, sizeof r);
    r.t = t;
    r.tt = tt;
    r.small = small;
    r.scope = scope;
    sync_wait_group_add(&r.wg, 1);
    /* Room for a hundred thousand levels, with sanitizers that make each
     * frame bigger. The pages are only used as the parse goes down. */
    size_t stack = scope ? (size_t)64 << 20 : (size_t)1 << 30;
    if (!go_stack(BURROW_FN(Func, depth_body, &r), stack))
        testing_t_fatalf_v(t, "go_stack failed");
    sync_wait_group_wait(&r.wg);
}

static void run_parse_small(void *env, TestingT *t) {
    run_depth(t, (const DepthTest *)env, true, false);
}

static void run_parse_big(void *env, TestingT *t) {
    run_depth(t, (const DepthTest *)env, false, false);
}

static void run_scope_small(void *env, TestingT *t) {
    run_depth(t, (const DepthTest *)env, true, true);
}

static void run_scope_big(void *env, TestingT *t) {
    run_depth(t, (const DepthTest *)env, false, true);
}

static void TestParseDepthLimit(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "test requires significant memory");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(parse_depth_tests); i++) {
        const DepthTest *tt = &parse_depth_tests[i];
        void *env = (void *)(uintptr_t)tt;
        testing_t_run(t, fmt_sprintf_v(a, "%s/small", cstr(tt->name)),
                      BURROW_FN(TestingTFunc, run_parse_small, env));
        testing_t_run(t, fmt_sprintf_v(a, "%s/big", cstr(tt->name)),
                      BURROW_FN(TestingTFunc, run_parse_big, env));
    }
    arena_free(&ar);
}

static void TestScopeDepthLimit(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(parse_depth_tests); i++) {
        const DepthTest *tt = &parse_depth_tests[i];
        if (!tt->scope)
            continue;
        void *env = (void *)(uintptr_t)tt;
        testing_t_run(t, fmt_sprintf_v(a, "%s/small", cstr(tt->name)),
                      BURROW_FN(TestingTFunc, run_scope_small, env));
        testing_t_run(t, fmt_sprintf_v(a, "%s/big", cstr(tt->name)),
                      BURROW_FN(TestingTFunc, run_scope_big, env));
    }
    arena_free(&ar);
}

typedef struct RangePos {
    TestingT *t;
    TokenFileSet *fset;
    Str src;
} RangePos;

static bool check_range_pos(void *env, AstNode x) {
    RangePos *c = (RangePos *)env;
    if (x != NULL && x->kind == AST_KIND_RANGE_STMT) {
        TokenPosition pos =
            token_file_set_position(c->fset, ((AstRangeStmt *)x)->range);
        Int want = strings_index(c->src, S("range"));
        if (pos.offset != want)
            testing_t_errorf_v(c->t, "%s: got offset %d, want %d", c->src, pos.offset,
                               want);
    }
    return true;
}

/* proposal go.dev/issue/50429 */
static void TestRangePos(TestingT *t) {
    static const char *const testcases[] = {
        "package p; func _() { for range x {} }",
        "package p; func _() { for i = range x {} }",
        "package p; func _() { for i := range x {} }",
        "package p; func _() { for k, v = range x {} }",
        "package p; func _() { for k, v := range x {} }",
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(testcases); i++) {
        Str src = cstr(testcases[i]);
        TokenFileSet *fset = token_new_file_set(a);
        Error err = BURROW_NO_ERROR;
        AstFile *f = parser_parse_file(a, fset, src, str_src(&src), 0, &err);
        if (!BURROW_OK(err))
            testing_t_fatal_v(t, error_text(err));

        RangePos c = {t, fset, src};
        ast_inspect(&f->node, BURROW_FN(AstInspectFunc, check_range_pos, &c));
    }
    arena_free(&ar);
}

/* TestIssue59180 tests that line number overflow doesn't cause an infinite
 * loop. */
static void TestIssue59180(TestingT *t) {
    static const char *const testcases[] = {
        "package p\n//line :9223372036854775806\n\n//",
        "package p\n//line :1:9223372036854775806\n\n//",
        "package p\n//line file:9223372036854775806\n\n//",
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(testcases); i++) {
        Str src = cstr(testcases[i]);
        Error err = BURROW_NO_ERROR;
        (void)parser_parse_file(a, token_new_file_set(a), BURROW_STR_EMPTY,
                                str_src(&src), PARSER_PARSE_COMMENTS, &err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "ParseFile(%s) succeeded unexpectedly", src);
    }
    arena_free(&ar);
}

static void TestGoVersion(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    Str dir = filepath_join_v(a, 2, testdata_dir(t, a), S("goversion"));
    Error err = BURROW_NO_ERROR;
    Map *pkgs = parser_parse_dir(a, fset, dir, (ParserFileFilter){NULL, NULL}, 0, &err);
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, error_text(err));

    MapIter it = map_iter(pkgs);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v)) {
        AstPackage *p = *(AstPackage **)v;
        Str want = strings_replace_all(a, p->name, S("_"), S("."));
        if (str_eq(want, S("none")))
            want = BURROW_STR_EMPTY;
        MapIter fit = map_iter(p->files);
        const void *fk = NULL;
        void *fv = NULL;
        while (map_next(&fit, &fk, &fv)) {
            AstFile *f = *(AstFile **)fv;
            if (!str_eq(f->go_version, want))
                testing_t_errorf_v(
                    t, "%s: GoVersion = %q, want %q",
                    token_position_string(
                        token_file_set_position(fset, ast_file_pos(f)), a),
                    f->go_version, want);
        }
    }
    arena_free(&ar);
}

static void TestIssue57490(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* program not correctly terminated */
    Str src = S("package p; func f() { var x struct");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *file =
        parser_parse_file(a, fset, BURROW_STR_EMPTY, str_src(&src), 0, &err);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "syntax error expected, but no error reported");

    /* Because of the syntax error, the end position of the function
     * declaration is past the end of the file's position range. */
    TokenPos func_end = ast_decl_end(BURROW_AT(AstDecl, file->decls, 0));

    /* Offset(funcEnd) must not panic (to test panic, set debug=true in token
     * package) (panic: offset 35 out of bounds [0, 34] (position 36 out of
     * bounds [1, 35])) */
    TokenFile *tok_file = token_file_set_file(fset, ast_file_pos(file));
    Int offset = token_file_offset(tok_file, func_end);
    if (offset != token_file_size(tok_file))
        testing_t_fatalf_v(t, "offset = %d, want %d", offset,
                           token_file_size(tok_file));
    arena_free(&ar);
}

static void TestParseTypeParamsAsParenExpr(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package p; type X[A (B),] struct{}");
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, token_new_file_set(a), S("test.go"), str_src(&src),
                          PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, error_text(err));

    AstField *field = BURROW_AT(AstField *, first_type_spec(f)->type_params->list, 0);
    AstExpr type_param = field->type;
    if (type_param->kind != AST_KIND_PAREN_EXPR)
        testing_t_fatalf_v(t, "typeParam is node kind %d; want: *ast.ParenExpr",
                           type_param->kind);
    arena_free(&ar);
}

/* TestEmptyFileHasValidStartEnd is a regression test for #70162. */
static void TestEmptyFileHasValidStartEnd(TestingT *t) {
    static const struct {
        const char *src;
        const char *want; /* "Pos() FileStart FileEnd" */
    } tests[] = {
        {"", "0 1 1"},
        {"package ", "0 1 9"},
        {"package p", "1 1 10"},
        {"type T int", "0 1 11"},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(tests); i++) {
        Str src = cstr(tests[i].src);
        Error err = BURROW_NO_ERROR;
        AstFile *f = parser_parse_file(a, token_new_file_set(a), S("a.go"),
                                       str_src(&src), 0, &err);
        if (f == NULL) {
            testing_t_fatalf_v(t, "src = %q: got no file", src);
            break;
        }
        Str got =
            fmt_sprintf_v(a, "%d %d %d", ast_file_pos(f), f->file_start, f->file_end);
        if (!str_eq(got, cstr(tests[i].want)))
            testing_t_fatalf_v(t, "src = %q: got %s, want %s", src, got,
                               cstr(tests[i].want));
    }
    arena_free(&ar);
}

/* What ast.Fprint shows of a list of comment groups, enough to see how two
 * differ. */
static Str dump_comment_groups(Alloc *a, Slice groups) {
    StringsBuilder b = STRINGS_BUILDER(a);
    for (Int i = 0; i < groups.len; i++) {
        AstCommentGroup *g = BURROW_AT(AstCommentGroup *, groups, i);
        (void)strings_builder_write_string(&b, fmt_sprintf_v(a, "group %d\n", i), NULL);
        for (Int j = 0; j < g->list.len; j++) {
            AstComment *c = BURROW_AT(AstComment *, g->list, j);
            (void)strings_builder_write_string(
                &b, fmt_sprintf_v(a, "  Slash: %d Text: %q\n", c->slash, c->text),
                NULL);
        }
    }
    return strings_builder_string(&b);
}

static void TestCommentGroupWithLineDirective(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package main\nfunc test() {\n//line a:15:1\n\t//\n}\n");
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, token_new_file_set(a), S("test.go"), str_src(&src),
                          PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, error_text(err));

    static const struct {
        TokenPos slash;
        const char *text;
    } want_comments[] = {
        {28, "//line a:15:1"},
        {43, "//"},
    };
    bool equal = f->comments.len == 1;
    if (equal) {
        AstCommentGroup *g = BURROW_AT(AstCommentGroup *, f->comments, 0);
        equal = g->list.len == NELEM(want_comments);
        for (Int j = 0; equal && j < g->list.len; j++) {
            AstComment *c = BURROW_AT(AstComment *, g->list, j);
            equal = c->slash == want_comments[j].slash &&
                    str_eq(c->text, cstr(want_comments[j].text));
        }
    }
    if (!equal) {
        Str want = S("group 0\n  Slash: 28 Text: \"//line a:15:1\"\n"
                     "  Slash: 43 Text: \"//\"\n");
        testing_t_fatalf_v(t, "unexpected f.Comments got:\n%s\nwant:\n%s",
                           dump_comment_groups(a, f->comments), want);
    }
    arena_free(&ar);
}

static AstValueSpec *first_value_spec(AstDecl d) {
    return (AstValueSpec *)BURROW_AT(AstSpec, ((AstGenDecl *)d)->specs, 0);
}

/* TestBothLineAndLeadComment makes sure that we populate the p.lineComment
 * field even though there is a comment after the line comment. */
static void TestBothLineAndLeadComment(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package test\n\nvar _ int; /* line comment */\n// Doc comment\n"
                "func _() {}\n\nvar _ int; /* line comment */\n// Some comment\n\n"
                "func _() {}\n");
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, token_new_file_set(a), BURROW_STR_EMPTY, str_src(&src),
                          PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION, &err);

    AstCommentGroup *line_comment =
        first_value_spec(BURROW_AT(AstDecl, f->decls, 0))->comment;
    AstCommentGroup *doc_comment =
        ((AstFuncDecl *)BURROW_AT(AstDecl, f->decls, 1))->doc;

    if (line_comment == NULL)
        testing_t_fatal_v(t, "missing line comment");
    if (doc_comment == NULL)
        testing_t_fatal_v(t, "missing doc comment");

    if (!str_eq(comment_text_at(line_comment, 0), S("/* line comment */")))
        testing_t_errorf_v(
            t, "unexpected line comment got = %q; want \"/* line comment */\"",
            comment_text_at(line_comment, 0));
    if (!str_eq(comment_text_at(doc_comment, 0), S("// Doc comment")))
        testing_t_errorf_v(t,
                           "unexpected line comment got = %q; want \"// Doc comment\"",
                           comment_text_at(doc_comment, 0));

    AstCommentGroup *line_comment2 =
        first_value_spec(BURROW_AT(AstDecl, f->decls, 2))->comment;
    if (line_comment2 == NULL)
        testing_t_fatal_v(t, "missing line comment");
    /* Go checks lineComment here again, not lineComment2. */
    if (!str_eq(comment_text_at(line_comment, 0), S("/* line comment */")))
        testing_t_errorf_v(
            t, "unexpected line comment got = %q; want \"/* line comment */\"",
            comment_text_at(line_comment, 0));

    AstCommentGroup *doc_comment2 =
        ((AstFuncDecl *)BURROW_AT(AstDecl, f->decls, 3))->doc;
    if (doc_comment2 != NULL)
        testing_t_errorf_v(t, "unexpected doc comment %s",
                           comment_text(a, doc_comment2));
    arena_free(&ar);
}

typedef struct BasicLitEnd {
    TestingT *t;
    Alloc *a;
    TokenFileSet *fset;
    TokenFile *tok_file;
    Str src;
    Str stringlit;
    int count;
} BasicLitEnd;

static bool check_basic_lit(void *env, AstNode n) {
    BasicLitEnd *c = (BasicLitEnd *)env;
    if (n == NULL || n->kind != AST_KIND_BASIC_LIT)
        return true;
    AstBasicLit *lit = (AstBasicLit *)n;
    c->count++;
    Int start = token_file_offset(c->tok_file, ast_basic_lit_pos(lit));
    Int end = token_file_offset(c->tok_file, ast_basic_lit_end(lit));
    Str where = token_position_string(
        token_file_set_position(c->fset, ast_basic_lit_pos(lit)), c->a);

    /* Check BasicLit.Value. */
    Str want = S("`abc\n`");
    if (!str_eq(lit->value, want))
        testing_t_errorf_v(c->t, "%s: BasicLit.Value = %q, want %q", where, lit->value,
                           want);

    /* Check source extent. */
    Str got = str_from_bytes(c->src.p + start, end - start);
    if (!str_eq(got, c->stringlit))
        testing_t_errorf_v(c->t, "%s: src[BasicLit.Pos:End] = %q, want %q", where, got,
                           c->stringlit);
    return true;
}

/* Tests of BasicLit.End() method, which in go1.26 started precisely recording
 * the Value token's end position instead of heuristically computing it, which
 * is inaccurate for strings containing "\r". */
static void TestBasicLit_End(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* lit is a raw string literal containing [a b c \r \n], denoting "abc\n",
     * because the scanner normalizes \r\n to \n. */
    Str stringlit = S("`abc\r\n`");

    /* The semicolons exercise the case in which the next token (a SEMICOLON
     * implied by a \n) isn't immediate but follows some horizontal space. */
    Str src = fmt_sprintf_v(a,
                            "package p\n\nimport %s ;\n\ntype _ struct{ x int %s }\n\n"
                            "const _ = %s ;\n",
                            stringlit, stringlit, stringlit);

    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, fset, BURROW_STR_EMPTY, str_src(&src),
                          PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION, &err);
    TokenFile *tok_file = token_file_set_file(fset, ast_file_pos(f));

    BasicLitEnd c = {t, a, fset, tok_file, src, stringlit, 0};
    ast_inspect(&f->node, BURROW_FN(AstInspectFunc, check_basic_lit, &c));
    if (c.count != 3)
        testing_t_errorf_v(t, "found %d BasicLit, want 3", c.count);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestParse)                                                                       \
    X(TestParseFile)                                                                   \
    X(TestParseExprFrom)                                                               \
    X(TestParseDir)                                                                    \
    X(TestIssue42951)                                                                  \
    X(TestParseExpr)                                                                   \
    X(TestColonEqualsScope)                                                            \
    X(TestVarScope)                                                                    \
    X(TestObjects)                                                                     \
    X(TestUnresolved)                                                                  \
    X(TestCommentGroups)                                                               \
    X(TestLeadAndLineComments)                                                         \
    X(TestIssue9979)                                                                   \
    X(TestFileStartEndPos)                                                             \
    X(TestIncompleteSelection)                                                         \
    X(TestLastLineComment)                                                             \
    X(TestParseDepthLimit)                                                             \
    X(TestScopeDepthLimit)                                                             \
    X(TestRangePos)                                                                    \
    X(TestIssue59180)                                                                  \
    X(TestGoVersion)                                                                   \
    X(TestIssue57490)                                                                  \
    X(TestParseTypeParamsAsParenExpr)                                                  \
    X(TestEmptyFileHasValidStartEnd)                                                   \
    X(TestCommentGroupWithLineDirective)                                               \
    X(TestBothLineAndLeadComment)                                                      \
    X(TestBasicLit_End)

TESTING_MAIN(TESTS)
