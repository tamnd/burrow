/* go/scanner: the scanner for Go source and its error list.
 *
 * The scanner is Go's, line for line. Where Go builds a string for a literal,
 * this hands back a view of the source, since a Go string of a []byte range
 * holds the same bytes. The two places Go makes new text, a comment or raw
 * string with its carriage returns taken out and the replacement character
 * for an illegal byte, are the only ones that are not views.
 *
 * Error messages are formatted in the goroutine's error arena and released
 * as soon as the handler returns, which is why the handler's msg is only good
 * for the length of the call.
 *
 * Derived from Go's src/go/scanner/scanner.go and errors.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/go/scanner.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/go/token.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/path/filepath.h"
#include "burrow/slice.h"
#include "burrow/sort.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ errors */

static const Type gs_error_desc = {
    {(const Byte *)"Error", 5},
    {(const Byte *)"go/scanner", 10},
    KIND_STRUCT,
    (uint32_t)sizeof(GoScannerError),
    (uint16_t)_Alignof(GoScannerError),
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

static const Type gs_error_ptr_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_POINTER,
    (uint32_t)sizeof(void *),
    (uint16_t)_Alignof(void *),
    0,
    0,
    NULL,
    NULL,
    &gs_error_desc,
    NULL,
    0,
    0,
    NULL,
};

static const Type gs_error_list_desc = {
    {(const Byte *)"ErrorList", 9},
    {(const Byte *)"go/scanner", 10},
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    0,
    NULL,
    NULL,
    &gs_error_ptr_desc,
    NULL,
    0,
    0,
    NULL,
};

const Type *const TYPE_GO_SCANNER_ERROR_LIST = &gs_error_list_desc;

Str go_scanner_error_error(GoScannerError e, Alloc *a) {
    if (e.pos.filename.len > 0 || token_position_is_valid(&e.pos)) {
        /* don't print "<unknown position>" */
        return fmt_sprintf_v(a, "%s: %s", token_position_string(e.pos, a), e.msg);
    }
    return str_clone(a, e.msg);
}

void go_scanner_error_list_add(GoScannerErrorList *p, Alloc *a, TokenPosition pos,
                               Str msg) {
    GoScannerError *e = (GoScannerError *)mem_alloc(a, sizeof(GoScannerError),
                                                    _Alignof(GoScannerError));
    if (e == NULL)
        return;
    e->pos = pos;
    e->pos.filename = str_clone(a, pos.filename);
    e->msg = str_clone(a, msg);
    if (e->pos.filename.len != pos.filename.len || e->msg.len != msg.len)
        return;
    Slice cur = *p;
    if (cur.elem == NULL)
        cur.elem = &gs_error_ptr_desc;
    Slice next = slice_append(a, cur, &e, 1);
    if (next.len == cur.len + 1)
        *p = next;
}

GoScannerError *go_scanner_error_list_at(GoScannerErrorList p, Int i) {
    return BURROW_AT(GoScannerError *, p, i);
}

void go_scanner_error_list_reset(GoScannerErrorList *p) {
    p->len = 0;
}

Int go_scanner_error_list_len(GoScannerErrorList p) {
    return p.len;
}

void go_scanner_error_list_swap(GoScannerErrorList p, Int i, Int j) {
    GoScannerError **v = (GoScannerError **)p.p;
    GoScannerError *t = v[i];
    v[i] = v[j];
    v[j] = t;
}

bool go_scanner_error_list_less(GoScannerErrorList p, Int i, Int j) {
    GoScannerError *const *v = (GoScannerError *const *)p.p;
    const TokenPosition *e = &v[i]->pos;
    const TokenPosition *f = &v[j]->pos;
    /* Note that it is not sufficient to simply compare file offsets because
     * the offsets do not reflect modified line information (through //line
     * comments). */
    if (!str_eq(e->filename, f->filename))
        return str_cmp(e->filename, f->filename) < 0;
    if (e->line != f->line)
        return e->line < f->line;
    if (e->column != f->column)
        return e->column < f->column;
    return str_cmp(v[i]->msg, v[j]->msg) < 0;
}

static Int gs_sort_len(void *self) {
    return ((GoScannerErrorList *)self)->len;
}

static bool gs_sort_less(void *self, Int i, Int j) {
    return go_scanner_error_list_less(*(GoScannerErrorList *)self, i, j);
}

static void gs_sort_swap(void *self, Int i, Int j) {
    go_scanner_error_list_swap(*(GoScannerErrorList *)self, i, j);
}

static const SortInterfaceVT gs_sort_vt = {
    &gs_error_list_desc,
    gs_sort_len,
    gs_sort_less,
    gs_sort_swap,
};

void go_scanner_error_list_sort(GoScannerErrorList p) {
    sort_sort((SortInterface){&gs_sort_vt, &p});
}

void go_scanner_error_list_remove_multiples(GoScannerErrorList *p) {
    go_scanner_error_list_sort(*p);
    GoScannerError **v = (GoScannerError **)p->p;
    TokenPosition last = {{NULL, 0}, 0, 0, 0}; /* initial last.line is != any legal
                                                  error line */
    Int i = 0;
    for (Int k = 0; k < p->len; k++) {
        GoScannerError *e = v[k];
        if (!str_eq(e->pos.filename, last.filename) || e->pos.line != last.line) {
            last = e->pos;
            v[i] = e;
            i++;
        }
    }
    p->len = i;
}

