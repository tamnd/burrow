/* Derived from Go's src/net/http/serve_test.go.
 * Go source: go1.27.1.
 *
 * The HTTP/1 server: what it sends back for a request, its errors, its hooks,
 * hijacking, Shutdown and Close, and the handlers that wrap others.
 *
 * The first tests are burrow's own. They follow serve_test.go, which feeds a
 * request through a connection that lives in memory and looks at the bytes
 * that come back. The responses here were made by Go 1.27.1 over net.Pipe with
 * the same handlers, with the Date lines taken out, since they change. The
 * tests after them are Go's, the ones that pin its fixes for request
 * smuggling, and then the CONNECT tests that came with go1.27.2. The ones that
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
#include "burrow/netpoll.h"
#include "burrow/panic.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/syscall.h"
#include "burrow/time.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_TCP 1
#endif

#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

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

/* The error the last read_job stopped on, io_eof when the server closed the
 * connection. */
static Error last_read_end;

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
        if (BURROW_FAILED(err)) {
            last_read_end = err;
            break;
        }
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
    if (n > 0)
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
    /* The handler and TimeoutHandler see the context end at the same time, and
     * a write can get in before the timeout is written down, which it can in
     * Go too. Writing until one fails waits that out, and what got in is
     * thrown away with the rest of the handler's output. */
    for (int i = 0; i < 5000; i++) {
        late_write_err = BURROW_NO_ERROR;
        (void)http_response_writer_write(w, bytes_of("late", 4), &late_write_err);
        if (BURROW_FAILED(late_write_err))
            break;
        time_sleep(TIME_MILLISECOND);
    }
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
     /* Go's probe stops at the hijack. The write after it is ours, and Go
      * logs that one too. */
     .want_log = "http: response.Write on hijacked connection from "},
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

/* ------------------------------------------ requests that smuggle another
 *
 * The tests from here to the CONNECT ones are Go's, from serve_test.go, and
 * each of these is the test that pins a request smuggling fix. Go's
 * setParallel and afterTest are left out. Go starts Serve and waits for the
 * connection to close. Not in Go: serve_one_conn then waits for Serve and for
 * the connection's goroutine, so that the test can give the connection back. */

typedef struct DummyAddr {
    Str s;
} DummyAddr;

static Str dummy_addr_network(void *self) {
    return ((const DummyAddr *)self)->s;
}

static Str dummy_addr_string(void *self, Alloc *a) {
    (void)a;
    return ((const DummyAddr *)self)->s;
}

static const NetAddrVT dummy_addr_vt = {NULL, dummy_addr_network, dummy_addr_string};
static const DummyAddr local_addr = {BURROW_S_INIT("local-addr")};
static const DummyAddr remote_addr = {BURROW_S_INIT("remote-addr")};

/* noopConn. */
static NetAddr noop_conn_local_addr(void *self) {
    (void)self;
    return (NetAddr){&dummy_addr_vt, (void *)(uintptr_t)&local_addr};
}

static NetAddr noop_conn_remote_addr(void *self) {
    (void)self;
    return (NetAddr){&dummy_addr_vt, (void *)(uintptr_t)&remote_addr};
}

static Error noop_conn_set_deadline(void *self, Time t) {
    (void)self;
    (void)t;
    return BURROW_NO_ERROR;
}

typedef struct TestConn {
    SyncMutex read_mu; /* for TestHandlerBodyClose */
    BytesBuffer read_buf;
    BytesBuffer write_buf;
    Chan *closec; /* 1-buffered; receives true when Close is called */
} TestConn;

static Int test_conn_read(void *self, Slice b, Error *err) {
    TestConn *c = (TestConn *)self;
    sync_mutex_lock(&c->read_mu);
    Int n = bytes_buffer_read(&c->read_buf, b, err);
    sync_mutex_unlock(&c->read_mu);
    return n;
}

static Int test_conn_write(void *self, Slice b, Error *err) {
    TestConn *c = (TestConn *)self;
    return bytes_buffer_write(&c->write_buf, b, err);
}

