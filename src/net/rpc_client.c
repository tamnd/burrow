/* Derived from Go's src/net/rpc/client.go, the client side of net/rpc.
 *
 * A call is waited on by whoever made it and finished by the goroutine reading
 * the responses, which in Go share it through the collector. Here the call is
 * the caller's, made in the allocator the caller gave, and the reading side
 * lets go of it once it has sent it on done.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/rpc.h"

#include "rpc_internal.h"

#include "burrow/bufio.h"
#include "burrow/chan.h"
#include "burrow/core.h"
#include "burrow/encoding/gob.h"
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
#include "burrow/proc.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

struct RpcClient {
    Alloc *a;
    RpcClientCodec codec;

    SyncMutex req_mutex; /* protects following */
    RpcRequest request;

    SyncMutex mutex; /* protects following */
    uint64_t seq;
    Map *pending;  /* uint64_t to RpcCall * */
    bool closing;  /* user has called Close */
    bool shutdown; /* server has told us to stop */

    SyncWaitGroup input; /* the goroutine reading responses */
};

/* Call.done. */
static void rc_call_done(RpcCall *call) {
    void *v = call;
    /* We don't want to block here. It is the caller's responsibility to make
     * sure the channel has enough buffer space. See comment in Go. */
    (void)chan_try_send(call->done, &v);
}

/* An error for the call, in the call's allocator. */
static void rc_call_fail(RpcCall *call, Error err) {
    call->error = BURROW_OK(err) ? err : error_retain(call->ea, err);
}

static RpcCall *rc_take(RpcClient *c, uint64_t seq) {
    RpcCall *call = NULL;
    sync_mutex_lock(&c->mutex);
    void **v = (void **)map_get(c->pending, &seq);
    if (v != NULL) {
        call = (RpcCall *)*v;
        map_del(c->pending, &seq);
    }
    sync_mutex_unlock(&c->mutex);
    return call;
}

static void rc_send(RpcClient *c, RpcCall *call) {
    sync_mutex_lock(&c->req_mutex);

    /* Register this call. */
    sync_mutex_lock(&c->mutex);
    if (c->shutdown || c->closing) {
        sync_mutex_unlock(&c->mutex);
        call->error = rpc_err_shutdown;
        rc_call_done(call);
        sync_mutex_unlock(&c->req_mutex);
        return;
    }
    uint64_t seq = c->seq;
    c->seq++;
    void *v = call;
    if (!map_set(c->pending, &seq, &v)) {
        sync_mutex_unlock(&c->mutex);
        call->error = burrow_err_out_of_memory;
        rc_call_done(call);
        sync_mutex_unlock(&c->req_mutex);
        return;
    }
    sync_mutex_unlock(&c->mutex);

    /* Encode and send the request. */
    c->request.seq = seq;
    c->request.service_method = call->service_method;
    Error err = c->codec.vt->write_request(c->codec.data, &c->request, call->args);
    if (BURROW_FAILED(err)) {
        RpcCall *got = rc_take(c, seq);
        if (got != NULL) {
            rc_call_fail(got, err);
            rc_call_done(got);
        }
    }
    sync_mutex_unlock(&c->req_mutex);
}

