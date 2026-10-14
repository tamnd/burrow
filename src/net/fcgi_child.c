/* Derived from Go's src/net/http/fcgi/child.go, FastCGI from the perspective
 * of a child process.
 *
 * Go leaves the lifetimes here to its collector. A request is wanted by the
 * child's map of requests and by the goroutine serving it, and a child by its
 * maker and by every request it is serving, so each counts its users and the
 * last one frees it.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http/fcgi.h"

#include "fcgi_internal.h"

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/cgi.h"
#include "burrow/os.h"
#include "burrow/proc.h"
#include "burrow/strings.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

BURROW_SENTINEL_ERROR(fcgi_err_request_aborted, "fcgi: request aborted by web server");
BURROW_SENTINEL_ERROR(fcgi_err_conn_closed, "fcgi: connection to web server closed");

BURROW_SENTINEL_ERROR(fch_err_close_conn, "fcgi: connection should be closed");
BURROW_SENTINEL_ERROR(fch_err_in_flight, "fcgi: received ID that is already in-flight");
BURROW_SENTINEL_ERROR(fch_err_begin_request, "fcgi: invalid begin request record");

/* envVarsContextKey uniquely identifies a mapping of CGI
 * environment variables to their values in a request context */
static const Type fch_env_key_desc = {
    {(const Byte *)"envVarsContextKey", 17},
    {(const Byte *)"net/http/fcgi", 13},
    KIND_INT,
    (uint32_t)sizeof(Int),
    (uint16_t)_Alignof(Int),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x66657663U, /* "fevc" */
    NULL,
};

static const Int fch_env_key_value = 0;

static const Any fch_env_key = {&fch_env_key_desc,
                                (void *)(uintptr_t)&fch_env_key_value};

/* ----------------------------------------------------------- the request */

/* request holds the state for an in-progress request. As soon as it's
 * complete, it's converted to an HttpRequest. */
typedef struct FchRequest {
    Arena arena; /* params and raw_params */
    SyncAtomicInt64 refs;
    IoPipeReader *pr;
    IoPipeWriter *pw;
    uint16_t req_id;
    Map *params; /* of Str to Str */
    Byte *raw_params;
    Int raw_len, raw_cap;
    bool keep_conn;
} FchRequest;

static FchRequest *fch_new_request(uint16_t req_id, uint8_t flags) {
    Alloc *h = heap_allocator();
    FchRequest *r = (FchRequest *)mem_alloc(h, sizeof *r, _Alignof(FchRequest));
    if (r == NULL)
        return NULL;
    memset(r, 0, sizeof *r);
    arena_init(&r->arena, NULL, 0);
    sync_atomic_int64_store(&r->refs, 1);
    r->req_id = req_id;
    r->params = map_make(arena_allocator(&r->arena), TYPE_STRING, TYPE_STRING, 0);
    r->keep_conn = (flags & BURROW__FCGI_FLAG_KEEP_CONN) != 0;
    if (r->params == NULL) {
        arena_free(&r->arena);
        mem_free(h, r, sizeof *r, _Alignof(FchRequest));
        return NULL;
    }
    return r;
}

static void fch_request_release(FchRequest *r) {
    if (sync_atomic_int64_add(&r->refs, -1) != 0)
        return;
    if (r->pr != NULL)
        io_pipe_free(r->pr);
    arena_free(&r->arena);
    mem_free(heap_allocator(), r, sizeof *r, _Alignof(FchRequest));
}

/* append(req.rawParams, content...). False when the arena says no. */
static bool fch_append_params(FchRequest *r, Slice content) {
    if (r->raw_len + content.len > r->raw_cap) {
        Int cap = r->raw_cap == 0 ? 1024 : r->raw_cap;
        while (cap < r->raw_len + content.len)
            cap *= 2;
        Byte *p = (Byte *)mem_alloc(arena_allocator(&r->arena), (size_t)cap, 1);
        if (p == NULL)
            return false;
        if (r->raw_len > 0)
            memcpy(p, r->raw_params, (size_t)r->raw_len);
        r->raw_params = p;
        r->raw_cap = cap;
    }
    memcpy(r->raw_params + r->raw_len, content.p, (size_t)content.len);
    r->raw_len += content.len;
    return true;
}

/* parseParams reads an encoded []byte into Params. The keys and values point
 * into the old raw buffer, which the arena keeps. */
