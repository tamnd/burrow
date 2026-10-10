/* Derived from Go's src/net/http/request.go, the parts that read a request
 * from the wire and look at one.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"

#include "http_ascii.h"

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/encoding/base64.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net/http.h"
#include "burrow/net/textproto.h"
#include "burrow/net/url.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

BURROW_SENTINEL_ERROR(burrow__http_err_too_large, "http: request too large");

static const Str hq_text_too_many_hosts = BURROW_S_INIT("too many Host headers");

static Error hq_error(const Str *text) {
    return (Error){&burrow_sentinel_error_vt, text};
}

static bool hq_has_prefix(Str s, Str prefix) {
    return s.len >= prefix.len && memcmp(s.p, prefix.p, (size_t)prefix.len) == 0;
}

bool http_parse_http_version(Str vers, Int *major, Int *minor) {
    *major = 0;
    *minor = 0;
    if (str_eq(vers, BURROW_S("HTTP/1.1"))) {
        *major = 1;
        *minor = 1;
        return true;
    }
    if (str_eq(vers, BURROW_S("HTTP/1.0"))) {
        *major = 1;
        return true;
    }
    if (!hq_has_prefix(vers, BURROW_S("HTTP/")))
        return false;
    if (vers.len != 8)
        return false;
    const Byte *p = vers.p;
    if (p[6] != '.')
        return false;
    /* strconv.ParseUint of one byte, which is a digit or an error. */
    if (p[5] < '0' || p[5] > '9' || p[7] < '0' || p[7] > '9')
        return false;
    *major = p[5] - '0';
    *minor = p[7] - '0';
    return true;
}

bool http_request_proto_at_least(const HttpRequest *r, Int major, Int minor) {
    return r->proto_major > major ||
           (r->proto_major == major && r->proto_minor >= minor);
}

Str http_request_user_agent(const HttpRequest *r) {
    return http_header_get(r->header, BURROW_S("User-Agent"));
}

Str http_request_referer(const HttpRequest *r) {
    return http_header_get(r->header, BURROW_S("Referer"));
}

bool burrow__http_parse_basic_auth(Alloc *a, Str auth, Str *username, Str *password) {
    Str prefix = BURROW_S("Basic ");
    *username = (Str){0};
    *password = (Str){0};
    /* The prefix in any case, issue 22736. */
    if (auth.len < prefix.len ||
        !burrow__http_ascii_equal_fold(str_from_bytes(auth.p, prefix.len), prefix))
        return false;
    Error err;
    Slice c = base64_encoding_decode_string(
        base64_std_encoding, a,
        str_from_bytes(auth.p + prefix.len, auth.len - prefix.len), &err);
    if (BURROW_FAILED(err))
        return false;
    const Byte *p = c.p;
    const Byte *colon = c.len > 0 ? (const Byte *)memchr(p, ':', (size_t)c.len) : NULL;
    if (colon == NULL)
        return false;
    Int i = (Int)(colon - p);
    *username = str_from_bytes(p, i);
    *password = str_from_bytes(colon + 1, c.len - i - 1);
    return true;
}

bool http_request_basic_auth(const HttpRequest *r, Alloc *a, Str *username,
                             Str *password) {
    Str auth = http_header_get(r->header, BURROW_S("Authorization"));
    if (auth.len == 0) {
        *username = (Str){0};
        *password = (Str){0};
        return false;
    }
    return burrow__http_parse_basic_auth(a, auth, username, password);
}

