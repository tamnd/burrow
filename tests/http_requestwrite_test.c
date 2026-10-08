/* Derived from Go's src/net/http/requestwrite_test.go, the tests of writing a
 * request to the wire.
 * Go source: go1.27.1.
 *
 * TestRequestWriteProbe is burrow's own. It runs the GET cases of
 * TestRequestWriteTransport through Request.Write, with no Transport.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/declare.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/testing/iotest.h"

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

/* A key and its values as a Go map literal has them. n is -1 for a nil
 * value. */
typedef struct RwKV {
    const char *key;
    const char *v[2];
    int n;
} RwKV;

static HttpHeader rw_header(Alloc *a, const RwKV *kvs) {
    HttpHeader h = must(http_header_make(a));
    for (const RwKV *kv = kvs; kv->key != NULL; kv++) {
        Slice s = slice_from(NULL, 0, 0, TYPE_STRING);
        if (kv->n >= 0) {
            Str *vs = must(mem_alloc(a, 2 * sizeof(Str), _Alignof(Str)));
            for (int i = 0; i < kv->n; i++)
                vs[i] = cs(kv->v[i]);
            s = slice_from(vs, kv->n, kv->n, TYPE_STRING);
        }
        Str key = cs(kv->key);
        if (!map_set(h, &key, &s))
            panic_str(S("out of memory"));
    }
    return h;
}

BURROW_SENTINEL_ERROR(rw_custom_error, "Custom reader error");

/* What Body is: nothing, a []byte, or one of the funcs that make a body. */
typedef enum RwBody {
    RW_BODY_NONE,
    RW_BODY_BYTES, /* io.NopCloser(bytes.NewReader(body)) */
    RW_BODY_LIMIT, /* io.NopCloser(io.LimitReader(strings.NewReader("xx"), limit)) */
    RW_BODY_NIL,   /* func() io.ReadCloser { return nil } */
    RW_BODY_X_THEN_ERR, /* "x" and then rw_custom_error */
    RW_BODY_ERR,        /* rw_custom_error at once */
} RwBody;

/* A URL as a literal gives it. */
typedef struct RwURL {
    const char *scheme;
    const char *host;
    const char *path;
    const char *opaque;
    const char *raw_query;
} RwURL;

typedef struct RwTest {
    const char *method;
    const char *parse; /* mustParseURL of this when it is not NULL */
    RwURL url;
    const char *proto;
    RwKV header[8];
    RwKV trailer[2];
    const char *te; /* TransferEncoding: []string{te} */
    int64_t content_length;
    const char *host;

    const char *body;
    int64_t limit;

    const char *want_write;
    const char *want_proxy;
    const char *want_error;
    int major, minor;
    RwBody body_kind;
    bool close;
} RwTest;