static void fch_parse_params(FchRequest *r) {
    Slice text = {r->raw_params, r->raw_len, r->raw_len, TYPE_BYTE};
    r->raw_params = NULL;
    r->raw_len = r->raw_cap = 0;
    while (text.len > 0) {
        Int n = 0;
        uint32_t key_len = burrow__fcgi_read_size(text, &n);
        if (n == 0)
            return;
        text = slice_sub(text, n, text.len);
        uint32_t val_len = burrow__fcgi_read_size(text, &n);
        if (n == 0)
            return;
        text = slice_sub(text, n, text.len);
        if ((Int)key_len + (Int)val_len > text.len)
            return;
        Str key = str_from_bytes(text.p, (Int)key_len);
        text = slice_sub(text, (Int)key_len, text.len);
        Str val = str_from_bytes(text.p, (Int)val_len);
        text = slice_sub(text, (Int)val_len, text.len);
        (void)map_set(r->params, &key, &val);
    }
}

/* ---------------------------------------------------------- the response */

typedef struct FchResponse {
    FchRequest *req;
    HttpHeader header;
    Int code;
    bool wrote_header;
    bool wrote_cgi_header;
    burrow__FcgiWriter *w;
    Alloc *a;
    Map *env; /* what fcgi_process_env gives, which the context points at */
} FchResponse;

static void fch_write_header(void *self, Int code) {
    FchResponse *r = (FchResponse *)self;
    if (r->wrote_header)
        return;
    r->wrote_header = true;
    r->code = code;
    if (code == HTTP_STATUS_NOT_MODIFIED) {
        /* Must not have body. */
        http_header_del(r->header, BURROW_S("Content-Type"));
        http_header_del(r->header, BURROW_S("Content-Length"));
        http_header_del(r->header, BURROW_S("Transfer-Encoding"));
    }
    if (http_header_get(r->header, BURROW_S("Date")).len == 0)
        (void)http_header_set(
            r->header, BURROW_S("Date"),
            time_format(time_utc(time_now()), r->a, HTTP_TIME_FORMAT));
}

/* writeCGIHeader finalizes the header sent to the client and writes it to the
 * output. p is not written by writeHeader, but is the first chunk of the body
 * that will be written. It is sniffed for a Content-Type if none is set
 * explicitly. */
static void fch_write_cgi_header(FchResponse *r, Slice p) {
    if (r->wrote_cgi_header)
        return;
    r->wrote_cgi_header = true;
    IoWriter w = burrow__fcgi_writer_as_io_writer(r->w);
    (void)fmt_fprintf_v(w, "Status: %d %s\r\n", r->code, http_status_text(r->code));
    Str ct = BURROW_S("Content-Type");
    if (r->code != HTTP_STATUS_NOT_MODIFIED && map_get(r->header, &ct) == NULL)
        (void)http_header_set(r->header, ct, http_detect_content_type(p));
    (void)http_header_write(r->header, w);
    (void)io_write_string(w, BURROW_S("\r\n"), NULL);
    (void)burrow__fcgi_writer_flush(r->w);
}

static Int fch_write(void *self, Slice p, Error *err) {
    FchResponse *r = (FchResponse *)self;
    if (!r->wrote_header)
        fch_write_header(r, HTTP_STATUS_OK);
    if (!r->wrote_cgi_header)
        fch_write_cgi_header(r, p);
    return burrow__fcgi_writer_write(r->w, p, err);
}

static HttpHeader fch_header(void *self) {
    return ((FchResponse *)self)->header;
}

static Error fch_flush(void *self) {
    FchResponse *r = (FchResponse *)self;
    if (!r->wrote_header)
        fch_write_header(r, HTTP_STATUS_OK);
    (void)burrow__fcgi_writer_flush(r->w);
    return BURROW_NO_ERROR;
}

static Error fch_response_close(FchResponse *r) {
    (void)fch_flush(r);
    return burrow__fcgi_writer_close(r->w);
}

