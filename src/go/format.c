/* go/format: standard formatting of Go source.
 *
 * Derived from Go's src/go/format/format.go and internal.go.
 * Go source: go1.27.1.
 *
 * Go's parse hands back a closure, sourceAdj, that cuts the wrapping it added
 * to a fragment back off the output. There are only ever two of them, one for
 * a list of declarations and one for a list of statements, so here they are
 * an enum and fm_source_adj switches on it.
 *
 * Copyright 2012 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/format.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/go/ast.h"
#include "burrow/go/parser.h"
#include "burrow/go/printer.h"
#include "burrow/iface.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define S(lit) BURROW_S(lit)

/* Keep these in sync with cmd/gofmt, as Go's comment says. */
#define FM_TAB_WIDTH 8
#define FM_PRINTER_MODE                                                                \
    (PRINTER_USE_SPACES | PRINTER_TAB_INDENT | BURROW__PRINTER_NORMALIZE_NUMBERS)
#define FM_PARSER_MODE (PARSER_PARSE_COMMENTS | PARSER_SKIP_OBJECT_RESOLUTION)

BURROW_NORETURN static void fm_oom(void) {
    panic_str(S("go/format: out of memory"));
}

/* What parse wrapped a fragment in, so that format can take it off again. */
typedef enum FmAdj {
    FM_ADJ_NONE, /* a whole file, nothing to take off */
    FM_ADJ_DECLS,
    FM_ADJ_STMTS
} FmAdj;

/* prefix, then src, then suffix, in a new slice from a. */
static Slice fm_wrap(Alloc *a, Str prefix, Slice src, Str suffix) {
    Int n = prefix.len + src.len + suffix.len;
    Slice out = slice_make(a, TYPE_BYTE, n, n);
    if (out.p == NULL) /* a zero length slice is not nil, so this is only OOM */
        fm_oom();
    Byte *p = (Byte *)out.p;
    if (prefix.len > 0)
        memcpy(p, prefix.p, (size_t)prefix.len);
    if (src.len > 0)
        memcpy(p + prefix.len, src.p, (size_t)src.len);
    if (suffix.len > 0)
        memcpy(p + prefix.len + src.len, suffix.p, (size_t)suffix.len);
    return out;
}

/* The *ast.File in node, when node holds one, or NULL. A file can come as its
 * own pointer type or inside one of go/ast's interfaces, which is how Go's any
 * holding an *ast.File looks once it has been through a typed slot. */
static AstFile *fm_as_file(Any node, bool *is_file) {
    *is_file = false;
    if (node.t == NULL || node.data == NULL)
        return NULL;
    if (node.t == TYPE_AST_FILE_PTR) {
        *is_file = true;
        return *(AstFile *const *)node.data;
    }
    if (node.t == TYPE_AST_NODE) {
        AstNode n = *(AstNode const *)node.data;
        if (n != NULL && n->kind == AST_KIND_FILE) {
            *is_file = true;
            return (AstFile *)n;
        }
    }
    return NULL;
}

/* hasUnsortedImports. */
static bool fm_has_unsorted_imports(const AstFile *file) {
    for (Int i = 0; i < file->decls.len; i++) {
        AstDecl d = ((AstDecl *)file->decls.p)[i];
        if (d == NULL || d->kind != AST_KIND_GEN_DECL)
            return false; /* not an import declaration, and imports come first */
        const AstGenDecl *g = (const AstGenDecl *)d;
        if (g->tok != TOKEN_IMPORT)
            return false;
        /* For now assume all grouped imports are unsorted, as Go does. */
        if (token_pos_is_valid(g->lparen))
            return true;
        /* ungrouped imports are sorted by default */
    }
    return false;
}

static PrinterConfig fm_config(Int indent) {
    PrinterConfig cfg = {FM_PRINTER_MODE, FM_TAB_WIDTH, indent};
    return cfg;
}

