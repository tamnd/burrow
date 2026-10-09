/* Derived from net/http/internal/http2's server_test.go in Go 1.27.1: the
 * HTTP/2 server, driven frame by frame from the client's side.
 *
 * Go's tester runs the server over a fake connection inside a synctest
 * bubble. This one serves the server's end of a net_pipe with
 * http_server_serve and UnencryptedHTTP2 on, so every connection goes
 * through net/http's check for the h2c preface first, and plays the client
 * on the other end with a Framer of its own. There is no bubble, so where Go
 * waits for the server to go quiet these wait for the handler to say it has
 * started. Go's tests that look inside the server's state, or move a fake
 * clock along, are not here.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http2.h"
#include "../src/xnet/hpack.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/os.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FATALF(...)                                                                    \
    do {                                                                               \
        testing_t_fatalf_v(t, __VA_ARGS__);                                            \
        return;                                                                        \
    } while (0)

typedef void (*H2tHandlerFn)(void *env, HttpResponseWriter w, HttpRequest *r);

/* ------------------------------------------------- a listener of one conn */

typedef struct H2tListener {
    NetConn conn;
    bool done;
} H2tListener;

static NetConn h2t_accept(void *self, Error *err) {
    H2tListener *l = (H2tListener *)self;
    NetConn none = {NULL, NULL};
    if (l->done) {
        *err = io_eof;
        return none;
    }
    l->done = true;
    *err = BURROW_NO_ERROR;
    return l->conn;
}

static Error h2t_listener_close(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static NetAddr h2t_listener_addr(void *self) {
    H2tListener *l = (H2tListener *)self;
    return l->conn.vt->local_addr(l->conn.data);
}

static const NetListenerVT h2t_listener_vt = {
    {NULL, h2t_listener_close}, h2t_accept, h2t_listener_addr};

/* ---------------------------------------------------------- the tester */

/* serverTester. */
typedef struct H2tTester {
    TestingT *t;
    Alloc *a;
    HttpServer srv;
    HttpProtocols protos;
    HttpHandlerFunc hf;
    LogLogger *lg;
    NetConn sc;
    NetConn cc;
    H2tListener ol;
    Error serve_err;
    SyncWaitGroup wg;
    Http2Framer *fr;
    HpackDecoder *dec;
    HpackEncoder *enc;
    BytesBuffer hbuf;
    /* What the client writes, which a goroutine of its own passes on to cc.
     * Go's fake connection buffers writes, so a test can write a frame the
     * server stops reading partway through, as it does when the frame header
     * alone is enough to end the connection, and still go on to read the
     * GOAWAY. A net_pipe write waits for the reader instead. */
    SyncMutex wmu;
    SyncCond wcond;
    BytesBuffer wbuf;
    bool wclosed;
    bool wfailed;
    SyncWaitGroup wwg;
} H2tTester;

static Int h2t_out_write(void *self, Slice p, Error *err) {
    H2tTester *st = (H2tTester *)self;
    sync_mutex_lock(&st->wmu);
    if (st->wfailed || st->wclosed) {
        sync_mutex_unlock(&st->wmu);
        *err = io_err_closed_pipe;
        return 0;
    }
    Int n = bytes_buffer_write(&st->wbuf, p, err);
    sync_cond_signal(&st->wcond);
    sync_mutex_unlock(&st->wmu);
    return n;
}

static const IoWriterVT h2t_out_vt = {NULL, h2t_out_write};

/* Passes wbuf on to cc until the tester closes and it is empty, or a write
 * to cc fails. */
static void h2t_out_job(void *env) {
    H2tTester *st = (H2tTester *)env;
    enum { CHUNK = 16 << 10 };
    Byte *chunk = (Byte *)mem_alloc_nozero(st->a, CHUNK, 1);
    sync_mutex_lock(&st->wmu);
    if (chunk == NULL)
        st->wfailed = true;
    while (!st->wfailed) {
        while (bytes_buffer_len(&st->wbuf) == 0 && !st->wclosed)
            sync_cond_wait(&st->wcond);
        if (bytes_buffer_len(&st->wbuf) == 0)
            break;
        Error err = BURROW_NO_ERROR;
        Int n = bytes_buffer_read(&st->wbuf, slice_from(chunk, CHUNK, CHUNK, TYPE_BYTE),
                                  &err);
        sync_mutex_unlock(&st->wmu);
        (void)st->cc.vt->writer.write(st->cc.data, slice_from(chunk, n, n, TYPE_BYTE),
                                      &err);
        sync_mutex_lock(&st->wmu);
        if (BURROW_FAILED(err))
            st->wfailed = true;
    }
    sync_mutex_unlock(&st->wmu);
    if (chunk != NULL)
        mem_free(st->a, chunk, CHUNK, 1);
}

static void h2t_nop_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)w;
    (void)r;
}

static void h2t_serve_job(void *env) {
    H2tTester *st = (H2tTester *)env;
    NetListener l = {&h2t_listener_vt, &st->ol};
    st->serve_err = http_server_serve(&st->srv, l);
}

/* newServerTester. The server's log goes nowhere, as optQuiet has it. */
/* newServerTester, with h2 as the server's HTTP/2 config, which can be NULL
 * and has to last until h2t_close, and max_header_bytes as its
 * MaxHeaderBytes. */
static bool h2t_start_server(H2tTester *st, TestingT *t, H2tHandlerFn fn, void *env,
                             const HttpHTTP2Config *h2, Int max_header_bytes) {
    memset(st, 0, sizeof *st);
    st->t = t;
    st->a = heap_allocator();
    st->hbuf = BYTES_BUFFER(st->a);
    http_protocols_set_http1(&st->protos, true);
    http_protocols_set_unencrypted_http2(&st->protos, true);
    st->srv.protocols = &st->protos;
    st->srv.http2 = h2;
    st->srv.max_header_bytes = max_header_bytes;
    st->hf = BURROW_FN(HttpHandlerFunc, fn != NULL ? fn : h2t_nop_handler, env);
    st->srv.handler = http_handler_func_as_handler(&st->hf);
    st->lg = log_new(st->a, io_discard, BURROW_STR_EMPTY, 0);
    st->srv.error_log = st->lg;
    net_pipe(st->a, &st->sc, &st->cc);
    if (st->lg == NULL || st->sc.data == NULL) {
        testing_t_errorf_v(t, "no memory for the tester");
        return false;
    }
    st->ol.conn = st->sc;
    (void)sync_wait_group_go(&st->wg, BURROW_FN(Func, h2t_serve_job, st));
    st->wcond = SYNC_COND(sync_mutex_locker(&st->wmu));
    st->wbuf = BYTES_BUFFER(st->a);
    if (!sync_wait_group_go(&st->wwg, BURROW_FN(Func, h2t_out_job, st))) {
        st->wfailed = true;
        testing_t_errorf_v(t, "no goroutine for the client's writes");
        return false;
    }

    IoWriter w = {&h2t_out_vt, st};
    IoReader r = {&st->cc.vt->reader, st->cc.data};
    st->fr = burrow__http2_new_framer(st->a, w, r);
    st->dec =
        burrow__hpack_new_decoder(st->a, HTTP2_INITIAL_HEADER_TABLE_SIZE, NULL, NULL);
    st->enc = burrow__hpack_new_encoder(st->a, bytes_buffer_as_io_writer(&st->hbuf));
    if (st->fr == NULL || st->dec == NULL || st->enc == NULL) {
        testing_t_errorf_v(t, "no memory for the client");
        return false;
    }
    st->fr->read_meta_headers = st->dec;
    (void)st->cc.vt->set_read_deadline(st->cc.data,
                                       time_add(time_now(), 10 * TIME_SECOND));
    return true;
}

static bool h2t_start_config(H2tTester *st, TestingT *t, H2tHandlerFn fn, void *env,
                             const HttpHTTP2Config *h2) {
    return h2t_start_server(st, t, fn, env, h2, 0);
}

static bool h2t_start(H2tTester *st, TestingT *t, H2tHandlerFn fn, void *env) {
    return h2t_start_config(st, t, fn, env, NULL);
}

/* Close: hangs up, which ends the connection, and waits for the server to
 * be done with it. */
static void h2t_close(H2tTester *st) {
    if (st->cc.data != NULL) {
        /* What was written goes first, as it would from Go's fake conn, but
         * not forever if the server has stopped reading. */
        (void)st->cc.vt->set_write_deadline(st->cc.data,
                                            time_add(time_now(), 10 * TIME_SECOND));
        sync_mutex_lock(&st->wmu);
        st->wclosed = true;
        sync_cond_signal(&st->wcond);
        sync_mutex_unlock(&st->wmu);
        sync_wait_group_wait(&st->wwg);
        (void)st->cc.vt->closer.close(st->cc.data);
    }
    sync_wait_group_wait(&st->wg);
    http_server_free(&st->srv);
    if (st->fr != NULL)
        burrow__http2_framer_free(st->fr);
    if (st->dec != NULL)
        burrow__hpack_decoder_free(st->dec);
    if (st->enc != NULL)
        burrow__hpack_encoder_free(st->enc);
    bytes_buffer_free(&st->hbuf);
    bytes_buffer_free(&st->wbuf);
    if (st->sc.data != NULL)
        net_pipe_free(st->sc);
    if (st->lg != NULL)
        log_logger_free(st->a, st->lg);
}

static bool h2t_failed(H2tTester *st, const char *what, Error err) {
    if (!BURROW_FAILED(err))
        return false;
    testing_t_errorf_v(st->t, "%s: %v", str_from_cstr(what), err);
    return true;
}

static bool h2t_write_preface(H2tTester *st) {
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    Error err = BURROW_NO_ERROR;
    Slice p = slice_from((void *)(uintptr_t)preface, 24, 24, TYPE_BYTE);
    (void)h2t_out_write(st, p, &err);
    return !h2t_failed(st, "writing the preface", err);
}

/* readFrame: the next frame, or NULL and the test failed. */
static Http2Frame *h2t_read_frame(H2tTester *st) {
    Error err = BURROW_NO_ERROR;
    Http2Frame *f = burrow__http2_framer_read_frame(st->fr, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(st->t, "reading a frame: %v", err);
        burrow__http2_frame_free(f);
        return NULL;
    }
    return f;
}

/* The next frame when it is of kind k, or NULL and the test failed. */
static Http2Frame *h2t_read_kind(H2tTester *st, Http2FrameKind k) {
    Http2Frame *f = h2t_read_frame(st);
    if (f != NULL && f->kind != k) {
        testing_t_errorf_v(st->t, "got frame type %d on stream %d, want kind %d",
                           (int)f->header.type, (int)f->header.stream_id, (int)k);
        burrow__http2_frame_free(f);
        return NULL;
    }
    return f;
}

typedef void (*H2tSettingFn)(void *env, Http2Setting s);

/* greetAndCheckSettings: the preface and the SETTINGS both ways, with check
 * called on each setting the server sends when it isn't NULL, then the
 * server's WINDOW_UPDATE and SETTINGS ACK, in either order. */
static bool h2t_greet_and_check_settings(H2tTester *st, H2tSettingFn check, void *env) {
    if (!h2t_write_preface(st))
        return false;
    if (h2t_failed(st, "writing SETTINGS",
                   burrow__http2_framer_write_settings(st->fr, NULL, 0)))
        return false;
    Http2Frame *f = h2t_read_kind(st, HTTP2_SETTINGS_FRAME);
    if (f == NULL)
        return false;
    bool ack = burrow__http2_settings_frame_is_ack(f);
    if (!ack && check != NULL) {
        Int n = burrow__http2_settings_frame_num_settings(f);
        for (Int i = 0; i < n; i++)
            check(env, burrow__http2_settings_frame_setting(f, i));
    }
    burrow__http2_frame_free(f);
    if (ack) {
        testing_t_errorf_v(st->t, "got SETTINGS frame with ACK set, want no ACK");
        return false;
    }
    if (h2t_failed(st, "writing SETTINGS ACK",
                   burrow__http2_framer_write_settings_ack(st->fr)))
        return false;

    bool got_settings_ack = false;
    bool got_window_update = false;
    for (int i = 0; i < 2; i++) {
        f = h2t_read_frame(st);
        if (f == NULL)
            return false;
        if (f->kind == HTTP2_SETTINGS_FRAME) {
            if (!burrow__http2_settings_frame_is_ack(f))
                testing_t_errorf_v(st->t, "Settings Frame didn't have ACK set");
            got_settings_ack = true;
        } else if (f->kind == HTTP2_WINDOW_UPDATE_FRAME) {
            if (f->header.stream_id != 0)
                testing_t_errorf_v(st->t, "WindowUpdate StreamID = %d; want 0",
                                   (int)f->header.stream_id);
            got_window_update = true;
        } else {
            testing_t_errorf_v(st->t,
                               "Wanting a settings ACK or window update, got type %d",
                               (int)f->header.type);
        }
        burrow__http2_frame_free(f);
    }
    if (!got_settings_ack || !got_window_update) {
        testing_t_errorf_v(st->t, "settings ACK %t, window update %t; want both",
                           got_settings_ack, got_window_update);
        return false;
    }
    return true;
}

/* greet. */
static bool h2t_greet(H2tTester *st) {
    return h2t_greet_and_check_settings(st, NULL, NULL);
}

static void h2t_field(H2tTester *st, Str k, Str v) {
    HpackHeaderField hf;
    hf.name = k;
    hf.value = v;
    hf.sensitive = false;
    if (BURROW_FAILED(burrow__hpack_encoder_write_field(st->enc, hf)))
        testing_t_errorf_v(st->t, "encoding %q", k);
}

/* encodeHeaderRaw: the fields as given, in order. kv holds n names and
 * values. */
static Slice h2t_encode_header_raw(H2tTester *st, const Str *kv, size_t n) {
    bytes_buffer_reset(&st->hbuf);
    for (size_t i = 0; i + 1 < n; i += 2)
        h2t_field(st, kv[i], kv[i + 1]);
    return bytes_buffer_bytes(&st->hbuf);
}

typedef struct H2tField {
    Str k;
    Str v[8];
    int nv;
    int npseudo;
} H2tField;

/* encodeHeader: :method GET, :scheme https, :authority dummy.tld and :path /
 * unless kv says otherwise, then the rest of kv. A pseudo header given twice
 * goes out twice. */
static Slice h2t_encode_header(H2tTester *st, const Str *kv, size_t n) {
    H2tField fs[24];
    memset(fs, 0, sizeof fs);
    size_t nf = 4;
    fs[0].k = BURROW_S(":method");
    fs[0].v[0] = BURROW_S("GET");
    fs[1].k = BURROW_S(":scheme");
    fs[1].v[0] = BURROW_S("https");
    fs[2].k = BURROW_S(":authority");
    fs[2].v[0] = BURROW_S("dummy.tld");
    fs[3].k = BURROW_S(":path");
    fs[3].v[0] = BURROW_S("/");
    for (size_t i = 0; i < 4; i++)
        fs[i].nv = 1;
    for (size_t i = 0; i + 1 < n; i += 2) {
        size_t j = 0;
        while (j < nf && !str_eq(fs[j].k, kv[i]))
            j++;
        if (j == nf) {
            if (nf == sizeof fs / sizeof fs[0])
                break;
            fs[nf++].k = kv[i];
        }
        H2tField *f = &fs[j];
        if (f->nv == (int)(sizeof f->v / sizeof f->v[0]))
            continue;
        if (kv[i].len > 0 && kv[i].p[0] == ':') {
            f->npseudo++;
            if (f->npseudo == 1)
                f->nv = 0;
        }
        f->v[f->nv++] = kv[i + 1];
    }
    bytes_buffer_reset(&st->hbuf);
    for (size_t i = 0; i < nf; i++) {
        for (int j = 0; j < fs[i].nv; j++)
            h2t_field(st, fs[i].k, fs[i].v[j]);
    }
    return bytes_buffer_bytes(&st->hbuf);
}

static bool h2t_write_headers(H2tTester *st, uint32_t id, Slice block, bool end_stream,
                              bool end_headers) {
    Http2HeadersFrameParam p;
    memset(&p, 0, sizeof p);
    p.stream_id = id;
    p.block_fragment = block;
    p.end_stream = end_stream;
    p.end_headers = end_headers;
    return !h2t_failed(st, "writing HEADERS",
                       burrow__http2_framer_write_headers(st->fr, p));
}

static bool h2t_write_data(H2tTester *st, uint32_t id, bool end_stream,
                           const char *data, Int n) {
    Slice p = slice_from((void *)(uintptr_t)data, n, n, TYPE_BYTE);
    return !h2t_failed(st, "writing DATA",
                       burrow__http2_framer_write_data(st->fr, id, end_stream, p));
}

/* bodylessReq1: stream 1, with encodeHeader's fields and END_STREAM. */
static bool h2t_bodyless_req1(H2tTester *st, const Str *kv, size_t n) {
    return h2t_write_headers(st, 1, h2t_encode_header(st, kv, n), true, true);
}

/* wantHeaders: a HEADERS frame on stream id, and for each name in want, the
 * values the frame has under it are the ones want gives it, in order. */
static bool h2t_want_headers(H2tTester *st, uint32_t id, bool end_stream,
                             const Str *want, size_t n) {
    Http2Frame *f = h2t_read_kind(st, HTTP2_META_HEADERS_FRAME);
    if (f == NULL)
        return false;
    bool ok = true;
    if (f->header.stream_id != id) {
        testing_t_errorf_v(st->t, "got stream ID %d, want %d", (int)f->header.stream_id,
                           (int)id);
        ok = false;
    }
    bool ended = (f->header.flags & HTTP2_FLAG_HEADERS_END_STREAM) != 0;
    if (ended != end_stream) {
        testing_t_errorf_v(st->t, "got stream ended %t, want %t", ended, end_stream);
        ok = false;
    }
    const HpackHeaderFields *fs = &f->u.meta_headers.fields;
    for (size_t i = 0; ok && i + 1 < n; i += 2) {
        bool seen = false;
        for (size_t j = 0; j < i; j += 2)
            seen = seen || str_eq(want[j], want[i]);
        if (seen)
            continue;
        size_t w = i;
        Int g = 0;
        for (;;) {
            while (w < n && !str_eq(want[w], want[i]))
                w += 2;
            while (g < fs->len && !str_eq(fs->p[g].name, want[i]))
                g++;
            if (w >= n && g >= fs->len)
                break;
            if (w >= n || g >= fs->len || !str_eq(want[w + 1], fs->p[g].value)) {
                testing_t_errorf_v(st->t, "header %q: got %q, want %q", want[i],
                                   g < fs->len ? fs->p[g].value : BURROW_S("(none)"),
                                   w < n ? want[w + 1] : BURROW_S("(none)"));
                ok = false;
                break;
            }
            w += 2;
            g++;
        }
    }
    burrow__http2_frame_free(f);
    return ok;
}

/* wantData, one frame of it. */
static bool h2t_want_data(H2tTester *st, uint32_t id, bool end_stream,
                          const char *want) {
    Http2Frame *f = h2t_read_kind(st, HTTP2_DATA_FRAME);
    if (f == NULL)
        return false;
    bool ok = true;
    Slice d = f->u.data.data;
    bool ended = (f->header.flags & HTTP2_FLAG_DATA_END_STREAM) != 0;
    Int wn = (Int)strlen(want);
    if (f->header.stream_id != id || ended != end_stream || d.len != wn ||
        (wn > 0 && memcmp(d.p, want, (size_t)wn) != 0)) {
        testing_t_errorf_v(st->t,
                           "got DATA stream %d end %t %q; want stream %d end %t %q",
                           (int)f->header.stream_id, ended, str_from_bytes(d.p, d.len),
                           (int)id, end_stream, str_from_cstr(want));
        ok = false;
    }
    burrow__http2_frame_free(f);
    return ok;
}

static bool h2t_want_rst_stream(H2tTester *st, uint32_t id, Http2ErrCode code) {
    Http2Frame *f = h2t_read_kind(st, HTTP2_RST_STREAM_FRAME);
    if (f == NULL)
        return false;
    bool ok = f->header.stream_id == id && f->u.rst_stream.err_code == code;
    if (!ok)
        testing_t_errorf_v(st->t, "got RST_STREAM StreamID=%d code=%d, want %d %d",
                           (int)f->header.stream_id, (int)f->u.rst_stream.err_code,
                           (int)id, (int)code);
    burrow__http2_frame_free(f);
    return ok;
}

