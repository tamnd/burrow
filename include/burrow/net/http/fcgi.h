/* net/http/fcgi, the FastCGI protocol, from the side of the program the web
 * server talks to.
 *
 * A FastCGI program stays running and the web server sends it requests over a
 * connection, which can carry more than one request at a time. fcgi_serve
 * accepts those connections and serves each request with a handler, on a
 * goroutine of its own:
 *
 *     NetListener l = net_listen(a, BURROW_S("tcp"), BURROW_S("127.0.0.1:9000"), &err);
 *     err = fcgi_serve(l, handler);
 *
 * A web server that starts the program itself hands it the listening socket as
 * its standard input, and a listener with no vt means that one.
 *
 * Only the responder role is supported, as in Go. See
 * https://fast-cgi.github.io/ for an unofficial mirror of the original
 * documentation.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/http/fcgi */

#ifndef BURROW_NET_HTTP_FCGI_H
#define BURROW_NET_HTTP_FCGI_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/map.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/own.h"

#ifdef __cplusplus
extern "C" {
#endif

/* fcgi.ErrRequestAborted, what a handler reading the body of a request gets
 * once the web server has aborted the request. */
extern const Error fcgi_err_request_aborted;

/* fcgi.ErrConnClosed, what a handler reading the body of a request gets once
 * the connection to the web server has closed. */
extern const Error fcgi_err_conn_closed;

/* fcgi.Serve. Accepts FastCGI connections on l, with a goroutine for each that
 * reads the requests on it and serves them with handler, each on a goroutine
 * of its own. A listener with no vt means one made from os_stdin, which is
 * closed and freed before this returns. A handler with no vt means
 * http_default_serve_mux. Returns the error from accepting a connection, and
 * runs until there is one. Call it from a goroutine. */
BURROW_BORROWS(ret) Error fcgi_serve(NetListener l, HttpHandler handler);

/* fcgi.ProcessEnv. The FastCGI variables of the request r that the request
 * says nothing of anywhere else, such as REMOTE_USER, as a Map of Str to Str.
 * They live in r's context. NULL when r did not come from fcgi_serve. */
BURROW_BORROWS(ret, r) Map *fcgi_process_env(const HttpRequest *r);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_HTTP_FCGI_H */
