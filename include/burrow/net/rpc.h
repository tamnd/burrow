/* net/rpc, calls to the methods of an object across a network or another
 * connection.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* A server registers an object, which makes it a service named after the
 * object's type, and the methods of that type that have the right shape can
 * then be called from the other end. Go's net/rpc finds the methods with
 * reflection, and so does this one, through the methods a type's descriptor
 * lists. A method is called with the arguments the caller sent and a pointer
 * to the reply to fill in, and returns an Error:
 *
 *     #define ARGS_FIELDS(F, T) F(T, Int, A, "") F(T, Int, B, "")
 *     BURROW_STRUCT(Args, ARGS_FIELDS);
 *     BURROW_PTR_TYPE(IntPtr, Int);
 *
 *     static Error arith_multiply(Arith *t, Args args, IntPtr reply) {
 *         *reply = args.A * args.B;
 *         return BURROW_NO_ERROR;
 *     }
 *
 *     #define ARITH_MULTIPLY_SIG(IN, OUT) IN(0, Args) IN(1, IntPtr) OUT(Error)
 *     #define ARITH_METHODS(M, T) M(T, Multiply, arith_multiply, ARITH_MULTIPLY_SIG)
 *     BURROW_STRUCT_DEFINE_METHODS(Arith, ARITH_FIELDS, ARITH_METHODS);
 *
 * Only the methods that look like that are made available, which in Go's words
 * is a method that is exported, with two arguments of exported or builtin
 * types, the second a pointer, and an error result. A method whose first
 * argument is an EncodingAllocArg takes three, and gets an allocator for what
 * the reply holds, which is given back once the reply has been sent:
 *
 *     #define ARITH_STRING_SIG(IN, OUT)                                         \
 *         IN(0, EncodingAllocArg) IN(1, Args) IN(2, StrPtr) OUT(Error)
 *
 * A method that returns an error has it sent to the caller as its text, and
 * no reply goes with it. The caller sees an RpcServerError holding the text.
 *
 * The server and the client both use encoding/gob on the wire unless they are
 * given a codec of their own, so the argument and reply types need what gob
 * needs, which is a descriptor. net/rpc/jsonrpc has a JSON codec.
 *
 *     rpc_register(BURROW_ANY(TYPE_OF(Arith), &arith));
 *     NetListener l = net_listen(a, BURROW_S("tcp"), BURROW_S(":1234"), &err);
 *     go(BURROW_FN(Func, accept_on, &l));   which calls rpc_accept(l)
 *
 * and a client:
 *
 *     RpcClient *c = rpc_dial(a, BURROW_S("tcp"), BURROW_S("host:1234"), &err);
 *     Args args = {7, 8};
 *     Int reply;
 *     err = rpc_client_call(c, a, BURROW_S("Arith.Multiply"),
 *                           BURROW_ANY(TYPE_OF(Args), &args),
 *                           BURROW_ANY(TYPE_INT, &reply));
 *
 * A receiver in C is always a pointer, so Go's difference between the methods
 * of T and those of *T is not there, and registering either finds them all.
 *
 * Go's net/rpc package is frozen and is not accepting new features, and the
 * same goes for this one. */

/* burrow:package net/rpc */

#ifndef BURROW_NET_RPC_H
#define BURROW_NET_RPC_H

#include "burrow/chan.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/own.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* rpc.DefaultRPCPath and rpc.DefaultDebugPath, where rpc_handle_http puts
 * the server and its debugging page. */
#define RPC_DEFAULT_RPC_PATH BURROW_S("/_goRPC_")
#define RPC_DEFAULT_DEBUG_PATH BURROW_S("/debug/rpc")

/* rpc.Request, the header written before every call. The server and the
 * codecs use it, and it is here as an aid to debugging, such as when looking
 * at the traffic. service_method is "Service.Method" and seq is the number the
 * client chose for the call. */
typedef struct RpcRequest {
    Str service_method;
    uint64_t seq;
} RpcRequest;

/* rpc.Response, the header written before every reply. service_method and seq
 * echo the request's, and error is the text of the error, if any. */
typedef struct RpcResponse {
    Str service_method;
    uint64_t seq;
    Str error;
} RpcResponse;

/* The descriptors gob sends the two headers with, under Go's names, Request
 * and Response, and Go's field names, so a Go client or server can talk to
 * this one. */
extern const Type *const TYPE_RPC_REQUEST;
extern const Type *const TYPE_RPC_RESPONSE;

/* rpc.ServerError, an error the method returned on the server, as its text. */
typedef Str RpcServerError;

extern const Type *const TYPE_RPC_SERVER_ERROR;

/* ServerError.Error, which is e. */
BURROW_BORROWS(ret, e) Str rpc_server_error_error(RpcServerError e);

/* e as an Error, made in a. errors_as with TYPE_RPC_SERVER_ERROR gives the
 * RpcServerError back. */
