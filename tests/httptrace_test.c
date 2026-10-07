/* Derived from Go's src/net/http/httptrace/trace_test.go, and the tests of the
 * trace hooks in src/net/http/transport_test.go and clientserver_test.go, in
 * their HTTP/1 mode.
 * Go source: go1.27.1.
 *
 * Go's TestCompose calls the unexported compose. Here composing is putting one
 * trace over another in a context, so the test does that.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/internal.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/http.h"
#include "burrow/net/http/httptest.h"
#include "burrow/net/http/httptrace.h"
#include "burrow/net/textproto.h"
#include "burrow/netpoll.h"
#include "burrow/sort.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_TCP 1
#endif

static void need_tcp(TestingT *t) {
#if !defined(HAVE_TCP)
    testing_t_skip_v(t, "TCP here needs the readiness poll FD");
#else
    (void)t;
#endif
}

/* t.Fatalf, with a return the analyzer can see. */
#define FATALF(...)                                                                    \
    do {                                                                               \
        testing_t_fatalf_v(t, __VA_ARGS__);                                            \
        return;                                                                        \
    } while (0)

static Str cs(const char *s) {
    return str_from_cstr(s);
}

static const char *handler_failed;

#define HCHECK(cond)                                                                   \
    do {                                                                               \
        if (!(cond) && handler_failed == NULL)                                         \
            handler_failed = #cond;                                                    \
    } while (0)

static void check_handlers(TestingT *t) {
    if (handler_failed != NULL)
        testing_t_errorf_v(t, "in a handler or a hook: %s", handler_failed);
    handler_failed = NULL;
}

static Int write_str(HttpResponseWriter w, Str s, Error *err) {
    return http_response_writer_write(
        w, slice_from((void *)(uintptr_t)s.p, s.len, s.len, TYPE_BYTE), err);
}

static void close_body(HttpResponse *res) {
    (void)res->body.vt->closer.close(res->body.data);
}

/* ------------------------------------------------------- trace_test.go */

typedef struct ByteHook {
    StringsBuilder *buf;
    Byte b;
    TestingT *t;
} ByteHook;

static void connect_start_byte(void *env, Str network, Str addr) {
    (void)network;
    ByteHook *h = (ByteHook *)env;
    if (!str_eq(addr, cs("addr")))
        testing_t_errorf_v(h->t, "args for %q case = %q, %q; want addr of \"addr\"",
                           str_from_bytes(&h->b, 1), network, addr);
    (void)strings_builder_write_byte(h->buf, h->b);
}

static HttptraceConnectStartFunc connect_start(ByteHook *h) {
    return BURROW_FN(HttptraceConnectStartFunc, connect_start_byte, h);
}

static void TestWithClientTrace(TestingT *t) {
    StringsBuilder buf = STRINGS_BUILDER(heap_allocator());
    ByteHook o = {&buf, 'O', t};
    ByteHook n = {&buf, 'N', t};

    HttptraceClientTrace oldtrace = {.connect_start = connect_start(&o)};
    Context ctx =
        httptrace_with_client_trace(heap_allocator(), context_background(), &oldtrace);
    HttptraceClientTrace newtrace = {.connect_start = connect_start(&n)};
    Context ctx2 = httptrace_with_client_trace(heap_allocator(), ctx, &newtrace);
    HttptraceClientTrace *trace = httptrace_context_client_trace(ctx2);

    strings_builder_reset(&buf);
    httptrace_client_trace_connect_start(trace, cs("net"), cs("addr"));
    Str got = strings_builder_string(&buf);
    if (!str_eq(got, cs("NO")))
        testing_t_errorf_v(t, "got %q; want %q", got, cs("NO"));
    context_release(ctx2);
    context_release(ctx);
    strings_builder_reset(&buf);
}

