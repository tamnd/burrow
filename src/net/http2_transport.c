/* net/http/internal/http2's transport.go and client_conn_pool.go: the client
 * side of an HTTP/2 connection, and httpcommon's EncodeHeaders, which turns a
 * request into the header fields that go in its HEADERS frame.
 *
 * net/http's transport dials, and hands the connection over with add_conn
 * once it knows the connection speaks HTTP/2. From then on the connection is
 * in a pool keyed by its host and port, and a round trip takes the first one
 * there with room for another stream. This transport never dials, as with
 * net/http's: when no connection can take a request, the round trip gives
 * ErrNoCachedConn and net/http dials a new one.
 *
 * Go runs one goroutine per connection to read frames, and one per request to
 * write the request and wait for the end of the response, and both are here.
 * What Go leaves to the collector is counted. A connection is held by the
 * pool, its read loop, each of its streams, each timer that is armed on it
 * and each goroutine it starts, and goes when the last of them lets go. A
 * stream is held by the connection's map, its round trip, the goroutine that
 * writes it, and the response once the caller has it. The response does not
 * outlive the request: freeing it waits for the goroutine that writes the
 * request, and a round trip that fails returns only once that goroutine is
 * done, so the request can be freed straight after either.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http2.h"
#include "http_ascii.h"
#include "http_internal.h"

#include "../xnet/hpack.h"
#include "../xnet/httpguts.h"
#include "../xnet/idna.h"

#include "burrow/bufio.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/compress/gzip.h"
#include "burrow/context.h"
#include "burrow/crypto/rand.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/math/rand.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptrace.h"
#include "burrow/net/textproto.h"
#include "burrow/net/url.h"
#include "burrow/os.h"
#include "burrow/proc.h"
#include "burrow/sort.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------- constants */

enum {
    /* transportDefaultConnFlow: how many connection-level flow control
     * tokens the transport gives the server at start-up, past the default
     * 64k. */
    H2C_DEFAULT_CONN_FLOW = 1 << 30,
    /* transportDefaultStreamFlow: how many stream-level flow control tokens
     * the transport announces, and how many bytes it buffers per stream. */
    H2C_DEFAULT_STREAM_FLOW = 4 << 20,
    /* initialMaxConcurrentStreams: a connection's maxConcurrentStreams until
     * the server's first SETTINGS frame, the spec's lowest recommended
     * value. */
    H2C_INITIAL_MAX_CONCURRENT_STREAMS = 100,
    /* defaultMaxConcurrentStreams: the limit when the server's first
     * SETTINGS frame does not give one. */
    H2C_DEFAULT_MAX_CONCURRENT_STREAMS = 1000,
    /* initialWindowSize and the default MAX_FRAME_SIZE, from RFC 7540. */
    H2C_INITIAL_WINDOW_SIZE = 65535,
    H2C_SPEC_MAX_FRAME_SIZE = 16 << 10,
    /* defaultMaxReadFrameSize. */
    H2C_DEFAULT_MAX_READ_FRAME_SIZE = 1 << 20,
    /* bufio.NewWriter's and bufio.NewReader's size. */
    H2C_BUF_SIZE = 4096,
    /* The most frameScratchBufferLen gives. */
    H2C_MAX_SCRATCH = 512 << 10,
};

#define H2C_DEFAULT_USER_AGENT "Go-http-client/2.0"
#define H2C_DEFAULT_PING_TIMEOUT (15 * TIME_SECOND)

/* ---------------------------------------------------------------- errors */

BURROW_SENTINEL_ERROR(burrow__http2_err_client_conn_closed,
                      "http2: client conn is closed");
BURROW_SENTINEL_ERROR(burrow__http2_err_client_conn_unusable,
                      "http2: client conn not usable");
BURROW_SENTINEL_ERROR(burrow__http2_err_client_conn_not_established,
                      "http2: client conn could not be established");
BURROW_SENTINEL_ERROR(burrow__http2_err_client_conn_got_go_away,
                      "http2: Transport received Server's graceful shutdown GOAWAY");
BURROW_SENTINEL_ERROR(burrow__http2_err_client_conn_force_closed,
                      "http2: client connection force closed via ClientConn.Close");
BURROW_SENTINEL_ERROR(burrow__http2_err_extended_connect_not_supported,
                      "net/http: extended connect not supported by peer");
BURROW_SENTINEL_ERROR(burrow__http2_err_stop_req_body_write,
                      "http2: aborting request body write");
BURROW_SENTINEL_ERROR(burrow__http2_err_stop_req_body_write_and_cancel,
                      "http2: canceling request");
BURROW_SENTINEL_ERROR(burrow__http2_err_req_body_too_long,
                      "http2: request body larger than specified content length");
BURROW_SENTINEL_ERROR(burrow__http2_err_response_header_list_size,
                      "http2: response header list larger than advertised limit");
BURROW_SENTINEL_ERROR(burrow__http2_err_request_header_list_size,
                      "request header list larger than peer's advertised limit");
BURROW_SENTINEL_ERROR(burrow__http2_err_closed_response_body,
                      "http2: response body closed");
BURROW_SENTINEL_ERROR(burrow__http2_err_concurrent_read_on_res_body,
                      "http2: concurrent read on response body");
BURROW_SENTINEL_ERROR(burrow__http2_err_no_cached_conn,
                      "http2: no cached connection was available");

/* errStopReadLoop. */
BURROW_SENTINEL_ERROR(h2c_err_stop_read_loop,
                      "client connection is closing (BUG: this is not user visible)");

/* The errors.New ones that are only made in one place. */
BURROW_SENTINEL_ERROR(h2c_err_unsupported_scheme, "http2: unsupported scheme");
BURROW_SENTINEL_ERROR(h2c_err_nil_url, "Request.URL is nil");
BURROW_SENTINEL_ERROR(h2c_err_invalid_host, "invalid Host header");
BURROW_SENTINEL_ERROR(h2c_err_invalid_protocol,
                      "invalid :protocol header in non-CONNECT request");
BURROW_SENTINEL_ERROR(h2c_err_conn_lost, "http2: client connection lost");
BURROW_SENTINEL_ERROR(h2c_err_headers_after_end,
                      "protocol error: headers after END_STREAM");
BURROW_SENTINEL_ERROR(h2c_err_missing_status,
                      "malformed response from server: missing status pseudo header");
BURROW_SENTINEL_ERROR(
    h2c_err_bad_status,
    "malformed response from server: malformed non-numeric status pseudo header");
BURROW_SENTINEL_ERROR(h2c_err_1xx_end_stream,
                      "1xx informational response with END_STREAM flag");
BURROW_SENTINEL_ERROR(h2c_err_1xx_too_large, "header list too large");
BURROW_SENTINEL_ERROR(
    h2c_err_too_much_body,
    "net/http: server replied with more than declared Content-Length; truncated");

/* Whether a is b itself, Go's err == b. */
static bool h2c_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

bool burrow__http2_is_no_cached_conn_error(Error err) {
    return h2c_same(err, burrow__http2_err_no_cached_conn);
}

/* GoAwayError. */
typedef struct h2c_GoAwayError {
    Http2GoAwayError ge;
    Str message;
} h2c_GoAwayError;

static Str h2c_go_away_message(const void *self) {
    return ((const h2c_GoAwayError *)self)->message;
}

static bool h2c_go_away_is(const void *self, Error target);
static Error h2c_go_away_clone(const void *self, Alloc *a);

static const ErrorVT h2c_go_away_vt = {
    .message = h2c_go_away_message,
    .is = h2c_go_away_is,
    .clone = h2c_go_away_clone,
};

Error burrow__http2_go_away_error(Alloc *a, Http2GoAwayError ge) {
    Byte cb[HTTP2_STRING_MAX];
    Str code = burrow__http2_err_code_string(ge.err_code, cb);
    /* The message is made in its own arena, since a may be the error
     * arena, which error_release would take back. */
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Str msg = fmt_sprintf_v(arena_allocator(&ar),
                            "http2: server sent GOAWAY and closed the connection; "
                            "LastStreamID=%v, ErrCode=%s, debug=%q",
                            ge.last_stream_id, code, ge.debug_data);
    size_t n = (size_t)msg.len + (size_t)ge.debug_data.len;
    h2c_GoAwayError *e = (h2c_GoAwayError *)mem_alloc_nozero(
        a, sizeof(h2c_GoAwayError) + n, _Alignof(h2c_GoAwayError));
    if (e == NULL) {
        arena_free(&ar);
        return burrow_err_out_of_memory;
    }
    Byte *p = (Byte *)(e + 1);
    if (msg.len > 0)
        memcpy(p, msg.p, (size_t)msg.len);
    e->message = str_from_bytes(p, msg.len);
    p += msg.len;
    if (ge.debug_data.len > 0)
        memcpy(p, ge.debug_data.p, (size_t)ge.debug_data.len);
    e->ge = ge;
    e->ge.debug_data = str_from_bytes(p, ge.debug_data.len);
    arena_free(&ar);
    return (Error){&h2c_go_away_vt, e};
}

static bool h2c_go_away_is(const void *self, Error target) {
    if (target.vt != &h2c_go_away_vt)
        return false;
    const Http2GoAwayError *x = &((const h2c_GoAwayError *)self)->ge;
    const Http2GoAwayError *y = &((const h2c_GoAwayError *)target.data)->ge;
    return x->last_stream_id == y->last_stream_id && x->err_code == y->err_code &&
           str_eq(x->debug_data, y->debug_data);
}

static Error h2c_go_away_clone(const void *self, Alloc *a) {
    return burrow__http2_go_away_error(a, ((const h2c_GoAwayError *)self)->ge);
}

bool burrow__http2_error_go_away(Error err, Http2GoAwayError *ge) {
    if (err.vt != &h2c_go_away_vt)
        return false;
    if (ge != NULL)
        *ge = ((const h2c_GoAwayError *)err.data)->ge;
    return true;
}

/* ---------------------------------------------------------------- config */

/* Config, as configFromTransport leaves it, with the fields a client looks
 * at. */
typedef struct h2c_Config {
    bool strict_max_concurrent_requests;
    int64_t max_decoder_header_table_size;
    int64_t max_encoder_header_table_size;
    int64_t max_read_frame_size;
    int64_t max_receive_buffer_per_connection;
    int64_t max_receive_buffer_per_stream;
    Duration send_ping_timeout;
    Duration ping_timeout;
    Duration write_byte_timeout;
    HttpCountErrorFunc count_error;
} h2c_Config;

static void h2c_set_default(int64_t *v, int64_t minval, int64_t maxval,
                            int64_t defval) {
    if (*v < minval || *v > maxval)
        *v = defval;
}

/* configFromTransport: what t1's HTTP2 field sets, and the defaults for the
 * rest. */
static h2c_Config h2c_config_from_transport(const HttpTransport *t1) {
    h2c_Config conf;
    memset(&conf, 0, sizeof conf);
    const HttpHTTP2Config *h2 = t1 != NULL ? t1->http2 : NULL;
    if (h2 != NULL) {
        conf.strict_max_concurrent_requests = h2->strict_max_concurrent_requests;
        conf.max_encoder_header_table_size = (int64_t)h2->max_encoder_header_table_size;
        conf.max_decoder_header_table_size = (int64_t)h2->max_decoder_header_table_size;
        conf.max_read_frame_size = (int64_t)h2->max_read_frame_size;
        conf.max_receive_buffer_per_connection =
            (int64_t)h2->max_receive_buffer_per_connection;
        conf.max_receive_buffer_per_stream = (int64_t)h2->max_receive_buffer_per_stream;
        conf.send_ping_timeout = h2->send_ping_timeout;
        conf.ping_timeout = h2->ping_timeout;
        conf.write_byte_timeout = h2->write_byte_timeout;
        conf.count_error = h2->count_error;
    }
    h2c_set_default(&conf.max_encoder_header_table_size, 1, INT32_MAX,
                    HTTP2_INITIAL_HEADER_TABLE_SIZE);
    h2c_set_default(&conf.max_decoder_header_table_size, 1, INT32_MAX,
                    HTTP2_INITIAL_HEADER_TABLE_SIZE);
    h2c_set_default(&conf.max_receive_buffer_per_connection, H2C_INITIAL_WINDOW_SIZE,
                    INT32_MAX, H2C_DEFAULT_CONN_FLOW);
    h2c_set_default(&conf.max_receive_buffer_per_stream, 1, INT32_MAX,
                    H2C_DEFAULT_STREAM_FLOW);
    h2c_set_default(&conf.max_read_frame_size, HTTP2_MIN_MAX_FRAME_SIZE,
                    HTTP2_MAX_FRAME_SIZE, H2C_DEFAULT_MAX_READ_FRAME_SIZE);
    h2c_set_default(&conf.ping_timeout, 1, INT64_MAX, H2C_DEFAULT_PING_TIMEOUT);
    return conf;
}

/* Transport.maxHeaderListSize. net/http's MaxHeaderListSize is 0, since there
 * is no x/net/http2 Transport behind it, so this is MaxResponseHeaderBytes
 * with room for ten fields' overhead. */
static uint32_t h2c_max_header_list_size(const HttpTransport *t1) {
    int64_t n = 0;
    int64_t b = t1 != NULL ? t1->max_response_header_bytes : 0;
    if (b != 0) {
        n = b;
        if (n > 0) /* adjustHTTP1MaxHeaderSize, which wraps as Go's does */
            n = (int64_t)((uint64_t)n + (uint64_t)10 * 32);
    }
    if (n <= 0)
        return 10 << 20;
    if (n >= 0xffffffff)
        return 0;
    return (uint32_t)n;
}

/* ------------------------------------------------------ authorityAddr */

Str burrow__http2_authority_addr(Alloc *a, Str scheme, Str authority) {
    Str host, port;
    ArenaMark m = error_mark();
    Error err = BURROW_NO_ERROR;
    host = net_split_host_port(authority, &port, &err);
    if (BURROW_FAILED(err)) { /* authority didn't have a port */
        host = authority;
        port = BURROW_STR_EMPTY;
    }
    if (port.len == 0) { /* authority's port was empty */
        port = BURROW_S("443");
        if (str_eq(scheme, BURROW_S("http")))
            port = BURROW_S("80");
    }
    Error ierr = BURROW_NO_ERROR;
    Str ah = burrow__idna_to_ascii(error_allocator(), host, &ierr);
    if (BURROW_OK(ierr))
        host = ah;
    Str out;
    /* IPv6 address literal, without a port: */
    if (strings_has_prefix(host, BURROW_S("[")) &&
        strings_has_suffix(host, BURROW_S("]")))
        out = fmt_sprintf_v(a, "%s:%s", host, port);
    else
        out = net_join_host_port(a, host, port);
    error_release(m);
    return out;
}

/* ---------------------------------------------------------- EncodeHeaders */

/* `%q` of a []string, which is ["a" "b"]. */
static Str h2c_quote_values(Alloc *a, Slice vv) {
    BytesBuffer b = BYTES_BUFFER(a);
    (void)bytes_buffer_write_byte(&b, '[');
    for (Int i = 0; i < vv.len; i++) {
        if (i > 0)
            (void)bytes_buffer_write_byte(&b, ' ');
        Str q = strconv_quote(a, *(const Str *)slice_at(vv, i));
        (void)bytes_buffer_write_string(&b, q, NULL);
    }
    (void)bytes_buffer_write_byte(&b, ']');
    Slice s = bytes_buffer_bytes(&b);
    return str_from_bytes(s.p, s.len);
}

static Str h2c_value0(Slice vv) {
    return *(const Str *)slice_at(vv, 0);
}

/* checkConnHeaders. */
static Error h2c_check_conn_headers(Alloc *a, HttpHeader h) {
    Slice vv = http_header_values(h, BURROW_S("Upgrade"));
    if (vv.len > 0 &&
        (h2c_value0(vv).len != 0 && !str_eq(h2c_value0(vv), BURROW_S("chunked"))))
        return fmt_errorf_v("invalid Upgrade request header: %s",
                            h2c_quote_values(a, vv));
    vv = http_header_values(h, BURROW_S("Transfer-Encoding"));
    if (vv.len > 0 && (vv.len > 1 || (h2c_value0(vv).len != 0 &&
                                      !str_eq(h2c_value0(vv), BURROW_S("chunked")))))
        return fmt_errorf_v("invalid Transfer-Encoding request header: %s",
                            h2c_quote_values(a, vv));
    vv = http_header_values(h, BURROW_S("Connection"));
    if (vv.len > 0 &&
        (vv.len > 1 ||
         (h2c_value0(vv).len != 0 &&
          !burrow__http_ascii_equal_fold(h2c_value0(vv), BURROW_S("close")) &&
          !burrow__http_ascii_equal_fold(h2c_value0(vv), BURROW_S("keep-alive")))))
        return fmt_errorf_v("invalid Connection request header: %s",
                            h2c_quote_values(a, vv));
    return BURROW_NO_ERROR;
}

/* commaSeparatedTrailers. */
static Error h2c_comma_separated_trailers(Alloc *a, HttpHeader trailer, Str *out) {
    *out = BURROW_STR_EMPTY;
    Int n = trailer != NULL ? map_len(trailer) : 0;
    if (n == 0)
        return BURROW_NO_ERROR;
    Slice keys = slice_make(a, TYPE_STRING, 0, n);
    if (keys.p == NULL)
        return burrow_err_out_of_memory;
    MapIter it = map_iter(trailer);
    const void *kp;
    void *vp;
    while (map_next(&it, &kp, &vp)) {
        Str k = textproto_canonical_mime_header_key(a, *(const Str *)kp);
        if (str_eq(k, BURROW_S("Transfer-Encoding")) ||
            str_eq(k, BURROW_S("Trailer")) || str_eq(k, BURROW_S("Content-Length")))
            return fmt_errorf_v("invalid Trailer key %q", k);
        keys = slice_append(a, keys, &k, 1);
        if (keys.p == NULL)
            return burrow_err_out_of_memory;
    }
    sort_strings(keys);
    *out = strings_join(a, keys, BURROW_S(","));
    return BURROW_NO_ERROR;
}

/* validPseudoPath. */
static bool h2c_valid_pseudo_path(Str v) {
    return (v.len > 0 && v.p[0] == '/') || str_eq(v, BURROW_S("*"));
}

/* validateHeaders: what is wrong with hdrs, or "". */
static Str h2c_validate_headers(Alloc *a, HttpHeader hdrs) {
    if (hdrs == NULL)
        return BURROW_STR_EMPTY;
    MapIter it = map_iter(hdrs);
    const void *kp;
    void *vp;
    while (map_next(&it, &kp, &vp)) {
        Str k = *(const Str *)kp;
        if (!burrow__httpguts_valid_header_field_name(k) &&
            !str_eq(k, BURROW_S(":protocol")))
            return fmt_sprintf_v(a, "name %q", k);
        Slice vv = *(const Slice *)vp;
        for (Int i = 0; i < vv.len; i++) {
            if (!burrow__httpguts_valid_header_field_value(
                    *(const Str *)slice_at(vv, i)))
                return fmt_sprintf_v(a, "value for header %q", k);
        }
    }
    return BURROW_STR_EMPTY;
}

/* shouldSendReqContentLength. */
static bool h2c_should_send_req_content_length(Str method, int64_t content_length) {
    if (content_length > 0)
        return true;
    if (content_length < 0)
        return false;
    /* For zero bodies, whether we send a content-length header depends on
     * the method. It also kinda doesn't matter for http2 either way, with
     * END_STREAM. */
    return str_eq(method, BURROW_S("POST")) || str_eq(method, BURROW_S("PUT")) ||
           str_eq(method, BURROW_S("PATCH"));
}

/* isRequestGzip. */
static bool h2c_is_request_gzip(Str method, HttpHeader header,
                                bool disable_compression) {
    /* TODO(bradfitz): this is a copy of the logic in net/http. Unify
     * somewhere? */
    return !disable_compression &&
           http_header_values(header, BURROW_S("Accept-Encoding")).len == 0 &&
           http_header_values(header, BURROW_S("Range")).len == 0 &&
           !str_eq(method, BURROW_S("HEAD"));
}

/* What enumerateHeaders works from. */
typedef struct h2c_Enum {
    const Http2EncodeHeadersParam *p;
    Str host;
    Str path;
    Str protocol;
    Str trailers;
    bool is_normal_connect;
    Alloc *a;
} h2c_Enum;

