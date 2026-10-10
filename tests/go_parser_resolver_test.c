/* Derived from Go's src/go/parser/resolver_test.go.
 * Go source: go1.27.1.
 *
 * The files of testdata/resolution come from tests/go_parser_test_gen.h, and
 * each is parsed from its bytes, as Go does once it has read the file.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/go/parser.h"
#include "burrow/go/scanner.h"
#include "burrow/mem/arena.h"

#include <stdint.h>
#include <string.h>

#define GPT_WANT_TESTDATA
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

static bool collect_decl(void *env, AstNode node) {
    Map *objmap = (Map *)env;
    /* Ignore blank identifiers to reduce noise. */
    if (node != NULL && node->kind == AST_KIND_IDENT) {
        AstIdent *ident = (AstIdent *)node;
        if (ident->obj != NULL && !str_eq(ident->name, S("_"))) {
            TokenPos k = ast_ident_pos(ident);
            TokenPos v = ast_object_pos(ident->obj);
            (void)map_set(objmap, &k, &v);
        }
    }
    return true;
}

/* declsFromParser walks the file and collects the map associating an
 * identifier position with its declaration position. */
static Map *decls_from_parser(Alloc *a, AstFile *file) {
    Map *objmap = map_make(a, TYPE_INT, TYPE_INT, 0);
    ast_inspect(&file->node, BURROW_FN(AstInspectFunc, collect_decl, objmap));
    return objmap;
}

/* annotatedObj: the name a comment marks, and whether it marks a declaration
 * ('=') or a use ('@'). */
static Str annotated_obj(Str lit, bool *decl, bool *use) {
    *decl = false;
    *use = false;
    if (lit.p[1] == '*')
        lit = str_from_bytes(lit.p, lit.len - 2); /* strip trailing */
    lit = strings_trim_space(str_from_bytes(lit.p + 2, lit.len - 2));

    for (Int idx = 0; idx < lit.len; idx++) {
        switch (lit.p[idx]) {
        case '=':
            *decl = true;
            break;
        case '@':
            *use = true;
            break;
        default:
            return str_from_bytes(lit.p + idx, lit.len - idx);
        }
    }
    return BURROW_STR_EMPTY;
}

/* positionMarkers extracts named positions from the source denoted by
 * comments prefixed with '=' (declarations) and '@' (uses): for example '@foo'
 * or '=@bar'. It returns a map of name->position for declarations, and the
 * uses as a list of names and a list of their positions, which Go keeps as a
 * map of name->positions. */
static void position_markers(Alloc *a, TokenFile *handle, Str src, Map **decls,
                             Slice *use_names, Slice *use_posns) {
    GoScanner s;
    go_scanner_init(&s, a, handle, bytes_of(src), (GoScannerErrorHandler){NULL, NULL},
                    GO_SCANNER_SCAN_COMMENTS);
    *decls = map_make(a, TYPE_STRING, TYPE_INT, 0);
    *use_names = slice_nil(TYPE_STRING);
    *use_posns = slice_nil(TYPE_INT);
    TokenPos prev = 0; /* position of last non-comment, non-semicolon token */

    for (;;) {
        Token tok = TOKEN_ILLEGAL;
        Str lit = BURROW_STR_EMPTY;
        TokenPos pos = go_scanner_scan(&s, &tok, &lit);
        switch ((int)tok) {
        case TOKEN_EOF:
            return;
        case TOKEN_COMMENT: {
            bool decl = false, use = false;
            Str name = annotated_obj(lit, &decl, &use);
            if (name.len > 0) {
                if (decl) {
                    if (map_get(*decls, &name) != NULL)
                        panic_str(fmt_sprintf_v(
                            a, "duplicate declaration markers for %s", name));
                    (void)map_set(*decls, &name, &prev);
                }
                if (use) {
                    *use_names = slice_append(a, *use_names, &name, 1);
                    *use_posns = slice_append(a, *use_posns, &prev, 1);
                }
            }
            break;
        }
        case TOKEN_SEMICOLON:
            /* ignore automatically inserted semicolon */
            if (str_eq(lit, S("\n")))
                break;
            prev = pos;
            break;
        default:
            prev = pos;
            break;
        }
    }
}

/* declsFromComments looks at comments annotating uses and declarations, and
 * maps each identifier use to its corresponding declaration. See the
 * description of these annotations in the documentation for TestResolution. */
