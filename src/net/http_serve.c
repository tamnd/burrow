/* Derived from Go's src/net/http/server.go, the HTTP/1 server, with
 * responsecontroller.go and MaxBytesReader from request.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "../xnet/httpguts.h"
#include "http_internal.h"
#include "internal.h"

#include "burrow/bufio.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/panic.h"
#include "burrow/path.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/strconv.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ errors */

BURROW_SENTINEL_ERROR(http_err_server_closed, "http: Server closed");
BURROW_SENTINEL_ERROR(http_err_hijacked, "http: connection has been hijacked");
BURROW_SENTINEL_ERROR(http_err_content_length,
                      "http: wrote more than the declared Content-Length");
BURROW_SENTINEL_ERROR(http_err_abort_handler, "net/http: abort Handler");
BURROW_SENTINEL_ERROR(http_err_not_supported, "feature not supported");
BURROW_SENTINEL_ERROR(http_err_handler_timeout, "http: Handler timeout");

/* errNotSupported, which wraps ErrNotSupported. */
static Error sv_err_not_supported(void) {
    return fmt_errorf_v("%w", http_err_not_supported);
}

/* ------------------------------------------------------------------- types */

static const Type sv_server_desc = {
    {(const Byte *)"Server", 6},
    {(const Byte *)"net/http", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(HttpServer),
    (uint16_t)_Alignof(HttpServer),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68737276U, /* "hsrv" */
    NULL,
};

const Type *const TYPE_HTTP_SERVER = &sv_server_desc;

static const Type sv_max_bytes_error_desc = {
    {(const Byte *)"MaxBytesError", 13},
    {(const Byte *)"net/http", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(HttpMaxBytesError),
    (uint16_t)_Alignof(HttpMaxBytesError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x686d6265U, /* "hmbe" */
    NULL,
};

const Type *const TYPE_HTTP_MAX_BYTES_ERROR = &sv_max_bytes_error_desc;

/* contextKey, the type of the two context keys, which Go tells apart by
 * pointer and this file by value. */
static const Type sv_context_key_desc = {
    {(const Byte *)"contextKey", 10},
    {(const Byte *)"net/http", 8},
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
    0x68636b79U, /* "hcky" */
    NULL,
};

static const Int sv_key_server = 0;
static const Int sv_key_local_addr = 1;

const Any http_server_context_key = {&sv_context_key_desc,
                                     (void *)(uintptr_t)&sv_key_server};
const Any http_local_addr_context_key = {&sv_context_key_desc,
                                         (void *)(uintptr_t)&sv_key_local_addr};

static const Type sv_response_desc = {
    {(const Byte *)"response", 8},
    {(const Byte *)"net/http", 8},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68727370U, /* "hrsp" */
    NULL,
};

/* ------------------------------------------------------------ ConnState */

Str http_conn_state_string(HttpConnState s) {
    switch (s) {
    case HTTP_STATE_NEW:
        return BURROW_S("new");
    case HTTP_STATE_ACTIVE:
        return BURROW_S("active");
    case HTTP_STATE_IDLE:
        return BURROW_S("idle");
    case HTTP_STATE_HIJACKED:
        return BURROW_S("hijacked");
    case HTTP_STATE_CLOSED:
        return BURROW_S("closed");
    default:
        return BURROW_S("");
    }
}

/* ----------------------------------------------------------- the structs */

/* bufferBeforeChunkingSize, the size of the writer in front of the chunk
 * writer, and the size of the connection's writer. */
#define SV_BUFFER_BEFORE_CHUNKING_SIZE 2048
#define SV_CONN_WRITER_SIZE (4 << 10)

/* rstAvoidanceDelay. */
#define SV_RST_AVOIDANCE_DELAY (500 * TIME_MILLISECOND)

/* shutdownPollIntervalMax. */
#define SV_SHUTDOWN_POLL_INTERVAL_MAX (500 * TIME_MILLISECOND)

typedef struct burrow__HttpServeConn sv_Conn;
typedef struct sv_Response sv_Response;

/* The context the connections of one Serve start from, which lives until
 * Serve and the last of its connections are done with it. */
typedef struct sv_ServeCtx {
    Context ctx; /* with ServerContextKey, over the base */
    Alloc *a;
    SyncAtomicInt64 refs;
} sv_ServeCtx;

/* connReader, the reader under the connection's bufio.Reader. It reads a byte
 * in the background while a handler runs, to see the client go away. */
typedef struct sv_ConnReader {
    NetConn rwc;
    SyncMutex mu; /* guards the rest */
    SyncCond cond;
    sv_Conn *conn; /* NULL once the handler has hijacked the connection */
    int64_t remain;
    Byte byte_buf[1];
    bool has_byte;
    bool in_read;
    bool aborted; /* set before the read deadline goes into the past */
} sv_ConnReader;

/* conn, the server's side of one connection. */
struct burrow__HttpServeConn {
    HttpServer *server;
    sv_ServeCtx *sctx;
    Context conn_ctx; /* what Serve and ConnContext gave */
    Context ctx;      /* conn_ctx with LocalAddrContextKey */
    Context cctx;     /* ctx with cancel_ctx */
    ContextCancelFunc cancel_ctx;
    NetConn rwc;
    Str remote_addr;
    Error werr; /* the first write error */
    sv_ConnReader r;
    BufioReader *bufr;
    BufioWriter *bufw;
    bool last_was_post;
    SyncAtomicPointer cur_req;  /* sv_Response */
    SyncAtomicUint64 cur_state; /* unix seconds << 8 | ConnState */
    SyncMutex mu;               /* guards hijackedv */
    bool hijackedv;
    bool tracked;
    Arena arena; /* for what lives as long as the connection */
    sv_Conn *prev;
    sv_Conn *next;
};

/* onceCloseListener, and the server's record of it. */
struct burrow__HttpServeListener {
    NetListener l;
    SyncMutex mu;
    bool closed;
    Error close_err;
    struct burrow__HttpServeListener *prev;
    struct burrow__HttpServeListener *next;
};

typedef struct burrow__HttpServeListener sv_Listener;

/* chunkWriter, between the response's bufio.Writer and the connection's. It
 * writes the header the first time it is written to, and then the body, in
 * chunks when the response is chunked. */
typedef struct sv_ChunkWriter {
    sv_Response *res;
    HttpHeader header; /* a copy of the handler's, or NULL */
    bool wrote_header;
    bool chunking;
} sv_ChunkWriter;

/* expectContinueReader, the body of a request that said
 * "Expect: 100-continue", which sends the 100 at the first read. */
typedef struct sv_ExpectContinueReader {
    sv_Response *resp;
    IoReadCloser read_closer;
    SyncAtomicBool closed;
} sv_ExpectContinueReader;

struct sv_Response {
    sv_Conn *conn;
    HttpRequest *req;
    void *req_body; /* the body read from the wire, NULL for none */
    ContextCancelFunc cancel_ctx;
    Context ctx;
    sv_ExpectContinueReader *ec_reader;
    SyncMutex write_continue_mu;
    SyncAtomicBool can_write_continue;
    BufioWriter *w; /* in front of cw */
    sv_ChunkWriter cw;
    HttpHeader handler_header;
    int64_t written;
    int64_t content_length; /* -1 when the handler did not set one */
    Int status;
    Slice trailers; /* of Str */
    SyncAtomicBool handler_done;
    SyncMutex lazy_close_notify_mu;
    Chan *close_notify_ch;
    Arena arena; /* for what lives as long as the response */
    bool wrote_header;
    bool wants10_keep_alive;
    bool wants_close;
    bool called_header;
    bool close_after_reply;
    bool full_duplex;
    bool request_body_limit_hit;
    bool close_notify_triggered;
};

/* ------------------------------------------------------------- helpers */

static Alloc *sv_alloc(const HttpServer *s) {
    return s->a != NULL ? s->a : heap_allocator();
}

static Str sv_hget(HttpHeader h, Str key) {
    return h == NULL ? BURROW_S("") : burrow__http_header_get(h, key);
}

static bool sv_hhas(HttpHeader h, Str key) {
    return h != NULL && burrow__http_header_has(h, key);
}

static IoWriter sv_bufw(BufioWriter *b) {
    return bufio_writer_as_io_writer(b);
}

static void sv_write_str(BufioWriter *b, Str s) {
    Error err;
    (void)bufio_writer_write_string(b, s, &err);
}

static Error sv_conn_close_rwc(NetConn c) {
    return c.vt->closer.close(c.data);
}

static Error sv_set_read_deadline(NetConn c, Time t) {
    return c.vt->set_read_deadline(c.data, t);
}

static Error sv_set_write_deadline(NetConn c, Time t) {
    return c.vt->set_write_deadline(c.data, t);
}

static Time sv_zero_time(void) {
    Time t = {0};
    return t;
}

/* s[lo:hi]. */
static Str sv_sub(Str s, Int lo, Int hi) {
    return str_from_bytes(s.p + lo, hi - lo);
}

/* textproto.TrimString. */
static Str sv_trim(Str s) {
    while (s.len > 0 &&
           (s.p[0] == ' ' || s.p[0] == '\t' || s.p[0] == '\r' || s.p[0] == '\n')) {
        s.p++;
        s.len--;
    }
    while (s.len > 0 && (s.p[s.len - 1] == ' ' || s.p[s.len - 1] == '\t' ||
                         s.p[s.len - 1] == '\r' || s.p[s.len - 1] == '\n'))
        s.len--;
    return s;
}

/* The server's logf. */
#define sv_logf(s, ...) log_logger_printf_v((s)->error_log, __VA_ARGS__)

/* relevantCaller. The first frame outside this package, to name in a log
 * line about a handler that used its writer wrongly. */
static RuntimeFrame sv_relevant_caller(void) {
    Uintptr pcs[16];
    Int n = runtime_callers(1, slice_from(pcs, 16, 16, TYPE_UINTPTR));
    RuntimeFrames it = runtime_callers_frames(slice_from(pcs, n, n, TYPE_UINTPTR));
    RuntimeFrame frame = {0};
    while (runtime_frames_next(&it, &frame)) {
        Str f = frame.function;
        if (!strings_has_prefix(f, BURROW_S("sv_")) &&
            !strings_has_prefix(f, BURROW_S("http_")) &&
            !strings_has_prefix(f, BURROW_S("burrow__http_")))
            return frame;
    }
    return frame;
}

static void sv_log_misuse(HttpServer *s, Str what) {
    RuntimeFrame f = sv_relevant_caller();
    sv_logf(s, "http: %s from %s (%s:%d)", what, f.function, path_base(f.file), f.line);
}

/* checkWriteHeaderCode. */
static void sv_check_write_header_code(Int code) {
    if (code < 100 || code > 999)
        panic_str(fmt_sprintf_v(heap_allocator(), "invalid WriteHeader code %v", code));
}

/* ------------------------------------------------------------ the server */

static bool sv_shutting_down(HttpServer *s) {
    return sync_atomic_bool_load(&s->in_shutdown);
}

static bool sv_do_keep_alives(HttpServer *s) {
    return !sync_atomic_bool_load(&s->disable_keep_alives) && !sv_shutting_down(s);
}

static Int sv_max_header_bytes(const HttpServer *s) {
    return s->max_header_bytes > 0 ? s->max_header_bytes
                                   : HTTP_DEFAULT_MAX_HEADER_BYTES;
}

static Int sv_max_header_value_count(const HttpServer *s) {
    return s->max_header_value_count > 0 ? s->max_header_value_count
                                         : HTTP_DEFAULT_MAX_HEADER_VALUE_COUNT;
}

/* initialReadLimitSize, with bufio's slop. */
static int64_t sv_initial_read_limit_size(const HttpServer *s) {
    return (int64_t)sv_max_header_bytes(s) + 4096;
}

static Duration sv_idle_timeout(const HttpServer *s) {
    return s->idle_timeout != 0 ? s->idle_timeout : s->read_timeout;
}

static Duration sv_read_header_timeout(const HttpServer *s) {
    return s->read_header_timeout != 0 ? s->read_header_timeout : s->read_timeout;
}

static HttpProtocols sv_protocols(const HttpServer *s) {
    if (s->protocols != NULL && !burrow__http_protocols_empty(*s->protocols))
        return *s->protocols;
    HttpProtocols p = {0};
    http_protocols_set_http1(&p, true);
    return p;
}

/* trackConn. */
static void sv_track_conn(sv_Conn *c, bool add) {
    HttpServer *s = c->server;
    sync_mutex_lock(&s->mu);
    if (add && !c->tracked) {
        c->prev = NULL;
        c->next = s->active_conn;
        if (s->active_conn != NULL)
            s->active_conn->prev = c;
        s->active_conn = c;
        c->tracked = true;
    } else if (!add && c->tracked) {
        if (c->prev != NULL)
            c->prev->next = c->next;
        else
            s->active_conn = c->next;
        if (c->next != NULL)
            c->next->prev = c->prev;
        c->prev = c->next = NULL;
        c->tracked = false;
    }
    sync_mutex_unlock(&s->mu);
}

/* conn.setState. */
static void sv_set_state(sv_Conn *c, HttpConnState state, bool run_hook) {
    HttpServer *s = c->server;
    switch (state) {
    case HTTP_STATE_NEW:
        sv_track_conn(c, true);
        break;
    case HTTP_STATE_HIJACKED:
    case HTTP_STATE_CLOSED:
        sv_track_conn(c, false);
        break;
    case HTTP_STATE_ACTIVE:
    case HTTP_STATE_IDLE:
    default:
        break;
    }
    uint64_t packed = ((uint64_t)time_unix(time_now()) << 8) | (uint64_t)state;
    sync_atomic_uint64_store(&c->cur_state, packed);
    if (!run_hook)
        return;
    if (!BURROW_FUNC_IS_NIL(s->conn_state))
        BURROW_CALLF(s->conn_state, c->rwc, state);
}

/* conn.getState. */
static HttpConnState sv_get_state(sv_Conn *c, int64_t *unix_sec) {
    uint64_t packed = sync_atomic_uint64_load(&c->cur_state);
    *unix_sec = (int64_t)(packed >> 8);
    return (HttpConnState)(packed & 0xff);
}

static bool sv_hijacked(sv_Conn *c) {
    sync_mutex_lock(&c->mu);
    bool v = c->hijackedv;
    sync_mutex_unlock(&c->mu);
    return v;
}

/* ------------------------------------------------------------ connReader */

static void sv_cr_close_notify(sv_Response *w);

/* handleReadErrorLocked. A read failed, so the client has gone, or will not
 * be heard from, and the request's context is cancelled. */
static void sv_cr_handle_read_error_locked(sv_ConnReader *cr) {
    if (cr->conn == NULL)
        return;
    BURROW_CALLF0(cr->conn->cancel_ctx);
    sv_Response *res = (sv_Response *)sync_atomic_pointer_load(&cr->conn->cur_req);
    if (res != NULL)
        sv_cr_close_notify(res);
}

static void sv_cr_background_read(void *env) {
    sv_ConnReader *cr = (sv_ConnReader *)env;
    Error err = BURROW_NO_ERROR;
    Int n = cr->rwc.vt->reader.read(cr->rwc.data,
                                    slice_from(cr->byte_buf, 1, 1, TYPE_BYTE), &err);
    sync_mutex_lock(&cr->mu);
    if (n == 1)
        cr->has_byte = true;
    if (cr->aborted && BURROW_FAILED(err) && net_error_timeout(err)) {
        /* The read was stopped on purpose, by abortPendingRead. */
    } else if (BURROW_FAILED(err)) {
        sv_cr_handle_read_error_locked(cr);
    }
    cr->aborted = false;
    cr->in_read = false;
    /* Before the unlock, unlike Go, so that a waiter that goes on to free the
     * reader cannot do it before this is done with it. */
    sync_cond_broadcast(&cr->cond);
    sync_mutex_unlock(&cr->mu);
}

/* startBackgroundRead. */
static void sv_cr_start_background_read(void *env) {
    sv_ConnReader *cr = (sv_ConnReader *)env;
    sync_mutex_lock(&cr->mu);
    if (cr->in_read) {
        sync_mutex_unlock(&cr->mu);
        panic_str(BURROW_S("invalid concurrent Body.Read call"));
    }
    if (cr->has_byte) {
        sync_mutex_unlock(&cr->mu);
        return;
    }
    cr->in_read = true;
    (void)sv_set_read_deadline(cr->rwc, sv_zero_time());
    if (!go(BURROW_FN(Func, sv_cr_background_read, cr)))
        cr->in_read = false;
    sync_mutex_unlock(&cr->mu);
}

/* abortPendingRead. Stops the background read, if there is one, and waits for
 * it to finish. */
static void sv_cr_abort_pending_read(sv_ConnReader *cr) {
    sync_mutex_lock(&cr->mu);
    if (!cr->in_read) {
        sync_mutex_unlock(&cr->mu);
        return;
    }
    cr->aborted = true;
    /* aLongTimeAgo, a time in the past that is not the zero Time. */
    (void)sv_set_read_deadline(cr->rwc, time_from_unix(1, 0));
    while (cr->in_read)
        sync_cond_wait(&cr->cond);
    (void)sv_set_read_deadline(cr->rwc, sv_zero_time());
    sync_mutex_unlock(&cr->mu);
}

static void sv_cr_release_conn(sv_ConnReader *cr) {
    sync_mutex_lock(&cr->mu);
    cr->conn = NULL;
    sync_mutex_unlock(&cr->mu);
}

static Int sv_cr_read(void *self, Slice p, Error *err) {
    sv_ConnReader *cr = (sv_ConnReader *)self;
    *err = BURROW_NO_ERROR;
    sync_mutex_lock(&cr->mu);
    if (cr->conn == NULL) {
        sync_mutex_unlock(&cr->mu);
        return cr->rwc.vt->reader.read(cr->rwc.data, p, err);
    }
    if (cr->in_read) {
        bool hijacked = sv_hijacked(cr->conn);
        sync_mutex_unlock(&cr->mu);
        if (hijacked)
            panic_str(BURROW_S("invalid Body.Read call. After hijacked, the original "
                               "Request must not be used"));
        panic_str(BURROW_S("invalid concurrent Body.Read call"));
    }
    if (cr->remain <= 0) {
        sync_mutex_unlock(&cr->mu);
        *err = io_eof;
        return 0;
    }
    if (p.len == 0) {
        sync_mutex_unlock(&cr->mu);
        return 0;
    }
    if ((int64_t)p.len > cr->remain)
        p.len = (Int)cr->remain;
    if (cr->has_byte) {
        ((Byte *)p.p)[0] = cr->byte_buf[0];
        cr->has_byte = false;
        sync_mutex_unlock(&cr->mu);
        return 1;
    }
    cr->in_read = true;
    sync_mutex_unlock(&cr->mu);
    Int n = cr->rwc.vt->reader.read(cr->rwc.data, p, err);

    sync_mutex_lock(&cr->mu);
    cr->in_read = false;
    if (BURROW_FAILED(*err))
        sv_cr_handle_read_error_locked(cr);
    cr->remain -= (int64_t)n;
    sync_cond_broadcast(&cr->cond);
    sync_mutex_unlock(&cr->mu);
    return n;
}

static const IoReaderVT sv_cr_vt = {NULL, sv_cr_read};

/* ------------------------------------------------------ checkConnErrorWriter */

static Int sv_check_conn_error_write(void *self, Slice p, Error *err) {
    sv_Conn *c = (sv_Conn *)self;
    Int n = c->rwc.vt->writer.write(c->rwc.data, p, err);
    if (BURROW_FAILED(*err) && BURROW_OK(c->werr)) {
        c->werr = *err;
        BURROW_CALLF0(c->cancel_ctx);
    }
    return n;
}

static const IoWriterVT sv_check_conn_error_vt = {NULL, sv_check_conn_error_write};

/* -------------------------------------------------------- the response */

static const HttpResponseWriterVT sv_response_vt;

static HttpResponseWriter sv_as_writer(sv_Response *w) {
    return (HttpResponseWriter){&sv_response_vt, w};
}

static Alloc *sv_wa(sv_Response *w) {
    return arena_allocator(&w->arena);
}

/* requestTooLarge, which a MaxBytesReader calls when its limit is hit. */
static void sv_request_too_large(sv_Response *w) {
    w->close_after_reply = true;
    w->request_body_limit_hit = true;
    if (!w->wrote_header)
        (void)http_header_set(sv_response_vt.header(w), BURROW_S("Connection"),
                              BURROW_S("close"));
}

/* disableWriteContinue. */
static void sv_disable_write_continue(sv_Response *w, bool skip_drain) {
    if (w->ec_reader == NULL)
        return;
    sync_mutex_lock(&w->write_continue_mu);
    if (sync_atomic_bool_load(&w->can_write_continue)) {
        sync_atomic_bool_store(&w->can_write_continue, false);
        if (skip_drain) {
            /* The body will not be read, so the connection cannot be used
             * again. */
            w->close_after_reply = true;
            sync_atomic_bool_store(&w->ec_reader->closed, true);
        }
    }
    sync_mutex_unlock(&w->write_continue_mu);
}

/* finalTrailers. The trailers to send after a chunked body, from a, or NULL
 * when there are none. */
static HttpHeader sv_final_trailers(sv_Response *w, Alloc *a) {
    HttpHeader t = NULL;
    MapIter it = map_iter(w->handler_header);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        bool found;
        Str kk = strings_cut_prefix(*(const Str *)k, HTTP_TRAILER_PREFIX, &found);
        if (!found)
            continue;
        if (t == NULL) {
            t = http_header_make(a);
            if (t == NULL)
                return NULL;
        }
        (void)map_set(t, &kk, v);
    }
    const Str *keys = (const Str *)w->trailers.p;
    for (Int i = 0; i < w->trailers.len; i++) {
        if (t == NULL) {
            t = http_header_make(a);
            if (t == NULL)
                return NULL;
        }
        Slice vv = http_header_values(w->handler_header, keys[i]);
        const Str *vs = (const Str *)vv.p;
        for (Int j = 0; j < vv.len; j++)
            (void)http_header_add(t, keys[i], vs[j]);
    }
    return t;
}

/* declareTrailer. */
static void sv_declare_trailer(sv_Response *w, Str k) {
    k = http_canonical_header_key(sv_wa(w), k);
    if (!burrow__httpguts_valid_trailer_header(k))
        return;
    w->trailers = slice_append(sv_wa(w), w->trailers, &k, 1);
}

/* foreachHeaderElement with declareTrailer. */
static void sv_declare_trailers(sv_Response *w, Str v) {
    v = sv_trim(v);
    while (v.len > 0) {
        Int i = strings_index_byte(v, ',');
        Str f = i < 0 ? v : sv_sub(v, 0, i);
        f = sv_trim(f);
        if (f.len > 0)
            sv_declare_trailer(w, f);
        if (i < 0)
            break;
        v = sv_sub(v, i + 1, v.len);
    }
}

/* writeStatusLine. */
static void sv_write_status_line(BufioWriter *bw, bool is11, Int code) {
    sv_write_str(bw, is11 ? BURROW_S("HTTP/1.1 ") : BURROW_S("HTTP/1.0 "));
    Str text = http_status_text(code);
    if (text.len > 0) {
        Byte buf[24];
        Slice b = strconv_append_int(
            NULL, slice_from(buf, 0, (Int)sizeof buf, TYPE_BYTE), (int64_t)code, 10);
        Error err;
        (void)bufio_writer_write(bw, b, &err);
        (void)bufio_writer_write_byte(bw, ' ');
        sv_write_str(bw, text);
        sv_write_str(bw, BURROW_S("\r\n"));
    } else {
        (void)fmt_fprintf_v(sv_bufw(bw), "%03d status code %d\r\n", code, code);
    }
}

/* suppressedHeaders. The fields a response with this status may not have. */
static Int sv_suppressed_headers(Int status, const Str **out) {
    static const Str suppressed304[] = {BURROW_S_INIT("Content-Type"),
                                        BURROW_S_INIT("Content-Length"),
                                        BURROW_S_INIT("Transfer-Encoding")};
    static const Str suppressed_no_body[] = {BURROW_S_INIT("Content-Length"),
                                             BURROW_S_INIT("Transfer-Encoding")};
    if (status == 304) {
        *out = suppressed304;
        return 3;
    }
    if (!burrow__http_body_allowed_for_status(status)) {
        *out = suppressed_no_body;
        return 2;
    }
    *out = NULL;
    return 0;
}

static const Str sv_excluded_headers_no_body[] = {BURROW_S_INIT("Content-Length"),
                                                  BURROW_S_INIT("Transfer-Encoding")};

/* isProtocolSwitchResponse. */
static bool sv_is_protocol_switch_response(Int code, HttpHeader h) {
    return code == HTTP_STATUS_SWITCHING_PROTOCOLS && sv_hhas(h, BURROW_S("Upgrade")) &&
           burrow__http_has_token(sv_hget(h, BURROW_S("Connection")),
                                  BURROW_S("Upgrade"));
}

/* What chunkWriter.writeHeader adds to the header, extraHeader. */
typedef struct sv_ExtraHeader {
    Str content_type;
    Str connection;
    Str transfer_encoding;
    Slice date;           /* written when p is not NULL */
    Slice content_length; /* written when p is not NULL */
} sv_ExtraHeader;

static void sv_extra_header_write(const sv_ExtraHeader *h, BufioWriter *w) {
    Error err;
    if (h->date.p != NULL) {
        sv_write_str(w, BURROW_S("Date: "));
        (void)bufio_writer_write(w, h->date, &err);
        sv_write_str(w, BURROW_S("\r\n"));
    }
    if (h->content_length.p != NULL) {
        sv_write_str(w, BURROW_S("Content-Length: "));
        (void)bufio_writer_write(w, h->content_length, &err);
        sv_write_str(w, BURROW_S("\r\n"));
    }
    const Str keys[] = {BURROW_S_INIT("Content-Type"), BURROW_S_INIT("Connection"),
                        BURROW_S_INIT("Transfer-Encoding")};
    const Str vals[] = {h->content_type, h->connection, h->transfer_encoding};
    for (Int i = 0; i < 3; i++) {
        if (vals[i].len > 0) {
            sv_write_str(w, keys[i]);
            sv_write_str(w, BURROW_S(": "));
            sv_write_str(w, vals[i]);
            sv_write_str(w, BURROW_S("\r\n"));
        }
    }
}

/* The keys chunkWriter.writeHeader leaves out of a header it does not own. */
typedef struct sv_Exclude {
    Str *keys;
    Int n;
    Int cap;
} sv_Exclude;

static void sv_exclude_add(sv_Exclude *e, Alloc *a, Str key) {
    for (Int i = 0; i < e->n; i++) {
        if (str_eq(e->keys[i], key))
            return;
    }
    if (e->n == e->cap) {
        Int cap = e->cap == 0 ? 8 : e->cap * 2;
        Str *keys = (Str *)mem_alloc(a, (size_t)cap * sizeof(Str), _Alignof(Str));
        if (keys == NULL)
            return;
        if (e->n > 0)
            memcpy(keys, e->keys, (size_t)e->n * sizeof(Str));
        e->keys = keys;
        e->cap = cap;
    }
    e->keys[e->n++] = key;
}

/* delHeader in chunkWriter.writeHeader. */
static void sv_del_header(HttpHeader header, bool owned, sv_Exclude *e, Alloc *a,
                          Str key) {
    if (owned) {
        http_header_del(header, key);
        return;
    }
    if (map_get(header, &key) == NULL)
        return;
    sv_exclude_add(e, a, key);
}

/* chunkWriter.writeHeader. Works out and writes the status line and the
 * header, the first time the response writes, with p the first data, which
 * may be all of it, for the Content-Length and the Content-Type. */
static void sv_cw_write_header(sv_ChunkWriter *cw, Slice p) {
    if (cw->wrote_header)
        return;
    cw->wrote_header = true;

    sv_Response *w = cw->res;
    Alloc *a = sv_wa(w);
    HttpServer *s = w->conn->server;
    bool keep_alives_enabled = sv_do_keep_alives(s);
    bool is_head = str_eq(w->req->method, BURROW_S("HEAD"));

    /* header is written to only when it is a copy, and the keys to leave out
     * of the handler's own go into exclude instead. */
    HttpHeader header = cw->header;
    bool owned = header != NULL;
    if (!owned)
        header = w->handler_header;
    sv_Exclude exclude = {NULL, 0, 0};
    sv_ExtraHeader set_header;
    memset(&set_header, 0, sizeof set_header);

    /* A trailer can be declared before the header is written, or after it,
     * with the "Trailer:" prefix. */
    bool trailers = false;
    if (cw->header != NULL) {
        MapIter it = map_iter(cw->header);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v)) {
            if (strings_has_prefix(*(const Str *)k, HTTP_TRAILER_PREFIX)) {
                sv_exclude_add(&exclude, a, *(const Str *)k);
                trailers = true;
            }
        }
        Slice tv = http_header_values(cw->header, BURROW_S("Trailer"));
        const Str *ts = (const Str *)tv.p;
        for (Int i = 0; i < tv.len; i++) {
            trailers = true;
            sv_declare_trailers(w, ts[i]);
        }
    }

    Str te = sv_hget(header, BURROW_S("Transfer-Encoding"));
    bool has_te = te.len > 0;

    /* The handler is done, so the whole body is p, and its length can be
     * sent, so long as the handler did not say otherwise. */
    if (sync_atomic_bool_load(&w->handler_done) && !trailers && !has_te &&
        burrow__http_body_allowed_for_status(w->status) &&
        !sv_hhas(header, BURROW_S("Content-Length")) && (!is_head || p.len > 0)) {
        w->content_length = (int64_t)p.len;
        Byte *buf = (Byte *)mem_alloc(a, 24, 1);
        if (buf != NULL)
            set_header.content_length = strconv_append_int(
                NULL, slice_from(buf, 0, 24, TYPE_BYTE), (int64_t)p.len, 10);
    }

    /* An HTTP/1.0 client that asked for keep-alive gets it when the handler
     * set a Content-Length and "Connection: keep-alive". */
    if (w->wants10_keep_alive && keep_alives_enabled) {
        bool sent_length = sv_hget(header, BURROW_S("Content-Length")).len > 0;
        if (sent_length &&
            str_eq(sv_hget(header, BURROW_S("Connection")), BURROW_S("keep-alive")))
            w->close_after_reply = false;
    }

    bool has_cl = w->content_length != -1;

    if (w->wants10_keep_alive &&
        (is_head || has_cl || !burrow__http_body_allowed_for_status(w->status))) {
        if (!sv_hhas(header, BURROW_S("Connection")))
            set_header.connection = BURROW_S("keep-alive");
    } else if (!http_request_proto_at_least(w->req, 1, 1) || w->wants_close) {
        w->close_after_reply = true;
    }

    if (str_eq(sv_hget(header, BURROW_S("Connection")), BURROW_S("close")) ||
        !keep_alives_enabled)
        w->close_after_reply = true;

    /* A client that sent "Expect: 100-continue" and was not asked for the body
     * may send it anyway, and the server cannot tell it from the next
     * request. */
    if (w->ec_reader != NULL && burrow__http_body_remains(w->req_body))
        w->close_after_reply = true;

    /* Read what is left of a small body so the connection can be used again,
     * or close it when there is too much of it. */
    if (w->req->content_length != 0 && w->req_body != NULL && !w->close_after_reply &&
        !w->full_duplex) {
        bool discard = false;
        bool too_big = false;
        bool closed;
        bool saw_eof;
        int64_t unread;
        burrow__http_body_state(w->req_body, &closed, &saw_eof, &unread);
        if (closed) {
            if (!saw_eof)
                w->close_after_reply = true;
        } else if (unread >= BURROW__HTTP_MAX_POST_HANDLER_READ_BYTES) {
            too_big = true;
        } else {
            discard = true;
        }
        if (discard) {
            (void)burrow__http_body_close(w->req_body);
            if (burrow__http_body_did_early_close(w->req_body))
                w->close_after_reply = true;
        }
        if (too_big) {
            sv_request_too_large(w);
            sv_del_header(header, owned, &exclude, a, BURROW_S("Connection"));
            set_header.connection = BURROW_S("close");
        }
    }

    Int code = w->status;
    if (burrow__http_body_allowed_for_status(code)) {
        /* Sniff the type when the handler did not give one. */
        bool have_type = sv_hhas(header, BURROW_S("Content-Type"));
        bool has_ce = http_header_get(header, BURROW_S("Content-Encoding")).len > 0;
        if (!has_ce && !have_type && !has_te && p.len > 0)
            set_header.content_type = http_detect_content_type(p);
    } else {
        const Str *keys;
        Int n = sv_suppressed_headers(code, &keys);
        for (Int i = 0; i < n; i++)
            sv_del_header(header, owned, &exclude, a, keys[i]);
    }

    if (!sv_hhas(header, BURROW_S("Date"))) {
        Byte *buf = (Byte *)mem_alloc(a, 32, 1);
        if (buf != NULL)
            set_header.date =
                time_append_format(time_utc(time_now()), NULL,
                                   slice_from(buf, 0, 32, TYPE_BYTE), HTTP_TIME_FORMAT);
    }

    if (has_cl && has_te && !str_eq(te, BURROW_S("identity"))) {
        sv_logf(s,
                "http: WriteHeader called with both Transfer-Encoding of %q and a "
                "Content-Length of %d",
                te, w->content_length);
        sv_del_header(header, owned, &exclude, a, BURROW_S("Content-Length"));
        has_cl = false;
    }

    if (is_head || !burrow__http_body_allowed_for_status(code) ||
        code == HTTP_STATUS_NO_CONTENT || has_cl) {
        /* No body, so no framing for one, or a length that frames it. */
        sv_del_header(header, owned, &exclude, a, BURROW_S("Transfer-Encoding"));
    } else if (http_request_proto_at_least(w->req, 1, 1)) {
        if (has_te && str_eq(te, BURROW_S("identity"))) {
            /* The body ends when the connection does. */
            cw->chunking = false;
            w->close_after_reply = true;
            sv_del_header(header, owned, &exclude, a, BURROW_S("Transfer-Encoding"));
        } else {
            cw->chunking = true;
            set_header.transfer_encoding = BURROW_S("chunked");
            if (has_te && str_eq(te, BURROW_S("chunked")))
                sv_del_header(header, owned, &exclude, a,
                              BURROW_S("Transfer-Encoding"));
        }
    } else {
        /* An HTTP/1.0 client with no length to go by reads to the end of the
         * connection. */
        w->close_after_reply = true;
        sv_del_header(header, owned, &exclude, a, BURROW_S("Transfer-Encoding"));
    }

    if (cw->chunking)
        sv_del_header(header, owned, &exclude, a, BURROW_S("Content-Length"));
    if (!http_request_proto_at_least(w->req, 1, 0))
        return;

    bool del_connection_header =
        w->close_after_reply &&
        (!keep_alives_enabled ||
         !burrow__http_has_token(sv_hget(cw->header, BURROW_S("Connection")),
                                 BURROW_S("close"))) &&
        !sv_is_protocol_switch_response(w->status, header);
    if (del_connection_header) {
        sv_del_header(header, owned, &exclude, a, BURROW_S("Connection"));
        if (http_request_proto_at_least(w->req, 1, 1))
            set_header.connection = BURROW_S("close");
    }

    BufioWriter *bw = w->conn->bufw;
    sv_write_status_line(bw, http_request_proto_at_least(w->req, 1, 1), code);
    if (cw->header != NULL)
        (void)burrow__http_header_write_except(cw->header, sv_bufw(bw), exclude.keys,
                                               exclude.n, NULL);
    sv_extra_header_write(&set_header, bw);
    sv_write_str(bw, BURROW_S("\r\n"));
}

