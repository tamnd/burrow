/* net/http/internal/http2's pipe.go: the buffered pipe a stream's body is
 * read through.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http2.h"

#include "burrow/chan.h"
#include "burrow/error.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include <stdbool.h>

BURROW_SENTINEL_ERROR(burrow__http2_err_closed_pipe_write, "write on closed buffer");
BURROW_SENTINEL_ERROR(burrow__http2_err_uninitialized_pipe_write,
                      "write on uninitialized buffer");

/* Locks the pipe, and points c at mu the first time, as Go's does lazily. */
static void hp_lock(Http2Pipe *p) {
    sync_mutex_lock(&p->mu);
    if (p->c.l.vt == NULL)
        p->c.l = sync_mutex_locker(&p->mu);
}

/* The caller's own copy of an error the pipe keeps. */
static Error hp_out_err(Error e) {
    return BURROW_FAILED(e) ? error_retain(error_allocator(), e) : e;
}

void burrow__http2_pipe_set_buffer(Http2Pipe *p, Http2PipeBuffer b) {
    hp_lock(p);
    if (BURROW_OK(p->err) && BURROW_OK(p->break_err))
        p->b = b;
    sync_mutex_unlock(&p->mu);
}

Int burrow__http2_pipe_len(Http2Pipe *p) {
    hp_lock(p);
    Int n = p->b.vt == NULL ? p->unread : p->b.vt->len(p->b.self);
    sync_mutex_unlock(&p->mu);
    return n;
}

Int burrow__http2_pipe_read(Http2Pipe *p, Slice d, Error *err) {
    hp_lock(p);
    for (;;) {
        if (BURROW_FAILED(p->break_err)) {
            Error e = p->break_err;
            sync_mutex_unlock(&p->mu);
            BURROW_OUT(err, hp_out_err(e));
            return 0;
        }
        if (p->b.vt != NULL && p->b.vt->len(p->b.self) > 0) {
            Error e = BURROW_NO_ERROR;
            Int n = p->b.vt->read(p->b.self, d, &e);
            sync_mutex_unlock(&p->mu);
            BURROW_OUT(err, e);
            return n;
        }
        if (BURROW_FAILED(p->err)) {
            if (p->read_fn.f != NULL) {
                Func fn = p->read_fn;
                p->read_fn.f = NULL; /* not sticky like err */
                p->read_fn.env = NULL;
                BURROW_CALLF0(fn); /* trailers get copied here, say */
            }
            p->b.vt = NULL;
            p->b.self = NULL;
            Error e = p->err;
            sync_mutex_unlock(&p->mu);
            BURROW_OUT(err, hp_out_err(e));
            return 0;
        }
        sync_cond_wait(&p->c);
    }
}

Int burrow__http2_pipe_write(Http2Pipe *p, Slice d, Error *err) {
    hp_lock(p);
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    if (BURROW_FAILED(p->err) || BURROW_FAILED(p->break_err))
        e = burrow__http2_err_closed_pipe_write;
    /* Writing before setBuffer is a bug in the caller, but an error is kinder
     * than a crash. */
    else if (p->b.vt == NULL)
        e = burrow__http2_err_uninitialized_pipe_write;
    else
        n = p->b.vt->write(p->b.self, d, &e);
    sync_cond_signal(&p->c);
    sync_mutex_unlock(&p->mu);
    BURROW_OUT(err, e);
    return n;
}

/* closeDoneLocked. p->mu is held. */
static void hp_close_done_locked(Http2Pipe *p) {
    if (p->donec == NULL)
        return;
    /* Closing only once is not racy, because it is always done holding mu. */
    bool ok = false;
    if (!chan_try_recv(p->donec, NULL, &ok) || ok)
        chan_close(p->donec);
}

