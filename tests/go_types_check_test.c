/* Derived from Go's src/go/types/check_test.go and commentMap_test.go. Go
 * source: go1.27.1.
 *
 * This file implements a typechecker test harness. The packages specified in
 * tests are typechecked. Error messages reported by the typechecker are
 * compared against the errors expected in the test files.
 *
 * Expected errors are indicated in the test files by putting comments of the
 * form / * ERROR pattern * / or / * ERRORx pattern * / (or a similar //-style
 * line comment) immediately following the tokens where errors are reported.
 * There must be exactly one blank before and after the ERROR/ERRORx
 * indicator, and the pattern must be a properly quoted Go string.
 *
 * The harness will verify that each ERROR pattern is a substring of the error
 * reported at that source position, and that each ERRORx pattern is a regular
 * expression matching the respective error. Consecutive comments may be used
 * to indicate multiple errors reported at the same position.
 *
 * The test files come from tests/go_types_check_test_gen.h, which
 * tools/gen-go-types-tests.sh writes from Go's testdata.
 *
 * Not here yet: a package that imports anything but unsafe (or C, with
 * -fakeImportC) is skipped, since burrow has no default importer yet. Not
 * here: TestManual, which reads files named on the command line.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/flag.h"
#include "burrow/go/ast.h"
#include "burrow/go/build.h"
#include "burrow/go/build/constraint.h"
#include "burrow/go/parser.h"
#include "burrow/go/scanner.h"
#include "burrow/go/token.h"
#include "burrow/go/types.h"
#include "burrow/mem/arena.h"
#include "burrow/platform.h"
#include "burrow/regexp.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"

#include <stdint.h>
#include <string.h>

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverlength-strings"
#endif
#include "go_types_check_test_gen.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define S(lit) BURROW_S(lit)
#define NELEM(x) ((Int)(sizeof(x) / sizeof((x)[0])))

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

#define TCHK_TYPE(var, T, gonm)                                                        \
    static const Type var = {                                                          \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"types_test", 10},                                              \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

typedef struct {
    Int line, col; /* comment position */
    Str text;      /* comment text, excluding "//", "/ *", or "* /" */
    /* Not in Go: */
    Str filename; /* the file it is in, for testFiles */
    bool done;    /* matched by an error, so no longer expected */
} Comment;

TCHK_TYPE(comment_type, Comment, "comment");

/* commentMap collects all comments in the given src with comment text that
 * matches the supplied regular expression rx and returns them as []comment
 * lists in a map indexed by line number. The comment text is the comment with
 * any comment markers ("//", "/ *", or "* /") stripped. The position for each
 * comment is the position of the token immediately preceding the comment,
 * with all comments that are on the same line collected in a slice, in source
 * order. If there is no preceding token (the matching comment appears at the
 * beginning of the file), then the recorded position is unknown (line, col =
 * 0, 0). If there are no matching comments, the result is nil.
 *
 * Not in Go: the result is one list of Comment in source order rather than a
 * map by line, since each comment holds its line anyway. */
static Slice comment_map(Alloc *a, Str src, const Regexp *rx) {
    Slice res = slice_nil(&comment_type);
    TokenFileSet *fset = token_new_file_set(a);
    TokenFile *file = token_file_set_add_file(fset, S(""), -1, src.len);

    GoScanner s;
    go_scanner_init(&s, a, file, bytes_of(src), (GoScannerErrorHandler){0},
                    GO_SCANNER_SCAN_COMMENTS);
    TokenPos prev =
        TOKEN_NO_POS; /* position of last non-comment, non-semicolon token */

    for (;;) {
        Token tok;
        Str lit = S("");
        TokenPos pos = go_scanner_scan(&s, &tok, &lit);
        switch (tok) {
        case TOKEN_EOF:
            return res;
        case TOKEN_COMMENT:
            if (lit.p[1] == '*')
                lit = str_from_bytes(lit.p, lit.len - 2); /* strip trailing * / */
            lit = str_from_bytes(lit.p + 2, lit.len - 2); /* strip leading // or / * */
            if (regexp_match_string(rx, lit)) {
                TokenPosition p = token_file_set_position(fset, prev);
                Comment err = {p.line, p.column, lit, S(""), false};
                res = slice_append(a, res, &err, 1);
            }
            break;
        case TOKEN_SEMICOLON:
            /* ignore automatically inserted semicolon */
            if (str_eq(lit, S("\n")))
                continue;
            prev = pos;
            break;
        default:
            prev = pos;
            break;
        }
    }
}

