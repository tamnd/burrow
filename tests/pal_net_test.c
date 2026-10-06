/* The socket group of the platform layer.
 *
 * Everything here talks to itself over loopback, so the tests need no network
 * and no other machine. What is checked is what burrow/pal.h promises and what
 * net will build on: a socket never blocks, an address comes back the way it
 * went in, a non blocking connect says PAL_EINPROGRESS on every system, the
 * options read back what was set, and a write to a connection the far end has
 * closed is an error and not a signal that ends the test.
 *
 * A socket that is not ready says PAL_EAGAIN, and with no poller here the
 * tests wait for it by trying again after a millisecond, for up to ten
 * seconds.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/pal.h"
#include "burrow/testing.h"

#include <stdio.h>
#include <string.h>

#define WAIT_TRIES 10000
#define WAIT_NS 1000000

#if defined(BURROW_OS_WASI)
#define SKIP_WITHOUT_SOCKETS(t) testing_t_skip_v((t), "wasip1 cannot make a socket")
#else
#define SKIP_WITHOUT_SOCKETS(t) ((void)(t))
#endif

static PalSockAddr loopback4(uint16_t port) {
    PalSockAddr a = {0};
    a.family = PAL_AF_INET;
    a.port = port;
    a.addr[0] = 127;
    a.addr[3] = 1;
    return a;
}

static PalSockAddr loopback6(uint16_t port) {
    PalSockAddr a = {0};
    a.family = PAL_AF_INET6;
    a.port = port;
    a.addr[15] = 1;
    return a;
}

static bool same_inet(const PalSockAddr *a, const PalSockAddr *b) {
    int n = a->family == PAL_AF_INET ? 4 : 16;
    return a->family == b->family && a->port == b->port &&
           memcmp(a->addr, b->addr, (size_t)n) == 0;
}

/* A listener on loopback on a port the system picks, with the address it got.
 * -1 when the family is not there, which the caller skips on. */
static int64_t listen_on(TestingT *t, PalSockAddr bind_to, int32_t type,
                         PalSockAddr *got) {
    PalErrno err;
    int64_t fd = pal_socket((int32_t)bind_to.family, type, 0, &err);
    if (fd < 0)
        return -1;
    if (!pal_bind(fd, &bind_to, &err)) {
        pal_socket_close(fd, NULL);
        if (err == PAL_EADDRNOTAVAIL || err == PAL_EAFNOSUPPORT)
            return -1;
        testing_t_fatalf_v(t, "bind: %s", pal_errno_string(err));
        return -1;
    }
    if (type == PAL_SOCK_STREAM && !pal_listen(fd, 16, &err)) {
        testing_t_fatalf_v(t, "listen: %s", pal_errno_string(err));
        return -1;
    }
    if (!pal_getsockname(fd, got, &err)) {
        testing_t_fatalf_v(t, "getsockname: %s", pal_errno_string(err));
        return -1;
    }
    return fd;
}

/* Accepts the next connection on l, trying again while there is none yet. */
static int64_t accept_one(int64_t l, PalSockAddr *peer, PalErrno *err) {
    for (int i = 0; i < WAIT_TRIES; i++) {
        int64_t fd = pal_accept(l, peer, err);
        if (fd >= 0 || *err != PAL_EAGAIN)
            return fd;
        pal_nanosleep(WAIT_NS);
    }
    return -1;
}

/* Reads exactly n bytes from a stream, or fewer at its end. */
static int64_t read_full(int64_t fd, char *buf, int64_t n, PalErrno *err) {
    int64_t got = 0;
    for (int i = 0; i < WAIT_TRIES && got < n; i++) {
        int64_t r = pal_recvfrom(fd, buf + got, n - got, NULL, err);
        if (r == 0)
            break;
        if (r > 0) {
            got += r;
            continue;
        }
        if (*err != PAL_EAGAIN)
            return -1;
        pal_nanosleep(WAIT_NS);
    }
    return got;
}