static void TestCompose(TestingT *t) {
    StringsBuilder buf = STRINGS_BUILDER(heap_allocator());
    ByteHook th = {&buf, 'T', t};
    ByteHook oh = {&buf, 'O', t};

    struct {
        bool trace_hook;
        bool has_old;
        const char *want;
    } tests[] = {
        {true, false, "T"},
        {true, true, "TO"},
        {false, true, "O"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        strings_builder_reset(&buf);
        HttptraceClientTrace old = {.connect_start = connect_start(&oh)};
        HttptraceClientTrace tr;
        memset(&tr, 0, sizeof tr);
        if (tests[i].trace_hook)
            tr.connect_start = connect_start(&th);

        Context base = context_background();
        Context octx = base;
        if (tests[i].has_old)
            octx = httptrace_with_client_trace(heap_allocator(), base, &old);
        Context ctx = httptrace_with_client_trace(heap_allocator(), octx, &tr);
        httptrace_client_trace_connect_start(httptrace_context_client_trace(ctx),
                                             cs("net"), cs("addr"));
        Str got = strings_builder_string(&buf);
        if (!str_eq(got, cs(tests[i].want)))
            testing_t_errorf_v(t, "%d. got = %q; want %q", (Int)i, got,
                               cs(tests[i].want));
        context_release(ctx);
        if (tests[i].has_old)
            context_release(octx);
    }
    strings_builder_reset(&buf);
}

/* A trace with no hooks in a context gives back that trace, and one put over
 * a trace with no hooks calls only its own. */
static void TestContextClientTrace(TestingT *t) {
    CHECK(httptrace_context_client_trace(context_background()) == NULL);
    StringsBuilder buf = STRINGS_BUILDER(heap_allocator());
    ByteHook n = {&buf, 'N', t};
    HttptraceClientTrace empty;
    memset(&empty, 0, sizeof empty);
    Context ctx =
        httptrace_with_client_trace(heap_allocator(), context_background(), &empty);
    CHECK(httptrace_context_client_trace(ctx) == &empty);
    HttptraceClientTrace tr = {.connect_start = connect_start(&n)};
    Context ctx2 = httptrace_with_client_trace(heap_allocator(), ctx, &tr);
    CHECK(httptrace_context_client_trace(ctx2) == &tr);
    httptrace_client_trace_connect_start(httptrace_context_client_trace(ctx2),
                                         cs("net"), cs("addr"));
    CHECK(str_eq(strings_builder_string(&buf), cs("N")));
    /* The same trace can go in another context over the same one. */
    Context ctx3 = httptrace_with_client_trace(heap_allocator(), ctx, &tr);
    CHECK(httptrace_context_client_trace(ctx3) == &tr);
    context_release(ctx3);
    context_release(ctx2);
    context_release(ctx);
    strings_builder_reset(&buf);
}

/* --------------------------------------------------------- the event log */

/* The logf of Go's tests: lines written by the hooks, under a lock. */
typedef struct EvLog {
    SyncMutex mu;
    Arena ar;
    StringsBuilder b;
} EvLog;

static void ev_init(EvLog *lg) {
    memset(lg, 0, sizeof *lg);
    arena_init(&lg->ar, heap_allocator(), 0);
    lg->b = STRINGS_BUILDER(arena_allocator(&lg->ar));
}

/* Takes the lock and gives the allocator to make the line in. */
static Alloc *ev_lock(EvLog *lg) {
    sync_mutex_lock(&lg->mu);
    return arena_allocator(&lg->ar);
}

/* Adds line and gives the lock back. */
static void ev_unlock(EvLog *lg, Str line) {
    (void)strings_builder_write_string(&lg->b, line, NULL);
    (void)strings_builder_write_byte(&lg->b, '\n');
    sync_mutex_unlock(&lg->mu);
}

static Str ev_string(EvLog *lg) {
    sync_mutex_lock(&lg->mu);
    Str s = strings_builder_string(&lg->b);
    sync_mutex_unlock(&lg->mu);
    return s;
}

/* A Slice of Str as Go's %v prints a []string. */
static Str strs_v(Alloc *a, Slice values) {
    StringsBuilder b = STRINGS_BUILDER(a);
    (void)strings_builder_write_byte(&b, '[');
    for (Int i = 0; i < values.len; i++) {
        if (i > 0)
            (void)strings_builder_write_byte(&b, ' ');
        (void)strings_builder_write_string(&b, *(const Str *)slice_at(values, i), NULL);
    }
    (void)strings_builder_write_byte(&b, ']');
    return strings_builder_string(&b);
}

/* A header as Go's %v prints the map, keys sorted. */
static Str header_v(Alloc *a, TextprotoMIMEHeader h) {
    Slice keys = slice_make(a, TYPE_STRING, 0, map_len(h));
    const void *k;
    for (MapIter it = map_iter(h); map_next(&it, &k, NULL);)
        keys = slice_append(a, keys, k, 1);
    sort_strings(keys);
    StringsBuilder b = STRINGS_BUILDER(a);
    (void)strings_builder_write_string(&b, cs("map["), NULL);
    for (Int i = 0; i < keys.len; i++) {
        Str key = *(const Str *)slice_at(keys, i);
        if (i > 0)
            (void)strings_builder_write_byte(&b, ' ');
        (void)strings_builder_write_string(&b, key, NULL);
        (void)strings_builder_write_byte(&b, ':');
        (void)strings_builder_write_string(
            &b, strs_v(a, textproto_mime_header_values(h, key)), NULL);
    }
    (void)strings_builder_write_byte(&b, ']');
    return strings_builder_string(&b);
}

/* --------------------------------------------------- TestTransportEventTrace */

typedef struct EvTest {
    EvLog log;
    Chan *got_wrote_req;
    bool no_hooks;
    Str ip;
} EvTest;

static void ev_get_conn(void *env, Str host_port) {
    EvLog *lg = &((EvTest *)env)->log;
    ev_unlock(lg, fmt_sprintf_v(ev_lock(lg), "Getting conn for %s ...", host_port));
}

static void ev_got_conn(void *env, HttptraceGotConnInfo ci) {
    EvLog *lg = &((EvTest *)env)->log;
    Alloc *a = ev_lock(lg);
    ev_unlock(lg,
              fmt_sprintf_v(a, "got conn: {Reused:%t WasIdle:%t IdleTime:%s}",
                            ci.reused, ci.was_idle, duration_string(ci.idle_time, a)));
}

static void ev_first_byte(void *env) {
    EvLog *lg = &((EvTest *)env)->log;
    (void)ev_lock(lg);
    ev_unlock(lg, cs("first response byte"));
}

static void ev_put_idle_conn(void *env, Error err) {
    EvLog *lg = &((EvTest *)env)->log;
    ev_unlock(lg, fmt_sprintf_v(ev_lock(lg), "PutIdleConn = %v", err));
}

static void ev_dns_start(void *env, HttptraceDNSStartInfo e) {
    EvLog *lg = &((EvTest *)env)->log;
    ev_unlock(lg, fmt_sprintf_v(ev_lock(lg), "DNS start: {Host:%s}", e.host));
}

/* A DNSDoneInfo as Go's %+v prints it, after prefix. */
static Str dns_done_v(Alloc *a, const char *prefix, HttptraceDNSDoneInfo e) {
    StringsBuilder b = STRINGS_BUILDER(a);
    (void)strings_builder_write_string(&b, cs(prefix), NULL);
    (void)strings_builder_write_string(&b, cs("{Addrs:["), NULL);
    for (Int i = 0; i < e.addrs.len; i++) {
        const NetIPAddr *ia = (const NetIPAddr *)slice_at(e.addrs, i);
        if (i > 0)
            (void)strings_builder_write_byte(&b, ' ');
        (void)strings_builder_write_string(
            &b, fmt_sprintf_v(a, "{IP:%s Zone:%s}", net_ip_string(ia->ip, a), ia->zone),
            NULL);
    }
    (void)strings_builder_write_string(
        &b, fmt_sprintf_v(a, "] Err:%v Coalesced:%t}", e.err, e.coalesced), NULL);
    return strings_builder_string(&b);
}

static void ev_dns_done(void *env, HttptraceDNSDoneInfo e) {
    EvLog *lg = &((EvTest *)env)->log;
    Alloc *a = ev_lock(lg);
    ev_unlock(lg, dns_done_v(a, "DNS done: ", e));
}

static void ev_connect_start(void *env, Str network, Str addr) {
    EvLog *lg = &((EvTest *)env)->log;
    ev_unlock(lg, fmt_sprintf_v(ev_lock(lg), "ConnectStart: Connecting to %s %s ...",
                                network, addr));
}

static void ev_connect_done(void *env, Str network, Str addr, Error err) {
    EvLog *lg = &((EvTest *)env)->log;
    HCHECK(BURROW_OK(err));
    ev_unlock(lg, fmt_sprintf_v(ev_lock(lg), "ConnectDone: connected to %s %s = %v",
                                network, addr, err));
}

static void ev_wrote_header_field(void *env, Str key, Slice values) {
    EvLog *lg = &((EvTest *)env)->log;
    Alloc *a = ev_lock(lg);
    ev_unlock(lg, fmt_sprintf_v(a, "WroteHeaderField: %s: %s", key, strs_v(a, values)));
}

static void ev_wrote_headers(void *env) {
    EvLog *lg = &((EvTest *)env)->log;
    (void)ev_lock(lg);
    ev_unlock(lg, cs("WroteHeaders"));
}

static void ev_wait100_continue(void *env) {
    EvLog *lg = &((EvTest *)env)->log;
    (void)ev_lock(lg);
    ev_unlock(lg, cs("Wait100Continue"));
}

static void ev_got100_continue(void *env) {
    EvLog *lg = &((EvTest *)env)->log;
    (void)ev_lock(lg);
    ev_unlock(lg, cs("Got100Continue"));
}

static void ev_wrote_request(void *env, HttptraceWroteRequestInfo e) {
    EvTest *et = (EvTest *)env;
    ev_unlock(&et->log,
              fmt_sprintf_v(ev_lock(&et->log), "WroteRequest: {Err:%v}", e.err));
    bool v = true;
    chan_send(et->got_wrote_req, &v);
}

static void ev_serve(void *env, HttpResponseWriter w, HttpRequest *r) {
    EvTest *et = (EvTest *)env;
    if (str_eq(r->method, cs("GET"))) {
        /* Do nothing for the second request. */
        return;
    }
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Error err = BURROW_NO_ERROR;
    (void)io_read_all(arena_allocator(&ar), io_read_closer_as_io_reader(r->body), &err);
    HCHECK(BURROW_OK(err));
    arena_free(&ar);
    if (!et->no_hooks)
        (void)chan_recv(et->got_wrote_req, NULL);
    (void)write_str(w, cs("some body"), &err);
}

/* The fake DNS server. */
static Slice ev_lookup(void *env, Alloc *a, Context ctx, Str network, Str host,
                       Error *err) {
    (void)ctx;
    (void)network;
    *err = BURROW_NO_ERROR;
    EvTest *et = (EvTest *)env;
    HCHECK(str_eq(host, cs("dns-is-faked.golang")));
    if (!str_eq(host, cs("dns-is-faked.golang")))
        return slice_nil(TYPE_NET_IP_ADDR);
    NetIPAddr ia = {net_parse_ip(a, et->ip), BURROW_STR_EMPTY};
    Slice out = slice_append(a, slice_nil(TYPE_NET_IP_ADDR), &ia, 1);
    if (out.len != 1)
        *err = burrow_err_out_of_memory;
    return out;
}

static void want_count(TestingT *t, Str got, Str sub, bool more) {
    Int n = strings_count(got, sub);
    if (more && n == 0)
        testing_t_errorf_v(t, "expected substring %q at least once in output.", sub);
    else if (!more && n != 1)
        testing_t_errorf_v(t, "expected substring %q exactly once in output.", sub);
}

static void transport_event_trace(TestingT *t, bool no_hooks) {
    need_tcp(t);
    Str res_body = cs("some body");
    EvTest et;
    memset(&et, 0, sizeof et);
    ev_init(&et.log);
    et.no_hooks = no_hooks;
    et.got_wrote_req = chan_make(heap_allocator(), TYPE_BOOL, 500);
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, ev_serve, &et);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&hf));
    HttpClient *c = httptest_server_client(ts);
    ts->transport.expect_continue_timeout = 1 * TIME_SECOND;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Str addr_str = strings_trim_prefix(ts->url, cs("http://"));
    Str port;
    Error err = BURROW_NO_ERROR;
    et.ip = net_split_host_port(addr_str, &port, &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);

    /* Install a fake DNS server. */
    burrow__NetLookupIPFunc alt = BURROW_FN(burrow__NetLookupIPFunc, ev_lookup, &et);
    Context ctx = context_with_value(a, context_background(),
                                     burrow__net_lookup_ip_alt_resolver_key,
                                     (Any){TYPE_UINTPTR, (void *)&alt});

    Str body = cs("some body");
    StringsReader br;
    strings_reader_reset(&br, body);
    Str url = fmt_sprintf_v(a, "http://dns-is-faked.golang:%s", port);
    HttpRequest *req0 =
        http_new_request(a, cs("POST"), url, strings_reader_as_io_reader(&br), &err);
    if (req0 == NULL)
        FATALF("NewRequest: %v", err);
    CHECK(http_header_add(req0->header, cs("X-Foo-Multiple-Vals"), cs("bar")));
    CHECK(http_header_add(req0->header, cs("X-Foo-Multiple-Vals"), cs("baz")));
    HttptraceClientTrace trace = {
        .get_conn = BURROW_FN(HttptraceGetConnFunc, ev_get_conn, &et),
        .got_conn = BURROW_FN(HttptraceGotConnFunc, ev_got_conn, &et),
        .got_first_response_byte = BURROW_FN(Func, ev_first_byte, &et),
        .put_idle_conn = BURROW_FN(HttptracePutIdleConnFunc, ev_put_idle_conn, &et),
        .dns_start = BURROW_FN(HttptraceDNSStartFunc, ev_dns_start, &et),
        .dns_done = BURROW_FN(HttptraceDNSDoneFunc, ev_dns_done, &et),
        .connect_start = BURROW_FN(HttptraceConnectStartFunc, ev_connect_start, &et),
        .connect_done = BURROW_FN(HttptraceConnectDoneFunc, ev_connect_done, &et),
        .wrote_header_field =
            BURROW_FN(HttptraceWroteHeaderFieldFunc, ev_wrote_header_field, &et),
        .wrote_headers = BURROW_FN(Func, ev_wrote_headers, &et),
        .wait100_continue = BURROW_FN(Func, ev_wait100_continue, &et),
        .got100_continue = BURROW_FN(Func, ev_got100_continue, &et),
        .wrote_request = BURROW_FN(HttptraceWroteRequestFunc, ev_wrote_request, &et),
    };
    if (no_hooks) {
        /* zero out all func pointers, trying to get some path to crash */
        memset(&trace, 0, sizeof trace);
    }
    Context tctx = httptrace_with_client_trace(a, ctx, &trace);
    HttpRequest *req = http_request_with_context(req0, a, tctx);
    CHECK(http_header_set(req->header, cs("Expect"), cs("100-continue")));
    HttpResponse *res = http_client_do(c, req, &err);
    if (res == NULL)
        FATALF("%v", err);
    (void)ev_lock(&et.log);
    ev_unlock(&et.log, cs("got roundtrip.response"));
    Slice slurp = io_read_all(a, io_read_closer_as_io_reader(res->body), &err);
    if (BURROW_FAILED(err))
        FATALF("%v", err);
    (void)ev_lock(&et.log);
    ev_unlock(&et.log, cs("consumed body"));
    Str got_body = str_from_bytes(slurp.p, slurp.len);
    if (!str_eq(got_body, res_body) || res->status_code != 200)
        FATALF("Got %q, %v; want %q, 200 OK", got_body, res->status, res_body);
    close_body(res);
    http_response_free(res);

    if (!no_hooks) {
        Str got = ev_string(&et.log);
        want_count(t, got,
                   fmt_sprintf_v(a, "Getting conn for dns-is-faked.golang:%s", port),
                   false);
        want_count(t, got, cs("DNS start: {Host:dns-is-faked.golang}"), false);
        want_count(t, got,
                   fmt_sprintf_v(a,
                                 "DNS done: {Addrs:[{IP:%s Zone:}] Err:<nil> "
                                 "Coalesced:false}",
                                 et.ip),
                   false);
        want_count(t, got, cs("got conn: {"), false);
        want_count(t, got, fmt_sprintf_v(a, "Connecting to tcp %s", addr_str), true);
        want_count(t, got, fmt_sprintf_v(a, "connected to tcp %s = <nil>", addr_str),
                   true);
        want_count(t, got, cs("Reused:false WasIdle:false IdleTime:0s"), false);
        want_count(t, got, cs("first response byte"), false);
        want_count(t, got, cs("PutIdleConn = <nil>"), false);
        want_count(t, got, cs("WroteHeaderField: User-Agent: [Go-http-client/1.1]"),
                   false);
        want_count(
            t, got,
            fmt_sprintf_v(a, "WroteHeaderField: Host: [dns-is-faked.golang:%s]", port),
            false);
        want_count(t, got,
                   fmt_sprintf_v(a, "WroteHeaderField: Content-Length: [%d]", body.len),
                   false);
        want_count(t, got, cs("WroteHeaderField: X-Foo-Multiple-Vals: [bar baz]"),
                   false);
        want_count(t, got, cs("WroteHeaderField: Accept-Encoding: [gzip]"), false);
        want_count(t, got, cs("WroteHeaders"), false);
        want_count(t, got, cs("Wait100Continue"), false);
        want_count(t, got, cs("Got100Continue"), false);
        want_count(t, got, cs("WroteRequest: {Err:<nil>}"), false);
        if (strings_contains(got, cs(" to udp ")))
            testing_t_errorf_v(t, "should not see UDP (DNS) connections");
        if (testing_t_failed(t))
            testing_t_errorf_v(t, "Output:\n%s", got);

        /* And do a second request: */
        IoReader none = {NULL, NULL};
        HttpRequest *req2_0 = http_new_request(a, cs("GET"), url, none, &err);
        if (req2_0 == NULL)
            FATALF("NewRequest: %v", err);
        Context tctx2 = httptrace_with_client_trace(a, ctx, &trace);
        HttpRequest *req2 = http_request_with_context(req2_0, a, tctx2);
        res = http_client_do(c, req2, &err);
        if (res == NULL)
            FATALF("%v", err);
        if (res->status_code != 200)
            FATALF("%s", res->status);
        close_body(res);
        http_response_free(res);

        got = ev_string(&et.log);
        Str sub = cs("Getting conn for dns-is-faked.golang:");
        Int gotn = strings_count(got, sub);
        if (gotn != 2)
            testing_t_errorf_v(t, "substring %q appeared %d times; want %d. Log:\n%s",
                               sub, gotn, (Int)2, got);
        context_release(tctx2);
    }
    httptest_server_free(ts);
    context_release(tctx);
    context_release(ctx);
    chan_free(et.got_wrote_req);
    arena_free(&et.log.ar);
    arena_free(&ar);
    check_handlers(t);
}

