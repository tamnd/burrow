/* net/http/internal/http2's server.go: the server side of an HTTP/2
 * connection, and the glue in net/http's http2.go that hands it one.
 *
 * Go runs one serve goroutine per connection, which owns the connection's
 * state, and talks to it over channels: one goroutine reads frames, handlers
 * ask for frames to be written, and a frame that may block is written on a
 * goroutine of its own. The same goroutines are here, with the same channels.
 *
 * What Go leaves to the collector is counted here. A stream is held by the
 * connection's map, its handler, each write request on it and each timer and
 * message that names it, and goes when the last of them lets go. The
 * connection waits for its handlers, timers and the goroutines it started
 * before it frees anything. Errors that go from one goroutine to another are
 * copied into an arena the connection keeps until it is done.
 *
 * Server push is not here, since net/http's ResponseWriter has no Push in C,
 * but the server still keeps track of whether the client allows it.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http2.h"
#include "http_internal.h"

#include "../xnet/hpack.h"
#include "../xnet/httpguts.h"

#include "burrow/bufio.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/crypto/rand.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/mime/multipart.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/textproto.h"
#include "burrow/net/url.h"
#include "burrow/os.h"
#include "burrow/panic.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
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
    /* initialWindowSize and initialMaxFrameSize, from RFC 7540. */
    H2S_INITIAL_WINDOW_SIZE = 65535,
    H2S_INITIAL_MAX_FRAME_SIZE = 16384,
    /* handlerChunkWriteSize. */
    H2S_HANDLER_CHUNK_WRITE_SIZE = 4 << 10,
    /* defaultMaxStreams. */
    H2S_DEFAULT_MAX_STREAMS = 250,
    /* maxQueuedControlFrames. */
    H2S_MAX_QUEUED_CONTROL_FRAMES = 10000,
    /* defaultMaxReadFrameSize. */
    H2S_DEFAULT_MAX_READ_FRAME_SIZE = 1 << 20,
    /* bufWriterPoolBufferSize. */
    H2S_BUF_WRITER_SIZE = 4 << 10,
    /* DefaultMaxHeaderBytes. */
    H2S_DEFAULT_MAX_HEADER_BYTES = 1 << 20,
};

#define H2S_PREFACE_TIMEOUT (10 * TIME_SECOND)
#define H2S_FIRST_SETTINGS_TIMEOUT (2 * TIME_SECOND)
#define H2S_GO_AWAY_TIMEOUT (1 * TIME_SECOND)
#define H2S_DEFAULT_PING_TIMEOUT (15 * TIME_SECOND)

BURROW_SENTINEL_ERROR(burrow__http2_err_client_disconnected, "client disconnected");
BURROW_SENTINEL_ERROR(burrow__http2_err_closed_body, "body closed by handler");
BURROW_SENTINEL_ERROR(burrow__http2_err_handler_complete,
                      "http2: request body closed due to handler exiting");
BURROW_SENTINEL_ERROR(burrow__http2_err_stream_closed, "http2: stream closed");
BURROW_SENTINEL_ERROR(burrow__http2_err_preface_timeout,
                      "timeout waiting for client preface");
BURROW_SENTINEL_ERROR(burrow__http2_err_handler_panicked, "http2: handler panicked");
BURROW_SENTINEL_ERROR(burrow__http2_err_handler_wrote_too_much,
                      "http2: handler wrote more than declared Content-Length");

/* ---------------------------------------------------------------- config */

/* Config, as configFromServer leaves it. */
typedef struct h2s_Config {
    int64_t max_concurrent_streams;
    int64_t max_decoder_header_table_size;
    int64_t max_encoder_header_table_size;
    int64_t max_read_frame_size;
    int64_t max_receive_buffer_per_connection;
    int64_t max_receive_buffer_per_stream;
    Duration send_ping_timeout;
    Duration ping_timeout;
    Duration write_byte_timeout;
    HttpCountErrorFunc count_error;
} h2s_Config;

static void h2s_set_default(int64_t *v, int64_t minval, int64_t maxval,
                            int64_t defval) {
    if (*v < minval || *v > maxval)
        *v = defval;
}

/* configFromServer: what the server's HTTP2 field sets, and the defaults for
 * the rest. */
