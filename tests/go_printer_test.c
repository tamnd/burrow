/* Derived from Go's src/go/printer/printer_test.go.
 * Go source: go1.27.1.
 *
 * TestFiles, TestBaseIndent and TestWriteErrors read testdata and printer.go
 * from tests/go_printer_test_gen.h. Go's TestFiles also fails a file that
 * takes over ten seconds to format; that limit is left to the test runner
 * here. Go's init check that the printer can run while the package
 * initialises is TestInit, and TestUnsupported is burrow's own.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/go/printer.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"

#include <stdint.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "go_printer_test_gen.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

enum { GPRT_TABWIDTH = 8 };

static const ParserMode gprt_parse_mode =
    PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION;

/* The file dir+name of the generated header, put back together. */
static bool gprt_file(Alloc *a, const char *dir, const char *name, Str *out) {
    size_t ndir = strlen(dir);
    for (Int i = 0; i < NELEM(gprt_files); i++) {
        const GprtFile *f = &gprt_files[i];
        if (strncmp(f->name, dir, ndir) != 0 || strcmp(f->name + ndir, name) != 0)
            continue;
        StringsBuilder b = STRINGS_BUILDER(a);
        for (int k = 0; k < f->npieces; k++)
            (void)strings_builder_write_string(
                &b, str_from_bytes((const Byte *)f->pieces[k].p, (Int)f->pieces[k].n),
                NULL);
        *out = strings_builder_string(&b);
        return true;
    }
    *out = BURROW_STR_EMPTY;
    return false;
}

static Any gprt_src(Str *s) {
    return BURROW_ANY(TYPE_STRING, s);
}

static AstIdent *gprt_ident(Alloc *a, Str name) {
    AstIdent *id = (AstIdent *)ast_node_new(a, AST_KIND_IDENT);
    id->name = name;
    return id;
}

/* format parses src, prints the tree, checks that the result parses, and
 * returns it in *res. */
static Error gprt_format(Alloc *a, TokenFileSet *fset, Str src, unsigned mode,
                         Str *res) {
    *res = BURROW_STR_EMPTY;

    /* parse src */
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, fset, BURROW_STR_EMPTY, gprt_src(&src),
                                   gprt_parse_mode, &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("parse: %s\n%s", error_text(err), src);

    /* filter exports if necessary */
    if ((mode & GPRT_EXPORT) != 0) {
        (void)ast_file_exports(f);                           /* ignore result */
        f->comments = (Slice){NULL, 0, 0, f->comments.elem}; /* not in the tree */
    }

    /* determine printer configuration */
    PrinterConfig cfg = {0, GPRT_TABWIDTH, 0};
    if ((mode & GPRT_RAW_FORMAT) != 0)
        cfg.mode |= PRINTER_RAW_FORMAT;
    if ((mode & GPRT_NORM_NUMBER) != 0)
        cfg.mode |= BURROW__PRINTER_NORMALIZE_NUMBERS;

    /* print the tree */
    StringsBuilder b = STRINGS_BUILDER(a);
    err = printer_config_fprint(&cfg, a, strings_builder_as_io_writer(&b), fset,
                                BURROW_ANY(TYPE_AST_FILE_PTR, &f));
    if (BURROW_FAILED(err))
        return fmt_errorf_v("print: %s", error_text(err));

    /* make sure the output is syntactically correct */
    Str out = strings_builder_string(&b);
    (void)parser_parse_file(a, fset, BURROW_STR_EMPTY, gprt_src(&out), gprt_parse_mode,
                            &err);
    if (BURROW_FAILED(err))
        return fmt_errorf_v("re-parse: %s\n%s", error_text(err), out);

    *res = out;
    return BURROW_NO_ERROR;
}

/* checkEqual. Go reports a diff; this reports the first line that differs. */
static bool gprt_check_equal(TestingT *t, Alloc *a, Str aname, Str bname, Str x, Str y,
                             const char *what) {
    if (str_eq(x, y))
        return true;
    Slice xl = strings_split(a, x, S("\n"));
    Slice yl = strings_split(a, y, S("\n"));
    Int i = 0;
    while (i < xl.len && i < yl.len && str_eq(((Str *)xl.p)[i], ((Str *)yl.p)[i]))
        i++;
    Str xs = i < xl.len ? ((Str *)xl.p)[i] : S("<end>");
    Str ys = i < yl.len ? ((Str *)yl.p)[i] : S("<end>");
    testing_t_errorf_v(t, "%s%s and %s differ at line %d:\n%s: %q\n%s: %q",
                       str_from_cstr(what), aname, bname, i + 1, aname, xs, bname, ys);
    return false;
}

/* ------------------------------------------------------------------ files */

