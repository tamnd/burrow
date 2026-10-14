/* Derived from Go's src/net/http/httputil/persist.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http/httputil.h"

#include "http_internal.h"

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/textproto.h"
#include "burrow/sync.h"

#include <stdbool.h>
#include <stddef.h>

static const HttpProtocolError pc_persist_eof = {
    BURROW_S_INIT("persistent connection closed")};
static const HttpProtocolError pc_closed = {BURROW_S_INIT("connection closed by user")};
static const HttpProtocolError pc_pipeline = {BURROW_S_INIT("pipeline error")};

const Error httputil_err_persist_eof = {&burrow__http_protocol_error_vt,
                                        &pc_persist_eof};
const Error httputil_err_closed = {&burrow__http_protocol_error_vt, &pc_closed};
const Error httputil_err_pipeline = {&burrow__http_protocol_error_vt, &pc_pipeline};

/* errClosed, a use of the local side after it was closed, where
 * httputil_err_persist_eof is the remote side closing. */
static const Str pc_err_closed_text =
    BURROW_S_INIT("i/o operation on closed connection");
static const Error pc_err_closed = {&burrow_sentinel_error_vt, &pc_err_closed_text};

static const Str pc_err_pipe_count_text = BURROW_S_INIT("persist server pipe count");
static const Error pc_err_pipe_count = {&burrow_sentinel_error_vt,
                                        &pc_err_pipe_count_text};

static bool pc_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* An error to keep in a conn, past the goroutine that got it. */
static Error pc_keep(Arena *errs, Error err) {
    return error_retain(arena_allocator(errs), err);
}

static Error pc_close_body(IoReadCloser body) {
    if (body.vt == NULL)
        return BURROW_NO_ERROR;
    return body.vt->closer.close(body.data);
}

/* ------------------------------------------------------------------ pipereq
 *
 * Go's pipereq map, from a request to the turn it took in the pipeline. There
 * are only as many entries as requests waiting for an answer, so a list does.
 * Room for one more is made before a request is read or written, so adding it
 * after cannot fail. */

static bool pc_reserve(Alloc *a, burrow__HttputilPipeReq **p, Int n, Int *cap) {
    if (n < *cap)
        return true;
    Int ncap = *cap == 0 ? 4 : *cap * 2;
    burrow__HttputilPipeReq *np = (burrow__HttputilPipeReq *)mem_realloc(
        a, *p, (size_t)*cap * sizeof **p, (size_t)ncap * sizeof **p,
        _Alignof(burrow__HttputilPipeReq));
    if (np == NULL)
        return false;
    *p = np;
    *cap = ncap;
    return true;
}

/* Takes req's entry out of the list and gives its id, or false when it has
 * none. */
static bool pc_take(burrow__HttputilPipeReq *p, Int *n, const HttpRequest *req,
                    Uint *id) {
    for (Int i = 0; i < *n; i++) {
        if (p[i].req == req) {
            *id = p[i].id;
            p[i] = p[*n - 1];
            (*n)--;
            return true;
        }
    }
    return false;
}

static void pc_pipereq_free(Alloc *a, burrow__HttputilPipeReq *p, Int cap) {
    if (p != NULL)
        mem_free(a, p, (size_t)cap * sizeof *p, _Alignof(burrow__HttputilPipeReq));
}

/* --------------------------------------------------------------- ServerConn */

HttputilServerConn *httputil_new_server_conn(Alloc *a, NetConn c, BufioReader *r) {
    HttputilServerConn *sc =
        (HttputilServerConn *)mem_alloc(a, sizeof *sc, _Alignof(HttputilServerConn));
    if (sc == NULL)
        return NULL;
    if (r == NULL) {
        r = bufio_new_reader(a, net_conn_as_io_reader(c));
        if (r == NULL) {
            mem_free(a, sc, sizeof *sc, _Alignof(HttputilServerConn));
            return NULL;
        }
        sc->made_r = r;
    }
    sc->c = c;
    sc->r = r;
    sc->a = a;
    arena_init(&sc->errs, a, 0);
    return sc;
}

