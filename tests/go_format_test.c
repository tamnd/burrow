/* Derived from Go's src/go/format/format_test.go and example_test.go.
 * Go source: go1.27.1.
 *
 * TestNode and TestSource read format_test.go in Go. Here they run over every
 * .go file of src/go/format, from tests/go_format_test_gen.h, format_test.go
 * first. TestExampleNode is ExampleNode, and TestNodeSortsImports and
 * TestSourceErrors are burrow's own, with what they expect taken from go1.26.5.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/go/format.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"

#include <stdint.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "go_format_test_gen.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static const ParserMode gfmt_parse_mode =
    PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION;

/* File f of the generated header, put back together. */
static Str gfmt_file(Alloc *a, const GfmtFile *f) {
    StringsBuilder b = STRINGS_BUILDER(a);
    for (int k = 0; k < f->npieces; k++)
        (void)strings_builder_write_string(
            &b, str_from_bytes((const Byte *)f->pieces[k].p, (Int)f->pieces[k].n),
            NULL);
    return strings_builder_string(&b);
}

/* The bytes of s, shared. */
static Slice gfmt_bytes(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Str gfmt_str(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

/* diff: the first line where dst and src part, or their lengths. */
static void gfmt_diff(TestingT *t, Str name, Str dst, Str src) {
    Int line = 1;
    Int offs = 0; /* line offset */
    for (Int i = 0; i < dst.len && i < src.len; i++) {
        Byte d = dst.p[i];
        Byte s = src.p[i];
        if (d != s) {
            testing_t_errorf_v(t, "%s: dst:%d: %s\n", name, line,
                               str_from_bytes(dst.p + offs, i + 1 - offs));
            testing_t_errorf_v(t, "%s: src:%d: %s\n", name, line,
                               str_from_bytes(src.p + offs, i + 1 - offs));
            return;
        }
        if (s == '\n') {
            line++;
            offs = i + 1;
        }
    }
    if (dst.len != src.len)
        testing_t_errorf_v(t, "%s: len(dst) = %d, len(src) = %d\nsrc = %q", name,
                           dst.len, src.len, src);
}

static void TestNode(TestingT *t) {
    for (Int i = 0; i < NELEM(gfmt_files); i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str name = str_from_cstr(gfmt_files[i].name);
        Str src = gfmt_file(a, &gfmt_files[i]);

        TokenFileSet *fset = token_new_file_set(a);
        Error err = BURROW_NO_ERROR;
        AstFile *file = parser_parse_file(a, fset, name, BURROW_ANY(TYPE_STRING, &src),
                                          gfmt_parse_mode, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: %s", name, error_text(err));
            arena_free(&ar);
            continue;
        }

        StringsBuilder b = STRINGS_BUILDER(a);
        err = format_node(a, strings_builder_as_io_writer(&b), fset,
                          BURROW_ANY(TYPE_AST_FILE_PTR, &file));
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: Node failed: %s", name, error_text(err));
        else
            gfmt_diff(t, name, strings_builder_string(&b), src);
        arena_free(&ar);
    }
}

/* Node is documented to not modify the AST. Test that it is so even when
 * numbers are normalized. */
static void TestNodeNoModify(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package p\n\nconst _ = 0000000123i\n");
    Str golden = S("package p\n\nconst _ = 123i\n");

    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *file =
        parser_parse_file(a, fset, BURROW_STR_EMPTY, BURROW_ANY(TYPE_STRING, &src),
                          gfmt_parse_mode, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }

    /* Capture the original address and value of a BasicLit node which will
     * undergo formatting changes during printing. */
    AstGenDecl *d = ((AstGenDecl **)file->decls.p)[0];
    AstValueSpec *vs = ((AstValueSpec **)d->specs.p)[0];
    AstBasicLit *want_lit = ((AstBasicLit **)vs->values.p)[0];
    Str want_val = want_lit->value;

    StringsBuilder b = STRINGS_BUILDER(a);
    err = format_node(a, strings_builder_as_io_writer(&b), fset,
                      BURROW_ANY(TYPE_AST_FILE_PTR, &file));
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "Node failed: %s", error_text(err));
        arena_free(&ar);
        return;
    }
    gfmt_diff(t, S("NoModify"), strings_builder_string(&b), golden);

    /* check if anything changed after Node returned */
    d = ((AstGenDecl **)file->decls.p)[0];
    vs = ((AstValueSpec **)d->specs.p)[0];
    AstBasicLit *got_lit = ((AstBasicLit **)vs->values.p)[0];
    Str got_val = got_lit->value;

    if (got_lit != want_lit)
        testing_t_errorf_v(t, "got *ast.BasicLit address %p, want %p", (void *)got_lit,
                           (void *)want_lit);
    if (!str_eq(got_val, want_val))
        testing_t_errorf_v(t, "got *ast.BasicLit value %q, want %q", got_val, want_val);
    arena_free(&ar);
}