bool http_request_set_basic_auth(HttpRequest *r, Alloc *a, Str username, Str password) {
    if (r->header == NULL)
        return false;
    /* "Basic " and the base64 of username + ":" + password. */
    Int n = username.len + 1 + password.len;
    Byte *plain = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (plain == NULL)
        return false;
    if (username.len > 0)
        memcpy(plain, username.p, (size_t)username.len);
    plain[username.len] = ':';
    if (password.len > 0)
        memcpy(plain + username.len + 1, password.p, (size_t)password.len);
    Int elen = base64_encoding_encoded_len(base64_std_encoding, n);
    Byte *v = (Byte *)mem_alloc_nozero(a, (size_t)elen + 6, 1);
    if (v == NULL) {
        mem_free(a, plain, (size_t)n, 1);
        return false;
    }
    memcpy(v, "Basic ", 6);
    base64_encoding_encode(base64_std_encoding,
                           slice_from(v + 6, elen, elen, TYPE_BYTE),
                           slice_from(plain, n, n, TYPE_BYTE));
    mem_free(a, plain, (size_t)n, 1);
    return http_header_set(r->header, BURROW_S("Authorization"),
                           str_from_bytes(v, elen + 6));
}

/* parseRequestLine. "GET /foo HTTP/1.1" in its three parts. */
static bool hq_parse_request_line(Str line, Str *method, Str *request_uri, Str *proto) {
    const Byte *p = line.p;
    const Byte *sp1 =
        line.len > 0 ? (const Byte *)memchr(p, ' ', (size_t)line.len) : NULL;
    if (sp1 == NULL)
        return false;
    const Byte *rest = sp1 + 1;
    Int rest_len = line.len - (Int)(rest - p);
    const Byte *sp2 =
        rest_len > 0 ? (const Byte *)memchr(rest, ' ', (size_t)rest_len) : NULL;
    if (sp2 == NULL)
        return false;
    *method = str_from_bytes(p, (Int)(sp1 - p));
    *request_uri = str_from_bytes(rest, (Int)(sp2 - rest));
    *proto = str_from_bytes(sp2 + 1, rest_len - (Int)(sp2 - rest) - 1);
    return true;
}

/* validMethod. A method is a token. */
static bool hq_valid_method(Str method) {
    return method.len > 0 && burrow__http_is_token(method);
}

bool burrow__http_fix_pragma_cache_control(HttpHeader header) {
    Str pragma = BURROW_S("Pragma");
    Str cc = BURROW_S("Cache-Control");
    Slice *hp = header != NULL ? (Slice *)map_get(header, &pragma) : NULL;
    if (hp != NULL && hp->len > 0 &&
        str_eq(((const Str *)hp->p)[0], BURROW_S("no-cache")) &&
        map_get(header, &cc) == NULL)
        return http_header_set(header, cc, BURROW_S("no-cache"));
    return true;
}

/* isH2Upgrade. Whether r is the "PRI * HTTP/2.0" start of HTTP/2's client
 * preface. */
static bool hq_is_h2_upgrade(const HttpRequest *r) {
    return str_eq(r->method, BURROW_S("PRI")) && map_len(r->header) == 0 &&
           str_eq(r->url->path, BURROW_S("*")) &&
           str_eq(r->proto, BURROW_S("HTTP/2.0"));
}

