/* Derived from Go's src/net/http/readrequest_test.go, the tests in
 * request_test.go and transfer_test.go of reading a request and its body, and
 * the rest of request_test.go that needs no server.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http_internal.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/declare.h"
#include "burrow/encoding/base64.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/strings.h"
#include "burrow/time.h"

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

static Slice bytes_of(Str s) {
    return slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE);
}

/* The header as Go's %v prints it, keys sorted, and "nil" for a nil one, which
 * reflect.DeepEqual tells from an empty one. */
static Str header_v(Alloc *a, HttpHeader h) {
    if (h == NULL)
        return S("nil");
    Int n = map_len(h);
    Str *keys =
        (Str *)mem_alloc(a, sizeof(Str) * (size_t)(n > 0 ? n : 1), _Alignof(Str));
    if (keys == NULL)
        return (Str){0};
    Int k = 0;
    const void *key;
    void *val;
    for (MapIter it = map_iter(h); map_next(&it, &key, &val);)
        keys[k++] = *(const Str *)key;
    for (Int i = 1; i < k; i++)
        for (Int j = i; j > 0 && str_cmp(keys[j - 1], keys[j]) > 0; j--) {
            Str x = keys[j];
            keys[j] = keys[j - 1];
            keys[j - 1] = x;
        }
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_string(&b, S("map["), NULL);
    for (Int i = 0; i < k; i++) {
        if (i > 0)
            strings_builder_write_byte(&b, ' ');
        strings_builder_write_string(&b, keys[i], NULL);
        strings_builder_write_string(&b, S(":["), NULL);
        const Slice *vs = (const Slice *)map_get(h, &keys[i]);
        for (Int j = 0; j < vs->len; j++) {
            if (j > 0)
                strings_builder_write_byte(&b, ' ');
            strings_builder_write_string(&b, ((const Str *)vs->p)[j], NULL);
        }
        strings_builder_write_byte(&b, ']');
    }
    strings_builder_write_byte(&b, ']');
    return strings_builder_string(&b);
}

static BufioReader *reader_of(Alloc *a, StringsReader *sr, Str s) {
    strings_reader_reset(sr, s);
    return bufio_new_reader(a, strings_reader_as_io_reader(sr));
}

/* ---------------------------------------------------------- TestReadRequest */

typedef struct ReqTest {
    const char *raw;
    const char *method; /* NULL when Go's Req is nil */
    const char *url_scheme;
    const char *url_host;
    const char *url_path;
    const char *proto;
    const char *header;
    const char *host;
    const char *request_uri;
    const char *body;
    const char *trailer; /* NULL for noTrailer */
    const char *error;
    int64_t content_length;
    Int proto_major;
    Int proto_minor;
    bool close;
    bool chunked; /* TransferEncoding: []string{"chunked"} */
} ReqTest;

static const ReqTest req_tests[] = {
    /* Baseline test; All Request fields included for template use */
    {
        .raw = "GET http://www.techcrunch.com/ HTTP/1.1\r\n"
               "Host: www.techcrunch.com\r\n"
               "User-Agent: Fake\r\n"
               "Accept: "
               "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8\r\n"
               "Accept-Language: en-us,en;q=0.5\r\n"
               "Accept-Encoding: gzip,deflate\r\n"
               "Accept-Charset: ISO-8859-1,utf-8;q=0.7,*;q=0.7\r\n"
               "Keep-Alive: 300\r\n"
               "Content-Length: 7\r\n"
               "Proxy-Connection: keep-alive\r\n\r\n"
               "abcdef\n???",
        .method = "GET",
        .url_scheme = "http",
        .url_host = "www.techcrunch.com",
        .url_path = "/",
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .header = "map[Accept:[text/html,application/xhtml+xml,application/xml;q=0.9,*/"
                  "*;q=0.8] Accept-Charset:[ISO-8859-1,utf-8;q=0.7,*;q=0.7] "
                  "Accept-Encoding:[gzip,deflate] Accept-Language:[en-us,en;q=0.5] "
                  "Content-Length:[7] Keep-Alive:[300] Proxy-Connection:[keep-alive] "
                  "User-Agent:[Fake]]",
        .close = false,
        .content_length = 7,
        .host = "www.techcrunch.com",
        .request_uri = "http://www.techcrunch.com/",
        .body = "abcdef\n",
    },

    /* GET request with no body (the normal case) */
    {
        .raw = "GET / HTTP/1.1\r\n"
               "Host: foo.com\r\n\r\n",
        .method = "GET",
        .url_path = "/",
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .header = "map[]",
        .close = false,
        .content_length = 0,
        .host = "foo.com",
        .request_uri = "/",
        .body = "",
    },

    /* Tests that we don't parse a path that looks like a scheme-relative URI
     * as a scheme-relative URI. */
    {
        .raw = "GET //user@host/is/actually/a/path/ HTTP/1.1\r\n"
               "Host: test\r\n\r\n",
        .method = "GET",
        .url_path = "//user@host/is/actually/a/path/",
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .header = "map[]",
        .close = false,
        .content_length = 0,
        .host = "test",
        .request_uri = "//user@host/is/actually/a/path/",
        .body = "",
    },

    /* Tests a bogus absolute-path on the Request-Line (RFC 7230 section
     * 5.3.1) */
    {
        .raw = "GET ../../../../etc/passwd HTTP/1.1\r\n"
               "Host: test\r\n\r\n",
        .body = "",
        .error = "parse \"../../../../etc/passwd\": invalid URI for request",
    },

    /* Tests missing URL: */
    {
        .raw = "GET  HTTP/1.1\r\n"
               "Host: test\r\n\r\n",
        .body = "",
        .error = "parse \"\": empty url",
    },

    /* Tests chunked body with trailer: */
    {
        .raw = "POST / HTTP/1.1\r\n"
               "Host: foo.com\r\n"
               "Transfer-Encoding: chunked\r\n\r\n"
               "3\r\nfoo\r\n"
               "3\r\nbar\r\n"
               "0\r\n"
               "Trailer-Key: Trailer-Value\r\n"
               "\r\n",
        .method = "POST",
        .url_path = "/",
        .chunked = true,
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .header = "map[]",
        .content_length = -1,
        .host = "foo.com",
        .request_uri = "/",
        .body = "foobar",
        .trailer = "map[Trailer-Key:[Trailer-Value]]",
    },

    /* Tests chunked body and a bogus Content-Length which should be
     * deleted. */
    {
        .raw = "POST / HTTP/1.1\r\n"
               "Host: foo.com\r\n"
               "Transfer-Encoding: chunked\r\n"
               "Content-Length: 9999\r\n\r\n" /* to be removed. */
               "3\r\nfoo\r\n"
               "3\r\nbar\r\n"
               "0\r\n"
               "\r\n",
        .method = "POST",
        .url_path = "/",
        .chunked = true,
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .header = "map[]",
        .content_length = -1,
        .host = "foo.com",
        .request_uri = "/",
        .body = "foobar",
    },

    /* Tests chunked body and an invalid Content-Length. */
    {
        .raw = "POST / HTTP/1.1\r\n"
               "Host: foo.com\r\n"
               "Transfer-Encoding: chunked\r\n"
               "Content-Length: notdigits\r\n\r\n" /* raise an error */
               "3\r\nfoo\r\n"
               "3\r\nbar\r\n"
               "0\r\n"
               "\r\n",
        .body = "",
        .error = "bad Content-Length \"notdigits\"",
    },

    /* CONNECT request with domain name: */
    {
        .raw = "CONNECT www.google.com:443 HTTP/1.1\r\n\r\n",
        .method = "CONNECT",
        .url_host = "www.google.com:443",
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .header = "map[]",
        .close = false,
        .content_length = 0,
        .host = "www.google.com:443",
        .request_uri = "www.google.com:443",
        .body = "",
    },

    /* CONNECT request with IP address: */
    {
        .raw = "CONNECT 127.0.0.1:6060 HTTP/1.1\r\n\r\n",
        .method = "CONNECT",
        .url_host = "127.0.0.1:6060",
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .header = "map[]",
        .close = false,
        .content_length = 0,
        .host = "127.0.0.1:6060",
        .request_uri = "127.0.0.1:6060",
        .body = "",
    },

    /* CONNECT request for RPC: */
    {
        .raw = "CONNECT /_goRPC_ HTTP/1.1\r\n\r\n",
        .method = "CONNECT",
        .url_path = "/_goRPC_",
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .header = "map[]",
        .close = false,
        .content_length = 0,
        .host = "",
        .request_uri = "/_goRPC_",
        .body = "",
    },

    /* SSDP Notify request. golang.org/issue/3692 */
    {
        .raw = "NOTIFY * HTTP/1.1\r\nServer: foo\r\n\r\n",
        .method = "NOTIFY",
        .url_path = "*",
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .header = "map[Server:[foo]]",
        .close = false,
        .content_length = 0,
        .request_uri = "*",
        .body = "",
    },

    /* OPTIONS request. Similar to golang.org/issue/3692 */
    {
        .raw = "OPTIONS * HTTP/1.1\r\nServer: foo\r\n\r\n",
        .method = "OPTIONS",
        .url_path = "*",
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .header = "map[Server:[foo]]",
        .close = false,
        .content_length = 0,
        .request_uri = "*",
        .body = "",
    },

    /* Connection: close. golang.org/issue/8261 */
    {
        .raw = "GET / HTTP/1.1\r\nHost: issue8261.com\r\nConnection: close\r\n\r\n",
        .method = "GET",
        .url_path = "/",
        /* This wasn't removed from Go 1.0 to Go 1.3, so locking it in that we
         * keep this: */
        .header = "map[Connection:[close]]",
        .host = "issue8261.com",
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .close = true,
        .request_uri = "/",
        .body = "",
    },

    /* HEAD with Content-Length 0. Make sure this is permitted, since I think
     * we used to send it. */
    {
        .raw = "HEAD / HTTP/1.1\r\nHost: issue8261.com\r\nConnection: "
               "close\r\nContent-Length: 0\r\n\r\n",
        .method = "HEAD",
        .url_path = "/",
        .header = "map[Connection:[close] Content-Length:[0]]",
        .host = "issue8261.com",
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .close = true,
        .request_uri = "/",
        .body = "",
    },

    /* http2 client preface: */
    {
        .raw = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n",
        .method = "PRI",
        .url_path = "*",
        .header = "map[]",
        .proto = "HTTP/2.0",
        .proto_major = 2,
        .proto_minor = 0,
        .request_uri = "*",
        .content_length = -1,
        .close = true,
        .body = "",
    },
};