static void TestCommentMap(TestingT *t) {
    Str src = S("/* ERROR \"0:0\" */ /* ERROR \"0:0\" */ // ERROR \"0:0\"\n"
                "// ERROR \"0:0\"\n"
                "x /* ERROR \"3:1\" */                // ignore automatically "
                "inserted semicolon here\n"
                "/* ERROR \"3:1\" */                  // position of x on previous "
                "line\n"
                "   x /* ERROR \"5:4\" */ ;           // do not ignore this semicolon\n"
                "/* ERROR \"5:24\" */                 // position of ; on previous "
                "line\n"
                "\tpackage /* ERROR \"7:2\" */  // indented with tab\n"
                "        import  /* ERROR \"8:9\" */  // indented with blanks\n");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice m = comment_map(a, src, regexp_must_compile(a, S("^ ERROR ")));
    Int found = 0; /* number of errors found */
    for (Int i = 0; i < m.len; i++) {
        Comment *err = &BURROW_AT(Comment, m, i);
        /* Not in Go: there is no map line to hold err.line against. */
        Int line = err->line;

        Str got = strings_trim_space(
            str_from_bytes(err->text.p + 7, err->text.len - 7)); /* len(" ERROR ") */
        Int col = err->col;
        Str want = fmt_sprintf_v(a, "\"%d:%d\"", line, col);
        if (!str_eq(got, want)) {
            testing_t_errorf_v(t, "%d:%d: got msg %q; want %q", line, col, got, want);
            continue;
        }
        found++;
    }
    Int want = strings_count(src, S(" ERROR "));
    if (found != want)
        testing_t_errorf_v(t, "commentMap got %d errors; want %d", found, want);
    arena_free(&ar);
}

/* Not in Go: an error as unpackError gives it, the position and message of a
 * scanner.Error or a types Error. */
typedef struct {
    TokenPosition pos;
    Str msg;
} PosMsg;

TCHK_TYPE(pos_msg_type, PosMsg, "posMsg");

typedef struct {
    Alloc *a;
    Slice *errlist;
} ErrEnv;

static void parse_files(TestingT *t, Alloc *a, TokenFileSet *fset, Slice filenames,
                        Slice srcs, ParserMode mode, Slice *files, Slice *errlist) {
    *files = slice_nil(TYPE_AST_FILE_PTR);
    for (Int i = 0; i < filenames.len; i++) {
        Str filename = BURROW_AT(Str, filenames, i);
        Str src = BURROW_AT(Str, srcs, i);
        Error err = BURROW_NO_ERROR;
        AstFile *file = parser_parse_file(a, fset, filename,
                                          BURROW_ANY(TYPE_STRING, &src), mode, &err);
        if (file == NULL)
            testing_t_fatalf_v(t, "%s: %s", filename, error_text(err));
        *files = slice_append(a, *files, &file, 1);
        if (BURROW_FAILED(err)) {
            const GoScannerErrorList *list =
                (const GoScannerErrorList *)errors_as(err, TYPE_GO_SCANNER_ERROR_LIST);
            /* Not in Go: unpackError would panic on any other error. */
            if (list == NULL) {
                testing_t_fatalf_v(t, "%s: %s", filename, error_text(err));
                continue;
            }
            for (Int k = 0; k < go_scanner_error_list_len(*list); k++) {
                GoScannerError *e = go_scanner_error_list_at(*list, k);
                PosMsg pm = {e->pos, e->msg};
                *errlist = slice_append(a, *errlist, &pm, 1);
            }
        }
    }
}

