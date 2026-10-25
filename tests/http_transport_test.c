/* Derived from Go's src/net/http/transport_test.go, in its HTTP/1 mode:
 * keep-alives, Connection: close, HEAD, gzip, the header timeout and limit,
 * closing a body early, and the errors the transport gives before it dials.
 * Go source: go1.27.1.
 *
 * Go's hostPortHandler also writes the address of the net.Conn, in case the
 * kernel hands out the same port again at once. A test can't get at that
 * here, so the tests go by the remote address alone.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/compress/gzip.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/url.h"
#include "burrow/netpoll.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include <stdint.h>
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

static Str cs(const char *s) {
    return str_from_cstr(s);
}

static const char *handler_failed;

#define HCHECK(cond)                                                                   \
    do {                                                                               \
        if (!(cond) && handler_failed == NULL)                                         \
            handler_failed = #cond;                                                    \
    } while (0)

static void check_handlers(TestingT *t) {
    if (handler_failed != NULL)
        testing_t_errorf_v(t, "in a handler: %s", handler_failed);
    handler_failed = NULL;
}

static Int write_str(HttpResponseWriter w, Str s, Error *err) {
    return http_response_writer_write(
        w, slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE), err);
}

/* The body of res, read to the end into a. */
static Str read_all(Alloc *a, HttpResponse *res, Error *err) {
    Slice b = io_read_all(a, io_read_closer_as_io_reader(res->body), err);
    return str_from_bytes(b.p, b.len);
}

static void close_body(HttpResponse *res) {
    (void)res->body.vt->closer.close(res->body.data);
}

static Str query(Alloc *a, HttpRequest *r, const char *key) {
    return url_values_get(url_query(r->url, a), cs(key));
}

/* hostPortHandler. */
static void serve_host_port(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    HttpHeader h = http_response_writer_header(w);
    if (str_eq(query(arena_allocator(&ar), r, "close"), cs("true")))
        HCHECK(http_header_set(h, cs("Connection"), cs("close")));
    HCHECK(http_header_set(h, cs("X-Saw-Close"), r->close ? cs("true") : cs("false")));
    Error err;
    (void)write_str(w, r->remote_addr, &err);
    arena_free(&ar);
}

typedef struct Fetcher {
    TestingT *t;
    HttptestServer *ts;
    Arena ar;
} Fetcher;

/* Gets path from the server with c, and gives back the body, or "" after
 * failing the test. */
static Str fetch(Fetcher *f, HttpClient *c, const char *path, bool req_close,
                 Str *saw_close) {
    Alloc *a = arena_allocator(&f->ar);
    Error err = BURROW_NO_ERROR;
    IoReader none = {NULL, NULL};
    HttpRequest *req = http_new_request(
        a, cs("GET"), fmt_sprintf_v(a, "%s%s", f->ts->url, cs(path)), none, &err);
    if (req == NULL) {
        testing_t_errorf_v(f->t, "NewRequest: %v", err);
        return BURROW_STR_EMPTY;
    }
    req->close = req_close;
    HttpResponse *res = http_client_do(c, req, &err);
    if (res == NULL) {
        testing_t_errorf_v(f->t, "Do %s: %v", cs(path), err);
        return BURROW_STR_EMPTY;
    }
    if (saw_close != NULL)
        *saw_close = str_clone(a, http_header_get(res->header, cs("X-Saw-Close")));
    Str body = read_all(a, res, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(f->t, "ReadAll %s: %v", cs(path), err);
    close_body(res);
    http_response_free(res);
    return body;
}

static HttptestServer *host_port_server(HttpHandlerFunc *f) {
    *f = BURROW_FN(HttpHandlerFunc, serve_host_port, NULL);
    return httptest_new_server(NULL, http_handler_func_as_handler(f));
}

/* Two requests in a row, and whether they came from one connection. */
static void TestTransportKeepAlives(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc hf;
    Fetcher f = {.t = t, .ts = host_port_server(&hf)};
    arena_init(&f.ar, heap_allocator(), 0);
    HttpClient *c = httptest_server_client(f.ts);
    for (int i = 0; i < 2; i++) {
        bool disable = i == 1;
        f.ts->transport.disable_keep_alives = disable;
        Str body1 = fetch(&f, c, "", false, NULL);
        Str body2 = fetch(&f, c, "", false, NULL);
        bool differ = !str_eq(body1, body2);
        if (differ != disable)
            testing_t_errorf_v(
                t,
                "error in disableKeepAlive=%t. unexpected bodiesDiffer=%t; "
                "body1=%q; body2=%q",
                disable, differ, body1, body2);
    }
    arena_free(&f.ar);
    httptest_server_free(f.ts);
    check_handlers(t);
}

static void TestTransportConnectionCloseOnResponse(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc hf;
    Fetcher f = {.t = t, .ts = host_port_server(&hf)};
    arena_init(&f.ar, heap_allocator(), 0);
    HttpClient *c = httptest_server_client(f.ts);
    for (int i = 0; i < 2; i++) {
        bool conn_close = i == 1;
        const char *path = conn_close ? "/?close=true" : "/?close=false";
        Str body1 = fetch(&f, c, path, false, NULL);
        Str body2 = fetch(&f, c, path, false, NULL);
        bool differ = !str_eq(body1, body2);
        if (differ != conn_close)
            testing_t_errorf_v(
                t,
                "error in connectionClose=%t. unexpected bodiesDiffer=%t; "
                "body1=%q; body2=%q",
                conn_close, differ, body1, body2);
        http_transport_close_idle_connections(&f.ts->transport);
    }
    arena_free(&f.ar);
    httptest_server_free(f.ts);
    check_handlers(t);
}

static void TestTransportConnectionCloseOnRequest(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc hf;
    Fetcher f = {.t = t, .ts = host_port_server(&hf)};
    arena_init(&f.ar, heap_allocator(), 0);
    HttpClient *c = httptest_server_client(f.ts);
    for (int i = 0; i < 2; i++) {
        bool req_close = i == 1;
        Str want_saw = req_close ? cs("true") : cs("false");
        Str saw1 = BURROW_STR_EMPTY;
        Str saw2 = BURROW_STR_EMPTY;
        Str body1 = fetch(&f, c, "", req_close, &saw1);
        Str body2 = fetch(&f, c, "", req_close, &saw2);
        if (!str_eq(saw1, want_saw) || !str_eq(saw2, want_saw))
            testing_t_errorf_v(
                t,
                "for Request.Close = %t; handler's X-Saw-Close was %s and "
                "%s; want %s",
                req_close, saw1, saw2, want_saw);
        Int got = str_eq(body1, body2) ? 1 : 2;
        Int want = req_close ? 2 : 1;
        if (got != want)
            testing_t_errorf_v(
                t,
                "for Request.Close=%t: server saw %d unique connections, "
                "wanted %d\n\nbodies were: %q and %q",
                req_close, got, want, body1, body2);
        http_transport_close_idle_connections(&f.ts->transport);
    }
    arena_free(&f.ar);
    httptest_server_free(f.ts);
    check_handlers(t);
}

static void TestTransportConnectionCloseOnRequestDisableKeepAlive(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc hf;
    Fetcher f = {.t = t, .ts = host_port_server(&hf)};
    arena_init(&f.ar, heap_allocator(), 0);
    f.ts->transport.disable_keep_alives = true;
    Str saw = BURROW_STR_EMPTY;
    (void)fetch(&f, httptest_server_client(f.ts), "", false, &saw);
    if (!str_eq(saw, cs("true")))
        testing_t_errorf_v(t, "handler didn't see Connection: close ");
    arena_free(&f.ar);
    httptest_server_free(f.ts);
    check_handlers(t);
}

/* serve_read_to_end, the handler of TestTransportReadToEndReusesConn. */
static void serve_read_to_end(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Str msg = cs("foobar");
    HttpHeader h = http_response_writer_header(w);
    HCHECK(http_header_set(h, cs("Remote-Addr"), r->remote_addr));
    if (str_eq(r->url->path, cs("/chunked/"))) {
        http_response_writer_write_header(w, 200);
        HttpResponseController rc = http_new_response_controller(w);
        HCHECK(BURROW_OK(http_response_controller_flush(&rc)));
    } else {
        HCHECK(http_header_set(h, cs("Content-Length"), cs("6")));
        http_response_writer_write_header(w, 200);
    }
    Error err;
    (void)write_str(w, msg, &err);
}

static void TestTransportReadToEndReusesConn(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, serve_read_to_end, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&hf));
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    static const char *const paths[] = {"/content-length/", "/chunked/"};
    for (int pi = 0; pi < 2; pi++) {
        int64_t want_len = pi == 0 ? 6 : -1;
        Str first = BURROW_STR_EMPTY;
        Int distinct = 0;
        for (int i = 0; i < 3; i++) {
            Error err = BURROW_NO_ERROR;
            HttpResponse *res =
                http_client_get(httptest_server_client(ts),
                                fmt_sprintf_v(a, "%s%s", ts->url, cs(paths[pi])), &err);
            if (res == NULL) {
                testing_t_errorf_v(t, "Get %s: %v", cs(paths[pi]), err);
                continue;
            }
            if (res->content_length != want_len)
                testing_t_errorf_v(t, "%s res.ContentLength = %d; want %d",
                                   cs(paths[pi]), res->content_length, want_len);
            /* Read to the end and not closed, until the response goes. */
            Str got = read_all(a, res, &err);
            if (!str_eq(got, cs("foobar")) || BURROW_FAILED(err))
                testing_t_errorf_v(t, "%s ReadAll(Body) = %q, %v; want %q, nil",
                                   cs(paths[pi]), got, err, cs("foobar"));
            Str addr = str_clone(a, http_header_get(res->header, cs("Remote-Addr")));
            if (distinct == 0 || !str_eq(addr, first))
                distinct++;
            if (distinct == 1)
                first = addr;
            close_body(res);
            http_response_free(res);
        }
        if (distinct != 1)
            testing_t_errorf_v(
                t, "for %s, server saw %d distinct client addresses; want 1",
                cs(paths[pi]), distinct);
    }
    arena_free(&ar);
    httptest_server_free(ts);
    check_handlers(t);
}

