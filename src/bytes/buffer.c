/* Derived from Go's src/bytes/buffer.go.
 * Go source: go1.27.1.
 *
 * Go grows the buffer with append and lets the collector have the old array.
 * Here the buffer frees what it allocated when it moves, and leaves alone the
 * slice bytes_new_buffer was given, which it does not own. Where Go panics with
 * ErrTooLarge the write returns burrow_err_out_of_memory and changes nothing.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/bytes.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <string.h>

/* The methods io asks for by name: io_copy looks for WriteTo and ReadFrom,
 * and io_write_string for WriteString. */
#define BYTES_BUFFER_METHODS(M, T)                                                     \
    M(T, ReadFrom, bytes_buffer_read_from, IO_SIG_READ_FROM)                           \
    M(T, WriteString, bytes_buffer_write_string, IO_SIG_WRITE_STRING)                  \
    M(T, WriteTo, bytes_buffer_write_to, IO_SIG_WRITE_TO)
BURROW_METHODS_DEFINE(BytesBuffer, BYTES_BUFFER_METHODS);

static const Type bytes_buffer_desc = {
    {(const Byte *)"Buffer", 6},
    {(const Byte *)"bytes", 5},
    KIND_STRUCT,
    (uint32_t)sizeof(BytesBuffer),
    (uint16_t)_Alignof(BytesBuffer),
    0,
    (uint16_t)(sizeof burrow__methods_BytesBuffer /
               sizeof burrow__methods_BytesBuffer[0]),
    NULL,
    burrow__methods_BytesBuffer,
    NULL,
    NULL,
    0,
    0x62627566U, /* "bbuf" */
    NULL,
};

const Type *const TYPE_BYTES_BUFFER = &bytes_buffer_desc;

/* The smallest array a buffer allocates, as in Go. */
#define BUFFER_SMALL 64

/* What the last read was. A rune read stores its size, 1 to 4. */
#define BUFFER_OP_READ (-1)
#define BUFFER_OP_INVALID 0

BURROW_SENTINEL_ERROR(bytes_err_too_large, "bytes.Buffer: too large");

#define BUFFER_ERROR(name, text)                                                       \
    static const Str name##__text = {(const Byte *)(text), (Int)(sizeof(text) - 1)};   \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}

BUFFER_ERROR(buffer_err_negative_read,
             "bytes.Buffer: reader returned negative count from Read");
BUFFER_ERROR(
    buffer_err_unread_rune,
    "bytes.Buffer: UnreadRune: previous operation was not a successful ReadRune");
BUFFER_ERROR(buffer_err_unread_byte,
             "bytes.Buffer: UnreadByte: previous operation was not a successful read");

#undef BUFFER_ERROR

static Byte *buffer_data(const BytesBuffer *b) {
    return (Byte *)b->buf.p;
}

BytesBuffer *bytes_new_buffer(Alloc *a, Slice buf) {
    BytesBuffer *b = BURROW_NEW(a, BytesBuffer);
    if (b == NULL)
        return NULL;
    b->a = a;
    b->buf = buf;
    b->buf.elem = TYPE_BYTE;
    b->heap = true;
    return b;
}

BytesBuffer *bytes_new_buffer_string(Alloc *a, Str s) {
    Byte *p = NULL;
    if (s.len > 0) {
        p = (Byte *)mem_alloc_nozero(a, (size_t)s.len, 1);
        if (p == NULL)
            return NULL;
        memcpy(p, s.p, (size_t)s.len);
    }
    BytesBuffer *b = bytes_new_buffer(a, slice_from(p, s.len, s.len, TYPE_BYTE));
    if (b == NULL) {
        if (p != NULL)
            mem_free(a, p, (size_t)s.len, 1);
        return NULL;
    }
    b->owned = p != NULL;
    return b;
}

void bytes_buffer_free(BytesBuffer *b) {
    if (b == NULL)
        return;
    if (b->owned)
        mem_free(b->a, b->buf.p, (size_t)b->buf.cap, 1);
    if (b->heap) {
        mem_free(b->a, b, sizeof *b, _Alignof(BytesBuffer));
        return;
    }
    b->buf = slice_nil(TYPE_BYTE);
    b->off = 0;
    b->last_read = BUFFER_OP_INVALID;
    b->owned = false;
}

Slice bytes_buffer_bytes(BytesBuffer *b) {
    if (b->buf.p == NULL)
        return slice_nil(TYPE_BYTE);
    return slice_from(buffer_data(b) + b->off, b->buf.len - b->off, b->buf.cap - b->off,
                      TYPE_BYTE);
}