void httputil_server_conn_free(HttputilServerConn *sc) {
    if (sc == NULL)
        return;
    /* The last request is in pipereq too until it has been answered. */
    for (Int i = 0; i < sc->npipereq; i++)
        http_request_free(sc->pipereq[i].req);
    if (sc->last != NULL && sc->last_answered)
        http_request_free(sc->last);
    pc_pipereq_free(sc->a, sc->pipereq, sc->cappipereq);
    if (sc->made_r != NULL)
        bufio_reader_free(sc->made_r);
    arena_free(&sc->errs);
    mem_free(sc->a, sc, sizeof *sc, _Alignof(HttputilServerConn));
}

NetConn httputil_server_conn_hijack(HttputilServerConn *sc, BufioReader **r) {
    sync_mutex_lock(&sc->mu);
    NetConn c = sc->c;
    BufioReader *br = sc->r;
    sc->c = (NetConn){NULL, NULL};
    sc->r = NULL;
    if (r != NULL) {
        *r = br;
        if (br != NULL && br == sc->made_r)
            sc->made_r = NULL;
    }
    sync_mutex_unlock(&sc->mu);
    return c;
}

Error httputil_server_conn_close(HttputilServerConn *sc) {
    NetConn c = httputil_server_conn_hijack(sc, NULL);
    if (c.vt != NULL)
        return c.vt->closer.close(c.data);
    return BURROW_NO_ERROR;
}

/* What Read does once it has its turn: NULL with *err set, or the request. */
static HttpRequest *pc_server_read(HttputilServerConn *sc, Error *err) {
    sync_mutex_lock(&sc->mu);
    if (BURROW_FAILED(sc->we)) { /* no point receiving if the write side is broken */
        *err = sc->we;
        sync_mutex_unlock(&sc->mu);
        return NULL;
    }
    if (BURROW_FAILED(sc->re)) {
        *err = sc->re;
        sync_mutex_unlock(&sc->mu);
        return NULL;
    }
    if (sc->r == NULL) { /* closed by the user in the meantime */
        *err = pc_err_closed;
        sync_mutex_unlock(&sc->mu);
        return NULL;
    }
    if (!pc_reserve(sc->a, &sc->pipereq, sc->npipereq, &sc->cappipereq)) {
        *err = burrow_err_out_of_memory;
        sync_mutex_unlock(&sc->mu);
        return NULL;
    }
    BufioReader *r = sc->r;
    HttpRequest *last = sc->last;
    IoReadCloser lastbody = sc->lastbody;
    sc->lastbody = (IoReadCloser){NULL, NULL};
    sync_mutex_unlock(&sc->mu);

    /* Make sure the body was all read, even if the user did not close it. A
     * write may answer last while this runs, and leaves it for here to free. */
    if (last != NULL) {
        Error e = pc_close_body(lastbody);
        sync_mutex_lock(&sc->mu);
        if (BURROW_FAILED(e)) {
            sc->re = pc_keep(&sc->errs, e);
            *err = sc->re;
        }
        if (sc->last_answered)
            http_request_free(last);
        sc->last = NULL;
        sc->last_answered = false;
        sync_mutex_unlock(&sc->mu);
        if (BURROW_FAILED(e))
            return NULL;
    }

    Error e;
    HttpRequest *req = http_read_request(sc->a, r, &e);
    sync_mutex_lock(&sc->mu);
    if (req == NULL) {
        /* A close from the client is taken as a graceful one, even with some
         * data before it that did not parse. */
        if (pc_same(e, io_err_unexpected_eof))
            sc->re = httputil_err_persist_eof;
        else
            sc->re = pc_keep(&sc->errs, e);
        *err = sc->re;
        sync_mutex_unlock(&sc->mu);
        return NULL;
    }
    sc->last = req;
    sc->lastbody = req->body;
    sc->nread++;
    *err = BURROW_NO_ERROR;
    if (req->close) {
        sc->re = httputil_err_persist_eof;
        *err = sc->re;
    }
    sync_mutex_unlock(&sc->mu);
    return req;
}

