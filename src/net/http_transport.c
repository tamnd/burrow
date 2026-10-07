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
BURROW_SENTINEL_ERROR(burrow__http_err_idle_conn_timeout, "http: idle connection timeout");
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
BURROW_SENTINEL_ERROR(tp_err_dial_nil, "net/http: Transport.Dial hook returned (nil, nil)");
BURROW_SENTINEL_ERROR(tp_err_dial_tls_nil,
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
    NULL, tp_wrapped_message, tp_wrapped_unwrap, NULL, NULL, NULL,
    tp_nothing_written_clone,
};

static const ErrorVT tp_read_from_server_vt = {
    NULL, tp_wrapped_message, tp_wrapped_unwrap, NULL, NULL, NULL,
    tp_read_from_server_clone,
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
    return burrow__http_has_token(burrow__http_header_get(r->header, BURROW_S("Connection")),
                                  BURROW_S("upgrade")) &&
           burrow__http_ascii_equal_fold(
               burrow__http_header_get(r->header, BURROW_S("Upgrade")), BURROW_S("websocket"));
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
    return burrow__http_has_token(burrow__http_header_get(r->header, BURROW_S("Expect")),
                                  BURROW_S("100-continue"));
}

static bool tp_wants_close(const HttpRequest *r) {
    if (r->close)
        return true;
    return burrow__http_has_token(burrow__http_header_get(r->header, BURROW_S("Connection")),
                                  BURROW_S("close"));
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
    Chan *write_err_ch; /* Error, in wl_arena */
    Chan *continue_ch;  /* bool, or NULL without "Expect: 100-continue" */
    Chan *wait_body;    /* bool, whether the body was read to its end */
    Chan *rl_done;      /* closed when the read loop is done with the trip */
    HttpResponse *res;
    tp_Body *body; /* what the response body is wrapped in */
    tp_Gzip *gz;
    Error rl_err;  /* in rl_arena */
    Error set_err; /* in wl_arena, under mu */
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
    Time res_idle_at;
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
    tp_IdleBucket *b = (tp_IdleBucket *)mem_alloc(a, sizeof *b, _Alignof(tp_IdleBucket));
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
        tp_PConn **nc = (tp_PConn **)mem_alloc(a, (size_t)ncap * sizeof *nc,
                                               _Alignof(tp_PConn *));
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
    tp_HostBucket *b = (tp_HostBucket *)mem_alloc(a, sizeof *b, _Alignof(tp_HostBucket));
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

/* @@TP-PART-3@@ */
