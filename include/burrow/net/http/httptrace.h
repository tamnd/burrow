/* net/http/httptrace, for watching what a client request goes through.
 *
 * An HttptraceClientTrace is a set of hooks, any of which can be left out. Put
 * one in a context with httptrace_with_client_trace and send a request with
 * that context, and the transport calls the hooks as it finds a connection,
 * looks up the name, dials, writes the request and reads the response:
 *
 *     static void got_conn(void *env, HttptraceGotConnInfo info) {
 *         (void)env;
 *         fmt_printf_v("reused: %t\n", info.reused);
 *     }
 *
 *     HttptraceClientTrace trace = {
 *         .got_conn = BURROW_FN(HttptraceGotConnFunc, got_conn, NULL),
 *     };
 *     Context ctx = httptrace_with_client_trace(a, http_request_context(req), &trace);
 *     HttpResponse *res = http_client_do(c, http_request_with_context(req, a, ctx), &err);
 *     ...
 *     http_response_free(res);
 *     context_release(ctx);
 *
 * The hooks run on the transport's goroutines, so a hook that touches
 * something the caller touches too needs a lock. They can run until the
 * response is freed, so the trace has to live that long.
 *
 * Two things differ from Go. A trace put in a context that already has one
 * calls its own hooks and then the older one's, which is what Go does, but
 * here it keeps a pointer to the older trace rather than rewriting its own
 * hook fields, so the hooks run through the httptrace_client_trace_* functions
 * below and not by calling a field. And the hooks for a dial the transport
 * starts for the request (dns_start, dns_done, connect_start and connect_done)
 * stop when the request gets its connection. In Go a dial that loses the race
 * to an idle connection goes on calling them, but here nothing says the trace
 * is still there by then.
 *
 * The TLS hooks are not here yet, as burrow has no crypto/tls.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package net/http/httptrace */

#ifndef BURROW_NET_HTTP_HTTPTRACE_H
#define BURROW_NET_HTTP_HTTPTRACE_H

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/net/textproto.h"
#include "burrow/own.h"
#include "burrow/slice.h"
#include "burrow/time.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DNSStartInfo: what a lookup is for. */
typedef struct HttptraceDNSStartInfo {
    Str host;
} HttptraceDNSStartInfo;

/* DNSDoneInfo: how a lookup went. addrs is a Slice of NetIPAddr, and the
 * hook must not change it. coalesced says the answer was shared with another
 * lookup of the same name that was going on at the same time. */
typedef struct HttptraceDNSDoneInfo {
    Slice addrs;
    Error err;
    bool coalesced;
} HttptraceDNSDoneInfo;

/* GotConnInfo: the connection a request got. conn belongs to the transport,
 * so the hook must not read from it, write to it or close it. idle_time is how
 * long it sat in the pool, when was_idle says it came from there. */
typedef struct HttptraceGotConnInfo {
    NetConn conn;
    Duration idle_time;
    bool reused;
    bool was_idle;
} HttptraceGotConnInfo;

/* WroteRequestInfo: how writing the request went. */
typedef struct HttptraceWroteRequestInfo {
    Error err;
} HttptraceWroteRequestInfo;

BURROW_FUNC(HttptraceGetConnFunc, void, Str host_port);
BURROW_FUNC(HttptraceGotConnFunc, void, HttptraceGotConnInfo info);
BURROW_FUNC(HttptracePutIdleConnFunc, void, Error err);
BURROW_FUNC(HttptraceGot1xxResponseFunc, Error, Int code, TextprotoMIMEHeader header);
BURROW_FUNC(HttptraceDNSStartFunc, void, HttptraceDNSStartInfo info);
BURROW_FUNC(HttptraceDNSDoneFunc, void, HttptraceDNSDoneInfo info);
BURROW_FUNC(HttptraceConnectStartFunc, void, Str network, Str addr);
BURROW_FUNC(HttptraceConnectDoneFunc, void, Str network, Str addr, Error err);
BURROW_FUNC(HttptraceWroteHeaderFieldFunc, void, Str key, Slice values);
BURROW_FUNC(HttptraceWroteRequestFunc, void, HttptraceWroteRequestInfo info);

/* ClientTrace. A zero hook is not called. What a hook is handed is only good
 * for the length of the call. */