static Error test_conn_close(void *self) {
    TestConn *c = (TestConn *)self;
    bool v = true;
    (void)chan_try_send(c->closec, &v);
    return BURROW_NO_ERROR;
}

static const NetConnVT test_conn_vt = {
    {NULL, test_conn_read}, {NULL, test_conn_write}, {NULL, test_conn_close},
    noop_conn_local_addr,   noop_conn_remote_addr,   noop_conn_set_deadline,
    noop_conn_set_deadline, noop_conn_set_deadline,
};

/* newTestConn. Not in Go: the connection comes from a, with a channel from the
 * heap that test_conn_free gives back. */
static TestConn *new_test_conn(TestingT *t, Alloc *a) {
    TestConn *c = BURROW_NEW(a, TestConn);
    if (c == NULL)
        testing_t_fatal_v(t, "out of memory");
    c->read_buf = BYTES_BUFFER(a);
    c->write_buf = BYTES_BUFFER(a);
    c->closec = chan_make(heap_allocator(), TYPE_BOOL, 1);
    if (c->closec == NULL)
        testing_t_fatal_v(t, "out of memory");
    return c;
}

static void test_conn_free(TestConn *c) {
    bytes_buffer_free(&c->read_buf);
    bytes_buffer_free(&c->write_buf);
    chan_free(c->closec);
}

/* Not in Go: go srv.Serve(ln) with a oneConnListener for conn, and
 * <-closec, and then the wait for Serve and the connection to be done with
 * that comes before the test gives the connection back. */
static void serve_one_conn(HttpServer *srv, NetConn conn, Chan *closec) {
    OneConnListener ol = {conn, false};
    ServeJob sj = {srv, {&one_vt, &ol}, BURROW_NO_ERROR};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, serve_job, &sj));
    bool v;
    (void)chan_recv(closec, &v);
    sync_wait_group_wait(&wg);
    http_server_free(srv);
}

/* testHandlerBodyConsumer represents a function injected into a test handler to
 * vary work done on a request Body. */
typedef struct TestHandlerBodyConsumer {
    const char *name;
    void (*f)(IoReadCloser r);
} TestHandlerBodyConsumer;

static void consume_nil(IoReadCloser r) {
    (void)r;
}

static void consume_close(IoReadCloser r) {
    (void)r.vt->closer.close(r.data);
}

static void consume_discard(IoReadCloser r) {
    Error err = BURROW_NO_ERROR;
    (void)io_copy(heap_allocator(), io_discard, io_read_closer_as_io_reader(r), &err);
}

static const TestHandlerBodyConsumer test_handler_body_consumers[] = {
    {"nil", consume_nil},
    {"close", consume_close},
    {"discard", consume_discard},
};

/* What the handlers below need, which Go's closures capture. */
typedef struct SecretTest {
    TestingT *t;
    const TestHandlerBodyConsumer *handler;
    int num_reqs;
} SecretTest;

static void serve_no_secret(void *env, HttpResponseWriter w, HttpRequest *req) {
    (void)w;
    SecretTest *st = (SecretTest *)env;
    st->num_reqs++;
    if (strings_contains(req->url->path, BURROW_S("secret")))
        testing_t_error_v(st->t,
                          "Request for /secret encountered, should not have happened.");
    st->handler->f(req->body);
}

static void serve_no_secret_named(void *env, HttpResponseWriter w, HttpRequest *req) {
    (void)w;
    SecretTest *st = (SecretTest *)env;
    st->num_reqs++;
    if (strings_contains(req->url->path, BURROW_S("secret")))
        testing_t_errorf_v(
            st->t,
            "Handler %s, Request for /secret encountered, should not have "
            "happened.",
            cs(st->handler->name));
    st->handler->f(req->body);
}

