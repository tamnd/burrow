/* Derived from Go's src/net/http/internal/chunked.go, the wire format of
 * HTTP's chunked Transfer-Encoding.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* maxLineLength, which has to be no more than bufio's default size. */
#define HK_MAX_LINE_LENGTH 4096

/* The text is shared so that httputil's ErrLineTooLong can be this same error,
 * as it is in Go. */
const Str burrow__http_line_too_long_text = BURROW_S_INIT("header line too long");
const Error burrow__http_err_line_too_long = {&burrow_sentinel_error_vt,
                                              &burrow__http_line_too_long_text};

/* The errors Go makes with errors.New where they happen. */
static const Str hk_text_too_much =
    BURROW_S_INIT("chunked encoding contains too much non-data");
static const Str hk_text_malformed = BURROW_S_INIT("malformed chunked encoding");
static const Str hk_text_bare_lf = BURROW_S_INIT("chunked line ends with bare LF");
static const Str hk_text_invalid_cr = BURROW_S_INIT("invalid CR in chunked line");
static const Str hk_text_empty_hex = BURROW_S_INIT("empty hex number for chunk length");
static const Str hk_text_invalid_byte = BURROW_S_INIT("invalid byte in chunk length");
static const Str hk_text_too_large = BURROW_S_INIT("http chunk length too large");

static Error hk_error(const Str *text) {
    return (Error){&burrow_sentinel_error_vt, text};
}

static bool hk_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* --------------------------------------------------------------- the reader */

struct HttpChunkedReader {
    BufioReader *r;
    Error err;
    uint64_t n;     /* unread bytes in the chunk */
    int64_t excess; /* chunk overhead beyond what is fair, to catch a sender
                     * who sends little but extensions */
    Byte buf[2];
    bool check_end; /* the \r\n after a chunk's data is still to come */
};

