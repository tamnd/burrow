/* Derived from Go's src/strings/builder.go.
 * Go source: go1.27.1.
 *
 * Go's Builder is a byte slice that append grows. This one is the same with the
 * allocator held in the builder, plus one thing Go gets from its collector for
 * free: knowing whether a buffer can be given back. Once String has pointed a
 * Str into the buffer, the buffer stays where it is for as long as the caller
 * wants it, and growing past it leaves it behind. A buffer nobody has seen is
 * reallocated in place or freed.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
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

#include <string.h>

/* The methods io asks for by name: io_copy looks for WriteTo and ReadFrom,
 * and io_write_string for WriteString. */
#define STRINGS_BUILDER_METHODS(M, T)                                                  \
    M(T, WriteString, strings_builder_write_string, IO_SIG_WRITE_STRING)
BURROW_METHODS_DEFINE(StringsBuilder, STRINGS_BUILDER_METHODS);

static const Type builder_desc = {
    {(const Byte *)"Builder", 7},
    {(const Byte *)"strings", 7},
    KIND_STRUCT,
    (uint32_t)sizeof(StringsBuilder),
    (uint16_t)_Alignof(StringsBuilder),
    0,
    (uint16_t)(sizeof burrow__methods_StringsBuilder /
               sizeof burrow__methods_StringsBuilder[0]),
    NULL,
    burrow__methods_StringsBuilder,
    NULL,
    NULL,
    0,
    0x73626c64U, /* "sbld" */
    NULL,
};

const Type *const TYPE_STRINGS_BUILDER = &builder_desc;

/* Go's copyCheck. A builder remembers its own address on first use, and one
 * that finds itself somewhere else has been copied, which would leave two
 * builders appending into one buffer. */
static void builder_copy_check(StringsBuilder *b) {
    if (b->addr == NULL)
        b->addr = b;
    else if (b->addr != b)
        panic_str(BURROW_S("strings: illegal use of non-zero Builder copied by value"));
}

/* Makes room for n more bytes, to 2*cap + n as Go does. */
static bool builder_grow(StringsBuilder *b, Int n) {
    if (b->cap > (BURROW_INT_MAX - n) / 2)
        return false;
    Int ncap = 2 * b->cap + n;
    Byte *nbuf;
    if (b->buf != NULL && !b->lent) {
        nbuf = (Byte *)mem_realloc(b->a, b->buf, (size_t)b->cap, (size_t)ncap, 1);
        if (nbuf == NULL)
            return false;
    } else {
        nbuf = (Byte *)mem_alloc_nozero(b->a, (size_t)ncap, 1);
        if (nbuf == NULL)
            return false;
        if (b->len > 0)
            memcpy(nbuf, b->buf, (size_t)b->len);
    }
    b->buf = nbuf;
    b->cap = ncap;
    b->lent = false;
    return true;
}

/* Room for n more, or false with the builder untouched. */
static bool builder_room(StringsBuilder *b, Int n) {
    return b->cap - b->len >= n || builder_grow(b, n);
}

Str strings_builder_string(StringsBuilder *b) {
    if (b->len == 0)
        return BURROW_STR_EMPTY;
    b->lent = true;
    Str s = {b->buf, b->len};
    return s;
}

Int strings_builder_len(StringsBuilder *b) {
    return b->len;
}

Int strings_builder_cap(StringsBuilder *b) {
    return b->cap;
}

void strings_builder_reset(StringsBuilder *b) {
    if (b->buf != NULL && !b->lent)
        mem_free(b->a, b->buf, (size_t)b->cap, 1);
    b->addr = NULL;
    b->buf = NULL;
    b->len = 0;
    b->cap = 0;
    b->lent = false;
}

bool strings_builder_grow(StringsBuilder *b, Int n) {
    builder_copy_check(b);
    if (n < 0)
        panic_str(BURROW_S("strings.Builder.Grow: negative count"));
    return builder_room(b, n);
}

/* The one place bytes go in. */
static Int builder_append(StringsBuilder *b, const Byte *p, Int n, Error *err) {
    builder_copy_check(b);
    if (n <= 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return 0;
    }
    if (!builder_room(b, n)) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return 0;
    }
    memcpy(b->buf + b->len, p, (size_t)n);
    b->len += n;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

Int strings_builder_write(StringsBuilder *b, Slice p, Error *err) {
    return builder_append(b, (const Byte *)p.p, p.len, err);
}

Error strings_builder_write_byte(StringsBuilder *b, Byte c) {
    Error err;
    builder_append(b, &c, 1, &err);
    return err;
}

Int strings_builder_write_rune(StringsBuilder *b, Rune r, Error *err) {
    Byte tmp[UTF8_UTF_MAX];
    Int n = utf8_encode_rune(slice_from(tmp, UTF8_UTF_MAX, UTF8_UTF_MAX, TYPE_BYTE), r);
    return builder_append(b, tmp, n, err);
}

Int strings_builder_write_string(StringsBuilder *b, Str s, Error *err) {
    return builder_append(b, s.p, s.len, err);
}

static Int builder_io_write(void *self, Slice p, Error *err) {
    return strings_builder_write((StringsBuilder *)self, p, err);
}

static const IoWriterVT builder_writer_vt = {&builder_desc, builder_io_write};

IoWriter strings_builder_as_io_writer(StringsBuilder *b) {
    IoWriter w = {&builder_writer_vt, b};
    return w;
}
