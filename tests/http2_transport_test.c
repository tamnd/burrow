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
 * the connections speak HTTP/2 with prior knowledge. Where Go moves a fake
 * clock along by a few seconds, these wait that long for real. Go's tests that
 * move it along by a minute or more, or look inside the client's state, are not
 * here.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http2.h"
#include "../src/xnet/hpack.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/context.h"
#include "burrow/encoding/hex.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/math/rand.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/http/httptrace.h"
#include "burrow/netpoll.h"
#include "burrow/os.h"
#include "burrow/sort.h"
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
    BytesBuffer late; /* what the client wrote after closing cli, before reof */
    Time reof_at;
    SyncWaitGroup rwg;
    /* What the test writes, which out_job passes on to srv. */
    SyncMutex wmu;
    SyncCond wcond;
    BytesBuffer wbuf;
    SyncWaitGroup wwg;
    NetConn cli;    /* the client's end, which the client has as h2ct_cli_vt */
    Error cli_werr; /* under rmu: what the client's writes fail with, if set */
    Int rlimit;     /* under rmu: SetReadBufferSize, the most rbuf holds, or 0 */
    struct H2ctConn *next;
    SyncAtomicBool cli_closed; /* the client closed cli */
    bool reof;
    bool wclosed;
    bool wshut; /* closeWrite: srv closes once wbuf is out */
    bool wfailed;
    bool claimed; /* given to the test by get_conn */
} H2ctConn;

enum { H2CT_CHUNK = 16 << 10 };

/* How much in_job may read next: what rlimit leaves room for in rbuf, waiting
 * for the test to make some. Once the conn is closed at either end, it reads
 * on regardless, to see the end. */
static Int h2ct_in_room(H2ctConn *tc) {
    for (;;) {
        sync_mutex_lock(&tc->rmu);
        Int room =
            tc->rlimit == 0 ? H2CT_CHUNK : tc->rlimit - bytes_buffer_len(&tc->rbuf);
        sync_mutex_unlock(&tc->rmu);
        if (room > 0)
            return room < H2CT_CHUNK ? room : H2CT_CHUNK;
        sync_mutex_lock(&tc->wmu);
        bool closed = tc->wclosed || sync_atomic_bool_load(&tc->cli_closed);
        sync_mutex_unlock(&tc->wmu);
        if (closed)
            return H2CT_CHUNK;
        time_sleep(TIME_MILLISECOND);
    }
}

/* Reads what the client writes into rbuf until srv ends. */
static void h2ct_in_job(void *env) {
    H2ctConn *tc = (H2ctConn *)env;
    Byte *chunk = (Byte *)mem_alloc_nozero(tc->a, H2CT_CHUNK, 1);
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Int n = 0;
        if (chunk != NULL) {
            Int max = h2ct_in_room(tc);
            n = tc->srv.vt->reader.read(tc->srv.data,
                                        slice_from(chunk, max, max, TYPE_BYTE), &err);
        }
        sync_mutex_lock(&tc->rmu);
        if (n > 0) {
            Error werr = BURROW_NO_ERROR;
            (void)bytes_buffer_write(&tc->rbuf, slice_from(chunk, n, n, TYPE_BYTE),
                                     &werr);
            if (BURROW_FAILED(werr))
                err = werr;
        }
        bool done = chunk == NULL || BURROW_FAILED(err);
        if (done) {
            Error lerr = BURROW_NO_ERROR;
            (void)bytes_buffer_write_to(&tc->late, bytes_buffer_as_io_writer(&tc->rbuf),
                                        &lerr);
            tc->reof = true;
            tc->reof_at = time_now();
        }
        sync_mutex_unlock(&tc->rmu);
        if (done)
            break;
    }
    if (chunk != NULL)
        mem_free(tc->a, chunk, H2CT_CHUNK, 1);
}

/* What the framer reads: rbuf, waiting for it until rdeadline, and the end of
 * it once srv has ended and it is empty. Go waits for its bubble to go quiet
 * before it reads, by when the client has written all it will, even after
 * closing the conn. This gives the client a moment after the end first. */
static Int h2ct_in_read(void *self, Slice p, Error *err) {
    H2ctConn *tc = (H2ctConn *)self;
    for (;;) {
        sync_mutex_lock(&tc->rmu);
        if (bytes_buffer_len(&tc->rbuf) > 0) {
            Int n = bytes_buffer_read(&tc->rbuf, p, err);
            sync_mutex_unlock(&tc->rmu);
            return n;
        }
        bool eof =
            tc->reof && !time_before(time_now(), time_add(tc->reof_at, H2CT_QUIET));
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
    if (tc->wfailed || tc->wclosed || tc->wshut) {
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
    bool shut = false;
    sync_mutex_lock(&tc->wmu);
    if (chunk == NULL)
        tc->wfailed = true;
    while (!tc->wfailed) {
        while (bytes_buffer_len(&tc->wbuf) == 0 && !tc->wclosed && !tc->wshut)
            sync_cond_wait(&tc->wcond);
        if (tc->wclosed)
            break;
        if (bytes_buffer_len(&tc->wbuf) == 0) {
            shut = true;
            break;
        }
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
    if (shut)
        (void)tc->srv.vt->closer.close(tc->srv.data);
    if (chunk != NULL)
        mem_free(tc->a, chunk, H2CT_CHUNK, 1);
}

/* closeWrite: once what the test wrote is out, the client reads the end of
 * the connection. */
static void h2ct_close_write(H2ctConn *tc) {
    sync_mutex_lock(&tc->wmu);
    tc->wshut = true;
    sync_cond_signal(&tc->wcond);
    sync_mutex_unlock(&tc->wmu);
}

/* Hangs up on the client at once. What the test wrote and the client has not
 * read yet goes. */
static void h2ct_conn_close(H2ctConn *tc) {
    sync_mutex_lock(&tc->wmu);
    tc->wclosed = true;
    sync_cond_signal(&tc->wcond);
    sync_mutex_unlock(&tc->wmu);
    (void)tc->srv.vt->closer.close(tc->srv.data);
}

/* The client's end of the pipe, as the client has it: cli, with its close
 * noted for isClosed. */
static Int h2ct_cli_read(void *self, Slice p, Error *err) {
    H2ctConn *tc = (H2ctConn *)self;
    return tc->cli.vt->reader.read(tc->cli.data, p, err);
}

static Int h2ct_cli_write(void *self, Slice p, Error *err) {
    H2ctConn *tc = (H2ctConn *)self;
    sync_mutex_lock(&tc->rmu);
    Error werr = tc->cli_werr;
    if (BURROW_OK(werr) && sync_atomic_bool_load(&tc->cli_closed)) {
        /* Go's fake conn takes writes after the client closes it: the close
         * ends what the test may write, and gives the test the end once it
         * has read the rest. The pipe is closed, so these go round it. */
        Int n = bytes_buffer_write(tc->reof ? &tc->rbuf : &tc->late, p, err);
        sync_mutex_unlock(&tc->rmu);
        return n;
    }
    sync_mutex_unlock(&tc->rmu);
    if (BURROW_FAILED(werr)) {
        *err = werr;
        return 0;
    }
    return tc->cli.vt->writer.write(tc->cli.data, p, err);
}

static Error h2ct_cli_close(void *self) {
    H2ctConn *tc = (H2ctConn *)self;
    sync_atomic_bool_store(&tc->cli_closed, true);
    return tc->cli.vt->closer.close(tc->cli.data);
}

static NetAddr h2ct_cli_local_addr(void *self) {
    H2ctConn *tc = (H2ctConn *)self;
    return tc->cli.vt->local_addr(tc->cli.data);
}

static NetAddr h2ct_cli_remote_addr(void *self) {
    H2ctConn *tc = (H2ctConn *)self;
    return tc->cli.vt->remote_addr(tc->cli.data);
}

static Error h2ct_cli_set_deadline(void *self, Time t) {
    H2ctConn *tc = (H2ctConn *)self;
    return tc->cli.vt->set_deadline(tc->cli.data, t);
}

static Error h2ct_cli_set_read_deadline(void *self, Time t) {
    H2ctConn *tc = (H2ctConn *)self;
    return tc->cli.vt->set_read_deadline(tc->cli.data, t);
}

static Error h2ct_cli_set_write_deadline(void *self, Time t) {
    H2ctConn *tc = (H2ctConn *)self;
    return tc->cli.vt->set_write_deadline(tc->cli.data, t);
}

static const NetConnVT h2ct_cli_vt = {
    {NULL, h2ct_cli_read},      {NULL, h2ct_cli_write},      {NULL, h2ct_cli_close},
    h2ct_cli_local_addr,        h2ct_cli_remote_addr,        h2ct_cli_set_deadline,
    h2ct_cli_set_read_deadline, h2ct_cli_set_write_deadline,
};

/* isClosed: whether the client has closed its end, given as long as a check
 * waits for it. */
static bool h2ct_is_closed(H2ctConn *tc) {
    Time deadline = time_add(time_now(), H2CT_WAIT);
    while (!sync_atomic_bool_load(&tc->cli_closed)) {
        if (!time_before(time_now(), deadline))
            return false;
        time_sleep(TIME_MILLISECOND);
    }
    return true;
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
    bytes_buffer_free(&tc->late);
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
    Http2ClientConn *cc; /* new_client_conn's, which tc_round_trip uses */
    SyncMutex mu;
    H2ctConn *conns;
    H2ctConn **conns_tail;
    H2ctRT *rts;
    H2ctBody *bodies;
    SyncWaitGroup rtwg;
    Int dials_waiting;  /* under mu: dials waiting on dial_held */
    SyncCond dial_cond; /* on mu, for dial_held */
    bool dial_held;     /* dials wait until it is false */
};

static H2ctConn *h2ct_conn_new(H2ctTT *tt, NetConn cli, NetConn srv) {
    Alloc *a = tt->a;
    H2ctConn *tc = (H2ctConn *)mem_alloc(a, sizeof *tc, _Alignof(H2ctConn));
    if (tc == NULL)
        return NULL;
    tc->t = tt->t;
    tc->a = a;
    tc->cli = cli;
    tc->srv = srv;
    tc->hbuf = BYTES_BUFFER(a);
    tc->rbuf = BYTES_BUFFER(a);
    tc->late = BYTES_BUFFER(a);
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
    sync_mutex_lock(&tt->mu);
    tt->dials_waiting++;
    while (tt->dial_held)
        sync_cond_wait(&tt->dial_cond);
    tt->dials_waiting--;
    sync_mutex_unlock(&tt->mu);
    NetConn cli;
    NetConn srv;
    net_pipe(tt->a, &cli, &srv);
    if (cli.data == NULL) {
        *err = burrow_err_out_of_memory;
        return cli;
    }
    H2ctConn *tc = h2ct_conn_new(tt, cli, srv);
    if (tc == NULL) {
        net_pipe_free(srv);
        *err = burrow_err_out_of_memory;
        return (NetConn){NULL, NULL};
    }
    *err = BURROW_NO_ERROR;
    return (NetConn){&h2ct_cli_vt, tc};
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
    tt->dial_cond = SYNC_COND(sync_mutex_locker(&tt->mu));
}

/* Lets the dials waiting for it go on. */
static void h2ct_tt_release_dials(H2ctTT *tt) {
    sync_mutex_lock(&tt->mu);
    tt->dial_held = false;
    sync_cond_broadcast(&tt->dial_cond);
    sync_mutex_unlock(&tt->mu);
}

static void h2ct_body_close_with_error(H2ctBody *b, Error err);

/* The cleanups Go's test helpers register. Every round trip still going is
 * cancelled and every connection hung up on, so they end, and then it all
 * goes. */
static void h2ct_tt_close(H2ctTT *tt) {
    h2ct_tt_release_dials(tt);
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
    burrow__http2_client_conn_release(tt->cc);
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
 * h2ct_tc_round_trip use without going through tr1 or t2's pool. */
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
    H2ctConn *tc = h2ct_conn_new(tt, cli, srv);
    if (tc == NULL) {
        net_pipe_free(srv);
        testing_t_errorf_v(tt->t, "no memory for the test's conn");
        return NULL;
    }
    NetConn c = {&h2ct_cli_vt, tc};
    Error err = burrow__http2_transport_add_conn(tt->t2, BURROW_S("http"),
                                                 BURROW_S("dummy.tld"), c);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(tt->t, "newClientConn: %v", err);
        return NULL;
    }
    tt->cc = burrow__http2_transport_client_conn(tt->t2, c);
    if (tt->cc == NULL) {
        testing_t_errorf_v(tt->t, "newClientConn: the transport has no conn");
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

/* Write: data, which reads give before anything else. */
static void h2ct_body_write(H2ctBody *b, Str data) {
    sync_mutex_lock(&b->mu);
    Error err = BURROW_NO_ERROR;
    (void)bytes_buffer_write_string(&b->buf, data, &err);
    if (BURROW_FAILED(err) && BURROW_OK(b->err))
        b->err = err;
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

static HttpRequest *h2ct_new_request_url(H2ctTT *tt, Context ctx, Str method, Str url,
                                         IoReader body) {
    Error err = BURROW_NO_ERROR;
    if (ctx.vt == NULL)
        ctx = context_background();
    HttpRequest *req =
        http_new_request_with_context(tt->a, ctx, method, url, body, &err);
    if (req == NULL)
        testing_t_errorf_v(tt->t, "NewRequest: %v", err);
    return req;
}

static HttpRequest *h2ct_new_request(H2ctTT *tt, Context ctx, Str method,
                                     IoReader body) {
    return h2ct_new_request_url(tt, ctx, method, BURROW_S("http://dummy.tld/"), body);
}

static void h2ct_rt_job(void *env) {
    H2ctRT *rt = (H2ctRT *)env;
    Error err = BURROW_NO_ERROR;
    HttpResponse *res;
    if (rt->via_t2)
        res = burrow__http2_client_conn_round_trip(rt->tt->cc, rt->req, &err);
    else
        res = http_transport_round_trip(&rt->tt->tr1, rt->req, &err);
    rt->res = res;
    rt->err = error_retain(arena_allocator(&rt->arena), err);
    sync_atomic_bool_store(&rt->done, true);
}

/* roundTrip: starts req on its way, with a context of its own that the test
 * can cancel and that tells it the request's stream ID. The test transport
 * has req from then on. via_t2 is tc.roundTrip, on new_client_conn's
 * connection, and the rest tt.roundTrip. */
static H2ctRT *h2ct_round_trip_via(H2ctTT *tt, HttpRequest *req, bool via_t2) {
    if (req == NULL)
        return NULL;
    if (via_t2 && tt->cc == NULL) {
        http_request_free(req);
        testing_t_errorf_v(tt->t, "tc.roundTrip with no newTestClientConn");
        return NULL;
    }
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

/* err: what RoundTrip returned as its error, once it has. */
static Error h2ct_rt_err(H2ctRT *rt) {
    if (!h2ct_rt_result(rt))
        return BURROW_NO_ERROR;
    return rt->err;
}

/* Whether err is a StreamError on stream id with code, and, when cause is not
 * NULL, a cause that reads the same as it. */
static bool h2ct_is_stream_error(Error err, uint32_t id, Http2ErrCode code,
                                 const Error *cause) {
    Http2StreamError se;
    memset(&se, 0, sizeof se);
    if (!burrow__http2_error_stream(err, &se))
        return false;
    if (id != 0 && se.stream_id != id)
        return false;
    if (se.code != code)
        return false;
    return cause == NULL || (BURROW_FAILED(se.cause) &&
                             str_eq(error_text(se.cause), error_text(*cause)));
}

/* Waits for RoundTrip to return, for as long as a check would, and says
 * whether it has. done in Go, where the bubble has already settled. */
static bool h2ct_rt_wait_done(H2ctRT *rt) {
    Time deadline = time_add(time_now(), H2CT_WAIT);
    while (!sync_atomic_bool_load(&rt->done)) {
        if (!time_before(time_now(), deadline))
            return false;
        time_sleep(TIME_MILLISECOND);
    }
    return true;
}

/* hasConn: whether the client has made a connection the test has not taken,
 * after giving it a moment. */
static bool h2ct_tt_has_conn(H2ctTT *tt) {
    time_sleep(H2CT_QUIET);
    sync_mutex_lock(&tt->mu);
    bool has = false;
    for (H2ctConn *tc = tt->conns; tc != NULL; tc = tc->next)
        has = has || !tc->claimed;
    sync_mutex_unlock(&tt->mu);
    return has;
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

/* wantIdle: no frame for a moment. */
static bool h2ct_want_idle(H2ctConn *tc) {
    Error err = BURROW_NO_ERROR;
    Http2Frame *f = h2ct_read_frame_within(tc, H2CT_QUIET, &err);
    if (f != NULL && BURROW_OK(err)) {
        testing_t_errorf_v(tc->t, "unexpected frame type %d on stream %d",
                           (int)f->header.type, (int)f->header.stream_id);
        burrow__http2_frame_free(f);
        return false;
    }
    burrow__http2_frame_free(f);
    if (!errors_is(err, os_err_deadline_exceeded)) {
        testing_t_errorf_v(tc->t, "want idle, got %v", err);
        return false;
    }
    return true;
}

static bool h2ct_want_frame_type(H2ctConn *tc, Http2FrameType want) {
    Http2Frame *f = h2ct_read_type(tc, want);
    burrow__http2_frame_free(f);
    return f != NULL;
}

/* wantRSTStream: a RST_STREAM frame on stream id with code. */
static bool h2ct_want_rst_stream(H2ctConn *tc, uint32_t id, Http2ErrCode code) {
    Http2Frame *f = h2ct_read_type(tc, HTTP2_FRAME_RST_STREAM);
    if (f == NULL)
        return false;
    bool ok = f->header.stream_id == id && f->u.rst_stream.err_code == code;
    if (!ok)
        testing_t_errorf_v(tc->t,
                           "got RST_STREAM stream=%d code=%d, want stream=%d code=%d",
                           (int)f->header.stream_id, (int)f->u.rst_stream.err_code,
                           (int)id, (int)code);
    burrow__http2_frame_free(f);
    return ok;
}

/* wantSettingsAck: a SETTINGS frame with the ACK flag. */
static bool h2ct_want_settings_ack(H2ctConn *tc) {
    Http2Frame *f = h2ct_read_type(tc, HTTP2_FRAME_SETTINGS);
    if (f == NULL)
        return false;
    bool ok = (f->header.flags & HTTP2_FLAG_SETTINGS_ACK) != 0;
    if (!ok)
        testing_t_errorf_v(tc->t, "Settings frame is not an ACK");
    burrow__http2_frame_free(f);
    return ok;
}

/* wantGoAway: a GOAWAY frame with max_stream_id and code. */
static bool h2ct_want_go_away(H2ctConn *tc, uint32_t max_stream_id, Http2ErrCode code) {
    Http2Frame *f = h2ct_read_type(tc, HTTP2_FRAME_GO_AWAY);
    if (f == NULL)
        return false;
    bool ok =
        f->u.go_away.last_stream_id == max_stream_id && f->u.go_away.err_code == code;
    if (!ok)
        testing_t_errorf_v(tc->t,
                           "got GOAWAY LastStreamID=%d code=%d, want LastStreamID=%d "
                           "code=%d",
                           (int)f->u.go_away.last_stream_id, (int)f->u.go_away.err_code,
                           (int)max_stream_id, (int)code);
    burrow__http2_frame_free(f);
    return ok;
}

/* wantClosed: the connection ends with no more frames. */
static bool h2ct_want_closed(H2ctConn *tc) {
    Error err = BURROW_NO_ERROR;
    Http2Frame *f = h2ct_read_frame_within(tc, H2CT_WAIT, &err);
    if (f != NULL && BURROW_OK(err)) {
        testing_t_errorf_v(tc->t,
                           "got unexpected frame type %d (want closed connection)",
                           (int)f->header.type);
        burrow__http2_frame_free(f);
        return false;
    }
    burrow__http2_frame_free(f);
    if (errors_is(err, os_err_deadline_exceeded)) {
        testing_t_errorf_v(tc->t, "connection is not closed; want it to be");
        return false;
    }
    return true;
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

/* writeData with n zero bytes, as Go's make([]byte, n). */
static bool h2ct_write_zeros(H2ctConn *tc, uint32_t id, bool end_stream, Int n) {
    static const Byte zeros[64 << 10];
    if (n > (Int)sizeof zeros) {
        testing_t_errorf_v(tc->t, "%d zero bytes is more than the test has", (int)n);
        return false;
    }
    Slice p = slice_from((void *)(uintptr_t)zeros, n, n, TYPE_BYTE);
    return !h2ct_failed(tc, "writing DATA",
                        burrow__http2_framer_write_data(tc->fr, id, end_stream, p));
}

static bool h2ct_write_go_away_debug(H2ctConn *tc, uint32_t max_stream_id,
                                     Http2ErrCode code, const char *debug) {
    Int n = (Int)strlen(debug);
    Slice d = slice_from((void *)(uintptr_t)debug, n, n, TYPE_BYTE);
    return !h2ct_failed(
        tc, "writing GOAWAY",
        burrow__http2_framer_write_go_away(tc->fr, max_stream_id, code, d));
}

static bool h2ct_write_go_away(H2ctConn *tc, uint32_t max_stream_id,
                               Http2ErrCode code) {
    return h2ct_write_go_away_debug(tc, max_stream_id, code, "");
}

static bool h2ct_write_rst_stream(H2ctConn *tc, uint32_t id, Http2ErrCode code) {
    return !h2ct_failed(tc, "writing RST_STREAM",
                        burrow__http2_framer_write_rst_stream(tc->fr, id, code));
}

static bool h2ct_write_ping(H2ctConn *tc, bool ack, const Byte data[8]) {
    return !h2ct_failed(tc, "writing PING",
                        burrow__http2_framer_write_ping(tc->fr, ack, data));
}

static bool h2ct_write_window_update(H2ctConn *tc, uint32_t id, uint32_t incr) {
    return !h2ct_failed(tc, "writing WINDOW_UPDATE",
                        burrow__http2_framer_write_window_update(tc->fr, id, incr));
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
    /* The hook runs on the client's read loop. Room made here keeps it from
     * taking more from the test's arena while the test takes from it too. */
    if (!bytes_buffer_grow(buf, 1024)) {
        testing_t_errorf_v(tt->t, "no memory for the trace");
        return;
    }
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

/* A GET for the test's conn, on its way. */
static H2ctRT *h2ct_get(H2ctTT *tt, Str method) {
    return h2ct_tc_round_trip(tt,
                              h2ct_new_request(tt, (Context){0}, method, h2ct_no_body));
}

static void h2ct_read_head_response(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_get(tt, BURROW_S("HEAD"));
    H2CT_TRY(rt != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    static const Str hdr[] = {S_(":status"), S_("200"), S_("content-length"),
                              S_("123")};
    /* Not END_STREAM, as the GFE does. */
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, hdr, 4), false)));
    H2CT_TRY(h2ct_write_data(tc, id, true, ""));

    HttpResponse *res = h2ct_rt_response(rt);
    H2CT_TRY(res != NULL);
    if (res->content_length != 123)
        FATALF("Content-Length = %d; want 123", (int)res->content_length);
    (void)h2ct_rt_want_body(rt, "");
}

static void TestTransportReadHeadResponse(TestingT *t) {
    h2ct_run(t, h2ct_read_head_response, NULL);
}

static void h2ct_read_head_response_with_body(H2ctTT *tt, const void *arg) {
    (void)arg;
    /* This test uses an invalid response format. */
    static const char response[] = "redirecting to /elsewhere";
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_get(tt, BURROW_S("HEAD"));
    H2CT_TRY(rt != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    Str hdr[] = {BURROW_S(":status"), BURROW_S("200"), BURROW_S("content-length"),
                 fmt_sprintf_v(arena_allocator(&tt->ar), "%d", (int)strlen(response))};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, hdr, 4), false)));
    H2CT_TRY(h2ct_write_data(tc, id, true, response));

    HttpResponse *res = h2ct_rt_response(rt);
    H2CT_TRY(res != NULL);
    if (res->content_length != (int64_t)strlen(response))
        FATALF("Content-Length = %d; want %d", (int)res->content_length,
               (int)strlen(response));
    (void)h2ct_rt_want_body(rt, "");
}

static void TestTransportReadHeadResponseWithBody(TestingT *t) {
    h2ct_run(t, h2ct_read_head_response_with_body, NULL);
}

static void h2ct_allocations_after_response_body_close(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    /* Send request. */
    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("PUT"), h2ct_no_body));
    H2CT_TRY(rt != NULL && h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));

    /* Receive response with some body. */
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    static const Str res_kv[] = {S_(":status"), S_("200")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, res_kv, 2), false)));
    H2CT_TRY(h2ct_write_zeros(tc, id, false, 64));
    H2CT_TRY(h2ct_want_idle(tc));

    /* Client reads a byte of the body, and then closes it. */
    HttpResponse *res = h2ct_rt_response(rt);
    H2CT_TRY(res != NULL);
    Byte b[1];
    Error err = BURROW_NO_ERROR;
    IoReader r = io_read_closer_as_io_reader(res->body);
    (void)r.vt->read(r.data, slice_from(b, 1, 1, TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(tt->t, "%v", err);
    err = res->body.vt->closer.close(res->body.data);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(tt->t, "%v", err);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_RST_STREAM));

    /* Server sends more of the body, which is ignored. */
    H2CT_TRY(h2ct_write_zeros(tc, id, false, 64));

    err = BURROW_NO_ERROR;
    (void)r.vt->read(r.data, slice_from(b, 1, 1, TYPE_BYTE), &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(tt->t, "read from closed body unexpectedly succeeded");
}

static void TestTransportAllocationsAfterResponseBodyClose(TestingT *t) {
    h2ct_run(t, h2ct_allocations_after_response_body_close, NULL);
}

static void h2ct_no_body_means_no_data(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    HttpRequest *req =
        h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body);
    H2CT_TRY(req != NULL);
    req->body = http_no_body;
    H2ctRT *rt = h2ct_tc_round_trip(tt, req);
    H2CT_TRY(rt != NULL);
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    static const Str want[] = {S_(":authority"), S_("dummy.tld"), S_(":method"),
                               S_("GET"),        S_(":path"),     S_("/")};
    /* END_STREAM should be set when body is http.NoBody. */
    H2CT_TRY(h2ct_want_headers(tc, id, true, want, 6));
    (void)h2ct_want_idle(tc);
}

static void TestTransportNoBodyMeansNoDATA(TestingT *t) {
    h2ct_run(t, h2ct_no_body_means_no_data, NULL);
}

static void h2ct_returns_error_on_bad_response_headers(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_get(tt, BURROW_S("GET"));
    H2CT_TRY(rt != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    static const Str hdr[] = {S_(":status"), S_("200"), S_("  content-type"),
                              S_("bogus")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, hdr, 4), false)));

    Error err = h2ct_rt_err(rt);
    Error cause = burrow__http2_header_field_name_error(BURROW_S("  content-type"));
    if (!h2ct_is_stream_error(err, 1, HTTP2_ERR_CODE_PROTOCOL, &cause))
        FATALF("RoundTrip error = %v; want StreamError on stream 1 with "
               "ErrCodeProtocol and cause %v",
               err, cause);

    Http2Frame *f = h2ct_read_type(tc, HTTP2_FRAME_RST_STREAM);
    H2CT_TRY(f != NULL);
    if (f->header.stream_id != 1 || f->u.rst_stream.err_code != HTTP2_ERR_CODE_PROTOCOL)
        testing_t_errorf_v(tt->t,
                           "Frame = RST_STREAM stream=%d code=%d; want RST_STREAM for "
                           "stream 1 with ErrCodeProtocol",
                           (int)f->header.stream_id, (int)f->u.rst_stream.err_code);
    burrow__http2_frame_free(f);
}

static void TestTransportReturnsErrorOnBadResponseHeaders(TestingT *t) {
    h2ct_run(t, h2ct_returns_error_on_bad_response_headers, NULL);
}