static void gprt_run_check(void *env, TestingT *t) {
    const GprtEntry *e = (const GprtEntry *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);

    Str src = BURROW_STR_EMPTY, gld = BURROW_STR_EMPTY;
    if (!gprt_file(a, "testdata/", e->source, &src) ||
        !gprt_file(a, "testdata/", e->golden, &gld)) {
        testing_t_errorf_v(t, "no testdata/%s or testdata/%s", str_from_cstr(e->source),
                           str_from_cstr(e->golden));
        arena_free(&ar);
        return;
    }
    Str source = fmt_sprintf_v(a, "testdata/%s", str_from_cstr(e->source));
    Str golden = fmt_sprintf_v(a, "testdata/%s", str_from_cstr(e->golden));

    Str res = BURROW_STR_EMPTY;
    Error err = gprt_format(a, fset, src, e->mode, &res);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }

    /* formatted source and golden must be the same */
    if (!gprt_check_equal(t, a, fmt_sprintf_v(a, "format(%s)", source), golden, res,
                          gld, "")) {
        arena_free(&ar);
        return;
    }

    if ((e->mode & GPRT_IDEMPOTENT) != 0) {
        /* formatting golden must be idempotent */
        err = gprt_format(a, fset, gld, e->mode, &res);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s", error_text(err));
            arena_free(&ar);
            return;
        }
        (void)gprt_check_equal(t, a, golden, fmt_sprintf_v(a, "format(%s)", golden),
                               gld, res, "golden is not idempotent: ");
    }
    arena_free(&ar);
}

static void TestFiles(TestingT *t) {
    for (Int i = 0; i < NELEM(gprt_data); i++) {
        const GprtEntry *e = &gprt_data[i];
        testing_t_run(t, str_from_cstr(e->source),
                      BURROW_FN(TestingTFunc, gprt_run_check, (void *)(uintptr_t)e));
    }
}

/* TestLineComments, using a simple test case, checks that consecutive line
 * comments are properly terminated with a newline even if the AST position
 * information is incorrect. */
static void TestLineComments(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("// comment 1\n"
                "\t// comment 2\n"
                "\t// comment 3\n"
                "\tpackage main\n"
                "\t");

    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, fset, BURROW_STR_EMPTY, gprt_src(&src),
                                   gprt_parse_mode, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }

    StringsBuilder b = STRINGS_BUILDER(a);
    fset = token_new_file_set(a); /* use the wrong file set */
    (void)printer_fprint(a, strings_builder_as_io_writer(&b), fset,
                         BURROW_ANY(TYPE_AST_FILE_PTR, &f));
    Str out = strings_builder_string(&b);

    Int nlines = strings_count(out, S("\n"));
    const Int expected = 3;
    if (nlines < expected) {
        testing_t_errorf_v(t, "got %d, expected %d\n", nlines, expected);
        testing_t_errorf_v(t, "result:\n%s", out);
    }
    arena_free(&ar);
}

/* Go's init: the printer can be invoked during initialization. */
static void TestInit(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str name = S("foobar");
    AstIdent *id = gprt_ident(a, name);
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err =
        printer_fprint(a, strings_builder_as_io_writer(&b), token_new_file_set(a),
                       BURROW_ANY(TYPE_AST_IDENT_PTR, &id));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    else if (!str_eq(strings_builder_string(&b), name))
        testing_t_fatalf_v(t, "got %s, want %s", strings_builder_string(&b), name);
    arena_free(&ar);
}

/* Verify that the printer doesn't crash if the AST contains BadXXX nodes. */
static void TestBadNodes(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package p\n(");
    Str res = S("package p\nBadDecl\n");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, fset, BURROW_STR_EMPTY, gprt_src(&src),
                                   gprt_parse_mode, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected illegal program"); /* error in test */
    StringsBuilder b = STRINGS_BUILDER(a);
    (void)printer_fprint(a, strings_builder_as_io_writer(&b), fset,
                         BURROW_ANY(TYPE_AST_FILE_PTR, &f));
    if (!str_eq(strings_builder_string(&b), res))
        testing_t_errorf_v(t, "got %q, expected %q", strings_builder_string(&b), res);
    arena_free(&ar);
}

/* testComment verifies that f can be parsed again after printing it with its
 * first comment set to comment at any possible source offset. */
static void gprt_test_comment(TestingT *t, Alloc *a, TokenFileSet *fset, AstFile *f,
                              Int srclen, AstComment *comment) {
    AstCommentGroup *g = ((AstCommentGroup **)f->comments.p)[0];
    ((AstComment **)g->list.p)[0] = comment;
    for (Int offs = 0; offs <= srclen; offs++) {
        StringsBuilder b = STRINGS_BUILDER(a);
        /* Printing f should result in a correct program no matter what the
         * (incorrect) comment position is. */
        Error err = printer_fprint(a, strings_builder_as_io_writer(&b), fset,
                                   BURROW_ANY(TYPE_AST_FILE_PTR, &f));
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s", error_text(err));
        Str out = strings_builder_string(&b);
        (void)parser_parse_file(a, fset, BURROW_STR_EMPTY, gprt_src(&out),
                                PARSER_SKIP_OBJECT_RESOLUTION, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "incorrect program for pos = %d:\n%s",
                               (Int)comment->slash, out);
            return;
        }
        /* Position information is just an offset. Move comment one byte down
         * in the source. */
        comment->slash++;
    }
}

