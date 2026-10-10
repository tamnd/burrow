/* go/doc/comment: the comment, HTML, Markdown and text printers.
 *
 * Derived from Go's src/go/doc/comment/print.go, html.go, markdown.go and
 * text.go.
 * Go source: go1.27.1.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/doc/comment.h"

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define S(lit) BURROW_S(lit)

BURROW_NORETURN static void dcp_oom(void) {
    panic_str(S("go/doc/comment: out of memory"));
}

/* A growing byte buffer, Go's bytes.Buffer. */
typedef struct DcpBuf {
    Alloc *a;
    Byte *p;
    Int len;
    Int cap;
} DcpBuf;

static void dcp_write(DcpBuf *b, const void *p, Int n) {
    if (n <= 0)
        return;
    if (b->len + n > b->cap) {
        Int nc = b->cap == 0 ? 64 : b->cap;
        while (nc < b->len + n)
            nc *= 2;
        Byte *q = (Byte *)mem_realloc(b->a, b->p, (size_t)b->cap, (size_t)nc, 1);
        if (q == NULL)
            dcp_oom();
        b->p = q;
        b->cap = nc;
    }
    memcpy(b->p + b->len, p, (size_t)n);
    b->len += n;
}

static void dcp_str(DcpBuf *b, Str s) {
    dcp_write(b, s.p, s.len);
}

static void dcp_byte(DcpBuf *b, Byte c) {
    dcp_write(b, &c, 1);
}

static Str dcp_view(const DcpBuf *b) {
    return str_from_bytes(b->p, b->len);
}

static void dcp_release(DcpBuf *b) {
    if (b->p != NULL)
        mem_free(b->a, b->p, (size_t)b->cap, 1);
    b->p = NULL;
    b->len = 0;
    b->cap = 0;
}

/* The bytes of b as a []byte, which then belongs to the caller. */
static Slice dcp_take(DcpBuf *b) {
    if (b->len == 0) {
        dcp_release(b);
        return slice_nil(TYPE_BYTE);
    }
    return slice_from(b->p, b->len, b->cap, TYPE_BYTE);
}

/* The bytes of b as an exactly sized Str in b's allocator. b is freed. */
static Str dcp_take_str(DcpBuf *b) {
    Str s = BURROW_STR_EMPTY;
    if (b->len > 0) {
        s = str_clone(b->a, dcp_view(b));
        if (s.len != b->len)
            dcp_oom();
    }
    dcp_release(b);
    return s;
}

/* Go's %T of a node, for the blocks a printer does not know. */
static Str dcp_type_name(CommentBase *x) {
    if (x == NULL)
        return S("<nil>");
    switch ((int)x->kind) {
    case COMMENT_KIND_CODE:
        return S("*comment.Code");
    case COMMENT_KIND_HEADING:
        return S("*comment.Heading");
    case COMMENT_KIND_LIST:
        return S("*comment.List");
    case COMMENT_KIND_PARAGRAPH:
        return S("*comment.Paragraph");
    case COMMENT_KIND_PLAIN:
        return S("comment.Plain");
    case COMMENT_KIND_ITALIC:
        return S("comment.Italic");
    case COMMENT_KIND_LINK:
        return S("*comment.Link");
    case COMMENT_KIND_DOC_LINK:
        return S("*comment.DocLink");
    default:
        return S("<invalid>");
    }
}

static Str dcp_node_text(CommentBase *t) {
    return t->kind == COMMENT_KIND_PLAIN ? ((CommentPlain *)t)->text
                                         : ((CommentItalic *)t)->text;
}

/* The Text of a list item's block, which has to be a paragraph. */
static Slice dcp_item_text(CommentBlock blk) {
    if (blk == NULL || blk->kind != COMMENT_KIND_PARAGRAPH)
        panic_str(S("go/doc/comment: list item block is not a *comment.Paragraph"));
    return ((CommentParagraph *)blk)->text;
}

static CommentListItem *dcp_item(CommentList *l, Int i) {
    return BURROW_AT(CommentListItem *, l->items, i);
}

/* -------------------------------------------------------------- defaults */

/* oneLongLine writes x to out as one long line, with no wrapping. */
static void dcp_one_long_line(DcpBuf *out, Slice x) {
    for (Int i = 0; i < x.len; i++) {
        CommentText t = BURROW_AT(CommentText, x, i);
        if (t == NULL)
            continue;
        switch ((int)t->kind) {
        case COMMENT_KIND_PLAIN:
        case COMMENT_KIND_ITALIC:
            dcp_str(out, dcp_node_text(t));
            break;
        case COMMENT_KIND_LINK:
            dcp_one_long_line(out, ((CommentLink *)t)->text);
            break;
        case COMMENT_KIND_DOC_LINK:
            dcp_one_long_line(out, ((CommentDocLink *)t)->text);
            break;
        default:
            break;
        }
    }
}

