/* Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* bufio: buffered readers and writers, and a Scanner that splits input into
 * lines, words or anything else.
 *
 * Go's bufio, all of it. A Reader sits in front of an IoReader and reads from
 * it in large blocks, so that small reads, byte at a time or line at a time,
 * cost a copy and not a call into the source. A Writer does the same for
 * writes and sends them on when its buffer fills or when you flush it. A
 * Scanner is the easy way to go through input a line at a time:
 *
 *     BufioScanner *sc = bufio_new_scanner(a, r);
 *     while (bufio_scanner_scan(sc)) {
 *         Str line = bufio_scanner_text(sc);
 *         ...
 *     }
 *     Error err = bufio_scanner_err(sc);
 *     bufio_scanner_free(sc);
 *
 * The three are allocated from the allocator you give them, buffer and all,
 * and the matching free gives both back. Nothing here keeps a pointer to the
 * allocator's memory beyond that.
 *
 * Like Go, bufio_new_reader_size hands back the reader it was given when that
 * is already a BufioReader with a big enough buffer, and the writer side does
 * the same. Here that means two owners of one reader, so the reader counts
 * them, and it is only freed when the last one calls bufio_reader_free. You can
 * free every reader you were handed without checking whether two of them are
 * the same.
 */

/* burrow:package bufio */

#ifndef BURROW_BUFIO_H
#define BURROW_BUFIO_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* bufio.MaxScanTokenSize. The largest token a Scanner accepts unless you give
 * it a buffer and a limit of your own with bufio_scanner_buffer. */
#define BUFIO_MAX_SCAN_TOKEN_SIZE 65536

/* ---------------------------------------------------------------- the errors */

extern const Error
    bufio_err_invalid_unread_byte; /* "bufio: invalid use of UnreadByte" */
extern const Error
    bufio_err_invalid_unread_rune;           /* "bufio: invalid use of UnreadRune" */
extern const Error bufio_err_buffer_full;    /* "bufio: buffer full" */
extern const Error bufio_err_negative_count; /* "bufio: negative count" */

extern const Error bufio_err_too_long;         /* the token is bigger than the limit */
extern const Error bufio_err_negative_advance; /* a split function advanced by < 0 */
extern const Error bufio_err_advance_too_far;  /* ... or past the end of its input */
extern const Error bufio_err_bad_read_count;   /* the reader said it read too much */

/* bufio.ErrFinalToken. A split function returns this with a token to say that
 * the token is the last one, and scanning stops after it without an error. */
extern const Error bufio_err_final_token;

/* ------------------------------------------------------------------ Reader
 *
 * bufio.Reader. The fields are here so the struct has a size, and are not for
 * touching. */
typedef struct BufioReader {
    Alloc *a;
    Byte *buf;
    Int size;
    IoReader rd;
    Int r, w; /* buf[r:w] is what has been read and not handed out */
    Error err;
    Int last_byte;      /* for UnreadByte, or -1 */
    Int last_rune_size; /* for UnreadRune, or -1 */
    Int refs;           /* how many bufio_new_reader_size calls returned this */
    bool own;           /* the struct came from a, not from the caller */
} BufioReader;

/* bufio.NewReaderSize. A reader with a buffer of at least size bytes, 16 at
 * the least, or rd itself when rd is a BufioReader whose buffer is already
 * that big. NULL when the allocator refuses. */
BURROW_OWNS(ret) BufioReader *bufio_new_reader_size(Alloc *a, IoReader rd, Int size);

/* bufio.NewReader, which is the above with Go's 4096 bytes. */
BURROW_OWNS(ret) BufioReader *bufio_new_reader(Alloc *a, IoReader rd);

/* Gives the reader and its buffer back to the allocator, once the last owner
 * has called it. NULL is fine. For a zeroed BufioReader of your own, which
 * bufio_reader_reset has given a buffer, this frees only the buffer. */
void bufio_reader_free(BufioReader *b);

/* bufio.Reader.Reset. Throws away anything buffered and any error and starts
 * reading from r, keeping the buffer. Resetting a reader to read from itself
 * does nothing, as in Go. A zeroed BufioReader works too: it gets a 4096 byte
 * buffer here, from its a field, or from the heap when that is NULL. */
void bufio_reader_reset(BufioReader *b, IoReader r);

