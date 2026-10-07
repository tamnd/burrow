/* Derived from Go's src/net/http/request.go, the parts that read a request
 * from the wire, write one to it and look at one.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"
#include "http_routing.h"

#include "../xnet/httpguts.h"
#include "http_ascii.h"

#include "burrow/bufio.h"
#include "burrow/bytes.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/encoding/base64.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptrace.h"
#include "burrow/net/textproto.h"
#include "burrow/net/url.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"

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

/* ------------------------------------------------------------------ writing */

static const Str hq_text_missing_host =
    BURROW_S_INIT("http: Request.Write on Request with no Host or URL set");
static const Str hq_text_invalid_host = BURROW_S_INIT("http: invalid Host header");
static const Str hq_text_ctl_in_url =
    BURROW_S_INIT("net/http: can't write control character in Request.URL");
static const Str hq_text_nil_context = BURROW_S_INIT("net/http: nil Context");

/* The fields Request.write writes itself, from the request's other fields,
 * and so leaves out of the header. */
static const Str hq_req_write_exclude[] = {
    BURROW_S_INIT("Host"),           BURROW_S_INIT("User-Agent"),
    BURROW_S_INIT("Content-Length"), BURROW_S_INIT("Transfer-Encoding"),
    BURROW_S_INIT("Trailer"),
};

/* requestBodyReadError, which holds the error it wraps after the vtable. */
static Str hq_body_read_error_message(const void *self) {
    return error_text(*(const Error *)self);
}

static Error hq_body_read_error_clone(const void *self, Alloc *a);

static const ErrorVT hq_body_read_error_vt = {
    NULL, hq_body_read_error_message, NULL, NULL, NULL, NULL, hq_body_read_error_clone,
};

static Error hq_body_read_error_in(Alloc *a, Error inner) {
    Error *box = (Error *)mem_alloc(a, sizeof *box, _Alignof(Error));
    if (box == NULL)
        return burrow_err_out_of_memory;
    *box = error_retain(a, inner);
    return (Error){&hq_body_read_error_vt, box};
}

static Error hq_body_read_error_clone(const void *self, Alloc *a) {
    return hq_body_read_error_in(a, *(const Error *)self);
}

Error burrow__http_request_body_read_error(Error inner) {
    return hq_body_read_error_in(error_allocator(), inner);
}

bool burrow__http_is_request_body_read_error(Error err, Error *inner) {
    if (err.vt != &hq_body_read_error_vt)
        return false;
    if (inner != NULL)
        *inner = *(const Error *)err.data;
    return true;
}

/* removeZone. "[fe80::1%en0]:8080" without its zone, "[fe80::1]:8080". */
static Str hq_remove_zone(Alloc *a, Str host) {
    if (host.len == 0 || host.p[0] != '[')
        return host;
    Int i = host.len - 1;
    while (i >= 0 && host.p[i] != ']')
        i--;
    if (i < 0)
        return host;
    Int j = i - 1;
    while (j >= 0 && host.p[j] != '%')
        j--;
    if (j < 0)
        return host;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(j + host.len - i), 1);
    if (p == NULL)
        return host;
    memcpy(p, host.p, (size_t)j);
    memcpy(p + j, host.p + i, (size_t)(host.len - i));
    return str_from_bytes(p, j + host.len - i);
}

/* Whether w can take a byte at a time, Go's io.ByteWriter, so that writing to
 * it costs nothing without a bufio.Writer in front. */
static bool hq_is_byte_writer(IoWriter w) {
    if (w.vt == NULL || w.vt->self_type == NULL)
        return false;
    const Type *t = w.vt->self_type;
    if (t == TYPE_BUFIO_WRITER || t == TYPE_STRINGS_BUILDER || t == TYPE_BYTES_BUFFER)
        return true;
    return type_method_by_name(t, BURROW_S("WriteByte")) != NULL;
}

