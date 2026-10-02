/* Derived from Go's src/testing/iotest/reader.go and writer.go. See
 * include/burrow/testing/iotest.h.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/testing/iotest.h"

#include "burrow/bytes.h"
#include "burrow/fmt.h"
#include "burrow/mem/heap.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

BURROW_SENTINEL_ERROR(iotest_err_timeout, "timeout");

#define IOTEST_TYPE(var, gonm, T)                                                      \
    static const Type var = {                                                          \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"testing/iotest", 14},                                          \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

/* Go compares with io.EOF, not errors.Is. */
static bool iotest_is_eof(Error e) {
    return e.vt == io_eof.vt && e.data == io_eof.data;
}

static void iotest_slice_free(Alloc *a, Slice s) {
    if (s.p != NULL)
        mem_free(a, s.p, (size_t)s.cap, 1);
}

/* Every wrapper is one allocation of its descriptor's size, which is how the
 * free functions know what to give back. */
static void *iotest_new(Alloc *a, const Type *t) {
    void *p = mem_alloc(a, t->size, t->align);
    return p;
}

void iotest_reader_free(Alloc *a, IoReader r) {
    if (r.data == NULL || r.vt == NULL)
        return;
    mem_free(a, r.data, r.vt->self_type->size, r.vt->self_type->align);
}

void iotest_writer_free(Alloc *a, IoWriter w) {
    if (w.data == NULL || w.vt == NULL)
        return;
    mem_free(a, w.data, w.vt->self_type->size, w.vt->self_type->align);
}

/* ---------------------------------------------------------- oneByteReader */

typedef struct OneByteReader {
    IoReader r;
} OneByteReader;

IOTEST_TYPE(one_byte_reader_desc, "oneByteReader", OneByteReader);

static Int one_byte_read(void *self, Slice p, Error *err) {
    OneByteReader *r = (OneByteReader *)self;
    if (p.len == 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return 0;
    }
    return BURROW_CALL(r->r, read, slice_sub(p, 0, 1), err);
}

static const IoReaderVT one_byte_reader_vt = {&one_byte_reader_desc, one_byte_read};

IoReader iotest_one_byte_reader(Alloc *a, IoReader r) {
    OneByteReader *o = (OneByteReader *)iotest_new(a, &one_byte_reader_desc);
    if (o == NULL)
        return (IoReader){0};
    o->r = r;
    return (IoReader){&one_byte_reader_vt, o};
}

/* ------------------------------------------------------------- halfReader */

typedef struct HalfReader {
    IoReader r;
} HalfReader;

IOTEST_TYPE(half_reader_desc, "halfReader", HalfReader);

static Int half_read(void *self, Slice p, Error *err) {
    HalfReader *r = (HalfReader *)self;
    return BURROW_CALL(r->r, read, slice_sub(p, 0, (p.len + 1) / 2), err);
}

static const IoReaderVT half_reader_vt = {&half_reader_desc, half_read};

IoReader iotest_half_reader(Alloc *a, IoReader r) {
    HalfReader *h = (HalfReader *)iotest_new(a, &half_reader_desc);
    if (h == NULL)
        return (IoReader){0};
    h->r = r;
    return (IoReader){&half_reader_vt, h};
}

/* ---------------------------------------------------------- dataErrReader */

/* Go's data is a 1024-byte slice made with the reader, so it is part of the
 * allocation here, and unread is the part of it from off for len bytes. */
typedef struct DataErrReader {
    IoReader r;
    Int off;
    Int len;
    Byte data[1024];
} DataErrReader;

IOTEST_TYPE(data_err_reader_desc, "dataErrReader", DataErrReader);

static Int data_err_read(void *self, Slice p, Error *err) {
    DataErrReader *r = (DataErrReader *)self;
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    for (;;) {
        if (r->len == 0) {
            Error e1 = BURROW_NO_ERROR;
            Int n1 = BURROW_CALL(r->r, read, slice_from(r->data, 1024, 1024, TYPE_BYTE),
                                 &e1);
            r->off = 0;
            r->len = n1;
            e = e1;
        }
        if (n > 0 || !BURROW_OK(e))
            break;
        n = p.len < r->len ? p.len : r->len;
        if (n > 0)
            memmove(p.p, r->data + r->off, (size_t)n);
        r->off += n;
        r->len -= n;
    }
    BURROW_OUT(err, e);
    return n;
}

static const IoReaderVT data_err_reader_vt = {&data_err_reader_desc, data_err_read};

IoReader iotest_data_err_reader(Alloc *a, IoReader r) {
    DataErrReader *d = (DataErrReader *)iotest_new(a, &data_err_reader_desc);
    if (d == NULL)
        return (IoReader){0};
    d->r = r;
    d->off = 0;
    d->len = 0;
    return (IoReader){&data_err_reader_vt, d};
}

