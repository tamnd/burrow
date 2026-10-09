/* UnixConn and UnixListener on sockets in a fresh directory, and UnixAddr.
 *
 * The cases follow Go's src/net/unixsock_test.go, unixsock_linux_test.go and
 * the Unix parts of error_test.go.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/netpoll.h"
#include "burrow/os.h"
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
#define HAVE_UNIX 1
#endif

/* Windows has stream sockets only. */
#if defined(HAVE_UNIX) && !defined(BURROW_OS_WINDOWS)
#define HAVE_UNIXGRAM 1
#endif

/* Linux names a socket that was never bound "@", and the BSDs name it "". */
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_ANDROID) || defined(BURROW_OS_COSMO)
#define UNNAMED "@"
#define HAVE_ABSTRACT 1
#else
#define UNNAMED ""
#endif

#define S(lit) BURROW_S(lit)

static Arena ar;
static Alloc *a;

static void need_unix(TestingT *t) {
#if !defined(HAVE_UNIX)
    testing_t_skip_v(t, "Unix sockets here need the readiness poll FD");
#else
    (void)t;
#endif
}

static void need_unixgram(TestingT *t) {
#if !defined(HAVE_UNIXGRAM)
    testing_t_skip_v(t,
                     "unixgram needs the readiness poll FD and a system that has it");
#else
    (void)t;
#endif
}

static Slice bytes_of(char *p, Int n) {
    return slice_from(p, n, n, TYPE_BYTE);
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

/* The text of an address, "<nil>" for nil, as a C string in buf. */
static const char *addr_text(NetAddr addr, char *buf, size_t n) {
    if (addr.vt == NULL)
        return c_text(S("<nil>"), buf, n);
    return c_text(addr.vt->string(addr.data, a), buf, n);
}

/* A fresh directory, whose name is short enough for macOS, where a socket's
 * path can be no longer than 103 bytes. */
static Str temp_dir(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    Str d = os_mkdir_temp(a, S(""), S("bxu"), &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "MkdirTemp: %v", e);
    return d;
}

/* dir and name joined with a slash, in the arena. */
static Str path_in(Str dir, const char *name) {
    Int n = (Int)strlen(name);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(dir.len + 1 + n), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, dir.p, (size_t)dir.len);
    p[dir.len] = '/';
    memcpy(p + dir.len + 1, name, (size_t)n);
    return str_from_bytes(p, dir.len + 1 + n);
}

static bool exists(Str path) {
    Error e = BURROW_NO_ERROR;
    (void)os_lstat(a, path, &e);
    return BURROW_OK(e);
}

static Time soon(void) {
    return time_add(time_now(), 10 * TIME_SECOND);
}

/* ---------------------------------------------------------------- streams */

