/* net/http's Transport: HTTP/1 requests over connections kept open and used
 * again, through a proxy when one is set.
 *
 * Derived from Go's src/net/http/transport.go and proxy parts of
 * src/net/http/transport.go. Go source: go1.27.1.
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
 * What is left out: HTTP/2 and HTTP/3, TLS of the transport's own, SOCKS5
 * proxies and httptrace. An "https" request needs dial_tls_context or
 * dial_tls until crypto/tls is here.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http_internal.h"

#include "../xnet/httpproxy.h"
#include "http_ascii.h"

#include "burrow/bufio.h"
#include "burrow/chan.h"
#include "burrow/compress/gzip.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/declare.h"
#include "burrow/encoding/base64.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
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
BURROW_SENTINEL_ERROR(burrow__http_err_no_tls,
                      "net/http: no TLS yet; set Transport.dial_tls_context or "
                      "dial_tls to make https connections");
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

/* timeoutError, errTimeout. A net.Error that is a timeout and is
 * context_deadline_exceeded to errors_is. */
typedef struct HttpErrTimeout {
    Byte unused;
} HttpErrTimeout;

static Str tp_timeout_m_error(HttpErrTimeout *self) {
    (void)self;
    return BURROW_S("net/http: timeout awaiting response headers");
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

/* The error lives as long as the program, so a copy is the error itself. */
static Error tp_static_clone(const void *self, Alloc *a);

static const ErrorVT tp_timeout_vt = {
    &tp_timeout_desc, tp_timeout_message, NULL, NULL, tp_timeout_is, NULL,
    tp_static_clone,
};

static const HttpErrTimeout tp_timeout_value = {0};

const Error burrow__http_err_timeout = {&tp_timeout_vt, &tp_timeout_value};

static Error tp_static_clone(const void *self, Alloc *a) {
    (void)a;
    return (Error){&tp_timeout_vt, self};
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
    return fmt_sprintf_v(a, "%s|%s%s|%s", proxy, target_scheme,
                         only_h1 ? BURROW_S(",h1") : BURROW_STR_EMPTY, addr);
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
    for (Int i = 0; i < vs.len; i++)
        if (burrow__http_has_token(((const Str *)vs.p)[i], BURROW_S("Upgrade")))
            return true;
    return false;
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
    tp_PConn *lru_prev, *lru_next;
    Time idle_at;
    bool in_lru;

    bool saw_eof; /* the read loop's */
    bool is_proxy;
    bool reused;    /* under mu */
    bool rl_exited; /* under mu, and reqch takes no more after */
    bool wl_exited; /* under mu, and writech takes no more after */
    bool uncounted; /* the dial gave back the count for key, not the close */
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
    Chan *wait_body;    /* bool, whether the body was read to its end */
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
    tp_Trip *trip;    /* the try that gave the response */
    tp_Track *tracks; /* every body tracker made, freed with the call */
    Func alt_on_free; /* the alternate round tripper's response's on_free */
    bool linked;      /* under t->req_mu */
    HttpRequest req;  /* orig with its body in a tracker */
};

/* wantConn: a call to getConn, which a dial and the idle pool race to hand a
 * connection to, unless it is canceled first. */
struct burrow__HttpWant {
    HttpTransport *t;
    Context ctx; /* for the dial, released with the want */
    ContextCancelFunc cancel_ctx;
    tp_ConnectMethod cm; /* in arena, with the proxy URL a copy */
    Str key;             /* in arena */
    Chan *ready;         /* bool, closed once there is a result */
    tp_PConn *res_pc;    /* a reference, under mu, until it is taken */
    Error res_err;       /* in err_arena, under mu */
    SyncAtomicInt64 refs;
    Arena arena; /* made in before the want is shared */
    Arena err_arena;
    SyncMutex mu;
    bool done;           /* under mu: delivered or canceled */
    bool has_result;     /* under mu */
    bool has_cancel_ctx; /* under t->conns_per_host_mu */
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
        tp_want_unref(tp_queue_pop(q));
    }
}

