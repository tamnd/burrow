/* Derived from Go's src/net/http/httputil/dump.go, which writes requests and
 * responses out as the bytes that go over the wire.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http/httputil.h"

#include "http_internal.h"

#include "burrow/bufio.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* A drained body: the bytes, and the two readers of them drainBody hands
 * back, one to put back in place and one to read now. */
typedef struct ud_Drained {
    BytesReader r1, r2;
    IoNopCloser c1, c2;
} ud_Drained;

/* drainBody. Reads all of b into memory from a and closes it, and sets *r1 and
 * *r2 to two readers of the same bytes. Go's reads the first one from the
 * buffer itself, so it can only be read once, and so can this. On an error *r2
 * is b, and *r1 is nil. */
static Error ud_drain_body(Alloc *a, IoReadCloser b, IoReadCloser *r1,
                           IoReadCloser *r2) {
    if (b.vt == NULL || b.vt == http_no_body.vt) {
        *r1 = http_no_body;
        *r2 = http_no_body;
        return BURROW_NO_ERROR;
    }
    *r1 = (IoReadCloser){NULL, NULL};
    *r2 = b;
    Error err;
    BytesBuffer buf = BYTES_BUFFER(a);
    (void)bytes_buffer_read_from(&buf, io_read_closer_as_io_reader(b), &err);
    if (BURROW_FAILED(err)) {
        bytes_buffer_free(&buf);
        return err;
    }
    err = b.vt->closer.close(b.data);
    if (BURROW_FAILED(err)) {
        bytes_buffer_free(&buf);
        return err;
    }
    ud_Drained *d = (ud_Drained *)mem_alloc(a, sizeof *d, _Alignof(ud_Drained));
    if (d == NULL) {
        bytes_buffer_free(&buf);
        return burrow_err_out_of_memory;
    }
    Slice data = bytes_buffer_bytes(&buf);
    bytes_reader_reset(&d->r1, data);
    bytes_reader_reset(&d->r2, data);
    d->c1 = io_nop_closer(bytes_reader_as_io_reader(&d->r1));
    d->c2 = io_nop_closer(bytes_reader_as_io_reader(&d->r2));
    *r1 = io_nop_closer_as_io_read_closer(&d->c1);
    *r2 = io_nop_closer_as_io_read_closer(&d->c2);
    return BURROW_NO_ERROR;
}

static Slice ud_nil_slice(void) {
    Slice s;
    memset(&s, 0, sizeof s);
    return s;
}

/* ------------------------------------------------------------ DumpRequestOut */

