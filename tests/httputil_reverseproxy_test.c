/* Derived from Go's src/net/http/httputil/reverseproxy_test.go, the reverse
 * proxy's tests. The backends and the frontends are httptest servers on the
 * loopback address.
 * Go source: go1.27.1.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"
#include "../src/xnet/httpguts.h"

#include "burrow/bufio.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/http/httptrace.h"
#include "burrow/net/http/httputil.h"
#include "burrow/net/textproto.h"
#include "burrow/net/url.h"
#include "burrow/netpoll.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_TCP 1
#endif

static void need_tcp(TestingT *t) {
#if !defined(HAVE_TCP)
    testing_t_skip_v(t, "TCP here needs the readiness poll FD");
#else
    (void)t;
#endif
}

/* t.Fatalf, with a return the analyzer can see. */
#define FATALF(...)                                                                    \
    do {                                                                               \
        testing_t_fatalf_v(t, __VA_ARGS__);                                            \
        return;                                                                        \
    } while (0)

/* Go adds a header of its own to hopHeaders for these tests. The list is
 * const here, so the tests use Proxy-Authenticate, a hop-by-hop field that no
 * server or client does anything with. */
#define FAKE_HOP_HEADER "Proxy-Authenticate"

BURROW_SENTINEL_ERROR(
    err_done_testing,
    "done testing the interesting part; so force a 502 Gateway error");
BURROW_SENTINEL_ERROR(err_some, "some error");

static Str cs(const char *s) {
    return str_from_cstr(s);
}

static IoReader body_reader(IoReadCloser rc) {
    return (IoReader){&rc.vt->reader, rc.data};
}

/* The rest of body, read into a. */
static Str read_all(Alloc *a, IoReadCloser body, Error *err) {
    Slice b = io_read_all(a, body_reader(body), err);
    return str_from_bytes(b.p, b.len);
}

static void write_str(HttpResponseWriter w, Str s) {
    (void)http_response_writer_write(
        w, slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE), NULL);
}

static HttpHeader hdr(HttpResponseWriter w) {
    return http_response_writer_header(w);
}

/* log.New(io.Discard, "", 0). */
static LogLogger *quiet_log(void) {
    return log_new(heap_allocator(), io_discard, BURROW_STR_EMPTY, 0);
}

static HttptestServer *serve(HttpHandlerFunc *f) {
    return httptest_new_server(NULL, http_handler_func_as_handler(f));
}

static HttptestServer *serve_proxy(HttputilReverseProxy *p) {
    return httptest_new_server(NULL, httputil_reverse_proxy_as_handler(p));
}

static IoReader no_body(void) {
    IoReader r = {NULL, NULL};
    return r;
}

/* http.NewRequest for a GET with no body, failing the test when it fails. */
static HttpRequest *new_get(TestingT *t, Str url) {
    Error err = BURROW_NO_ERROR;
    HttpRequest *req =
        http_new_request(heap_allocator(), cs("GET"), url, no_body(), &err);
    if (req == NULL)
        testing_t_errorf_v(t, "NewRequest: %v", err);
    return req;
}

/* upgradeType, as the tests use it on a header they have. */
static Str upgrade_type(HttpHeader h) {
    Slice conn = http_header_values(h, cs("Connection"));
    if (!burrow__httpguts_header_values_contains_token((const Str *)conn.p, conn.len,
                                                       cs("Upgrade")))
        return BURROW_STR_EMPTY;
    return http_header_get(h, cs("Upgrade"));
}

/* Hijacks the connection and gives back what Hijack gave, which free_hijacked
 * gives back. */
typedef struct Hijacked {
    NetConn conn;
    BufioReadWriter brw;
} Hijacked;

static bool hijack(HttpResponseWriter w, Hijacked *h, Error *err) {
    memset(h, 0, sizeof *h);
    HttpResponseController rc = http_new_response_controller(w);
    h->conn = http_response_controller_hijack(&rc, &h->brw, err);
    return BURROW_OK(*err);
}

static void free_hijacked(Hijacked *h) {
    net_conn_free(h->conn);
    if (h->brw.reader != NULL)
        bufio_reader_free(h->brw.reader);
    if (h->brw.writer != NULL)
        bufio_writer_free(h->brw.writer);
    memset(h, 0, sizeof *h);
}

/* -------------------------------------------------------- TestReverseProxy */

static void rp_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    TestingT *t = (TestingT *)env;
    if (str_eq(r->method, cs("GET")) &&
        str_eq(http_request_form_value(r, cs("mode")), cs("hangup"))) {
        Hijacked h;
        Error err = BURROW_NO_ERROR;
        (void)hijack(w, &h, &err);
        free_hijacked(&h);
        return;
    }
    if (r->transfer_encoding.len > 0)
        testing_t_errorf_v(t, "backend got unexpected TransferEncoding: %v",
                           r->transfer_encoding);
    if (http_header_get(r->header, cs("X-Forwarded-For")).len == 0)
        testing_t_errorf_v(t, "didn't get X-Forwarded-For header");
    Str c = http_header_get(r->header, cs("Connection"));
    if (c.len != 0)
        testing_t_errorf_v(t, "handler got Connection header value %q", c);
    c = http_header_get(r->header, cs("Te"));
    if (!str_eq(c, cs("trailers")))
        testing_t_errorf_v(t, "handler got Te header value %q; want 'trailers'", c);
    c = http_header_get(r->header, cs("Upgrade"));
    if (c.len != 0)
        testing_t_errorf_v(t, "handler got Upgrade header value %q", c);
    c = http_header_get(r->header, cs("Proxy-Connection"));
    if (c.len != 0)
        testing_t_errorf_v(t, "handler got Proxy-Connection header value %q", c);
    if (!str_eq(r->host, cs("some-name")))
        testing_t_errorf_v(t, "backend got Host header %q, want %q", r->host,
                           cs("some-name"));
    HttpHeader h = hdr(w);
    (void)http_header_set(h, cs("Trailers"), cs("not a special header field name"));
    (void)http_header_set(h, cs("Trailer"), cs("X-Trailer"));
    (void)http_header_set(h, cs("X-Foo"), cs("bar"));
    (void)http_header_set(h, cs("Upgrade"), cs("foo"));
    (void)http_header_set(h, cs(FAKE_HOP_HEADER), cs("foo"));
    (void)http_header_add(h, cs("X-Multi-Value"), cs("foo"));
    (void)http_header_add(h, cs("X-Multi-Value"), cs("bar"));
    HttpCookie ck;
    memset(&ck, 0, sizeof ck);
    ck.name = cs("flavor");
    ck.value = cs("chocolateChip");
    (void)http_set_cookie(w, &ck);
    http_response_writer_write_header(w, 404);
    write_str(w, cs("I am the backend"));
    (void)http_header_set(h, cs("X-Trailer"), cs("trailer_value"));
    (void)http_header_set(h, cs("Trailer:X-Unannounced-Trailer"),
                          cs("unannounced_trailer_value"));
}

static void TestReverseProxy(TestingT *t) {
    need_tcp(t);
    const Str backend_response = cs("I am the backend");
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, rp_backend, t);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    LogLogger *quiet = quiet_log();
    p->error_log = quiet;
    HttptestServer *frontend = serve_proxy(p);
    HttpClient *client = httptest_server_client(frontend);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    HttpRequest *get = new_get(t, frontend->url);
    HttpResponse *res = NULL;
    if (get != NULL) {
        get->host = cs("some-name");
        (void)http_header_set(get->header, cs("Connection"), cs("close, TE"));
        (void)http_header_add(get->header, cs("Te"), cs("foo"));
        (void)http_header_add(get->header, cs("Te"), cs("bar, trailers"));
        (void)http_header_set(get->header, cs("Proxy-Connection"),
                              cs("should be deleted"));
        (void)http_header_set(get->header, cs("Upgrade"), cs("foo"));
        get->close = true;
        res = http_client_do(client, get, &err);
    }
    if (res == NULL) {
        testing_t_errorf_v(t, "Get: %v", err);
    } else {
        if (res->status_code != 404)
            testing_t_errorf_v(t, "got res.StatusCode %d; expected %d",
                               res->status_code, 404);
        Str g = http_header_get(res->header, cs("X-Foo"));
        if (!str_eq(g, cs("bar")))
            testing_t_errorf_v(t, "got X-Foo %q; expected %q", g, cs("bar"));
        g = http_header_get(res->header, cs(FAKE_HOP_HEADER));
        if (g.len != 0)
            testing_t_errorf_v(t, "got %s header value %q", cs(FAKE_HOP_HEADER), g);
        g = http_header_get(res->header, cs("Trailers"));
        if (!str_eq(g, cs("not a special header field name")))
            testing_t_errorf_v(t, "header Trailers = %q; want %q", g,
                               cs("not a special header field name"));
        Int n = http_header_values(res->header, cs("X-Multi-Value")).len;
        if (n != 2)
            testing_t_errorf_v(t, "got %d X-Multi-Value header values; expected %d", n,
                               2);
        n = http_header_values(res->header, cs("Set-Cookie")).len;
        if (n != 1)
            testing_t_errorf_v(t, "got %d SetCookies, want %d", n, 1);
        Slice *tv = res->trailer != NULL
                        ? BURROW_MAP_GET(Str, Slice, res->trailer, cs("X-Trailer"))
                        : NULL;
        if (tv == NULL || tv->len != 0 || map_len(res->trailer) != 1)
            testing_t_errorf_v(t,
                               "before reading body, Trailer = %v; want "
                               "http.Header{\"X-Trailer\":[]string(nil)}",
                               res->trailer);
        Slice cookies = http_response_cookies(res, a);
        if (cookies.len < 1 ||
            !str_eq(((const HttpCookie *)cookies.p)[0].name, cs("flavor")))
            testing_t_errorf_v(t, "unexpected cookies %d", cookies.len);
        Str body = read_all(a, res->body, &err);
        if (!str_eq(body, backend_response))
            testing_t_errorf_v(t, "got body %q; expected %q", body, backend_response);
        g = http_header_get(res->trailer, cs("X-Trailer"));
        if (!str_eq(g, cs("trailer_value")))
            testing_t_errorf_v(t, "Trailer(X-Trailer) = %q ; want %q", g,
                               cs("trailer_value"));
        g = http_header_get(res->trailer, cs("X-Unannounced-Trailer"));
        if (!str_eq(g, cs("unannounced_trailer_value")))
            testing_t_errorf_v(t, "Trailer(X-Unannounced-Trailer) = %q ; want %q", g,
                               cs("unannounced_trailer_value"));
    }
    http_response_free(res);
    http_request_free(get);

    /* A backend that cannot be reached, or that sends no response, makes a
     * StatusBadGateway. */
    get = new_get(t, fmt_sprintf_v(a, "%s/?mode=hangup", frontend->url));
    res = NULL;
    if (get != NULL) {
        get->close = true;
        res = http_client_do(client, get, &err);
    }
    if (res == NULL)
        testing_t_errorf_v(t, "%v", err);
    else if (res->status_code != HTTP_STATUS_BAD_GATEWAY)
        testing_t_errorf_v(t, "request to bad proxy = %v; want 502 StatusBadGateway",
                           res->status);
    http_response_free(res);
    http_request_free(get);

    httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    log_logger_free(heap_allocator(), quiet);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
    arena_free(&ar);
}

/* ------------------------------------- headers named in Connection, 16875 */

#define FAKE_CONNECTION_TOKEN "X-Fake-Connection-Token"
#define SOME_CONN_HEADER "X-Some-Conn-Header"

typedef struct ProxyEnv {
    TestingT *t;
    HttputilReverseProxy *p;
} ProxyEnv;

static void strip_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    TestingT *t = (TestingT *)env;
    const char *names[] = {"Connection", FAKE_CONNECTION_TOKEN, SOME_CONN_HEADER};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        Str c = http_header_get(r->header, cs(names[i]));
        if (c.len != 0)
            testing_t_errorf_v(t, "handler got header %q = %q; want empty",
                               cs(names[i]), c);
    }
    HttpHeader h = hdr(w);
    (void)http_header_add(h, cs("Connection"), cs("Upgrade, " FAKE_CONNECTION_TOKEN));
    (void)http_header_add(h, cs("Connection"), cs(SOME_CONN_HEADER));
    (void)http_header_set(h, cs(SOME_CONN_HEADER), cs("should be deleted"));
    (void)http_header_set(h, cs(FAKE_CONNECTION_TOKEN), cs("should be deleted"));
    write_str(w, cs("I am the backend"));
}

static void strip_frontend(void *env, HttpResponseWriter w, HttpRequest *r) {
    ProxyEnv *e = (ProxyEnv *)env;
    TestingT *t = e->t;
    httputil_reverse_proxy_serve_http(e->p, w, r);
    const char *names[] = {SOME_CONN_HEADER, FAKE_CONNECTION_TOKEN};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        Str c = http_header_get(r->header, cs(names[i]));
        if (!str_eq(c, cs("should be deleted")))
            testing_t_errorf_v(t, "handler modified header %q = %q; want %q",
                               cs(names[i]), c, cs("should be deleted"));
    }
    /* The fields of Connection, which have to be the three the request
     * named. */
    const char *want[] = {"Upgrade", SOME_CONN_HEADER, FAKE_CONNECTION_TOKEN};
    bool seen[3] = {false, false, false};
    Int n = 0;
    bool unknown = false;
    Slice conn = http_header_values(r->header, cs("Connection"));
    for (Int i = 0; i < conn.len; i++) {
        Str f = ((const Str *)conn.p)[i];
        for (;;) {
            Str rest = BURROW_STR_EMPTY;
            bool found = false;
            Str sf = strings_trim_space(strings_cut(f, cs(","), &rest, &found));
            if (sf.len > 0) {
                n++;
                bool known = false;
                for (size_t j = 0; j < 3; j++) {
                    if (str_eq(sf, cs(want[j]))) {
                        known = !seen[j];
                        seen[j] = true;
                    }
                }
                unknown = unknown || !known;
            }
            if (!found)
                break;
            f = rest;
        }
    }
    if (unknown || n != 3)
        testing_t_errorf_v(t, "handler modified header %q = %v; want %q %q %q",
                           cs("Connection"), conn, cs(want[0]), cs(want[1]),
                           cs(want[2]));
}