static void rc_input(void *env) {
    RpcClient *c = (RpcClient *)env;
    Arena scratch;
    arena_init(&scratch, heap_allocator(), 0);
    ArenaMark em = error_mark();
    Error err = BURROW_NO_ERROR;
    while (BURROW_OK(err)) {
        arena_reset(&scratch);
        Alloc *sa = arena_allocator(&scratch);
        RpcResponse response;
        memset(&response, 0, sizeof response);
        err = c->codec.vt->read_response_header(c->codec.data, sa, &response);
        if (BURROW_FAILED(err))
            break;
        RpcCall *call = rc_take(c, response.seq);

        if (call == NULL) {
            /* We've got no pending call. That usually means that WriteRequest
             * partially failed, and call was already removed; response is a
             * server telling us about an error reading request body. We
             * should still attempt to read error body, but there's no one to
             * give it to. */
            err = c->codec.vt->read_response_body(c->codec.data, sa,
                                                  BURROW_ANY(NULL, NULL));
            if (BURROW_FAILED(err))
                err = fmt_errorf_v("reading error body: %s", error_text(err));
        } else if (response.error.len > 0) {
            /* We've got an error response. Give this to the request; any
             * subsequent requests will get the ReadResponseBody error if there
             * is one. */
            call->error = rpc_server_error_as_error(response.error, call->ea);
            err = c->codec.vt->read_response_body(c->codec.data, sa,
                                                  BURROW_ANY(NULL, NULL));
            if (BURROW_FAILED(err))
                err = fmt_errorf_v("reading error body: %s", error_text(err));
            rc_call_done(call);
        } else {
            err = c->codec.vt->read_response_body(c->codec.data, call->a, call->reply);
            if (BURROW_FAILED(err))
                rc_call_fail(call, fmt_errorf_v("reading body %s", error_text(err)));
            rc_call_done(call);
        }
    }

    /* Terminate pending calls. */
    sync_mutex_lock(&c->req_mutex);
    sync_mutex_lock(&c->mutex);
    c->shutdown = true;
    bool closing = c->closing;
    if (burrow__rpc_is_eof(err))
        err = closing ? rpc_err_shutdown : io_err_unexpected_eof;
    MapIter it = map_iter(c->pending);
    const void *k = NULL;
    void *v = NULL;
    while (map_next(&it, &k, &v)) {
        RpcCall *call = *(RpcCall **)v;
        rc_call_fail(call, err);
        rc_call_done(call);
    }
    map_clear(c->pending);
    sync_mutex_unlock(&c->mutex);
    sync_mutex_unlock(&c->req_mutex);
    error_release(em);
    arena_free(&scratch);
    sync_wait_group_done(&c->input);
}

RpcClient *rpc_new_client_with_codec(Alloc *a, RpcClientCodec codec) {
    RpcClient *c = BURROW_NEW(a, RpcClient);
    if (c != NULL) {
        c->a = a;
        c->codec = codec;
        c->pending = map_make(a, TYPE_UINT64, TYPE_UNSAFE_POINTER, 0);
    }
    if (c == NULL || c->pending == NULL) {
        if (c != NULL)
            mem_free(a, c, sizeof *c, _Alignof(RpcClient));
        (void)codec.vt->close(codec.data);
        if (codec.vt->free != NULL)
            codec.vt->free(codec.data);
        return NULL;
    }
    sync_wait_group_add(&c->input, 1);
    if (!go(BURROW_FN(Func, rc_input, c))) {
        sync_wait_group_done(&c->input);
        map_free(c->pending);
        mem_free(a, c, sizeof *c, _Alignof(RpcClient));
        (void)codec.vt->close(codec.data);
        if (codec.vt->free != NULL)
            codec.vt->free(codec.data);
        return NULL;
    }
    return c;
}

/* ------------------------------------------------------- the gob codec */

/* gobClientCodec, and the connection when the codec is the one that has to
 * give it back. */
typedef struct RcGobCodec {
    Alloc *a;
    IoReadWriteCloser rwc;
    GobDecoder *dec;
    GobEncoder *enc;
    BufioWriter *enc_buf;
    NetConn conn;
    bool owns_conn;
} RcGobCodec;

static Error rc_gob_write_request(void *self, const RpcRequest *r, Any body) {
    RcGobCodec *c = (RcGobCodec *)self;
    Error err =
        gob_encoder_encode(c->enc, BURROW_ANY(TYPE_RPC_REQUEST, (void *)(uintptr_t)r));
    if (BURROW_FAILED(err))
        return err;
    err = gob_encoder_encode(c->enc, body);
    if (BURROW_FAILED(err))
        return err;
    return bufio_writer_flush(c->enc_buf);
}

static Error rc_gob_read_response_header(void *self, Alloc *a, RpcResponse *r) {
    RcGobCodec *c = (RcGobCodec *)self;
    return gob_decoder_decode_in(c->dec, a, BURROW_ANY(TYPE_RPC_RESPONSE, r));
}

static Error rc_gob_read_response_body(void *self, Alloc *a, Any body) {
    RcGobCodec *c = (RcGobCodec *)self;
    return gob_decoder_decode_in(c->dec, a, body);
}

static Error rc_gob_close(void *self) {
    RcGobCodec *c = (RcGobCodec *)self;
    return c->rwc.vt->closer.close(c->rwc.data);
}

