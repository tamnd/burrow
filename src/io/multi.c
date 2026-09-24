/* Derived from Go's src/io/multi.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/io.h"

#include "internal.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

/* Both types are one allocation: the struct, and the list right after it. The
 * list is Go's slice, and consuming a reader is moving the start of it along,
 * so the struct remembers the whole allocation separately in order to free
 * it. */

/* ------------------------------------------------------------- multiReader */

static int64_t multi_reader_write_to(MultiReader *mr, IoWriter w, Error *err);

#define MULTI_READER_METHODS(M, T) M(T, WriteTo, multi_reader_write_to, IO_SIG_WRITE_TO)
BURROW_METHODS_DEFINE(MultiReader, MULTI_READER_METHODS);

static const Type multi_reader_desc = {
    {(const Byte *)"multiReader", 11},
    {(const Byte *)"io", 2},
    KIND_STRUCT,
    (uint32_t)sizeof(MultiReader),
    (uint16_t)_Alignof(MultiReader),
    0,
    (uint16_t)(sizeof burrow__methods_MultiReader /
               sizeof burrow__methods_MultiReader[0]),
    NULL,
    burrow__methods_MultiReader,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Int multi_io_read(void *self, Slice p, Error *err);

static const IoReaderVT multi_reader_vt = {&multi_reader_desc, multi_io_read};

/* Go's eofReader, which a finished reader is replaced with so the collector
 * can have it. There is no collector to help here, but the list still has to
 * say something about a slot that has been used up, and a reader that answers
 * io_eof is the honest thing to say. */
static const Type eof_reader_desc = {
    {(const Byte *)"eofReader", 9},
    {(const Byte *)"io", 2},
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

static Int eof_io_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    *err = io_eof;
    return 0;
}

static const IoReaderVT eof_reader_vt = {&eof_reader_desc, eof_io_read};

static Int multi_io_read(void *self, Slice p, Error *err) {
    MultiReader *mr = (MultiReader *)self;
    while (mr->n > 0) {
        /* Optimization to flatten nested multiReaders (Issue 13558). */
        if (mr->n == 1 && mr->readers[0].vt == &multi_reader_vt) {
            const MultiReader *inner = (const MultiReader *)mr->readers[0].data;
            mr->readers = inner->readers;
            mr->n = inner->n;
            continue;
        }
        Error e = BURROW_NO_ERROR;
        Int n = BURROW_CALL(mr->readers[0], read, p, &e);
        bool eof = burrow__io_is_eof(e);
        if (eof) {
            mr->readers[0].vt = &eof_reader_vt;
            mr->readers[0].data = NULL;
            mr->readers++;
            mr->n--;
        }
        if (n > 0 || !eof) {
            if (eof && mr->n > 0)
                /* Don't return EOF yet. More readers remain. */
                e = BURROW_NO_ERROR;
            *err = e;
            return n;
        }
    }
    *err = io_eof;
    return 0;
}

static int64_t multi_reader_write_to_buf(MultiReader *mr, IoWriter w, Slice buf,
                                         Error *err) {
    int64_t sum = 0;
    for (Int i = 0; i < mr->n; i++) {
        IoReader r = mr->readers[i];
        Error e = BURROW_NO_ERROR;
        int64_t n;
        if (r.vt == &multi_reader_vt)
            /* Reuse the buffer with nested multiReaders. */
            n = multi_reader_write_to_buf((MultiReader *)r.data, w, buf, &e);
        else
            n = io_copy_buffer(w, r, buf, &e);
        sum += n;
        if (BURROW_FAILED(e)) {
            /* Permit resume or retry after an error. */
            mr->readers += i;
            mr->n -= i;
            *err = e;
            return sum;
        }
        mr->readers[i].vt = NULL;
        mr->readers[i].data = NULL;
    }
    mr->readers += mr->n;
    mr->n = 0;
    *err = BURROW_NO_ERROR;
    return sum;
}

static int64_t multi_reader_write_to(MultiReader *mr, IoWriter w, Error *err) {
    enum { BUF = 32 * 1024 };
    void *p = mem_alloc_nozero(mr->a, (size_t)BUF, 1);
    if (p == NULL) {
        *err = burrow_err_out_of_memory;
        return 0;
    }
    int64_t n =
        multi_reader_write_to_buf(mr, w, slice_from(p, BUF, BUF, TYPE_BYTE), err);
    mem_free(mr->a, p, (size_t)BUF, 1);
    return n;
}

/* The struct and the list in one block, the list aligned for IoReader. */
static size_t multi_head(size_t size) {
    size_t al = _Alignof(IoReader);
    return (size + al - 1) / al * al;
}

IoReader io_multi_reader(Alloc *a, const IoReader *readers, Int n) {
    IoReader out = {NULL, NULL};
    size_t head = multi_head(sizeof(MultiReader));
    size_t size = head + (size_t)n * sizeof(IoReader);
    void *p = mem_alloc(a, size, BURROW_ALIGN_MAX);
    if (p == NULL)
        return out;
    MultiReader *mr = p;
    void *list = (Byte *)p + head;
    mr->a = a;
    mr->readers = list;
    mr->n = n;
    mr->total = n;
    if (n > 0)
        memcpy(mr->readers, readers, (size_t)n * sizeof(IoReader));
    out.vt = &multi_reader_vt;
    out.data = mr;
    return out;
}

void io_multi_reader_free(Alloc *a, IoReader r) {
    if (r.vt != &multi_reader_vt)
        return;
    const MultiReader *mr = (const MultiReader *)r.data;
    size_t size =
        multi_head(sizeof(MultiReader)) + (size_t)mr->total * sizeof(IoReader);
    mem_free(a, r.data, size, BURROW_ALIGN_MAX);
}

/* ------------------------------------------------------------- multiWriter */

static Int multi_writer_write_string(MultiWriter *t, Str s, Error *err);

#define MULTI_WRITER_METHODS(M, T)                                                     \
    M(T, WriteString, multi_writer_write_string, IO_SIG_WRITE_STRING)
BURROW_METHODS_DEFINE(MultiWriter, MULTI_WRITER_METHODS);

static const Type multi_writer_desc = {
    {(const Byte *)"multiWriter", 11},
    {(const Byte *)"io", 2},
    KIND_STRUCT,
    (uint32_t)sizeof(MultiWriter),
    (uint16_t)_Alignof(MultiWriter),
    0,
    (uint16_t)(sizeof burrow__methods_MultiWriter /
               sizeof burrow__methods_MultiWriter[0]),
    NULL,
    burrow__methods_MultiWriter,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

static Int multi_io_write(void *self, Slice p, Error *err) {
    const MultiWriter *t = (const MultiWriter *)self;
    for (Int i = 0; i < t->n; i++) {
        Error e = BURROW_NO_ERROR;
        Int n = BURROW_CALL(t->writers[i], write, p, &e);
        if (BURROW_FAILED(e)) {
            *err = e;
            return n;
        }
        if (n != p.len) {
            *err = io_err_short_write;
            return n;
        }
    }
    return p.len;
}

static Int multi_writer_write_string(MultiWriter *t, Str s, Error *err) {
    /* Go converts to []byte at most once, lazily. A Str is already bytes, so
     * io_write_string's fallback costs nothing and is the whole loop body. */
    for (Int i = 0; i < t->n; i++) {
        Error e = BURROW_NO_ERROR;
        Int n = io_write_string(t->writers[i], s, &e);
        if (BURROW_FAILED(e)) {
            *err = e;
            return n;
        }
        if (n != s.len) {
            *err = io_err_short_write;
            return n;
        }
    }
    *err = BURROW_NO_ERROR;
    return s.len;
}

static const IoWriterVT multi_writer_vt = {&multi_writer_desc, multi_io_write};

IoWriter io_multi_writer(Alloc *a, const IoWriter *writers, Int n) {
    IoWriter out = {NULL, NULL};
    Int total = 0;
    for (Int i = 0; i < n; i++) {
        if (writers[i].vt == &multi_writer_vt)
            total += ((const MultiWriter *)writers[i].data)->n;
        else
            total++;
    }
    size_t head = multi_head(sizeof(MultiWriter));
    void *p = mem_alloc(a, head + (size_t)total * sizeof(IoWriter), BURROW_ALIGN_MAX);
    if (p == NULL)
        return out;
    MultiWriter *mw = p;
    void *list = (Byte *)p + head;
    mw->writers = list;
    mw->n = 0;
    for (Int i = 0; i < n; i++) {
        if (writers[i].vt == &multi_writer_vt) {
            const MultiWriter *inner = (const MultiWriter *)writers[i].data;
            for (Int j = 0; j < inner->n; j++)
                mw->writers[mw->n++] = inner->writers[j];
        } else {
            mw->writers[mw->n++] = writers[i];
        }
    }
    out.vt = &multi_writer_vt;
    out.data = mw;
    return out;
}

void io_multi_writer_free(Alloc *a, IoWriter w) {
    if (w.vt != &multi_writer_vt)
        return;
    const MultiWriter *mw = (const MultiWriter *)w.data;
    size_t size = multi_head(sizeof(MultiWriter)) + (size_t)mw->n * sizeof(IoWriter);
    mem_free(a, w.data, size, BURROW_ALIGN_MAX);
}