static void TestTransportEventTrace(TestingT *t) {
    transport_event_trace(t, false);
}

/* test a non-nil httptrace.ClientTrace but with all hooks set to zero. */
static void TestTransportEventTrace_NoHooks(TestingT *t) {
    transport_event_trace(t, true);
}

static void real_dns_start(void *env, HttptraceDNSStartInfo e) {
    EvLog *lg = (EvLog *)env;
    ev_unlock(lg, fmt_sprintf_v(ev_lock(lg), "DNSStart: {Host:%s}", e.host));
}

static void real_dns_done(void *env, HttptraceDNSDoneInfo e) {
    EvLog *lg = (EvLog *)env;
    Alloc *a = ev_lock(lg);
    ev_unlock(lg, dns_done_v(a, "DNSDone: ", e));
}

static void real_connect_start(void *env, Str network, Str addr) {
    EvLog *lg = (EvLog *)env;
    ev_unlock(lg, fmt_sprintf_v(ev_lock(lg), "ConnectStart: %s %s", network, addr));
}

static void real_connect_done(void *env, Str network, Str addr, Error err) {
    EvLog *lg = (EvLog *)env;
    ev_unlock(lg,
              fmt_sprintf_v(ev_lock(lg), "ConnectDone: %s %s %v", network, addr, err));
}