/* headerNewlineToSpace and then textproto.TrimString. */
static Str hq_header_value(Alloc *a, Str v) {
    Int lo = 0, hi = v.len;
    while (lo < hi &&
           (v.p[lo] == ' ' || v.p[lo] == '\t' || v.p[lo] == '\n' || v.p[lo] == '\r'))
        lo++;
    while (hi > lo && (v.p[hi - 1] == ' ' || v.p[hi - 1] == '\t' ||
                       v.p[hi - 1] == '\n' || v.p[hi - 1] == '\r'))
        hi--;
    v = str_from_bytes(v.p + lo, hi - lo);
    if (v.len == 0 || (memchr(v.p, '\n', (size_t)v.len) == NULL &&
                       memchr(v.p, '\r', (size_t)v.len) == NULL))
        return v;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)v.len, 1);
    if (p == NULL)
        return v;
    for (Int i = 0; i < v.len; i++)
        p[i] = v.p[i] == '\n' || v.p[i] == '\r' ? ' ' : v.p[i];
    return str_from_bytes(p, v.len);
}

static Error hq_close_body(HttpRequest *r) {
    if (r->body.vt == NULL)
        return BURROW_NO_ERROR;
    return r->body.vt->closer.close(r->body.data);
}

/* The rest of Request.write, once the scratch arena is there. *closed says
 * whether the body has been closed, or handed to the code that closes it. */
static Error hq_write(HttpRequest *r, IoWriter w, bool using_proxy, HttpHeader extra,
                      burrow__HttpWaitFunc wait, const HttptraceClientTrace *trace,
                      Alloc *sa, burrow__HttpTransferWriter *tw, BufioWriter **bwp,
                      bool *closed) {
    /* The host from the Host field when there is one, and from the URL when
     * not, cleaned in case it has something unexpected in it. */
    Str host = r->host;
    if (host.len == 0) {
        if (r->url == NULL)
            return hq_error(&hq_text_missing_host);
        host = r->url->host;
    }
    Error err = BURROW_NO_ERROR;
    host = burrow__httpguts_punycode_host_port(sa, host, &err);
    if (BURROW_FAILED(err))
        return err;
    /* A Host that is not valid in a header is sent as "", not altered, since
     * an altered one opens a way to smuggle a request. A proxy can do nothing
     * with an empty one, so that is an error. */
    if (!burrow__httpguts_valid_host_header(host)) {
        if (using_proxy)
            return hq_error(&hq_text_invalid_host);
        host = BURROW_STR_EMPTY;
    }
    /* RFC 6874 has a client take the IPv6 zone off an outgoing URI. */
    host = hq_remove_zone(sa, host);

    if (r->url == NULL)
        runtime_panic(BURROW_S("http: Request.Write on Request with a nil URL"));
    const Url *u = r->url;
    Str ruri = url_request_uri(u, sa);
    if (using_proxy && u->scheme.len > 0 && u->opaque.len == 0) {
        ruri = fmt_sprintf_v(sa, "%s://%s%s", u->scheme, host, ruri);
    } else if (str_eq(r->method, BURROW_S("CONNECT")) && u->path.len == 0) {
        /* CONNECT gives just the host and port, not a whole URL. */
        ruri = host;
        if (u->opaque.len > 0)
            ruri = u->opaque;
    }
    if (burrow__http_string_contains_ctl_byte(ruri))
        return hq_error(&hq_text_ctl_in_url);

    /* A writer that is not buffered gets a BufioWriter in front of it. */
    if (!hq_is_byte_writer(w)) {
        *bwp = bufio_new_writer(sa, w);
        if (*bwp == NULL)
            return burrow_err_out_of_memory;
        w = bufio_writer_as_io_writer(*bwp);
    }

    Str method = r->method.len > 0 ? r->method : BURROW_S("GET");
    (void)io_write_string(w, fmt_sprintf_v(sa, "%s %s HTTP/1.1\r\n", method, ruri),
                          &err);
    if (BURROW_FAILED(err))
        return err;

    (void)io_write_string(w, fmt_sprintf_v(sa, "Host: %s\r\n", host), &err);
    if (BURROW_FAILED(err))
        return err;
    bool trace_fields = BURROW__HTTPTRACE_HAS(trace, wrote_header_field);
    if (trace_fields)
        httptrace_client_trace_wrote_header_field(trace, BURROW_S("Host"),
                                                  slice_from(&host, 1, 1, TYPE_STRING));

    /* The default User-Agent unless the header has one, which may be empty so
     * that none is sent. */
    Str user_agent = BURROW_S("Go-http-client/1.1");
    if (burrow__http_header_has(r->header, BURROW_S("User-Agent")))
        user_agent = http_header_get(r->header, BURROW_S("User-Agent"));
    if (user_agent.len > 0) {
        user_agent = hq_header_value(sa, user_agent);
        (void)io_write_string(w, fmt_sprintf_v(sa, "User-Agent: %s\r\n", user_agent),
                              &err);
        if (BURROW_FAILED(err))
            return err;
        if (trace_fields)
            httptrace_client_trace_wrote_header_field(
                trace, BURROW_S("User-Agent"),
                slice_from(&user_agent, 1, 1, TYPE_STRING));
    }

    /* The body, content_length, close and trailer. */
    err = burrow__http_new_transfer_writer(tw, sa, r, NULL);
    if (BURROW_FAILED(err))
        return err;
    err = burrow__http_transfer_writer_write_header(tw, sa, w, trace);
    if (BURROW_FAILED(err))
        return err;

    err = burrow__http_header_write_except(
        r->header, w, hq_req_write_exclude,
        (Int)(sizeof hq_req_write_exclude / sizeof hq_req_write_exclude[0]), trace);
    if (BURROW_FAILED(err))
        return err;

    if (extra != NULL) {
        err = burrow__http_header_write_except(extra, w, NULL, 0, trace);
        if (BURROW_FAILED(err))
            return err;
    }

    (void)io_write_string(w, BURROW_S("\r\n"), &err);
    if (BURROW_FAILED(err))
        return err;
    httptrace_client_trace_wrote_headers(trace);

    /* Flush, and wait for 100-continue if it is expected. */
    BufioWriter *wbuf = burrow__http_bufio_writer_of(w);
    if (wait.f != NULL) {
        if (wbuf != NULL) {
            err = bufio_writer_flush(wbuf);
            if (BURROW_FAILED(err))
                return err;
        }
        httptrace_client_trace_wait100_continue(trace);
        if (!wait.f(wait.env)) {
            *closed = true;
            (void)hq_close_body(r);
            return BURROW_NO_ERROR;
        }
    }

    if (wbuf != NULL && tw->flush_headers) {
        err = bufio_writer_flush(wbuf);
        if (BURROW_FAILED(err))
            return err;
    }

    /* The body and the trailer. */
    *closed = true;
    err = burrow__http_transfer_writer_write_body(tw, sa, w);
    if (BURROW_FAILED(err)) {
        if (tw->body_read_error.vt == err.vt && tw->body_read_error.data == err.data)
            err = burrow__http_request_body_read_error(err);
        return err;
    }

    if (*bwp != NULL)
        return bufio_writer_flush(*bwp);
    return BURROW_NO_ERROR;
}