static AstComment *gprt_comment(Alloc *a, TokenPos pos, Str text) {
    AstComment *c = (AstComment *)ast_node_new(a, AST_KIND_COMMENT);
    c->slash = pos;
    c->text = text;
    return c;
}

/* Verify that the printer produces a correct program even if the position
 * information of comments introducing newlines is incorrect. */
static void TestBadComments(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("\n"
                "// first comment - text and position changed by test\n"
                "package p\n"
                "import \"fmt\"\n"
                "const pi = 3.14 // rough circle\n"
                "var (\n"
                "\tx, y, z int = 1, 2, 3\n"
                "\tu, v float64\n"
                ")\n"
                "func fibo(n int) {\n"
                "\tif n < 2 {\n"
                "\t\treturn n /* seed values */\n"
                "\t}\n"
                "\treturn fibo(n-1) + fibo(n-2)\n"
                "}\n");

    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, fset, BURROW_STR_EMPTY, gprt_src(&src),
                                   gprt_parse_mode, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s", error_text(err)); /* error in test */
    if (f == NULL || f->comments.len == 0) {
        testing_t_fatalf_v(t, "no comments");
        arena_free(&ar);
        return;
    }

    AstComment *comment =
        ((AstComment **)((AstCommentGroup **)f->comments.p)[0]->list.p)[0];
    TokenPos pos = ast_comment_pos(comment);
    if (token_file_set_position_for(fset, pos, false /* absolute position */).offset !=
        1)
        testing_t_errorf_v(t, "expected offset 1"); /* error in test */

    gprt_test_comment(t, a, fset, f, src.len,
                      gprt_comment(a, pos, S("//-style comment")));
    gprt_test_comment(t, a, fset, f, src.len,
                      gprt_comment(a, pos, S("/*-style comment */")));
    gprt_test_comment(t, a, fset, f, src.len,
                      gprt_comment(a, pos, S("/*-style \n comment */")));
    gprt_test_comment(t, a, fset, f, src.len,
                      gprt_comment(a, pos, S("/*-style comment \n\n\n */")));
    arena_free(&ar);
}

/* idents, with ast_inspect in place of Go's walk over a channel. */
enum { GPRT_MAX_IDENTS = 256 };

typedef struct GprtIdents {
    AstIdent *list[GPRT_MAX_IDENTS];
    Int len;
    bool overflow;
} GprtIdents;

static bool gprt_collect_ident(void *env, AstNode n) {
    GprtIdents *v = (GprtIdents *)env;
    if (n != NULL && n->kind == AST_KIND_IDENT) {
        if (v->len < GPRT_MAX_IDENTS)
            v->list[v->len++] = (AstIdent *)n;
        else
            v->overflow = true;
    }
    return true;
}

/* Verify that the SourcePos mode emits correct //line directives by testing
 * that position information for matching identifiers is maintained. */
static void TestSourcePos(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("\n"
                "package p\n"
                "import ( \"go/printer\"; \"math\" )\n"
                "const pi = 3.14; var x = 0\n"
                "type t struct{ x, y, z int; u, v, w float32 }\n"
                "func (t *t) foo(a, b, c int) int {\n"
                "\treturn a*t.x + b*t.y +\n"
                "\t\t// two extra lines here\n"
                "\t\t// ...\n"
                "\t\tc*t.z\n"
                "}\n");

    /* parse original */
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f1 =
        parser_parse_file(a, fset, S("src"), gprt_src(&src), gprt_parse_mode, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }

    /* pretty-print original */
    StringsBuilder b = STRINGS_BUILDER(a);
    PrinterConfig cfg = {PRINTER_USE_SPACES | PRINTER_SOURCE_POS, 8, 0};
    err = printer_config_fprint(&cfg, a, strings_builder_as_io_writer(&b), fset,
                                BURROW_ANY(TYPE_AST_FILE_PTR, &f1));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }
    Str out = strings_builder_string(&b);

    /* parse pretty printed original (//line directives must be interpreted
     * even without PARSER_PARSE_COMMENTS) */
    AstFile *f2 = parser_parse_file(a, fset, BURROW_STR_EMPTY, gprt_src(&out),
                                    PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s\n%s", error_text(err), out);
        arena_free(&ar);
        return;
    }

    /* At this point the position information of identifiers in f2 should
     * match the position information of corresponding identifiers in f1. */
    static const GprtIdents zero = {{NULL}, 0, false};
    GprtIdents v1 = zero, v2 = zero;
    ast_inspect(&f1->node, (AstInspectFunc){gprt_collect_ident, &v1});
    ast_inspect(&f2->node, (AstInspectFunc){gprt_collect_ident, &v2});
    if (v1.overflow || v2.overflow) {
        testing_t_fatalf_v(t, "more than %d idents", (Int)GPRT_MAX_IDENTS);
        arena_free(&ar);
        return;
    }

    /* number of identifiers must be > 0 (test should run) and must match */
    if (v1.len == 0) {
        testing_t_fatalf_v(t, "got no idents");
        arena_free(&ar);
        return;
    }
    if (v2.len != v1.len)
        testing_t_errorf_v(t, "got %d idents; want %d", v2.len, v1.len);

    /* verify that all identifiers have correct line information */
    for (Int i = 0; i < v1.len && i < v2.len; i++) {
        AstIdent *id1 = v1.list[i];
        AstIdent *id2 = v2.list[i];
        if (!str_eq(id2->name, id1->name))
            testing_t_errorf_v(t, "got ident %s; want %s", id2->name, id1->name);

        /* here we care about the relative (line-directive adjusted)
         * positions */
        Int l1 = token_file_set_position(fset, ast_node_pos(&id1->node)).line;
        Int l2 = token_file_set_position(fset, ast_node_pos(&id2->node)).line;
        if (l2 != l1)
            testing_t_errorf_v(t, "got line %d; want %d for %s", l2, l1, id1->name);
    }

    if (testing_t_failed(t))
        testing_t_logf_v(t, "\n%s", out);
    arena_free(&ar);
}