/* chunkWriter.Write. */
static Int sv_cw_write(void *self, Slice p, Error *err) {
    sv_ChunkWriter *cw = (sv_ChunkWriter *)self;
    *err = BURROW_NO_ERROR;
    if (!cw->wrote_header)
        sv_cw_write_header(cw, p);
    sv_Response *res = cw->res;
    if (str_eq(res->req->method, BURROW_S("HEAD")))
        return p.len; /* Eat the body. */
    BufioWriter *bw = res->conn->bufw;
    if (cw->chunking) {
        (void)fmt_fprintf_v(sv_bufw(bw), "%x\r\n", p.len);
        if (BURROW_FAILED(bw->err)) {
            *err = bw->err;
            (void)sv_conn_close_rwc(res->conn->rwc);
            return 0;
        }
    }
    Int n = bufio_writer_write(bw, p, err);
    if (cw->chunking && BURROW_OK(*err))
        (void)bufio_writer_write_string(bw, BURROW_S("\r\n"), err);
    if (BURROW_FAILED(*err))
        (void)sv_conn_close_rwc(res->conn->rwc);
    return n;
}

static const IoWriterVT sv_cw_vt = {NULL, sv_cw_write};

/* chunkWriter.flush. */
static Error sv_cw_flush(sv_ChunkWriter *cw) {
    if (!cw->wrote_header)
        sv_cw_write_header(cw, slice_from(NULL, 0, 0, TYPE_BYTE));
    return bufio_writer_flush(cw->res->conn->bufw);
}

