/* Derived from Go's src/net/rpc/server.go and debug.go, the server side of
 * net/rpc.
 *
 * Go finds a service's methods with reflect and calls them with
 * reflect.Value.Call. Here the methods are the ones the receiver's type
 * descriptor lists, and the call goes through each method's thunk. What Go
 * leaves to its collector, a request's header, its argument and its reply, is
 * made in an arena of the request's own, which goes once the reply is out.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/rpc.h"

#include "rpc_internal.h"

#include "burrow/bufio.h"
#include "burrow/core.h"
#include "burrow/encoding.h"
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
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/type.h"
#include "burrow/unicode.h"
#include "burrow/utf8.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define RS_COUNT(x) ((uint16_t)(sizeof(x) / sizeof((x)[0])))

/* ------------------------------------------------------------ the headers */

static const Field rs_request_fields[] = {
    {BURROW_S_INIT("ServiceMethod"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(RpcRequest, service_method)},
    {BURROW_S_INIT("Seq"), {NULL, 0}, TYPE_UINT64, (uint32_t)offsetof(RpcRequest, seq)},
};

static const Field rs_response_fields[] = {
    {BURROW_S_INIT("ServiceMethod"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(RpcResponse, service_method)},
    {BURROW_S_INIT("Seq"),
     {NULL, 0},
     TYPE_UINT64,
     (uint32_t)offsetof(RpcResponse, seq)},
    {BURROW_S_INIT("Error"),
     {NULL, 0},
     TYPE_STRING,
     (uint32_t)offsetof(RpcResponse, error)},
};

static const Type rs_request_desc = {
    BURROW_S_INIT("Request"),
    BURROW_S_INIT("net/rpc"),
    KIND_STRUCT,
    (uint32_t)sizeof(RpcRequest),
    (uint16_t)_Alignof(RpcRequest),
    RS_COUNT(rs_request_fields),
    0,
    rs_request_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x72706371U, /* "rpcq" */
    NULL,
};

static const Type rs_response_desc = {
    BURROW_S_INIT("Response"),
    BURROW_S_INIT("net/rpc"),
    KIND_STRUCT,
    (uint32_t)sizeof(RpcResponse),
    (uint16_t)_Alignof(RpcResponse),
    RS_COUNT(rs_response_fields),
    0,
    rs_response_fields,
    NULL,
    NULL,
    NULL,
    0,
    0x72706373U, /* "rpcs" */
    NULL,
};

const Type *const TYPE_RPC_REQUEST = &rs_request_desc;
const Type *const TYPE_RPC_RESPONSE = &rs_response_desc;

/* ------------------------------------------------------------- ServerError */

static const Type rs_server_error_desc = {
    BURROW_S_INIT("ServerError"),
    BURROW_S_INIT("net/rpc"),
    KIND_STRING,
    (uint32_t)sizeof(Str),
    (uint16_t)_Alignof(Str),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x72706365U, /* "rpce" */
    NULL,
};

const Type *const TYPE_RPC_SERVER_ERROR = &rs_server_error_desc;

/* A ServerError is a string, and the box is the string, so errors_as gives a
 * Str * and the message is the same Str. */
static Str rs_server_error_message(const void *self) {
    return *(const Str *)self;
}

static bool rs_server_error_is(const void *self, Error target);
static Error rs_server_error_clone(const void *self, Alloc *a);

static const ErrorVT rs_server_error_vt = {
    &rs_server_error_desc,
    rs_server_error_message,
    NULL,
    NULL,
    rs_server_error_is,
    NULL,
    rs_server_error_clone,
};

/* Go compares the two values with ==, which for a string is the text. */
static bool rs_server_error_is(const void *self, Error target) {
    if (target.vt != &rs_server_error_vt || target.data == NULL)
        return false;
    return str_eq(*(const Str *)self, *(const Str *)target.data);
}

Str rpc_server_error_error(RpcServerError e) {
    return e;
}

Error rpc_server_error_as_error(RpcServerError e, Alloc *a) {
    Str *b = (Str *)mem_alloc_nozero(a, sizeof(Str) + (size_t)e.len, _Alignof(Str));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *text = (Byte *)(b + 1);
    if (e.len > 0)
        memcpy(text, e.p, (size_t)e.len);
    *b = str_from_bytes(text, e.len);
    return (Error){&rs_server_error_vt, b};
}

static Error rs_server_error_clone(const void *self, Alloc *a) {
    return rpc_server_error_as_error(*(const Str *)self, a);
}

BURROW_SENTINEL_ERROR(rpc_err_shutdown, "connection is shut down");

/* ---------------------------------------------------------------- shared */

bool burrow__rpc_is_eof(Error err) {
    return err.vt == io_eof.vt && err.data == io_eof.data;
}

static bool rs_is_unexpected_eof(Error err) {
    return err.vt == io_err_unexpected_eof.vt && err.data == io_err_unexpected_eof.data;
}

/* token.IsExported. */
static bool rs_is_exported(Str name) {
    if (name.len == 0)
        return false;
    Int size = 0;
    return unicode_is_upper(utf8_decode_rune_in_string(name, &size));
}

/* isExportedOrBuiltinType. A type with no package path is a builtin as far as
 * Go is concerned, which takes in the types BURROW_STRUCT declares. */
static bool rs_is_exported_or_builtin(const Type *t) {
    for (int i = 0; t != NULL && t->kind == KIND_POINTER && i < 100; i++)
        t = t->elem;
    if (t == NULL)
        return false;
    return rs_is_exported(t->name) || t->pkg_path.len == 0;
}

/* reflect.Type.String, which is what fmt's %T writes, except for an interface
 * type, where %T looks at what the interface holds and String does not. */
static Str rs_type_string(Alloc *a, const Type *t) {
    if (t != NULL && t->kind == KIND_INTERFACE) {
        if (t->name.len == 0)
            return BURROW_S("interface {}");
        Str pkg = t->pkg_path;
        Int slash = strings_last_index_byte(pkg, '/');
        if (slash >= 0)
            pkg = str_from_bytes(pkg.p + slash + 1, pkg.len - slash - 1);
        if (pkg.len == 0)
            return t->name;
        return fmt_sprintf_v(a, "%s.%s", pkg, t->name);
    }
    if (t == NULL)
        return BURROW_S("<nil>");
    size_t size = t->size > 0 ? t->size : 1;
    void *zero = mem_alloc(a, size, t->align > 0 ? t->align : 1);
    if (zero == NULL)
        return BURROW_STR_EMPTY;
    Str s = fmt_sprintf_v(a, "%T", BURROW_ANY(t, zero));
    mem_free(a, zero, size, t->align > 0 ? t->align : 1);
    return s;
}

/* ---------------------------------------------------------------- server */

/* methodType. */
typedef struct RsMethod {
    Str name;
    const Method *method;
    const Type *arg_type;
    const Type *reply_type;
    bool takes_alloc;
    SyncMutex mu; /* protects num_calls */
    uint64_t num_calls;
} RsMethod;

/* service. */
typedef struct RsService {
    Str name;
    void *rcvr;
    const Type *typ;
    RsMethod *method;
    Int nmethod;
} RsService;

struct RpcServer {
    Alloc *a;
    SyncRWMutex mu;
    Map *services; /* Str to RsService *, made on the first Register */
};

static RpcServer rs_default_server;

RpcServer *const rpc_default_server = &rs_default_server;

static Alloc *rs_alloc(const RpcServer *s) {
    return s->a != NULL ? s->a : heap_allocator();
}

RpcServer *rpc_new_server(Alloc *a) {
    RpcServer *s = BURROW_NEW(a, RpcServer);
    if (s == NULL)
        return NULL;
    s->a = a;
    return s;
}

static void rs_service_free(Alloc *a, RsService *svc) {
    if (svc->method != NULL)
        mem_free(a, svc->method, sizeof(RsMethod) * (size_t)svc->nmethod,
                 _Alignof(RsMethod));
    if (svc->name.len > 0)
        mem_free(a, (void *)(uintptr_t)svc->name.p, (size_t)svc->name.len, 1);
    mem_free(a, svc, sizeof *svc, _Alignof(RsService));
}

void rpc_server_free(RpcServer *s) {
    if (s == NULL || s == &rs_default_server)
        return;
    Alloc *a = rs_alloc(s);
    if (s->services != NULL) {
        MapIter it = map_iter(s->services);
        const void *k = NULL;
        void *v = NULL;
        while (map_next(&it, &k, &v))
            rs_service_free(a, *(RsService **)v);
        map_free(s->services);
    }
    mem_free(a, s, sizeof *s, _Alignof(RpcServer));
}

/* Whether m has one of the two shapes, and if it does, what it takes. */
static bool rs_suitable(const Method *m, RsMethod *out) {
    if (!rs_is_exported(m->name) || m->thunk == NULL)
        return false;
    const Type *ft = m->ftype;
    Int nin = type_num_in(ft);
    Int off = 0;
    if (nin == 3 && type_in(ft, 0) == TYPE_OF(EncodingAllocArg))
        off = 1;
    else if (nin != 2)
        return false;
    const Type *arg = type_in(ft, off);
    if (arg == NULL || !rs_is_exported_or_builtin(arg))
        return false;
    const Type *reply = type_in(ft, off + 1);
    if (reply == NULL || reply->kind != KIND_POINTER || reply->elem == NULL)
        return false;
    if (!rs_is_exported_or_builtin(reply))
        return false;
    if (type_num_out(ft) != 1 || type_out(ft, 0) != TYPE_ERROR)
        return false;
    out->name = m->name;
    out->method = m;
    out->arg_type = arg;
    out->reply_type = reply;
    out->takes_alloc = off == 1;
    return true;
}

/* An error with the text, which is logged first, as Go logs it. */
static Error rs_register_error(Str text) {
    log_print_v(text);
    return errors_new(error_allocator(), text);
}

static Error rs_register(RpcServer *s, Any rcvr, Str name, bool use_name) {
    const Type *typ = rcvr.t;
    const Type *base = typ;
    void *recv = rcvr.data;
    if (typ != NULL && typ->kind == KIND_POINTER) {
        base = typ->elem;
        recv = rcvr.data != NULL ? *(void **)rcvr.data : NULL;
    }
    Alloc *ea = error_allocator();
    Str sname = name;
    if (!use_name)
        sname = base != NULL ? base->name : BURROW_STR_EMPTY;
    if (sname.len == 0)
        return rs_register_error(fmt_sprintf_v(
            ea, "rpc.Register: no service name for type %s", rs_type_string(ea, typ)));
    if (!use_name && !rs_is_exported(sname))
        return rs_register_error(
            fmt_sprintf_v(ea, "rpc.Register: type %s is not exported", sname));
    if (base == NULL)
        return rs_register_error(fmt_sprintf_v(
            ea, "rpc.Register: type %s has no exported methods of suitable type",
            rs_type_string(ea, typ)));

    Alloc *a = rs_alloc(s);
    Int n = 0;
    for (uint16_t i = 0; i < base->nmethod; i++) {
        RsMethod m;
        memset(&m, 0, sizeof m);
        if (rs_suitable(&base->methods[i], &m))
            n++;
    }
    if (n == 0)
        return rs_register_error(fmt_sprintf_v(
            ea, "rpc.Register: type %s has no exported methods of suitable type",
            sname));

    RsService *svc = BURROW_NEW(a, RsService);
    if (svc == NULL)
        return burrow_err_out_of_memory;
    svc->rcvr = recv;
    svc->typ = typ;
    svc->name = str_clone(a, sname);
    svc->method =
        (RsMethod *)mem_alloc(a, sizeof(RsMethod) * (size_t)n, _Alignof(RsMethod));
    if (svc->method == NULL || svc->name.len != sname.len) {
        rs_service_free(a, svc);
        return burrow_err_out_of_memory;
    }
    for (uint16_t i = 0; i < base->nmethod; i++) {
        if (rs_suitable(&base->methods[i], &svc->method[svc->nmethod]))
            svc->nmethod++;
    }

    Error err = BURROW_NO_ERROR;
    sync_rw_mutex_lock(&s->mu);
    if (s->services == NULL)
        s->services = map_make(a, TYPE_STRING, TYPE_UNSAFE_POINTER, 0);
    if (s->services == NULL) {
        err = burrow_err_out_of_memory;
    } else if (map_get(s->services, &svc->name) != NULL) {
        err = fmt_errorf_v("rpc: service already defined: %s", sname);
    } else {
        void *v = svc;
        if (!map_set(s->services, &svc->name, &v))
            err = burrow_err_out_of_memory;
    }
    sync_rw_mutex_unlock(&s->mu);
    if (BURROW_FAILED(err))
        rs_service_free(a, svc);
    return err;
}

Error rpc_server_register(RpcServer *s, Any rcvr) {
    return rs_register(s, rcvr, BURROW_STR_EMPTY, false);
}

Error rpc_server_register_name(RpcServer *s, Str name, Any rcvr) {
    return rs_register(s, rcvr, name, true);
}

/* ------------------------------------------------------------- a request */

/* What Go keeps in readRequest's results and passes to service.call: the
 * request, where it is going, the argument and the reply, all in the arena,
 * and how to answer. */
typedef struct RsCall {
    Arena ar;
    RpcRequest req;
    RsService *svc;
    RsMethod *m;
    void *arg;     /* what the method's argument is read into */
    void *arg_ptr; /* arg, when the method takes a pointer */
    void *reply;   /* what the reply points at */
    Alloc *ralloc; /* the allocator a method of the three argument kind gets */
    SyncMutex *sending;
    SyncWaitGroup *wg;
    RpcServerCodec codec;
} RsCall;

static RsCall *rs_call_new(void) {
    RsCall *c = BURROW_NEW(heap_allocator(), RsCall);
    if (c != NULL)
        arena_init(&c->ar, heap_allocator(), 0);
    return c;
}

static void rs_call_free(RsCall *c) {
    arena_free(&c->ar);
    mem_free(heap_allocator(), c, sizeof *c, _Alignof(RsCall));
}

/* struct{}{}, the reply that goes with an error. */
static const Type rs_invalid_request_desc = {
    BURROW_S_INIT(""),
    BURROW_S_INIT(""),
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
    0x72706369U, /* "rpci" */
    NULL,
};

static const Byte rs_invalid_request = 0;

static Any rs_invalid(void) {
    return BURROW_ANY(&rs_invalid_request_desc, (void *)(uintptr_t)&rs_invalid_request);
}

static void rs_send_response(SyncMutex *sending, const RpcRequest *req, Any reply,
                             RpcServerCodec codec, Str errmsg) {
    RpcResponse resp;
    memset(&resp, 0, sizeof resp);
    resp.service_method = req->service_method;
    if (errmsg.len > 0) {
        resp.error = errmsg;
        reply = rs_invalid();
    }
    resp.seq = req->seq;
    sync_mutex_lock(sending);
    (void)codec.vt->write_response(codec.data, &resp, reply);
    sync_mutex_unlock(sending);
}

static RsMethod *rs_find_method(RsService *svc, Str name) {
    for (Int i = 0; i < svc->nmethod; i++) {
        if (str_eq(svc->method[i].name, name))
            return &svc->method[i];
    }
    return NULL;
}

/* readRequestHeader. *keep is whether the connection is still worth reading
 * from, and *out the request when there is one to answer, which there is
 * whenever the header was read. */
static Error rs_read_request_header(RpcServer *s, RpcServerCodec codec, RsCall **out,
                                    bool *keep) {
    *out = NULL;
    *keep = false;
    RsCall *c = rs_call_new();
    if (c == NULL)
        return burrow_err_out_of_memory;
    Error err =
        codec.vt->read_request_header(codec.data, arena_allocator(&c->ar), &c->req);
    if (BURROW_FAILED(err)) {
        rs_call_free(c);
        if (burrow__rpc_is_eof(err) || rs_is_unexpected_eof(err))
            return err;
        return fmt_errorf_v("rpc: server cannot decode request: %s", error_text(err));
    }
    *out = c;
    *keep = true;

    Str sm = c->req.service_method;
    Int dot = strings_last_index_byte(sm, '.');
    if (dot < 0)
        return fmt_errorf_v("rpc: service/method request ill-formed: %s", sm);
    Str service_name = str_from_bytes(sm.p, dot);
    Str method_name = str_from_bytes(sm.p + dot + 1, sm.len - dot - 1);

    sync_rw_mutex_r_lock(&s->mu);
    void **v =
        s->services != NULL ? (void **)map_get(s->services, &service_name) : NULL;
    c->svc = v != NULL ? (RsService *)*v : NULL;
    sync_rw_mutex_r_unlock(&s->mu);
    if (c->svc == NULL)
        return fmt_errorf_v("rpc: can't find service %s", sm);
    c->m = rs_find_method(c->svc, method_name);
    if (c->m == NULL)
        return fmt_errorf_v("rpc: can't find method %s", sm);
    return BURROW_NO_ERROR;
}

static void *rs_new_value(Alloc *a, const Type *t) {
    return mem_alloc(a, t->size > 0 ? t->size : 1, t->align > 0 ? t->align : 1);
}

/* readRequest. */
static Error rs_read_request(RpcServer *s, RpcServerCodec codec, RsCall **out,
                             bool *keep) {
    Error err = rs_read_request_header(s, codec, out, keep);
    RsCall *c = *out;
    if (c == NULL)
        return BURROW_FAILED(err) ? err : burrow_err_out_of_memory;
    if (BURROW_FAILED(err)) {
        if (!*keep)
            return err;
        /* discard body */
        (void)codec.vt->read_request_body(codec.data, arena_allocator(&c->ar),
                                          BURROW_ANY(NULL, NULL));
        return err;
    }

    Alloc *a = arena_allocator(&c->ar);
    const Type *at = c->m->arg_type;
    const Type *target = at->kind == KIND_POINTER ? at->elem : at;
    c->arg = rs_new_value(a, target);
    if (c->arg == NULL)
        return burrow_err_out_of_memory;
    err = codec.vt->read_request_body(codec.data, a, BURROW_ANY(target, c->arg));
    if (BURROW_FAILED(err))
        return err;
    c->arg_ptr = c->arg;

    const Type *rt = c->m->reply_type->elem;
    c->reply = rs_new_value(a, rt);
    if (c->reply == NULL)
        return burrow_err_out_of_memory;
    switch ((int)rt->kind) {
    case KIND_MAP:
        *(Map **)c->reply = map_make(a, rt->key, rt->elem, 0);
        break;
    case KIND_SLICE:
        *(Slice *)c->reply = slice_make(a, rt->elem, 0, 0);
        break;
    default:
        break;
    }
    return BURROW_NO_ERROR;
}

/* service.call. */
static void rs_call(RsCall *c) {
    RsMethod *m = c->m;
    sync_mutex_lock(&m->mu);
    m->num_calls++;
    sync_mutex_unlock(&m->mu);

    ArenaMark em = error_mark();
    c->ralloc = arena_allocator(&c->ar);
    void *args[3];
    int n = 0;
    if (m->takes_alloc)
        args[n++] = &c->ralloc;
    args[n++] = m->arg_type->kind == KIND_POINTER ? (void *)&c->arg_ptr : c->arg;
    args[n++] = &c->reply;
    Error err = BURROW_NO_ERROR;
    void *rets[1] = {&err};
    (void)method_call(m->method, c->svc->rcvr, args, rets);
    Str errmsg = BURROW_FAILED(err) ? error_text(err) : BURROW_STR_EMPTY;
    rs_send_response(c->sending, &c->req, BURROW_ANY(m->reply_type->elem, c->reply),
                     c->codec, errmsg);
    error_release(em);
    SyncWaitGroup *wg = c->wg;
    rs_call_free(c);
    if (wg != NULL)
        sync_wait_group_done(wg);
}

static void rs_call_go(void *env) {
    rs_call((RsCall *)env);
}

void rpc_server_serve_codec(RpcServer *s, RpcServerCodec codec) {
    SyncMutex sending;
    SyncWaitGroup wg;
    memset(&sending, 0, sizeof sending);
    memset(&wg, 0, sizeof wg);
    for (;;) {
        ArenaMark em = error_mark();
        RsCall *c = NULL;
        bool keep = false;
        Error err = rs_read_request(s, codec, &c, &keep);
        if (BURROW_FAILED(err) || c == NULL) {
            if (!keep) {
                error_release(em);
                break;
            }
            if (c != NULL) {
                rs_send_response(&sending, &c->req, rs_invalid(), codec,
                                 error_text(err));
                rs_call_free(c);
            }
            error_release(em);
            continue;
        }
        error_release(em);
        c->sending = &sending;
        c->wg = &wg;
        c->codec = codec;
        sync_wait_group_add(&wg, 1);
        if (!go(BURROW_FN(Func, rs_call_go, c)))
            rs_call(c);
    }
    sync_wait_group_wait(&wg);
    (void)codec.vt->close(codec.data);
}

Error rpc_server_serve_request(RpcServer *s, RpcServerCodec codec) {
    SyncMutex sending;
    memset(&sending, 0, sizeof sending);
    RsCall *c = NULL;
    bool keep = false;
    Error err = rs_read_request(s, codec, &c, &keep);
    if (BURROW_FAILED(err)) {
        if (!keep)
            return err;
        if (c != NULL) {
            rs_send_response(&sending, &c->req, rs_invalid(), codec, error_text(err));
            rs_call_free(c);
        }
        return err;
    }
    c->sending = &sending;
    c->codec = codec;
    rs_call(c);
    return BURROW_NO_ERROR;
}

/* ------------------------------------------------------- the gob codec */

/* gobServerCodec. */
typedef struct RsGobCodec {
    IoReadWriteCloser rwc;
    GobDecoder *dec;
    GobEncoder *enc;
    BufioWriter *enc_buf;
    bool closed;
} RsGobCodec;

static Error rs_gob_read_request_header(void *self, Alloc *a, RpcRequest *r) {
    RsGobCodec *c = (RsGobCodec *)self;
    return gob_decoder_decode_in(c->dec, a, BURROW_ANY(TYPE_RPC_REQUEST, r));
}

static Error rs_gob_read_request_body(void *self, Alloc *a, Any body) {
    RsGobCodec *c = (RsGobCodec *)self;
    return gob_decoder_decode_in(c->dec, a, body);
}

static Error rs_gob_close(void *self) {
    RsGobCodec *c = (RsGobCodec *)self;
    if (c->closed)
        return BURROW_NO_ERROR;
    c->closed = true;
    return c->rwc.vt->closer.close(c->rwc.data);
}

static Error rs_gob_write_response(void *self, const RpcResponse *r, Any body) {
    RsGobCodec *c = (RsGobCodec *)self;
    Error err =
        gob_encoder_encode(c->enc, BURROW_ANY(TYPE_RPC_RESPONSE, (void *)(uintptr_t)r));
    if (BURROW_FAILED(err)) {
        if (BURROW_OK(bufio_writer_flush(c->enc_buf))) {
            log_println_v("rpc: gob error encoding response:", err);
            (void)rs_gob_close(c);
        }
        return err;
    }
    err = gob_encoder_encode(c->enc, body);
    if (BURROW_FAILED(err)) {
        if (BURROW_OK(bufio_writer_flush(c->enc_buf))) {
            log_println_v("rpc: gob error encoding body:", err);
            (void)rs_gob_close(c);
        }
        return err;
    }
    return bufio_writer_flush(c->enc_buf);
}

static const RpcServerCodecVT rs_gob_vt = {
    NULL,
    rs_gob_read_request_header,
    rs_gob_read_request_body,
    rs_gob_write_response,
    rs_gob_close,
    NULL,
};

void rpc_server_serve_conn(RpcServer *s, IoReadWriteCloser conn) {
    Alloc *a = heap_allocator();
    RsGobCodec c;
    memset(&c, 0, sizeof c);
    c.rwc = conn;
    c.enc_buf = bufio_new_writer(a, (IoWriter){&conn.vt->writer, conn.data});
    c.dec = gob_new_decoder(a, (IoReader){&conn.vt->reader, conn.data});
    c.enc = c.enc_buf != NULL ? gob_new_encoder(a, bufio_writer_as_io_writer(c.enc_buf))
                              : NULL;
    if (c.enc_buf != NULL && c.dec != NULL && c.enc != NULL)
        rpc_server_serve_codec(s, (RpcServerCodec){&rs_gob_vt, &c});
    else
        (void)rs_gob_close(&c);
    gob_encoder_free(c.enc);
    gob_decoder_free(c.dec);
    bufio_writer_free(c.enc_buf);
}

/* ---------------------------------------------------------------- Accept */

typedef struct RsAccepted {
    RpcServer *s;
    NetConn conn;
} RsAccepted;

static void rs_serve_accepted(void *env) {
    RsAccepted *ac = (RsAccepted *)env;
    rpc_server_serve_conn(ac->s, net_conn_as_io_read_write_closer(&ac->conn));
    net_conn_free(ac->conn);
    mem_free(heap_allocator(), ac, sizeof *ac, _Alignof(RsAccepted));
}

void rpc_server_accept(RpcServer *s, NetListener lis) {
    for (;;) {
        Error err = BURROW_NO_ERROR;
        NetConn conn = lis.vt->accept(lis.data, &err);
        if (BURROW_FAILED(err)) {
            log_print_v("rpc.Serve: accept:", error_text(err));
            return;
        }
        RsAccepted *ac = BURROW_NEW(heap_allocator(), RsAccepted);
        if (ac == NULL) {
            net_conn_free(conn);
            continue;
        }
        ac->s = s;
        ac->conn = conn;
        if (!go(BURROW_FN(Func, rs_serve_accepted, ac)))
            rs_serve_accepted(ac);
    }
}

/* --------------------------------------------------------- the defaults */

Error rpc_register(Any rcvr) {
    return rpc_server_register(&rs_default_server, rcvr);
}

Error rpc_register_name(Str name, Any rcvr) {
    return rpc_server_register_name(&rs_default_server, name, rcvr);
}

void rpc_serve_conn(IoReadWriteCloser conn) {
    rpc_server_serve_conn(&rs_default_server, conn);
}

void rpc_serve_codec(RpcServerCodec codec) {
    rpc_server_serve_codec(&rs_default_server, codec);
}

Error rpc_serve_request(RpcServerCodec codec) {
    return rpc_server_serve_request(&rs_default_server, codec);
}

void rpc_accept(NetListener lis) {
    rpc_server_accept(&rs_default_server, lis);
}

/* ------------------------------------------------------------------ HTTP */

#define RS_CONNECTED "200 Connected to Go RPC"

void rpc_server_serve_http(RpcServer *s, HttpResponseWriter w, HttpRequest *r) {
    if (!str_eq(r->method, BURROW_S("CONNECT"))) {
        (void)http_header_set(http_response_writer_header(w), BURROW_S("Content-Type"),
                              BURROW_S("text/plain; charset=utf-8"));
        http_response_writer_write_header(w, HTTP_STATUS_METHOD_NOT_ALLOWED);
        (void)io_write_string(http_response_writer_as_io_writer(w),
                              BURROW_S("405 must CONNECT\n"), NULL);
        return;
    }
    HttpResponseController rc = http_new_response_controller(w);
    BufioReadWriter buf = {NULL, NULL};
    Error err = BURROW_NO_ERROR;
    NetConn conn = http_response_controller_hijack(&rc, &buf, &err);
    if (BURROW_FAILED(err)) {
        log_print_v("rpc hijacking ", r->remote_addr, ": ", error_text(err));
        return;
    }
    (void)io_write_string(net_conn_as_io_writer(conn),
                          BURROW_S("HTTP/1.0 " RS_CONNECTED "\n\n"), NULL);
    rpc_server_serve_conn(s, net_conn_as_io_read_write_closer(&conn));
    bufio_reader_free(buf.reader);
    bufio_writer_free(buf.writer);
    net_conn_free(conn);
}

static void rs_handler_serve(void *self, HttpResponseWriter w, HttpRequest *r) {
    rpc_server_serve_http((RpcServer *)self, w, r);
}

static const HttpHandlerVT rs_handler_vt = {NULL, rs_handler_serve};

HttpHandler rpc_server_as_handler(RpcServer *s) {
    return (HttpHandler){&rs_handler_vt, s};
}

/* debug.go. The template is written out by hand, with what html/template
 * would escape escaped the same way. */

static void rs_write_escaped(IoWriter w, Str s) {
    Int start = 0;
    for (Int i = 0; i < s.len; i++) {
        Str rep;
        switch (s.p[i]) {
        case 0:
            rep = BURROW_S("\xef\xbf\xbd");
            break;
        case '"':
            rep = BURROW_S("&#34;");
            break;
        case '&':
            rep = BURROW_S("&amp;");
            break;
        case '\'':
            rep = BURROW_S("&#39;");
            break;
        case '+':
            rep = BURROW_S("&#43;");
            break;
        case '<':
            rep = BURROW_S("&lt;");
            break;
        case '>':
            rep = BURROW_S("&gt;");
            break;
        default:
            continue;
        }
        if (i > start)
            (void)io_write_string(w, str_from_bytes(s.p + start, i - start), NULL);
        (void)io_write_string(w, rep, NULL);
        start = i + 1;
    }
    if (s.len > start)
        (void)io_write_string(w, str_from_bytes(s.p + start, s.len - start), NULL);
}

static void rs_sort_services(RsService **v, Int n) {
    for (Int i = 1; i < n; i++) {
        RsService *x = v[i];
        Int j = i;
        while (j > 0 && strings_compare(v[j - 1]->name, x->name) > 0) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
}

static void rs_sort_methods(RsMethod **v, Int n) {
    for (Int i = 1; i < n; i++) {
        RsMethod *x = v[i];
        Int j = i;
        while (j > 0 && strings_compare(v[j - 1]->name, x->name) > 0) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
}

static void rs_debug_write_service(IoWriter w, Alloc *a, RsService *svc) {
    (void)io_write_string(w, BURROW_S("\n\t<hr>\n\tService "), NULL);
    rs_write_escaped(w, svc->name);
    (void)io_write_string(w,
                          BURROW_S("\n\t<hr>\n\t\t<table>\n\t\t<th align=center>Method"
                                   "</th><th align=center>Calls</th>\n\t\t"),
                          NULL);
    RsMethod **ms = (RsMethod **)mem_alloc(
        a, sizeof(RsMethod *) * (size_t)(svc->nmethod + 1), _Alignof(RsMethod *));
    if (ms != NULL) {
        for (Int i = 0; i < svc->nmethod; i++)
            ms[i] = &svc->method[i];
        rs_sort_methods(ms, svc->nmethod);
        for (Int i = 0; i < svc->nmethod; i++) {
            RsMethod *m = ms[i];
            sync_mutex_lock(&m->mu);
            uint64_t calls = m->num_calls;
            sync_mutex_unlock(&m->mu);
            char num[24];
            int nn = snprintf(num, sizeof num, "%llu", (unsigned long long)calls);
            (void)io_write_string(
                w, BURROW_S("\n\t\t\t<tr>\n\t\t\t<td align=left font=fixed>"), NULL);
            rs_write_escaped(w, m->name);
            (void)io_write_string(w, BURROW_S("("), NULL);
            rs_write_escaped(w, rs_type_string(a, m->arg_type));
            (void)io_write_string(w, BURROW_S(", "), NULL);
            rs_write_escaped(w, rs_type_string(a, m->reply_type));
            (void)io_write_string(w, BURROW_S(") error</td>\n\t\t\t<td align=center>"),
                                  NULL);
            (void)io_write_string(w, str_from_bytes(num, nn > 0 ? nn : 0), NULL);
            (void)io_write_string(w, BURROW_S("</td>\n\t\t\t</tr>\n\t\t"), NULL);
        }
    }
    (void)io_write_string(w, BURROW_S("\n\t\t</table>\n\t"), NULL);
}

static void rs_debug_serve(void *self, HttpResponseWriter rw, HttpRequest *r) {
    (void)r;
    RpcServer *s = (RpcServer *)self;
    IoWriter w = http_response_writer_as_io_writer(rw);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    /* Services are never removed, so the pointers stay good once the lock is
     * given back. */
    sync_rw_mutex_r_lock(&s->mu);
    Int n = s->services != NULL ? map_len(s->services) : 0;
    RsService **svcs = (RsService **)mem_alloc(a, sizeof(RsService *) * (size_t)(n + 1),
                                               _Alignof(RsService *));
    Int got = 0;
    if (svcs != NULL && n > 0) {
        MapIter it = map_iter(s->services);
        const void *k = NULL;
        void *v = NULL;
        while (got < n && map_next(&it, &k, &v))
            svcs[got++] = *(RsService **)v;
    }
    sync_rw_mutex_r_unlock(&s->mu);
    rs_sort_services(svcs, got);

    (void)io_write_string(
        w, BURROW_S("<html>\n\t<body>\n\t<title>Services</title>\n\t"), NULL);
    for (Int i = 0; i < got; i++)
        rs_debug_write_service(w, a, svcs[i]);
    (void)io_write_string(w, BURROW_S("\n\t</body>\n\t</html>"), NULL);
    arena_free(&ar);
}

static const HttpHandlerVT rs_debug_vt = {NULL, rs_debug_serve};

void rpc_server_handle_http(RpcServer *s, Str rpc_path, Str debug_path) {
    http_handle(rpc_path, rpc_server_as_handler(s));
    http_handle(debug_path, ((HttpHandler){&rs_debug_vt, s}));
}

void rpc_handle_http(void) {
    rpc_server_handle_http(&rs_default_server, RPC_DEFAULT_RPC_PATH,
                           RPC_DEFAULT_DEBUG_PATH);
}