static void serve_head(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    HCHECK(str_eq(r->method, cs("HEAD")));
    HCHECK(http_header_set(http_response_writer_header(w), cs("Content-Length"),
                           cs("123")));
    http_response_writer_write_header(w, 200);
}

static void TestTransportHeadResponses(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, serve_head, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&hf));
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    for (int i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        HttpResponse *res = http_client_head(httptest_server_client(ts), ts->url, &err);
        if (res == NULL) {
            testing_t_errorf_v(t, "error on loop %d: %v", (Int)i, err);
            continue;
        }
        Str cl = http_header_get(res->header, cs("Content-Length"));
        if (!str_eq(cl, cs("123")))
            testing_t_errorf_v(t,
                               "loop %d: expected Content-Length header of %q, got %q",
                               (Int)i, cs("123"), cl);
        if (res->content_length != 123)
            testing_t_errorf_v(t, "loop %d: expected res.ContentLength of %d, got %d",
                               (Int)i, (int64_t)123, res->content_length);
        Str all = read_all(arena_allocator(&ar), res, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "loop %d: Body ReadAll: %v", (Int)i, err);
        else if (all.len != 0)
            testing_t_errorf_v(t, "Bogus body %q", all);
        close_body(res);
        http_response_free(res);
    }
    arena_free(&ar);
    httptest_server_free(ts);
    check_handlers(t);
}

static void serve_head_chunked(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    HCHECK(str_eq(r->method, cs("HEAD")));
    HttpHeader h = http_response_writer_header(w);
    HCHECK(http_header_set(h, cs("Transfer-Encoding"), cs("chunked"))); /* ignored */
    HCHECK(http_header_set(h, cs("x-client-ipport"), r->remote_addr));
    http_response_writer_write_header(w, 200);
}

/* Go waits for the read loop between the two with a hook. Here the second
 * request waits for the connection to be idle instead, which it is once the
 * first response is freed. */
static void TestTransportHeadChunkedResponse(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, serve_head_chunked, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&hf));
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str v[2] = {BURROW_STR_EMPTY, BURROW_STR_EMPTY};
    for (int i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        HttpResponse *res = http_client_head(httptest_server_client(ts), ts->url, &err);
        if (res == NULL) {
            testing_t_errorf_v(t, "request %d error: %v", (Int)i + 1, err);
            break;
        }
        v[i] = str_clone(a, http_header_get(res->header, cs("x-client-ipport")));
        close_body(res);
        http_response_free(res);
    }
    if (!str_eq(v[0], v[1]))
        testing_t_errorf_v(t, "ip/ports differed between head requests: %q vs %q", v[0],
                           v[1]);
    arena_free(&ar);
    httptest_server_free(ts);
    check_handlers(t);
}

static const char response_body[] = "test response body";

