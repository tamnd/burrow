/* Derived from net/http/internal/http2's transport_test.go in Go 1.27.1, with
 * the testClientConn and testTransport of clientconn_test.go and the frame
 * checks of connframes_test.go: the HTTP/2 client, driven frame by frame from
 * the server's side.
 *
 * Go runs these inside a synctest bubble, over a fake connection that buffers
 * whatever is written to it, and checks that nothing more has happened by
 * waiting for the bubble to go quiet. Here the client has one end of a
 * net_pipe, and on the other end one goroutine reads everything the client
 * writes into a buffer while another passes on what the test writes, so
 * neither side ever waits on the other. Where Go waits for the bubble, these
 * wait for what they expect to arrive, for up to ten seconds, and where Go
 * checks that nothing arrived, they give the client a moment first, which is a
 * weaker check. There is no TLS yet, so the requests are for "http" URLs and
 * the connections speak HTTP/2 with prior knowledge. Go's tests that move a
 * fake clock along, or look inside the client's state, are not here.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http2.h"
#include "../src/xnet/hpack.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/context.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptrace.h"
#include "burrow/os.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

/* How long a check waits for what it expects, and how long one that expects
 * nothing gives the client first. */
#define H2CT_WAIT (10 * TIME_SECOND)
#define H2CT_QUIET (50 * TIME_MILLISECOND)

/* Ends the test function when a check failed, which has already said why. */
#define H2CT_TRY(x)                                                                    \
    do {                                                                               \
        if (!(x))                                                                      \
            return;                                                                    \
    } while (0)

#define S_ BURROW_S_INIT

/* The tests all have their testTransport as tt. */
#define FATALF(...)                                                                    \
    do {                                                                               \
        testing_t_fatalf_v(tt->t, __VA_ARGS__);                                        \
        return;                                                                        \
    } while (0)

typedef struct H2ctTT H2ctTT;

/* ------------------------------------------------- the server's end of a conn */

/* testClientConn, without the ClientConn: the server's end of one
 * connection, which the test reads frames from and writes frames to. */
typedef struct H2ctConn {
    TestingT *t;
    Alloc *a;
    NetConn srv;
    Http2Framer *fr;
    HpackDecoder *dec;
    HpackEncoder *enc;
    BytesBuffer hbuf;
    Time rdeadline; /* for the framer's reads */
    /* What the client writes, which in_job reads off srv into rbuf. reof is
     * set once srv has nothing more to give. */
    SyncMutex rmu;
    BytesBuffer rbuf;
    SyncWaitGroup rwg;
    /* What the test writes, which out_job passes on to srv. */
    SyncMutex wmu;
    SyncCond wcond;
    BytesBuffer wbuf;
    SyncWaitGroup wwg;
    struct H2ctConn *next;
    bool reof;
    bool wclosed;
    bool wfailed;
    bool claimed; /* given to the test by get_conn */
} H2ctConn;

enum { H2CT_CHUNK = 16 << 10 };

/* Reads what the client writes into rbuf until srv ends. */
static void h2ct_in_job(void *env) {
    H2ctConn *tc = (H2ctConn *)env;
    Byte *chunk = (Byte *)mem_alloc_nozero(tc->a, H2CT_CHUNK, 1);
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Int n = 0;
        if (chunk != NULL)
            n = tc->srv.vt->reader.read(
                tc->srv.data, slice_from(chunk, H2CT_CHUNK, H2CT_CHUNK, TYPE_BYTE),
                &err);
        sync_mutex_lock(&tc->rmu);
        if (n > 0) {
            Error werr = BURROW_NO_ERROR;
            (void)bytes_buffer_write(&tc->rbuf, slice_from(chunk, n, n, TYPE_BYTE),
                                     &werr);
            if (BURROW_FAILED(werr))
                err = werr;
        }
        bool done = chunk == NULL || BURROW_FAILED(err);
        if (done)
            tc->reof = true;
        sync_mutex_unlock(&tc->rmu);
        if (done)
            break;
    }
    if (chunk != NULL)
        mem_free(tc->a, chunk, H2CT_CHUNK, 1);
}

/* What the framer reads: rbuf, waiting for it until rdeadline, and the end of
 * it once srv has ended and it is empty. */
static Int h2ct_in_read(void *self, Slice p, Error *err) {
    H2ctConn *tc = (H2ctConn *)self;
    for (;;) {
        sync_mutex_lock(&tc->rmu);
        if (bytes_buffer_len(&tc->rbuf) > 0) {
            Int n = bytes_buffer_read(&tc->rbuf, p, err);
            sync_mutex_unlock(&tc->rmu);
            return n;
        }
        bool eof = tc->reof;
        sync_mutex_unlock(&tc->rmu);
        if (eof) {
            *err = io_eof;
            return 0;
        }
        if (!time_before(time_now(), tc->rdeadline)) {
            *err = os_err_deadline_exceeded;
            return 0;
        }
        time_sleep(TIME_MILLISECOND);
    }
}

static const IoReaderVT h2ct_in_vt = {NULL, h2ct_in_read};

static Int h2ct_out_write(void *self, Slice p, Error *err) {
    H2ctConn *tc = (H2ctConn *)self;
    sync_mutex_lock(&tc->wmu);
    if (tc->wfailed || tc->wclosed) {
        sync_mutex_unlock(&tc->wmu);
        *err = io_err_closed_pipe;
        return 0;
    }
    Int n = bytes_buffer_write(&tc->wbuf, p, err);
    sync_cond_signal(&tc->wcond);
    sync_mutex_unlock(&tc->wmu);
    return n;
}

static const IoWriterVT h2ct_out_vt = {NULL, h2ct_out_write};

/* Passes wbuf on to srv until the conn closes, or a write to srv fails. */
static void h2ct_out_job(void *env) {
    H2ctConn *tc = (H2ctConn *)env;
    Byte *chunk = (Byte *)mem_alloc_nozero(tc->a, H2CT_CHUNK, 1);
    sync_mutex_lock(&tc->wmu);
    if (chunk == NULL)
        tc->wfailed = true;
    while (!tc->wfailed) {
        while (bytes_buffer_len(&tc->wbuf) == 0 && !tc->wclosed)
            sync_cond_wait(&tc->wcond);
        if (tc->wclosed)
            break;
        Error err = BURROW_NO_ERROR;
        Int n = bytes_buffer_read(
            &tc->wbuf, slice_from(chunk, H2CT_CHUNK, H2CT_CHUNK, TYPE_BYTE), &err);
        sync_mutex_unlock(&tc->wmu);
        (void)tc->srv.vt->writer.write(tc->srv.data, slice_from(chunk, n, n, TYPE_BYTE),
                                       &err);
        sync_mutex_lock(&tc->wmu);
        if (BURROW_FAILED(err))
            tc->wfailed = true;
    }
    sync_mutex_unlock(&tc->wmu);
    if (chunk != NULL)
        mem_free(tc->a, chunk, H2CT_CHUNK, 1);
}

/* closeWrite: hangs up on the client, which then reads the end of the
 * connection. What the test wrote and the client has not read yet goes. */
static void h2ct_conn_close(H2ctConn *tc) {
    sync_mutex_lock(&tc->wmu);
    tc->wclosed = true;
    sync_cond_signal(&tc->wcond);
    sync_mutex_unlock(&tc->wmu);
    (void)tc->srv.vt->closer.close(tc->srv.data);
}

static void h2ct_conn_free(H2ctConn *tc) {
    h2ct_conn_close(tc);
    sync_wait_group_wait(&tc->wwg);
    sync_wait_group_wait(&tc->rwg);
    if (tc->fr != NULL)
        burrow__http2_framer_free(tc->fr);
    if (tc->dec != NULL)
        burrow__hpack_decoder_free(tc->dec);
    if (tc->enc != NULL)
        burrow__hpack_encoder_free(tc->enc);
    bytes_buffer_free(&tc->hbuf);
    bytes_buffer_free(&tc->rbuf);
    bytes_buffer_free(&tc->wbuf);
    net_pipe_free(tc->srv);
    mem_free(tc->a, tc, sizeof *tc, _Alignof(H2ctConn));
}

/* ------------------------------------------------------------ the transport */

/* A request body for the tests, as Go's testRequestBody: bytes the test gives
 * it, or a count of arbitrary ones, then err. A read waits for one of them. */