static bool h2t_want_go_away(H2tTester *st, uint32_t max_stream_id, Http2ErrCode code) {
    Http2Frame *f = h2t_read_kind(st, HTTP2_GO_AWAY_FRAME);
    if (f == NULL)
        return false;
    bool ok =
        f->u.go_away.last_stream_id == max_stream_id && f->u.go_away.err_code == code;
    if (!ok)
        testing_t_errorf_v(st->t, "got GOAWAY LastStreamID=%d code=%d, want %d %d",
                           (int)f->u.go_away.last_stream_id, (int)f->u.go_away.err_code,
                           (int)max_stream_id, (int)code);
    burrow__http2_frame_free(f);
    return ok;
}

/* ------------------------------------------------------------ requests */

typedef bool (*H2tWriteFn)(H2tTester *st);
typedef void (*H2tCheckFn)(TestingT *t, HttpRequest *r);

typedef struct H2tReq {
    TestingT *t;
    Chan *got;
    H2tCheckFn check;
} H2tReq;

static void h2t_req_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    H2tReq *q = (H2tReq *)env;
    if (r->body.vt == NULL)
        testing_t_errorf_v(q->t, "nil Body");
    else
        q->check(q->t, r);
    bool v = true;
    chan_send(q->got, &v);
}

/* testServerRequest: write sends a request, and check looks at what the
 * handler got. */
static void h2t_server_request(TestingT *t, H2tWriteFn write, H2tCheckFn check) {
    H2tReq q = {t, chan_make(heap_allocator(), TYPE_BOOL, 1), check};
    if (q.got == NULL)
        FATALF("no memory");
    H2tTester st;
    if (h2t_start(&st, t, h2t_req_handler, &q) && h2t_greet(&st) && write(&st)) {
        bool v;
        (void)chan_recv(q.got, &v);
    }
    h2t_close(&st);
    chan_free(q.got);
}

/* A read of one byte from the body, which should be 0 and io_eof. */
static void h2t_want_body_eof(TestingT *t, HttpRequest *r) {
    Byte b[1];
    Error err = BURROW_NO_ERROR;
    Int n = r->body.vt->reader.read(r->body.data, slice_from(b, 1, 1, TYPE_BYTE), &err);
    if (n != 0 || !errors_is(err, io_eof))
        testing_t_errorf_v(t, "Read = %d, %v; want 0, EOF", n, err);
}

static bool h2t_write_get(H2tTester *st) {
    static const Str kv[] = {BURROW_S_INIT("foo-bar"), BURROW_S_INIT("some-value")};
    return h2t_write_headers(st, 1, h2t_encode_header(st, kv, 2), true, true);
}

static void h2t_check_get(TestingT *t, HttpRequest *r) {
    if (!str_eq(r->method, BURROW_S("GET")))
        testing_t_errorf_v(t, "Method = %q; want GET", r->method);
    if (!str_eq(r->url->path, BURROW_S("/")))
        testing_t_errorf_v(t, "URL.Path = %q; want /", r->url->path);
    if (r->content_length != 0)
        testing_t_errorf_v(t, "ContentLength = %d; want 0", r->content_length);
    if (r->close)
        testing_t_errorf_v(t, "Close = true; want false");
    /* Go's fake connection has a host and port. A pipe's address is "pipe". */
    if (!str_eq(r->remote_addr, BURROW_S("pipe")))
        testing_t_errorf_v(t, "RemoteAddr = %q; want pipe", r->remote_addr);
    if (!str_eq(r->proto, BURROW_S("HTTP/2.0")) || r->proto_major != 2 ||
        r->proto_minor != 0)
        testing_t_errorf_v(t, "Proto = %q Major=%d,Minor=%d; want HTTP/2.0", r->proto,
                           r->proto_major, r->proto_minor);
    Slice vs = http_header_values(r->header, BURROW_S("Foo-Bar"));
    if (map_len(r->header) != 1 || vs.len != 1 ||
        !str_eq(((const Str *)vs.p)[0], BURROW_S("some-value")))
        testing_t_errorf_v(t, "Header has %d keys; want only Foo-Bar: some-value",
                           map_len(r->header));
    h2t_want_body_eof(t, r);
}

static void TestServer_Request_Get(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_request(t, h2t_write_get, h2t_check_get);
}

static bool h2t_write_path_slashes(H2tTester *st) {
    static const Str kv[] = {BURROW_S_INIT(":path"), BURROW_S_INIT("/%2f/")};
    return h2t_write_headers(st, 1, h2t_encode_header(st, kv, 2), true, true);
}

static void h2t_check_path_slashes(TestingT *t, HttpRequest *r) {
    if (!str_eq(r->request_uri, BURROW_S("/%2f/")))
        testing_t_errorf_v(t, "RequestURI = %q; want /%%2f/", r->request_uri);
    if (!str_eq(r->url->path, BURROW_S("///")))
        testing_t_errorf_v(t, "URL.Path = %q; want ///", r->url->path);
}

static void TestServer_Request_Get_PathSlashes(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_request(t, h2t_write_path_slashes, h2t_check_path_slashes);
}

static const Str h2t_post[] = {BURROW_S_INIT(":method"), BURROW_S_INIT("POST")};

static bool h2t_write_post_end_stream(H2tTester *st) {
    return h2t_write_headers(st, 1, h2t_encode_header(st, h2t_post, 2), true, true);
}

static void h2t_check_post_no_body(TestingT *t, HttpRequest *r) {
    if (!str_eq(r->method, BURROW_S("POST")))
        testing_t_errorf_v(t, "Method = %q; want POST", r->method);
    if (r->content_length != 0)
        testing_t_errorf_v(t, "ContentLength = %d; want 0", r->content_length);
    h2t_want_body_eof(t, r);
}

static void TestServer_Request_Post_NoContentLength_EndStream(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_request(t, h2t_write_post_end_stream, h2t_check_post_no_body);
}

/* testBodyContents and testBodyContentsFail: what the handler should see,
 * set before each run. */
static int64_t h2t_want_content_length;
static const char *h2t_want_body;
static const char *h2t_want_read_error;

static void h2t_check_body(TestingT *t, HttpRequest *r) {
    if (!str_eq(r->method, BURROW_S("POST")))
        testing_t_errorf_v(t, "Method = %q; want POST", r->method);
    if (r->content_length != h2t_want_content_length)
        testing_t_errorf_v(t, "ContentLength = %d; want %d", r->content_length,
                           h2t_want_content_length);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Slice all =
        io_read_all(arena_allocator(&ar), io_read_closer_as_io_reader(r->body), &err);
    Str got = str_from_bytes(all.p, all.len);
    if (h2t_want_read_error == NULL) {
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "ReadAll: %v", err);
        else if (!str_eq(got, str_from_cstr(h2t_want_body)))
            testing_t_errorf_v(t, "Read = %q; want %q", got,
                               str_from_cstr(h2t_want_body));
    } else if (!BURROW_FAILED(err)) {
        testing_t_errorf_v(t,
                           "expected an error (%q) reading from the body. Successfully "
                           "read %q instead.",
                           str_from_cstr(h2t_want_read_error), got);
    } else if (!strings_contains(error_text(err), str_from_cstr(h2t_want_read_error))) {
        testing_t_errorf_v(t, "Body.Read = %v; want substring %q", err,
                           str_from_cstr(h2t_want_read_error));
    }
    arena_free(&ar);
    Error cerr = r->body.vt->closer.close(r->body.data);
    if (BURROW_FAILED(cerr))
        testing_t_errorf_v(t, "Close: %v", cerr);
}

static void h2t_body_contents(TestingT *t, int64_t want_content_length,
                              const char *want_body, H2tWriteFn write) {
    h2t_want_content_length = want_content_length;
    h2t_want_body = want_body;
    h2t_want_read_error = NULL;
    h2t_server_request(t, write, h2t_check_body);
}

static void h2t_body_contents_fail(TestingT *t, int64_t want_content_length,
                                   const char *want_read_error, H2tWriteFn write) {
    h2t_want_content_length = want_content_length;
    h2t_want_body = NULL;
    h2t_want_read_error = want_read_error;
    h2t_server_request(t, write, h2t_check_body);
}

#define H2T_CONTENT "Some content"

static bool h2t_write_post_headers(H2tTester *st) {
    return h2t_write_headers(st, 1, h2t_encode_header(st, h2t_post, 2), false, true);
}

static bool h2t_write_immediate_eof(H2tTester *st) {
    return h2t_write_post_headers(st) && h2t_write_data(st, 1, true, NULL, 0);
}

static void TestServer_Request_Post_Body_ImmediateEOF(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_body_contents(t, -1, "", h2t_write_immediate_eof);
}

static bool h2t_write_one_data(H2tTester *st) {
    return h2t_write_post_headers(st) &&
           h2t_write_data(st, 1, true, H2T_CONTENT, (Int)strlen(H2T_CONTENT));
}

static void TestServer_Request_Post_Body_OneData(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_body_contents(t, -1, H2T_CONTENT, h2t_write_one_data);
}

static bool h2t_write_two_data(H2tTester *st) {
    return h2t_write_post_headers(st) && h2t_write_data(st, 1, false, H2T_CONTENT, 5) &&
           h2t_write_data(st, 1, true, &H2T_CONTENT[5], (Int)strlen(H2T_CONTENT) - 5);
}

static void TestServer_Request_Post_Body_TwoData(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_body_contents(t, -1, H2T_CONTENT, h2t_write_two_data);
}

static bool h2t_write_post_cl(H2tTester *st, const char *cl) {
    Str kv[4] = {BURROW_S(":method"), BURROW_S("POST"), BURROW_S("content-length"),
                 str_from_cstr(cl)};
    return h2t_write_headers(st, 1, h2t_encode_header(st, kv, 4), false, true);
}

static bool h2t_write_cl_correct(H2tTester *st) {
    return h2t_write_post_cl(st, "12") &&
           h2t_write_data(st, 1, true, H2T_CONTENT, (Int)strlen(H2T_CONTENT));
}

static void TestServer_Request_Post_Body_ContentLength_Correct(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_body_contents(t, (int64_t)strlen(H2T_CONTENT), H2T_CONTENT,
                      h2t_write_cl_correct);
}

static bool h2t_write_cl_too_large(H2tTester *st) {
    return h2t_write_post_cl(st, "3") && h2t_write_data(st, 1, true, "12", 2);
}

static void TestServer_Request_Post_Body_ContentLength_TooLarge(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_body_contents_fail(
        t, 3, "request declared a Content-Length of 3 but only wrote 2 bytes",
        h2t_write_cl_too_large);
}

static bool h2t_write_cl_too_small(H2tTester *st) {
    return h2t_write_post_cl(st, "4") && h2t_write_data(st, 1, true, "12345", 5) &&
           h2t_want_rst_stream(st, 1, HTTP2_ERR_CODE_PROTOCOL);
}

static void TestServer_Request_Post_Body_ContentLength_TooSmall(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_body_contents_fail(
        t, 4,
        "sender tried to send more than declared Content-Length of 4 "
        "bytes",
        h2t_write_cl_too_small);
}

static void h2t_check_host(TestingT *t, HttpRequest *r) {
    if (!str_eq(r->host, BURROW_S("example.com")))
        testing_t_errorf_v(t, "Host = %q; want %q", r->host, BURROW_S("example.com"));
}

/* Using a Host header, instead of :authority. */
static bool h2t_write_host(H2tTester *st) {
    static const Str kv[] = {BURROW_S_INIT(":authority"), BURROW_S_INIT(""),
                             BURROW_S_INIT("host"), BURROW_S_INIT("example.com")};
    return h2t_write_headers(st, 1, h2t_encode_header(st, kv, 4), true, true);
}

static void TestServer_Request_Get_Host(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_request(t, h2t_write_host, h2t_check_host);
}

/* Using :authority, instead of a Host header. */
static bool h2t_write_authority(H2tTester *st) {
    static const Str kv[] = {BURROW_S_INIT(":authority"), BURROW_S_INIT("example.com")};
    return h2t_write_headers(st, 1, h2t_encode_header(st, kv, 2), true, true);
}

static void TestServer_Request_Get_Authority(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_request(t, h2t_write_authority, h2t_check_host);
}

static bool h2t_write_cookies(H2tTester *st) {
    static const Str kv[] = {
        BURROW_S_INIT(":authority"), BURROW_S_INIT("example.com"),
        BURROW_S_INIT("cookie"),     BURROW_S_INIT("a=b"),
        BURROW_S_INIT("cookie"),     BURROW_S_INIT("c=d"),
        BURROW_S_INIT("cookie"),     BURROW_S_INIT("e=f"),
    };
    return h2t_bodyless_req1(st, kv, sizeof kv / sizeof kv[0]);
}

static void h2t_check_cookies(TestingT *t, HttpRequest *r) {
    Str got = http_header_get(r->header, BURROW_S("Cookie"));
    if (!str_eq(got, BURROW_S("a=b; c=d; e=f")))
        testing_t_errorf_v(t, "Cookie = %q; want %q", got, BURROW_S("a=b; c=d; e=f"));
}

static void TestServer_Request_CookieConcat(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_request(t, h2t_write_cookies, h2t_check_cookies);
}

/* --------------------------------------------------------- rejections */

static void h2t_reject_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    (void)r;
    testing_t_errorf_v((TestingT *)env,
                       "server request made it to handler; should've been rejected");
}

/* testRejectRequest, with the request's fields given to encodeHeader, or
 * as they are when raw. */
static void h2t_reject_request(TestingT *t, const Str *kv, size_t n, bool raw) {
    H2tTester st;
    if (h2t_start(&st, t, h2t_reject_handler, t) && h2t_greet(&st)) {
        Slice block =
            raw ? h2t_encode_header_raw(&st, kv, n) : h2t_encode_header(&st, kv, n);
        if (h2t_write_headers(&st, 1, block, true, true))
            (void)h2t_want_rst_stream(&st, 1, HTTP2_ERR_CODE_PROTOCOL);
    }
    h2t_close(&st);
}

#define H2T_REJECT(name, ...)                                                          \
    static void name(TestingT *t) {                                                    \
        SKIP_WITHOUT_THREADS(t);                                                       \
        static const Str kv[] = {__VA_ARGS__};                                         \
        h2t_reject_request(t, kv, sizeof kv / sizeof kv[0], false);                    \
    }

#define S_ BURROW_S_INIT

H2T_REJECT(TestServer_Request_Reject_CapitalHeader, S_("UPPER"), S_("v"))
H2T_REJECT(TestServer_Request_Reject_HeaderFieldNameColon, S_("has:colon"), S_("v"))
H2T_REJECT(TestServer_Request_Reject_HeaderFieldNameNULL,
           S_("has\x00"
              "null"),
           S_("v"))
H2T_REJECT(TestServer_Request_Reject_HeaderFieldNameEmpty, S_(""), S_("v"))
H2T_REJECT(TestServer_Request_Reject_HeaderFieldValueNewline, S_("foo"),
           S_("has\nnewline"))
H2T_REJECT(TestServer_Request_Reject_HeaderFieldValueCR, S_("foo"), S_("has\rcarriage"))
H2T_REJECT(TestServer_Request_Reject_HeaderFieldValueDEL, S_("foo"),
           S_("has\x7f"
              "del"))
H2T_REJECT(TestServer_Request_Reject_Pseudo_Missing_method, S_(":method"), S_(""))
H2T_REJECT(TestServer_Request_Reject_Pseudo_ExactlyOne, S_(":method"), S_("GET"),
           S_(":method"), S_("POST"))
H2T_REJECT(TestServer_Request_Reject_Pseudo_Missing_path, S_(":path"), S_(""))
H2T_REJECT(TestServer_Request_Reject_Pseudo_Missing_scheme, S_(":scheme"), S_(""))
H2T_REJECT(TestServer_Request_Reject_Pseudo_scheme_invalid, S_(":scheme"), S_("bogus"))
H2T_REJECT(TestServer_Request_Reject_Pseudo_Unknown, S_(":unknown_thing"), S_(""))

static void TestServer_Request_Reject_Pseudo_AfterRegular(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    static const Str kv[] = {S_(":method"), S_("GET"), S_("regular"), S_("foobar"),
                             S_(":path"),   S_("/"),   S_(":scheme"), S_("https")};
    h2t_reject_request(t, kv, sizeof kv / sizeof kv[0], true);
}

static void TestServer_Request_Reject_Authority_Userinfo(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    static const Str kv[] = {S_(":authority"), S_("userinfo@example.tld"),
                             S_(":method"),    S_("GET"),
                             S_(":path"),      S_("/"),
                             S_(":scheme"),    S_("https")};
    h2t_reject_request(t, kv, sizeof kv / sizeof kv[0], true);
}

/* newServerTesterForError, then one frame, then the GOAWAY it brings. */
typedef bool (*H2tFrameFn)(H2tTester *st);

static void h2t_want_conn_error(TestingT *t, H2tFrameFn send, uint32_t last_id) {
    H2tTester st;
    if (h2t_start(&st, t, h2t_reject_handler, t) && h2t_greet(&st) && send(&st))
        (void)h2t_want_go_away(&st, last_id, HTTP2_ERR_CODE_PROTOCOL);
    h2t_close(&st);
}

static bool h2t_send_idle_window_update(H2tTester *st) {
    return !h2t_failed(st, "WriteWindowUpdate",
                       burrow__http2_framer_write_window_update(st->fr, 123, 456));
}

/* Section 5.1, on idle connections: "Receiving any frame other than HEADERS
 * or PRIORITY on a stream in this state MUST be treated as a connection error
 * (Section 5.4.1) of type PROTOCOL_ERROR." */
static void TestRejectFrameOnIdle_WindowUpdate(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_want_conn_error(t, h2t_send_idle_window_update, 123);
}

static bool h2t_send_idle_data(H2tTester *st) {
    return h2t_write_data(st, 123, true, NULL, 0);
}

static void TestRejectFrameOnIdle_Data(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_want_conn_error(t, h2t_send_idle_data, 123);
}

static bool h2t_send_idle_rst_stream(H2tTester *st) {
    return !h2t_failed(
        st, "WriteRSTStream",
        burrow__http2_framer_write_rst_stream(st->fr, 123, HTTP2_ERR_CODE_CANCEL));
}

static void TestRejectFrameOnIdle_RSTStream(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_want_conn_error(t, h2t_send_idle_rst_stream, 123);
}

static bool h2t_send_headers0(H2tTester *st) {
    st->fr->allow_illegal_writes = true;
    return h2t_write_headers(st, 0, h2t_encode_header(st, NULL, 0), true, true);
}

static void TestServer_Rejects_Headers0(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_want_conn_error(t, h2t_send_headers0, 0);
}

static bool h2t_send_continuation0(H2tTester *st) {
    st->fr->allow_illegal_writes = true;
    return !h2t_failed(st, "WriteContinuation",
                       burrow__http2_framer_write_continuation(
                           st->fr, 0, true, h2t_encode_header(st, NULL, 0)));
}

static void TestServer_Rejects_Continuation0(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_want_conn_error(t, h2t_send_continuation0, 0);
}

static bool h2t_send_priority0(H2tTester *st) {
    st->fr->allow_illegal_writes = true;
    Http2PriorityParam p;
    memset(&p, 0, sizeof p);
    p.stream_dep = 1;
    return !h2t_failed(st, "WritePriority",
                       burrow__http2_framer_write_priority(st->fr, 0, p));
}

static void TestServer_Rejects_Priority0(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_want_conn_error(t, h2t_send_priority0, 0);
}

static bool h2t_send_push_promise(H2tTester *st) {
    Http2PushPromiseParam pp;
    memset(&pp, 0, sizeof pp);
    pp.stream_id = 1;
    pp.promise_id = 3;
    return !h2t_failed(st, "WritePushPromise",
                       burrow__http2_framer_write_push_promise(st->fr, pp));
}

static void TestServer_Rejects_PushPromise(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_want_conn_error(t, h2t_send_push_promise, 1);
}

/* ----------------------------------------------------------------- PING */

