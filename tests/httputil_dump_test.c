/* Derived from Go's src/net/http/httputil/dump_test.go, with TestChunk from
 * src/net/http/internal/chunked_test.go run through httputil's own chunked
 * reader and writer.
 * Go source: go1.27.1.
 *
 * Go's TestDumpRequest ends by counting goroutines, to catch DumpRequestOut
 * leaving some behind. There is no goroutine count here, and
 * httputil_dump_request_out waits for every goroutine it starts before it
 * returns, so that part is left out.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/math/rand.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httputil.h"
#include "burrow/net/url.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/strings.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

/* t.Fatalf, with a return the analyzer can see. */
#define FATALF(...)                                                                    \
    do {                                                                               \
        testing_t_fatalf_v(t, __VA_ARGS__);                                            \
        return;                                                                        \
    } while (0)

static Str cs(const char *s) {
    return str_from_cstr(s);
}

static Str slice_str(Slice b) {
    return str_from_bytes(b.p, b.len);
}

/* s[lo:hi]. */
static Str sub(Str s, Int lo, Int hi) {
    return str_from_bytes(s.p + lo, hi - lo);
}

/* io.NopCloser(strings.NewReader(s)), which reads the same as Go's
 * bytes.NewReader of the same bytes. */
static IoReadCloser body_of(Alloc *a, Str s) {
    StringsReader *r = strings_new_reader(a, s);
    IoNopCloser *c = (IoNopCloser *)mem_alloc(a, sizeof *c, _Alignof(IoNopCloser));
    if (r == NULL || c == NULL)
        return (IoReadCloser){NULL, NULL};
    *c = io_nop_closer(strings_reader_as_io_reader(r));
    return io_nop_closer_as_io_read_closer(c);
}

/* eofReader. */
static Int eof_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    *err = io_eof;
    return 0;
}

static Error eof_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static const IoReadCloserVT eof_reader_vt = {{NULL, eof_read}, {NULL, eof_close}};

/* &http.Request{...}, with an empty header, as the test gives each request that
 * has none. */
static HttpRequest *new_req(Alloc *a, const char *method, int major, int minor) {
    HttpRequest *r = (HttpRequest *)mem_alloc(a, sizeof *r, _Alignof(HttpRequest));
    if (r == NULL)
        return NULL;
    memset(r, 0, sizeof *r);
    r->method = cs(method);
    r->proto_major = major;
    r->proto_minor = minor;
    r->header = http_header_make(a);
    return r;
}

/* &url.URL{Scheme: scheme, Host: host, Path: path}. */
static Url *new_url(Alloc *a, const char *scheme, const char *host, const char *path) {
    Url *u = (Url *)mem_alloc(a, sizeof *u, _Alignof(Url));
    if (u == NULL)
        return NULL;
    memset(u, 0, sizeof *u);
    u->scheme = cs(scheme);
    u->host = cs(host);
    u->path = cs(path);
    return u;
}

static HttpRequest *must_new_request(Alloc *a, const char *method, const char *url) {
    Error err;
    IoReader none = {NULL, NULL};
    HttpRequest *r = http_new_request(a, cs(method), cs(url), none, &err);
    if (r == NULL)
        panic_str(fmt_sprintf_v(heap_allocator(), "NewRequest(%q, %q) err = %v",
                                cs(method), cs(url), err));
    return r;
}

static HttpRequest *must_read_request(Alloc *a, const char *s) {
    Error err;
    StringsReader *sr = strings_new_reader(a, cs(s));
    if (sr == NULL)
        panic_str(cs("out of memory"));
    BufioReader *br = bufio_new_reader(a, strings_reader_as_io_reader(sr));
    if (br == NULL)
        panic_str(cs("out of memory"));
    HttpRequest *r = http_read_request(a, br, &err);
    if (r == NULL)
        panic_str(error_text(err));
    return r;
}

/* ------------------------------------------------------------------ requests */

