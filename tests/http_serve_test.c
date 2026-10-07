/* The HTTP/1 server: what it sends back for a request, its errors, its hooks,
 * hijacking, Shutdown and Close, and the handlers that wrap others.
 *
 * Most cases follow Go's src/net/http/serve_test.go, which feeds a request
 * through a connection that lives in memory and looks at the bytes that come
 * back. The responses here were made by Go 1.27.1 over net.Pipe with the same
 * handlers, with the Date lines taken out, since they change. The ones that
 * need a real socket skip where TCP is not here yet.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/url.h"
#include "burrow/panic.h"
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

static Str cs(const char *s) {
    return str_from_cstr(s);
}

static Slice bytes_of(const char *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

static void write_str(HttpResponseWriter w, const char *s) {
    Error err;
    (void)http_response_writer_write(w, bytes_of(s, (Int)strlen(s)), &err);
}

/* ------------------------------------------------- one connection, in memory */

/* oneConnListener: gives its connection once, and then io_eof. */
typedef struct OneConnListener {
    NetConn conn;
    bool done;
} OneConnListener;

static NetConn one_accept(void *self, Error *err) {
    OneConnListener *l = (OneConnListener *)self;
    if (l->done) {
        *err = io_eof;
        return (NetConn){NULL, NULL};
    }
    l->done = true;
    *err = BURROW_NO_ERROR;
    return l->conn;
}

static Error one_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static NetAddr one_addr(void *self) {
    OneConnListener *l = (OneConnListener *)self;
    return l->conn.vt->local_addr(l->conn.data);
}

static const NetListenerVT one_vt = {{NULL, one_close}, one_accept, one_addr};

typedef struct ServeJob {
    HttpServer *srv;
    NetListener l;
    Error err;
} ServeJob;

static void serve_job(void *env) {
    ServeJob *j = (ServeJob *)env;
    j->err = http_server_serve(j->srv, j->l);
}

typedef struct ClientJob {
    NetConn c;
    Str req;
    char *got;
    size_t cap;
    size_t n;
} ClientJob;

static void write_job(void *env) {
    ClientJob *j = (ClientJob *)env;
    Error err;
    (void)j->c.vt->writer.write(j->c.data, bytes_of((const char *)j->req.p, j->req.len),
                                &err);
}

/* Reads to the end, which is when the server closes its end, and then closes
 * this one so that a write still waiting gives up. */
static void read_job(void *env) {
    ClientJob *j = (ClientJob *)env;
    (void)j->c.vt->set_read_deadline(j->c.data, time_add(time_now(), 10 * TIME_SECOND));
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Int n = 0;
        if (j->n < j->cap - 1)
            n = j->c.vt->reader.read(
                j->c.data, bytes_of(j->got + j->n, (Int)(j->cap - 1 - j->n)), &err);
        else
            break;
        j->n += (size_t)n;
        if (BURROW_FAILED(err))
            break;
    }
    j->got[j->n] = '\0';
    (void)j->c.vt->closer.close(j->c.data);
}

/* Takes the "Date: ..." lines out of s. */
static void strip_dates(char *s) {
    char *out = s;
    const char *p = s;
    while (*p != '\0') {
        const char *eol = strstr(p, "\r\n");
        size_t n = eol == NULL ? strlen(p) : (size_t)(eol - p) + 2;
        if (strncmp(p, "Date: ", 6) != 0) {
            memmove(out, p, n);
            out += n;
        }
        p += n;
    }
    *out = '\0';
}

/* Serves req with srv on a connection that lives in memory, and gives what
 * came back without its Date lines in got and the first line the server
 * logged in log_line. */
