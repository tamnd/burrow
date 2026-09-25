/* Derived from Go's src/bufio/bufio.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/bufio.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <string.h>

enum {
    BUFIO_DEFAULT_BUF_SIZE = 4096,
    BUFIO_MIN_READ_BUFFER_SIZE = 16,
    BUFIO_MAX_CONSECUTIVE_EMPTY_READS = 100,
};

BURROW_SENTINEL_ERROR(bufio_err_invalid_unread_byte,
                      "bufio: invalid use of UnreadByte");
BURROW_SENTINEL_ERROR(bufio_err_invalid_unread_rune,
                      "bufio: invalid use of UnreadRune");
BURROW_SENTINEL_ERROR(bufio_err_buffer_full, "bufio: buffer full");
BURROW_SENTINEL_ERROR(bufio_err_negative_count, "bufio: negative count");

static bool bufio_same_error(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static void bufio_negative_read(void) {
    runtime_panic(BURROW_S("bufio: reader returned negative count from Read"));
}

static void bufio_negative_write(void) {
    runtime_panic(BURROW_S("bufio: writer returned negative count from Write"));
}

static Slice bufio_bytes_of(Byte *p, Int len) {
    return slice_from(p, len, len, TYPE_BYTE);
}

/* ------------------------------------------------------------------ Reader */

#define BUFIO_READER_METHODS(M, T) M(T, WriteTo, bufio_reader_write_to, IO_SIG_WRITE_TO)
BURROW_METHODS_DEFINE(BufioReader, BUFIO_READER_METHODS);

static const Type bufio_reader_desc = {
    {(const Byte *)"Reader", 6},
    {(const Byte *)"bufio", 5},
    KIND_STRUCT,
    (uint32_t)sizeof(BufioReader),
    (uint16_t)_Alignof(BufioReader),
    0,
    (uint16_t)(sizeof burrow__methods_BufioReader /
               sizeof burrow__methods_BufioReader[0]),
    NULL,
    burrow__methods_BufioReader,
    NULL,
    NULL,
    0,
    0x62667264U, /* "bfrd" */
    NULL,
};

const Type *const TYPE_BUFIO_READER = &bufio_reader_desc;

static Int bufio_reader_io_read(void *self, Slice p, Error *err) {
    return bufio_reader_read((BufioReader *)self, p, err);
}

static const IoReaderVT bufio_reader_reader_vt = {&bufio_reader_desc,
                                                  bufio_reader_io_read};

static void bufio_reader_init(BufioReader *b, Byte *buf, Int size, IoReader r) {
    b->buf = buf;
    b->size = size;
    b->rd = r;
    b->r = 0;
    b->w = 0;
    b->err = BURROW_NO_ERROR;
    b->last_byte = -1;
    b->last_rune_size = -1;
}

BufioReader *bufio_new_reader_size(Alloc *a, IoReader rd, Int size) {
    if (rd.vt == &bufio_reader_reader_vt) {
        BufioReader *b = (BufioReader *)rd.data;
        if (b->size >= size) {
            b->refs++;
            return b;
        }
    }
    if (size < BUFIO_MIN_READ_BUFFER_SIZE)
        size = BUFIO_MIN_READ_BUFFER_SIZE;
    BufioReader *r = (BufioReader *)mem_alloc(a, sizeof *r, _Alignof(BufioReader));
    if (r == NULL)
        return NULL;
    Byte *buf = (Byte *)mem_alloc_nozero(a, (size_t)size, 1);
    if (buf == NULL) {
        mem_free(a, r, sizeof *r, _Alignof(BufioReader));
        return NULL;
    }
    r->a = a;
    r->refs = 1;
    r->own = true;
    bufio_reader_init(r, buf, size, rd);
    return r;
}

BufioReader *bufio_new_reader(Alloc *a, IoReader rd) {
    return bufio_new_reader_size(a, rd, BUFIO_DEFAULT_BUF_SIZE);
}

void bufio_reader_free(BufioReader *b) {
    if (b == NULL)
        return;
    if (b->refs > (b->own ? 1 : 0)) {
        b->refs--;
        return;
    }
    if (b->buf != NULL)
        mem_free(b->a, b->buf, (size_t)b->size, 1);
    if (b->own) {
        mem_free(b->a, b, sizeof *b, _Alignof(BufioReader));
        return;
    }
    /* A zeroed reader of the caller's: the struct is theirs. */
    b->buf = NULL;
    b->size = 0;
}

Int bufio_reader_size(BufioReader *b) {
    return b->size;
}

void bufio_reader_reset(BufioReader *b, IoReader r) {
    /* If a Reader r is passed to NewReader, NewReader will return r. Different
     * layers of code may do that, and then later pass r to Reset. Avoid
     * infinite recursion in that case. */
    if (r.vt == &bufio_reader_reader_vt && r.data == b)
        return;
    if (b->buf == NULL) {
        if (b->a == NULL)
            b->a = heap_allocator();
        b->buf = (Byte *)mem_alloc_nozero(b->a, BUFIO_DEFAULT_BUF_SIZE, 1);
        if (b->buf == NULL)
            runtime_panic(BURROW_S("bufio: out of memory"));
        b->size = BUFIO_DEFAULT_BUF_SIZE;
    }
    bufio_reader_init(b, b->buf, b->size, r);
}

