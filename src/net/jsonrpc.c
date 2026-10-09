/* Derived from Go's src/net/rpc/jsonrpc/client.go and server.go, JSON-RPC 1.0
 * for net/rpc.
 *
 * Go keeps the request id of a call on the server side as a *json.RawMessage
 * in a map until the response goes out, and the method name of a call on the
 * client side the same way. Both maps here hold copies made in the codec's
 * allocator, which the codec's free gives back along with any a connection
 * left behind.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/rpc/jsonrpc.h"

#include "rpc_internal.h"

#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/encoding/json.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/rpc.h"
#include "burrow/sync.h"
#include "burrow/type.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------- descriptors */

/* *json.RawMessage. */
static const Type jr_raw_ptr_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_POINTER,
    (uint32_t)sizeof(void *),
    (uint16_t)_Alignof(void *),
    0,
    0,
    NULL,
    NULL,
    TYPE_JSON_RAW_MESSAGE,
    NULL,
    0,
    0,
    NULL,
};

/* [1]any, which is how the params go out and come in. */
static const Type jr_params_desc = {
    {NULL, 0},
    {NULL, 0},
    KIND_ARRAY,
    (uint32_t)sizeof(Any),
    (uint16_t)_Alignof(Any),
    0,
    0,
    NULL,
    NULL,
    TYPE_OF(Any),
    NULL,
    1,
    0,
    NULL,
};

/* serverRequest. */
typedef struct JrServerRequest {
    Str method;
    JsonRawMessage *params;
    JsonRawMessage *id;
} JrServerRequest;

static const Field jr_server_request_fields[] = {
    {BURROW_S_INIT("Method"), BURROW_S_INIT("json:\"method\""), TYPE_STRING,
     (uint32_t)offsetof(JrServerRequest, method)},
    {BURROW_S_INIT("Params"), BURROW_S_INIT("json:\"params\""), &jr_raw_ptr_desc,
     (uint32_t)offsetof(JrServerRequest, params)},
    {BURROW_S_INIT("Id"), BURROW_S_INIT("json:\"id\""), &jr_raw_ptr_desc,
     (uint32_t)offsetof(JrServerRequest, id)},
};

