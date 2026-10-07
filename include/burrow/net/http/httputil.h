/* net/http/httputil, HTTP helpers that net/http leaves out: a reverse proxy,
 * dumping requests and responses as they go over the wire, and the chunked
 * encoding on its own.
 *
 * A reverse proxy is a handler that sends each request on to another server
 * and copies the answer back:
 *
 *     Url *target = url_parse(a, BURROW_S("http://127.0.0.1:8081/base"), &err);
 *     HttputilReverseProxy *p = httputil_new_single_host_reverse_proxy(a, target);
 *     err = http_listen_and_serve(BURROW_S(":8080"),
 *                                 httputil_reverse_proxy_as_handler(p));
 *
 * A dump is the bytes of the request or response, for debugging:
 *
 *     Slice dump = httputil_dump_request(a, req, true, &err);
 *     if (!BURROW_FAILED(err))
 *         fmt_printf_v("%s", str_from_bytes(dump.p, dump.len));
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/http/httputil */

#ifndef BURROW_NET_HTTP_HTTPUTIL_H
#define BURROW_NET_HTTP_HTTPUTIL_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/log.h"
#include "burrow/mem.h"
#include "burrow/net/http.h"
#include "burrow/net/url.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/time.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ chunked */

/* httputil.ErrLineTooLong, "header line too long", from a chunked reader
 * given a line longer than it will read. It is the same error net/http gives
 * for the same thing, so errors_is matches either. */
extern const Error httputil_err_line_too_long;

/* httputil.NewChunkedReader. A reader of the data in the chunks that r has,
 * the format of a body sent with Transfer-Encoding: chunked. It gives io_eof
 * after the chunk of no bytes that ends the body, and leaves any trailer after
 * that in r. A BufioReader is read from as it is, and anything else through a
 * new one made in a. net/http does this for itself, so this is only for a
 * program that reads chunks some other way. A nil reader when a says no, and
 * httputil_chunked_reader_free gives the memory back. */
IoReader httputil_new_chunked_reader(Alloc *a, IoReader r);
void httputil_chunked_reader_free(Alloc *a, IoReader r);

/* httputil.NewChunkedWriter. Each write goes to w as a chunk, a write of no
 * bytes writes nothing, and closing it writes the "0\r\n" of the last chunk.
 * The trailer and the "\r\n" after it are left to the caller to write. A nil
 * writer when a says no, and httputil_chunked_writer_free gives the memory
 * back. */
IoWriteCloser httputil_new_chunked_writer(Alloc *a, IoWriter w);
void httputil_chunked_writer_free(Alloc *a, IoWriteCloser w);

/* -------------------------------------------------------------------- dumps
 *
 * The dumps are made in a. When body is true each of them reads the whole
 * body into memory from a, closes it, and sets the body to a reader of those
 * bytes again, so the request or response can still be used after. a has to
 * last as long as that body does, which with an arena is no trouble. A dump
 * is nil on an error. */

/* httputil.DumpRequest. req as a server got it: the request line with the
 * method, which is GET when it is "", request_uri or the URL's own, and
 * proto_major and proto_minor; then the Host field unless request_uri is an
 * absolute URL, Transfer-Encoding, and the header without Host,
 * Transfer-Encoding and Trailer. A body comes after in chunks when
 * transfer_encoding starts with "chunked". A request about to be sent is
 * better dumped with httputil_dump_request_out. */
BURROW_OWNS(ret) Slice httputil_dump_request(Alloc *a, HttpRequest *req, bool body,
                                             Error *err);

/* httputil.DumpRequestOut. req as an HttpTransport would send it, with the
 * User-Agent, Accept-Encoding and other fields the transport adds. It is sent
 * through a transport to a connection that only records it, and an "https"
 * request goes as "http" so nothing tries to start TLS. When body is false a
 * body is still sent, as that many "x" bytes, so the header is right, and is
 * then cut off the dump. */
BURROW_OWNS(ret) Slice httputil_dump_request_out(Alloc *a, HttpRequest *req, bool body,
                                                 Error *err);

/* httputil.DumpResponse. resp as http_response_write writes it. When body is
 * false the body is left out, but content_length stays what it was, so the
 * header has the length the body would have had. */
BURROW_OWNS(ret) Slice httputil_dump_response(Alloc *a, HttpResponse *resp, bool body,
                                              Error *err);

/* ------------------------------------------------------------ reverse proxy */

/* httputil.ProxyRequest, what a rewrite function gets: in, the request the
 * proxy got, which is not to be changed, and out, the request it will send.
 * out is the proxy's copy of in, made with http_request_clone, and a rewrite
 * may change it or put another request there. The proxy gives either back with
 * http_request_free once it is done, so a request put there has to be one that
 * can go that way. Strings for out are best made in out's own arena, which
 * lasts as long as out does and is where the functions below make theirs. */
typedef struct HttputilProxyRequest {
    HttpRequest *in;
    HttpRequest *out;
} HttputilProxyRequest;

/* ProxyRequest.SetURL. Sends out to target: its scheme and host become
 * target's, its path is target's path joined to its own with one slash
 * between them, and target's query goes in front of its own. out's host is
 * set to "", so the Host field the server sees is target's host. */
void httputil_proxy_request_set_url(HttputilProxyRequest *r, const Url *target);

