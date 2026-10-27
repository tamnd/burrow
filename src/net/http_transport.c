/* net/http's Transport: HTTP/1 requests over connections kept open and used
 * again, through a proxy when one is set.
 *
 * Derived from Go's src/net/http/transport.go.
 * Go source: go1.27.1.
 *
 * Go leans on its collector for most of the lifetimes here. A connection, a
 * request in flight and a goroutine waiting for a connection are each reached
 * from two or three goroutines at once, and whichever lets go last frees it.
 * Here each of those is counted instead: tp_PConn, tp_Want, tp_Trip and
 * tp_Call have a count of references, and the last one to drop its reference
 * frees the thing. Errors that cross from one goroutine to another are copied
 * with error_retain into an arena of the thing they belong to first, since each
 * goroutine's error_allocator goes away with it.
 *
 * What is left out: HTTP/3 and SOCKS5 proxies.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http2.h"
#include "http_internal.h"

#include "../crypto/tls_on_free.h"
#include "../xnet/httpguts.h"
#include "../xnet/httpproxy.h"
#include "http_ascii.h"
#include "internal.h"

#include "burrow/bufio.h"
#include "burrow/chan.h"
#include "burrow/compress/gzip.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/crypto/tls.h"
#include "burrow/declare.h"
#include "burrow/encoding/base64.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptrace.h"
#include "burrow/net/url.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TP_COUNT(arr) (uint16_t)(sizeof(arr) / sizeof((arr)[0]))

/* maxPostCloseReadBytes and maxPostCloseReadTime: how much of a body closed
 * early is read to keep its connection, and for how long. */
#define TP_MAX_POST_CLOSE_READ_BYTES ((int64_t)256 << 10)
#define TP_MAX_POST_CLOSE_READ_TIME (50 * TIME_MILLISECOND)

/* maxWriteWaitBeforeConnReuse. */
#define TP_MAX_WRITE_WAIT_BEFORE_CONN_REUSE (50 * TIME_MILLISECOND)

/* ------------------------------------------------------------------- errors */

BURROW_SENTINEL_ERROR(http_err_skip_alt_protocol, "net/http: skip alternate protocol");
BURROW_SENTINEL_ERROR(burrow__http_err_keep_alives_disabled,
                      "http: putIdleConn: keep alives disabled");
BURROW_SENTINEL_ERROR(burrow__http_err_conn_broken,
                      "http: putIdleConn: connection is in bad state");
BURROW_SENTINEL_ERROR(burrow__http_err_close_idle,
                      "http: putIdleConn: CloseIdleConnections was called");
BURROW_SENTINEL_ERROR(burrow__http_err_too_many_idle,
                      "http: putIdleConn: too many idle connections");
BURROW_SENTINEL_ERROR(burrow__http_err_too_many_idle_host,
                      "http: putIdleConn: too many idle connections for host");
BURROW_SENTINEL_ERROR(burrow__http_err_close_idle_conns,
                      "http: CloseIdleConnections called");
BURROW_SENTINEL_ERROR(burrow__http_err_read_loop_exiting,
                      "http: persistConn.readLoop exiting");
BURROW_SENTINEL_ERROR(burrow__http_err_idle_conn_timeout,
                      "http: idle connection timeout");
BURROW_SENTINEL_ERROR(burrow__http_err_server_closed_idle,
                      "http: server closed idle connection");
BURROW_SENTINEL_ERROR(burrow__http_err_caller_owns_conn,
                      "read loop ending; caller owns writable underlying conn");
BURROW_SENTINEL_ERROR(burrow__http_err_request_canceled, "net/http: request canceled");
BURROW_SENTINEL_ERROR(burrow__http_err_request_canceled_conn,
                      "net/http: request canceled while waiting for connection");
BURROW_SENTINEL_ERROR(burrow__http_err_request_done, "net/http: request completed");
BURROW_SENTINEL_ERROR(burrow__http_err_read_on_closed_res_body,
                      "http: read on closed response body");
BURROW_SENTINEL_ERROR(burrow__http_err_concurrent_read_on_res_body,
                      "http: concurrent read on response body");
BURROW_SENTINEL_ERROR(burrow__http_err_cannot_rewind,
                      "net/http: cannot rewind body after connection loss");
BURROW_SENTINEL_ERROR(burrow__http_err_no_host_in_url, "http: no Host in request URL");
BURROW_SENTINEL_ERROR(tp_err_nil_url, "http: nil Request.URL");
BURROW_SENTINEL_ERROR(tp_err_nil_header, "http: nil Request.Header");
BURROW_SENTINEL_ERROR(tp_err_dial_context_nil,
                      "net/http: Transport.DialContext hook returned (nil, nil)");
BURROW_SENTINEL_ERROR(tp_err_dial_nil,
                      "net/http: Transport.Dial hook returned (nil, nil)");
BURROW_SENTINEL_ERROR(
    tp_err_dial_tls_nil,
    "net/http: Transport.DialTLS or DialTLSContext returned (nil, nil)");
BURROW_SENTINEL_ERROR(tp_err_unknown_status, "unknown status code");
BURROW_SENTINEL_ERROR(tp_err_unencrypted_h2,
                      "http: Transport does not support unencrypted HTTP/2");
BURROW_SENTINEL_ERROR(tp_err_socks5, "net/http: SOCKS5 proxies are not supported yet");
BURROW_SENTINEL_ERROR(tp_err_stub_round_tripper, "unexpected use of stub RoundTripper");
BURROW_SENTINEL_ERROR(tp_err_h2_no_client_conn,
                      "http: HTTP/2 implementation does not support NewClientConn "
                      "(update golang.org/x/net?)");
BURROW_SENTINEL_ERROR(
    tp_err_not_client_conn,
    "http: NewClientConn returned something that is not a ClientConn");
BURROW_SENTINEL_ERROR(tp_err_client_conn_closed, "ClientConn closed");
BURROW_SENTINEL_ERROR(tp_err_conn_unavailable, "connection is unavailable");

/* timeoutError, and errTimeout, which is one. A net.Error that is a timeout
 * and is context_deadline_exceeded to errors_is. */
typedef struct HttpErrTimeout {
    Str text;
} HttpErrTimeout;

static Str tp_timeout_m_error(HttpErrTimeout *self) {
    return self->text;
}

static bool tp_timeout_m_true(HttpErrTimeout *self) {
    (void)self;
    return true;
}

#define TP_SIG_STRING(IN, OUT) OUT(Str)
#define TP_SIG_BOOL(IN, OUT) OUT(bool)

#define TP_TIMEOUT_METHODS(M, T)                                                       \
    M(T, Error, tp_timeout_m_error, TP_SIG_STRING)                                     \
    M(T, Temporary, tp_timeout_m_true, TP_SIG_BOOL)                                    \
    M(T, Timeout, tp_timeout_m_true, TP_SIG_BOOL)

BURROW_METHODS_DEFINE(HttpErrTimeout, TP_TIMEOUT_METHODS);

static const Type tp_timeout_desc = {
    BURROW_S_INIT("timeoutError"),
    BURROW_S_INIT("net/http"),
    KIND_STRUCT,
    (uint32_t)sizeof(HttpErrTimeout),
    (uint16_t)_Alignof(HttpErrTimeout),
    0,
    TP_COUNT(burrow__methods_HttpErrTimeout),
    NULL,
    burrow__methods_HttpErrTimeout,
    NULL,
    NULL,
    0,
    0x68747469U, /* "htti" */
    NULL,
};

static Str tp_timeout_message(const void *self) {
    return tp_timeout_m_error((HttpErrTimeout *)(uintptr_t)self);
}

static bool tp_timeout_is(const void *self, Error target) {
    (void)self;
    return target.vt == context_deadline_exceeded.vt &&
           target.data == context_deadline_exceeded.data;
}

static Error tp_timeout_clone(const void *self, Alloc *a);

static const ErrorVT tp_timeout_vt = {
    &tp_timeout_desc, tp_timeout_message, NULL, NULL, tp_timeout_is, NULL,
    tp_timeout_clone,
};

static const HttpErrTimeout tp_timeout_value = {
    BURROW_S_INIT("net/http: timeout awaiting response headers")};

const Error burrow__http_err_timeout = {&tp_timeout_vt, &tp_timeout_value};

Error burrow__http_timeout_error(Alloc *a, Str text) {
    /* One block, the text after the struct. */
    HttpErrTimeout *e = (HttpErrTimeout *)mem_alloc(a, sizeof *e + (size_t)text.len,
                                                    _Alignof(HttpErrTimeout));
    if (e == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(e + 1);
    if (text.len > 0)
        memcpy(p, text.p, (size_t)text.len);
    e->text = str_from_bytes(p, text.len);
    return (Error){&tp_timeout_vt, e};
}

/* errTimeout lives as long as the program, so a copy of it is itself. */
static Error tp_timeout_clone(const void *self, Alloc *a) {
    if (self == &tp_timeout_value)
        return burrow__http_err_timeout;
    return burrow__http_timeout_error(a, ((const HttpErrTimeout *)self)->text);
}

/* tlsHandshakeTimeoutError, a net.Error that is a timeout, and unlike
 * timeoutError is not context_deadline_exceeded to errors_is. */
static const Type tp_tls_timeout_desc = {
    BURROW_S_INIT("tlsHandshakeTimeoutError"),
    BURROW_S_INIT("net/http"),
    KIND_STRUCT,
    (uint32_t)sizeof(HttpErrTimeout),
    (uint16_t)_Alignof(HttpErrTimeout),
    0,
    TP_COUNT(burrow__methods_HttpErrTimeout),
    NULL,
    burrow__methods_HttpErrTimeout,
    NULL,
    NULL,
    0,
    0x68747468U, /* "htth" */
    NULL,
};

static Error tp_tls_timeout_clone(const void *self, Alloc *a);

static const ErrorVT tp_tls_timeout_vt = {
    &tp_tls_timeout_desc, tp_timeout_message, NULL, NULL, NULL, NULL,
    tp_tls_timeout_clone,
};

static const HttpErrTimeout tp_tls_timeout_value = {
    BURROW_S_INIT("net/http: TLS handshake timeout")};

static const Error tp_err_tls_handshake_timeout = {&tp_tls_timeout_vt,
                                                   &tp_tls_timeout_value};

/* There is only the one, so a copy of it is itself. */
static Error tp_tls_timeout_clone(const void *self, Alloc *a) {
    (void)self;
    (void)a;
    return tp_err_tls_handshake_timeout;
}

static bool tp_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* nothingWrittenError and transportReadFromServerError, which wrap an error to
 * say that none of the request went out, or that the error came from reading
 * the response. The first has the message of what it wraps, and the second
 * says what happened first. Both unwrap to what they wrap. */
typedef struct tp_Wrapped {
    Error inner;
    Str text;
} tp_Wrapped;

static Str tp_wrapped_message(const void *self) {
    return ((const tp_Wrapped *)self)->text;
}

static Error tp_wrapped_unwrap(const void *self) {
    return ((const tp_Wrapped *)self)->inner;
}

static Error tp_nothing_written_clone(const void *self, Alloc *a);
static Error tp_read_from_server_clone(const void *self, Alloc *a);

static const ErrorVT tp_nothing_written_vt = {
    NULL, tp_wrapped_message,       tp_wrapped_unwrap, NULL, NULL,
    NULL, tp_nothing_written_clone,
};

static const ErrorVT tp_read_from_server_vt = {
    NULL, tp_wrapped_message,        tp_wrapped_unwrap, NULL, NULL,
    NULL, tp_read_from_server_clone,
};

static Error tp_wrap(Alloc *a, const ErrorVT *vt, Error inner) {
    tp_Wrapped *w = (tp_Wrapped *)mem_alloc(a, sizeof *w, _Alignof(tp_Wrapped));
    if (w == NULL)
        return burrow_err_out_of_memory;
    w->inner = error_retain(a, inner);
    if (vt == &tp_nothing_written_vt) {
        w->text = error_text(w->inner);
    } else {
        Str msg = error_text(w->inner);
        Str pre = BURROW_S("net/http: Transport failed to read from server: ");
        Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(pre.len + msg.len), 1);
        if (p == NULL)
            return burrow_err_out_of_memory;
        memcpy(p, pre.p, (size_t)pre.len);
        if (msg.len > 0)
            memcpy(p + pre.len, msg.p, (size_t)msg.len);
        w->text = str_from_bytes(p, pre.len + msg.len);
    }
    return (Error){vt, w};
}

static Error tp_nothing_written_clone(const void *self, Alloc *a) {
    return tp_wrap(a, &tp_nothing_written_vt, ((const tp_Wrapped *)self)->inner);
}

static Error tp_read_from_server_clone(const void *self, Alloc *a) {
    return tp_wrap(a, &tp_read_from_server_vt, ((const tp_Wrapped *)self)->inner);
}

static bool tp_is_nothing_written(Error err) {
    return err.vt == &tp_nothing_written_vt;
}

static bool tp_is_read_from_server(Error err) {
    return err.vt == &tp_read_from_server_vt;
}

/* ------------------------------------------------------------------ helpers */

static Alloc *tp_alloc(const HttpTransport *t) {
    return t->a != NULL ? t->a : heap_allocator();
}

static bool tp_is_no_body(IoReadCloser b) {
    return b.vt == NULL || b.vt == http_no_body.vt;
}

/* A copy of s in a, or "" when a says no. */
static Str tp_dup(Alloc *a, Str s) {
    if (s.len == 0)
        return BURROW_STR_EMPTY;
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)s.len, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, s.p, (size_t)s.len);
    return str_from_bytes(p, s.len);
}

/* Takes a reference out on a wait group and gives it back when fn returns. */
typedef struct tp_Go {
    Func fn;
    SyncWaitGroup *wg;
} tp_Go;

static void tp_go_run(void *env) {
    tp_Go g = *(tp_Go *)env;
    mem_free(heap_allocator(), env, sizeof g, _Alignof(tp_Go));
    BURROW_CALLF0(g.fn);
    sync_wait_group_done(g.wg);
}

/* go fn, counted in t's live. False when the goroutine could not be made. */
static bool tp_go(HttpTransport *t, Func fn) {
    tp_Go *g = (tp_Go *)mem_alloc(heap_allocator(), sizeof *g, _Alignof(tp_Go));
    if (g == NULL)
        return false;
    g->fn = fn;
    g->wg = &t->live;
    sync_wait_group_add(&t->live, 1);
    if (!go(BURROW_FN(Func, tp_go_run, g))) {
        sync_wait_group_done(&t->live);
        mem_free(heap_allocator(), g, sizeof *g, _Alignof(tp_Go));
        return false;
    }
    return true;
}

/* Waits on c, which is closed or sent on by another goroutine. */
static void tp_wait(Chan *c) {
    if (c != NULL)
        (void)chan_recv(c, NULL);
}

/* ----------------------------------------------------------- connectMethod */

/* connectMethod: how a request gets to where it goes, directly or through a
 * proxy. */
typedef struct tp_ConnectMethod {
    Url *proxy_url;    /* NULL for none */
    Str target_scheme; /* "http" or "https" */
    Str target_addr;   /* the host and port, with no proxy in it */
    bool only_h1;
} tp_ConnectMethod;

/* connectMethod.scheme. */
static Str tp_cm_scheme(const tp_ConnectMethod *cm) {
    return cm->proxy_url != NULL ? cm->proxy_url->scheme : cm->target_scheme;
}

/* connectMethod.addr: where the connection goes. */
static Str tp_cm_addr(Alloc *a, const tp_ConnectMethod *cm) {
    if (cm->proxy_url != NULL)
        return burrow__httpproxy_canonical_addr(a, cm->proxy_url);
    return cm->target_addr;
}

Str burrow__http_proxy_auth(Alloc *a, const Url *proxy_url) {
    if (proxy_url == NULL || proxy_url->user == NULL)
        return BURROW_STR_EMPTY;
    Str user = url_userinfo_username(proxy_url->user);
    bool ok = false;
    Str pass = url_userinfo_password(proxy_url->user, &ok);
    Int n = user.len + 1 + pass.len;
    Byte *plain = (Byte *)mem_alloc_nozero(a, (size_t)n, 1);
    if (plain == NULL)
        return BURROW_STR_EMPTY;
    if (user.len > 0)
        memcpy(plain, user.p, (size_t)user.len);
    plain[user.len] = ':';
    if (pass.len > 0)
        memcpy(plain + user.len + 1, pass.p, (size_t)pass.len);
    Str enc = base64_encoding_encode_to_string(base64_std_encoding, a,
                                               slice_from(plain, n, n, TYPE_BYTE));
    mem_free(a, plain, (size_t)n, 1);
    Str pre = BURROW_S("Basic ");
    Byte *v = (Byte *)mem_alloc_nozero(a, (size_t)(pre.len + enc.len), 1);
    if (v == NULL)
        return BURROW_STR_EMPTY;
    memcpy(v, pre.p, (size_t)pre.len);
    if (enc.len > 0)
        memcpy(v + pre.len, enc.p, (size_t)enc.len);
    return str_from_bytes(v, pre.len + enc.len);
}

Str burrow__http_connect_method_key(Alloc *a, const Url *proxy_url, Str target_scheme,
                                    Str target_addr, bool only_h1) {
    Str proxy = BURROW_STR_EMPTY;
    Str addr = target_addr;
    if (proxy_url != NULL) {
        proxy = url_string(proxy_url, a);
        if ((str_eq(proxy_url->scheme, BURROW_S("http")) ||
             str_eq(proxy_url->scheme, BURROW_S("https"))) &&
            str_eq(target_scheme, BURROW_S("http")))
            addr = BURROW_STR_EMPTY;
    }
    return fmt_sprintf_v(a, "%s|%s%s|%s", proxy, target_scheme, only_h1 ? ",h1" : "",
                         addr);
}

static Str tp_cm_key(Alloc *a, const tp_ConnectMethod *cm) {
    return burrow__http_connect_method_key(a, cm->proxy_url, cm->target_scheme,
                                           cm->target_addr, cm->only_h1);
}

/* Request.requiresHTTP1: a websocket upgrade. */
static bool tp_requires_http1(const HttpRequest *r) {
    return burrow__http_has_token(
               burrow__http_header_get(r->header, BURROW_S("Connection")),
               BURROW_S("upgrade")) &&
           burrow__http_ascii_equal_fold(
               burrow__http_header_get(r->header, BURROW_S("Upgrade")),
               BURROW_S("websocket"));
}

/* isProtocolSwitchHeader. */
static bool tp_is_protocol_switch_header(HttpHeader h) {
    if (burrow__http_header_get(h, BURROW_S("Upgrade")).len == 0)
        return false;
    Slice vs = http_header_values(h, BURROW_S("Connection"));
    return burrow__httpguts_header_values_contains_token((const Str *)vs.p, vs.len,
                                                         BURROW_S("Upgrade"));
}

/* Request.expectsContinue and wantsClose. */
static bool tp_expects_continue(const HttpRequest *r) {
    return burrow__http_has_token(
        burrow__http_header_get(r->header, BURROW_S("Expect")),
        BURROW_S("100-continue"));
}

static bool tp_wants_close(const HttpRequest *r) {
    if (r->close)
        return true;
    return burrow__http_has_token(
        burrow__http_header_get(r->header, BURROW_S("Connection")), BURROW_S("close"));
}

/* Request.isReplayable. */
static bool tp_is_replayable(const HttpRequest *r) {
    if (tp_is_no_body(r->body) || r->get_body.f != NULL) {
        Str m = r->method.len > 0 ? r->method : BURROW_S("GET");
        if (str_eq(m, BURROW_S("GET")) || str_eq(m, BURROW_S("HEAD")) ||
            str_eq(m, BURROW_S("OPTIONS")) || str_eq(m, BURROW_S("TRACE")))
            return true;
        if (burrow__http_header_has(r->header, BURROW_S("Idempotency-Key")) ||
            burrow__http_header_has(r->header, BURROW_S("X-Idempotency-Key")))
            return true;
    }
    return false;
}

/* Response.isProtocolSwitch. */
static bool tp_is_protocol_switch(const HttpResponse *r) {
    return r->status_code == HTTP_STATUS_SWITCHING_PROTOCOLS &&
           tp_is_protocol_switch_header(r->header);
}

/* ------------------------------------------------------------------ structs */

typedef struct burrow__HttpWant tp_Want;
typedef struct burrow__HttpPConn tp_PConn;
typedef struct burrow__HttpCall tp_Call;
typedef struct burrow__HttpWantQueue tp_Queue;
typedef struct tp_Trip tp_Trip;
typedef struct tp_Body tp_Body;
typedef struct tp_Gzip tp_Gzip;
typedef struct tp_Track tp_Track;

/* persistConn: one connection, with a goroutine reading the responses from it
 * and one writing the requests to it. Each of those has a reference, and so do
 * the trip using it, an idle timer that is set and a want it was handed to. */
struct burrow__HttpPConn {
    HttpTransport *t;
    NetConn conn;
    BufioReader *br;
    BufioWriter *bw;
    Chan *reqch;           /* of tp_Trip *, to the read loop */
    Chan *writech;         /* of tp_Trip *, to the write loop */
    Chan *closech;         /* closed when the connection is */
    Chan *write_err_ch;    /* bool, whether the last request went out */
    Chan *write_loop_done; /* closed when the write loop ends */
    Chan *read_loop_done;  /* closed when the read loop ends */
    Chan *eofc;            /* the read loop's, see tp_read_loop */
    Str key;               /* in arena */
    Str proxy_auth;        /* in arena, under mu, and cleared on close */
    Error closed;          /* in err_arena, under mu */
    Error canceled_err;    /* in err_arena, under mu */
    SyncAtomicInt64 refs;
    SyncAtomicInt64 nwrite;
    int64_t read_limit; /* the read loop's */
    Int num_expected;   /* under mu */
    Arena arena;        /* made in before the loops start, and only read after */
    Arena err_arena;
    SyncMutex mu;

    /* Under t->idle_mu. */
    TimeTimer *idle_timer;
    tp_PConn *closing_next; /* see http_transport_close_idle_connections */
    tp_PConn *lru_prev, *lru_next;
    Time idle_at;
    bool in_lru;