/* Print f with cfg and return the output, failing t on an error. */
static bool gprt_print(TestingT *t, Alloc *a, const PrinterConfig *cfg,
                       TokenFileSet *fset, Any node, Str *out) {
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err =
        printer_config_fprint(cfg, a, strings_builder_as_io_writer(&b), fset, node);
    *out = strings_builder_string(&b);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        return false;
    }
    return true;
}

static const PrinterConfig gprt_default = {0, 8, 0};

/* Verify that the SourcePos mode doesn't emit unnecessary //line directives
 * before empty lines. */
static void TestIssue5945(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str orig = S("\n"
                 "package p   // line 2\n"
                 "func f() {} // line 3\n"
                 "\n"
                 "var x, y, z int\n"
                 "\n"
                 "\n"
                 "func g() { // line 8\n"
                 "}\n");

    Str want = S("//line src.go:2\n"
                 "package p\n"
                 "\n"
                 "//line src.go:3\n"
                 "func f() {}\n"
                 "\n"
                 "var x, y, z int\n"
                 "\n"
                 "//line src.go:8\n"
                 "func g() {\n"
                 "}\n");

    /* parse original */
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f1 = parser_parse_file(a, fset, S("src.go"), gprt_src(&orig),
                                    PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }

    /* pretty-print original */
    PrinterConfig cfg = {PRINTER_USE_SPACES | PRINTER_SOURCE_POS, 8, 0};
    Str got = BURROW_STR_EMPTY;
    if (gprt_print(t, a, &cfg, fset, BURROW_ANY(TYPE_AST_FILE_PTR, &f1), &got) &&
        !str_eq(got, want))
        testing_t_errorf_v(t, "got:\n%s\nwant:\n%s\n", got, want);
    arena_free(&ar);
}

static void TestIssue52605(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str orig = S("\n"
                 "package p\n"
                 "\n"
                 "// Doc\n"
                 "//\n"
                 "type T struct {\n"
                 "// This is not\n"
                 "//\ta doc comment.\n"
                 "X int\n"
                 "}\n");

    Str want = S("package p\n"
                 "\n"
                 "// Doc\n"
                 "type T struct {\n"
                 "\t// This is not\n"
                 "\t//\ta doc comment.\n"
                 "\tX int\n"
                 "}\n");

    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, fset, S("src.go"), gprt_src(&orig), gprt_parse_mode, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }

    Str got = BURROW_STR_EMPTY;
    if (gprt_print(t, a, &gprt_default, fset, BURROW_ANY(TYPE_AST_FILE_PTR, &f),
                   &got) &&
        !str_eq(got, want))
        testing_t_errorf_v(t, "got:\n%s\nwant:\n%s\n", got, want);
    arena_free(&ar);
}

static const char *const gprt_decls[] = {
    "import \"fmt\"",
    "const pi = 3.1415\nconst e = 2.71828\n\nvar x = pi",
    "func sum(x, y int) int\t{ return x + y }",
};

static void TestDeclLists(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    for (Int i = 0; i < NELEM(gprt_decls); i++) {
        Str src = str_from_cstr(gprt_decls[i]);
        Str full = fmt_sprintf_v(a, "package p;%s", src);
        Error err = BURROW_NO_ERROR;
        AstFile *file = parser_parse_file(a, fset, BURROW_STR_EMPTY, gprt_src(&full),
                                          gprt_parse_mode, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "%s", error_text(err)); /* error in test */
            break;
        }

        /* only print declarations */
        Str out = BURROW_STR_EMPTY;
        if (!gprt_print(t, a, &gprt_default, fset,
                        BURROW_ANY(TYPE_AST_DECL_SLICE, &file->decls), &out))
            break;
        if (!str_eq(out, src))
            testing_t_errorf_v(t, "\ngot : %q\nwant: %q\n", out, src);
    }
    arena_free(&ar);
}