static void TestAStreamListenerAcceptsAndTalks(TestingT *t) {
    need_unix(t);
    Str dir = temp_dir(t);
    if (dir.len == 0)
        return;
    NetUnixAddr addr = {path_in(dir, "s"), S("unix")};
    Error e = BURROW_NO_ERROR;
    NetUnixListener *l = net_listen_unix(a, S("unix"), &addr, &e);
    if (l == NULL) {
        testing_t_errorf_v(t, "listen: %v", e);
        (void)os_remove_all(dir);
        return;
    }
    CHECK(exists(addr.name));
    char want[160];
    char got[160];
    c_text(addr.name, want, sizeof want);
    CHECK_STR_EQ(addr_text(net_unix_listener_addr(l), got, sizeof got), want);
    CHECK_STR_EQ(
        c_text(net_unix_listener_addr(l).vt->network(net_unix_listener_addr(l).data),
               got, sizeof got),
        "unix");

    NetUnixConn *c = net_dial_unix(a, S("unix"), NULL, &addr, &e);
    CHECK(c != NULL);
    NetUnixConn *s = net_unix_listener_accept_unix(l, &e);
    CHECK(s != NULL);
    if (c == NULL || s == NULL) {
        testing_t_errorf_v(t, "dial or accept: %v", e);
        net_unix_conn_free(c);
        net_unix_conn_free(s);
        net_unix_listener_free(l);
        (void)os_remove_all(dir);
        return;
    }
    (void)net_unix_conn_set_deadline(c, soon());
    (void)net_unix_conn_set_deadline(s, soon());

    /* The dialer's end has no name, and the listener's end has the path. */
    CHECK_STR_EQ(addr_text(net_unix_conn_remote_addr(c), got, sizeof got), want);
    CHECK_STR_EQ(addr_text(net_unix_conn_local_addr(c), got, sizeof got), UNNAMED);
    CHECK_STR_EQ(addr_text(net_unix_conn_local_addr(s), got, sizeof got), want);

    char hello[] = "hello";
    CHECK_INT_EQ(net_unix_conn_write(c, bytes_of(hello, 5), &e), 5);
    char buf[16] = {0};
    CHECK_INT_EQ(net_unix_conn_read(s, bytes_of(buf, 16), &e), 5);
    CHECK(memcmp(buf, "hello", 5) == 0);
    char back[] = "back";
    CHECK_INT_EQ(net_unix_conn_write(s, bytes_of(back, 4), &e), 4);
    CHECK_INT_EQ(net_unix_conn_read(c, bytes_of(buf, 16), &e), 4);
    CHECK(memcmp(buf, "back", 4) == 0);

    /* On a stream nobody has a name to read from. */
    NetUnixAddr *from = NULL;
    CHECK_INT_EQ(net_unix_conn_write(c, bytes_of(hello, 5), &e), 5);
    CHECK_INT_EQ(net_unix_conn_read_from_unix(s, bytes_of(buf, 16), a, &from, &e), 5);
    CHECK(BURROW_OK(e));
    /* Linux gives no name at all, and the BSDs may give an empty one. */
    CHECK(from == NULL || from->name.len == 0);

    /* CloseWrite is the end of the stream for the other side. */
    CHECK(BURROW_OK(net_unix_conn_close_write(c)));
    CHECK_INT_EQ(net_unix_conn_read(s, bytes_of(buf, 16), &e), 0);
    CHECK(errors_is(e, io_eof));

    net_unix_conn_free(c);
    net_unix_conn_free(s);
    CHECK(BURROW_OK(net_unix_listener_close(l)));
    CHECK(!exists(addr.name));
    net_unix_listener_free(l);
    (void)os_remove_all(dir);
}

static void TestTheNamesAtEachEndAreGos(TestingT *t) {
    need_unix(t);
    Str dir = temp_dir(t);
    if (dir.len == 0)
        return;
    /* unixsock_test.go's TestUnixConnLocalAndRemoteNames: a dialer bound to
     * a path has that name, and one that is not has none. */
    for (int bound = 0; bound < 2; bound++) {
        NetUnixAddr addr = {path_in(dir, bound ? "s1" : "s0"), S("unix")};
        NetUnixAddr local = {path_in(dir, "c1"), S("unix")};
        Error e = BURROW_NO_ERROR;
        NetUnixListener *l = net_listen_unix(a, S("unix"), &addr, &e);
        if (l == NULL) {
            testing_t_errorf_v(t, "listen: %v", e);
            break;
        }
        NetUnixConn *c = net_dial_unix(a, S("unix"), bound ? &local : NULL, &addr, &e);
        NetUnixConn *s = c != NULL ? net_unix_listener_accept_unix(l, &e) : NULL;
        if (c == NULL || s == NULL) {
            testing_t_errorf_v(t, "dial or accept: %v", e);
        } else {
            char got[160];
            char want[160];
            const char *name = bound ? c_text(local.name, want, sizeof want) : UNNAMED;
            CHECK_STR_EQ(addr_text(net_unix_conn_local_addr(c), got, sizeof got), name);
            CHECK_STR_EQ(addr_text(net_unix_conn_remote_addr(s), got, sizeof got),
                         name);
            NetAddr ra = net_unix_conn_remote_addr(c);
            CHECK_STR_EQ(c_text(ra.vt->network(ra.data), got, sizeof got), "unix");
        }
        net_unix_conn_free(c);
        net_unix_conn_free(s);
        net_unix_listener_free(l);
    }
    (void)os_remove_all(dir);
}