/* HTTP/1.1 => chunked coding; body; empty trailer */
static HttpRequest *req_chunked(Alloc *a) {
    HttpRequest *r = new_req(a, "GET", 1, 1);
    Str *te = (Str *)mem_alloc(a, sizeof *te, _Alignof(Str));
    if (r == NULL || te == NULL)
        return NULL;
    r->url = new_url(a, "http", "www.google.com", "/search");
    te[0] = cs("chunked");
    r->transfer_encoding = slice_from(te, 1, 1, TYPE_STRING);
    return r;
}

/* Verify that DumpRequest preserves the HTTP version number, doesn't add a
 * Host, and doesn't add a User-Agent. */
static HttpRequest *req_http10(Alloc *a) {
    HttpRequest *r = new_req(a, "GET", 1, 0);
    if (r == NULL)
        return NULL;
    Error err;
    r->url = url_parse(a, cs("/foo"), &err);
    (void)http_header_set(r->header, cs("X-Foo"), cs("X-Bar"));
    return r;
}

static HttpRequest *req_get(Alloc *a) {
    return must_new_request(a, "GET", "http://example.com/foo");
}

/* Test that an https URL doesn't try to do an SSL negotiation with a
 * bytes.Buffer and hang with all goroutines not runnable. */
static HttpRequest *req_https(Alloc *a) {
    return must_new_request(a, "GET", "https://example.com/foo");
}

/* Request with Body, but Dump requested without it. */
static HttpRequest *req_post6(Alloc *a) {
    HttpRequest *r = new_req(a, "POST", 1, 1);
    if (r == NULL)
        return NULL;
    r->url = new_url(a, "http", "post.tld", "/");
    r->content_length = 6;
    return r;
}

/* Request with Body > 8196 (default buffer size) */
static HttpRequest *req_post8193(Alloc *a) {
    HttpRequest *r = new_req(a, "POST", 1, 1);
    if (r == NULL)
        return NULL;
    r->url = new_url(a, "http", "post.tld", "/");
    (void)http_header_set(r->header, cs("Content-Length"), cs("8193"));
    r->content_length = 8193;
    return r;
}

static HttpRequest *req_read_abs(Alloc *a) {
    return must_read_request(a, "GET http://foo.com/ HTTP/1.1\r\n"
                                "User-Agent: blah\r\n\r\n");
}

/* Issue #7215. DumpRequest should return the "Content-Length" when set */
static HttpRequest *req_read_cl3(Alloc *a) {
    return must_read_request(a, "POST /v2/api/?login HTTP/1.1\r\n"
                                "Host: passport.myhost.com\r\n"
                                "Content-Length: 3\r\n"
                                "\r\nkey1=name1&key2=name2");
}

/* Issue #7215. DumpRequest should return the "Content-Length" in ReadRequest */
static HttpRequest *req_read_cl0(Alloc *a) {
    return must_read_request(a, "POST /v2/api/?login HTTP/1.1\r\n"
                                "Host: passport.myhost.com\r\n"
                                "Content-Length: 0\r\n"
                                "\r\nkey1=name1&key2=name2");
}

/* Issue #7215. DumpRequest should not return the "Content-Length" if unset */
static HttpRequest *req_read_nocl(Alloc *a) {
    return must_read_request(a, "POST /v2/api/?login HTTP/1.1\r\n"
                                "Host: passport.myhost.com\r\n"
                                "\r\nkey1=name1&key2=name2");
}

/* Issue 18506: make drainBody recognize NoBody. Otherwise this was turning
 * into a chunked request. */
static HttpRequest *req_no_body(Alloc *a) {
    HttpRequest *r = must_new_request(a, "POST", "http://example.com/foo");
    r->body = http_no_body;
    r->content_length = 0;
    return r;
}

/* Issue 34504: a non-nil Body without ContentLength set should be chunked */
static HttpRequest *req_put_eof(Alloc *a) {
    HttpRequest *r = new_req(a, "PUT", 1, 1);
    if (r == NULL)
        return NULL;
    r->url = new_url(a, "http", "post.tld", "/test");
    r->proto = cs("HTTP/1.1");
    r->body = (IoReadCloser){&eof_reader_vt, NULL};
    return r;
}

/* Issue 54616: request with Connection header doesn't result in duplicate
 * header. */