static void TestRequestBodyReadErrorClosesConnection(TestingT *t) {
    for (Int i = 0; i < LEN(test_handler_body_consumers); i++) {
        const TestHandlerBodyConsumer *handler = &test_handler_body_consumers[i];
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        TestConn *conn = new_test_conn(t, arena_allocator(&ar));
        (void)bytes_buffer_write_string(
            &conn->read_buf,
            BURROW_S("POST /public HTTP/1.1\r\n"
                     "Host: test\r\n"
                     "Transfer-Encoding: chunked\r\n"
                     "\r\n"
                     "hax\r\n" /* Invalid chunked encoding */
                     "GET /secret HTTP/1.1\r\n"
                     "Host: test\r\n"
                     "\r\n"),
            NULL);

        SecretTest st = {t, handler, 0};
        HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_no_secret, &st);
        HttpServer srv = {.handler = http_handler_func_as_handler(&f)};
        serve_one_conn(&srv, (NetConn){&test_conn_vt, conn}, conn->closec);
        if (st.num_reqs != 1)
            testing_t_errorf_v(t, "Handler %v: got %d reqs; want 1", cs(handler->name),
                               st.num_reqs);
        test_conn_free(conn);
        arena_free(&ar);
    }
}

static void TestInvalidTrailerClosesConnection(TestingT *t) {
    for (Int i = 0; i < LEN(test_handler_body_consumers); i++) {
        const TestHandlerBodyConsumer *handler = &test_handler_body_consumers[i];
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        TestConn *conn = new_test_conn(t, arena_allocator(&ar));
        (void)bytes_buffer_write_string(&conn->read_buf,
                                        BURROW_S("POST /public HTTP/1.1\r\n"
                                                 "Host: test\r\n"
                                                 "Trailer: hack\r\n"
                                                 "Transfer-Encoding: chunked\r\n"
                                                 "\r\n"
                                                 "3\r\n"
                                                 "hax\r\n"
                                                 "0\r\n"
                                                 "I'm not a valid trailer\r\n"
                                                 "GET /secret HTTP/1.1\r\n"
                                                 "Host: test\r\n"
                                                 "\r\n"),
                                        NULL);

        SecretTest st = {t, handler, 0};
        HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_no_secret_named, &st);
        HttpServer srv = {.handler = http_handler_func_as_handler(&f)};
        serve_one_conn(&srv, (NetConn){&test_conn_vt, conn}, conn->closec);
        if (st.num_reqs != 1)
            testing_t_errorf_v(t, "Handler %s: got %d reqs; want 1", cs(handler->name),
                               st.num_reqs);
        test_conn_free(conn);
        arena_free(&ar);
    }
}

/* One cue of a slowTestConn's script: a time.Duration to sleep when sleep is
 * set, and a string to read when it is not. */
typedef struct SlowCue {
    bool sleep;
    Duration d;
    Str s;
} SlowCue;

/* slowTestConn is a net.Conn that provides a means to simulate parts of a
 * request being received piecemeal. Deadlines can be set and enforced in both
 * Read and Write. */
typedef struct SlowTestConn {
    /* over multiple calls to Read, time.Durations are slept, strings are read. */
    SlowCue script[4];
    Int pos, n;
    Chan *closec;

    SyncMutex mu; /* guards rd/wd */
    Time rd, wd;  /* read, write deadline */

    /* Not in Go: syscall.ETIMEDOUT as an Error. */
    Error timedout;
} SlowTestConn;

static Error slow_test_conn_set_read_deadline(void *self, Time t) {
    SlowTestConn *c = (SlowTestConn *)self;
    sync_mutex_lock(&c->mu);
    c->rd = t;
    sync_mutex_unlock(&c->mu);
    return BURROW_NO_ERROR;
}

static Error slow_test_conn_set_write_deadline(void *self, Time t) {
    SlowTestConn *c = (SlowTestConn *)self;
    sync_mutex_lock(&c->mu);
    c->wd = t;
    sync_mutex_unlock(&c->mu);
    return BURROW_NO_ERROR;
}