/* enumerateHeaders. */
static void h2c_enumerate_headers(const h2c_Enum *en, Http2HeaderFunc f, void *env) {
    const Http2EncodeHeadersParam *p = en->p;
    f(env, BURROW_S(":authority"), en->host);
    Str m = p->method;
    if (m.len == 0)
        m = BURROW_S("GET");
    f(env, BURROW_S(":method"), m);
    if (!en->is_normal_connect) {
        f(env, BURROW_S(":path"), en->path);
        f(env, BURROW_S(":scheme"), p->url->scheme);
    }
    if (en->protocol.len != 0)
        f(env, BURROW_S(":protocol"), en->protocol);
    if (en->trailers.len != 0)
        f(env, BURROW_S("trailer"), en->trailers);

    bool did_ua = false;
    if (p->header != NULL) {
        MapIter it = map_iter(p->header);
        const void *kp;
        void *vp;
        while (map_next(&it, &kp, &vp)) {
            Str k = *(const Str *)kp;
            Slice vv = *(const Slice *)vp;
            /* Host is :authority, already sent. Content-Length is automatic,
             * sent below. And per 8.1.2.2 Connection-Specific Header Fields,
             * don't send connection-specific fields. We have already checked
             * if any are error-worthy so just ignore the rest. */
            if (burrow__http_ascii_equal_fold(k, BURROW_S("host")) ||
                burrow__http_ascii_equal_fold(k, BURROW_S("content-length")) ||
                burrow__http_ascii_equal_fold(k, BURROW_S("connection")) ||
                burrow__http_ascii_equal_fold(k, BURROW_S("proxy-connection")) ||
                burrow__http_ascii_equal_fold(k, BURROW_S("transfer-encoding")) ||
                burrow__http_ascii_equal_fold(k, BURROW_S("upgrade")) ||
                burrow__http_ascii_equal_fold(k, BURROW_S("keep-alive")))
                continue;
            if (burrow__http_ascii_equal_fold(k, BURROW_S("user-agent"))) {
                /* Match Go's http1 behavior: at most one User-Agent. If set
                 * to nil or empty string, then omit it. Otherwise if not
                 * mentioned, include the default (below). */
                did_ua = true;
                if (vv.len < 1)
                    continue;
                vv = slice_sub(vv, 0, 1);
                if (h2c_value0(vv).len == 0)
                    continue;
            } else if (burrow__http_ascii_equal_fold(k, BURROW_S("cookie"))) {
                /* Per 8.1.2.5 To allow for better compression efficiency,
                 * the Cookie header field MAY be split into separate header
                 * fields, each with one or more cookie-pairs. */
                for (Int i = 0; i < vv.len; i++) {
                    Str v = *(const Str *)slice_at(vv, i);
                    for (;;) {
                        Int pi = strings_index_byte(v, ';');
                        if (pi < 0)
                            break;
                        f(env, BURROW_S("cookie"), str_from_bytes(v.p, pi));
                        pi++;
                        /* strip space after semicolon if any. */
                        while (pi + 1 <= v.len && v.p[pi] == ' ')
                            pi++;
                        v = str_from_bytes(v.p + pi, v.len - pi);
                    }
                    if (v.len > 0)
                        f(env, BURROW_S("cookie"), v);
                }
                continue;
            } else if (str_eq(k, BURROW_S(":protocol"))) {
                /* :protocol pseudo-header was already sent above. */
                continue;
            }
            for (Int i = 0; i < vv.len; i++)
                f(env, k, *(const Str *)slice_at(vv, i));
        }
    }
    if (h2c_should_send_req_content_length(p->method, p->actual_content_length))
        f(env, BURROW_S("content-length"),
          strconv_format_int(en->a, p->actual_content_length, 10));
    if (p->add_gzip_header)
        f(env, BURROW_S("accept-encoding"), BURROW_S("gzip"));
    if (!did_ua)
        f(env, BURROW_S("user-agent"), p->default_user_agent);
}

static void h2c_count_size(void *env, Str name, Str value) {
    HpackHeaderField hf;
    memset(&hf, 0, sizeof hf);
    hf.name = name;
    hf.value = value;
    *(uint64_t *)env += (uint64_t)burrow__hpack_header_field_size(hf);
}

typedef struct h2c_LowerEnv {
    Http2HeaderFunc f;
    void *env;
    const HttptraceClientTrace *trace;
    Alloc *a;
} h2c_LowerEnv;

static void h2c_lower_and_send(void *env, Str name, Str value) {
    h2c_LowerEnv *le = (h2c_LowerEnv *)env;
    bool ascii = false;
    Str lname = burrow__http_ascii_to_lower(le->a, name, &ascii);
    if (!ascii) {
        /* Skip writing invalid headers. Per RFC 7540, Section 8.1.2,
         * header field names have to be ASCII characters (just as in
         * HTTP/1.x). */
        return;
    }
    le->f(le->env, lname, value);
    if (le->trace != NULL && BURROW__HTTPTRACE_HAS(le->trace, wrote_header_field)) {
        Str one = value;
        httptrace_client_trace_wrote_header_field(le->trace, lname,
                                                  slice_from(&one, 1, 1, TYPE_STRING));
    }
}

Error burrow__http2_encode_headers(Alloc *a, const Http2EncodeHeadersParam *p,
                                   Http2HeaderFunc f, void *env,
                                   Http2EncodeHeadersResult *res) {
    memset(res, 0, sizeof *res);
    Error err = h2c_check_conn_headers(a, p->header);
    if (BURROW_FAILED(err))
        return err;
    if (p->url == NULL)
        return h2c_err_nil_url;

    Str host = p->host;
    if (host.len == 0)
        host = p->url->host;
    host = burrow__httpguts_punycode_host_port(a, host, &err);
    if (BURROW_FAILED(err))
        return err;
    if (!burrow__httpguts_valid_host_header(host))
        return h2c_err_invalid_host;

    /* isNormalConnect is true if this is a non-extended CONNECT request. */
    bool is_normal_connect = false;
    Str protocol = BURROW_STR_EMPTY;
    /* The key is not canonical, so it is looked up as it is. */
    Slice pv = slice_from(NULL, 0, 0, TYPE_STRING);
    if (p->header != NULL) {
        Str pk = BURROW_S(":protocol");
        const Slice *vv = (const Slice *)map_get(p->header, &pk);
        if (vv != NULL)
            pv = *vv;
    }
    if (pv.len > 0)
        protocol = h2c_value0(pv);
    if (str_eq(p->method, BURROW_S("CONNECT")) && protocol.len == 0)
        is_normal_connect = true;
    else if (protocol.len != 0 && !str_eq(p->method, BURROW_S("CONNECT")))
        return h2c_err_invalid_protocol;

    Str path = BURROW_STR_EMPTY;
    if (!is_normal_connect) {
        path = url_request_uri(p->url, a);
        if (!h2c_valid_pseudo_path(path)) {
            Str orig = path;
            Str prefix = fmt_sprintf_v(a, "%s://%s", p->url->scheme, host);
            path = strings_trim_prefix(path, prefix);
            if (!h2c_valid_pseudo_path(path)) {
                if (p->url->opaque.len != 0)
                    return fmt_errorf_v("invalid request :path %q from URL.Opaque = %q",
                                        orig, p->url->opaque);
                return fmt_errorf_v("invalid request :path %q", orig);
            }
        }
    }

    /* Check for any invalid headers+trailers and return an error before we
     * potentially pollute our hpack state. (We want to be able to continue
     * using the hpack encoder.) */
    Str bad = h2c_validate_headers(a, p->header);
    if (bad.len != 0)
        return fmt_errorf_v("invalid HTTP header %s", bad);
    bad = h2c_validate_headers(a, p->trailer);
    if (bad.len != 0)
        return fmt_errorf_v("invalid HTTP trailer %s", bad);

    Str trailers;
    err = h2c_comma_separated_trailers(a, p->trailer, &trailers);
    if (BURROW_FAILED(err))
        return err;

    h2c_Enum en;
    memset(&en, 0, sizeof en);
    en.p = p;
    en.host = host;
    en.path = path;
    en.protocol = protocol;
    en.trailers = trailers;
    en.is_normal_connect = is_normal_connect;
    en.a = a;

    /* Do a first pass over the headers counting bytes to ensure we don't
     * exceed cc.peerMaxHeaderListSize. This is done as a separate pass
     * before encoding the headers to prevent modifying the hpack state. */
    if (p->peer_max_header_list_size > 0) {
        uint64_t hl_size = 0;
        h2c_enumerate_headers(&en, h2c_count_size, &hl_size);
        if (hl_size > p->peer_max_header_list_size)
            return burrow__http2_err_request_header_list_size;
    }

    h2c_LowerEnv le;
    le.f = f;
    le.env = env;
    le.trace = httptrace_context_client_trace(p->ctx);
    le.a = a;
    /* Header list size is ok. Write the headers. */
    h2c_enumerate_headers(&en, h2c_lower_and_send, &le);

    res->has_body = p->actual_content_length != 0;
    res->has_trailers = trailers.len != 0;
    return BURROW_NO_ERROR;
}

/* ----------------------------------------------------------------- types */

typedef struct h2c_Conn h2c_Conn;
typedef struct h2c_Stream h2c_Stream;
typedef struct h2c_GzipReader h2c_GzipReader;

/* addConnCall: an add_conn that is setting up a connection for a key, which
 * the ones for the same key that come while it does wait for. */
typedef struct h2c_AddCall {
    Str key;
    Chan *done; /* closed when it is done */
    Error err;  /* in earena, set before done is closed */
    Arena earena;
    Int refs; /* under the transport's mu */
    struct h2c_AddCall *next;
} h2c_AddCall;

/* Transport, with clientConnPool in it. Go's pool keeps a list of
 * connections for each key. This keeps one list of them all, in the order
 * they came, and each connection knows its key, which gives the same order
 * for a key. */
struct burrow__Http2Transport {
    HttpTransport *t1;
    Alloc *a;
    SyncMutex mu;
    SyncCond gone; /* on mu, broadcast as a connection is freed */
    h2c_Conn **pool;
    Int npool;
    Int pool_cap;
    h2c_AddCall *calls;
    h2c_Conn *all; /* every connection not yet freed */
    SyncAtomicBool closed;
};

/* ClientConn. The flags are at the end, each under what its group says. */
struct h2c_Conn {
    Http2Transport *t;
    Alloc *a;
    h2c_Conn *all_prev; /* under t->mu */
    h2c_Conn *all_next;
    Str key; /* in a */
    NetConn tconn;
    h2c_Config conf;
    SyncAtomicInt32 refs;
    SyncAtomicUint32 atomic_reused;

    /* readLoop goroutine fields: */
    Chan *reader_done; /* closed on error */
    Error reader_err;  /* kept, and set before reader_done is closed */

    Duration idle_timeout; /* or 0 for never */
    /* idleTimer, which is idle_t or, once the read loop is done, dead_t.
     * Each armed timer holds a reference. All three are under mu. */
    TimeTimer *idle_timer;
    TimeTimer *idle_t;
    TimeTimer *dead_t;
    TimeTimer *read_idle_t; /* the read loop's health check */

    SyncMutex mu;  /* guards the following */
    SyncCond cond; /* on mu, broadcast on flow and closed changes */
    Http2Outflow flow;
    Http2Inflow inflow;
    Chan *seen_settings_chan; /* closed when seen_settings is or reading fails */
    Str go_away_debug;        /* in earena */
    Map *streams; /* uint32_t to h2c_Stream *, which each hold a reference */
    Int streams_reserved;
    Int pending_requests;
    Map *pings; /* the 8 bytes, as a uint64_t, to the Chan * a PING ack closes */
    BufioReader *br;
    Time last_active;
    Time last_idle;
    uint32_t go_away_last_stream_id;
    Http2ErrCode go_away_err_code;
    uint32_t next_stream_id;

    /* Settings from peer, also guarded by wmu. */
    uint32_t max_frame_size;
    uint32_t max_concurrent_streams;
    uint32_t peer_max_header_table_size;
    uint32_t initial_window_size;
    int32_t initial_stream_recv_window_size;
    uint32_t read_before_stream_id;
    uint64_t peer_max_header_list_size;
    Duration read_idle_timeout;
    Duration ping_timeout;
    Int pending_resets;

    /* A semaphore with room for one, held while a request's headers are
     * written. Taken before mu or wmu. */
    Chan *req_header_mu;

    /* Held while writing. Taken before mu when both are. */
    SyncMutex wmu;
    BufioWriter *bw;
    Http2Framer *fr;
    Error werr; /* the first write error, kept */
    BytesBuffer hbuf;
    HpackEncoder *henc;
    HpackDecoder *hdec;

    /* Where errors that outlive a call are kept. */
    SyncMutex emu;
    Arena earena;

    bool in_pool;         /* under t->mu, and holds a reference */
    bool get_conn_called; /* under t->mu */
    bool free_closed;     /* under t->mu, once the transport's free closed it */
    bool single_use;

    /* Under mu. */
    bool do_not_reuse;
    bool closing;
    bool closed;
    bool closed_on_idle;
    bool seen_settings;
    bool want_settings_ack;
    bool has_go_away;

    /* Settings from peer, under mu and wmu. */
    bool extended_connect_allowed;
    bool strict_max_concurrent_streams;
    bool rst_stream_pings_blocked;
};

/* clientStream. The flags are at the end. */
struct h2c_Stream {
    h2c_Conn *cc; /* a reference */
    HttpRequest *req;

    Context ctx;
    /* trace is cleared, under tmu, before donec is closed, and the read loop
     * calls it under tmu, so nothing calls it once the round trip is over. */
    SyncMutex tmu;
    const HttptraceClientTrace *trace;

    SyncAtomicInt32 refs;
    uint32_t id;
    Http2Pipe buf_pipe; /* the flow-controlled response body */
    Http2DataBuffer dbuf;

    Chan *abort;       /* closed to end the stream straight away */
    Error abort_err;   /* kept, set if abort is closed */
    Chan *peer_closed; /* closed when the peer sends END_STREAM */
    Chan *donec;       /* closed after the stream is closed */
    Chan *on100;       /* room for one, sent to on a 100 */

    Chan *resp_header_recv; /* closed when headers are received */
    HttpResponse *res;      /* set if resp_header_recv is closed */

    Http2Outflow flow;    /* under cc->mu */
    Http2Inflow inflow;   /* under cc->mu */
    int64_t bytes_remain; /* -1 means unknown, owned by the body's Read */
    Error read_err;       /* kept, sticky, owned by the body's Read */

    IoReadCloser req_body;
    int64_t req_body_content_length; /* -1 means unknown */
    Chan *req_body_closed;           /* closed once req_body_closing's Close is done */

    /* Owned by the read loop. */
    int64_t total_header_size;
    Arena tarena;
    HttpHeader trailer; /* in tarena */

    h2c_GzipReader *gz;

    SyncMutex emu;
    Arena earena;

    bool requested_gzip;
    bool is_head;
    bool aborted;          /* under cc->mu */
    bool res_taken;        /* res is the caller's, and holds a reference */
    bool req_body_closing; /* under cc->mu, set once Close has begun */
    bool body_closed;      /* the response body's Close has run */

    /* Owned by writeRequest. */
    bool sent_end_stream;
    bool sent_headers;

    /* Owned by the read loop. */
    bool first_byte;
    bool past_headers;
    bool past_trailers;
    bool read_closed;
    bool read_aborted;
};

/* ------------------------------------------------------ connection basics */

/* A copy of err that lasts as long as the connection. */
static Error h2c_keep_err(h2c_Conn *cc, Error err) {
    if (BURROW_OK(err))
        return err;
    sync_mutex_lock(&cc->emu);
    Error kept = error_retain(arena_allocator(&cc->earena), err);
    sync_mutex_unlock(&cc->emu);
    return kept;
}

/* A copy of err that lasts as long as the stream. */
static Error h2c_skeep(h2c_Stream *cs, Error err) {
    if (BURROW_OK(err))
        return err;
    sync_mutex_lock(&cs->emu);
    Error kept = error_retain(arena_allocator(&cs->earena), err);
    sync_mutex_unlock(&cs->emu);
    return kept;
}

static void h2c_conn_free(h2c_Conn *cc);
static void h2c_stream_free(h2c_Stream *cs);

static void h2c_ref(h2c_Conn *cc) {
    (void)sync_atomic_int32_add(&cc->refs, 1);
}

static void h2c_unref(h2c_Conn *cc) {
    if (sync_atomic_int32_add(&cc->refs, -1) == 0)
        h2c_conn_free(cc);
}

/* A reference, unless the last one has gone and cc is on its way out. */
static bool h2c_tryref(h2c_Conn *cc) {
    for (;;) {
        int32_t n = sync_atomic_int32_load(&cc->refs);
        if (n <= 0)
            return false;
        if (sync_atomic_int32_compare_and_swap(&cc->refs, n, n + 1))
            return true;
    }
}

static void h2c_sref(h2c_Stream *cs) {
    (void)sync_atomic_int32_add(&cs->refs, 1);
}

static void h2c_sunref(h2c_Stream *cs) {
    if (sync_atomic_int32_add(&cs->refs, -1) == 0)
        h2c_stream_free(cs);
}

/* time.AfterFunc into *tp, which holds a reference while it is armed. The
 * caller holds one too, and mu. */
static void h2c_timer_start(h2c_Conn *cc, TimeTimer **tp, Duration d, Func fn) {
    h2c_ref(cc);
    if (*tp == NULL) {
        *tp = time_after_func(cc->a, d, fn);
        if (*tp == NULL)
            h2c_unref(cc);
        return;
    }
    bool pending = false;
    bool armed = time_timer_reset(*tp, d, &pending);
    if (!armed)
        h2c_unref(cc);
    if (pending)
        h2c_unref(cc);
}

/* Timer.Reset on a timer h2c_timer_start made. */
static void h2c_timer_reset(h2c_Conn *cc, TimeTimer *t, Duration d) {
    h2c_ref(cc);
    bool pending = false;
    bool armed = time_timer_reset(t, d, &pending);
    if (!armed)
        h2c_unref(cc);
    if (pending)
        h2c_unref(cc);
}

/* Timer.Stop. */
static void h2c_timer_stop(h2c_Conn *cc, TimeTimer *t) {
    if (t != NULL && time_timer_stop(t))
        h2c_unref(cc);
}

/* closeConn. Go also arms a timer to close the conn under a TLS conn, which
 * a plain conn does not have. */
static void h2c_close_conn(h2c_Conn *cc) {
    ArenaMark m = error_mark();
    (void)cc->tconn.vt->closer.close(cc->tconn.data);
    error_release(m);
}

/* stickyErrWriter.Write, with writeWithByteTimeout. */
static Int h2c_sticky_write(void *self, Slice p, Error *err) {
    h2c_Conn *cc = (h2c_Conn *)self;
    NetConn c = cc->tconn;
    if (BURROW_FAILED(cc->werr)) {
        *err = cc->werr;
        return 0;
    }
    Duration timeout = cc->conf.write_byte_timeout;
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    if (timeout <= 0) {
        n = c.vt->writer.write(c.data, p, &e);
    } else {
        for (;;) {
            ArenaMark m = error_mark();
            (void)c.vt->set_write_deadline(c.data, time_add(time_now(), timeout));
            e = BURROW_NO_ERROR;
            Int nn = c.vt->writer.write(c.data, slice_sub(p, n, p.len), &e);
            n += nn;
            if (n == p.len || nn == 0 || !errors_is(e, os_err_deadline_exceeded)) {
                Time zero;
                memset(&zero, 0, sizeof zero);
                (void)c.vt->set_write_deadline(c.data, zero);
                break;
            }
            error_release(m);
        }
    }
    cc->werr = h2c_keep_err(cc, e);
    *err = cc->werr;
    return n;
}

static const IoWriterVT h2c_sticky_writer_vt = {NULL, h2c_sticky_write};

/* countError, for the names the client counts. */
static void h2c_count(h2c_Conn *cc, const char *name, Http2ErrCode code) {
    if (BURROW_FUNC_IS_NIL(cc->conf.count_error))
        return;
    Byte cb[HTTP2_STRING_MAX];
    Str cs = burrow__http2_err_code_string_token(code, cb);
    char out[128];
    int n = snprintf(out, sizeof out, "%s%.*s", name, (int)cs.len, (const char *)cs.p);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof out)
        n = (int)sizeof out - 1;
    BURROW_CALLF(cc->conf.count_error, str_from_bytes(out, n));
}

static void h2c_count_name(h2c_Conn *cc, const char *name) {
    if (!BURROW_FUNC_IS_NIL(cc->conf.count_error))
        BURROW_CALLF(cc->conf.count_error, str_from_cstr(name));
}

/* logf and vlogf, which go to the log package, as Transport.logf does. */
#define h2c_logf(...) log_printf_v(__VA_ARGS__)
#define h2c_vlogf(...)                                                                 \
    do {                                                                               \
        if (burrow__http2_verbose_logs())                                              \
            log_printf_v(__VA_ARGS__);                                                 \
    } while (0)

/* -------------------------------------------------------- the pool's side */

/* clientConnPool.MarkDead. Takes the pool's reference, so the caller must
 * hold one of its own, and must not hold cc->mu. */
static void h2c_mark_dead(h2c_Conn *cc) {
    Http2Transport *t = cc->t;
    sync_mutex_lock(&t->mu);
    bool was = cc->in_pool;
    if (was) {
        Int j = 0;
        for (Int i = 0; i < t->npool; i++) {
            if (t->pool[i] != cc)
                t->pool[j++] = t->pool[i];
        }
        t->npool = j;
        cc->in_pool = false;
    }
    sync_mutex_unlock(&t->mu);
    if (was)
        h2c_unref(cc);
}