/* absDiff returns the absolute difference between x and y. */
static Int abs_diff(Int x, Int y) {
    if (x < y)
        return y - x;
    return x - y;
}

/* parseFlags parses flags from the first line of the given source if the line
 * starts with "//" (line comment) followed by "-" (possibly with spaces
 * between). Otherwise the line is ignored. */
static Error parse_flags(Alloc *a, Str src, FlagFlagSet *flags) {
    /* we must have a line comment that starts with a "-" */
    Str prefix = S("//");
    if (!strings_has_prefix(src, prefix))
        return BURROW_NO_ERROR; /* first line is not a line comment */
    src = str_from_bytes(src.p + prefix.len, src.len - prefix.len);
    Int i = strings_index(src, S("-"));
    if (i < 0 || strings_trim_space(str_from_bytes(src.p, i)).len != 0)
        return BURROW_NO_ERROR; /* comment doesn't start with a "-" */
    Int end = strings_index(src, S("\n"));
    const Int max_len = 256;
    if (end < 0 || end > max_len)
        return errors_new(a, S("flags comment line too long"));
    return flag_flag_set_parse(flags, strings_fields(a, str_from_bytes(src.p, end)));
}

static void collect_error(void *env, const TypesError *err) {
    ErrEnv *e = env;
    /* Ignore secondary error messages starting with "\t"; they are clarifying
     * messages for a primary error. */
    if (!strings_contains(types_error_error(err, e->a), S(": \t"))) {
        PosMsg pm = {token_file_set_position(err->fset, err->pos), err->msg};
        *e->errlist = slice_append(e->a, *e->errlist, &pm, 1);
    }
}

/* Not in Go: whether a file imports a package no importer can give it. Go's
 * test has the default importer; burrow has none yet. */
static bool needs_importer(Alloc *a, AstFile *file, bool fake_import_c, Str *path) {
    for (Int i = 0; i < file->imports.len; i++) {
        AstImportSpec *spec = BURROW_AT(AstImportSpec *, file->imports, i);
        Error err = BURROW_NO_ERROR;
        *path = strconv_unquote(a, spec->path->value, &err);
        if (BURROW_FAILED(err))
            continue; /* the checker reports it */
        if (str_eq(*path, S("unsafe")) || (fake_import_c && str_eq(*path, S("C"))))
            continue;
        return true;
    }
    return false;
}

/* testFiles type-checks the package consisting of the given files, and
 * compares the resulting errors with the ERROR annotations in the source.
 *
 * The srcs slice contains the file content for the files named in the
 * filenames slice.
 *
 * If provided, opts may be used to mutate the Config before type-checking.
 *
 * Not in Go: there is no manual parameter, since TestManual is not here, and
 * the one option the tests use, setting the Sizes, is a TypesStdSizes, or
 * NULL for none. Everything comes from the caller's arena ar, which a skip
 * frees on the way out. */