static void TestClosingAListenerRemovesItsFileUnlessToldNot(TestingT *t) {
    need_unix(t);
    Str dir = temp_dir(t);
    if (dir.len == 0)
        return;
    NetUnixAddr addr = {path_in(dir, "s"), S("unix")};
    Error e = BURROW_NO_ERROR;
    NetUnixListener *l = net_listen_unix(a, S("unix"), &addr, &e);
    if (l == NULL) {
        testing_t_errorf_v(t, "listen: %v", e);
        (void)os_remove_all(dir);
        return;
    }
    net_unix_listener_set_unlink_on_close(l, false);
    CHECK(BURROW_OK(net_unix_listener_close(l)));
    CHECK(exists(addr.name));

    /* A second close is an error and does not remove the file either. */
    char buf[512];
    char want[512];
    char path[160];
    snprintf(want, sizeof want, "close unix %s: use of closed network connection",
             c_text(addr.name, path, sizeof path));
    CHECK_STR_EQ(text_of(net_unix_listener_close(l), buf, sizeof buf), want);
    CHECK(exists(addr.name));
    net_unix_listener_free(l);

    /* The path is taken while the file is there. */
    l = net_listen_unix(a, S("unix"), &addr, &e);
    CHECK(l == NULL);
    snprintf(want, sizeof want, "listen unix %s: bind: address already in use", path);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), want);
    net_unix_listener_free(l);
    (void)os_remove_all(dir);
}

static void TestAListenerWorksAsANetListener(TestingT *t) {
    need_unix(t);
    Str dir = temp_dir(t);
    if (dir.len == 0)
        return;
    NetUnixAddr addr = {path_in(dir, "s"), S("unix")};
    Error e = BURROW_NO_ERROR;
    NetUnixListener *ul = net_listen_unix(a, S("unix"), &addr, &e);
    if (ul == NULL) {
        testing_t_errorf_v(t, "listen: %v", e);
        (void)os_remove_all(dir);
        return;
    }
    NetListener l = net_unix_listener_as_listener(ul);
    NetUnixConn *c = net_dial_unix(a, S("unix"), NULL, &addr, &e);
    NetConn sc = l.vt->accept(l.data, &e);
    NetUnixConn *s = net_conn_as_unix_conn(sc);
    CHECK(c != NULL && s != NULL);
    if (c != NULL && s != NULL) {
        NetConn cc = net_unix_conn_as_conn(c);
        char ping[] = "ping";
        CHECK_INT_EQ(cc.vt->writer.write(cc.data, bytes_of(ping, 4), &e), 4);
        char buf[8] = {0};
        (void)net_unix_conn_set_read_deadline(s, soon());
        CHECK_INT_EQ(sc.vt->reader.read(sc.data, bytes_of(buf, 8), &e), 4);
        CHECK(memcmp(buf, "ping", 4) == 0);
        CHECK(net_conn_as_unix_conn(cc) == c);
    }
    CHECK(net_conn_as_unix_conn((NetConn){NULL, NULL}) == NULL);
    net_unix_conn_free(c);
    net_unix_conn_free(s);
    CHECK(BURROW_OK(l.vt->closer.close(l.data)));
    CHECK(!exists(addr.name));
    net_unix_listener_free(ul);
    (void)os_remove_all(dir);
}

static void TestAReadPastItsDeadlineTimesOut(TestingT *t) {
    need_unix(t);
    Str dir = temp_dir(t);
    if (dir.len == 0)
        return;
    NetUnixAddr addr = {path_in(dir, "s"), S("unix")};
    Error e = BURROW_NO_ERROR;
    NetUnixListener *l = net_listen_unix(a, S("unix"), &addr, &e);
    NetUnixConn *c = l != NULL ? net_dial_unix(a, S("unix"), NULL, &addr, &e) : NULL;
    NetUnixConn *s = c != NULL ? net_unix_listener_accept_unix(l, &e) : NULL;
    if (s == NULL) {
        testing_t_errorf_v(t, "listen, dial or accept: %v", e);
    } else {
        CHECK(BURROW_OK(net_unix_conn_set_read_deadline(
            s, time_add(time_now(), 30 * TIME_MILLISECOND))));
        char buf[8];
        CHECK_INT_EQ(net_unix_conn_read(s, bytes_of(buf, 8), &e), 0);
        CHECK(errors_is(e, os_err_deadline_exceeded));
        CHECK(net_error_timeout(e));
        char want[512];
        char got[512];
        char path[160];
        snprintf(want, sizeof want, "read unix %s->" UNNAMED ": i/o timeout",
                 c_text(addr.name, path, sizeof path));
        CHECK_STR_EQ(text_of(e, got, sizeof got), want);
        CHECK(BURROW_OK(net_unix_conn_set_read_buffer(s, 1 << 16)));
        CHECK(BURROW_OK(net_unix_conn_set_write_buffer(s, 1 << 16)));
    }
    net_unix_conn_free(c);
    net_unix_conn_free(s);
    net_unix_listener_free(l);
    (void)os_remove_all(dir);
}