static const Type jr_server_request_desc = {
    BURROW_S_INIT("serverRequest"),
    BURROW_S_INIT("net/rpc/jsonrpc"),
    KIND_STRUCT,
    (uint32_t)sizeof(JrServerRequest),
    (uint16_t)_Alignof(JrServerRequest),
    (uint16_t)(sizeof jr_server_request_fields / sizeof jr_server_request_fields[0]),
    0,
    jr_server_request_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* serverResponse. */
typedef struct JrServerResponse {
    JsonRawMessage *id;
    Any result;
    Any error;
} JrServerResponse;

static const Field jr_server_response_fields[] = {
    {BURROW_S_INIT("Id"), BURROW_S_INIT("json:\"id\""), &jr_raw_ptr_desc,
     (uint32_t)offsetof(JrServerResponse, id)},
    {BURROW_S_INIT("Result"), BURROW_S_INIT("json:\"result\""), TYPE_OF(Any),
     (uint32_t)offsetof(JrServerResponse, result)},
    {BURROW_S_INIT("Error"), BURROW_S_INIT("json:\"error\""), TYPE_OF(Any),
     (uint32_t)offsetof(JrServerResponse, error)},
};

static const Type jr_server_response_desc = {
    BURROW_S_INIT("serverResponse"),
    BURROW_S_INIT("net/rpc/jsonrpc"),
    KIND_STRUCT,
    (uint32_t)sizeof(JrServerResponse),
    (uint16_t)_Alignof(JrServerResponse),
    (uint16_t)(sizeof jr_server_response_fields / sizeof jr_server_response_fields[0]),
    0,
    jr_server_response_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* clientRequest. */
typedef struct JrClientRequest {
    Str method;
    Any params[1];
    uint64_t id;
} JrClientRequest;

static const Field jr_client_request_fields[] = {
    {BURROW_S_INIT("Method"), BURROW_S_INIT("json:\"method\""), TYPE_STRING,
     (uint32_t)offsetof(JrClientRequest, method)},
    {BURROW_S_INIT("Params"), BURROW_S_INIT("json:\"params\""), &jr_params_desc,
     (uint32_t)offsetof(JrClientRequest, params)},
    {BURROW_S_INIT("Id"), BURROW_S_INIT("json:\"id\""), TYPE_UINT64,
     (uint32_t)offsetof(JrClientRequest, id)},
};

static const Type jr_client_request_desc = {
    BURROW_S_INIT("clientRequest"),
    BURROW_S_INIT("net/rpc/jsonrpc"),
    KIND_STRUCT,
    (uint32_t)sizeof(JrClientRequest),
    (uint16_t)_Alignof(JrClientRequest),
    (uint16_t)(sizeof jr_client_request_fields / sizeof jr_client_request_fields[0]),
    0,
    jr_client_request_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* clientResponse. */
typedef struct JrClientResponse {
    uint64_t id;
    JsonRawMessage *result;
    Any error;
} JrClientResponse;

static const Field jr_client_response_fields[] = {
    {BURROW_S_INIT("Id"), BURROW_S_INIT("json:\"id\""), TYPE_UINT64,
     (uint32_t)offsetof(JrClientResponse, id)},
    {BURROW_S_INIT("Result"), BURROW_S_INIT("json:\"result\""), &jr_raw_ptr_desc,
     (uint32_t)offsetof(JrClientResponse, result)},
    {BURROW_S_INIT("Error"), BURROW_S_INIT("json:\"error\""), TYPE_OF(Any),
     (uint32_t)offsetof(JrClientResponse, error)},
};

static const Type jr_client_response_desc = {
    BURROW_S_INIT("clientResponse"),
    BURROW_S_INIT("net/rpc/jsonrpc"),
    KIND_STRUCT,
    (uint32_t)sizeof(JrClientResponse),
    (uint16_t)_Alignof(JrClientResponse),
    (uint16_t)(sizeof jr_client_response_fields / sizeof jr_client_response_fields[0]),
    0,
    jr_client_response_fields,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

BURROW_SENTINEL_ERROR(jsonrpc_err_missing_params,
                      "jsonrpc: request body missing params");

/* ---------------------------------------------------------------- server */

typedef struct JrServerCodec {
    Alloc *a;
    JsonDecoder *dec; /* for reading JSON values */
    JsonEncoder *enc; /* for writing JSON values */
    IoReadWriteCloser c;

    /* temporary work space */
    JrServerRequest req;

    /* JSON-RPC clients can use arbitrary json values as request IDs. Package
     * rpc expects uint64 request IDs. We assign uint64 sequence numbers to
     * incoming requests but save the original request ID in the pending map.
     * When rpc responds, we use the sequence number in the response to find
     * the original request ID. */
    SyncMutex mutex; /* protects seq, pending */
    uint64_t seq;
    Map *pending; /* uint64_t to a copy of the id, nil for none */
} JrServerCodec;

static void jr_free_bytes(Alloc *a, Slice b) {
    if (b.p != NULL)
        mem_free(a, b.p, (size_t)b.len, 1);
}

static Error jr_server_read_request_header(void *self, Alloc *a, RpcRequest *r) {
    JrServerCodec *c = (JrServerCodec *)self;
    memset(&c->req, 0, sizeof c->req);
    Error err =
        json_decoder_decode_in(c->dec, a, BURROW_ANY(&jr_server_request_desc, &c->req));
    if (BURROW_FAILED(err))
        return err;
    r->service_method = c->req.method;

    /* JSON request id can be any JSON value; RPC package expects uint64.
     * Translate to internal uint64 and save JSON on the side. */
    JsonRawMessage id;
    memset(&id, 0, sizeof id);
    if (c->req.id != NULL) {
        id.len = c->req.id->len;
        id.cap = id.len;
        id.elem = c->req.id->elem;
        id.p = mem_alloc(c->a, id.len > 0 ? (size_t)id.len : 1, 1);
        if (id.p == NULL)
            return burrow_err_out_of_memory;
        if (id.len > 0)
            memcpy(id.p, c->req.id->p, (size_t)id.len);
    }
    sync_mutex_lock(&c->mutex);
    c->seq++;
    uint64_t seq = c->seq;
    bool ok = map_set(c->pending, &seq, &id);
    sync_mutex_unlock(&c->mutex);
    if (!ok) {
        jr_free_bytes(c->a, id);
        return burrow_err_out_of_memory;
    }
    c->req.id = NULL;
    r->seq = seq;
    return BURROW_NO_ERROR;
}

static Error jr_server_read_request_body(void *self, Alloc *a, Any x) {
    JrServerCodec *c = (JrServerCodec *)self;
    if (x.t == NULL)
        return BURROW_NO_ERROR;
    if (c->req.params == NULL)
        return jsonrpc_err_missing_params;
    /* JSON params is array value. RPC params is struct. Unmarshal into array
     * containing struct for now. Should think about making RPC more general.
     *
     * Go hands over a pointer here, and json fills in what an interface holding
     * a pointer points at, so x goes in as one. */
    Type ptr;
    memset(&ptr, 0, sizeof ptr);
    ptr.kind = KIND_POINTER;
    ptr.size = (uint32_t)sizeof(void *);
    ptr.align = (uint16_t)_Alignof(void *);
    ptr.elem = x.t;
    void *target = x.data;
    Any params[1];
    params[0].t = &ptr;
    params[0].data = &target;
    return json_unmarshal(a, *c->req.params, BURROW_ANY(&jr_params_desc, params));
}

#define JR_NULL "null"

static Error jr_server_write_response(void *self, const RpcResponse *r, Any x) {
    JrServerCodec *c = (JrServerCodec *)self;
    uint64_t seq = r->seq;
    sync_mutex_lock(&c->mutex);
    JsonRawMessage *v = (JsonRawMessage *)map_get(c->pending, &seq);
    if (v == NULL) {
        sync_mutex_unlock(&c->mutex);
        return errors_new(error_allocator(),
                          BURROW_S("invalid sequence number in response"));
    }
    JsonRawMessage b = *v;
    map_del(c->pending, &seq);
    sync_mutex_unlock(&c->mutex);

    JsonRawMessage null = BURROW_B(JR_NULL);
    JrServerResponse resp;
    memset(&resp, 0, sizeof resp);
    /* Invalid request so no id. Use JSON null. */
    resp.id = b.p == NULL ? &null : &b;
    if (r->error.len == 0) {
        resp.result = x;
    } else {
        resp.error.t = TYPE_STRING;
        resp.error.data = (void *)(uintptr_t)&r->error;
    }
    Error err =
        json_encoder_encode(c->enc, BURROW_ANY(&jr_server_response_desc, &resp));
    jr_free_bytes(c->a, b);
    return err;
}

static Error jr_server_close(void *self) {
    JrServerCodec *c = (JrServerCodec *)self;
    return c->c.vt->closer.close(c->c.data);
}

static void jr_server_free(void *self) {
    JrServerCodec *c = (JrServerCodec *)self;
    if (c->pending != NULL) {
        MapIter it = map_iter(c->pending);
        const void *k = NULL;
        void *v = NULL;
        while (map_next(&it, &k, &v))
            jr_free_bytes(c->a, *(JsonRawMessage *)v);
        map_free(c->pending);
    }
    json_decoder_free(c->dec);
    json_encoder_free(c->enc);
    mem_free(c->a, c, sizeof *c, _Alignof(JrServerCodec));
}

static const RpcServerCodecVT jr_server_vt = {
    NULL,
    jr_server_read_request_header,
    jr_server_read_request_body,
    jr_server_write_response,
    jr_server_close,
    jr_server_free,
};

RpcServerCodec jsonrpc_new_server_codec(Alloc *a, IoReadWriteCloser conn) {
    RpcServerCodec none = {NULL, NULL};
    JrServerCodec *c = BURROW_NEW(a, JrServerCodec);
    if (c == NULL)
        return none;
    c->a = a;
    c->c = conn;
    c->dec = json_new_decoder(a, (IoReader){&conn.vt->reader, conn.data});
    c->enc = json_new_encoder(a, (IoWriter){&conn.vt->writer, conn.data});
    c->pending = map_make(a, TYPE_UINT64, TYPE_JSON_RAW_MESSAGE, 0);
    if (c->dec == NULL || c->enc == NULL || c->pending == NULL) {
        jr_server_free(c);
        return none;
    }
    return (RpcServerCodec){&jr_server_vt, c};
}

void jsonrpc_serve_conn(IoReadWriteCloser conn) {
    RpcServerCodec codec = jsonrpc_new_server_codec(heap_allocator(), conn);
    if (codec.vt == NULL) {
        (void)conn.vt->closer.close(conn.data);
        return;
    }
    rpc_serve_codec(codec);
    jr_server_free(codec.data);
}

/* ---------------------------------------------------------------- client */

typedef struct JrClientCodec {
    Alloc *a;
    JsonDecoder *dec; /* for reading JSON values */
    JsonEncoder *enc; /* for writing JSON values */
    IoReadWriteCloser c;

    /* temporary work space */
    JrClientRequest req;
    JrClientResponse resp;

    /* JSON-RPC responses include the request id but not the request method.
     * Package rpc expects both. We save the request method in pending when
     * sending a request and then look it up by request ID when filling out
     * the rpc Response. */
    SyncMutex mutex; /* protects pending */
    Map *pending;    /* uint64_t to a copy of the method */

    /* The connection, when jsonrpc_dial made it and the codec frees it. */
    NetConn conn;
    bool owns_conn;
} JrClientCodec;

static void jr_free_str(Alloc *a, Str s) {
    if (s.p != NULL)
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

static Error jr_client_write_request(void *self, const RpcRequest *r, Any param) {
    JrClientCodec *c = (JrClientCodec *)self;
    Str method = str_clone(c->a, r->service_method);
    if (method.p == NULL && r->service_method.len > 0)
        return burrow_err_out_of_memory;
    uint64_t seq = r->seq;
    sync_mutex_lock(&c->mutex);
    Str *old = (Str *)map_get(c->pending, &seq);
    if (old != NULL)
        jr_free_str(c->a, *old);
    bool ok = map_set(c->pending, &seq, &method);
    sync_mutex_unlock(&c->mutex);
    if (!ok) {
        jr_free_str(c->a, method);
        return burrow_err_out_of_memory;
    }
    c->req.method = r->service_method;
    c->req.params[0] = param;
    c->req.id = r->seq;
    return json_encoder_encode(c->enc, BURROW_ANY(&jr_client_request_desc, &c->req));
}

#define JR_UNSPECIFIED "unspecified error"

static Error jr_client_read_response_header(void *self, Alloc *a, RpcResponse *r) {
    JrClientCodec *c = (JrClientCodec *)self;
    memset(&c->resp, 0, sizeof c->resp);
    Error err = json_decoder_decode_in(c->dec, a,
                                       BURROW_ANY(&jr_client_response_desc, &c->resp));
    if (BURROW_FAILED(err))
        return err;

    uint64_t id = c->resp.id;
    Str method = BURROW_STR_EMPTY;
    sync_mutex_lock(&c->mutex);
    Str *v = (Str *)map_get(c->pending, &id);
    if (v != NULL) {
        method = *v;
        map_del(c->pending, &id);
    }
    sync_mutex_unlock(&c->mutex);
    r->service_method = str_clone(a, method);
    jr_free_str(c->a, method);

    r->error = BURROW_STR_EMPTY;
    r->seq = id;
    if (c->resp.error.t != NULL || c->resp.result == NULL) {
        Any e = c->resp.error;
        if (e.t != TYPE_STRING)
            return fmt_errorf_v("invalid error %v", e);
        Str x = *(const Str *)e.data;
        if (x.len == 0)
            x = BURROW_S(JR_UNSPECIFIED);
        r->error = x;
    }
    return BURROW_NO_ERROR;
}

static Error jr_client_read_response_body(void *self, Alloc *a, Any x) {
    JrClientCodec *c = (JrClientCodec *)self;
    if (x.t == NULL)
        return BURROW_NO_ERROR;
    return json_unmarshal(a, *c->resp.result, x);
}

static Error jr_client_close(void *self) {
    JrClientCodec *c = (JrClientCodec *)self;
    return c->c.vt->closer.close(c->c.data);
}

static void jr_client_free(void *self) {
    JrClientCodec *c = (JrClientCodec *)self;
    if (c->pending != NULL) {
        MapIter it = map_iter(c->pending);
        const void *k = NULL;
        void *v = NULL;
        while (map_next(&it, &k, &v))
            jr_free_str(c->a, *(Str *)v);
        map_free(c->pending);
    }
    json_decoder_free(c->dec);
    json_encoder_free(c->enc);
    if (c->owns_conn)
        net_conn_free(c->conn);
    mem_free(c->a, c, sizeof *c, _Alignof(JrClientCodec));
}

static const RpcClientCodecVT jr_client_vt = {
    NULL,
    jr_client_write_request,
    jr_client_read_response_header,
    jr_client_read_response_body,
    jr_client_close,
    jr_client_free,
};

/* NewClientCodec over conn, or over nc when owns_conn says the codec is to
 * free it. */
static JrClientCodec *jr_new_client_codec(Alloc *a, IoReadWriteCloser conn, NetConn nc,
                                          bool owns_conn) {
    JrClientCodec *c = BURROW_NEW(a, JrClientCodec);
    if (c == NULL)
        return NULL;
    c->a = a;
    c->conn = nc;
    c->owns_conn = owns_conn;
    c->c = owns_conn ? net_conn_as_io_read_write_closer(&c->conn) : conn;
    c->dec = json_new_decoder(a, (IoReader){&c->c.vt->reader, c->c.data});
    c->enc = json_new_encoder(a, (IoWriter){&c->c.vt->writer, c->c.data});
    c->pending = map_make(a, TYPE_UINT64, TYPE_STRING, 0);
    if (c->dec == NULL || c->enc == NULL || c->pending == NULL) {
        c->owns_conn = false;
        jr_client_free(c);
        return NULL;
    }
    return c;
}

RpcClientCodec jsonrpc_new_client_codec(Alloc *a, IoReadWriteCloser conn) {
    NetConn none = {NULL, NULL};
    JrClientCodec *c = jr_new_client_codec(a, conn, none, false);
    if (c == NULL)
        return (RpcClientCodec){NULL, NULL};
    return (RpcClientCodec){&jr_client_vt, c};
}

RpcClient *jsonrpc_new_client(Alloc *a, IoReadWriteCloser conn) {
    RpcClientCodec codec = jsonrpc_new_client_codec(a, conn);
    if (codec.vt == NULL) {
        (void)conn.vt->closer.close(conn.data);
        return NULL;
    }
    return rpc_new_client_with_codec(a, codec);
}

RpcClient *jsonrpc_dial(Alloc *a, Str network, Str address, Error *err) {
    Error e = BURROW_NO_ERROR;
    NetConn conn = net_dial(a, network, address, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    IoReadWriteCloser none = {NULL, NULL};
    JrClientCodec *c = jr_new_client_codec(a, none, conn, true);
    if (c == NULL) {
        net_conn_free(conn);
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    RpcClient *client =
        rpc_new_client_with_codec(a, (RpcClientCodec){&jr_client_vt, c});
    BURROW_OUT(err, client != NULL ? BURROW_NO_ERROR : burrow_err_out_of_memory);
    return client;
}