static Error slow_test_conn_set_deadline(void *self, Time t) {
    (void)slow_test_conn_set_read_deadline(self, t);
    (void)slow_test_conn_set_write_deadline(self, t);
    return BURROW_NO_ERROR;
}

static Int slow_test_conn_read(void *self, Slice b, Error *err) {
    SlowTestConn *c = (SlowTestConn *)self;
    Int n = 0;
    *err = BURROW_NO_ERROR;
    sync_mutex_lock(&c->mu);
    for (;;) {
        if (!time_is_zero(c->rd) && time_after(time_now(), c->rd)) {
            *err = c->timedout;
            break;
        }
        if (c->pos == c->n) {
            *err = io_eof;
            break;
        }

        SlowCue *cue = &c->script[c->pos];
        if (cue->sleep) {
            if (!time_is_zero(c->rd)) {
                /* If the deadline falls in the middle of our sleep window,
                 * deduct part of the sleep, then return a timeout. */
                Duration remaining = time_until(c->rd);
                if (remaining < cue->d) {
                    cue->d -= remaining;
                    time_sleep(remaining);
                    *err = c->timedout;
                    break;
                }
            }
            c->pos++;
            time_sleep(cue->d);
            continue;
        }

        n = cue->s.len < b.len ? cue->s.len : b.len;
        memcpy(b.p, cue->s.p, (size_t)n);
        /* If cue is too big for the buffer, leave the end for the next Read. */
        if (cue->s.len > n)
            cue->s = str_from_bytes(cue->s.p + n, cue->s.len - n);
        else
            c->pos++;
        break;
    }
    sync_mutex_unlock(&c->mu);
    return n;
}

static Error slow_test_conn_close(void *self) {
    SlowTestConn *c = (SlowTestConn *)self;
    bool v = true;
    (void)chan_try_send(c->closec, &v);
    return BURROW_NO_ERROR;
}

static Int slow_test_conn_write(void *self, Slice b, Error *err) {
    SlowTestConn *c = (SlowTestConn *)self;
    *err = BURROW_NO_ERROR;
    if (!time_is_zero(c->wd) && time_after(time_now(), c->wd)) {
        *err = c->timedout;
        return 0;
    }
    return b.len;
}

static const NetConnVT slow_test_conn_vt = {
    {NULL, slow_test_conn_read},
    {NULL, slow_test_conn_write},
    {NULL, slow_test_conn_close},
    noop_conn_local_addr,
    noop_conn_remote_addr,
    slow_test_conn_set_deadline,
    slow_test_conn_set_read_deadline,
    slow_test_conn_set_write_deadline,
};

static void TestRequestBodyTimeoutClosesConnection(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping in -short mode");
    for (Int i = 0; i < LEN(test_handler_body_consumers); i++) {
        const TestHandlerBodyConsumer *handler = &test_handler_body_consumers[i];
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        SlowTestConn conn = {
            .script =
                {
                    {false, 0,
                     BURROW_S("POST /public HTTP/1.1\r\n"
                              "Host: test\r\n"
                              "Content-Length: 10000\r\n"
                              "\r\n")},
                    {false, 0, BURROW_S("foo bar baz")},
                    {true, 600 * TIME_MILLISECOND,
                     BURROW_STR_EMPTY}, /* Request deadline should hit here */
                    {false, 0,
                     BURROW_S("GET /secret HTTP/1.1\r\n"
                              "Host: test\r\n"
                              "\r\n")},
                },
            .n = 4,
            .closec = chan_make(heap_allocator(), TYPE_BOOL, 1),
            .timedout = syscall_errno_as_error(SYSCALL_ETIMEDOUT, arena_allocator(&ar)),
        };
        if (conn.closec == NULL)
            testing_t_fatal_v(t, "out of memory");

        SecretTest st = {t, handler, 0};
        HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_no_secret, &st);
        HttpServer s = {
            .handler = http_handler_func_as_handler(&f),
            .read_timeout = 400 * TIME_MILLISECOND,
        };
        serve_one_conn(&s, (NetConn){&slow_test_conn_vt, &conn}, conn.closec);

        if (st.num_reqs != 1)
            testing_t_errorf_v(t, "Handler %v: got %d reqs; want 1", cs(handler->name),
                               st.num_reqs);
        chan_free(conn.closec);
        arena_free(&ar);
    }
}