static const HttpResponseWriterVT fch_response_vt = {
    {NULL, fch_write},
    fch_header,
    fch_write_header,
    fch_flush,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

/* ------------------------------------------------------------- the child */

struct burrow__FcgiChild {
    burrow__FcgiConn *conn;
    HttpHandler handler;
    Map *requests; /* keyed by request ID, of Int to FchRequest * as Uintptr */
    SyncAtomicInt64 refs;
    NetConn nc;
};

burrow__FcgiChild *burrow__fcgi_new_child(IoReader r, IoWriter w, IoCloser c,
                                          HttpHandler handler) {
    Alloc *h = heap_allocator();
    burrow__FcgiChild *ch =
        (burrow__FcgiChild *)mem_alloc(h, sizeof *ch, _Alignof(burrow__FcgiChild));
    if (ch == NULL)
        return NULL;
    memset(ch, 0, sizeof *ch);
    ch->conn = burrow__fcgi_new_conn(h, r, w, c);
    ch->requests = map_make(h, TYPE_INT, TYPE_UINTPTR, 0);
    if (ch->conn == NULL || ch->requests == NULL) {
        if (ch->conn != NULL)
            mem_free(h, ch->conn, sizeof *ch->conn, _Alignof(burrow__FcgiConn));
        map_free(ch->requests);
        mem_free(h, ch, sizeof *ch, _Alignof(burrow__FcgiChild));
        return NULL;
    }
    ch->handler = handler;
    sync_atomic_int64_store(&ch->refs, 1);
    return ch;
}

void burrow__fcgi_child_set_net_conn(burrow__FcgiChild *c, NetConn nc) {
    c->nc = nc;
}

void burrow__fcgi_child_release(burrow__FcgiChild *c) {
    if (c == NULL || sync_atomic_int64_add(&c->refs, -1) != 0)
        return;
    Alloc *h = heap_allocator();
    const void *kp;
    void *vp;
    for (MapIter it = map_iter(c->requests); map_next(&it, &kp, &vp);)
        fch_request_release((FchRequest *)*(uintptr_t *)vp);
    map_free(c->requests);
    if (c->nc.vt != NULL)
        net_conn_free(c->nc);
    mem_free(h, c->conn, sizeof *c->conn, _Alignof(burrow__FcgiConn));
    mem_free(h, c, sizeof *c, _Alignof(burrow__FcgiChild));
}

static FchRequest *fch_lookup(burrow__FcgiChild *c, uint16_t id) {
    Int k = id;
    uintptr_t *p = (uintptr_t *)map_get(c->requests, &k);
    return p != NULL ? (FchRequest *)*p : NULL;
}

/* delete(c.requests, id), and the map's hold on the request with it. */
static void fch_delete(burrow__FcgiChild *c, FchRequest *req) {
    Int k = req->req_id;
    map_del(c->requests, &k);
    fch_request_release(req);
}

/* cleanUp. */
static void fch_clean_up(burrow__FcgiChild *c) {
    const void *kp;
    void *vp;
    for (MapIter it = map_iter(c->requests); map_next(&it, &kp, &vp);) {
        FchRequest *req = (FchRequest *)*(uintptr_t *)vp;
        if (req->pw != NULL) {
            /* race with call to Close in fch_serve_request doesn't matter
             * because the pipe's Closes are idempotent */
            (void)io_pipe_writer_close_with_error(req->pw, fcgi_err_conn_closed);
        }
    }
}

void burrow__fcgi_child_serve(burrow__FcgiChild *c) {
    Alloc *h = heap_allocator();
    burrow__FcgiRecord *rec =
        (burrow__FcgiRecord *)mem_alloc(h, sizeof *rec, _Alignof(burrow__FcgiRecord));
    if (rec != NULL) {
        for (;;) {
            if (BURROW_FAILED(burrow__fcgi_record_read(rec, c->conn->r)))
                break;
            if (BURROW_FAILED(burrow__fcgi_child_handle_record(c, rec)))
                break;
        }
        mem_free(h, rec, sizeof *rec, _Alignof(burrow__FcgiRecord));
    }
    fch_clean_up(c);
    (void)burrow__fcgi_conn_close(c->conn);
}

/* emptyBody, io.NopCloser(strings.NewReader("")). */
static Int fch_empty_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    BURROW_OUT(err, io_eof);
    return 0;
}

static Error fch_empty_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static const IoReadCloserVT fch_empty_body_vt = {{NULL, fch_empty_read},
                                                 {NULL, fch_empty_close}};

/* addFastCGIEnvToContext reports whether to include the FastCGI environment
 * variable s in the request's context, accessible via fcgi_process_env. */
