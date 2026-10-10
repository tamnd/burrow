/* Derived from Go's src/go/doc/doc_test.go, comment_test.go, example_test.go,
 * example_internal_test.go and synopsis_test.go.
 * Go source: go1.27.1.
 *
 * Test renders each package under testdata the way testdata/template.txt
 * does, in C, and compares it with the golden file for the mode. TestExamples
 * reads the txtar goldens of testdata/examples with a small parser of its own.
 * Go's ExampleNewFromFiles is TestExampleNewFromFiles. Everything read from
 * disk in Go is in tests/go_doc_test_gen.h.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/go/doc.h"
#include "burrow/go/format.h"
#include "burrow/go/printer.h"
#include "burrow/mem/arena.h"
#include "burrow/sort.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"

#include <stdint.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "go_doc_test_gen.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

/* Exported from src/go/doc.c for these tests, unexported in Go. */
Str burrow__doc_first_sentence(Str s);
Slice burrow__doc_find_import_group_starts1(Alloc *a, Slice orig_imps);

static Str gdoc_cstr(const char *s) {
    return str_from_cstr(s);
}

/* s[i:j], without a NULL + 0 for an empty s. */
static Str gdoc_sub(Str s, Int i, Int j) {
    if (i == j)
        return BURROW_STR_EMPTY;
    return str_from_bytes(s.p + i, j - i);
}

static Str gdoc_concat(Alloc *a, Str x, Str y) {
    StringsBuilder b = STRINGS_BUILDER(a);
    (void)strings_builder_write_string(&b, x, NULL);
    (void)strings_builder_write_string(&b, y, NULL);
    return strings_builder_string(&b);
}

/* The file of the generated header put back together. */
static Str gdoc_join(Alloc *a, const GdocFile *f) {
    StringsBuilder b = STRINGS_BUILDER(a);
    for (int k = 0; k < f->npieces; k++)
        (void)strings_builder_write_string(
            &b, str_from_bytes(f->pieces[k].p, (Int)f->pieces[k].n), NULL);
    return strings_builder_string(&b);
}

static bool gdoc_file(Alloc *a, Str name, Str *out) {
    for (Int i = 0; i < NELEM(gdoc_files); i++) {
        if (str_eq(gdoc_cstr(gdoc_files[i].name), name)) {
            *out = gdoc_join(a, &gdoc_files[i]);
            return true;
        }
    }
    *out = BURROW_STR_EMPTY;
    return false;
}

static AstFile *gdoc_parse(TestingT *t, Alloc *a, TokenFileSet *fset, Str name, Str src,
                           ParserMode mode) {
    Error err = BURROW_NO_ERROR;
    AstFile *f =
        parser_parse_file(a, fset, name, BURROW_ANY(TYPE_STRING, &src), mode, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: %s", name, error_text(err));
        return NULL;
    }
    return f;
}

static Slice gdoc_files_of(Alloc *a, AstFile *f1, AstFile *f2) {
    Slice files = slice_make(a, TYPE_AST_FILE_PTR, 0, 2);
    files = slice_append(a, files, &f1, 1);
    return slice_append(a, files, &f2, 1);
}

static void gdoc_put(StringsBuilder *b, Str s) {
    (void)strings_builder_write_string(b, s, NULL);
}

/* synopsisFmt of doc_test.go. */
static void gdoc_synopsis_fmt(Alloc *a, StringsBuilder *b, Str s) {
    enum { N = 64 };
    if (s.len > N) {
        s = gdoc_sub(s, 0, N);
        Int i = strings_last_index_any(s, S("\t\n "));
        if (i >= 0)
            s = gdoc_sub(s, 0, i);
        s = gdoc_concat(a, strings_trim_space(s), S(" ..."));
    }
    gdoc_put(b, S("// "));
    gdoc_put(b, strings_replace_all(a, s, S("\n"), S(" ")));
}

/* nodeFmt of doc_test.go. */
static bool gdoc_node_fmt(TestingT *t, Alloc *a, StringsBuilder *b, TokenFileSet *fset,
                          Any node) {
    StringsBuilder buf = STRINGS_BUILDER(a);
    Error err = printer_fprint(a, strings_builder_as_io_writer(&buf), fset, node);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "printer_fprint: %s", error_text(err));
        return false;
    }
    Str s = strings_trim_space(strings_builder_string(&buf));
    gdoc_put(b, strings_replace_all(a, s, S("\n"), S("\n\t")));
    return true;
}

