/* Derived from Go's src/net/http/responsewrite_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/net/http.h"
#include "burrow/strings.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S

#define ARENA_BEGIN                                                                    \
    Arena ar;                                                                          \
    arena_init(&ar, NULL, 0);                                                          \
    Alloc *a = arena_allocator(&ar)
#define ARENA_END arena_free(&ar)

static Str cs(const char *s) {
    return s != NULL ? str_from_cstr(s) : (Str){0};
}

static void *must(void *p) {
    if (p == NULL)
        panic_str(S("out of memory"));
    return p;
}

/* Sets key to one value as it is, with no canonical form. */
static HttpHeader one_header(Alloc *a, const char *key, const char *value) {
    HttpHeader h = must(http_header_make(a));
    if (key == NULL)
        return h;
    Str *v = must(mem_alloc(a, sizeof(Str), _Alignof(Str)));
    *v = cs(value);
    Slice s = slice_from(v, 1, 1, TYPE_STRING);
    Str k = cs(key);
    if (!map_set(h, &k, &s))
        panic_str(S("out of memory"));
    return h;
}

/* dummyReq and dummyReq11. */
static HttpRequest *dummy_req(Alloc *a, const char *method, bool http11) {
    HttpRequest *r = must(mem_alloc(a, sizeof *r, _Alignof(HttpRequest)));
    r->method = cs(method);
    if (http11) {
        r->proto = S("HTTP/1.1");
        r->proto_major = 1;
        r->proto_minor = 1;
    }
    return r;
}

/* Which request the response is to: dummyReq, dummyReq11, or a bare
 * &Request{Method: "POST"}. */
enum { REQ_GET, REQ_GET11, REQ_POST };

typedef struct RespWriteTest {
    Int status_code;
    const char *status;
    const char *hkey, *hvalue; /* one header field, or none */
    const char *body;          /* io.NopCloser(strings.NewReader(body)), or nil */
    int64_t content_length;
    const char *raw;
    int major, minor;
    int req;
    bool chunked;
    bool close;
} RespWriteTest;