/* chunkWriter.close. */
static void sv_cw_close(sv_ChunkWriter *cw) {
    if (!cw->wrote_header)
        sv_cw_write_header(cw, slice_from(NULL, 0, 0, TYPE_BYTE));
    if (cw->chunking) {
        BufioWriter *bw = cw->res->conn->bufw;
        sv_write_str(bw, BURROW_S("0\r\n"));
        HttpHeader trailers = sv_final_trailers(cw->res, sv_wa(cw->res));
        if (trailers != NULL)
            (void)http_header_write(trailers, sv_bufw(bw)); /* bw keeps the error */
        sv_write_str(bw, BURROW_S("\r\n"));
    }
}

/* response.Header. */
static HttpHeader sv_response_header(void *self) {
    sv_Response *w = (sv_Response *)self;
    if (w->cw.header == NULL && w->wrote_header && !w->cw.wrote_header) {
        /* The handler is changing the header after WriteHeader and before
         * the first write, which Go allows for no good reason, so the header
         * sent is the one from WriteHeader. */
        w->cw.header = http_header_clone(sv_wa(w), w->handler_header);
    }
    w->called_header = true;
    return w->handler_header;
}

/* response.WriteHeader. */
static void sv_response_write_header(void *self, Int code) {
    sv_Response *w = (sv_Response *)self;
    HttpServer *s = w->conn->server;
    if (sv_hijacked(w->conn)) {
        sv_log_misuse(s, BURROW_S("response.WriteHeader on hijacked connection"));
        return;
    }
    if (w->wrote_header) {
        sv_log_misuse(s, BURROW_S("superfluous response.WriteHeader call"));
        return;
    }
    sv_check_write_header_code(code);

    if (code == 100 || code >= 200)
        sv_disable_write_continue(w, code >= 200);

    /* An informational response goes out now, and the real one comes
     * later. */
    if (code >= 100 && code <= 199 && code != HTTP_STATUS_SWITCHING_PROTOCOLS) {
        BufioWriter *bw = w->conn->bufw;
        sv_write_status_line(bw, http_request_proto_at_least(w->req, 1, 1), code);
        (void)burrow__http_header_write_except(w->handler_header, sv_bufw(bw),
                                               sv_excluded_headers_no_body, 2, NULL);
        sv_write_str(bw, BURROW_S("\r\n"));
        (void)bufio_writer_flush(bw);
        return;
    }

    w->wrote_header = true;
    w->status = code;

    if (w->called_header && w->cw.header == NULL)
        w->cw.header = http_header_clone(sv_wa(w), w->handler_header);

    Str cl = burrow__http_header_get(w->handler_header, BURROW_S("Content-Length"));
    if (cl.len > 0) {
        Error err;
        int64_t v = strconv_parse_int(cl, 10, 64, &err);
        if (BURROW_OK(err) && v >= 0) {
            w->content_length = v;
        } else {
            sv_logf(s, "http: invalid Content-Length of %q", cl);
            http_header_del(w->handler_header, BURROW_S("Content-Length"));
        }
    }
}