typedef struct H2ctBody {
    SyncMutex mu;
    SyncCond cond;
    BytesBuffer buf;
    Int bytes;
    Error err;
    struct H2ctBody *next;
} H2ctBody;

/* testRoundTrip. */
typedef struct H2ctRT {
    H2ctTT *tt;
    HttpRequest *req;
    HttpResponse *res;
    Error err; /* in arena */
    Arena arena;
    Context cctx;
    Context vctx;
    ContextCancelFunc cancel;
    SyncAtomicInt64 id;
    SyncAtomicBool done;
    struct H2ctRT *next;
    bool via_t2;
} H2ctRT;

/* testTransport: an HttpTransport that speaks unencrypted HTTP/2 over pipes
 * it dials here, each with a conn for the test to take with get_conn. t2 is
 * newTestClientConn's, which round trips that bypass tr1 go to. */
struct H2ctTT {
    TestingT *t;
    Alloc *a;
    Arena ar; /* for what a test needs until the end */
    HttpTransport tr1;
    HttpProtocols protos;
    Http2Transport *t2;
    SyncMutex mu;
    H2ctConn *conns;
    H2ctConn **conns_tail;
    H2ctRT *rts;
    H2ctBody *bodies;
    SyncWaitGroup rtwg;
};

static H2ctConn *h2ct_conn_new(H2ctTT *tt, NetConn srv) {
    Alloc *a = tt->a;
    H2ctConn *tc = (H2ctConn *)mem_alloc(a, sizeof *tc, _Alignof(H2ctConn));
    if (tc == NULL)
        return NULL;
    tc->t = tt->t;
    tc->a = a;
    tc->srv = srv;
    tc->hbuf = BYTES_BUFFER(a);
    tc->rbuf = BYTES_BUFFER(a);
    tc->wbuf = BYTES_BUFFER(a);
    tc->wcond = SYNC_COND(sync_mutex_locker(&tc->wmu));
    tc->rdeadline = time_now();
    IoWriter w = {&h2ct_out_vt, tc};
    IoReader r = {&h2ct_in_vt, tc};
    tc->fr = burrow__http2_new_framer(a, w, r);
    tc->dec = burrow__hpack_new_decoder(a, HTTP2_INITIAL_HEADER_TABLE_SIZE, NULL, NULL);
    tc->enc = burrow__hpack_new_encoder(a, bytes_buffer_as_io_writer(&tc->hbuf));
    if (tc->fr != NULL && tc->dec != NULL) {
        tc->fr->read_meta_headers = tc->dec;
        burrow__http2_framer_set_max_read_frame_size(tc->fr, 10 << 20);
    }
    if (!sync_wait_group_go(&tc->rwg, BURROW_FN(Func, h2ct_in_job, tc)))
        tc->reof = true;
    if (!sync_wait_group_go(&tc->wwg, BURROW_FN(Func, h2ct_out_job, tc)))
        tc->wfailed = true;
    sync_mutex_lock(&tt->mu);
    *tt->conns_tail = tc;
    tt->conns_tail = &tc->next;
    sync_mutex_unlock(&tt->mu);
    return tc;
}

/* tr1's DialContext. */
static NetConn h2ct_dial(void *env, Context ctx, Str network, Str addr, Error *err) {
    (void)ctx;
    (void)network;
    (void)addr;
    H2ctTT *tt = (H2ctTT *)env;
    NetConn cli;
    NetConn srv;
    net_pipe(tt->a, &cli, &srv);
    if (cli.data == NULL) {
        *err = burrow_err_out_of_memory;
        return cli;
    }
    if (h2ct_conn_new(tt, srv) == NULL) {
        net_pipe_free(srv);
        *err = burrow_err_out_of_memory;
        return (NetConn){NULL, NULL};
    }
    *err = BURROW_NO_ERROR;
    return cli;
}

/* The pipe is the test's conn's to free, once the transports are gone. */
static void h2ct_free_conn(void *env, NetConn c) {
    (void)env;
    (void)c;
}

/* newTestTransport. */
static void h2ct_tt_start(H2ctTT *tt, TestingT *t) {
    memset(tt, 0, sizeof *tt);
    tt->t = t;
    tt->a = heap_allocator();
    arena_init(&tt->ar, tt->a, 0);
    http_protocols_set_unencrypted_http2(&tt->protos, true);
    tt->tr1.protocols = &tt->protos;
    tt->tr1.dial_context = BURROW_FN(HttpDialContextFunc, h2ct_dial, tt);
    tt->tr1.free_conn = BURROW_FN(HttpFreeConnFunc, h2ct_free_conn, tt);
    tt->conns_tail = &tt->conns;
}

static void h2ct_body_close_with_error(H2ctBody *b, Error err);

/* The cleanups Go's test helpers register. Every round trip still going is
 * cancelled and every connection hung up on, so they end, and then it all
 * goes. */
static void h2ct_tt_close(H2ctTT *tt) {
    sync_mutex_lock(&tt->mu);
    Int unclaimed = 0;
    for (H2ctConn *tc = tt->conns; tc != NULL; tc = tc->next) {
        if (!tc->claimed)
            unclaimed++;
    }
    H2ctRT *rts = tt->rts;
    sync_mutex_unlock(&tt->mu);
    if (unclaimed > 0)
        testing_t_errorf_v(tt->t,
                           "%d test ClientConns created, but not examined by test",
                           (int)unclaimed);

    for (H2ctBody *b = tt->bodies; b != NULL; b = b->next)
        h2ct_body_close_with_error(b, io_err_closed_pipe);
    for (H2ctRT *rt = rts; rt != NULL; rt = rt->next)
        BURROW_CALLF0(rt->cancel);
    for (int pass = 0; pass < 2; pass++) {
        /* A retry can dial again while the first pass hangs up. */
        sync_mutex_lock(&tt->mu);
        for (H2ctConn *tc = tt->conns; tc != NULL; tc = tc->next)
            h2ct_conn_close(tc);
        sync_mutex_unlock(&tt->mu);
        if (pass == 0)
            sync_wait_group_wait(&tt->rtwg);
    }
    for (H2ctRT *rt = rts; rt != NULL; rt = rt->next)
        http_response_free(rt->res);
    burrow__http2_transport_free(tt->t2);
    http_transport_free(&tt->tr1);

    while (tt->conns != NULL) {
        H2ctConn *tc = tt->conns;
        tt->conns = tc->next;
        h2ct_conn_free(tc);
    }
    while (tt->rts != NULL) {
        H2ctRT *rt = tt->rts;
        tt->rts = rt->next;
        http_request_free(rt->req);
        context_release(rt->cctx);
        context_release(rt->vctx);
        arena_free(&rt->arena);
        mem_free(tt->a, rt, sizeof *rt, _Alignof(H2ctRT));
    }
    while (tt->bodies != NULL) {
        H2ctBody *b = tt->bodies;
        tt->bodies = b->next;
        bytes_buffer_free(&b->buf);
        mem_free(tt->a, b, sizeof *b, _Alignof(H2ctBody));
    }
    arena_free(&tt->ar);
}

typedef void (*H2ctTestFn)(H2ctTT *tt, const void *arg);

/* Runs fn with a new testTransport, and cleans up after it. */
static void h2ct_run(TestingT *t, H2ctTestFn fn, const void *arg) {
    H2ctTT tt;
    h2ct_tt_start(&tt, t);
    fn(&tt, arg);
    h2ct_tt_close(&tt);
}

/* readClientPreface. */
static bool h2ct_read_client_preface(H2ctConn *tc) {
    static const char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    Byte buf[sizeof preface - 1];
    tc->rdeadline = time_add(time_now(), H2CT_WAIT);
    Error err = BURROW_NO_ERROR;
    (void)io_read_full((IoReader){&h2ct_in_vt, tc},
                       slice_from(buf, sizeof buf, sizeof buf, TYPE_BYTE), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(tc->t, "reading preface: %v", err);
        return false;
    }
    if (memcmp(buf, preface, sizeof buf) != 0) {
        testing_t_errorf_v(tc->t, "client preface: %q, want %q",
                           str_from_bytes(buf, (Int)sizeof buf),
                           str_from_cstr(preface));
        return false;
    }
    return true;
}