/* ---------------------------------------------------------- timeoutReader */

typedef struct TimeoutReader {
    IoReader r;
    Int count;
} TimeoutReader;

IOTEST_TYPE(timeout_reader_desc, "timeoutReader", TimeoutReader);

static Int timeout_read(void *self, Slice p, Error *err) {
    TimeoutReader *r = (TimeoutReader *)self;
    r->count++;
    if (r->count == 2) {
        BURROW_OUT(err, iotest_err_timeout);
        return 0;
    }
    return BURROW_CALL(r->r, read, p, err);
}

static const IoReaderVT timeout_reader_vt = {&timeout_reader_desc, timeout_read};

IoReader iotest_timeout_reader(Alloc *a, IoReader r) {
    TimeoutReader *t = (TimeoutReader *)iotest_new(a, &timeout_reader_desc);
    if (t == NULL)
        return (IoReader){0};
    t->r = r;
    t->count = 0;
    return (IoReader){&timeout_reader_vt, t};
}

/* -------------------------------------------------------------- errReader */

typedef struct ErrReader {
    Error err;
} ErrReader;

IOTEST_TYPE(err_reader_desc, "errReader", ErrReader);

static Int err_read(void *self, Slice p, Error *err) {
    (void)p;
    BURROW_OUT(err, ((ErrReader *)self)->err);
    return 0;
}

static const IoReaderVT err_reader_vt = {&err_reader_desc, err_read};

IoReader iotest_err_reader(Alloc *a, Error err) {
    ErrReader *r = (ErrReader *)iotest_new(a, &err_reader_desc);
    if (r == NULL)
        return (IoReader){0};
    r->err = err;
    return (IoReader){&err_reader_vt, r};
}

/* --------------------------------------------------------- truncateWriter */

typedef struct TruncateWriter {
    IoWriter w;
    int64_t n;
} TruncateWriter;

IOTEST_TYPE(truncate_writer_desc, "truncateWriter", TruncateWriter);

static Int truncate_write(void *self, Slice p, Error *err) {
    TruncateWriter *t = (TruncateWriter *)self;
    if (t->n <= 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return p.len;
    }
    Int n = p.len;
    if ((int64_t)n > t->n)
        n = (Int)t->n;
    Error e = BURROW_NO_ERROR;
    n = BURROW_CALL(t->w, write, slice_sub(p, 0, n), &e);
    t->n -= (int64_t)n;
    if (BURROW_OK(e))
        n = p.len;
    BURROW_OUT(err, e);
    return n;
}

static const IoWriterVT truncate_writer_vt = {&truncate_writer_desc, truncate_write};

IoWriter iotest_truncate_writer(Alloc *a, IoWriter w, int64_t n) {
    TruncateWriter *t = (TruncateWriter *)iotest_new(a, &truncate_writer_desc);
    if (t == NULL)
        return (IoWriter){0};
    t->w = w;
    t->n = n;
    return (IoWriter){&truncate_writer_vt, t};
}

/* ------------------------------------------------------------- TestReader */

/* Go's smallByteReader, which reads 1, 2, 3, 1, 2, 3... bytes at a time and
 * says where a read failed. */
typedef struct SmallByteReader {
    IoReader r;
    Int off;
    Int n;
} SmallByteReader;

IOTEST_TYPE(small_byte_reader_desc, "smallByteReader", SmallByteReader);

static Int small_byte_read(void *self, Slice p, Error *err) {
    SmallByteReader *r = (SmallByteReader *)self;
    if (p.len == 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return 0;
    }
    r->n = r->n % 3 + 1;
    Int n = r->n;
    if (n > p.len)
        n = p.len;
    Error e = BURROW_NO_ERROR;
    n = BURROW_CALL(r->r, read, slice_sub(p, 0, n), &e);
    if (!BURROW_OK(e) && !iotest_is_eof(e))
        e = fmt_errorf_v("Read(%d bytes at offset %d): %v", n, r->off, e);
    r->off += n;
    BURROW_OUT(err, e);
    return n;
}

static const IoReaderVT small_byte_reader_vt = {&small_byte_reader_desc,
                                                small_byte_read};

static Slice iotest_read_small(Alloc *a, IoReader r, Error *err) {
    SmallByteReader s = {r, 0, 0};
    return io_read_all(a, (IoReader){&small_byte_reader_vt, &s}, err);
}

static Str iotest_str(Slice s) {
    return str_from_bytes(s.p, s.len);
}

static Slice iotest_tail(Slice s, Int from) {
    return slice_from((Byte *)s.p + from, s.len - from, s.len - from, TYPE_BYTE);
}

/* The part after the plain reads. Go writes it as one function with the
 * ReadAll results going to the collector, and here they are freed on the way
 * out, which is why it is split from the reads before it. */