/* ------------------------------------------------- connection state */

/* tooIdleLocked. */
static bool h2c_too_idle_locked(h2c_Conn *cc) {
    /* The Round(0) strips the monotonic clock reading so the times are
     * compared in wall time, which is right here: a laptop that slept is
     * not to keep its idle connections. */
    return cc->idle_timeout != 0 && !time_is_zero(cc->last_idle) &&
           time_since(time_round(cc->last_idle, 0)) > cc->idle_timeout;
}

/* isUsableLocked. */
static bool h2c_is_usable_locked(h2c_Conn *cc) {
    return !cc->has_go_away && !cc->closed && !cc->closing && !cc->do_not_reuse &&
           (int64_t)cc->next_stream_id + 2 * (int64_t)cc->pending_requests <
               INT32_MAX &&
           !h2c_too_idle_locked(cc);
}

/* currentRequestCountLocked. */
static Int h2c_current_request_count_locked(h2c_Conn *cc) {
    return map_len(cc->streams) + cc->streams_reserved + cc->pending_resets;
}

/* idleStateLocked, which is canTakeNewRequestLocked. */
static bool h2c_can_take_new_request_locked(h2c_Conn *cc) {
    if (cc->single_use && cc->next_stream_id > 1)
        return false;
    bool max_concurrent_okay;
    if (cc->strict_max_concurrent_streams) {
        /* We'll tell the caller we can take a new request to prevent the
         * caller from dialing a new TCP connection, but then we'll block
         * later before writing it. */
        max_concurrent_okay = true;
    } else {
        max_concurrent_okay =
            h2c_current_request_count_locked(cc) < (Int)cc->max_concurrent_streams;
    }
    bool ok = max_concurrent_okay && h2c_is_usable_locked(cc);
    /* If this connection has never been used for a request and is closed,
     * then let it take a request (which will fail). If the conn was closed
     * for idleness, we're racing the idle timer; don't try to use the
     * conn. This avoids a situation where an error early in a connection's
     * lifetime goes unreported. */
    if (cc->next_stream_id == 1 && cc->streams_reserved == 0 && cc->closed &&
        !cc->closed_on_idle)
        ok = true;
    return ok;
}

/* CanTakeNewRequest. */
static bool h2c_can_take_new_request(h2c_Conn *cc) {
    sync_mutex_lock(&cc->mu);
    bool ok = h2c_can_take_new_request_locked(cc);
    sync_mutex_unlock(&cc->mu);
    return ok;
}

/* ReserveNewRequest. */
static bool h2c_reserve_new_request(h2c_Conn *cc) {
    sync_mutex_lock(&cc->mu);
    bool ok = h2c_can_take_new_request_locked(cc);
    if (ok)
        cc->streams_reserved++;
    sync_mutex_unlock(&cc->mu);
    return ok;
}

/* SetDoNotReuse. */
static void h2c_set_do_not_reuse(h2c_Conn *cc) {
    sync_mutex_lock(&cc->mu);
    cc->do_not_reuse = true;
    sync_mutex_unlock(&cc->mu);
}

/* decrStreamReservationsLocked. */
static void h2c_decr_stream_reservations_locked(h2c_Conn *cc) {
    if (cc->streams_reserved > 0)
        cc->streams_reserved--;
}

static void h2c_decr_stream_reservations(h2c_Conn *cc) {
    sync_mutex_lock(&cc->mu);
    h2c_decr_stream_reservations_locked(cc);
    sync_mutex_unlock(&cc->mu);
}

/* closeIfIdle. */
static void h2c_close_if_idle(h2c_Conn *cc) {
    sync_mutex_lock(&cc->mu);
    if (map_len(cc->streams) > 0 || cc->streams_reserved > 0) {
        sync_mutex_unlock(&cc->mu);
        return;
    }
    cc->closed = true;
    cc->closed_on_idle = true;
    sync_mutex_unlock(&cc->mu);
    h2c_close_conn(cc);
}

/* onIdleTimeout, from idle_t. */
static void h2c_on_idle_timeout(void *env) {
    h2c_Conn *cc = (h2c_Conn *)env;
    h2c_close_if_idle(cc);
    h2c_unref(cc);
}

/* What dead_t runs. */
static void h2c_on_dead(void *env) {
    h2c_Conn *cc = (h2c_Conn *)env;
    h2c_mark_dead(cc);
    h2c_unref(cc);
}

static void h2c_close_req_body_locked(h2c_Stream *cs);

/* abortStreamLocked. */
static void h2c_abort_stream_locked(h2c_Stream *cs, Error err) {
    if (!cs->aborted) {
        cs->aborted = true;
        cs->abort_err = h2c_skeep(cs, err);
        chan_close(cs->abort);
    }
    if (cs->req_body.vt != NULL)
        h2c_close_req_body_locked(cs);
    /* Wake up writeRequestBody if it is waiting on flow control. */
    sync_cond_broadcast(&cs->cc->cond);
}

/* abortStream. */
static void h2c_abort_stream(h2c_Stream *cs, Error err) {
    sync_mutex_lock(&cs->cc->mu);
    h2c_abort_stream_locked(cs, err);
    sync_mutex_unlock(&cs->cc->mu);
}

/* Each stream in cc->streams, for fn to look at under cc->mu. */
/* NOLINTBEGIN(bugprone-macro-parentheses): cs is the name it declares. */
#define H2C_EACH_STREAM(cc, cs)                                                        \
    for (MapIter it_ = map_iter((cc)->streams); map_next(&it_, &k_, &v_);)             \
        for (h2c_Stream *cs = (h2c_Stream *)*(uintptr_t *)v_; cs != NULL; cs = NULL)
/* NOLINTEND(bugprone-macro-parentheses) */

/* closeForError. */
static void h2c_close_for_error(h2c_Conn *cc, Error err) {
    sync_mutex_lock(&cc->mu);
    cc->closed = true;
    const void *k_;
    void *v_;
    H2C_EACH_STREAM(cc, cs) {
        h2c_abort_stream_locked(cs, err);
    }
    sync_cond_broadcast(&cc->cond);
    sync_mutex_unlock(&cc->mu);
    h2c_close_conn(cc);
}

/* closeForLostPing. */
static void h2c_close_for_lost_ping(h2c_Conn *cc) {
    h2c_count_name(cc, "conn_close_lost_ping");
    h2c_close_for_error(cc, h2c_err_conn_lost);
}

/* --------------------------------------------------------- newClientConn */

static void h2c_read_loop(void *env);

/* The last reference has gone, so the read loop is over and no stream,
 * timer or goroutine has the connection. */
static void h2c_conn_free(h2c_Conn *cc) {
    Http2Transport *t = cc->t;
    Alloc *a = cc->a;
    time_timer_free(cc->idle_t);
    time_timer_free(cc->dead_t);
    time_timer_free(cc->read_idle_t);
    if (cc->fr != NULL)
        burrow__http2_framer_free(cc->fr);
    if (cc->hdec != NULL)
        burrow__hpack_decoder_free(cc->hdec);
    if (cc->henc != NULL)
        burrow__hpack_encoder_free(cc->henc);
    bytes_buffer_free(&cc->hbuf);
    if (cc->bw != NULL)
        bufio_writer_free(cc->bw);
    if (cc->br != NULL)
        bufio_reader_free(cc->br);
    /* Each Ping takes its channel out of pings before it goes, and holds a
     * reference while it is there, so pings is empty. */
    map_free(cc->pings);
    map_free(cc->streams);
    chan_free(cc->reader_done);
    chan_free(cc->seen_settings_chan);
    chan_free(cc->req_header_mu);
    burrow__http_transport_free_conn(t->t1, cc->tconn);
    arena_free(&cc->earena);

    sync_mutex_lock(&t->mu);
    if (cc->all_prev != NULL)
        cc->all_prev->all_next = cc->all_next;
    else
        t->all = cc->all_next;
    if (cc->all_next != NULL)
        cc->all_next->all_prev = cc->all_prev;
    mem_free(a, cc, sizeof *cc, _Alignof(h2c_Conn));
    sync_cond_broadcast(&t->gone);
    sync_mutex_unlock(&t->mu);
}

/* newClientConn. Takes c whatever happens: when there is an error, c has
 * been closed and given back. The connection comes with one reference, the
 * caller's. */
static h2c_Conn *h2c_new_client_conn(Http2Transport *t, NetConn c, bool single_use,
                                     Str key, Error *err) {
    Alloc *a = t->a;
    h2c_Conn *cc = (h2c_Conn *)mem_alloc(a, sizeof *cc, _Alignof(h2c_Conn));
    if (cc == NULL) {
        burrow__http_transport_free_conn(t->t1, c);
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    cc->t = t;
    cc->a = a;
    cc->refs.v = 1;
    cc->tconn = c;
    cc->conf = h2c_config_from_transport(t->t1);
    cc->single_use = single_use;
    cc->next_stream_id = 1;
    cc->max_frame_size = H2C_SPEC_MAX_FRAME_SIZE;      /* spec default */
    cc->initial_window_size = H2C_INITIAL_WINDOW_SIZE; /* spec default */
    cc->initial_stream_recv_window_size =
        (int32_t)cc->conf.max_receive_buffer_per_stream;
    /* "infinite", per spec. Use a smaller value until we have received
     * server settings. */
    cc->max_concurrent_streams = H2C_INITIAL_MAX_CONCURRENT_STREAMS;
    cc->strict_max_concurrent_streams = cc->conf.strict_max_concurrent_requests;
    /* "infinite", per spec. Use 2^64-1 instead. */
    cc->peer_max_header_list_size = UINT64_MAX;
    cc->want_settings_ack = true;
    cc->read_idle_timeout = cc->conf.send_ping_timeout;
    cc->ping_timeout = cc->conf.ping_timeout;
    cc->last_active = time_now();
    cc->cond = SYNC_COND(sync_mutex_locker(&cc->mu));
    arena_init(&cc->earena, a, 0);
    cc->hbuf = BYTES_BUFFER(a);

    sync_mutex_lock(&t->mu);
    cc->all_next = t->all;
    if (t->all != NULL)
        t->all->all_prev = cc;
    t->all = cc;
    sync_mutex_unlock(&t->mu);

    cc->key = str_clone(arena_allocator(&cc->earena), key);
    cc->reader_done = chan_make(a, TYPE_BOOL, 0);
    cc->seen_settings_chan = chan_make(a, TYPE_BOOL, 0);
    cc->req_header_mu = chan_make(a, TYPE_BOOL, 1);
    cc->streams = map_make(a, TYPE_OF(uint32_t), TYPE_UINTPTR, 0);
    cc->pings = map_make(a, TYPE_OF(uint64_t), TYPE_UINTPTR, 0);
    if (cc->key.len != key.len || cc->reader_done == NULL ||
        cc->seen_settings_chan == NULL || cc->req_header_mu == NULL ||
        cc->streams == NULL || cc->pings == NULL)
        goto oom;

    (void)burrow__http2_outflow_add(&cc->flow, H2C_INITIAL_WINDOW_SIZE);
    IoWriter sw = {&h2c_sticky_writer_vt, cc};
    cc->bw = bufio_new_writer_size(a, sw, H2C_BUF_SIZE);
    IoReader cr = {&c.vt->reader, c.data};
    cc->br = bufio_new_reader_size(a, cr, H2C_BUF_SIZE);
    if (cc->bw == NULL || cc->br == NULL)
        goto oom;
    cc->fr = burrow__http2_new_framer(a, bufio_writer_as_io_writer(cc->bw),
                                      bufio_reader_as_io_reader(cc->br));
    if (cc->fr == NULL)
        goto oom;
    burrow__http2_framer_set_max_read_frame_size(
        cc->fr, (uint32_t)cc->conf.max_read_frame_size);
    if (!BURROW_FUNC_IS_NIL(cc->conf.count_error)) {
        cc->fr->count_error.f = cc->conf.count_error.f;
        cc->fr->count_error.env = cc->conf.count_error.env;
    }
    uint32_t max_header_table_size = (uint32_t)cc->conf.max_decoder_header_table_size;
    cc->hdec = burrow__hpack_new_decoder(a, max_header_table_size, NULL, NULL);
    if (cc->hdec == NULL)
        goto oom;
    cc->fr->read_meta_headers = cc->hdec;
    uint32_t max_header_list_size = h2c_max_header_list_size(t->t1);
    cc->fr->max_header_list_size = max_header_list_size;

    cc->henc = burrow__hpack_new_encoder(a, bytes_buffer_as_io_writer(&cc->hbuf));
    if (cc->henc == NULL)
        goto oom;
    burrow__hpack_encoder_set_max_dynamic_table_size_limit(
        cc->henc, (uint32_t)cc->conf.max_encoder_header_table_size);
    cc->peer_max_header_table_size = HTTP2_INITIAL_HEADER_TABLE_SIZE;

    Http2Setting initial[5];
    Int n = 0;
    initial[n++] = (Http2Setting){HTTP2_SETTING_ENABLE_PUSH, 0};
    initial[n++] = (Http2Setting){HTTP2_SETTING_INITIAL_WINDOW_SIZE,
                                  (uint32_t)cc->initial_stream_recv_window_size};
    initial[n++] = (Http2Setting){HTTP2_SETTING_MAX_FRAME_SIZE,
                                  (uint32_t)cc->conf.max_read_frame_size};
    if (max_header_list_size != 0)
        initial[n++] =
            (Http2Setting){HTTP2_SETTING_MAX_HEADER_LIST_SIZE, max_header_list_size};
    if (max_header_table_size != HTTP2_INITIAL_HEADER_TABLE_SIZE)
        initial[n++] =
            (Http2Setting){HTTP2_SETTING_HEADER_TABLE_SIZE, max_header_table_size};

    ArenaMark m = error_mark();
    Error e = BURROW_NO_ERROR;
    (void)bufio_writer_write_string(cc->bw, BURROW_S(HTTP2_CLIENT_PREFACE), &e);
    (void)burrow__http2_framer_write_settings(cc->fr, initial, n);
    int64_t conn_buf = cc->conf.max_receive_buffer_per_connection;
    (void)burrow__http2_framer_write_window_update(cc->fr, 0, (uint32_t)conn_buf);
    /* int32 arithmetic, which wraps as Go's does. */
    burrow__http2_inflow_init(&cc->inflow,
                              (int32_t)(uint32_t)(conn_buf + H2C_INITIAL_WINDOW_SIZE));
    (void)bufio_writer_flush(cc->bw);
    error_release(m);
    if (BURROW_FAILED(cc->werr)) {
        h2c_close_for_error(cc, burrow__http2_err_client_conn_force_closed);
        *err = error_retain(error_allocator(), cc->werr);
        h2c_unref(cc);
        return NULL;
    }

    Duration d = t->t1 != NULL ? t->t1->idle_conn_timeout : 0;
    sync_mutex_lock(&cc->mu);
    if (d != 0) {
        cc->idle_timeout = d;
        h2c_timer_start(cc, &cc->idle_t, d, BURROW_FN(Func, h2c_on_idle_timeout, cc));
        cc->idle_timer = cc->idle_t;
    }
    sync_mutex_unlock(&cc->mu);

    h2c_ref(cc);
    if (!go(BURROW_FN(Func, h2c_read_loop, cc))) {
        h2c_unref(cc);
        sync_mutex_lock(&cc->mu);
        h2c_timer_stop(cc, cc->idle_timer);
        cc->closed = true;
        sync_mutex_unlock(&cc->mu);
        h2c_close_conn(cc);
        h2c_unref(cc);
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    *err = BURROW_NO_ERROR;
    return cc;

oom:
    h2c_close_conn(cc);
    h2c_unref(cc);
    *err = burrow_err_out_of_memory;
    return NULL;
}

/* ---------------------------------------------------------------- AddConn */

/* addConnLocked, which hands the caller's reference to the pool. */
static bool h2c_pool_add_locked(Http2Transport *t, h2c_Conn *cc) {
    if (t->npool == t->pool_cap) {
        Int ncap = t->pool_cap == 0 ? 4 : t->pool_cap * 2;
        h2c_Conn **np = (h2c_Conn **)mem_alloc(t->a, (size_t)ncap * sizeof *np,
                                               _Alignof(h2c_Conn *));
        if (np == NULL)
            return false;
        if (t->npool > 0)
            memcpy(np, t->pool, (size_t)t->npool * sizeof *np);
        if (t->pool != NULL)
            mem_free(t->a, t->pool, (size_t)t->pool_cap * sizeof *np,
                     _Alignof(h2c_Conn *));
        t->pool = np;
        t->pool_cap = ncap;
    }
    t->pool[t->npool++] = cc;
    cc->in_pool = true;
    return true;
}

static void h2c_add_call_unref(Http2Transport *t, h2c_AddCall *call) {
    sync_mutex_lock(&t->mu);
    bool last = --call->refs == 0;
    sync_mutex_unlock(&t->mu);
    if (!last)
        return;
    chan_free(call->done);
    arena_free(&call->earena);
    mem_free(t->a, call, sizeof *call, _Alignof(h2c_AddCall));
}

Http2Transport *burrow__http2_new_transport(HttpTransport *t1) {
    Alloc *a = t1 != NULL && t1->a != NULL ? t1->a : heap_allocator();
    Http2Transport *t =
        (Http2Transport *)mem_alloc(a, sizeof *t, _Alignof(Http2Transport));
    if (t == NULL)
        return NULL;
    t->t1 = t1;
    t->a = a;
    t->gone = SYNC_COND(sync_mutex_locker(&t->mu));
    return t;
}

/* AddConn, with addConnIfNeeded and addConnCall.run, which the first caller
 * for a key runs itself rather than in a goroutine of its own, since it
 * waits for it anyway. */
Error burrow__http2_transport_add_conn(Http2Transport *t, Str scheme, Str authority,
                                       NetConn c) {
    Arena tmp;
    arena_init(&tmp, heap_allocator(), 0);
    Str addr = burrow__http2_authority_addr(arena_allocator(&tmp), scheme, authority);

    sync_mutex_lock(&t->mu);
    for (Int i = 0; i < t->npool; i++) {
        h2c_Conn *cc = t->pool[i];
        if (str_eq(cc->key, addr) && h2c_can_take_new_request(cc)) {
            sync_mutex_unlock(&t->mu);
            arena_free(&tmp);
            burrow__http_transport_free_conn(t->t1, c);
            return BURROW_NO_ERROR;
        }
    }
    h2c_AddCall *call = t->calls;
    while (call != NULL && !str_eq(call->key, addr))
        call = call->next;
    bool dup = call != NULL;
    if (dup) {
        call->refs++;
    } else {
        call = (h2c_AddCall *)mem_alloc(t->a, sizeof *call, _Alignof(h2c_AddCall));
        if (call != NULL) {
            arena_init(&call->earena, t->a, 0);
            call->key = str_clone(arena_allocator(&call->earena), addr);
            call->done = chan_make(t->a, TYPE_BOOL, 0);
            if (call->done == NULL || call->key.len != addr.len) {
                chan_free(call->done);
                arena_free(&call->earena);
                mem_free(t->a, call, sizeof *call, _Alignof(h2c_AddCall));
                call = NULL;
            }
        }
        if (call == NULL) {
            sync_mutex_unlock(&t->mu);
            arena_free(&tmp);
            burrow__http_transport_free_conn(t->t1, c);
            return burrow_err_out_of_memory;
        }
        call->refs = 1;
        call->next = t->calls;
        t->calls = call;
    }
    sync_mutex_unlock(&t->mu);

    if (!dup) {
        ArenaMark m = error_mark();
        Error e = BURROW_NO_ERROR;
        bool single_use = t->t1 != NULL && t->t1->disable_keep_alives;
        h2c_Conn *cc = h2c_new_client_conn(t, c, single_use, addr, &e);
        bool added = false;
        sync_mutex_lock(&t->mu);
        if (cc == NULL) {
            call->err = error_retain(arena_allocator(&call->earena), e);
        } else {
            /* Already called by the net/http package. */
            cc->get_conn_called = true;
            added = h2c_pool_add_locked(t, cc);
            if (!added)
                call->err = burrow_err_out_of_memory;
        }
        h2c_AddCall **pp = &t->calls;
        while (*pp != call)
            pp = &(*pp)->next;
        *pp = call->next;
        sync_mutex_unlock(&t->mu);
        error_release(m);
        if (cc != NULL && !added) {
            h2c_close_for_error(cc, burrow__http2_err_client_conn_force_closed);
            h2c_unref(cc);
        }
        chan_close(call->done);
    } else {
        bool v;
        (void)chan_recv(call->done, &v);
        burrow__http_transport_free_conn(t->t1, c);
    }
    arena_free(&tmp);
    Error err = BURROW_NO_ERROR;
    if (BURROW_FAILED(call->err))
        err = error_retain(error_allocator(), call->err);
    h2c_add_call_unref(t, call);
    return err;
}

/* ------------------------------------------------------------ read loop */

/* The name Go's %T gives a frame. */
static Str h2c_frame_type_name(const Http2Frame *f) {
    switch (f->kind) {
    case HTTP2_DATA_FRAME:
        return BURROW_S("*http2.DataFrame");
    case HTTP2_HEADERS_FRAME:
        return BURROW_S("*http2.HeadersFrame");
    case HTTP2_PRIORITY_FRAME:
        return BURROW_S("*http2.PriorityFrame");
    case HTTP2_RST_STREAM_FRAME:
        return BURROW_S("*http2.RSTStreamFrame");
    case HTTP2_SETTINGS_FRAME:
        return BURROW_S("*http2.SettingsFrame");
    case HTTP2_PUSH_PROMISE_FRAME:
        return BURROW_S("*http2.PushPromiseFrame");
    case HTTP2_PING_FRAME:
        return BURROW_S("*http2.PingFrame");
    case HTTP2_GO_AWAY_FRAME:
        return BURROW_S("*http2.GoAwayFrame");
    case HTTP2_WINDOW_UPDATE_FRAME:
        return BURROW_S("*http2.WindowUpdateFrame");
    case HTTP2_CONTINUATION_FRAME:
        return BURROW_S("*http2.ContinuationFrame");
    case HTTP2_PRIORITY_UPDATE_FRAME:
        return BURROW_S("*http2.PriorityUpdateFrame");
    case HTTP2_UNKNOWN_FRAME:
        return BURROW_S("*http2.UnknownFrame");
    case HTTP2_META_HEADERS_FRAME:
        return BURROW_S("*http2.MetaHeadersFrame");
    default:
        return BURROW_S("*http2.Frame");
    }
}

/* streamByID, with a reference to the stream for the caller to drop. */
static h2c_Stream *h2c_stream_by_id(h2c_Conn *cc, uint32_t id, bool header_or_data) {
    sync_mutex_lock(&cc->mu);
    if (header_or_data) {
        /* Work around an unfortunate gRPC behavior. See the comment on
         * rst_stream_pings_blocked. */
        cc->rst_stream_pings_blocked = false;
    }
    cc->read_before_stream_id = cc->next_stream_id;
    h2c_Stream *cs = NULL;
    uintptr_t *v = (uintptr_t *)map_get(cc->streams, &id);
    if (v != NULL) {
        cs = (h2c_Stream *)*v;
        if (cs->read_aborted)
            cs = NULL;
        else
            h2c_sref(cs);
    }
    sync_mutex_unlock(&cc->mu);
    return cs;
}

/* copyTrailers, which the body's Read runs once it has read the last of the
 * body. The trailer goes into the response's own arena. */
static void h2c_copy_trailers(void *env) {
    h2c_Stream *cs = (h2c_Stream *)env;
    HttpResponse *res = cs->res;
    if (res == NULL || cs->trailer == NULL)
        return;
    Alloc *ra = arena_allocator(&res->arena);
    MapIter it = map_iter(cs->trailer);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        if (res->trailer == NULL) {
            res->trailer = http_header_make(ra);
            if (res->trailer == NULL)
                return;
        }
        const Slice *vv = (const Slice *)v;
        Str key = str_clone(ra, *(const Str *)k);
        Slice nv = slice_make(ra, TYPE_STRING, vv->len, vv->len);
        if (key.len != ((const Str *)k)->len || (vv->len > 0 && nv.p == NULL))
            return;
        for (Int i = 0; i < vv->len; i++) {
            Str s = ((const Str *)vv->p)[i];
            ((Str *)nv.p)[i] = str_clone(ra, s);
        }
        if (!map_set(res->trailer, &key, &nv))
            return;
    }
}

/* endStream. */
static void h2c_end_stream(h2c_Conn *cc, h2c_Stream *cs) {
    /* TODO: check that any declared content-length matches, like server.go's
     * (*stream).endStream method. */
    if (cs->read_closed)
        return;
    cs->read_closed = true;
    /* Close buf_pipe and peer_closed with cc->mu held to avoid a race: the
     * caller can read io.EOF from the body and close it before peer_closed
     * is closed, which makes cleanupWriteRequest send a RST_STREAM. */
    sync_mutex_lock(&cc->mu);
    burrow__http2_pipe_close_with_error_and_code(
        &cs->buf_pipe, io_eof, BURROW_FN(Func, h2c_copy_trailers, cs));
    chan_close(cs->peer_closed);
    sync_mutex_unlock(&cc->mu);
}

/* endStreamError. */
static void h2c_end_stream_error(h2c_Stream *cs, Error err) {
    cs->read_aborted = true;
    h2c_abort_stream(cs, err);
}

/* endStreamErrorLocked. */
static void h2c_end_stream_error_locked(h2c_Stream *cs, Error err) {
    cs->read_aborted = true;
    h2c_abort_stream_locked(cs, err);
}

/* missingBody, for a response that ends with its header and yet says it has
 * a body. */
static Int h2c_missing_body_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    *err = io_err_unexpected_eof;
    return 0;
}

