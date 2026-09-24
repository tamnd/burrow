/* Derived from Go's src/strings/reader.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/strings.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

#include "internal.h"

#include <stdint.h>
#include <string.h>

/* The methods io asks for by name: io_copy looks for WriteTo and ReadFrom,
 * and io_write_string for WriteString. */
#define STRINGS_READER_METHODS(M, T)                                                   \
    M(T, WriteTo, strings_reader_write_to, IO_SIG_WRITE_TO)
BURROW_METHODS_DEFINE(StringsReader, STRINGS_READER_METHODS);

static const Type reader_desc = {
    {(const Byte *)"Reader", 6},
    {(const Byte *)"strings", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(StringsReader),
    (uint16_t)_Alignof(StringsReader),
    0,
    (uint16_t)(sizeof burrow__methods_StringsReader /
               sizeof burrow__methods_StringsReader[0]),
    NULL,
    burrow__methods_StringsReader,
    NULL,
    NULL,
    0,
    0x73726472U, /* "srdr" */
    NULL,
};

const Type *const TYPE_STRINGS_READER = &reader_desc;

/* Go makes these with errors.New inside each method, so nobody can compare
 * against them, and neither can anybody here. */
#define READER_ERROR(name, text)                                                       \
    static const Str name##__text = {(const Byte *)(text), (Int)(sizeof(text) - 1)};   \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}

READER_ERROR(reader_err_negative_offset, "strings.Reader.ReadAt: negative offset");
READER_ERROR(reader_err_unread_byte,
             "strings.Reader.UnreadByte: at beginning of string");
READER_ERROR(reader_err_unread_rune_start,
             "strings.Reader.UnreadRune: at beginning of string");
READER_ERROR(reader_err_unread_rune_prev,
             "strings.Reader.UnreadRune: previous operation was not ReadRune");
READER_ERROR(reader_err_whence, "strings.Reader.Seek: invalid whence");
READER_ERROR(reader_err_negative_position, "strings.Reader.Seek: negative position");

#undef READER_ERROR

StringsReader *strings_new_reader(Alloc *a, Str s) {
    StringsReader *r =
        (StringsReader *)mem_alloc(a, sizeof *r, _Alignof(StringsReader));
    if (r == NULL)
        return NULL;
    strings_reader_reset(r, s);
    return r;
}

void strings_reader_reset(StringsReader *r, Str s) {
    r->s = s;
    r->i = 0;
    r->prev_rune = -1;
}

Int strings_reader_len(StringsReader *r) {
    if (r->i >= (int64_t)r->s.len)
        return 0;
    return (Int)((int64_t)r->s.len - r->i);
}

int64_t strings_reader_size(StringsReader *r) {
    return (int64_t)r->s.len;
}

static Int copy_out(Slice b, Str s, int64_t from) {
    Int n = s.len - (Int)from;
    if (n > b.len)
        n = b.len;
    if (n > 0)
        memcpy(b.p, s.p + from, (size_t)n);
    return n;
}

Int strings_reader_read(StringsReader *r, Slice b, Error *err) {
    if (r->i >= (int64_t)r->s.len) {
        BURROW_OUT(err, io_eof);
        return 0;
    }
    r->prev_rune = -1;
    Int n = copy_out(b, r->s, r->i);
    r->i += n;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

Int strings_reader_read_at(StringsReader *r, Slice b, int64_t off, Error *err) {
    /* cannot modify state - see io.ReaderAt */
    if (off < 0) {
        BURROW_OUT(err, reader_err_negative_offset);
        return 0;
    }
    if (off >= (int64_t)r->s.len) {
        BURROW_OUT(err, io_eof);
        return 0;
    }
    Int n = copy_out(b, r->s, off);
    BURROW_OUT(err, n < b.len ? io_eof : BURROW_NO_ERROR);
    return n;
}

Byte strings_reader_read_byte(StringsReader *r, Error *err) {
    r->prev_rune = -1;
    if (r->i >= (int64_t)r->s.len) {
        BURROW_OUT(err, io_eof);
        return 0;
    }
    Byte b = r->s.p[r->i];
    r->i++;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return b;
}

Error strings_reader_unread_byte(StringsReader *r) {
    if (r->i <= 0)
        return reader_err_unread_byte;
    r->prev_rune = -1;
    r->i--;
    return BURROW_NO_ERROR;
}

Rune strings_reader_read_rune(StringsReader *r, Int *size, Error *err) {
    if (r->i >= (int64_t)r->s.len) {
        r->prev_rune = -1;
        BURROW_OUT(size, 0);
        BURROW_OUT(err, io_eof);
        return 0;
    }
    r->prev_rune = (Int)r->i;
    Byte c = r->s.p[r->i];
    if (c < UTF8_RUNE_SELF) {
        r->i++;
        BURROW_OUT(size, 1);
        BURROW_OUT(err, BURROW_NO_ERROR);
        return c;
    }
    Str rest = {r->s.p + r->i, r->s.len - (Int)r->i};
    Int n;
    Rune ch = utf8_decode_rune_in_string(rest, &n);
    r->i += n;
    BURROW_OUT(size, n);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ch;
}

Error strings_reader_unread_rune(StringsReader *r) {
    if (r->i <= 0)
        return reader_err_unread_rune_start;
    if (r->prev_rune < 0)
        return reader_err_unread_rune_prev;
    r->i = r->prev_rune;
    r->prev_rune = -1;
    return BURROW_NO_ERROR;
}

int64_t strings_reader_seek(StringsReader *r, int64_t offset, Int whence, Error *err) {
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
        BURROW_OUT(err, reader_err_whence);
        return 0;
    }
    if (abs < 0) {
        BURROW_OUT(err, reader_err_negative_position);
        return 0;
    }
    r->i = abs;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return abs;
}

int64_t strings_reader_write_to(StringsReader *r, IoWriter w, Error *err) {
    r->prev_rune = -1;
    if (r->i >= (int64_t)r->s.len) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return 0;
    }
    Str s = {r->s.p + r->i, r->s.len - (Int)r->i};
    Error werr = BURROW_NO_ERROR;
    Int m = BURROW_CALL(w, write, burrow__strings_bytes(s), &werr);
    if (m > s.len)
        panic_str(BURROW_S("strings.Reader.WriteTo: invalid WriteString count"));
    r->i += m;
    if (m != s.len && BURROW_OK(werr))
        werr = io_err_short_write;
    BURROW_OUT(err, werr);
    return (int64_t)m;
}