static void check_str_field(TestingT *t, Int i, const char *name, Str got,
                            const char *want) {
    if (!str_eq(got, cs(want)))
        testing_t_errorf_v(t, "Test %d: %s = %q want %q", i, cs(name), got, cs(want));
}

static void check_url(TestingT *t, Int i, const Url *u, const ReqTest *tt) {
    if (u == NULL) {
        testing_t_errorf_v(t, "Test %d: URL = nil", i);
        return;
    }
    check_str_field(t, i, "URL.Scheme", u->scheme, tt->url_scheme);
    check_str_field(t, i, "URL.Host", u->host, tt->url_host);
    check_str_field(t, i, "URL.Path", u->path, tt->url_path);
    if (u->user != NULL || u->opaque.len > 0 || u->raw_query.len > 0 ||
        u->fragment.len > 0 || u->raw_path.len > 0 || u->force_query || u->omit_host)
        testing_t_errorf_v(t, "Test %d: URL has more than Scheme, Host and Path", i);
}

static void TestReadRequest(TestingT *t) {
    for (Int i = 0; i < (Int)(sizeof req_tests / sizeof req_tests[0]); i++) {
        const ReqTest *tt = &req_tests[i];
        ARENA_BEGIN;
        StringsReader sr;
        BufioReader *br = reader_of(a, &sr, cs(tt->raw));
        Error err;
        HttpRequest *req = http_read_request(a, br, &err);
        if (BURROW_FAILED(err)) {
            if (!str_eq(error_text(err), cs(tt->error)))
                testing_t_errorf_v(t, "#%d: error %q, want error %q", i,
                                   error_text(err), cs(tt->error));
            bufio_reader_free(br);
            ARENA_END;
            continue;
        }
        if (tt->method == NULL) {
            testing_t_errorf_v(t, "#%d: no error, want error %q", i, cs(tt->error));
            http_request_free(req);
            bufio_reader_free(br);
            ARENA_END;
            continue;
        }
        check_str_field(t, i, "Method", req->method, tt->method);
        check_url(t, i, req->url, tt);
        check_str_field(t, i, "Proto", req->proto, tt->proto);
        if (req->proto_major != tt->proto_major || req->proto_minor != tt->proto_minor)
            testing_t_errorf_v(
                t, "Test %d: ProtoMajor, ProtoMinor = %d, %d want %d, %d", i,
                req->proto_major, req->proto_minor, tt->proto_major, tt->proto_minor);
        check_str_field(t, i, "Header", header_v(a, req->header), tt->header);
        if (req->content_length != tt->content_length)
            testing_t_errorf_v(t, "Test %d: ContentLength = %d want %d", i,
                               req->content_length, tt->content_length);
        bool chunked = req->transfer_encoding.len == 1 &&
                       str_eq(((const Str *)req->transfer_encoding.p)[0], S("chunked"));
        if (chunked != tt->chunked || (!chunked && req->transfer_encoding.len != 0))
            testing_t_errorf_v(t, "Test %d: TransferEncoding = %q", i,
                               req->transfer_encoding);
        if (req->close != tt->close)
            testing_t_errorf_v(t, "Test %d: Close = %t want %t", i, req->close,
                               tt->close);
        check_str_field(t, i, "Host", req->host, tt->host);
        check_str_field(t, i, "RequestURI", req->request_uri, tt->request_uri);
        check_str_field(t, i, "RemoteAddr", req->remote_addr, "");
        check_str_field(t, i, "Pattern", req->pattern, "");

        Slice body = io_read_all(a, io_read_closer_as_io_reader(req->body), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "Test %d: copying body: %v", i, err);
        (void)req->body.vt->closer.close(req->body.data);
        if (!str_eq(str_from_bytes(body.p, body.len), cs(tt->body)))
            testing_t_errorf_v(t, "Test %d: Body = %q want %q", i,
                               str_from_bytes(body.p, body.len), cs(tt->body));

        Str want_trailer = tt->trailer != NULL ? cs(tt->trailer) : S("nil");
        if (!str_eq(header_v(a, req->trailer), want_trailer))
            testing_t_errorf_v(t, "Test %d: Trailers differ.\n got: %v\nwant: %v", i,
                               header_v(a, req->trailer), want_trailer);
        http_request_free(req);
        bufio_reader_free(br);
        ARENA_END;
    }
}