static void dcp_default_url(DcpBuf *out, CommentDocLink *l, Str base_url) {
    if (l->import_path.len != 0) {
        Str slash = BURROW_STR_EMPTY;
        dcp_str(out, base_url);
        if (strings_has_suffix(base_url, S("/")))
            slash = S("/");
        else
            dcp_byte(out, '/');
        dcp_str(out, l->import_path);
        dcp_str(out, slash);
        if (l->name.len == 0)
            return;
        dcp_byte(out, '#');
        if (l->recv.len != 0) {
            dcp_str(out, l->recv);
            dcp_byte(out, '.');
        }
        dcp_str(out, l->name);
        return;
    }
    dcp_byte(out, '#');
    if (l->recv.len != 0) {
        dcp_str(out, l->recv);
        dcp_byte(out, '.');
    }
    dcp_str(out, l->name);
}

Str comment_doc_link_default_url(CommentDocLink *l, Alloc *a, Str base_url) {
    DcpBuf out = {a, NULL, 0, 0};
    dcp_default_url(&out, l, base_url);
    return dcp_take_str(&out);
}

static bool dcp_is_ident_ascii(Byte c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
           c == '_';
}

/* The heading's default ID goes to out, and tmp is scratch space. The hdr-
 * prefix is important to avoid DOM clobbering attacks. See
 * https://pkg.go.dev/github.com/google/safehtml#Identifier. */
static void dcp_default_id(DcpBuf *out, DcpBuf *tmp, CommentHeading *h) {
    tmp->len = 0;
    dcp_one_long_line(tmp, h->text);
    Str s = strings_trim_space(dcp_view(tmp));
    if (s.len == 0)
        return;
    dcp_str(out, S("hdr-"));
    for (Int i = 0; i < s.len;) {
        Int n = 1;
        Byte c = s.p[i];
        if (c < 0x80) {
            dcp_byte(out, dcp_is_ident_ascii(c) ? c : '_');
        } else {
            (void)utf8_decode_rune_in_string(str_from_bytes(s.p + i, s.len - i), &n);
            dcp_byte(out, '_');
        }
        i += n;
    }
}

Str comment_heading_default_id(CommentHeading *h, Alloc *a) {
    DcpBuf out = {a, NULL, 0, 0};
    DcpBuf tmp = {a, NULL, 0, 0};
    dcp_default_id(&out, &tmp, h);
    dcp_release(&tmp);
    return dcp_take_str(&out);
}

/* The state every printer shares: the Printer, with its zero value standing in
 * for NULL, and scratch buffers for heading IDs and doc link URLs. */
typedef struct DcpState {
    CommentPrinter *p;
    DcpBuf id;
    DcpBuf url;
    DcpBuf tmp;
} DcpState;

static void dcp_state_init(DcpState *st, CommentPrinter *p, CommentPrinter *zero,
                           Alloc *a) {
    st->p = p != NULL ? p : zero;
    st->id = (DcpBuf){a, NULL, 0, 0};
    st->url = (DcpBuf){a, NULL, 0, 0};
    st->tmp = (DcpBuf){a, NULL, 0, 0};
}

static void dcp_state_free(DcpState *st) {
    dcp_release(&st->id);
    dcp_release(&st->url);
    dcp_release(&st->tmp);
}

static Int dcp_heading_level(DcpState *st) {
    return st->p->heading_level <= 0 ? 3 : st->p->heading_level;
}

static Str dcp_heading_id(DcpState *st, CommentHeading *h) {
    if (!BURROW_FUNC_IS_NIL(st->p->heading_id))
        return BURROW_CALLF(st->p->heading_id, h);
    st->id.len = 0;
    dcp_default_id(&st->id, &st->tmp, h);
    return dcp_view(&st->id);
}

static Str dcp_doc_link_url(DcpState *st, CommentDocLink *link) {
    if (!BURROW_FUNC_IS_NIL(st->p->doc_link_url))
        return BURROW_CALLF(st->p->doc_link_url, link);
    st->url.len = 0;
    dcp_default_url(&st->url, link, st->p->doc_link_base_url);
    return dcp_view(&st->url);
}

/* blankBefore reports whether the block x requires a blank line before it.
 * All blocks do, except for Lists that return false from x.BlankBefore(). */
static bool dcp_blank_before(CommentBlock x) {
    if (x != NULL && x->kind == COMMENT_KIND_LIST)
        return comment_list_blank_before((CommentList *)x);
    return true;
}

/* Writes each line of a code block's text with prefix in front, leaving empty
 * lines empty, ending each with nl. */
static void dcp_code_lines(DcpBuf *out, Str text, Str prefix, void (*nl)(DcpBuf *out)) {
    while (text.len > 0) {
        bool found = false;
        Str rest = BURROW_STR_EMPTY;
        Str line = strings_cut(text, S("\n"), &rest, &found);
        if (line.len != 0) {
            dcp_str(out, prefix);
            dcp_str(out, line);
        }
        nl(out);
        text = rest;
    }
}

static void dcp_plain_nl(DcpBuf *out) {
    dcp_byte(out, '\n');
}

/* --------------------------------------------------------------- comment */

/* indent writes s to out, inserting indent after each newline. */
static void dcp_comment_indent(DcpBuf *out, Str indent, Str s) {
    while (s.len > 0) {
        bool ok = false;
        Str rest = BURROW_STR_EMPTY;
        Str line = strings_cut(s, S("\n"), &rest, &ok);
        dcp_str(out, line);
        if (ok) {
            dcp_byte(out, '\n');
            dcp_str(out, indent);
        }
        s = rest;
    }
}