/* indentFmt of doc_test.go. */
static void gdoc_indent_fmt(Alloc *a, StringsBuilder *b, Str indent, Str s) {
    Str end = BURROW_STR_EMPTY;
    if (strings_has_suffix(s, S("\n"))) {
        end = S("\n");
        s = gdoc_sub(s, 0, s.len - 1);
    }
    gdoc_put(b, indent);
    gdoc_put(b, strings_replace_all(a, s, S("\n"), gdoc_concat(a, S("\n"), indent)));
    gdoc_put(b, end);
}

/* One {{synopsis .Doc}} and {{node .Decl $.FSet}} entry of the template. */
static bool gdoc_entry(TestingT *t, Alloc *a, StringsBuilder *b, TokenFileSet *fset,
                       Str doc, Any decl) {
    gdoc_put(b, S("\t"));
    gdoc_synopsis_fmt(a, b, doc);
    gdoc_put(b, S("\n\t"));
    if (!gdoc_node_fmt(t, a, b, fset, decl))
        return false;
    gdoc_put(b, S("\n\n"));
    return true;
}

static bool gdoc_values(TestingT *t, Alloc *a, StringsBuilder *b, TokenFileSet *fset,
                        Slice values) {
    for (Int i = 0; i < values.len; i++) {
        DocValue *v = BURROW_AT(DocValue *, values, i);
        if (!gdoc_entry(t, a, b, fset, v->doc,
                        BURROW_ANY(TYPE_OF(AstGenDeclPtr), &v->decl)))
            return false;
    }
    return true;
}

static bool gdoc_funcs(TestingT *t, Alloc *a, StringsBuilder *b, TokenFileSet *fset,
                       Slice funcs) {
    for (Int i = 0; i < funcs.len; i++) {
        DocFunc *f = BURROW_AT(DocFunc *, funcs, i);
        if (!gdoc_entry(t, a, b, fset, f->doc,
                        BURROW_ANY(TYPE_OF(AstFuncDeclPtr), &f->decl)))
            return false;
    }
    return true;
}

