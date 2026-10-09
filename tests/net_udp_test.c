/* UDPConn over loopback, and UDPAddr's text and its trips to and from
 * netip.AddrPort.
 *
 * The cases follow Go's src/net/udpsock_test.go and error_test.go where they
 * can run without the resolver, which is still to come, so every address
 * here is a literal.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/netpoll.h"
#include "burrow/os.h"
#include "burrow/syscall.h"
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
#define HAVE_UDP 1
#endif

static void need_udp(TestingT *t) {
#if !defined(HAVE_UDP)
    testing_t_skip_v(t, "UDP here needs the readiness poll FD");
#else
    (void)t;
#endif
}

static Byte loop4_bytes[4] = {127, 0, 0, 1};
static Byte loop6_bytes[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};

static NetUDPAddr loop4(Int port) {
    NetUDPAddr a = {slice_from(loop4_bytes, 4, 4, TYPE_BYTE), port, BURROW_STR_EMPTY};
    return a;
}

static NetUDPAddr loop6(Int port) {
    NetUDPAddr a = {slice_from(loop6_bytes, 16, 16, TYPE_BYTE), port, BURROW_STR_EMPTY};
    return a;
}

static Slice bytes_of(char *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
}

/* A C string's bytes as a Str. */
static Str str_of(const char *s) {
    return str_from_cstr(s);
}

/* s as a C string in buf, cut to fit. */
static const char *c_text(Str s, char *buf, size_t n) {
    size_t len = (size_t)s.len < n - 1 ? (size_t)s.len : n - 1;
    if (len > 0)
        memcpy(buf, s.p, len);
    buf[len] = '\0';
    return buf;
}

static const char *text_of(Error err, char *buf, size_t n) {
    return c_text(error_text(err), buf, n);
}

/* The text of an address, as a C string in buf. */
static const char *addr_text(NetAddr a, char *buf, size_t n) {
    if (a.vt == NULL) {
        memcpy(buf, "<nil>", 6);
        return buf;
    }
    Str s = a.vt->string(a.data, heap_allocator());
    c_text(s, buf, n);
    mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    return buf;
}

static const char *addr_port_text(NetipAddrPort ap, char *buf, size_t n) {
    Str s = netip_addr_port_string(ap, heap_allocator());
    c_text(s, buf, n);
    mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    return buf;
}

static Int port_of(NetAddr a) {
    return a.data == NULL ? -1 : ((const NetUDPAddr *)a.data)->port;
}

/* A socket bound to an unused port on 127.0.0.1, with a read deadline far
 * enough off that a lost datagram fails the test instead of hanging it. */
static NetUDPConn *listen_loop4(TestingT *t, const char *network) {
    NetUDPAddr la = loop4(0);
    Error e = BURROW_NO_ERROR;
    NetUDPConn *c = net_listen_udp(heap_allocator(), str_of(network), &la, &e);
    if (c == NULL) {
        testing_t_errorf_v(t, "listen: %v", e);
        return NULL;
    }
    (void)net_udp_conn_set_read_deadline(c, time_add(time_now(), 10 * TIME_SECOND));
    return c;
}

/* ------------------------------------------------------- send and receive */

static void TestTwoListenersTradeDatagrams(TestingT *t) {
    need_udp(t);
    NetUDPConn *s = listen_loop4(t, "udp");
    NetUDPConn *c = listen_loop4(t, "udp");
    if (s == NULL || c == NULL) {
        net_udp_conn_free(s);
        net_udp_conn_free(c);
        return;
    }
    Int sport = port_of(net_udp_conn_local_addr(s));
    Int cport = port_of(net_udp_conn_local_addr(c));
    CHECK(sport > 0 && cport > 0);
    CHECK(net_udp_conn_remote_addr(s).vt == NULL);

    NetUDPAddr to = loop4(sport);
    Error e = BURROW_NO_ERROR;
    char ping[] = "ping";
    CHECK_INT_EQ(net_udp_conn_write_to_udp(c, bytes_of(ping, 4), &to, &e), 4);
    CHECK(!BURROW_FAILED(e));

    /* The sender comes back as a UDPAddr of the caller's. */
    char buf[16] = {0};
    NetUDPAddr *from = NULL;
    CHECK_INT_EQ(
        net_udp_conn_read_from_udp(s, bytes_of(buf, 16), heap_allocator(), &from, &e),
        4);
    CHECK(!BURROW_FAILED(e));
    CHECK(memcmp(buf, "ping", 4) == 0);
    char want[64];
    char text[64];
    snprintf(want, sizeof want, "127.0.0.1:%d", (int)cport);
    CHECK(from != NULL);
    if (from == NULL)
        return;
    CHECK_STR_EQ(addr_text(net_udp_addr_as_addr(from), text, sizeof text), want);
    CHECK_INT_EQ(from->ip.len, 4);

    /* The answer goes back by AddrPort, and the sender is one too. */
    char pong[] = "pong";
    CHECK_INT_EQ(net_udp_conn_write_to_udp_addr_port(s, bytes_of(pong, 4),
                                                     net_udp_addr_addr_port(from), &e),
                 4);
    CHECK(!BURROW_FAILED(e));
    net_udp_addr_free(heap_allocator(), from);
    NetipAddrPort ap = {0};
    CHECK_INT_EQ(net_udp_conn_read_from_udp_addr_port(c, bytes_of(buf, 16), &ap, &e),
                 4);
    CHECK(!BURROW_FAILED(e));
    CHECK(memcmp(buf, "pong", 4) == 0);
    snprintf(want, sizeof want, "127.0.0.1:%d", (int)sport);
    CHECK_STR_EQ(addr_port_text(ap, text, sizeof text), want);
    CHECK(netip_addr_is4(netip_addr_port_addr(ap)));

    CHECK(!BURROW_FAILED(net_udp_conn_close(s)));
    CHECK(!BURROW_FAILED(net_udp_conn_close(c)));
    net_udp_conn_free(s);
    net_udp_conn_free(c);
}

