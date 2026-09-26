/* Derived from Go's src/text/tabwriter/tabwriter.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/text/tabwriter.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <string.h>

static const Type tabwriter_writer_desc = {
    {(const Byte *)"Writer", 6},
    {(const Byte *)"text/tabwriter", 14},
    KIND_STRUCT,
    (uint32_t)sizeof(TabwriterWriter),
    (uint16_t)_Alignof(TabwriterWriter),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x74627772U, /* "tbwr" */
    NULL,
};

const Type *const TYPE_TABWRITER_WRITER = &tabwriter_writer_desc;

/* Go gets out of a failed write, however deep in format it is, by panicking
 * with an osError and recovering it in Write or Flush. This does the same:
 * the error goes in b->err and the panic carries this text, which only shows
 * if something gets between the two, and nothing should. */
static const Str tw_os_error = {(const Byte *)"tabwriter: output error", 23};

BURROW_NORETURN static void tw_fail(TabwriterWriter *b, Error e) {
    b->err = e;
    panic_str(tw_os_error);
}

/* ------------------------------------------------------------------ memory */

static void *tw_grow(TabwriterWriter *b, void *p, Int *cap, Int want, size_t elem,
                     size_t align) {
    Int n = *cap * 2;
    if (n < want)
        n = want;
    if (n < 8)
        n = 8;
    void *q = mem_realloc(b->a, p, (size_t)*cap * elem, (size_t)n * elem, align);
    if (q == NULL)
        tw_fail(b, burrow_err_out_of_memory);
    *cap = n;
    return q;
}

static void tw_append_bytes(TabwriterWriter *b, const Byte *p, Int n) {
    if (n == 0)
        return;
    if (b->buf_len + n > b->buf_cap)
        b->buf = (Byte *)tw_grow(b, b->buf, &b->buf_cap, b->buf_len + n, 1, 1);
    memcpy(b->buf + b->buf_len, p, (size_t)n);
    b->buf_len += n;
}

static void tw_line_reserve(TabwriterWriter *b, TabwriterLine *l, Int want) {
    if (want > l->cap)
        l->cells = (TabwriterCell *)tw_grow(
            b, l->cells, &l->cap, want, sizeof(TabwriterCell), _Alignof(TabwriterCell));
}

static void tw_push_width(TabwriterWriter *b, Int w) {
    if (b->widths_len == b->widths_cap)
        b->widths = (Int *)tw_grow(b, b->widths, &b->widths_cap, b->widths_len + 1,
                                   sizeof(Int), _Alignof(Int));
    b->widths[b->widths_len++] = w;
}

/* ------------------------------------------------------------------ the text */

static void tw_add_line(TabwriterWriter *b, bool flushed) {
    /* Grow slice instead of appending, as that gives us an opportunity to
     * re-use an existing []cell. */
    if (b->lines_len == b->lines_cap) {
        Int old = b->lines_cap;
        b->lines =
            (TabwriterLine *)tw_grow(b, b->lines, &b->lines_cap, old + 1,
                                     sizeof(TabwriterLine), _Alignof(TabwriterLine));
        memset(b->lines + old, 0, (size_t)(b->lines_cap - old) * sizeof(TabwriterLine));
    }
    Int n = ++b->lines_len;
    b->lines[n - 1].len = 0;

    if (!flushed) {
        /* The previous line is probably a good indicator of how many cells the
         * current line will have. If the current line's capacity is smaller
         * than that, abandon it and make a new one. */
        if (n >= 2)
            tw_line_reserve(b, &b->lines[n - 1], b->lines[n - 2].len);
    }
}

/* Reset the current state. */
static void tw_reset(TabwriterWriter *b) {
    b->buf_len = 0;
    b->pos = 0;
    b->cell = (TabwriterCell){0, 0, false};
    b->end_char = 0;
    b->lines_len = 0;
    b->widths_len = 0;
    tw_add_line(b, true);
}

/* p + i, except that it leaves a NULL p alone, since the buffer is NULL until
 * the first byte arrives and adding even zero to NULL is undefined. */
static const Byte *tw_at(const Byte *p, Int i) {
    return p == NULL ? p : p + i;
}

static void tw_write0(TabwriterWriter *b, const Byte *p, Int n) {
    Error e = BURROW_NO_ERROR;
    Int m = b->output.vt->write(b->output.data,
                                (Slice){(void *)(uintptr_t)p, n, n, TYPE_BYTE}, &e);
    if (m != n && BURROW_OK(e))
        e = io_err_short_write;
    if (BURROW_FAILED(e))
        tw_fail(b, e);
}