static h2s_Config h2s_config_from_server(const HttpServer *s) {
    h2s_Config conf;
    memset(&conf, 0, sizeof conf);
    const HttpHTTP2Config *h2 = s->http2;
    if (h2 != NULL) {
        conf.max_concurrent_streams = (int64_t)h2->max_concurrent_streams;
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
    h2s_set_default(&conf.max_concurrent_streams, 1, INT32_MAX,
                    H2S_DEFAULT_MAX_STREAMS);
    h2s_set_default(&conf.max_encoder_header_table_size, 1, INT32_MAX,
                    HTTP2_INITIAL_HEADER_TABLE_SIZE);
    h2s_set_default(&conf.max_decoder_header_table_size, 1, INT32_MAX,
                    HTTP2_INITIAL_HEADER_TABLE_SIZE);
    h2s_set_default(&conf.max_receive_buffer_per_connection, H2S_INITIAL_WINDOW_SIZE,
                    INT32_MAX, 1 << 20);
    h2s_set_default(&conf.max_receive_buffer_per_stream, 1, INT32_MAX, 1 << 20);
    h2s_set_default(&conf.max_read_frame_size, HTTP2_MIN_MAX_FRAME_SIZE,
                    HTTP2_MAX_FRAME_SIZE, H2S_DEFAULT_MAX_READ_FRAME_SIZE);
    h2s_set_default(&conf.ping_timeout, 1, INT64_MAX, H2S_DEFAULT_PING_TIMEOUT);
    return conf;
}

/* -------------------------------------------------------------- the types */

typedef struct burrow__Http2ServeConn h2s_Conn;
typedef struct h2s_Stream h2s_Stream;

/* streamState. */
typedef enum h2s_StreamState {
    H2S_STATE_IDLE,
    H2S_STATE_OPEN,
    H2S_STATE_HALF_CLOSED_LOCAL,
    H2S_STATE_HALF_CLOSED_REMOTE,
    H2S_STATE_CLOSED,
} h2s_StreamState;

/* Which handler a stream runs: the server's, or one of the two that answer a
 * request the server could not take. */
typedef enum h2s_HandlerKind {
    H2S_HANDLER_SERVER,
    H2S_HANDLER_431,
    H2S_HANDLER_400,
} h2s_HandlerKind;

/* bufferedWriter, which the Framer writes to. Go takes the bufio.Writer from
 * a pool while there is something in it, and here the connection keeps one,
 * with active for whether Go would have it. */
typedef struct h2s_BufWriter {
    h2s_Conn *sc;
    NetConn conn;
    BufioWriter *bw;
    bool active;
    Duration byte_timeout;
    Error werr;
} h2s_BufWriter;

/* responseWriterState. It lives as long as its stream, so that nothing the
 * serve goroutine still holds can point at memory the handler gave back. */
typedef struct h2s_Rws {
    h2s_Stream *stream;
    HttpRequest *req;
    h2s_Conn *conn;
    BufioWriter *bw; /* in front of the chunk writer */
    HttpHeader handler_header;
    HttpHeader snap_header;
    Str *trailers;
    Int ntrailers;
    Int trailers_cap;
    Int status;
    bool wrote_header;
    bool sent_header;
    bool handler_done;
    int64_t sent_content_len;
    int64_t wrote_bytes;
    SyncMutex close_notifier_mu;
    Chan *close_notifier_ch;
    /* The channel a write waits on for its result, which Go takes from a
     * pool. One that a wait gave up on may still be sent to, so it is put
     * aside and a new one made. */
    Chan *errc; /* NULL when a new one could not be made */
    Chan **old_errc;
    Int nold_errc;
    Int old_errc_cap;
    Arena arena;
} h2s_Rws;

/* responseWriter, which is only a pointer to the state, so that the methods
 * can tell when the handler has finished. */
typedef struct h2s_Rw {
    h2s_Rws *rws;
} h2s_Rw;

/* stream. */
/* requestBody, the handler's Request.Body. Read and Close may be called
 * concurrently. */
typedef struct h2s_ReqBody {
    struct h2s_Stream *stream;
    SyncAtomicBool closed; /* for Close only */
    bool saw_eof;          /* for Read only */
    bool has_pipe;         /* there is an HTTP entity message body */
    bool needs_continue;   /* need to send a 100-continue */
} h2s_ReqBody;

struct h2s_Stream {
    Http2Stream base; /* first, for the scheduler */
    h2s_Conn *sc;
    SyncAtomicInt32 refs;
    Context ctx;
    ContextCancelFunc cancel_ctx;
    Http2Pipe pipe;
    Http2DataBuffer dbuf;
    bool has_body; /* Go's body != nil */
    Chan *cw;      /* closed when the stream closes */
    int64_t body_bytes;
    int64_t decl_body_bytes;
    Http2Inflow inflow;
    h2s_StreamState state;
    bool reset_queued;
    bool got_trailer_header;
    bool wrote_headers;
    TimeTimer *read_deadline;
    TimeTimer *write_deadline;
    bool read_deadline_live; /* Go's readDeadline != nil */
    bool write_deadline_live;
    Error close_err;
    HttpHeader trailer;
    HttpHeader req_trailer;
    HttpRequest *req;
    h2s_Rws *rws;
    h2s_Rw rw;
    h2s_ReqBody body;
    h2s_HandlerKind handler_kind;
    Str handler_msg;
    Arena arena;
};

/* readFrameResult. The reader waits on gate before it reads again, since the
 * frame's bytes are in the Framer's buffer. */
typedef struct h2s_ReadFrameResult {
    Http2Frame *f;
    Error err;
} h2s_ReadFrameResult;

/* frameWriteResult. */
typedef struct h2s_FrameWriteResult {
    Http2FrameWriteRequest wr;
    Error err;
} h2s_FrameWriteResult;

/* bodyReadMsg. */
typedef struct h2s_BodyReadMsg {
    h2s_Stream *st;
    Int n;
} h2s_BodyReadMsg;

/* The messages on serveMsgCh. Go sends values of several types, and the
 * functions the response writer sends for its deadlines are the last two
 * kinds, with the stream and the deadline. */
typedef enum h2s_MsgKind {
    H2S_MSG_SETTINGS_TIMER,
    H2S_MSG_IDLE_TIMER,
    H2S_MSG_READ_IDLE_TIMER,
    H2S_MSG_SHUTDOWN_TIMER,
    H2S_MSG_GRACEFUL,
    H2S_MSG_HANDLER_DONE,
    H2S_MSG_READ_DEADLINE,
    H2S_MSG_WRITE_DEADLINE,
} h2s_MsgKind;

typedef struct h2s_Msg {
    h2s_MsgKind kind;
    h2s_Stream *st;
    Time deadline;
} h2s_Msg;

/* unstartedHandler. */
typedef struct h2s_Unstarted {
    h2s_Stream *st;
} h2s_Unstarted;

/* A connection timer, made once and armed again as Go's AfterFunc timers
 * are. */
typedef struct h2s_Timer {
    TimeTimer *t;
    h2s_Conn *sc;
    h2s_MsgKind kind;
} h2s_Timer;

/* res headers handed to the writer, deep enough that the handler may change
 * its header after. */
typedef struct h2s_ResHeaders {
    Http2WriteResHeaders rh;
    Arena arena;
} h2s_ResHeaders;

/* serverConn. */
struct burrow__Http2ServeConn {
    Http2ServerConn base; /* first, for the scheduler */
    HttpServer *srv;
    Alloc *a;
    NetConn conn;
    Str remote_addr;
    Context base_ctx;
    ContextCancelFunc cancel_ctx;
    h2s_Config conf;
    h2s_BufWriter bw;
    Http2Framer *framer;
    HpackDecoder *hdec;
    Chan *done_serving;        /* closed when serve returns */
    Chan *read_frame_ch;       /* of h2s_ReadFrameResult */
    Chan *read_gate;           /* of bool */
    Chan *want_write_frame_ch; /* of Http2FrameWriteRequest */
    Chan *wrote_frame_ch;      /* of h2s_FrameWriteResult */
    Chan *body_read_ch;        /* of h2s_BodyReadMsg */
    Chan *serve_msg_ch;        /* of h2s_Msg */
    Chan *preface_ch;          /* of Error */
    Http2Outflow flow;
    Http2Inflow inflow;
    bool saw_client_preface;
    Http2WriteScheduler write_sched;

    /* Everything from here on is the serve goroutine's. */
    bool in_go_away;
    bool need_to_send_go_away;
    Http2ErrCode go_away_code;
    bool push_enabled;
    bool priority_aware;
    bool has_intermediary;
    bool need_to_send_settings_ack;
    bool saw_first_settings;
    int64_t unacked_settings;
    int64_t queued_control_frames;
    uint32_t client_max_streams;
    uint32_t adv_max_streams;
    uint32_t cur_client_streams;
    uint32_t cur_pushed_streams;
    uint32_t cur_handlers;
    uint32_t max_client_stream_id;
    uint32_t max_pushed_stream_id;
    int32_t initial_stream_send_window_size;
    int32_t initial_stream_recv_window_size;
    uint32_t peer_max_header_list_size;
    Map *streams; /* uint32 to h2s_Stream* */
    h2s_Unstarted *unstarted;
    Int nunstarted;
    Int unstarted_cap;
    bool writing_frame;
    bool writing_frame_async;
    bool needs_frame_flush;
    bool in_frame_schedule_loop;
    bool ping_sent;
    Byte sent_ping_data[8];
    Duration read_idle_timeout;
    Time last_frame_time;
    /* A HEADER_TABLE_SIZE that came while the encoder was busy on another
     * goroutine, for when it is done. */
    bool pending_table_size;
    uint32_t pending_table_size_v;
    BytesBuffer header_write_buf;
    HpackEncoder *hpack_encoder;
    Http2Setting settings[8];
    Int nsettings;
    h2s_Timer idle_timer;
    h2s_Timer read_idle_timer;
    h2s_Timer settings_timer;
    h2s_Timer shutdown_timer;

    SyncAtomicBool shutdown_once;
    /* Handlers, armed timers and their callbacks, and other goroutines that
     * may touch the connection. */
    SyncWaitGroup wg;
    /* The reader, the asynchronous writer and the preface reader, which end
     * once the connection is closed. */
    SyncWaitGroup io_wg;
    SyncMutex emu; /* guards earena */
    Arena earena;
    struct burrow__Http2ServeConn *prev;
    struct burrow__Http2ServeConn *next;
};

/* The element types of the channels. */
static const Type h2s_read_frame_result_desc = {
    {(const Byte *)"readFrameResult", 15},
    {(const Byte *)"net/http/internal/http2", 23},
    KIND_STRUCT,
    (uint32_t)sizeof(h2s_ReadFrameResult),
    (uint16_t)_Alignof(h2s_ReadFrameResult),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6b1f3a21U,
    NULL};

static const Type h2s_frame_write_request_desc = {
    {(const Byte *)"FrameWriteRequest", 17},
    {(const Byte *)"net/http/internal/http2", 23},
    KIND_STRUCT,
    (uint32_t)sizeof(Http2FrameWriteRequest),
    (uint16_t)_Alignof(Http2FrameWriteRequest),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x2c9e5d47U,
    NULL};

static const Type h2s_frame_write_result_desc = {
    {(const Byte *)"frameWriteResult", 16},
    {(const Byte *)"net/http/internal/http2", 23},
    KIND_STRUCT,
    (uint32_t)sizeof(h2s_FrameWriteResult),
    (uint16_t)_Alignof(h2s_FrameWriteResult),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x51d8a0c3U,
    NULL};

static const Type h2s_body_read_msg_desc = {
    {(const Byte *)"bodyReadMsg", 11},
    {(const Byte *)"net/http/internal/http2", 23},
    KIND_STRUCT,
    (uint32_t)sizeof(h2s_BodyReadMsg),
    (uint16_t)_Alignof(h2s_BodyReadMsg),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x0f73b96eU,
    NULL};

static const Type h2s_msg_desc = {{(const Byte *)"serverMessage", 13},
                                  {(const Byte *)"net/http/internal/http2", 23},
                                  KIND_STRUCT,
                                  (uint32_t)sizeof(h2s_Msg),
                                  (uint16_t)_Alignof(h2s_Msg),
                                  0,
                                  0,
                                  NULL,
                                  NULL,
                                  NULL,
                                  NULL,
                                  0,
                                  0x9a4e17b5U,
                                  NULL};

/* --------------------------------------------------------------- helpers */

#define h2s_logf(sc, ...) log_logger_printf_v((sc)->srv->error_log, __VA_ARGS__)
#define h2s_vlogf(sc, ...)                                                             \
    do {                                                                               \
        if (burrow__http2_verbose_logs())                                              \
            h2s_logf(sc, __VA_ARGS__);                                                 \
    } while (0)

static Time h2s_time_zero(void) {
    Time t;
    memset(&t, 0, sizeof t);
    return t;
}

/* A copy of err that lasts as long as the connection, for an error that goes
 * from one goroutine to another. */
static Error h2s_keep_err(h2s_Conn *sc, Error err) {
    if (BURROW_OK(err))
        return err;
    sync_mutex_lock(&sc->emu);
    Error kept = error_retain(arena_allocator(&sc->earena), err);
    sync_mutex_unlock(&sc->emu);
    return kept;
}

/* isClosedConnError. */
static bool h2s_is_closed_conn_error(Error err) {
    if (BURROW_OK(err))
        return false;
    if (errors_is(err, net_err_closed))
        return true;
    return strings_contains(error_text(err),
                            BURROW_S("use of closed network connection"));
}

/* condlogf. */
static bool h2s_quiet_error(Error err) {
    return errors_is(err, io_eof) || errors_is(err, io_err_unexpected_eof) ||
           h2s_is_closed_conn_error(err) ||
           errors_is(err, burrow__http2_err_preface_timeout);
}

/* streamError. */
static Error h2s_stream_error(uint32_t id, Http2ErrCode code) {
    return burrow__http2_stream_error(id, code, BURROW_NO_ERROR);
}

/* countError. */
static Error h2s_count_error(h2s_Conn *sc, const char *name, Error err) {
    if (BURROW_FUNC_IS_NIL(sc->conf.count_error))
        return err;
    const char *typ;
    Http2ErrCode code;
    Http2StreamError se;
    if (burrow__http2_error_connection(err, &code)) {
        typ = "conn";
    } else if (burrow__http2_error_stream(err, &se)) {
        typ = "stream";
        code = se.code;
    } else {
        return err;
    }
    Byte cb[HTTP2_STRING_MAX];
    Str cs;
    if (code <= HTTP2_ERR_CODE_HTTP_1_1_REQUIRED) {
        cs = burrow__http2_err_code_string_token(code, cb);
    } else {
        int n = snprintf((char *)cb, sizeof cb, "%u", (unsigned)code);
        cs = str_from_bytes(cb, n);
    }
    char out[128];
    int n = snprintf(out, sizeof out, "%s_%.*s_%s", typ, (int)cs.len,
                     (const char *)cs.p, name);
    if (n < 0)
        return err;
    if ((size_t)n >= sizeof out)
        n = (int)sizeof out - 1;
    BURROW_CALLF(sc->conf.count_error, str_from_bytes(out, n));
    return err;
}

/* A ConnectionError, counted. */
static Error h2s_conn_err(h2s_Conn *sc, const char *name, Http2ErrCode code) {
    return h2s_count_error(sc, name, burrow__http2_connection_error(code));
}

/* A StreamError, counted. */
static Error h2s_stream_err(h2s_Conn *sc, const char *name, uint32_t id,
                            Http2ErrCode code) {
    return h2s_count_error(sc, name, h2s_stream_error(id, code));
}

/* -------------------------------------------------------- bufferedWriter */

/* bufferedWriterTimeoutWriter.Write, which is writeWithByteTimeout. */
static Int h2s_timeout_write(void *self, Slice p, Error *err) {
    h2s_BufWriter *w = (h2s_BufWriter *)self;
    NetConn c = w->conn;
    if (w->byte_timeout <= 0) {
        Error e = BURROW_NO_ERROR;
        Int n = c.vt->writer.write(c.data, p, &e);
        *err = h2s_keep_err(w->sc, e);
        return n;
    }
    Int n = 0;
    for (;;) {
        ArenaMark m = error_mark();
        (void)c.vt->set_write_deadline(c.data, time_add(time_now(), w->byte_timeout));
        Error e = BURROW_NO_ERROR;
        Int nn = c.vt->writer.write(c.data, slice_sub(p, n, p.len), &e);
        n += nn;
        if (n == p.len || nn == 0 || !errors_is(e, os_err_deadline_exceeded)) {
            (void)c.vt->set_write_deadline(c.data, h2s_time_zero());
            *err = h2s_keep_err(w->sc, e);
            return n;
        }
        error_release(m);
    }
}

static const IoWriterVT h2s_timeout_writer_vt = {NULL, h2s_timeout_write};

static Int h2s_bw_available(const h2s_BufWriter *w) {
    if (!w->active)
        return H2S_BUF_WRITER_SIZE;
    return bufio_writer_available(w->bw);
}

static Int h2s_bw_write(void *self, Slice p, Error *err) {
    h2s_BufWriter *w = (h2s_BufWriter *)self;
    if (BURROW_FAILED(w->werr)) {
        *err = w->werr;
        return 0;
    }
    if (!w->active) {
        IoWriter tw = {&h2s_timeout_writer_vt, w};
        bufio_writer_reset(w->bw, tw);
        w->active = true;
    }
    Error e = BURROW_NO_ERROR;
    Int n = bufio_writer_write(w->bw, p, &e);
    w->werr = e;
    *err = e;
    return n;
}

static const IoWriterVT h2s_bw_vt = {NULL, h2s_bw_write};

static Error h2s_bw_flush(h2s_BufWriter *w) {
    if (!w->active)
        return BURROW_NO_ERROR;
    if (BURROW_FAILED(w->werr))
        return w->werr;
    w->werr = bufio_writer_flush(w->bw);
    IoWriter tw = {&h2s_timeout_writer_vt, w};
    bufio_writer_reset(w->bw, tw);
    w->active = false;
    return w->werr;
}

/* ---------------------------------------------------------- writeContext */

static Http2Framer *h2s_wc_framer(void *self) {
    return ((h2s_Conn *)self)->framer;
}

static Error h2s_wc_flush(void *self) {
    return h2s_bw_flush(&((h2s_Conn *)self)->bw);
}

static Error h2s_wc_close_conn(void *self) {
    NetConn c = ((h2s_Conn *)self)->conn;
    return c.vt->closer.close(c.data);
}

static HpackEncoder *h2s_wc_header_encoder(void *self, BytesBuffer **buf) {
    h2s_Conn *sc = (h2s_Conn *)self;
    *buf = &sc->header_write_buf;
    return sc->hpack_encoder;
}

static const Http2WriteContextVT h2s_write_context_vt = {
    h2s_wc_framer, h2s_wc_flush, h2s_wc_close_conn, h2s_wc_header_encoder};

static Http2WriteContext h2s_write_context(h2s_Conn *sc) {
    Http2WriteContext wc = {&h2s_write_context_vt, sc};
    return wc;
}

/* --------------------------------------------------------------- streams */

static void h2s_rws_free(h2s_Rws *rws, Alloc *a);

static void h2s_stream_ref(h2s_Stream *st) {
    (void)sync_atomic_int32_add(&st->refs, 1);
}

static void h2s_stream_free(h2s_Stream *st) {
    Alloc *a = st->sc->a;
    if (st->rws != NULL)
        h2s_rws_free(st->rws, a);
    time_timer_free(st->read_deadline);
    time_timer_free(st->write_deadline);
    burrow__http2_pipe_free(&st->pipe);
    burrow__http2_data_buffer_free(&st->dbuf);
    chan_free(st->cw);
    if (!BURROW_FUNC_IS_NIL(st->cancel_ctx))
        BURROW_CALLF0(st->cancel_ctx);
    http_request_free(st->req);
    if (st->ctx.vt != NULL)
        context_release(st->ctx);
    arena_free(&st->arena);
    mem_free(a, st, sizeof *st, _Alignof(h2s_Stream));
}

static void h2s_stream_unref(h2s_Stream *st) {
    if (sync_atomic_int32_add(&st->refs, -1) == 0)
        h2s_stream_free(st);
}

/* ---------------------------------------------------------- write requests */

static void h2s_res_headers_free(Alloc *a, Http2WriteResHeaders *rh) {
    h2s_ResHeaders *r = (h2s_ResHeaders *)(void *)rh;
    arena_free(&r->arena);
    mem_free(a, r, sizeof *r, _Alignof(h2s_ResHeaders));
}

/* A FrameWriteRequest, which holds st when there is one. */
static Http2FrameWriteRequest h2s_wr_make(Http2WriteFramer w, h2s_Stream *st,
                                          Chan *done) {
    Http2FrameWriteRequest wr;
    memset(&wr, 0, sizeof wr);
    wr.write = w;
    wr.stream = st != NULL ? &st->base : NULL;
    wr.done = done;
    if (st != NULL)
        h2s_stream_ref(st);
    return wr;
}

/* Lets go of what a request holds, once it is written or thrown away. */
static void h2s_wr_release(h2s_Conn *sc, Http2FrameWriteRequest *wr) {
    if (wr->write.kind == HTTP2_WRITE_RES_HEADERS && wr->write.u.res_headers != NULL) {
        h2s_res_headers_free(sc->a, wr->write.u.res_headers);
        wr->write.u.res_headers = NULL;
    }
    if (wr->stream != NULL) {
        h2s_Stream *st = (h2s_Stream *)(void *)wr->stream;
        wr->stream = NULL;
        h2s_stream_unref(st);
    }
}

static void h2s_sched_retain(void *ctx, const Http2FrameWriteRequest *wr) {
    (void)ctx;
    if (wr->stream != NULL)
        h2s_stream_ref((h2s_Stream *)(void *)wr->stream);
}

static void h2s_sched_drop(void *ctx, Http2FrameWriteRequest *wr) {
    h2s_wr_release((h2s_Conn *)ctx, wr);
}

/* --------------------------------------------------------- serve messages */

/* sendServeMsg. A message with a stream holds it, and lets go if the
 * connection is gone. */
static void h2s_send_serve_msg(h2s_Conn *sc, h2s_Msg m) {
    SelectCase cases[] = {
        BURROW_SEND(sc->serve_msg_ch, &m),
        BURROW_RECV(sc->done_serving, NULL),
    };
    if (chan_select(cases, 2) == 1 && m.st != NULL)
        h2s_stream_unref(m.st);
}

static h2s_Msg h2s_msg(h2s_MsgKind kind) {
    h2s_Msg m;
    memset(&m, 0, sizeof m);
    m.kind = kind;
    return m;
}

/* writeFrameFromHandler: hands wr to the serve goroutine, or lets go of it
 * when the connection is gone. */
static Error h2s_write_frame_from_handler(h2s_Conn *sc, Http2FrameWriteRequest wr) {
    SelectCase cases[] = {
        BURROW_SEND(sc->want_write_frame_ch, &wr),
        BURROW_RECV(sc->done_serving, NULL),
    };
    if (chan_select(cases, 2) == 0)
        return BURROW_NO_ERROR;
    h2s_wr_release(sc, &wr);
    return burrow__http2_err_client_disconnected;
}

/* ----------------------------------------------------------------- timers */

static void h2s_timer_fire(void *env) {
    h2s_Timer *t = (h2s_Timer *)env;
    h2s_Conn *sc = t->sc;
    h2s_send_serve_msg(sc, h2s_msg(t->kind));
    sync_wait_group_done(&sc->wg);
}

/* time.AfterFunc the first time and Reset after that. Each arming is counted
 * in wg until it fires or is stopped. */
static bool h2s_timer_arm(h2s_Conn *sc, h2s_Timer *t, Duration d) {
    if (t->t != NULL && time_timer_stop(t->t))
        sync_wait_group_done(&sc->wg);
    sync_wait_group_add(&sc->wg, 1);
    bool ok;
    if (t->t == NULL) {
        Func fn;
        fn.f = h2s_timer_fire;
        fn.env = t;
        t->t = time_after_func(sc->a, d, fn);
        ok = t->t != NULL;
    } else {
        ok = time_timer_reset(t->t, d, NULL);
    }
    if (!ok)
        sync_wait_group_done(&sc->wg);
    return ok;
}

static void h2s_timer_stop(h2s_Conn *sc, h2s_Timer *t) {
    if (t->t != NULL && time_timer_stop(t->t))
        sync_wait_group_done(&sc->wg);
}

/* onReadTimeout. */
static void h2s_on_read_timeout(h2s_Stream *st) {
    if (st->has_body) {
        /* Wrap the ErrDeadlineExceeded to avoid callers depending on us
         * returning the bare error. */
        Error e = fmt_errorf_v("%w", os_err_deadline_exceeded);
        burrow__http2_pipe_close_with_error(&st->pipe, e);
    }
}

/* onWriteTimeout. */
static void h2s_on_write_timeout(h2s_Stream *st) {
    Http2StreamError se;
    se.stream_id = st->base.id;
    se.code = HTTP2_ERR_CODE_INTERNAL;
    se.cause = os_err_deadline_exceeded;
    (void)h2s_write_frame_from_handler(
        st->sc, h2s_wr_make(burrow__http2_write_stream_error(se), NULL, NULL));
}

static void h2s_read_deadline_fire(void *env) {
    h2s_Stream *st = (h2s_Stream *)env;
    h2s_Conn *sc = st->sc;
    h2s_on_read_timeout(st);
    h2s_stream_unref(st);
    sync_wait_group_done(&sc->wg);
}

static void h2s_write_deadline_fire(void *env) {
    h2s_Stream *st = (h2s_Stream *)env;
    h2s_Conn *sc = st->sc;
    h2s_on_write_timeout(st);
    h2s_stream_unref(st);
    sync_wait_group_done(&sc->wg);
}

/* Arms one of a stream's deadline timers, which holds the stream while it is
 * armed. */
static bool h2s_stream_timer_arm(h2s_Stream *st, TimeTimer **t, Duration d,
                                 void (*fire)(void *env)) {
    h2s_Conn *sc = st->sc;
    if (*t != NULL && time_timer_stop(*t)) {
        h2s_stream_unref(st);
        sync_wait_group_done(&sc->wg);
    }
    sync_wait_group_add(&sc->wg, 1);
    h2s_stream_ref(st);
    bool ok;
    if (*t == NULL) {
        Func fn;
        fn.f = fire;
        fn.env = st;
        *t = time_after_func(sc->a, d, fn);
        ok = *t != NULL;
    } else {
        ok = time_timer_reset(*t, d, NULL);
    }
    if (!ok) {
        h2s_stream_unref(st);
        sync_wait_group_done(&sc->wg);
    }
    return ok;
}

/* Stop, and whether the timer was armed. */
static bool h2s_stream_timer_stop(h2s_Stream *st, TimeTimer *t) {
    if (t == NULL || !time_timer_stop(t))
        return false;
    h2s_Conn *sc = st->sc;
    h2s_stream_unref(st);
    sync_wait_group_done(&sc->wg);
    return true;
}

/* Go's SetReadDeadline and SetWriteDeadline message, on the serve goroutine.
 * live is Go's timer != nil. */
static void h2s_stream_apply_deadline(h2s_Stream *st, TimeTimer **t, bool *live,
                                      Time deadline, void (*fire)(void *env)) {
    if (*live && !h2s_stream_timer_stop(st, *t))
        return; /* deadline already exceeded, or stream has been closed */
    if (time_is_zero(deadline)) {
        *live = false;
        return;
    }
    *live = h2s_stream_timer_arm(st, t, time_until(deadline), fire);
}

/* --------------------------------------------------- reading and writing */

static void h2s_reader_done(h2s_Conn *sc) {
    sync_wait_group_done(&sc->io_wg);
}

/* readFrames, on its own goroutine. Frame byte fields are only good until
 * the next read, so the serve goroutine hands the turn back on read_gate when
 * it is done with a frame, and frees the frame itself. */
static void h2s_read_frames(void *env) {
    h2s_Conn *sc = (h2s_Conn *)env;
    for (;;) {
        ArenaMark m = error_mark();
        Error err = BURROW_NO_ERROR;
        h2s_ReadFrameResult res;
        res.f = burrow__http2_framer_read_frame(sc->framer, &err);
        res.err = h2s_keep_err(sc, err);
        error_release(m);
        Http2StreamError se;
        bool terminal =
            BURROW_FAILED(res.err) && !burrow__http2_error_stream(res.err, &se);
        SelectCase send[] = {
            BURROW_SEND(sc->read_frame_ch, &res),
            BURROW_RECV(sc->done_serving, NULL),
        };
        if (chan_select(send, 2) == 1) {
            burrow__http2_frame_free(res.f);
            break;
        }
        SelectCase gate[] = {
            BURROW_RECV(sc->read_gate, NULL),
            BURROW_RECV(sc->done_serving, NULL),
        };
        if (chan_select(gate, 2) == 1 || terminal)
            break;
    }
    h2s_reader_done(sc);
}

typedef struct h2s_AsyncWrite {
    h2s_Conn *sc;
    Http2FrameWriteRequest wr;
    bool data; /* the frame is started and only end_write is left */
} h2s_AsyncWrite;

/* writeFrameAsync, on its own goroutine. At most one runs at a time. */
static void h2s_write_frame_async(void *env) {
    h2s_AsyncWrite *aw = (h2s_AsyncWrite *)env;
    h2s_Conn *sc = aw->sc;
    ArenaMark m = error_mark();
    Error err;
    if (aw->data)
        err = burrow__http2_framer_end_write(sc->framer);
    else
        err = burrow__http2_write_frame(&aw->wr.write, h2s_write_context(sc));
    h2s_FrameWriteResult res;
    res.wr = aw->wr;
    res.err = h2s_keep_err(sc, err);
    error_release(m);
    mem_free(sc->a, aw, sizeof *aw, _Alignof(h2s_AsyncWrite));
    chan_send(sc->wrote_frame_ch, &res); /* room for one, never blocks */
    sync_wait_group_done(&sc->io_wg);
}

#define H2S_PREFACE_LEN (sizeof HTTP2_CLIENT_PREFACE - 1)

static void h2s_read_preface_body(void *env) {
    h2s_Conn *sc = (h2s_Conn *)env;
    ArenaMark m = error_mark();
    Byte buf[H2S_PREFACE_LEN];
    Error err = BURROW_NO_ERROR;
    IoReader r = {&sc->conn.vt->reader, sc->conn.data};
    (void)io_read_full(r, slice_from(buf, sizeof buf, sizeof buf, TYPE_BYTE), &err);
    if (BURROW_OK(err) &&
        !str_eq(str_from_bytes(buf, sizeof buf), BURROW_S(HTTP2_CLIENT_PREFACE)))
        err = fmt_errorf_v("bogus greeting %q", str_from_bytes(buf, sizeof buf));
    err = h2s_keep_err(sc, err);
    error_release(m);
    chan_send(sc->preface_ch, &err); /* room for one */
    sync_wait_group_done(&sc->io_wg);
}

/* readPreface: the client's greeting, or errPrefaceTimeout. */
static Error h2s_read_preface(h2s_Conn *sc) {
    if (sc->saw_client_preface)
        return BURROW_NO_ERROR;
    sync_wait_group_add(&sc->io_wg, 1);
    Func fn;
    fn.f = h2s_read_preface_body;
    fn.env = sc;
    if (!go(fn)) {
        sync_wait_group_done(&sc->io_wg);
        return burrow__http2_err_preface_timeout;
    }
    TimeTimer *timer = time_new_timer(sc->a, H2S_PREFACE_TIMEOUT);
    Error err = BURROW_NO_ERROR;
    SelectCase cases[] = {
        BURROW_RECV(sc->preface_ch, &err),
        BURROW_RECV(timer != NULL ? time_timer_c(timer) : NULL, NULL),
    };
    if (chan_select(cases, 2) == 1) {
        err = burrow__http2_err_preface_timeout;
    } else if (BURROW_OK(err)) {
        h2s_vlogf(sc, "http2: server: client %.*s said hello", (int)sc->remote_addr.len,
                  (const char *)sc->remote_addr.p);
    }
    if (timer != NULL) {
        (void)time_timer_stop(timer);
        time_timer_free(timer);
    }
    return err;
}

/* ------------------------------------------------------------ write path */

static void h2s_close_stream(h2s_Conn *sc, h2s_Stream *st, Error err);
static void h2s_reset_stream(h2s_Conn *sc, Http2StreamError se);

static h2s_Stream *h2s_stream_lookup(h2s_Conn *sc, uint32_t id) {
    h2s_Stream *st = NULL;
    if (!map_get2(sc->streams, &id, &st))
        return NULL;
    return st;
}

/* Go's state. */
static h2s_StreamState h2s_state(h2s_Conn *sc, uint32_t id, h2s_Stream **out) {
    *out = NULL;
    h2s_Stream *st = h2s_stream_lookup(sc, id);
    if (st != NULL) {
        *out = st;
        return st->state;
    }
    /* "The first use of a new stream identifier implicitly closes all
     * streams in the "idle" state that might have been initiated by that
     * peer with a lower-valued stream identifier." (RFC 9113 5.1.1) */
    if (id % 2 == 1) {
        if (id <= sc->max_client_stream_id)
            return H2S_STATE_CLOSED;
    } else {
        if (id <= sc->max_pushed_stream_id)
            return H2S_STATE_CLOSED;
    }
    return H2S_STATE_IDLE;
}

static BURROW_NORETURN void h2s_panic_wr(const char *what,
                                         const Http2FrameWriteRequest *wr) {
    Str msg = fmt_sprintf_v(heap_allocator(),
                            "internal error: attempt to send frame on a %s stream: "
                            "[FrameWriteRequest stream=%d, ch=%t, writer=%d]",
                            what, (Int)burrow__http2_frame_write_request_stream_id(wr),
                            wr->done != NULL, (Int)wr->write.kind);
    panic_str(msg);
}

static void h2s_schedule_frame_write(h2s_Conn *sc);
static void h2s_wrote_frame(h2s_Conn *sc, h2s_FrameWriteResult res);

/* writeFrame: queue wr, unless its stream is gone. */
static void h2s_write_frame(h2s_Conn *sc, Http2FrameWriteRequest wr) {
    bool ignore = false;
    uint32_t id = burrow__http2_frame_write_request_stream_id(&wr);
    if (id != 0) {
        bool is_reset = wr.write.kind == HTTP2_WRITE_STREAM_ERROR;
        h2s_Stream *st;
        if (h2s_state(sc, id, &st) == H2S_STATE_CLOSED && !is_reset)
            ignore = true;
    }
    switch (wr.write.kind) {
    case HTTP2_WRITE_RES_HEADERS:
        if (wr.stream != NULL)
            ((h2s_Stream *)(void *)wr.stream)->wrote_headers = true;
        break;
    case HTTP2_WRITE_100_CONTINUE:
        if (wr.stream != NULL && ((h2s_Stream *)(void *)wr.stream)->wrote_headers) {
            /* Sending a 100 Continue after the response headers have been
             * written is not allowed. */
            if (wr.done != NULL)
                panic_str(BURROW_S("wr.done != nil for write100ContinueHeadersFrame"));
            ignore = true;
        }
        break;
    case HTTP2_WRITE_NIL:
    case HTTP2_WRITE_FLUSH:
    case HTTP2_WRITE_SETTINGS:
    case HTTP2_WRITE_GO_AWAY:
    case HTTP2_WRITE_DATA:
    case HTTP2_WRITE_HANDLER_PANIC_RST:
    case HTTP2_WRITE_STREAM_ERROR:
    case HTTP2_WRITE_PING:
    case HTTP2_WRITE_PING_ACK:
    case HTTP2_WRITE_SETTINGS_ACK:
    case HTTP2_WRITE_PUSH_PROMISE:
    case HTTP2_WRITE_WINDOW_UPDATE:
    default:
        break;
    }
    if (!ignore) {
        bool control = burrow__http2_frame_write_request_is_control(&wr);
        if (control) {
            sc->queued_control_frames++;
            if (sc->queued_control_frames < 0) {
                /* For extra safety, detect wraparounds, which should not
                 * happen, and pull the plug. */
                (void)h2s_wc_close_conn(sc);
            }
        }
        if (!burrow__http2_write_scheduler_push(sc->write_sched, wr)) {
            /* Out of memory. The writer hears about it as a write error. */
            if (control)
                sc->queued_control_frames--;
            burrow__http2_frame_write_request_reply_to_writer(&wr,
                                                              burrow_err_out_of_memory);
            h2s_wr_release(sc, &wr);
        }
    } else {
        h2s_wr_release(sc, &wr);
    }
    h2s_schedule_frame_write(sc);
}

/* startFrameWrite. */
static void h2s_start_frame_write(h2s_Conn *sc, Http2FrameWriteRequest wr) {
    if (sc->writing_frame)
        panic_str(BURROW_S("internal error: can only be writing one frame at a time"));
    h2s_Stream *st = (h2s_Stream *)(void *)wr.stream;
    if (st != NULL) {
        switch (st->state) {
        case H2S_STATE_HALF_CLOSED_LOCAL:
            switch (wr.write.kind) {
            case HTTP2_WRITE_STREAM_ERROR:
            case HTTP2_WRITE_HANDLER_PANIC_RST:
            case HTTP2_WRITE_WINDOW_UPDATE:
                /* RFC 9113 8.1 allows this: a server can send a complete
                 * response before the client has sent the entire request,
                 * and then RST_STREAM it. */
                break;
            case HTTP2_WRITE_NIL:
            case HTTP2_WRITE_FLUSH:
            case HTTP2_WRITE_SETTINGS:
            case HTTP2_WRITE_GO_AWAY:
            case HTTP2_WRITE_DATA:
            case HTTP2_WRITE_PING:
            case HTTP2_WRITE_PING_ACK:
            case HTTP2_WRITE_SETTINGS_ACK:
            case HTTP2_WRITE_RES_HEADERS:
            case HTTP2_WRITE_PUSH_PROMISE:
            case HTTP2_WRITE_100_CONTINUE:
            default:
                h2s_panic_wr("half-closed-local", &wr);
            }
            break;
        case H2S_STATE_CLOSED:
            h2s_panic_wr("closed", &wr);
        case H2S_STATE_IDLE:
        case H2S_STATE_OPEN:
        case H2S_STATE_HALF_CLOSED_REMOTE:
        default:
            break;
        }
    }

    sc->writing_frame = true;
    sc->needs_frame_flush = true;
    if (burrow__http2_write_stays_within_buffer(&wr.write, h2s_bw_available(&sc->bw))) {
        sc->writing_frame_async = false;
        ArenaMark m = error_mark();
        h2s_FrameWriteResult res;
        res.wr = wr;
        res.err = h2s_keep_err(
            sc, burrow__http2_write_frame(&wr.write, h2s_write_context(sc)));
        error_release(m);
        h2s_wrote_frame(sc, res);
        return;
    }
    bool data = wr.write.kind == HTTP2_WRITE_DATA;
    Error serr = BURROW_NO_ERROR;
    if (data) {
        /* Encode the frame here, so the goroutine below only writes bytes
         * and never reads the handler's buffer. */
        Slice nopad;
        memset(&nopad, 0, sizeof nopad);
        ArenaMark m = error_mark();
        serr =
            h2s_keep_err(sc, burrow__http2_framer_start_write_data_padded(
                                 sc->framer, wr.write.u.data.stream_id,
                                 wr.write.u.data.end_stream, wr.write.u.data.p, nopad));
        error_release(m);
    }
    h2s_AsyncWrite *aw =
        (h2s_AsyncWrite *)mem_alloc(sc->a, sizeof *aw, _Alignof(h2s_AsyncWrite));
    if (aw != NULL) {
        aw->sc = sc;
        aw->wr = wr;
        aw->data = data;
        sync_wait_group_add(&sc->io_wg, 1);
    }
    Func fn;
    fn.f = h2s_write_frame_async;
    fn.env = aw;
    if (aw == NULL || BURROW_FAILED(serr) || !go(fn)) {
        if (aw != NULL) {
            sync_wait_group_done(&sc->io_wg);
            mem_free(sc->a, aw, sizeof *aw, _Alignof(h2s_AsyncWrite));
        }
        sc->writing_frame_async = false;
        h2s_FrameWriteResult res;
        res.wr = wr;
        res.err = BURROW_FAILED(serr) ? serr : burrow_err_out_of_memory;
        h2s_wrote_frame(sc, res);
        return;
    }
    sc->writing_frame_async = true;
}

/* wroteFrame: a frame write finished, on this goroutine or another. */
static void h2s_wrote_frame(h2s_Conn *sc, h2s_FrameWriteResult res) {
    if (!sc->writing_frame)
        panic_str(BURROW_S("internal error: expected to be already writing a frame"));
    sc->writing_frame = false;
    sc->writing_frame_async = false;

    if (BURROW_FAILED(res.err))
        (void)h2s_wc_close_conn(sc);

    Http2FrameWriteRequest wr = res.wr;
    if (burrow__http2_write_ends_stream(&wr.write)) {
        h2s_Stream *st = (h2s_Stream *)(void *)wr.stream;
        if (st == NULL)
            panic_str(BURROW_S("internal error: expecting non-nil stream"));
        switch (st->state) {
        case H2S_STATE_OPEN:
            /* Here we would go to stateHalfClosedLocal in theory, but since
             * our handler is done and the client hasn't finished sending its
             * request, there's nothing left to do. Reset the stream, and the
             * client can close its half. */
            st->state = H2S_STATE_HALF_CLOSED_LOCAL;
            {
                Http2StreamError se;
                se.stream_id = st->base.id;
                se.code = HTTP2_ERR_CODE_NO;
                se.cause = BURROW_NO_ERROR;
                h2s_reset_stream(sc, se);
            }
            break;
        case H2S_STATE_HALF_CLOSED_REMOTE:
            h2s_close_stream(sc, st, burrow__http2_err_handler_complete);
            break;
        case H2S_STATE_IDLE:
        case H2S_STATE_HALF_CLOSED_LOCAL:
        case H2S_STATE_CLOSED:
        default:
            break;
        }
    } else if (wr.write.kind == HTTP2_WRITE_STREAM_ERROR) {
        Http2StreamError v = wr.write.u.stream_error;
        h2s_Stream *st = h2s_stream_lookup(sc, v.stream_id);
        if (st != NULL) {
            ArenaMark m = error_mark();
            h2s_close_stream(sc, st,
                             burrow__http2_stream_error(v.stream_id, v.code, v.cause));
            error_release(m);
        }
    } else if (wr.write.kind == HTTP2_WRITE_HANDLER_PANIC_RST) {
        h2s_close_stream(sc, (h2s_Stream *)(void *)wr.stream,
                         burrow__http2_err_handler_panicked);
    }

    /* Reply to the writer before the table size change below, which needs
     * nothing from it. */
    burrow__http2_frame_write_request_reply_to_writer(&wr, res.err);
    h2s_wr_release(sc, &wr);

    if (sc->pending_table_size && !sc->writing_frame) {
        /* SETTINGS_HEADER_TABLE_SIZE from the peer, held back until no
         * HEADERS frame is being encoded on another goroutine. */
        sc->pending_table_size = false;
        burrow__hpack_encoder_set_max_dynamic_table_size(sc->hpack_encoder,
                                                         sc->pending_table_size_v);
    }

    h2s_schedule_frame_write(sc);
}

/* scheduleFrameWrite: start writing the next frame, if no write is under
 * way. */
static void h2s_schedule_frame_write(h2s_Conn *sc) {
    if (sc->writing_frame || sc->in_frame_schedule_loop)
        return;
    sc->in_frame_schedule_loop = true;
    while (!sc->writing_frame_async) {
        if (sc->need_to_send_go_away) {
            sc->need_to_send_go_away = false;
            h2s_start_frame_write(
                sc, h2s_wr_make(burrow__http2_write_go_away(sc->max_client_stream_id,
                                                            sc->go_away_code),
                                NULL, NULL));
            continue;
        }
        if (sc->need_to_send_settings_ack) {
            sc->need_to_send_settings_ack = false;
            h2s_start_frame_write(
                sc, h2s_wr_make(burrow__http2_write_settings_ack(), NULL, NULL));
            continue;
        }
        if (!sc->in_go_away || sc->go_away_code == HTTP2_ERR_CODE_NO) {
            Http2FrameWriteRequest wr;
            if (burrow__http2_write_scheduler_pop(sc->write_sched, &wr)) {
                if (burrow__http2_frame_write_request_is_control(&wr))
                    sc->queued_control_frames--;
                h2s_start_frame_write(sc, wr);
                continue;
            }
        }
        if (sc->needs_frame_flush) {
            h2s_start_frame_write(sc,
                                  h2s_wr_make(burrow__http2_write_flush(), NULL, NULL));
            sc->needs_frame_flush = false; /* after start_frame_write, which sets it */
            continue;
        }
        break;
    }
    sc->in_frame_schedule_loop = false;
}

/* startGracefulShutdown, from any goroutine but the serve one. */
static void h2s_start_graceful_shutdown(h2s_Conn *sc) {
    if (sync_atomic_bool_compare_and_swap(&sc->shutdown_once, false, true))
        h2s_send_serve_msg(sc, h2s_msg(H2S_MSG_GRACEFUL));
}

/* goAway. */
static void h2s_go_away(h2s_Conn *sc, Http2ErrCode code) {
    if (sc->in_go_away) {
        if (sc->go_away_code == HTTP2_ERR_CODE_NO)
            sc->go_away_code = code;
        return;
    }
    sc->in_go_away = true;
    sc->need_to_send_go_away = true;
    sc->go_away_code = code;
    h2s_schedule_frame_write(sc);
}

/* resetStream. The cause has to outlive this call, since the write is
 * queued. */
static void h2s_reset_stream(h2s_Conn *sc, Http2StreamError se) {
    se.cause = h2s_keep_err(sc, se.cause);
    h2s_write_frame(sc, h2s_wr_make(burrow__http2_write_stream_error(se), NULL, NULL));
    h2s_Stream *st = h2s_stream_lookup(sc, se.stream_id);
    if (st != NULL)
        st->reset_queued = true;
}

/* ------------------------------------------------------- frame processing */

static Error h2s_process_frame(h2s_Conn *sc, Http2Frame *f);
static void h2s_end_stream(h2s_Stream *st);

/* setConnState. */
static void h2s_set_conn_state(h2s_Conn *sc, HttpConnState state) {
    if (!BURROW_FUNC_IS_NIL(sc->srv->conn_state))
        BURROW_CALLF(sc->srv->conn_state, sc->conn, state);
}

/* IdleTimeout, or ReadTimeout when that is unset. */
static Duration h2s_idle_timeout(const h2s_Conn *sc) {
    if (sc->srv->idle_timeout != 0)
        return sc->srv->idle_timeout;
    return sc->srv->read_timeout;
}

static uint32_t h2s_cur_open_streams(const h2s_Conn *sc) {
    return sc->cur_client_streams + sc->cur_pushed_streams;
}

/* sendWindowUpdate, for the connection when st is NULL. */
static void h2s_send_window_update(h2s_Conn *sc, h2s_Stream *st, Int n) {
    uint32_t id = 0;
    int32_t send;
    if (st == NULL) {
        send = burrow__http2_inflow_add(&sc->inflow, n);
    } else {
        id = st->base.id;
        send = burrow__http2_inflow_add(&st->inflow, n);
    }
    if (send == 0)
        return;
    h2s_write_frame(
        sc,
        h2s_wr_make(burrow__http2_write_window_update(id, (uint32_t)send), st, NULL));
}

/* noteBodyRead. */
static void h2s_note_body_read(h2s_Conn *sc, h2s_Stream *st, Int n) {
    h2s_send_window_update(sc, NULL, n); /* conn-level */
    if (st->state != H2S_STATE_HALF_CLOSED_REMOTE && st->state != H2S_STATE_CLOSED) {
        /* Don't send this WINDOW_UPDATE if the stream is closed remotely. */
        h2s_send_window_update(sc, st, n);
    }
}

/* processFrameFromReader: false ends the connection. */
static bool h2s_process_frame_from_reader(h2s_Conn *sc, h2s_ReadFrameResult res) {
    Error err = res.err;
    if (BURROW_FAILED(err)) {
        if (errors_is(err, burrow__http2_err_frame_too_large)) {
            h2s_go_away(sc, HTTP2_ERR_CODE_FRAME_SIZE);
            return true; /* goAway will close the loop */
        }
        bool client_gone = errors_is(err, io_eof) ||
                           errors_is(err, io_err_unexpected_eof) ||
                           h2s_is_closed_conn_error(err);
        if (client_gone) {
            /* TODO: could we also get into this state if the peer does a
             * half close (e.g. CloseWrite) because they're done sending
             * frames but they're still wanting our open replies? Investigate
             * this. */
            return false;
        }
    } else {
        Http2Frame *f = res.f;
        if (burrow__http2_verbose_logs()) {
            Str sum = burrow__http2_summarize_frame(sc->a, f);
            h2s_vlogf(sc, "http2: server read frame %s", sum);
            mem_free(sc->a, (void *)(uintptr_t)sum.p, (size_t)sum.len, 1);
        }
        err = h2s_process_frame(sc, f);
        if (BURROW_OK(err))
            return true;
    }

    Http2StreamError se;
    Http2ErrCode code;
    if (burrow__http2_error_stream(err, &se)) {
        h2s_reset_stream(sc, se);
        return true;
    }
    if (errors_is(err, burrow__http2_err_go_away_flow)) {
        h2s_go_away(sc, HTTP2_ERR_CODE_FLOW_CONTROL);
        return true;
    }
    if (burrow__http2_error_connection(err, &code)) {
        if (res.f != NULL) {
            uint32_t id = res.f->header.stream_id;
            if (id > sc->max_client_stream_id)
                sc->max_client_stream_id = id;
        }
        h2s_logf(sc, "http2: server connection error from %s: %v", sc->remote_addr,
                 err);
        h2s_go_away(sc, code);
        return true; /* goAway will handle shutdown */
    }
    if (BURROW_FAILED(res.err)) {
        h2s_vlogf(sc,
                  "http2: server closing client connection; error reading frame from "
                  "client %s: %v",
                  sc->remote_addr, err);
    } else {
        h2s_logf(sc, "http2: server closing client connection: %v", err);
    }
    return false;
}

/* processPing. */
static Error h2s_process_ping(h2s_Conn *sc, Http2Frame *f) {
    if (burrow__http2_ping_frame_is_ack(f)) {
        if (sc->ping_sent && memcmp(sc->sent_ping_data, f->u.ping.data, 8) == 0) {
            /* This is a response to a PING we sent. */
            sc->ping_sent = false;
            (void)h2s_timer_arm(sc, &sc->read_idle_timer, sc->read_idle_timeout);
        }
        /* 6.7 PING: "An endpoint MUST NOT respond to PING frames containing
         * this flag." */
        return BURROW_NO_ERROR;
    }
    if (f->header.stream_id != 0) {
        /* "PING frames are not associated with any individual stream. If a
         * PING frame is received with a stream identifier field value other
         * than 0x0, the recipient MUST respond with a connection error
         * (Section 5.4.1) of type PROTOCOL_ERROR." */
        return h2s_conn_err(sc, "ping_on_stream", HTTP2_ERR_CODE_PROTOCOL);
    }
    h2s_write_frame(
        sc, h2s_wr_make(burrow__http2_write_ping_ack(f->u.ping.data), NULL, NULL));
    return BURROW_NO_ERROR;
}

/* processWindowUpdate. */
static Error h2s_process_window_update(h2s_Conn *sc, Http2Frame *f) {
    uint32_t id = f->header.stream_id;
    int32_t inc = (int32_t)f->u.window_update.increment;
    if (id != 0) { /* stream-level flow control */
        h2s_Stream *st;
        h2s_StreamState state = h2s_state(sc, id, &st);
        if (state == H2S_STATE_IDLE) {
            /* Section 5.1: "Receiving any frame other than HEADERS or
             * PRIORITY on a stream in this state MUST be treated as a
             * connection error (Section 5.4.1) of type PROTOCOL_ERROR." */
            return h2s_conn_err(sc, "stream_idle", HTTP2_ERR_CODE_PROTOCOL);
        }
        if (st == NULL) {
            /* "WINDOW_UPDATE can be sent by a peer that has sent a frame
             * bearing the END_STREAM flag. This means that a receiver could
             * receive a WINDOW_UPDATE frame on a "half closed (remote)" or
             * "closed" stream. A receiver MUST NOT treat this as an error,
             * see Section 5.1." */
            return BURROW_NO_ERROR;
        }
        h2s_Stream *s2 = st;
        if (!burrow__http2_outflow_add(&s2->base.flow, inc))
            return h2s_stream_err(sc, "bad_flow", id, HTTP2_ERR_CODE_FLOW_CONTROL);
    } else { /* connection-level flow control */
        if (!burrow__http2_outflow_add(&sc->flow, inc))
            return burrow__http2_err_go_away_flow;
    }
    h2s_schedule_frame_write(sc);
    return BURROW_NO_ERROR;
}

/* processResetStream. */
static Error h2s_process_reset_stream(h2s_Conn *sc, Http2Frame *f) {
    uint32_t id = f->header.stream_id;
    h2s_Stream *st;
    h2s_StreamState state = h2s_state(sc, id, &st);
    if (state == H2S_STATE_IDLE) {
        /* 6.4 "RST_STREAM frames MUST NOT be sent for a stream in the "idle"
         * state. If a RST_STREAM frame identifying an idle stream is
         * received, the recipient MUST treat this as a connection error
         * (Section 5.4.1) of type PROTOCOL_ERROR. */
        return h2s_conn_err(sc, "reset_idle_stream", HTTP2_ERR_CODE_PROTOCOL);
    }
    if (st != NULL) {
        BURROW_CALLF0(st->cancel_ctx);
        h2s_close_stream(sc, st, h2s_stream_error(id, f->u.rst_stream.err_code));
    }
    return BURROW_NO_ERROR;
}

/* closeStream. */
static void h2s_close_stream(h2s_Conn *sc, h2s_Stream *st, Error err) {
    if (st->state == H2S_STATE_IDLE || st->state == H2S_STATE_CLOSED) {
        panic_str(fmt_sprintf_v(heap_allocator(),
                                "invariant; can't close stream in state %d",
                                (Int)st->state));
    }
    st->state = H2S_STATE_CLOSED;
    if (st->read_deadline_live)
        (void)h2s_stream_timer_stop(st, st->read_deadline);
    if (st->write_deadline_live)
        (void)h2s_stream_timer_stop(st, st->write_deadline);
    if (st->base.id % 2 == 0)
        sc->cur_pushed_streams--;
    else
        sc->cur_client_streams--;
    uint32_t id = st->base.id;
    map_del(sc->streams, &id);
    if (map_len(sc->streams) == 0) {
        h2s_set_conn_state(sc, HTTP_STATE_IDLE);
        Duration idle = h2s_idle_timeout(sc);
        if (idle > 0 && sc->idle_timer.t != NULL)
            (void)h2s_timer_arm(sc, &sc->idle_timer, idle);
        if (!burrow__http_server_do_keep_alives(sc->srv))
            h2s_go_away(sc, HTTP2_ERR_CODE_NO);
    }
    if (st->has_body) {
        /* Return any buffered unread bytes worth of conn-level flow control.
         * See golang.org/issue/16481 */
        h2s_send_window_update(sc, NULL, burrow__http2_pipe_len(&st->pipe));
        burrow__http2_pipe_close_with_error(&st->pipe, err);
    }
    Http2StreamError se;
    if (burrow__http2_error_stream(err, &se))
        err = BURROW_OK(se.cause) ? burrow__http2_err_stream_closed : se.cause;
    st->close_err = error_retain(arena_allocator(&st->arena), err);
    BURROW_CALLF0(st->cancel_ctx);
    chan_close(st->cw); /* signals Handler's CloseNotifier, unblocks writes, etc */
    burrow__http2_write_scheduler_close_stream(sc->write_sched, id);
    h2s_stream_unref(st); /* the map's */
}

/* processSetting, as a Http2SettingFunc. */
static Error h2s_process_setting_initial_window_size(h2s_Conn *sc, uint32_t val);

static Error h2s_process_setting(void *env, Http2Setting s) {
    h2s_Conn *sc = (h2s_Conn *)env;
    Error err = burrow__http2_setting_valid(s);
    if (BURROW_FAILED(err))
        return err;
    if (burrow__http2_verbose_logs()) {
        Str ss = burrow__http2_setting_string(sc->a, s);
        h2s_vlogf(sc, "http2: server processing setting %s", ss);
        mem_free(sc->a, (void *)(uintptr_t)ss.p, (size_t)ss.len, 1);
    }
    switch (s.id) {
    case HTTP2_SETTING_HEADER_TABLE_SIZE:
        if (sc->writing_frame_async) {
            /* A HEADERS frame may be being encoded on the writer goroutine.
             * Go changes the table under it; here it waits for wroteFrame. */
            sc->pending_table_size = true;
            sc->pending_table_size_v = s.val;
        } else {
            sc->pending_table_size = false;
            burrow__hpack_encoder_set_max_dynamic_table_size(sc->hpack_encoder, s.val);
        }
        break;
    case HTTP2_SETTING_ENABLE_PUSH:
        sc->push_enabled = s.val != 0;
        break;
    case HTTP2_SETTING_MAX_CONCURRENT_STREAMS:
        sc->client_max_streams = s.val;
        break;
    case HTTP2_SETTING_INITIAL_WINDOW_SIZE:
        return h2s_process_setting_initial_window_size(sc, s.val);
    case HTTP2_SETTING_MAX_FRAME_SIZE:
        sc->base.max_frame_size =
            (int32_t)s.val; /* the maximum valid s.val is < 2^31 */
        break;
    case HTTP2_SETTING_MAX_HEADER_LIST_SIZE:
        sc->peer_max_header_list_size = s.val;
        break;
    case HTTP2_SETTING_ENABLE_CONNECT_PROTOCOL:
        /* Receipt of this parameter by a server does not have any impact. */
        break;
    case HTTP2_SETTING_NO_RFC7540_PRIORITIES:
        if (s.val > 1)
            return burrow__http2_connection_error(HTTP2_ERR_CODE_PROTOCOL);
        break;
    default:
        /* Unknown setting: "An endpoint that receives a SETTINGS frame with
         * any unknown or unsupported identifier MUST ignore that setting." */
        if (burrow__http2_verbose_logs()) {
            Str ss = burrow__http2_setting_string(sc->a, s);
            h2s_vlogf(sc, "http2: server ignoring unknown setting %s", ss);
            mem_free(sc->a, (void *)(uintptr_t)ss.p, (size_t)ss.len, 1);
        }
        break;
    }
    return BURROW_NO_ERROR;
}

/* processSettingInitialWindowSize. */
static Error h2s_process_setting_initial_window_size(h2s_Conn *sc, uint32_t val) {
    /* Note: val already validated to be within range by processSetting's
     * Valid call. */
    int32_t old = sc->initial_stream_send_window_size;
    sc->initial_stream_send_window_size = (int32_t)val;
    int32_t growth = (int32_t)val - old; /* may be negative */
    MapIter it = map_iter(sc->streams);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        h2s_Stream *st = *(h2s_Stream **)v;
        if (!burrow__http2_outflow_add(&st->base.flow, growth)) {
            /* 6.9.2 Initial Flow Control Window Size "An endpoint MUST treat
             * a change to SETTINGS_INITIAL_WINDOW_SIZE that causes any flow
             * control window to exceed the maximum size as a connection
             * error (Section 5.4.1) of type FLOW_CONTROL_ERROR." */
            return h2s_conn_err(sc, "setting_win_size", HTTP2_ERR_CODE_FLOW_CONTROL);
        }
    }
    return BURROW_NO_ERROR;
}