/* getConn: the next connection the client made, once its preface is in. */
static H2ctConn *h2ct_get_conn(H2ctTT *tt) {
    Time deadline = time_add(time_now(), H2CT_WAIT);
    for (;;) {
        sync_mutex_lock(&tt->mu);
        H2ctConn *tc = tt->conns;
        while (tc != NULL && tc->claimed)
            tc = tc->next;
        if (tc != NULL)
            tc->claimed = true;
        sync_mutex_unlock(&tt->mu);
        if (tc != NULL) {
            if (tc->fr == NULL || tc->dec == NULL || tc->enc == NULL) {
                testing_t_errorf_v(tt->t, "no memory for the test's conn");
                return NULL;
            }
            return h2ct_read_client_preface(tc) ? tc : NULL;
        }
        if (!time_before(time_now(), deadline)) {
            testing_t_errorf_v(tt->t, "no new ClientConns created; wanted one");
            return NULL;
        }
        time_sleep(TIME_MILLISECOND);
    }
}

/* newTestClientConn: a connection added to t2, which the round trips of
 * h2ct_tc_round_trip use without going through tr1. */
static H2ctConn *h2ct_new_client_conn(H2ctTT *tt) {
    tt->t2 = burrow__http2_new_transport(&tt->tr1);
    if (tt->t2 == NULL) {
        testing_t_errorf_v(tt->t, "no memory for the Transport");
        return NULL;
    }
    NetConn cli;
    NetConn srv;
    net_pipe(tt->a, &cli, &srv);
    if (cli.data == NULL) {
        testing_t_errorf_v(tt->t, "no memory for the pipe");
        return NULL;
    }
    if (h2ct_conn_new(tt, srv) == NULL) {
        net_pipe_free(srv);
        testing_t_errorf_v(tt->t, "no memory for the test's conn");
        return NULL;
    }
    Error err = burrow__http2_transport_add_conn(tt->t2, BURROW_S("http"),
                                                 BURROW_S("dummy.tld"), cli);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(tt->t, "newClientConn: %v", err);
        return NULL;
    }
    return h2ct_get_conn(tt);
}

/* ---------------------------------------------------------- request bodies */

static Int h2ct_body_read(void *self, Slice p, Error *err) {
    H2ctBody *b = (H2ctBody *)self;
    sync_mutex_lock(&b->mu);
    while (bytes_buffer_len(&b->buf) == 0 && b->bytes == 0 && BURROW_OK(b->err))
        sync_cond_wait(&b->cond);
    Int n = 0;
    *err = BURROW_NO_ERROR;
    if (bytes_buffer_len(&b->buf) > 0) {
        n = bytes_buffer_read(&b->buf, p, err);
    } else if (b->bytes > 0) {
        n = p.len < b->bytes ? p.len : b->bytes;
        b->bytes -= n;
        memset(p.p, 'A', (size_t)n);
    } else {
        *err = b->err;
    }
    sync_mutex_unlock(&b->mu);
    return n;
}

static const IoReaderVT h2ct_body_vt = {NULL, h2ct_body_read};

/* newRequestBody. */
static H2ctBody *h2ct_new_request_body(H2ctTT *tt) {
    H2ctBody *b = (H2ctBody *)mem_alloc(tt->a, sizeof *b, _Alignof(H2ctBody));
    if (b == NULL) {
        testing_t_errorf_v(tt->t, "no memory for the body");
        return NULL;
    }
    b->cond = SYNC_COND(sync_mutex_locker(&b->mu));
    b->buf = BYTES_BUFFER(tt->a);
    b->next = tt->bodies;
    tt->bodies = b;
    return b;
}

/* writeBytes: n more arbitrary bytes. */
static void h2ct_body_write_bytes(H2ctBody *b, Int n) {
    sync_mutex_lock(&b->mu);
    b->bytes += n;
    sync_cond_broadcast(&b->cond);
    sync_mutex_unlock(&b->mu);
}

/* closeWithError: what a read gives once the rest is read. */
static void h2ct_body_close_with_error(H2ctBody *b, Error err) {
    sync_mutex_lock(&b->mu);
    if (BURROW_OK(b->err))
        b->err = err;
    sync_cond_broadcast(&b->cond);
    sync_mutex_unlock(&b->mu);
}

/* -------------------------------------------------------------- round trips */

static HttpRequest *h2ct_new_request(H2ctTT *tt, Context ctx, Str method,
                                     IoReader body) {
    Error err = BURROW_NO_ERROR;
    if (ctx.vt == NULL)
        ctx = context_background();
    HttpRequest *req = http_new_request_with_context(
        tt->a, ctx, method, BURROW_S("http://dummy.tld/"), body, &err);
    if (req == NULL)
        testing_t_errorf_v(tt->t, "NewRequest: %v", err);
    return req;
}

static void h2ct_rt_job(void *env) {
    H2ctRT *rt = (H2ctRT *)env;
    Error err = BURROW_NO_ERROR;
    HttpResponse *res;
    if (rt->via_t2)
        res = burrow__http2_transport_round_trip(rt->tt->t2, rt->req, &err);
    else
        res = http_transport_round_trip(&rt->tt->tr1, rt->req, &err);
    rt->res = res;
    rt->err = error_retain(arena_allocator(&rt->arena), err);
    sync_atomic_bool_store(&rt->done, true);
}

/* roundTrip: starts req on its way, with a context of its own that the test
 * can cancel and that tells it the request's stream ID. The test transport
 * has req from then on. via_t2 is tc.roundTrip, and the rest tt.roundTrip. */
static H2ctRT *h2ct_round_trip_via(H2ctTT *tt, HttpRequest *req, bool via_t2) {
    if (req == NULL)
        return NULL;
    H2ctRT *rt = (H2ctRT *)mem_alloc(tt->a, sizeof *rt, _Alignof(H2ctRT));
    if (rt == NULL) {
        http_request_free(req);
        testing_t_errorf_v(tt->t, "no memory for the round trip");
        return NULL;
    }
    arena_init(&rt->arena, tt->a, 0);
    rt->tt = tt;
    rt->req = req;
    rt->via_t2 = via_t2;
    rt->cctx = context_with_cancel(tt->a, http_request_context(req), &rt->cancel);
    rt->vctx = context_with_value(tt->a, rt->cctx, burrow__http2_stream_id_hook_key,
                                  BURROW_ANY(TYPE_INT64, &rt->id));
    req->ctx = rt->vctx;
    sync_mutex_lock(&tt->mu);
    rt->next = tt->rts;
    tt->rts = rt;
    sync_mutex_unlock(&tt->mu);
    if (!sync_wait_group_go(&tt->rtwg, BURROW_FN(Func, h2ct_rt_job, rt))) {
        rt->err = burrow_err_out_of_memory;
        sync_atomic_bool_store(&rt->done, true);
    }
    return rt;
}

static H2ctRT *h2ct_tc_round_trip(H2ctTT *tt, HttpRequest *req) {
    return h2ct_round_trip_via(tt, req, true);
}

static H2ctRT *h2ct_tt_round_trip(H2ctTT *tt, HttpRequest *req) {
    return h2ct_round_trip_via(tt, req, false);
}

/* streamID, once the client has given the request one. */
static uint32_t h2ct_rt_stream_id(H2ctRT *rt) {
    Time deadline = time_add(time_now(), H2CT_WAIT);
    for (;;) {
        int64_t id = sync_atomic_int64_load(&rt->id);
        if (id != 0)
            return (uint32_t)id;
        if (!time_before(time_now(), deadline) || sync_atomic_bool_load(&rt->done)) {
            testing_t_errorf_v(rt->tt->t, "stream ID unknown");
            return 0;
        }
        time_sleep(TIME_MILLISECOND);
    }
}

/* done: whether RoundTrip has returned, after giving it a moment. */
static bool h2ct_rt_done(H2ctRT *rt) {
    time_sleep(H2CT_QUIET);
    return sync_atomic_bool_load(&rt->done);
}

/* result: false when RoundTrip has not returned in time. */
static bool h2ct_rt_result(H2ctRT *rt) {
    Time deadline = time_add(time_now(), H2CT_WAIT);
    while (!sync_atomic_bool_load(&rt->done)) {
        if (!time_before(time_now(), deadline)) {
            testing_t_errorf_v(rt->tt->t, "RoundTrip is not done; want it to be");
            return false;
        }
        time_sleep(TIME_MILLISECOND);
    }
    return true;
}