static void TestTransportEventTraceRealDNS(TestingT *t) {
    HttpTransport tr;
    memset(&tr, 0, sizeof tr);
    HttpClient c;
    memset(&c, 0, sizeof c);
    c.transport = http_transport_as_round_tripper(&tr);
    EvLog lg;
    ev_init(&lg);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);

    Error err = BURROW_NO_ERROR;
    IoReader none = {NULL, NULL};
    HttpRequest *req0 = http_new_request(
        a, cs("GET"), cs("http://dns-should-not-resolve.golang:80"), none, &err);
    if (req0 == NULL)
        FATALF("NewRequest: %v", err);
    HttptraceClientTrace trace = {
        .dns_start = BURROW_FN(HttptraceDNSStartFunc, real_dns_start, &lg),
        .dns_done = BURROW_FN(HttptraceDNSDoneFunc, real_dns_done, &lg),
        .connect_start = BURROW_FN(HttptraceConnectStartFunc, real_connect_start, &lg),
        .connect_done = BURROW_FN(HttptraceConnectDoneFunc, real_connect_done, &lg),
    };
    Context ctx = httptrace_with_client_trace(a, context_background(), &trace);
    HttpRequest *req = http_request_with_context(req0, a, ctx);
    HttpResponse *resp = http_client_do(&c, req, &err);
    if (resp != NULL) {
        close_body(resp);
        http_response_free(resp);
        FATALF("expected error during DNS lookup");
    }

    Str got = ev_string(&lg);
    if (!strings_contains(got, cs("DNSStart: {Host:dns-should-not-resolve.golang}")))
        testing_t_errorf_v(t, "expected substring %q in output.",
                           cs("DNSStart: {Host:dns-should-not-resolve.golang}"));
    if (!strings_contains(got, cs("DNSDone: {Addrs:[] Err:")))
        testing_t_errorf_v(t, "expected substring %q in output.",
                           cs("DNSDone: {Addrs:[] Err:"));
    if (strings_contains(got, cs("ConnectStart")) ||
        strings_contains(got, cs("ConnectDone")))
        testing_t_errorf_v(t, "should not see Connect events");
    if (testing_t_failed(t))
        testing_t_errorf_v(t, "Output:\n%s", got);
    http_transport_close_idle_connections(&tr);
    http_transport_free(&tr);
    context_release(ctx);
    arena_free(&lg.ar);
    arena_free(&ar);
}