Int bufio_reader_size(BufioReader *b);
Int bufio_reader_buffered(BufioReader *b);

/* bufio.Reader.Peek. The next n bytes without reading them. The slice points
 * into the buffer and is good until the next read. Fewer than n bytes comes
 * with the reason, which is bufio_err_buffer_full when n is bigger than the
 * buffer. */
BURROW_BORROWS(ret, b) Slice bufio_reader_peek(BufioReader *b, Int n, Error *err);

/* bufio.Reader.Discard. Skips n bytes and returns how many it skipped, which
 * is n unless there is an error. */
Int bufio_reader_discard(BufioReader *b, Int n, Error *err);

Int bufio_reader_read(BufioReader *b, Slice p, Error *err);
Byte bufio_reader_read_byte(BufioReader *b, Error *err);
BURROW_STATIC(ret) Error bufio_reader_unread_byte(BufioReader *b);
Rune bufio_reader_read_rune(BufioReader *b, Int *size, Error *err);
BURROW_STATIC(ret) Error bufio_reader_unread_rune(BufioReader *b);

/* bufio.Reader.ReadSlice. Reads up to and including delim and returns a slice
 * of the buffer holding it, good until the next read. A line longer than the
 * buffer gives the full buffer and bufio_err_buffer_full. Most callers want
 * bufio_reader_read_bytes or bufio_reader_read_string, which have no limit. */
BURROW_BORROWS(ret, b) Slice bufio_reader_read_slice(BufioReader *b, Byte delim,
                                                     Error *err);

/* bufio.Reader.ReadLine. A line without its "\n" or "\r\n", from the buffer.
 * A line too long for the buffer comes in pieces with *is_prefix set on all
 * but the last. The low level way to read lines, and the Scanner is the easy
 * one. */
BURROW_BORROWS(ret, b) Slice bufio_reader_read_line(BufioReader *b, bool *is_prefix,
                                                    Error *err);

/* bufio.Reader.ReadBytes and ReadString. Read up to and including delim, as
 * much as that takes, into memory from a. The end of the input before delim
 * gives what was read and the error, usually io_eof. */
BURROW_OWNS(ret) Slice bufio_reader_read_bytes(BufioReader *b, Alloc *a, Byte delim,
                                               Error *err);
BURROW_OWNS(ret) Str bufio_reader_read_string(BufioReader *b, Alloc *a, Byte delim,
                                              Error *err);

/* bufio.Reader.WriteTo. Sends everything left to w, using w's ReadFrom or the
 * underlying reader's WriteTo when there is one. */
int64_t bufio_reader_write_to(BufioReader *b, IoWriter w, Error *err);

IoReader bufio_reader_as_io_reader(BufioReader *b);
IoByteReader bufio_reader_as_io_byte_reader(BufioReader *b);
IoByteScanner bufio_reader_as_io_byte_scanner(BufioReader *b);
IoRuneReader bufio_reader_as_io_rune_reader(BufioReader *b);
IoRuneScanner bufio_reader_as_io_rune_scanner(BufioReader *b);
IoWriterTo bufio_reader_as_io_writer_to(BufioReader *b);

extern const Type *const TYPE_BUFIO_READER;

/* ------------------------------------------------------------------ Writer
 *
 * bufio.Writer. After a write error every later write fails with the same
 * error, and so does Flush, so checking the result of the final
 * bufio_writer_flush is enough to know whether everything got through. */
typedef struct BufioWriter {
    Alloc *a;
    Error err;
    Byte *buf;
    Int size;
    Int n; /* bytes in buf */
    IoWriter wr;
    Int refs;
    bool own;
} BufioWriter;

/* bufio.NewWriterSize. A size of zero or less means Go's 4096. Hands back w
 * itself when it is a BufioWriter with a buffer that big already. */
BURROW_OWNS(ret) BufioWriter *bufio_new_writer_size(Alloc *a, IoWriter w, Int size);
BURROW_OWNS(ret) BufioWriter *bufio_new_writer(Alloc *a, IoWriter w);

/* Frees without flushing, so flush first. As with the reader, a zeroed
 * BufioWriter of your own only has its buffer freed. */
void bufio_writer_free(BufioWriter *b);