static void exchange(TestingT *t, HttpServer *srv, const char *req, char *got,
                     size_t cap, char *log_line, size_t log_cap) {
    got[0] = '\0';
    log_line[0] = '\0';
    Alloc *h = heap_allocator();
    BytesBuffer *lb = bytes_new_buffer(h, slice_from(NULL, 0, 0, TYPE_BYTE));
    LogLogger *lg =
        lb == NULL ? NULL : log_new(h, bytes_buffer_as_io_writer(lb), cs(""), 0);
    if (lg == NULL) {
        testing_t_errorf_v(t, "no memory for the log");
        bytes_buffer_free(lb);
        return;
    }
    srv->error_log = lg;

    NetConn sc;
    NetConn cc;
    net_pipe(h, &sc, &cc);
    if (sc.data == NULL) {
        testing_t_errorf_v(t, "no memory for the pipe");
        log_logger_free(h, lg);
        bytes_buffer_free(lb);
        return;
    }
    OneConnListener ol = {sc, false};
    ServeJob sj = {srv, {&one_vt, &ol}, BURROW_NO_ERROR};
    ClientJob cj = {cc, cs(req), got, cap, 0};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, serve_job, &sj));
    sync_wait_group_go(&wg, BURROW_FN(Func, write_job, &cj));
    sync_wait_group_go(&wg, BURROW_FN(Func, read_job, &cj));
    sync_wait_group_wait(&wg);
    http_server_free(srv);
    CHECK(errors_is(sj.err, io_eof));
    net_pipe_free(sc);
    strip_dates(got);

    Str ls = bytes_buffer_string(lb, h);
    size_t n = 0;
    while (n < (size_t)ls.len && n < log_cap - 1 && ls.p[n] != '\n')
        n++;
    memcpy(log_line, ls.p, n);
    log_line[n] = '\0';
    mem_free(h, (void *)(uintptr_t)ls.p, (size_t)ls.len, 1);
    srv->error_log = NULL;
    log_logger_free(h, lg);
    bytes_buffer_free(lb);
}

/* ------------------------------------------------------------ the handlers */

/* A handler has no TestingT, so it notes the first check that failed here,
 * and the test looks after each exchange. */
static const char *handler_failed;

#define HCHECK(cond)                                                                   \
    do {                                                                               \
        if (!(cond) && handler_failed == NULL)                                         \
            handler_failed = #cond;                                                    \
    } while (0)

static void serve_hello(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    write_str(w, "hello");
}

static void serve_flushed(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    write_str(w, "a");
    HttpResponseController rc = http_new_response_controller(w);
    HCHECK(!BURROW_FAILED(http_response_controller_flush(&rc)));
    write_str(w, "b");
}

static void serve_echo(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Error err;
    Slice b = io_read_all(heap_allocator(),
                          (IoReader){&r->body.vt->reader, r->body.data}, &err);
    (void)http_response_writer_write(w, b, &err);
    mem_free(heap_allocator(), b.p, (size_t)b.cap, 1);
}

static void serve_panic(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)w;
    (void)r;
    panic_str(cs("boom"));
}

static void serve_abort(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)w;
    (void)r;
    panic(BURROW_ANY(TYPE_ERROR, (void *)(uintptr_t)&http_err_abort_handler));
}

static void serve_max_bytes(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Error err;
    Slice b = io_read_all(heap_allocator(),
                          (IoReader){&r->body.vt->reader, r->body.data}, &err);
    mem_free(heap_allocator(), b.p, (size_t)b.cap, 1);
    if (BURROW_FAILED(err)) {
        const HttpMaxBytesError *me = errors_as(err, TYPE_HTTP_MAX_BYTES_ERROR);
        HCHECK(me != NULL && me->limit == 2);
        http_error(w, error_text(err), HTTP_STATUS_REQUEST_ENTITY_TOO_LARGE);
        return;
    }
    write_str(w, "fine");
}

static Error late_write_err;

static void serve_slow(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)chan_recv(context_done(http_request_context(r)), NULL);
    (void)http_response_writer_write(w, bytes_of("late", 4), &late_write_err);
}

static void serve_fast(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    (void)http_header_set(http_response_writer_header(w), cs("X-A"), cs("b"));
    http_response_writer_write_header(w, HTTP_STATUS_CREATED);
    write_str(w, "ok");
}

static void serve_raw_query(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Error err;
    Str q = r->url->raw_query;
    (void)http_response_writer_write(w, bytes_of((const char *)q.p, q.len), &err);
}

static void serve_trailer(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    HttpHeader h = http_response_writer_header(w);
    (void)http_header_set(h, cs("Trailer"), cs("X-T"));
    write_str(w, "a");
    (void)http_header_set(h, cs("X-T"), cs("v"));
    (void)http_header_set(h, cs("Trailer:X-U"), cs("u"));
}

static void serve_superfluous(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    http_response_writer_write_header(w, HTTP_STATUS_ACCEPTED);
    http_response_writer_write_header(w, 203);
}

static void serve_info(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    HttpHeader h = http_response_writer_header(w);
    (void)http_header_set(h, cs("Link"), cs("</a.css>"));
    http_response_writer_write_header(w, HTTP_STATUS_EARLY_HINTS);
    http_header_del(h, cs("Link"));
    write_str(w, "x");
}

static Error no_content_err;

static void serve_no_content(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    (void)http_header_set(http_response_writer_header(w), cs("Content-Length"),
                          cs("5"));
    http_response_writer_write_header(w, HTTP_STATUS_NO_CONTENT);
    (void)http_response_writer_write(w, bytes_of("x", 1), &no_content_err);
}