Error burrow__http_request_write(HttpRequest *r, IoWriter w, bool using_proxy,
                                 HttpHeader extra, burrow__HttpWaitFunc wait) {
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    burrow__HttpTransferWriter tw;
    memset(&tw, 0, sizeof tw);
    BufioWriter *bw = NULL;
    bool closed = false;
    const HttptraceClientTrace *trace =
        httptrace_context_client_trace(http_request_context(r));
    Error err = hq_write(r, w, using_proxy, extra, wait, trace,
                         arena_allocator(&scratch), &tw, &bw, &closed);
    if (!closed) {
        Error cerr = hq_close_body(r);
        if (BURROW_FAILED(cerr) && BURROW_OK(err))
            err = cerr;
    }
    httptrace_client_trace_wrote_request(trace,
                                         (HttptraceWroteRequestInfo){.err = err});
    burrow__http_transfer_writer_done(&tw);
    /* An error made in the scratch arena has to outlive it. */
    if (BURROW_FAILED(err))
        err = error_retain(error_allocator(), err);
    bufio_writer_free(bw);
    arena_free(&scratch);
    return err;
}

Error http_request_write(HttpRequest *r, IoWriter w) {
    return burrow__http_request_write(r, w, false, NULL, (burrow__HttpWaitFunc){0});
}

Error http_request_write_proxy(HttpRequest *r, IoWriter w) {
    return burrow__http_request_write(r, w, true, NULL, (burrow__HttpWaitFunc){0});
}

/* --------------------------------------------------------------- NewRequest */