/* response: the response of a successful RoundTrip, or NULL. */
static HttpResponse *h2ct_rt_response(H2ctRT *rt) {
    if (!h2ct_rt_result(rt))
        return NULL;
    if (BURROW_FAILED(rt->err)) {
        testing_t_errorf_v(rt->tt->t, "RoundTrip returned unexpected error: %v",
                           rt->err);
        return NULL;
    }
    if (rt->res == NULL)
        testing_t_errorf_v(rt->tt->t, "RoundTrip returned nil *Response and nil error");
    return rt->res;
}

static bool h2ct_rt_want_status(H2ctRT *rt, Int want) {
    HttpResponse *res = h2ct_rt_response(rt);
    if (res == NULL)
        return false;
    if (res->status_code != want) {
        testing_t_errorf_v(rt->tt->t, "got response status %d, want %d",
                           (int)res->status_code, (int)want);
        return false;
    }
    return true;
}

/* readBody, into the transport's arena. */
static Slice h2ct_rt_read_body(H2ctRT *rt, Error *err) {
    HttpResponse *res = h2ct_rt_response(rt);
    if (res == NULL) {
        *err = burrow_err_out_of_memory;
        return slice_make(arena_allocator(&rt->tt->ar), TYPE_BYTE, 0, 0);
    }
    return io_read_all(arena_allocator(&rt->tt->ar),
                       io_read_closer_as_io_reader(res->body), err);
}

static bool h2ct_rt_want_body(H2ctRT *rt, const char *want) {
    if (h2ct_rt_response(rt) == NULL)
        return false;
    Error err = BURROW_NO_ERROR;
    Slice got = h2ct_rt_read_body(rt, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(rt->tt->t, "unexpected error reading response body: %v",
                           err);
        return false;
    }
    Str g = str_from_bytes(got.p, got.len);
    if (!str_eq(g, str_from_cstr(want))) {
        testing_t_errorf_v(rt->tt->t, "unexpected response body:\ngot:  %q\nwant: %q",
                           g, str_from_cstr(want));
        return false;
    }
    return true;
}

/* diffHeaders, by the header as HTTP/1 writes it, which has the keys sorted:
 * want is that, and "" for none. */
static bool h2ct_want_header(TestingT *t, const char *what, HttpHeader h,
                             const char *want) {
    BytesBuffer b = BYTES_BUFFER(heap_allocator());
    Error err = BURROW_NO_ERROR;
    if (h != NULL)
        err = http_header_write(h, bytes_buffer_as_io_writer(&b));
    Slice got = bytes_buffer_bytes(&b);
    Str g = str_from_bytes(got.p, got.len);
    bool ok = BURROW_OK(err) && str_eq(g, str_from_cstr(want));
    if (!ok)
        testing_t_errorf_v(t, "unexpected response %s:\ngot:  %q\nwant: %q",
                           str_from_cstr(what), g, str_from_cstr(want));
    bytes_buffer_free(&b);
    return ok;
}

static bool h2ct_rt_want_trailers(H2ctRT *rt, const char *want) {
    HttpResponse *res = h2ct_rt_response(rt);
    return res != NULL && h2ct_want_header(rt->tt->t, "trailers", res->trailer, want);
}

/* ------------------------------------------------------- reading frames */

static Http2Frame *h2ct_read_frame_within(H2ctConn *tc, Duration d, Error *err) {
    tc->rdeadline = time_add(time_now(), d);
    return burrow__http2_framer_read_frame(tc->fr, err);
}

/* readFrame: the next frame, or NULL at the end of the connection or when no
 * frame came. */
static Http2Frame *h2ct_read_frame(H2ctConn *tc) {
    Error err = BURROW_NO_ERROR;
    Http2Frame *f = h2ct_read_frame_within(tc, H2CT_WAIT, &err);
    if (BURROW_FAILED(err)) {
        if (!errors_is(err, io_eof) && !errors_is(err, os_err_deadline_exceeded))
            testing_t_errorf_v(tc->t, "ReadFrame: %v", err);
        burrow__http2_frame_free(f);
        return NULL;
    }
    return f;
}

/* readFrame[T]: the next frame when it is of type want. */
static Http2Frame *h2ct_read_type(H2ctConn *tc, Http2FrameType want) {
    Http2Frame *f = h2ct_read_frame(tc);
    if (f == NULL) {
        testing_t_errorf_v(tc->t, "got no frame, want frame type %d", (int)want);
        return NULL;
    }
    if (f->header.type != want) {
        testing_t_errorf_v(tc->t, "got frame type %d on stream %d, want type %d",
                           (int)f->header.type, (int)f->header.stream_id, (int)want);
        burrow__http2_frame_free(f);
        return NULL;
    }
    return f;
}

static bool h2ct_want_frame_type(H2ctConn *tc, Http2FrameType want) {
    Http2Frame *f = h2ct_read_type(tc, want);
    burrow__http2_frame_free(f);
    return f != NULL;
}

/* wantHeaders: a HEADERS frame on stream id, and for each name in want, the
 * values the frame has under it are the ones want gives it, in order. want
 * holds n names and values. */