static const RwTest req_write_tests[] = {
    /* HTTP/1.1 => chunked coding; no body; no trailer */
    {
        .method = "GET",
        .url = {.scheme = "http", .host = "www.techcrunch.com", .path = "/"},
        .proto = "HTTP/1.1",
        .major = 1,
        .minor = 1,
        .header =
            {
                {"Accept",
                 {"text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
                 1},
                {"Accept-Charset", {"ISO-8859-1,utf-8;q=0.7,*;q=0.7"}, 1},
                {"Accept-Encoding", {"gzip,deflate"}, 1},
                {"Accept-Language", {"en-us,en;q=0.5"}, 1},
                {"Keep-Alive", {"300"}, 1},
                {"Proxy-Connection", {"keep-alive"}, 1},
                {"User-Agent", {"Fake"}, 1},
            },
        .host = "www.techcrunch.com",
        .want_write =
            "GET / HTTP/1.1\r\n"
            "Host: www.techcrunch.com\r\n"
            "User-Agent: Fake\r\n"
            "Accept: "
            "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
            "Accept-Charset: ISO-8859-1,utf-8;q=0.7,*;q=0.7\r\n"
            "Accept-Encoding: gzip,deflate\r\n"
            "Accept-Language: en-us,en;q=0.5\r\n"
            "Keep-Alive: 300\r\n"
            "Proxy-Connection: keep-alive\r\n\r\n",
        .want_proxy =
            "GET http://www.techcrunch.com/ HTTP/1.1\r\n"
            "Host: www.techcrunch.com\r\n"
            "User-Agent: Fake\r\n"
            "Accept: "
            "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
            "Accept-Charset: ISO-8859-1,utf-8;q=0.7,*;q=0.7\r\n"
            "Accept-Encoding: gzip,deflate\r\n"
            "Accept-Language: en-us,en;q=0.5\r\n"
            "Keep-Alive: 300\r\n"
            "Proxy-Connection: keep-alive\r\n\r\n",
    },
    /* HTTP/1.1 => chunked coding; body; empty trailer */
    {
        .method = "GET",
        .url = {.scheme = "http", .host = "www.google.com", .path = "/search"},
        .major = 1,
        .minor = 1,
        .te = "chunked",
        .body_kind = RW_BODY_BYTES,
        .body = "abcdef",
        .want_write = "GET /search HTTP/1.1\r\n"
                      "Host: www.google.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Transfer-Encoding: chunked\r\n\r\n"
                      "6\r\nabcdef\r\n0\r\n\r\n",
        .want_proxy = "GET http://www.google.com/search HTTP/1.1\r\n"
                      "Host: www.google.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Transfer-Encoding: chunked\r\n\r\n"
                      "6\r\nabcdef\r\n0\r\n\r\n",
    },
    /* HTTP/1.1 POST => chunked coding; body; empty trailer */
    {
        .method = "POST",
        .url = {.scheme = "http", .host = "www.google.com", .path = "/search"},
        .major = 1,
        .minor = 1,
        .close = true,
        .te = "chunked",
        .body_kind = RW_BODY_BYTES,
        .body = "abcdef",
        .want_write = "POST /search HTTP/1.1\r\n"
                      "Host: www.google.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Connection: close\r\n"
                      "Transfer-Encoding: chunked\r\n\r\n"
                      "6\r\nabcdef\r\n0\r\n\r\n",
        .want_proxy = "POST http://www.google.com/search HTTP/1.1\r\n"
                      "Host: www.google.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Connection: close\r\n"
                      "Transfer-Encoding: chunked\r\n\r\n"
                      "6\r\nabcdef\r\n0\r\n\r\n",
    },
    /* HTTP/1.1 POST with Content-Length, no chunking */
    {
        .method = "POST",
        .url = {.scheme = "http", .host = "www.google.com", .path = "/search"},
        .major = 1,
        .minor = 1,
        .close = true,
        .content_length = 6,
        .body_kind = RW_BODY_BYTES,
        .body = "abcdef",
        .want_write = "POST /search HTTP/1.1\r\n"
                      "Host: www.google.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Connection: close\r\n"
                      "Content-Length: 6\r\n"
                      "\r\n"
                      "abcdef",
        .want_proxy = "POST http://www.google.com/search HTTP/1.1\r\n"
                      "Host: www.google.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Connection: close\r\n"
                      "Content-Length: 6\r\n"
                      "\r\n"
                      "abcdef",
    },
    /* HTTP/1.1 POST with Content-Length in headers */
    {
        .method = "POST",
        .parse = "http://example.com/",
        .host = "example.com",
        .header = {{"Content-Length", {"10"}, 1}}, /* ignored */
        .content_length = 6,
        .body_kind = RW_BODY_BYTES,
        .body = "abcdef",
        .want_write = "POST / HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Content-Length: 6\r\n"
                      "\r\n"
                      "abcdef",
        .want_proxy = "POST http://example.com/ HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Content-Length: 6\r\n"
                      "\r\n"
                      "abcdef",
    },
    /* default to HTTP/1.1 */
    {
        .method = "GET",
        .parse = "/search",
        .host = "www.google.com",
        .want_write = "GET /search HTTP/1.1\r\n"
                      "Host: www.google.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "\r\n",
    },
    /* Request with a 0 ContentLength and a 0 byte body. */
    {
        .method = "POST",
        .parse = "/",
        .host = "example.com",
        .major = 1,
        .minor = 1,
        .content_length = 0, /* as if unset by user */
        .body_kind = RW_BODY_LIMIT,
        .limit = 0,
        .want_write = "POST / HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n0\r\n\r\n",
        .want_proxy = "POST / HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "\r\n0\r\n\r\n",
    },
    /* Request with a 0 ContentLength and a nil body. */
    {
        .method = "POST",
        .parse = "/",
        .host = "example.com",
        .major = 1,
        .minor = 1,
        .content_length = 0, /* as if unset by user */
        .body_kind = RW_BODY_NIL,
        .want_write = "POST / HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Content-Length: 0\r\n"
                      "\r\n",
        .want_proxy = "POST / HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Content-Length: 0\r\n"
                      "\r\n",
    },
    /* Request with a 0 ContentLength and a 1 byte body. */
    {
        .method = "POST",
        .parse = "/",
        .host = "example.com",
        .major = 1,
        .minor = 1,
        .content_length = 0, /* as if unset by user */
        .body_kind = RW_BODY_LIMIT,
        .limit = 1,
        .want_write = "POST / HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Transfer-Encoding: chunked\r\n\r\n"
                      "1\r\nx\r\n0\r\n\r\n",
        .want_proxy = "POST / HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Transfer-Encoding: chunked\r\n\r\n"
                      "1\r\nx\r\n0\r\n\r\n",
    },
    /* Request with a ContentLength of 10 but a 5 byte body. */
    {
        .method = "POST",
        .parse = "/",
        .host = "example.com",
        .major = 1,
        .minor = 1,
        .content_length = 10, /* but we're going to send only 5 bytes */
        .body_kind = RW_BODY_BYTES,
        .body = "12345",
        .want_error = "http: ContentLength=10 with Body length 5",
    },
    /* Request with a ContentLength of 4 but an 8 byte body. */
    {
        .method = "POST",
        .parse = "/",
        .host = "example.com",
        .major = 1,
        .minor = 1,
        .content_length = 4, /* but we're going to try to send 8 bytes */
        .body_kind = RW_BODY_BYTES,
        .body = "12345678",
        .want_error = "http: ContentLength=4 with Body length 8",
    },
    /* Request with a 5 ContentLength and nil body. */
    {
        .method = "POST",
        .parse = "/",
        .host = "example.com",
        .major = 1,
        .minor = 1,
        .content_length = 5, /* but we'll omit the body */
        .want_error = "http: Request.ContentLength=5 with nil Body",
    },
    /* Request with a 0 ContentLength and a body with 1 byte content and an
     * error. */
    {
        .method = "POST",
        .parse = "/",
        .host = "example.com",
        .major = 1,
        .minor = 1,
        .content_length = 0, /* as if unset by user */
        .body_kind = RW_BODY_X_THEN_ERR,
        .want_error = "Custom reader error",
    },
    /* Request with a 0 ContentLength and a body without content and an
     * error. */
    {
        .method = "POST",
        .parse = "/",
        .host = "example.com",
        .major = 1,
        .minor = 1,
        .content_length = 0, /* as if unset by user */
        .body_kind = RW_BODY_ERR,
        .want_error = "Custom reader error",
    },
    /* Verify that DumpRequest preserves the HTTP version number, doesn't add
     * a Host, and doesn't add a User-Agent. */
    {
        .method = "GET",
        .parse = "/foo",
        .major = 1,
        .minor = 0,
        .header = {{"X-Foo", {"X-Bar"}, 1}},
        .want_write = "GET /foo HTTP/1.1\r\n"
                      "Host: \r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "X-Foo: X-Bar\r\n\r\n",
    },
    /* If no Request.Host and no Request.URL.Host, we send an empty Host
     * header, and don't use Request.Header["Host"]. This is just testing that
     * we don't change Go 1.0 behavior. */
    {
        .method = "GET",
        .host = "",
        .url = {.scheme = "http", .host = "", .path = "/search"},
        .major = 1,
        .minor = 1,
        .header = {{"Host", {"bad.example.com"}, 1}},
        .want_write = "GET /search HTTP/1.1\r\n"
                      "Host: \r\n"
                      "User-Agent: Go-http-client/1.1\r\n\r\n",
    },
    /* Opaque test #1 from golang.org/issue/4860 */
    {
        .method = "GET",
        .url = {.scheme = "http", .host = "www.google.com", .opaque = "/%2F/%2F/"},
        .major = 1,
        .minor = 1,
        .want_write = "GET /%2F/%2F/ HTTP/1.1\r\n"
                      "Host: www.google.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n\r\n",
    },
    /* Opaque test #2 from golang.org/issue/4860 */
    {
        .method = "GET",
        .url = {.scheme = "http",
                .host = "x.google.com",
                .opaque = "//y.google.com/%2F/%2F/"},
        .major = 1,
        .minor = 1,
        .want_write = "GET http://y.google.com/%2F/%2F/ HTTP/1.1\r\n"
                      "Host: x.google.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n\r\n",
    },
    /* Testing custom case in header keys. Issue 5022. */
    {
        .method = "GET",
        .url = {.scheme = "http", .host = "www.google.com", .path = "/"},
        .proto = "HTTP/1.1",
        .major = 1,
        .minor = 1,
        .header = {{"ALL-CAPS", {"x"}, 1}},
        .want_write = "GET / HTTP/1.1\r\n"
                      "Host: www.google.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "ALL-CAPS: x\r\n"
                      "\r\n",
    },
    /* Request with host header field; IPv6 address with zone identifier */
    {
        .method = "GET",
        .url = {.host = "[fe80::1%en0]"},
        .want_write = "GET / HTTP/1.1\r\n"
                      "Host: [fe80::1]\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "\r\n",
    },
    /* Request with optional host header field; IPv6 address with zone
     * identifier */
    {
        .method = "GET",
        .url = {.host = "www.example.com"},
        .host = "[fe80::1%en0]:8080",
        .want_write = "GET / HTTP/1.1\r\n"
                      "Host: [fe80::1]:8080\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "\r\n",
    },
    /* CONNECT without Opaque */
    {
        .method = "CONNECT",
        .url = {.scheme = "https", .host = "proxy.com"}, /* of proxy.com */
        /* What we used to do, locking that behavior in: */
        .want_write = "CONNECT proxy.com HTTP/1.1\r\n"
                      "Host: proxy.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "\r\n",
    },
    /* CONNECT with Opaque */
    {
        .method = "CONNECT",
        .url = {.scheme = "https", .host = "proxy.com", .opaque = "backend:443"},
        .want_write = "CONNECT backend:443 HTTP/1.1\r\n"
                      "Host: proxy.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "\r\n",
    },
    /* Verify that a nil header value doesn't get written. */
    {
        .method = "GET",
        .parse = "/foo",
        .header = {{"X-Foo", {"X-Bar"}, 1}, {"X-Idempotency-Key", {NULL}, -1}},
        .want_write = "GET /foo HTTP/1.1\r\n"
                      "Host: \r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "X-Foo: X-Bar\r\n\r\n",
    },
    {
        .method = "GET",
        .parse = "/foo",
        .header = {{"X-Foo", {"X-Bar"}, 1}, {"X-Idempotency-Key", {NULL}, 0}},
        .want_write = "GET /foo HTTP/1.1\r\n"
                      "Host: \r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "X-Foo: X-Bar\r\n\r\n",
    },
    {
        .method = "GET",
        .url = {.host = "www.example.com", .raw_query = "new\nline"}, /* or any CTL */
        .want_error = "net/http: can't write control character in Request.URL",
    },
    /* Request with nil body and PATCH method. Issue #40978 */
    {
        .method = "PATCH",
        .parse = "/",
        .host = "example.com",
        .major = 1,
        .minor = 1,
        .content_length = 0, /* as if unset by user */
        .want_write = "PATCH / HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Content-Length: 0\r\n\r\n",
        .want_proxy = "PATCH / HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Content-Length: 0\r\n\r\n",
    },
    /* Valid Trailer keeps working after trailer validation. Issue #78775 */
    {
        .method = "POST",
        .url = {.scheme = "http", .host = "example.com", .path = "/"},
        .major = 1,
        .minor = 1,
        .te = "chunked",
        .trailer = {{"X-Trailer", {"ok"}, 1}},
        .body_kind = RW_BODY_BYTES,
        .body = "abcdef",
        .want_write = "POST / HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "User-Agent: Go-http-client/1.1\r\n"
                      "Transfer-Encoding: chunked\r\n"
                      "Trailer: X-Trailer\r\n\r\n"
                      "6\r\nabcdef\r\n"
                      "0\r\n"
                      "X-Trailer: ok\r\n"
                      "\r\n",
    },
    /* Trailer names with control characters must not reach the wire, where
     * they would permit header injection on the "Trailer:" line. Issue
     * #78775 */
    {
        .method = "POST",
        .url = {.scheme = "http", .host = "example.com", .path = "/"},
        .major = 1,
        .minor = 1,
        .te = "chunked",
        .trailer = {{"X-Trailer\r\nInjected: 1", {"ok"}, 1}},
        .body_kind = RW_BODY_BYTES,
        .body = "abcdef",
        .want_error =
            "net/http: invalid trailer field name \"X-Trailer\\r\\nInjected: 1\"",
    },
    /* Trailer values with control characters are rejected as well. Issue
     * #78775 */
    {
        .method = "POST",
        .url = {.scheme = "http", .host = "example.com", .path = "/"},
        .major = 1,
        .minor = 1,
        .te = "chunked",
        .trailer = {{"X-Trailer", {"evil\r\nInjected: 1"}, 1}},
        .body_kind = RW_BODY_BYTES,
        .body = "abcdef",
        .want_error = "net/http: invalid trailer field value for \"X-Trailer\"",
    },
    /* An empty Trailer name is rejected. Issue #78775 */
    {
        .method = "POST",
        .url = {.scheme = "http", .host = "example.com", .path = "/"},
        .major = 1,
        .minor = 1,
        .te = "chunked",
        .trailer = {{"", {"ok"}, 1}},
        .body_kind = RW_BODY_BYTES,
        .body = "abcdef",
        .want_error = "net/http: invalid trailer field name \"\"",
    },
    /* A later (non-first) value in a Trailer is validated too. Issue #78775 */
    {
        .method = "POST",
        .url = {.scheme = "http", .host = "example.com", .path = "/"},
        .major = 1,
        .minor = 1,
        .te = "chunked",
        .trailer = {{"X-Trailer", {"ok", "evil\r\nInjected: 1"}, 2}},
        .body_kind = RW_BODY_BYTES,
        .body = "abcdef",
        .want_error = "net/http: invalid trailer field value for \"X-Trailer\"",
    },
};

/* setBody, which makes a fresh body each time it is called, the way the
 * funcs in the table do. */
static void rw_set_body(Alloc *a, HttpRequest *r, const RwTest *tt) {
    IoNopCloser *nc = must(mem_alloc(a, sizeof *nc, _Alignof(IoNopCloser)));
    switch (tt->body_kind) {
    case RW_BODY_NONE:
        return;
    case RW_BODY_NIL:
        r->body = (IoReadCloser){0};
        return;
    case RW_BODY_BYTES: {
        Str b = cs(tt->body);
        BytesReader *br = must(mem_alloc(a, sizeof *br, _Alignof(BytesReader)));
        bytes_reader_reset(br,
                           slice_from((void *)(uintptr_t)b.p, b.len, b.len, TYPE_BYTE));
        *nc = io_nop_closer(bytes_reader_as_io_reader(br));
        break;
    }
    case RW_BODY_LIMIT: {
        StringsReader *sr = must(mem_alloc(a, sizeof *sr, _Alignof(StringsReader)));
        strings_reader_reset(sr, S("xx"));
        IoLimitedReader *lr = must(mem_alloc(a, sizeof *lr, _Alignof(IoLimitedReader)));
        *lr = io_limit_reader(strings_reader_as_io_reader(sr), tt->limit);
        *nc = io_nop_closer(io_limited_reader_as_io_reader(lr));
        break;
    }
    case RW_BODY_X_THEN_ERR: {
        StringsReader *sr = must(mem_alloc(a, sizeof *sr, _Alignof(StringsReader)));
        strings_reader_reset(sr, S("x"));
        IoReader rs[2] = {strings_reader_as_io_reader(sr),
                          iotest_err_reader(a, rw_custom_error)};
        *nc = io_nop_closer(io_multi_reader(a, rs, 2));
        break;
    }
    case RW_BODY_ERR:
        *nc = io_nop_closer(iotest_err_reader(a, rw_custom_error));
        break;
    default:
        return;
    }
    r->body = io_nop_closer_as_io_read_closer(nc);
}

static HttpRequest *rw_request(Alloc *a, const RwTest *tt) {
    HttpRequest *r = must(mem_alloc(a, sizeof *r, _Alignof(HttpRequest)));
    r->method = cs(tt->method);
    if (tt->parse != NULL) {
        Error err;
        r->url = url_parse(a, cs(tt->parse), &err);
        if (r->url == NULL)
            panic_str(S("Error parsing URL"));
    } else {
        Url *u = must(mem_alloc(a, sizeof *u, _Alignof(Url)));
        u->scheme = cs(tt->url.scheme);
        u->host = cs(tt->url.host);
        u->path = cs(tt->url.path);
        u->opaque = cs(tt->url.opaque);
        u->raw_query = cs(tt->url.raw_query);
        r->url = u;
    }
    r->proto = cs(tt->proto);
    r->proto_major = tt->major;
    r->proto_minor = tt->minor;
    r->header = rw_header(a, tt->header);
    if (tt->trailer[0].key != NULL)
        r->trailer = rw_header(a, tt->trailer);
    if (tt->te != NULL) {
        Str *te = must(mem_alloc(a, sizeof(Str), _Alignof(Str)));
        *te = cs(tt->te);
        r->transfer_encoding = slice_from(te, 1, 1, TYPE_STRING);
    }
    r->content_length = tt->content_length;
    r->close = tt->close;
    r->host = cs(tt->host);
    return r;
}

/* CHECK_STR_EQ for a Str. */
#define CHECK_S(got, want)                                                             \
    do {                                                                               \
        Str g_ = (got), w_ = (want);                                                   \
        if (!str_eq(g_, w_))                                                           \
            testing_t_errorf_v(t, "got %q, want %q", g_, w_);                          \
    } while (0)

static Str err_v(Error err) {
    return BURROW_OK(err) ? S("<nil>") : error_text(err);
}

static void TestRequestWrite(TestingT *t) {
    ARENA_BEGIN;
    Int n = (Int)(sizeof req_write_tests / sizeof req_write_tests[0]);
    for (Int i = 0; i < n; i++) {
        const RwTest *tt = &req_write_tests[i];
        HttpRequest *req = rw_request(a, tt);
        rw_set_body(a, req, tt);

        StringsBuilder braw = STRINGS_BUILDER(a);
        Error err = http_request_write(req, strings_builder_as_io_writer(&braw));
        Str g = err_v(err);
        Str e = tt->want_error != NULL ? cs(tt->want_error) : S("<nil>");
        if (!str_eq(g, e)) {
            testing_t_errorf_v(t, "writing #%d, err = %q, want %q", i, g, e);
            continue;
        }
        if (BURROW_FAILED(err))
            continue;

        if (tt->want_write != NULL) {
            Str sraw = strings_builder_string(&braw);
            if (!str_eq(sraw, cs(tt->want_write))) {
                testing_t_errorf_v(t, "Test %d, expecting:\n%s\nGot:\n%s\n", i,
                                   cs(tt->want_write), sraw);
                continue;
            }
        }

        if (tt->want_proxy != NULL) {
            rw_set_body(a, req, tt);
            StringsBuilder praw = STRINGS_BUILDER(a);
            err = http_request_write_proxy(req, strings_builder_as_io_writer(&praw));
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "WriteProxy #%d: %s", i, error_text(err));
                continue;
            }
            Str sraw = strings_builder_string(&praw);
            if (!str_eq(sraw, cs(tt->want_proxy))) {
                testing_t_errorf_v(t, "Test Proxy %d, expecting:\n%s\nGot:\n%s\n", i,
                                   cs(tt->want_proxy), sraw);
                continue;
            }
        }
    }
    ARENA_END;
}