static Error h2c_missing_body_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static const IoReadCloserVT h2c_missing_body_vt = {{NULL, h2c_missing_body_read},
                                                   {NULL, h2c_missing_body_close}};

/* transportResponseBody, whose self is the stream. */
static Int h2c_body_read(void *self, Slice p, Error *err);
static Error h2c_body_close(void *self);

static const IoReadCloserVT h2c_body_vt = {{NULL, h2c_body_read},
                                           {NULL, h2c_body_close}};

/* The gzipReader around the stream's body, or a body with a NULL vt when the
 * memory for it is not there. */
static IoReadCloser h2c_gzip_body(h2c_Stream *cs);

typedef struct h2c_TrailerEnv {
    HttpHeader t;
    Alloc *a;
    bool oom;
} h2c_TrailerEnv;

/* t[CanonicalHeader(v)] = nil, for each name a Trailer header gives. */
static void h2c_declare_trailer(void *env, Str v) {
    h2c_TrailerEnv *te = (h2c_TrailerEnv *)env;
    if (te->oom)
        return;
    Str name = str_clone(te->a, v);
    Str key = textproto_canonical_mime_header_key(te->a, name);
    Slice nil = slice_from(NULL, 0, 0, TYPE_STRING);
    if (name.len != v.len || key.len != v.len || !map_set(te->t, &key, &nil))
        te->oom = true;
}

/* handleResponse. The response, or NULL with *err set, or NULL and no error
 * to skip the frame, which is what a 1xx response does. An error that is not
 * a ConnectionError is a StreamError of PROTOCOL_ERROR, and the error is its
 * cause. */
