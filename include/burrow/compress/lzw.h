/* compress/lzw, the Lempel-Ziv-Welch format of GIF, TIFF and PDF.
 *
 * This is the variable width LZW that GIF and PDF use: codes start one bit
 * wider than a literal, grow to at most 12 bits, and a clear code starts the
 * table over. TIFF writes the width change one code early, which this package
 * does not do, so it reads GIF and PDF streams but not TIFF's.
 *
 *     LzwWriter *zw = lzw_new_writer(a, out, LZW_LSB, 8);
 *     lzw_writer_write(zw, data, &err);
 *     err = lzw_writer_close(zw);
 *     lzw_writer_free(zw);
 *
 *     LzwReader *zr = lzw_new_reader(a, compressed, LZW_LSB, 8);
 *     Slice text = io_read_all(a, lzw_reader_as_io_reader(zr), &err);
 *     lzw_reader_free(zr);
 *
 * The reader pulls bytes one at a time through ReadByte when r has one, and
 * any other reader gets a bufio.Reader of its own in front of it. The writer
 * writes through a bufio.Writer: w itself when it is one, or one of its own.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package compress/lzw */

#ifndef BURROW_COMPRESS_LZW_H
#define BURROW_COMPRESS_LZW_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* lzw.Order: how codes are packed into bytes. LZW_LSB puts the first code in
 * the low bits of the first byte, as GIF does, and LZW_MSB in the high bits,
 * as TIFF and PDF do. Any other value is an error when it is used. */
typedef Int LzwOrder;

enum {
    LZW_LSB = 0,
    LZW_MSB = 1,
};

/* ---------------------------------------------------------------- reading */

/* lzw.Reader: decompresses what it reads from another reader. */
typedef struct LzwReader LzwReader;

extern const Type *const TYPE_LZW_READER;

/* lzw.NewReader. A reader of the decompressed form of r, from a, where
 * lit_width is the width in bits of a literal code, from 2 to 8, and is
 * usually 8. An order or width out of range is not reported here: reading
 * gives the error, "lzw: unknown order" or "lzw: litWidth 9 out of range", as
 * Go's does. NULL when a refuses. Go's NewReader returns an io.ReadCloser whose
 * value is always a *Reader, and lzw_reader_as_io_read_closer gives that view. */
BURROW_OWNS(ret) LzwReader *lzw_new_reader(Alloc *a, IoReader r, LzwOrder order,
                                           Int lit_width);

/* Reader.Read. Gives io_eof after the end code, io_err_unexpected_eof when the
 * input stops before it, and "lzw: invalid code" for a code the table does not
 * have yet, each after the bytes that decoded before it. */
Int lzw_reader_read(LzwReader *r, Slice p, Error *err);

/* Reader.Close: makes every later read give "lzw: reader/writer is closed".
 * It does not close the underlying reader and always gives no error. */
BURROW_STATIC(ret) Error lzw_reader_close(LzwReader *r);

/* Reader.Reset: forgets all state and starts reading a new stream from src,
 * as if the reader had just come from lzw_new_reader. A bufio.Reader the old
 * source needed is kept and reused. */
void lzw_reader_reset(LzwReader *r, IoReader src, LzwOrder order, Int lit_width);

/* The reader as an IoReader and an IoReadCloser, borrowing r. */
IoReader lzw_reader_as_io_reader(LzwReader *r);
IoReadCloser lzw_reader_as_io_read_closer(LzwReader *r);

/* Gives the reader and everything it holds back to its allocator. NULL is
 * fine. */
void lzw_reader_free(LzwReader *r);

/* ---------------------------------------------------------------- writing */

/* lzw.Writer: compresses what is written to it into another writer. The
 * output is the same as Go's, byte for byte. */
typedef struct LzwWriter LzwWriter;

extern const Type *const TYPE_LZW_WRITER;

/* lzw.NewWriter. A writer into w, from a, with literals lit_width bits wide.
 * As with the reader, a bad order or width shows up as the error of the first
 * write or close. NULL when a refuses. */
BURROW_OWNS(ret) LzwWriter *lzw_new_writer(Alloc *a, IoWriter w, LzwOrder order,
                                           Int lit_width);

/* Writer.Write. Gives len(p), or 0 and an error, which is "lzw: input byte too
 * large for the litWidth" for a byte that does not fit in lit_width bits.
 * Nothing reaches w until a bufio.Writer's worth has built up or the writer is
 * closed. Once anything has failed, every call gives that error. */
Int lzw_writer_write(LzwWriter *w, Slice p, Error *err);

/* Writer.Close: writes the last code and the end code and flushes. It does
 * not close the underlying writer. Writing after it gives "lzw: reader/writer
 * is closed", and closing again gives no error. */
BURROW_STATIC(ret) Error lzw_writer_close(LzwWriter *w);

/* Writer.Reset: forgets all state and starts a new stream into dst, as if the
 * writer had just come from lzw_new_writer. */
void lzw_writer_reset(LzwWriter *w, IoWriter dst, LzwOrder order, Int lit_width);

/* The writer as an IoWriter and an IoWriteCloser, borrowing w. */
IoWriter lzw_writer_as_io_writer(LzwWriter *w);
IoWriteCloser lzw_writer_as_io_write_closer(LzwWriter *w);

/* Gives the writer and everything it holds back to its allocator. NULL is
 * fine. It does not close the writer first. */
void lzw_writer_free(LzwWriter *w);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_COMPRESS_LZW_H */