static void h2ct_response_data_before_headers(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    /* First request is normal to ensure the check is per stream and not per
     * connection. */
    H2ctRT *rt1 = h2ct_get(tt, BURROW_S("GET"));
    H2CT_TRY(rt1 != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id1 = h2ct_rt_stream_id(rt1);
    H2CT_TRY(id1 != 0);
    H2CT_TRY(h2ct_write_status(tc, id1, "200"));
    H2CT_TRY(h2ct_rt_want_status(rt1, 200));

    /* Second request returns a DATA frame with no HEADERS. */
    H2ctRT *rt2 = h2ct_get(tt, BURROW_S("GET"));
    H2CT_TRY(rt2 != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id2 = h2ct_rt_stream_id(rt2);
    H2CT_TRY(id2 != 0);
    H2CT_TRY(h2ct_write_data(tc, id2, true, "payload"));
    Error err = h2ct_rt_err(rt2);
    if (!h2ct_is_stream_error(err, 0, HTTP2_ERR_CODE_PROTOCOL, NULL))
        FATALF("expected stream PROTOCOL_ERROR, got: %v", err);
}

static void TestTransportResponseDataBeforeHeaders(TestingT *t) {
    h2ct_run(t, h2ct_response_data_before_headers, NULL);
}

static void h2ct_handles_invalid_statusless_response(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_get(tt, BURROW_S("GET"));
    H2CT_TRY(rt != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    /* No :status header, and not END_STREAM: we'll send some DATA to try to
     * crash the transport. */
    static const Str hdr[] = {S_("content-type"), S_("text/html")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, hdr, 2), false)));
    H2CT_TRY(h2ct_write_data(tc, id, true, "payload"));
    /* Go stops here. This also checks that RoundTrip failed. */
    H2CT_TRY(h2ct_rt_result(rt));
    if (BURROW_OK(rt->err))
        testing_t_errorf_v(tt->t, "RoundTrip succeeded; want an error");
}

static void TestTransportHandlesInvalidStatuslessResponse(TestingT *t) {
    h2ct_run(t, h2ct_handles_invalid_statusless_response, NULL);
}

static void h2ct_no_retry_on_stream_protocol_error(H2ctTT *tt, const void *arg) {
    (void)arg;
    /* Start two requests. The first is a long request that will finish after
     * the second. The second one will result in the protocol error. */

    /* Request #1: The long request. */
    H2ctRT *rt1 = h2ct_tt_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt1 != NULL);
    H2ctConn *tc1 = h2ct_get_conn(tt);
    H2CT_TRY(tc1 != NULL && h2ct_want_new_conn_get(tc1));
    H2CT_TRY(h2ct_write_settings(tc1, NULL, 0));
    H2CT_TRY(h2ct_want_frame_type(tc1, HTTP2_FRAME_SETTINGS)); /* settings ACK */

    /* Request #2: The short request. */
    H2ctRT *rt2 = h2ct_tt_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt2 != NULL);
    H2CT_TRY(h2ct_want_headers(tc1, 3, true, NULL, 0));

    /* Request #2 fails with ErrCodeProtocol. */
    H2CT_TRY(h2ct_write_rst_stream(tc1, 3, HTTP2_ERR_CODE_PROTOCOL));
    if (!h2ct_rt_wait_done(rt2))
        FATALF("After protocol error on RoundTrip #2, RoundTrip #2 is in progress; "
               "want done");
    if (h2ct_rt_done(rt1))
        FATALF("After protocol error on RoundTrip #2, RoundTrip #1 is done; want "
               "still in progress");
    /* Request #2 should not be retried. */
    if (h2ct_tt_has_conn(tt))
        FATALF("After protocol error on RoundTrip #2, RoundTrip #2 is unexpectedly "
               "retried");

    /* Request #1 succeeds. */
    H2CT_TRY(h2ct_write_status(tc1, 1, "200"));
    (void)h2ct_rt_want_status(rt1, 200);
}

static void TestTransportNoRetryOnStreamProtocolError(TestingT *t) {
    h2ct_run(t, h2ct_no_retry_on_stream_protocol_error, NULL);
}

/* https://go.dev/issue/65927 - server sends a 1xx response, followed by a
 * DATA frame. */
static void h2ct_data_after_1xx_header(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_get(tt, BURROW_S("GET"));
    H2CT_TRY(rt != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    static const Str hdr[] = {S_(":status"), S_("100")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, hdr, 2), false)));
    static const Byte zero[1] = {0};
    H2CT_TRY(!h2ct_failed(
        tc, "writing DATA",
        burrow__http2_framer_write_data(
            tc->fr, id, true, slice_from((void *)(uintptr_t)zero, 1, 1, TYPE_BYTE))));
    Error err = h2ct_rt_err(rt);
    if (!h2ct_is_stream_error(err, 0, HTTP2_ERR_CODE_PROTOCOL, NULL))
        testing_t_errorf_v(tt->t, "RoundTrip error: %v; want ErrCodeProtocol", err);
    (void)h2ct_want_frame_type(tc, HTTP2_FRAME_RST_STREAM);
}

static void TestTransportDataAfter1xxHeader(TestingT *t) {
    h2ct_run(t, h2ct_data_after_1xx_header, NULL);
}

static void h2ct_uses_go_away_debug_error(H2ctTT *tt, const void *arg) {
    bool fail_mid_body = *(const bool *)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    const Http2ErrCode go_away_err_code =
        HTTP2_ERR_CODE_HTTP_1_1_REQUIRED; /* arbitrary */
    static const char go_away_debug_data[] = "some debug data";

    H2ctRT *rt = h2ct_get(tt, BURROW_S("GET"));
    H2CT_TRY(rt != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);

    if (fail_mid_body) {
        static const Str hdr[] = {S_(":status"), S_("200"), S_("content-length"),
                                  S_("123")};
        H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, hdr, 4), false)));
    }

    /* Write two GOAWAY frames, to test that the Transport takes the
     * interesting parts of both. */
    H2CT_TRY(h2ct_write_go_away_debug(tc, 5, HTTP2_ERR_CODE_NO, go_away_debug_data));
    H2CT_TRY(h2ct_write_go_away(tc, 5, go_away_err_code));
    h2ct_close_write(tc);

    H2CT_TRY(h2ct_rt_result(rt));
    Error err = rt->err;
    const char *whence = "RoundTrip";
    if (fail_mid_body) {
        whence = "Body.Read";
        if (BURROW_FAILED(err))
            FATALF("RoundTrip error = %v, want success", err);
        Byte b[1];
        IoReader r = io_read_closer_as_io_reader(rt->res->body);
        (void)r.vt->read(r.data, slice_from(b, 1, 1, TYPE_BYTE), &err);
    }

    Http2GoAwayError ge;
    memset(&ge, 0, sizeof ge);
    bool ok = burrow__http2_error_go_away(err, &ge) && ge.last_stream_id == 5 &&
              ge.err_code == go_away_err_code &&
              str_eq(ge.debug_data, str_from_cstr(go_away_debug_data));
    if (!ok)
        testing_t_errorf_v(tt->t,
                           "%s error = %v, want GoAwayError{LastStreamID: 5, ErrCode: "
                           "HTTP_1_1_REQUIRED, DebugData: %q}",
                           str_from_cstr(whence), err,
                           str_from_cstr(go_away_debug_data));
}

static void TestTransportUsesGoAwayDebugError_RoundTrip(TestingT *t) {
    static const bool fail_mid_body = false;
    h2ct_run(t, h2ct_uses_go_away_debug_error, &fail_mid_body);
}

static void TestTransportUsesGoAwayDebugError_Body(TestingT *t) {
    static const bool fail_mid_body = true;
    h2ct_run(t, h2ct_uses_go_away_debug_error, &fail_mid_body);
}

static void h2ct_cookie_header_split(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    HttpRequest *req =
        h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body);
    H2CT_TRY(req != NULL);
    (void)http_header_add(req->header, BURROW_S("Cookie"), BURROW_S("a=b;c=d;  e=f;"));
    (void)http_header_add(req->header, BURROW_S("Cookie"), BURROW_S("e=f;g=h; "));
    (void)http_header_add(req->header, BURROW_S("Cookie"), BURROW_S("i=j"));
    H2ctRT *rt = h2ct_tc_round_trip(tt, req);
    H2CT_TRY(rt != NULL);
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);

    static const Str want[] = {S_("cookie"), S_("a=b"), S_("cookie"), S_("c=d"),
                               S_("cookie"), S_("e=f"), S_("cookie"), S_("e=f"),
                               S_("cookie"), S_("g=h"), S_("cookie"), S_("i=j")};
    H2CT_TRY(h2ct_want_headers(tc, id, true, want, 12));
    H2CT_TRY(h2ct_write_status(tc, id, "204"));

    Error err = h2ct_rt_err(rt);
    if (BURROW_FAILED(err))
        FATALF("RoundTrip = %v, want success", err);
}

static void TestTransportCookieHeaderSplit(TestingT *t) {
    h2ct_run(t, h2ct_cookie_header_split, NULL);
}

static void h2ct_body_eager_end_stream(H2ctTT *tt, const void *arg) {
    (void)arg;
    static const char req_body[] = "some request body";
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    StringsReader *body =
        strings_new_reader(arena_allocator(&tt->ar), str_from_cstr(req_body));
    H2CT_TRY(body != NULL);
    H2ctRT *rt =
        h2ct_tc_round_trip(tt, h2ct_new_request(tt, (Context){0}, BURROW_S("PUT"),
                                                strings_reader_as_io_reader(body)));
    H2CT_TRY(rt != NULL);

    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    Http2Frame *f = h2ct_read_type(tc, HTTP2_FRAME_DATA);
    H2CT_TRY(f != NULL);
    bool ended = (f->header.flags & HTTP2_FLAG_DATA_END_STREAM) != 0;
    Int n = f->u.data.data.len;
    burrow__http2_frame_free(f);
    if (!ended)
        FATALF("data frame without END_STREAM, %d bytes", (int)n);
}

static void TestTransportBodyEagerEndStream(TestingT *t) {
    h2ct_run(t, h2ct_body_eager_end_stream, NULL);
}

/* headerListSize: the size of h as RFC 7540 section 6.5.2 counts it. */
static uint64_t h2ct_header_list_size(HttpHeader h) {
    uint64_t size = 0;
    const void *key;
    void *val;
    for (MapIter it = map_iter(h); map_next(&it, &key, &val);) {
        Str k = *(const Str *)key;
        Slice vv = *(const Slice *)val;
        for (Int i = 0; i < vv.len; i++)
            size += (uint64_t)k.len + (uint64_t)((const Str *)vv.p)[i].len + 32;
    }
    return size;
}

/* padHeaders adds data to an http.Header until headerListSize(h) == limit.
 * Due to the way header list sizes are calculated, padHeaders cannot add
 * fewer than len("Pad-Headers") + 32 bytes to h, and will fail if asked to
 * add fewer. The names and values come from a. */
static bool h2ct_pad_headers(TestingT *t, Alloc *a, HttpHeader h, uint64_t limit,
                             Str filler) {
    if (limit > 0xffffffffU) {
        testing_t_fatalf_v(t,
                           "padHeaders: refusing to pad to more than 2^32-1 bytes. "
                           "limit = %d",
                           limit);
        return false;
    }
    uint64_t min_padding = sizeof "Pad-Headers" - 1 + 32;
    uint64_t size = h2ct_header_list_size(h);

    uint64_t minlimit = size + min_padding;
    if (limit < minlimit) {
        testing_t_fatalf_v(t, "padHeaders: limit %d < %d", limit, minlimit);
        return false;
    }

    /* Use a fixed-width format for name so that fieldSize remains
     * constant. */
    uint64_t field_size = sizeof "Pad-Headers-000001" - 1 + (uint64_t)filler.len + 32;

    /* Add as many complete filler values as possible, leaving room for at
     * least one empty "Pad-Headers" key. */
    limit = limit - min_padding;
    for (int i = 0; size + field_size < limit; i++) {
        Str name = fmt_sprintf_v(a, "Pad-Headers-%06d", i);
        if (name.len == 0 || !http_header_add(h, name, filler)) {
            testing_t_fatalf_v(t, "padHeaders: out of memory");
            return false;
        }
        size += field_size;
    }

    /* Add enough bytes to reach limit. */
    uint64_t remain = limit - size;
    Str last_value = strings_repeat(a, BURROW_S("*"), (Int)remain);
    if ((uint64_t)last_value.len != remain ||
        !http_header_add(h, BURROW_S("Pad-Headers"), last_value)) {
        testing_t_fatalf_v(t, "padHeaders: out of memory");
        return false;
    }
    return true;
}

/* One check of TestPadHeaders, on h, whose padding comes from keep, or on a
 * new header when h is NULL. */
static bool h2ct_pad_check(TestingT *t, Arena *keep, HttpHeader h, uint32_t limit,
                           Int filler_len) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = h == NULL ? arena_allocator(&ar) : arena_allocator(keep);
    if (h == NULL)
        h = http_header_make(a);
    bool ok = h != NULL;
    if (!ok)
        testing_t_fatalf_v(t, "no memory for the header");
    if (ok)
        ok = h2ct_pad_headers(t, a, h, limit,
                              strings_repeat(a, BURROW_S("f"), filler_len));
    if (ok) {
        uint64_t got_size = h2ct_header_list_size(h);
        if (got_size != limit)
            testing_t_errorf_v(t, "Got size = %d; want %d", got_size, (int64_t)limit);
    }
    arena_free(&ar);
    return ok;
}

static void TestPadHeaders(TestingT *t) {
    /* Try all possible combinations for small fillerLen and limit. */
    const uint32_t min_limit = sizeof "Pad-Headers" - 1 + 32;
    for (uint32_t limit = min_limit; limit <= 128; limit++) {
        for (Int filler_len = 0; (uint32_t)filler_len <= limit; filler_len++) {
            if (!h2ct_pad_check(t, NULL, NULL, limit, filler_len))
                return;
        }
    }

    /* Try a few tests with larger limits, plus cumulative tests. Since these
     * tests are cumulative, tests[i+1].limit must be >= tests[i].limit +
     * minLimit. See the comment on padHeaders for more info on why the limit
     * arg has this restriction. */
    static const struct {
        Int filler_len;
        uint32_t limit;
    } tests[] = {
        {64, 1024}, {1024, 1286}, {256, 2048}, {1024, 10 * 1024}, {1023, 11 * 1024},
    };
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    HttpHeader h = http_header_make(arena_allocator(&ar));
    if (h == NULL) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "no memory for the header");
        return;
    }
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        if (!h2ct_pad_check(t, NULL, NULL, tests[i].limit, tests[i].filler_len) ||
            !h2ct_pad_check(t, &ar, h, tests[i].limit, tests[i].filler_len))
            break;
    }
    arena_free(&ar);
}

/* What ErrResponseHeaderListSize, as the cause of a StreamError or not. */
static bool h2ct_is_header_list_size(Error err) {
    Http2StreamError se;
    memset(&se, 0, sizeof se);
    if (burrow__http2_error_stream(err, &se))
        err = se.cause;
    return BURROW_FAILED(err) &&
           str_eq(error_text(err),
                  error_text(burrow__http2_err_response_header_list_size));
}

enum { H2CT_LARGE = 1 << 10, H2CT_LARGE_PAIRS = 5042 };

/* The block both header list size tests send: 5042 pairs of a 1KB name and
 * value, over 10MB in all, after a :status of 200 when status is set. It has
 * to come to want bytes. */
static bool h2ct_large_block(H2ctTT *tt, H2ctConn *tc, bool status, Int want,
                             Slice *out) {
    Alloc *a = arena_allocator(&tt->ar);
    size_t n = 2 * H2CT_LARGE_PAIRS + (status ? 2 : 0);
    Str *kv = (Str *)mem_alloc(a, n * sizeof *kv, _Alignof(Str));
    char *large = (char *)mem_alloc_nozero(a, H2CT_LARGE, 1);
    if (kv == NULL || large == NULL) {
        testing_t_errorf_v(tt->t, "no memory for the headers");
        return false;
    }
    memset(large, 'a', H2CT_LARGE);
    size_t i = 0;
    if (status) {
        kv[i++] = BURROW_S(":status");
        kv[i++] = BURROW_S("200");
    }
    while (i < n)
        kv[i++] = str_from_bytes(large, H2CT_LARGE);
    *out = h2ct_block(tc, kv, n);
    /* Note: this number might change if our hpack implementation changes.
     * That's fine. This is just a sanity check that our response can fit in a
     * single header block fragment frame. */
    if (out->len != want) {
        testing_t_errorf_v(tt->t,
                           "encoding over 10MB of duplicate keypairs took %d bytes; "
                           "expected %d",
                           (int)out->len, (int)want);
        return false;
    }
    return true;
}

static void h2ct_checks_response_header_list_size(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);

    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));

    Slice hbf;
    H2CT_TRY(h2ct_large_block(tt, tc, true, 6329, &hbf));
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, hbf, true)));

    H2CT_TRY(h2ct_rt_result(rt));
    if (!h2ct_is_header_list_size(rt->err))
        FATALF("RoundTrip Error = %v; want errResponseHeaderListSize", rt->err);
}

static void h2ct_checks_response_trailer_header_list_size(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);

    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    static const Str kv[] = {S_(":status"), S_("200"), S_("trailer"), S_("x-trailer")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, kv, 4), false)));
    H2CT_TRY(h2ct_rt_want_status(rt, 200));

    Slice hbf;
    H2CT_TRY(h2ct_large_block(tt, tc, false, 6328, &hbf));
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, hbf, true)));

    Error err = BURROW_NO_ERROR;
    (void)h2ct_rt_read_body(rt, &err);
    if (!h2ct_is_header_list_size(err))
        testing_t_errorf_v(tt->t, "Read = %v, want %v", err,
                           burrow__http2_err_response_header_list_size);
    /* Verify that this is treated as a StreamError that does not close the
     * whole connection down. */
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_RST_STREAM));
    H2CT_TRY(h2ct_want_idle(tc));
}

static void TestTransportChecksResponseHeaderListSize_headers(TestingT *t) {
    h2ct_run(t, h2ct_checks_response_header_list_size, NULL);
}

static void TestTransportChecksResponseHeaderListSize_trailers(TestingT *t) {
    h2ct_run(t, h2ct_checks_response_trailer_header_list_size, NULL);
}

/* #15425: Transport goroutine leak while the transport is still trying to
 * write its body after the stream has completed. */
static void h2ct_stream_ends_while_body_is_being_written(H2ctTT *tt, const void *arg) {
    (void)arg;
    static const char body[] = "this is the client request body";
    enum { WINDOW_SIZE = 10 }; /* less than len(body) */

    H2ctConn *tc = h2ct_new_client_conn(tt);
    static const Http2Setting s[] = {{HTTP2_SETTING_INITIAL_WINDOW_SIZE, WINDOW_SIZE}};
    H2CT_TRY(tc != NULL && h2ct_greet(tc, s, 1));

    /* Client sends a request, and as much body as fits into the stream
     * window. */
    StringsReader *r =
        strings_new_reader(arena_allocator(&tt->ar), str_from_cstr(body));
    H2CT_TRY(r != NULL);
    H2ctRT *rt =
        h2ct_tc_round_trip(tt, h2ct_new_request(tt, (Context){0}, BURROW_S("PUT"),
                                                strings_reader_as_io_reader(r)));
    H2CT_TRY(rt != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    H2CT_TRY(h2ct_want_data(tc, id, false, WINDOW_SIZE, NULL, false));

    /* Server responds without permitting the rest of the body to be sent. */
    H2CT_TRY(h2ct_write_status(tc, id, "413"));
    H2CT_TRY(h2ct_rt_want_status(rt, 413));
}

static void TestTransportStreamEndsWhileBodyIsBeingWritten(TestingT *t) {
    h2ct_run(t, h2ct_stream_ends_while_body_is_being_written, NULL);
}

/* testTransportClosesConnAfterGoAway verifies that the transport closes a
 * connection after reading a GOAWAY from it.
 *
 * lastStream is the last stream ID in the GOAWAY frame. When 0, the transport
 * (unsuccessfully) retries the request (stream 1); when 1, the transport reads
 * the response after receiving the GOAWAY. */
static void h2ct_closes_conn_after_go_away(H2ctTT *tt, const void *arg) {
    uint32_t last_stream = *(const uint32_t *)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);

    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    H2CT_TRY(h2ct_write_go_away(tc, last_stream, HTTP2_ERR_CODE_NO));

    if (last_stream > 0) {
        /* Send a valid response to first request. */
        uint32_t id = h2ct_rt_stream_id(rt);
        H2CT_TRY(id != 0);
        H2CT_TRY(h2ct_write_status(tc, id, "200"));
    }

    h2ct_close_write(tc);
    Error err = h2ct_rt_err(rt);
    bool got_err = BURROW_FAILED(err);
    bool want_err = last_stream == 0;
    if (got_err != want_err)
        testing_t_errorf_v(tt->t, "RoundTrip got error %v (want error: %v)", err,
                           want_err);
    if (!h2ct_is_closed(tc))
        testing_t_errorf_v(tt->t,
                           "ClientConn did not close its net.Conn, expected it to");
}

static void TestTransportClosesConnAfterGoAwayNoStreams(TestingT *t) {
    static const uint32_t last_stream = 0;
    h2ct_run(t, h2ct_closes_conn_after_go_away, &last_stream);
}

static void TestTransportClosesConnAfterGoAwayLastStream(TestingT *t) {
    static const uint32_t last_stream = 1;
    h2ct_run(t, h2ct_closes_conn_after_go_away, &last_stream);
}

/* testTransportSettingsFlowControlUpdate: a SETTINGS frame that raises the
 * initial window takes a stream with extra flow control to the limit, or one
 * past it. */
static void h2ct_settings_flow_control_update(H2ctTT *tt, const void *arg) {
    bool beyond = *(const bool *)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);

    /* Give this stream some additional flow control. */
    enum { WINDOW_INCREASE = 1000 };
    H2CT_TRY(h2ct_write_window_update(tc, id, WINDOW_INCREASE));
    H2CT_TRY(h2ct_want_idle(tc));

    /* Adjust the initial flow control window. Within the limit, the stream is
     * just at it. Beyond it, the stream is now over it. */
    const uint32_t max_window_size = 0x7fffffffU; /* RFC 9113, 6.9.1 */
    const uint32_t max_initial_window_size = max_window_size - WINDOW_INCREASE;
    Http2Setting s[1];
    s[0].id = HTTP2_SETTING_INITIAL_WINDOW_SIZE;
    s[0].val = beyond ? max_initial_window_size + 1 : max_initial_window_size;
    H2CT_TRY(h2ct_write_settings(tc, s, 1));
    if (beyond) {
        H2CT_TRY(h2ct_want_go_away(tc, 0, HTTP2_ERR_CODE_FLOW_CONTROL));
        return;
    }
    H2CT_TRY(h2ct_want_settings_ack(tc));
    H2CT_TRY(h2ct_want_idle(tc));
}

static void TestTransportSettingsFlowControlUpdateBeyondLimit(TestingT *t) {
    static const bool beyond = true;
    h2ct_run(t, h2ct_settings_flow_control_update, &beyond);
}

static void TestTransportSettingsFlowControlUpdateWithinLimit(TestingT *t) {
    static const bool beyond = false;
    h2ct_run(t, h2ct_settings_flow_control_update, &beyond);
}

static void h2ct_window_update_beyond_limit(H2ctTT *tt, const void *arg) {
    (void)arg;
    /* Will cause window to exceed limit of 2^31-1. */
    const uint32_t window_increase = 0x7fffffffU;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    H2CT_TRY(h2ct_want_headers(tc, id, true, NULL, 0));

    H2CT_TRY(h2ct_write_window_update(tc, id, window_increase));
    H2CT_TRY(h2ct_want_rst_stream(tc, id, HTTP2_ERR_CODE_FLOW_CONTROL));

    H2CT_TRY(h2ct_write_window_update(tc, 0, window_increase));
    H2CT_TRY(h2ct_want_closed(tc));
}

static void TestTransportWindowUpdateBeyondLimit(TestingT *t) {
    h2ct_run(t, h2ct_window_update_beyond_limit, NULL);
}

/* Go's InitialWindowSize, the window a connection and its streams start
 * with. */
enum { H2CT_INITIAL_WINDOW_SIZE = 65535 };

static void h2ct_adjusts_flow_control(H2ctTT *tt, const void *arg) {
    (void)arg;
    enum { BODY_SIZE = 1 << 20 };

    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_SETTINGS));
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_WINDOW_UPDATE));
    /* Don't write our SETTINGS yet. */

    H2ctBody *body = h2ct_new_request_body(tt);
    H2CT_TRY(body != NULL);
    h2ct_body_write_bytes(body, BODY_SIZE);
    h2ct_body_close_with_error(body, io_eof);

    H2ctRT *rt =
        h2ct_tc_round_trip(tt, h2ct_new_request(tt, (Context){0}, BURROW_S("POST"),
                                                (IoReader){&h2ct_body_vt, body}));
    H2CT_TRY(rt != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));

    int64_t got_bytes = 0;
    for (;;) {
        Http2Frame *f = h2ct_read_type(tc, HTTP2_FRAME_DATA);
        H2CT_TRY(f != NULL);
        got_bytes += f->u.data.data.len;
        burrow__http2_frame_free(f);
        /* After we've got half the client's initial flow control window's
         * worth of request body data, give it just enough flow control to
         * finish. */
        if (got_bytes >= H2CT_INITIAL_WINDOW_SIZE / 2)
            break;
    }

    Http2Setting s[1];
    s[0].id = HTTP2_SETTING_INITIAL_WINDOW_SIZE;
    s[0].val = BODY_SIZE;
    H2CT_TRY(h2ct_write_settings(tc, s, 1));
    H2CT_TRY(h2ct_write_window_update(tc, 0, BODY_SIZE));
    H2CT_TRY(h2ct_write_settings_ack(tc));

    /* wantUnorderedFrames: a SETTINGS frame, and DATA frames up to the one
     * that ends the stream. */
    bool seen_settings = false;
    bool seen_end = false;
    while (!seen_settings || !seen_end) {
        Http2Frame *f = h2ct_read_frame(tc);
        H2CT_TRY(f != NULL);
        bool ok = true;
        if (f->header.type == HTTP2_FRAME_SETTINGS && !seen_settings) {
            seen_settings = true;
        } else if (f->header.type == HTTP2_FRAME_DATA && !seen_end) {
            got_bytes += f->u.data.data.len;
            seen_end = (f->header.flags & HTTP2_FLAG_DATA_END_STREAM) != 0;
        } else {
            testing_t_errorf_v(tt->t, "got unexpected frame type %d",
                               (int)f->header.type);
            ok = false;
        }
        burrow__http2_frame_free(f);
        H2CT_TRY(ok);
    }

    if (got_bytes != BODY_SIZE)
        FATALF("server received %d bytes of body, want %d", got_bytes, BODY_SIZE);

    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    H2CT_TRY(h2ct_write_status(tc, id, "200"));
    H2CT_TRY(h2ct_rt_want_status(rt, 200));
}

static void TestTransportAdjustsFlowControl(TestingT *t) {
    h2ct_run(t, h2ct_adjusts_flow_control, NULL);
}

/* Go also checks that the connection's inflow window is back where it
 * started. The test can't see inside the client here, so that check is left
 * out, and the WINDOW_UPDATE for all 5000 bytes is what shows they came back. */
static void h2ct_returns_unused_flow_control(H2ctTT *tt, const void *arg) {
    bool one_data_frame = *(const bool *)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);

    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    static const Str res_kv[] = {S_(":status"), S_("200"), S_("content-length"),
                                 S_("5000")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, res_kv, 4), false)));

    /* Two cases:
     * - Send one DATA frame with 5000 bytes.
     * - Send two DATA frames with 1 and 4999 bytes each.
     *
     * In both cases, the client should consume one byte of data, refund that
     * byte, then refund the following 4999 bytes.
     *
     * In the second case, the server waits for the client to reset the stream
     * before sending the second DATA frame. This tests the case where the
     * client receives a DATA frame after it has reset the stream. */
    const bool stream_not_ended = false;
    H2CT_TRY(h2ct_write_zeros(tc, id, stream_not_ended, one_data_frame ? 5000 : 1));

    HttpResponse *res = h2ct_rt_response(rt);
    H2CT_TRY(res != NULL);
    Byte b[1];
    Error err = BURROW_NO_ERROR;
    IoReader r = io_read_closer_as_io_reader(res->body);
    Int n = r.vt->read(r.data, slice_from(b, 1, 1, TYPE_BYTE), &err);
    if (BURROW_FAILED(err) || n != 1)
        FATALF("body read = %d, %v; want 1, nil", n, err);
    (void)res->body.vt->closer.close(res->body.data); /* leaving 4999 bytes unread */

    /* wantUnorderedFrames: a RST_STREAM and a WINDOW_UPDATE. */
    bool sent_additional_data = false;
    bool seen_rst = false;
    bool seen_wu = false;
    while (!seen_rst || !seen_wu) {
        Http2Frame *f = h2ct_read_frame(tc);
        H2CT_TRY(f != NULL);
        bool ok = true;
        if (f->header.type == HTTP2_FRAME_RST_STREAM && !seen_rst) {
            seen_rst = true;
            if (f->u.rst_stream.err_code != HTTP2_ERR_CODE_CANCEL) {
                testing_t_errorf_v(tt->t,
                                   "Expected a RSTStreamFrame with code cancel; got "
                                   "code %d",
                                   (int)f->u.rst_stream.err_code);
                ok = false;
            } else if (!one_data_frame) {
                /* Send the remaining data now. */
                ok = h2ct_write_zeros(tc, id, stream_not_ended, 4999);
                sent_additional_data = true;
            }
        } else if (f->header.type == HTTP2_FRAME_WINDOW_UPDATE && !seen_wu) {
            seen_wu = true;
            if (!one_data_frame && !sent_additional_data) {
                testing_t_errorf_v(tt->t,
                                   "Got WindowUpdateFrame, don't expect one yet");
                ok = false;
            } else if (f->u.window_update.increment != 5000) {
                testing_t_errorf_v(tt->t,
                                   "Expected WindowUpdateFrames for 5000 bytes; got "
                                   "stream %d increment %d",
                                   (int)f->header.stream_id,
                                   (int)f->u.window_update.increment);
                ok = false;
            }
        } else {
            testing_t_errorf_v(tt->t, "got unexpected frame type %d",
                               (int)f->header.type);
            ok = false;
        }
        burrow__http2_frame_free(f);
        H2CT_TRY(ok);
    }
}

