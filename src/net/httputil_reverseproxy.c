/* Derived from Go's src/net/http/httputil/reverseproxy.go, an HTTP handler
 * that sends the requests it gets on to another server.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http/httputil.h"

#include "http_internal.h"

#include "../xnet/httpguts.h"
#include "http_ascii.h"

#include "burrow/bufio.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mime.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptrace.h"
#include "burrow/net/textproto.h"
#include "burrow/net/url.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* The size of the buffer a body is copied through when there is no pool. */
#define RP_COPY_BUFFER 32768

/* defaultMaxParams, which is the same as net/url's. */
#define RP_DEFAULT_MAX_PARAMS 10000

BURROW_SENTINEL_ERROR(rp_err_director_rewrite,
                      "ReverseProxy must have exactly one of Director or Rewrite set");
BURROW_SENTINEL_ERROR(rp_err_not_writable, "internal error: 101 switching protocols "
                                           "response with non-writable body");
BURROW_SENTINEL_ERROR(rp_err_copy_done, "hijacked connection copy complete");

static bool rp_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* ------------------------------------------------------------ URL rewriting */

/* singleJoiningSlash. */
Str burrow__httputil_single_joining_slash(Alloc *a, Str x, Str y) {
    bool xslash = strings_has_suffix(x, BURROW_S("/"));
    bool yslash = strings_has_prefix(y, BURROW_S("/"));
    if (xslash && yslash)
        return fmt_sprintf_v(a, "%s%s", x, str_from_bytes(y.p + 1, y.len - 1));
    if (!xslash && !yslash)
        return fmt_sprintf_v(a, "%s/%s", x, y);
    return fmt_sprintf_v(a, "%s%s", x, y);
}

/* The string after its first byte, or "" when it has none. */
static Str rp_tail(Str s) {
    return s.len > 0 ? str_from_bytes(s.p + 1, s.len - 1) : s;
}

/* joinURLPath. */
void burrow__httputil_join_url_path(Alloc *a, const Url *x, const Url *y, Str *path,
                                    Str *raw_path) {
    if (x->raw_path.len == 0 && y->raw_path.len == 0) {
        *path = burrow__httputil_single_joining_slash(a, x->path, y->path);
        *raw_path = BURROW_STR_EMPTY;
        return;
    }
    /* singleJoiningSlash, with EscapedPath saying whether a slash goes in. */
    Str xpath = url_escaped_path(x, a);
    Str ypath = url_escaped_path(y, a);
    bool xslash = strings_has_suffix(xpath, BURROW_S("/"));
    bool yslash = strings_has_prefix(ypath, BURROW_S("/"));
    if (xslash && yslash) {
        *path = fmt_sprintf_v(a, "%s%s", x->path, rp_tail(y->path));
        *raw_path = fmt_sprintf_v(a, "%s%s", xpath, rp_tail(ypath));
    } else if (!xslash && !yslash) {
        *path = fmt_sprintf_v(a, "%s/%s", x->path, y->path);
        *raw_path = fmt_sprintf_v(a, "%s/%s", xpath, ypath);
    } else {
        *path = fmt_sprintf_v(a, "%s%s", x->path, y->path);
        *raw_path = fmt_sprintf_v(a, "%s%s", xpath, ypath);
    }
}

/* rewriteRequestURL, with the strings made in a. */
static void rp_rewrite_request_url(Alloc *a, HttpRequest *req, const Url *target) {
    Url *u = req->url;
    Str target_query = target->raw_query;
    u->scheme = str_clone(a, target->scheme);
    u->host = str_clone(a, target->host);
    Str path;
    Str raw_path;
    burrow__httputil_join_url_path(a, target, u, &path, &raw_path);
    u->path = path;
    u->raw_path = raw_path;
    if (target_query.len == 0 || u->raw_query.len == 0)
        u->raw_query = fmt_sprintf_v(a, "%s%s", target_query, u->raw_query);
    else
        u->raw_query = fmt_sprintf_v(a, "%s&%s", target_query, u->raw_query);
}

void httputil_proxy_request_set_url(HttputilProxyRequest *r, const Url *target) {
    rp_rewrite_request_url(arena_allocator(&r->out->arena), r->out, target);
    r->out->host = BURROW_STR_EMPTY;
}

/* The values joined with ", ". */
static Str rp_join(Alloc *a, Slice values) {
    return strings_join(a, values, BURROW_S(", "));
}