static void serve_nothing(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)w;
    (void)r;
}

static void TestServerValidatesHeaders(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str too_large =
        fmt_sprintf_v(a, "Foo: %s\r\n", strings_repeat(a, BURROW_S("x"), 1 << 21));
    const struct {
        Str header;
        Int want;
    } tests[] = {
        {BURROW_S(""), 200},
        {BURROW_S("Foo: bar\r\n"), 200},
        {BURROW_S("X-Foo: bar\r\n"), 200},
        {BURROW_S("Foo: a space\r\n"), 200},

        {BURROW_S("A space: foo\r\n"), 400},    /* space in header */
        {BURROW_S("foo\377bar: foo\r\n"), 400}, /* binary in header */
        {BURROW_S("foo\000bar: foo\r\n"), 400}, /* binary in header */
        {too_large, 431},                       /* header too large */
        /* Spaces between the header key and colon are not allowed.
         * See RFC 7230, Section 3.2.4. */
        {BURROW_S("Foo : bar\r\n"), 400},
        {BURROW_S("Foo\t: bar\r\n"), 400},

        /* Empty header keys are invalid.
         * See RFC 7230, Section 3.2. */
        {BURROW_S(": empty key\r\n"), 400},

        /* Requests with invalid Content-Length headers should be rejected
         * regardless of the presence of a Transfer-Encoding header.
         * Check out RFC 9110, Section 8.6 and RFC 9112, Section 6.3.3. */
        {BURROW_S("Content-Length: notdigits\r\n"), 400},
        {BURROW_S("Content-Length: notdigits\r\nTransfer-Encoding: "
                  "chunked\r\n\r\n0\r\n\r\n"),
         400},

        {BURROW_S("foo: foo foo\r\n"), 200},    /* LWS space is okay */
        {BURROW_S("foo: foo\tfoo\r\n"), 200},   /* LWS tab is okay */
        {BURROW_S("foo: foo\000foo\r\n"), 400}, /* CTL 0x00 in value is bad */
        {BURROW_S("foo: foo\177foo\r\n"), 400}, /* CTL 0x7f in value is bad */
        {BURROW_S("foo: foo\377foo\r\n"),
         200}, /* non-ASCII high octets in value are fine */
    };
    LogLogger *quiet_log = log_new(a, io_discard, BURROW_S(""), 0);
    for (Int i = 0; i < LEN(tests); i++) {
        TestConn *conn = new_test_conn(t, a);
        (void)bytes_buffer_write_string(
            &conn->read_buf, BURROW_S("GET / HTTP/1.1\r\nHost: foo\r\n"), NULL);
        (void)bytes_buffer_write_string(&conn->read_buf, tests[i].header, NULL);
        (void)bytes_buffer_write_string(&conn->read_buf, BURROW_S("\r\n"), NULL);

        HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_nothing, NULL);
        HttpServer srv = {
            .error_log = quiet_log,
            .handler = http_handler_func_as_handler(&f),
        };
        serve_one_conn(&srv, (NetConn){&test_conn_vt, conn}, conn->closec);
        Error err = BURROW_NO_ERROR;
        BufioReader *br =
            bufio_new_reader(a, bytes_buffer_as_io_reader(&conn->write_buf));
        HttpResponse *res = br == NULL ? NULL : http_read_response(a, br, NULL, &err);
        if (res == NULL) {
            /* Go prints res here, which is nil when ReadResponse fails. */
            testing_t_errorf_v(t, "For %q, ReadResponse: %v", tests[i].header,
                               BURROW_S("<nil>"));
        } else {
            if (res->status_code != tests[i].want)
                testing_t_errorf_v(t, "For %q, Status = %d; want %d", tests[i].header,
                                   res->status_code, tests[i].want);
            http_response_free(res);
        }
        test_conn_free(conn);
    }
    log_logger_free(a, quiet_log);
    arena_free(&ar);
}