static void TestADialedConnTalksToItsPeerOnly(TestingT *t) {
    need_udp(t);
    NetUDPConn *s = listen_loop4(t, "udp");
    if (s == NULL)
        return;
    Int sport = port_of(net_udp_conn_local_addr(s));
    NetUDPAddr ra = loop4(sport);
    Error e = BURROW_NO_ERROR;
    NetUDPConn *c = net_dial_udp(heap_allocator(), BURROW_S("udp"), NULL, &ra, &e);
    if (c == NULL) {
        testing_t_fatalf_v(t, "dial: %v", e);
        return;
    }
    (void)net_udp_conn_set_read_deadline(c, time_add(time_now(), 10 * TIME_SECOND));
    char want[64];
    char text[64];
    snprintf(want, sizeof want, "127.0.0.1:%d", (int)sport);
    CHECK_STR_EQ(addr_text(net_udp_conn_remote_addr(c), text, sizeof text), want);

    char hi[] = "hi";
    CHECK_INT_EQ(net_udp_conn_write(c, bytes_of(hi, 2), &e), 2);
    CHECK(!BURROW_FAILED(e));
    char buf[16] = {0};
    NetAddr from = {0};
    CHECK_INT_EQ(
        net_udp_conn_read_from(s, bytes_of(buf, 16), heap_allocator(), &from, &e), 2);
    CHECK(!BURROW_FAILED(e));
    char local[64];
    CHECK_STR_EQ(addr_text(from, text, sizeof text),
                 addr_text(net_udp_conn_local_addr(c), local, sizeof local));
    CHECK(from.vt != NULL && str_eq(from.vt->network(from.data), BURROW_S("udp")));

    /* WriteTo takes the NetAddr ReadFrom gave. */
    char ho[] = "ho";
    CHECK_INT_EQ(net_udp_conn_write_to(s, bytes_of(ho, 2), from, &e), 2);
    CHECK(!BURROW_FAILED(e));
    net_udp_addr_free(heap_allocator(), (NetUDPAddr *)from.data);
    CHECK_INT_EQ(net_udp_conn_read(c, bytes_of(buf, 16), &e), 2);
    CHECK(!BURROW_FAILED(e));
    CHECK(memcmp(buf, "ho", 2) == 0);
    net_udp_conn_free(c);
    net_udp_conn_free(s);
}

static void TestADatagramIsReadWholeOrCut(TestingT *t) {
    need_udp(t);
#if defined(BURROW_OS_WINDOWS)
    testing_t_skip_v(t, "Windows says WSAEMSGSIZE for a cut datagram");
    return;
#else
    NetUDPConn *s = listen_loop4(t, "udp4");
    NetUDPConn *c = listen_loop4(t, "udp4");
    if (s == NULL || c == NULL) {
        net_udp_conn_free(s);
        net_udp_conn_free(c);
        return;
    }
    NetUDPAddr to = loop4(port_of(net_udp_conn_local_addr(s)));
    Error e = BURROW_NO_ERROR;
    char first[] = "0123456789";
    char second[] = "ab";
    CHECK_INT_EQ(net_udp_conn_write_to_udp(c, bytes_of(first, 10), &to, &e), 10);
    CHECK_INT_EQ(net_udp_conn_write_to_udp(c, bytes_of(second, 2), &to, &e), 2);
    char buf[8] = {0};
    CHECK_INT_EQ(net_udp_conn_read(s, bytes_of(buf, 4), &e), 4);
    CHECK(!BURROW_FAILED(e));
    CHECK(memcmp(buf, "0123", 4) == 0);
    /* What did not fit is gone, and the next read is the next datagram. */
    CHECK_INT_EQ(net_udp_conn_read(s, bytes_of(buf, 8), &e), 2);
    CHECK(memcmp(buf, "ab", 2) == 0);
    net_udp_conn_free(c);
    net_udp_conn_free(s);
#endif
}

