/* IPConn.
 *
 * The cases follow Go's src/net/iprawsock_test.go. Raw sockets need root,
 * which is Go's testableNetwork rule for the "ip" networks, so most of these
 * skip without it. The error texts are the ones Go 1.27.1 gives on Linux.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/netpoll.h"
#include "burrow/os.h"
#include "burrow/time.h"

#include "check.h"

#include <stdint.h>
#include <string.h>

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_SOCKETS 1
#endif

#define S(lit) BURROW_S(lit)

static Arena ar;
static Alloc *a;

static void need_sockets(TestingT *t) {
#if !defined(HAVE_SOCKETS)
    testing_t_skip_v(t, "sockets here need the readiness poll FD");
#else
    (void)t;
#endif
}

/* testableNetwork for "ip", "ip4" and "ip6": only root may have a raw
 * socket. */
static bool privileged(void) {
#if !defined(HAVE_SOCKETS) || defined(BURROW_OS_WINDOWS)
    return false;
#else
    return os_getuid() == 0;
#endif
}

static void need_root(TestingT *t) {
    need_sockets(t);
    if (!privileged())
        testing_t_skip_v(t, "raw sockets need root");
}

static const char *c_text(Str s, char *buf, size_t n) {
    size_t m = (size_t)s.len < n - 1 ? (size_t)s.len : n - 1;
    if (m > 0)
        memcpy(buf, s.p, m);
    buf[m] = '\0';
    return buf;
}

static Str addr_str(NetAddr addr) {
    if (addr.vt == NULL)
        return S("<nil>");
    return addr.vt->string(addr.data, a);
}

static NetIPAddr ip4(Byte a0, Byte a1, Byte a2, Byte a3) {
    NetIPAddr r = {net_ipv4(a, a0, a1, a2, a3), BURROW_STR_EMPTY};
    return r;
}

/* The errors that come before any socket is made, which need no
 * privileges. */
static void TestIPConnErrors(TestingT *t) {
    need_sockets(t);
    char buf[128];
    Error e = BURROW_NO_ERROR;
    NetIPConn *c = net_listen_ip(heap_allocator(), S("ip4"), NULL, &e);
    CHECK(c == NULL);
    CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf),
                 "listen ip4 : unknown network ip4");
    c = net_dial_ip(heap_allocator(), S("ip4:icmp"), NULL, NULL, &e);
    CHECK(c == NULL);
    CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf),
                 "dial ip4:icmp: missing address");
    NetIPAddr lo = ip4(127, 0, 0, 1);
    c = net_dial_ip(heap_allocator(), S("ip4:bogusproto"), NULL, &lo, &e);
    CHECK(c == NULL);
    CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf),
                 "dial ip4:bogusproto 127.0.0.1: address bogusproto: unknown IP "
                 "protocol specified");
    Slice p = slice_from(buf, 1, 1, TYPE_BYTE);
    CHECK(net_ip_conn_write(NULL, p, &e) == 0 && BURROW_FAILED(e));
    CHECK(net_ip_conn_write_to_ip(NULL, p, &lo, &e) == 0 && BURROW_FAILED(e));
    CHECK(BURROW_FAILED(net_ip_conn_close(NULL)));
    CHECK(net_ip_conn_local_addr(NULL).vt == NULL);
    CHECK(net_conn_as_ip_conn(net_ip_conn_as_conn(NULL)) == NULL);
    net_ip_conn_free(NULL);
}

/* Dialer.DialIP, whose addresses become IPAddrs even when they are the zero
 * Addr, so an error names an empty source. The texts are Go's. */
static void TestDialerDialIP(TestingT *t) {
    need_sockets(t);
    char buf[160];
    NetipAddr lo = netip_must_parse_addr(S("127.0.0.1"));
    NetipAddr none = {0};
    struct {
        const char *network;
        NetipAddr laddr;
        const char *want;
    } tests[] = {
        {"ip4:bogusproto", none,
         "dial ip4:bogusproto ->127.0.0.1: address bogusproto: unknown IP protocol "
         "specified"},
        {"ip4:bogusproto", lo,
         "dial ip4:bogusproto 127.0.0.1->127.0.0.1: address bogusproto: unknown IP "
         "protocol specified"},
        {"tcp", none, "dial tcp ->127.0.0.1: unknown network tcp"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Error e = BURROW_NO_ERROR;
        NetIPConn *c =
            net_dialer_dial_ip(NULL, heap_allocator(), context_background(),
                               str_from_cstr(tests[i].network), tests[i].laddr, lo, &e);
        CHECK(c == NULL);
        net_ip_conn_free(c);
        CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf), tests[i].want);
    }
    if (!privileged())
        return;
    Error e = BURROW_NO_ERROR;
    NetIPConn *c = net_dialer_dial_ip(NULL, heap_allocator(), context_background(),
                                      S("ip4:icmp"), none, lo, &e);
    if (c == NULL) {
        testing_t_errorf_v(t, "DialIP: %v", e);
        return;
    }
    CHECK_STR_EQ(c_text(addr_str(net_ip_conn_local_addr(c)), buf, sizeof buf),
                 "127.0.0.1");
    CHECK_STR_EQ(c_text(addr_str(net_ip_conn_remote_addr(c)), buf, sizeof buf),
                 "127.0.0.1");
    net_ip_conn_free(c);
}