static void TestTransportReturnsUnusedFlowControlSingleWrite(TestingT *t) {
    static const bool one_data_frame = true;
    h2ct_run(t, h2ct_returns_unused_flow_control, &one_data_frame);
}

static void TestTransportReturnsUnusedFlowControlMultipleWrites(TestingT *t) {
    static const bool one_data_frame = false;
    h2ct_run(t, h2ct_returns_unused_flow_control, &one_data_frame);
}

/* Waits for a dial to be waiting on dial_held, or for none to be. */
static bool h2ct_tt_wait_dials(H2ctTT *tt, bool waiting) {
    Time deadline = time_add(time_now(), H2CT_WAIT);
    for (;;) {
        sync_mutex_lock(&tt->mu);
        bool ok = (tt->dials_waiting > 0) == waiting;
        sync_mutex_unlock(&tt->mu);
        if (ok)
            return true;
        if (!time_before(time_now(), deadline))
            return false;
        time_sleep(TIME_MILLISECOND);
    }
}

/* newTestTransportWithUnusedConn: tt with a connection that was dialed for
 * a request cancelled before the dial was done, and so never used. */
static bool h2ct_tt_with_unused_conn(H2ctTT *tt) {
    sync_mutex_lock(&tt->mu);
    tt->dial_held = true;
    sync_mutex_unlock(&tt->mu);

    H2ctRT *rt = h2ct_tt_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    if (rt == NULL) {
        h2ct_tt_release_dials(tt);
        return false;
    }
    /* Go's roundTrip waits for the bubble, so the dial has started by the
     * time the request is canceled. This waits for it to. */
    if (!h2ct_tt_wait_dials(tt, true)) {
        h2ct_tt_release_dials(tt);
        testing_t_fatalf_v(tt->t, "RoundTrip did not dial");
        return false;
    }
    BURROW_CALLF0(rt->cancel);
    bool done = h2ct_rt_wait_done(rt);
    if (!done || BURROW_OK(rt->err)) {
        h2ct_tt_release_dials(tt);
        testing_t_fatalf_v(tt->t, "RoundTrip still running after request is canceled");
        return false;
    }

    h2ct_tt_release_dials(tt);
    /* And Go waits for the bubble again, for the conn to be in the pool. */
    (void)h2ct_tt_wait_dials(tt, false);
    time_sleep(H2CT_QUIET);
    return true;
}

static void h2ct_go_away_with_no_conns(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2CT_TRY(h2ct_tt_with_unused_conn(tt));
    H2ctConn *tc = h2ct_get_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));
    H2CT_TRY(h2ct_write_go_away(tc, 1, HTTP2_ERR_CODE_NO));
    H2CT_TRY(h2ct_want_closed(tc));
}

static void TestTransportGoAwayWithNoConns(TestingT *t) {
    h2ct_run(t, h2ct_go_away_with_no_conns, NULL);
}

static void h2ct_unused_conn_ok(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2CT_TRY(h2ct_tt_with_unused_conn(tt));

    H2ctConn *tc = h2ct_get_conn(tt);
    H2CT_TRY(tc != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_SETTINGS));
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_WINDOW_UPDATE));

    /* Send a request on the Transport. It uses the conn we provided. */
    H2ctRT *rt = h2ct_tt_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);
    static const Str want[] = {S_(":authority"), S_("dummy.tld"), S_(":method"),
                               S_("GET"),        S_(":path"),     S_("/")};
    H2CT_TRY(h2ct_want_headers(tc, 1, true, want, 6));

    H2CT_TRY(h2ct_write_settings(tc, NULL, 0));
    H2CT_TRY(h2ct_write_settings_ack(tc));
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_SETTINGS)); /* acknowledgement */

    H2CT_TRY(h2ct_write_status(tc, 1, "200"));
    H2CT_TRY(h2ct_rt_want_status(rt, 200));
    (void)h2ct_rt_want_body(rt, "");
}

static void TestTransportUnusedConnOK(TestingT *t) {
    h2ct_run(t, h2ct_unused_conn_ok, NULL);
}

/* What the three tests below end with: a request the Transport sends on a
 * new conn. */
static void h2ct_want_new_conn(H2ctTT *tt) {
    H2ctRT *rt = h2ct_tt_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);
    H2ctConn *tc2 = h2ct_get_conn(tt);
    H2CT_TRY(tc2 != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc2, HTTP2_FRAME_SETTINGS));
    H2CT_TRY(h2ct_want_frame_type(tc2, HTTP2_FRAME_WINDOW_UPDATE));
    H2CT_TRY(h2ct_want_frame_type(tc2, HTTP2_FRAME_HEADERS));
}

static void h2ct_unused_conn_immediate_failure_used(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2CT_TRY(h2ct_tt_with_unused_conn(tt));

    /* The connection encounters an error before we send a request that uses
     * it. */
    H2ctConn *tc1 = h2ct_get_conn(tt);
    H2CT_TRY(tc1 != NULL);
    h2ct_close_write(tc1);

    /* Send a request on the Transport.
     *
     * It should fail, because we have no usable connections, but not with
     * ErrNoCachedConn. */
    H2ctRT *rt = h2ct_tt_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);
    Error err = h2ct_rt_err(rt);
    if (BURROW_OK(err) || errors_is(err, burrow__http2_err_no_cached_conn))
        FATALF("RoundTrip with broken conn: got %v, want an error other than "
               "ErrNoCachedConn",
               err);

    /* Send the request again. This time it is sent on a new conn because the
     * dead conn has been removed from the pool. */
    h2ct_want_new_conn(tt);
}

static void TestTransportUnusedConnImmediateFailureUsed(TestingT *t) {
    h2ct_run(t, h2ct_unused_conn_immediate_failure_used, NULL);
}

static void h2ct_unused_conn_idle_timeout_before_use(H2ctTT *tt, const void *arg) {
    (void)arg;
    tt->tr1.idle_conn_timeout = 1 * TIME_SECOND;
    H2CT_TRY(h2ct_tt_with_unused_conn(tt));
    H2CT_TRY(h2ct_get_conn(tt) != NULL);

    /* The connection idles out before we send a request that uses it. */
    time_sleep(2 * TIME_SECOND);

    /* Send a request on the Transport.
     *
     * It is sent on a new conn because the old one has idled out and been
     * removed from the pool. */
    h2ct_want_new_conn(tt);
}

static void TestTransportUnusedConnIdleTimoutBeforeUse(TestingT *t) {
    h2ct_run(t, h2ct_unused_conn_idle_timeout_before_use, NULL);
}

static void h2ct_tls_next_proto_conn_immediate_failure_unused(H2ctTT *tt,
                                                              const void *arg) {
    (void)arg;
    tt->tr1.idle_conn_timeout = 1 * TIME_SECOND;
    H2CT_TRY(h2ct_tt_with_unused_conn(tt));

    /* The connection encounters an error before we send a request that uses
     * it. */
    H2ctConn *tc1 = h2ct_get_conn(tt);
    H2CT_TRY(tc1 != NULL);
    h2ct_close_write(tc1);

    /* Some time passes. The dead connection is removed from the pool. Go
     * lets 10 seconds pass on its fake clock. Here 2 real ones, past the
     * idle timeout, are enough. */
    time_sleep(2 * TIME_SECOND);

    /* Send a request on the Transport. It is sent on a new conn. */
    h2ct_want_new_conn(tt);
}

static void TestTransportTLSNextProtoConnImmediateFailureUnused(TestingT *t) {
    h2ct_run(t, h2ct_tls_next_proto_conn_immediate_failure_unused, NULL);
}

/* Go cancels one request with its Cancel channel, which an HttpRequest
 * doesn't have, so this cancels the request's context instead. */
static void h2ct_requests_stall_at_server_limit(H2ctTT *tt, const void *arg) {
    (void)arg;
    enum { MAX_CONCURRENT = 2 };

    static const HttpHTTP2Config h2 = {.strict_max_concurrent_requests = true};
    tt->tr1.http2 = &h2;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    static const Http2Setting s[] = {
        {HTTP2_SETTING_MAX_CONCURRENT_STREAMS, MAX_CONCURRENT}};
    H2CT_TRY(tc != NULL && h2ct_greet(tc, s, 1));

    /* Start maxConcurrent+2 requests. The server does not respond to any of
     * them yet. */
    H2ctRT *rts[MAX_CONCURRENT + 2];
    for (int k = 0; k < MAX_CONCURRENT + 2; k++) {
        Str url = fmt_sprintf_v(arena_allocator(&tt->ar), "http://dummy.tld/%d", k);
        H2ctRT *rt = h2ct_tc_round_trip(
            tt,
            h2ct_new_request_url(tt, (Context){0}, BURROW_S("GET"), url, h2ct_no_body));
        H2CT_TRY(rt != NULL);
        rts[k] = rt;

        if (k < MAX_CONCURRENT) {
            /* We are under the stream limit, so the client sends the
             * request. */
            uint32_t id = h2ct_rt_stream_id(rt);
            H2CT_TRY(id != 0);
            Str want[] = {S_(":authority"), S_("dummy.tld"), S_(":method"),
                          S_("GET"),        S_(":path"),     {NULL, 0}};
            want[5] = str_from_bytes(url.p + 16, url.len - 16);
            H2CT_TRY(h2ct_want_headers(tc, id, true, want, 6));
        } else {
            /* We have reached the stream limit, so the client cannot send the
             * request. */
            if (!h2ct_want_idle(tc))
                FATALF("after making new request while at stream limit, got "
                       "unexpected frame");
        }

        if (h2ct_rt_done(rt))
            FATALF("rt %d done", k);
    }

    /* Cancel the maxConcurrent'th request. The request should fail. */
    BURROW_CALLF0(rts[MAX_CONCURRENT]->cancel);
    if (BURROW_OK(h2ct_rt_err(rts[MAX_CONCURRENT])))
        FATALF("RoundTrip(%d) should have failed due to cancel, did not",
               MAX_CONCURRENT);

    /* No requests should be complete, except for the canceled one. */
    for (int i = 0; i < MAX_CONCURRENT + 2; i++) {
        if (i != MAX_CONCURRENT && h2ct_rt_done(rts[i]))
            FATALF("RoundTrip(%d) is done, but should not be", i);
    }

    /* Server responds to a request, unblocking the last one. */
    uint32_t id0 = h2ct_rt_stream_id(rts[0]);
    H2CT_TRY(id0 != 0);
    H2CT_TRY(h2ct_write_status(tc, id0, "200"));
    uint32_t id = h2ct_rt_stream_id(rts[MAX_CONCURRENT + 1]);
    H2CT_TRY(id != 0);
    static const Str want[] = {S_(":authority"), S_("dummy.tld"), S_(":method"),
                               S_("GET"),        S_(":path"),     S_("/3")};
    H2CT_TRY(h2ct_want_headers(tc, id, true, want, 6));
    H2CT_TRY(h2ct_rt_want_status(rts[0], 200));
}

static void TestTransportRequestsStallAtServerLimit(TestingT *t) {
    h2ct_run(t, h2ct_requests_stall_at_server_limit, NULL);
}

typedef struct H2ctFrameSize {
    Int max_read_frame_size;
    uint32_t want;
} H2ctFrameSize;

static void h2ct_max_frame_read_size(H2ctTT *tt, const void *arg) {
    const H2ctFrameSize *test = (const H2ctFrameSize *)arg;
    HttpHTTP2Config *h2 = (HttpHTTP2Config *)mem_alloc(
        arena_allocator(&tt->ar), sizeof *h2, _Alignof(HttpHTTP2Config));
    H2CT_TRY(h2 != NULL);
    memset(h2, 0, sizeof *h2);
    h2->max_read_frame_size = test->max_read_frame_size;
    tt->tr1.http2 = h2;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL);

    Http2Frame *fr = h2ct_read_type(tc, HTTP2_FRAME_SETTINGS);
    H2CT_TRY(fr != NULL);
    uint32_t got = 0;
    if (!burrow__http2_settings_frame_value(fr, HTTP2_SETTING_MAX_FRAME_SIZE, &got))
        testing_t_errorf_v(tt->t,
                           "Transport.MaxReadFrameSize = %d; server got no setting, "
                           "want %d",
                           test->max_read_frame_size, (int64_t)test->want);
    else if (got != test->want)
        testing_t_errorf_v(
            tt->t, "Transport.MaxReadFrameSize = %d; server got %d, want %d",
            test->max_read_frame_size, (int64_t)got, (int64_t)test->want);
    burrow__http2_frame_free(fr);
}

static void TestTransportMaxFrameReadSize_64000(TestingT *t) {
    static const H2ctFrameSize test = {64000, 64000};
    h2ct_run(t, h2ct_max_frame_read_size, &test);
}

/* Setting net/http.Transport.HTTP2Config.MaxReadFrameSize to an out of range
 * value reverts to the default, Go's DefaultMaxReadFrameSize. */
static void TestTransportMaxFrameReadSize_1024(TestingT *t) {
    static const H2ctFrameSize test = {1024, 1U << 20};
    h2ct_run(t, h2ct_max_frame_read_size, &test);
}

static void h2ct_close_after_lost_ping(H2ctTT *tt, const void *arg) {
    (void)arg;
    static const HttpHTTP2Config h2 = {.send_ping_timeout = 1 * TIME_SECOND,
                                       .ping_timeout = 1 * TIME_SECOND};
    tt->tr1.http2 = &h2;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));

    /* A second goes by, and then the PING. */
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_PING));

    /* A second more, with no answer, and the connection is lost. */
    Error err = h2ct_rt_err(rt);
    if (BURROW_OK(err) ||
        !strings_contains(error_text(err), BURROW_S("client connection lost")))
        FATALF("expected to get error about \"connection lost\", got %v", err);
}

static void TestTransportCloseAfterLostPing(TestingT *t) {
    h2ct_run(t, h2ct_close_after_lost_ping, NULL);
}

static void h2ct_send_ping_with_reset(H2ctTT *tt, const void *arg) {
    (void)arg;
    static const HttpHTTP2Config h2 = {.strict_max_concurrent_requests = true};
    tt->tr1.http2 = &h2;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    enum { MAX_CONCURRENT = 3 };
    static const Http2Setting s[] = {
        {HTTP2_SETTING_MAX_CONCURRENT_STREAMS, MAX_CONCURRENT}};
    H2CT_TRY(tc != NULL && h2ct_greet(tc, s, 1));

    /* Start several requests. */
    H2ctRT *rts[MAX_CONCURRENT];
    for (int i = 0; i < MAX_CONCURRENT + 1; i++) {
        H2ctRT *rt = h2ct_tc_round_trip(
            tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
        H2CT_TRY(rt != NULL);
        if (i >= MAX_CONCURRENT) {
            H2CT_TRY(h2ct_want_idle(tc));
            continue;
        }
        H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
        rts[i] = rt;
    }

    /* Cancel one request. We send a PING frame along with the RST_STREAM. */
    uint32_t id0 = h2ct_rt_stream_id(rts[0]);
    H2CT_TRY(id0 != 0);
    BURROW_CALLF0(rts[0]->cancel);
    H2CT_TRY(h2ct_want_rst_stream(tc, id0, HTTP2_ERR_CODE_CANCEL));
    Http2Frame *pf = h2ct_read_type(tc, HTTP2_FRAME_PING);
    H2CT_TRY(pf != NULL);
    Byte data[8];
    memcpy(data, pf->u.ping.data, sizeof data);
    burrow__http2_frame_free(pf);
    H2CT_TRY(h2ct_want_idle(tc));

    /* Cancel another request. No PING frame, since one is in flight. */
    uint32_t id1 = h2ct_rt_stream_id(rts[1]);
    H2CT_TRY(id1 != 0);
    BURROW_CALLF0(rts[1]->cancel);
    H2CT_TRY(h2ct_want_rst_stream(tc, id1, HTTP2_ERR_CODE_CANCEL));
    H2CT_TRY(h2ct_want_idle(tc));

    /* Respond to the PING. This finalizes the previous resets, and allows the
     * pending request to be sent. */
    H2CT_TRY(h2ct_write_ping(tc, true, data));
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    H2CT_TRY(h2ct_want_idle(tc));
}

static void TestTransportSendPingWithReset(TestingT *t) {
    h2ct_run(t, h2ct_send_ping_with_reset, NULL);
}

static void h2ct_no_ping_after_reset_with_frames(H2ctTT *tt, const void *arg) {
    (void)arg;
    static const HttpHTTP2Config h2 = {.strict_max_concurrent_requests = true};
    tt->tr1.http2 = &h2;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    static const Http2Setting s[] = {{HTTP2_SETTING_MAX_CONCURRENT_STREAMS, 1}};
    H2CT_TRY(tc != NULL && h2ct_greet(tc, s, 1));

    /* Start request #1. The server immediately responds with request
     * headers. */
    H2ctRT *rt1 = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt1 != NULL);
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id1 = h2ct_rt_stream_id(rt1);
    H2CT_TRY(id1 != 0);
    static const Str res_kv[] = {S_(":status"), S_("200")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id1, h2ct_block(tc, res_kv, 2), false)));
    H2CT_TRY(h2ct_rt_want_status(rt1, 200));

    /* Start request #2. The connection is at its concurrency limit, so this
     * request is not yet sent. */
    H2ctRT *rt2 = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt2 != NULL);
    H2CT_TRY(h2ct_want_idle(tc));

    /* Cancel request #1. This frees a concurrency slot, and request #2 is
     * sent. */
    BURROW_CALLF0(rt1->cancel);
    H2CT_TRY(h2ct_want_rst_stream(tc, id1, HTTP2_ERR_CODE_CANCEL));
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));

    /* Cancel request #2. We send a PING along with the RST_STREAM, since no
     * frames have been received since this request was sent. */
    uint32_t id2 = h2ct_rt_stream_id(rt2);
    H2CT_TRY(id2 != 0);
    BURROW_CALLF0(rt2->cancel);
    H2CT_TRY(h2ct_want_rst_stream(tc, id2, HTTP2_ERR_CODE_CANCEL));
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_PING));
}

static void TestTransportNoPingAfterResetWithFrames(TestingT *t) {
    h2ct_run(t, h2ct_no_ping_after_reset_with_frames, NULL);
}

static void h2ct_count_header_size(void *env, Str name, Str value) {
    *(uint64_t *)env += (uint64_t)name.len + (uint64_t)value.len + 32;
}

/* headerListSizeForRequest: the size of the header list EncodeHeaders makes
 * for req, with no limit. */
static bool h2ct_header_list_size_for_request(H2ctTT *tt, HttpRequest *req,
                                              uint64_t *size) {
    Http2EncodeHeadersParam p;
    memset(&p, 0, sizeof p);
    p.ctx = context_background();
    p.url = req->url;
    p.method = req->method;
    p.host = req->host;
    p.header = req->header;
    p.trailer = req->trailer;
    p.actual_content_length = req->content_length;
    p.add_gzip_header = true;
    p.peer_max_header_list_size = UINT64_MAX;
    Http2EncodeHeadersResult res;
    *size = 0;
    Error err = burrow__http2_encode_headers(arena_allocator(&tt->ar), &p,
                                             h2ct_count_header_size, size, &res);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(tt->t, "%v", err);
        return false;
    }
    return true;
}

/* newRequest: a POST with a body, so that it can have trailers, and no
 * User-Agent. Go's request is to https://example.tld/. This one goes to
 * dummy.tld, the host the test's connection is for, which changes the sizes
 * but not what they add up to. */
static HttpRequest *h2ct_new_list_size_request(H2ctTT *tt) {
    static const char bodytext[] = "hello";
    StringsReader *body =
        strings_new_reader(arena_allocator(&tt->ar), str_from_cstr(bodytext));
    if (body == NULL) {
        testing_t_fatalf_v(tt->t, "newRequest: no memory for the body");
        return NULL;
    }
    HttpRequest *req = h2ct_new_request(tt, (Context){0}, BURROW_S("POST"),
                                        strings_reader_as_io_reader(body));
    if (req == NULL)
        return NULL;
    req->content_length = (int64_t)strlen(bodytext);
    /* http.Header{"User-Agent": nil}: the key with no values. */
    if (!http_header_add(req->header, BURROW_S("User-Agent"), BURROW_S(""))) {
        testing_t_fatalf_v(tt->t, "newRequest: no memory for the header");
        return NULL;
    }
    Str ua = BURROW_S("User-Agent");
    Slice *vs = (Slice *)map_get(req->header, &ua);
    if (vs != NULL)
        vs->len = 0;
    return req;
}

static HttpHeader h2ct_new_trailer(H2ctTT *tt, HttpRequest *req) {
    req->trailer = http_header_make(arena_allocator(&tt->ar));
    if (req->trailer == NULL)
        testing_t_fatalf_v(tt->t, "no memory for the trailer");
    return req->trailer;
}

/* checkRoundTrip: req fails with ErrRequestHeaderListSize when want_err, and
 * gets a 200 otherwise. */
static bool h2ct_check_header_list_round_trip(H2ctTT *tt, H2ctConn *tc,
                                              HttpRequest *req, bool want_err,
                                              const char *desc) {
    H2ctRT *rt = h2ct_tc_round_trip(tt, req);
    if (rt == NULL)
        return false;
    if (want_err) {
        Error err = h2ct_rt_err(rt);
        if (!errors_is(err, burrow__http2_err_request_header_list_size))
            testing_t_errorf_v(tt->t, "%s: RoundTrip err = %v; want %v",
                               str_from_cstr(desc), err,
                               burrow__http2_err_request_header_list_size);
        return true;
    }

    if (!h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS))
        return false;
    uint32_t id = h2ct_rt_stream_id(rt);
    return id != 0 && h2ct_write_status(tc, id, "200") && h2ct_rt_want_status(rt, 200);
}

static void h2ct_checks_request_header_list_size(H2ctTT *tt, const void *arg) {
    (void)arg;
    enum { PEER_SIZE = 16 << 10 };
    Alloc *a = arena_allocator(&tt->ar);

    H2ctConn *tc = h2ct_new_client_conn(tt);
    static const Http2Setting s[] = {{HTTP2_SETTING_MAX_HEADER_LIST_SIZE, PEER_SIZE}};
    H2CT_TRY(tc != NULL && h2ct_greet(tc, s, 1));

    /* Pad headers & trailers, but stay under peerSize. */
    HttpRequest *req = h2ct_new_list_size_request(tt);
    H2CT_TRY(req != NULL);
    HttpHeader trailer = h2ct_new_trailer(tt, req);
    H2CT_TRY(trailer != NULL);
    Str filler = strings_repeat(a, BURROW_S("*"), 1024);
    H2CT_TRY(h2ct_pad_headers(tt->t, a, trailer, PEER_SIZE, filler));
    /* cc.encodeHeaders adds some default headers to the request, so we need
     * to leave room for those. */
    uint64_t default_bytes = 0;
    H2CT_TRY(h2ct_header_list_size_for_request(tt, req, &default_bytes));
    H2CT_TRY(
        h2ct_pad_headers(tt->t, a, req->header, PEER_SIZE - default_bytes, filler));
    H2CT_TRY(h2ct_check_header_list_round_trip(tt, tc, req, false,
                                               "Headers & Trailers under limit"));

    /* Add enough header bytes to push us over peerSize. */
    req = h2ct_new_list_size_request(tt);
    H2CT_TRY(req != NULL);
    H2CT_TRY(h2ct_pad_headers(tt->t, a, req->header, PEER_SIZE, filler));
    H2CT_TRY(
        h2ct_check_header_list_round_trip(tt, tc, req, true, "Headers over limit"));

    /* Push trailers over the limit. */
    req = h2ct_new_list_size_request(tt);
    H2CT_TRY(req != NULL);
    trailer = h2ct_new_trailer(tt, req);
    H2CT_TRY(trailer != NULL);
    H2CT_TRY(h2ct_pad_headers(tt->t, a, trailer, PEER_SIZE + 1, filler));
    H2CT_TRY(
        h2ct_check_header_list_round_trip(tt, tc, req, true, "Trailers over limit"));

    /* Send headers with a single large value. */
    req = h2ct_new_list_size_request(tt);
    H2CT_TRY(req != NULL);
    filler = strings_repeat(a, BURROW_S("*"), PEER_SIZE);
    H2CT_TRY(http_header_set(req->header, BURROW_S("Big"), filler));
    H2CT_TRY(
        h2ct_check_header_list_round_trip(tt, tc, req, true, "Single large header"));

    /* Send trailers with a single large value. */
    req = h2ct_new_list_size_request(tt);
    H2CT_TRY(req != NULL);
    trailer = h2ct_new_trailer(tt, req);
    H2CT_TRY(trailer != NULL);
    H2CT_TRY(http_header_set(trailer, BURROW_S("Big"), filler));
    H2CT_TRY(
        h2ct_check_header_list_round_trip(tt, tc, req, true, "Single large trailer"));
}

static void TestTransportChecksRequestHeaderListSize(TestingT *t) {
    h2ct_run(t, h2ct_checks_request_header_list_size, NULL);
}

/* tc.cc.Close(). */
static void h2ct_cc_close(H2ctTT *tt, H2ctConn *tc) {
    burrow__http2_transport_close_conn(tt->t2, (NetConn){&h2ct_cli_vt, tc});
}

static void h2ct_client_conn_close_at_headers(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    HttpRequest *req = h2ct_new_request_url(
        tt, (Context){0}, BURROW_S("GET"), BURROW_S("http://dummy.tld/"), h2ct_no_body);
    H2CT_TRY(req != NULL);
    H2ctRT *rt = h2ct_tc_round_trip(tt, req);
    H2CT_TRY(rt != NULL && h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));

    h2ct_cc_close(tt, tc);
    Error err = h2ct_rt_err(rt);
    if (!errors_is(err, burrow__http2_err_client_conn_force_closed))
        FATALF("RoundTrip error = %v, want errClientConnForceClosed", err);
}

static void TestClientConnCloseAtHeaders(TestingT *t) {
    h2ct_run(t, h2ct_client_conn_close_at_headers, NULL);
}

static void h2ct_client_conn_close_at_body(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    HttpRequest *req = h2ct_new_request_url(
        tt, (Context){0}, BURROW_S("GET"), BURROW_S("http://dummy.tld/"), h2ct_no_body);
    H2CT_TRY(req != NULL);
    H2ctRT *rt = h2ct_tc_round_trip(tt, req);
    H2CT_TRY(rt != NULL && h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));

    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    Str kv[] = {BURROW_S(":status"), BURROW_S("200")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, kv, 2), false)));
    H2CT_TRY(h2ct_write_zeros(tc, id, false, 64));
    H2CT_TRY(h2ct_rt_response(rt) != NULL);
    h2ct_cc_close(tt, tc);

    Error err = BURROW_NO_ERROR;
    (void)h2ct_rt_read_body(rt, &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(tt->t, "expected a Copy error, got nil");
}

static void TestClientConnCloseAtBody(TestingT *t) {
    h2ct_run(t, h2ct_client_conn_close_at_body, NULL);
}