static bool sv_body_allowed(sv_Response *w) {
    if (!w->wrote_header)
        panic_str(
            BURROW_S("net/http: bodyAllowed called before the header was written"));
    return burrow__http_body_allowed_for_status(w->status);
}

/* response.write. */
static Int sv_response_write(void *self, Slice data, Error *err) {
    sv_Response *w = (sv_Response *)self;
    *err = BURROW_NO_ERROR;
    if (sv_hijacked(w->conn)) {
        if (data.len > 0)
            sv_log_misuse(w->conn->server,
                          BURROW_S("response.Write on hijacked connection"));
        *err = http_err_hijacked;
        return 0;
    }

    if (sync_atomic_bool_load(&w->can_write_continue)) {
        /* The body was not read before the response started, and it will not
         * be. */
        sv_disable_write_continue(w, true);
    }

    if (!w->wrote_header)
        sv_response_write_header(w, HTTP_STATUS_OK);
    if (data.len == 0)
        return 0;
    if (!sv_body_allowed(w)) {
        *err = http_err_body_not_allowed;
        return 0;
    }

    w->written += (int64_t)data.len; /* ignoring errors, for errorKludge */
    if (w->content_length != -1 && w->written > w->content_length) {
        *err = http_err_content_length;
        return 0;
    }
    return bufio_writer_write(w->w, data, err);
}

/* response.FlushError. */
static Error sv_response_flush(void *self) {
    sv_Response *w = (sv_Response *)self;
    if (!w->wrote_header)
        sv_response_write_header(w, HTTP_STATUS_OK);
    Error err = bufio_writer_flush(w->w);
    Error e2 = sv_cw_flush(&w->cw);
    if (BURROW_OK(err))
        err = e2;
    return err;
}

/* conn.hijackLocked, with c->mu held. */
static NetConn sv_hijack_locked(sv_Conn *c, BufioReadWriter *buf, Error *err) {
    NetConn none = {NULL, NULL};
    if (c->hijackedv) {
        *err = http_err_hijacked;
        return none;
    }
    sv_cr_abort_pending_read(&c->r);

    c->hijackedv = true;
    NetConn rwc = c->rwc;
    (void)rwc.vt->set_deadline(rwc.data, sv_zero_time());

    if (c->r.has_byte) {
        Error perr;
        (void)bufio_reader_peek(c->bufr, bufio_reader_buffered(c->bufr) + 1, &perr);
        if (BURROW_FAILED(perr)) {
            *err =
                fmt_errorf_v("unexpected Peek failure reading buffered byte: %v", perr);
            return none;
        }
    }
    /* The reader went through the connection's reader, which is gone once the
     * handler returns, and which has nothing more to give than rwc does. */
    c->bufr->rd = net_conn_as_io_reader(rwc);
    bufio_writer_reset(c->bufw, net_conn_as_io_writer(rwc));
    buf->reader = c->bufr;
    buf->writer = c->bufw;

    sv_set_state(c, HTTP_STATE_HIJACKED, true);
    *err = BURROW_NO_ERROR;
    return rwc;
}

/* response.Hijack. */
static NetConn sv_response_hijack(void *self, BufioReadWriter *buf, Error *err) {
    sv_Response *w = (sv_Response *)self;
    if (sync_atomic_bool_load(&w->handler_done))
        panic_str(BURROW_S("net/http: Hijack called after ServeHTTP finished"));
    sv_disable_write_continue(w, false);
    if (w->wrote_header)
        (void)sv_cw_flush(&w->cw);
    /* The body reads from the handler's connection from here on, and the
     * server's reader that a background read would use is gone once the
     * handler returns. */
    burrow__http_body_register_on_hit_eof(w->req_body, (Func){NULL, NULL});

    sv_Conn *c = w->conn;
    sync_mutex_lock(&c->mu);
    NetConn rwc = sv_hijack_locked(c, buf, err);
    sync_mutex_unlock(&c->mu);
    if (BURROW_OK(*err))
        w->w = NULL;
    return rwc;
}

static Error sv_response_set_read_deadline(void *self, Time deadline) {
    return sv_set_read_deadline(((sv_Response *)self)->conn->rwc, deadline);
}

static Error sv_response_set_write_deadline(void *self, Time deadline) {
    return sv_set_write_deadline(((sv_Response *)self)->conn->rwc, deadline);
}

static Error sv_response_enable_full_duplex(void *self) {
    ((sv_Response *)self)->full_duplex = true;
    return BURROW_NO_ERROR;
}

/* response.CloseNotify. */
static Chan *sv_response_close_notify(void *self) {
    sv_Response *w = (sv_Response *)self;
    sync_mutex_lock(&w->lazy_close_notify_mu);
    if (sync_atomic_bool_load(&w->handler_done)) {
        sync_mutex_unlock(&w->lazy_close_notify_mu);
        panic_str(BURROW_S("net/http: CloseNotify called after ServeHTTP finished"));
    }
    if (w->close_notify_ch == NULL) {
        w->close_notify_ch = chan_make(sv_wa(w), TYPE_BOOL, 1);
        if (w->close_notify_triggered && w->close_notify_ch != NULL) {
            bool t = true;
            chan_send(w->close_notify_ch, &t);
        }
    }
    Chan *ch = w->close_notify_ch;
    sync_mutex_unlock(&w->lazy_close_notify_mu);
    return ch;
}

/* response.closeNotify. */
static void sv_cr_close_notify(sv_Response *w) {
    sync_mutex_lock(&w->lazy_close_notify_mu);
    if (!w->close_notify_triggered) {
        w->close_notify_triggered = true;
        if (w->close_notify_ch != NULL) {
            bool t = true;
            chan_send(w->close_notify_ch, &t);
        }
    }
    sync_mutex_unlock(&w->lazy_close_notify_mu);
}

static const HttpResponseWriterVT sv_response_vt = {
    {&sv_response_desc, sv_response_write},
    sv_response_header,
    sv_response_write_header,
    sv_response_flush,
    sv_response_hijack,
    sv_response_set_read_deadline,
    sv_response_set_write_deadline,
    sv_response_enable_full_duplex,
    sv_response_close_notify,
    NULL,
};

/* ------------------------------------------------- expectContinueReader */

static Int sv_ecr_read(void *self, Slice p, Error *err) {
    sv_ExpectContinueReader *ecr = (sv_ExpectContinueReader *)self;
    if (sync_atomic_bool_load(&ecr->closed)) {
        *err = http_err_body_read_after_close;
        return 0;
    }
    sv_Response *w = ecr->resp;
    if (sync_atomic_bool_load(&w->can_write_continue)) {
        sync_mutex_lock(&w->write_continue_mu);
        if (sync_atomic_bool_load(&w->can_write_continue)) {
            sv_write_str(w->conn->bufw, BURROW_S("HTTP/1.1 100 Continue\r\n\r\n"));
            (void)bufio_writer_flush(w->conn->bufw);
            sync_atomic_bool_store(&w->can_write_continue, false);
        }
        sync_mutex_unlock(&w->write_continue_mu);
    }
    return ecr->read_closer.vt->reader.read(ecr->read_closer.data, p, err);
}

static Error sv_ecr_close(void *self) {
    sv_ExpectContinueReader *ecr = (sv_ExpectContinueReader *)self;
    if (sync_atomic_bool_load(&ecr->resp->can_write_continue))
        sv_disable_write_continue(ecr->resp, true);
    if (sync_atomic_bool_swap(&ecr->closed, true))
        return BURROW_NO_ERROR;
    return ecr->read_closer.vt->closer.close(ecr->read_closer.data);
}

static const IoReadCloserVT sv_ecr_vt = {{NULL, sv_ecr_read}, {NULL, sv_ecr_close}};

/* -------------------------------------------------------- readRequest */

/* What readRequest failed with, for the reply serve sends. */
typedef struct sv_ReadError {
    Error err;
    Int status; /* statusError's code, or 0 */
    Str text;   /* statusError's text */
    bool too_large;
} sv_ReadError;

/* numLeadingCRorLF. */
static Int sv_num_leading_crlf(Slice v) {
    const Byte *b = (const Byte *)v.p;
    Int n = 0;
    while (n < v.len && (b[n] == '\r' || b[n] == '\n'))
        n++;
    return n;
}

static bool sv_http1_server_supports_request(const HttpRequest *req) {
    if (req->proto_major == 1)
        return true;
    /* The HTTP/2 preface, which serve does not get to on its own. */
    return req->proto_major == 2 && req->proto_minor == 0 &&
           str_eq(req->method, BURROW_S("PRI")) &&
           str_eq(req->request_uri, BURROW_S("*"));
}

/* Request.isH2Upgrade. */
static bool sv_is_h2_upgrade(const HttpRequest *r) {
    return str_eq(r->method, BURROW_S("PRI")) && map_len(r->header) == 0 &&
           r->url != NULL && str_eq(r->url->path, BURROW_S("*")) &&
           str_eq(r->proto, BURROW_S("HTTP/2.0"));
}

/* Request.expectsContinue, wantsHttp10KeepAlive and wantsClose. */
static bool sv_expects_continue(const HttpRequest *r) {
    return burrow__http_has_token(
        burrow__http_header_get(r->header, BURROW_S("Expect")),
        BURROW_S("100-continue"));
}

static bool sv_wants_http10_keep_alive(const HttpRequest *r) {
    if (r->proto_major != 1 || r->proto_minor != 0)
        return false;
    return burrow__http_has_token(
        burrow__http_header_get(r->header, BURROW_S("Connection")),
        BURROW_S("keep-alive"));
}

static bool sv_wants_close(const HttpRequest *r) {
    if (r->close)
        return true;
    return burrow__http_has_token(
        burrow__http_header_get(r->header, BURROW_S("Connection")), BURROW_S("close"));
}

static void sv_response_free(sv_Response *w);

static void sv_bad_request(sv_ReadError *e, Str text) {
    e->status = HTTP_STATUS_BAD_REQUEST;
    e->text = text;
}

/* The checks readRequest makes of a request it has read. True when it is
 * fine. */
static bool sv_check_request(const HttpRequest *req, sv_ReadError *e) {
    if (!sv_http1_server_supports_request(req)) {
        e->status = HTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED;
        e->text = BURROW_S("unsupported protocol version");
        return false;
    }
    Str key = BURROW_S("Host");
    const Slice *hosts = (const Slice *)map_get(req->header, &key);
    Int nhosts = hosts == NULL ? 0 : hosts->len;
    if (http_request_proto_at_least(req, 1, 1) && nhosts == 0 &&
        !sv_is_h2_upgrade(req) && !str_eq(req->method, BURROW_S("CONNECT"))) {
        sv_bad_request(e, BURROW_S("missing required Host header"));
        return false;
    }
    if (nhosts == 1 &&
        !burrow__httpguts_valid_host_header(*(const Str *)slice_at(*hosts, 0))) {
        sv_bad_request(e, BURROW_S("malformed Host header"));
        return false;
    }
    MapIter it = map_iter(req->header);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        if (!burrow__httpguts_valid_header_field_name(*(const Str *)k)) {
            sv_bad_request(e, BURROW_S("invalid header name"));
            return false;
        }
        const Slice *vv = (const Slice *)v;
        const Str *vs = (const Str *)vv->p;
        for (Int i = 0; i < vv->len; i++) {
            if (!burrow__httpguts_valid_header_field_value(vs[i])) {
                sv_bad_request(e, BURROW_S("invalid header value"));
                return false;
            }
        }
    }
    return true;
}

/* conn.readRequest. The next request on c and the response to it, or NULL and
 * what went wrong in *e. */
static sv_Response *sv_read_request(sv_Conn *c, Context ctx, sv_ReadError *e) {
    HttpServer *s = c->server;
    Alloc *sa = sv_alloc(s);
    *e = (sv_ReadError){BURROW_NO_ERROR, 0, {NULL, 0}, false};
    if (sv_hijacked(c)) {
        e->err = http_err_hijacked;
        return NULL;
    }

    Time t0 = time_now();
    Time whole_req_deadline = sv_zero_time();
    if (s->read_timeout > 0)
        whole_req_deadline = time_add(t0, s->read_timeout);

    sv_Response *w = NULL;
    c->r.remain = sv_initial_read_limit_size(s);
    if (c->last_was_post) {
        /* RFC 7230 section 3 tolerance for old buggy clients, which send a
         * CRLF after a POST body. */
        Error perr;
        Slice peek = bufio_reader_peek(c->bufr, 4, &perr); /* readRequest sees err */
        (void)bufio_reader_discard(c->bufr, sv_num_leading_crlf(peek), &perr);
    }
    Error err;
    HttpRequest *req = burrow__http_read_request_limit(
        sa, c->bufr, (int64_t)sv_max_header_value_count(s), &err);
    if (req == NULL) {
        if (c->r.remain <= 0)
            e->too_large = true;
        else
            e->err = err;
        goto out;
    }

    if (!sv_check_request(req, e)) {
        http_request_free(req);
        goto out;
    }

    c->last_was_post = str_eq(req->method, BURROW_S("POST"));
    c->r.remain = INT64_MAX;
    bool is_h2_upgrade = sv_is_h2_upgrade(req);
    http_header_del(req->header, BURROW_S("Host"));

    w = (sv_Response *)mem_alloc(sa, sizeof *w, _Alignof(sv_Response));
    if (w == NULL) {
        http_request_free(req);
        e->err = burrow_err_out_of_memory;
        goto out;
    }
    memset(w, 0, sizeof *w);
    arena_init(&w->arena, sa, 0);
    w->conn = c;
    w->req = req;
    w->ctx = context_with_cancel(sv_wa(w), ctx, &w->cancel_ctx);
    w->handler_header = http_header_make(sv_wa(w));
    w->cw.res = w;
    w->w = bufio_new_writer_size(sv_wa(w), (IoWriter){&sv_cw_vt, &w->cw},
                                 SV_BUFFER_BEFORE_CHUNKING_SIZE);
    w->trailers = slice_from(NULL, 0, 0, TYPE_STRING);
    if (w->ctx.vt == NULL || w->handler_header == NULL || w->w == NULL) {
        sv_response_free(w);
        w = NULL;
        e->err = burrow_err_out_of_memory;
        goto out;
    }
    req->ctx = w->ctx;
    req->remote_addr = c->remote_addr;
    if (req->body.vt != http_no_body.vt && req->wire != NULL) {
        w->req_body = req->wire;
        burrow__http_body_set_do_early_close(w->req_body, true);
    }

    (void)sv_set_read_deadline(c->rwc, whole_req_deadline);

    w->content_length = -1;
    w->wants10_keep_alive = sv_wants_http10_keep_alive(req);
    w->wants_close = sv_wants_close(req);
    if (is_h2_upgrade)
        w->close_after_reply = true;

out:
    if (s->write_timeout > 0)
        (void)sv_set_write_deadline(c->rwc, time_add(time_now(), s->write_timeout));
    return w;
}