static Error too_much_err;

static void serve_too_much(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    (void)http_header_set(http_response_writer_header(w), cs("Content-Length"),
                          cs("2"));
    (void)http_response_writer_write(w, bytes_of("abc", 3), &too_much_err);
}

static void serve_te_identity(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    (void)http_header_set(http_response_writer_header(w), cs("Transfer-Encoding"),
                          cs("identity"));
    write_str(w, "abc");
}

static void serve_hijack(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    HttpResponseController rc = http_new_response_controller(w);
    BufioReadWriter buf;
    Error err;
    NetConn c = http_response_controller_hijack(&rc, &buf, &err);
    if (BURROW_FAILED(err)) {
        panic(BURROW_ANY(TYPE_ERROR, &err));
        return;
    }
    Error perr;
    Slice rest =
        bufio_reader_peek(buf.reader, bufio_reader_buffered(buf.reader), &perr);
    (void)fmt_fprintf_v(bufio_writer_as_io_writer(buf.writer), "raw %q",
                        str_from_bytes(rest.p, rest.len));
    (void)bufio_writer_flush(buf.writer);
    (void)c.vt->closer.close(c.data);
    bufio_reader_free(buf.reader);
    bufio_writer_free(buf.writer);

    /* Once it is hijacked, the writer is no more use. */
    Int n = http_response_writer_write(w, bytes_of("x", 1), &err);
    HCHECK(n == 0);
    HCHECK(errors_is(err, http_err_hijacked));
}

/* ------------------------------------------------------- the exchanges */

typedef enum Wrap {
    WRAP_NONE,
    WRAP_MAX_BYTES,
    WRAP_TIMEOUT,
    WRAP_TIMEOUT_MSG,
    WRAP_TIMEOUT_FAST,
    WRAP_SEMICOLONS,
} Wrap;

typedef struct Case {
    const char *name;
    void (*fn)(void *env, HttpResponseWriter w, HttpRequest *r);
    Int max_header_bytes;
    const char *req;
    const char *want;
    const char *want_log; /* the start of the log's first line */
    Wrap wrap;
    bool no_options;
} Case;

#define OK_HELLO                                                                       \
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain; "               \
    "charset=utf-8\r\n\r\nhello"
#define OK_HELLO_CLOSE                                                                 \
    "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain; "               \
    "charset=utf-8\r\nConnection: close\r\n\r\nhello"
#define GET_CLOSE "GET / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n"

