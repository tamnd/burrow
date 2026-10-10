/* go/printer: printing of Go syntax trees.
 *
 * Derived from Go's src/go/printer/printer.go, nodes.go, comment.go,
 * gobuild.go and math.go.
 * Go source: go1.27.1.
 *
 * Go's print takes a list of items of mixed types and switches on each one.
 * Here every kind of item has a function of its own (pt_ws, pt_mode, pt_tok,
 * pt_ident, pt_lit and pt_text) and they all end in the same place.
 *
 * A Fprint works in an arena of its own. nodeSize prints a node again to see
 * how wide it is, in the same arena, and gives back what it used with a mark,
 * so the nested printing costs nothing once it returns. The map of node sizes
 * lives in the caller's allocator, since it has to outlive the marks.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/printer.h"

#include "burrow/core.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/go/build/constraint.h"
#include "burrow/go/doc/comment.h"
#include "burrow/iface.h"
#include "burrow/map.h"
#include "burrow/math.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/text/tabwriter.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define S(lit) BURROW_S(lit)

BURROW_NORETURN static void pt_oom(void) {
    panic_str(S("go/printer: out of memory"));
}

/* ------------------------------------------------------------------ state */

enum { PT_MAX_NEWLINES = 2 };

#define PT_INFINITY ((Int)1 << 30)

/* whiteSpace. */
typedef Byte PtWs;

enum {
    PT_IGNORE = 0,
    PT_BLANK = ' ',
    PT_VTAB = '\v',
    PT_NEWLINE = '\n',
    PT_FORMFEED = '\f',
    PT_INDENT = '>',
    PT_UNINDENT = '<'
};

/* pmode. */
typedef Int PtMode;

enum {
    PT_NO_EXTRA_BLANK = 1 << 0,    /* no extra blank after a block comment */
    PT_NO_EXTRA_LINEBREAK = 1 << 1 /* no extra line break after one */
};

/* commentInfo. */
typedef struct PtCommentInfo {
    Int cindex;               /* index of the next comment */
    AstCommentGroup *comment; /* comments[cindex-1], or NULL */
    Int comment_offset;       /* offset of comment's first comment, or infinity */
    bool comment_newline;     /* whether the group has a newline in it */
} PtCommentInfo;

typedef struct PtIntList {
    Int *p;
    Int len, cap;
} PtIntList;

typedef struct PtPrinter {
    PrinterConfig cfg;
    TokenFileSet *fset;
    Arena *ar;
    Alloc *a; /* the arena's allocator */

    /* current state */
    Byte *output;
    Int output_len, output_cap;
    Int indent;
    Int level; /* 0 outside a composite literal, above 0 inside one */
    PtMode mode;
    bool end_alignment;
    bool implied_semi;
    Token last_tok;  /* TOKEN_ILLEGAL if it was white space */
    Token prev_open; /* the previous ( or [ not yet followed by a token */
    PtWs wsbuf[16];  /* delayed white space */
    Int ws_len;
    PtIntList go_build;   /* where each //go:build comment starts in output */
    PtIntList plus_build; /* where each // +build comment starts in output */

    /* positions */
    TokenPosition pos;  /* in the source */
    TokenPosition out;  /* in the output */
    TokenPosition last; /* pos after the last pt_write_string */
    Int *line_ptr;
    Error source_pos_err;

    /* the comments, in source order; nil when comments.p is NULL */
    Slice comments;
    bool use_node_comments;
    PtCommentInfo ci;

    Map *node_sizes; /* of uintptr node to Int */

    TokenPos cached_pos;
    Int cached_line;
} PtPrinter;

#define PT_AT(s, T, i) (((T *)(s).p)[i])

/* nodes, further down */
static void pt_expr(PtPrinter *p, AstExpr x);
static void pt_stmt(PtPrinter *p, AstStmt stmt, bool next_is_rbrace);
static void pt_decl(PtPrinter *p, AstDecl decl);
static void pt_spec(PtPrinter *p, AstSpec spec, Int n, bool do_indent);
static void pt_stmt_list(PtPrinter *p, Slice list, Int nindent, bool next_is_rbrace);
static void pt_decl_list(PtPrinter *p, Slice list);
static void pt_file(PtPrinter *p, AstFile *src);

/* ---------------------------------------------------------------- helpers */

static void *pt_alloc(PtPrinter *p, size_t size) {
    void *q = mem_alloc(p->a, size, _Alignof(max_align_t));
    if (q == NULL)
        pt_oom();
    return q;
}

static void pt_bytes_grow(Alloc *a, Byte **buf, Int *cap, Int need) {
    if (need <= *cap)
        return;
    Int nc = *cap == 0 ? 256 : *cap;
    while (nc < need)
        nc *= 2;
    Byte *q = (Byte *)mem_realloc(a, *buf, (size_t)*cap, (size_t)nc, 1);
    if (q == NULL)
        pt_oom();
    *buf = q;
    *cap = nc;
}

static void pt_out_write(PtPrinter *p, const Byte *s, Int n) {
    if (n <= 0)
        return;
    pt_bytes_grow(p->a, &p->output, &p->output_cap, p->output_len + n);
    memcpy(p->output + p->output_len, s, (size_t)n);
    p->output_len += n;
}

static void pt_out_byte(PtPrinter *p, Byte b) {
    pt_out_write(p, &b, 1);
}

static void pt_int_push(PtPrinter *p, PtIntList *l, Int v) {
    if (l->len == l->cap) {
        Int nc = l->cap == 0 ? 4 : l->cap * 2;
        Int *q = (Int *)mem_realloc(p->a, l->p, (size_t)l->cap * sizeof(Int),
                                    (size_t)nc * sizeof(Int), _Alignof(Int));
        if (q == NULL)
            pt_oom();
        l->p = q;
        l->cap = nc;
    }
    l->p[l->len++] = v;
}

/* a + b, made in the printer's arena. */
static Str pt_concat(PtPrinter *p, Str a, Str b) {
    Int n = a.len + b.len;
    if (n == 0)
        return BURROW_STR_EMPTY;
    Byte *q = (Byte *)pt_alloc(p, (size_t)n);
    if (a.len > 0)
        memcpy(q, a.p, (size_t)a.len);
    if (b.len > 0)
        memcpy(q + a.len, b.p, (size_t)b.len);
    return str_from_bytes(q, n);
}

static bool pt_valid(TokenPosition pos) {
    return pos.line > 0;
}

static bool pt_pos_eq(TokenPosition x, TokenPosition y) {
    return x.offset == y.offset && x.line == y.line && x.column == y.column &&
           str_eq(x.filename, y.filename);
}

/* The second byte of a comment's text, which says whether it is a line or a
 * block comment. */
static Byte pt_c1(Str text) {
    return text.len > 1 ? text.p[1] : 0;
}

static Str pt_tok_str(PtPrinter *p, Token tok) {
    return token_string(tok, p->a);
}

static bool pt_is_space(void *env, Rune r) {
    (void)env;
    return unicode_is_space(r);
}

/* trimRight. */
static Str pt_trim_right(Str s) {
    return strings_trim_right_func(s, (RuneFunc){pt_is_space, NULL});
}

/* -------------------------------------------------------------- positions */

static TokenPosition pt_pos_for(PtPrinter *p, TokenPos pos) {
    /* not used frequently enough to cache the entire position */
    return token_file_set_position_for(p->fset, pos, false);
}

static Int pt_line_for(PtPrinter *p, TokenPos pos) {
    if (pos != p->cached_pos) {
        p->cached_pos = pos;
        p->cached_line = token_file_set_position_for(p->fset, pos, false).line;
    }
    return p->cached_line;
}

/* commentsHaveNewline: whether the comments of a group have a newline in
 * them. The positions may be only partly right, so the text is read too. */
static bool pt_comments_have_newline(PtPrinter *p, Slice list) {
    /* list.len > 0 */
    Int line = pt_line_for(p, ast_comment_pos(PT_AT(list, AstComment *, 0)));
    for (Int i = 0; i < list.len; i++) {
        AstComment *c = PT_AT(list, AstComment *, i);
        if (i > 0 && pt_line_for(p, ast_comment_pos(c)) != line)
            return true; /* not all comments on the same line */
        Str t = c->text;
        if (t.len >= 2 && (t.p[1] == '/' || strings_index_byte(t, '\n') >= 0))
            return true;
    }
    return false;
}

static void pt_next_comment(PtPrinter *p) {
    while (p->comments.p != NULL && p->ci.cindex < p->comments.len) {
        AstCommentGroup *c = PT_AT(p->comments, AstCommentGroup *, p->ci.cindex);
        p->ci.cindex++;
        Slice list = c->list;
        if (list.len > 0) {
            p->ci.comment = c;
            p->ci.comment_offset =
                pt_pos_for(p, ast_comment_pos(PT_AT(list, AstComment *, 0))).offset;
            p->ci.comment_newline = pt_comments_have_newline(p, list);
            return;
        }
        /* we should not get here (correct trees have no empty groups),
         * but be conservative and try again */
    }
    /* no more comments */
    p->ci.comment_offset = PT_INFINITY;
}

/* commentBefore: whether the current comment group comes before next in the
 * source and printing it brings in no implicit semicolons. */
static bool pt_comment_before(PtPrinter *p, TokenPosition next) {
    return p->ci.comment_offset < next.offset &&
           (!p->implied_semi || !p->ci.comment_newline);
}

/* commentSizeBefore: an estimate of the size of the comments on the same line
 * before next. */
static Int pt_comment_size_before(PtPrinter *p, TokenPosition next) {
    PtCommentInfo saved = p->ci; /* pt_next_comment changes it */
    Int size = 0;
    while (pt_comment_before(p, next)) {
        Slice list = p->ci.comment->list;
        for (Int i = 0; i < list.len; i++)
            size += PT_AT(list, AstComment *, i)->text.len;
        pt_next_comment(p);
    }
    p->ci = saved;
    return size;
}

/* recordLine: record the output line of the next token that is not white
 * space in *line_ptr. */
static void pt_record_line(PtPrinter *p, Int *line_ptr) {
    p->line_ptr = line_ptr;
}

/* linesFrom: the number of output lines between the current one and line. */
static Int pt_lines_from(PtPrinter *p, Int line) {
    return p->out.line - line;
}

/* ---------------------------------------------------------------- writing */

/* writeLineDirective: a //line directive, if one is needed. */
static void pt_write_line_directive(PtPrinter *p, TokenPosition pos) {
    if (pt_valid(pos) &&
        (p->out.line != pos.line || !str_eq(p->out.filename, pos.filename))) {
        if (strings_contains_any(pos.filename, S("\r\n"))) {
            if (!BURROW_FAILED(p->source_pos_err))
                p->source_pos_err = fmt_errorf_v(
                    "go/printer: source filename contains unexpected newline "
                    "character: %q",
                    pos.filename);
            return;
        }
        Str line = fmt_sprintf_v(p->a, "//line %s:%d\n", pos.filename, pos.line);
        pt_out_byte(p, TABWRITER_ESCAPE); /* keep the \n away from the tabwriter */
        pt_out_write(p, line.p, line.len);
        pt_out_byte(p, TABWRITER_ESCAPE);
        /* out has to match the //line directive */
        p->out.filename = pos.filename;
        p->out.line = pos.line;
    }
}

/* writeIndent. */
static void pt_write_indent(PtPrinter *p) {
    /* hard tabs: indentation columns must not be dropped by the tabwriter */
    Int n = p->cfg.indent + p->indent; /* base indentation included */
    for (Int i = 0; i < n; i++)
        pt_out_byte(p, '\t');
    p->pos.offset += n;
    p->pos.column += n;
    p->out.column += n;
}

/* writeByte: ch n times, for white space only. */
static void pt_write_byte(PtPrinter *p, Byte ch, Int n) {
    if (p->end_alignment) {
        /* Ignore alignment characters, and break the line with a formfeed
         * to end the columns in place. */
        switch (ch) {
        case '\t':
        case '\v':
            ch = ' ';
            break;
        case '\n':
        case '\f':
            ch = '\f';
            p->end_alignment = false;
            break;
        default:
            break;
        }
    }

    if (p->out.column == 1) {
        /* no line directive is needed before white space */
        pt_write_indent(p);
    }

    for (Int i = 0; i < n; i++)
        pt_out_byte(p, ch);

    p->pos.offset += n;
    if (ch == '\n' || ch == '\f') {
        p->pos.line += n;
        p->out.line += n;
        p->pos.column = 1;
        p->out.column = 1;
        return;
    }
    p->pos.column += n;
    p->out.column += n;
}

/* writeString: s, a token, literal or comment, which has to come out as it
 * is. With is_lit, s goes between TABWRITER_ESCAPE bytes so the tabwriter
 * leaves it alone. */
static void pt_write_string(PtPrinter *p, TokenPosition pos, Str s, bool is_lit) {
    if (p->out.column == 1) {
        if ((p->cfg.mode & PRINTER_SOURCE_POS) != 0)
            pt_write_line_directive(p, pos);
        pt_write_indent(p);
    }

    if (pt_valid(pos)) {
        /* Done after the line start, since pt_write_indent moves pos and
         * pos is where s is. */
        p->pos = pos;
    }

    if (is_lit) {
        /* Valid Go cannot hold TABWRITER_ESCAPE, which is not valid
         * UTF-8. */
        pt_out_byte(p, TABWRITER_ESCAPE);
    }

    pt_out_write(p, s.p, s.len);

    Int nlines = 0;
    Int li = 0; /* index of the last newline, if nlines > 0 */
    for (Int i = 0; i < s.len; i++) {
        /* raw string literals may hold anything but a back quote */
        Byte ch = s.p[i];
        if (ch == '\n' || ch == '\f') {
            nlines++;
            li = i;
            /* A line break inside a literal breaks the columns in place,
             * so ignore any more alignment to the end of the line. */
            p->end_alignment = true;
        }
    }
    p->pos.offset += s.len;
    if (nlines > 0) {
        p->pos.line += nlines;
        p->out.line += nlines;
        Int c = s.len - li;
        p->pos.column = c;
        p->out.column = c;
    } else {
        p->pos.column += s.len;
        p->out.column += s.len;
    }

    if (is_lit)
        pt_out_byte(p, TABWRITER_ESCAPE);

    p->last = p->pos;
}

static void pt_write_whitespace(PtPrinter *p, Int n);

/* writeCommentPrefix: the white space before a comment at pos. next is where
 * the item after the pending comments is, prev the comment before this one in
 * its group or NULL, and tok the next token. */
static void pt_write_comment_prefix(PtPrinter *p, TokenPosition pos, TokenPosition next,
                                    AstComment *prev, Token tok) {
    if (p->output_len == 0) {
        /* the comment is the first thing printed */
        return;
    }

    if (pt_valid(pos) && !str_eq(pos.filename, p->last.filename)) {
        /* a comment in another file */
        pt_write_byte(p, '\f', PT_MAX_NEWLINES);
        return;
    }

    if (pos.line == p->last.line && (prev == NULL || pt_c1(prev->text) != '/')) {
        /* On the same line as the last item: separate them with at least
         * one separator. */
        bool has_sep = false;
        if (prev == NULL) {
            /* the first comment of a group */
            Int j = 0;
            for (Int i = 0; i < p->ws_len; i++) {
                PtWs ch = p->wsbuf[i];
                if (ch == PT_BLANK) {
                    /* ignore any blanks before a comment */
                    p->wsbuf[i] = PT_IGNORE;
                    continue;
                }
                if (ch == PT_VTAB) {
                    /* keep tabs, which commented structs need */
                    has_sep = true;
                    continue;
                }
                if (ch == PT_INDENT) {
                    /* apply pending indentation */
                    continue;
                }
                j = i;
                break;
            }
            pt_write_whitespace(p, j);
        }
        /* at least one separator */
        if (!has_sep) {
            Byte sep = '\t';
            if (pos.line == next.line) {
                /* The next item is on the comment's line, so the comment is
                 * a block one: use a blank. */
                sep = ' ';
            }
            pt_write_byte(p, sep, 1);
        }
    } else {
        /* On another line: separate them with at least one line break. */
        bool dropped_linebreak = false;
        Int j = 0;
        for (Int i = 0; i < p->ws_len; i++) {
            PtWs ch = p->wsbuf[i];
            if (ch == PT_BLANK || ch == PT_VTAB) {
                /* ignore horizontal space before line breaks */
                p->wsbuf[i] = PT_IGNORE;
                continue;
            }
            if (ch == PT_INDENT) {
                /* apply pending indentation */
                continue;
            }
            if (ch == PT_UNINDENT) {
                /* If this is not the last unindent, apply it as it is: it
                 * likely belongs to the last construct, a multi-line
                 * expression list say, and does not close a block. */
                if (i + 1 < p->ws_len && p->wsbuf[i + 1] == PT_UNINDENT)
                    continue;
                /* If the next token is not a closing }, apply the unindent
                 * when the comment looks aligned with the token. Otherwise
                 * take the unindent to close a block and stop, as with
                 * comments before a case label that belong to the next
                 * case. */
                if (tok != TOKEN_RBRACE && pos.column == next.column)
                    continue;
            } else if (ch == PT_NEWLINE || ch == PT_FORMFEED) {
                p->wsbuf[i] = PT_IGNORE;
                dropped_linebreak = prev == NULL; /* only for a group's first */
            }
            j = i;
            break;
        }
        pt_write_whitespace(p, j);

        /* how many line breaks go before the comment */
        Int n = 0;
        if (pt_valid(pos) && pt_valid(p->last)) {
            n = pos.line - p->last.line;
            if (n < 0) /* should never happen */
                n = 0;
        }

        /* At package scope only, add the newline dropped before back: it
         * keeps a blank line before a doc comment there (issue 2570). */
        if (p->indent == 0 && dropped_linebreak)
            n++;

        /* at least one line break after a line comment */
        if (n == 0 && prev != NULL && pt_c1(prev->text) == '/')
            n = 1;

        if (n > 0) {
            /* Formfeeds break the columns before a comment, as they do
             * between the lines of a block one. */
            pt_write_byte(p, '\f', n < PT_MAX_NEWLINES ? n : PT_MAX_NEWLINES);
        }
    }
}

/* isBlank: whether s is only white space (only tabs and blanks can be here). */
static bool pt_is_blank(Str s) {
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] > ' ')
            return false;
    }
    return true;
}

/* commonPrefix. */
static Str pt_common_prefix(Str a, Str b) {
    Int i = 0;
    while (i < a.len && i < b.len && a.p[i] == b.p[i] &&
           (a.p[i] <= ' ' || a.p[i] == '*'))
        i++;
    return str_from_bytes(a.p, i);
}

/* stripCommonPrefix: take a common prefix off the lines of a block comment
 * (unless no line is indented, all but the first have some space in front).
 * The prefix comes from heuristics that try to keep the text laid out well
 * once each line is printed again at the printer's indentation. */