static bool h2ct_want_headers(H2ctConn *tc, uint32_t id, bool end_stream,
                              const Str *want, size_t n) {
    Http2Frame *f = h2ct_read_type(tc, HTTP2_FRAME_HEADERS);
    if (f == NULL)
        return false;
    bool ok = true;
    if (f->header.stream_id != id) {
        testing_t_errorf_v(tc->t, "got stream ID %d, want %d", (int)f->header.stream_id,
                           (int)id);
        ok = false;
    }
    bool ended = (f->header.flags & HTTP2_FLAG_HEADERS_END_STREAM) != 0;
    if (ok && ended != end_stream) {
        testing_t_errorf_v(tc->t, "got stream ended %t, want %t", ended, end_stream);
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
                testing_t_errorf_v(tc->t, "got header %q = %q; want %q", want[i],
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

/* wantData: size bytes of DATA on stream id, in one frame or, with
 * multiple, in as many as it takes, and data when it is not NULL. */
static bool h2ct_want_data(H2ctConn *tc, uint32_t id, bool end_stream, Int size,
                           const char *data, bool multiple) {
    if (data != NULL)
        size = (Int)strlen(data);
    Int got_size = 0;
    bool got_end_stream = false;
    bool ok = true;
    for (;;) {
        Http2Frame *f = h2ct_read_frame(tc);
        if (f == NULL)
            break;
        if (f->header.type != HTTP2_FRAME_DATA) {
            testing_t_errorf_v(tc->t, "got frame type %d, want DataFrame",
                               (int)f->header.type);
            burrow__http2_frame_free(f);
            return false;
        }
        Slice d = f->u.data.data;
        if (data != NULL && f->header.stream_id == id &&
            (got_size + d.len > size ||
             memcmp(data + got_size, d.p, (size_t)d.len) != 0)) {
            testing_t_errorf_v(tc->t, "got data %q at %d, want %q",
                               str_from_bytes(d.p, d.len), (int)got_size,
                               str_from_cstr(data));
            ok = false;
        }
        if (f->header.stream_id != id) {
            testing_t_errorf_v(tc->t, "got DATA on stream %d, want %d",
                               (int)f->header.stream_id, (int)id);
            ok = false;
        }
        got_size += d.len;
        bool ended = (f->header.flags & HTTP2_FLAG_DATA_END_STREAM) != 0;
        burrow__http2_frame_free(f);
        if (!ok)
            return false;
        if (ended) {
            got_end_stream = true;
            break;
        }
        if (!end_stream && got_size >= size)
            break;
        if (!multiple)
            break;
    }
    if (got_size != size) {
        testing_t_errorf_v(tc->t, "got %d bytes of DATA frames, want %d", (int)got_size,
                           (int)size);
        return false;
    }
    if (got_end_stream != end_stream) {
        testing_t_errorf_v(tc->t,
                           "after %d bytes of DATA frames, got END_STREAM=%t; want %t",
                           (int)got_size, got_end_stream, end_stream);
        return false;
    }
    return true;
}

/* -------------------------------------------------------- writing frames */

static bool h2ct_failed(H2ctConn *tc, const char *what, Error err) {
    if (BURROW_OK(err))
        return false;
    testing_t_errorf_v(tc->t, "%s: %v", str_from_cstr(what), err);
    return true;
}

static bool h2ct_write_settings(H2ctConn *tc, const Http2Setting *s, Int n) {
    return !h2ct_failed(tc, "writing SETTINGS",
                        burrow__http2_framer_write_settings(tc->fr, s, n));
}

static bool h2ct_write_settings_ack(H2ctConn *tc) {
    return !h2ct_failed(tc, "writing SETTINGS ACK",
                        burrow__http2_framer_write_settings_ack(tc->fr));
}

static bool h2ct_write_data(H2ctConn *tc, uint32_t id, bool end_stream,
                            const char *data) {
    Int n = (Int)strlen(data);
    Slice p = slice_from((void *)(uintptr_t)data, n, n, TYPE_BYTE);
    return !h2ct_failed(tc, "writing DATA",
                        burrow__http2_framer_write_data(tc->fr, id, end_stream, p));
}

static bool h2ct_write_go_away(H2ctConn *tc, uint32_t max_stream_id,
                               Http2ErrCode code) {
    Slice none = {0};
    return !h2ct_failed(
        tc, "writing GOAWAY",
        burrow__http2_framer_write_go_away(tc->fr, max_stream_id, code, none));
}

static bool h2ct_write_rst_stream(H2ctConn *tc, uint32_t id, Http2ErrCode code) {
    return !h2ct_failed(tc, "writing RST_STREAM",
                        burrow__http2_framer_write_rst_stream(tc->fr, id, code));
}

/* makeHeaderBlockFragment: kv holds n names and values. The block is good
 * until the next one. */
static Slice h2ct_block(H2ctConn *tc, const Str *kv, size_t n) {
    bytes_buffer_reset(&tc->hbuf);
    for (size_t i = 0; i + 1 < n; i += 2) {
        HpackHeaderField hf;
        hf.name = kv[i];
        hf.value = kv[i + 1];
        hf.sensitive = false;
        if (BURROW_FAILED(burrow__hpack_encoder_write_field(tc->enc, hf)))
            testing_t_errorf_v(tc->t, "encoding %q", kv[i]);
    }
    return bytes_buffer_bytes(&tc->hbuf);
}

static Http2HeadersFrameParam h2ct_hfp(uint32_t id, Slice block, bool end_stream) {
    Http2HeadersFrameParam p;
    memset(&p, 0, sizeof p);
    p.stream_id = id;
    p.block_fragment = block;
    p.end_stream = end_stream;
    p.end_headers = true;
    return p;
}

static bool h2ct_write_headers(H2ctConn *tc, Http2HeadersFrameParam p) {
    return !h2ct_failed(tc, "writing HEADERS",
                        burrow__http2_framer_write_headers(tc->fr, p));
}

typedef enum H2ctHeaderType {
    H2CT_NO_HEADER, /* omitted */
    H2CT_ONE_HEADER,
    H2CT_SPLIT_HEADER, /* broken into continuation on purpose */
} H2ctHeaderType;

/* writeHeadersMode. */
static bool h2ct_write_headers_mode(H2ctConn *tc, H2ctHeaderType mode,
                                    Http2HeadersFrameParam p) {
    switch (mode) {
    case H2CT_NO_HEADER:
        return true;
    case H2CT_ONE_HEADER:
        return h2ct_write_headers(tc, p);
    case H2CT_SPLIT_HEADER: {
        if (p.block_fragment.len < 2) {
            testing_t_errorf_v(tc->t, "too small");
            return false;
        }
        Slice cont_data = slice_sub(p.block_fragment, 1, p.block_fragment.len);
        bool cont_end = p.end_headers;
        p.block_fragment = slice_sub(p.block_fragment, 0, 1);
        p.end_headers = false;
        if (!h2ct_write_headers(tc, p))
            return false;
        return !h2ct_failed(tc, "writing CONTINUATION",
                            burrow__http2_framer_write_continuation(
                                tc->fr, p.stream_id, cont_end, cont_data));
    }
    default:
        testing_t_errorf_v(tc->t, "bogus mode");
        return false;
    }
}

/* greet: the client's SETTINGS and WINDOW_UPDATE, then settings back, and
 * the client's ACK of them. */
static bool h2ct_greet(H2ctConn *tc, const Http2Setting *s, Int n) {
    return h2ct_want_frame_type(tc, HTTP2_FRAME_SETTINGS) &&
           h2ct_want_frame_type(tc, HTTP2_FRAME_WINDOW_UPDATE) &&
           h2ct_write_settings(tc, s, n) && h2ct_write_settings_ack(tc) &&
           h2ct_want_frame_type(tc, HTTP2_FRAME_SETTINGS);
}

/* ------------------------------------------------------------------ tests */

static const IoReader h2ct_no_body = {NULL, NULL};

/* TestTestClientConn demonstrates usage of testClientConn. */
static void h2ct_test_client_conn(H2ctTT *tt, const void *arg) {
    (void)arg;
    /* newTestClientConn creates a ClientConn and surrounding test
     * infrastructure. */
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL);

    /* greet reads the client's initial SETTINGS and WINDOW_UPDATE frames, and
     * sends a SETTINGS frame to the client. */
    H2CT_TRY(h2ct_greet(tc, NULL, 0));

    /* Request bodies must either be constant or created with
     * new_request_body. */
    H2ctBody *body = h2ct_new_request_body(tt);
    H2CT_TRY(body != NULL);
    h2ct_body_write_bytes(body, 10);          /* 10 arbitrary bytes... */
    h2ct_body_close_with_error(body, io_eof); /* ...followed by EOF. */

    /* roundTrip calls RoundTrip, but does not wait for it to return. */
    H2ctRT *rt =
        h2ct_tc_round_trip(tt, h2ct_new_request(tt, (Context){0}, BURROW_S("PUT"),
                                                (IoReader){&h2ct_body_vt, body}));
    H2CT_TRY(rt != NULL);

    /* Look for headers and the request body. */
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    static const Str want[] = {S_(":authority"), S_("dummy.tld"), S_(":method"),
                               S_("PUT"),        S_(":path"),     S_("/")};
    H2CT_TRY(h2ct_want_headers(tc, id, false, want, 6));
    /* Expect 10 bytes of request body in DATA frames. */
    H2CT_TRY(h2ct_want_data(tc, id, true, 10, NULL, true));

    /* Send a HEADERS frame back to the client. */
    static const Str res[] = {S_(":status"), S_("200")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, res, 2), true)));

    /* Now that we've received headers, RoundTrip has finished. */
    H2CT_TRY(h2ct_rt_want_status(rt, 200));
    (void)h2ct_rt_want_body(rt, "");
}

static void TestTestClientConn(TestingT *t) {
    h2ct_run(t, h2ct_test_client_conn, NULL);
}

typedef struct H2ctPattern {
    H2ctHeaderType expect100_continue;
    H2ctHeaderType res_header;
    H2ctHeaderType trailers;
    bool with_data;
} H2ctPattern;