static const Case cases[] = {
    {.name = "http10",
     .fn = serve_hello,
     .req = "GET / HTTP/1.0\r\n\r\n",
     .want = "HTTP/1.0 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain; "
             "charset=utf-8\r\n\r\nhello",
     .want_log = ""},
    {.name = "keepalive",
     .fn = serve_hello,
     .req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n" GET_CLOSE,
     .want = OK_HELLO OK_HELLO_CLOSE,
     .want_log = ""},
    {.name = "http10 keep-alive",
     .fn = serve_hello,
     .req = "GET / HTTP/1.0\r\nConnection: keep-alive\r\n\r\nGET / HTTP/1.0\r\n\r\n",
     .want = "HTTP/1.0 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain; "
             "charset=utf-8\r\nConnection: keep-alive\r\n\r\nhello"
             "HTTP/1.0 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain; "
             "charset=utf-8\r\n\r\nhello",
     .want_log = ""},
    {.name = "chunked",
     .fn = serve_flushed,
     .req = GET_CLOSE,
     .want =
         "HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: "
         "close\r\nTransfer-Encoding: chunked\r\n\r\n1\r\na\r\n1\r\nb\r\n0\r\n\r\n",
     .want_log = ""},
    {.name = "head",
     .fn = serve_hello,
     .req = "HEAD / HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
     .want = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Type: text/plain; "
             "charset=utf-8\r\nConnection: close\r\n\r\n",
     .want_log = ""},
    {.name = "no host",
     .fn = serve_hello,
     .req = "GET / HTTP/1.1\r\n\r\n",
     .want = "HTTP/1.1 400 Bad Request: missing required Host header\r\nContent-Type: "
             "text/plain; charset=utf-8\r\nConnection: close\r\n\r\n400 Bad Request: "
             "missing "
             "required Host header",
     .want_log = ""},
    {.name = "bad host",
     .fn = serve_hello,
     .req = "GET / HTTP/1.1\r\nHost: a b\r\n\r\n",
     .want =
         "HTTP/1.1 400 Bad Request: malformed Host header\r\nContent-Type: text/plain; "
         "charset=utf-8\r\nConnection: close\r\n\r\n400 Bad Request: malformed Host "
         "header",
     .want_log = ""},
    {.name = "version",
     .fn = serve_hello,
     .req = "GET / HTTP/3.0\r\nHost: x\r\n\r\n",
     .want =
         "HTTP/1.1 505 HTTP Version Not Supported: unsupported protocol "
         "version\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: "
         "close\r\n\r\n505 HTTP Version Not Supported: unsupported protocol version",
     .want_log = ""},
    {.name = "garbage",
     .fn = serve_hello,
     .req = "NOT HTTP\r\n\r\n",
     .want = "HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain; "
             "charset=utf-8\r\nConnection: "
             "close\r\n\r\n400 Bad Request",
     .want_log = ""},
    {.name = "too large",
     .fn = serve_hello,
     .max_header_bytes = 100,
     .want =
         "HTTP/1.1 431 Request Header Fields Too Large\r\nContent-Type: text/plain; "
         "charset=utf-8\r\nConnection: close\r\n\r\n431 Request Header Fields Too "
         "Large",
     .want_log = ""},
    {.name = "transfer encoding",
     .fn = serve_hello,
     .req = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: gzip\r\n\r\n",
     .want = "HTTP/1.1 501 Not Implemented\r\nContent-Type: text/plain; "
             "charset=utf-8\r\nConnection: close\r\n\r\nUnsupported transfer encoding",
     .want_log = ""},
    {.name = "100 continue",
     .fn = serve_echo,
     .req = "POST / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\nContent-Length: "
            "3\r\nConnection: close\r\n\r\nabc",
     .want =
         "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: "
         "3\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\nabc",
     .want_log = ""},
    {.name = "expect foo",
     .fn = serve_echo,
     .req = "POST / HTTP/1.1\r\nHost: x\r\nExpect: foo\r\nContent-Length: 3\r\n\r\nabc",
     .want = "HTTP/1.1 417 Expectation Failed\r\nConnection: close\r\nContent-Length: "
             "0\r\n\r\n",
     .want_log = ""},
    {.name = "unread body",
     .fn = serve_hello,
     .req = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 3\r\n\r\nabc" GET_CLOSE,
     .want = OK_HELLO OK_HELLO_CLOSE,
     .want_log = ""},
    {.name = "panic",
     .fn = serve_panic,
     .req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n",
     .want = "",
     .want_log = "http: panic serving pipe: boom"},
    {.name = "abort",
     .fn = serve_abort,
     .req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n",
     .want = "",
     .want_log = ""},
    {.name = "options",
     .fn = serve_hello,
     .req = "OPTIONS * HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
     .want = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
     .want_log = ""},
    {.name = "options off",
     .fn = serve_hello,
     .req = "OPTIONS * HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
     .want = OK_HELLO_CLOSE,
     .want_log = "",
     .no_options = true},
    {.name = "max bytes",
     .fn = serve_max_bytes,
     .req = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 6\r\n\r\nabcdef",
     .want =
         "HTTP/1.1 413 Request Entity Too Large\r\nConnection: close\r\nContent-Type: "
         "text/plain; charset=utf-8\r\nX-Content-Type-Options: "
         "nosniff\r\nContent-Length: "
         "29\r\n\r\nhttp: request body too large\n",
     .want_log = "",
     .wrap = WRAP_MAX_BYTES},
    {.name = "timeout",
     .fn = serve_slow,
     .req = GET_CLOSE,
     .want = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 77\r\nContent-Type: "
             "text/html; charset=utf-8\r\nConnection: "
             "close\r\n\r\n<html><head><title>Timeout</"
             "title></head><body><h1>Timeout</h1></body></html>",
     .want_log = "",
     .wrap = WRAP_TIMEOUT},
    {.name = "timeout message",
     .fn = serve_slow,
     .req = GET_CLOSE,
     .want = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 8\r\nContent-Type: "
             "text/plain; charset=utf-8\r\nConnection: close\r\n\r\ntoo slow",
     .want_log = "",
     .wrap = WRAP_TIMEOUT_MSG},
    {.name = "timeout fast",
     .fn = serve_fast,
     .req = GET_CLOSE,
     .want = "HTTP/1.1 201 Created\r\nX-A: b\r\nContent-Length: 2\r\nContent-Type: "
             "text/plain; "
             "charset=utf-8\r\nConnection: close\r\n\r\nok",
     .want_log = "",
     .wrap = WRAP_TIMEOUT_FAST},
    {.name = "semicolons",
     .fn = serve_raw_query,
     .req = "GET /?a=1;b=2 HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n",
     .want = "HTTP/1.1 200 OK\r\nContent-Length: 7\r\nContent-Type: text/plain; "
             "charset=utf-8\r\nConnection: close\r\n\r\na=1&b=2",
     .want_log = "",
     .wrap = WRAP_SEMICOLONS},
    {.name = "trailer",
     .fn = serve_trailer,
     .req = GET_CLOSE,
     .want = "HTTP/1.1 200 OK\r\nTrailer: X-T\r\nContent-Type: text/plain; "
             "charset=utf-8\r\nConnection: close\r\nTransfer-Encoding: "
             "chunked\r\n\r\n1\r\na\r\n0\r\nX-T: v\r\nX-U: u\r\n\r\n",
     .want_log = ""},
    {.name = "superfluous",
     .fn = serve_superfluous,
     .req = GET_CLOSE,
     .want = "HTTP/1.1 202 Accepted\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
     .want_log = "http: superfluous response.WriteHeader call from "},
    {.name = "informational",
     .fn = serve_info,
     .req = GET_CLOSE,
     .want =
         "HTTP/1.1 103 Early Hints\r\nLink: </a.css>\r\n\r\nHTTP/1.1 200 "
         "OK\r\nContent-Length: "
         "1\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\nx",
     .want_log = ""},
    {.name = "no content",
     .fn = serve_no_content,
     .req = GET_CLOSE,
     .want = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n",
     .want_log = ""},
    {.name = "too much",
     .fn = serve_too_much,
     .req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n",
     .want = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n",
     .want_log = ""},
    {.name = "identity",
     .fn = serve_te_identity,
     .req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n",
     .want = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nabc",
     .want_log = ""},
    {.name = "hijack",
     .fn = serve_hijack,
     .req = "GET / HTTP/1.1\r\nHost: x\r\n\r\nextra",
     .want = "raw \"extra\"",
     .want_log = ""},
};