/* testdata/template.txt, executed with p and fset. */
static bool gdoc_render(TestingT *t, Alloc *a, StringsBuilder *b, TokenFileSet *fset,
                        DocPackage *p) {
    gdoc_synopsis_fmt(a, b, p->doc);
    gdoc_put(b, S("\nPACKAGE "));
    gdoc_put(b, p->name);
    gdoc_put(b, S("\n\nIMPORTPATH\n\t"));
    gdoc_put(b, p->import_path);
    gdoc_put(b, S("\n\n"));
    if (p->imports.len > 0) {
        gdoc_put(b, S("IMPORTS\n"));
        for (Int i = 0; i < p->imports.len; i++) {
            gdoc_put(b, S("\t"));
            gdoc_put(b, BURROW_AT(Str, p->imports, i));
            gdoc_put(b, S("\n"));
        }
        gdoc_put(b, S("\n"));
    }
    gdoc_put(b, S("FILENAMES\n"));
    for (Int i = 0; i < p->filenames.len; i++) {
        gdoc_put(b, S("\t"));
        gdoc_put(b, BURROW_AT(Str, p->filenames, i));
        gdoc_put(b, S("\n"));
    }
    if (p->consts.len > 0) {
        gdoc_put(b, S("\nCONSTANTS\n"));
        if (!gdoc_values(t, a, b, fset, p->consts))
            return false;
    }
    if (p->vars.len > 0) {
        gdoc_put(b, S("\nVARIABLES\n"));
        if (!gdoc_values(t, a, b, fset, p->vars))
            return false;
    }
    if (p->funcs.len > 0) {
        gdoc_put(b, S("\nFUNCTIONS\n"));
        if (!gdoc_funcs(t, a, b, fset, p->funcs))
            return false;
    }
    if (p->types.len > 0) {
        gdoc_put(b, S("\nTYPES\n"));
        for (Int i = 0; i < p->types.len; i++) {
            DocType *ty = BURROW_AT(DocType *, p->types, i);
            if (!gdoc_entry(t, a, b, fset, ty->doc,
                            BURROW_ANY(TYPE_OF(AstGenDeclPtr), &ty->decl)) ||
                !gdoc_values(t, a, b, fset, ty->consts) ||
                !gdoc_values(t, a, b, fset, ty->vars) ||
                !gdoc_funcs(t, a, b, fset, ty->funcs) ||
                !gdoc_funcs(t, a, b, fset, ty->methods))
                return false;
        }
    }
    if (p->bugs.len > 0) {
        gdoc_put(b, S("\nBUGS .Bugs is now deprecated, please use .Notes instead\n"));
        for (Int i = 0; i < p->bugs.len; i++) {
            gdoc_indent_fmt(a, b, S("\t"), BURROW_AT(Str, p->bugs, i));
            gdoc_put(b, S("\n"));
        }
    }
    if (p->notes != NULL && map_len(p->notes) > 0) {
        /* range over a map goes in key order */
        Slice markers = slice_make(a, TYPE_STRING, 0, map_len(p->notes));
        const void *k = NULL;
        void *v = NULL;
        for (MapIter it = map_iter(p->notes); map_next(&it, &k, &v);)
            markers = slice_append(a, markers, k, 1);
        sort_strings(markers);
        for (Int i = 0; i < markers.len; i++) {
            Str marker = BURROW_AT(Str, markers, i);
            Slice *notes = map_get(p->notes, &marker);
            gdoc_put(b, S("\n"));
            gdoc_put(b, marker);
            gdoc_put(b, S("S\n"));
            for (Int j = 0; notes != NULL && j < notes->len; j++) {
                DocNote *n = BURROW_AT(DocNote *, *notes, j);
                gdoc_put(b, marker);
                gdoc_put(b, S("("));
                gdoc_put(b, n->uid);
                gdoc_put(b, S(")"));
                gdoc_indent_fmt(a, b, S("\t"), n->body);
                gdoc_put(b, S("\n"));
            }
        }
    }
    return true;
}

/* The package clause of a file under testdata, which is what ParseDir groups
 * the files by. */
static Str gdoc_package_of(Alloc *a, Str src) {
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    AstFile *f = parser_parse_file(a, fset, S("x.go"), BURROW_ANY(TYPE_STRING, &src),
                                   PARSER_PACKAGE_CLAUSE_ONLY, &err);
    if (BURROW_FAILED(err) || f == NULL)
        return BURROW_STR_EMPTY;
    return f->name->name;
}

static void gdoc_test_mode(TestingT *t, DocMode mode) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* The packages, by the first file of each, in the order of the files. */
    Str pkgs[64];
    int npkgs = 0;
    for (Int i = 0; i < NELEM(gdoc_files); i++) {
        Str name = gdoc_cstr(gdoc_files[i].name);
        if (!strings_has_suffix(name, S(".go")) || strings_index(name, S("/")) >= 0)
            continue;
        Str pkg = gdoc_package_of(a, gdoc_join(a, &gdoc_files[i]));
        bool seen = false;
        for (int k = 0; k < npkgs; k++)
            seen = seen || str_eq(pkgs[k], pkg);
        if (!seen && npkgs < NELEM(pkgs))
            pkgs[npkgs++] = pkg;
    }
    if (npkgs < 20)
        testing_t_errorf_v(t, "found %d packages in testdata", npkgs);

    for (int k = 0; k < npkgs; k++) {
        TokenFileSet *fset = token_new_file_set(a);
        Slice files = slice_make(a, TYPE_AST_FILE_PTR, 0, 4);
        bool ok = true;
        for (Int i = 0; i < NELEM(gdoc_files); i++) {
            Str name = gdoc_cstr(gdoc_files[i].name);
            if (!strings_has_suffix(name, S(".go")) || strings_index(name, S("/")) >= 0)
                continue;
            Str src = gdoc_join(a, &gdoc_files[i]);
            if (!str_eq(gdoc_package_of(a, src), pkgs[k]))
                continue;
            AstFile *f = gdoc_parse(t, a, fset, gdoc_concat(a, S("testdata/"), name),
                                    src, PARSER_PARSE_COMMENTS);
            if (f == NULL) {
                ok = false;
                break;
            }
            files = slice_append(a, files, &f, 1);
        }
        if (!ok)
            continue;
        Error err = BURROW_NO_ERROR;
        DocPackage *p = doc_new_from_files(
            a, fset, files, gdoc_concat(a, S("testdata/"), pkgs[k]), mode, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: %s", pkgs[k], error_text(err));
            continue;
        }
        StringsBuilder b = STRINGS_BUILDER(a);
        if (!gdoc_render(t, a, &b, fset, p))
            continue;
        Str got = strings_builder_string(&b);

        Str golden = fmt_sprintf_v(a, "%s.%d.golden", pkgs[k], (int)mode);
        Str want = BURROW_STR_EMPTY;
        if (!gdoc_file(a, golden, &want)) {
            testing_t_errorf_v(t, "no golden file %s", golden);
            continue;
        }
        if (!str_eq(got, want))
            testing_t_errorf_v(t, "package %s, mode %d\n\tgot:\n%s\n\twant:\n%s",
                               pkgs[k], (int)mode, got, want);
    }
    arena_free(&ar);
}

