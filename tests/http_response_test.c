/* Derived from Go's src/net/http/response_test.go, the tests of reading a
 * response, the ones that write back what they read, and
 * TestFinalChunkedBodyReadEOF from transfer_test.go.
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
#include "burrow/compress/gzip.h"
#include "burrow/crypto/rand.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
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

static Error body_close(IoReadCloser rc) {
    return rc.vt->closer.close(rc.data);
}

/* --------------------------------------------------------- TestReadResponse */

/* A string with its length, for the ones with NUL bytes in them. */
#define L(s) {s, sizeof(s) - 1}

typedef struct RespTest {
    struct {
        const char *p;
        size_t n;
    } raw, raw_out, body;
    const char *status;
    const char *proto;
    const char *request; /* dummyReq's method, NULL for a nil Request */
    const char *header;
    int64_t content_length;
    Int status_code;
    Int proto_major;
    Int proto_minor;
    bool close;
    bool chunked; /* TransferEncoding: []string{"chunked"} */
} RespTest;

static const RespTest resp_tests[] = {
    /* Unchunked response without Content-Length. */
    {
        .raw = L("HTTP/1.0 200 OK\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "Body here\n"),
        .raw_out = L("HTTP/1.0 200 OK\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "Body here\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.0",
        .proto_major = 1,
        .proto_minor = 0,
        .request = "GET",
        .header = "map[Connection:[close]]",
        .close = true,
        .content_length = -1,
        .body = L("Body here\n"),
    },

    /* Unchunked HTTP/1.1 response without Content-Length or Connection
     * headers. */
    {
        .raw = L("HTTP/1.1 200 OK\r\n"
                 "\r\n"
                 "Body here\n"),
        .raw_out = L("HTTP/1.1 200 OK\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "Body here\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .header = "map[]",
        .request = "GET",
        .close = true,
        .content_length = -1,
        .body = L("Body here\n"),
    },

    /* Unchunked HTTP/1.1 204 response without Content-Length. */
    {
        .raw = L("HTTP/1.1 204 No Content\r\n"
                 "\r\n"
                 "Body should not be read!\n"),
        .raw_out = L("HTTP/1.1 204 No Content\r\n"
                     "\r\n"),
        .status = "204 No Content",
        .status_code = 204,
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .header = "map[]",
        .request = "GET",
        .close = false,
        .content_length = 0,
        .body = L(""),
    },

    /* Unchunked response with Content-Length. */
    {
        .raw = L("HTTP/1.0 200 OK\r\n"
                 "Content-Length: 10\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "Body here\n"),
        .raw_out = L("HTTP/1.0 200 OK\r\n"
                     "Content-Length: 10\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "Body here\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.0",
        .proto_major = 1,
        .proto_minor = 0,
        .request = "GET",
        .header = "map[Connection:[close] Content-Length:[10]]",
        .close = true,
        .content_length = 10,
        .body = L("Body here\n"),
    },

    /* Chunked response without Content-Length. */
    {
        .raw = L("HTTP/1.1 200 OK\r\n"
                 "Transfer-Encoding: chunked\r\n"
                 "\r\n"
                 "0a\r\n"
                 "Body here\n\r\n"
                 "09\r\n"
                 "continued\r\n"
                 "0\r\n"
                 "\r\n"),
        .raw_out = L("HTTP/1.1 200 OK\r\n"
                     "Transfer-Encoding: chunked\r\n"
                     "\r\n"
                     "13\r\n"
                     "Body here\n"
                     "continued\r\n"
                     "0\r\n"
                     "\r\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .request = "GET",
        .header = "map[]",
        .close = false,
        .content_length = -1,
        .chunked = true,
        .body = L("Body here\ncontinued"),
    },

    /* Trailer header but no TransferEncoding */
    {
        .raw = L("HTTP/1.0 200 OK\r\n"
                 "Trailer: Content-MD5, Content-Sources\r\n"
                 "Content-Length: 10\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "Body here\n"),
        .raw_out = L("HTTP/1.0 200 OK\r\n"
                     "Content-Length: 10\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "Body here\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.0",
        .proto_major = 1,
        .proto_minor = 0,
        .request = "GET",
        .header = "map[Connection:[close] Content-Length:[10] Trailer:[Content-MD5, "
                  "Content-Sources]]",
        .close = true,
        .content_length = 10,
        .body = L("Body here\n"),
    },

    /* Chunked response with Content-Length. */
    {
        .raw = L("HTTP/1.1 200 OK\r\n"
                 "Transfer-Encoding: chunked\r\n"
                 "Content-Length: 10\r\n"
                 "\r\n"
                 "0a\r\n"
                 "Body here\n\r\n"
                 "0\r\n"
                 "\r\n"),
        .raw_out = L("HTTP/1.1 200 OK\r\n"
                     "Transfer-Encoding: chunked\r\n"
                     "\r\n"
                     "a\r\n"
                     "Body here\n"
                     "\r\n"
                     "0\r\n"
                     "\r\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .request = "GET",
        .header = "map[]",
        .close = false,
        .content_length = -1,
        .chunked = true,
        .body = L("Body here\n"),
    },

    /* Chunked response in response to a HEAD request */
    {
        .raw = L("HTTP/1.1 200 OK\r\n"
                 "Transfer-Encoding: chunked\r\n"
                 "\r\n"),
        .raw_out = L("HTTP/1.1 200 OK\r\n"
                     "Transfer-Encoding: chunked\r\n"
                     "\r\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .request = "HEAD",
        .header = "map[]",
        .chunked = true,
        .close = false,
        .content_length = -1,
        .body = L(""),
    },

    /* Content-Length in response to a HEAD request */
    {
        .raw = L("HTTP/1.0 200 OK\r\n"
                 "Content-Length: 256\r\n"
                 "\r\n"),
        .raw_out = L("HTTP/1.0 200 OK\r\n"
                     "Connection: close\r\n"
                     "Content-Length: 256\r\n"
                     "\r\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.0",
        .proto_major = 1,
        .proto_minor = 0,
        .request = "HEAD",
        .header = "map[Content-Length:[256]]",
        .close = true,
        .content_length = 256,
        .body = L(""),
    },

    /* Content-Length in response to a HEAD request with HTTP/1.1 */
    {
        .raw = L("HTTP/1.1 200 OK\r\n"
                 "Content-Length: 256\r\n"
                 "\r\n"),
        .raw_out = L("HTTP/1.1 200 OK\r\n"
                     "Content-Length: 256\r\n"
                     "\r\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .request = "HEAD",
        .header = "map[Content-Length:[256]]",
        .close = false,
        .content_length = 256,
        .body = L(""),
    },

    /* No Content-Length or Chunked in response to a HEAD request */
    {
        .raw = L("HTTP/1.0 200 OK\r\n"
                 "\r\n"),
        .raw_out = L("HTTP/1.0 200 OK\r\n"
                     "Connection: close\r\n"
                     "\r\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.0",
        .proto_major = 1,
        .proto_minor = 0,
        .request = "HEAD",
        .header = "map[]",
        .close = true,
        .content_length = -1,
        .body = L(""),
    },

    /* explicit Content-Length of 0. */
    {
        .raw = L("HTTP/1.1 200 OK\r\n"
                 "Content-Length: 0\r\n"
                 "\r\n"),
        .raw_out = L("HTTP/1.1 200 OK\r\n"
                     "Content-Length: 0\r\n"
                     "\r\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .request = "GET",
        .header = "map[Content-Length:[0]]",
        .close = false,
        .content_length = 0,
        .body = L(""),
    },

    /* Status line without a Reason-Phrase, but trailing space. (permitted by
     * RFC 7230, section 3.1.2) */
    {
        .raw = L("HTTP/1.0 303 \r\n\r\n"),
        .raw_out = L("HTTP/1.0 303 \r\n"
                     "Connection: close\r\n"
                     "\r\n"),
        .status = "303 ",
        .status_code = 303,
        .proto = "HTTP/1.0",
        .proto_major = 1,
        .proto_minor = 0,
        .request = "GET",
        .header = "map[]",
        .close = true,
        .content_length = -1,
        .body = L(""),
    },

    /* Status line without a Reason-Phrase, and no trailing space. (not
     * permitted by RFC 7230, but we'll accept it anyway) */
    {
        .raw = L("HTTP/1.0 303\r\n\r\n"),
        .raw_out = L("HTTP/1.0 303 303\r\n"
                     "Connection: close\r\n"
                     "\r\n"),
        .status = "303",
        .status_code = 303,
        .proto = "HTTP/1.0",
        .proto_major = 1,
        .proto_minor = 0,
        .request = "GET",
        .header = "map[]",
        .close = true,
        .content_length = -1,
        .body = L(""),
    },

    /* golang.org/issue/4767: don't special-case multipart/byteranges
     * responses */
    {
        .raw = L("HTTP/1.1 206 Partial Content\n"
                 "Connection: close\n"
                 "Content-Type: multipart/byteranges; boundary=18a75608c8f47cef\n"
                 "\n"
                 "some body"),
        .raw_out = L("HTTP/1.1 206 Partial Content\r\n"
                     "Connection: close\r\n"
                     "Content-Type: multipart/byteranges; boundary=18a75608c8f47cef\r\n"
                     "\r\n"
                     "some body"),
        .status = "206 Partial Content",
        .status_code = 206,
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .request = "GET",
        .header = "map[Content-Type:[multipart/byteranges; boundary=18a75608c8f47cef]]",
        .close = true,
        .content_length = -1,
        .body = L("some body"),
    },

    /* Unchunked response without Content-Length, Request is nil */
    {
        .raw = L("HTTP/1.0 200 OK\r\n"
                 "Connection: close\r\n"
                 "\r\n"
                 "Body here\n"),
        .raw_out = L("HTTP/1.0 200 OK\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "Body here\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.0",
        .proto_major = 1,
        .proto_minor = 0,
        .header = "map[Connection:[close]]",
        .close = true,
        .content_length = -1,
        .body = L("Body here\n"),
    },

    /* 206 Partial Content. golang.org/issue/8923 */
    {
        .raw = L("HTTP/1.1 206 Partial Content\r\n"
                 "Content-Type: text/plain; charset=utf-8\r\n"
                 "Accept-Ranges: bytes\r\n"
                 "Content-Range: bytes 0-5/1862\r\n"
                 "Content-Length: 6\r\n\r\n"
                 "foobar"),
        .raw_out = L("HTTP/1.1 206 Partial Content\r\n"
                     "Content-Length: 6\r\n"
                     "Accept-Ranges: bytes\r\n"
                     "Content-Range: bytes 0-5/1862\r\n"
                     "Content-Type: text/plain; charset=utf-8\r\n"
                     "\r\n"
                     "foobar"),
        .status = "206 Partial Content",
        .status_code = 206,
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .request = "GET",
        .header = "map[Accept-Ranges:[bytes] Content-Length:[6] Content-Range:[bytes "
                  "0-5/1862] Content-Type:[text/plain; charset=utf-8]]",
        .content_length = 6,
        .body = L("foobar"),
    },

    /* Both keep-alive and close, on the same Connection line. (Issue 8840) */
    {
        .raw = L("HTTP/1.1 200 OK\r\n"
                 "Content-Length: 256\r\n"
                 "Connection: keep-alive, close\r\n"
                 "\r\n"),
        .raw_out = L("HTTP/1.1 200 OK\r\n"
                     "Connection: close\r\n"
                     "Content-Length: 256\r\n"
                     "\r\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .request = "HEAD",
        .header = "map[Content-Length:[256]]",
        .close = true,
        .content_length = 256,
        .body = L(""),
    },

    /* Both keep-alive and close, on different Connection lines. (Issue
     * 8840) */
    {
        .raw = L("HTTP/1.1 200 OK\r\n"
                 "Content-Length: 256\r\n"
                 "Connection: keep-alive\r\n"
                 "Connection: close\r\n"
                 "\r\n"),
        .raw_out = L("HTTP/1.1 200 OK\r\n"
                     "Connection: close\r\n"
                     "Content-Length: 256\r\n"
                     "\r\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .request = "HEAD",
        .header = "map[Content-Length:[256]]",
        .close = true,
        .content_length = 256,
        .body = L(""),
    },

    /* Issue 12785: HTTP/1.0 response with bogus (to be ignored)
     * Transfer-Encoding. Without a Content-Length. */
    {
        .raw = L("HTTP/1.0 200 OK\r\n"
                 "Transfer-Encoding: bogus\r\n"
                 "\r\n"
                 "Body here\n"),
        .raw_out = L("HTTP/1.0 200 OK\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "Body here\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.0",
        .proto_major = 1,
        .proto_minor = 0,
        .request = "GET",
        .header = "map[]",
        .close = true,
        .content_length = -1,
        .body = L("Body here\n"),
    },

    /* Issue 12785: HTTP/1.0 response with bogus (to be ignored)
     * Transfer-Encoding. With a Content-Length. */
    {
        .raw = L("HTTP/1.0 200 OK\r\n"
                 "Transfer-Encoding: bogus\r\n"
                 "Content-Length: 10\r\n"
                 "\r\n"
                 "Body here\n"),
        .raw_out = L("HTTP/1.0 200 OK\r\n"
                     "Connection: close\r\n"
                     "Content-Length: 10\r\n"
                     "\r\n"
                     "Body here\n"),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.0",
        .proto_major = 1,
        .proto_minor = 0,
        .request = "GET",
        .header = "map[Content-Length:[10]]",
        .close = true,
        .content_length = 10,
        .body = L("Body here\n"),
    },

    {
        .raw =
            L("HTTP/1.1 200 OK\r\n"
              "Content-Encoding: gzip\r\n"
              "Content-Length: 23\r\n"
              "Connection: keep-alive\r\n"
              "Keep-Alive: timeout=7200\r\n\r\n"
              "\x1f\x8b\b\x00\x00\x00\x00\x00\x00\x00s\xf3\xf7\a\x00\xab'\xd4\x1a\x03"
              "\x00\x00\x00"),
        .raw_out = L("HTTP/1.1 200 OK\r\n"
                     "Content-Length: 23\r\n"
                     "Connection: keep-alive\r\n"
                     "Content-Encoding: gzip\r\n"
                     "Keep-Alive: timeout=7200\r\n"
                     "\r\n"
                     "\x1f"
                     "\x8b"
                     "\x08"
                     "\x00"
                     "\x00"
                     "\x00"
                     "\x00"
                     "\x00"
                     "\x00"
                     "\x00"
                     "s\xf3"
                     "\xf7"
                     "\x07"
                     "\x00"
                     "\xab"
                     "'\xd4"
                     "\x1a"
                     "\x03"
                     "\x00"
                     "\x00"
                     "\x00"
                     ""),
        .status = "200 OK",
        .status_code = 200,
        .proto = "HTTP/1.1",
        .proto_major = 1,
        .proto_minor = 1,
        .request = "GET",
        .header = "map[Connection:[keep-alive] Content-Encoding:[gzip] "
                  "Content-Length:[23] Keep-Alive:[timeout=7200]]",
        .close = false,
        .content_length = 23,
        .body =
            L("\x1f\x8b\b\x00\x00\x00\x00\x00\x00\x00s\xf3\xf7\a\x00\xab'\xd4\x1a\x03"
              "\x00\x00\x00"),
    },

    /* Issue 19989: two spaces between HTTP version and status. */
    {
        .raw = L("HTTP/1.0  401 Unauthorized\r\n"
                 "Content-type: text/html\r\n"
                 "WWW-Authenticate: Basic realm=\"\"\r\n\r\n"
                 "Your Authentication failed.\r\n"),
        .raw_out = L("HTTP/1.0 401 Unauthorized\r\n"
                     "Connection: close\r\n"
                     "Content-Type: text/html\r\n"
                     "Www-Authenticate: Basic realm=\"\"\r\n"
                     "\r\n"
                     "Your Authentication failed.\r\n"),
        .status = "401 Unauthorized",
        .status_code = 401,
        .proto = "HTTP/1.0",
        .proto_major = 1,
        .proto_minor = 0,
        .request = "GET",
        .header = "map[Content-Type:[text/html] Www-Authenticate:[Basic realm=\"\"]]",
        .close = true,
        .content_length = -1,
        .body = L("Your Authentication failed.\r\n"),
    },
};

static void check_str_field(TestingT *t, Int i, const char *name, Str got,
                            const char *want) {
    if (!str_eq(got, cs(want)))
        testing_t_errorf_v(t, "#%d Response: %s = %q want %q", i, cs(name), got,
                           cs(want));
}

/* tests successful calls to ReadResponse, and inspects the returned Response.
 * For error cases, see TestReadResponseErrors below. */
static void TestReadResponse(TestingT *t) {
    for (Int i = 0; i < (Int)(sizeof resp_tests / sizeof resp_tests[0]); i++) {
        const RespTest *tt = &resp_tests[i];
        ARENA_BEGIN;
        HttpRequest dummy = {0};
        dummy.method = cs(tt->request);
        StringsReader sr;
        BufioReader *br = reader_of(a, &sr, str_from_bytes(tt->raw.p, (Int)tt->raw.n));
        Error err;
        HttpResponse *resp =
            http_read_response(a, br, tt->request != NULL ? &dummy : NULL, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d: %v", i, err);
            bufio_reader_free(br);
            ARENA_END;
            continue;
        }
        check_str_field(t, i, "Status", resp->status, tt->status);
        if (resp->status_code != tt->status_code)
            testing_t_errorf_v(t, "#%d Response: StatusCode = %d want %d", i,
                               resp->status_code, tt->status_code);
        check_str_field(t, i, "Proto", resp->proto, tt->proto);
        if (resp->proto_major != tt->proto_major ||
            resp->proto_minor != tt->proto_minor)
            testing_t_errorf_v(
                t, "#%d Response: ProtoMajor, ProtoMinor = %d, %d want %d, %d", i,
                resp->proto_major, resp->proto_minor, tt->proto_major, tt->proto_minor);
        check_str_field(t, i, "Header", header_v(a, resp->header), tt->header);
        if (resp->content_length != tt->content_length)
            testing_t_errorf_v(t, "#%d Response: ContentLength = %d want %d", i,
                               resp->content_length, tt->content_length);
        bool chunked =
            resp->transfer_encoding.len == 1 &&
            str_eq(((const Str *)resp->transfer_encoding.p)[0], S("chunked"));
        if (chunked != tt->chunked || (!chunked && resp->transfer_encoding.len != 0))
            testing_t_errorf_v(t, "#%d Response: TransferEncoding = %q", i,
                               resp->transfer_encoding);
        if (resp->close != tt->close)
            testing_t_errorf_v(t, "#%d Response: Close = %t want %t", i, resp->close,
                               tt->close);
        if (resp->trailer != NULL)
            testing_t_errorf_v(t, "#%d Response: Trailer = %v want nil", i,
                               header_v(a, resp->trailer));
        if (resp->uncompressed)
            testing_t_errorf_v(t, "#%d Response: Uncompressed = true want false", i);
        if (resp->request != (tt->request != NULL ? &dummy : NULL))
            testing_t_errorf_v(t, "#%d Response: Request is not the one passed in", i);

        Slice body = io_read_all(a, io_read_closer_as_io_reader(resp->body), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d: %v", i, err);
        } else {
            (void)body_close(resp->body);
            Str got = str_from_bytes(body.p, body.len);
            Str want = str_from_bytes(tt->body.p, (Int)tt->body.n);
            if (!str_eq(got, want))
                testing_t_errorf_v(t, "#%d: Body = %q want %q", i, got, want);
        }
        http_response_free(resp);
        bufio_reader_free(br);
        ARENA_END;
    }
}

static void TestWriteResponse(TestingT *t) {
    for (Int i = 0; i < (Int)(sizeof resp_tests / sizeof resp_tests[0]); i++) {
        const RespTest *tt = &resp_tests[i];
        ARENA_BEGIN;
        HttpRequest dummy = {0};
        dummy.method = cs(tt->request);
        StringsReader sr;
        BufioReader *br = reader_of(a, &sr, str_from_bytes(tt->raw.p, (Int)tt->raw.n));
        Error err;
        HttpResponse *resp =
            http_read_response(a, br, tt->request != NULL ? &dummy : NULL, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d: %v", i, err);
            bufio_reader_free(br);
            ARENA_END;
            continue;
        }
        BytesBuffer buf = BYTES_BUFFER(a);
        err = http_response_write(resp, bytes_buffer_as_io_writer(&buf));
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d: %v", i, err);
        } else {
            Str got = bytes_buffer_string(&buf, a);
            Str want = str_from_bytes(tt->raw_out.p, (Int)tt->raw_out.n);
            if (!str_eq(got, want))
                testing_t_errorf_v(t,
                                   "#%d: response differs; "
                                   "got:\n----\n%v\n----\nwant:\n----\n%v\n----\n",
                                   i, strings_replace_all(a, got, S("\r"), S("\\r")),
                                   strings_replace_all(a, want, S("\r"), S("\\r")));
        }
        http_response_free(resp);
        bufio_reader_free(br);
        ARENA_END;
    }
}

static void TestResponseStatusStutter(TestingT *t) {
    ARENA_BEGIN;
    HttpResponse r = {0};
    r.status = S("123 some status");
    r.status_code = 123;
    r.proto_major = 1;
    r.proto_minor = 3;
    StringsBuilder buf = STRINGS_BUILDER(a);
    (void)http_response_write(&r, strings_builder_as_io_writer(&buf));
    if (strings_contains(strings_builder_string(&buf), S("123 123")))
        testing_t_errorf_v(t, "stutter in status: %s", strings_builder_string(&buf));
    ARENA_END;
}

static void TestResponseWritesOnlySingleConnectionClose(TestingT *t) {
    ARENA_BEGIN;
    Str connection_close_header = S("Connection: close");

    StringsReader sr;
    BufioReader *br = reader_of(a, &sr, S("HTTP/1.0 200 OK\r\n\r\nAAAA"));
    Error err;
    HttpResponse *res = http_read_response(a, br, NULL, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadResponse failed %v", err);

    BytesBuffer buf1 = BYTES_BUFFER(a);
    err = http_response_write(res, bytes_buffer_as_io_writer(&buf1));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Write failed %v", err);
    BufioReader *br2 = bufio_new_reader(a, bytes_buffer_as_io_reader(&buf1));
    HttpResponse *res2 = http_read_response(a, br2, NULL, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "ReadResponse failed %v", err);

    StringsBuilder buf2 = STRINGS_BUILDER(a);
    err = http_response_write(res2, strings_builder_as_io_writer(&buf2));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Write failed %v", err);
    Int count = strings_count(strings_builder_string(&buf2), connection_close_header);
    if (count != 1)
        testing_t_errorf_v(t, "Found %d %q header", count, connection_close_header);
    http_response_free(res2);
    http_response_free(res);
    bufio_reader_free(br2);
    bufio_reader_free(br);
    ARENA_END;
}

/* ------------------------------------------- TestReadResponseCloseInMiddle */

static void close_in_middle(TestingT *t, bool chunked, bool compressed) {
    ARENA_BEGIN;
    BytesBuffer buf = BYTES_BUFFER(a);
    bytes_buffer_write_string(&buf, S("HTTP/1.1 200 OK\r\n"), NULL);
    if (chunked)
        bytes_buffer_write_string(&buf, S("Transfer-Encoding: chunked\r\n"), NULL);
    else
        bytes_buffer_write_string(&buf, S("Content-Length: 1000000\r\n"), NULL);
    IoWriter wr = bytes_buffer_as_io_writer(&buf);
    HttpChunkedWriter cw = {wr, NULL};
    if (chunked)
        wr = io_write_closer_as_io_writer(
            burrow__http_chunked_writer_as_io_write_closer(&cw));
    GzipWriter *zw = NULL;
    if (compressed) {
        bytes_buffer_write_string(&buf, S("Content-Encoding: gzip\r\n"), NULL);
        zw = gzip_new_writer(a, wr);
        wr = gzip_writer_as_io_writer(zw);
    }
    bytes_buffer_write_string(&buf, S("\r\n"), NULL);

    Byte chunk[1000];
    memset(chunk, 'x', sizeof chunk);
    Error err;
    for (int i = 0; i < 1000; i++) {
        if (compressed) {
            /* Otherwise this compresses too well. */
            (void)crypto_rand_read(slice_from(chunk, 1000, 1000, TYPE_BYTE), &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(
                    t, "on test chunked=%t, compressed=%t: rand.Reader ReadFull: %v",
                    chunked, compressed, err);
        }
        (void)wr.vt->write(wr.data, slice_from(chunk, 1000, 1000, TYPE_BYTE), &err);
    }
    if (compressed) {
        err = gzip_writer_close(zw);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(
                t, "on test chunked=%t, compressed=%t: compressor close: %v", chunked,
                compressed, err);
    }
    if (chunked)
        bytes_buffer_write_string(&buf, S("0\r\n\r\n"), NULL);
    bytes_buffer_write_string(&buf, S("Next Request Here"), NULL);

    BufioReader *bufr = bufio_new_reader(a, bytes_buffer_as_io_reader(&buf));
    HttpRequest dummy = {0};
    dummy.method = S("GET");
    HttpResponse *resp = http_read_response(a, bufr, &dummy, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "on test chunked=%t, compressed=%t: ReadResponse: %v",
                           chunked, compressed, err);
    int64_t expected_length = -1;
    if (!chunked)
        expected_length = 1000000;
    if (resp->content_length != expected_length)
        testing_t_fatalf_v(
            t,
            "on test chunked=%t, compressed=%t: expected response length %d, "
            "got %d",
            chunked, compressed, expected_length, resp->content_length);
    if (resp->body.vt == NULL)
        testing_t_fatalf_v(t, "on test chunked=%t, compressed=%t: nil body", chunked,
                           compressed);
    IoReader body = io_read_closer_as_io_reader(resp->body);
    GzipReader *zr = NULL;
    if (compressed) {
        zr = gzip_new_reader(a, body, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t,
                               "on test chunked=%t, compressed=%t: gzip.NewReader: %v",
                               chunked, compressed, err);
        body = gzip_reader_as_io_reader(zr);
    }

    Byte rbuf[2500];
    Int n = io_read_full(body, slice_from(rbuf, 2500, 2500, TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t,
                           "on test chunked=%t, compressed=%t: 2500 byte ReadFull: %v",
                           chunked, compressed, err);
    if (n != 2500)
        testing_t_fatalf_v(
            t, "on test chunked=%t, compressed=%t: ReadFull only read %d bytes",
            chunked, compressed, n);
    if (!compressed) {
        Byte want[2500];
        memset(want, 'x', sizeof want);
        if (memcmp(rbuf, want, sizeof want) != 0)
            testing_t_fatalf_v(
                t,
                "on test chunked=%t, compressed=%t: ReadFull didn't read 2500 "
                "'x'; got %q",
                chunked, compressed, str_from_bytes(rbuf, 2500));
    }
    (void)body_close(resp->body);

    Slice rest = io_read_all(a, bufio_reader_as_io_reader(bufr), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(
            t, "on test chunked=%t, compressed=%t: ReadAll on remainder: %v", chunked,
            compressed, err);
    Str g = str_from_bytes(rest.p, rest.len);
    if (!str_eq(g, S("Next Request Here"))) {
        /* Go shortens the runs of x before printing. */
        Str shown = g.len > 200 ? S("(a long remainder)") : g;
        testing_t_fatalf_v(
            t, "on test chunked=%t, compressed=%t: remainder = %q, expected %q",
            chunked, compressed, shown, S("Next Request Here"));
    }
    if (zr != NULL)
        gzip_reader_free(zr);
    if (zw != NULL)
        gzip_writer_free(zw);
    http_response_free(resp);
    bufio_reader_free(bufr);
    bytes_buffer_free(&buf);
    ARENA_END;
}

/* TestReadResponseCloseInMiddle tests that closing a body after reading only
 * part of its contents advances the read to the end of the request, right up
 * until the next request. */
static void TestReadResponseCloseInMiddle(TestingT *t) {
    static const struct {
        bool chunked, compressed;
    } tests[] = {
        {false, false},
        {true, false},
        {true, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        close_in_middle(t, tests[i].chunked, tests[i].compressed);
}

/* ------------------------------------------------------ TestLocationResponse */

static void TestLocationResponse(TestingT *t) {
    static const struct {
        const char *location; /* Response's Location header or "" */
        const char *requrl;   /* Response.Request.URL or "" */
        const char *want;
        bool want_err; /* ErrNoLocation */
    } tests[] = {
        {"/foo", "http://bar.com/baz", "http://bar.com/foo", false},
        {"http://foo.com/", "http://bar.com/baz", "http://foo.com/", false},
        {"", "http://bar.com/baz", "", true},
        {"/bar", "", "/bar", false},
    };
    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        ARENA_BEGIN;
        HttpResponse res = {0};
        res.header = http_header_make(a);
        http_header_set(res.header, S("Location"), cs(tests[i].location));
        HttpRequest req = {0};
        Error err;
        if (tests[i].requrl[0] != '\0') {
            res.request = &req;
            req.url = url_parse(a, cs(tests[i].requrl), &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "bad test URL %q: %v", cs(tests[i].requrl), err);
        }

        Url *got = http_response_location(&res, a, &err);
        if (tests[i].want_err) {
            if (BURROW_OK(err))
                testing_t_errorf_v(t, "%d. err=nil; want %q", i,
                                   error_text(http_err_no_location));
            else if (!str_eq(error_text(err), error_text(http_err_no_location)))
                testing_t_errorf_v(t, "%d. err=%q; want %q", i, error_text(err),
                                   error_text(http_err_no_location));
        } else if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%d. err=%q", i, error_text(err));
        } else {
            Str g = url_string(got, a);
            if (!str_eq(g, cs(tests[i].want)))
                testing_t_errorf_v(t, "%d. Location=%q; want %q", i, g,
                                   cs(tests[i].want));
        }
        ARENA_END;
    }
}

static void TestResponseContentLengthShortBody(TestingT *t) {
    ARENA_BEGIN;
    Str short_body = S("Short body, not 123 bytes.");
    StringsReader sr;
    BufioReader *br = reader_of(a, &sr,
                                S("HTTP/1.1 200 OK\r\n"
                                  "Content-Length: 123\r\n"
                                  "\r\n"
                                  "Short body, not 123 bytes."));
    HttpRequest req = {0};
    req.method = S("GET");
    Error err;
    HttpResponse *res = http_read_response(a, br, &req, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    if (res->content_length != 123)
        testing_t_fatalf_v(t, "Content-Length = %d; want 123", res->content_length);
    BytesBuffer buf = BYTES_BUFFER(a);
    int64_t n = io_copy(a, bytes_buffer_as_io_writer(&buf),
                        io_read_closer_as_io_reader(res->body), &err);
    if (n != short_body.len)
        testing_t_errorf_v(t, "Copied %d bytes; want %d, len(%q)", n, short_body.len,
                           short_body);
    Str got = str_from_bytes(bytes_buffer_bytes(&buf).p, bytes_buffer_len(&buf));
    if (!str_eq(got, short_body))
        testing_t_errorf_v(t, "Read body %q; want %q", got, short_body);
    if (err.vt != io_err_unexpected_eof.vt || err.data != io_err_unexpected_eof.data)
        testing_t_errorf_v(t, "io.Copy error = %v; want io.ErrUnexpectedEOF", err);
    (void)body_close(res->body);
    http_response_free(res);
    bufio_reader_free(br);
    bytes_buffer_free(&buf);
    ARENA_END;
}

/* ------------------------------------------------------ TestReadResponseErrors */

/* What a case wants: success, an error value, or an error with a substring. */
typedef struct ErrWant {
    const char *sub; /* NULL with eof false for success */
    bool eof;        /* io.ErrUnexpectedEOF itself */
} ErrWant;

#define WANT_OK {NULL, false}
#define WANT_UNEXPECTED_EOF {NULL, true}
#define WANT(sub) {sub, false}

/* matchErr. Empty when err is what want says, and why not otherwise. */
static Str match_err(Alloc *a, Error err, ErrWant want) {
    bool want_nil = want.sub == NULL && !want.eof;
    if (BURROW_OK(err)) {
        if (want_nil)
            return (Str){0};
        if (want.sub != NULL)
            return fmt_sprintf_v(a, "unexpected success; want error with substring %q",
                                 cs(want.sub));
        return fmt_sprintf_v(a, "unexpected success; want error %v",
                             io_err_unexpected_eof);
    }
    if (want_nil)
        return fmt_sprintf_v(a, "%v; want success", err);
    if (want.sub != NULL) {
        if (strings_contains(error_text(err), cs(want.sub)))
            return (Str){0};
        return fmt_sprintf_v(a, "error = %v; want an error with substring %q", err,
                             cs(want.sub));
    }
    if (err.vt == io_err_unexpected_eof.vt && err.data == io_err_unexpected_eof.data)
        return (Str){0};
    return fmt_sprintf_v(a, "%v; want %v", err, io_err_unexpected_eof);
}

/* Test various ReadResponse error cases. (also tests success cases, but mostly
 * it's about errors).  This does not test anything involving the bodies. Only
 * the return value from ReadResponse itself. */
static void TestReadResponseErrors(TestingT *t) {
#define BAD_STATUS WANT("malformed HTTP status code")
#define BAD_VERSION WANT("malformed HTTP version")
#define ERR_MULTI_CL WANT("message cannot contain multiple Content-Length headers")
#define ERR_EMPTY_CL WANT("invalid empty Content-Length")
    /* kind is how Go builds in: 0 as it is, 1 status(s), 2 version(s) and 3
     * contentLength(status, body). */
    static const struct {
        const char *name; /* optional, defaults to in */
        const char *in;
        const char *body;
        ErrWant want;
        int kind;
    } tests[] = {
        {"", "", NULL, WANT_UNEXPECTED_EOF, 0},
        {"", "HTTP/1.1 301 Moved Permanently\r\nFoo: bar", NULL, WANT_UNEXPECTED_EOF,
         0},
        {"", "HTTP/1.1", NULL, WANT("malformed HTTP response"), 0},
        {"", "HTTP/2.0", NULL, WANT("malformed HTTP response"), 0},
        {NULL, "20X Unknown", NULL, BAD_STATUS, 1},
        {NULL, "abcd Unknown", NULL, BAD_STATUS, 1},
        {NULL, "二百/两百 OK", NULL, BAD_STATUS, 1},
        {NULL, " Unknown", NULL, BAD_STATUS, 1},
        {NULL, "c8 OK", NULL, BAD_STATUS, 1},
        {NULL, "0x12d Moved Permanently", NULL, BAD_STATUS, 1},
        {NULL, "200 OK", NULL, WANT_OK, 1},
        {NULL, "000 OK", NULL, WANT_OK, 1},
        {NULL, "001 OK", NULL, WANT_OK, 1},
        {NULL, "404 NOTFOUND", NULL, WANT_OK, 1},
        {NULL, "20 OK", NULL, BAD_STATUS, 1},
        {NULL, "00 OK", NULL, BAD_STATUS, 1},
        {NULL, "-10 OK", NULL, BAD_STATUS, 1},
        {NULL, "1000 OK", NULL, BAD_STATUS, 1},
        {NULL, "999 Done", NULL, WANT_OK, 1},
        {NULL, "-1 OK", NULL, BAD_STATUS, 1},
        {NULL, "-200 OK", NULL, BAD_STATUS, 1},
        {NULL, "HTTP/1.2", NULL, WANT_OK, 2},
        {NULL, "HTTP/2.0", NULL, WANT_OK, 2},
        {NULL, "HTTP/1.100000000002", NULL, BAD_VERSION, 2},
        {NULL, "HTTP/1.-1", NULL, BAD_VERSION, 2},
        {NULL, "HTTP/A.B", NULL, BAD_VERSION, 2},
        {NULL, "HTTP/1", NULL, BAD_VERSION, 2},
        {NULL, "http/1.1", NULL, BAD_VERSION, 2},

        {NULL, "200 OK",
         "Content-Length: 10\r\nContent-Length: 7\r\n\r\nGopher hey\r\n", ERR_MULTI_CL,
         3},
        {NULL, "200 OK", "Content-Length: 7\r\nContent-Length: 7\r\n\r\nGophers\r\n",
         WANT_OK, 3},
        {NULL, "201 OK", "Content-Length: 0\r\nContent-Length: 7\r\n\r\nGophers\r\n",
         ERR_MULTI_CL, 3},
        {NULL, "300 OK", "Content-Length: 0\r\nContent-Length: 0 \r\n\r\nGophers\r\n",
         WANT_OK, 3},
        {NULL, "200 OK", "Content-Length:\r\nContent-Length:\r\n\r\nGophers\r\n",
         ERR_EMPTY_CL, 3},
        {NULL, "206 OK",
         "Content-Length:\r\nContent-Length: 0 \r\nConnection: "
         "close\r\n\r\nGophers\r\n",
         ERR_MULTI_CL, 3},

        /* multiple content-length headers for 204 and 304 should still be
         * checked */
        {NULL, "204 OK", "Content-Length: 7\r\nContent-Length: 8\r\n\r\n", ERR_MULTI_CL,
         3},
        {NULL, "204 OK", "Content-Length: 3\r\nContent-Length: 3\r\n\r\n", WANT_OK, 3},
        {NULL, "304 OK", "Content-Length: 880\r\nContent-Length: 1\r\n\r\n",
         ERR_MULTI_CL, 3},
        {NULL, "304 OK", "Content-Length: 961\r\nContent-Length: 961\r\n\r\n", WANT_OK,
         3},

        /* golang.org/issue/22464 */
        {"leading space in header",
         "HTTP/1.1 200 OK\r\n Content-type: text/html\r\nFoo: bar\r\n\r\n", NULL,
         WANT("malformed MIME"), 0},
        {"leading tab in header",
         "HTTP/1.1 200 OK\r\n\tContent-type: text/html\r\nFoo: bar\r\n\r\n", NULL,
         WANT("malformed MIME"), 0},
    };
#undef BAD_STATUS
#undef BAD_VERSION
#undef ERR_MULTI_CL
#undef ERR_EMPTY_CL

    for (Int i = 0; i < (Int)(sizeof tests / sizeof tests[0]); i++) {
        ARENA_BEGIN;
        Str s = cs(tests[i].in);
        Str in = s;
        Str name = cs(tests[i].name);
        switch (tests[i].kind) {
        case 1:
            name = fmt_sprintf_v(a, "status %q", s);
            in = fmt_sprintf_v(a, "HTTP/1.1 %s\r\nFoo: bar\r\n\r\n", s);
            break;
        case 2:
            name = fmt_sprintf_v(a, "version %q", s);
            in = fmt_sprintf_v(a, "%s 200 OK\r\n\r\n", s);
            break;
        case 3:
            name = fmt_sprintf_v(a, "status %q %q", s, cs(tests[i].body));
            in = fmt_sprintf_v(a, "HTTP/1.1 %s\r\n%s", s, cs(tests[i].body));
            break;
        default:
            break;
        }
        StringsReader sr;
        BufioReader *br = reader_of(a, &sr, in);
        Error rerr;
        HttpResponse *res = http_read_response(a, br, NULL, &rerr);
        Str why = match_err(a, rerr, tests[i].want);
        if (why.len > 0) {
            if (name.len == 0)
                name = fmt_sprintf_v(a, "%d. input %q", i, in);
            testing_t_errorf_v(t, "%s: %s", name, why);
        }
        http_response_free(res);
        bufio_reader_free(br);
        ARENA_END;
    }
}

/* ---------------------------------------------------- transfer_test.go's */

static void TestFinalChunkedBodyReadEOF(TestingT *t) {
    ARENA_BEGIN;
    StringsReader sr;
    BufioReader *br = reader_of(a, &sr,
                                S("HTTP/1.1 200 OK\r\n"
                                  "Transfer-Encoding: chunked\r\n"
                                  "\r\n"
                                  "0a\r\n"
                                  "Body here\n\r\n"
                                  "09\r\n"
                                  "continued\r\n"
                                  "0\r\n"
                                  "\r\n"));
    Error err;
    HttpResponse *res = http_read_response(a, br, NULL, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%v", err);
    Str want = S("Body here\ncontinued");
    Byte buf[19];
    IoReader body = io_read_closer_as_io_reader(res->body);
    Int n =
        body.vt->read(body.data, slice_from(buf, want.len, want.len, TYPE_BYTE), &err);
    if (n != want.len || err.vt != io_eof.vt || err.data != io_eof.data)
        testing_t_errorf_v(t, "Read = %d, %v; want %d, EOF", n, err, want.len);
    if (!str_eq(str_from_bytes(buf, want.len), want))
        testing_t_errorf_v(t, "buf = %q; want %q", str_from_bytes(buf, want.len), want);
    http_response_free(res);
    bufio_reader_free(br);
    ARENA_END;
}

#define TESTS(X)                                                                       \
    X(TestReadResponse)                                                                \
    X(TestWriteResponse)                                                               \
    X(TestResponseStatusStutter)                                                       \
    X(TestResponseWritesOnlySingleConnectionClose)                                     \
    X(TestReadResponseCloseInMiddle)                                                   \
    X(TestLocationResponse)                                                            \
    X(TestResponseContentLengthShortBody)                                              \
    X(TestReadResponseErrors)                                                          \
    X(TestFinalChunkedBodyReadEOF)

TESTING_MAIN(TESTS)