static void TestServerResponsesMatchGo(TestingT *t) {
    enum { cap = 8192 };
    static char got[cap];
    static char big[6000];
    char log_line[256];
    int n = snprintf(big, sizeof big, "GET / HTTP/1.1\r\nHost: x\r\nX-Big: ");
    memset(big + n, 'a', 5000);
    memcpy(big + n + 5000, "\r\n\r\n", 5);

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const Case *c = &cases[i];
        HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, c->fn, NULL);
        HttpHandler h = http_handler_func_as_handler(&f);
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        switch (c->wrap) {
        case WRAP_MAX_BYTES:
            h = http_max_bytes_handler(a, h, 2);
            break;
        case WRAP_TIMEOUT:
            h = http_timeout_handler(a, h, 20 * TIME_MILLISECOND, cs(""));
            break;
        case WRAP_TIMEOUT_MSG:
            h = http_timeout_handler(a, h, 20 * TIME_MILLISECOND, cs("too slow"));
            break;
        case WRAP_TIMEOUT_FAST:
            h = http_timeout_handler(a, h, TIME_SECOND, cs(""));
            break;
        case WRAP_SEMICOLONS:
            h = http_allow_query_semicolons(a, h);
            break;
        case WRAP_NONE:
        default:
            break;
        }
        HttpServer srv = {.handler = h,
                          .max_header_bytes = c->max_header_bytes,
                          .disable_general_options_handler = c->no_options};
        handler_failed = NULL;
        late_write_err = BURROW_NO_ERROR;
        no_content_err = BURROW_NO_ERROR;
        too_much_err = BURROW_NO_ERROR;
        exchange(t, &srv, c->req != NULL ? c->req : big, got, cap, log_line,
                 sizeof log_line);
        if (strcmp(got, c->want) != 0)
            testing_t_errorf_v(t, "%s: got %q, want %q", cs(c->name), cs(got),
                               cs(c->want));
        if (strncmp(log_line, c->want_log, strlen(c->want_log)) != 0 ||
            (c->want_log[0] == '\0' && log_line[0] != '\0'))
            testing_t_errorf_v(t, "%s: logged %q, want %q", cs(c->name), cs(log_line),
                               cs(c->want_log));
        if (handler_failed != NULL)
            testing_t_errorf_v(t, "%s: in the handler, %s", cs(c->name),
                               cs(handler_failed));
        if (c->fn == serve_slow)
            CHECK(errors_is(late_write_err, http_err_handler_timeout));
        if (c->fn == serve_no_content)
            CHECK(errors_is(no_content_err, http_err_body_not_allowed));
        if (c->fn == serve_too_much)
            CHECK(errors_is(too_much_err, http_err_content_length));
        arena_free(&ar);
    }
}