static void serve_gzip(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Str accept = http_header_get(r->header, cs("Accept-Encoding"));
    HCHECK(str_eq(accept, query(arena_allocator(&ar), r, "expect_accept")));
    HttpHeader h = http_response_writer_header(w);
    Error err;
    Str body = cs(response_body);
    if (str_eq(accept, cs("gzip"))) {
        HCHECK(http_header_set(h, cs("Content-Encoding"), cs("gzip")));
        GzipWriter *gz =
            gzip_new_writer(heap_allocator(), http_response_writer_as_io_writer(w));
        HCHECK(gz != NULL);
        if (gz != NULL) {
            (void)gzip_writer_write(
                gz,
                slice_from((void *)(uintptr_t)body.p, body.len, body.len, TYPE_BYTE),
                &err);
            HCHECK(BURROW_OK(gzip_writer_close(gz)));
            gzip_writer_free(gz);
        }
    } else {
        HCHECK(http_header_set(h, cs("Content-Encoding"), accept));
        (void)write_str(w, body, &err);
    }
    arena_free(&ar);
}

/* What the round tripper changes in the request it puts back. */
static void TestRoundTripGzip(TestingT *t) {
    need_tcp(t);
    static const struct {
        const char *accept, *expect_accept;
        bool compressed;
    } tests[] = {
        /* Requests with no accept-encoding header use transparent compression. */
        {"", "gzip", false},
        /* Requests with other accept-encoding should pass through unmodified. */
        {"foo", "foo", false},
        /* Requests with accept-encoding == gzip should be passed through. */
        {"gzip", "gzip", true},
    };
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, serve_gzip, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&hf));
    HttpRoundTripper tr = http_transport_as_round_tripper(&ts->transport);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    IoReader none = {NULL, NULL};
    for (int i = 0; i < 3; i++) {
        Error err = BURROW_NO_ERROR;
        Str url = fmt_sprintf_v(a, "%s/?testnum=%d&expect_accept=%s", ts->url, (Int)i,
                                cs(tests[i].expect_accept));
        HttpRequest *req = http_new_request(a, cs("GET"), url, none, &err);
        CHECK(req != NULL);
        if (req == NULL)
            continue;
        Str accept = cs(tests[i].accept);
        if (accept.len > 0)
            CHECK(http_header_set(req->header, cs("Accept-Encoding"), accept));
        HttpResponse *res = http_round_tripper_round_trip(tr, req, &err);
        if (res == NULL) {
            testing_t_errorf_v(t, "%d. RoundTrip: %v", (Int)i, err);
            continue;
        }
        Str body = BURROW_STR_EMPTY;
        if (tests[i].compressed) {
            GzipReader *zr =
                gzip_new_reader(a, io_read_closer_as_io_reader(res->body), &err);
            if (zr == NULL) {
                testing_t_errorf_v(t, "%d. gzip NewReader: %v", (Int)i, err);
                close_body(res);
                http_response_free(res);
                continue;
            }
            Slice b = io_read_all(a, gzip_reader_as_io_reader(zr), &err);
            body = str_from_bytes(b.p, b.len);
            gzip_reader_free(zr);
        } else {
            body = read_all(a, res, &err);
        }
        close_body(res);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%d. Error: %q", (Int)i, error_text(err));
        else if (!str_eq(body, cs(response_body)))
            testing_t_errorf_v(t, "%d. body = %q; want %q", (Int)i, body,
                               cs(response_body));
        Str g = http_header_get(req->header, cs("Accept-Encoding"));
        if (!str_eq(g, accept))
            testing_t_errorf_v(t,
                               "%d. Accept-Encoding = %q; want %q (it was mutated, in "
                               "violation of RoundTrip contract)",
                               (Int)i, g, accept);
        g = http_header_get(res->header, cs("Content-Encoding"));
        if (!str_eq(g, accept))
            testing_t_errorf_v(t, "%d. Content-Encoding = %q; want %q", (Int)i, g,
                               accept);
        http_response_free(res);
    }
    arena_free(&ar);
    httptest_server_free(ts);
    check_handlers(t);
}

/* The handler of TestTransportResponseHeaderTimeout. */
typedef struct HeaderTimeout {
    SyncWaitGroup srv_wg;
    Chan *in_handler;
} HeaderTimeout;

static void serve_fast(void *env, HttpResponseWriter w, HttpRequest *r) {
    HeaderTimeout *ht = (HeaderTimeout *)env;
    (void)w;
    (void)r;
    bool v = true;
    chan_send(ht->in_handler, &v);
    sync_wait_group_done(&ht->srv_wg);
}

static void serve_slow(void *env, HttpResponseWriter w, HttpRequest *r) {
    HeaderTimeout *ht = (HeaderTimeout *)env;
    (void)w;
    bool v = true;
    chan_send(ht->in_handler, &v);
    (void)chan_recv(context_done(http_request_context(r)), NULL);
    sync_wait_group_done(&ht->srv_wg);
}

static void TestTransportResponseHeaderTimeout(TestingT *t) {
    need_tcp(t);
    if (testing_short())
        testing_t_skip_v(t, "skipping timeout test in -short mode");
    Duration timeout = 2 * TIME_MILLISECOND;
    bool retry = true;
    while (retry && !testing_t_failed(t)) {
        HeaderTimeout ht;
        memset(&ht, 0, sizeof ht);
        ht.in_handler = chan_make(heap_allocator(), TYPE_BOOL, 1);
        HttpHandlerFunc fast = BURROW_FN(HttpHandlerFunc, serve_fast, &ht);
        HttpHandlerFunc slow = BURROW_FN(HttpHandlerFunc, serve_slow, &ht);
        HttpServeMux *mux = http_new_serve_mux(heap_allocator());
        http_serve_mux_handle_func(mux, cs("/fast"), fast);
        http_serve_mux_handle_func(mux, cs("/slow"), slow);
        HttptestServer *ts = httptest_new_server(NULL, http_serve_mux_as_handler(mux));
        HttpClient *c = httptest_server_client(ts);
        ts->transport.response_header_timeout = timeout;

        retry = false;
        sync_wait_group_add(&ht.srv_wg, 3);
        static const struct {
            const char *path;
            bool want_timeout;
        } tests[] = {{"/fast", false}, {"/slow", true}, {"/fast", false}};
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Alloc *a = arena_allocator(&ar);
        for (int i = 0; i < 3; i++) {
            Error err = BURROW_NO_ERROR;
            HttpResponse *res = http_client_get(
                c, fmt_sprintf_v(a, "%s%s", ts->url, cs(tests[i].path)), &err);
            (void)chan_recv(ht.in_handler, NULL);
            if (res == NULL) {
                const UrlError *ue = (const UrlError *)errors_as(err, TYPE_URL_ERROR);
                if (ue == NULL) {
                    testing_t_errorf_v(t, "error is not a url.Error; got: %v", err);
                    continue;
                }
                if (!net_is_error(ue->err)) {
                    testing_t_errorf_v(
                        t,
                        "error does not satisfy net.Error interface; got: "
                        "%v",
                        err);
                    continue;
                }
                if (!net_error_timeout(ue->err)) {
                    testing_t_errorf_v(t, "want timeout error; got: %q",
                                       error_text(err));
                    continue;
                }
                if (!tests[i].want_timeout && !retry) {
                    /* The timeout may be set too short. Retry with a longer one. */
                    testing_t_logf_v(
                        t,
                        "unexpected timeout for path %q after %v; retrying "
                        "with longer timeout",
                        cs(tests[i].path), timeout);
                    timeout *= 2;
                    retry = true;
                }
                if (!strings_contains(error_text(err),
                                      cs("timeout awaiting response headers")))
                    testing_t_errorf_v(t, "%d. unexpected error: %v", (Int)i, err);
                continue;
            }
            if (tests[i].want_timeout)
                testing_t_errorf_v(t,
                                   "no error for path %q; expected \"timeout awaiting "
                                   "response headers\"",
                                   cs(tests[i].path));
            if (res->status_code != 200)
                testing_t_errorf_v(t, "%d. Status = %d; want 200", (Int)i,
                                   res->status_code);
            close_body(res);
            http_response_free(res);
        }
        sync_wait_group_wait(&ht.srv_wg);
        arena_free(&ar);
        httptest_server_free(ts);
        http_serve_mux_free(mux);
        chan_free(ht.in_handler);
    }
}