static void TestReverseProxyStripHeadersPresentInConnection(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, strip_backend, t);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    ProxyEnv e = {
        t, httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url)};
    HttpHandlerFunc ff = BURROW_FN(HttpHandlerFunc, strip_frontend, &e);
    HttptestServer *frontend = serve(&ff);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);

    HttpRequest *get = new_get(t, frontend->url);
    HttpResponse *res = NULL;
    if (get != NULL) {
        (void)http_header_add(get->header, cs("Connection"),
                              cs("Upgrade, " FAKE_CONNECTION_TOKEN));
        (void)http_header_add(get->header, cs("Connection"), cs(SOME_CONN_HEADER));
        (void)http_header_set(get->header, cs(SOME_CONN_HEADER),
                              cs("should be deleted"));
        (void)http_header_set(get->header, cs(FAKE_CONNECTION_TOKEN),
                              cs("should be deleted"));
        res = http_client_do(httptest_server_client(frontend), get, &err);
    }
    if (res == NULL) {
        testing_t_errorf_v(t, "Get: %v", err);
    } else {
        Str body = read_all(arena_allocator(&ar), res->body, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "reading body: %v", err);
        else if (!str_eq(body, cs("I am the backend")))
            testing_t_errorf_v(t, "got body %q; want %q", body, cs("I am the backend"));
        const char *names[] = {"Connection", SOME_CONN_HEADER, FAKE_CONNECTION_TOKEN};
        for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
            Str c = http_header_get(res->header, cs(names[i]));
            if (c.len != 0)
                testing_t_errorf_v(t, "handler got header %q = %q; want empty",
                                   cs(names[i]), c);
        }
    }
    http_response_free(res);
    http_request_free(get);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(e.p);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
    arena_free(&ar);
}

/* Issue 46313. */
static void strip_empty_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    TestingT *t = (TestingT *)env;
    Slice c = http_header_values(r->header, cs("Connection"));
    if (c.len != 0)
        testing_t_errorf_v(t, "handler got header %q = %v; want empty",
                           cs("Connection"), c);
    Str v = http_header_get(r->header, cs(SOME_CONN_HEADER));
    if (v.len != 0)
        testing_t_errorf_v(t, "handler got header %q = %q; want empty",
                           cs(SOME_CONN_HEADER), v);
    HttpHeader h = hdr(w);
    (void)http_header_add(h, cs("Connection"), BURROW_STR_EMPTY);
    (void)http_header_add(h, cs("Connection"), cs(SOME_CONN_HEADER));
    (void)http_header_set(h, cs(SOME_CONN_HEADER), cs("should be deleted"));
    write_str(w, cs("I am the backend"));
}

static void strip_empty_frontend(void *env, HttpResponseWriter w, HttpRequest *r) {
    ProxyEnv *e = (ProxyEnv *)env;
    TestingT *t = e->t;
    httputil_reverse_proxy_serve_http(e->p, w, r);
    Str c = http_header_get(r->header, cs(SOME_CONN_HEADER));
    if (!str_eq(c, cs("should be deleted")))
        testing_t_errorf_v(t, "handler modified header %q = %q; want %q",
                           cs(SOME_CONN_HEADER), c, cs("should be deleted"));
}

static void TestReverseProxyStripEmptyConnection(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, strip_empty_backend, t);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    ProxyEnv e = {
        t, httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url)};
    HttpHandlerFunc ff = BURROW_FN(HttpHandlerFunc, strip_empty_frontend, &e);
    HttptestServer *frontend = serve(&ff);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);

    HttpRequest *get = new_get(t, frontend->url);
    HttpResponse *res = NULL;
    if (get != NULL) {
        (void)http_header_add(get->header, cs("Connection"), BURROW_STR_EMPTY);
        (void)http_header_add(get->header, cs("Connection"), cs(SOME_CONN_HEADER));
        (void)http_header_set(get->header, cs(SOME_CONN_HEADER),
                              cs("should be deleted"));
        res = http_client_do(httptest_server_client(frontend), get, &err);
    }
    if (res == NULL) {
        testing_t_errorf_v(t, "Get: %v", err);
    } else {
        Str body = read_all(arena_allocator(&ar), res->body, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "reading body: %v", err);
        else if (!str_eq(body, cs("I am the backend")))
            testing_t_errorf_v(t, "got body %q; want %q", body, cs("I am the backend"));
        Str c = http_header_get(res->header, cs("Connection"));
        if (c.len != 0)
            testing_t_errorf_v(t, "handler got header %q = %q; want empty",
                               cs("Connection"), c);
        c = http_header_get(res->header, cs(SOME_CONN_HEADER));
        if (c.len != 0)
            testing_t_errorf_v(t, "handler got header %q = %q; want empty",
                               cs(SOME_CONN_HEADER), c);
    }
    http_response_free(res);
    http_request_free(get);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(e.p);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
    arena_free(&ar);
}

/* ------------------------------------------------------- X-Forwarded-For */

static void xff_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    TestingT *t = (TestingT *)env;
    Str v = http_header_get(r->header, cs("X-Forwarded-For"));
    if (v.len == 0)
        testing_t_errorf_v(t, "didn't get X-Forwarded-For header");
    if (!strings_contains(v, cs("client ip")))
        testing_t_errorf_v(t, "X-Forwarded-For didn't contain prior data");
    http_response_writer_write_header(w, 404);
    write_str(w, cs("I am the backend"));
}

static void TestXForwardedFor(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, xff_backend, t);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    HttptestServer *frontend = serve_proxy(p);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);

    HttpRequest *get = new_get(t, frontend->url);
    HttpResponse *res = NULL;
    if (get != NULL) {
        (void)http_header_set(get->header, cs("Connection"), cs("close"));
        (void)http_header_set(get->header, cs("X-Forwarded-For"), cs("client ip"));
        get->close = true;
        res = http_client_do(httptest_server_client(frontend), get, &err);
    }
    if (res == NULL) {
        testing_t_errorf_v(t, "Get: %v", err);
    } else {
        if (res->status_code != 404)
            testing_t_errorf_v(t, "got res.StatusCode %d; expected %d",
                               res->status_code, 404);
        Str body = read_all(arena_allocator(&ar), res->body, &err);
        if (!str_eq(body, cs("I am the backend")))
            testing_t_errorf_v(t, "got body %q; expected %q", body,
                               cs("I am the backend"));
    }
    http_response_free(res);
    http_request_free(get);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
    arena_free(&ar);
}

/* Issue 38079: X-Forwarded-For is left alone when it is there and nil. */
static void omit_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    TestingT *t = (TestingT *)env;
    Str v = http_header_get(r->header, cs("X-Forwarded-For"));
    if (v.len != 0)
        testing_t_errorf_v(t, "got X-Forwarded-For header: %q", v);
    write_str(w, cs("hi"));
}

typedef struct OldDirector {
    HttputilDirectorFunc old;
    bool parse_form;
} OldDirector;

static void omit_director(void *env, HttpRequest *r) {
    OldDirector *d = (OldDirector *)env;
    Str key = cs("X-Forwarded-For");
    Slice nil = {NULL, 0, 0, TYPE_STRING};
    (void)map_set(r->header, &key, &nil);
    BURROW_CALLF(d->old, r);
}

static void TestXForwardedFor_Omit(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, omit_backend, t);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    HttptestServer *frontend = serve_proxy(p);
    OldDirector d = {p->director, false};
    p->director = BURROW_FN(HttputilDirectorFunc, omit_director, &d);

    HttpRequest *get = new_get(t, frontend->url);
    HttpResponse *res = NULL;
    if (get != NULL) {
        get->host = cs("some-name");
        get->close = true;
        res = http_client_do(httptest_server_client(frontend), get, &err);
        if (res == NULL)
            testing_t_errorf_v(t, "Get: %v", err);
    }
    http_response_free(res);
    http_request_free(get);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
}

static const char *const forwarded_headers[] = {
    "Forwarded",
    "X-Forwarded-For",
    "X-Forwarded-Host",
    "X-Forwarded-Proto",
};

static void forwarded_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    TestingT *t = (TestingT *)env;
    (void)w;
    for (size_t i = 0; i < sizeof forwarded_headers / sizeof forwarded_headers[0];
         i++) {
        Str v = http_header_get(r->header, cs(forwarded_headers[i]));
        if (v.len != 0)
            testing_t_errorf_v(t, "got %s header: %q", cs(forwarded_headers[i]), v);
    }
}

static void set_url_rewrite(void *env, HttputilProxyRequest *r) {
    httputil_proxy_request_set_url(r, (const Url *)env);
}

/* &ReverseProxy{Rewrite: func(r) { r.SetURL(target) }}. */
static HttputilReverseProxy set_url_proxy(const Url *target) {
    HttputilReverseProxy p;
    memset(&p, 0, sizeof p);
    p.rewrite =
        BURROW_FN(HttputilRewriteFunc, set_url_rewrite, (void *)(uintptr_t)target);
    return p;
}

static void TestReverseProxyRewriteStripsForwarded(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, forwarded_backend, t);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    HttputilReverseProxy p = set_url_proxy(backend_url);
    HttptestServer *frontend = serve_proxy(&p);

    HttpRequest *get = new_get(t, frontend->url);
    HttpResponse *res = NULL;
    if (get != NULL) {
        get->host = cs("some-name");
        get->close = true;
        for (size_t i = 0; i < sizeof forwarded_headers / sizeof forwarded_headers[0];
             i++)
            (void)http_header_set(get->header, cs(forwarded_headers[i]), cs("x"));
        res = http_client_do(httptest_server_client(frontend), get, &err);
        if (res == NULL)
            testing_t_errorf_v(t, "Get: %v", err);
    }
    http_response_free(res);
    http_request_free(get);
    httptest_server_free(frontend);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
}

/* ------------------------------------------------------------------ query */

static void query_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    HttpHeader h = hdr(w);
    /* A header keeps the values it is given, and the request goes first. */
    (void)http_header_set(h, cs("X-Got-Query"),
                          str_clone(burrow__map_allocator(h), r->url->raw_query));
    write_str(w, cs("hi"));
}

static void TestReverseProxyQuery(TestingT *t) {
    need_tcp(t);
    static const struct {
        const char *base_suffix; /* to add to the backend's URL */
        const char *req_suffix;  /* to add to the frontend's */
        const char *want;        /* the query the backend sees, without "?" */
    } tests[] = {
        {"", "", ""},
        {"?sta=tic", "?us=er", "sta=tic&us=er"},
        {"", "?us=er", "us=er"},
        {"?sta=tic", "", "sta=tic"},
    };
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, query_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        Url *u = url_parse(
            a, fmt_sprintf_v(a, "%s%s", backend->url, cs(tests[i].base_suffix)), &err);
        if (u == NULL) {
            testing_t_errorf_v(t, "%v", err);
            break;
        }
        HttputilReverseProxy *p = httputil_new_single_host_reverse_proxy(a, u);
        HttptestServer *frontend = serve_proxy(p);
        HttpRequest *req = new_get(
            t, fmt_sprintf_v(a, "%s%s", frontend->url, cs(tests[i].req_suffix)));
        HttpResponse *res = NULL;
        if (req != NULL) {
            req->close = true;
            res = http_client_do(httptest_server_client(frontend), req, &err);
        }
        if (res == NULL) {
            testing_t_errorf_v(t, "%d. Get: %v", (Int)i, err);
        } else {
            Str g = http_header_get(res->header, cs("X-Got-Query"));
            if (!str_eq(g, cs(tests[i].want)))
                testing_t_errorf_v(t, "%d. got query %q; expected %q", (Int)i, g,
                                   cs(tests[i].want));
        }
        http_response_free(res);
        http_request_free(req);
        httptest_server_free(frontend);
        httputil_reverse_proxy_free(p);
    }
    httptest_server_free(backend);
    arena_free(&ar);
}

/* --------------------------------------------------------------- flushing */

static void hi_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    write_str(w, cs("hi"));
}

/* Gets url with a client that closes the connection, and checks for the
 * body "hi". */
static void get_hi(TestingT *t, HttptestServer *frontend) {
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = new_get(t, frontend->url);
    if (req == NULL)
        return;
    req->close = true;
    HttpResponse *res = http_client_do(httptest_server_client(frontend), req, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "Get: %v", err);
    } else {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Str body = read_all(arena_allocator(&ar), res->body, &err);
        if (!str_eq(body, cs("hi")))
            testing_t_errorf_v(t, "got body %q; expected %q", body, cs("hi"));
        arena_free(&ar);
    }
    http_response_free(res);
    http_request_free(req);
}

static void TestReverseProxyFlushInterval(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, hi_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    p->flush_interval = TIME_MICROSECOND;
    HttptestServer *frontend = serve_proxy(p);
    get_hi(t, frontend);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
}

/* mockFlusher, a writer whose Flush only notes that it was called, and
 * wrappedRW, a writer with no Flush whose Unwrap gives the mockFlusher. */
typedef struct MockFlusher {
    HttpResponseWriter rw;
    bool flushed;
} MockFlusher;

static Int mf_write(void *self, Slice p, Error *err) {
    return http_response_writer_write(((MockFlusher *)self)->rw, p, err);
}
static HttpHeader mf_header(void *self) {
    return hdr(((MockFlusher *)self)->rw);
}
static void mf_write_header(void *self, Int code) {
    http_response_writer_write_header(((MockFlusher *)self)->rw, code);
}
static Error mf_flush(void *self) {
    ((MockFlusher *)self)->flushed = true;
    return BURROW_NO_ERROR;
}

static const HttpResponseWriterVT mock_flusher_vt = {
    .writer = {NULL, mf_write},
    .header = mf_header,
    .write_header = mf_write_header,
    .flush = mf_flush,
};

static HttpResponseWriter wrapped_unwrap(void *self) {
    HttpResponseWriter w = {&mock_flusher_vt, self};
    return w;
}

static const HttpResponseWriterVT wrapped_vt = {
    .writer = {NULL, mf_write},
    .header = mf_header,
    .write_header = mf_write_header,
    .unwrap = wrapped_unwrap,
};

typedef struct FlushEnv {
    HttputilReverseProxy *p;
    MockFlusher mf;
} FlushEnv;