static const char *const gprt_stmts[] = {
    "i := 0",
    "select {}\nvar a, b = 1, 2\nreturn a + b",
    "go f()\ndefer func() {}()",
};

static void TestStmtLists(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    for (Int i = 0; i < NELEM(gprt_stmts); i++) {
        Str src = str_from_cstr(gprt_stmts[i]);
        Str full = fmt_sprintf_v(a, "package p; func _() {%s}", src);
        Error err = BURROW_NO_ERROR;
        AstFile *file = parser_parse_file(a, fset, BURROW_STR_EMPTY, gprt_src(&full),
                                          gprt_parse_mode, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "%s", error_text(err)); /* error in test */
            break;
        }

        /* only print statements */
        AstFuncDecl *fd = (AstFuncDecl *)((AstDecl *)file->decls.p)[0];
        Str out = BURROW_STR_EMPTY;
        if (!gprt_print(t, a, &gprt_default, fset,
                        BURROW_ANY(TYPE_AST_STMT_SLICE, &fd->body->list), &out))
            break;
        if (!str_eq(out, src))
            testing_t_errorf_v(t, "\ngot : %q\nwant: %q\n", out, src);
    }
    arena_free(&ar);
}

typedef struct GprtBaseIndent {
    TokenFileSet *fset;
    AstFile *file;
    Int indent;
} GprtBaseIndent;

static void gprt_run_base_indent(void *env, TestingT *t) {
    const GprtBaseIndent *e = (const GprtBaseIndent *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    PrinterConfig cfg = {0, GPRT_TABWIDTH, e->indent};
    AstFile *file = e->file;
    StringsBuilder b = STRINGS_BUILDER(a);
    (void)printer_config_fprint(&cfg, a, strings_builder_as_io_writer(&b), e->fset,
                                BURROW_ANY(TYPE_AST_FILE_PTR, &file));
    /* all code must be indented by at least indent tabs */
    Slice lines = strings_split(a, strings_builder_string(&b), S("\n"));
    for (Int i = 0; i < lines.len; i++) {
        Str line = ((Str *)lines.p)[i];
        if (line.len == 0)
            continue; /* empty lines don't have indentation */
        Int n = 0;
        for (Int j = 0; j < line.len; j++) {
            if (line.p[j] != '\t') {
                /* end of indentation */
                n = j;
                break;
            }
        }
        if (n < e->indent)
            testing_t_errorf_v(t, "line %d: got only %d tabs; want at least %d: %q", i,
                               n, e->indent, line);
    }
    arena_free(&ar);
}

static void TestBaseIndent(TestingT *t) {
    /* The test file must not contain multi-line raw strings since those are
     * not indented (because their values must not change) and make this test
     * fail. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = BURROW_STR_EMPTY;
    if (!gprt_file(a, "", "printer.go", &src)) {
        testing_t_fatalf_v(t, "no printer.go");
        arena_free(&ar);
        return;
    }

    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *file = parser_parse_file(a, fset, S("printer.go"), gprt_src(&src),
                                      PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err)); /* error in test */
        arena_free(&ar);
        return;
    }

    GprtBaseIndent envs[4];
    for (Int indent = 0; indent < 4; indent++) {
        envs[indent] = (GprtBaseIndent){fset, file, indent};
        testing_t_run(t, fmt_sprintf_v(a, "%d", indent),
                      BURROW_FN(TestingTFunc, gprt_run_base_indent, &envs[indent]));
    }
    arena_free(&ar);
}

/* TestFuncType tests that an AstFuncType with a NULL params field can be
 * printed (per go/ast specification). Test case for issue 3870. */
static void TestFuncType(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    AstFuncDecl *fd = (AstFuncDecl *)ast_node_new(a, AST_KIND_FUNC_DECL);
    fd->name = gprt_ident(a, S("f"));
    fd->type = (AstFuncType *)ast_node_new(a, AST_KIND_FUNC_TYPE);
    AstDecl decls[1] = {&fd->node};
    AstFile *src = (AstFile *)ast_node_new(a, AST_KIND_FILE);
    src->name = gprt_ident(a, S("p"));
    src->decls = (Slice){decls, 1, 1, TYPE_AST_DECL};

    Str want = S("package p\n"
                 "\n"
                 "func f()\n");

    Str got = BURROW_STR_EMPTY;
    if (gprt_print(t, a, &gprt_default, token_new_file_set(a),
                   BURROW_ANY(TYPE_AST_FILE_PTR, &src), &got) &&
        !str_eq(got, want))
        testing_t_fatalf_v(t, "got:\n%s\nwant:\n%s\n", got, want);
    arena_free(&ar);
}

/* TestChanType tests that the tree for <-(<-chan int), without ParenExpr, is
 * correctly formatted with parens. Test case for issue #63362. */