/* closeChecker: a reader that notes it was closed. */
typedef struct CloseChecker {
    StringsReader r;
    bool closed;
} CloseChecker;

static Int close_checker_read(void *self, Slice p, Error *err) {
    CloseChecker *c = (CloseChecker *)self;
    return strings_reader_read(&c->r, p, err);
}

static Error close_checker_close(void *self) {
    ((CloseChecker *)self)->closed = true;
    return BURROW_NO_ERROR;
}

static const IoReadCloserVT close_checker_vt = {
    {NULL, close_checker_read},
    {NULL, close_checker_close},
};

/* TestRequestWriteClosesBody tests that Request.Write closes its request.Body.
 * It also indirectly tests NewRequest and that it serializes it correctly.
 * NewRequest here takes a reader, and wraps what it is given in a nop closer,
 * so the body that gets closed is the request's own, set after. */
static void TestRequestWriteClosesBody(TestingT *t) {
    ARENA_BEGIN;
    CloseChecker rc = {0};
    strings_reader_reset(&rc.r, S("my body"));
    IoReadCloser body = {&close_checker_vt, &rc};
    Error err;
    HttpRequest *req = http_new_request(a, S("POST"), S("http://foo.com/"),
                                        io_read_closer_as_io_reader(body), &err);
    if (req == NULL) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        ARENA_END;
        return;
    }
    req->body = body;
    StringsBuilder buf = STRINGS_BUILDER(a);
    err = http_request_write(req, strings_builder_as_io_writer(&buf));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s", error_text(err));
    if (!rc.closed)
        testing_t_error_v(t, "body not closed after write");
    Str expected = S("POST / HTTP/1.1\r\n"
                     "Host: foo.com\r\n"
                     "User-Agent: Go-http-client/1.1\r\n"
                     "Transfer-Encoding: chunked\r\n\r\n"
                     "7\r\nmy body\r\n"
                     "0\r\n\r\n");
    if (!str_eq(strings_builder_string(&buf), expected))
        testing_t_errorf_v(t, "write:\n got: %s\nwant: %s",
                           strings_builder_string(&buf), expected);
    http_request_free(req);
    ARENA_END;
}