/* ------------------------------------------------------------- 1xx responses */

static void serve_1xx_hijack(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    HttpResponseController rc = http_new_response_controller(w);
    BufioReadWriter buf;
    Error err = BURROW_NO_ERROR;
    NetConn conn = http_response_controller_hijack(&rc, &buf, &err);
    HCHECK(BURROW_OK(err));
    if (BURROW_FAILED(err))
        return;
    (void)bufio_writer_write_string(
        buf.writer,
        cs("HTTP/1.1 123 OneTwoThree\r\nFoo: bar\r\n\r\nHTTP/1.1 200 OK\r\nBar: "
           "baz\r\nContent-Length: 5\r\n\r\nHello"),
        &err);
    (void)bufio_writer_flush(buf.writer);
    (void)conn.vt->closer.close(conn.data);
    bufio_reader_free(buf.reader);
    bufio_writer_free(buf.writer);
    net_tcp_conn_free(net_conn_as_tcp_conn(conn));
}

typedef struct Got1xx {
    SyncMutex mu;
    StringsBuilder *got;
    Int n;
} Got1xx;

static Error got_1xx_log(void *env, Int code, TextprotoMIMEHeader header) {
    Got1xx *g = (Got1xx *)env;
    sync_mutex_lock(&g->mu);
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    (void)strings_builder_write_string(
        g->got,
        fmt_sprintf_v(a, "1xx: code=%d, header=%s\n", code, header_v(a, header)), NULL);
    arena_free(&ar);
    sync_mutex_unlock(&g->mu);
    return BURROW_NO_ERROR;
}