static void TestServer_Ping(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tTester st;
    if (!h2t_start(&st, t, NULL, NULL) || !h2t_greet(&st)) {
        h2t_close(&st);
        return;
    }
    static const Byte ack_ping_data[8] = {1, 2, 4, 8, 16, 32, 64, 128};
    static const Byte ping_data[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    if (h2t_failed(&st, "WritePing",
                   burrow__http2_framer_write_ping(st.fr, true, ack_ping_data)) ||
        h2t_failed(&st, "WritePing",
                   burrow__http2_framer_write_ping(st.fr, false, ping_data))) {
        h2t_close(&st);
        return;
    }
    Http2Frame *f = h2t_read_kind(&st, HTTP2_PING_FRAME);
    if (f != NULL) {
        if ((f->header.flags & HTTP2_FLAG_PING_ACK) == 0)
            testing_t_errorf_v(t, "response ping doesn't have ACK set");
        if (memcmp(f->u.ping.data, ping_data, 8) != 0)
            testing_t_errorf_v(t, "response ping has data %q; want %q",
                               str_from_bytes(f->u.ping.data, 8),
                               str_from_bytes(ping_data, 8));
        burrow__http2_frame_free(f);
    }
    h2t_close(&st);
}

/* ------------------------------------------------------------ responses */

typedef void (*H2tRespFn)(HttpResponseWriter w, HttpRequest *r);
typedef bool (*H2tClientFn)(H2tTester *st);

typedef struct H2tResp {
    TestingT *t;
    Chan *done;
    H2tRespFn handler;
} H2tResp;

static void h2t_resp_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    H2tResp *q = (H2tResp *)env;
    if (r->body.vt == NULL)
        testing_t_errorf_v(q->t, "nil Body");
    q->handler(w, r);
    bool v = true;
    if (!chan_try_send(q->done, &v))
        testing_t_errorf_v(q->t, "unexpected duplicate request");
}

/* testServerResponse. */
static void h2t_server_response(TestingT *t, H2tRespFn handler, H2tClientFn client) {
    H2tResp q = {t, chan_make(heap_allocator(), TYPE_BOOL, 1), handler};
    if (q.done == NULL)
        FATALF("no memory");
    H2tTester st;
    if (h2t_start(&st, t, h2t_resp_handler, &q) && h2t_greet(&st) && client(&st)) {
        bool v;
        (void)chan_recv(q.done, &v);
    }
    h2t_close(&st);
    chan_free(q.done);
}

static bool h2t_get_slash(H2tTester *st) {
    return h2t_bodyless_req1(st, NULL, 0);
}

static void h2t_handle_nothing(HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    (void)r;
}

static bool h2t_client_no_data(H2tTester *st) {
    return h2t_get_slash(st) && h2t_want_headers(st, 1, true, NULL, 0);
}

static void TestServer_Response_NoData(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_nothing, h2t_client_no_data);
}

static void h2t_handle_foo_bar(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    (void)http_header_set(http_response_writer_header(w), BURROW_S("Foo-Bar"),
                          BURROW_S("some-value"));
}

static bool h2t_client_foo_bar(H2tTester *st) {
    static const Str want[] = {S_(":status"),        S_("200"),
                               S_("foo-bar"),        S_("some-value"),
                               S_("content-length"), S_("0")};
    return h2t_get_slash(st) &&
           h2t_want_headers(st, 1, true, want, sizeof want / sizeof want[0]);
}

static void TestServer_Response_NoData_Header_FooBar(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_foo_bar, h2t_client_foo_bar);
}

static void h2t_write_str(HttpResponseWriter w, const char *s) {
    Error err = BURROW_NO_ERROR;
    Int n = (Int)strlen(s);
    (void)http_response_writer_write(
        w, slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE), &err);
}

static void h2t_handle_chunked(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    /* should be stripped */
    (void)http_header_set(http_response_writer_header(w), BURROW_S("Transfer-Encoding"),
                          BURROW_S("chunked"));
    h2t_write_str(w, "hi");
}

static bool h2t_client_chunked(H2tTester *st) {
    static const Str want[] = {S_(":status"),        S_("200"),
                               S_("content-type"),   S_("text/plain; charset=utf-8"),
                               S_("content-length"), S_("2")};
    return h2t_get_slash(st) &&
           h2t_want_headers(st, 1, false, want, sizeof want / sizeof want[0]);
}

static void TestServer_Response_TransferEncoding_chunked(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_chunked, h2t_client_chunked);
}

static void TestServer_Rejects_ConnHeaders(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tTester st;
    if (h2t_start(&st, t, h2t_reject_handler, t) && h2t_greet(&st)) {
        static const Str kv[] = {S_("connection"), S_("foo")};
        static const Str want[] = {
            S_(":status"),
            S_("400"),
            S_("content-type"),
            S_("text/plain; charset=utf-8"),
            S_("x-content-type-options"),
            S_("nosniff"),
            S_("content-length"),
            S_("51"),
        };
        if (h2t_bodyless_req1(&st, kv, 2))
            (void)h2t_want_headers(&st, 1, false, want, sizeof want / sizeof want[0]);
    }
    h2t_close(&st);
}

static void h2t_handle_read_all(HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    (void)io_read_all(arena_allocator(&ar), io_read_closer_as_io_reader(r->body), &err);
    arena_free(&ar);
}

static void TestServer_Rejects_TooSmall(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_read_all, h2t_write_cl_too_small);
}

/* ------------------------------------------------------------- trailers */

static bool h2t_write_req_trailers(H2tTester *st) {
    static const Str kv[] = {S_("trailer"), S_("Foo, Bar"), S_("trailer"), S_("Baz")};
    static const Str tr[] = {S_("foo"),      S_("foov"),
                             S_("bar"),      S_("barv"),
                             S_("baz"),      S_("bazv"),
                             S_("surprise"), S_("wasn't declared; shouldn't show up")};
    return h2t_write_headers(st, 1, h2t_encode_header(st, kv, 4), false, true) &&
           h2t_write_data(st, 1, false, "some test body", 14) &&
           h2t_write_headers(st, 1,
                             h2t_encode_header_raw(st, tr, sizeof tr / sizeof tr[0]),
                             true, true);
}

static void h2t_check_trailer(TestingT *t, HttpRequest *r, const char *when,
                              bool values) {
    static const char *const keys[] = {"Foo", "Bar", "Baz"};
    static const char *const vals[] = {"foov", "barv", "bazv"};
    if (r->trailer == NULL || map_len(r->trailer) != 3) {
        testing_t_errorf_v(t, "%s Trailer has %d keys; want 3", str_from_cstr(when),
                           r->trailer == NULL ? (Int)0 : map_len(r->trailer));
        return;
    }
    for (int i = 0; i < 3; i++) {
        Slice vs = http_header_values(r->trailer, str_from_cstr(keys[i]));
        bool ok = values ? vs.len == 1 &&
                               str_eq(((const Str *)vs.p)[0], str_from_cstr(vals[i]))
                         : vs.len == 0;
        if (!ok)
            testing_t_errorf_v(t, "%s Trailer %s has %d values; want %s",
                               str_from_cstr(when), str_from_cstr(keys[i]), vs.len,
                               str_from_cstr(values ? vals[i] : "none"));
    }
}

static void h2t_check_req_trailers(TestingT *t, HttpRequest *r) {
    h2t_check_trailer(t, r, "initial", false);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error err = BURROW_NO_ERROR;
    Slice all =
        io_read_all(arena_allocator(&ar), io_read_closer_as_io_reader(r->body), &err);
    Str got = str_from_bytes(all.p, all.len);
    if (!str_eq(got, BURROW_S("some test body")))
        testing_t_errorf_v(t, "read body %q; want %q", got, BURROW_S("some test body"));
    arena_free(&ar);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Body slurp: %v", err);
        return;
    }
    h2t_check_trailer(t, r, "final", true);
}

static void TestServerReadsTrailers(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_request(t, h2t_write_req_trailers, h2t_check_req_trailers);
}

static bool h2t_trailers_with_flush;

static void h2t_handle_trailers(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    HttpHeader h = http_response_writer_header(w);
    (void)http_header_set(h, BURROW_S("Trailer"),
                          BURROW_S("Server-Trailer-A, Server-Trailer-B"));
    (void)http_header_add(h, BURROW_S("Trailer"), BURROW_S("Server-Trailer-C"));
    /* filtered */
    (void)http_header_add(h, BURROW_S("Trailer"),
                          BURROW_S("Transfer-Encoding, Content-Length, Trailer"));

    (void)http_header_set(h, BURROW_S("Foo"), BURROW_S("Bar"));
    (void)http_header_set(h, BURROW_S("Content-Length"),
                          BURROW_S("5")); /* len("Hello") */

    h2t_write_str(w, "Hello");
    if (h2t_trailers_with_flush)
        (void)w.vt->flush(w.data);
    (void)http_header_set(h, BURROW_S("Server-Trailer-A"), BURROW_S("valuea"));
    (void)http_header_set(h, BURROW_S("Server-Trailer-C"),
                          BURROW_S("valuec")); /* skipping B */
    (void)http_header_set(h, BURROW_S("Server-Surpise"),
                          BURROW_S("surprise! this isn't predeclared!"));
    (void)http_header_set(h, BURROW_S("Trailer:Post-Header-Trailer"), BURROW_S("hi1"));
    (void)http_header_set(h, BURROW_S("Trailer:post-header-trailer2"), BURROW_S("hi2"));
    (void)http_header_set(h, BURROW_S("Trailer:Range"), BURROW_S("invalid"));
    (void)http_header_set(h,
                          BURROW_S("Trailer:Foo\x01"
                                   "Bogus"),
                          BURROW_S("invalid"));
    (void)http_header_set(
        h, BURROW_S("Transfer-Encoding"),
        BURROW_S("should not be included; Forbidden by RFC 7230 4.1.2"));
    (void)http_header_set(
        h, BURROW_S("Content-Length"),
        BURROW_S("should not be included; Forbidden by RFC 7230 4.1.2"));
    (void)http_header_set(
        h, BURROW_S("Trailer"),
        BURROW_S("should not be included; Forbidden by RFC 7230 4.1.2"));
}

static bool h2t_client_trailers(H2tTester *st) {
    static const Str want[] = {
        S_(":status"),
        S_("200"),
        S_("foo"),
        S_("Bar"),
        S_("trailer"),
        S_("Server-Trailer-A, Server-Trailer-B"),
        S_("trailer"),
        S_("Server-Trailer-C"),
        S_("trailer"),
        S_("Transfer-Encoding, Content-Length, Trailer"),
        S_("content-type"),
        S_("text/plain; charset=utf-8"),
        S_("content-length"),
        S_("5"),
    };
    static const Str want_trailers[] = {
        S_("post-header-trailer"),  S_("hi1"),
        S_("post-header-trailer2"), S_("hi2"),
        S_("server-trailer-a"),     S_("valuea"),
        S_("server-trailer-c"),     S_("valuec"),
    };
    return h2t_get_slash(st) &&
           h2t_want_headers(st, 1, false, want, sizeof want / sizeof want[0]) &&
           h2t_want_data(st, 1, false, "Hello") &&
           h2t_want_headers(st, 1, true, want_trailers,
                            sizeof want_trailers / sizeof want_trailers[0]);
}

static void TestServerWritesTrailers_WithFlush(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_trailers_with_flush = true;
    h2t_server_response(t, h2t_handle_trailers, h2t_client_trailers);
}

static void TestServerWritesTrailers_WithoutFlush(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_trailers_with_flush = false;
    h2t_server_response(t, h2t_handle_trailers, h2t_client_trailers);
}

/* -------------------------------------------------------- the server */

typedef struct H2tBasic {
    Chan *got_req;
} H2tBasic;

static void h2t_handle_basic(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    H2tBasic *b = (H2tBasic *)env;
    (void)http_header_set(http_response_writer_header(w), BURROW_S("Foo"),
                          BURROW_S("Bar"));
    bool v = true;
    chan_send(b->got_req, &v);
}

static void TestServer(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tBasic b = {chan_make(heap_allocator(), TYPE_BOOL, 1)};
    if (b.got_req == NULL)
        FATALF("no memory");
    H2tTester st;
    if (h2t_start(&st, t, h2t_handle_basic, &b) && h2t_greet(&st) &&
        h2t_write_headers(&st, 1, h2t_encode_header(&st, NULL, 0), true, true)) {
        bool v;
        (void)chan_recv(b.got_req, &v);
    }
    h2t_close(&st);
    chan_free(b.got_req);
}

/* The handler says it has started, which stands in for Go's st.sync(), and
 * waits to be let go. */
typedef struct H2tGraceful {
    Chan *started;
    Chan *handler_done;
} H2tGraceful;

static void h2t_handle_graceful(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    H2tGraceful *g = (H2tGraceful *)env;
    bool v = true;
    chan_send(g->started, &v);
    (void)chan_recv(g->handler_done, &v);
    (void)http_header_set(http_response_writer_header(w), BURROW_S("x-foo"),
                          BURROW_S("bar"));
}

static void h2t_shutdown_job(void *env) {
    (void)http_server_shutdown((HttpServer *)env, context_background());
}

static void TestServerGracefulShutdown(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tGraceful g = {chan_make(heap_allocator(), TYPE_BOOL, 1),
                     chan_make(heap_allocator(), TYPE_BOOL, 0)};
    if (g.started == NULL || g.handler_done == NULL) {
        chan_free(g.started);
        chan_free(g.handler_done);
        FATALF("no memory");
    }
    H2tTester st;
    SyncWaitGroup shutdown = {0};
    bool handler_running = false;
    if (!h2t_start(&st, t, h2t_handle_graceful, &g) || !h2t_greet(&st) ||
        !h2t_bodyless_req1(&st, NULL, 0))
        goto out;
    bool v;
    (void)chan_recv(g.started, &v);
    handler_running = true;

    (void)sync_wait_group_go(&shutdown, BURROW_FN(Func, h2t_shutdown_job, &st.srv));

    if (!h2t_want_go_away(&st, 1, HTTP2_ERR_CODE_NO))
        goto out;

    chan_close(g.handler_done);
    handler_running = false;

    static const Str want[] = {S_(":status"),        S_("200"), S_("x-foo"), S_("bar"),
                               S_("content-length"), S_("0")};
    if (!h2t_want_headers(&st, 1, true, want, sizeof want / sizeof want[0]))
        goto out;

    Byte b[1];
    Error err = BURROW_NO_ERROR;
    Int n = st.cc.vt->reader.read(st.cc.data, slice_from(b, 1, 1, TYPE_BYTE), &err);
    if (n != 0 || !BURROW_FAILED(err))
        testing_t_errorf_v(t, "Read = %d, %v; want 0, non-nil", n, err);

out:
    if (handler_running)
        chan_close(g.handler_done);
    h2t_close(&st);
    sync_wait_group_wait(&shutdown);
    chan_free(g.started);
    chan_free(g.handler_done);
}

/* ------------------------------------------------ frames out of order */

/* HEADERS on stream 1 without END_HEADERS. */
static bool h2t_headers_no_end(H2tTester *st) {
    return h2t_write_headers(st, 1, h2t_encode_header(st, NULL, 0), true, false);
}

static bool h2t_send_no_end_then_headers(H2tTester *st) {
    /* Not a continuation, and on a different stream. */
    return h2t_headers_no_end(st) &&
           h2t_write_headers(st, 3, h2t_encode_header(st, NULL, 0), true, true);
}

/* HEADERS without END_HEADERS, then another HEADERS. */
static void TestServer_Rejects_HeadersNoEnd_Then_Headers(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_want_conn_error(t, h2t_send_no_end_then_headers, 0);
}

static bool h2t_send_no_end_then_ping(H2tTester *st) {
    static const Byte zero[8] = {0};
    return h2t_headers_no_end(st) &&
           !h2t_failed(st, "WritePing",
                       burrow__http2_framer_write_ping(st->fr, false, zero));
}

/* HEADERS without END_HEADERS, then a PING. */
static void TestServer_Rejects_HeadersNoEnd_Then_Ping(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_want_conn_error(t, h2t_send_no_end_then_ping, 0);
}

static bool h2t_write_foo_continuation(H2tTester *st, uint32_t id) {
    static const Str kv[] = {S_("foo"), S_("bar")};
    return !h2t_failed(st, "WriteContinuation",
                       burrow__http2_framer_write_continuation(
                           st->fr, id, true, h2t_encode_header_raw(st, kv, 2)));
}

/* HEADERS with END_HEADERS, then a CONTINUATION. */
static void TestServer_Rejects_HeadersEnd_Then_Continuation(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tTester st;
    if (h2t_start(&st, t, NULL, NULL) && h2t_greet(&st) &&
        h2t_bodyless_req1(&st, NULL, 0) && h2t_want_headers(&st, 1, true, NULL, 0) &&
        h2t_write_foo_continuation(&st, 1))
        (void)h2t_want_go_away(&st, 1, HTTP2_ERR_CODE_PROTOCOL);
    h2t_close(&st);
}

static bool h2t_send_continuation_wrong_stream(H2tTester *st) {
    return h2t_headers_no_end(st) && h2t_write_foo_continuation(st, 3);
}

/* HEADERS without END_HEADERS, then a CONTINUATION on another stream. */
static void TestServer_Rejects_HeadersNoEnd_Then_ContinuationWrongStream(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_want_conn_error(t, h2t_send_continuation_wrong_stream, 0);
}

static bool h2t_send_priority_update0(H2tTester *st) {
    st->fr->allow_illegal_writes = true;
    return !h2t_failed(
        st, "WritePriorityUpdate",
        burrow__http2_framer_write_priority_update(st->fr, 0, BURROW_STR_EMPTY));
}

/* PRIORITY_UPDATE only takes a stream other than 0 in its payload. */
static void TestServer_Rejects_PriorityUpdate0(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_want_conn_error(t, h2t_send_priority_update0, 0);
}

/* testServerRejectsStream: the request write sends is answered with a
 * RST_STREAM carrying code. */
static void h2t_rejects_stream(TestingT *t, Http2ErrCode code, H2tFrameFn write) {
    H2tTester st;
    if (h2t_start(&st, t, NULL, NULL) && h2t_greet(&st) && write(&st))
        (void)h2t_want_rst_stream(&st, 1, code);
    h2t_close(&st);
}

static bool h2t_send_priority_unparsable(H2tTester *st) {
    return !h2t_failed(st, "WritePriorityUpdate",
                       burrow__http2_framer_write_priority_update(
                           st->fr, 1, BURROW_S("Invalid dictionary: ((((")));
}

/* PRIORITY_UPDATE whose priority doesn't parse. */
static void TestServer_Rejects_PriorityUpdateUnparsable(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_rejects_stream(t, HTTP2_ERR_CODE_PROTOCOL, h2t_send_priority_unparsable);
}

static bool h2t_send_headers_self_dep(H2tTester *st) {
    st->fr->allow_illegal_writes = true;
    Http2HeadersFrameParam p;
    memset(&p, 0, sizeof p);
    p.stream_id = 1;
    p.block_fragment = h2t_encode_header(st, NULL, 0);
    p.end_stream = true;
    p.end_headers = true;
    p.priority.stream_dep = 1;
    return !h2t_failed(st, "writing HEADERS",
                       burrow__http2_framer_write_headers(st->fr, p));
}

/* No HEADERS frame that depends on its own stream. */
static void TestServer_Rejects_HeadersSelfDependence(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_rejects_stream(t, HTTP2_ERR_CODE_PROTOCOL, h2t_send_headers_self_dep);
}

static bool h2t_send_priority_self_dep(H2tTester *st) {
    st->fr->allow_illegal_writes = true;
    Http2PriorityParam p;
    memset(&p, 0, sizeof p);
    p.stream_dep = 1;
    return !h2t_failed(st, "WritePriority",
                       burrow__http2_framer_write_priority(st->fr, 1, p));
}

/* No PRIORITY frame that depends on its own stream. */
static void TestServer_Rejects_PrioritySelfDependence(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_rejects_stream(t, HTTP2_ERR_CODE_PROTOCOL, h2t_send_priority_self_dep);
}

/* wantClosed: the next read finds the connection closed, not a frame and not
 * the read deadline. */