void httputil_proxy_request_set_x_forwarded(HttputilProxyRequest *r) {
    Alloc *a = arena_allocator(&r->out->arena);
    HttpHeader h = r->out->header;
    Error err = BURROW_NO_ERROR;
    Str client_ip = net_split_host_port(r->in->remote_addr, NULL, &err);
    if (BURROW_OK(err)) {
        Slice prior = http_header_values(h, BURROW_S("X-Forwarded-For"));
        if (prior.len > 0)
            client_ip = fmt_sprintf_v(a, "%s, %s", rp_join(a, prior), client_ip);
        else
            client_ip = str_clone(a, client_ip);
        (void)http_header_set(h, BURROW_S("X-Forwarded-For"), client_ip);
    } else {
        http_header_del(h, BURROW_S("X-Forwarded-For"));
    }
    (void)http_header_set(h, BURROW_S("X-Forwarded-Host"), r->in->host);
    /* There is no TLS here yet, so a request never came in as "https". */
    (void)http_header_set(h, BURROW_S("X-Forwarded-Proto"), BURROW_S("http"));
}

/* -------------------------------------------------------------- the proxy */

/* The director NewSingleHostReverseProxy makes. */
static void rp_single_host_director(void *env, HttpRequest *req) {
    const Url *target = (const Url *)env;
    rp_rewrite_request_url(arena_allocator(&req->arena), req, target);
}

HttputilReverseProxy *httputil_new_single_host_reverse_proxy(Alloc *a,
                                                             const Url *target) {
    HttputilReverseProxy *p = (HttputilReverseProxy *)mem_alloc(
        a, sizeof *p, _Alignof(HttputilReverseProxy));
    if (p == NULL)
        return NULL;
    p->a = a;
    p->target = url_clone(target, a);
    if (p->target == NULL) {
        mem_free(a, p, sizeof *p, _Alignof(HttputilReverseProxy));
        return NULL;
    }
    p->director = BURROW_FN(HttputilDirectorFunc, rp_single_host_director, p->target);
    return p;
}

void httputil_reverse_proxy_free(HttputilReverseProxy *p) {
    if (p == NULL || p->a == NULL)
        return;
    Alloc *a = p->a;
    url_free(a, p->target);
    mem_free(a, p, sizeof *p, _Alignof(HttputilReverseProxy));
}

/* copyHeader. The keys and values are copied into dst's allocator, since src
 * is usually gone before dst is. False when that says no. */
static bool rp_copy_header(HttpHeader dst, HttpHeader src) {
    if (src == NULL)
        return true;
    Alloc *a = burrow__map_allocator(dst);
    MapIter it = map_iter(src);
    const void *kp;
    void *vp;
    while (map_next(&it, &kp, &vp)) {
        Str k = str_clone(a, *(const Str *)kp);
        if (k.len == 0 && ((const Str *)kp)->len != 0)
            return false;
        const Slice *vv = (const Slice *)vp;
        for (Int i = 0; i < vv->len; i++) {
            Str v = ((const Str *)vv->p)[i];
            Str c = str_clone(a, v);
            if (c.len != v.len || !http_header_add(dst, k, c))
                return false;
        }
    }
    return true;
}

/* Hop-by-hop headers, which are taken off what goes to the backend. RFC 7230
 * says these go in the Connection field, and these are the ones the older
 * RFC 2616, section 13.5.1, named, kept for compatibility. */
static const Str rp_hop_headers[] = {
    BURROW_S_INIT("Connection"),
    BURROW_S_INIT("Proxy-Connection"), /* not standard, but libcurl sends it */
    BURROW_S_INIT("Keep-Alive"),
    BURROW_S_INIT("Proxy-Authenticate"),
    BURROW_S_INIT("Proxy-Authorization"),
    BURROW_S_INIT("Te"),      /* the canonical form of "TE" */
    BURROW_S_INIT("Trailer"), /* not Trailers, see RFC errata 4522 */
    BURROW_S_INIT("Transfer-Encoding"),
    BURROW_S_INIT("Upgrade"),
};

/* removeHopByHopHeaders. */
static void rp_remove_hop_by_hop_headers(HttpHeader h) {
    if (h == NULL)
        return;
    /* RFC 7230, section 6.1: the fields Connection names. The Slice is copied
     * first, since one of them may be Connection itself. */
    Slice conn = http_header_values(h, BURROW_S("Connection"));
    for (Int i = 0; i < conn.len; i++) {
        Str f = ((const Str *)conn.p)[i];
        while (f.len >= 0) {
            Str rest = BURROW_STR_EMPTY;
            bool found = false;
            Str sf = strings_cut(f, BURROW_S(","), &rest, &found);
            sf = textproto_trim_string(sf);
            if (sf.len > 0)
                http_header_del(h, sf);
            if (!found)
                break;
            f = rest;
        }
    }
    /* RFC 2616, section 13.5.1, which RFC 7230 replaces, but which is kept for
     * compatibility. */
    for (size_t i = 0; i < sizeof rp_hop_headers / sizeof rp_hop_headers[0]; i++)
        http_header_del(h, rp_hop_headers[i]);
}

/* upgradeType. */
static Str rp_upgrade_type(HttpHeader h) {
    if (h == NULL)
        return BURROW_STR_EMPTY;
    Slice conn = http_header_values(h, BURROW_S("Connection"));
    if (!burrow__httpguts_header_values_contains_token((const Str *)conn.p, conn.len,
                                                       BURROW_S("Upgrade")))
        return BURROW_STR_EMPTY;
    return http_header_get(h, BURROW_S("Upgrade"));
}

