/* Derived from Go's src/io/io.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/io.h"

#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/type.h"
#include "internal.h"

#include <stdint.h>

/* The io package: io.go, everything in it but the multi and pipe halves, which
 * are in multi.c and pipe.c as they are in Go.
 *
 * ReadAtLeast, ReadFull and copyBuffer are ported line for line, including the
 * two parts of them that look wrong and are not. ReadAtLeast clears the error
 * once it has the bytes it was asked for, so a reader that hands over the last
 * of its input and reports the end in the same call is a success and not a
 * failure. copyBuffer swallows the end of the input for the same reason: a copy
 * that ran to the end of the source is a copy that worked. */

/* ---------------------------------------------------------------- sentinels */

BURROW_SENTINEL_ERROR(io_eof, "EOF");
BURROW_SENTINEL_ERROR(io_err_unexpected_eof, "unexpected EOF");
BURROW_SENTINEL_ERROR(io_err_short_write, "short write");
BURROW_SENTINEL_ERROR(io_err_short_buffer, "short buffer");
BURROW_SENTINEL_ERROR(io_err_no_progress,
                      "multiple Read calls return no data or error");
BURROW_SENTINEL_ERROR(io_err_closed_pipe, "io: read/write on closed pipe");

/* Go's errWhence and errOffset, unexported, for the two Seek methods. */
#define IO_STATIC_ERROR(name, text)                                                    \
    static const Str name##__text = {(const Byte *)(text), (Int)(sizeof(text) - 1)};   \
    static const Error name = {&burrow_sentinel_error_vt, &name##__text}

IO_STATIC_ERROR(io_err_whence, "Seek: invalid whence");
IO_STATIC_ERROR(io_err_offset, "Seek: invalid offset");

/* Go's errInvalidWrite, which is unexported, so this one is static. A caller
 * cannot compare against it and does not need to: it means the Writer it was
 * given has a bug, and the only thing to do about that is read the message. */
static const Str io_err_invalid_write__text = {
    (const Byte *)"invalid write result", (Int)(sizeof("invalid write result") - 1)};
static const Error io_err_invalid_write = {&burrow_sentinel_error_vt,
                                           &io_err_invalid_write__text};

/* Go writes err == EOF rather than errors.Is(err, EOF) in both of the loops
 * below, and it is worth keeping. An error that merely wraps the end of the
 * input is a reader that has taken the one value meaning nothing went wrong and
 * dressed it up as something that did. Treating that as a clean end would turn a
 * truncated file into an empty one, which is the failure nobody notices. */
bool burrow__io_is_eof(Error e) {
    return e.vt == io_eof.vt && e.data == io_eof.data;
}

/* ------------------------------------------------------------- conversions */

IoReader io_read_writer_as_io_reader(IoReadWriter rw) {
    IoReader r = {NULL, NULL};
    if (rw.vt == NULL)
        return r;
    r.vt = &rw.vt->reader;
    r.data = rw.data;
    return r;
}

IoWriter io_read_writer_as_io_writer(IoReadWriter rw) {
    IoWriter w = {NULL, NULL};
    if (rw.vt == NULL)
        return w;
    w.vt = &rw.vt->writer;
    w.data = rw.data;
    return w;
}

IoReader io_read_closer_as_io_reader(IoReadCloser rc) {
    IoReader r = {NULL, NULL};
    if (rc.vt == NULL)
        return r;
    r.vt = &rc.vt->reader;
    r.data = rc.data;
    return r;
}

IoCloser io_read_closer_as_io_closer(IoReadCloser rc) {
    IoCloser c = {NULL, NULL};
    if (rc.vt == NULL)
        return c;
    c.vt = &rc.vt->closer;
    c.data = rc.data;
    return c;
}

IoWriter io_write_closer_as_io_writer(IoWriteCloser wc) {
    IoWriter w = {NULL, NULL};
    if (wc.vt == NULL)
        return w;
    w.vt = &wc.vt->writer;
    w.data = wc.data;
    return w;
}

IoCloser io_write_closer_as_io_closer(IoWriteCloser wc) {
    IoCloser c = {NULL, NULL};
    if (wc.vt == NULL)
        return c;
    c.vt = &wc.vt->closer;
    c.data = wc.data;
    return c;
}

IoReader io_read_write_closer_as_io_reader(IoReadWriteCloser rwc) {
    IoReader r = {NULL, NULL};
    if (rwc.vt == NULL)
        return r;
    r.vt = &rwc.vt->reader;
    r.data = rwc.data;
    return r;
}

IoWriter io_read_write_closer_as_io_writer(IoReadWriteCloser rwc) {
    IoWriter w = {NULL, NULL};
    if (rwc.vt == NULL)
        return w;
    w.vt = &rwc.vt->writer;
    w.data = rwc.data;
    return w;
}

IoCloser io_read_write_closer_as_io_closer(IoReadWriteCloser rwc) {
    IoCloser c = {NULL, NULL};
    if (rwc.vt == NULL)
        return c;
    c.vt = &rwc.vt->closer;
    c.data = rwc.data;
    return c;
}

IoReader io_read_seeker_as_io_reader(IoReadSeeker rs) {
    IoReader out = {NULL, NULL};
    if (rs.vt == NULL)
        return out;
    out.vt = &rs.vt->reader;
    out.data = rs.data;
    return out;
}

IoSeeker io_read_seeker_as_io_seeker(IoReadSeeker rs) {
    IoSeeker out = {NULL, NULL};
    if (rs.vt == NULL)
        return out;
    out.vt = &rs.vt->seeker;
    out.data = rs.data;
    return out;
}

IoReader io_read_seek_closer_as_io_reader(IoReadSeekCloser rsc) {
    IoReader out = {NULL, NULL};
    if (rsc.vt == NULL)
        return out;
    out.vt = &rsc.vt->reader;
    out.data = rsc.data;
    return out;
}

IoSeeker io_read_seek_closer_as_io_seeker(IoReadSeekCloser rsc) {
    IoSeeker out = {NULL, NULL};
    if (rsc.vt == NULL)
        return out;
    out.vt = &rsc.vt->seeker;
    out.data = rsc.data;
    return out;
}

IoCloser io_read_seek_closer_as_io_closer(IoReadSeekCloser rsc) {
    IoCloser out = {NULL, NULL};
    if (rsc.vt == NULL)
        return out;
    out.vt = &rsc.vt->closer;
    out.data = rsc.data;
    return out;
}

IoWriter io_write_seeker_as_io_writer(IoWriteSeeker ws) {
    IoWriter out = {NULL, NULL};
    if (ws.vt == NULL)
        return out;
    out.vt = &ws.vt->writer;
    out.data = ws.data;
    return out;
}

IoSeeker io_write_seeker_as_io_seeker(IoWriteSeeker ws) {
    IoSeeker out = {NULL, NULL};
    if (ws.vt == NULL)
        return out;
    out.vt = &ws.vt->seeker;
    out.data = ws.data;
    return out;
}

IoReader io_read_write_seeker_as_io_reader(IoReadWriteSeeker rws) {
    IoReader out = {NULL, NULL};
    if (rws.vt == NULL)
        return out;
    out.vt = &rws.vt->reader;
    out.data = rws.data;
    return out;
}

IoWriter io_read_write_seeker_as_io_writer(IoReadWriteSeeker rws) {
    IoWriter out = {NULL, NULL};
    if (rws.vt == NULL)
        return out;
    out.vt = &rws.vt->writer;
    out.data = rws.data;
    return out;
}

IoSeeker io_read_write_seeker_as_io_seeker(IoReadWriteSeeker rws) {
    IoSeeker out = {NULL, NULL};
    if (rws.vt == NULL)
        return out;
    out.vt = &rws.vt->seeker;
    out.data = rws.data;
    return out;
}

IoByteReader io_byte_scanner_as_io_byte_reader(IoByteScanner bs) {
    IoByteReader out = {NULL, NULL};
    if (bs.vt == NULL)
        return out;
    out.vt = &bs.vt->byte_reader;
    out.data = bs.data;
    return out;
}

IoRuneReader io_rune_scanner_as_io_rune_reader(IoRuneScanner rs) {
    IoRuneReader out = {NULL, NULL};
    if (rs.vt == NULL)
        return out;
    out.vt = &rs.vt->rune_reader;
    out.data = rs.data;
    return out;
}

/* ------------------------------------------------------------- method sets */

#define IO_IFACE_TYPE(cname, gonm)                                                     \
    const Type burrow_type_##cname = {                                                 \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"io", 2},                                                       \
        KIND_INTERFACE,                                                                \
        (uint32_t)sizeof(cname),                                                       \
        (uint16_t)_Alignof(cname),                                                     \
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

IO_IFACE_TYPE(IoReader, "Reader");
IO_IFACE_TYPE(IoWriter, "Writer");

/* Only here to be named in a signature, so as far as reflection can tell it is
 * an unsafe pointer, with a name that says what it points at. */
const Type burrow_type_IoErrorArg = {
    {(const Byte *)"*error", 6},
    {NULL, 0},
    KIND_UNSAFE_POINTER,
    (uint32_t)sizeof(IoErrorArg),
    (uint16_t)_Alignof(IoErrorArg),
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

static const Str io_name_write_to = {(const Byte *)"WriteTo", 7};
static const Str io_name_read_from = {(const Byte *)"ReadFrom", 8};
static const Str io_name_write_string = {(const Byte *)"WriteString", 11};

/* The method called name on t, if it has one and it has the shape of the
 * interface being asked about: one argument of type in0, the error pointer,
 * and one result of type out. A method with the right name and the wrong shape
 * is not an implementation, as in Go, where it would not satisfy the
 * interface. */
static const Method *io_method(const Type *t, Str name, const Type *in0,
                               const Type *out) {
    if (t == NULL)
        return NULL;
    const Method *m = type_method_by_name(t, name);
    if (m == NULL || m->ftype == NULL || m->thunk == NULL)
        return NULL;
    const Type *f = m->ftype;
    if (type_num_in(f) != 2 || type_num_out(f) != 1)
        return NULL;
    if (type_in(f, 0) != in0 || type_in(f, 1) != &burrow_type_IoErrorArg ||
        type_out(f, 0) != out)
        return NULL;
    return m;
}

bool burrow__io_try_write_to(IoReader src, IoWriter dst, int64_t *n, Error *err) {
    if (src.vt == NULL)
        return false;
    const Method *m = io_method(src.vt->self_type, io_name_write_to,
                                &burrow_type_IoWriter, TYPE_OF(int64_t));
    if (m == NULL)
        return false;
    Error e = BURROW_NO_ERROR;
    IoErrorArg ea = &e;
    int64_t out = 0;
    void *args[2] = {&dst, (void *)&ea};
    void *rets[1] = {&out};
    method_call(m, src.data, args, rets);
    BURROW_OUT(n, out);
    BURROW_OUT(err, e);
    return true;
}

bool burrow__io_try_read_from(IoWriter dst, IoReader src, int64_t *n, Error *err) {
    if (dst.vt == NULL)
        return false;
    const Method *m = io_method(dst.vt->self_type, io_name_read_from,
                                &burrow_type_IoReader, TYPE_OF(int64_t));
    if (m == NULL)
        return false;
    Error e = BURROW_NO_ERROR;
    IoErrorArg ea = &e;
    int64_t out = 0;
    void *args[2] = {&src, (void *)&ea};
    void *rets[1] = {&out};
    method_call(m, dst.data, args, rets);
    BURROW_OUT(n, out);
    BURROW_OUT(err, e);
    return true;
}

bool burrow__io_try_write_string(IoWriter w, Str s, Int *n, Error *err) {
    if (w.vt == NULL)
        return false;
    const Method *m =
        io_method(w.vt->self_type, io_name_write_string, TYPE_STRING, TYPE_INT);
    if (m == NULL)
        return false;
    Error e = BURROW_NO_ERROR;
    IoErrorArg ea = &e;
    Int out = 0;
    void *args[2] = {&s, (void *)&ea};
    void *rets[1] = {&out};
    method_call(m, w.data, args, rets);
    BURROW_OUT(n, out);
    BURROW_OUT(err, e);
    return true;
}

/* ---------------------------------------------------------------- functions */

Int io_read_at_least(IoReader r, Slice buf, Int min, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = 0;

    if (min > buf.len) {
        BURROW_OUT(err, io_err_short_buffer);
        return 0;
    }

    /* A reader returning a negative count is broken beyond anything this can
     * paper over. The sub slice below catches it with Go's bounds message,
     * which names the number, rather than adding a silent check here. */
    while (n < min && BURROW_OK(e))
        n += BURROW_CALL(r, read, slice_sub(buf, n, buf.len), &e);

    if (n >= min)
        e = BURROW_NO_ERROR;
    else if (n > 0 && burrow__io_is_eof(e))
        e = io_err_unexpected_eof;

    BURROW_OUT(err, e);
    return n;
}

Int io_read_full(IoReader r, Slice buf, Error *err) {
    return io_read_at_least(r, buf, buf.len, err);
}

/* Go's copyBuffer after the two shortcuts, which the callers have already
 * tried. */
static int64_t copy_loop(IoWriter dst, IoReader src, Slice buf, Error *err) {
    int64_t written = 0;
    Error e = BURROW_NO_ERROR;

    for (;;) {
        Error re = BURROW_NO_ERROR;
        Int nr = BURROW_CALL(src, read, buf, &re);

        if (nr > 0) {
            Error we = BURROW_NO_ERROR;
            Int nw = BURROW_CALL(dst, write, slice_sub(buf, 0, nr), &we);

            /* A Writer claiming it wrote more than it was given, or less than
             * nothing, has lost count, and believing either number would mean
             * reporting a copy that did not happen. */
            if (nw < 0 || nw > nr) {
                nw = 0;
                if (BURROW_OK(we))
                    we = io_err_invalid_write;
            }
            written += nw;
            if (BURROW_FAILED(we)) {
                e = we;
                break;
            }
            if (nr != nw) {
                e = io_err_short_write;
                break;
            }
        }
        if (BURROW_FAILED(re)) {
            if (!burrow__io_is_eof(re))
                e = re;
            break;
        }
    }

    BURROW_OUT(err, e);
    return written;
}

int64_t io_copy_buffer(IoWriter dst, IoReader src, Slice buf, Error *err) {
    int64_t n = 0;
    if (burrow__io_try_write_to(src, dst, &n, err))
        return n;
    if (burrow__io_try_read_from(dst, src, &n, err))
        return n;
    if (buf.len < 1) {
        BURROW_OUT(err, io_err_short_buffer);
        return 0;
    }
    return copy_loop(dst, src, buf, err);
}

/* The limited reader's table, defined further down. A forward declaration of
 * the const object itself is a tentative definition, which MSVC warns about. */
static const IoReaderVT *limited_reader_table(void);

int64_t io_copy(Alloc *a, IoWriter dst, IoReader src, Error *err) {
    int64_t n = 0;
    if (burrow__io_try_write_to(src, dst, &n, err))
        return n;
    if (burrow__io_try_read_from(dst, src, &n, err))
        return n;

    /* Go's size, and it is not arbitrary. It is large enough that the per call
     * cost of a read on a file or a socket disappears against the copying, and
     * small enough to come out of an allocator without anybody noticing. A
     * limited reader with less than that left gets a buffer its own size. */
    Int size = (Int)32 * 1024;
    if (src.vt == limited_reader_table()) {
        const IoLimitedReader *l = (const IoLimitedReader *)src.data;
        if ((int64_t)size > l->n)
            size = l->n < 1 ? 1 : (Int)l->n;
    }

    /* Not zeroed. Every byte of it is written by a Read before anything reads
     * it back, and 32 KiB of memset on a copy that is about to touch megabytes
     * is pure waste. */
    void *p = mem_alloc_nozero(a, (size_t)size, 1);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return 0;
    }

    Slice buf = slice_from(p, size, size, TYPE_BYTE);
    n = copy_loop(dst, src, buf, err);

    mem_free(a, p, (size_t)size, 1);
    return n;
}

int64_t io_copy_n(Alloc *a, IoWriter dst, IoReader src, int64_t n, Error *err) {
    IoLimitedReader l = io_limit_reader(src, n);
    Error e = BURROW_NO_ERROR;
    int64_t written = io_copy(a, dst, io_limited_reader_as_io_reader(&l), &e);
    if (written == n) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return n;
    }
    /* The source ran out before n. */
    if (written < n && BURROW_OK(e))
        e = io_eof;
    BURROW_OUT(err, e);
    return written;
}

