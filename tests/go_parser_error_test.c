/* Derived from Go's src/go/parser/error_test.go and short_test.go.
 * Go source: go1.27.1.
 *
 * error_test.go checks the errors the parser reports against the ERROR
 * comments in the files of testdata, and short_test.go makes the same check on
 * short programs, so the two are here together. The files and the programs
 * come from tests/go_parser_test_gen.h. Go reads each testdata file and hands
 * its bytes to ParseFile, and here the bytes are handed over the same way. The
 * -trace_errs flag is left out.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/go/parser.h"
#include "burrow/go/scanner.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/regexp.h"

#include <stdint.h>
#include <string.h>

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

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

static Str piece_str(const GptPiece *p) {
    return str_from_bytes(p->p, (Int)p->n);
}

/* The bytes of a generated file, its pieces joined up. */
static Str file_bytes(Alloc *a, const GptFile *f) {
    if (f->npieces == 1)
        return piece_str(&f->pieces[0]);
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

typedef struct FindFile {
    Alloc *a;
    Str name;
    TokenFile *file;
} FindFile;

static bool find_file(void *env, TokenFile *f) {
    FindFile *ff = (FindFile *)env;
    if (str_eq(token_file_name(f), ff->name)) {
        if (ff->file != NULL)
            panic_str(fmt_sprintf_v(ff->a, "%s used multiple times", ff->name));
        ff->file = f;
    }
    return true;
}

/* getFile assumes that each filename occurs at most once */
static TokenFile *get_file(Alloc *a, TokenFileSet *fset, Str filename) {
    FindFile ff = {a, filename, NULL};
    token_file_set_iterate(fset, BURROW_FN(TokenFileFunc, find_file, &ff));
    return ff.file;
}

static TokenPos get_pos(Alloc *a, TokenFileSet *fset, Str filename, Int offset) {
    TokenFile *f = get_file(a, fset, filename);
    if (f != NULL)
        return token_file_pos(f, offset);
    return 0;
}

/* ERROR comments must be of the form / * ERROR "rx" * / and rx is a regular
 * expression that matches the expected error message. The special form
 * / * ERROR HERE "rx" * / must be used for error messages that appear
 * immediately after a token, rather than at a token's position, and ERROR
 * AFTER means after the comment (e.g. at end of line). */
#define ERR_RX "^/\\* *ERROR *(HERE|AFTER)? *\"([^\"]*)\" *\\*/$"

/* expectedErrors collects the regular expressions of ERROR comments found in
 * files and returns them as a map of error positions (TokenPos) to error
 * messages (Str). */
static Map *expected_errors(Alloc *a, TokenFileSet *fset, Str filename, Str src) {
    Map *errors = map_make(a, TYPE_INT, TYPE_STRING, 0);
    Regexp *err_rx = regexp_must_compile(a, S(ERR_RX));

    GoScanner s;
    /* file was parsed already - do not add it again to the file set otherwise
     * the position information returned here will not match the position
     * information collected by the parser */
    go_scanner_init(&s, a, get_file(a, fset, filename), bytes_of(src),
                    (GoScannerErrorHandler){NULL, NULL}, GO_SCANNER_SCAN_COMMENTS);
    TokenPos prev = 0; /* position of last non-comment, non-semicolon token */
    TokenPos here = 0; /* position immediately after the token at position prev */

    for (;;) {
        Token tok = TOKEN_ILLEGAL;
        Str lit = BURROW_STR_EMPTY;
        TokenPos pos = go_scanner_scan(&s, &tok, &lit);
        TokenPos end = go_scanner_end(&s);

        switch ((int)tok) {
        case TOKEN_EOF:
            return errors;
        case TOKEN_COMMENT: {
            Slice m = regexp_find_string_submatch(err_rx, a, lit);
            if (m.len == 3) {
                Str kind = BURROW_AT(Str, m, 1);
                if (str_eq(kind, S("HERE")))
                    pos = here; /* position right after the previous token prior to
                                   comment */
                else if (str_eq(kind, S("AFTER")))
                    pos += lit.len; /* end of comment */
                else
                    pos = prev; /* token prior to comment */
                Str msg = BURROW_AT(Str, m, 2);
                (void)map_set(errors, &pos, &msg);
            }
            break;
        }
        case TOKEN_SEMICOLON:
            /* don't use the position of auto-inserted (invisible) semicolons */
            if (!str_eq(lit, S(";")))
                break;
            prev = pos;
            here = end;
            break;
        default:
            prev = pos;
            here = end;
            break;
        }
    }
}

/* compareErrors compares the map of expected error messages with the list of
 * found errors and reports discrepancies. */
static void compare_errors(TestingT *t, Alloc *a, TokenFileSet *fset, Map *expected,
                           GoScannerErrorList found) {
    for (Int i = 0; i < go_scanner_error_list_len(found); i++) {
        GoScannerError *e = go_scanner_error_list_at(found, i);
        Str where = token_position_string(e->pos, a);
        /* e->pos is a TokenPosition, but we want a TokenPos so we can do a map
         * lookup */
        TokenPos pos = get_pos(a, fset, e->pos.filename, e->pos.offset);
        Str *msg = (Str *)map_get(expected, &pos);
        if (msg != NULL) {
            /* we expect a message at pos; check if it matches */
            Error err = BURROW_NO_ERROR;
            Regexp *rx = regexp_compile(a, *msg, &err);
            if (!BURROW_OK(err)) {
                testing_t_errorf_v(t, "%s: %s", where, error_text(err));
                continue;
            }
            if (!regexp_match_string(rx, e->msg)) {
                testing_t_errorf_v(t, "%s: %q does not match %q", where, e->msg, *msg);
                continue;
            }
            /* we have a match - eliminate this error */
            map_del(expected, &pos);
        } else {
            /* To keep in mind when analyzing failed test output: If the same
             * error position occurs multiple times in errors, this message
             * will be triggered (because the first error at the position
             * removes this position from the expected errors). */
            testing_t_errorf_v(t, "%s: unexpected error: %s", where, e->msg);
        }
    }

    /* there should be no expected errors left */
    if (map_len(expected) > 0) {
        testing_t_errorf_v(t, "%d errors not reported:", map_len(expected));
        MapIter it = map_iter(expected);
        const void *k = NULL;
        void *v = NULL;
        while (map_next(&it, &k, &v)) {
            TokenPosition p = token_file_set_position(fset, *(const TokenPos *)k);
            testing_t_errorf_v(t, "%s: %s\n", token_position_string(p, a), *(Str *)v);
        }
    }
}

/* Go's checkErrors takes the source as an any and reads it with readSource,
 * which gives back the bytes of a string or a []byte. Here it gets the bytes. */
static void check_errors(TestingT *t, Alloc *a, Str filename, Str src, ParserMode mode,
                         bool expect_errors) {
    TokenFileSet *fset = token_new_file_set(a);
    Error err = BURROW_NO_ERROR;
    Slice b = bytes_of(src);
    (void)parser_parse_file(a, fset, filename, BURROW_ANY(TYPE_BYTES, &b), mode, &err);
    GoScannerErrorList found = slice_nil(TYPE_GO_SCANNER_ERROR_LIST->elem);
    if (!BURROW_OK(err)) {
        const GoScannerErrorList *l =
            (const GoScannerErrorList *)errors_as(err, TYPE_GO_SCANNER_ERROR_LIST);
        if (l == NULL) {
            testing_t_error_v(t, error_text(err));
            return;
        }
        found = *l;
    }
    go_scanner_error_list_remove_multiples(&found);

    Map *expected = map_make(a, TYPE_INT, TYPE_STRING, 0);
    if (expect_errors) {
        /* we are expecting the following errors (collect these after parsing a
         * file so that it is found in the file set) */
        expected = expected_errors(a, fset, filename, src);
    }

    /* verify errors returned by the parser */
    compare_errors(t, a, fset, expected, found);
}

static void run_testdata(void *env, TestingT *t) {
    const GptFile *f = (const GptFile *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str name = str_from_bytes(f->name, (Int)strlen(f->name));
    check_errors(t, a, filepath_join_v(a, 2, S("testdata"), name), file_bytes(a, f),
                 PARSER_DECLARATION_ERRORS | PARSER_ALL_ERRORS, true);
    arena_free(&ar);
}

/* Go reads the directory and runs a subtest for each entry, checking the
 * .src and .go2 files in it. The files under the subdirectories are other
 * tests'. */
static void TestErrors(TestingT *t) {
    for (Int i = 0; i < NELEM(gpt_testdata); i++) {
        const GptFile *f = &gpt_testdata[i];
        Str name = str_from_bytes(f->name, (Int)strlen(f->name));
        if (strings_contains(name, S("/")) || strings_has_prefix(name, S(".")))
            continue;
        if (!strings_has_suffix(name, S(".src")) &&
            !strings_has_suffix(name, S(".go2")))
            continue;
        testing_t_run(t, name,
                      BURROW_FN(TestingTFunc, run_testdata, (void *)(uintptr_t)f));
    }
}

/* ------------------------------------------------------------- short_test.go */

static void TestValid(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(gpt_valids); i++) {
        Str src = piece_str(&gpt_valids[i]);
        check_errors(t, a, src, src, PARSER_DECLARATION_ERRORS | PARSER_ALL_ERRORS,
                     false);
    }
    arena_free(&ar);
}

/* TestSingle is useful to track down a problem with a single short test
 * program. */
static void TestSingle(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Str src = S("package p; var _ = T{}");
    check_errors(t, a, src, src, PARSER_DECLARATION_ERRORS | PARSER_ALL_ERRORS, true);
    arena_free(&ar);
}

static void TestInvalid(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NELEM(gpt_invalids); i++) {
        Str src = piece_str(&gpt_invalids[i]);
        check_errors(t, a, src, src, PARSER_DECLARATION_ERRORS | PARSER_ALL_ERRORS,
                     true);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestErrors)                                                                      \
    X(TestValid)                                                                       \
    X(TestSingle)                                                                      \
    X(TestInvalid)

TESTING_MAIN(TESTS)