/* Reads a new chunk into the buffer. */
static void bufio_fill(BufioReader *b) {
    /* Slide existing data to beginning. */
    if (b->r > 0) {
        memmove(b->buf, b->buf + b->r, (size_t)(b->w - b->r));
        b->w -= b->r;
        b->r = 0;
    }

    if (b->w >= b->size)
        runtime_panic(BURROW_S("bufio: tried to fill full buffer"));

    /* Read new data: try a limited number of times. */
    for (int i = BUFIO_MAX_CONSECUTIVE_EMPTY_READS; i > 0; i--) {
        Error err = BURROW_NO_ERROR;
        Int n = BURROW_CALL(b->rd, read, bufio_bytes_of(b->buf + b->w, b->size - b->w),
                            &err);
        if (n < 0)
            bufio_negative_read();
        b->w += n;
        if (BURROW_FAILED(err)) {
            b->err = err;
            return;
        }
        if (n > 0)
            return;
    }
    b->err = io_err_no_progress;
}

static Error bufio_read_err(BufioReader *b) {
    Error err = b->err;
    b->err = BURROW_NO_ERROR;
    return err;
}

Int bufio_reader_buffered(BufioReader *b) {
    return b->w - b->r;
}

Slice bufio_reader_peek(BufioReader *b, Int n, Error *err) {
    if (n < 0) {
        BURROW_OUT(err, bufio_err_negative_count);
        return slice_nil(TYPE_BYTE);
    }

    b->last_byte = -1;
    b->last_rune_size = -1;

    while (b->w - b->r < n && b->w - b->r < b->size && BURROW_OK(b->err))
        bufio_fill(b); /* b->w-b->r < b->size => buffer is not full */

    if (n > b->size) {
        BURROW_OUT(err, bufio_err_buffer_full);
        return bufio_bytes_of(b->buf + b->r, b->w - b->r);
    }

    /* 0 <= n <= b->size */
    Error e = BURROW_NO_ERROR;
    Int avail = b->w - b->r;
    if (avail < n) {
        /* not enough data in buffer */
        n = avail;
        e = bufio_read_err(b);
        if (BURROW_OK(e))
            e = bufio_err_buffer_full;
    }
    BURROW_OUT(err, e);
    return bufio_bytes_of(b->buf + b->r, n);
}

