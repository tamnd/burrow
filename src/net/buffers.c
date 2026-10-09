/* net.Buffers, from Go's src/net/net.go: runs of bytes that a connection
 * writes with one writev, and that anything else is written a run at a time.
 *
 * Go source: go1.27.1.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "internal.h"

#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/net.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

/* w.(buffersWriter): the four conns, which wasip1 and js do not give one to
 * since Go's writeBuffers is for unix and windows only. */
burrow__NetConnCore *burrow__net_buffers_writer(IoWriter w) {
#if defined(BURROW_OS_WASI) || defined(BURROW_OS_JS)
    (void)w;
    return NULL;
#else
    if (w.vt == NULL || w.data == NULL)
        return NULL;
    if (w.vt == burrow__nt_conn_writer || w.vt == burrow__nu_conn_writer ||
        w.vt == burrow__ir_conn_writer || w.vt == burrow__nx_conn_writer ||
        w.vt == burrow__nt_plain_writer)
        return (burrow__NetConnCore *)w.data;
    return NULL;
#endif
}

int64_t net_buffers_write_to(NetBuffers *v, IoWriter w, Error *err) {
    burrow__NetConnCore *c = burrow__net_buffers_writer(w);
    if (c != NULL)
        return burrow__conn_write_buffers(c, v, err);
    int64_t n = 0;
    const Slice *b = (const Slice *)v->p;
    for (Int i = 0; i < v->len; i++) {
        Error e = BURROW_NO_ERROR;
        Int nb = w.vt->write(w.data, b[i], &e);
        n += (int64_t)nb;
        if (BURROW_FAILED(e)) {
            burrow__net_buffers_consume(v, n);
            BURROW_OUT(err, e);
            return n;
        }
    }
    burrow__net_buffers_consume(v, n);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return n;
}

Int net_buffers_read(NetBuffers *v, Slice p, Error *err) {
    Int n = 0;
    while (p.len > 0 && v->len > 0) {
        const Slice *b0 = (const Slice *)v->p;
        Int n0 = b0->len < p.len ? b0->len : p.len;
        if (n0 > 0 && p.p != NULL && b0->p != NULL)
            memmove(p.p, b0->p, (size_t)n0);
        burrow__net_buffers_consume(v, n0);
        p = slice_sub(p, n0, p.len);
        n += n0;
    }
    BURROW_OUT(err, v->len == 0 ? io_eof : BURROW_NO_ERROR);
    return n;
}

/* ------------------------------------------------------------- descriptor */

#define BV_SIG_READ(IN, OUT) IN(0, Bytes) IN(1, IoErrorArg) OUT(Int)

#define BV_METHODS(M, T)                                                               \
    M(T, Read, net_buffers_read, BV_SIG_READ)                                          \
    M(T, WriteTo, net_buffers_write_to, IO_SIG_WRITE_TO)

BURROW_METHODS_DEFINE(NetBuffers, BV_METHODS);

static const Type bv_desc = {
    BURROW_S_INIT("Buffers"),
    BURROW_S_INIT("net"),
    KIND_SLICE,
    (uint32_t)sizeof(Slice),
    (uint16_t)_Alignof(Slice),
    0,
    (uint16_t)(sizeof burrow__methods_NetBuffers /
               sizeof burrow__methods_NetBuffers[0]),
    NULL,
    burrow__methods_NetBuffers,
    TYPE_BYTES,
    NULL,
    0,
    0x6e627566U, /* "nbuf" */
    NULL,
};

const Type *const TYPE_NET_BUFFERS = &bv_desc;

static Int bv_read(void *self, Slice p, Error *err) {
    return net_buffers_read((NetBuffers *)self, p, err);
}

static const IoReaderVT bv_reader_vt = {&bv_desc, bv_read};

IoReader net_buffers_as_io_reader(NetBuffers *v) {
    IoReader r = {NULL, NULL};
    if (v != NULL) {
        r.vt = &bv_reader_vt;
        r.data = v;
    }
    return r;
}