static void gdoc_test_default(void *env, TestingT *t) {
    (void)env;
    gdoc_test_mode(t, 0);
}

static void gdoc_test_all_decls(void *env, TestingT *t) {
    (void)env;
    gdoc_test_mode(t, DOC_ALL_DECLS);
}

static void gdoc_test_all_methods(void *env, TestingT *t) {
    (void)env;
    gdoc_test_mode(t, DOC_ALL_METHODS);
}

static void Test(TestingT *t) {
    testing_t_run(t, S("default"), BURROW_FN(TestingTFunc, gdoc_test_default, NULL));
    testing_t_run(t, S("AllDecls"), BURROW_FN(TestingTFunc, gdoc_test_all_decls, NULL));
    testing_t_run(t, S("AllMethods"),
                  BURROW_FN(TestingTFunc, gdoc_test_all_methods, NULL));
}

static bool gdoc_func_eq(TestingT *t, const char *msg, DocFunc *got,
                         const GdocFunc *want) {
    if (str_eq(got->doc, BURROW_STR_EMPTY) &&
        str_eq(got->name, gdoc_cstr(want->name)) &&
        str_eq(got->recv, gdoc_cstr(want->recv)) &&
        str_eq(got->orig, gdoc_cstr(want->orig)) && got->level == want->level)
        return true;
    testing_t_errorf_v(t,
                       "%s:\ngot  {Doc:%q Name:%s Recv:%s Orig:%s Level:%d}\nwant "
                       "{Doc: Name:%s Recv:%s Orig:%s Level:%d}",
                       msg, got->doc, got->name, got->recv, got->orig, got->level,
                       want->name, want->recv, want->orig, (Int)want->level);
    return false;
}

/* The funcsPackage rows of type and kind. */
static void gdoc_compare_funcs(TestingT *t, const char *what, Slice got,
                               const char *type, const char *kind) {
    Int n = 0;
    for (Int i = 0; i < NELEM(gdoc_funcs_package); i++) {
        const GdocFunc *w = &gdoc_funcs_package[i];
        if (strcmp(w->type, type) != 0 || strcmp(w->kind, kind) != 0)
            continue;
        if (n < got.len)
            (void)gdoc_func_eq(t, what, BURROW_AT(DocFunc *, got, n), w);
        n++;
    }
    if (got.len != n)
        testing_t_errorf_v(t, "%s %s: got %d, want %d", type, what, got.len, n);
}