static void pt_strip_common_prefix(PtPrinter *p, Str *lines, Int n) {
    if (n <= 1)
        return; /* at most one line */

    /* The heuristic handles a few common shapes: the opening and closing
     * aligned with the text aligned and indented with blanks or tabs, a
     * vertical line of stars on the left, and the closing on the same line
     * as the last text.
     *
     * Work out the longest common white prefix of all but the first, last
     * and blank lines, and empty the blank lines (the first line starts
     * with the opening and has no prefix). When only the first and last
     * lines are not blank, as in a comment of two lines or one whose inner
     * lines are all blank, use the last line, since otherwise the prefix
     * would be empty.
     *
     * The first and last lines are never empty, as they hold the opening
     * and closing, so the blank line check can leave them out. */
    Str prefix = BURROW_STR_EMPTY;
    bool prefix_set = false;
    if (n > 2) {
        for (Int i = 1; i < n - 1; i++) {
            Str line = lines[i];
            if (pt_is_blank(line)) {
                lines[i] = BURROW_STR_EMPTY;
            } else {
                if (!prefix_set) {
                    prefix = line;
                    prefix_set = true;
                }
                prefix = pt_common_prefix(prefix, line);
            }
        }
    }
    /* no prefix yet: use the last line */
    if (!prefix_set) {
        Str line = lines[n - 1];
        prefix = pt_common_prefix(line, line);
    }

    /* a vertical line of stars changes the prefix */
    bool line_of_stars = false;
    bool ok = false;
    Str before = strings_cut(prefix, S("*"), NULL, &ok);
    if (ok) {
        /* drop a trailing blank so the stars stay aligned */
        prefix = strings_trim_suffix(before, S(" "));
        line_of_stars = true;
    } else {
        /* No line of stars. Find the white space on the first line after
         * the opening and before the text, taking two blanks for the
         * opening unless a tab follows it. If the first line has nothing
         * but the opening, take up to 3 blanks or a tab. That white space
         * may be a suffix of the common prefix. */
        Str first = lines[0];
        if (pt_is_blank(str_from_bytes(first.p + 2, first.len - 2))) {
            /* No text on the first line: shorten the prefix by up to 3
             * blanks or a tab, which keeps the text indented from the
             * opening and closing if it was in the first place. */
            Int i = prefix.len;
            for (Int k = 0; k < 3 && i > 0 && prefix.p[i - 1] == ' '; k++)
                i--;
            if (i == prefix.len && i > 0 && prefix.p[i - 1] == '\t')
                i--;
            prefix = str_from_bytes(prefix.p, i);
        } else {
            /* text on the first line */
            Byte *suffix = (Byte *)pt_alloc(p, (size_t)first.len);
            Int k = 2; /* after the opening */
            while (k < first.len && first.p[k] <= ' ') {
                suffix[k] = first.p[k];
                k++;
            }
            Str suf;
            if (k > 2 && suffix[2] == '\t') {
                /* take the tab to make up for the opening */
                suf = str_from_bytes(suffix + 2, k - 2);
            } else {
                /* otherwise take two blanks */
                suffix[0] = ' ';
                suffix[1] = ' ';
                suf = str_from_bytes(suffix, k);
            }
            /* Shorten the prefix by the suffix if it ends with it. */
            prefix = strings_trim_suffix(prefix, suf);
        }
    }

    /* The last line: align a closing on its own with the opening, and
     * otherwise align the text with the other lines. */
    Str last = lines[n - 1];
    Str closing = S("*/");
    Str head = strings_cut(last, closing, NULL, NULL); /* closing is always there */
    if (pt_is_blank(head)) {
        /* the last line is only the closing */
        if (line_of_stars)
            closing = S(" */"); /* a blank to align the last star */
        lines[n - 1] = pt_concat(p, prefix, closing);
    } else {
        /* the last line has more text: take it to be aligned like the
         * others and count it in the prefix */
        prefix = pt_common_prefix(prefix, last);
    }

    /* Take the prefix off all but the first and empty lines. */
    for (Int i = 1; i < n; i++) {
        if (lines[i].len != 0)
            lines[i] =
                str_from_bytes(lines[i].p + prefix.len, lines[i].len - prefix.len);
    }
}

static void pt_write_comment(PtPrinter *p, AstComment *comment) {
    Str text = comment->text;
    TokenPosition pos = pt_pos_for(p, ast_comment_pos(comment));

    Int saved_indent = p->indent;
    bool restore = false;
    if (strings_has_prefix(text, S("//line ")) && (!pt_valid(pos) || pos.column == 1)) {
        /* Possibly a //-style line directive. Suspend indentation for a
         * while to keep it valid. */
        restore = true;
        p->indent = 0;
    }

    if (pt_c1(text) == '/') {
        /* the common case of //-style comments */
        if (constraint_is_go_build(text))
            pt_int_push(p, &p->go_build, p->output_len);
        else if (constraint_is_plus_build(text))
            pt_int_push(p, &p->plus_build, p->output_len);
        pt_write_string(p, pos, pt_trim_right(text), true);
        if (restore)
            p->indent = saved_indent;
        return;
    }

    /* A block comment goes out line by line and the write functions take
     * care of the indentation. */
    Slice ls = strings_split(p->a, text, S("\n"));
    if (ls.p == NULL && ls.len > 0)
        pt_oom();
    Str *lines = (Str *)ls.p;
    Int n = ls.len;

    /* The comment started in the first column and is going to be indented.
     * Indent every line as if it had been already, so the common prefix
     * comes out the same however often the text is formatted (issue
     * 1835). */
    if (pt_valid(pos) && pos.column == 1 && p->indent > 0) {
        for (Int i = 1; i < n; i++)
            lines[i] = pt_concat(p, S("   "), lines[i]);
    }

    pt_strip_common_prefix(p, lines, n);

    /* the lines, separated by formfeeds, with no line break after the last */
    for (Int i = 0; i < n; i++) {
        if (i > 0) {
            pt_write_byte(p, '\f', 1);
            pos = p->pos;
        }
        if (lines[i].len > 0)
            pt_write_string(p, pos, pt_trim_right(lines[i]), true);
    }
    if (restore)
        p->indent = saved_indent;
}

/* writeCommentSuffix: a line break after a comment if one is needed, and the
 * rest of the pending indentation. Which kind of break depends on the pending
 * white space. wrote_newline says whether a newline went out and dropped_ff
 * whether a formfeed was dropped from the buffer. */
static void pt_write_comment_suffix(PtPrinter *p, bool needs_linebreak,
                                    bool *wrote_newline, bool *dropped_ff) {
    *wrote_newline = false;
    *dropped_ff = false;
    for (Int i = 0; i < p->ws_len; i++) {
        PtWs ch = p->wsbuf[i];
        switch (ch) {
        case PT_BLANK:
        case PT_VTAB:
            /* ignore trailing white space */
            p->wsbuf[i] = PT_IGNORE;
            break;
        case PT_INDENT:
        case PT_UNINDENT:
            /* keep indentation */
            break;
        case PT_NEWLINE:
        case PT_FORMFEED:
            /* If a line break is needed keep exactly one, but remember any
             * formfeed dropped. */
            if (needs_linebreak) {
                needs_linebreak = false;
                *wrote_newline = true;
            } else {
                if (ch == PT_FORMFEED)
                    *dropped_ff = true;
                p->wsbuf[i] = PT_IGNORE;
            }
            break;
        default:
            break;
        }
    }
    pt_write_whitespace(p, p->ws_len);

    /* make sure there is a line break */
    if (needs_linebreak) {
        pt_write_byte(p, '\n', 1);
        *wrote_newline = true;
    }
}

/* containsLinebreak. */
static bool pt_contains_linebreak(PtPrinter *p) {
    for (Int i = 0; i < p->ws_len; i++) {
        if (p->wsbuf[i] == PT_NEWLINE || p->wsbuf[i] == PT_FORMFEED)
            return true;
    }
    return false;
}

static Slice pt_format_doc_comment(PtPrinter *p, Slice list);

/* intersperseComments: print the comments before the next token tok together
 * with the buffered white space, mixing the two by a heuristic. */
static void pt_intersperse_comments(PtPrinter *p, TokenPosition next, Token tok,
                                    bool *wrote_newline, bool *dropped_ff) {
    AstComment *last = NULL;
    while (pt_comment_before(p, next)) {
        AstCommentGroup *g = p->ci.comment;
        Slice list = g->list;
        bool changed = false;

        if (tok != TOKEN_IDENT &&
            p->last_tok != TOKEN_IMPORT && /* leave cgo's import "C" comments alone */
            pt_pos_for(p, ast_comment_group_pos(g)).column == 1 &&
            pt_pos_eq(pt_pos_for(p, ast_comment_group_end(g) + 1), next)) {
            /* An unindented comment right before the next token: a top
             * level doc comment. */
            list = pt_format_doc_comment(p, list);
            changed = true;

            if (g->list.len > 0 && list.len == 0) {
                /* The doc comment went away altogether. Keep the white
                 * space before it. */
                pt_write_comment_prefix(p, pt_pos_for(p, ast_comment_group_pos(g)),
                                        next, last, tok);
                /* carry on at next */
                p->pos = next;
                p->last = next;
                /* there can be no more comments */
                pt_next_comment(p);
                pt_write_comment_suffix(p, false, wrote_newline, dropped_ff);
                return;
            }
        }
        for (Int i = 0; i < list.len; i++) {
            AstComment *c = PT_AT(list, AstComment *, i);
            pt_write_comment_prefix(p, pt_pos_for(p, ast_comment_pos(c)), next, last,
                                    tok);
            pt_write_comment(p, c);
            last = c;
        }
        /* If the list was rewritten, carry on where the original would
         * have ended. */
        if (g->list.len > 0 && changed) {
            last = PT_AT(g->list, AstComment *, g->list.len - 1);
            p->pos = pt_pos_for(p, ast_comment_end(last));
            p->last = p->pos;
        }
        pt_next_comment(p);
    }

    if (last != NULL) {
        /* If the last comment is a block one and the next item follows
         * on the same line but is not a comma, and not a closing token
         * right after its opening one, add a separator unless that is
         * turned off. The separator is a blank, unless line breaks are
         * pending, are not turned off, and this is outside a composite
         * literal, in which case it is a line break (issue 15137). */
        bool needs_linebreak = false;
        if ((p->mode & PT_NO_EXTRA_BLANK) == 0 && pt_c1(last->text) == '*' &&
            pt_line_for(p, ast_comment_pos(last)) == next.line && tok != TOKEN_COMMA &&
            (tok != TOKEN_RPAREN || p->prev_open == TOKEN_LPAREN) &&
            (tok != TOKEN_RBRACK || p->prev_open == TOKEN_LBRACK)) {
            if (pt_contains_linebreak(p) && (p->mode & PT_NO_EXTRA_LINEBREAK) == 0 &&
                p->level == 0)
                needs_linebreak = true;
            else
                pt_write_byte(p, ' ', 1);
        }
        /* A line break after a //-style comment, before EOF, and before a
         * closing } unless that is turned off. */
        if (pt_c1(last->text) == '/' || tok == TOKEN_EOF ||
            (tok == TOKEN_RBRACE && (p->mode & PT_NO_EXTRA_LINEBREAK) == 0))
            needs_linebreak = true;
        pt_write_comment_suffix(p, needs_linebreak, wrote_newline, dropped_ff);
        return;
    }

    /* no comment was written, which cannot happen when this is called */
    *wrote_newline = false;
    *dropped_ff = false;
}

/* writeWhitespace: the first n entries of the white space buffer. */
static void pt_write_whitespace(PtPrinter *p, Int n) {
    for (Int i = 0; i < n; i++) {
        PtWs ch = p->wsbuf[i];
        switch (ch) {
        case PT_IGNORE:
            break;
        case PT_INDENT:
            p->indent++;
            break;
        case PT_UNINDENT:
            p->indent--;
            if (p->indent < 0)
                p->indent = 0;
            break;
        case PT_NEWLINE:
        case PT_FORMFEED:
            /* A line break right before a correcting unindent swaps with
             * it, which puts labels in the right place. With a comment
             * between the two, the unindent is not part of the comment's
             * white space and the comment is indented right. */
            if (i + 1 < n && p->wsbuf[i + 1] == PT_UNINDENT) {
                /* A formfeed ends the section, or a long label on the next
                 * line could widen the column of the lines before it. */
                p->wsbuf[i] = PT_UNINDENT;
                p->wsbuf[i + 1] = PT_FORMFEED;
                i--; /* do it again */
                continue;
            }
            pt_write_byte(p, ch, 1);
            break;
        default:
            pt_write_byte(p, ch, 1);
            break;
        }
    }

    /* shift the rest down */
    Int l = p->ws_len - n;
    memmove(p->wsbuf, p->wsbuf + n, (size_t)l);
    p->ws_len = l;
}

/* ------------------------------------------------------- printing interface */

static Int pt_nlimit(Int n) {
    return n < PT_MAX_NEWLINES ? n : PT_MAX_NEWLINES;
}

static bool pt_may_combine(Token prev, Byte next) {
    switch (prev) {
    case TOKEN_INT:
        return next == '.'; /* 1. */
    case TOKEN_ADD:
        return next == '+'; /* ++ */
    case TOKEN_SUB:
        return next == '-'; /* -- */
    case TOKEN_QUO:
        return next == '*'; /* a block comment */
    case TOKEN_LSS:
        return next == '-' || next == '<'; /* <- or << */
    case TOKEN_AND:
        return next == '&' || next == '^'; /* && or &^ */
    default:
        return false;
    }
}

static void pt_set_pos(PtPrinter *p, TokenPos pos) {
    if (token_pos_is_valid(pos))
        p->pos = pt_pos_for(p, pos); /* where the next item is */
}

/* flush: the pending comments and white space before the next token tok. */
static void pt_flush(PtPrinter *p, TokenPosition next, Token tok, bool *wrote_newline,
                     bool *dropped_ff) {
    *wrote_newline = false;
    *dropped_ff = false;
    if (pt_comment_before(p, next)) {
        /* comments come before the next item: intersperse them */
        pt_intersperse_comments(p, next, tok, wrote_newline, dropped_ff);
    } else {
        /* otherwise write the white space left over */
        pt_write_whitespace(p, p->ws_len);
    }
}

/* The start of every item of Go's print: note the previous opening token. */
static void pt_item(PtPrinter *p) {
    switch (p->last_tok) {
    case TOKEN_ILLEGAL:
        /* white space */
        break;
    case TOKEN_LPAREN:
    case TOKEN_LBRACK:
        p->prev_open = p->last_tok;
        break;
    default:
        /* other tokens followed any opening one */
        p->prev_open = TOKEN_ILLEGAL;
        break;
    }
}

/* The end of an item of Go's print that is not white space or a mode: flush
 * what is pending, then write data. */
static void pt_emit(PtPrinter *p, Str data, bool is_lit, bool implied_semi) {
    TokenPosition next = p->pos; /* where the next item is, roughly or exactly */
    bool wrote_newline = false;
    bool dropped_ff = false;
    pt_flush(p, next, p->last_tok, &wrote_newline, &dropped_ff);

    /* Add the newlines the source had, if they bring in no semicolons. (It
     * is not done in flush, which would add newlines at the end of a
     * file.) */
    if (!p->implied_semi) {
        Int n = pt_nlimit(next.line - p->pos.line);
        /* no more than PT_MAX_NEWLINES counting one already written */
        if (wrote_newline && n == PT_MAX_NEWLINES)
            n = PT_MAX_NEWLINES - 1;
        if (n > 0) {
            Byte ch = '\n';
            if (dropped_ff)
                ch = '\f'; /* a formfeed, since one was dropped before */
            pt_write_byte(p, ch, n);
            implied_semi = false;
        }
    }

    /* the next token starts here: record its line if asked to */
    if (p->line_ptr != NULL) {
        *p->line_ptr = p->out.line;
        p->line_ptr = NULL;
    }

    pt_write_string(p, next, data, is_lit);
    p->implied_semi = implied_semi;
}

/* A pmode item: toggle the mode. */
static void pt_mode(PtPrinter *p, PtMode m) {
    pt_item(p);
    p->mode ^= m;
}

/* A whiteSpace item. */
static void pt_ws(PtPrinter *p, PtWs x) {
    pt_item(p);
    if (x == PT_IGNORE) {
        /* Ignores stay out of the buffer, where they can spoil correcting
         * unindents (see the labeled statement). */
        return;
    }
    Int i = p->ws_len;
    if (i == (Int)sizeof p->wsbuf) {
        /* White space runs are very short, so this should never happen.
         * Cope (with comments possibly in the wrong place) if it does. */
        pt_write_whitespace(p, i);
        i = 0;
    }
    p->ws_len = i + 1;
    p->wsbuf[i] = x;
    if (x == PT_NEWLINE || x == PT_FORMFEED) {
        /* Newlines change the current state (implied_semi), not the state
         * after the item, since comments can come before it. */
        p->implied_semi = false;
    }
    p->last_tok = TOKEN_ILLEGAL;
}

static void pt_ws2(PtPrinter *p, PtWs x, PtWs y) {
    pt_ws(p, x);
    pt_ws(p, y);
}

/* An *ast.Ident item. */
static void pt_ident(PtPrinter *p, AstIdent *x) {
    pt_item(p);
    p->last_tok = TOKEN_IDENT;
    pt_emit(p, x->name, false, true);
}

/* An *ast.BasicLit item. */
static void pt_lit(PtPrinter *p, AstBasicLit *x) {
    pt_item(p);
    p->last_tok = x->kind;
    pt_emit(p, x->value, true, true);
}

/* A token.Token item. */
static void pt_tok(PtPrinter *p, Token x) {
    pt_item(p);
    Str s = pt_tok_str(p, x);
    if (s.len > 0 && pt_may_combine(p->last_tok, s.p[0])) {
        /* The two tokens need a blank between them or they combine into
         * another, wrong, sequence. Apart from an INT followed by a '.',
         * this should never happen, since the binary expression formatting
         * takes care of it. */
        p->wsbuf[0] = ' ';
        p->ws_len = 1;
    }
    /* some keywords followed by a newline imply a semicolon */
    bool implied = false;
    switch (x) {
    case TOKEN_BREAK:
    case TOKEN_CONTINUE:
    case TOKEN_FALLTHROUGH:
    case TOKEN_RETURN:
    case TOKEN_INC:
    case TOKEN_DEC:
    case TOKEN_RPAREN:
    case TOKEN_RBRACK:
    case TOKEN_RBRACE:
        implied = true;
        break;
    default:
        break;
    }
    p->last_tok = x;
    pt_emit(p, s, false, implied);
}

static void pt_tok2(PtPrinter *p, Token x, Token y) {
    pt_tok(p, x);
    pt_tok(p, y);
}

/* A token followed by a whiteSpace, the most common pair. */
static void pt_tok_ws(PtPrinter *p, Token x, PtWs y) {
    pt_tok(p, x);
    pt_ws(p, y);
}

/* A string item, which is text for an incorrect tree. */
static void pt_text(PtPrinter *p, Str x) {
    pt_item(p);
    p->last_tok = TOKEN_STRING;
    pt_emit(p, x, true, true);
}

/* ---------------------------------------------------------- doc comments */

/* A growable list of comment pointers. */
typedef struct PtComments {
    AstComment **p;
    Int len, cap;
} PtComments;