/* The handler of TestTransportCloseResponseBody. */
typedef struct Young {
    Chan *write_err;
} Young;

static void serve_young(void *env, HttpResponseWriter w, HttpRequest *r) {
    Young *y = (Young *)env;
    (void)r;
    HttpResponseController rc = http_new_response_controller(w);
    for (;;) {
        Error err = BURROW_NO_ERROR;
        (void)write_str(w, cs("young\n"), &err);
        if (BURROW_FAILED(err)) {
            bool failed = true;
            chan_send(y->write_err, &failed);
            return;
        }
        (void)http_response_controller_flush(&rc);
    }
}

static void TestTransportCloseResponseBody(TestingT *t) {
    need_tcp(t);
    Young y = {chan_make(heap_allocator(), TYPE_BOOL, 1)};
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, serve_young, &y);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&hf));
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(httptest_server_client(ts), ts->url, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        Byte buf[18];
        Int n = io_read_full(io_read_closer_as_io_reader(res->body),
                             slice_from(buf, 18, 18, TYPE_BYTE), &err);
        if (n != 18 || BURROW_FAILED(err))
            testing_t_errorf_v(t, "ReadFull: %d, %v", n, err);
        else if (memcmp(buf, "young\nyoung\nyoung\n", 18) != 0)
            testing_t_errorf_v(t, "read %q; want %q", str_from_bytes(buf, 18),
                               cs("young\nyoung\nyoung\n"));
        Error cerr = res->body.vt->closer.close(res->body.data);
        if (BURROW_FAILED(cerr))
            testing_t_errorf_v(t, "Close = %v", cerr);
        bool failed = false;
        (void)chan_recv(y.write_err, &failed);
        if (!failed)
            testing_t_errorf_v(t, "expected non-nil write error");
        http_response_free(res);
    }
    httptest_server_free(ts);
    chan_free(y.write_err);
    check_handlers(t);
}