static void TestAConnCanBeUsedAsANetConn(TestingT *t) {
    need_udp(t);
    NetUDPConn *s = listen_loop4(t, "udp");
    if (s == NULL)
        return;
    NetUDPAddr ra = loop4(port_of(net_udp_conn_local_addr(s)));
    Error e = BURROW_NO_ERROR;
    NetUDPConn *c = net_dial_udp(heap_allocator(), BURROW_S("udp4"), NULL, &ra, &e);
    if (c == NULL) {
        testing_t_fatalf_v(t, "dial: %v", e);
        return;
    }
    NetConn cc = net_udp_conn_as_conn(c);
    CHECK(net_conn_as_udp_conn(cc) == c);
    char hi[] = "hi";
    CHECK_INT_EQ(cc.vt->writer.write(cc.data, bytes_of(hi, 2), &e), 2);
    NetConn sc = net_udp_conn_as_conn(s);
    char buf[4] = {0};
    CHECK_INT_EQ(sc.vt->reader.read(sc.data, bytes_of(buf, 4), &e), 2);
    CHECK_STR_EQ(buf, "hi");
    CHECK(!BURROW_FAILED(cc.vt->closer.close(cc.data)));
    net_udp_conn_free(c);
    net_udp_conn_free(s);
}

static void TestAListenerWithNoAddressTakesAnyPort(TestingT *t) {
    need_udp(t);
    Error e = BURROW_NO_ERROR;
    NetUDPConn *c = net_listen_udp(heap_allocator(), BURROW_S("udp"), NULL, &e);
    if (c == NULL) {
        testing_t_fatalf_v(t, "listen: %v", e);
        return;
    }
    CHECK(port_of(net_udp_conn_local_addr(c)) > 0);
    net_udp_conn_free(c);
}

/* ---------------------------------------------------------------- errors */

static void TestAnUnknownNetworkIsSaidSo(TestingT *t) {
    (void)t;
    NetUDPAddr ra = loop4(80);
    Error e = BURROW_NO_ERROR;
    char buf[96];
    CHECK(net_dial_udp(heap_allocator(), BURROW_S("udp5"), NULL, &ra, &e) == NULL);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf),
                 "dial udp5 127.0.0.1:80: unknown network udp5");
    CHECK(errors_as(e, TYPE_NET_UNKNOWN_NETWORK_ERROR) != NULL);

    CHECK(net_listen_udp(heap_allocator(), BURROW_S("tcp"), NULL, &e) == NULL);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), "listen tcp: unknown network tcp");
}

static void TestADialWithNoAddressIsMissingOne(TestingT *t) {
    (void)t;
    Error e = BURROW_NO_ERROR;
    char buf[96];
    CHECK(net_dial_udp(heap_allocator(), BURROW_S("udp"), NULL, NULL, &e) == NULL);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), "dial udp: missing address");
}

static void TestTheWriteToErrorsReadTheWayGosDo(TestingT *t) {
    need_udp(t);
    NetUDPConn *s = listen_loop4(t, "udp4");
    if (s == NULL)
        return;
    int port = (int)port_of(net_udp_conn_local_addr(s));
    Error e = BURROW_NO_ERROR;
    char want[128];
    char buf[128];
    char x[] = "x";

    CHECK_INT_EQ(net_udp_conn_write_to_udp(s, bytes_of(x, 1), NULL, &e), 0);
    snprintf(want, sizeof want, "write udp4 127.0.0.1:%d: missing address", port);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), want);

    CHECK_INT_EQ(
        net_udp_conn_write_to_udp_addr_port(s, bytes_of(x, 1), (NetipAddrPort){0}, &e),
        0);
    snprintf(want, sizeof want,
             "write udp4 127.0.0.1:%d->invalid AddrPort: missing address", port);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), want);

    /* An IPv6 address cannot go out of an IPv4 socket, by either road. */
    NetipAddrPort v6 = netip_must_parse_addr_port(BURROW_S("[::1]:53"));
    CHECK_INT_EQ(net_udp_conn_write_to_udp_addr_port(s, bytes_of(x, 1), v6, &e), 0);
    snprintf(want, sizeof want,
             "write udp4 127.0.0.1:%d->[::1]:53: address ::1: non-IPv4 address", port);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), want);
    CHECK(errors_as(e, TYPE_NET_ADDR_ERROR) != NULL);
    NetUDPAddr v6a = loop6(53);
    CHECK_INT_EQ(net_udp_conn_write_to_udp(s, bytes_of(x, 1), &v6a, &e), 0);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), want);

    /* The port is checked where the sockaddr is made, which Go leaves to
     * sendto. */
    NetUDPAddr far = loop4(70000);
    CHECK_INT_EQ(net_udp_conn_write_to_udp(s, bytes_of(x, 1), &far, &e), 0);
    snprintf(want, sizeof want,
             "write udp4 127.0.0.1:%d->127.0.0.1:70000: sendto: invalid argument",
             port);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), want);
    CHECK(errors_as(e, TYPE_OS_SYSCALL_ERROR) != NULL);

    /* WriteTo takes nothing but a UDPAddr. */
    NetTCPAddr tcp = {slice_from(loop4_bytes, 4, 4, TYPE_BYTE), 80, BURROW_STR_EMPTY};
    CHECK_INT_EQ(
        net_udp_conn_write_to(s, bytes_of(x, 1), net_tcp_addr_as_addr(&tcp), &e), 0);
    snprintf(want, sizeof want,
             "write udp4 127.0.0.1:%d->127.0.0.1:80: invalid argument", port);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), want);
    net_udp_conn_free(s);
}

