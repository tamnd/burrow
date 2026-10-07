/* The transport over a real connection: keep-alives, Connection: close, HEAD,
 * gzip, the header timeout and limit, closing a body early, and the errors it
 * gives before it dials. Ported from Go's src/net/http/transport_test.go,
 * go1.27.1, in its HTTP/1 mode.
 *
 * Go's hostPortHandler also writes the address of the net.Conn, in case the
 * kernel hands out the same port again at once. That isn't something a test
 * can get at here, so the tests go by the remote address alone. */

#include "check.h"

#include "../src/net/http_internal.h"

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
    Fetcher f = {t, host_port_server(&hf), {0}};
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
    Fetcher f = {t, host_port_server(&hf), {0}};
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
    Fetcher f = {t, host_port_server(&hf), {0}};
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
    Fetcher f = {t, host_port_server(&hf), {0}};
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
    X(TestTransportRejectsAlphaPort)
TESTING_MAIN(TESTS)