/* TestDialListenIPArgs: networks with no protocol, an empty one, or one
 * that is not a protocol at all fail every way they can be used. */
static void TestDialListenIPArgs(TestingT *t) {
    need_sockets(t);
    static const char *const fails[][2] = {
        {"ip", "127.0.0.1"},  {"ip:", "127.0.0.1"},  {"ip::", "127.0.0.1"},
        {"ip", "::1"},        {"ip:", "::1"},        {"ip::", "::1"},
        {"ip4", "127.0.0.1"}, {"ip4:", "127.0.0.1"}, {"ip4::", "127.0.0.1"},
        {"ip6", "::1"},       {"ip6:", "::1"},       {"ip6::", "::1"},
    };
    static const char *const works[][2] = {
        {"ip4:47", "127.0.0.1"},
        {"ip6:47", "::1"},
    };
    for (int pass = 0; pass < 2; pass++) {
        bool should_fail = pass == 0;
        if (!should_fail && !privileged())
            break;
        size_t n = should_fail ? sizeof fails / sizeof fails[0]
                               : sizeof works / sizeof works[0];
        for (size_t i = 0; i < n; i++) {
            Str network = str_from_cstr(should_fail ? fails[i][0] : works[i][0]);
            Str address = str_from_cstr(should_fail ? fails[i][1] : works[i][1]);
            Error e = BURROW_NO_ERROR;
            if (!should_fail) {
                /* Go leaves out a row that this machine cannot listen on. */
                NetPacketConn pc =
                    net_listen_packet(heap_allocator(), network, address, &e);
                if (pc.vt == NULL)
                    continue;
                net_packet_conn_free(pc);
            }
            NetConn c = net_dial(heap_allocator(), network, address, &e);
            if (should_fail != BURROW_FAILED(e))
                testing_t_errorf_v(t, "Dial(%s, %s) = %v; want failure %t", network,
                                   address, e, should_fail);
            net_conn_free(c);
            NetPacketConn pc =
                net_listen_packet(heap_allocator(), network, address, &e);
            if (should_fail != BURROW_FAILED(e))
                testing_t_errorf_v(t, "ListenPacket(%s, %s) = %v; want failure %t",
                                   network, address, e, should_fail);
            net_packet_conn_free(pc);
            NetIPAddr *ra = net_resolve_ip_addr(a, S("ip"), address, &e);
            if (ra == NULL) {
                testing_t_errorf_v(t, "ResolveIPAddr(ip, %s) = %v", address, e);
                continue;
            }
            NetIPConn *ic = net_dial_ip(heap_allocator(), network, NULL, ra, &e);
            if (should_fail != BURROW_FAILED(e))
                testing_t_errorf_v(t, "DialIP(%s, %s) = %v; want failure %t", network,
                                   address, e, should_fail);
            net_ip_conn_free(ic);
            ic = net_listen_ip(heap_allocator(), network, ra, &e);
            if (should_fail != BURROW_FAILED(e))
                testing_t_errorf_v(t, "ListenIP(%s, %s) = %v; want failure %t", network,
                                   address, e, should_fail);
            net_ip_conn_free(ic);
        }
    }
}

/* TestIPConnLocalName */
static void TestIPConnLocalName(TestingT *t) {
    need_root(t);
    NetIPAddr lo = ip4(127, 0, 0, 1);
    NetIPAddr zero = {0};
    const NetIPAddr *laddrs[] = {&lo, &zero, NULL};
    const char *want[] = {"127.0.0.1", "0.0.0.0", "0.0.0.0"};
    for (size_t i = 0; i < sizeof laddrs / sizeof laddrs[0]; i++) {
        Error e = BURROW_NO_ERROR;
        NetIPConn *c = net_listen_ip(heap_allocator(), S("ip4:icmp"), laddrs[i], &e);
        if (c == NULL) {
            testing_t_errorf_v(t, "ListenIP: %v", e);
            continue;
        }
        NetAddr la = net_ip_conn_local_addr(c);
        CHECK(la.vt != NULL);
        char buf[64];
        CHECK_STR_EQ(c_text(addr_str(la), buf, sizeof buf), want[i]);
        CHECK(net_ip_conn_remote_addr(c).vt == NULL);
        net_ip_conn_free(c);
    }
}