/* text prints the text sequence x to out. */
static void dcp_comment_text(DcpBuf *out, Str indent, Slice x) {
    for (Int i = 0; i < x.len; i++) {
        CommentText t = BURROW_AT(CommentText, x, i);
        if (t == NULL)
            continue;
        switch ((int)t->kind) {
        case COMMENT_KIND_PLAIN:
        case COMMENT_KIND_ITALIC:
            dcp_comment_indent(out, indent, dcp_node_text(t));
            break;
        case COMMENT_KIND_LINK: {
            CommentLink *l = (CommentLink *)t;
            if (l->auto_) {
                dcp_comment_text(out, indent, l->text);
            } else {
                dcp_byte(out, '[');
                dcp_comment_text(out, indent, l->text);
                dcp_byte(out, ']');
            }
            break;
        }
        case COMMENT_KIND_DOC_LINK:
            dcp_byte(out, '[');
            dcp_comment_text(out, indent, ((CommentDocLink *)t)->text);
            dcp_byte(out, ']');
            break;
        default:
            break;
        }
    }
}

/* block prints the block x to out. */
static void dcp_comment_block(DcpBuf *out, CommentBlock x) {
    switch (x == NULL ? 0 : (int)x->kind) {
    case COMMENT_KIND_PARAGRAPH:
        dcp_comment_text(out, BURROW_STR_EMPTY, ((CommentParagraph *)x)->text);
        dcp_byte(out, '\n');
        break;
    case COMMENT_KIND_HEADING:
        dcp_str(out, S("# "));
        dcp_comment_text(out, BURROW_STR_EMPTY, ((CommentHeading *)x)->text);
        dcp_byte(out, '\n');
        break;
    case COMMENT_KIND_CODE:
        dcp_code_lines(out, ((CommentCode *)x)->text, S("\t"), dcp_plain_nl);
        break;
    case COMMENT_KIND_LIST: {
        CommentList *l = (CommentList *)x;
        bool loose = comment_list_blank_between(l);
        for (Int i = 0; i < l->items.len; i++) {
            CommentListItem *item = dcp_item(l, i);
            if (i > 0 && loose)
                dcp_byte(out, '\n');
            dcp_byte(out, ' ');
            if (item->number.len == 0) {
                dcp_str(out, S(" - "));
            } else {
                dcp_str(out, item->number);
                dcp_str(out, S(". "));
            }
            for (Int j = 0; j < item->content.len; j++) {
                if (j > 0)
                    dcp_str(out, S("\n    "));
                dcp_comment_text(
                    out, S("    "),
                    dcp_item_text(BURROW_AT(CommentBlock, item->content, j)));
                dcp_byte(out, '\n');
            }
        }
        break;
    }
    default:
        dcp_byte(out, '?');
        dcp_str(out, dcp_type_name(x));
        break;
    }
}

Slice comment_printer_comment(CommentPrinter *p, Alloc *a, CommentDoc *d) {
    (void)p;
    DcpBuf out = {a, NULL, 0, 0};
    for (Int i = 0; i < d->content.len; i++) {
        CommentBlock x = BURROW_AT(CommentBlock, d->content, i);
        if (i > 0 && dcp_blank_before(x))
            dcp_byte(&out, '\n');
        dcp_comment_block(&out, x);
    }

    /* Print all the used links first, then the unused ones. */
    for (int k = 0; k < 2; k++) {
        bool used = k == 0;
        bool first = true;
        for (Int i = 0; i < d->links.len; i++) {
            CommentLinkDef *def = BURROW_AT(CommentLinkDef *, d->links, i);
            if (def->used != used)
                continue;
            if (first) {
                dcp_byte(&out, '\n');
                first = false;
            }
            dcp_byte(&out, '[');
            dcp_str(&out, def->text);
            dcp_str(&out, S("]: "));
            dcp_str(&out, def->url);
            dcp_byte(&out, '\n');
        }
    }
    return dcp_take(&out);
}

/* ------------------------------------------------------------------ html */

typedef struct DcpHTML {
    DcpState st;
    bool tight;
} DcpHTML;

/* escape prints s to out as plain text, escaping < & " ' and > to avoid being
 * misinterpreted in larger HTML constructs. */
static void dcp_html_escape(DcpBuf *out, Str s) {
    Int start = 0;
    for (Int i = 0; i < s.len; i++) {
        Str rep = BURROW_STR_EMPTY;
        switch (s.p[i]) {
        case '<':
            rep = S("&lt;");
            break;
        case '&':
            rep = S("&amp;");
            break;
        case '"':
            rep = S("&quot;");
            break;
        case '\'':
            rep = S("&apos;");
            break;
        case '>':
            rep = S("&gt;");
            break;
        default:
            continue;
        }
        dcp_write(out, s.p + start, i - start);
        dcp_str(out, rep);
        start = i + 1;
    }
    dcp_write(out, s.p + start, s.len - start);
}