HttpRequest *httputil_server_conn_read(HttputilServerConn *sc, Error *err) {
    /* Reads and writes take their turns in order. */
    Uint id = textproto_pipeline_next(&sc->pipe);
    textproto_pipeline_start_request(&sc->pipe, id);
    HttpRequest *req = pc_server_read(sc, err);
    textproto_pipeline_end_request(&sc->pipe, id);
    if (req == NULL) {
        textproto_pipeline_start_response(&sc->pipe, id);
        textproto_pipeline_end_response(&sc->pipe, id);
    } else {
        /* Remember the turn this request took. */
        sync_mutex_lock(&sc->mu);
        sc->pipereq[sc->npipereq++] = (burrow__HttputilPipeReq){req, id};
        sync_mutex_unlock(&sc->mu);
    }
    return req;
}

Int httputil_server_conn_pending(HttputilServerConn *sc) {
    sync_mutex_lock(&sc->mu);
    Int n = sc->nread - sc->nwritten;
    sync_mutex_unlock(&sc->mu);
    return n;
}

static Error pc_server_write(HttputilServerConn *sc, HttpResponse *resp) {
    sync_mutex_lock(&sc->mu);
    if (BURROW_FAILED(sc->we)) {
        Error e = sc->we;
        sync_mutex_unlock(&sc->mu);
        return e;
    }
    if (sc->c.vt == NULL) { /* closed by the user in the meantime */
        sync_mutex_unlock(&sc->mu);
        return httputil_err_closed;
    }
    NetConn c = sc->c;
    if (sc->nread <= sc->nwritten) {
        sync_mutex_unlock(&sc->mu);
        return pc_err_pipe_count;
    }
    /* After saying the connection is to close, any requests waiting unread are
     * lost. Draining them first is up to the user. */
    if (resp->close)
        sc->re = httputil_err_persist_eof;
    sync_mutex_unlock(&sc->mu);

    Error err = http_response_write(resp, net_conn_as_io_writer(c));
    sync_mutex_lock(&sc->mu);
    if (BURROW_FAILED(err)) {
        sc->we = pc_keep(&sc->errs, err);
        err = sc->we;
        sync_mutex_unlock(&sc->mu);
        return err;
    }
    sc->nwritten++;
    sync_mutex_unlock(&sc->mu);
    return BURROW_NO_ERROR;
}

Error httputil_server_conn_write(HttputilServerConn *sc, HttpRequest *req,
                                 HttpResponse *resp) {
    /* The turn this request/response pair has. */
    Uint id;
    sync_mutex_lock(&sc->mu);
    bool ok = pc_take(sc->pipereq, &sc->npipereq, req, &id);
    sync_mutex_unlock(&sc->mu);
    if (!ok)
        return httputil_err_pipeline;

    textproto_pipeline_start_response(&sc->pipe, id);
    Error err = pc_server_write(sc, resp);
    /* req is answered, and goes now, unless its body is the one the next read
     * closes, in which case that read frees it after. */
    sync_mutex_lock(&sc->mu);
    if (req == sc->last)
        sc->last_answered = true;
    else
        http_request_free(req);
    sync_mutex_unlock(&sc->mu);
    textproto_pipeline_end_response(&sc->pipe, id);
    return err;
}

/* --------------------------------------------------------------- ClientConn */