static HttpRequest *req_read_conn_close(Alloc *a) {
    return must_read_request(a, "GET / HTTP/1.1\r\n"
                                "Host: example.com\r\n"
                                "Connection: close\r\n\r\n");
}

static HttpRequest *req_empty_url(Alloc *a) {
    return must_new_request(a, "GET", "");
}

typedef struct DumpTest {
    HttpRequest *(*req)(Alloc *a);
    const char *body; /* populates the request's body when not NULL */
    Int big;          /* that many "a" bytes for the body, and after each want */
    const char *want_dump;
    const char *want_dump_out;
    bool must_error; /* the test is expected to give an error */
    bool no_body;    /* dump with body false */
} DumpTest;

static const DumpTest dump_tests[] = {
    {.req = req_chunked,
     .body = "abcdef",
     .want_dump = "GET /search HTTP/1.1\r\n"
                  "Host: www.google.com\r\n"
                  "Transfer-Encoding: chunked\r\n\r\n"
                  "6\r\nabcdef\r\n"
                  "0\r\n\r\n"},
    {.req = req_http10, .want_dump = "GET /foo HTTP/1.0\r\nX-Foo: X-Bar\r\n\r\n"},
    {.req = req_get,
     .want_dump_out = "GET /foo HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Accept-Encoding: gzip\r\n\r\n"},
    {.req = req_https,
     .want_dump_out = "GET /foo HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Accept-Encoding: gzip\r\n\r\n"},
    {.req = req_post6,
     .body = "abcdef",
     .want_dump_out = "POST / HTTP/1.1\r\n"
                      "Host: post.tld\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Content-Length: 6\r\n"
                      "Accept-Encoding: gzip\r\n\r\n",
     .no_body = true},
    {.req = req_post8193,
     .big = 8193,
     .want_dump_out = "POST / HTTP/1.1\r\n"
                      "Host: post.tld\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Content-Length: 8193\r\n"
                      "Accept-Encoding: gzip\r\n\r\n",
     .want_dump = "POST / HTTP/1.1\r\n"
                  "Host: post.tld\r\n"
                  "Content-Length: 8193\r\n\r\n"},
    {.req = req_read_abs,
     .no_body = true,
     .want_dump = "GET http://foo.com/ HTTP/1.1\r\n"
                  "User-Agent: blah\r\n\r\n"},
    {.req = req_read_cl3,
     .want_dump = "POST /v2/api/?login HTTP/1.1\r\n"
                  "Host: passport.myhost.com\r\n"
                  "Content-Length: 3\r\n"
                  "\r\nkey"},
    {.req = req_read_cl0,
     .want_dump = "POST /v2/api/?login HTTP/1.1\r\n"
                  "Host: passport.myhost.com\r\n"
                  "Content-Length: 0\r\n\r\n"},
    {.req = req_read_nocl,
     .want_dump = "POST /v2/api/?login HTTP/1.1\r\n"
                  "Host: passport.myhost.com\r\n\r\n"},
    {.req = req_no_body,
     .want_dump_out = "POST /foo HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Content-Length: 0\r\n"
                      "Accept-Encoding: gzip\r\n\r\n"},
    {.req = req_put_eof,
     .no_body = true,
     .want_dump_out = "PUT /test HTTP/1.1\r\n"
                      "Host: post.tld\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Accept-Encoding: gzip\r\n\r\n"},
    {.req = req_read_conn_close,
     .no_body = true,
     .want_dump = "GET / HTTP/1.1\r\n"
                  "Host: example.com\r\n"
                  "Connection: close\r\n\r\n"},
};

/* freshReq. */
static HttpRequest *fresh_req(Alloc *a, const DumpTest *tt) {
    HttpRequest *req = tt->req(a);
    if (req == NULL)
        return NULL;
    if (tt->body != NULL)
        req->body = body_of(a, cs(tt->body));
    else if (tt->big > 0)
        req->body = body_of(a, strings_repeat(a, cs("a"), tt->big));
    return req;
}