static void flush_middleware(void *env, HttpResponseWriter w, HttpRequest *r) {
    FlushEnv *e = (FlushEnv *)env;
    e->mf.rw = w;
    HttpResponseWriter wrapped = {&wrapped_vt, &e->mf};
    httputil_reverse_proxy_serve_http(e->p, wrapped, r);
}

static void TestReverseProxyResponseControllerFlushInterval(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, hi_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    FlushEnv e;
    memset(&e, 0, sizeof e);
    e.p = httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    e.p->flush_interval = -1; /* flush at once */
    HttpHandlerFunc ff = BURROW_FN(HttpHandlerFunc, flush_middleware, &e);
    HttptestServer *frontend = serve(&ff);
    get_hi(t, frontend);
    if (!e.mf.flushed)
        testing_t_errorf_v(t, "response writer was not flushed");
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(e.p);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
}

static void flush_headers_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    Chan *stop = (Chan *)env;
    (void)r;
    (void)http_header_add(hdr(w), cs("MyHeader"), cs("hi"));
    http_response_writer_write_header(w, 200);
    HttpResponseController rc = http_new_response_controller(w);
    (void)http_response_controller_flush(&rc);
    (void)chan_recv(stop, NULL);
}

static void TestReverseProxyFlushIntervalHeaders(TestingT *t) {
    need_tcp(t);
    Chan *stop = chan_make(heap_allocator(), TYPE_BOOL, 0);
    if (stop == NULL)
        FATALF("chan_make");
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, flush_headers_backend, stop);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        chan_close(stop);
        httptest_server_free(backend);
        chan_free(stop);
        FATALF("%v", err);
    }
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    p->flush_interval = TIME_MICROSECOND;
    HttptestServer *frontend = serve_proxy(p);

    ContextCancelFunc cancel;
    Context ctx = context_with_timeout(heap_allocator(), context_background(),
                                       10 * TIME_SECOND, &cancel);
    HttpRequest *req = http_new_request_with_context(heap_allocator(), ctx, cs("GET"),
                                                     frontend->url, no_body(), &err);
    HttpResponse *res = NULL;
    if (req != NULL) {
        req->close = true;
        res = http_client_do(httptest_server_client(frontend), req, &err);
    }
    if (res == NULL) {
        testing_t_errorf_v(t, "Get: %v", err);
    } else {
        Str g = http_header_get(res->header, cs("MyHeader"));
        if (!str_eq(g, cs("hi")))
            testing_t_errorf_v(t, "got header %q; expected %q", g, cs("hi"));
    }
    /* The backend waits for this, and so the frontend does too. */
    chan_close(stop);
    http_response_free(res);
    http_request_free(req);
    BURROW_CALLF0(cancel);
    context_release(ctx);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
    chan_free(stop);
}

/* ----------------------------------------------------------- cancellation */

typedef struct CancelEnv {
    TestingT *t;
    Chan *in_flight;
    Chan *done;
    HttpTransport *tr;
    HttpRequest *req;
} CancelEnv;

static void cancel_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    CancelEnv *e = (CancelEnv *)env;
    (void)r;
    chan_close(e->in_flight); /* so the client cancels its request */
    if (w.vt->close_notify == NULL) {
        testing_t_errorf_v(e->t, "ResponseWriter is not a CloseNotifier");
        return;
    }
    TimeTimer *tm = time_new_timer(heap_allocator(), 10 * TIME_SECOND);
    if (tm == NULL)
        return;
    SelectCase cases[2] = {BURROW_RECV(time_timer_c(tm), NULL),
                           BURROW_RECV(w.vt->close_notify(w.data), NULL)};
    Int i = chan_select(cases, 2);
    (void)time_timer_stop(tm);
    time_timer_free(tm);
    if (i == 0) {
        /* Only a broken implementation gets here, as the CloseNotify case
         * should be at once. */
        testing_t_errorf_v(e->t, "Handler never saw CloseNotify");
        return;
    }
    http_response_writer_write_header(w, HTTP_STATUS_OK);
    write_str(w, cs("I am the backend"));
}

static void cancel_when_in_flight(void *env) {
    CancelEnv *e = (CancelEnv *)env;
    SelectCase cases[2] = {BURROW_RECV(e->in_flight, NULL), BURROW_RECV(e->done, NULL)};
    if (chan_select(cases, 2) == 0)
        http_transport_cancel_request(e->tr, e->req);
}

static void TestReverseProxyCancellation(TestingT *t) {
    need_tcp(t);
    CancelEnv e;
    memset(&e, 0, sizeof e);
    e.t = t;
    e.in_flight = chan_make(heap_allocator(), TYPE_BOOL, 0);
    e.done = chan_make(heap_allocator(), TYPE_BOOL, 0);
    if (e.in_flight == NULL || e.done == NULL) {
        chan_free(e.in_flight);
        chan_free(e.done);
        FATALF("chan_make");
    }
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, cancel_backend, &e);
    HttptestServer *backend = serve(&bf);
    LogLogger *quiet = quiet_log();
    backend->config.error_log = quiet;
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    HttputilReverseProxy *p =
        backend_url != NULL
            ? httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url)
            : NULL;
    HttptestServer *frontend = NULL;
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    if (p == NULL) {
        testing_t_errorf_v(t, "%v", err);
        goto done;
    }
    /* Leaves out errors such as "http: proxy error: read tcp
     * 127.0.0.1:44643: use of closed network connection". */
    p->error_log = quiet;
    frontend = serve_proxy(p);
    e.tr = &frontend->transport;
    e.req = new_get(t, frontend->url);
    if (e.req == NULL)
        goto done;
    if (!sync_wait_group_go(&wg, BURROW_FN(Func, cancel_when_in_flight, &e))) {
        testing_t_errorf_v(t, "go failed");
        goto done;
    }
    HttpResponse *res = http_client_do(httptest_server_client(frontend), e.req, &err);
    if (res != NULL)
        testing_t_errorf_v(t, "got response %v; want nil", res->status);
    /* An error such as: Get "http://127.0.0.1:58079": read tcp
     * 127.0.0.1:58079: use of closed network connection. */
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "Server.Client().Do() returned nil error; want non-nil "
                              "error");
    http_response_free(res);

done:
    chan_close(e.done);
    sync_wait_group_wait(&wg);
    http_request_free(e.req);
    if (frontend != NULL)
        httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
    log_logger_free(heap_allocator(), quiet);
    chan_free(e.in_flight);
    chan_free(e.done);
}

/* -------------------------------------------------------------- nil body */

typedef struct NilBodyEnv {
    TestingT *t;
    Str backend_url;
} NilBodyEnv;

static void nil_body_frontend(void *env, HttpResponseWriter w, HttpRequest *in) {
    NilBodyEnv *e = (NilBodyEnv *)env;
    (void)in;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Url *back = url_parse(a, e->backend_url, &err);
    HttputilReverseProxy *rp =
        back != NULL ? httputil_new_single_host_reverse_proxy(a, back) : NULL;
    StringsReader *sr = strings_new_reader(a, cs("GET / HTTP/1.0\r\n\r\n"));
    BufioReader *br =
        sr != NULL ? bufio_new_reader(a, strings_reader_as_io_reader(sr)) : NULL;
    HttpRequest *r = br != NULL ? http_read_request(a, br, &err) : NULL;
    if (rp == NULL || r == NULL) {
        testing_t_errorf_v(e->t, "%v", err);
    } else {
        /* This worked by accident in Go 1.4 and before, so it is kept
         * working. */
        r->body = (IoReadCloser){NULL, NULL};
        httputil_reverse_proxy_serve_http(rp, w, r);
    }
    http_request_free(r);
    httputil_reverse_proxy_free(rp);
    arena_free(&ar);
}

/* Issue 12344. */
static void TestNilBody(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, hi_backend, NULL);
    HttptestServer *backend = serve(&bf);
    NilBodyEnv e = {t, backend->url};
    HttpHandlerFunc ff = BURROW_FN(HttpHandlerFunc, nil_body_frontend, &e);
    HttptestServer *frontend = serve(&ff);
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_get(frontend->url, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Str slurp = read_all(arena_allocator(&ar), res->body, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%v", err);
        else if (!str_eq(slurp, cs("hi")))
            testing_t_errorf_v(t, "Got %q; want %q", slurp, cs("hi"));
        arena_free(&ar);
    }
    http_response_free(res);
    httptest_server_free(frontend);
    httptest_server_free(backend);
}

/* -------------------------------------------------------------- User-Agent */

typedef struct UAEnv {
    SyncMutex mu;
    Arena ar;
    Str got;
} UAEnv;

static void ua_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    UAEnv *e = (UAEnv *)env;
    (void)w;
    sync_mutex_lock(&e->mu);
    e->got = str_clone(arena_allocator(&e->ar),
                       http_header_get(r->header, cs("User-Agent")));
    sync_mutex_unlock(&e->mu);
}

static void url_director(void *env, HttpRequest *req) {
    req->url = (Url *)env;
}

/* Issue 15524. */
static void TestUserAgentHeader(TestingT *t) {
    need_tcp(t);
    UAEnv e;
    memset(&e, 0, sizeof e);
    arena_init(&e.ar, heap_allocator(), 0);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, ua_backend, &e);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        arena_free(&e.ar);
        FATALF("%v", err);
    }
    HttputilReverseProxy p;
    memset(&p, 0, sizeof p);
    LogLogger *quiet = quiet_log();
    p.error_log = quiet;
    p.director = BURROW_FN(HttputilDirectorFunc, url_director, backend_url);
    HttptestServer *frontend = serve_proxy(&p);

    const char *sent[] = {"explicit UA", ""};
    for (size_t i = 0; i < sizeof sent / sizeof sent[0]; i++) {
        HttpRequest *req = new_get(t, frontend->url);
        if (req == NULL)
            break;
        (void)http_header_set(req->header, cs("User-Agent"), cs(sent[i]));
        req->close = true;
        HttpResponse *res = http_client_do(httptest_server_client(frontend), req, &err);
        if (res == NULL) {
            testing_t_errorf_v(t, "Get: %v", err);
            http_request_free(req);
            break;
        }
        (void)res->body.vt->closer.close(res->body.data);
        sync_mutex_lock(&e.mu);
        Str got = e.got;
        sync_mutex_unlock(&e.mu);
        if (!str_eq(got, cs(sent[i])))
            testing_t_errorf_v(t, "got forwarded User-Agent %q, want %q", got,
                               cs(sent[i]));
        http_response_free(res);
        http_request_free(req);
    }
    httptest_server_free(frontend);
    log_logger_free(heap_allocator(), quiet);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
    arena_free(&e.ar);
}

/* ------------------------------------------------------------- BufferPool */

#define POOL_SIZE 1234

typedef struct LogPool {
    SyncMutex mu;
    char log[4][32];
    Int n;
    Byte buf[POOL_SIZE];
} LogPool;

static void pool_add_log(LogPool *lp, Str event) {
    sync_mutex_lock(&lp->mu);
    if (lp->n < 4) {
        size_t n = event.len < 31 ? (size_t)event.len : 31;
        memcpy(lp->log[lp->n], event.p, n);
        lp->log[lp->n][n] = '\0';
    }
    lp->n++;
    sync_mutex_unlock(&lp->mu);
}

static Slice pool_get(void *self) {
    LogPool *lp = (LogPool *)self;
    pool_add_log(lp, cs("getBuf"));
    return slice_from(lp->buf, POOL_SIZE, POOL_SIZE, TYPE_BYTE);
}

static void pool_put(void *self, Slice p) {
    char ev[32];
    (void)snprintf(ev, sizeof ev, "putBuf-%lld", (long long)p.len);
    pool_add_log((LogPool *)self, cs(ev));
}

static const HttputilBufferPoolVT log_pool_vt = {NULL, pool_get, pool_put};

static void TestReverseProxyGetPutBuffer(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, hi_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    LogPool lp;
    memset(&lp, 0, sizeof lp);
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    p->buffer_pool = (HttputilBufferPool){&log_pool_vt, &lp};
    HttptestServer *frontend = serve_proxy(p);
    get_hi(t, frontend);
    sync_mutex_lock(&lp.mu);
    if (lp.n != 2 || strcmp(lp.log[0], "getBuf") != 0 ||
        strcmp(lp.log[1], "putBuf-1234") != 0)
        testing_t_errorf_v(t,
                           "Log events = %d %q %q; want [\"getBuf\" \"putBuf-1234\"]",
                           lp.n, cs(lp.log[0]), cs(lp.log[1]));
    sync_mutex_unlock(&lp.mu);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
}

/* ------------------------------------------------------------------- POST */

typedef struct PostEnv {
    TestingT *t;
    Slice want;
} PostEnv;

static void post_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    PostEnv *e = (PostEnv *)env;
    TestingT *t = e->t;
    Error err = BURROW_NO_ERROR;
    Slice slurp = io_read_all(heap_allocator(), body_reader(r->body), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Backend body read = %v", err);
    if (slurp.len != e->want.len)
        testing_t_errorf_v(t, "Backend read %d request body bytes; want %d", slurp.len,
                           e->want.len);
    if (!bytes_equal(slurp, e->want))
        testing_t_errorf_v(t, "Backend read wrong request body."); /* 1MB, no details */
    if (slurp.p != NULL)
        mem_free(heap_allocator(), slurp.p, (size_t)slurp.cap, 1);
    write_str(w, cs("I am the backend"));
}

static void TestReverseProxy_Post(TestingT *t) {
    need_tcp(t);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    PostEnv e = {t, bytes_repeat(a, slice_from((void *)(uintptr_t)"a", 1, 1, TYPE_BYTE),
                                 1 << 20)};
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, post_backend, &e);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        arena_free(&ar);
        FATALF("%v", err);
    }
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    HttptestServer *frontend = serve_proxy(p);

    BytesReader *br = bytes_new_reader(a, e.want);
    HttpRequest *post =
        br != NULL ? http_new_request(heap_allocator(), cs("POST"), frontend->url,
                                      bytes_reader_as_io_reader(br), &err)
                   : NULL;
    HttpResponse *res =
        post != NULL ? http_client_do(httptest_server_client(frontend), post, &err)
                     : NULL;
    if (res == NULL) {
        testing_t_errorf_v(t, "Do: %v", err);
    } else {
        if (res->status_code != 200)
            testing_t_errorf_v(t, "got res.StatusCode %d; expected %d",
                               res->status_code, 200);
        Str body = read_all(a, res->body, &err);
        if (!str_eq(body, cs("I am the backend")))
            testing_t_errorf_v(t, "got body %q; expected %q", body,
                               cs("I am the backend"));
    }
    http_response_free(res);
    http_request_free(post);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
    arena_free(&ar);
}