static Map *decls_from_comments(Alloc *a, TokenFile *handle, Str src) {
    Map *decls = NULL;
    Slice names = {0}, posns = {0};
    position_markers(a, handle, src, &decls, &names, &posns);

    Map *objmap = map_make(a, TYPE_INT, TYPE_INT, 0);
    /* Join decls and uses on name, to build the map of use->decl. */
    for (Int i = 0; i < names.len; i++) {
        Str name = BURROW_AT(Str, names, i);
        TokenPos *declpos = (TokenPos *)map_get(decls, &name);
        if (declpos == NULL)
            panic_str(fmt_sprintf_v(a, "missing declaration for %s", name));
        (void)map_set(objmap, &BURROW_AT(TokenPos, posns, i), declpos);
    }
    return objmap;
}

/* The file name is implied by the subtest, so it is removed to avoid clutter
 * in error messages. */
static Str pos_string(Alloc *a, TokenFile *handle, TokenPos pos) {
    TokenPosition p = token_file_position(handle, pos);
    p.filename = BURROW_STR_EMPTY;
    return token_position_string(p, a);
}

static void run_resolution(void *env, TestingT *t) {
    const GptFile *f = (const GptFile *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    TokenFileSet *fset = token_new_file_set(a);
    Str rel = str_from_bytes(f->name, (Int)strlen(f->name));
    Str path =
        filepath_join_v(a, 3, S("testdata"), S("resolution"), filepath_base(rel));
    Str src = file_bytes(a, f);
    Slice b = bytes_of(src);
    ParserMode mode = 0;
    Error err = BURROW_NO_ERROR;
    AstFile *file =
        parser_parse_file(a, fset, path, BURROW_ANY(TYPE_BYTES, &b), mode, &err);
    if (!BURROW_OK(err))
        testing_t_fatal_v(t, error_text(err));

    /* Compare the positions of objects resolved during parsing (fromParser) to
     * those annotated in source comments (fromComments). */

    TokenFile *handle = token_file_set_file(fset, file->package);
    Map *from_parser = decls_from_parser(a, file);
    Map *from_comments = decls_from_comments(a, handle, src);

    MapIter it = map_iter(from_comments);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v)) {
        TokenPos key = *(const TokenPos *)k;
        TokenPos want = *(TokenPos *)v;
        TokenPos *found = (TokenPos *)map_get(from_parser, &key);
        TokenPos got = found != NULL ? *found : 0;
        if (got != want)
            testing_t_errorf_v(t, "%s resolved to %s, want %s",
                               pos_string(a, handle, key), pos_string(a, handle, got),
                               pos_string(a, handle, want));
        map_del(from_parser, &key);
    }
    /* What remains in fromParser are unexpected resolutions. */
    it = map_iter(from_parser);
    while (map_next(&it, &k, &v))
        testing_t_errorf_v(t, "%s resolved to %s, want no object",
                           pos_string(a, handle, *(const TokenPos *)k),
                           pos_string(a, handle, *(TokenPos *)v));
    arena_free(&ar);
}

/* TestResolution checks that identifiers are resolved to the declarations
 * annotated in the source, by comparing the positions of the resulting
 * Ident.Obj.Decl to positions marked in the source via special comments.
 *
 * In the test source, any comment prefixed with '=' or '@' (or both) marks
 * the previous token position as the declaration ('=') or a use ('@') of an
 * identifier. The text following '=' and '@' in the comment string is the
 * label to use for the location. Declaration labels must be unique within the
 * file, and use labels must refer to an existing declaration label. It's OK
 * for a comment to denote both the declaration and use of a label (e.g.
 * '=@foo'). Leading and trailing whitespace is ignored. Any comment not
 * beginning with '=' or '@' is ignored. */
static void TestResolution(TestingT *t) {
    for (Int i = 0; i < NELEM(gpt_testdata); i++) {
        const GptFile *f = &gpt_testdata[i];
        Str name = str_from_bytes(f->name, (Int)strlen(f->name));
        if (!strings_has_prefix(name, S("resolution/")))
            continue;
        testing_t_run(t, filepath_base(name),
                      BURROW_FN(TestingTFunc, run_resolution, (void *)(uintptr_t)f));
    }
}

#define TESTS(X) X(TestResolution)

TESTING_MAIN(TESTS)