Int bufio_reader_discard(BufioReader *b, Int n, Error *err) {
    if (n < 0) {
        BURROW_OUT(err, bufio_err_negative_count);
        return 0;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (n == 0)
        return 0;

    b->last_byte = -1;
    b->last_rune_size = -1;

    Int remain = n;
    for (;;) {
        Int skip = bufio_reader_buffered(b);
        if (skip == 0) {
            bufio_fill(b);
            skip = bufio_reader_buffered(b);
        }
        if (skip > remain)
            skip = remain;
        b->r += skip;
        remain -= skip;
        if (remain == 0)
            return n;
        if (BURROW_FAILED(b->err)) {
            BURROW_OUT(err, bufio_read_err(b));
            return n - remain;
        }
    }
}

Int bufio_reader_read(BufioReader *b, Slice p, Error *err) {
    Int n = p.len;
    if (n == 0) {
        if (bufio_reader_buffered(b) > 0) {
            BURROW_OUT(err, BURROW_NO_ERROR);
            return 0;
        }
        BURROW_OUT(err, bufio_read_err(b));
        return 0;
    }
    if (b->r == b->w) {
        if (BURROW_FAILED(b->err)) {
            BURROW_OUT(err, bufio_read_err(b));
            return 0;
        }
        if (p.len >= b->size) {
            /* Large read, empty buffer. Read directly into p to avoid copy. */
            Error e = BURROW_NO_ERROR;
            n = BURROW_CALL(b->rd, read, p, &e);
            b->err = e;
            if (n < 0)
                bufio_negative_read();
            if (n > 0) {
                b->last_byte = ((const Byte *)p.p)[n - 1];
                b->last_rune_size = -1;
            }
            BURROW_OUT(err, bufio_read_err(b));
            return n;
        }
        /* One read. Do not use fill, which will loop. */
        b->r = 0;
        b->w = 0;
        Error e = BURROW_NO_ERROR;
        n = BURROW_CALL(b->rd, read, bufio_bytes_of(b->buf, b->size), &e);
        b->err = e;
        if (n < 0)
            bufio_negative_read();
        if (n == 0) {
            BURROW_OUT(err, bufio_read_err(b));
            return 0;
        }
        b->w += n;
    }

    /* copy as much as we can. Note: if the slice panics here, it is probably
     * because the underlying reader returned a bad count. See issue 49795. */
    n = b->w - b->r;
    if (n > p.len)
        n = p.len;
    memcpy(p.p, b->buf + b->r, (size_t)n);
    b->r += n;
    b->last_byte = b->buf[b->r - 1];
    b->last_rune_size = -1;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

Byte bufio_reader_read_byte(BufioReader *b, Error *err) {
    b->last_rune_size = -1;
    while (b->r == b->w) {
        if (BURROW_FAILED(b->err)) {
            BURROW_OUT(err, bufio_read_err(b));
            return 0;
        }
        bufio_fill(b); /* buffer is empty */
    }
    Byte c = b->buf[b->r];
    b->r++;
    b->last_byte = c;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

Error bufio_reader_unread_byte(BufioReader *b) {
    if (b->last_byte < 0 || (b->r == 0 && b->w > 0))
        return bufio_err_invalid_unread_byte;
    /* b->r > 0 || b->w == 0 */
    if (b->r > 0)
        b->r--;
    else
        b->w = 1; /* b->r == 0 && b->w == 0 */
    b->buf[b->r] = (Byte)b->last_byte;
    b->last_byte = -1;
    b->last_rune_size = -1;
    return BURROW_NO_ERROR;
}

Rune bufio_reader_read_rune(BufioReader *b, Int *size, Error *err) {
    while (b->r + UTF8_UTF_MAX > b->w &&
           !utf8_full_rune(bufio_bytes_of(b->buf + b->r, b->w - b->r)) &&
           BURROW_OK(b->err) && b->w - b->r < b->size)
        bufio_fill(b); /* b->w-b->r < b->size => buffer is not full */
    b->last_rune_size = -1;
    if (b->r == b->w) {
        BURROW_OUT(size, 0);
        BURROW_OUT(err, bufio_read_err(b));
        return 0;
    }
    Int sz = 0;
    Rune r = utf8_decode_rune(bufio_bytes_of(b->buf + b->r, b->w - b->r), &sz);
    b->r += sz;
    b->last_byte = b->buf[b->r - 1];
    b->last_rune_size = sz;
    BURROW_OUT(size, sz);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return r;
}

Error bufio_reader_unread_rune(BufioReader *b) {
    if (b->last_rune_size < 0 || b->r < b->last_rune_size)
        return bufio_err_invalid_unread_rune;
    b->r -= b->last_rune_size;
    b->last_byte = -1;
    b->last_rune_size = -1;
    return BURROW_NO_ERROR;
}

Slice bufio_reader_read_slice(BufioReader *b, Byte delim, Error *err) {
    Error e = BURROW_NO_ERROR;
    Slice line;
    Int s = 0; /* search start index */
    for (;;) {
        /* Search buffer. */
        const Byte *hit =
            (const Byte *)memchr(b->buf + b->r + s, delim, (size_t)(b->w - b->r - s));
        if (hit != NULL) {
            Int i = (Int)(hit - (b->buf + b->r));
            line = bufio_bytes_of(b->buf + b->r, i + 1);
            b->r += i + 1;
            break;
        }

        /* Pending error? */
        if (BURROW_FAILED(b->err)) {
            line = bufio_bytes_of(b->buf + b->r, b->w - b->r);
            b->r = b->w;
            e = bufio_read_err(b);
            break;
        }

        /* Buffer full? */
        if (bufio_reader_buffered(b) >= b->size) {
            b->r = b->w;
            line = bufio_bytes_of(b->buf, b->size);
            e = bufio_err_buffer_full;
            break;
        }

        s = b->w - b->r; /* do not rescan area we scanned before */

        bufio_fill(b); /* buffer is not full */
    }

    /* Handle last byte, if any. */
    if (line.len > 0) {
        b->last_byte = ((const Byte *)line.p)[line.len - 1];
        b->last_rune_size = -1;
    }

    BURROW_OUT(err, e);
    return line;
}

Slice bufio_reader_read_line(BufioReader *b, bool *is_prefix, Error *err) {
    Error e = BURROW_NO_ERROR;
    Slice line = bufio_reader_read_slice(b, '\n', &e);
    if (bufio_same_error(e, bufio_err_buffer_full)) {
        /* Handle the case where "\r\n" straddles the buffer. */
        if (line.len > 0 && ((const Byte *)line.p)[line.len - 1] == '\r') {
            /* Put the '\r' back on buf and drop it from line. Let the next
             * call to ReadLine check for "\r\n". */
            if (b->r == 0) {
                /* should be unreachable */
                runtime_panic(BURROW_S("bufio: tried to rewind past start of buffer"));
            }
            b->r--;
            line.len--;
            line.cap = line.len;
        }
        BURROW_OUT(is_prefix, true);
        BURROW_OUT(err, BURROW_NO_ERROR);
        return line;
    }

    BURROW_OUT(is_prefix, false);
    if (line.len == 0) {
        if (BURROW_FAILED(e))
            line = slice_nil(TYPE_BYTE);
        BURROW_OUT(err, e);
        return line;
    }

    if (((const Byte *)line.p)[line.len - 1] == '\n') {
        Int drop = 1;
        if (line.len > 1 && ((const Byte *)line.p)[line.len - 2] == '\r')
            drop = 2;
        line.len -= drop;
        line.cap = line.len;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return line;
}

/* Reads until the first occurrence of delim, growing one block from a to hold
 * all of it. Go gathers the full buffers into a list and copies them once at
 * the end. Growing in place does the same number of copies in the common case
 * of a line that fits, and one realloc per doubling otherwise. On success the
 * block is exactly *len bytes long. */
static Byte *bufio_collect(BufioReader *b, Alloc *a, Byte delim, Int *len, Error *err) {
    Byte *out = NULL;
    Int n = 0, cap = 0;
    Error e = BURROW_NO_ERROR;
    for (;;) {
        Error fe = BURROW_NO_ERROR;
        Slice frag = bufio_reader_read_slice(b, delim, &fe);
        if (n + frag.len > cap) {
            Int want = cap == 0 ? frag.len : cap;
            while (want < n + frag.len)
                want *= 2;
            Byte *grown = (Byte *)mem_realloc(a, out, (size_t)cap, (size_t)want, 1);
            if (grown == NULL) {
                if (out != NULL)
                    mem_free(a, out, (size_t)cap, 1);
                *len = 0;
                *err = burrow_err_out_of_memory;
                return NULL;
            }
            out = grown;
            cap = want;
        }
        if (frag.len > 0 && out != NULL)
            memcpy(out + n, frag.p, (size_t)frag.len);
        n += frag.len;
        if (BURROW_OK(fe)) /* got final fragment */
            break;
        if (!bufio_same_error(fe, bufio_err_buffer_full)) { /* unexpected error */
            e = fe;
            break;
        }
    }
    if (n < cap) {
        Byte *exact = (Byte *)mem_realloc(a, out, (size_t)cap, (size_t)n, 1);
        if (exact != NULL || n == 0)
            out = exact;
        else
            n = cap; /* keep the bigger block rather than lose the bytes */
    }
    *len = n;
    *err = e;
    return out;
}

Slice bufio_reader_read_bytes(BufioReader *b, Alloc *a, Byte delim, Error *err) {
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    Byte *p = bufio_collect(b, a, delim, &n, &e);
    BURROW_OUT(err, e);
    if (p == NULL)
        return slice_nil(TYPE_BYTE);
    return bufio_bytes_of(p, n);
}

Str bufio_reader_read_string(BufioReader *b, Alloc *a, Byte delim, Error *err) {
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    Byte *p = bufio_collect(b, a, delim, &n, &e);
    BURROW_OUT(err, e);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    return str_from_bytes(p, n);
}

/* Writes the buffer to w. */
static int64_t bufio_write_buf(BufioReader *b, IoWriter w, Error *err) {
    Int n = BURROW_CALL(w, write, bufio_bytes_of(b->buf + b->r, b->w - b->r), err);
    if (n < 0)
        bufio_negative_write();
    b->r += n;
    return (int64_t)n;
}

int64_t bufio_reader_write_to(BufioReader *b, IoWriter w, Error *err) {
    b->last_byte = -1;
    b->last_rune_size = -1;

    int64_t n = 0;
    Error e = BURROW_NO_ERROR;
    if (b->r < b->w) {
        n = bufio_write_buf(b, w, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return n;
        }
    }

    int64_t m = 0;
    if (burrow__io_try_write_to(b->rd, w, &m, &e)) {
        BURROW_OUT(err, e);
        return n + m;
    }

    if (burrow__io_try_read_from(w, b->rd, &m, &e)) {
        BURROW_OUT(err, e);
        return n + m;
    }

    if (b->w - b->r < b->size)
        bufio_fill(b); /* buffer not full */

    while (b->r < b->w) {
        /* b->r < b->w => buffer is not empty */
        m = bufio_write_buf(b, w, &e);
        n += m;
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return n;
        }
        bufio_fill(b); /* buffer is empty */
    }

    if (bufio_same_error(b->err, io_eof))
        b->err = BURROW_NO_ERROR;

    BURROW_OUT(err, bufio_read_err(b));
    return n;
}

static Byte bufio_reader_io_read_byte(void *self, Error *err) {
    return bufio_reader_read_byte((BufioReader *)self, err);
}

static Error bufio_reader_io_unread_byte(void *self) {
    return bufio_reader_unread_byte((BufioReader *)self);
}

static Rune bufio_reader_io_read_rune(void *self, Int *size, Error *err) {
    return bufio_reader_read_rune((BufioReader *)self, size, err);
}

static Error bufio_reader_io_unread_rune(void *self) {
    return bufio_reader_unread_rune((BufioReader *)self);
}

static int64_t bufio_reader_io_write_to(void *self, IoWriter w, Error *err) {
    return bufio_reader_write_to((BufioReader *)self, w, err);
}

static const IoByteScannerVT bufio_reader_byte_scanner_vt = {
    {&bufio_reader_desc, bufio_reader_io_read_byte}, bufio_reader_io_unread_byte};
static const IoRuneScannerVT bufio_reader_rune_scanner_vt = {
    {&bufio_reader_desc, bufio_reader_io_read_rune}, bufio_reader_io_unread_rune};
static const IoWriterToVT bufio_reader_writer_to_vt = {&bufio_reader_desc,
                                                       bufio_reader_io_write_to};

IoReader bufio_reader_as_io_reader(BufioReader *b) {
    IoReader r = {&bufio_reader_reader_vt, b};
    return r;
}

IoByteReader bufio_reader_as_io_byte_reader(BufioReader *b) {
    IoByteReader r = {&bufio_reader_byte_scanner_vt.byte_reader, b};
    return r;
}

IoByteScanner bufio_reader_as_io_byte_scanner(BufioReader *b) {
    IoByteScanner r = {&bufio_reader_byte_scanner_vt, b};
    return r;
}

IoRuneReader bufio_reader_as_io_rune_reader(BufioReader *b) {
    IoRuneReader r = {&bufio_reader_rune_scanner_vt.rune_reader, b};
    return r;
}

IoRuneScanner bufio_reader_as_io_rune_scanner(BufioReader *b) {
    IoRuneScanner r = {&bufio_reader_rune_scanner_vt, b};
    return r;
}

IoWriterTo bufio_reader_as_io_writer_to(BufioReader *b) {
    IoWriterTo r = {&bufio_reader_writer_to_vt, b};
    return r;
}

/* ------------------------------------------------------------------ Writer */

#define BUFIO_WRITER_METHODS(M, T)                                                     \
    M(T, ReadFrom, bufio_writer_read_from, IO_SIG_READ_FROM)                           \
    M(T, WriteString, bufio_writer_write_string, IO_SIG_WRITE_STRING)
BURROW_METHODS_DEFINE(BufioWriter, BUFIO_WRITER_METHODS);

static const Type bufio_writer_desc = {
    {(const Byte *)"Writer", 6},
    {(const Byte *)"bufio", 5},
    KIND_STRUCT,
    (uint32_t)sizeof(BufioWriter),
    (uint16_t)_Alignof(BufioWriter),
    0,
    (uint16_t)(sizeof burrow__methods_BufioWriter /
               sizeof burrow__methods_BufioWriter[0]),
    NULL,
    burrow__methods_BufioWriter,
    NULL,
    NULL,
    0,
    0x62667772U, /* "bfwr" */
    NULL,
};

const Type *const TYPE_BUFIO_WRITER = &bufio_writer_desc;

static Int bufio_writer_io_write(void *self, Slice p, Error *err) {
    return bufio_writer_write((BufioWriter *)self, p, err);
}

static const IoWriterVT bufio_writer_writer_vt = {&bufio_writer_desc,
                                                  bufio_writer_io_write};

BufioWriter *bufio_new_writer_size(Alloc *a, IoWriter w, Int size) {
    /* Is it already a Writer? */
    if (w.vt == &bufio_writer_writer_vt) {
        BufioWriter *b = (BufioWriter *)w.data;
        if (b->size >= size) {
            b->refs++;
            return b;
        }
    }
    if (size <= 0)
        size = BUFIO_DEFAULT_BUF_SIZE;
    BufioWriter *b = (BufioWriter *)mem_alloc(a, sizeof *b, _Alignof(BufioWriter));
    if (b == NULL)
        return NULL;
    Byte *buf = (Byte *)mem_alloc_nozero(a, (size_t)size, 1);
    if (buf == NULL) {
        mem_free(a, b, sizeof *b, _Alignof(BufioWriter));
        return NULL;
    }
    b->a = a;
    b->err = BURROW_NO_ERROR;
    b->buf = buf;
    b->size = size;
    b->n = 0;
    b->wr = w;
    b->refs = 1;
    b->own = true;
    return b;
}

BufioWriter *bufio_new_writer(Alloc *a, IoWriter w) {
    return bufio_new_writer_size(a, w, BUFIO_DEFAULT_BUF_SIZE);
}

void bufio_writer_free(BufioWriter *b) {
    if (b == NULL)
        return;
    if (b->refs > (b->own ? 1 : 0)) {
        b->refs--;
        return;
    }
    if (b->buf != NULL)
        mem_free(b->a, b->buf, (size_t)b->size, 1);
    if (b->own) {
        mem_free(b->a, b, sizeof *b, _Alignof(BufioWriter));
        return;
    }
    /* A zeroed writer of the caller's: the struct is theirs. */
    b->buf = NULL;
    b->size = 0;
}

Int bufio_writer_size(BufioWriter *b) {
    return b->size;
}

void bufio_writer_reset(BufioWriter *b, IoWriter w) {
    /* If a Writer w is passed to NewWriter, NewWriter will return w. Different
     * layers of code may do that, and then later pass w to Reset. Avoid
     * infinite recursion in that case. */
    if (w.vt == &bufio_writer_writer_vt && w.data == b)
        return;
    if (b->buf == NULL) {
        if (b->a == NULL)
            b->a = heap_allocator();
        b->buf = (Byte *)mem_alloc_nozero(b->a, BUFIO_DEFAULT_BUF_SIZE, 1);
        if (b->buf == NULL)
            runtime_panic(BURROW_S("bufio: out of memory"));
        b->size = BUFIO_DEFAULT_BUF_SIZE;
    }
    b->err = BURROW_NO_ERROR;
    b->n = 0;
    b->wr = w;
}

Error bufio_writer_flush(BufioWriter *b) {
    if (BURROW_FAILED(b->err))
        return b->err;
    if (b->n == 0)
        return BURROW_NO_ERROR;
    Error err = BURROW_NO_ERROR;
    Int n = BURROW_CALL(b->wr, write, bufio_bytes_of(b->buf, b->n), &err);
    if (n < b->n && BURROW_OK(err))
        err = io_err_short_write;
    if (BURROW_FAILED(err)) {
        if (n > 0 && n < b->n)
            memmove(b->buf, b->buf + n, (size_t)(b->n - n));
        b->n -= n;
        b->err = err;
        return err;
    }
    b->n = 0;
    return BURROW_NO_ERROR;
}

Int bufio_writer_available(BufioWriter *b) {
    return b->size - b->n;
}

Slice bufio_writer_available_buffer(BufioWriter *b) {
    return slice_from(b->buf + b->n, 0, b->size - b->n, TYPE_BYTE);
}

Int bufio_writer_buffered(BufioWriter *b) {
    return b->n;
}

static Int bufio_copy_in(BufioWriter *b, const void *p, Int len) {
    Int n = b->size - b->n;
    if (n > len)
        n = len;
    /* p may be the free part of the buffer itself, from AvailableBuffer. */
    if (n > 0 && (const Byte *)p != b->buf + b->n)
        memmove(b->buf + b->n, p, (size_t)n);
    b->n += n;
    return n;
}

Int bufio_writer_write(BufioWriter *b, Slice p, Error *err) {
    Int nn = 0;
    const Byte *src = (const Byte *)p.p;
    Int len = p.len;
    while (len > bufio_writer_available(b) && BURROW_OK(b->err)) {
        Int n;
        if (bufio_writer_buffered(b) == 0) {
            /* Large write, empty buffer. Write directly from p to avoid copy. */
            Error e = BURROW_NO_ERROR;
            n = BURROW_CALL(b->wr, write,
                            slice_from((void *)(uintptr_t)src, len, len, TYPE_BYTE),
                            &e);
            b->err = e;
        } else {
            n = bufio_copy_in(b, src, len);
            (void)bufio_writer_flush(b);
        }
        nn += n;
        src += n;
        len -= n;
    }
    if (BURROW_FAILED(b->err)) {
        BURROW_OUT(err, b->err);
        return nn;
    }
    nn += bufio_copy_in(b, src, len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return nn;
}

Error bufio_writer_write_byte(BufioWriter *b, Byte c) {
    if (BURROW_FAILED(b->err))
        return b->err;
    if (bufio_writer_available(b) <= 0 && BURROW_FAILED(bufio_writer_flush(b)))
        return b->err;
    b->buf[b->n] = c;
    b->n++;
    return BURROW_NO_ERROR;
}

Int bufio_writer_write_rune(BufioWriter *b, Rune r, Error *err) {
    /* Compare as uint32 to correctly handle negative runes. */
    if ((uint32_t)r < UTF8_RUNE_SELF) {
        Error e = bufio_writer_write_byte(b, (Byte)r);
        BURROW_OUT(err, e);
        return BURROW_FAILED(e) ? 0 : 1;
    }
    if (BURROW_FAILED(b->err)) {
        BURROW_OUT(err, b->err);
        return 0;
    }
    Int n = bufio_writer_available(b);
    if (n < UTF8_UTF_MAX) {
        if (BURROW_FAILED(bufio_writer_flush(b))) {
            BURROW_OUT(err, b->err);
            return 0;
        }
        n = bufio_writer_available(b);
        if (n < UTF8_UTF_MAX) {
            /* Can only happen if buffer is silly small. */
            Byte tmp[UTF8_UTF_MAX];
            Int k = utf8_encode_rune(bufio_bytes_of(tmp, UTF8_UTF_MAX), r);
            return bufio_writer_write_string(b, str_from_bytes(tmp, k), err);
        }
    }
    Int size = utf8_encode_rune(bufio_bytes_of(b->buf + b->n, b->size - b->n), r);
    b->n += size;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return size;
}

Int bufio_writer_write_string(BufioWriter *b, Str s, Error *err) {
    bool try_string_writer = true;
    Int nn = 0;
    while (s.len > bufio_writer_available(b) && BURROW_OK(b->err)) {
        Int n = 0;
        Error e = BURROW_NO_ERROR;
        if (bufio_writer_buffered(b) == 0 && try_string_writer &&
            burrow__io_try_write_string(b->wr, s, &n, &e)) {
            /* Large write, empty buffer, and the underlying writer supports
             * WriteString: forward the write to the underlying StringWriter.
             * This avoids an extra copy. */
            b->err = e;
        } else {
            if (bufio_writer_buffered(b) == 0)
                try_string_writer = false;
            n = bufio_copy_in(b, s.p, s.len);
            (void)bufio_writer_flush(b);
        }
        nn += n;
        s.p += n;
        s.len -= n;
    }
    if (BURROW_FAILED(b->err)) {
        BURROW_OUT(err, b->err);
        return nn;
    }
    nn += bufio_copy_in(b, s.p, s.len);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return nn;
}

int64_t bufio_writer_read_from(BufioWriter *b, IoReader r, Error *err) {
    if (BURROW_FAILED(b->err)) {
        BURROW_OUT(err, b->err);
        return 0;
    }
    bool reader_from_ok = true;
    int64_t n = 0;
    Error e = BURROW_NO_ERROR;
    for (;;) {
        if (bufio_writer_available(b) == 0) {
            Error e1 = bufio_writer_flush(b);
            if (BURROW_FAILED(e1)) {
                BURROW_OUT(err, e1);
                return n;
            }
        }
        if (reader_from_ok && bufio_writer_buffered(b) == 0) {
            int64_t nn = 0;
            Error re = BURROW_NO_ERROR;
            if (burrow__io_try_read_from(b->wr, r, &nn, &re)) {
                /* Large write, empty buffer. Read directly into the
                 * underlying writer. */
                b->err = re;
                BURROW_OUT(err, re);
                return n + nn;
            }
            reader_from_ok = false;
        }
        Int m = 0;
        int nr = 0;
        while (nr < BUFIO_MAX_CONSECUTIVE_EMPTY_READS) {
            e = BURROW_NO_ERROR;
            m = BURROW_CALL(r, read, bufio_bytes_of(b->buf + b->n, b->size - b->n), &e);
            if (m != 0 || BURROW_FAILED(e))
                break;
            nr++;
        }
        if (nr == BUFIO_MAX_CONSECUTIVE_EMPTY_READS) {
            BURROW_OUT(err, io_err_no_progress);
            return n;
        }
        b->n += m;
        n += (int64_t)m;
        if (BURROW_FAILED(e))
            break;
    }
    if (bufio_same_error(e, io_eof)) {
        /* If we filled the buffer exactly, flush preemptively. */
        if (bufio_writer_available(b) == 0)
            e = bufio_writer_flush(b);
        else
            e = BURROW_NO_ERROR;
    }
    BURROW_OUT(err, e);
    return n;
}

static Error bufio_writer_io_write_byte(void *self, Byte c) {
    return bufio_writer_write_byte((BufioWriter *)self, c);
}

static Int bufio_writer_io_write_string(void *self, Str s, Error *err) {
    return bufio_writer_write_string((BufioWriter *)self, s, err);
}

static int64_t bufio_writer_io_read_from(void *self, IoReader r, Error *err) {
    return bufio_writer_read_from((BufioWriter *)self, r, err);
}

static const IoByteWriterVT bufio_writer_byte_writer_vt = {&bufio_writer_desc,
                                                           bufio_writer_io_write_byte};
static const IoStringWriterVT bufio_writer_string_writer_vt = {
    &bufio_writer_desc, bufio_writer_io_write_string};
static const IoReaderFromVT bufio_writer_reader_from_vt = {&bufio_writer_desc,
                                                           bufio_writer_io_read_from};

IoWriter bufio_writer_as_io_writer(BufioWriter *b) {
    IoWriter w = {&bufio_writer_writer_vt, b};
    return w;
}

IoByteWriter bufio_writer_as_io_byte_writer(BufioWriter *b) {
    IoByteWriter w = {&bufio_writer_byte_writer_vt, b};
    return w;
}

IoStringWriter bufio_writer_as_io_string_writer(BufioWriter *b) {
    IoStringWriter w = {&bufio_writer_string_writer_vt, b};
    return w;
}

IoReaderFrom bufio_writer_as_io_reader_from(BufioWriter *b) {
    IoReaderFrom w = {&bufio_writer_reader_from_vt, b};
    return w;
}

/* -------------------------------------------------------------- ReadWriter */

/* Go's ReadWriter methods take the struct by value. The method set wants a
 * pointer receiver, so these three bridge the two. */
static int64_t bufio_read_writer_m_read_from(BufioReadWriter *rw, IoReader r,
                                             Error *err) {
    return bufio_writer_read_from(rw->writer, r, err);
}

static Int bufio_read_writer_m_write_string(BufioReadWriter *rw, Str s, Error *err) {
    return bufio_writer_write_string(rw->writer, s, err);
}

static int64_t bufio_read_writer_m_write_to(BufioReadWriter *rw, IoWriter w,
                                            Error *err) {
    return bufio_reader_write_to(rw->reader, w, err);
}

#define BUFIO_READ_WRITER_METHODS(M, T)                                                \
    M(T, ReadFrom, bufio_read_writer_m_read_from, IO_SIG_READ_FROM)                    \
    M(T, WriteString, bufio_read_writer_m_write_string, IO_SIG_WRITE_STRING)           \
    M(T, WriteTo, bufio_read_writer_m_write_to, IO_SIG_WRITE_TO)
BURROW_METHODS_DEFINE(BufioReadWriter, BUFIO_READ_WRITER_METHODS);

static const Type bufio_read_writer_desc = {
    {(const Byte *)"ReadWriter", 10},
    {(const Byte *)"bufio", 5},
    KIND_STRUCT,
    (uint32_t)sizeof(BufioReadWriter),
    (uint16_t)_Alignof(BufioReadWriter),
    0,
    (uint16_t)(sizeof burrow__methods_BufioReadWriter /
               sizeof burrow__methods_BufioReadWriter[0]),
    NULL,
    burrow__methods_BufioReadWriter,
    NULL,
    NULL,
    0,
    0x62667277U, /* "bfrw" */
    NULL,
};

const Type *const TYPE_BUFIO_READ_WRITER = &bufio_read_writer_desc;

BufioReadWriter bufio_new_read_writer(BufioReader *r, BufioWriter *w) {
    BufioReadWriter rw = {r, w};
    return rw;
}

Int bufio_read_writer_read(BufioReadWriter rw, Slice p, Error *err) {
    return bufio_reader_read(rw.reader, p, err);
}

Byte bufio_read_writer_read_byte(BufioReadWriter rw, Error *err) {
    return bufio_reader_read_byte(rw.reader, err);
}

Error bufio_read_writer_unread_byte(BufioReadWriter rw) {
    return bufio_reader_unread_byte(rw.reader);
}

Rune bufio_read_writer_read_rune(BufioReadWriter rw, Int *size, Error *err) {
    return bufio_reader_read_rune(rw.reader, size, err);
}

Error bufio_read_writer_unread_rune(BufioReadWriter rw) {
    return bufio_reader_unread_rune(rw.reader);
}

Slice bufio_read_writer_peek(BufioReadWriter rw, Int n, Error *err) {
    return bufio_reader_peek(rw.reader, n, err);
}

Int bufio_read_writer_discard(BufioReadWriter rw, Int n, Error *err) {
    return bufio_reader_discard(rw.reader, n, err);
}

Slice bufio_read_writer_read_slice(BufioReadWriter rw, Byte delim, Error *err) {
    return bufio_reader_read_slice(rw.reader, delim, err);
}

Slice bufio_read_writer_read_line(BufioReadWriter rw, bool *is_prefix, Error *err) {
    return bufio_reader_read_line(rw.reader, is_prefix, err);
}

Slice bufio_read_writer_read_bytes(BufioReadWriter rw, Alloc *a, Byte delim,
                                   Error *err) {
    return bufio_reader_read_bytes(rw.reader, a, delim, err);
}

Str bufio_read_writer_read_string(BufioReadWriter rw, Alloc *a, Byte delim,
                                  Error *err) {
    return bufio_reader_read_string(rw.reader, a, delim, err);
}

int64_t bufio_read_writer_write_to(BufioReadWriter rw, IoWriter w, Error *err) {
    return bufio_reader_write_to(rw.reader, w, err);
}

Int bufio_read_writer_write(BufioReadWriter rw, Slice p, Error *err) {
    return bufio_writer_write(rw.writer, p, err);
}

Error bufio_read_writer_write_byte(BufioReadWriter rw, Byte c) {
    return bufio_writer_write_byte(rw.writer, c);
}

Int bufio_read_writer_write_rune(BufioReadWriter rw, Rune r, Error *err) {
    return bufio_writer_write_rune(rw.writer, r, err);
}

Int bufio_read_writer_write_string(BufioReadWriter rw, Str s, Error *err) {
    return bufio_writer_write_string(rw.writer, s, err);
}

int64_t bufio_read_writer_read_from(BufioReadWriter rw, IoReader r, Error *err) {
    return bufio_writer_read_from(rw.writer, r, err);
}

Error bufio_read_writer_flush(BufioReadWriter rw) {
    return bufio_writer_flush(rw.writer);
}

Int bufio_read_writer_available(BufioReadWriter rw) {
    return bufio_writer_available(rw.writer);
}

Slice bufio_read_writer_available_buffer(BufioReadWriter rw) {
    return bufio_writer_available_buffer(rw.writer);
}

static Int bufio_read_writer_io_read(void *self, Slice p, Error *err) {
    return bufio_read_writer_read(*(BufioReadWriter *)self, p, err);
}

static Int bufio_read_writer_io_write(void *self, Slice p, Error *err) {
    return bufio_read_writer_write(*(BufioReadWriter *)self, p, err);
}

static const IoReadWriterVT bufio_read_writer_vt = {
    {&bufio_read_writer_desc, bufio_read_writer_io_read},
    {&bufio_read_writer_desc, bufio_read_writer_io_write},
};

IoReader bufio_read_writer_as_io_reader(BufioReadWriter *rw) {
    IoReader r = {&bufio_read_writer_vt.reader, rw};
    return r;
}

IoWriter bufio_read_writer_as_io_writer(BufioReadWriter *rw) {
    IoWriter w = {&bufio_read_writer_vt.writer, rw};
    return w;
}

IoReadWriter bufio_read_writer_as_io_read_writer(BufioReadWriter *rw) {
    IoReadWriter r = {&bufio_read_writer_vt, rw};
    return r;
}