static HttpResponse *h2c_handle_response(h2c_Conn *cc, h2c_Stream *cs,
                                         const Http2Frame *f, Error *err) {
    const Http2MetaHeadersFrame *mh = &f->u.meta_headers;
    bool stream_ended = burrow__http2_headers_frame_stream_ended(f);
    if (mh->truncated) {
        *err = burrow__http2_err_response_header_list_size;
        return NULL;
    }

    Str status = burrow__http2_meta_headers_frame_pseudo_value(f, BURROW_S("status"));
    if (status.len == 0) {
        *err = h2c_err_missing_status;
        return NULL;
    }
    Error aerr = BURROW_NO_ERROR;
    Int status_code = strconv_atoi(status, &aerr);
    if (BURROW_FAILED(aerr)) {
        *err = h2c_err_bad_status;
        return NULL;
    }

    Alloc *a = cc->a;
    HttpResponse *res =
        (HttpResponse *)mem_alloc(a, sizeof *res, _Alignof(HttpResponse));
    if (res == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    res->a = a;
    arena_init(&res->arena, a, 0);
    res->body = http_no_body;
    Alloc *ra = arena_allocator(&res->arena);

    /* The glue in net/http's http2RoundTrip makes Status from the code. */
    Str text = http_status_text(status_code);
    Byte *sp = (Byte *)mem_alloc_nozero(ra, (size_t)(status.len + 1 + text.len), 1);
    res->header = http_header_make(ra);
    if (sp == NULL || res->header == NULL)
        goto oom;
    memcpy(sp, status.p, (size_t)status.len);
    sp[status.len] = ' ';
    if (text.len > 0)
        memcpy(sp + status.len + 1, text.p, (size_t)text.len);
    res->status = str_from_bytes(sp, status.len + 1 + text.len);
    res->status_code = status_code;
    res->proto = BURROW_S("HTTP/2.0");
    res->proto_major = 2;
    res->proto_minor = 0;
    res->request = cs->req;

    HpackHeaderFields regular = burrow__http2_meta_headers_frame_regular_fields(f);
    for (Int i = 0; i < regular.len; i++) {
        const HpackHeaderField *hf = &regular.p[i];
        if (burrow__http_ascii_equal_fold(hf->name, BURROW_S("trailer"))) {
            if (res->trailer == NULL) {
                res->trailer = http_header_make(ra);
                if (res->trailer == NULL)
                    goto oom;
            }
            h2c_TrailerEnv te = {res->trailer, ra, false};
            burrow__http_foreach_header_element(hf->value, h2c_declare_trailer, &te);
            if (te.oom)
                goto oom;
        } else {
            Str name = str_clone(ra, hf->name);
            Str value = str_clone(ra, hf->value);
            if (name.len != hf->name.len || value.len != hf->value.len ||
                !http_header_add(res->header, name, value))
                goto oom;
        }
    }

    if (status_code >= 100 && status_code <= 199) {
        Error e = BURROW_NO_ERROR;
        if (stream_ended) {
            e = h2c_err_1xx_end_stream;
            goto skip;
        }
        bool hooked = false;
        sync_mutex_lock(&cs->tmu);
        if (cs->trace != NULL && BURROW__HTTPTRACE_HAS(cs->trace, got1xx_response)) {
            /* If the 1xx response is being delivered to the user, then
             * they're responsible for limiting the number of responses. */
            hooked = true;
            e = httptrace_client_trace_got1xx_response(cs->trace, status_code,
                                                       res->header);
        }
        sync_mutex_unlock(&cs->tmu);
        if (BURROW_FAILED(e))
            goto skip;
        if (!hooked) {
            /* If the user didn't examine the 1xx response, then we limit the
             * size of all 1xx headers. This differs a bit from the HTTP/1
             * implementation, which limits the size of all 1xx headers plus
             * the final response. Use the larger limit of MaxHeaderListSize
             * and net/http.Transport.MaxResponseHeaderBytes. */
            int64_t limit = (int64_t)h2c_max_header_list_size(cc->t->t1);
            if (cc->t->t1 != NULL && cc->t->t1->max_response_header_bytes > limit)
                limit = cc->t->t1->max_response_header_bytes;
            for (Int i = 0; i < mh->fields.len; i++)
                cs->total_header_size +=
                    (int64_t)burrow__hpack_header_field_size(mh->fields.p[i]);
            if (cs->total_header_size > limit) {
                h2c_vlogf("http2: 1xx informational responses too large");
                e = h2c_err_1xx_too_large;
                goto skip;
            }
        }
        if (status_code == 100) {
            sync_mutex_lock(&cs->tmu);
            httptrace_client_trace_got100_continue(cs->trace);
            sync_mutex_unlock(&cs->tmu);
            bool one = true;
            (void)chan_try_send(cs->on100, &one);
        }
        cs->past_headers = false; /* do it all again */
    skip:
        http_response_free(res);
        *err = e;
        return NULL;
    }

    res->content_length = -1;
    Slice clens = http_header_values(res->header, BURROW_S("Content-Length"));
    if (clens.len == 1) {
        Error pe = BURROW_NO_ERROR;
        uint64_t cl = strconv_parse_uint(((const Str *)clens.p)[0], 10, 63, &pe);
        if (BURROW_OK(pe))
            res->content_length = (int64_t)cl;
        /* Otherwise, unlike http/1, it won't mess up our framing, so it's
         * safer smuggling-wise to ignore it. The same goes for more than one
         * of them. */
    } else if (clens.len == 0 && stream_ended && !cs->is_head) {
        res->content_length = 0;
    }

    if (cs->is_head)
        return res;

    if (stream_ended) {
        if (res->content_length > 0)
            res->body = (IoReadCloser){&h2c_missing_body_vt, NULL};
        return res;
    }

    memset(&cs->dbuf, 0, sizeof cs->dbuf);
    cs->dbuf.a = a;
    cs->dbuf.expected = res->content_length;
    burrow__http2_pipe_set_buffer(&cs->buf_pipe,
                                  burrow__http2_data_buffer_as_pipe_buffer(&cs->dbuf));
    cs->bytes_remain = res->content_length;
    res->body = (IoReadCloser){&h2c_body_vt, cs};

    if (cs->requested_gzip &&
        burrow__http_ascii_equal_fold(
            http_header_get(res->header, BURROW_S("Content-Encoding")),
            BURROW_S("gzip"))) {
        IoReadCloser gz = h2c_gzip_body(cs);
        if (gz.vt == NULL)
            goto oom;
        http_header_del(res->header, BURROW_S("Content-Encoding"));
        http_header_del(res->header, BURROW_S("Content-Length"));
        res->content_length = -1;
        res->body = gz;
        res->uncompressed = true;
    }
    return res;

oom:
    http_response_free(res);
    *err = burrow_err_out_of_memory;
    return NULL;
}

/* processTrailers. */
static Error h2c_process_trailers(h2c_Conn *cc, h2c_Stream *cs, const Http2Frame *f) {
    if (cs->past_trailers) {
        /* Too many HEADERS frames for this stream. */
        return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
    }
    cs->past_trailers = true;
    if (!burrow__http2_headers_frame_stream_ended(f)) {
        /* We expect that any headers for trailers also has END_STREAM. */
        return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
    }
    if (burrow__http2_meta_headers_frame_pseudo_fields(f).len > 0) {
        /* No pseudo header fields are defined for trailers.
         * TODO: ConnectionError might be overly harsh? Check. */
        return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
    }
    if (f->u.meta_headers.truncated) {
        h2c_end_stream_error(cs, burrow__http2_stream_error(
                                     f->header.stream_id, HTTP2_ERR_CODE_PROTOCOL,
                                     burrow__http2_err_response_header_list_size));
        return BURROW_NO_ERROR;
    }

    Alloc *ta = arena_allocator(&cs->tarena);
    HttpHeader trailer = http_header_make(ta);
    if (trailer == NULL)
        goto oom;
    HpackHeaderFields regular = burrow__http2_meta_headers_frame_regular_fields(f);
    for (Int i = 0; i < regular.len; i++) {
        const HpackHeaderField *hf = &regular.p[i];
        Str name = str_clone(ta, hf->name);
        Str value = str_clone(ta, hf->value);
        if (name.len != hf->name.len || value.len != hf->value.len ||
            !http_header_add(trailer, name, value))
            goto oom;
    }
    cs->trailer = trailer;

    h2c_end_stream(cc, cs);
    return BURROW_NO_ERROR;

oom:
    h2c_end_stream_error(cs, burrow_err_out_of_memory);
    return BURROW_NO_ERROR;
}

/* processHeaders, for a stream there is. */
static Error h2c_process_stream_headers(h2c_Conn *cc, h2c_Stream *cs,
                                        const Http2Frame *f) {
    uint32_t id = f->header.stream_id;
    if (cs->read_closed) {
        h2c_end_stream_error(cs, burrow__http2_stream_error(id, HTTP2_ERR_CODE_PROTOCOL,
                                                            h2c_err_headers_after_end));
        return BURROW_NO_ERROR;
    }
    if (!cs->first_byte) {
        /* TODO(bradfitz): move first response byte earlier, when we first
         * read the 9 byte header, not waiting until all the
         * HEADERS+CONTINUATION frames have been merged. This works for
         * now. */
        sync_mutex_lock(&cs->tmu);
        httptrace_client_trace_got_first_response_byte(cs->trace);
        sync_mutex_unlock(&cs->tmu);
        cs->first_byte = true;
    }
    if (cs->past_headers)
        return h2c_process_trailers(cc, cs, f);
    cs->past_headers = true;

    Error err = BURROW_NO_ERROR;
    HttpResponse *res = h2c_handle_response(cc, cs, f, &err);
    if (BURROW_FAILED(err)) {
        Http2ErrCode code;
        if (burrow__http2_error_connection(err, &code))
            return err;
        /* Any other error type is a stream error. */
        h2c_end_stream_error(
            cs, burrow__http2_stream_error(id, HTTP2_ERR_CODE_PROTOCOL, err));
        return BURROW_NO_ERROR; /* to keep the connection alive */
    }
    if (res == NULL) {
        /* The 1xx case. */
        return BURROW_NO_ERROR;
    }
    cs->res = res;
    chan_close(cs->resp_header_recv);
    if (burrow__http2_headers_frame_stream_ended(f))
        h2c_end_stream(cc, cs);
    return BURROW_NO_ERROR;
}

/* processHeaders. */
static Error h2c_process_headers(h2c_Conn *cc, const Http2Frame *f) {
    h2c_Stream *cs = h2c_stream_by_id(cc, f->header.stream_id, true);
    if (cs == NULL) {
        /* We'd get here if we canceled a request while the server had its
         * response still in flight. So if this was just something we
         * canceled, ignore it. */
        return BURROW_NO_ERROR;
    }
    Error err = h2c_process_stream_headers(cc, cs, f);
    h2c_sunref(cs);
    return err;
}

/* WINDOW_UPDATE frames for the connection and a stream, then a flush, all
 * under wmu. A zero leaves that one out. Errors stay in werr. */
static void h2c_write_window_updates(h2c_Conn *cc, uint32_t id, int32_t conn_add,
                                     int32_t stream_add) {
    sync_mutex_lock(&cc->wmu);
    ArenaMark m = error_mark();
    if (conn_add > 0)
        (void)burrow__http2_framer_write_window_update(cc->fr, 0, (uint32_t)conn_add);
    if (stream_add > 0)
        (void)burrow__http2_framer_write_window_update(cc->fr, id,
                                                       (uint32_t)stream_add);
    (void)bufio_writer_flush(cc->bw);
    error_release(m);
    sync_mutex_unlock(&cc->wmu);
}

/* processData, for a stream there is. */
static Error h2c_process_stream_data(h2c_Conn *cc, h2c_Stream *cs,
                                     const Http2Frame *f) {
    uint32_t id = f->header.stream_id;
    uint32_t length = f->header.length;
    Slice data = burrow__http2_data_frame_data(f);
    if (cs->read_closed) {
        h2c_logf("protocol error: received DATA after END_STREAM");
        h2c_end_stream_error(cs, burrow__http2_stream_error(id, HTTP2_ERR_CODE_PROTOCOL,
                                                            BURROW_NO_ERROR));
        return BURROW_NO_ERROR;
    }
    if (!cs->past_headers) {
        h2c_logf("protocol error: received DATA before a HEADERS frame");
        h2c_end_stream_error(cs, burrow__http2_stream_error(id, HTTP2_ERR_CODE_PROTOCOL,
                                                            BURROW_NO_ERROR));
        return BURROW_NO_ERROR;
    }
    if (length > 0) {
        if (cs->is_head && data.len > 0) {
            h2c_logf("protocol error: received DATA on a HEAD request");
            h2c_end_stream_error(cs, burrow__http2_stream_error(
                                         id, HTTP2_ERR_CODE_PROTOCOL, BURROW_NO_ERROR));
            return BURROW_NO_ERROR;
        }
        /* Check connection-level flow control. */
        sync_mutex_lock(&cc->mu);
        if (!burrow__http2_take_inflows(&cc->inflow, &cs->inflow, length)) {
            sync_mutex_unlock(&cc->mu);
            return burrow__http2_connection_error(HTTP2_ERR_CODE_FLOW_CONTROL);
        }
        /* Return any padded flow control now, since we won't refund it later
         * on body reads. */
        Int refund = 0;
        Int pad = (Int)length - data.len;
        if (pad > 0)
            refund += pad;

        bool did_reset = false;
        Error err = BURROW_NO_ERROR;
        if (data.len > 0) {
            (void)burrow__http2_pipe_write(&cs->buf_pipe, data, &err);
            if (BURROW_FAILED(err)) {
                /* Return len(data) now if the stream is already closed,
                 * since data will never be read. */
                did_reset = true;
                refund += data.len;
            }
        }

        int32_t send_conn = burrow__http2_inflow_add(&cc->inflow, refund);
        int32_t send_stream = 0;
        if (!did_reset)
            send_stream = burrow__http2_inflow_add(&cs->inflow, refund);
        sync_mutex_unlock(&cc->mu);

        if (send_conn > 0 || send_stream > 0)
            h2c_write_window_updates(cc, cs->id, send_conn, send_stream);

        if (BURROW_FAILED(err)) {
            h2c_end_stream_error(cs, err);
            return BURROW_NO_ERROR;
        }
    }

    if (burrow__http2_data_frame_stream_ended(f))
        h2c_end_stream(cc, cs);
    return BURROW_NO_ERROR;
}

/* processData. */
static Error h2c_process_data(h2c_Conn *cc, const Http2Frame *f) {
    uint32_t id = f->header.stream_id;
    h2c_Stream *cs = h2c_stream_by_id(cc, id, true);
    if (cs != NULL) {
        Error err = h2c_process_stream_data(cc, cs, f);
        h2c_sunref(cs);
        return err;
    }
    sync_mutex_lock(&cc->mu);
    uint32_t never_sent = cc->next_stream_id;
    sync_mutex_unlock(&cc->mu);
    if (id >= never_sent) {
        /* We never asked for this. */
        h2c_logf(
            "http2: Transport received unsolicited DATA frame; closing connection");
        return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
    }
    /* We probably did ask for this, but canceled. Just ignore it, but at
     * least return their flow control. */
    uint32_t length = f->header.length;
    if (length > 0) {
        sync_mutex_lock(&cc->mu);
        bool ok = burrow__http2_inflow_take(&cc->inflow, length);
        int32_t conn_add = burrow__http2_inflow_add(&cc->inflow, (Int)length);
        sync_mutex_unlock(&cc->mu);
        if (!ok)
            return burrow__http2_connection_error(HTTP2_ERR_CODE_FLOW_CONTROL);
        if (conn_add > 0)
            h2c_write_window_updates(cc, 0, conn_add, 0);
    }
    return BURROW_NO_ERROR;
}

/* processGoAway. */
static Error h2c_process_go_away(h2c_Conn *cc, const Http2Frame *f) {
    const Http2GoAwayFrame *g = &f->u.go_away;
    h2c_mark_dead(cc);
    Byte cb[HTTP2_STRING_MAX];
    if (g->err_code != HTTP2_ERR_CODE_NO) {
        /* TODO: deal with GOAWAY more. particularly the error code */
        h2c_vlogf("transport got GOAWAY with error code = %v",
                  burrow__http2_err_code_string(g->err_code, cb));
        h2c_count(cc, "recv_goaway_", g->err_code);
    }

    sync_mutex_lock(&cc->mu);
    bool had = cc->has_go_away;
    Http2ErrCode old_code = cc->go_away_err_code;
    cc->has_go_away = true;
    cc->go_away_last_stream_id = g->last_stream_id;
    cc->go_away_err_code = g->err_code;

    /* Merge the previous and current GoAway error frames. */
    if (cc->go_away_debug.len == 0 && g->debug_data.len > 0) {
        sync_mutex_lock(&cc->emu);
        cc->go_away_debug =
            str_clone(arena_allocator(&cc->earena),
                      str_from_bytes((const char *)g->debug_data.p, g->debug_data.len));
        sync_mutex_unlock(&cc->emu);
    }
    if (had && old_code != HTTP2_ERR_CODE_NO)
        cc->go_away_err_code = old_code;
    uint32_t last = g->last_stream_id;
    if (map_len(cc->streams) == 0) {
        /* Received a GOAWAY and no streams active, just close the conn. */
        sync_mutex_unlock(&cc->mu);
        return h2c_err_stop_read_loop;
    }
    const void *k_;
    void *v_;
    H2C_EACH_STREAM(cc, cs) {
        if (cs->id <= last) {
            /* The server's GOAWAY indicates that it received this stream.
             * It will either finish processing it, or close the connection
             * without doing so. Either way, leave the stream alone for
             * now. */
            continue;
        }
        if (cs->id == 1 && cc->go_away_err_code != HTTP2_ERR_CODE_NO) {
            /* Don't retry the first stream on a connection if we get a
             * non-NO error. If the server is sending an error on a new
             * connection, retrying the request on a new one probably isn't
             * going to work. */
            h2c_abort_stream_locked(
                cs,
                fmt_errorf_v("http2: Transport received GOAWAY from server ErrCode:%v",
                             burrow__http2_err_code_string(cc->go_away_err_code, cb)));
        } else {
            /* Aborting the stream with errClientConnGotGoAway indicates that
             * the request should be retried on a new connection. */
            h2c_abort_stream_locked(cs, burrow__http2_err_client_conn_got_go_away);
        }
    }
    sync_mutex_unlock(&cc->mu);
    return BURROW_NO_ERROR;
}

/* processSettingsNoWrite. */
static Error h2c_process_settings_no_write(h2c_Conn *cc, const Http2Frame *f) {
    sync_mutex_lock(&cc->mu);
    Error err = BURROW_NO_ERROR;
    if (burrow__http2_settings_frame_is_ack(f)) {
        if (cc->want_settings_ack)
            cc->want_settings_ack = false;
        else
            err = burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
        sync_mutex_unlock(&cc->mu);
        return err;
    }

    bool seen_max_concurrent_streams = false;
    Int n = burrow__http2_settings_frame_num_settings(f);
    for (Int i = 0; i < n && BURROW_OK(err); i++) {
        Http2Setting s = burrow__http2_settings_frame_setting(f, i);
        err = burrow__http2_setting_valid(s);
        if (BURROW_FAILED(err))
            break;
        switch ((uint32_t)s.id) {
        case HTTP2_SETTING_MAX_FRAME_SIZE:
            cc->max_frame_size = s.val;
            break;
        case HTTP2_SETTING_MAX_CONCURRENT_STREAMS:
            cc->max_concurrent_streams = s.val;
            seen_max_concurrent_streams = true;
            break;
        case HTTP2_SETTING_MAX_HEADER_LIST_SIZE:
            cc->peer_max_header_list_size = (uint64_t)s.val;
            break;
        case HTTP2_SETTING_INITIAL_WINDOW_SIZE: {
            /* Adjust flow control of currently-open frames by the difference
             * of the old initial window size and this one. */
            int32_t delta = (int32_t)s.val - (int32_t)cc->initial_window_size;
            const void *k_;
            void *v_;
            H2C_EACH_STREAM(cc, cs) {
                if (!burrow__http2_outflow_add(&cs->flow, delta))
                    err = burrow__http2_connection_error(HTTP2_ERR_CODE_FLOW_CONTROL);
            }
            if (BURROW_FAILED(err))
                break;
            sync_cond_broadcast(&cc->cond);
            cc->initial_window_size = s.val;
            break;
        }
        case HTTP2_SETTING_HEADER_TABLE_SIZE:
            burrow__hpack_encoder_set_max_dynamic_table_size(cc->henc, s.val);
            cc->peer_max_header_table_size = s.val;
            break;
        case HTTP2_SETTING_ENABLE_CONNECT_PROTOCOL:
            /* If the peer wants to send us SETTINGS_ENABLE_CONNECT_PROTOCOL,
             * we require that it do so in the first SETTINGS frame. When we
             * attempt to use extended CONNECT, we wait for the first
             * SETTINGS frame to see if the server supports it. If we let
             * the server enable the feature with a later SETTINGS frame,
             * then users will see inconsistent results depending on whether
             * we've seen that frame or not. */
            if (!cc->seen_settings)
                cc->extended_connect_allowed = s.val == 1;
            break;
        default:
            if (burrow__http2_verbose_logs()) {
                Str ss = burrow__http2_setting_string(cc->a, s);
                log_printf_v("Unhandled Setting: %v", ss);
                mem_free(cc->a, (void *)(uintptr_t)ss.p, (size_t)ss.len, 1);
            }
            break;
        }
    }
    if (BURROW_FAILED(err)) {
        sync_mutex_unlock(&cc->mu);
        return err;
    }

    if (!cc->seen_settings) {
        if (!seen_max_concurrent_streams) {
            /* This was the server's initial SETTINGS frame and it didn't
             * contain a MAX_CONCURRENT_STREAMS field so increase the number
             * of concurrent streams this connection can establish to our
             * default. */
            cc->max_concurrent_streams = H2C_DEFAULT_MAX_CONCURRENT_STREAMS;
        }
        chan_close(cc->seen_settings_chan);
        cc->seen_settings = true;
    }
    sync_mutex_unlock(&cc->mu);
    return BURROW_NO_ERROR;
}

/* processSettings. Holding wmu as well as mu lets frame encoding read the
 * settings with only wmu held. */
static Error h2c_process_settings(h2c_Conn *cc, const Http2Frame *f) {
    sync_mutex_lock(&cc->wmu);
    Error err = h2c_process_settings_no_write(cc, f);
    if (BURROW_OK(err) && !burrow__http2_settings_frame_is_ack(f)) {
        ArenaMark m = error_mark();
        (void)burrow__http2_framer_write_settings_ack(cc->fr);
        (void)bufio_writer_flush(cc->bw);
        error_release(m);
    }
    sync_mutex_unlock(&cc->wmu);
    return err;
}

/* processWindowUpdate. */
static Error h2c_process_window_update(h2c_Conn *cc, const Http2Frame *f) {
    uint32_t id = f->header.stream_id;
    h2c_Stream *cs = h2c_stream_by_id(cc, id, false);
    if (id != 0 && cs == NULL)
        return BURROW_NO_ERROR;

    Error err = BURROW_NO_ERROR;
    sync_mutex_lock(&cc->mu);
    Http2Outflow *fl = cs != NULL ? &cs->flow : &cc->flow;
    if (!burrow__http2_outflow_add(fl, (int32_t)f->u.window_update.increment)) {
        /* For a stream, the sender sends RST_STREAM with an error code of
         * FLOW_CONTROL_ERROR. */
        if (cs != NULL)
            h2c_end_stream_error_locked(
                cs, burrow__http2_stream_error(id, HTTP2_ERR_CODE_FLOW_CONTROL,
                                               BURROW_NO_ERROR));
        else
            err = burrow__http2_connection_error(HTTP2_ERR_CODE_FLOW_CONTROL);
    } else {
        sync_cond_broadcast(&cc->cond);
    }
    sync_mutex_unlock(&cc->mu);
    if (cs != NULL)
        h2c_sunref(cs);
    return err;
}

/* processResetStream. */
static Error h2c_process_reset_stream(h2c_Conn *cc, const Http2Frame *f) {
    h2c_Stream *cs = h2c_stream_by_id(cc, f->header.stream_id, false);
    if (cs == NULL) {
        /* TODO: return error if server tries to RST_STREAM an idle stream */
        return BURROW_NO_ERROR;
    }
    Http2ErrCode code = f->u.rst_stream.err_code;
    Error serr = h2c_skeep(
        cs, burrow__http2_stream_error(cs->id, code, burrow__http2_err_from_peer));
    if (code == HTTP2_ERR_CODE_PROTOCOL)
        h2c_set_do_not_reuse(cc);
    h2c_count(cc, "recv_rststream_", code);
    h2c_abort_stream(cs, serr);

    burrow__http2_pipe_close_with_error(&cs->buf_pipe, serr);
    h2c_sunref(cs);
    return BURROW_NO_ERROR;
}

/* What Ping shares with the goroutine that writes the PING frame. */
typedef struct h2c_PingWrite {
    h2c_Conn *cc; /* a reference */
    SyncAtomicInt32 refs;
    Byte data[8];
    Chan *errc; /* closed when the write fails */
    Error err;  /* kept, set before errc is closed */
} h2c_PingWrite;

static void h2c_ping_write_unref(h2c_PingWrite *pw) {
    if (sync_atomic_int32_add(&pw->refs, -1) != 0)
        return;
    h2c_Conn *cc = pw->cc;
    chan_free(pw->errc);
    mem_free(cc->a, pw, sizeof *pw, _Alignof(h2c_PingWrite));
    h2c_unref(cc);
}

static void h2c_ping_write(void *env) {
    h2c_PingWrite *pw = (h2c_PingWrite *)env;
    h2c_Conn *cc = pw->cc;
    sync_mutex_lock(&cc->wmu);
    ArenaMark m = error_mark();
    Error err = burrow__http2_framer_write_ping(cc->fr, false, pw->data);
    if (BURROW_OK(err))
        err = bufio_writer_flush(cc->bw);
    if (BURROW_FAILED(err)) {
        pw->err = h2c_keep_err(cc, err);
        chan_close(pw->errc);
    }
    error_release(m);
    sync_mutex_unlock(&cc->wmu);
    h2c_ping_write_unref(pw);
}

/* ClientConn.Ping: sends a PING frame to the server and waits for the ack.
 * The error lasts as long as the connection. */
static Error h2c_ping(h2c_Conn *cc, Context ctx) {
    Chan *c = chan_make(cc->a, TYPE_BOOL, 0);
    if (c == NULL)
        return burrow_err_out_of_memory;
    /* Generate a random payload. */
    Byte p[8];
    uint64_t key = 0;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        (void)crypto_rand_read(slice_from(p, 8, 8, TYPE_BYTE), &err);
        if (BURROW_FAILED(err)) {
            chan_free(c);
            return h2c_keep_err(cc, err);
        }
        memcpy(&key, p, sizeof key);
        sync_mutex_lock(&cc->mu);
        /* check for dup before insert */
        if (map_get(cc->pings, &key) == NULL) {
            uintptr_t cv = (uintptr_t)c;
            bool ok = map_set(cc->pings, &key, &cv);
            sync_mutex_unlock(&cc->mu);
            if (!ok) {
                chan_free(c);
                return burrow_err_out_of_memory;
            }
            break;
        }
        sync_mutex_unlock(&cc->mu);
    }

    Error err = burrow_err_out_of_memory;
    h2c_PingWrite *pw =
        (h2c_PingWrite *)mem_alloc(cc->a, sizeof *pw, _Alignof(h2c_PingWrite));
    if (pw == NULL)
        goto out;
    pw->cc = cc;
    pw->refs.v = 2;
    memcpy(pw->data, p, sizeof p);
    pw->errc = chan_make(cc->a, TYPE_BOOL, 0);
    if (pw->errc == NULL) {
        mem_free(cc->a, pw, sizeof *pw, _Alignof(h2c_PingWrite));
        goto out;
    }
    h2c_ref(cc);
    if (!go(BURROW_FN(Func, h2c_ping_write, pw))) {
        pw->refs.v = 1;
        h2c_ping_write_unref(pw);
        goto out;
    }
    SelectCase cases[4] = {
        BURROW_RECV(c, NULL),
        BURROW_RECV(pw->errc, NULL),
        BURROW_RECV(context_done(ctx), NULL),
        BURROW_RECV(cc->reader_done, NULL),
    };
    switch (chan_select(cases, 4)) {
    case 0:
        err = BURROW_NO_ERROR;
        break;
    case 1:
        err = pw->err;
        break;
    case 2:
        err = h2c_keep_err(cc, context_err(ctx));
        break;
    default:
        /* connection closed */
        err = cc->reader_err;
        break;
    }
    h2c_ping_write_unref(pw);

out:
    /* An ack closes c and takes it out of pings, and nothing else touches
     * it, so once it is out of pings here it can go. */
    sync_mutex_lock(&cc->mu);
    map_del(cc->pings, &key);
    sync_mutex_unlock(&cc->mu);
    chan_free(c);
    return err;
}

/* healthCheck, which read_idle_t runs, with the reference it holds. */
static void h2c_health_check(void *env) {
    h2c_Conn *cc = (h2c_Conn *)env;
    /* We don't need to periodically ping in the health check, because the
     * readLoop of ClientConn will trigger the healthCheck again if there is
     * no frame received. */
    ContextCancelFunc cancel;
    Context ctx =
        context_with_timeout(cc->a, context_background(), cc->ping_timeout, &cancel);
    h2c_vlogf("http2: Transport sending health check");
    Error err = h2c_ping(cc, ctx);
    BURROW_CALLF0(cancel);
    context_release(ctx);
    if (BURROW_FAILED(err)) {
        h2c_vlogf("http2: Transport health check failure: %v", err);
        h2c_close_for_lost_ping(cc);
    } else {
        h2c_vlogf("http2: Transport health check success");
    }
    h2c_unref(cc);
}

/* processPing. */
static Error h2c_process_ping(h2c_Conn *cc, const Http2Frame *f) {
    if (burrow__http2_ping_frame_is_ack(f)) {
        uint64_t key;
        memcpy(&key, f->u.ping.data, sizeof key);
        sync_mutex_lock(&cc->mu);
        /* If ack, notify listener if any. */
        uintptr_t *v = (uintptr_t *)map_get(cc->pings, &key);
        if (v != NULL) {
            chan_close((Chan *)*v);
            map_del(cc->pings, &key);
        }
        if (cc->pending_resets > 0) {
            /* See cleanupWriteRequest. */
            cc->pending_resets = 0;
            cc->rst_stream_pings_blocked = true;
            sync_cond_broadcast(&cc->cond);
        }
        sync_mutex_unlock(&cc->mu);
        return BURROW_NO_ERROR;
    }
    sync_mutex_lock(&cc->wmu);
    Error err = burrow__http2_framer_write_ping(cc->fr, true, f->u.ping.data);
    if (BURROW_OK(err))
        err = bufio_writer_flush(cc->bw);
    sync_mutex_unlock(&cc->wmu);
    return err;
}

/* processPushPromise. We told the peer we don't want them. The spec says:
 * "PUSH_PROMISE MUST NOT be sent if the SETTINGS_ENABLE_PUSH setting of the
 * peer endpoint is set to 0. An endpoint that has set this setting and has
 * received acknowledgement MUST treat the receipt of a PUSH_PROMISE frame as
 * a connection error (Section 5.4.1) of type PROTOCOL_ERROR." */
static Error h2c_process_push_promise(void) {
    return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
}

/* writeStreamReset: a RST_STREAM frame and, with ping, a PING frame with a
 * random payload. */
static void h2c_write_stream_reset(h2c_Conn *cc, uint32_t id, Http2ErrCode code,
                                   bool ping) {
    /* TODO: map err to more interesting error codes, once the HTTP community
     * comes up with some. But currently for RST_STREAM there's no equivalent
     * to GOAWAY frame's debug data, and the error codes are all pretty vague
     * ("cancel"). */
    sync_mutex_lock(&cc->wmu);
    ArenaMark m = error_mark();
    (void)burrow__http2_framer_write_rst_stream(cc->fr, id, code);
    if (ping) {
        Byte payload[8];
        (void)crypto_rand_read(slice_from(payload, 8, 8, TYPE_BYTE), NULL);
        (void)burrow__http2_framer_write_ping(cc->fr, false, payload);
    }
    (void)bufio_writer_flush(cc->bw);
    error_release(m);
    sync_mutex_unlock(&cc->wmu);
}

/* countReadFrameError. */
static void h2c_count_read_frame_error(h2c_Conn *cc, Error err) {
    if (BURROW_FUNC_IS_NIL(cc->conf.count_error) || BURROW_OK(err))
        return;
    Http2ErrCode code;
    if (burrow__http2_error_connection(err, &code))
        h2c_count(cc, "read_frame_conn_error_", code);
    else if (errors_is(err, io_eof))
        h2c_count_name(cc, "read_frame_eof");
    else if (errors_is(err, io_err_unexpected_eof))
        h2c_count_name(cc, "read_frame_unexpected_eof");
    else if (errors_is(err, burrow__http2_err_frame_too_large))
        h2c_count_name(cc, "read_frame_too_large");
    else
        h2c_count_name(cc, "read_frame_other");
}

/* The frame's handler, from run's type switch. */
static Error h2c_process_frame(h2c_Conn *cc, const Http2Frame *f) {
    switch (f->kind) {
    case HTTP2_META_HEADERS_FRAME:
        return h2c_process_headers(cc, f);
    case HTTP2_DATA_FRAME:
        return h2c_process_data(cc, f);
    case HTTP2_GO_AWAY_FRAME:
        return h2c_process_go_away(cc, f);
    case HTTP2_RST_STREAM_FRAME:
        return h2c_process_reset_stream(cc, f);
    case HTTP2_SETTINGS_FRAME:
        return h2c_process_settings(cc, f);
    case HTTP2_PUSH_PROMISE_FRAME:
        return h2c_process_push_promise();
    case HTTP2_WINDOW_UPDATE_FRAME:
        return h2c_process_window_update(cc, f);
    case HTTP2_PING_FRAME:
        return h2c_process_ping(cc, f);
    case HTTP2_HEADERS_FRAME:
    case HTTP2_PRIORITY_FRAME:
    case HTTP2_CONTINUATION_FRAME:
    case HTTP2_PRIORITY_UPDATE_FRAME:
    case HTTP2_UNKNOWN_FRAME:
    default:
        h2c_logf("Transport: unhandled response frame type %s", h2c_frame_type_name(f));
        return BURROW_NO_ERROR;
    }
}