Slice io_read_all(Alloc *a, IoReader r, Error *err) {
    /* Go starts at 512 and grows by half again each time the space left drops
     * under a sixteenth, keeping the chunks apart and joining them at the end
     * so nothing is copied twice. An allocator can often grow in place, which
     * is the cheaper version of the same idea, so this grows the one block. */
    Int cap = 512;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)cap, 1);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    Int len = 0;
    Error e = BURROW_NO_ERROR;
    for (;;) {
        Error re = BURROW_NO_ERROR;
        Int n = BURROW_CALL(r, read,
                            slice_from(p + len, cap - len, cap - len, TYPE_BYTE), &re);
        /* The same sub slice check io_read_at_least relies on, for a reader
         * that answers with a count it cannot have read. */
        if (n < 0 || n > cap - len)
            runtime_slice_bounds_out_of_range(len + n, len + n, cap);
        len += n;
        if (BURROW_FAILED(re)) {
            if (!burrow__io_is_eof(re))
                e = re;
            break;
        }
        if (cap - len < cap / 16) {
            Int ncap = cap + cap / 2 + 256;
            Byte *np = (Byte *)mem_realloc(a, p, (size_t)cap, (size_t)ncap, 1);
            if (np == NULL) {
                e = burrow_err_out_of_memory;
                break;
            }
            p = np;
            cap = ncap;
        }
    }
    BURROW_OUT(err, e);
    return slice_from(p, len, cap, TYPE_BYTE);
}