static void h2ct_res_pattern(H2ctTT *tt, const void *arg) {
    const H2ctPattern *p = (const H2ctPattern *)arg;
    static const char req_body[] = "some request body";
    static const char res_body[] = "some response body";

    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    StringsReader *sr =
        strings_new_reader(arena_allocator(&tt->ar), str_from_cstr(req_body));
    H2CT_TRY(sr != NULL);
    HttpRequest *req = h2ct_new_request(tt, (Context){0}, BURROW_S("POST"),
                                        strings_reader_as_io_reader(sr));
    H2CT_TRY(req != NULL);
    if (p->expect100_continue != H2CT_NO_HEADER)
        (void)http_header_set(req->header, BURROW_S("Expect"),
                              BURROW_S("100-continue"));
    H2ctRT *rt = h2ct_tc_round_trip(tt, req);
    H2CT_TRY(rt != NULL);

    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);

    /* Possibly 100-continue, or skip when there is none. */
    static const Str status100[] = {S_(":status"), S_("100")};
    H2CT_TRY(h2ct_write_headers_mode(
        tc, p->expect100_continue, h2ct_hfp(id, h2ct_block(tc, status100, 2), false)));

    /* Client sends request body. */
    H2CT_TRY(h2ct_want_data(tc, id, true, (Int)strlen(req_body), NULL, false));

    static const Str hdr[] = {S_(":status"), S_("200"),         S_("x-foo"),
                              S_("blah"),    S_("x-bar"),       S_("more"),
                              S_("trailer"), S_("some-trailer")};
    size_t nhdr = p->trailers != H2CT_NO_HEADER ? 8 : 6;
    bool end_stream = !p->with_data && p->trailers == H2CT_NO_HEADER;
    H2CT_TRY(h2ct_write_headers_mode(
        tc, p->res_header, h2ct_hfp(id, h2ct_block(tc, hdr, nhdr), end_stream)));
    if (p->with_data)
        H2CT_TRY(h2ct_write_data(tc, id, p->trailers == H2CT_NO_HEADER, res_body));
    static const Str trailer[] = {S_("some-trailer"), S_("some-value")};
    H2CT_TRY(h2ct_write_headers_mode(tc, p->trailers,
                                     h2ct_hfp(id, h2ct_block(tc, trailer, 2), true)));

    H2CT_TRY(h2ct_rt_want_status(rt, 200));
    H2CT_TRY(h2ct_rt_want_body(rt, p->with_data ? res_body : ""));
    (void)h2ct_rt_want_trailers(
        rt, p->trailers == H2CT_NO_HEADER ? "" : "Some-Trailer: some-value\r\n");
}

/* Test all 36 combinations of response frame orders: (3 ways of
 * 100-continue) * (2 ways of headers) * (2 ways of data) * (3 ways of
 * trailers). */
static void h2ct_res_pattern_test(TestingT *t, H2ctHeaderType expect100_continue,
                                  H2ctHeaderType res_header, bool with_data,
                                  H2ctHeaderType trailers) {
    H2ctPattern p;
    p.expect100_continue = expect100_continue;
    p.res_header = res_header;
    p.trailers = trailers;
    p.with_data = with_data;
    h2ct_run(t, h2ct_res_pattern, &p);
}

#define F0 H2CT_NO_HEADER
#define F1 H2CT_ONE_HEADER
#define F2 H2CT_SPLIT_HEADER
#define D0 false
#define D1 true
/* clang-format off */
static void TestTransportResPattern_c0h1d0t0(TestingT *t) { h2ct_res_pattern_test(t, F0, F1, D0, F0); }
static void TestTransportResPattern_c0h1d0t1(TestingT *t) { h2ct_res_pattern_test(t, F0, F1, D0, F1); }
static void TestTransportResPattern_c0h1d0t2(TestingT *t) { h2ct_res_pattern_test(t, F0, F1, D0, F2); }
static void TestTransportResPattern_c0h1d1t0(TestingT *t) { h2ct_res_pattern_test(t, F0, F1, D1, F0); }
static void TestTransportResPattern_c0h1d1t1(TestingT *t) { h2ct_res_pattern_test(t, F0, F1, D1, F1); }
static void TestTransportResPattern_c0h1d1t2(TestingT *t) { h2ct_res_pattern_test(t, F0, F1, D1, F2); }
static void TestTransportResPattern_c0h2d0t0(TestingT *t) { h2ct_res_pattern_test(t, F0, F2, D0, F0); }
static void TestTransportResPattern_c0h2d0t1(TestingT *t) { h2ct_res_pattern_test(t, F0, F2, D0, F1); }
static void TestTransportResPattern_c0h2d0t2(TestingT *t) { h2ct_res_pattern_test(t, F0, F2, D0, F2); }
static void TestTransportResPattern_c0h2d1t0(TestingT *t) { h2ct_res_pattern_test(t, F0, F2, D1, F0); }
static void TestTransportResPattern_c0h2d1t1(TestingT *t) { h2ct_res_pattern_test(t, F0, F2, D1, F1); }
static void TestTransportResPattern_c0h2d1t2(TestingT *t) { h2ct_res_pattern_test(t, F0, F2, D1, F2); }
static void TestTransportResPattern_c1h1d0t0(TestingT *t) { h2ct_res_pattern_test(t, F1, F1, D0, F0); }
static void TestTransportResPattern_c1h1d0t1(TestingT *t) { h2ct_res_pattern_test(t, F1, F1, D0, F1); }
static void TestTransportResPattern_c1h1d0t2(TestingT *t) { h2ct_res_pattern_test(t, F1, F1, D0, F2); }
static void TestTransportResPattern_c1h1d1t0(TestingT *t) { h2ct_res_pattern_test(t, F1, F1, D1, F0); }
static void TestTransportResPattern_c1h1d1t1(TestingT *t) { h2ct_res_pattern_test(t, F1, F1, D1, F1); }
static void TestTransportResPattern_c1h1d1t2(TestingT *t) { h2ct_res_pattern_test(t, F1, F1, D1, F2); }
static void TestTransportResPattern_c1h2d0t0(TestingT *t) { h2ct_res_pattern_test(t, F1, F2, D0, F0); }
static void TestTransportResPattern_c1h2d0t1(TestingT *t) { h2ct_res_pattern_test(t, F1, F2, D0, F1); }
static void TestTransportResPattern_c1h2d0t2(TestingT *t) { h2ct_res_pattern_test(t, F1, F2, D0, F2); }
static void TestTransportResPattern_c1h2d1t0(TestingT *t) { h2ct_res_pattern_test(t, F1, F2, D1, F0); }
static void TestTransportResPattern_c1h2d1t1(TestingT *t) { h2ct_res_pattern_test(t, F1, F2, D1, F1); }
static void TestTransportResPattern_c1h2d1t2(TestingT *t) { h2ct_res_pattern_test(t, F1, F2, D1, F2); }
static void TestTransportResPattern_c2h1d0t0(TestingT *t) { h2ct_res_pattern_test(t, F2, F1, D0, F0); }
static void TestTransportResPattern_c2h1d0t1(TestingT *t) { h2ct_res_pattern_test(t, F2, F1, D0, F1); }
static void TestTransportResPattern_c2h1d0t2(TestingT *t) { h2ct_res_pattern_test(t, F2, F1, D0, F2); }
static void TestTransportResPattern_c2h1d1t0(TestingT *t) { h2ct_res_pattern_test(t, F2, F1, D1, F0); }
static void TestTransportResPattern_c2h1d1t1(TestingT *t) { h2ct_res_pattern_test(t, F2, F1, D1, F1); }
static void TestTransportResPattern_c2h1d1t2(TestingT *t) { h2ct_res_pattern_test(t, F2, F1, D1, F2); }
static void TestTransportResPattern_c2h2d0t0(TestingT *t) { h2ct_res_pattern_test(t, F2, F2, D0, F0); }
static void TestTransportResPattern_c2h2d0t1(TestingT *t) { h2ct_res_pattern_test(t, F2, F2, D0, F1); }
static void TestTransportResPattern_c2h2d0t2(TestingT *t) { h2ct_res_pattern_test(t, F2, F2, D0, F2); }
static void TestTransportResPattern_c2h2d1t0(TestingT *t) { h2ct_res_pattern_test(t, F2, F2, D1, F0); }
static void TestTransportResPattern_c2h2d1t1(TestingT *t) { h2ct_res_pattern_test(t, F2, F2, D1, F1); }
static void TestTransportResPattern_c2h2d1t2(TestingT *t) { h2ct_res_pattern_test(t, F2, F2, D1, F2); }
/* clang-format on */
#undef F0
#undef F1
#undef F2
#undef D0
#undef D1

/* What the got1xx hook saw. Go prints the header with %v, as a map; this has
 * it as HTTP/1 writes it. */
static Error h2ct_got1xx(void *env, Int code, TextprotoMIMEHeader header) {
    BytesBuffer *buf = (BytesBuffer *)env;
    IoWriter w = bytes_buffer_as_io_writer(buf);
    fmt_fprintf_v(w, "code=%d header=", code);
    (void)http_header_write(header, w);
    return BURROW_NO_ERROR;
}