/* Issue 17739: the HTTP client must ignore any unknown 1xx informational
 * responses before the actual response. */
static void TestTransportIgnore1xxResponses(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, serve_1xx_hijack, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&hf));
    HttpClient *c = httptest_server_client(ts);
    /* prevent log spam; our test server is hanging up anyway */
    ts->transport.disable_keep_alives = true;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder got = STRINGS_BUILDER(a);
    Got1xx g;
    memset(&g, 0, sizeof g);
    g.got = &got;

    Error err = BURROW_NO_ERROR;
    IoReader none = {NULL, NULL};
    HttpRequest *req0 = http_new_request(a, cs("GET"), ts->url, none, &err);
    if (req0 == NULL)
        FATALF("NewRequest: %v", err);
    HttptraceClientTrace trace = {
        .got1xx_response = BURROW_FN(HttptraceGot1xxResponseFunc, got_1xx_log, &g),
    };
    Context ctx = httptrace_with_client_trace(a, context_background(), &trace);
    HttpRequest *req = http_request_with_context(req0, a, ctx);
    HttpResponse *res = http_client_do(c, req, &err);
    if (res == NULL) {
        FATALF("%v", err);
    }
    (void)http_response_write(res, strings_builder_as_io_writer(&got));
    close_body(res);
    http_response_free(res);
    Str want =
        cs("1xx: code=123, header=map[Foo:[bar]]\nHTTP/1.1 200 OK\r\nContent-Length: "
           "5\r\nBar: baz\r\n\r\nHello");
    Str gs = strings_builder_string(&got);
    if (!str_eq(gs, want))
        testing_t_errorf_v(t, " got: %q\nwant: %q\n", gs, want);
    httptest_server_free(ts);
    context_release(ctx);
    arena_free(&ar);
    check_handlers(t);
}