Int io_write_string(IoWriter w, Str s, Error *err) {
    Int n = 0;
    if (burrow__io_try_write_string(w, s, &n, err))
        return n;
    Error e = BURROW_NO_ERROR;
    n = BURROW_CALL(w, write, burrow__io_str_bytes(s), &e);
    BURROW_OUT(err, e);
    return n;
}

/* ------------------------------------------------------------ LimitedReader */

#define IO_TYPE(var, gonm, ctype, nmeth, meths)                                        \
    static const Type var = {                                                          \
        {(const Byte *)(gonm), (Int)(sizeof(gonm) - 1)},                               \
        {(const Byte *)"io", 2},                                                       \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(ctype),                                                       \
        (uint16_t)_Alignof(ctype),                                                     \
        0,                                                                             \
        (nmeth),                                                                       \
        NULL,                                                                          \
        (meths),                                                                       \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

IO_TYPE(limited_reader_desc, "LimitedReader", IoLimitedReader, 0, NULL);

IoLimitedReader io_limit_reader(IoReader r, int64_t n) {
    IoLimitedReader l = {r, n};
    return l;
}

Int io_limited_reader_read(IoLimitedReader *l, Slice p, Error *err) {
    if (l->n <= 0) {
        BURROW_OUT(err, io_eof);
        return 0;
    }
    if ((int64_t)p.len > l->n)
        p = slice_sub(p, 0, (Int)l->n);
    Error e = BURROW_NO_ERROR;
    Int n = BURROW_CALL(l->r, read, p, &e);
    l->n -= n;
    BURROW_OUT(err, e);
    return n;
}

static Int limited_io_read(void *self, Slice p, Error *err) {
    return io_limited_reader_read((IoLimitedReader *)self, p, err);
}

static const IoReaderVT limited_reader_vt = {&limited_reader_desc, limited_io_read};

static const IoReaderVT *limited_reader_table(void) {
    return &limited_reader_vt;
}

IoReader io_limited_reader_as_io_reader(IoLimitedReader *l) {
    IoReader r = {&limited_reader_vt, l};
    return r;
}

/* ------------------------------------------------------------ SectionReader */

IO_TYPE(section_reader_desc, "SectionReader", IoSectionReader, 0, NULL);

IoSectionReader io_new_section_reader(IoReaderAt r, int64_t off, int64_t n) {
    int64_t remaining;
    if (off <= INT64_MAX - n)
        remaining = n + off;
    else
        /* Overflow, with no way to indicate an error. Assume the caller meant
         * the rest of the input, which is what Go assumes. */
        remaining = INT64_MAX;
    IoSectionReader s = {r, off, off, remaining, n};
    return s;
}

Int io_section_reader_read(IoSectionReader *s, Slice p, Error *err) {
    if (s->off >= s->limit) {
        BURROW_OUT(err, io_eof);
        return 0;
    }
    int64_t max = s->limit - s->off;
    if ((int64_t)p.len > max)
        p = slice_sub(p, 0, (Int)max);
    Error e = BURROW_NO_ERROR;
    Int n = BURROW_CALL(s->r, read_at, p, s->off, &e);
    s->off += n;
    BURROW_OUT(err, e);
    return n;
}

int64_t io_section_reader_seek(IoSectionReader *s, int64_t offset, Int whence,
                               Error *err) {
    switch (whence) {
    case BURROW_IO_SEEK_START:
        offset += s->base;
        break;
    case BURROW_IO_SEEK_CURRENT:
        offset += s->off;
        break;
    case BURROW_IO_SEEK_END:
        offset += s->limit;
        break;
    default:
        BURROW_OUT(err, io_err_whence);
        return 0;
    }
    if (offset < s->base) {
        BURROW_OUT(err, io_err_offset);
        return 0;
    }
    s->off = offset;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return offset - s->base;
}

Int io_section_reader_read_at(IoSectionReader *s, Slice p, int64_t off, Error *err) {
    if (off < 0 || off >= io_section_reader_size(s)) {
        BURROW_OUT(err, io_eof);
        return 0;
    }
    off += s->base;
    int64_t max = s->limit - off;
    if ((int64_t)p.len > max) {
        Error e = BURROW_NO_ERROR;
        Int n = BURROW_CALL(s->r, read_at, slice_sub(p, 0, (Int)max), off, &e);
        if (BURROW_OK(e))
            e = io_eof;
        BURROW_OUT(err, e);
        return n;
    }
    Error e = BURROW_NO_ERROR;
    Int n = BURROW_CALL(s->r, read_at, p, off, &e);
    BURROW_OUT(err, e);
    return n;
}

int64_t io_section_reader_size(const IoSectionReader *s) {
    return s->limit - s->base;
}

IoReaderAt io_section_reader_outer(const IoSectionReader *s, int64_t *off, int64_t *n) {
    BURROW_OUT(off, s->base);
    BURROW_OUT(n, s->n);
    return s->r;
}

static Int section_io_read(void *self, Slice p, Error *err) {
    return io_section_reader_read((IoSectionReader *)self, p, err);
}

static int64_t section_io_seek(void *self, int64_t offset, int whence, Error *err) {
    return io_section_reader_seek((IoSectionReader *)self, offset, whence, err);
}

static Int section_io_read_at(void *self, Slice p, int64_t off, Error *err) {
    return io_section_reader_read_at((IoSectionReader *)self, p, off, err);
}

static const IoReadSeekerVT section_read_seeker_vt = {
    {&section_reader_desc, section_io_read},
    {&section_reader_desc, section_io_seek},
};
static const IoReaderAtVT section_reader_at_vt = {&section_reader_desc,
                                                  section_io_read_at};

IoReader io_section_reader_as_io_reader(IoSectionReader *s) {
    IoReader r = {&section_read_seeker_vt.reader, s};
    return r;
}

IoSeeker io_section_reader_as_io_seeker(IoSectionReader *s) {
    IoSeeker k = {&section_read_seeker_vt.seeker, s};
    return k;
}

IoReaderAt io_section_reader_as_io_reader_at(IoSectionReader *s) {
    IoReaderAt r = {&section_reader_at_vt, s};
    return r;
}

IoReadSeeker io_section_reader_as_io_read_seeker(IoSectionReader *s) {
    IoReadSeeker rs = {&section_read_seeker_vt, s};
    return rs;
}

/* ------------------------------------------------------------- OffsetWriter */

IO_TYPE(offset_writer_desc, "OffsetWriter", IoOffsetWriter, 0, NULL);

IoOffsetWriter io_new_offset_writer(IoWriterAt w, int64_t off) {
    IoOffsetWriter o = {w, off, off};
    return o;
}

Int io_offset_writer_write(IoOffsetWriter *o, Slice p, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = BURROW_CALL(o->w, write_at, p, o->off, &e);
    o->off += n;
    BURROW_OUT(err, e);
    return n;
}

Int io_offset_writer_write_at(IoOffsetWriter *o, Slice p, int64_t off, Error *err) {
    if (off < 0) {
        BURROW_OUT(err, io_err_offset);
        return 0;
    }
    off += o->base;
    Error e = BURROW_NO_ERROR;
    Int n = BURROW_CALL(o->w, write_at, p, off, &e);
    BURROW_OUT(err, e);
    return n;
}

int64_t io_offset_writer_seek(IoOffsetWriter *o, int64_t offset, Int whence,
                              Error *err) {
    switch (whence) {
    case BURROW_IO_SEEK_START:
        offset += o->base;
        break;
    case BURROW_IO_SEEK_CURRENT:
        offset += o->off;
        break;
    default:
        BURROW_OUT(err, io_err_whence);
        return 0;
    }
    if (offset < o->base) {
        BURROW_OUT(err, io_err_offset);
        return 0;
    }
    o->off = offset;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return offset - o->base;
}

static Int offset_io_write(void *self, Slice p, Error *err) {
    return io_offset_writer_write((IoOffsetWriter *)self, p, err);
}

static Int offset_io_write_at(void *self, Slice p, int64_t off, Error *err) {
    return io_offset_writer_write_at((IoOffsetWriter *)self, p, off, err);
}

static int64_t offset_io_seek(void *self, int64_t offset, int whence, Error *err) {
    return io_offset_writer_seek((IoOffsetWriter *)self, offset, whence, err);
}

static const IoWriteSeekerVT offset_write_seeker_vt = {
    {&offset_writer_desc, offset_io_write},
    {&offset_writer_desc, offset_io_seek},
};
static const IoWriterAtVT offset_writer_at_vt = {&offset_writer_desc,
                                                 offset_io_write_at};

IoWriter io_offset_writer_as_io_writer(IoOffsetWriter *o) {
    IoWriter w = {&offset_write_seeker_vt.writer, o};
    return w;
}

IoWriterAt io_offset_writer_as_io_writer_at(IoOffsetWriter *o) {
    IoWriterAt w = {&offset_writer_at_vt, o};
    return w;
}

IoSeeker io_offset_writer_as_io_seeker(IoOffsetWriter *o) {
    IoSeeker k = {&offset_write_seeker_vt.seeker, o};
    return k;
}

IoWriteSeeker io_offset_writer_as_io_write_seeker(IoOffsetWriter *o) {
    IoWriteSeeker ws = {&offset_write_seeker_vt, o};
    return ws;
}

/* ---------------------------------------------------------------- TeeReader */

IO_TYPE(tee_reader_desc, "teeReader", IoTeeReader, 0, NULL);

IoTeeReader io_tee_reader(IoReader r, IoWriter w) {
    IoTeeReader t = {r, w};
    return t;
}

static Int tee_io_read(void *self, Slice p, Error *err) {
    IoTeeReader *t = (IoTeeReader *)self;
    Int n = BURROW_CALL(t->r, read, p, err);
    if (n > 0) {
        Error we = BURROW_NO_ERROR;
        Int m = BURROW_CALL(t->w, write, slice_sub(p, 0, n), &we);
        if (BURROW_FAILED(we)) {
            *err = we;
            return m;
        }
    }
    return n;
}

static const IoReaderVT tee_reader_vt = {&tee_reader_desc, tee_io_read};

IoReader io_tee_reader_as_io_reader(IoTeeReader *t) {
    IoReader r = {&tee_reader_vt, t};
    return r;
}

/* ------------------------------------------------------------------ Discard */

typedef struct IoDiscard {
    Byte unused;
} IoDiscard;

static Int discard_write_string(IoDiscard *d, Str s, Error *err) {
    (void)d;
    *err = BURROW_NO_ERROR;
    return s.len;
}

/* Go keeps its 8 KiB buffers in a sync.Pool. Nothing reads what lands in this
 * one, so every caller can share a single static buffer: the bytes written into
 * it by two readers at once are garbage either way, and garbage is what a black
 * hole is for. */
static Byte discard_black_hole[8192];

static int64_t discard_read_from(IoDiscard *d, IoReader r, Error *err) {
    (void)d;
    int64_t n = 0;
    Slice buf = slice_from(discard_black_hole, (Int)sizeof discard_black_hole,
                           (Int)sizeof discard_black_hole, TYPE_BYTE);
    for (;;) {
        Error e = BURROW_NO_ERROR;
        Int m = BURROW_CALL(r, read, buf, &e);
        n += m;
        if (BURROW_FAILED(e)) {
            *err = burrow__io_is_eof(e) ? BURROW_NO_ERROR : e;
            return n;
        }
    }
}

#define DISCARD_METHODS(M, T)                                                          \
    M(T, ReadFrom, discard_read_from, IO_SIG_READ_FROM)                                \
    M(T, WriteString, discard_write_string, IO_SIG_WRITE_STRING)
BURROW_METHODS_DEFINE(IoDiscard, DISCARD_METHODS);

IO_TYPE(discard_desc, "discard", IoDiscard,
        (uint16_t)(sizeof burrow__methods_IoDiscard /
                   sizeof burrow__methods_IoDiscard[0]),
        burrow__methods_IoDiscard);

static Int discard_io_write(void *self, Slice p, Error *err) {
    (void)self;
    (void)err;
    return p.len;
}

static const IoWriterVT discard_writer_vt = {&discard_desc, discard_io_write};

const IoWriter io_discard = {&discard_writer_vt, NULL};

/* ---------------------------------------------------------------- NopCloser */

IO_TYPE(nop_closer_desc, "nopCloser", IoNopCloser, 0, NULL);

static int64_t nop_closer_write_to(IoNopCloser *c, IoWriter w, Error *err) {
    int64_t n = 0;
    /* The closer only gets this descriptor when r has WriteTo, so this is Go's
     * c.Reader.(WriterTo) and cannot fail. */
    if (!burrow__io_try_write_to(c->r, w, &n, err))
        runtime_panic(BURROW_S("interface conversion: io.Reader is not io.WriterTo"));
    return n;
}

#define NOP_CLOSER_METHODS(M, T) M(T, WriteTo, nop_closer_write_to, IO_SIG_WRITE_TO)
BURROW_METHODS_DEFINE(IoNopCloser, NOP_CLOSER_METHODS);

IO_TYPE(nop_closer_writer_to_desc, "nopCloserWriterTo", IoNopCloser,
        (uint16_t)(sizeof burrow__methods_IoNopCloser /
                   sizeof burrow__methods_IoNopCloser[0]),
        burrow__methods_IoNopCloser);

IoNopCloser io_nop_closer(IoReader r) {
    IoNopCloser c = {r};
    return c;
}

static Int nop_io_read(void *self, Slice p, Error *err) {
    IoNopCloser *c = (IoNopCloser *)self;
    return BURROW_CALL(c->r, read, p, err);
}

static Error nop_io_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static const IoReadCloserVT nop_closer_vt = {
    {&nop_closer_desc, nop_io_read},
    {&nop_closer_desc, nop_io_close},
};
static const IoReadCloserVT nop_closer_writer_to_vt = {
    {&nop_closer_writer_to_desc, nop_io_read},
    {&nop_closer_writer_to_desc, nop_io_close},
};

IoReadCloser io_nop_closer_as_io_read_closer(IoNopCloser *c) {
    const IoReadCloserVT *vt = &nop_closer_vt;
    if (c->r.vt != NULL && io_method(c->r.vt->self_type, io_name_write_to,
                                     &burrow_type_IoWriter, TYPE_OF(int64_t)) != NULL)
        vt = &nop_closer_writer_to_vt;
    IoReadCloser rc = {vt, c};
    return rc;
}