/* Issue 26189, Issue 17739: ignore unknown 1xx responses. */
static void h2ct_unknown_1xx(H2ctTT *tt, const void *arg) {
    (void)arg;
    Alloc *a = arena_allocator(&tt->ar);
    BytesBuffer *buf = (BytesBuffer *)mem_alloc(a, sizeof *buf, _Alignof(BytesBuffer));
    HttptraceClientTrace *trace = (HttptraceClientTrace *)mem_alloc(
        a, sizeof *trace, _Alignof(HttptraceClientTrace));
    if (buf == NULL || trace == NULL) {
        testing_t_errorf_v(tt->t, "no memory for the trace");
        return;
    }
    *buf = BYTES_BUFFER(a);
    trace->got1xx_response = BURROW_FN(HttptraceGot1xxResponseFunc, h2ct_got1xx, buf);
    Context ctx = httptrace_with_client_trace(a, context_background(), trace);

    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, ctx, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);

    for (int i = 110; i <= 114; i++) {
        Str code = fmt_sprintf_v(a, "%d", i);
        Str kv[] = {BURROW_S(":status"), code, BURROW_S("foo-bar"), code};
        H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, kv, 4), false)));
    }
    static const Str status204[] = {S_(":status"), S_("204")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, status204, 2), true)));

    HttpResponse *res = h2ct_rt_response(rt);
    H2CT_TRY(res != NULL);
    if (res->status_code != 204)
        FATALF("status code = %d; want 204", (int)res->status_code);
    static const char want[] = "code=110 header=Foo-Bar: 110\r\n"
                               "code=111 header=Foo-Bar: 111\r\n"
                               "code=112 header=Foo-Bar: 112\r\n"
                               "code=113 header=Foo-Bar: 113\r\n"
                               "code=114 header=Foo-Bar: 114\r\n";
    Slice got = bytes_buffer_bytes(buf);
    Str g = str_from_bytes(got.p, got.len);
    if (!str_eq(g, str_from_cstr(want)))
        testing_t_errorf_v(tt->t, "Got trace:\n%s\nWant:\n%s", g, str_from_cstr(want));
}

static void TestTransportUnknown1xx(TestingT *t) {
    h2ct_run(t, h2ct_unknown_1xx, NULL);
}

static void h2ct_receive_undeclared_trailer(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);

    static const Str status[] = {S_(":status"), S_("200")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, status, 2), false)));
    static const Str trailer[] = {S_("some-trailer"), S_("I'm an undeclared Trailer!")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, trailer, 2), true)));

    H2CT_TRY(h2ct_rt_want_status(rt, 200));
    H2CT_TRY(h2ct_rt_want_body(rt, ""));
    (void)h2ct_rt_want_trailers(rt, "Some-Trailer: I'm an undeclared Trailer!\r\n");
}

static void TestTransportReceiveUndeclaredTrailer(TestingT *t) {
    h2ct_run(t, h2ct_receive_undeclared_trailer, NULL);
}

typedef enum H2ctTrailerErr {
    H2CT_PSEUDO_HEADER_ERROR,
    H2CT_HEADER_FIELD_NAME_ERROR,
    H2CT_HEADER_FIELD_VALUE_ERROR,
} H2ctTrailerErr;

typedef struct H2ctInvalidTrailer {
    H2ctHeaderType mode;
    H2ctTrailerErr kind;
    Str name; /* the one the error is about */
    const Str *trailers;
    size_t n;
} H2ctInvalidTrailer;

static void h2ct_invalid_trailer(H2ctTT *tt, const void *arg) {
    const H2ctInvalidTrailer *p = (const H2ctInvalidTrailer *)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);

    static const Str hdr[] = {S_(":status"), S_("200"), S_("trailer"), S_("declared")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, hdr, 4), false)));
    H2CT_TRY(h2ct_write_headers_mode(
        tc, p->mode, h2ct_hfp(id, h2ct_block(tc, p->trailers, p->n), true)));

    H2CT_TRY(h2ct_rt_want_status(rt, 200));
    Error err = BURROW_NO_ERROR;
    Slice body = h2ct_rt_read_body(rt, &err);
    Error want_err = BURROW_NO_ERROR;
    switch (p->kind) {
    case H2CT_PSEUDO_HEADER_ERROR:
        want_err = burrow__http2_pseudo_header_error(p->name);
        break;
    case H2CT_HEADER_FIELD_NAME_ERROR:
        want_err = burrow__http2_header_field_name_error(p->name);
        break;
    case H2CT_HEADER_FIELD_VALUE_ERROR:
        want_err = burrow__http2_header_field_value_error(p->name);
        break;
    default:
        break;
    }
    Http2StreamError se;
    memset(&se, 0, sizeof se);
    bool ok = burrow__http2_error_stream(err, &se) && BURROW_FAILED(se.cause) &&
              str_eq(error_text(se.cause), error_text(want_err));
    if (!ok)
        FATALF("res.Body ReadAll error = %q, %v; want StreamError with cause %v",
               str_from_bytes(body.p, body.len), err, want_err);
    if (body.len > 0)
        FATALF("body = %q; want nothing", str_from_bytes(body.p, body.len));
}

static void h2ct_invalid_trailer_test(TestingT *t, H2ctHeaderType mode,
                                      H2ctTrailerErr kind, Str name,
                                      const Str *trailers, size_t n) {
    H2ctInvalidTrailer p;
    p.mode = mode;
    p.kind = kind;
    p.name = name;
    p.trailers = trailers;
    p.n = n;
    h2ct_run(t, h2ct_invalid_trailer, &p);
}

static const Str h2ct_pseudo_trailer[] = {S_(":colon"), S_("foo"), S_("foo"),
                                          S_("bar")};

static void TestTransportInvalidTrailer_Pseudo1(TestingT *t) {
    h2ct_invalid_trailer_test(t, H2CT_ONE_HEADER, H2CT_PSEUDO_HEADER_ERROR,
                              BURROW_S(":colon"), h2ct_pseudo_trailer, 4);
}

static void TestTransportInvalidTrailer_Pseudo2(TestingT *t) {
    h2ct_invalid_trailer_test(t, H2CT_SPLIT_HEADER, H2CT_PSEUDO_HEADER_ERROR,
                              BURROW_S(":colon"), h2ct_pseudo_trailer, 4);
}

static const Str h2ct_capital_trailer[] = {S_("foo"), S_("bar"), S_("Capital"),
                                           S_("bad")};

static void TestTransportInvalidTrailer_Capital1(TestingT *t) {
    h2ct_invalid_trailer_test(t, H2CT_ONE_HEADER, H2CT_HEADER_FIELD_NAME_ERROR,
                              BURROW_S("Capital"), h2ct_capital_trailer, 4);
}

static void TestTransportInvalidTrailer_Capital2(TestingT *t) {
    h2ct_invalid_trailer_test(t, H2CT_SPLIT_HEADER, H2CT_HEADER_FIELD_NAME_ERROR,
                              BURROW_S("Capital"), h2ct_capital_trailer, 4);
}

static void TestTransportInvalidTrailer_EmptyFieldName(TestingT *t) {
    static const Str trailer[] = {S_(""), S_("bad")};
    h2ct_invalid_trailer_test(t, H2CT_ONE_HEADER, H2CT_HEADER_FIELD_NAME_ERROR,
                              BURROW_S(""), trailer, 2);
}

static void TestTransportInvalidTrailer_BinaryFieldValue(TestingT *t) {
    static const Str trailer[] = {S_("x"), S_("has\nnewline")};
    h2ct_invalid_trailer_test(t, H2CT_ONE_HEADER, H2CT_HEADER_FIELD_VALUE_ERROR,
                              BURROW_S("x"), trailer, 2);
}

/* The start of a connection a retry makes: the client's SETTINGS and
 * WINDOW_UPDATE, and the HEADERS of a GET on stream 1. */
static bool h2ct_want_new_conn_get(H2ctConn *tc) {
    return h2ct_want_frame_type(tc, HTTP2_FRAME_SETTINGS) &&
           h2ct_want_frame_type(tc, HTTP2_FRAME_WINDOW_UPDATE) &&
           h2ct_want_headers(tc, 1, true, NULL, 0);
}

static bool h2ct_write_status(H2ctConn *tc, uint32_t id, const char *status) {
    Str kv[] = {BURROW_S(":status"), str_from_cstr(status)};
    return h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, kv, 2), true));
}