static bool rp_is_hex(Byte c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* cleanQueryParams. s as it is when every parameter in it parses, and the ones
 * that do parse encoded again, in a, when some do not. */
static Str rp_clean_query_params(Alloc *a, Str s) {
    bool reencode = burrow__url_max_query_params_named() ||
                    strings_count(s, BURROW_S("&")) + 1 > RP_DEFAULT_MAX_PARAMS;
    for (Int i = 0; !reencode && i < s.len;) {
        if (s.p[i] == ';') {
            reencode = true;
        } else if (s.p[i] == '%') {
            if (i + 2 >= s.len || !rp_is_hex(s.p[i + 1]) || !rp_is_hex(s.p[i + 2]))
                reencode = true;
            i += 3;
        } else {
            i++;
        }
    }
    if (!reencode)
        return s;
    Error err = BURROW_NO_ERROR;
    UrlValues v = url_parse_query(a, s, &err);
    return url_values_encode(v, a);
}

/* logf. */
#define RP_LOGF(p, ...) log_logger_printf_v((p)->error_log, __VA_ARGS__)

/* defaultErrorHandler and getErrorHandler. */
static void rp_error(HttputilReverseProxy *p, HttpResponseWriter rw, HttpRequest *req,
                     Error err) {
    if (!BURROW_FUNC_IS_NIL(p->error_handler)) {
        BURROW_CALLF(p->error_handler, rw, req, err);
        return;
    }
    RP_LOGF(p, "http: proxy error: %v", err);
    http_response_writer_write_header(rw, HTTP_STATUS_BAD_GATEWAY);
}

/* Closes a body that is not nil. */
static void rp_close(IoReadCloser body) {
    if (body.vt != NULL)
        (void)body.vt->closer.close(body.data);
}

/* modifyResponse. Whether to go on. */
static bool rp_modify_response(HttputilReverseProxy *p, HttpResponseWriter rw,
                               HttpResponse *res, HttpRequest *req) {
    if (BURROW_FUNC_IS_NIL(p->modify_response))
        return true;
    Error err = BURROW_CALLF(p->modify_response, res);
    if (BURROW_FAILED(err)) {
        rp_close(res->body);
        rp_error(p, rw, req, err);
        return false;
    }
    return true;
}

/* flushInterval. */
Duration burrow__httputil_flush_interval(const HttputilReverseProxy *p,
                                         const HttpResponse *res) {
    Str ct = res->header != NULL ? http_header_get(res->header, BURROW_S("Content-Type"))
                                 : BURROW_STR_EMPTY;
    /* Server-Sent Events are flushed at once, and a negative interval says
     * so. The type is in https://www.w3.org/TR/eventsource/#text-event-stream */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Str base = mime_parse_media_type(arena_allocator(&ar), ct, NULL, &err);
    bool sse = str_eq(base, BURROW_S("text/event-stream"));
    arena_free(&ar);
    if (sse)
        return -1;
    /* A stream may have no Content-Length. */
    if (res->content_length == -1)
        return -1;
    return p->flush_interval;
}

/* ------------------------------------------------------ maxLatencyWriter */

/* maxLatencyWriter. Go has a time.AfterFunc call delayedFlush. Here one
 * goroutine waits on the timer for the life of the writer, so that stop can
 * wait for it to be done before the writer goes away. */
typedef struct rp_Mlw {
    HttpResponseWriter dst;
    Duration latency; /* not zero; negative means to flush after each write */

    SyncMutex mu; /* for t, flush_pending, and flushing dst */
    TimeTimer *t;
    bool flush_pending;

    Chan *stop; /* closed by stop */
    SyncWaitGroup wg;
} rp_Mlw;

static void rp_mlw_flush(rp_Mlw *m) {
    HttpResponseController rc = http_new_response_controller(m->dst);
    (void)http_response_controller_flush(&rc);
}

static Int rp_mlw_write(void *self, Slice b, Error *err) {
    rp_Mlw *m = (rp_Mlw *)self;
    sync_mutex_lock(&m->mu);
    Int n = http_response_writer_write(m->dst, b, err);
    if (m->latency < 0) {
        rp_mlw_flush(m);
    } else if (!m->flush_pending) {
        (void)time_timer_reset(m->t, m->latency, NULL);
        m->flush_pending = true;
    }
    sync_mutex_unlock(&m->mu);
    return n;
}

static const IoWriterVT rp_mlw_vt = {NULL, rp_mlw_write};

/* delayedFlush, each time the timer fires, until stop. */
static void rp_mlw_loop(void *env) {
    rp_Mlw *m = (rp_Mlw *)env;
    for (;;) {
        SelectCase cases[2] = {BURROW_RECV(time_timer_c(m->t), NULL),
                               BURROW_RECV(m->stop, NULL)};
        if (chan_select(cases, 2) != 0)
            return;
        sync_mutex_lock(&m->mu);
        /* stop may have come first. */
        if (m->flush_pending) {
            rp_mlw_flush(m);
            m->flush_pending = false;
        }
        sync_mutex_unlock(&m->mu);
    }
}

static void rp_mlw_free(rp_Mlw *m) {
    Alloc *a = heap_allocator();
    time_timer_free(m->t);
    chan_free(m->stop);
    mem_free(a, m, sizeof *m, _Alignof(rp_Mlw));
}

/* The writer, with the first timer set so that the header goes out even when
 * the body is slow to start. NULL when there is no memory for it. */
static rp_Mlw *rp_mlw_start(HttpResponseWriter dst, Duration latency) {
    Alloc *a = heap_allocator();
    rp_Mlw *m = (rp_Mlw *)mem_alloc(a, sizeof *m, _Alignof(rp_Mlw));
    if (m == NULL)
        return NULL;
    m->dst = dst;
    m->latency = latency;
    m->flush_pending = true;
    m->t = time_new_timer(a, latency);
    m->stop = chan_make(a, TYPE_BOOL, 0);
    if (m->t == NULL || m->stop == NULL ||
        !sync_wait_group_go(&m->wg, BURROW_FN(Func, rp_mlw_loop, m))) {
        rp_mlw_free(m);
        return NULL;
    }
    return m;
}

/* stop, and then the writer's memory back. */
static void rp_mlw_stop(rp_Mlw *m) {
    sync_mutex_lock(&m->mu);
    m->flush_pending = false;
    (void)time_timer_stop(m->t);
    sync_mutex_unlock(&m->mu);
    chan_close(m->stop);
    sync_wait_group_wait(&m->wg);
    rp_mlw_free(m);
}

/* ------------------------------------------------------------ the copies */

/* copyBuffer. The write errors and the read errors that are not io_eof. */
static Error rp_copy_buffer(HttputilReverseProxy *p, IoWriter dst, IoReader src,
                            Slice buf) {
    Alloc *a = heap_allocator();
    Byte *own = NULL;
    if (buf.len == 0) {
        own = (Byte *)mem_alloc(a, RP_COPY_BUFFER, 1);
        if (own == NULL)
            return burrow_err_out_of_memory;
        buf = slice_from(own, RP_COPY_BUFFER, RP_COPY_BUFFER, TYPE_BYTE);
    }
    Error ret = BURROW_NO_ERROR;
    for (;;) {
        Error rerr = BURROW_NO_ERROR;
        Int nr = src.vt->read(src.data, buf, &rerr);
        if (BURROW_FAILED(rerr) && !rp_same(rerr, io_eof) &&
            !rp_same(rerr, context_canceled))
            RP_LOGF(p, "httputil: ReverseProxy read error during body copy: %v", rerr);
        if (nr > 0) {
            Error werr = BURROW_NO_ERROR;
            Int nw = dst.vt->write(dst.data, slice_sub(buf, 0, nr), &werr);
            if (BURROW_FAILED(werr)) {
                ret = werr;
                break;
            }
            if (nr != nw) {
                ret = io_err_short_write;
                break;
            }
        }
        if (BURROW_FAILED(rerr)) {
            if (!rp_same(rerr, io_eof))
                ret = rerr;
            break;
        }
    }
    if (own != NULL)
        mem_free(a, own, RP_COPY_BUFFER, 1);
    return ret;
}

/* copyResponse. */
static Error rp_copy_response(HttputilReverseProxy *p, HttpResponseWriter dst,
                              IoReader src, Duration flush_interval) {
    IoWriter w = http_response_writer_as_io_writer(dst);
    rp_Mlw *m = NULL;
    if (flush_interval != 0) {
        m = rp_mlw_start(dst, flush_interval);
        if (m == NULL)
            return burrow_err_out_of_memory;
        w = (IoWriter){&rp_mlw_vt, m};
    }
    HttputilBufferPool pool = p->buffer_pool;
    Slice buf = slice_nil(TYPE_BYTE);
    if (pool.vt != NULL)
        buf = pool.vt->get(pool.data);
    Error err = rp_copy_buffer(p, w, src, buf);
    if (pool.vt != NULL)
        pool.vt->put(pool.data, buf);
    if (m != NULL)
        rp_mlw_stop(m);
    return err;
}

/* shouldPanicOnCopyError. Go 1.10 and before did not panic, so only a request
 * that came through an HttpServer, which recovers, gets the panic. */
static bool rp_should_panic_on_copy_error(const HttpRequest *req) {
    Any v = context_value(http_request_context(req), http_server_context_key);
    return v.t != NULL;
}

/* --------------------------------------------------------------- upgrades */

/* One way of a switchProtocolCopier: src to dst, and then dst's CloseWrite
 * when it has one. */
typedef struct rp_Copier {
    IoWriter dst;
    IoReader src;
    Error (*close_write)(void *self);
    void *close_write_self;
    Chan *errc;
} rp_Copier;

static void rp_copier_run(void *env) {
    rp_Copier *c = (rp_Copier *)env;
    Error err = BURROW_NO_ERROR;
    (void)io_copy(heap_allocator(), c->dst, c->src, &err);
    if (BURROW_OK(err))
        err = c->close_write != NULL ? c->close_write(c->close_write_self)
                                     : rp_err_copy_done;
    chan_send(c->errc, &err);
}

/* CloseWrite on a connection from a server's Hijack. */
static Error rp_conn_close_write(void *self) {
    NetConn c = *(const NetConn *)self;
    NetTCPConn *tc = net_conn_as_tcp_conn(c);
    if (tc != NULL)
        return net_tcp_conn_close_write(tc);
    NetUnixConn *uc = net_conn_as_unix_conn(c);
    if (uc != NULL)
        return net_unix_conn_close_write(uc);
    return errors_err_unsupported;
}

/* The goroutine that closes the backend connection when the request is done,
 * issue 35559, or when the copies are. */
typedef struct rp_BackClose {
    Chan *done; /* the request context's */
    Chan *closed;
    IoReadCloser back;
} rp_BackClose;

static void rp_back_close(void *env) {
    rp_BackClose *b = (rp_BackClose *)env;
    SelectCase cases[2] = {BURROW_RECV(b->done, NULL), BURROW_RECV(b->closed, NULL)};
    (void)chan_select(cases, 2);
    rp_close(b->back);
}

/* handleUpgradeResponse. */
static void rp_handle_upgrade_response(HttputilReverseProxy *p, HttpResponseWriter rw,
                                       HttpRequest *req, HttpResponse *res) {
    Str req_up = rp_upgrade_type(req->header);
    Str res_up = rp_upgrade_type(res->header);
    /* req_up is ASCII, which the caller made sure of. */
    if (!burrow__http_ascii_is_print(res_up)) {
        rp_error(p, rw, req,
                 fmt_errorf_v("backend tried to switch to invalid protocol %q", res_up));
        return;
    }
    if (!burrow__http_ascii_equal_fold(req_up, res_up)) {
        rp_error(p, rw, req,
                 fmt_errorf_v("backend tried to switch protocol %q when %q was requested",
                              res_up, req_up));
        return;
    }

    IoWriter back_w;
    if (!http_response_body_writer(res, &back_w)) {
        rp_error(p, rw, req, rp_err_not_writable);
        return;
    }

    HttpResponseController rc = http_new_response_controller(rw);
    BufioReadWriter brw = {NULL, NULL};
    Error hijack_err = BURROW_NO_ERROR;
    NetConn conn = http_response_controller_hijack(&rc, &brw, &hijack_err);
    if (errors_is(hijack_err, http_err_not_supported)) {
        Str name = BURROW_S("<nil>");
        const Type *t = rw.vt->writer.self_type;
        if (t != NULL)
            name = fmt_sprintf_v(error_allocator(), "*%s", type_name(t));
        rp_error(p, rw, req,
                 fmt_errorf_v("can't switch protocols using non-Hijacker "
                              "ResponseWriter type %s",
                              name));
        return;
    }

    Alloc *a = heap_allocator();
    rp_BackClose bc;
    memset(&bc, 0, sizeof bc);
    bc.done = context_done(http_request_context(req));
    bc.closed = chan_make(a, TYPE_BOOL, 0);
    bc.back = res->body;
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    if (bc.closed == NULL || !sync_wait_group_go(&wg, BURROW_FN(Func, rp_back_close, &bc))) {
        chan_free(bc.closed);
        rp_close(res->body);
        net_conn_free(conn);
        if (brw.reader != NULL)
            bufio_reader_free(brw.reader);
        if (brw.writer != NULL)
            bufio_writer_free(brw.writer);
        rp_error(p, rw, req, burrow_err_out_of_memory);
        return;
    }

    Chan *errc = NULL;
    Int sent = 0;
    if (BURROW_FAILED(hijack_err)) {
        rp_error(p, rw, req,
                 fmt_errorf_v("Hijack failed on protocol switch: %v", hijack_err));
        goto done;
    }

    HttpResponseWriter w = rw;
    if (!rp_copy_header(http_response_writer_header(w), res->header)) {
        rp_error(p, rw, req, burrow_err_out_of_memory);
        goto done;
    }
    /* res is the proxy's to free, so its own header and body go back in
     * before it is. */
    HttpHeader res_header = res->header;
    IoReadCloser res_body = res->body;
    res->header = http_response_writer_header(w);
    res->body = (IoReadCloser){NULL, NULL}; /* so only the header is written */
    Error err = http_response_write(res, bufio_writer_as_io_writer(brw.writer));
    res->header = res_header;
    res->body = res_body;
    if (BURROW_FAILED(err)) {
        rp_error(p, rw, req, fmt_errorf_v("response write: %v", err));
        goto done;
    }
    err = bufio_writer_flush(brw.writer);
    if (BURROW_FAILED(err)) {
        rp_error(p, rw, req, fmt_errorf_v("response flush: %v", err));
        goto done;
    }

    errc = chan_make(a, TYPE_ERROR, 2);
    if (errc == NULL) {
        rp_error(p, rw, req, burrow_err_out_of_memory);
        goto done;
    }
    /* The client's bytes come through brw.reader, which may hold some
     * already. */
    rp_Copier to_back = {back_w, bufio_reader_as_io_reader(brw.reader),
                         res->body_close_write, back_w.data, errc};
    rp_Copier from_back = {net_conn_as_io_writer(conn), io_read_closer_as_io_reader(res->body),
                           rp_conn_close_write, &conn, errc};
    if (go(BURROW_FN(Func, rp_copier_run, &to_back)))
        sent++;
    if (go(BURROW_FN(Func, rp_copier_run, &from_back)))
        sent++;

    /* Until both copies have sent, or one of them fails. */
    if (sent > 0) {
        Error e = BURROW_NO_ERROR;
        (void)chan_recv(errc, &e);
        sent--;
        if (BURROW_OK(e) && sent > 0) {
            (void)chan_recv(errc, &e);
            sent--;
        }
    }

done:
    /* Go's deferred conn.Close and close(backConnCloseCh), and then what Go
     * leaves to its collector: the copy that is still going stops once both
     * connections are closed. */
    if (conn.vt != NULL)
        (void)conn.vt->closer.close(conn.data);
    chan_close(bc.closed);
    sync_wait_group_wait(&wg);
    while (sent > 0) {
        Error e = BURROW_NO_ERROR;
        (void)chan_recv(errc, &e);
        sent--;
    }
    chan_free(errc);
    chan_free(bc.closed);
    net_conn_free(conn);
    if (brw.reader != NULL)
        bufio_reader_free(brw.reader);
    if (brw.writer != NULL)
        bufio_writer_free(brw.writer);
}

/* ---------------------------------------------------------------- ServeHTTP */

/* The Got1xxResponse hook and what it shares with ServeHTTP. */
typedef struct rp_Got1xx {
    SyncMutex mu;
    bool round_trip_done;
    HttpResponseWriter rw;
} rp_Got1xx;

static Error rp_got1xx(void *env, Int code, TextprotoMIMEHeader header) {
    rp_Got1xx *g = (rp_Got1xx *)env;
    sync_mutex_lock(&g->mu);
    /* Once RoundTrip has returned the writer's header is left alone. */
    if (!g->round_trip_done) {
        HttpHeader h = http_response_writer_header(g->rw);
        (void)rp_copy_header(h, header);
        http_response_writer_write_header(g->rw, code);
        /* WriteHeader does not clear the header after a 1xx. */
        map_clear(h);
    }
    sync_mutex_unlock(&g->mu);
    return BURROW_NO_ERROR;
}

/* What ServeHTTP has to give back when it is done, in one place, since there
 * are many ways out of it. */
typedef struct rp_Serve {
    Context cctx; /* the CloseNotify context, or nil */
    ContextCancelFunc cancel;
    Chan *notify;
    SyncWaitGroup wg;

    HttpRequest *clone;
    IoReadCloser body; /* the clone's body, closed at the end */
    HttpRequest *out;  /* what the rewrite left in out, when not the clone */
    Context tctx;      /* the context with the trace */
    HttptraceClientTrace trace;
    rp_Got1xx got;
    HttpResponse *res;
} rp_Serve;

/* The goroutine that cancels the context when the writer's CloseNotify
 * fires. */
static void rp_close_notify(void *env) {
    rp_Serve *s = (rp_Serve *)env;
    SelectCase cases[2] = {BURROW_RECV(s->notify, NULL),
                           BURROW_RECV(context_done(s->cctx), NULL)};
    if (chan_select(cases, 2) == 0)
        BURROW_CALLF0(s->cancel);
}

static void rp_serve_end(rp_Serve *s) {
    http_response_free(s->res);
    if (s->out != s->clone)
        http_request_free(s->out);
    http_request_free(s->clone);
    context_release(s->tctx);
    rp_close(s->body);
    if (s->cctx.vt != NULL) {
        BURROW_CALLF0(s->cancel);
        sync_wait_group_wait(&s->wg);
        context_release(s->cctx);
    }
}

void httputil_reverse_proxy_serve_http(HttputilReverseProxy *p, HttpResponseWriter rw,
                                       HttpRequest *req) {
    HttpRoundTripper transport = p->transport;
    if (transport.vt == NULL)
        transport = http_transport_as_round_tripper(http_default_transport);

    rp_Serve s;
    memset(&s, 0, sizeof s);
    Context ctx = http_request_context(req);
    /* CloseNotify came before contexts, which do the same thing, so it is only
     * watched when the request's context has no Done channel. */
    if (context_done(ctx) == NULL && rw.vt->close_notify != NULL) {
        s.cctx = context_with_cancel(heap_allocator(), ctx, &s.cancel);
        if (s.cctx.vt != NULL) {
            s.notify = rw.vt->close_notify(rw.data);
            if (sync_wait_group_go(&s.wg, BURROW_FN(Func, rp_close_notify, &s)))
                ctx = s.cctx;
            else {
                BURROW_CALLF0(s.cancel);
                context_release(s.cctx);
                s.cctx = (Context){NULL, NULL};
            }
        }
    }

    HttpRequest *outreq = http_request_clone(req, heap_allocator(), ctx);
    if (outreq == NULL) {
        rp_serve_end(&s);
        rp_error(p, rw, req, burrow_err_out_of_memory);
        return;
    }
    s.clone = outreq;
    s.out = outreq;
    Alloc *oa = arena_allocator(&outreq->arena);
    if (req->content_length == 0)
        outreq->body = (IoReadCloser){NULL, NULL}; /* issue 16036, for retries */
    /* Nothing reads a request body after the handler has returned, and the
     * transport's reads can outlive this, issue 46866. */
    s.body = outreq->body;
    if (outreq->header == NULL) {
        /* Issue 33142: there has always been a header. */
        outreq->header = http_header_make(oa);
        if (outreq->header == NULL) {
            rp_error(p, rw, req, burrow_err_out_of_memory);
            rp_serve_end(&s);
            return;
        }
    }

    if (BURROW_FUNC_IS_NIL(p->director) == BURROW_FUNC_IS_NIL(p->rewrite)) {
        rp_error(p, rw, req, rp_err_director_rewrite);
        rp_serve_end(&s);
        return;
    }

    if (!BURROW_FUNC_IS_NIL(p->director)) {
        BURROW_CALLF(p->director, outreq);
        if (outreq->form != NULL && outreq->url != NULL)
            outreq->url->raw_query = rp_clean_query_params(oa, outreq->url->raw_query);
    }
    outreq->close = false;

    Str req_up = rp_upgrade_type(outreq->header);
    if (!burrow__http_ascii_is_print(req_up)) {
        rp_error(p, rw, req,
                 fmt_errorf_v("client tried to switch to invalid protocol %q", req_up));
        rp_serve_end(&s);
        return;
    }
    rp_remove_hop_by_hop_headers(outreq->header);

    /* Issue 21096: the backend learns that trailers work when the client
     * thought it worth saying. This looks at req, since outreq has had the
     * hop-by-hop fields taken off. */
    if (req->header != NULL) {
        Slice te = http_header_values(req->header, BURROW_S("Te"));
        if (burrow__httpguts_header_values_contains_token((const Str *)te.p, te.len,
                                                          BURROW_S("trailers")))
            (void)http_header_set(outreq->header, BURROW_S("Te"), BURROW_S("trailers"));
    }

    /* The hop-by-hop fields an upgrade such as a websocket needs go back. */
    if (req_up.len > 0) {
        (void)http_header_set(outreq->header, BURROW_S("Connection"), BURROW_S("Upgrade"));
        (void)http_header_set(outreq->header, BURROW_S("Upgrade"), req_up);
    }

    if (!BURROW_FUNC_IS_NIL(p->rewrite)) {
        /* The rewrite may set these again with set_x_forwarded, or copy the
         * client's. */
        http_header_del(outreq->header, BURROW_S("Forwarded"));
        http_header_del(outreq->header, BURROW_S("X-Forwarded-For"));
        http_header_del(outreq->header, BURROW_S("X-Forwarded-Host"));
        http_header_del(outreq->header, BURROW_S("X-Forwarded-Proto"));

        if (outreq->url != NULL)
            outreq->url->raw_query = rp_clean_query_params(oa, outreq->url->raw_query);

        HttputilProxyRequest pr = {req, outreq};
        BURROW_CALLF(p->rewrite, &pr);
        outreq = pr.out;
        s.out = outreq;
    } else {
        Error err = BURROW_NO_ERROR;
        Str client_ip = net_split_host_port(req->remote_addr, NULL, &err);
        if (BURROW_OK(err)) {
            /* A proxy after another keeps what came before, as one field
             * with ", " between. Issue 38079: a nil Slice says to leave the
             * field out. */
            Slice *prior =
                BURROW_MAP_GET(Str, Slice, outreq->header, BURROW_S("X-Forwarded-For"));
            bool omit = prior != NULL && prior->p == NULL;
            if (prior != NULL && prior->len > 0)
                client_ip = fmt_sprintf_v(oa, "%s, %s", rp_join(oa, *prior), client_ip);
            if (!omit)
                (void)http_header_set(outreq->header, BURROW_S("X-Forwarded-For"),
                                      client_ip);
        }
    }

    if (!burrow__http_header_has(outreq->header, BURROW_S("User-Agent"))) {
        /* Not the default Go client's User-Agent, when the request had
         * none. */
        (void)http_header_set(outreq->header, BURROW_S("User-Agent"), BURROW_STR_EMPTY);
    }

    s.got.rw = rw;
    s.trace.got1xx_response = BURROW_FN(HttptraceGot1xxResponseFunc, rp_got1xx, &s.got);
    s.tctx = httptrace_with_client_trace(heap_allocator(), http_request_context(outreq),
                                         &s.trace);
    if (s.tctx.vt == NULL) {
        rp_error(p, rw, outreq, burrow_err_out_of_memory);
        rp_serve_end(&s);
        return;
    }
    outreq->ctx = s.tctx;

    Error err = BURROW_NO_ERROR;
    HttpResponse *res = transport.vt->round_trip(transport.data, outreq, &err);
    sync_mutex_lock(&s.got.mu);
    s.got.round_trip_done = true;
    sync_mutex_unlock(&s.got.mu);
    s.res = res;
    if (BURROW_FAILED(err) || res == NULL) {
        rp_error(p, rw, outreq, err);
        rp_serve_end(&s);
        return;
    }

    /* 101 Switching Protocols, for a websocket, h2c and the like. */
    if (res->status_code == HTTP_STATUS_SWITCHING_PROTOCOLS) {
        if (rp_modify_response(p, rw, res, outreq))
            rp_handle_upgrade_response(p, rw, outreq, res);
        rp_serve_end(&s);
        return;
    }

    rp_remove_hop_by_hop_headers(res->header);

    if (!rp_modify_response(p, rw, res, outreq)) {
        rp_serve_end(&s);
        return;
    }

    HttpHeader h = http_response_writer_header(rw);
    (void)rp_copy_header(h, res->header);

    /* The transport's response has no Trailer field, so it is made again from
     * the trailer. */
    Int announced = res->trailer != NULL ? map_len(res->trailer) : 0;
    if (announced > 0) {
        Alloc *ha = burrow__map_allocator(h);
        Slice keys = slice_make(ha, TYPE_STRING, 0, announced);
        MapIter it = map_iter(res->trailer);
        const void *kp;
        while (map_next(&it, &kp, NULL))
            keys = slice_append(ha, keys, kp, 1);
        Str joined = strings_join(ha, keys, BURROW_S(", "));
        (void)http_header_add(h, BURROW_S("Trailer"), joined);
    }

    http_response_writer_write_header(rw, res->status_code);

    err = rp_copy_response(p, rw, io_read_closer_as_io_reader(res->body),
                           burrow__httputil_flush_interval(p, res));
    if (BURROW_FAILED(err)) {
        rp_close(res->body);
        /* All that can be done partway through a body is to give up on the
         * request, issue 23643. */
        if (!rp_should_panic_on_copy_error(req)) {
            RP_LOGF(p,
                    "suppressing panic for copyResponse error in test; copy error: %v",
                    err);
            rp_serve_end(&s);
            return;
        }
        rp_serve_end(&s);
        panic(BURROW_ANY(TYPE_ERROR, (void *)(uintptr_t)&http_err_abort_handler));
    }
    rp_close(res->body); /* now, so that the trailer is filled in */

    Int trailers = res->trailer != NULL ? map_len(res->trailer) : 0;
    if (trailers > 0) {
        /* A trailer means a chunked response, and a flush makes sure of it
         * rather than the server working out a Content-Length for a short
         * body. */
        HttpResponseController rc = http_new_response_controller(rw);
        (void)http_response_controller_flush(&rc);
    }

    if (trailers == announced) {
        (void)rp_copy_header(h, res->trailer);
        rp_serve_end(&s);
        return;
    }

    Alloc *ha = burrow__map_allocator(h);
    MapIter it = map_iter(res->trailer);
    const void *kp;
    void *vp;
    while (map_next(&it, &kp, &vp)) {
        Str k = fmt_sprintf_v(ha, "%s%s", HTTP_TRAILER_PREFIX, *(const Str *)kp);
        const Slice *vv = (const Slice *)vp;
        for (Int i = 0; i < vv->len; i++)
            (void)http_header_add(h, k, str_clone(ha, ((const Str *)vv->p)[i]));
    }
    rp_serve_end(&s);
}

static void rp_handler_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    httputil_reverse_proxy_serve_http((HttputilReverseProxy *)self, w, r);
}

static const HttpHandlerVT rp_handler_vt = {NULL, rp_handler_serve};

HttpHandler httputil_reverse_proxy_as_handler(HttputilReverseProxy *p) {
    return (HttpHandler){&rp_handler_vt, p};
}