HttputilClientConn *httputil_new_client_conn(Alloc *a, NetConn c, BufioReader *r) {
    HttputilClientConn *cc =
        (HttputilClientConn *)mem_alloc(a, sizeof *cc, _Alignof(HttputilClientConn));
    if (cc == NULL)
        return NULL;
    if (r == NULL) {
        r = bufio_new_reader(a, net_conn_as_io_reader(c));
        if (r == NULL) {
            mem_free(a, cc, sizeof *cc, _Alignof(HttputilClientConn));
            return NULL;
        }
        cc->made_r = r;
    }
    cc->c = c;
    cc->r = r;
    cc->a = a;
    arena_init(&cc->errs, a, 0);
    return cc;
}

HttputilClientConn *httputil_new_proxy_client_conn(Alloc *a, NetConn c,
                                                   BufioReader *r) {
    HttputilClientConn *cc = httputil_new_client_conn(a, c, r);
    if (cc != NULL)
        cc->proxy = true;
    return cc;
}

void httputil_client_conn_free(HttputilClientConn *cc) {
    if (cc == NULL)
        return;
    http_response_free(cc->last);
    pc_pipereq_free(cc->a, cc->pipereq, cc->cappipereq);
    if (cc->made_r != NULL)
        bufio_reader_free(cc->made_r);
    arena_free(&cc->errs);
    mem_free(cc->a, cc, sizeof *cc, _Alignof(HttputilClientConn));
}

NetConn httputil_client_conn_hijack(HttputilClientConn *cc, BufioReader **r) {
    sync_mutex_lock(&cc->mu);
    NetConn c = cc->c;
    BufioReader *br = cc->r;
    cc->c = (NetConn){NULL, NULL};
    cc->r = NULL;
    if (r != NULL) {
        *r = br;
        if (br != NULL && br == cc->made_r)
            cc->made_r = NULL;
    }
    sync_mutex_unlock(&cc->mu);
    return c;
}

Error httputil_client_conn_close(HttputilClientConn *cc) {
    NetConn c = httputil_client_conn_hijack(cc, NULL);
    if (c.vt != NULL)
        return c.vt->closer.close(c.data);
    return BURROW_NO_ERROR;
}

/* What Write does once it has its turn. *forget says whether to forget the
 * request, which Go's Write does only for an error from writing it: the ones
 * it returns before that leave its err nil, and it remembers the request. */
static Error pc_client_write(HttputilClientConn *cc, HttpRequest *req, bool *forget) {
    *forget = false;
    sync_mutex_lock(&cc->mu);
    if (!pc_reserve(cc->a, &cc->pipereq, cc->npipereq, &cc->cappipereq)) {
        *forget = true;
        sync_mutex_unlock(&cc->mu);
        return burrow_err_out_of_memory;
    }
    if (BURROW_FAILED(cc->re)) { /* no point sending if the read side is done */
        Error e = cc->re;
        sync_mutex_unlock(&cc->mu);
        return e;
    }
    if (BURROW_FAILED(cc->we)) {
        Error e = cc->we;
        sync_mutex_unlock(&cc->mu);
        return e;
    }
    if (cc->c.vt == NULL) { /* closed by the user in the meantime */
        sync_mutex_unlock(&cc->mu);
        return pc_err_closed;
    }
    NetConn c = cc->c;
    /* The end goes in the write side's error, since there may still be
     * responses to read. */
    if (req->close)
        cc->we = httputil_err_persist_eof;
    sync_mutex_unlock(&cc->mu);

    IoWriter w = net_conn_as_io_writer(c);
    Error err =
        cc->proxy ? http_request_write_proxy(req, w) : http_request_write(req, w);
    sync_mutex_lock(&cc->mu);
    if (BURROW_FAILED(err)) {
        *forget = true;
        cc->we = pc_keep(&cc->errs, err);
        err = cc->we;
        sync_mutex_unlock(&cc->mu);
        return err;
    }
    cc->nwritten++;
    sync_mutex_unlock(&cc->mu);
    return BURROW_NO_ERROR;
}

