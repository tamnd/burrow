/* Multipath TCP, from Go's mptcpsock_linux_test.go.
 *
 * TestMultiPathTCP listens and dials with Multipath TCP asked for, once by
 * SetMultipathTCP with GODEBUG=multipathtcp=0 and once by GODEBUG alone with
 * multipathtcp=1, sends a few bytes both ways and checks that both ends say
 * they use it. It needs a Linux kernel that lets it make an MPTCP socket.
 * Go also asks the socket for its protocol on 5.16 and later kernels, which
 * reaches into the netFD, and this leaves that out.
 *
 * TestMultipathTCPGODEBUG has the answers Go 1.27.1 gave for each value of
 * GODEBUG, and TestMultipathTCPErrors what a nil connection gives.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/error.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/netpoll.h"
#include "burrow/os.h"
#include "burrow/pal.h"
#include "burrow/platform.h"
#include "burrow/testing.h"

#include "../src/net/internal.h"
#include "check.h"

#include <stddef.h>
#include <string.h>

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_SOCKETS 1
#endif

#define S(lit) BURROW_S(lit)

/* GODEBUG as the test found it, put back by godebug_restore. */
typedef struct Godebug {
    Str old;
    bool found;
} Godebug;

static Godebug godebug_set(TestingT *t, Str v) {
    Godebug g = {BURROW_STR_EMPTY, false};
    g.old = os_lookup_env(error_allocator(), S("GODEBUG"), &g.found);
    if (BURROW_FAILED(os_setenv(S("GODEBUG"), v)))
        testing_t_fatalf_v(t, "Setenv(GODEBUG, %q) failed", v);
    return g;
}

static void godebug_restore(Godebug g) {
    if (g.found)
        (void)os_setenv(S("GODEBUG"), g.old);
    else
        (void)os_unsetenv(S("GODEBUG"));
}

/* What Dialer.MultipathTCP and ListenConfig.MultipathTCP said with nothing
 * set and each GODEBUG value, on Linux 6.8. */
static void TestMultipathTCPGODEBUG(TestingT *t) {
#if defined(BURROW_OS_WINDOWS)
    /* The environment the net package reads is the one the process started
     * with here. */
    testing_t_skip_v(t, "GODEBUG is read once on Windows");
#else
    struct {
        const char *godebug;
        bool dial, listen;
    } tests[] = {
        {"", false, true},
        {"multipathtcp=0", false, false},
        {"multipathtcp=1", true, true},
        {"multipathtcp=2", false, true},
        {"multipathtcp=3", true, false},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Godebug g = godebug_set(t, str_from_cstr(tests[i].godebug));
        NetDialer d;
        memset(&d, 0, sizeof d);
        NetListenConfig lc;
        memset(&lc, 0, sizeof lc);
        bool dial = net_dialer_multipath_tcp(&d);
        bool listen = net_listen_config_multipath_tcp(&lc);
        if (dial != tests[i].dial)
            testing_t_errorf_v(t, "GODEBUG=%s: Dialer.MultipathTCP() = %t; want %t",
                               str_from_cstr(tests[i].godebug), dial, tests[i].dial);
        if (listen != tests[i].listen)
            testing_t_errorf_v(
                t, "GODEBUG=%s: ListenConfig.MultipathTCP() = %t; want %t",
                str_from_cstr(tests[i].godebug), listen, tests[i].listen);
        /* What was set wins over GODEBUG, both ways. */
        net_dialer_set_multipath_tcp(&d, !tests[i].dial);
        net_listen_config_set_multipath_tcp(&lc, !tests[i].listen);
        CHECK(net_dialer_multipath_tcp(&d) == !tests[i].dial);
        CHECK(net_listen_config_multipath_tcp(&lc) == !tests[i].listen);
        godebug_restore(g);
    }
    /* NULL is the zero Dialer and the zero ListenConfig. */
    Godebug g = godebug_set(t, S(""));
    CHECK(!net_dialer_multipath_tcp(NULL));
    CHECK(net_listen_config_multipath_tcp(NULL));
    godebug_restore(g);
#endif
}

static void TestMultipathTCPErrors(TestingT *t) {
    (void)t;
    Error e = BURROW_NO_ERROR;
    CHECK(!net_tcp_conn_multipath_tcp(NULL, &e));
    CHECK(errors_is(e, burrow__net_einval()));
}

/* canCreateMPTCPSocket: available is not enough, as the administrator may
 * have blocked it. */
static bool can_create_mptcp_socket(void) {
#if defined(BURROW_OS_LINUX)
    int64_t fd = pal_socket(PAL_AF_INET, PAL_SOCK_STREAM, PAL_IPPROTO_MPTCP, NULL);
    if (fd < 0)
        return false;
    (void)pal_socket_close(fd, NULL);
    return true;
#else
    return false;
#endif
}

/* The listener's side: the transponder, which sends back what it reads, and
 * then postAcceptMPTCP. */
typedef struct Server {
    NetListener l;
    SyncWaitGroup wg;
    Error err;
    const char *fail;
} Server;