/* What became of a non blocking connect, once it is settled: PAL_OK when it
 * is connected, or the reason it is not. */
static PalErrno connect_result(int64_t fd) {
    for (int i = 0; i < WAIT_TRIES; i++) {
        PalSockAddr peer;
        PalErrno err;
        int64_t v = 0;
        if (!pal_getsockopt(fd, PAL_SO_ERROR, &v, &err))
            return err;
        if (v != PAL_OK)
            return (PalErrno)v;
        if (pal_getpeername(fd, &peer, &err))
            return PAL_OK;
        pal_nanosleep(WAIT_NS);
    }
    return PAL_ETIMEDOUT;
}

/* Dials addr from a new socket of the same family and waits until it is
 * connected. */
static int64_t dial(TestingT *t, const PalSockAddr *addr) {
    PalErrno err;
    int64_t fd = pal_socket((int32_t)addr->family, PAL_SOCK_STREAM, 0, &err);
    if (fd < 0) {
        testing_t_fatalf_v(t, "socket: %s", pal_errno_string(err));
        return -1;
    }
    if (!pal_connect(fd, addr, &err) && err != PAL_EINPROGRESS) {
        testing_t_fatalf_v(t, "connect: %s", pal_errno_string(err));
        return -1;
    }
    PalErrno r = connect_result(fd);
    if (r != PAL_OK) {
        testing_t_fatalf_v(t, "connect finished with %s", pal_errno_string(r));
        return -1;
    }
    return fd;
}

/* One stream connection over addr's family: both ends agree on who is who,
 * bytes go both ways, and a shutdown is an end of stream at the other end. */
static void stream_round_trip(TestingT *t, PalSockAddr bind_to) {
    PalSockAddr laddr;
    int64_t l = listen_on(t, bind_to, PAL_SOCK_STREAM, &laddr);
    if (l < 0) {
        testing_t_skip_v(t, "no loopback for this family here");
        return;
    }
    if (laddr.port == 0)
        testing_t_errorf_v(t, "the listener got port 0");
    int64_t c = dial(t, &laddr);
    PalSockAddr peer;
    PalErrno err;
    int64_t s = accept_one(l, &peer, &err);
    if (s < 0) {
        testing_t_fatalf_v(t, "accept: %s", pal_errno_string(err));
        return;
    }

    PalSockAddr cname;
    PalSockAddr cpeer;
    CHECK(pal_getsockname(c, &cname, NULL));
    CHECK(pal_getpeername(c, &cpeer, NULL));
    if (!same_inet(&cpeer, &laddr))
        testing_t_errorf_v(t, "the client's peer is not the listener's address");
    if (!same_inet(&peer, &cname))
        testing_t_errorf_v(t, "accept's peer is not the client's own address");

    CHECK_INT_EQ(pal_sendto(c, "hello", 5, NULL, &err), 5);
    char buf[16] = {0};
    CHECK_INT_EQ(read_full(s, buf, 5, &err), 5);
    CHECK_STR_EQ(buf, "hello");

    CHECK_INT_EQ(pal_sendto(s, "back", 4, NULL, &err), 4);
    memset(buf, 0, sizeof buf);
    CHECK_INT_EQ(read_full(c, buf, 4, &err), 4);
    CHECK_STR_EQ(buf, "back");

    /* Nothing more has been sent, so a read now would block. */
    CHECK_INT_EQ(pal_recvfrom(s, buf, sizeof buf, NULL, &err), -1);
    CHECK_INT_EQ(err, PAL_EAGAIN);

    CHECK(pal_shutdown(c, PAL_SHUT_WR, &err));
    CHECK_INT_EQ(read_full(s, buf, sizeof buf, &err), 0);

    CHECK(pal_socket_close(s, NULL));
    CHECK(pal_socket_close(c, NULL));
    CHECK(pal_socket_close(l, NULL));
}