/* ----------------------------------------------------- test round trippers */

/* http.Response{StatusCode: code}, as a transport would give it, so the proxy
 * can free it. */
static HttpResponse *new_response(Int code) {
    Alloc *a = heap_allocator();
    HttpResponse *r = (HttpResponse *)mem_alloc(a, sizeof *r, _Alignof(HttpResponse));
    if (r == NULL)
        return NULL;
    memset(r, 0, sizeof *r);
    r->a = a;
    arena_init(&r->arena, a, 0);
    r->status_code = code;
    r->status = http_status_text(code);
    r->proto = cs("HTTP/1.1");
    r->proto_major = 1;
    r->proto_minor = 1;
    r->header = http_header_make(arena_allocator(&r->arena));
    return r;
}

/* A RoundTripperFunc that checks the request and fails. */
static HttpResponse *nil_body_round_trip(void *self, HttpRequest *req, Error *err) {
    TestingT *t = (TestingT *)self;
    if (req->body.vt != NULL)
        testing_t_errorf_v(t, "Body != nil; want a nil Body");
    *err = err_done_testing;
    return NULL;
}

static const HttpRoundTripperVT nil_body_rt_vt = {NULL, nil_body_round_trip};

/* Issue 16036: a request is sent with a nil body when it can be. */
static void TestReverseProxy_NilBody(TestingT *t) {
    need_tcp(t);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), cs("http://fake.tld/"), &err);
    if (backend_url == NULL)
        FATALF("%v", err);
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    LogLogger *quiet = quiet_log();
    p->error_log = quiet;
    p->transport = (HttpRoundTripper){&nil_body_rt_vt, t};
    HttptestServer *frontend = serve_proxy(p);
    HttpResponse *res =
        http_client_get(httptest_server_client(frontend), frontend->url, &err);
    if (res == NULL)
        testing_t_errorf_v(t, "%v", err);
    else if (res->status_code != 502)
        testing_t_errorf_v(t, "status code = %v; want 502 (Gateway Error)",
                           res->status);
    http_response_free(res);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    log_logger_free(heap_allocator(), quiet);
    url_free(heap_allocator(), backend_url);
}

static HttpResponse *header_round_trip(void *self, HttpRequest *req, Error *err) {
    TestingT *t = (TestingT *)self;
    if (req->header == NULL)
        testing_t_errorf_v(t, "Header == nil; want a non-nil Header");
    *err = err_done_testing;
    return NULL;
}

static const HttpRoundTripperVT header_rt_vt = {NULL, header_round_trip};

static void noop_director(void *env, HttpRequest *req) {
    (void)env;
    (void)req;
}

/* Issue 33142: the outgoing request always has a header. */
static void TestReverseProxy_AllocatedHeader(TestingT *t) {
    HttputilReverseProxy p;
    memset(&p, 0, sizeof p);
    LogLogger *quiet = quiet_log();
    p.error_log = quiet;
    p.director = BURROW_FN(HttputilDirectorFunc, noop_director, NULL);
    p.transport = (HttpRoundTripper){&header_rt_vt, t};
    HttptestResponseRecorder *rec = httptest_new_recorder(heap_allocator());
    HttpRequest *req = new_get(t, cs("http://fake.tld/"));
    if (rec != NULL && req != NULL) {
        req->header = NULL;
        req->proto = cs("HTTP/1.0");
        req->proto_major = 1;
        req->proto_minor = 0;
        httputil_reverse_proxy_serve_http(
            &p, httptest_response_recorder_as_response_writer(rec), req);
    }
    http_request_free(req);
    httptest_response_recorder_free(rec);
    log_logger_free(heap_allocator(), quiet);
}

/* -------------------------------------------------------- ModifyResponse */

static void hit_mod_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)http_header_add(hdr(w), cs("X-Hit-Mod"),
                          str_eq(r->url->path, cs("/mod")) ? cs("true") : cs("false"));
}

static Error hit_mod_check(void *env, HttpResponse *resp) {
    (void)env;
    if (!str_eq(http_header_get(resp->header, cs("X-Hit-Mod")), cs("true")))
        return fmt_errorf_v("tried to by-pass proxy");
    return BURROW_NO_ERROR;
}

/* Issue 14237. An error from ModifyResponse makes a StatusBadGateway, and no
 * error a StatusOK. */
static void TestReverseProxyModifyResponse(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, hit_mod_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *rp_url = url_parse(heap_allocator(), backend->url, &err);
    if (rp_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), rp_url);
    LogLogger *quiet = quiet_log();
    p->error_log = quiet;
    p->modify_response = BURROW_FN(HttputilModifyResponseFunc, hit_mod_check, NULL);
    HttptestServer *frontend = serve_proxy(p);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);

    static const struct {
        const char *path;
        Int want_code;
    } tests[] = {
        {"/mod", HTTP_STATUS_OK},
        {"/schedule", HTTP_STATUS_BAD_GATEWAY},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str u = fmt_sprintf_v(arena_allocator(&ar), "%s%s", frontend->url,
                              cs(tests[i].path));
        HttpResponse *resp = http_get(u, &err);
        if (resp == NULL) {
            testing_t_errorf_v(t, "failed to reach proxy: %v", err);
            break;
        }
        if (resp->status_code != tests[i].want_code)
            testing_t_errorf_v(t, "#%d: got res.StatusCode %d; expected %d", (Int)i,
                               resp->status_code, tests[i].want_code);
        http_response_free(resp);
    }
    arena_free(&ar);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    log_logger_free(heap_allocator(), quiet);
    url_free(heap_allocator(), rp_url);
    httptest_server_free(backend);
}

/* failingRoundTripper and staticResponseRoundTripper, which gives a new
 * response each time, since the proxy frees the one it gets. */
static HttpResponse *failing_round_trip(void *self, HttpRequest *req, Error *err) {
    (void)self;
    (void)req;
    *err = err_some;
    return NULL;
}

static const HttpRoundTripperVT failing_rt_vt = {NULL, failing_round_trip};

static HttpResponse *static_round_trip(void *self, HttpRequest *req, Error *err) {
    (void)self;
    HttpResponse *r = new_response(345);
    if (r == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    r->body = http_no_body;
    r->request = req;
    *err = BURROW_NO_ERROR;
    return r;
}

static const HttpRoundTripperVT static_rt_vt = {NULL, static_round_trip};

static void teapot_handler(void *env, HttpResponseWriter rw, HttpRequest *req,
                           Error err) {
    (void)env;
    (void)req;
    (void)err;
    http_response_writer_write_header(rw, HTTP_STATUS_TEAPOT);
}

BURROW_SENTINEL_ERROR(err_trigger_handler, "some error to trigger errorHandler");

static Error modify_inc(void *env, HttpResponse *res) {
    res->status_code++;
    return env != NULL ? err_trigger_handler : BURROW_NO_ERROR;
}

typedef struct ErrorHandlerCase {
    const char *name;
    Int want_code;
    bool error_handler;
    bool static_transport; /* failingRoundTripper when false */
    int modify;            /* 0 for none, 1 for no error, 2 for an error */
} ErrorHandlerCase;

static const ErrorHandlerCase error_handler_cases[] = {
    {"default", HTTP_STATUS_BAD_GATEWAY, false, false, 0},
    {"errorhandler", HTTP_STATUS_TEAPOT, true, false, 0},
    {"modifyresponse_noerr", 346, true, true, 1},
    {"modifyresponse_err", HTTP_STATUS_TEAPOT, true, true, 2},
};

static void error_handler_case(void *env, TestingT *t) {
    const ErrorHandlerCase *tt = (const ErrorHandlerCase *)env;
    Error err = BURROW_NO_ERROR;
    Url *target = url_parse(heap_allocator(), cs("http://dummy.tld/"), &err);
    if (target == NULL)
        FATALF("%v", err);
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), target);
    p->transport = tt->static_transport ? (HttpRoundTripper){&static_rt_vt, NULL}
                                        : (HttpRoundTripper){&failing_rt_vt, NULL};
    if (tt->modify != 0)
        p->modify_response = BURROW_FN(HttputilModifyResponseFunc, modify_inc,
                                       tt->modify == 2 ? (void *)p : NULL);
    LogLogger *quiet = quiet_log();
    p->error_log = quiet;
    if (tt->error_handler)
        p->error_handler = BURROW_FN(HttputilErrorHandlerFunc, teapot_handler, NULL);
    HttptestServer *frontend = serve_proxy(p);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    HttpResponse *resp =
        http_get(fmt_sprintf_v(arena_allocator(&ar), "%s/test", frontend->url), &err);
    if (resp == NULL)
        testing_t_errorf_v(t, "failed to reach proxy: %v", err);
    else if (resp->status_code != tt->want_code)
        testing_t_errorf_v(t, "got res.StatusCode %d; expected %d", resp->status_code,
                           tt->want_code);
    http_response_free(resp);
    arena_free(&ar);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    log_logger_free(heap_allocator(), quiet);
    url_free(heap_allocator(), target);
}

static void TestReverseProxyErrorHandler(TestingT *t) {
    need_tcp(t);
    for (size_t i = 0; i < sizeof error_handler_cases / sizeof error_handler_cases[0];
         i++)
        (void)testing_t_run(t, cs(error_handler_cases[i].name),
                            BURROW_FN(TestingTFunc, error_handler_case,
                                      (void *)(uintptr_t)&error_handler_cases[i]));
}

/* ----------------------------------------------- a body that ends too soon */

/* A backend whose Content-Length is twice the body it sends. */
static void short_body_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    /* Twice the length of the line, without its newline. */
    (void)http_header_set(hdr(w), cs("Content-Length"), cs("84"));
    write_str(w, cs("this call was relayed by the reverse proxy\n"));
}

typedef struct CopyEnv {
    HttputilReverseProxy *p;
    Chan *donec;
    bool panicked;
    Any recovered;
} CopyEnv;

static void copy_frontend(void *env, HttpResponseWriter w, HttpRequest *r) {
    CopyEnv *e = (CopyEnv *)env;
    BURROW_TRY {
        httputil_reverse_proxy_serve_http(e->p, w, r);
    }
    BURROW_CATCH(p) {
        e->panicked = true;
        e->recovered = p;
    }
    BURROW_TRY_END;
    bool done = true;
    chan_send(e->donec, &done);
    /* Go's deferred send lets the panic go on, which the server recovers
     * from. */
    if (e->panicked)
        panic(e->recovered);
}

static void TestReverseProxy_CopyBuffer(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, short_body_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *rp_url = url_parse(heap_allocator(), backend->url, &err);
    if (rp_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    BytesBuffer proxy_log = BYTES_BUFFER(heap_allocator());
    LogLogger *l = log_new(heap_allocator(), bytes_buffer_as_io_writer(&proxy_log),
                           BURROW_STR_EMPTY, LOG_LSHORTFILE);
    CopyEnv e;
    memset(&e, 0, sizeof e);
    e.p = httputil_new_single_host_reverse_proxy(heap_allocator(), rp_url);
    e.p->error_log = l;
    e.donec = chan_make(heap_allocator(), TYPE_BOOL, 1);
    HttpHandlerFunc ff = BURROW_FN(HttpHandlerFunc, copy_frontend, &e);
    HttptestServer *frontend = serve(&ff);

    HttpResponse *res =
        http_client_get(httptest_server_client(frontend), frontend->url, &err);
    if (res != NULL || BURROW_OK(err))
        testing_t_errorf_v(t, "want non-nil error");
    http_response_free(res);
    (void)chan_recv(e.donec, NULL);

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Str got = bytes_buffer_string(&proxy_log, arena_allocator(&ar));
    const char *expected[] = {"EOF", "read"};
    for (size_t i = 0; i < sizeof expected / sizeof expected[0]; i++)
        if (!strings_contains(got, cs(expected[i])))
            testing_t_errorf_v(t, "expected log to contain phrase %q", cs(expected[i]));
    arena_free(&ar);
    httptest_server_free(frontend);
    chan_free(e.donec);
    httputil_reverse_proxy_free(e.p);
    log_logger_free(heap_allocator(), l);
    bytes_buffer_free(&proxy_log);
    url_free(heap_allocator(), rp_url);
    httptest_server_free(backend);
}

/* ------------------------------------------------- the caller's request */

typedef struct DeepCopyEnv {
    HttputilReverseProxy *p;
    Alloc *a;
    Str before, after;
    Chan *resultc;
} DeepCopyEnv;

static void deep_copy_frontend(void *env, HttpResponseWriter w, HttpRequest *r) {
    DeepCopyEnv *e = (DeepCopyEnv *)env;
    e->before = url_string(r->url, e->a);
    httputil_reverse_proxy_serve_http(e->p, w, r);
    e->after = url_string(r->url, e->a);
    bool done = true;
    chan_send(e->resultc, &done);
}

static void hello_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    write_str(w, cs("Hello Gopher!"));
}

static void TestServeHTTPDeepCopy(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, hello_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    DeepCopyEnv e;
    memset(&e, 0, sizeof e);
    e.p = httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    e.a = arena_allocator(&ar);
    e.resultc = chan_make(heap_allocator(), TYPE_BOOL, 1);
    HttpHandlerFunc ff = BURROW_FN(HttpHandlerFunc, deep_copy_frontend, &e);
    HttptestServer *frontend = serve(&ff);

    HttpResponse *res =
        http_client_get(httptest_server_client(frontend), frontend->url, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "Do: %v", err);
    } else {
        http_response_free(res);
        (void)chan_recv(e.resultc, NULL);
        if (!str_eq(e.before, cs("/")) || !str_eq(e.after, cs("/")))
            testing_t_errorf_v(t,
                               "got = {before:%s after:%s}; want = {before:/ after:/}",
                               e.before, e.after);
    }
    httptest_server_free(frontend);
    chan_free(e.resultc);
    httputil_reverse_proxy_free(e.p);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
    arena_free(&ar);
}

