/* go/token: tokens, positions, files and file sets.
 *
 * Go's FileSet keeps its files in an AVL tree keyed by their Pos ranges, and
 * the tree here is the same tree. What Go leaves to its collector is done with
 * two lists. A set keeps every file it made, removed ones included, and frees
 * them with itself, since a removed file may still be in a caller's hands. And
 * a tree node that is deleted while token_file_set_iterate is walking the tree
 * waits on a list until no walk is going on, because the walk keeps a pointer
 * to its node across the call into yield, with the lock let go, and finds its
 * way on from the node's key when it comes back.
 *
 * Derived from Go's src/go/token/token.go, position.go, tree.go and
 * serialize.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/token.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ tokens */

static const Str tk_tokens[BURROW__TOKEN_ADDITIONAL_END] = {
    [TOKEN_ILLEGAL] = BURROW_S_INIT("ILLEGAL"),

    [TOKEN_EOF] = BURROW_S_INIT("EOF"),
    [TOKEN_COMMENT] = BURROW_S_INIT("COMMENT"),

    [TOKEN_IDENT] = BURROW_S_INIT("IDENT"),
    [TOKEN_INT] = BURROW_S_INIT("INT"),
    [TOKEN_FLOAT] = BURROW_S_INIT("FLOAT"),
    [TOKEN_IMAG] = BURROW_S_INIT("IMAG"),
    [TOKEN_CHAR] = BURROW_S_INIT("CHAR"),
    [TOKEN_STRING] = BURROW_S_INIT("STRING"),

    [TOKEN_ADD] = BURROW_S_INIT("+"),
    [TOKEN_SUB] = BURROW_S_INIT("-"),
    [TOKEN_MUL] = BURROW_S_INIT("*"),
    [TOKEN_QUO] = BURROW_S_INIT("/"),
    [TOKEN_REM] = BURROW_S_INIT("%"),

    [TOKEN_AND] = BURROW_S_INIT("&"),
    [TOKEN_OR] = BURROW_S_INIT("|"),
    [TOKEN_XOR] = BURROW_S_INIT("^"),
    [TOKEN_SHL] = BURROW_S_INIT("<<"),
    [TOKEN_SHR] = BURROW_S_INIT(">>"),
    [TOKEN_AND_NOT] = BURROW_S_INIT("&^"),

    [TOKEN_ADD_ASSIGN] = BURROW_S_INIT("+="),
    [TOKEN_SUB_ASSIGN] = BURROW_S_INIT("-="),
    [TOKEN_MUL_ASSIGN] = BURROW_S_INIT("*="),
    [TOKEN_QUO_ASSIGN] = BURROW_S_INIT("/="),
    [TOKEN_REM_ASSIGN] = BURROW_S_INIT("%="),

    [TOKEN_AND_ASSIGN] = BURROW_S_INIT("&="),
    [TOKEN_OR_ASSIGN] = BURROW_S_INIT("|="),
    [TOKEN_XOR_ASSIGN] = BURROW_S_INIT("^="),
    [TOKEN_SHL_ASSIGN] = BURROW_S_INIT("<<="),
    [TOKEN_SHR_ASSIGN] = BURROW_S_INIT(">>="),
    [TOKEN_AND_NOT_ASSIGN] = BURROW_S_INIT("&^="),

    [TOKEN_LAND] = BURROW_S_INIT("&&"),
    [TOKEN_LOR] = BURROW_S_INIT("||"),
    [TOKEN_ARROW] = BURROW_S_INIT("<-"),
    [TOKEN_INC] = BURROW_S_INIT("++"),
    [TOKEN_DEC] = BURROW_S_INIT("--"),

    [TOKEN_EQL] = BURROW_S_INIT("=="),
    [TOKEN_LSS] = BURROW_S_INIT("<"),
    [TOKEN_GTR] = BURROW_S_INIT(">"),
    [TOKEN_ASSIGN] = BURROW_S_INIT("="),
    [TOKEN_NOT] = BURROW_S_INIT("!"),

    [TOKEN_NEQ] = BURROW_S_INIT("!="),
    [TOKEN_LEQ] = BURROW_S_INIT("<="),
    [TOKEN_GEQ] = BURROW_S_INIT(">="),
    [TOKEN_DEFINE] = BURROW_S_INIT(":="),
    [TOKEN_ELLIPSIS] = BURROW_S_INIT("..."),

    [TOKEN_LPAREN] = BURROW_S_INIT("("),
    [TOKEN_LBRACK] = BURROW_S_INIT("["),
    [TOKEN_LBRACE] = BURROW_S_INIT("{"),
    [TOKEN_COMMA] = BURROW_S_INIT(","),
    [TOKEN_PERIOD] = BURROW_S_INIT("."),

    [TOKEN_RPAREN] = BURROW_S_INIT(")"),
    [TOKEN_RBRACK] = BURROW_S_INIT("]"),
    [TOKEN_RBRACE] = BURROW_S_INIT("}"),
    [TOKEN_SEMICOLON] = BURROW_S_INIT(";"),
    [TOKEN_COLON] = BURROW_S_INIT(":"),

    [TOKEN_BREAK] = BURROW_S_INIT("break"),
    [TOKEN_CASE] = BURROW_S_INIT("case"),
    [TOKEN_CHAN] = BURROW_S_INIT("chan"),
    [TOKEN_CONST] = BURROW_S_INIT("const"),
    [TOKEN_CONTINUE] = BURROW_S_INIT("continue"),

    [TOKEN_DEFAULT] = BURROW_S_INIT("default"),
    [TOKEN_DEFER] = BURROW_S_INIT("defer"),
    [TOKEN_ELSE] = BURROW_S_INIT("else"),
    [TOKEN_FALLTHROUGH] = BURROW_S_INIT("fallthrough"),
    [TOKEN_FOR] = BURROW_S_INIT("for"),

    [TOKEN_FUNC] = BURROW_S_INIT("func"),
    [TOKEN_GO] = BURROW_S_INIT("go"),
    [TOKEN_GOTO] = BURROW_S_INIT("goto"),
    [TOKEN_IF] = BURROW_S_INIT("if"),
    [TOKEN_IMPORT] = BURROW_S_INIT("import"),

    [TOKEN_INTERFACE] = BURROW_S_INIT("interface"),
    [TOKEN_MAP] = BURROW_S_INIT("map"),
    [TOKEN_PACKAGE] = BURROW_S_INIT("package"),
    [TOKEN_RANGE] = BURROW_S_INIT("range"),
    [TOKEN_RETURN] = BURROW_S_INIT("return"),

    [TOKEN_SELECT] = BURROW_S_INIT("select"),
    [TOKEN_STRUCT] = BURROW_S_INIT("struct"),
    [TOKEN_SWITCH] = BURROW_S_INIT("switch"),
    [TOKEN_TYPE_] = BURROW_S_INIT("type"),
    [TOKEN_VAR] = BURROW_S_INIT("var"),

    [TOKEN_TILDE] = BURROW_S_INIT("~"),
};

