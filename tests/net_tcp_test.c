/* TCPConn and TCPListener over loopback, and TCPAddr's text.
 *
 * The cases follow Go's src/net/tcpsock_test.go, net_test.go and
 * error_test.go where they can run without the resolver, which is still to
 * come, so every address here is a literal.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/netpoll.h"
#include "burrow/os.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include "check.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* What EINVAL says. WASI's table has it capitalised, and so does Go's
 * tables_wasip1.go. */
#if defined(BURROW_OS_WASI)
#define EINVAL_TEXT "Invalid argument"
#else
#define EINVAL_TEXT "invalid argument"
#endif

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

static Byte loop4_bytes[4] = {127, 0, 0, 1};
static Byte loop6_bytes[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

static NetTCPAddr loop4(Int port) {
    NetTCPAddr a = {slice_from(loop4_bytes, 4, 4, TYPE_BYTE), port, BURROW_STR_EMPTY};
    return a;
}

static NetTCPAddr loop6(Int port) {
    NetTCPAddr a = {slice_from(loop6_bytes, 16, 16, TYPE_BYTE), port, BURROW_STR_EMPTY};
    return a;
}

static Slice bytes_of(char *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

/* The text of err, as a C string in buf. */
static const char *text_of(Error err, char *buf, size_t n) {
    Str s = error_text(err);
    size_t len = (size_t)s.len < n - 1 ? (size_t)s.len : n - 1;
    memcpy(buf, s.p, len);
    buf[len] = '\0';
    return buf;
}

/* The text of an address, as a C string in buf. */
static const char *addr_text(NetAddr a, char *buf, size_t n) {
    if (a.vt == NULL) {
        memcpy(buf, "<nil>", 6);
        return buf;
    }
    Str s = a.vt->string(a.data, heap_allocator());
    size_t len = (size_t)s.len < n - 1 ? (size_t)s.len : n - 1;
    memcpy(buf, s.p, len);
    buf[len] = '\0';
    mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    return buf;
}

static Int port_of(NetAddr a) {
    return a.data == NULL ? -1 : ((const NetTCPAddr *)a.data)->port;
}

static NetTCPListener *listen_loop4(TestingT *t) {
    NetTCPAddr la = loop4(0);
    Error e = BURROW_NO_ERROR;
    NetTCPListener *l = net_listen_tcp(heap_allocator(), BURROW_S("tcp"), &la, &e);
    if (l == NULL)
        testing_t_errorf_v(t, "listen: %v", e);
    return l;
}

/* ------------------------------------------------------------- the echo */

/* The errors the jobs below hand back are made on their own goroutines, whose
 * error arenas go when they end, so they keep them in an arena of the test's. */
typedef struct EchoJob {
    NetTCPListener *l;
    Error err;
    Arena keep;
    char remote[64];
} EchoJob;

/* Accepts one connection and writes back what it reads until the end. */
static void echo_job(void *env) {
    EchoJob *j = env;
    Error ae = BURROW_NO_ERROR;
    NetTCPConn *c = net_tcp_listener_accept_tcp(j->l, &ae);
    if (c == NULL) {
        j->err = error_retain(arena_allocator(&j->keep), ae);
        return;
    }
    (void)addr_text(net_tcp_conn_remote_addr(c), j->remote, sizeof j->remote);
    char buf[256];
    for (;;) {
        Error e = BURROW_NO_ERROR;
        Int n = net_tcp_conn_read(c, bytes_of(buf, (Int)sizeof buf), &e);
        if (n > 0)
            (void)net_tcp_conn_write(c, bytes_of(buf, n), NULL);
        if (BURROW_FAILED(e)) {
            if (!errors_is(e, io_eof))
                j->err = error_retain(arena_allocator(&j->keep), e);
            break;
        }
    }
    net_tcp_conn_free(c);
}

static void TestADialedConnectionTalksToTheOneAccepted(TestingT *t) {
    need_tcp(t);
    NetTCPListener *l = listen_loop4(t);
    if (l == NULL)
        return;
    Int port = port_of(net_tcp_listener_addr(l));
    CHECK(port > 0);
    EchoJob j = {.l = l};
    arena_init(&j.keep, NULL, 0);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, echo_job, &j));

    NetTCPAddr ra = loop4(port);
    Error e = BURROW_NO_ERROR;
    NetTCPConn *c = net_dial_tcp(heap_allocator(), BURROW_S("tcp"), NULL, &ra, &e);
    if (c == NULL) {
        testing_t_fatalf_v(t, "dial: %v", e);
        return;
    }
    char hi[] = "hello, world";
    CHECK_INT_EQ(net_tcp_conn_write(c, bytes_of(hi, 12), &e), 12);
    CHECK(!BURROW_FAILED(e));
    char buf[32] = {0};
    Int got = 0;
    while (got < 12 && !BURROW_FAILED(e))
        got += net_tcp_conn_read(c, bytes_of(buf + got, 32 - got), &e);
    CHECK_INT_EQ(got, 12);
    CHECK(memcmp(buf, hi, 12) == 0);

    /* Each end's address is the other's remote one. */
    char want[64];
    char text[64];
    snprintf(want, sizeof want, "127.0.0.1:%d", (int)port);
    CHECK_STR_EQ(addr_text(net_tcp_conn_remote_addr(c), text, sizeof text), want);
    CHECK_STR_EQ(addr_text(net_tcp_listener_addr(l), text, sizeof text), want);
    NetAddr cl = net_tcp_conn_local_addr(c);
    CHECK(cl.vt != NULL && str_eq(cl.vt->network(cl.data), BURROW_S("tcp")));

    CHECK(!BURROW_FAILED(net_tcp_conn_close_write(c)));
    sync_wait_group_wait(&wg);
    CHECK(!BURROW_FAILED(j.err));
    CHECK_STR_EQ(j.remote, addr_text(net_tcp_conn_local_addr(c), text, sizeof text));

    /* After the echo has gone, what is left to read is the end. */
    CHECK_INT_EQ(net_tcp_conn_read(c, bytes_of(buf, 32), &e), 0);
    CHECK(errors_is(e, io_eof));
    CHECK(!BURROW_FAILED(net_tcp_conn_close(c)));
    net_tcp_conn_free(c);
    CHECK(!BURROW_FAILED(net_tcp_listener_close(l)));
    net_tcp_listener_free(l);
    arena_free(&j.keep);
}

