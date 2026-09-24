/* Derived from Go's src/bytes/reader.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/bytes.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include <stdint.h>
#include <string.h>

static const Type bytes_reader_desc = {
    {(const Byte *)"Reader", 6},
    {(const Byte *)"bytes", 5},
    KIND_STRUCT,
    (uint32_t)sizeof(BytesReader),
    (uint16_t)_Alignof(BytesReader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x62726472U, /* "brdr" */
    NULL,
};

const Type *const TYPE_BYTES_READER = &bytes_reader_desc;

/* Go makes these with errors.New inside each method, so nobody can compare
 * against them, and neither can anybody here. */
#define BYTES_READER_ERROR(name, text)                                                 \
    static const Str name##__text = {(const Byte *)(text), (Int)(sizeof(text) - 1)};   \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}

BYTES_READER_ERROR(bytes_reader_err_negative_offset,
                   "bytes.Reader.ReadAt: negative offset");
BYTES_READER_ERROR(bytes_reader_err_unread_byte,
                   "bytes.Reader.UnreadByte: at beginning of slice");
BYTES_READER_ERROR(bytes_reader_err_unread_rune_start,
                   "bytes.Reader.UnreadRune: at beginning of slice");
BYTES_READER_ERROR(bytes_reader_err_unread_rune_prev,
                   "bytes.Reader.UnreadRune: previous operation was not ReadRune");
BYTES_READER_ERROR(bytes_reader_err_whence, "bytes.Reader.Seek: invalid whence");
BYTES_READER_ERROR(bytes_reader_err_negative_position,
                   "bytes.Reader.Seek: negative position");

#undef BYTES_READER_ERROR

BytesReader *bytes_new_reader(Alloc *a, Slice s) {
    BytesReader *r = (BytesReader *)mem_alloc(a, sizeof *r, _Alignof(BytesReader));
    if (r == NULL)
        return NULL;
    bytes_reader_reset(r, s);
    return r;
}

void bytes_reader_reset(BytesReader *r, Slice s) {
    r->s = s;
    r->i = 0;
    r->prev_rune = -1;
}

Int bytes_reader_len(BytesReader *r) {
    if (r->i >= (int64_t)r->s.len)
        return 0;
    return (Int)((int64_t)r->s.len - r->i);
}

int64_t bytes_reader_size(BytesReader *r) {
    return (int64_t)r->s.len;
}

static Int bytes_reader_copy(Slice b, Slice s, int64_t from) {
    Int n = s.len - (Int)from;
    if (n > b.len)
        n = b.len;
    if (n > 0)
        memcpy(b.p, (const Byte *)s.p + from, (size_t)n);
    return n;
}

