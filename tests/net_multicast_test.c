/* Multicast listeners, from Go's listen_test.go.
 *
 * TestIPv4MulticastListener and TestIPv6MulticastListener join groups on the
 * loopback interface, twice each, and look for the group in the interface's
 * list of joined groups, which is how Go checks that the join happened. Go
 * also tries the system's choice of interface, but only when told the machine
 * has outside connectivity, and these leave that out as Go does by default.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/error.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/platform.h"
#include "burrow/testing.h"

#include "../src/net/internal.h"
#include "check.h"

#include <stddef.h>
#include <string.h>

#define S(lit) BURROW_S(lit)

static Arena ar;
static Alloc *a;

/* loopbackInterface: the first loopback interface that is up, or NULL. */
static const NetInterface *loopback_interface(void) {
    Error err = BURROW_NO_ERROR;
    Slice ift = net_interfaces(a, &err);
    if (BURROW_FAILED(err))
        return NULL;
    const NetInterface *is = (const NetInterface *)ift.p;
    for (Int i = 0; i < ift.len; i++)
        if ((is[i].flags & NET_FLAG_LOOPBACK) != 0 && (is[i].flags & NET_FLAG_UP) != 0)
            return &is[i];
    return NULL;
}

/* multicastRIBContains: whether some interface has joined ip. */
static bool multicast_rib_contains(TestingT *t, NetIP ip) {
#if defined(BURROW_OS_AIX) || defined(BURROW_OS_DRAGONFLY) ||                          \
    defined(BURROW_OS_NETBSD) || defined(BURROW_OS_OPENBSD) ||                         \
    defined(BURROW_OS_SOLARIS)
    (void)t;
    (void)ip;
    return true; /* Go does not look on these either */
#else
    Error err = BURROW_NO_ERROR;
    Slice ift = net_interfaces(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_error_v(t, err);
        return false;
    }
    const NetInterface *is = (const NetInterface *)ift.p;
    for (Int i = 0; i < ift.len; i++) {
        Slice ifmat = net_interface_multicast_addrs(&is[i], a, &err);
        if (BURROW_FAILED(err)) {
            testing_t_error_v(t, err);
            return false;
        }
        const NetAddr *as = (const NetAddr *)ifmat.p;
        for (Int j = 0; j < ifmat.len; j++)
            if (net_ip_equal(((const NetIPAddr *)as[j].data)->ip, ip))
                return true;
    }
    testing_t_error_v(t, net_ip_string(ip, a), S(" not found in multicast rib"));
    return false;
#endif
}

/* checkMulticastListener. */
static bool check_multicast_listener(TestingT *t, NetUDPConn *c, NetIP ip) {
    if (!multicast_rib_contains(t, ip))
        return false;
    NetAddr la = net_udp_conn_local_addr(c);
    if (la.vt == NULL || la.vt->self_type != TYPE_NET_UDP_ADDR ||
        ((const NetUDPAddr *)la.data)->port == 0) {
        Str got = la.vt != NULL ? la.vt->string(la.data, a) : S("<nil>");
        testing_t_error_v(t, S("got "), got,
                          S("; want a proper address with non-zero port number"));
        return false;
    }
    return true;
}

/* Both listeners of a test, which the same group and port take twice. */
static void listen_twice(TestingT *t, Str network, const NetInterface *ifi,
                         const NetUDPAddr *gaddr) {
    NetUDPConn *cs[2] = {NULL, NULL};
    for (int i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        cs[i] = net_listen_multicast_udp(heap_allocator(), network, ifi, gaddr, &err);
        if (BURROW_FAILED(err)) {
            testing_t_error_v(t, err);
            break;
        }
        if (!check_multicast_listener(t, cs[i], gaddr->ip))
            break;
    }
    net_udp_conn_free(cs[0]);
    net_udp_conn_free(cs[1]);
}

static void TestIPv4MulticastListener(TestingT *t) {
    if (testing_short()) {
        testing_t_skip_v(t, "skipping test: no external network in -short mode");
        return;
    }
    if (!burrow__net_supports_ipv4()) {
        testing_t_skip_v(t, "IPv4 is not supported");
        return;
    }
    const NetInterface *ifi = loopback_interface();
    if (ifi == NULL)
        return;
    /* 224.0.0.254 is RFC 4727's group for experiments. */
    NetUDPAddr gaddr = {net_ipv4(a, 224, 0, 0, 254), 12345, BURROW_STR_EMPTY};
    static const char *const nets[] = {"udp", "udp4"};
    for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++) {
        listen_twice(t, str_from_cstr(nets[i]), ifi, &gaddr);
        if (testing_t_failed(t))
            return;
    }
}

static void TestIPv6MulticastListener(TestingT *t) {
    if (testing_short()) {
        testing_t_skip_v(t, "skipping test: no external network in -short mode");
        return;
    }
    if (!burrow__net_supports_ipv6()) {
        testing_t_skip_v(t, "IPv6 is not supported");
        return;
    }
#if !defined(BURROW_OS_WINDOWS)
    if (os_getuid() != 0) {
        testing_t_skip_v(t, "must be root");
        return;
    }
#endif
    const NetInterface *ifi = loopback_interface();
    if (ifi == NULL)
        return;
    static const char *const nets[] = {"udp", "udp6"};
    static const char *const groups[] = {"ff01::114", "ff02::114", "ff04::114",
                                         "ff05::114", "ff08::114", "ff0e::114"};
    for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++) {
        for (size_t j = 0; j < sizeof groups / sizeof groups[0]; j++) {
            NetUDPAddr gaddr = {net_parse_ip(a, str_from_cstr(groups[j])), 12345,
                                BURROW_STR_EMPTY};
            listen_twice(t, str_from_cstr(nets[i]), ifi, &gaddr);
            if (testing_t_failed(t))
                return;
        }
    }
}

/* The errors ListenMulticastUDP gives before it makes a socket. */
static void TestListenMulticastUDPErrors(TestingT *t) {
    NetUDPAddr nil_ip = {slice_nil(TYPE_BYTE), 0, BURROW_STR_EMPTY};
    NetUDPAddr group = {net_ipv4(a, 224, 0, 0, 254), 12345, BURROW_STR_EMPTY};
    struct {
        const char *network;
        const NetUDPAddr *gaddr;
        const char *want;
    } tests[] = {
        {"tcp", &group, "listen tcp 224.0.0.254:12345: unknown network tcp"},
        {"ip4", NULL, "listen ip4: unknown network ip4"},
        {"udp", NULL, "listen udp: missing address"},
        {"udp4", &nil_ip, "listen udp4 :0: missing address"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error err = BURROW_NO_ERROR;
        NetUDPConn *c =
            net_listen_multicast_udp(heap_allocator(), str_from_cstr(tests[i].network),
                                     NULL, tests[i].gaddr, &err);
        if (c != NULL) {
            testing_t_error_v(t, S("ListenMulticastUDP("),
                              str_from_cstr(tests[i].network), S(") made a socket"));
            net_udp_conn_free(c);
            continue;
        }
        if (!str_eq(error_text(err), str_from_cstr(tests[i].want)))
            testing_t_error_v(t, S("got "), error_text(err), S("; want "),
                              str_from_cstr(tests[i].want));
    }
}

#define TESTS(X)                                                                       \
    X(TestIPv4MulticastListener)                                                       \
    X(TestIPv6MulticastListener)                                                       \
    X(TestListenMulticastUDPErrors)

static int net_multicast_main(TestingM *m) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    int code = testing_m_run(m);
    arena_free(&ar);
    return code;
}

TESTING_MAIN_WITH(net_multicast_main, TESTS)