static void TestAConnectionCanBeUsedAsANetConn(TestingT *t) {
    need_tcp(t);
    NetTCPListener *l = listen_loop4(t);
    if (l == NULL)
        return;
    NetListener nl = net_tcp_listener_as_listener(l);
    NetTCPAddr ra = loop4(port_of(nl.vt->addr(nl.data)));
    Error e = BURROW_NO_ERROR;
    NetTCPConn *c = net_dial_tcp(heap_allocator(), BURROW_S("tcp4"), NULL, &ra, &e);
    if (c == NULL) {
        testing_t_fatalf_v(t, "dial: %v", e);
        return;
    }
    NetConn s = nl.vt->accept(nl.data, &e);
    if (s.vt == NULL) {
        testing_t_fatalf_v(t, "accept: %v", e);
        return;
    }
    NetConn cc = net_tcp_conn_as_conn(c);
    CHECK(net_conn_as_tcp_conn(cc) == c);
    char hi[] = "hi";
    CHECK_INT_EQ(cc.vt->writer.write(cc.data, bytes_of(hi, 2), &e), 2);
    char buf[4] = {0};
    CHECK_INT_EQ(io_read_full(net_conn_as_io_reader(s), bytes_of(buf, 2), &e), 2);
    CHECK_STR_EQ(buf, "hi");
    net_tcp_conn_free(net_conn_as_tcp_conn(s));
    net_tcp_conn_free(c);
    net_tcp_listener_free(l);
}

/* ---------------------------------------------------------------- errors */