Slice bytes_buffer_available_buffer(BytesBuffer *b) {
    if (b->buf.p == NULL)
        return slice_nil(TYPE_BYTE);
    return slice_from(buffer_data(b) + b->buf.len, 0, b->buf.cap - b->buf.len,
                      TYPE_BYTE);
}

Str bytes_buffer_string(BytesBuffer *b, Alloc *a) {
    if (b == NULL)
        return str_clone(a, BURROW_S("<nil>"));
    Int n = b->buf.len - b->off;
    if (n == 0)
        return BURROW_STR_EMPTY;
    Str s = {buffer_data(b) + b->off, n};
    return str_clone(a, s);
}

Slice bytes_buffer_peek(BytesBuffer *b, Int n, Error *err) {
    Slice all = bytes_buffer_bytes(b);
    if (all.len < n) {
        BURROW_OUT(err, io_eof);
        return all;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    all.len = n;
    return all;
}

static bool buffer_empty(const BytesBuffer *b) {
    return b->buf.len <= b->off;
}

Int bytes_buffer_len(BytesBuffer *b) {
    return b->buf.len - b->off;
}

Int bytes_buffer_cap(BytesBuffer *b) {
    return b->buf.cap;
}

Int bytes_buffer_available(BytesBuffer *b) {
    return b->buf.cap - b->buf.len;
}

void bytes_buffer_reset(BytesBuffer *b) {
    b->buf.len = 0;
    b->off = 0;
    b->last_read = BUFFER_OP_INVALID;
}

void bytes_buffer_truncate(BytesBuffer *b, Int n) {
    if (n == 0) {
        bytes_buffer_reset(b);
        return;
    }
    b->last_read = BUFFER_OP_INVALID;
    if (n < 0 || n > bytes_buffer_len(b))
        panic_str(BURROW_S("bytes.Buffer: truncation out of range"));
    b->buf.len = b->off + n;
}

/* Go's tryGrowByReslice. */
static bool buffer_reslice(BytesBuffer *b, Int n, Int *at) {
    Int l = b->buf.len;
    if (n <= b->buf.cap - l) {
        b->buf.len = l + n;
        *at = l;
        return true;
    }
    return false;
}

/* Go's grow: makes room for n more bytes and extends buf over them, and says
 * where they start. Returns false with the buffer unchanged, apart from a
 * reset of an empty one, when the room cannot be had. */
static bool buffer_grow(BytesBuffer *b, Int n, Int *at) {
    Int m = bytes_buffer_len(b);
    if (m == 0 && b->off != 0)
        bytes_buffer_reset(b);
    if (buffer_reslice(b, n, at))
        return true;
    Int c = b->buf.cap;
    if (b->buf.p == NULL && n <= BUFFER_SMALL) {
        Byte *p = (Byte *)mem_alloc_nozero(b->a, BUFFER_SMALL, 1);
        if (p == NULL)
            return false;
        b->buf = slice_from(p, n, BUFFER_SMALL, TYPE_BYTE);
        b->owned = true;
        *at = 0;
        return true;
    }
    if (n <= c / 2 - m) {
        /* Enough room once the read bytes are dropped: slide down. */
        memmove(buffer_data(b), buffer_data(b) + b->off, (size_t)m);
    } else if (c > BURROW_INT_MAX - c - n) {
        return false;
    } else {
        /* Go's growSlice(buf[off:], off+n): room for off+n more, and at
         * least twice the capacity of what is left. */
        Int nc = m + b->off + n;
        if (nc < 2 * (c - b->off))
            nc = 2 * (c - b->off);
        Byte *p = (Byte *)mem_alloc_nozero(b->a, (size_t)nc, 1);
        if (p == NULL)
            return false;
        if (m > 0)
            memcpy(p, buffer_data(b) + b->off, (size_t)m);
        if (b->owned)
            mem_free(b->a, b->buf.p, (size_t)c, 1);
        b->buf = slice_from(p, m, nc, TYPE_BYTE);
        b->owned = true;
    }
    b->off = 0;
    b->buf.len = m + n;
    *at = m;
    return true;
}

bool bytes_buffer_grow(BytesBuffer *b, Int n) {
    if (n < 0)
        panic_str(BURROW_S("bytes.Buffer.Grow: negative count"));
    Int m;
    if (!buffer_grow(b, n, &m))
        return false;
    b->buf.len = m;
    return true;
}

/* Room for n bytes at the end, marked written, or false. */
static bool buffer_room(BytesBuffer *b, Int n, Int *at) {
    b->last_read = BUFFER_OP_INVALID;
    return buffer_reslice(b, n, at) || buffer_grow(b, n, at);
}

static Int buffer_append(BytesBuffer *b, const void *p, Int n, Error *err) {
    Int m;
    if (!buffer_room(b, n, &m)) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return 0;
    }
    if (n > 0)
        memmove(buffer_data(b) + m, p, (size_t)n); /* p may be AvailableBuffer */
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

Int bytes_buffer_write(BytesBuffer *b, Slice p, Error *err) {
    return buffer_append(b, p.p, p.len, err);
}

Int bytes_buffer_write_string(BytesBuffer *b, Str s, Error *err) {
    return buffer_append(b, s.p, s.len, err);
}

Error bytes_buffer_write_byte(BytesBuffer *b, Byte c) {
    Error err;
    buffer_append(b, &c, 1, &err);
    return err;
}

Int bytes_buffer_write_rune(BytesBuffer *b, Rune r, Error *err) {
    if ((uint32_t)r < UTF8_RUNE_SELF) {
        Byte c = (Byte)r;
        return buffer_append(b, &c, 1, err);
    }
    Int m;
    if (!buffer_room(b, UTF8_UTF_MAX, &m)) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return 0;
    }
    Int n = utf8_encode_rune(
        slice_from(buffer_data(b) + m, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE), r);
    b->buf.len = m + n;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

int64_t bytes_buffer_read_from(BytesBuffer *b, IoReader r, Error *err) {
    b->last_read = BUFFER_OP_INVALID;
    int64_t n = 0;
    for (;;) {
        Int i;
        if (!buffer_grow(b, BYTES_MIN_READ, &i)) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return n;
        }
        b->buf.len = i;
        Error e = BURROW_NO_ERROR;
        Int m = BURROW_CALL(
            r, read,
            slice_from(buffer_data(b) + i, b->buf.cap - i, b->buf.cap - i, TYPE_BYTE),
            &e);
        if (m < 0) {
            Error neg = buffer_err_negative_read;
            panic(BURROW_ANY(TYPE_ERROR, &neg));
        }
        b->buf.len = i + m;
        n += (int64_t)m;
        /* Go compares with ==, so an error wrapping EOF is an error. */
        if (e.vt == io_eof.vt && e.data == io_eof.data) {
            BURROW_OUT(err, BURROW_NO_ERROR);
            return n;
        }
        if (!BURROW_OK(e)) {
            BURROW_OUT(err, e);
            return n;
        }
    }
}