/* -------------------------------------------------------------- datagrams */

static void TestDatagramsCarryTheirSendersName(TestingT *t) {
    need_unixgram(t);
    Str dir = temp_dir(t);
    if (dir.len == 0)
        return;
    NetUnixAddr ra = {path_in(dir, "r"), S("unixgram")};
    NetUnixAddr sa = {path_in(dir, "s"), S("unixgram")};
    Error e = BURROW_NO_ERROR;
    NetUnixConn *r = net_listen_unixgram(a, S("unixgram"), &ra, &e);
    NetUnixConn *s = net_listen_unixgram(a, S("unixgram"), &sa, &e);
    if (r == NULL || s == NULL) {
        testing_t_errorf_v(t, "listen: %v", e);
        net_unix_conn_free(r);
        net_unix_conn_free(s);
        (void)os_remove_all(dir);
        return;
    }
    (void)net_unix_conn_set_read_deadline(r, soon());
    CHECK(net_unix_conn_remote_addr(r).vt == NULL);

    char ping[] = "ping";
    CHECK_INT_EQ(net_unix_conn_write_to_unix(s, bytes_of(ping, 4), &ra, &e), 4);
    CHECK(BURROW_OK(e));
    char buf[16] = {0};
    NetUnixAddr *from = NULL;
    CHECK_INT_EQ(net_unix_conn_read_from_unix(r, bytes_of(buf, 16), a, &from, &e), 4);
    CHECK(BURROW_OK(e));
    CHECK(memcmp(buf, "ping", 4) == 0);
    char got[160];
    char want[160];
    CHECK(from != NULL);
    if (from != NULL) {
        CHECK_STR_EQ(c_text(from->name, got, sizeof got),
                     c_text(sa.name, want, sizeof want));
        CHECK_STR_EQ(c_text(net_unix_addr_network(from), got, sizeof got), "unixgram");

        /* And the answer goes back to it through WriteTo. */
        char pong[] = "pong";
        (void)net_unix_conn_set_read_deadline(s, soon());
        CHECK_INT_EQ(net_unix_conn_write_to(r, bytes_of(pong, 4),
                                            net_unix_addr_as_addr(from), &e),
                     4);
        NetAddr back = {NULL, NULL};
        CHECK_INT_EQ(net_unix_conn_read_from(s, bytes_of(buf, 16), a, &back, &e), 4);
        CHECK(memcmp(buf, "pong", 4) == 0);
        CHECK_STR_EQ(addr_text(back, got, sizeof got),
                     c_text(ra.name, want, sizeof want));
        net_unix_addr_free(a, from);
    }
    net_unix_conn_free(r);
    net_unix_conn_free(s);
    /* Only a listener removes its file, as in Go. */
    CHECK(exists(ra.name));
    (void)os_remove_all(dir);
}