static void TestFuncs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *file =
        gdoc_parse(t, a, fset, S("funcs.go"), gdoc_cstr(gdoc_funcs_test_file),
                   PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION);
    if (file == NULL) {
        arena_free(&ar);
        return;
    }
    Slice files = slice_make(a, TYPE_AST_FILE_PTR, 0, 1);
    files = slice_append(a, files, &file, 1);
    Error err = BURROW_NO_ERROR;
    DocPackage *p = doc_new_from_files(a, fset, files, S("importPath"), 0, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "doc_new_from_files: %s", error_text(err));
        arena_free(&ar);
        return;
    }
    gdoc_compare_funcs(t, "Funcs", p->funcs, "", "f");
    if (p->types.len != NELEM(gdoc_funcs_types))
        testing_t_errorf_v(t, "Types: got %d, want %d", p->types.len,
                           NELEM(gdoc_funcs_types));
    for (Int i = 0; i < p->types.len && i < NELEM(gdoc_funcs_types); i++) {
        DocType *ty = BURROW_AT(DocType *, p->types, i);
        const char *want = gdoc_funcs_types[i];
        if (!str_eq(ty->name, gdoc_cstr(want))) {
            testing_t_errorf_v(t, "Types[%d].Name: got %q, want %q", i, ty->name,
                               gdoc_cstr(want));
            continue;
        }
        gdoc_compare_funcs(t, "Funcs", ty->funcs, want, "f");
        gdoc_compare_funcs(t, "Methods", ty->methods, want, "m");
    }
    arena_free(&ar);
}

/* One section of a txtar archive. */
typedef struct GdocSection {
    Str name;
    Str data;
} GdocSection;

/* The sections of the txtar archive s, which start at lines "-- NAME --". */
static int gdoc_txtar(Str s, GdocSection *out, int max) {
    int n = 0;
    Int i = 0, start = 0;
    while (i < s.len) {
        Int eol = strings_index(gdoc_sub(s, i, s.len), S("\n"));
        Int next = eol < 0 ? s.len : i + eol + 1;
        Str line = gdoc_sub(s, i, eol < 0 ? s.len : i + eol);
        if (line.len >= 6 && strings_has_prefix(line, S("-- ")) &&
            strings_has_suffix(line, S(" --"))) {
            if (n > 0)
                out[n - 1].data = gdoc_sub(s, start, i);
            if (n == max)
                return n;
            out[n].name = strings_trim_space(gdoc_sub(line, 3, line.len - 3));
            out[n].data = BURROW_STR_EMPTY;
            start = next;
            n++;
        }
        i = next;
    }
    if (n > 0)
        out[n - 1].data = gdoc_sub(s, start, s.len);
    return n;
}

static Str gdoc_format_file(TestingT *t, Alloc *a, TokenFileSet *fset, AstFile *f) {
    if (f == NULL)
        return S("<nil>");
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err = format_node(a, strings_builder_as_io_writer(&b), fset,
                            BURROW_ANY(TYPE_AST_FILE_PTR, &f));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "format_node: %s", error_text(err));
    return strings_builder_string(&b);
}