/* processSettings. */
static Error h2s_process_settings(h2s_Conn *sc, Http2Frame *f) {
    if (burrow__http2_settings_frame_is_ack(f)) {
        sc->unacked_settings--;
        if (sc->unacked_settings < 0) {
            /* Why is the peer ACKing settings we never sent? The spec
             * doesn't mention this case, but hang up on them anyway. */
            return h2s_conn_err(sc, "ack_mystery", HTTP2_ERR_CODE_PROTOCOL);
        }
        return BURROW_NO_ERROR;
    }
    if (burrow__http2_settings_frame_num_settings(f) > 100 ||
        burrow__http2_settings_frame_has_duplicates(f)) {
        /* This isn't actually in the spec, but hang up on suspiciously large
         * settings frames or those with duplicate entries. */
        return h2s_conn_err(sc, "settings_big_or_dups", HTTP2_ERR_CODE_PROTOCOL);
    }
    Http2SettingFunc fn;
    fn.f = h2s_process_setting;
    fn.env = sc;
    Error err = burrow__http2_settings_frame_foreach_setting(f, fn);
    if (BURROW_FAILED(err))
        return err;
    /* TODO: judging by RFC 7540, Section 6.5.3 each SETTINGS frame should be
     * acknowledged individually, even if multiple are received before the
     * ACK. */
    sc->need_to_send_settings_ack = true;
    h2s_schedule_frame_write(sc);
    return BURROW_NO_ERROR;
}