static void TestChanType(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    AstChanType *ch = (AstChanType *)ast_node_new(a, AST_KIND_CHAN_TYPE);
    ch->dir = AST_RECV;
    ch->value = &gprt_ident(a, S("int"))->node;
    AstExpr args[1] = {&gprt_ident(a, S("nil"))->node};
    AstCallExpr *call = (AstCallExpr *)ast_node_new(a, AST_KIND_CALL_EXPR);
    call->fun = &ch->node;
    call->args = (Slice){args, 1, 1, TYPE_AST_EXPR};
    AstUnaryExpr *expr = (AstUnaryExpr *)ast_node_new(a, AST_KIND_UNARY_EXPR);
    expr->op = TOKEN_ARROW;
    expr->x = &call->node;

    Str got = BURROW_STR_EMPTY;
    Str want = S("<-(<-chan int)(nil)");
    if (gprt_print(t, a, &gprt_default, token_new_file_set(a),
                   BURROW_ANY(TYPE_OF(AstUnaryExprPtr), &expr), &got) &&
        !str_eq(got, want))
        testing_t_fatalf_v(t, "got:\n%s\nwant:\n%s\n", got, want);
    arena_free(&ar);
}

typedef struct GprtLimitWriter {
    Int remaining;
    Int err_count;
} GprtLimitWriter;

static Int gprt_limit_write(void *self, Slice buf, Error *err) {
    GprtLimitWriter *l = (GprtLimitWriter *)self;
    Int n = buf.len;
    *err = BURROW_NO_ERROR;
    if (n >= l->remaining) {
        n = l->remaining;
        *err = io_eof;
        l->err_count++;
    }
    l->remaining -= n;
    return n;
}

static const IoWriterVT gprt_limit_vt = {NULL, gprt_limit_write};

/* Test whether the printer stops writing after the first error. */
static void TestWriteErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = BURROW_STR_EMPTY;
    if (!gprt_file(a, "", "printer.go", &src)) {
        testing_t_fatalf_v(t, "no printer.go");
        arena_free(&ar);
        return;
    }
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *file = parser_parse_file(a, fset, S("printer.go"), gprt_src(&src),
                                      PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err)); /* error in test */
        arena_free(&ar);
        return;
    }
    for (Int i = 0; i < 20; i++) {
        GprtLimitWriter lw = {i, 0};
        PrinterConfig cfg = {PRINTER_RAW_FORMAT, 0, 0};
        err = printer_config_fprint(&cfg, a, (IoWriter){&gprt_limit_vt, &lw}, fset,
                                    BURROW_ANY(TYPE_AST_FILE_PTR, &file));
        if (lw.err_count > 1) {
            testing_t_fatalf_v(t, "Writes continued after first error returned");
            break;
        }
        /* We expect err_count be 1 iff err is set */
        if ((lw.err_count != 0) != BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "Expected err when errCount != 0");
            break;
        }
    }
    arena_free(&ar);
}

/* TestX is a skeleton test that can be filled in for debugging one-off cases.
 * Do not remove. */
static void TestX(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("\n"
                "package p\n"
                "func _() {}\n");
    Str res = BURROW_STR_EMPTY;
    Error err = gprt_format(a, token_new_file_set(a), src, 0, &res);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s", error_text(err));
    arena_free(&ar);
}

static void TestCommentedNode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str input = S("package main\n"
                  "\n"
                  "func foo() {\n"
                  "\t// comment inside func\n"
                  "}\n"
                  "\n"
                  "// leading comment\n"
                  "type bar int // comment2\n"
                  "\n");

    Str foo = S("func foo() {\n"
                "\t// comment inside func\n"
                "}");

    Str bar = S("// leading comment\n"
                "type bar int\t// comment2\n");

    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, fset, S("input.go"), gprt_src(&input),
                                   gprt_parse_mode, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }

    AstDecl d0 = ((AstDecl *)f->decls.p)[0];
    PrinterCommentedNode cn = {BURROW_ANY(TYPE_AST_DECL, &d0), f->comments};
    PrinterCommentedNode *cnp = &cn;
    Str got = BURROW_STR_EMPTY;
    if (!gprt_print(t, a, &gprt_default, fset,
                    BURROW_ANY(TYPE_PRINTER_COMMENTED_NODE_PTR, &cnp), &got)) {
        arena_free(&ar);
        return;
    }
    if (!str_eq(got, foo))
        testing_t_errorf_v(t, "got %q, want %q", got, foo);

    AstDecl d1 = ((AstDecl *)f->decls.p)[1];
    cn = (PrinterCommentedNode){BURROW_ANY(TYPE_AST_DECL, &d1), f->comments};
    if (!gprt_print(t, a, &gprt_default, fset,
                    BURROW_ANY(TYPE_PRINTER_COMMENTED_NODE_PTR, &cnp), &got)) {
        arena_free(&ar);
        return;
    }
    if (!str_eq(got, bar))
        testing_t_errorf_v(t, "got %q, want %q", got, bar);
    arena_free(&ar);
}