static void TestAWriteToOnADialedConnIsRefused(TestingT *t) {
    need_udp(t);
    NetUDPConn *s = listen_loop4(t, "udp");
    if (s == NULL)
        return;
    NetUDPAddr ra = loop4(port_of(net_udp_conn_local_addr(s)));
    Error e = BURROW_NO_ERROR;
    NetUDPConn *c = net_dial_udp(heap_allocator(), BURROW_S("udp"), NULL, &ra, &e);
    if (c == NULL) {
        testing_t_fatalf_v(t, "dial: %v", e);
        return;
    }
    char x[] = "x";
    CHECK_INT_EQ(net_udp_conn_write_to_udp(c, bytes_of(x, 1), &ra, &e), 0);
    CHECK(errors_is(e, net_err_write_to_connected));
    char want[128];
    char buf[128];
    char local[64];
    snprintf(want, sizeof want,
             "write udp %s->127.0.0.1:%d: use of WriteTo with pre-connected connection",
             addr_text(net_udp_conn_local_addr(c), local, sizeof local), (int)ra.port);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), want);
    CHECK_INT_EQ(net_udp_conn_write_to_udp_addr_port(c, bytes_of(x, 1),
                                                     net_udp_addr_addr_port(&ra), &e),
                 0);
    CHECK(errors_is(e, net_err_write_to_connected));
    net_udp_conn_free(c);
    net_udp_conn_free(s);
}

static void TestAReadPastItsDeadlineTimesOut(TestingT *t) {
    need_udp(t);
    NetUDPConn *s = listen_loop4(t, "udp");
    if (s == NULL)
        return;
    int port = (int)port_of(net_udp_conn_local_addr(s));
    CHECK(!BURROW_FAILED(net_udp_conn_set_read_deadline(
        s, time_add(time_now(), 30 * TIME_MILLISECOND))));
    char buf[8];
    Error e = BURROW_NO_ERROR;
    NetipAddrPort ap = {0};
    CHECK_INT_EQ(net_udp_conn_read_from_udp_addr_port(s, bytes_of(buf, 8), &ap, &e), 0);
    CHECK(errors_is(e, os_err_deadline_exceeded));
    CHECK(net_error_timeout(e));
    CHECK(!netip_addr_port_is_valid(ap));
    char want[96];
    char text[96];
    snprintf(want, sizeof want, "read udp 127.0.0.1:%d: i/o timeout", port);
    CHECK_STR_EQ(text_of(e, text, sizeof text), want);
    CHECK(!BURROW_FAILED(net_udp_conn_set_read_buffer(s, 1 << 16)));
    CHECK(!BURROW_FAILED(net_udp_conn_set_write_buffer(s, 1 << 16)));
    net_udp_conn_free(s);
}

static void TestANilConnIsAnInvalidArgument(TestingT *t) {
    (void)t;
    char buf[64];
    Error e = BURROW_NO_ERROR;
    CHECK_STR_EQ(text_of(net_udp_conn_close(NULL), buf, sizeof buf), EINVAL_TEXT);
    CHECK_INT_EQ(net_udp_conn_read(NULL, (Slice){0}, &e), 0);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), EINVAL_TEXT);
    CHECK(net_udp_conn_local_addr(NULL).vt == NULL);
    CHECK(net_udp_conn_as_conn(NULL).vt == NULL);
    net_udp_conn_free(NULL);
}

/* ---------------------------------------------------------------- IPv6 */