static void pt_comments_push(PtPrinter *p, PtComments *l, AstComment *c) {
    if (l->len == l->cap) {
        Int nc = l->cap == 0 ? 8 : l->cap * 2;
        AstComment **q = (AstComment **)mem_realloc(
            p->a, (void *)l->p, (size_t)l->cap * sizeof(AstComment *),
            (size_t)nc * sizeof(AstComment *), _Alignof(AstComment *));
        if (q == NULL)
            pt_oom();
        l->p = q;
        l->cap = nc;
    }
    l->p[l->len++] = c;
}

static AstComment *pt_new_comment(PtPrinter *p, TokenPos slash, Str text) {
    AstComment *c = (AstComment *)pt_alloc(p, sizeof *c);
    memset(c, 0, sizeof *c);
    c->node.kind = AST_KIND_COMMENT;
    c->slash = slash;
    c->text = text;
    return c;
}

static Slice pt_comments_slice(PtComments l) {
    return (Slice){(void *)l.p, l.len, l.len, TYPE_AST_COMMENT_PTR};
}

/* isDirective: whether c is a comment directive, with the // taken off (see
 * go.dev/issue/37974). The same code is in go/ast. */
static bool pt_is_directive(Str c) {
    /* "//line " is a line directive, "//extern " is for gccgo and
     * "//export " for cgo. */
    if (strings_has_prefix(c, S("line ")) || strings_has_prefix(c, S("extern ")) ||
        strings_has_prefix(c, S("export ")))
        return true;

    /* "//[a-z0-9]+:[a-z0-9]" */
    Int colon = strings_index(c, S(":"));
    if (colon <= 0 || colon + 1 >= c.len)
        return false;
    for (Int i = 0; i <= colon + 1; i++) {
        if (i == colon)
            continue;
        Byte b = c.p[i];
        if (!(('a' <= b && b <= 'z') || ('0' <= b && b <= '9')))
            return false;
    }
    return true;
}

/* allStars: whether text is the inside of an old style block comment with a
 * star at the start of each line. */
static bool pt_all_stars(Str text) {
    for (Int i = 0; i < text.len; i++) {
        if (text.p[i] == '\n') {
            Int j = i + 1;
            while (j < text.len && (text.p[j] == ' ' || text.p[j] == '\t'))
                j++;
            if (j < text.len && text.p[j] != '*')
                return false;
        }
    }
    return true;
}

/* formatDocComment: the doc comment list in its canonical layout. */
static Slice pt_format_doc_comment(PtPrinter *p, Slice list) {
    /* the text without the comment markers */
    bool block = false;
    Str text = BURROW_STR_EMPTY;
    PtComments directives = {0};
    AstComment *first = PT_AT(list, AstComment *, 0);
    if (list.len == 1 && strings_has_prefix(first->text, S("/*"))) {
        block = true;
        text = first->text;
        if (!strings_contains(text, S("\n")) || pt_all_stars(text)) {
            /* A one line block comment in doc comment position, or an old
             * style one with a column of stars. Neither works well as a doc
             * comment and reformatting only makes them worse, so leave them
             * alone. */
            return list;
        }
        text = str_from_bytes(text.p + 2, text.len - 4); /* cut the markers */
    } else if (strings_has_prefix(first->text, S("//"))) {
        Byte *buf = NULL;
        Int n = 0, cap = 0;
        for (Int i = 0; i < list.len; i++) {
            AstComment *c = PT_AT(list, AstComment *, i);
            bool found = false;
            Str after = strings_cut_prefix(c->text, S("//"), &found);
            if (!found)
                return list;
            /* //go:build and other directive lines go aside */
            if (pt_is_directive(after)) {
                pt_comments_push(p, &directives, c);
                continue;
            }
            after = strings_trim_prefix(after, S(" "));
            pt_bytes_grow(p->a, &buf, &cap, n + after.len + 1);
            if (after.len > 0)
                memcpy(buf + n, after.p, (size_t)after.len);
            n += after.len;
            buf[n++] = '\n';
        }
        text = str_from_bytes(buf, n);
    } else {
        /* not something known: leave it alone */
        return list;
    }

    if (text.len == 0)
        return list;

    /* parse the comment and format it as text again */
    CommentDoc *d = comment_parser_parse(NULL, p->a, text);
    CommentPrinter pr = {0};
    Slice out_text = comment_printer_comment(&pr, p->a, d);
    text = str_from_bytes((const Byte *)out_text.p, out_text.len);

    /* a block comment comes back as one comment with the text inside */
    TokenPos slash = first->slash;
    PtComments out = {0};
    if (block) {
        Str t = pt_concat(p, pt_concat(p, S("/*\n"), text), S("*/"));
        pt_comments_push(p, &out, pt_new_comment(p, slash, t));
        return pt_comments_slice(out);
    }

    /* a // comment comes back as a // line for each line */
    while (text.len > 0) {
        Str rest = BURROW_STR_EMPTY;
        Str line = strings_cut(text, S("\n"), &rest, NULL);
        text = rest;
        if (line.len == 0)
            line = S("//");
        else if (strings_has_prefix(line, S("\t")))
            line = pt_concat(p, S("//"), line);
        else
            line = pt_concat(p, S("// "), line);
        pt_comments_push(p, &out, pt_new_comment(p, slash, line));
    }
    if (directives.len > 0) {
        pt_comments_push(p, &out, pt_new_comment(p, slash, S("//")));
        for (Int i = 0; i < directives.len; i++)
            pt_comments_push(p, &out, pt_new_comment(p, slash, directives.p[i]->text));
    }
    return pt_comments_slice(out);
}

/* ------------------------------------------------------- //go:build lines */

static bool pt_is_nl(Byte b) {
    return b == '\n' || b == '\f';
}

typedef struct PtBuf {
    Byte *p;
    Int len, cap;
} PtBuf;

static void pt_buf_write(PtPrinter *p, PtBuf *b, const Byte *s, Int n) {
    if (n <= 0)
        return;
    pt_bytes_grow(p->a, &b->p, &b->cap, b->len + n);
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
}

static void pt_buf_str(PtPrinter *p, PtBuf *b, Str s) {
    pt_buf_write(p, b, s.p, s.len);
}

static void pt_buf_byte(PtPrinter *p, PtBuf *b, Byte c) {
    pt_buf_write(p, b, &c, 1);
}

/* appendLines: y appended to x without making a doubled blank line, which
 * gofmt never writes. Only whole blocks of lines are appended. */
static void pt_append_lines(PtPrinter *p, PtBuf *x, const Byte *y, Int n) {
    if (n > 0 && pt_is_nl(y[0]) && /* y starts with a blank line */
        (x->len == 0 ||
         (x->len >= 2 && pt_is_nl(x->p[x->len - 1]) && pt_is_nl(x->p[x->len - 2])))) {
        /* x is empty or ends with a blank line: drop y's blank line */
        y++;
        n--;
    }
    pt_buf_write(p, x, y, n);
}

/* lineAt: the length of the output line that starts at start. */
static Int pt_line_at(PtPrinter *p, Int start) {
    Int pos = start;
    while (pos < p->output_len && !pt_is_nl(p->output[pos]))
        pos++;
    if (pos < p->output_len)
        pos++;
    return pos - start;
}

static Str pt_comment_text_at(PtPrinter *p, Int start) {
    if (start < p->output_len && p->output[start] == TABWRITER_ESCAPE)
        start++;
    Int pos = start;
    while (pos < p->output_len && p->output[pos] != TABWRITER_ESCAPE &&
           !pt_is_nl(p->output[pos]))
        pos++;
    return str_from_bytes(p->output + start, pos - start);
}

