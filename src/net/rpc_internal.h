/* What rpc.c, rpc_client.c and jsonrpc.c share.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_NET_RPC_INTERNAL_H
#define BURROW_SRC_NET_RPC_INTERNAL_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/net.h"

/* Go's err == io.EOF, which compares the error itself rather than what it
 * wraps. */
bool burrow__rpc_is_eof(Error err);

#endif