/* reqBytes treats req as a request (with \n delimiters) and returns it with
 * \r\n delimiters, ending in \r\n\r\n */
static Str req_bytes(Alloc *a, const char *req) {
    Str s = strings_replace_all(a, strings_trim_space(cs(req)), S("\n"), S("\r\n"));
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_string(&b, s, NULL);
    strings_builder_write_string(&b, S("\r\n\r\n"), NULL);
    return strings_builder_string(&b);
}

static void TestReadRequest_Bad(TestingT *t) {
    static const struct {
        const char *name;
        const char *req;
    } tests[] = {
        {"bad_connect_host", "CONNECT "
                             "[]%20%48%54%54%50%2f%31%2e%31%0a%4d%79%48%65%61%64%65%72%"
                             "3a%20%31%32%33%0a%0a "
                             "HTTP/1.0"},
        {"smuggle_two_contentlen", "POST / HTTP/1.1\n"
                                   "Content-Length: 3\n"
                                   "Content-Length: 4\n"
                                   "\n"
                                   "abc"},
        {"smuggle_two_content_len_head", "HEAD / HTTP/1.1\n"
                                         "Host: foo\n"
                                         "Content-Length: 4\n"
                                         "Content-Length: 5\n"
                                         "\n"
                                         "1234"},

        /* golang.org/issue/22464 */
        {"leading_space_in_header", "GET / HTTP/1.1\n"
                                    " Host: foo"},
        {"leading_tab_in_header", "GET / HTTP/1.1\n"
                                  "\t"
                                  "Host: foo"},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        ARENA_BEGIN;
        StringsReader sr;
        BufioReader *br = reader_of(a, &sr, req_bytes(a, tests[i].req));
        Error err;
        HttpRequest *got = http_read_request(a, br, &err);
        if (BURROW_OK(err)) {
            Slice all = io_read_all(a, io_read_closer_as_io_reader(got->body), &err);
            testing_t_errorf_v(t, "%s: got unexpected request\n  Body = %q, %v",
                               cs(tests[i].name), str_from_bytes(all.p, all.len), err);
            http_request_free(got);
        }
        bufio_reader_free(br);
        ARENA_END;
    }
}

/* ---------------------------------------------------- request_test.go's */

static void TestReadRequestErrors(TestingT *t) {
    static const struct {
        const char *in;
        const char *err;
        const char *header; /* NULL for nil */
    } tests[] = {
        {"GET / HTTP/1.1\r\nheader:foo\r\n\r\n", "", "map[Header:[foo]]"},
        {"GET / HTTP/1.1\r\nheader:foo\r\n", "unexpected EOF", NULL},
        {"", "EOF", NULL},
        {"HEAD / HTTP/1.1\r\n\r\n", "", "map[]"},

        /* Multiple Content-Length values should either be deduplicated if same
         * or reject otherwise. See Issue 16490. */
        {"POST / HTTP/1.1\r\nContent-Length: 10\r\nContent-Length: 0\r\n\r\nGopher "
         "hey\r\n",
         "cannot contain multiple Content-Length headers", NULL},
        {"POST / HTTP/1.1\r\nContent-Length: 10\r\nContent-Length: 6\r\n\r\nGopher\r\n",
         "cannot contain multiple Content-Length headers", NULL},
        {"PUT / HTTP/1.1\r\nContent-Length: 6 \r\nContent-Length: "
         "6\r\nContent-Length:6\r\n\r\nGopher\r\n",
         "", "map[Content-Length:[6]]"},
        {"PUT / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 6 \r\n\r\n",
         "cannot contain multiple Content-Length headers", NULL},
        {"POST / HTTP/1.1\r\nContent-Length:\r\nContent-Length: 3\r\n\r\n",
         "cannot contain multiple Content-Length headers", NULL},
        {"HEAD / HTTP/1.1\r\nContent-Length:0\r\nContent-Length: 0\r\n\r\n", "",
         "map[Content-Length:[0]]"},
        {"HEAD / HTTP/1.1\r\nHost: foo\r\nHost: bar\r\n\r\n\r\n\r\n",
         "too many Host headers", NULL},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        ARENA_BEGIN;
        StringsReader sr;
        BufioReader *br = reader_of(a, &sr, cs(tests[i].in));
        Error err;
        HttpRequest *req = http_read_request(a, br, &err);
        if (BURROW_OK(err)) {
            if (tests[i].err[0] != '\0')
                testing_t_errorf_v(t, "#%d: got nil err; want %q", i, cs(tests[i].err));
            Str want = tests[i].header != NULL ? cs(tests[i].header) : S("nil");
            if (!str_eq(header_v(a, req->header), want))
                testing_t_errorf_v(t, "#%d: gotHeader: %v wantHeader: %v", i,
                                   header_v(a, req->header), want);
            http_request_free(req);
        } else if (tests[i].err[0] == '\0' ||
                   !strings_contains(error_text(err), cs(tests[i].err))) {
            testing_t_errorf_v(t, "%d: got error = %v; want %v", i, err,
                               cs(tests[i].err));
        }
        bufio_reader_free(br);
        ARENA_END;
    }
}

static void TestParseHTTPVersion(TestingT *t) {
    static const struct {
        const char *vers;
        Int major, minor;
        bool ok;
    } tests[] = {
        {"HTTP/0.0", 0, 0, true},
        {"HTTP/0.9", 0, 9, true},
        {"HTTP/1.0", 1, 0, true},
        {"HTTP/1.1", 1, 1, true},

        {"HTTP", 0, 0, false},
        {"HTTP/one.one", 0, 0, false},
        {"HTTP/1.1/", 0, 0, false},
        {"HTTP/-1,0", 0, 0, false},
        {"HTTP/0,-1", 0, 0, false},
        {"HTTP/", 0, 0, false},
        {"HTTP/1,1", 0, 0, false},
        {"HTTP/+1.1", 0, 0, false},
        {"HTTP/1.+1", 0, 0, false},
        {"HTTP/0000000001.1", 0, 0, false},
        {"HTTP/1.0000000001", 0, 0, false},
        {"HTTP/3.14", 0, 0, false},
        {"HTTP/12.3", 0, 0, false},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        Int major = -1;
        Int minor = -1;
        bool ok = http_parse_http_version(cs(tests[i].vers), &major, &minor);
        if (ok != tests[i].ok || major != tests[i].major || minor != tests[i].minor)
            testing_t_errorf_v(
                t, "failed to parse %q, expected: {%d %d %t}, got {%d %d %t}",
                cs(tests[i].vers), tests[i].major, tests[i].minor, tests[i].ok, major,
                minor, ok);
    }
}