/* text prints the text sequence x to out. */
static void dcp_html_text(DcpHTML *hp, DcpBuf *out, Slice x) {
    for (Int i = 0; i < x.len; i++) {
        CommentText t = BURROW_AT(CommentText, x, i);
        if (t == NULL)
            continue;
        switch ((int)t->kind) {
        case COMMENT_KIND_PLAIN:
            dcp_html_escape(out, ((CommentPlain *)t)->text);
            break;
        case COMMENT_KIND_ITALIC:
            dcp_str(out, S("<i>"));
            dcp_html_escape(out, ((CommentItalic *)t)->text);
            dcp_str(out, S("</i>"));
            break;
        case COMMENT_KIND_LINK: {
            CommentLink *l = (CommentLink *)t;
            dcp_str(out, S("<a href=\""));
            dcp_html_escape(out, l->url);
            dcp_str(out, S("\">"));
            dcp_html_text(hp, out, l->text);
            dcp_str(out, S("</a>"));
            break;
        }
        case COMMENT_KIND_DOC_LINK: {
            CommentDocLink *l = (CommentDocLink *)t;
            Str url = dcp_doc_link_url(&hp->st, l);
            bool has = url.len != 0;
            if (has) {
                dcp_str(out, S("<a href=\""));
                dcp_html_escape(out, url);
                dcp_str(out, S("\">"));
            }
            dcp_html_text(hp, out, l->text);
            if (has)
                dcp_str(out, S("</a>"));
            break;
        }
        default:
            break;
        }
    }
}

/* inc increments the decimal string held in b, as Go's inc does to a string:
 * "1" becomes "2", "9" becomes "10" and so on. b has room for one more byte. */
static void dcp_inc(Byte *b, Int *n) {
    for (Int i = *n - 1; i >= 0; i--) {
        if (b[i] < '9') {
            b[i]++;
            return;
        }
        b[i] = '0';
    }
    memmove(b + 1, b, (size_t)*n);
    b[0] = '1';
    (*n)++;
}

static void dcp_html_block(DcpHTML *hp, DcpBuf *out, CommentBlock x) {
    switch (x == NULL ? 0 : (int)x->kind) {
    case COMMENT_KIND_PARAGRAPH:
        if (!hp->tight)
            dcp_str(out, S("<p>"));
        dcp_html_text(hp, out, ((CommentParagraph *)x)->text);
        dcp_byte(out, '\n');
        break;
    case COMMENT_KIND_HEADING: {
        CommentHeading *h = (CommentHeading *)x;
        Byte lv[24];
        Int nlv = 0;
        uint64_t level = (uint64_t)dcp_heading_level(&hp->st);
        Byte rev[24];
        Int nrev = 0;
        do {
            rev[nrev++] = (Byte)('0' + level % 10);
            level /= 10;
        } while (level > 0);
        while (nrev > 0)
            lv[nlv++] = rev[--nrev];
        dcp_str(out, S("<h"));
        dcp_write(out, lv, nlv);
        Str id = dcp_heading_id(&hp->st, h);
        if (id.len != 0) {
            dcp_str(out, S(" id=\""));
            dcp_html_escape(out, id);
            dcp_byte(out, '"');
        }
        dcp_byte(out, '>');
        dcp_html_text(hp, out, h->text);
        dcp_str(out, S("</h"));
        dcp_write(out, lv, nlv);
        dcp_str(out, S(">\n"));
        break;
    }
    case COMMENT_KIND_CODE:
        dcp_str(out, S("<pre>"));
        dcp_html_escape(out, ((CommentCode *)x)->text);
        dcp_str(out, S("</pre>\n"));
        break;
    case COMMENT_KIND_LIST: {
        CommentList *l = (CommentList *)x;
        if (l->items.len == 0)
            panic_str(S("runtime error: index out of range [0] with length 0"));
        Str kind = dcp_item(l, 0)->number.len == 0 ? S("ul>\n") : S("ol>\n");
        dcp_byte(out, '<');
        dcp_str(out, kind);
        /* next is the number the next item gets without a value attribute. It
         * never has more digits than the longest item number plus one. */
        Int most = 1;
        for (Int i = 0; i < l->items.len; i++) {
            if (dcp_item(l, i)->number.len > most)
                most = dcp_item(l, i)->number.len;
        }
        Byte small[32];
        Byte *next = small;
        if ((size_t)most + 1 > sizeof(small)) {
            next = (Byte *)mem_alloc(out->a, (size_t)most + 1, 1);
            if (next == NULL)
                dcp_oom();
        }
        next[0] = '1';
        Int nnext = 1;
        for (Int i = 0; i < l->items.len; i++) {
            CommentListItem *item = dcp_item(l, i);
            dcp_str(out, S("<li"));
            Str n = item->number;
            if (n.len != 0) {
                if (!str_eq(n, str_from_bytes(next, nnext))) {
                    dcp_str(out, S(" value=\""));
                    dcp_str(out, n);
                    dcp_byte(out, '"');
                    memcpy(next, n.p, (size_t)n.len);
                    nnext = n.len;
                }
                dcp_inc(next, &nnext);
            }
            dcp_byte(out, '>');
            hp->tight = !comment_list_blank_between(l);
            for (Int j = 0; j < item->content.len; j++)
                dcp_html_block(hp, out, BURROW_AT(CommentBlock, item->content, j));
            hp->tight = false;
        }
        if (next != small)
            mem_free(out->a, next, (size_t)most + 1, 1);
        dcp_str(out, S("</"));
        dcp_str(out, kind);
        break;
    }
    default:
        dcp_byte(out, '?');
        dcp_str(out, dcp_type_name(x));
        break;
    }
}