/* neverEnding('x'), a reader that never runs out of x. */
static Int ud_never_ending_read(void *self, Slice p, Error *err) {
    (void)self;
    memset(p.p, 'x', (size_t)p.len);
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoReaderVT ud_never_ending_vt = {NULL, ud_never_ending_read};

/* outgoingLength. The length the transport will send req's body with, 0 for
 * none and -1 for not known. */
static int64_t ud_outgoing_length(const HttpRequest *req) {
    if (req->body.vt == NULL || req->body.vt == http_no_body.vt)
        return 0;
    if (req->content_length != 0)
        return req->content_length;
    return -1;
}

/* What DumpRequestOut shares with the connection it dials and the goroutine
 * that reads the request off the other end of the pipe. */
typedef struct ud_Out {
    BytesBuffer buf; /* records the output */
    IoPipeReader *pr;
    IoPipeWriter *pw;
    IoWriter mw; /* to buf and pw */

    /* delegateReader. c gets a value when the response is there to be read,
     * and is closed instead when the round trip failed, in which case a read
     * gives err. */
    Chan *c;
    Error err;
    bool have;
    StringsReader res;

    Chan *quit; /* quitReadCh */
    SyncWaitGroup wg;
} ud_Out;

static Int ud_conn_read(void *self, Slice p, Error *err) {
    ud_Out *o = (ud_Out *)self;
    if (!o->have) {
        bool v;
        if (!chan_recv(o->c, &v)) {
            *err = o->err;
            return 0;
        }
        o->have = true;
    }
    return strings_reader_read(&o->res, p, err);
}

static Int ud_conn_write(void *self, Slice p, Error *err) {
    ud_Out *o = (ud_Out *)self;
    return o->mw.vt->write(o->mw.data, p, err);
}

static Error ud_conn_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static NetAddr ud_conn_addr(void *self) {
    (void)self;
    return (NetAddr){NULL, NULL};
}

static Error ud_conn_set_deadline(void *self, Time t) {
    (void)self;
    (void)t;
    return BURROW_NO_ERROR;
}

static const NetConnVT ud_conn_vt = {
    {NULL, ud_conn_read}, {NULL, ud_conn_write}, {NULL, ud_conn_close},
    ud_conn_addr,         ud_conn_addr,          ud_conn_set_deadline,
    ud_conn_set_deadline, ud_conn_set_deadline,
};

static NetConn ud_dial(void *env, Str network, Str addr, Error *err) {
    (void)network;
    (void)addr;
    *err = BURROW_NO_ERROR;
    return (NetConn){&ud_conn_vt, env};
}

/* The connection is the ud_Out, which DumpRequestOut gives back itself. */
static void ud_free_conn(void *env, NetConn c) {
    (void)env;
    (void)c;
}

/* Reads the request the transport writes, and its body, and then has the
 * connection answer it, unless the round trip has failed by then. */
static void ud_read_request(void *env) {
    ud_Out *o = (ud_Out *)env;
    Alloc *h = heap_allocator();
    BufioReader *br = bufio_new_reader(h, io_pipe_reader_as_io_reader(o->pr));
    if (br != NULL) {
        Error err;
        HttpRequest *req = http_read_request(h, br, &err);
        if (req != NULL) {
            (void)io_copy(h, io_discard, io_read_closer_as_io_reader(req->body), &err);
            (void)req->body.vt->closer.close(req->body.data);
            http_request_free(req);
        }
        bufio_reader_free(br);
    }
    bool v = true;
    SelectCase cases[] = {BURROW_SEND(o->c, &v), BURROW_RECV(o->quit, NULL)};
    if (chan_select(cases, 2) == 1)
        chan_close(o->c);
}

static void ud_out_free(ud_Out *o) {
    chan_free(o->quit);
    chan_free(o->c);
    io_multi_writer_free(heap_allocator(), o->mw);
    io_pipe_free(o->pr);
    bytes_buffer_free(&o->buf);
    mem_free(heap_allocator(), o, sizeof *o, _Alignof(ud_Out));
}

static ud_Out *ud_out_new(void) {
    Alloc *h = heap_allocator();
    ud_Out *o = (ud_Out *)mem_alloc(h, sizeof *o, _Alignof(ud_Out));
    if (o == NULL)
        return NULL;
    memset(o, 0, sizeof *o);
    o->buf = BYTES_BUFFER(h);
    io_pipe(h, &o->pr, &o->pw);
    o->c = chan_make(h, TYPE_BOOL, 0);
    o->quit = chan_make(h, TYPE_BOOL, 0);
    if (o->pr != NULL) {
        IoWriter ws[2] = {bytes_buffer_as_io_writer(&o->buf),
                          io_pipe_writer_as_io_writer(o->pw)};
        o->mw = io_multi_writer(h, ws, 2);
    }
    if (o->pr == NULL || o->c == NULL || o->quit == NULL || o->mw.vt == NULL) {
        ud_out_free(o);
        return NULL;
    }
    strings_reader_reset(
        &o->res, BURROW_S("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"));
    return o;
}

Slice httputil_dump_request_out(Alloc *a, HttpRequest *req, bool body, Error *err) {
    IoReadCloser save = req->body;
    bool dummy_body = false;
    IoLimitedReader lr;
    IoNopCloser dummy;
    if (!body) {
        int64_t content_length = ud_outgoing_length(req);
        if (content_length != 0) {
            lr = io_limit_reader((IoReader){&ud_never_ending_vt, NULL}, content_length);
            dummy = io_nop_closer(io_limited_reader_as_io_reader(&lr));
            req->body = io_nop_closer_as_io_read_closer(&dummy);
            dummy_body = true;
        }
    } else {
        *err = ud_drain_body(a, req->body, &save, &req->body);
        if (BURROW_FAILED(*err))
            return ud_nil_slice();
    }

    /* An https request goes as http, so the transport does not try TLS on a
     * connection that is only a buffer. Go sends a copy, and here the URL is
     * swapped for the time of the round trip, which comes to the same thing
     * since req is not to be used elsewhere while this has it. */
    Url *url = req->url;
    Url plain;
    if (url != NULL && str_eq(url->scheme, BURROW_S("https"))) {
        plain = *url;
        plain.scheme = BURROW_S("http");
        req->url = &plain;
    }

    ud_Out *o = ud_out_new();
    if (o == NULL) {
        req->url = url;
        req->body = save;
        *err = burrow_err_out_of_memory;
        return ud_nil_slice();
    }
    HttpTransport t;
    memset(&t, 0, sizeof t);
    t.dial = BURROW_FN(HttpDialFunc, ud_dial, o);
    t.free_conn = BURROW_FN(HttpFreeConnFunc, ud_free_conn, NULL);

    Error e = BURROW_NO_ERROR;
    if (!sync_wait_group_go(&o->wg, BURROW_FN(Func, ud_read_request, o)))
        e = burrow_err_out_of_memory;
    HttpResponse *res = NULL;
    if (!BURROW_FAILED(e))
        res = http_transport_round_trip(&t, req, &e);

    req->url = url;
    req->body = save;
    if (BURROW_FAILED(e)) {
        o->err = e;
        chan_close(o->quit);
    }
    http_response_free(res);
    /* Go leaves its goroutine and the transport's to finish on their own.
     * These have to be done before o goes, and closing the pipe is what lets
     * a reader or writer still on it go. */
    (void)io_pipe_reader_close(o->pr);
    (void)io_pipe_writer_close(o->pw);
    sync_wait_group_wait(&o->wg);
    http_transport_free(&t);

    Slice dump = ud_nil_slice();
    if (!BURROW_FAILED(e)) {
        Slice b = bytes_buffer_bytes(&o->buf);
        Int n = b.len;
        if (dummy_body) {
            Int i = strings_index(str_from_bytes(b.p, b.len), BURROW_S("\r\n\r\n"));
            if (i >= 0)
                n = i + 4;
        }
        dump = slice_make(a, TYPE_BYTE, n, n);
        if (dump.p == NULL && n > 0)
            e = burrow_err_out_of_memory;
        else if (n > 0)
            memcpy(dump.p, b.p, (size_t)n);
    }
    ud_out_free(o);
    *err = e;
    return dump;
}

/* --------------------------------------------------------------- DumpRequest */

/* reqWriteExcludeHeaderDump. */
static const Str ud_req_exclude[] = {
    BURROW_S_INIT("Host"),
    BURROW_S_INIT("Transfer-Encoding"),
    BURROW_S_INIT("Trailer"),
};

static bool ud_has_prefix(Str s, const char *prefix) {
    return strings_has_prefix(s, str_from_cstr(prefix));
}

Slice httputil_dump_request(Alloc *a, HttpRequest *req, bool body, Error *err) {
    IoReadCloser save = req->body;
    IoReadCloser rbody = {NULL, NULL};
    if (body && req->body.vt != NULL) {
        *err = ud_drain_body(a, req->body, &save, &rbody);
        if (BURROW_FAILED(*err))
            return ud_nil_slice();
    }

    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    Alloc *sa = arena_allocator(&scratch);
    BytesBuffer b = BYTES_BUFFER(a);
    IoWriter w = bytes_buffer_as_io_writer(&b);
    Error e = BURROW_NO_ERROR;

    Str req_uri = req->request_uri;
    if (req_uri.len == 0 && req->url != NULL)
        req_uri = url_request_uri(req->url, sa);
    Str method = req->method.len != 0 ? req->method : BURROW_S("GET");
    fmt_fprintf_v(w, "%s %s HTTP/%d.%d\r\n", method, req_uri, req->proto_major,
                  req->proto_minor);

    bool abs_request_uri = ud_has_prefix(req->request_uri, "http://") ||
                           ud_has_prefix(req->request_uri, "https://");
    if (!abs_request_uri) {
        Str host = req->host;
        if (host.len == 0 && req->url != NULL)
            host = req->url->host;
        if (host.len != 0)
            fmt_fprintf_v(w, "Host: %s\r\n", host);
    }

    const Str *te = (const Str *)req->transfer_encoding.p;
    bool chunked = req->transfer_encoding.len > 0 && str_eq(te[0], BURROW_S("chunked"));
    if (req->transfer_encoding.len > 0)
        fmt_fprintf_v(w, "Transfer-Encoding: %s\r\n",
                      strings_join(sa, req->transfer_encoding, BURROW_S(",")));

    e = burrow__http_header_write_except(
        req->header, w, ud_req_exclude,
        (Int)(sizeof ud_req_exclude / sizeof ud_req_exclude[0]), NULL);
    if (!BURROW_FAILED(e)) {
        (void)bytes_buffer_write_string(&b, BURROW_S("\r\n"), &e);
        if (rbody.vt != NULL) {
            HttpChunkedWriter cw = {w, NULL};
            IoWriter dest = w;
            if (chunked)
                dest = io_write_closer_as_io_writer(
                    burrow__http_chunked_writer_as_io_write_closer(&cw));
            (void)io_copy(sa, dest, io_read_closer_as_io_reader(rbody), &e);
            if (chunked) {
                (void)burrow__http_chunked_writer_close(&cw);
                Error ignored;
                (void)bytes_buffer_write_string(&b, BURROW_S("\r\n"), &ignored);
            }
        }
    }
    arena_free(&scratch);

    req->body = save;
    *err = e;
    if (BURROW_FAILED(e)) {
        bytes_buffer_free(&b);
        return ud_nil_slice();
    }
    return bytes_buffer_bytes(&b);
}

/* -------------------------------------------------------------- DumpResponse */

/* errNoBody, what failureToReadBody gives instead of the body. */
BURROW_SENTINEL_ERROR(ud_err_no_body, "sentinel error value");

static Int ud_failure_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    *err = ud_err_no_body;
    return 0;
}