static bool fch_add_fast_cgi_env_to_context(Str s) {
    /* Exclude things supported by net/http natively: */
    static const char *const native[] = {
        "CONTENT_LENGTH", "CONTENT_TYPE", "HTTPS",       "PATH_INFO",
        "QUERY_STRING",   "REMOTE_ADDR",  "REMOTE_HOST", "REMOTE_PORT",
        "REQUEST_METHOD", "REQUEST_URI",  "SCRIPT_NAME", "SERVER_PROTOCOL",
    };
    for (size_t i = 0; i < sizeof native / sizeof native[0]; i++) {
        if (str_eq(s, str_from_cstr(native[i])))
            return false;
    }
    if (strings_has_prefix(s, BURROW_S("HTTP_")))
        return false;
    /* Explicitly include FastCGI-specific things. Go lists REMOTE_USER here,
     * as documentation of the sorts of things to expect. Unknown, so include
     * it to be safe. */
    return true;
}

/* filterOutUsedEnvVars returns a new map of env vars without the variables in
 * the given envVars map that are read for creating each HttpRequest */
static Map *fch_filter_out_used_env_vars(Alloc *a, Map *env_vars) {
    Map *without_used_env_vars = map_make(a, TYPE_STRING, TYPE_STRING, 0);
    if (without_used_env_vars == NULL)
        return NULL;
    const void *kp;
    void *vp;
    for (MapIter it = map_iter(env_vars); map_next(&it, &kp, &vp);) {
        if (fch_add_fast_cgi_env_to_context(*(const Str *)kp)) {
            if (!map_set(without_used_env_vars, kp, vp))
                return NULL;
        }
    }
    return without_used_env_vars;
}

typedef struct FchServeArgs {
    burrow__FcgiChild *c;
    FchRequest *req;
    IoReadCloser body;
} FchServeArgs;

static void fch_serve_request(void *env) {
    FchServeArgs *args = (FchServeArgs *)env;
    burrow__FcgiChild *c = args->c;
    FchRequest *req = args->req;
    IoReadCloser body = args->body;
    mem_free(heap_allocator(), args, sizeof *args, _Alignof(FchServeArgs));

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    FchResponse *r = (FchResponse *)mem_alloc(a, sizeof *r, _Alignof(FchResponse));
    if (r != NULL) {
        memset(r, 0, sizeof *r);
        r->req = req;
        r->a = a;
        r->header = http_header_make(a);
        r->w =
            burrow__fcgi_new_writer(a, c->conn, BURROW__FCGI_TYPE_STDOUT, req->req_id);
        if (r->header == NULL || r->w == NULL)
            r = NULL;
    }
    if (r != NULL) {
        HttpResponseWriter rw = {&fch_response_vt, r};
        Error err = BURROW_NO_ERROR;
        HttpRequest *http_req =
            cgi_request_from_map(heap_allocator(), req->params, &err);
        if (http_req == NULL) {
            /* there was an error reading the request */
            fch_write_header(r, HTTP_STATUS_INTERNAL_SERVER_ERROR);
            Str text = error_text(err);
            (void)burrow__fcgi_conn_write_record(
                c->conn, BURROW__FCGI_TYPE_STDERR, req->req_id,
                (Slice){(void *)(uintptr_t)text.p, text.len, text.len, TYPE_BYTE});
        } else {
            http_req->body = body;
            r->env = fch_filter_out_used_env_vars(a, req->params);
            Context parent = http_req->ctx;
            Context env_var_ctx = {0};
            if (r->env != NULL)
                env_var_ctx =
                    context_with_value(a, http_request_context(http_req), fch_env_key,
                                       BURROW_ANY(TYPE_UNSAFE_POINTER, &r->env));
            if (env_var_ctx.vt != NULL)
                http_req->ctx = env_var_ctx;
            HttpHandler handler = c->handler;
            if (handler.vt == NULL)
                handler = http_serve_mux_as_handler(http_default_serve_mux);
            http_handler_serve_http(handler, rw, http_req);
            http_req->ctx = parent;
            context_release(env_var_ctx);
            http_req->body = (IoReadCloser){0};
            http_request_free(http_req);
        }
        /* Make sure we serve something even if nothing was written to r */
        (void)fch_write(r, (Slice){0}, &err);
        (void)fch_response_close(r);
    }
    (void)burrow__fcgi_conn_write_end_request(c->conn, req->req_id, 0,
                                              BURROW__FCGI_STATUS_REQUEST_COMPLETE);

    /* Consume the entire body, so the host isn't still writing to us when we
     * close the socket below in the !keep_conn case, otherwise we'd send a RST.
     * (golang.org/issue/4183) For now just bound it a little. */
    (void)io_copy_n(a, io_discard, io_read_closer_as_io_reader(body),
                    (int64_t)100 << 20, NULL);
    (void)body.vt->closer.close(body.data);

    if (!req->keep_conn)
        (void)burrow__fcgi_conn_close(c->conn);
    burrow__fcgi_writer_free(a, r != NULL ? r->w : NULL);
    arena_free(&ar);
    fch_request_release(req);
    burrow__fcgi_child_release(c);
}