static void h2ct_flow_control(H2ctTT *tt, const void *arg) {
    (void)arg;
    enum { MAX_BUFFER = 64 << 10 }; /* 64KiB */
    static const HttpHTTP2Config h2 = {.max_receive_buffer_per_connection = MAX_BUFFER,
                                       .max_receive_buffer_per_stream = MAX_BUFFER,
                                       .max_read_frame_size = 16 << 20}; /* 16MiB */
    tt->tr1.http2 = &h2;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    HttpRequest *req = h2ct_new_request_url(
        tt, (Context){0}, BURROW_S("GET"), BURROW_S("http://dummy.tld/"), h2ct_no_body);
    H2CT_TRY(req != NULL);
    H2ctRT *rt = h2ct_tc_round_trip(tt, req);
    H2CT_TRY(rt != NULL && h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));

    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    Str kv[] = {BURROW_S(":status"), BURROW_S("200")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, kv, 2), false)));
    H2CT_TRY(h2ct_rt_want_status(rt, 200));

    /* Server fills up its transmit buffer. The client does not provide more
     * flow control tokens, since the data hasn't been consumed by the user. */
    H2CT_TRY(h2ct_write_zeros(tc, id, false, MAX_BUFFER));
    H2CT_TRY(h2ct_want_idle(tc));

    /* User reads data from the response body. The client sends more flow
     * control tokens. */
    HttpResponse *res = h2ct_rt_response(rt);
    H2CT_TRY(res != NULL);
    Slice buf = slice_make(arena_allocator(&tt->ar), TYPE_BYTE, MAX_BUFFER, MAX_BUFFER);
    H2CT_TRY(buf.p != NULL);
    Error err = BURROW_NO_ERROR;
    (void)io_read_full(io_read_closer_as_io_reader(res->body), buf, &err);
    if (BURROW_FAILED(err))
        FATALF("io.Body.Read: %v", err);
    uint32_t conn_tokens = 0;
    uint32_t stream_tokens = 0;
    for (;;) {
        Http2Frame *f = h2ct_read_frame_within(tc, H2CT_QUIET, &err);
        if (BURROW_FAILED(err)) {
            burrow__http2_frame_free(f);
            if (!errors_is(err, os_err_deadline_exceeded))
                FATALF("ReadFrame: %v", err);
            break;
        }
        if (f->header.type != HTTP2_FRAME_WINDOW_UPDATE) {
            int type = (int)f->header.type;
            burrow__http2_frame_free(f);
            FATALF("received unexpected frame type %d (want WINDOW_UPDATE)", type);
        }
        /* Go's switch has wu.StreamID as its second case, which every stream
         * matches. */
        if (f->header.stream_id == 0)
            conn_tokens += f->u.window_update.increment;
        else
            stream_tokens += f->u.window_update.increment;
        burrow__http2_frame_free(f);
    }
    if (conn_tokens != (uint32_t)MAX_BUFFER)
        testing_t_errorf_v(tt->t,
                           "transport provided %d bytes of connection WINDOW_UPDATE, "
                           "want %d",
                           (int)conn_tokens, (int)MAX_BUFFER);
    if (stream_tokens != (uint32_t)MAX_BUFFER)
        testing_t_errorf_v(
            tt->t, "transport provided %d bytes of stream WINDOW_UPDATE, want %d",
            (int)stream_tokens, (int)MAX_BUFFER);
}

static void TestTransportFlowControl(TestingT *t) {
    h2ct_run(t, h2ct_flow_control, NULL);
}

static void h2ct_body_read_error(H2ctTT *tt, const void *arg) {
    Str body = *(const Str *)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    Error body_read_error =
        errors_new(arena_allocator(&tt->ar), BURROW_S("body read error"));
    H2ctBody *b = h2ct_new_request_body(tt);
    H2CT_TRY(b != NULL);
    h2ct_body_write(b, body);
    h2ct_body_close_with_error(b, body_read_error);
    HttpRequest *req = h2ct_new_request_url(tt, (Context){0}, BURROW_S("PUT"),
                                            BURROW_S("http://dummy.tld/"),
                                            (IoReader){&h2ct_body_vt, b});
    H2CT_TRY(req != NULL);
    H2ctRT *rt = h2ct_tc_round_trip(tt, req);
    H2CT_TRY(rt != NULL && h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));

    BytesBuffer received = BYTES_BUFFER(arena_allocator(&tt->ar));
    for (;;) {
        Http2Frame *f = h2ct_read_frame(tc);
        if (f == NULL)
            FATALF("transport is idle, want RST_STREAM");
        Http2FrameType type = f->header.type;
        if (type == HTTP2_FRAME_DATA) {
            Error err = BURROW_NO_ERROR;
            (void)bytes_buffer_write(&received, f->u.data.data, &err);
        }
        burrow__http2_frame_free(f);
        if (type == HTTP2_FRAME_RST_STREAM)
            break;
        if (type != HTTP2_FRAME_DATA)
            FATALF("unexpected frame: type %d", (int)type);
    }
    Str got = bytes_buffer_string(&received, arena_allocator(&tt->ar));
    if (!str_eq(got, body))
        FATALF("body: %q; expected %q", got, body);

    Error err = h2ct_rt_err(rt);
    if (!errors_is(err, body_read_error))
        FATALF("err = %v; want %v", err, body_read_error);
}

static void TestTransportBodyReadError_Immediately(TestingT *t) {
    static const Str body = BURROW_S_INIT("");
    h2ct_run(t, h2ct_body_read_error, &body);
}

static void TestTransportBodyReadError_Some(TestingT *t) {
    static const Str body = BURROW_S_INIT("123");
    h2ct_run(t, h2ct_body_read_error, &body);
}

typedef struct H2ct1xxLimits {
    int64_t max_response_header_bytes;
    Int hcount;
    bool trace;       /* with a Got1xxResponse hook */
    bool trace_limit; /* which fails the tenth 1xx response */
    bool limited;
} H2ct1xxLimits;

static Error h2ct_got1xx_count(void *env, Int code, TextprotoMIMEHeader header) {
    (void)code;
    (void)header;
    int *count = (int *)env;
    (*count)++;
    if (*count >= 10)
        return errors_new(error_allocator(), BURROW_S("too many 1xx"));
    return BURROW_NO_ERROR;
}

static Error h2ct_got1xx_nil(void *env, Int code, TextprotoMIMEHeader header) {
    (void)env;
    (void)code;
    (void)header;
    return BURROW_NO_ERROR;
}

static void h2ct_1xx_limits(H2ctTT *tt, const void *arg) {
    const H2ct1xxLimits *test = (const H2ct1xxLimits *)arg;
    Alloc *a = arena_allocator(&tt->ar);
    tt->tr1.max_response_header_bytes = test->max_response_header_bytes;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    Context ctx = context_background();
    if (test->trace) {
        HttptraceClientTrace *trace = (HttptraceClientTrace *)mem_alloc(
            a, sizeof *trace, _Alignof(HttptraceClientTrace));
        int *count = (int *)mem_alloc(a, sizeof *count, _Alignof(int));
        if (trace == NULL || count == NULL)
            FATALF("no memory for the trace");
        memset(trace, 0, sizeof *trace);
        *count = 0;
        trace->got1xx_response =
            test->trace_limit
                ? BURROW_FN(HttptraceGot1xxResponseFunc, h2ct_got1xx_count, count)
                : BURROW_FN(HttptraceGot1xxResponseFunc, h2ct_got1xx_nil, NULL);
        ctx = httptrace_with_client_trace(a, ctx, trace);
    }
    HttpRequest *req = h2ct_new_request_url(
        tt, ctx, BURROW_S("GET"), BURROW_S("http://dummy.tld/"), h2ct_no_body);
    H2CT_TRY(req != NULL);
    H2ctRT *rt = h2ct_tc_round_trip(tt, req);
    H2CT_TRY(rt != NULL && h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);

    Str field = strings_repeat(a, BURROW_S("a"), 1000);
    for (Int i = 0; i < test->hcount; i++) {
        Error err = BURROW_NO_ERROR;
        Http2Frame *f = h2ct_read_frame_within(tc, H2CT_QUIET, &err);
        if (!errors_is(err, os_err_deadline_exceeded)) {
            int type = f != NULL ? (int)f->header.type : -1;
            burrow__http2_frame_free(f);
            FATALF("after writing %d 1xx headers: read frame type %d, %v; want idle",
                   (int)i, type, err);
        }
        burrow__http2_frame_free(f);
        Str kv[] = {BURROW_S(":status"), BURROW_S("103"), BURROW_S("x-field"), field};
        H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, kv, 4), false)));
    }
    if (test->limited)
        (void)h2ct_want_frame_type(tc, HTTP2_FRAME_RST_STREAM);
    else
        (void)h2ct_want_idle(tc);
}

static void TestTransport1xxLimits_default(TestingT *t) {
    static const H2ct1xxLimits test = {.hcount = 10, .limited = false};
    h2ct_run(t, h2ct_1xx_limits, &test);
}

static void TestTransport1xxLimits_MaxResponseHeaderBytes(TestingT *t) {
    static const H2ct1xxLimits test = {
        .max_response_header_bytes = 10000, .hcount = 10, .limited = true};
    h2ct_run(t, h2ct_1xx_limits, &test);
}

static void TestTransport1xxLimits_limit_by_client_trace(TestingT *t) {
    static const H2ct1xxLimits test = {
        .hcount = 10, .trace = true, .trace_limit = true, .limited = true};
    h2ct_run(t, h2ct_1xx_limits, &test);
}

static void TestTransport1xxLimits_limit_disabled_by_client_trace(TestingT *t) {
    static const H2ct1xxLimits test = {.max_response_header_bytes = 10000,
                                       .hcount = 20,
                                       .trace = true,
                                       .limited = false};
    h2ct_run(t, h2ct_1xx_limits, &test);
}

/* Go waits 5ms in a bubble and looks at 4ms. The clock here is real, so the
 * timeout is long enough for a loaded machine to read the body in, and the
 * round trip is looked at as soon as it has been. */
static void h2ct_response_header_timeout(H2ctTT *tt, const void *arg) {
    bool body = *(const bool *)arg;
    enum { BODY_SIZE = 4 << 20 };
    tt->tr1.response_header_timeout = 2 * TIME_SECOND;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    HttpRequest *req = NULL;
    if (body) {
        H2ctBody *req_body = h2ct_new_request_body(tt);
        H2CT_TRY(req_body != NULL);
        h2ct_body_write_bytes(req_body, BODY_SIZE);
        h2ct_body_close_with_error(req_body, io_eof);
        req = h2ct_new_request_url(tt, (Context){0}, BURROW_S("POST"),
                                   BURROW_S("http://dummy.tld/"),
                                   (IoReader){&h2ct_body_vt, req_body});
        H2CT_TRY(req != NULL);
        http_header_set(req->header, BURROW_S("Content-Type"), BURROW_S("text/foo"));
    } else {
        req = h2ct_new_request_url(tt, (Context){0}, BURROW_S("GET"),
                                   BURROW_S("http://dummy.tld/"), h2ct_no_body);
        H2CT_TRY(req != NULL);
    }

    H2ctRT *rt = h2ct_tc_round_trip(tt, req);
    H2CT_TRY(rt != NULL && h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);

    H2CT_TRY(h2ct_write_window_update(tc, 0, BODY_SIZE));
    H2CT_TRY(h2ct_write_window_update(tc, id, BODY_SIZE));

    if (body)
        H2CT_TRY(h2ct_want_data(tc, id, true, BODY_SIZE, NULL, true));

    if (sync_atomic_bool_load(&rt->done))
        FATALF("RoundTrip is done before the timeout; want still waiting");

    Error err = h2ct_rt_err(rt);
    if (!os_is_timeout(err))
        FATALF("RoundTrip error: %v; want timeout error", err);
}

static void TestTransportResponseHeaderTimeout_NoBody(TestingT *t) {
    static const bool body = false;
    h2ct_run(t, h2ct_response_header_timeout, &body);
}

static void TestTransportResponseHeaderTimeout_Body(TestingT *t) {
    static const bool body = true;
    h2ct_run(t, h2ct_response_header_timeout, &body);
}

static void h2ct_do_not_hang_on_zero_max_frame_size(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL);
    static const Http2Setting s[] = {{HTTP2_SETTING_MAX_FRAME_SIZE, 0}};
    H2CT_TRY(h2ct_write_settings(tc, s, 1));
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_SETTINGS));

    StringsReader *body =
        strings_new_reader(arena_allocator(&tt->ar), BURROW_S("body"));
    H2CT_TRY(body != NULL);
    HttpRequest *req = h2ct_new_request_url(tt, (Context){0}, BURROW_S("POST"),
                                            BURROW_S("http://dummy.tld/"),
                                            strings_reader_as_io_reader(body));
    H2CT_TRY(req != NULL);
    (void)h2ct_tc_round_trip(tt, req);
    /* Previously, https://go.dev/issue/78476 caused an infinite hang here. */
}

static void TestTransportDoNotHangOnZeroMaxFrameSize(TestingT *t) {
    h2ct_run(t, h2ct_do_not_hang_on_zero_max_frame_size, NULL);
}

/* makeAndResetRequest: a request the client sends and then cancels. */
static bool h2ct_make_and_reset_request(H2ctTT *tt, H2ctConn *tc) {
    HttpRequest *req = h2ct_new_request_url(
        tt, (Context){0}, BURROW_S("GET"), BURROW_S("http://dummy.tld/"), h2ct_no_body);
    if (req == NULL)
        return false;
    H2ctRT *rt = h2ct_tc_round_trip(tt, req);
    if (rt == NULL || !h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS))
        return false;
    uint32_t id = h2ct_rt_stream_id(rt);
    if (id == 0)
        return false;
    BURROW_CALLF0(rt->cancel);
    /* client sends RST_STREAM */
    return h2ct_want_rst_stream(tc, id, HTTP2_ERR_CODE_CANCEL);
}

static void h2ct_send_no_more_than_one_ping_with_reset(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    /* Create a request and cancel it. The client sends a PING frame along
     * with the reset. */
    H2CT_TRY(h2ct_make_and_reset_request(tt, tc));
    Http2Frame *pf1 = h2ct_read_type(tc, HTTP2_FRAME_PING); /* client sends PING */
    H2CT_TRY(pf1 != NULL);
    Byte data[8];
    memcpy(data, pf1->u.ping.data, sizeof data);
    burrow__http2_frame_free(pf1);
    H2CT_TRY(h2ct_want_idle(tc));

    /* Create another request and cancel it. We do not send a PING frame along
     * with the reset, because we haven't received a HEADERS or DATA frame from
     * the server since the last PING we sent. */
    H2CT_TRY(h2ct_make_and_reset_request(tt, tc));
    H2CT_TRY(h2ct_want_idle(tc));

    /* Server belatedly responds to request 1. The server has not responded to
     * our first PING yet. */
    H2CT_TRY(h2ct_write_status(tc, 1, "200"));
    H2CT_TRY(h2ct_want_idle(tc));

    /* Create yet another request and cancel it. We still do not send a PING
     * frame along with the reset. We've received a HEADERS frame, but it came
     * before the response to the PING. */
    H2CT_TRY(h2ct_make_and_reset_request(tt, tc));
    H2CT_TRY(h2ct_want_idle(tc));

    /* The server responds to our PING. */
    H2CT_TRY(h2ct_write_ping(tc, true, data));
    H2CT_TRY(h2ct_want_idle(tc));

    /* Create yet another request and cancel it. Still no PING frame; we got a
     * response to the previous one, but no HEADERS or DATA. */
    H2CT_TRY(h2ct_make_and_reset_request(tt, tc));
    H2CT_TRY(h2ct_want_idle(tc));

    /* Server belatedly responds to the second request. */
    H2CT_TRY(h2ct_write_status(tc, 3, "200"));
    H2CT_TRY(h2ct_want_idle(tc));

    /* One more request. This time we send a PING frame. */
    H2CT_TRY(h2ct_make_and_reset_request(tt, tc));
    (void)h2ct_want_frame_type(tc, HTTP2_FRAME_PING);
}

static void TestTransportSendNoMoreThanOnePingWithReset(TestingT *t) {
    h2ct_run(t, h2ct_send_no_more_than_one_ping_with_reset, NULL);
}

/* A new connection's first request, which the server answers with 200 after
 * limiting the connection to max_concurrent streams. */
static bool h2ct_first_request_ok(H2ctConn *tc, H2ctRT *rt, uint32_t max_concurrent) {
    if (!h2ct_want_frame_type(tc, HTTP2_FRAME_SETTINGS) ||
        !h2ct_want_frame_type(tc, HTTP2_FRAME_WINDOW_UPDATE))
        return false;
    Http2Frame *hf = h2ct_read_type(tc, HTTP2_FRAME_HEADERS);
    if (hf == NULL)
        return false;
    uint32_t id = hf->header.stream_id;
    burrow__http2_frame_free(hf);
    Http2Setting s[1];
    s[0].id = HTTP2_SETTING_MAX_CONCURRENT_STREAMS;
    s[0].val = max_concurrent;
    if (!h2ct_write_settings(tc, s, 1) ||
        !h2ct_want_frame_type(tc, HTTP2_FRAME_SETTINGS) /* ack */ ||
        !h2ct_write_status(tc, id, "200") || !h2ct_rt_want_status(rt, 200))
        return false;
    HttpResponse *res = h2ct_rt_response(rt);
    (void)res->body.vt->closer.close(res->body.data);
    return true;
}

/* We send a number of requests in series to an unresponsive connection. Each
 * request is canceled or times out without a response. Eventually, we open a
 * new connection rather than trying to use the old one. */
static void h2ct_conn_becomes_unresponsive(H2ctTT *tt, const void *arg) {
    (void)arg;
    enum { MAX_CONCURRENT = 3 };

    testing_t_logf_v(tt->t, "first request opens a new connection and succeeds");
    H2ctRT *rt1 = h2ct_tt_round_trip(
        tt, h2ct_new_request_url(tt, (Context){0}, BURROW_S("GET"),
                                 BURROW_S("http://dummy.tld/"), h2ct_no_body));
    H2CT_TRY(rt1 != NULL);
    H2ctConn *tc1 = h2ct_get_conn(tt);
    H2CT_TRY(tc1 != NULL && h2ct_first_request_ok(tc1, rt1, MAX_CONCURRENT));

    /* Send more requests. None receive a response. Each is canceled. */
    for (int i = 0; i < MAX_CONCURRENT; i++) {
        testing_t_logf_v(tt->t, "request %d receives no response and is canceled", i);
        H2ctRT *rt = h2ct_tt_round_trip(
            tt, h2ct_new_request_url(tt, (Context){0}, BURROW_S("GET"),
                                     BURROW_S("http://dummy.tld/"), h2ct_no_body));
        H2CT_TRY(rt != NULL);
        if (h2ct_tt_has_conn(tt))
            FATALF("new connection created; expect existing conn to be reused");
        H2CT_TRY(h2ct_want_frame_type(tc1, HTTP2_FRAME_HEADERS));
        BURROW_CALLF0(rt->cancel);
        H2CT_TRY(h2ct_want_frame_type(tc1, HTTP2_FRAME_RST_STREAM));
        if (i == 0)
            H2CT_TRY(h2ct_want_frame_type(tc1, HTTP2_FRAME_PING));
        H2CT_TRY(h2ct_want_idle(tc1));
    }

    /* The conn has hit its concurrency limit. The next request is sent on a
     * new conn. */
    H2ctRT *rt2 = h2ct_tt_round_trip(
        tt, h2ct_new_request_url(tt, (Context){0}, BURROW_S("GET"),
                                 BURROW_S("http://dummy.tld/"), h2ct_no_body));
    H2CT_TRY(rt2 != NULL);
    H2ctConn *tc2 = h2ct_get_conn(tt);
    H2CT_TRY(tc2 != NULL && h2ct_first_request_ok(tc2, rt2, MAX_CONCURRENT));
}

static void TestTransportConnBecomesUnresponsive(TestingT *t) {
    h2ct_run(t, h2ct_conn_becomes_unresponsive, NULL);
}

typedef struct H2ctIdleConnTimeout {
    Duration idle_conn_timeout;
    Duration wait;
    bool want_new_conn;
} H2ctIdleConnTimeout;

/* Go waits in a bubble. These are real waits, as long as Go's. */
static void h2ct_idle_conn_timeout(H2ctTT *tt, const void *arg) {
    const H2ctIdleConnTimeout *test = (const H2ctIdleConnTimeout *)arg;
    tt->tr1.idle_conn_timeout = test->idle_conn_timeout;
    H2ctConn *tc = NULL;
    for (int i = 0; i < 3; i++) {
        H2ctRT *rt = h2ct_tt_round_trip(
            tt, h2ct_new_request_url(tt, (Context){0}, BURROW_S("GET"),
                                     BURROW_S("http://dummy.tld/"), h2ct_no_body));
        H2CT_TRY(rt != NULL);

        /* This request happens on a new conn if it's the first request (and
         * there is no cached conn), or if the test timeout is long enough
         * that old conns are being closed. */
        bool want_conn = i == 0 || test->want_new_conn;
        bool has = h2ct_tt_has_conn(tt);
        if (has != want_conn)
            FATALF("request %d: hasConn=%v, want %v", i, has, want_conn);
        if (want_conn) {
            tc = h2ct_get_conn(tt);
            /* Read client's SETTINGS and first WINDOW_UPDATE, send our
             * SETTINGS. */
            H2CT_TRY(tc != NULL && h2ct_want_frame_type(tc, HTTP2_FRAME_SETTINGS) &&
                     h2ct_want_frame_type(tc, HTTP2_FRAME_WINDOW_UPDATE) &&
                     h2ct_write_settings(tc, NULL, 0));
        }
        if (h2ct_tt_has_conn(tt))
            FATALF("request %d: Transport has more than one conn", i);

        /* Respond to the client's request. */
        Http2Frame *hf = h2ct_read_type(tc, HTTP2_FRAME_HEADERS);
        H2CT_TRY(hf != NULL);
        uint32_t id = hf->header.stream_id;
        burrow__http2_frame_free(hf);
        H2CT_TRY(h2ct_write_status(tc, id, "200"));
        H2CT_TRY(h2ct_rt_want_status(rt, 200));

        /* If this was a newly-accepted conn, read the SETTINGS ACK. */
        if (want_conn)
            H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_SETTINGS)); /* ACK */

        time_sleep(test->wait);
        /* is_closed waits for the close, which is only worth it when one is
         * wanted. */
        bool got = test->want_new_conn ? h2ct_is_closed(tc)
                                       : sync_atomic_bool_load(&tc->cli_closed);
        if (got != test->want_new_conn)
            FATALF("after waiting %s, conn closed=%v; want %v",
                   duration_string(test->wait, arena_allocator(&tt->ar)), got,
                   test->want_new_conn);
    }
}

/* Go's timeout is 2s. net/http keeps an HTTP/2 connection in its pool from
 * when it was first put there, and only uses it within the timeout of that.
 * Go's third request comes exactly 2s after on its fake clock, which is still
 * within it, and here it always comes later, so the timeout is 4s. */
static void TestIdleConnTimeout_NoExpiry(TestingT *t) {
    static const H2ctIdleConnTimeout test = {4 * TIME_SECOND, 1 * TIME_SECOND, false};
    h2ct_run(t, h2ct_idle_conn_timeout, &test);
}

static void TestIdleConnTimeout_H2TransportTimeoutExpires(TestingT *t) {
    static const H2ctIdleConnTimeout test = {1 * TIME_SECOND, 2 * TIME_SECOND, true};
    h2ct_run(t, h2ct_idle_conn_timeout, &test);
}

/* Go gives this case a base transport with an IdleConnTimeout of 2s, which
 * the test never uses. */
static void TestIdleConnTimeout_H1TransportTimeoutExpires(TestingT *t) {
    static const H2ctIdleConnTimeout test = {0, 1 * TIME_SECOND, false};
    h2ct_run(t, h2ct_idle_conn_timeout, &test);
}

static void h2ct_req_body_after_response(H2ctTT *tt, const void *arg) {
    Int status = *(const Int *)arg;
    enum { BODY_SIZE = 1 << 10 };
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctBody *body = h2ct_new_request_body(tt);
    H2CT_TRY(body != NULL);
    h2ct_body_write_bytes(body, BODY_SIZE / 2);
    HttpRequest *req = h2ct_new_request_url(tt, (Context){0}, BURROW_S("PUT"),
                                            BURROW_S("http://dummy.tld/"),
                                            (IoReader){&h2ct_body_vt, body});
    H2CT_TRY(req != NULL);
    H2ctRT *rt = h2ct_tc_round_trip(tt, req);
    H2CT_TRY(rt != NULL);
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);

    static const Str want[] = {S_(":authority"), S_("dummy.tld"), S_(":method"),
                               S_("PUT"),        S_(":path"),     S_("/")};
    H2CT_TRY(h2ct_want_headers(tc, id, false, want, 6));

    /* Provide enough congestion window for the full request body. */
    H2CT_TRY(h2ct_write_window_update(tc, 0, BODY_SIZE));
    H2CT_TRY(h2ct_write_window_update(tc, id, BODY_SIZE));

    H2CT_TRY(h2ct_want_data(tc, id, false, BODY_SIZE / 2, NULL, false));

    Str code = fmt_sprintf_v(arena_allocator(&tt->ar), "%d", status);
    Str kv[] = {BURROW_S(":status"), code};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, kv, 2), true)));

    HttpResponse *res = h2ct_rt_response(rt);
    H2CT_TRY(res != NULL);
    if (res->status_code != status)
        FATALF("status code = %d; want %d", (int)res->status_code, (int)status);

    h2ct_body_write_bytes(body, BODY_SIZE / 2);
    h2ct_body_close_with_error(body, io_eof);

    if (status == 200) {
        /* After a 200 response, client sends the remaining request body. */
        H2CT_TRY(h2ct_want_data(tc, id, true, BODY_SIZE / 2, NULL, true));
    } else {
        /* After a 403 response, client gives up and resets the stream. */
        H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_RST_STREAM));
    }

    (void)h2ct_rt_want_body(rt, "");
}

static void TestTransportReqBodyAfterResponse_200(TestingT *t) {
    static const Int status = 200;
    h2ct_run(t, h2ct_req_body_after_response, &status);
}

static void TestTransportReqBodyAfterResponse_403(TestingT *t) {
    static const Int status = 403;
    h2ct_run(t, h2ct_req_body_after_response, &status);
}

static void h2ct_record_path(void *env, Str name, Str value) {
    if (str_eq(name, BURROW_S(":path")))
        *(Str *)env = value;
}

/* Go encodes the headers with hpack and decodes them again to find :path.
 * This takes :path as it is encoded. */
static void TestTransportRequestPathPseudo(TestingT *t) {
    static const struct {
        const char *method;
        const char *host; /* Request.Host */
        const char *scheme;
        const char *opaque;
        const char *url_host;
        const char *path;
        const char *want_path;
        const char *want_err;
    } tests[] = {
        {"GET", "", "", "", "foo.com", "/foo", "/foo", ""},
        /* In Go 1.7, we accepted paths of "//foo". In Go 1.8, we rejected it
         * (issue 16847). In Go 1.9, we accepted it again (issue 19103). */
        {"GET", "", "", "", "foo.com", "//foo", "//foo", ""},
        /* Opaque with //$Matching_Hostname/path */
        {"GET", "", "https", "//foo.com/path", "foo.com", "/ignored", "/path", ""},
        /* Opaque with some other Request.Host instead: */
        {"GET", "bar.com", "https", "//bar.com/path", "foo.com", "/ignored", "/path",
         ""},
        /* Opaque without the leading "//": */
        {"GET", "", "", "/path", "foo.com", "/ignored", "/path", ""},
        /* Opaque we can't handle: */
        {"GET", "", "https", "//unknown_host/path", "foo.com", "/ignored", "",
         "invalid request :path \"https://unknown_host/path\" from URL.Opaque = "
         "\"//unknown_host/path\""},
        /* A CONNECT request: */
        {"CONNECT", "", "", "", "foo.com", "", "", ""},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Url u;
        memset(&u, 0, sizeof u);
        u.scheme = str_from_cstr(tests[i].scheme);
        u.opaque = str_from_cstr(tests[i].opaque);
        u.host = str_from_cstr(tests[i].url_host);
        u.path = str_from_cstr(tests[i].path);
        Http2EncodeHeadersParam p;
        memset(&p, 0, sizeof p);
        p.ctx = context_background();
        p.url = &u;
        p.method = str_from_cstr(tests[i].method);
        p.host = str_from_cstr(tests[i].host);
        p.add_gzip_header = false;
        p.peer_max_header_list_size = UINT64_MAX;
        Http2EncodeHeadersResult res;
        Str got_path = BURROW_STR_EMPTY;
        Error err = burrow__http2_encode_headers(arena_allocator(&ar), &p,
                                                 h2ct_record_path, &got_path, &res);
        Str got_err = BURROW_STR_EMPTY;
        if (BURROW_FAILED(err)) {
            got_err = error_text(err);
            got_path = BURROW_STR_EMPTY;
        }
        Str want_path = str_from_cstr(tests[i].want_path);
        Str want_err = str_from_cstr(tests[i].want_err);
        if (!str_eq(got_path, want_path) || !str_eq(got_err, want_err))
            testing_t_errorf_v(t, "%d. got {path:%s err:%s}; want {path:%s err:%s}",
                               (int)i, got_path, got_err, want_path, want_err);
        arena_free(&ar);
    }
}