static void TestSource(TestingT *t) {
    for (Int i = 0; i < NELEM(gfmt_files); i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str name = str_from_cstr(gfmt_files[i].name);
        Str src = gfmt_file(a, &gfmt_files[i]);

        Error err = BURROW_NO_ERROR;
        Slice res = format_source(a, gfmt_bytes(src), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: Source failed: %s", name, error_text(err));
        else
            gfmt_diff(t, name, gfmt_str(res), src);
        arena_free(&ar);
    }
}

/* Test cases that are expected to fail are marked by the prefix "ERROR". The
 * formatted result must look the same as the input for successful tests. */
static const char *const gfmt_tests[] = {
    /* declaration lists */
    "import \"go/format\"",
    "var x int",
    "var x int\n\ntype T struct{}",

    /* statement lists */
    "x := 0",
    "f(a, b, c)\nvar x int = f(1, 2, 3)",

    /* indentation, leading and trailing space */
    "\tx := 0\n\tgo f()",
    "\tx := 0\n\tgo f()\n\n\n",
    "\n\t\t\n\n\tx := 0\n\tgo f()\n\n\n",
    "\n\t\t\n\n\t\t\tx := 0\n\t\t\tgo f()\n\n\n",
    /* no indentation added inside raw strings */
    "\n\t\t\n\n\t\t\tx := 0\n\t\t\tconst s = `\nfoo\n`\n\n\n",
    /* no indentation removed inside raw strings */
    "\n\t\t\n\n\t\t\tx := 0\n\t\t\tconst s = `\n\t\tfoo\n`\n\n\n",

    /* comments */
    "/* Comment */",
    "\t/* Comment */ ",
    "\n/* Comment */ ",
    "i := 5 /* Comment */",         /* issue #5551 */
    "\ta()\n//line :1",             /* issue #11276 */
    "\t//xxx\n\ta()\n//line :2",    /* issue #11276 */
    "\ta() //line :1\n\tb()\n",     /* issue #11276 */
    "x := 0\n//line :1\n//line :2", /* issue #11276 */

    /* whitespace */
    "",     /* issue #11275 */
    " ",    /* issue #11275 */
    "\t",   /* issue #11275 */
    "\t\t", /* issue #11275 */
    "\n",   /* issue #11275 */
    "\n\n", /* issue #11275 */
    "\t\n", /* issue #11275 */

    /* erroneous programs */
    "ERROR1 + 2 +",
    "ERRORx :=  0",

    /* build comments */
    "// copyright\n\n//go:build x\n\npackage p\n",
    "// copyright\n\n//go:build x\n// +build x\n\npackage p\n",
};

static void TestPartial(TestingT *t) {
    for (Int i = 0; i < NELEM(gfmt_tests); i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str src = str_from_cstr(gfmt_tests[i]);
        bool want_err = strings_has_prefix(src, S("ERROR"));
        if (want_err)
            src = str_from_bytes(src.p + 5, src.len - 5); /* remove the ERROR prefix */

        Error err = BURROW_NO_ERROR;
        Str res = gfmt_str(format_source(a, gfmt_bytes(src), &err));
        if (want_err) {
            /* test expected to fail */
            if (BURROW_OK(err) && str_eq(res, src))
                testing_t_errorf_v(
                    t, "formatting succeeded but was expected to fail:\n%q", src);
        } else if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "formatting failed (%s):\n%q", error_text(err), src);
        } else if (!str_eq(res, src)) {
            testing_t_errorf_v(t, "formatting incorrect:\nsource: %q\nresult: %q", src,
                               res);
        }
        arena_free(&ar);
    }
}

