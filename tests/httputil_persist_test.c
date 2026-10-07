/* Tests for the ServerConn and ClientConn of Go's
 * src/net/http/httputil/persist.go.
 * Go source: go1.27.1.
 *
 * Go has no tests for persist.go, so these are burrow's own. They check what
 * the Go code does: the order requests and responses go in, the body of one
 * request read off the connection before the next is read, the errors once
 * either side says the connection is done, and the whole URL a proxy conn
 * puts in the request line.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httputil.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/testing.h"

#include <string.h>

/* t.Fatalf, with a return the analyzer can see. */
#define FATALF(...)                                                                    \
    do {                                                                               \
        testing_t_fatalf_v(t, __VA_ARGS__);                                            \
        return;                                                                        \
    } while (0)

/* A response with body as its body, which has to last until it is written. */
typedef struct Reply {
    HttpResponse resp;
    StringsReader sr;
    IoNopCloser nc;
} Reply;

static void reply_init(Reply *r, Alloc *a, Str body, bool close) {
    memset(r, 0, sizeof *r);
    strings_reader_reset(&r->sr, body);
    r->nc = io_nop_closer(strings_reader_as_io_reader(&r->sr));
    r->resp.status_code = 200;
    r->resp.proto_major = 1;
    r->resp.proto_minor = 1;
    r->resp.header = http_header_make(a);
    r->resp.body = io_nop_closer_as_io_read_closer(&r->nc);
    r->resp.content_length = body.len;
    r->resp.close = close;
}

/* A client that writes reqs as they are and then reads everything that
 * comes back, until the server closes the connection. */
typedef struct RawClient {
    NetConn c;
    Str reqs;
    Slice got;
    Error err;
} RawClient;

static void raw_client(void *env) {
    RawClient *rc = (RawClient *)env;
    (void)io_write_string(net_conn_as_io_writer(rc->c), rc->reqs, &rc->err);
    if (BURROW_FAILED(rc->err))
        return;
    rc->got = io_read_all(heap_allocator(), net_conn_as_io_reader(rc->c), &rc->err);
}

/* Reads a response from br and checks its body and close. */
static void check_reply(TestingT *t, Alloc *a, BufioReader *br, const char *body,
                        bool close) {
    Error err;
    HttpResponse *res = http_read_response(a, br, NULL, &err);
    if (res == NULL)
        FATALF("ReadResponse: %v", err);
    Slice b = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "reading the body: %v", err);
    CHECK_INT_EQ(res->status_code, 200);
    if (!str_eq(str_from_bytes(b.p, b.len), str_from_cstr(body)))
        testing_t_errorf_v(t, "body = %q, want %q", str_from_bytes(b.p, b.len), body);
    if (res->close != close)
        testing_t_errorf_v(t, "close = %v, want %v", res->close, close);
    http_response_free(res);
}

/* Two requests sent at once, the first with a body nothing reads and the
 * second asking to close, and the answers to both. */