static void h2t_want_closed(H2tTester *st) {
    Error err = BURROW_NO_ERROR;
    Http2Frame *f = burrow__http2_framer_read_frame(st->fr, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(st->t, "got frame type %d, want closed connection",
                           f != NULL ? (int)f->header.type : -1);
    else if (errors_is(err, os_err_deadline_exceeded))
        testing_t_errorf_v(st->t, "connection is not closed; want it to be");
    burrow__http2_frame_free(f);
}

/* A frame one byte larger than the server reads. The server only reads its
 * header before it hangs up, so the rest stays in the tester's buffer. Go
 * moves its clock past GoAwayTimeout, and this waits for it. */
static void TestServer_RejectsLargeFrames(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    enum { N = (1 << 20) + 1 };
    H2tTester st;
    Byte *big = NULL;
    if (h2t_start(&st, t, NULL, NULL) && h2t_greet(&st)) {
        big = (Byte *)mem_alloc(st.a, N, 1);
        if (big == NULL) {
            testing_t_errorf_v(t, "no memory");
        } else {
            (void)burrow__http2_framer_write_raw_frame(
                st.fr, (Http2FrameType)0xff, 0, 0, slice_from(big, N, N, TYPE_BYTE));
            if (h2t_want_go_away(&st, 0, HTTP2_ERR_CODE_FRAME_SIZE))
                h2t_want_closed(&st);
        }
    }
    h2t_close(&st);
    if (big != NULL)
        mem_free(st.a, big, N, 1);
}

static bool h2t_write_connect(H2tTester *st) {
    static const Str kv[] = {S_(":method"), S_("CONNECT"), S_(":authority"),
                             S_("example.com:123")};
    return h2t_write_headers(st, 1, h2t_encode_header_raw(st, kv, 4), true, true);
}

static void h2t_check_connect(TestingT *t, HttpRequest *r) {
    if (!str_eq(r->method, BURROW_S("CONNECT")))
        testing_t_errorf_v(t, "Method = %q; want %q", r->method, BURROW_S("CONNECT"));
    if (!str_eq(r->request_uri, BURROW_S("example.com:123")))
        testing_t_errorf_v(t, "RequestURI = %q; want %q", r->request_uri,
                           BURROW_S("example.com:123"));
    if (!str_eq(r->url->host, BURROW_S("example.com:123")))
        testing_t_errorf_v(t, "URL.Host = %q; want %q", r->url->host,
                           BURROW_S("example.com:123"));
}

static void TestServer_Request_Connect(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_request(t, h2t_write_connect, h2t_check_connect);
}

static bool h2t_write_connect_path(H2tTester *st) {
    static const Str kv[] = {S_(":method"),         S_("CONNECT"), S_(":authority"),
                             S_("example.com:123"), S_(":path"),   S_("/bogus")};
    return h2t_write_headers(st, 1, h2t_encode_header_raw(st, kv, 6), true, true);
}

static void TestServer_Request_Connect_InvalidPath(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_rejects_stream(t, HTTP2_ERR_CODE_PROTOCOL, h2t_write_connect_path);
}

static bool h2t_write_connect_scheme(H2tTester *st) {
    static const Str kv[] = {S_(":method"),         S_("CONNECT"), S_(":authority"),
                             S_("example.com:123"), S_(":scheme"), S_("https")};
    return h2t_write_headers(st, 1, h2t_encode_header_raw(st, kv, 6), true, true);
}

static void TestServer_Request_Connect_InvalidScheme(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_rejects_stream(t, HTTP2_ERR_CODE_PROTOCOL, h2t_write_connect_scheme);
}

H2T_REJECT(TestServer_Request_Post_Body_ContentLength_EndStream, S_(":method"),
           S_("POST"), S_("content-length"), S_("3"))

/* The request's header block in chunks of 5 bytes, one HEADERS frame and
 * then CONTINUATION frames. */
static bool h2t_write_continued(H2tTester *st) {
    static const Str kv[] = {S_("foo-one"),   S_("value-one"), S_("foo-two"),
                             S_("value-two"), S_("foo-three"), S_("value-three")};
    Slice remain = h2t_encode_header(st, kv, 6);
    int chunks = 0;
    while (remain.len > 0) {
        Int n = remain.len < 5 ? remain.len : 5;
        Slice chunk = slice_sub(remain, 0, n);
        remain = slice_sub(remain, n, remain.len);
        if (chunks == 0) {
            /* no DATA frames, and CONTINUATION frames to come */
            if (!h2t_write_headers(st, 1, chunk, true, false))
                return false;
        } else if (h2t_failed(st, "WriteContinuation",
                              burrow__http2_framer_write_continuation(
                                  st->fr, 1, remain.len == 0, chunk))) {
            return false;
        }
        chunks++;
    }
    if (chunks < 2) {
        testing_t_errorf_v(st->t, "too few chunks");
        return false;
    }
    return true;
}

static void h2t_check_continued(TestingT *t, HttpRequest *r) {
    static const char *const keys[] = {"Foo-One", "Foo-Two", "Foo-Three"};
    static const char *const vals[] = {"value-one", "value-two", "value-three"};
    bool ok = map_len(r->header) == 3;
    for (int i = 0; ok && i < 3; i++) {
        Slice vs = http_header_values(r->header, str_from_cstr(keys[i]));
        ok = vs.len == 1 && str_eq(((const Str *)vs.p)[0], str_from_cstr(vals[i]));
    }
    if (!ok)
        testing_t_errorf_v(t, "Header has %d keys; want Foo-One, Foo-Two and Foo-Three",
                           map_len(r->header));
}

static void TestServer_Request_WithContinuation(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_request(t, h2t_write_continued, h2t_check_continued);
}

/* ------------------------------------------------------- stream limit */

enum { H2T_MAX_STREAMS = 250 }; /* defaultMaxStreams */

/* Each handler says which stream it has and waits for its own channel, which
 * stands in for Go's serverHandlerCall. */
typedef struct H2tCalls {
    Chan *started;
    Chan *exit[H2T_MAX_STREAMS + 2];
} H2tCalls;

static void h2t_handle_call(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    H2tCalls *c = (H2tCalls *)env;
    Int id = 0;
    Str p = r->url->path;
    for (Int i = 1; i < p.len && p.p[i] >= '0' && p.p[i] <= '9'; i++)
        id = id * 10 + (p.p[i] - '0');
    chan_send(c->started, &id);
    Int k = (id - 1) / 2;
    if (k >= 0 && k < (Int)(sizeof c->exit / sizeof c->exit[0])) {
        bool v;
        (void)chan_recv(c->exit[k], &v);
    }
}

static Slice h2t_encode_path(H2tTester *st, uint32_t id) {
    char path[16];
    int n = 0;
    char digits[12];
    int nd = 0;
    path[n++] = '/';
    do {
        digits[nd++] = (char)('0' + id % 10);
        id /= 10;
    } while (id > 0);
    while (nd > 0)
        path[n++] = digits[--nd];
    Str kv[2] = {BURROW_S(":path"), str_from_bytes(path, n)};
    return h2t_encode_header(st, kv, 2);
}

static bool h2t_next_call(H2tTester *st, H2tCalls *c, Int want) {
    Int id = 0;
    (void)chan_recv(c->started, &id);
    if (id != want) {
        testing_t_errorf_v(st->t, "Got request for /%d, want /%d", id, want);
        return false;
    }
    return true;
}

static void TestServer_Rejects_Too_Many_Streams(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tCalls c;
    memset(&c, 0, sizeof c);
    size_t nexit = sizeof c.exit / sizeof c.exit[0];
    c.started = chan_make(heap_allocator(), TYPE_INT, (Int)nexit);
    bool ok = c.started != NULL;
    for (size_t i = 0; i < nexit; i++) {
        c.exit[i] = chan_make(heap_allocator(), TYPE_BOOL, 1);
        ok = ok && c.exit[i] != NULL;
    }
    H2tTester st;
    if (!ok) {
        testing_t_errorf_v(t, "no memory");
        goto done;
    }
    if (!h2t_start(&st, t, h2t_handle_call, &c) || !h2t_greet(&st))
        goto out;
    uint32_t id = 1;
    for (int i = 0; i < H2T_MAX_STREAMS; i++, id += 2) {
        if (!h2t_write_headers(&st, id, h2t_encode_path(&st, id), true, true) ||
            !h2t_next_call(&st, &c, (Int)id))
            goto out;
    }

    /* This one crosses the limit. It goes as HEADERS and a CONTINUATION, to
     * check that the decoder's state is still kept for a stream turned away. */
    uint32_t reject_id = id;
    id += 2;
    Slice block = h2t_encode_path(&st, reject_id);
    if (!h2t_write_headers(&st, reject_id, slice_sub(block, 0, 3), true, false) ||
        h2t_failed(&st, "WriteContinuation",
                   burrow__http2_framer_write_continuation(
                       st.fr, reject_id, true, slice_sub(block, 3, block.len))) ||
        !h2t_want_rst_stream(&st, reject_id, HTTP2_ERR_CODE_PROTOCOL))
        goto out;

    /* Let one handler finish. */
    bool v = true;
    chan_send(c.exit[0], &v);
    if (!h2t_want_headers(&st, 1, true, NULL, 0))
        goto out;

    /* And now another stream can start. */
    uint32_t good_id = id;
    if (h2t_write_headers(&st, good_id, h2t_encode_path(&st, good_id), true, true))
        (void)h2t_next_call(&st, &c, (Int)good_id);

out:
    for (size_t i = 0; i < nexit; i++)
        chan_close(c.exit[i]);
    h2t_close(&st);
done:
    chan_free(c.started);
    for (size_t i = 0; i < nexit; i++)
        chan_free(c.exit[i]);
}

/* ------------------------------------------------- more responses */

/* The test a handler given to h2t_server_response reports to, which Go's
 * handlers do by returning an error. */
static TestingT *h2t_resp_t;

static void h2t_server_response_t(TestingT *t, H2tRespFn handler, H2tClientFn client) {
    h2t_resp_t = t;
    h2t_server_response(t, handler, client);
    h2t_resp_t = NULL;
}

#define H2T_HTML "<html>this is HTML."

static bool h2t_want_settings_ack(H2tTester *st) {
    Http2Frame *f = h2t_read_kind(st, HTTP2_SETTINGS_FRAME);
    if (f == NULL)
        return false;
    bool ack = burrow__http2_settings_frame_is_ack(f);
    burrow__http2_frame_free(f);
    if (!ack)
        testing_t_errorf_v(st->t, "Settings Frame didn't have ACK set");
    return ack;
}

static bool h2t_write_settings(H2tTester *st, const Http2Setting *s, Int n) {
    return !h2t_failed(st, "WriteSettings",
                       burrow__http2_framer_write_settings(st->fr, s, n)) &&
           h2t_want_settings_ack(st);
}

static bool h2t_write_window_update(H2tTester *st, uint32_t id, uint32_t n) {
    return !h2t_failed(st, "WriteWindowUpdate",
                       burrow__http2_framer_write_window_update(st->fr, id, n));
}

/* wantData with a size and no data: one DATA frame of n bytes. */
static bool h2t_want_data_size(H2tTester *st, uint32_t id, bool end_stream, Int n) {
    Http2Frame *f = h2t_read_kind(st, HTTP2_DATA_FRAME);
    if (f == NULL)
        return false;
    bool ended = (f->header.flags & HTTP2_FLAG_DATA_END_STREAM) != 0;
    bool ok =
        f->header.stream_id == id && ended == end_stream && f->u.data.data.len == n;
    if (!ok)
        testing_t_errorf_v(st->t,
                           "got DATA stream %d end %t of %d bytes; want stream %d end "
                           "%t of %d bytes",
                           (int)f->header.stream_id, ended, f->u.data.data.len, (int)id,
                           end_stream, n);
    burrow__http2_frame_free(f);
    return ok;
}

static void h2t_handle_foo_bar_type(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    (void)http_header_set(http_response_writer_header(w), BURROW_S("Content-Type"),
                          BURROW_S("foo/bar"));
    h2t_write_str(w, H2T_HTML);
}

static bool h2t_client_foo_bar_type(H2tTester *st) {
    static const Str want[] = {S_(":status"),        S_("200"),
                               S_("content-type"),   S_("foo/bar"),
                               S_("content-length"), S_("19")};
    return h2t_get_slash(st) &&
           h2t_want_headers(st, 1, false, want, sizeof want / sizeof want[0]) &&
           h2t_want_data(st, 1, true, H2T_HTML);
}

static void TestServer_Response_Data_Sniff_DoesntOverride(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_foo_bar_type, h2t_client_foo_bar_type);
}

static const Str h2t_html_want[] = {
    S_(":status"),        S_("200"), S_("content-type"), S_("text/html; charset=utf-8"),
    S_("content-length"), S_("19")};

static void h2t_handle_ignore_after(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    h2t_write_str(w, H2T_HTML);
    (void)http_header_set(http_response_writer_header(w), BURROW_S("foo"),
                          BURROW_S("should be ignored"));
}

static bool h2t_client_html(H2tTester *st) {
    return h2t_get_slash(st) &&
           h2t_want_headers(st, 1, false, h2t_html_want,
                            sizeof h2t_html_want / sizeof h2t_html_want[0]);
}

/* Header looked at only after the first write. */
static void TestServer_Response_Data_IgnoreHeaderAfterWrite_After(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_ignore_after, h2t_client_html);
}

static void h2t_handle_ignore_overwrite(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    HttpHeader h = http_response_writer_header(w);
    (void)http_header_set(h, BURROW_S("foo"), BURROW_S("proper value"));
    h2t_write_str(w, H2T_HTML);
    (void)http_header_set(h, BURROW_S("foo"), BURROW_S("should be ignored"));
}

static bool h2t_client_overwrite(H2tTester *st) {
    static const Str want[] = {S_(":status"),
                               S_("200"),
                               S_("foo"),
                               S_("proper value"),
                               S_("content-type"),
                               S_("text/html; charset=utf-8"),
                               S_("content-length"),
                               S_("19")};
    return h2t_get_slash(st) &&
           h2t_want_headers(st, 1, false, want, sizeof want / sizeof want[0]);
}

/* Header looked at before the first write and changed after it. */
static void TestServer_Response_Data_IgnoreHeaderAfterWrite_Overwrite(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_ignore_overwrite, h2t_client_overwrite);
}

static void h2t_handle_html(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    h2t_write_str(w, H2T_HTML);
}

static bool h2t_client_html_data(H2tTester *st) {
    return h2t_client_html(st) && h2t_want_data(st, 1, true, H2T_HTML);
}

static void TestServer_Response_Data_SniffLenType(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_html, h2t_client_html_data);
}

#define H2T_MSG "<html>this is HTML"
#define H2T_MSG2 ", and this is the next chunk"

static void h2t_handle_flush_mid_write(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    h2t_write_str(w, H2T_MSG);
    (void)w.vt->flush(w.data);
    h2t_write_str(w, H2T_MSG2);
}

static bool h2t_client_flush_mid_write(H2tTester *st) {
    /* sniffed, and no content-length */
    static const Str want[] = {S_(":status"), S_("200"), S_("content-type"),
                               S_("text/html; charset=utf-8")};
    return h2t_get_slash(st) &&
           h2t_want_headers(st, 1, false, want, sizeof want / sizeof want[0]) &&
           h2t_want_data(st, 1, false, H2T_MSG) && h2t_want_data(st, 1, true, H2T_MSG2);
}

static void TestServer_Response_Header_Flush_MidWrite(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_flush_mid_write, h2t_client_flush_mid_write);
}

/* A handler's write of n bytes of 'a', with a flush first when flush is set,
 * reporting whether it failed. */
static bool h2t_write_as(HttpResponseWriter w, Int n, bool flush, Error *err) {
    if (flush)
        (void)w.vt->flush(w.data);
    Byte *p = (Byte *)mem_alloc_nozero(heap_allocator(), (size_t)n, 1);
    if (p == NULL) {
        testing_t_errorf_v(h2t_resp_t, "no memory");
        return false;
    }
    memset(p, 'a', (size_t)n);
    *err = BURROW_NO_ERROR;
    Int got = http_response_writer_write(w, slice_from(p, n, n, TYPE_BYTE), err);
    mem_free(heap_allocator(), p, (size_t)n, 1);
    if (!BURROW_FAILED(*err) && got != n)
        testing_t_errorf_v(h2t_resp_t, "Error in handler: wrong size %d from Write",
                           got);
    return !BURROW_FAILED(*err);
}

enum { H2T_LARGE = 1 << 20, H2T_MAX_FRAME = 16 << 10 };

static void h2t_handle_large_write(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    Error err = BURROW_NO_ERROR;
    if (!h2t_write_as(w, H2T_LARGE, false, &err))
        testing_t_errorf_v(h2t_resp_t, "Error in handler: Write error: %v", err);
}

static const Http2Setting h2t_no_window_small_frames[] = {
    {HTTP2_SETTING_INITIAL_WINDOW_SIZE, 0},
    {HTTP2_SETTING_MAX_FRAME_SIZE, H2T_MAX_FRAME}};

static bool h2t_client_large_write(H2tTester *st) {
    /* sniffed, and no content-length */
    static const Str want[] = {S_(":status"), S_("200"), S_("content-type"),
                               S_("text/plain; charset=utf-8")};
    if (!h2t_write_settings(st, h2t_no_window_small_frames, 2) || !h2t_get_slash(st) ||
        /* Quota for the handler to write, on the stream and on the connection. */
        !h2t_write_window_update(st, 1, H2T_LARGE) ||
        !h2t_write_window_update(st, 0, H2T_LARGE) ||
        !h2t_want_headers(st, 1, false, want, sizeof want / sizeof want[0]))
        return false;
    Int bytes = 0;
    Int frames = 0;
    for (;;) {
        Http2Frame *f = h2t_read_kind(st, HTTP2_DATA_FRAME);
        if (f == NULL)
            return false;
        Slice d = f->u.data.data;
        bytes += d.len;
        frames++;
        bool non_a = false;
        for (Int i = 0; i < d.len; i++)
            non_a = non_a || ((const Byte *)d.p)[i] != 'a';
        bool ended = (f->header.flags & HTTP2_FLAG_DATA_END_STREAM) != 0;
        burrow__http2_frame_free(f);
        if (non_a) {
            testing_t_errorf_v(st->t, "non-'a' byte seen in DATA");
            return false;
        }
        if (ended)
            break;
    }
    if (bytes != H2T_LARGE)
        testing_t_errorf_v(st->t, "Got %d bytes; want %d", bytes, (Int)H2T_LARGE);
    Int want_frames = H2T_LARGE / H2T_MAX_FRAME;
    if (frames < want_frames || frames > want_frames * 2)
        testing_t_errorf_v(st->t, "Got %d frames; want %d", frames, want_frames);
    return true;
}

static void TestServer_Response_LargeWrite(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response_t(t, h2t_handle_large_write, h2t_client_large_write);
}

/* Before each read the client gives exactly enough window for it. */
static const Int h2t_reads[] = {123, 1, 13, 127};

static void h2t_handle_flow_controlled(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    Error err = BURROW_NO_ERROR;
    if (!h2t_write_as(w, 123 + 1 + 13 + 127, true, &err))
        testing_t_errorf_v(h2t_resp_t, "Error in handler: Write error: %v", err);
}

static bool h2t_client_flow_controlled(H2tTester *st) {
    /* The window, and so how much comes before the first WINDOW_UPDATE. */
    Http2Setting s = {HTTP2_SETTING_INITIAL_WINDOW_SIZE, (uint32_t)h2t_reads[0]};
    if (!h2t_write_settings(st, &s, 1) || !h2t_get_slash(st) ||
        !h2t_want_headers(st, 1, false, NULL, 0) ||
        !h2t_want_data_size(st, 1, false, h2t_reads[0]))
        return false;
    size_t n = sizeof h2t_reads / sizeof h2t_reads[0];
    for (size_t i = 1; i < n; i++) {
        if (!h2t_write_window_update(st, 1, (uint32_t)h2t_reads[i]) ||
            !h2t_want_data_size(st, 1, i == n - 1, h2t_reads[i]))
            return false;
    }
    return true;
}

/* The handler can't write more than the client allows. */
static void TestServer_Response_LargeWrite_FlowControlled(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response_t(t, h2t_handle_flow_controlled, h2t_client_flow_controlled);
}

static void h2t_handle_rst_unblocks(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    Error err = BURROW_NO_ERROR;
    if (h2t_write_as(w, H2T_LARGE, true, &err))
        testing_t_errorf_v(
            h2t_resp_t, "Error in handler: unexpected nil error from Write in handler");
}