/* bufio.Writer.Reset. Drops anything buffered and any error and writes to w
 * from now on. A zeroed BufioWriter gets a 4096 byte buffer here, the same way
 * a zeroed reader does. */
void bufio_writer_reset(BufioWriter *b, IoWriter w);

BURROW_STATIC(ret) Error bufio_writer_flush(BufioWriter *b);
Int bufio_writer_size(BufioWriter *b);
Int bufio_writer_available(BufioWriter *b);
Int bufio_writer_buffered(BufioWriter *b);

/* bufio.Writer.AvailableBuffer. An empty slice with the free part of the
 * buffer as its capacity. Append to it and pass the result to
 * bufio_writer_write, and the bytes are only copied once. */
BURROW_BORROWS(ret, b) Slice bufio_writer_available_buffer(BufioWriter *b);

Int bufio_writer_write(BufioWriter *b, Slice p, Error *err);
BURROW_STATIC(ret) Error bufio_writer_write_byte(BufioWriter *b, Byte c);
Int bufio_writer_write_rune(BufioWriter *b, Rune r, Error *err);
Int bufio_writer_write_string(BufioWriter *b, Str s, Error *err);
int64_t bufio_writer_read_from(BufioWriter *b, IoReader r, Error *err);

IoWriter bufio_writer_as_io_writer(BufioWriter *b);
IoByteWriter bufio_writer_as_io_byte_writer(BufioWriter *b);
IoStringWriter bufio_writer_as_io_string_writer(BufioWriter *b);
IoReaderFrom bufio_writer_as_io_reader_from(BufioWriter *b);

extern const Type *const TYPE_BUFIO_WRITER;

/* -------------------------------------------------------------- ReadWriter
 *
 * bufio.ReadWriter, a reader and a writer side by side. Go embeds the two
 * pointers so their methods are promoted. C cannot do that, so each promoted
 * method is written out and passes straight through. It owns neither side.
 * The methods take it by value, as Go's do, and the interface conversions
 * take its address, which has to outlive the interface. */
typedef struct BufioReadWriter {
    BufioReader *reader;
    BufioWriter *writer;
} BufioReadWriter;

/* bufio.NewReadWriter. Go returns a pointer, and this returns the two pointers
 * by value, since there is nothing to allocate. */
BufioReadWriter bufio_new_read_writer(BufioReader *r, BufioWriter *w);

Int bufio_read_writer_read(BufioReadWriter rw, Slice p, Error *err);
Byte bufio_read_writer_read_byte(BufioReadWriter rw, Error *err);
BURROW_STATIC(ret) Error bufio_read_writer_unread_byte(BufioReadWriter rw);
Rune bufio_read_writer_read_rune(BufioReadWriter rw, Int *size, Error *err);
BURROW_STATIC(ret) Error bufio_read_writer_unread_rune(BufioReadWriter rw);
BURROW_BORROWS(ret, rw) Slice bufio_read_writer_peek(BufioReadWriter rw, Int n,
                                                     Error *err);
Int bufio_read_writer_discard(BufioReadWriter rw, Int n, Error *err);
BURROW_BORROWS(ret, rw) Slice bufio_read_writer_read_slice(BufioReadWriter rw,
                                                           Byte delim, Error *err);
BURROW_BORROWS(ret, rw) Slice bufio_read_writer_read_line(BufioReadWriter rw,
                                                          bool *is_prefix, Error *err);
BURROW_OWNS(ret) Slice bufio_read_writer_read_bytes(BufioReadWriter rw, Alloc *a,
                                                    Byte delim, Error *err);
BURROW_OWNS(ret) Str bufio_read_writer_read_string(BufioReadWriter rw, Alloc *a,
                                                   Byte delim, Error *err);
int64_t bufio_read_writer_write_to(BufioReadWriter rw, IoWriter w, Error *err);

Int bufio_read_writer_write(BufioReadWriter rw, Slice p, Error *err);
BURROW_STATIC(ret) Error bufio_read_writer_write_byte(BufioReadWriter rw, Byte c);
Int bufio_read_writer_write_rune(BufioReadWriter rw, Rune r, Error *err);
Int bufio_read_writer_write_string(BufioReadWriter rw, Str s, Error *err);
int64_t bufio_read_writer_read_from(BufioReadWriter rw, IoReader r, Error *err);
BURROW_STATIC(ret) Error bufio_read_writer_flush(BufioReadWriter rw);
Int bufio_read_writer_available(BufioReadWriter rw);
BURROW_BORROWS(ret, rw) Slice bufio_read_writer_available_buffer(BufioReadWriter rw);