/* TestIPConnRemoteName, with the texts Go gives for both ends. */
static void TestIPConnRemoteName(TestingT *t) {
    need_root(t);
    NetIPAddr lo16 = ip4(127, 0, 0, 1);
    NetIPAddr ra = {net_ip_to4(lo16.ip), BURROW_STR_EMPTY};
    Error e = BURROW_NO_ERROR;
    NetIPConn *c = net_dial_ip(heap_allocator(), S("ip:tcp"), &lo16, &ra, &e);
    if (c == NULL) {
        testing_t_errorf_v(t, "DialIP: %v", e);
        return;
    }
    NetAddr r = net_ip_conn_remote_addr(c);
    CHECK(r.vt != NULL && r.vt->self_type == TYPE_NET_IP_ADDR);
    if (r.vt != NULL) {
        const NetIPAddr *got = r.data;
        CHECK(got->ip.len == 4 && memcmp(got->ip.p, ra.ip.p, 4) == 0);
        CHECK(got->zone.len == 0);
    }
    char buf[96];
    CHECK_STR_EQ(c_text(addr_str(net_ip_conn_local_addr(c)), buf, sizeof buf),
                 "127.0.0.1");
    /* A dialed connection only writes to where it is connected. */
    Slice p = slice_from(buf, 1, 1, TYPE_BYTE);
    CHECK(net_ip_conn_write_to_ip(c, p, &lo16, &e) == 0);
    CHECK(errors_is(e, net_err_write_to_connected));
    CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf),
                 "write ip 127.0.0.1->127.0.0.1: use of WriteTo with pre-connected "
                 "connection");
    net_ip_conn_free(c);
}

/* What a raw socket that is not connected says to a write with no address,
 * and to an address that is not an IPAddr. */
static void TestIPConnWriteErrors(TestingT *t) {
    need_root(t);
    Error e = BURROW_NO_ERROR;
    NetIPConn *c = net_listen_ip(heap_allocator(), S("ip4:icmp"), NULL, &e);
    if (c == NULL) {
        testing_t_errorf_v(t, "ListenIP: %v", e);
        return;
    }
    char buf[96];
    Slice p = slice_from(buf, 1, 1, TYPE_BYTE);
    CHECK(net_ip_conn_write(c, p, &e) == 0);
    CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf),
                 "write ip4 0.0.0.0: write: destination address required");
    NetUDPAddr u = {0};
    CHECK(net_ip_conn_write_to(c, p, net_udp_addr_as_addr(&u), &e) == 0);
    CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf),
                 "write ip4 0.0.0.0->:0: invalid argument");
    Int oobn = -1;
    CHECK(net_ip_conn_write_msg_ip(c, p, (Slice){0}, NULL, &oobn, &e) == 0);
    CHECK_INT_EQ(oobn, 0);
    CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf),
                 "write ip4 0.0.0.0: missing address");
    net_ip_conn_free(c);
}

/* An ICMP echo request to the loopback address, and the reply the system
 * sends back, read with the IPv4 header taken off as Go does. */
