/* What debug/macho, debug/pe and debug/plan9obj share. See objfile_internal.h.
 *
 * Derived from Go's src/internal/saferio/io.go and the DWARF methods of
 * src/debug/macho/file.go and src/debug/pe/file.go. Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "objfile_internal.h"

#include "burrow/bytes.h"
#include "burrow/compress/zlib.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* saferio's chunk: the most read up front for a length a file claims. */
#define OBJ_CHUNK ((uint64_t)10 << 20)

static Slice obj_bytes(void *p, Int n) {
    Slice s = {p, n, n, TYPE_BYTE};
    return s;
}

static bool obj_same_error(Error x, Error y) {
    return x.vt == y.vt && x.data == y.data;
}

/* Grows buf, of cap bytes holding len, to take next more of the n that are
 * still to come. */
static bool obj_grow(Alloc *a, Byte **buf, uint64_t len, uint64_t *cap, uint64_t next,
                     uint64_t n) {
    if (len + next <= *cap)
        return true;
    uint64_t ncap = *cap == 0 ? OBJ_CHUNK : *cap * 2;
    if (ncap < len + next)
        ncap = len + next;
    if (ncap > len + n)
        ncap = len + n;
    Byte *nb = (Byte *)mem_realloc(a, *buf, (size_t)*cap, (size_t)ncap, 1);
    if (nb == NULL)
        return false;
    *buf = nb;
    *cap = ncap;
    return true;
}