static void test_files(TestingT *t, Arena *ar, Slice filenames, Slice srcs,
                       const TypesStdSizes *sizes) {
    if (filenames.len == 0) {
        Str msg = S("no source files");
        testing_t_fatal_v(t, msg);
    }

    Alloc *a = arena_allocator(ar);
    TokenFileSet *fset = token_new_file_set(a);

    /* parse files */
    Slice files;
    Slice errlist = slice_nil(&pos_msg_type);
    parse_files(t, a, fset, filenames, srcs,
                PARSER_ALL_ERRORS | PARSER_SKIP_OBJECT_RESOLUTION, &files, &errlist);
    Str pkg_name = S("<no package>");
    if (files.len > 0)
        pkg_name = BURROW_AT(AstFile *, files, 0)->name->name;

    /* set up typechecker */
    TypesConfig conf = {0};
    ErrEnv env = {a, &errlist};
    conf.error = (TypesErrorFunc){collect_error, &env};

    /* apply custom configuration */
    TypesStdSizes sz;
    if (sizes != NULL) {
        sz = *sizes;
        conf.sizes = types_std_sizes_as_sizes(&sz);
    }

    /* apply flag setting (overrides custom configuration) */
    Str goexperiment = S("");
    FlagFlagSet *flags = flag_new_flag_set(a, S(""), FLAG_PANIC_ON_ERROR);
    flag_flag_set_string_var(flags, &conf.go_version, S("lang"), S(""), S(""));
    flag_flag_set_string_var(flags, &goexperiment, S("goexperiment"), S(""), S(""));
    flag_flag_set_bool_var(flags, &conf.fake_import_c, S("fakeImportC"), false, S(""));
    Error ferr = parse_flags(a, BURROW_AT(Str, srcs, 0), flags);
    if (BURROW_FAILED(ferr))
        testing_t_fatal_v(t, error_text(ferr));
    /* Not in Go: burrow has no internal/buildcfg to set, and no file in Go's
     * testdata sets -goexperiment. */
    if (goexperiment.len != 0)
        testing_t_fatalf_v(t, "-goexperiment=%s is not supported", goexperiment);

    for (Int i = 0; i < files.len; i++) {
        Str path = S("");
        if (needs_importer(a, BURROW_AT(AstFile *, files, i), conf.fake_import_c,
                           &path)) {
            char buf[256];
            Int n = path.len < (Int)sizeof(buf) ? path.len : (Int)sizeof(buf);
            memcpy(buf, path.p, (size_t)n);
            arena_free(ar);
            testing_t_skipf_v(t, "imports %q, and burrow has no importer yet",
                              str_from_bytes(buf, n));
        }
    }

    /* Provide Config.Info with all maps so that info recording is tested. */
    TypesInfo info = types_info_all(a);

    /* typecheck */
    Error cerr = BURROW_NO_ERROR;
    (void)types_config_check(&conf, a, pkg_name, fset, files, &info, &cerr);

    /* collect expected errors */
    Regexp *rx = regexp_must_compile(a, S("^ ERRORx? "));
    Slice errmap = slice_nil(&comment_type);
    for (Int i = 0; i < filenames.len; i++) {
        Slice m = comment_map(a, BURROW_AT(Str, srcs, i), rx);
        for (Int k = 0; k < m.len; k++)
            BURROW_AT(Comment, m, k).filename = BURROW_AT(Str, filenames, i);
        errmap = slice_append(a, errmap, m.p, m.len);
    }

    /* match against found errors */
    Slice indices = slice_nil(TYPE_INT); /* list indices of matching errors, reused */
    for (Int e = 0; e < errlist.len; e++) {
        PosMsg *err = &BURROW_AT(PosMsg, errlist, e);
        TokenPosition got_pos = err->pos;
        Str got_msg = err->msg;

        /* find list of errors for the respective error line */
        Str filename = got_pos.filename;
        Int line = got_pos.line;

        /* At least one of the errors in errList should match the current
         * error. */
        indices.len = 0;
        for (Int i = 0; i < errmap.len; i++) {
            Comment *want = &BURROW_AT(Comment, errmap, i);
            if (want->done || want->line != line || !str_eq(want->filename, filename))
                continue;
            bool substr = false;
            Str pattern = strings_cut_prefix(want->text, S(" ERROR "), &substr);
            if (!substr) {
                bool found = false;
                pattern = strings_cut_prefix(want->text, S(" ERRORx "), &found);
                if (!found)
                    panic_str(S("unreachable"));
            }
            Int col = want->col;
            Error uerr = BURROW_NO_ERROR;
            Str unquoted = strconv_unquote(a, strings_trim_space(pattern), &uerr);
            if (BURROW_FAILED(uerr)) {
                testing_t_errorf_v(
                    t, "%s:%d:%d: invalid ERROR pattern (cannot unquote %s)", filename,
                    line, col, pattern);
                continue;
            }
            if (substr) {
                if (!strings_contains(got_msg, unquoted))
                    continue;
            } else {
                Error rerr = BURROW_NO_ERROR;
                Regexp *re = regexp_compile(a, unquoted, &rerr);
                if (BURROW_FAILED(rerr)) {
                    testing_t_errorf_v(t, "%s:%d:%d: %s", filename, line, col,
                                       error_text(rerr));
                    continue;
                }
                bool matched = regexp_match_string(re, got_msg);
                regexp_free(re);
                if (!matched)
                    continue;
            }
            indices = slice_append(a, indices, &i, 1);
        }
        Str where = token_position_string(got_pos, a);
        if (indices.len == 0) {
            testing_t_errorf_v(t, "%s: no error expected: %q", where, got_msg);
            continue;
        }
        /* len(indices) > 0 */

        /* If there are multiple matching errors, select the one with the
         * closest column position. */
        Int index = -1; /* index of matching error */
        Int delta = 0;
        for (Int k = 0; k < indices.len; k++) {
            Int i = BURROW_AT(Int, indices, k);
            Int d = abs_diff(got_pos.column, BURROW_AT(Comment, errmap, i).col);
            if (index < 0 || d < delta) {
                index = i;
                delta = d;
            }
        }

        /* The closest column position must be within expected colDelta. */
        const Int col_delta = 0; /* go/types errors are positioned correctly */
        if (delta > col_delta) {
            Int got_col = got_pos.column;
            Int want_col = BURROW_AT(Comment, errmap, index).col;
            testing_t_errorf_v(t, "%s: got col = %d; want %d", where, got_col,
                               want_col);
        }

        /* eliminate from errList */
        BURROW_AT(Comment, errmap, index).done = true;
    }

    /* there should be no expected errors left */
    bool header = false;
    for (Int i = 0; i < errmap.len; i++) {
        Comment *err = &BURROW_AT(Comment, errmap, i);
        if (err->done)
            continue;
        if (!header) {
            testing_t_errorf_v(t, "--- %s: unreported errors:", pkg_name);
            header = true;
        }
        testing_t_errorf_v(t, "%s:%d:%d: %s", err->filename, err->line, err->col,
                           err->text);
    }
}