/* clientConnReadLoop.run. The error lasts as long as the connection. */
static Error h2c_run(h2c_Conn *cc) {
    bool got_settings = false;
    Duration read_idle_timeout = cc->read_idle_timeout;
    if (read_idle_timeout != 0) {
        sync_mutex_lock(&cc->mu);
        h2c_timer_start(cc, &cc->read_idle_t, read_idle_timeout,
                        BURROW_FN(Func, h2c_health_check, cc));
        sync_mutex_unlock(&cc->mu);
    }
    Error ret;
    for (;;) {
        ArenaMark m = error_mark();
        Error err = BURROW_NO_ERROR;
        Http2Frame *f = burrow__http2_framer_read_frame(cc->fr, &err);
        if (read_idle_timeout != 0 && cc->read_idle_t != NULL)
            h2c_timer_reset(cc, cc->read_idle_t, read_idle_timeout);
        if (BURROW_FAILED(err))
            h2c_vlogf("http2: Transport readFrame error on conn %p: (%s) %v",
                      (void *)cc,
                      err.vt->self_type != NULL ? type_name(err.vt->self_type)
                                                : BURROW_S("error"),
                      err);
        Http2StreamError se;
        if (BURROW_FAILED(err) && burrow__http2_error_stream(err, &se)) {
            h2c_Stream *cs = h2c_stream_by_id(cc, se.stream_id, false);
            if (cs != NULL) {
                if (BURROW_OK(se.cause))
                    se.cause = cc->fr->err_detail;
                h2c_end_stream_error(
                    cs, burrow__http2_stream_error(se.stream_id, se.code, se.cause));
                h2c_sunref(cs);
            }
            if (f != NULL)
                burrow__http2_frame_free(f);
            error_release(m);
            continue;
        }
        if (BURROW_FAILED(err)) {
            h2c_count_read_frame_error(cc, err);
            ret = h2c_keep_err(cc, err);
            if (f != NULL)
                burrow__http2_frame_free(f);
            error_release(m);
            break;
        }
        if (burrow__http2_verbose_logs()) {
            Str sum = burrow__http2_summarize_frame(cc->a, f);
            log_printf_v("http2: Transport received %s", sum);
            mem_free(cc->a, (void *)(uintptr_t)sum.p, (size_t)sum.len, 1);
        }
        if (!got_settings) {
            if (f->kind != HTTP2_SETTINGS_FRAME) {
                h2c_logf("protocol error: received %s before a SETTINGS frame",
                         h2c_frame_type_name(f));
                ret = burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
                burrow__http2_frame_free(f);
                error_release(m);
                break;
            }
            got_settings = true;
        }

        err = h2c_process_frame(cc, f);
        if (BURROW_FAILED(err)) {
            if (burrow__http2_verbose_logs() &&
                !h2c_same(err, h2c_err_stop_read_loop)) {
                Str sum = burrow__http2_summarize_frame(cc->a, f);
                log_printf_v("http2: Transport conn %p received error from processing "
                             "frame %v: %v",
                             (void *)cc, sum, err);
                mem_free(cc->a, (void *)(uintptr_t)sum.p, (size_t)sum.len, 1);
            }
            ret = h2c_keep_err(cc, err);
            burrow__http2_frame_free(f);
            error_release(m);
            break;
        }
        burrow__http2_frame_free(f);
        error_release(m);
    }
    /* Go leaves the timer to fire on a connection that is gone, where all
     * the health check can do is fail. */
    sync_mutex_lock(&cc->mu);
    h2c_timer_stop(cc, cc->read_idle_t);
    sync_mutex_unlock(&cc->mu);
    return ret;
}

/* isEOFOrNetReadError. */
static bool h2c_is_eof_or_net_read_error(Error err) {
    if (h2c_same(err, io_eof))
        return true;
    if (err.vt != NULL && err.vt->self_type == TYPE_NET_OP_ERROR) {
        const NetOpError *oe = (const NetOpError *)err.data;
        return str_eq(oe->op, BURROW_S("read"));
    }
    return false;
}

/* clientConnReadLoop.cleanup. */
static void h2c_cleanup(h2c_Conn *cc) {
    sync_mutex_lock(&cc->mu);
    h2c_timer_stop(cc, cc->idle_timer);

    /* Close any response bodies if the server closes prematurely.
     * TODO: also do this if we've written the headers but not gotten a
     * response yet. */
    Error err = cc->reader_err;
    if (cc->has_go_away && h2c_is_eof_or_net_read_error(err)) {
        Http2GoAwayError ge = {cc->go_away_last_stream_id, cc->go_away_err_code,
                               cc->go_away_debug};
        sync_mutex_lock(&cc->emu);
        err = burrow__http2_go_away_error(arena_allocator(&cc->earena), ge);
        sync_mutex_unlock(&cc->emu);
    } else if (h2c_same(err, io_eof)) {
        err = io_err_unexpected_eof;
    }
    cc->closed = true;

    /* If the connection has never been used, and has been open for only a
     * short time, leave it in the connection pool for a little while. This
     * avoids a situation where new connections are constantly created,
     * added to the pool, fail, and are removed from the pool, without any
     * error being surfaced to the user. */
    Duration unused_wait_time = 5 * TIME_SECOND;
    if (cc->idle_timeout > 0 && unused_wait_time > cc->idle_timeout)
        unused_wait_time = cc->idle_timeout;
    Duration idle_time = time_since(cc->last_active);
    if (sync_atomic_uint32_load(&cc->atomic_reused) == 0 &&
        idle_time < unused_wait_time && !cc->closed_on_idle &&
        !sync_atomic_bool_load(&cc->t->closed)) {
        h2c_timer_start(cc, &cc->dead_t, unused_wait_time - idle_time,
                        BURROW_FN(Func, h2c_on_dead, cc));
        cc->idle_timer = cc->dead_t;
    } else {
        sync_mutex_unlock(&cc->mu); /* avoid any deadlocks in MarkDead */
        h2c_mark_dead(cc);
        sync_mutex_lock(&cc->mu);
    }

    const void *k_;
    void *v_;
    H2C_EACH_STREAM(cc, cs) {
        /* A stream whose peer_closed is closed was closed by the server
         * before it closed the conn, so there is no need to interrupt it. */
        if (!chan_try_recv(cs->peer_closed, NULL, NULL))
            h2c_abort_stream_locked(cs, err);
    }
    sync_cond_broadcast(&cc->cond);
    sync_mutex_unlock(&cc->mu);

    if (!cc->seen_settings) {
        /* If we have a pending request that wants extended CONNECT, let it
         * continue and fail with the connection error. */
        cc->extended_connect_allowed = true;
        chan_close(cc->seen_settings_chan);
    }
    chan_close(cc->reader_done);
    h2c_close_conn(cc);
}

/* readLoop, which has a reference of its own. */
static void h2c_read_loop(void *env) {
    h2c_Conn *cc = (h2c_Conn *)env;
    cc->reader_err = h2c_run(cc);
    Http2ErrCode code;
    if (burrow__http2_error_connection(cc->reader_err, &code)) {
        /* Into bw, with no flush, as Go does. */
        sync_mutex_lock(&cc->wmu);
        ArenaMark m = error_mark();
        (void)burrow__http2_framer_write_go_away(cc->fr, 0, code,
                                                 slice_from(NULL, 0, 0, TYPE_BYTE));
        error_release(m);
        sync_mutex_unlock(&cc->wmu);
    }
    h2c_cleanup(cc);
    h2c_unref(cc);
}

/* --------------------------------------------------------------- streams */

static void h2c_gzip_free(h2c_GzipReader *gz);

/* The last reference to the stream has gone: the round trip, the response,
 * the stream map and doRequest are all done with it. */
static void h2c_stream_free(h2c_Stream *cs) {
    h2c_Conn *cc = cs->cc;
    if (cs->res != NULL && !cs->res_taken)
        http_response_free(cs->res);
    if (cs->gz != NULL)
        h2c_gzip_free(cs->gz);
    burrow__http2_pipe_free(&cs->buf_pipe);
    burrow__http2_data_buffer_free(&cs->dbuf);
    chan_free(cs->abort);
    chan_free(cs->peer_closed);
    chan_free(cs->donec);
    chan_free(cs->on100);
    chan_free(cs->resp_header_recv);
    chan_free(cs->req_body_closed);
    arena_free(&cs->tarena);
    arena_free(&cs->earena);
    mem_free(cc->a, cs, sizeof *cs, _Alignof(h2c_Stream));
    h2c_unref(cc);
}

/* The clientStream roundTrip makes, with one reference, the caller's. */
/* body stands in for req->body, so a retry can send the one GetBody gave
 * without copying the request. */
static h2c_Stream *h2c_new_stream(h2c_Conn *cc, HttpRequest *req, IoReadCloser body) {
    Alloc *a = cc->a;
    h2c_Stream *cs = (h2c_Stream *)mem_alloc(a, sizeof *cs, _Alignof(h2c_Stream));
    if (cs == NULL)
        return NULL;
    h2c_ref(cc);
    cs->cc = cc;
    cs->refs.v = 1;
    cs->req = req;
    cs->ctx = http_request_context(req);
    cs->trace = httptrace_context_client_trace(cs->ctx);
    cs->is_head = str_eq(req->method, BURROW_S("HEAD"));
    cs->req_body = body;
    /* actualContentLength, of the request with body in it. */
    if (body.vt == NULL || body.vt == http_no_body.vt)
        cs->req_body_content_length = 0;
    else if (req->content_length != 0)
        cs->req_body_content_length = req->content_length;
    else
        cs->req_body_content_length = -1;
    cs->bytes_remain = -1;
    arena_init(&cs->tarena, a, 0);
    arena_init(&cs->earena, a, 0);
    cs->abort = chan_make(a, TYPE_BOOL, 0);
    cs->peer_closed = chan_make(a, TYPE_BOOL, 0);
    cs->donec = chan_make(a, TYPE_BOOL, 0);
    cs->resp_header_recv = chan_make(a, TYPE_BOOL, 0);
    cs->req_body_closed = chan_make(a, TYPE_BOOL, 0);
    if (cs->abort == NULL || cs->peer_closed == NULL || cs->donec == NULL ||
        cs->resp_header_recv == NULL || cs->req_body_closed == NULL) {
        h2c_sunref(cs);
        return NULL;
    }
    bool disable_compression = cc->t->t1 != NULL && cc->t->t1->disable_compression;
    cs->requested_gzip =
        h2c_is_request_gzip(req->method, req->header, disable_compression);
    return cs;
}

/* The goroutine closeReqBodyLocked starts. */
static void h2c_close_req_body(void *env) {
    h2c_Stream *cs = (h2c_Stream *)env;
    ArenaMark m = error_mark();
    (void)cs->req_body.vt->closer.close(cs->req_body.data);
    error_release(m);
    chan_close(cs->req_body_closed);
    h2c_sunref(cs);
}

/* closeReqBodyLocked. */
static void h2c_close_req_body_locked(h2c_Stream *cs) {
    if (cs->req_body_closing)
        return;
    cs->req_body_closing = true;
    h2c_sref(cs);
    if (!go(BURROW_FN(Func, h2c_close_req_body, cs))) {
        /* No goroutine to be had, so close it here and now. */
        h2c_close_req_body(cs);
    }
}

/* abortRequestBodyWrite. */
static void h2c_abort_request_body_write(h2c_Stream *cs) {
    h2c_Conn *cc = cs->cc;
    sync_mutex_lock(&cc->mu);
    if (cs->req_body.vt != NULL && !cs->req_body_closing) {
        h2c_close_req_body_locked(cs);
        sync_cond_broadcast(&cc->cond);
    }
    sync_mutex_unlock(&cc->mu);
}

/* Whether the connection is to close once it has no streams. */
static bool h2c_close_on_idle_locked(h2c_Conn *cc) {
    return cc->single_use || cc->do_not_reuse ||
           (cc->t->t1 != NULL && cc->t->t1->disable_keep_alives) || cc->has_go_away;
}

static const Type h2c_stream_id_key_desc = {
    {(const Byte *)"streamIDHookKey", 15},
    {(const Byte *)"net/http/internal/http2", 23},
    KIND_INT,
    (uint32_t)sizeof(Int),
    (uint16_t)_Alignof(Int),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68327369U, /* "h2si" */
    NULL,
};

static const Int h2c_stream_id_key_v = 0;

const Any burrow__http2_stream_id_hook_key = {&h2c_stream_id_key_desc,
                                              (void *)(uintptr_t)&h2c_stream_id_key_v};

/* addStreamLocked, which gives the stream map a reference. */
static Error h2c_add_stream_locked(h2c_Conn *cc, h2c_Stream *cs) {
    uint32_t id = cc->next_stream_id;
    uintptr_t v = (uintptr_t)cs;
    if (!map_set(cc->streams, &id, &v))
        return burrow_err_out_of_memory;
    h2c_sref(cs);
    (void)burrow__http2_outflow_add(&cs->flow, (int32_t)cc->initial_window_size);
    burrow__http2_outflow_set_conn_flow(&cs->flow, &cc->flow);
    burrow__http2_inflow_init(&cs->inflow, cc->initial_stream_recv_window_size);
    cs->id = id;
    cc->next_stream_id += 2;
    Any hook = context_value(cs->ctx, burrow__http2_stream_id_hook_key);
    if (hook.data != NULL)
        sync_atomic_int64_store((SyncAtomicInt64 *)hook.data, (int64_t)id);
    return BURROW_NO_ERROR;
}

/* forgetStreamID, which drops the stream map's reference. */
static void h2c_forget_stream_id(h2c_Conn *cc, uint32_t id) {
    sync_mutex_lock(&cc->mu);
    h2c_Stream *cs = NULL;
    uintptr_t *v = (uintptr_t *)map_get(cc->streams, &id);
    if (v != NULL) {
        cs = (h2c_Stream *)*v;
        map_del(cc->streams, &id);
    }
    cc->last_active = time_now();
    if (map_len(cc->streams) == 0 && cc->idle_timer != NULL) {
        h2c_timer_reset(cc, cc->idle_timer, cc->idle_timeout);
        cc->last_idle = time_now();
    }
    /* Wake up checkResetOrDone via clientStream.awaitFlowControl and
     * wake up RoundTrip if there is a pending request. */
    sync_cond_broadcast(&cc->cond);

    bool close_conn = false;
    if (h2c_close_on_idle_locked(cc) && cc->streams_reserved == 0 &&
        map_len(cc->streams) == 0) {
        bool single_use = cc->single_use;
        uint32_t max_stream = cc->next_stream_id - 2;
        h2c_vlogf(
            "http2: Transport closing idle conn %p (forSingleUse=%v, maxStream=%v)",
            (void *)cc, single_use, max_stream);
        cc->closed = true;
        close_conn = true;
    }
    sync_mutex_unlock(&cc->mu);
    if (close_conn)
        h2c_close_conn(cc);
    if (cs != NULL)
        h2c_sunref(cs);
}

/* awaitOpenSlotForStreamLocked. */
static Error h2c_await_open_slot_for_stream_locked(h2c_Conn *cc, h2c_Stream *cs) {
    for (;;) {
        if (cc->closed && cc->next_stream_id == 1 && cc->streams_reserved == 0) {
            /* This is the very first request sent to this connection. Return
             * a fatal error which aborts the retry loop. */
            return burrow__http2_err_client_conn_not_established;
        }
        cc->last_active = time_now();
        if (cc->closed || !h2c_can_take_new_request_locked(cc))
            return burrow__http2_err_client_conn_unusable;
        memset(&cc->last_idle, 0, sizeof cc->last_idle);
        if (h2c_current_request_count_locked(cc) < (Int)cc->max_concurrent_streams)
            return BURROW_NO_ERROR;
        cc->pending_requests++;
        sync_cond_wait(&cc->cond);
        cc->pending_requests--;
        if (cs->aborted)
            return cs->abort_err;
    }
}

/* -------------------------------------------------------- writing requests */

/* writeHeader, as the encoder's callback, under wmu. */
static void h2c_write_header(void *env, Str name, Str value) {
    h2c_Conn *cc = (h2c_Conn *)env;
    h2c_vlogf("http2: Transport encoding header %q = %q", name, value);
    HpackHeaderField hf;
    memset(&hf, 0, sizeof hf);
    hf.name = name;
    hf.value = value;
    ArenaMark m = error_mark();
    (void)burrow__hpack_encoder_write_field(cc->henc, hf);
    error_release(m);
}

/* writeHeaders: hdrs as a HEADERS frame and the CONTINUATION frames after
 * it, under wmu. */
static Error h2c_write_headers(h2c_Conn *cc, uint32_t id, bool end_stream,
                               Int max_frame_size, Slice hdrs) {
    bool first = true; /* first frame written (HEADERS is first, then CONTINUATION) */
    ArenaMark m = error_mark();
    while (hdrs.len > 0 && BURROW_OK(cc->werr)) {
        Slice chunk = hdrs;
        if (chunk.len > max_frame_size)
            chunk = slice_sub(chunk, 0, max_frame_size);
        hdrs = slice_sub(hdrs, chunk.len, hdrs.len);
        bool end_headers = hdrs.len == 0;
        if (first) {
            Http2HeadersFrameParam p;
            memset(&p, 0, sizeof p);
            p.stream_id = id;
            p.block_fragment = chunk;
            p.end_stream = end_stream;
            p.end_headers = end_headers;
            (void)burrow__http2_framer_write_headers(cc->fr, p);
            first = false;
        } else {
            (void)burrow__http2_framer_write_continuation(cc->fr, id, end_headers,
                                                          chunk);
        }
    }
    (void)bufio_writer_flush(cc->bw);
    error_release(m);
    return cc->werr;
}

/* encodeAndWriteHeaders. */
static Error h2c_encode_and_write_headers(h2c_Stream *cs) {
    h2c_Conn *cc = cs->cc;
    HttpRequest *req = cs->req;
    sync_mutex_lock(&cc->wmu);

    /* If the request was canceled while waiting for cc.mu, just quit. */
    Error err = BURROW_NO_ERROR;
    if (chan_try_recv(cs->abort, NULL, NULL)) {
        err = cs->abort_err;
    } else if (chan_try_recv(context_done(cs->ctx), NULL, NULL)) {
        err = context_err(cs->ctx);
    }
    if (BURROW_FAILED(err)) {
        sync_mutex_unlock(&cc->wmu);
        return err;
    }

    /* Encode headers.
     *
     * We send: HEADERS{1}, CONTINUATION{0,} + DATA{0,} (DATA is sent by
     * writeRequestBody below, along with any Trailers, again in form
     * HEADERS{1}, CONTINUATION{0,}). */
    bytes_buffer_reset(&cc->hbuf);
    Arena tmp;
    arena_init(&tmp, cc->a, 0);
    Http2EncodeHeadersParam p;
    memset(&p, 0, sizeof p);
    p.ctx = cs->ctx;
    p.url = req->url;
    p.method = req->method;
    p.host = req->host;
    p.header = req->header;
    p.trailer = req->trailer;
    p.actual_content_length = cs->req_body_content_length;
    p.add_gzip_header = cs->requested_gzip;
    p.peer_max_header_list_size = cc->peer_max_header_list_size;
    p.default_user_agent = BURROW_S(H2C_DEFAULT_USER_AGENT);
    Http2EncodeHeadersResult res;
    err = burrow__http2_encode_headers(arena_allocator(&tmp), &p, h2c_write_header, cc,
                                       &res);
    if (BURROW_FAILED(err)) {
        err = fmt_errorf_v("http2: %w", err);
    } else {
        Slice hdrs = bytes_buffer_bytes(&cc->hbuf);
        /* Write the request. */
        bool end_stream = !res.has_body && !res.has_trailers;
        cs->sent_headers = true;
        err = h2c_write_headers(cc, cs->id, end_stream, (Int)cc->max_frame_size, hdrs);
        httptrace_client_trace_wrote_headers(cs->trace);
    }
    arena_free(&tmp);
    sync_mutex_unlock(&cc->wmu);
    return err;
}

/* frameScratchBufferLen returns the length of a buffer to use for outgoing
 * request body DATA frames: a frame's worth, but no more than the body when
 * its length is known, plus one byte to see that the body has ended. */
static Int h2c_frame_scratch_buffer_len(const h2c_Stream *cs, Int max_frame_size) {
    int64_t n = max_frame_size < H2C_MAX_SCRATCH ? max_frame_size : H2C_MAX_SCRATCH;
    int64_t cl = cs->req_body_content_length;
    if (cl != -1 && cl + 1 < n) {
        /* Don't allocate a big buffer if we know the body is small. */
        n = cl + 1;
    }
    if (n < 1)
        return 1;
    return (Int)n;
}

/* awaitFlowControl waits for [1, min(maxBytes, cc.cs.maxFrameSize)] flow
 * control tokens from the server. It returns either the non-zero number of
 * tokens taken or an error if the stream is dead. */
static int32_t h2c_await_flow_control(h2c_Stream *cs, Int max_bytes, Error *err) {
    h2c_Conn *cc = cs->cc;
    Chan *ctx_done = context_done(cs->ctx);
    sync_mutex_lock(&cc->mu);
    for (;;) {
        if (cc->closed) {
            *err = burrow__http2_err_client_conn_closed;
            break;
        }
        if (cs->req_body_closing) {
            *err = burrow__http2_err_stop_req_body_write;
            break;
        }
        if (cs->aborted) {
            *err = cs->abort_err;
            break;
        }
        if (chan_try_recv(ctx_done, NULL, NULL)) {
            *err = context_err(cs->ctx);
            break;
        }
        int32_t a = burrow__http2_outflow_available(&cs->flow);
        if (a > 0) {
            int32_t take = a;
            if ((Int)take > max_bytes)
                take = (int32_t)max_bytes;
            if (take > (int32_t)cc->max_frame_size)
                take = (int32_t)cc->max_frame_size;
            burrow__http2_outflow_take(&cs->flow, take);
            sync_mutex_unlock(&cc->mu);
            *err = BURROW_NO_ERROR;
            return take;
        }
        sync_cond_wait(&cc->cond);
    }
    sync_mutex_unlock(&cc->mu);
    return 0;
}