/* The writer of TestRequestWriteError, which has a WriteByte so that it is not
 * wrapped in a BufioWriter, and fails exactly once on its Nth Write. */
typedef struct FailWriter {
    int fail_after;
    int write_count;
} FailWriter;

BURROW_SENTINEL_ERROR(rw_err_fail, "fake write failure");

static Error fail_writer_write_byte(FailWriter *w, Byte c) {
    (void)w;
    (void)c;
    panic_str(S("WriteByte is only there to be seen"));
}

static Int fail_writer_write(void *self, Slice p, Error *err) {
    FailWriter *w = (FailWriter *)self;
    w->write_count++;
    *err = w->fail_after == 0 ? rw_err_fail : BURROW_NO_ERROR;
    w->fail_after--;
    return p.len;
}

#define FAIL_WRITER_SIG_WRITE_BYTE(IN, OUT) IN(0, Byte) OUT(Error)
#define FAIL_WRITER_METHODS(M, T)                                                      \
    M(T, WriteByte, fail_writer_write_byte, FAIL_WRITER_SIG_WRITE_BYTE)
BURROW_METHODS_DEFINE(FailWriter, FAIL_WRITER_METHODS);

static const Type fail_writer_type = {
    {(const Byte *)"failWriter", 10},
    {(const Byte *)"http_test", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(FailWriter),
    (uint16_t)_Alignof(FailWriter),
    0,
    (uint16_t)(sizeof burrow__methods_FailWriter /
               sizeof burrow__methods_FailWriter[0]),
    NULL,
    burrow__methods_FailWriter,
    NULL,
    NULL,
    0,
    0x66777274U,
    NULL,
};

static const IoWriterVT fail_writer_vt = {&fail_writer_type, fail_writer_write};

/* TestRequestWriteError tests the Write err != nil checks in (*Request).write. */
static void TestRequestWriteError(TestingT *t) {
    ARENA_BEGIN;
    FailWriter fw = {0};
    IoWriter w = {&fail_writer_vt, &fw};

    Error err;
    HttpRequest *req =
        http_new_request(a, S("GET"), S("http://example.com/"), (IoReader){0}, &err);
    if (req == NULL) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        ARENA_END;
        return;
    }
    enum { write_calls = 4 }; /* number of Write calls in current implementation */
    bool saw_good = false;
    for (int n = 0; n <= write_calls + 2; n++) {
        fw.fail_after = n;
        fw.write_count = 0;
        err = http_request_write(req, w);
        Error want_err = n < write_calls ? rw_err_fail : BURROW_NO_ERROR;
        if (err.vt != want_err.vt || err.data != want_err.data) {
            testing_t_errorf_v(t, "for fail-after %d Writes, err = %s; want %s", n,
                               err_v(err), err_v(want_err));
            continue;
        }
        if (BURROW_OK(err)) {
            saw_good = true;
            if (fw.write_count != write_calls) {
                testing_t_fatalf_v(t, "writeCalls constant is outdated in test");
                break;
            }
        }
        if (fw.write_count > write_calls || fw.write_count > n + 1)
            testing_t_errorf_v(
                t, "for fail-after %d, saw unexpectedly high (%d) write calls", n,
                fw.write_count);
    }
    if (!saw_good)
        testing_t_fatalf_v(t, "writeCalls constant is outdated in test");
    http_request_free(req);
    ARENA_END;
}