static void TestIPv6LoopbackWorksWhereThereIsOne(TestingT *t) {
    need_udp(t);
    NetUDPAddr la = loop6(0);
    Error e = BURROW_NO_ERROR;
    NetUDPConn *s = net_listen_udp(heap_allocator(), BURROW_S("udp6"), &la, &e);
    if (s == NULL) {
        testing_t_skip_v(t, "no IPv6 loopback here: %v", e);
        return;
    }
    (void)net_udp_conn_set_read_deadline(s, time_add(time_now(), 10 * TIME_SECOND));
    NetUDPAddr ra = loop6(port_of(net_udp_conn_local_addr(s)));
    NetUDPConn *c = net_dial_udp(heap_allocator(), BURROW_S("udp6"), NULL, &ra, &e);
    if (c == NULL) {
        testing_t_fatalf_v(t, "dial: %v", e);
        return;
    }
    char hi[] = "hi";
    CHECK_INT_EQ(net_udp_conn_write(c, bytes_of(hi, 2), &e), 2);
    char buf[4];
    NetipAddrPort ap = {0};
    CHECK_INT_EQ(net_udp_conn_read_from_udp_addr_port(s, bytes_of(buf, 4), &ap, &e), 2);
    char want[64];
    char text[64];
    snprintf(want, sizeof want, "[::1]:%d", (int)port_of(net_udp_conn_local_addr(c)));
    CHECK_STR_EQ(addr_port_text(ap, text, sizeof text), want);
    net_udp_conn_free(c);
    net_udp_conn_free(s);
}

/* ------------------------------------------------------------ messages */

/* TestUDPConnSpecificMethods, for the Msg calls: a message to itself, with
 * room for control messages and none asked for. */
static void TestAMessageGoesOutAndComesBack(TestingT *t) {
    need_udp(t);
    NetUDPConn *c = listen_loop4(t, "udp4");
    if (c == NULL)
        return;
    NetUDPAddr self = loop4(port_of(net_udp_conn_local_addr(c)));
    char wb[] = "UDPCONN TEST";
    char rb[128];
    char oob[128];
    Error e = BURROW_NO_ERROR;
    Int oobn = -1;
    CHECK_INT_EQ(net_udp_conn_write_msg_udp(c, bytes_of(wb, 12), bytes_of(oob, 0),
                                            &self, &oobn, &e),
                 12);
    CHECK(BURROW_OK(e));
    CHECK_INT_EQ(oobn, 0);
    Int flags = -1;
    NetUDPAddr *from = NULL;
    CHECK_INT_EQ(net_udp_conn_read_msg_udp(c, bytes_of(rb, 128), bytes_of(oob, 128),
                                           heap_allocator(), &oobn, &flags, &from, &e),
                 12);
    CHECK(BURROW_OK(e));
    CHECK_INT_EQ(oobn, 0);
    CHECK_INT_EQ(flags, 0);
    CHECK(memcmp(rb, wb, 12) == 0);
    char want[64];
    char text[64];
    snprintf(want, sizeof want, "127.0.0.1:%d", (int)self.port);
    CHECK_STR_EQ(addr_text(net_udp_addr_as_addr(from), text, sizeof text), want);
    net_udp_addr_free(heap_allocator(), from);
    net_udp_conn_free(c);
}

/* A datagram cut to fit says so in its flags, which are the system's. */
static void TestACutMessageIsFlaggedAsCut(TestingT *t) {
    need_udp(t);
    NetUDPConn *c = listen_loop4(t, "udp4");
    if (c == NULL)
        return;
    NetipAddrPort self =
        net_udp_addr_addr_port((const NetUDPAddr *)net_udp_conn_local_addr(c).data);
    char wb[] = "0123456789";
    char rb[4];
    Error e = BURROW_NO_ERROR;
    CHECK_INT_EQ(net_udp_conn_write_msg_udp_addr_port(c, bytes_of(wb, 10), (Slice){0},
                                                      self, NULL, &e),
                 10);
    Int flags = 0;
    Int n = net_udp_conn_read_msg_udp_addr_port(c, bytes_of(rb, 4), (Slice){0}, NULL,
                                                &flags, NULL, &e);
#if defined(BURROW_OS_WINDOWS)
    /* WSARecvMsg fails a datagram it had to cut, with what was read. */
    (void)n;
    (void)flags;
    CHECK(BURROW_FAILED(e));
#else
    CHECK(BURROW_OK(e));
    CHECK_INT_EQ(n, 4);
    CHECK((flags & SYSCALL_MSG_TRUNC) != 0);
#endif
    net_udp_conn_free(c);
}

/* testWriteToConn and testWriteToPacketConn, for WriteMsgUDP: a dialed conn
 * takes no address and a listener needs one. */