static void serve_many_1xx(void *env, HttpResponseWriter w, HttpRequest *r) {
    (void)env;
    (void)r;
    HttpHeader h = http_response_writer_header(w);
    HCHECK(http_header_add(h, cs("X-Header"),
                           strings_repeat(burrow__map_allocator(h), cs("a"), 100)));
    for (int i = 0; i < 10; i++)
        http_response_writer_write_header(w, 123);
    http_response_writer_write_header(w, 204);
}

static void TestTransportLimits1xxResponses(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, serve_many_1xx, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&hf));
    HttpClient *c = httptest_server_client(ts);
    /* prevent log spam; our test server is hanging up anyway */
    ts->transport.disable_keep_alives = true;
    ts->transport.max_response_header_bytes = 1000;

    Error err = BURROW_NO_ERROR;
    HttpResponse *res = http_client_get(c, ts->url, &err);
    if (res != NULL) {
        close_body(res);
        http_response_free(res);
        FATALF("RoundTrip succeeded; want error");
    }
    Str msg = error_text(err);
    if (!strings_contains(msg, cs("response headers exceeded")) &&
        !strings_contains(msg, cs("too many 1xx")) &&
        !strings_contains(msg, cs("header list too large")))
        testing_t_errorf_v(
            t, "got error %q; want \"response headers exceeded\" or \"too many 1xx\"",
            msg);
    httptest_server_free(ts);
    check_handlers(t);
}