static void TestADialToAClosedPortIsRefused(TestingT *t) {
    need_tcp(t);
    NetTCPListener *l = listen_loop4(t);
    if (l == NULL)
        return;
    Int port = port_of(net_tcp_listener_addr(l));
    net_tcp_listener_free(l);

    NetTCPAddr ra = loop4(port);
    Error e = BURROW_NO_ERROR;
    NetTCPConn *c = net_dial_tcp(heap_allocator(), BURROW_S("tcp"), NULL, &ra, &e);
    CHECK(c == NULL);
    char want[96];
    char buf[96];
    snprintf(want, sizeof want, "dial tcp 127.0.0.1:%d: connect: connection refused",
             (int)port);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), want);
    CHECK(errors_as(e, TYPE_NET_OP_ERROR) != NULL);
    CHECK(errors_as(e, TYPE_OS_SYSCALL_ERROR) != NULL);
}

static void TestAnUnknownNetworkIsSaidSo(TestingT *t) {
    NetTCPAddr ra = loop4(80);
    Error e = BURROW_NO_ERROR;
    char buf[96];
    CHECK(net_dial_tcp(heap_allocator(), BURROW_S("tcp5"), NULL, &ra, &e) == NULL);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf),
                 "dial tcp5 127.0.0.1:80: unknown network tcp5");
    const Str *name = errors_as(e, TYPE_NET_UNKNOWN_NETWORK_ERROR);
    CHECK(name != NULL && str_eq(*name, BURROW_S("tcp5")));
    CHECK(errors_is(e, net_unknown_network_error(error_allocator(), BURROW_S("tcp5"))));

    CHECK(net_listen_tcp(heap_allocator(), BURROW_S("udp"), NULL, &e) == NULL);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), "listen udp: unknown network udp");
}

static void TestADialWithNoAddressIsMissingOne(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    char buf[96];
    CHECK(net_dial_tcp(heap_allocator(), BURROW_S("tcp"), NULL, NULL, &e) == NULL);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), "dial tcp: missing address");
}

static void TestAnAddressTheFamilyCannotHoldIsAnAddrError(TestingT *t) {
    need_tcp(t);
    NetTCPAddr ra = loop6(80);
    Error e = BURROW_NO_ERROR;
    char buf[96];
    CHECK(net_dial_tcp(heap_allocator(), BURROW_S("tcp4"), NULL, &ra, &e) == NULL);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf),
                 "dial tcp4 [::1]:80: address ::1: non-IPv4 address");
    CHECK(errors_as(e, TYPE_NET_ADDR_ERROR) != NULL);
}

static void TestAPortPastTheLastIsAnInvalidArgument(TestingT *t) {
    need_tcp(t);
    NetTCPAddr ra = loop4(70000);
    Error e = BURROW_NO_ERROR;
    char buf[96];
    CHECK(net_dial_tcp(heap_allocator(), BURROW_S("tcp"), NULL, &ra, &e) == NULL);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf),
                 "dial tcp 127.0.0.1:70000: connect: invalid argument");

    NetTCPAddr la = loop4(-1);
    CHECK(net_listen_tcp(heap_allocator(), BURROW_S("tcp"), &la, &e) == NULL);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf),
                 "listen tcp 127.0.0.1:-1: bind: invalid argument");
}

static void TestANilConnIsAnInvalidArgument(TestingT *t) {
    char buf[64];
    CHECK_STR_EQ(text_of(net_tcp_conn_close(NULL), buf, sizeof buf), EINVAL_TEXT);
    CHECK_STR_EQ(text_of(net_tcp_listener_close(NULL), buf, sizeof buf), EINVAL_TEXT);
    CHECK(net_tcp_conn_local_addr(NULL).vt == NULL);
    net_tcp_conn_free(NULL);
    net_tcp_listener_free(NULL);
}

/* ------------------------------------------------------- closes, deadlines */

typedef struct AcceptJob {
    NetTCPListener *l;
    NetTCPConn *c;
    Error err;
    Arena keep;
} AcceptJob;

static void accept_job(void *env) {
    AcceptJob *j = env;
    Error e = BURROW_NO_ERROR;
    j->c = net_tcp_listener_accept_tcp(j->l, &e);
    j->err = error_retain(arena_allocator(&j->keep), e);
}