/* cleanFrontCanceled, under conns_per_host_mu. */
static void tp_queue_clean_front_canceled(tp_Queue *q) {
    for (;;) {
        tp_Want *w = tp_queue_peek(q);
        if (w == NULL || w->has_cancel_ctx)
            return;
        tp_want_unref(tp_queue_pop(q));
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

/* Closes c and gives it back the way the transport gives back each one. */
static void tp_free_conn(HttpTransport *t, NetConn c) {
    if (c.vt == NULL)
        return;
    (void)c.vt->closer.close(c.data);
    if (t->free_conn.f != NULL)
        BURROW_CALLF(t->free_conn, c);
    else
        net_conn_free(c);
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
    chan_free(pc->eofc);
    time_timer_free(pc->idle_timer);
    arena_free(&pc->arena);
    arena_free(&pc->err_arena);
    mem_free(a, pc, sizeof *pc, _Alignof(tp_PConn));
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
    if (!tp_same(err, burrow__http_err_caller_owns_conn))
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

/* closeConnIfStillIdle, which the idle timer runs. */
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
    return removed;
}

/* removeIdleConn. */
static bool tp_remove_idle(HttpTransport *t, tp_PConn *pc) {
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

static bool tp_want_try_deliver(tp_Want *w, tp_PConn *pc, Error err);

static Error tp_try_put_idle_conn_locked(HttpTransport *t, tp_PConn *pc) {
    /* Again, now under idle_mu. The read loop closes a connection and then
     * takes it out of the pool under idle_mu, so one that is closed by now
     * would stay in the pool after its read loop has gone. */
    if (tp_pc_is_broken(pc))
        return burrow__http_err_conn_broken;

    /* Hand pc to a want waiting for an idle connection, if there is one. It
     * may be dialing as well, but this one is ready first. */
    tp_IdleBucket *b = tp_idle_bucket(t, pc->key, false);
    if (b != NULL && tp_queue_len(&b->wait) > 0) {
        bool done = false;
        while (!done && tp_queue_len(&b->wait) > 0) {
            tp_Want *w = tp_queue_pop(&b->wait);
            done = tp_want_try_deliver(w, pc, BURROW_NO_ERROR);
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
    if (t->max_idle_conns != 0 && t->lru_len > t->max_idle_conns) {
        tp_PConn *oldest = tp_lru_remove_oldest(t);
        tp_pc_close(oldest, burrow__http_err_too_many_idle);
        (void)tp_remove_idle_locked(t, oldest);
    }
    if (t->idle_conn_timeout > 0)
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
                continue;
            }
            delivered = tp_want_try_deliver(w, pc, BURROW_NO_ERROR);
            if (delivered) {
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
    context_release(w->ctx);
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
 * one already or been canceled. */
static bool tp_want_try_deliver(tp_Want *w, tp_PConn *pc, Error err) {
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
    if (pc != NULL) {
        tp_put_or_close_idle_conn(w->t, pc);
        tp_pc_unref(pc);
    }
}

/* ---------------------------------------------------------------- dialing */

static tp_PConn *tp_dial_conn(HttpTransport *t, Context ctx, const tp_ConnectMethod *cm,
                              Error *err);
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
    tp_PConn *pc = tp_dial_conn(t, w->ctx, &w->cm, &err);
    bool delivered = tp_want_try_deliver(w, pc, err);
    if (pc != NULL) {
        /* Nobody wanted it, so it goes in the pool for the next request. */
        if (!delivered)
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
    (void)tp_want_try_deliver(w, NULL, burrow_err_out_of_memory);
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
        (void)tp_want_try_deliver(w, NULL, burrow_err_out_of_memory);
    } else if (b->n < t->max_conns_per_host) {
        b->n++;
        tp_start_dial_conn_for_locked(t, w);
    } else {
        tp_queue_clean_front_not_waiting(&b->wait);
        if (!tp_queue_push(t, &b->wait, w)) {
            (void)tp_want_try_deliver(w, NULL, burrow_err_out_of_memory);
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
    hdr = hdr != NULL ? http_header_clone(sa, hdr) : http_header_make(sa);
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

/* dialConn. A connection for cm with its loops going, and a reference for the
 * caller. Only HTTP/1 is spoken, and TLS comes from dial_tls_context or
 * dial_tls, and only to the first hop. */
static tp_PConn *tp_dial_conn(HttpTransport *t, Context ctx, const tp_ConnectMethod *cm,
                              Error *err) {
    Alloc *a = tp_alloc(t);
    if (cm->proxy_url != NULL && (str_eq(cm->proxy_url->scheme, BURROW_S("socks5")) ||
                                  str_eq(cm->proxy_url->scheme, BURROW_S("socks5h")))) {
        BURROW_OUT(err, tp_err_socks5);
        return NULL;
    }
    tp_PConn *pc = (tp_PConn *)mem_alloc(a, sizeof *pc, _Alignof(tp_PConn));
    if (pc == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    pc->t = t;
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
    pc->eofc = chan_make(a, TYPE_BOOL, 0);
    Str addr = tp_cm_addr(pa, cm);
    if (pc->key.len == 0 || addr.len == 0 || pc->reqch == NULL || pc->writech == NULL ||
        pc->closech == NULL || pc->write_err_ch == NULL ||
        pc->write_loop_done == NULL || pc->eofc == NULL) {
        e = burrow_err_out_of_memory;
        goto fail;
    }

    bool https = str_eq(tp_cm_scheme(cm), BURROW_S("https"));
    if (https && (t->dial_tls_context.f != NULL || t->dial_tls.f != NULL)) {
        pc->conn = tp_custom_dial_tls(t, ctx, BURROW_S("tcp"), addr, &e);
        if (BURROW_FAILED(e)) {
            e = tp_wrap_dial_err(cm, e);
            goto fail;
        }
    } else {
        pc->conn = tp_dial(t, ctx, BURROW_S("tcp"), addr, &e);
        if (BURROW_FAILED(e)) {
            e = tp_wrap_dial_err(cm, e);
            goto fail;
        }
        if (https) {
            /* addTLS, which needs crypto/tls. */
            e = tp_wrap_dial_err(cm, burrow__http_err_no_tls);
            goto fail;
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
        /* addTLS over the tunnel, which needs crypto/tls. */
        e = burrow__http_err_no_tls;
        goto fail;
    }

    /* Unencrypted HTTP/2 with prior knowledge, which needs HTTP/2. The
     * connection is never TLS of the transport's own here. */
    const HttpProtocols *p = t->protocols;
    if (p != NULL && http_protocols_unencrypted_http2(*p) &&
        !http_protocols_http1(*p)) {
        e = tp_err_unencrypted_h2;
        goto fail;
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

/* fn. err is nil for a Close at the end of the body. */
static Error tp_body_fn(tp_Body *b, Error err) {
    tp_PConn *pc = b->tr->pc;
    bool is_eof = tp_same(err, io_eof);
    chan_send(b->tr->wait_body, &is_eof);
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
    if (!tp_same(b->rerr, io_eof)) {
        /* earlyCloseFn. */
        bool f = false;
        chan_send(b->tr->wait_body, &f);
        tp_wait(b->tr->pc->eofc);
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

static const IoReadCloserVT tp_rwc_vt = {{NULL, tp_rwc_read}, {NULL, tp_rwc_close}};
static const IoWriterVT tp_rwc_writer_vt = {NULL, tp_rwc_write};

bool http_response_body_writer(const HttpResponse *r, IoWriter *w) {
    if (r == NULL || r->body.vt != &tp_rwc_vt)
        return false;
    if (w != NULL)
        *w = (IoWriter){&tp_rwc_writer_vt, r->body.data};
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
    Chan *continue_ch = tr->continue_ch;
    HttpResponse *resp;
    for (;;) {
        resp = http_read_response(tp_alloc(pc->t), pc->br, tr->req, err);
        if (resp == NULL)
            return NULL;
        Int code = resp->status_code;
        if (continue_ch != NULL && code == HTTP_STATUS_CONTINUE) {
            bool v = true;
            chan_send(continue_ch, &v);
            continue_ch = NULL;
        }
        /* A 101 is the last, see Go's issue 26161. */
        if (100 <= code && code <= 199 && code != HTTP_STATUS_SWITCHING_PROTOCOLS) {
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
    return resp;
}

/* The tryPutIdleConn of readLoop. */
static bool tp_rl_try_put_idle(tp_PConn *pc, Error *close_err) {
    Error e = tp_try_put_idle_conn(pc->t, pc);
    if (BURROW_FAILED(e)) {
        *close_err = e;
        return false;
    }
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
    TimeTimer *tm = time_new_timer(a, TP_MAX_POST_CLOSE_READ_TIME);
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
        sync_mutex_unlock(&pc->mu);

        uintptr_t p = 0;
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
        bool has_body =
            !str_eq(tr->req->method, BURROW_S("HEAD")) && resp->content_length != 0;
        if (resp->close || tr->req->close || resp->status_code <= 199 || writable) {
            /* No keep-alive when either side asked to close, or after an
             * unexpected 1xx. */
            alive = false;
        }

        if (!has_body || writable) {
            /* Back in the pool before the response goes out, so that a quick
             * next request gets this connection. resc has no buffer, so the
             * round trip is out of its select, which also waits for pc to
             * close, by the time this goroutine can end. */
            alive = alive && !pc->saw_eof && tp_pc_wrote_request(pc) &&
                    tp_rl_try_put_idle(pc, &close_err);
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
        bool body_eof = false;
        SelectCase cases[3] = {BURROW_RECV(tr->wait_body, &body_eof),
                               BURROW_RECV(context_done(tr->ctx), NULL),
                               BURROW_RECV(pc->closech, NULL)};
        switch (chan_select(cases, 3)) {
        case 0: {
            bool try_drain =
                !body_eof && resp->content_length <= TP_MAX_POST_CLOSE_READ_BYTES;
            bool v = true;
            if (try_drain) {
                chan_send(pc->eofc, &v);
                body_eof = tp_maybe_drain_body(pc, body->body, close_err);
            }
            alive = alive && body_eof && !pc->saw_eof && tp_pc_wrote_request(pc) &&
                    tp_rl_try_put_idle(pc, &close_err);
            if (!try_drain && body_eof)
                chan_send(pc->eofc, &v);
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
    sync_mutex_lock(&pc->mu);
    pc->rl_exited = true;
    sync_mutex_unlock(&pc->mu);
    uintptr_t q = 0;
    while (chan_try_recv(pc->reqch, &q, NULL))
        tp_trip_rl_release((tp_Trip *)q);
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
    tr->wait_body = chan_make(a, TYPE_BOOL, 2);
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
    if (tr->res != NULL)
        return tr->res;
    BURROW_OUT(err, tp_map_round_trip_error(pc, tr, start, tr->rl_err));
    return NULL;
}

/* persistConn.roundTrip. The error may point into tr and pc, so it is to be
 * retained before they go. */
static HttpResponse *tp_pc_round_trip(tp_PConn *pc, tp_Trip *tr, Error *err) {
    HttpTransport *t = pc->t;
    HttpRequest *req = tr->req;
    Alloc *ta = arena_allocator(&tr->arena);
    bool ok = true;

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
 * in the request's context do not reach dial_context. */
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
    w->has_cancel_ctx = true;
    if (!ok || w->cm.target_scheme.len != cm->target_scheme.len ||
        w->cm.target_addr.len != cm->target_addr.len || w->key.len == 0 ||
        w->ready == NULL || BURROW_CONTEXT_IS_NIL(w->ctx)) {
        tp_want_free(w);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }

    if (!tp_queue_for_idle_conn(t, w))
        tp_queue_for_dial(t, w);

    SelectCase cases[2] = {BURROW_RECV(w->ready, NULL),
                           BURROW_RECV(context_done(call->ctx), NULL)};
    tp_PConn *pc = NULL;
    Error e = BURROW_NO_ERROR;
    if (chan_select(cases, 2) == 0) {
        sync_mutex_lock(&w->mu);
        pc = w->res_pc;
        w->res_pc = NULL;
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
    tp_want_unref(w);
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

HttpRoundTripper http_transport_as_round_tripper(HttpTransport *t) {
    HttpRoundTripper rt = {&tp_rt_vt, t};
    return rt;
}

/* ---------------------------------------------------- the rest of Transport */

void http_transport_close_idle_connections(HttpTransport *t) {
    /* A connection in the pool has a read loop, which takes it out under
     * idle_mu, so it is still there while this holds idle_mu. */
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
        }
        tp_idle_bucket_trim(t, b);
        b = next;
    }
    sync_mutex_unlock(&t->idle_mu);

    /* Dials nobody waits for any more are stopped too. */
    sync_mutex_lock(&t->conns_per_host_mu);
    for (Int i = 0; i < t->dials.len; i++) {
        tp_Want *w = t->dials.w[(t->dials.head + i) % t->dials.cap];
        if (w->has_cancel_ctx && !tp_want_waiting(w))
            BURROW_CALLF0(w->cancel_ctx);
    }
    sync_mutex_unlock(&t->conns_per_host_mu);
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

HttpTransport http_transport_clone(const HttpTransport *t, Alloc *a) {
    HttpTransport t2;
    memset(&t2, 0, sizeof t2);
    t2.proxy = t->proxy;
    t2.on_proxy_connect_response = t->on_proxy_connect_response;
    t2.dial_context = t->dial_context;
    t2.dial = t->dial;
    t2.dial_tls_context = t->dial_tls_context;
    t2.dial_tls = t->dial_tls;
    t2.free_conn = t->free_conn;
    t2.disable_keep_alives = t->disable_keep_alives;
    t2.disable_compression = t->disable_compression;
    t2.max_idle_conns = t->max_idle_conns;
    t2.max_idle_conns_per_host = t->max_idle_conns_per_host;
    t2.max_conns_per_host = t->max_conns_per_host;
    t2.idle_conn_timeout = t->idle_conn_timeout;
    t2.response_header_timeout = t->response_header_timeout;
    t2.expect_continue_timeout = t->expect_continue_timeout;
    if (t->proxy_connect_header != NULL)
        t2.proxy_connect_header = http_header_clone(a, t->proxy_connect_header);
    t2.get_proxy_connect_header = t->get_proxy_connect_header;
    t2.max_response_header_bytes = t->max_response_header_bytes;
    t2.write_buffer_size = t->write_buffer_size;
    t2.read_buffer_size = t->read_buffer_size;
    t2.protocols = t->protocols;
    t2.a = t->a;
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
    .max_idle_conns = 100,
    .idle_conn_timeout = 90 * TIME_SECOND,
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

Url *http_proxy_from_environment(HttpRequest *req, Alloc *a, Error *err) {
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
    return http_proxy_from_environment(req, a, err);
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