Slice comment_printer_html(CommentPrinter *p, Alloc *a, CommentDoc *d) {
    CommentPrinter zero = {0};
    DcpHTML hp = {0};
    dcp_state_init(&hp.st, p, &zero, a);
    DcpBuf out = {a, NULL, 0, 0};
    for (Int i = 0; i < d->content.len; i++)
        dcp_html_block(&hp, &out, BURROW_AT(CommentBlock, d->content, i));
    dcp_state_free(&hp.st);
    return dcp_take(&out);
}

/* -------------------------------------------------------------- markdown */

typedef struct DcpMD {
    DcpState st;
    Int heading_level;
    DcpBuf raw;
} DcpMD;

/* escape prints s to out as plain text, escaping special characters to avoid
 * being misinterpreted as Markdown markup sequences. */
static void dcp_md_escape(DcpBuf *out, Str s) {
    Int start = 0;
    for (Int i = 0; i < s.len; i++) {
        Byte c = s.p[i];
        switch (c) {
        case '\n':
            /* Turn all \n into spaces, for a few reasons:
             *   - Avoid introducing paragraph breaks accidentally.
             *   - Avoid the need to reindent after the newline.
             *   - Avoid problems with Markdown renderers treating
             *     every mid-paragraph newline as a <br>. */
            dcp_write(out, s.p + start, i - start);
            dcp_byte(out, ' ');
            start = i + 1;
            break;
        case '`':
        case '_':
        case '*':
        case '[':
        case '<':
        case '\\':
            /* Not all of these need to be escaped all the time, but it is
             * cleaner to escape them always. */
            dcp_write(out, s.p + start, i - start);
            dcp_byte(out, '\\');
            dcp_byte(out, c);
            start = i + 1;
            break;
        default:
            break;
        }
    }
    dcp_write(out, s.p + start, s.len - start);
}

/* rawText prints the text sequence x to out, without worrying about line
 * breaks. */
static void dcp_md_raw_text(DcpMD *mp, DcpBuf *out, Slice x) {
    for (Int i = 0; i < x.len; i++) {
        CommentText t = BURROW_AT(CommentText, x, i);
        if (t == NULL)
            continue;
        switch ((int)t->kind) {
        case COMMENT_KIND_PLAIN:
            dcp_md_escape(out, ((CommentPlain *)t)->text);
            break;
        case COMMENT_KIND_ITALIC:
            dcp_byte(out, '*');
            dcp_md_escape(out, ((CommentItalic *)t)->text);
            dcp_byte(out, '*');
            break;
        case COMMENT_KIND_LINK: {
            CommentLink *l = (CommentLink *)t;
            dcp_byte(out, '[');
            dcp_md_raw_text(mp, out, l->text);
            dcp_str(out, S("]("));
            dcp_str(out, l->url);
            dcp_byte(out, ')');
            break;
        }
        case COMMENT_KIND_DOC_LINK: {
            CommentDocLink *l = (CommentDocLink *)t;
            Str url = dcp_doc_link_url(&mp->st, l);
            bool has = url.len != 0;
            if (has)
                dcp_byte(out, '[');
            dcp_md_raw_text(mp, out, l->text);
            if (has) {
                dcp_str(out, S("]("));
                for (Int j = 0; j < url.len; j++) {
                    if (url.p[j] == '(')
                        dcp_str(out, S("%28"));
                    else if (url.p[j] == ')')
                        dcp_str(out, S("%29"));
                    else
                        dcp_byte(out, url.p[j]);
                }
                dcp_byte(out, ')');
            }
            break;
        }
        default:
            break;
        }
    }
}

/* text prints the text sequence x to out. */
static void dcp_md_text(DcpMD *mp, DcpBuf *out, Slice x) {
    mp->raw.len = 0;
    dcp_md_raw_text(mp, &mp->raw, x);
    Str line = strings_trim_space(dcp_view(&mp->raw));
    if (line.len == 0)
        return;
    switch (line.p[0]) {
    case '+':
    case '-':
    case '*':
    case '#':
        /* Escape what would be the start of an unordered list or heading. */
        dcp_byte(out, '\\');
        break;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9': {
        Int i = 1;
        while (i < line.len && '0' <= line.p[i] && line.p[i] <= '9')
            i++;
        if (i < line.len && (line.p[i] == '.' || line.p[i] == ')')) {
            /* Escape what would be the start of an ordered list. */
            dcp_write(out, line.p, i);
            dcp_byte(out, '\\');
            line = str_from_bytes(line.p + i, line.len - i);
        }
        break;
    }
    default:
        break;
    }
    dcp_str(out, line);
}