static void TestAnUnboundSenderHasNoName(TestingT *t) {
    need_unixgram(t);
    Str dir = temp_dir(t);
    if (dir.len == 0)
        return;
    NetUnixAddr ra = {path_in(dir, "r"), S("unixgram")};
    Error e = BURROW_NO_ERROR;
    NetUnixConn *r = net_listen_unixgram(a, S("unixgram"), &ra, &e);
    NetUnixConn *c = r != NULL ? net_dial_unix(a, S("unixgram"), NULL, &ra, &e) : NULL;
    if (c == NULL) {
        testing_t_errorf_v(t, "listen or dial: %v", e);
    } else {
        (void)net_unix_conn_set_read_deadline(r, soon());
        char ping[] = "ping";
        CHECK_INT_EQ(net_unix_conn_write(c, bytes_of(ping, 4), &e), 4);
        char buf[16];
        NetAddr from = {NULL, NULL};
        CHECK_INT_EQ(net_unix_conn_read_from(r, bytes_of(buf, 16), a, &from, &e), 4);
        CHECK(BURROW_OK(e));
        char none[160];
        CHECK(from.vt == NULL || addr_text(from, none, sizeof none)[0] == '\0');
#if defined(BURROW_OS_LINUX) || defined(BURROW_OS_ANDROID)
        CHECK(from.vt == NULL);
#endif

        /* A dialed socket has its peer and takes no WriteTo. */
        char got[512];
        char want[512];
        char path[160];
        c_text(ra.name, path, sizeof path);
        CHECK_INT_EQ(net_unix_conn_write_to_unix(c, bytes_of(ping, 4), &ra, &e), 0);
        snprintf(want, sizeof want,
                 "write unixgram " UNNAMED "->%s: use of WriteTo with pre-connected "
                 "connection",
                 path);
        CHECK_STR_EQ(text_of(e, got, sizeof got), want);
        CHECK(errors_is(e, net_err_write_to_connected));
    }
    net_unix_conn_free(c);
    net_unix_conn_free(r);
    (void)os_remove_all(dir);
}

static void TestADatagramIsReadWholeOrCut(TestingT *t) {
    need_unixgram(t);
    Str dir = temp_dir(t);
    if (dir.len == 0)
        return;
    NetUnixAddr ra = {path_in(dir, "r"), S("unixgram")};
    Error e = BURROW_NO_ERROR;
    NetUnixConn *r = net_listen_unixgram(a, S("unixgram"), &ra, &e);
    NetUnixConn *c = r != NULL ? net_dial_unix(a, S("unixgram"), NULL, &ra, &e) : NULL;
    if (c == NULL) {
        testing_t_errorf_v(t, "listen or dial: %v", e);
    } else {
        (void)net_unix_conn_set_read_deadline(r, soon());
        char one[] = "abcdef";
        char two[] = "gh";
        CHECK_INT_EQ(net_unix_conn_write(c, bytes_of(one, 6), &e), 6);
        CHECK_INT_EQ(net_unix_conn_write(c, bytes_of(two, 2), &e), 2);
        char buf[4] = {0};
        CHECK_INT_EQ(net_unix_conn_read(r, bytes_of(buf, 4), &e), 4);
        CHECK(memcmp(buf, "abcd", 4) == 0);
        CHECK_INT_EQ(net_unix_conn_read(r, bytes_of(buf, 4), &e), 2);
        CHECK(memcmp(buf, "gh", 2) == 0);

        /* An empty datagram is read as nothing, and is not the end. */
        CHECK_INT_EQ(net_unix_conn_write(c, bytes_of(one, 0), &e), 0);
        CHECK(BURROW_OK(e));
        CHECK_INT_EQ(net_unix_conn_read(r, bytes_of(buf, 4), &e), 0);
        CHECK(BURROW_OK(e));
    }
    net_unix_conn_free(c);
    net_unix_conn_free(r);
    (void)os_remove_all(dir);
}