Error httputil_client_conn_write(HttputilClientConn *cc, HttpRequest *req) {
    /* Writes take their turns in order. */
    Uint id = textproto_pipeline_next(&cc->pipe);
    textproto_pipeline_start_request(&cc->pipe, id);
    bool forget;
    Error err = pc_client_write(cc, req, &forget);
    textproto_pipeline_end_request(&cc->pipe, id);
    if (forget) {
        textproto_pipeline_start_response(&cc->pipe, id);
        textproto_pipeline_end_response(&cc->pipe, id);
    } else {
        /* Remember the turn this request took. */
        sync_mutex_lock(&cc->mu);
        cc->pipereq[cc->npipereq++] = (burrow__HttputilPipeReq){req, id};
        sync_mutex_unlock(&cc->mu);
    }
    return err;
}

Int httputil_client_conn_pending(HttputilClientConn *cc) {
    sync_mutex_lock(&cc->mu);
    Int n = cc->nwritten - cc->nread;
    sync_mutex_unlock(&cc->mu);
    return n;
}

static HttpResponse *pc_client_read(HttputilClientConn *cc, HttpRequest *req,
                                    Error *err) {
    sync_mutex_lock(&cc->mu);
    if (BURROW_FAILED(cc->re)) {
        *err = cc->re;
        sync_mutex_unlock(&cc->mu);
        return NULL;
    }
    if (cc->r == NULL) { /* closed by the user in the meantime */
        *err = pc_err_closed;
        sync_mutex_unlock(&cc->mu);
        return NULL;
    }
    BufioReader *r = cc->r;
    HttpResponse *last = cc->last;
    IoReadCloser lastbody = cc->lastbody;
    cc->last = NULL;
    cc->lastbody = (IoReadCloser){NULL, NULL};
    sync_mutex_unlock(&cc->mu);

    /* Make sure the body was all read, even if the user did not close it. */
    if (last != NULL) {
        Error e = pc_close_body(lastbody);
        if (BURROW_FAILED(e)) {
            sync_mutex_lock(&cc->mu);
            cc->re = pc_keep(&cc->errs, e);
            *err = cc->re;
            sync_mutex_unlock(&cc->mu);
        }
        http_response_free(last);
        if (BURROW_FAILED(e))
            return NULL;
    }

    Error e;
    HttpResponse *resp = http_read_response(cc->a, r, req, &e);
    sync_mutex_lock(&cc->mu);
    if (resp == NULL) {
        cc->re = pc_keep(&cc->errs, e);
        *err = cc->re;
        sync_mutex_unlock(&cc->mu);
        return NULL;
    }
    cc->last = resp;
    cc->lastbody = resp->body;
    cc->nread++;
    *err = BURROW_NO_ERROR;
    if (resp->close) { /* send no more requests */
        cc->re = httputil_err_persist_eof;
        *err = cc->re;
    }
    sync_mutex_unlock(&cc->mu);
    return resp;
}

HttpResponse *httputil_client_conn_read(HttputilClientConn *cc, HttpRequest *req,
                                        Error *err) {
    /* The turn this request/response pair has. */
    Uint id;
    sync_mutex_lock(&cc->mu);
    bool ok = pc_take(cc->pipereq, &cc->npipereq, req, &id);
    sync_mutex_unlock(&cc->mu);
    if (!ok) {
        *err = httputil_err_pipeline;
        return NULL;
    }

    textproto_pipeline_start_response(&cc->pipe, id);
    HttpResponse *resp = pc_client_read(cc, req, err);
    textproto_pipeline_end_response(&cc->pipe, id);
    return resp;
}

HttpResponse *httputil_client_conn_do(HttputilClientConn *cc, HttpRequest *req,
                                      Error *err) {
    *err = httputil_client_conn_write(cc, req);
    if (BURROW_FAILED(*err))
        return NULL;
    return httputil_client_conn_read(cc, req, err);
}