/* go c.serveRequest(req, body). The request and the child are held for the
 * goroutine. When there is no goroutine to be had, the request is ended as
 * overloaded, which is what that status is for. */
static void fch_go_serve_request(burrow__FcgiChild *c, FchRequest *req,
                                 IoReadCloser body) {
    FchServeArgs *args = (FchServeArgs *)mem_alloc(heap_allocator(), sizeof *args,
                                                   _Alignof(FchServeArgs));
    if (args != NULL) {
        args->c = c;
        args->req = req;
        args->body = body;
        (void)sync_atomic_int64_add(&c->refs, 1);
        (void)sync_atomic_int64_add(&req->refs, 1);
        if (go(BURROW_FN(Func, fch_serve_request, args)))
            return;
        (void)sync_atomic_int64_add(&c->refs, -1);
        (void)sync_atomic_int64_add(&req->refs, -1);
        mem_free(heap_allocator(), args, sizeof *args, _Alignof(FchServeArgs));
    }
    if (req->pw != NULL)
        (void)io_pipe_writer_close_with_error(req->pw, burrow_err_out_of_memory);
    (void)burrow__fcgi_conn_write_end_request(c->conn, req->req_id, 0,
                                              BURROW__FCGI_STATUS_OVERLOADED);
}

Error burrow__fcgi_child_handle_record(burrow__FcgiChild *c, burrow__FcgiRecord *rec) {
    FchRequest *req = fch_lookup(c, rec->h.id);
    uint8_t type = rec->h.type;
    if (req == NULL && type != BURROW__FCGI_TYPE_BEGIN_REQUEST &&
        type != BURROW__FCGI_TYPE_GET_VALUES) {
        /* The spec says to ignore unknown request IDs. */
        return BURROW_NO_ERROR;
    }

    Slice content = burrow__fcgi_record_content(rec);
    switch (type) {
    case BURROW__FCGI_TYPE_BEGIN_REQUEST: {
        if (req != NULL) {
            /* The server is trying to begin a request with the same ID as an
             * in-progress request. This is an error. */
            return fch_err_in_flight;
        }
        if (content.len != 8)
            return fch_err_begin_request;
        const Byte *b = (const Byte *)content.p;
        uint16_t role = (uint16_t)((uint16_t)b[0] << 8 | b[1]);
        uint8_t flags = b[2];
        if (role != BURROW__FCGI_ROLE_RESPONDER) {
            (void)burrow__fcgi_conn_write_end_request(c->conn, rec->h.id, 0,
                                                      BURROW__FCGI_STATUS_UNKNOWN_ROLE);
            return BURROW_NO_ERROR;
        }
        req = fch_new_request(rec->h.id, flags);
        if (req == NULL)
            return burrow_err_out_of_memory;
        Int k = rec->h.id;
        uintptr_t v = (uintptr_t)req;
        if (!map_set(c->requests, &k, &v)) {
            fch_request_release(req);
            return burrow_err_out_of_memory;
        }
        return BURROW_NO_ERROR;
    }
    case BURROW__FCGI_TYPE_PARAMS:
        /* NOTE(eds): Technically a key-value pair can straddle the boundary
         * between two packets. We buffer until we've received all
         * parameters. */
        if (content.len > 0) {
            if (!fch_append_params(req, content))
                return burrow_err_out_of_memory;
            return BURROW_NO_ERROR;
        }
        fch_parse_params(req);
        return BURROW_NO_ERROR;
    case BURROW__FCGI_TYPE_STDIN:
        if (req->pw == NULL) {
            IoReadCloser body;
            if (content.len > 0) {
                /* body could be an io.LimitReader, but it shouldn't matter
                 * as long as both sides are behaving. */
                io_pipe(heap_allocator(), &req->pr, &req->pw);
                if (req->pr == NULL)
                    return burrow_err_out_of_memory;
                body = io_pipe_reader_as_io_read_closer(req->pr);
            } else {
                body = (IoReadCloser){&fch_empty_body_vt, NULL};
            }
            fch_go_serve_request(c, req, body);
        }
        if (content.len > 0) {
            /* TODO(eds): This blocks until the handler reads from the pipe.
             * If the handler takes a long time, it might be a problem. */
            (void)io_pipe_writer_write(req->pw, content, NULL);
        } else {
            if (req->pw != NULL)
                (void)io_pipe_writer_close(req->pw);
            fch_delete(c, req);
        }
        return BURROW_NO_ERROR;
    case BURROW__FCGI_TYPE_GET_VALUES: {
        Map *values = map_make(heap_allocator(), TYPE_STRING, TYPE_STRING, 1);
        if (values == NULL)
            return burrow_err_out_of_memory;
        Str k = BURROW_S("FCGI_MPXS_CONNS"), v = BURROW_S("1");
        if (map_set(values, &k, &v))
            (void)burrow__fcgi_conn_write_pairs(
                c->conn, BURROW__FCGI_TYPE_GET_VALUES_RESULT, 0, values);
        map_free(values);
        return BURROW_NO_ERROR;
    }
    case BURROW__FCGI_TYPE_DATA:
        /* If the filter role is implemented, read the data stream here. */
        return BURROW_NO_ERROR;
    case BURROW__FCGI_TYPE_ABORT_REQUEST: {
        (void)burrow__fcgi_conn_write_end_request(c->conn, rec->h.id, 0,
                                                  BURROW__FCGI_STATUS_REQUEST_COMPLETE);
        if (req->pw != NULL)
            (void)io_pipe_writer_close_with_error(req->pw, fcgi_err_request_aborted);
        bool keep_conn = req->keep_conn;
        fch_delete(c, req);
        if (!keep_conn) {
            /* connection will close upon return */
            return fch_err_close_conn;
        }
        return BURROW_NO_ERROR;
    }
    default: {
        Byte b[8] = {0};
        b[0] = type;
        (void)burrow__fcgi_conn_write_record(c->conn, BURROW__FCGI_TYPE_UNKNOWN_TYPE, 0,
                                             (Slice){b, 8, 8, TYPE_BYTE});
        return BURROW_NO_ERROR;
    }
    }
}