static void tw_write_n(TabwriterWriter *b, const Byte *src, Int len, Int n) {
    while (n > len) {
        tw_write0(b, src, len);
        n -= len;
    }
    tw_write0(b, src, n);
}

static const Byte tw_newline[1] = {'\n'};
static const Byte tw_tabs[8] = {'\t', '\t', '\t', '\t', '\t', '\t', '\t', '\t'};
static const Byte tw_vbar[1] = {'|'};
static const Byte tw_hbar[4] = {'-', '-', '-', '\n'};

static void tw_write_padding(TabwriterWriter *b, Int textw, Int cellw, bool use_tabs) {
    if (b->padbytes[0] == '\t' || use_tabs) {
        /* Padding is done with tabs. */
        if (b->tabwidth == 0)
            return; /* tabs have no width, so no padding can be done */
        /* Make cellw the smallest multiple of b->tabwidth. */
        cellw = (cellw + b->tabwidth - 1) / b->tabwidth * b->tabwidth;
        Int n = cellw - textw; /* amount of padding */
        if (n < 0)
            runtime_panic(BURROW_S("internal error"));
        tw_write_n(b, tw_tabs, 8, (n + b->tabwidth - 1) / b->tabwidth);
        return;
    }

    /* Padding is done with non-tab characters. */
    tw_write_n(b, b->padbytes, 8, cellw - textw);
}

static Int tw_write_lines(TabwriterWriter *b, Int pos, Int line0, Int line1) {
    for (Int i = line0; i < line1; i++) {
        TabwriterLine *line = &b->lines[i];

        /* If TabIndent is set, use tabs to pad leading empty cells. */
        bool use_tabs = (b->flags & TABWRITER_TAB_INDENT) != 0;

        for (Int j = 0; j < line->len; j++) {
            TabwriterCell c = line->cells[j];
            if (j > 0 && (b->flags & TABWRITER_DEBUG) != 0) {
                /* Indicate column break. */
                tw_write0(b, tw_vbar, 1);
            }

            if (c.size == 0) {
                /* Empty cell. */
                if (j < b->widths_len)
                    tw_write_padding(b, c.width, b->widths[j], use_tabs);
            } else {
                /* Non-empty cell. */
                use_tabs = false;
                if ((b->flags & TABWRITER_ALIGN_RIGHT) == 0) { /* align left */
                    tw_write0(b, tw_at(b->buf, pos), c.size);
                    pos += c.size;
                    if (j < b->widths_len)
                        tw_write_padding(b, c.width, b->widths[j], false);
                } else { /* align right */
                    if (j < b->widths_len)
                        tw_write_padding(b, c.width, b->widths[j], false);
                    tw_write0(b, tw_at(b->buf, pos), c.size);
                    pos += c.size;
                }
            }
        }

        if (i + 1 == b->lines_len) {
            /* Last buffered line: we don't have a newline, so just write any
             * outstanding buffered data. */
            tw_write0(b, tw_at(b->buf, pos), b->cell.size);
            pos += b->cell.size;
        } else {
            /* Not the last line: write newline. */
            tw_write0(b, tw_newline, 1);
        }
    }
    return pos;
}

/* Format the text between line0 and line1 (excluding line1). pos is the
 * buffer position corresponding to the beginning of line0. Returns the buffer
 * position corresponding to the beginning of line1. */
static Int tw_format(TabwriterWriter *b, Int pos, Int line0, Int line1) {
    Int column = b->widths_len;
    for (Int cur = line0; cur < line1; cur++) {
        TabwriterLine *line = &b->lines[cur];

        if (column >= line->len - 1)
            continue;
        /* Cell exists in this column => this line has more cells than the
         * previous line (the last cell per line is ignored because cells are
         * tab-terminated; the last cell per line describes the text before
         * the newline/formfeed and does not belong to a column). */

        /* Print unprinted lines until beginning of block. */
        pos = tw_write_lines(b, pos, line0, cur);
        line0 = cur;

        /* Column block begin. */
        Int width = b->minwidth; /* minimal column width */
        bool discardable = true; /* all cells in this column empty and "soft" */
        for (; cur < line1; cur++) {
            line = &b->lines[cur];
            if (column >= line->len - 1)
                break;
            /* Cell exists in this column. */
            TabwriterCell c = line->cells[column];
            /* Update width. */
            Int w = c.width + b->padding;
            if (w > width)
                width = w;
            /* Update discardable. */
            if (c.width > 0 || c.htab)
                discardable = false;
        }
        /* Column block end. */

        /* Discard empty columns if necessary. */
        if (discardable && (b->flags & TABWRITER_DISCARD_EMPTY_COLUMNS) != 0)
            width = 0;

        /* Format and print all columns to the right of this column (we know
         * the widths of this column and all columns to the left). */
        tw_push_width(b, width);
        pos = tw_format(b, pos, line0, cur);
        b->widths_len--;
        line0 = cur;
    }

    /* Print unprinted lines until end. */
    return tw_write_lines(b, pos, line0, line1);
}

