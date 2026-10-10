/* go/build: reading the top of a Go file, up to the end of its imports.
 *
 * Derived from Go's src/go/build/read.go.
 * Go source: go1.27.1.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "build_internal.h"

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/go/ast.h"
#include "burrow/go/parser.h"
#include "burrow/go/scanner.h"
#include "burrow/go/token.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define S(lit) BURROW_S(lit)

static const Type gbr_file_import_desc = {
    {(const Byte *)"fileImport", 10},
    {(const Byte *)"go/build", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(burrow__BuildFileImport),
    (uint16_t)_Alignof(burrow__BuildFileImport),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x67626669U, /* "gbfi" */
    NULL,
};

static const Type gbr_file_embed_desc = {
    {(const Byte *)"fileEmbed", 9},
    {(const Byte *)"go/build", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(burrow__BuildFileEmbed),
    (uint16_t)_Alignof(burrow__BuildFileEmbed),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x67626665U, /* "gbfe" */
    NULL,
};

static const Str gbr_syntax_text = {(const Byte *)"syntax error", 12};
static const Error gbr_err_syntax = {&burrow_sentinel_error_vt, &gbr_syntax_text};
static const Str gbr_nul_text = {(const Byte *)"unexpected NUL in input", 23};
static const Error gbr_err_nul = {&burrow_sentinel_error_vt, &gbr_nul_text};

BURROW_NORETURN static void gbr_oom(void) {
    panic_str(S("go/build: out of memory"));
}

/* x then y, in a. */
static Str gbr_cat(Alloc *a, Str x, Str y) {
    Int n = x.len + y.len;
    if (n == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (p == NULL)
        gbr_oom();
    if (x.len > 0)
        memcpy(p, x.p, (size_t)x.len);
    if (y.len > 0)
        memcpy(p + x.len, y.p, (size_t)y.len);
    return str_from_bytes(p, n);
}

static void gbr_append(Alloc *a, Slice *s, const void *v) {
    Slice next = slice_append(a, *s, v, 1);
    if (next.len != s->len + 1)
        gbr_oom();
    *s = next;
}

/* ---------------------------------------------------------- importReader */

typedef struct GbrReader {
    Alloc *a;
    BufioReader *b;
    Byte *buf;
    Int len, cap;
    bool pinned; /* a view of buf is kept, so an old buf is never freed */
    Byte peek;
    Error err;
    bool eof;
    Int nerr;
    Str filename;
} GbrReader;

static bool gbr_is_eof(Error err) {
    return errors_is(err, io_eof);
}

static void gbr_init(GbrReader *r, Alloc *a, Str name, IoReader f) {
    memset(r, 0, sizeof *r);
    r->a = a;
    r->err = BURROW_NO_ERROR;
    r->filename = name;
    r->b = bufio_new_reader(a, f);
    if (r->b == NULL)
        gbr_oom();
    /* Drop a leading UTF-8 byte order mark, which a compiler may ignore. */
    Error perr = BURROW_NO_ERROR;
    Slice lead = bufio_reader_peek(r->b, 3, &perr);
    if (BURROW_OK(perr) && lead.len == 3) {
        const Byte *p = (const Byte *)lead.p;
        if (p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) {
            Error derr = BURROW_NO_ERROR;
            bufio_reader_discard(r->b, 3, &derr);
        }
    }
}

/* Room for at least one more byte in r->buf, doubling it as append does. */
static void gbr_grow(GbrReader *r) {
    Int ncap = r->cap < 256 ? 512 : r->cap * 2;
    Byte *nb = (Byte *)mem_alloc_nozero(r->a, (size_t)ncap, 1);
    if (nb == NULL)
        gbr_oom();
    if (r->len > 0)
        memcpy(nb, r->buf, (size_t)r->len);
    if (r->buf != NULL && !r->pinned)
        mem_free(r->a, r->buf, (size_t)r->cap, 1);
    r->buf = nb;
    r->cap = ncap;
}

static void gbr_syntax_error(GbrReader *r) {
    if (BURROW_OK(r->err))
        r->err = gbr_err_syntax;
}

static Byte gbr_read_byte(GbrReader *r) {
    Error err = BURROW_NO_ERROR;
    Byte c = bufio_reader_read_byte(r->b, &err);
    if (BURROW_OK(err)) {
        if (r->len == r->cap)
            gbr_grow(r);
        r->buf[r->len++] = c;
        if (c == 0)
            err = gbr_err_nul;
    }
    if (BURROW_FAILED(err)) {
        if (gbr_is_eof(err))
            r->eof = true;
        else if (BURROW_OK(r->err))
            r->err = err;
        c = 0;
    }
    return c;
}

/* readRest: the rest of the file into r->buf. */
static void gbr_read_rest(GbrReader *r) {
    for (;;) {
        if (r->len == r->cap)
            gbr_grow(r);
        Error err = BURROW_NO_ERROR;
        Slice room =
            slice_from(r->buf + r->len, r->cap - r->len, r->cap - r->len, TYPE_BYTE);
        Int n = bufio_reader_read(r->b, room, &err);
        r->len += n;
        if (BURROW_FAILED(err)) {
            if (gbr_is_eof(err))
                r->eof = true;
            else if (BURROW_OK(r->err))
                r->err = err;
            break;
        }
    }
}

/* peekByte: the next byte, not consumed, after spaces and comments when
 * skip_space is set. Semicolons count as spaces. */
static Byte gbr_peek_byte(GbrReader *r, bool skip_space) {
    if (BURROW_FAILED(r->err)) {
        r->nerr++;
        if (r->nerr > 10000)
            panic_str(S("go/build: import reader looping"));
        return 0;
    }

    /* r->peek may be left by gbr_peek_byte(r, false), and this may be a
     * skip_space call, so it is the first byte and not the answer. */
    Byte c = r->peek;
    if (c == 0)
        c = gbr_read_byte(r);
    while (BURROW_OK(r->err) && !r->eof) {
        if (skip_space) {
            if (c == ' ' || c == '\f' || c == '\t' || c == '\r' || c == '\n' ||
                c == ';') {
                c = gbr_read_byte(r);
                continue;
            }
            if (c == '/') {
                c = gbr_read_byte(r);
                if (c == '/') {
                    while (c != '\n' && BURROW_OK(r->err) && !r->eof)
                        c = gbr_read_byte(r);
                } else if (c == '*') {
                    Byte c1 = 0;
                    while ((c != '*' || c1 != '/') && BURROW_OK(r->err)) {
                        if (r->eof)
                            gbr_syntax_error(r);
                        c = c1;
                        c1 = gbr_read_byte(r);
                    }
                } else {
                    gbr_syntax_error(r);
                }
                c = gbr_read_byte(r);
                continue;
            }
        }
        break;
    }
    r->peek = c;
    return r->peek;
}

static Byte gbr_next_byte(GbrReader *r, bool skip_space) {
    Byte c = gbr_peek_byte(r, skip_space);
    r->peek = 0;
    return c;
}

static bool gbr_is_ident(Byte c) {
    return ('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z') || ('0' <= c && c <= '9') ||
           c == '_' || c >= UTF8_RUNE_SELF;
}

static void gbr_read_keyword(GbrReader *r, Str kw) {
    gbr_peek_byte(r, true);
    for (Int i = 0; i < kw.len; i++) {
        if (gbr_next_byte(r, false) != kw.p[i]) {
            gbr_syntax_error(r);
            return;
        }
    }
    if (gbr_is_ident(gbr_peek_byte(r, false)))
        gbr_syntax_error(r);
}

static void gbr_read_ident(GbrReader *r) {
    Byte c = gbr_peek_byte(r, true);
    if (!gbr_is_ident(c)) {
        gbr_syntax_error(r);
        return;
    }
    while (gbr_is_ident(gbr_peek_byte(r, false)))
        r->peek = 0;
}

static void gbr_read_string(GbrReader *r) {
    Byte q = gbr_next_byte(r, true);
    if (q == '`') {
        while (BURROW_OK(r->err)) {
            if (gbr_next_byte(r, false) == '`')
                break;
            if (r->eof)
                gbr_syntax_error(r);
        }
    } else if (q == '"') {
        while (BURROW_OK(r->err)) {
            Byte c = gbr_next_byte(r, false);
            if (c == '"')
                break;
            if (r->eof || c == '\n')
                gbr_syntax_error(r);
            if (c == '\\')
                gbr_next_byte(r, false);
        }
    } else {
        gbr_syntax_error(r);
    }
}

/* readImport: an optional name and a quoted path. */
static void gbr_read_import(GbrReader *r) {
    Byte c = gbr_peek_byte(r, true);
    if (c == '.')
        r->peek = 0;
    else if (gbr_is_ident(c))
        gbr_read_ident(r);
    gbr_read_string(r);
}

static Str gbr_buf(GbrReader *r, Int n) {
    r->pinned = true;
    return n == 0 ? BURROW_STR_EMPTY : str_from_bytes(r->buf, n);
}

Str burrow__build_read_comments(Alloc *a, IoReader f, Error *err) {
    GbrReader r;
    gbr_init(&r, a, BURROW_STR_EMPTY, f);
    gbr_peek_byte(&r, true);
    Int n = r.len;
    if (BURROW_OK(r.err) && !r.eof) {
        /* Not at EOF, so a byte that is not space was read. Drop it. */
        n--;
    }
    Str out = gbr_buf(&r, n);
    *err = r.err;
    bufio_reader_free(r.b);
    return out;
}

/* ---------------------------------------------------------- readGoInfo */

/* isValidImport: the stricter import path check the spec allows, which the
 * parser no longer makes. */
static bool gbr_is_valid_import(Str s) {
    static const Str illegal = BURROW_S_INIT("!\"#$%&'()*,:;<=>?[\\]^{|}`\xef\xbf\xbd");
    Int i = 0;
    while (i < s.len) {
        Int size = 0;
        Rune r = utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &size);
        if (!unicode_is_graphic(r) || unicode_is_space(r) ||
            strings_contains_rune(illegal, r))
            return false;
        i += size;
    }
    return s.len != 0;
}

/* parseGoEmbed: the patterns of a //go:embed comment at pos, unquoted or in
 * either kind of Go string. A comment that is not one gives none. */
static Error gbr_parse_go_embed(Alloc *a, TokenFileSet *fset, TokenPos pos, Str comment,
                                Slice *list) {
    bool ok = false;
    AstDirective dir = ast_parse_directive(pos, comment, &ok);
    if (!ok || !str_eq(dir.tool, S("go")) || !str_eq(dir.name, S("embed")))
        return BURROW_NO_ERROR;
    Error err = BURROW_NO_ERROR;
    Slice args = ast_directive_parse_args(&dir, a, &err);
    if (BURROW_FAILED(err))
        return err;
    for (Int i = 0; i < args.len; i++) {
        AstDirectiveArg *arg = &BURROW_AT(AstDirectiveArg, args, i);
        burrow__BuildFileEmbed e = {arg->arg, token_file_set_position(fset, arg->pos)};
        gbr_append(a, list, &e);
    }
    return BURROW_NO_ERROR;
}

Error burrow__build_read_go_info(Alloc *a, IoReader f, burrow__BuildFileInfo *info) {
    GbrReader r;
    gbr_init(&r, a, info->name, f);

    gbr_read_keyword(&r, S("package"));
    gbr_read_ident(&r);
    while (gbr_peek_byte(&r, true) == 'i') {
        gbr_read_keyword(&r, S("import"));
        if (gbr_peek_byte(&r, true) == '(') {
            gbr_next_byte(&r, false);
            while (gbr_peek_byte(&r, true) != ')' && BURROW_OK(r.err))
                gbr_read_import(&r);
            gbr_next_byte(&r, false);
        } else {
            gbr_read_import(&r);
        }
    }

    /* Stopping cleanly before EOF means a byte was read that says the imports
     * are over, and it would be a syntax error to the parser, so it stays
     * out. */
    if (BURROW_OK(r.err) && !r.eof)
        info->header = gbr_buf(&r, r.len - 1);
    else
        info->header = gbr_buf(&r, r.len);

    /* After a syntax error read the whole file, so that the errors are the
     * ones the parser would give. */
    if (r.err.vt == gbr_err_syntax.vt && r.err.data == gbr_err_syntax.data) {
        r.err = BURROW_NO_ERROR;
        gbr_read_rest(&r);
        info->header = gbr_buf(&r, r.len);
    }
    if (BURROW_FAILED(r.err)) {
        bufio_reader_free(r.b);
        return r.err;
    }

    if (info->fset == NULL) {
        bufio_reader_free(r.b);
        return BURROW_NO_ERROR;
    }

    Str header = info->header;
    Error perr = BURROW_NO_ERROR;
    info->parsed = parser_parse_file(
        a, info->fset, info->name, BURROW_ANY(TYPE_STRING, &header),
        PARSER_IMPORTS_ONLY | PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION,
        &perr);
    info->parse_err = perr;
    if (BURROW_FAILED(perr)) {
        bufio_reader_free(r.b);
        return BURROW_NO_ERROR;
    }

    bool has_embed = false;
    AstFile *file = info->parsed;
    for (Int i = 0; i < file->decls.len; i++) {
        AstDecl decl = BURROW_AT(AstDecl, file->decls, i);
        if (decl->kind != AST_KIND_GEN_DECL)
            continue;
        AstGenDecl *d = (AstGenDecl *)decl;
        for (Int j = 0; j < d->specs.len; j++) {
            AstSpec dspec = BURROW_AT(AstSpec, d->specs, j);
            if (dspec->kind != AST_KIND_IMPORT_SPEC)
                continue;
            AstImportSpec *spec = (AstImportSpec *)dspec;
            Str quoted = spec->path->value;
            Error uerr = BURROW_NO_ERROR;
            Str path = strconv_unquote(a, quoted, &uerr);
            if (BURROW_FAILED(uerr)) {
                bufio_reader_free(r.b);
                return fmt_errorf_v("parser returned invalid quoted string: <%s>",
                                    quoted);
            }
            if (!gbr_is_valid_import(path)) {
                /* The parser used to report these and no longer does. Go makes
                 * a lone *scanner.Error, and a list of one says the same. */
                GoScannerErrorList list = {0};
                Str msg = gbr_cat(a, S("invalid import path: "), path);
                go_scanner_error_list_add(
                    &list, error_allocator(),
                    token_file_set_position(info->fset, ast_node_pos(dspec)), msg);
                info->parse_err = go_scanner_error_list_err(list);
                info->imports = slice_nil(&gbr_file_import_desc);
                bufio_reader_free(r.b);
                return BURROW_NO_ERROR;
            }
            if (str_eq(path, S("embed")))
                has_embed = true;

            AstCommentGroup *doc = spec->doc;
            if (doc == NULL && d->specs.len == 1)
                doc = d->doc;
            burrow__BuildFileImport fi = {path, ast_node_pos(dspec), doc};
            if (info->imports.elem == NULL)
                info->imports = slice_nil(&gbr_file_import_desc);
            gbr_append(a, &info->imports, &fi);
        }
    }

    /* The //go: comments before the package clause. */
    for (Int i = 0; i < file->comments.len; i++) {
        AstCommentGroup *group = BURROW_AT(AstCommentGroup *, file->comments, i);
        if (ast_comment_group_pos(group) >= file->package)
            break;
        for (Int j = 0; j < group->list.len; j++) {
            AstComment *c = BURROW_AT(AstComment *, group->list, j);
            if (strings_has_prefix(c->text, S("//go:"))) {
                BuildDirective dv = {c->text,
                                     token_file_set_position(info->fset, c->slash)};
                if (info->directives.elem == NULL)
                    info->directives = slice_nil(TYPE_BUILD_DIRECTIVE);
                gbr_append(a, &info->directives, &dv);
            }
        }
    }

    /* With "embed" imported the patterns of the //go:embed comments in the
     * rest of the file are wanted too. The compiler checks where they are and
     * rejects any up by the imports, so those can be taken along here. */
    if (has_embed) {
        gbr_read_rest(&r);
        TokenFileSet *fset = token_new_file_set(a);
        if (fset == NULL)
            gbr_oom();
        TokenFile *tf = token_file_set_add_file(fset, r.filename, -1, r.len);
        GoScanner sc;
        go_scanner_init(&sc, a, tf, slice_from(r.buf, r.len, r.len, TYPE_BYTE),
                        (GoScannerErrorHandler){0}, GO_SCANNER_SCAN_COMMENTS);
        if (info->embeds.elem == NULL)
            info->embeds = slice_nil(&gbr_file_embed_desc);
        for (;;) {
            Token tok = 0;
            Str lit = BURROW_STR_EMPTY;
            TokenPos pos = go_scanner_scan(&sc, &tok, &lit);
            if (tok == TOKEN_EOF)
                break;
            if (tok == TOKEN_COMMENT && strings_has_prefix(lit, S("//go:embed"))) {
                /* A bad line is left for the compiler to report, so that go
                 * list can still say what it knows. */
                Slice embs = slice_nil(&gbr_file_embed_desc);
                Error eerr = gbr_parse_go_embed(a, fset, pos, lit, &embs);
                if (BURROW_OK(eerr) && embs.len > 0) {
                    Slice next = slice_append_slice(a, info->embeds, embs);
                    if (next.len != info->embeds.len + embs.len)
                        gbr_oom();
                    info->embeds = next;
                }
            }
        }
    }

    bufio_reader_free(r.b);
    return BURROW_NO_ERROR;
}