static void h2ct_round_trip_doesnt_consume_request_body_early(H2ctTT *tt,
                                                              const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));
    h2ct_close_write(tc);
    /* Go waits for the bubble to go quiet. This waits for the client to see
     * the end of the conn and close it. */
    H2CT_TRY(h2ct_is_closed(tc));

    static const char body[] = "foo";
    StringsReader *sr = strings_new_reader(arena_allocator(&tt->ar), BURROW_S("foo"));
    H2CT_TRY(sr != NULL);
    HttpRequest *req = h2ct_new_request_url(tt, (Context){0}, BURROW_S("POST"),
                                            BURROW_S("http://foo.com/"),
                                            strings_reader_as_io_reader(sr));
    H2CT_TRY(req != NULL);
    H2ctRT *rt = h2ct_tc_round_trip(tt, req);
    H2CT_TRY(rt != NULL);
    Error err = h2ct_rt_err(rt);
    if (!errors_is(err, burrow__http2_err_client_conn_not_established))
        FATALF("RoundTrip = %v; want errClientConnNotEstablished", err);

    Error rerr = BURROW_NO_ERROR;
    Slice slurp = io_read_all(arena_allocator(&tt->ar),
                              io_read_closer_as_io_reader(req->body), &rerr);
    if (BURROW_FAILED(rerr))
        testing_t_errorf_v(tt->t, "ReadAll = %v", rerr);
    Str got = str_from_bytes(slurp.p, slurp.len);
    if (!str_eq(got, str_from_cstr(body)))
        testing_t_errorf_v(tt->t, "Body = %q; want %q", got, str_from_cstr(body));
}

static void TestRoundTripDoesntConsumeRequestBodyEarly(TestingT *t) {
    h2ct_run(t, h2ct_round_trip_doesnt_consume_request_body_early, NULL);
}

/* Go's closeWriteWithError also makes the client's reads end. That is left
 * out here: the client closes the conn once a write fails, which ends its
 * reads, and an end it reads first could fail the request before the write
 * error does. */
static void h2ct_roundtrip_close_on_write_error(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctBody *body = h2ct_new_request_body(tt);
    H2CT_TRY(body != NULL);
    h2ct_body_write_bytes(body, 1);
    HttpRequest *req = h2ct_new_request_url(tt, (Context){0}, BURROW_S("GET"),
                                            BURROW_S("http://dummy.tld/"),
                                            (IoReader){&h2ct_body_vt, body});
    H2CT_TRY(req != NULL);
    H2ctRT *rt = h2ct_tc_round_trip(tt, req);
    H2CT_TRY(rt != NULL);

    Error write_err = errors_new(arena_allocator(&tt->ar), BURROW_S("write error"));
    sync_mutex_lock(&tc->rmu);
    tc->cli_werr = write_err;
    sync_mutex_unlock(&tc->rmu);

    h2ct_body_write_bytes(body, 1);
    Error err = h2ct_rt_err(rt);
    if (!errors_is(err, write_err))
        FATALF("RoundTrip error %v, want %v", err, write_err);

    /* The first round trip has req, so the second takes a copy, as Go's
     * roundTrip makes with WithContext. */
    HttpRequest *req2 =
        http_request_with_context(req, tt->a, http_request_context(req));
    H2CT_TRY(req2 != NULL);
    H2ctRT *rt2 = h2ct_tc_round_trip(tt, req2);
    H2CT_TRY(rt2 != NULL);
    err = h2ct_rt_err(rt2);
    if (!errors_is(err, burrow__http2_err_client_conn_unusable))
        FATALF("RoundTrip error %v, want errClientConnUnusable", err);
}

static void TestTransportRoundtripCloseOnWriteError(TestingT *t) {
    h2ct_run(t, h2ct_roundtrip_close_on_write_error, NULL);
}

typedef enum H2ctBlockingWrite {
    H2CT_BLOCK_HEADERS,
    H2CT_BLOCK_BODY,
    H2CT_BLOCK_TRAILER,
} H2ctBlockingWrite;

/* Go's filler is the hex of 2048 random bytes. This is as long, and hpack
 * shrinks it no more. */
static Str h2ct_blocking_filler(H2ctTT *tt) {
    return strings_repeat(arena_allocator(&tt->ar), BURROW_S("0123456789abcdef"), 256);
}

static HttpRequest *h2ct_blocking_request(H2ctTT *tt, H2ctBlockingWrite kind) {
    Str filler = h2ct_blocking_filler(tt);
    Alloc *a = arena_allocator(&tt->ar);
    Str url = BURROW_S("http://dummy.tld/");
    HttpRequest *req = NULL;
    switch (kind) {
    case H2CT_BLOCK_HEADERS:
        req =
            h2ct_new_request_url(tt, (Context){0}, BURROW_S("POST"), url, h2ct_no_body);
        if (req != NULL && !http_header_set(req->header, BURROW_S("Big"), filler))
            return NULL;
        return req;
    case H2CT_BLOCK_BODY: {
        StringsReader *sr = strings_new_reader(a, filler);
        if (sr == NULL)
            return NULL;
        return h2ct_new_request_url(tt, (Context){0}, BURROW_S("POST"), url,
                                    strings_reader_as_io_reader(sr));
    }
    case H2CT_BLOCK_TRAILER: {
        StringsReader *sr = strings_new_reader(a, BURROW_S("body"));
        if (sr == NULL)
            return NULL;
        req = h2ct_new_request_url(tt, (Context){0}, BURROW_S("POST"), url,
                                   strings_reader_as_io_reader(sr));
        if (req == NULL)
            return NULL;
        req->trailer = http_header_make(a);
        if (req->trailer == NULL ||
            !http_header_set(req->trailer, BURROW_S("Big"), filler))
            return NULL;
        return req;
    }
    default:
        return NULL;
    }
}

/* Waits for tr1's HTTP/2 transport to have a connection that can take a
 * request, or for it to have none, as it does in Go's bubble once a request's
 * stream is gone, or once a request has the last stream. */
static bool h2ct_tt_wait_idle_conn(H2ctTT *tt, bool want) {
    sync_mutex_lock(&tt->tr1.alt_mu);
    Http2Transport *t2 = tt->tr1.h2;
    sync_mutex_unlock(&tt->tr1.alt_mu);
    if (t2 == NULL) {
        testing_t_errorf_v(tt->t, "the Transport has no HTTP/2 transport");
        return false;
    }
    Time deadline = time_add(time_now(), H2CT_WAIT);
    for (;;) {
        Arena ar;
        arena_init(&ar, tt->a, 0);
        Slice idle = burrow__http2_transport_idle_conn_strs(t2, arena_allocator(&ar));
        bool ok = (idle.len > 0) == want;
        arena_free(&ar);
        if (ok)
            return true;
        if (!time_before(time_now(), deadline)) {
            testing_t_errorf_v(tt->t, "a conn can take a new request: %t, want %t",
                               !want, want);
            return false;
        }
        time_sleep(TIME_MILLISECOND);
    }
}

static HttpRequest *h2ct_small_request(H2ctTT *tt) {
    return h2ct_new_request_url(tt, (Context){0}, BURROW_S("GET"),
                                BURROW_S("http://dummy.tld/"), h2ct_no_body);
}

static void h2ct_blocking_request_write(H2ctTT *tt, const void *arg) {
    H2ctBlockingWrite kind = *(const H2ctBlockingWrite *)arg;

    /* Request 1: A small request to ensure we read the server
     * MaxConcurrentStreams. */
    H2ctRT *rt1 = h2ct_tt_round_trip(tt, h2ct_small_request(tt));
    H2CT_TRY(rt1 != NULL);
    H2ctConn *tc1 = h2ct_get_conn(tt);
    H2CT_TRY(tc1 != NULL && h2ct_want_frame_type(tc1, HTTP2_FRAME_SETTINGS) &&
             h2ct_want_frame_type(tc1, HTTP2_FRAME_WINDOW_UPDATE) &&
             h2ct_want_headers(tc1, 1, true, NULL, 0));
    static const Http2Setting s[] = {{HTTP2_SETTING_MAX_CONCURRENT_STREAMS, 1}};
    H2CT_TRY(h2ct_write_settings(tc1, s, 1));
    H2CT_TRY(h2ct_write_status(tc1, 1, "200"));
    H2CT_TRY(h2ct_rt_want_status(rt1, 200));
    H2CT_TRY(h2ct_want_frame_type(tc1, HTTP2_FRAME_SETTINGS)); /* settings ACK */
    H2CT_TRY(h2ct_tt_wait_idle_conn(tt, true));

    /* Request 2: A large request that blocks while being written. */
    sync_mutex_lock(&tc1->rmu);
    tc1->rlimit = 1024;
    sync_mutex_unlock(&tc1->rmu);
    HttpRequest *req2 = h2ct_blocking_request(tt, kind);
    H2CT_TRY(req2 != NULL);
    H2ctRT *rt2 = h2ct_tt_round_trip(tt, req2);
    H2CT_TRY(rt2 != NULL && h2ct_tt_wait_idle_conn(tt, false));

    /* Request 3: A small request that is sent on a new connection, since
     * request 2 is hogging the only available stream on the previous
     * connection. */
    H2ctRT *rt3 = h2ct_tt_round_trip(tt, h2ct_small_request(tt));
    H2CT_TRY(rt3 != NULL);
    H2ctConn *tc2 = h2ct_get_conn(tt);
    H2CT_TRY(tc2 != NULL && h2ct_want_frame_type(tc2, HTTP2_FRAME_SETTINGS) &&
             h2ct_want_frame_type(tc2, HTTP2_FRAME_WINDOW_UPDATE) &&
             h2ct_want_headers(tc2, 1, true, NULL, 0));
    H2CT_TRY(h2ct_write_settings(tc2, NULL, 0));
    H2CT_TRY(h2ct_write_status(tc2, 1, "200"));
    H2CT_TRY(h2ct_rt_want_status(rt3, 200));
    H2CT_TRY(h2ct_want_frame_type(tc2, HTTP2_FRAME_SETTINGS)); /* settings ACK */

    if (h2ct_rt_done(rt2))
        testing_t_errorf_v(tt->t, "RoundTrip 2 is done, expect it to be still pending");
}

static void TestTransportBlockingRequestWrite_headers(TestingT *t) {
    static const H2ctBlockingWrite kind = H2CT_BLOCK_HEADERS;
    h2ct_run(t, h2ct_blocking_request_write, &kind);
}

static void TestTransportBlockingRequestWrite_body(TestingT *t) {
    static const H2ctBlockingWrite kind = H2CT_BLOCK_BODY;
    h2ct_run(t, h2ct_blocking_request_write, &kind);
}

static void TestTransportBlockingRequestWrite_trailer(TestingT *t) {
    static const H2ctBlockingWrite kind = H2CT_BLOCK_TRAILER;
    h2ct_run(t, h2ct_blocking_request_write, &kind);
}

/* tc.cc.Shutdown on a goroutine of its own, which finish waits for, having
 * closed the conn so that it does not wait on a test that gave up. */
typedef struct H2ctShutdown {
    H2ctTT *tt;
    Context ctx;
    Arena arena;
    Error err; /* in arena */
    SyncAtomicBool done;
    SyncWaitGroup wg;
} H2ctShutdown;

static void h2ct_shutdown_job(void *env) {
    H2ctShutdown *sd = (H2ctShutdown *)env;
    Error err = burrow__http2_client_conn_shutdown(sd->tt->cc, sd->ctx);
    sd->err = error_retain(arena_allocator(&sd->arena), err);
    sync_atomic_bool_store(&sd->done, true);
}

static bool h2ct_shutdown_start(H2ctTT *tt, H2ctShutdown *sd, Context ctx) {
    memset(sd, 0, sizeof *sd);
    sd->tt = tt;
    sd->ctx = ctx;
    arena_init(&sd->arena, tt->a, 0);
    if (!sync_wait_group_go(&sd->wg, BURROW_FN(Func, h2ct_shutdown_job, sd))) {
        testing_t_errorf_v(tt->t, "no memory for Shutdown's goroutine");
        return false;
    }
    return true;
}

static void h2ct_shutdown_finish(H2ctConn *tc, H2ctShutdown *sd) {
    h2ct_conn_close(tc);
    sync_wait_group_wait(&sd->wg);
    arena_free(&sd->arena);
}

static void h2ct_client_conn_shutdown_steps(H2ctConn *tc, H2ctRT *rt) {
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_GO_AWAY));
    H2CT_TRY(h2ct_want_idle(tc)); /* connection is not closed */
    uint32_t id = h2ct_rt_stream_id(rt);
    H2CT_TRY(id != 0);
    static const Str res_kv[] = {S_(":status"), S_("200")};
    H2CT_TRY(h2ct_write_headers(tc, h2ct_hfp(id, h2ct_block(tc, res_kv, 2), false)));
    H2CT_TRY(h2ct_write_data(tc, id, true, "body"));

    H2CT_TRY(h2ct_rt_want_status(rt, 200));
    H2CT_TRY(h2ct_rt_want_body(rt, "body"));

    /* Now that the client has received the response, it closes the
     * connection. */
    (void)h2ct_want_closed(tc);
}

static void h2ct_client_conn_shutdown(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL && h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));

    H2ctShutdown sd;
    H2CT_TRY(h2ct_shutdown_start(tt, &sd, context_background()));
    h2ct_client_conn_shutdown_steps(tc, rt);
    h2ct_shutdown_finish(tc, &sd);
}

static void TestClientConnShutdown(TestingT *t) {
    h2ct_run(t, h2ct_client_conn_shutdown, NULL);
}

static void h2ct_client_conn_shutdown_cancel_steps(H2ctTT *tt, H2ctConn *tc, H2ctRT *rt,
                                                   H2ctShutdown *sd,
                                                   ContextCancelFunc cancel) {
    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_GO_AWAY));
    H2CT_TRY(h2ct_want_idle(tc)); /* connection is not closed */

    BURROW_CALLF0(cancel);
    Time deadline = time_add(time_now(), H2CT_WAIT);
    while (!sync_atomic_bool_load(&sd->done) && time_before(time_now(), deadline))
        time_sleep(TIME_MILLISECOND);

    if (!sync_atomic_bool_load(&sd->done) || !errors_is(sd->err, context_canceled))
        FATALF("ClientConn.Shutdown(ctx) did not return context.Canceled after "
               "cancelling context");

    /* The documentation for this test states:
     *     The expected behavior is the client closing the connection
     *     after the context is canceled.
     *
     * This seems reasonable, but it isn't what we do.
     * When ClientConn.Shutdown's context is canceled, Shutdown returns but
     * the connection is not closed.
     *
     * TODO: Figure out the correct behavior. */
    if (h2ct_rt_done(rt))
        FATALF("RoundTrip unexpectedly returned during shutdown");
}

/* The client sends a GOAWAY frame before the server finishes processing a
 * request, but cancels the passed context before the request is completed.
 * The expected behavior is the client closing the connection after the
 * context is canceled. */
static void h2ct_client_conn_shutdown_cancel(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    H2CT_TRY(rt != NULL && h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));

    ContextCancelFunc cancel;
    Context ctx = context_with_cancel(tt->a, context_background(), &cancel);
    H2ctShutdown sd;
    if (h2ct_shutdown_start(tt, &sd, ctx)) {
        h2ct_client_conn_shutdown_cancel_steps(tt, tc, rt, &sd, cancel);
        BURROW_CALLF0(cancel);
        h2ct_shutdown_finish(tc, &sd);
    }
    context_release(ctx);
}

static void TestClientConnShutdownCancel(TestingT *t) {
    h2ct_run(t, h2ct_client_conn_shutdown_cancel, NULL);
}

/* Go's bubble has the stream gone by the time the response is read. Here it
 * waits for that. */
static bool h2ct_reservations_round_trip(H2ctTT *tt, H2ctConn *tc) {
    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request(tt, (Context){0}, BURROW_S("GET"), h2ct_no_body));
    if (rt == NULL || !h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS))
        return false;
    uint32_t id = h2ct_rt_stream_id(rt);
    if (id == 0 || !h2ct_write_status(tc, id, "200") || !h2ct_rt_want_status(rt, 200))
        return false;
    Time deadline = time_add(time_now(), H2CT_WAIT);
    while (burrow__http2_client_conn_streams_active(tt->cc) > 0) {
        if (!time_before(time_now(), deadline)) {
            testing_t_errorf_v(tt->t, "stream %d is still active", (int)id);
            return false;
        }
        time_sleep(TIME_MILLISECOND);
    }
    return true;
}

static void h2ct_client_conn_reservations(H2ctTT *tt, const void *arg) {
    (void)arg;
    enum { INITIAL_MAX_CONCURRENT_STREAMS = 100 };
    H2ctConn *tc = h2ct_new_client_conn(tt);
    static const Http2Setting s[] = {
        {HTTP2_SETTING_MAX_CONCURRENT_STREAMS, INITIAL_MAX_CONCURRENT_STREAMS}};
    H2CT_TRY(tc != NULL && h2ct_greet(tc, s, 1));

    int n = 0;
    while (n <= INITIAL_MAX_CONCURRENT_STREAMS &&
           burrow__http2_client_conn_reserve_new_request(tt->cc))
        n++;
    if (n != INITIAL_MAX_CONCURRENT_STREAMS)
        testing_t_errorf_v(tt->t, "did %d reservations; want %d", n,
                           INITIAL_MAX_CONCURRENT_STREAMS);
    H2CT_TRY(h2ct_reservations_round_trip(tt, tc));
    int n2 = 0;
    while (n2 <= 5 && burrow__http2_client_conn_reserve_new_request(tt->cc))
        n2++;
    if (n2 != 1)
        FATALF("after one RoundTrip, did %d reservations; want 1", n2);

    /* Use up all the reservations */
    for (int i = 0; i < n; i++)
        H2CT_TRY(h2ct_reservations_round_trip(tt, tc));

    n2 = 0;
    while (n2 <= INITIAL_MAX_CONCURRENT_STREAMS &&
           burrow__http2_client_conn_reserve_new_request(tt->cc))
        n2++;
    if (n2 != n)
        testing_t_errorf_v(tt->t, "after reset, reservations = %d; want %d", n2, n);
}

static void TestClientConnReservations(TestingT *t) {
    h2ct_run(t, h2ct_client_conn_reservations, NULL);
}

/* Go sleeps five seconds of its fake clock. This sleeps five real ones. */
static void h2ct_timeout_server_hangs(H2ctTT *tt, const void *arg) {
    (void)arg;
    H2ctConn *tc = h2ct_new_client_conn(tt);
    H2CT_TRY(tc != NULL && h2ct_greet(tc, NULL, 0));

    H2ctRT *rt = h2ct_tc_round_trip(
        tt, h2ct_new_request_url(tt, (Context){0}, BURROW_S("PUT"),
                                 BURROW_S("http://dummy.tld/"), h2ct_no_body));
    H2CT_TRY(rt != NULL);

    H2CT_TRY(h2ct_want_frame_type(tc, HTTP2_FRAME_HEADERS));
    time_sleep(5 * TIME_SECOND);
    Error err = BURROW_NO_ERROR;
    Http2Frame *f = h2ct_read_frame_within(tc, H2CT_QUIET, &err);
    if (f != NULL && BURROW_OK(err)) {
        int type = (int)f->header.type;
        burrow__http2_frame_free(f);
        FATALF("unexpected frame: type %d", type);
    }
    if (sync_atomic_bool_load(&rt->done))
        FATALF("after 5 seconds with no response, RoundTrip unexpectedly returned");

    BURROW_CALLF0(rt->cancel);
    err = h2ct_rt_err(rt);
    if (!errors_is(err, context_canceled))
        FATALF("RoundTrip error: %v; want context.Canceled", err);
}

static void TestTransportTimeoutServerHangs(TestingT *t) {
    h2ct_run(t, h2ct_timeout_server_hangs, NULL);
}

/* ------------------------------------------------------------ a real server */

/* The tests from here on talk to a real server over loopback TCP, which is
 * Go's newTestServer. Go's speaks HTTP/2 over TLS unless the test asks for
 * h2c. There is no TLS here yet, so this one always speaks h2c, unencrypted
 * HTTP/2 with prior knowledge, and so do the transports, and the checks Go
 * makes on Response.TLS are left out. */

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define H2CT_HAVE_TCP 1
#endif

typedef struct H2ctTS {
    TestingT *t;
    HttpHandlerFunc hf;
    HttpProtocols sprotos; /* the server's */
    HttpProtocols cprotos; /* the transports' */
    HttpHTTP2Config h2;    /* newTransport's */
    HttptestServer *ts;
    HttpTransport tr; /* newTransport */
    Arena ar;         /* the test's */
} H2ctTS;

static void h2ct_ts_empty_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)w;
    (void)r;
}

/* newTestServer, not started yet, so the test can set more of s->ts->config
 * first, and newTransport, which is s->tr. A NULL fn answers every request
 * with an empty 200. False when the test can't go on, and either way
 * h2ct_ts_close cleans up. */
static bool h2ct_ts_new(H2ctTS *s, TestingT *t,
                        void (*fn)(void *, HttpResponseWriter, HttpRequest *),
                        void *env) {
    memset(s, 0, sizeof *s);
    s->t = t;
    arena_init(&s->ar, heap_allocator(), 0);
    s->hf = BURROW_FN(HttpHandlerFunc, fn != NULL ? fn : h2ct_ts_empty_handler, env);
#if !defined(H2CT_HAVE_TCP)
    testing_t_skip_v(t, "TCP here needs the readiness poll FD");
    return false;
#else
    s->ts = httptest_new_unstarted_server(NULL, http_handler_func_as_handler(&s->hf));
    if (s->ts == NULL) {
        testing_t_errorf_v(t, "no memory for the server");
        return false;
    }
    http_protocols_set_unencrypted_http2(&s->sprotos, true);
    s->ts->config.protocols = &s->sprotos;
    http_protocols_set_unencrypted_http2(&s->cprotos, true);
    s->ts->transport.protocols = &s->cprotos;
    s->tr.protocols = &s->cprotos;
    s->tr.http2 = &s->h2;
    return true;
#endif
}

/* newTestServer and newTransport, with the server started. */
static bool h2ct_ts_start(H2ctTS *s, TestingT *t,
                          void (*fn)(void *, HttpResponseWriter, HttpRequest *),
                          void *env) {
    if (!h2ct_ts_new(s, t, fn, env))
        return false;
    httptest_server_start(s->ts);
    return true;
}

/* The cleanups Go's helpers register, newTransport's last in and so first. */
static void h2ct_ts_close(H2ctTS *s) {
    http_transport_close_idle_connections(&s->tr);
    http_transport_free(&s->tr);
    if (s->ts != NULL) {
        httptest_server_close_client_connections(s->ts);
        httptest_server_free(s->ts);
    }
    arena_free(&s->ar);
}

/* ts.URL with path after it. */
static Str h2ct_ts_url(H2ctTS *s, const char *path) {
    return fmt_sprintf_v(arena_allocator(&s->ar), "%s%s", s->ts->url,
                         str_from_cstr(path));
}

/* A request for the server, with ctx when it isn't nil. */
static HttpRequest *h2ct_ts_request(H2ctTS *s, Context ctx, Str method,
                                    const char *path) {
    Error err = BURROW_NO_ERROR;
    Str url = h2ct_ts_url(s, path);
    HttpRequest *req =
        ctx.vt != NULL
            ? http_new_request_with_context(heap_allocator(), ctx, method, url,
                                            h2ct_no_body, &err)
            : http_new_request(heap_allocator(), method, url, h2ct_no_body, &err);
    if (req == NULL)
        testing_t_errorf_v(s->t, "NewRequest: %v", err);
    return req;
}

/* io.ReadAll of res's body, into the test's arena. */
static bool h2ct_ts_read_body(H2ctTS *s, HttpResponse *res, Str *body) {
    Error err = BURROW_NO_ERROR;
    Slice b = io_read_all(arena_allocator(&s->ar),
                          io_read_closer_as_io_reader(res->body), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(s->t, "Body read: %v", err);
        return false;
    }
    *body = str_from_bytes((const Byte *)b.p, b.len);
    return true;
}

static void h2ct_write_string(HttpResponseWriter w, Str s) {
    (void)io_write_string(http_response_writer_as_io_writer(w), s, NULL);
}

static void h2ct_hello_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    /* Go also says whether r.TLS is nil, which here it always is. */
    (void)fmt_fprintf_v(http_response_writer_as_io_writer(w), "Hello, %s, http: true",
                        r->url->path);
}

static void h2ct_count_new_conns(void *env, HttptraceGotConnInfo info) {
    if (!info.reused)
        (void)sync_atomic_int32_add((SyncAtomicInt32 *)env, 1);
}

/* The checks of TestTransportH2c on a request through tr. */
static void h2ct_h2c(H2ctTS *s, HttpRequest *req, const SyncAtomicInt32 *conns) {
    TestingT *t = s->t;
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_transport_round_trip(&s->tr, req, &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "RoundTrip: %v", err);
        return;
    }
    Str body = BURROW_STR_EMPTY;
    if (res->proto_major != 2)
        testing_t_errorf_v(t, "proto not h2c");
    else if (h2ct_ts_read_body(s, res, &body)) {
        if (!str_eq(body, BURROW_S("Hello, /foobar, http: true")))
            testing_t_errorf_v(t, "response got %s, want %s", body,
                               BURROW_S("Hello, /foobar, http: true"));
        else if (sync_atomic_int32_load(conns) != 1)
            testing_t_errorf_v(t, "Too many got connections: %d",
                               (int)sync_atomic_int32_load(conns));
    }
    http_response_free(res);
}

static void TestTransportH2c(TestingT *t) {
    H2ctTS s;
    SyncAtomicInt32 conns;
    memset(&conns, 0, sizeof conns);
    HttptraceClientTrace trace;
    memset(&trace, 0, sizeof trace);
    trace.got_conn = BURROW_FN(HttptraceGotConnFunc, h2ct_count_new_conns, &conns);
    if (h2ct_ts_start(&s, t, h2ct_hello_handler, NULL)) {
        Context ctx = httptrace_with_client_trace(arena_allocator(&s.ar),
                                                  context_background(), &trace);
        HttpRequest *req = h2ct_ts_request(&s, ctx, BURROW_S("GET"), "/foobar");
        if (req != NULL)
            h2ct_h2c(&s, req, &conns);
        http_request_free(req);
        context_release(ctx);
    }
    h2ct_ts_close(&s);
}

static void h2ct_sup_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    h2ct_write_string(w, BURROW_S("sup"));
}

/* The response TestTransport wants, from request i. */
static void h2ct_check_sup(H2ctTS *s, int i, HttpRequest *req, HttpResponse *res) {
    TestingT *t = s->t;
    if (res->status_code != 200)
        testing_t_errorf_v(t, "%d: StatusCode = %d; want 200", i, res->status_code);
    if (!str_eq(res->status, BURROW_S("200 OK")))
        testing_t_errorf_v(t, "%d: Status = %q; want %q", i, res->status,
                           BURROW_S("200 OK"));
    /* Content-Length: 3, Content-Type: text/plain; charset=utf-8, and a Date. */
    Slice cl = http_header_values(res->header, BURROW_S("Content-Length"));
    Slice ct = http_header_values(res->header, BURROW_S("Content-Type"));
    Slice date = http_header_values(res->header, BURROW_S("Date"));
    if (map_len(res->header) != 3 || cl.len != 1 || ct.len != 1 || date.len != 1 ||
        !str_eq(((const Str *)cl.p)[0], BURROW_S("3")) ||
        !str_eq(((const Str *)ct.p)[0], BURROW_S("text/plain; charset=utf-8"))) {
        BytesBuffer b = BYTES_BUFFER(heap_allocator());
        (void)http_header_write(res->header, bytes_buffer_as_io_writer(&b));
        testing_t_errorf_v(
            t, "%d: res Header = %q", i,
            str_from_bytes(bytes_buffer_bytes(&b).p, bytes_buffer_len(&b)));
        bytes_buffer_free(&b);
    }
    if (res->request != req)
        testing_t_errorf_v(t, "%d: Response.Request isn't the request", i);
    Str body = BURROW_STR_EMPTY;
    if (h2ct_ts_read_body(s, res, &body) && !str_eq(body, BURROW_S("sup")))
        testing_t_errorf_v(t, "%d: Body = %q; want %q", i, body, BURROW_S("sup"));
}