static void from_director(void *env, HttpRequest *req) {
    (void)env;
    (void)http_header_set(req->header, cs("From-Director"), cs("1"));
}

static HttpResponse *from_director_round_trip(void *self, HttpRequest *req,
                                              Error *err) {
    TestingT *t = (TestingT *)self;
    Str v = http_header_get(req->header, cs("From-Director"));
    if (!str_eq(v, cs("1")))
        testing_t_errorf_v(t, "From-Directory value = %q; want 1", v);
    *err = io_eof;
    return NULL;
}

static const HttpRoundTripperVT from_director_rt_vt = {NULL, from_director_round_trip};

static void TestClonesRequestHeaders(TestingT *t) {
    /* Go sends the standard logger to io.Discard for this; the proxy's own
     * logger does the same here without changing a global. */
    LogLogger *quiet = quiet_log();
    HttpRequest *req = new_get(t, cs("http://foo.tld/"));
    if (req == NULL) {
        log_logger_free(heap_allocator(), quiet);
        return;
    }
    req->remote_addr = cs("1.2.3.4:56789");
    HttputilReverseProxy rp;
    memset(&rp, 0, sizeof rp);
    rp.director = BURROW_FN(HttputilDirectorFunc, from_director, NULL);
    rp.transport = (HttpRoundTripper){&from_director_rt_vt, t};
    rp.error_log = quiet;
    HttptestResponseRecorder *rec = httptest_new_recorder(heap_allocator());
    if (rec != NULL)
        httputil_reverse_proxy_serve_http(
            &rp, httptest_response_recorder_as_response_writer(rec), req);

    const char *names[] = {"From-Director", "X-Forwarded-For"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (http_header_get(req->header, cs(names[i])).len != 0)
            testing_t_errorf_v(t, "%s header mutation modified caller's request",
                               cs(names[i]));
    httptest_response_recorder_free(rec);
    http_request_free(req);
    log_logger_free(heap_allocator(), quiet);
}

/* checkCloser, a body that never ends and remembers being closed. */
typedef struct CheckCloser {
    bool closed;
} CheckCloser;

static Int cc_read(void *self, Slice p, Error *err) {
    (void)self;
    *err = BURROW_NO_ERROR;
    return p.len;
}

static Error cc_close(void *self) {
    ((CheckCloser *)self)->closed = true;
    return BURROW_NO_ERROR;
}

static const IoReadCloserVT check_closer_vt = {{NULL, cc_read}, {NULL, cc_close}};

static HttpResponse *check_closer_round_trip(void *self, HttpRequest *req, Error *err) {
    HttpResponse *r = new_response(200);
    if (r == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    r->body = (IoReadCloser){&check_closer_vt, self};
    r->request = req;
    *err = BURROW_NO_ERROR;
    return r;
}

static const HttpRoundTripperVT check_closer_rt_vt = {NULL, check_closer_round_trip};

BURROW_SENTINEL_ERROR(err_modify_response, "ModifyResponse error");

static Error modify_fails(void *env, HttpResponse *res) {
    (void)env;
    (void)res;
    return err_modify_response;
}

static void TestModifyResponseClosesBody(TestingT *t) {
    HttpRequest *req = new_get(t, cs("http://foo.tld/"));
    if (req == NULL)
        return;
    req->remote_addr = cs("1.2.3.4:56789");
    CheckCloser close_check = {false};
    BytesBuffer log_buf = BYTES_BUFFER(heap_allocator());
    LogLogger *l = log_new(heap_allocator(), bytes_buffer_as_io_writer(&log_buf),
                           BURROW_STR_EMPTY, 0);
    HttputilReverseProxy rp;
    memset(&rp, 0, sizeof rp);
    rp.director = BURROW_FN(HttputilDirectorFunc, noop_director, NULL);
    rp.transport = (HttpRoundTripper){&check_closer_rt_vt, &close_check};
    rp.error_log = l;
    rp.modify_response = BURROW_FN(HttputilModifyResponseFunc, modify_fails, NULL);
    HttptestResponseRecorder *rec = httptest_new_recorder(heap_allocator());
    if (rec == NULL) {
        testing_t_errorf_v(t, "httptest_new_recorder");
    } else {
        httputil_reverse_proxy_serve_http(
            &rp, httptest_response_recorder_as_response_writer(rec), req);
        HttpResponse *res = httptest_response_recorder_result(rec);
        if (res->status_code != HTTP_STATUS_BAD_GATEWAY)
            testing_t_errorf_v(t, "got res.StatusCode %d; expected %d",
                               res->status_code, HTTP_STATUS_BAD_GATEWAY);
        if (!close_check.closed)
            testing_t_errorf_v(t, "body should have been closed");
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Str g = bytes_buffer_string(&log_buf, arena_allocator(&ar));
        if (!strings_contains(g, error_text(err_modify_response)))
            testing_t_errorf_v(t, "ErrorLog %q does not contain %q", g,
                               error_text(err_modify_response));
        arena_free(&ar);
    }
    httptest_response_recorder_free(rec);
    log_logger_free(heap_allocator(), l);
    bytes_buffer_free(&log_buf);
    http_request_free(req);
}

/* ---------------------------------------------------------------- panics */

static void TestReverseProxy_PanicBodyError(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, short_body_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *rp_url = url_parse(heap_allocator(), backend->url, &err);
    if (rp_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    HttputilReverseProxy *rproxy =
        httputil_new_single_host_reverse_proxy(heap_allocator(), rp_url);
    LogLogger *quiet = quiet_log();
    rproxy->error_log = quiet;
    /* Go's proxy always panics in its own tests. Here it panics for a request
     * from an HttpServer, which its context says this one is. */
    HttpServer srv;
    memset(&srv, 0, sizeof srv);
    Context ctx =
        context_with_value(heap_allocator(), context_background(),
                           http_server_context_key, BURROW_ANY(TYPE_HTTP_SERVER, &srv));
    HttpRequest *req = http_new_request_with_context(
        heap_allocator(), ctx, cs("GET"), cs("http://foo.tld/"), no_body(), &err);
    HttptestResponseRecorder *rec = httptest_new_recorder(heap_allocator());
    bool panicked = false;
    Any rec_val = {NULL, NULL};
    if (req != NULL && rec != NULL) {
        BURROW_TRY {
            httputil_reverse_proxy_serve_http(
                rproxy, httptest_response_recorder_as_response_writer(rec), req);
        }
        BURROW_CATCH(p) {
            panicked = true;
            rec_val = p;
        }
        BURROW_TRY_END;
        if (!panicked)
            testing_t_errorf_v(t, "handler should have panicked");
        else if (rec_val.t != TYPE_ERROR ||
                 !errors_is(*(const Error *)rec_val.data, http_err_abort_handler))
            testing_t_errorf_v(t, "expected ErrAbortHandler, got %v", rec_val);
    }
    httptest_response_recorder_free(rec);
    http_request_free(req);
    context_release(ctx);
    httputil_reverse_proxy_free(rproxy);
    log_logger_free(heap_allocator(), quiet);
    url_free(heap_allocator(), rp_url);
    httptest_server_free(backend);
}

/* neverEnding('x'). */
static Int never_ending_read(void *self, Slice p, Error *err) {
    (void)self;
    memset(p.p, 'x', (size_t)p.len);
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoReaderVT never_ending_vt = {NULL, never_ending_read};

typedef struct PostLoopEnv {
    Str url;
    HttpTransport *tr;
} PostLoopEnv;

#define PANIC_REQ_LEN ((int64_t)6 * 1024 * 1024)

static void post_loop(void *env) {
    PostLoopEnv *e = (PostLoopEnv *)env;
    for (int j = 0; j < 10; j++) {
        IoLimitedReader lr =
            io_limit_reader((IoReader){&never_ending_vt, NULL}, PANIC_REQ_LEN);
        Error err = BURROW_NO_ERROR;
        HttpRequest *req = http_new_request(heap_allocator(), cs("POST"), e->url,
                                            io_limited_reader_as_io_reader(&lr), &err);
        if (req == NULL)
            continue;
        req->content_length = PANIC_REQ_LEN;
        HttpResponse *resp = http_round_tripper_round_trip(
            http_transport_as_round_tripper(e->tr), req, &err);
        if (resp != NULL) {
            (void)io_copy(heap_allocator(), io_discard, body_reader(resp->body), NULL);
            (void)resp->body.vt->closer.close(resp->body.data);
        }
        /* After the response, which waits for the transport to be done with
         * the request's body. */
        http_response_free(resp);
        http_request_free(req);
    }
}

static void TestReverseProxy_PanicClosesIncomingBody(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, short_body_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    LogLogger *quiet = quiet_log();
    p->error_log = quiet;
    HttptestServer *frontend = serve_proxy(p);
    PostLoopEnv e = {frontend->url, &frontend->transport};
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    for (int i = 0; i < 2; i++)
        if (!sync_wait_group_go(&wg, BURROW_FN(Func, post_loop, &e)))
            testing_t_errorf_v(t, "go failed");
    sync_wait_group_wait(&wg);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    log_logger_free(heap_allocator(), quiet);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
}

/* ------------------------------------------------------- flushInterval */

static void TestSelectFlushInterval(TestingT *t) {
    static const struct {
        const char *name;
        const char *content_type; /* or NULL */
        int64_t content_length;
        Duration flush_interval;
        Duration want;
    } tests[] = {
        {"default", NULL, 0, 123, 123},
        {"server-sent events overrides non-zero", "text/event-stream", 0, 123, -1},
        {"server-sent events overrides zero", "text/event-stream", 0, 0, -1},
        {"server-sent events with media-type parameters overrides non-zero",
         "text/event-stream;charset=utf-8", 0, 123, -1},
        {"server-sent events with media-type parameters overrides zero",
         "text/event-stream;charset=utf-8", 0, 0, -1},
        {"Content-Length: -1, overrides non-zero", NULL, -1, 123, -1},
        {"Content-Length: -1, overrides zero", NULL, -1, 0, -1},
    };
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttputilReverseProxy p;
        memset(&p, 0, sizeof p);
        p.flush_interval = tests[i].flush_interval;
        HttpResponse res;
        memset(&res, 0, sizeof res);
        res.content_length = tests[i].content_length;
        if (tests[i].content_type != NULL) {
            res.header = http_header_make(arena_allocator(&ar));
            (void)http_header_set(res.header, cs("Content-Type"),
                                  cs(tests[i].content_type));
        }
        Duration got = burrow__httputil_flush_interval(&p, &res);
        if (got != tests[i].want)
            testing_t_errorf_v(t, "%s: flushLatency = %v; want %v", cs(tests[i].name),
                               got, tests[i].want);
    }
    arena_free(&ar);
}

/* ------------------------------------------------------------ websockets */

static const Str upgrade_msg = BURROW_S_INIT("HTTP/1.1 101 Switching Protocols\r\n"
                                             "Connection: upgrade\r\n"
                                             "Upgrade: WebSocket\r\n\r\n");

static void ws_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    TestingT *t = (TestingT *)env;
    if (!str_eq(upgrade_type(r->header), cs("websocket"))) {
        testing_t_errorf_v(t, "unexpected backend request");
        http_error(w, cs("unexpected request"), 400);
        return;
    }
    Hijacked h;
    Error err = BURROW_NO_ERROR;
    if (!hijack(w, &h, &err)) {
        testing_t_errorf_v(t, "%v", err);
        free_hijacked(&h);
        return;
    }
    IoWriter c = net_conn_as_io_writer(h.conn);
    (void)io_write_string(c, upgrade_msg, NULL);
    BufioScanner *bs =
        bufio_new_scanner(heap_allocator(), net_conn_as_io_reader(h.conn));
    if (bs == NULL || !bufio_scanner_scan(bs)) {
        testing_t_errorf_v(t, "backend failed to read line from client: %v",
                           bs != NULL ? bufio_scanner_err(bs)
                                      : burrow_err_out_of_memory);
    } else {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        (void)io_write_string(c,
                              fmt_sprintf_v(arena_allocator(&ar), "backend got %q\n",
                                            bufio_scanner_text(bs)),
                              NULL);
        arena_free(&ar);
    }
    bufio_scanner_free(bs);
    free_hijacked(&h);
}

static Error add_modified(void *env, HttpResponse *res) {
    (void)env;
    (void)http_header_add(res->header, cs("X-Modified"), cs("true"));
    return BURROW_NO_ERROR;
}

static void ws_frontend(void *env, HttpResponseWriter rw, HttpRequest *req) {
    ProxyEnv *e = (ProxyEnv *)env;
    (void)http_header_set(hdr(rw), cs("X-Header"), cs("X-Value"));
    httputil_reverse_proxy_serve_http(e->p, rw, req);
    Str got = http_header_get(hdr(rw), cs("X-Modified"));
    if (!str_eq(got, cs("true")))
        testing_t_errorf_v(e->t, "response writer X-Modified header = %q; want %q", got,
                           cs("true"));
}

/* An upgrade request to url. */
static HttpRequest *new_upgrade(TestingT *t, Str url) {
    HttpRequest *req = new_get(t, url);
    if (req != NULL) {
        (void)http_header_set(req->header, cs("Connection"), cs("Upgrade"));
        (void)http_header_set(req->header, cs("Upgrade"), cs("websocket"));
    }
    return req;
}

/* Checks the 101 response the frontends give, and gives the body's writer. */
static bool check_upgrade(TestingT *t, const HttpResponse *res, IoWriter *rwc) {
    if (res->status_code != 101) {
        testing_t_errorf_v(t, "status = %v; want 101", res->status);
        return false;
    }
    Str got = http_header_get(res->header, cs("X-Header"));
    if (!str_eq(got, cs("X-Value")))
        testing_t_errorf_v(t, "Header(XHeader) = %q; want %q", got, cs("X-Value"));
    if (!strings_equal_fold(upgrade_type(res->header), cs("websocket"))) {
        testing_t_errorf_v(t, "not websocket upgrade; got %v", res->header);
        return false;
    }
    if (!http_response_body_writer(res, rwc)) {
        testing_t_errorf_v(t, "response body does not implement ReadWriteCloser");
        return false;
    }
    got = http_header_get(res->header, cs("X-Modified"));
    if (!str_eq(got, cs("true")))
        testing_t_errorf_v(t, "response X-Modified header = %q; want %q", got,
                           cs("true"));
    return true;
}

static void TestReverseProxyWebSocket(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, ws_backend, t);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *back_url = url_parse(heap_allocator(), backend->url, &err);
    if (back_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    ProxyEnv e = {t,
                  httputil_new_single_host_reverse_proxy(heap_allocator(), back_url)};
    LogLogger *quiet = quiet_log();
    e.p->error_log = quiet;
    e.p->modify_response = BURROW_FN(HttputilModifyResponseFunc, add_modified, NULL);
    HttpHandlerFunc ff = BURROW_FN(HttpHandlerFunc, ws_frontend, &e);
    HttptestServer *frontend = serve(&ff);

    HttpRequest *req = new_upgrade(t, frontend->url);
    HttpResponse *res =
        req != NULL ? http_client_do(httptest_server_client(frontend), req, &err)
                    : NULL;
    IoWriter rwc;
    if (res == NULL) {
        testing_t_errorf_v(t, "%v", err);
    } else if (check_upgrade(t, res, &rwc)) {
        (void)io_write_string(rwc, cs("Hello\n"), NULL);
        BufioScanner *bs = bufio_new_scanner(heap_allocator(), body_reader(res->body));
        if (bs == NULL || !bufio_scanner_scan(bs)) {
            testing_t_errorf_v(t, "Scan: %v",
                               bs != NULL ? bufio_scanner_err(bs)
                                          : burrow_err_out_of_memory);
        } else {
            Str got = bufio_scanner_text(bs);
            if (!str_eq(got, cs("backend got \"Hello\"")))
                testing_t_errorf_v(t, "got %#q, want %#q", got,
                                   cs("backend got \"Hello\""));
        }
        bufio_scanner_free(bs);
    }
    if (res != NULL)
        (void)res->body.vt->closer.close(res->body.data);
    http_response_free(res);
    http_request_free(req);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(e.p);
    log_logger_free(heap_allocator(), quiet);
    url_free(heap_allocator(), back_url);
    httptest_server_free(backend);
}

#define WS_N 5

typedef struct WSCancelEnv {
    TestingT *t;
    Chan *trigger_cancel;
    HttputilReverseProxy *p;
} WSCancelEnv;

static Str nth_response(Alloc *a, Int i) {
    return fmt_sprintf_v(a, "backend response #%d\n", i);
}

#define TERMINAL_MSG "final message"

/* Whether the client has asked for the cancellation, which is what makes a
 * failed write expected. */
static bool cancel_triggered(Chan *c) {
    bool ok = false;
    return chan_try_recv(c, NULL, &ok);
}

static void ws_cancel_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    WSCancelEnv *e = (WSCancelEnv *)env;
    TestingT *t = e->t;
    Str g = upgrade_type(r->header);
    if (!str_eq(g, cs("websocket"))) {
        testing_t_errorf_v(t, "Unexpected upgrade type %q, want %q", g,
                           cs("websocket"));
        http_error(w, cs("Unexpected request"), 400);
        return;
    }
    Hijacked h;
    Error err = BURROW_NO_ERROR;
    if (!hijack(w, &h, &err)) {
        testing_t_errorf_v(t, "%v", err);
        free_hijacked(&h);
        return;
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    (void)io_write_string(net_conn_as_io_writer(h.conn), upgrade_msg, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
        goto done;
    }
    bool is_prefix = false;
    (void)bufio_reader_read_line(h.brw.reader, &is_prefix, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Failed to read line from client: %v", err);
        goto done;
    }
    for (Int i = 0; i < WS_N; i++) {
        (void)bufio_writer_write_string(h.brw.writer,
                                        nth_response(arena_allocator(&ar), i), &err);
        if (BURROW_FAILED(err)) {
            if (!cancel_triggered(e->trigger_cancel))
                testing_t_errorf_v(t, "Writing response #%d failed: %v", i, err);
            goto done;
        }
        (void)bufio_writer_flush(h.brw.writer);
        time_sleep(TIME_SECOND);
    }
    (void)bufio_writer_write_string(h.brw.writer, cs(TERMINAL_MSG), &err);
    if (BURROW_FAILED(err) && !cancel_triggered(e->trigger_cancel))
        testing_t_errorf_v(t, "Failed to write terminal message: %v", err);
    (void)bufio_writer_flush(h.brw.writer);
done:
    arena_free(&ar);
    free_hijacked(&h);
}

typedef struct CancelOnTrigger {
    Chan *trigger;
    Chan *done;
    ContextCancelFunc cancel;
} CancelOnTrigger;

static void cancel_on_trigger(void *env) {
    CancelOnTrigger *c = (CancelOnTrigger *)env;
    SelectCase cases[2] = {BURROW_RECV(c->trigger, NULL), BURROW_RECV(c->done, NULL)};
    if (chan_select(cases, 2) == 0)
        BURROW_CALLF0(c->cancel);
}

static void ws_cancel_frontend(void *env, HttpResponseWriter rw, HttpRequest *req) {
    WSCancelEnv *e = (WSCancelEnv *)env;
    (void)http_header_set(hdr(rw), cs("X-Header"), cs("X-Value"));
    CancelOnTrigger c;
    memset(&c, 0, sizeof c);
    c.trigger = e->trigger_cancel;
    Context ctx =
        context_with_cancel(heap_allocator(), http_request_context(req), &c.cancel);
    c.done = chan_make(heap_allocator(), TYPE_BOOL, 0);
    HttpRequest *r2 =
        ctx.vt != NULL ? http_request_with_context(req, heap_allocator(), ctx) : NULL;
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    if (c.done == NULL || r2 == NULL ||
        !sync_wait_group_go(&wg, BURROW_FN(Func, cancel_on_trigger, &c))) {
        testing_t_errorf_v(e->t, "out of memory");
    } else {
        httputil_reverse_proxy_serve_http(e->p, rw, r2);
    }
    if (c.done != NULL)
        chan_close(c.done);
    sync_wait_group_wait(&wg);
    http_request_free(r2);
    if (ctx.vt != NULL) {
        BURROW_CALLF0(c.cancel);
        context_release(ctx);
    }
    chan_free(c.done);
}

static void TestReverseProxyWebSocketCancellation(TestingT *t) {
    need_tcp(t);
    WSCancelEnv e;
    memset(&e, 0, sizeof e);
    e.t = t;
    e.trigger_cancel = chan_make(heap_allocator(), TYPE_BOOL, WS_N);
    if (e.trigger_cancel == NULL)
        FATALF("chan_make");
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, ws_cancel_backend, &e);
    HttptestServer *cst = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), cst->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(cst);
        chan_free(e.trigger_cancel);
        FATALF("%v", err);
    }
    e.p = httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    LogLogger *quiet = quiet_log();
    e.p->error_log = quiet;
    e.p->modify_response = BURROW_FN(HttputilModifyResponseFunc, add_modified, NULL);
    HttpHandlerFunc ff = BURROW_FN(HttpHandlerFunc, ws_cancel_frontend, &e);
    HttptestServer *frontend = serve(&ff);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    bool triggered = false;

    HttpRequest *req = new_upgrade(t, frontend->url);
    HttpResponse *res =
        req != NULL ? http_client_do(httptest_server_client(frontend), req, &err)
                    : NULL;
    IoWriter rwc;
    if (res == NULL) {
        testing_t_errorf_v(t, "Dialing to frontend proxy: %v", err);
    } else if (check_upgrade(t, res, &rwc)) {
        (void)io_write_string(rwc, cs("Hello\n"), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Failed to write first message: %v", err);
            goto done;
        }
        /* The cancellation is asked for once the first response is back. */
        BufioReader *br = bufio_new_reader(a, body_reader(res->body));
        if (br == NULL)
            goto done;
        Str first = nth_response(a, 0);
        for (;;) {
            Str line = bufio_reader_read_string(br, a, '\n', &err);
            if (str_eq(line, cs(TERMINAL_MSG))) { /* before err == io_eof */
                testing_t_errorf_v(t, "The websocket request was not canceled, "
                                      "unfortunately!");
                break;
            }
            if (errors_is(err, io_eof))
                break;
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "Unexpected error: %v", err);
                break;
            }
            if (str_eq(line, first) && !triggered) {
                chan_close(e.trigger_cancel);
                triggered = true;
            }
        }
    }
