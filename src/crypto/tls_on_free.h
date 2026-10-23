/* Not in Go: a function a Conn runs when tls_conn_free has given it back. A
 * caller that made the connection under a Conn, or the config it was made
 * with, uses it to free them once the Conn is gone, wherever it is freed.
 * net/http's Transport does, for the connections it wraps in TLS.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SRC_CRYPTO_TLS_ON_FREE_H
#define BURROW_SRC_CRYPTO_TLS_ON_FREE_H

#include "burrow/crypto/tls.h"
#include "burrow/func.h"

/* Sets the function tls_conn_free runs, last, after c is freed. */
void burrow__tls_conn_set_on_free(TlsConn *c, Func f);

/* The function set by burrow__tls_conn_set_on_free, or a zero Func. */
Func burrow__tls_conn_on_free(const TlsConn *c);

#endif /* BURROW_SRC_CRYPTO_TLS_ON_FREE_H */