/* Append text to the current cell. */
static void tw_append(TabwriterWriter *b, const Byte *p, Int n) {
    tw_append_bytes(b, p, n);
    b->cell.size += n;
}

/* Update the cell width. Text is mostly ASCII, so that is counted here and
 * utf8_rune_count only sees the rest. */
static void tw_update_width(TabwriterWriter *b) {
    const Byte *p = tw_at(b->buf, b->pos);
    Int n = b->buf_len - b->pos;
    Int i = 0;
    while (i < n && p[i] < 0x80)
        i++;
    b->cell.width += i;
    if (i < n)
        b->cell.width += utf8_rune_count(
            slice_from((void *)(uintptr_t)(p + i), n - i, n - i, TYPE_BYTE));
    b->pos = b->buf_len;
}

/* Start escaped mode. */
static void tw_start_escape(TabwriterWriter *b, Byte ch) {
    switch (ch) {
    case TABWRITER_ESCAPE:
        b->end_char = TABWRITER_ESCAPE;
        break;
    case '<':
        b->end_char = '>';
        break;
    case '&':
        b->end_char = ';';
        break;
    default:
        break;
    }
}

/* Terminate escaped mode. If the escaped text was an HTML tag, its width is
 * assumed to be zero for formatting purposes; if it was an HTML entity, its
 * width is assumed to be one. In all other cases, the width is the unicode
 * width of the text. */
static void tw_end_escape(TabwriterWriter *b) {
    switch (b->end_char) {
    case TABWRITER_ESCAPE:
        tw_update_width(b);
        if ((b->flags & TABWRITER_STRIP_ESCAPE) == 0)
            b->cell.width -= 2; /* don't count the Escape chars */
        break;
    case '>': /* tag of zero width */
        break;
    case ';':
        b->cell.width++; /* entity, count as one rune */
        break;
    default:
        break;
    }
    b->pos = b->buf_len;
    b->end_char = 0;
}

/* Terminate the current cell by adding it to the list of cells of the current
 * line. Returns the number of cells in that line. */
static Int tw_terminate_cell(TabwriterWriter *b, bool htab) {
    b->cell.htab = htab;
    TabwriterLine *line = &b->lines[b->lines_len - 1];
    tw_line_reserve(b, line, line->len + 1);
    line->cells[line->len++] = b->cell;
    b->cell = (TabwriterCell){0, 0, false};
    return line->len;
}

static void tw_flush_no_defers(TabwriterWriter *b) {
    /* Add current cell if not empty. */
    if (b->cell.size > 0) {
        if (b->end_char != 0) {
            /* Inside escape: terminate it even if incomplete. */
            tw_end_escape(b);
        }
        tw_terminate_cell(b, false);
    }

    /* Format contents of buffer. */
    tw_format(b, 0, 0, b->lines_len);
    tw_reset(b);
}

/* Go's handlePanic, for the catch blocks below. A write error comes back as
 * the error. Anything else is passed on with the operation named, and the
 * text is kept in the writer, so it lives until the writer is freed or panics
 * again. */
static Error tw_handle_panic(TabwriterWriter *b, Any r, Str op) {
    Error e = b->err;
    b->err = BURROW_NO_ERROR;
    if (BURROW_FAILED(e))
        return e;
    Str msg = fmt_sprintf_v(b->a, "tabwriter: panic during %s (%v)", op, r);
    if (b->panic_msg.p != NULL)
        mem_free(b->a, (void *)(uintptr_t)b->panic_msg.p, (size_t)b->panic_msg.len, 1);
    b->panic_msg = msg;
    panic_str(msg);
}

/* ------------------------------------------------------------------ the API */