static void TestTransport(TestingT *t) {
    H2ctTS s;
    if (h2ct_ts_start(&s, t, h2ct_sup_handler, NULL)) {
        /* ts.Client().Transport, and a method of "" for the second, which is
         * GET. */
        HttpTransport *tr = &s.ts->transport;
        static const Str methods[] = {S_("GET"), S_("")};
        for (int i = 0; i < 2; i++) {
            HttpRequest *req = h2ct_ts_request(&s, (Context){0}, BURROW_S("GET"), "");
            if (req == NULL)
                break;
            req->method = methods[i];
            Error err = BURROW_NO_ERROR;
            HttpResponse *res = http_transport_round_trip(tr, req, &err);
            if (res == NULL) {
                testing_t_errorf_v(t, "%d: %v", i, err);
                http_request_free(req);
                break;
            }
            h2ct_check_sup(&s, i, req, res);
            http_response_free(res);
            http_request_free(req);
        }
        http_transport_close_idle_connections(tr);
    }
    h2ct_ts_close(&s);
}

static void h2ct_remote_addr_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    h2ct_write_string(w, r->remote_addr);
}

typedef enum H2ctModReq {
    H2CT_REUSE_CONN,
    H2CT_REQUEST_CLOSE,
    H2CT_CONN_CLOSE,
} H2ctModReq;

/* get in testTransportReusesConns: the address the server saw the request
 * come from. */
static bool h2ct_get_addr(H2ctTS *s, H2ctModReq mod, Str *addr) {
    HttpRequest *req = h2ct_ts_request(s, (Context){0}, BURROW_S("GET"), "");
    if (req == NULL)
        return false;
    switch (mod) {
    case H2CT_REQUEST_CLOSE:
        req->close = true;
        break;
    case H2CT_CONN_CLOSE:
        (void)http_header_set(req->header, BURROW_S("Connection"), BURROW_S("close"));
        break;
    case H2CT_REUSE_CONN:
    default:
        break;
    }
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_transport_round_trip(&s->tr, req, &err);
    bool ok = res != NULL;
    if (!ok)
        testing_t_errorf_v(s->t, "RoundTrip: %v", err);
    else if (h2ct_ts_read_body(s, res, addr)) {
        *addr = strings_trim_space(*addr);
        if (addr->len == 0) {
            testing_t_errorf_v(s->t, "didn't get an addr in response");
            ok = false;
        }
    } else
        ok = false;
    http_response_free(res);
    http_request_free(req);
    return ok;
}

static void h2ct_reuses_conns(TestingT *t, bool want_same, H2ctModReq mod) {
    H2ctTS s;
    Str first;
    Str second;
    if (h2ct_ts_start(&s, t, h2ct_remote_addr_handler, NULL) &&
        h2ct_get_addr(&s, mod, &first) && h2ct_get_addr(&s, mod, &second) &&
        str_eq(first, second) != want_same)
        testing_t_errorf_v(t,
                           "first and second responses on same connection: %t; want %t",
                           str_eq(first, second), want_same);
    h2ct_ts_close(&s);
}

static void TestTransportReusesConns_ReuseConn(TestingT *t) {
    h2ct_reuses_conns(t, true, H2CT_REUSE_CONN);
}

static void TestTransportReusesConns_RequestClose(TestingT *t) {
    h2ct_reuses_conns(t, false, H2CT_REQUEST_CLOSE);
}

static void TestTransportReusesConns_ConnClose(TestingT *t) {
    h2ct_reuses_conns(t, false, H2CT_CONN_CLOSE);
}

typedef struct H2ctConnHooks {
    TestingT *t;
    int i;
    SyncAtomicInt32 get_conns;
    SyncAtomicInt32 got_conns;
} H2ctConnHooks;

static void h2ct_hooks_get_conn(void *env, Str host_port) {
    (void)host_port;
    (void)sync_atomic_int32_add(&((H2ctConnHooks *)env)->get_conns, 1);
}

static void h2ct_hooks_got_conn(void *env, HttptraceGotConnInfo info) {
    H2ctConnHooks *h = (H2ctConnHooks *)env;
    int32_t got = sync_atomic_int32_add(&h->got_conns, 1);
    bool want_reused = got > 1;
    bool want_was_idle = got > 1;
    if (info.reused != want_reused || info.was_idle != want_was_idle)
        testing_t_errorf_v(
            h->t, "GotConn %d: Reused=%t (want %t), WasIdle=%t (want %t)", h->i,
            info.reused, want_reused, info.was_idle, want_was_idle);
}

static void h2ct_get_got_conn_hooks(TestingT *t, bool use_client) {
    H2ctTS s;
    H2ctConnHooks h;
    memset(&h, 0, sizeof h);
    h.t = t;
    HttptraceClientTrace trace;
    memset(&trace, 0, sizeof trace);
    trace.get_conn = BURROW_FN(HttptraceGetConnFunc, h2ct_hooks_get_conn, &h);
    trace.got_conn = BURROW_FN(HttptraceGotConnFunc, h2ct_hooks_got_conn, &h);
    if (h2ct_ts_start(&s, t, h2ct_remote_addr_handler, NULL)) {
        HttpClient *client = httptest_server_client(s.ts);
        Context ctx = httptrace_with_client_trace(arena_allocator(&s.ar),
                                                  context_background(), &trace);
        for (int i = 0; i < 2; i++) {
            h.i = i;
            HttpRequest *req = h2ct_ts_request(&s, ctx, BURROW_S("GET"), "");
            if (req == NULL)
                break;
            Error err = BURROW_NO_ERROR;
            HttpResponse *res = use_client
                                    ? http_client_do(client, req, &err)
                                    : http_transport_round_trip(&s.tr, req, &err);
            if (res == NULL) {
                testing_t_errorf_v(t, "%v", err);
                http_request_free(req);
                break;
            }
            (void)res->body.vt->closer.close(res->body.data);
            http_response_free(res);
            http_request_free(req);
            /* Go wants one GetConn a request, which is what it does over
             * TLS. Over h2c Go 1.27.1 calls it twice for a request on a
             * connection it already has, once in net/http and once more in
             * the HTTP/2 pool, and so does this. */
            int32_t want_get = i == 0 ? 1 : 3;
            int32_t get = sync_atomic_int32_load(&h.get_conns);
            int32_t got = sync_atomic_int32_load(&h.got_conns);
            if (get != want_get)
                testing_t_errorf_v(t, "after request %d, %d calls to GetConns: want %d",
                                   i, (int)get, (int)want_get);
            if (got != i + 1)
                testing_t_errorf_v(t, "after request %d, %d calls to GotConns: want %d",
                                   i, (int)got, i + 1);
        }
        context_release(ctx);
    }
    h2ct_ts_close(&s);
}

static void TestTransportGetGotConnHooks_HTTP2Transport(TestingT *t) {
    h2ct_get_got_conn_hooks(t, false);
}

static void TestTransportGetGotConnHooks_Client(TestingT *t) {
    h2ct_get_got_conn_hooks(t, true);
}

typedef struct H2ctAbort {
    H2ctTS *s;
    Chan *shutdown;
    SyncAtomicBool done;
} H2ctAbort;

static void h2ct_abort_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    H2ctAbort *ab = (H2ctAbort *)env;
    (void)w.vt->flush(w.data);
    bool v;
    (void)chan_recv(ab->shutdown, &v);
}

static void h2ct_abort_job(void *env) {
    H2ctAbort *ab = (H2ctAbort *)env;
    H2ctTS *s = ab->s;
    HttpRequest *req = h2ct_ts_request(s, (Context){0}, BURROW_S("GET"), "");
    Error err = BURROW_NO_ERROR;
    HttpResponse *res =
        req != NULL ? http_transport_round_trip(&s->tr, req, &err) : NULL;
    if (req != NULL && res == NULL)
        testing_t_errorf_v(s->t, "%v", err);
    if (res != NULL) {
        httptest_server_close_client_connections(s->ts);
        (void)io_read_all(arena_allocator(&s->ar),
                          io_read_closer_as_io_reader(res->body), &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(s->t, "expected error from res.Body.Read");
    }
    http_response_free(res);
    http_request_free(req);
    sync_atomic_bool_store(&ab->done, true);
}

static void TestTransportAbortClosesPipes(TestingT *t) {
    H2ctTS s;
    H2ctAbort ab;
    memset(&ab, 0, sizeof ab);
    ab.s = &s;
    SyncWaitGroup wg = {0};
    if (h2ct_ts_start(&s, t, h2ct_abort_handler, &ab)) {
        ab.shutdown = chan_make(heap_allocator(), TYPE_BOOL, 0);
        if (ab.shutdown == NULL)
            testing_t_errorf_v(t, "no memory");
        else
            (void)sync_wait_group_go(&wg, BURROW_FN(Func, h2ct_abort_job, &ab));
        Time deadline = time_add(time_now(), 3 * TIME_SECOND);
        while (ab.shutdown != NULL && !sync_atomic_bool_load(&ab.done) &&
               time_before(time_now(), deadline))
            time_sleep(10 * TIME_MILLISECOND);
        if (ab.shutdown != NULL && !sync_atomic_bool_load(&ab.done))
            testing_t_errorf_v(t, "timeout");
    }
    /* The handler has to go before the server can. */
    if (ab.shutdown != NULL)
        chan_close(ab.shutdown);
    sync_wait_group_wait(&wg);
    h2ct_ts_close(&s);
    chan_free(ab.shutdown);
}

typedef struct H2ctGotURL {
    Chan *gotc;
    char path[64];
    char query[64];
} H2ctGotURL;

static void h2ct_copy(char *dst, size_t cap, Str s) {
    size_t n = s.len < (Int)cap ? (size_t)s.len : cap - 1;
    memcpy(dst, s.p, n);
    dst[n] = '\0';
}

static void h2ct_path_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    H2ctGotURL *g = (H2ctGotURL *)env;
    h2ct_copy(g->path, sizeof g->path, r->url->path);
    h2ct_copy(g->query, sizeof g->query, r->url->raw_query);
    bool v = true;
    chan_send(g->gotc, &v);
}

static void TestTransportPath(TestingT *t) {
    H2ctTS s;
    H2ctGotURL g;
    memset(&g, 0, sizeof g);
    if (h2ct_ts_start(&s, t, h2ct_path_handler, &g)) {
        g.gotc = chan_make(heap_allocator(), TYPE_BOOL, 1);
        if (g.gotc == NULL)
            testing_t_errorf_v(t, "no memory");
    }
    if (g.gotc != NULL) {
        HttpRequest *req =
            h2ct_ts_request(&s, (Context){0}, BURROW_S("POST"), "/testpath?q=1");
        HttpClient c = {0};
        c.transport = http_transport_as_round_tripper(&s.tr);
        Error err = BURROW_NO_ERROR;
        HttpResponse *res = req != NULL ? http_client_do(&c, req, &err) : NULL;
        bool v;
        if (req != NULL && res == NULL)
            testing_t_errorf_v(t, "%v", err);
        else if (res != NULL && chan_recv(g.gotc, &v)) {
            if (strcmp(g.path, "/testpath") != 0)
                testing_t_errorf_v(t, "Read Path = %q; want %q", str_from_cstr(g.path),
                                   BURROW_S("/testpath"));
            if (strcmp(g.query, "q=1") != 0)
                testing_t_errorf_v(t, "Read RawQuery = %q; want %q",
                                   str_from_cstr(g.query), BURROW_S("q=1"));
        }
        http_response_free(res);
        http_request_free(req);
    }
    h2ct_ts_close(&s);
    chan_free(g.gotc);
}

/* randString: n bytes from Go's math/rand seeded with n, so the same as Go's. */
static Byte *h2ct_rand_string(Alloc *a, Int n) {
    MathRandRand *rnd = math_rand_new(a, math_rand_new_source(a, (int64_t)n));
    Byte *b = (Byte *)mem_alloc(a, (size_t)n, 1);
    if (rnd == NULL || b == NULL)
        return NULL;
    for (Int i = 0; i < n; i++)
        b[i] = (Byte)math_rand_rand_intn(rnd, 256);
    return b;
}

/* shortString. */
static Str h2ct_short_string(Alloc *a, Str v) {
    enum { MAX_LEN = 100 };
    if (v.len <= MAX_LEN)
        return v;
    return fmt_sprintf_v(a, "%s[...%d bytes omitted...]%s",
                         str_from_bytes(v.p, MAX_LEN / 2), v.len - MAX_LEN,
                         str_from_bytes(v.p + v.len - MAX_LEN / 2, MAX_LEN / 2));
}

/* struct{ io.Reader }: just a reader, which hides what it reads from. */
static Int h2ct_just_read(void *self, Slice p, Error *err) {
    const IoReader *r = (const IoReader *)self;
    return r->vt->read(r->data, p, err);
}

static const IoReaderVT h2ct_just_reader_vt = {NULL, h2ct_just_read};

typedef struct H2ctGotBody {
    Chan *gotc;
    Slice slurp; /* from the heap */
    int64_t content_length;
    bool failed;
    char err[128];
} H2ctGotBody;

static void h2ct_got_body_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    H2ctGotBody *g = (H2ctGotBody *)env;
    Error err = BURROW_NO_ERROR;
    g->slurp =
        io_read_all(heap_allocator(), io_read_closer_as_io_reader(r->body), &err);
    g->content_length = r->content_length;
    g->failed = BURROW_FAILED(err);
    if (g->failed) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        h2ct_copy(g->err, sizeof g->err,
                  fmt_sprintf_v(arena_allocator(&ar), "%v", err));
        arena_free(&ar);
    }
    bool v = true;
    chan_send(g->gotc, &v);
}

static void h2ct_body_test(H2ctTS *s, H2ctGotBody *g, int i, Str body,
                           bool no_content_len) {
    TestingT *t = s->t;
    /* newTransport, one for each. */
    HttpTransport tr;
    memset(&tr, 0, sizeof tr);
    tr.protocols = &s->cprotos;
    tr.http2 = &s->h2;
    StringsReader sr;
    strings_reader_reset(&sr, body);
    IoReader just = strings_reader_as_io_reader(&sr);
    IoReader r = no_content_len ? (IoReader){&h2ct_just_reader_vt, &just}
                                : strings_reader_as_io_reader(&sr);
    Error err = BURROW_NO_ERROR;
    HttpRequest *req =
        http_new_request(heap_allocator(), BURROW_S("POST"), s->ts->url, r, &err);
    HttpClient c = {0};
    c.transport = http_transport_as_round_tripper(&tr);
    HttpResponse *res = req != NULL ? http_client_do(&c, req, &err) : NULL;
    bool v;
    if (res == NULL)
        testing_t_errorf_v(t, "#%d: %v", i, err);
    else if (!chan_recv(g->gotc, &v))
        testing_t_errorf_v(t, "#%d: no request", i);
    else if (g->failed)
        testing_t_errorf_v(t, "#%d: read error: %s", i, str_from_cstr(g->err));
    else {
        Str got = str_from_bytes((const Byte *)g->slurp.p, g->slurp.len);
        Alloc *a = arena_allocator(&s->ar);
        if (!str_eq(got, body))
            testing_t_errorf_v(
                t, "#%d: Read body mismatch.\n got: %q (len %d)\nwant: %q (len %d)", i,
                h2ct_short_string(a, got), got.len, h2ct_short_string(a, body),
                body.len);
        int64_t want_len = no_content_len && body.len != 0 ? -1 : (int64_t)body.len;
        if (g->content_length != want_len)
            testing_t_errorf_v(t, "#%d. handler got ContentLength = %d; want %d", i,
                               g->content_length, want_len);
    }
    if (g->slurp.p != NULL)
        mem_free(heap_allocator(), g->slurp.p, (size_t)g->slurp.cap, 1);
    g->slurp = slice_from(NULL, 0, 0, TYPE_BYTE);
    http_response_free(res);
    http_request_free(req);
    http_transport_close_idle_connections(&tr);
    http_transport_free(&tr);
}

static void TestTransportBody(TestingT *t) {
    /* The body: 'm' for "some message", 'a' for n of 'a' and 'r' for
     * randString(n). */
    static const struct {
        Int n;
        char kind;
        bool no_content_len;
    } tests[] = {
        {0, 'm', false},
        {0, 'm', true},
        {1 << 20, 'a', true},
        {1 << 20, 'a', false},
        {(16 << 10) - 1, 'r', false},
        {16 << 10, 'r', false},
        {(16 << 10) + 1, 'r', false},
        {(512 << 10) - 1, 'r', false},
        {512 << 10, 'r', false},
        {(512 << 10) + 1, 'r', false},
        {(1 << 20) - 1, 'r', false},
        {1 << 20, 'r', false},
        {(1 << 20) + 2, 'r', false},
    };
    H2ctTS s;
    H2ctGotBody g;
    memset(&g, 0, sizeof g);
    if (h2ct_ts_start(&s, t, h2ct_got_body_handler, &g)) {
        g.gotc = chan_make(heap_allocator(), TYPE_BOOL, 1);
        if (g.gotc == NULL)
            testing_t_errorf_v(t, "no memory");
    }
    for (int i = 0; g.gotc != NULL && i < (int)(sizeof tests / sizeof tests[0]); i++) {
        Arena ar;
        arena_init(&ar, heap_allocator(), 0);
        Str body = BURROW_S("some message");
        if (tests[i].kind != 'm') {
            Byte *b =
                tests[i].kind == 'r'
                    ? h2ct_rand_string(arena_allocator(&ar), tests[i].n)
                    : (Byte *)mem_alloc(arena_allocator(&ar), (size_t)tests[i].n, 1);
            if (b == NULL) {
                testing_t_errorf_v(t, "no memory");
                arena_free(&ar);
                break;
            }
            if (tests[i].kind == 'a')
                memset(b, 'a', (size_t)tests[i].n);
            body = str_from_bytes(b, tests[i].n);
        }
        h2ct_body_test(&s, &g, i, body, tests[i].no_content_len);
        arena_free(&ar);
    }
    h2ct_ts_close(&s);
    chan_free(g.gotc);
}

/* capitalizeReader and flushWriter, on the server's side. */
static void h2ct_full_duplex_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    http_response_writer_write_header(w, 200); /* redundant but for clarity */
    (void)w.vt->flush(w.data);
    IoReader body = io_read_closer_as_io_reader(r->body);
    Byte buf[512];
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Int n = body.vt->read(body.data,
                              slice_from(buf, sizeof buf, sizeof buf, TYPE_BYTE), &err);
        for (Int i = 0; i < n; i++) {
            if (buf[i] >= 'a' && buf[i] <= 'z')
                buf[i] = (Byte)(buf[i] - ('a' - 'A'));
        }
        Error werr = BURROW_NO_ERROR;
        if (n > 0) {
            (void)http_response_writer_write(w, slice_from(buf, n, n, TYPE_BYTE),
                                             &werr);
            (void)w.vt->flush(w.data);
        }
        if (BURROW_FAILED(err) || BURROW_FAILED(werr))
            break;
    }
    h2ct_write_string(w, BURROW_S("bye.\n"));
}

static bool h2ct_scan_want(TestingT *t, BufioScanner *bs, const char *v) {
    if (!bufio_scanner_scan(bs)) {
        testing_t_errorf_v(t, "wanted to read %q but Scan() = false, err = %v",
                           str_from_cstr(v), bufio_scanner_err(bs));
        return false;
    }
    return true;
}

static bool h2ct_pipe_write(TestingT *t, IoPipeWriter *pw, Str v) {
    Error err = BURROW_NO_ERROR;
    (void)io_pipe_writer_write(
        pw, slice_from((void *)(uintptr_t)v.p, v.len, v.len, TYPE_BYTE), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "pipe write: %v", err);
        return false;
    }
    return true;
}

static void h2ct_full_duplex(H2ctTS *s, IoPipeReader *pr, IoPipeWriter *pw) {
    TestingT *t = s->t;
    HttpClient c = {0};
    c.transport = http_transport_as_round_tripper(&s->tr);
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = http_new_request(heap_allocator(), BURROW_S("PUT"), s->ts->url,
                                        io_pipe_reader_as_io_reader(pr), &err);
    if (req == NULL) {
        testing_t_errorf_v(t, "%v", err);
        return;
    }
    req->content_length = -1;
    HttpResponse *res = http_client_do(&c, req, &err);
    if (res == NULL)
        testing_t_errorf_v(t, "%v", err);
    else if (res->status_code != 200)
        testing_t_errorf_v(t, "StatusCode = %d; want %d", res->status_code, 200);
    else {
        BufioScanner *bs =
            bufio_new_scanner(heap_allocator(), io_read_closer_as_io_reader(res->body));
        if (bs == NULL)
            testing_t_errorf_v(t, "no memory");
        else if (h2ct_pipe_write(t, pw, BURROW_S("foo\n")) &&
                 h2ct_scan_want(t, bs, "FOO") &&
                 h2ct_pipe_write(t, pw, BURROW_S("bar\n")) &&
                 h2ct_scan_want(t, bs, "BAR")) {
            (void)io_pipe_writer_close(pw);
            if (h2ct_scan_want(t, bs, "bye.") && BURROW_FAILED(bufio_scanner_err(bs)))
                testing_t_errorf_v(t, "%v", bufio_scanner_err(bs));
        }
        bufio_scanner_free(bs);
    }
    /* Ends the request's body, if the test failed before it did. */
    (void)io_pipe_writer_close_with_error(pw, io_err_closed_pipe);
    http_response_free(res);
    http_request_free(req);
}

static void TestTransportFullDuplex(TestingT *t) {
    H2ctTS s;
    IoPipeReader *pr = NULL;
    IoPipeWriter *pw = NULL;
    if (h2ct_ts_start(&s, t, h2ct_full_duplex_handler, NULL)) {
        io_pipe(heap_allocator(), &pr, &pw);
        if (pr == NULL)
            testing_t_errorf_v(t, "no memory");
        else
            h2ct_full_duplex(&s, pr, pw);
    }
    h2ct_ts_close(&s);
    if (pr != NULL)
        io_pipe_free(pr);
}

typedef struct H2ctGotConnect {
    Chan *gotc;
    char method[16];
    char host[64];
    char url_host[64];
} H2ctGotConnect;

static void h2ct_connect_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)w;
    H2ctGotConnect *g = (H2ctGotConnect *)env;
    h2ct_copy(g->method, sizeof g->method, r->method);
    h2ct_copy(g->host, sizeof g->host, r->host);
    h2ct_copy(g->url_host, sizeof g->url_host, r->url->host);
    bool v = true;
    chan_send(g->gotc, &v);
}

static void h2ct_connect_request(H2ctTS *s, H2ctGotConnect *g, int i,
                                 const char *host) {
    TestingT *t = s->t;
    HttpClient c = {0};
    c.transport = http_transport_as_round_tripper(&s->tr);
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = http_new_request(heap_allocator(), BURROW_S("CONNECT"),
                                        s->ts->url, h2ct_no_body, &err);
    if (req == NULL) {
        testing_t_errorf_v(t, "%v", err);
        return;
    }
    /* Go's requests have no Host but the second one's, so the first goes to
     * u.Host. */
    Str want = host != NULL ? str_from_cstr(host) : req->url->host;
    req->host = host != NULL ? str_from_cstr(host) : BURROW_STR_EMPTY;
    HttpResponse *res = http_client_do(&c, req, &err);
    bool v;
    if (res == NULL)
        testing_t_errorf_v(t, "%d. RoundTrip = %v", i, err);
    else if (chan_recv(g->gotc, &v)) {
        (void)res->body.vt->closer.close(res->body.data);
        if (strcmp(g->method, "CONNECT") != 0)
            testing_t_errorf_v(t, "method = %q; want CONNECT",
                               str_from_cstr(g->method));
        if (!str_eq(str_from_cstr(g->host), want))
            testing_t_errorf_v(t, "Host = %q; want %q", str_from_cstr(g->host), want);
        if (!str_eq(str_from_cstr(g->url_host), want))
            testing_t_errorf_v(t, "URL.Host = %q; want %q", str_from_cstr(g->url_host),
                               want);
    }
    http_response_free(res);
    http_request_free(req);
}

static void TestTransportConnectRequest(TestingT *t) {
    H2ctTS s;
    H2ctGotConnect g;
    memset(&g, 0, sizeof g);
    if (h2ct_ts_start(&s, t, h2ct_connect_handler, &g)) {
        g.gotc = chan_make(heap_allocator(), TYPE_BOOL, 1);
        if (g.gotc == NULL)
            testing_t_errorf_v(t, "no memory");
    }
    if (g.gotc != NULL) {
        h2ct_connect_request(&s, &g, 0, NULL);
        h2ct_connect_request(&s, &g, 1, "example.com:123");
    }
    h2ct_ts_close(&s);
    chan_free(g.gotc);
}

typedef struct H2ctPanic {
    Chan *do_panic;
} H2ctPanic;

static void h2ct_panic_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    H2ctPanic *p = (H2ctPanic *)env;
    (void)w.vt->flush(w.data); /* force headers out */
    bool v;
    (void)chan_recv(p->do_panic, &v);
    panic_str(BURROW_S("boom"));
}

static void h2ct_body_read_error_type(H2ctTS *s, H2ctPanic *p) {
    TestingT *t = s->t;
    HttpClient c = {0};
    c.transport = http_transport_as_round_tripper(&s->tr);
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(&c, s->ts->url, &err);
    bool v = true;
    chan_send(p->do_panic, &v);
    if (res == NULL) {
        testing_t_errorf_v(t, "%v", err);
        return;
    }
    Byte buf[100];
    IoReader body = io_read_closer_as_io_reader(res->body);
    Int n = body.vt->read(body.data, slice_from(buf, sizeof buf, sizeof buf, TYPE_BYTE),
                          &err);
    if (!h2ct_is_stream_error(err, 1, HTTP2_ERR_CODE_INTERNAL, NULL))
        testing_t_errorf_v(
            t, "Read = %d, %v; want error StreamError{StreamID:0x1, Code:0x2}", n, err);
    http_response_free(res);
}

static void TestTransportBodyReadErrorType(TestingT *t) {
    H2ctTS s;
    H2ctPanic p = {chan_make(heap_allocator(), TYPE_BOOL, 1)};
    LogLogger *quiet = NULL;
    if (h2ct_ts_new(&s, t, h2ct_panic_handler, &p)) {
        /* optQuiet */
        quiet = log_new(heap_allocator(), io_discard, BURROW_STR_EMPTY, 0);
        if (p.do_panic == NULL || quiet == NULL)
            testing_t_errorf_v(t, "no memory");
        else {
            s.ts->config.error_log = quiet;
            httptest_server_start(s.ts);
            h2ct_body_read_error_type(&s, &p);
        }
    }
    h2ct_ts_close(&s);
    log_logger_free(heap_allocator(), quiet);
    chan_free(p.do_panic);
}

/* noteCloseConn: a connection that calls closefn the first time it is
 * closed. */
typedef struct H2ctNoteClose {
    NetConn c;
    Func closefn;
    SyncOnce once_close;
} H2ctNoteClose;

static Int h2ct_nc_read(void *self, Slice p, Error *err) {
    NetConn c = ((H2ctNoteClose *)self)->c;
    return c.vt->reader.read(c.data, p, err);
}

static Int h2ct_nc_write(void *self, Slice p, Error *err) {
    NetConn c = ((H2ctNoteClose *)self)->c;
    return c.vt->writer.write(c.data, p, err);
}

static Error h2ct_nc_close(void *self) {
    H2ctNoteClose *nc = (H2ctNoteClose *)self;
    sync_once_do(&nc->once_close, nc->closefn);
    return nc->c.vt->closer.close(nc->c.data);
}

static NetAddr h2ct_nc_local_addr(void *self) {
    NetConn c = ((H2ctNoteClose *)self)->c;
    return c.vt->local_addr(c.data);
}

static NetAddr h2ct_nc_remote_addr(void *self) {
    NetConn c = ((H2ctNoteClose *)self)->c;
    return c.vt->remote_addr(c.data);
}