static Error iotest_test_seek(IoReader r, const Method *seek, Slice content, Alloc *a) {
    Int clen = content.len;
    Error e = BURROW_NO_ERROR;
    int64_t off;

    /* Seek(0, 1) should report the current file position (EOF). */
    off = burrow__io_seek(seek, r.data, 0, 1, &e);
    if (off != (int64_t)clen || !BURROW_OK(e))
        return fmt_errorf_v("Seek(0, 1) from EOF = %d, %v, want %d, nil", off, e, clen);

    /* Seek backward partway through file, in two steps. If middle is 0,
     * content is empty, and the -1 and +1 seeks cannot be used. */
    Int middle = clen - clen / 3;
    if (middle > 0) {
        off = burrow__io_seek(seek, r.data, -1, 1, &e);
        if (off != (int64_t)(clen - 1) || !BURROW_OK(e))
            /* Go prints -off here, and so does this. */
            return fmt_errorf_v("Seek(-1, 1) from EOF = %d, %v, want %d, nil", -off, e,
                                clen - 1);
        off = burrow__io_seek(seek, r.data, (int64_t)(-clen / 3), 1, &e);
        if (off != (int64_t)(middle - 1) || !BURROW_OK(e))
            return fmt_errorf_v("Seek(%d, 1) from %d = %d, %v, want %d, nil", -clen / 3,
                                clen - 1, off, e, middle - 1);
        off = burrow__io_seek(seek, r.data, 1, 1, &e);
        if (off != (int64_t)middle || !BURROW_OK(e))
            return fmt_errorf_v("Seek(+1, 1) from %d = %d, %v, want %d, nil",
                                middle - 1, off, e, middle);
    }

    /* Seek(0, 1) should report the current file position (middle). */
    off = burrow__io_seek(seek, r.data, 0, 1, &e);
    if (off != (int64_t)middle || !BURROW_OK(e))
        return fmt_errorf_v("Seek(0, 1) from %d = %d, %v, want %d, nil", middle, off, e,
                            middle);

    /* Reading forward should return the last part of the file. */
    Slice data = iotest_read_small(a, r, &e);
    if (!BURROW_OK(e)) {
        iotest_slice_free(a, data);
        return fmt_errorf_v("ReadAll from offset %d: %v", middle, e);
    }
    Slice want = iotest_tail(content, middle);
    bool same = bytes_equal(data, want);
    Error ret = BURROW_NO_ERROR;
    if (!same)
        ret = fmt_errorf_v("ReadAll from offset %d = %q\n\twant %q", middle,
                           iotest_str(data), iotest_str(want));
    iotest_slice_free(a, data);
    if (!same)
        return ret;

    /* Seek relative to end of file, but start elsewhere. */
    off = burrow__io_seek(seek, r.data, (int64_t)(middle / 2), 0, &e);
    if (off != (int64_t)(middle / 2) || !BURROW_OK(e))
        return fmt_errorf_v("Seek(%d, 0) from EOF = %d, %v, want %d, nil", middle / 2,
                            off, e, middle / 2);
    off = burrow__io_seek(seek, r.data, (int64_t)(-clen / 3), 2, &e);
    if (off != (int64_t)middle || !BURROW_OK(e))
        return fmt_errorf_v("Seek(%d, 2) from %d = %d, %v, want %d, nil", -clen / 3,
                            middle / 2, off, e, middle);

    /* Reading forward should return the last part of the file (again). */
    data = iotest_read_small(a, r, &e);
    if (!BURROW_OK(e)) {
        iotest_slice_free(a, data);
        return fmt_errorf_v("ReadAll from offset %d: %v", middle, e);
    }
    same = bytes_equal(data, want);
    if (!same)
        ret = fmt_errorf_v("ReadAll from offset %d = %q\n\twant %q", middle,
                           iotest_str(data), iotest_str(want));
    iotest_slice_free(a, data);
    if (!same)
        return ret;

    /* Absolute seek & read forward. */
    off = burrow__io_seek(seek, r.data, (int64_t)(middle / 2), 0, &e);
    if (off != (int64_t)(middle / 2) || !BURROW_OK(e))
        return fmt_errorf_v("Seek(%d, 0) from EOF = %d, %v, want %d, nil", middle / 2,
                            off, e, middle / 2);
    data = io_read_all(a, r, &e);
    if (!BURROW_OK(e)) {
        iotest_slice_free(a, data);
        return fmt_errorf_v("ReadAll from offset %d: %v", middle / 2, e);
    }
    want = iotest_tail(content, middle / 2);
    same = bytes_equal(data, want);
    if (!same)
        ret = fmt_errorf_v("ReadAll from offset %d = %q\n\twant %q", middle / 2,
                           iotest_str(data), iotest_str(want));
    iotest_slice_free(a, data);
    return ret;
}