static bool h2t_client_rst_unblocks(H2tTester *st) {
    return h2t_write_settings(st, h2t_no_window_small_frames, 2) && h2t_get_slash(st) &&
           h2t_want_headers(st, 1, false, NULL, 0) &&
           !h2t_failed(
               st, "WriteRSTStream",
               burrow__http2_framer_write_rst_stream(st->fr, 1, HTTP2_ERR_CODE_CANCEL));
}

/* A handler blocked in a write is let go when the client resets the stream. */
static void TestServer_Response_RST_Unblocks_LargeWrite(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response_t(t, h2t_handle_rst_unblocks, h2t_client_rst_unblocks);
}

static void h2t_handle_flush_only(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    (void)w.vt->flush(w.data);
    /* Nothing, so an empty DATA frame. */
}

static bool h2t_client_empty_data(H2tTester *st) {
    /* No window for the handler. */
    Http2Setting s = {HTTP2_SETTING_INITIAL_WINDOW_SIZE, 0};
    return h2t_write_settings(st, &s, 1) && h2t_get_slash(st) &&
           h2t_want_headers(st, 1, false, NULL, 0) &&
           h2t_want_data_size(st, 1, true, 0);
}

static void TestServer_Response_Empty_Data_Not_FlowControlled(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_flush_only, h2t_client_empty_data);
}

static void h2t_handle_100_continue(HttpResponseWriter w, HttpRequest *r) {
    Str v = http_header_get(r->header, BURROW_S("Expect"));
    if (v.len > 0)
        testing_t_errorf_v(h2t_resp_t, "Expect header = %q; want empty", v);
    /* This read is what sends the 100-continue. */
    Byte buf[3];
    Error err = BURROW_NO_ERROR;
    Int n = io_read_full(io_read_closer_as_io_reader(r->body),
                         slice_from(buf, 3, 3, TYPE_BYTE), &err);
    if (BURROW_FAILED(err) || n != 3 || memcmp(buf, "foo", 3) != 0) {
        testing_t_errorf_v(h2t_resp_t,
                           "Error in handler: ReadFull = %q, %v; want %q, nil",
                           str_from_bytes(buf, n), err, BURROW_S("foo"));
        return;
    }
    h2t_write_str(w, "bar");
}

static bool h2t_client_100_continue(H2tTester *st) {
    static const Str kv[] = {S_(":method"), S_("POST"), S_("expect"),
                             S_("100-Continue")};
    static const Str want100[] = {S_(":status"), S_("100")};
    static const Str want[] = {S_(":status"),        S_("200"),
                               S_("content-type"),   S_("text/plain; charset=utf-8"),
                               S_("content-length"), S_("3")};
    /* With the 100 in, the client can send its gigantic and/or sensitive "foo". */
    return h2t_write_headers(st, 1, h2t_encode_header(st, kv, 4), false, true) &&
           h2t_want_headers(st, 1, false, want100, 2) &&
           h2t_write_data(st, 1, true, "foo", 3) &&
           h2t_want_headers(st, 1, false, want, sizeof want / sizeof want[0]) &&
           h2t_want_data(st, 1, true, "bar");
}

static void TestServer_Response_Automatic100Continue(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response_t(t, h2t_handle_100_continue, h2t_client_100_continue);
}

/* A header keeps the value it's given rather than a copy, so the values live
 * here, past the handler's return. */
static char h2t_many_values[5000][16];

static void h2t_handle_many_headers(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    HttpHeader h = http_response_writer_header(w);
    char k[24];
    for (int i = 0; i < 5000; i++) {
        char *v = h2t_many_values[i];
        int kn = snprintf(k, sizeof k, "x-header-%d", i);
        int vn = snprintf(v, sizeof h2t_many_values[i], "x-value-%d", i);
        (void)http_header_set(h, str_from_bytes(k, kn), str_from_bytes(v, vn));
    }
}

/* Read as frames of their own rather than as one block, so the client's
 * framer stops putting them together for this. */
static bool h2t_client_many_headers(H2tTester *st) {
    if (!h2t_get_slash(st))
        return false;
    st->fr->read_meta_headers = NULL;
    Http2Frame *f = h2t_read_kind(st, HTTP2_HEADERS_FRAME);
    if (f == NULL)
        return false;
    bool ended = (f->header.flags & HTTP2_FLAG_HEADERS_END_HEADERS) != 0;
    burrow__http2_frame_free(f);
    if (ended) {
        testing_t_errorf_v(st->t, "got unwanted END_HEADERS flag");
        return false;
    }
    int n = 0;
    for (;;) {
        n++;
        f = h2t_read_kind(st, HTTP2_CONTINUATION_FRAME);
        if (f == NULL)
            return false;
        ended = (f->header.flags & HTTP2_FLAG_CONTINUATION_END_HEADERS) != 0;
        burrow__http2_frame_free(f);
        if (ended)
            break;
    }
    if (n < 5)
        testing_t_errorf_v(st->t, "Only got %d CONTINUATION frames; expected 5+", n);
    return true;
}

/* So many response headers that the server needs CONTINUATION frames. */
static void TestServer_Response_ManyHeaders_With_Continuation(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_many_headers, h2t_client_many_headers);
}

/* A handler that says it has started, then waits for block to close, which
 * stands in for the handler calls Go's tester keeps when it has no handler. */
typedef struct H2tBlock {
    Chan *in;
    Chan *block;
} H2tBlock;

static void h2t_block_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    (void)r;
    H2tBlock *b = (H2tBlock *)env;
    bool v = true;
    (void)chan_try_send(b->in, &v);
    (void)chan_recv(b->block, &v);
}

static bool h2t_block_make(TestingT *t, H2tBlock *b) {
    b->in = chan_make(heap_allocator(), TYPE_BOOL, 1);
    b->block = chan_make(heap_allocator(), TYPE_BOOL, 0);
    if (b->in != NULL && b->block != NULL)
        return true;
    testing_t_errorf_v(t, "no memory");
    return false;
}

static void h2t_block_free(H2tBlock *b) {
    if (b->in != NULL)
        chan_free(b->in);
    if (b->block != NULL)
        chan_free(b->block);
}

static void TestServer_Send_GoAway_After_Bogus_WindowUpdate(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tTester st;
    if (h2t_start(&st, t, NULL, NULL) && h2t_greet(&st) &&
        h2t_write_window_update(&st, 0, (1U << 31) - 1))
        (void)h2t_want_go_away(&st, 0, HTTP2_ERR_CODE_FLOW_CONTROL);
    h2t_close(&st);
}

/* A POST that stays open, with its handler waiting, and then a window update
 * that takes the stream's window past the limit. */
static void TestServer_Send_RstStream_After_Bogus_WindowUpdate(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tBlock b;
    if (!h2t_block_make(t, &b)) {
        h2t_block_free(&b);
        return;
    }
    H2tTester st;
    if (h2t_start(&st, t, h2t_block_handler, &b) && h2t_greet(&st) &&
        h2t_write_headers(&st, 1, h2t_encode_header(&st, h2t_post, 2), false, true)) {
        bool v;
        (void)chan_recv(b.in, &v);
        if (h2t_write_window_update(&st, 1, (1U << 31) - 1))
            (void)h2t_want_rst_stream(&st, 1, HTTP2_ERR_CODE_FLOW_CONTROL);
    }
    chan_close(b.block);
    h2t_close(&st);
    h2t_block_free(&b);
}

typedef struct H2tCLSign {
    Str name;
    Str cl;
    Str want_cl;
    int64_t want;
} H2tCLSign;

static const H2tCLSign h2t_cl_sign[] = {
    {S_("proper content-length"), S_("3"), S_("3"), 3},
    {S_("ignore cl with plus sign"), S_("+3"), S_("0"), 0},
    {S_("ignore cl with minus sign"), S_("-3"), S_("0"), 0},
    {S_("max int64, for safe uint64->int64 conversion"), S_("9223372036854775807"),
     S_("9223372036854775807"), INT64_MAX},
    {S_("overflows int64, so ignored"), S_("9223372036854775808"), S_("0"), 0},
};

/* The case the handler and client below are on. */
static const H2tCLSign *h2t_cl;

static void h2t_handle_cl_sign(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    (void)http_header_set(http_response_writer_header(w), BURROW_S("content-length"),
                          h2t_cl->cl);
}

static bool h2t_client_cl_sign(H2tTester *st) {
    Str want[] = {BURROW_S(":status"), BURROW_S("200"), BURROW_S("content-length"),
                  h2t_cl->want_cl};
    return h2t_get_slash(st) && h2t_want_headers(st, 1, true, want, 4);
}

static void TestServerIgnoresContentLengthSignWhenWritingChunks(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    for (size_t i = 0; i < sizeof h2t_cl_sign / sizeof h2t_cl_sign[0]; i++) {
        h2t_cl = &h2t_cl_sign[i];
        h2t_server_response(t, h2t_handle_cl_sign, h2t_client_cl_sign);
    }
}

static bool h2t_write_req_cl_sign(H2tTester *st) {
    Str kv[] = {BURROW_S("content-length"), h2t_cl->cl};
    return h2t_write_headers(st, 1, h2t_encode_header(st, kv, 2), false, true) &&
           h2t_write_data(st, 1, false, "", 0);
}

static void h2t_check_req_cl_sign(TestingT *t, HttpRequest *r) {
    if (r->content_length != h2t_cl->want)
        testing_t_errorf_v(t, "Got: %d\nWant: %d", (Int)r->content_length,
                           (Int)h2t_cl->want);
}

static void h2t_req_cl_sign(void *env, TestingT *t) {
    h2t_cl = (const H2tCLSign *)env;
    h2t_server_request(t, h2t_write_req_cl_sign, h2t_check_req_cl_sign);
}

/* Reject content-length headers containing a sign. See
 * https://golang.org/issue/39017 */
static void TestServerRejectsContentLengthWithSignNewRequests(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    for (size_t i = 0; i < sizeof h2t_cl_sign / sizeof h2t_cl_sign[0]; i++)
        testing_t_run(t, h2t_cl_sign[i].name,
                      BURROW_FN(TestingTFunc, h2t_req_cl_sign,
                                (void *)(uintptr_t)&h2t_cl_sign[i]));
}

static void h2t_handle_200(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    http_response_writer_write_header(w, 200);
}

typedef struct H2tCLDup {
    Str name;
    Str values[3];
    size_t n;
    bool ok;
} H2tCLDup;

static void h2t_cl_dup(void *env, TestingT *t) {
    const H2tCLDup *c = (const H2tCLDup *)env;
    H2tTester st;
    if (h2t_start(&st, t, h2t_handle_200, NULL) && h2t_greet(&st)) {
        Str kv[8];
        size_t n = 0;
        kv[n++] = BURROW_S(":method");
        kv[n++] = BURROW_S("GET");
        for (size_t i = 0; i < c->n; i++) {
            kv[n++] = BURROW_S("content-length");
            kv[n++] = c->values[i];
        }
        if (h2t_write_headers(&st, 1, h2t_encode_header(&st, kv, n), false, true)) {
            static const Str want[] = {S_(":status"), S_("200")};
            if (c->ok)
                (void)h2t_want_headers(&st, 1, true, want, 2);
            else
                (void)h2t_want_rst_stream(&st, 1, HTTP2_ERR_CODE_PROTOCOL);
        }
    }
    h2t_close(&st);
}

static void TestServerContentLengthDuplicates(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    static const H2tCLDup tests[] = {
        {S_("single value"), {S_("123")}, 1, true},
        {S_("identical duplicate values"), {S_("123"), S_("123"), S_("123")}, 3, true},
        {S_("identical duplicate values with extra whitespace"),
         {S_("123"), S_(" 123"), S_("123")},
         3,
         false},
        {S_("different duplicate values"), {S_("123"), S_("321"), S_("123")}, 3, false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        testing_t_run(
            t, tests[i].name,
            BURROW_FN(TestingTFunc, h2t_cl_dup, (void *)(uintptr_t)&tests[i]));
}

static void h2t_handle_invalid_headers(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    HttpHeader h = http_response_writer_header(w);
    (void)http_header_add(h, BURROW_S("OK1"), BURROW_S("x"));
    (void)http_header_add(h, BURROW_S("Bad:Colon"), BURROW_S("x")); /* colon in key */
    (void)http_header_add(h, BURROW_S("Bad1\x00"), BURROW_S("x"));  /* null in key */
    (void)http_header_add(h, BURROW_S("Bad2"), BURROW_S("x\x00y")); /* null in value */
}

static bool h2t_client_invalid_headers(H2tTester *st) {
    static const Str want[] = {S_(":status"),        S_("200"), S_("ok1"), S_("x"),
                               S_("content-length"), S_("0")};
    return h2t_get_slash(st) &&
           h2t_want_headers(st, 1, true, want, sizeof want / sizeof want[0]);
}

static void TestServerDoesntWriteInvalidHeaders(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_invalid_headers, h2t_client_invalid_headers);
}

static void TestServerNoAutoContentLengthOnHead(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    static const Str head[] = {S_(":method"), S_("HEAD")};
    static const Str want[] = {S_(":status"), S_("200")};
    H2tTester st;
    if (h2t_start(&st, t, NULL, NULL) && h2t_greet(&st) &&
        h2t_write_headers(&st, 1, h2t_encode_header(&st, head, 2), true, true))
        (void)h2t_want_headers(&st, 1, true, want, 2);
    h2t_close(&st);
}

static void h2t_handle_empty_content_type(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    (void)http_header_set(http_response_writer_header(w), BURROW_S("Content-Type"),
                          BURROW_S(""));
    h2t_write_str(w, "<html><head></head><body>hi</body></html>");
}

static bool h2t_client_empty_content_type(H2tTester *st) {
    static const Str want[] = {S_(":status"),        S_("200"),
                               S_("content-type"),   S_(""),
                               S_("content-length"), S_("41")};
    return h2t_get_slash(st) &&
           h2t_want_headers(st, 1, false, want, sizeof want / sizeof want[0]);
}

/* golang.org/issue/13495 */
static void TestServerNoDuplicateContentType(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_empty_content_type,
                        h2t_client_empty_content_type);
}

static void h2t_handle_no_content_length(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    Str key = BURROW_S("Content-Length");
    Slice none = slice_from(NULL, 0, 0, TYPE_STRING);
    (void)map_set(http_response_writer_header(w), &key, &none);
    h2t_write_str(w, "OK");
}

static bool h2t_client_no_content_length(H2tTester *st) {
    static const Str want[] = {S_(":status"), S_("200"), S_("content-type"),
                               S_("text/plain; charset=utf-8")};
    return h2t_get_slash(st) &&
           h2t_want_headers(st, 1, false, want, sizeof want / sizeof want[0]);
}

static void TestServerContentLengthCanBeDisabled(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_no_content_length, h2t_client_no_content_length);
}

static void h2t_handle_processing(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    http_response_writer_write_header(w, HTTP_STATUS_PROCESSING);
    h2t_write_str(w, "stuff");
}

static bool h2t_client_processing(H2tTester *st) {
    static const Str want1[] = {S_(":status"), S_("102")};
    static const Str want2[] = {S_(":status"),        S_("200"),
                                S_("content-type"),   S_("text/plain; charset=utf-8"),
                                S_("content-length"), S_("5")};
    return h2t_get_slash(st) && h2t_want_headers(st, 1, false, want1, 2) &&
           h2t_want_headers(st, 1, false, want2, sizeof want2 / sizeof want2[0]);
}

static void TestServerSendsProcessing(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_processing, h2t_client_processing);
}

#define H2T_LINK_STYLE "</style.css>; rel=preload; as=style"
#define H2T_LINK_SCRIPT "</script.js>; rel=preload; as=script"
#define H2T_LINK_FOO "</foo.js>; rel=preload; as=script"

static void h2t_handle_early_hints(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    HttpHeader h = http_response_writer_header(w);
    (void)http_header_add(h, BURROW_S("Content-Length"), BURROW_S("123"));
    (void)http_header_add(h, BURROW_S("Link"), BURROW_S(H2T_LINK_STYLE));
    (void)http_header_add(h, BURROW_S("Link"), BURROW_S(H2T_LINK_SCRIPT));
    http_response_writer_write_header(w, HTTP_STATUS_EARLY_HINTS);

    (void)http_header_add(h, BURROW_S("Link"), BURROW_S(H2T_LINK_FOO));
    http_response_writer_write_header(w, HTTP_STATUS_EARLY_HINTS);

    h2t_write_str(w, "stuff");
}

static bool h2t_client_early_hints(H2tTester *st) {
    static const Str want1[] = {S_(":status"),      S_("103"),  S_("link"),
                                S_(H2T_LINK_STYLE), S_("link"), S_(H2T_LINK_SCRIPT)};
    static const Str want2[] = {
        S_(":status"), S_("103"),           S_("link"), S_(H2T_LINK_STYLE),
        S_("link"),    S_(H2T_LINK_SCRIPT), S_("link"), S_(H2T_LINK_FOO)};
    static const Str want3[] = {S_(":status"),        S_("200"),
                                S_("link"),           S_(H2T_LINK_STYLE),
                                S_("link"),           S_(H2T_LINK_SCRIPT),
                                S_("link"),           S_(H2T_LINK_FOO),
                                S_("content-type"),   S_("text/plain; charset=utf-8"),
                                S_("content-length"), S_("123")};
    return h2t_get_slash(st) &&
           h2t_want_headers(st, 1, false, want1, sizeof want1 / sizeof want1[0]) &&
           h2t_want_headers(st, 1, false, want2, sizeof want2 / sizeof want2[0]) &&
           h2t_want_headers(st, 1, false, want3, sizeof want3 / sizeof want3[0]);
}

static void TestServerSendsEarlyHints(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_early_hints, h2t_client_early_hints);
}

static const Str h2t_bad_paths[] = {S_(""), S_("\x00"), S_("https://example.com/")};

/* The path the write below sends. */
static Str h2t_bad_path;

static bool h2t_write_bad_path(H2tTester *st) {
    st->fr->allow_illegal_writes = true;
    Str kv[] = {BURROW_S(":path"), h2t_bad_path};
    return h2t_write_headers(st, 1, h2t_encode_header(st, kv, 2), true, true);
}

static void TestServerInvalidPathHeader(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    for (size_t i = 0; i < sizeof h2t_bad_paths / sizeof h2t_bad_paths[0]; i++) {
        h2t_bad_path = h2t_bad_paths[i];
        h2t_rejects_stream(t, HTTP2_ERR_CODE_PROTOCOL, h2t_write_bad_path);
    }
}

#define H2T_SLASHES_PATH "//narf.com/path"

static bool h2t_write_initial_slashes(H2tTester *st) {
    static const Str kv[] = {S_(":path"), S_(H2T_SLASHES_PATH)};
    return h2t_write_headers(st, 1, h2t_encode_header(st, kv, 2), true, true);
}

static void h2t_check_initial_slashes(TestingT *t, HttpRequest *r) {
    if (!str_eq(r->url->host, BURROW_S("")))
        testing_t_errorf_v(t, "got req.URL.Host %q, want %q", r->url->host,
                           BURROW_S(""));
    if (!str_eq(r->url->path, BURROW_S(H2T_SLASHES_PATH)))
        testing_t_errorf_v(t, "got req.URL.Path %q, want %q", r->url->path,
                           BURROW_S(H2T_SLASHES_PATH));
}

/* The path goes through as it is, not as a URL relative to the protocol and
 * not with its first slashes taken off. */
static void TestServerPathInitialSlashes(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_request(t, h2t_write_initial_slashes, h2t_check_initial_slashes);
}

enum { H2T_WINDOW_INCREASE = 1000 };
#define H2T_MAX_WINDOW ((1U << 31) - 1) /* RFC 9113, 6.9.1 */

/* A POST with its data still to come, and some more window for it. Then a
 * SETTINGS frame moves the initial window to over_by past what the stream can
 * take. */
