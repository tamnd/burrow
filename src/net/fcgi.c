/* Derived from Go's src/net/http/fcgi/fcgi.go, the raw protocol and some
 * utilities used by the child and the host.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http/fcgi.h"

#include "fcgi_internal.h"

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/sync.h"

#include <stdint.h>
#include <string.h>

/* for padding so we don't have to allocate all the time */
static const Byte fcg_pad[BURROW__FCGI_MAX_PAD] = {0};

BURROW_SENTINEL_ERROR(fcg_err_invalid_version, "fcgi: invalid header version");

static void fcg_put_uint16(Byte *b, uint16_t v) {
    b[0] = (Byte)(v >> 8);
    b[1] = (Byte)v;
}

static void fcg_put_uint32(Byte *b, uint32_t v) {
    b[0] = (Byte)(v >> 24);
    b[1] = (Byte)(v >> 16);
    b[2] = (Byte)(v >> 8);
    b[3] = (Byte)v;
}

static void fcg_header_init(burrow__FcgiHeader *h, uint8_t rec_type, uint16_t req_id,
                            Int content_length) {
    h->version = 1;
    h->type = rec_type;
    h->id = req_id;
    h->content_length = (uint16_t)content_length;
    h->padding_length = (uint8_t)(-content_length & 7);
    h->reserved = 0;
}

burrow__FcgiConn *burrow__fcgi_new_conn(Alloc *a, IoReader r, IoWriter w, IoCloser c) {
    burrow__FcgiConn *conn =
        (burrow__FcgiConn *)mem_alloc(a, sizeof *conn, _Alignof(burrow__FcgiConn));
    if (conn == NULL)
        return NULL;
    memset(conn, 0, sizeof *conn);
    conn->r = r;
    conn->w = w;
    conn->c = c;
    return conn;
}

/* Close closes the conn if it is not already closed. */
Error burrow__fcgi_conn_close(burrow__FcgiConn *c) {
    sync_mutex_lock(&c->mutex);
    if (!c->closed) {
        c->close_err = c->c.vt->close(c->c.data);
        c->closed = true;
    }
    Error err = c->close_err;
    sync_mutex_unlock(&c->mutex);
    return err;
}

Error burrow__fcgi_record_read(burrow__FcgiRecord *rec, IoReader r) {
    Byte h[8];
    Error err = BURROW_NO_ERROR;
    (void)io_read_full(r, (Slice){h, 8, 8, NULL}, &err);
    if (BURROW_FAILED(err))
        return err;
    rec->h.version = h[0];
    rec->h.type = h[1];
    rec->h.id = (uint16_t)((uint16_t)h[2] << 8 | h[3]);
    rec->h.content_length = (uint16_t)((uint16_t)h[4] << 8 | h[5]);
    rec->h.padding_length = h[6];
    rec->h.reserved = h[7];
    if (rec->h.version != 1)
        return fcg_err_invalid_version;
    Int n = (Int)rec->h.content_length + (Int)rec->h.padding_length;
    (void)io_read_full(r, (Slice){rec->buf, n, n, NULL}, &err);
    return err;
}

Slice burrow__fcgi_record_content(burrow__FcgiRecord *rec) {
    Int n = rec->h.content_length;
    return (Slice){rec->buf, n, n, NULL};
}

/* writeRecord writes and sends a single record. */
Error burrow__fcgi_conn_write_record(burrow__FcgiConn *c, uint8_t rec_type,
                                     uint16_t req_id, Slice b) {
    sync_mutex_lock(&c->mutex);
    burrow__FcgiHeader h;
    fcg_header_init(&h, rec_type, req_id, b.len);
    Byte *buf = c->buf;
    buf[0] = h.version;
    buf[1] = h.type;
    fcg_put_uint16(buf + 2, h.id);
    fcg_put_uint16(buf + 4, h.content_length);
    buf[6] = h.padding_length;
    buf[7] = h.reserved;
    if (b.len > 0)
        memcpy(buf + 8, b.p, (size_t)b.len);
    memcpy(buf + 8 + b.len, fcg_pad, h.padding_length);
    Int n = 8 + b.len + h.padding_length;
    Error err = BURROW_NO_ERROR;
    (void)c->w.vt->write(c->w.data, (Slice){buf, n, n, NULL}, &err);
    sync_mutex_unlock(&c->mutex);
    return err;
}

Error burrow__fcgi_conn_write_end_request(burrow__FcgiConn *c, uint16_t req_id,
                                          Int app_status, uint8_t protocol_status) {
    Byte b[8] = {0};
    fcg_put_uint32(b, (uint32_t)app_status);
    b[4] = protocol_status;
    return burrow__fcgi_conn_write_record(c, BURROW__FCGI_TYPE_END_REQUEST, req_id,
                                          (Slice){b, 8, 8, NULL});
}

