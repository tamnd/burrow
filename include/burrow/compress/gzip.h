/* compress/gzip, the gzip file format of RFC 1952.
 *
 * gzip is DEFLATE with a header that can carry a file name, a comment, a
 * modification time and some extra bytes, and a trailer with the CRC-32 and the
 * length of the uncompressed data. It is what .gz files and HTTP's
 * Content-Encoding: gzip are. The compression itself is compress/flate's.
 *
 *     GzipWriter *zw = gzip_new_writer(a, out);
 *     zw->header.name = BURROW_S("notes.txt");
 *     gzip_writer_write(zw, data, &err);
 *     err = gzip_writer_close(zw);
 *     gzip_writer_free(zw);
 *
 *     GzipReader *zr = gzip_new_reader(a, compressed, &err);
 *     int64_t n = io_copy(a, out, gzip_reader_as_io_reader(zr), &err);
 *     gzip_reader_free(zr);
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package compress/gzip */

#ifndef BURROW_COMPRESS_GZIP_H
#define BURROW_COMPRESS_GZIP_H

#include "burrow/bufio.h"
#include "burrow/compress/flate.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/time.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The compression levels, the same numbers as compress/flate's. */
enum {
    GZIP_NO_COMPRESSION = 0,
    GZIP_BEST_SPEED = 1,
    GZIP_BEST_COMPRESSION = 9,
    GZIP_DEFAULT_COMPRESSION = -1,
    GZIP_HUFFMAN_ONLY = -2,
};

/* gzip.ErrChecksum: the CRC-32 or the length at the end does not match. */
extern const Error gzip_err_checksum;

/* gzip.ErrHeader: the data does not start with a gzip header. */
extern const Error gzip_err_header;

/* gzip.Header: what the header of a gzip stream says about the file in it.
 * The strings are UTF-8 here and Latin-1 in the stream, so a writer refuses
 * one with a character past U+00FF or a NUL in it. An extra with a nil p is
 * left out of the stream, and one with a p but no bytes goes in empty. */
typedef struct GzipHeader {
    Str comment;
    Slice extra;
    Time mod_time; /* the zero Time when the stream has none */
    Str name;
    Byte os; /* 255 for unknown */
} GzipHeader;

/* ---------------------------------------------------------------- reading */

/* gzip.Reader: decompresses a gzip stream. header is what the first member's
 * header said. Its strings and extra belong to the reader and last until the
 * next reset or the free.
 *
 * A gzip stream can be several members one after another, which read as one.
 * gzip_reader_multistream turns that off. Each member's checksum is checked at
 * its end, so the data a read gives back is not known to be good until the
 * read that gives io_eof. */
typedef struct GzipReader {
    GzipHeader header;

    Alloc *a;
    IoReader r; /* the source, with ReadByte: r itself, or rbuf around it */
    BufioReader *rbuf;
    IoReadCloser decompressor;
    uint32_t digest;
    uint32_t size;
    Error err;
    bool multistream;
    Byte buf[512];
} GzipReader;

extern const Type *const TYPE_GZIP_READER;

/* gzip.NewReader. Reads the header from r and gives a reader of the data
 * after it, from a. NULL with the error when the header cannot be read: io_eof
 * for an empty r, io_err_unexpected_eof for one that stops inside the header,
 * and gzip_err_header for one that is not gzip. When r has no ReadByte the
 * reader puts a bufio.Reader in front of it, which may read past the end of
 * the gzip data. */
BURROW_OWNS(ret) GzipReader *gzip_new_reader(Alloc *a, IoReader r, Error *err);

/* Reader.Reset: forgets the state and starts on r, reading its header as
 * gzip_new_reader does, and turns multistream back on. The allocations the
 * reader has are kept for the new stream. */
BURROW_STATIC(ret) Error gzip_reader_reset(GzipReader *z, IoReader r);

/* Reader.Multistream: with ok false, the reader stops at the end of the first
 * member and gives io_eof there, leaving the source just after it, so that
 * gzip_reader_reset can start on the next. Go's Multistream(false) comment
 * says this only works when the source has ReadByte, and it is the same here. */
void gzip_reader_multistream(GzipReader *z, bool ok);

/* Reader.Read. Once anything has failed, every call gives that error. */
Int gzip_reader_read(GzipReader *z, Slice p, Error *err);

/* Reader.Close: closes the decompressor, not the source. The checksum is only
 * checked by reading to the end, not by closing. */
BURROW_STATIC(ret) Error gzip_reader_close(GzipReader *z);

/* The reader as an IoReader and an IoReadCloser, borrowing z. */
IoReader gzip_reader_as_io_reader(GzipReader *z);
IoReadCloser gzip_reader_as_io_read_closer(GzipReader *z);

/* Gives the reader, its header and everything else it holds back to its
 * allocator. NULL is fine. */
void gzip_reader_free(GzipReader *z);

/* ---------------------------------------------------------------- writing */

/* gzip.Writer: compresses what is written to it into another writer. The
 * output is the same as Go's, byte for byte.
 *
 * Set the fields of header before the first write, flush or close, which is
 * when the header goes out. The writer borrows them until then and does not
 * free them. */
typedef struct GzipWriter {
    GzipHeader header;

    Alloc *a;
    IoWriter w;
    Int level;
    bool wrote_header;
    bool closed;
    Byte buf[10];
    FlateWriter *compressor;
    uint32_t digest;
    uint32_t size;
    Error err;
} GzipWriter;

extern const Type *const TYPE_GZIP_WRITER;

/* gzip.NewWriter: a writer into w at the default level, from a, with an empty
 * header but for os, which is 255. NULL when a refuses. The compressor itself
 * is allocated when the header is written. */
BURROW_OWNS(ret) GzipWriter *gzip_new_writer(Alloc *a, IoWriter w);

/* gzip.NewWriterLevel: the same at level. A level outside [-2, 9] gives NULL
 * and the error "gzip: invalid compression level: 10". */
BURROW_OWNS(ret) GzipWriter *gzip_new_writer_level(Alloc *a, IoWriter w, Int level,
                                                   Error *err);

/* Writer.Reset: forgets the state, any error and the header, and starts a new
 * stream into w at the same level. */
void gzip_writer_reset(GzipWriter *z, IoWriter w);

/* Writer.Write. Writes the header first if it has not been written, then
 * compresses data. Once anything has failed, every call gives that error. */
Int gzip_writer_write(GzipWriter *z, Slice data, Error *err);

/* Writer.Flush: flushes the compressor, as flate_writer_flush does. Nothing
 * after a close. */
BURROW_STATIC(ret) Error gzip_writer_flush(GzipWriter *z);

/* Writer.Close: ends the compressed stream and writes the trailer. It does not
 * close the underlying writer. A second close does nothing. */
BURROW_STATIC(ret) Error gzip_writer_close(GzipWriter *z);

/* The writer as an IoWriter and an IoWriteCloser, borrowing z. */
IoWriter gzip_writer_as_io_writer(GzipWriter *z);
IoWriteCloser gzip_writer_as_io_write_closer(GzipWriter *z);

/* Gives the writer and its compressor back to its allocator. NULL is fine. It
 * does not close the writer first, and leaves header alone. */
void gzip_writer_free(GzipWriter *z);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_COMPRESS_GZIP_H */