static void TestACloseWakesAnAccept(TestingT *t) {
    need_tcp(t);
    NetTCPListener *l = listen_loop4(t);
    if (l == NULL)
        return;
    Int port = port_of(net_tcp_listener_addr(l));
    AcceptJob j = {.l = l};
    arena_init(&j.keep, NULL, 0);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, accept_job, &j));
    time_sleep(20 * TIME_MILLISECOND);
    CHECK(!BURROW_FAILED(net_tcp_listener_close(l)));
    sync_wait_group_wait(&wg);
    CHECK(j.c == NULL);
    CHECK(errors_is(j.err, net_err_closed));
    char want[96];
    char buf[96];
    snprintf(want, sizeof want,
             "accept tcp 127.0.0.1:%d: use of closed network connection", (int)port);
    CHECK_STR_EQ(text_of(j.err, buf, sizeof buf), want);

    /* A second close says the first one happened. */
    snprintf(want, sizeof want,
             "close tcp 127.0.0.1:%d: use of closed network connection", (int)port);
    CHECK_STR_EQ(text_of(net_tcp_listener_close(l), buf, sizeof buf), want);
    net_tcp_listener_free(l);
    arena_free(&j.keep);
}

static void TestAReadPastItsDeadlineTimesOut(TestingT *t) {
    need_tcp(t);
    NetTCPListener *l = listen_loop4(t);
    if (l == NULL)
        return;
    NetTCPAddr ra = loop4(port_of(net_tcp_listener_addr(l)));
    Error e = BURROW_NO_ERROR;
    NetTCPConn *c = net_dial_tcp(heap_allocator(), BURROW_S("tcp"), NULL, &ra, &e);
    NetTCPConn *s = net_tcp_listener_accept_tcp(l, &e);
    if (c == NULL || s == NULL) {
        testing_t_fatalf_v(t, "dial or accept: %v", e);
        return;
    }
    CHECK(!BURROW_FAILED(net_tcp_conn_set_read_deadline(
        c, time_add(time_now(), 30 * TIME_MILLISECOND))));
    char buf[8];
    CHECK_INT_EQ(net_tcp_conn_read(c, bytes_of(buf, 8), &e), 0);
    CHECK(errors_is(e, os_err_deadline_exceeded));
    CHECK(net_error_timeout(e));
    CHECK(errors_as(e, TYPE_NET_OP_ERROR) != NULL);

    /* A cleared deadline lets the next read wait for the bytes. */
    CHECK(!BURROW_FAILED(net_tcp_conn_set_read_deadline(c, (Time){0})));
    char hi[] = "x";
    CHECK_INT_EQ(net_tcp_conn_write(s, bytes_of(hi, 1), &e), 1);
    CHECK_INT_EQ(net_tcp_conn_read(c, bytes_of(buf, 8), &e), 1);
    CHECK(!BURROW_FAILED(e));
    net_tcp_conn_free(s);
    net_tcp_conn_free(c);
    net_tcp_listener_free(l);
}

static void TestTheSocketOptionsCanBeSet(TestingT *t) {
    need_tcp(t);
    NetTCPListener *l = listen_loop4(t);
    if (l == NULL)
        return;
    NetTCPAddr ra = loop4(port_of(net_tcp_listener_addr(l)));
    Error e = BURROW_NO_ERROR;
    NetTCPConn *c = net_dial_tcp(heap_allocator(), BURROW_S("tcp"), NULL, &ra, &e);
    if (c == NULL) {
        testing_t_fatalf_v(t, "dial: %v", e);
        return;
    }
    CHECK(!BURROW_FAILED(net_tcp_conn_set_no_delay(c, false)));
    CHECK(!BURROW_FAILED(net_tcp_conn_set_keep_alive(c, true)));
    CHECK(!BURROW_FAILED(net_tcp_conn_set_linger(c, 0)));
    CHECK(!BURROW_FAILED(net_tcp_conn_set_linger(c, -1)));
    CHECK(!BURROW_FAILED(net_tcp_conn_set_read_buffer(c, 1 << 16)));
    CHECK(!BURROW_FAILED(net_tcp_conn_set_write_buffer(c, 1 << 16)));
#if !defined(BURROW_OS_OPENBSD)
    CHECK(!BURROW_FAILED(net_tcp_conn_set_keep_alive_period(c, 30 * TIME_SECOND)));
    NetKeepAliveConfig cfg = {true, 20 * TIME_SECOND, 5 * TIME_SECOND, 3};
    CHECK(!BURROW_FAILED(net_tcp_conn_set_keep_alive_config(c, cfg)));
#endif
    net_tcp_conn_free(c);
    net_tcp_listener_free(l);
}