IoReader bufio_read_writer_as_io_reader(BufioReadWriter *rw);
IoWriter bufio_read_writer_as_io_writer(BufioReadWriter *rw);
IoReadWriter bufio_read_writer_as_io_read_writer(BufioReadWriter *rw);

extern const Type *const TYPE_BUFIO_READ_WRITER;

/* ----------------------------------------------------------------- Scanner
 *
 * bufio.SplitFunc. Given the unread input, and whether the reader has ended,
 * returns how many bytes to move past and sets *token to the next token, or
 * leaves it nil to ask for more input. A token that is non-nil but empty is a
 * real, empty token. An error stops the scan, except bufio_err_final_token,
 * which ends it cleanly after the token that came with it.
 *
 * *token and *err come in nil, so a split function only sets what it has. */
BURROW_FUNC(BufioSplitFunc, Int, Slice data, bool at_eof, Slice *token, Error *err);

/* bufio.ScanBytes, ScanRunes, ScanLines and ScanWords as plain functions, for
 * calling from a split function of your own. */
Int bufio_scan_bytes(Slice data, bool at_eof, Slice *token, Error *err);
Int bufio_scan_runes(Slice data, bool at_eof, Slice *token, Error *err);
Int bufio_scan_lines(Slice data, bool at_eof, Slice *token, Error *err);
Int bufio_scan_words(Slice data, bool at_eof, Slice *token, Error *err);

/* The same four as split function values, for bufio_scanner_split. */
extern const BufioSplitFunc BUFIO_SCAN_BYTES;
extern const BufioSplitFunc BUFIO_SCAN_RUNES;
extern const BufioSplitFunc BUFIO_SCAN_LINES;
extern const BufioSplitFunc BUFIO_SCAN_WORDS;

/* bufio.Scanner. */
typedef struct BufioScanner {
    Alloc *a;
    IoReader r;
    BufioSplitFunc split;
    Int max_token_size;
    Slice token; /* the last token, or nil */
    Byte *buf;
    Int buf_size;
    bool own_buf; /* buf came from a and is freed with the scanner */
    Int start;    /* the first byte of buf not yet handed to split */
    Int end;      /* the end of the data in buf */
    Error err;
    Int empties; /* empty tokens in a row */
    bool scan_called;
    bool done;
} BufioScanner;

/* bufio.NewScanner. Splits on lines until told otherwise. NULL when the
 * allocator refuses. */
BURROW_OWNS(ret) BufioScanner *bufio_new_scanner(Alloc *a, IoReader r);
void bufio_scanner_free(BufioScanner *s);

/* bufio.Scanner.Scan. Moves to the next token and says whether there is one.
 * False means the input ended or an error came up, and bufio_scanner_err
 * says which. */
bool bufio_scanner_scan(BufioScanner *s);

/* bufio.Scanner.Bytes and Text. The current token, pointing into the
 * scanner's buffer and good until the next scan. Go's Text makes a copy.
 * This one does not, so copy it yourself if you keep it. */
BURROW_BORROWS(ret, s) Slice bufio_scanner_bytes(BufioScanner *s);
BURROW_BORROWS(ret, s) Str bufio_scanner_text(BufioScanner *s);

/* bufio.Scanner.Err. The error that stopped the scan, and nil if it was the
 * end of the input. */
BURROW_BORROWS(ret, s) Error bufio_scanner_err(BufioScanner *s);

/* bufio.Scanner.Buffer. Scans in buf to begin with and in larger buffers up
 * to max bytes after that. buf stays yours. Panics once scanning has begun. */
void bufio_scanner_buffer(BufioScanner *s, Slice buf, Int max);

/* bufio.Scanner.Split. Panics once scanning has begun. */
void bufio_scanner_split(BufioScanner *s, BufioSplitFunc split);

/* The space test ScanWords uses, which is its own and not unicode_is_space.
 * Exported for the tests, as Go does in export_test.go. */
bool burrow__bufio_is_space(Rune r);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_BUFIO_H */