/* Gives back the response, its request and what they hold. */
static void sv_response_free(sv_Response *w) {
    if (w == NULL)
        return;
    Alloc *sa = sv_alloc(w->conn->server);
    if (w->ctx.vt != NULL)
        context_release(w->ctx);
    if (w->close_notify_ch != NULL)
        chan_free(w->close_notify_ch);
    http_request_free(w->req);
    arena_free(&w->arena);
    mem_free(sa, w, sizeof *w, _Alignof(sv_Response));
}

/* response.finishRequest. */
static void sv_finish_request(sv_Response *w) {
    sync_atomic_bool_store(&w->handler_done, true);

    if (!w->wrote_header)
        sv_response_write_header(w, HTTP_STATUS_OK);

    (void)bufio_writer_flush(w->w);
    sv_cw_close(&w->cw);
    (void)bufio_writer_flush(w->conn->bufw);

    sv_cr_abort_pending_read(&w->conn->r);

    /* Prevent a new background read from starting. */
    burrow__http_body_register_on_hit_eof(w->req_body, (Func){NULL, NULL});

    if (sync_atomic_bool_load(&w->can_write_continue))
        sv_disable_write_continue(w, true);

    /* Close the body (regardless of w->close_after_reply) so we can re-use
     * its bufio.Reader later safely. */
    (void)burrow__http_body_close(w->req_body);
}

static bool sv_closed_request_body_early(sv_Response *w) {
    return w->req_body != NULL && burrow__http_body_did_early_close(w->req_body);
}

/* response.shouldReuseConnection. */
static bool sv_should_reuse_connection(sv_Response *w) {
    if (w->close_after_reply)
        return false;
    if (!str_eq(w->req->method, BURROW_S("HEAD")) && w->content_length != -1 &&
        sv_body_allowed(w) && w->content_length != w->written)
        return false; /* Did not write enough, and the client is confused. */
    if (BURROW_FAILED(w->conn->werr))
        return false;
    if (sv_closed_request_body_early(w))
        return false;
    return true;
}

/* response.sendExpectationFailed. */
static void sv_send_expectation_failed(sv_Response *w) {
    (void)http_header_set(sv_response_header(w), BURROW_S("Connection"),
                          BURROW_S("close"));
    sv_response_write_header(w, HTTP_STATUS_EXPECTATION_FAILED);
    sv_finish_request(w);
}

/* ------------------------------------------------------- serverHandler */

/* globalOptionsHandler, which answers "OPTIONS *". */
static void sv_global_options_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    (void)self;
    (void)http_header_set(http_response_writer_header(w), BURROW_S("Content-Length"),
                          BURROW_S("0"));
    if (r->content_length != 0) {
        /* Read up to 4 KiB of the body, so the connection can be used
         * again. */
        IoReadCloser mb =
            http_max_bytes_reader(arena_allocator(&r->arena), w, r->body, 4 << 10);
        if (mb.vt != NULL) {
            Error err;
            (void)io_copy(heap_allocator(), io_discard,
                          (IoReader){&mb.vt->reader, mb.data}, &err);
        }
    }
}

static const HttpHandlerVT sv_global_options_vt = {NULL, sv_global_options_serve};

/* serverHandler.ServeHTTP. */
static void sv_server_handler_serve(HttpServer *s, HttpResponseWriter rw,
                                    HttpRequest *req) {
    HttpHandler handler = s->handler;
    if (handler.vt == NULL)
        handler = http_serve_mux_as_handler(http_default_serve_mux);
    if (!s->disable_general_options_handler &&
        str_eq(req->request_uri, BURROW_S("*")) &&
        str_eq(req->method, BURROW_S("OPTIONS")))
        handler = (HttpHandler){&sv_global_options_vt, NULL};
    http_handler_serve_http(handler, rw, req);
}

/* A copy of a caught panic's value that outlives the frame that caught it, as
 * sync's once does. */
static Any sv_keep(burrow__PanicValue *storage, Any v) {
    if (v.t == NULL || v.data == NULL || v.t->size > sizeof storage->bytes)
        return v;
    type_copy(v.t, storage->bytes, v.data);
    v.data = storage->bytes;
    return v;
}

/* Runs the handler, and true with the value in *out when it panicked. */
static bool sv_call_handler(HttpServer *s, sv_Response *w, burrow__PanicValue *storage,
                            Any *out) {
    volatile bool panicked = false;
    BURROW_TRY {
        sv_server_handler_serve(s, sv_as_writer(w), w->req);
    }
    BURROW_CATCH(p) {
        *out = sv_keep(storage, p);
        panicked = true;
    }
    BURROW_TRY_END;
    return panicked;
}

static bool sv_is_abort_handler(Any p) {
    if (p.t != TYPE_ERROR || p.data == NULL)
        return false;
    const Error *e = (const Error *)p.data;
    return e->vt == http_err_abort_handler.vt && e->data == http_err_abort_handler.data;
}

static void sv_log_panic(sv_Conn *c, Any p) {
    enum { size = 64 << 10 };
    Alloc *h = heap_allocator();
    Byte *buf = (Byte *)mem_alloc(h, size, 1);
    Int n =
        buf == NULL ? 0 : runtime_stack(slice_from(buf, size, size, TYPE_BYTE), false);
    sv_logf(c->server, "http: panic serving %v: %v\n%s", c->remote_addr, panic_text(p),
            str_from_bytes(buf, n));
    mem_free(h, buf, size, 1);
}

/* ------------------------------------------------------------ conn.serve */

/* isCommonNetReadError. */
static bool sv_is_common_net_read_error(Error err) {
    if (errors_is(err, io_eof) && err.vt == io_eof.vt && err.data == io_eof.data)
        return true;
    if (net_is_error(err) && net_error_timeout(err))
        return true;
    if (err.vt != NULL && err.vt->self_type == TYPE_NET_OP_ERROR) {
        const NetOpError *oe = (const NetOpError *)err.data;
        if (str_eq(oe->op, BURROW_S("read")))
            return true;
    }
    return false;
}

/* finalFlush and close. */
static void sv_final_flush(sv_Conn *c) {
    if (c->bufw != NULL)
        (void)bufio_writer_flush(c->bufw);
}

static void sv_conn_close(sv_Conn *c) {
    sv_final_flush(c);
    (void)sv_conn_close_rwc(c->rwc);
}

/* closeWriteAndWait. Flushes, closes the write side, and waits a little, so
 * the client reads the response before a reset can get to it. */
static void sv_close_write_and_wait(sv_Conn *c) {
    sv_final_flush(c);
    NetTCPConn *tcp = net_conn_as_tcp_conn(c->rwc);
    if (tcp != NULL)
        (void)net_tcp_conn_close_write(tcp);
    time_sleep(SV_RST_AVOIDANCE_DELAY);
}

static void sv_write_raw(sv_Conn *c, Str s) {
    Error err;
    (void)c->rwc.vt->writer.write(
        c->rwc.data, slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE), &err);
}

#define SV_ERROR_HEADERS                                                               \
    "\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\n"

/* The reply to a request that could not be read. False when there is none,
 * and the connection just closes. */
static void sv_reply_read_error(sv_Conn *c, const sv_ReadError *e) {
    if (e->too_large) {
        /* Their HTTP client may or may not be able to read this if we are
         * responding to them and hanging up while they are still writing
         * their request. Undefined behaviour. */
        sv_write_raw(
            c, BURROW_S("HTTP/1.1 431 Request Header Fields Too Large" SV_ERROR_HEADERS
                        "431 Request Header Fields Too Large"));
        sv_close_write_and_wait(c);
        return;
    }
    if (e->status == 0 && burrow__http_is_unsupported_te_error(e->err)) {
        /* Respond as per RFC 7230 Section 3.3.1, which says that a server
         * receiving a transfer coding it does not understand should respond
         * with 501 Not Implemented. */
        (void)fmt_fprintf_v(net_conn_as_io_writer(c->rwc),
                            "HTTP/1.1 %d %s%sUnsupported transfer encoding",
                            (Int)HTTP_STATUS_NOT_IMPLEMENTED,
                            http_status_text(HTTP_STATUS_NOT_IMPLEMENTED),
                            BURROW_S(SV_ERROR_HEADERS));
        return;
    }
    if (e->status == 0 && sv_is_common_net_read_error(e->err))
        return; /* don't reply */
    if (e->status != 0) {
        Str st = http_status_text(e->status);
        (void)fmt_fprintf_v(net_conn_as_io_writer(c->rwc),
                            "HTTP/1.1 %d %s: %s%s%d %s: %s", e->status, st, e->text,
                            BURROW_S(SV_ERROR_HEADERS), e->status, st, e->text);
        return;
    }
    sv_write_raw(
        c, BURROW_S("HTTP/1.1 400 Bad Request" SV_ERROR_HEADERS "400 Bad Request"));
}

static void sv_serve_ctx_unref(sv_ServeCtx *sc) {
    if (sync_atomic_int64_add(&sc->refs, -1) != 0)
        return;
    context_release(sc->ctx);
    mem_free(sc->a, sc, sizeof *sc, _Alignof(sv_ServeCtx));
}

/* Frees the NetConn, when its kind is one that is the server's to free. */
static void sv_free_rwc(NetConn rwc) {
    NetTCPConn *tcp = net_conn_as_tcp_conn(rwc);
    if (tcp != NULL) {
        net_tcp_conn_free(tcp);
        return;
    }
    NetUnixConn *unix_conn = net_conn_as_unix_conn(rwc);
    if (unix_conn != NULL)
        net_unix_conn_free(unix_conn);
}