/* What TestInvalidChunkedBodies's handler sends on reqc. Not in Go: it sends
 * whether io.ReadAll failed rather than the error, which belongs to the
 * server's connection and may be gone by the time the test looks at it. */
typedef struct ChunkedBodyTest {
    TestingT *t;
    Chan *reqc;
} ChunkedBodyTest;

static void serve_read_all_body(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    ChunkedBodyTest *ct = (ChunkedBodyTest *)env;
    Error err = BURROW_NO_ERROR;
    Slice got =
        io_read_all(heap_allocator(), io_read_closer_as_io_reader(r->body), &err);
    if (!BURROW_FAILED(err))
        testing_t_logf_v(ct->t, "read body: %q", got);
    mem_free(heap_allocator(), got.p, (size_t)got.cap, 1);
    bool failed = BURROW_FAILED(err);
    chan_send(ct->reqc, &failed);
}

typedef struct ChunkedBodyCase {
    const char *name;
    const char *b;
} ChunkedBodyCase;

static void invalid_chunked_body(void *env, TestingT *t) {
    need_tcp(t);
    const ChunkedBodyCase *test = (const ChunkedBodyCase *)env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    ChunkedBodyTest ct = {t, chan_make(heap_allocator(), TYPE_BOOL, 0)};
    if (ct.reqc == NULL)
        testing_t_fatal_v(t, "out of memory");
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_read_all_body, &ct);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&f));

    Error err = BURROW_NO_ERROR;
    Url *server_url = url_parse(a, ts->url, &err);
    NetConn conn = {NULL, NULL};
    if (server_url != NULL)
        conn = net_dial(a, BURROW_S("tcp"), server_url->host, &err);
    if (!BURROW_FAILED(err)) {
        Str req = fmt_sprintf_v(a,
                                "POST / HTTP/1.1\r\n"
                                "Host: localhost\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "Connection: close\r\n"
                                "\r\n"
                                "%s",
                                cs(test->b));
        (void)conn.vt->writer.write(conn.data, bytes_of((const char *)req.p, req.len),
                                    &err);
    }
    if (!BURROW_FAILED(err)) {
        (void)net_tcp_conn_close_write(net_conn_as_tcp_conn(conn));

        bool failed = false;
        (void)chan_recv(ct.reqc, &failed);
        if (!failed)
            testing_t_errorf_v(
                t, "server handler: io.ReadAll(r.Body) succeeded, want error");
    } else {
        /* Not in Go: t.Fatal, after the cleanup below. */
        testing_t_error_v(t, err);
    }
    if (conn.vt != NULL) {
        (void)conn.vt->closer.close(conn.data);
        net_conn_free(conn);
    }
    httptest_server_free(ts);
    chan_free(ct.reqc);
    arena_free(&ar);
}

static void TestInvalidChunkedBodies(TestingT *t) {
    static const ChunkedBodyCase tests[] = {
        {"bare LF in chunk size", "1\na\r\n0\r\n\r\n"},
        {"bare LF at body end", "1\r\na\r\n0\r\n\n"},
    };
    for (Int i = 0; i < LEN(tests); i++)
        (void)testing_t_run(t, cs(tests[i].name),
                            BURROW_FN(TestingTFunc, invalid_chunked_body,
                                      (void *)(uintptr_t)&tests[i]));
}

/* fetchWireResponse is a helper for dialing to host,
 * sending http1ReqBody as the payload and retrieving
 * the response as it was sent on the wire. */
static Slice fetch_wire_response(Alloc *a, Str host, Str http1_req_body, Error *err) {
    NetConn conn = net_dial(a, BURROW_S("tcp"), host, err);
    if (BURROW_FAILED(*err))
        return slice_nil(TYPE_BYTE);

    Slice out = slice_nil(TYPE_BYTE);
    (void)conn.vt->writer.write(
        conn.data, bytes_of((const char *)http1_req_body.p, http1_req_body.len), err);
    if (!BURROW_FAILED(*err))
        out = io_read_all(a, (IoReader){&conn.vt->reader, conn.data}, err);
    (void)conn.vt->closer.close(conn.data);
    net_conn_free(conn);
    return out;
}