/* The GET cases of TestRequestWriteTransport, written straight to a builder
 * rather than through a Transport. A GET body of unknown length is read a byte
 * ahead to see whether it is empty, on a goroutine of its own with a timer,
 * which works because the test itself runs on a goroutine. */
typedef struct ProbeCase {
    const char *method;
    int64_t clen;
    const char *body; /* NULL for no body */
    const char *want[3];
    bool want_no_length;
} ProbeCase;

static const ProbeCase probe_cases[] = {
    {"GET", 0, NULL, {NULL}, true},
    {"GET", 0, "", {NULL}, true},
    {"GET", -1, "", {NULL}, true},
    /* A GET with a body, with explicit content length: */
    {"GET", 7, "foobody", {"Content-Length: 7", "foobody"}, false},
    /* A GET with a body, sniffing the leading "f" from "foobody". */
    {"GET",
     -1,
     "foobody",
     {"Transfer-Encoding: chunked", "\r\n1\r\nf\r\n", "oobody"},
     false},
    /* But a POST request is expected to have a body, so no sniffing
     * happens: */
    {"POST", -1, "foobody", {"Transfer-Encoding: chunked", "foobody"}, false},
    {"POST", -1, "", {"Transfer-Encoding: chunked"}, false},
};

typedef struct ProbeRun {
    Str out[sizeof probe_cases / sizeof probe_cases[0]];
    Str errs[sizeof probe_cases / sizeof probe_cases[0]];
    Alloc *a;
} ProbeRun;