Str go_scanner_error_list_error(GoScannerErrorList p, Alloc *a) {
    switch (p.len) {
    case 0:
        return str_clone(a, BURROW_S("no errors"));
    case 1:
        return go_scanner_error_error(*go_scanner_error_list_at(p, 0), a);
    default:
        break;
    }
    return fmt_sprintf_v(a, "%s (and %d more errors)",
                         go_scanner_error_error(*go_scanner_error_list_at(p, 0), a),
                         p.len - 1);
}

/* The Error that ErrorList.Err gives. The list comes first, so that the
 * pointer errors_as hands back is a GoScannerErrorList pointer. */
typedef struct GsListBox {
    GoScannerErrorList list;
    Str message;
} GsListBox;

static Str gs_list_message(const void *self) {
    return ((const GsListBox *)self)->message;
}

static Error gs_list_clone(const void *self, Alloc *a);

static const ErrorVT gs_list_vt = {
    &gs_error_list_desc, gs_list_message, NULL, NULL, NULL, NULL, gs_list_clone,
};

static Error gs_list_box(Alloc *a, GoScannerErrorList p) {
    GsListBox *b = (GsListBox *)mem_alloc(a, sizeof(GsListBox), _Alignof(GsListBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->list = p;
    b->message = go_scanner_error_list_error(p, a);
    return (Error){&gs_list_vt, b};
}

Error go_scanner_error_list_err(GoScannerErrorList p) {
    if (p.len == 0)
        return BURROW_NO_ERROR;
    return gs_list_box(error_allocator(), p);
}

/* What error_retain calls: the whole list, errors and text, copied into a. */
static Error gs_list_clone(const void *self, Alloc *a) {
    const GsListBox *b = (const GsListBox *)self;
    GoScannerErrorList copy = {NULL, 0, 0, &gs_error_ptr_desc};
    for (Int i = 0; i < b->list.len; i++) {
        const GoScannerError *e = go_scanner_error_list_at(b->list, i);
        go_scanner_error_list_add(&copy, a, e->pos, e->msg);
        if (copy.len != i + 1)
            return burrow_err_out_of_memory;
    }
    return gs_list_box(a, copy);
}

void go_scanner_print_error(IoWriter w, Error err) {
    if (err.vt == &gs_list_vt) {
        GoScannerErrorList list = ((const GsListBox *)err.data)->list;
        for (Int i = 0; i < list.len; i++) {
            ArenaMark m = error_mark();
            Str text = go_scanner_error_error(*go_scanner_error_list_at(list, i),
                                              error_allocator());
            fmt_fprintf_v(w, "%s\n", text);
            error_release(m);
        }
    } else if (BURROW_FAILED(err)) {
        fmt_fprintf_v(w, "%s\n", error_text(err));
    }
}

/* ----------------------------------------------------------------- scanner */

enum {
    GS_BOM = 0xFEFF, /* byte order mark, only permitted as very first character */
    GS_EOF = -1      /* end of file */
};

static const Byte *gs_src(const GoScanner *s) {
    return (const Byte *)s->src.p;
}

static Str gs_text(const GoScanner *s, Int from, Int to) {
    return str_from_bytes(gs_src(s) + from, to - from);
}

static void gs_error(GoScanner *s, Int offs, Str msg) {
    if (s->err.f != NULL)
        BURROW_CALLF(s->err,
                     token_file_position(s->file, token_file_pos(s->file, offs)), msg);
    s->error_count++;
}

/* errorf, with the message made in the error arena and dropped once the
 * handler has seen it. */
#define GS_ERRORF(s, offs, ...)                                                        \
    do {                                                                               \
        ArenaMark gs_m_ = error_mark();                                                \
        gs_error((s), (offs), fmt_sprintf_v(error_allocator(), __VA_ARGS__));          \
        error_release(gs_m_);                                                          \
    } while (0)

/* Read the next Unicode char into s->ch. s->ch < 0 means end-of-file.
 *
 * For optimization, there is some overlap between this function and
 * gs_scan_identifier. */
static void gs_next(GoScanner *s) {
    const Byte *src = gs_src(s);
    if (s->rd_offset < s->src.len) {
        s->offset = s->rd_offset;
        if (s->ch == '\n') {
            s->line_offset = s->offset;
            token_file_add_line(s->file, s->offset);
        }
        Rune r = (Rune)src[s->rd_offset];
        Int w = 1;
        if (r == 0) {
            gs_error(s, s->offset, BURROW_S("illegal character NUL"));
        } else if (r >= UTF8_RUNE_SELF) {
            /* not ASCII */
            r = utf8_decode_rune_in_string(gs_text(s, s->rd_offset, s->src.len), &w);
            if (r == UTF8_RUNE_ERROR && w == 1) {
                const Byte *in = src + s->rd_offset;
                Int nin = s->src.len - s->rd_offset;
                if (s->offset == 0 && nin >= 2 &&
                    ((in[0] == 0xFF && in[1] == 0xFE) ||
                     (in[0] == 0xFE && in[1] == 0xFF))) {
                    /* U+FEFF BOM at start of file, encoded as big- or
                     * little-endian UCS-2 (i.e. 2-byte UTF-16). Give specific
                     * error (go.dev/issue/71950). */
                    gs_error(s, s->offset,
                             BURROW_S("illegal UTF-8 encoding (got UTF-16)"));
                    s->rd_offset += nin; /* consume all input to avoid error cascade */
                } else {
                    gs_error(s, s->offset, BURROW_S("illegal UTF-8 encoding"));
                }
            } else if (r == GS_BOM && s->offset > 0) {
                gs_error(s, s->offset, BURROW_S("illegal byte order mark"));
            }
        }
        s->rd_offset += w;
        s->ch = r;
    } else {
        s->offset = s->src.len;
        if (s->ch == '\n') {
            s->line_offset = s->offset;
            token_file_add_line(s->file, s->offset);
        }
        s->ch = GS_EOF;
    }
}

/* peek returns the byte following the most recently read character without
 * advancing the scanner. If the scanner is at EOF, peek returns 0. */
static Byte gs_peek(const GoScanner *s) {
    if (s->rd_offset < s->src.len)
        return gs_src(s)[s->rd_offset];
    return 0;
}

void go_scanner_init(GoScanner *s, Alloc *a, TokenFile *file, Slice src,
                     GoScannerErrorHandler err, GoScannerMode mode) {
    /* Explicitly initialize all fields since a scanner may be reused. */
    if (token_file_size(file) != src.len) {
        panic_str(fmt_sprintf_v(error_allocator(),
                                "file size (%d) does not match src len (%d)",
                                token_file_size(file), src.len));
        return;
    }

    Str base;
    Str dir = filepath_split(token_file_name(file), &base);

    memset(s, 0, sizeof *s);
    s->a = a;
    s->file = file;
    s->dir = dir;
    s->src = src;
    s->err = err;
    s->mode = mode;
    s->ch = ' ';
    s->end_pos_valid = true;
    s->end_pos = TOKEN_NO_POS;

    gs_next(s);
    if (s->ch == GS_BOM)
        gs_next(s); /* ignore BOM at file beginning */
}

/* stripCR, in a. In a slash-star comment, a \r right before the closing
 * slash of a star-slash stays (see below). */
Str burrow__go_scanner_strip_cr(Alloc *a, Str b, bool comment);
Str burrow__go_scanner_strip_cr(Alloc *a, Str b, bool comment) {
    Byte *c = (Byte *)mem_alloc_nozero(a, b.len > 0 ? (size_t)b.len : 1, 1);
    if (c == NULL)
        return (Str){NULL, 0};
    Int i = 0;
    for (Int j = 0; j < b.len; j++) {
        Byte ch = b.p[j];
        /* In a slash-star comment, don't strip \r from *\r/ (incl. sequences
         * of \r from *\r\r...\r/) since the resulting star-slash would
         * terminate the comment too early unless the \r is immediately
         * following the opening slash-star in which case it's ok because the
         * comment is not closed yet (issue #11151). */
        if (ch != '\r' || (comment && i > 2 && c[i - 1] == '*' && j + 1 < b.len &&
                           b.p[j + 1] == '/')) {
            c[i] = ch;
            i++;
        }
    }
    return str_from_bytes(c, i);
}

/* trailingDigits: the index after the last ':' in text, the number after it,
 * and whether that parsed. An index of 0 means there is no ':'. */
static Int gs_trailing_digits(Str text, Int *n, bool *ok) {
    Int i = text.len - 1; /* look from right (Windows filenames may contain ':') */
    while (i >= 0 && text.p[i] != ':')
        i--;
    if (i < 0) {
        *n = 0;
        *ok = false;
        return 0; /* no ":" */
    }
    /* i >= 0 */
    Error err = BURROW_NO_ERROR;
    uint64_t v = strconv_parse_uint(str_from_bytes(text.p + i + 1, text.len - i - 1),
                                    10, 0, &err);
    *n = (Int)v;
    *ok = BURROW_OK(err);
    return i + 1;
}

/* updateLineInfo parses the incoming comment text at offset offs as a line
 * directive. If successful, it updates the line info table for the position
 * next per the line directive. */
static void gs_update_line_info(GoScanner *s, Int next, Int offs, Str text) {
    /* extract comment text */
    if (text.p[1] == '*')
        text.len -= 2;                               /* lop off trailing star-slash */
    text = str_from_bytes(text.p + 7, text.len - 7); /* lop off leading "//line " */
    offs += 7;

    Int n = 0;
    bool ok = false;
    Int i = gs_trailing_digits(text, &n, &ok);
    if (i == 0)
        return; /* ignore (not a line directive) */
    /* i > 0 */

    ArenaMark m = error_mark();
    Alloc *ea = error_allocator();
    if (!ok) {
        /* text has a suffix :xxx but xxx is not a number */
        gs_error(s, offs + i,
                 fmt_sprintf_v(ea, "invalid line number: %s",
                               str_from_bytes(text.p + i, text.len - i)));
        error_release(m);
        return;
    }

    /* Put a cap on the maximum size of line and column numbers. 30 bits allows
     * for some additional space before wrapping an int32. Keep this consistent
     * with cmd/compile/internal/syntax.PosMax. */
    const Int max_line_col = (Int)1 << 30;
    Int line = 0;
    Int col = 0;
    Int n2 = 0;
    bool ok2 = false;
    Int i2 = gs_trailing_digits(str_from_bytes(text.p, i - 1), &n2, &ok2);
    if (ok2) {
        /* //line filename:line:col */
        Int t = i;
        i = i2;
        i2 = t;
        line = n2;
        col = n;
        if (col == 0 || col > max_line_col) {
            gs_error(s, offs + i2,
                     fmt_sprintf_v(ea, "invalid column number: %s",
                                   str_from_bytes(text.p + i2, text.len - i2)));
            error_release(m);
            return;
        }
        text.len = i2 - 1; /* lop off ":col" */
    } else {
        /* //line filename:line */
        line = n;
    }

    if (line == 0 || line > max_line_col) {
        gs_error(s, offs + i,
                 fmt_sprintf_v(ea, "invalid line number: %s",
                               str_from_bytes(text.p + i, text.len - i)));
        error_release(m);
        return;
    }

    /* If we have a column (//line filename:line:col form), an empty filename
     * means to use the previous filename. */
    Str filename = str_from_bytes(text.p, i - 1); /* lop off ":line" */
    if (filename.len == 0 && ok2) {
        filename = token_file_position(s->file, token_file_pos(s->file, offs)).filename;
    } else if (filename.len > 0) {
        /* Put a relative filename in the current directory. This is for
         * compatibility with earlier releases. See issue 26671. */
        filename = filepath_clean(ea, filename);
        if (!filepath_is_abs(filename))
            filename = filepath_join_v(ea, 2, s->dir, filename);
    }

    token_file_add_line_column_info(s->file, next, filename, line, col);
    error_release(m);
}

/* scanComment returns the text of the comment and (if nonzero) the offset of
 * the first newline within it, which implies a slash-star comment. */
static Str gs_scan_comment(GoScanner *s, Int *nl) {
    /* initial '/' already consumed; s->ch == '/' || s->ch == '*' */
    Int offs = s->offset - 1; /* position of initial '/' */
    Int next = -1; /* position immediately following the comment; < 0 means invalid
                      comment */
    Int num_cr = 0;
    Int nl_offset = 0; /* offset of first newline within the comment */

    if (s->ch == '/') {
        /* //-style comment (the final '\n' is not considered part of the
         * comment) */
        gs_next(s);
        while (s->ch != '\n' && s->ch >= 0) {
            if (s->ch == '\r')
                num_cr++;
            gs_next(s);
        }
        /* if we are at '\n', the position following the comment is
         * afterwards */
        next = s->offset;
        if (s->ch == '\n')
            next++;
        goto exit;
    }

    /* slash-star comment */
    gs_next(s);
    while (s->ch >= 0) {
        Rune ch = s->ch;
        if (ch == '\r')
            num_cr++;
        else if (ch == '\n' && nl_offset == 0)
            nl_offset = s->offset;
        gs_next(s);
        if (ch == '*' && s->ch == '/') {
            gs_next(s);
            next = s->offset;
            goto exit;
        }
    }

    gs_error(s, offs, BURROW_S("comment not terminated"));

exit:;
    Str lit = gs_text(s, offs, s->offset);

    /* On Windows, a (//-comment) line may end in "\r\n". Remove the final '\r'
     * before analyzing the text for line directives (matching the compiler).
     * Remove any other '\r' afterwards (matching the pre-existing behavior of
     * the scanner). */
    if (num_cr > 0 && lit.len >= 2 && lit.p[1] == '/' && lit.p[lit.len - 1] == '\r') {
        lit.len--;
        num_cr--;
    }

    /* interpret line directives (//line directives must start at the beginning
     * of the current line) */
    if (next >= 0 /* implies valid comment */ &&
        (lit.p[1] == '*' || offs == s->line_offset) &&
        strings_has_prefix(str_from_bytes(lit.p + 2, lit.len - 2), BURROW_S("line ")))
        gs_update_line_info(s, next, offs, lit);

    if (num_cr > 0)
        lit = burrow__go_scanner_strip_cr(s->a, lit, lit.p[1] == '*');

    *nl = nl_offset;
    return lit;
}

static Rune gs_lower(Rune ch) {
    return ('a' - 'A') | ch; /* returns lower-case ch iff ch is ASCII letter */
}

static bool gs_is_decimal(Rune ch) {
    return '0' <= ch && ch <= '9';
}

static bool gs_is_hex(Rune ch) {
    return ('0' <= ch && ch <= '9') || ('a' <= gs_lower(ch) && gs_lower(ch) <= 'f');
}

static bool gs_is_letter(Rune ch) {
    return ('a' <= gs_lower(ch) && gs_lower(ch) <= 'z') || ch == '_' ||
           (ch >= UTF8_RUNE_SELF && unicode_is_letter(ch));
}

static bool gs_is_digit(Rune ch) {
    return gs_is_decimal(ch) || (ch >= UTF8_RUNE_SELF && unicode_is_digit(ch));
}

/* scanIdentifier reads the string of valid identifier characters at
 * s->offset. It must only be called when s->ch is known to be a valid
 * letter. */
static Str gs_scan_identifier(GoScanner *s) {
    Int offs = s->offset;
    const Byte *src = gs_src(s);

    /* Optimize for the common case of an ASCII identifier. In case we
     * encounter a non-ASCII character, fall back on the slower path of calling
     * into gs_next. */
    for (Int rd = s->rd_offset; rd < s->src.len; rd++) {
        Byte b = src[rd];
        if (('a' <= b && b <= 'z') || ('A' <= b && b <= 'Z') || b == '_' ||
            ('0' <= b && b <= '9'))
            continue;
        s->rd_offset = rd;
        if (0 < b && b < UTF8_RUNE_SELF) {
            /* Optimization: we've encountered an ASCII character that's not a
             * letter or number. Avoid the call into gs_next and corresponding
             * set up.
             *
             * Note that gs_next does some line accounting if s->ch is '\n', so
             * this shortcut is only possible because we know that the
             * preceding character is not '\n'. */
            s->ch = (Rune)b;
            s->offset = s->rd_offset;
            s->rd_offset++;
            return gs_text(s, offs, s->offset);
        }
        /* We know that the preceding character is valid for an identifier
         * because gs_scan_identifier is only called when s->ch is a letter, so
         * calling gs_next at s->rd_offset resets the scanner state. */
        gs_next(s);
        while (gs_is_letter(s->ch) || gs_is_digit(s->ch))
            gs_next(s);
        return gs_text(s, offs, s->offset);
    }
    s->offset = s->src.len;
    s->rd_offset = s->src.len;
    s->ch = GS_EOF;
    return gs_text(s, offs, s->offset);
}

static Int gs_digit_val(Rune ch) {
    if ('0' <= ch && ch <= '9')
        return ch - '0';
    if ('a' <= gs_lower(ch) && gs_lower(ch) <= 'f')
        return gs_lower(ch) - 'a' + 10;
    return 16; /* larger than any legal digit val */
}

/* digits accepts the sequence { digit | '_' }. If base <= 10, digits accepts
 * any decimal digit but records the offset (relative to the source start) of
 * a digit >= base in *invalid, if *invalid < 0. digits returns a bitset
 * describing whether the sequence contained digits (bit 0 is set), or
 * separators '_' (bit 1 is set). */
static int gs_digits(GoScanner *s, int base, Int *invalid) {
    int digsep = 0;
    if (base <= 10) {
        Rune max = (Rune)('0' + base);
        while (gs_is_decimal(s->ch) || s->ch == '_') {
            int ds = 1;
            if (s->ch == '_')
                ds = 2;
            else if (s->ch >= max && *invalid < 0)
                *invalid = s->offset; /* record invalid rune offset */
            digsep |= ds;
            gs_next(s);
        }
    } else {
        while (gs_is_hex(s->ch) || s->ch == '_') {
            int ds = 1;
            if (s->ch == '_')
                ds = 2;
            digsep |= ds;
            gs_next(s);
        }
    }
    return digsep;
}

static Str gs_litname(Rune prefix) {
    switch (prefix) {
    case 'x':
        return BURROW_S("hexadecimal literal");
    case 'o':
    case '0':
        return BURROW_S("octal literal");
    case 'b':
        return BURROW_S("binary literal");
    default:
        break;
    }
    return BURROW_S("decimal literal");
}

/* invalidSep returns the index of the first invalid separator in x, or -1. */
static Int gs_invalid_sep(Str x) {
    Rune x1 = ' '; /* prefix char, we only care if it's 'x' */
    Rune d = '.';  /* digit, one of '_', '0' (a digit), or '.' (anything else) */
    Int i = 0;

    /* a prefix counts as a digit */
    if (x.len >= 2 && x.p[0] == '0') {
        x1 = gs_lower((Rune)x.p[1]);
        if (x1 == 'x' || x1 == 'o' || x1 == 'b') {
            d = '0';
            i = 2;
        }
    }

    /* mantissa and exponent */
    for (; i < x.len; i++) {
        Rune p = d; /* previous digit */
        d = (Rune)x.p[i];
        if (d == '_') {
            if (p != '0')
                return i;
        } else if (gs_is_decimal(d) || (x1 == 'x' && gs_is_hex(d))) {
            d = '0';
        } else {
            if (p == '_')
                return i - 1;
            d = '.';
        }
    }
    if (d == '_')
        return x.len - 1;

    return -1;
}

static Token gs_scan_number(GoScanner *s, Str *lit) {
    Int offs = s->offset;
    Token tok = TOKEN_ILLEGAL;

    int base = 10;    /* number base */
    Rune prefix = 0;  /* one of 0 (decimal), '0' (0-octal), 'x', 'o', or 'b' */
    int digsep = 0;   /* bit 0: digit present, bit 1: '_' present */
    Int invalid = -1; /* index of invalid digit in literal, or < 0 */

    /* integer part */
    if (s->ch != '.') {
        tok = TOKEN_INT;
        if (s->ch == '0') {
            gs_next(s);
            switch (gs_lower(s->ch)) {
            case 'x':
                gs_next(s);
                base = 16;
                prefix = 'x';
                break;
            case 'o':
                gs_next(s);
                base = 8;
                prefix = 'o';
                break;
            case 'b':
                gs_next(s);
                base = 2;
                prefix = 'b';
                break;
            default:
                base = 8;
                prefix = '0';
                digsep = 1; /* leading 0 */
                break;
            }
        }
        digsep |= gs_digits(s, base, &invalid);
    }

    /* fractional part */
    if (s->ch == '.') {
        tok = TOKEN_FLOAT;
        if (prefix == 'o' || prefix == 'b')
            GS_ERRORF(s, s->offset, "invalid radix point in %s", gs_litname(prefix));
        gs_next(s);
        digsep |= gs_digits(s, base, &invalid);
    }

    if ((digsep & 1) == 0)
        GS_ERRORF(s, s->offset, "%s has no digits", gs_litname(prefix));

    /* exponent */
    Rune e = gs_lower(s->ch);
    if (e == 'e' || e == 'p') {
        if (e == 'e' && prefix != 0 && prefix != '0')
            GS_ERRORF(s, s->offset, "%q exponent requires decimal mantissa", s->ch);
        else if (e == 'p' && prefix != 'x')
            GS_ERRORF(s, s->offset, "%q exponent requires hexadecimal mantissa", s->ch);
        gs_next(s);
        tok = TOKEN_FLOAT;
        if (s->ch == '+' || s->ch == '-')
            gs_next(s);
        Int none = -1;
        int ds = gs_digits(s, 10, &none);
        digsep |= ds;
        if ((ds & 1) == 0)
            gs_error(s, s->offset, BURROW_S("exponent has no digits"));
    } else if (prefix == 'x' && tok == TOKEN_FLOAT) {
        gs_error(s, s->offset,
                 BURROW_S("hexadecimal mantissa requires a 'p' exponent"));
    }

    /* suffix 'i' */
    if (s->ch == 'i') {
        tok = TOKEN_IMAG;
        gs_next(s);
    }

    Str l = gs_text(s, offs, s->offset);
    if (tok == TOKEN_INT && invalid >= 0)
        GS_ERRORF(s, invalid, "invalid digit %q in %s", (Rune)l.p[invalid - offs],
                  gs_litname(prefix));
    if ((digsep & 2) != 0) {
        Int i = gs_invalid_sep(l);
        if (i >= 0)
            gs_error(s, offs + i, BURROW_S("'_' must separate successive digits"));
    }

    *lit = l;
    return tok;
}

/* scanEscape parses an escape sequence where quote is the accepted escaped
 * quote. In case of a syntax error, it stops at the offending character
 * (without consuming it) and returns false. Otherwise it returns true. */
static bool gs_scan_escape(GoScanner *s, Rune quote) {
    Int offs = s->offset;

    int n = 0;
    uint32_t base = 0;
    uint32_t max = 0;
    switch (s->ch) {
    case 'a':
    case 'b':
    case 'f':
    case 'n':
    case 'r':
    case 't':
    case 'v':
    case '\\':
        gs_next(s);
        return true;
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
        n = 3;
        base = 8;
        max = 255;
        break;
    case 'x':
        gs_next(s);
        n = 2;
        base = 16;
        max = 255;
        break;
    case 'u':
        gs_next(s);
        n = 4;
        base = 16;
        max = UNICODE_MAX_RUNE;
        break;
    case 'U':
        gs_next(s);
        n = 8;
        base = 16;
        max = UNICODE_MAX_RUNE;
        break;
    default:
        if (s->ch == quote) {
            gs_next(s);
            return true;
        }
        gs_error(s, offs,
                 s->ch < 0 ? BURROW_S("escape sequence not terminated")
                           : BURROW_S("unknown escape sequence"));
        return false;
    }

    uint32_t x = 0;
    while (n > 0) {
        uint32_t d = (uint32_t)gs_digit_val(s->ch);
        if (d >= base) {
            if (s->ch < 0)
                gs_error(s, s->offset, BURROW_S("escape sequence not terminated"));
            else
                GS_ERRORF(s, s->offset, "illegal character %#U in escape sequence",
                          s->ch);
            return false;
        }
        x = x * base + d;
        gs_next(s);
        n--;
    }

    if (x > max || (0xD800 <= x && x < 0xE000)) {
        gs_error(s, offs, BURROW_S("escape sequence is invalid Unicode code point"));
        return false;
    }

    return true;
}

static Str gs_scan_rune(GoScanner *s) {
    /* '\'' opening already consumed */
    Int offs = s->offset - 1;

    bool valid = true;
    Int n = 0;
    for (;;) {
        Rune ch = s->ch;
        if (ch == '\n' || ch < 0) {
            /* only report error if we don't have one already */
            if (valid) {
                gs_error(s, offs, BURROW_S("rune literal not terminated"));
                valid = false;
            }
            break;
        }
        gs_next(s);
        if (ch == '\'')
            break;
        n++;
        if (ch == '\\') {
            if (!gs_scan_escape(s, '\''))
                valid = false;
            /* continue to read to closing quote */
        }
    }

    if (valid && n != 1)
        gs_error(s, offs, BURROW_S("illegal rune literal"));

    return gs_text(s, offs, s->offset);
}

static Str gs_scan_string(GoScanner *s) {
    /* '"' opening already consumed */
    Int offs = s->offset - 1;

    for (;;) {
        Rune ch = s->ch;
        if (ch == '\n' || ch < 0) {
            gs_error(s, offs, BURROW_S("string literal not terminated"));
            break;
        }
        gs_next(s);
        if (ch == '"')
            break;
        if (ch == '\\')
            gs_scan_escape(s, '"');
    }

    return gs_text(s, offs, s->offset);
}

static Str gs_scan_raw_string(GoScanner *s) {
    /* '`' opening already consumed */
    Int offs = s->offset - 1;

    bool has_cr = false;
    for (;;) {
        Rune ch = s->ch;
        if (ch < 0) {
            gs_error(s, offs, BURROW_S("raw string literal not terminated"));
            break;
        }
        gs_next(s);
        if (ch == '`')
            break;
        if (ch == '\r')
            has_cr = true;
    }

    Str lit = gs_text(s, offs, s->offset);
    if (has_cr)
        lit = burrow__go_scanner_strip_cr(s->a, lit, false);
    return lit;
}

static void gs_skip_whitespace(GoScanner *s) {
    while (s->ch == ' ' || s->ch == '\t' || (s->ch == '\n' && !s->insert_semi) ||
           s->ch == '\r')
        gs_next(s);
}

/* Helper functions for scanning multi-byte tokens such as >> += >>= .
 * Different routines recognize different length tok_i based on matches of
 * ch_i. If a token ends in '=', the result is tok1 or tok3 respectively.
 * Otherwise, the result is tok0 if there was no other matching character, or
 * tok2 if the matching character was ch2. */

static Token gs_switch2(GoScanner *s, Token tok0, Token tok1) {
    if (s->ch == '=') {
        gs_next(s);
        return tok1;
    }
    return tok0;
}

static Token gs_switch3(GoScanner *s, Token tok0, Token tok1, Rune ch2, Token tok2) {
    if (s->ch == '=') {
        gs_next(s);
        return tok1;
    }
    if (s->ch == ch2) {
        gs_next(s);
        return tok2;
    }
    return tok0;
}

static Token gs_switch4(GoScanner *s, Token tok0, Token tok1, Rune ch2, Token tok2,
                        Token tok3) {
    if (s->ch == '=') {
        gs_next(s);
        return tok1;
    }
    if (s->ch == ch2) {
        gs_next(s);
        if (s->ch == '=') {
            gs_next(s);
            return tok3;
        }
        return tok2;
    }
    return tok0;
}

TokenPos go_scanner_end(const GoScanner *s) {
    /* Handles special case:
     *   - Makes sure we return TOKEN_NO_POS, even when go_scanner_init has
     *     consumed a BOM.
     *   - When the previous token was a synthetic TOKEN_SEMICOLON inside a
     *     multi-line comment, we make sure End returns its ending position
     *     (i.e. prevPos+len("\n")). */
    if (s->end_pos_valid)
        return s->end_pos;

    /* Normal case: the position of s->offset is the end of the token */
    return token_file_pos(s->file, s->offset);
}

static TokenPos gs_scan(GoScanner *s, Token *tokp, Str *litp) {
    TokenPos pos = TOKEN_NO_POS;
    Token tok = TOKEN_ILLEGAL;
    Str lit = {NULL, 0};

scan_again:
    s->end_pos_valid = false;
    if (token_pos_is_valid(s->nl_pos)) {
        /* Return artificial ';' token after slash-star comment containing
         * newline, at position of first newline. */
        pos = s->nl_pos;
        s->end_pos = pos + 1;
        s->end_pos_valid = true;
        s->nl_pos = TOKEN_NO_POS;
        *tokp = TOKEN_SEMICOLON;
        *litp = BURROW_S("\n");
        return pos;
    }

    gs_skip_whitespace(s);

    /* current token start */
    pos = token_file_pos(s->file, s->offset);

    /* determine token value */
    bool insert_semi = false;
    Rune ch = s->ch;
    if (gs_is_letter(ch)) {
        lit = gs_scan_identifier(s);
        if (lit.len > 1) {
            /* keywords are longer than one letter - avoid lookup otherwise */
            tok = token_lookup(lit);
            switch (tok) {
            case TOKEN_IDENT:
            case TOKEN_BREAK:
            case TOKEN_CONTINUE:
            case TOKEN_FALLTHROUGH:
            case TOKEN_RETURN:
                insert_semi = true;
                break;
            default:
                break;
            }
        } else {
            insert_semi = true;
            tok = TOKEN_IDENT;
        }
    } else if (gs_is_decimal(ch) || (ch == '.' && gs_is_decimal((Rune)gs_peek(s)))) {
        insert_semi = true;
        tok = gs_scan_number(s, &lit);
    } else {
        gs_next(s); /* always make progress */
        switch (ch) {
        case GS_EOF:
            if (s->insert_semi) {
                s->insert_semi = false; /* EOF consumed */
                *tokp = TOKEN_SEMICOLON;
                *litp = BURROW_S("\n");
                return pos;
            }
            tok = TOKEN_EOF;
            break;
        case '\n':
            /* we only reach here if s->insert_semi was set in the first place
             * and exited early from gs_skip_whitespace */
            s->insert_semi = false; /* newline consumed */
            *tokp = TOKEN_SEMICOLON;
            *litp = BURROW_S("\n");
            return pos;
        case '"':
            insert_semi = true;
            tok = TOKEN_STRING;
            lit = gs_scan_string(s);
            break;
        case '\'':
            insert_semi = true;
            tok = TOKEN_CHAR;
            lit = gs_scan_rune(s);
            break;
        case '`':
            insert_semi = true;
            tok = TOKEN_STRING;
            lit = gs_scan_raw_string(s);
            break;
        case ':':
            tok = gs_switch2(s, TOKEN_COLON, TOKEN_DEFINE);
            break;
        case '.':
            /* fractions starting with a '.' are handled by outer switch */
            tok = TOKEN_PERIOD;
            if (s->ch == '.' && gs_peek(s) == '.') {
                gs_next(s);
                gs_next(s); /* consume last '.' */
                tok = TOKEN_ELLIPSIS;
            }
            break;
        case ',':
            tok = TOKEN_COMMA;
            break;
        case ';':
            tok = TOKEN_SEMICOLON;
            lit = BURROW_S(";");
            break;
        case '(':
            tok = TOKEN_LPAREN;
            break;
        case ')':
            insert_semi = true;
            tok = TOKEN_RPAREN;
            break;
        case '[':
            tok = TOKEN_LBRACK;
            break;
        case ']':
            insert_semi = true;
            tok = TOKEN_RBRACK;
            break;
        case '{':
            tok = TOKEN_LBRACE;
            break;
        case '}':
            insert_semi = true;
            tok = TOKEN_RBRACE;
            break;
        case '+':
            tok = gs_switch3(s, TOKEN_ADD, TOKEN_ADD_ASSIGN, '+', TOKEN_INC);
            if (tok == TOKEN_INC)
                insert_semi = true;
            break;
        case '-':
            tok = gs_switch3(s, TOKEN_SUB, TOKEN_SUB_ASSIGN, '-', TOKEN_DEC);
            if (tok == TOKEN_DEC)
                insert_semi = true;
            break;
        case '*':
            tok = gs_switch2(s, TOKEN_MUL, TOKEN_MUL_ASSIGN);
            break;
        case '/':
            if (s->ch == '/' || s->ch == '*') {
                /* comment */
                Int nl_offset = 0;
                Str comment = gs_scan_comment(s, &nl_offset);
                if (s->insert_semi && nl_offset != 0) {
                    /* For a slash-star comment containing \n, return COMMENT
                     * then artificial SEMICOLON. */
                    s->nl_pos = token_file_pos(s->file, nl_offset);
                    s->insert_semi = false;
                } else {
                    insert_semi = s->insert_semi; /* preserve insert_semi info */
                }
                if ((s->mode & GO_SCANNER_SCAN_COMMENTS) == 0) {
                    /* skip comment */
                    goto scan_again;
                }
                tok = TOKEN_COMMENT;
                lit = comment;
            } else {
                /* division */
                tok = gs_switch2(s, TOKEN_QUO, TOKEN_QUO_ASSIGN);
            }
            break;
        case '%':
            tok = gs_switch2(s, TOKEN_REM, TOKEN_REM_ASSIGN);
            break;
        case '^':
            tok = gs_switch2(s, TOKEN_XOR, TOKEN_XOR_ASSIGN);
            break;
        case '<':
            if (s->ch == '-') {
                gs_next(s);
                tok = TOKEN_ARROW;
            } else {
                tok = gs_switch4(s, TOKEN_LSS, TOKEN_LEQ, '<', TOKEN_SHL,
                                 TOKEN_SHL_ASSIGN);
            }
            break;
        case '>':
            tok = gs_switch4(s, TOKEN_GTR, TOKEN_GEQ, '>', TOKEN_SHR, TOKEN_SHR_ASSIGN);
            break;
        case '=':
            tok = gs_switch2(s, TOKEN_ASSIGN, TOKEN_EQL);
            break;
        case '!':
            tok = gs_switch2(s, TOKEN_NOT, TOKEN_NEQ);
            break;
        case '&':
            if (s->ch == '^') {
                gs_next(s);
                tok = gs_switch2(s, TOKEN_AND_NOT, TOKEN_AND_NOT_ASSIGN);
            } else {
                tok = gs_switch3(s, TOKEN_AND, TOKEN_AND_ASSIGN, '&', TOKEN_LAND);
            }
            break;
        case '|':
            tok = gs_switch3(s, TOKEN_OR, TOKEN_OR_ASSIGN, '|', TOKEN_LOR);
            break;
        case '~':
            tok = TOKEN_TILDE;
            break;
        default:
            /* gs_next reports unexpected BOMs - don't repeat */
            if (ch != GS_BOM) {
                /* Report an informative error for U+201[CD] quotation marks,
                 * which are easily introduced via copy and paste. */
                if (ch == 0x201C || ch == 0x201D)
                    GS_ERRORF(s, token_file_offset(s->file, pos),
                              "curly quotation mark %q (use neutral %q)", ch,
                              (Rune)'"');
                else
                    GS_ERRORF(s, token_file_offset(s->file, pos),
                              "illegal character %#U", ch);
            }
            insert_semi = s->insert_semi; /* preserve insert_semi info */
            tok = TOKEN_ILLEGAL;
            /* string(ch): the character's bytes in the source, except for an
             * invalid one, which Go turns into U+FFFD. */
            if (ch == UTF8_RUNE_ERROR) {
                lit = BURROW_S("\xEF\xBF\xBD");
            } else {
                Int off = token_file_offset(s->file, pos);
                lit = gs_text(s, off, off + utf8_rune_len(ch));
            }
            break;
        }
    }
    if ((s->mode & BURROW__GO_SCANNER_DONT_INSERT_SEMIS) == 0)
        s->insert_semi = insert_semi;

    *tokp = tok;
    *litp = lit;
    return pos;
}

TokenPos go_scanner_scan(GoScanner *s, Token *tok, Str *lit) {
    Token t = TOKEN_ILLEGAL;
    Str l = {NULL, 0};
    TokenPos pos = gs_scan(s, &t, &l);
    if (tok != NULL)
        *tok = t;
    if (lit != NULL)
        *lit = l;
    return pos;
}