int64_t bytes_buffer_write_to(BytesBuffer *b, IoWriter w, Error *err) {
    b->last_read = BUFFER_OP_INVALID;
    int64_t n = 0;
    Int nbytes = bytes_buffer_len(b);
    if (nbytes > 0) {
        Error e = BURROW_NO_ERROR;
        Int m = BURROW_CALL(w, write, bytes_buffer_bytes(b), &e);
        if (m > nbytes)
            panic_str(BURROW_S("bytes.Buffer.WriteTo: invalid Write count"));
        b->off += m;
        n = (int64_t)m;
        if (!BURROW_OK(e)) {
            BURROW_OUT(err, e);
            return n;
        }
        if (m != nbytes) {
            BURROW_OUT(err, io_err_short_write);
            return n;
        }
    }
    bytes_buffer_reset(b);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

Int bytes_buffer_read(BytesBuffer *b, Slice p, Error *err) {
    b->last_read = BUFFER_OP_INVALID;
    if (buffer_empty(b)) {
        bytes_buffer_reset(b);
        BURROW_OUT(err, p.len == 0 ? BURROW_NO_ERROR : io_eof);
        return 0;
    }
    Int n = bytes_buffer_len(b);
    if (n > p.len)
        n = p.len;
    if (n > 0) {
        memcpy(p.p, buffer_data(b) + b->off, (size_t)n);
        b->off += n;
        b->last_read = BUFFER_OP_READ;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

Slice bytes_buffer_next(BytesBuffer *b, Int n) {
    b->last_read = BUFFER_OP_INVALID;
    Int m = bytes_buffer_len(b);
    if (n > m)
        n = m;
    Slice data = bytes_buffer_bytes(b);
    data.len = n;
    b->off += n;
    if (n > 0)
        b->last_read = BUFFER_OP_READ;
    return data;
}

Byte bytes_buffer_read_byte(BytesBuffer *b, Error *err) {
    if (buffer_empty(b)) {
        bytes_buffer_reset(b);
        BURROW_OUT(err, io_eof);
        return 0;
    }
    Byte c = buffer_data(b)[b->off];
    b->off++;
    b->last_read = BUFFER_OP_READ;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return c;
}

Rune bytes_buffer_read_rune(BytesBuffer *b, Int *size, Error *err) {
    if (buffer_empty(b)) {
        bytes_buffer_reset(b);
        if (size != NULL)
            *size = 0;
        BURROW_OUT(err, io_eof);
        return 0;
    }
    const Byte *p = buffer_data(b) + b->off;
    Int n = 1;
    Rune r = p[0];
    if (r >= UTF8_RUNE_SELF) {
        Str rest = {p, bytes_buffer_len(b)};
        r = utf8_decode_rune_in_string(rest, &n);
    }
    b->off += n;
    b->last_read = (signed char)n;
    if (size != NULL)
        *size = n;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return r;
}

Error bytes_buffer_unread_rune(BytesBuffer *b) {
    if (b->last_read <= BUFFER_OP_INVALID)
        return buffer_err_unread_rune;
    if (b->off >= b->last_read)
        b->off -= b->last_read;
    b->last_read = BUFFER_OP_INVALID;
    return BURROW_NO_ERROR;
}

Error bytes_buffer_unread_byte(BytesBuffer *b) {
    if (b->last_read == BUFFER_OP_INVALID)
        return buffer_err_unread_byte;
    b->last_read = BUFFER_OP_INVALID;
    if (b->off > 0)
        b->off--;
    return BURROW_NO_ERROR;
}

/* Go's readSlice: the bytes up to and including delim, as a view. */
static Str buffer_read_slice(BytesBuffer *b, Byte delim, Error *err) {
    Int m = bytes_buffer_len(b);
    const Byte *p = m > 0 ? buffer_data(b) + b->off : NULL;
    const Byte *hit = m > 0 ? (const Byte *)memchr(p, delim, (size_t)m) : NULL;
    Int end = hit != NULL ? (Int)(hit - p) + 1 : m;
    BURROW_OUT(err, hit != NULL ? BURROW_NO_ERROR : io_eof);
    Str line = {m > 0 ? p : NULL, end};
    b->off += end;
    b->last_read = BUFFER_OP_READ;
    return line;
}

Slice bytes_buffer_read_bytes(BytesBuffer *b, Alloc *a, Byte delim, Error *err) {
    Error e;
    Str line = buffer_read_slice(b, delim, &e);
    if (line.len == 0 || line.p == NULL) {
        /* append([]byte(nil), empty...) is nil. */
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)line.len, 1);
    if (p == NULL) {
        b->off -= line.len;
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    if (line.len > 0)
        memcpy(p, line.p, (size_t)line.len);
    BURROW_OUT(err, e);
    return slice_from(p, line.len, line.len, TYPE_BYTE);
}

Str bytes_buffer_read_string(BytesBuffer *b, Alloc *a, Byte delim, Error *err) {
    Error e;
    Str line = buffer_read_slice(b, delim, &e);
    if (line.len == 0) {
        BURROW_OUT(err, e);
        return BURROW_STR_EMPTY;
    }
    Str s = str_clone(a, line);
    if (s.len != line.len) {
        b->off -= line.len;
        BURROW_OUT(err, burrow_err_out_of_memory);
        return BURROW_STR_EMPTY;
    }
    BURROW_OUT(err, e);
    return s;
}

static Int buffer_io_read(void *self, Slice p, Error *err) {
    return bytes_buffer_read((BytesBuffer *)self, p, err);
}

static Int buffer_io_write(void *self, Slice p, Error *err) {
    return bytes_buffer_write((BytesBuffer *)self, p, err);
}

static const IoReaderVT buffer_reader_vt = {&bytes_buffer_desc, buffer_io_read};
static const IoWriterVT buffer_writer_vt = {&bytes_buffer_desc, buffer_io_write};

IoReader bytes_buffer_as_io_reader(BytesBuffer *b) {
    IoReader r = {&buffer_reader_vt, b};
    return r;
}

IoWriter bytes_buffer_as_io_writer(BytesBuffer *b) {
    IoWriter w = {&buffer_writer_vt, b};
    return w;
}

static Byte bytes_buffer_io_read_byte(void *self, Error *err) {
    return bytes_buffer_read_byte((BytesBuffer *)self, err);
}

static const IoByteReaderVT bytes_buffer_byte_reader_vt = {&bytes_buffer_desc,
                                                           bytes_buffer_io_read_byte};

IoByteReader bytes_buffer_as_io_byte_reader(BytesBuffer *r) {
    IoByteReader br = {&bytes_buffer_byte_reader_vt, r};
    return br;
}