static Error h2ct_nc_set_deadline(void *self, Time d) {
    NetConn c = ((H2ctNoteClose *)self)->c;
    return c.vt->set_deadline(c.data, d);
}

static Error h2ct_nc_set_read_deadline(void *self, Time d) {
    NetConn c = ((H2ctNoteClose *)self)->c;
    return c.vt->set_read_deadline(c.data, d);
}

static Error h2ct_nc_set_write_deadline(void *self, Time d) {
    NetConn c = ((H2ctNoteClose *)self)->c;
    return c.vt->set_write_deadline(c.data, d);
}

static const NetConnVT h2ct_note_close_vt = {
    .reader = {NULL, h2ct_nc_read},
    .writer = {NULL, h2ct_nc_write},
    .closer = {NULL, h2ct_nc_close},
    .local_addr = h2ct_nc_local_addr,
    .remote_addr = h2ct_nc_remote_addr,
    .set_deadline = h2ct_nc_set_deadline,
    .set_read_deadline = h2ct_nc_set_read_deadline,
    .set_write_deadline = h2ct_nc_set_write_deadline,
};

/* The tests' tr.Dial, which wraps each connection in a noteCloseConn. */
typedef struct H2ctNoteDialer {
    Func closefn;
    SyncWaitGroup *conns; /* when not NULL, Add(1) for each connection */
    SyncAtomicInt32 dials;
} H2ctNoteDialer;

static NetConn h2ct_note_close_dial(void *env, Str network, Str addr, Error *err) {
    H2ctNoteDialer *d = (H2ctNoteDialer *)env;
    NetConn none = {NULL, NULL};
    NetConn tc = net_dial(heap_allocator(), network, addr, err);
    if (BURROW_FAILED(*err))
        return none;
    H2ctNoteClose *nc = (H2ctNoteClose *)mem_alloc(heap_allocator(), sizeof *nc,
                                                   _Alignof(H2ctNoteClose));
    if (nc == NULL) {
        net_conn_free(tc);
        *err = burrow_err_out_of_memory;
        return none;
    }
    nc->c = tc;
    nc->closefn = d->closefn;
    (void)sync_atomic_int32_add(&d->dials, 1);
    if (d->conns != NULL)
        sync_wait_group_add(d->conns, 1);
    return (NetConn){&h2ct_note_close_vt, nc};
}

static void h2ct_note_close_free(void *env, NetConn c) {
    (void)env;
    H2ctNoteClose *nc = (H2ctNoteClose *)c.data;
    net_conn_free(nc->c);
    mem_free(heap_allocator(), nc, sizeof *nc, _Alignof(H2ctNoteClose));
}

static void h2ct_note_dialer_use(H2ctNoteDialer *d, HttpTransport *tr) {
    tr->dial = BURROW_FN(HttpDialFunc, h2ct_note_close_dial, d);
    tr->free_conn = BURROW_FN(HttpFreeConnFunc, h2ct_note_close_free, d);
}

static void h2ct_hi_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    h2ct_write_string(w, BURROW_S("hi"));
}

/* A request through c, its body read and closed. */
static bool h2ct_get_hi(H2ctTS *s, HttpClient *c) {
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(c, s->ts->url, &err);
    if (res == NULL) {
        testing_t_errorf_v(s->t, "%v", err);
        return false;
    }
    Slice b =
        io_read_all(heap_allocator(), io_read_closer_as_io_reader(res->body), &err);
    if (b.p != NULL)
        mem_free(heap_allocator(), b.p, (size_t)b.cap, 1);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(s->t, "%v", err);
    (void)res->body.vt->closer.close(res->body.data);
    http_response_free(res);
    return !BURROW_FAILED(err);
}

static void h2ct_set_bool(void *env) {
    sync_atomic_bool_store((SyncAtomicBool *)env, true);
}

/* Test that the http1 Transport.DisableKeepAlives option is respected and
 * connections are closed as soon as idle. */
static void TestTransportDisableKeepAlives(TestingT *t) {
    H2ctTS s;
    SyncAtomicBool conn_closed;
    memset(&conn_closed, 0, sizeof conn_closed);
    H2ctNoteDialer d;
    memset(&d, 0, sizeof d);
    d.closefn = BURROW_FN(Func, h2ct_set_bool, &conn_closed);
    if (h2ct_ts_start(&s, t, h2ct_hi_handler, NULL)) {
        h2ct_note_dialer_use(&d, &s.tr);
        s.tr.disable_keep_alives = true;
        HttpClient c = {0};
        c.transport = http_transport_as_round_tripper(&s.tr);
        if (h2ct_get_hi(&s, &c)) {
            Time deadline = time_add(time_now(), TIME_SECOND);
            while (!sync_atomic_bool_load(&conn_closed) &&
                   time_before(time_now(), deadline))
                time_sleep(TIME_MILLISECOND);
            if (!sync_atomic_bool_load(&conn_closed))
                testing_t_errorf_v(t, "timeout");
        }
    }
    h2ct_ts_close(&s);
}

enum { H2CT_KA_D = 25 }; /* milliseconds */

static void h2ct_slow_hi_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    time_sleep((Duration)H2CT_KA_D * TIME_MILLISECOND);
    h2ct_write_string(w, BURROW_S("hi"));
}

typedef struct H2ctKAConc {
    H2ctTS *s;
    HttpClient c;
} H2ctKAConc;

static void h2ct_ka_conc_job(void *env) {
    H2ctKAConc *k = (H2ctKAConc *)env;
    (void)h2ct_get_hi(k->s, &k->c);
}

static void h2ct_wg_done(void *env) {
    sync_wait_group_done((SyncWaitGroup *)env);
}

/* Test concurrent requests with Transport.DisableKeepAlives. We can share
 * connections, but when things are totally idle, it still needs to close. */
static void TestTransportDisableKeepAlives_Concurrency(TestingT *t) {
    enum { N = 20 };
    H2ctTS s;
    SyncWaitGroup conns = {0};
    H2ctNoteDialer d;
    memset(&d, 0, sizeof d);
    d.closefn = BURROW_FN(Func, h2ct_wg_done, &conns);
    d.conns = &conns;
    if (h2ct_ts_start(&s, t, h2ct_slow_hi_handler, NULL)) {
        h2ct_note_dialer_use(&d, &s.tr);
        s.tr.disable_keep_alives = true;
        H2ctKAConc k;
        memset(&k, 0, sizeof k);
        k.s = &s;
        k.c.transport = http_transport_as_round_tripper(&s.tr);
        SyncWaitGroup reqs = {0};
        for (int i = 0; i < N; i++) {
            if (i == N - 1) {
                /* For the final request, try to make all the others close.
                 * This isn't verified in the count, other than the Log
                 * statement, since it's so timing dependent. This test is
                 * really to make sure we don't interrupt a valid request. */
                time_sleep((Duration)(2 * H2CT_KA_D) * TIME_MILLISECOND);
            }
            if (!sync_wait_group_go(&reqs, BURROW_FN(Func, h2ct_ka_conc_job, &k)))
                testing_t_errorf_v(t, "no goroutine for request %d", i);
        }
        sync_wait_group_wait(&reqs);
        sync_wait_group_wait(&conns);
        testing_t_logf_v(t, "did %d dials, %d requests",
                         (int)sync_atomic_int32_load(&d.dials), N);
    }
    h2ct_ts_close(&s);
}

static void h2ct_no_compression_handler(void *env, HttpResponseWriter w,
                                        HttpRequest *r) {
    (void)w;
    TestingT *t = (TestingT *)env;
    Slice ua = http_header_values(r->header, BURROW_S("User-Agent"));
    if (map_len(r->header) != 1 || ua.len != 1 ||
        !str_eq(((const Str *)ua.p)[0], BURROW_S("Go-http-client/2.0"))) {
        BytesBuffer b = BYTES_BUFFER(heap_allocator());
        (void)http_header_write(r->header, bytes_buffer_as_io_writer(&b));
        testing_t_errorf_v(
            t, "request headers = %q; want %q",
            str_from_bytes(bytes_buffer_bytes(&b).p, bytes_buffer_len(&b)),
            BURROW_S("User-Agent: Go-http-client/2.0\r\n"));
        bytes_buffer_free(&b);
    }
}

static void TestTransportDisableCompression(TestingT *t) {
    H2ctTS s;
    if (h2ct_ts_start(&s, t, h2ct_no_compression_handler, t)) {
        s.tr.disable_compression = true;
        HttpRequest *req = h2ct_ts_request(&s, (Context){0}, BURROW_S("GET"), "");
        Error err = BURROW_NO_ERROR;
        HttpResponse *res =
            req != NULL ? http_transport_round_trip(&s.tr, req, &err) : NULL;
        if (req != NULL && res == NULL)
            testing_t_errorf_v(t, "%v", err);
        else if (res != NULL)
            (void)res->body.vt->closer.close(res->body.data);
        http_response_free(res);
        http_request_free(req);
    }
    h2ct_ts_close(&s);
}

/* Sets Got-Header to the request's header keys, sorted and joined with ",".
 * The header holds the value without copying it, so it goes out before the
 * arena it is in does. */
static void h2ct_got_header_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Slice got = slice_from(NULL, 0, 0, TYPE_STRING);
    const void *k;
    for (MapIter it = map_iter(r->header); map_next(&it, &k, NULL);)
        got = slice_append(a, got, k, 1);
    sort_strings(got);
    (void)http_header_set(http_response_writer_header(w), BURROW_S("Got-Header"),
                          strings_join(a, got, BURROW_S(",")));
    http_response_writer_write_header(w, 200);
    (void)w.vt->flush(w.data);
    arena_free(&ar);
}

/* RFC 7540 section 8.1.2.2 */
static void TestTransportRejectsConnHeaders(TestingT *t) {
    static const struct {
        Str value[2];
        const char *key;
        Int nvalue;
        const char *want;
    } tests[] = {
        {{S_("anything")},
         "Upgrade",
         1,
         "ERROR: http2: invalid Upgrade request header: [\"anything\"]"},
        {{S_("foo")},
         "Connection",
         1,
         "ERROR: http2: invalid Connection request header: [\"foo\"]"},
        {{S_("close")}, "Connection", 1, "Accept-Encoding,User-Agent"},
        {{S_("CLoSe")}, "Connection", 1, "Accept-Encoding,User-Agent"},
        {{S_("close"), S_("something-else")},
         "Connection",
         2,
         "ERROR: http2: invalid Connection request header: [\"close\" "
         "\"something-else\"]"},
        {{S_("keep-alive")}, "Connection", 1, "Accept-Encoding,User-Agent"},
        {{S_("Keep-ALIVE")}, "Connection", 1, "Accept-Encoding,User-Agent"},
        /* just deleted and ignored */
        {{S_("keep-alive")}, "Proxy-Connection", 1, "Accept-Encoding,User-Agent"},
        {{S_("")}, "Transfer-Encoding", 1, "Accept-Encoding,User-Agent"},
        {{S_("foo")},
         "Transfer-Encoding",
         1,
         "ERROR: http2: invalid Transfer-Encoding request header: [\"foo\"]"},
        {{S_("chunked")}, "Transfer-Encoding", 1, "Accept-Encoding,User-Agent"},
        /* Go's comment says Kelvin sign, but its K is the ASCII one. */
        {{S_("chunKed")},
         "Transfer-Encoding",
         1,
         "ERROR: http2: invalid Transfer-Encoding request header: [\"chunKed\"]"},
        {{S_("chunked"), S_("other")},
         "Transfer-Encoding",
         2,
         "ERROR: http2: invalid Transfer-Encoding request header: [\"chunked\" "
         "\"other\"]"},
        {{S_("123")}, "Content-Length", 1, "Accept-Encoding,User-Agent"},
        {{S_("doop")}, "Keep-Alive", 1, "Accept-Encoding,User-Agent"},
    };
    H2ctTS s;
    if (h2ct_ts_start(&s, t, h2ct_got_header_handler, NULL)) {
        Alloc *a = arena_allocator(&s.ar);
        for (int i = 0; i < (int)(sizeof tests / sizeof tests[0]); i++) {
            HttpRequest *req = h2ct_ts_request(&s, (Context){0}, BURROW_S("GET"), "");
            if (req == NULL)
                break;
            Str key = str_from_cstr(tests[i].key);
            Slice value = slice_from((void *)(uintptr_t)tests[i].value, tests[i].nvalue,
                                     tests[i].nvalue, TYPE_STRING);
            if (!map_set(req->header, &key, &value)) {
                testing_t_errorf_v(t, "no memory");
                http_request_free(req);
                break;
            }
            Error err = BURROW_NO_ERROR;
            HttpResponse *res = http_transport_round_trip(&s.tr, req, &err);
            Str got;
            if (res == NULL)
                got = fmt_sprintf_v(a, "ERROR: %v", err);
            else {
                got = http_header_get(res->header, BURROW_S("Got-Header"));
                (void)res->body.vt->closer.close(res->body.data);
            }
            if (!str_eq(got, str_from_cstr(tests[i].want)))
                testing_t_errorf_v(t, "For key %q, value %v, got = %q; want %q", key,
                                   value, got, str_from_cstr(tests[i].want));
            http_response_free(res);
            http_request_free(req);
        }
    }
    h2ct_ts_close(&s);
}

typedef struct H2ctSignCase {
    const char *name;
    const char *cl;
    const char *want_cl;
} H2ctSignCase;

static void h2ct_cl_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    const H2ctSignCase *tc = (const H2ctSignCase *)env;
    (void)http_header_set(http_response_writer_header(w), BURROW_S("Content-Length"),
                          str_from_cstr(tc->cl));
}

static void h2ct_rejects_cl_with_sign(void *env, TestingT *t) {
    const H2ctSignCase *tc = (const H2ctSignCase *)env;
    H2ctTS s;
    if (h2ct_ts_start(&s, t, h2ct_cl_handler, (void *)(uintptr_t)tc)) {
        HttpRequest *req = h2ct_ts_request(&s, (Context){0}, BURROW_S("HEAD"), "");
        Error err = BURROW_NO_ERROR;
        HttpResponse *res =
            req != NULL ? http_transport_round_trip(&s.tr, req, &err) : NULL;
        Str got = BURROW_STR_EMPTY;
        if (res == NULL)
            got = fmt_sprintf_v(arena_allocator(&s.ar), "ERROR: %v", err);
        else {
            got = http_header_get(res->header, BURROW_S("Content-Length"));
            (void)res->body.vt->closer.close(res->body.data);
        }
        if (req != NULL && !str_eq(got, str_from_cstr(tc->want_cl)))
            testing_t_errorf_v(t, "Got: %q\nWant: %q", got, str_from_cstr(tc->want_cl));
        http_response_free(res);
        http_request_free(req);
    }
    h2ct_ts_close(&s);
}

/* Reject content-length headers containing a sign. */
static void TestTransportRejectsContentLengthWithSign(TestingT *t) {
    static const H2ctSignCase tests[] = {
        {"proper content-length", "3", "3"},
        {"ignore cl with plus sign", "+3", ""},
        {"ignore cl with minus sign", "-3", ""},
        {"max int64, for safe uint64->int64 conversion", "9223372036854775807",
         "9223372036854775807"},
        {"overflows int64, so ignored", "9223372036854775808", ""},
    };
    for (int i = 0; i < (int)(sizeof tests / sizeof tests[0]); i++) {
        (void)testing_t_run(t, str_from_cstr(tests[i].name),
                            BURROW_FN(TestingTFunc, h2ct_rejects_cl_with_sign,
                                      (void *)(uintptr_t)&tests[i]));
    }
}

static void h2ct_invalid_headers(H2ctTS *s, int i, const Str *hkey, const Str *hval,
                                 const Str *tkey, const Str *tval,
                                 const char *want_err) {
    TestingT *t = s->t;
    Alloc *a = arena_allocator(&s->ar);
    HttpRequest *req = h2ct_ts_request(s, (Context){0}, BURROW_S("GET"), "");
    if (req == NULL)
        return;
    /* req.Header = tt.h, and req.Trailer = tt.t. */
    req->header = http_header_make(a);
    Slice hv = slice_from((void *)(uintptr_t)hval, 1, 1, TYPE_STRING);
    Slice tv = slice_from((void *)(uintptr_t)tval, 1, 1, TYPE_STRING);
    if (tkey != NULL)
        req->trailer = http_header_make(a);
    if (req->header == NULL || (tkey != NULL && req->trailer == NULL) ||
        (hkey != NULL && !map_set(req->header, hkey, &hv)) ||
        (tkey != NULL && !map_set(req->trailer, tkey, &tv))) {
        testing_t_errorf_v(t, "no memory");
        http_request_free(req);
        return;
    }
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_transport_round_trip(&s->tr, req, &err);
    bool bad = false;
    if (want_err == NULL) {
        if (res == NULL) {
            bad = true;
            testing_t_errorf_v(t, "case %d: error = %v; want no error", i, err);
        }
    } else if (res != NULL ||
               !strings_contains(error_text(err), str_from_cstr(want_err))) {
        bad = true;
        testing_t_errorf_v(t, "case %d: error = %v; want error %q", i, err,
                           str_from_cstr(want_err));
    }
    if (res != NULL) {
        if (bad)
            testing_t_logf_v(t, "case %d: server got headers %q", i,
                             http_header_get(res->header, BURROW_S("Got-Header")));
        (void)res->body.vt->closer.close(res->body.data);
    }
    http_response_free(res);
    http_request_free(req);
}

static void TestTransportFailsOnInvalidHeadersAndTrailers(TestingT *t) {
    static const struct {
        Str hkey, hval, tkey, tval;
        const char *want_err;
    } tests[] = {
        {S_("with space"), S_("foo"), S_(""), S_(""),
         "net/http: invalid header field name \"with space\""},
        /* name: Брэд, which is okay */
        {S_("name"), S_("\xd0\x91\xd1\x80\xd1\x8d\xd0\xb4"), S_(""), S_(""), NULL},
        /* имя: Brad */
        {S_("\xd0\xb8\xd0\xbc\xd1\x8f"), S_("Brad"), S_(""), S_(""),
         "net/http: invalid header field name \"\xd0\xb8\xd0\xbc\xd1\x8f\""},
        {S_("foo"),
         S_("foo\x01"
            "bar"),
         S_(""), S_(""), "net/http: invalid header field value for \"foo\""},
        {S_(""), S_(""), S_("foo"),
         S_("foo\x01"
            "bar"),
         "net/http: invalid trailer field value for \"foo\""},
        {S_(""), S_(""), S_("x-\r\nda"),
         S_("foo\x01"
            "bar"),
         "net/http: invalid trailer field name \"x-\\r\\nda\""},
    };
    H2ctTS s;
    if (h2ct_ts_start(&s, t, h2ct_got_header_handler, NULL)) {
        for (int i = 0; i < (int)(sizeof tests / sizeof tests[0]); i++)
            h2ct_invalid_headers(&s, i, tests[i].hkey.len != 0 ? &tests[i].hkey : NULL,
                                 &tests[i].hval,
                                 tests[i].tkey.len != 0 ? &tests[i].tkey : NULL,
                                 &tests[i].tval, tests[i].want_err);
    }
    h2ct_ts_close(&s);
}

/* byteAndEOFReader: one byte and io.EOF from every Read. */
static Int h2ct_byte_and_eof_read(void *self, Slice p, Error *err) {
    if (p.len == 0)
        panic_str(BURROW_S("unexpected useless call"));
    ((Byte *)p.p)[0] = *(const Byte *)self;
    *err = io_eof;
    return 1;
}

static const IoReaderVT h2ct_byte_and_eof_vt = {NULL, h2ct_byte_and_eof_read};

static void TestTransportBodyDoubleEndStream(TestingT *t) {
    H2ctTS s;
    HttpRequest *reqs[2] = {NULL, NULL};
    HttpResponse *ress[2] = {NULL, NULL};
    Byte a = 'a';
    if (h2ct_ts_start(&s, t, NULL, NULL)) {
        for (int i = 0; i < 2; i++) {
            Error err = BURROW_NO_ERROR;
            reqs[i] = http_new_request(heap_allocator(), BURROW_S("POST"), s.ts->url,
                                       (IoReader){&h2ct_byte_and_eof_vt, &a}, &err);
            if (reqs[i] == NULL) {
                testing_t_errorf_v(t, "NewRequest: %v", err);
                break;
            }
            reqs[i]->content_length = 1;
            ress[i] = http_transport_round_trip(&s.tr, reqs[i], &err);
            if (ress[i] == NULL) {
                testing_t_errorf_v(t, "failure on req %d: %v", i + 1, err);
                break;
            }
        }
    }
    for (int i = 1; i >= 0; i--) {
        if (ress[i] != NULL)
            (void)ress[i]->body.vt->closer.close(ress[i]->body.data);
        http_response_free(ress[i]);
        http_request_free(reqs[i]);
    }
    h2ct_ts_close(&s);
}

#define H2CT_HELLO_MSG "Hello."

typedef struct H2ctCancelRace {
    Chan *client_got_response;
    ContextCancelFunc cancel;
} H2ctCancelRace;

static void h2ct_cancel_race_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    H2ctCancelRace *cr = (H2ctCancelRace *)env;
    if (strings_contains(r->url->path, BURROW_S("/hello"))) {
        time_sleep(50 * TIME_MILLISECOND);
        h2ct_write_string(w, BURROW_S(H2CT_HELLO_MSG));
        return;
    }
    for (int i = 0; i < 50; i++) {
        h2ct_write_string(w, BURROW_S("Some data."));
        (void)w.vt->flush(w.data);
        if (i == 2) {
            bool v;
            (void)chan_recv(cr->client_got_response, &v);
            BURROW_CALLF0(cr->cancel);
        }
        time_sleep(10 * TIME_MILLISECOND);
    }
}

static void h2ct_cancel_race(H2ctTS *s, H2ctCancelRace *cr, Context ctx) {
    TestingT *t = s->t;
    HttpClient c = {0};
    c.transport = http_transport_as_round_tripper(&s->tr);
    HttpRequest *req = h2ct_ts_request(s, ctx, BURROW_S("GET"), "");
    if (req == NULL)
        return;
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_do(&c, req, &err);
    bool v = true;
    chan_send(cr->client_got_response, &v);
    if (res == NULL) {
        testing_t_errorf_v(t, "%v", err);
        http_request_free(req);
        return;
    }
    (void)io_copy(heap_allocator(), io_discard, io_read_closer_as_io_reader(res->body),
                  &err);
    bool ok = BURROW_FAILED(err);
    if (!ok)
        testing_t_errorf_v(t, "unexpected success");
    http_response_free(res);
    http_request_free(req);
    if (!ok)
        return;

    err = BURROW_NO_ERROR;
    res = http_client_get(&c, h2ct_ts_url(s, "/hello"), &err);
    if (res == NULL) {
        testing_t_errorf_v(t, "%v", err);
        return;
    }
    Str slurp = BURROW_STR_EMPTY;
    if (h2ct_ts_read_body(s, res, &slurp) && !str_eq(slurp, BURROW_S(H2CT_HELLO_MSG)))
        testing_t_errorf_v(t, "Got = %q; want %q", slurp, BURROW_S(H2CT_HELLO_MSG));
    http_response_free(res);
}

/* Go cancels the request with Request.Cancel, which the C Request doesn't
 * have. Cancelling its context does the same thing to a request. */
static void TestTransportCancelDataResponseRace(TestingT *t) {
    H2ctTS s;
    H2ctCancelRace cr;
    memset(&cr, 0, sizeof cr);
    cr.client_got_response = chan_make(heap_allocator(), TYPE_BOOL, 1);
    if (h2ct_ts_start(&s, t, h2ct_cancel_race_handler, &cr)) {
        if (cr.client_got_response == NULL)
            testing_t_errorf_v(t, "no memory");
        else {
            Context ctx = context_with_cancel(arena_allocator(&s.ar),
                                              context_background(), &cr.cancel);
            h2ct_cancel_race(&s, &cr, ctx);
            BURROW_CALLF0(cr.cancel);
            context_release(ctx);
        }
    }
    h2ct_ts_close(&s);
    chan_free(cr.client_got_response);
}

static void h2ct_body_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    http_response_writer_write_header(w, 200);
    h2ct_write_string(w, BURROW_S("body"));
}

/* Issue 21316: It should be safe to reuse an http.Request after the request
 * has completed. */
static void TestTransportNoRaceOnRequestObjectAfterRequestComplete(TestingT *t) {
    H2ctTS s;
    if (h2ct_ts_start(&s, t, h2ct_body_handler, NULL)) {
        HttpRequest *req = h2ct_ts_request(&s, (Context){0}, BURROW_S("GET"), "");
        Error err = BURROW_NO_ERROR;
        HttpResponse *res =
            req != NULL ? http_transport_round_trip(&s.tr, req, &err) : NULL;
        if (req != NULL && res == NULL)
            testing_t_errorf_v(t, "%v", err);
        else if (res != NULL) {
            (void)io_copy(heap_allocator(), io_discard,
                          io_read_closer_as_io_reader(res->body), &err);
            Error cerr = res->body.vt->closer.close(res->body.data);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "error reading response body: %v", err);
            else if (BURROW_FAILED(cerr))
                testing_t_errorf_v(t, "error closing response body: %v", cerr);
            else {
                /* This access of req.Header should not race with code in the
                 * transport. */
                req->header = http_header_make(arena_allocator(&s.ar));
            }
        }
        http_response_free(res);
        http_request_free(req);
    }
    h2ct_ts_close(&s);
}

/* The other end of the pipe TestTransportPingWriteBlocks dials, which reads
 * what the client writes first and then nothing more. */
typedef struct H2ctPingBlock {
    NetConn s;
    SyncWaitGroup wg;
    SyncAtomicInt32 dials;
} H2ctPingBlock;

static void h2ct_ping_block_job(void *env) {
    H2ctPingBlock *pb = (H2ctPingBlock *)env;
    /* Read initial handshake frames. Without this, we block indefinitely in
     * newClientConn, and never get to the point of sending a PING. */
    Byte buf[1024];
    Error err = BURROW_NO_ERROR;
    (void)pb->s.vt->reader.read(
        pb->s.data, slice_from(buf, sizeof buf, sizeof buf, TYPE_BYTE), &err);
}

static NetConn h2ct_ping_block_dial(void *env, Str network, Str addr, Error *err) {
    (void)network;
    (void)addr;
    H2ctPingBlock *pb = (H2ctPingBlock *)env;
    NetConn none = {NULL, NULL};
    /* One is all the test needs, and the one end it keeps. */
    if (sync_atomic_int32_add(&pb->dials, 1) != 1) {
        *err = errors_new(error_allocator(), BURROW_S("one dial only"));
        return none;
    }
    NetConn c;
    net_pipe(heap_allocator(), &pb->s, &c); /* unbuffered, unlike a TCP conn */
    if (pb->s.vt == NULL) {
        *err = burrow_err_out_of_memory;
        return none;
    }
    if (!sync_wait_group_go(&pb->wg, BURROW_FN(Func, h2ct_ping_block_job, pb))) {
        net_conn_free(c);
        *err = burrow_err_out_of_memory;
        return none;
    }
    *err = BURROW_NO_ERROR;
    return c;
}

static void TestTransportPingWriteBlocks(TestingT *t) {
    H2ctTS s;
    H2ctPingBlock pb;
    memset(&pb, 0, sizeof pb);
    if (h2ct_ts_start(&s, t, NULL, NULL)) {
        s.tr.dial = BURROW_FN(HttpDialFunc, h2ct_ping_block_dial, &pb);
        s.h2.ping_timeout = TIME_MILLISECOND;
        s.h2.send_ping_timeout = TIME_MILLISECOND;
        HttpClient c = {0};
        c.transport = http_transport_as_round_tripper(&s.tr);
        Error err = BURROW_NO_ERROR;
        HttpResponse *res = http_client_get(&c, s.ts->url, &err);
        if (res != NULL) {
            testing_t_errorf_v(t, "Get = nil, want error");
            (void)res->body.vt->closer.close(res->body.data);
        }
        http_response_free(res);
    }
    h2ct_ts_close(&s);
    if (pb.s.vt != NULL) {
        (void)pb.s.vt->closer.close(pb.s.data);
        sync_wait_group_wait(&pb.wg);
        net_conn_free(pb.s);
    }
}

static NetConn h2ct_count_dial(void *env, Str network, Str addr, Error *err) {
    (void)sync_atomic_int32_add((SyncAtomicInt32 *)env, 1);
    return net_dial(heap_allocator(), network, addr, err);
}