BURROW_OWNS(ret) Error rpc_server_error_as_error(RpcServerError e, Alloc *a);

/* rpc.ErrShutdown, from a call on a client that has been closed or whose
 * connection has gone. */
extern const Error rpc_err_shutdown;

/* ------------------------------------------------------------------- codecs */

/* rpc.ServerCodec, what reads requests and writes responses for the server
 * side of a connection.
 *
 * The server calls read_request_header and read_request_body in pairs, and
 * write_response to send a reply back, from more than one goroutine but never
 * two at once. Whatever the header and the body hold is made in a, which the
 * server gives back once the request is over. A body with no type means read
 * it and throw it away. close is called when the server is done with the
 * connection, and may be called more than once. free, which may be NULL, gives
 * the codec back, for whoever owns it. */
typedef struct RpcServerCodecVT {
    const Type *self_type;
    Error (*read_request_header)(void *self, Alloc *a, RpcRequest *r);
    Error (*read_request_body)(void *self, Alloc *a, Any body);
    Error (*write_response)(void *self, const RpcResponse *r, Any body);
    Error (*close)(void *self);
    void (*free)(void *self);
} RpcServerCodecVT;

typedef struct RpcServerCodec {
    const RpcServerCodecVT *vt;
    void *data;
} RpcServerCodec;

/* rpc.ClientCodec, what writes requests and reads responses for the client
 * side of a connection.
 *
 * The client calls write_request from the goroutine making the call, and
 * read_response_header and read_response_body in pairs from a goroutine of its
 * own. What a response holds is made in a. A body with no type means read it
 * and throw it away. close is called once, by rpc_client_close, and free,
 * which may be NULL, by rpc_client_free. */
typedef struct RpcClientCodecVT {
    const Type *self_type;
    Error (*write_request)(void *self, const RpcRequest *r, Any body);
    Error (*read_response_header)(void *self, Alloc *a, RpcResponse *r);
    Error (*read_response_body)(void *self, Alloc *a, Any body);
    Error (*close)(void *self);
    void (*free)(void *self);
} RpcClientCodecVT;

typedef struct RpcClientCodec {
    const RpcClientCodecVT *vt;
    void *data;
} RpcClientCodec;

/* ------------------------------------------------------------------- server */

/* rpc.Server. Safe to use from any number of goroutines. */
typedef struct RpcServer RpcServer;

/* rpc.NewServer. NULL when a says no. Give it back with rpc_server_free. */
BURROW_OWNS(ret) RpcServer *rpc_new_server(Alloc *a);

/* Gives back the server and what it holds, once nothing is using it. NULL is
 * fine, and so is rpc_default_server, which keeps what it has. */
void rpc_server_free(RpcServer *s);

/* rpc.DefaultServer, the server the functions without a server use. */
extern RpcServer *const rpc_default_server;

/* Server.Register. Makes the methods of rcvr.t that have the right shape
 * callable as "Type.Method", with rcvr.data as the receiver, which has to
 * outlive the server. rcvr.t may be a pointer type, which stands for what it
 * points at, with rcvr.data pointing at the pointer.
 *
 * An error when the type has no name, the name is not exported, it has no
 * methods of the right shape, or a service of the same name is registered
 * already. Each of those but the last is logged through log as well, as Go
 * does. */
BURROW_BORROWS(ret) Error rpc_server_register(RpcServer *s, Any rcvr);

/* Server.RegisterName. rpc_server_register under name rather than the type's
 * name, which does not have to be exported. */
BURROW_BORROWS(ret) Error rpc_server_register_name(RpcServer *s, Str name, Any rcvr);

/* Server.ServeConn. Serves the connection with gob until the client hangs up,
 * running each call on a goroutine of its own, and closes it at the end.
 * Blocks, so it is usually run on a goroutine. */
void rpc_server_serve_conn(RpcServer *s, IoReadWriteCloser conn);

/* Server.ServeCodec. rpc_server_serve_conn with a codec of the caller's,
 * which it closes at the end but does not free. */
void rpc_server_serve_codec(RpcServer *s, RpcServerCodec codec);

/* Server.ServeRequest. Serves one request, on this goroutine, and does not
 * close the codec. The error is from reading the request. */
BURROW_BORROWS(ret) Error rpc_server_serve_request(RpcServer *s, RpcServerCodec codec);

/* Server.Accept. Accepts connections on lis and serves each on a goroutine of
 * its own, freeing it with net_conn_free when the client is done, until
 * accepting fails, which is logged. Blocks, so it is usually run on a
 * goroutine. */
void rpc_server_accept(RpcServer *s, NetListener lis);

/* Server.ServeHTTP, as a handler that takes a CONNECT request over and serves
 * RPC on its connection. Any other method gets a 405. The handler points at s.
 * The connection is the one an HttpServer accepted from a TCP listener. */