/* encodeTrailers, under wmu. The block is in hbuf. */
static Error h2c_encode_trailers(h2c_Conn *cc, HttpHeader trailer, Slice *out) {
    bytes_buffer_reset(&cc->hbuf);

    uint64_t hl_size = 0;
    MapIter it = map_iter(trailer);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        const Slice *vv = (const Slice *)v;
        for (Int i = 0; i < vv->len; i++) {
            HpackHeaderField hf;
            memset(&hf, 0, sizeof hf);
            hf.name = *(const Str *)k;
            hf.value = ((const Str *)vv->p)[i];
            hl_size += (uint64_t)burrow__hpack_header_field_size(hf);
        }
    }
    if (hl_size > cc->peer_max_header_list_size)
        return burrow__http2_err_request_header_list_size;

    Arena tmp;
    arena_init(&tmp, cc->a, 0);
    it = map_iter(trailer);
    while (map_next(&it, &k, &v)) {
        bool ascii = false;
        Str low_key =
            burrow__http_ascii_to_lower(arena_allocator(&tmp), *(const Str *)k, &ascii);
        if (!ascii) {
            /* Skip writing invalid headers. Per RFC 7540, Section 8.1.2,
             * header field names have to be ASCII characters (just as in
             * HTTP/1.x). */
            continue;
        }
        const Slice *vv = (const Slice *)v;
        for (Int i = 0; i < vv->len; i++)
            h2c_write_header(cc, low_key, ((const Str *)vv->p)[i]);
    }
    arena_free(&tmp);
    *out = bytes_buffer_bytes(&cc->hbuf);
    return BURROW_NO_ERROR;
}

/* writeRequestBody. */
static Error h2c_write_request_body(h2c_Stream *cs) {
    h2c_Conn *cc = cs->cc;
    HttpRequest *req = cs->req;
    IoReadCloser body = cs->req_body;
    bool sent_end = false; /* whether we sent the final DATA frame w/ END_STREAM */

    bool has_trailers = req->trailer != NULL;
    int64_t remain_len = cs->req_body_content_length;
    bool has_content_len = remain_len != -1;

    sync_mutex_lock(&cc->mu);
    Int max_frame_size = (Int)cc->max_frame_size;
    sync_mutex_unlock(&cc->mu);

    /* Scratch buffer for reading into & writing from. */
    Int scratch_len = h2c_frame_scratch_buffer_len(cs, max_frame_size);
    Byte *buf = (Byte *)mem_alloc_nozero(cc->a, (size_t)scratch_len, 1);
    if (buf == NULL)
        return burrow_err_out_of_memory;
    Slice whole = slice_from(buf, scratch_len, scratch_len, TYPE_BYTE);

    Error err = BURROW_NO_ERROR;
    bool saw_eof = false;
    while (!saw_eof) {
        Error rerr = BURROW_NO_ERROR;
        Int n = body.vt->reader.read(body.data, whole, &rerr);
        if (has_content_len) {
            remain_len -= n;
            if (remain_len == 0 && BURROW_OK(rerr)) {
                /* The request body's Content-Length was predeclared and we
                 * just finished reading it all, but the underlying
                 * io.Reader returned the final chunk with a nil error (which
                 * is one of the two valid things a Reader can do at EOF).
                 * Because we'd prefer to send the END_STREAM bit early,
                 * double-check that we're actually at EOF. Subsequent reads
                 * should return (0, EOF) at this point. If either value is
                 * different, we return an error in one of two ways below. */
                Byte scratch[1];
                Int n1 = body.vt->reader.read(
                    body.data, slice_from(scratch, 1, 1, TYPE_BYTE), &rerr);
                remain_len -= n1;
            }
            if (remain_len < 0) {
                err = burrow__http2_err_req_body_too_long;
                goto out;
            }
        }
        if (BURROW_FAILED(rerr)) {
            sync_mutex_lock(&cc->mu);
            bool body_closed = cs->req_body_closing;
            sync_mutex_unlock(&cc->mu);
            if (body_closed) {
                err = burrow__http2_err_stop_req_body_write;
                goto out;
            }
            if (!h2c_same(rerr, io_eof)) {
                err = rerr;
                goto out;
            }
            saw_eof = true;
        }

        Slice remain = slice_sub(whole, 0, n);
        while (remain.len > 0) {
            int32_t allowed = h2c_await_flow_control(cs, remain.len, &err);
            if (BURROW_FAILED(err))
                goto out;
            sync_mutex_lock(&cc->wmu);
            Slice data = slice_sub(remain, 0, allowed);
            remain = slice_sub(remain, allowed, remain.len);
            sent_end = saw_eof && remain.len == 0 && !has_trailers;
            err = burrow__http2_framer_write_data(cc->fr, cs->id, sent_end, data);
            if (BURROW_OK(err))
                err = bufio_writer_flush(cc->bw);
            sync_mutex_unlock(&cc->wmu);
            if (BURROW_FAILED(err))
                goto out;
        }
    }

    if (sent_end) {
        /* Already sent END_STREAM (which implies we have no trailers) and
         * flushed, because currently all WriteData frames above get a flush.
         * So we're done. */
        goto out;
    }

    /* Since the RoundTrip contract permits the caller to "mutate or reuse" a
     * request after the Response's Body is closed, verify that this hasn't
     * happened before accessing the trailers. */
    sync_mutex_lock(&cc->mu);
    HttpHeader trailer = req->trailer;
    if (cs->aborted)
        err = cs->abort_err;
    sync_mutex_unlock(&cc->mu);
    if (BURROW_FAILED(err))
        goto out;

    sync_mutex_lock(&cc->wmu);
    Slice trls = slice_from(NULL, 0, 0, TYPE_BYTE);
    if (trailer != NULL && map_len(trailer) > 0) {
        err = h2c_encode_trailers(cc, trailer, &trls);
        if (BURROW_FAILED(err)) {
            sync_mutex_unlock(&cc->wmu);
            goto out;
        }
    }

    /* Two ways to send END_STREAM: either with trailers, or with an empty
     * DATA frame. */
    if (trls.len > 0)
        err = h2c_write_headers(cc, cs->id, true, max_frame_size, trls);
    else
        err = burrow__http2_framer_write_data(cc->fr, cs->id, true,
                                              slice_from(NULL, 0, 0, TYPE_BYTE));
    Error ferr = bufio_writer_flush(cc->bw);
    if (BURROW_FAILED(ferr) && BURROW_OK(err))
        err = ferr;
    sync_mutex_unlock(&cc->wmu);

out:
    mem_free(cc->a, buf, (size_t)scratch_len, 1);
    return err;
}

/* isConnectionCloseRequest reports whether req should use its own connection
 * for a single request and then close the connection. */
static bool h2c_is_connection_close_request(const HttpRequest *req) {
    Slice v = http_header_values(req->header, BURROW_S("Connection"));
    return req->close || burrow__httpguts_header_values_contains_token(
                             (const Str *)v.p, v.len, BURROW_S("close"));
}

/* writeRequest sends a request.
 *
 * It returns no error after the request is written, the response read, and
 * the request stream is half-closed by the peer.
 *
 * It returns an error if the request ends otherwise. If the returned error
 * is a StreamError, its code may be used in resetting the stream. */
static Error h2c_write_request(h2c_Stream *cs) {
    h2c_Conn *cc = cs->cc;
    HttpRequest *req = cs->req;
    HttpTransport *t1 = cc->t->t1;
    Chan *ctx_done = context_done(cs->ctx);

    /* Wait for setting frames to be received, a server can change this value
     * later, but we just wait for the first settings frame. */
    bool is_extended_connect =
        str_eq(req->method, BURROW_S("CONNECT")) &&
        http_header_get(req->header, BURROW_S(":protocol")).len != 0;
    if (is_extended_connect) {
        SelectCase sc[] = {
            BURROW_RECV(ctx_done, NULL),
            BURROW_RECV(cc->seen_settings_chan, NULL),
        };
        if (chan_select(sc, 2) == 0)
            return context_err(cs->ctx);
        sync_mutex_lock(&cc->mu);
        bool allowed = cc->extended_connect_allowed;
        sync_mutex_unlock(&cc->mu);
        if (!allowed)
            return burrow__http2_err_extended_connect_not_supported;
    }

    /* Room for the 100-continue signal, made before the stream is in the map
     * so that the read loop only ever sees it set. */
    Duration continue_timeout = t1 != NULL ? t1->expect_continue_timeout : 0;
    if (continue_timeout != 0) {
        Slice ex = http_header_values(req->header, BURROW_S("Expect"));
        if (!burrow__httpguts_header_values_contains_token((const Str *)ex.p, ex.len,
                                                           BURROW_S("100-continue"))) {
            continue_timeout = 0;
        } else {
            cs->on100 = chan_make(cc->a, TYPE_BOOL, 1);
            if (cs->on100 == NULL)
                return burrow_err_out_of_memory;
        }
    }

    /* Acquire the new-request lock by writing to reqHeaderMu. This lock
     * guards the critical section covering allocating a new stream ID
     * (requires mu) and creating the stream (requires wmu). */
    bool token = true;
    SelectCase hc[] = {
        BURROW_SEND(cc->req_header_mu, &token),
        BURROW_RECV(ctx_done, NULL),
    };
    if (chan_select(hc, 2) == 1)
        return context_err(cs->ctx);

    sync_mutex_lock(&cc->mu);
    h2c_timer_stop(cc, cc->idle_timer);
    h2c_decr_stream_reservations_locked(cc);
    Error err = h2c_await_open_slot_for_stream_locked(cc, cs);
    if (BURROW_OK(err))
        err = h2c_add_stream_locked(cc, cs); /* assigns stream ID */
    if (BURROW_FAILED(err)) {
        sync_mutex_unlock(&cc->mu);
        (void)chan_recv(cc->req_header_mu, NULL);
        return err;
    }
    if (h2c_is_connection_close_request(req))
        cc->do_not_reuse = true;
    sync_mutex_unlock(&cc->mu);

    /* Past this point (where we send request headers), it is possible for
     * RoundTrip to return successfully. Since the RoundTrip contract permits
     * the caller to "mutate or reuse" the Request after closing the
     * Response's Body, we must take care when referencing the Request from
     * here on. */
    err = h2c_encode_and_write_headers(cs);
    (void)chan_recv(cc->req_header_mu, NULL);
    if (BURROW_FAILED(err))
        return err;

    bool has_body = cs->req_body_content_length != 0;
    if (!has_body) {
        cs->sent_end_stream = true;
    } else {
        if (continue_timeout != 0) {
            httptrace_client_trace_wait100_continue(cs->trace);
            TimeTimer *timer = time_new_timer(cc->a, continue_timeout);
            if (timer == NULL) {
                err = burrow_err_out_of_memory;
            } else {
                SelectCase sc[] = {
                    BURROW_RECV(time_timer_c(timer), NULL),
                    BURROW_RECV(cs->on100, NULL),
                    BURROW_RECV(cs->abort, NULL),
                    BURROW_RECV(ctx_done, NULL),
                };
                switch (chan_select(sc, 4)) {
                case 2:
                    err = cs->abort_err;
                    break;
                case 3:
                    err = context_err(cs->ctx);
                    break;
                default:
                    break;
                }
                (void)time_timer_stop(timer);
                time_timer_free(timer);
            }
            if (BURROW_FAILED(err)) {
                httptrace_client_trace_wrote_request(cs->trace,
                                                     (HttptraceWroteRequestInfo){err});
                return err;
            }
        }

        err = h2c_write_request_body(cs);
        if (BURROW_FAILED(err)) {
            if (!h2c_same(err, burrow__http2_err_stop_req_body_write)) {
                httptrace_client_trace_wrote_request(cs->trace,
                                                     (HttptraceWroteRequestInfo){err});
                return err;
            }
        } else {
            cs->sent_end_stream = true;
        }
    }

    httptrace_client_trace_wrote_request(cs->trace, (HttptraceWroteRequestInfo){err});

    TimeTimer *timer = NULL;
    Chan *resp_header_timer = NULL;
    Chan *resp_header_recv = NULL;
    Duration d = t1 != NULL ? t1->response_header_timeout : 0;
    if (d != 0) {
        timer = time_new_timer(cc->a, d);
        if (timer == NULL)
            return burrow_err_out_of_memory;
        resp_header_timer = time_timer_c(timer);
        resp_header_recv = cs->resp_header_recv;
    }
    /* Wait until the peer half-closes its end of the stream, or until the
     * request is aborted (via context, error, or otherwise), whichever comes
     * first. */
    for (;;) {
        SelectCase sc[] = {
            BURROW_RECV(cs->peer_closed, NULL),  BURROW_RECV(resp_header_timer, NULL),
            BURROW_RECV(resp_header_recv, NULL), BURROW_RECV(cs->abort, NULL),
            BURROW_RECV(ctx_done, NULL),
        };
        Int i = chan_select(sc, 5);
        if (i == 2) {
            resp_header_recv = NULL;
            resp_header_timer = NULL; /* keep waiting for END_STREAM */
            continue;
        }
        switch (i) {
        case 0:
            err = BURROW_NO_ERROR;
            break;
        case 1:
            err = burrow__http_timeout_error(
                error_allocator(), BURROW_S("http2: timeout awaiting response "
                                            "headers"));
            break;
        case 3:
            err = cs->abort_err;
            break;
        default:
            err = context_err(cs->ctx);
            break;
        }
        break;
    }
    if (timer != NULL) {
        (void)time_timer_stop(timer);
        time_timer_free(timer);
    }
    return err;
}

/* cleanupWriteRequest performs post-request tasks.
 *
 * If err (the result of writeRequest) is an error and the stream is not
 * closed, cleanupWriteRequest will send a reset to the peer. */
static void h2c_cleanup_write_request(h2c_Stream *cs, Error err) {
    h2c_Conn *cc = cs->cc;

    if (cs->id == 0) {
        /* We were canceled before creating the stream, so return our
         * reservation. */
        h2c_decr_stream_reservations(cc);
    }

    /* TODO: write h12Compare test showing whether
     * Request.Body is closed by the Transport,
     * and in multiple cases: server replies <=299 and >299
     * while still writing request body. */
    sync_mutex_lock(&cc->mu);
    bool must_close_body = false;
    if (cs->req_body.vt != NULL && !cs->req_body_closing) {
        must_close_body = true;
        cs->req_body_closing = true;
    }
    bool body_closing = cs->req_body_closing;
    bool close_on_idle = h2c_close_on_idle_locked(cc);
    /* Have we read any frames from the connection since sending this
     * request? */
    bool read_since_stream = cc->read_before_stream_id > cs->id;
    sync_mutex_unlock(&cc->mu);
    if (must_close_body) {
        ArenaMark m = error_mark();
        (void)cs->req_body.vt->closer.close(cs->req_body.data);
        error_release(m);
        chan_close(cs->req_body_closed);
    }
    if (body_closing)
        (void)chan_recv(cs->req_body_closed, NULL);

    if (BURROW_FAILED(err) && cs->sent_end_stream) {
        /* If the connection is closed immediately after the response is
         * read, we may be aborted before finishing up here. If the stream
         * was closed cleanly on both sides, there is no error. */
        if (chan_try_recv(cs->peer_closed, NULL, NULL))
            err = BURROW_NO_ERROR;
    }
    if (BURROW_FAILED(err)) {
        h2c_abort_stream(cs, err); /* possibly redundant, but harmless */
        if (cs->sent_headers) {
            Http2StreamError se;
            if (burrow__http2_error_stream(err, &se)) {
                if (!h2c_same(se.cause, burrow__http2_err_from_peer))
                    h2c_write_stream_reset(cc, cs->id, se.code, false);
            } else {
                /* We're cancelling an in-flight request.
                 *
                 * This could be due to the server becoming unresponsive. To
                 * avoid sending too many requests on a dead connection, if
                 * we haven't read any frames from the connection since
                 * sending this request, we let it continue to consume a
                 * concurrency slot until we can confirm the server is still
                 * responding. We do this by sending a PING frame along with
                 * the RST_STREAM. */
                bool ping = false;
                if (!close_on_idle && !read_since_stream) {
                    sync_mutex_lock(&cc->mu);
                    /* rstStreamPingsBlocked works around a gRPC behavior:
                     * see comment on the field for details. */
                    if (!cc->rst_stream_pings_blocked) {
                        if (cc->pending_resets == 0)
                            ping = true;
                        cc->pending_resets++;
                    }
                    sync_mutex_unlock(&cc->mu);
                }
                h2c_write_stream_reset(cc, cs->id, HTTP2_ERR_CODE_CANCEL, ping);
            }
        }
        burrow__http2_pipe_close_with_error(&cs->buf_pipe,
                                            err); /* no-op if already closed */
    } else {
        if (cs->sent_headers && !cs->sent_end_stream)
            h2c_write_stream_reset(cc, cs->id, HTTP2_ERR_CODE_NO, false);
        burrow__http2_pipe_close_with_error(&cs->buf_pipe,
                                            burrow__http_err_request_canceled);
    }
    if (cs->id != 0)
        h2c_forget_stream_id(cc, cs->id);

    sync_mutex_lock(&cc->wmu);
    bool werr = BURROW_FAILED(cc->werr);
    sync_mutex_unlock(&cc->wmu);
    if (werr)
        h2c_close_for_error(cc, burrow__http2_err_client_conn_force_closed);

    /* Nothing calls the trace once the round trip is over. */
    sync_mutex_lock(&cs->tmu);
    cs->trace = NULL;
    sync_mutex_unlock(&cs->tmu);
    chan_close(cs->donec);
}

/* doRequest runs for the duration of the request lifetime. It sends the
 * request and performs post-request cleanup (closing Request.Body, etc.). */
static void h2c_do_request(void *env) {
    h2c_Stream *cs = (h2c_Stream *)env;
    ArenaMark m = error_mark();
    Error err = h2c_write_request(cs);
    h2c_cleanup_write_request(cs, err);
    error_release(m);
    h2c_sunref(cs);
}

/* ---------------------------------------------------------------- roundTrip */

/* The response's on_free, which holds the round trip's reference. Closing a
 * body that is still open hands back its flow control, and the wait on donec
 * means the request is no longer in use once the response is gone. */
static void h2c_response_on_free(void *env) {
    h2c_Stream *cs = (h2c_Stream *)env;
    if (!cs->body_closed) {
        ArenaMark m = error_mark();
        (void)h2c_body_close(cs);
        error_release(m);
    }
    (void)chan_recv(cs->donec, NULL);
    h2c_sunref(cs);
}

/* The rest of a round trip that has its response headers. Takes the
 * caller's reference, which goes to the response or is dropped. */
static HttpResponse *h2c_handle_response_headers(h2c_Stream *cs, Error *err) {
    HttpResponse *res = cs->res;
    if (res->status_code > 299) {
        /* On error or status code 3xx, 4xx, 5xx, etc abort any ongoing
         * write, assuming that the server doesn't care about our request
         * body. If the server replied with 1xx or 2xx, however, then assume
         * the server DOES potentially want our body (e.g. full-duplex
         * streaming: golang.org/issue/13444). If it turns out the server
         * doesn't, they'll RST_STREAM us soon enough. This is a heuristic to
         * avoid adding knobs to Transport. Hopefully we can keep it. */
        h2c_abort_request_body_write(cs);
    }
    if (res->body.vt == http_no_body.vt && cs->req_body_content_length == 0) {
        /* If there isn't a request or response body still being written,
         * then wait for the stream to be closed before RoundTrip returns. */
        SelectCase sc[] = {
            BURROW_RECV(cs->donec, NULL),
            BURROW_RECV(context_done(cs->ctx), NULL),
        };
        if (chan_select(sc, 2) == 1) {
            *err = error_retain(error_allocator(), context_err(cs->ctx));
            (void)chan_recv(cs->donec, NULL);
            h2c_sunref(cs);
            return NULL;
        }
    }
    cs->res_taken = true;
    res->on_free = BURROW_FN(Func, h2c_response_on_free, cs);
    *err = BURROW_NO_ERROR;
    return res;
}

/* cancelRequest, then the wait for doRequest to be done with the request,
 * which Go leaves to the collector. Drops the caller's reference. */
static Error h2c_cancel_request(h2c_Stream *cs, Error err) {
    h2c_Conn *cc = cs->cc;
    sync_mutex_lock(&cc->mu);
    bool body_closing = cs->req_body_closing;
    sync_mutex_unlock(&cc->mu);
    /* Wait for the request body to be closed.
     *
     * If nothing closed the body before now, abortStreamLocked will have
     * started a goroutine to close it.
     *
     * Closing the body before returning avoids a race condition with
     * net/http checking its readTrackingBody to see if the body was read
     * from or closed. See golang/go#60041. */
    if (body_closing)
        (void)chan_recv(cs->req_body_closed, NULL);
    (void)chan_recv(cs->donec, NULL);
    Error kept = error_retain(error_allocator(), err);
    h2c_sunref(cs);
    return kept;
}