static void gdoc_example_file(void *env, TestingT *t) {
    Str filename = *(Str *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    Str src = BURROW_STR_EMPTY;
    (void)gdoc_file(a, filename, &src);
    /* requires object resolution */
    AstFile *f = gdoc_parse(t, a, fset, gdoc_concat(a, S("testdata/"), filename), src,
                            PARSER_PARSE_COMMENTS);
    Str archive = BURROW_STR_EMPTY;
    Str golden = gdoc_concat(a, strings_trim_suffix(filename, S(".go")), S(".golden"));
    if (f == NULL || !gdoc_file(a, golden, &archive)) {
        if (f != NULL)
            testing_t_errorf_v(t, "no golden file %s", golden);
        arena_free(&ar);
        return;
    }
    GdocSection sections[64];
    int nsec = gdoc_txtar(archive, sections, NELEM(sections));
    for (int i = 0; i < nsec; i++)
        sections[i].data = strings_trim_space(sections[i].data);

    Slice test_files = slice_make(a, TYPE_AST_FILE_PTR, 0, 1);
    test_files = slice_append(a, test_files, &f, 1);
    Slice examples = doc_examples(a, test_files);

    /* Every example against its sections, a missing one being empty. */
    for (Int i = 0; i < examples.len; i++) {
        DocExample *e = BURROW_AT(DocExample *, examples, i);
        Str play = S("Play"), output = S("Output");
        for (int kind = 0; kind < 2; kind++) {
            Str key =
                gdoc_concat(a, gdoc_concat(a, e->name, S(".")), kind ? output : play);
            Str want = BURROW_STR_EMPTY;
            for (int k = 0; k < nsec; k++)
                if (str_eq(sections[k].name, key))
                    want = sections[k].data;
            Str got = kind ? strings_trim_space(e->output)
                           : strings_trim_space(gdoc_format_file(t, a, fset, e->play));
            if (!str_eq(got, want))
                testing_t_errorf_v(t, "%s mismatch:\nwant:\n%s\ngot:\n%s", key, want,
                                   got);
        }
    }
    /* And every section names an example there is. */
    for (int k = 0; k < nsec; k++) {
        bool found = false;
        Str after = BURROW_STR_EMPTY;
        Str name = strings_cut(sections[k].name, S("."), &after, &found);
        if (!found) {
            testing_t_errorf_v(t, "bad section name %q, want EXAMPLE_NAME.KIND",
                               sections[k].name);
            continue;
        }
        bool have = false;
        for (Int i = 0; i < examples.len; i++)
            have = have || str_eq(BURROW_AT(DocExample *, examples, i)->name, name);
        if (!have)
            testing_t_errorf_v(t, "no example named %q", name);
        if (!str_eq(after, S("Play")) && !str_eq(after, S("Output")))
            testing_t_errorf_v(t, "bad section kind %q", after);
    }
    arena_free(&ar);
}

static void TestExamples(TestingT *t) {
    Str names[64];
    int n = 0;
    for (Int i = 0; i < NELEM(gdoc_files) && n < NELEM(names); i++) {
        Str name = gdoc_cstr(gdoc_files[i].name);
        if (strings_has_prefix(name, S("examples/")) &&
            strings_has_suffix(name, S(".go")))
            names[n++] = name;
    }
    if (n < 10)
        testing_t_errorf_v(t, "found %d example files", n);
    for (int i = 0; i < n; i++) {
        Str base = strings_trim_suffix(gdoc_sub(names[i], 9, names[i].len), S(".go"));
        testing_t_run(t, base, BURROW_FN(TestingTFunc, gdoc_example_file, &names[i]));
    }
}

static void TestExampleNewFromFiles(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const ParserMode mode = PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION;
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *src =
        gdoc_parse(t, a, fset, S("src.go"), gdoc_cstr(gdoc_example_src), mode);
    AstFile *test =
        gdoc_parse(t, a, fset, S("src_test.go"), gdoc_cstr(gdoc_example_test), mode);
    if (src == NULL || test == NULL) {
        arena_free(&ar);
        return;
    }
    Error err = BURROW_NO_ERROR;
    DocPackage *p = doc_new_from_files(a, fset, gdoc_files_of(a, src, test),
                                       S("example.com/p"), 0, &err);
    if (BURROW_FAILED(err) || p->funcs.len < 1 ||
        BURROW_AT(DocFunc *, p->funcs, 0)->examples.len < 1) {
        testing_t_errorf_v(t, "doc_new_from_files: %s", error_text(err));
        arena_free(&ar);
        return;
    }
    DocFunc *f = BURROW_AT(DocFunc *, p->funcs, 0);
    DocExample *e = BURROW_AT(DocExample *, f->examples, 0);
    Str got = fmt_sprintf_v(a,
                            "package %s - %sfunc %s - %s \342\244\267 example with "
                            "suffix %q - %s",
                            p->name, p->doc, f->name, f->doc, e->suffix, e->doc);
    Str want = gdoc_cstr(gdoc_example_output);
    if (!str_eq(strings_trim_space(got), strings_trim_space(want)))
        testing_t_errorf_v(t, "got:\n%s\nwant:\n%s", got, want);
    arena_free(&ar);
}

static void gdoc_check_class(TestingT *t, Alloc *a, Str id, Slice examples) {
    const GdocClass *want = NULL;
    for (Int i = 0; i < NELEM(gdoc_classify_want); i++)
        if (str_eq(gdoc_cstr(gdoc_classify_want[i].id), id))
            want = &gdoc_classify_want[i];
    Int nwant = want == NULL ? 0 : want->n;
    bool same = examples.len == nwant;
    for (Int i = 0; same && i < nwant; i++)
        same = str_eq(BURROW_AT(DocExample *, examples, i)->suffix,
                      gdoc_cstr(want->suffixes[i]));
    if (same)
        return;
    StringsBuilder b = STRINGS_BUILDER(a);
    for (Int i = 0; i < examples.len; i++) {
        gdoc_put(&b, strconv_quote(a, BURROW_AT(DocExample *, examples, i)->suffix));
        gdoc_put(&b, S(" "));
    }
    testing_t_errorf_v(t, "classification mismatch for %q:\ngot  [%s]\nwant %d of them",
                       id, strings_builder_string(&b), nwant);
}

static void TestClassifyExamples(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const ParserMode mode = PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION;
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *src =
        gdoc_parse(t, a, fset, S("src.go"), gdoc_cstr(gdoc_classify_src), mode);
    AstFile *test =
        gdoc_parse(t, a, fset, S("src_test.go"), gdoc_cstr(gdoc_classify_test), mode);
    if (src == NULL || test == NULL) {
        arena_free(&ar);
        return;
    }
    Error err = BURROW_NO_ERROR;
    DocPackage *p = doc_new_from_files(a, fset, gdoc_files_of(a, src, test),
                                       S("example.com/p"), 0, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "doc_new_from_files: %s", error_text(err));
        arena_free(&ar);
        return;
    }

    /* Every id there is, checked against want, where a missing one has no
     * examples, and every id of want is one there is. */
    Slice ids = slice_make(a, TYPE_STRING, 0, 16);
    Str empty = BURROW_STR_EMPTY;
    ids = slice_append(a, ids, &empty, 1);
    gdoc_check_class(t, a, empty, p->examples);
    for (Int i = 0; i < p->funcs.len; i++) {
        DocFunc *f = BURROW_AT(DocFunc *, p->funcs, i);
        ids = slice_append(a, ids, &f->name, 1);
        gdoc_check_class(t, a, f->name, f->examples);
    }
    for (Int i = 0; i < p->types.len; i++) {
        DocType *ty = BURROW_AT(DocType *, p->types, i);
        ids = slice_append(a, ids, &ty->name, 1);
        gdoc_check_class(t, a, ty->name, ty->examples);
        for (Int j = 0; j < ty->funcs.len; j++) {
            DocFunc *f = BURROW_AT(DocFunc *, ty->funcs, j);
            ids = slice_append(a, ids, &f->name, 1);
            gdoc_check_class(t, a, f->name, f->examples);
        }
        for (Int j = 0; j < ty->methods.len; j++) {
            DocFunc *m = BURROW_AT(DocFunc *, ty->methods, j);
            Str id = gdoc_concat(a, gdoc_concat(a, ty->name, S(".")), m->name);
            ids = slice_append(a, ids, &id, 1);
            gdoc_check_class(t, a, id, m->examples);
        }
    }
    for (Int i = 0; i < NELEM(gdoc_classify_want); i++) {
        Str id = gdoc_cstr(gdoc_classify_want[i].id);
        bool found = false;
        for (Int j = 0; j < ids.len; j++)
            found = found || str_eq(BURROW_AT(Str, ids, j), id);
        if (!found)
            testing_t_errorf_v(t, "did not find %q", id);
    }
    arena_free(&ar);
}

