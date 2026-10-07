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
static NetConn tp_dial(HttpTransport *t, Context ctx, Str network, Str addr, Error *err) {
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
        IoLimitedReader *lr = (IoLimitedReader *)mem_alloc(c->a, sizeof *lr,
                                                           _Alignof(IoLimitedReader));
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
    const HttpProtocols *p = t->protocols;
    if (p != NULL && http_protocols_unencrypted_http2(*p) && !http_protocols_http1(*p)) {
        /* Unencrypted HTTP/2 with prior knowledge, which needs HTTP/2. */
        BURROW_OUT(err, tp_err_unencrypted_h2);
        return NULL;
    }
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
        pc->closech == NULL || pc->write_err_ch == NULL || pc->write_loop_done == NULL ||
        pc->eofc == NULL) {
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

/* @@TP-PART-5@@ */
