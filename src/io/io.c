/* Derived from Go's src/io/io.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/io.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

/* The io package, which at this point is the interfaces plus the four
 * functions that need nothing but an interface to run.
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
static bool is_eof(Error e) {
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

/* ---------------------------------------------------------------- functions */

Int io_read_at_least(IoReader r, Slice buf, Int min, Error *err) {
    Error e = BURROW_NO_ERROR;
    Int n = 0;

    if (min > buf.len) {
        if (err != NULL)
            *err = io_err_short_buffer;
        return 0;
    }

    /* A reader returning a negative count is broken beyond anything this can
     * paper over. The sub slice below catches it with Go's bounds message,
     * which names the number, rather than adding a silent check here. */
    while (n < min && BURROW_OK(e))
        n += BURROW_CALL(r, read, slice_sub(buf, n, buf.len), &e);

    if (n >= min)
        e = BURROW_NO_ERROR;
    else if (n > 0 && is_eof(e))
        e = io_err_unexpected_eof;

    if (err != NULL)
        *err = e;
    return n;
}

Int io_read_full(IoReader r, Slice buf, Error *err) {
    return io_read_at_least(r, buf, buf.len, err);
}

int64_t io_copy_buffer(IoWriter dst, IoReader src, Slice buf, Error *err) {
    int64_t written = 0;
    Error e = BURROW_NO_ERROR;

    if (buf.len < 1) {
        if (err != NULL)
            *err = io_err_short_buffer;
        return 0;
    }

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
            if (!is_eof(re))
                e = re;
            break;
        }
    }

    if (err != NULL)
        *err = e;
    return written;
}

int64_t io_copy(Alloc *a, IoWriter dst, IoReader src, Error *err) {
    /* Go's size, and it is not arbitrary. It is large enough that the per call
     * cost of a read on a file or a socket disappears against the copying, and
     * small enough to come out of an allocator without anybody noticing. */
    enum { COPY_BUF = 32 * 1024 };

    Slice buf;
    void *p;
    int64_t n;

    /* Not zeroed. Every byte of it is written by a Read before anything reads
     * it back, and 32 KiB of memset on a copy that is about to touch megabytes
     * is pure waste. */
    p = mem_alloc_nozero(a, (size_t)COPY_BUF, 1);
    if (p == NULL) {
        if (err != NULL)
            *err = burrow_err_out_of_memory;
        return 0;
    }

    buf = slice_from(p, (Int)COPY_BUF, (Int)COPY_BUF, TYPE_BYTE);
    n = io_copy_buffer(dst, src, buf, err);

    mem_free(a, p, (size_t)COPY_BUF, 1);
    return n;
}