Error burrow__fcgi_conn_write_pairs(burrow__FcgiConn *c, uint8_t rec_type,
                                    uint16_t req_id, Map *pairs) {
    /* The writer's buffer is MAX_WRITE bytes, too much for the stack, and
     * nothing else here has an allocator to hand. */
    Alloc *a = heap_allocator();
    burrow__FcgiWriter *w = burrow__fcgi_new_writer(a, c, rec_type, req_id);
    if (w == NULL)
        return burrow_err_out_of_memory;
    Error err = BURROW_NO_ERROR;
    Byte b[8];
    const void *kp;
    void *vp;
    for (MapIter it = map_iter(pairs); map_next(&it, &kp, &vp);) {
        Str k = *(const Str *)kp, v = *(const Str *)vp;
        Int n = burrow__fcgi_encode_size(b, (uint32_t)k.len);
        n += burrow__fcgi_encode_size(b + n, (uint32_t)v.len);
        (void)burrow__fcgi_writer_write(w, (Slice){b, n, n, NULL}, &err);
        if (BURROW_FAILED(err))
            goto out;
        (void)burrow__fcgi_writer_write(
            w, (Slice){(void *)(uintptr_t)k.p, k.len, k.len, NULL}, &err);
        if (BURROW_FAILED(err))
            goto out;
        (void)burrow__fcgi_writer_write(
            w, (Slice){(void *)(uintptr_t)v.p, v.len, v.len, NULL}, &err);
        if (BURROW_FAILED(err))
            goto out;
    }
    err = burrow__fcgi_writer_close(w);
out:
    burrow__fcgi_writer_free(a, w);
    return err;
}

uint32_t burrow__fcgi_read_size(Slice s, Int *n) {
    const Byte *p = (const Byte *)s.p;
    *n = 0;
    if (s.len == 0)
        return 0;
    uint32_t size = p[0];
    *n = 1;
    if ((size & (1U << 7)) != 0) {
        if (s.len < 4) {
            *n = 0;
            return 0;
        }
        *n = 4;
        size = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        size &= ~(1U << 31);
    }
    return size;
}

Int burrow__fcgi_encode_size(Byte *b, uint32_t size) {
    if (size > 127) {
        size |= 1U << 31;
        fcg_put_uint32(b, size);
        return 4;
    }
    b[0] = (Byte)size;
    return 1;
}

/* streamWriter abstracts out the separation of a stream into discrete records.
 * It only writes maxWrite bytes at a time. bufWriter, around it, is a
 * bufio.Writer that also closes the stream when closed. */
struct burrow__FcgiWriter {
    burrow__FcgiConn *c;
    uint8_t rec_type;
    uint16_t req_id;
    BufioWriter *w;
};

static Int fcg_stream_write(void *self, Slice p, Error *err) {
    burrow__FcgiWriter *w = (burrow__FcgiWriter *)self;
    Int nn = 0;
    while (p.len > 0) {
        Int n = p.len;
        if (n > BURROW__FCGI_MAX_WRITE)
            n = BURROW__FCGI_MAX_WRITE;
        Error e = burrow__fcgi_conn_write_record(w->c, w->rec_type, w->req_id,
                                                 (Slice){p.p, n, n, NULL});
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return nn;
        }
        nn += n;
        p = (Slice){(Byte *)p.p + n, p.len - n, p.len - n, NULL};
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return nn;
}

static const IoWriterVT fcg_stream_vt = {NULL, fcg_stream_write};

/* streamWriter.Close: send empty record to close the stream */
static Error fcg_stream_close(burrow__FcgiWriter *w) {
    return burrow__fcgi_conn_write_record(w->c, w->rec_type, w->req_id, (Slice){0});
}

burrow__FcgiWriter *burrow__fcgi_new_writer(Alloc *a, burrow__FcgiConn *c,
                                            uint8_t rec_type, uint16_t req_id) {
    burrow__FcgiWriter *w =
        (burrow__FcgiWriter *)mem_alloc(a, sizeof *w, _Alignof(burrow__FcgiWriter));
    if (w == NULL)
        return NULL;
    w->c = c;
    w->rec_type = rec_type;
    w->req_id = req_id;
    w->w =
        bufio_new_writer_size(a, (IoWriter){&fcg_stream_vt, w}, BURROW__FCGI_MAX_WRITE);
    if (w->w == NULL) {
        mem_free(a, w, sizeof *w, _Alignof(burrow__FcgiWriter));
        return NULL;
    }
    return w;
}

void burrow__fcgi_writer_free(Alloc *a, burrow__FcgiWriter *w) {
    if (w == NULL)
        return;
    bufio_writer_free(w->w);
    mem_free(a, w, sizeof *w, _Alignof(burrow__FcgiWriter));
}

Int burrow__fcgi_writer_write(burrow__FcgiWriter *w, Slice p, Error *err) {
    return bufio_writer_write(w->w, p, err);
}

IoWriter burrow__fcgi_writer_as_io_writer(burrow__FcgiWriter *w) {
    return bufio_writer_as_io_writer(w->w);
}

Error burrow__fcgi_writer_flush(burrow__FcgiWriter *w) {
    return bufio_writer_flush(w->w);
}

Error burrow__fcgi_writer_close(burrow__FcgiWriter *w) {
    Error err = bufio_writer_flush(w->w);
    if (BURROW_FAILED(err)) {
        (void)fcg_stream_close(w);
        return err;
    }
    return fcg_stream_close(w);
}
