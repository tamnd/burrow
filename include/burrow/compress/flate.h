/* compress/flate, the DEFLATE compressed data format of RFC 1951.
 *
 * DEFLATE is the compression inside gzip, zlib, zip and PNG. This header has
 * the raw format with no framing around it: compress/gzip and compress/zlib
 * put their headers and checksums on top.
 *
 *     IoReadCloser rc = flate_new_reader(a, compressed);
 *     int64_t n = io_copy(a, out, io_read_closer_as_io_reader(rc), &err);
 *     flate_reader_free(rc);
 *
 * The reader pulls compressed bytes from r one at a time through ReadByte when
 * r has one, which bufio.Reader, bytes.Buffer, bytes.Reader and strings.Reader
 * all do, and then it never reads past the end of the compressed stream. Any
 * other reader gets a bufio.Reader of its own in front of it, as in Go, and
 * that one may read ahead.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package compress/flate */

#ifndef BURROW_COMPRESS_FLATE_H
#define BURROW_COMPRESS_FLATE_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ errors */

/* flate.CorruptInputError: the input is not valid DEFLATE, and the value is
 * how many bytes of it had been read when that became clear. The message is
 * Go's, "flate: corrupt input before offset 5". errors_as with
 * TYPE_FLATE_CORRUPT_INPUT_ERROR gives a pointer to the offset. */
typedef int64_t FlateCorruptInputError;

extern const Type *const TYPE_FLATE_CORRUPT_INPUT_ERROR;

/* Go's Error method, built in a. */
BURROW_OWNS(ret) Str flate_corrupt_input_error_error(FlateCorruptInputError e,
                                                     Alloc *a);

/* An Error for e with its message, built in a. Out of memory gives
 * burrow_err_out_of_memory. */
BURROW_OWNS(ret) Error flate_corrupt_input_error_as_error(FlateCorruptInputError e,
                                                          Alloc *a);

/* flate.InternalError: a bug in this package rather than in the input. The
 * message is "flate: internal error: " and then the text. */
typedef Str FlateInternalError;

extern const Type *const TYPE_FLATE_INTERNAL_ERROR;

BURROW_OWNS(ret) Str flate_internal_error_error(FlateInternalError e, Alloc *a);
BURROW_OWNS(ret) Error flate_internal_error_as_error(FlateInternalError e, Alloc *a);

/* flate.ReadError and flate.WriteError. Deprecated: nothing returns them,
 * in Go or here. They are kept so that code naming them still builds. The
 * messages are "flate: read error at offset 3: " and then err's message, and
 * the same with write. */
typedef struct FlateReadError {
    int64_t offset;
    Error err;
} FlateReadError;

typedef struct FlateWriteError {
    int64_t offset;
    Error err;
} FlateWriteError;

extern const Type *const TYPE_FLATE_READ_ERROR;
extern const Type *const TYPE_FLATE_WRITE_ERROR;

BURROW_OWNS(ret) Str flate_read_error_error(const FlateReadError *e, Alloc *a);
BURROW_OWNS(ret) Error flate_read_error_as_error(const FlateReadError *e, Alloc *a);
BURROW_OWNS(ret) Str flate_write_error_error(const FlateWriteError *e, Alloc *a);
BURROW_OWNS(ret) Error flate_write_error_as_error(const FlateWriteError *e, Alloc *a);

/* -------------------------------------------------------------- interfaces */

/* flate.Reader: what the decompressor wants from its input, an io.Reader that
 * can also hand out one byte at a time. The decompressor does not take this
 * type. It takes an IoReader and looks for ReadByte in the reader's method
 * set, which is the same question Go's NewReader asks with a type assertion. */
typedef struct FlateReaderVT {
    IoReaderVT reader;
    IoByteReaderVT byte_reader;
} FlateReaderVT;

typedef struct FlateReader {
    const FlateReaderVT *vt;
    void *data;
} FlateReader;

/* flate.Resetter: points a reader from flate_new_reader at a new input, with a
 * new preset dictionary, so that it can be used again without allocating. Reset
 * gives no error, as in Go, unless a reader with no ReadByte needs a bufio.Reader
 * in front of it and there is no memory for one, and then it is
 * burrow_err_out_of_memory. */
typedef struct FlateResetterVT {
    const Type *self_type;
    Error (*reset)(void *self, IoReader r, Slice dict);
} FlateResetterVT;

typedef struct FlateResetter {
    const FlateResetterVT *vt;
    void *data;
} FlateResetter;

BURROW_STATIC(ret) static inline Error flate_resetter_reset(FlateResetter rs,
                                                            IoReader r, Slice dict) {
    return rs.vt->reset(rs.data, r, dict);
}

/* -------------------------------------------------------------- decompress */

/* flate.NewReader. A reader of the decompressed form of r, from a. Reading it
 * gives io_eof at the end of the final block, and a corrupt stream gives a
 * FlateCorruptInputError after handing out whatever decoded before it. Closing
 * it does not close r and only reports the error that stopped reading, if that
 * was not the end of the stream. A nil IoReadCloser when a refuses. */
BURROW_OWNS(ret) IoReadCloser flate_new_reader(Alloc *a, IoReader r);

/* flate.NewReaderDict: the same, with a preset dictionary, for a stream that
 * was compressed with one. Only the last 32 KiB of dict matter. The reader
 * keeps its own copy. */
BURROW_OWNS(ret) IoReadCloser flate_new_reader_dict(Alloc *a, IoReader r, Slice dict);

/* The Resetter a reader from flate_new_reader has, which is Go's
 * rc.(flate.Resetter). A nil FlateResetter for any other reader. */
FlateResetter flate_reader_as_resetter(IoReadCloser rc);

/* Gives a reader from flate_new_reader and everything it holds back to its
 * allocator. A nil IoReadCloser is fine, and anything else not from
 * flate_new_reader is left alone. */
void flate_reader_free(IoReadCloser rc);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_COMPRESS_FLATE_H */