static const RespWriteTest resp_write_tests[] = {
    /* HTTP/1.0, identity coding; no trailer */
    {
        .status_code = 503,
        .major = 1,
        .minor = 0,
        .req = REQ_GET,
        .body = "abcdef",
        .content_length = 6,
        .raw = "HTTP/1.0 503 Service Unavailable\r\n"
               "Content-Length: 6\r\n\r\n"
               "abcdef",
    },
    /* Unchunked response without Content-Length. */
    {
        .status_code = 200,
        .major = 1,
        .minor = 0,
        .req = REQ_GET,
        .body = "abcdef",
        .content_length = -1,
        .raw = "HTTP/1.0 200 OK\r\n"
               "\r\n"
               "abcdef",
    },
    /* HTTP/1.1 response with unknown length and Connection: close */
    {
        .status_code = 200,
        .major = 1,
        .minor = 1,
        .req = REQ_GET,
        .body = "abcdef",
        .content_length = -1,
        .close = true,
        .raw = "HTTP/1.1 200 OK\r\n"
               "Connection: close\r\n"
               "\r\n"
               "abcdef",
    },
    /* HTTP/1.1 response with unknown length and not setting connection:
     * close */
    {
        .status_code = 200,
        .major = 1,
        .minor = 1,
        .req = REQ_GET11,
        .body = "abcdef",
        .content_length = -1,
        .close = false,
        .raw = "HTTP/1.1 200 OK\r\n"
               "Connection: close\r\n"
               "\r\n"
               "abcdef",
    },
    /* HTTP/1.1 response with unknown length and not setting connection:
     * close, but setting chunked. */
    {
        .status_code = 200,
        .major = 1,
        .minor = 1,
        .req = REQ_GET11,
        .body = "abcdef",
        .content_length = -1,
        .chunked = true,
        .close = false,
        .raw = "HTTP/1.1 200 OK\r\n"
               "Transfer-Encoding: chunked\r\n\r\n"
               "6\r\nabcdef\r\n0\r\n\r\n",
    },
    /* HTTP/1.1 response 0 content-length, and nil body */
    {
        .status_code = 200,
        .major = 1,
        .minor = 1,
        .req = REQ_GET11,
        .body = NULL,
        .content_length = 0,
        .close = false,
        .raw = "HTTP/1.1 200 OK\r\n"
               "Content-Length: 0\r\n"
               "\r\n",
    },
    /* HTTP/1.1 response 0 content-length, and non-nil empty body */
    {
        .status_code = 200,
        .major = 1,
        .minor = 1,
        .req = REQ_GET11,
        .body = "",
        .content_length = 0,
        .close = false,
        .raw = "HTTP/1.1 200 OK\r\n"
               "Content-Length: 0\r\n"
               "\r\n",
    },
    /* HTTP/1.1 response 0 content-length, and non-nil non-empty body */
    {
        .status_code = 200,
        .major = 1,
        .minor = 1,
        .req = REQ_GET11,
        .body = "foo",
        .content_length = 0,
        .close = false,
        .raw = "HTTP/1.1 200 OK\r\n"
               "Connection: close\r\n"
               "\r\nfoo",
    },
    /* HTTP/1.1, chunked coding; empty trailer; close */
    {
        .status_code = 200,
        .major = 1,
        .minor = 1,
        .req = REQ_GET,
        .body = "abcdef",
        .content_length = 6,
        .chunked = true,
        .close = true,
        .raw = "HTTP/1.1 200 OK\r\n"
               "Connection: close\r\n"
               "Transfer-Encoding: chunked\r\n\r\n"
               "6\r\nabcdef\r\n0\r\n\r\n",
    },
    /* Header value with a newline character (Issue 914). Also tests removal
     * of leading and trailing whitespace. */
    {
        .status_code = 204,
        .major = 1,
        .minor = 1,
        .req = REQ_GET,
        .hkey = "Foo",
        .hvalue = " Bar\nBaz ",
        .body = NULL,
        .content_length = 0,
        .chunked = true,
        .close = true,
        .raw = "HTTP/1.1 204 No Content\r\n"
               "Connection: close\r\n"
               "Foo: Bar Baz\r\n"
               "\r\n",
    },
    /* Want a single Content-Length header. Fixing issue 8180 where there were
     * two. */
    {
        .status_code = 200,
        .major = 1,
        .minor = 1,
        .req = REQ_POST,
        .content_length = 0,
        .body = NULL,
        .raw = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
    },
    /* When a response to a POST has Content-Length: -1, make sure we don't
     * write the Content-Length as -1. */
    {
        .status_code = 200,
        .major = 1,
        .minor = 1,
        .req = REQ_POST,
        .content_length = -1,
        .body = "abcdef",
        .raw = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nabcdef",
    },
    /* Status code under 100 should be zero-padded to three digits. Still
     * bogus, but less bogus. (be consistent with generating three digits,
     * since the Transport requires it) */
    {
        .status_code = 7,
        .status = "license to violate specs",
        .major = 1,
        .minor = 0,
        .req = REQ_GET,
        .body = NULL,
        .raw = "HTTP/1.0 007 license to violate specs\r\nContent-Length: 0\r\n\r\n",
    },
    /* No stutter. Status code in 1xx range response should not include a
     * Content-Length header. See issue #16942. */
    {
        .status_code = 123,
        .status = "123 Sesame Street",
        .major = 1,
        .minor = 0,
        .req = REQ_GET,
        .body = NULL,
        .raw = "HTTP/1.0 123 Sesame Street\r\n\r\n",
    },
    /* Status code 204 (No content) response should not include a
     * Content-Length header. See issue #16942. */
    {
        .status_code = 204,
        .status = "No Content",
        .major = 1,
        .minor = 0,
        .req = REQ_GET,
        .body = NULL,
        .raw = "HTTP/1.0 204 No Content\r\n\r\n",
    },
};

static const Str chunked_te[1] = {BURROW_S_INIT("chunked")};

static IoReadCloser nop_body(Alloc *a, const char *body) {
    StringsReader *sr = must(mem_alloc(a, sizeof *sr, _Alignof(StringsReader)));
    strings_reader_reset(sr, cs(body));
    IoNopCloser *nc = must(mem_alloc(a, sizeof *nc, _Alignof(IoNopCloser)));
    *nc = io_nop_closer(strings_reader_as_io_reader(sr));
    return io_nop_closer_as_io_read_closer(nc);
}

