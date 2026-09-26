/* text/tabwriter, a writer that lines text up in columns.
 *
 * Go's text/tabwriter. The writer takes text made of cells separated by tabs
 * and lines ended by newlines, and pads the cells so that each column comes
 * out as wide as its widest cell. It is the elastic tabstops algorithm, so a
 * column is a run of adjacent lines that have a cell in that position, and
 * not every cell in position two of the whole text.
 *
 *     TabwriterWriter *w = tabwriter_new_writer(a, out, 0, 8, 1, ' ', 0);
 *     IoWriter tw = tabwriter_writer_as_io_writer(w);
 *     fmt_fprintf_v(tw, "%s\t%s\t%d\n", "name", "kind", 1);
 *     fmt_fprintf_v(tw, "%s\t%s\t%d\n", "longer name", "k", 22);
 *     Error err = tabwriter_writer_flush(w);
 *     tabwriter_writer_free(w);
 *
 * Nothing reaches out until a line has only one cell, a form feed arrives, or
 * you call tabwriter_writer_flush, since a column cannot be sized before its
 * last cell is known. Flush when you are done.
 *
 * A tab ends a cell. A newline or form feed ends a line, and so ends a cell
 * too. The text after the last tab on a line is a cell that is not part of any
 * column, so give each line a trailing tab when the last column should line
 * up as well.
 *
 * The writer keeps the text it has not written yet in memory from the
 * allocator you give it. When that runs out, the write or flush that needed
 * the memory returns burrow_err_out_of_memory.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package text/tabwriter */

#ifndef BURROW_TEXT_TABWRITER_H
#define BURROW_TEXT_TABWRITER_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The flags, which are Go's, and which you or together. */
enum {
    /* Skip over HTML tags and treat an entity such as &amp; as one character
     * wide. */
    TABWRITER_FILTER_HTML = 1 << 0,
    /* Take the escape characters out of the text they bracket, rather than
     * passing them through. */
    TABWRITER_STRIP_ESCAPE = 1 << 1,
    /* Put the padding before the cell text, not after it. */
    TABWRITER_ALIGN_RIGHT = 1 << 2,
    /* Treat a column made only of empty cells ended by a vertical tab as if it
     * were not there. */
    TABWRITER_DISCARD_EMPTY_COLUMNS = 1 << 3,
    /* Pad the empty cells at the start of a line with tabs, whatever the pad
     * character, so indentation stays tabs. */
    TABWRITER_TAB_INDENT = 1 << 4,
    /* Print a vertical bar between columns, to see where they are. */
    TABWRITER_DEBUG = 1 << 5,
};

/* tabwriter.Escape. Text between two of these is one cell with no tabs or
 * newlines in it, whatever it holds, and its width is that of the text inside
 * the pair. */
#define TABWRITER_ESCAPE ((Byte)0xff)

/* One cell. */
typedef struct TabwriterCell {
    Int size;  /* in bytes */
    Int width; /* in runes */
    bool htab; /* ended by a tab, not a vertical tab */
} TabwriterCell;

/* One line of cells. */
typedef struct TabwriterLine {
    TabwriterCell *cells;
    Int len, cap;
} TabwriterLine;

/* tabwriter.Writer. The fields are here so the struct has a size, and are not
 * for touching. A zeroed one is ready for tabwriter_writer_init, and takes its
 * memory from the heap unless you set a first. */
typedef struct TabwriterWriter {
    Alloc *a;
    IoWriter output;
    Int minwidth;
    Int tabwidth;
    Int padding;
    Byte padbytes[8];
    Uint flags;

    Byte *buf; /* the text not yet written, without tabs or line breaks */
    Int buf_len, buf_cap;
    Int pos;            /* how far into buf cell.width has been counted */
    TabwriterCell cell; /* the cell being filled */
    Byte end_char;      /* what ends the escape we are in, or 0 */
    TabwriterLine *lines;
    Int lines_len, lines_cap;
    Int *widths; /* column widths, used while formatting */
    Int widths_len, widths_cap;

    Error err;     /* set on the way out of a failed write */
    Str panic_msg; /* the text of the last panic this writer passed on */
    bool own;      /* the struct came from a, not from the caller */
} TabwriterWriter;

/* tabwriter.Writer.Init. Sets the writer up to write to output, dropping
 * anything it was holding.
 *
 * minwidth is the smallest a cell gets, padding included. tabwidth is how
 * wide a tab is taken to be when padchar is a tab or TABWRITER_TAB_INDENT is
 * on. padding is added to each cell before its width is worked out. padchar
 * is what the padding is made of, and when it is a tab the text is left
 * aligned whatever the flags say, since a terminal decides where tabs stop.
 *
 * Panics when minwidth, tabwidth or padding is negative, as Go does. Returns
 * b. */
BURROW_BORROWS(ret, b) TabwriterWriter *
tabwriter_writer_init(TabwriterWriter *b, IoWriter output, Int minwidth, Int tabwidth,
                      Int padding, Byte padchar, Uint flags);

/* tabwriter.NewWriter. A writer from a, set up as tabwriter_writer_init does.
 * NULL when the allocator refuses. */
BURROW_OWNS(ret) TabwriterWriter *tabwriter_new_writer(Alloc *a, IoWriter output,
                                                       Int minwidth, Int tabwidth,
                                                       Int padding, Byte padchar,
                                                       Uint flags);

/* Gives back the writer's memory, and the writer itself when it came from
 * tabwriter_new_writer. Does not flush, so flush first. NULL is fine. */
void tabwriter_writer_free(TabwriterWriter *b);

/* tabwriter.Writer.Write. Takes in buf and writes out whatever lines are now
 * complete. Returns len(buf) unless the output failed, and then the error and
 * how far into buf it got.
 *
 * A panic in the output writer is passed on as a panic whose text is
 * "tabwriter: panic during Write (...)" with the original inside, as in Go. */
Int tabwriter_writer_write(TabwriterWriter *b, Slice buf, Error *err);

/* tabwriter.Writer.Flush. Writes out everything held, ending the cell and the
 * line in progress. Call it when you are done writing. After an error the
 * writer is empty and ready to be used again. */
BURROW_STATIC(ret) Error tabwriter_writer_flush(TabwriterWriter *b);

IoWriter tabwriter_writer_as_io_writer(TabwriterWriter *b);

extern const Type *const TYPE_TABWRITER_WRITER;

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TEXT_TABWRITER_H */