Slice burrow__saferio_read_data(Alloc *a, IoReader r, uint64_t n, Error *err) {
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    if (n > (uint64_t)INTPTR_MAX) {
        BURROW_OUT(err, io_err_unexpected_eof);
        return none;
    }
    Error e = BURROW_NO_ERROR;
    if (n < OBJ_CHUNK) {
        Byte *buf = (Byte *)mem_alloc_nozero(a, n == 0 ? 1 : (size_t)n, 1);
        if (buf == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
        io_read_full(r, obj_bytes(buf, (Int)n), &e);
        if (BURROW_FAILED(e)) {
            mem_free(a, buf, n == 0 ? 1 : (size_t)n, 1);
            BURROW_OUT(err, e);
            return none;
        }
        BURROW_OUT(err, BURROW_NO_ERROR);
        return obj_bytes(buf, (Int)n);
    }
    Byte *buf = NULL;
    uint64_t len = 0, cap = 0;
    while (n > 0) {
        uint64_t next = n > OBJ_CHUNK ? OBJ_CHUNK : n;
        if (!obj_grow(a, &buf, len, &cap, next, n)) {
            mem_free(a, buf, (size_t)cap, 1);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
        io_read_full(r, obj_bytes(buf + len, (Int)next), &e);
        if (BURROW_FAILED(e)) {
            mem_free(a, buf, (size_t)cap, 1);
            if (len > 0 && obj_same_error(e, io_eof))
                e = io_err_unexpected_eof;
            BURROW_OUT(err, e);
            return none;
        }
        len += next;
        n -= next;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice out = {buf, (Int)len, (Int)cap, TYPE_BYTE};
    return out;
}

Slice burrow__saferio_read_data_at(Alloc *a, IoReaderAt r, uint64_t n, int64_t off,
                                   Error *err) {
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    if (n > (uint64_t)INTPTR_MAX) {
        BURROW_OUT(err, io_err_unexpected_eof);
        return none;
    }
    Error e = BURROW_NO_ERROR;
    if (n < OBJ_CHUNK) {
        Byte *buf = (Byte *)mem_alloc(a, n == 0 ? 1 : (size_t)n, 1);
        if (buf == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
        r.vt->read_at(r.data, obj_bytes(buf, (Int)n), off, &e);
        /* io.SectionReader can return EOF for n == 0, but for our purposes
         * that is a success. */
        if (BURROW_FAILED(e) && (!obj_same_error(e, io_eof) || n > 0)) {
            mem_free(a, buf, n == 0 ? 1 : (size_t)n, 1);
            BURROW_OUT(err, e);
            return none;
        }
        BURROW_OUT(err, BURROW_NO_ERROR);
        return obj_bytes(buf, (Int)n);
    }
    Byte *buf = NULL;
    uint64_t len = 0, cap = 0;
    while (n > 0) {
        uint64_t next = n > OBJ_CHUNK ? OBJ_CHUNK : n;
        if (!obj_grow(a, &buf, len, &cap, next, n)) {
            mem_free(a, buf, (size_t)cap, 1);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
        r.vt->read_at(r.data, obj_bytes(buf + len, (Int)next), off, &e);
        if (BURROW_FAILED(e)) {
            mem_free(a, buf, (size_t)cap, 1);
            BURROW_OUT(err, e);
            return none;
        }
        len += next;
        n -= next;
        off += (int64_t)next;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    Slice out = {buf, (Int)len, (Int)cap, TYPE_BYTE};
    return out;
}

Int burrow__saferio_slice_cap(uint64_t size, uint64_t c) {
    if (c > (uint64_t)INTPTR_MAX)
        return -1;
    if (size > 0 && c > UINT64_MAX / size)
        return -1;
    if (c * size > OBJ_CHUNK) {
        c = OBJ_CHUNK / size;
        if (c == 0)
            c = 1;
    }
    return (Int)c;
}

/* ---------------------------------------------------------------- readers */

typedef struct ObjReaderBox {
    IoSectionReader sr;
    Alloc *a;
} ObjReaderBox;

static const Type obj_oom_reader_desc = {
    {(const Byte *)"oomReader", 9},
    {(const Byte *)"debug", 5},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Int obj_oom_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    BURROW_OUT(err, burrow_err_out_of_memory);
    return 0;
}

static int64_t obj_oom_seek(void *self, int64_t offset, int whence, Error *err) {
    (void)self;
    (void)offset;
    (void)whence;
    BURROW_OUT(err, burrow_err_out_of_memory);
    return 0;
}

static const IoReadSeekerVT obj_oom_reader_vt = {
    {&obj_oom_reader_desc, obj_oom_read},
    {&obj_oom_reader_desc, obj_oom_seek},
};

IoReadSeeker burrow__objfile_open(Alloc *a, IoSectionReader *sr) {
    ObjReaderBox *b = (ObjReaderBox *)mem_alloc(a, sizeof *b, _Alignof(ObjReaderBox));
    if (b == NULL) {
        IoReadSeeker r = {&obj_oom_reader_vt, NULL};
        return r;
    }
    b->a = a;
    b->sr = io_new_section_reader(io_section_reader_as_io_reader_at(sr), 0, INT64_MAX);
    return io_section_reader_as_io_read_seeker(&b->sr);
}

void burrow__objfile_reader_free(IoReadSeeker r) {
    ObjReaderBox *b = (ObjReaderBox *)r.data;
    if (b == NULL)
        return;
    mem_free(b->a, b, sizeof *b, _Alignof(ObjReaderBox));
}

/* ------------------------------------------------------------------ dwarf */

Slice burrow__objfile_unzlib(Alloc *a, Slice b, Error *err) {
    const Byte *p = (const Byte *)b.p;
    if (b.len < 12 || memcmp(p, "ZLIB", 4) != 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return b;
    }
    Slice none = {NULL, 0, 0, TYPE_BYTE};
    uint64_t dlen = 0;
    for (int i = 4; i < 12; i++)
        dlen = dlen << 8 | p[i];
    BytesReader br;
    bytes_reader_reset(&br, obj_bytes((Byte *)b.p + 12, b.len - 12));
    Error e = BURROW_NO_ERROR;
    IoReadCloser r = zlib_new_reader(a, bytes_reader_as_io_reader(&br), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return none;
    }
    IoReader rd = {&r.vt->reader, r.data};
    Slice dbuf = burrow__saferio_read_data(a, rd, dlen, &e);
    if (BURROW_OK(e))
        e = r.vt->closer.close(r.data);
    zlib_reader_free(r);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return none;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return dbuf;
}

Str burrow__objfile_cstring(Alloc *a, const Byte *p, Int n) {
    const Byte *end = n > 0 ? (const Byte *)memchr(p, 0, (size_t)n) : NULL;
    Int k = end == NULL ? n : (Int)(end - p);
    if (k == 0)
        return BURROW_STR_EMPTY;
    Byte *q = (Byte *)mem_alloc_nozero(a, (size_t)k, 1);
    if (q == NULL)
        return BURROW_STR_EMPTY;
    memcpy(q, p, (size_t)k);
    return str_from_bytes(q, k);
}