/* -------------------------------------------------------------- the hooks */

typedef struct StateLog {
    SyncMutex mu;
    HttpConnState states[8];
    Int n;
} StateLog;

static void record_state(void *env, NetConn c, HttpConnState s) {
    (void)c;
    StateLog *l = (StateLog *)env;
    sync_mutex_lock(&l->mu);
    if (l->n < 8)
        l->states[l->n++] = s;
    sync_mutex_unlock(&l->mu);
}

static void TestConnStateHookSeesEachState(TestingT *t) {
    char got[512];
    char log_line[128];
    StateLog sl = {0};
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_hello, NULL);
    HttpServer srv = {.handler = http_handler_func_as_handler(&f),
                      .conn_state = BURROW_FN(HttpConnStateFunc, record_state, &sl)};
    exchange(t, &srv, "GET / HTTP/1.1\r\nHost: x\r\n\r\n" GET_CLOSE, got, sizeof got,
             log_line, sizeof log_line);
    CHECK_STR_EQ(got, OK_HELLO OK_HELLO_CLOSE);
    static const HttpConnState want[] = {HTTP_STATE_NEW, HTTP_STATE_ACTIVE,
                                         HTTP_STATE_IDLE, HTTP_STATE_ACTIVE,
                                         HTTP_STATE_CLOSED};
    CHECK_INT_EQ(sl.n, 5);
    for (Int i = 0; i < sl.n && i < 5; i++)
        if (sl.states[i] != want[i])
            testing_t_errorf_v(t, "state %d: got %s, want %s", i,
                               http_conn_state_string(sl.states[i]),
                               http_conn_state_string(want[i]));
}

static void TestConnStateNames(TestingT *t) {
    (void)t;
    CHECK(str_eq(http_conn_state_string(HTTP_STATE_NEW), cs("new")));
    CHECK(str_eq(http_conn_state_string(HTTP_STATE_ACTIVE), cs("active")));
    CHECK(str_eq(http_conn_state_string(HTTP_STATE_IDLE), cs("idle")));
    CHECK(str_eq(http_conn_state_string(HTTP_STATE_HIJACKED), cs("hijacked")));
    CHECK(str_eq(http_conn_state_string(HTTP_STATE_CLOSED), cs("closed")));
}

static HttpServer *ctx_server;

static void serve_context(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Context ctx = http_request_context(r);
    Any s = context_value(ctx, http_server_context_key);
    Any la = context_value(ctx, http_local_addr_context_key);
    bool ok = s.t == TYPE_HTTP_SERVER && s.data == ctx_server && la.t != NULL;
    write_str(w, ok ? "yes" : "no");
}

static void TestTheRequestContextHasTheServerAndTheLocalAddr(TestingT *t) {
    char got[512];
    char log_line[128];
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_context, NULL);
    HttpServer srv = {.handler = http_handler_func_as_handler(&f)};
    ctx_server = &srv;
    exchange(t, &srv, GET_CLOSE, got, sizeof got, log_line, sizeof log_line);
    CHECK_STR_EQ(got,
                 "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nContent-Type: text/plain; "
                 "charset=utf-8\r\nConnection: close\r\n\r\nyes");
}

/* ------------------------------------------------- ResponseController */

static void TestAResponseControllerSaysWhatItCannotDo(TestingT *t) {
    (void)t;
    HttptestResponseRecorder *rec = httptest_new_recorder(heap_allocator());
    HttpResponseController rc = http_new_response_controller(
        httptest_response_recorder_as_response_writer(rec));
    CHECK(!BURROW_FAILED(http_response_controller_flush(&rc)));
    CHECK(rec->flushed);
    BufioReadWriter buf;
    Error err;
    NetConn c = http_response_controller_hijack(&rc, &buf, &err);
    CHECK(c.vt == NULL);
    CHECK(errors_is(err, http_err_not_supported));
    CHECK(errors_is(http_response_controller_set_read_deadline(&rc, time_now()),
                    http_err_not_supported));
    CHECK(errors_is(http_response_controller_set_write_deadline(&rc, time_now()),
                    http_err_not_supported));
    CHECK(errors_is(http_response_controller_enable_full_duplex(&rc),
                    http_err_not_supported));
    httptest_response_recorder_free(rec);
}