static void TestTransportRequestsLowServerLimit(TestingT *t) {
    enum { REQ_COUNT = 3 };
    H2ctTS s;
    HttpHTTP2Config h2;
    memset(&h2, 0, sizeof h2);
    h2.max_concurrent_streams = 1;
    SyncAtomicInt32 conn_count;
    memset(&conn_count, 0, sizeof conn_count);
    if (h2ct_ts_new(&s, t, NULL, NULL)) {
        s.ts->config.http2 = &h2;
        httptest_server_start(s.ts);
        s.tr.dial = BURROW_FN(HttpDialFunc, h2ct_count_dial, &conn_count);
        for (int i = 0; i < REQ_COUNT; i++) {
            HttpRequest *req = h2ct_ts_request(&s, (Context){0}, BURROW_S("GET"), "");
            if (req == NULL)
                break;
            Error err = BURROW_NO_ERROR;
            HttpResponse *res = http_transport_round_trip(&s.tr, req, &err);
            if (res == NULL) {
                testing_t_errorf_v(t, "%v", err);
                http_request_free(req);
                break;
            }
            if (res->status_code != 200)
                testing_t_errorf_v(t, "StatusCode = %d; want %d", res->status_code,
                                   200);
            (void)res->body.vt->closer.close(res->body.data);
            http_response_free(res);
            http_request_free(req);
        }
        if (sync_atomic_int32_load(&conn_count) != 1)
            testing_t_errorf_v(t, "created %d connections for %d requests, want 1",
                               (int)sync_atomic_int32_load(&conn_count), REQ_COUNT);
    }
    h2ct_ts_close(&s);
}

static void h2ct_ok_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    http_response_writer_write_header(w, HTTP_STATUS_OK);
}

/* infiniteReader */
static Int h2ct_infinite_read(void *self, Slice p, Error *err) {
    (void)self;
    *err = BURROW_NO_ERROR;
    return p.len;
}

static const IoReaderVT h2ct_infinite_vt = {NULL, h2ct_infinite_read};

/* Issue 20521: it is not an error to receive a response and end stream from
 * the server without the body being consumed. */
static void TestTransportResponseAndResetWithoutConsumingBodyRace(TestingT *t) {
    H2ctTS s;
    if (h2ct_ts_start(&s, t, h2ct_ok_handler, NULL)) {
        /* The request body needs to be big enough to trigger flow control. */
        Error err = BURROW_NO_ERROR;
        HttpRequest *req =
            http_new_request(heap_allocator(), BURROW_S("PUT"), s.ts->url,
                             (IoReader){&h2ct_infinite_vt, NULL}, &err);
        HttpResponse *res =
            req != NULL ? http_transport_round_trip(&s.tr, req, &err) : NULL;
        if (res == NULL)
            testing_t_errorf_v(t, "%v", err);
        else {
            if (res->status_code != HTTP_STATUS_OK)
                testing_t_errorf_v(t, "Response code = %d; want %d", res->status_code,
                                   HTTP_STATUS_OK);
            (void)res->body.vt->closer.close(res->body.data);
        }
        http_response_free(res);
        http_request_free(req);
    }
    h2ct_ts_close(&s);
}

/* chunkReader */
typedef struct H2ctChunkReader {
    const char *const *chunks;
    int n;
} H2ctChunkReader;

static Int h2ct_chunk_read(void *self, Slice p, Error *err) {
    H2ctChunkReader *r = (H2ctChunkReader *)self;
    if (r->n == 0)
        panic_str(BURROW_S("shouldn't read this many times"));
    Str c = str_from_cstr(r->chunks[0]);
    Int n = c.len < p.len ? c.len : p.len;
    memcpy(p.p, c.p, (size_t)n);
    r->chunks++;
    r->n--;
    *err = BURROW_NO_ERROR;
    return n;
}

static const IoReaderVT h2ct_chunk_reader_vt = {NULL, h2ct_chunk_read};

static void h2ct_read6_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)w;
    Byte b[6];
    Error err = BURROW_NO_ERROR;
    IoReader body = io_read_closer_as_io_reader(r->body);
    (void)body.vt->read(body.data, slice_from(b, sizeof b, sizeof b, TYPE_BYTE), &err);
}

static void h2ct_body_larger_than_specified_content_length(TestingT *t,
                                                           H2ctChunkReader *body,
                                                           int64_t content_len) {
    H2ctTS s;
    if (h2ct_ts_start(&s, t, h2ct_read6_handler, NULL)) {
        Error err = BURROW_NO_ERROR;
        HttpRequest *req =
            http_new_request(heap_allocator(), BURROW_S("POST"), s.ts->url,
                             (IoReader){&h2ct_chunk_reader_vt, body}, &err);
        HttpResponse *res = NULL;
        if (req == NULL)
            testing_t_errorf_v(t, "NewRequest: %v", err);
        else {
            req->content_length = content_len;
            res = http_transport_round_trip(&s.tr, req, &err);
            if (res != NULL || !errors_is(err, burrow__http2_err_req_body_too_long))
                testing_t_errorf_v(t, "expected %v, got %v",
                                   burrow__http2_err_req_body_too_long, err);
        }
        if (res != NULL)
            (void)res->body.vt->closer.close(res->body.data);
        http_response_free(res);
        http_request_free(req);
    }
    h2ct_ts_close(&s);
}

/* Issue 32254: if the request body is larger than the specified content
 * length, the client should refuse to send the extra part and abort the
 * stream.
 *
 * In _len3 case, the first Read() matches the expected content length but
 * the second read returns more data.
 *
 * In _len2 case, the first Read() exceeds the expected content length. */
static void TestTransportBodyLargerThanSpecifiedContentLength_len3(TestingT *t) {
    static const char *const chunks[] = {"123", "456"};
    H2ctChunkReader body = {chunks, 2};
    h2ct_body_larger_than_specified_content_length(t, &body, 3);
}

static void TestTransportBodyLargerThanSpecifiedContentLength_len2(TestingT *t) {
    static const char *const chunks[] = {"123"};
    H2ctChunkReader body = {chunks, 1};
    h2ct_body_larger_than_specified_content_length(t, &body, 2);
}

static void h2ct_conn_close_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    (void)http_header_set(http_response_writer_header(w), BURROW_S("Connection"),
                          BURROW_S("close"));
    http_response_writer_write_header(w, HTTP_STATUS_OK);
}

typedef struct H2ctRewind {
    HttpClient *client;
    HttpRequest *req;
} H2ctRewind;

static void h2ct_rewind_job(void *env) {
    H2ctRewind *rw = (H2ctRewind *)env;
    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_do(rw->client, rw->req, &err);
    if (res != NULL)
        (void)res->body.vt->closer.close(res->body.data);
    http_response_free(res);
}

/* Issue 31192: A failed request may be retried if the body has not been read
 * already. If the request body has started to be sent, one must wait until it
 * is completed. */
static void TestTransportBodyRewindRace(TestingT *t) {
    enum { CLIENTS = 50 };
    H2ctTS s;
    H2ctRewind rw[CLIENTS];
    memset(rw, 0, sizeof rw);
    if (h2ct_ts_start(&s, t, h2ct_conn_close_handler, NULL)) {
        s.tr.max_conns_per_host = 1;
        HttpClient client = {0};
        client.transport = http_transport_as_round_tripper(&s.tr);
        SyncWaitGroup wg = {0};
        for (int i = 0; i < CLIENTS; i++) {
            StringsReader *sr =
                strings_new_reader(arena_allocator(&s.ar), BURROW_S("abcdef"));
            Error err = BURROW_NO_ERROR;
            rw[i].req =
                sr == NULL
                    ? NULL
                    : http_new_request(heap_allocator(), BURROW_S("POST"), s.ts->url,
                                       strings_reader_as_io_reader(sr), &err);
            if (rw[i].req == NULL) {
                testing_t_errorf_v(t, "unexpected new request error: %v", err);
                break;
            }
            rw[i].client = &client;
            if (!sync_wait_group_go(&wg, BURROW_FN(Func, h2ct_rewind_job, &rw[i])))
                testing_t_errorf_v(t, "no goroutine for request %d", i);
        }
        sync_wait_group_wait(&wg);
    }
    for (int i = 0; i < CLIENTS; i++)
        http_request_free(rw[i].req);
    h2ct_ts_close(&s);
}

/* errorReader */
static Int h2ct_error_read(void *self, Slice p, Error *err) {
    (void)p;
    *err = *(const Error *)self;
    return 0;
}

static const IoReaderVT h2ct_error_reader_vt = {NULL, h2ct_error_read};

static void h2ct_unauthorized_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    http_response_writer_write_header(w, HTTP_STATUS_UNAUTHORIZED);
}

/* Issue 42498: A request with a body will never be sent if the stream is
 * reset prior to sending any data. */
static void TestTransportServerResetStreamAtHeaders(TestingT *t) {
    H2ctTS s;
    Error eof = io_eof;
    if (h2ct_ts_start(&s, t, h2ct_unauthorized_handler, NULL)) {
        s.tr.max_conns_per_host = 1;
        s.tr.expect_continue_timeout = 10 * TIME_SECOND;
        HttpClient client = {0};
        client.transport = http_transport_as_round_tripper(&s.tr);
        Error err = BURROW_NO_ERROR;
        HttpRequest *req =
            http_new_request(heap_allocator(), BURROW_S("POST"), s.ts->url,
                             (IoReader){&h2ct_error_reader_vt, &eof}, &err);
        HttpResponse *res = NULL;
        if (req == NULL)
            testing_t_errorf_v(t, "unexpected new request error: %v", err);
        else {
            req->content_length = 0; /* so transport is tempted to sniff it */
            (void)http_header_set(req->header, BURROW_S("Expect"),
                                  BURROW_S("100-continue"));
            res = http_client_do(&client, req, &err);
            if (res == NULL)
                testing_t_errorf_v(t, "%v", err);
            else
                (void)res->body.vt->closer.close(res->body.data);
        }
        http_response_free(res);
        http_request_free(req);
    }
    h2ct_ts_close(&s);
}

/* trackingReader */
typedef struct H2ctTrackingReader {
    StringsReader rdr;
    SyncAtomicBool was_read;
} H2ctTrackingReader;

static Int h2ct_tracking_read(void *self, Slice p, Error *err) {
    H2ctTrackingReader *tr = (H2ctTrackingReader *)self;
    sync_atomic_bool_store(&tr->was_read, true);
    IoReader r = strings_reader_as_io_reader(&tr->rdr);
    return r.vt->read(r.data, p, err);
}

static const IoReaderVT h2ct_tracking_reader_vt = {NULL, h2ct_tracking_read};

static void h2ct_expect_continue_handler(void *env, HttpResponseWriter w,
                                         HttpRequest *r) {
    (void)env;
    if (str_eq(r->url->path, BURROW_S("/reject"))) {
        http_response_writer_write_header(w, 403);
        return;
    }
    Error err = BURROW_NO_ERROR;
    (void)io_copy(heap_allocator(), io_discard, io_read_closer_as_io_reader(r->body),
                  &err);
}

typedef struct H2ctExpectCase {
    const char *name;
    const char *path;
    int expected_code;
    H2ctTS *s;
    HttpClient *client;
    H2ctTrackingReader body; /* the test's, so it outlives the transport */
    bool should_read;
} H2ctExpectCase;

static void h2ct_expect_continue_case(void *env, TestingT *t) {
    H2ctExpectCase *tc = (H2ctExpectCase *)env;
    H2ctTrackingReader *body = &tc->body;
    strings_reader_reset(&body->rdr, BURROW_S("hello"));
    Time start_time = time_now();
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = http_new_request(
        heap_allocator(), BURROW_S("POST"), h2ct_ts_url(tc->s, tc->path),
        (IoReader){&h2ct_tracking_reader_vt, body}, &err);
    if (req == NULL) {
        testing_t_fatalf_v(t, "%v", err);
        return;
    }
    (void)http_header_set(req->header, BURROW_S("Expect"), BURROW_S("100-continue"));
    HttpResponse *res = http_client_do(tc->client, req, &err);
    if (res == NULL) {
        http_request_free(req);
        testing_t_fatalf_v(t, "%v", err);
        return;
    }
    (void)res->body.vt->closer.close(res->body.data);
    if (time_since(start_time) >= tc->s->tr.expect_continue_timeout)
        testing_t_errorf_v(t, "Request didn't finish before expect continue timeout");
    if (res->status_code != tc->expected_code)
        testing_t_errorf_v(t, "Unexpected status code, got %d, expected %d",
                           res->status_code, tc->expected_code);
    if (sync_atomic_bool_load(&body->was_read) != tc->should_read)
        testing_t_errorf_v(t, "Unexpected read status, got %t, expected %t",
                           sync_atomic_bool_load(&body->was_read), tc->should_read);
    http_response_free(res);
    http_request_free(req);
}

static void TestTransportExpectContinue(TestingT *t) {
    H2ctTS s;
    HttpClient client = {0};
    H2ctExpectCase test_cases[2];
    memset(test_cases, 0, sizeof test_cases);
    test_cases[0].name = "read-all";
    test_cases[0].path = "/";
    test_cases[0].expected_code = 200;
    test_cases[0].should_read = true;
    test_cases[1].name = "reject";
    test_cases[1].path = "/reject";
    test_cases[1].expected_code = 403;
    if (h2ct_ts_start(&s, t, h2ct_expect_continue_handler, NULL)) {
        s.tr.max_conns_per_host = 1;
        s.tr.expect_continue_timeout = 10 * TIME_SECOND;
        client.transport = http_transport_as_round_tripper(&s.tr);
        for (int i = 0; i < 2; i++) {
            test_cases[i].s = &s;
            test_cases[i].client = &client;
            (void)testing_t_run(
                t, str_from_cstr(test_cases[i].name),
                BURROW_FN(TestingTFunc, h2ct_expect_continue_case, &test_cases[i]));
        }
    }
    h2ct_ts_close(&s);
}

typedef struct H2ctFiller {
    TestingT *t;
    H2ctTS *s;
    Str filler;
} H2ctFiller;

static void h2ct_frame_buffer_reuse_handler(void *env, HttpResponseWriter w,
                                            HttpRequest *r) {
    (void)w;
    const H2ctFiller *f = (const H2ctFiller *)env;
    TestingT *t = f->t;
    Str got = http_header_get(r->header, BURROW_S("Big"));
    if (!str_eq(got, f->filler))
        testing_t_errorf_v(t, "r.Header.Get(\"Big\") = %q, want %q", got, f->filler);
    Error err = BURROW_NO_ERROR;
    Slice b = io_read_all(heap_allocator(), io_read_closer_as_io_reader(r->body), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "error reading request body: %v", err);
    got = str_from_bytes((const Byte *)b.p, b.len);
    if (!str_eq(got, f->filler))
        testing_t_errorf_v(t, "request body = %q, want %q", got, f->filler);
    if (b.p != NULL)
        mem_free(heap_allocator(), b.p, (size_t)b.cap, 1);
    got = http_header_get(r->trailer, BURROW_S("Big"));
    if (!str_eq(got, f->filler))
        testing_t_errorf_v(t, "r.Trailer.Get(\"Big\") = %q, want %q", got, f->filler);
}

static void h2ct_frame_buffer_reuse_job(void *env) {
    const H2ctFiller *f = (const H2ctFiller *)env;
    TestingT *t = f->t;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StringsReader *sr = strings_new_reader(a, f->filler);
    Error err = BURROW_NO_ERROR;
    HttpRequest *req =
        sr == NULL ? NULL
                   : http_new_request(heap_allocator(), BURROW_S("POST"), f->s->ts->url,
                                      strings_reader_as_io_reader(sr), &err);
    HttpResponse *res = NULL;
    if (req == NULL)
        testing_t_errorf_v(t, "%v", err);
    else {
        (void)http_header_set(req->header, BURROW_S("Big"), f->filler);
        req->trailer = http_header_make(a);
        if (req->trailer == NULL ||
            !http_header_set(req->trailer, BURROW_S("Big"), f->filler))
            testing_t_errorf_v(t, "no memory");
        else
            res = http_transport_round_trip(&f->s->tr, req, &err);
        if (req->trailer != NULL && res == NULL)
            testing_t_errorf_v(t, "%v", err);
    }
    if (res != NULL) {
        if (res->status_code != 200)
            testing_t_errorf_v(t, "StatusCode = %d; want %d", res->status_code, 200);
        (void)res->body.vt->closer.close(res->body.data);
    }
    http_response_free(res);
    http_request_free(req);
    arena_free(&ar);
}

/* Write several requests to a ClientConn at the same time, looking for race
 * conditions. See golang.org/issue/48340 */
static void TestTransportFrameBufferReuse(TestingT *t) {
    H2ctTS s;
    H2ctFiller f = {t, &s, BURROW_STR_EMPTY};
    if (h2ct_ts_new(&s, t, h2ct_frame_buffer_reuse_handler, &f)) {
        Alloc *a = arena_allocator(&s.ar);
        Byte *rs = h2ct_rand_string(a, 2048);
        if (rs != NULL)
            f.filler = hex_encode_to_string(a, slice_from(rs, 2048, 2048, TYPE_BYTE));
        if (f.filler.len != 4096)
            testing_t_errorf_v(t, "no memory");
        else {
            httptest_server_start(s.ts);
            SyncWaitGroup wg = {0};
            for (int i = 0; i < 10; i++) {
                if (!sync_wait_group_go(
                        &wg, BURROW_FN(Func, h2ct_frame_buffer_reuse_job, &f)))
                    testing_t_errorf_v(t, "no goroutine for request %d", i);
            }
            sync_wait_group_wait(&wg);
        }
    }
    h2ct_ts_close(&s);
}

/* closeChecker, on an io.Pipe's reader. */
typedef struct H2ctCloseChecker {
    IoPipeReader *pr;
    SyncAtomicBool closed;
} H2ctCloseChecker;

static Int h2ct_ccheck_read(void *self, Slice p, Error *err) {
    H2ctCloseChecker *rc = (H2ctCloseChecker *)self;
    if (sync_atomic_bool_load(&rc->closed)) {
        /* Go's TODO: Consider restructuring the request write to avoid
         * reading from the request body after closing it, and check for
         * read-after-close here. Currently, abortRequestBodyWrite races with
         * writeRequestBody. */
        *err = errors_new(error_allocator(), BURROW_S("read after Body.Close"));
        return 0;
    }
    return io_pipe_reader_read(rc->pr, p, err);
}

static Error h2ct_ccheck_close(void *self) {
    H2ctCloseChecker *rc = (H2ctCloseChecker *)self;
    sync_atomic_bool_store(&rc->closed, true);
    return io_pipe_reader_close(rc->pr);
}

static const IoReadCloserVT h2ct_close_checker_vt = {{NULL, h2ct_ccheck_read},
                                                     {NULL, h2ct_ccheck_close}};

/* The RoundTrip contract says that it will close the request body, but that
 * it may do so in a separate goroutine. Wait a reasonable amount of time
 * before concluding that the body isn't being closed. */
static bool h2ct_ccheck_is_closed(H2ctCloseChecker *rc) {
    Time deadline = time_add(time_now(), 10 * TIME_SECOND);
    while (!sync_atomic_bool_load(&rc->closed) && time_before(time_now(), deadline))
        time_sleep(TIME_MILLISECOND);
    return sync_atomic_bool_load(&rc->closed);
}

static void h2ct_status_handler(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)r;
    http_response_writer_write_header(
        w, (Int)sync_atomic_int32_load((SyncAtomicInt32 *)env));
}

typedef struct H2ctCloseBody {
    H2ctTS *s;
    SyncAtomicInt32 *status_code;
    H2ctCloseChecker *body; /* freed after the transport is gone */
    int status;
} H2ctCloseBody;

static void h2ct_close_request_body(void *env, TestingT *t) {
    const H2ctCloseBody *cb = (const H2ctCloseBody *)env;
    H2ctCloseChecker *body = cb->body;
    sync_atomic_int32_store(cb->status_code, cb->status);
    IoPipeWriter *pw = NULL;
    io_pipe(heap_allocator(), &body->pr, &pw);
    if (body->pr == NULL) {
        testing_t_fatalf_v(t, "no memory");
        return;
    }
    Error err = BURROW_NO_ERROR;
    HttpRequest *req = http_new_request(heap_allocator(), BURROW_S("PUT"),
                                        cb->s->ts->url, h2ct_no_body, &err);
    if (req == NULL) {
        (void)io_pipe_writer_close(pw);
        testing_t_fatalf_v(t, "%v", err);
        return;
    }
    req->body = (IoReadCloser){&h2ct_close_checker_vt, body};
    HttpResponse *res = http_transport_round_trip(&cb->s->tr, req, &err);
    if (res == NULL)
        testing_t_errorf_v(t, "%v", err);
    else
        (void)res->body.vt->closer.close(res->body.data);
    (void)io_pipe_writer_close(pw);
    if (res != NULL && !h2ct_ccheck_is_closed(body))
        testing_t_errorf_v(t, "body not closed after %v", 10 * TIME_SECOND);
    http_response_free(res);
    http_request_free(req);
}

/* Go makes the connection with Transport.NewClientConn and sends the
 * requests on it, which the C Transport doesn't have yet. These go through
 * the transport's pool, which has just the one connection. */
static void TestTransportCloseRequestBody(TestingT *t) {
    static const int statuses[] = {200, 401};
    H2ctTS s;
    SyncAtomicInt32 status_code;
    H2ctCloseChecker bodies[2];
    memset(&status_code, 0, sizeof status_code);
    memset(bodies, 0, sizeof bodies);
    if (h2ct_ts_start(&s, t, h2ct_status_handler, &status_code)) {
        for (int i = 0; i < 2; i++) {
            H2ctCloseBody cb = {&s, &status_code, &bodies[i], statuses[i]};
            Str name = fmt_sprintf_v(arena_allocator(&s.ar), "status=%d", statuses[i]);
            (void)testing_t_run(t, name,
                                BURROW_FN(TestingTFunc, h2ct_close_request_body, &cb));
        }
    }
    h2ct_ts_close(&s);
    for (int i = 0; i < 2; i++)
        io_pipe_free(bodies[i].pr);
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
    X(TestTransportRetryAfterRefusedStream)                                            \
    X(TestTransportReadHeadResponse)                                                   \
    X(TestTransportReadHeadResponseWithBody)                                           \
    X(TestTransportAllocationsAfterResponseBodyClose)                                  \
    X(TestTransportNoBodyMeansNoDATA)                                                  \
    X(TestTransportReturnsErrorOnBadResponseHeaders)                                   \
    X(TestTransportResponseDataBeforeHeaders)                                          \
    X(TestTransportHandlesInvalidStatuslessResponse)                                   \
    X(TestTransportNoRetryOnStreamProtocolError)                                       \
    X(TestTransportDataAfter1xxHeader)                                                 \
    X(TestTransportUsesGoAwayDebugError_RoundTrip)                                     \
    X(TestTransportUsesGoAwayDebugError_Body)                                          \
    X(TestTransportCookieHeaderSplit)                                                  \
    X(TestTransportBodyEagerEndStream)                                                 \
    X(TestTransportChecksResponseHeaderListSize_headers)                               \
    X(TestTransportChecksResponseHeaderListSize_trailers)                              \
    X(TestTransportStreamEndsWhileBodyIsBeingWritten)                                  \
    X(TestTransportClosesConnAfterGoAwayNoStreams)                                     \
    X(TestTransportClosesConnAfterGoAwayLastStream)                                    \
    X(TestTransportSettingsFlowControlUpdateBeyondLimit)                               \
    X(TestTransportSettingsFlowControlUpdateWithinLimit)                               \
    X(TestTransportWindowUpdateBeyondLimit)                                            \
    X(TestTransportAdjustsFlowControl)                                                 \
    X(TestTransportReturnsUnusedFlowControlSingleWrite)                                \
    X(TestTransportReturnsUnusedFlowControlMultipleWrites)                             \
    X(TestTransportGoAwayWithNoConns)                                                  \
    X(TestTransportUnusedConnOK)                                                       \
    X(TestTransportUnusedConnImmediateFailureUsed)                                     \
    X(TestTransportUnusedConnIdleTimoutBeforeUse)                                      \
    X(TestTransportTLSNextProtoConnImmediateFailureUnused)                             \
    X(TestTransportRequestsStallAtServerLimit)                                         \
    X(TestTransportMaxFrameReadSize_64000)                                             \
    X(TestTransportMaxFrameReadSize_1024)                                              \
    X(TestTransportCloseAfterLostPing)                                                 \
    X(TestTransportSendPingWithReset)                                                  \
    X(TestTransportNoPingAfterResetWithFrames)                                         \
    X(TestPadHeaders)                                                                  \
    X(TestTransportChecksRequestHeaderListSize)                                        \
    X(TestClientConnCloseAtHeaders)                                                    \
    X(TestClientConnCloseAtBody)                                                       \
    X(TestTransportFlowControl)                                                        \
    X(TestTransportBodyReadError_Immediately)                                          \
    X(TestTransportBodyReadError_Some)                                                 \
    X(TestTransport1xxLimits_default)                                                  \
    X(TestTransport1xxLimits_MaxResponseHeaderBytes)                                   \
    X(TestTransport1xxLimits_limit_by_client_trace)                                    \
    X(TestTransport1xxLimits_limit_disabled_by_client_trace)                           \
    X(TestTransportResponseHeaderTimeout_NoBody)                                       \
    X(TestTransportResponseHeaderTimeout_Body)                                         \
    X(TestTransportDoNotHangOnZeroMaxFrameSize)                                        \
    X(TestTransportSendNoMoreThanOnePingWithReset)                                     \
    X(TestTransportConnBecomesUnresponsive)                                            \
    X(TestIdleConnTimeout_NoExpiry)                                                    \
    X(TestIdleConnTimeout_H2TransportTimeoutExpires)                                   \
    X(TestIdleConnTimeout_H1TransportTimeoutExpires)                                   \
    X(TestTransportReqBodyAfterResponse_200)                                           \
    X(TestTransportReqBodyAfterResponse_403)                                           \
    X(TestTransportRequestPathPseudo)                                                  \
    X(TestRoundTripDoesntConsumeRequestBodyEarly)                                      \
    X(TestTransportRoundtripCloseOnWriteError)                                         \
    X(TestTransportBlockingRequestWrite_headers)                                       \
    X(TestTransportBlockingRequestWrite_body)                                          \
    X(TestTransportBlockingRequestWrite_trailer)                                       \
    X(TestClientConnShutdown)                                                          \
    X(TestClientConnShutdownCancel)                                                    \
    X(TestClientConnReservations)                                                      \
    X(TestTransportTimeoutServerHangs)                                                 \
    X(TestTransportH2c)                                                                \
    X(TestTransport)                                                                   \
    X(TestTransportReusesConns_ReuseConn)                                              \
    X(TestTransportReusesConns_RequestClose)                                           \
    X(TestTransportReusesConns_ConnClose)                                              \
    X(TestTransportGetGotConnHooks_HTTP2Transport)                                     \
    X(TestTransportGetGotConnHooks_Client)                                             \
    X(TestTransportAbortClosesPipes)                                                   \
    X(TestTransportPath)                                                               \
    X(TestTransportBody)                                                               \
    X(TestTransportFullDuplex)                                                         \
    X(TestTransportConnectRequest)                                                     \
    X(TestTransportBodyReadErrorType)                                                  \
    X(TestTransportDisableKeepAlives)                                                  \
    X(TestTransportDisableKeepAlives_Concurrency)                                      \
    X(TestTransportDisableCompression)                                                 \
    X(TestTransportRejectsConnHeaders)                                                 \
    X(TestTransportRejectsContentLengthWithSign)                                       \
    X(TestTransportFailsOnInvalidHeadersAndTrailers)                                   \
    X(TestTransportBodyDoubleEndStream)                                                \
    X(TestTransportCancelDataResponseRace)                                             \
    X(TestTransportNoRaceOnRequestObjectAfterRequestComplete)                          \
    X(TestTransportPingWriteBlocks)                                                    \
    X(TestTransportRequestsLowServerLimit)                                             \
    X(TestTransportResponseAndResetWithoutConsumingBodyRace)                           \
    X(TestTransportBodyLargerThanSpecifiedContentLength_len3)                          \
    X(TestTransportBodyLargerThanSpecifiedContentLength_len2)                          \
    X(TestTransportBodyRewindRace)                                                     \
    X(TestTransportServerResetStreamAtHeaders)                                         \
    X(TestTransportExpectContinue) X(TestTransportFrameBufferReuse)                    \
        X(TestTransportCloseRequestBody)

TESTING_MAIN(TESTS)