static void TestIPConnICMPEcho(TestingT *t) {
    need_root(t);
    NetIPAddr lo = ip4(127, 0, 0, 1);
    Error e = BURROW_NO_ERROR;
    NetPacketConn pc =
        net_listen_packet(heap_allocator(), S("ip4:icmp"), S("127.0.0.1"), &e);
    if (pc.vt == NULL) {
        testing_t_errorf_v(t, "ListenPacket: %v", e);
        return;
    }
    NetIPConn *c = net_packet_conn_as_ip_conn(pc);
    CHECK(c != NULL);
    (void)net_ip_conn_set_deadline(c, time_add(time_now(), 5 * TIME_SECOND));
    /* Type 8, code 0, the checksum, an identifier of 0x6278 and sequence 1. */
    Byte req[12] = {8, 0, 0, 0, 0x62, 0x78, 0, 1, 'p', 'i', 'n', 'g'};
    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof req; i += 2)
        sum += (uint32_t)req[i] << 8 | req[i + 1];
    while (sum >> 16 != 0)
        sum = (sum & 0xffff) + (sum >> 16);
    sum = ~sum & 0xffff;
    req[2] = (Byte)(sum >> 8);
    req[3] = (Byte)sum;
    Int n = net_ip_conn_write_to_ip(
        c, slice_from(req, sizeof req, sizeof req, TYPE_BYTE), &lo, &e);
    if (n != (Int)sizeof req || BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "WriteToIP: %d, %v", n, e);
        net_packet_conn_free(pc);
        return;
    }
    /* The socket sees every ICMP packet, the request it sent included, so
     * look for the reply. */
    bool replied = false;
    for (int tries = 0; tries < 8 && !replied; tries++) {
        Byte got[128];
        NetIPAddr *from = NULL;
        n = net_ip_conn_read_from_ip(
            c, slice_from(got, sizeof got, sizeof got, TYPE_BYTE), a, &from, &e);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "ReadFromIP: %v", e);
            break;
        }
        CHECK(from != NULL);
        if (n == (Int)sizeof req && got[0] == 0 && got[4] == 0x62 && got[5] == 0x78 &&
            memcmp(got + 8, "ping", 4) == 0) {
            replied = true;
            char buf[32];
            CHECK_STR_EQ(c_text(net_ip_addr_string(from, a), buf, sizeof buf),
                         "127.0.0.1");
        }
    }
    CHECK(replied);
    net_packet_conn_free(pc);
}

/* The same echo through WriteMsgIP and ReadMsgIP, which leaves the IPv4
 * header in front of the reply, as Go's readMsg does. */
static void TestIPConnICMPEchoMsg(TestingT *t) {
    need_root(t);
    NetIPAddr lo = ip4(127, 0, 0, 1);
    Error e = BURROW_NO_ERROR;
    NetIPConn *c = net_listen_ip(heap_allocator(), S("ip4:icmp"), &lo, &e);
    if (c == NULL) {
        testing_t_errorf_v(t, "ListenIP: %v", e);
        return;
    }
    (void)net_ip_conn_set_deadline(c, time_add(time_now(), 5 * TIME_SECOND));
    Byte req[12] = {8, 0, 0, 0, 0x62, 0x79, 0, 1, 'p', 'i', 'n', 'g'};
    uint32_t sum = 0;
    for (size_t i = 0; i < sizeof req; i += 2)
        sum += (uint32_t)req[i] << 8 | req[i + 1];
    while (sum >> 16 != 0)
        sum = (sum & 0xffff) + (sum >> 16);
    sum = ~sum & 0xffff;
    req[2] = (Byte)(sum >> 8);
    req[3] = (Byte)sum;
    Int oobn = -1;
    Int n =
        net_ip_conn_write_msg_ip(c, slice_from(req, sizeof req, sizeof req, TYPE_BYTE),
                                 (Slice){0}, &lo, &oobn, &e);
    if (n != (Int)sizeof req || BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "WriteMsgIP: %d, %v", n, e);
        net_ip_conn_free(c);
        return;
    }
    CHECK_INT_EQ(oobn, 0);
    bool replied = false;
    for (int tries = 0; tries < 8 && !replied; tries++) {
        Byte got[128];
        Byte oob[64];
        NetIPAddr *from = NULL;
        Int flags = -1;
        n = net_ip_conn_read_msg_ip(c,
                                    slice_from(got, sizeof got, sizeof got, TYPE_BYTE),
                                    slice_from(oob, sizeof oob, sizeof oob, TYPE_BYTE),
                                    a, &oobn, &flags, &from, &e);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "ReadMsgIP: %v", e);
            break;
        }
        CHECK(from != NULL);
        CHECK_INT_EQ(oobn, 0);
        CHECK_INT_EQ(flags, 0);
        Int hl = (Int)(got[0] & 0x0f) << 2;
        if (got[0] >> 4 == 4 && n == hl + (Int)sizeof req && got[hl] == 0 &&
            got[hl + 4] == 0x62 && got[hl + 5] == 0x79) {
            replied = true;
            char buf[32];
            CHECK_STR_EQ(c_text(net_ip_addr_string(from, a), buf, sizeof buf),
                         "127.0.0.1");
        }
    }
    CHECK(replied);
    net_ip_conn_free(c);
}

#define TESTS(X)                                                                       \
    X(TestIPConnErrors)                                                                \
    X(TestDialerDialIP)                                                                \
    X(TestDialListenIPArgs)                                                            \
    X(TestIPConnLocalName)                                                             \
    X(TestIPConnRemoteName)                                                            \
    X(TestIPConnWriteErrors)                                                           \
    X(TestIPConnICMPEcho)                                                              \
    X(TestIPConnICMPEchoMsg)

static int net_ipraw_main(TestingM *m) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(net_ipraw_main, TESTS)