static Str want_of(Alloc *a, const DumpTest *tt, const char *want) {
    Str w = cs(want);
    if (tt->big == 0)
        return w;
    return fmt_sprintf_v(a, "%s%s", w, strings_repeat(a, cs("a"), tt->big));
}

static void run_dump_test(TestingT *t, Alloc *a, Int i, const DumpTest *tt) {
    Error err;
    if (tt->want_dump != NULL) {
        HttpRequest *req = fresh_req(a, tt);
        if (req == NULL)
            FATALF("#%d: out of memory", i);
        Str want = want_of(a, tt, tt->want_dump);
        Slice dump = httputil_dump_request(a, req, !tt->no_body, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "DumpRequest #%d: %v\nWantDump:\n%s", i, err, want);
            return;
        }
        if (!str_eq(slice_str(dump), want)) {
            testing_t_errorf_v(t, "DumpRequest %d, expecting:\n%s\nGot:\n%s\n", i, want,
                               slice_str(dump));
            return;
        }
    }

    if (tt->must_error) {
        HttpRequest *req = fresh_req(a, tt);
        if (req == NULL)
            FATALF("#%d: out of memory", i);
        (void)httputil_dump_request_out(a, req, !tt->no_body, &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "DumpRequestOut #%d: expected an error, got nil", i);
        return;
    }

    if (tt->want_dump_out != NULL) {
        HttpRequest *req = fresh_req(a, tt);
        if (req == NULL)
            FATALF("#%d: out of memory", i);
        Str want = want_of(a, tt, tt->want_dump_out);
        Slice dump = httputil_dump_request_out(a, req, !tt->no_body, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "DumpRequestOut #%d: %v", i, err);
            return;
        }
        if (!str_eq(slice_str(dump), want)) {
            testing_t_errorf_v(t, "DumpRequestOut %d, expecting:\n%s\nGot:\n%s\n", i,
                               want, slice_str(dump));
            return;
        }
    }
}

static void TestDumpRequest(TestingT *t) {
    Int n = (Int)(sizeof dump_tests / sizeof dump_tests[0]);
    /* The 10 cases with an empty URL Go adds, to check that no goroutines are
     * leaked. See golang.org/issue/32571. */
    DumpTest empty;
    memset(&empty, 0, sizeof empty);
    empty.req = req_empty_url;
    empty.must_error = true;
    for (Int i = 0; i < n + 10; i++) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        run_dump_test(t, arena_allocator(&ar), i, i < n ? &dump_tests[i] : &empty);
        arena_free(&ar);
    }
}

/* ----------------------------------------------------------------- responses */

typedef struct DumpResTest {
    int64_t content_length;
    const char *header; /* "Key: value" lines, or NULL */
    const char *body;   /* NULL for a nil body */
    bool chunked;
    bool dump_body;
    const char *want;
} DumpResTest;

static const DumpResTest dump_res_tests[] = {
    {.content_length = 50,
     .header = "Foo: Bar",
     .body = "foo",      /* shouldn't be used */
     .dump_body = false, /* to verify we see 50, not empty or 3. */
     .want = "HTTP/1.1 200 OK\n"
             "Content-Length: 50\n"
             "Foo: Bar"},
    {.content_length = 3,
     .body = "foo",
     .dump_body = true,
     .want = "HTTP/1.1 200 OK\n"
             "Content-Length: 3\n"
             "\n"
             "foo"},
    {.content_length = -1,
     .body = "foo",
     .chunked = true,
     .dump_body = true,
     .want = "HTTP/1.1 200 OK\n"
             "Transfer-Encoding: chunked\n"
             "\n"
             "3\n"
             "foo\n"
             "0"},
    {.content_length = 0,
     /* To verify if headers are not filtered out. */
     .header = "Foo1: Bar1\nFoo2: Bar2",
     .dump_body = false, /* to verify we see 0, not empty. */
     .want = "HTTP/1.1 200 OK\n"
             "Foo1: Bar1\n"
             "Foo2: Bar2\n"
             "Content-Length: 0"},
};