/* fooProto. */
static HttpResponse *foo_round_trip(void *self, HttpRequest *req, Error *err) {
    (void)self;
    Alloc *a = heap_allocator();
    HttpResponse *r = (HttpResponse *)mem_alloc(a, sizeof *r, _Alignof(HttpResponse));
    if (r == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    r->a = a;
    arena_init(&r->arena, a, 0);
    Alloc *ra = arena_allocator(&r->arena);
    r->status = cs("200 OK");
    r->status_code = 200;
    r->header = http_header_make(ra);
    StringsReader *sr = strings_new_reader(
        ra, fmt_sprintf_v(ra, "You wanted %s", url_string(req->url, ra)));
    IoNopCloser *nc = (IoNopCloser *)mem_alloc(ra, sizeof *nc, _Alignof(IoNopCloser));
    if (r->header == NULL || sr == NULL || nc == NULL) {
        http_response_free(r);
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    *nc = io_nop_closer(strings_reader_as_io_reader(sr));
    r->body = io_nop_closer_as_io_read_closer(nc);
    *err = BURROW_NO_ERROR;
    return r;
}

static const HttpRoundTripperVT foo_vt = {NULL, foo_round_trip};

static void TestTransportAltProto(TestingT *t) {
    HttpTransport tr;
    memset(&tr, 0, sizeof tr);
    HttpClient c;
    memset(&c, 0, sizeof c);
    c.transport = http_transport_as_round_tripper(&tr);
    CHECK(http_transport_register_protocol(&tr, cs("foo"),
                                           (HttpRoundTripper){&foo_vt, NULL}));
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(&c, cs("foo://bar.com/path"), &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Str body = read_all(arena_allocator(&ar), res, &err);
        CHECK(BURROW_OK(err));
        if (!str_eq(body, cs("You wanted foo://bar.com/path")))
            testing_t_errorf_v(t, "got response %q, want %q", body,
                               cs("You wanted foo://bar.com/path"));
        arena_free(&ar);
        close_body(res);
        http_response_free(res);
    }
    http_transport_free(&tr);
}

static void TestTransportNoHost(TestingT *t) {
    HttpTransport tr;
    memset(&tr, 0, sizeof tr);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Url u;
    memset(&u, 0, sizeof u);
    u.scheme = cs("http");
    HttpRequest req;
    memset(&req, 0, sizeof req);
    req.header = http_header_make(a);
    req.url = &u;
    Error err = BURROW_NO_ERROR;
    HttpResponse *res =
        http_round_tripper_round_trip(http_transport_as_round_tripper(&tr), &req, &err);
    CHECK(res == NULL);
    http_response_free(res);
    if (!str_eq(error_text(err), cs("http: no Host in request URL")))
        testing_t_errorf_v(t, "error = %v; want %q", err,
                           cs("http: no Host in request URL"));
    arena_free(&ar);
    http_transport_free(&tr);
}

static void serve_long_header(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    if (!str_eq(r->url->path, cs("/long")))
        return;
    HttpHeader h = http_response_writer_header(w);
    Str v = strings_repeat(burrow__map_allocator(h), cs("a"), 1 << 20);
    HCHECK(http_header_set(h, cs("Long"), v));
}

static void TestTransportResponseHeaderLength(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, serve_long_header, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&hf));
    HttpClient *c = httptest_server_client(ts);
    ts->transport.max_response_header_bytes = 512 << 10;
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(c, ts->url, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        close_body(res);
        http_response_free(res);
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    res = http_client_get(c, fmt_sprintf_v(arena_allocator(&ar), "%s/long", ts->url),
                          &err);
    if (res != NULL) {
        testing_t_errorf_v(t, "Unexpected success. Got %s", res->status);
        close_body(res);
        http_response_free(res);
    } else if (!strings_contains(error_text(err),
                                 cs("server response headers exceeded 524288 bytes"))) {
        testing_t_errorf_v(t, "got error: %v; want %q", err,
                           cs("server response headers exceeded 524288 bytes"));
    }
    arena_free(&ar);
    httptest_server_free(ts);
    check_handlers(t);
}

static void TestTransportRejectsAlphaPort(TestingT *t) {
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_get(cs("http://dummy.tld:123foo/bar"), &err);
    if (res != NULL) {
        close_body(res);
        http_response_free(res);
        testing_t_fatalf_v(t, "unexpected success");
        return;
    }
    const UrlError *ue = (const UrlError *)errors_as(err, TYPE_URL_ERROR);
    if (ue == NULL) {
        testing_t_fatalf_v(t, "got %v; want *url.Error", err);
        return;
    }
    Str got = error_text(ue->err);
    if (!str_eq(got, cs("invalid port \":123foo\" after host")))
        testing_t_errorf_v(t, "got error %q; want %q", got,
                           cs("invalid port \":123foo\" after host"));
}

/* ------------------------------------------------------------- CONNECT */

/* TestTransportCONNECTRejected is newer than go1.27.1: it came with the
 * go1.27.2 fix for GO-2026-6605, CL 847306. Go runs it under synctest with
 * newHTTP1TransportTest, which fakes the dial and the connection. That is not
 * here. Not in Go: a loopback listener plays the proxy, connect_dial notes
 * what the transport dials and dials the listener, and connect_proxy does the
 * proxy's side in a goroutine. The connection was closed when connect_proxy's
 * read after the response stops on io_eof. */

typedef struct ConnectProxy {
    NetListener ln;
    Str ln_addr;
    char network[16]; /* what the transport dialed */
    char addr[64];
    Error accept_err;
    Error read_err;
    Str path;
    bool got_request;
    Error end; /* the error the read after the response stopped on */
    Arena ar;  /* connect_proxy's */
} ConnectProxy;

static void copy_cstr(char *dst, size_t cap, Str s) {
    size_t n = (size_t)s.len < cap - 1 ? (size_t)s.len : cap - 1;
    if (n > 0)
        memcpy(dst, s.p, n);
    dst[n] = '\0';
}

static NetConn connect_dial(void *env, Context ctx, Str network, Str addr, Error *err) {
    ConnectProxy *p = (ConnectProxy *)env;
    copy_cstr(p->network, sizeof p->network, network);
    copy_cstr(p->addr, sizeof p->addr, addr);
    NetDialer d;
    memset(&d, 0, sizeof d);
    return net_dialer_dial_context(&d, heap_allocator(), ctx, BURROW_S("tcp"),
                                   p->ln_addr, err);
}

static void connect_proxy(void *env) {
    ConnectProxy *p = (ConnectProxy *)env;
    Alloc *a = arena_allocator(&p->ar);
    Error err = BURROW_NO_ERROR;
    NetConn conn = p->ln.vt->accept(p->ln.data, &err);
    if (BURROW_FAILED(err)) {
        p->accept_err = err;
        return;
    }
    (void)conn.vt->set_deadline(conn.data, time_add(time_now(), 10 * TIME_SECOND));
    BufioReader *br = bufio_new_reader(a, net_conn_as_io_reader(conn));
    HttpRequest *req = br == NULL ? NULL : http_read_request(a, br, &err);
    if (req == NULL) {
        p->read_err = err;
        net_conn_free(conn);
        return;
    }
    p->got_request = true;
    p->path = str_clone(a, req->url->path);
    http_request_free(req);

    /* conn.writeMessage. */
    static const char msg[] = "HTTP/1.1 405 We Have No Connections Today\r\n"
                              "Content-Length: 0\r\n"
                              "\r\n";
    (void)conn.vt->writer.write(
        conn.data,
        slice_from((void *)(uintptr_t)msg, sizeof msg - 1, sizeof msg - 1, TYPE_BYTE),
        &err);

    /* conn.wantClosed. */
    Byte b[1];
    for (;;) {
        Error rerr = BURROW_NO_ERROR;
        (void)bufio_reader_read(br, slice_from(b, 1, 1, TYPE_BYTE), &rerr);
        if (BURROW_FAILED(rerr)) {
            p->end = rerr;
            break;
        }
    }
    net_conn_free(conn);
}

static void TestTransportCONNECTRejected(TestingT *t) {
    need_tcp(t);
    ConnectProxy p;
    memset(&p, 0, sizeof p);
    arena_init(&p.ar, heap_allocator(), 0);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    p.ln = net_listen(heap_allocator(), BURROW_S("tcp"), BURROW_S("127.0.0.1:0"), &err);
    if (BURROW_FAILED(err)) {
        err = BURROW_NO_ERROR;
        p.ln =
            net_listen(heap_allocator(), BURROW_S("tcp6"), BURROW_S("[::1]:0"), &err);
    }
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        arena_free(&p.ar);
        testing_t_fatal_v(t, err);
    }
    NetAddr laddr = p.ln.vt->addr(p.ln.data);
    p.ln_addr = laddr.vt->string(laddr.data, a);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, connect_proxy, &p));

    HttpTransport tr;
    memset(&tr, 0, sizeof tr);
    tr.dial_context = BURROW_FN(HttpDialContextFunc, connect_dial, &p);

    /* Not in Go: the Request literal, with its URL. */
    Url u;
    memset(&u, 0, sizeof u);
    u.scheme = cs("http");
    u.opaque = cs("backend.example.tld:80");
    u.host = cs("proxy.example.tld");
    HttpRequest sent_req;
    memset(&sent_req, 0, sizeof sent_req);
    sent_req.method = cs("CONNECT");
    sent_req.url = &u;
    sent_req.host = cs("proxy.example.tld");
    sent_req.header = http_header_make(a);
    HttpResponse *res = http_round_tripper_round_trip(
        http_transport_as_round_tripper(&tr), &sent_req, &err);

    /* rt.wantStatus(405). */
    if (res == NULL) {
        testing_t_errorf_v(t, "RoundTrip: %v", err);
    } else {
        if (res->status_code != 405)
            testing_t_errorf_v(t, "got response status %v, want %v", res->status_code,
                               405);
        close_body(res);
        http_response_free(res);
    }

    /* Once the response is in, the transport should close the connection, and
     * the proxy's read ends. Not in Go: should it not, the deadline on the
     * proxy's side ends it. */
    sync_wait_group_wait(&wg);
    if (BURROW_FAILED(p.accept_err))
        testing_t_errorf_v(t, "accept: %v", p.accept_err);
    if (!str_eq(cs(p.network), cs("tcp")) ||
        !str_eq(cs(p.addr), cs("proxy.example.tld:80")))
        testing_t_errorf_v(t, "dialed %q %q, want %q %q", cs(p.network), cs(p.addr),
                           cs("tcp"), cs("proxy.example.tld:80"));
    if (!p.got_request && BURROW_OK(p.accept_err))
        testing_t_errorf_v(t, "reading the request: %v", p.read_err);
    if (p.got_request && !str_eq(p.path, u.path))
        testing_t_errorf_v(t, "read request path %q, want %q", p.path, u.path);
    if (p.got_request && !errors_is(p.end, io_eof))
        testing_t_errorf_v(t, "conn.Read = %v, want io.EOF", p.end);
    http_transport_free(&tr);
    net_listener_free(p.ln);
    arena_free(&p.ar);
    arena_free(&ar);
}