static void TestAPacketConnKeepsItsMessagesApart(TestingT *t) {
    need_unix(t);
#if !defined(BURROW_OS_LINUX) && !defined(BURROW_OS_ANDROID)
    testing_t_skip_v(t, "unixpacket is tested on Linux");
    return;
#endif
    Str dir = temp_dir(t);
    if (dir.len == 0)
        return;
    NetUnixAddr addr = {path_in(dir, "p"), S("unixpacket")};
    Error e = BURROW_NO_ERROR;
    NetUnixListener *l = net_listen_unix(a, S("unixpacket"), &addr, &e);
    NetUnixConn *c =
        l != NULL ? net_dial_unix(a, S("unixpacket"), NULL, &addr, &e) : NULL;
    NetUnixConn *s = c != NULL ? net_unix_listener_accept_unix(l, &e) : NULL;
    if (s == NULL) {
        testing_t_errorf_v(t, "listen, dial or accept: %v", e);
    } else {
        (void)net_unix_conn_set_read_deadline(s, soon());
        char got[160];
        CHECK_STR_EQ(c_text(net_unix_listener_addr(l).vt->network(
                                net_unix_listener_addr(l).data),
                            got, sizeof got),
                     "unixpacket");
        char ab[] = "ab";
        char cd[] = "cd";
        CHECK_INT_EQ(net_unix_conn_write(c, bytes_of(ab, 2), &e), 2);
        CHECK_INT_EQ(net_unix_conn_write(c, bytes_of(cd, 2), &e), 2);
        char buf[8] = {0};
        CHECK_INT_EQ(net_unix_conn_read(s, bytes_of(buf, 8), &e), 2);
        CHECK(memcmp(buf, "ab", 2) == 0);
        CHECK_INT_EQ(net_unix_conn_read(s, bytes_of(buf, 8), &e), 2);
        CHECK(memcmp(buf, "cd", 2) == 0);

        /* The other end going away is the end, as on a stream. */
        net_unix_conn_free(c);
        c = NULL;
        CHECK_INT_EQ(net_unix_conn_read(s, bytes_of(buf, 8), &e), 0);
        CHECK(errors_is(e, io_eof));
    }
    net_unix_conn_free(c);
    net_unix_conn_free(s);
    net_unix_listener_free(l);
    CHECK(!exists(addr.name));
    (void)os_remove_all(dir);
}

/* --------------------------------------------------------------- abstract */

static void TestAbstractNamesNeedNoFile(TestingT *t) {
    need_unix(t);
#if !defined(HAVE_ABSTRACT)
    testing_t_skip_v(t, "abstract names are Linux's");
#elif defined(BURROW_OS_COSMO)
    /* A bind to the empty name fails with EINVAL in a Cosmopolitan build, even
     * on Linux, so there is no autobind to test. */
    testing_t_skip_v(t, "Cosmopolitan does not autobind");
#else
    /* unixsock_linux_test.go's TestUnixAutobind: an empty name binds to a
     * fresh abstract one. */
    NetUnixAddr any = {BURROW_STR_EMPTY, S("unix")};
    Error e = BURROW_NO_ERROR;
    NetUnixListener *l = net_listen_unix(a, S("unix"), &any, &e);
    if (l == NULL) {
        testing_t_errorf_v(t, "listen: %v", e);
        return;
    }
    char name[160];
    addr_text(net_unix_listener_addr(l), name, sizeof name);
    CHECK(name[0] == '@' && strlen(name) > 1);

    /* Dialing that name, "@" and all, reaches it. */
    NetUnixAddr to = {str_from_cstr(name), S("unix")};
    NetUnixConn *c = net_dial_unix(a, S("unix"), NULL, &to, &e);
    NetUnixConn *s = c != NULL ? net_unix_listener_accept_unix(l, &e) : NULL;
    CHECK(s != NULL);
    if (s != NULL) {
        char got[160];
        CHECK_STR_EQ(addr_text(net_unix_conn_remote_addr(c), got, sizeof got), name);
        char hi[] = "hi";
        (void)net_unix_conn_set_read_deadline(s, soon());
        CHECK_INT_EQ(net_unix_conn_write(c, bytes_of(hi, 2), &e), 2);
        char buf[4];
        CHECK_INT_EQ(net_unix_conn_read(s, bytes_of(buf, 4), &e), 2);
    }
    net_unix_conn_free(c);
    net_unix_conn_free(s);
    CHECK(BURROW_OK(net_unix_listener_close(l)));
    net_unix_listener_free(l);
#endif
}

/* ----------------------------------------------------------------- errors */