static void probe_main(void *env) {
    ProbeRun *run = (ProbeRun *)env;
    Alloc *a = run->a;
    for (size_t i = 0; i < sizeof probe_cases / sizeof probe_cases[0]; i++) {
        const ProbeCase *tc = &probe_cases[i];
        HttpRequest *req = must(mem_alloc(a, sizeof *req, _Alignof(HttpRequest)));
        Url *u = must(mem_alloc(a, sizeof *u, _Alignof(Url)));
        u->scheme = S("http");
        u->host = S("example.com");
        req->method = cs(tc->method);
        req->url = u;
        req->header = must(http_header_make(a));
        req->content_length = tc->clen;
        if (tc->body != NULL) {
            /* Behind a LimitReader, so that it is not a reader known to be in
             * memory and the probe really runs. */
            StringsReader *sr = must(mem_alloc(a, sizeof *sr, _Alignof(StringsReader)));
            strings_reader_reset(sr, cs(tc->body));
            IoLimitedReader *lr =
                must(mem_alloc(a, sizeof *lr, _Alignof(IoLimitedReader)));
            *lr = io_limit_reader(strings_reader_as_io_reader(sr), 1 << 20);
            IoNopCloser *nc = must(mem_alloc(a, sizeof *nc, _Alignof(IoNopCloser)));
            *nc = io_nop_closer(io_limited_reader_as_io_reader(lr));
            req->body = io_nop_closer_as_io_read_closer(nc);
        }
        StringsBuilder b = STRINGS_BUILDER(a);
        Error err = http_request_write(req, strings_builder_as_io_writer(&b));
        run->errs[i] = BURROW_OK(err) ? (Str){0} : str_clone(a, error_text(err));
        run->out[i] = strings_builder_string(&b);
    }
}

static bool contains(Str s, const char *sub) {
    return strings_contains(s, cs(sub));
}

static void TestRequestWriteProbe(TestingT *t) {
    ARENA_BEGIN;
    ProbeRun run = {.a = a};
    probe_main(&run);
    for (size_t i = 0; i < sizeof probe_cases / sizeof probe_cases[0]; i++) {
        const ProbeCase *tc = &probe_cases[i];
        Str got = run.out[i];
        if (run.errs[i].len > 0) {
            testing_t_errorf_v(t, "test[%d]: %s", (Int)i, run.errs[i]);
            continue;
        }
        if (tc->want_no_length) {
            if (contains(got, "Content-Length: "))
                testing_t_errorf_v(t,
                                   "test[%d]: unexpected Content-Length in request: %s",
                                   (Int)i, got);
            if (contains(got, "Transfer-Encoding: "))
                testing_t_errorf_v(
                    t, "test[%d]: unexpected Transfer-Encoding in request: %s", (Int)i,
                    got);
        }
        for (int j = 0; j < 3 && tc->want[j] != NULL; j++)
            if (!contains(got, tc->want[j]))
                testing_t_errorf_v(t, "test[%d]: expected substring %q in request: %s",
                                   (Int)i, cs(tc->want[j]), got);
    }
    ARENA_END;
}

/* NewRequest with each of the readers it knows the length of, and GetBody
 * giving the body again. The expected values are what Go gives for the same
 * calls. */