static void rc_gob_free(void *self) {
    RcGobCodec *c = (RcGobCodec *)self;
    gob_encoder_free(c->enc);
    gob_decoder_free(c->dec);
    bufio_writer_free(c->enc_buf);
    if (c->owns_conn)
        net_conn_free(c->conn);
    mem_free(c->a, c, sizeof *c, _Alignof(RcGobCodec));
}

static const RpcClientCodecVT rc_gob_vt = {
    NULL,
    rc_gob_write_request,
    rc_gob_read_response_header,
    rc_gob_read_response_body,
    rc_gob_close,
    rc_gob_free,
};

/* NewClient over rwc, or over conn when owns_conn says the codec is to free
 * it. */
static RpcClient *rc_new_gob_client(Alloc *a, IoReadWriteCloser rwc, NetConn conn,
                                    bool owns_conn) {
    RcGobCodec *c = BURROW_NEW(a, RcGobCodec);
    if (c == NULL) {
        if (owns_conn)
            net_conn_free(conn);
        else
            (void)rwc.vt->closer.close(rwc.data);
        return NULL;
    }
    c->a = a;
    c->conn = conn;
    c->owns_conn = owns_conn;
    c->rwc = owns_conn ? net_conn_as_io_read_write_closer(&c->conn) : rwc;
    c->enc_buf = bufio_new_writer(a, (IoWriter){&c->rwc.vt->writer, c->rwc.data});
    c->dec = gob_new_decoder(a, (IoReader){&c->rwc.vt->reader, c->rwc.data});
    if (c->enc_buf != NULL)
        c->enc = gob_new_encoder(a, bufio_writer_as_io_writer(c->enc_buf));
    if (c->enc_buf == NULL || c->dec == NULL || c->enc == NULL) {
        (void)rc_gob_close(c);
        rc_gob_free(c);
        return NULL;
    }
    return rpc_new_client_with_codec(a, (RpcClientCodec){&rc_gob_vt, c});
}

RpcClient *rpc_new_client(Alloc *a, IoReadWriteCloser conn) {
    NetConn none = {NULL, NULL};
    return rc_new_gob_client(a, conn, none, false);
}