BURROW_BORROWS(ret, s) HttpHandler rpc_server_as_handler(RpcServer *s);

/* Server.ServeHTTP itself. */
void rpc_server_serve_http(RpcServer *s, HttpResponseWriter w, HttpRequest *r);

/* Server.HandleHTTP. Registers the server at rpc_path and a page listing its
 * services and how often each method has been called at debug_path, both on
 * http_default_serve_mux. An HttpServer still has to be serving. */
void rpc_server_handle_http(RpcServer *s, Str rpc_path, Str debug_path);

/* The same functions on rpc_default_server: rpc.Register, rpc.RegisterName,
 * rpc.ServeConn, rpc.ServeCodec, rpc.ServeRequest, rpc.Accept, and
 * rpc.HandleHTTP, which uses RPC_DEFAULT_RPC_PATH and
 * RPC_DEFAULT_DEBUG_PATH. */
BURROW_BORROWS(ret) Error rpc_register(Any rcvr);
BURROW_BORROWS(ret) Error rpc_register_name(Str name, Any rcvr);
void rpc_serve_conn(IoReadWriteCloser conn);
void rpc_serve_codec(RpcServerCodec codec);
BURROW_BORROWS(ret) Error rpc_serve_request(RpcServerCodec codec);
void rpc_accept(NetListener lis);
void rpc_handle_http(void);

/* ------------------------------------------------------------------- client */

/* rpc.Call, one call in flight or done.
 *
 * service_method, args and reply are what rpc_client_go was given, and the
 * call reads args and fills in reply, so both have to outlive it. error is set
 * once the call is over, and then the call is sent on done, as an RpcCall *
 * in a channel of TYPE_UNSAFE_POINTER. The call, its error and what reply gets
 * are made in a, which is the allocator rpc_client_go was given. */
typedef struct RpcCall {
    Str service_method;
    Any args;
    Any reply;
    Error error;
    Chan *done;

    /* The call's own. ea is where error goes, which is a except for the call
     * rpc_client_call makes for itself. */
    Alloc *a;
    Alloc *ea;
    Chan *own_done;
} RpcCall;

/* Gives back a call rpc_client_go made, once it is done, and the channel it
 * made if it was not given one. Not the error, which is in a with the reply.
 * NULL is fine. */
void rpc_call_free(RpcCall *call);

/* rpc.Client. One client can be used by any number of goroutines at once. */
typedef struct RpcClient RpcClient;

/* rpc.NewClient. A client talking gob over conn, which it closes when it is
 * closed but does not free. NULL when a says no. */
BURROW_OWNS(ret) RpcClient *rpc_new_client(Alloc *a, IoReadWriteCloser conn);

/* rpc.NewClientWithCodec. A client using codec, which it owns from here on,
 * and frees with the codec's free if it has one. */
BURROW_OWNS(ret) RpcClient *rpc_new_client_with_codec(Alloc *a, RpcClientCodec codec);

/* rpc.Dial. Connects to the server at address and gives a client over the
 * connection, which the client owns. */
BURROW_OWNS(ret) RpcClient *rpc_dial(Alloc *a, Str network, Str address, Error *err);

/* rpc.DialHTTP and rpc.DialHTTPPath. Connects to an HTTP server and asks it
 * for the RPC server at path, or RPC_DEFAULT_RPC_PATH, with a CONNECT. A
 * failure is a NetOpError with "dial-http" as its op. */
BURROW_OWNS(ret) RpcClient *rpc_dial_http(Alloc *a, Str network, Str address,
                                          Error *err);
BURROW_OWNS(ret) RpcClient *rpc_dial_http_path(Alloc *a, Str network, Str address,
                                               Str path, Error *err);

/* Client.Close. Closes the codec, after which every call fails with
 * rpc_err_shutdown, as does a second Close. */
BURROW_BORROWS(ret) Error rpc_client_close(RpcClient *c);

/* Closes the client if it is open, waits for the goroutine reading its
 * responses to stop, and gives it back with its codec. NULL is fine. */
void rpc_client_free(RpcClient *c);

/* Client.Go. Starts a call and returns it without waiting. done is where the
 * call is sent when it is over, and NULL makes one with room for ten. A done
 * with no room panics, as in Go. The call is made in a, and so is what the
 * reply gets, so a has to be one the client's goroutine can use, such as the
 * heap or an arena only this call uses. Give the call back with
 * rpc_call_free. */
BURROW_OWNS(ret) RpcCall *rpc_client_go(RpcClient *c, Alloc *a, Str service_method,
                                        Any args, Any reply, Chan *done);

/* Client.Call. Makes the call and waits for it. What the reply gets is made in
 * a, and the error is in the calling goroutine's error arena. */
BURROW_BORROWS(ret) Error rpc_client_call(RpcClient *c, Alloc *a, Str service_method,
                                          Any args, Any reply);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_RPC_H */