done:
    if (!triggered)
        chan_close(e.trigger_cancel);
    if (res != NULL)
        (void)res->body.vt->closer.close(res->body.data);
    http_response_free(res);
    http_request_free(req);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(e.p);
    log_logger_free(heap_allocator(), quiet);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(cst);
    chan_free(e.trigger_cancel);
    arena_free(&ar);
}

/* The two halves of an upgraded TCP connection, closed one at a time. */
typedef struct HalfEnv {
    TestingT *t;
    Int which;
    SyncMutex mu;
    Hijacked srv; /* the backend's end */
} HalfEnv;

static void half_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    HalfEnv *e = (HalfEnv *)env;
    TestingT *t = e->t;
    Str g = upgrade_type(r->header);
    if (!str_eq(g, cs("websocket"))) {
        testing_t_errorf_v(t, "Unexpected upgrade type %q, want %q", g,
                           cs("websocket"));
        return;
    }
    Hijacked h;
    Error err = BURROW_NO_ERROR;
    if (!hijack(w, &h, &err)) {
        free_hijacked(&h);
        testing_t_errorf_v(t, "hijack failed: %v", err);
        return;
    }
    if (net_conn_as_tcp_conn(h.conn) == NULL) {
        free_hijacked(&h);
        testing_t_errorf_v(t, "conn is not a TCPConn");
        return;
    }
    /* Store srv before sending the 101, as Go does. The client looks for it as
     * soon as the 101 arrives. */
    sync_mutex_lock(&e->mu);
    e->srv = h;
    sync_mutex_unlock(&e->mu);
    (void)io_write_string(net_conn_as_io_writer(h.conn), upgrade_msg, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "backend upgrade failed: %v", err);
}

static void must_read(TestingT *t, NetTCPConn *conn, const char *msg) {
    Byte b[32];
    Int n = (Int)strlen(msg);
    Error err = BURROW_NO_ERROR;
    (void)net_tcp_conn_read(conn, slice_from(b, n, n, TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "failed to read: %v", err);
    Str got = str_from_bytes(b, n);
    if (!str_eq(got, cs(msg)))
        testing_t_errorf_v(t, "got %#q, want %#q", got, cs(msg));
}

static void must_read_error(TestingT *t, NetTCPConn *conn, Error e) {
    Byte b[1];
    Error err = BURROW_NO_ERROR;
    (void)net_tcp_conn_read(conn, slice_from(b, 1, 1, TYPE_BYTE), &err);
    if (!errors_is(err, e))
        testing_t_errorf_v(t, "failed to read error: %v", err);
}

static void must_write(TestingT *t, NetTCPConn *conn, const char *msg) {
    Error err = BURROW_NO_ERROR;
    Int n = (Int)strlen(msg);
    (void)net_tcp_conn_write(conn, slice_from((void *)(uintptr_t)msg, n, n, TYPE_BYTE),
                             &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "failed to write: %v", err);
}

static void must_close_read(TestingT *t, NetTCPConn *conn) {
    Error err = net_tcp_conn_close_read(conn);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "failed to CloseRead: %v", err);
}

static void must_close_write(TestingT *t, NetTCPConn *conn) {
    Error err = net_tcp_conn_close_write(conn);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "failed to CloseWrite: %v", err);
}

static const char *const half_names[] = {
    "server close read",
    "server close write",
    "client close read",
    "client close write",
};

static void half_test(TestingT *t, Int which, NetTCPConn *cli, NetTCPConn *srv) {
    switch (which) {
    case 0:
        must_close_read(t, srv);
        must_write(t, srv, "server sends");
        must_read(t, cli, "server sends");
        break;
    case 1:
        must_close_write(t, srv);
        must_write(t, cli, "client sends");
        must_read(t, srv, "client sends");
        must_read_error(t, cli, io_eof);
        break;
    case 2:
        must_close_read(t, cli);
        must_write(t, cli, "client sends");
        must_read(t, srv, "client sends");
        break;
    default:
        must_close_write(t, cli);
        must_write(t, srv, "server sends");
        must_read(t, cli, "server sends");
        must_read_error(t, srv, io_eof);
        break;
    }
}

static void half_tcp_case(void *env, TestingT *t) {
    HalfEnv e;
    memset(&e, 0, sizeof e);
    e.t = t;
    e.which = (Int)(intptr_t)env;
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, half_backend, &e);
    HttptestServer *backend = serve(&bf);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(a, backend->url, &err);
    HttputilReverseProxy *rproxy =
        backend_url != NULL ? httputil_new_single_host_reverse_proxy(a, backend_url)
                            : NULL;
    if (rproxy == NULL) {
        httptest_server_free(backend);
        arena_free(&ar);
        FATALF("%v", err);
    }
    LogLogger *quiet = quiet_log();
    rproxy->error_log = quiet;
    HttptestServer *frontend = serve_proxy(rproxy);
    NetTCPConn *cli = NULL;
    HttpRequest *req = NULL;
    HttpResponse *resp = NULL;

    Url *frontend_url = url_parse(a, frontend->url, &err);
    NetTCPAddr *addr =
        frontend_url != NULL
            ? net_resolve_tcp_addr(a, cs("tcp"), frontend_url->host, &err)
            : NULL;
    if (addr == NULL) {
        testing_t_errorf_v(t, "failed to resolve TCP address: %v", err);
        goto done;
    }
    cli = net_dial_tcp(heap_allocator(), cs("tcp"), NULL, addr, &err);
    if (cli == NULL) {
        testing_t_errorf_v(t, "failed to dial TCP address: %v", err);
        goto done;
    }
    req = new_upgrade(t, frontend->url);
    if (req == NULL)
        goto done;
    err = http_request_write(req, net_conn_as_io_writer(net_tcp_conn_as_conn(cli)));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "failed to write request: %v", err);
        goto done;
    }
    BufioReader *br =
        bufio_new_reader(a, net_conn_as_io_reader(net_tcp_conn_as_conn(cli)));
    resp = br != NULL ? http_read_response(heap_allocator(), br, NULL, &err) : NULL;
    if (resp == NULL) {
        testing_t_errorf_v(t, "failed to read response: %v", err);
        goto done;
    }
    if (resp->status_code != 101) {
        testing_t_errorf_v(t, "status code not 101: %v", resp->status_code);
        goto done;
    }
    if (!str_eq(strings_to_lower(a, http_header_get(resp->header, cs("Upgrade"))),
                cs("websocket")) ||
        !str_eq(strings_to_lower(a, http_header_get(resp->header, cs("Connection"))),
                cs("upgrade"))) {
        testing_t_errorf_v(t, "frontend upgrade failed");
        goto done;
    }
    sync_mutex_lock(&e.mu);
    NetTCPConn *srv = net_conn_as_tcp_conn(e.srv.conn);
    sync_mutex_unlock(&e.mu);
    if (srv == NULL) {
        testing_t_errorf_v(t, "no backend connection");
        goto done;
    }
    half_test(t, e.which, cli, srv);

