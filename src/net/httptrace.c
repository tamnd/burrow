/* Derived from Go's src/net/http/httptrace/trace.go.
 * Go source: go1.27.1.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net/http/httptrace.h"

#include "internal.h"

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The trace whose burrow_net field self is. */
static const HttptraceClientTrace *tr_of(const void *self) {
    return (const HttptraceClientTrace *)(const void *)((const char *)self -
                                                        offsetof(HttptraceClientTrace,
                                                                 burrow_net));
}

/* What nettrace.Trace's hooks turn into: the trace's own, with Go's info
 * structs around what net hands over. */

static void tr_net_dns_start(const void *self, Str host) {
    httptrace_client_trace_dns_start(tr_of(self),
                                     (HttptraceDNSStartInfo){.host = host});
}

static void tr_net_dns_done(const void *self, Slice addrs, bool coalesced, Error err) {
    httptrace_client_trace_dns_done(
        tr_of(self),
        (HttptraceDNSDoneInfo){.addrs = addrs, .err = err, .coalesced = coalesced});
}

static void tr_net_connect_start(const void *self, Str network, Str addr) {
    httptrace_client_trace_connect_start(tr_of(self), network, addr);
}

static void tr_net_connect_done(const void *self, Str network, Str addr, Error err) {
    httptrace_client_trace_connect_done(tr_of(self), network, addr, err);
}

static const burrow__NetTraceVT tr_net_vt = {
    tr_net_dns_start,
    tr_net_dns_done,
    tr_net_connect_start,
    tr_net_connect_done,
};

/* hasNetHooks, over the whole chain. */
static bool tr_has_net_hooks(const HttptraceClientTrace *t) {
    for (; t != NULL; t = t->burrow_old) {
        if (t->dns_start.f != NULL || t->dns_done.f != NULL ||
            t->connect_start.f != NULL || t->connect_done.f != NULL)
            return true;
    }
    return false;
}

HttptraceClientTrace *httptrace_context_client_trace(Context ctx) {
    if (BURROW_CONTEXT_IS_NIL(ctx))
        return NULL;
    Any v = context_value(ctx, burrow__nettrace_key);
    if (v.data == NULL)
        return NULL;
    return (HttptraceClientTrace *)(uintptr_t)tr_of(v.data);
}

Context httptrace_with_client_trace(Alloc *a, Context ctx,
                                    HttptraceClientTrace *trace) {
    if (trace == NULL)
        panic_str(BURROW_S("nil trace"));
    const HttptraceClientTrace *old = httptrace_context_client_trace(ctx);
    if (trace->burrow_old != NULL && trace->burrow_old != old)
        panic_str(BURROW_S("httptrace: trace already put over a different trace"));
    for (const HttptraceClientTrace *o = old; o != NULL; o = o->burrow_old) {
        if (o == trace)
            panic_str(BURROW_S("httptrace: trace put over itself"));
    }
    trace->burrow_old = old;
    trace->burrow_net = tr_has_net_hooks(trace) ? (const void *)&tr_net_vt : NULL;
    return context_with_value(
        a, ctx, burrow__nettrace_key,
        (Any){TYPE_UINTPTR, (void *)(uintptr_t)&trace->burrow_net});
}

/* ------------------------------------------------------------- the hooks
 *
 * Go's compose makes each hook call the new trace's and then the old one's,
 * so walking from the newest down calls them in the same order. */

void httptrace_client_trace_get_conn(const HttptraceClientTrace *t, Str host_port) {
    for (; t != NULL; t = t->burrow_old)
        if (t->get_conn.f != NULL)
            t->get_conn.f(t->get_conn.env, host_port);
}

void httptrace_client_trace_got_conn(const HttptraceClientTrace *t,
                                     HttptraceGotConnInfo info) {
    for (; t != NULL; t = t->burrow_old)
        if (t->got_conn.f != NULL)
            t->got_conn.f(t->got_conn.env, info);
}

void httptrace_client_trace_put_idle_conn(const HttptraceClientTrace *t, Error err) {
    for (; t != NULL; t = t->burrow_old)
        if (t->put_idle_conn.f != NULL)
            t->put_idle_conn.f(t->put_idle_conn.env, err);
}

void httptrace_client_trace_got_first_response_byte(const HttptraceClientTrace *t) {
    for (; t != NULL; t = t->burrow_old)
        if (t->got_first_response_byte.f != NULL)
            t->got_first_response_byte.f(t->got_first_response_byte.env);
}

void httptrace_client_trace_got100_continue(const HttptraceClientTrace *t) {
    for (; t != NULL; t = t->burrow_old)
        if (t->got100_continue.f != NULL)
            t->got100_continue.f(t->got100_continue.env);
}

Error httptrace_client_trace_got1xx_response(const HttptraceClientTrace *t, Int code,
                                             TextprotoMIMEHeader header) {
    Error err = BURROW_NO_ERROR;
    for (; t != NULL; t = t->burrow_old)
        if (t->got1xx_response.f != NULL)
            err = t->got1xx_response.f(t->got1xx_response.env, code, header);
    return err;
}

void httptrace_client_trace_dns_start(const HttptraceClientTrace *t,
                                      HttptraceDNSStartInfo info) {
    for (; t != NULL; t = t->burrow_old)
        if (t->dns_start.f != NULL)
            t->dns_start.f(t->dns_start.env, info);
}

void httptrace_client_trace_dns_done(const HttptraceClientTrace *t,
                                     HttptraceDNSDoneInfo info) {
    for (; t != NULL; t = t->burrow_old)
        if (t->dns_done.f != NULL)
            t->dns_done.f(t->dns_done.env, info);
}

void httptrace_client_trace_connect_start(const HttptraceClientTrace *t, Str network,
                                          Str addr) {
    for (; t != NULL; t = t->burrow_old)
        if (t->connect_start.f != NULL)
            t->connect_start.f(t->connect_start.env, network, addr);
}

void httptrace_client_trace_connect_done(const HttptraceClientTrace *t, Str network,
                                         Str addr, Error err) {
    for (; t != NULL; t = t->burrow_old)
        if (t->connect_done.f != NULL)
            t->connect_done.f(t->connect_done.env, network, addr, err);
}

void httptrace_client_trace_wrote_header_field(const HttptraceClientTrace *t, Str key,
                                               Slice values) {
    for (; t != NULL; t = t->burrow_old)
        if (t->wrote_header_field.f != NULL)
            t->wrote_header_field.f(t->wrote_header_field.env, key, values);
}

void httptrace_client_trace_wrote_headers(const HttptraceClientTrace *t) {
    for (; t != NULL; t = t->burrow_old)
        if (t->wrote_headers.f != NULL)
            t->wrote_headers.f(t->wrote_headers.env);
}

void httptrace_client_trace_wait100_continue(const HttptraceClientTrace *t) {
    for (; t != NULL; t = t->burrow_old)
        if (t->wait100_continue.f != NULL)
            t->wait100_continue.f(t->wait100_continue.env);
}

void httptrace_client_trace_wrote_request(const HttptraceClientTrace *t,
                                          HttptraceWroteRequestInfo info) {
    for (; t != NULL; t = t->burrow_old)
        if (t->wrote_request.f != NULL)
            t->wrote_request.f(t->wrote_request.env, info);
}