static void dcp_md_block(DcpMD *mp, DcpBuf *out, CommentBlock x) {
    switch (x == NULL ? 0 : (int)x->kind) {
    case COMMENT_KIND_PARAGRAPH:
        /* TODO in Go: Is there a better way to know whether
         * the paragraph is in a list? */
        dcp_md_text(mp, out, ((CommentParagraph *)x)->text);
        dcp_byte(out, '\n');
        break;
    case COMMENT_KIND_HEADING: {
        CommentHeading *h = (CommentHeading *)x;
        for (Int i = 0; i < mp->heading_level; i++)
            dcp_byte(out, '#');
        dcp_byte(out, ' ');
        dcp_md_text(mp, out, h->text);
        Str id = dcp_heading_id(&mp->st, h);
        if (id.len != 0) {
            dcp_str(out, S(" {#"));
            dcp_str(out, id);
            dcp_byte(out, '}');
        }
        dcp_byte(out, '\n');
        break;
    }
    case COMMENT_KIND_CODE:
        dcp_code_lines(out, ((CommentCode *)x)->text, S("\t"), dcp_plain_nl);
        break;
    case COMMENT_KIND_LIST: {
        CommentList *l = (CommentList *)x;
        bool loose = comment_list_blank_between(l);
        for (Int i = 0; i < l->items.len; i++) {
            CommentListItem *item = dcp_item(l, i);
            if (i > 0 && loose)
                dcp_byte(out, '\n');
            if (item->number.len != 0) {
                dcp_byte(out, ' ');
                dcp_str(out, item->number);
                dcp_str(out, S(". "));
            } else {
                dcp_str(out, S("  - ")); /* SP SP - SP */
            }
            for (Int j = 0; j < item->content.len; j++) {
                if (j > 0)
                    dcp_str(out, S("\n    "));
                dcp_md_text(mp, out,
                            dcp_item_text(BURROW_AT(CommentBlock, item->content, j)));
                dcp_byte(out, '\n');
            }
        }
        break;
    }
    default:
        dcp_byte(out, '?');
        dcp_str(out, dcp_type_name(x));
        break;
    }
}

Slice comment_printer_markdown(CommentPrinter *p, Alloc *a, CommentDoc *d) {
    CommentPrinter zero = {0};
    DcpMD mp = {0};
    dcp_state_init(&mp.st, p, &zero, a);
    mp.heading_level = dcp_heading_level(&mp.st);
    mp.raw = (DcpBuf){a, NULL, 0, 0};
    DcpBuf out = {a, NULL, 0, 0};
    for (Int i = 0; i < d->content.len; i++) {
        if (i > 0)
            dcp_byte(&out, '\n');
        dcp_md_block(&mp, &out, BURROW_AT(CommentBlock, d->content, i));
    }
    dcp_release(&mp.raw);
    dcp_state_free(&mp.st);
    return dcp_take(&out);
}

/* ------------------------------------------------------------------ wrap */

/* A score is the score (also called weight) for a given line. add and cmp add
 * and compare scores. */
typedef struct DcpScore {
    int64_t hi;
    int64_t lo;
} DcpScore;

static DcpScore dcp_score_add(DcpScore s, DcpScore t) {
    return (DcpScore){s.hi + t.hi, s.lo + t.lo};
}

static int dcp_score_cmp(DcpScore s, DcpScore t) {
    if (s.hi < t.hi)
        return -1;
    if (s.hi > t.hi)
        return +1;
    if (s.lo < t.lo)
        return -1;
    if (s.lo > t.lo)
        return +1;
    return 0;
}

/* wrapPenalty is the penalty for inserting a line break after word s. */
static int64_t dcp_wrap_penalty(Str s) {
    switch (s.p[s.len - 1]) {
    case '.':
    case ',':
    case ':':
    case ';':
        return 0;
    default:
        return 64;
    }
}

int64_t burrow__comment_wrap_penalty(Str s);
int64_t burrow__comment_wrap_penalty(Str s) {
    return dcp_wrap_penalty(s);
}

typedef struct DcpWrap {
    const Str *words;
    Int n;
    Int max;
    Int *total;
    DcpScore *f;
} DcpWrap;

/* weight is the weight of a line holding words[i:j]. */
static DcpScore dcp_weight(const DcpWrap *w, Int i, Int j) {
    Int n = w->total[j] - 1 - w->total[i];
    if (j == w->n && n <= w->max)
        return (DcpScore){0, 0};
    int64_t p = dcp_wrap_penalty(w->words[j - 1]);
    int64_t v = (int64_t)(w->max - n) * (int64_t)(w->max - n);
    if (n > w->max)
        return (DcpScore){v, p};
    return (DcpScore){0, v + p};
}

/* g is the weight of the best breaks ending at i, plus a line i to j. */
static DcpScore dcp_g(const DcpWrap *w, Int i, Int j) {
    return dcp_score_add(w->f[i], dcp_weight(w, i, j));
}

/* bridge reports whether b can be dropped from the list of candidates a, b, c
 * because one of a or c is always better. */
static bool dcp_bridge(const DcpWrap *w, Int a, Int b, Int c) {
    /* sort.Search over k in [0, n+1-c) for the first g(a, k+c) > g(b, k+c). */
    Int lo = 0, hi = w->n + 1 - c;
    while (lo < hi) {
        Int h = lo + (hi - lo) / 2;
        if (!(dcp_score_cmp(dcp_g(w, a, h + c), dcp_g(w, b, h + c)) > 0))
            lo = h + 1;
        else
            hi = h;
    }
    Int k = c + lo;
    if (k > w->n)
        return true;
    return dcp_score_cmp(dcp_g(w, c, k), dcp_g(w, b, k)) <= 0;
}