Error format_node(Alloc *a, IoWriter dst, TokenFileSet *fset, Any node) {
    PrinterConfig cfg = fm_config(0);

    /* whether this is a complete source file (file != NULL) */
    bool is_file = false;
    AstFile *file = fm_as_file(node, &is_file);
    const PrinterCommentedNode *cnode = NULL;
    if (!is_file && node.t == TYPE_PRINTER_COMMENTED_NODE_PTR && node.data != NULL) {
        const PrinterCommentedNode *n = *(PrinterCommentedNode *const *)node.data;
        if (n != NULL) {
            file = fm_as_file(n->node, &is_file);
            if (is_file)
                cnode = n;
        }
    }
    if (file == NULL || !fm_has_unsorted_imports(file))
        return printer_config_fprint(&cfg, a, dst, fset, node);

    /* ast_sort_imports changes the tree, so sort a copy, which is printing the
     * file and parsing it back. Go has a TODO to do this more efficiently. */
    BytesBuffer buf = BYTES_BUFFER(a);
    Error err = printer_config_fprint(&cfg, a, bytes_buffer_as_io_writer(&buf), fset,
                                      BURROW_ANY(TYPE_AST_FILE_PTR, &file));
    if (BURROW_FAILED(err)) {
        bytes_buffer_free(&buf);
        return err;
    }
    Slice text = bytes_buffer_bytes(&buf);
    AstFile *copy = parser_parse_file(
        a, fset, BURROW_STR_EMPTY, BURROW_ANY(TYPE_BYTES, &text), FM_PARSER_MODE, &err);
    bytes_buffer_free(&buf);
    if (BURROW_FAILED(err)) /* should never happen, so say where it came from */
        return fmt_errorf_v("format.Node internal error (%s)", err);
    ast_sort_imports(a, fset, copy);

    /* print the new file, with the sorted imports */
    if (cnode != NULL) {
        PrinterCommentedNode n = {BURROW_ANY(TYPE_AST_FILE_PTR, &copy),
                                  cnode->comments};
        PrinterCommentedNode *np = &n;
        return printer_config_fprint(&cfg, a, dst, fset,
                                     BURROW_ANY(TYPE_PRINTER_COMMENTED_NODE_PTR, &np));
    }
    return printer_config_fprint(&cfg, a, dst, fset,
                                 BURROW_ANY(TYPE_AST_FILE_PTR, &copy));
}

/* parse, from internal.go: src as a Go source file, a list of declarations or
 * a list of statements, tried in that order. */
static AstFile *fm_parse(Alloc *a, TokenFileSet *fset, Slice src, bool fragment_ok,
                         FmAdj *adj, Int *indent_adj, Error *err) {
    *adj = FM_ADJ_NONE;
    *indent_adj = 0;

    /* Try as a whole source file. Stop on success, or on any error other than
     * the source not starting with a package clause. */
    AstFile *file = parser_parse_file(
        a, fset, BURROW_STR_EMPTY, BURROW_ANY(TYPE_BYTES, &src), FM_PARSER_MODE, err);
    if (BURROW_OK(*err) || !fragment_ok ||
        !strings_contains(error_text(*err), S("expected 'package'")))
        return file;

    /* A declaration list becomes a file with a package clause in front. It
     * goes in with a ';', not a newline, so the line numbers match src's. */
    Slice psrc = fm_wrap(a, S("package p;"), src, BURROW_STR_EMPTY);
    *err = BURROW_NO_ERROR;
    file = parser_parse_file(a, fset, BURROW_STR_EMPTY, BURROW_ANY(TYPE_BYTES, &psrc),
                             FM_PARSER_MODE, err);
    if (BURROW_OK(*err)) {
        *adj = FM_ADJ_DECLS;
        return file;
    }
    /* Fall through to a statement list only if the source did not start with
     * a declaration. */
    if (!strings_contains(error_text(*err), S("expected declaration")))
        return file;

    /* A statement list, or an expression, becomes the body of a function. The
     * extra '\n' before the '}' makes sure comments are flushed before it. */
    Slice fsrc = fm_wrap(a, S("package p; func _() {"), src, S("\n\n}"));
    *err = BURROW_NO_ERROR;
    file = parser_parse_file(a, fset, BURROW_STR_EMPTY, BURROW_ANY(TYPE_BYTES, &fsrc),
                             FM_PARSER_MODE, err);
    if (BURROW_OK(*err)) {
        *adj = FM_ADJ_STMTS;
        /* gofmt also indented the function body one level */
        *indent_adj = -1;
    }
    return file;
}

/* The sourceAdj closures parse makes: what the wrapping turned into, cut off
 * the formatted output again. */
static Slice fm_source_adj(FmAdj adj, Slice out, Int indent) {
    if (adj == FM_ADJ_DECLS) {
        /* gofmt has turned the ';' into a '\n' */
        out = slice_sub(out, indent + (Int)(sizeof "package p\n" - 1), out.len);
        return bytes_trim_space(out);
    }
    if (indent < 0)
        indent = 0;
    /* Gofmt has turned the "; " into a "\n\n", and there are two non-blank
     * lines with indent, hence 2*indent. */
    out = slice_sub(out, 2 * indent + (Int)(sizeof "package p\n\nfunc _() {" - 1),
                    out.len);
    /* only the "}\n" suffix, the rest of the space is trimmed anyway */
    out = slice_sub(out, 0, out.len - 2);
    return bytes_trim_space(out);
}