static void TestTheErrorsReadTheWayGosDo(TestingT *t) {
    need_unix(t);
    char got[400];
    Error e = BURROW_NO_ERROR;
    NetUnixAddr x = {S("/x"), S("unix")};
    NetUnixAddr empty = {BURROW_STR_EMPTY, S("unix")};

    CHECK(net_dial_unix(a, S("unix5"), NULL, &x, &e) == NULL);
    CHECK_STR_EQ(text_of(e, got, sizeof got), "dial unix5 /x: unknown network unix5");
    CHECK(net_listen_unix(a, S("unixgram"), &x, &e) == NULL);
    CHECK_STR_EQ(text_of(e, got, sizeof got),
                 "listen unixgram /x: unknown network unixgram");
    CHECK(net_listen_unix(a, S("unix"), NULL, &e) == NULL);
    CHECK_STR_EQ(text_of(e, got, sizeof got), "listen unix: missing address");
    CHECK(net_listen_unixgram(a, S("unix"), &x, &e) == NULL);
    CHECK_STR_EQ(text_of(e, got, sizeof got), "listen unix /x: unknown network unix");
    CHECK(net_listen_unixgram(a, S("unixgram"), NULL, &e) == NULL);
    CHECK_STR_EQ(text_of(e, got, sizeof got), "listen unixgram: missing address");
    CHECK(net_dial_unix(a, S("unix"), NULL, NULL, &e) == NULL);
    CHECK_STR_EQ(text_of(e, got, sizeof got), "dial unix: missing address");

    /* An empty name is no address for a dial, and the error says so with
     * the empty name in it. */
    CHECK(net_dial_unix(a, S("unix"), NULL, &empty, &e) == NULL);
    CHECK_STR_EQ(text_of(e, got, sizeof got), "dial unix : missing address");

    /* Nothing is listening on a path that is not there. */
    NetUnixAddr gone = {S("/nonexistent/bx/s"), S("unix")};
    CHECK(net_dial_unix(a, S("unix"), NULL, &gone, &e) == NULL);
    CHECK_STR_EQ(text_of(e, got, sizeof got),
                 "dial unix /nonexistent/bx/s: connect: no such file or directory");

    /* A name longer than the system takes. */
    char lng[201];
    memset(lng, 'x', 200);
    lng[200] = 0;
    NetUnixAddr big = {str_from_cstr(lng), S("unix")};
    CHECK(net_listen_unix(a, S("unix"), &big, &e) == NULL);
    char want[400];
    snprintf(want, sizeof want, "listen unix %s: bind: invalid argument", lng);
    CHECK_STR_EQ(text_of(e, got, sizeof got), want);
}

static void TestTheWriteToErrorsReadTheWayGosDo(TestingT *t) {
    need_unixgram(t);
    Str dir = temp_dir(t);
    if (dir.len == 0)
        return;
    NetUnixAddr ra = {path_in(dir, "r"), S("unixgram")};
    Error e = BURROW_NO_ERROR;
    NetUnixConn *r = net_listen_unixgram(a, S("unixgram"), &ra, &e);
    if (r == NULL) {
        testing_t_errorf_v(t, "listen: %v", e);
        (void)os_remove_all(dir);
        return;
    }
    char path[160];
    c_text(ra.name, path, sizeof path);
    char got[512];
    char want[512];
    char p[] = "p";

    CHECK_INT_EQ(net_unix_conn_write_to_unix(r, bytes_of(p, 1), NULL, &e), 0);
    snprintf(want, sizeof want, "write unixgram %s: missing address", path);
    CHECK_STR_EQ(text_of(e, got, sizeof got), want);

    /* An address for another network is not one this socket can send to. */
    NetUnixAddr stream = {ra.name, S("unix")};
    CHECK_INT_EQ(net_unix_conn_write_to_unix(r, bytes_of(p, 1), &stream, &e), 0);
    snprintf(want, sizeof want,
             "write unixgram %s->%s: address family not supported by protocol", path,
             path);
    CHECK(strncmp(text_of(e, got, sizeof got), want, strlen(want)) == 0);

    /* WriteTo takes only a UnixAddr. */
    CHECK_INT_EQ(net_unix_conn_write_to(r, bytes_of(p, 1), (NetAddr){NULL, NULL}, &e),
                 0);
    snprintf(want, sizeof want, "write unixgram %s: invalid argument", path);
    CHECK_STR_EQ(text_of(e, got, sizeof got), want);

    /* Nobody at the name. */
    NetUnixAddr gone = {path_in(dir, "gone"), S("unixgram")};
    char gpath[160];
    c_text(gone.name, gpath, sizeof gpath);
    CHECK_INT_EQ(net_unix_conn_write_to_unix(r, bytes_of(p, 1), &gone, &e), 0);
    snprintf(want, sizeof want,
             "write unixgram %s->%s: sendto: no such file or directory", path, gpath);
    CHECK_STR_EQ(text_of(e, got, sizeof got), want);

    net_unix_conn_free(r);
    (void)os_remove_all(dir);
}