/* ProxyRequest.SetXForwarded. Sets X-Forwarded-For on out to the client's IP
 * address from in's remote_addr, after any values out had for it already,
 * X-Forwarded-Host to in's host, and X-Forwarded-Proto to "http". The proxy
 * takes those three off out before a rewrite runs, so a rewrite that wants
 * in's X-Forwarded-For kept copies it to out before calling this. */
void httputil_proxy_request_set_x_forwarded(HttputilProxyRequest *r);

/* The functions an HttputilReverseProxy calls, as described there. */
BURROW_FUNC(HttputilRewriteFunc, void, HttputilProxyRequest *r);
BURROW_FUNC(HttputilDirectorFunc, void, HttpRequest *req);
BURROW_FUNC(HttputilModifyResponseFunc, Error, HttpResponse *res);
BURROW_FUNC(HttputilErrorHandlerFunc, void, HttpResponseWriter rw, HttpRequest *req,
            Error err);

/* httputil.BufferPool, where a proxy gets the buffers it copies response
 * bodies through, with get, and gives them back, with put. */
typedef struct HttputilBufferPoolVT {
    const Type *self_type;
    Slice (*get)(void *self);
    void (*put)(void *self, Slice buf);
} HttputilBufferPoolVT;

typedef struct HttputilBufferPool {
    const HttputilBufferPoolVT *vt;
    void *data;
} HttputilBufferPool;

/* httputil.ReverseProxy, a handler that sends the requests it gets to another
 * server and copies the responses back to the client. A zeroed one with
 * rewrite or director set is ready to use.
 *
 * rewrite turns the request into the one to send. Before it runs the proxy
 * takes the hop-by-hop fields off the outgoing request: the ones Connection
 * names, and Connection, Proxy-Connection, Keep-Alive, Proxy-Authenticate,
 * Proxy-Authorization, TE, Trailer, Transfer-Encoding and Upgrade, keeping
 * "TE: trailers" and the fields an upgrade needs. It takes off Forwarded and
 * the three X-Forwarded fields as well, and the query parameters that do not
 * parse. Exactly one of rewrite and director is to be set.
 *
 * director is Go's older and deprecated way: it changes the outgoing request
 * in place, the hop-by-hop fields come off after it runs, and the client's IP
 * address is added to X-Forwarded-For, unless the director put the key in the
 * header with a nil Slice of values. A director that parsed the form gets the
 * query parameters that do not parse taken off. Strings it makes go in the
 * request's own arena.
 *
 * transport sends the request, and is http_default_transport when nil.
 * flush_interval is how often what has been copied of a response body is
 * flushed to the client, with 0 for never and a negative one for after every
 * write. A response of unknown length, or of type text/event-stream, is
 * flushed after every write whatever this says. error_log is where errors go,
 * and the standard logger when NULL. buffer_pool, when set, gives the buffers
 * bodies are copied through.
 *
 * modify_response, when set, gets the response before it is copied, with the
 * hop-by-hop fields off it, and can change it. An error from it closes the
 * body and goes to error_handler. error_handler gets the errors there are in
 * sending the request, and when nil they are logged and the client gets a 502
 * Bad Gateway.
 *
 * A 101 Switching Protocols from the server, to an upgrade the client asked
 * for, takes the client's connection from the server it came on, and the
 * proxy copies bytes both ways until either side stops. The response's body
 * has to be one http_response_body_writer can write to for that.
 *
 * A body that fails partway, once the status has been sent, panics with
 * http_err_abort_handler when the request came through an HttpServer, which
 * recovers from it, and is only logged otherwise. */
typedef struct HttputilReverseProxy {
    HttputilRewriteFunc rewrite;
    HttpRoundTripper transport;
    Duration flush_interval;
    LogLogger *error_log;
    HttputilBufferPool buffer_pool;
    HttputilModifyResponseFunc modify_response;
    HttputilErrorHandlerFunc error_handler;
    HttputilDirectorFunc director;

    /* What httputil_new_single_host_reverse_proxy made. */
    Alloc *a;
    Url *target;
} HttputilReverseProxy;

/* httputil.NewSingleHostReverseProxy. A proxy, made in a, whose director
 * sends each request to target's scheme, host and path, so that a request
 * for "/dir" to a target of "/base" goes to "/base/dir". The Host field is
 * left as the client sent it, as are the X-Forwarded fields. The proxy has a
 * copy of target, so target may go after this. NULL when a says no, and
 * httputil_reverse_proxy_free gives it back. */
BURROW_OWNS(ret) HttputilReverseProxy *
httputil_new_single_host_reverse_proxy(Alloc *a, const Url *target);

/* Gives back a proxy from httputil_new_single_host_reverse_proxy. NULL is
 * fine. */
void httputil_reverse_proxy_free(HttputilReverseProxy *p);

/* ReverseProxy.ServeHTTP. */
void httputil_reverse_proxy_serve_http(HttputilReverseProxy *p, HttpResponseWriter rw,
                                       HttpRequest *req);

/* p as an HttpHandler, borrowing p. */
BURROW_BORROWS(ret, p) HttpHandler
httputil_reverse_proxy_as_handler(HttputilReverseProxy *p);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_HTTP_HTTPUTIL_H */
