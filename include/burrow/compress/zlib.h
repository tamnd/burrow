/* compress/zlib, the zlib format of RFC 1950.
 *
 * zlib is DEFLATE with a two byte header in front and an Adler-32 checksum of
 * the uncompressed data at the end. It is what PNG uses inside its IDAT chunks
 * and what HTTP calls "deflate". The compression itself is compress/flate's.
 *
 *     ZlibWriter *zw = zlib_new_writer(a, out);
 *     zlib_writer_write(zw, data, &err);
 *     err = zlib_writer_close(zw);
 *     zlib_writer_free(zw);
 *
 *     IoReadCloser rc = zlib_new_reader(a, compressed, &err);
 *     int64_t n = io_copy(a, out, io_read_closer_as_io_reader(rc), &err);
 *     zlib_reader_free(rc);
 *
 * As with compress/flate, the reader pulls bytes one at a time through ReadByte
 * when r has one and never reads past the checksum then. Any other reader gets
 * a bufio.Reader of its own in front of it, which may read ahead.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package compress/zlib */

#ifndef BURROW_COMPRESS_ZLIB_H
#define BURROW_COMPRESS_ZLIB_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The compression levels, the same numbers as compress/flate's. */
enum {
    ZLIB_NO_COMPRESSION = 0,
    ZLIB_BEST_SPEED = 1,
    ZLIB_BEST_COMPRESSION = 9,
    ZLIB_DEFAULT_COMPRESSION = -1,
    ZLIB_HUFFMAN_ONLY = -2,
};

/* ------------------------------------------------------------------ errors */

/* zlib.ErrChecksum: the checksum at the end does not match the data. */
extern const Error zlib_err_checksum;

/* zlib.ErrDictionary: the stream was compressed with a preset dictionary and
 * the reader was not given that one. */
extern const Error zlib_err_dictionary;

/* zlib.ErrHeader: the first two bytes are not a zlib header. */
extern const Error zlib_err_header;

/* ---------------------------------------------------------------- reading */

/* zlib.Resetter: points a reader from zlib_new_reader at a new stream, with a
 * new preset dictionary, reusing what it has allocated. The error is the one
 * reading the new header gave, as from zlib_new_reader_dict. */
typedef struct ZlibResetterVT {
    const Type *self_type;
    Error (*reset)(void *self, IoReader r, Slice dict);
} ZlibResetterVT;

typedef struct ZlibResetter {
    const ZlibResetterVT *vt;
    void *data;
} ZlibResetter;

BURROW_STATIC(ret) static inline Error zlib_resetter_reset(ZlibResetter rs, IoReader r,
                                                           Slice dict) {
    return rs.vt->reset(rs.data, r, dict);
}

/* zlib.NewReader. Reads the header from r and gives a reader of the data
 * after it, from a. A bad header gives a nil IoReadCloser and zlib_err_header,
 * a stream that stops inside the header gives io_err_unexpected_eof, and a
 * stream that needs a dictionary gives zlib_err_dictionary. Reading gives
 * io_eof once the checksum at the end has matched, and zlib_err_checksum when
 * it has not. Closing does not close r. */
BURROW_OWNS(ret) IoReadCloser zlib_new_reader(Alloc *a, IoReader r, Error *err);

/* zlib.NewReaderDict: the same, for a stream that may need the preset
 * dictionary dict. A stream that does not need one reads fine either way. */
BURROW_OWNS(ret) IoReadCloser zlib_new_reader_dict(Alloc *a, IoReader r, Slice dict,
                                                   Error *err);

/* The Resetter a reader from zlib_new_reader has. A nil ZlibResetter for any
 * other reader. */
ZlibResetter zlib_reader_as_resetter(IoReadCloser rc);

/* Gives a reader from zlib_new_reader and everything it holds back to its
 * allocator. A nil IoReadCloser is fine, and anything else is left alone. */
void zlib_reader_free(IoReadCloser rc);

/* ---------------------------------------------------------------- writing */

/* zlib.Writer: compresses what is written to it into another writer. The
 * output is the same as Go's, byte for byte. */
typedef struct ZlibWriter ZlibWriter;

extern const Type *const TYPE_ZLIB_WRITER;

/* zlib.NewWriter: a writer into w at the default level, from a. NULL when a
 * refuses. The compressor itself is allocated on the first write, flush or
 * close. */
BURROW_OWNS(ret) ZlibWriter *zlib_new_writer(Alloc *a, IoWriter w);

/* zlib.NewWriterLevel: the same at level. A level outside [-2, 9] gives NULL
 * and the error "zlib: invalid compression level: 10". */
BURROW_OWNS(ret) ZlibWriter *zlib_new_writer_level(Alloc *a, IoWriter w, Int level,
                                                   Error *err);

/* zlib.NewWriterLevelDict: the same with a preset dictionary, which the output
 * then needs to be read. The writer keeps its own copy of dict. A dict with a
 * nil p means none, and an empty one that is not nil still marks the header. */
BURROW_OWNS(ret) ZlibWriter *zlib_new_writer_level_dict(Alloc *a, IoWriter w, Int level,
                                                        Slice dict, Error *err);

/* Writer.Write. Writes the header first if it has not been written, then
 * compresses data. Once anything has failed, every call gives that error. */
Int zlib_writer_write(ZlibWriter *z, Slice data, Error *err);

/* Writer.Flush: flushes the compressor, as flate_writer_flush does. */
BURROW_STATIC(ret) Error zlib_writer_flush(ZlibWriter *z);

/* Writer.Close: ends the compressed stream and writes the checksum. It does
 * not close the underlying writer. */
BURROW_STATIC(ret) Error zlib_writer_close(ZlibWriter *z);

/* Writer.Reset: forgets the state and any error and starts a new stream into
 * w, at the same level and with the same dictionary. */
void zlib_writer_reset(ZlibWriter *z, IoWriter w);

/* The writer as an IoWriter and an IoWriteCloser, borrowing z. */
IoWriter zlib_writer_as_io_writer(ZlibWriter *z);
IoWriteCloser zlib_writer_as_io_write_closer(ZlibWriter *z);

/* Gives the writer and everything it holds back to its allocator. NULL is
 * fine. It does not close the writer first. */
void zlib_writer_free(ZlibWriter *z);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_COMPRESS_ZLIB_H */