static void TestMaxBytesReaderStopsAtTheLimit(TestingT *t) {
    Alloc *h = heap_allocator();
    HttptestResponseRecorder *rec = httptest_new_recorder(h);
    StringsReader sr;
    strings_reader_reset(&sr, cs("abcdef"));
    IoNopCloser nc = io_nop_closer(strings_reader_as_io_reader(&sr));
    Arena ar;
    arena_init(&ar, NULL, 0);
    IoReadCloser r = http_max_bytes_reader(
        arena_allocator(&ar), httptest_response_recorder_as_response_writer(rec),
        io_nop_closer_as_io_read_closer(&nc), 4);
    Error err;
    Slice b = io_read_all(h, (IoReader){&r.vt->reader, r.data}, &err);
    CHECK_INT_EQ(b.len, 4);
    CHECK(memcmp(b.p, "abcd", 4) == 0);
    mem_free(h, b.p, (size_t)b.cap, 1);
    const HttpMaxBytesError *me = errors_as(err, TYPE_HTTP_MAX_BYTES_ERROR);
    CHECK(me != NULL && me->limit == 4);
    CHECK(str_eq(error_text(err), cs("http: request body too large")));
    arena_free(&ar);
    httptest_response_recorder_free(rec);
}

static void TestServerErrorTexts(TestingT *t) {
    (void)t;
    CHECK(str_eq(error_text(http_err_server_closed), cs("http: Server closed")));
    CHECK(str_eq(error_text(http_err_hijacked),
                 cs("http: connection has been hijacked")));
    CHECK(str_eq(error_text(http_err_content_length),
                 cs("http: wrote more than the declared Content-Length")));
    CHECK(str_eq(error_text(http_err_handler_timeout), cs("http: Handler timeout")));
    CHECK(str_eq(error_text(http_err_abort_handler), cs("net/http: abort Handler")));
    CHECK(str_eq(error_text(http_err_not_supported), cs("feature not supported")));
}

/* ------------------------------------------------- ListenAndServe errors */

static void TestListenAndServeSaysWhatIsWrongWithTheAddress(TestingT *t) {
    static const struct {
        const char *addr;
        const char *want;
    } tests[] = {
        {"127.0.0.1:nope", "listen tcp: lookup tcp/nope: unknown port"},
        {"127.0.0.1:99999", "listen tcp: address 99999: invalid port"},
        {"127.0.0.1", "listen tcp: address 127.0.0.1: missing port in address"},
    };
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_hello, NULL);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err =
            http_listen_and_serve(cs(tests[i].addr), http_handler_func_as_handler(&f));
        Str s = error_text(err);
        if (!str_eq(s, cs(tests[i].want)))
            testing_t_errorf_v(t, "%s: got %q, want %q", cs(tests[i].addr), s,
                               cs(tests[i].want));
    }
}

static void TestServeAfterCloseIsServerClosed(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_hello, NULL);
    HttpServer srv = {.handler = http_handler_func_as_handler(&f)};
    CHECK(!BURROW_FAILED(http_server_close(&srv)));
    CHECK(errors_is(http_server_listen_and_serve(&srv), http_err_server_closed));
    http_server_free(&srv);
}

/* ------------------------------------------------- Shutdown, over TCP */

static Byte loop4_bytes[4] = {127, 0, 0, 1};

static void on_shutdown(void *env) {
    sync_wait_group_done((SyncWaitGroup *)env);
}

/* Reads from c until got ends with tail. */
static bool read_until(NetTCPConn *c, char *got, size_t cap, const char *tail) {
    size_t n = 0;
    size_t tl = strlen(tail);
    while (n < cap - 1) {
        Error err = BURROW_NO_ERROR;
        Int k = net_tcp_conn_read(c, bytes_of(got + n, (Int)(cap - 1 - n)), &err);
        n += (size_t)k;
        got[n] = '\0';
        if (n >= tl && memcmp(got + n - tl, tail, tl) == 0)
            return true;
        if (BURROW_FAILED(err))
            return false;
    }
    return false;
}