static void TestLongConstants(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str ones1 = strings_repeat(a, S("1"), 9999);
    Str ones2 = strings_repeat(a, S("1"), 10001);
    Str src = fmt_sprintf_v(a,
                            "package longconst; const _ = %s /* ERROR \"constant "
                            "overflow\" */; const _ = %s // ERROR \"excessively long "
                            "constant\"",
                            ones1, ones2);
    Str filename = S("longconst.go");
    test_files(t, &ar, slice_append(a, slice_nil(TYPE_STRING), &filename, 1),
               slice_append(a, slice_nil(TYPE_STRING), &src, 1), NULL);
    arena_free(&ar);
}

/* TestIndexRepresentability tests that constant index operands must be
 * representable as int even if they already have a type that can represent
 * larger values. */
static void TestIndexRepresentability(TestingT *t) {
    Str src = S("package index; var s []byte; var _ = s[int64 /* ERRORx "
                "\"int64\\\\(1\\\\) << 40 \\\\(.*\\\\) overflows int\" */ (1) << 40]");
    Str filename = S("index.go");
    const TypesStdSizes sizes = {4, 4};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    test_files(t, &ar, slice_append(a, slice_nil(TYPE_STRING), &filename, 1),
               slice_append(a, slice_nil(TYPE_STRING), &src, 1), &sizes);
    arena_free(&ar);
}

static void TestIssue47243_TypedRHS(TestingT *t) {
    /* The RHS of the shift expression below overflows uint on 32bit platforms,
     * but this is OK as it is explicitly typed. */
    Str src = S(
        "package issue47243; var a uint64; var _ = a << uint64(4294967296)"); /* uint64(1<<32) */
    Str filename = S("p.go");
    const TypesStdSizes sizes = {4, 4};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    test_files(t, &ar, slice_append(a, slice_nil(TYPE_STRING), &filename, 1),
               slice_append(a, slice_nil(TYPE_STRING), &src, 1), &sizes);
    arena_free(&ar);
}