/* processData. */
static Error h2s_process_data(h2s_Conn *sc, Http2Frame *f) {
    uint32_t id = f->header.stream_id;
    uint32_t length = f->header.length;
    Slice data = burrow__http2_data_frame_data(f);
    h2s_Stream *st;
    h2s_StreamState state = h2s_state(sc, id, &st);
    if (id == 0 || state == H2S_STATE_IDLE) {
        /* Section 6.1: "DATA frames MUST be associated with a stream. If a
         * DATA frame is received whose stream identifier field is 0x0, the
         * recipient MUST respond with a connection error (Section 5.4.1) of
         * type PROTOCOL_ERROR." */
        return h2s_conn_err(sc, "data_on_idle", HTTP2_ERR_CODE_PROTOCOL);
    }

    /* "If a DATA frame is received whose stream is not in "open" or "half
     * closed (local)" state, the recipient MUST respond with a stream error
     * (Section 5.4.2) of type STREAM_CLOSED." */
    if (st == NULL || state != H2S_STATE_OPEN || st->got_trailer_header ||
        st->reset_queued) {
        /* This includes sending a RST_STREAM if the stream is in
         * stateHalfClosedLocal (which currently means that the http.Handler
         * returned, so it's done reading & done writing). Try to stop the
         * client from sending more DATA. */

        /* But still enforce their connection-level flow control, and return
         * any flow control bytes since we're not going to consume them. */
        if (!burrow__http2_inflow_take(&sc->inflow, length))
            return h2s_stream_err(sc, "data_flow", id, HTTP2_ERR_CODE_FLOW_CONTROL);
        h2s_send_window_update(sc, NULL, (Int)length); /* conn-level */

        if (st != NULL && st->reset_queued) {
            /* Already have a stream error in flight. Don't send another. */
            return BURROW_NO_ERROR;
        }
        return h2s_stream_err(sc, "closed", id, HTTP2_ERR_CODE_STREAM_CLOSED);
    }
    if (!st->has_body)
        panic_str(BURROW_S("internal error: should have a body in this state"));

    /* Sender sending more than they'd declared? */
    if (st->decl_body_bytes != -1 && st->body_bytes + data.len > st->decl_body_bytes) {
        if (!burrow__http2_inflow_take(&sc->inflow, length))
            return h2s_stream_err(sc, "data_flow", id, HTTP2_ERR_CODE_FLOW_CONTROL);
        h2s_send_window_update(sc, NULL, (Int)length); /* conn-level */

        burrow__http2_pipe_close_with_error(
            &st->pipe,
            fmt_errorf_v("sender tried to send more than declared Content-Length "
                         "of %d bytes",
                         (Int)st->decl_body_bytes));
        /* RFC 7540, sec 8.1.2.6: A request or response is also malformed if
         * the value of a content-length header field does not equal the sum
         * of the DATA frame payload lengths that form the body. */
        return h2s_stream_err(sc, "send_too_much", id, HTTP2_ERR_CODE_PROTOCOL);
    }
    if (length > 0) {
        /* Check whether the client has flow control quota. */
        if (!burrow__http2_take_inflows(&sc->inflow, &st->inflow, length))
            return h2s_stream_err(sc, "flow_on_data_length", id,
                                  HTTP2_ERR_CODE_FLOW_CONTROL);

        if (data.len > 0) {
            st->body_bytes += data.len;
            Error werr = BURROW_NO_ERROR;
            Int wrote = burrow__http2_pipe_write(&st->pipe, data, &werr);
            if (BURROW_FAILED(werr)) {
                /* The handler has closed the request body. Return the
                 * connection-level flow control for the discarded data, but
                 * not the stream-level flow control. */
                h2s_send_window_update(sc, NULL, (Int)length - wrote);
                return BURROW_NO_ERROR;
            }
            if (wrote != data.len)
                panic_str(BURROW_S("internal error: bad Writer"));
        }

        /* Return any padded flow control now, since we won't refund it
         * later on body reads. Call sendWindowUpdate even if there is no
         * padding, to return buffered flow control credit if the sent window
         * has become too small. */
        int32_t pad = (int32_t)length - (int32_t)data.len;
        h2s_send_window_update(sc, NULL, pad);
        h2s_send_window_update(sc, st, pad);
    }
    if (burrow__http2_data_frame_stream_ended(f))
        h2s_end_stream(st);
    return BURROW_NO_ERROR;
}

/* processGoAway. */
static Error h2s_process_go_away(h2s_Conn *sc, Http2Frame *f) {
    Str fh = burrow__http2_frame_header_string(sc->a, f->header);
    Byte cb[HTTP2_STRING_MAX];
    Str code = burrow__http2_err_code_string(f->u.go_away.err_code, cb);
    if (f->u.go_away.err_code != HTTP2_ERR_CODE_NO) {
        h2s_logf(sc,
                 "http2: received GOAWAY &{FrameHeader:%s LastStreamID:%d ErrCode:%s "
                 "debugData:%v}, starting graceful shutdown",
                 fh, (Int)f->u.go_away.last_stream_id, code, f->u.go_away.debug_data);
    } else {
        h2s_vlogf(sc,
                  "http2: received GOAWAY &{FrameHeader:%s LastStreamID:%d ErrCode:%s "
                  "debugData:%v}, starting graceful shutdown",
                  fh, (Int)f->u.go_away.last_stream_id, code, f->u.go_away.debug_data);
    }
    mem_free(sc->a, (void *)(uintptr_t)fh.p, (size_t)fh.len, 1);
    h2s_go_away(sc, HTTP2_ERR_CODE_NO);
    /* http://tools.ietf.org/html/rfc7540#section-6.8 We should not create
     * any new streams, which means we should disable push. */
    sc->push_enabled = false;
    return BURROW_NO_ERROR;
}

/* copyTrailersToHandlerRequest, which the handler's last body Read runs. */
static void h2s_copy_trailers_to_handler_request(void *env) {
    h2s_Stream *st = (h2s_Stream *)env;
    MapIter it = map_iter(st->trailer);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        if (map_get(st->req_trailer, k) != NULL) {
            /* Only copy it over it was pre-declared in the initial header
             * keys. */
            (void)map_set(st->req_trailer, k, v);
        }
    }
}

/* endStream: the client is done sending. */
static void h2s_end_stream(h2s_Stream *st) {
    if (st->decl_body_bytes != -1 && st->decl_body_bytes != st->body_bytes) {
        burrow__http2_pipe_close_with_error(
            &st->pipe,
            fmt_errorf_v("request declared a Content-Length of %d but only wrote "
                         "%d bytes",
                         (Int)st->decl_body_bytes, (Int)st->body_bytes));
    } else {
        Func fn;
        memset(&fn, 0, sizeof fn);
        if (st->trailer != NULL) {
            fn.f = h2s_copy_trailers_to_handler_request;
            fn.env = st;
        }
        burrow__http2_pipe_close_with_error_and_code(&st->pipe, io_eof, fn);
        burrow__http2_pipe_close_with_error(&st->pipe, io_eof);
    }
    st->state = H2S_STATE_HALF_CLOSED_REMOTE;
}

/* checkPriority. */
static Error h2s_check_priority(h2s_Conn *sc, uint32_t id, Http2PriorityParam p) {
    if (id == p.stream_dep) {
        /* Section 5.3.1: "A stream cannot depend on itself. An endpoint MUST
         * treat this as a stream error (Section 5.4.2) of type
         * PROTOCOL_ERROR." Section 5.3.3 says that a stream can depend on
         * one of its dependencies, so it's only self-dependencies that are
         * forbidden. */
        return h2s_stream_err(sc, "priority", id, HTTP2_ERR_CODE_PROTOCOL);
    }
    return BURROW_NO_ERROR;
}

/* writeSchedIgnoresRFC7540: both of the schedulers here do. */
static bool h2s_write_sched_ignores_rfc7540(const h2s_Conn *sc) {
    (void)sc;
    return true;
}

/* processPriority. */
static Error h2s_process_priority(h2s_Conn *sc, Http2Frame *f) {
    Error err = h2s_check_priority(sc, f->header.stream_id, f->u.priority.priority);
    if (BURROW_FAILED(err))
        return err;
    if (h2s_write_sched_ignores_rfc7540(sc))
        return BURROW_NO_ERROR;
    burrow__http2_write_scheduler_adjust_stream(sc->write_sched, f->header.stream_id,
                                                f->u.priority.priority);
    return BURROW_NO_ERROR;
}

/* processPriorityUpdate. */
static Error h2s_process_priority_update(h2s_Conn *sc, Http2Frame *f) {
    sc->priority_aware = true;
    if (!burrow__http2_write_scheduler_is_rfc9218(sc->write_sched))
        return BURROW_NO_ERROR;
    Http2PriorityParam p;
    if (!burrow__http2_parse_rfc9218_priority(f->u.priority_update.priority,
                                              sc->priority_aware, &p)) {
        return h2s_stream_err(sc, "unparsable_priority_update",
                              f->u.priority_update.prioritized_stream_id,
                              HTTP2_ERR_CODE_PROTOCOL);
    }
    burrow__http2_write_scheduler_adjust_stream(
        sc->write_sched, f->u.priority_update.prioritized_stream_id, p);
    return BURROW_NO_ERROR;
}

/* --------------------------------------------------------- new streams */

static void h2s_stream_timer_init_deadline(h2s_Stream *st, bool write, Duration d) {
    if (write)
        st->write_deadline_live =
            h2s_stream_timer_arm(st, &st->write_deadline, d, h2s_write_deadline_fire);
    else
        st->read_deadline_live =
            h2s_stream_timer_arm(st, &st->read_deadline, d, h2s_read_deadline_fire);
}

/* newStream. NULL when memory runs out. */
static h2s_Stream *h2s_new_stream(h2s_Conn *sc, uint32_t id, uint32_t pusher_id,
                                  h2s_StreamState state, Http2PriorityParam priority) {
    if (id == 0)
        panic_str(BURROW_S("internal error: cannot create stream with id 0"));
    h2s_Stream *st = (h2s_Stream *)mem_alloc(sc->a, sizeof *st, _Alignof(h2s_Stream));
    if (st == NULL)
        return NULL;
    memset(st, 0, sizeof *st);
    arena_init(&st->arena, sc->a, 0);
    sync_atomic_int32_store(&st->refs, 1); /* the map's */
    st->sc = sc;
    st->base.sc = &sc->base;
    st->base.id = id;
    st->state = state;
    st->ctx = context_with_cancel(sc->a, sc->base_ctx, &st->cancel_ctx);
    st->cw = chan_make(sc->a, TYPE_BOOL, 0);
    if (st->cw == NULL || !map_set(sc->streams, &id, &st)) {
        st->state = H2S_STATE_IDLE;
        h2s_stream_free(st);
        return NULL;
    }
    st->base.flow.conn = &sc->flow;
    (void)burrow__http2_outflow_add(&st->base.flow,
                                    sc->initial_stream_send_window_size);
    burrow__http2_inflow_init(&st->inflow, sc->initial_stream_recv_window_size);
    if (sc->srv->write_timeout > 0)
        h2s_stream_timer_init_deadline(st, true, sc->srv->write_timeout);

    Http2OpenStreamOptions opts;
    memset(&opts, 0, sizeof opts);
    opts.pusher_id = pusher_id;
    opts.priority = priority;
    (void)burrow__http2_write_scheduler_open_stream(sc->write_sched, id, opts);
    if (id % 2 == 0)
        sc->cur_pushed_streams++;
    else
        sc->cur_client_streams++;
    if (h2s_cur_open_streams(sc) == 1)
        h2s_set_conn_state(sc, HTTP_STATE_ACTIVE);
    return st;
}

/* processTrailerHeaders. */
static Error h2s_process_trailer_headers(h2s_Stream *st, Http2Frame *f) {
    h2s_Conn *sc = st->sc;
    uint32_t id = st->base.id;
    if (st->got_trailer_header)
        return h2s_conn_err(sc, "dup_trailers", HTTP2_ERR_CODE_PROTOCOL);
    st->got_trailer_header = true;
    if (!burrow__http2_headers_frame_stream_ended(f))
        return h2s_stream_err(sc, "trailers_not_ended", id, HTTP2_ERR_CODE_PROTOCOL);

    if (burrow__http2_meta_headers_frame_pseudo_fields(f).len > 0)
        return h2s_stream_err(sc, "trailers_pseudo", id, HTTP2_ERR_CODE_PROTOCOL);
    if (f->u.meta_headers.truncated)
        return h2s_stream_err(sc, "trailers_too_large", id, HTTP2_ERR_CODE_PROTOCOL);
    if (st->trailer != NULL) {
        Alloc *a = arena_allocator(&st->arena);
        HpackHeaderFields fs = burrow__http2_meta_headers_frame_regular_fields(f);
        for (Int i = 0; i < fs.len; i++) {
            Str key = http_canonical_header_key(a, str_clone(a, fs.p[i].name));
            if (!burrow__httpguts_valid_trailer_header(key)) {
                /* TODO: send more details to the peer somehow. But http2 has
                 * no way to send debug data at a stream level. Discuss with
                 * HTTP folk. */
                return h2s_stream_err(sc, "trailers_bogus", id,
                                      HTTP2_ERR_CODE_PROTOCOL);
            }
            (void)http_header_add(st->trailer, key, str_clone(a, fs.p[i].value));
        }
    }
    h2s_end_stream(st);
    return BURROW_NO_ERROR;
}

static Int h2s_rb_read(void *self, Slice p, Error *err);
static Error h2s_rb_close(void *self);
static const IoReadCloserVT h2s_rb_vt = {{NULL, h2s_rb_read}, {NULL, h2s_rb_close}};

static h2s_Rws *h2s_new_response_writer(h2s_Stream *st);