static HttpResponse *new_res(Alloc *a, const DumpResTest *tt) {
    HttpResponse *r = (HttpResponse *)mem_alloc(a, sizeof *r, _Alignof(HttpResponse));
    Str *te = (Str *)mem_alloc(a, sizeof *te, _Alignof(Str));
    if (r == NULL || te == NULL)
        return NULL;
    memset(r, 0, sizeof *r);
    r->status = cs("200 OK");
    r->status_code = 200;
    r->proto = cs("HTTP/1.1");
    r->proto_major = 1;
    r->proto_minor = 1;
    r->content_length = tt->content_length;
    if (tt->header != NULL) {
        r->header = http_header_make(a);
        Str rest = cs(tt->header);
        while (rest.len > 0) {
            Str line = rest;
            Int nl = strings_index_byte(rest, '\n');
            if (nl >= 0) {
                line = sub(rest, 0, nl);
                rest = sub(rest, nl + 1, rest.len);
            } else {
                rest = sub(rest, rest.len, rest.len);
            }
            Int colon = strings_index(line, cs(": "));
            (void)http_header_add(r->header, sub(line, 0, colon),
                                  sub(line, colon + 2, line.len));
        }
    }
    if (tt->body != NULL)
        r->body = body_of(a, cs(tt->body));
    if (tt->chunked) {
        te[0] = cs("chunked");
        r->transfer_encoding = slice_from(te, 1, 1, TYPE_STRING);
    }
    return r;
}

static void TestDumpResponse(TestingT *t) {
    Int n = (Int)(sizeof dump_res_tests / sizeof dump_res_tests[0]);
    for (Int i = 0; i < n; i++) {
        const DumpResTest *tt = &dump_res_tests[i];
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Alloc *a = arena_allocator(&ar);
        HttpResponse *res = new_res(a, tt);
        if (res == NULL) {
            arena_free(&ar);
            FATALF("%d: out of memory", i);
        }
        Error err;
        Slice gotb = httputil_dump_response(a, res, tt->dump_body, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%d. DumpResponse = %v", i, err);
        } else {
            Str got = strings_trim_space(slice_str(gotb));
            got = strings_replace_all(a, got, cs("\r"), cs(""));
            if (!str_eq(got, cs(tt->want)))
                testing_t_errorf_v(t, "%d.\nDumpResponse got:\n%s\n\nWant:\n%s\n", i,
                                   got, cs(tt->want));
        }
        arena_free(&ar);
    }
}

/* ----------------------------------------------------------- Issue 38352 */

typedef struct DumpOutJob {
    HttpRequest *req;
    Chan *out; /* of bool */
} DumpOutJob;

static void dump_out_job(void *env) {
    DumpOutJob *j = (DumpOutJob *)env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Error err;
    (void)httputil_dump_request_out(arena_allocator(&ar), j->req, true, &err);
    arena_free(&ar);
    bool done = true;
    chan_send(j->out, &done);
}

/* Issue 38352: Check for deadlock on canceled requests. */
static void TestDumpRequestOutIssue38352(TestingT *t) {
    if (testing_short())
        return;
    testing_t_parallel(t);

    Duration timeout = 10 * TIME_SECOND;
    for (Int i = 0; i < 1000; i++) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Alloc *a = arena_allocator(&ar);
        Duration delay = (Duration)math_rand_intn(5) * TIME_MILLISECOND;
        ContextCancelFunc cancel;
        Context ctx = context_with_timeout(a, context_background(), delay, &cancel);

        BytesBuffer *r = (BytesBuffer *)mem_alloc(a, sizeof *r, _Alignof(BytesBuffer));
        Slice zeros = slice_make(a, TYPE_BYTE, 10000, 10000);
        if (r == NULL || zeros.p == NULL) {
            arena_free(&ar);
            FATALF("out of memory");
        }
        memset(zeros.p, 0, (size_t)zeros.len);
        *r = BYTES_BUFFER(a);
        Error err;
        (void)bytes_buffer_write(r, zeros, &err);
        HttpRequest *req =
            http_new_request_with_context(a, ctx, cs("POST"), cs("http://example.com"),
                                          bytes_buffer_as_io_reader(r), &err);
        if (req == NULL) {
            BURROW_CALLF0(cancel);
            context_release(ctx);
            arena_free(&ar);
            FATALF("%v", err);
        }

        DumpOutJob job = {req, chan_make(a, TYPE_BOOL, 1)};
        Chan *after = time_after_chan(a, timeout);
        if (job.out == NULL || after == NULL ||
            !go(BURROW_FN(Func, dump_out_job, &job))) {
            arena_free(&ar);
            FATALF("out of memory");
        }
        SelectCase cases[] = {BURROW_RECV(job.out, NULL), BURROW_RECV(after, NULL)};
        if (chan_select(cases, 2) == 1) {
            testing_t_fatalf_v(
                t, "deadlock detected on iteration %d after %v with delay: %v", i,
                BURROW_ANY(TYPE_DURATION, &timeout), BURROW_ANY(TYPE_DURATION, &delay));
            return;
        }
        chan_free(after);
        http_request_free(req);
        BURROW_CALLF0(cancel);
        context_release(ctx);
        arena_free(&ar);
    }
}