static void TestIssue11151(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package p\t/*\r/1\r*\r/2*\r\r\r\r/3*\r\r+\r\r/4*/\n");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, fset, BURROW_STR_EMPTY, gprt_src(&src),
                                   gprt_parse_mode, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }

    StringsBuilder b = STRINGS_BUILDER(a);
    (void)printer_fprint(a, strings_builder_as_io_writer(&b), fset,
                         BURROW_ANY(TYPE_AST_FILE_PTR, &f));
    Str got = strings_builder_string(&b);
    /* \r following opening slash and star should be stripped */
    Str want = S("package p\t/*/1*\r/2*\r/3*+/4*/\n");
    if (!str_eq(got, want))
        testing_t_errorf_v(t, "\ngot : %q\nwant: %q", got, want);

    /* the resulting program must be valid */
    (void)parser_parse_file(a, fset, BURROW_STR_EMPTY, gprt_src(&got),
                            PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s\norig: %q\ngot : %q", error_text(err), src, got);
    arena_free(&ar);
}

/* If a declaration has multiple specifications, a parenthesized declaration
 * must be printed even if lparen is TOKEN_NO_POS. */
static void TestParenthesizedDecl(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* a package with multiple specs in a single declaration */
    Str src = S("package p; var ( a float64; b int )");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, fset, BURROW_STR_EMPTY, gprt_src(&src),
                                   PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }

    /* print the original package */
    Str original = BURROW_STR_EMPTY;
    if (!gprt_print(t, a, &gprt_default, fset, BURROW_ANY(TYPE_AST_FILE_PTR, &f),
                    &original)) {
        arena_free(&ar);
        return;
    }

    /* now remove parentheses from the declaration */
    for (Int i = 0; i != f->decls.len; i++)
        ((AstGenDecl *)((AstDecl *)f->decls.p)[i])->lparen = TOKEN_NO_POS;
    Str noparen = BURROW_STR_EMPTY;
    if (!gprt_print(t, a, &gprt_default, fset, BURROW_ANY(TYPE_AST_FILE_PTR, &f),
                    &noparen)) {
        arena_free(&ar);
        return;
    }

    if (!str_eq(noparen, original))
        testing_t_errorf_v(t, "got %q, want %q", noparen, original);
    arena_free(&ar);
}

/* Verify that we don't print a newline between "return" and its results, as
 * that would incorrectly cause a naked return. */
static void TestIssue32854(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package foo\n"
                "\n"
                "func f() {\n"
                "        return Composite{\n"
                "                call(),\n"
                "        }\n"
                "}");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *file = parser_parse_file(a, fset, BURROW_STR_EMPTY, gprt_src(&src),
                                      PARSER_SKIP_OBJECT_RESOLUTION, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }

    /* Replace the result with call(), which is on the next line. */
    AstFuncDecl *fd = (AstFuncDecl *)((AstDecl *)file->decls.p)[0];
    AstReturnStmt *ret = (AstReturnStmt *)((AstStmt *)fd->body->list.p)[0];
    AstExpr *results = (AstExpr *)ret->results.p;
    results[0] = ((AstExpr *)((AstCompositeLit *)results[0])->elts.p)[0];

    Str got = BURROW_STR_EMPTY;
    Str want = S("return call()");
    if (gprt_print(t, a, &gprt_default, fset,
                   BURROW_ANY(TYPE_OF(AstReturnStmtPtr), &ret), &got) &&
        !str_eq(got, want))
        testing_t_fatalf_v(t, "got %q, want %q", got, want);
    arena_free(&ar);
}

static void TestSourcePosNewline(TestingT *t) {
    /* We don't provide a syntax for escaping or unescaping characters in line
     * directives (see https://go.dev/issue/24183#issuecomment-372449628). As a
     * result, we cannot write a line directive with the correct path for a
     * filename containing newlines. We should return an error rather than
     * silently dropping or mangling it. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str fname = S("foo\nbar/bar.go");
    Str src = S("package bar");
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, fset, fname, gprt_src(&src),
                                   gprt_parse_mode | PARSER_ALL_ERRORS, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }

    PrinterConfig cfg = {PRINTER_SOURCE_POS, 8, 0}; /* emit line comments */
    StringsBuilder b = STRINGS_BUILDER(a);
    err = printer_config_fprint(&cfg, a, strings_builder_as_io_writer(&b), fset,
                                BURROW_ANY(TYPE_AST_FILE_PTR, &f));
    if (BURROW_OK(err))
        testing_t_errorf_v(
            t, "Fprint did not error for source file path containing newline");
    if (strings_builder_len(&b) != 0)
        testing_t_errorf_v(t, "unexpected Fprint output:\n%s",
                           strings_builder_string(&b));
    arena_free(&ar);
}

/* TestEmptyDecl tests that empty decls for const, var, import are printed with
 * valid syntax e.g "var ()" instead of just "var", which is invalid and cannot
 * be parsed. Issue 63566. */