done:
    sync_mutex_lock(&e.mu);
    free_hijacked(&e.srv);
    sync_mutex_unlock(&e.mu);
    http_response_free(resp);
    http_request_free(req);
    net_tcp_conn_free(cli);
    httptest_server_free(frontend);
    log_logger_free(heap_allocator(), quiet);
    httputil_reverse_proxy_free(rproxy);
    httptest_server_free(backend);
    arena_free(&ar);
}

static void TestReverseProxyWebSocketHalfTCP(TestingT *t) {
    need_tcp(t);
    for (Int i = 0; i < 4; i++)
        (void)testing_t_run(
            t, cs(half_names[i]),
            BURROW_FN(TestingTFunc, half_tcp_case, (void *)(intptr_t)i));
}

typedef struct NoCloseWriteEnv {
    TestingT *t;
    Chan *backend_done;
} NoCloseWriteEnv;

static void no_close_write_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    NoCloseWriteEnv *e = (NoCloseWriteEnv *)env;
    (void)r;
    (void)http_header_set(hdr(w), cs("Connection"), cs("upgrade"));
    (void)http_header_set(hdr(w), cs("Upgrade"), cs("u"));
    http_response_writer_write_header(w, 101);
    Hijacked h;
    Error err = BURROW_NO_ERROR;
    if (!hijack(w, &h, &err))
        testing_t_errorf_v(e->t, "Hijack: %v", err);
    else
        (void)io_copy(heap_allocator(), io_discard, net_conn_as_io_reader(h.conn),
                      NULL);
    free_hijacked(&h);
    chan_close(e->backend_done);
}

/* type readWriteCloserOnly struct{ io.ReadWriteCloser }, which has no
 * CloseWrite. */
static Error drop_close_write(void *env, HttpResponse *resp) {
    (void)env;
    resp->body_close_write = NULL;
    return BURROW_NO_ERROR;
}

static void TestReverseProxyUpgradeNoCloseWrite(TestingT *t) {
    need_tcp(t);
    NoCloseWriteEnv e = {t, chan_make(heap_allocator(), TYPE_BOOL, 0)};
    if (e.backend_done == NULL)
        FATALF("chan_make");
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, no_close_write_backend, &e);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        chan_free(e.backend_done);
        FATALF("%v", err);
    }
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    p->modify_response = BURROW_FN(HttputilModifyResponseFunc, drop_close_write, NULL);
    HttptestServer *frontend = serve_proxy(p);

    HttpRequest *req = new_get(t, frontend->url);
    HttpResponse *resp = NULL;
    if (req != NULL) {
        (void)http_header_set(req->header, cs("Connection"), cs("upgrade"));
        (void)http_header_set(req->header, cs("Upgrade"), cs("u"));
        resp = http_client_do(httptest_server_client(frontend), req, &err);
    }
    if (resp == NULL) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        (void)resp->body.vt->closer.close(resp->body.data);
        (void)chan_recv(e.backend_done, NULL);
    }
    http_response_free(resp);
    http_request_free(req);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
    chan_free(e.backend_done);
}

/* -------------------------------------------------------------- trailers */

static void unannounced_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    http_response_writer_write_header(w, HTTP_STATUS_OK);
    HttpResponseController rc = http_new_response_controller(w);
    (void)http_response_controller_flush(&rc);
    (void)http_header_set(hdr(w), cs("Trailer:X-Unannounced-Trailer"),
                          cs("unannounced_trailer_value"));
}

static void TestUnannouncedTrailer(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, unannounced_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    LogLogger *quiet = quiet_log();
    p->error_log = quiet;
    HttptestServer *frontend = serve_proxy(p);
    HttpResponse *res =
        http_client_get(httptest_server_client(frontend), frontend->url, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "Get: %v", err);
    } else {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        (void)read_all(arena_allocator(&ar), res->body, &err);
        (void)res->body.vt->closer.close(res->body.data);
        Str g = res->trailer != NULL
                    ? http_header_get(res->trailer, cs("X-Unannounced-Trailer"))
                    : BURROW_STR_EMPTY;
        if (!str_eq(g, cs("unannounced_trailer_value")))
            testing_t_errorf_v(t, "Trailer(X-Unannounced-Trailer) = %q; want %q", g,
                               cs("unannounced_trailer_value"));
        arena_free(&ar);
    }
    http_response_free(res);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    log_logger_free(heap_allocator(), quiet);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
}

/* ---------------------------------------------------------- Rewrite, SetURL */

static void host_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    write_str(w, r->host);
}

static void TestSetURL(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, host_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    HttputilReverseProxy p = set_url_proxy(backend_url);
    HttptestServer *frontend = serve_proxy(&p);
    HttpResponse *res =
        http_client_get(httptest_server_client(frontend), frontend->url, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "Get: %v", err);
    } else {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Str body = read_all(arena_allocator(&ar), res->body, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "Reading body: %v", err);
        else if (!str_eq(body, backend_url->host))
            testing_t_errorf_v(t, "backend got Host %q, want %q", body,
                               backend_url->host);
        arena_free(&ar);
    }
    http_response_free(res);
    httptest_server_free(frontend);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
}

static void TestSingleJoinSlash(TestingT *t) {
    static const struct {
        const char *slasha, *slashb, *expected;
    } tests[] = {
        {"https://www.google.com/", "/favicon.ico",
         "https://www.google.com/favicon.ico"},
        {"https://www.google.com", "/favicon.ico",
         "https://www.google.com/favicon.ico"},
        {"https://www.google.com", "favicon.ico", "https://www.google.com/favicon.ico"},
        {"https://www.google.com", "", "https://www.google.com/"},
        {"", "favicon.ico", "/favicon.ico"},
    };
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str got = burrow__httputil_single_joining_slash(
            arena_allocator(&ar), cs(tests[i].slasha), cs(tests[i].slashb));
        if (!str_eq(got, cs(tests[i].expected)))
            testing_t_errorf_v(t, "singleJoiningSlash(%q,%q) want %q got %q",
                               cs(tests[i].slasha), cs(tests[i].slashb),
                               cs(tests[i].expected), got);
    }
    arena_free(&ar);
}

static void TestJoinURLPath(TestingT *t) {
    static const struct {
        const char *a_path, *a_raw, *b_path, *b_raw;
        const char *want_path, *want_raw;
    } tests[] = {
        {"/a/b", "", "/c", "", "/a/b/c", ""},
        {"/a/b", "badpath", "c", "", "/a/b/c", "/a/b/c"},
        {"/a/b", "/a%2Fb", "/c", "", "/a/b/c", "/a%2Fb/c"},
        {"/a/b", "/a%2Fb", "/c", "", "/a/b/c", "/a%2Fb/c"},
        {"/a/b/", "/a%2Fb%2F", "c", "", "/a/b//c", "/a%2Fb%2F/c"},
        {"/a/b/", "/a%2Fb/", "/c/d", "/c%2Fd", "/a/b/c/d", "/a%2Fb/c%2Fd"},
    };
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Url a, b;
        memset(&a, 0, sizeof a);
        memset(&b, 0, sizeof b);
        a.path = cs(tests[i].a_path);
        a.raw_path = cs(tests[i].a_raw);
        b.path = cs(tests[i].b_path);
        b.raw_path = cs(tests[i].b_raw);
        Str p = BURROW_STR_EMPTY, rp = BURROW_STR_EMPTY;
        burrow__httputil_join_url_path(arena_allocator(&ar), &a, &b, &p, &rp);
        if (!str_eq(p, cs(tests[i].want_path)) || !str_eq(rp, cs(tests[i].want_raw)))
            testing_t_errorf_v(t,
                               "joinURLPath(URL(%q,%q),URL(%q,%q)) want (%q,%q) got "
                               "(%q,%q)",
                               a.path, a.raw_path, b.path, b.raw_path,
                               cs(tests[i].want_path), cs(tests[i].want_raw), p, rp);
    }
    arena_free(&ar);
}

static void replace_out(void *env, HttputilProxyRequest *r) {
    /* The proxy gives back what is in out, so the request it replaces is
     * its to free as well. */
    Error err = BURROW_NO_ERROR;
    HttpRequest *out = http_new_request(heap_allocator(), cs("GET"), *(const Str *)env,
                                        no_body(), &err);
    if (out != NULL)
        r->out = out;
}

static void content_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    write_str(w, cs("response_content"));
}

static void TestReverseProxyRewriteReplacesOut(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, content_backend, NULL);
    HttptestServer *backend = serve(&bf);
    HttputilReverseProxy p;
    memset(&p, 0, sizeof p);
    p.rewrite = BURROW_FN(HttputilRewriteFunc, replace_out, &backend->url);
    HttptestServer *frontend = serve_proxy(&p);
    Error err = BURROW_NO_ERROR;
    HttpResponse *res =
        http_client_get(httptest_server_client(frontend), frontend->url, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "Get: %v", err);
    } else {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Str body = read_all(arena_allocator(&ar), res->body, &err);
        if (!str_eq(body, cs("response_content")))
            testing_t_errorf_v(t, "got response %q, want %q", body,
                               cs("response_content"));
        arena_free(&ar);
    }
    http_response_free(res);
    httptest_server_free(frontend);
    httptest_server_free(backend);
}

/* ------------------------------------------------------------ 1xx responses */

/* testResponseWriter: a header made when first asked for, an optional
 * WriteHeader and Hijack, and writes that go nowhere. */
typedef struct TestRW {
    Arena ar;
    HttpHeader h;
    TestingT *t;
    bool hijacked;
    NetConn cli; /* the client's end of the pipe Hijack gives */
    SyncWaitGroup wg;
} TestRW;

static HttpHeader trw_header(void *self) {
    TestRW *rw = (TestRW *)self;
    if (rw->h == NULL)
        rw->h = http_header_make(arena_allocator(&rw->ar));
    return rw->h;
}

static void trw_write_header(void *self, Int code) {
    TestRW *rw = (TestRW *)self;
    if (rw->hijacked)
        testing_t_errorf_v(rw->t, "WriteHeader(%v) called after Hijack", code);
}

static Int trw_write(void *self, Slice p, Error *err) {
    (void)self;
    *err = BURROW_NO_ERROR;
    return p.len;
}

static void trw_copy_cli(void *env) {
    TestRW *rw = (TestRW *)env;
    (void)io_copy(heap_allocator(), io_discard, net_conn_as_io_reader(rw->cli), NULL);
    net_conn_free(rw->cli);
}

static NetConn trw_hijack(void *self, BufioReadWriter *buf, Error *err) {
    TestRW *rw = (TestRW *)self;
    NetConn none = {NULL, NULL};
    rw->hijacked = true;
    NetConn srv;
    net_pipe(heap_allocator(), &rw->cli, &srv);
    if (srv.vt == NULL) {
        *err = burrow_err_out_of_memory;
        return none;
    }
    buf->reader = bufio_new_reader(heap_allocator(), net_conn_as_io_reader(srv));
    buf->writer = bufio_new_writer(heap_allocator(), net_conn_as_io_writer(srv));
    if (!sync_wait_group_go(&rw->wg, BURROW_FN(Func, trw_copy_cli, rw)))
        net_conn_free(rw->cli);
    *err = BURROW_NO_ERROR;
    return srv;
}

static const HttpResponseWriterVT test_rw_vt = {
    .writer = {NULL, trw_write},
    .header = trw_header,
    .write_header = trw_write_header,
};

static const HttpResponseWriterVT test_rw_hijack_vt = {
    .writer = {NULL, trw_write},
    .header = trw_header,
    .write_header = trw_write_header,
    .hijack = trw_hijack,
};

static void test_rw_init(TestRW *rw, TestingT *t) {
    memset(rw, 0, sizeof *rw);
    arena_init(&rw->ar, heap_allocator(), 0);
    rw->t = t;
}

static void test_rw_free(TestRW *rw) {
    sync_wait_group_wait(&rw->wg);
    arena_free(&rw->ar);
}

static void early_hints_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    for (int i = 0; i < 5; i++)
        http_response_writer_write_header(w, 103);
}

static Error cancel_on_1xx(void *env, Int code, TextprotoMIMEHeader header) {
    (void)code;
    (void)header;
    BURROW_CALLF0(*(ContextCancelFunc *)env);
    return BURROW_NO_ERROR;
}