static void hp_close_with_error(Http2Pipe *p, bool brk, Error err, Func fn) {
    if (BURROW_OK(err))
        panic_str(BURROW_S("err must be non-nil"));
    hp_lock(p);
    Error *dst = brk ? &p->break_err : &p->err;
    if (BURROW_OK(*dst)) {
        p->read_fn = fn;
        if (brk) {
            if (p->b.vt != NULL)
                p->unread += p->b.vt->len(p->b.self);
            p->b.vt = NULL;
            p->b.self = NULL;
        }
        *dst = error_retain(arena_allocator(&p->err_arena), err);
        hp_close_done_locked(p);
    }
    sync_cond_signal(&p->c);
    sync_mutex_unlock(&p->mu);
}

static const Func hp_no_fn = {NULL, NULL};

void burrow__http2_pipe_close_with_error(Http2Pipe *p, Error err) {
    hp_close_with_error(p, false, err, hp_no_fn);
}

void burrow__http2_pipe_break_with_error(Http2Pipe *p, Error err) {
    hp_close_with_error(p, true, err, hp_no_fn);
}

void burrow__http2_pipe_close_with_error_and_code(Http2Pipe *p, Error err, Func fn) {
    hp_close_with_error(p, false, err, fn);
}

Error burrow__http2_pipe_err(Http2Pipe *p) {
    hp_lock(p);
    Error e = BURROW_FAILED(p->break_err) ? p->break_err : p->err;
    sync_mutex_unlock(&p->mu);
    return hp_out_err(e);
}

Chan *burrow__http2_pipe_done(Http2Pipe *p) {
    hp_lock(p);
    if (p->donec == NULL) {
        p->donec = chan_make(heap_allocator(), TYPE_BOOL, 0);
        if (p->donec == NULL)
            panic_str(BURROW_S("http2: out of memory making a pipe's done channel"));
        /* Already hit an error. */
        if (BURROW_FAILED(p->err) || BURROW_FAILED(p->break_err))
            hp_close_done_locked(p);
    }
    Chan *c = p->donec;
    sync_mutex_unlock(&p->mu);
    return c;
}

static Int hp_io_read(void *self, Slice d, Error *err) {
    return burrow__http2_pipe_read((Http2Pipe *)self, d, err);
}

static Int hp_io_write(void *self, Slice d, Error *err) {
    return burrow__http2_pipe_write((Http2Pipe *)self, d, err);
}

static const IoReaderVT hp_reader_vt = {NULL, hp_io_read};
static const IoWriterVT hp_writer_vt = {NULL, hp_io_write};

IoReader burrow__http2_pipe_as_io_reader(Http2Pipe *p) {
    IoReader r = {&hp_reader_vt, p};
    return r;
}

IoWriter burrow__http2_pipe_as_io_writer(Http2Pipe *p) {
    IoWriter w = {&hp_writer_vt, p};
    return w;
}

void burrow__http2_pipe_free(Http2Pipe *p) {
    if (p->donec != NULL)
        chan_free(p->donec);
    p->donec = NULL;
    p->err = BURROW_NO_ERROR;
    p->break_err = BURROW_NO_ERROR;
    arena_free(&p->err_arena);
}

/* bytes.Buffer as a pipeBuffer, which Go's tests use. */
static Int hp_bb_len(void *self) {
    return bytes_buffer_len((BytesBuffer *)self);
}

static Int hp_bb_read(void *self, Slice p, Error *err) {
    return bytes_buffer_read((BytesBuffer *)self, p, err);
}

static Int hp_bb_write(void *self, Slice p, Error *err) {
    return bytes_buffer_write((BytesBuffer *)self, p, err);
}

static const Http2PipeBufferVT hp_bytes_buffer_vt = {hp_bb_len, hp_bb_read,
                                                     hp_bb_write};

Http2PipeBuffer burrow__http2_bytes_buffer_as_pipe_buffer(BytesBuffer *b) {
    Http2PipeBuffer pb = {&hp_bytes_buffer_vt, b};
    return pb;
}