/* NewServerRequest's work on the header: Expect, Cookie and Trailer. */
static HttpHeader h2s_declared_trailers(Alloc *a, HttpHeader header) {
    HttpHeader trailer = NULL;
    Slice vs = http_header_values(header, BURROW_S("Trailer"));
    for (Int i = 0; i < vs.len; i++) {
        Str v = ((const Str *)vs.p)[i];
        for (;;) {
            Int c = strings_index_byte(v, ',');
            Str key = c < 0 ? v : str_from_bytes(v.p, c);
            key = http_canonical_header_key(a, textproto_trim_string(key));
            if (!str_eq(key, BURROW_S("Transfer-Encoding")) &&
                !str_eq(key, BURROW_S("Trailer")) &&
                !str_eq(key, BURROW_S("Content-Length"))) {
                /* Bogus. (copy of http1 rules) Ignore. */
                if (trailer == NULL)
                    trailer = http_header_make(a);
                Slice none;
                memset(&none, 0, sizeof none);
                if (trailer != NULL)
                    (void)map_set(trailer, &key, &none);
            }
            if (c < 0)
                break;
            v = str_from_bytes(v.p + c + 1, v.len - c - 1);
        }
    }
    http_header_del(header, BURROW_S("Trailer"));
    return trailer;
}