static void h2t_settings_flow_control(TestingT *t, uint32_t over_by) {
    H2tBlock b;
    if (!h2t_block_make(t, &b)) {
        h2t_block_free(&b);
        return;
    }
    H2tTester st;
    if (h2t_start(&st, t, h2t_block_handler, &b) && h2t_greet(&st) &&
        h2t_write_headers(&st, 1, h2t_encode_header(&st, h2t_post, 2), false, true) &&
        h2t_write_window_update(&st, 1, H2T_WINDOW_INCREASE)) {
        Http2Setting s = {HTTP2_SETTING_INITIAL_WINDOW_SIZE,
                          H2T_MAX_WINDOW - H2T_WINDOW_INCREASE + over_by};
        if (over_by == 0)
            (void)h2t_write_settings(&st, &s, 1);
        else if (!h2t_failed(&st, "WriteSettings",
                             burrow__http2_framer_write_settings(st.fr, &s, 1)))
            (void)h2t_want_go_away(&st, 1, HTTP2_ERR_CODE_FLOW_CONTROL);
    }
    chan_close(b.block);
    h2t_close(&st);
    h2t_block_free(&b);
}

/* "An endpoint MUST treat a change to SETTINGS_INITIAL_WINDOW_SIZE that causes
 * any flow-control window to exceed the maximum size as a connection error
 * (Section 5.4.1) of type FLOW_CONTROL_ERROR."
 * -- https://www.rfc-editor.org/rfc/rfc9113.html#section-6.9.2-7 */
static void TestServerSettingsFlowControlUpdateBeyondLimit(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_settings_flow_control(t, 1);
}

/* A SETTINGS update which doesn't quite put a stream over the limit. */
static void TestServerSettingsFlowControlUpdateWithinLimit(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_settings_flow_control(t, 0);
}

/* wantFrameType. */
static bool h2t_want_frame_type(H2tTester *st, Http2FrameKind k) {
    Http2Frame *f = h2t_read_kind(st, k);
    bool ok = f != NULL;
    burrow__http2_frame_free(f);
    return ok;
}

static void h2t_hello_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    h2t_write_str(w, "hello");
}

/* Go's fake connection takes what the server writes without waiting for the
 * test to read it, so the server's SETTINGS and WINDOW_UPDATE are out before it
 * reads a bad frame. A net_pipe write waits for the reader, and a server that
 * reads the bad frame first sends its GOAWAY and drops the WINDOW_UPDATE, as
 * Go's does. So the bad bytes go after the test has read the two frames. */
static bool h2t_write_after_greeting(H2tTester *st, const char *p, Int n) {
    Error err = BURROW_NO_ERROR;
    if (!h2t_want_frame_type(st, HTTP2_SETTINGS_FRAME) ||
        !h2t_want_frame_type(st, HTTP2_WINDOW_UPDATE_FRAME))
        return false;
    (void)h2t_out_write(st, slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE), &err);
    return !h2t_failed(st, "writing", err);
}

/* A HEADERS frame on stream 0x696e6669 with a length of 0, right after the
 * preface. Go moves its clock past GoAwayTimeout before it looks for the
 * close, and this waits for it. */
static void TestIssue53(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    static const char data[] = "\x00\x00\x00\x01\ainfinfin\ad";
    H2tTester st;
    if (h2t_start(&st, t, h2t_hello_handler, NULL) && h2t_write_preface(&st) &&
        h2t_write_after_greeting(&st, data, (Int)sizeof data - 1) &&
        h2t_want_frame_type(&st, HTTP2_GO_AWAY_FRAME))
        h2t_want_closed(&st);
    h2t_close(&st);
}

typedef Error (*H2tUnblockFn)(HttpResponseWriter w, HttpRequest *r);
typedef void (*H2tCheckErrFn)(TestingT *t, Error err);

typedef struct H2tUnblock {
    TestingT *t;
    Chan *in;
    Chan *done;
    H2tUnblockFn handler;
    H2tCheckErrFn check;
} H2tUnblock;

static void h2t_unblock_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    H2tUnblock *u = (H2tUnblock *)env;
    bool v = true;
    chan_send(u->in, &v);
    Error err = u->handler(w, r);
    if (u->check != NULL)
        u->check(u->t, err);
    chan_send(u->done, &v);
}

/* testServerPostUnblock: a POST that stays open, with other's headers too,
 * and a handler that waits in handler. Then fn, after which handler has to
 * return. Go checks the error handler gives back on the test's goroutine, and
 * this checks it on the handler's. */
static void h2t_server_post_unblock(TestingT *t, H2tUnblockFn handler, H2tFrameFn fn,
                                    H2tCheckErrFn check, const Str *other,
                                    size_t nother) {
    H2tUnblock u = {t, chan_make(heap_allocator(), TYPE_BOOL, 1),
                    chan_make(heap_allocator(), TYPE_BOOL, 1), handler, check};
    if (u.in == NULL || u.done == NULL) {
        testing_t_errorf_v(t, "no memory");
    } else {
        Str kv[6] = {BURROW_S(":method"), BURROW_S("POST")};
        size_t n = 2;
        for (size_t i = 0; i < nother && n < sizeof kv / sizeof kv[0]; i++)
            kv[n++] = other[i];
        H2tTester st;
        if (h2t_start(&st, t, h2t_unblock_handler, &u) && h2t_greet(&st) &&
            h2t_write_headers(&st, 1, h2t_encode_header(&st, kv, n), false, true)) {
            bool v;
            (void)chan_recv(u.in, &v);
            if (fn(&st))
                (void)chan_recv(u.done, &v);
        }
        h2t_close(&st);
    }
    if (u.in != NULL)
        chan_free(u.in);
    if (u.done != NULL)
        chan_free(u.done);
}

static Error h2t_block_until_closed(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    Chan *c = w.vt->close_notify(w.data);
    if (c != NULL)
        (void)chan_recv(c, NULL);
    return BURROW_NO_ERROR;
}

static bool h2t_close_conn(H2tTester *st) {
    (void)st->cc.vt->closer.close(st->cc.data);
    return true;
}

static bool h2t_write_rst_cancel(H2tTester *st) {
    return !h2t_failed(
        st, "WriteRSTStream",
        burrow__http2_framer_write_rst_stream(st->fr, 1, HTTP2_ERR_CODE_CANCEL));
}

/* Data longer than the declared Content-Length, which is a stream error. */
static bool h2t_write_past_content_length(H2tTester *st) {
    return h2t_write_data(st, 1, true, "1234", 4);
}

static void TestServer_CloseNotify_After_ConnClose(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_post_unblock(t, h2t_block_until_closed, h2t_close_conn, NULL, NULL, 0);
}

static void TestServer_CloseNotify_After_RSTStream(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_post_unblock(t, h2t_block_until_closed, h2t_write_rst_cancel, NULL, NULL,
                            0);
}

/* CloseNotify unblocks after a stream error from the client's side that it
 * didn't ask for, unlike TestServer_CloseNotify_After_RSTStream. */
static void TestServer_CloseNotify_After_StreamError(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    static const Str cl[] = {S_("content-length"), S_("3")};
    h2t_server_post_unblock(t, h2t_block_until_closed, h2t_write_past_content_length,
                            NULL, cl, 2);
}

static Error h2t_read_one_byte(HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    Byte b[1];
    Error err = BURROW_NO_ERROR;
    IoReader body = io_read_closer_as_io_reader(r->body);
    (void)body.vt->read(body.data, slice_from(b, 1, 1, TYPE_BYTE), &err);
    return err;
}

static void h2t_want_some_read_error(TestingT *t, Error err) {
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected nil error from Request.Body.Read");
}

static void TestServer_DeadConn_Unblocks_Read(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_post_unblock(t, h2t_read_one_byte, h2t_close_conn,
                            h2t_want_some_read_error, NULL, 0);
}

static void h2t_want_cancel_error(TestingT *t, Error err) {
    Http2StreamError se;
    if (!burrow__http2_error_stream(err, &se) || se.stream_id != 1 ||
        se.code != HTTP2_ERR_CODE_CANCEL || BURROW_FAILED(se.cause))
        testing_t_errorf_v(
            t, "Read error = %v; want %v", err,
            burrow__http2_stream_error(1, HTTP2_ERR_CODE_CANCEL, BURROW_NO_ERROR));
}

static void TestServer_RSTStream_Unblocks_Read(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_post_unblock(t, h2t_read_one_byte, h2t_write_rst_cancel,
                            h2t_want_cancel_error, NULL, 0);
}

enum { H2T_IN, H2T_WROTE_RST, H2T_HEADER_WRITTEN, H2T_UNBLOCK, H2T_NCHAN };

static void h2t_header_write_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    Chan **c = (Chan **)env;
    bool v = true;
    chan_send(c[H2T_IN], &v);
    (void)chan_recv(c[H2T_WROTE_RST], &v);
    (void)http_header_set(http_response_writer_header(w), BURROW_S("foo"),
                          BURROW_S("bar"));
    http_response_writer_write_header(w, 200);
    (void)w.vt->flush(w.data);
    chan_send(c[H2T_HEADER_WRITTEN], &v);
    (void)chan_recv(c[H2T_UNBLOCK], &v);
}

/* A handler that writes its header after the client has reset the stream
 * doesn't get stuck. */
static void TestServer_RSTStream_Unblocks_Header_Write(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    Chan *c[H2T_NCHAN];
    bool ok = true;
    for (int i = 0; i < H2T_NCHAN; i++) {
        c[i] = chan_make(heap_allocator(), TYPE_BOOL, 1);
        ok = ok && c[i] != NULL;
    }
    H2tTester st;
    if (!ok) {
        testing_t_errorf_v(t, "no memory");
    } else {
        if (h2t_start(&st, t, h2t_header_write_handler, c) && h2t_greet(&st) &&
            h2t_write_headers(&st, 1, h2t_encode_header(&st, h2t_post, 2), false,
                              true)) {
            bool v = true;
            (void)chan_recv(c[H2T_IN], &v);
            if (h2t_write_rst_cancel(&st)) {
                chan_send(c[H2T_WROTE_RST], &v);
                (void)chan_recv(c[H2T_HEADER_WRITTEN], &v);
            }
        }
        chan_close(c[H2T_WROTE_RST]);
        chan_close(c[H2T_UNBLOCK]);
        h2t_close(&st);
    }
    for (int i = 0; i < H2T_NCHAN; i++) {
        if (c[i] != NULL)
            chan_free(c[i]);
    }
}

/* Go's call.do with readBodyHandler. Each count sent on step has the handler
 * read that many bytes of the body, which the tests send as zeros, and then
 * say it's done. The handler returns once step is closed. */
typedef struct H2tReads {
    TestingT *t;
    Chan *step;
    Chan *done;
} H2tReads;

static void h2t_reads_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    H2tReads *rd = (H2tReads *)env;
    Int n = 0;
    while (chan_recv(rd->step, &n)) {
        Byte *buf = (Byte *)mem_alloc(heap_allocator(), (size_t)n, 1);
        if (buf == NULL) {
            testing_t_errorf_v(rd->t, "no memory");
        } else {
            Error err = BURROW_NO_ERROR;
            (void)io_read_full(io_read_closer_as_io_reader(r->body),
                               slice_from(buf, n, n, TYPE_BYTE), &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(rd->t, "%v", err);
            } else {
                for (Int i = 0; i < n; i++) {
                    if (buf[i] != 0) {
                        testing_t_errorf_v(rd->t, "read byte %d = %d, want 0", i,
                                           (int)buf[i]);
                        break;
                    }
                }
            }
            mem_free(heap_allocator(), buf, (size_t)n, 1);
        }
        bool v = true;
        chan_send(rd->done, &v);
    }
}

static bool h2t_reads_make(TestingT *t, H2tReads *rd) {
    rd->t = t;
    rd->step = chan_make(heap_allocator(), TYPE_INT, 0);
    rd->done = chan_make(heap_allocator(), TYPE_BOOL, 1);
    if (rd->step != NULL && rd->done != NULL)
        return true;
    testing_t_errorf_v(t, "no memory");
    return false;
}

static void h2t_reads_free(H2tReads *rd) {
    if (rd->step != NULL)
        chan_free(rd->step);
    if (rd->done != NULL)
        chan_free(rd->done);
}

static bool h2t_do_read(H2tReads *rd, Int n) {
    chan_send(rd->step, &n);
    bool v;
    return chan_recv(rd->done, &v);
}

static bool h2t_want_window_update(H2tTester *st, uint32_t id, uint32_t incr) {
    Http2Frame *f = h2t_read_kind(st, HTTP2_WINDOW_UPDATE_FRAME);
    if (f == NULL)
        return false;
    bool ok = f->header.stream_id == id && f->u.window_update.increment == incr;
    if (!ok)
        testing_t_errorf_v(st->t,
                           "got WINDOW_UPDATE stream %d increment %d, want %d %d",
                           (int)f->header.stream_id, (Int)f->u.window_update.increment,
                           (int)id, (Int)incr);
    burrow__http2_frame_free(f);
    return ok;
}

/* At least twice the initial window, or greet gets stuck waiting for a
 * WINDOW_UPDATE, and less than MAX_FRAME_SIZE. */
enum { H2T_WINDOW_SIZE = 65535 * 2 };

/* A POST with its data to come, to a server whose windows are
 * H2T_WINDOW_SIZE, and a handler that reads as rd says. data is
 * H2T_WINDOW_SIZE zeros. */
static bool h2t_window_update_start(H2tTester *st, TestingT *t, H2tReads *rd,
                                    HttpHTTP2Config *h2, Byte **data) {
    memset(h2, 0, sizeof *h2);
    h2->max_receive_buffer_per_connection = H2T_WINDOW_SIZE;
    h2->max_receive_buffer_per_stream = H2T_WINDOW_SIZE;
    *data = (Byte *)mem_alloc(heap_allocator(), H2T_WINDOW_SIZE, 1);
    if (*data == NULL) {
        testing_t_errorf_v(t, "no memory");
        memset(st, 0, sizeof *st);
        return false;
    }
    return h2t_start_config(st, t, h2t_reads_handler, rd, h2) && h2t_greet(st) &&
           h2t_write_headers(st, 1, h2t_encode_header(st, h2t_post, 2), false, true);
}

static void TestServer_Handler_Sends_WindowUpdate(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tReads rd;
    if (!h2t_reads_make(t, &rd)) {
        h2t_reads_free(&rd);
        return;
    }
    HttpHTTP2Config h2;
    Byte *data = NULL;
    H2tTester st;
    /* Less than half the window, which the handler reads. The server holds on
     * to the 1024 bytes of credit, to go with a later update. Then up to the
     * window's limit, and the server gives back what it held. Then the handler
     * reads that too, and the server gives back the rest. */
    if (h2t_window_update_start(&st, t, &rd, &h2, &data) &&
        h2t_write_data(&st, 1, false, (const char *)data, 1024) &&
        h2t_do_read(&rd, 1024) &&
        h2t_write_data(&st, 1, false, (const char *)data + 1024,
                       H2T_WINDOW_SIZE - 1024) &&
        h2t_want_window_update(&st, 0, 1024) && h2t_want_window_update(&st, 1, 1024) &&
        h2t_do_read(&rd, H2T_WINDOW_SIZE - 1024) &&
        h2t_want_window_update(&st, 0, H2T_WINDOW_SIZE - 1024))
        (void)h2t_want_window_update(&st, 1, H2T_WINDOW_SIZE - 1024);
    chan_close(rd.step);
    h2t_close(&st);
    h2t_reads_free(&rd);
    if (data != NULL)
        mem_free(heap_allocator(), data, H2T_WINDOW_SIZE, 1);
}

/* TestServer_Handler_Sends_WindowUpdate with padding. See
 * golang.org/issue/16556 */
static void TestServer_Handler_Sends_WindowUpdate_Padding(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    enum { N = H2T_WINDOW_SIZE / 2, PAD = 4 };
    H2tReads rd;
    if (!h2t_reads_make(t, &rd)) {
        h2t_reads_free(&rd);
        return;
    }
    HttpHTTP2Config h2;
    Byte *data = NULL;
    H2tTester st;
    /* Half a window of data, with some padding, which the handler reads. The
     * server gives back the body and the padding, 4 bytes of it and 1 of its
     * length. */
    if (h2t_window_update_start(&st, t, &rd, &h2, &data) &&
        !h2t_failed(&st, "WriteDataPadded",
                    burrow__http2_framer_write_data_padded(
                        st.fr, 1, false, slice_from(data, N, N, TYPE_BYTE),
                        slice_from(data, PAD, PAD, TYPE_BYTE))) &&
        h2t_do_read(&rd, N) && h2t_want_window_update(&st, 0, N + 1 + PAD))
        (void)h2t_want_window_update(&st, 1, N + 1 + PAD);
    chan_close(rd.step);
    h2t_close(&st);
    h2t_reads_free(&rd);
    if (data != NULL)
        mem_free(heap_allocator(), data, H2T_WINDOW_SIZE, 1);
}

static void h2t_handle_missing_trailer(HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    (void)http_header_set(http_response_writer_header(w), BURROW_S("Trailer"),
                          BURROW_S("test-trailer"));
}

static bool h2t_client_missing_trailer(H2tTester *st) {
    return h2t_get_slash(st) && h2t_want_headers(st, 1, false, NULL, 0) &&
           h2t_want_data(st, 1, true, "");
}

static void TestServer_Response_Stream_With_Missing_Trailer(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    h2t_server_response(t, h2t_handle_missing_trailer, h2t_client_missing_trailer);
}

static void h2t_check_no_rfc7540(void *env, Http2Setting s) {
    if (s.id == HTTP2_SETTING_NO_RFC7540_PRIORITIES)
        *(bool *)env = s.val == 1;
}

static void TestServerSettingNoRFC7540Priorities(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    bool got = false;
    H2tTester st;
    if (h2t_start(&st, t, NULL, NULL) &&
        h2t_greet_and_check_settings(&st, h2t_check_no_rfc7540, &got) && !got)
        testing_t_errorf_v(t, "want SETTINGS_NO_RFC7540_PRIORITIES to be %t, got %t",
                           true, got);
    h2t_close(&st);
}

/* The SETTINGS frame goes after the server's, for the reason
 * h2t_write_after_greeting gives. */
static void TestServerSettingNoRFC7540PrioritiesInvalid(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    Http2Setting s = {HTTP2_SETTING_NO_RFC7540_PRIORITIES, 2};
    H2tTester st;
    if (h2t_start(&st, t, NULL, NULL) && h2t_write_preface(&st) &&
        h2t_want_frame_type(&st, HTTP2_SETTINGS_FRAME) &&
        h2t_want_frame_type(&st, HTTP2_WINDOW_UPDATE_FRAME) &&
        !h2t_failed(&st, "WriteSettings",
                    burrow__http2_framer_write_settings(st.fr, &s, 1)))
        (void)h2t_want_go_away(&st, 0, HTTP2_ERR_CODE_PROTOCOL);
    h2t_close(&st);
}

/* A header block one byte short, so the decoder fails when the block ends. */
static void TestCompressionErrorOnClose(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    static const Str kv[] = {S_("foo"), S_("bar")};
    H2tTester st;
    if (h2t_start(&st, t, NULL, NULL) && h2t_greet(&st)) {
        Slice hbf = h2t_encode_header(&st, kv, 2);
        hbf.len--;
        if (h2t_write_headers(&st, 1, hbf, true, true))
            (void)h2t_want_go_away(&st, 1, HTTP2_ERR_CODE_COMPRESSION);
    }
    h2t_close(&st);
}

static void h2t_write_until_error(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    static const char p[] = "some data.\n";
    Int n = (Int)sizeof p - 1;
    Error err = BURROW_NO_ERROR;
    while (!BURROW_FAILED(err))
        (void)http_response_writer_write(
            w, slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE), &err);
    bool v = true;
    chan_send((Chan *)env, &v);
}

static void TestServer_HandlerWriteErrorOnDisconnect(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    Chan *errc = chan_make(heap_allocator(), TYPE_BOOL, 1);
    if (errc == NULL)
        FATALF("no memory");
    H2tTester st;
    if (h2t_start(&st, t, h2t_write_until_error, errc) && h2t_greet(&st) &&
        h2t_write_headers(&st, 1, h2t_encode_header(&st, NULL, 0), false, true) &&
        h2t_want_headers(&st, 1, false, NULL, 0)) {
        /* Close the connection and wait for the handler to notice. */
        bool v;
        (void)st.cc.vt->closer.close(st.cc.data);
        (void)chan_recv(errc, &v);
    }
    h2t_close(&st);
    chan_free(errc);
}