static void TestANilConnIsAnInvalidArgument(TestingT *t) {
    (void)t;
    char buf[64];
    Error e = BURROW_NO_ERROR;
    CHECK_STR_EQ(text_of(net_unix_conn_close(NULL), buf, sizeof buf), EINVAL_TEXT);
    CHECK_INT_EQ(net_unix_conn_read(NULL, (Slice){0}, &e), 0);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), EINVAL_TEXT);
    NetUnixAddr *from = NULL;
    CHECK_INT_EQ(net_unix_conn_read_from_unix(NULL, (Slice){0}, a, &from, &e), 0);
    CHECK(from == NULL);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), EINVAL_TEXT);
    CHECK(net_unix_listener_accept_unix(NULL, &e) == NULL);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), EINVAL_TEXT);
    CHECK_STR_EQ(text_of(net_unix_listener_close(NULL), buf, sizeof buf), EINVAL_TEXT);
    CHECK(net_unix_conn_local_addr(NULL).vt == NULL);
    CHECK(net_unix_listener_addr(NULL).vt == NULL);
    net_unix_conn_free(NULL);
    net_unix_listener_free(NULL);
}

/* --------------------------------------------------------------- UnixAddr */

static void TestAUnixAddrIsItsName(TestingT *t) {
    (void)t;
    char buf[64];
    NetUnixAddr x = {S("/tmp/x.sock"), S("unixgram")};
    CHECK_STR_EQ(c_text(net_unix_addr_string(&x, a), buf, sizeof buf), "/tmp/x.sock");
    CHECK_STR_EQ(c_text(net_unix_addr_string(NULL, a), buf, sizeof buf), "<nil>");
    CHECK_STR_EQ(c_text(net_unix_addr_network(&x), buf, sizeof buf), "unixgram");
    CHECK(net_unix_addr_as_addr(NULL).vt == NULL);
    NetAddr na = net_unix_addr_as_addr(&x);
    CHECK(na.vt != NULL && na.vt->self_type == TYPE_NET_UNIX_ADDR);

    /* ResolveUnixAddr takes the three networks and nothing else. */
    const char *nets[] = {"unix", "unixgram", "unixpacket"};
    for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++) {
        Error e = BURROW_NO_ERROR;
        NetUnixAddr *r =
            net_resolve_unix_addr(a, str_from_cstr(nets[i]), S("@abstract"), &e);
        CHECK(r != NULL && BURROW_OK(e));
        if (r != NULL) {
            CHECK_STR_EQ(c_text(r->name, buf, sizeof buf), "@abstract");
            CHECK_STR_EQ(c_text(r->net, buf, sizeof buf), nets[i]);
            net_unix_addr_free(a, r);
        }
    }
    Error e = BURROW_NO_ERROR;
    CHECK(net_resolve_unix_addr(a, S("tcp"), S("/x"), &e) == NULL);
    CHECK_STR_EQ(text_of(e, buf, sizeof buf), "unknown network tcp");
}

static void setup(void) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
}

#define TESTS(X)                                                                       \
    X(TestAStreamListenerAcceptsAndTalks)                                              \
    X(TestTheNamesAtEachEndAreGos)                                                     \
    X(TestClosingAListenerRemovesItsFileUnlessToldNot)                                 \
    X(TestAListenerWorksAsANetListener)                                                \
    X(TestAReadPastItsDeadlineTimesOut)                                                \
    X(TestDatagramsCarryTheirSendersName)                                              \
    X(TestAnUnboundSenderHasNoName)                                                    \
    X(TestADatagramIsReadWholeOrCut)                                                   \
    X(TestAPacketConnKeepsItsMessagesApart)                                            \
    X(TestAbstractNamesNeedNoFile)                                                     \
    X(TestTheErrorsReadTheWayGosDo)                                                    \
    X(TestTheWriteToErrorsReadTheWayGosDo)                                             \
    X(TestANilConnIsAnInvalidArgument)                                                 \
    X(TestAUnixAddrIsItsName)

static int net_unix_main(TestingM *m) {
    setup();
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(net_unix_main, TESTS)