TabwriterWriter *tabwriter_writer_init(TabwriterWriter *b, IoWriter output,
                                       Int minwidth, Int tabwidth, Int padding,
                                       Byte padchar, Uint flags) {
    if (minwidth < 0 || tabwidth < 0 || padding < 0)
        panic_str(BURROW_S("negative minwidth, tabwidth, or padding"));
    if (b->a == NULL)
        b->a = heap_allocator();
    b->output = output;
    b->minwidth = minwidth;
    b->tabwidth = tabwidth;
    b->padding = padding;
    memset(b->padbytes, padchar, sizeof b->padbytes);
    if (padchar == '\t') {
        /* Tab padding enforces left-alignment. */
        flags &= ~(Uint)TABWRITER_ALIGN_RIGHT;
    }
    b->flags = flags;
    b->err = BURROW_NO_ERROR;

    /* The first line needs memory, and a panic here has nowhere to go but
     * out, so running out of it is a panic with the error. */
    BURROW_TRY {
        tw_reset(b);
    }
    BURROW_CATCH(r) {
        (void)r;
        Error e = b->err;
        b->err = BURROW_NO_ERROR;
        panic(BURROW_ANY(TYPE_ERROR, &e));
    }
    BURROW_TRY_END;

    return b;
}

TabwriterWriter *tabwriter_new_writer(Alloc *a, IoWriter output, Int minwidth,
                                      Int tabwidth, Int padding, Byte padchar,
                                      Uint flags) {
    if (minwidth < 0 || tabwidth < 0 || padding < 0)
        panic_str(BURROW_S("negative minwidth, tabwidth, or padding"));
    TabwriterWriter *b =
        (TabwriterWriter *)mem_alloc(a, sizeof *b, _Alignof(TabwriterWriter));
    if (b == NULL)
        return NULL;
    b->a = a;
    b->own = true;
    /* Room for some lines up front, so that init has nothing to allocate and
     * cannot panic, and NULL is the one way this fails. */
    b->lines = (TabwriterLine *)mem_alloc(a, 8 * sizeof(TabwriterLine),
                                          _Alignof(TabwriterLine));
    if (b->lines == NULL) {
        mem_free(a, b, sizeof *b, _Alignof(TabwriterWriter));
        return NULL;
    }
    b->lines_cap = 8;
    return tabwriter_writer_init(b, output, minwidth, tabwidth, padding, padchar,
                                 flags);
}

void tabwriter_writer_free(TabwriterWriter *b) {
    if (b == NULL || b->a == NULL)
        return;
    Alloc *a = b->a;
    for (Int i = 0; i < b->lines_cap; i++)
        if (b->lines[i].cells != NULL)
            mem_free(a, b->lines[i].cells,
                     (size_t)b->lines[i].cap * sizeof(TabwriterCell),
                     _Alignof(TabwriterCell));
    if (b->lines != NULL)
        mem_free(a, b->lines, (size_t)b->lines_cap * sizeof(TabwriterLine),
                 _Alignof(TabwriterLine));
    if (b->buf != NULL)
        mem_free(a, b->buf, (size_t)b->buf_cap, 1);
    if (b->widths != NULL)
        mem_free(a, b->widths, (size_t)b->widths_cap * sizeof(Int), _Alignof(Int));
    if (b->panic_msg.p != NULL)
        mem_free(a, (void *)(uintptr_t)b->panic_msg.p, (size_t)b->panic_msg.len, 1);
    if (b->own) {
        mem_free(a, b, sizeof *b, _Alignof(TabwriterWriter));
        return;
    }
    /* A writer of the caller's: the struct is theirs, and it goes back to
     * zero apart from the allocator. */
    memset(b, 0, sizeof *b);
    b->a = a;
}

/* The panic handling for Flush. The error goes out through e, since a local
 * set in a catch block may be lost to the jump. */
static void tw_flush_catch(TabwriterWriter *b, Error *e) {
    BURROW_TRY {
        tw_flush_no_defers(b);
    }
    BURROW_CATCH(r) {
        /* If Flush ran into a panic, we still need to reset. */
        b->buf_len = 0;
        b->pos = 0;
        b->cell = (TabwriterCell){0, 0, false};
        b->end_char = 0;
        b->lines_len = 1;
        b->lines[0].len = 0;
        b->widths_len = 0;
        *e = tw_handle_panic(b, r, BURROW_S("Flush"));
    }
    BURROW_TRY_END;
}