/* Not in Go: a file put back together from its pieces. */
static Str file_src(Alloc *a, const GtckFile *f) {
    Slice parts = slice_nil(TYPE_STRING);
    for (int i = 0; i < f->npieces; i++) {
        Str p = str_from_bytes(f->pieces[i].p, (Int)f->pieces[i].n);
        parts = slice_append(a, parts, &p, 1);
    }
    return strings_join(a, parts, S(""));
}

static bool should_test_match(void *env, Str tag) {
    const BuildContext *ctxt = env;
    /* We only care GOOS, GOARCH, and go version tags. */
    for (Int i = 0; i < ctxt->release_tags.len; i++)
        if (str_eq(BURROW_AT(Str, ctxt->release_tags, i), tag))
            return true;
    return str_eq(tag, S(BURROW_OS_NAME)) || str_eq(tag, S(BURROW_ARCH_NAME));
}

/* shouldTest checks build tags in src and returns whether the file should be
 * tested according to the tags. */
static bool should_test(Alloc *a, Str src) {
    BuildContext ctxt = build_default(a);
    ConstraintTagFunc match = {should_test_match, &ctxt};
    Slice lines = strings_split(a, src, S("\n"));
    for (Int i = 0; i < lines.len; i++) {
        Str line = BURROW_AT(Str, lines, i);
        if (strings_has_prefix(line, S("package ")))
            break;
        Error err = BURROW_NO_ERROR;
        ConstraintExpr expr = constraint_parse(a, line, &err);
        if (BURROW_OK(err))
            return constraint_expr_eval(expr, match);
    }
    return true;
}

static void test_pkg(void *env, TestingT *t) {
    const GtckPkg *pkg = &gtck_pkgs[(uintptr_t)env];
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice fs = slice_nil(TYPE_STRING);
    Slice srcs = slice_nil(TYPE_STRING);
    for (int i = pkg->first; i < pkg->first + pkg->nfiles; i++) {
        Str filename = str_from_cstr(gtck_files[i].name);
        Str src = file_src(a, &gtck_files[i]);
        if (!should_test(a, src))
            continue;
        fs = slice_append(a, fs, &filename, 1);
        srcs = slice_append(a, srcs, &src, 1);
    }
    if (fs.len == 0) {
        arena_free(&ar);
        Str msg = S("all files skipped by build tags");
        testing_t_skip_v(t, msg);
    }
    test_files(t, &ar, fs, srcs, NULL);
    arena_free(&ar);
}

/* Not in Go: the files are in gtck_pkgs, not read from dir. A package that is
 * a directory is run under its name as testDir runs it, and one that is a
 * file under its file name, which is the same thing here. */
static void test_dir_files(TestingT *t, const char *suite) {
    Str want = str_from_cstr(suite);
    for (Int i = 0; i < NELEM(gtck_pkgs); i++) {
        if (!str_eq(str_from_cstr(gtck_pkgs[i].suite), want))
            continue;
        testing_t_run(t, str_from_cstr(gtck_pkgs[i].name),
                      BURROW_FN(TestingTFunc, test_pkg, (uintptr_t)i));
    }
}

static void TestCheck(TestingT *t) {
    types_def_predeclared_test_funcs();
    test_dir_files(t, "check");
}
static void TestSpec(TestingT *t) {
    test_dir_files(t, "spec");
}
static void TestExamples(TestingT *t) {
    test_dir_files(t, "examples");
}
static void TestFixedbugs(TestingT *t) {
    test_dir_files(t, "fixedbugs");
}
static void TestLocal(TestingT *t) {
    test_dir_files(t, "local");
}

#define TESTS(X)                                                                       \
    X(TestCommentMap)                                                                  \
    X(TestLongConstants)                                                               \
    X(TestIndexRepresentability)                                                       \
    X(TestIssue47243_TypedRHS)                                                         \
    X(TestCheck)                                                                       \
    X(TestSpec)                                                                        \
    X(TestExamples)                                                                    \
    X(TestFixedbugs)                                                                   \
    X(TestLocal)

TESTING_MAIN(TESTS)