static void h2ct_retry_after_go_away_no_retry(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctRT *rt = h2ct_tt_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);

    /* First attempt: Server sends a GOAWAY with an error and a MaxStreamID
     * less than the request ID. This probably indicates that there was
     * something wrong with our request, so we don't retry it. */
    H2ctConn *tc = h2ct_get_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_want_new_conn_get(tc));
    H2CT_TRY(h2ct_write_settings(tc, NULL, 0));
    H2CT_TRY(h2ct_write_go_away(tc, 0, HTTP2_ERR_CODE_INTERNAL));
    H2CT_TRY(h2ct_rt_result(rt));
    if (BURROW_OK(rt->err))
        testing_t_errorf_v(tt->t, "after GOAWAY, RoundTrip is not done, want error");
}

static void TestTransportRetryAfterGOAWAYNoRetry(TestingT *t) {
    h2ct_run(t, h2ct_retry_after_go_away_no_retry, NULL);
}

static void h2ct_retry_after_go_away_retry(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctRT *rt = h2ct_tt_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);

    /* First attempt: Server sends a GOAWAY with ErrCodeNo and a MaxStreamID
     * less than the request ID. We take the server at its word that nothing
     * has really gone wrong, and retry the request. */
    H2ctConn *tc = h2ct_get_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_want_new_conn_get(tc));
    H2CT_TRY(h2ct_write_settings(tc, NULL, 0));
    H2CT_TRY(h2ct_write_go_away(tc, 0, HTTP2_ERR_CODE_NO));
    if (h2ct_rt_done(rt))
        FATALF("after GOAWAY, RoundTrip is done; want it to be retrying");

    /* Second attempt succeeds on a new connection. */
    tc = h2ct_get_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_want_new_conn_get(tc));
    H2CT_TRY(h2ct_write_settings(tc, NULL, 0));
    H2CT_TRY(h2ct_write_status(tc, 1, "200"));
    (void)h2ct_rt_want_status(rt, 200);
}

static void TestTransportRetryAfterGOAWAYRetry(TestingT *t) {
    h2ct_run(t, h2ct_retry_after_go_away_retry, NULL);
}

static void h2ct_retry_after_go_away_second_request(H2ctTT *tt, const void *arg) {
    (void)arg;
    /* First request succeeds. */
    H2ctRT *rt1 = h2ct_tt_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt1 != NULL);
    H2ctConn *tc = h2ct_get_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_want_new_conn_get(tc));
    H2CT_TRY(h2ct_write_settings(tc, NULL, 0));
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_SETTINGS)); /* Settings ACK */
    H2CT_TRY(h2ct_write_status(tc, 1, "200"));
    H2CT_TRY(h2ct_rt_want_status(rt1, 200));

    /* Second request: Server sends a GOAWAY with a MaxStreamID less than the
     * request ID. The server says it didn't see this request, so we retry it
     * on a new connection. */
    H2ctRT *rt2 = h2ct_tt_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt2 != NULL);

    /* Second request, first attempt. */
    H2CT_TRY(h2ct_want_headers(tc, 3, true, NULL, 0));
    H2CT_TRY(h2ct_write_settings(tc, NULL, 0));
    H2CT_TRY(h2ct_write_go_away(tc, 1, HTTP2_ERR_CODE_PROTOCOL));
    if (h2ct_rt_done(rt2))
        FATALF("after GOAWAY, RoundTrip is done; want it to be retrying");

    /* Second request, second attempt. */
    tc = h2ct_get_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_want_new_conn_get(tc));
    H2CT_TRY(h2ct_write_settings(tc, NULL, 0));
    H2CT_TRY(h2ct_write_status(tc, 1, "200"));
    (void)h2ct_rt_want_status(rt2, 200);
}

static void TestTransportRetryAfterGOAWAYSecondRequest(TestingT *t) {
    h2ct_run(t, h2ct_retry_after_go_away_second_request, NULL);
}

static void h2ct_retry_after_refused_stream(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctRT *rt = h2ct_tt_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);

    /* First attempt: Server sends a RST_STREAM. */
    H2ctConn *tc = h2ct_get_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_want_new_conn_get(tc));
    H2CT_TRY(h2ct_write_settings(tc, NULL, 0));
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_SETTINGS)); /* settings ACK */
    H2CT_TRY(h2ct_write_rst_stream(tc, 1, HTTP2_ERR_CODE_REFUSED_STREAM));
    if (h2ct_rt_done(rt))
        FATALF("after RST_STREAM, RoundTrip is done; want it to be retrying");

    /* Second attempt succeeds on the same connection. */
    H2CT_TRY(h2ct_want_headers(tc, 3, true, NULL, 0));
    H2CT_TRY(h2ct_write_settings(tc, NULL, 0));
    H2CT_TRY(h2ct_write_status(tc, 3, "204"));
    (void)h2ct_rt_want_status(rt, 204);
}

static void TestTransportRetryAfterRefusedStream(TestingT *t) {
    h2ct_run(t, h2ct_retry_after_refused_stream, NULL);
}

#define TESTS(X)                                                                       \
    X(TestTestClientConn)                                                              \
    X(TestTransportResPattern_c0h1d0t0)                                                \
    X(TestTransportResPattern_c0h1d0t1)                                                \
    X(TestTransportResPattern_c0h1d0t2)                                                \
    X(TestTransportResPattern_c0h1d1t0)                                                \
    X(TestTransportResPattern_c0h1d1t1)                                                \
    X(TestTransportResPattern_c0h1d1t2)                                                \
    X(TestTransportResPattern_c0h2d0t0)                                                \
    X(TestTransportResPattern_c0h2d0t1)                                                \
    X(TestTransportResPattern_c0h2d0t2)                                                \
    X(TestTransportResPattern_c0h2d1t0)                                                \
    X(TestTransportResPattern_c0h2d1t1)                                                \
    X(TestTransportResPattern_c0h2d1t2)                                                \
    X(TestTransportResPattern_c1h1d0t0)                                                \
    X(TestTransportResPattern_c1h1d0t1)                                                \
    X(TestTransportResPattern_c1h1d0t2)                                                \
    X(TestTransportResPattern_c1h1d1t0)                                                \
    X(TestTransportResPattern_c1h1d1t1)                                                \
    X(TestTransportResPattern_c1h1d1t2)                                                \
    X(TestTransportResPattern_c1h2d0t0)                                                \
    X(TestTransportResPattern_c1h2d0t1)                                                \
    X(TestTransportResPattern_c1h2d0t2)                                                \
    X(TestTransportResPattern_c1h2d1t0)                                                \
    X(TestTransportResPattern_c1h2d1t1)                                                \
    X(TestTransportResPattern_c1h2d1t2)                                                \
    X(TestTransportResPattern_c2h1d0t0)                                                \
    X(TestTransportResPattern_c2h1d0t1)                                                \
    X(TestTransportResPattern_c2h1d0t2)                                                \
    X(TestTransportResPattern_c2h1d1t0)                                                \
    X(TestTransportResPattern_c2h1d1t1)                                                \
    X(TestTransportResPattern_c2h1d1t2)                                                \
    X(TestTransportResPattern_c2h2d0t0)                                                \
    X(TestTransportResPattern_c2h2d0t1)                                                \
    X(TestTransportResPattern_c2h2d0t2)                                                \
    X(TestTransportResPattern_c2h2d1t0)                                                \
    X(TestTransportResPattern_c2h2d1t1)                                                \
    X(TestTransportResPattern_c2h2d1t2)                                                \
    X(TestTransportUnknown1xx)                                                         \
    X(TestTransportReceiveUndeclaredTrailer)                                           \
    X(TestTransportInvalidTrailer_Pseudo1)                                             \
    X(TestTransportInvalidTrailer_Pseudo2)                                             \
    X(TestTransportInvalidTrailer_Capital1)                                            \
    X(TestTransportInvalidTrailer_Capital2)                                            \
    X(TestTransportInvalidTrailer_EmptyFieldName)                                      \
    X(TestTransportInvalidTrailer_BinaryFieldValue)                                    \
    X(TestTransportRetryAfterGOAWAYNoRetry)                                            \
    X(TestTransportRetryAfterGOAWAYRetry)                                              \
    X(TestTransportRetryAfterGOAWAYSecondRequest)                                      \
    X(TestTransportRetryAfterRefusedStream)

TESTING_MAIN(TESTS)