static void pt_sort_ints(Int *v, Int n) {
    for (Int i = 1; i < n; i++) {
        Int x = v[i];
        Int j = i;
        while (j > 0 && v[j - 1] > x) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
}

/* fixGoBuildLines: move the //go:build and // +build lines to the top of the
 * file, synthesising a //go:build line from // +build ones if there is
 * none. */
static void pt_fix_go_build_lines(PtPrinter *p) {
    if (p->go_build.len + p->plus_build.len == 0)
        return;

    /* Find the latest place the lines can go: right after the last blank
     * line before something that is not a comment. (Another blank line
     * follows the block.) This is tabwriter input, so every comment starts
     * and ends with a TABWRITER_ESCAPE byte, and some newlines are now \f
     * bytes. */
    Int insert = 0;
    for (Int pos = 0;;) {
        /* skip the space at the start of the line */
        bool blank = true;
        while (pos < p->output_len && (p->output[pos] == ' ' || p->output[pos] == '\t'))
            pos++;
        /* skip a // comment */
        if (pos + 3 < p->output_len && p->output[pos] == TABWRITER_ESCAPE &&
            p->output[pos + 1] == '/' && p->output[pos + 2] == '/') {
            blank = false;
            while (pos < p->output_len && !pt_is_nl(p->output[pos]))
                pos++;
        }
        /* skip the line end */
        if (pos >= p->output_len || !pt_is_nl(p->output[pos]))
            break;
        pos++;

        if (blank)
            insert = pos;
    }

    /* A //go:build comment before that place is used instead. (Earlier in
     * the file is always fine.) */
    if (p->go_build.len > 0 && p->go_build.p[0] < insert)
        insert = p->go_build.p[0];
    else if (p->plus_build.len > 0 && p->plus_build.p[0] < insert)
        insert = p->plus_build.p[0];

    ConstraintExpr x = NULL;
    if (p->go_build.len == 0) {
        /* make a //go:build expression from the // +build lines */
        for (Int i = 0; i < p->plus_build.len; i++) {
            Error err = BURROW_NO_ERROR;
            ConstraintExpr y =
                constraint_parse(p->a, pt_comment_text_at(p, p->plus_build.p[i]), &err);
            if (BURROW_FAILED(err)) {
                x = NULL;
                break;
            }
            if (x == NULL) {
                x = y;
            } else {
                ConstraintAndExpr *and_ =
                    (ConstraintAndExpr *)pt_alloc(p, sizeof *and_);
                memset(and_, 0, sizeof *and_);
                and_->expr.kind = CONSTRAINT_KIND_AND;
                and_->x = x;
                and_->y = y;
                x = &and_->expr;
            }
        }
    } else if (p->go_build.len == 1) {
        /* parse the //go:build expression */
        Error err = BURROW_NO_ERROR;
        x = constraint_parse(p->a, pt_comment_text_at(p, p->go_build.p[0]), &err);
        if (BURROW_FAILED(err))
            x = NULL;
    }

    PtBuf block = {0};
    if (x == NULL) {
        /* No //go:build expression that can be trusted. Bring the lines
         * together but leave them as they are. They are escaped for the
         * tabwriter already. */
        for (Int i = 0; i < p->go_build.len; i++) {
            Int at = p->go_build.p[i];
            pt_buf_write(p, &block, p->output + at, pt_line_at(p, at));
        }
        for (Int i = 0; i < p->plus_build.len; i++) {
            Int at = p->plus_build.p[i];
            pt_buf_write(p, &block, p->output + at, pt_line_at(p, at));
        }
    } else {
        pt_buf_byte(p, &block, TABWRITER_ESCAPE);
        pt_buf_str(p, &block, S("//go:build "));
        pt_buf_str(p, &block, constraint_expr_string(x, p->a));
        pt_buf_byte(p, &block, TABWRITER_ESCAPE);
        pt_buf_byte(p, &block, '\n');
        if (p->plus_build.len > 0) {
            Error err = BURROW_NO_ERROR;
            Slice lines = constraint_plus_build_lines(p->a, x, &err);
            if (BURROW_FAILED(err)) {
                Str line = pt_concat(p, S("// +build error: "), error_text(err));
                lines = (Slice){pt_alloc(p, sizeof(Str)), 1, 1, TYPE_STRING};
                PT_AT(lines, Str, 0) = line;
            }
            for (Int i = 0; i < lines.len; i++) {
                pt_buf_byte(p, &block, TABWRITER_ESCAPE);
                pt_buf_str(p, &block, PT_AT(lines, Str, i));
                pt_buf_byte(p, &block, TABWRITER_ESCAPE);
                pt_buf_byte(p, &block, '\n');
            }
        }
    }
    pt_buf_byte(p, &block, '\n');

    /* the sorted lines to delete from the rest of the output */
    Int ndel = p->go_build.len + p->plus_build.len;
    Int *to_delete = (Int *)pt_alloc(p, (size_t)ndel * sizeof(Int));
    for (Int i = 0; i < p->go_build.len; i++)
        to_delete[i] = p->go_build.p[i];
    for (Int i = 0; i < p->plus_build.len; i++)
        to_delete[p->go_build.len + i] = p->plus_build.p[i];
    pt_sort_ints(to_delete, ndel);

    /* the output after the insertion point, without those lines */
    PtBuf after = {0};
    Int start = insert;
    for (Int i = 0; i < ndel; i++) {
        Int end = to_delete[i];
        if (end < start)
            continue;
        pt_append_lines(p, &after, p->output + start, end - start);
        start = end + pt_line_at(p, end);
    }
    pt_append_lines(p, &after, p->output + start, p->output_len - start);
    if (after.len >= 2 && pt_is_nl(after.p[after.len - 1]) &&
        pt_is_nl(after.p[after.len - 2]))
        after.len--;

    p->output_len = insert;
    pt_out_write(p, block.p, block.len);
    pt_out_write(p, after.p, after.len);
}

/* ------------------------------------------------------------------- math */

/* log2ish: a rough log2(x), the same on every architecture. Only the
 * alignment heuristics use it. */
static double pt_log2ish(double x) {
    Int e = 0;
    double f = math_frexp(x, &e);
    return (double)e + 2 * (f - 1);
}

/* exp2ish: a rough 2**x, the same on every architecture. */
static double pt_exp2ish(double x) {
    double n = math_floor(x);
    double f = x - n;
    return math_ldexp(1 + f, (Int)n);
}

/* ------------------------------------------------------------------ nodes */

/* Go's nodes.go: the printing of AST nodes. Node positions guide the line
 * breaks but are never trusted to be correct, so the output is right however
 * wrong they are. */

static Int pt_node_size(PtPrinter *p, AstNode n, Int max_size);
static AstExpr pt_strip_parens_always(AstExpr x);

#define PT_NODE(x) ((AstNode)(x))
#define PT_KIND(x) ((x)->kind)

/* setPos, from printer.go. */
static void pt_set_pos_node(PtPrinter *p, AstNode n) {
    pt_set_pos(p, ast_node_pos(n));
}

/* linebreak: at least min line breaks, and as many as the source had up to
 * line, but no more than PT_MAX_NEWLINES. ws is written first if any are
 * written. new_section makes the first a formfeed. The result is the number
 * of breaks, with a formfeed counting as two. */
static Int pt_linebreak(PtPrinter *p, Int line, Int min, PtWs ws, bool new_section) {
    Int n = pt_nlimit(line - p->pos.line);
    if (n < min)
        n = min;
    Int nbreaks = 0;
    if (n > 0) {
        pt_ws(p, ws);
        if (new_section) {
            pt_ws(p, PT_FORMFEED);
            n--;
            nbreaks = 2;
        }
        nbreaks += n;
        for (; n > 0; n--)
            pt_ws(p, PT_NEWLINE);
    }
    return nbreaks;
}

/* setComment: g becomes the next comment, if g is not NULL and node comments
 * are in use, as when printing a fragment of source such as the exported
 * parts only. There is no pending comment in comments and at most one in
 * ci. */
static void pt_set_comment(PtPrinter *p, AstCommentGroup *g) {
    if (g == NULL || !p->use_node_comments)
        return;
    if (p->comments.p == NULL) {
        /* make the comments lazily */
        AstCommentGroup **one = (AstCommentGroup **)pt_alloc(p, sizeof *one);
        p->comments = (Slice){(void *)one, 1, 1, TYPE_AST_COMMENT_GROUP_PTR};
    } else if (p->ci.cindex < p->comments.len) {
        /* Pending comments, which should never happen. Cope by flushing
         * those before g and dropping any after it. */
        bool wrote_newline = false;
        bool dropped_ff = false;
        pt_flush(p, pt_pos_for(p, ast_comment_pos(PT_AT(g->list, AstComment *, 0))),
                 TOKEN_ILLEGAL, &wrote_newline, &dropped_ff);
        p->comments.len = 1;
    }
    PT_AT(p->comments, AstCommentGroup *, 0) = g;
    p->ci.cindex = 0;
    /* Leave a pending comment in ci alone (a line comment can be followed
     * right away by a lead comment with no token between them). */
    if (p->ci.comment_offset == PT_INFINITY)
        pt_next_comment(p); /* get the comment ready */
}

/* exprListMode. */
enum {
    PT_COMMA_TERM = 1 << 0, /* the list may end with a comma */
    PT_NO_INDENT = 1 << 1   /* no extra indentation in lists of many lines */
};

static void pt_expr_list(PtPrinter *p, TokenPos prev0, Slice list, Int depth, Int mode,
                         TokenPos next0, bool is_incomplete);
static void pt_expr0(PtPrinter *p, AstExpr x, Int depth);

/* identList: a list of names. With indent, a list over several lines is
 * indented after the first line break. */
static void pt_ident_list(PtPrinter *p, Slice list, bool indent) {
    /* as an expression list, to share exprList's layout */
    AstExpr *xs = NULL;
    if (list.len > 0) {
        xs = (AstExpr *)pt_alloc(p, (size_t)list.len * sizeof(AstExpr));
        for (Int i = 0; i < list.len; i++)
            xs[i] = PT_NODE(PT_AT(list, AstIdent *, i));
    }
    Slice xlist = {(void *)xs, list.len, list.len, list.elem};
    pt_expr_list(p, TOKEN_NO_POS, xlist, 1, indent ? 0 : PT_NO_INDENT, TOKEN_NO_POS,
                 false);
}

#define PT_FILTERED_MSG "contains filtered or unexported fields"

/* exprList: a list of expressions. A list over several source lines keeps
 * the source's line breaks between expressions. */
static void pt_expr_list(PtPrinter *p, TokenPos prev0, Slice list, Int depth, Int mode,
                         TokenPos next0, bool is_incomplete) {
    if (list.len == 0 || list.p == NULL) {
        if (is_incomplete) {
            TokenPosition prev = pt_pos_for(p, prev0);
            TokenPosition next = pt_pos_for(p, next0);
            if (pt_valid(prev) && prev.line == next.line) {
                pt_text(p, S("/* " PT_FILTERED_MSG " */"));
            } else {
                pt_ws(p, PT_NEWLINE);
                pt_ws(p, PT_INDENT);
                pt_text(p, S("// " PT_FILTERED_MSG));
                pt_ws2(p, PT_UNINDENT, PT_NEWLINE);
            }
        }
        return;
    }

    TokenPosition prev = pt_pos_for(p, prev0);
    TokenPosition next = pt_pos_for(p, next0);
    Int line = pt_line_for(p, ast_node_pos(PT_AT(list, AstExpr, 0)));
    Int end_line = pt_line_for(p, ast_node_end(PT_AT(list, AstExpr, list.len - 1)));

    if (pt_valid(prev) && prev.line == line && line == end_line) {
        /* the whole list on one line */
        for (Int i = 0; i < list.len; i++) {
            AstExpr x = PT_AT(list, AstExpr, i);
            if (i > 0) {
                /* the comma takes the position of the expression after
                 * it, so comments land in the right place */
                pt_set_pos_node(p, x);
                pt_tok_ws(p, TOKEN_COMMA, PT_BLANK);
            }
            pt_expr0(p, x, depth);
        }
        if (is_incomplete) {
            pt_tok_ws(p, TOKEN_COMMA, PT_BLANK);
            pt_text(p, S("/* " PT_FILTERED_MSG " */"));
        }
        return;
    }

    /* The list is over several lines: let the source positions guide the
     * line breaks. */

    /* With PT_NO_INDENT, add no indentation: act as if the first line were
     * indented already. */
    PtWs ws = PT_IGNORE;
    if ((mode & PT_NO_INDENT) == 0)
        ws = PT_INDENT;

    /* The first line break is always a formfeed, since this section must
     * not depend on the layout before it. */
    Int prev_break = -1; /* the last expression followed by a line break */
    if (pt_valid(prev) && prev.line < line && pt_linebreak(p, line, 0, ws, true) > 0) {
        ws = PT_IGNORE;
        prev_break = 0;
    }

    /* the size of the expression or key; 0 means it does not fit on a line */
    Int size = 0;

    /* The ratio of the current size to the geometric mean of the sizes
     * before it decides whether to break the alignment. The mean comes from
     * the sum of the log2 of the sizes and their count. */
    double log2sum = 0.0;
    Int count = 0;

    /* every element of the list */
    Int prev_line = prev.line;
    for (Int i = 0; i < list.len; i++) {
        AstExpr x = PT_AT(list, AstExpr, i);
        line = pt_line_for(p, ast_node_pos(x));

        /* Whether the next line break, if any, is a formfeed: the size of
         * the whole node decides, or the key's for a key: value pair. */
        bool use_ff = true;

        /* The element's size. Without positions for the tokens around it
         * (generated code, most likely) the size counts for nothing and is
         * 0. */
        Int prev_size = size;
        const Int infinity = 1000000; /* longer than any source line */
        size = pt_node_size(p, x, infinity);
        AstKeyValueExpr *pair = NULL;
        if (PT_KIND(x) == AST_KIND_KEY_VALUE_EXPR)
            pair = (AstKeyValueExpr *)x;
        if (size <= infinity && pt_valid(prev) && pt_valid(next)) {
            /* x fits on one line */
            if (pair != NULL)
                size = pt_node_size(p, pair->key, infinity); /* size <= infinity */
        } else {
            /* too big, or no good layout information */
            size = 0;
        }

        /* If this line and the one before have expressions of one line
         * each, and the keys are small or the ratio of this key to the
         * geometric mean of the keys before is under a threshold, align the
         * columns and use no formfeed. */
        if (prev_size > 0 && size > 0) {
            const Int small_size = 40;
            if (count == 0 || (prev_size <= small_size && size <= small_size)) {
                use_ff = false;
            } else {
                const double r = 2.5;                                 /* threshold */
                double geomean = pt_exp2ish(log2sum / (double)count); /* count > 0 */
                double ratio = (double)size / geomean;
                use_ff = r * ratio <= 1 || r <= ratio;
            }
        }

        bool needs_linebreak = 0 < prev_line && prev_line < line;
        if (i > 0) {
            /* The comma takes the position of the expression after it, so
             * comments land in the right place, but only when that is on
             * the same line. */
            if (!needs_linebreak)
                pt_set_pos_node(p, x);
            pt_tok(p, TOKEN_COMMA);
            bool needs_blank = true;
            if (needs_linebreak) {
                /* Lines break with newlines so comments stay aligned, unless
                 * use_ff is set or a line held several expressions, which
                 * take a formfeed. */
                Int nbreaks =
                    pt_linebreak(p, line, 0, ws, use_ff || prev_break + 1 < i);
                if (nbreaks > 0) {
                    ws = PT_IGNORE;
                    prev_break = i;
                    needs_blank = false; /* a line break instead */
                }
                /* A new section, or more than one newline (which makes the
                 * tabwriter break the section), starts a new group of
                 * elements with the next one, so start the mean again. */
                if (nbreaks > 1) {
                    log2sum = 0;
                    count = 0;
                }
            }
            if (needs_blank)
                pt_ws(p, PT_BLANK);
        }

        if (list.len > 1 && pair != NULL && size > 0 && needs_linebreak) {
            /* A key: value pair that fits on a line, not on the line of the
             * expression before it: put the key in a column so the entries
             * can line up. */
            pt_expr(p, pair->key);
            pt_set_pos(p, pair->colon);
            pt_tok_ws(p, TOKEN_COLON, PT_VTAB);
            pt_expr(p, pair->value);
        } else {
            pt_expr0(p, x, depth);
        }

        if (size > 0) {
            log2sum += pt_log2ish((double)size);
            count++;
        }

        prev_line = line;
    }

    if ((mode & PT_COMMA_TERM) != 0 && pt_valid(next) && p->pos.line < next.line) {
        /* a comma at the end if the next token is on a new line */
        pt_tok(p, TOKEN_COMMA);
        if (is_incomplete) {
            pt_ws(p, PT_NEWLINE);
            pt_text(p, S("// " PT_FILTERED_MSG));
        }
        if (ws == PT_IGNORE && (mode & PT_NO_INDENT) == 0) {
            /* unindent if indented */
            pt_ws(p, PT_UNINDENT);
        }
        pt_ws(p, PT_FORMFEED); /* the comma needs a line break to look good */
        return;
    }

    if (is_incomplete) {
        pt_tok_ws(p, TOKEN_COMMA, PT_NEWLINE);
        pt_text(p, S("// " PT_FILTERED_MSG));
        pt_ws(p, PT_NEWLINE);
    }

    if (ws == PT_IGNORE && (mode & PT_NO_INDENT) == 0) {
        /* unindent if indented */
        pt_ws(p, PT_UNINDENT);
    }
}

/* paramMode. */
enum { PT_FUNC_PARAM, PT_FUNC_TPARAM, PT_TYPE_TPARAM };

static bool pt_combines_with_name(AstExpr x);

static void pt_parameters(PtPrinter *p, AstFieldList *fields, Int mode) {
    Token open_tok = TOKEN_LPAREN;
    Token close_tok = TOKEN_RPAREN;
    if (mode != PT_FUNC_PARAM) {
        open_tok = TOKEN_LBRACK;
        close_tok = TOKEN_RBRACK;
    }
    pt_set_pos(p, fields->opening);
    pt_tok(p, open_tok);
    if (fields->list.len > 0) {
        Int prev_line = pt_line_for(p, fields->opening);
        PtWs ws = PT_INDENT;
        for (Int i = 0; i < fields->list.len; i++) {
            AstField *par = PT_AT(fields->list, AstField *, i);
            /* the lines the parameter starts and ends on, which differ if
             * it has several names or its type is on another line */
            Int par_line_beg = pt_line_for(p, ast_node_pos(PT_NODE(par)));
            Int par_line_end = pt_line_for(p, ast_node_end(PT_NODE(par)));
            /* a comma if needed */
            bool needs_linebreak = 0 < prev_line && prev_line < par_line_beg;
            if (i > 0) {
                /* the comma takes the position of the next parameter, but
                 * only when that is on the same line */
                if (!needs_linebreak)
                    pt_set_pos_node(p, PT_NODE(par));
                pt_tok(p, TOKEN_COMMA);
            }
            /* a separator if needed, a line break or a blank */
            if (needs_linebreak && pt_linebreak(p, par_line_beg, 0, ws, true) > 0) {
                /* break the line if the ( or the parameter before ended on
                 * another line */
                ws = PT_IGNORE;
            } else if (i > 0) {
                pt_ws(p, PT_BLANK);
            }
            /* the names */
            if (par->names.len > 0) {
                /* Subtle. If there was an indent before (ws is PT_IGNORE),
                 * identList adds none. If not (ws is PT_INDENT), identList
                 * indents a list over several lines and unindents at its
                 * end, leaving ws as it was. Either way a later indent, by
                 * a line break after a type or in the next long list, does
                 * the right thing. */
                pt_ident_list(p, par->names, ws == PT_INDENT);
                pt_ws(p, PT_BLANK);
            }
            /* the type */
            pt_expr(p, pt_strip_parens_always(par->type));
            prev_line = par_line_end;
        }

        /* With the ) on another line than the last parameter, add a comma
         * and a line break. */
        Int closing = pt_line_for(p, fields->closing);
        if (0 < prev_line && prev_line < closing) {
            pt_tok(p, TOKEN_COMMA);
            pt_linebreak(p, closing, 0, PT_IGNORE, true);
        } else if (mode == PT_TYPE_TPARAM && ast_field_list_num_fields(fields) == 1 &&
                   pt_combines_with_name(pt_strip_parens_always(
                       PT_AT(fields->list, AstField *, 0)->type))) {
            /* A type parameter list [P T] where P and T together make
             * another valid expression needs a comma at the end, as in
             * [P *T,] (or an interface around T, as in [P interface{*T}]),
             * so it is not parsed as the array length [P*T]. */
            pt_tok(p, TOKEN_COMMA);
        }

        /* unindent if indented */
        if (ws == PT_IGNORE)
            pt_ws(p, PT_UNINDENT);
    }

    pt_set_pos(p, fields->closing);
    pt_tok(p, close_tok);
}

/* isTypeElem: whether x is a type element expression, perhaps in
 * parentheses. It is false if x could be one or an ordinary expression. */
static bool pt_is_type_elem(AstExpr x) {
    switch ((int)PT_KIND(x)) {
    case AST_KIND_ARRAY_TYPE:
    case AST_KIND_STRUCT_TYPE:
    case AST_KIND_FUNC_TYPE:
    case AST_KIND_INTERFACE_TYPE:
    case AST_KIND_MAP_TYPE:
    case AST_KIND_CHAN_TYPE:
        return true;
    case AST_KIND_UNARY_EXPR:
        return ((AstUnaryExpr *)x)->op == TOKEN_TILDE;
    case AST_KIND_BINARY_EXPR: {
        AstBinaryExpr *b = (AstBinaryExpr *)x;
        return pt_is_type_elem(b->x) || pt_is_type_elem(b->y);
    }
    case AST_KIND_PAREN_EXPR:
        return pt_is_type_elem(((AstParenExpr *)x)->x);
    default:
        return false;
    }
}

/* combinesWithName: whether a name followed by x makes another valid
 * expression, as "name *T" reads as name*T. "name P|Q" or "name *P|~Q"
 * cannot. */
static bool pt_combines_with_name(AstExpr x) {
    switch ((int)PT_KIND(x)) {
    case AST_KIND_STAR_EXPR:
        /* name *x.X makes name*x.X if x.X is not a type element */
        return !pt_is_type_elem(((AstStarExpr *)x)->x);
    case AST_KIND_BINARY_EXPR: {
        AstBinaryExpr *b = (AstBinaryExpr *)x;
        return pt_combines_with_name(b->x) && !pt_is_type_elem(b->y);
    }
    case AST_KIND_PAREN_EXPR:
        return !pt_is_type_elem(((AstParenExpr *)x)->x);
    default:
        return false;
    }
}

static void pt_signature(PtPrinter *p, AstFuncType *sig) {
    if (sig->type_params != NULL)
        pt_parameters(p, sig->type_params, PT_FUNC_TPARAM);
    if (sig->params != NULL)
        pt_parameters(p, sig->params, PT_FUNC_PARAM);
    else
        pt_tok2(p, TOKEN_LPAREN, TOKEN_RPAREN);
    AstFieldList *res = sig->results;
    Int n = ast_field_list_num_fields(res);
    if (n > 0) {
        /* res is not NULL */
        pt_ws(p, PT_BLANK);
        AstField *f0 = PT_AT(res->list, AstField *, 0);
        if (n == 1 && f0->names.p == NULL) {
            /* one result with no name: no parentheses */
            pt_expr(p, pt_strip_parens_always(f0->type));
            return;
        }
        pt_parameters(p, res, PT_FUNC_PARAM);
    }
}

static Int pt_ident_list_size(Slice list, Int max_size) {
    Int size = 0;
    for (Int i = 0; i < list.len; i++) {
        if (i > 0)
            size += 2; /* ", " */
        size += utf8_rune_count_in_string(PT_AT(list, AstIdent *, i)->name);
        if (size >= max_size)
            break;
    }
    return size;
}

static bool pt_is_one_line_field_list(PtPrinter *p, Slice list) {
    if (list.len != 1)
        return false; /* one field only */
    AstField *f = PT_AT(list, AstField *, 0);
    if (f->tag != NULL || f->comment != NULL)
        return false; /* no tags or comments */
    /* only names and a type */
    const Int max_size = 30; /* about right */
    Int names_size = pt_ident_list_size(f->names, max_size);
    if (names_size > 0)
        names_size = 1; /* the blank between the names and the type */
    Int type_size = pt_node_size(p, f->type, max_size);
    return names_size + type_size <= max_size;
}

static void pt_set_line_comment(PtPrinter *p, Str text) {
    AstComment *c = pt_new_comment(p, TOKEN_NO_POS, text);
    AstComment **one = (AstComment **)pt_alloc(p, sizeof *one);
    one[0] = c;
    AstCommentGroup *g = (AstCommentGroup *)pt_alloc(p, sizeof *g);
    memset(g, 0, sizeof *g);
    g->node.kind = AST_KIND_COMMENT_GROUP;
    g->list = (Slice){(void *)one, 1, 1, TYPE_AST_COMMENT_PTR};
    pt_set_comment(p, g);
}

/* recordLine hands the printer a pointer to a local. Drop it before the
 * local goes away, in case no token came along to use it. */
static void pt_forget_line(PtPrinter *p, const Int *line) {
    if (p->line_ptr == line)
        p->line_ptr = NULL;
}

static void pt_field_list(PtPrinter *p, AstFieldList *fields, bool is_struct,
                          bool is_incomplete) {
    TokenPos lbrace = fields->opening;
    Slice list = fields->list;
    TokenPos rbrace = fields->closing;
    bool has_comments = is_incomplete || pt_comment_before(p, pt_pos_for(p, rbrace));
    bool src_is_one_line = token_pos_is_valid(lbrace) && token_pos_is_valid(rbrace) &&
                           pt_line_for(p, lbrace) == pt_line_for(p, rbrace);

    if (!has_comments && src_is_one_line) {
        /* perhaps a struct or interface on one line */
        if (list.len == 0) {
            /* no blank between the keyword and {} here */
            pt_set_pos(p, lbrace);
            pt_tok(p, TOKEN_LBRACE);
            pt_set_pos(p, rbrace);
            pt_tok(p, TOKEN_RBRACE);
            return;
        }
        if (pt_is_one_line_field_list(p, list)) {
            /* small enough for one line (no identList, and the source's
             * line breaks do not count) */
            pt_set_pos(p, lbrace);
            pt_tok_ws(p, TOKEN_LBRACE, PT_BLANK);
            AstField *f = PT_AT(list, AstField *, 0);
            if (is_struct) {
                for (Int i = 0; i < f->names.len; i++) {
                    if (i > 0) {
                        /* no comments, so the comma needs no position */
                        pt_tok_ws(p, TOKEN_COMMA, PT_BLANK);
                    }
                    pt_expr(p, PT_NODE(PT_AT(f->names, AstIdent *, i)));
                }
                if (f->names.len > 0)
                    pt_ws(p, PT_BLANK);
                pt_expr(p, f->type);
            } else { /* interface */
                if (f->names.len > 0) {
                    AstIdent *name = PT_AT(f->names, AstIdent *, 0); /* method name */
                    pt_expr(p, PT_NODE(name));
                    pt_signature(p, (AstFuncType *)f->type); /* no "func" */
                } else {
                    /* embedded interface */
                    pt_expr(p, f->type);
                }
            }
            pt_ws(p, PT_BLANK);
            pt_set_pos(p, rbrace);
            pt_tok(p, TOKEN_RBRACE);
            return;
        }
    }
    /* has_comments || !src_is_one_line */

    pt_ws(p, PT_BLANK);
    pt_set_pos(p, lbrace);
    pt_tok_ws(p, TOKEN_LBRACE, PT_INDENT);
    if (has_comments || list.len > 0)
        pt_ws(p, PT_FORMFEED);

    if (is_struct) {
        PtWs sep = PT_VTAB;
        if (list.len == 1)
            sep = PT_BLANK;
        Int line = 0;
        for (Int i = 0; i < list.len; i++) {
            AstField *f = PT_AT(list, AstField *, i);
            if (i > 0)
                pt_linebreak(p, pt_line_for(p, ast_node_pos(PT_NODE(f))), 1, PT_IGNORE,
                             pt_lines_from(p, line) > 0);
            Int extra_tabs = 0;
            pt_set_comment(p, f->doc);
            pt_record_line(p, &line);
            if (f->names.len > 0) {
                /* named fields */
                pt_ident_list(p, f->names, false);
                pt_ws(p, sep);
                pt_expr(p, f->type);
                extra_tabs = 1;
            } else {
                /* anonymous field */
                pt_expr(p, f->type);
                extra_tabs = 2;
            }
            if (f->tag != NULL) {
                if (f->names.len > 0 && sep == PT_VTAB)
                    pt_ws(p, sep);
                pt_ws(p, sep);
                pt_expr(p, PT_NODE(f->tag));
                extra_tabs = 0;
            }
            if (f->comment != NULL) {
                for (; extra_tabs > 0; extra_tabs--)
                    pt_ws(p, sep);
                pt_set_comment(p, f->comment);
            }
        }
        if (is_incomplete) {
            if (list.len > 0)
                pt_ws(p, PT_FORMFEED);
            /* keep the last line comment */
            bool wrote_newline = false;
            bool dropped_ff = false;
            pt_flush(p, pt_pos_for(p, rbrace), TOKEN_RBRACE, &wrote_newline,
                     &dropped_ff);
            pt_set_line_comment(p, S("// " PT_FILTERED_MSG));
        }
        pt_forget_line(p, &line);
    } else { /* interface */
        Int line = 0;
        for (Int i = 0; i < list.len; i++) {
            AstField *f = PT_AT(list, AstField *, i);
            AstIdent *name = NULL; /* the first name, or NULL */
            if (f->names.len > 0)
                name = PT_AT(f->names, AstIdent *, 0);
            if (i > 0) {
                /* Go keeps the last "type" name here to put no line break
                 * in a list of types, but never sets it, so min is 1. */
                pt_linebreak(p, pt_line_for(p, ast_node_pos(PT_NODE(f))), 1, PT_IGNORE,
                             pt_lines_from(p, line) > 0);
            }
            pt_set_comment(p, f->doc);
            pt_record_line(p, &line);
            if (name != NULL) {
                /* method */
                pt_expr(p, PT_NODE(name));
                pt_signature(p, (AstFuncType *)f->type); /* no "func" */
            } else {
                /* embedded interface */
                pt_expr(p, f->type);
            }
            pt_set_comment(p, f->comment);
        }
        if (is_incomplete) {
            if (list.len > 0)
                pt_ws(p, PT_FORMFEED);
            /* keep the last line comment */
            bool wrote_newline = false;
            bool dropped_ff = false;
            pt_flush(p, pt_pos_for(p, rbrace), TOKEN_RBRACE, &wrote_newline,
                     &dropped_ff);
            pt_set_line_comment(p, S("// contains filtered or unexported methods"));
        }
        pt_forget_line(p, &line);
    }
    pt_ws2(p, PT_UNINDENT, PT_FORMFEED);
    pt_set_pos(p, rbrace);
    pt_tok(p, TOKEN_RBRACE);
}

/* ------------------------------------------------------------ expressions */

static void pt_expr1(PtPrinter *p, AstExpr expr, Int prec1, Int depth);

static bool pt_is_binary(AstExpr x) {
    return x != NULL && PT_KIND(x) == AST_KIND_BINARY_EXPR;
}

static void pt_walk_binary(AstBinaryExpr *e, bool *has4, bool *has5, Int *max_problem) {
    Int eprec = token_precedence(e->op);
    switch (eprec) {
    case 4:
        *has4 = true;
        break;
    case 5:
        *has5 = true;
        break;
    default:
        break;
    }

    if (PT_KIND(e->x) == AST_KIND_BINARY_EXPR) {
        AstBinaryExpr *l = (AstBinaryExpr *)e->x;
        /* With a lower precedence parentheses go in: take it as a
         * ParenExpr and do nothing. */
        if (token_precedence(l->op) >= eprec) {
            bool h4 = false, h5 = false;
            Int mp = 0;
            pt_walk_binary(l, &h4, &h5, &mp);
            *has4 = *has4 || h4;
            *has5 = *has5 || h5;
            if (mp > *max_problem)
                *max_problem = mp;
        }
    }

    switch ((int)PT_KIND(e->y)) {
    case AST_KIND_BINARY_EXPR: {
        AstBinaryExpr *r = (AstBinaryExpr *)e->y;
        /* as for the left side */
        if (token_precedence(r->op) > eprec) {
            bool h4 = false, h5 = false;
            Int mp = 0;
            pt_walk_binary(r, &h4, &h5, &mp);
            *has4 = *has4 || h4;
            *has5 = *has5 || h5;
            if (mp > *max_problem)
                *max_problem = mp;
        }
        break;
    }
    case AST_KIND_STAR_EXPR:
        if (e->op == TOKEN_QUO) /* a slash and a star */
            *max_problem = 5;
        break;
    case AST_KIND_UNARY_EXPR: {
        Token rop = ((AstUnaryExpr *)e->y)->op;
        if ((e->op == TOKEN_QUO && rop == TOKEN_MUL) ||
            (e->op == TOKEN_AND && (rop == TOKEN_AND || rop == TOKEN_XOR))) {
            *max_problem = 5;
        } else if ((e->op == TOKEN_ADD && rop == TOKEN_ADD) ||
                   (e->op == TOKEN_SUB && rop == TOKEN_SUB)) {
            if (*max_problem < 4)
                *max_problem = 4;
        }
        break;
    }
    default:
        break;
    }
}

static Int pt_cutoff(AstBinaryExpr *e, Int depth) {
    bool has4 = false, has5 = false;
    Int max_problem = 0;
    pt_walk_binary(e, &has4, &has5, &max_problem);
    if (max_problem > 0)
        return max_problem + 1;
    if (has4 && has5) {
        if (depth == 1)
            return 5;
        return 4;
    }
    if (depth == 1)
        return 6;
    return 4;
}

static Int pt_diff_prec(AstExpr expr, Int prec) {
    if (PT_KIND(expr) != AST_KIND_BINARY_EXPR ||
        prec != token_precedence(((AstBinaryExpr *)expr)->op))
        return 1;
    return 0;
}

static Int pt_reduce_depth(Int depth) {
    depth--;
    if (depth < 1)
        depth = 1;
    return depth;
}

/* binaryExpr: decide the cutoff, then print. Depth 1 is the normal mode and
 * a greater depth the compact one (Russ Cox's algorithm). The precedences
 * are
 *
 *     5    *  /  %  <<  >>  &  &^
 *     4    +  -  |  ^
 *     3    ==  !=  <  <=  >  >=
 *     2    &&
 *     1    ||
 *
 * and the only question is whether levels 4 and 5 get blanks around them.
 * Level 6 (unary) never does and levels 3 and below always do. The cutoff
 * comes from the whole expression, leaving out primary expressions (calls,
 * expressions in parentheses):
 *
 *  1. A binary operator with a unary operand on its right that would clash
 *     with it without a blank sets the cutoff to 6 for a slash and a star,
 *     && or &^, and to 5 for ++ or --. (Comparisons always have blanks.)
 *
 *  2. A mix of level 5 and level 4 operators sets it to 5 (blanks show the
 *     precedence) in normal mode and 4 (no blanks) in compact mode.
 *
 *  3. With no level 4 or no level 5 operators it is 6 (blanks always) in
 *     normal mode and 4 (never) in compact mode. */
static void pt_binary_expr(PtPrinter *p, AstBinaryExpr *x, Int prec1, Int cutoff,
                           Int depth) {
    Int prec = token_precedence(x->op);
    if (prec < prec1) {
        /* Parentheses needed. The parser makes a ParenExpr, so this only
         * happens with a tree made some other way. */
        pt_tok(p, TOKEN_LPAREN);
        pt_expr0(p, PT_NODE(x), pt_reduce_depth(depth)); /* one level less inside */
        pt_tok(p, TOKEN_RPAREN);
        return;
    }

    bool print_blank = prec < cutoff;

    PtWs ws = PT_INDENT;
    pt_expr1(p, x->x, prec, depth + pt_diff_prec(x->x, prec));
    if (print_blank)
        pt_ws(p, PT_BLANK);
    Int xline = p->pos.line; /* before the operator, which can be on the next line */
    Int yline = pt_line_for(p, ast_node_pos(x->y));
    pt_set_pos(p, x->op_pos);
    pt_tok(p, x->op);
    if (xline != yline && xline > 0 && yline > 0) {
        /* at least one line break, keeping an extra empty line from the
         * source */
        if (pt_linebreak(p, yline, 1, ws, true) > 0) {
            ws = PT_IGNORE;
            print_blank = false; /* no blank after a line break */
        }
    }
    if (print_blank)
        pt_ws(p, PT_BLANK);
    pt_expr1(p, x->y, prec + 1, depth + 1);
    if (ws == PT_IGNORE)
        pt_ws(p, PT_UNINDENT);
}

/* normalizedNumber: lit with lower case base prefixes and exponents (0X123
 * as 0x123, 1.2E3 as 1.2e3) and integer imaginary literals without leading
 * zeros (0765i as 765i). Hexadecimal digits stay as they are. lit is left
 * alone and comes back if it is not a number or is canonical already;
 * otherwise the result is a new literal. */
static AstBasicLit *pt_normalized_number(PtPrinter *p, AstBasicLit *lit) {
    if (lit->kind != TOKEN_INT && lit->kind != TOKEN_FLOAT && lit->kind != TOKEN_IMAG)
        return lit; /* not a number */
    if (lit->value.len < 2)
        return lit; /* one digit, the common case */

    /* The kind does not help, since an imaginary literal can be an integer
     * or a float, decimal or not. Only the text counts. */
    Str v = lit->value;
    Int n = v.len;
    Byte *x = (Byte *)pt_alloc(p, (size_t)n);
    memcpy(x, v.p, (size_t)n);
    Str out = str_from_bytes(x, n);
    if (x[0] == '0' && x[1] == 'X') {
        x[1] = 'x';
        /* perhaps a hexadecimal float */
        Int i = strings_last_index_byte(out, 'P');
        if (i >= 0)
            x[i] = 'p';
    } else if (x[0] == '0' && x[1] == 'x') {
        /* perhaps a hexadecimal float */
        Int i = strings_last_index_byte(out, 'P');
        if (i == -1)
            return lit; /* nothing to do */
        x[i] = 'p';
    } else if (x[0] == '0' && (x[1] == 'o' || x[1] == 'b')) {
        return lit; /* nothing to do */
    } else if (x[0] == '0' && x[1] == 'O') {
        x[1] = 'o';
    } else if (x[0] == '0' && x[1] == 'B') {
        x[1] = 'b';
    } else {
        /* an octal with a 0 prefix, a decimal integer or a float, perhaps
         * with an i suffix */
        Int i = strings_last_index_byte(out, 'E');
        if (i >= 0) {
            x[i] = 'e';
        } else if (x[n - 1] == 'i' && !strings_contains_any(out, S(".e"))) {
            /* leading zeros off an integer (not a float) imaginary */
            out = strings_trim_left(out, S("0_"));
            if (out.len == 1)
                out = S("0i");
        }
    }

    AstBasicLit *r = (AstBasicLit *)pt_alloc(p, sizeof *r);
    memset(r, 0, sizeof *r);
    r->node.kind = AST_KIND_BASIC_LIT;
    r->value_pos = lit->value_pos;
    r->kind = lit->kind;
    r->value = out;
    return r;
}

static bool pt_selector_expr(PtPrinter *p, AstSelectorExpr *x, Int depth,
                             bool is_method);

static bool pt_possible_selector_expr(PtPrinter *p, AstExpr expr, Int prec1,
                                      Int depth) {
    if (PT_KIND(expr) == AST_KIND_SELECTOR_EXPR)
        return pt_selector_expr(p, (AstSelectorExpr *)expr, depth, true);
    pt_expr1(p, expr, prec1, depth);
    return false;
}

/* selectorExpr: print x and say whether it is over several lines. */
static bool pt_selector_expr(PtPrinter *p, AstSelectorExpr *x, Int depth,
                             bool is_method) {
    pt_expr1(p, x->x, TOKEN_HIGHEST_PREC, depth);
    pt_tok(p, TOKEN_PERIOD);
    Int line = pt_line_for(p, ast_node_pos(PT_NODE(x->sel)));
    if (pt_valid(p->pos) && p->pos.line < line) {
        pt_ws2(p, PT_INDENT, PT_NEWLINE);
        pt_set_pos_node(p, PT_NODE(x->sel));
        pt_ident(p, x->sel);
        if (!is_method)
            pt_ws(p, PT_UNINDENT);
        return true;
    }
    pt_set_pos_node(p, PT_NODE(x->sel));
    pt_ident(p, x->sel);
    return false;
}

static void pt_func_body(PtPrinter *p, Int header_size, PtWs sep, AstBlockStmt *b);
static Int pt_distance_from(PtPrinter *p, TokenPos start_pos, Int start_out_col);

static void pt_slice_expr(PtPrinter *p, AstSliceExpr *x, Int depth) {
    pt_expr1(p, x->x, TOKEN_HIGHEST_PREC, 1);
    pt_set_pos(p, x->lbrack);
    pt_tok(p, TOKEN_LBRACK);
    AstExpr indices[3] = {x->low, x->high, x->max};
    Int nidx = x->max != NULL ? 3 : 2;
    /* whether ':' needs blanks around it */
    bool needs_blanks = false;
    if (depth <= 1) {
        Int index_count = 0;
        bool has_binaries = false;
        for (Int i = 0; i < nidx; i++) {
            if (indices[i] != NULL) {
                index_count++;
                if (pt_is_binary(indices[i]))
                    has_binaries = true;
            }
        }
        if (index_count > 1 && has_binaries)
            needs_blanks = true;
    }
    for (Int i = 0; i < nidx; i++) {
        AstExpr y = indices[i];
        if (i > 0) {
            if (indices[i - 1] != NULL && needs_blanks)
                pt_ws(p, PT_BLANK);
            pt_tok(p, TOKEN_COLON);
            if (y != NULL && needs_blanks)
                pt_ws(p, PT_BLANK);
        }
        if (y != NULL)
            pt_expr0(p, y, depth + 1);
    }
    pt_set_pos(p, x->rbrack);
    pt_tok(p, TOKEN_RBRACK);
}

static void pt_call_expr(PtPrinter *p, AstCallExpr *x, Int depth) {
    if (x->args.len > 1)
        depth++;

    /* Conversions to function literal types or <-chan types need the type
     * in parentheses. */
    bool paren = false;
    if (PT_KIND(x->fun) == AST_KIND_FUNC_TYPE)
        paren = true;
    else if (PT_KIND(x->fun) == AST_KIND_CHAN_TYPE)
        paren = ((AstChanType *)x->fun)->dir == AST_RECV;
    if (paren)
        pt_tok(p, TOKEN_LPAREN);
    bool was_indented = pt_possible_selector_expr(p, x->fun, TOKEN_HIGHEST_PREC, depth);
    if (paren)
        pt_tok(p, TOKEN_RPAREN);

    pt_set_pos(p, x->lparen);
    pt_tok(p, TOKEN_LPAREN);
    if (token_pos_is_valid(x->ellipsis)) {
        pt_expr_list(p, x->lparen, x->args, depth, 0, x->ellipsis, false);
        pt_set_pos(p, x->ellipsis);
        pt_tok(p, TOKEN_ELLIPSIS);
        if (token_pos_is_valid(x->rparen) &&
            pt_line_for(p, x->ellipsis) < pt_line_for(p, x->rparen)) {
            pt_tok_ws(p, TOKEN_COMMA, PT_FORMFEED);
        }
    } else {
        pt_expr_list(p, x->lparen, x->args, depth, PT_COMMA_TERM, x->rparen, false);
    }
    pt_set_pos(p, x->rparen);
    pt_tok(p, TOKEN_RPAREN);
    if (was_indented)
        pt_ws(p, PT_UNINDENT);
}

static void pt_expr1(PtPrinter *p, AstExpr expr, Int prec1, Int depth) {
    pt_set_pos_node(p, expr);

    switch ((int)PT_KIND(expr)) {
    case AST_KIND_BAD_EXPR:
        pt_text(p, S("BadExpr"));
        break;

    case AST_KIND_IDENT:
        pt_ident(p, (AstIdent *)expr);
        break;

    case AST_KIND_BINARY_EXPR: {
        AstBinaryExpr *x = (AstBinaryExpr *)expr;
        if (depth < 1)
            depth = 1; /* Go reports this as an internal error in debug mode */
        pt_binary_expr(p, x, prec1, pt_cutoff(x, depth), depth);
        break;
    }

    case AST_KIND_KEY_VALUE_EXPR: {
        AstKeyValueExpr *x = (AstKeyValueExpr *)expr;
        pt_expr(p, x->key);
        pt_set_pos(p, x->colon);
        pt_tok_ws(p, TOKEN_COLON, PT_BLANK);
        pt_expr(p, x->value);
        break;
    }

    case AST_KIND_STAR_EXPR: {
        AstStarExpr *x = (AstStarExpr *)expr;
        if (TOKEN_UNARY_PREC < prec1) {
            /* parentheses needed */
            pt_tok2(p, TOKEN_LPAREN, TOKEN_MUL);
            pt_expr(p, x->x);
            pt_tok(p, TOKEN_RPAREN);
        } else {
            /* no parentheses needed */
            pt_tok(p, TOKEN_MUL);
            pt_expr(p, x->x);
        }
        break;
    }

    case AST_KIND_UNARY_EXPR: {
        AstUnaryExpr *x = (AstUnaryExpr *)expr;
        if (TOKEN_UNARY_PREC < prec1) {
            /* parentheses needed */
            pt_tok(p, TOKEN_LPAREN);
            pt_expr(p, expr);
            pt_tok(p, TOKEN_RPAREN);
        } else {
            /* no parentheses needed */
            pt_tok(p, x->op);
            if (x->op == TOKEN_RANGE)
                pt_ws(p, PT_BLANK);
            pt_expr1(p, x->x, TOKEN_UNARY_PREC, depth);
        }
        break;
    }

    case AST_KIND_BASIC_LIT: {
        AstBasicLit *x = (AstBasicLit *)expr;
        if ((p->cfg.mode & BURROW__PRINTER_NORMALIZE_NUMBERS) != 0)
            x = pt_normalized_number(p, x);
        pt_lit(p, x);
        break;
    }

    case AST_KIND_FUNC_LIT: {
        AstFuncLit *x = (AstFuncLit *)expr;
        pt_set_pos_node(p, PT_NODE(x->type));
        pt_tok(p, TOKEN_FUNC);
        /* See funcDecl for how the size of the header is worked out. */
        Int start_col = p->out.column - 4; /* len("func") */
        pt_signature(p, x->type);
        pt_func_body(p, pt_distance_from(p, ast_node_pos(PT_NODE(x->type)), start_col),
                     PT_BLANK, x->body);
        break;
    }

    case AST_KIND_PAREN_EXPR: {
        AstParenExpr *x = (AstParenExpr *)expr;
        if (PT_KIND(x->x) == AST_KIND_PAREN_EXPR) {
            /* no parentheses around an expression in parentheses already */
            pt_expr0(p, x->x, depth);
        } else {
            pt_tok(p, TOKEN_LPAREN);
            pt_expr0(p, x->x, pt_reduce_depth(depth)); /* one level less inside */
            pt_set_pos(p, x->rparen);
            pt_tok(p, TOKEN_RPAREN);
        }
        break;
    }

    case AST_KIND_SELECTOR_EXPR:
        pt_selector_expr(p, (AstSelectorExpr *)expr, depth, false);
        break;

    case AST_KIND_TYPE_ASSERT_EXPR: {
        AstTypeAssertExpr *x = (AstTypeAssertExpr *)expr;
        pt_expr1(p, x->x, TOKEN_HIGHEST_PREC, depth);
        pt_tok(p, TOKEN_PERIOD);
        pt_set_pos(p, x->lparen);
        pt_tok(p, TOKEN_LPAREN);
        if (x->type != NULL)
            pt_expr(p, x->type);
        else
            pt_tok(p, TOKEN_TYPE_);
        pt_set_pos(p, x->rparen);
        pt_tok(p, TOKEN_RPAREN);
        break;
    }

    case AST_KIND_INDEX_EXPR: {
        AstIndexExpr *x = (AstIndexExpr *)expr;
        pt_expr1(p, x->x, TOKEN_HIGHEST_PREC, 1);
        pt_set_pos(p, x->lbrack);
        pt_tok(p, TOKEN_LBRACK);
        pt_expr0(p, x->index, depth + 1);
        pt_set_pos(p, x->rbrack);
        pt_tok(p, TOKEN_RBRACK);
        break;
    }

    case AST_KIND_INDEX_LIST_EXPR: {
        AstIndexListExpr *x = (AstIndexListExpr *)expr;
        pt_expr1(p, x->x, TOKEN_HIGHEST_PREC, 1);
        pt_set_pos(p, x->lbrack);
        pt_tok(p, TOKEN_LBRACK);
        pt_expr_list(p, x->lbrack, x->indices, depth + 1, PT_COMMA_TERM, x->rbrack,
                     false);
        pt_set_pos(p, x->rbrack);
        pt_tok(p, TOKEN_RBRACK);
        break;
    }

    case AST_KIND_SLICE_EXPR:
        pt_slice_expr(p, (AstSliceExpr *)expr, depth);
        break;

    case AST_KIND_CALL_EXPR:
        pt_call_expr(p, (AstCallExpr *)expr, depth);
        break;

    case AST_KIND_COMPOSITE_LIT: {
        AstCompositeLit *x = (AstCompositeLit *)expr;
        /* elements that are composite literals can leave out the type */
        if (x->type != NULL)
            pt_expr1(p, x->type, TOKEN_HIGHEST_PREC, depth);
        p->level++;
        pt_set_pos(p, x->lbrace);
        pt_tok(p, TOKEN_LBRACE);
        pt_expr_list(p, x->lbrace, x->elts, 1, PT_COMMA_TERM, x->rbrace, x->incomplete);
        /* No extra line break after a block comment before the closing },
         * which could break the code if there is no comma at the end. */
        PtMode mode = PT_NO_EXTRA_LINEBREAK;
        /* and no extra blank there unless the literal is empty */
        if (x->elts.len > 0)
            mode |= PT_NO_EXTRA_BLANK;
        /* the indent is needed to print lone comments at the right level */
        pt_ws2(p, PT_INDENT, PT_UNINDENT);
        pt_mode(p, mode);
        pt_set_pos(p, x->rbrace);
        pt_tok(p, TOKEN_RBRACE);
        pt_mode(p, mode);
        p->level--;
        break;
    }

    case AST_KIND_ELLIPSIS: {
        AstEllipsis *x = (AstEllipsis *)expr;
        pt_tok(p, TOKEN_ELLIPSIS);
        if (x->elt != NULL)
            pt_expr(p, x->elt);
        break;
    }

    case AST_KIND_ARRAY_TYPE: {
        AstArrayType *x = (AstArrayType *)expr;
        pt_tok(p, TOKEN_LBRACK);
        if (x->len != NULL)
            pt_expr(p, x->len);
        pt_tok(p, TOKEN_RBRACK);
        pt_expr(p, x->elt);
        break;
    }

    case AST_KIND_STRUCT_TYPE: {
        AstStructType *x = (AstStructType *)expr;
        pt_tok(p, TOKEN_STRUCT);
        pt_field_list(p, x->fields, true, x->incomplete);
        break;
    }

    case AST_KIND_FUNC_TYPE:
        pt_tok(p, TOKEN_FUNC);
        pt_signature(p, (AstFuncType *)expr);
        break;

    case AST_KIND_INTERFACE_TYPE: {
        AstInterfaceType *x = (AstInterfaceType *)expr;
        pt_tok(p, TOKEN_INTERFACE);
        pt_field_list(p, x->methods, false, x->incomplete);
        break;
    }

    case AST_KIND_MAP_TYPE: {
        AstMapType *x = (AstMapType *)expr;
        pt_tok2(p, TOKEN_MAP, TOKEN_LBRACK);
        pt_expr(p, x->key);
        pt_tok(p, TOKEN_RBRACK);
        pt_expr(p, x->value);
        break;
    }

    case AST_KIND_CHAN_TYPE: {
        AstChanType *x = (AstChanType *)expr;
        switch ((int)x->dir) {
        case AST_SEND | AST_RECV:
            pt_tok(p, TOKEN_CHAN);
            break;
        case AST_RECV:
            pt_tok2(p, TOKEN_ARROW, TOKEN_CHAN); /* x->arrow is the node's pos */
            break;
        case AST_SEND:
            pt_tok(p, TOKEN_CHAN);
            pt_set_pos(p, x->arrow);
            pt_tok(p, TOKEN_ARROW);
            break;
        default:
            break;
        }
        pt_ws(p, PT_BLANK);
        pt_expr(p, x->value);
        break;
    }

    default:
        panic_str(S("unreachable"));
    }
}

static void pt_expr0(PtPrinter *p, AstExpr x, Int depth) {
    pt_expr1(p, x, TOKEN_LOWEST_PREC, depth);
}

static void pt_expr(PtPrinter *p, AstExpr x) {
    const Int depth = 1;
    pt_expr1(p, x, TOKEN_LOWEST_PREC, depth);
}

/* ------------------------------------------------------------- statements */

/* stmtList: the statements, indented, with no newline after the last. The
 * source's extra line breaks between statements are kept, but no more than
 * one empty line. */
static void pt_stmt_list(PtPrinter *p, Slice list, Int nindent, bool next_is_rbrace) {
    if (nindent > 0)
        pt_ws(p, PT_INDENT);
    Int line = 0;
    Int i = 0;
    for (Int k = 0; k < list.len; k++) {
        AstStmt s = PT_AT(list, AstStmt, k);
        /* empty statements are left out (issue 3466) */
        if (PT_KIND(s) == AST_KIND_EMPTY_STMT)
            continue;
        /* nindent is 0 only for the case clauses of a switch or select,
         * and there every clause is a new section */
        if (p->output_len > 0) {
            /* a line break, unless this is the start of the output (when
             * printing part of a program) */
            pt_linebreak(p, pt_line_for(p, ast_node_pos(s)), 1, PT_IGNORE,
                         i == 0 || nindent == 0 || pt_lines_from(p, line) > 0);
        }
        pt_record_line(p, &line);
        pt_stmt(p, s, next_is_rbrace && i == list.len - 1);
        /* Labels go on lines of their own, but this wants the line the
         * statement itself starts on, so count a line for each label. */
        for (AstStmt t = s; t != NULL && PT_KIND(t) == AST_KIND_LABELED_STMT;
             t = ((AstLabeledStmt *)t)->stmt)
            line++;
        i++;
    }
    pt_forget_line(p, &line);
    if (nindent > 0)
        pt_ws(p, PT_UNINDENT);
}

/* block: b, which always takes at least two lines. */
static void pt_block(PtPrinter *p, AstBlockStmt *b, Int nindent) {
    pt_set_pos(p, b->lbrace);
    pt_tok(p, TOKEN_LBRACE);
    pt_stmt_list(p, b->list, nindent, true);
    pt_linebreak(p, pt_line_for(p, b->rbrace), 1, PT_IGNORE, true);
    pt_set_pos(p, b->rbrace);
    pt_tok(p, TOKEN_RBRACE);
}

static bool pt_is_type_name(AstExpr x) {
    if (x == NULL)
        return false;
    switch ((int)PT_KIND(x)) {
    case AST_KIND_IDENT:
        return true;
    case AST_KIND_SELECTOR_EXPR:
        return pt_is_type_name(((AstSelectorExpr *)x)->x);
    default:
        return false;
    }
}

static bool pt_strip_visit(void *env, AstNode node) {
    bool *strip = (bool *)env;
    if (node == NULL)
        return true;
    switch ((int)PT_KIND(node)) {
    case AST_KIND_PAREN_EXPR:
        /* parentheses protect the composite literals inside them */
        return false;
    case AST_KIND_COMPOSITE_LIT:
        if (pt_is_type_name(((AstCompositeLit *)node)->type))
            *strip = false; /* keep the parentheses */
        return false;
    default:
        /* look further in anything else */
        return true;
    }
}

static AstExpr pt_strip_parens(AstExpr x) {
    if (PT_KIND(x) == AST_KIND_PAREN_EXPR) {
        AstParenExpr *px = (AstParenExpr *)x;
        /* The parentheses stay if there is a composite literal starting
         * with a type name that is not in parentheses of its own. */
        bool strip = true;
        ast_inspect(px->x, (AstInspectFunc){pt_strip_visit, &strip});
        if (strip)
            return pt_strip_parens(px->x);
    }
    return x;
}

static AstExpr pt_strip_parens_always(AstExpr x) {
    while (PT_KIND(x) == AST_KIND_PAREN_EXPR)
        x = ((AstParenExpr *)x)->x;
    return x;
}

static void pt_control_clause(PtPrinter *p, bool is_for_stmt, AstStmt init,
                              AstExpr expr, AstStmt post) {
    pt_ws(p, PT_BLANK);
    bool needs_blank = false;
    if (init == NULL && post == NULL) {
        /* no semicolons needed */
        if (expr != NULL) {
            pt_expr(p, pt_strip_parens(expr));
            needs_blank = true;
        }
    } else {
        /* all the semicolons needed (they are not separators here, so
         * they are printed by hand) */
        if (init != NULL)
            pt_stmt(p, init, false);
        pt_tok_ws(p, TOKEN_SEMICOLON, PT_BLANK);
        if (expr != NULL) {
            pt_expr(p, pt_strip_parens(expr));
            needs_blank = true;
        }
        if (is_for_stmt) {
            pt_tok_ws(p, TOKEN_SEMICOLON, PT_BLANK);
            needs_blank = false;
            if (post != NULL) {
                pt_stmt(p, post, false);
                needs_blank = true;
            }
        }
    }
    if (needs_blank)
        pt_ws(p, PT_BLANK);
}

/* isCompositeLitLike: whether x is a composite literal or has one at its
 * core, as &T{...} does, not counting parentheses. */
static bool pt_is_composite_lit_like(AstExpr x) {
    x = pt_strip_parens_always(x);
    switch ((int)PT_KIND(x)) {
    case AST_KIND_COMPOSITE_LIT:
        return true;
    case AST_KIND_UNARY_EXPR: {
        AstUnaryExpr *u = (AstUnaryExpr *)x;
        return u->op == TOKEN_AND &&
               PT_KIND(pt_strip_parens_always(u->x)) == AST_KIND_COMPOSITE_LIT;
    }
    default:
        return false;
    }
}

/* indentList: whether the list looks better indented as a whole, from the
 * first element on rather than from the first line break. Only return
 * statements use it. */
static bool pt_indent_list(PtPrinter *p, Slice list) {
    /* The heuristic: true if more than one element is over several lines
     * (a complex expression, say, but not a composite literal), or if an
     * element does not start on the line the one before it ends on. */
    if (list.len >= 2) {
        Int b = pt_line_for(p, ast_node_pos(PT_AT(list, AstExpr, 0)));
        Int e = pt_line_for(p, ast_node_end(PT_AT(list, AstExpr, list.len - 1)));
        if (0 < b && b < e) {
            /* the list is over several lines */
            Int n = 0; /* elements over several lines */
            Int line = b;
            for (Int i = 0; i < list.len; i++) {
                AstExpr x = PT_AT(list, AstExpr, i);
                Int xb = pt_line_for(p, ast_node_pos(x));
                Int xe = pt_line_for(p, ast_node_end(x));
                if (line < xb) {
                    /* x does not start on the line the one before ends */
                    return true;
                }
                if (xb < xe && !pt_is_composite_lit_like(x)) {
                    /* over several lines and not a composite literal
                     * (which has its own indentation, go.dev/issue/7195) */
                    n++;
                }
                line = xe;
            }
            return n > 1;
        }
    }
    return false;
}

static void pt_stmt(PtPrinter *p, AstStmt stmt, bool next_is_rbrace) {
    pt_set_pos_node(p, stmt);

    switch ((int)PT_KIND(stmt)) {
    case AST_KIND_BAD_STMT:
        pt_text(p, S("BadStmt"));
        break;

    case AST_KIND_DECL_STMT:
        pt_decl(p, ((AstDeclStmt *)stmt)->decl);
        break;

    case AST_KIND_EMPTY_STMT:
        /* nothing to do */
        break;

    case AST_KIND_LABELED_STMT: {
        AstLabeledStmt *s = (AstLabeledStmt *)stmt;
        /* A correcting unindent right after a line break goes before the
         * line break if no comment is between them (see
         * pt_write_whitespace). */
        pt_ws(p, PT_UNINDENT);
        pt_expr(p, PT_NODE(s->label));
        pt_set_pos(p, s->colon);
        pt_tok_ws(p, TOKEN_COLON, PT_INDENT);
        if (PT_KIND(s->stmt) == AST_KIND_EMPTY_STMT) {
            if (!next_is_rbrace) {
                pt_ws(p, PT_NEWLINE);
                pt_set_pos_node(p, s->stmt);
                pt_tok(p, TOKEN_SEMICOLON);
                break;
            }
        } else {
            pt_linebreak(p, pt_line_for(p, ast_node_pos(s->stmt)), 1, PT_IGNORE, true);
        }
        pt_stmt(p, s->stmt, next_is_rbrace);
        break;
    }

    case AST_KIND_EXPR_STMT: {
        const Int depth = 1;
        pt_expr0(p, ((AstExprStmt *)stmt)->x, depth);
        break;
    }

    case AST_KIND_SEND_STMT: {
        AstSendStmt *s = (AstSendStmt *)stmt;
        const Int depth = 1;
        pt_expr0(p, s->chan, depth);
        pt_ws(p, PT_BLANK);
        pt_set_pos(p, s->arrow);
        pt_tok_ws(p, TOKEN_ARROW, PT_BLANK);
        pt_expr0(p, s->value, depth);
        break;
    }

    case AST_KIND_INC_DEC_STMT: {
        AstIncDecStmt *s = (AstIncDecStmt *)stmt;
        const Int depth = 1;
        pt_expr0(p, s->x, depth + 1);
        pt_set_pos(p, s->tok_pos);
        pt_tok(p, s->tok);
        break;
    }

    case AST_KIND_ASSIGN_STMT: {
        AstAssignStmt *s = (AstAssignStmt *)stmt;
        Int depth = 1;
        if (s->lhs.len > 1 && s->rhs.len > 1)
            depth++;
        pt_expr_list(p, ast_node_pos(stmt), s->lhs, depth, 0, s->tok_pos, false);
        pt_ws(p, PT_BLANK);
        pt_set_pos(p, s->tok_pos);
        pt_tok_ws(p, s->tok, PT_BLANK);
        pt_expr_list(p, s->tok_pos, s->rhs, depth, 0, TOKEN_NO_POS, false);
        break;
    }

    case AST_KIND_GO_STMT:
        pt_tok_ws(p, TOKEN_GO, PT_BLANK);
        pt_expr(p, PT_NODE(((AstGoStmt *)stmt)->call));
        break;

    case AST_KIND_DEFER_STMT:
        pt_tok_ws(p, TOKEN_DEFER, PT_BLANK);
        pt_expr(p, PT_NODE(((AstDeferStmt *)stmt)->call));
        break;

    case AST_KIND_RETURN_STMT: {
        AstReturnStmt *s = (AstReturnStmt *)stmt;
        pt_tok(p, TOKEN_RETURN);
        if (s->results.p != NULL) {
            pt_ws(p, PT_BLANK);
            /* indentList makes some corner cases look better (issue
             * 1207). Always indenting would be more systematic, but would
             * reformat a lot of code for no clear gain. */
            if (pt_indent_list(p, s->results)) {
                pt_ws(p, PT_INDENT);
                /* TOKEN_NO_POS, so a newline never goes before the results
                 * (issue 32854) */
                pt_expr_list(p, TOKEN_NO_POS, s->results, 1, PT_NO_INDENT, TOKEN_NO_POS,
                             false);
                pt_ws(p, PT_UNINDENT);
            } else {
                pt_expr_list(p, TOKEN_NO_POS, s->results, 1, 0, TOKEN_NO_POS, false);
            }
        }
        break;
    }

    case AST_KIND_BRANCH_STMT: {
        AstBranchStmt *s = (AstBranchStmt *)stmt;
        pt_tok(p, s->tok);
        if (s->label != NULL) {
            pt_ws(p, PT_BLANK);
            pt_expr(p, PT_NODE(s->label));
        }
        break;
    }

    case AST_KIND_BLOCK_STMT:
        pt_block(p, (AstBlockStmt *)stmt, 1);
        break;

    case AST_KIND_IF_STMT: {
        AstIfStmt *s = (AstIfStmt *)stmt;
        pt_tok(p, TOKEN_IF);
        pt_control_clause(p, false, s->init, s->cond, NULL);
        pt_block(p, s->body, 1);
        if (s->else_ != NULL) {
            pt_ws(p, PT_BLANK);
            pt_tok_ws(p, TOKEN_ELSE, PT_BLANK);
            if (PT_KIND(s->else_) == AST_KIND_BLOCK_STMT ||
                PT_KIND(s->else_) == AST_KIND_IF_STMT) {
                pt_stmt(p, s->else_, next_is_rbrace);
            } else {
                /* Only a badly built tree gets here. Allow it, but print it
                 * so it parses. */
                pt_tok_ws(p, TOKEN_LBRACE, PT_INDENT);
                pt_ws(p, PT_FORMFEED);
                pt_stmt(p, s->else_, true);
                pt_ws2(p, PT_UNINDENT, PT_FORMFEED);
                pt_tok(p, TOKEN_RBRACE);
            }
        }
        break;
    }

    case AST_KIND_CASE_CLAUSE: {
        AstCaseClause *s = (AstCaseClause *)stmt;
        if (s->list.p != NULL) {
            pt_tok_ws(p, TOKEN_CASE, PT_BLANK);
            pt_expr_list(p, ast_node_pos(stmt), s->list, 1, 0, s->colon, false);
        } else {
            pt_tok(p, TOKEN_DEFAULT);
        }
        pt_set_pos(p, s->colon);
        pt_tok(p, TOKEN_COLON);
        pt_stmt_list(p, s->body, 1, next_is_rbrace);
        break;
    }

    case AST_KIND_SWITCH_STMT: {
        AstSwitchStmt *s = (AstSwitchStmt *)stmt;
        pt_tok(p, TOKEN_SWITCH);
        pt_control_clause(p, false, s->init, s->tag, NULL);
        pt_block(p, s->body, 0);
        break;
    }

    case AST_KIND_TYPE_SWITCH_STMT: {
        AstTypeSwitchStmt *s = (AstTypeSwitchStmt *)stmt;
        pt_tok(p, TOKEN_SWITCH);
        if (s->init != NULL) {
            pt_ws(p, PT_BLANK);
            pt_stmt(p, s->init, false);
            pt_tok(p, TOKEN_SEMICOLON);
        }
        pt_ws(p, PT_BLANK);
        pt_stmt(p, s->assign, false);
        pt_ws(p, PT_BLANK);
        pt_block(p, s->body, 0);
        break;
    }

    case AST_KIND_COMM_CLAUSE: {
        AstCommClause *s = (AstCommClause *)stmt;
        if (s->comm != NULL) {
            pt_tok_ws(p, TOKEN_CASE, PT_BLANK);
            pt_stmt(p, s->comm, false);
        } else {
            pt_tok(p, TOKEN_DEFAULT);
        }
        pt_set_pos(p, s->colon);
        pt_tok(p, TOKEN_COLON);
        pt_stmt_list(p, s->body, 1, next_is_rbrace);
        break;
    }

    case AST_KIND_SELECT_STMT: {
        AstSelectStmt *s = (AstSelectStmt *)stmt;
        pt_tok_ws(p, TOKEN_SELECT, PT_BLANK);
        AstBlockStmt *body = s->body;
        if (body->list.len == 0 && !pt_comment_before(p, pt_pos_for(p, body->rbrace))) {
            /* an empty select with no comments goes on one line */
            pt_set_pos(p, body->lbrace);
            pt_tok(p, TOKEN_LBRACE);
            pt_set_pos(p, body->rbrace);
            pt_tok(p, TOKEN_RBRACE);
        } else {
            pt_block(p, body, 0);
        }
        break;
    }

    case AST_KIND_FOR_STMT: {
        AstForStmt *s = (AstForStmt *)stmt;
        pt_tok(p, TOKEN_FOR);
        pt_control_clause(p, true, s->init, s->cond, s->post);
        pt_block(p, s->body, 1);
        break;
    }

    case AST_KIND_RANGE_STMT: {
        AstRangeStmt *s = (AstRangeStmt *)stmt;
        pt_tok_ws(p, TOKEN_FOR, PT_BLANK);
        if (s->key != NULL) {
            pt_expr(p, s->key);
            if (s->value != NULL) {
                /* the comma takes the position of the value after it, so
                 * comments land in the right place */
                pt_set_pos_node(p, s->value);
                pt_tok_ws(p, TOKEN_COMMA, PT_BLANK);
                pt_expr(p, s->value);
            }
            pt_ws(p, PT_BLANK);
            pt_set_pos(p, s->tok_pos);
            pt_tok_ws(p, s->tok, PT_BLANK);
        }
        pt_tok_ws(p, TOKEN_RANGE, PT_BLANK);
        pt_expr(p, pt_strip_parens(s->x));
        pt_ws(p, PT_BLANK);
        pt_block(p, s->body, 1);
        break;
    }

    default:
        panic_str(S("unreachable"));
    }
}

/* ----------------------------------------------------------- declarations */

/* keepTypeColumn: for a run of const or var specs, whether each one must keep
 * its type column, or whether the values (V) can move into the type column
 * (T). Only whole columns move, so they stay aligned. The declaration
 *
 *     const (
 *         foobar int = 42 // comment
 *         x          = 7  // comment
 *         foo
 *         bar = 991
 *     )
 *
 * gives the matrix below. A run of value columns can move into the type
 * column if none of its values has a type.
 *
 *     matrix    formatted    result
 *     T  V   ->   T  V   ->  true     there is a T, so the column stays
 *     -  V        -  V       true
 *     -  -        -  -       false
 *     -  V        V  -       false    V moves into the T column */
static bool *pt_keep_type_column(PtPrinter *p, Slice specs) {
    bool *m = (bool *)pt_alloc(p, (size_t)specs.len * sizeof(bool));
    memset(m, 0, (size_t)specs.len * sizeof(bool));

    Int i0 = -1; /* the start of the run, if i0 >= 0 */
    bool keep_type = false;
    for (Int i = 0; i < specs.len; i++) {
        AstValueSpec *t = PT_AT(specs, AstValueSpec *, i);
        if (t->values.p != NULL) {
            if (i0 < 0) {
                /* the start of a run of specs with values */
                i0 = i;
                keep_type = false;
            }
        } else if (i0 >= 0) {
            /* the end of a run */
            if (keep_type) {
                for (Int k = i0; k < i; k++)
                    m[k] = true;
            }
            i0 = -1;
        }
        if (t->type != NULL)
            keep_type = true;
    }
    if (i0 >= 0 && keep_type) {
        /* the end of a run */
        for (Int k = i0; k < specs.len; k++)
            m[k] = true;
    }
    return m;
}

static void pt_value_spec(PtPrinter *p, AstValueSpec *s, bool keep_type) {
    pt_set_comment(p, s->doc);
    pt_ident_list(p, s->names, false); /* always there */
    Int extra_tabs = 3;
    if (s->type != NULL || keep_type) {
        pt_ws(p, PT_VTAB);
        extra_tabs--;
    }
    if (s->type != NULL)
        pt_expr(p, s->type);
    if (s->values.p != NULL) {
        pt_ws(p, PT_VTAB);
        pt_tok_ws(p, TOKEN_ASSIGN, PT_BLANK);
        pt_expr_list(p, TOKEN_NO_POS, s->values, 1, 0, TOKEN_NO_POS, false);
        extra_tabs--;
    }
    if (s->comment != NULL) {
        for (; extra_tabs > 0; extra_tabs--)
            pt_ws(p, PT_VTAB);
        pt_set_comment(p, s->comment);
    }
}

static AstBasicLit *pt_sanitize_import_path(PtPrinter *p, AstBasicLit *lit) {
    /* A tree from go/parser holds a path in back or double quotes with no
     * bad characters in it, so most of this is not needed. A changed or
     * generated tree can hold anything though, and the work is cheap. */

    /* not a proper string: give back what there is */
    if (lit->kind != TOKEN_STRING)
        return lit;
    Error err = BURROW_NO_ERROR;
    Str s = strconv_unquote(p->a, lit->value, &err);
    if (BURROW_FAILED(err))
        return lit;

    /* An invalid path comes back as it is. The spec: "Implementation
     * restriction: A compiler may restrict ImportPaths to non-empty strings
     * using only characters belonging to Unicode's L, M, N, P, and S general
     * categories (the Graphic characters without spaces) and may also
     * exclude the characters !"#$%&'()*,:;<=>?[\]^`{|} and the Unicode
     * replacement character U+FFFD." */
    if (s.len == 0)
        return lit;
    Str illegal = S("!\"#$%&'()*,:;<=>?[\\]^{|}`");
    for (Int i = 0; i < s.len;) {
        Int size = 0;
        Rune r = utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &size);
        i += size;
        if (!unicode_is_graphic(r) || unicode_is_space(r) ||
            strings_contains_rune(illegal, r) || r == 0xFFFD)
            return lit;
    }

    /* otherwise the path in double quotes */
    s = strconv_quote(p->a, s);
    if (str_eq(s, lit->value))
        return lit; /* nothing wrong with lit */
    AstBasicLit *r = (AstBasicLit *)pt_alloc(p, sizeof *r);
    memset(r, 0, sizeof *r);
    r->node.kind = AST_KIND_BASIC_LIT;
    r->value_pos = lit->value_pos;
    r->kind = TOKEN_STRING;
    r->value = s;
    return r;
}