static void serve_hello_world(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    write_str(w, "Hello, World!");
}

static void unsupported_transfer_encodings_return_501(void *env, TestingT *t) {
    (void)env;
    need_tcp(t);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, serve_hello_world, NULL);
    HttptestServer *cst = httptest_new_server(NULL, http_handler_func_as_handler(&f));

    Error err = BURROW_NO_ERROR;
    Url *server_url = url_parse(a, cst->url, &err);
    if (server_url == NULL) {
        /* Not in Go: t.Fatalf, as an error, so that the cleanup can run. */
        testing_t_errorf_v(t, "Failed to parse server URL: %v", err);
        httptest_server_free(cst);
        arena_free(&ar);
        return;
    }

    static const char *const unsupported_tes[] = {
        "fugazi",
        "foo-bar",
        "unknown",
        "\" chunked\"",
    };

    for (Int i = 0; i < LEN(unsupported_tes); i++) {
        Str bad_te = cs(unsupported_tes[i]);
        Str http1_req_body =
            fmt_sprintf_v(a,
                          ""
                          "POST / HTTP/1.1\r\nConnection: close\r\n"
                          "Host: localhost\r\nTransfer-Encoding: %s\r\n\r\n",
                          bad_te);

        Slice got_body = fetch_wire_response(a, server_url->host, http1_req_body, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%q. unexpected error: %v", bad_te, err);
            continue;
        }

        Str want_body =
            BURROW_S(""
                     "HTTP/1.1 501 Not Implemented\r\nContent-Type: text/plain; "
                     "charset=utf-8\r\n"
                     "Connection: close\r\n\r\nUnsupported transfer encoding");

        if (!str_eq(str_from_bytes(got_body.p, got_body.len), want_body))
            testing_t_errorf_v(t, "%q. body\ngot\n%q\nwant\n%q", bad_te, got_body,
                               want_body);
    }
    httptest_server_free(cst);
    arena_free(&ar);
}

static void TestUnsupportedTransferEncodingsReturn501(TestingT *t) {
    (void)testing_t_run(
        t, BURROW_S("h1"),
        BURROW_FN(TestingTFunc, unsupported_transfer_encodings_return_501, NULL));
}

/* ------------------------------------------------------------- CONNECT */

/* TestServerCONNECTSuccess and TestServerCONNECTFailure are newer than
 * go1.27.1: they came with the go1.27.2 fix for GO-2026-6613, CL 847311. Go
 * runs them under synctest with its newHTTP1ServerTest and newTestHandler,
 * which are not here. Not in Go: the handler's steps are in serve_connect_*,
 * the request goes through exchange, and the response is read back from what
 * came out. The connection was closed when exchange's read stopped on
 * io_eof. */

static void serve_connect_success(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    http_response_writer_write_header(w, 200);
    write_str(w, "body");
    HttpResponseController rc = http_new_response_controller(w);
    (void)http_response_controller_flush(&rc);
}

static void serve_connect_failure(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    (void)http_header_set(http_response_writer_header(w), BURROW_S("Content-Length"),
                          BURROW_S("4"));
    http_response_writer_write_header(w, 409);
    write_str(w, "body");
}

/* Serves a CONNECT with fn, and reads the response that came back. NULL when
 * there was none, after saying so. */