static void TestAStreamOverIPv4LoopbackCarriesBytesBothWays(TestingT *t) {
    SKIP_WITHOUT_SOCKETS(t);
    stream_round_trip(t, loopback4(0));
}

static void TestAStreamOverIPv6LoopbackCarriesBytesBothWays(TestingT *t) {
    SKIP_WITHOUT_SOCKETS(t);
    stream_round_trip(t, loopback6(0));
}

static void TestADatagramSaysWhereItCameFrom(TestingT *t) {
    SKIP_WITHOUT_SOCKETS(t);
    PalSockAddr a1;
    PalSockAddr a2;
    int64_t u1 = listen_on(t, loopback4(0), PAL_SOCK_DGRAM, &a1);
    int64_t u2 = listen_on(t, loopback4(0), PAL_SOCK_DGRAM, &a2);
    if (u1 < 0 || u2 < 0) {
        testing_t_fatalf_v(t, "no UDP on IPv4 loopback");
        return;
    }
    PalErrno err;
    CHECK_INT_EQ(pal_sendto(u1, "ping", 4, &a2, &err), 4);
    char buf[16] = {0};
    PalSockAddr from;
    int64_t n = -1;
    for (int i = 0; i < WAIT_TRIES; i++) {
        n = pal_recvfrom(u2, buf, sizeof buf, &from, &err);
        if (n >= 0 || err != PAL_EAGAIN)
            break;
        pal_nanosleep(WAIT_NS);
    }
    CHECK_INT_EQ(n, 4);
    CHECK_STR_EQ(buf, "ping");
    if (!same_inet(&from, &a1))
        testing_t_errorf_v(t, "the datagram came from port %d, want %d", (int)from.port,
                           (int)a1.port);
    CHECK(pal_socket_close(u1, NULL));
    CHECK(pal_socket_close(u2, NULL));
}

static void TestAUnixSocketHasThePathItWasBoundTo(TestingT *t) {
    SKIP_WITHOUT_SOCKETS(t);
    char dir[512];
    PalErrno err;
    int64_t dn = pal_temp_dir(dir, (int64_t)sizeof dir - 1, &err);
    if (dn < 0) {
        testing_t_fatalf_v(t, "temp dir: %s", pal_errno_string(err));
        return;
    }
    dir[dn] = 0;
    char path[200];
    int pn =
        snprintf(path, sizeof path, "%s/bx-%lld.sock", dir, (long long)pal_getpid());
    if (pn <= 0 || pn >= 100) {
        testing_t_skip_v(t, "the temporary directory's name is too long for a socket");
        return;
    }
    pal_unlink(path, NULL);

    PalSockAddr a = {0};
    a.family = PAL_AF_UNIX;
    memcpy(a.path, path, (size_t)pn + 1);
    a.path_len = (uint16_t)(pn + 1);
    int64_t l = pal_socket(PAL_AF_UNIX, PAL_SOCK_STREAM, 0, &err);
    if (l < 0) {
        testing_t_skip_v(t, "no Unix domain sockets here: %s", pal_errno_string(err));
        return;
    }
    if (!pal_bind(l, &a, &err) || !pal_listen(l, 4, &err)) {
        testing_t_fatalf_v(t, "bind and listen on %s: %s", path, pal_errno_string(err));
        return;
    }
    PalSockAddr got;
    CHECK(pal_getsockname(l, &got, &err));
    CHECK_INT_EQ(got.family, PAL_AF_UNIX);
    /* The name ends at its NUL, and some systems report room after it. */
    if (got.path_len < pn || memcmp(got.path, path, (size_t)pn) != 0)
        testing_t_errorf_v(t, "getsockname gave a different path");

    int64_t c = dial(t, &a);
    PalSockAddr peer;
    int64_t s = accept_one(l, &peer, &err);
    if (s < 0) {
        testing_t_fatalf_v(t, "accept: %s", pal_errno_string(err));
        return;
    }
    CHECK_INT_EQ(peer.family, PAL_AF_UNIX);
    /* The client was never bound, so it has no name, whatever length the
     * system reports it with. */
    if (peer.path_len > 0 && peer.path[0] != 0)
        testing_t_errorf_v(t, "an unbound client has a name");
    CHECK_INT_EQ(pal_sendto(c, "unix", 4, NULL, &err), 4);
    char buf[8] = {0};
    CHECK_INT_EQ(read_full(s, buf, 4, &err), 4);
    CHECK_STR_EQ(buf, "unix");

    CHECK(pal_socket_close(s, NULL));
    CHECK(pal_socket_close(c, NULL));
    CHECK(pal_socket_close(l, NULL));
    pal_unlink(path, NULL);
}