/* conn.serve, on a goroutine of its own. */
static void sv_conn_serve(void *env) {
    sv_Conn *c = (sv_Conn *)env;
    HttpServer *s = c->server;
    Alloc *sa = sv_alloc(s);
    Alloc *ca = arena_allocator(&c->arena);
    sv_Response *in_flight = NULL;
    sv_Response *cur = NULL;
    burrow__PanicValue storage;
    Any pv = {NULL, NULL};

    NetAddr ra = c->rwc.vt->remote_addr(c->rwc.data);
    if (ra.vt != NULL)
        c->remote_addr = ra.vt->string(ra.data, ca);
    NetAddr la = c->rwc.vt->local_addr(c->rwc.data);
    Any laddr = {la.vt != NULL ? la.vt->self_type : NULL, la.data};
    c->ctx = context_with_value(ca, c->conn_ctx, http_local_addr_context_key, laddr);
    if (c->ctx.vt == NULL)
        goto done;

    c->cctx = context_with_cancel(ca, c->ctx, &c->cancel_ctx);
    if (c->cctx.vt == NULL)
        goto done;

    c->r.rwc = c->rwc;
    c->r.conn = c;
    c->r.cond = SYNC_COND(sync_mutex_locker(&c->r.mu));
    c->bufr = bufio_new_reader(sa, (IoReader){&sv_cr_vt, &c->r});
    c->bufw = bufio_new_writer_size(sa, (IoWriter){&sv_check_conn_error_vt, c},
                                    SV_CONN_WRITER_SIZE);
    if (c->bufr == NULL || c->bufw == NULL)
        goto done;

    Duration d = sv_read_header_timeout(s);
    if (d > 0)
        (void)sv_set_read_deadline(c->rwc, time_add(time_now(), d));

    if (!http_protocols_http1(sv_protocols(s)))
        goto done;

    for (;;) {
        sv_ReadError rerr;
        cur = sv_read_request(c, c->cctx, &rerr);
        if (c->r.remain != sv_initial_read_limit_size(s)) {
            /* If we read any bytes off the wire, we're active. */
            sv_set_state(c, HTTP_STATE_ACTIVE, true);
        }
        if (sv_shutting_down(s))
            goto done;
        if (cur == NULL) {
            sv_reply_read_error(c, &rerr);
            goto done;
        }

        /* Expect 100 Continue support. */
        sv_Response *w = cur;
        HttpRequest *req = w->req;
        if (sv_expects_continue(req)) {
            if (http_request_proto_at_least(req, 1, 1) && req->content_length != 0) {
                /* Wrap the body reader with one that replies on the
                 * connection. */
                sv_ExpectContinueReader *ecr = (sv_ExpectContinueReader *)mem_alloc(
                    sv_wa(w), sizeof *ecr, _Alignof(sv_ExpectContinueReader));
                if (ecr == NULL)
                    goto done;
                memset(ecr, 0, sizeof *ecr);
                ecr->read_closer = req->body;
                ecr->resp = w;
                w->ec_reader = ecr;
                sync_atomic_bool_store(&w->can_write_continue, true);
                req->body = (IoReadCloser){&sv_ecr_vt, ecr};
            }
        } else if (burrow__http_header_get(req->header, BURROW_S("Expect")).len > 0) {
            sv_send_expectation_failed(w);
            goto done;
        }

        sync_atomic_pointer_store(&c->cur_req, w);

        if (burrow__http_body_remains(w->req_body)) {
            burrow__http_body_register_on_hit_eof(
                w->req_body, BURROW_FN(Func, sv_cr_start_background_read, &c->r));
        } else {
            sv_cr_start_background_read(&c->r);
        }

        /* HTTP cannot have multiple simultaneous active requests. Until the
         * server replies to this request, it can't read another, so we might
         * as well run the handler in this goroutine. */
        in_flight = w;
        if (sv_call_handler(s, w, &storage, &pv)) {
            if (!sv_is_abort_handler(pv))
                sv_log_panic(c, pv);
            goto done;
        }
        in_flight = NULL;
        BURROW_CALLF0(w->cancel_ctx);
        if (sv_hijacked(c)) {
            sv_cr_release_conn(&c->r);
            goto done;
        }
        sv_finish_request(w);
        (void)sv_set_write_deadline(c->rwc, sv_zero_time());
        if (!sv_should_reuse_connection(w)) {
            if (w->request_body_limit_hit || sv_closed_request_body_early(w))
                sv_close_write_and_wait(c);
            goto done;
        }
        sv_set_state(c, HTTP_STATE_IDLE, true);
        sync_atomic_pointer_store(&c->cur_req, NULL);
        sv_response_free(w);
        cur = NULL;

        if (!sv_do_keep_alives(s)) {
            /* We're in shutdown mode. We might've replied to the user
             * without "Connection: close" and they might think they can
             * send another request, but such is life with HTTP/1.1. */
            goto done;
        }

        d = sv_idle_timeout(s);
        (void)sv_set_read_deadline(c->rwc,
                                   d > 0 ? time_add(time_now(), d) : sv_zero_time());

        /* Wait for the connection to become readable again before trying to
         * read the next request. This prevents a ReadHeaderTimeout or
         * ReadTimeout from starting until the first bytes of the next request
         * have been received. */
        Error perr;
        (void)bufio_reader_peek(c->bufr, 4, &perr);
        if (BURROW_FAILED(perr))
            goto done;

        d = sv_read_header_timeout(s);
        (void)sv_set_read_deadline(c->rwc,
                                   d > 0 ? time_add(time_now(), d) : sv_zero_time());
    }

done:;
    bool hijacked = sv_hijacked(c);
    if (in_flight != NULL) {
        BURROW_CALLF0(in_flight->cancel_ctx);
        sv_disable_write_continue(in_flight, true);
    }
    if (!hijacked) {
        /* A background read has c, which is freed below, so it has to be
         * over first. Go leaves it to fail when the connection closes. */
        sv_cr_abort_pending_read(&c->r);
        if (in_flight != NULL) {
            burrow__http_body_register_on_hit_eof(in_flight->req_body,
                                                  (Func){NULL, NULL});
            (void)burrow__http_body_close(in_flight->req_body);
        }
        sv_conn_close(c);
        sv_set_state(c, HTTP_STATE_CLOSED, true);
    }
    if (!BURROW_FUNC_IS_NIL(c->cancel_ctx))
        BURROW_CALLF0(c->cancel_ctx);
    sync_atomic_pointer_store(&c->cur_req, NULL);
    sv_response_free(cur);
    if (c->cctx.vt != NULL)
        context_release(c->cctx);
    if (c->ctx.vt != NULL)
        context_release(c->ctx);
    if (!hijacked) {
        bufio_reader_free(c->bufr);
        bufio_writer_free(c->bufw);
        sv_free_rwc(c->rwc);
    }
    /* Off the list whatever happened, before it goes. */
    sv_track_conn(c, false);
    sv_serve_ctx_unref(c->sctx);
    arena_free(&c->arena);
    mem_free(sa, c, sizeof *c, _Alignof(sv_Conn));
    sync_wait_group_done(&s->conn_group);
}

/* -------------------------------------------------------------- Serve */

/* trackListener. */
static bool sv_track_listener(HttpServer *s, sv_Listener *ln, bool add) {
    sync_mutex_lock(&s->mu);
    if (add) {
        if (sv_shutting_down(s)) {
            sync_mutex_unlock(&s->mu);
            return false;
        }
        ln->prev = NULL;
        ln->next = s->listeners;
        if (s->listeners != NULL)
            s->listeners->prev = ln;
        s->listeners = ln;
        sync_wait_group_add(&s->listener_group, 1);
    } else {
        if (ln->prev != NULL)
            ln->prev->next = ln->next;
        else
            s->listeners = ln->next;
        if (ln->next != NULL)
            ln->next->prev = ln->prev;
        sync_wait_group_done(&s->listener_group);
    }
    sync_mutex_unlock(&s->mu);
    return true;
}

/* onceCloseListener.Close. */
static Error sv_listener_close(sv_Listener *ln) {
    sync_mutex_lock(&ln->mu);
    if (!ln->closed) {
        ln->closed = true;
        ln->close_err = ln->l.vt->closer.close(ln->l.data);
    }
    Error err = ln->close_err;
    sync_mutex_unlock(&ln->mu);
    return err;
}

/* closeListenersLocked. */
static Error sv_close_listeners_locked(HttpServer *s) {
    Error err = BURROW_NO_ERROR;
    for (sv_Listener *ln = s->listeners; ln != NULL; ln = ln->next) {
        Error cerr = sv_listener_close(ln);
        if (BURROW_FAILED(cerr) && BURROW_OK(err))
            err = cerr;
    }
    return err;
}

/* The accept loop of Serve, which gives the error to return. */
static Error sv_accept_loop(HttpServer *s, sv_Listener *ln, NetListener orig,
                            sv_ServeCtx *sc) {
    Alloc *sa = sv_alloc(s);
    Duration temp_delay = 0; /* how long to sleep on accept failure */
    for (;;) {
        Error err;
        NetConn rw = ln->l.vt->accept(ln->l.data, &err);
        if (BURROW_FAILED(err)) {
            if (sv_shutting_down(s))
                return http_err_server_closed;
            if (net_error_temporary(err)) {
                temp_delay = temp_delay == 0 ? 5 * TIME_MILLISECOND : temp_delay * 2;
                if (temp_delay > TIME_SECOND)
                    temp_delay = TIME_SECOND;
                Str ds = duration_string(temp_delay, heap_allocator());
                sv_logf(s, "http: Accept error: %v; retrying in %s", err, ds);
                mem_free(heap_allocator(), (void *)(uintptr_t)ds.p, (size_t)ds.len, 1);
                time_sleep(temp_delay);
                continue;
            }
            return err;
        }
        (void)orig;
        Context conn_ctx = sc->ctx;
        if (!BURROW_FUNC_IS_NIL(s->conn_context)) {
            conn_ctx = BURROW_CALLF(s->conn_context, conn_ctx, rw);
            if (conn_ctx.vt == NULL)
                panic_str(BURROW_S("ConnContext returned nil"));
        }
        temp_delay = 0;
        sv_Conn *c = (sv_Conn *)mem_alloc(sa, sizeof *c, _Alignof(sv_Conn));
        if (c == NULL) {
            (void)rw.vt->closer.close(rw.data);
            sv_free_rwc(rw);
            continue;
        }
        memset(c, 0, sizeof *c);
        c->server = s;
        c->rwc = rw;
        c->sctx = sc;
        c->conn_ctx = conn_ctx;
        arena_init(&c->arena, sa, 0);
        (void)sync_atomic_int64_add(&sc->refs, 1);
        sync_wait_group_add(&s->conn_group, 1);
        sv_set_state(c, HTTP_STATE_NEW, true); /* before Serve can return */
        if (!go(BURROW_FN(Func, sv_conn_serve, c))) {
            /* No goroutine, so serve it here rather than drop it. */
            sv_conn_serve(c);
        }
    }
}

Error http_server_serve(HttpServer *s, NetListener l) {
    Alloc *sa = sv_alloc(s);
    sv_Listener *ln = (sv_Listener *)mem_alloc(sa, sizeof *ln, _Alignof(sv_Listener));
    if (ln == NULL) {
        (void)l.vt->closer.close(l.data);
        return burrow_err_out_of_memory;
    }
    memset(ln, 0, sizeof *ln);
    ln->l = l;

    Error err;
    if (!sv_track_listener(s, ln, true)) {
        err = http_err_server_closed;
        goto close;
    }

    Context base_ctx = context_background();
    if (!BURROW_FUNC_IS_NIL(s->base_context)) {
        base_ctx = BURROW_CALLF(s->base_context, l);
        if (base_ctx.vt == NULL)
            panic_str(BURROW_S("BaseContext returned a nil context"));
    }
    sv_ServeCtx *sc = (sv_ServeCtx *)mem_alloc(sa, sizeof *sc, _Alignof(sv_ServeCtx));
    if (sc == NULL) {
        err = burrow_err_out_of_memory;
        goto untrack;
    }
    sc->a = sa;
    sync_atomic_int64_store(&sc->refs, 1);
    sc->ctx = context_with_value(sa, base_ctx, http_server_context_key,
                                 BURROW_ANY(TYPE_HTTP_SERVER, s));
    if (sc->ctx.vt == NULL) {
        mem_free(sa, sc, sizeof *sc, _Alignof(sv_ServeCtx));
        err = burrow_err_out_of_memory;
        goto untrack;
    }
    err = sv_accept_loop(s, ln, l, sc);
    sv_serve_ctx_unref(sc);

untrack:
    sv_track_listener(s, ln, false);
close:
    (void)sv_listener_close(ln);
    mem_free(sa, ln, sizeof *ln, _Alignof(sv_Listener));
    return err;
}

/* -------------------------------------------------------- ListenAndServe */

/* The ports Go knows without /etc/services, from net's lookup.go. */
static const struct {
    const char *name;
    Int port;
} sv_services[] = {
    {"ftp", 21},    {"ftps", 990},  {"gopher", 70}, {"http", 80},
    {"https", 443}, {"imap2", 143}, {"imap3", 220}, {"imaps", 993},
    {"pop3", 110},  {"pop3s", 995}, {"smtp", 25},   {"submissions", 465},
    {"ssh", 22},    {"telnet", 23},
};

/* The TCP address addr names, a host that is empty, an IP address or a name
 * in the hosts file, and a port that is a number or a service. */
static Error sv_resolve_listen_addr(Alloc *a, Str addr, NetTCPAddr *out) {
    Str port;
    Error err;
    Str host = net_split_host_port(addr, &port, &err);
    if (BURROW_FAILED(err))
        return err;
    memset(out, 0, sizeof *out);

    Error perr;
    int64_t pn = strconv_parse_int(port, 10, 64, &perr);
    if (BURROW_FAILED(perr)) {
        pn = -1;
        for (size_t i = 0; i < sizeof sv_services / sizeof sv_services[0]; i++) {
            if (strings_equal_fold(port, str_from_cstr(sv_services[i].name))) {
                pn = sv_services[i].port;
                break;
            }
        }
        if (pn < 0)
            return fmt_errorf_v("lookup tcp/%s: unknown port", port);
    }
    if (pn < 0 || pn > 0xffff)
        return fmt_errorf_v("address %s: invalid port", port);
    out->port = (Int)pn;

    if (host.len == 0)
        return BURROW_NO_ERROR;
    Int pct = strings_index_byte(host, '%');
    Str bare = host;
    if (pct >= 0) {
        bare = sv_sub(host, 0, pct);
        out->zone = sv_sub(host, pct + 1, host.len);
    }
    out->ip = net_parse_ip(a, bare);
    if (out->ip.len > 0)
        return BURROW_NO_ERROR;
    Str canonical;
    Slice addrs = burrow__net_lookup_static_host(a, host, &canonical);
    const Str *as = (const Str *)addrs.p;
    for (Int i = 0; i < addrs.len; i++) {
        out->ip = net_parse_ip(a, as[i]);
        if (out->ip.len > 0)
            return BURROW_NO_ERROR;
    }
    return fmt_errorf_v("lookup %s: no such host", host);
}

Error http_server_listen_and_serve(HttpServer *s) {
    if (sv_shutting_down(s))
        return http_err_server_closed;
    Str addr = s->addr;
    if (addr.len == 0)
        addr = BURROW_S(":http");
    Alloc *sa = sv_alloc(s);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    NetTCPAddr la;
    Error err = sv_resolve_listen_addr(arena_allocator(&ar), addr, &la);
    if (BURROW_FAILED(err)) {
        err = fmt_errorf_v("listen tcp: %w", err);
        arena_free(&ar);
        return err;
    }
    NetTCPListener *ln = net_listen_tcp(sa, BURROW_S("tcp"), &la, &err);
    arena_free(&ar);
    if (ln == NULL)
        return err;
    err = http_server_serve(s, net_tcp_listener_as_listener(ln));
    net_tcp_listener_free(ln);
    return err;
}