/* spec: n is the number of specs in the group. With do_indent, a list of
 * names over several lines is indented from its first line break. */
static void pt_spec(PtPrinter *p, AstSpec spec, Int n, bool do_indent) {
    switch ((int)PT_KIND(spec)) {
    case AST_KIND_IMPORT_SPEC: {
        AstImportSpec *s = (AstImportSpec *)spec;
        pt_set_comment(p, s->doc);
        if (s->name != NULL) {
            pt_expr(p, PT_NODE(s->name));
            pt_ws(p, PT_BLANK);
        }
        pt_expr(p, PT_NODE(pt_sanitize_import_path(p, s->path)));
        pt_set_comment(p, s->comment);
        pt_set_pos(p, s->end_pos);
        break;
    }

    case AST_KIND_VALUE_SPEC: {
        AstValueSpec *s = (AstValueSpec *)spec;
        /* n is 1 here; Go checks that in debug mode */
        pt_set_comment(p, s->doc);
        pt_ident_list(p, s->names, do_indent); /* always there */
        if (s->type != NULL) {
            pt_ws(p, PT_BLANK);
            pt_expr(p, s->type);
        }
        if (s->values.p != NULL) {
            pt_ws(p, PT_BLANK);
            pt_tok_ws(p, TOKEN_ASSIGN, PT_BLANK);
            pt_expr_list(p, TOKEN_NO_POS, s->values, 1, 0, TOKEN_NO_POS, false);
        }
        pt_set_comment(p, s->comment);
        break;
    }

    case AST_KIND_TYPE_SPEC: {
        AstTypeSpec *s = (AstTypeSpec *)spec;
        pt_set_comment(p, s->doc);
        pt_expr(p, PT_NODE(s->name));
        if (s->type_params != NULL)
            pt_parameters(p, s->type_params, PT_TYPE_TPARAM);
        if (n == 1)
            pt_ws(p, PT_BLANK);
        else
            pt_ws(p, PT_VTAB);
        if (token_pos_is_valid(s->assign))
            pt_tok_ws(p, TOKEN_ASSIGN, PT_BLANK);
        pt_expr(p, s->type);
        pt_set_comment(p, s->comment);
        break;
    }

    default:
        panic_str(S("unreachable"));
    }
}