static void TestGetBasicAuth(TestingT *t) {
    static const struct {
        const char *username, *password;
        bool ok;
    } tests[] = {
        {"Aladdin", "open sesame", true},
        {"Aladdin", "open:sesame", true},
        {"", "", true},
    };
    ARENA_BEGIN;
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        HttpRequest r = {0};
        r.header = http_header_make(a);
        http_request_set_basic_auth(&r, a, cs(tests[i].username),
                                    cs(tests[i].password));
        Str username;
        Str password;
        bool ok = http_request_basic_auth(&r, a, &username, &password);
        if (ok != tests[i].ok || !str_eq(username, cs(tests[i].username)) ||
            !str_eq(password, cs(tests[i].password)))
            testing_t_errorf_v(t, "BasicAuth() = {%q %q %t}, want {%q %q %t}", username,
                               password, ok, cs(tests[i].username),
                               cs(tests[i].password), tests[i].ok);
    }
    /* Unauthenticated request. */
    HttpRequest r = {0};
    r.header = http_header_make(a);
    Str username;
    Str password;
    bool ok = http_request_basic_auth(&r, a, &username, &password);
    if (ok)
        testing_t_errorf_v(
            t, "expected false from BasicAuth when the request is unauthenticated");
    if (username.len != 0 || password.len != 0)
        testing_t_errorf_v(t,
                           "expected credentials: {\"\" \"\"} when the request is "
                           "unauthenticated, got {%q %q}",
                           username, password);
    ARENA_END;
}

static Str basic_join(Alloc *a, const char *prefix, const char *plain) {
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_string(&b, cs(prefix), NULL);
    strings_builder_write_string(
        &b,
        base64_encoding_encode_to_string(base64_std_encoding, a, bytes_of(cs(plain))),
        NULL);
    return strings_builder_string(&b);
}

static void TestParseBasicAuth(TestingT *t) {
    ARENA_BEGIN;
    const struct {
        Str header;
        const char *username, *password;
        bool ok;
    } tests[] = {
        {basic_join(a, "Basic ", "Aladdin:open sesame"), "Aladdin", "open sesame",
         true},

        /* Case doesn't matter: */
        {basic_join(a, "BASIC ", "Aladdin:open sesame"), "Aladdin", "open sesame",
         true},
        {basic_join(a, "basic ", "Aladdin:open sesame"), "Aladdin", "open sesame",
         true},

        {basic_join(a, "Basic ", "Aladdin:open:sesame"), "Aladdin", "open:sesame",
         true},
        {basic_join(a, "Basic ", ":"), "", "", true},
        {basic_join(a, "Basic", "Aladdin:open sesame"), "", "", false},
        {basic_join(a, "", "Aladdin:open sesame"), "", "", false},
        {S("Basic "), "", "", false},
        {S("Basic Aladdin:open sesame"), "", "", false},
        {S("Digest username=\"Aladdin\""), "", "", false},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        HttpRequest r = {0};
        r.header = http_header_make(a);
        http_header_set(r.header, S("Authorization"), tests[i].header);
        Str username;
        Str password;
        bool ok = http_request_basic_auth(&r, a, &username, &password);
        if (ok != tests[i].ok || !str_eq(username, cs(tests[i].username)) ||
            !str_eq(password, cs(tests[i].password)))
            testing_t_errorf_v(t, "BasicAuth() = {%q %q %t}, want {%q %q %t}", username,
                               password, ok, cs(tests[i].username),
                               cs(tests[i].password), tests[i].ok);
    }
    ARENA_END;
}