/* isSpace: ' ', '\t', '\n' and '\r'. */
static bool fm_is_space(Byte b) {
    return b == ' ' || b == '\t' || b == '\n' || b == '\r';
}

/* format, from internal.go: file printed with cfg, and for a fragment, the
 * wrapping taken off and src's leading and trailing space put back. */
static Slice fm_format(Alloc *a, TokenFileSet *fset, AstFile *file, FmAdj adj,
                       Int indent_adj, Slice src, PrinterConfig cfg, Error *err) {
    BytesBuffer buf = BYTES_BUFFER(a);
    if (adj == FM_ADJ_NONE) {
        /* a complete source file */
        *err = printer_config_fprint(&cfg, a, bytes_buffer_as_io_writer(&buf), fset,
                                     BURROW_ANY(TYPE_AST_FILE_PTR, &file));
        Slice res = slice_nil(TYPE_BYTE);
        if (BURROW_OK(*err))
            res = fm_wrap(a, BURROW_STR_EMPTY, bytes_buffer_bytes(&buf),
                          BURROW_STR_EMPTY);
        bytes_buffer_free(&buf);
        return res;
    }

    /* A partial source file. The leading space goes in front as it was. */
    const Byte *s = (const Byte *)src.p;
    Int i = 0;
    Int j = 0;
    while (j < src.len && fm_is_space(s[j])) {
        if (s[j] == '\n')
            i = j + 1; /* byte offset of the last line in the leading space */
        j++;
    }

    /* Then the indentation of the first code line. Spaces are ignored unless
     * there are no tabs, in which case spaces count as one tab. */
    Int indent = 0;
    bool has_space = false;
    for (Int k = i; k < j; k++) {
        if (s[k] == ' ')
            has_space = true;
        else if (s[k] == '\t')
            indent++;
    }
    if (indent == 0 && has_space)
        indent = 1;

    /* Format the source, without any leading and trailing space. */
    cfg.indent = indent + indent_adj;
    *err = printer_config_fprint(&cfg, a, bytes_buffer_as_io_writer(&buf), fset,
                                 BURROW_ANY(TYPE_AST_FILE_PTR, &file));
    if (BURROW_FAILED(*err)) {
        bytes_buffer_free(&buf);
        return slice_nil(TYPE_BYTE);
    }
    Slice out = fm_source_adj(adj, bytes_buffer_bytes(&buf), cfg.indent);

    /* An empty result means the source was empty but for space, and the
     * result is the source. Go hands back src itself; this copies it, so the
     * result is always the caller's to keep. */
    if (out.len == 0) {
        bytes_buffer_free(&buf);
        return fm_wrap(a, BURROW_STR_EMPTY, src, BURROW_STR_EMPTY);
    }

    /* the trailing space of src goes on the end */
    Int t = src.len;
    while (t > 0 && fm_is_space(s[t - 1]))
        t--;

    Int n = i + indent + out.len + (src.len - t);
    Slice res = slice_make(a, TYPE_BYTE, n, n);
    if (res.p == NULL)
        fm_oom();
    Byte *p = (Byte *)res.p;
    if (i > 0)
        memcpy(p, s, (size_t)i);
    p += i;
    for (Int k = 0; k < indent; k++)
        *p++ = '\t';
    memcpy(p, out.p, (size_t)out.len);
    p += out.len;
    if (t < src.len)
        memcpy(p, s + t, (size_t)(src.len - t));
    bytes_buffer_free(&buf);
    return res;
}

Slice format_source(Alloc *a, Slice src, Error *err) {
    *err = BURROW_NO_ERROR;
    TokenFileSet *fset = token_new_file_set(a);
    if (fset == NULL)
        fm_oom();
    FmAdj adj = FM_ADJ_NONE;
    Int indent_adj = 0;
    AstFile *file = fm_parse(a, fset, src, true, &adj, &indent_adj, err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_BYTE);

    if (adj == FM_ADJ_NONE) {
        /* a complete source file; Go has a TODO to consider doing this always */
        ast_sort_imports(a, fset, file);
    }
    return fm_format(a, fset, file, adj, indent_adj, src, fm_config(0), err);
}