static void gdoc_import_groups_case(void *env, TestingT *t) {
    const GdocImportGroups *c = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    AstFile *file = gdoc_parse(t, a, fset, S("test.go"), gdoc_cstr(c->in),
                               PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION);
    if (file == NULL) {
        arena_free(&ar);
        return;
    }
    Slice imps = burrow__doc_find_import_group_starts1(a, file->imports);
    bool same = imps.len == c->n;
    StringsBuilder b = STRINGS_BUILDER(a);
    for (Int i = 0; i < imps.len; i++) {
        AstImportSpec *imp = BURROW_AT(AstImportSpec *, imps, i);
        Error err = BURROW_NO_ERROR;
        Str got = strconv_unquote(a, imp->path->value, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s", error_text(err));
            arena_free(&ar);
            return;
        }
        if (i < c->n)
            same = same && str_eq(got, gdoc_cstr(c->want[i]));
        gdoc_put(&b, got);
        gdoc_put(&b, S(" "));
    }
    if (!same)
        testing_t_errorf_v(t, "got [%s], want %d paths, the first %q",
                           strings_builder_string(&b), (Int)c->n,
                           gdoc_cstr(c->n > 0 ? c->want[0] : ""));
    arena_free(&ar);
}

static void TestImportGroupStarts(TestingT *t) {
    for (Int i = 0; i < NELEM(gdoc_import_groups); i++) {
        const GdocImportGroups *c = &gdoc_import_groups[i];
        testing_t_run(
            t, gdoc_cstr(c->name),
            BURROW_FN(TestingTFunc, gdoc_import_groups_case, (void *)(uintptr_t)c));
    }
}