static void TestEmptyDecl(TestingT *t) {
    static const Token toks[] = {TOKEN_IMPORT, TOKEN_CONST, TOKEN_TYPE_, TOKEN_VAR};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(toks); i++) {
        AstGenDecl *d = (AstGenDecl *)ast_node_new(a, AST_KIND_GEN_DECL);
        d->tok = toks[i];
        StringsBuilder b = STRINGS_BUILDER(a);
        (void)printer_fprint(a, strings_builder_as_io_writer(&b), token_new_file_set(a),
                             BURROW_ANY(TYPE_OF(AstGenDeclPtr), &d));
        Str got = strings_builder_string(&b);
        Str want = fmt_sprintf_v(a, "%s ()", token_string(toks[i], a));
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "got %q, want %q", got, want);
    }
    arena_free(&ar);
}

/* TestIssue7195 checks that go/printer does not add an extra level of
 * indentation when printing a return statement with multiple multi-line
 * composite literals. */
static void TestIssue7195(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package p\n"
                "type T struct{ x int }\n"
                "func _() (T, *T) {\n"
                "\treturn T{\n"
                "\t\t\tx: 1,\n"
                "\t\t}, &T{\n"
                "\t\t\tx: 2,\n"
                "\t\t}\n"
                "}\n");

    Str want = S("package p\n"
                 "\n"
                 "type T struct{ x int }\n"
                 "\n"
                 "func _() (T, *T) {\n"
                 "\treturn T{\n"
                 "\t\tx: 1,\n"
                 "\t}, &T{\n"
                 "\t\tx: 2,\n"
                 "\t}\n"
                 "}\n");

    Str got = BURROW_STR_EMPTY;
    Error err = gprt_format(a, token_new_file_set(a), src, 0, &got);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    else if (!str_eq(got, want))
        testing_t_fatalf_v(t, "got:\n%s\nwant:\n%s\n", got, want);
    arena_free(&ar);
}

/* burrow's own: the error for each kind of value Go's printNode turns away,
 * with the text Go gives for the same value. */
static void TestUnsupported(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    Str s = S("x");
    AstField *field = (AstField *)ast_node_new(a, AST_KIND_FIELD);
    AstExpr nil_expr = NULL;
    Slice decls = {NULL, 0, 0, TYPE_AST_DECL};
    AstCommentGroup *groups[1] = {NULL};
    PrinterCommentedNode cn = {BURROW_ANY(TYPE_AST_DECL_SLICE, &decls),
                               {groups, 0, 1, TYPE_AST_COMMENT_GROUP_PTR}};
    PrinterCommentedNode *cnp = &cn;
    const struct {
        Any node;
        const char *want;
    } tests[] = {
        {{NULL, NULL}, "go/printer: unsupported node type <nil>"},
        {BURROW_ANY(TYPE_STRING, &s), "go/printer: unsupported node type string"},
        {BURROW_ANY(TYPE_AST_FIELD_PTR, &field),
         "go/printer: unsupported node type *ast.Field"},
        {BURROW_ANY(TYPE_AST_EXPR, &nil_expr),
         "go/printer: unsupported node type <nil>"},
        {BURROW_ANY(TYPE_PRINTER_COMMENTED_NODE_PTR, &cnp),
         "go/printer: unsupported node type []ast.Decl"},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        StringsBuilder b = STRINGS_BUILDER(a);
        Error err =
            printer_fprint(a, strings_builder_as_io_writer(&b), fset, tests[i].node);
        Str want = str_from_cstr(tests[i].want);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "%d: no error, want %q", i, want);
        else if (!str_eq(error_text(err), want))
            testing_t_errorf_v(t, "%d: error %q, want %q", i, error_text(err), want);
        if (strings_builder_len(&b) != 0)
            testing_t_errorf_v(t, "%d: wrote %q", i, strings_builder_string(&b));
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestFiles)                                                                       \
    X(TestLineComments)                                                                \
    X(TestInit)                                                                        \
    X(TestBadNodes)                                                                    \
    X(TestBadComments)                                                                 \
    X(TestSourcePos)                                                                   \
    X(TestIssue5945)                                                                   \
    X(TestIssue52605)                                                                  \
    X(TestDeclLists)                                                                   \
    X(TestStmtLists)                                                                   \
    X(TestBaseIndent)                                                                  \
    X(TestFuncType)                                                                    \
    X(TestChanType)                                                                    \
    X(TestWriteErrors)                                                                 \
    X(TestX)                                                                           \
    X(TestCommentedNode)                                                               \
    X(TestIssue11151)                                                                  \
    X(TestParenthesizedDecl)                                                           \
    X(TestIssue32854)                                                                  \
    X(TestSourcePosNewline)                                                            \
    X(TestEmptyDecl)                                                                   \
    X(TestIssue7195)                                                                   \
    X(TestUnsupported)

TESTING_MAIN(TESTS)