Int bytes_reader_read(BytesReader *r, Slice b, Error *err) {
    if (r->i >= (int64_t)r->s.len) {
        BURROW_OUT(err, io_eof);
        return 0;
    }
    r->prev_rune = -1;
    Int n = bytes_reader_copy(b, r->s, r->i);
    r->i += n;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

Int bytes_reader_read_at(BytesReader *r, Slice b, int64_t off, Error *err) {
    /* cannot modify state - see io.ReaderAt */
    if (off < 0) {
        BURROW_OUT(err, bytes_reader_err_negative_offset);
        return 0;
    }
    if (off >= (int64_t)r->s.len) {
        BURROW_OUT(err, io_eof);
        return 0;
    }
    Int n = bytes_reader_copy(b, r->s, off);
    BURROW_OUT(err, n < b.len ? io_eof : BURROW_NO_ERROR);
    return n;
}

Byte bytes_reader_read_byte(BytesReader *r, Error *err) {
    r->prev_rune = -1;
    if (r->i >= (int64_t)r->s.len) {
        BURROW_OUT(err, io_eof);
        return 0;
    }
    Byte b = ((const Byte *)r->s.p)[r->i];
    r->i++;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return b;
}

Error bytes_reader_unread_byte(BytesReader *r) {
    if (r->i <= 0)
        return bytes_reader_err_unread_byte;
    r->prev_rune = -1;
    r->i--;
    return BURROW_NO_ERROR;
}

Rune bytes_reader_read_rune(BytesReader *r, Int *size, Error *err) {
    if (r->i >= (int64_t)r->s.len) {
        r->prev_rune = -1;
        BURROW_OUT(size, 0);
        BURROW_OUT(err, io_eof);
        return 0;
    }
    r->prev_rune = (Int)r->i;
    Byte c = ((const Byte *)r->s.p)[r->i];
    if (c < UTF8_RUNE_SELF) {
        r->i++;
        BURROW_OUT(size, 1);
        BURROW_OUT(err, BURROW_NO_ERROR);
        return c;
    }
    Str rest = {(const Byte *)r->s.p + r->i, r->s.len - (Int)r->i};
    Int n;
    Rune ch = utf8_decode_rune_in_string(rest, &n);
    r->i += n;
    BURROW_OUT(size, n);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ch;
}

Error bytes_reader_unread_rune(BytesReader *r) {
    if (r->i <= 0)
        return bytes_reader_err_unread_rune_start;
    if (r->prev_rune < 0)
        return bytes_reader_err_unread_rune_prev;
    r->i = r->prev_rune;
    r->prev_rune = -1;
    return BURROW_NO_ERROR;
}

int64_t bytes_reader_seek(BytesReader *r, int64_t offset, Int whence, Error *err) {
    r->prev_rune = -1;
    int64_t abs;
    switch (whence) {
    case BURROW_IO_SEEK_START:
        abs = offset;
        break;
    case BURROW_IO_SEEK_CURRENT:
        abs = r->i + offset;
        break;
    case BURROW_IO_SEEK_END:
        abs = (int64_t)r->s.len + offset;
        break;
    default:
        BURROW_OUT(err, bytes_reader_err_whence);
        return 0;
    }
    if (abs < 0) {
        BURROW_OUT(err, bytes_reader_err_negative_position);
        return 0;
    }
    r->i = abs;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return abs;
}

int64_t bytes_reader_write_to(BytesReader *r, IoWriter w, Error *err) {
    r->prev_rune = -1;
    if (r->i >= (int64_t)r->s.len) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return 0;
    }
    Slice s = slice_sub(r->s, (Int)r->i, r->s.len);
    Error werr = BURROW_NO_ERROR;
    Int m = BURROW_CALL(w, write, s, &werr);
    if (m > s.len)
        panic_str(BURROW_S("bytes.Reader.WriteTo: invalid Write count"));
    r->i += m;
    if (m != s.len && BURROW_OK(werr))
        werr = io_err_short_write;
    BURROW_OUT(err, werr);
    return (int64_t)m;
}

static Int bytes_reader_io_read(void *self, Slice p, Error *err) {
    return bytes_reader_read((BytesReader *)self, p, err);
}

static int64_t bytes_reader_io_seek(void *self, int64_t offset, int whence,
                                    Error *err) {
    return bytes_reader_seek((BytesReader *)self, offset, whence, err);
}

static const IoReaderVT bytes_reader_reader_vt = {&bytes_reader_desc,
                                                  bytes_reader_io_read};
static const IoSeekerVT bytes_reader_seeker_vt = {&bytes_reader_desc,
                                                  bytes_reader_io_seek};

IoReader bytes_reader_as_io_reader(BytesReader *r) {
    IoReader rd = {&bytes_reader_reader_vt, r};
    return rd;
}

IoSeeker bytes_reader_as_io_seeker(BytesReader *r) {
    IoSeeker sk = {&bytes_reader_seeker_vt, r};
    return sk;
}

static Byte bytes_reader_io_read_byte(void *self, Error *err) {
    return bytes_reader_read_byte((BytesReader *)self, err);
}

static const IoByteReaderVT bytes_reader_byte_reader_vt = {&bytes_reader_desc,
                                                           bytes_reader_io_read_byte};

IoByteReader bytes_reader_as_io_byte_reader(BytesReader *r) {
    IoByteReader br = {&bytes_reader_byte_reader_vt, r};
    return br;
}