/* ------------------------------------------------ draining a response body */

/* TestTransportResponseBodyDrainReadAndClose and
 * TestTransportResponseBodyDrainDropsTrailers are newer than go1.27.1: they
 * came with go1.27.2's fixes for go.dev/issue/81404 and 81511 and for the
 * trailer of a drained body. Go runs them under synctest with
 * newHTTP1TransportTest, as TestTransportCONNECTRejected above. Not in Go: a
 * loopback listener plays example.tld, peer_dial notes what the transport
 * dials and dials the listener, and the test does the server's side of the
 * connection itself. Where Go waits for the bubble to go quiet, the test
 * sleeps a little before a Close that should find a Read in flight, and waits
 * for the transport to have the connection back in its pool before the next
 * request. */

typedef struct Peer {
    NetListener ln;
    Str ln_addr;
    SyncMutex mu;     /* guards network and addr, which peer_dial writes */
    char network[16]; /* what the transport dialed */
    char addr[64];
    Arena ar;
} Peer;

static NetConn peer_dial(void *env, Context ctx, Str network, Str addr, Error *err) {
    Peer *p = (Peer *)env;
    sync_mutex_lock(&p->mu);
    copy_cstr(p->network, sizeof p->network, network);
    copy_cstr(p->addr, sizeof p->addr, addr);
    sync_mutex_unlock(&p->mu);
    NetDialer d;
    memset(&d, 0, sizeof d);
    return net_dialer_dial_context(&d, heap_allocator(), ctx, BURROW_S("tcp"),
                                   p->ln_addr, err);
}

/* newHTTP1TransportTest, with tr dialing p. */
static bool peer_listen(TestingT *t, Peer *p, HttpTransport *tr) {
    memset(p, 0, sizeof *p);
    arena_init(&p->ar, heap_allocator(), 0);
    Error err = BURROW_NO_ERROR;
    p->ln =
        net_listen(heap_allocator(), BURROW_S("tcp"), BURROW_S("127.0.0.1:0"), &err);
    if (BURROW_FAILED(err)) {
        err = BURROW_NO_ERROR;
        p->ln =
            net_listen(heap_allocator(), BURROW_S("tcp6"), BURROW_S("[::1]:0"), &err);
    }
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "listen: %v", err);
        arena_free(&p->ar);
        return false;
    }
    NetAddr laddr = p->ln.vt->addr(p->ln.data);
    p->ln_addr = laddr.vt->string(laddr.data, arena_allocator(&p->ar));
    memset(tr, 0, sizeof *tr);
    tr->dial_context = BURROW_FN(HttpDialContextFunc, peer_dial, p);
    return true;
}

/* The test's side of the connection. */
typedef struct PeerConn {
    NetConn conn;
    BufioReader *br;
} PeerConn;

/* tt.wantDial("tcp", "example.tld:80").connect(). Not in Go: the deadline,
 * which keeps a test from hanging on a connection that the transport does not
 * use. */
static bool peer_accept(TestingT *t, Peer *p, PeerConn *c) {
    Error err = BURROW_NO_ERROR;
    c->conn = p->ln.vt->accept(p->ln.data, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "accept: %v", err);
        return false;
    }
    sync_mutex_lock(&p->mu);
    Str network = cs(p->network), addr = cs(p->addr);
    if (!str_eq(network, cs("tcp")) || !str_eq(addr, cs("example.tld:80")))
        testing_t_errorf_v(t, "dialed %q %q, want %q %q", network, addr, cs("tcp"),
                           cs("example.tld:80"));
    sync_mutex_unlock(&p->mu);
    (void)c->conn.vt->set_deadline(c->conn.data,
                                   time_add(time_now(), 10 * TIME_SECOND));
    c->br = bufio_new_reader(arena_allocator(&p->ar), net_conn_as_io_reader(c->conn));
    if (c->br == NULL) {
        testing_t_errorf_v(t, "bufio_new_reader: out of memory");
        net_conn_free(c->conn);
        return false;
    }
    return true;
}

/* conn.readRequest, giving the request's path. */
static bool peer_read_request(TestingT *t, Peer *p, PeerConn *c, Str *path) {
    Alloc *a = arena_allocator(&p->ar);
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = http_read_request(a, c->br, &err);
    if (req == NULL) {
        testing_t_errorf_v(t, "reading the request: %v", err);
        return false;
    }
    *path = str_clone(a, req->url->path);
    http_request_free(req);
    return true;
}

/* conn.writeMessage, with msg already joined by CRLF. */
static bool peer_write(TestingT *t, PeerConn *c, const char *msg) {
    Error err = BURROW_NO_ERROR;
    Str s = cs(msg);
    (void)c->conn.vt->writer.write(
        c->conn.data, slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE),
        &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "conn write: %v", err);
        return false;
    }
    return true;
}

/* conn.wantClosed. */
static void peer_want_closed(TestingT *t, PeerConn *c) {
    Error err = BURROW_NO_ERROR;
    Slice got = bufio_reader_peek(c->br, 32, &err);
    if (got.len != 0 || !errors_is(err, io_eof)) {
        Str gs = str_from_bytes(got.p, got.len);
        testing_t_errorf_v(t, "read from conn: %q; expect conn to be closed", gs);
    }
}

/* tt.roundTrip, in a goroutine of its own. */
typedef struct PeerRT {
    HttpTransport *tr;
    HttpRequest *req;
    HttpResponse *res;
    Error err; /* in ar */
    Arena ar;
    SyncWaitGroup wg;
} PeerRT;