typedef struct H2tSeen {
    bool seen[16];
    uint32_t val[16];
} H2tSeen;

static void h2t_record_setting(void *env, Http2Setting s) {
    H2tSeen *v = (H2tSeen *)env;
    size_t i = (size_t)s.id;
    if (i < sizeof v->seen / sizeof v->seen[0]) {
        v->seen[i] = true;
        v->val[i] = s.val;
    }
}

static void TestServer_MaxDecoderHeaderTableSize(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    const uint32_t want = HTTP2_INITIAL_HEADER_TABLE_SIZE * 2;
    HttpHTTP2Config h2;
    memset(&h2, 0, sizeof h2);
    h2.max_decoder_header_table_size = (Int)want;
    H2tSeen seen;
    memset(&seen, 0, sizeof seen);
    H2tTester st;
    if (h2t_start_config(&st, t, NULL, NULL, &h2) &&
        h2t_greet_and_check_settings(&st, h2t_record_setting, &seen)) {
        if (!seen.seen[HTTP2_SETTING_HEADER_TABLE_SIZE])
            testing_t_errorf_v(t, "server didn't advertise a header table size");
        else if (seen.val[HTTP2_SETTING_HEADER_TABLE_SIZE] != want)
            testing_t_errorf_v(
                t, "server advertised a header table size of %d, want %d",
                (Int)seen.val[HTTP2_SETTING_HEADER_TABLE_SIZE], (Int)want);
    }
    h2t_close(&st);
}

/* Closes b->in once it starts, then waits for the request's context to be
 * done and closes b->block. */
static void h2t_cancel_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    H2tBlock *b = (H2tBlock *)env;
    chan_close(b->in);
    (void)chan_recv(context_done(http_request_context(r)), NULL);
    chan_close(b->block);
}

/* Issue 67036: A stream error should result in the handler's request context
 * being canceled. A second set of request headers on the stream is the error,
 * and resets it. */
static void TestServerRequestCancelOnError(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tBlock b;
    if (!h2t_block_make(t, &b)) {
        h2t_block_free(&b);
        return;
    }
    H2tTester st;
    if (h2t_start(&st, t, h2t_cancel_handler, &b) && h2t_greet(&st) &&
        h2t_bodyless_req1(&st, NULL, 0)) {
        (void)chan_recv(b.in, NULL);
        if (h2t_bodyless_req1(&st, NULL, 0))
            (void)chan_recv(b.block, NULL);
    }
    h2t_close(&st);
    h2t_block_free(&b);
}

/* Closes the body, sends the header and waits on b->block. */
static void h2t_close_body_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    H2tBlock *b = (H2tBlock *)env;
    (void)r->body.vt->closer.close(r->body.data);
    http_response_writer_write_header(w, 200);
    (void)w.vt->flush(w.data);
    bool v;
    (void)chan_recv(b->block, &v);
}

static void TestServerReturnsStreamAndConnFlowControlOnBodyClose(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    /* Enough to make the server give the flow control back. */
    static const char zeros[HTTP2_INFLOW_MIN_REFRESH];
    H2tBlock b;
    if (!h2t_block_make(t, &b)) {
        h2t_block_free(&b);
        return;
    }
    H2tTester st;
    if (h2t_start(&st, t, h2t_close_body_handler, &b) && h2t_greet(&st) &&
        h2t_write_headers(&st, 1, h2t_encode_header(&st, NULL, 0), false, true) &&
        h2t_want_headers(&st, 1, false, NULL, 0) &&
        h2t_write_data(&st, 1, false, zeros, (Int)sizeof zeros) &&
        h2t_want_window_update(&st, 0, (uint32_t)sizeof zeros)) {
        bool v = true;
        chan_send(b.block, &v);
        (void)h2t_want_data(&st, 1, true, "");
    }
    chan_close(b.block);
    h2t_close(&st);
    h2t_block_free(&b);
}

/* Data on a stream whose handler has ended but whose client side hasn't, which
 * once made Go's server panic. Go then checks the connection's flow control
 * and that nothing panicked as the connection closes, both from inside the
 * server. Here a crash would end the test. */
static void TestServer_NoCrash_HandlerClose_Then_ClientClose(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tTester st;
    /* The handler ending while the client is still sending DATA resets the
     * stream with NO_ERROR, and the DATA that comes anyway resets it again
     * with STREAM_CLOSED. */
    if (h2t_start(&st, t, NULL, NULL) && h2t_greet(&st) &&
        h2t_write_headers(&st, 1, h2t_encode_header(&st, NULL, 0), false, true) &&
        h2t_want_headers(&st, 1, true, NULL, 0) &&
        h2t_want_rst_stream(&st, 1, HTTP2_ERR_CODE_NO) &&
        h2t_write_data(&st, 1, true, "foo", 3))
        (void)h2t_want_rst_stream(&st, 1, HTTP2_ERR_CODE_STREAM_CLOSED);
    h2t_close(&st);
}

/* Reads until the connection closes, which it has to, and fails on a HEADERS
 * frame. Says whether a GOAWAY came, with the last of their stream IDs in
 * *last_id. */
static bool h2t_read_until_closed(H2tTester *st, uint32_t *last_id) {
    bool saw_go_away = false;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Http2Frame *f = burrow__http2_framer_read_frame(st->fr, &err);
        if (BURROW_FAILED(err)) {
            if (errors_is(err, os_err_deadline_exceeded))
                testing_t_errorf_v(st->t, "connection is not closed; want it to be");
            burrow__http2_frame_free(f);
            return saw_go_away;
        }
        bool headers = f->kind == HTTP2_META_HEADERS_FRAME;
        if (f->kind == HTTP2_GO_AWAY_FRAME) {
            saw_go_away = true;
            *last_id = f->u.go_away.last_stream_id;
        }
        burrow__http2_frame_free(f);
        if (headers) {
            testing_t_errorf_v(st->t, "received HEADERS frame; want GOAWAY");
            return saw_go_away;
        }
    }
}

static void TestServerContinuationFlood(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tTester st;
    if (h2t_start_server(&st, t, NULL, NULL, NULL, 4096) && h2t_greet(&st) &&
        h2t_write_headers(&st, 1, h2t_encode_header(&st, NULL, 0), true, false)) {
        bool ok = true;
        for (int i = 0; ok && i < 1000; i++) {
            char name[16];
            (void)snprintf(name, sizeof name, "x-%d", i);
            Str kv[] = {str_from_cstr(name), S_("1234567890")};
            ok = !h2t_failed(&st, "WriteContinuation",
                             burrow__http2_framer_write_continuation(
                                 st.fr, 1, false, h2t_encode_header_raw(&st, kv, 2)));
        }
        static const Str last[] = {S_("x-last-header"), S_("1")};
        if (ok)
            (void)burrow__http2_framer_write_continuation(
                st.fr, 1, true, h2t_encode_header_raw(&st, last, 2));
        /* The server closes the connection a second after its GOAWAY, which
         * may not be out by then, so only its stream ID is checked. */
        uint32_t last_id = 0;
        if (h2t_read_until_closed(&st, &last_id) && last_id != 1)
            testing_t_errorf_v(t, "received GOAWAY with LastStreamId %d, want %d",
                               (Int)last_id, 1);
    }
    h2t_close(&st);
}

static void TestServerContinuationAfterInvalidHeader(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    static const Str invalid[] = {S_("x-invalid-header"), BURROW_S_INIT("\x00")};
    static const Str valid[] = {S_("x-valid-header"), S_("1")};
    H2tTester st;
    if (h2t_start(&st, t, NULL, NULL) && h2t_greet(&st) &&
        h2t_write_headers(&st, 1, h2t_encode_header(&st, NULL, 0), true, false) &&
        !h2t_failed(&st, "WriteContinuation",
                    burrow__http2_framer_write_continuation(
                        st.fr, 1, false, h2t_encode_header_raw(&st, invalid, 2))) &&
        !h2t_failed(&st, "WriteContinuation",
                    burrow__http2_framer_write_continuation(
                        st.fr, 1, true, h2t_encode_header_raw(&st, valid, 2)))) {
        uint32_t last_id = 0;
        if (!h2t_read_until_closed(&st, &last_id))
            testing_t_errorf_v(t, "connection closed with no GOAWAY frame; want one");
    }
    h2t_close(&st);
}

static char h2t_cookie[4058];

/* A cookie of about 4K, then a megabyte of CONTINUATION frames that repeat it
 * by its index in the header table. */
static void TestServerDoS_MaxHeaderListSize(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    enum { SIZE = 1 << 20 };
    H2tSeen seen;
    memset(&seen, 0, sizeof seen);
    H2tTester st;
    Byte *b = NULL;
    Int blen = 0;
    if (!h2t_start(&st, t, NULL, NULL) ||
        !h2t_greet_and_check_settings(&st, h2t_record_setting, &seen)) {
        h2t_close(&st);
        return;
    }
    Int frame_size = 1 << 20; /* DefaultMaxReadFrameSize */
    if (seen.seen[HTTP2_SETTING_MAX_FRAME_SIZE]) {
        uint32_t v = seen.val[HTTP2_SETTING_MAX_FRAME_SIZE];
        if (v < HTTP2_MIN_MAX_FRAME_SIZE)
            frame_size = HTTP2_MIN_MAX_FRAME_SIZE;
        else if (v > HTTP2_MAX_FRAME_SIZE)
            frame_size = HTTP2_MAX_FRAME_SIZE;
        else
            frame_size = (Int)v;
    }
    if (!seen.seen[HTTP2_SETTING_MAX_HEADER_LIST_SIZE])
        testing_t_errorf_v(t, "server didn't advertise a max header list size");
    else if (seen.val[HTTP2_SETTING_MAX_HEADER_LIST_SIZE] == 0)
        testing_t_errorf_v(t, "server advertised a max header list size of 0");

    memset(h2t_cookie, '*', sizeof h2t_cookie);
    Str cookie = str_from_bytes((const Byte *)h2t_cookie, (Int)sizeof h2t_cookie);
    bytes_buffer_reset(&st.hbuf);
    h2t_field(&st, BURROW_S(":method"), BURROW_S("GET"));
    h2t_field(&st, BURROW_S(":path"), BURROW_S("/"));
    h2t_field(&st, BURROW_S(":scheme"), BURROW_S("https"));
    h2t_field(&st, BURROW_S("cookie"), cookie);
    bool ok = h2t_write_headers(&st, 1, bytes_buffer_bytes(&st.hbuf), true, false);

    /* The short encoding of the cookie, now that it has been sent once. */
    bytes_buffer_reset(&st.hbuf);
    h2t_field(&st, BURROW_S("cookie"), cookie);
    Slice one = bytes_buffer_bytes(&st.hbuf);
    if (ok && one.len > 0) {
        blen = SIZE / one.len * one.len;
        b = (Byte *)mem_alloc_nozero(st.a, (size_t)blen, 1);
        if (b == NULL) {
            testing_t_errorf_v(t, "no memory");
            ok = false;
        }
    }
    for (Int i = 0; ok && i < blen; i += one.len)
        memcpy(b + i, one.p, (size_t)one.len);
    for (Int off = 0; ok && off < blen;) {
        Int n = blen - off < frame_size ? blen - off : frame_size;
        off += n;
        ok = !h2t_failed(
            &st, "WriteContinuation",
            burrow__http2_framer_write_continuation(
                st.fr, 1, off == blen, slice_from(b + off - n, n, n, TYPE_BYTE)));
    }
    if (ok) {
        static const Str want[] = {S_(":status"),        S_("431"),
                                   S_("content-type"),   S_("text/html; charset=utf-8"),
                                   S_("content-length"), S_("63")};
        (void)h2t_want_headers(&st, 1, false, want, 6);
    }
    h2t_close(&st);
    if (b != NULL)
        mem_free(st.a, b, (size_t)blen, 1);
}

typedef struct H2tWindowCase {
    Str name;
    Int want;
} H2tWindowCase;

static void h2t_initial_window(void *env, TestingT *t) {
    const H2tWindowCase *c = (const H2tWindowCase *)env;
    HttpHTTP2Config h2;
    memset(&h2, 0, sizeof h2);
    h2.max_receive_buffer_per_connection = c->want;
    H2tTester st;
    if (h2t_start_config(&st, t, NULL, NULL, &h2) && h2t_write_preface(&st) &&
        !h2t_failed(&st, "WriteSettings",
                    burrow__http2_framer_write_settings(st.fr, NULL, 0)) &&
        h2t_want_frame_type(&st, HTTP2_SETTINGS_FRAME) &&
        !h2t_failed(&st, "WriteSettingsAck",
                    burrow__http2_framer_write_settings_ack(st.fr)) &&
        h2t_bodyless_req1(&st, NULL, 0)) {
        Int window = 65535;
        bool ok = true;
        for (;;) {
            Http2Frame *f = h2t_read_frame(&st);
            if (f == NULL)
                break;
            bool done = f->kind == HTTP2_META_HEADERS_FRAME;
            if (f->kind == HTTP2_WINDOW_UPDATE_FRAME) {
                if (f->header.stream_id != 0) {
                    testing_t_errorf_v(t, "WindowUpdate StreamID = %d; want 0",
                                       (int)f->header.stream_id);
                    ok = false;
                    burrow__http2_frame_free(f);
                    break;
                }
                window += (Int)f->u.window_update.increment;
            }
            burrow__http2_frame_free(f);
            if (done)
                break;
        }
        if (ok && window != c->want)
            testing_t_errorf_v(t, "got initial flow control window = %d, want %d",
                               window, c->want);
    }
    h2t_close(&st);
}

/* For a connection buffer between 65535 and twice that, the server sends no
 * initial WINDOW_UPDATE, since it only sends one when the window drops below
 * half of the most it can be. That case isn't tested, as in Go. */
static void TestServerInitialFlowControlWindow(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    static const H2tWindowCase cases[] = {
        {S_("65535"), 65535},
        {S_("524288"), 1 << 19},
        {S_("2097152"), 1 << 21},
        {S_("131070"), (Int)65535 * 2},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        testing_t_run(
            t, cases[i].name,
            BURROW_FN(TestingTFunc, h2t_initial_window, (void *)(uintptr_t)&cases[i]));
}

typedef struct H2tBodyClose {
    TestingT *t;
    Chan *errc;
} H2tBodyClose;

/* Reads 4 bytes of the body and closes it. */
static void h2t_read4_close_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    H2tBodyClose *bc = (H2tBodyClose *)env;
    Byte buf[4];
    Error err = BURROW_NO_ERROR;
    Int n = io_read_full(io_read_closer_as_io_reader(r->body),
                         slice_from(buf, 4, 4, TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(bc->t, "%v", err);
    else if (n != 4)
        testing_t_errorf_v(bc->t, "too few bytes read: %d", n);
    else
        (void)r->body.vt->closer.close(r->body.data);
    bool v = true;
    chan_send(bc->errc, &v);
}

/* Data written after the handler closes the body gives its flow control
 * credit straight back. */
static void TestServerWindowUpdateOnBodyClose(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    enum { HALF = H2T_WINDOW_SIZE / 2 };
    H2tBodyClose bc = {t, chan_make(heap_allocator(), TYPE_BOOL, 1)};
    Byte *content = (Byte *)mem_alloc(heap_allocator(), H2T_WINDOW_SIZE, 1);
    if (bc.errc == NULL || content == NULL) {
        testing_t_errorf_v(t, "no memory");
    } else {
        HttpHTTP2Config h2;
        memset(&h2, 0, sizeof h2);
        h2.max_receive_buffer_per_connection = H2T_WINDOW_SIZE;
        h2.max_receive_buffer_per_stream = H2T_WINDOW_SIZE;
        static const Str kv[] = {S_(":method"), S_("POST"), S_("content-length"),
                                 S_("131070")};
        H2tTester st;
        if (h2t_start_config(&st, t, h2t_read4_close_handler, &bc, &h2) &&
            h2t_greet(&st) &&
            h2t_write_headers(&st, 1, h2t_encode_header(&st, kv, 4), false, true) &&
            h2t_write_data(&st, 1, false, (const char *)content, HALF)) {
            bool v;
            (void)chan_recv(bc.errc, &v);
            /* The credit for what has been written so far. */
            Int increments = HALF;
            while (increments != 0) {
                Http2Frame *f = h2t_read_frame(&st);
                if (f == NULL)
                    break;
                if (f->kind == HTTP2_WINDOW_UPDATE_FRAME && f->header.stream_id == 0)
                    increments -= (Int)f->u.window_update.increment;
                burrow__http2_frame_free(f);
            }
            if (increments == 0 &&
                h2t_write_data(&st, 1, false, (const char *)content + HALF, HALF))
                (void)h2t_want_window_update(&st, 0, HALF);
        }
        h2t_close(&st);
    }
    if (bc.errc != NULL)
        chan_free(bc.errc);
    if (content != NULL)
        mem_free(heap_allocator(), content, H2T_WINDOW_SIZE, 1);
}

static void h2t_close_body_job(void *env) {
    HttpRequest *r = (HttpRequest *)env;
    (void)r->body.vt->closer.close(r->body.data);
}

/* Closes the body on another goroutine while it reads it. Go doesn't wait for
 * the close, but here the request is gone once the handler returns. */
static void h2t_read_close_race_handler(void *env, HttpResponseWriter w,
                                        HttpRequest *r) {
    (void)env;
    (void)w;
    SyncWaitGroup wg;
    memset(&wg, 0, sizeof wg);
    bool started = sync_wait_group_go(&wg, BURROW_FN(Func, h2t_close_body_job, r));
    Error err = BURROW_NO_ERROR;
    (void)io_copy(heap_allocator(), io_discard, io_read_closer_as_io_reader(r->body),
                  &err);
    if (started)
        sync_wait_group_wait(&wg);
    else
        h2t_close_body_job(r);
}

static void TestRequestBodyReadCloseRace(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    static const char data[1024];
    H2tTester st;
    if (h2t_start(&st, t, h2t_read_close_race_handler, NULL) && h2t_greet(&st)) {
        for (uint32_t i = 0; i < 100; i++) {
            /* The DATA goes on stream 1 every time, as in Go. */
            uint32_t id = 1 + i * 2;
            if (!h2t_write_headers(&st, id, h2t_encode_header(&st, NULL, 0), false,
                                   true) ||
                !h2t_write_data(&st, 1, false, data, (Int)sizeof data))
                break;
            /* A RST_STREAM, skipping anything else, with NO or STREAM_CLOSED
             * depending on scheduling. */
            Http2Frame *f = NULL;
            do {
                burrow__http2_frame_free(f);
                f = h2t_read_frame(&st);
            } while (f != NULL && f->kind != HTTP2_RST_STREAM_FRAME);
            if (f == NULL) {
                testing_t_errorf_v(t, "got no RSTStreamFrame, want one");
                break;
            }
            Http2ErrCode code = f->u.rst_stream.err_code;
            burrow__http2_frame_free(f);
            if (code != HTTP2_ERR_CODE_NO && code != HTTP2_ERR_CODE_STREAM_CLOSED) {
                testing_t_errorf_v(t,
                                   "got RSTStreamFrame with error code %d, want "
                                   "ErrCodeNo or ErrCodeStreamClosed",
                                   (int)code);
                break;
            }
        }
    }
    h2t_close(&st);
}

enum { H2T_MAX_HANDLERS = 10 };

/* handlerc carries the stop channel of each handler that starts, as a
 * uintptr_t, and the test owns the channels it is given. */
typedef struct H2tMaxHandlers {
    Chan *handlerc;
    Chan *donec;
} H2tMaxHandlers;

static void h2t_max_handlers_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    (void)r;
    H2tMaxHandlers *m = (H2tMaxHandlers *)env;
    Chan *stopc = chan_make(heap_allocator(), TYPE_BOOL, 1);
    if (stopc == NULL)
        return;
    uintptr_t p = (uintptr_t)stopc;
    SelectCase hand[] = {BURROW_SEND(m->handlerc, &p), BURROW_RECV(m->donec, NULL)};
    if (chan_select(hand, 2) != 0) {
        chan_free(stopc);
        return;
    }
    bool should_panic = false;
    SelectCase wait[] = {BURROW_RECV(stopc, &should_panic),
                         BURROW_RECV(m->donec, NULL)};
    if (chan_select(wait, 2) == 0 && should_panic)
        panic(BURROW_ANY(TYPE_ERROR, (void *)(uintptr_t)&http_err_abort_handler));
}

static bool h2t_get_rst(H2tTester *st, uint32_t id, bool rst) {
    return h2t_write_headers(st, id, h2t_encode_header(st, NULL, 0), true, true) &&
           (!rst || !h2t_failed(st, "WriteRSTStream",
                                burrow__http2_framer_write_rst_stream(
                                    st->fr, id, HTTP2_ERR_CODE_CANCEL)));
}

static Chan *h2t_next_handler(H2tMaxHandlers *m) {
    uintptr_t p = 0;
    (void)chan_recv(m->handlerc, &p);
    return (Chan *)p;
}

/* The client's side of TestServerMaxHandlerGoroutines, which puts the stop
 * channels it is given in stops. */
static void h2t_max_handlers_client(H2tTester *st, H2tMaxHandlers *m, Chan **stops,
                                    size_t *nstops) {
    TestingT *t = st->t;
    /* maxHandlers concurrent requests, each reset once its handler has
     * started. */
    uint32_t id = 1;
    for (int i = 0; i < H2T_MAX_HANDLERS; i++, id += 2) {
        if (!h2t_get_rst(st, id, false))
            return;
        stops[(*nstops)++] = h2t_next_handler(m);
        if (h2t_failed(st, "WriteRSTStream",
                       burrow__http2_framer_write_rst_stream(st->fr, id,
                                                             HTTP2_ERR_CODE_CANCEL)))
            return;
    }
    /* Another, reset straight away, and two more that aren't. */
    if (!h2t_get_rst(st, id, true) || !h2t_get_rst(st, id + 2, false) ||
        !h2t_get_rst(st, id + 4, false))
        return;
    id += 6;

    /* The first maxHandlers handlers are still running, so the last two
     * requests start none. Go waits a millisecond of its fake clock, and this
     * waits 50ms. */
    time_sleep(50 * TIME_MILLISECOND);
    uintptr_t p = 0;
    bool ok = false;
    if (chan_try_recv(m->handlerc, &p, &ok)) {
        testing_t_errorf_v(t, "handler unexpectedly started while maxHandlers are "
                              "already running");
        stops[(*nstops)++] = (Chan *)p;
    }

    /* Two handlers exit, one of them by panicking, and the waiting requests
     * that weren't reset start theirs. */
    bool v = false;
    chan_send(stops[0], &v);
    v = true;
    chan_send(stops[1], &v);
    stops[(*nstops)++] = h2t_next_handler(m);
    stops[(*nstops)++] = h2t_next_handler(m);

    /* A lot more requests, until the server tells the client to go away. */
    for (int i = 0; i < 5 * H2T_MAX_HANDLERS; i++, id += 2) {
        if (!h2t_get_rst(st, id, true))
            return;
    }
    Http2Frame *f = h2t_read_kind(st, HTTP2_GO_AWAY_FRAME);
    if (f != NULL && f->u.go_away.err_code != HTTP2_ERR_CODE_ENHANCE_YOUR_CALM)
        testing_t_errorf_v(t, "err code = %d; want %d", (int)f->u.go_away.err_code,
                           (int)HTTP2_ERR_CODE_ENHANCE_YOUR_CALM);
    burrow__http2_frame_free(f);
}

static void TestServerMaxHandlerGoroutines(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tMaxHandlers m = {chan_make(heap_allocator(), TYPE_UINTPTR, 0),
                        chan_make(heap_allocator(), TYPE_BOOL, 0)};
    Chan *stops[H2T_MAX_HANDLERS + 4];
    size_t nstops = 0;
    if (m.handlerc == NULL || m.donec == NULL) {
        testing_t_errorf_v(t, "no memory");
    } else {
        HttpHTTP2Config h2;
        memset(&h2, 0, sizeof h2);
        h2.max_concurrent_streams = H2T_MAX_HANDLERS;
        H2tTester st;
        if (h2t_start_config(&st, t, h2t_max_handlers_handler, &m, &h2) &&
            h2t_greet(&st))
            h2t_max_handlers_client(&st, &m, stops, &nstops);
        for (size_t i = 0; i < nstops; i++)
            chan_close(stops[i]);
        chan_close(m.donec);
        h2t_close(&st);
    }
    for (size_t i = 0; i < nstops; i++)
        chan_free(stops[i]);
    if (m.handlerc != NULL)
        chan_free(m.handlerc);
    if (m.donec != NULL)
        chan_free(m.donec);
}

/* Go's handlers write 16MB. Anything past a stream's window does the same, as
 * the client here never gives a stream more than its first 65535 bytes. */
enum { H2T_BIG = 1 << 17, H2T_STREAM_WINDOW = 65535 };
static Byte h2t_big[H2T_BIG];

static void h2t_big_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    Error err = BURROW_NO_ERROR;
    (void)http_response_writer_write(
        w, slice_from(h2t_big, H2T_BIG, H2T_BIG, TYPE_BYTE), &err);
    if (!BURROW_FAILED(err))
        (void)w.vt->flush(w.data);
}