static void TestShutdownClosesIdleConnectionsAndStopsServe(TestingT *t) {
    need_tcp(t);
    Alloc *h = heap_allocator();
    NetTCPAddr la = {slice_from(loop4_bytes, 4, 4, TYPE_BYTE), 0, BURROW_STR_EMPTY};
    Error err = BURROW_NO_ERROR;
    NetTCPListener *l = net_listen_tcp(h, cs("tcp"), &la, &err);
    if (l == NULL) {
        testing_t_fatalf_v(t, "listen: %v", err);
        return;
    }
    NetAddr addr = net_tcp_listener_addr(l);
    Int port = ((const NetTCPAddr *)addr.data)->port;

    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_hello, NULL);
    HttpServer srv = {.handler = http_handler_func_as_handler(&f)};
    SyncWaitGroup hooks = {0};
    sync_wait_group_add(&hooks, 1);
    CHECK(http_server_register_on_shutdown(&srv, BURROW_FN(Func, on_shutdown, &hooks)));
    ServeJob sj = {&srv, net_tcp_listener_as_listener(l), BURROW_NO_ERROR};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, serve_job, &sj));

    NetTCPAddr ra = {slice_from(loop4_bytes, 4, 4, TYPE_BYTE), port, BURROW_STR_EMPTY};
    NetTCPConn *c = net_dial_tcp(h, cs("tcp"), NULL, &ra, &err);
    if (c == NULL) {
        testing_t_errorf_v(t, "dial: %v", err);
    } else {
        const char *req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        (void)net_tcp_conn_write(c, bytes_of(req, (Int)strlen(req)), &err);
        char got[512];
        CHECK(read_until(c, got, sizeof got, "hello"));
    }

    /* The connection is idle now, or soon will be, so Shutdown closes it. */
    CHECK(!BURROW_FAILED(http_server_shutdown(&srv, context_background())));
    sync_wait_group_wait(&wg);
    CHECK(errors_is(sj.err, http_err_server_closed));
    sync_wait_group_wait(&hooks);

    if (c != NULL) {
        char buf[16];
        (void)net_tcp_conn_set_read_deadline(c, time_add(time_now(), 10 * TIME_SECOND));
        Int n = net_tcp_conn_read(c, bytes_of(buf, (Int)sizeof buf), &err);
        CHECK_INT_EQ(n, 0);
        CHECK(errors_is(err, io_eof));
        (void)net_tcp_conn_close(c);
        net_tcp_conn_free(c);
    }
    http_server_free(&srv);
    net_tcp_listener_free(l);
}

static void TestShutdownGivesUpWhenTheContextIsDone(TestingT *t) {
    need_tcp(t);
    Alloc *h = heap_allocator();
    NetTCPAddr la = {slice_from(loop4_bytes, 4, 4, TYPE_BYTE), 0, BURROW_STR_EMPTY};
    Error err = BURROW_NO_ERROR;
    NetTCPListener *l = net_listen_tcp(h, cs("tcp"), &la, &err);
    if (l == NULL) {
        testing_t_fatalf_v(t, "listen: %v", err);
        return;
    }
    NetAddr addr = net_tcp_listener_addr(l);
    Int port = ((const NetTCPAddr *)addr.data)->port;
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_hello, NULL);
    HttpServer srv = {.handler = http_handler_func_as_handler(&f)};
    ServeJob sj = {&srv, net_tcp_listener_as_listener(l), BURROW_NO_ERROR};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, serve_job, &sj));

    /* A connection that never sends a request is new, not idle, for five
     * seconds, so Shutdown waits for it. */
    NetTCPAddr ra = {slice_from(loop4_bytes, 4, 4, TYPE_BYTE), port, BURROW_STR_EMPTY};
    NetTCPConn *c = net_dial_tcp(h, cs("tcp"), NULL, &ra, &err);
    CHECK(c != NULL);
    time_sleep(50 * TIME_MILLISECOND);

    ContextCancelFunc cancel;
    Context ctx =
        context_with_timeout(h, context_background(), 100 * TIME_MILLISECOND, &cancel);
    CHECK(errors_is(http_server_shutdown(&srv, ctx), context_deadline_exceeded));
    BURROW_CALLF0(cancel);
    context_release(ctx);

    /* Close takes the rest. */
    CHECK(!BURROW_FAILED(http_server_close(&srv)));
    sync_wait_group_wait(&wg);
    CHECK(errors_is(sj.err, http_err_server_closed));
    if (c != NULL) {
        (void)net_tcp_conn_close(c);
        net_tcp_conn_free(c);
    }
    http_server_free(&srv);
    net_tcp_listener_free(l);
}

#define TESTS(X)                                                                       \
    X(TestServerResponsesMatchGo)                                                      \
    X(TestConnStateHookSeesEachState)                                                  \
    X(TestConnStateNames)                                                              \
    X(TestTheRequestContextHasTheServerAndTheLocalAddr)                                \
    X(TestAResponseControllerSaysWhatItCannotDo)                                       \
    X(TestMaxBytesReaderStopsAtTheLimit)                                               \
    X(TestServerErrorTexts)                                                            \
    X(TestListenAndServeSaysWhatIsWrongWithTheAddress)                                 \
    X(TestServeAfterCloseIsServerClosed)                                               \
    X(TestShutdownClosesIdleConnectionsAndStopsServe)                                  \
    X(TestShutdownGivesUpWhenTheContextIsDone)

TESTING_MAIN(TESTS)