static void server_job(void *env) {
    Server *s = env;
    NetTCPConn *c =
        net_tcp_listener_accept_tcp(net_listener_as_tcp_listener(s->l), &s->err);
    if (c != NULL) {
        char b[64];
        NetConn nc = net_tcp_conn_as_conn(c);
        Int n = nc.vt->reader.read(
            nc.data, slice_from(b, sizeof b, sizeof b, TYPE_BYTE), &s->err);
        if (n > 0 && BURROW_OK(s->err))
            (void)nc.vt->writer.write(nc.data, slice_from(b, n, n, TYPE_BYTE), &s->err);
        if (BURROW_OK(s->err) && !net_tcp_conn_multipath_tcp(c, &s->err) &&
            BURROW_OK(s->err))
            s->fail = "incoming connection is not with MPTCP";
        net_tcp_conn_free(c);
    }
    sync_wait_group_done(&s->wg);
}

static void dialer_mptcp(TestingT *t, Str addr, bool env_var) {
    NetDialer d;
    memset(&d, 0, sizeof d);
    if (env_var) {
        if (!net_dialer_multipath_tcp(&d)) {
            testing_t_error_v(t, S("MultipathTCP Dialer is not on despite "
                                   "GODEBUG=multipathtcp=1"));
            return;
        }
    } else {
        if (net_dialer_multipath_tcp(&d))
            testing_t_error_v(t, S("MultipathTCP should be off by default"));
        net_dialer_set_multipath_tcp(&d, true);
        if (!net_dialer_multipath_tcp(&d)) {
            testing_t_error_v(t, S("MultipathTCP is not on after having been forced "
                                   "to on"));
            return;
        }
    }
    Error e = BURROW_NO_ERROR;
    NetConn c = net_dialer_dial(&d, heap_allocator(), S("tcp"), addr, &e);
    if (c.vt == NULL) {
        testing_t_error_v(t, e);
        return;
    }
    /* Transfer a bit of data to make sure everything is still OK. */
    static const char snt[] = "MPTCP TEST";
    Int n = (Int)sizeof snt - 1;
    char b[sizeof snt - 1];
    (void)c.vt->writer.write(c.data,
                             slice_from((void *)(uintptr_t)snt, n, n, TYPE_BYTE), &e);
    if (BURROW_OK(e))
        (void)io_read_full(net_conn_as_io_reader(c), slice_from(b, n, n, TYPE_BYTE),
                           &e);
    if (BURROW_FAILED(e))
        testing_t_error_v(t, e);
    else if (memcmp(snt, b, sizeof b) != 0)
        testing_t_error_v(t, S("sent bytes are different from received ones"));
    NetTCPConn *tc = net_conn_as_tcp_conn(c);
    bool mptcp = net_tcp_conn_multipath_tcp(tc, &e);
    if (BURROW_FAILED(e))
        testing_t_error_v(t, e);
    else if (!mptcp)
        testing_t_error_v(t, S("outgoing connection is not with MPTCP"));
    net_conn_free(c);
}

static void test_multipath_tcp(TestingT *t, bool env_var) {
    Godebug g = godebug_set(t, env_var ? S("multipathtcp=1") : S("multipathtcp=0"));
    NetListenConfig lc;
    memset(&lc, 0, sizeof lc);
    if (env_var) {
        if (!net_listen_config_multipath_tcp(&lc)) {
            testing_t_error_v(t, S("MultipathTCP Listen is not on despite "
                                   "GODEBUG=multipathtcp=1"));
            godebug_restore(g);
            return;
        }
    } else {
        if (net_listen_config_multipath_tcp(&lc))
            testing_t_error_v(t, S("MultipathTCP should be off by default"));
        net_listen_config_set_multipath_tcp(&lc, true);
    }
    Server s;
    memset(&s, 0, sizeof s);
    Error e = BURROW_NO_ERROR;
    s.l = net_listen_config_listen(&lc, heap_allocator(), context_background(),
                                   S("tcp"), S("127.0.0.1:0"), &e);
    if (s.l.vt == NULL) {
        testing_t_error_v(t, e);
        godebug_restore(g);
        return;
    }
    NetAddr la = s.l.vt->addr(s.l.data);
    if (!str_eq(la.vt->network(la.data), S("tcp")))
        testing_t_error_v(t, S("Network type mismatch: got "), la.vt->network(la.data),
                          S(", want tcp"));
    sync_wait_group_add(&s.wg, 1);
    if (!go(BURROW_FN(Func, server_job, &s))) {
        sync_wait_group_done(&s.wg);
        testing_t_error_v(t, S("go: out of memory"));
    } else {
        dialer_mptcp(t, la.vt->string(la.data, heap_allocator()), env_var);
        (void)s.l.vt->closer.close(s.l.data);
        sync_wait_group_wait(&s.wg);
        if (BURROW_FAILED(s.err))
            testing_t_error_v(t, s.err);
        if (s.fail != NULL)
            testing_t_error_v(t, str_from_cstr(s.fail));
    }
    net_listener_free(s.l);
    godebug_restore(g);
}

static void TestMultiPathTCP(TestingT *t) {
#if defined(BURROW_OS_LINUX) && defined(HAVE_SOCKETS)
    if (!can_create_mptcp_socket()) {
        testing_t_skip_v(t, "Cannot create MPTCP sockets");
    } else {
        test_multipath_tcp(t, false);
        test_multipath_tcp(t, true);
    }
#else
    (void)test_multipath_tcp;
    (void)can_create_mptcp_socket;
    testing_t_skip_v(t, "Multipath TCP is Linux only");
#endif
}

#define TESTS(X)                                                                       \
    X(TestMultipathTCPGODEBUG)                                                         \
    X(TestMultipathTCPErrors)                                                          \
    X(TestMultiPathTCP)

TESTING_MAIN(TESTS)