static void pt_gen_decl(PtPrinter *p, AstGenDecl *d) {
    pt_set_comment(p, d->doc);
    pt_set_pos_node(p, PT_NODE(d));
    pt_tok_ws(p, d->tok, PT_BLANK);

    if (token_pos_is_valid(d->lparen) || d->specs.len != 1) {
        /* a group of declarations in parentheses */
        pt_set_pos(p, d->lparen);
        pt_tok(p, TOKEN_LPAREN);
        Int n = d->specs.len;
        if (n > 0) {
            pt_ws2(p, PT_INDENT, PT_FORMFEED);
            Int line = 0;
            if (n > 1 && (d->tok == TOKEN_CONST || d->tok == TOKEN_VAR)) {
                /* two or more const or var declarations in a group: does
                 * the type column stay? */
                bool *keep_type = pt_keep_type_column(p, d->specs);
                for (Int i = 0; i < n; i++) {
                    AstSpec s = PT_AT(d->specs, AstSpec, i);
                    if (i > 0)
                        pt_linebreak(p, pt_line_for(p, ast_node_pos(s)), 1, PT_IGNORE,
                                     pt_lines_from(p, line) > 0);
                    pt_record_line(p, &line);
                    pt_value_spec(p, (AstValueSpec *)s, keep_type[i]);
                }
            } else {
                for (Int i = 0; i < n; i++) {
                    AstSpec s = PT_AT(d->specs, AstSpec, i);
                    if (i > 0)
                        pt_linebreak(p, pt_line_for(p, ast_node_pos(s)), 1, PT_IGNORE,
                                     pt_lines_from(p, line) > 0);
                    pt_record_line(p, &line);
                    pt_spec(p, s, n, false);
                }
            }
            pt_forget_line(p, &line);
            pt_ws2(p, PT_UNINDENT, PT_FORMFEED);
        }
        pt_set_pos(p, d->rparen);
        pt_tok(p, TOKEN_RPAREN);
    } else if (d->specs.len > 0) {
        /* a single declaration */
        pt_spec(p, PT_AT(d->specs, AstSpec, 0), 1, true);
    }
}