    bool saw_eof; /* the read loop's */
    bool is_proxy;
    bool reused;             /* under mu */
    bool rl_exited;          /* under mu, and reqch takes no more after */
    bool wl_exited;          /* under mu, and writech takes no more after */
    bool uncounted;          /* the dial gave back the count for key, not the close */
    bool alt;                /* HTTP/2's: conn is t->h2's, and there are no loops */
    HttpRoundTripper alt_rt; /* with alt, from TLSNextProto, or none for t->h2 */
    const TlsConnectionState *tls_state; /* NULL, or &tls_state_v */
    TlsConnectionState tls_state_v;

    /* ClientConn only. */
    bool is_client_conn; /* whether this is a ClientConn (outside any pool) */
    Chan *availch;       /* bool, holds a value when the conn is usable */
    bool reserved;       /* under mu: concurrency slot reserved */
    bool in_flight;      /* under mu: request is in flight */
    Func state_hook;     /* ClientConn state hook */
    Func hook_done;      /* Not in Go: called as pc is freed */
    void *alt_cc;        /* Not in Go: the Http2ClientConn alt is, for a dial */
};

/* transportRequest and requestAndChan in one: a single try at sending a request
 * on a connection. The caller, the read loop and the write loop each have a
 * reference while they use it. */
struct tp_Trip {
    HttpTransport *t;
    tp_Call *call; /* the read loop's only once the response is delivered */
    HttpRequest *req;
    tp_PConn *pc; /* a reference */
    Context ctx;
    HttpHeader extra;   /* in arena */
    Chan *resc;         /* bool, unbuffered: res or rl_err is set */
    Chan *gone;         /* closed when the caller has returned */
    Chan *write_err_ch; /* bool, after wl_err is set */
    Chan *continue_ch;  /* bool, or NULL without "Expect: 100-continue" */
    Chan *wait_body;    /* Int, a TP_BODY_ outcome */
    Chan *rl_done;      /* closed when the read loop is done with the trip */
    Chan *wl_done;      /* closed when the write loop is done with the trip */
    HttpResponse *res;
    tp_Body *body; /* what the response body is wrapped in */
    tp_Gzip *gz;
    Error rl_err;  /* in rl_arena */
    Error set_err; /* in wl_arena, under mu */
    Error wl_err;  /* in wl_arena, under mu, before write_err_ch is sent on */
    SyncAtomicInt64 refs;
    Arena arena;    /* the caller's, until the trip is handed over */
    Arena rl_arena; /* the read loop's */
    Arena wl_arena; /* the write loop's, under mu */
    SyncMutex mu;
    bool added_gzip;
    bool put_idle; /* the connection went back to the pool after it */
};

/* A round trip as the caller sees it, from Transport.roundTrip's start until
 * the response it gives is freed, over however many tries it takes. */
struct burrow__HttpCall {
    tp_Call *next; /* in t->calls, under t->req_mu */
    HttpTransport *t;
    HttpRequest *orig;
    Context ctx;
    ContextCancelCauseFunc cancel;
    tp_Trip *trip;     /* the try that gave the response */
    tp_Track *tracks;  /* every body tracker made, freed with the call */
    Func alt_on_free;  /* the alternate round tripper's response's on_free */
    Chan *legacy_done; /* closed once awaitLegacyCancel returns, or NULL */
    bool linked;       /* under t->req_mu */
    HttpRequest req;   /* orig with its body in a tracker */
};

/* wantConn: a call to getConn, which a dial and the idle pool race to hand a
 * connection to, unless it is canceled first. */
struct burrow__HttpWant {
    HttpTransport *t;
    Context ctx;     /* for the dial, the last of ctxs */
    Context ctxs[3]; /* what ctx is made of, released with the want */
    int nctx;
    ContextCancelFunc cancel_ctx;
    tp_ConnectMethod cm; /* in arena, with the proxy URL a copy */
    Str key;             /* in arena */
    Chan *ready;         /* bool, closed once there is a result */
    tp_PConn *res_pc;    /* a reference, under mu, until it is taken */
    Time res_idle_at;    /* under mu, when res_pc came from the pool */
    Error res_err;       /* in err_arena, under mu */
    SyncAtomicInt64 refs;
    Arena arena; /* made in before the want is shared */
    Arena err_arena;
    SyncMutex mu;
    bool done;           /* under mu: delivered or canceled */
    bool has_result;     /* under mu */
    bool has_cancel_ctx; /* under t->conns_per_host_mu */

    /* What the dial sees of the request's context. Go dials with a context
     * that keeps the request's values, but the request can be gone before the
     * dial is done, so here ctx gets copies: the alternate resolver tests set,
     * and in place of the trace, trace_proxy, a trace with no hooks of its own
     * whose net hooks pass the dial's events on to trace_to. That stops once
     * the request has its connection. */
    burrow__NetLookupIPFunc alt;
    HttptraceClientTrace trace_proxy;
    const HttptraceClientTrace *trace_to; /* under trace_mu */
    SyncMutex trace_mu;
};

/* The idle connections for one key, and the wants waiting for one. */
typedef struct burrow__HttpIdleBucket {
    struct burrow__HttpIdleBucket *next;
    Str key;
    tp_PConn **conns;
    Int n, cap;
    tp_Queue wait;
} tp_IdleBucket;

/* The connections for one key, when max_conns_per_host limits them, and the
 * wants waiting for there to be fewer. */
typedef struct burrow__HttpHostBucket {
    struct burrow__HttpHostBucket *next;
    Str key;
    Int n;
    tp_Queue wait;
} tp_HostBucket;

/* An alternate round tripper, from http_transport_register_protocol. */
typedef struct burrow__HttpAltProto {
    struct burrow__HttpAltProto *next;
    Str scheme;
    HttpRoundTripper rt;
} tp_AltProto;

static void tp_pc_unref(tp_PConn *pc);
static void tp_want_unref(tp_Want *w);
static void tp_trip_unref(tp_Trip *tr);
static void tp_call_cancel(tp_Call *call, Error cause);
static void tp_pc_close(tp_PConn *pc, Error err);
static void tp_pc_close_locked(tp_PConn *pc, Error err);
static void tp_dec_conns_per_host(HttpTransport *t, Str key);

/* ------------------------------------------------------------- want queues */

static Int tp_queue_len(const tp_Queue *q) {
    return q->len;
}

/* pushBack. The queue takes a reference on w. False when the ring cannot
 * grow. */
static bool tp_queue_push(HttpTransport *t, tp_Queue *q, tp_Want *w) {
    if (q->len == q->cap) {
        Int ncap = q->cap == 0 ? 4 : q->cap * 2;
        tp_Want **nw = (tp_Want **)mem_alloc(tp_alloc(t), (size_t)ncap * sizeof *nw,
                                             _Alignof(tp_Want *));
        if (nw == NULL)
            return false;
        for (Int i = 0; i < q->len; i++)
            nw[i] = q->w[(q->head + i) % q->cap];
        if (q->w != NULL)
            mem_free(tp_alloc(t), q->w, (size_t)q->cap * sizeof *q->w,
                     _Alignof(tp_Want *));
        q->w = nw;
        q->cap = ncap;
        q->head = 0;
    }
    q->w[(q->head + q->len) % q->cap] = w;
    q->len++;
    sync_atomic_int64_add(&w->refs, 1);
    return true;
}

/* peekFront. */
static tp_Want *tp_queue_peek(const tp_Queue *q) {
    return q->len == 0 ? NULL : q->w[q->head];
}

/* popFront. The reference the queue had is the caller's now. */
static tp_Want *tp_queue_pop(tp_Queue *q) {
    if (q->len == 0)
        return NULL;
    tp_Want *w = q->w[q->head];
    q->w[q->head] = NULL;
    q->head = (q->head + 1) % q->cap;
    q->len--;
    return w;
}

static void tp_queue_free(HttpTransport *t, tp_Queue *q) {
    while (q->len > 0)
        tp_want_unref(tp_queue_pop(q));
    if (q->w != NULL)
        mem_free(tp_alloc(t), q->w, (size_t)q->cap * sizeof *q->w, _Alignof(tp_Want *));
    q->w = NULL;
    q->cap = 0;
    q->head = 0;
}

/* wantConn.waiting. */
static bool tp_want_waiting(tp_Want *w) {
    sync_mutex_lock(&w->mu);
    bool waiting = !w->done;
    sync_mutex_unlock(&w->mu);
    return waiting;
}

/* cleanFrontNotWaiting. */
static void tp_queue_clean_front_not_waiting(tp_Queue *q) {
    for (;;) {
        tp_Want *w = tp_queue_peek(q);
        if (w == NULL || tp_want_waiting(w))
            return;
        (void)tp_queue_pop(q);
        tp_want_unref(w);
    }
}

/* cleanFrontCanceled, under conns_per_host_mu. */
static void tp_queue_clean_front_canceled(tp_Queue *q) {
    for (;;) {
        tp_Want *w = tp_queue_peek(q);
        if (w == NULL || w->has_cancel_ctx)
            return;
        (void)tp_queue_pop(q);
        tp_want_unref(w);
    }
}

/* ------------------------------------------------------------ the buckets */

static tp_IdleBucket *tp_idle_bucket(HttpTransport *t, Str key, bool make) {
    for (tp_IdleBucket *b = t->idle; b != NULL; b = b->next)
        if (str_eq(b->key, key))
            return b;
    if (!make)
        return NULL;
    Alloc *a = tp_alloc(t);
    tp_IdleBucket *b =
        (tp_IdleBucket *)mem_alloc(a, sizeof *b, _Alignof(tp_IdleBucket));
    if (b == NULL)
        return NULL;
    b->key = tp_dup(a, key);
    if (b->key.len != key.len) {
        mem_free(a, b, sizeof *b, _Alignof(tp_IdleBucket));
        return NULL;
    }
    b->next = t->idle;
    t->idle = b;
    return b;
}

/* IdleConnCountForTesting, from Go's export_test.go. */
Int burrow__http_transport_idle_conn_count_for_testing(HttpTransport *t, Str scheme,
                                                       Str addr) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Str key = burrow__http_connect_method_key(arena_allocator(&ar), NULL, scheme, addr,
                                              false);
    Int n = 0;
    sync_mutex_lock(&t->idle_mu);
    tp_IdleBucket *b = tp_idle_bucket(t, key, false);
    if (b != NULL)
        n = b->n;
    sync_mutex_unlock(&t->idle_mu);
    arena_free(&ar);
    return n;
}

/* Not in Go. Go's tests of the drain run under synctest, where its 50ms only
 * pass once every goroutine is blocked. burrow's run on a real clock, and a
 * loaded machine can take that long to hand the drain the end of the body. */
void burrow__http_transport_set_post_close_read_time_for_testing(HttpTransport *t,
                                                                 Duration d) {
    t->post_close_read_time = d;
}

const Duration burrow__http_max_post_close_read_time = TP_MAX_POST_CLOSE_READ_TIME;

/* Takes b out and frees it once it has no connections or wants left. */
static void tp_idle_bucket_trim(HttpTransport *t, tp_IdleBucket *b) {
    if (b->n > 0 || tp_queue_len(&b->wait) > 0)
        return;
    for (tp_IdleBucket **pp = &t->idle; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == b) {
            *pp = b->next;
            break;
        }
    }
    Alloc *a = tp_alloc(t);
    tp_queue_free(t, &b->wait);
    if (b->conns != NULL)
        mem_free(a, b->conns, (size_t)b->cap * sizeof *b->conns, _Alignof(tp_PConn *));
    if (b->key.len > 0)
        mem_free(a, (void *)(uintptr_t)b->key.p, (size_t)b->key.len, 1);
    mem_free(a, b, sizeof *b, _Alignof(tp_IdleBucket));
}

static bool tp_idle_bucket_append(HttpTransport *t, tp_IdleBucket *b, tp_PConn *pc) {
    if (b->n == b->cap) {
        Alloc *a = tp_alloc(t);
        Int ncap = b->cap == 0 ? 2 : b->cap * 2;
        tp_PConn **nc =
            (tp_PConn **)mem_alloc(a, (size_t)ncap * sizeof *nc, _Alignof(tp_PConn *));
        if (nc == NULL)
            return false;
        if (b->n > 0)
            memcpy(nc, b->conns, (size_t)b->n * sizeof *nc);
        if (b->conns != NULL)
            mem_free(a, b->conns, (size_t)b->cap * sizeof *b->conns,
                     _Alignof(tp_PConn *));
        b->conns = nc;
        b->cap = ncap;
    }
    b->conns[b->n++] = pc;
    return true;
}

static tp_HostBucket *tp_host_bucket(HttpTransport *t, Str key, bool make) {
    for (tp_HostBucket *b = t->conns_per_host; b != NULL; b = b->next)
        if (str_eq(b->key, key))
            return b;
    if (!make)
        return NULL;
    Alloc *a = tp_alloc(t);
    tp_HostBucket *b =
        (tp_HostBucket *)mem_alloc(a, sizeof *b, _Alignof(tp_HostBucket));
    if (b == NULL)
        return NULL;
    b->key = tp_dup(a, key);
    if (b->key.len != key.len) {
        mem_free(a, b, sizeof *b, _Alignof(tp_HostBucket));
        return NULL;
    }
    b->next = t->conns_per_host;
    t->conns_per_host = b;
    return b;
}

static void tp_host_bucket_trim(HttpTransport *t, tp_HostBucket *b) {
    if (b->n > 0 || tp_queue_len(&b->wait) > 0)
        return;
    for (tp_HostBucket **pp = &t->conns_per_host; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == b) {
            *pp = b->next;
            break;
        }
    }
    Alloc *a = tp_alloc(t);
    tp_queue_free(t, &b->wait);
    if (b->key.len > 0)
        mem_free(a, (void *)(uintptr_t)b->key.p, (size_t)b->key.len, 1);
    mem_free(a, b, sizeof *b, _Alignof(tp_HostBucket));
}

/* ---------------------------------------------------------------- the LRU */

/* connLRU, oldest at the head. Under idle_mu. */
static void tp_lru_add(HttpTransport *t, tp_PConn *pc) {
    pc->lru_next = NULL;
    pc->lru_prev = t->lru_tail;
    if (t->lru_tail != NULL)
        t->lru_tail->lru_next = pc;
    else
        t->lru_head = pc;
    t->lru_tail = pc;
    pc->in_lru = true;
    t->lru_len++;
}

static void tp_lru_remove(HttpTransport *t, tp_PConn *pc) {
    if (!pc->in_lru)
        return;
    if (pc->lru_prev != NULL)
        pc->lru_prev->lru_next = pc->lru_next;
    else
        t->lru_head = pc->lru_next;
    if (pc->lru_next != NULL)
        pc->lru_next->lru_prev = pc->lru_prev;
    else
        t->lru_tail = pc->lru_prev;
    pc->lru_prev = NULL;
    pc->lru_next = NULL;
    pc->in_lru = false;
    t->lru_len--;
}

static tp_PConn *tp_lru_remove_oldest(HttpTransport *t) {
    tp_PConn *pc = t->lru_head;
    if (pc != NULL)
        tp_lru_remove(t, pc);
    return pc;
}

/* ----------------------------------------------------------- the connection */

/* Transport.maxHeaderResponseSize, readBufferSize and writeBufferSize. */
static int64_t tp_max_header_response_size(const HttpTransport *t) {
    return t->max_response_header_bytes != 0 ? t->max_response_header_bytes
                                             : (int64_t)10 << 20;
}

static Int tp_read_buffer_size(const HttpTransport *t) {
    return t->read_buffer_size > 0 ? t->read_buffer_size : (Int)4 << 10;
}

static Int tp_write_buffer_size(const HttpTransport *t) {
    return t->write_buffer_size > 0 ? t->write_buffer_size : (Int)4 << 10;
}

static void tp_pc_ref(tp_PConn *pc) {
    (void)sync_atomic_int64_add(&pc->refs, 1);
}

static TlsConn *tp_own_tls_conn(NetConn c);

/* Closes c and gives it back the way the transport gives back each one. A TLS
 * connection the transport made gives back the one under it the same way when
 * it is freed. */
static void tp_free_conn(HttpTransport *t, NetConn c) {
    if (c.vt == NULL)
        return;
    (void)c.vt->closer.close(c.data);
    TlsConn *tc = tp_own_tls_conn(c);
    if (tc != NULL)
        tls_conn_free(tc);
    else if (t->free_conn.f != NULL)
        BURROW_CALLF(t->free_conn, c);
    else
        net_conn_free(c);
}

void burrow__http_transport_free_conn(HttpTransport *t, NetConn c) {
    tp_free_conn(t, c);
}

/* The last reference is gone, so both loops have ended and the connection is
 * closed. It is closed again here for a connection a 101 response handed to
 * its caller, which may not have. */
static void tp_pc_free(tp_PConn *pc) {
    HttpTransport *t = pc->t;
    Alloc *a = tp_alloc(t);
    tp_free_conn(t, pc->conn);
    if (pc->br != NULL)
        bufio_reader_free(pc->br);
    if (pc->bw != NULL)
        bufio_writer_free(pc->bw);
    chan_free(pc->reqch);
    chan_free(pc->writech);
    chan_free(pc->closech);
    chan_free(pc->write_err_ch);
    chan_free(pc->write_loop_done);
    chan_free(pc->read_loop_done);
    chan_free(pc->eofc);
    chan_free(pc->availch);
    time_timer_free(pc->idle_timer);
    arena_free(&pc->arena);
    arena_free(&pc->err_arena);
    Func done = pc->hook_done;
    mem_free(a, pc, sizeof *pc, _Alignof(tp_PConn));
    if (!BURROW_FUNC_IS_NIL(done))
        BURROW_CALLF0(done);
    sync_wait_group_done(&t->live);
}

static void tp_pc_unref(tp_PConn *pc) {
    if (sync_atomic_int64_add(&pc->refs, -1) != 0)
        return;
    tp_pc_free(pc);
}

/* persistConn.Read, what br reads from: the connection, and no more of it than
 * read_limit, which the read loop sets for each response's header. */
static Int tp_pc_read(void *self, Slice p, Error *err) {
    tp_PConn *pc = (tp_PConn *)self;
    if (pc->read_limit <= 0) {
        BURROW_OUT(err, fmt_errorf_v("read limit of %d bytes exhausted",
                                     tp_max_header_response_size(pc->t)));
        return 0;
    }
    if ((int64_t)p.len > pc->read_limit)
        p.len = (Int)pc->read_limit;
    Error e = BURROW_NO_ERROR;
    Int n = pc->conn.vt->reader.read(pc->conn.data, p, &e);
    if (tp_same(e, io_eof))
        pc->saw_eof = true;
    pc->read_limit -= (int64_t)n;
    BURROW_OUT(err, e);
    return n;
}

static const IoReaderVT tp_pc_reader_vt = {NULL, tp_pc_read};

/* persistConnWriter, what bw writes to, which counts what went out. */
static Int tp_pc_write(void *self, Slice p, Error *err) {
    tp_PConn *pc = (tp_PConn *)self;
    Int n = pc->conn.vt->writer.write(pc->conn.data, p, err);
    (void)sync_atomic_int64_add(&pc->nwrite, (int64_t)n);
    return n;
}

static const IoWriterVT tp_pc_writer_vt = {NULL, tp_pc_write};

/* isBroken. */
static bool tp_pc_is_broken(tp_PConn *pc) {
    sync_mutex_lock(&pc->mu);
    bool b = BURROW_FAILED(pc->closed);
    sync_mutex_unlock(&pc->mu);
    return b;
}

/* canceled. The error is pc's, and lives as long as it does. */
static Error tp_pc_canceled(tp_PConn *pc) {
    sync_mutex_lock(&pc->mu);
    Error e = pc->canceled_err;
    sync_mutex_unlock(&pc->mu);
    return e;
}

/* isReused and markReused. */
static bool tp_pc_is_reused(tp_PConn *pc) {
    sync_mutex_lock(&pc->mu);
    bool r = pc->reused;
    sync_mutex_unlock(&pc->mu);
    return r;
}

static void tp_pc_mark_reused(tp_PConn *pc) {
    sync_mutex_lock(&pc->mu);
    pc->reused = true;
    sync_mutex_unlock(&pc->mu);
}

/* cancelRequest. */
static void tp_pc_cancel_request(tp_PConn *pc, Error err) {
    sync_mutex_lock(&pc->mu);
    pc->canceled_err = error_retain(arena_allocator(&pc->err_arena), err);
    tp_pc_close_locked(pc, burrow__http_err_request_canceled);
    sync_mutex_unlock(&pc->mu);
}

/* close and closeLocked. The connection is closed, unless a 101 response has
 * handed it to its caller, and closech with it. err is only for the curious. */
static void tp_pc_close(tp_PConn *pc, Error err) {
    sync_mutex_lock(&pc->mu);
    tp_pc_close_locked(pc, err);
    sync_mutex_unlock(&pc->mu);
}

static void tp_pc_close_locked(tp_PConn *pc, Error err) {
    if (BURROW_FAILED(pc->closed))
        return;
    pc->closed = error_retain(arena_allocator(&pc->err_arena), err);
    if (!pc->uncounted)
        tp_dec_conns_per_host(pc->t, pc->key);
    /* HTTP/2 closes its connection itself. */
    if (!pc->alt && !tp_same(err, burrow__http_err_caller_owns_conn))
        (void)pc->conn.vt->closer.close(pc->conn.data);
    chan_close(pc->closech);
}

/* ------------------------------------------------------------ the idle pool */

/* An idle timer that is armed has a reference on its connection, which the
 * callback gives back when it has run, and a stop that catches it in time
 * gives back instead. It counts in t's live as well, so that the transport
 * outlives the callback's last look at it. */
static void tp_idle_token_take(tp_PConn *pc) {
    tp_pc_ref(pc);
    sync_wait_group_add(&pc->t->live, 1);
}

static void tp_idle_token_drop(tp_PConn *pc) {
    HttpTransport *t = pc->t;
    tp_pc_unref(pc);
    sync_wait_group_done(&t->live);
}

/* An HTTP/2 connection has no read loop to keep it alive while it is in the
 * pool, so the pool has a reference on it, which this gives back when it
 * leaves. Under idle_mu. */
static void tp_idle_release(tp_PConn *pc) {
    if (pc->alt)
        tp_pc_unref(pc);
}

/* closeConnIfStillIdle, which the idle timer runs. HTTP/2 has no idle timer
 * here, as its own transport has one. */
static bool tp_remove_idle_locked(HttpTransport *t, tp_PConn *pc);