static void TestTransportDoesNotLimitDelivered1xxResponses(TestingT *t) {
    need_tcp(t);
    const Int num1xx = 10;
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, serve_many_1xx, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&hf));
    HttpClient *c = httptest_server_client(ts);
    /* prevent log spam; our test server is hanging up anyway */
    ts->transport.disable_keep_alives = true;
    ts->transport.max_response_header_bytes = 1000;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    StringsBuilder log = STRINGS_BUILDER(a);
    Got1xx g;
    memset(&g, 0, sizeof g);
    g.got = &log;
    HttptraceClientTrace trace = {
        .got1xx_response = BURROW_FN(HttptraceGot1xxResponseFunc, got_1xx_log, &g),
    };
    Context ctx = httptrace_with_client_trace(a, context_background(), &trace);
    Error err = BURROW_NO_ERROR;
    IoReader none = {NULL, NULL};
    HttpRequest *req0 = http_new_request(a, cs("GET"), ts->url, none, &err);
    if (req0 == NULL)
        FATALF("NewRequest: %v", err);
    HttpRequest *req = http_request_with_context(req0, a, ctx);
    HttpResponse *res = http_client_do(c, req, &err);
    if (res == NULL)
        FATALF("%v", err);
    close_body(res);
    http_response_free(res);
    Int got1xx = strings_count(strings_builder_string(&log), cs("1xx: code=123"));
    if (got1xx != num1xx)
        testing_t_errorf_v(t, "Got %v 1xx responses, want %x", got1xx, num1xx);
    httptest_server_free(ts);
    context_release(ctx);
    arena_free(&ar);
    check_handlers(t);
}

static Error got_1xx_fail(void *env, Int code, TextprotoMIMEHeader header) {
    (void)code;
    (void)header;
    return *(const Error *)env;
}

/* An error from the hook ends the request with that error. Go has no test of
 * its own for this. */
static void TestTransportGot1xxResponseError(TestingT *t) {
    need_tcp(t);
    HttpHandlerFunc hf = BURROW_FN(HttpHandlerFunc, serve_many_1xx, NULL);
    HttptestServer *ts = httptest_new_server(NULL, http_handler_func_as_handler(&hf));
    HttpClient *c = httptest_server_client(ts);
    ts->transport.disable_keep_alives = true;

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error stop = errors_new(a, cs("no more 1xx"));
    HttptraceClientTrace trace = {
        .got1xx_response = BURROW_FN(HttptraceGot1xxResponseFunc, got_1xx_fail, &stop),
    };
    Context ctx = httptrace_with_client_trace(a, context_background(), &trace);
    Error err = BURROW_NO_ERROR;
    IoReader none = {NULL, NULL};
    HttpRequest *req0 = http_new_request(a, cs("GET"), ts->url, none, &err);
    if (req0 == NULL)
        FATALF("NewRequest: %v", err);
    HttpRequest *req = http_request_with_context(req0, a, ctx);
    HttpResponse *res = http_client_do(c, req, &err);
    if (res != NULL) {
        close_body(res);
        http_response_free(res);
        FATALF("RoundTrip succeeded; want error");
    }
    if (!errors_is(err, stop))
        testing_t_errorf_v(t, "got error %v; want %v", err, stop);
    httptest_server_free(ts);
    context_release(ctx);
    arena_free(&ar);
    check_handlers(t);
}

#define TESTS(X)                                                                       \
    X(TestWithClientTrace)                                                             \
    X(TestCompose)                                                                     \
    X(TestContextClientTrace)                                                          \
    X(TestTransportEventTrace)                                                         \
    X(TestTransportEventTrace_NoHooks)                                                 \
    X(TestTransportEventTraceRealDNS)                                                  \
    X(TestTransportIgnore1xxResponses)                                                 \
    X(TestTransportLimits1xxResponses)                                                 \
    X(TestTransportDoesNotLimitDelivered1xxResponses)                                  \
    X(TestTransportGot1xxResponseError)
TESTING_MAIN(TESTS)