static void TestWriteMsgUDPWantsAnAddressOnlyWhenNotDialed(TestingT *t) {
    need_udp(t);
    NetUDPConn *s = listen_loop4(t, "udp");
    if (s == NULL)
        return;
    NetUDPAddr ra = loop4(port_of(net_udp_conn_local_addr(s)));
    Error e = BURROW_NO_ERROR;
    NetUDPConn *c = net_dial_udp(heap_allocator(), BURROW_S("udp"), NULL, &ra, &e);
    if (c == NULL) {
        testing_t_fatalf_v(t, "dial: %v", e);
        net_udp_conn_free(s);
        return;
    }
    char b[] = "CONNECTED-MODE SOCKET";
    Int oobn = -1;
    CHECK_INT_EQ(
        net_udp_conn_write_msg_udp(c, bytes_of(b, 21), (Slice){0}, &ra, &oobn, &e), 0);
    CHECK(errors_is(e, net_err_write_to_connected));
    CHECK_INT_EQ(net_udp_conn_write_msg_udp_addr_port(c, bytes_of(b, 21), (Slice){0},
                                                      net_udp_addr_addr_port(&ra), NULL,
                                                      &e),
                 0);
    CHECK(errors_is(e, net_err_write_to_connected));
    CHECK_INT_EQ(
        net_udp_conn_write_msg_udp(c, bytes_of(b, 21), (Slice){0}, NULL, &oobn, &e),
        21);
    CHECK(BURROW_OK(e));
    CHECK_INT_EQ(net_udp_conn_write_msg_udp_addr_port(c, bytes_of(b, 21), (Slice){0},
                                                      (NetipAddrPort){0}, NULL, &e),
                 21);
    CHECK(BURROW_OK(e));

    char p[] = "UNCONNECTED-MODE SOCKET";
    CHECK_INT_EQ(
        net_udp_conn_write_msg_udp(s, bytes_of(p, 23), (Slice){0}, NULL, &oobn, &e), 0);
    char want[128];
    char buf[128];
    snprintf(want, sizeof want, "write udp 127.0.0.1:%d: missing address",
             (int)ra.port);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), want);
    CHECK_INT_EQ(net_udp_conn_write_msg_udp_addr_port(s, bytes_of(p, 23), (Slice){0},
                                                      (NetipAddrPort){0}, NULL, &e),
                 0);
    snprintf(want, sizeof want,
             "write udp 127.0.0.1:%d->invalid AddrPort: missing address", (int)ra.port);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), want);
    CHECK_INT_EQ(
        net_udp_conn_write_msg_udp(s, bytes_of(p, 23), (Slice){0}, &ra, &oobn, &e), 23);
    CHECK(BURROW_OK(e));

    /* The port is checked where the sockaddr is made, as for WriteToUDP. */
    NetUDPAddr far = loop4(70000);
    CHECK_INT_EQ(
        net_udp_conn_write_msg_udp(s, bytes_of(p, 23), (Slice){0}, &far, NULL, &e), 0);
#if defined(BURROW_OS_WINDOWS)
    snprintf(want, sizeof want,
             "write udp 127.0.0.1:%d->127.0.0.1:70000: wsasendmsg: invalid argument",
             (int)ra.port);
#else
    snprintf(want, sizeof want,
             "write udp 127.0.0.1:%d->127.0.0.1:70000: sendmsg: invalid argument",
             (int)ra.port);
#endif
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), want);
    net_udp_conn_free(c);
    net_udp_conn_free(s);
}

/* TestUDPIPVersionReadMsg: an IPv4 sender reads back as IPv4 both ways. */
static void TestUDPIPVersionReadMsg(TestingT *t) {
    need_udp(t);
    NetUDPConn *c = listen_loop4(t, "udp4");
    if (c == NULL)
        return;
    NetipAddrPort daddr =
        net_udp_addr_addr_port((const NetUDPAddr *)net_udp_conn_local_addr(c).data);
    char buf[8] = {0};
    Error e = BURROW_NO_ERROR;
    CHECK_INT_EQ(net_udp_conn_write_to_udp_addr_port(c, bytes_of(buf, 8), daddr, &e),
                 8);
    NetipAddrPort saddr = {0};
    CHECK_INT_EQ(net_udp_conn_read_msg_udp_addr_port(c, bytes_of(buf, 8), (Slice){0},
                                                     NULL, NULL, &saddr, &e),
                 8);
    CHECK(BURROW_OK(e));
    if (!netip_addr_is4(netip_addr_port_addr(saddr)))
        testing_t_error_v(t, "returned AddrPort is not IPv4");
    CHECK_INT_EQ(net_udp_conn_write_to_udp_addr_port(c, bytes_of(buf, 8), daddr, &e),
                 8);
    NetUDPAddr *soldaddr = NULL;
    CHECK_INT_EQ(net_udp_conn_read_msg_udp(c, bytes_of(buf, 8), (Slice){0},
                                           heap_allocator(), NULL, NULL, &soldaddr, &e),
                 8);
    CHECK(BURROW_OK(e));
    if (soldaddr == NULL || soldaddr->ip.len != 4)
        testing_t_error_v(t, "returned UDPAddr is not IPv4");
    net_udp_addr_free(heap_allocator(), soldaddr);
    net_udp_conn_free(c);
}

/* TestIPv6WriteMsgUDPAddrPortTargetAddrIPVersion: a dual-stack socket sends
 * to IPv4, IPv4-mapped and IPv6 addresses alike. */