static Error iotest_test_read_at(IoReader r, const Method *read_at, Slice content,
                                 Alloc *a) {
    Int clen = content.len;
    Error e = BURROW_NO_ERROR;
    Error ret = BURROW_NO_ERROR;
    Int n;

    /* Go's make([]byte, len(content), len(content)+1). */
    Slice buf = slice_make(a, TYPE_BYTE, clen + 1, clen + 1);
    if (buf.p == NULL)
        return errors_new(error_allocator(), BURROW_S("iotest: out of memory"));
    Slice data = slice_sub(buf, 0, clen);
    Byte *d = (Byte *)data.p;
    const Byte *c = (const Byte *)content.p;

    memset(d, 0xfe, (size_t)clen);
    n = burrow__io_read_at(read_at, r.data, data, 0, &e);
    if (n != clen || (!BURROW_OK(e) && !iotest_is_eof(e))) {
        ret = fmt_errorf_v("ReadAt(%d, 0) = %v, %v, want %d, nil or EOF", clen, n, e,
                           clen);
        goto out;
    }
    if (!bytes_equal(data, content)) {
        ret = fmt_errorf_v("ReadAt(%d, 0) = %q\n\twant %q", clen, iotest_str(data),
                           iotest_str(content));
        goto out;
    }

    n = burrow__io_read_at(read_at, r.data, slice_sub(buf, 0, 1), (int64_t)clen, &e);
    if (n != 0 || !iotest_is_eof(e)) {
        ret = fmt_errorf_v("ReadAt(1, %d) = %v, %v, want 0, EOF", clen, n, e);
        goto out;
    }

    memset(d, 0xfe, (size_t)clen);
    n = burrow__io_read_at(read_at, r.data, buf, 0, &e);
    if (n != clen || !iotest_is_eof(e)) {
        ret =
            fmt_errorf_v("ReadAt(%d, 0) = %v, %v, want %d, EOF", clen + 1, n, e, clen);
        goto out;
    }
    if (!bytes_equal(data, content)) {
        ret = fmt_errorf_v("ReadAt(%d, 0) = %q\n\twant %q", clen, iotest_str(data),
                           iotest_str(content));
        goto out;
    }

    memset(d, 0xfe, (size_t)clen);
    for (Int i = 0; i < clen; i++) {
        n = burrow__io_read_at(read_at, r.data, slice_sub(data, i, i + 1), (int64_t)i,
                               &e);
        if (n != 1 || (!BURROW_OK(e) && (i != clen - 1 || !iotest_is_eof(e)))) {
            const char *want = i == clen - 1 ? "nil or EOF" : "nil";
            ret = fmt_errorf_v("ReadAt(1, %d) = %v, %v, want 1, %s", i, n, e, want);
            goto out;
        }
        if (d[i] != c[i]) {
            ret = fmt_errorf_v("ReadAt(1, %d) = %q want %q", i,
                               str_from_bytes(d + i, 1), str_from_bytes(c + i, 1));
            goto out;
        }
    }

out:
    iotest_slice_free(a, buf);
    return ret;
}

Error iotest_test_reader(IoReader r, Slice content) {
    Alloc *a = heap_allocator();
    Error e = BURROW_NO_ERROR;

    if (content.len > 0) {
        Int n = BURROW_CALL(r, read, slice_from(NULL, 0, 0, TYPE_BYTE), &e);
        if (n != 0 || !BURROW_OK(e))
            return fmt_errorf_v("Read(0) = %d, %v, want 0, nil", n, e);
    }

    Slice data = iotest_read_small(a, r, &e);
    if (!BURROW_OK(e)) {
        iotest_slice_free(a, data);
        return e;
    }
    bool same = bytes_equal(data, content);
    Error ret = BURROW_NO_ERROR;
    if (!same)
        ret = fmt_errorf_v("ReadAll(small amounts) = %q\n\twant %q", iotest_str(data),
                           iotest_str(content));
    iotest_slice_free(a, data);
    if (!same)
        return ret;

    Byte ten[10];
    Int n = BURROW_CALL(r, read, slice_from(ten, 10, 10, TYPE_BYTE), &e);
    if (n != 0 || !iotest_is_eof(e))
        return fmt_errorf_v("Read(10) at EOF = %v, %v, want 0, EOF", n, e);

    const Type *t = r.vt == NULL ? NULL : r.vt->self_type;
    const Method *seek = burrow__io_seek_method(t);
    if (seek != NULL) {
        ret = iotest_test_seek(r, seek, content, a);
        if (!BURROW_OK(ret))
            return ret;
    }

    const Method *read_at = burrow__io_read_at_method(t);
    if (read_at != NULL)
        return iotest_test_read_at(r, read_at, content, a);
    return BURROW_NO_ERROR;
}