/* sizeCounter: an IoWriter that counts the bytes written and notes whether a
 * newline went by. */
typedef struct PtSizeCounter {
    bool has_newline;
    Int size;
} PtSizeCounter;

static Int pt_size_counter_write(void *self, Slice b, Error *err) {
    PtSizeCounter *c = (PtSizeCounter *)self;
    const Byte *q = (const Byte *)b.p;
    if (!c->has_newline) {
        for (Int i = 0; i < b.len; i++) {
            if (q[i] == '\n' || q[i] == '\f') {
                c->has_newline = true;
                break;
            }
        }
    }
    c->size += b.len;
    *err = BURROW_NO_ERROR;
    return b.len;
}

static const Type pt_size_counter_desc = {
    {(const Byte *)"sizeCounter", 11},
    {(const Byte *)"go/printer", 10},
    KIND_STRUCT,
    (uint32_t)sizeof(PtSizeCounter),
    (uint16_t)_Alignof(PtSizeCounter),
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

static const IoWriterVT pt_size_counter_vt = {&pt_size_counter_desc,
                                              pt_size_counter_write};

/* What printNode prints: a node, a []ast.Stmt or a []ast.Decl, and the
 * comments of a CommentedNode if it came in one. */
enum { PT_TARGET_NODE, PT_TARGET_STMTS, PT_TARGET_DECLS };

typedef struct PtTarget {
    int what;
    AstNode node; /* PT_TARGET_NODE */
    Slice list;   /* PT_TARGET_STMTS and PT_TARGET_DECLS */
    bool commented;
    Slice comments; /* the CommentedNode's, if commented */
} PtTarget;

static Error pt_fprint(const PrinterConfig *cfg, Arena *ar, Map *node_sizes,
                       IoWriter output, TokenFileSet *fset, const PtTarget *t);

/* nodeSize: the width of n once formatted. It is at most max_size if n fits
 * on one line of at most max_size bytes with no control characters in it,
 * and more than max_size otherwise. */
static Int pt_node_size(PtPrinter *p, AstNode n, Int max_size) {
    /* nodeSize runs the printer, which can run nodeSize again. Deep nests of
     * composite literals could take exponential time, so earlier results
     * are kept to cut that short (issue 1628). */
    uintptr_t key = (uintptr_t)(void *)n;
    Int *known = BURROW_MAP_GET(uintptr_t, Int, p->node_sizes, key);
    if (known != NULL)
        return *known;

    Int size = max_size + 1; /* take it that n does not fit */
    if (!BURROW_MAP_SET(uintptr_t, Int, p->node_sizes, key, size))
        pt_oom();

    /* The size must not depend on the style, so the same choice comes out
     * every time: print in raw format. The nested printer works in the same
     * arena and gives back all it took before this returns. */
    PrinterConfig cfg = {PRINTER_RAW_FORMAT, 0, 0};
    PtSizeCounter counter = {false, 0};
    IoWriter w = {&pt_size_counter_vt, &counter};
    PtTarget t = {PT_TARGET_NODE, n, {NULL, 0, 0, NULL}, false, {NULL, 0, 0, NULL}};
    ArenaMark mark = arena_mark(p->ar);
    Error err = pt_fprint(&cfg, p->ar, p->node_sizes, w, p->fset, &t);
    arena_release(p->ar, mark);
    if (BURROW_FAILED(err))
        return size;
    if (counter.size <= max_size && !counter.has_newline) {
        /* n fits on one line */
        size = counter.size;
        if (!BURROW_MAP_SET(uintptr_t, Int, p->node_sizes, key, size))
            pt_oom();
    }
    return size;
}

/* numLines: the number of source lines n takes. */
static Int pt_num_lines(PtPrinter *p, AstNode n) {
    TokenPos from = ast_node_pos(n);
    if (token_pos_is_valid(from)) {
        TokenPos to = ast_node_end(n);
        if (token_pos_is_valid(to))
            return pt_line_for(p, to) - pt_line_for(p, from) + 1;
    }
    return PT_INFINITY;
}

/* bodySize: nodeSize for a block. */
static Int pt_body_size(PtPrinter *p, AstBlockStmt *b, Int max_size) {
    TokenPos pos1 = ast_node_pos(PT_NODE(b));
    TokenPos pos2 = b->rbrace;
    if (token_pos_is_valid(pos1) && token_pos_is_valid(pos2) &&
        pt_line_for(p, pos1) != pt_line_for(p, pos2)) {
        /* the braces are on different lines: not a one-liner */
        return max_size + 1;
    }
    if (b->list.len > 5) {
        /* too many statements for a one-liner */
        return max_size + 1;
    }
    /* otherwise an estimate of the size */
    Int body_size = pt_comment_size_before(p, pt_pos_for(p, pos2));
    for (Int i = 0; i < b->list.len; i++) {
        if (body_size > max_size)
            break; /* no need to go on */
        if (i > 0)
            body_size += 2; /* a semicolon and a blank */
        body_size += pt_node_size(p, PT_AT(b->list, AstStmt, i), max_size);
    }
    return body_size;
}

/* funcBody: a function body after a header of header_size. If the two are
 * small enough and the block simple enough, the block goes on the current
 * line with no line breaks, separated from the header by sep. Otherwise the
 * { goes on the current line, then the statements and the } on lines of
 * their own. */
static void pt_func_body(PtPrinter *p, Int header_size, PtWs sep, AstBlockStmt *b) {
    if (b == NULL)
        return;

    /* the composite literal level is put back at the end */
    Int level = p->level;
    p->level = 0;

    const Int max_size = 100;
    if (header_size + pt_body_size(p, b, max_size) <= max_size) {
        pt_ws(p, sep);
        pt_set_pos(p, b->lbrace);
        pt_tok(p, TOKEN_LBRACE);
        if (b->list.len > 0) {
            pt_ws(p, PT_BLANK);
            for (Int i = 0; i < b->list.len; i++) {
                if (i > 0)
                    pt_tok_ws(p, TOKEN_SEMICOLON, PT_BLANK);
                pt_stmt(p, PT_AT(b->list, AstStmt, i), i == b->list.len - 1);
            }
            pt_ws(p, PT_BLANK);
        }
        pt_mode(p, PT_NO_EXTRA_LINEBREAK);
        pt_set_pos(p, b->rbrace);
        pt_tok(p, TOKEN_RBRACE);
        pt_mode(p, PT_NO_EXTRA_LINEBREAK);
        p->level = level;
        return;
    }

    if (sep != PT_IGNORE)
        pt_ws(p, PT_BLANK); /* always a blank */
    pt_block(p, b, 1);
    p->level = level;
}

/* distanceFrom: the columns between the current output position and
 * start_out_col, or PT_INFINITY if start_pos is on another line or either
 * position is unknown. */
static Int pt_distance_from(PtPrinter *p, TokenPos start_pos, Int start_out_col) {
    if (token_pos_is_valid(start_pos) && pt_valid(p->pos) &&
        pt_pos_for(p, start_pos).line == p->pos.line)
        return p->out.column - start_out_col;
    return PT_INFINITY;
}

static void pt_func_decl(PtPrinter *p, AstFuncDecl *d) {
    pt_set_comment(p, d->doc);
    pt_set_pos_node(p, PT_NODE(d));
    pt_tok_ws(p, TOKEN_FUNC, PT_BLANK);
    /* The start column is taken after the func goes out, since the white
     * space before it only goes out with it and can move it to another
     * line. */
    Int start_col = p->out.column - 5; /* len("func ") */
    if (d->recv != NULL) {
        pt_parameters(p, d->recv, PT_FUNC_PARAM); /* a method's receiver */
        pt_ws(p, PT_BLANK);
    }
    pt_expr(p, PT_NODE(d->name));
    pt_signature(p, d->type);
    pt_func_body(p, pt_distance_from(p, ast_node_pos(PT_NODE(d)), start_col), PT_VTAB,
                 d->body);
}

static void pt_decl(PtPrinter *p, AstDecl decl) {
    switch ((int)PT_KIND(decl)) {
    case AST_KIND_BAD_DECL:
        pt_set_pos_node(p, decl);
        pt_text(p, S("BadDecl"));
        break;
    case AST_KIND_GEN_DECL:
        pt_gen_decl(p, (AstGenDecl *)decl);
        break;
    case AST_KIND_FUNC_DECL:
        pt_func_decl(p, (AstFuncDecl *)decl);
        break;
    default:
        panic_str(S("unreachable"));
    }
}

/* ------------------------------------------------------------------ files */

/* getDoc. */
static AstCommentGroup *pt_get_doc(AstNode n) {
    switch ((int)PT_KIND(n)) {
    case AST_KIND_FIELD:
        return ((AstField *)n)->doc;
    case AST_KIND_IMPORT_SPEC:
        return ((AstImportSpec *)n)->doc;
    case AST_KIND_VALUE_SPEC:
        return ((AstValueSpec *)n)->doc;
    case AST_KIND_TYPE_SPEC:
        return ((AstTypeSpec *)n)->doc;
    case AST_KIND_GEN_DECL:
        return ((AstGenDecl *)n)->doc;
    case AST_KIND_FUNC_DECL:
        return ((AstFuncDecl *)n)->doc;
    case AST_KIND_FILE:
        return ((AstFile *)n)->doc;
    default:
        return NULL;
    }
}

/* getLastComment. */
static AstCommentGroup *pt_get_last_comment(AstNode n) {
    switch ((int)PT_KIND(n)) {
    case AST_KIND_FIELD:
        return ((AstField *)n)->comment;
    case AST_KIND_IMPORT_SPEC:
        return ((AstImportSpec *)n)->comment;
    case AST_KIND_VALUE_SPEC:
        return ((AstValueSpec *)n)->comment;
    case AST_KIND_TYPE_SPEC:
        return ((AstTypeSpec *)n)->comment;
    case AST_KIND_GEN_DECL: {
        Slice specs = ((AstGenDecl *)n)->specs;
        if (specs.len > 0)
            return pt_get_last_comment(PT_AT(specs, AstSpec, specs.len - 1));
        return NULL;
    }
    case AST_KIND_FILE: {
        Slice comments = ((AstFile *)n)->comments;
        if (comments.len > 0)
            return PT_AT(comments, AstCommentGroup *, comments.len - 1);
        return NULL;
    }
    default:
        return NULL;
    }
}

static Token pt_decl_token(AstDecl decl) {
    switch ((int)PT_KIND(decl)) {
    case AST_KIND_GEN_DECL:
        return ((AstGenDecl *)decl)->tok;
    case AST_KIND_FUNC_DECL:
        return TOKEN_FUNC;
    default:
        return TOKEN_ILLEGAL;
    }
}

static void pt_decl_list(PtPrinter *p, Slice list) {
    Token tok = TOKEN_ILLEGAL;
    for (Int i = 0; i < list.len; i++) {
        AstDecl d = PT_AT(list, AstDecl, i);
        Token prev = tok;
        tok = pt_decl_token(d);
        /* An empty line between top level declarations if the token
         * changes (from const to type, say) or the next one has a doc
         * comment. (pt_linebreak gets d's position, which is after any doc
         * comment, so the minimum holds without the pt_get_doc check, but
         * it stays in case the line break logic gets better.) */
        if (p->output_len > 0) {
            /* a line break, unless this is the start of the output (when
             * printing part of a program) */
            Int min = 1;
            if (prev != tok || pt_get_doc(d) != NULL)
                min = 2;
            /* a new section if the next declaration is a function over
             * several lines (issue 19544) */
            pt_linebreak(p, pt_line_for(p, ast_node_pos(d)), min, PT_IGNORE,
                         tok == TOKEN_FUNC && pt_num_lines(p, d) > 1);
        }
        pt_decl(p, d);
    }
}

static void pt_file(PtPrinter *p, AstFile *src) {
    pt_set_comment(p, src->doc);
    pt_set_pos_node(p, PT_NODE(src));
    pt_tok_ws(p, TOKEN_PACKAGE, PT_BLANK);
    pt_expr(p, PT_NODE(src->name));
    pt_decl_list(p, src->decls);
    pt_ws(p, PT_NEWLINE);
}

/* ---------------------------------------------------------------- printer */

static void pt_init(PtPrinter *p, const PrinterConfig *cfg, Arena *ar,
                    TokenFileSet *fset, Map *node_sizes) {
    memset(p, 0, sizeof *p);
    p->cfg = *cfg;
    p->fset = fset;
    p->ar = ar;
    p->a = arena_allocator(ar);
    p->pos.line = 1;
    p->pos.column = 1;
    p->out.line = 1;
    p->out.column = 1;
    p->source_pos_err = BURROW_NO_ERROR;
    p->node_sizes = node_sizes;
    p->cached_pos = -1;
}

static bool pt_is_expr_kind(Int k) {
    return k >= AST_KIND_BAD_EXPR && k <= AST_KIND_CHAN_TYPE;
}

static bool pt_is_stmt_kind(Int k) {
    return k >= AST_KIND_BAD_STMT && k <= AST_KIND_RANGE_STMT;
}

static bool pt_is_spec_kind(Int k) {
    return k >= AST_KIND_IMPORT_SPEC && k <= AST_KIND_TYPE_SPEC;
}

static bool pt_is_decl_kind(Int k) {
    return k >= AST_KIND_BAD_DECL && k <= AST_KIND_FUNC_DECL;
}

/* printNode. The target was checked to be one the printer takes, so this
 * cannot fail on its type. */
static Error pt_print_node(PtPrinter *p, const PtTarget *t) {
    if (t->commented && t->comments.p != NULL) {
        /* the comments inside the node, and its doc comment */
        AstNode n = t->node;
        TokenPos beg = ast_node_pos(n);
        TokenPos end = ast_node_end(n);
        AstCommentGroup *doc = pt_get_doc(n);
        if (doc != NULL)
            beg = ast_comment_group_pos(doc);
        AstCommentGroup *com = pt_get_last_comment(n);
        if (com != NULL) {
            TokenPos e = ast_comment_group_end(com);
            if (e > end)
                end = e;
        }
        Slice comments = t->comments;
        Int i = 0;
        while (i < comments.len &&
               ast_comment_group_end(PT_AT(comments, AstCommentGroup *, i)) < beg)
            i++;
        Int j = i;
        while (j < comments.len &&
               ast_comment_group_pos(PT_AT(comments, AstCommentGroup *, j)) < end)
            j++;
        if (i < j) {
            p->comments = (Slice){(void *)&PT_AT(comments, AstCommentGroup *, i), j - i,
                                  j - i, comments.elem};
        }
    } else if (t->what == PT_TARGET_NODE && PT_KIND(t->node) == AST_KIND_FILE) {
        p->comments = ((AstFile *)t->node)->comments;
    }

    p->use_node_comments = p->comments.p == NULL;

    pt_next_comment(p);

    pt_mode(p, 0);

    if (t->what == PT_TARGET_STMTS) {
        for (Int i = 0; i < t->list.len; i++) {
            if (PT_KIND(PT_AT(t->list, AstStmt, i)) == AST_KIND_LABELED_STMT)
                p->indent = 1;
        }
        pt_stmt_list(p, t->list, 0, false);
    } else if (t->what == PT_TARGET_DECLS) {
        pt_decl_list(p, t->list);
    } else {
        AstNode n = t->node;
        Int k = PT_KIND(n);
        if (pt_is_expr_kind(k)) {
            pt_expr(p, n);
        } else if (pt_is_stmt_kind(k)) {
            if (k == AST_KIND_LABELED_STMT)
                p->indent = 1;
            pt_stmt(p, n, false);
        } else if (pt_is_decl_kind(k)) {
            pt_decl(p, n);
        } else if (pt_is_spec_kind(k)) {
            pt_spec(p, n, 1, false);
        } else {
            pt_file(p, (AstFile *)n);
        }
    }

    return p->source_pos_err;
}

/* trimmer: an IoWriter that drops the white space at the ends of lines,
 * turns \v into \t and \f into \n, and takes out the TABWRITER_ESCAPE bytes
 * around text that must not change. Tabwriter input must keep its trailing
 * tabs, which carry layout, so the trimming happens after it. The tabwriter
 * could trim too, but there is none in raw format. */
typedef struct PtTrimmer {
    IoWriter output;
    Alloc *a;
    int state;
    Byte *space;
    Int space_len, space_cap;
} PtTrimmer;

enum {
    PT_IN_SPACE,  /* in space */
    PT_IN_ESCAPE, /* in text between TABWRITER_ESCAPE bytes */
    PT_IN_TEXT    /* in text */
};

static void pt_trimmer_reset_space(PtTrimmer *t) {
    t->state = PT_IN_SPACE;
    t->space_len = 0;
}

static Int pt_trimmer_out(PtTrimmer *t, const Byte *b, Int n, Error *err) {
    Slice s = {(void *)(uintptr_t)b, n, n, TYPE_BYTE};
    return t->output.vt->write(t->output.data, s, err);
}

static Int pt_trimmer_write(void *self, Slice data, Error *err) {
    PtTrimmer *t = (PtTrimmer *)self;
    const Byte *d = (const Byte *)data.p;
    static const Byte a_newline[1] = {'\n'};
    *err = BURROW_NO_ERROR;
    Int m = 0;
    Int n = 0;
    for (n = 0; n < data.len; n++) {
        Byte b = d[n];
        if (b == '\v')
            b = '\t'; /* a horizontal tab */
        switch (t->state) {
        case PT_IN_SPACE:
            switch (b) {
            case '\t':
            case ' ':
                pt_bytes_grow(t->a, &t->space, &t->space_cap, t->space_len + 1);
                t->space[t->space_len++] = b;
                break;
            case '\n':
            case '\f':
                pt_trimmer_reset_space(t); /* drop the trailing space */
                pt_trimmer_out(t, a_newline, 1, err);
                break;
            case TABWRITER_ESCAPE:
                pt_trimmer_out(t, t->space, t->space_len, err);
                t->state = PT_IN_ESCAPE;
                m = n + 1; /* skip the escape */
                break;
            default:
                pt_trimmer_out(t, t->space, t->space_len, err);
                t->state = PT_IN_TEXT;
                m = n;
                break;
            }
            break;
        case PT_IN_ESCAPE:
            if (b == TABWRITER_ESCAPE) {
                pt_trimmer_out(t, d + m, n - m, err);
                pt_trimmer_reset_space(t);
            }
            break;
        case PT_IN_TEXT:
            switch (b) {
            case '\t':
            case ' ':
                pt_trimmer_out(t, d + m, n - m, err);
                pt_trimmer_reset_space(t);
                pt_bytes_grow(t->a, &t->space, &t->space_cap, t->space_len + 1);
                t->space[t->space_len++] = b;
                break;
            case '\n':
            case '\f':
                pt_trimmer_out(t, d + m, n - m, err);
                pt_trimmer_reset_space(t);
                if (BURROW_OK(*err))
                    pt_trimmer_out(t, a_newline, 1, err);
                break;
            case TABWRITER_ESCAPE:
                pt_trimmer_out(t, d + m, n - m, err);
                t->state = PT_IN_ESCAPE;
                m = n + 1; /* skip the escape */
                break;
            default:
                break;
            }
            break;
        default:
            panic_str(S("unreachable"));
        }
        if (BURROW_FAILED(*err))
            return n;
    }
    n = data.len;

    if (t->state == PT_IN_ESCAPE || t->state == PT_IN_TEXT) {
        pt_trimmer_out(t, d + m, n - m, err);
        pt_trimmer_reset_space(t);
    }

    return n;
}

static const Type pt_trimmer_desc = {
    {(const Byte *)"trimmer", 7},
    {(const Byte *)"go/printer", 10},
    KIND_STRUCT,
    (uint32_t)sizeof(PtTrimmer),
    (uint16_t)_Alignof(PtTrimmer),
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

static const IoWriterVT pt_trimmer_vt = {&pt_trimmer_desc, pt_trimmer_write};

/* Config.fprint. */
static Error pt_fprint(const PrinterConfig *cfg, Arena *ar, Map *node_sizes,
                       IoWriter output, TokenFileSet *fset, const PtTarget *t) {
    PtPrinter pr;
    PtPrinter *p = &pr;
    pt_init(p, cfg, ar, fset, node_sizes);
    Error err = pt_print_node(p, t);
    if (BURROW_FAILED(err))
        return err;
    /* the comments still to come */
    p->implied_semi = false; /* EOF works as a newline */
    TokenPosition eof = {BURROW_STR_EMPTY, PT_INFINITY, PT_INFINITY, 0};
    bool wrote_newline = false;
    bool dropped_ff = false;
    pt_flush(p, eof, TOKEN_EOF, &wrote_newline, &dropped_ff);

    /* the output is all in p->output now: fix the build lines */
    pt_fix_go_build_lines(p);

    /* through a trimmer, to drop the white space at the ends of lines */
    PtTrimmer trim;
    memset(&trim, 0, sizeof trim);
    trim.output = output;
    trim.a = p->a;
    IoWriter w = {&pt_trimmer_vt, &trim};

    /* and through a tabwriter unless the format is raw */
    TabwriterWriter tw;
    bool use_tw = (cfg->mode & PRINTER_RAW_FORMAT) == 0;
    if (use_tw) {
        Int minwidth = cfg->tabwidth;

        Byte padchar = '\t';
        if ((cfg->mode & PRINTER_USE_SPACES) != 0)
            padchar = ' ';

        Uint twmode = TABWRITER_DISCARD_EMPTY_COLUMNS;
        if ((cfg->mode & PRINTER_TAB_INDENT) != 0) {
            minwidth = 0;
            twmode |= TABWRITER_TAB_INDENT;
        }

        memset(&tw, 0, sizeof tw);
        tw.a = p->a;
        tabwriter_writer_init(&tw, w, minwidth, cfg->tabwidth, 1, padchar, twmode);
        w = tabwriter_writer_as_io_writer(&tw);
    }

    /* the result, through the tabwriter and trimmer, to output */
    Slice out = {(void *)p->output, p->output_len, p->output_len, TYPE_BYTE};
    w.vt->write(w.data, out, &err);
    if (BURROW_OK(err) && use_tw)
        err = tabwriter_writer_flush(&tw);
    if (use_tw)
        tabwriter_writer_free(&tw);
    return err;
}

/* Whether t is one of go/ast's interfaces, Node, Expr, Stmt, Decl or Spec. */
static bool pt_is_iface_type(const Type *t) {
    return t == &burrow_type_AstNode || t == &burrow_type_AstExpr ||
           t == &burrow_type_AstStmt || t == &burrow_type_AstDecl ||
           t == &burrow_type_AstSpec;
}

/* The node in v, classified for printNode. Returns false for a type the
 * printer does not take, with *bad set to the node if v held one. */
static bool pt_classify(Any v, PtTarget *t, AstNode *bad) {
    *bad = NULL;
    if (v.t == NULL || v.data == NULL)
        return false;
    if (v.t->kind == KIND_SLICE && v.t->elem == &burrow_type_AstStmt) {
        t->what = PT_TARGET_STMTS;
        t->list = *(const Slice *)v.data;
        return true;
    }
    if (v.t->kind == KIND_SLICE && v.t->elem == &burrow_type_AstDecl) {
        t->what = PT_TARGET_DECLS;
        t->list = *(const Slice *)v.data;
        return true;
    }
    bool ast_ptr = v.t->kind == KIND_POINTER &&
                   (str_eq(v.t->pkg_path, S("go/ast")) ||
                    (v.t->elem != NULL && str_eq(v.t->elem->pkg_path, S("go/ast"))));
    /* an ast.Object or ast.Scope is not a node and has no kind to look at */
    if (!ast_ptr || v.t == &burrow_type_AstObjectPtr || v.t == &burrow_type_AstScopePtr)
        return false;
    AstNode n = *(AstNode const *)v.data;
    if (n == NULL)
        return false; /* a nil node, or a nil interface */
    Int k = PT_KIND(n);
    if (pt_is_expr_kind(k) || pt_is_stmt_kind(k) || pt_is_decl_kind(k) ||
        pt_is_spec_kind(k) || k == AST_KIND_FILE) {
        t->what = PT_TARGET_NODE;
        t->node = n;
        return true;
    }
    *bad = n;
    return false;
}

Error printer_config_fprint(const PrinterConfig *cfg, Alloc *a, IoWriter output,
                            TokenFileSet *fset, Any node) {
    PtTarget t;
    memset(&t, 0, sizeof t);
    if (node.t == TYPE_PRINTER_COMMENTED_NODE_PTR && node.data != NULL) {
        const PrinterCommentedNode *cn = *(PrinterCommentedNode *const *)node.data;
        if (cn == NULL)
            panic_str(S("runtime error: invalid memory address or nil pointer "
                        "dereference"));
        t.commented = true;
        t.comments = cn->comments;
        node = cn->node;
    }
    AstNode bad = NULL;
    bool ok = pt_classify(node, &t, &bad);
    if (ok && t.commented && t.comments.p != NULL && t.what != PT_TARGET_NODE)
        ok = false; /* only a node can take comments */
    if (!ok) {
        if (bad != NULL) {
            const Type *bt = ast_node_type(bad);
            Str name = bt != NULL ? bt->name : S("Node");
            return fmt_errorf_v("go/printer: unsupported node type *ast.%s", name);
        }
        if (pt_is_iface_type(node.t) && node.data != NULL &&
            *(AstNode const *)node.data == NULL)
            node = (Any){NULL, NULL}; /* a nil interface holds no type */
        return fmt_errorf_v("go/printer: unsupported node type %T", node);
    }

    Arena ar;
    arena_init(&ar, a, 0);
    Map *node_sizes = map_make(a, TYPE_UINTPTR, TYPE_INT, 0);
    if (node_sizes == NULL)
        pt_oom();
    Error err = pt_fprint(cfg, &ar, node_sizes, output, fset, &t);
    map_free(node_sizes);
    arena_free(&ar);
    return err;
}

Error printer_fprint(Alloc *a, IoWriter output, TokenFileSet *fset, Any node) {
    PrinterConfig cfg = {0, 8, 0};
    return printer_config_fprint(&cfg, a, output, fset, node);
}

/* ------------------------------------------------------------------ types */

#define PT_STR(s) {(const Byte *)(s), (Int)sizeof(s) - 1}

const Type burrow_type_PrinterMode = {
    PT_STR("Mode"),
    PT_STR("go/printer"),
    KIND_UINT,
    (uint32_t)sizeof(PrinterMode),
    (uint16_t)_Alignof(PrinterMode),
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

static const Field pt_fields_config[] = {
    {PT_STR("Mode"),
     {NULL, 0},
     &burrow_type_PrinterMode,
     (uint32_t)offsetof(PrinterConfig, mode)},
    {PT_STR("Tabwidth"),
     {NULL, 0},
     &burrow_type_Int,
     (uint32_t)offsetof(PrinterConfig, tabwidth)},
    {PT_STR("Indent"),
     {NULL, 0},
     &burrow_type_Int,
     (uint32_t)offsetof(PrinterConfig, indent)},
};

const Type burrow_type_PrinterConfig = {
    PT_STR("Config"),
    PT_STR("go/printer"),
    KIND_STRUCT,
    (uint32_t)sizeof(PrinterConfig),
    (uint16_t)_Alignof(PrinterConfig),
    (uint16_t)(sizeof pt_fields_config / sizeof pt_fields_config[0]),
    0,
    pt_fields_config,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Type burrow_type_PrinterConfigPtr = {
    {NULL, 0},
    {NULL, 0},
    KIND_POINTER,
    (uint32_t)sizeof(void *),
    (uint16_t)_Alignof(void *),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_PrinterConfig,
    NULL,
    0,
    0,
    NULL,
};

static const Type pt_slice_comment_group = {
    {NULL, 0},
    {NULL, 0},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_AstCommentGroupPtr,
    NULL,
    0,
    0,
    NULL,
};

static const Field pt_fields_commented_node[] = {
    {PT_STR("Node"),
     {NULL, 0},
     &burrow_type_Any,
     (uint32_t)offsetof(PrinterCommentedNode, node)},
    {PT_STR("Comments"),
     {NULL, 0},
     &pt_slice_comment_group,
     (uint32_t)offsetof(PrinterCommentedNode, comments)},
};

const Type burrow_type_PrinterCommentedNode = {
    PT_STR("CommentedNode"),
    PT_STR("go/printer"),
    KIND_STRUCT,
    (uint32_t)sizeof(PrinterCommentedNode),
    (uint16_t)_Alignof(PrinterCommentedNode),
    (uint16_t)(sizeof pt_fields_commented_node / sizeof pt_fields_commented_node[0]),
    0,
    pt_fields_commented_node,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Type burrow_type_PrinterCommentedNodePtr = {
    {NULL, 0},
    {NULL, 0},
    KIND_POINTER,
    (uint32_t)sizeof(void *),
    (uint16_t)_Alignof(void *),
    0,
    0,
    NULL,
    NULL,
    &burrow_type_PrinterCommentedNode,
    NULL,
    0,
    0,
    NULL,
};