/* ------------------------------------------------------------- Serve */

static void fch_serve_child(void *env) {
    burrow__FcgiChild *c = (burrow__FcgiChild *)env;
    burrow__fcgi_child_serve(c);
    burrow__fcgi_child_release(c);
}

Error fcgi_serve(NetListener l, HttpHandler handler) {
    Alloc *h = heap_allocator();
    bool own = false;
    if (l.vt == NULL) {
        Error err = BURROW_NO_ERROR;
        l = net_file_listener(h, os_stdin, &err);
        if (BURROW_FAILED(err))
            return err;
        own = true;
    }
    if (handler.vt == NULL)
        handler = http_serve_mux_as_handler(http_default_serve_mux);
    for (;;) {
        Error err = BURROW_NO_ERROR;
        NetConn rw = l.vt->accept(l.data, &err);
        if (BURROW_FAILED(err)) {
            if (own) {
                (void)l.vt->closer.close(l.data);
                net_listener_free(l);
            }
            return err;
        }
        burrow__FcgiChild *c =
            burrow__fcgi_new_child(net_conn_as_io_reader(rw), net_conn_as_io_writer(rw),
                                   net_conn_as_io_closer(rw), handler);
        if (c == NULL) {
            (void)rw.vt->closer.close(rw.data);
            net_conn_free(rw);
            continue;
        }
        burrow__fcgi_child_set_net_conn(c, rw);
        if (!go(BURROW_FN(Func, fch_serve_child, c))) {
            /* No goroutine for it, so the web server is told no by the
             * connection closing. */
            (void)burrow__fcgi_conn_close(c->conn);
            burrow__fcgi_child_release(c);
        }
    }
}

/* ProcessEnv returns FastCGI environment variables associated with the request
 * r for which no effort was made to be included in the request itself - the
 * data is hidden in the request's context. As an example, if REMOTE_USER is
 * set for a request, it will not be found anywhere in r, but it will be
 * included in ProcessEnv's response (via r's context). */
Map *fcgi_process_env(const HttpRequest *r) {
    Any v = context_value(http_request_context(r), fch_env_key);
    if (v.t != TYPE_UNSAFE_POINTER || v.data == NULL)
        return NULL;
    return *(Map **)v.data;
}