typedef struct HttptraceClientTrace {
    /* Before a connection is made or taken from the pool, with the
     * "host:port" of the target or the proxy. Called even when there is an
     * idle connection to use. */
    HttptraceGetConnFunc get_conn;

    /* Once there is a connection. Failing to get one has no hook: the round
     * trip's error says what went wrong. */
    HttptraceGotConnFunc got_conn;

    /* When the connection goes back to the pool, with no error, or with the
     * reason it did not. Not called when keep-alives are disabled. It comes
     * before freeing the response returns. */
    HttptracePutIdleConnFunc put_idle_conn;

    /* When the first byte of the response's header is there. */
    Func got_first_response_byte;

    /* When the server says "100 Continue". */
    Func got100_continue;

    /* For each 1xx response that comes before the final one, "100 Continue"
     * included. An error from it ends the request with that error. */
    HttptraceGot1xxResponseFunc got1xx_response;

    /* When a lookup starts and when it ends. */
    HttptraceDNSStartFunc dns_start;
    HttptraceDNSDoneFunc dns_done;

    /* When a dial starts and when it ends, with err saying how it went. With
     * more than one address these can come more than once. */
    HttptraceConnectStartFunc connect_start;
    HttptraceConnectDoneFunc connect_done;

    /* After each header of the request is written, with its values as a
     * Slice of Str. They may still be in a buffer at the time. */
    HttptraceWroteHeaderFieldFunc wrote_header_field;

    /* After all of the header is written. */
    Func wrote_headers;

    /* When the request said "Expect: 100-continue", after the header is
     * written and before waiting for the server's "100 Continue". */
    Func wait100_continue;

    /* After the request and its body are written, or failed to be. A request
     * that is tried again calls it again. */
    HttptraceWroteRequestFunc wrote_request;

    /* Internal: the trace this one was put over, and the hooks the dial
     * sees. Set by httptrace_with_client_trace. */
    const struct HttptraceClientTrace *burrow_old;
    const void *burrow_net;
} HttptraceClientTrace;

/* ContextClientTrace: the trace in ctx, or NULL. */
BURROW_BORROWS(ret, ctx) HttptraceClientTrace *
httptrace_context_client_trace(Context ctx);

/* WithClientTrace: a context made from ctx, in a, that carries trace. When
 * ctx carries a trace already, trace's hooks are called first and then the
 * older trace's. trace has to outlive the context and every request sent with
 * it, and give it back with context_release.
 *
 * A trace can be put in any number of contexts, but over one older trace at
 * most: putting it over a different one panics, as would a loop of traces.
 * Panics on a nil trace, with Go's message. NULL is not a context: a nil
 * Context is what comes back when a says no. */
BURROW_OWNS(ret) Context httptrace_with_client_trace(Alloc *a, Context ctx,
                                                     HttptraceClientTrace *trace);

/* Call a hook of t and of each trace under it, newest first, which is how the
 * transport calls them. Any of them can be called on a NULL trace, which does
 * nothing. got1xx_response gives the error of the oldest hook it called, the
 * same as Go's composed hook does. */
void httptrace_client_trace_get_conn(const HttptraceClientTrace *t, Str host_port);
void httptrace_client_trace_got_conn(const HttptraceClientTrace *t,
                                     HttptraceGotConnInfo info);
void httptrace_client_trace_put_idle_conn(const HttptraceClientTrace *t, Error err);
void httptrace_client_trace_got_first_response_byte(const HttptraceClientTrace *t);
void httptrace_client_trace_got100_continue(const HttptraceClientTrace *t);
BURROW_BORROWS(ret) Error httptrace_client_trace_got1xx_response(
    const HttptraceClientTrace *t, Int code, TextprotoMIMEHeader header);
void httptrace_client_trace_dns_start(const HttptraceClientTrace *t,
                                      HttptraceDNSStartInfo info);
void httptrace_client_trace_dns_done(const HttptraceClientTrace *t,
                                     HttptraceDNSDoneInfo info);
void httptrace_client_trace_connect_start(const HttptraceClientTrace *t, Str network,
                                          Str addr);
void httptrace_client_trace_connect_done(const HttptraceClientTrace *t, Str network,
                                         Str addr, Error err);
void httptrace_client_trace_wrote_header_field(const HttptraceClientTrace *t, Str key,
                                               Slice values);
void httptrace_client_trace_wrote_headers(const HttptraceClientTrace *t);
void httptrace_client_trace_wait100_continue(const HttptraceClientTrace *t);
void httptrace_client_trace_wrote_request(const HttptraceClientTrace *t,
                                          HttptraceWroteRequestInfo info);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NET_HTTP_HTTPTRACE_H */