/* The text of tok, or the empty string when it has none. */
static Str tk_text(Token tok) {
    if (tok >= 0 && tok < BURROW__TOKEN_ADDITIONAL_END)
        return tk_tokens[tok];
    return BURROW_STR_EMPTY;
}

Str token_string(Token tok, Alloc *a) {
    Str s = tk_text(tok);
    if (s.len == 0)
        return fmt_sprintf_v(a, "token(%d)", tok);
    return str_clone(a, s);
}

Int token_precedence(Token op) {
    switch (op) {
    case TOKEN_LOR:
        return 1;
    case TOKEN_LAND:
        return 2;
    case TOKEN_EQL:
    case TOKEN_NEQ:
    case TOKEN_LSS:
    case TOKEN_LEQ:
    case TOKEN_GTR:
    case TOKEN_GEQ:
        return 3;
    case TOKEN_ADD:
    case TOKEN_SUB:
    case TOKEN_OR:
    case TOKEN_XOR:
        return 4;
    case TOKEN_MUL:
    case TOKEN_QUO:
    case TOKEN_REM:
    case TOKEN_SHL:
    case TOKEN_SHR:
    case TOKEN_AND:
    case TOKEN_AND_NOT:
        return 5;
    default:
        return TOKEN_LOWEST_PREC;
    }
}

/* Go keeps the keywords in a map. There are 25 of them and the table is in
 * order already, so this looks through it, skipping on the first byte. */
static Token tk_keyword(Str ident) {
    if (ident.len < 2 || ident.len > 11)
        return TOKEN_ILLEGAL;
    for (Token i = BURROW__TOKEN_KEYWORD_BEG + 1; i < BURROW__TOKEN_KEYWORD_END; i++) {
        if (tk_tokens[i].p[0] == ident.p[0] && str_eq(tk_tokens[i], ident))
            return i;
    }
    return TOKEN_ILLEGAL;
}

Token token_lookup(Str ident) {
    Token tok = tk_keyword(ident);
    return tok != TOKEN_ILLEGAL ? tok : TOKEN_IDENT;
}

bool token_is_literal(Token tok) {
    return BURROW__TOKEN_LITERAL_BEG < tok && tok < BURROW__TOKEN_LITERAL_END;
}

bool token_is_operator(Token tok) {
    return (BURROW__TOKEN_OPERATOR_BEG < tok && tok < BURROW__TOKEN_OPERATOR_END) ||
           tok == TOKEN_TILDE;
}

bool token_is_keyword(Token tok) {
    return BURROW__TOKEN_KEYWORD_BEG < tok && tok < BURROW__TOKEN_KEYWORD_END;
}

bool token_is_exported(Str name) {
    Int size = 0;
    Rune ch = utf8_decode_rune_in_string(name, &size);
    return unicode_is_upper(ch);
}

bool token_is_keyword_str(Str name) {
    return tk_keyword(name) != TOKEN_ILLEGAL;
}

bool token_is_identifier(Str name) {
    if (name.len == 0 || token_is_keyword_str(name))
        return false;
    for (Int i = 0; i < name.len;) {
        Int size = 0;
        Rune c =
            utf8_decode_rune_in_string(str_from_bytes(name.p + i, name.len - i), &size);
        if (!unicode_is_letter(c) && c != '_' && (i == 0 || !unicode_is_digit(c)))
            return false;
        i += size;
    }
    return true;
}

/* --------------------------------------------------------------- positions */

bool token_position_is_valid(const TokenPosition *pos) {
    return pos->line > 0;
}

Str token_position_string(TokenPosition pos, Alloc *a) {
    bool named = pos.filename.len > 0;
    if (token_position_is_valid(&pos)) {
        if (named && pos.column != 0)
            return fmt_sprintf_v(a, "%s:%d:%d", pos.filename, pos.line, pos.column);
        if (named)
            return fmt_sprintf_v(a, "%s:%d", pos.filename, pos.line);
        if (pos.column != 0)
            return fmt_sprintf_v(a, "%d:%d", pos.line, pos.column);
        return fmt_sprintf_v(a, "%d", pos.line);
    }
    if (named)
        return str_clone(a, pos.filename);
    return str_clone(a, BURROW_S("-"));
}

bool token_pos_is_valid(TokenPos p) {
    return p != TOKEN_NO_POS;
}

/* ------------------------------------------------------------- descriptors */

static Str tk_m_string(Token *self) {
    return token_string(*self, error_allocator());
}

#define TK_SIG_STRING(IN, OUT) OUT(Str)
#define TK_TOKEN_METHODS(M, T) M(T, String, tk_m_string, TK_SIG_STRING)

BURROW_METHODS_DEFINE(Token, TK_TOKEN_METHODS);