static HttpResponse *connect_exchange(TestingT *t, Alloc *a, StringsReader *sr,
                                      void (*fn)(void *, HttpResponseWriter,
                                                 HttpRequest *),
                                      char *got, size_t cap) {
    char log_line[256];
    HttpHandlerFunc f = BURROW_FN(HttpHandlerFunc, fn, NULL);
    HttpServer srv = {.handler = http_handler_func_as_handler(&f)};
    last_read_end = BURROW_NO_ERROR;
    exchange(t, &srv,
             "CONNECT backend.example.tld:80 HTTP/1.1\r\n"
             "Host: example.tld\r\n"
             "\r\n",
             got, cap, log_line, sizeof log_line);
    strings_reader_reset(sr, cs(got));
    BufioReader *br = bufio_new_reader(a, strings_reader_as_io_reader(sr));
    Error err = BURROW_NO_ERROR;
    HttpResponse *resp = br == NULL ? NULL : http_read_response(a, br, NULL, &err);
    if (resp == NULL)
        testing_t_errorf_v(t, "reading the response %q: %v", cs(got), err);
    return resp;
}

/* conn.wantBytes(body) and conn.wantClosed(). */
static void want_body_closed(TestingT *t, Alloc *a, HttpResponse *resp,
                             const char *body) {
    Error err = BURROW_NO_ERROR;
    Slice got = io_read_all(a, io_read_closer_as_io_reader(resp->body), &err);
    if (BURROW_FAILED(err) || !str_eq(str_from_bytes(got.p, got.len), cs(body)))
        testing_t_errorf_v(t, "read body %q, %v; want %q",
                           str_from_bytes(got.p, got.len), err, cs(body));
    if (!errors_is(last_read_end, io_eof))
        testing_t_errorf_v(t, "the read ended with %v; want the connection closed",
                           last_read_end);
}

static void TestServerCONNECTSuccess(TestingT *t) {
    enum { cap = 4096 };
    static char got[cap];
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsReader sr;
    HttpResponse *resp = connect_exchange(t, a, &sr, serve_connect_success, got, cap);
    if (resp != NULL) {
        const Int code = 200;
        if (resp->status_code != code)
            testing_t_errorf_v(t, "response status = %v, want %v", resp->status_code,
                               code);
        if (resp->content_length != -1)
            testing_t_errorf_v(t, "Content-Length: %v; want absent",
                               resp->content_length);
        if (resp->transfer_encoding.len > 0)
            testing_t_errorf_v(t, "Transfer-Encoding: %q; want absent",
                               resp->transfer_encoding);
        const Str hs[] = {BURROW_S_INIT("Content-Length"),
                          BURROW_S_INIT("Transfer-Encoding")};
        for (Int i = 0; i < 2; i++) {
            Slice *v = BURROW_MAP_GET(Str, Slice, resp->header, hs[i]);
            if (v != NULL)
                testing_t_errorf_v(t, "response header %q = %q; want absent", hs[i],
                                   *v);
        }
        want_body_closed(t, a, resp, "body");
        http_response_free(resp);
    }
    arena_free(&ar);
}

static void TestServerCONNECTFailure(TestingT *t) {
    enum { cap = 4096 };
    static char got[cap];
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    StringsReader sr;
    HttpResponse *resp = connect_exchange(t, a, &sr, serve_connect_failure, got, cap);
    if (resp != NULL) {
        const Int code = 409;
        if (resp->status_code != code)
            testing_t_errorf_v(t, "response status = %v, want %v", resp->status_code,
                               code);
        if (!resp->close)
            testing_t_errorf_v(t, "Connection: close not set; want it to be");
        want_body_closed(t, a, resp, "body");
        http_response_free(resp);
    }
    arena_free(&ar);
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
    X(TestShutdownGivesUpWhenTheContextIsDone)                                         \
    X(TestRequestBodyReadErrorClosesConnection)                                        \
    X(TestInvalidTrailerClosesConnection)                                              \
    X(TestRequestBodyTimeoutClosesConnection)                                          \
    X(TestServerValidatesHeaders)                                                      \
    X(TestInvalidChunkedBodies)                                                        \
    X(TestUnsupportedTransferEncodingsReturn501)                                       \
    X(TestServerCONNECTSuccess)                                                        \
    X(TestServerCONNECTFailure)

TESTING_MAIN(TESTS)
