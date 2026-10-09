/* net/http/cgi, CGI as RFC 3875 has it, from both sides.
 *
 * A CgiHandler is the web server's side. It runs a program for each request,
 * with the request in its environment and the body on its standard input, and
 * sends back what the program prints:
 *
 *     CgiHandler h = {0};
 *     h.path = BURROW_S("/usr/lib/cgi-bin/hello");
 *     h.root = BURROW_S("/hello");
 *     err = http_listen_and_serve(BURROW_S(":8080"), cgi_handler_as_handler(&h));
 *
 * cgi_serve is the program's side. It reads the request a CgiHandler, or any
 * other web server, put in the environment, and serves it with a handler:
 *
 *     int main(void) {
 *         Error err = cgi_serve(handler);
 *         ...
 *     }
 *
 * Go's request has a TLS field, which cgi.Request sets when HTTPS is "on".
 * There is no crypto/tls here yet and so no TLS field, and HTTPS shows only in
 * the URL's scheme, which is https. A CgiHandler always runs the program as
 * if the request came without TLS.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/http/cgi */

#ifndef BURROW_NET_HTTP_CGI_H
#define BURROW_NET_HTTP_CGI_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/net/http.h"
#include "burrow/own.h"
#include "burrow/slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ the program */

/* cgi.Request. The request the environment describes, for a program run by a
 * web server as CGI. When the request has a body, its body reads that many
 * bytes from standard input. Free it with http_request_free. */
BURROW_OWNS(ret) HttpRequest *cgi_request(Alloc *a, Error *err);

/* cgi.RequestFromMap. The request that params, a Map of Str to Str holding the
 * CGI variables, describes. The request's body is left unset. The request
 * copies what it keeps, so params can go once this returns. */
BURROW_OWNS(ret) HttpRequest *cgi_request_from_map(Alloc *a, Map *params, Error *err);

/* cgi.Serve. Serves the request in the environment with handler, writing the
 * response to standard output. A handler with no vt means
 * http_default_serve_mux. An error when there is no request in the
 * environment, or the response could not be written. */
BURROW_STATIC(ret) Error cgi_serve(HttpHandler handler);

/* ------------------------------------------------------------- the server */

/* cgi.Handler. Runs the program at path for each request, in the directory
 * dir, or the one path is in when dir is empty.
 *
 * root is the start of the URL path the handler is at, "" for "/". What comes
 * after it is the program's PATH_INFO. env is more of the program's
 * environment, of Str "key=value", and inherit_env names variables of this
 * process's environment to pass on, of Str. args are the program's arguments
 * after its name, of Str. Errors go to logger, or the standard logger when it
 * is NULL. The program's standard error goes to stderr_, or os_stderr when it
 * has no vt.
 *
 * A program that answers with a Location that starts with "/" is redirected
 * to internally, by serving a GET for that path with path_location_handler,
 * when it has a vt.
 *
 * The handler reads its fields and changes none of them, so one handler can
 * serve any number of requests at once. */
typedef struct CgiHandler {
    Str path;
    Str root;
    Str dir;
    Slice env;         /* of Str */
    Slice inherit_env; /* of Str */
    LogLogger *logger;
    Slice args; /* of Str */
    IoWriter stderr_;
    HttpHandler path_location_handler;
} CgiHandler;

/* Handler.ServeHTTP. */
void cgi_handler_serve_http(CgiHandler *h, HttpResponseWriter rw, HttpRequest *req);

/* h as an HttpHandler. */
BURROW_BORROWS(ret, h) HttpHandler cgi_handler_as_handler(CgiHandler *h);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_HTTP_CGI_H */