static void *dcp_scratch(Alloc *a, Int n, size_t size, size_t align) {
    void *p = mem_alloc(a, (size_t)n * size, align);
    if (p == NULL)
        dcp_oom();
    return p;
}

/* wrap wraps words into lines of at most max runes, minimizing the sum of the
 * squares of the leftover lengths at the end of each line (except the last,
 * of course), with a penalty for ending a line with a non-punctuation word.
 * The result is the indexes seq[0] = 0 < seq[1] < ... < seq[k] = len(words)
 * of the line starts, so line i holds words[seq[i]:seq[i+1]]. It is stored in
 * an array of nseq Ints from a.
 *
 * This is the algorithm of Hirschberg and Larmore, "The Least Weight
 * Subsequence Problem", with the two-word fix Go's text.go explains. */
static Int *dcp_wrap(Alloc *a, const Str *words, Int nwords, Int max, Int *nseq) {
    Int nt = nwords + 1;
    Int nb = nwords + 2;
    DcpWrap w = {words, nwords, max, NULL, NULL};
    w.total = (Int *)dcp_scratch(a, nt, sizeof(Int), _Alignof(Int));
    w.f = (DcpScore *)dcp_scratch(a, nt, sizeof(DcpScore), _Alignof(DcpScore));
    Int *dq = (Int *)dcp_scratch(a, nt, sizeof(Int), _Alignof(Int));
    Int *bestleft = (Int *)dcp_scratch(a, nb, sizeof(Int), _Alignof(Int));

    w.total[0] = 0;
    for (Int i = 0; i < nwords; i++)
        w.total[1 + i] = w.total[i] + utf8_rune_count_in_string(words[i]) + 1;

    w.f[0] = (DcpScore){0, 0};
    Int nf = 1;
    /* d is a deque, dq[d0:d1]. */
    Int d0 = 0, d1 = 1;
    dq[0] = 0;
    bestleft[0] = -1;
    Int nbest = 1;
    for (Int m = 1; m < nwords; m++) {
        w.f[nf++] = dcp_g(&w, dq[d0], m);
        bestleft[nbest++] = dq[d0];
        while (d1 - d0 > 1 && dcp_score_cmp(dcp_g(&w, dq[d0 + 1], m + 1),
                                            dcp_g(&w, dq[d0], m + 1)) <= 0)
            d0++; /* Retire */
        while (d1 - d0 > 1 && dcp_bridge(&w, dq[d1 - 2], dq[d1 - 1], m))
            d1--; /* Fire */
        if (dcp_score_cmp(dcp_g(&w, m, nwords), dcp_g(&w, dq[d1 - 1], nwords)) < 0) {
            dq[d1++] = m; /* Hire */
            /* To handle the two-word case where both have the same score. */
            if (d1 - d0 == 2 && dcp_score_cmp(dcp_g(&w, dq[d0 + 1], m + 1),
                                              dcp_g(&w, dq[d0], m + 1)) <= 0)
                d0++;
        }
    }
    bestleft[nbest++] = dq[d0];

    Int n = 1;
    for (Int m = nwords; m > 0; m = bestleft[m])
        n++;
    Int *seq = (Int *)dcp_scratch(a, n, sizeof(Int), _Alignof(Int));
    *nseq = n;
    seq[0] = 0;
    for (Int m = nwords; m > 0; m = bestleft[m])
        seq[--n] = m;

    mem_free(a, bestleft, (size_t)nb * sizeof(Int), _Alignof(Int));
    mem_free(a, dq, (size_t)nt * sizeof(Int), _Alignof(Int));
    mem_free(a, w.f, (size_t)nt * sizeof(DcpScore), _Alignof(DcpScore));
    mem_free(a, w.total, (size_t)nt * sizeof(Int), _Alignof(Int));
    return seq;
}

Slice burrow__comment_wrap(Alloc *a, Slice words, Int max);
Slice burrow__comment_wrap(Alloc *a, Slice words, Int max) {
    Int n = 0;
    Int *seq = dcp_wrap(a, (const Str *)words.p, words.len, max, &n);
    return slice_from(seq, n, n, TYPE_INT);
}

/* ------------------------------------------------------------------ text */

typedef struct DcpText {
    DcpState st;
    DcpBuf long_;
    Str prefix;
    Str code_prefix;
    Int width;
} DcpText;

/* writeNL calls out.WriteByte('\n') but first trims trailing spaces and tabs
 * on the last line of out. */
static void dcp_write_nl(DcpBuf *out) {
    while (out->len > 0 &&
           (out->p[out->len - 1] == ' ' || out->p[out->len - 1] == '\t'))
        out->len--;
    dcp_byte(out, '\n');
}

/* text prints the text sequence x to out, wrapping it at tp.width and putting
 * prefix and indent at the start of each line after the first. */
