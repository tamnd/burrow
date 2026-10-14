/* net/rpc/jsonrpc, a JSON-RPC 1.0 codec for net/rpc.
 *
 * A request goes out as {"method": "Arith.Multiply", "params": [args], "id": n}
 * and comes back as {"id": n, "result": reply, "error": null}, or with the
 * error's text in "error" and a null result. The argument and reply types need
 * what encoding/json needs, which is a descriptor.
 *
 *     rpc_register(BURROW_ANY(TYPE_OF(Arith), &arith));
 *     jsonrpc_serve_conn(conn);
 *
 * and on the other end
 *
 *     RpcClient *c = jsonrpc_dial(a, BURROW_S("tcp"), BURROW_S("host:1234"), &err);
 *
 * after which the client is used the way any other RpcClient is.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/rpc/jsonrpc */

#ifndef BURROW_NET_RPC_JSONRPC_H
#define BURROW_NET_RPC_JSONRPC_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/net/rpc.h"
#include "burrow/own.h"

#ifdef __cplusplus
extern "C" {
#endif

/* jsonrpc.NewServerCodec. A codec reading requests from conn and writing the
 * responses to it, which closes conn when it is closed. Its free gives the
 * codec back but leaves conn alone. A codec with a NULL vtable when a says no.
 * a is used from the goroutines answering the calls, so it has to be one that
 * any goroutine can use, such as the heap. */
BURROW_OWNS(ret) RpcServerCodec jsonrpc_new_server_codec(Alloc *a,
                                                         IoReadWriteCloser conn);

/* jsonrpc.ServeConn. Serves conn on rpc_default_server with this codec until
 * the client hangs up, and closes conn at the end. Blocks, so it is usually run
 * on a goroutine. */
void jsonrpc_serve_conn(IoReadWriteCloser conn);

/* jsonrpc.NewClientCodec. The client side, which closes conn when it is closed.
 * Its free gives the codec back but leaves conn alone. A codec with a NULL
 * vtable when a says no. */
BURROW_OWNS(ret) RpcClientCodec jsonrpc_new_client_codec(Alloc *a,
                                                         IoReadWriteCloser conn);

/* jsonrpc.NewClient. An RpcClient talking JSON-RPC over conn, which it closes
 * when it is closed but does not free. NULL when a says no. */
BURROW_OWNS(ret) RpcClient *jsonrpc_new_client(Alloc *a, IoReadWriteCloser conn);

/* jsonrpc.Dial. Connects to the server at address and gives a client over the
 * connection, which the client owns. */
BURROW_OWNS(ret) RpcClient *jsonrpc_dial(Alloc *a, Str network, Str address,
                                         Error *err);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_RPC_JSONRPC_H */