static Int reader_io_read(void *self, Slice p, Error *err) {
    return strings_reader_read((StringsReader *)self, p, err);
}

static int64_t reader_io_seek(void *self, int64_t offset, int whence, Error *err) {
    return strings_reader_seek((StringsReader *)self, offset, whence, err);
}

static const IoReaderVT reader_reader_vt = {&reader_desc, reader_io_read};
static const IoSeekerVT reader_seeker_vt = {&reader_desc, reader_io_seek};

IoReader strings_reader_as_io_reader(StringsReader *r) {
    IoReader rd = {&reader_reader_vt, r};
    return rd;
}

IoSeeker strings_reader_as_io_seeker(StringsReader *r) {
    IoSeeker sk = {&reader_seeker_vt, r};
    return sk;
}

static Byte strings_reader_io_read_byte(void *self, Error *err) {
    return strings_reader_read_byte((StringsReader *)self, err);
}

static const IoByteReaderVT strings_reader_byte_reader_vt = {
    &reader_desc, strings_reader_io_read_byte};

IoByteReader strings_reader_as_io_byte_reader(StringsReader *r) {
    IoByteReader br = {&strings_reader_byte_reader_vt, r};
    return br;
}

static Int strings_reader_io_read_at(void *self, Slice p, int64_t off, Error *err) {
    return strings_reader_read_at((StringsReader *)self, p, off, err);
}

static const IoReaderAtVT strings_reader_reader_at_vt = {&reader_desc,
                                                         strings_reader_io_read_at};

IoReaderAt strings_reader_as_io_reader_at(StringsReader *r) {
    IoReaderAt ra = {&strings_reader_reader_at_vt, r};
    return ra;
}