static void TestServerConnPipelined(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    NetConn cli, srv;
    net_pipe(heap_allocator(), &cli, &srv);
    if (srv.vt == NULL) {
        arena_free(&ar);
        FATALF("net_pipe failed");
    }
    RawClient rc;
    memset(&rc, 0, sizeof rc);
    rc.c = cli;
    rc.reqs = BURROW_S("POST /one HTTP/1.1\r\nHost: x\r\nContent-Length: 5\r\n\r\nhello"
                       "GET /two HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    if (!sync_wait_group_go(&wg, BURROW_FN(Func, raw_client, &rc))) {
        net_pipe_free(cli);
        arena_free(&ar);
        FATALF("no goroutine");
    }
    HttputilServerConn *sc = httputil_new_server_conn(heap_allocator(), srv, NULL);
    if (sc == NULL) {
        net_conn_free(srv);
        sync_wait_group_wait(&wg);
        net_pipe_free(cli);
        arena_free(&ar);
        FATALF("NewServerConn failed");
    }

    Error err;
    HttpRequest *r1 = httputil_server_conn_read(sc, &err);
    HttpRequest *r2 = NULL;
    if (r1 == NULL) {
        testing_t_errorf_v(t, "Read #1: %v", err);
        goto done;
    }
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Read #1 error = %v, want nil", err);
    if (!str_eq(r1->url->path, BURROW_S("/one")))
        testing_t_errorf_v(t, "Read #1 path = %q, want /one", r1->url->path);
    CHECK_INT_EQ(httputil_server_conn_pending(sc), 1);

    /* The second read has to get past the body of the first. */
    r2 = httputil_server_conn_read(sc, &err);
    if (r2 == NULL) {
        testing_t_errorf_v(t, "Read #2: %v", err);
        goto done;
    }
    if (!errors_is(err, httputil_err_persist_eof))
        testing_t_errorf_v(t, "Read #2 error = %v, want %v", err,
                           httputil_err_persist_eof);
    if (!str_eq(r2->url->path, BURROW_S("/two")))
        testing_t_errorf_v(t, "Read #2 path = %q, want /two", r2->url->path);
    CHECK_INT_EQ(httputil_server_conn_pending(sc), 2);

    Reply rep;
    reply_init(&rep, a, BURROW_S("one"), false);
    err = httputil_server_conn_write(sc, r1, &rep.resp);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Write #1: %v", err);
    CHECK_INT_EQ(httputil_server_conn_pending(sc), 1);
    reply_init(&rep, a, BURROW_S("two"), true);
    err = httputil_server_conn_write(sc, r2, &rep.resp);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Write #2: %v", err);
    CHECK_INT_EQ(httputil_server_conn_pending(sc), 0);

    /* The client is done, so there is nothing more to read. A Read that gets no
     * request waits for the earlier responses to go out, so it comes after the
     * writes. */
    HttpRequest *r3 = httputil_server_conn_read(sc, &err);
    if (r3 != NULL || !errors_is(err, httputil_err_persist_eof))
        testing_t_errorf_v(t, "Read #3 = %p, %v; want nil, %v", (void *)r3, err,
                           httputil_err_persist_eof);

    /* A request the conn did not read, or has answered, is not in its pipeline. */
    HttpRequest other;
    memset(&other, 0, sizeof other);
    err = httputil_server_conn_write(sc, &other, &rep.resp);
    if (!errors_is(err, httputil_err_pipeline))
        testing_t_errorf_v(t, "Write of another request = %v, want %v", err,
                           httputil_err_pipeline);

done:
    err = httputil_server_conn_close(sc);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Close: %v", err);
    sync_wait_group_wait(&wg);
    httputil_server_conn_free(sc);
    net_pipe_free(cli);
    if (r2 != NULL) {
        if (BURROW_FAILED(rc.err))
            testing_t_errorf_v(t, "client: %v", rc.err);
        StringsReader sr;
        strings_reader_reset(&sr, str_from_bytes(rc.got.p, rc.got.len));
        BufioReader *br = bufio_new_reader(a, strings_reader_as_io_reader(&sr));
        if (br == NULL) {
            testing_t_errorf_v(t, "NewReader failed");
        } else {
            check_reply(t, a, br, "one", false);
            check_reply(t, a, br, "two", true);
        }
    }
    if (rc.got.p != NULL)
        mem_free(heap_allocator(), rc.got.p, (size_t)rc.got.cap, 1);
    arena_free(&ar);
}

/* What a server conn gives once the user has closed it. */
static void TestServerConnClosed(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    NetConn cli, srv;
    net_pipe(heap_allocator(), &cli, &srv);
    if (srv.vt == NULL) {
        arena_free(&ar);
        FATALF("net_pipe failed");
    }
    RawClient rc;
    memset(&rc, 0, sizeof rc);
    rc.c = cli;
    rc.reqs = BURROW_S("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    if (!sync_wait_group_go(&wg, BURROW_FN(Func, raw_client, &rc))) {
        net_pipe_free(cli);
        arena_free(&ar);
        FATALF("no goroutine");
    }
    HttputilServerConn *sc = httputil_new_server_conn(heap_allocator(), srv, NULL);
    if (sc == NULL) {
        net_conn_free(srv);
        sync_wait_group_wait(&wg);
        net_pipe_free(cli);
        arena_free(&ar);
        FATALF("NewServerConn failed");
    }

    Error err;
    HttpRequest *req = httputil_server_conn_read(sc, &err);
    if (req == NULL)
        testing_t_errorf_v(t, "Read: %v", err);
    err = httputil_server_conn_close(sc);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Close: %v", err);
    sync_wait_group_wait(&wg);

    if (req != NULL) {
        Reply rep;
        reply_init(&rep, a, BURROW_S("x"), false);
        err = httputil_server_conn_write(sc, req, &rep.resp);
        if (!errors_is(err, httputil_err_closed))
            testing_t_errorf_v(t, "Write after Close = %v, want %v", err,
                               httputil_err_closed);
    }
    HttpRequest *again = httputil_server_conn_read(sc, &err);
    if (again != NULL || !BURROW_FAILED(err) ||
        !str_eq(error_text(err), BURROW_S("i/o operation on closed connection")))
        testing_t_errorf_v(t, "Read after Close = %p, %v; want nil and %q",
                           (void *)again, err, "i/o operation on closed connection");
    CHECK(
        BURROW_FAILED(httputil_err_closed) &&
        str_eq(error_text(httputil_err_closed), BURROW_S("connection closed by user")));
    CHECK(errors_as(httputil_err_persist_eof, TYPE_HTTP_PROTOCOL_ERROR) != NULL);

    httputil_server_conn_free(sc);
    net_pipe_free(cli);
    if (rc.got.p != NULL)
        mem_free(heap_allocator(), rc.got.p, (size_t)rc.got.cap, 1);
    arena_free(&ar);
}