static void TestNewRequestContentLength(TestingT *t) {
    ARENA_BEGIN;
    StringsReader sr;
    strings_reader_reset(&sr, S("hello"));
    Error err;
    HttpRequest *req = http_new_request(a, S(""), S("http://example.com:/x"),
                                        strings_reader_as_io_reader(&sr), &err);
    if (req == NULL) {
        testing_t_fatalf_v(t, "%s", error_text(err));
        ARENA_END;
        return;
    }
    CHECK_S(req->method, S("GET"));
    CHECK_S(req->host, S("example.com"));
    CHECK_S(req->proto, S("HTTP/1.1"));
    CHECK_INT_EQ(req->content_length, 5);
    if (req->get_body.f == NULL) {
        testing_t_fatalf_v(t, "GetBody is nil");
    } else {
        for (int k = 0; k < 2; k++) {
            IoReadCloser rc = req->get_body.f(req->get_body.env, &err);
            Slice all = io_read_all(a, io_read_closer_as_io_reader(rc), &err);
            CHECK(BURROW_OK(err));
            CHECK_S(str_from_bytes((const Byte *)all.p, all.len), S("hello"));
        }
    }
    http_request_free(req);

    StringsReader empty;
    strings_reader_reset(&empty, S(""));
    req = http_new_request(a, S("POST"), S("http://example.com/"),
                           strings_reader_as_io_reader(&empty), &err);
    CHECK(req != NULL);
    if (req != NULL) {
        CHECK_INT_EQ(req->content_length, 0);
        CHECK(req->body.vt == http_no_body.vt);
        http_request_free(req);
    }

    req = http_new_request(a, S("bad method"), S("http://example.com/"), (IoReader){0},
                           &err);
    CHECK(req == NULL);
    CHECK_S(err_v(err), S("net/http: invalid method \"bad method\""));
    ARENA_END;
}

/* -------------------------------------------- TestRequestWriteTransport */

/* dumpRequestOut, the test's own and not httputil's. A Transport sends req to
 * a connection that records what it is given, and that answers with a dummy
 * response once it has read the whole request. on_read_headers, when it is
 * set, runs once the request's header has been read. It always dumps the
 * whole body. */
typedef struct RwDump {
    BytesBuffer buf; /* records the output */
    IoPipeReader *pr;
    IoPipeWriter *pw;
    IoWriter mw; /* to buf and pw */

    /* delegateReader. c gets a value when the response is there to be read,
     * and is closed instead when the round trip failed. */
    Chan *c;
    bool have;
    StringsReader res;

    Chan *quit;
    Func on_read_headers;
    SyncWaitGroup wg;
} RwDump;

static Int rw_conn_read(void *self, Slice p, Error *err) {
    RwDump *d = (RwDump *)self;
    if (!d->have) {
        bool v;
        if (!chan_recv(d->c, &v)) {
            *err = io_eof;
            return 0;
        }
        d->have = true;
    }
    return strings_reader_read(&d->res, p, err);
}

static Int rw_conn_write(void *self, Slice p, Error *err) {
    RwDump *d = (RwDump *)self;
    return d->mw.vt->write(d->mw.data, p, err);
}

static Error rw_conn_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static NetAddr rw_conn_addr(void *self) {
    (void)self;
    return (NetAddr){NULL, NULL};
}

static Error rw_conn_set_deadline(void *self, Time tm) {
    (void)self;
    (void)tm;
    return BURROW_NO_ERROR;
}

static const NetConnVT rw_conn_vt = {
    {NULL, rw_conn_read}, {NULL, rw_conn_write}, {NULL, rw_conn_close},
    rw_conn_addr,         rw_conn_addr,          rw_conn_set_deadline,
    rw_conn_set_deadline, rw_conn_set_deadline,
};

static NetConn rw_dial(void *env, Str network, Str addr, Error *err) {
    (void)network;
    (void)addr;
    *err = BURROW_NO_ERROR;
    return (NetConn){&rw_conn_vt, env};
}

/* The connection is the RwDump, which rw_dump_request_out frees itself. */
static void rw_free_conn(void *env, NetConn c) {
    (void)env;
    (void)c;
}

/* Wait for the request before replying with a dummy response. */
static void rw_read_request(void *env) {
    RwDump *d = (RwDump *)env;
    Alloc *h = heap_allocator();
    BufioReader *br = bufio_new_reader(h, io_pipe_reader_as_io_reader(d->pr));
    if (br != NULL) {
        Error err;
        HttpRequest *req = http_read_request(h, br, &err);
        if (req != NULL) {
            if (d->on_read_headers.f != NULL)
                d->on_read_headers.f(d->on_read_headers.env);
            /* Ensure all the body is read; otherwise we'll get a partial
             * dump. */
            (void)io_copy(h, io_discard, io_read_closer_as_io_reader(req->body), &err);
            (void)req->body.vt->closer.close(req->body.data);
            http_request_free(req);
        }
        bufio_reader_free(br);
    }
    bool v = true;
    SelectCase cases[] = {BURROW_SEND(d->c, &v), BURROW_RECV(d->quit, NULL)};
    if (chan_select(cases, 2) == 1)
        chan_close(d->c);
}