static void Test1xxHeadersNotModifiedAfterRoundTrip(TestingT *t) {
    need_tcp(t);
    /* https://go.dev/issue/65123: the proxy sets no headers on rw once
     * RoundTrip has returned. */
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, early_hints_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    LogLogger *quiet = quiet_log();
    p->error_log = quiet;

    TestRW rw;
    test_rw_init(&rw, t);
    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(heap_allocator(), context_background(), &cancel);
    HttptraceClientTrace trace;
    memset(&trace, 0, sizeof trace);
    /* Cancel the request, which makes RoundTrip return, as soon as a 1xx
     * response comes. */
    trace.got1xx_response =
        BURROW_FN(HttptraceGot1xxResponseFunc, cancel_on_1xx, &cancel);
    Context tctx = httptrace_with_client_trace(heap_allocator(), ctx, &trace);
    HttpRequest *req = http_new_request_with_context(
        heap_allocator(), tctx, cs("GET"), cs("http://go.dev/"), no_body(), &err);
    if (req != NULL)
        httputil_reverse_proxy_serve_http(p, (HttpResponseWriter){&test_rw_vt, &rw},
                                          req);
    BURROW_CALLF0(cancel);
    /* Set off a data race over the response headers. A thread sanitizer sees the
     * condition in https://go.dev/issue/65123 often enough with this. */
    if (rw.h != NULL) {
        MapIter it = map_iter(rw.h);
        while (map_next(&it, NULL, NULL)) {
        }
    }
    http_request_free(req);
    context_release(tctx);
    context_release(ctx);
    test_rw_free(&rw);
    httputil_reverse_proxy_free(p);
    log_logger_free(heap_allocator(), quiet);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
}

static void links_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    HttpHeader h = hdr(w);
    (void)http_header_add(h, cs("Link"), cs("</style.css>; rel=preload; as=style"));
    (void)http_header_add(h, cs("Link"), cs("</script.js>; rel=preload; as=script"));
    http_response_writer_write_header(w, HTTP_STATUS_EARLY_HINTS);
    (void)http_header_add(h, cs("Link"), cs("</foo.js>; rel=preload; as=script"));
    http_response_writer_write_header(w, HTTP_STATUS_PROCESSING);
    write_str(w, cs("Hello"));
}

static const char *const links_all[] = {
    "</style.css>; rel=preload; as=style",
    "</script.js>; rel=preload; as=script",
    "</foo.js>; rel=preload; as=script",
};

static void check_link_headers(TestingT *t, Int n_expected, HttpHeader h) {
    Slice got = h != NULL ? http_header_values(h, cs("Link")) : (Slice){0};
    if (got.len != n_expected)
        testing_t_errorf_v(t, "Expected %d link headers; got %d", n_expected, got.len);
    for (Int i = 0; i < n_expected; i++) {
        if (i >= got.len) {
            testing_t_errorf_v(t, "Expected %q link header; got nothing",
                               cs(links_all[i]));
            continue;
        }
        Str g = ((const Str *)got.p)[i];
        if (!str_eq(g, cs(links_all[i])))
            testing_t_errorf_v(t, "Expected %q link header; got %q", cs(links_all[i]),
                               g);
    }
}

typedef struct Count1xx {
    TestingT *t;
    Int n;
} Count1xx;

static Error count_1xx(void *env, Int code, TextprotoMIMEHeader header) {
    Count1xx *c = (Count1xx *)env;
    switch (code) {
    case HTTP_STATUS_EARLY_HINTS:
        check_link_headers(c->t, 2, header);
        break;
    case HTTP_STATUS_PROCESSING:
        check_link_headers(c->t, 3, header);
        break;
    default:
        testing_t_errorf_v(c->t, "Unexpected 1xx response");
        break;
    }
    c->n++;
    return BURROW_NO_ERROR;
}

static void Test1xxResponses(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, links_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    HttputilReverseProxy *p =
        httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
    LogLogger *quiet = quiet_log();
    p->error_log = quiet;
    HttptestServer *frontend = serve_proxy(p);

    Count1xx counter = {t, 0};
    HttptraceClientTrace trace;
    memset(&trace, 0, sizeof trace);
    trace.got1xx_response = BURROW_FN(HttptraceGot1xxResponseFunc, count_1xx, &counter);
    Context ctx =
        httptrace_with_client_trace(heap_allocator(), context_background(), &trace);
    HttpRequest *req = http_new_request_with_context(heap_allocator(), ctx, cs("GET"),
                                                     frontend->url, no_body(), &err);
    HttpResponse *res =
        req != NULL ? http_client_do(httptest_server_client(frontend), req, &err)
                    : NULL;
    if (res == NULL) {
        testing_t_errorf_v(t, "Get: %v", err);
    } else {
        if (counter.n != 2)
            testing_t_errorf_v(t, "Expected 2 1xx responses; got %d", counter.n);
        check_link_headers(t, 3, res->header);
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Str body = read_all(arena_allocator(&ar), res->body, &err);
        if (!str_eq(body, cs("Hello")))
            testing_t_errorf_v(t, "Read body %q; want Hello", body);
        arena_free(&ar);
    }
    http_response_free(res);
    http_request_free(req);
    context_release(ctx);
    httptest_server_free(frontend);
    httputil_reverse_proxy_free(p);
    log_logger_free(heap_allocator(), quiet);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
}

/* ------------------------------------------------ query parameter smuggling */

static void raw_query_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    write_str(w, r->url->raw_query);
}

static void form_director(void *env, HttpRequest *r) {
    OldDirector *d = (OldDirector *)env;
    if (d->parse_form)
        (void)http_request_form_value(r, cs("a"));
    BURROW_CALLF(d->old, r);
}

static void set_url_keep_query(void *env, HttputilProxyRequest *r) {
    httputil_proxy_request_set_url(r, (const Url *)env);
    r->out->url->raw_query = r->in->url->raw_query;
}

typedef enum SmuggleProxy {
    SMUGGLE_DIRECTOR,
    SMUGGLE_DIRECTOR_PARSES_FORM,
    SMUGGLE_REWRITE,
    SMUGGLE_REWRITE_KEEPS_QUERY,
} SmuggleProxy;

static void test_query_parameter_smuggling(TestingT *t, bool want_clean_query,
                                           SmuggleProxy kind) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, raw_query_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    HttputilReverseProxy *single = NULL;
    HttputilReverseProxy rw;
    memset(&rw, 0, sizeof rw);
    HttputilReverseProxy *p = &rw;
    OldDirector d = {{NULL, NULL}, false};
    switch (kind) {
    case SMUGGLE_DIRECTOR:
    case SMUGGLE_DIRECTOR_PARSES_FORM:
        single = httputil_new_single_host_reverse_proxy(heap_allocator(), backend_url);
        p = single;
        d.old = single->director;
        d.parse_form = kind == SMUGGLE_DIRECTOR_PARSES_FORM;
        single->director = BURROW_FN(HttputilDirectorFunc, form_director, &d);
        break;
    case SMUGGLE_REWRITE:
        rw.rewrite = BURROW_FN(HttputilRewriteFunc, set_url_rewrite, backend_url);
        break;
    case SMUGGLE_REWRITE_KEEPS_QUERY:
    default:
        rw.rewrite = BURROW_FN(HttputilRewriteFunc, set_url_keep_query, backend_url);
        break;
    }
    HttptestServer *frontend = serve_proxy(p);
    LogLogger *quiet = quiet_log();
    backend->config.error_log = quiet;
    frontend->config.error_log = quiet;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    struct {
        Str raw_query, clean_query;
    } tests[] = {
        {cs("a=1&a=2;b=3"), cs("a=1")},
        {cs("a=1&a=%zz&b=3"), cs("a=1&b=3")},
        {cs("a=%zz"), BURROW_STR_EMPTY},
        {fmt_sprintf_v(a, "%sa=1", strings_repeat(a, cs("a=1&"), 10000)),
         BURROW_STR_EMPTY},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        HttpResponse *res = http_client_get(
            httptest_server_client(frontend),
            fmt_sprintf_v(a, "%s?%s", frontend->url, tests[i].raw_query), &err);
        if (res == NULL) {
            testing_t_errorf_v(t, "Get: %v", err);
            break;
        }
        Str body = read_all(a, res->body, &err);
        Str want = want_clean_query ? tests[i].clean_query : tests[i].raw_query;
        if (!str_eq(body, want))
            testing_t_errorf_v(t, "proxy forwarded raw query %q as %q, want %q",
                               tests[i].raw_query, body, want);
        http_response_free(res);
    }
    arena_free(&ar);
    httptest_server_free(frontend);
    log_logger_free(heap_allocator(), quiet);
    httputil_reverse_proxy_free(single);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
}

static void
TestReverseProxyQueryParameterSmugglingDirectorDoesNotParseForm(TestingT *t) {
    test_query_parameter_smuggling(t, false, SMUGGLE_DIRECTOR);
}

static void TestReverseProxyQueryParameterSmugglingDirectorParsesForm(TestingT *t) {
    test_query_parameter_smuggling(t, true, SMUGGLE_DIRECTOR_PARSES_FORM);
}

static void TestReverseProxyQueryParameterSmugglingRewrite(TestingT *t) {
    test_query_parameter_smuggling(t, true, SMUGGLE_REWRITE);
}

static void
TestReverseProxyQueryParameterSmugglingRewritePreservesRawQuery(TestingT *t) {
    test_query_parameter_smuggling(t, false, SMUGGLE_REWRITE_KEEPS_QUERY);
}

/* ---------------------------------------------------------- hijack errors */

static void switching_backend(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    (void)http_header_set(hdr(w), cs("Upgrade"), cs("someproto"));
    http_response_writer_write_header(w, HTTP_STATUS_SWITCHING_PROTOCOLS);
}

/* testReadWriteCloser with only read set, to fail. Its close closes the body
 * it took the place of, which Go's collector would see to. */
typedef struct FailingRWC {
    IoReadCloser orig;
} FailingRWC;

BURROW_SENTINEL_ERROR(err_read, "read error");

static Int frwc_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    *err = err_read;
    return 0;
}

static Int frwc_write(void *self, Slice p, Error *err) {
    (void)self;
    *err = BURROW_NO_ERROR;
    return p.len;
}

static Error frwc_close(void *self) {
    FailingRWC *f = (FailingRWC *)self;
    if (f->orig.vt != NULL)
        (void)f->orig.vt->closer.close(f->orig.data);
    f->orig = (IoReadCloser){NULL, NULL};
    return BURROW_NO_ERROR;
}

static const IoReadCloserVT frwc_vt = {{NULL, frwc_read}, {NULL, frwc_close}};
static const IoWriterVT frwc_writer_vt = {NULL, frwc_write};

static Error replace_body(void *env, HttpResponse *resp) {
    FailingRWC *f = (FailingRWC *)env;
    f->orig = resp->body;
    resp->body = (IoReadCloser){&frwc_vt, f};
    resp->body_writer = (IoWriter){&frwc_writer_vt, f};
    resp->body_close_write = NULL;
    return BURROW_NO_ERROR;
}

static void TestReverseProxyHijackCopyError(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc bf = BURROW_FN(HttpHandlerFunc, switching_backend, NULL);
    HttptestServer *backend = serve(&bf);
    Error err = BURROW_NO_ERROR;
    Url *backend_url = url_parse(heap_allocator(), backend->url, &err);
    if (backend_url == NULL) {
        httptest_server_free(backend);
        FATALF("%v", err);
    }
    FailingRWC f;
    memset(&f, 0, sizeof f);
    HttputilReverseProxy p = set_url_proxy(backend_url);
    p.modify_response = BURROW_FN(HttputilModifyResponseFunc, replace_body, &f);
    LogLogger *quiet = quiet_log();
    p.error_log = quiet;

    TestRW rw;
    test_rw_init(&rw, t);
    HttpRequest *req = new_get(t, cs("http://example.tld/"));
    if (req != NULL) {
        (void)http_header_set(req->header, cs("Upgrade"), cs("someproto"));
        httputil_reverse_proxy_serve_http(
            &p, (HttpResponseWriter){&test_rw_hijack_vt, &rw}, req);
    }
    test_rw_free(&rw);
    http_request_free(req);
    log_logger_free(heap_allocator(), quiet);
    url_free(heap_allocator(), backend_url);
    httptest_server_free(backend);
}

#define TESTS(X)                                                                       \
    X(TestReverseProxy)                                                                \
    X(TestReverseProxyStripHeadersPresentInConnection)                                 \
    X(TestReverseProxyStripEmptyConnection)                                            \
    X(TestXForwardedFor)                                                               \
    X(TestXForwardedFor_Omit)                                                          \
    X(TestReverseProxyRewriteStripsForwarded)                                          \
    X(TestReverseProxyQuery)                                                           \
    X(TestReverseProxyFlushInterval)                                                   \
    X(TestReverseProxyResponseControllerFlushInterval)                                 \
    X(TestReverseProxyFlushIntervalHeaders)                                            \
    X(TestReverseProxyCancellation)                                                    \
    X(TestNilBody)                                                                     \
    X(TestUserAgentHeader)                                                             \
    X(TestReverseProxyGetPutBuffer)                                                    \
    X(TestReverseProxy_Post)                                                           \
    X(TestReverseProxy_NilBody)                                                        \
    X(TestReverseProxy_AllocatedHeader)                                                \
    X(TestReverseProxyModifyResponse)                                                  \
    X(TestReverseProxyErrorHandler)                                                    \
    X(TestReverseProxy_CopyBuffer)                                                     \
    X(TestServeHTTPDeepCopy)                                                           \
    X(TestClonesRequestHeaders)                                                        \
    X(TestModifyResponseClosesBody)                                                    \
    X(TestReverseProxy_PanicBodyError)                                                 \
    X(TestReverseProxy_PanicClosesIncomingBody)                                        \
    X(TestSelectFlushInterval)                                                         \
    X(TestReverseProxyWebSocket)                                                       \
    X(TestReverseProxyWebSocketCancellation)                                           \
    X(TestReverseProxyWebSocketHalfTCP)                                                \
    X(TestReverseProxyUpgradeNoCloseWrite)                                             \
    X(TestUnannouncedTrailer)                                                          \
    X(TestSetURL)                                                                      \
    X(TestSingleJoinSlash)                                                             \
    X(TestJoinURLPath)                                                                 \
    X(TestReverseProxyRewriteReplacesOut)                                              \
    X(Test1xxHeadersNotModifiedAfterRoundTrip)                                         \
    X(Test1xxResponses)                                                                \
    X(TestReverseProxyQueryParameterSmugglingDirectorDoesNotParseForm)                 \
    X(TestReverseProxyQueryParameterSmugglingDirectorParsesForm)                       \
    X(TestReverseProxyQueryParameterSmugglingRewrite)                                  \
    X(TestReverseProxyQueryParameterSmugglingRewritePreservesRawQuery)                 \
    X(TestReverseProxyHijackCopyError)

TESTING_MAIN(TESTS)