static void tp_close_if_still_idle(void *env) {
    tp_PConn *pc = (tp_PConn *)env;
    HttpTransport *t = pc->t;
    sync_mutex_lock(&t->idle_mu);
    if (pc->in_lru) {
        (void)tp_remove_idle_locked(t, pc);
        tp_pc_close(pc, burrow__http_err_idle_conn_timeout);
    }
    sync_mutex_unlock(&t->idle_mu);
    tp_pc_unref(pc);
    sync_wait_group_done(&t->live);
}

/* Arms the idle timer, or again when it is armed already. Under idle_mu. */
static void tp_arm_idle_timer(HttpTransport *t, tp_PConn *pc) {
    tp_idle_token_take(pc);
    if (pc->idle_timer == NULL) {
        pc->idle_timer = time_after_func(tp_alloc(t), t->idle_conn_timeout,
                                         BURROW_FN(Func, tp_close_if_still_idle, pc));
        if (pc->idle_timer == NULL)
            tp_idle_token_drop(pc);
        return;
    }
    /* A timer still waiting has its token already, and one that is not armed
     * now gives back the one it had. */
    bool pending = false;
    bool ok = time_timer_reset(pc->idle_timer, t->idle_conn_timeout, &pending);
    if (!ok || pending)
        tp_idle_token_drop(pc);
    if (!ok && pending)
        tp_idle_token_drop(pc);
}

static void tp_stop_idle_timer(tp_PConn *pc) {
    if (pc->idle_timer != NULL && time_timer_stop(pc->idle_timer))
        tp_idle_token_drop(pc);
}

/* removeIdleConnLocked. The timer is stopped last, since what it gives back
 * may be what keeps pc alive for the rest. */
static bool tp_remove_idle_locked(HttpTransport *t, tp_PConn *pc) {
    tp_lru_remove(t, pc);
    bool removed = false;
    tp_IdleBucket *b = tp_idle_bucket(t, pc->key, false);
    if (b != NULL) {
        for (Int i = 0; i < b->n; i++) {
            if (b->conns[i] != pc)
                continue;
            /* Slide down, keeping the ones used last at the end. */
            memmove(&b->conns[i], &b->conns[i + 1],
                    (size_t)(b->n - i - 1) * sizeof *b->conns);
            b->n--;
            removed = true;
            break;
        }
        tp_idle_bucket_trim(t, b);
    }
    tp_stop_idle_timer(pc);
    if (removed)
        tp_idle_release(pc);
    return removed;
}

/* removeIdleConn marks pconn as dead. */
static bool tp_remove_idle(HttpTransport *t, tp_PConn *pc) {
    if (pc->is_client_conn)
        return true;
    sync_mutex_lock(&t->idle_mu);
    bool removed = tp_remove_idle_locked(t, pc);
    sync_mutex_unlock(&t->idle_mu);
    return removed;
}

/* maxIdleConnsPerHost. */
static Int tp_max_idle_conns_per_host(const HttpTransport *t) {
    return t->max_idle_conns_per_host != 0 ? t->max_idle_conns_per_host
                                           : HTTP_DEFAULT_MAX_IDLE_CONNS_PER_HOST;
}

static bool tp_want_try_deliver(tp_Want *w, tp_PConn *pc, Error err, Time idle_at);

static Error tp_try_put_idle_conn_locked(HttpTransport *t, tp_PConn *pc) {
    /* Again, now under idle_mu. The read loop closes a connection and then
     * takes it out of the pool under idle_mu, so one that is closed by now
     * would stay in the pool after its read loop has gone. */
    if (tp_pc_is_broken(pc))
        return burrow__http_err_conn_broken;
    /* HTTP/2 connections do not come out of the pool when they are used, as
     * many requests can share one, so one given back is there already. */
    if (pc->alt && pc->in_lru)
        return BURROW_NO_ERROR;

    /* Hand pc to a want waiting for an idle connection, if there is one. It
     * may be dialing as well, but this one is ready first. An HTTP/2 one goes
     * to every want in line, and in the pool after for the ones to come. */
    tp_IdleBucket *b = tp_idle_bucket(t, pc->key, false);
    if (b != NULL && tp_queue_len(&b->wait) > 0) {
        bool done = false;
        while (!done && tp_queue_len(&b->wait) > 0) {
            tp_Want *w = tp_queue_pop(&b->wait);
            bool delivered = tp_want_try_deliver(w, pc, BURROW_NO_ERROR, (Time){0});
            done = delivered && !pc->alt;
            tp_want_unref(w);
        }
        if (done) {
            tp_idle_bucket_trim(t, b);
            return BURROW_NO_ERROR;
        }
    }

    if (t->close_idle) {
        if (b != NULL)
            tp_idle_bucket_trim(t, b);
        return burrow__http_err_close_idle;
    }
    if (b == NULL)
        b = tp_idle_bucket(t, pc->key, true);
    if (b == NULL)
        return burrow_err_out_of_memory;
    if (b->n >= tp_max_idle_conns_per_host(t)) {
        tp_idle_bucket_trim(t, b);
        return burrow__http_err_too_many_idle_host;
    }
    for (Int i = 0; i < b->n; i++)
        if (b->conns[i] == pc)
            panic_str(BURROW_S("dup idle pconn in freelist"));
    if (!tp_idle_bucket_append(t, b, pc)) {
        tp_idle_bucket_trim(t, b);
        return burrow_err_out_of_memory;
    }
    tp_lru_add(t, pc);
    if (pc->alt)
        tp_pc_ref(pc);
    if (t->max_idle_conns != 0 && t->lru_len > t->max_idle_conns) {
        tp_PConn *oldest = tp_lru_remove_oldest(t);
        tp_pc_close(oldest, burrow__http_err_too_many_idle);
        (void)tp_remove_idle_locked(t, oldest);
    }
    if (t->idle_conn_timeout > 0 && !pc->alt)
        tp_arm_idle_timer(t, pc);
    pc->idle_at = time_now();
    return BURROW_NO_ERROR;
}

/* tryPutIdleConn. Puts pc in the pool, or gives the reason it did not. */
static Error tp_try_put_idle_conn(HttpTransport *t, tp_PConn *pc) {
    if (t->disable_keep_alives || t->max_idle_conns_per_host < 0)
        return burrow__http_err_keep_alives_disabled;
    if (tp_pc_is_broken(pc))
        return burrow__http_err_conn_broken;
    tp_pc_mark_reused(pc);
    if (pc->is_client_conn) {
        /* internalStateHook is always set for conns created by NewClientConn. */
        sync_mutex_lock(&pc->mu);
        if (!pc->in_flight)
            panic_str(BURROW_S("pconn is not in flight"));
        pc->in_flight = false;
        bool v = true;
        if (!chan_try_send(pc->availch, &v))
            panic_str(BURROW_S("unable to make pconn available"));
        sync_mutex_unlock(&pc->mu);
        BURROW_CALLF0(pc->state_hook);
        return BURROW_NO_ERROR;
    }
    sync_mutex_lock(&t->idle_mu);
    Error err = tp_try_put_idle_conn_locked(t, pc);
    sync_mutex_unlock(&t->idle_mu);
    return err;
}

/* putOrCloseIdleConn. */
static void tp_put_or_close_idle_conn(HttpTransport *t, tp_PConn *pc) {
    Error err = tp_try_put_idle_conn(t, pc);
    if (BURROW_FAILED(err))
        tp_pc_close(pc, err);
}

/* queueForIdleConn. Hands w the idle connection used last, or puts it in line
 * for the next one, and says whether it handed one over. */
static bool tp_queue_for_idle_conn(HttpTransport *t, tp_Want *w) {
    if (t->disable_keep_alives)
        return false;
    sync_mutex_lock(&t->idle_mu);
    /* Stop closing connections that go idle, as one may be wanted. */
    t->close_idle = false;

    /* The oldest idle_at a connection may have and still be used. Only the
     * wall clock counts, in case this is a laptop coming out of sleep. */
    bool has_old = t->idle_conn_timeout > 0;
    Time old_time = time_now();
    if (has_old)
        old_time = time_add(old_time, -t->idle_conn_timeout);

    tp_IdleBucket *b = tp_idle_bucket(t, w->key, false);
    if (b != NULL && b->n > 0) {
        bool stop = false;
        bool delivered = false;
        while (b->n > 0 && !stop) {
            tp_PConn *pc = b->conns[b->n - 1];
            bool too_old = has_old && time_before(time_round(pc->idle_at, 0), old_time);
            if (too_old) {
                /* Go closes it from a goroutine of its own, as if its timer had
                 * fired. It is closed here and now instead. */
                tp_lru_remove(t, pc);
                tp_pc_close(pc, burrow__http_err_idle_conn_timeout);
                tp_stop_idle_timer(pc);
            }
            if (too_old || tp_pc_is_broken(pc)) {
                /* Either way it is on its way to being closed. */
                b->n--;
                tp_idle_release(pc);
                continue;
            }
            delivered = tp_want_try_deliver(w, pc, BURROW_NO_ERROR, pc->idle_at);
            /* An HTTP/2 one can be shared, so it stays. */
            if (delivered && !pc->alt) {
                tp_lru_remove(t, pc);
                b->n--;
                tp_stop_idle_timer(pc);
            }
            stop = true;
        }
        if (stop) {
            tp_idle_bucket_trim(t, b);
            sync_mutex_unlock(&t->idle_mu);
            return delivered;
        }
    }

    /* Wait for the next connection to go idle. A want that cannot wait still
     * gets the one it dials. */
    if (b == NULL)
        b = tp_idle_bucket(t, w->key, true);
    if (b != NULL) {
        tp_queue_clean_front_not_waiting(&b->wait);
        (void)tp_queue_push(t, &b->wait, w);
        tp_idle_bucket_trim(t, b);
    }
    sync_mutex_unlock(&t->idle_mu);
    return false;
}

/* ------------------------------------------------------------------ wantConn */

static void tp_want_free(tp_Want *w) {
    HttpTransport *t = w->t;
    for (int i = w->nctx - 1; i >= 0; i--)
        context_release(w->ctxs[i]);
    chan_free(w->ready);
    arena_free(&w->arena);
    arena_free(&w->err_arena);
    mem_free(tp_alloc(t), w, sizeof *w, _Alignof(tp_Want));
}

static void tp_want_unref(tp_Want *w) {
    if (sync_atomic_int64_add(&w->refs, -1) != 0)
        return;
    tp_want_free(w);
}

/* tryDeliver. Gives w a reference on pc, or a copy of err, unless it has had
 * one already or been canceled. idle_at is when pc went in the pool, for one
 * that comes from there, and the zero Time otherwise. */
static bool tp_want_try_deliver(tp_Want *w, tp_PConn *pc, Error err, Time idle_at) {
    sync_mutex_lock(&w->mu);
    if (w->done) {
        sync_mutex_unlock(&w->mu);
        return false;
    }
    if ((pc == NULL) == BURROW_OK(err))
        panic_str(BURROW_S("net/http: internal error: misuse of tryDeliver"));
    w->done = true;
    w->has_result = true;
    if (pc != NULL) {
        tp_pc_ref(pc);
        w->res_pc = pc;
        w->res_idle_at = idle_at;
    } else {
        w->res_err = error_retain(arena_allocator(&w->err_arena), err);
    }
    chan_close(w->ready);
    sync_mutex_unlock(&w->mu);
    return true;
}

/* cancel. A connection handed over and not taken goes back to the pool. */
static void tp_want_cancel(tp_Want *w) {
    tp_PConn *pc = NULL;
    sync_mutex_lock(&w->mu);
    if (w->done) {
        pc = w->res_pc;
        w->res_pc = NULL;
    } else {
        chan_close(w->ready);
    }
    w->done = true;
    sync_mutex_unlock(&w->mu);
    /* An HTTP/2 one was never taken out of the pool, and if it is not there
     * now, it was taken out for an error. */
    if (pc != NULL) {
        if (!pc->alt)
            tp_put_or_close_idle_conn(w->t, pc);
        tp_pc_unref(pc);
    }
}

/* The want whose trace_proxy.burrow_net self is. */
static tp_Want *tp_want_of_trace(const void *self) {
    return (tp_Want *)((uintptr_t)self - offsetof(tp_Want, trace_proxy) -
                       offsetof(HttptraceClientTrace, burrow_net));
}

static void tp_trace_dns_start(const void *self, Str host) {
    tp_Want *w = tp_want_of_trace(self);
    sync_mutex_lock(&w->trace_mu);
    httptrace_client_trace_dns_start(w->trace_to,
                                     (HttptraceDNSStartInfo){.host = host});
    sync_mutex_unlock(&w->trace_mu);
}

static void tp_trace_dns_done(const void *self, Slice addrs, bool coalesced,
                              Error err) {
    tp_Want *w = tp_want_of_trace(self);
    sync_mutex_lock(&w->trace_mu);
    httptrace_client_trace_dns_done(
        w->trace_to,
        (HttptraceDNSDoneInfo){.addrs = addrs, .err = err, .coalesced = coalesced});
    sync_mutex_unlock(&w->trace_mu);
}

static void tp_trace_connect_start(const void *self, Str network, Str addr) {
    tp_Want *w = tp_want_of_trace(self);
    sync_mutex_lock(&w->trace_mu);
    httptrace_client_trace_connect_start(w->trace_to, network, addr);
    sync_mutex_unlock(&w->trace_mu);
}

static void tp_trace_connect_done(const void *self, Str network, Str addr, Error err) {
    tp_Want *w = tp_want_of_trace(self);
    sync_mutex_lock(&w->trace_mu);
    httptrace_client_trace_connect_done(w->trace_to, network, addr, err);
    sync_mutex_unlock(&w->trace_mu);
}

static const burrow__NetTraceVT tp_trace_proxy_vt = {
    tp_trace_dns_start,
    tp_trace_dns_done,
    tp_trace_connect_start,
    tp_trace_connect_done,
};

/* Puts what the dial is to see of the request's context, from ctx, over the
 * want's cancel context. False when the allocator says no. */
static bool tp_want_dial_values(tp_Want *w, Alloc *a, Context ctx,
                                const HttptraceClientTrace *trace) {
    w->alt = burrow__net_lookup_ip_alt_resolver(ctx);
    if (w->alt.f != NULL) {
        Context c = context_with_value(a, w->ctxs[w->nctx - 1],
                                       burrow__net_lookup_ip_alt_resolver_key,
                                       (Any){TYPE_UINTPTR, (void *)&w->alt});
        if (BURROW_CONTEXT_IS_NIL(c))
            return false;
        w->ctxs[w->nctx++] = c;
    }
    if (BURROW__HTTPTRACE_HAS(trace, dns_start) ||
        BURROW__HTTPTRACE_HAS(trace, dns_done) ||
        BURROW__HTTPTRACE_HAS(trace, connect_start) ||
        BURROW__HTTPTRACE_HAS(trace, connect_done)) {
        w->trace_to = trace;
        w->trace_proxy.burrow_net = &tp_trace_proxy_vt;
        Context c =
            context_with_value(a, w->ctxs[w->nctx - 1], burrow__nettrace_key,
                               (Any){TYPE_UINTPTR, (void *)&w->trace_proxy.burrow_net});
        if (BURROW_CONTEXT_IS_NIL(c))
            return false;
        w->ctxs[w->nctx++] = c;
    }
    /* The TLS events come from the dial itself, with no context value. */
    if (BURROW__HTTPTRACE_HAS(trace, tls_handshake_start) ||
        BURROW__HTTPTRACE_HAS(trace, tls_handshake_done))
        w->trace_to = trace;
    w->ctx = w->ctxs[w->nctx - 1];
    return true;
}

/* Stops the dial's events going to the request's trace, which may be gone
 * once the request has its connection or has given up. */
static void tp_want_detach_trace(tp_Want *w) {
    sync_mutex_lock(&w->trace_mu);
    w->trace_to = NULL;
    sync_mutex_unlock(&w->trace_mu);
}

/* ---------------------------------------------------------------- dialing */

/* What dialConn has for a ClientConn: its internalStateHook, and the done
 * burrow__http_transport_dial_client_conn takes with it. */
typedef struct tp_ClientConnDial {
    Func hook;
    Func done;
} tp_ClientConnDial;

static tp_PConn *tp_dial_conn(HttpTransport *t, Context ctx, const tp_ConnectMethod *cm,
                              tp_Want *w, const tp_ClientConnDial *cd, Error *err);
static void tp_dec_conns_per_host_locked(HttpTransport *t, Str key);

/* dialConnFor. Dials for w, which may have been canceled or given an idle
 * connection by now, and hands it what came of it. The count of connections
 * for its key goes down again when there is no connection to count. */
static void tp_dial_conn_for(HttpTransport *t, tp_Want *w) {
    sync_mutex_lock(&w->mu);
    bool done = w->done;
    sync_mutex_unlock(&w->mu);
    if (done) {
        tp_dec_conns_per_host(t, w->key);
        return;
    }
    Error err = BURROW_NO_ERROR;
    tp_PConn *pc = tp_dial_conn(t, w->ctx, &w->cm, w, NULL, &err);
    bool delivered = tp_want_try_deliver(w, pc, err, (Time){0});
    if (pc != NULL) {
        /* Nobody wanted it, or it is HTTP/2 and can be shared, so it goes in
         * the pool for the next request. */
        if (!delivered || pc->alt)
            tp_put_or_close_idle_conn(t, pc);
        tp_pc_unref(pc);
    } else {
        tp_dec_conns_per_host(t, w->key);
    }
}

static void tp_dial_job(void *env) {
    tp_Want *w = (tp_Want *)env;
    HttpTransport *t = w->t;
    tp_dial_conn_for(t, w);
    sync_mutex_lock(&t->conns_per_host_mu);
    w->has_cancel_ctx = false;
    sync_mutex_unlock(&t->conns_per_host_mu);
    tp_want_unref(w);
}

/* startDialConnForLocked, under conns_per_host_mu. When there is no goroutine
 * to dial with, w has the allocator's error, and the count it was to have goes
 * to the next want in line. */
static void tp_start_dial_conn_for_locked(HttpTransport *t, tp_Want *w) {
    tp_queue_clean_front_canceled(&t->dials);
    if (tp_queue_push(t, &t->dials, w)) {
        (void)sync_atomic_int64_add(&w->refs, 1);
        if (tp_go(t, BURROW_FN(Func, tp_dial_job, w)))
            return;
        (void)sync_atomic_int64_add(&w->refs, -1);
    }
    w->has_cancel_ctx = false;
    (void)tp_want_try_deliver(w, NULL, burrow_err_out_of_memory, (Time){0});
    tp_dec_conns_per_host_locked(t, w->key);
}

/* queueForDial. Dials for w now, or once the connections for its key are
 * fewer than max_conns_per_host. */
static void tp_queue_for_dial(HttpTransport *t, tp_Want *w) {
    sync_mutex_lock(&t->conns_per_host_mu);
    if (t->max_conns_per_host <= 0) {
        tp_start_dial_conn_for_locked(t, w);
        sync_mutex_unlock(&t->conns_per_host_mu);
        return;
    }
    tp_HostBucket *b = tp_host_bucket(t, w->key, true);
    if (b == NULL) {
        (void)tp_want_try_deliver(w, NULL, burrow_err_out_of_memory, (Time){0});
    } else if (b->n < t->max_conns_per_host) {
        b->n++;
        tp_start_dial_conn_for_locked(t, w);
    } else {
        tp_queue_clean_front_not_waiting(&b->wait);
        if (!tp_queue_push(t, &b->wait, w)) {
            (void)tp_want_try_deliver(w, NULL, burrow_err_out_of_memory, (Time){0});
            tp_host_bucket_trim(t, b);
        }
    }
    sync_mutex_unlock(&t->conns_per_host_mu);
}

/* decConnsPerHost, under conns_per_host_mu. The count goes to a want still
 * waiting to dial when there is one. */
static void tp_dec_conns_per_host_locked(HttpTransport *t, Str key) {
    if (t->max_conns_per_host <= 0)
        return;
    tp_HostBucket *b = tp_host_bucket(t, key, false);
    if (b == NULL || b->n == 0)
        panic_str(BURROW_S("net/http: internal error: connCount underflow"));
    while (tp_queue_len(&b->wait) > 0) {
        tp_Want *w = tp_queue_pop(&b->wait);
        bool waiting = tp_want_waiting(w);
        if (waiting)
            tp_start_dial_conn_for_locked(t, w);
        tp_want_unref(w);
        if (waiting)
            return;
    }
    b->n--;
    tp_host_bucket_trim(t, b);
}

static void tp_dec_conns_per_host(HttpTransport *t, Str key) {
    if (t->max_conns_per_host <= 0)
        return;
    sync_mutex_lock(&t->conns_per_host_mu);
    tp_dec_conns_per_host_locked(t, key);
    sync_mutex_unlock(&t->conns_per_host_mu);
}

/* dial. */
static NetConn tp_dial(HttpTransport *t, Context ctx, Str network, Str addr,
                       Error *err) {
    Error e = BURROW_NO_ERROR;
    NetConn c;
    if (t->dial_context.f != NULL) {
        c = BURROW_CALLF(t->dial_context, ctx, network, addr, &e);
        if (c.vt == NULL && BURROW_OK(e))
            e = tp_err_dial_context_nil;
    } else if (t->dial.f != NULL) {
        c = BURROW_CALLF(t->dial, network, addr, &e);
        if (c.vt == NULL && BURROW_OK(e))
            e = tp_err_dial_nil;
    } else {
        NetDialer d;
        memset(&d, 0, sizeof d);
        c = net_dialer_dial_context(&d, tp_alloc(t), ctx, network, addr, &e);
    }
    if (BURROW_FAILED(e) && c.vt != NULL) {
        tp_free_conn(t, c);
        c = (NetConn){NULL, NULL};
    }
    BURROW_OUT(err, e);
    return c;
}

/* customDialTLS. */
static NetConn tp_custom_dial_tls(HttpTransport *t, Context ctx, Str network, Str addr,
                                  Error *err) {
    Error e = BURROW_NO_ERROR;
    NetConn c;
    if (t->dial_tls_context.f != NULL)
        c = BURROW_CALLF(t->dial_tls_context, ctx, network, addr, &e);
    else
        c = BURROW_CALLF(t->dial_tls, network, addr, &e);
    if (c.vt == NULL && BURROW_OK(e))
        e = tp_err_dial_tls_nil;
    if (BURROW_FAILED(e) && c.vt != NULL) {
        tp_free_conn(t, c);
        c = (NetConn){NULL, NULL};
    }
    BURROW_OUT(err, e);
    return c;
}