/* ------------------------------------------------------ Close and Shutdown */

Error http_server_close(HttpServer *s) {
    sync_atomic_bool_store(&s->in_shutdown, true);
    sync_mutex_lock(&s->mu);
    Error err = sv_close_listeners_locked(s);

    /* Unlock while waiting for the Serve calls to stop, since they untrack
     * their listeners on the way out. */
    sync_mutex_unlock(&s->mu);
    sync_wait_group_wait(&s->listener_group);
    sync_mutex_lock(&s->mu);

    for (sv_Conn *c = s->active_conn; c != NULL;) {
        sv_Conn *next = c->next;
        (void)sv_conn_close_rwc(c->rwc);
        c->prev = c->next = NULL;
        c->tracked = false;
        c = next;
    }
    s->active_conn = NULL;
    sync_mutex_unlock(&s->mu);
    return err;
}

/* closeIdleConns. Closes the idle connections, and true when there were no
 * others. */
static bool sv_close_idle_conns(HttpServer *s) {
    sync_mutex_lock(&s->mu);
    bool quiescent = true;
    int64_t now = time_unix(time_now());
    for (sv_Conn *c = s->active_conn; c != NULL;) {
        sv_Conn *next = c->next;
        int64_t unix_sec;
        HttpConnState st = sv_get_state(c, &unix_sec);
        /* Issue 22682: treat StateNew connections as if they're idle if we
         * haven't read the first request's header in over 5 seconds. */
        if (st == HTTP_STATE_NEW && unix_sec < now - 5)
            st = HTTP_STATE_IDLE;
        if (st != HTTP_STATE_IDLE || unix_sec == 0) {
            /* Assume unix_sec == 0 means it's a very new connection, without
             * state set yet. */
            quiescent = false;
            c = next;
            continue;
        }
        (void)sv_conn_close_rwc(c->rwc);
        if (c->prev != NULL)
            c->prev->next = c->next;
        else
            s->active_conn = c->next;
        if (c->next != NULL)
            c->next->prev = c->prev;
        c->prev = c->next = NULL;
        c->tracked = false;
        c = next;
    }
    sync_mutex_unlock(&s->mu);
    return quiescent;
}

static void sv_run_on_shutdown(void *env) {
    Func *f = (Func *)env;
    BURROW_CALLF0(*f);
}

Error http_server_shutdown(HttpServer *s, Context ctx) {
    sync_atomic_bool_store(&s->in_shutdown, true);

    sync_mutex_lock(&s->mu);
    Error lnerr = sv_close_listeners_locked(s);
    Func *fs = (Func *)s->on_shutdown.p;
    for (Int i = 0; i < s->on_shutdown.len; i++) {
        if (!go(BURROW_FN(Func, sv_run_on_shutdown, &fs[i])))
            BURROW_CALLF0(fs[i]);
    }
    sync_mutex_unlock(&s->mu);
    sync_wait_group_wait(&s->listener_group);

    Duration poll_interval_base = TIME_MILLISECOND;
    TimeTimer *timer = NULL;
    Error err = BURROW_NO_ERROR;
    for (;;) {
        if (sv_close_idle_conns(s)) {
            err = lnerr;
            break;
        }
        /* Add 10% jitter. */
        Duration interval =
            poll_interval_base +
            (Duration)(runtime_rand64() % (uint64_t)(poll_interval_base / 10));
        poll_interval_base *= 2;
        if (poll_interval_base > SV_SHUTDOWN_POLL_INTERVAL_MAX)
            poll_interval_base = SV_SHUTDOWN_POLL_INTERVAL_MAX;
        if (timer == NULL) {
            timer = time_new_timer(heap_allocator(), interval);
            if (timer == NULL) {
                err = burrow_err_out_of_memory;
                break;
            }
        } else {
            bool pending;
            (void)time_timer_reset(timer, interval, &pending);
        }
        SelectCase cases[] = {
            BURROW_RECV(context_done(ctx), NULL),
            BURROW_RECV(time_timer_c(timer), NULL),
        };
        if (chan_select(cases, 2) == 0) {
            err = context_err(ctx);
            break;
        }
    }
    if (timer != NULL) {
        (void)time_timer_stop(timer);
        time_timer_free(timer);
    }
    return err;
}

bool http_server_register_on_shutdown(HttpServer *s, Func f) {
    Alloc *a = sv_alloc(s);
    sync_mutex_lock(&s->mu);
    Slice *fs = &s->on_shutdown;
    if (fs->len == fs->cap) {
        Int cap = fs->cap == 0 ? 4 : fs->cap * 2;
        Func *p = (Func *)mem_alloc(a, (size_t)cap * sizeof(Func), _Alignof(Func));
        if (p == NULL) {
            sync_mutex_unlock(&s->mu);
            return false;
        }
        if (fs->len > 0)
            memcpy(p, fs->p, (size_t)fs->len * sizeof(Func));
        if (fs->p != NULL)
            mem_free(a, fs->p, (size_t)fs->cap * sizeof(Func), _Alignof(Func));
        fs->p = p;
        fs->cap = cap;
    }
    ((Func *)fs->p)[fs->len++] = f;
    sync_mutex_unlock(&s->mu);
    return true;
}

void http_server_set_keep_alives_enabled(HttpServer *s, bool v) {
    if (v) {
        sync_atomic_bool_store(&s->disable_keep_alives, false);
        return;
    }
    sync_atomic_bool_store(&s->disable_keep_alives, true);

    /* Close idle HTTP/1 conns: */
    (void)sv_close_idle_conns(s);
}

void http_server_free(HttpServer *s) {
    if (s == NULL)
        return;
    sync_wait_group_wait(&s->conn_group);
    Slice *fs = &s->on_shutdown;
    if (fs->p != NULL)
        mem_free(sv_alloc(s), fs->p, (size_t)fs->cap * sizeof(Func), _Alignof(Func));
    fs->p = NULL;
    fs->len = fs->cap = 0;
}

Error http_serve(NetListener l, HttpHandler handler) {
    HttpServer srv = {.handler = handler};
    Error err = http_server_serve(&srv, l);
    http_server_free(&srv);
    return err;
}

Error http_listen_and_serve(Str addr, HttpHandler handler) {
    HttpServer srv = {.addr = addr, .handler = handler};
    Error err = http_server_listen_and_serve(&srv);
    http_server_free(&srv);
    return err;
}

/* ---------------------------------------------------- ResponseController */

HttpResponseController http_new_response_controller(HttpResponseWriter rw) {
    HttpResponseController c = {rw};
    return c;
}

typedef enum sv_RcMethod {
    SV_RC_FLUSH,
    SV_RC_HIJACK,
    SV_RC_SET_READ_DEADLINE,
    SV_RC_SET_WRITE_DEADLINE,
    SV_RC_ENABLE_FULL_DUPLEX,
} sv_RcMethod;

static bool sv_rc_has(const HttpResponseWriterVT *vt, sv_RcMethod m) {
    switch (m) {
    case SV_RC_FLUSH:
        return vt->flush != NULL;
    case SV_RC_HIJACK:
        return vt->hijack != NULL;
    case SV_RC_SET_READ_DEADLINE:
        return vt->set_read_deadline != NULL;
    case SV_RC_SET_WRITE_DEADLINE:
        return vt->set_write_deadline != NULL;
    case SV_RC_ENABLE_FULL_DUPLEX:
        return vt->enable_full_duplex != NULL;
    default:
        return false;
    }
}

/* The writer in the chain from rw, through unwrap, that has method m, or
 * false. */
static bool sv_rc_find(HttpResponseWriter rw, sv_RcMethod m, HttpResponseWriter *out) {
    for (;;) {
        if (rw.vt == NULL)
            return false;
        if (sv_rc_has(rw.vt, m)) {
            *out = rw;
            return true;
        }
        if (rw.vt->unwrap == NULL)
            return false;
        rw = rw.vt->unwrap(rw.data);
    }
}

Error http_response_controller_flush(HttpResponseController *c) {
    HttpResponseWriter rw;
    if (!sv_rc_find(c->rw, SV_RC_FLUSH, &rw))
        return sv_err_not_supported();
    return rw.vt->flush(rw.data);
}

NetConn http_response_controller_hijack(HttpResponseController *c, BufioReadWriter *buf,
                                        Error *err) {
    HttpResponseWriter rw;
    if (!sv_rc_find(c->rw, SV_RC_HIJACK, &rw)) {
        *err = sv_err_not_supported();
        buf->reader = NULL;
        buf->writer = NULL;
        return (NetConn){NULL, NULL};
    }
    return rw.vt->hijack(rw.data, buf, err);
}

Error http_response_controller_set_read_deadline(HttpResponseController *c,
                                                 Time deadline) {
    HttpResponseWriter rw;
    if (!sv_rc_find(c->rw, SV_RC_SET_READ_DEADLINE, &rw))
        return sv_err_not_supported();
    return rw.vt->set_read_deadline(rw.data, deadline);
}

Error http_response_controller_set_write_deadline(HttpResponseController *c,
                                                  Time deadline) {
    HttpResponseWriter rw;
    if (!sv_rc_find(c->rw, SV_RC_SET_WRITE_DEADLINE, &rw))
        return sv_err_not_supported();
    return rw.vt->set_write_deadline(rw.data, deadline);
}

Error http_response_controller_enable_full_duplex(HttpResponseController *c) {
    HttpResponseWriter rw;
    if (!sv_rc_find(c->rw, SV_RC_ENABLE_FULL_DUPLEX, &rw))
        return sv_err_not_supported();
    return rw.vt->enable_full_duplex(rw.data);
}

/* ------------------------------------------------------- TimeoutHandler */

typedef struct sv_TimeoutHandler {
    HttpHandler handler;
    Str body;
    Duration dt;
} sv_TimeoutHandler;

/* timeoutWriter, and what one request under the handler needs. */
typedef struct sv_TimeoutWriter {
    HttpResponseWriter w;
    HttpHeader h;
    BytesBuffer *wbuf;
    HttpRequest *req;
    HttpHandler handler;
    SyncMutex mu; /* guards the four below */
    Error err;
    bool wrote_header;
    Int code;
    Chan *done; /* gets false when the handler returns and true when it panics */
    Any panic_value;
    burrow__PanicValue storage;
    Arena arena;
} sv_TimeoutWriter;

static HttpHeader sv_tw_header(void *self) {
    return ((sv_TimeoutWriter *)self)->h;
}

static void sv_tw_write_header_locked(sv_TimeoutWriter *tw, Int code) {
    sv_check_write_header_code(code);
    if (BURROW_FAILED(tw->err))
        return;
    if (tw->wrote_header) {
        if (tw->req != NULL) {
            Any v =
                context_value(http_request_context(tw->req), http_server_context_key);
            HttpServer *s = v.t == TYPE_HTTP_SERVER ? (HttpServer *)v.data : NULL;
            RuntimeFrame f = sv_relevant_caller();
            log_logger_printf_v(s != NULL ? s->error_log : NULL,
                                "http: superfluous response.WriteHeader call from %s "
                                "(%s:%d)",
                                f.function, path_base(f.file), f.line);
        }
        return;
    }
    tw->wrote_header = true;
    tw->code = code;
}

static Int sv_tw_write(void *self, Slice p, Error *err) {
    sv_TimeoutWriter *tw = (sv_TimeoutWriter *)self;
    sync_mutex_lock(&tw->mu);
    Int n = 0;
    if (BURROW_FAILED(tw->err)) {
        *err = tw->err;
    } else {
        if (!tw->wrote_header)
            sv_tw_write_header_locked(tw, HTTP_STATUS_OK);
        n = bytes_buffer_write(tw->wbuf, p, err);
    }
    sync_mutex_unlock(&tw->mu);
    return n;
}

static void sv_tw_write_header(void *self, Int code) {
    sv_TimeoutWriter *tw = (sv_TimeoutWriter *)self;
    sync_mutex_lock(&tw->mu);
    sv_tw_write_header_locked(tw, code);
    sync_mutex_unlock(&tw->mu);
}