static void TestAPathTooLongForASocketIsRefused(TestingT *t) {
    SKIP_WITHOUT_SOCKETS(t);
    PalErrno err;
    int64_t fd = pal_socket(PAL_AF_UNIX, PAL_SOCK_STREAM, 0, &err);
    if (fd < 0) {
        testing_t_skip_v(t, "no Unix domain sockets here: %s", pal_errno_string(err));
        return;
    }
    PalSockAddr a = {0};
    a.family = PAL_AF_UNIX;
    a.path_len = sizeof a.path + 1;
    CHECK(!pal_bind(fd, &a, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);
    CHECK(pal_socket_close(fd, NULL));
}

static void TestAConnectionToAClosedPortIsRefused(TestingT *t) {
    SKIP_WITHOUT_SOCKETS(t);
    /* A port that was just free: bind, read the port back, close. */
    PalSockAddr a;
    int64_t l = listen_on(t, loopback4(0), PAL_SOCK_STREAM, &a);
    if (l < 0) {
        testing_t_fatalf_v(t, "no TCP on IPv4 loopback");
        return;
    }
    CHECK(pal_socket_close(l, NULL));

    PalErrno err;
    int64_t fd = pal_socket(PAL_AF_INET, PAL_SOCK_STREAM, 0, &err);
    CHECK(fd >= 0);
    PalErrno r = PAL_OK;
    if (pal_connect(fd, &a, &err))
        r = PAL_OK;
    else if (err == PAL_EINPROGRESS)
        r = connect_result(fd);
    else
        r = err;
    CHECK_INT_EQ(r, PAL_ECONNREFUSED);
    CHECK(pal_socket_close(fd, NULL));
}

static void TestASecondBindToTheSamePortIsInUse(TestingT *t) {
    SKIP_WITHOUT_SOCKETS(t);
    PalSockAddr a;
    int64_t l = listen_on(t, loopback4(0), PAL_SOCK_STREAM, &a);
    if (l < 0) {
        testing_t_fatalf_v(t, "no TCP on IPv4 loopback");
        return;
    }
    PalErrno err;
    int64_t fd = pal_socket(PAL_AF_INET, PAL_SOCK_STREAM, 0, &err);
    CHECK(fd >= 0);
    CHECK(!pal_bind(fd, &a, &err));
    CHECK_INT_EQ(err, PAL_EADDRINUSE);
    CHECK(pal_socket_close(fd, NULL));
    CHECK(pal_socket_close(l, NULL));
}

static void TestAWriteToAClosedConnectionIsAnErrorAndNotASignal(TestingT *t) {
    SKIP_WITHOUT_SOCKETS(t);
    PalSockAddr a;
    int64_t l = listen_on(t, loopback4(0), PAL_SOCK_STREAM, &a);
    if (l < 0) {
        testing_t_fatalf_v(t, "no TCP on IPv4 loopback");
        return;
    }
    int64_t c = dial(t, &a);
    PalErrno err;
    int64_t s = accept_one(l, NULL, &err);
    if (s < 0) {
        testing_t_fatalf_v(t, "accept: %s", pal_errno_string(err));
        return;
    }
    CHECK(pal_socket_close(s, NULL));
    /* The first write can still go, into the buffer, before the reset comes
     * back. A later one fails, and the test is still running to see it. */
    PalErrno last = PAL_OK;
    char buf[1024] = {0};
    for (int i = 0; i < WAIT_TRIES; i++) {
        if (pal_sendto(c, buf, sizeof buf, NULL, &err) < 0 && err != PAL_EAGAIN) {
            last = err;
            break;
        }
        pal_nanosleep(WAIT_NS);
    }
    if (last != PAL_EPIPE && last != PAL_ECONNRESET && last != PAL_ECONNABORTED)
        testing_t_errorf_v(t, "writing to a closed connection gave %s",
                           pal_errno_string(last));
    CHECK(pal_socket_close(c, NULL));
    CHECK(pal_socket_close(l, NULL));
}

static void TestTheOptionsReadBackWhatWasSet(TestingT *t) {
    SKIP_WITHOUT_SOCKETS(t);
    PalErrno err;
    int64_t fd = pal_socket(PAL_AF_INET, PAL_SOCK_STREAM, 0, &err);
    CHECK(fd >= 0);
    int64_t v = -2;

    CHECK(pal_setsockopt(fd, PAL_TCP_NODELAY, 1, &err));
    CHECK(pal_getsockopt(fd, PAL_TCP_NODELAY, &v, &err));
    CHECK_INT_EQ(v, 1);
    CHECK(pal_setsockopt(fd, PAL_TCP_NODELAY, 0, &err));
    CHECK(pal_getsockopt(fd, PAL_TCP_NODELAY, &v, &err));
    CHECK_INT_EQ(v, 0);

    CHECK(pal_setsockopt(fd, PAL_SO_KEEPALIVE, 1, &err));
    CHECK(pal_getsockopt(fd, PAL_SO_KEEPALIVE, &v, &err));
    CHECK_INT_EQ(v, 1);

    CHECK(pal_setsockopt(fd, PAL_SO_REUSEADDR, 1, &err));
    CHECK(pal_getsockopt(fd, PAL_SO_REUSEADDR, &v, &err));
    CHECK_INT_EQ(v, 1);

    CHECK(pal_setsockopt(fd, PAL_SO_LINGER, 5, &err));
    CHECK(pal_getsockopt(fd, PAL_SO_LINGER, &v, &err));
    CHECK_INT_EQ(v, 5);
    CHECK(pal_setsockopt(fd, PAL_SO_LINGER, -1, &err));
    CHECK(pal_getsockopt(fd, PAL_SO_LINGER, &v, &err));
    CHECK_INT_EQ(v, -1);

    CHECK(pal_setsockopt(fd, PAL_IP_TTL, 33, &err));
    CHECK(pal_getsockopt(fd, PAL_IP_TTL, &v, &err));
    CHECK_INT_EQ(v, 33);

    /* A buffer size reads back as whatever the system made of it, which Linux
     * doubles, but never as less than a byte. */
    CHECK(pal_setsockopt(fd, PAL_SO_RCVBUF, 8192, &err));
    CHECK(pal_getsockopt(fd, PAL_SO_RCVBUF, &v, &err));
    CHECK(v > 0);

    CHECK(pal_getsockopt(fd, PAL_SO_ERROR, &v, &err));
    CHECK_INT_EQ(v, PAL_OK);
    CHECK(!pal_setsockopt(fd, PAL_SO_ERROR, 0, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);
    CHECK(!pal_setsockopt(fd, PAL_TCP_NODELAY, -1, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);
    CHECK(!pal_getsockopt(fd, 999, &v, &err));
    CHECK_INT_EQ(err, PAL_ENOTSUP);
    CHECK(pal_socket_close(fd, NULL));
}

static void TestIPv6OnlyCanBeTurnedOnAndOff(TestingT *t) {
    SKIP_WITHOUT_SOCKETS(t);
    PalErrno err;
    int64_t fd = pal_socket(PAL_AF_INET6, PAL_SOCK_STREAM, 0, &err);
    if (fd < 0) {
        testing_t_skip_v(t, "no IPv6 here: %s", pal_errno_string(err));
        return;
    }
    int64_t v = -2;
    CHECK(pal_setsockopt(fd, PAL_IPV6_V6ONLY, 1, &err));
    CHECK(pal_getsockopt(fd, PAL_IPV6_V6ONLY, &v, &err));
    CHECK_INT_EQ(v, 1);
    CHECK(pal_setsockopt(fd, PAL_IPV6_V6ONLY, 0, &err));
    CHECK(pal_getsockopt(fd, PAL_IPV6_V6ONLY, &v, &err));
    CHECK_INT_EQ(v, 0);
    CHECK(pal_socket_close(fd, NULL));
}

static void TestASocketOfNoKnownKindIsRefused(TestingT *t) {
    SKIP_WITHOUT_SOCKETS(t);
    PalErrno err;
    CHECK_INT_EQ(pal_socket(99, PAL_SOCK_STREAM, 0, &err), -1);
    CHECK_INT_EQ(err, PAL_EAFNOSUPPORT);
    CHECK_INT_EQ(pal_socket(PAL_AF_UNSPEC, PAL_SOCK_STREAM, 0, &err), -1);
    CHECK_INT_EQ(err, PAL_EAFNOSUPPORT);
    CHECK_INT_EQ(pal_socket(PAL_AF_INET, 99, 0, &err), -1);
    CHECK_INT_EQ(err, PAL_EINVAL);

    int64_t fd = pal_socket(PAL_AF_INET, PAL_SOCK_STREAM, 0, &err);
    CHECK(fd >= 0);
    PalSockAddr none = {0};
    CHECK(!pal_bind(fd, &none, &err));
    CHECK_INT_EQ(err, PAL_EAFNOSUPPORT);
    CHECK(!pal_bind(fd, NULL, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);
    CHECK(!pal_shutdown(fd, 7, &err));
    CHECK_INT_EQ(err, PAL_EINVAL);
    CHECK(pal_socket_close(fd, NULL));
    CHECK(!pal_listen(-1, 1, &err));
    CHECK_INT_EQ(err, PAL_EBADF);
}

static void TestWasip1HasNoSockets(TestingT *t) {
#if defined(BURROW_OS_WASI)
    PalErrno err;
    CHECK_INT_EQ(pal_socket(PAL_AF_INET, PAL_SOCK_STREAM, 0, &err), -1);
    CHECK_INT_EQ(err, PAL_ENOSYS);
#else
    testing_t_skip_v(t, "this is not wasip1");
#endif
}

#define TESTS(X)                                                                       \
    X(TestAStreamOverIPv4LoopbackCarriesBytesBothWays)                                 \
    X(TestAStreamOverIPv6LoopbackCarriesBytesBothWays)                                 \
    X(TestADatagramSaysWhereItCameFrom)                                                \
    X(TestAUnixSocketHasThePathItWasBoundTo)                                           \
    X(TestAPathTooLongForASocketIsRefused)                                             \
    X(TestAConnectionToAClosedPortIsRefused)                                           \
    X(TestASecondBindToTheSamePortIsInUse)                                             \
    X(TestAWriteToAClosedConnectionIsAnErrorAndNotASignal)                             \
    X(TestTheOptionsReadBackWhatWasSet)                                                \
    X(TestIPv6OnlyCanBeTurnedOnAndOff)                                                 \
    X(TestASocketOfNoKnownKindIsRefused)                                               \
    X(TestWasip1HasNoSockets)

TESTING_MAIN_BARE(TESTS)
