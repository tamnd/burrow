/* Derived from Go's src/io/pipe.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/io.h"

#include "internal.h"

#include "burrow/chan.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include <string.h>

/* Pipe adapter to connect code expecting an io.Reader with code expecting an
 * io.Writer, ported with Go's channels: a write offers its slice on wr_ch, the
 * read that takes it copies what fits and says how much on rd_ch, and done is
 * closed once either end is. The one thing Go has that this does not is the
 * collector, so the three channels and the struct are freed by io_pipe_free. */

/* Go's onceError: an error that keeps the first value stored in it. */
typedef struct OnceError {
    SyncMutex mu;
    Error err;
} OnceError;

static void once_error_store(OnceError *a, Error err) {
    sync_mutex_lock(&a->mu);
    if (BURROW_OK(a->err))
        a->err = err;
    sync_mutex_unlock(&a->mu);
}

static Error once_error_load(OnceError *a) {
    sync_mutex_lock(&a->mu);
    Error e = a->err;
    sync_mutex_unlock(&a->mu);
    return e;
}

typedef struct Pipe {
    SyncMutex wr_mu; /* serializes Write operations */
    Chan *wr_ch;     /* Slice */
    Chan *rd_ch;     /* Int */

    SyncMutex once_mu; /* protects closing done */
    bool closed;
    Chan *done; /* bool, never sent on, only closed */

    OnceError rerr;
    OnceError werr;

    Alloc *a;
} Pipe;

/* Go's PipeReader is struct{ pipe } and PipeWriter is struct{ r PipeReader },
 * so both point at the same pipe, and here they are the same pointer. The two
 * names exist so the two ends cannot be mixed up in a signature. */
struct IoPipeReader {
    Pipe p;
};

struct IoPipeWriter {
    IoPipeReader r;
};

static Error pipe_read_close_error(Pipe *p) {
    Error rerr = once_error_load(&p->rerr);
    Error werr = once_error_load(&p->werr);
    if (BURROW_OK(rerr) && BURROW_FAILED(werr))
        return werr;
    return io_err_closed_pipe;
}

static Error pipe_write_close_error(Pipe *p) {
    Error werr = once_error_load(&p->werr);
    Error rerr = once_error_load(&p->rerr);
    if (BURROW_OK(werr) && BURROW_FAILED(rerr))
        return rerr;
    return io_err_closed_pipe;
}

static void pipe_close_done(Pipe *p) {
    sync_mutex_lock(&p->once_mu);
    if (!p->closed) {
        p->closed = true;
        chan_close(p->done);
    }
    sync_mutex_unlock(&p->once_mu);
}

static bool pipe_is_done(Pipe *p) {
    bool ok = true;
    return chan_try_recv(p->done, NULL, &ok) && !ok;
}

static Int pipe_read(Pipe *p, Slice b, Error *err) {
    if (pipe_is_done(p)) {
        BURROW_OUT(err, pipe_read_close_error(p));
        return 0;
    }

    Slice bw;
    SelectCase cases[2] = {BURROW_RECV(p->wr_ch, &bw), BURROW_RECV(p->done, NULL)};
    if (chan_select(cases, 2) == 0) {
        Int nr = bw.len < b.len ? bw.len : b.len;
        if (nr > 0)
            memmove(b.p, bw.p, (size_t)nr);
        chan_send(p->rd_ch, &nr);
        BURROW_OUT(err, BURROW_NO_ERROR);
        return nr;
    }
    BURROW_OUT(err, pipe_read_close_error(p));
    return 0;
}

static Error pipe_close_read(Pipe *p, Error err) {
    if (BURROW_OK(err))
        err = io_err_closed_pipe;
    once_error_store(&p->rerr, err);
    pipe_close_done(p);
    return BURROW_NO_ERROR;
}

static Int pipe_write(Pipe *p, Slice b, Error *err) {
    if (pipe_is_done(p)) {
        BURROW_OUT(err, pipe_write_close_error(p));
        return 0;
    }
    sync_mutex_lock(&p->wr_mu);

    Int n = 0;
    Error e = BURROW_NO_ERROR;
    for (bool once = true; once || b.len > 0; once = false) {
        SelectCase cases[2] = {BURROW_SEND(p->wr_ch, &b), BURROW_RECV(p->done, NULL)};
        if (chan_select(cases, 2) == 0) {
            Int nw = 0;
            chan_recv(p->rd_ch, &nw);
            b = slice_sub(b, nw, b.len);
            n += nw;
        } else {
            e = pipe_write_close_error(p);
            break;
        }
    }

    sync_mutex_unlock(&p->wr_mu);
    BURROW_OUT(err, e);
    return n;
}

static Error pipe_close_write(Pipe *p, Error err) {
    if (BURROW_OK(err))
        err = io_eof;
    once_error_store(&p->werr, err);
    pipe_close_done(p);
    return BURROW_NO_ERROR;
}