static const Type sv_timeout_writer_desc = {
    {(const Byte *)"timeoutWriter", 13},
    {(const Byte *)"net/http", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(sv_TimeoutWriter),
    (uint16_t)_Alignof(sv_TimeoutWriter),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6874776eU, /* "htwn" */
    NULL,
};

static const HttpResponseWriterVT sv_timeout_writer_vt = {
    {&sv_timeout_writer_desc, sv_tw_write},
    sv_tw_header,
    sv_tw_write_header,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

static bool sv_tw_call(sv_TimeoutWriter *tw) {
    volatile bool panicked = false;
    HttpResponseWriter w = {&sv_timeout_writer_vt, tw};
    BURROW_TRY {
        http_handler_serve_http(tw->handler, w, tw->req);
    }
    BURROW_CATCH(p) {
        tw->panic_value = sv_keep(&tw->storage, p);
        panicked = true;
    }
    BURROW_TRY_END;
    return panicked;
}

static void sv_tw_run(void *env) {
    sv_TimeoutWriter *tw = (sv_TimeoutWriter *)env;
    bool panicked = sv_tw_call(tw);
    chan_send(tw->done, &panicked);
}

static void sv_tw_free(sv_TimeoutWriter *tw, Context ctx) {
    if (ctx.vt != NULL)
        context_release(ctx);
    if (tw->req != NULL)
        http_request_free(tw->req);
    if (tw->done != NULL)
        chan_free(tw->done);
    arena_free(&tw->arena);
    mem_free(heap_allocator(), tw, sizeof *tw, _Alignof(sv_TimeoutWriter));
}

static void sv_timeout_handler_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    const sv_TimeoutHandler *h = (const sv_TimeoutHandler *)self;
    sv_TimeoutWriter *tw = (sv_TimeoutWriter *)mem_alloc(heap_allocator(), sizeof *tw,
                                                         _Alignof(sv_TimeoutWriter));
    if (tw == NULL) {
        http_error(w, BURROW_S("out of memory"), HTTP_STATUS_SERVICE_UNAVAILABLE);
        return;
    }
    memset(tw, 0, sizeof *tw);
    arena_init(&tw->arena, heap_allocator(), 0);
    Alloc *ta = arena_allocator(&tw->arena);
    ContextCancelFunc cancel = {NULL, NULL};
    Context ctx = context_with_timeout(ta, http_request_context(r), h->dt, &cancel);
    tw->w = w;
    tw->handler = h->handler;
    tw->h = http_header_make(ta);
    tw->wbuf = bytes_new_buffer(ta, slice_from(NULL, 0, 0, TYPE_BYTE));
    tw->done = chan_make(ta, TYPE_BOOL, 1);
    if (ctx.vt != NULL)
        tw->req = http_request_with_context(r, ta, ctx);
    if (ctx.vt == NULL || tw->h == NULL || tw->wbuf == NULL || tw->done == NULL ||
        tw->req == NULL) {
        sv_tw_free(tw, ctx);
        http_error(w, BURROW_S("out of memory"), HTTP_STATUS_SERVICE_UNAVAILABLE);
        return;
    }
    if (!go(BURROW_FN(Func, sv_tw_run, tw)))
        sv_tw_run(tw);

    bool panicked = false;
    SelectCase cases[] = {
        BURROW_RECV(tw->done, &panicked),
        BURROW_RECV(context_done(ctx), NULL),
    };
    bool finished = chan_select(cases, 2) == 0;
    sync_mutex_lock(&tw->mu);
    if (finished && !panicked) {
        /* Go's dst[k] = vv shares the slices. Here they are in tw's arena,
         * which goes before the response is written out, so they are copied
         * into dst's own allocator. */
        HttpHeader dst = http_response_writer_header(w);
        Alloc *da = burrow__map_allocator(dst);
        MapIter it = map_iter(tw->h);
        const void *k;
        void *v;
        while (map_next(&it, &k, &v)) {
            const Slice *vv = (const Slice *)v;
            Str *cp = NULL;
            if (vv->len > 0) {
                cp = (Str *)mem_alloc(da, (size_t)vv->len * sizeof(Str), _Alignof(Str));
                if (cp == NULL)
                    continue;
                for (Int i = 0; i < vv->len; i++)
                    cp[i] = str_clone(da, ((const Str *)vv->p)[i]);
            }
            Str key = str_clone(da, *(const Str *)k);
            Slice vals = slice_from(cp, vv->len, vv->len, TYPE_STRING);
            (void)map_set(dst, &key, &vals);
        }
        if (!tw->wrote_header)
            tw->code = HTTP_STATUS_OK;
        http_response_writer_write_header(w, tw->code);
        Error err;
        (void)http_response_writer_write(w, bytes_buffer_bytes(tw->wbuf), &err);
    } else if (!finished) {
        Error err = context_err(ctx);
        http_response_writer_write_header(w, HTTP_STATUS_SERVICE_UNAVAILABLE);
        if (errors_is(err, context_deadline_exceeded)) {
            Str body = h->body.len > 0
                           ? h->body
                           : BURROW_S("<html><head><title>Timeout</title></head><body>"
                                      "<h1>Timeout</h1></body></html>");
            Error werr;
            (void)http_response_writer_write(
                w, slice_from((void *)(uintptr_t)body.p, body.len, body.len, TYPE_BYTE),
                &werr);
            tw->err = http_err_handler_timeout;
        } else {
            tw->err = err;
        }
    }
    sync_mutex_unlock(&tw->mu);
    BURROW_CALLF0(cancel);

    /* The request h->handler has is freed here, so it has to be done with
     * it. */
    if (!finished)
        (void)chan_recv(tw->done, &panicked);
    burrow__PanicValue storage;
    Any pv = panicked ? sv_keep(&storage, tw->panic_value) : (Any){NULL, NULL};
    sv_tw_free(tw, ctx);
    if (panicked && finished)
        panic(pv);
}

static const Type sv_timeout_handler_desc = {
    {(const Byte *)"timeoutHandler", 14},
    {(const Byte *)"net/http", 8},
    KIND_STRUCT,
    (uint32_t)sizeof(sv_TimeoutHandler),
    (uint16_t)_Alignof(sv_TimeoutHandler),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6874686eU, /* "hthn" */
    NULL,
};

static const HttpHandlerVT sv_timeout_handler_vt = {&sv_timeout_handler_desc,
                                                    sv_timeout_handler_serve};

HttpHandler http_timeout_handler(Alloc *a, HttpHandler h, Duration dt, Str msg) {
    sv_TimeoutHandler *th =
        (sv_TimeoutHandler *)mem_alloc(a, sizeof *th, _Alignof(sv_TimeoutHandler));
    if (th == NULL)
        return (HttpHandler){&sv_timeout_handler_vt, NULL};
    th->handler = h;
    th->body = msg;
    th->dt = dt;
    return (HttpHandler){&sv_timeout_handler_vt, th};
}

/* -------------------------------------------------------- MaxBytesReader */

static Str sv_max_bytes_error_message(const void *self) {
    (void)self;
    return BURROW_S("http: request body too large");
}

static const ErrorVT sv_max_bytes_error_vt = {
    &sv_max_bytes_error_desc, sv_max_bytes_error_message, NULL, NULL, NULL, NULL, NULL,
};

typedef struct sv_MaxBytesReader {
    HttpResponseWriter w;
    IoReadCloser r;
    int64_t i; /* max bytes initially, for MaxBytesError */
    int64_t n; /* max bytes remaining */
    Error err; /* sticky error */
    HttpMaxBytesError e;
} sv_MaxBytesReader;

static Int sv_mbr_read(void *self, Slice p, Error *err) {
    sv_MaxBytesReader *l = (sv_MaxBytesReader *)self;
    if (BURROW_FAILED(l->err)) {
        *err = l->err;
        return 0;
    }
    *err = BURROW_NO_ERROR;
    if (p.len == 0)
        return 0;
    /* If they asked for a 32KB byte read but only 5 bytes are remaining, no
     * need to read 32KB. 6 bytes will answer the question of whether we hit
     * the limit or go past it. */
    if ((int64_t)p.len - 1 > l->n)
        p.len = (Int)(l->n + 1);
    Int n = l->r.vt->reader.read(l->r.data, p, err);

    if ((int64_t)n <= l->n) {
        l->n -= (int64_t)n;
        l->err = *err;
        return n;
    }

    n = (Int)l->n;
    l->n = 0;

    /* The server code and client code both use maxBytesReader. This
     * "requestTooLarge" check is only used by the server code. To prevent
     * binaries which only using the HTTP Client code (such as cmd/go) from
     * also linking in the HTTP server, don't use a static type assertion to
     * the server "*response" type. Check this interface instead. */
    if (l->w.vt == &sv_response_vt)
        sv_request_too_large((sv_Response *)l->w.data);
    l->e.limit = l->i;
    l->err = (Error){&sv_max_bytes_error_vt, &l->e};
    *err = l->err;
    return n;
}

static Error sv_mbr_close(void *self) {
    sv_MaxBytesReader *l = (sv_MaxBytesReader *)self;
    return l->r.vt->closer.close(l->r.data);
}

static const IoReadCloserVT sv_mbr_vt = {{NULL, sv_mbr_read}, {NULL, sv_mbr_close}};

IoReadCloser http_max_bytes_reader(Alloc *a, HttpResponseWriter w, IoReadCloser r,
                                   int64_t n) {
    if (n < 0) /* Treat negative limits as equivalent to 0. */
        n = 0;
    sv_MaxBytesReader *l =
        (sv_MaxBytesReader *)mem_alloc(a, sizeof *l, _Alignof(sv_MaxBytesReader));
    if (l == NULL)
        return (IoReadCloser){NULL, NULL};
    memset(l, 0, sizeof *l);
    l->w = w;
    l->r = r;
    l->i = n;
    l->n = n;
    return (IoReadCloser){&sv_mbr_vt, l};
}

typedef struct sv_MaxBytesHandler {
    HttpHandler h;
    int64_t n;
} sv_MaxBytesHandler;

static void sv_arena_free(void *ar) {
    arena_free((Arena *)ar);
}

/* Runs fn(self, w, r, a) with a the request's arena, or a scratch one for a
 * request that has none, such as the copy a wrapping handler made. */
typedef void (*sv_ServeIn)(const void *self, HttpResponseWriter w, HttpRequest *r,
                           Alloc *a);

static void sv_serve_with_arena(sv_ServeIn fn, const void *self, HttpResponseWriter w,
                                HttpRequest *r) {
    if (r->a != NULL) {
        fn(self, w, r, arena_allocator(&r->arena));
        return;
    }
    Arena scratch;
    arena_init(&scratch, NULL, 0);
    BURROW_SCOPE {
        BURROW_DEFER(sv_arena_free, &scratch);
        fn(self, w, r, arena_allocator(&scratch));
    }
    BURROW_SCOPE_END;
}

static void sv_max_bytes_serve_in(const void *self, HttpResponseWriter w,
                                  HttpRequest *r, Alloc *a) {
    const sv_MaxBytesHandler *mh = (const sv_MaxBytesHandler *)self;
    HttpRequest r2 = *r;
    r2.body = http_max_bytes_reader(a, w, r->body, mh->n);
    if (r2.body.vt == NULL) {
        http_error(w, BURROW_S("out of memory"), HTTP_STATUS_SERVICE_UNAVAILABLE);
        return;
    }
    /* The copy owns nothing of r's. */
    r2.a = NULL;
    arena_init(&r2.arena, NULL, 0);
    r2.wire = NULL;
    http_handler_serve_http(mh->h, w, &r2);
}

static void sv_max_bytes_handler_serve(void *self, HttpResponseWriter w,
                                       HttpRequest *r) {
    sv_serve_with_arena(sv_max_bytes_serve_in, self, w, r);
}

static const Type sv_max_bytes_handler_desc = {
    {(const Byte *)"HandlerFunc", 11},
    {(const Byte *)"net/http", 8},
    KIND_FUNC,
    (uint32_t)sizeof(sv_MaxBytesHandler),
    (uint16_t)_Alignof(sv_MaxBytesHandler),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x686d6268U, /* "hmbh" */
    NULL,
};

static const HttpHandlerVT sv_max_bytes_handler_vt = {&sv_max_bytes_handler_desc,
                                                      sv_max_bytes_handler_serve};

HttpHandler http_max_bytes_handler(Alloc *a, HttpHandler h, int64_t n) {
    sv_MaxBytesHandler *mh =
        (sv_MaxBytesHandler *)mem_alloc(a, sizeof *mh, _Alignof(sv_MaxBytesHandler));
    if (mh == NULL)
        return (HttpHandler){&sv_max_bytes_handler_vt, NULL};
    mh->h = h;
    mh->n = n;
    return (HttpHandler){&sv_max_bytes_handler_vt, mh};
}

/* -------------------------------------------------- AllowQuerySemicolons */

static void sv_allow_query_semicolons_in(const void *self, HttpResponseWriter w,
                                         HttpRequest *r, Alloc *a) {
    const HttpHandler *h = (const HttpHandler *)self;
    HttpRequest r2 = *r;
    Url u2 = *r->url;
    u2.raw_query =
        strings_replace_all(a, r->url->raw_query, BURROW_S(";"), BURROW_S("&"));
    /* The copies own nothing, so a mux further on uses an arena of its own
     * for r2 rather than a copy of r's. */
    u2.mem = NULL;
    u2.size = 0;
    r2.url = &u2;
    r2.a = NULL;
    arena_init(&r2.arena, NULL, 0);
    r2.wire = NULL;
    http_handler_serve_http(*h, w, &r2);
}

static void sv_allow_query_semicolons_serve(void *self, HttpResponseWriter w,
                                            HttpRequest *r) {
    const HttpHandler *h = (const HttpHandler *)self;
    if (r->url == NULL || strings_index_byte(r->url->raw_query, ';') < 0) {
        http_handler_serve_http(*h, w, r);
        return;
    }
    sv_serve_with_arena(sv_allow_query_semicolons_in, self, w, r);
}

static const Type sv_allow_query_semicolons_desc = {
    {(const Byte *)"HandlerFunc", 11},
    {(const Byte *)"net/http", 8},
    KIND_FUNC,
    (uint32_t)sizeof(HttpHandler),
    (uint16_t)_Alignof(HttpHandler),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x68617173U, /* "haqs" */
    NULL,
};

static const HttpHandlerVT sv_allow_query_semicolons_vt = {
    &sv_allow_query_semicolons_desc, sv_allow_query_semicolons_serve};

HttpHandler http_allow_query_semicolons(Alloc *a, HttpHandler h) {
    HttpHandler *p = (HttpHandler *)mem_alloc(a, sizeof *p, _Alignof(HttpHandler));
    if (p == NULL)
        return (HttpHandler){&sv_allow_query_semicolons_vt, NULL};
    *p = h;
    return (HttpHandler){&sv_allow_query_semicolons_vt, p};
}