Error tabwriter_writer_flush(TabwriterWriter *b) {
    Error e = BURROW_NO_ERROR;
    tw_flush_catch(b, &e);
    return e;
}

/* The body of Write, apart from the function that calls setjmp, so that the
 * compiler keeps its locals in registers. *n is how much of p has been taken
 * in, which Write reports after a panic. */
static void tw_write(TabwriterWriter *b, const Byte *p, Int len, volatile Int *n) {
    if (len == 0)
        return; /* p may be NULL, and NULL + 0 is not allowed in C */
    /* Split text into cells. */
    for (Int i = 0; i < len; i++) {
        Byte ch = p[i];
        if (b->end_char == 0) {
            /* Outside escape. */
            switch (ch) {
            case '\t':
            case '\v':
            case '\n':
            case '\f': {
                /* End of cell. */
                tw_append(b, p + *n, i - *n);
                tw_update_width(b);
                *n = i + 1; /* ch consumed */
                Int ncells = tw_terminate_cell(b, ch == '\t');
                if (ch == '\n' || ch == '\f') {
                    /* Terminate line. */
                    tw_add_line(b, ch == '\f');
                    if (ch == '\f' || ncells == 1) {
                        /* A '\f' always forces a flush. Otherwise, if the
                         * previous line has only one cell which does not
                         * have an impact on the formatting of the
                         * following lines (the last cell per line is
                         * ignored by format()), thus we can flush the
                         * Writer contents. */
                        tw_flush_no_defers(b);
                        if (ch == '\f' && (b->flags & TABWRITER_DEBUG) != 0) {
                            /* Indicate section break. */
                            tw_write0(b, tw_hbar, 4);
                        }
                    }
                }
                break;
            }

            case TABWRITER_ESCAPE:
                /* Start of escaped sequence. */
                tw_append(b, p + *n, i - *n);
                tw_update_width(b);
                *n = i;
                if ((b->flags & TABWRITER_STRIP_ESCAPE) != 0)
                    *n = *n + 1; /* strip Escape */
                tw_start_escape(b, TABWRITER_ESCAPE);
                break;

            case '<':
            case '&':
                /* Possibly an html tag/entity. */
                if ((b->flags & TABWRITER_FILTER_HTML) != 0) {
                    /* Begin of tag/entity. */
                    tw_append(b, p + *n, i - *n);
                    tw_update_width(b);
                    *n = i;
                    tw_start_escape(b, ch);
                }
                break;

            default:
                break;
            }

        } else {
            /* Inside escape. */
            if (ch == b->end_char) {
                /* End of tag/entity. */
                Int j = i + 1;
                if (ch == TABWRITER_ESCAPE && (b->flags & TABWRITER_STRIP_ESCAPE) != 0)
                    j = i; /* strip Escape */
                tw_append(b, p + *n, j - *n);
                *n = i + 1; /* ch consumed */
                tw_end_escape(b);
            }
        }
    }

    /* Append leftover text. */
    tw_append(b, p + *n, len - *n);
    *n = len;
}

/* Write writes buf to the writer b. The only errors returned are ones
 * encountered while writing to the underlying output stream. */
/* The panic handling for Write, which reports through e as Flush does. */
static void tw_write_catch(TabwriterWriter *b, Slice buf, volatile Int *n, Error *e) {
    BURROW_TRY {
        tw_write(b, (const Byte *)buf.p, buf.len, n);
    }
    BURROW_CATCH(r) {
        *e = tw_handle_panic(b, r, BURROW_S("Write"));
    }
    BURROW_TRY_END;
}

Int tabwriter_writer_write(TabwriterWriter *b, Slice buf, Error *err) {
    /* Read after a panic, so it lives in memory and not in a register that
     * the jump back does not restore. */
    volatile Int n = 0;
    Error e = BURROW_NO_ERROR;
    tw_write_catch(b, buf, &n, &e);
    if (err != NULL)
        *err = e;
    return n;
}

static Int tabwriter_io_write(void *self, Slice p, Error *err) {
    return tabwriter_writer_write((TabwriterWriter *)self, p, err);
}

static const IoWriterVT tabwriter_writer_vt = {&tabwriter_writer_desc,
                                               tabwriter_io_write};

IoWriter tabwriter_writer_as_io_writer(TabwriterWriter *b) {
    IoWriter w = {&tabwriter_writer_vt, b};
    return w;
}