/* What GetBody for a body NewRequest knows copies: the reader as it was when
 * the request was made, and the arena of the request to put the copies in. */
typedef struct hq_BodySnapshot {
    Arena *arena;
    BytesReader bytes;
    StringsReader str;
    bool is_str;
} hq_BodySnapshot;

/* GetBody for such a body: a fresh reader of the same bytes. */
static IoReadCloser hq_get_body_snapshot(void *env, Error *err) {
    const hq_BodySnapshot *snap = (const hq_BodySnapshot *)env;
    Alloc *a = arena_allocator(snap->arena);
    IoNopCloser *nc = (IoNopCloser *)mem_alloc(a, sizeof *nc, _Alignof(IoNopCloser));
    if (nc == NULL) {
        *err = burrow_err_out_of_memory;
        return (IoReadCloser){0};
    }
    if (snap->is_str) {
        StringsReader *sr =
            (StringsReader *)mem_alloc(a, sizeof *sr, _Alignof(StringsReader));
        if (sr == NULL) {
            *err = burrow_err_out_of_memory;
            return (IoReadCloser){0};
        }
        *sr = snap->str;
        *nc = io_nop_closer(strings_reader_as_io_reader(sr));
    } else {
        BytesReader *br =
            (BytesReader *)mem_alloc(a, sizeof *br, _Alignof(BytesReader));
        if (br == NULL) {
            *err = burrow_err_out_of_memory;
            return (IoReadCloser){0};
        }
        *br = snap->bytes;
        *nc = io_nop_closer(bytes_reader_as_io_reader(br));
    }
    *err = BURROW_NO_ERROR;
    return io_nop_closer_as_io_read_closer(nc);
}

static IoReadCloser hq_get_no_body(void *env, Error *err) {
    (void)env;
    *err = BURROW_NO_ERROR;
    return http_no_body;
}

HttpRequest *http_new_request(Alloc *a, Str method, Str url, IoReader body,
                              Error *err) {
    return http_new_request_with_context(a, context_background(), method, url, body,
                                         err);
}