/* ---------------------------------------------------------------- the ends */

Int io_pipe_reader_read(IoPipeReader *r, Slice data, Error *err) {
    return pipe_read(&r->p, data, err);
}

Error io_pipe_reader_close(IoPipeReader *r) {
    return io_pipe_reader_close_with_error(r, BURROW_NO_ERROR);
}

Error io_pipe_reader_close_with_error(IoPipeReader *r, Error err) {
    return pipe_close_read(&r->p, err);
}

Int io_pipe_writer_write(IoPipeWriter *w, Slice data, Error *err) {
    return pipe_write(&w->r.p, data, err);
}

Error io_pipe_writer_close(IoPipeWriter *w) {
    return io_pipe_writer_close_with_error(w, BURROW_NO_ERROR);
}

Error io_pipe_writer_close_with_error(IoPipeWriter *w, Error err) {
    return pipe_close_write(&w->r.p, err);
}

void io_pipe(Alloc *a, IoPipeReader **r, IoPipeWriter **w) {
    BURROW_OUT(r, NULL);
    BURROW_OUT(w, NULL);
    IoPipeWriter *pw =
        (IoPipeWriter *)mem_alloc(a, sizeof(IoPipeWriter), _Alignof(IoPipeWriter));
    if (pw == NULL)
        return;
    Pipe *p = &pw->r.p;
    p->a = a;
    p->wr_ch = chan_make(a, TYPE_BYTES, 0);
    p->rd_ch = chan_make(a, TYPE_INT, 0);
    p->done = chan_make(a, TYPE_BOOL, 0);
    if (p->wr_ch == NULL || p->rd_ch == NULL || p->done == NULL) {
        io_pipe_free(&pw->r);
        return;
    }
    BURROW_OUT(r, &pw->r);
    BURROW_OUT(w, pw);
}

void io_pipe_free(IoPipeReader *r) {
    if (r == NULL)
        return;
    Pipe *p = &r->p;
    if (p->wr_ch != NULL)
        chan_free(p->wr_ch);
    if (p->rd_ch != NULL)
        chan_free(p->rd_ch);
    if (p->done != NULL)
        chan_free(p->done);
    mem_free(p->a, r, sizeof(IoPipeWriter), _Alignof(IoPipeWriter));
}

/* ------------------------------------------------------------- interfaces */

static const Type pipe_reader_desc = {
    {(const Byte *)"PipeReader", 10},
    {(const Byte *)"io", 2},
    KIND_STRUCT,
    (uint32_t)sizeof(IoPipeReader),
    (uint16_t)_Alignof(IoPipeReader),
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

static const Type pipe_writer_desc = {
    {(const Byte *)"PipeWriter", 10},
    {(const Byte *)"io", 2},
    KIND_STRUCT,
    (uint32_t)sizeof(IoPipeWriter),
    (uint16_t)_Alignof(IoPipeWriter),
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

static Int pipe_io_read(void *self, Slice p, Error *err) {
    return io_pipe_reader_read((IoPipeReader *)self, p, err);
}

static Error pipe_io_close_reader(void *self) {
    return io_pipe_reader_close((IoPipeReader *)self);
}

static Int pipe_io_write(void *self, Slice p, Error *err) {
    return io_pipe_writer_write((IoPipeWriter *)self, p, err);
}

static Error pipe_io_close_writer(void *self) {
    return io_pipe_writer_close((IoPipeWriter *)self);
}

static const IoReadCloserVT pipe_reader_vt = {
    {&pipe_reader_desc, pipe_io_read},
    {&pipe_reader_desc, pipe_io_close_reader},
};

static const IoWriteCloserVT pipe_writer_vt = {
    {&pipe_writer_desc, pipe_io_write},
    {&pipe_writer_desc, pipe_io_close_writer},
};

IoReader io_pipe_reader_as_io_reader(IoPipeReader *r) {
    IoReader out = {&pipe_reader_vt.reader, r};
    return out;
}

IoCloser io_pipe_reader_as_io_closer(IoPipeReader *r) {
    IoCloser out = {&pipe_reader_vt.closer, r};
    return out;
}

IoReadCloser io_pipe_reader_as_io_read_closer(IoPipeReader *r) {
    IoReadCloser out = {&pipe_reader_vt, r};
    return out;
}

IoWriter io_pipe_writer_as_io_writer(IoPipeWriter *w) {
    IoWriter out = {&pipe_writer_vt.writer, w};
    return out;
}

IoCloser io_pipe_writer_as_io_closer(IoPipeWriter *w) {
    IoCloser out = {&pipe_writer_vt.closer, w};
    return out;
}

IoWriteCloser io_pipe_writer_as_io_write_closer(IoPipeWriter *w) {
    IoWriteCloser out = {&pipe_writer_vt, w};
    return out;
}