static void TestIPv6WriteMsgUDPAddrPortTargetAddrIPVersion(TestingT *t) {
    need_udp(t);
#if defined(BURROW_OS_DRAGONFLY) || defined(BURROW_OS_OPENBSD)
    testing_t_skip_v(t, "IPv6 sockets are always IPv6-only here");
#else
    NetUDPAddr la6 = loop6(0);
    Error e = BURROW_NO_ERROR;
    NetUDPConn *probe = net_listen_udp(heap_allocator(), BURROW_S("udp6"), &la6, &e);
    if (probe == NULL) {
        testing_t_skip_v(t, "skipping: udp6 not available");
        return;
    }
    net_udp_conn_free(probe);
    NetUDPConn *c = net_listen_udp(heap_allocator(), BURROW_S("udp"), NULL, &e);
    if (c == NULL) {
        testing_t_fatalf_v(t, "listen: %v", e);
        return;
    }
    static const char *const daddrs[] = {"127.0.0.1:12345", "[::ffff:127.0.0.1]:12345",
                                         "[::1]:12345"};
    char buf[8] = {0};
    for (size_t i = 0; i < sizeof daddrs / sizeof daddrs[0]; i++) {
        NetipAddrPort d = netip_must_parse_addr_port(str_of(daddrs[i]));
        (void)net_udp_conn_write_msg_udp_addr_port(c, bytes_of(buf, 8), (Slice){0}, d,
                                                   NULL, &e);
        if (BURROW_FAILED(e))
            testing_t_errorf_v(t, "%s: %v", daddrs[i], e);
    }
    net_udp_conn_free(c);
#endif
}

/* TestIPv4WriteMsgUDPAddrPortTargetAddrIPVersion: an IPv4 socket sends to
 * IPv4 and IPv4-mapped addresses, and not to IPv6 ones. */
static void TestIPv4WriteMsgUDPAddrPortTargetAddrIPVersion(TestingT *t) {
    need_udp(t);
    NetUDPConn *c = listen_loop4(t, "udp4");
    if (c == NULL)
        return;
    char buf[8] = {0};
    Error e = BURROW_NO_ERROR;
    NetipAddrPort d4 = netip_must_parse_addr_port(BURROW_S("127.0.0.1:12345"));
    NetipAddrPort d4in6 =
        netip_must_parse_addr_port(BURROW_S("[::ffff:127.0.0.1]:12345"));
    NetipAddrPort d6 = netip_must_parse_addr_port(BURROW_S("[::1]:12345"));
    (void)net_udp_conn_write_msg_udp_addr_port(c, bytes_of(buf, 8), (Slice){0}, d4,
                                               NULL, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "conn.WriteMsgUDPAddrPort(buf, nil, daddr4) failed: %v",
                           e);
    (void)net_udp_conn_write_msg_udp_addr_port(c, bytes_of(buf, 8), (Slice){0}, d4in6,
                                               NULL, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(
            t, "conn.WriteMsgUDPAddrPort(buf, nil, daddr4in6) failed: %v", e);
    (void)net_udp_conn_write_msg_udp_addr_port(c, bytes_of(buf, 8), (Slice){0}, d6,
                                               NULL, &e);
    if (BURROW_OK(e))
        testing_t_error_v(t, "conn.WriteMsgUDPAddrPort(buf, nil, daddr6) should have "
                             "failed, but got no error");
    net_udp_conn_free(c);
}

/* TestReadWriteMsgUDPAddrPortEmptyCmsg: oob with room and no length is no
 * oob at all, golang.org/issue/77875. */
static void TestReadWriteMsgUDPAddrPortEmptyCmsg(TestingT *t) {
    need_udp(t);
    NetUDPConn *c = listen_loop4(t, "udp4");
    if (c == NULL)
        return;
    char buf[8] = {0};
    char cmsg[8];
    Slice cmsg_buf = slice_from(cmsg, 0, 8, TYPE_BYTE);
    NetipAddrPort daddr =
        net_udp_addr_addr_port((const NetUDPAddr *)net_udp_conn_local_addr(c).data);
    Error e = BURROW_NO_ERROR;
    Int cmsgn = -1;
    (void)net_udp_conn_write_msg_udp_addr_port(c, bytes_of(buf, 8), cmsg_buf, daddr,
                                               &cmsgn, &e);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "WriteMsgUDPAddrPort failed: %v", e);
        net_udp_conn_free(c);
        return;
    }
    if (cmsgn != 0)
        testing_t_errorf_v(t, "WriteMsgUDPAddrPort wrote %d cmsg bytes; want 0", cmsgn);
    (void)net_udp_conn_read_msg_udp_addr_port(c, bytes_of(buf, 8), cmsg_buf, &cmsgn,
                                              NULL, NULL, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "ReadMsgUDPAddrPort failed: %v", e);
    else if (cmsgn != 0)
        testing_t_errorf_v(t, "ReadMsgUDPAddrPort read %d cmsg bytes; want 0", cmsgn);
    net_udp_conn_free(c);
}

/* ------------------------------------------------------------- UDPAddr */