static void peer_rt_job(void *env) {
    PeerRT *rt = (PeerRT *)env;
    Error err = BURROW_NO_ERROR;
    rt->res = http_round_tripper_round_trip(http_transport_as_round_tripper(rt->tr),
                                            rt->req, &err);
    rt->err = error_retain(arena_allocator(&rt->ar), err);
}

static bool peer_round_trip(TestingT *t, PeerRT *rt, HttpTransport *tr, Str url) {
    memset(rt, 0, sizeof *rt);
    arena_init(&rt->ar, heap_allocator(), 0);
    rt->tr = tr;
    Error err = BURROW_NO_ERROR;
    rt->req = http_new_request(arena_allocator(&rt->ar), BURROW_S("GET"), url,
                               (IoReader){0}, &err);
    if (rt->req == NULL) {
        testing_t_errorf_v(t, "NewRequest: %v", err);
        arena_free(&rt->ar);
        return false;
    }
    sync_wait_group_go(&rt->wg, BURROW_FN(Func, peer_rt_job, rt));
    return true;
}

/* rt.response. */
static HttpResponse *peer_rt_response(TestingT *t, PeerRT *rt) {
    sync_wait_group_wait(&rt->wg);
    if (rt->res == NULL)
        testing_t_errorf_v(t, "RoundTrip: %v", rt->err);
    return rt->res;
}

/* rt.wantStatus. */
static void peer_rt_want_status(TestingT *t, PeerRT *rt, Int want) {
    HttpResponse *res = peer_rt_response(t, rt);
    if (res != NULL && res->status_code != want)
        testing_t_errorf_v(t, "got response status %v, want %v", res->status_code,
                           want);
}

/* Not in Go: what the round trip leaves, once it is done. */
static void peer_rt_free(PeerRT *rt) {
    sync_wait_group_wait(&rt->wg);
    if (rt->res != NULL) {
        close_body(rt->res);
        http_response_free(rt->res);
    }
    http_request_free(rt->req);
    arena_free(&rt->ar);
}

/* With the listener closed first, a round trip still dialing it fails, so
 * that rt can be waited for. rt may be NULL. */
static void peer_free(Peer *p, HttpTransport *tr, PeerRT *rt) {
    net_listener_free(p->ln);
    if (rt != NULL)
        peer_rt_free(rt);
    http_transport_free(tr);
    arena_free(&p->ar);
}

/* Not in Go: in place of synctest.Wait before the next request, wait for the
 * transport to have the connection back in its pool. */
static void peer_wait_idle(HttpTransport *tr) {
    Time deadline = time_add(time_now(), 10 * TIME_SECOND);
    while (burrow__http_transport_idle_conn_count_for_testing(
               tr, BURROW_S("http"), BURROW_S("example.tld:80")) == 0 &&
           time_before(time_now(), deadline))
        time_sleep(TIME_MILLISECOND);
}

/* The next request, which should come on the same connection. */
static void peer_want_reuse(TestingT *t, Peer *p, PeerConn *c, HttpTransport *tr) {
    peer_wait_idle(tr);
    PeerRT rt;
    if (!peer_round_trip(t, &rt, tr, BURROW_S("http://example.tld/next")))
        return;
    Str got = {0};
    if (peer_read_request(t, p, c, &got)) {
        if (!str_eq(got, BURROW_S("/next")))
            testing_t_errorf_v(t, "request path = %q, want /next", got);
        else if (peer_write(t, c,
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Length: 0\r\n"
                            "\r\n"))
            peer_rt_want_status(t, &rt, 200);
    }
    /* Not in Go: a round trip that is still waiting ends when the test's side
     * of the connection closes. */
    (void)net_tcp_conn_close_write(net_conn_as_tcp_conn(c->conn));
    peer_rt_free(&rt);
}

typedef struct DrainCase {
    const char *name;
    bool read;
    bool close_early;
    bool close_after_read;
    bool server_truncate;
    const Error *want_read_err; /* NULL for nil */
    const Error *want_close_err;
    bool want_reuse;
} DrainCase;

static const DrainCase drain_cases[] = {
    /* go.dev/issue/81404. */
    {
        .name = "concurrent early close and read to eof",
        .read = true,
        .close_early = true,
        .want_read_err = &io_eof,
        .want_reuse = true,
    },
    {
        .name = "concurrent early close and unexpected read error",
        .read = true,
        .close_early = true,
        .server_truncate = true,
        .want_read_err = &io_err_unexpected_eof,
        .want_reuse = false,
    },
    {
        .name = "unexpected read error without any close",
        .read = true,
        .server_truncate = true,
        .want_read_err = &io_err_unexpected_eof,
        .want_reuse = false,
    },
    {
        .name = "unexpected read error followed by close",
        .read = true,
        .close_after_read = true,
        .server_truncate = true,
        .want_read_err = &io_err_unexpected_eof,
        /* golang.org/issue/81511: Close does not repeat the Read error. */
        .want_close_err = NULL,
        .want_reuse = false,
    },
    {
        .name = "early close without any read",
        .read = false,
        .close_early = true,
        .want_reuse = true,
    },
    {
        .name = "read all then close",
        .read = true,
        .close_after_read = true,
        .want_read_err = &io_eof,
        .want_reuse = true,
    },
};

static bool drain_err_is(Error err, const Error *want) {
    return want == NULL ? BURROW_OK(err) : errors_is(err, *want);
}

static Error drain_want(const Error *want) {
    return want == NULL ? BURROW_NO_ERROR : *want;
}

/* The Read in its goroutine. */
typedef struct DrainRead {
    HttpResponse *res;
    Error err; /* in ar */
    Arena ar;
    SyncWaitGroup wg;
} DrainRead;

static void drain_read_job(void *env) {
    DrainRead *r = (DrainRead *)env;
    Byte buf[1];
    Error err = BURROW_NO_ERROR;
    (void)r->res->body.vt->reader.read(r->res->body.data,
                                       slice_from(buf, 1, 1, TYPE_BYTE), &err);
    r->err = error_retain(arena_allocator(&r->ar), err);
}

static void drain_close(TestingT *t, HttpResponse *res, const Error *want) {
    Error err = res->body.vt->closer.close(res->body.data);
    if (!drain_err_is(err, want)) {
        Error w = drain_want(want);
        testing_t_errorf_v(t, "Close = %v, want %v", err, w);
    }
}