/* The wrapErr of dialConn: a dial to a proxy fails with an OpError. */
static Error tp_wrap_dial_err(const tp_ConnectMethod *cm, Error err) {
    if (cm->proxy_url == NULL || tp_same(err, burrow_err_out_of_memory))
        return err;
    NetOpError oe;
    memset(&oe, 0, sizeof oe);
    oe.op = BURROW_S("proxyconnect");
    oe.net = BURROW_S("tcp");
    oe.err = err;
    return net_op_error_as_error(&oe, error_allocator());
}

/* What the goroutine that sends a CONNECT and reads its answer shares with
 * tp_proxy_connect, which waits for it. */
typedef struct tp_Connect {
    NetConn conn;
    HttpRequest *req;
    HttpResponse *resp;
    BufioReader *br;
    Alloc *a;
    Error err; /* in a */
    int64_t limit;
    Chan *done;
} tp_Connect;

static void tp_connect_job(void *env) {
    tp_Connect *c = (tp_Connect *)env;
    Error e = http_request_write(c->req, net_conn_as_io_writer(c->conn));
    if (BURROW_OK(e)) {
        /* A buffered reader that goes after the response is fine, since the
         * server does not speak until spoken to once the tunnel is up. */
        IoLimitedReader *lr =
            (IoLimitedReader *)mem_alloc(c->a, sizeof *lr, _Alignof(IoLimitedReader));
        if (lr != NULL) {
            *lr = io_limit_reader(net_conn_as_io_reader(c->conn), c->limit);
            c->br = bufio_new_reader(c->a, io_limited_reader_as_io_reader(lr));
        }
        if (c->br == NULL)
            e = burrow_err_out_of_memory;
        else
            c->resp = http_read_response(c->a, c->br, c->req, &e);
    }
    c->err = error_retain(c->a, e);
    chan_close(c->done);
}

/* The CONNECT to the proxy for an "https" request, which leaves conn a tunnel
 * to the target. */
static Error tp_proxy_connect(HttpTransport *t, Context ctx, const tp_ConnectMethod *cm,
                              NetConn conn) {
    Arena scratch;
    arena_init(&scratch, tp_alloc(t), 0);
    Alloc *sa = arena_allocator(&scratch);
    Error err = BURROW_NO_ERROR;
    Url target;
    memset(&target, 0, sizeof target);
    HttpRequest req;
    memset(&req, 0, sizeof req);
    tp_Connect c;
    memset(&c, 0, sizeof c);
    HttpHeader hdr = NULL;
    if (t->get_proxy_connect_header.f != NULL) {
        hdr = BURROW_CALLF(t->get_proxy_connect_header, ctx, sa, cm->proxy_url,
                           cm->target_addr, &err);
        if (BURROW_FAILED(err))
            goto done;
    } else {
        hdr = t->proxy_connect_header;
    }
    hdr = hdr != NULL ? http_header_clone(hdr, sa) : http_header_make(sa);
    Str pa = burrow__http_proxy_auth(sa, cm->proxy_url);
    target.opaque = cm->target_addr;
    req.method = BURROW_S("CONNECT");
    req.url = &target;
    req.host = cm->target_addr;
    req.header = hdr;
    req.ctx = ctx;
    c.conn = conn;
    c.req = &req;
    c.a = sa;
    c.limit = tp_max_header_response_size(t);
    c.done = chan_make(sa, TYPE_BOOL, 0);
    if (hdr == NULL || c.done == NULL ||
        (pa.len > 0 && !http_header_set(hdr, BURROW_S("Proxy-Authorization"), pa))) {
        err = burrow_err_out_of_memory;
        goto done;
    }

    /* A long time, so that a proxy that stops answering once connected to does
     * not keep the goroutine forever. */
    ContextCancelFunc cancel;
    Context cctx = context_with_timeout(sa, ctx, TIME_MINUTE, &cancel);
    if (!go(BURROW_FN(Func, tp_connect_job, &c))) {
        err = burrow_err_out_of_memory;
    } else {
        SelectCase cases[2] = {BURROW_RECV(context_done(cctx), NULL),
                               BURROW_RECV(c.done, NULL)};
        if (chan_select(cases, 2) == 0) {
            (void)conn.vt->closer.close(conn.data);
            tp_wait(c.done);
            err = context_err(cctx);
        } else {
            err = c.err;
        }
    }
    BURROW_CALLF0(cancel);
    context_release(cctx);
    if (BURROW_FAILED(err))
        goto done;

    if (t->on_proxy_connect_response.f != NULL) {
        err = BURROW_CALLF(t->on_proxy_connect_response, ctx, cm->proxy_url, &req,
                           c.resp);
        if (BURROW_FAILED(err))
            goto done;
    }
    if (c.resp->status_code != 200) {
        Str after = BURROW_STR_EMPTY;
        bool ok = false;
        (void)strings_cut(c.resp->status, BURROW_S(" "), &after, &ok);
        err = ok ? errors_new(error_allocator(), after) : tp_err_unknown_status;
    }

done:
    /* err has to outlive the scratch arena. */
    err = error_retain(error_allocator(), err);
    http_response_free(c.resp);
    if (c.br != NULL)
        bufio_reader_free(c.br);
    arena_free(&scratch);
    return err;
}

static void tp_read_loop(void *env);
static void tp_write_loop(void *env);

/* ------------------------------------------------------------------ TLS */

static const Type tp_tls_next_proto_func_desc = {
    BURROW_S_INIT("func(string, *tls.Conn) RoundTripper"),
    BURROW_S_INIT("net/http"),
    KIND_FUNC,
    (uint32_t)sizeof(HttpTransportTLSNextProtoFunc),
    (uint16_t)_Alignof(HttpTransportTLSNextProtoFunc),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68746e74U, /* "htnt" */
    NULL,
};

const Type *const TYPE_HTTP_TRANSPORT_TLS_NEXT_PROTO_FUNC =
    &tp_tls_next_proto_func_desc;

/* http2ErringRoundTripper, whose round trips all fail with *self. */
static HttpResponse *tp_erring_round_trip(void *self, HttpRequest *req, Error *err) {
    (void)req;
    BURROW_OUT(err, *(const Error *)self);
    return NULL;
}

static const HttpRoundTripperVT tp_erring_rt_vt = {NULL, tp_erring_round_trip};

/* The function configureHTTP2 puts in TLSNextProto["h2"]. Not in Go: it
 * closes and frees c, which it owns. */
static HttpRoundTripper tp_h2_next_proto_stub(void *env, Str authority, TlsConn *c) {
    (void)env;
    (void)authority;
    (void)tls_conn_close(c);
    tls_conn_free(c);
    HttpRoundTripper rt = {&tp_erring_rt_vt,
                           (void *)(uintptr_t)&tp_err_stub_round_tripper};
    return rt;
}

/* Whether t->tls_next_proto has a function that is not nil for proto. */
static bool tp_has_next_proto_func(const HttpTransport *t, Str proto) {
    const HttpTransportTLSNextProtoFunc *fn =
        (const HttpTransportTLSNextProtoFunc *)map_get(t->tls_next_proto, &proto);
    return fn != NULL && fn->f != NULL;
}

static bool tp_has_custom_tls_dialer(const HttpTransport *t) {
    return t->dial_tls.f != NULL || t->dial_tls_context.f != NULL;
}

/* protocols. */
static HttpProtocols tp_protocols(const HttpTransport *t) {
    if (t->protocols != NULL)
        return *t->protocols; /* user-configured set */
    HttpProtocols p = {0};
    http_protocols_set_http1(&p, true); /* default always includes HTTP/1 */
    if (t->tls_next_proto != NULL) {
        /* Setting TLSNextProto to an empty map is a documented way
         * to disable HTTP/2 on a Transport. */
        if (tp_has_next_proto_func(t, BURROW_S("h2")))
            http_protocols_set_http2(&p, true);
    } else if (!t->force_attempt_http2 &&
               (t->tls_client_config != NULL || t->dial.f != NULL ||
                t->dial_context.f != NULL || tp_has_custom_tls_dialer(t))) {
        /* Be conservative and don't automatically enable
         * http2 if they've specified a custom TLS config or
         * custom dialers. Let them opt-in themselves via
         * Transport.Protocols.SetHTTP2(true) so we don't surprise them
         * by modifying their tls.Config. Issue 14275.
         * However, if ForceAttemptHTTP2 is true, it overrides the above
         * checks. */
    } else if (!burrow__http_godebug_http2client_disabled()) {
        http_protocols_set_http2(&p, true);
    }
    return p;
}

/* configureHTTP2. When memory runs out, HTTP/2 is left off. The adjusted
 * NextProtos go in the clone of tls_client_config each connection gets, in
 * tp_add_tls, rather than in tls_client_config itself. */
static void tp_configure_http2(HttpTransport *t, HttpProtocols protocols) {
    Alloc *a = tp_alloc(t);
    if (t->tls_client_config == NULL) {
        TlsConfig *cfg = (TlsConfig *)mem_alloc(a, sizeof *cfg, _Alignof(TlsConfig));
        if (cfg == NULL)
            return;
        memset(cfg, 0, sizeof *cfg);
        t->own_tls_client_config = cfg;
        t->tls_client_config = cfg;
    }
    if (t->tls_next_proto == NULL) {
        Map *m = map_make(a, TYPE_STRING, TYPE_HTTP_TRANSPORT_TLS_NEXT_PROTO_FUNC, 0);
        if (m == NULL)
            return;
        t->own_tls_next_proto = m;
        t->tls_next_proto = m;
    }
    Http2Transport *t2 = burrow__http2_new_transport(t);
    if (t2 == NULL)
        return;
    /* Historically, the presence of a TLSNextProto["h2"] key has been the
     * signal to enable/disable HTTP/2 support. Set a value in the map, but
     * we'll never use it. */
    Str key = BURROW_S("h2");
    HttpTransportTLSNextProtoFunc fn =
        BURROW_FN(HttpTransportTLSNextProtoFunc, tp_h2_next_proto_stub, NULL);
    if (!map_set(t->tls_next_proto, &key, &fn)) {
        burrow__http2_transport_free(t2);
        return;
    }
    sync_mutex_lock(&t->alt_mu);
    t->h2 = t2;
    sync_mutex_unlock(&t->alt_mu);
    t->h2_protocols = protocols;
    t->h2_configured = true;
}

/* onceSetNextProtoDefaults initializes TLSNextProto.
 * It must be called via t.nextProtoOnce.Do.
 *
 * Go looks for an x/net/http2 transport registered for "https" first, which
 * there is no such thing as here. */
static void tp_once_set_next_proto_defaults(void *env) {
    HttpTransport *t = (HttpTransport *)env;
    t->tls_next_proto_was_nil = t->tls_next_proto == NULL;
    if (burrow__http_godebug_http2client_disabled())
        return;
    Str h2 = BURROW_S("h2");
    if (map_get(t->tls_next_proto, &h2) != NULL)
        return; /* There's an existing HTTP/2 implementation installed. */
    HttpProtocols protocols = tp_protocols(t);
    if (!http_protocols_http2(protocols) &&
        !http_protocols_unencrypted_http2(protocols))
        return;
    tp_configure_http2(t, protocols);
}

static void tp_next_proto_defaults(HttpTransport *t) {
    sync_once_do(&t->next_proto_once,
                 BURROW_FN(Func, tp_once_set_next_proto_defaults, t));
}

/* The HTTP/2 transport, which onceSetNextProtoDefaults makes when the
 * protocols have HTTP/2 in them, and NULL otherwise. */
static Http2Transport *tp_h2_transport(HttpTransport *t) {
    sync_mutex_lock(&t->alt_mu);
    Http2Transport *t2 = t->h2;
    sync_mutex_unlock(&t->alt_mu);
    return t2;
}

/* The TLS hooks of the request's trace, while the dial still has one. */
static void tp_trace_tls_handshake_start(tp_Want *w) {
    if (w == NULL)
        return; /* Not in Go: a ClientConn's dial has no trace. */
    sync_mutex_lock(&w->trace_mu);
    httptrace_client_trace_tls_handshake_start(w->trace_to);
    sync_mutex_unlock(&w->trace_mu);
}

static void tp_trace_tls_handshake_done(tp_Want *w, TlsConnectionState cs, Error err) {
    if (w == NULL)
        return;
    sync_mutex_lock(&w->trace_mu);
    httptrace_client_trace_tls_handshake_done(w->trace_to, cs, err);
    sync_mutex_unlock(&w->trace_mu);
}

/* What a TLS connection the transport made keeps until tls_conn_free gives it
 * back: the connection under it and the config it was made with, which are
 * given back after it. */
typedef struct tp_TlsOwn {
    HttpTransport *t;
    NetConn plain;
    TlsConfig *cfg; /* in arena */
    Arena arena;
} tp_TlsOwn;

static void tp_tls_own_free(tp_TlsOwn *o) {
    Alloc *a = tp_alloc(o->t);
    if (o->cfg != NULL)
        tls_config_release(o->cfg);
    arena_free(&o->arena);
    mem_free(a, o, sizeof *o, _Alignof(tp_TlsOwn));
}

/* What tls_conn_free runs last for one of the transport's TLS connections. */
static void tp_tls_on_free(void *env) {
    tp_TlsOwn *o = (tp_TlsOwn *)env;
    tp_free_conn(o->t, o->plain);
    tp_tls_own_free(o);
}

/* The TLS connection in c when it is one of the transport's own, and NULL
 * otherwise. */
static TlsConn *tp_own_tls_conn(NetConn c) {
    TlsConn *tc = tls_conn_from_net_conn(c);
    if (tc == NULL || burrow__tls_conn_on_free(tc).f != tp_tls_on_free)
        return NULL;
    return tc;
}

/* The goroutine of addTLS that runs the handshake, and what it shares with
 * tp_add_tls, which always waits for it. */
typedef struct tp_Handshake {
    TlsConn *tc;
    Context ctx;
    tp_Want *w;
    Chan *done; /* closed once err is set */
    Arena arena;
    Error err; /* in arena */
} tp_Handshake;

static void tp_handshake_job(void *env) {
    tp_Handshake *h = (tp_Handshake *)env;
    tp_trace_tls_handshake_start(h->w);
    Error err = tls_conn_handshake_context(h->tc, h->ctx);
    h->err = error_retain(arena_allocator(&h->arena), err);
    chan_close(h->done);
}

/* Add TLS to a persistent connection, i.e. negotiate a TLS session. If pconn
 * is already a TLS tunnel, this function establishes a nested TLS session
 * inside the encrypted channel. The remote endpoint's name may be overridden
 * by TLSClientConfig.ServerName.
 *
 * Not in Go: when it fails, pc->conn is still the connection it had, closed,
 * for the caller to give back. */
static Error tp_add_tls(tp_PConn *pc, Context ctx, Str name, bool only_h1, tp_Want *w) {
    HttpTransport *t = pc->t;
    Alloc *a = tp_alloc(t);
    tp_TlsOwn *o = (tp_TlsOwn *)mem_alloc(a, sizeof *o, _Alignof(tp_TlsOwn));
    if (o == NULL)
        return burrow_err_out_of_memory;
    memset(o, 0, sizeof *o);
    o->t = t;
    arena_init(&o->arena, a, 0);
    Alloc *oa = arena_allocator(&o->arena);

    /* Initiate TLS and check remote host name against certificate. */
    TlsConfig *cfg;
    if (t->tls_client_config != NULL) {
        cfg = tls_config_clone(t->tls_client_config, oa);
    } else {
        cfg = (TlsConfig *)mem_alloc(oa, sizeof *cfg, _Alignof(TlsConfig));
        if (cfg != NULL)
            memset(cfg, 0, sizeof *cfg);
    }
    o->cfg = cfg;
    if (cfg == NULL) {
        tp_tls_own_free(o);
        return burrow_err_out_of_memory;
    }
    if (cfg->server_name.len == 0) {
        cfg->server_name = str_clone(oa, name);
        if (cfg->server_name.len != name.len) {
            tp_tls_own_free(o);
            return burrow_err_out_of_memory;
        }
    }
    if (t->h2_configured) {
        cfg->next_protos =
            burrow__http_adjust_next_protos(oa, cfg->next_protos, t->h2_protocols);
        if (cfg->next_protos.p == NULL) {
            tp_tls_own_free(o);
            return burrow_err_out_of_memory;
        }
    }
    if (only_h1)
        cfg->next_protos = (Slice){0};
    NetConn plain_conn = pc->conn;
    TlsConn *tls_conn = tls_client(a, plain_conn, cfg);
    if (tls_conn == NULL) {
        tp_tls_own_free(o);
        return burrow_err_out_of_memory;
    }

    tp_Handshake h;
    memset(&h, 0, sizeof h);
    h.tc = tls_conn;
    h.ctx = ctx;
    h.w = w;
    arena_init(&h.arena, a, 0);
    h.done = chan_make(a, TYPE_BOOL, 0);
    /* for canceling TLS handshake. Go's is a time.AfterFunc that sends on
     * the channel the handshake's error comes on, and this is a timer with a
     * channel of its own to wait on with it. */
    TimeTimer *timer = NULL;
    Duration d = t->tls_handshake_timeout;
    if (d != 0)
        timer = time_new_timer(a, d);
    Error err = BURROW_NO_ERROR;
    if (h.done == NULL || (d != 0 && timer == NULL) ||
        !go(BURROW_FN(Func, tp_handshake_job, &h))) {
        err = burrow_err_out_of_memory;
    } else {
        bool timed_out = false;
        if (timer != NULL) {
            SelectCase cases[2] = {BURROW_RECV(h.done, NULL),
                                   BURROW_RECV(time_timer_c(timer), NULL)};
            timed_out = chan_select(cases, 2) == 1;
            (void)time_timer_stop(timer);
        }
        if (timed_out) {
            /* clang-analyzer finds a path where tp_dial gives an empty conn
             * with no error, which it never does.
             * NOLINTNEXTLINE(clang-analyzer-core.NullDereference) */
            (void)plain_conn.vt->closer.close(plain_conn.data);
            /* Now that we have closed the connection,
             * wait for the call to HandshakeContext to return. */
            tp_wait(h.done);
            err = tp_err_tls_handshake_timeout;
        } else {
            tp_wait(h.done);
            err = error_retain(error_allocator(), h.err);
        }
    }
    time_timer_free(timer);
    chan_free(h.done);
    arena_free(&h.arena);
    if (BURROW_FAILED(err)) {
        (void)plain_conn.vt->closer.close(plain_conn.data);
        tp_trace_tls_handshake_done(w, (TlsConnectionState){0}, err);
        tls_conn_free(tls_conn);
        tp_tls_own_free(o);
        return err;
    }
    TlsConnectionState cs = tls_conn_connection_state(tls_conn);
    tp_trace_tls_handshake_done(w, cs, BURROW_NO_ERROR);
    o->plain = plain_conn;
    burrow__tls_conn_set_on_free(tls_conn, BURROW_FN(Func, tp_tls_on_free, o));
    pc->tls_state_v = cs;
    pc->tls_state = &pc->tls_state_v;
    pc->conn = tls_conn_as_net_conn(tls_conn);
    return BURROW_NO_ERROR;
}

/* dialConn. A connection for cm with its loops going, and a reference for the
 * caller, or one that is another round tripper's, with alt set. w is the want
 * it is for, whose trace gets the TLS events, and NULL for a ClientConn, which
 * has cd. Its done is called once whatever happens: by the connection as it
 * is freed, or here when there is none. An HTTP/2 ClientConn is alt_cc, and
 * pc no more than what it came in. */