static Int ud_empty_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    *err = io_eof;
    return 0;
}

static Error ud_nop_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

/* failureToReadBody, and emptyBody, which is a reader of "" in Go. */
static const IoReadCloserVT ud_failure_vt = {{NULL, ud_failure_read},
                                             {NULL, ud_nop_close}};
static const IoReadCloserVT ud_empty_vt = {{NULL, ud_empty_read}, {NULL, ud_nop_close}};

Slice httputil_dump_response(Alloc *a, HttpResponse *resp, bool body, Error *err) {
    IoReadCloser save = resp->body;
    int64_t savecl = resp->content_length;

    if (!body) {
        if (resp->content_length == 0)
            resp->body = (IoReadCloser){&ud_empty_vt, NULL};
        else
            resp->body = (IoReadCloser){&ud_failure_vt, NULL};
    } else if (resp->body.vt == NULL) {
        resp->body = (IoReadCloser){&ud_empty_vt, NULL};
    } else {
        *err = ud_drain_body(a, resp->body, &save, &resp->body);
        if (BURROW_FAILED(*err))
            return ud_nil_slice();
    }
    BytesBuffer b = BYTES_BUFFER(a);
    Error e = http_response_write(resp, bytes_buffer_as_io_writer(&b));
    if (errors_is(e, ud_err_no_body))
        e = BURROW_NO_ERROR;
    resp->body = save;
    resp->content_length = savecl;
    *err = e;
    if (BURROW_FAILED(e)) {
        bytes_buffer_free(&b);
        return ud_nil_slice();
    }
    return bytes_buffer_bytes(&b);
}