/* A server whose handlers write more than a stream's window, and a client that
 * has opened the connection's window wide. */
static bool h2t_big_start(H2tTester *st, TestingT *t) {
    memset(h2t_big, 'a', sizeof h2t_big);
    return h2t_start(st, t, h2t_big_handler, NULL) && h2t_greet(st) &&
           h2t_write_window_update(st, 0, 1U << 30);
}

/* A GET on stream id, with a priority header of u=urgency when urgency isn't
 * negative, and a Via header when via is true. */
static bool h2t_write_prio_get(H2tTester *st, uint32_t id, int urgency, bool via) {
    char prio[16];
    Str kv[4];
    size_t n = 0;
    if (urgency >= 0) {
        (void)snprintf(prio, sizeof prio, "u=%d", urgency);
        kv[n++] = BURROW_S("priority");
        kv[n++] = str_from_cstr(prio);
    }
    if (via) {
        kv[n++] = BURROW_S("via");
        kv[n++] = BURROW_S("a proxy");
    }
    return h2t_write_headers(st, id, h2t_encode_header(st, kv, n), true, true);
}

/* The frames for the n streams from first on, until each has had all of its
 * window. Go reads until the bubble has nothing more to give, and waits for it
 * to go quiet before each frame, so each handler has its data queued by the
 * time its stream comes up. This waits a little before each frame for the
 * same. Puts the stream of each frame in order, up to cap of them, and
 * answers how many, or -1 when the test failed. */
static Int h2t_read_windows(H2tTester *st, uint32_t first, int n, uint32_t *order,
                            Int cap) {
    Int got[32];
    Int nframes = 0;
    int full = 0;
    memset(got, 0, sizeof got);
    while (full < n) {
        time_sleep(2 * TIME_MILLISECOND);
        Http2Frame *f = h2t_read_frame(st);
        if (f == NULL)
            return -1;
        uint32_t id = f->header.stream_id;
        if (nframes < cap)
            order[nframes++] = id;
        if (f->kind == HTTP2_DATA_FRAME && id >= first && (id - first) % 2 == 0 &&
            (id - first) / 2 < (uint32_t)n) {
            Int *g = &got[(id - first) / 2];
            bool was_full = *g >= H2T_STREAM_WINDOW;
            *g += f->u.data.data.len;
            if (!was_full && *g >= H2T_STREAM_WINDOW)
                full++;
        }
        burrow__http2_frame_free(f);
    }
    return nframes;
}

/* Whether no two frames next to each other in the middle half of order are on
 * the same stream, which is what round robin gives. */
static bool h2t_middle_half_alternates(const uint32_t *order, Int n) {
    for (Int i = n / 4 + 1; i < n * 3 / 4; i++) {
        if (order[i] == order[i - 1])
            return false;
    }
    return true;
}

static void h2t_print_order(TestingT *t, const char *what, const uint32_t *order,
                            Int n) {
    BytesBuffer b = BYTES_BUFFER(heap_allocator());
    for (Int i = 0; i < n; i++) {
        char num[16];
        (void)snprintf(num, sizeof num, i == 0 ? "%u" : " %u", (unsigned)order[i]);
        (void)bytes_buffer_write_string(&b, str_from_cstr(num), NULL);
    }
    testing_t_errorf_v(t, "%s, got: [%s]", str_from_cstr(what),
                       str_from_bytes(bytes_buffer_bytes(&b).p, bytes_buffer_len(&b)));
    bytes_buffer_free(&b);
}

/* How long the handlers get to queue their first writes, where Go waits for
 * its bubble to go quiet. */
#define H2T_SETTLE (100 * TIME_MILLISECOND)

static void TestServerRFC9218Priority(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tTester st;
    if (h2t_big_start(&st, t)) {
        /* 8 streams, where a stream with a larger ID has a lower urgency
         * value, which is more urgent. */
        bool ok = true;
        for (int i = 0; ok && i < 8; i++)
            ok = h2t_write_prio_get(&st, (uint32_t)(i * 2 + 1), 7 - i, false);
        time_sleep(H2T_SETTLE);
        uint32_t order[256];
        Int n = ok ? h2t_read_windows(&st, 1, 8, order, 256) : -1;
        /* The last frame of each stream, which is when it was done. */
        Int last[8];
        for (int i = 0; i < 8; i++)
            last[i] = -1;
        for (Int i = 0; i < n; i++) {
            if (order[i] % 2 == 1 && order[i] < 16)
                last[order[i] / 2] = i;
        }
        for (int i = 0; n >= 0 && i < 7; i++) {
            if (last[i] < last[i + 1])
                testing_t_errorf_v(t,
                                   "stream %d finished before stream %d unexpectedly",
                                   i * 2 + 1, i * 2 + 3);
        }
    }
    h2t_close(&st);
}

static void TestServerRFC9218PriorityIgnoredWhenProxied(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    H2tTester st;
    if (h2t_big_start(&st, t)) {
        /* The same 8 streams, which come through a proxy, so their urgencies
         * don't count. */
        bool ok = true;
        for (int i = 0; ok && i < 8; i++)
            ok = h2t_write_prio_get(&st, (uint32_t)(i * 2 + 1), 7 - i, true);
        time_sleep(H2T_SETTLE);
        uint32_t order[256];
        Int n = ok ? h2t_read_windows(&st, 1, 8, order, 256) : -1;
        /* Only the middle half, since stream 1 gets a few frames out before
         * the others open, and the end isn't quite round robin either. */
        if (n >= 0 && !h2t_middle_half_alternates(order, n))
            h2t_print_order(
                t, "want stream to be processed in round-robin manner when proxied",
                order, n);
    }
    h2t_close(&st);
}

static void TestServerRFC9218PriorityAware(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    enum { STREAMS = 10 };
    H2tTester st;
    if (h2t_big_start(&st, t)) {
        /* With no sign that the client knows RFC 9218 priorities, the streams
         * take turns. */
        bool ok = true;
        for (int i = 0; ok && i < STREAMS; i++)
            ok = h2t_write_prio_get(&st, (uint32_t)(i * 2 + 1), -1, false);
        time_sleep(H2T_SETTLE);
        uint32_t order[256];
        Int n = ok ? h2t_read_windows(&st, 1, STREAMS, order, 256) : -1;
        if (n >= 0 && !h2t_middle_half_alternates(order, n))
            h2t_print_order(t,
                            "want stream to be processed in round-robin manner when "
                            "unaware of priority",
                            order, n);

        /* A PRIORITY_UPDATE for stream 1, which is done, so all it does is
         * tell the server the client knows about RFC 9218. Then streams of the
         * same urgency that aren't incremental go one at a time to the end.
         * Go checks the first streams' order again here rather than these
         * ones', which they wouldn't pass, and so does this. */
        ok = n >= 0 && !h2t_failed(&st, "WritePriorityUpdate",
                                   burrow__http2_framer_write_priority_update(
                                       st.fr, 1, BURROW_STR_EMPTY));
        time_sleep(H2T_SETTLE);
        for (int i = STREAMS; ok && i < 2 * STREAMS; i++)
            ok = h2t_write_prio_get(&st, (uint32_t)(i * 2 + 1), -1, false);
        uint32_t order2[256];
        if (ok && h2t_read_windows(&st, 2 * STREAMS + 1, STREAMS, order2, 256) >= 0 &&
            !h2t_middle_half_alternates(order, n))
            h2t_print_order(t,
                            "want stream to be processed one-by-one to completion when "
                            "aware of priority",
                            order, n);
    }
    h2t_close(&st);
}

#define TESTS(X)                                                                       \
    X(TestServer)                                                                      \
    X(TestServer_Request_Get)                                                          \
    X(TestServer_Request_Get_PathSlashes)                                              \
    X(TestServer_Request_Post_NoContentLength_EndStream)                               \
    X(TestServer_Request_Post_Body_ImmediateEOF)                                       \
    X(TestServer_Request_Post_Body_OneData)                                            \
    X(TestServer_Request_Post_Body_TwoData)                                            \
    X(TestServer_Request_Post_Body_ContentLength_Correct)                              \
    X(TestServer_Request_Post_Body_ContentLength_TooLarge)                             \
    X(TestServer_Request_Post_Body_ContentLength_TooSmall)                             \
    X(TestServer_Request_Get_Host)                                                     \
    X(TestServer_Request_Get_Authority)                                                \
    X(TestServer_Request_CookieConcat)                                                 \
    X(TestServer_Request_Reject_CapitalHeader)                                         \
    X(TestServer_Request_Reject_HeaderFieldNameColon)                                  \
    X(TestServer_Request_Reject_HeaderFieldNameNULL)                                   \
    X(TestServer_Request_Reject_HeaderFieldNameEmpty)                                  \
    X(TestServer_Request_Reject_HeaderFieldValueNewline)                               \
    X(TestServer_Request_Reject_HeaderFieldValueCR)                                    \
    X(TestServer_Request_Reject_HeaderFieldValueDEL)                                   \
    X(TestServer_Request_Reject_Pseudo_Missing_method)                                 \
    X(TestServer_Request_Reject_Pseudo_ExactlyOne)                                     \
    X(TestServer_Request_Reject_Pseudo_AfterRegular)                                   \
    X(TestServer_Request_Reject_Pseudo_Missing_path)                                   \
    X(TestServer_Request_Reject_Pseudo_Missing_scheme)                                 \
    X(TestServer_Request_Reject_Pseudo_scheme_invalid)                                 \
    X(TestServer_Request_Reject_Pseudo_Unknown)                                        \
    X(TestServer_Request_Reject_Authority_Userinfo)                                    \
    X(TestRejectFrameOnIdle_WindowUpdate)                                              \
    X(TestRejectFrameOnIdle_Data)                                                      \
    X(TestRejectFrameOnIdle_RSTStream)                                                 \
    X(TestServer_Ping)                                                                 \
    X(TestServer_Rejects_Headers0)                                                     \
    X(TestServer_Rejects_Continuation0)                                                \
    X(TestServer_Rejects_Priority0)                                                    \
    X(TestServer_Rejects_PushPromise)                                                  \
    X(TestServer_Response_NoData)                                                      \
    X(TestServer_Response_NoData_Header_FooBar)                                        \
    X(TestServer_Response_TransferEncoding_chunked)                                    \
    X(TestServer_Rejects_ConnHeaders)                                                  \
    X(TestServer_Rejects_TooSmall)                                                     \
    X(TestServerReadsTrailers)                                                         \
    X(TestServerWritesTrailers_WithFlush)                                              \
    X(TestServerWritesTrailers_WithoutFlush)                                           \
    X(TestServerGracefulShutdown)                                                      \
    X(TestServer_Request_Post_Body_ContentLength_EndStream)                            \
    X(TestServer_Request_WithContinuation)                                             \
    X(TestServer_Request_Connect)                                                      \
    X(TestServer_Request_Connect_InvalidPath)                                          \
    X(TestServer_Request_Connect_InvalidScheme)                                        \
    X(TestServer_RejectsLargeFrames)                                                   \
    X(TestServer_Rejects_HeadersNoEnd_Then_Headers)                                    \
    X(TestServer_Rejects_HeadersNoEnd_Then_Ping)                                       \
    X(TestServer_Rejects_HeadersEnd_Then_Continuation)                                 \
    X(TestServer_Rejects_HeadersNoEnd_Then_ContinuationWrongStream)                    \
    X(TestServer_Rejects_PriorityUpdate0)                                              \
    X(TestServer_Rejects_PriorityUpdateUnparsable)                                     \
    X(TestServer_Rejects_HeadersSelfDependence)                                        \
    X(TestServer_Rejects_PrioritySelfDependence)                                       \
    X(TestServer_Rejects_Too_Many_Streams)                                             \
    X(TestServer_Response_Data_Sniff_DoesntOverride)                                   \
    X(TestServer_Response_Data_IgnoreHeaderAfterWrite_After)                           \
    X(TestServer_Response_Data_IgnoreHeaderAfterWrite_Overwrite)                       \
    X(TestServer_Response_Data_SniffLenType)                                           \
    X(TestServer_Response_Header_Flush_MidWrite)                                       \
    X(TestServer_Response_LargeWrite)                                                  \
    X(TestServer_Response_LargeWrite_FlowControlled)                                   \
    X(TestServer_Response_RST_Unblocks_LargeWrite)                                     \
    X(TestServer_Response_Empty_Data_Not_FlowControlled)                               \
    X(TestServer_Response_Automatic100Continue)                                        \
    X(TestServer_Response_ManyHeaders_With_Continuation)                               \
    X(TestServer_Send_GoAway_After_Bogus_WindowUpdate)                                 \
    X(TestServer_Send_RstStream_After_Bogus_WindowUpdate)                              \
    X(TestServerIgnoresContentLengthSignWhenWritingChunks)                             \
    X(TestServerRejectsContentLengthWithSignNewRequests)                               \
    X(TestServerContentLengthDuplicates)                                               \
    X(TestServerDoesntWriteInvalidHeaders)                                             \
    X(TestServerNoAutoContentLengthOnHead)                                             \
    X(TestServerNoDuplicateContentType)                                                \
    X(TestServerContentLengthCanBeDisabled)                                            \
    X(TestServerSendsProcessing)                                                       \
    X(TestServerSendsEarlyHints)                                                       \
    X(TestServerInvalidPathHeader)                                                     \
    X(TestServerPathInitialSlashes)                                                    \
    X(TestServerSettingsFlowControlUpdateBeyondLimit)                                  \
    X(TestServerSettingsFlowControlUpdateWithinLimit)                                  \
    X(TestIssue53)                                                                     \
    X(TestServer_CloseNotify_After_ConnClose)                                          \
    X(TestServer_CloseNotify_After_RSTStream)                                          \
    X(TestServer_CloseNotify_After_StreamError)                                        \
    X(TestServer_DeadConn_Unblocks_Read)                                               \
    X(TestServer_RSTStream_Unblocks_Read)                                              \
    X(TestServer_RSTStream_Unblocks_Header_Write)                                      \
    X(TestServer_Handler_Sends_WindowUpdate)                                           \
    X(TestServer_Handler_Sends_WindowUpdate_Padding)                                   \
    X(TestServer_Response_Stream_With_Missing_Trailer)                                 \
    X(TestServerSettingNoRFC7540Priorities)                                            \
    X(TestServerSettingNoRFC7540PrioritiesInvalid)                                     \
    X(TestCompressionErrorOnClose)                                                     \
    X(TestServer_HandlerWriteErrorOnDisconnect)                                        \
    X(TestServer_MaxDecoderHeaderTableSize)                                            \
    X(TestServerRequestCancelOnError)                                                  \
    X(TestServerReturnsStreamAndConnFlowControlOnBodyClose)                            \
    X(TestServer_NoCrash_HandlerClose_Then_ClientClose)                                \
    X(TestServerContinuationFlood)                                                     \
    X(TestServerContinuationAfterInvalidHeader)                                        \
    X(TestServerDoS_MaxHeaderListSize)                                                 \
    X(TestServerInitialFlowControlWindow)                                              \
    X(TestServerWindowUpdateOnBodyClose)                                               \
    X(TestRequestBodyReadCloseRace)                                                    \
    X(TestServerMaxHandlerGoroutines)                                                  \
    X(TestServerRFC9218Priority)                                                       \
    X(TestServerRFC9218PriorityIgnoredWhenProxied)                                     \
    X(TestServerRFC9218PriorityAware)

TESTING_MAIN(TESTS)