/* readRequestLimit after the first line, which is s. */
static Error hq_read_rest(HttpRequest *req, TextprotoReader *tp, BufioReader *b, Str s,
                          int64_t max_headers) {
    Alloc *a = arena_allocator(&req->arena);
    if (!hq_parse_request_line(s, &req->method, &req->request_uri, &req->proto))
        return burrow__http_bad_string_error(BURROW_S("malformed HTTP request"), s);
    if (!hq_valid_method(req->method))
        return burrow__http_bad_string_error(BURROW_S("invalid method"), req->method);
    Str rawurl = req->request_uri;
    if (!http_parse_http_version(req->proto, &req->proto_major, &req->proto_minor))
        return burrow__http_bad_string_error(BURROW_S("malformed HTTP version"),
                                             req->proto);

    /* CONNECT is used two ways, and neither has a full URL. The usual one
     * tunnels HTTPS through a proxy, as "CONNECT www.google.com:443 HTTP/1.1",
     * where the target is the authority part of a URL and goes in url->host.
     * net/rpc's has a path starting with a slash, which the URL parser reads
     * as it is, into url->path where RPC wants it. */
    bool just_authority = str_eq(req->method, BURROW_S("CONNECT")) &&
                          !hq_has_prefix(rawurl, BURROW_S("/"));
    if (just_authority) {
        Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)rawurl.len + 7, 1);
        if (p == NULL)
            return burrow_err_out_of_memory;
        memcpy(p, "http://", 7);
        if (rawurl.len > 0)
            memcpy(p + 7, rawurl.p, (size_t)rawurl.len);
        rawurl = str_from_bytes(p, rawurl.len + 7);
    }

    Error err;
    req->url = url_parse_request_uri(a, rawurl, &err);
    if (BURROW_FAILED(err))
        return err;
    if (just_authority) {
        /* Take the made up "http://" back off. */
        req->url->scheme = (Str){0};
    }

    /* The header lines. */
    HttpHeader h =
        burrow__textproto_read_mime_header(tp, a, INT64_MAX, max_headers, &err);
    if (BURROW_FAILED(err)) {
        if (str_eq(error_text(err), BURROW_S("message too large")))
            return burrow__http_err_too_large;
        return err;
    }
    req->header = h;
    Str host_key = BURROW_S("Host");
    Slice *hosts = (Slice *)map_get(h, &host_key);
    if (hosts != NULL && hosts->len > 1)
        return hq_error(&hq_text_too_many_hosts);

    /* RFC 7230 section 5.3: "GET /index.html" with "Host: www.google.com" is
     * the same as "GET http://www.google.com/index.html", where any Host line
     * is ignored. */
    req->host = req->url->host;
    if (req->host.len == 0)
        req->host = burrow__http_header_get(h, host_key);

    if (!burrow__http_fix_pragma_cache_control(h))
        return burrow_err_out_of_memory;

    req->close =
        burrow__http_should_close(req->proto_major, req->proto_minor, h, false);

    err = burrow__http_read_transfer(req, NULL, b, max_headers);
    if (BURROW_FAILED(err))
        return err;

    if (hq_is_h2_upgrade(req)) {
        /* Neither chunked nor declared. */
        req->content_length = -1;
        /* A handler may take the connection over, and if it does not, the
         * server is not to go on with it. */
        req->close = true;
    }
    return BURROW_NO_ERROR;
}

static HttpRequest *hq_fail(HttpRequest *req, Error e, Error *err) {
    /* The error may be in the request's arena, which is about to go. */
    *err = error_retain(error_allocator(), e);
    http_request_free(req);
    return NULL;
}

HttpRequest *burrow__http_read_request_limit(Alloc *a, BufioReader *b,
                                             int64_t max_headers, Error *err) {
    HttpRequest *req = (HttpRequest *)mem_alloc(a, sizeof *req, _Alignof(HttpRequest));
    if (req == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    req->a = a;
    arena_init(&req->arena, a, 0);
    req->body = http_no_body;

    TextprotoReader tp = {0};
    tp.r = b;

    /* The first line, such as "GET /index.html HTTP/1.0". */
    Error e;
    Str s = textproto_reader_read_line(&tp, arena_allocator(&req->arena), &e);
    if (BURROW_FAILED(e)) {
        textproto_reader_free(&tp);
        return hq_fail(req, e, err);
    }
    e = hq_read_rest(req, &tp, b, s, max_headers);
    textproto_reader_free(&tp);
    if (BURROW_FAILED(e)) {
        if (e.vt == io_eof.vt && e.data == io_eof.data)
            e = io_err_unexpected_eof;
        return hq_fail(req, e, err);
    }
    *err = BURROW_NO_ERROR;
    return req;
}

HttpRequest *http_read_request(Alloc *a, BufioReader *b, Error *err) {
    HttpRequest *req = burrow__http_read_request_limit(a, b, INT64_MAX, err);
    if (req == NULL)
        return NULL;
    Str host_key = BURROW_S("Host");
    map_del(req->header, &host_key);
    return req;
}

void http_request_free(HttpRequest *r) {
    if (r == NULL)
        return;
    burrow__http_body_free(r->wire);
    arena_free(&r->arena);
    mem_free(r->a, r, sizeof *r, _Alignof(HttpRequest));
}