static void dcp_text_text(DcpText *tp, DcpBuf *out, Str indent, Slice x) {
    Alloc *a = out->a;
    tp->long_.len = 0;
    dcp_one_long_line(&tp->long_, x);
    Str long_line = dcp_view(&tp->long_);
    Slice words = strings_fields(a, long_line);
    if (words.len == 0 && strings_trim_space(long_line).len != 0)
        dcp_oom();
    const Str *ws = (const Str *)words.p;

    Int one[2] = {0, words.len};
    Int *seq = NULL;
    Int nseq = 2;
    if (tp->width < 0 || words.len == 0) /* one long line */
        seq = one;
    else
        seq = dcp_wrap(a, ws, words.len, tp->width - utf8_rune_count_in_string(indent),
                       &nseq);
    for (Int i = 0; i + 1 < nseq; i++) {
        if (i > 0) {
            dcp_str(out, tp->prefix);
            dcp_str(out, indent);
        }
        for (Int j = seq[i]; j < seq[i + 1]; j++) {
            if (j > seq[i])
                dcp_byte(out, ' ');
            dcp_str(out, ws[j]);
        }
        dcp_write_nl(out);
    }
    if (seq != one)
        mem_free(a, seq, (size_t)nseq * sizeof(Int), _Alignof(Int));
    if (words.p != NULL)
        mem_free(a, words.p, (size_t)words.cap * sizeof(Str), _Alignof(Str));
}

static void dcp_text_block(DcpText *tp, DcpBuf *out, CommentBlock x) {
    switch (x == NULL ? 0 : (int)x->kind) {
    case COMMENT_KIND_PARAGRAPH:
        dcp_str(out, tp->prefix);
        dcp_text_text(tp, out, BURROW_STR_EMPTY, ((CommentParagraph *)x)->text);
        break;
    case COMMENT_KIND_HEADING:
        dcp_str(out, tp->prefix);
        dcp_str(out, S("# "));
        dcp_text_text(tp, out, BURROW_STR_EMPTY, ((CommentHeading *)x)->text);
        break;
    case COMMENT_KIND_CODE:
        dcp_code_lines(out, ((CommentCode *)x)->text, tp->code_prefix, dcp_write_nl);
        break;
    case COMMENT_KIND_LIST: {
        CommentList *l = (CommentList *)x;
        bool loose = comment_list_blank_between(l);
        for (Int i = 0; i < l->items.len; i++) {
            CommentListItem *item = dcp_item(l, i);
            if (i > 0 && loose) {
                dcp_str(out, tp->prefix);
                dcp_write_nl(out);
            }
            dcp_str(out, tp->prefix);
            dcp_byte(out, ' ');
            if (item->number.len == 0) {
                dcp_str(out, S(" - "));
            } else {
                dcp_str(out, item->number);
                dcp_str(out, S(". "));
            }
            for (Int j = 0; j < item->content.len; j++) {
                if (j > 0) {
                    dcp_write_nl(out);
                    dcp_str(out, tp->prefix);
                    dcp_str(out, S("    "));
                }
                dcp_text_text(tp, out, S("    "),
                              dcp_item_text(BURROW_AT(CommentBlock, item->content, j)));
            }
        }
        break;
    }
    default:
        dcp_byte(out, '?');
        dcp_str(out, dcp_type_name(x));
        dcp_byte(out, '\n');
        break;
    }
}

Slice comment_printer_text(CommentPrinter *p, Alloc *a, CommentDoc *d) {
    CommentPrinter zero = {0};
    DcpText tp = {0};
    dcp_state_init(&tp.st, p, &zero, a);
    p = tp.st.p;
    tp.long_ = (DcpBuf){a, NULL, 0, 0};
    tp.prefix = p->text_prefix;
    tp.code_prefix = p->text_code_prefix;
    tp.width = p->text_width;
    DcpBuf code_prefix = {a, NULL, 0, 0};
    if (tp.code_prefix.len == 0) {
        dcp_str(&code_prefix, p->text_prefix);
        dcp_byte(&code_prefix, '\t');
        tp.code_prefix = dcp_view(&code_prefix);
    }
    if (tp.width == 0)
        tp.width = 80 - utf8_rune_count_in_string(tp.prefix);

    DcpBuf out = {a, NULL, 0, 0};
    for (Int i = 0; i < d->content.len; i++) {
        CommentBlock x = BURROW_AT(CommentBlock, d->content, i);
        if (i > 0 && dcp_blank_before(x)) {
            dcp_str(&out, tp.prefix);
            dcp_write_nl(&out);
        }
        dcp_text_block(&tp, &out, x);
    }

    bool any_used = false;
    for (Int i = 0; i < d->links.len; i++) {
        if (BURROW_AT(CommentLinkDef *, d->links, i)->used) {
            any_used = true;
            break;
        }
    }
    if (any_used) {
        dcp_write_nl(&out);
        for (Int i = 0; i < d->links.len; i++) {
            CommentLinkDef *def = BURROW_AT(CommentLinkDef *, d->links, i);
            if (!def->used)
                continue;
            dcp_byte(&out, '[');
            dcp_str(&out, def->text);
            dcp_str(&out, S("]: "));
            dcp_str(&out, def->url);
            dcp_byte(&out, '\n');
        }
    }
    dcp_release(&code_prefix);
    dcp_release(&tp.long_);
    dcp_state_free(&tp.st);
    return dcp_take(&out);
}