RpcClient *rpc_dial(Alloc *a, Str network, Str address, Error *err) {
    Error e = BURROW_NO_ERROR;
    NetConn conn = net_dial(a, network, address, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    IoReadWriteCloser none = {NULL, NULL};
    RpcClient *c = rc_new_gob_client(a, none, conn, true);
    BURROW_OUT(err, c != NULL ? BURROW_NO_ERROR : burrow_err_out_of_memory);
    return c;
}

#define RC_CONNECTED "200 Connected to Go RPC"

RpcClient *rpc_dial_http(Alloc *a, Str network, Str address, Error *err) {
    return rpc_dial_http_path(a, network, address, RPC_DEFAULT_RPC_PATH, err);
}

RpcClient *rpc_dial_http_path(Alloc *a, Str network, Str address, Str path,
                              Error *err) {
    Error e = BURROW_NO_ERROR;
    NetConn conn = net_dial(a, network, address, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *sa = arena_allocator(&ar);
    IoWriter w = net_conn_as_io_writer(conn);
    (void)io_write_string(w, BURROW_S("CONNECT "), NULL);
    (void)io_write_string(w, path, NULL);
    (void)io_write_string(w, BURROW_S(" HTTP/1.0\n\n"), NULL);

    /* Require successful HTTP response before switching to RPC protocol. */
    HttpRequest req;
    memset(&req, 0, sizeof req);
    req.method = BURROW_S("CONNECT");
    BufioReader *br = bufio_new_reader(sa, net_conn_as_io_reader(conn));
    HttpResponse *resp = NULL;
    if (br == NULL)
        e = burrow_err_out_of_memory;
    else
        resp = http_read_response(sa, br, &req, &e);
    if (BURROW_OK(e) && resp != NULL && str_eq(resp->status, BURROW_S(RC_CONNECTED))) {
        http_response_free(resp);
        bufio_reader_free(br);
        arena_free(&ar);
        IoReadWriteCloser none = {NULL, NULL};
        RpcClient *c = rc_new_gob_client(a, none, conn, true);
        BURROW_OUT(err, c != NULL ? BURROW_NO_ERROR : burrow_err_out_of_memory);
        return c;
    }
    if (BURROW_OK(e) && resp == NULL)
        e = burrow_err_out_of_memory;
    else if (BURROW_OK(e))
        e = fmt_errorf_v("unexpected HTTP response: %s", resp->status);
    Error wrapped = error_retain(error_allocator(), e);
    if (resp != NULL)
        http_response_free(resp);
    bufio_reader_free(br);
    arena_free(&ar);
    net_conn_free(conn);

    Alloc *ea = error_allocator();
    NetOpError op;
    memset(&op, 0, sizeof op);
    op.op = BURROW_S("dial-http");
    op.net = fmt_sprintf_v(ea, "%s %s", network, address);
    op.err = wrapped;
    BURROW_OUT(err, net_op_error_as_error(&op, ea));
    return NULL;
}

/* ---------------------------------------------------------------- calls */

Error rpc_client_close(RpcClient *c) {
    sync_mutex_lock(&c->mutex);
    if (c->closing) {
        sync_mutex_unlock(&c->mutex);
        return rpc_err_shutdown;
    }
    c->closing = true;
    sync_mutex_unlock(&c->mutex);
    return c->codec.vt->close(c->codec.data);
}

void rpc_client_free(RpcClient *c) {
    if (c == NULL)
        return;
    sync_mutex_lock(&c->mutex);
    bool closing = c->closing;
    c->closing = true;
    sync_mutex_unlock(&c->mutex);
    if (!closing)
        (void)c->codec.vt->close(c->codec.data);
    sync_wait_group_wait(&c->input);
    if (c->codec.vt->free != NULL)
        c->codec.vt->free(c->codec.data);
    map_free(c->pending);
    mem_free(c->a, c, sizeof *c, _Alignof(RpcClient));
}

static void rc_call_init(RpcCall *call, Alloc *a, Str service_method, Any args,
                         Any reply, Chan *done) {
    memset(call, 0, sizeof *call);
    call->service_method = service_method;
    call->args = args;
    call->reply = reply;
    call->done = done;
    call->a = a;
    call->ea = a;
}

RpcCall *rpc_client_go(RpcClient *c, Alloc *a, Str service_method, Any args, Any reply,
                       Chan *done) {
    Chan *own = NULL;
    if (done == NULL) {
        own = chan_make(a, TYPE_UNSAFE_POINTER, 10); /* buffered. */
        if (own == NULL)
            return NULL;
        done = own;
    } else if (chan_cap(done) == 0) {
        /* If caller passes done != nil, it must arrange that done has enough
         * buffer for the number of simultaneous RPCs that will be using that
         * channel. If the channel is totally unbuffered, it's best not to run
         * at all. */
        log_panic_v("rpc: done channel is unbuffered");
    }
    RpcCall *call = BURROW_NEW(a, RpcCall);
    if (call == NULL) {
        chan_free(own);
        return NULL;
    }
    rc_call_init(call, a, service_method, args, reply, done);
    call->own_done = own;
    rc_send(c, call);
    return call;
}

void rpc_call_free(RpcCall *call) {
    if (call == NULL)
        return;
    chan_free(call->own_done);
    mem_free(call->a, call, sizeof *call, _Alignof(RpcCall));
}

Error rpc_client_call(RpcClient *c, Alloc *a, Str service_method, Any args, Any reply) {
    Chan *done = chan_make(heap_allocator(), TYPE_UNSAFE_POINTER, 1);
    if (done == NULL)
        return burrow_err_out_of_memory;
    /* The error comes back in the caller's error arena, so the copy the input
     * loop makes only has to last until then. */
    Arena ea;
    arena_init(&ea, heap_allocator(), 0);
    RpcCall call;
    rc_call_init(&call, a, service_method, args, reply, done);
    call.ea = arena_allocator(&ea);
    rc_send(c, &call);
    void *got = NULL;
    (void)chan_recv(done, &got);
    chan_free(done);
    Error err = BURROW_OK(call.error) ? call.error
                                      : error_retain(error_allocator(), call.error);
    arena_free(&ea);
    return err;
}