/* A server conn on a goroutine that answers each request with its path, and
 * closes once a request asks it to. */
typedef struct EchoServer {
    HttputilServerConn *sc;
    Arena ar; /* the goroutine's own */
    int n;
    Error err;
} EchoServer;

static void echo_server(void *env) {
    EchoServer *es = (EchoServer *)env;
    Alloc *a = arena_allocator(&es->ar);
    for (;;) {
        Error err;
        HttpRequest *req = httputil_server_conn_read(es->sc, &err);
        if (req == NULL)
            return;
        es->n++;
        Reply rep;
        reply_init(&rep, a, str_clone(a, req->url->path), req->close);
        Error werr = httputil_server_conn_write(es->sc, req, &rep.resp);
        if (BURROW_FAILED(werr)) {
            es->err = error_retain(a, werr);
            return;
        }
        if (BURROW_FAILED(err))
            return;
    }
}

static HttpRequest *new_get(TestingT *t, const char *url) {
    Error err;
    HttpRequest *req =
        http_new_request(heap_allocator(), BURROW_S("GET"), str_from_cstr(url),
                         (IoReader){NULL, NULL}, &err);
    if (req == NULL)
        testing_t_errorf_v(t, "NewRequest(%q): %v", url, err);
    return req;
}

/* The body of res, read to its end. */
static Str read_body(TestingT *t, Alloc *a, HttpResponse *res) {
    Error err;
    Slice b = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "reading the body: %v", err);
    return str_from_bytes(b.p, b.len);
}

/* A client conn against a server conn: a request and its answer, then one
 * that says it is the last, and what the client conn gives after that. */
static void TestClientConn(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    NetConn cli, srv;
    net_pipe(heap_allocator(), &cli, &srv);
    if (srv.vt == NULL) {
        arena_free(&ar);
        FATALF("net_pipe failed");
    }
    EchoServer es;
    memset(&es, 0, sizeof es);
    arena_init(&es.ar, heap_allocator(), 0);
    es.sc = httputil_new_server_conn(heap_allocator(), srv, NULL);
    HttputilClientConn *cc = httputil_new_client_conn(heap_allocator(), cli, NULL);
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    if (es.sc == NULL || cc == NULL ||
        !sync_wait_group_go(&wg, BURROW_FN(Func, echo_server, &es))) {
        httputil_server_conn_free(es.sc);
        httputil_client_conn_free(cc);
        net_pipe_free(cli);
        arena_free(&es.ar);
        arena_free(&ar);
        FATALF("setting up failed");
    }

    HttpRequest *r1 = new_get(t, "http://example.com/a");
    HttpRequest *r2 = new_get(t, "http://example.com/b");
    HttpRequest *r3 = new_get(t, "http://example.com/c");
    if (r1 == NULL || r2 == NULL || r3 == NULL)
        goto done;
    r2->close = true;

    Error err;
    HttpResponse *res = httputil_client_conn_do(cc, r1, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "Do #1: %v", err);
        goto done;
    }
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Do #1 error = %v, want nil", err);
    Str body = read_body(t, a, res);
    if (!str_eq(body, BURROW_S("/a")))
        testing_t_errorf_v(t, "Do #1 body = %q, want /a", body);
    CHECK_INT_EQ(httputil_client_conn_pending(cc), 0);

    err = httputil_client_conn_write(cc, r2);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Write #2: %v", err);
        goto done;
    }
    CHECK_INT_EQ(httputil_client_conn_pending(cc), 1);
    /* A request that was never written has no response to read. */
    HttpResponse *none = httputil_client_conn_read(cc, r3, &err);
    if (none != NULL || !errors_is(err, httputil_err_pipeline))
        testing_t_errorf_v(t, "Read of an unwritten request = %p, %v; want nil, %v",
                           (void *)none, err, httputil_err_pipeline);
    res = httputil_client_conn_read(cc, r2, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "Read #2: %v", err);
        goto done;
    }
    if (!errors_is(err, httputil_err_persist_eof))
        testing_t_errorf_v(t, "Read #2 error = %v, want %v", err,
                           httputil_err_persist_eof);
    if (!res->close)
        testing_t_errorf_v(t, "Read #2 close = false, want true");
    body = read_body(t, a, res);
    if (!str_eq(body, BURROW_S("/b")))
        testing_t_errorf_v(t, "Read #2 body = %q, want /b", body);
    CHECK_INT_EQ(httputil_client_conn_pending(cc), 0);

    /* The server said that was the last, so nothing more goes out. */
    err = httputil_client_conn_write(cc, r3);
    if (!errors_is(err, httputil_err_persist_eof))
        testing_t_errorf_v(t, "Write #3 = %v, want %v", err, httputil_err_persist_eof);