static Str rw_dump_request_out(Alloc *a, HttpRequest *req, Func on_read_headers,
                               Error *err) {
    Alloc *h = heap_allocator();
    RwDump *d = must(mem_alloc(h, sizeof *d, _Alignof(RwDump)));
    memset(d, 0, sizeof *d);
    d->buf = BYTES_BUFFER(h);
    io_pipe(h, &d->pr, &d->pw);
    must(d->pr);
    IoWriter ws[2] = {bytes_buffer_as_io_writer(&d->buf),
                      io_pipe_writer_as_io_writer(d->pw)};
    d->mw = io_multi_writer(h, ws, 2);
    must(d->mw.data);
    d->c = must(chan_make(h, TYPE_BOOL, 0));
    d->quit = must(chan_make(h, TYPE_BOOL, 0));
    strings_reader_reset(&d->res,
                         S("HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n"));
    d->on_read_headers = on_read_headers;

    HttpTransport tr;
    memset(&tr, 0, sizeof tr);
    tr.dial = BURROW_FN(HttpDialFunc, rw_dial, d);
    tr.free_conn = BURROW_FN(HttpFreeConnFunc, rw_free_conn, NULL);

    if (!sync_wait_group_go(&d->wg, BURROW_FN(Func, rw_read_request, d)))
        panic_str(S("out of memory"));
    HttpResponse *res = http_transport_round_trip(&tr, req, err);
    if (BURROW_FAILED(*err))
        chan_close(d->quit);
    http_response_free(res);
    /* Go leaves the goroutines to finish on their own. Here they have to be
     * done before d goes, and closing the pipe lets any still on it go. */
    (void)io_pipe_reader_close(d->pr);
    (void)io_pipe_writer_close(d->pw);
    sync_wait_group_wait(&d->wg);
    http_transport_close_idle_connections(&tr);
    http_transport_free(&tr);

    Slice b = bytes_buffer_bytes(&d->buf);
    Str out = str_clone(a, str_from_bytes((const Byte *)b.p, b.len));
    chan_free(d->quit);
    chan_free(d->c);
    io_multi_writer_free(h, d->mw);
    io_pipe_free(d->pr);
    bytes_buffer_free(&d->buf);
    mem_free(h, d, sizeof *d, _Alignof(RwDump));
    return out;
}

typedef enum { WT_NO_BODY, WT_STRING, WT_PIPE } WtBody;

typedef struct WtCase {
    const char *method;
    int64_t clen; /* ContentLength */
    WtBody body;
    const char *str;
    const char *want[3];
    bool want_no_length; /* noContentLengthOrTransferEncoding */
} WtCase;

static const WtCase wt_cases[] = {
    {"GET", 0, WT_NO_BODY, NULL, {NULL}, true},
    {"GET", 0, WT_STRING, "", {NULL}, true},
    {"GET", -1, WT_STRING, "", {NULL}, true},
    /* A GET with a body, with explicit content length: */
    {"GET", 7, WT_STRING, "foobody", {"Content-Length: 7", "foobody"}, false},
    /* A GET with a body, sniffing the leading "f" from "foobody". */
    {"GET",
     -1,
     WT_STRING,
     "foobody",
     {"Transfer-Encoding: chunked", "\r\n1\r\nf\r\n", "oobody"},
     false},
    /* But a POST request is expected to have a body, so no sniffing
     * happens: */
    {"POST",
     -1,
     WT_STRING,
     "foobody",
     {"Transfer-Encoding: chunked", "foobody"},
     false},
    {"POST", -1, WT_STRING, "", {"Transfer-Encoding: chunked"}, false},
    /* Verify that a blocking Request.Body doesn't block forever. */
    {"GET", -1, WT_PIPE, NULL, {"Transfer-Encoding: chunked"}, false},
};

static void close_pipe_writer(void *env) {
    (void)io_pipe_writer_close((IoPipeWriter *)env);
}

/* Go runs this in a synctest bubble, since it relies on the transport probing
 * the request body within 200ms, and a fake clock keeps that from flaking on
 * slow builders. There is no fake clock here, so the 200ms are real ones. */
static void TestRequestWriteTransport(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    ARENA_BEGIN;
    for (Int i = 0; i < (Int)(sizeof wt_cases / sizeof wt_cases[0]); i++) {
        const WtCase *tt = &wt_cases[i];
        Url u = {0};
        u.scheme = S("http");
        u.host = S("example.com");
        HttpRequest *req = must(mem_alloc(a, sizeof *req, _Alignof(HttpRequest)));
        req->method = cs(tt->method);
        req->url = &u;
        req->header = must(http_header_make(a));
        req->content_length = tt->clen;
        StringsReader sr;
        IoNopCloser nc;
        IoPipeReader *pr = NULL;
        IoPipeWriter *pw = NULL;
        Func after_req_read = {0};
        if (tt->body == WT_STRING) {
            strings_reader_reset(&sr, cs(tt->str));
            nc = io_nop_closer(strings_reader_as_io_reader(&sr));
            req->body = io_nop_closer_as_io_read_closer(&nc);
        } else if (tt->body == WT_PIPE) {
            io_pipe(heap_allocator(), &pr, &pw);
            must(pr);
            after_req_read = BURROW_FN(Func, close_pipe_writer, pw);
            nc = io_nop_closer(io_pipe_reader_as_io_reader(pr));
            req->body = io_nop_closer_as_io_read_closer(&nc);
        }
        Error err;
        Str got = rw_dump_request_out(a, req, after_req_read, &err);
        arena_free(&req->arena);
        if (pr != NULL)
            io_pipe_free(pr);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test[%d]: %v", i, err);
            continue;
        }
        if (tt->want_no_length) {
            if (contains(got, "Content-Length: "))
                testing_t_errorf_v(
                    t, "test[%d]: unexpected Content-Length in request: %s", i, got);
            if (contains(got, "Transfer-Encoding: "))
                testing_t_errorf_v(
                    t, "test[%d]: unexpected Transfer-Encoding in request: %s", i, got);
        }
        for (int j = 0; j < 3 && tt->want[j] != NULL; j++)
            if (!contains(got, tt->want[j]))
                testing_t_errorf_v(t, "test[%d]: expected substring %q in request: %s",
                                   i, cs(tt->want[j]), got);
    }
    ARENA_END;
}

#define TESTS(X)                                                                       \
    X(TestRequestWrite)                                                                \
    X(TestRequestWriteClosesBody)                                                      \
    X(TestRequestWriteError)                                                           \
    X(TestRequestWriteProbe)                                                           \
    X(TestNewRequestContentLength)                                                     \
    X(TestRequestWriteTransport)

TESTING_MAIN(TESTS)
