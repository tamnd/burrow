/* mime/quotedprintable, the quoted-printable encoding of RFC 2045.
 *
 * Mail bodies that are mostly ASCII use it: printable bytes stay as they are,
 * anything else becomes = and two hex digits, and a line longer than 76
 * characters is broken with a soft line break, an = at the end of the line.
 * Both sides are streams, a reader that decodes what it reads and a writer
 * that encodes what is written to it:
 *
 *     QuotedprintableWriter *w = quotedprintable_new_writer(a, out);
 *     quotedprintable_writer_write(w, body, &err);
 *     err = quotedprintable_writer_close(w);
 *     quotedprintable_writer_free(w);
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package mime/quotedprintable */

#ifndef BURROW_MIME_QUOTEDPRINTABLE_H
#define BURROW_MIME_QUOTEDPRINTABLE_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- reading */

/* quotedprintable.Reader: decodes what it reads from another reader. */
typedef struct QuotedprintableReader QuotedprintableReader;

extern const Type *const TYPE_QUOTEDPRINTABLE_READER;

/* quotedprintable.NewReader. A reader of the decoded form of r, from a, which
 * reads r through a bufio.Reader, or through r itself when r is already one
 * with 4096 bytes or more of buffer. NULL when a refuses. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, r) QuotedprintableReader *
quotedprintable_new_reader(Alloc *a, IoReader r);

/* Reader.Read. Decodes into p, turning soft line breaks into nothing and
 * dropping the spaces and tabs at the end of each line, as Go does. It is
 * lenient in the same places Go's is: an = that is not followed by two hex
 * digits or a line break is passed through as a plain =, and bytes of 0x80
 * and over are passed through too. A control character other than tab, CR
 * and LF gives "quotedprintable: invalid unescaped byte 0x01 in body" after
 * the bytes before it. At the end of the input err gets io_eof. */
Int quotedprintable_reader_read(QuotedprintableReader *r, Slice p, Error *err);

/* The reader as an IoReader, borrowing r. */
IoReader quotedprintable_reader_as_io_reader(QuotedprintableReader *r);

/* Gives the reader and its buffer back to its allocator. NULL is fine. */
void quotedprintable_reader_free(QuotedprintableReader *r);

/* ---------------------------------------------------------------- writing */

/* quotedprintable.Writer: encodes what is written to it into another writer.
 * The output is the same as Go's, byte for byte.
 *
 * binary is Go's Binary field. With it false, which is how a new writer
 * starts, the input is text: a CR, an LF or a CRLF is a line break and comes
 * out as CRLF. With it true every CR and LF is data and is escaped. Set it
 * before the first write. The other fields belong to the writer. */
typedef struct QuotedprintableWriter {
    bool binary;

    Alloc *a;
    IoWriter w;
    Int i;         /* bytes of line in use */
    Byte line[78]; /* the line being built, 76 characters and a CRLF */
    bool cr;       /* the last byte written was a CR */
} QuotedprintableWriter;

extern const Type *const TYPE_QUOTEDPRINTABLE_WRITER;

/* quotedprintable.NewWriter. A writer into w, from a, with binary false. NULL
 * when a refuses. */
BURROW_OWNS(ret) BURROW_BORROWS(ret, w) QuotedprintableWriter *
quotedprintable_new_writer(Alloc *a, IoWriter w);

/* Writer.Write. Encodes p and gives len(p), or how much of p was taken before
 * w failed, with its error. A line goes to w each time it fills, so some of
 * the output is held back until the writer is closed. */
Int quotedprintable_writer_write(QuotedprintableWriter *w, Slice p, Error *err);

/* Writer.Close: escapes a space or tab at the end of the last line, which a
 * reader would drop otherwise, and writes out the rest of the line. It does
 * not close the underlying writer. */
BURROW_STATIC(ret) Error quotedprintable_writer_close(QuotedprintableWriter *w);

/* The writer as an IoWriter and an IoWriteCloser, borrowing w. */
IoWriter quotedprintable_writer_as_io_writer(QuotedprintableWriter *w);
IoWriteCloser quotedprintable_writer_as_io_write_closer(QuotedprintableWriter *w);

/* Gives the writer back to its allocator. NULL is fine. It does not close the
 * writer first. */
void quotedprintable_writer_free(QuotedprintableWriter *w);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MIME_QUOTEDPRINTABLE_H */