HttpRequest *http_new_request_with_context(Alloc *a, Context ctx, Str method, Str url,
                                           IoReader body, Error *err) {
    /* "" is documented to mean GET, and people rely on that. */
    if (method.len == 0)
        method = BURROW_S("GET");
    if (!hq_valid_method(method)) {
        *err = fmt_errorf_v("net/http: invalid method %q", method);
        return NULL;
    }
    if (ctx.vt == NULL) {
        *err = hq_error(&hq_text_nil_context);
        return NULL;
    }
    HttpRequest *req = (HttpRequest *)mem_alloc(a, sizeof *req, _Alignof(HttpRequest));
    if (req == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    req->a = a;
    arena_init(&req->arena, a, 0);
    Alloc *ra = arena_allocator(&req->arena);

    Error e;
    Url *u = url_parse(ra, url, &e);
    if (u == NULL) {
        *err = error_retain(error_allocator(), e);
        http_request_free(req);
        return NULL;
    }
    /* The host's colon and port are normalized, issue 14836. */
    if (u->host.len > 0 && u->host.p[u->host.len - 1] == ':')
        u->host.len--;
    req->ctx = ctx;
    req->method = str_clone(ra, method);
    req->url = u;
    req->proto = BURROW_S("HTTP/1.1");
    req->proto_major = 1;
    req->proto_minor = 1;
    req->header = http_header_make(ra);
    req->host = u->host;
    req->transfer_encoding = slice_from(NULL, 0, 0, TYPE_STRING);
    if (req->header == NULL || (method.len > 0 && req->method.len == 0)) {
        *err = burrow_err_out_of_memory;
        http_request_free(req);
        return NULL;
    }

    if (body.vt != NULL) {
        IoNopCloser *nc =
            (IoNopCloser *)mem_alloc(ra, sizeof *nc, _Alignof(IoNopCloser));
        if (nc == NULL) {
            *err = burrow_err_out_of_memory;
            http_request_free(req);
            return NULL;
        }
        *nc = io_nop_closer(body);
        req->body = io_nop_closer_as_io_read_closer(nc);

        const Type *t = body.vt->self_type;
        hq_BodySnapshot snap = {.arena = &req->arena};
        bool known = true;
        if (t != NULL && t == TYPE_BYTES_BUFFER) {
            BytesBuffer *b = (BytesBuffer *)body.data;
            req->content_length = bytes_buffer_len(b);
            bytes_reader_reset(&snap.bytes, bytes_buffer_bytes(b));
        } else if (t != NULL && t == TYPE_BYTES_READER) {
            BytesReader *b = (BytesReader *)body.data;
            req->content_length = bytes_reader_len(b);
            snap.bytes = *b;
        } else if (t != NULL && t == TYPE_STRINGS_READER) {
            StringsReader *s = (StringsReader *)body.data;
            req->content_length = strings_reader_len(s);
            snap.str = *s;
            snap.is_str = true;
        } else {
            known = false;
        }
        if (known) {
            hq_BodySnapshot *sp =
                (hq_BodySnapshot *)mem_alloc(ra, sizeof *sp, _Alignof(hq_BodySnapshot));
            if (sp == NULL) {
                *err = burrow_err_out_of_memory;
                http_request_free(req);
                return NULL;
            }
            *sp = snap;
            req->get_body = BURROW_FN(HttpGetBodyFunc, hq_get_body_snapshot, sp);
        }
        /* For a client request a content_length of 0 means either none or not
         * known, and the only way to say none is a nil body. Too much code
         * wants a body that is not nil, so http_no_body says it instead. */
        if (req->get_body.f != NULL && req->content_length == 0) {
            req->body = http_no_body;
            req->get_body = BURROW_FN(HttpGetBodyFunc, hq_get_no_body, NULL);
        }
    }
    *err = BURROW_NO_ERROR;
    return req;
}

Context http_request_context(const HttpRequest *r) {
    if (r->ctx.vt != NULL)
        return r->ctx;
    return context_background();
}

HttpRequest *http_request_with_context(const HttpRequest *r, Alloc *a, Context ctx) {
    if (ctx.vt == NULL)
        runtime_panic(BURROW_S("nil context"));
    HttpRequest *r2 = (HttpRequest *)mem_alloc(a, sizeof *r2, _Alignof(HttpRequest));
    if (r2 == NULL)
        return NULL;
    *r2 = *r;
    r2->ctx = ctx;
    /* The copy owns nothing of r's. */
    r2->a = a;
    arena_init(&r2->arena, a, 0);
    r2->wire = NULL;
    return r2;
}

/* --------------------------------------------------------------- path values */

/* patIndex. Where name is among the pattern's named wildcards, or -1. A
 * search beats a map here, since most patterns have a wildcard or two. */
static Int hq_pat_index(const HttpRequest *r, Str name) {
    if (r->pat == NULL)
        return -1;
    Int i = 0;
    for (Int k = 0; k < r->pat->nsegments; k++) {
        const burrow__HttpSegment *seg = &r->pat->segments[k];
        if (seg->wild && seg->s.len > 0) {
            if (str_eq(name, seg->s))
                return i;
            i++;
        }
    }
    return -1;
}

Str http_request_path_value(const HttpRequest *r, Str name) {
    Int i = hq_pat_index(r, name);
    if (i >= 0)
        return i < r->matches.len ? ((const Str *)r->matches.p)[i] : (Str){0};
    if (r->other_values == NULL)
        return (Str){0};
    const Str *v = (const Str *)map_get(r->other_values, &name);
    return v != NULL ? *v : (Str){0};
}

bool http_request_set_path_value(HttpRequest *r, Alloc *a, Str name, Str value) {
    Int i = hq_pat_index(r, name);
    if (i >= 0 && i < r->matches.len) {
        ((Str *)r->matches.p)[i] = value;
        return true;
    }
    if (r->other_values == NULL) {
        r->other_values = map_make(a, TYPE_STRING, TYPE_STRING, 0);
        if (r->other_values == NULL)
            return false;
    }
    return map_set(r->other_values, &name, &value);
}

/* ---------------------------------------------------- for the transport */

bool burrow__http_valid_method(Str m) {
    return hq_valid_method(m);
}

bool burrow__http_is_err_missing_host(Error e) {
    return e.vt == &burrow_sentinel_error_vt && e.data == &hq_text_missing_host;
}