const Type burrow_type_Token = {
    {(const Byte *)"Token", 5},
    {(const Byte *)"go/token", 8},
    KIND_INT,
    (uint32_t)sizeof(Token),
    (uint16_t)_Alignof(Token),
    0,
    (uint16_t)(sizeof(burrow__methods_Token) / sizeof(burrow__methods_Token[0])),
    NULL,
    burrow__methods_Token,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Type burrow_type_TokenPos = {
    {(const Byte *)"Pos", 3},
    {(const Byte *)"go/token", 8},
    KIND_INT,
    (uint32_t)sizeof(TokenPos),
    (uint16_t)_Alignof(TokenPos),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* -------------------------------------------------------------------- files */

/* Go's lineInfo, with its fields capitalised for encoding/gob, which is what
 * serialize.go needs them for. The C names are the Go ones for the same
 * reason. */
#define TK_LINE_INFO_FIELDS(F, T)                                                      \
    F(T, Int, Offset, Offset, "")                                                      \
    F(T, Str, Filename, Filename, "")                                                  \
    F(T, Int, Line, Line, "")                                                          \
    F(T, Int, Column, Column, "")
BURROW_STRUCT_AS(TkLineInfo, TK_LINE_INFO_FIELDS);

struct TokenFile {
    Alloc *a;     /* the allocator of the set that made the file */
    Str name;     /* file name as provided to AddFile */
    Int base;     /* Pos value range for this file is [base...base+size] */
    Int size;     /* file size as provided to AddFile */
    SyncMutex mu; /* over lines and infos */
    Int *lines;   /* the offset of the first character of each line */
    Int nlines;
    Int cap_lines;
    TkLineInfo *infos;
    Int ninfos;
    Int cap_infos;
    TokenFile *next_own; /* the next file the same set made */
};

Str token_file_name(TokenFile *f) {
    return f->name;
}

Int token_file_base(TokenFile *f) {
    return f->base;
}

Int token_file_size(TokenFile *f) {
    return f->size;
}

TokenPos token_file_end(TokenFile *f) {
    return f->base + f->size;
}

Str token_file_string(TokenFile *f, Alloc *a) {
    return fmt_sprintf_v(a, "%s(%d-%d)", f->name, f->base, token_file_end(f));
}

Int token_file_line_count(TokenFile *f) {
    sync_mutex_lock(&f->mu);
    Int n = f->nlines;
    sync_mutex_unlock(&f->mu);
    return n;
}

/* Room for one more line, under the file's lock. False when the allocator
 * says no. */
static bool tk_grow_lines(TokenFile *f) {
    if (f->nlines < f->cap_lines)
        return true;
    Int ncap = f->cap_lines < 8 ? 8 : f->cap_lines * 2;
    Int *p = mem_realloc(f->a, f->lines, (size_t)f->cap_lines * sizeof(Int),
                         (size_t)ncap * sizeof(Int), _Alignof(Int));
    if (p == NULL)
        return false;
    f->lines = p;
    f->cap_lines = ncap;
    return true;
}

void token_file_add_line(TokenFile *f, Int offset) {
    sync_mutex_lock(&f->mu);
    Int i = f->nlines;
    if ((i == 0 || f->lines[i - 1] < offset) && offset < f->size && tk_grow_lines(f))
        f->lines[f->nlines++] = offset;
    sync_mutex_unlock(&f->mu);
}

void token_file_merge_line(TokenFile *f, Int line) {
    if (line < 1)
        panic_str(fmt_sprintf_v(error_allocator(),
                                "invalid line number %d (should be >= 1)", line));
    sync_mutex_lock(&f->mu);
    Int n = f->nlines;
    if (line >= n) {
        sync_mutex_unlock(&f->mu);
        panic_str(fmt_sprintf_v(error_allocator(),
                                "invalid line number %d (should be < %d)", line, n));
    }
    /* Merging line with line+1 removes the entry for line+1, which is at index
     * line since the lines count from 1. */
    for (Int i = line; i + 1 < n; i++)
        f->lines[i] = f->lines[i + 1];
    f->nlines--;
    sync_mutex_unlock(&f->mu);
}

Slice token_file_lines(TokenFile *f) {
    sync_mutex_lock(&f->mu);
    Slice s = slice_from(f->lines, f->nlines, f->nlines, TYPE_INT);
    sync_mutex_unlock(&f->mu);
    return s;
}

/* Swaps in a new line table of n lines with room for cap, under the file's
 * lock, and frees the old one. */
static void tk_replace_lines(TokenFile *f, Int *lines, Int n, Int cap) {
    sync_mutex_lock(&f->mu);
    Int *old = f->lines;
    Int old_cap = f->cap_lines;
    f->lines = lines;
    f->nlines = n;
    f->cap_lines = cap;
    sync_mutex_unlock(&f->mu);
    if (old != NULL)
        mem_free(f->a, old, (size_t)old_cap * sizeof(Int), _Alignof(Int));
}

bool token_file_set_lines(TokenFile *f, Slice lines) {
    /* verify validity of lines table */
    Int size = f->size;
    const Int *in = lines.p;
    for (Int i = 0; i < lines.len; i++) {
        if ((i > 0 && in[i] <= in[i - 1]) || size <= in[i])
            return false;
    }
    Int *p = NULL;
    if (lines.len > 0) {
        p = mem_alloc_array(f->a, (size_t)lines.len, sizeof(Int), _Alignof(Int));
        if (p == NULL)
            return false;
        for (Int i = 0; i < lines.len; i++)
            p[i] = in[i];
    }
    tk_replace_lines(f, p, lines.len, lines.len);
    return true;
}

void token_file_set_lines_for_content(TokenFile *f, Slice content) {
    const Byte *b = content.p;
    Int n = 0;
    Int line = 0;
    for (Int offset = 0; offset < content.len; offset++) {
        if (line >= 0)
            n++;
        line = -1;
        if (b[offset] == '\n')
            line = offset + 1;
    }
    Int *lines = NULL;
    if (n > 0) {
        lines = mem_alloc_array(f->a, (size_t)n, sizeof(Int), _Alignof(Int));
        if (lines == NULL)
            return;
    }
    Int i = 0;
    line = 0;
    for (Int offset = 0; offset < content.len; offset++) {
        if (line >= 0)
            lines[i++] = line;
        line = -1;
        if (b[offset] == '\n')
            line = offset + 1;
    }
    tk_replace_lines(f, lines, n, n);
}

TokenPos token_file_line_start(TokenFile *f, Int line) {
    if (line < 1)
        panic_str(fmt_sprintf_v(error_allocator(),
                                "invalid line number %d (should be >= 1)", line));
    sync_mutex_lock(&f->mu);
    Int n = f->nlines;
    if (line > n) {
        sync_mutex_unlock(&f->mu);
        panic_str(fmt_sprintf_v(error_allocator(),
                                "invalid line number %d (should be < %d)", line, n));
    }
    TokenPos p = f->base + f->lines[line - 1];
    sync_mutex_unlock(&f->mu);
    return p;
}

void token_file_add_line_info(TokenFile *f, Int offset, Str filename, Int line) {
    token_file_add_line_column_info(f, offset, filename, line, 1);
}

void token_file_add_line_column_info(TokenFile *f, Int offset, Str filename, Int line,
                                     Int column) {
    sync_mutex_lock(&f->mu);
    Int i = f->ninfos;
    if ((i == 0 || f->infos[i - 1].Offset < offset) && offset < f->size) {
        if (f->ninfos == f->cap_infos) {
            Int ncap = f->cap_infos < 4 ? 4 : f->cap_infos * 2;
            TkLineInfo *p =
                mem_realloc(f->a, f->infos, (size_t)f->cap_infos * sizeof(TkLineInfo),
                            (size_t)ncap * sizeof(TkLineInfo), _Alignof(TkLineInfo));
            if (p == NULL)
                goto out;
            f->infos = p;
            f->cap_infos = ncap;
        }
        Str name = str_clone(f->a, filename);
        if (name.len != filename.len)
            goto out;
        f->infos[f->ninfos++] = (TkLineInfo){offset, name, line, column};
    }
out:
    sync_mutex_unlock(&f->mu);
}

bool burrow__token_file_info(TokenFile *f, Int i, Int *offset, Str *filename, Int *line,
                             Int *column);

/* For the tests, which look at f.infos directly as Go's do. The i'th line info
 * of f, or false when f has no more than i. */
bool burrow__token_file_info(TokenFile *f, Int i, Int *offset, Str *filename, Int *line,
                             Int *column) {
    sync_mutex_lock(&f->mu);
    bool ok = i >= 0 && i < f->ninfos;
    if (ok) {
        *offset = f->infos[i].Offset;
        *filename = f->infos[i].Filename;
        *line = f->infos[i].Line;
        *column = f->infos[i].Column;
    }
    sync_mutex_unlock(&f->mu);
    return ok;
}

/* fixOffset: offset brought inside 0 to f's size. */
static Int tk_fix_offset(const TokenFile *f, Int offset) {
    if (offset > f->size)
        offset = f->size;
    return offset < 0 ? 0 : offset;
}

TokenPos token_file_pos(TokenFile *f, Int offset) {
    return f->base + tk_fix_offset(f, offset);
}

Int token_file_offset(TokenFile *f, TokenPos p) {
    return tk_fix_offset(f, p - f->base);
}

Int token_file_line(TokenFile *f, TokenPos p) {
    return token_file_position(f, p).line;
}

/* searchInts: the index of the last of the n ascending a that is at most x, or
 * -1 when there is none. */
static Int tk_search_ints(const Int *a, Int n, Int x) {
    Int i = 0;
    Int j = n;
    while (i < j) {
        Int h = (Int)((uint64_t)(i + j) >> 1);
        if (a[h] <= x)
            i = h + 1;
        else
            j = h;
    }
    return i - 1;
}

/* searchLineInfos: the index of the line info that covers offset x. */
static Int tk_search_line_infos(const TkLineInfo *a, Int n, Int x) {
    Int i = 0;
    Int j = n;
    while (i < j) {
        Int h = (Int)((uint64_t)(i + j) >> 1);
        if (a[h].Offset < x)
            i = h + 1;
        else
            j = h;
    }
    if (!(i < n && a[i].Offset == x)) {
        /* We want the lineInfo containing x, but if we didn't find x then i is
         * the next one. */
        i--;
    }
    return i;
}

/* unpack: the file name, line and column of offset, through the //line
 * information when adjusted is set. */
static TokenPosition tk_unpack(TokenFile *f, Int offset, bool adjusted) {
    TokenPosition pos = {f->name, offset, 0, 0};
    sync_mutex_lock(&f->mu);
    Int i = tk_search_ints(f->lines, f->nlines, offset);
    if (i >= 0) {
        pos.line = i + 1;
        pos.column = offset - f->lines[i] + 1;
    }
    if (adjusted && f->ninfos > 0) {
        /* few files have extra line infos */
        Int k = tk_search_line_infos(f->infos, f->ninfos, offset);
        if (k >= 0) {
            const TkLineInfo *alt = &f->infos[k];
            pos.filename = alt->Filename;
            Int j = tk_search_ints(f->lines, f->nlines, alt->Offset);
            if (j >= 0) {
                /* j+1 is the line at which the alternative position was
                 * recorded */
                Int d = pos.line -
                        (j + 1); /* line distance from alternative position base */
                pos.line = alt->Line + d;
                if (alt->Column == 0) {
                    /* alternative column is unknown => relative column is
                     * unknown (the current specification for line directives
                     * requires this to apply until the next PosBase/line
                     * directive, not just until the new newline) */
                    pos.column = 0;
                } else if (d == 0) {
                    /* the alternative position base is on the current line =>
                     * column is relative to alternative column */
                    pos.column = alt->Column + (offset - alt->Offset);
                }
            }
        }
    }
    sync_mutex_unlock(&f->mu);
    return pos;
}

static TokenPosition tk_position(TokenFile *f, TokenPos p, bool adjusted) {
    return tk_unpack(f, tk_fix_offset(f, p - f->base), adjusted);
}

TokenPosition token_file_position_for(TokenFile *f, TokenPos p, bool adjusted) {
    TokenPosition pos = {BURROW_STR_EMPTY, 0, 0, 0};
    if (p != TOKEN_NO_POS)
        pos = tk_position(f, p, adjusted);
    return pos;
}

TokenPosition token_file_position(TokenFile *f, TokenPos p) {
    return token_file_position_for(f, p, true);
}

/* --------------------------------------------------------------------- tree */

/* Go's tree.go: an AVL tree of the files in a set, ordered by their ranges,
 * where two ranges that overlap compare equal. */
typedef struct TkNode {
    struct TkNode *parent;
    struct TkNode *left;
    struct TkNode *right;
    TokenFile *file;
    Int key_start;   /* = file's base, but improves locality */
    Int key_end;     /* = file's end */
    int32_t balance; /* at most +-2 */
    int32_t height;
    struct TkNode *dead_next; /* on the set's list of nodes to free later */
} TkNode;

struct TokenFileSet {
    Alloc *a;
    SyncRWMutex mu;     /* protects the file set */
    Int base;           /* base offset for the next file */
    TkNode *root;       /* tree of files in ascending base order */
    TokenFile *last;    /* cache of last file looked up, atomic */
    TokenFile *own;     /* every file this set made */
    TkNode *dead;       /* deleted nodes a walk may still be holding */
    uint32_t iterating; /* walks going on, atomic */
};

static int tk_compare_key(Int xs, Int xe, Int ys, Int ye) {
    if (xe < ys)
        return -1;
    if (ye < xs)
        return +1;
    return 0;
}

/* locate: where a node with the key would be, and its parent. */
static TkNode **tk_locate(TokenFileSet *s, Int ks, Int ke, TkNode **parent) {
    TkNode **pos = &s->root;
    TkNode *x = s->root;
    *parent = NULL;
    while (x != NULL) {
        int sign = tk_compare_key(ks, ke, x->key_start, x->key_end);
        if (sign < 0) {
            pos = &x->left;
            *parent = x;
            x = x->left;
        } else if (sign > 0) {
            pos = &x->right;
            *parent = x;
            x = x->right;
        } else {
            break;
        }
    }
    return pos;
}

static TkNode *tk_next(TkNode *x) {
    if (x->right == NULL) {
        while (x->parent != NULL && x->parent->right == x)
            x = x->parent;
        return x->parent;
    }
    x = x->right;
    while (x->left != NULL)
        x = x->left;
    return x;
}

/* nextAfter: the node after where locate said a key would be. */
static TkNode *tk_next_after(TkNode **pos, TkNode *parent) {
    if (*pos != NULL)
        return tk_next(*pos);
    if (parent == NULL)
        return NULL;
    if (pos == &parent->left)
        return parent;
    return tk_next(parent);
}

static void tk_set_root(TokenFileSet *s, TkNode *x) {
    s->root = x;
    if (x != NULL)
        x->parent = NULL;
}

static void tk_set_left(TkNode *x, TkNode *y) {
    x->left = y;
    if (y != NULL)
        y->parent = x;
}

static void tk_set_right(TkNode *x, TkNode *y) {
    x->right = y;
    if (y != NULL)
        y->parent = x;
}

static int32_t tk_safe_height(const TkNode *n) {
    return n == NULL ? -1 : n->height;
}

static void tk_update(TkNode *n) {
    int32_t lheight = tk_safe_height(n->left);
    int32_t rheight = tk_safe_height(n->right);
    n->height = (lheight > rheight ? lheight : rheight) + 1;
    n->balance = rheight - lheight;
}

static void tk_replace_child(TokenFileSet *s, TkNode *parent, TkNode *old,
                             TkNode *new_) {
    if (parent == NULL) {
        if (s->root != old)
            panic_str(BURROW_S("corrupt tree"));
        tk_set_root(s, new_);
    } else if (parent->left == old) {
        tk_set_left(parent, new_);
    } else if (parent->right == old) {
        tk_set_right(parent, new_);
    } else {
        panic_str(BURROW_S("corrupt tree"));
    }
}

static TkNode *tk_rotate_right(TokenFileSet *s, TkNode *y) {
    TkNode *p = y->parent;
    TkNode *x = y->left;
    TkNode *b = x->right;
    tk_set_right(x, y);
    tk_set_left(y, b);
    tk_replace_child(s, p, y, x);
    tk_update(y);
    tk_update(x);
    return x;
}

static TkNode *tk_rotate_left(TokenFileSet *s, TkNode *x) {
    TkNode *p = x->parent;
    TkNode *y = x->right;
    TkNode *b = y->left;
    tk_set_left(y, x);
    tk_set_right(x, b);
    tk_replace_child(s, p, x, y);
    tk_update(x);
    tk_update(y);
    return y;
}

static void tk_rebalance_up(TokenFileSet *s, TkNode *x) {
    while (x != NULL) {
        int32_t h = x->height;
        tk_update(x);
        switch (x->balance) {
        case -2:
            if (x->left->balance == 1)
                tk_rotate_left(s, x->left);
            x = tk_rotate_right(s, x);
            break;
        case +2:
            if (x->right->balance == -1)
                tk_rotate_right(s, x->right);
            x = tk_rotate_left(s, x);
            break;
        default:
            break;
        }
        if (x->height == h) {
            /* No change in height of this subtree, so no change in balance of
             * the ancestors. */
            return;
        }
        x = x->parent;
    }
}

/* set: puts the node x for file at pos, which is empty. */
static void tk_set(TokenFileSet *s, TkNode *x, TokenFile *file, TkNode **pos,
                   TkNode *parent) {
    *x = (TkNode){0};
    x->file = file;
    x->key_start = file->base;
    x->key_end = file->base + file->size;
    x->parent = parent;
    x->height = -1;
    *pos = x;
    tk_rebalance_up(s, x);
}

static TkNode *tk_delete_min(TkNode **zpos) {
    while ((*zpos)->left != NULL)
        zpos = &(*zpos)->left;
    TkNode *z = *zpos;
    *zpos = z->right;
    if (*zpos != NULL)
        (*zpos)->parent = z->parent;
    return z;
}

static void tk_delete_swap(TokenFileSet *s, TkNode **pos) {
    TkNode *x = *pos;
    TkNode *z = tk_delete_min(&x->right);

    *pos = z;
    TkNode *unbalanced = z->parent; /* lowest potentially unbalanced node */
    if (unbalanced == x)
        unbalanced = z; /* (x a (z nil b)) -> (z a b) */
    z->parent = x->parent;
    z->height = x->height;
    z->balance = x->balance;
    tk_set_left(z, x->left);
    tk_set_right(z, x->right);

    tk_rebalance_up(s, unbalanced);
}

static void tk_free_node(TokenFileSet *s, TkNode *x) {
    mem_free(s->a, x, sizeof *x, _Alignof(TkNode));
}

/* delete: takes the node at pos out of the tree. It is freed now when no walk
 * is going on, and kept for later when one is. */
static void tk_delete(TokenFileSet *s, TkNode **pos) {
    TkNode *x = *pos;
    if (x->left == NULL) {
        *pos = x->right;
        if (*pos != NULL)
            (*pos)->parent = x->parent;
        tk_rebalance_up(s, x->parent);
    } else if (x->right == NULL) {
        *pos = x->left;
        x->left->parent = x->parent;
        tk_rebalance_up(s, x->parent);
    } else {
        tk_delete_swap(s, pos);
    }

    x->balance = -100;
    x->parent = NULL;
    x->left = NULL;
    x->right = NULL;
    x->height = -1;

    if (burrow__atomic_load_u32(&s->iterating) == 0) {
        while (s->dead != NULL) {
            TkNode *d = s->dead;
            s->dead = d->dead_next;
            tk_free_node(s, d);
        }
        tk_free_node(s, x);
    } else {
        x->dead_next = s->dead;
        s->dead = x;
    }
}

/* add: puts file in the tree with the node x, unless it is there already, in
 * which case x is not used and false comes back. Panics, with the set
 * unlocked, when file overlaps another. */
static bool tk_add(TokenFileSet *s, TokenFile *file, TkNode *x) {
    TkNode *parent = NULL;
    TkNode **pos = tk_locate(s, file->base, file->base + file->size, &parent);
    if (*pos == NULL) {
        tk_set(s, x, file, pos, parent); /* missing; insert */
        return true;
    }
    TokenFile *prev = (*pos)->file;
    if (prev != file) {
        sync_rw_mutex_unlock(&s->mu);
        panic_str(fmt_sprintf_v(error_allocator(),
                                "file %s (%d-%d) overlaps with file %s (%d-%d)",
                                prev->name, prev->base, token_file_end(prev),
                                file->name, file->base, token_file_end(file)));
    }
    return false;
}

/* ---------------------------------------------------------------- file sets */

TokenFileSet *token_new_file_set(Alloc *a) {
    TokenFileSet *s = BURROW_NEW(a, TokenFileSet);
    if (s == NULL)
        return NULL;
    s->a = a;
    s->base = 1; /* 0 == NoPos */
    return s;
}

static void tk_free_file(Alloc *a, TokenFile *f) {
    if (f->name.len > 0)
        mem_free(a, (void *)(uintptr_t)f->name.p, (size_t)f->name.len, 1);
    if (f->lines != NULL)
        mem_free(a, f->lines, (size_t)f->cap_lines * sizeof(Int), _Alignof(Int));
    for (Int i = 0; f->infos != NULL && i < f->ninfos; i++) {
        Str n = f->infos[i].Filename;
        if (n.len > 0)
            mem_free(a, (void *)(uintptr_t)n.p, (size_t)n.len, 1);
    }
    if (f->infos != NULL)
        mem_free(a, f->infos, (size_t)f->cap_infos * sizeof(TkLineInfo),
                 _Alignof(TkLineInfo));
    mem_free(a, f, sizeof *f, _Alignof(TokenFile));
}

static void tk_free_tree(TokenFileSet *s, TkNode *x) {
    while (x != NULL) {
        tk_free_tree(s, x->left);
        TkNode *right = x->right;
        tk_free_node(s, x);
        x = right;
    }
}

void token_file_set_free(TokenFileSet *s) {
    if (s == NULL)
        return;
    tk_free_tree(s, s->root);
    while (s->dead != NULL) {
        TkNode *d = s->dead;
        s->dead = d->dead_next;
        tk_free_node(s, d);
    }
    while (s->own != NULL) {
        TokenFile *f = s->own;
        s->own = f->next_own;
        tk_free_file(s->a, f);
    }
    mem_free(s->a, s, sizeof *s, _Alignof(TokenFileSet));
}

Int token_file_set_base(TokenFileSet *s) {
    sync_rw_mutex_r_lock(&s->mu);
    Int b = s->base;
    sync_rw_mutex_r_unlock(&s->mu);
    return b;
}

/* A file of the set's own with no lines and no line info, or NULL when the
 * allocator says no. */
static TokenFile *tk_new_file(TokenFileSet *s, Str filename, Int base, Int size) {
    TokenFile *f = BURROW_NEW(s->a, TokenFile);
    if (f == NULL)
        return NULL;
    f->a = s->a;
    f->name = str_clone(s->a, filename);
    if (f->name.len != filename.len) {
        mem_free(s->a, f, sizeof *f, _Alignof(TokenFile));
        return NULL;
    }
    f->base = base;
    f->size = size;
    return f;
}

TokenFile *token_file_set_add_file(TokenFileSet *s, Str filename, Int base, Int size) {
    /* Allocate f outside the critical section. */
    TokenFile *f = tk_new_file(s, filename, 0, size);
    TkNode *x = BURROW_NEW(s->a, TkNode);
    if (f != NULL && x != NULL && !tk_grow_lines(f)) {
        tk_free_file(s->a, f);
        f = NULL;
    }
    if (f == NULL || x == NULL) {
        if (f != NULL)
            tk_free_file(s->a, f);
        if (x != NULL)
            tk_free_node(s, x);
        return NULL;
    }
    f->lines[f->nlines++] = 0;

    sync_rw_mutex_lock(&s->mu);
    if (base < 0)
        base = s->base;
    if (base < s->base) {
        Int sb = s->base;
        sync_rw_mutex_unlock(&s->mu);
        tk_free_file(s->a, f);
        tk_free_node(s, x);
        panic_str(fmt_sprintf_v(error_allocator(), "invalid base %d (should be >= %d)",
                                base, sb));
    }
    f->base = base;
    if (size < 0) {
        sync_rw_mutex_unlock(&s->mu);
        tk_free_file(s->a, f);
        tk_free_node(s, x);
        panic_str(
            fmt_sprintf_v(error_allocator(), "invalid size %d (should be >= 0)", size));
    }
    /* base >= s.base && size >= 0. +1 because EOF also has a position. */
    if (base > INT64_MAX - size - 1) {
        sync_rw_mutex_unlock(&s->mu);
        tk_free_file(s->a, f);
        tk_free_node(s, x);
        panic_str(
            BURROW_S("token.Pos offset overflow (> 2G of source code in file set)"));
    }
    base += size + 1;

    /* add the file to the file set */
    s->base = base;
    f->next_own = s->own;
    s->own = f;
    (void)tk_add(s, f, x);
    burrow__atomic_store_ptr((void **)&s->last, f);
    sync_rw_mutex_unlock(&s->mu);
    return f;
}

void token_file_set_add_existing_files(TokenFileSet *s, TokenFile *const *files,
                                       Int n) {
    sync_rw_mutex_lock(&s->mu);
    for (Int i = 0; i < n; i++) {
        TokenFile *f = files[i];
        TkNode *x = BURROW_NEW(s->a, TkNode);
        if (x == NULL)
            break;
        if (!tk_add(s, f, x))
            tk_free_node(s, x);
        Int end = f->base + f->size + 1;
        if (end > s->base)
            s->base = end;
    }
    sync_rw_mutex_unlock(&s->mu);
}

void token_file_set_remove_file(TokenFileSet *s, TokenFile *file) {
    sync_rw_mutex_lock(&s->mu);
    void *want = file;
    (void)burrow__atomic_cas_ptr((void **)&s->last, &want,
                                 NULL); /* clear last file cache */
    TkNode *parent = NULL;
    TkNode **pn = tk_locate(s, file->base, file->base + file->size, &parent);
    if (*pn != NULL && (*pn)->file == file)
        tk_delete(s, pn);
    sync_rw_mutex_unlock(&s->mu);
}

void token_file_set_iterate(TokenFileSet *s, TokenFileFunc yield) {
    sync_rw_mutex_r_lock(&s->mu);
    (void)burrow__atomic_add_u32(&s->iterating, 1);
    TkNode *x = s->root;
    if (x != NULL) {
        while (x->left != NULL)
            x = x->left;
    }
    while (x != NULL) {
        TokenFile *f = x->file;
        /* Unlock around user code. The walk is robust to modification by
         * yield. */
        sync_rw_mutex_r_unlock(&s->mu);
        bool more = BURROW_CALLF(yield, f);
        sync_rw_mutex_r_lock(&s->mu);
        if (!more)
            break;
        if (x->height >= 0) {
            x = tk_next(x);
        } else {
            /* x was removed while yield ran, so find its successor from its
             * key. */
            TkNode *parent = NULL;
            TkNode **pos = tk_locate(s, x->key_start, x->key_end, &parent);
            x = tk_next_after(pos, parent);
        }
    }
    (void)burrow__atomic_add_u32(&s->iterating, (uint32_t)-1);
    sync_rw_mutex_r_unlock(&s->mu);
}

static TokenFile *tk_file(TokenFileSet *s, TokenPos p) {
    /* common case: p is in last file. */
    TokenFile *f = burrow__atomic_load_ptr((void *const *)&s->last);
    if (f != NULL && f->base <= p && p <= f->base + f->size)
        return f;

    sync_rw_mutex_r_lock(&s->mu);
    TkNode *parent = NULL;
    TkNode *n = *tk_locate(s, p, p, &parent);
    f = NULL;
    if (n != NULL) {
        /* Update cache of last file. A race is ok, but an exclusive lock causes
         * heavy contention. */
        burrow__atomic_store_ptr((void **)&s->last, n->file);
        f = n->file;
    }
    sync_rw_mutex_r_unlock(&s->mu);
    return f;
}

TokenFile *token_file_set_file(TokenFileSet *s, TokenPos p) {
    if (p == TOKEN_NO_POS)
        return NULL;
    return tk_file(s, p);
}

TokenPosition token_file_set_position_for(TokenFileSet *s, TokenPos p, bool adjusted) {
    TokenPosition pos = {BURROW_STR_EMPTY, 0, 0, 0};
    if (p != TOKEN_NO_POS) {
        TokenFile *f = tk_file(s, p);
        if (f != NULL)
            pos = tk_position(f, p, adjusted);
    }
    return pos;
}

TokenPosition token_file_set_position(TokenFileSet *s, TokenPos p) {
    return token_file_set_position_for(s, p, true);
}

/* -------------------------------------------------------------- serializing */

typedef Slice TkInts;
static BURROW_SLICE_TYPE_DEFINE(TkInts, Int);
typedef Slice TkLineInfos;
static BURROW_SLICE_TYPE_DEFINE(TkLineInfos, TkLineInfo);

/* serializedFile and serializedFileSet. */
#define TK_SERIALIZED_FILE_FIELDS(F, T)                                                \
    F(T, Str, Name, Name, "")                                                          \
    F(T, Int, Base, Base, "")                                                          \
    F(T, Int, Size, Size, "")                                                          \
    F(T, TkInts, Lines, Lines, "")                                                     \
    F(T, TkLineInfos, Infos, Infos, "")
BURROW_STRUCT_AS(TkSerializedFile, TK_SERIALIZED_FILE_FIELDS);

typedef Slice TkSerializedFiles;
static BURROW_SLICE_TYPE_DEFINE(TkSerializedFiles, TkSerializedFile);

#define TK_SERIALIZED_FILE_SET_FIELDS(F, T)                                            \
    F(T, Int, Base, Base, "")                                                          \
    F(T, TkSerializedFiles, Files, Files, "")
BURROW_STRUCT_AS(TkSerializedFileSet, TK_SERIALIZED_FILE_SET_FIELDS);

/* A file of the set's own made from a decoded one, with copies of its tables,
 * or NULL when the allocator says no. */
static TokenFile *tk_file_from(TokenFileSet *s, const TkSerializedFile *sf) {
    TokenFile *f = tk_new_file(s, sf->Name, sf->Base, sf->Size);
    if (f == NULL)
        return NULL;
    if (sf->Lines.len > 0) {
        f->lines =
            mem_alloc_array(s->a, (size_t)sf->Lines.len, sizeof(Int), _Alignof(Int));
        if (f->lines == NULL)
            goto fail;
        f->cap_lines = sf->Lines.len;
        const Int *in = sf->Lines.p;
        for (Int i = 0; i < sf->Lines.len; i++)
            f->lines[f->nlines++] = in[i];
    }
    if (sf->Infos.len > 0) {
        f->infos = mem_alloc_array(s->a, (size_t)sf->Infos.len, sizeof(TkLineInfo),
                                   _Alignof(TkLineInfo));
        if (f->infos == NULL)
            goto fail;
        f->cap_infos = sf->Infos.len;
        const TkLineInfo *in = sf->Infos.p;
        for (Int i = 0; i < sf->Infos.len; i++) {
            TkLineInfo li = in[i];
            li.Filename = str_clone(s->a, in[i].Filename);
            if (li.Filename.len != in[i].Filename.len)
                goto fail;
            f->infos[f->ninfos++] = li;
        }
    }
    return f;
fail:
    tk_free_file(s->a, f);
    return NULL;
}

Error token_file_set_read(TokenFileSet *s, TokenCodecFunc decode) {
    TkSerializedFileSet ss = {0};
    Error err = BURROW_CALLF(decode, BURROW_ANY(TYPE_OF(TkSerializedFileSet), &ss));
    if (BURROW_FAILED(err))
        return err;

    const TkSerializedFile *in = ss.Files.p;
    sync_rw_mutex_lock(&s->mu);
    s->base = ss.Base;
    for (Int i = 0; i < ss.Files.len; i++) {
        TokenFile *f = tk_file_from(s, &in[i]);
        TkNode *x = f != NULL ? BURROW_NEW(s->a, TkNode) : NULL;
        if (x == NULL) {
            if (f != NULL)
                tk_free_file(s->a, f);
            sync_rw_mutex_unlock(&s->mu);
            return errors_new(error_allocator(), BURROW_S("go/token: out of memory"));
        }
        f->next_own = s->own;
        s->own = f;
        if (!tk_add(s, f, x))
            tk_free_node(s, x);
    }
    burrow__atomic_store_ptr((void **)&s->last, NULL);
    sync_rw_mutex_unlock(&s->mu);
    return BURROW_NO_ERROR;
}

/* What token_file_set_write allocates for ss, given back. */
static void tk_free_serialized(Alloc *a, TkSerializedFileSet *ss) {
    TkSerializedFile *files = ss->Files.p;
    for (Int i = 0; i < ss->Files.len; i++) {
        if (files[i].Lines.p != NULL)
            mem_free(a, files[i].Lines.p, (size_t)files[i].Lines.cap * sizeof(Int),
                     _Alignof(Int));
        if (files[i].Infos.p != NULL)
            mem_free(a, files[i].Infos.p,
                     (size_t)files[i].Infos.cap * sizeof(TkLineInfo),
                     _Alignof(TkLineInfo));
    }
    if (files != NULL)
        mem_free(a, files, (size_t)ss->Files.cap * sizeof(TkSerializedFile),
                 _Alignof(TkSerializedFile));
}

Error token_file_set_write(TokenFileSet *s, TokenCodecFunc encode) {
    TkSerializedFileSet ss = {0};
    ss.Files = slice_nil(TYPE_OF(TkSerializedFile));
    bool oom = false;

    sync_rw_mutex_lock(&s->mu);
    ss.Base = s->base;
    Int n = 0;
    TkNode *first = s->root;
    while (first != NULL && first->left != NULL)
        first = first->left;
    for (TkNode *x = first; x != NULL; x = tk_next(x))
        n++;
    TkSerializedFile *files = NULL;
    if (n > 0) {
        files = mem_alloc_array(s->a, (size_t)n, sizeof(TkSerializedFile),
                                _Alignof(TkSerializedFile));
        oom = files == NULL;
    }
    if (files != NULL)
        ss.Files = slice_from(files, n, n, TYPE_OF(TkSerializedFile));
    Int i = 0;
    for (TkNode *x = first; x != NULL && !oom; x = tk_next(x), i++) {
        TokenFile *f = x->file;
        TkSerializedFile *sf = &files[i];
        sync_mutex_lock(&f->mu);
        sf->Name = f->name;
        sf->Base = f->base;
        sf->Size = f->size;
        sf->Lines = slice_nil(TYPE_INT);
        sf->Infos = slice_nil(TYPE_OF(TkLineInfo));
        if (f->nlines > 0) {
            Int *lines =
                mem_alloc_array(s->a, (size_t)f->nlines, sizeof(Int), _Alignof(Int));
            if (lines != NULL) {
                for (Int j = 0; j < f->nlines; j++)
                    lines[j] = f->lines[j];
                sf->Lines = slice_from(lines, f->nlines, f->nlines, TYPE_INT);
            }
            oom = lines == NULL;
        }
        if (f->ninfos > 0 && !oom) {
            TkLineInfo *infos = mem_alloc_array(
                s->a, (size_t)f->ninfos, sizeof(TkLineInfo), _Alignof(TkLineInfo));
            if (infos != NULL) {
                for (Int j = 0; j < f->ninfos; j++)
                    infos[j] = f->infos[j];
                sf->Infos =
                    slice_from(infos, f->ninfos, f->ninfos, TYPE_OF(TkLineInfo));
            }
            oom = infos == NULL;
        }
        sync_mutex_unlock(&f->mu);
    }
    sync_rw_mutex_unlock(&s->mu);

    Error err = BURROW_NO_ERROR;
    if (oom)
        err = errors_new(error_allocator(), BURROW_S("go/token: out of memory"));
    else
        err = BURROW_CALLF(encode, BURROW_ANY(TYPE_OF(TkSerializedFileSet), &ss));
    tk_free_serialized(s->a, &ss);
    return err;
}