uint64_t burrow__http_parse_hex_uint(Slice v, Error *err) {
    const Byte *p = (const Byte *)v.p;
    uint64_t n = 0;
    if (v.len == 0) {
        BURROW_OUT(err, hk_error(&hk_text_empty_hex));
        return 0;
    }
    for (Int i = 0; i < v.len; i++) {
        Byte b = p[i];
        if ('0' <= b && b <= '9') {
            b = (Byte)(b - '0');
        } else if ('a' <= b && b <= 'f') {
            b = (Byte)(b - 'a' + 10);
        } else if ('A' <= b && b <= 'F') {
            b = (Byte)(b - 'A' + 10);
        } else {
            BURROW_OUT(err, hk_error(&hk_text_invalid_byte));
            return 0;
        }
        if (i == 16) {
            BURROW_OUT(err, hk_error(&hk_text_too_large));
            return 0;
        }
        n <<= 4;
        n |= (uint64_t)b;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

/* readChunkLine. A line up to its \r\n, without them, pointing into the
 * bufio buffer and good until the next read. Too long a line is an error, and
 * so is one that ends in a bare \n or has a \r before its end. */
static Slice hk_read_line(BufioReader *b, Error *err) {
    Slice none = {0};
    Error e;
    Slice p = bufio_reader_read_slice(b, '\n', &e);
    if (BURROW_FAILED(e)) {
        /* The caller asked for a line, and EOF always comes where it is
         * expected, so there should have been one. */
        if (hk_same_error(e, io_eof))
            e = io_err_unexpected_eof;
        else if (hk_same_error(e, bufio_err_buffer_full))
            e = burrow__http_err_line_too_long;
        *err = e;
        return none;
    }
    /* RFC 9112 lets a parser take a bare \n as the end of a header line, but
     * not of a chunk line, and erratum 7633 says so. */
    const Byte *cr = (const Byte *)memchr(p.p, '\r', (size_t)p.len);
    if (cr == NULL) {
        *err = hk_error(&hk_text_bare_lf);
        return none;
    }
    if (cr - (const Byte *)p.p != p.len - 2) {
        *err = hk_error(&hk_text_invalid_cr);
        return none;
    }
    p.len -= 2;
    if (p.len >= HK_MAX_LINE_LENGTH) {
        *err = burrow__http_err_line_too_long;
        return none;
    }
    *err = BURROW_NO_ERROR;
    return p;
}

static bool hk_is_ows(Byte b) {
    return b == ' ' || b == '\t';
}

static void hk_begin_chunk(HttpChunkedReader *cr) {
    /* chunk-size CRLF */
    Slice line = hk_read_line(cr->r, &cr->err);
    if (BURROW_FAILED(cr->err))
        return;
    cr->excess += (int64_t)line.len + 2; /* the line, and the \r\n after the data */
    const Byte *p = (const Byte *)line.p;
    while (line.len > 0 && hk_is_ows(p[line.len - 1]))
        line.len--;
    /* removeChunkExtension. Go drops an extension without looking at it. */
    const Byte *semi =
        line.len > 0 ? (const Byte *)memchr(p, ';', (size_t)line.len) : NULL;
    if (semi != NULL)
        line.len = (Int)(semi - p);
    cr->n = burrow__http_parse_hex_uint(line, &cr->err);
    if (BURROW_FAILED(cr->err))
        return;
    /* One byte a chunk is five bytes of overhead a byte, "1\r\nX\r\n", and
     * that is allowed, since streaming a byte at a time can be fair. An
     * extension can add any amount more, and that is not. So up to 16 bytes
     * of overhead a chunk are fine, plus twice the data in it, and more than
     * 16 KiB beyond that is an error. */
    cr->excess -= 16 + 2 * (int64_t)cr->n;
    if (cr->excess < 0)
        cr->excess = 0;
    if (cr->excess > (int64_t)16 * 1024)
        cr->err = hk_error(&hk_text_too_much);
    if (cr->n == 0)
        cr->err = io_eof;
}

/* Whether a whole chunk header is in the buffer already. */
static bool hk_header_available(HttpChunkedReader *cr) {
    Int n = bufio_reader_buffered(cr->r);
    if (n > 0) {
        Error e;
        Slice peek = bufio_reader_peek(cr->r, n, &e);
        return memchr(peek.p, '\n', (size_t)peek.len) != NULL;
    }
    return false;
}

Int burrow__http_chunked_reader_read(HttpChunkedReader *cr, Slice b, Error *err) {
    Int n = 0;
    while (BURROW_OK(cr->err)) {
        if (cr->check_end) {
            if (n > 0 && bufio_reader_buffered(cr->r) < 2) {
                /* There is data to give back, so give it rather than
                 * perhaps block reading more, as io.Reader says to. */
                break;
            }
            (void)io_read_full(bufio_reader_as_io_reader(cr->r),
                               slice_from(cr->buf, 2, 2, TYPE_BYTE), &cr->err);
            if (BURROW_OK(cr->err)) {
                if (cr->buf[0] != '\r' || cr->buf[1] != '\n') {
                    cr->err = hk_error(&hk_text_malformed);
                    break;
                }
            } else {
                if (hk_same_error(cr->err, io_eof))
                    cr->err = io_err_unexpected_eof;
                break;
            }
            cr->check_end = false;
        }
        if (cr->n == 0) {
            if (n > 0 && !hk_header_available(cr)) {
                /* Enough has been read, and the next header might block. */
                break;
            }
            hk_begin_chunk(cr);
            continue;
        }
        if (b.len == 0)
            break;
        Slice rbuf = b;
        if ((uint64_t)rbuf.len > cr->n)
            rbuf.len = (Int)cr->n;
        Int n0 = bufio_reader_read(cr->r, rbuf, &cr->err);
        n += n0;
        b = slice_sub(b, n0, b.len);
        cr->n -= (uint64_t)n0;
        /* At the end of a chunk, the next two bytes have to be \r\n. */
        if (cr->n == 0 && BURROW_OK(cr->err))
            cr->check_end = true;
        else if (hk_same_error(cr->err, io_eof))
            cr->err = io_err_unexpected_eof;
    }
    *err = cr->err;
    return n;
}

static Int hk_reader_read(void *self, Slice p, Error *err) {
    return burrow__http_chunked_reader_read((HttpChunkedReader *)self, p, err);
}

static const IoReaderVT hk_reader_vt = {NULL, hk_reader_read};

HttpChunkedReader *burrow__http_new_chunked_reader(Alloc *a, IoReader r) {
    HttpChunkedReader *cr =
        (HttpChunkedReader *)mem_alloc(a, sizeof *cr, _Alignof(HttpChunkedReader));
    if (cr == NULL)
        return NULL;
    /* A BufioReader is used as it is, whatever its size, as Go's type
     * assertion does, and 16 is the smallest buffer bufio makes. Anything
     * else gets a reader of Go's default size. */
    BufioReader probe = {0};
    if (r.vt == bufio_reader_as_io_reader(&probe).vt)
        cr->r = bufio_new_reader_size(a, r, 16);
    else
        cr->r = bufio_new_reader(a, r);
    if (cr->r == NULL) {
        mem_free(a, cr, sizeof *cr, _Alignof(HttpChunkedReader));
        return NULL;
    }
    return cr;
}

void burrow__http_chunked_reader_free(Alloc *a, HttpChunkedReader *cr) {
    if (cr == NULL)
        return;
    bufio_reader_free(cr->r);
    mem_free(a, cr, sizeof *cr, _Alignof(HttpChunkedReader));
}

IoReader burrow__http_chunked_reader_as_io_reader(HttpChunkedReader *cr) {
    return (IoReader){&hk_reader_vt, cr};
}

/* --------------------------------------------------------------- the writer */

Int burrow__http_chunked_writer_write(HttpChunkedWriter *cw, Slice data, Error *err) {
    /* A chunk of no bytes would read as the end of the stream. */
    if (data.len == 0) {
        *err = BURROW_NO_ERROR;
        return 0;
    }
    Byte tmp[24];
    Slice head = strconv_append_uint(NULL, slice_from(tmp, 0, 20, TYPE_BYTE),
                                     (uint64_t)data.len, 16);
    ((Byte *)head.p)[head.len++] = '\r';
    ((Byte *)head.p)[head.len++] = '\n';
    (void)cw->wire.vt->write(cw->wire.data, head, err);
    if (BURROW_FAILED(*err))
        return 0;
    Int n = cw->wire.vt->write(cw->wire.data, data, err);
    if (BURROW_FAILED(*err))
        return n;
    if (n != data.len) {
        *err = io_err_short_write;
        return n;
    }
    (void)io_write_string(cw->wire, BURROW_S("\r\n"), err);
    if (BURROW_FAILED(*err))
        return n;
    if (cw->flush != NULL)
        *err = bufio_writer_flush(cw->flush);
    return n;
}

Error burrow__http_chunked_writer_close(HttpChunkedWriter *cw) {
    Error err;
    (void)io_write_string(cw->wire, BURROW_S("0\r\n"), &err);
    return err;
}

static Int hk_writer_write(void *self, Slice p, Error *err) {
    return burrow__http_chunked_writer_write((HttpChunkedWriter *)self, p, err);
}

static Error hk_writer_close(void *self) {
    return burrow__http_chunked_writer_close((HttpChunkedWriter *)self);
}

static const IoWriteCloserVT hk_writer_vt = {{NULL, hk_writer_write},
                                             {NULL, hk_writer_close}};

IoWriteCloser burrow__http_chunked_writer_as_io_write_closer(HttpChunkedWriter *cw) {
    return (IoWriteCloser){&hk_writer_vt, cw};
}