/* ClientConn.RoundTrip. The caller holds a reservation on cc, which this
 * gives back. The error is in the calling goroutine's error arena. */
static HttpResponse *h2c_conn_round_trip(h2c_Conn *cc, HttpRequest *req,
                                         IoReadCloser body, Error *err) {
    h2c_Stream *cs = h2c_new_stream(cc, req, body);
    if (cs == NULL) {
        h2c_decr_stream_reservations(cc);
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    h2c_sref(cs); /* doRequest's */
    if (!go(BURROW_FN(Func, h2c_do_request, cs))) {
        h2c_sunref(cs);
        h2c_sunref(cs);
        h2c_decr_stream_reservations(cc);
        *err = burrow_err_out_of_memory;
        return NULL;
    }

    Chan *ctx_done = context_done(cs->ctx);
    SelectCase sc[] = {
        BURROW_RECV(cs->resp_header_recv, NULL),
        BURROW_RECV(cs->abort, NULL),
        BURROW_RECV(ctx_done, NULL),
    };
    switch (chan_select(sc, 3)) {
    case 0:
        return h2c_handle_response_headers(cs, err);
    case 1:
        if (chan_try_recv(cs->resp_header_recv, NULL, NULL)) {
            /* If both cs.respHeaderRecv and cs.abort are signaling, pick
             * respHeaderRecv. The server probably wrote the response and
             * immediately reset the stream. golang.org/issue/49645 */
            return h2c_handle_response_headers(cs, err);
        }
        *err = h2c_cancel_request(cs, cs->abort_err);
        return NULL;
    default: {
        Error e = context_err(cs->ctx);
        h2c_abort_stream(cs, e);
        *err = h2c_cancel_request(cs, e);
        return NULL;
    }
    }
}

/* ---------------------------------------------------------- response body */

/* transportResponseBody.Read. */
static Int h2c_body_read(void *self, Slice p, Error *err) {
    h2c_Stream *cs = (h2c_Stream *)self;
    h2c_Conn *cc = cs->cc;

    if (BURROW_FAILED(cs->read_err)) {
        *err = cs->read_err;
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = burrow__http2_pipe_read(&cs->buf_pipe, p, &e);
    if (cs->bytes_remain != -1) {
        if ((int64_t)n > cs->bytes_remain) {
            n = (Int)cs->bytes_remain;
            if (BURROW_OK(e)) {
                e = h2c_err_too_much_body;
                h2c_abort_stream(cs, e);
            }
            cs->read_err = h2c_skeep(cs, e);
            *err = e;
            return n;
        }
        cs->bytes_remain -= n;
        if (errors_is(e, io_eof) && cs->bytes_remain > 0) {
            e = io_err_unexpected_eof;
            cs->read_err = e;
            *err = e;
            return n;
        }
    }
    *err = e;
    if (n == 0) {
        /* No flow control tokens to send back. */
        return 0;
    }

    sync_mutex_lock(&cc->mu);
    int32_t conn_add = burrow__http2_inflow_add(&cc->inflow, n);
    int32_t stream_add = 0;
    if (BURROW_OK(e)) /* No need to refresh if the stream is over or failed. */
        stream_add = burrow__http2_inflow_add(&cs->inflow, n);
    sync_mutex_unlock(&cc->mu);

    if (conn_add != 0 || stream_add != 0)
        h2c_write_window_updates(cc, cs->id, conn_add, stream_add);
    return n;
}

/* transportResponseBody.Close. */
static Error h2c_body_close(void *self) {
    h2c_Stream *cs = (h2c_Stream *)self;
    h2c_Conn *cc = cs->cc;
    cs->body_closed = true;

    burrow__http2_pipe_break_with_error(&cs->buf_pipe,
                                        burrow__http2_err_closed_response_body);
    h2c_abort_stream(cs, burrow__http2_err_closed_response_body);

    Int unread = burrow__http2_pipe_len(&cs->buf_pipe);
    if (unread > 0) {
        sync_mutex_lock(&cc->mu);
        /* Return connection-level flow control. */
        int32_t conn_add = burrow__http2_inflow_add(&cc->inflow, unread);
        sync_mutex_unlock(&cc->mu);

        /* TODO(dneil): Acquiring this mutex can block indefinitely. Move
         * flow control return to a goroutine? */
        h2c_write_window_updates(cc, 0, conn_add, 0);
    }

    SelectCase sc[] = {
        BURROW_RECV(cs->donec, NULL),
        BURROW_RECV(context_done(cs->ctx), NULL),
    };
    /* See golang/go#49366: The net/http package can cancel the request
     * context after the response body is fully read. Don't treat this as an
     * error. */
    (void)chan_select(sc, 2);
    return BURROW_NO_ERROR;
}

/* -------------------------------------------------------------- gzipReader */

/* gzipReader wraps a response body so it can lazily make its gzip.Reader on
 * the first call to Read. Go keeps them in a pool, which only saves work, so
 * here each body makes its own. zerr is errConcurrentReadOnResBody while a
 * Read has zr, and fs.ErrClosed once the body is closed. */
struct h2c_GzipReader {
    h2c_Stream *cs;
    GzipReader *zr; /* under mu */
    Error zerr;     /* in arena, under mu */
    Arena arena;
    SyncMutex mu;
};

/* acquire returns a gzip.Reader for reading the response body. It must be
 * released after use. */
static GzipReader *h2c_gzip_acquire(h2c_GzipReader *gz, Error *err) {
    sync_mutex_lock(&gz->mu);
    if (BURROW_FAILED(gz->zerr)) {
        *err = gz->zerr;
        sync_mutex_unlock(&gz->mu);
        return NULL;
    }
    if (gz->zr == NULL) {
        /* gzip_new_reader might block indefinitely since it reads the gzip
         * header. Therefore, drop mu temporarily when using it. We set zerr
         * to errConcurrentReadOnResBody to prevent concurrent read even when
         * mu is temporarily dropped. */
        gz->zerr = burrow__http2_err_concurrent_read_on_res_body;
        sync_mutex_unlock(&gz->mu);
        Error e = BURROW_NO_ERROR;
        GzipReader *zr =
            gzip_new_reader(gz->cs->cc->a, (IoReader){&h2c_body_vt.reader, gz->cs}, &e);
        sync_mutex_lock(&gz->mu);
        /* Guard against Close being called while gzip_new_reader is
         * running. */
        if (!h2c_same(gz->zerr, burrow__http2_err_concurrent_read_on_res_body)) {
            gzip_reader_free(zr);
            *err = gz->zerr;
            sync_mutex_unlock(&gz->mu);
            return NULL;
        }
        gz->zr = zr;
        gz->zerr = error_retain(arena_allocator(&gz->arena), e);
        if (BURROW_FAILED(gz->zerr)) {
            gzip_reader_free(gz->zr);
            gz->zr = NULL;
            *err = gz->zerr;
            sync_mutex_unlock(&gz->mu);
            return NULL;
        }
    }
    GzipReader *ret = gz->zr;
    gz->zr = NULL;
    gz->zerr = burrow__http2_err_concurrent_read_on_res_body;
    sync_mutex_unlock(&gz->mu);
    *err = BURROW_NO_ERROR;
    return ret;
}

/* release keeps the gzip.Reader for the next Read, or frees it if Close was
 * called during this one. */
static void h2c_gzip_release(h2c_GzipReader *gz, GzipReader *zr) {
    sync_mutex_lock(&gz->mu);
    if (h2c_same(gz->zerr, burrow__http2_err_concurrent_read_on_res_body)) {
        gz->zr = zr;
        gz->zerr = BURROW_NO_ERROR;
    } else { /* fs.ErrClosed */
        gzip_reader_free(zr);
    }
    sync_mutex_unlock(&gz->mu);
}

/* close frees the gzip.Reader now, or signals release to do so after Read
 * completes. */
static void h2c_gzip_close_reader(h2c_GzipReader *gz) {
    sync_mutex_lock(&gz->mu);
    if (BURROW_OK(gz->zerr) && gz->zr != NULL) {
        gzip_reader_free(gz->zr);
        gz->zr = NULL;
    }
    gz->zerr = fs_err_closed;
    sync_mutex_unlock(&gz->mu);
}

static Int h2c_gzip_read(void *self, Slice p, Error *err) {
    h2c_GzipReader *gz = (h2c_GzipReader *)self;
    GzipReader *zr = h2c_gzip_acquire(gz, err);
    if (zr == NULL)
        return 0;
    Int n = gzip_reader_read(zr, p, err);
    h2c_gzip_release(gz, zr);
    return n;
}

static Error h2c_gzip_close(void *self) {
    h2c_GzipReader *gz = (h2c_GzipReader *)self;
    h2c_gzip_close_reader(gz);
    return h2c_body_close(gz->cs);
}

static const IoReadCloserVT h2c_gzip_vt = {{NULL, h2c_gzip_read},
                                           {NULL, h2c_gzip_close}};

static IoReadCloser h2c_gzip_body(h2c_Stream *cs) {
    Alloc *a = cs->cc->a;
    h2c_GzipReader *gz =
        (h2c_GzipReader *)mem_alloc(a, sizeof *gz, _Alignof(h2c_GzipReader));
    if (gz == NULL)
        return (IoReadCloser){NULL, NULL};
    gz->cs = cs;
    arena_init(&gz->arena, a, 0);
    cs->gz = gz;
    return (IoReadCloser){&h2c_gzip_vt, gz};
}

/* Once the stream is gone, and so nothing reads the body any more. */
static void h2c_gzip_free(h2c_GzipReader *gz) {
    Alloc *a = gz->cs->cc->a;
    gzip_reader_free(gz->zr);
    arena_free(&gz->arena);
    mem_free(a, gz, sizeof *gz, _Alignof(h2c_GzipReader));
}

/* -------------------------------------------------------------- RoundTrip */

/* clientConnPool.getClientConn, which never dials in net/http. The
 * connection comes with a reference and a stream reservation. */
static h2c_Conn *h2c_get_client_conn(Http2Transport *t, HttpRequest *req, Str addr,
                                     Error *err) {
    const HttptraceClientTrace *trace =
        httptrace_context_client_trace(http_request_context(req));
    sync_mutex_lock(&t->mu);
    for (Int i = 0; i < t->npool; i++) {
        h2c_Conn *cc = t->pool[i];
        if (!str_eq(cc->key, addr) || !h2c_reserve_new_request(cc))
            continue;
        /* When a connection is presented to us by the net/http package, the
         * GetConn hook has already been called. Don't call it a second time
         * here. */
        if (!cc->get_conn_called)
            httptrace_client_trace_get_conn(trace, addr);
        cc->get_conn_called = false;
        h2c_ref(cc);
        sync_mutex_unlock(&t->mu);
        return cc;
    }
    sync_mutex_unlock(&t->mu);
    *err = burrow__http2_err_no_cached_conn;
    return NULL;
}

/* traceGotConn. */
static void h2c_trace_got_conn(HttpRequest *req, h2c_Conn *cc, bool reused) {
    const HttptraceClientTrace *trace =
        httptrace_context_client_trace(http_request_context(req));
    if (trace == NULL)
        return;
    HttptraceGotConnInfo ci;
    memset(&ci, 0, sizeof ci);
    ci.conn = cc->tconn;
    ci.reused = reused;
    sync_mutex_lock(&cc->mu);
    ci.was_idle = map_len(cc->streams) == 0 && reused;
    if (ci.was_idle && !time_is_zero(cc->last_active))
        ci.idle_time = time_since(cc->last_active);
    sync_mutex_unlock(&cc->mu);
    httptrace_client_trace_got_conn(trace, ci);
}

/* canRetryError. */
static bool h2c_can_retry_error(Error err) {
    if (h2c_same(err, burrow__http2_err_client_conn_unusable) ||
        h2c_same(err, burrow__http2_err_client_conn_got_go_away))
        return true;
    Http2StreamError se;
    if (burrow__http2_error_stream(err, &se))
        return se.code == HTTP2_ERR_CODE_REFUSED_STREAM;
    return false;
}

/* shouldRetryRequest. Go makes a copy of the request with the body to send
 * next. Here *body is that body, which starts as the one the last try sent.
 * No error means try again. */
static Error h2c_should_retry_request(HttpRequest *req, IoReadCloser *body, Error err) {
    if (!h2c_can_retry_error(err))
        return err;
    /* If the Body is nil (or http.NoBody), it's safe to reuse this request's
     * Body. */
    if (body->vt == NULL || body->vt == http_no_body.vt)
        return BURROW_NO_ERROR;

    /* If the request body can be reset back to its original state via the
     * optional req.GetBody, do that. */
    if (req->get_body.f != NULL) {
        Error e = BURROW_NO_ERROR;
        IoReadCloser nb = BURROW_CALLF(req->get_body, &e);
        if (BURROW_FAILED(e))
            return e;
        *body = nb;
        return BURROW_NO_ERROR;
    }

    /* The Request.Body can't reset back to the beginning, but we don't seem
     * to have started to read from it yet, so reuse the body. */
    if (h2c_same(err, burrow__http2_err_client_conn_unusable))
        return BURROW_NO_ERROR;

    return fmt_errorf_v(
        "http2: Transport: cannot retry err [%v] after Request.Body was "
        "written; define Request.GetBody to avoid this error",
        err);
}

/* RoundTrip, which is RoundTripOpt with no options. */
HttpResponse *burrow__http2_transport_round_trip(Http2Transport *t, HttpRequest *req,
                                                 Error *err) {
    *err = BURROW_NO_ERROR;
    if (req->url == NULL) {
        *err = h2c_err_nil_url;
        return NULL;
    }
    Str scheme = req->url->scheme;
    if (!str_eq(scheme, BURROW_S("https")) && !str_eq(scheme, BURROW_S("http"))) {
        *err = h2c_err_unsupported_scheme;
        return NULL;
    }

    Arena tmp;
    arena_init(&tmp, heap_allocator(), 0);
    Str addr =
        burrow__http2_authority_addr(arena_allocator(&tmp), scheme, req->url->host);
    if (addr.len == 0) {
        arena_free(&tmp);
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    Context ctx = http_request_context(req);
    IoReadCloser body = req->body;
    HttpResponse *res = NULL;
    Error e = BURROW_NO_ERROR;
    for (Int retry = 0;; retry++) {
        h2c_Conn *cc = h2c_get_client_conn(t, req, addr, &e);
        if (cc == NULL) {
            h2c_vlogf("http2: Transport failed to get client conn for %s: %v", addr, e);
            arena_free(&tmp);
            *err = e;
            return NULL;
        }
        bool reused = !sync_atomic_uint32_compare_and_swap(&cc->atomic_reused, 0, 1);
        h2c_trace_got_conn(req, cc, reused);
        res = h2c_conn_round_trip(cc, req, body, &e);
        if (res == NULL && retry <= 6) {
            Error round_trip_err = e;
            IoReadCloser sent = body;
            e = h2c_should_retry_request(req, &body, e);
            if (BURROW_OK(e)) {
                /* After the first retry, do exponential backoff with 10%
                 * jitter. Go turns the backoff into whole seconds before it
                 * multiplies, so the jitter never shows, and neither does it
                 * here. */
                if (retry == 0) {
                    h2c_vlogf("RoundTrip retrying after failure: %v", round_trip_err);
                    h2c_unref(cc);
                    continue;
                }
                double backoff = (double)((uint64_t)1 << (retry - 1));
                backoff += backoff * (0.1 * math_rand_float64());
                Duration d = TIME_SECOND * (Duration)backoff;
                TimeTimer *tm = time_new_timer(t->a, d);
                if (tm == NULL) {
                    e = burrow_err_out_of_memory;
                } else {
                    SelectCase sc[] = {
                        BURROW_RECV(time_timer_c(tm), NULL),
                        BURROW_RECV(context_done(ctx), NULL),
                    };
                    Int i = chan_select(sc, 2);
                    (void)time_timer_stop(tm);
                    time_timer_free(tm);
                    if (i == 0) {
                        h2c_vlogf("RoundTrip retrying after failure: %v",
                                  round_trip_err);
                        h2c_unref(cc);
                        continue;
                    }
                    e = context_err(ctx);
                }
                /* A body GetBody gave that nothing will send. */
                if (body.data != sent.data || body.vt != sent.vt) {
                    ArenaMark m = error_mark();
                    (void)body.vt->closer.close(body.data);
                    error_release(m);
                }
            }
        }
        if (h2c_same(e, burrow__http2_err_client_conn_not_established)) {
            /* This ClientConn was created recently, this is the first
             * request to use it, and the connection is closed and not
             * usable.
             *
             * In this state, cc.idleTimer will remove the conn from the pool
             * when it fires. Stop the timer and remove it here so future
             * requests won't try to use this connection.
             *
             * If the timer has already fired and we're racing it, the
             * redundant call to MarkDead is harmless. */
            sync_mutex_lock(&cc->mu);
            h2c_timer_stop(cc, cc->idle_timer);
            sync_mutex_unlock(&cc->mu);
            h2c_mark_dead(cc);
        }
        h2c_unref(cc);
        break;
    }
    arena_free(&tmp);
    if (res == NULL) {
        h2c_vlogf("RoundTrip failure: %v", e);
        *err = e;
        return NULL;
    }
    return res;
}

/* --------------------------------------------------- the rest of Transport */

/* CloseIdleConnections, which is clientConnPool.closeIdleConnections. */
void burrow__http2_transport_close_idle_connections(Http2Transport *t) {
    sync_mutex_lock(&t->mu);
    for (Int i = 0; i < t->npool; i++)
        h2c_close_if_idle(t->pool[i]);
    sync_mutex_unlock(&t->mu);
}

/* ClientConn.Close, for the conn the transport made of c. */
void burrow__http2_transport_close_conn(Http2Transport *t, NetConn c) {
    h2c_Conn *cc = NULL;
    sync_mutex_lock(&t->mu);
    for (Int i = 0; i < t->npool; i++) {
        if (t->pool[i]->tconn.data == c.data && h2c_tryref(t->pool[i])) {
            cc = t->pool[i];
            break;
        }
    }
    sync_mutex_unlock(&t->mu);
    if (cc == NULL)
        return;
    h2c_close_for_error(cc, burrow__http2_err_client_conn_force_closed);
    h2c_unref(cc);
}

/* IdleConnStrsForTesting. */
Slice burrow__http2_transport_idle_conn_strs(Http2Transport *t, Alloc *a) {
    Slice ret = slice_make(a, TYPE_STRING, 0, 0);
    sync_mutex_lock(&t->mu);
    for (Int i = 0; i < t->npool; i++) {
        h2c_Conn *cc = t->pool[i];
        if (!h2c_can_take_new_request(cc))
            continue;
        Str k = str_clone(a, cc->key);
        ret = slice_append(a, ret, &k, 1);
    }
    sync_mutex_unlock(&t->mu);
    sort_strings(ret);
    return ret;
}

/* Go's Transport has no Close, and leaves the rest to the garbage collector.
 * This takes the pool's references, closes every connection, and waits for
 * the last reference to each to go. */
void burrow__http2_transport_free(Http2Transport *t) {
    if (t == NULL)
        return;
    sync_atomic_bool_store(&t->closed, true);

    sync_mutex_lock(&t->mu);
    h2c_Conn **pool = t->pool;
    Int npool = t->npool;
    Int pool_cap = t->pool_cap;
    t->pool = NULL;
    t->npool = 0;
    t->pool_cap = 0;
    for (Int i = 0; i < npool; i++)
        pool[i]->in_pool = false;
    sync_mutex_unlock(&t->mu);
    for (Int i = 0; i < npool; i++)
        h2c_unref(pool[i]);
    if (pool != NULL)
        mem_free(t->a, pool, (size_t)pool_cap * sizeof *pool, _Alignof(h2c_Conn *));

    sync_mutex_lock(&t->mu);
    for (;;) {
        h2c_Conn *cc = t->all;
        while (cc != NULL && (cc->free_closed || !h2c_tryref(cc)))
            cc = cc->all_next;
        if (cc == NULL)
            break;
        cc->free_closed = true;
        sync_mutex_unlock(&t->mu);
        h2c_close_for_error(cc, burrow__http2_err_client_conn_force_closed);
        /* A connection the read loop left for a while, to show the error to
         * the next request, which there is none of now. */
        sync_mutex_lock(&cc->mu);
        if (cc->idle_timer == cc->dead_t)
            h2c_timer_stop(cc, cc->idle_timer);
        sync_mutex_unlock(&cc->mu);
        h2c_unref(cc);
        sync_mutex_lock(&t->mu);
    }
    while (t->all != NULL)
        sync_cond_wait(&t->gone);
    sync_mutex_unlock(&t->mu);
    mem_free(t->a, t, sizeof *t, _Alignof(Http2Transport));
}