static void TestSynopsis(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(gdoc_synopsis); i++) {
        const GdocSynopsis *e = &gdoc_synopsis[i];
        Str txt = str_from_bytes(e->txt, (Int)e->txt_len);
        Str fs = burrow__doc_first_sentence(txt);
        Str want = gdoc_sub(txt, 0, (Int)e->fsl);
        if (!str_eq(fs, want))
            testing_t_errorf_v(t, "firstSentence(%q) = %q, want %q", txt, fs, want);
        Str syn = doc_synopsis(a, txt);
        if (!str_eq(syn, gdoc_cstr(e->syn)))
            testing_t_errorf_v(t, "Synopsis(%q) = %q, want %q", txt, syn,
                               gdoc_cstr(e->syn));
    }
    arena_free(&ar);
}

static void gdoc_want(TestingT *t, const char *what, Str got, const char *want) {
    if (!str_eq(got, gdoc_cstr(want)))
        testing_t_errorf_v(t, "%s:\ngot:\n%s\nwant:\n%s", what, got, gdoc_cstr(want));
}

static Str gdoc_bytes(Slice b) {
    return str_from_bytes(b.p, b.len);
}

static void TestComment(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TokenFileSet *fset = token_new_file_set(a);
    Str src = BURROW_STR_EMPTY;
    (void)gdoc_file(a, S("pkgdoc/doc.go"), &src);
    AstFile *f =
        gdoc_parse(t, a, fset, S("testdata/pkgdoc/doc.go"), src, PARSER_PARSE_COMMENTS);
    if (f == NULL) {
        arena_free(&ar);
        return;
    }
    /* What parser.ParseDir makes of the directory. */
    AstPackage *pkg = (AstPackage *)ast_node_new(a, AST_KIND_PACKAGE);
    pkg->name = f->name->name;
    pkg->files = map_make(a, TYPE_STRING, TYPE_AST_FILE_PTR, 0);
    Str fname = S("testdata/pkgdoc/doc.go");
    (void)map_set(pkg->files, &fname, &f);
    DocPackage *p = doc_new(a, pkg, S("testdata/pkgdoc"), 0);

    Str input = gdoc_cstr(gdoc_comment_input);
    gdoc_want(t, "pkg.HTML", gdoc_bytes(doc_package_html(p, a, input)),
              gdoc_comment_want_html);
    gdoc_want(t, "pkg.Markdown", gdoc_bytes(doc_package_markdown(p, a, input)),
              gdoc_comment_want_markdown);
    gdoc_want(t, "pkg.Text", gdoc_bytes(doc_package_text(p, a, input)),
              gdoc_comment_want_text);
    gdoc_want(t, "pkg.Synopsis", doc_package_synopsis(p, a, input),
              gdoc_comment_want_synopsis);

    Map *words = map_make(a, TYPE_STRING, TYPE_STRING, 1);
    Str types = S("types"), none = BURROW_STR_EMPTY;
    (void)map_set(words, &types, &none);
    StringsBuilder b = STRINGS_BUILDER(a);
    doc_to_html(a, strings_builder_as_io_writer(&b), input, words);
    gdoc_want(t, "ToHTML", strings_builder_string(&b), gdoc_comment_want_old_html);

    b = STRINGS_BUILDER(a);
    doc_to_text(a, strings_builder_as_io_writer(&b), input, BURROW_STR_EMPTY, S("\t"),
                80);
    gdoc_want(t, "ToText", strings_builder_string(&b), gdoc_comment_want_old_text);

    gdoc_want(t, "Synopsis", doc_synopsis(a, input), gdoc_comment_want_old_synopsis);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(Test)                                                                            \
    X(TestFuncs)                                                                       \
    X(TestExamples)                                                                    \
    X(TestExampleNewFromFiles)                                                         \
    X(TestClassifyExamples)                                                            \
    X(TestImportGroupStarts)                                                           \
    X(TestSynopsis)                                                                    \
    X(TestComment)

TESTING_MAIN(TESTS)