static void TestSetBasicAuth(TestingT *t) {
    ARENA_BEGIN;
    HttpRequest r = {0};
    r.header = http_header_make(a);
    http_request_set_basic_auth(&r, a, S("Aladdin"), S("open sesame"));
    Str g = http_header_get(r.header, S("Authorization"));
    Str e = S("Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
    if (!str_eq(g, e))
        testing_t_errorf_v(t, "got header %q, want %q", g, e);
    ARENA_END;
}

/* ---------------------------------------------------- transfer_test.go's */

static void TestBodyReadBadTrailer(TestingT *t) {
    ARENA_BEGIN;
    StringsReader src;
    strings_reader_reset(&src, S("foobar"));
    StringsReader empty;
    BufioReader *r = reader_of(a, &empty, S(""));
    HttpHeader trailer = NULL; /* force reading the trailer */
    IoReadCloser b =
        burrow__http_new_body(a, strings_reader_as_io_reader(&src), &trailer, r);
    if (b.vt == NULL)
        testing_t_fatalf_v(t, "out of memory");
    IoReader rd = io_read_closer_as_io_reader(b);
    Byte buf[7];
    Error err;
    Int n = rd.vt->read(rd.data, slice_from(buf, 3, 7, TYPE_BYTE), &err);
    Str got = str_from_bytes(buf, n);
    if (!str_eq(got, S("foo")) || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "first Read = %d (%q), %v; want 3 (\"foo\")", n, got,
                           err);

    n = rd.vt->read(rd.data, slice_from(buf, 7, 7, TYPE_BYTE), &err);
    got = str_from_bytes(buf, n);
    if (!str_eq(got, S("bar")) || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "second Read = %d (%q), %v; want 3 (\"bar\")", n, got,
                           err);

    n = rd.vt->read(rd.data, slice_from(buf, 7, 7, TYPE_BYTE), &err);
    got = str_from_bytes(buf, n);
    if (BURROW_OK(err))
        testing_t_errorf_v(
            t, "final Read was successful (%q), expected error from trailer read", got);
    burrow__http_body_free(b.data);
    bufio_reader_free(r);
    ARENA_END;
}

static void TestParseTransferEncoding(TestingT *t) {
    static const struct {
        const char *values[3];
        const char *want_err; /* NULL for nil */
    } tests[] = {
        {{"fugazi"}, "unsupported transfer encoding: \"fugazi\""},
        {{"chunked, chunked", "identity", "chunked"},
         "too many transfer encodings: [\"chunked, chunked\" \"identity\" "
         "\"chunked\"]"},
        {{""}, "unsupported transfer encoding: \"\""},
        {{"chunked, identity"}, "unsupported transfer encoding: \"chunked, identity\""},
        {{"chunked", "identity"},
         "too many transfer encodings: [\"chunked\" \"identity\"]"},
        {{"\x0b"
          "chunked"},
         "unsupported transfer encoding: \"\\vchunked\""},
        {{"chunked"}, NULL},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        ARENA_BEGIN;
        HttpHeader h = http_header_make(a);
        for (Int j = 0; j < 3 && tests[i].values[j] != NULL; j++)
            http_header_add(h, S("Transfer-Encoding"), cs(tests[i].values[j]));
        bool chunked;
        Error got = burrow__http_parse_transfer_encoding(h, 1, 1, &chunked);
        bool same = tests[i].want_err == NULL
                        ? BURROW_OK(got)
                        : burrow__http_is_unsupported_te_error(got) &&
                              str_eq(error_text(got), cs(tests[i].want_err));
        if (!same)
            testing_t_errorf_v(t, "%d.\ngot error:\n%v\nwant error:\n%v\n\n", i, got,
                               tests[i].want_err != NULL ? cs(tests[i].want_err)
                                                         : S("<nil>"));
        ARENA_END;
    }
}

/* issue 39017 - disallow Content-Length values such as "+3" */
static void TestParseContentLength(TestingT *t) {
    static const struct {
        const char *cl;
        const char *want_err; /* NULL for nil */
    } tests[] = {
        {"", "invalid empty Content-Length \"\""},
        {"3", NULL},
        {"+3", "bad Content-Length \"+3\""},
        {"-3", "bad Content-Length \"-3\""},
        /* max int64, for safe conversion before returning */
        {"9223372036854775807", NULL},
        {"9223372036854775808", "bad Content-Length \"9223372036854775808\""},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        Str cl = cs(tests[i].cl);
        Error got;
        (void)burrow__http_parse_content_length(slice_from(&cl, 1, 1, TYPE_STRING),
                                                &got);
        bool same =
            tests[i].want_err == NULL
                ? BURROW_OK(got)
                : BURROW_FAILED(got) && str_eq(error_text(got), cs(tests[i].want_err));
        if (!same)
            testing_t_errorf_v(t, "%q:\n\tgot=%v\n\twant=%v", cl, got,
                               tests[i].want_err != NULL ? cs(tests[i].want_err)
                                                         : S("<nil>"));
    }
}

/* ------------------------------------------ the rest of request_test.go's */

/* Whether two errors are the same error, which is Go's == on them. */
static bool same_err(Error x, Error y) {
    return x.vt == y.vt && x.data == y.data;
}

static const struct {
    const char *in;
    const char *out;
} new_request_host_tests[] = {
    {"http://www.example.com/", "www.example.com"},
    {"http://www.example.com:8080/", "www.example.com:8080"},

    {"http://192.168.0.1/", "192.168.0.1"},
    {"http://192.168.0.1:8080/", "192.168.0.1:8080"},
    {"http://192.168.0.1:/", "192.168.0.1"},

    {"http://[fe80::1]/", "[fe80::1]"},
    {"http://[fe80::1]:8080/", "[fe80::1]:8080"},
    {"http://[fe80::1%25en0]/", "[fe80::1%en0]"},
    {"http://[fe80::1%25en0]:8080/", "[fe80::1%en0]:8080"},
    {"http://[fe80::1%25en0]:/", "[fe80::1%en0]"},
};

static void TestNewRequestHost(TestingT *t) {
    ARENA_BEGIN;
    Int n = (Int)(sizeof new_request_host_tests / sizeof new_request_host_tests[0]);
    for (Int i = 0; i < n; i++) {
        Error err;
        HttpRequest *req = http_new_request(
            a, S("GET"), cs(new_request_host_tests[i].in), (IoReader){0}, &err);
        if (req == NULL) {
            testing_t_errorf_v(t, "#%v: %v", i, err);
            continue;
        }
        Str want = cs(new_request_host_tests[i].out);
        if (!str_eq(req->host, want))
            testing_t_errorf_v(t, "got %q; want %q", req->host, want);
        http_request_free(req);
    }
    ARENA_END;
}

static void TestRequestInvalidMethod(TestingT *t) {
    ARENA_BEGIN;
    Error err;
    HttpRequest *req =
        http_new_request(a, S("bad method"), S("http://foo.com/"), (IoReader){0}, &err);
    if (req != NULL) {
        testing_t_errorf_v(t, "expected error from NewRequest with invalid method");
        http_request_free(req);
    }
    req = http_new_request(a, S("GET"), S("http://foo.example/"), (IoReader){0}, &err);
    if (req == NULL) {
        testing_t_fatalf_v(t, "%v", err);
        ARENA_END;
        return;
    }
    req->method = S("bad method");
    HttpResponse *res = http_client_do(http_default_client, req, &err);
    if (res != NULL)
        http_response_free(res);
    if (BURROW_OK(err) || !strings_contains(error_text(err), S("invalid method")))
        testing_t_errorf_v(t, "Transport error = %v; want invalid method", err);
    http_request_free(req);

    req = http_new_request(a, S(""), S("http://foo.com/"), (IoReader){0}, &err);
    if (req == NULL) {
        testing_t_errorf_v(t, "NewRequest(empty method) = %v; want nil", err);
    } else {
        if (!str_eq(req->method, S("GET")))
            testing_t_errorf_v(t, "NewRequest(empty method) has method %q; want GET",
                               req->method);
        http_request_free(req);
    }
    ARENA_END;
}

/* logWrites, a writer with a WriteByte, so that a request is written to it
 * with no BufioWriter in front, which keeps what each Write was given. */
typedef struct LogWrites {
    TestingT *t;
    Alloc *a;
    Str got[16];
    Int n;
} LogWrites;

static Error log_writes_write_byte(LogWrites *l, Byte c) {
    (void)c;
    testing_t_fatalf_v(l->t, "unexpected WriteByte call");
    return BURROW_NO_ERROR;
}

static Int log_writes_write(void *self, Slice p, Error *err) {
    LogWrites *l = (LogWrites *)self;
    if (l->n < (Int)(sizeof l->got / sizeof l->got[0]))
        l->got[l->n] = str_clone(l->a, str_from_bytes((const Byte *)p.p, p.len));
    l->n++;
    *err = BURROW_NO_ERROR;
    return p.len;
}

#define LOG_WRITES_SIG_WRITE_BYTE(IN, OUT) IN(0, Byte) OUT(Error)
#define LOG_WRITES_METHODS(M, T)                                                       \
    M(T, WriteByte, log_writes_write_byte, LOG_WRITES_SIG_WRITE_BYTE)
BURROW_METHODS_DEFINE(LogWrites, LOG_WRITES_METHODS);

static const Type log_writes_type = {
    {(const Byte *)"logWrites", 9},
    {(const Byte *)"http_test", 9},
    KIND_STRUCT,
    (uint32_t)sizeof(LogWrites),
    (uint16_t)_Alignof(LogWrites),
    0,
    (uint16_t)(sizeof burrow__methods_LogWrites / sizeof burrow__methods_LogWrites[0]),
    NULL,
    burrow__methods_LogWrites,
    NULL,
    NULL,
    0,
    0x6c6f6777U,
    NULL,
};

static const IoWriterVT log_writes_vt = {&log_writes_type, log_writes_write};

/* The writes as Go's %q prints a []string. */
static Str quoted_list(Alloc *a, const Str *v, Int n) {
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_byte(&b, '[');
    for (Int i = 0; i < n; i++) {
        if (i > 0)
            strings_builder_write_byte(&b, ' ');
        strings_builder_write_string(&b, fmt_sprintf_v(a, "%q", v[i]), NULL);
    }
    strings_builder_write_byte(&b, ']');
    return strings_builder_string(&b);
}

static void check_writes(TestingT *t, Alloc *a, const LogWrites *l,
                         const char *const *want, Int nwant) {
    Str w[8];
    for (Int i = 0; i < nwant; i++)
        w[i] = cs(want[i]);
    bool same = l->n == nwant;
    for (Int i = 0; same && i < nwant; i++)
        same = str_eq(l->got[i], w[i]);
    if (!same) {
        Int n = l->n;
        if (n > (Int)(sizeof l->got / sizeof l->got[0]))
            n = (Int)(sizeof l->got / sizeof l->got[0]);
        testing_t_errorf_v(t, "Writes = %s\n  Want = %s", quoted_list(a, l->got, n),
                           quoted_list(a, w, nwant));
    }
}

static void TestRequestWriteBufferedWriter(TestingT *t) {
    ARENA_BEGIN;
    LogWrites got = {t, a, {{0}}, 0};
    Error err;
    HttpRequest *req =
        http_new_request(a, S("GET"), S("http://foo.com/"), (IoReader){0}, &err);
    if (req == NULL) {
        testing_t_fatalf_v(t, "%v", err);
        ARENA_END;
        return;
    }
    (void)http_request_write(req, (IoWriter){&log_writes_vt, &got});
    static const char *const want[] = {
        "GET / HTTP/1.1\r\n",
        "Host: foo.com\r\n",
        "User-Agent: Go-http-client/1.1\r\n",
        "\r\n",
    };
    check_writes(t, a, &got, want, (Int)(sizeof want / sizeof want[0]));
    http_request_free(req);
    ARENA_END;
}

static void TestRequestBadHostHeader(TestingT *t) {
    ARENA_BEGIN;
    LogWrites got = {t, a, {{0}}, 0};
    Error err;
    HttpRequest *req =
        http_new_request(a, S("GET"), S("http://foo/after"), (IoReader){0}, &err);
    if (req == NULL) {
        testing_t_fatalf_v(t, "%v", err);
        ARENA_END;
        return;
    }
    req->host = S("foo.com\nnewline");
    req->url->host = S("foo.com\nnewline");
    (void)http_request_write(req, (IoWriter){&log_writes_vt, &got});
    static const char *const want[] = {
        "GET /after HTTP/1.1\r\n",
        "Host: \r\n",
        "User-Agent: Go-http-client/1.1\r\n",
        "\r\n",
    };
    check_writes(t, a, &got, want, (Int)(sizeof want / sizeof want[0]));
    http_request_free(req);
    ARENA_END;
}

static void TestRequestBadUserAgent(TestingT *t) {
    ARENA_BEGIN;
    LogWrites got = {t, a, {{0}}, 0};
    Error err;
    HttpRequest *req =
        http_new_request(a, S("GET"), S("http://foo/after"), (IoReader){0}, &err);
    if (req == NULL) {
        testing_t_fatalf_v(t, "%v", err);
        ARENA_END;
        return;
    }
    http_header_set(req->header, S("User-Agent"), S("evil\r\nX-Evil: evil"));
    (void)http_request_write(req, (IoWriter){&log_writes_vt, &got});
    static const char *const want[] = {
        "GET /after HTTP/1.1\r\n",
        "Host: foo\r\n",
        "User-Agent: evil  X-Evil: evil\r\n",
        "\r\n",
    };
    check_writes(t, a, &got, want, (Int)(sizeof want / sizeof want[0]));
    http_request_free(req);
    ARENA_END;
}

static bool str_slices_equal(Slice x, Slice y) {
    if ((x.p == NULL) != (y.p == NULL) || x.len != y.len)
        return false;
    for (Int i = 0; i < x.len; i++)
        if (!str_eq(((const Str *)x.p)[i], ((const Str *)y.p)[i]))
            return false;
    return true;
}

static bool urls_equal(const Url *x, const Url *y) {
    if (x == NULL || y == NULL)
        return x == y;
    if ((x->user == NULL) != (y->user == NULL))
        return false;
    if (x->user != NULL && (!str_eq(x->user->username, y->user->username) ||
                            !str_eq(x->user->password, y->user->password) ||
                            x->user->password_set != y->user->password_set))
        return false;
    return str_eq(x->scheme, y->scheme) && str_eq(x->opaque, y->opaque) &&
           str_eq(x->host, y->host) && str_eq(x->path, y->path) &&
           str_eq(x->fragment, y->fragment) && str_eq(x->raw_query, y->raw_query) &&
           str_eq(x->raw_path, y->raw_path) &&
           str_eq(x->raw_fragment, y->raw_fragment) &&
           x->force_query == y->force_query && x->omit_host == y->omit_host;
}

/* The first field where two requests differ, leaving out the header, as
 * reflect.DeepEqual would find it, and NULL when they don't. Both bodies are
 * http_no_body here, or nil, so their vtables say whether they match. */
static const char *request_diff(const HttpRequest *x, const HttpRequest *y) {
    if (!str_eq(x->method, y->method))
        return "Method";
    if (!urls_equal(x->url, y->url))
        return "URL";
    if (!str_eq(x->proto, y->proto) || x->proto_major != y->proto_major ||
        x->proto_minor != y->proto_minor)
        return "Proto";
    if (x->body.vt != y->body.vt)
        return "Body";
    if (x->content_length != y->content_length)
        return "ContentLength";
    if (!str_slices_equal(x->transfer_encoding, y->transfer_encoding))
        return "TransferEncoding";
    if (x->close != y->close)
        return "Close";
    if (!str_eq(x->host, y->host))
        return "Host";
    if (x->form != NULL || y->form != NULL || x->post_form != NULL ||
        y->post_form != NULL || x->multipart_form != NULL || y->multipart_form != NULL)
        return "Form";
    if ((x->trailer == NULL) != (y->trailer == NULL) ||
        (x->trailer != NULL && (map_len(x->trailer) != 0 || map_len(y->trailer) != 0)))
        return "Trailer";
    if (!str_eq(x->remote_addr, y->remote_addr))
        return "RemoteAddr";
    if (!str_eq(x->request_uri, y->request_uri))
        return "RequestURI";
    if (!str_eq(x->pattern, y->pattern))
        return "Pattern";
    if (x->get_body.f != NULL || y->get_body.f != NULL)
        return "GetBody";
    if (x->ctx.vt != y->ctx.vt || x->ctx.data != y->ctx.data)
        return "ctx";
    if (x->response != NULL || y->response != NULL)
        return "Response";
    if (x->pat != y->pat || x->matches.len != y->matches.len ||
        x->other_values != y->other_values)
        return "pat";
    return NULL;
}

static void TestStarRequest(TestingT *t) {
    ARENA_BEGIN;
    StringsReader sr;
    BufioReader *br = reader_of(a, &sr, S("M-SEARCH * HTTP/1.1\r\n\r\n"));
    Error err;
    HttpRequest *req = http_read_request(a, br, &err);
    if (req == NULL) {
        bufio_reader_free(br);
        ARENA_END;
        return;
    }
    if (req->content_length != 0)
        testing_t_errorf_v(t, "ContentLength = %d; want 0", req->content_length);
    if (req->body.vt == NULL)
        testing_t_errorf_v(t, "Body = nil; want non-nil");

    /* Request.Write has Client semantics for Body/ContentLength, where
     * ContentLength 0 means unknown if Body is non-nil, and thus chunking will
     * happen unless we change semantics and signal that we want to serialize
     * it as exactly zero. The only way to do that for outbound requests is
     * with a nil Body. Go writes a copy of the request with a nil Body, and
     * here the body is put aside while the request is written. */
    IoReadCloser body = req->body;
    req->body = (IoReadCloser){0};
    StringsBuilder out = STRINGS_BUILDER(a);
    err = http_request_write(req, strings_builder_as_io_writer(&out));
    req->body = body;
    Str wrote = strings_builder_string(&out);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "%v", err);
    } else {
        if (strings_contains(wrote, S("chunked")))
            testing_t_errorf_v(t, "wrote chunked request; want no body");
        StringsReader sr2;
        BufioReader *br2 = reader_of(a, &sr2, wrote);
        HttpRequest *back = http_read_request(a, br2, &err);
        if (back == NULL) {
            testing_t_fatalf_v(t, "%v", err);
        } else {
            /* Ignore the Headers (the User-Agent breaks the deep equal, but we
             * don't care about it). */
            const char *field = request_diff(req, back);
            if (field != NULL) {
                testing_t_errorf_v(t, "Original request doesn't match Request read "
                                      "back.");
                testing_t_logf_v(t, "They differ in %s", cs(field));
                testing_t_logf_v(t, "Wrote: %s", wrote);
            }
            http_request_free(back);
        }
        bufio_reader_free(br2);
    }
    http_request_free(req);
    bufio_reader_free(br);
    ARENA_END;
}