done:
    (void)httputil_client_conn_close(cc);
    (void)httputil_server_conn_close(es.sc);
    sync_wait_group_wait(&wg);
    if (BURROW_FAILED(es.err))
        testing_t_errorf_v(t, "server: %v", es.err);
    CHECK_INT_EQ(es.n, 2);
    httputil_client_conn_free(cc);
    httputil_server_conn_free(es.sc);
    http_request_free(r1);
    http_request_free(r2);
    http_request_free(r3);
    net_pipe_free(cli);
    arena_free(&es.ar);
    arena_free(&ar);
}

typedef struct ProxyWrite {
    HttputilClientConn *cc;
    HttpRequest *req;
    Error err;
} ProxyWrite;

static void proxy_write(void *env) {
    ProxyWrite *pw = (ProxyWrite *)env;
    pw->err = httputil_client_conn_write(pw->cc, pw->req);
}

/* A proxy client conn puts the whole URL in the request line. */
static void TestProxyClientConn(TestingT *t) {
    NetConn cli, srv;
    net_pipe(heap_allocator(), &cli, &srv);
    if (srv.vt == NULL)
        FATALF("net_pipe failed");
    ProxyWrite pw;
    memset(&pw, 0, sizeof pw);
    pw.cc = httputil_new_proxy_client_conn(heap_allocator(), cli, NULL);
    pw.req = new_get(t, "http://example.com/p?q=1");
    HttputilServerConn *sc = httputil_new_server_conn(heap_allocator(), srv, NULL);
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    if (pw.cc == NULL || pw.req == NULL || sc == NULL ||
        !sync_wait_group_go(&wg, BURROW_FN(Func, proxy_write, &pw))) {
        httputil_client_conn_free(pw.cc);
        http_request_free(pw.req);
        httputil_server_conn_free(sc);
        net_pipe_free(cli);
        FATALF("setting up failed");
    }

    Error err;
    HttpRequest *got = httputil_server_conn_read(sc, &err);
    if (got == NULL)
        testing_t_errorf_v(t, "Read: %v", err);
    else if (!str_eq(got->request_uri, BURROW_S("http://example.com/p?q=1")))
        testing_t_errorf_v(t, "request URI = %q, want %q", got->request_uri,
                           "http://example.com/p?q=1");
    sync_wait_group_wait(&wg);
    if (BURROW_FAILED(pw.err))
        testing_t_errorf_v(t, "Write: %v", pw.err);
    CHECK_INT_EQ(httputil_client_conn_pending(pw.cc), 1);

    (void)httputil_client_conn_close(pw.cc);
    (void)httputil_server_conn_close(sc);
    httputil_client_conn_free(pw.cc);
    httputil_server_conn_free(sc);
    http_request_free(pw.req);
    net_pipe_free(cli);
}

#define TESTS(X)                                                                       \
    X(TestServerConnPipelined)                                                         \
    X(TestServerConnClosed)                                                            \
    X(TestClientConn)                                                                  \
    X(TestProxyClientConn)

TESTING_MAIN(TESTS)