static void TestResponseWrite(TestingT *t) {
    ARENA_BEGIN;
    Int n = (Int)(sizeof resp_write_tests / sizeof resp_write_tests[0]);
    for (Int i = 0; i < n; i++) {
        const RespWriteTest *tt = &resp_write_tests[i];
        HttpResponse resp;
        memset(&resp, 0, sizeof resp);
        resp.status_code = tt->status_code;
        resp.status = cs(tt->status);
        resp.proto_major = tt->major;
        resp.proto_minor = tt->minor;
        resp.request =
            dummy_req(a, tt->req == REQ_POST ? "POST" : "GET", tt->req == REQ_GET11);
        resp.header = one_header(a, tt->hkey, tt->hvalue);
        if (tt->body != NULL)
            resp.body = nop_body(a, tt->body);
        resp.content_length = tt->content_length;
        if (tt->chunked)
            resp.transfer_encoding =
                slice_from((void *)(uintptr_t)chunked_te, 1, 1, TYPE_STRING);
        resp.close = tt->close;

        StringsBuilder braw = STRINGS_BUILDER(a);
        Error err = http_response_write(&resp, strings_builder_as_io_writer(&braw));
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "error writing #%d: %s", i, error_text(err));
            continue;
        }
        Str sraw = strings_builder_string(&braw);
        if (!str_eq(sraw, cs(tt->raw))) {
            testing_t_errorf_v(t, "Test %d, expecting:\n%q\nGot:\n%q\n", i, cs(tt->raw),
                               sraw);
            continue;
        }
    }
    ARENA_END;
}

/* Response.Write shares the trailer validation added for Issue #78775 with
 * Request.Write, so an invalid trailer name or value must be rejected rather
 * than written. */
typedef struct TrailerCase {
    const char *name;
    const char *key, *value;
    const char *want_err;
} TrailerCase;

static const TrailerCase trailer_cases[] = {
    {"key", "X-Trailer\r\nInjected: 1", "ok",
     "net/http: invalid trailer field name \"X-Trailer\\r\\nInjected: 1\""},
    {"value", "X-Trailer", "evil\r\nInjected: 1",
     "net/http: invalid trailer field value for \"X-Trailer\""},
};

static void invalid_trailer_case(void *env, TestingT *t) {
    const TrailerCase *tt = (const TrailerCase *)env;
    ARENA_BEGIN;
    HttpResponse resp;
    memset(&resp, 0, sizeof resp);
    resp.status_code = 200;
    resp.proto_major = 1;
    resp.proto_minor = 1;
    resp.request = dummy_req(a, "GET", false);
    resp.header = one_header(a, NULL, NULL);
    resp.body = nop_body(a, "abcdef");
    resp.content_length = -1;
    resp.transfer_encoding =
        slice_from((void *)(uintptr_t)chunked_te, 1, 1, TYPE_STRING);
    resp.trailer = one_header(a, tt->key, tt->value);
    StringsBuilder b = STRINGS_BUILDER(a);
    Error err = http_response_write(&resp, strings_builder_as_io_writer(&b));
    if (BURROW_OK(err) || !str_eq(error_text(err), cs(tt->want_err)))
        testing_t_fatalf_v(t, "Response.Write error = %s, want %q",
                           BURROW_OK(err) ? S("<nil>") : error_text(err),
                           cs(tt->want_err));
    ARENA_END;
}

static void TestResponseWriteInvalidTrailer(TestingT *t) {
    for (size_t i = 0; i < sizeof trailer_cases / sizeof trailer_cases[0]; i++)
        testing_t_run(t, cs(trailer_cases[i].name),
                      BURROW_FN(TestingTFunc, invalid_trailer_case,
                                (void *)(uintptr_t)&trailer_cases[i]));
}

#define TESTS(X)                                                                       \
    X(TestResponseWrite)                                                               \
    X(TestResponseWriteInvalidTrailer)

TESTING_MAIN(TESTS)