static void TestAUDPAddrPrintsTheWayGoPrintsIt(TestingT *t) {
    (void)t;
    Alloc *a = heap_allocator();
    char buf[64];
    struct {
        NetUDPAddr addr;
        const char *want;
    } cases[] = {
        {loop4(53), "127.0.0.1:53"},
        {loop6(443), "[::1]:443"},
        {{slice_from(loop6_bytes, 16, 16, TYPE_BYTE), 5, BURROW_S("eth0")},
         "[::1%eth0]:5"},
        {{{0}, 0, BURROW_STR_EMPTY}, ":0"},
        {{slice_from(loop4_bytes, 3, 3, TYPE_BYTE), 1, BURROW_STR_EMPTY}, "?7f0000:1"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Str s = net_udp_addr_string(&cases[i].addr, a);
        CHECK_STR_EQ(c_text(s, buf, sizeof buf), cases[i].want);
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    }
    Str s = net_udp_addr_string(NULL, a);
    CHECK(str_eq(s, BURROW_S("<nil>")));
    mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
    CHECK(str_eq(net_udp_addr_network(NULL), BURROW_S("udp")));
    CHECK(net_udp_addr_as_addr(NULL).vt == NULL);
}

static void TestAUDPAddrGoesToAndFromAnAddrPort(TestingT *t) {
    (void)t;
    Alloc *a = heap_allocator();
    char buf[64];
    struct {
        const char *in;
        Int iplen;
        const char *want;
    } cases[] = {
        {"1.2.3.4:53", 4, "1.2.3.4:53"},
        /* IP.String prints a mapped IPv4 address as the IPv4 one. */
        {"[::ffff:1.2.3.4]:53", 16, "1.2.3.4:53"},
        {"[fe80::1%eth0]:5353", 16, "[fe80::1%eth0]:5353"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        NetipAddrPort ap = netip_must_parse_addr_port(str_of(cases[i].in));
        NetUDPAddr *u = net_udp_addr_from_addr_port(a, ap);
        CHECK(u != NULL);
        if (u == NULL)
            continue;
        CHECK_INT_EQ(u->ip.len, cases[i].iplen);
        Str s = net_udp_addr_string(u, a);
        CHECK_STR_EQ(c_text(s, buf, sizeof buf), cases[i].want);
        mem_free(a, (void *)(uintptr_t)s.p, (size_t)s.len, 1);
        CHECK(netip_addr_port_compare(net_udp_addr_addr_port(u), ap) == 0);
        net_udp_addr_free(a, u);
    }

    /* The zero AddrPort is a UDPAddr with no IP, and back again it is not
     * valid, as is one made from an IP of the wrong length. */
    NetUDPAddr *z = net_udp_addr_from_addr_port(a, (NetipAddrPort){0});
    CHECK(z != NULL);
    if (z != NULL) {
        CHECK(z->ip.p == NULL && z->port == 0 && z->zone.len == 0);
        CHECK(!netip_addr_port_is_valid(net_udp_addr_addr_port(z)));
        net_udp_addr_free(a, z);
    }
    CHECK(!netip_addr_port_is_valid(net_udp_addr_addr_port(NULL)));
    NetUDPAddr odd = {slice_from(loop4_bytes, 3, 3, TYPE_BYTE), 1, BURROW_STR_EMPTY};
    CHECK_STR_EQ(addr_port_text(net_udp_addr_addr_port(&odd), buf, sizeof buf),
                 "invalid AddrPort");
}

#define TESTS(X)                                                                       \
    X(TestTwoListenersTradeDatagrams)                                                  \
    X(TestADialedConnTalksToItsPeerOnly)                                               \
    X(TestADatagramIsReadWholeOrCut)                                                   \
    X(TestAConnCanBeUsedAsANetConn)                                                    \
    X(TestAListenerWithNoAddressTakesAnyPort)                                          \
    X(TestAnUnknownNetworkIsSaidSo)                                                    \
    X(TestADialWithNoAddressIsMissingOne)                                              \
    X(TestTheWriteToErrorsReadTheWayGosDo)                                             \
    X(TestAWriteToOnADialedConnIsRefused)                                              \
    X(TestAReadPastItsDeadlineTimesOut)                                                \
    X(TestANilConnIsAnInvalidArgument)                                                 \
    X(TestIPv6LoopbackWorksWhereThereIsOne)                                            \
    X(TestAMessageGoesOutAndComesBack)                                                 \
    X(TestACutMessageIsFlaggedAsCut)                                                   \
    X(TestWriteMsgUDPWantsAnAddressOnlyWhenNotDialed)                                  \
    X(TestUDPIPVersionReadMsg)                                                         \
    X(TestIPv6WriteMsgUDPAddrPortTargetAddrIPVersion)                                  \
    X(TestIPv4WriteMsgUDPAddrPortTargetAddrIPVersion)                                  \
    X(TestReadWriteMsgUDPAddrPortEmptyCmsg)                                            \
    X(TestAUDPAddrPrintsTheWayGoPrintsIt)                                              \
    X(TestAUDPAddrGoesToAndFromAnAddrPort)

TESTING_MAIN(TESTS)