static void drain_case(void *env, TestingT *t) {
    const DrainCase *tc = (const DrainCase *)env;
    need_tcp(t);
    Peer p;
    HttpTransport tr;
    if (!peer_listen(t, &p, &tr))
        return;
    PeerRT rt;
    PeerConn conn;
    Str path = {0};
    if (!peer_round_trip(t, &rt, &tr, BURROW_S("http://example.tld/"))) {
        peer_free(&p, &tr, NULL);
        return;
    }
    if (!peer_accept(t, &p, &conn)) {
        peer_free(&p, &tr, &rt);
        return;
    }
    if (!peer_read_request(t, &p, &conn, &path) ||
        !peer_write(t, &conn,
                    "HTTP/1.1 200 OK\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "\r\n"))
        goto out;
    HttpResponse *res = peer_rt_response(t, &rt);
    if (res == NULL)
        goto out;

    DrainRead dr;
    memset(&dr, 0, sizeof dr);
    arena_init(&dr.ar, heap_allocator(), 0);
    dr.res = res;
    if (tc->read) {
        sync_wait_group_go(&dr.wg, BURROW_FN(Func, drain_read_job, &dr));
        /* Wait for the Read to block on chunk data. */
        time_sleep(100 * TIME_MILLISECOND);
    }

    if (tc->close_early)
        drain_close(t, res, tc->want_close_err);

    if (tc->server_truncate)
        (void)net_tcp_conn_close_write(net_conn_as_tcp_conn(conn.conn));
    else
        (void)peer_write(t, &conn,
                         "0\r\n"
                         "\r\n");

    if (tc->read) {
        sync_wait_group_wait(&dr.wg);
        if (!drain_err_is(dr.err, tc->want_read_err)) {
            Error w = drain_want(tc->want_read_err);
            testing_t_errorf_v(t, "Read = %v, want %v", dr.err, w);
        }
    }

    if (tc->close_after_read)
        drain_close(t, res, tc->want_close_err);

    if (!tc->want_reuse)
        peer_want_closed(t, &conn);
    else
        peer_want_reuse(t, &p, &conn, &tr);
    arena_free(&dr.ar);

out:
    /* Not in Go: a round trip that is still waiting ends when the test's side
     * of the connection closes. */
    (void)net_tcp_conn_close_write(net_conn_as_tcp_conn(conn.conn));
    peer_free(&p, &tr, &rt);
    net_conn_free(conn.conn);
}

static void TestTransportResponseBodyDrainReadAndClose(TestingT *t) {
    for (Int i = 0; i < (Int)(sizeof drain_cases / sizeof drain_cases[0]); i++)
        (void)testing_t_run(
            t, cs(drain_cases[i].name),
            BURROW_FN(TestingTFunc, drain_case, (void *)(uintptr_t)&drain_cases[i]));
}

/* When a response body is automatically drained, it should read the trailer to
 * make sure the connection is reusable. Additionally, it should not populate
 * the Response.Trailer: by the time Close returns, Response belongs to the
 * caller and cannot be written to anymore by the draining goroutine. */
static void TestTransportResponseBodyDrainDropsTrailers(TestingT *t) {
    need_tcp(t);
    Peer p;
    HttpTransport tr;
    if (!peer_listen(t, &p, &tr))
        return;
    PeerRT rt;
    PeerConn conn;
    Str path = {0};
    if (!peer_round_trip(t, &rt, &tr, BURROW_S("http://example.tld/"))) {
        peer_free(&p, &tr, NULL);
        return;
    }
    if (!peer_accept(t, &p, &conn)) {
        peer_free(&p, &tr, &rt);
        return;
    }
    if (!peer_read_request(t, &p, &conn, &path) ||
        !peer_write(t, &conn,
                    "HTTP/1.1 200 OK\r\n"
                    "Transfer-Encoding: chunked\r\n"
                    "Trailer: X-Test\r\n"
                    "\r\n"
                    "5\r\n"
                    "hello\r\n"))
        goto out;
    HttpResponse *res = peer_rt_response(t, &rt);
    if (res == NULL)
        goto out;

    Error err = res->body.vt->closer.close(res->body.data);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Close = %v, want nil", err);
    if (!peer_write(t, &conn,
                    "0\r\n"
                    "X-Test: v\r\n"
                    "\r\n"))
        goto out;
    peer_wait_idle(&tr);

    /* Trailer must not be populated, to avoid racy write, and to be
     * consistent with how trailers are normally empty when one does not
     * read to EOF. */
    Str got = http_header_get(res->trailer, BURROW_S("X-Test"));
    if (got.len != 0)
        testing_t_errorf_v(t, "drained response body unexpectedly has populated "
                              "trailers");

    /* The trailer must still have been consumed from the connection, or
     * the connection would not be reusable. */
    peer_want_reuse(t, &p, &conn, &tr);

out:
    (void)net_tcp_conn_close_write(net_conn_as_tcp_conn(conn.conn));
    peer_free(&p, &tr, &rt);
    net_conn_free(conn.conn);
}

/* TestTransportResponseWriteAfterEarlyClose came with go1.27.2 too. */

static void serve_hello(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    Error err = BURROW_NO_ERROR;
    (void)write_str(w, cs("hello"), &err);
}

/* After Body.Close returns, the Response belongs to the caller. readLoop
 * used to read resp.ContentLength after letting Close return; run with
 * -race. */
static void TestTransportResponseWriteAfterEarlyClose(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, serve_hello, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&hf));
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(httptest_server_client(ts), ts->url, &err);
    if (res == NULL) {
        httptest_server_free(ts);
        testing_t_fatal_v(t, err);
    }
    err = res->body.vt->closer.close(res->body.data);
    if (BURROW_FAILED(err)) {
        http_response_free(res);
        httptest_server_free(ts);
        testing_t_fatal_v(t, err);
    }
    res->content_length = 0;
    http_response_free(res);
    httptest_server_free(ts);
}

#define TESTS(X)                                                                       \
    X(TestTransportKeepAlives)                                                         \
    X(TestTransportConnectionCloseOnResponse)                                          \
    X(TestTransportConnectionCloseOnRequest)                                           \
    X(TestTransportConnectionCloseOnRequestDisableKeepAlive)                           \
    X(TestTransportReadToEndReusesConn)                                                \
    X(TestTransportHeadResponses)                                                      \
    X(TestTransportHeadChunkedResponse)                                                \
    X(TestRoundTripGzip)                                                               \
    X(TestTransportResponseHeaderTimeout)                                              \
    X(TestTransportCloseResponseBody)                                                  \
    X(TestTransportAltProto)                                                           \
    X(TestTransportNoHost)                                                             \
    X(TestTransportResponseHeaderLength)                                               \
    X(TestTransportRejectsAlphaPort)                                                   \
    X(TestTransportCONNECTRejected)                                                    \
    X(TestTransportResponseBodyDrainReadAndClose)                                      \
    X(TestTransportResponseBodyDrainDropsTrailers)                                     \
    X(TestTransportResponseWriteAfterEarlyClose)
TESTING_MAIN(TESTS)