/* ------------------------------------------------------------------- chunked */

static void TestChunk(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    BytesBuffer b = BYTES_BUFFER(a);
    Error err;

    IoWriteCloser w = httputil_new_chunked_writer(a, bytes_buffer_as_io_writer(&b));
    if (w.vt == NULL) {
        arena_free(&ar);
        FATALF("out of memory");
    }
    Str chunk1 = cs("hello, ");
    Str chunk2 = cs("world! 0123456789abcdef");
    (void)io_write_string(io_write_closer_as_io_writer(w), chunk1, &err);
    (void)io_write_string(io_write_closer_as_io_writer(w), chunk2, &err);
    (void)w.vt->closer.close(w.data);
    httputil_chunked_writer_free(a, w);

    Str g = slice_str(bytes_buffer_bytes(&b));
    Str e = cs("7\r\nhello, \r\n17\r\nworld! 0123456789abcdef\r\n0\r\n");
    if (!str_eq(g, e)) {
        testing_t_errorf_v(t, "chunk writer wrote %q; want %q", g, e);
        arena_free(&ar);
        return;
    }

    IoReader r = httputil_new_chunked_reader(a, bytes_buffer_as_io_reader(&b));
    Slice data = io_read_all(a, r, &err);
    if (BURROW_FAILED(err)) {
        testing_t_logf_v(t, "data: \"%s\"", slice_str(data));
        testing_t_errorf_v(t, "ReadAll from reader: %v", err);
    } else if (!str_eq(slice_str(data), fmt_sprintf_v(a, "%s%s", chunk1, chunk2))) {
        testing_t_errorf_v(t, "chunk reader read %q; want %q", slice_str(data),
                           fmt_sprintf_v(a, "%s%s", chunk1, chunk2));
    }
    httputil_chunked_reader_free(a, r);
    arena_free(&ar);
}

/* Not in Go. httputil.ErrLineTooLong is internal.ErrLineTooLong, so a line too
 * long for the chunked reader gives an error both of them match. */
static void TestChunkedReaderLineTooLong(TestingT *t) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str in = fmt_sprintf_v(a, "%s\r\n", strings_repeat(a, cs("1"), 5000));
    StringsReader *sr = strings_new_reader(a, in);
    if (sr == NULL) {
        arena_free(&ar);
        FATALF("out of memory");
    }
    IoReader r = httputil_new_chunked_reader(a, strings_reader_as_io_reader(sr));
    Error err;
    (void)io_read_all(a, r, &err);
    if (!errors_is(err, httputil_err_line_too_long))
        testing_t_errorf_v(t, "ReadAll = %v; want %v", err, httputil_err_line_too_long);
    if (!errors_is(err, burrow__http_err_line_too_long))
        testing_t_errorf_v(t, "errors_is(%v, the net/http error) = false", err);
    httputil_chunked_reader_free(a, r);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestDumpRequest)                                                                 \
    X(TestDumpResponse)                                                                \
    X(TestDumpRequestOutIssue38352)                                                    \
    X(TestChunk)                                                                       \
    X(TestChunkedReaderLineTooLong)

TESTING_MAIN(TESTS)