/* newWriterAndRequest. NULL and an error, or the request. */
static HttpRequest *h2s_new_writer_and_request(h2s_Conn *sc, h2s_Stream *st,
                                               Http2Frame *f, Error *err) {
    *err = BURROW_NO_ERROR;
    uint32_t id = f->header.stream_id;
    Str method = burrow__http2_meta_headers_frame_pseudo_value(f, BURROW_S("method"));
    Str scheme = burrow__http2_meta_headers_frame_pseudo_value(f, BURROW_S("scheme"));
    Str authority =
        burrow__http2_meta_headers_frame_pseudo_value(f, BURROW_S("authority"));
    Str path = burrow__http2_meta_headers_frame_pseudo_value(f, BURROW_S("path"));
    Str protocol =
        burrow__http2_meta_headers_frame_pseudo_value(f, BURROW_S("protocol"));

    if (burrow__http2_extended_connect_disabled() && protocol.len != 0) {
        *err = h2s_stream_err(sc, "bad_connect", id, HTTP2_ERR_CODE_PROTOCOL);
        return NULL;
    }

    bool is_connect = str_eq(method, BURROW_S("CONNECT"));
    if (is_connect) {
        if (protocol.len == 0 &&
            (path.len != 0 || scheme.len != 0 || authority.len == 0)) {
            *err = h2s_stream_err(sc, "bad_connect", id, HTTP2_ERR_CODE_PROTOCOL);
            return NULL;
        }
    } else if (method.len == 0 || path.len == 0 ||
               (!str_eq(scheme, BURROW_S("https")) &&
                !str_eq(scheme, BURROW_S("http")))) {
        /* See 8.1.2.6 Malformed Requests and Responses:

           Malformed requests or responses that are detected MUST be treated
           as a stream error (Section 5.4.2) of type PROTOCOL_ERROR.

           8.1.2.3 Request Pseudo-Header Fields "All HTTP/2 requests MUST
           include exactly one valid value for the :method, :scheme, and
           :path pseudo-header fields" */
        *err = h2s_stream_err(sc, "bad_path_method", id, HTTP2_ERR_CODE_PROTOCOL);
        return NULL;
    }

    HttpRequest *req =
        (HttpRequest *)mem_alloc(sc->a, sizeof *req, _Alignof(HttpRequest));
    if (req == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    memset(req, 0, sizeof *req);
    req->a = sc->a;
    arena_init(&req->arena, sc->a, 0);
    st->req = req; /* the stream frees it */
    Alloc *ra = arena_allocator(&req->arena);
    method = str_clone(ra, method);
    scheme = str_clone(ra, scheme);
    authority = str_clone(ra, authority);
    path = str_clone(ra, path);
    protocol = str_clone(ra, protocol);

    HttpHeader header = http_header_make(ra);
    if (header == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    HpackHeaderFields fs = burrow__http2_meta_headers_frame_regular_fields(f);
    for (Int i = 0; i < fs.len; i++) {
        Str key = http_canonical_header_key(ra, str_clone(ra, fs.p[i].name));
        (void)http_header_add(header, key, str_clone(ra, fs.p[i].value));
    }
    if (authority.len == 0)
        authority = http_header_get(header, BURROW_S("Host"));
    if (protocol.len != 0)
        (void)http_header_set(header, BURROW_S(":protocol"), protocol);

    /* newWriterAndRequestNoBody, with httpcommon.NewServerRequest. */
    Slice expect = http_header_values(header, BURROW_S("Expect"));
    bool needs_continue = burrow__httpguts_header_values_contains_token(
        (const Str *)expect.p, expect.len, BURROW_S("100-continue"));
    if (needs_continue)
        http_header_del(header, BURROW_S("Expect"));
    /* Merge Cookie headers into one "; "-delimited value. */
    Slice cookies = http_header_values(header, BURROW_S("Cookie"));
    if (cookies.len > 1)
        (void)http_header_set(header, BURROW_S("Cookie"),
                              strings_join(ra, cookies, BURROW_S("; ")));
    /* Setup Trailers */
    HttpHeader trailer = h2s_declared_trailers(ra, header);

    const char *invalid = NULL;
    Url *u = NULL;
    Str request_uri;
    memset(&request_uri, 0, sizeof request_uri);
    /* "':authority' MUST NOT include the deprecated userinfo subcomponent
     * for "http" or "https" schemed URIs." RFC 9113 8.3.1 */
    if (strings_index_byte(authority, '@') != -1 &&
        (str_eq(scheme, BURROW_S("http")) || str_eq(scheme, BURROW_S("https")))) {
        invalid = "userinfo_in_authority";
    } else if (is_connect && protocol.len == 0) {
        u = (Url *)mem_alloc(ra, sizeof *u, _Alignof(Url));
        if (u == NULL) {
            *err = burrow_err_out_of_memory;
            return NULL;
        }
        memset(u, 0, sizeof *u);
        u->host = authority;
        request_uri = authority; /* mimic HTTP/1 server behavior */
    } else {
        /* Note: CONNECT with :protocol (RFC 8441) uses the normal path. */
        if (path.len == 0 || (path.p[0] != '/' && !str_eq(path, BURROW_S("*")))) {
            /* "All HTTP/2 requests MUST include exactly one valid value for
             * the :path pseudo-header field unless it is a CONNECT request
             * (Section 8.5)." */
            invalid = "bad_path";
        } else {
            ArenaMark m = error_mark();
            Error perr = BURROW_NO_ERROR;
            u = url_parse_request_uri(ra, path, &perr);
            error_release(m);
            if (u == NULL)
                invalid = "bad_path";
            request_uri = path;
        }
    }
    if (invalid != NULL) {
        *err = h2s_stream_err(sc, invalid, st->base.id, HTTP2_ERR_CODE_PROTOCOL);
        return NULL;
    }

    st->body.stream = st;
    st->body.needs_continue = needs_continue;
    st->rws = h2s_new_response_writer(st);
    if (st->rws == NULL) {
        *err = burrow_err_out_of_memory;
        return NULL;
    }
    st->rws->req = req;
    req->ctx = st->ctx;
    req->method = method;
    req->url = u;
    req->remote_addr = str_clone(ra, sc->remote_addr);
    req->header = header;
    req->request_uri = request_uri;
    req->proto = BURROW_S("HTTP/2.0");
    req->proto_major = 2;
    req->proto_minor = 0;
    req->host = authority;
    req->body.vt = &h2s_rb_vt;
    req->body.data = &st->body;
    req->trailer = trailer;

    bool body_open = !burrow__http2_headers_frame_stream_ended(f);
    Slice clens = http_header_values(header, BURROW_S("Content-Length"));
    if (clens.len > 0) {
        const Str *cl = (const Str *)clens.p;
        ArenaMark m = error_mark();
        Error perr = BURROW_NO_ERROR;
        uint64_t n = strconv_parse_uint(cl[0], 10, 63, &perr);
        req->content_length = BURROW_OK(perr) ? (int64_t)n : 0;
        error_release(m);
        if (!body_open && req->content_length != 0) {
            *err = h2s_stream_err(sc, "bodyless_content_length", id,
                                  HTTP2_ERR_CODE_PROTOCOL);
            return NULL;
        }
        if (clens.len > 1) {
            for (Int i = 1; i < clens.len; i++) {
                if (!str_eq(cl[0], cl[i])) {
                    *err = h2s_stream_err(sc, "duplicate_content_length", id,
                                          HTTP2_ERR_CODE_PROTOCOL);
                    return NULL;
                }
            }
            Slice one = clens;
            one.len = 1;
            Str key = BURROW_S("Content-Length");
            (void)map_set(header, &key, &one);
        }
    }
    if (body_open) {
        if (clens.len == 0)
            req->content_length = -1;
        st->dbuf.a = sc->a;
        st->dbuf.expected = req->content_length;
        burrow__http2_pipe_set_buffer(
            &st->pipe, burrow__http2_data_buffer_as_pipe_buffer(&st->dbuf));
        st->body.has_pipe = true;
    }
    return req;
}

/* checkValidHTTP2RequestHeaders. */
static Error h2s_check_valid_request_headers(HttpHeader h) {
    static const Str conn_headers[] = {
        BURROW_S_INIT("Connection"),       BURROW_S_INIT("Keep-Alive"),
        BURROW_S_INIT("Proxy-Connection"), BURROW_S_INIT("Transfer-Encoding"),
        BURROW_S_INIT("Upgrade"),
    };
    for (size_t i = 0; i < sizeof conn_headers / sizeof conn_headers[0]; i++) {
        if (map_get(h, &conn_headers[i]) != NULL)
            return fmt_errorf_v("request header %q is not valid in HTTP/2",
                                conn_headers[i]);
    }
    Slice te = http_header_values(h, BURROW_S("Te"));
    if (te.len > 0) {
        const Str *v = (const Str *)te.p;
        if (te.len > 1 || (!str_eq(v[0], BURROW_S("trailers")) && v[0].len != 0))
            return fmt_errorf_v(
                "request header \"TE\" may only be \"trailers\" in HTTP/2");
    }
    return BURROW_NO_ERROR;
}

static Error h2s_schedule_handler(h2s_Conn *sc, h2s_Stream *st);

/* processHeaders. */
static Error h2s_process_headers(h2s_Conn *sc, Http2Frame *f) {
    uint32_t id = f->header.stream_id;

    /* http://tools.ietf.org/html/rfc7540#section-5.1.1 Streams initiated by
     * a client MUST use odd-numbered stream identifiers. [...] An endpoint
     * that receives an unexpected stream identifier MUST respond with a
     * connection error (Section 5.4.1) of type PROTOCOL_ERROR. */
    if (id % 2 != 1)
        return h2s_conn_err(sc, "headers_even", HTTP2_ERR_CODE_PROTOCOL);
    /* A HEADERS frame can be used to create a new stream or send a trailer
     * for an open one. If we already have a stream open, then this is a
     * trailer. */
    h2s_Stream *existing = h2s_stream_lookup(sc, id);
    if (existing != NULL) {
        if (existing->reset_queued) {
            /* We're sending RST_STREAM to close the stream, so don't bother
             * processing this frame. */
            return BURROW_NO_ERROR;
        }
        /* RFC 7540, sec 5.1: If an endpoint receives additional frames,
         * other than WINDOW_UPDATE, PRIORITY, or RST_STREAM, for a stream
         * that is in this state, it MUST respond with a stream error
         * (Section 5.4.2) of type STREAM_CLOSED. */
        if (existing->state == H2S_STATE_HALF_CLOSED_REMOTE)
            return h2s_stream_err(sc, "headers_half_closed", id,
                                  HTTP2_ERR_CODE_STREAM_CLOSED);
        return h2s_process_trailer_headers(existing, f);
    }

    /* [...] The identifier of a newly established stream MUST be
     * numerically greater than all streams that the initiating endpoint has
     * opened or reserved. [...] An endpoint that receives an unexpected
     * stream identifier MUST respond with a connection error (Section
     * 5.4.1) of type PROTOCOL_ERROR. */
    if (id <= sc->max_client_stream_id)
        return h2s_conn_err(sc, "stream_went_down", HTTP2_ERR_CODE_PROTOCOL);
    sc->max_client_stream_id = id;

    h2s_timer_stop(sc, &sc->idle_timer);

    /* http://tools.ietf.org/html/rfc7540#section-5.1.2 [...] Endpoints MUST
     * NOT exceed the limit set by their peer. An endpoint that receives a
     * HEADERS frame that causes their advertised concurrent stream limit to
     * be exceeded MUST treat this as a stream error (Section 5.4.2) of type
     * PROTOCOL_ERROR or REFUSED_STREAM. */
    if (sc->cur_client_streams + 1 > sc->adv_max_streams) {
        if (sc->unacked_settings == 0) {
            /* They should know better. */
            return h2s_stream_err(sc, "over_max_streams", id, HTTP2_ERR_CODE_PROTOCOL);
        }
        /* Assume it's a network race, where they just haven't received our
         * last SETTINGS update. But actually we can't be certain. Send
         * REFUSED_STREAM. */
        return h2s_stream_err(sc, "over_max_streams_race", id,
                              HTTP2_ERR_CODE_REFUSED_STREAM);
    }

    h2s_StreamState initial_state = H2S_STATE_OPEN;
    if (burrow__http2_headers_frame_stream_ended(f))
        initial_state = H2S_STATE_HALF_CLOSED_REMOTE;

    /* We are handling two special cases here:
     * 1. When a request is sent via an intermediary, we force priority to be
     *    u=3,i. This is essentially a round-robin behavior, and is done to
     *    ensure fairness between, for example, multiple clients using the
     *    same proxy.
     * 2. Until a client has shown that it is aware of RFC 9218, we make its
     *    streams non-incremental by default. This is done to preserve the
     *    historical behavior of handling streams in a round-robin manner,
     *    rather than one-by-one to completion. */
    Http2PriorityParam initial_priority = burrow__http2_default_rfc9218_priority(
        sc->priority_aware && !sc->has_intermediary);
    if (burrow__http2_write_scheduler_is_rfc9218(sc->write_sched) &&
        !sc->has_intermediary) {
        bool aware = false;
        bool inter = false;
        initial_priority = burrow__http2_meta_headers_frame_rfc9218_priority(
            f, sc->priority_aware, &aware, &inter);
        sc->has_intermediary = inter;
        if (aware)
            sc->priority_aware = true;
    }
    h2s_Stream *st = h2s_new_stream(sc, id, 0, initial_state, initial_priority);
    if (st == NULL)
        return h2s_conn_err(sc, "out_of_memory", HTTP2_ERR_CODE_INTERNAL);

    if (burrow__http2_headers_frame_has_priority(f)) {
        Error err = h2s_check_priority(sc, id, f->u.meta_headers.headers.priority);
        if (BURROW_FAILED(err))
            return err;
        if (!h2s_write_sched_ignores_rfc7540(sc))
            burrow__http2_write_scheduler_adjust_stream(
                sc->write_sched, id, f->u.meta_headers.headers.priority);
    }

    Error err;
    HttpRequest *req = h2s_new_writer_and_request(sc, st, f, &err);
    if (req == NULL)
        return err;
    st->req_trailer = req->trailer;
    if (st->req_trailer != NULL) {
        st->trailer = http_header_make(arena_allocator(&st->arena));
        if (st->trailer == NULL)
            return h2s_conn_err(sc, "out_of_memory", HTTP2_ERR_CODE_INTERNAL);
    }
    st->has_body = st->body.has_pipe;
    st->decl_body_bytes = req->content_length;

    st->handler_kind = H2S_HANDLER_SERVER;
    if (f->u.meta_headers.truncated) {
        /* Their header list was too long. Send a 431 error. */
        st->handler_kind = H2S_HANDLER_431;
    } else {
        ArenaMark m = error_mark();
        Error herr = h2s_check_valid_request_headers(req->header);
        if (BURROW_FAILED(herr)) {
            st->handler_kind = H2S_HANDLER_400;
            st->handler_msg = str_clone(arena_allocator(&st->arena), error_text(herr));
        }
        error_release(m);
    }

    /* The net/http package sets the read deadline from the
     * http.Server.ReadTimeout during the TLS handshake, but then passes
     * control to us. Set the deadline here. */
    if (sc->srv->read_timeout > 0)
        h2s_stream_timer_init_deadline(st, false, sc->srv->read_timeout);

    return h2s_schedule_handler(sc, st);
}

/* processFrame. */
static Error h2s_process_frame(h2s_Conn *sc, Http2Frame *f) {
    /* First frame received must be SETTINGS. */
    if (!sc->saw_first_settings) {
        if (f->kind != HTTP2_SETTINGS_FRAME)
            return h2s_conn_err(sc, "first_settings", HTTP2_ERR_CODE_PROTOCOL);
        sc->saw_first_settings = true;
    }

    /* Discard frames for streams initiated after the identified last stream
     * sent in a GOAWAY, or all frames after sending an error. We still need
     * to return connection-level flow control for DATA frames. RFC 9113
     * Section 6.8. */
    if (sc->in_go_away && (sc->go_away_code != HTTP2_ERR_CODE_NO ||
                           f->header.stream_id > sc->max_client_stream_id)) {
        if (f->kind == HTTP2_DATA_FRAME) {
            if (!burrow__http2_inflow_take(&sc->inflow, f->header.length))
                return h2s_stream_err(sc, "data_flow", f->header.stream_id,
                                      HTTP2_ERR_CODE_FLOW_CONTROL);
            h2s_send_window_update(sc, NULL, (Int)f->header.length); /* conn-level */
        }
        return BURROW_NO_ERROR;
    }

    switch (f->kind) {
    case HTTP2_SETTINGS_FRAME:
        return h2s_process_settings(sc, f);
    case HTTP2_META_HEADERS_FRAME:
        return h2s_process_headers(sc, f);
    case HTTP2_WINDOW_UPDATE_FRAME:
        return h2s_process_window_update(sc, f);
    case HTTP2_PING_FRAME:
        return h2s_process_ping(sc, f);
    case HTTP2_DATA_FRAME:
        return h2s_process_data(sc, f);
    case HTTP2_RST_STREAM_FRAME:
        return h2s_process_reset_stream(sc, f);
    case HTTP2_PRIORITY_FRAME:
        return h2s_process_priority(sc, f);
    case HTTP2_GO_AWAY_FRAME:
        return h2s_process_go_away(sc, f);
    case HTTP2_PUSH_PROMISE_FRAME:
        /* A client cannot push. Thus, servers MUST treat the receipt of a
         * PUSH_PROMISE frame as a connection error (Section 5.4.1) of type
         * PROTOCOL_ERROR. */
        return h2s_conn_err(sc, "push_promise", HTTP2_ERR_CODE_PROTOCOL);
    case HTTP2_PRIORITY_UPDATE_FRAME:
        return h2s_process_priority_update(sc, f);
    case HTTP2_HEADERS_FRAME:
    case HTTP2_CONTINUATION_FRAME:
    case HTTP2_UNKNOWN_FRAME:
    default: {
        Str fh = burrow__http2_frame_header_string(sc->a, f->header);
        h2s_vlogf(sc, "http2: server ignoring frame: %s", fh);
        mem_free(sc->a, (void *)(uintptr_t)fh.p, (size_t)fh.len, 1);
        return BURROW_NO_ERROR;
    }
    }
}

/* ------------------------------------------------------- the request body */

static bool h2s_is_eof(Error err) {
    return err.vt == io_eof.vt && err.data == io_eof.data;
}

/* noteBodyReadFromHandler. The message holds st. */
static void h2s_note_body_read_from_handler(h2s_Stream *st, Int n) {
    if (n <= 0)
        return;
    h2s_Conn *sc = st->sc;
    h2s_BodyReadMsg m;
    m.st = st;
    m.n = n;
    h2s_stream_ref(st);
    SelectCase cases[] = {
        BURROW_SEND(sc->body_read_ch, &m),
        BURROW_RECV(sc->done_serving, NULL),
    };
    if (chan_select(cases, 2) == 1)
        h2s_stream_unref(st);
}

/* write100ContinueHeaders. */
static void h2s_write_100_continue_headers(h2s_Stream *st) {
    (void)h2s_write_frame_from_handler(
        st->sc, h2s_wr_make(burrow__http2_write_100_continue(st->base.id), st, NULL));
}

/* requestBody.Read. */
static Int h2s_rb_read(void *self, Slice p, Error *err) {
    h2s_ReqBody *b = (h2s_ReqBody *)self;
    *err = BURROW_NO_ERROR;
    if (b->needs_continue) {
        b->needs_continue = false;
        h2s_write_100_continue_headers(b->stream);
    }
    if (!b->has_pipe || b->saw_eof) {
        *err = io_eof;
        return 0;
    }
    Int n = burrow__http2_pipe_read(&b->stream->pipe, p, err);
    if (h2s_is_eof(*err))
        b->saw_eof = true;
    h2s_note_body_read_from_handler(b->stream, n);
    return n;
}

/* requestBody.Close. */
static Error h2s_rb_close(void *self) {
    h2s_ReqBody *b = (h2s_ReqBody *)self;
    if (!sync_atomic_bool_swap(&b->closed, true) && b->has_pipe)
        burrow__http2_pipe_break_with_error(&b->stream->pipe,
                                            burrow__http2_err_closed_body);
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------ the response writer state */

static Int h2s_chunk_write(void *self, Slice p, Error *err);

static const Type h2s_chunk_writer_desc = {
    {(const Byte *)"chunkWriter", 11},
    {(const Byte *)"net/http/internal/http2", 23},
    KIND_STRUCT,
    (uint32_t)sizeof(h2s_Rws *),
    (uint16_t)_Alignof(h2s_Rws *),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x3c81d02eU,
    NULL};

static const IoWriterVT h2s_chunk_writer_vt = {&h2s_chunk_writer_desc, h2s_chunk_write};

static h2s_Rws *h2s_new_response_writer(h2s_Stream *st) {
    h2s_Conn *sc = st->sc;
    h2s_Rws *rws = (h2s_Rws *)mem_alloc(sc->a, sizeof *rws, _Alignof(h2s_Rws));
    if (rws == NULL)
        return NULL;
    memset(rws, 0, sizeof *rws);
    arena_init(&rws->arena, sc->a, 0);
    rws->stream = st;
    rws->conn = sc;
    IoWriter w;
    w.vt = &h2s_chunk_writer_vt;
    w.data = rws;
    rws->bw = bufio_new_writer_size(sc->a, w, H2S_HANDLER_CHUNK_WRITE_SIZE);
    rws->errc = chan_make(sc->a, TYPE_ERROR, 1);
    if (rws->bw == NULL || rws->errc == NULL) {
        h2s_rws_free(rws, sc->a);
        return NULL;
    }
    st->rw.rws = rws;
    return rws;
}

static void h2s_rws_free(h2s_Rws *rws, Alloc *a) {
    if (rws->bw != NULL)
        bufio_writer_free(rws->bw);
    if (rws->errc != NULL)
        chan_free(rws->errc);
    for (Int i = 0; i < rws->nold_errc; i++)
        chan_free(rws->old_errc[i]);
    if (rws->close_notifier_ch != NULL)
        chan_free(rws->close_notifier_ch);
    arena_free(&rws->arena);
    mem_free(a, rws, sizeof *rws, _Alignof(h2s_Rws));
}

/* A wait on errc gave up, and the serve goroutine may still answer on it, so
 * it is put aside until the stream goes and the next write gets a new one. */
static void h2s_rws_abandon_errc(h2s_Rws *rws) {
    if (rws->nold_errc == rws->old_errc_cap) {
        Int cap = rws->old_errc_cap == 0 ? 4 : rws->old_errc_cap * 2;
        Chan **n = (Chan **)mem_alloc(arena_allocator(&rws->arena),
                                      (size_t)cap * sizeof *n, _Alignof(Chan *));
        if (n == NULL) {
            /* Lose the channel rather than reuse it. */
            rws->errc = NULL;
            return;
        }
        for (Int i = 0; i < rws->nold_errc; i++)
            n[i] = rws->old_errc[i];
        rws->old_errc = n;
        rws->old_errc_cap = cap;
    }
    rws->old_errc[rws->nold_errc++] = rws->errc;
    rws->errc = chan_make(rws->conn->a, TYPE_ERROR, 1);
}

/* The wait at the end of writeHeaders and writeDataFromHandler. */
static Error h2s_wait_write(h2s_Stream *st, bool data) {
    h2s_Conn *sc = st->sc;
    h2s_Rws *rws = st->rws;
    Error err = BURROW_NO_ERROR;
    SelectCase cases[] = {
        BURROW_RECV(rws->errc, &err),
        BURROW_RECV(sc->done_serving, NULL),
        BURROW_RECV(st->cw, NULL),
    };
    switch (chan_select(cases, 3)) {
    case 0:
        return err;
    case 1:
        h2s_rws_abandon_errc(rws);
        return burrow__http2_err_client_disconnected;
    default:
        /* If both ch and stream.cw were ready (as might happen on the final
         * Write after an http.Handler ends), prefer the write result.
         * Otherwise this might just be us successfully closing the stream.
         * The writeFrameAsync and serve goroutines guarantee that the ch
         * send will happen before the stream.cw close. */
        if (data && chan_try_recv(rws->errc, &err, NULL))
            return err;
        h2s_rws_abandon_errc(rws);
        return burrow__http2_err_stream_closed;
    }
}

/* A copy of rh for the writer, which may outlive the handler's header. */
static h2s_ResHeaders *h2s_res_headers_copy(Alloc *a, const Http2WriteResHeaders *src) {
    h2s_ResHeaders *r =
        (h2s_ResHeaders *)mem_alloc(a, sizeof *r, _Alignof(h2s_ResHeaders));
    if (r == NULL)
        return NULL;
    memset(r, 0, sizeof *r);
    arena_init(&r->arena, a, 0);
    Alloc *ra = arena_allocator(&r->arena);
    r->rh = *src;
    r->rh.trailers = NULL;
    if (src->h != NULL) {
        r->rh.h = http_header_clone(ra, src->h);
        if (r->rh.h == NULL)
            goto fail;
    }
    if (src->ntrailers > 0) {
        Str *t =
            (Str *)mem_alloc(ra, (size_t)src->ntrailers * sizeof *t, _Alignof(Str));
        if (t == NULL)
            goto fail;
        for (Int i = 0; i < src->ntrailers; i++)
            t[i] = str_clone(ra, src->trailers[i]);
        r->rh.trailers = t;
    }
    r->rh.date = str_clone(ra, src->date);
    r->rh.content_type = str_clone(ra, src->content_type);
    r->rh.content_length = str_clone(ra, src->content_length);
    return r;
fail:
    arena_free(&r->arena);
    mem_free(a, r, sizeof *r, _Alignof(h2s_ResHeaders));
    return NULL;
}

/* writeHeaders, called from handler goroutines. */
static Error h2s_write_headers(h2s_Stream *st, const Http2WriteResHeaders *rh) {
    h2s_Conn *sc = st->sc;
    h2s_Rws *rws = st->rws;
    /* If there's a header map (which we don't own), so we have to block on
     * waiting for this frame to be written, so an http.Flush mid-handler
     * writes out the correct value of keys, before a handler later
     * potentially mutates it. */
    Chan *errc = NULL;
    if (rh->h != NULL) {
        errc = rws->errc;
        if (errc == NULL)
            return burrow_err_out_of_memory;
    }
    h2s_ResHeaders *r = h2s_res_headers_copy(sc->a, rh);
    if (r == NULL)
        return burrow_err_out_of_memory;
    Error err = h2s_write_frame_from_handler(
        sc, h2s_wr_make(burrow__http2_write_res_headers(&r->rh), st, errc));
    if (BURROW_FAILED(err))
        return err;
    if (errc == NULL)
        return BURROW_NO_ERROR;
    return h2s_wait_write(st, false);
}

/* writeDataFromHandler writes DATA response frames from a handler on the
 * given stream. */
static Error h2s_write_data_from_handler(h2s_Stream *st, Slice data, bool end_stream) {
    h2s_Rws *rws = st->rws;
    if (rws->errc == NULL)
        return burrow_err_out_of_memory;
    Error err = h2s_write_frame_from_handler(
        st->sc, h2s_wr_make(burrow__http2_write_data(st->base.id, data, end_stream), st,
                            rws->errc));
    if (BURROW_FAILED(err))
        return err;
    return h2s_wait_write(st, true);
}

static bool h2s_hhas(HttpHeader h, Str key) {
    return h != NULL && map_get(h, &key) != NULL;
}

static Str h2s_hget(HttpHeader h, Str key) {
    if (h == NULL) {
        Str z;
        memset(&z, 0, sizeof z);
        return z;
    }
    return http_header_get(h, key);
}

/* declareTrailer is called for each Trailer header when the response header
 * is written. It notes that a header will need to be written in the
 * trailers at the end of the response. */
static void h2s_declare_trailer(void *env, Str k) {
    h2s_Rws *rws = (h2s_Rws *)env;
    Alloc *ra = arena_allocator(&rws->arena);
    k = http_canonical_header_key(ra, k);
    if (!burrow__httpguts_valid_trailer_header(k)) {
        /* Forbidden by RFC 7230, section 4.1.2. */
        h2s_logf(rws->conn, "ignoring invalid trailer %q", k);
        return;
    }
    for (Int i = 0; i < rws->ntrailers; i++) {
        if (str_eq(rws->trailers[i], k))
            return;
    }
    if (rws->ntrailers == rws->trailers_cap) {
        Int cap = rws->trailers_cap == 0 ? 4 : rws->trailers_cap * 2;
        Str *n = (Str *)mem_alloc(ra, (size_t)cap * sizeof *n, _Alignof(Str));
        if (n == NULL)
            return;
        for (Int i = 0; i < rws->ntrailers; i++)
            n[i] = rws->trailers[i];
        rws->trailers = n;
        rws->trailers_cap = cap;
    }
    rws->trailers[rws->ntrailers++] = str_clone(ra, k);
}

#define H2S_TRAILER_PREFIX "Trailer:"

/* promoteUndeclaredTrailers permits http.Handlers to set trailers after the
 * header has already been flushed. Because the Go ResponseWriter interface
 * has no way to set Trailers (only the Header), and because we didn't want
 * to expand the ResponseWriter interface, and because nobody used trailers,
 * and because RFC 7230 says you SHOULD (but not must) predeclare any
 * trailers in the header, the official ResponseWriter rules said trailers
 * in Go must be predeclared, and then we reuse the same
 * ResponseWriter.Header() map to mean both Headers and Trailers. When it's
 * time to write the Trailers, we pick out the fields of Headers that were
 * declared as trailers. That worked for a while, until we found the first
 * major user of Trailers in the wild: gRPC (using them only over http2),
 * and gRPC libraries permit setting trailers mid-stream without predeclaring
 * them. So: change of plans. We still permit the old way, but we also permit
 * this hack: if a Header() key begins with "Trailer:", the suffix of that key
 * is a Trailer. Because ':' is an invalid token byte anyway, there is no
 * ambiguity. (And it's already filtered out) It's mildly hacky, but not
 * terrible.
 *
 * This method runs after the Handler is done and promotes any Header fields
 * to be trailers. */
static void h2s_promote_undeclared_trailers(h2s_Rws *rws) {
    HttpHeader h = rws->handler_header;
    if (h == NULL)
        return;
    Alloc *ra = arena_allocator(&rws->arena);
    Str prefix = BURROW_S(H2S_TRAILER_PREFIX);
    /* Collected first, since the map can't change while it is walked. */
    Int n = 0;
    MapIter it = map_iter(h);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v)) {
        if (strings_has_prefix(*(const Str *)k, prefix))
            n++;
    }
    if (n > 0) {
        Str *keys = (Str *)mem_alloc(ra, (size_t)n * sizeof *keys, _Alignof(Str));
        Slice *vals = (Slice *)mem_alloc(ra, (size_t)n * sizeof *vals, _Alignof(Slice));
        if (keys == NULL || vals == NULL)
            return;
        Int i = 0;
        it = map_iter(h);
        while (i < n && map_next(&it, &k, &v)) {
            Str key = *(const Str *)k;
            if (!strings_has_prefix(key, prefix))
                continue;
            keys[i] = str_from_bytes(key.p + prefix.len, key.len - prefix.len);
            vals[i] = *(const Slice *)v;
            i++;
        }
        for (i = 0; i < n; i++) {
            h2s_declare_trailer(rws, keys[i]);
            Str ck = http_canonical_header_key(ra, str_clone(ra, keys[i]));
            (void)map_set(h, &ck, &vals[i]);
        }
    }
    if (rws->ntrailers > 1)
        sort_strings(
            slice_from(rws->trailers, rws->ntrailers, rws->ntrailers, TYPE_STRING));
}

/* writeHeader. */
static void h2s_rws_write_header(h2s_Rws *rws, Int code) {
    if (rws->wrote_header)
        return;
    /* checkWriteHeaderCode */
    if (code < 100 || code > 999)
        panic_str(fmt_sprintf_v(heap_allocator(), "invalid WriteHeader code %v", code));

    /* Handle informational headers */
    if (code >= 100 && code <= 199) {
        /* Per RFC 8297 we must not clear the current header map */
        HttpHeader h = rws->handler_header;
        bool cl = h2s_hhas(h, BURROW_S("Content-Length"));
        bool te = h2s_hhas(h, BURROW_S("Transfer-Encoding"));
        if (cl || te) {
            h = http_header_clone(arena_allocator(&rws->arena), h);
            if (h != NULL) {
                http_header_del(h, BURROW_S("Content-Length"));
                http_header_del(h, BURROW_S("Transfer-Encoding"));
            }
        }
        Http2WriteResHeaders rh;
        memset(&rh, 0, sizeof rh);
        rh.stream_id = rws->stream->base.id;
        rh.http_res_code = code;
        rh.h = h;
        rh.end_stream = rws->handler_done && rws->ntrailers == 0;
        (void)h2s_write_headers(rws->stream, &rh);
        return;
    }

    rws->wrote_header = true;
    rws->status = code;
    if (rws->handler_header != NULL && map_len(rws->handler_header) > 0)
        rws->snap_header =
            http_header_clone(arena_allocator(&rws->arena), rws->handler_header);
}

static bool h2s_has_nonempty_trailers(h2s_Rws *rws) {
    for (Int i = 0; i < rws->ntrailers; i++) {
        if (h2s_hhas(rws->handler_header, rws->trailers[i]))
            return true;
    }
    return false;
}

static void h2s_declare_trailer_elements(h2s_Rws *rws) {
    if (rws->snap_header == NULL)
        return;
    Slice vs = http_header_values(rws->snap_header, BURROW_S("Trailer"));
    for (Int i = 0; i < vs.len; i++)
        burrow__http_foreach_header_element(((const Str *)vs.p)[i], h2s_declare_trailer,
                                            rws);
}

/* writeChunk writes chunks from the bufio.Writer. But because bufio.Writer
 * may bypass its chunking, sometimes p may be arbitrarily large.
 *
 * writeChunk is also responsible (on the first chunk) for sending the
 * HEADER response. */
static Int h2s_write_chunk(h2s_Rws *rws, Slice p, Error *err) {
    *err = BURROW_NO_ERROR;
    Alloc *ra = arena_allocator(&rws->arena);
    if (!rws->wrote_header)
        h2s_rws_write_header(rws, 200);

    if (rws->handler_done)
        h2s_promote_undeclared_trailers(rws);

    bool is_head_resp = str_eq(rws->req->method, BURROW_S("HEAD"));
    if (!rws->sent_header) {
        rws->sent_header = true;
        HttpHeader snap = rws->snap_header;
        Str ctype;
        Str clen = h2s_hget(snap, BURROW_S("Content-Length"));
        memset(&ctype, 0, sizeof ctype);
        if (clen.len != 0) {
            clen = str_clone(ra, clen);
            http_header_del(snap, BURROW_S("Content-Length"));
            ArenaMark m = error_mark();
            Error perr = BURROW_NO_ERROR;
            uint64_t cl = strconv_parse_uint(clen, 10, 63, &perr);
            if (BURROW_OK(perr))
                rws->sent_content_len = (int64_t)cl;
            else
                memset(&clen, 0, sizeof clen);
            error_release(m);
        }
        bool has_content_length = h2s_hhas(snap, BURROW_S("Content-Length"));
        if (!has_content_length && clen.len == 0 && rws->handler_done &&
            burrow__http_body_allowed_for_status(rws->status) &&
            (p.len > 0 || !is_head_resp))
            clen = strconv_itoa(ra, p.len);
        bool has_content_type = h2s_hhas(snap, BURROW_S("Content-Type"));
        /* If the Content-Encoding is non-blank, we shouldn't sniff the body.
         * See Issue golang.org/issue/31753. */
        bool has_ce = h2s_hget(snap, BURROW_S("Content-Encoding")).len > 0;
        if (!has_ce && !has_content_type &&
            burrow__http_body_allowed_for_status(rws->status) && p.len > 0)
            ctype = http_detect_content_type(p);
        Str date;
        memset(&date, 0, sizeof date);
        if (!h2s_hhas(snap, BURROW_S("Date"))) {
            Byte *buf = (Byte *)mem_alloc(ra, 32, 1);
            if (buf != NULL) {
                Slice d = time_append_format(time_utc(time_now()), NULL,
                                             slice_from(buf, 0, 32, TYPE_BYTE),
                                             HTTP_TIME_FORMAT);
                date = str_from_bytes(d.p, d.len);
            }
        }

        h2s_declare_trailer_elements(rws);

        /* "Connection" headers aren't allowed in HTTP/2 (RFC 7540,
         * 8.1.2.2), but respect "Connection" == "close" to mean sending a
         * GOAWAY and tearing down the TCP connection when idle, like we do
         * for HTTP/1.
         * TODO: remove more Connection-specific header fields here, in
         * addition to "Connection". */
        if (h2s_hhas(snap, BURROW_S("Connection"))) {
            Str v = h2s_hget(snap, BURROW_S("Connection"));
            bool close = str_eq(v, BURROW_S("close"));
            Str key = BURROW_S("Connection");
            map_del(snap, &key);
            if (close)
                h2s_start_graceful_shutdown(rws->conn);
        }

        bool end_stream =
            (rws->handler_done && rws->ntrailers == 0 && p.len == 0) || is_head_resp;
        Http2WriteResHeaders rh;
        memset(&rh, 0, sizeof rh);
        rh.stream_id = rws->stream->base.id;
        rh.http_res_code = rws->status;
        rh.h = snap;
        rh.end_stream = end_stream;
        rh.content_type = ctype;
        rh.content_length = clen;
        rh.date = date;
        *err = h2s_write_headers(rws->stream, &rh);
        if (BURROW_FAILED(*err))
            return 0;
        if (end_stream)
            return 0;
    }
    if (is_head_resp)
        return p.len;
    if (p.len == 0 && !rws->handler_done)
        return 0;

    /* only send trailers if they have actually been defined by the server
     * handler. */
    bool has_nonempty_trailers = h2s_has_nonempty_trailers(rws);
    bool end_stream = rws->handler_done && !has_nonempty_trailers;
    if (p.len > 0 || end_stream) {
        /* only send a 0 byte DATA frame if we're ending the stream. */
        *err = h2s_write_data_from_handler(rws->stream, p, end_stream);
        if (BURROW_FAILED(*err))
            return 0;
    }

    if (rws->handler_done && has_nonempty_trailers) {
        Http2WriteResHeaders rh;
        memset(&rh, 0, sizeof rh);
        rh.stream_id = rws->stream->base.id;
        rh.h = rws->handler_header;
        rh.trailers = rws->trailers;
        rh.ntrailers = rws->ntrailers;
        rh.end_stream = true;
        *err = h2s_write_headers(rws->stream, &rh);
        return p.len;
    }
    return p.len;
}

/* chunkWriter.Write. */
static Int h2s_chunk_write(void *self, Slice p, Error *err) {
    h2s_Rws *rws = (h2s_Rws *)self;
    Int n = h2s_write_chunk(rws, p, err);
    if (err->vt == burrow__http2_err_stream_closed.vt &&
        err->data == burrow__http2_err_stream_closed.data) {
        /* If writing failed because the stream has been closed, return the
         * reason it was closed. */
        *err = rws->stream->close_err;
    }
    return n;
}

/* ----------------------------------------------------- the response writer */

static h2s_Rws *h2s_rw_state(void *self, const char *what) {
    h2s_Rws *rws = ((h2s_Rw *)self)->rws;
    if (rws == NULL)
        panic_str(
            fmt_sprintf_v(heap_allocator(), "%s called after Handler finished", what));
    return rws;
}

/* responseWriter.FlushError. */
static Error h2s_rw_flush(void *self) {
    h2s_Rws *rws = h2s_rw_state(self, "Header");
    Error err;
    if (bufio_writer_buffered(rws->bw) > 0) {
        err = bufio_writer_flush(rws->bw);
    } else {
        /* The bufio.Writer won't call chunkWriter.Write (writeChunk with
         * zero bytes), so we have to do it ourselves to force the HTTP
         * response header and/or final DATA frame (with END_STREAM) to be
         * sent. */
        Slice none;
        memset(&none, 0, sizeof none);
        (void)h2s_chunk_write(rws, none, &err);
        if (BURROW_OK(err) && chan_try_recv(rws->stream->cw, NULL, NULL))
            err = rws->stream->close_err;
    }
    return err;
}

/* The goroutine CloseNotify starts, which holds the stream. */
static void h2s_close_notify_wait(void *env) {
    h2s_Rws *rws = (h2s_Rws *)env;
    h2s_Stream *st = rws->stream;
    h2s_Conn *sc = rws->conn;
    (void)chan_recv(st->cw, NULL); /* wait for close */
    bool t = true;
    chan_send(rws->close_notifier_ch, &t);
    h2s_stream_unref(st);
    sync_wait_group_done(&sc->wg);
}

/* responseWriter.CloseNotify. */
static Chan *h2s_rw_close_notify(void *self) {
    h2s_Rws *rws = h2s_rw_state(self, "CloseNotify");
    sync_mutex_lock(&rws->close_notifier_mu);
    Chan *ch = rws->close_notifier_ch;
    if (ch == NULL) {
        ch = chan_make(rws->conn->a, TYPE_BOOL, 1);
        rws->close_notifier_ch = ch;
        if (ch != NULL) {
            h2s_Conn *sc = rws->conn;
            h2s_stream_ref(rws->stream);
            sync_wait_group_add(&sc->wg, 1);
            Func fn;
            fn.f = h2s_close_notify_wait;
            fn.env = rws;
            if (!go(fn)) {
                h2s_stream_unref(rws->stream);
                sync_wait_group_done(&sc->wg);
            }
        }
    }
    sync_mutex_unlock(&rws->close_notifier_mu);
    return ch;
}

/* responseWriter.Header. */
static HttpHeader h2s_rw_header(void *self) {
    h2s_Rws *rws = h2s_rw_state(self, "Header");
    if (rws->handler_header == NULL)
        rws->handler_header = http_header_make(arena_allocator(&rws->arena));
    return rws->handler_header;
}

/* responseWriter.WriteHeader. */
static void h2s_rw_write_header(void *self, Int code) {
    h2s_rws_write_header(h2s_rw_state(self, "WriteHeader"), code);
}

/* responseWriter.Write. */
static Int h2s_rw_write(void *self, Slice p, Error *err) {
    *err = BURROW_NO_ERROR;
    h2s_Rws *rws = h2s_rw_state(self, "Write");
    if (!rws->wrote_header)
        h2s_rws_write_header(rws, 200);
    if (!burrow__http_body_allowed_for_status(rws->status)) {
        *err = http_err_body_not_allowed;
        return 0;
    }
    rws->wrote_bytes += (int64_t)p.len;
    if (rws->sent_content_len != 0 && rws->wrote_bytes > rws->sent_content_len) {
        /* TODO: send a RST_STREAM */
        *err = burrow__http2_err_handler_wrote_too_much;
        return 0;
    }
    return bufio_writer_write(rws->bw, p, err);
}

static h2s_Stream *h2s_rw_stream(void *self) {
    h2s_Rws *rws = ((h2s_Rw *)self)->rws;
    if (rws == NULL)
        panic_str(BURROW_S(
            "runtime error: invalid memory address or nil pointer dereference"));
    return rws->stream;
}

/* The function SetReadDeadline and SetWriteDeadline send to the serve
 * goroutine. The message holds st. */
static void h2s_send_deadline(h2s_Stream *st, h2s_MsgKind kind, Time deadline) {
    h2s_Msg m = h2s_msg(kind);
    m.st = st;
    m.deadline = deadline;
    h2s_stream_ref(st);
    h2s_send_serve_msg(st->sc, m);
}

/* responseWriter.SetReadDeadline. */
static Error h2s_rw_set_read_deadline(void *self, Time deadline) {
    h2s_Stream *st = h2s_rw_stream(self);
    if (!time_is_zero(deadline) && time_before(deadline, time_now())) {
        /* If we're setting a deadline in the past, reset the stream
         * immediately so writes after SetWriteDeadline returns will fail. */
        h2s_on_read_timeout(st);
        return BURROW_NO_ERROR;
    }
    h2s_send_deadline(st, H2S_MSG_READ_DEADLINE, deadline);
    return BURROW_NO_ERROR;
}

/* responseWriter.SetWriteDeadline. */
static Error h2s_rw_set_write_deadline(void *self, Time deadline) {
    h2s_Stream *st = h2s_rw_stream(self);
    if (!time_is_zero(deadline) && time_before(deadline, time_now())) {
        /* If we're setting a deadline in the past, reset the stream
         * immediately so writes after SetWriteDeadline returns will fail. */
        h2s_on_write_timeout(st);
        return BURROW_NO_ERROR;
    }
    h2s_send_deadline(st, H2S_MSG_WRITE_DEADLINE, deadline);
    return BURROW_NO_ERROR;
}

static Error h2s_rw_enable_full_duplex(void *self) {
    /* We always support full duplex responses, so this is a no-op. */
    (void)self;
    return BURROW_NO_ERROR;
}

static const Type h2s_rw_desc = {{(const Byte *)"responseWriter", 14},
                                 {(const Byte *)"net/http/internal/http2", 23},
                                 KIND_STRUCT,
                                 (uint32_t)sizeof(h2s_Rw),
                                 (uint16_t)_Alignof(h2s_Rw),
                                 0,
                                 0,
                                 NULL,
                                 NULL,
                                 NULL,
                                 NULL,
                                 0,
                                 0x5b17e2c9U,
                                 NULL};

/* No Hijack, which HTTP/2 can't do, and no Unwrap. */
static const HttpResponseWriterVT h2s_rw_vt = {
    {&h2s_rw_desc, h2s_rw_write},
    h2s_rw_header,
    h2s_rw_write_header,
    h2s_rw_flush,
    NULL,
    h2s_rw_set_read_deadline,
    h2s_rw_set_write_deadline,
    h2s_rw_enable_full_duplex,
    h2s_rw_close_notify,
    NULL,
};

/* responseWriter.handlerDone. */
static void h2s_rw_handler_done(h2s_Rw *w) {
    h2s_Rws *rws = w->rws;
    rws->handler_done = true;
    (void)h2s_rw_flush(w);
    w->rws = NULL;
}

/* --------------------------------------------------------------- handlers */

static void h2s_run_handler(void *env);

static bool h2s_go_handler(h2s_Conn *sc, h2s_Stream *st) {
    sync_wait_group_add(&sc->wg, 1);
    Func fn;
    fn.f = h2s_run_handler;
    fn.env = st;
    if (go(fn))
        return true;
    sync_wait_group_done(&sc->wg);
    return false;
}

/* A handler that could not be started: the stream is reset as a handler
 * panic would. */
static void h2s_handler_not_started(h2s_Conn *sc, h2s_Stream *st) {
    sc->cur_handlers--;
    h2s_write_frame(
        sc, h2s_wr_make(burrow__http2_write_handler_panic_rst(st->base.id), st, NULL));
    h2s_stream_unref(st);
}

/* scheduleHandler. The handler holds st from here until the serve goroutine
 * hears it is done. */
static Error h2s_schedule_handler(h2s_Conn *sc, h2s_Stream *st) {
    uint32_t max_handlers = sc->adv_max_streams;
    if (sc->cur_handlers < max_handlers) {
        sc->cur_handlers++;
        h2s_stream_ref(st);
        if (!h2s_go_handler(sc, st))
            h2s_handler_not_started(sc, st);
        return BURROW_NO_ERROR;
    }
    if (sc->nunstarted > (Int)(4 * (int64_t)sc->adv_max_streams))
        return h2s_conn_err(sc, "too_many_early_resets",
                            HTTP2_ERR_CODE_ENHANCE_YOUR_CALM);
    if (sc->nunstarted == sc->unstarted_cap) {
        Int cap = sc->unstarted_cap == 0 ? 8 : sc->unstarted_cap * 2;
        h2s_Unstarted *n = (h2s_Unstarted *)mem_alloc(sc->a, (size_t)cap * sizeof *n,
                                                      _Alignof(h2s_Unstarted));
        if (n == NULL)
            return h2s_conn_err(sc, "out_of_memory", HTTP2_ERR_CODE_INTERNAL);
        for (Int i = 0; i < sc->nunstarted; i++)
            n[i] = sc->unstarted[i];
        if (sc->unstarted != NULL)
            mem_free(sc->a, sc->unstarted, (size_t)sc->unstarted_cap * sizeof *n,
                     _Alignof(h2s_Unstarted));
        sc->unstarted = n;
        sc->unstarted_cap = cap;
    }
    h2s_stream_ref(st);
    sc->unstarted[sc->nunstarted++].st = st;
    return BURROW_NO_ERROR;
}

/* handlerDone. */
static void h2s_handler_done(h2s_Conn *sc) {
    sc->cur_handlers--;
    Int i = 0;
    uint32_t max_handlers = sc->adv_max_streams;
    for (; i < sc->nunstarted; i++) {
        h2s_Stream *st = sc->unstarted[i].st;
        if (h2s_stream_lookup(sc, st->base.id) != st) {
            /* This stream was reset before its goroutine had a chance to
             * start. */
            h2s_stream_unref(st);
            continue;
        }
        if (sc->cur_handlers >= max_handlers)
            break;
        sc->cur_handlers++;
        if (!h2s_go_handler(sc, st))
            h2s_handler_not_started(sc, st);
    }
    Int left = sc->nunstarted - i;
    for (Int j = 0; j < left; j++)
        sc->unstarted[j] = sc->unstarted[i + j];
    sc->nunstarted = left;
}

/* handleHeaderListTooLong. */
static void h2s_handle_header_list_too_long(HttpResponseWriter w) {
    /* 10.5.1 Limits on Header Block Size: .. "A server that receives a
     * larger header block than it is willing to handle can send an HTTP 431
     * (Request Header Fields Too Large) status code" */
    http_response_writer_write_header(w, 431);
    Error err;
    (void)io_write_string(
        http_response_writer_as_io_writer(w),
        BURROW_S("<h1>HTTP Error 431</h1><p>Request Header Field(s) Too "
                 "Large</p>"),
        &err);
}

/* new400Handler. */
static void h2s_handle_400(HttpResponseWriter w, Str msg) {
    HttpHeader h = http_response_writer_header(w);
    http_header_del(h, BURROW_S("Content-Length"));
    (void)http_header_set(h, BURROW_S("Content-Type"),
                          BURROW_S("text/plain; charset=utf-8"));
    (void)http_header_set(h, BURROW_S("X-Content-Type-Options"), BURROW_S("nosniff"));
    http_response_writer_write_header(w, 400);
    Error err;
    IoWriter iw = http_response_writer_as_io_writer(w);
    (void)io_write_string(iw, msg, &err);
    (void)io_write_string(iw, BURROW_S("\n"), &err);
}

static void h2s_serve_handler(h2s_Stream *st, HttpResponseWriter w) {
    switch (st->handler_kind) {
    case H2S_HANDLER_431:
        h2s_handle_header_list_too_long(w);
        break;
    case H2S_HANDLER_400:
        h2s_handle_400(w, st->handler_msg);
        break;
    case H2S_HANDLER_SERVER:
    default:
        burrow__http_server_handler_serve(st->sc->srv, w, st->req);
        break;
    }
}

/* A copy of a caught panic's value that outlives the frame that caught it. */
static Any h2s_keep_panic(burrow__PanicValue *storage, Any v) {
    if (v.t == NULL || v.data == NULL || v.t->size > sizeof storage->bytes)
        return v;
    type_copy(v.t, storage->bytes, v.data);
    v.data = storage->bytes;
    return v;
}

/* Runs the handler, and true with the value in *out when it panicked. */
static bool h2s_call_handler(h2s_Stream *st, burrow__PanicValue *storage, Any *out) {
    volatile bool panicked = false;
    BURROW_TRY {
        HttpResponseWriter w;
        w.vt = &h2s_rw_vt;
        w.data = &st->rw;
        h2s_serve_handler(st, w);
    }
    BURROW_CATCH(p) {
        *out = h2s_keep_panic(storage, p);
        panicked = true;
    }
    BURROW_TRY_END;
    return panicked;
}

static bool h2s_is_abort_handler(Any p) {
    if (p.t != TYPE_ERROR || p.data == NULL)
        return false;
    const Error *e = (const Error *)p.data;
    return e->vt == http_err_abort_handler.vt && e->data == http_err_abort_handler.data;
}

static void h2s_log_panic(h2s_Conn *sc, Any p) {
    enum { size = 64 << 10 };
    Alloc *h = heap_allocator();
    Byte *buf = (Byte *)mem_alloc(h, size, 1);
    Int n =
        buf == NULL ? 0 : runtime_stack(slice_from(buf, size, size, TYPE_BYTE), false);
    h2s_logf(sc, "http2: panic serving %v: %v\n%s", sc->remote_addr, panic_text(p),
             str_from_bytes(buf, n));
    mem_free(h, buf, size, 1);
}

/* runHandler, on its own goroutine. */
static void h2s_run_handler(void *env) {
    h2s_Stream *st = (h2s_Stream *)env;
    h2s_Conn *sc = st->sc;
    burrow__PanicValue storage;
    Any e;
    memset(&e, 0, sizeof e);
    bool did_panic = h2s_call_handler(st, &storage, &e);
    BURROW_CALLF0(st->cancel_ctx);
    if (st->req->multipart_form != NULL)
        (void)multipart_form_remove_all(st->req->multipart_form);
    if (did_panic) {
        (void)h2s_write_frame_from_handler(
            sc,
            h2s_wr_make(burrow__http2_write_handler_panic_rst(st->base.id), st, NULL));
        /* Same as net/http: */
        if (e.t != NULL && !h2s_is_abort_handler(e))
            h2s_log_panic(sc, e);
    } else {
        h2s_rw_handler_done(&st->rw);
    }
    h2s_Msg m = h2s_msg(H2S_MSG_HANDLER_DONE);
    m.st = st; /* the handler's hold, which the serve goroutine lets go of */
    h2s_send_serve_msg(sc, m);
    sync_wait_group_done(&sc->wg);
}

/* ------------------------------------------------------------- the serve loop */

/* handlePingTimer. */
static void h2s_handle_ping_timer(h2s_Conn *sc) {
    if (sc->ping_sent) {
        h2s_logf(sc, "timeout waiting for PING response");
        if (!BURROW_FUNC_IS_NIL(sc->conf.count_error))
            BURROW_CALLF(sc->conf.count_error, BURROW_S("conn_close_lost_ping"));
        (void)sc->conn.vt->closer.close(sc->conn.data);
        return;
    }

    Time ping_at = time_add(sc->last_frame_time, sc->read_idle_timeout);
    Time now = time_now();
    if (time_after(ping_at, now)) {
        /* We received frames since arming the ping timer. Reset it for the
         * next possible timeout. */
        (void)h2s_timer_arm(sc, &sc->read_idle_timer, time_sub(ping_at, now));
        return;
    }

    sc->ping_sent = true;
    /* Ignore crypto/rand.Read errors: It generally can't fail, and worse
     * case if it does is we send a PING frame containing 0s. */
    ArenaMark m = error_mark();
    Error err = BURROW_NO_ERROR;
    (void)crypto_rand_read(slice_from(sc->sent_ping_data, 8, 8, TYPE_BYTE), &err);
    error_release(m);
    h2s_write_frame(
        sc, h2s_wr_make(burrow__http2_write_ping(sc->sent_ping_data), NULL, NULL));
    (void)h2s_timer_arm(sc, &sc->read_idle_timer, sc->conf.ping_timeout);
}

/* maxHeaderListSize. */
static uint32_t h2s_max_header_list_size(const h2s_Conn *sc) {
    int64_t n = sc->srv->max_header_bytes;
    if (n <= 0)
        n = HTTP_DEFAULT_MAX_HEADER_BYTES;
    /* http2's count is in a slightly different unit and includes 32 bytes
     * per pair. So, take the net/http.Server value and pad it up a bit,
     * assuming 10 headers. */
    return (uint32_t)(n + (int64_t)10 * 32);
}

static void h2s_serve_msg(h2s_Conn *sc, h2s_Msg m, bool *stop) {
    switch (m.kind) {
    case H2S_MSG_SETTINGS_TIMER:
        h2s_logf(sc, "timeout waiting for SETTINGS frames from %v", sc->remote_addr);
        *stop = true;
        break;
    case H2S_MSG_IDLE_TIMER:
        h2s_vlogf(sc, "connection is idle");
        h2s_go_away(sc, HTTP2_ERR_CODE_NO);
        break;
    case H2S_MSG_READ_IDLE_TIMER:
        h2s_handle_ping_timer(sc);
        break;
    case H2S_MSG_SHUTDOWN_TIMER:
        h2s_vlogf(sc, "GOAWAY close timer fired; closing conn from %v",
                  sc->remote_addr);
        *stop = true;
        break;
    case H2S_MSG_GRACEFUL:
        /* startGracefulShutdownInternal */
        h2s_go_away(sc, HTTP2_ERR_CODE_NO);
        break;
    case H2S_MSG_HANDLER_DONE:
        h2s_handler_done(sc);
        break;
    case H2S_MSG_READ_DEADLINE:
        h2s_stream_apply_deadline(m.st, &m.st->read_deadline, &m.st->read_deadline_live,
                                  m.deadline, h2s_read_deadline_fire);
        break;
    case H2S_MSG_WRITE_DEADLINE:
        h2s_stream_apply_deadline(m.st, &m.st->write_deadline,
                                  &m.st->write_deadline_live, m.deadline,
                                  h2s_write_deadline_fire);
        break;
    default:
        panic_str(BURROW_S("unknown timer"));
    }
    if (m.st != NULL)
        h2s_stream_unref(m.st);
}

/* One turn of the serve loop. false ends it. */
static bool h2s_serve_once(h2s_Conn *sc, bool *settings_timer_live) {
    h2s_ReadFrameResult res;
    Http2FrameWriteRequest wr;
    h2s_FrameWriteResult wres;
    h2s_BodyReadMsg brm;
    h2s_Msg msg;
    SelectCase cases[] = {
        BURROW_RECV(sc->want_write_frame_ch, &wr),
        BURROW_RECV(sc->wrote_frame_ch, &wres),
        BURROW_RECV(sc->read_frame_ch, &res),
        BURROW_RECV(sc->body_read_ch, &brm),
        BURROW_RECV(sc->serve_msg_ch, &msg),
    };
    bool stop = false;
    switch (chan_select(cases, 5)) {
    case 0:
        if (wr.write.kind == HTTP2_WRITE_STREAM_ERROR) {
            Http2StreamError se = wr.write.u.stream_error;
            h2s_wr_release(sc, &wr);
            h2s_reset_stream(sc, se);
            break;
        }
        h2s_write_frame(sc, wr);
        break;
    case 1:
        h2s_wrote_frame(sc, wres);
        break;
    case 2: {
        sc->last_frame_time = time_now();
        /* Process any written frames before reading new frames from the
         * client since a written frame could have triggered a new stream to
         * be started. */
        if (sc->writing_frame_async && chan_try_recv(sc->wrote_frame_ch, &wres, NULL))
            h2s_wrote_frame(sc, wres);
        bool more = h2s_process_frame_from_reader(sc, res);
        burrow__http2_frame_free(res.f);
        if (!more)
            return false;
        bool t = true;
        chan_send(sc->read_gate, &t); /* readMore */
        if (*settings_timer_live) {
            h2s_timer_stop(sc, &sc->settings_timer);
            *settings_timer_live = false;
        }
        break;
    }
    case 3:
        h2s_note_body_read(sc, brm.st, brm.n);
        h2s_stream_unref(brm.st);
        break;
    default:
        h2s_serve_msg(sc, msg, &stop);
        if (stop)
            return false;
        break;
    }

    /* If the peer is causing us to generate a lot of control frames, but
     * not reading them from us, assume they are trying to make us run out
     * of memory. */
    if (sc->queued_control_frames > H2S_MAX_QUEUED_CONTROL_FRAMES) {
        h2s_vlogf(sc,
                  "http2: too many control frames in send queue, closing connection");
        return false;
    }

    /* Start the shutdown timer after sending a GOAWAY. When sending GOAWAY
     * with no error code (graceful shutdown), don't start the timer until
     * all open streams have been completed. */
    bool sent_go_away =
        sc->in_go_away && !sc->need_to_send_go_away && !sc->writing_frame;
    bool graceful_shutdown_complete =
        sc->go_away_code == HTTP2_ERR_CODE_NO && h2s_cur_open_streams(sc) == 0;
    if (sent_go_away && sc->shutdown_timer.t == NULL &&
        (sc->go_away_code != HTTP2_ERR_CODE_NO || graceful_shutdown_complete))
        (void)h2s_timer_arm(sc, &sc->shutdown_timer,
                            H2S_GO_AWAY_TIMEOUT); /* shutDownIn */
    return true;
}

static bool h2s_go_reader(h2s_Conn *sc) {
    sync_wait_group_add(&sc->io_wg, 1);
    Func fn;
    fn.f = h2s_read_frames;
    fn.env = sc;
    if (go(fn))
        return true;
    sync_wait_group_done(&sc->io_wg);
    return false;
}

/* serve, up to the loop's end. Teardown is the caller's. */
static void h2s_serve(h2s_Conn *sc) {
    h2s_vlogf(sc, "http2: server connection from %v on %p", sc->remote_addr,
              (void *)sc->srv);

    Int n = 0;
    Http2Setting *s = sc->settings;
    s[n].id = HTTP2_SETTING_MAX_FRAME_SIZE;
    s[n++].val = (uint32_t)sc->conf.max_read_frame_size;
    s[n].id = HTTP2_SETTING_MAX_CONCURRENT_STREAMS;
    s[n++].val = sc->adv_max_streams;
    s[n].id = HTTP2_SETTING_MAX_HEADER_LIST_SIZE;
    s[n++].val = h2s_max_header_list_size(sc);
    s[n].id = HTTP2_SETTING_HEADER_TABLE_SIZE;
    s[n++].val = (uint32_t)sc->conf.max_decoder_header_table_size;
    s[n].id = HTTP2_SETTING_INITIAL_WINDOW_SIZE;
    s[n++].val = (uint32_t)sc->initial_stream_recv_window_size;
    if (!burrow__http2_extended_connect_disabled()) {
        s[n].id = HTTP2_SETTING_ENABLE_CONNECT_PROTOCOL;
        s[n++].val = 1;
    }
    if (h2s_write_sched_ignores_rfc7540(sc)) {
        s[n].id = HTTP2_SETTING_NO_RFC7540_PRIORITIES;
        s[n++].val = 1;
    }
    sc->nsettings = n;
    h2s_write_frame(sc, h2s_wr_make(burrow__http2_write_settings(s, n), NULL, NULL));
    sc->unacked_settings++;

    /* Each connection starts with initialWindowSize inflow tokens. If a
     * higher value is configured, we add more tokens. */
    int64_t diff = sc->conf.max_receive_buffer_per_connection - H2S_INITIAL_WINDOW_SIZE;
    if (diff > 0)
        h2s_send_window_update(sc, NULL, (Int)diff);

    Error err = h2s_read_preface(sc);
    if (BURROW_FAILED(err)) {
        if (h2s_quiet_error(err))
            h2s_vlogf(sc, "http2: server: error reading preface from client %v: %v",
                      sc->remote_addr, err);
        else
            h2s_logf(sc, "http2: server: error reading preface from client %v: %v",
                     sc->remote_addr, err);
        return;
    }
    /* Now that we've got the preface, get us out of the "StateNew" state.
     * We can't go directly to idle, though. Active means we read some data
     * and anticipate a request. We'll do another Active when we get a
     * HEADERS frame. */
    h2s_set_conn_state(sc, HTTP_STATE_ACTIVE);
    h2s_set_conn_state(sc, HTTP_STATE_IDLE);

    Duration idle = h2s_idle_timeout(sc);
    if (idle > 0)
        (void)h2s_timer_arm(sc, &sc->idle_timer, idle);

    if (sc->conf.send_ping_timeout > 0) {
        sc->read_idle_timeout = sc->conf.send_ping_timeout;
        (void)h2s_timer_arm(sc, &sc->read_idle_timer, sc->conf.send_ping_timeout);
    }

    if (!h2s_go_reader(sc))
        return;

    bool settings_timer_live =
        h2s_timer_arm(sc, &sc->settings_timer, H2S_FIRST_SETTINGS_TIMEOUT);

    sc->last_frame_time = time_now();
    for (;;) {
        ArenaMark m = error_mark();
        bool more = h2s_serve_once(sc, &settings_timer_live);
        error_release(m);
        if (!more)
            return;
    }
}

static void h2s_stop_timers(h2s_Conn *sc) {
    h2s_timer_stop(sc, &sc->settings_timer);
    h2s_timer_stop(sc, &sc->read_idle_timer);
    h2s_timer_stop(sc, &sc->idle_timer);
}

/* closeAllStreamsOnConnClose. The ids are taken first, since closing a
 * stream takes it out of the map. */
static void h2s_close_all_streams(h2s_Conn *sc) {
    Int n = map_len(sc->streams);
    if (n == 0)
        return;
    uint32_t *ids =
        (uint32_t *)mem_alloc(sc->a, (size_t)n * sizeof *ids, _Alignof(uint32_t));
    if (ids == NULL)
        return;
    Int i = 0;
    MapIter it = map_iter(sc->streams);
    const void *k;
    void *v;
    while (i < n && map_next(&it, &k, &v))
        ids[i++] = *(const uint32_t *)k;
    for (Int j = 0; j < i; j++) {
        h2s_Stream *st = h2s_stream_lookup(sc, ids[j]);
        if (st != NULL)
            h2s_close_stream(sc, st, burrow__http2_err_client_disconnected);
    }
    mem_free(sc->a, ids, (size_t)n * sizeof *ids, _Alignof(uint32_t));
}

/* What the channels still hold once every goroutine is gone. */
static void h2s_drain(h2s_Conn *sc) {
    Http2FrameWriteRequest wr;
    while (chan_try_recv(sc->want_write_frame_ch, &wr, NULL))
        h2s_wr_release(sc, &wr);
    h2s_FrameWriteResult wres;
    while (chan_try_recv(sc->wrote_frame_ch, &wres, NULL))
        h2s_wr_release(sc, &wres.wr);
    h2s_Msg m;
    while (chan_try_recv(sc->serve_msg_ch, &m, NULL)) {
        if (m.st != NULL)
            h2s_stream_unref(m.st);
    }
    h2s_ReadFrameResult res;
    while (chan_try_recv(sc->read_frame_ch, &res, NULL))
        burrow__http2_frame_free(res.f);
    for (Int i = 0; i < sc->nunstarted; i++)
        h2s_stream_unref(sc->unstarted[i].st);
    sc->nunstarted = 0;
}

static void h2s_register(h2s_Conn *sc) {
    HttpServer *s = sc->srv;
    sync_mutex_lock(&s->h2_mu);
    sc->prev = NULL;
    sc->next = s->h2_conns;
    if (s->h2_conns != NULL)
        s->h2_conns->prev = sc;
    s->h2_conns = sc;
    sync_mutex_unlock(&s->h2_mu);
}

static void h2s_unregister(h2s_Conn *sc) {
    HttpServer *s = sc->srv;
    sync_mutex_lock(&s->h2_mu);
    if (sc->prev != NULL)
        sc->prev->next = sc->next;
    else
        s->h2_conns = sc->next;
    if (sc->next != NULL)
        sc->next->prev = sc->prev;
    sync_mutex_unlock(&s->h2_mu);
}

static void h2s_conn_free(h2s_Conn *sc) {
    Alloc *a = sc->a;
    if (sc->write_sched.vt != NULL)
        burrow__http2_write_scheduler_free(sc->write_sched);
    if (sc->framer != NULL)
        burrow__http2_framer_free(sc->framer);
    if (sc->hdec != NULL)
        burrow__hpack_decoder_free(sc->hdec);
    if (sc->hpack_encoder != NULL)
        burrow__hpack_encoder_free(sc->hpack_encoder);
    bytes_buffer_free(&sc->header_write_buf);
    if (sc->bw.bw != NULL)
        bufio_writer_free(sc->bw.bw);
    h2s_Timer *timers[] = {&sc->idle_timer, &sc->read_idle_timer, &sc->settings_timer,
                           &sc->shutdown_timer};
    for (size_t i = 0; i < sizeof timers / sizeof timers[0]; i++)
        time_timer_free(timers[i]->t);
    Chan *chans[] = {sc->done_serving,        sc->read_frame_ch,  sc->read_gate,
                     sc->want_write_frame_ch, sc->wrote_frame_ch, sc->body_read_ch,
                     sc->serve_msg_ch,        sc->preface_ch};
    for (size_t i = 0; i < sizeof chans / sizeof chans[0]; i++) {
        if (chans[i] != NULL)
            chan_free(chans[i]);
    }
    if (sc->streams != NULL)
        map_free(sc->streams);
    if (sc->unstarted != NULL)
        mem_free(a, sc->unstarted, (size_t)sc->unstarted_cap * sizeof *sc->unstarted,
                 _Alignof(h2s_Unstarted));
    if (!BURROW_FUNC_IS_NIL(sc->cancel_ctx))
        BURROW_CALLF0(sc->cancel_ctx);
    if (sc->base_ctx.vt != NULL)
        context_release(sc->base_ctx);
    arena_free(&sc->earena);
    mem_free(a, sc, sizeof *sc, _Alignof(h2s_Conn));
}

/* The setup in serveConn after the struct, false when memory ran out. */
static bool h2s_setup(h2s_Conn *sc) {
    Alloc *a = sc->a;
    sc->done_serving = chan_make(a, TYPE_BOOL, 0);
    sc->read_frame_ch = chan_make(a, &h2s_read_frame_result_desc, 0);
    sc->read_gate = chan_make(a, TYPE_BOOL, 1);
    sc->want_write_frame_ch = chan_make(a, &h2s_frame_write_request_desc, 8);
    sc->wrote_frame_ch = chan_make(a, &h2s_frame_write_result_desc, 1);
    sc->body_read_ch = chan_make(a, &h2s_body_read_msg_desc, 0);
    sc->serve_msg_ch = chan_make(a, &h2s_msg_desc, 8);
    sc->preface_ch = chan_make(a, TYPE_ERROR, 1);
    sc->streams = map_make(a, TYPE_OF(uint32_t), TYPE_UINTPTR, 0);
    if (sc->done_serving == NULL || sc->read_frame_ch == NULL ||
        sc->read_gate == NULL || sc->want_write_frame_ch == NULL ||
        sc->wrote_frame_ch == NULL || sc->body_read_ch == NULL ||
        sc->serve_msg_ch == NULL || sc->preface_ch == NULL || sc->streams == NULL)
        return false;

    if (sc->srv->disable_client_priority)
        sc->write_sched = burrow__http2_new_round_robin_write_scheduler(a);
    else
        sc->write_sched = burrow__http2_new_priority_write_scheduler_rfc9218(a);
    if (sc->write_sched.vt == NULL)
        return false;
    Http2WriteRefHooks hooks;
    hooks.retain = h2s_sched_retain;
    hooks.drop = h2s_sched_drop;
    hooks.ctx = sc;
    burrow__http2_write_scheduler_set_hooks(sc->write_sched, hooks);

    /* These start at the RFC-specified defaults. If there is a higher
     * configured value for inflow, that will be updated when we send a
     * WINDOW_UPDATE shortly after sending SETTINGS. */
    (void)burrow__http2_outflow_add(&sc->flow, H2S_INITIAL_WINDOW_SIZE);
    burrow__http2_inflow_init(&sc->inflow, H2S_INITIAL_WINDOW_SIZE);
    sc->header_write_buf = BYTES_BUFFER(a);
    sc->hpack_encoder =
        burrow__hpack_new_encoder(a, bytes_buffer_as_io_writer(&sc->header_write_buf));
    if (sc->hpack_encoder == NULL)
        return false;
    burrow__hpack_encoder_set_max_dynamic_table_size_limit(
        sc->hpack_encoder, (uint32_t)sc->conf.max_encoder_header_table_size);

    sc->bw.sc = sc;
    sc->bw.conn = sc->conn;
    sc->bw.byte_timeout = sc->conf.write_byte_timeout;
    IoWriter tw = {&h2s_timeout_writer_vt, &sc->bw};
    sc->bw.bw = bufio_new_writer_size(a, tw, H2S_BUF_WRITER_SIZE);
    if (sc->bw.bw == NULL)
        return false;
    IoWriter w = {&h2s_bw_vt, &sc->bw};
    IoReader r = {&sc->conn.vt->reader, sc->conn.data};
    Http2Framer *fr = burrow__http2_new_framer(a, w, r);
    if (fr == NULL)
        return false;
    sc->framer = fr;
    if (!BURROW_FUNC_IS_NIL(sc->conf.count_error)) {
        fr->count_error.f = sc->conf.count_error.f;
        fr->count_error.env = sc->conf.count_error.env;
    }
    sc->hdec = burrow__hpack_new_decoder(
        a, (uint32_t)sc->conf.max_decoder_header_table_size, NULL, NULL);
    if (sc->hdec == NULL)
        return false;
    fr->read_meta_headers = sc->hdec;
    fr->max_header_list_size = h2s_max_header_list_size(sc);
    fr->max_header_value_count = burrow__http_server_max_header_value_count(sc->srv);
    burrow__http2_framer_set_max_read_frame_size(
        fr, (uint32_t)sc->conf.max_read_frame_size);
    return true;
}

/* Server.ServeConn with the options net/http gives it: serves HTTP/2 on c
 * until the connection is done, with ctx as the base context. */
void burrow__http2_serve_conn(HttpServer *srv, Alloc *a, NetConn c, Context ctx,
                              bool saw_client_preface) {
    h2s_Conn *sc = (h2s_Conn *)mem_alloc(a, sizeof *sc, _Alignof(h2s_Conn));
    if (sc == NULL) {
        (void)c.vt->closer.close(c.data);
        return;
    }
    memset(sc, 0, sizeof *sc);
    sc->a = a;
    sc->srv = srv;
    sc->conn = c;
    arena_init(&sc->earena, a, 0);
    sc->base_ctx = context_with_cancel(a, ctx, &sc->cancel_ctx);
    sc->conf = h2s_config_from_server(srv);
    NetAddr ra = c.vt->remote_addr(c.data);
    if (ra.vt != NULL)
        sc->remote_addr = ra.vt->string(ra.data, arena_allocator(&sc->earena));
    sc->client_max_streams = UINT32_MAX; /* Section 6.5.2: "Initially, there is no limit
                                          * to this value" */
    sc->adv_max_streams = (uint32_t)sc->conf.max_concurrent_streams;
    sc->initial_stream_send_window_size = H2S_INITIAL_WINDOW_SIZE;
    sc->initial_stream_recv_window_size =
        (int32_t)sc->conf.max_receive_buffer_per_stream;
    sc->base.max_frame_size = H2S_INITIAL_MAX_FRAME_SIZE;
    sc->push_enabled = true;
    sc->saw_client_preface = saw_client_preface;
    sc->idle_timer.sc = sc;
    sc->idle_timer.kind = H2S_MSG_IDLE_TIMER;
    sc->read_idle_timer.sc = sc;
    sc->read_idle_timer.kind = H2S_MSG_READ_IDLE_TIMER;
    sc->settings_timer.sc = sc;
    sc->settings_timer.kind = H2S_MSG_SETTINGS_TIMER;
    sc->shutdown_timer.sc = sc;
    sc->shutdown_timer.kind = H2S_MSG_SHUTDOWN_TIMER;

    if (sc->base_ctx.vt == NULL || !h2s_setup(sc)) {
        (void)c.vt->closer.close(c.data);
        h2s_conn_free(sc);
        return;
    }

    h2s_register(sc);
    h2s_serve(sc);

    /* serve's deferred calls, last first. */
    h2s_stop_timers(sc);
    chan_close(sc->done_serving); /* unblocks handlers trying to send */
    h2s_timer_stop(sc, &sc->shutdown_timer);
    h2s_close_all_streams(sc);
    (void)c.vt->closer.close(c.data);

    /* Closing a stream may have armed the idle timer again. Then the
     * goroutines that hold the connection are waited for, which Go leaves
     * to its collector. */
    h2s_stop_timers(sc);
    sync_wait_group_wait(&sc->io_wg);
    sync_wait_group_wait(&sc->wg);
    h2s_drain(sc);
    h2s_unregister(sc);
    h2s_conn_free(sc);
}

/* GracefulShutdown, which net/http runs from Shutdown. */
static void h2s_graceful_shutdown(void *env) {
    HttpServer *s = (HttpServer *)env;
    sync_mutex_lock(&s->h2_mu);
    for (h2s_Conn *sc = s->h2_conns; sc != NULL; sc = sc->next)
        h2s_start_graceful_shutdown(sc);
    sync_mutex_unlock(&s->h2_mu);
}

/* configureHTTP2, without the TLS parts. False when the shutdown hook could
 * not be added. */
bool burrow__http2_configure_server(HttpServer *s) {
    sync_mutex_lock(&s->h2_mu);
    bool ok = true;
    if (!s->h2_hooked) {
        Func fn;
        fn.f = h2s_graceful_shutdown;
        fn.env = s;
        ok = http_server_register_on_shutdown(s, fn);
        s->h2_hooked = ok;
    }
    sync_mutex_unlock(&s->h2_mu);
    return ok;
}

bool burrow__http2_server_configured(HttpServer *s) {
    sync_mutex_lock(&s->h2_mu);
    bool ok = s->h2_hooked;
    sync_mutex_unlock(&s->h2_mu);
    return ok;
}