/* ---------------------------------------------------------------- IPv6 */

static void TestIPv6LoopbackWorksWhereThereIsOne(TestingT *t) {
    need_tcp(t);
    NetTCPAddr la = loop6(0);
    Error e = BURROW_NO_ERROR;
    NetTCPListener *l = net_listen_tcp(heap_allocator(), BURROW_S("tcp6"), &la, &e);
    if (l == NULL) {
        testing_t_skip_v(t, "no IPv6 loopback here: %v", e);
        return;
    }
    Int port = port_of(net_tcp_listener_addr(l));
    NetTCPAddr ra = loop6(port);
    NetTCPConn *c = net_dial_tcp(heap_allocator(), BURROW_S("tcp6"), NULL, &ra, &e);
    if (c == NULL) {
        testing_t_fatalf_v(t, "dial: %v", e);
        return;
    }
    NetTCPConn *s = net_tcp_listener_accept_tcp(l, &e);
    CHECK(s != NULL);
    char want[64];
    char text[64];
    snprintf(want, sizeof want, "[::1]:%d", (int)port);
    CHECK_STR_EQ(addr_text(net_tcp_conn_remote_addr(c), text, sizeof text), want);
    net_tcp_conn_free(s);
    net_tcp_conn_free(c);
    net_tcp_listener_free(l);
}

/* ------------------------------------------------------------- TCPAddr */

static void TestATCPAddrPrintsTheWayGoPrintsIt(TestingT *t) {
    (void)t;
    Alloc *a = heap_allocator();
    char buf[64];
    struct {
        NetTCPAddr addr;
        const char *want;
    } cases[] = {
        {loop4(80), "127.0.0.1:80"},
        {loop6(443), "[::1]:443"},
        {{slice_from(loop6_bytes, 16, 16, TYPE_BYTE), 5, BURROW_S("eth0")},
         "[::1%eth0]:5"},
        {{{0}, 0, BURROW_STR_EMPTY}, ":0"},
        {{{0}, 8080, BURROW_STR_EMPTY}, ":8080"},
        {{slice_from(loop4_bytes, 3, 3, TYPE_BYTE), 1, BURROW_STR_EMPTY}, "?7f0000:1"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Str s = net_tcp_addr_string(&cases[i].addr, a);
        size_t n = (size_t)s.len < sizeof buf - 1 ? (size_t)s.len : sizeof buf - 1;
        memcpy(buf, s.p, n);
        buf[n] = '\0';
        CHECK_STR_EQ(buf, cases[i].want);
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    }
    Str s = net_tcp_addr_string(NULL, a);
    CHECK(str_eq(s, BURROW_S("<nil>")));
    mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    CHECK(str_eq(net_tcp_addr_network(NULL), BURROW_S("tcp")));
    CHECK(net_tcp_addr_as_addr(NULL).vt == NULL);
}

#define TESTS(X)                                                                       \
    X(TestADialedConnectionTalksToTheOneAccepted)                                      \
    X(TestAConnectionCanBeUsedAsANetConn)                                              \
    X(TestADialToAClosedPortIsRefused)                                                 \
    X(TestAnUnknownNetworkIsSaidSo)                                                    \
    X(TestADialWithNoAddressIsMissingOne)                                              \
    X(TestAnAddressTheFamilyCannotHoldIsAnAddrError)                                   \
    X(TestAPortPastTheLastIsAnInvalidArgument)                                         \
    X(TestANilConnIsAnInvalidArgument)                                                 \
    X(TestACloseWakesAnAccept)                                                         \
    X(TestAReadPastItsDeadlineTimesOut)                                                \
    X(TestTheSocketOptionsCanBeSet)                                                    \
    X(TestIPv6LoopbackWorksWhereThereIsOne)                                            \
    X(TestATCPAddrPrintsTheWayGoPrintsIt)

TESTING_MAIN(TESTS)