/* responseWriterJustWriter, a ResponseWriter that is only a writer. */
static HttpHeader just_writer_header(void *self) {
    (void)self;
    panic_str(S("should not be called"));
}

static void just_writer_write_header(void *self, Int code) {
    (void)self;
    (void)code;
    panic_str(S("should not be called"));
}

static Int just_writer_write(void *self, Slice p, Error *err) {
    IoWriter *w = (IoWriter *)self;
    return w->vt->write(w->data, p, err);
}

static const HttpResponseWriterVT just_writer_vt = {
    {NULL, just_writer_write},
    just_writer_header,
    just_writer_write_header,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

/* delayedEOFReader never returns (n > 0, io.EOF), instead putting off the
 * io.EOF until a subsequent Read call. */
static Int delayed_eof_read(void *self, Slice p, Error *err) {
    IoReader *r = (IoReader *)self;
    Int n = r->vt->read(r->data, p, err);
    if (n > 0 && same_err(*err, io_eof))
        *err = BURROW_NO_ERROR;
    return n;
}

static const IoReaderVT delayed_eof_vt = {NULL, delayed_eof_read};

static void TestIssue10884_MaxBytesEOF(TestingT *t) {
    ARENA_BEGIN;
    IoWriter dst = io_discard;
    StringsReader sr;
    strings_reader_reset(&sr, S("12345"));
    IoReader inner = strings_reader_as_io_reader(&sr);
    IoNopCloser nc = io_nop_closer((IoReader){&delayed_eof_vt, &inner});
    IoReadCloser rc =
        http_max_bytes_reader(a, (HttpResponseWriter){&just_writer_vt, &dst},
                              io_nop_closer_as_io_read_closer(&nc), 5);
    Error err;
    (void)io_copy(a, dst, io_read_closer_as_io_reader(rc), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    ARENA_END;
}

/* Issue 14981: MaxBytesReader's return error wasn't sticky. It doesn't
 * technically need to be, but people expected it to be. The error, or a zero
 * Str when it is. */
static Str is_sticky(TestingT *t, Alloc *a, IoReader r) {
    StringsBuilder log = STRINGS_BUILDER(a);
    Slice buf = slice_from(mem_alloc(a, 1000, 1), 1000, 1000, TYPE_BYTE);
    Error first = BURROW_NO_ERROR;
    for (;;) {
        Error err;
        Int n = r.vt->read(r.data, buf, &err);
        strings_builder_write_string(
            &log, fmt_sprintf_v(a, "Read(%d) = %d, %v\n", buf.len, n, err), NULL);
        if (BURROW_OK(err))
            continue;
        if (BURROW_OK(first)) {
            first = err;
            continue;
        }
        if (!same_err(err, first))
            return fmt_sprintf_v(a, "non-sticky error. got log:\n%s",
                                 strings_builder_string(&log));
        testing_t_logf_v(t, "Got log: %s", strings_builder_string(&log));
        return (Str){0};
    }
}

static void TestMaxBytesReaderStickyError(TestingT *t) {
    ARENA_BEGIN;
    static const struct {
        Int readable;
        int64_t limit;
    } tests[] = {
        {99, 100},
        {100, 100},
        {101, 100},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        BytesReader br;
        bytes_reader_reset(&br,
                           slice_from(mem_alloc(a, (size_t)tests[i].readable, 1),
                                      tests[i].readable, tests[i].readable, TYPE_BYTE));
        IoNopCloser nc = io_nop_closer(bytes_reader_as_io_reader(&br));
        IoReadCloser rc =
            http_max_bytes_reader(a, (HttpResponseWriter){0},
                                  io_nop_closer_as_io_read_closer(&nc), tests[i].limit);
        Str e = is_sticky(t, a, io_read_closer_as_io_reader(rc));
        if (e.p != NULL)
            testing_t_errorf_v(t, "%d. error: %s", i, e);
    }
    ARENA_END;
}

/* Issue 45101: maxBytesReader's Read panicked when n < -1. This test also
 * ensures that Read treats negative limits as equivalent to 0. */
static void TestMaxBytesReaderDifferentLimits(TestingT *t) {
    ARENA_BEGIN;
    static const char test_str[] = "1234";
    const Int len = (Int)(sizeof test_str - 1);
    const struct {
        int64_t limit;
        Int len_p;
        Int want_n;
        bool want_err;
    } tests[] = {
        /* Ensure we won't return an error when the limit is negative, but we
         * don't need to read. */
        {-123, 0, 0, false},
        {-100, 32 * 1024, 0, true},
        {-2, 1, 0, true},
        {-1, 2, 0, true},
        {0, 3, 0, true},
        {1, 4, 1, true},
        {2, 5, 2, true},
        {3, 2, 2, false},
        {len, len, len, false},
        {100, 6, len, false},
        {INT64_MAX, len, len, false}, /* Issue 54408 */
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        StringsReader sr;
        strings_reader_reset(&sr, cs(test_str));
        IoNopCloser nc = io_nop_closer(strings_reader_as_io_reader(&sr));
        IoReadCloser rc =
            http_max_bytes_reader(a, (HttpResponseWriter){0},
                                  io_nop_closer_as_io_read_closer(&nc), tests[i].limit);
        Byte *p = (Byte *)mem_alloc(a, (size_t)(tests[i].len_p + 1), 1);
        Error err;
        Int n = rc.vt->reader.read(
            rc.data, slice_from(p, tests[i].len_p, tests[i].len_p, TYPE_BYTE), &err);

        if (n != tests[i].want_n)
            testing_t_errorf_v(t, "%d. n: %d, want n: %d", i, n, tests[i].want_n);

        if (BURROW_FAILED(err) != tests[i].want_err)
            testing_t_errorf_v(t, "%d. error: %v", i, err);
    }
    ARENA_END;
}

/* Verify that NewRequest sets Request.GetBody and that it works. */
static void TestNewRequestGetBody(TestingT *t) {
    ARENA_BEGIN;
    StringsReader sr;
    strings_reader_reset(&sr, S("hello"));
    BytesReader br;
    bytes_reader_reset(&br, bytes_of(S("hello")));
    BytesBuffer *bb = bytes_new_buffer(a, bytes_of(S("hello")));
    if (bb == NULL) {
        testing_t_fatalf_v(t, "out of memory");
        ARENA_END;
        return;
    }
    IoReader tests[] = {
        strings_reader_as_io_reader(&sr),
        bytes_reader_as_io_reader(&br),
        bytes_buffer_as_io_reader(bb),
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        Error err;
        HttpRequest *req =
            http_new_request(a, S("POST"), S("http://foo.tld/"), tests[i], &err);
        if (req == NULL) {
            testing_t_errorf_v(t, "test[%d]: %v", i, err);
            continue;
        }
        if (req->body.vt == NULL) {
            testing_t_errorf_v(t, "test[%d]: Body = nil", i);
            http_request_free(req);
            continue;
        }
        if (req->get_body.f == NULL) {
            testing_t_errorf_v(t, "test[%d]: GetBody = nil", i);
            http_request_free(req);
            continue;
        }
        Slice slurp1 = io_read_all(a, io_read_closer_as_io_reader(req->body), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "test[%d]: ReadAll(Body) = %v", i, err);
        IoReadCloser new_body = req->get_body.f(req->get_body.env, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test[%d]: GetBody = %v", i, err);
            http_request_free(req);
            continue;
        }
        Slice slurp2 = io_read_all(a, io_read_closer_as_io_reader(new_body), &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "test[%d]: ReadAll(GetBody()) = %v", i, err);
        Str s1 = str_from_bytes((const Byte *)slurp1.p, slurp1.len);
        Str s2 = str_from_bytes((const Byte *)slurp2.p, slurp2.len);
        if (!str_eq(s1, s2))
            testing_t_errorf_v(t, "test[%d]: Body %q != GetBody %q", i, s1, s2);
        http_request_free(req);
    }
    ARENA_END;
}

/* Issue 53181: verify Request.Cookie return the correct Cookie. Return
 * ErrNoCookie instead of the first cookie when name is "". */
static void TestRequestCookie(TestingT *t) {
    ARENA_BEGIN;
    const struct {
        const char *name;
        const char *value;
        const Error *expected_err;
    } tests[] = {
        {"foo", "bar", NULL},
        {"", "", &http_err_no_cookie},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        Error err;
        HttpRequest *req = http_new_request(a, S("GET"), S("http://example.com/"),
                                            (IoReader){0}, &err);
        if (req == NULL) {
            testing_t_fatalf_v(t, "%v", err);
            break;
        }
        HttpCookie add = {0};
        add.name = cs(tests[i].name);
        add.value = cs(tests[i].value);
        (void)http_request_add_cookie(req, a, &add);
        HttpCookie c = http_request_cookie(req, a, cs(tests[i].name), &err);
        Error want =
            tests[i].expected_err != NULL ? *tests[i].expected_err : BURROW_NO_ERROR;
        if (!same_err(err, want))
            testing_t_errorf_v(t, "got %v, want %v", err, want);

        /* skip if error occurred. */
        if (BURROW_OK(err)) {
            if (!str_eq(c.value, add.value))
                testing_t_errorf_v(t, "got %v, want %v", c.value, add.value);
            if (!str_eq(c.name, add.name))
                testing_t_errorf_v(t, "got %s, want %v", add.name, c.name);
        }
        http_request_free(req);
    }
    ARENA_END;
}

/* A cookie as Go's readCookies makes one: a name and a value and nothing
 * else, which reflect.DeepEqual checks against &Cookie{Name:, Value:}. */
static bool cookie_is(const HttpCookie *c, const char *name, const char *value) {
    return str_eq(c->name, cs(name)) && str_eq(c->value, cs(value)) &&
           c->path.len == 0 && c->domain.len == 0 && time_is_zero(c->expires) &&
           c->raw_expires.len == 0 && c->max_age == 0 && c->raw.len == 0 &&
           c->unparsed.len == 0 && c->same_site == 0 && !c->quoted && !c->secure &&
           !c->http_only && !c->partitioned;
}

typedef struct CookiePair {
    const char *name;
    const char *value;
} CookiePair;

typedef struct CookiesByNameCase {
    CookiePair in[3];
    Int nin;
    const char *filter;
    CookiePair want[2];
    Int nwant;
} CookiesByNameCase;

static const CookiesByNameCase cookies_by_name_tests[] = {
    {{{"foo", "foo-1"}, {"bar", "bar"}}, 2, "foo", {{"foo", "foo-1"}}, 1},
    {{{"foo", "foo-1"}, {"foo", "foo-2"}, {"bar", "bar"}},
     3,
     "foo",
     {{"foo", "foo-1"}, {"foo", "foo-2"}},
     2},
    {{{"bar", "bar"}}, 1, "foo", {{NULL, NULL}}, 0},
    {{{"bar", "bar"}}, 1, "", {{NULL, NULL}}, 0},
    {{{NULL, NULL}}, 0, "foo", {{NULL, NULL}}, 0},
};

static Str cookies_v(Alloc *a, const HttpCookie *c, Int n) {
    StringsBuilder b = STRINGS_BUILDER(a);
    strings_builder_write_byte(&b, '[');
    for (Int i = 0; i < n; i++) {
        if (i > 0)
            strings_builder_write_string(&b, S(", "), NULL);
        strings_builder_write_string(
            &b, fmt_sprintf_v(a, "{%q: %q}", c[i].name, c[i].value), NULL);
    }
    strings_builder_write_byte(&b, ']');
    return strings_builder_string(&b);
}

static void cookies_by_name_case(void *env, TestingT *t) {
    const CookiesByNameCase *tt = (const CookiesByNameCase *)env;
    ARENA_BEGIN;
    Error err;
    HttpRequest *req =
        http_new_request(a, S("GET"), S("http://example.com/"), (IoReader){0}, &err);
    if (req == NULL) {
        testing_t_fatalf_v(t, "%v", err);
        ARENA_END;
        return;
    }
    for (Int i = 0; i < tt->nin; i++) {
        HttpCookie c = {0};
        c.name = cs(tt->in[i].name);
        c.value = cs(tt->in[i].value);
        (void)http_request_add_cookie(req, a, &c);
    }

    Slice got = http_request_cookies_named(req, a, cs(tt->filter));
    const HttpCookie *gc = (const HttpCookie *)got.p;

    bool same = got.len == tt->nwant;
    for (Int i = 0; same && i < tt->nwant; i++)
        same = cookie_is(&gc[i], tt->want[i].name, tt->want[i].value);
    if (!same) {
        HttpCookie want[2] = {{0}};
        for (Int i = 0; i < tt->nwant; i++) {
            want[i].name = cs(tt->want[i].name);
            want[i].value = cs(tt->want[i].value);
        }
        testing_t_errorf_v(t, "Result mismatch\n\tGot: %s\n\tWant: %s",
                           cookies_v(a, gc, got.len), cookies_v(a, want, tt->nwant));
    }
    http_request_free(req);
    ARENA_END;
}

static void TestRequestCookiesByName(TestingT *t) {
    Int n = (Int)(sizeof cookies_by_name_tests / sizeof cookies_by_name_tests[0]);
    for (Int i = 0; i < n; i++)
        testing_t_run(t, cs(cookies_by_name_tests[i].filter),
                      BURROW_FN(TestingTFunc, cookies_by_name_case,
                                (void *)(uintptr_t)&cookies_by_name_tests[i]));
}

#define TESTS(X)                                                                       \
    X(TestReadRequest)                                                                 \
    X(TestReadRequest_Bad)                                                             \
    X(TestReadRequestErrors)                                                           \
    X(TestParseHTTPVersion)                                                            \
    X(TestGetBasicAuth)                                                                \
    X(TestParseBasicAuth)                                                              \
    X(TestSetBasicAuth)                                                                \
    X(TestBodyReadBadTrailer)                                                          \
    X(TestParseTransferEncoding)                                                       \
    X(TestParseContentLength)                                                          \
    X(TestNewRequestHost)                                                              \
    X(TestRequestInvalidMethod)                                                        \
    X(TestRequestWriteBufferedWriter)                                                  \
    X(TestRequestBadHostHeader)                                                        \
    X(TestRequestBadUserAgent)                                                         \
    X(TestStarRequest)                                                                 \
    X(TestIssue10884_MaxBytesEOF)                                                      \
    X(TestMaxBytesReaderStickyError)                                                   \
    X(TestMaxBytesReaderDifferentLimits)                                               \
    X(TestNewRequestGetBody)                                                           \
    X(TestRequestCookie)                                                               \
    X(TestRequestCookiesByName)

TESTING_MAIN(TESTS)