/* ExampleNode. */
static void TestExampleNode(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    AstExpr node = parser_parse_expr(a, S("(6+2*3)/4"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }
    /* a FileSet for node, empty since node does not come from a source file */
    TokenFileSet *fset = token_new_file_set(a);
    StringsBuilder b = STRINGS_BUILDER(a);
    err = format_node(a, strings_builder_as_io_writer(&b), fset,
                      BURROW_ANY(TYPE_AST_EXPR, &node));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s", error_text(err));
    Str got = strings_builder_string(&b);
    if (!str_eq(got, S("(6 + 2*3) / 4")))
        testing_t_errorf_v(t, "got %q, want %q", got, S("(6 + 2*3) / 4"));
    arena_free(&ar);
}

/* A grouped import block is sorted in what Node writes, through a copy, so
 * the file given to it keeps its order. Source sorts the file it parses. */
static void TestNodeSortsImports(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package p\n\nimport (\n\t\"os\"\n\t\"fmt\" // f\n\n\t\"bytes\"\n"
                "\t\"errors\"\n)\n\n// x\nvar _ = fmt.Println\nvar _ = os.Exit\n");
    Str want = S("package p\n\nimport (\n\t\"fmt\" // f\n\t\"os\"\n\n\t\"bytes\"\n"
                 "\t\"errors\"\n)\n\n// x\nvar _ = fmt.Println\nvar _ = os.Exit\n");

    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *file =
        parser_parse_file(a, fset, BURROW_STR_EMPTY, BURROW_ANY(TYPE_STRING, &src),
                          gfmt_parse_mode, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        arena_free(&ar);
        return;
    }

    StringsBuilder b = STRINGS_BUILDER(a);
    err = format_node(a, strings_builder_as_io_writer(&b), fset,
                      BURROW_ANY(TYPE_AST_FILE_PTR, &file));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Node: %s", error_text(err));
    gfmt_diff(t, S("Node"), strings_builder_string(&b), want);

    AstImportSpec *first = ((AstImportSpec **)file->imports.p)[0];
    if (!str_eq(first->path->value, S("\"os\"")))
        testing_t_errorf_v(t, "first import after Node is %s, want \"os\"",
                           first->path->value);

    PrinterCommentedNode cn = {BURROW_ANY(TYPE_AST_FILE_PTR, &file), file->comments};
    PrinterCommentedNode *cnp = &cn;
    b = STRINGS_BUILDER(a);
    err = format_node(a, strings_builder_as_io_writer(&b), fset,
                      BURROW_ANY(TYPE_PRINTER_COMMENTED_NODE_PTR, &cnp));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Node of a CommentedNode: %s", error_text(err));
    gfmt_diff(t, S("CommentedNode"), strings_builder_string(&b), want);

    Slice res = format_source(a, gfmt_bytes(src), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Source: %s", error_text(err));
    gfmt_diff(t, S("Source"), gfmt_str(res), want);

    int x = 42;
    b = STRINGS_BUILDER(a);
    err = format_node(a, strings_builder_as_io_writer(&b), fset,
                      BURROW_ANY(TYPE_INT, &x));
    Str msg = error_text(err);
    if (!str_eq(msg, S("go/printer: unsupported node type int")))
        testing_t_errorf_v(t, "Node of an int: got error %q", msg);
    if (strings_builder_len(&b) != 0)
        testing_t_errorf_v(t, "Node of an int wrote %q", strings_builder_string(&b));
    arena_free(&ar);
}

/* What Source makes of fragments, errors included. */
static void TestSourceErrors(TestingT *t) {
    static const struct {
        const char *src, *want, *err;
    } tests[] = {
        {"1 + 2 +", NULL, "3:1: expected operand, found '}'"},
        {"x :=  0", "x := 0", NULL},
        {"package p\nfunc", NULL, "2:5: expected 'IDENT', found 'EOF'"},
        {"func f() {", NULL, "1:21: expected '}', found 'EOF'"},
        {"  \n\tvar x = 0X1P4\n  ", "  \n\tvar x = 0x1p4\n  ", NULL},
        {"  x  :=  1 /* c */ ;y:=2\n", "\tx := 1 /* c */\n\ty := 2\n", NULL},
    };
    for (Int i = 0; i < NELEM(tests); i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str src = str_from_cstr(tests[i].src);
        Error err = BURROW_NO_ERROR;
        Slice res = format_source(a, gfmt_bytes(src), &err);
        if (tests[i].err != NULL) {
            if (!str_eq(error_text(err), str_from_cstr(tests[i].err)))
                testing_t_errorf_v(t, "%q: got error %q, want %q", src, error_text(err),
                                   str_from_cstr(tests[i].err));
            if (!slice_is_nil(res))
                testing_t_errorf_v(t, "%q: got %q with the error, want nil", src,
                                   gfmt_str(res));
        } else if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%q: %s", src, error_text(err));
        } else if (!str_eq(gfmt_str(res), str_from_cstr(tests[i].want))) {
            testing_t_errorf_v(t, "%q: got %q, want %q", src, gfmt_str(res),
                               str_from_cstr(tests[i].want));
        }
        arena_free(&ar);
    }
}

#define TESTS(X)                                                                       \
    X(TestNode)                                                                        \
    X(TestNodeNoModify)                                                                \
    X(TestSource)                                                                      \
    X(TestPartial)                                                                     \
    X(TestExampleNode)                                                                 \
    X(TestNodeSortsImports)                                                            \
    X(TestSourceErrors)

TESTING_MAIN(TESTS)