static tp_PConn *tp_dial_conn(HttpTransport *t, Context ctx, const tp_ConnectMethod *cm,
                              tp_Want *w, const tp_ClientConnDial *cd, Error *err) {
    Alloc *a = tp_alloc(t);
    if (cm->proxy_url != NULL && (str_eq(cm->proxy_url->scheme, BURROW_S("socks5")) ||
                                  str_eq(cm->proxy_url->scheme, BURROW_S("socks5h")))) {
        if (cd != NULL)
            BURROW_CALLF0(cd->done);
        BURROW_OUT(err, tp_err_socks5);
        return NULL;
    }
    tp_PConn *pc = (tp_PConn *)mem_alloc(a, sizeof *pc, _Alignof(tp_PConn));
    if (pc == NULL) {
        if (cd != NULL)
            BURROW_CALLF0(cd->done);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    pc->t = t;
    if (cd != NULL) {
        /* A ClientConn's is not counted against MaxConnsPerHost. */
        pc->is_client_conn = true;
        pc->uncounted = true;
        pc->state_hook = cd->hook;
        pc->hook_done = cd->done;
        pc->availch = chan_make(a, TYPE_BOOL, 1);
    }
    arena_init(&pc->arena, a, 0);
    arena_init(&pc->err_arena, a, 0);
    sync_atomic_int64_store(&pc->refs, 1);
    sync_wait_group_add(&t->live, 1);
    Alloc *pa = arena_allocator(&pc->arena);
    Error e = BURROW_NO_ERROR;
    pc->key = tp_cm_key(pa, cm);
    pc->reqch = chan_make(a, TYPE_UINTPTR, 1);
    pc->writech = chan_make(a, TYPE_UINTPTR, 1);
    pc->closech = chan_make(a, TYPE_BOOL, 0);
    pc->write_err_ch = chan_make(a, TYPE_BOOL, 1);
    pc->write_loop_done = chan_make(a, TYPE_BOOL, 0);
    pc->read_loop_done = chan_make(a, TYPE_BOOL, 0);
    pc->eofc = chan_make(a, TYPE_BOOL, 0);
    Str addr = tp_cm_addr(pa, cm);
    if (pc->key.len == 0 || addr.len == 0 || pc->reqch == NULL || pc->writech == NULL ||
        pc->closech == NULL || pc->write_err_ch == NULL ||
        pc->write_loop_done == NULL || pc->read_loop_done == NULL || pc->eofc == NULL ||
        (cd != NULL && pc->availch == NULL)) {
        e = burrow_err_out_of_memory;
        goto fail;
    }

    bool https = str_eq(tp_cm_scheme(cm), BURROW_S("https"));
    if (https && tp_has_custom_tls_dialer(t)) {
        pc->conn = tp_custom_dial_tls(t, ctx, BURROW_S("tcp"), addr, &e);
        if (BURROW_FAILED(e)) {
            e = tp_wrap_dial_err(cm, e);
            goto fail;
        }
        TlsConn *tc = tls_conn_from_net_conn(pc->conn);
        if (tc != NULL) {
            /* Handshake here, in case DialTLS didn't. TLSNextProto below
             * depends on it for knowing the connection state. */
            tp_trace_tls_handshake_start(w);
            e = tls_conn_handshake_context(tc, ctx);
            if (BURROW_FAILED(e)) {
                /* Go closes the connection in a goroutine of its own. Here
                 * the fail path closes and frees it before returning. */
                tp_trace_tls_handshake_done(w, (TlsConnectionState){0}, e);
                goto fail;
            }
            TlsConnectionState cs = tls_conn_connection_state(tc);
            tp_trace_tls_handshake_done(w, cs, BURROW_NO_ERROR);
            pc->tls_state_v = cs;
            pc->tls_state = &pc->tls_state_v;
        }
    } else {
        pc->conn = tp_dial(t, ctx, BURROW_S("tcp"), addr, &e);
        if (BURROW_FAILED(e)) {
            e = tp_wrap_dial_err(cm, e);
            goto fail;
        }
        if (https) {
            Str first_tls_host = net_split_host_port(addr, NULL, &e);
            if (BURROW_FAILED(e)) {
                e = tp_wrap_dial_err(cm, e);
                goto fail;
            }
            e = tp_add_tls(pc, ctx, first_tls_host, cm->only_h1, w);
            if (BURROW_FAILED(e)) {
                e = tp_wrap_dial_err(cm, e);
                goto fail;
            }
        }
    }

    /* The proxy. */
    if (cm->proxy_url != NULL && str_eq(cm->target_scheme, BURROW_S("http"))) {
        pc->is_proxy = true;
        pc->proxy_auth = burrow__http_proxy_auth(pa, cm->proxy_url);
    } else if (cm->proxy_url != NULL && str_eq(cm->target_scheme, BURROW_S("https"))) {
        e = tp_proxy_connect(t, ctx, cm, pc->conn);
        if (BURROW_FAILED(e))
            goto fail;
    }

    if (cm->proxy_url != NULL && str_eq(cm->target_scheme, BURROW_S("https"))) {
        e = tp_add_tls(pc, ctx, burrow__http_remove_port(cm->target_addr), cm->only_h1,
                       w);
        if (BURROW_FAILED(e))
            goto fail;
    }

    /* Possible unencrypted HTTP/2 with prior knowledge. */
    const HttpProtocols *p = t->protocols;
    bool unencrypted_http2 = pc->tls_state == NULL && p != NULL &&
                             http_protocols_unencrypted_http2(*p) &&
                             !http_protocols_http1(*p);
    bool http2 = unencrypted_http2 ||
                 (pc->tls_state != NULL &&
                  str_eq(pc->tls_state->negotiated_protocol, BURROW_S("h2")));
    Http2Transport *t2 = http2 ? tp_h2_transport(t) : NULL;
    if (t2 != NULL && cd != NULL) {
        /* The connection, and the done, go to the HTTP/2 transport. */
        NetConn c = pc->conn;
        memset(&pc->conn, 0, sizeof pc->conn);
        pc->hook_done = (Func){0};
        Http2ClientConn *cc = burrow__http2_transport_new_client_conn(
            t2, c, cm->target_addr, cd->hook, cd->done, &e);
        if (cc == NULL)
            goto fail;
        pc->alt = true;
        pc->alt_cc = cc;
        BURROW_OUT(err, BURROW_NO_ERROR);
        return pc;
    }
    if (cd != NULL && http2) {
        e = tp_err_h2_no_client_conn;
        goto fail;
    }
    if (t2 != NULL) {
        /* The connection goes to the HTTP/2 transport, and pc is no more than
         * its place in the pool. */
        NetConn c = pc->conn;
        memset(&pc->conn, 0, sizeof pc->conn);
        e = burrow__http2_transport_add_conn(t2, cm->target_scheme, cm->target_addr, c);
        if (BURROW_FAILED(e))
            goto fail;
        pc->alt = true;
        BURROW_OUT(err, BURROW_NO_ERROR);
        return pc;
    }
    if (unencrypted_http2) {
        e = tp_err_unencrypted_h2;
        goto fail;
    }

    const TlsConnectionState *s = pc->tls_state;
    if (s != NULL && s->negotiated_protocol_is_mutual &&
        s->negotiated_protocol.len > 0) {
        TlsConn *tls_conn = tls_conn_from_net_conn(pc->conn);
        const HttpTransportTLSNextProtoFunc *next =
            tls_conn != NULL ? (const HttpTransportTLSNextProtoFunc *)map_get(
                                   t->tls_next_proto, &s->negotiated_protocol)
                             : NULL;
        if (next != NULL && next->f != NULL) {
            /* The function owns the connection from here on. */
            memset(&pc->conn, 0, sizeof pc->conn);
            HttpRoundTripper alt = BURROW_CALLF(*next, cm->target_addr, tls_conn);
            if (alt.vt == &tp_erring_rt_vt) {
                /* pconn.conn was closed by next
                 * (http2configureTransports.upgradeFn). */
                e = *(const Error *)alt.data;
                goto fail;
            }
            pc->alt = true;
            pc->alt_rt = alt;
            BURROW_OUT(err, BURROW_NO_ERROR);
            return pc;
        }
    }

    pc->br = bufio_new_reader_size(a, (IoReader){&tp_pc_reader_vt, pc},
                                   tp_read_buffer_size(t));
    pc->bw = bufio_new_writer_size(a, (IoWriter){&tp_pc_writer_vt, pc},
                                   tp_write_buffer_size(t));
    if (pc->br == NULL || pc->bw == NULL) {
        e = burrow_err_out_of_memory;
        goto fail;
    }

    tp_pc_ref(pc);
    if (!tp_go(t, BURROW_FN(Func, tp_read_loop, pc))) {
        (void)sync_atomic_int64_add(&pc->refs, -1);
        e = burrow_err_out_of_memory;
        goto fail;
    }
    tp_pc_ref(pc);
    if (!tp_go(t, BURROW_FN(Func, tp_write_loop, pc))) {
        /* The read loop is going, so it is the one to free pc, once closing
         * the connection has ended it. The dial gives back its count for the
         * key itself, so the close is not to. */
        (void)sync_atomic_int64_add(&pc->refs, -1);
        sync_mutex_lock(&pc->mu);
        pc->wl_exited = true;
        pc->uncounted = true;
        tp_pc_close_locked(pc, burrow_err_out_of_memory);
        sync_mutex_unlock(&pc->mu);
        chan_close(pc->write_loop_done);
        tp_pc_unref(pc);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return pc;

fail:
    tp_pc_free(pc);
    BURROW_OUT(err, e);
    return NULL;
}

/* ------------------------------------------------------------ the trips */

/* What each loop does with a trip when it is done with it: the read loop and
 * the write loop each close their channel, so that whoever frees the request
 * knows nothing looks at it any more, and give back their reference. */
static void tp_trip_rl_release(tp_Trip *tr) {
    chan_close(tr->rl_done);
    tp_trip_unref(tr);
}

static void tp_trip_wl_release(tp_Trip *tr) {
    chan_close(tr->wl_done);
    tp_trip_unref(tr);
}

/* transportRequest.setError. The first error set is kept. */
static void tp_trip_set_error(tp_Trip *tr, Error err) {
    sync_mutex_lock(&tr->mu);
    if (BURROW_OK(tr->set_err))
        tr->set_err = error_retain(arena_allocator(&tr->wl_arena), err);
    sync_mutex_unlock(&tr->mu);
}

/* transportRequest.cancel, which cancels the call's context. */
static void tp_trip_cancel(tp_Trip *tr, Error cause) {
    if (tr->call != NULL)
        tp_call_cancel(tr->call, cause);
}

/* The read loop's send of its result, which loses to the caller having gone.
 * res, or rl_err when it is NULL, is the result. */
static bool tp_trip_deliver(tp_Trip *tr, HttpResponse *res) {
    tr->res = res;
    bool v = true;
    SelectCase cases[2] = {BURROW_SEND(tr->resc, &v), BURROW_RECV(tr->gone, NULL)};
    if (chan_select(cases, 2) == 0)
        return true;
    tr->res = NULL;
    return false;
}

/* ------------------------------------------------------ the response bodies */

/* bodyEOFSignal. The response body the read loop hands over, which tells it
 * when the body has been read to its end, or closed before then, so that it
 * can read the next response. fn and earlyCloseFn are always the read loop's,
 * so they are written out here. */
struct tp_Body {
    IoReadCloser body; /* the response's own */
    tp_Trip *tr;
    Error rerr; /* in arena, under mu */
    Arena arena;
    SyncMutex mu;
    bool closed; /* under mu */
    bool fn_ran; /* under mu */
};

/* What waitForBodyRead carries. Go sends the error itself, and the read loop
 * only asks whether it is io.EOF, errClosedEarly or something else. */
enum {
    TP_BODY_EOF,          /* io.EOF */
    TP_BODY_CLOSED_EARLY, /* errClosedEarly */
    TP_BODY_ERR           /* any other error */
};

/* fn, called on the final non-nil error from Read. */
static Error tp_body_fn(tp_Body *b, Error err) {
    tp_PConn *pc = b->tr->pc;
    bool is_eof = tp_same(err, io_eof);
    Int outcome = is_eof ? TP_BODY_EOF : TP_BODY_ERR;
    chan_send(b->tr->wait_body, &outcome);
    if (is_eof) {
        tp_wait(pc->eofc);
    } else if (BURROW_FAILED(err)) {
        Error cerr = tp_pc_canceled(pc);
        if (BURROW_FAILED(cerr))
            return cerr;
    }
    return err;
}

/* condfn, with b->mu held. */
static Error tp_body_condfn(tp_Body *b, Error err) {
    if (b->fn_ran)
        return err;
    b->fn_ran = true;
    return tp_body_fn(b, err);
}

static Int tp_body_read(void *self, Slice p, Error *err) {
    tp_Body *b = (tp_Body *)self;
    sync_mutex_lock(&b->mu);
    bool closed = b->closed;
    Error rerr = b->rerr;
    sync_mutex_unlock(&b->mu);
    if (closed) {
        BURROW_OUT(err, burrow__http_err_read_on_closed_res_body);
        return 0;
    }
    if (BURROW_FAILED(rerr)) {
        BURROW_OUT(err, rerr);
        return 0;
    }
    Error e = BURROW_NO_ERROR;
    Int n = b->body.vt->reader.read(b->body.data, p, &e);
    if (BURROW_FAILED(e)) {
        sync_mutex_lock(&b->mu);
        if (BURROW_OK(b->rerr))
            b->rerr = error_retain(arena_allocator(&b->arena), e);
        e = tp_body_condfn(b, e);
        sync_mutex_unlock(&b->mu);
    }
    BURROW_OUT(err, e);
    return n;
}

static Error tp_body_close(void *self) {
    tp_Body *b = (tp_Body *)self;
    sync_mutex_lock(&b->mu);
    if (b->closed) {
        sync_mutex_unlock(&b->mu);
        return BURROW_NO_ERROR;
    }
    b->closed = true;
    Error err;
    if (BURROW_OK(b->rerr)) {
        /* earlyCloseFn, which leaves fn not to run. */
        b->fn_ran = true;
        Int outcome = TP_BODY_CLOSED_EARLY;
        chan_send(b->tr->wait_body, &outcome);
        tp_wait(b->tr->pc->eofc);
        err = BURROW_NO_ERROR;
    } else if (!tp_same(b->rerr, io_eof)) {
        /* Read already returned this error and readLoop gave up the
         * connection. Draining would only read the same error again. */
        err = BURROW_NO_ERROR;
    } else {
        err = tp_body_condfn(b, b->body.vt->closer.close(b->body.data));
    }
    sync_mutex_unlock(&b->mu);
    return err;
}

static const IoReadCloserVT tp_body_vt = {{NULL, tp_body_read}, {NULL, tp_body_close}};

static tp_Body *tp_body_new(HttpTransport *t, tp_Trip *tr, IoReadCloser body) {
    Alloc *a = tp_alloc(t);
    tp_Body *b = (tp_Body *)mem_alloc(a, sizeof *b, _Alignof(tp_Body));
    if (b == NULL)
        return NULL;
    b->body = body;
    b->tr = tr;
    arena_init(&b->arena, a, 0);
    return b;
}

static void tp_body_free(HttpTransport *t, tp_Body *b) {
    if (b == NULL)
        return;
    arena_free(&b->arena);
    mem_free(tp_alloc(t), b, sizeof *b, _Alignof(tp_Body));
}

/* gzipReader. Go keeps its gzip readers in a pool, which only saves work, so
 * here each body makes its own the first time it is read. zerr is
 * burrow__http_err_concurrent_read_on_res_body while a read has zr, and
 * burrow__http_err_read_on_closed_res_body once the body is closed. */
struct tp_Gzip {
    HttpTransport *t;
    tp_Body *body;
    GzipReader *zr; /* under mu */
    Error zerr;     /* in arena, under mu */
    Arena arena;
    SyncMutex mu;
};

static GzipReader *tp_gzip_acquire(tp_Gzip *gz, Error *err) {
    sync_mutex_lock(&gz->mu);
    if (BURROW_FAILED(gz->zerr)) {
        BURROW_OUT(err, gz->zerr);
        sync_mutex_unlock(&gz->mu);
        return NULL;
    }
    if (gz->zr == NULL) {
        /* gzip_new_reader reads the header, which may block for as long as
         * the server likes, so mu is let go meanwhile and zerr keeps other
         * reads out. */
        gz->zerr = burrow__http_err_concurrent_read_on_res_body;
        sync_mutex_unlock(&gz->mu);
        Error e = BURROW_NO_ERROR;
        GzipReader *zr = gzip_new_reader(tp_alloc(gz->t),
                                         (IoReader){&tp_body_vt.reader, gz->body}, &e);
        sync_mutex_lock(&gz->mu);
        if (!tp_same(gz->zerr, burrow__http_err_concurrent_read_on_res_body)) {
            /* Closed meanwhile. */
            gzip_reader_free(zr);
            BURROW_OUT(err, gz->zerr);
            sync_mutex_unlock(&gz->mu);
            return NULL;
        }
        gz->zr = zr;
        gz->zerr = error_retain(arena_allocator(&gz->arena), e);
        if (BURROW_FAILED(gz->zerr)) {
            gzip_reader_free(gz->zr);
            gz->zr = NULL;
            BURROW_OUT(err, gz->zerr);
            sync_mutex_unlock(&gz->mu);
            return NULL;
        }
    }
    GzipReader *ret = gz->zr;
    gz->zr = NULL;
    gz->zerr = burrow__http_err_concurrent_read_on_res_body;
    sync_mutex_unlock(&gz->mu);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return ret;
}

static void tp_gzip_release(tp_Gzip *gz, GzipReader *zr) {
    sync_mutex_lock(&gz->mu);
    if (tp_same(gz->zerr, burrow__http_err_concurrent_read_on_res_body)) {
        gz->zr = zr;
        gz->zerr = BURROW_NO_ERROR;
    } else {
        gzip_reader_free(zr);
    }
    sync_mutex_unlock(&gz->mu);
}

/* gzipReader.close. */
static void tp_gzip_close_reader(tp_Gzip *gz) {
    sync_mutex_lock(&gz->mu);
    if (BURROW_OK(gz->zerr) && gz->zr != NULL) {
        gzip_reader_free(gz->zr);
        gz->zr = NULL;
    }
    gz->zerr = burrow__http_err_read_on_closed_res_body;
    sync_mutex_unlock(&gz->mu);
}

static Int tp_gzip_read(void *self, Slice p, Error *err) {
    tp_Gzip *gz = (tp_Gzip *)self;
    GzipReader *zr = tp_gzip_acquire(gz, err);
    if (zr == NULL)
        return 0;
    Int n = gzip_reader_read(zr, p, err);
    tp_gzip_release(gz, zr);
    return n;
}

static Error tp_gzip_close(void *self) {
    tp_Gzip *gz = (tp_Gzip *)self;
    tp_gzip_close_reader(gz);
    return tp_body_close(gz->body);
}

static const IoReadCloserVT tp_gzip_vt = {{NULL, tp_gzip_read}, {NULL, tp_gzip_close}};

static tp_Gzip *tp_gzip_new(HttpTransport *t, tp_Body *body) {
    Alloc *a = tp_alloc(t);
    tp_Gzip *gz = (tp_Gzip *)mem_alloc(a, sizeof *gz, _Alignof(tp_Gzip));
    if (gz == NULL)
        return NULL;
    gz->t = t;
    gz->body = body;
    arena_init(&gz->arena, a, 0);
    return gz;
}

/* Once nothing reads the body any more. */
static void tp_gzip_free(tp_Gzip *gz) {
    if (gz == NULL)
        return;
    gzip_reader_free(gz->zr);
    arena_free(&gz->arena);
    mem_free(tp_alloc(gz->t), gz, sizeof *gz, _Alignof(tp_Gzip));
}

/* readWriteCloserBody, the body of a 101 response, which is the connection
 * itself after what br has of it already. It is the caller's to write to and
 * close, and its connection goes with the transport's connection when the
 * response is freed. */
typedef struct tp_Rwc {
    BufioReader *br; /* NULL once it has nothing left */
    NetConn conn;
} tp_Rwc;

static Int tp_rwc_read(void *self, Slice p, Error *err) {
    tp_Rwc *b = (tp_Rwc *)self;
    if (b->br != NULL) {
        Int n = bufio_reader_buffered(b->br);
        if (p.len > n)
            p.len = n;
        n = bufio_reader_read(b->br, p, err);
        if (bufio_reader_buffered(b->br) == 0)
            b->br = NULL;
        return n;
    }
    return b->conn.vt->reader.read(b->conn.data, p, err);
}

static Int tp_rwc_write(void *self, Slice p, Error *err) {
    tp_Rwc *b = (tp_Rwc *)self;
    return b->conn.vt->writer.write(b->conn.data, p, err);
}

static Error tp_rwc_close(void *self) {
    tp_Rwc *b = (tp_Rwc *)self;
    return b->conn.vt->closer.close(b->conn.data);
}

/* CloseWrite, when the connection is one that has it. */
static Error tp_rwc_close_write(void *self) {
    tp_Rwc *b = (tp_Rwc *)self;
    NetTCPConn *tc = net_conn_as_tcp_conn(b->conn);
    if (tc != NULL)
        return net_tcp_conn_close_write(tc);
    NetUnixConn *uc = net_conn_as_unix_conn(b->conn);
    if (uc != NULL)
        return net_unix_conn_close_write(uc);
    return errors_err_unsupported;
}

static const IoReadCloserVT tp_rwc_vt = {{NULL, tp_rwc_read}, {NULL, tp_rwc_close}};
static const IoWriterVT tp_rwc_writer_vt = {NULL, tp_rwc_write};

bool http_response_body_writer(const HttpResponse *r, IoWriter *w) {
    if (r == NULL || r->body_writer.vt == NULL)
        return false;
    if (w != NULL)
        *w = r->body_writer;
    return true;
}

/* -------------------------------------------------------- the write loop */

/* waitForContinue. Whether to send the body after "Expect: 100-continue". */
static bool tp_wait_for_continue(void *env) {
    tp_Trip *tr = (tp_Trip *)env;
    tp_PConn *pc = tr->pc;
    TimeTimer *tm = time_new_timer(tp_alloc(pc->t), pc->t->expect_continue_timeout);
    if (tm == NULL)
        return true; /* as if the time were up */
    bool ok = false;
    SelectCase cases[3] = {BURROW_RECV_OK(tr->continue_ch, NULL, &ok),
                           BURROW_RECV(time_timer_c(tm), NULL),
                           BURROW_RECV(pc->closech, NULL)};
    Int i = chan_select(cases, 3);
    (void)time_timer_stop(tm);
    time_timer_free(tm);
    if (i == 0)
        return ok;
    return i == 1;
}

/* writeLoop. */
static void tp_write_loop(void *env) {
    tp_PConn *pc = (tp_PConn *)env;
    for (;;) {
        uintptr_t p = 0;
        SelectCase cases[2] = {BURROW_RECV(pc->writech, &p),
                               BURROW_RECV(pc->closech, NULL)};
        if (chan_select(cases, 2) != 0)
            break;
        tp_Trip *tr = (tp_Trip *)p;
        ArenaMark m = error_mark();
        int64_t start = sync_atomic_int64_load(&pc->nwrite);
        burrow__HttpWaitFunc wait;
        memset(&wait, 0, sizeof wait);
        if (tr->continue_ch != NULL)
            wait = BURROW_FN(burrow__HttpWaitFunc, tp_wait_for_continue, tr);
        Error err = burrow__http_request_write(
            tr->req, bufio_writer_as_io_writer(pc->bw), pc->is_proxy, tr->extra, wait);
        Error inner = BURROW_NO_ERROR;
        if (burrow__http_is_request_body_read_error(err, &inner)) {
            /* An error reading the caller's body comes first, so it is set
             * before the channels below or the close tell anyone of the
             * errors that follow from it. */
            err = inner;
            tp_trip_set_error(tr, err);
        }
        if (BURROW_OK(err))
            err = bufio_writer_flush(pc->bw);
        if (BURROW_FAILED(err) && sync_atomic_int64_load(&pc->nwrite) == start)
            err = tp_wrap(error_allocator(), &tp_nothing_written_vt, err);
        bool ok = BURROW_OK(err);
        sync_mutex_lock(&tr->mu);
        tr->wl_err = error_retain(arena_allocator(&tr->wl_arena), err);
        sync_mutex_unlock(&tr->mu);
        chan_send(pc->write_err_ch, &ok); /* to the body, which may reuse pc */
        chan_send(tr->write_err_ch, &ok); /* to the round trip */
        if (!ok) {
            tp_pc_close(pc, err);
            tp_trip_wl_release(tr);
            break;
        }
        tp_trip_wl_release(tr);
        error_release(m);
    }
    sync_mutex_lock(&pc->mu);
    pc->wl_exited = true;
    sync_mutex_unlock(&pc->mu);
    uintptr_t q = 0;
    while (chan_try_recv(pc->writech, &q, NULL))
        tp_trip_wl_release((tp_Trip *)q);
    chan_close(pc->write_loop_done);
    tp_pc_unref(pc);
}

/* wroteRequest. Whether the last request went out, waiting a little for the
 * write loop to say when it has not yet. */
static bool tp_pc_wrote_request(tp_PConn *pc) {
    bool ok = false;
    if (chan_try_recv(pc->write_err_ch, &ok, NULL))
        return ok;
    TimeTimer *tm =
        time_new_timer(tp_alloc(pc->t), TP_MAX_WRITE_WAIT_BEFORE_CONN_REUSE);
    if (tm == NULL)
        return false;
    SelectCase cases[2] = {BURROW_RECV(pc->write_err_ch, &ok),
                           BURROW_RECV(time_timer_c(tm), NULL)};
    Int i = chan_select(cases, 2);
    (void)time_timer_stop(tm);
    time_timer_free(tm);
    return i == 0 && ok;
}

/* --------------------------------------------------------- the read loop */

/* is408Message. */
static bool tp_is_408_message(Slice buf) {
    const Byte *b = (const Byte *)buf.p;
    if (buf.len < 12)
        return false;
    if (memcmp(b, "HTTP/1.", 7) != 0)
        return false;
    return memcmp(b + 8, " 408", 4) == 0;
}

/* readLoopPeekFailLocked. */
static void tp_read_loop_peek_fail_locked(tp_PConn *pc, Error peek_err) {
    if (BURROW_FAILED(pc->closed))
        return;
    Int n = bufio_reader_buffered(pc->br);
    if (n > 0) {
        Error e = BURROW_NO_ERROR;
        Slice buf = bufio_reader_peek(pc->br, n, &e);
        if (tp_is_408_message(buf)) {
            tp_pc_close_locked(pc, burrow__http_err_server_closed_idle);
            return;
        }
        log_printf_v("Unsolicited response received on idle HTTP channel starting with "
                     "%q; err=%v",
                     str_from_bytes((const Byte *)buf.p, buf.len), peek_err);
    }
    if (tp_same(peek_err, io_eof))
        tp_pc_close_locked(pc, burrow__http_err_server_closed_idle);
    else
        tp_pc_close_locked(pc, fmt_errorf_v("readLoopPeekFailLocked: %w", peek_err));
}

/* readResponse. The final response to tr's request, after any 1xx ones but a
 * 101. */
static HttpResponse *tp_read_response(tp_PConn *pc, tp_Trip *tr, Error *err) {
    const HttptraceClientTrace *trace = httptrace_context_client_trace(tr->ctx);
    if (BURROW__HTTPTRACE_HAS(trace, got_first_response_byte)) {
        Error pe = BURROW_NO_ERROR;
        Slice peek = bufio_reader_peek(pc->br, 1, &pe);
        if (BURROW_OK(pe) && peek.len == 1)
            httptrace_client_trace_got_first_response_byte(trace);
    }

    Chan *continue_ch = tr->continue_ch;
    HttpResponse *resp;
    for (;;) {
        resp = http_read_response(tp_alloc(pc->t), pc->br, tr->req, err);
        if (resp == NULL)
            return NULL;
        Int code = resp->status_code;
        if (continue_ch != NULL && code == HTTP_STATUS_CONTINUE) {
            httptrace_client_trace_got100_continue(trace);
            bool v = true;
            chan_send(continue_ch, &v);
            continue_ch = NULL;
        }
        /* A 101 is the last, see Go's issue 26161. */
        if (100 <= code && code <= 199 && code != HTTP_STATUS_SWITCHING_PROTOCOLS) {
            if (BURROW__HTTPTRACE_HAS(trace, got1xx_response)) {
                Error e =
                    httptrace_client_trace_got1xx_response(trace, code, resp->header);
                if (BURROW_FAILED(e)) {
                    http_response_free(resp);
                    BURROW_OUT(err, e);
                    return NULL;
                }
                /* The hook saw the 1xx, so limiting how many there are is up
                 * to it. Without one, the 1xx headers and the final one
                 * share the limit. */
                pc->read_limit = tp_max_header_response_size(pc->t);
            }
            http_response_free(resp);
            continue;
        }
        break;
    }
    if (tp_is_protocol_switch(resp)) {
        tp_Rwc *b = (tp_Rwc *)mem_alloc(arena_allocator(&resp->arena), sizeof *b,
                                        _Alignof(tp_Rwc));
        if (b == NULL) {
            http_response_free(resp);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        if (bufio_reader_buffered(pc->br) != 0)
            b->br = pc->br;
        b->conn = pc->conn;
        resp->body = (IoReadCloser){&tp_rwc_vt, b};
        resp->body_writer = (IoWriter){&tp_rwc_writer_vt, b};
        if (net_conn_as_tcp_conn(b->conn) != NULL ||
            net_conn_as_unix_conn(b->conn) != NULL)
            resp->body_close_write = tp_rwc_close_write;
    }
    if (continue_ch != NULL) {
        /* The request said "Expect: 100-continue" and the response came
         * without a 100 first. The body goes out if the connection is to be
         * used again, and not if it is to close. A 101 gets the body as well,
         * since it would go once expect_continue_timeout is up anyway. */
        if (resp->close || tr->req->close) {
            chan_close(continue_ch);
        } else {
            bool v = true;
            chan_send(continue_ch, &v);
        }
    }
    resp->tls = pc->tls_state;
    return resp;
}

/* The tryPutIdleConn of readLoop. */
static bool tp_rl_try_put_idle(tp_PConn *pc, tp_Trip *tr, Error *close_err) {
    const HttptraceClientTrace *trace = httptrace_context_client_trace(tr->ctx);
    Error e = tp_try_put_idle_conn(pc->t, pc);
    if (BURROW_FAILED(e)) {
        *close_err = e;
        if (!tp_same(e, burrow__http_err_keep_alives_disabled))
            httptrace_client_trace_put_idle_conn(trace, e);
        return false;
    }
    httptrace_client_trace_put_idle_conn(trace, BURROW_NO_ERROR);
    return true;
}

typedef struct tp_Drain {
    HttpTransport *t;
    IoReadCloser body;
    Chan *done;
    bool drained;
} tp_Drain;

static void tp_drain_job(void *env) {
    tp_Drain *d = (tp_Drain *)env;
    /* When we drain the body and (hopefully) reach EOF, we might
     * potentially need to deal with trailers. Make sure they are discarded
     * so the connection can actually be reused. */
    burrow__http_body_discard_trailer(d->body);
    Error e = BURROW_NO_ERROR;
    (void)io_copy_n(tp_alloc(d->t), io_discard, io_read_closer_as_io_reader(d->body),
                    TP_MAX_POST_CLOSE_READ_BYTES + 1, &e);
    d->drained = tp_same(e, io_eof);
    chan_close(d->done);
}

/* maybeDrainBody. Go leaves the goroutine reading when its time is up, and
 * closing the connection after ends it. The body goes with the response here,
 * which may be freed as soon as the read loop is done with it, so the
 * connection is closed at once and the goroutine waited for. */
static bool tp_maybe_drain_body(tp_PConn *pc, IoReadCloser body, Error close_err) {
    Alloc *a = tp_alloc(pc->t);
    tp_Drain d = {pc->t, body, chan_make(a, TYPE_BOOL, 0), false};
    if (d.done == NULL)
        return false;
    if (!go(BURROW_FN(Func, tp_drain_job, &d))) {
        chan_free(d.done);
        return false;
    }
    Duration wait = pc->t->post_close_read_time;
    if (wait == 0)
        wait = TP_MAX_POST_CLOSE_READ_TIME;
    TimeTimer *tm = time_new_timer(a, wait);
    Int i = 1;
    if (tm != NULL) {
        SelectCase cases[2] = {BURROW_RECV(d.done, NULL),
                               BURROW_RECV(time_timer_c(tm), NULL)};
        i = chan_select(cases, 2);
        (void)time_timer_stop(tm);
        time_timer_free(tm);
    }
    if (i != 0) {
        tp_pc_close(pc, close_err);
        tp_wait(d.done);
    }
    chan_free(d.done);
    return i == 0 && d.drained;
}

/* A response the read loop could not hand over, or whose wrapping failed. */
static void tp_rl_fail(tp_Trip *tr, Error err) {
    tr->rl_err = error_retain(arena_allocator(&tr->rl_arena), err);
    (void)tp_trip_deliver(tr, NULL);
    tp_trip_rl_release(tr);
}

/* readLoop. */
static void tp_read_loop(void *env) {
    tp_PConn *pc = (tp_PConn *)env;
    HttpTransport *t = pc->t;
    Error close_err = burrow__http_err_read_loop_exiting;
    bool alive = true;
    while (alive) {
        ArenaMark m = error_mark();
        pc->read_limit = tp_max_header_response_size(t);
        Error err = BURROW_NO_ERROR;
        (void)bufio_reader_peek(pc->br, 1, &err);

        sync_mutex_lock(&pc->mu);
        if (pc->num_expected == 0) {
            tp_read_loop_peek_fail_locked(pc, err);
            sync_mutex_unlock(&pc->mu);
            break;
        }
        /* Taken under mu, where a failed round trip takes its trip back, so
         * that the two never both think they have it. The trip goes in with
         * num_expected under the same lock, so it is there to be had. */
        uintptr_t p = 0;
        bool have = chan_try_recv(pc->reqch, &p, NULL);
        sync_mutex_unlock(&pc->mu);
        if (!have)
            (void)chan_recv(pc->reqch, &p);
        tp_Trip *tr = (tp_Trip *)p;

        HttpResponse *resp = NULL;
        if (BURROW_OK(err)) {
            resp = tp_read_response(pc, tr, &err);
        } else {
            err = tp_wrap(error_allocator(), &tp_read_from_server_vt, err);
            close_err = err;
        }
        if (BURROW_FAILED(err) || resp == NULL) {
            if (pc->read_limit <= 0)
                err =
                    fmt_errorf_v("net/http: server response headers exceeded %d bytes; "
                                 "aborted",
                                 tp_max_header_response_size(t));
            tp_rl_fail(tr, err);
            break;
        }
        pc->read_limit = INT64_MAX; /* no limit for the body */

        sync_mutex_lock(&pc->mu);
        pc->num_expected--;
        sync_mutex_unlock(&pc->mu);

        bool writable = resp->body.vt == &tp_rwc_vt;
        bool is_connect = str_eq(tr->req->method, BURROW_S("CONNECT"));
        bool has_body =
            !str_eq(tr->req->method, BURROW_S("HEAD")) && resp->content_length != 0;
        if (resp->close || tr->req->close || resp->status_code <= 199 || writable ||
            is_connect) {
            /* No keep-alive when either side asked to close, or after an
             * unexpected 1xx.
             *
             * Don't do keep-alive after sending a CONNECT request.
             * Only a 2xx response converts the connection into a tunnel,
             * but for safety we'll drop the connection even after getting a
             * non-2xx. */
            alive = false;
        }

        if (!has_body || writable) {
            /* Back in the pool before the response goes out, so that a quick
             * next request gets this connection. resc has no buffer, so the
             * round trip is out of its select, which also waits for pc to
             * close, by the time this goroutine can end. */
            alive = alive && !pc->saw_eof && tp_pc_wrote_request(pc) &&
                    tp_rl_try_put_idle(pc, tr, &close_err);
            if (writable)
                close_err = burrow__http_err_caller_owns_conn;
            if (!tp_trip_deliver(tr, resp)) {
                http_response_free(resp);
                tp_trip_rl_release(tr);
                break;
            }
            tp_trip_cancel(tr, burrow__http_err_request_done);
            tp_trip_rl_release(tr);
            error_release(m);
            continue;
        }

        tp_Body *body = tp_body_new(t, tr, resp->body);
        tp_Gzip *gz = NULL;
        bool gzipped = tr->added_gzip &&
                       burrow__http_ascii_equal_fold(
                           http_header_get(resp->header, BURROW_S("Content-Encoding")),
                           BURROW_S("gzip"));
        if (body != NULL && gzipped) {
            gz = tp_gzip_new(t, body);
            if (gz == NULL) {
                tp_body_free(t, body);
                body = NULL;
            }
        }
        if (body == NULL) {
            http_response_free(resp);
            tp_rl_fail(tr, burrow_err_out_of_memory);
            break;
        }
        tr->body = body;
        tr->gz = gz;
        resp->body = (IoReadCloser){&tp_body_vt, body};
        if (gz != NULL) {
            resp->body = (IoReadCloser){&tp_gzip_vt, gz};
            http_header_del(resp->header, BURROW_S("Content-Encoding"));
            http_header_del(resp->header, BURROW_S("Content-Length"));
            resp->content_length = -1;
            resp->uncompressed = true;
        }

        if (!tp_trip_deliver(tr, resp)) {
            http_response_free(resp);
            tp_trip_rl_release(tr);
            break;
        }

        /* Wait for the caller to read the body to its end, or close it, or
         * give up, before reading on. The response is not freed before the
         * read loop is done with the trip. */
        Int outcome = TP_BODY_ERR;
        SelectCase cases[3] = {BURROW_RECV(tr->wait_body, &outcome),
                               BURROW_RECV(context_done(tr->ctx), NULL),
                               BURROW_RECV(pc->closech, NULL)};
        switch (chan_select(cases, 3)) {
        case 0: {
            bool v = true;
            if (outcome == TP_BODY_EOF) {
                alive = alive && !pc->saw_eof && tp_pc_wrote_request(pc) &&
                        tp_rl_try_put_idle(pc, tr, &close_err);
                chan_send(pc->eofc, &v);
            } else if (outcome == TP_BODY_CLOSED_EARLY) {
                /* Read resp before signaling eofc: the send lets the caller's
                 * Close return, and resp belongs to the caller after that. */
                bool try_drain =
                    alive && resp->content_length <= TP_MAX_POST_CLOSE_READ_BYTES;
                chan_send(pc->eofc, &v);
                if (try_drain && tp_maybe_drain_body(pc, body->body, close_err))
                    alive = alive && !pc->saw_eof && tp_pc_wrote_request(pc) &&
                            tp_rl_try_put_idle(pc, tr, &close_err);
                else
                    alive = false;
            } else {
                alive = false;
            }
            break;
        }
        case 1:
            alive = false;
            tp_pc_cancel_request(pc, context_cause(tr->ctx));
            break;
        default:
            alive = false;
            break;
        }

        tp_trip_cancel(tr, burrow__http_err_request_done);
        tp_trip_rl_release(tr);
        error_release(m);
    }

    chan_close(pc->eofc);
    tp_pc_close(pc, close_err);
    (void)tp_remove_idle(t, pc);
    if (!BURROW_FUNC_IS_NIL(pc->state_hook))
        BURROW_CALLF0(pc->state_hook);
    sync_mutex_lock(&pc->mu);
    pc->rl_exited = true;
    sync_mutex_unlock(&pc->mu);
    uintptr_t q = 0;
    while (chan_try_recv(pc->reqch, &q, NULL))
        tp_trip_rl_release((tp_Trip *)q);
    chan_close(pc->read_loop_done);
    tp_pc_unref(pc);
}

/* ------------------------------------------------------- making the trips */

static void tp_trip_free(tp_Trip *tr) {
    HttpTransport *t = tr->t;
    tp_gzip_free(tr->gz);
    tp_body_free(t, tr->body);
    chan_free(tr->resc);
    chan_free(tr->gone);
    chan_free(tr->write_err_ch);
    chan_free(tr->continue_ch);
    chan_free(tr->wait_body);
    chan_free(tr->rl_done);
    chan_free(tr->wl_done);
    arena_free(&tr->arena);
    arena_free(&tr->rl_arena);
    arena_free(&tr->wl_arena);
    if (tr->pc != NULL)
        tp_pc_unref(tr->pc);
    mem_free(tp_alloc(t), tr, sizeof *tr, _Alignof(tp_Trip));
}

static void tp_trip_unref(tp_Trip *tr) {
    if (sync_atomic_int64_add(&tr->refs, -1) != 0)
        return;
    tp_trip_free(tr);
}

/* A try at call's request on pc, which it takes a reference on. NULL when the
 * allocator says no. */
static tp_Trip *tp_trip_new(HttpTransport *t, tp_Call *call, tp_PConn *pc) {
    Alloc *a = tp_alloc(t);
    tp_Trip *tr = (tp_Trip *)mem_alloc(a, sizeof *tr, _Alignof(tp_Trip));
    if (tr == NULL)
        return NULL;
    tr->t = t;
    tr->call = call;
    tr->req = &call->req;
    tr->ctx = call->ctx;
    tp_pc_ref(pc);
    tr->pc = pc;
    sync_atomic_int64_store(&tr->refs, 1);
    arena_init(&tr->arena, a, 0);
    arena_init(&tr->rl_arena, a, 0);
    arena_init(&tr->wl_arena, a, 0);
    tr->resc = chan_make(a, TYPE_BOOL, 0);
    tr->gone = chan_make(a, TYPE_BOOL, 0);
    tr->write_err_ch = chan_make(a, TYPE_BOOL, 1);
    tr->wait_body = chan_make(a, TYPE_INT, 1);
    tr->rl_done = chan_make(a, TYPE_BOOL, 0);
    tr->wl_done = chan_make(a, TYPE_BOOL, 0);
    if (tr->resc == NULL || tr->gone == NULL || tr->write_err_ch == NULL ||
        tr->wait_body == NULL || tr->rl_done == NULL || tr->wl_done == NULL) {
        tp_trip_free(tr);
        return NULL;
    }
    return tr;
}

/* transportRequest.extraHeaders, made the first time it is wanted. False when
 * the allocator says no. */
static bool tp_trip_set_extra(tp_Trip *tr, Str key, Str value) {
    if (tr->extra == NULL) {
        tr->extra = http_header_make(arena_allocator(&tr->arena));
        if (tr->extra == NULL)
            return false;
    }
    return http_header_set(tr->extra, key, value);
}

/* ------------------------------------------------------ persistConn.roundTrip */

/* mapRoundTripError. Waits for the write loop to end first, which it does
 * once the connection is closed, as it is or soon will be for every error
 * this is called with. */
static Error tp_map_round_trip_error(tp_PConn *pc, tp_Trip *tr, int64_t start,
                                     Error err) {
    if (BURROW_OK(err))
        return err;
    tp_wait(pc->write_loop_done);
    Error cerr = tp_pc_canceled(pc);
    if (BURROW_FAILED(cerr))
        return cerr;
    sync_mutex_lock(&tr->mu);
    Error req_err = tr->set_err;
    sync_mutex_unlock(&tr->mu);
    if (BURROW_FAILED(req_err))
        return req_err;
    if (tp_same(err, burrow__http_err_server_closed_idle))
        return err;
    bool nothing = sync_atomic_int64_load(&pc->nwrite) == start;
    if (tp_is_read_from_server(err))
        return nothing ? tp_wrap(error_allocator(), &tp_nothing_written_vt, err) : err;
    if (tp_pc_is_broken(pc)) {
        if (nothing)
            return tp_wrap(error_allocator(), &tp_nothing_written_vt, err);
        return fmt_errorf_v("net/http: HTTP/1.x transport connection broken: %w", err);
    }
    return err;
}

/* The response the read loop sent, or its error mapped. */
static HttpResponse *tp_pc_handle_response(tp_PConn *pc, tp_Trip *tr, int64_t start,
                                           Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (tr->res != NULL)
        return tr->res;
    BURROW_OUT(err, tp_map_round_trip_error(pc, tr, start, tr->rl_err));
    return NULL;
}

/* persistConn.roundTrip. The error may point into tr and pc, so it is to be
 * retained before they go. */
/* After a round trip that failed: takes tr back from a loop that has not
 * picked it up yet, so that nobody has to wait for that loop to be done with
 * it. A read loop only takes the next trip once the connection has something
 * to say, and a connection whose Close does not end a Read in progress, such
 * as the one httputil.DumpRequestOut dials, would never say it. Go does not
 * wait for the loops, so it never meets this. */
static void tp_pc_take_back(tp_PConn *pc, tp_Trip *tr) {
    uintptr_t q = 0;
    bool from_rl = false;
    bool from_wl = false;
    sync_mutex_lock(&pc->mu);
    if (chan_try_recv(pc->reqch, &q, NULL)) {
        if ((tp_Trip *)q != tr)
            runtime_throw(BURROW_S("net/http: a trip that is not ours on reqch"));
        from_rl = true;
        pc->num_expected--;
    }
    if (chan_try_recv(pc->writech, &q, NULL)) {
        if ((tp_Trip *)q != tr)
            runtime_throw(BURROW_S("net/http: a trip that is not ours on writech"));
        from_wl = true;
    }
    sync_mutex_unlock(&pc->mu);
    if (from_rl)
        tp_trip_rl_release(tr);
    if (from_wl)
        tp_trip_wl_release(tr);
}

/* waitForAvailability. The error is pc's, or the context's. */
static Error tp_pc_wait_for_availability(tp_PConn *pc, Context ctx) {
    SelectCase sc[] = {
        BURROW_RECV(pc->availch, NULL),
        BURROW_RECV(pc->closech, NULL),
        BURROW_RECV(context_done(ctx), NULL),
    };
    switch (chan_select(sc, 3)) {
    case 0:
        return BURROW_NO_ERROR;
    case 1:
        return pc->closed;
    default:
        return context_err(ctx);
    }
}

static HttpResponse *tp_pc_round_trip(tp_PConn *pc, tp_Trip *tr, Error *err) {
    HttpTransport *t = pc->t;
    HttpRequest *req = tr->req;
    Alloc *ta = arena_allocator(&tr->arena);
    bool ok = true;

    if (pc->is_client_conn) {
        sync_mutex_lock(&pc->mu);
        if (!pc->reserved) {
            sync_mutex_unlock(&pc->mu);
            Error e = tp_pc_wait_for_availability(pc, tr->ctx);
            if (BURROW_FAILED(e)) {
                chan_close(tr->rl_done);
                chan_close(tr->wl_done);
                BURROW_OUT(err, e);
                return NULL;
            }
            sync_mutex_lock(&pc->mu);
        }
        pc->reserved = false;
        pc->in_flight = true;
        sync_mutex_unlock(&pc->mu);
    }

    /* The headers and channels the trip needs are made before the read loop
     * is told to expect a response, so that there is nothing left to fail
     * once it has been. */
    sync_mutex_lock(&pc->mu);
    Str auth = pc->is_proxy ? tp_dup(ta, pc->proxy_auth) : BURROW_STR_EMPTY;
    ok = auth.len == (pc->is_proxy ? pc->proxy_auth.len : 0);
    sync_mutex_unlock(&pc->mu);
    if (ok && auth.len > 0)
        ok = tp_trip_set_extra(tr, BURROW_S("Proxy-Authorization"), auth);

    /* Ask for gzip when the caller has not asked for an encoding of its own,
     * and take it off again only then. Not for a range, which would fail to
     * decode, or HEAD, for nginx's sake. */
    bool requested_gzip = false;
    if (ok && !t->disable_compression &&
        http_header_get(req->header, BURROW_S("Accept-Encoding")).len == 0 &&
        http_header_get(req->header, BURROW_S("Range")).len == 0 &&
        !str_eq(req->method, BURROW_S("HEAD"))) {
        requested_gzip = true;
        ok = tp_trip_set_extra(tr, BURROW_S("Accept-Encoding"), BURROW_S("gzip"));
    }
    if (ok && http_request_proto_at_least(req, 1, 1) && req->body.vt != NULL &&
        tp_expects_continue(req)) {
        tr->continue_ch = chan_make(tp_alloc(t), TYPE_BOOL, 1);
        ok = tr->continue_ch != NULL;
    }
    if (ok && t->disable_keep_alives && !tp_wants_close(req) &&
        !tp_is_protocol_switch_header(req->header))
        ok = tp_trip_set_extra(tr, BURROW_S("Connection"), BURROW_S("close"));
    if (!ok) {
        chan_close(tr->rl_done);
        chan_close(tr->wl_done);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    tr->added_gzip = requested_gzip;

    /* Write the request while waiting for the response, in case the server
     * answers before it has read the whole body. A loop that has ended takes
     * no more trips, and the closed connection that ended it is what the
     * select below finds. */
    int64_t start = sync_atomic_int64_load(&pc->nwrite);
    uintptr_t p = (uintptr_t)tr;
    sync_mutex_lock(&pc->mu);
    pc->num_expected++;
    bool to_wl = false;
    bool to_rl = false;
    (void)sync_atomic_int64_add(&tr->refs, 2);
    if (!pc->wl_exited)
        to_wl = chan_try_send(pc->writech, &p);
    if (!pc->rl_exited)
        to_rl = chan_try_send(pc->reqch, &p);
    (void)sync_atomic_int64_add(&tr->refs, -(int64_t)(!to_wl) - (int64_t)(!to_rl));
    sync_mutex_unlock(&pc->mu);
    if (!to_wl)
        chan_close(tr->wl_done);
    if (!to_rl)
        chan_close(tr->rl_done);

    TimeTimer *hdr_timer = NULL;
    Chan *ctx_done = context_done(tr->ctx);
    HttpResponse *res = NULL;
    Error e = BURROW_NO_ERROR;
    for (;;) {
        bool wrote = false;
        SelectCase cases[5] = {
            BURROW_RECV(tr->write_err_ch, &wrote),
            BURROW_RECV(pc->closech, NULL),
            BURROW_RECV(hdr_timer != NULL ? time_timer_c(hdr_timer) : NULL, NULL),
            BURROW_RECV(tr->resc, NULL),
            BURROW_RECV(ctx_done, NULL),
        };
        Int i = chan_select(cases, 5);
        if (i == 0) {
            if (!wrote) {
                sync_mutex_lock(&tr->mu);
                Error werr = tr->wl_err;
                sync_mutex_unlock(&tr->mu);
                tp_pc_close(pc, fmt_errorf_v("write error: %w", werr));
                e = tp_map_round_trip_error(pc, tr, start, werr);
                break;
            }
            if (t->response_header_timeout > 0) {
                hdr_timer = time_new_timer(tp_alloc(t), t->response_header_timeout);
                if (hdr_timer == NULL) {
                    tp_pc_close(pc, burrow_err_out_of_memory);
                    e = burrow_err_out_of_memory;
                    break;
                }
            }
        } else if (i == 1) {
            /* The connection closing raced with the response, which is most
             * likely a server that wrote one and closed at once. Use it. */
            if (chan_try_recv(tr->resc, NULL, NULL)) {
                res = tp_pc_handle_response(pc, tr, start, &e);
                break;
            }
            sync_mutex_lock(&pc->mu);
            Error closed = pc->closed;
            sync_mutex_unlock(&pc->mu);
            e = tp_map_round_trip_error(pc, tr, start, closed);
            break;
        } else if (i == 2) {
            tp_pc_close(pc, burrow__http_err_timeout);
            e = burrow__http_err_timeout;
            break;
        } else if (i == 3) {
            res = tp_pc_handle_response(pc, tr, start, &e);
            break;
        } else {
            /* The read loop cancels the context once the body is read, so a
             * response may have beaten it here. */
            if (chan_try_recv(tr->resc, NULL, NULL)) {
                res = tp_pc_handle_response(pc, tr, start, &e);
                break;
            }
            tp_pc_cancel_request(pc, context_cause(tr->ctx));
            ctx_done = NULL; /* closech is next */
        }
    }
    if (hdr_timer != NULL) {
        (void)time_timer_stop(hdr_timer);
        time_timer_free(hdr_timer);
    }
    if (res == NULL)
        tp_pc_take_back(pc, tr);
    BURROW_OUT(err, e);
    return res;
}

/* --------------------------------------------------------- the request body */

/* readTrackingBody. What the request body is wrapped in, to tell whether it
 * was read or closed when the request is to be sent again. */
struct tp_Track {
    tp_Track *next; /* in the call's list */
    IoReadCloser body;
    SyncAtomicBool did_close;
    bool did_read; /* the write loop's, and the caller's once it is done */
};

static Int tp_track_read(void *self, Slice p, Error *err) {
    tp_Track *k = (tp_Track *)self;
    k->did_read = true;
    return k->body.vt->reader.read(k->body.data, p, err);
}

static Error tp_track_close(void *self) {
    tp_Track *k = (tp_Track *)self;
    if (!sync_atomic_bool_compare_and_swap(&k->did_close, false, true))
        return BURROW_NO_ERROR;
    return k->body.vt->closer.close(k->body.data);
}

static const IoReadCloserVT tp_track_vt = {{NULL, tp_track_read},
                                           {NULL, tp_track_close}};

/* Request.closeBody. */
static void tp_close_body(HttpRequest *r) {
    if (r->body.vt != NULL)
        (void)r->body.vt->closer.close(r->body.data);
}

/* Puts body in a tracker as call's request body. False when the allocator
 * says no. */
static bool tp_call_track(tp_Call *call, IoReadCloser body) {
    tp_Track *k =
        (tp_Track *)mem_alloc(tp_alloc(call->t), sizeof *k, _Alignof(tp_Track));
    if (k == NULL)
        return false;
    k->body = body;
    k->next = call->tracks;
    call->tracks = k;
    call->req.body = (IoReadCloser){&tp_track_vt, k};
    return true;
}

/* rewindBody. Gets call's request ready to go again, with a new body from
 * get_body when the one it had was read or closed. */
static Error tp_call_rewind(tp_Call *call) {
    HttpRequest *r = &call->req;
    if (tp_is_no_body(r->body))
        return BURROW_NO_ERROR;
    tp_Track *k = (tp_Track *)r->body.data;
    bool closed = sync_atomic_bool_load(&k->did_close);
    if (!k->did_read && !closed)
        return BURROW_NO_ERROR; /* nothing to rewind */
    if (!closed)
        tp_close_body(r);
    if (r->get_body.f == NULL)
        return burrow__http_err_cannot_rewind;
    Error e = BURROW_NO_ERROR;
    IoReadCloser body = BURROW_CALLF(r->get_body, &e);
    if (BURROW_FAILED(e))
        return e;
    if (!tp_call_track(call, body)) {
        if (body.vt != NULL)
            (void)body.vt->closer.close(body.data);
        return burrow_err_out_of_memory;
    }
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------------------ the calls */

/* Takes call out of t->calls, so that CancelRequest no longer finds it. */
static void tp_call_unlink(tp_Call *call) {
    HttpTransport *t = call->t;
    sync_mutex_lock(&t->req_mu);
    if (call->linked) {
        for (tp_Call **pp = &t->calls; *pp != NULL; pp = &(*pp)->next) {
            if (*pp == call) {
                *pp = call->next;
                break;
            }
        }
        call->linked = false;
    }
    sync_mutex_unlock(&t->req_mu);
}

/* The cancel that prepareTransportCancel makes, which forgets the request as
 * well. */
static void tp_call_cancel(tp_Call *call, Error cause) {
    BURROW_CALLF(call->cancel, cause);
    tp_call_unlink(call);
}

static tp_Call *tp_call_new(HttpTransport *t, HttpRequest *orig) {
    tp_Call *call = (tp_Call *)mem_alloc(tp_alloc(t), sizeof *call, _Alignof(tp_Call));
    if (call == NULL)
        return NULL;
    call->t = t;
    call->orig = orig;
    call->req = *orig;
    return call;
}

static void tp_call_free(tp_Call *call) {
    HttpTransport *t = call->t;
    Alloc *a = tp_alloc(t);
    tp_call_unlink(call);
    if (call->legacy_done != NULL) {
        /* Not in Go: awaitLegacyCancel is done before its context goes. */
        BURROW_CALLF(call->cancel, burrow__http_err_request_done);
        (void)chan_recv(call->legacy_done, NULL);
        chan_free(call->legacy_done);
    }
    context_release(call->ctx);
    while (call->tracks != NULL) {
        tp_Track *k = call->tracks;
        call->tracks = k->next;
        mem_free(a, k, sizeof *k, _Alignof(tp_Track));
    }
    mem_free(a, call, sizeof *call, _Alignof(tp_Call));
}

/* The response's on_free. The body is closed first, which lets the read loop
 * go on, and the request is not freed before both loops are done with it. */
static void tp_call_on_free(void *env) {
    tp_Call *call = (tp_Call *)env;
    tp_Trip *tr = call->trip;
    if (tr->gz != NULL)
        tp_gzip_close_reader(tr->gz);
    if (tr->body != NULL)
        (void)tp_body_close(tr->body);
    tp_wait(tr->rl_done);
    tp_wait(tr->wl_done);
    tp_trip_unref(tr);
    tp_call_free(call);
}

/* The on_free of a response from an alternate round tripper, which keeps the
 * call for as long as the response may point at its request. */
static void tp_call_alt_on_free(void *env) {
    tp_Call *call = (tp_Call *)env;
    if (!BURROW_FUNC_IS_NIL(call->alt_on_free))
        BURROW_CALLF0(call->alt_on_free);
    tp_call_free(call);
}

/* awaitLegacyCancel. */
static void tp_await_legacy_cancel(void *env) {
    tp_Call *call = (tp_Call *)env;
    SelectCase sc[] = {
        BURROW_RECV(call->orig->cancel, NULL),
        BURROW_RECV(context_done(call->ctx), NULL),
    };
    if (chan_select(sc, 2) == 0)
        BURROW_CALLF(call->cancel, burrow__http_err_request_canceled);
    chan_close(call->legacy_done);
}

/* ------------------------------------------------------------------- getConn */

/* The context's cause, as getConn gives it. */
static Error tp_get_conn_cause(Context ctx) {
    Error e = context_cause(ctx);
    if (tp_same(e, burrow__http_err_request_canceled))
        e = burrow__http_err_request_canceled_conn;
    return e;
}

/* getConn. A connection for cm, from the pool or a new dial, whichever comes
 * first, with a reference that is the caller's. The dial goes on when the
 * call gives up, and its connection goes in the pool. Its context comes from
 * the background and not the request's, which may be gone by then, so values
 * in the request's context do not reach dial_context, apart from the copies
 * tp_want_dial_values makes. */
static tp_PConn *tp_get_conn(HttpTransport *t, tp_Call *call,
                             const tp_ConnectMethod *cm, Error *err) {
    Alloc *a = tp_alloc(t);
    tp_Want *w = (tp_Want *)mem_alloc(a, sizeof *w, _Alignof(tp_Want));
    if (w == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    w->t = t;
    sync_atomic_int64_store(&w->refs, 1);
    arena_init(&w->arena, a, 0);
    arena_init(&w->err_arena, a, 0);
    Alloc *wa = arena_allocator(&w->arena);
    bool ok = true;
    if (cm->proxy_url != NULL) {
        w->cm.proxy_url = url_clone(cm->proxy_url, wa);
        ok = w->cm.proxy_url != NULL;
    }
    w->cm.target_scheme = tp_dup(wa, cm->target_scheme);
    w->cm.target_addr = tp_dup(wa, cm->target_addr);
    w->cm.only_h1 = cm->only_h1;
    w->key = tp_cm_key(wa, &w->cm);
    w->ready = chan_make(a, TYPE_BOOL, 0);
    w->ctx = context_with_cancel(a, context_background(), &w->cancel_ctx);
    if (!BURROW_CONTEXT_IS_NIL(w->ctx))
        w->ctxs[w->nctx++] = w->ctx;
    w->has_cancel_ctx = true;
    const HttptraceClientTrace *trace = httptrace_context_client_trace(call->ctx);
    if (!ok || w->cm.target_scheme.len != cm->target_scheme.len ||
        w->cm.target_addr.len != cm->target_addr.len || w->key.len == 0 ||
        w->ready == NULL || BURROW_CONTEXT_IS_NIL(w->ctx) ||
        !tp_want_dial_values(w, a, call->ctx, trace)) {
        tp_want_free(w);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }

    if (BURROW__HTTPTRACE_HAS(trace, get_conn))
        httptrace_client_trace_get_conn(trace, tp_cm_addr(wa, &w->cm));

    if (!tp_queue_for_idle_conn(t, w))
        tp_queue_for_dial(t, w);

    SelectCase cases[2] = {BURROW_RECV(w->ready, NULL),
                           BURROW_RECV(context_done(call->ctx), NULL)};
    tp_PConn *pc = NULL;
    Time idle_at = {0};
    Error e = BURROW_NO_ERROR;
    if (chan_select(cases, 2) == 0) {
        sync_mutex_lock(&w->mu);
        pc = w->res_pc;
        w->res_pc = NULL;
        idle_at = w->res_idle_at;
        e = w->res_err;
        sync_mutex_unlock(&w->mu);
        /* An error that came with the request canceled is the cancel's. */
        if (pc == NULL && BURROW_FAILED(context_err(call->ctx)))
            e = tp_get_conn_cause(call->ctx);
    } else {
        e = tp_get_conn_cause(call->ctx);
    }
    if (pc == NULL) {
        e = error_retain(error_allocator(), e);
        tp_want_cancel(w);
    }
    tp_want_detach_trace(w);
    tp_want_unref(w);
    /* HTTP/2 calls got_conn itself. */
    if (pc != NULL && !pc->alt && BURROW__HTTPTRACE_HAS(trace, got_conn)) {
        HttptraceGotConnInfo info = {.conn = pc->conn, .reused = tp_pc_is_reused(pc)};
        if (!time_is_zero(idle_at)) {
            info.was_idle = true;
            info.idle_time = time_since(idle_at);
        }
        httptrace_client_trace_got_conn(trace, info);
    }
    BURROW_OUT(err, e);
    return pc;
}

/* ---------------------------------------------------------- Transport.roundTrip */

/* Transport.useRegisteredProtocol and alternateRoundTripper. */
static bool tp_alternate_round_tripper(HttpTransport *t, const HttpRequest *req,
                                       HttpRoundTripper *rt) {
    if (str_eq(req->url->scheme, BURROW_S("https")) && tp_requires_http1(req))
        return false;
    bool found = false;
    sync_mutex_lock(&t->alt_mu);
    for (tp_AltProto *p = t->alt; p != NULL; p = p->next) {
        if (str_eq(p->scheme, req->url->scheme)) {
            *rt = p->rt;
            found = true;
            break;
        }
    }
    sync_mutex_unlock(&t->alt_mu);
    return found;
}

/* persistConn.shouldRetryRequest. */
static bool tp_should_retry_request(tp_PConn *pc, const HttpRequest *req, Error err) {
    if (burrow__http2_is_no_cached_conn_error(err))
        return true; /* the HTTP/2 transport had no connection that would take it */
    if (burrow__http_is_err_missing_host(err))
        return false;
    if (!tp_pc_is_reused(pc))
        return false; /* a new connection that failed is not the server's fault */
    if (tp_is_nothing_written(err))
        return burrow__http_request_outgoing_length(req) == 0 ||
               req->get_body.f != NULL;
    if (!tp_is_replayable(req))
        return false;
    if (tp_is_read_from_server(err))
        return true;
    return tp_same(err, burrow__http_err_server_closed_idle);
}

/* connectMethodForRequest, with what it makes in a. */
static Error tp_connect_method_for_request(HttpTransport *t, HttpRequest *req, Alloc *a,
                                           tp_ConnectMethod *cm) {
    memset(cm, 0, sizeof *cm);
    cm->target_scheme = req->url->scheme;
    cm->target_addr = burrow__httpproxy_canonical_addr(a, req->url);
    if (cm->target_addr.len == 0)
        return burrow_err_out_of_memory;
    Error e = BURROW_NO_ERROR;
    if (t->proxy.f != NULL)
        cm->proxy_url = BURROW_CALLF(t->proxy, req, a, &e);
    cm->only_h1 = tp_requires_http1(req);
    return e;
}

/* The checks Transport.roundTrip makes before it sends anything. */
static Error tp_check_request(HttpRequest *req) {
    if (req->url == NULL)
        return tp_err_nil_url;
    if (req->header == NULL)
        return tp_err_nil_header;
    Str scheme = req->url->scheme;
    if (str_eq(scheme, BURROW_S("http")) || str_eq(scheme, BURROW_S("https"))) {
        Str bad = burrow__http_validate_headers(error_allocator(), req->header);
        if (bad.len > 0)
            return fmt_errorf_v("net/http: invalid header %s", bad);
        bad = burrow__http_validate_headers(error_allocator(), req->trailer);
        if (bad.len > 0)
            return fmt_errorf_v("net/http: invalid trailer %s", bad);
    }
    return BURROW_NO_ERROR;
}

/* Transport.roundTrip, from setupRewindBody on. */
/* The HTTP/2 path of a try, on pc, which gives back the reference on it. It
 * says whether there is a response, and otherwise e is the error to give up
 * with, or no error for one to try again after. */
static bool tp_round_trip_h2(HttpTransport *t, tp_Call *call, tp_PConn *pc,
                             HttpResponse **res, Error *e) {
    HttpRequest *req = &call->req;
    Error err = BURROW_NO_ERROR;
    HttpResponse *r =
        pc->alt_rt.vt != NULL
            ? pc->alt_rt.vt->round_trip(pc->alt_rt.data, req, &err)
            : burrow__http2_transport_round_trip(tp_h2_transport(t), req, &err);
    if (r != NULL) {
        tp_pc_unref(pc);
        /* HTTP/2 requests are not cancelable with CancelRequest, so the
         * context is not needed any more. */
        tp_call_cancel(call, burrow__http_err_request_done);
        r->request = call->orig;
        call->alt_on_free = r->on_free;
        r->on_free = BURROW_FN(Func, tp_call_alt_on_free, call);
        *res = r;
        *e = BURROW_NO_ERROR;
        return true;
    }
    err = error_retain(error_allocator(), err);
    if (burrow__http2_is_no_cached_conn_error(err)) {
        /* The pool still had a connection the HTTP/2 transport has let go of.
         * Closing it gives back its count for the key. */
        if (tp_remove_idle(t, pc))
            tp_pc_close(pc, err);
        tp_pc_unref(pc);
        *e = BURROW_NO_ERROR;
        return false;
    }
    bool retry = tp_should_retry_request(pc, req, err);
    tp_pc_unref(pc);
    if (retry) {
        *e = BURROW_NO_ERROR;
        return false;
    }
    if (req->body.vt == &tp_track_vt &&
        !sync_atomic_bool_load(&((tp_Track *)req->body.data)->did_close))
        tp_close_body(req);
    *e = err;
    return false;
}

static HttpResponse *tp_round_trip_call(HttpTransport *t, tp_Call *call, Error *err) {
    HttpRequest *orig = call->orig;
    HttpRequest *req = &call->req;
    Alloc *a = tp_alloc(t);
    Error e = BURROW_NO_ERROR;
    HttpResponse *res = NULL;

    HttpRoundTripper alt;
    if (tp_alternate_round_tripper(t, req, &alt)) {
        res = http_round_tripper_round_trip(alt, req, &e);
        if (!tp_same(e, http_err_skip_alt_protocol)) {
            if (res == NULL) {
                tp_call_free(call);
                BURROW_OUT(err, e);
                return NULL;
            }
            if (res->request == req)
                res->request = orig;
            call->alt_on_free = res->on_free;
            res->on_free = BURROW_FN(Func, tp_call_alt_on_free, call);
            BURROW_OUT(err, e);
            return res;
        }
        e = tp_call_rewind(call);
        if (BURROW_FAILED(e)) {
            tp_call_free(call);
            BURROW_OUT(err, e);
            return NULL;
        }
    }

    Str scheme = req->url->scheme;
    if (!str_eq(scheme, BURROW_S("http")) && !str_eq(scheme, BURROW_S("https"))) {
        tp_close_body(req);
        e = burrow__http_bad_string_error(BURROW_S("unsupported protocol scheme"),
                                          scheme);
        tp_call_free(call);
        BURROW_OUT(err, e);
        return NULL;
    }
    if (req->method.len != 0 && !burrow__http_valid_method(req->method)) {
        tp_close_body(req);
        e = fmt_errorf_v("net/http: invalid method %q", req->method);
        tp_call_free(call);
        BURROW_OUT(err, e);
        return NULL;
    }
    if (req->url->host.len == 0) {
        tp_close_body(req);
        tp_call_free(call);
        BURROW_OUT(err, burrow__http_err_no_host_in_url);
        return NULL;
    }

    call->ctx = context_with_cancel_cause(a, http_request_context(orig), &call->cancel);
    if (BURROW_CONTEXT_IS_NIL(call->ctx)) {
        tp_close_body(req);
        tp_call_free(call);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    /* Convert Request.Cancel into context cancellation. */
    if (orig->cancel != NULL) {
        call->legacy_done = chan_make(a, TYPE_BOOL, 0);
        if (call->legacy_done == NULL ||
            !go(BURROW_FN(Func, tp_await_legacy_cancel, call))) {
            chan_free(call->legacy_done);
            call->legacy_done = NULL;
            tp_close_body(req);
            tp_call_free(call);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
    }
    /* prepareTransportCancel. */
    sync_mutex_lock(&t->req_mu);
    call->next = t->calls;
    t->calls = call;
    call->linked = true;
    sync_mutex_unlock(&t->req_mu);

    Arena scratch;
    arena_init(&scratch, a, 0);
    for (;;) {
        if (BURROW_FAILED(context_err(call->ctx))) {
            tp_close_body(req);
            e = context_cause(call->ctx);
            break;
        }
        tp_ConnectMethod cm;
        e = tp_connect_method_for_request(t, req, arena_allocator(&scratch), &cm);
        if (BURROW_FAILED(e)) {
            tp_close_body(req);
            break;
        }
        tp_PConn *pc = tp_get_conn(t, call, &cm, &e);
        if (pc == NULL) {
            tp_close_body(req);
            break;
        }
        if (pc->alt) {
            if (tp_round_trip_h2(t, call, pc, &res, &e)) {
                arena_free(&scratch);
                BURROW_OUT(err, BURROW_NO_ERROR);
                return res;
            }
            if (BURROW_FAILED(e))
                break;
            e = tp_call_rewind(call);
            if (BURROW_FAILED(e))
                break;
            continue;
        }
        tp_Trip *tr = tp_trip_new(t, call, pc);
        if (tr == NULL) {
            tp_put_or_close_idle_conn(t, pc);
            tp_pc_unref(pc);
            tp_close_body(req);
            e = burrow_err_out_of_memory;
            break;
        }
        tp_pc_unref(pc);
        res = tp_pc_round_trip(pc, tr, &e);
        chan_close(tr->gone);
        if (res != NULL) {
            call->trip = tr;
            res->request = orig;
            res->on_free = BURROW_FN(Func, tp_call_on_free, call);
            arena_free(&scratch);
            BURROW_OUT(err, BURROW_NO_ERROR);
            return res;
        }

        /* Neither loop looks at the request after this. */
        tp_wait(tr->rl_done);
        tp_wait(tr->wl_done);
        e = error_retain(error_allocator(), e);
        bool retry = tp_should_retry_request(pc, req, e);
        tp_trip_unref(tr);
        if (!retry) {
            if (tp_is_nothing_written(e))
                e = tp_wrapped_unwrap(e.data);
            if (tp_is_read_from_server(e))
                e = tp_wrapped_unwrap(e.data);
            if (req->body.vt == &tp_track_vt &&
                !sync_atomic_bool_load(&((tp_Track *)req->body.data)->did_close))
                tp_close_body(req);
            break;
        }
        e = tp_call_rewind(call);
        if (BURROW_FAILED(e))
            break;
    }
    arena_free(&scratch);
    e = error_retain(error_allocator(), e);
    tp_call_cancel(call, e);
    tp_call_free(call);
    BURROW_OUT(err, e);
    return NULL;
}

HttpResponse *http_transport_round_trip(HttpTransport *t, HttpRequest *req,
                                        Error *err) {
    tp_next_proto_defaults(t);
    Error e = tp_check_request(req);
    if (BURROW_FAILED(e)) {
        tp_close_body(req);
        BURROW_OUT(err, e);
        return NULL;
    }
    tp_Call *call = tp_call_new(t, req);
    if (call == NULL ||
        (!tp_is_no_body(req->body) && !tp_call_track(call, req->body))) {
        if (call != NULL)
            tp_call_free(call);
        tp_close_body(req);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    return tp_round_trip_call(t, call, err);
}

static HttpResponse *tp_rt_round_trip(void *self, HttpRequest *req, Error *err) {
    return http_transport_round_trip((HttpTransport *)self, req, err);
}

static const HttpRoundTripperVT tp_rt_vt = {NULL, tp_rt_round_trip};

HttpTransport *burrow__http_as_transport(HttpRoundTripper rt) {
    return rt.vt == &tp_rt_vt ? (HttpTransport *)rt.data : NULL;
}

bool burrow__http_transport_alternate(HttpTransport *t, const HttpRequest *req,
                                      HttpRoundTripper *rt) {
    return tp_alternate_round_tripper(t, req, rt);
}

HttpRoundTripper http_transport_as_round_tripper(HttpTransport *t) {
    HttpRoundTripper rt = {&tp_rt_vt, t};
    return rt;
}

/* ------------------------------------------------------------ ClientConn */

/* http1ClientConn.RoundTrip. As with Transport.RoundTrip, the response's
 * on_free waits for both loops to be done with req. */
static HttpResponse *tp_cc_round_trip(void *self, HttpRequest *req, Error *err) {
    tp_PConn *pc = (tp_PConn *)self;
    HttpTransport *t = pc->t;
    Alloc *a = tp_alloc(t);
    tp_Call *call = tp_call_new(t, req);
    if (call == NULL ||
        (!tp_is_no_body(req->body) && !tp_call_track(call, req->body))) {
        if (call != NULL)
            tp_call_free(call);
        tp_close_body(req);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }

    /* Convert Request.Cancel into context cancellation. */
    call->ctx = context_with_cancel_cause(a, http_request_context(req), &call->cancel);
    if (BURROW_CONTEXT_IS_NIL(call->ctx)) {
        tp_close_body(&call->req);
        tp_call_free(call);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    if (req->cancel != NULL) {
        call->legacy_done = chan_make(a, TYPE_BOOL, 0);
        if (call->legacy_done == NULL ||
            !go(BURROW_FN(Func, tp_await_legacy_cancel, call))) {
            chan_free(call->legacy_done);
            call->legacy_done = NULL;
            tp_close_body(&call->req);
            tp_call_free(call);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
    }

    tp_Trip *tr = tp_trip_new(t, call, pc);
    if (tr == NULL) {
        tp_close_body(&call->req);
        tp_call_free(call);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    Error e = BURROW_NO_ERROR;
    HttpResponse *res = tp_pc_round_trip(pc, tr, &e);
    chan_close(tr->gone);
    if (res != NULL) {
        call->trip = tr;
        res->request = req;
        res->on_free = BURROW_FN(Func, tp_call_on_free, call);
        BURROW_OUT(err, BURROW_NO_ERROR);
        return res;
    }

    /* Not in Go: neither loop looks at the request after this, and the body
     * is closed if nothing has closed it yet. */
    tp_wait(tr->rl_done);
    tp_wait(tr->wl_done);
    e = error_retain(error_allocator(), e);
    tp_trip_unref(tr);
    if (call->req.body.vt == &tp_track_vt &&
        !sync_atomic_bool_load(&((tp_Track *)call->req.body.data)->did_close))
        tp_close_body(&call->req);
    tp_call_cancel(call, e);
    tp_call_free(call);
    BURROW_OUT(err, e);
    return NULL;
}

static Error tp_cc_close(void *self) {
    tp_pc_close((tp_PConn *)self, tp_err_client_conn_closed);
    return BURROW_NO_ERROR;
}

static Error tp_cc_err(void *self) {
    tp_PConn *pc = (tp_PConn *)self;
    if (chan_try_recv(pc->closech, NULL, NULL))
        return pc->closed;
    return BURROW_NO_ERROR;
}

static Int tp_cc_available(void *self) {
    tp_PConn *pc = (tp_PConn *)self;
    sync_mutex_lock(&pc->mu);
    Int n = BURROW_FAILED(pc->closed) || pc->reserved || pc->in_flight ? 0 : 1;
    sync_mutex_unlock(&pc->mu);
    return n;
}

static Int tp_cc_in_flight(void *self) {
    tp_PConn *pc = (tp_PConn *)self;
    sync_mutex_lock(&pc->mu);
    Int n = BURROW_OK(pc->closed) && (pc->reserved || pc->in_flight) ? 1 : 0;
    sync_mutex_unlock(&pc->mu);
    return n;
}

static Error tp_cc_reserve(void *self) {
    tp_PConn *pc = (tp_PConn *)self;
    sync_mutex_lock(&pc->mu);
    Error e = pc->closed;
    if (BURROW_OK(e)) {
        if (chan_try_recv(pc->availch, NULL, NULL))
            pc->reserved = true;
        else
            e = tp_err_conn_unavailable;
    }
    sync_mutex_unlock(&pc->mu);
    return e;
}

static void tp_cc_release(void *self) {
    tp_PConn *pc = (tp_PConn *)self;
    sync_mutex_lock(&pc->mu);
    if (pc->reserved) {
        bool v = true;
        if (!chan_try_send(pc->availch, &v))
            panic_str(BURROW_S("cannot release reservation"));
        pc->reserved = false;
    }
    sync_mutex_unlock(&pc->mu);
}

/* Not in Go: closes pc and lets go of it. Its loops have the rest. */
static void tp_cc_free(void *self) {
    tp_PConn *pc = (tp_PConn *)self;
    tp_pc_close(pc, tp_err_client_conn_closed);
    tp_pc_unref(pc);
}

static const burrow__HttpGenericClientConnVT tp_cc_vt = {
    tp_cc_close,     tp_cc_err,     tp_cc_round_trip,
    tp_cc_reserve,   tp_cc_release, tp_cc_available,
    tp_cc_in_flight, NULL,          tp_cc_free,
};

Error burrow__http_transport_dial_client_conn(HttpTransport *t, Context ctx,
                                              Str target_scheme, Str target_addr,
                                              Url *proxy_url, Func hook, Func done,
                                              burrow__HttpGenericClientConn *gc) {
    tp_next_proto_defaults(t);
    tp_ConnectMethod cm;
    memset(&cm, 0, sizeof cm);
    cm.target_scheme = target_scheme;
    cm.target_addr = target_addr;
    cm.proxy_url = proxy_url;
    tp_ClientConnDial cd = {hook, done};
    Error e = BURROW_NO_ERROR;
    tp_PConn *pc = tp_dial_conn(t, ctx, &cm, NULL, &cd, &e);
    if (pc == NULL)
        return e;
    if (pc->alt) {
        /* If pconn.alt is set, this is a connection implemented in another
         * package. */
        void *cc = pc->alt_cc;
        tp_pc_unref(pc);
        if (cc == NULL)
            return tp_err_not_client_conn;
        gc->vt = &burrow__http2_client_conn_vt;
        gc->self = cc;
        return BURROW_NO_ERROR;
    }
    /* This is an HTTP/1 connection. */
    bool v = true;
    (void)chan_try_send(pc->availch, &v);
    gc->vt = &tp_cc_vt;
    gc->self = pc;
    return BURROW_NO_ERROR;
}

/* ---------------------------------------------------- the rest of Transport */

void http_transport_close_idle_connections(HttpTransport *t) {
    tp_next_proto_defaults(t);
    /* A connection in the pool has a read loop, which takes it out under
     * idle_mu, or the pool's reference for HTTP/2, so it is still there while
     * this holds idle_mu. */
    tp_PConn *closing = NULL;
    sync_mutex_lock(&t->idle_mu);
    t->close_idle = true; /* and close the ones that go idle from now on */
    tp_IdleBucket *b = t->idle;
    while (b != NULL) {
        tp_IdleBucket *next = b->next;
        while (b->n > 0) {
            tp_PConn *pc = b->conns[--b->n];
            tp_lru_remove(t, pc);
            tp_pc_close(pc, burrow__http_err_close_idle_conns);
            tp_stop_idle_timer(pc);
            if (!pc->alt) {
                tp_pc_ref(pc);
                pc->closing_next = closing;
                closing = pc;
            }
            tp_idle_release(pc);
        }
        tp_idle_bucket_trim(t, b);
        b = next;
    }
    sync_mutex_unlock(&t->idle_mu);

    /* Not in Go. The loops of the connections closed above end on their own,
     * and Go's collector frees each connection when they have. Here a loop
     * holds the last reference, and a program that returns before the loop
     * has run leaves the connection to be freed by nobody. So this waits for
     * them, which is not long: the connection is closed, so the read loop's
     * read fails and the write loop sees closech. Not under idle_mu, which the
     * read loop takes on its way out. */
    while (closing != NULL) {
        tp_PConn *pc = closing;
        closing = pc->closing_next;
        tp_wait(pc->read_loop_done);
        tp_wait(pc->write_loop_done);
        tp_pc_unref(pc);
    }

    /* Dials nobody waits for any more are stopped too. */
    sync_mutex_lock(&t->conns_per_host_mu);
    for (Int i = 0; i < t->dials.len; i++) {
        tp_Want *w = t->dials.w[(t->dials.head + i) % t->dials.cap];
        if (w->has_cancel_ctx && !tp_want_waiting(w))
            BURROW_CALLF0(w->cancel_ctx);
    }
    sync_mutex_unlock(&t->conns_per_host_mu);

    /* And the HTTP/2 transport's. */
    sync_mutex_lock(&t->alt_mu);
    Http2Transport *t2 = t->h2;
    sync_mutex_unlock(&t->alt_mu);
    if (t2 != NULL)
        burrow__http2_transport_close_idle_connections(t2);
}

void http_transport_cancel_request(HttpTransport *t, HttpRequest *req) {
    sync_mutex_lock(&t->req_mu);
    for (tp_Call **pp = &t->calls; *pp != NULL; pp = &(*pp)->next) {
        tp_Call *call = *pp;
        if (call->orig != req)
            continue;
        /* Under req_mu, which the call is taken out under before it is
         * freed. */
        BURROW_CALLF(call->cancel, burrow__http_err_request_canceled);
        *pp = call->next;
        call->linked = false;
        break;
    }
    sync_mutex_unlock(&t->req_mu);
}

bool http_transport_register_protocol(HttpTransport *t, Str scheme,
                                      HttpRoundTripper rt) {
    Alloc *a = tp_alloc(t);
    bool ok = false;
    sync_mutex_lock(&t->alt_mu);
    bool dup = false;
    for (tp_AltProto *p = t->alt; p != NULL; p = p->next)
        dup = dup || str_eq(p->scheme, scheme);
    tp_AltProto *p = NULL;
    if (!dup)
        p = (tp_AltProto *)mem_alloc(a, sizeof *p, _Alignof(tp_AltProto));
    if (p != NULL) {
        p->scheme = tp_dup(a, scheme);
        if (p->scheme.len == scheme.len) {
            p->rt = rt;
            p->next = t->alt;
            t->alt = p;
            ok = true;
        } else {
            mem_free(a, p, sizeof *p, _Alignof(tp_AltProto));
        }
    }
    sync_mutex_unlock(&t->alt_mu);
    return ok;
}

HttpTransport http_transport_clone(HttpTransport *t, Alloc *a) {
    tp_next_proto_defaults(t);
    HttpTransport t2;
    memset(&t2, 0, sizeof t2);
    t2.proxy = t->proxy;
    t2.on_proxy_connect_response = t->on_proxy_connect_response;
    t2.dial_context = t->dial_context;
    t2.dial = t->dial;
    t2.dial_tls_context = t->dial_tls_context;
    t2.dial_tls = t->dial_tls;
    t2.free_conn = t->free_conn;
    t2.tls_handshake_timeout = t->tls_handshake_timeout;
    t2.disable_keep_alives = t->disable_keep_alives;
    t2.disable_compression = t->disable_compression;
    t2.max_idle_conns = t->max_idle_conns;
    t2.max_idle_conns_per_host = t->max_idle_conns_per_host;
    t2.max_conns_per_host = t->max_conns_per_host;
    t2.idle_conn_timeout = t->idle_conn_timeout;
    t2.response_header_timeout = t->response_header_timeout;
    t2.expect_continue_timeout = t->expect_continue_timeout;
    if (t->proxy_connect_header != NULL)
        t2.proxy_connect_header = http_header_clone(t->proxy_connect_header, a);
    t2.get_proxy_connect_header = t->get_proxy_connect_header;
    t2.max_response_header_bytes = t->max_response_header_bytes;
    t2.force_attempt_http2 = t->force_attempt_http2;
    t2.write_buffer_size = t->write_buffer_size;
    t2.read_buffer_size = t->read_buffer_size;
    t2.protocols = t->protocols;
    t2.http2 = t->http2;
    t2.a = t->a;
    /* Not in Go: what runs out of memory here is left nil. */
    Alloc *ta = tp_alloc(t);
    if (t->tls_client_config != NULL) {
        t2.own_tls_client_config = tls_config_clone(t->tls_client_config, ta);
        t2.tls_client_config = t2.own_tls_client_config;
    }
    if (!t->tls_next_proto_was_nil) {
        Map *npm = map_clone(ta, t->tls_next_proto);
        if (npm == NULL)
            npm = map_make(ta, TYPE_STRING, TYPE_HTTP_TRANSPORT_TLS_NEXT_PROTO_FUNC, 0);
        t2.own_tls_next_proto = npm;
        t2.tls_next_proto = npm;
    }
    return t2;
}

void http_transport_free(HttpTransport *t) {
    http_transport_close_idle_connections(t);
    sync_wait_group_wait(&t->live);
    Alloc *a = tp_alloc(t);
    sync_mutex_lock(&t->idle_mu);
    while (t->idle != NULL) {
        tp_IdleBucket *b = t->idle;
        b->n = 0;
        tp_queue_free(t, &b->wait);
        tp_idle_bucket_trim(t, b);
    }
    sync_mutex_unlock(&t->idle_mu);
    sync_mutex_lock(&t->conns_per_host_mu);
    while (t->conns_per_host != NULL) {
        tp_HostBucket *b = t->conns_per_host;
        b->n = 0;
        tp_queue_free(t, &b->wait);
        tp_host_bucket_trim(t, b);
    }
    tp_queue_free(t, &t->dials);
    sync_mutex_unlock(&t->conns_per_host_mu);
    sync_mutex_lock(&t->alt_mu);
    Http2Transport *t2 = t->h2;
    t->h2 = NULL;
    sync_mutex_unlock(&t->alt_mu);
    if (t2 != NULL)
        burrow__http2_transport_free(t2);
    if (t->own_tls_client_config != NULL) {
        if (t->tls_client_config == t->own_tls_client_config)
            t->tls_client_config = NULL;
        tls_config_release(t->own_tls_client_config);
        mem_free(a, t->own_tls_client_config, sizeof(TlsConfig), _Alignof(TlsConfig));
        t->own_tls_client_config = NULL;
    }
    if (t->own_tls_next_proto != NULL) {
        if (t->tls_next_proto == t->own_tls_next_proto)
            t->tls_next_proto = NULL;
        map_free(t->own_tls_next_proto);
        t->own_tls_next_proto = NULL;
    }
    memset(&t->next_proto_once, 0, sizeof t->next_proto_once);
    t->tls_next_proto_was_nil = false;
    t->h2_configured = false;
    memset(&t->h2_protocols, 0, sizeof t->h2_protocols);
    sync_mutex_lock(&t->alt_mu);
    while (t->alt != NULL) {
        tp_AltProto *p = t->alt;
        t->alt = p->next;
        if (p->scheme.len > 0)
            mem_free(a, (void *)(uintptr_t)p->scheme.p, (size_t)p->scheme.len, 1);
        mem_free(a, p, sizeof *p, _Alignof(tp_AltProto));
    }
    sync_mutex_unlock(&t->alt_mu);
}

/* --------------------------------------------------------- DefaultTransport */

static NetConn tp_default_dial(void *env, Context ctx, Str network, Str addr,
                               Error *err) {
    (void)env;
    NetDialer d;
    memset(&d, 0, sizeof d);
    d.timeout = 30 * TIME_SECOND;
    d.keep_alive = 30 * TIME_SECOND;
    return net_dialer_dial_context(&d, heap_allocator(), ctx, network, addr, err);
}

static Url *tp_env_proxy(void *env, HttpRequest *req, Alloc *a, Error *err);

static HttpTransport tp_default_transport = {
    .proxy = {.f = tp_env_proxy, .env = NULL},
    .dial_context = {.f = tp_default_dial, .env = NULL},
    .force_attempt_http2 = true,
    .max_idle_conns = 100,
    .idle_conn_timeout = 90 * TIME_SECOND,
    .tls_handshake_timeout = 10 * TIME_SECOND,
    .expect_continue_timeout = 1 * TIME_SECOND,
};

HttpTransport *const http_default_transport = &tp_default_transport;

/* ------------------------------------------------------------ the proxies */

/* envProxyFunc's cache, which resetProxyConfig empties. */
typedef struct tp_EnvProxy {
    SyncMutex mu;
    bool loaded;
    Arena arena;
    burrow__Httpproxy *p;
} tp_EnvProxy;

static tp_EnvProxy tp_env_proxy_cache;

Url *http_proxy_from_environment(Alloc *a, HttpRequest *req, Error *err) {
    tp_EnvProxy *c = &tp_env_proxy_cache;
    Error e = BURROW_NO_ERROR;
    Url *out = NULL;
    sync_mutex_lock(&c->mu);
    if (!c->loaded) {
        arena_init(&c->arena, heap_allocator(), 0);
        Alloc *ea = arena_allocator(&c->arena);
        burrow__HttpproxyConfig cfg = burrow__httpproxy_from_environment(ea);
        c->p = burrow__httpproxy_new(ea, &cfg);
        if (c->p == NULL) {
            arena_free(&c->arena);
            sync_mutex_unlock(&c->mu);
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        c->loaded = true;
    }
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    const Url *u =
        burrow__httpproxy_proxy_for_url(c->p, req->url, arena_allocator(&scratch), &e);
    if (u != NULL && BURROW_OK(e)) {
        out = url_clone(u, a);
        if (out == NULL)
            e = burrow_err_out_of_memory;
    }
    sync_mutex_unlock(&c->mu);
    arena_free(&scratch);
    BURROW_OUT(err, e);
    return out;
}

void burrow__http_reset_cached_environment(void) {
    tp_EnvProxy *c = &tp_env_proxy_cache;
    sync_mutex_lock(&c->mu);
    if (c->loaded) {
        arena_free(&c->arena);
        c->p = NULL;
        c->loaded = false;
    }
    sync_mutex_unlock(&c->mu);
}

static Url *tp_env_proxy(void *env, HttpRequest *req, Alloc *a, Error *err) {
    (void)env;
    return http_proxy_from_environment(a, req, err);
}

HttpProxyFunc http_proxy_from_environment_func(void) {
    HttpProxyFunc f = {tp_env_proxy, NULL};
    return f;
}

static Url *tp_fixed_proxy(void *env, HttpRequest *req, Alloc *a, Error *err) {
    (void)req;
    const Url *u = (const Url *)env;
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (u == NULL)
        return NULL;
    Url *c = url_clone(u, a);
    if (c == NULL)
        BURROW_OUT(err, burrow_err_out_of_memory);
    return c;
}

HttpProxyFunc http_proxy_url(const Url *u) {
    HttpProxyFunc f = {tp_fixed_proxy, (void *)(uintptr_t)u};
    return f;
}
