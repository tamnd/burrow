/* Dialer, ListenConfig and the Resolve functions.
 *
 * The cases follow Go's src/net/dial_test.go, and the Resolve tables come
 * from tcpsock_test.go, udpsock_test.go and iprawsock_test.go. Go swaps in a
 * fake lookup for "localhost" there, which this cannot, so the rows that
 * name localhost are left out and every address here is a literal.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/netpoll.h"
#include "burrow/os.h"
#include "burrow/proc.h"
#include "burrow/strings.h"
#include "burrow/sync.h"
#include "burrow/syscall.h"
#include "burrow/time.h"

#include "../src/net/internal.h"
#include "check.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_SOCKETS 1
#endif

/* Windows has stream Unix sockets only, and only Linux has unixpacket. */
#if defined(HAVE_SOCKETS) && !defined(BURROW_OS_WINDOWS)
#define HAVE_UNIXGRAM 1
#endif
#if defined(HAVE_SOCKETS) && (defined(BURROW_OS_LINUX) ||                              \
                              defined(BURROW_OS_ANDROID) || defined(BURROW_OS_COSMO))
#define HAVE_UNIXPACKET 1
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

static Str addr_str(NetAddr addr) {
    if (addr.vt == NULL)
        return S("<nil>");
    return addr.vt->string(addr.data, a);
}

static Str listener_addr(NetListener l) {
    return addr_str(l.vt->addr(l.data));
}

static Str port_of(Str hostport) {
    Str port = BURROW_STR_EMPTY;
    (void)net_split_host_port(hostport, &port, NULL);
    return port;
}

/* s with a prefix, in the arena. */
static Str concat(const char *prefix, Str s) {
    Int n = (Int)strlen(prefix);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)(n + s.len), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    memcpy(p, prefix, (size_t)n);
    if (s.len > 0)
        memcpy(p + n, s.p, (size_t)s.len);
    return str_from_bytes(p, n + s.len);
}

/* ---------------------------------------------------------------- servers */

/* Whether this machine can listen on network, which is testableNetwork
 * without the ones that need privileges. */
static bool testable(Str network) {
#if !defined(HAVE_SOCKETS)
    (void)network;
    return false;
#else
    if (str_eq(network, S("unixgram"))) {
#if defined(HAVE_UNIXGRAM)
        return true;
#else
        return false;
#endif
    }
    if (str_eq(network, S("unixpacket"))) {
#if defined(HAVE_UNIXPACKET)
        return true;
#else
        return false;
#endif
    }
    if (str_eq(network, S("tcp6")) || str_eq(network, S("udp6"))) {
        Error e = BURROW_NO_ERROR;
        NetListener l = net_listen(heap_allocator(), S("tcp6"), S("[::1]:0"), &e);
        if (BURROW_FAILED(e))
            return false;
        net_listener_free(l);
    }
    return true;
#endif
}

/* A fresh name for a Unix socket, short enough for macOS, in a directory the
 * caller removes. */
static Str unix_path(TestingT *t, Str *dir) {
    Error e = BURROW_NO_ERROR;
    *dir = os_mkdir_temp(a, S(""), S("bxd"), &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "MkdirTemp: %v", e);
    return fmt_sprintf_v(a, "%s/s", *dir);
}

/* newLocalListener */
static NetListener local_listener(TestingT *t, Str network, Str *dir) {
    *dir = BURROW_STR_EMPTY;
    Str address = S("127.0.0.1:0");
    if (str_eq(network, S("tcp6")))
        address = S("[::1]:0");
    else if (str_eq(network, S("unix")) || str_eq(network, S("unixpacket")))
        address = unix_path(t, dir);
    Error e = BURROW_NO_ERROR;
    NetListener l = net_listen(heap_allocator(), network, address, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "Listen(%s, %s): %v", network, address, e);
    return l;
}

/* newLocalPacketListener, as a NetConn, since ListenPacket is still to come. */
static NetConn local_packet_listener(TestingT *t, Str network, Str *dir) {
    NetConn none = {NULL, NULL};
    *dir = BURROW_STR_EMPTY;
    Error e = BURROW_NO_ERROR;
    if (str_eq(network, S("unixgram"))) {
        NetUnixAddr ua = {unix_path(t, dir), S("unixgram")};
        NetUnixConn *c = net_listen_unixgram(heap_allocator(), network, &ua, &e);
        if (c == NULL) {
            testing_t_errorf_v(t, "ListenUnixgram: %v", e);
            return none;
        }
        return net_unix_conn_as_conn(c);
    }
    NetUDPAddr *la = net_resolve_udp_addr(
        a, network, str_eq(network, S("udp6")) ? S("[::1]:0") : S("127.0.0.1:0"), &e);
    NetUDPConn *c =
        la != NULL ? net_listen_udp(heap_allocator(), network, la, &e) : NULL;
    if (c == NULL) {
        testing_t_errorf_v(t, "ListenUDP(%s): %v", network, e);
        return none;
    }
    return net_udp_conn_as_conn(c);
}

static void remove_dir(Str dir) {
    if (dir.len > 0)
        (void)os_remove_all(dir);
}

/* A server that accepts connections and closes them, until its listener
 * is closed. */
typedef struct Closer {
    NetListener l;
    SyncWaitGroup wg;
} Closer;

static void closer_job(void *env) {
    Closer *s = env;
    for (;;) {
        Error e = BURROW_NO_ERROR;
        NetConn c = s->l.vt->accept(s->l.data, &e);
        if (BURROW_FAILED(e))
            break;
        net_conn_free(c);
    }
    sync_wait_group_done(&s->wg);
}

static void closer_start(Closer *s) {
    sync_wait_group_add(&s->wg, 1);
    if (!go(BURROW_FN(Func, closer_job, s)))
        sync_wait_group_done(&s->wg);
}

static void closer_stop(Closer *s) {
    (void)s->l.vt->closer.close(s->l.data);
    sync_wait_group_wait(&s->wg);
    net_listener_free(s->l);
}

/* --------------------------------------------------------------- the tests */

static void TestDialLocal(TestingT *t) {
    need_sockets(t);
    Str dir;
    NetListener l = local_listener(t, S("tcp"), &dir);
    if (l.vt == NULL)
        return;
    Str port = port_of(listener_addr(l));
    Error e = BURROW_NO_ERROR;
    NetConn c =
        net_dial(heap_allocator(), S("tcp"), net_join_host_port(a, S(""), port), &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "Dial: %v", e);
    net_conn_free(c);
    net_listener_free(l);
}

static void TestDialerPartialDeadline(TestingT *t) {
    const int64_t now = 1000 * TIME_SECOND;
    const int64_t no_deadline = 0;
    struct {
        int64_t now;
        int64_t deadline;
        Int addrs;
        int64_t expect_deadline;
        bool expect_timeout;
    } cases[] = {
        /* Regular division. */
        {now, now + 12 * TIME_SECOND, 1, now + 12 * TIME_SECOND, false},
        {now, now + 12 * TIME_SECOND, 2, now + 6 * TIME_SECOND, false},
        {now, now + 12 * TIME_SECOND, 3, now + 4 * TIME_SECOND, false},
        /* Bump against the 2-second sane minimum. */
        {now, now + 12 * TIME_SECOND, 999, now + 2 * TIME_SECOND, false},
        /* Total available is now below the sane minimum. */
        {now, now + 1900 * TIME_MILLISECOND, 999, now + 1900 * TIME_MILLISECOND, false},
        /* Null deadline. */
        {now, no_deadline, 1, no_deadline, false},
        /* Step the clock forward and cross the deadline. */
        {now - 1 * TIME_MILLISECOND, now, 1, now, false},
        {now + 0 * TIME_MILLISECOND, now, 1, no_deadline, true},
        {now + 1 * TIME_MILLISECOND, now, 1, no_deadline, true},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int64_t got = -1;
        Error e = burrow__net_partial_deadline(cases[i].now, cases[i].deadline,
                                               cases[i].addrs, &got);
        bool timeout = errors_is(e, burrow__net_err_timeout);
        if (timeout != cases[i].expect_timeout || (BURROW_FAILED(e) && !timeout))
            testing_t_errorf_v(t, "#%d: got %v", (int)i, e);
        if (got != cases[i].expect_deadline)
            testing_t_errorf_v(t, "#%d: got %d; want %d", (int)i, (Int)got,
                               (Int)cases[i].expect_deadline);
    }
}

static void TestDialerLocalAddr(TestingT *t) {
    need_sockets(t);
    if (!testable(S("tcp6"))) {
        testing_t_skip_v(t, "both IPv4 and IPv6 are required");
        return;
    }
    static Byte any4[4] = {0, 0, 0, 0};
    static Byte any16[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0};
    static Byte un6[16] = {0};
    static Byte lo4[4] = {127, 0, 0, 1};
    static Byte lo16[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 127, 0, 0, 1};
    static Byte lo6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    enum {
        NONE,
        TCP_NIL,
        TCP_ANY16,
        TCP_ANY4,
        TCP_UN6,
        TCP_LO4,
        TCP_LO16,
        TCP_LO6,
        UDP,
        UNIX
    };
    NetTCPAddr tcp[] = {
        {slice_nil(TYPE_BYTE), 0, BURROW_STR_EMPTY},
        {slice_nil(TYPE_BYTE), 0, BURROW_STR_EMPTY},
        {slice_from(any16, 16, 16, TYPE_BYTE), 0, BURROW_STR_EMPTY},
        {slice_from(any4, 4, 4, TYPE_BYTE), 0, BURROW_STR_EMPTY},
        {slice_from(un6, 16, 16, TYPE_BYTE), 0, BURROW_STR_EMPTY},
        {slice_from(lo4, 4, 4, TYPE_BYTE), 0, BURROW_STR_EMPTY},
        {slice_from(lo16, 16, 16, TYPE_BYTE), 0, BURROW_STR_EMPTY},
        {slice_from(lo6, 16, 16, TYPE_BYTE), 0, BURROW_STR_EMPTY},
    };
    NetUDPAddr udp = {slice_nil(TYPE_BYTE), 0, BURROW_STR_EMPTY};
    NetUnixAddr ux = {BURROW_STR_EMPTY, BURROW_STR_EMPTY};
    struct {
        const char *network;
        int laddr;
        bool v6;
        bool fails;
    } cases[] = {
        {"tcp4", NONE, false, false},      {"tcp4", TCP_NIL, false, false},
        {"tcp4", TCP_ANY16, false, false}, {"tcp4", TCP_ANY4, false, false},
        {"tcp4", TCP_UN6, false, true},    {"tcp4", TCP_LO4, false, false},
        {"tcp4", TCP_LO16, false, false},  {"tcp4", TCP_LO6, false, true},
        {"tcp4", UDP, false, true},        {"tcp4", UNIX, false, true},

        {"tcp6", NONE, true, false},       {"tcp6", TCP_NIL, true, false},
        {"tcp6", TCP_ANY16, true, false},  {"tcp6", TCP_ANY4, true, false},
        {"tcp6", TCP_UN6, true, false},    {"tcp6", TCP_LO4, true, true},
        {"tcp6", TCP_LO16, true, true},    {"tcp6", TCP_LO6, true, false},
        {"tcp6", UDP, true, true},         {"tcp6", UNIX, true, true},

        {"tcp", NONE, false, false},       {"tcp", TCP_NIL, false, false},
        {"tcp", TCP_ANY16, false, false},  {"tcp", TCP_ANY4, false, false},
        {"tcp", TCP_LO4, false, false},    {"tcp", TCP_LO16, false, false},
        {"tcp", TCP_LO6, false, true},     {"tcp", UDP, false, true},
        {"tcp", UNIX, false, true},

        {"tcp", NONE, true, false},        {"tcp", TCP_NIL, true, false},
        {"tcp", TCP_ANY16, true, false},   {"tcp", TCP_ANY4, true, false},
        {"tcp", TCP_UN6, true, false},     {"tcp", TCP_LO4, true, true},
        {"tcp", TCP_LO16, true, true},     {"tcp", TCP_LO6, true, false},
        {"tcp", UDP, true, true},          {"tcp", UNIX, true, true},
    };

    Closer servers[2];
    memset(servers, 0, sizeof servers);
    Str addrs[2];
    for (int i = 0; i < 2; i++) {
        Str dir;
        servers[i].l = local_listener(t, i == 0 ? S("tcp4") : S("tcp6"), &dir);
        if (servers[i].l.vt == NULL)
            return;
        addrs[i] = listener_addr(servers[i].l);
        closer_start(&servers[i]);
    }
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        NetDialer d;
        memset(&d, 0, sizeof d);
        int k = cases[i].laddr;
        if (k == UDP)
            d.local_addr = net_udp_addr_as_addr(&udp);
        else if (k == UNIX)
            d.local_addr = net_unix_addr_as_addr(&ux);
        else if (k != NONE)
            d.local_addr = net_tcp_addr_as_addr(&tcp[k - TCP_NIL]);
        Str network = str_from_cstr(cases[i].network);
        Error e = BURROW_NO_ERROR;
        NetConn c = net_dialer_dial(&d, heap_allocator(), network,
                                    addrs[cases[i].v6 ? 1 : 0], &e);
        if (BURROW_FAILED(e) != cases[i].fails)
            testing_t_errorf_v(t, "#%d %s %s->%s: got %v", (int)i, network,
                               addr_str(d.local_addr), addrs[cases[i].v6 ? 1 : 0], e);
        if (BURROW_FAILED(e) && errors_as(e, TYPE_NET_OP_ERROR) == NULL)
            testing_t_errorf_v(t, "#%d: %v is not an OpError", (int)i, e);
        net_conn_free(c);
    }
    closer_stop(&servers[0]);
    closer_stop(&servers[1]);
}

/* TestCancelAfterDial: echoes back the first line of each connection. */
typedef struct LineEcho {
    NetListener l;
    SyncWaitGroup wg;
    Error err;
} LineEcho;

static void line_echo_job(void *env) {
    LineEcho *s = env;
    for (;;) {
        Error e = BURROW_NO_ERROR;
        NetConn c = s->l.vt->accept(s->l.data, &e);
        if (BURROW_FAILED(e))
            break;
        char line[64];
        Int n = 0;
        while (n < (Int)sizeof line) {
            Int m = c.vt->reader.read(c.data, bytes_of(line + n, 1), &e);
            n += m;
            if (BURROW_FAILED(e) || (m == 1 && line[n - 1] == '\n'))
                break;
        }
        if (BURROW_OK(e))
            (void)c.vt->writer.write(c.data, bytes_of(line, n), &e);
        if (BURROW_FAILED(e))
            s->err = e;
        net_conn_free(c);
    }
    sync_wait_group_done(&s->wg);
}

static void TestCancelAfterDial(TestingT *t) {
    need_sockets(t);
    if (testing_short()) {
        testing_t_skip_v(t, "avoiding time.Sleep");
        return;
    }
    LineEcho s;
    memset(&s, 0, sizeof s);
    Str dir;
    s.l = local_listener(t, S("tcp"), &dir);
    if (s.l.vt == NULL)
        return;
    Str address = listener_addr(s.l);
    sync_wait_group_add(&s.wg, 1);
    if (!go(BURROW_FN(Func, line_echo_job, &s)))
        sync_wait_group_done(&s.wg);

    /* This bug manifested about 50% of the time, so try it a few times. */
    for (int i = 0; i < 10; i++) {
        Chan *cancel = chan_make(heap_allocator(), TYPE_INT, 0);
        NetDialer d;
        memset(&d, 0, sizeof d);
        d.cancel = cancel;
        Error e = BURROW_NO_ERROR;
        NetConn c = net_dialer_dial(&d, heap_allocator(), S("tcp"), address, &e);

        /* Immediately after dialing, request cancellation and sleep. Before
         * Go's issue 15078 was fixed, this would cause subsequent operations
         * to fail with an i/o timeout roughly 50% of the time. */
        chan_close(cancel);
        time_sleep(10 * TIME_MILLISECOND);
        chan_free(cancel);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "Dial: %v", e);
            break;
        }

        /* Send some data to confirm that the connection is still alive. */
        char message[] = "echo!\n";
        Int len = (Int)sizeof message - 1;
        (void)c.vt->writer.write(c.data, bytes_of(message, len), &e);
        if (BURROW_FAILED(e))
            testing_t_errorf_v(t, "Write: %v", e);

        /* The server should echo the line, and close the connection. */
        char got[16];
        Int n = io_read_full(net_conn_as_io_reader(c), bytes_of(got, len), &e);
        if (BURROW_FAILED(e) || n != len || memcmp(got, message, (size_t)len) != 0)
            testing_t_errorf_v(t, "got %d bytes, %v; want %d", n, e, len);
        e = BURROW_NO_ERROR;
        (void)c.vt->reader.read(c.data, bytes_of(got, 1), &e);
        if (!errors_is(e, io_eof))
            testing_t_errorf_v(t, "got %v; want EOF", e);
        net_conn_free(c);
    }
    (void)s.l.vt->closer.close(s.l.data);
    sync_wait_group_wait(&s.wg);
    if (BURROW_FAILED(s.err))
        testing_t_errorf_v(t, "server: %v", s.err);
    net_listener_free(s.l);
}

/* Go's issue 18806: it should always be possible to dial a listener's
 * address when it listened on ":n", even on a machine that can bind on "::"
 * but not connect to it. Go listens on tcp4 localhost and dials the text a
 * dual-stack listener would have reported. */
static void TestDialListenerAddr(TestingT *t) {
    need_sockets(t);
    Error e = BURROW_NO_ERROR;
    NetListener l = net_listen(heap_allocator(), S("tcp4"), S("127.0.0.1:0"), &e);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "Listen: %v", e);
        return;
    }
    Str dial_addr = concat("[::]:", port_of(listener_addr(l)));
    NetConn c = net_dial(heap_allocator(), S("tcp4"), dial_addr, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "Dial(\"tcp4\", %s): %v", dial_addr, e);
    net_conn_free(c);
    net_listener_free(l);
}

/* controlOnConnSetup, which wants the network a socket was made for, never
 * the ones that leave the family open, and a descriptor it can reach. */
static void note_fd(void *env, Uintptr fd) {
    *(Uintptr *)env = fd;
}

static Error control_on_conn_setup(void *env, Str network, Str address,
                                   SyscallRawConn c) {
    int *calls = env;
    (void)address;
    if (str_eq(network, S("tcp")) || str_eq(network, S("udp")) ||
        str_eq(network, S("ip")))
        return errors_new(error_allocator(), concat("ambiguous network: ", network));
    Uintptr fd = (Uintptr)-1;
    Error e = c.vt->control(c.data, BURROW_FN(SyscallFdFunc, note_fd, &fd));
    if (BURROW_FAILED(e))
        return e;
    if (fd == (Uintptr)-1)
        return errors_new(error_allocator(), S("Control never called back"));
    if (calls != NULL)
        (*calls)++;
    return BURROW_NO_ERROR;
}

static void TestDialerControl(TestingT *t) {
    need_sockets(t);
    const char *streams[] = {"tcp", "tcp4", "tcp6", "unix", "unixpacket"};
    for (size_t i = 0; i < sizeof streams / sizeof streams[0]; i++) {
        Str network = str_from_cstr(streams[i]);
        if (!testable(network))
            continue;
        Str dir;
        NetListener l = local_listener(t, network, &dir);
        if (l.vt == NULL)
            continue;
        int calls = 0;
        NetDialer d;
        memset(&d, 0, sizeof d);
        d.control = BURROW_FN(NetDialerControl, control_on_conn_setup, &calls);
        Error e = BURROW_NO_ERROR;
        NetConn c =
            net_dialer_dial(&d, heap_allocator(), network, listener_addr(l), &e);
        if (BURROW_FAILED(e) || calls != 1)
            testing_t_errorf_v(t, "%s: %v, %d calls", network, e, calls);
        net_conn_free(c);
        net_listener_free(l);
        remove_dir(dir);
    }
    const char *packets[] = {"udp", "udp4", "udp6", "unixgram"};
    for (size_t i = 0; i < sizeof packets / sizeof packets[0]; i++) {
        Str network = str_from_cstr(packets[i]);
        if (!testable(network))
            continue;
        Str dir;
        NetConn c1 = local_packet_listener(t, network, &dir);
        if (c1.vt == NULL)
            continue;
        int calls = 0;
        NetDialer d;
        memset(&d, 0, sizeof d);
        d.control = BURROW_FN(NetDialerControl, control_on_conn_setup, &calls);
        Error e = BURROW_NO_ERROR;
        NetConn c2 = net_dialer_dial(&d, heap_allocator(), network,
                                     addr_str(c1.vt->local_addr(c1.data)), &e);
        if (BURROW_FAILED(e) || calls != 1)
            testing_t_errorf_v(t, "%s: %v, %d calls", network, e, calls);
        net_conn_free(c2);
        net_conn_free(c1);
        remove_dir(dir);
    }
}

static Int id_key = 0;
#define ID_KEY BURROW_ANY(TYPE_INT, &id_key)

typedef struct IdSeen {
    Int id;
} IdSeen;

static Error control_context(void *env, Context ctx, Str network, Str address,
                             SyscallRawConn c) {
    IdSeen *seen = env;
    Any v = context_value(ctx, ID_KEY);
    const Int *id = any_assert(v, TYPE_INT);
    seen->id = id != NULL ? *id : -1;
    return control_on_conn_setup(NULL, network, address, c);
}

static void TestDialerControlContext(TestingT *t) {
    need_sockets(t);
    const char *streams[] = {"tcp", "tcp4", "tcp6", "unix", "unixpacket"};
    for (size_t i = 0; i < sizeof streams / sizeof streams[0]; i++) {
        Str network = str_from_cstr(streams[i]);
        if (!testable(network))
            continue;
        Str dir;
        NetListener l = local_listener(t, network, &dir);
        if (l.vt == NULL)
            continue;
        IdSeen seen = {0};
        NetDialer d;
        memset(&d, 0, sizeof d);
        d.control_context = BURROW_FN(NetDialerControlContext, control_context, &seen);
        Int id = (Int)i + 1;
        Context ctx = context_with_value(a, context_background(), ID_KEY,
                                         BURROW_ANY(TYPE_INT, &id));
        Error e = BURROW_NO_ERROR;
        NetConn c = net_dialer_dial_context(&d, heap_allocator(), ctx, network,
                                            listener_addr(l), &e);
        if (BURROW_FAILED(e))
            testing_t_errorf_v(t, "%s: %v", network, e);
        if (seen.id != id)
            testing_t_errorf_v(t, "%s: got id %d, want %d", network, seen.id, id);
        net_conn_free(c);
        net_listener_free(l);
        remove_dir(dir);
    }
}

static void TestDialContext(TestingT *t) {
    need_sockets(t);
    const char *streams[] = {"tcp", "tcp4", "tcp6", "unix", "unixpacket"};
    for (size_t i = 0; i < sizeof streams / sizeof streams[0]; i++) {
        Str network = str_from_cstr(streams[i]);
        if (!testable(network))
            continue;
        Str dir;
        NetListener l = local_listener(t, network, &dir);
        if (l.vt == NULL)
            continue;
        IdSeen seen = {0};
        NetDialer d;
        memset(&d, 0, sizeof d);
        d.control_context = BURROW_FN(NetDialerControlContext, control_context, &seen);
        Int id = (Int)i + 1;
        Context ctx = context_with_value(a, context_background(), ID_KEY,
                                         BURROW_ANY(TYPE_INT, &id));
        Error e = BURROW_NO_ERROR;
        NetConn c = {NULL, NULL};
        if (strings_has_prefix(network, S("tcp"))) {
            NetipAddrPort raddr = netip_parse_addr_port(listener_addr(l), &e);
            NetipAddrPort zero;
            memset(&zero, 0, sizeof zero);
            NetTCPConn *tc = BURROW_OK(e)
                                 ? net_dialer_dial_tcp(&d, heap_allocator(), ctx,
                                                       network, zero, raddr, &e)
                                 : NULL;
            if (tc != NULL)
                c = net_tcp_conn_as_conn(tc);
        } else {
            NetUnixAddr *raddr =
                net_resolve_unix_addr(a, network, listener_addr(l), &e);
            NetUnixConn *uc = raddr != NULL
                                  ? net_dialer_dial_unix(&d, heap_allocator(), ctx,
                                                         network, NULL, raddr, &e)
                                  : NULL;
            if (uc != NULL)
                c = net_unix_conn_as_conn(uc);
        }
        if (BURROW_FAILED(e))
            testing_t_errorf_v(t, "%s: %v", network, e);
        else if (seen.id != id)
            testing_t_errorf_v(t, "%s: got id %d, want %d", network, seen.id, id);
        net_conn_free(c);
        net_listener_free(l);
        remove_dir(dir);
    }
    const char *packets[] = {"udp", "udp4", "udp6", "unixgram"};
    for (size_t i = 0; i < sizeof packets / sizeof packets[0]; i++) {
        Str network = str_from_cstr(packets[i]);
        if (!testable(network))
            continue;
        Str dir;
        NetConn c1 = local_packet_listener(t, network, &dir);
        if (c1.vt == NULL)
            continue;
        IdSeen seen = {0};
        NetDialer d;
        memset(&d, 0, sizeof d);
        d.control_context = BURROW_FN(NetDialerControlContext, control_context, &seen);
        Int id = (Int)i + 1;
        Context ctx = context_with_value(a, context_background(), ID_KEY,
                                         BURROW_ANY(TYPE_INT, &id));
        Str laddr = addr_str(c1.vt->local_addr(c1.data));
        Error e = BURROW_NO_ERROR;
        NetConn c2 = {NULL, NULL};
        if (strings_has_prefix(network, S("udp"))) {
            NetipAddrPort raddr = netip_parse_addr_port(laddr, &e);
            NetipAddrPort zero;
            memset(&zero, 0, sizeof zero);
            NetUDPConn *uc = BURROW_OK(e)
                                 ? net_dialer_dial_udp(&d, heap_allocator(), ctx,
                                                       network, zero, raddr, &e)
                                 : NULL;
            if (uc != NULL)
                c2 = net_udp_conn_as_conn(uc);
        } else {
            NetUnixAddr *raddr = net_resolve_unix_addr(a, network, laddr, &e);
            NetUnixConn *xc = raddr != NULL
                                  ? net_dialer_dial_unix(&d, heap_allocator(), ctx,
                                                         network, NULL, raddr, &e)
                                  : NULL;
            if (xc != NULL)
                c2 = net_unix_conn_as_conn(xc);
        }
        if (BURROW_FAILED(e))
            testing_t_errorf_v(t, "%s: %v", network, e);
        else if (seen.id != id)
            testing_t_errorf_v(t, "%s: got id %d, want %d", network, seen.id, id);
        net_conn_free(c2);
        net_conn_free(c1);
        remove_dir(dir);
    }
}

/* TestListenConfigControl, from listen_test.go. */
static void TestListenConfigControl(TestingT *t) {
    need_sockets(t);
    const char *streams[] = {"tcp", "tcp4", "tcp6", "unix", "unixpacket"};
    for (size_t i = 0; i < sizeof streams / sizeof streams[0]; i++) {
        Str network = str_from_cstr(streams[i]);
        if (!testable(network))
            continue;
        Str dir = BURROW_STR_EMPTY;
        Str address = S("127.0.0.1:0");
        if (str_eq(network, S("tcp6")))
            address = S("[::1]:0");
        else if (strings_has_prefix(network, S("unix")))
            address = unix_path(t, &dir);
        int calls = 0;
        NetListenConfig lc;
        memset(&lc, 0, sizeof lc);
        lc.control = BURROW_FN(NetListenConfigControl, control_on_conn_setup, &calls);
        Error e = BURROW_NO_ERROR;
        NetListener l = net_listen_config_listen(
            &lc, heap_allocator(), context_background(), network, address, &e);
        if (BURROW_FAILED(e) || calls != 1)
            testing_t_errorf_v(t, "%s: %v, %d calls", network, e, calls);
        net_listener_free(l);
        remove_dir(dir);
    }
}

static void TestADialErrorIsAnOpError(TestingT *t) {
    need_sockets(t);
    Error e = BURROW_NO_ERROR;
    NetConn c = net_dial(heap_allocator(), S("tcp"), S(""), &e);
    CHECK(c.vt == NULL);
    char buf[128];
    CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf), "dial tcp: missing address");
    c = net_dial(heap_allocator(), S("tcp"), S("127.0.0.1"), &e);
    CHECK(c.vt == NULL);
    CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf),
                 "dial tcp: address 127.0.0.1: missing port in address");
    c = net_dial(heap_allocator(), S("bogus"), S("127.0.0.1:1"), &e);
    CHECK(c.vt == NULL);
    CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf),
                 "dial bogus: unknown network bogus");
    c = net_dial(heap_allocator(), S("tcp6"), S("127.0.0.1:1"), &e);
    CHECK(c.vt == NULL);
    CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf),
                 "dial tcp6: address 127.0.0.1: no suitable address found");
    const NetOpError *oe = errors_as(e, TYPE_NET_OP_ERROR);
    CHECK(oe != NULL);
    c = net_dial(heap_allocator(), S("ip4:icmp"), S("127.0.0.1"), &e);
    CHECK(c.vt == NULL && BURROW_FAILED(e));
    CHECK(errors_as(e, TYPE_NET_OP_ERROR) != NULL);
}

static void TestDialTimeoutGivesUp(TestingT *t) {
    need_sockets(t);
    Str dir;
    NetListener l = local_listener(t, S("tcp"), &dir);
    if (l.vt == NULL)
        return;
    Error e = BURROW_NO_ERROR;
    NetConn c = net_dial_timeout(heap_allocator(), S("tcp"), listener_addr(l),
                                 -1 * TIME_SECOND, &e);
    CHECK(c.vt == NULL);
    CHECK(net_error_timeout(e));
    CHECK(errors_is(e, os_err_deadline_exceeded));
    c = net_dial_timeout(heap_allocator(), S("tcp"), listener_addr(l), TIME_SECOND, &e);
    CHECK(BURROW_OK(e));
    net_conn_free(c);
    net_listener_free(l);
}

/* ------------------------------------------------------------ the Resolve */

static NetIP ip_of(const char *s) {
    if (s == NULL)
        return slice_nil(TYPE_BYTE);
    return net_parse_ip(a, str_from_cstr(s));
}

static void TestResolveTCPAddr(TestingT *t) {
    struct {
        const char *network;
        const char *lit;
        const char *ip;
        Int port;
        const char *zone;
        const char *err;
    } cases[] = {
        {"tcp", "127.0.0.1:0", "127.0.0.1", 0, "", NULL},
        {"tcp4", "127.0.0.1:65535", "127.0.0.1", 65535, "", NULL},
        {"tcp", "[::1]:0", "::1", 0, "", NULL},
        {"tcp6", "[::1]:65535", "::1", 65535, "", NULL},
        {"tcp", "[::1%en0]:1", "::1", 1, "en0", NULL},
        {"tcp6", "[::1%911]:2", "::1", 2, "911", NULL},
        {"", "127.0.0.1:0", "127.0.0.1", 0, "", NULL},
        {"", "[::1]:0", "::1", 0, "", NULL},
        {"tcp", ":12345", NULL, 12345, "", NULL},
        {"http", "127.0.0.1:0", NULL, 0, "", "unknown network http"},
        {"tcp", "127.0.0.1:http", "127.0.0.1", 80, "", NULL},
        {"tcp", "[::ffff:127.0.0.1]:http", "::ffff:127.0.0.1", 80, "", NULL},
        {"tcp", "[2001:db8::1]:http", "2001:db8::1", 80, "", NULL},
        {"tcp4", "127.0.0.1:http", "127.0.0.1", 80, "", NULL},
        {"tcp4", "[::ffff:127.0.0.1]:http", "127.0.0.1", 80, "", NULL},
        {"tcp6", "[2001:db8::1]:http", "2001:db8::1", 80, "", NULL},
        {"tcp4", "[2001:db8::1]:http", NULL, 0, "",
         "address 2001:db8::1: no suitable address found"},
        {"tcp6", "127.0.0.1:http", NULL, 0, "",
         "address 127.0.0.1: no suitable address found"},
        {"tcp6", "[::ffff:127.0.0.1]:http", NULL, 0, "",
         "address ::ffff:127.0.0.1: no suitable address found"},
        /* Go's issue 20911: no IPv4 address for the IPv6 unspecified one. */
        {"tcp", "[::]:4", "::", 4, "", NULL},
    };
    char buf[128];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Error e = BURROW_NO_ERROR;
        NetTCPAddr *got = net_resolve_tcp_addr(a, str_from_cstr(cases[i].network),
                                               str_from_cstr(cases[i].lit), &e);
        if (cases[i].err != NULL) {
            if (got != NULL || BURROW_OK(e))
                testing_t_errorf_v(t, "ResolveTCPAddr(%s, %s) did not fail",
                                   cases[i].network, cases[i].lit);
            else
                CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf), cases[i].err);
            continue;
        }
        NetIP want = ip_of(cases[i].ip);
        for (int round = 0; round < 2 && got != NULL; round++) {
            if (!net_ip_equal(got->ip, want) || got->ip.len != want.len ||
                got->port != cases[i].port ||
                !str_eq(got->zone, str_from_cstr(cases[i].zone)))
                testing_t_errorf_v(t, "ResolveTCPAddr(%s, %s) #%d = %s",
                                   cases[i].network, cases[i].lit, round,
                                   net_tcp_addr_string(got, a));
            /* And again from what it gave. */
            got = net_resolve_tcp_addr(a, net_tcp_addr_network(got),
                                       net_tcp_addr_string(got, a), &e);
        }
        if (got == NULL || BURROW_FAILED(e))
            testing_t_errorf_v(t, "ResolveTCPAddr(%s, %s): %v", cases[i].network,
                               cases[i].lit, e);
    }
}

static void TestResolveUDPAddr(TestingT *t) {
    struct {
        const char *network;
        const char *lit;
        const char *ip;
        Int port;
        const char *zone;
        const char *err;
    } cases[] = {
        {"udp", "127.0.0.1:0", "127.0.0.1", 0, "", NULL},
        {"udp4", "127.0.0.1:65535", "127.0.0.1", 65535, "", NULL},
        {"udp", "[::1]:0", "::1", 0, "", NULL},
        {"udp6", "[::1]:65535", "::1", 65535, "", NULL},
        {"udp", "[::1%en0]:1", "::1", 1, "en0", NULL},
        {"udp6", "[::1%911]:2", "::1", 2, "911", NULL},
        {"", "127.0.0.1:0", "127.0.0.1", 0, "", NULL},
        {"", "[::1]:0", "::1", 0, "", NULL},
        {"udp", ":12345", NULL, 12345, "", NULL},
        {"http", "127.0.0.1:0", NULL, 0, "", "unknown network http"},
        {"udp", "127.0.0.1:domain", "127.0.0.1", 53, "", NULL},
        {"udp", "[::ffff:127.0.0.1]:domain", "::ffff:127.0.0.1", 53, "", NULL},
        {"udp", "[2001:db8::1]:domain", "2001:db8::1", 53, "", NULL},
        {"udp4", "127.0.0.1:domain", "127.0.0.1", 53, "", NULL},
        {"udp4", "[::ffff:127.0.0.1]:domain", "127.0.0.1", 53, "", NULL},
        {"udp6", "[2001:db8::1]:domain", "2001:db8::1", 53, "", NULL},
        {"udp4", "[2001:db8::1]:domain", NULL, 0, "",
         "address 2001:db8::1: no suitable address found"},
        {"udp6", "127.0.0.1:domain", NULL, 0, "",
         "address 127.0.0.1: no suitable address found"},
        {"udp6", "[::ffff:127.0.0.1]:domain", NULL, 0, "",
         "address ::ffff:127.0.0.1: no suitable address found"},
        {"udp", "[::]:4", "::", 4, "", NULL},
    };
    char buf[128];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Error e = BURROW_NO_ERROR;
        NetUDPAddr *got = net_resolve_udp_addr(a, str_from_cstr(cases[i].network),
                                               str_from_cstr(cases[i].lit), &e);
        if (cases[i].err != NULL) {
            if (got != NULL || BURROW_OK(e))
                testing_t_errorf_v(t, "ResolveUDPAddr(%s, %s) did not fail",
                                   cases[i].network, cases[i].lit);
            else
                CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf), cases[i].err);
            continue;
        }
        NetIP want = ip_of(cases[i].ip);
        for (int round = 0; round < 2 && got != NULL; round++) {
            if (!net_ip_equal(got->ip, want) || got->ip.len != want.len ||
                got->port != cases[i].port ||
                !str_eq(got->zone, str_from_cstr(cases[i].zone)))
                testing_t_errorf_v(t, "ResolveUDPAddr(%s, %s) #%d = %s",
                                   cases[i].network, cases[i].lit, round,
                                   net_udp_addr_string(got, a));
            got = net_resolve_udp_addr(a, net_udp_addr_network(got),
                                       net_udp_addr_string(got, a), &e);
        }
        if (got == NULL || BURROW_FAILED(e))
            testing_t_errorf_v(t, "ResolveUDPAddr(%s, %s): %v", cases[i].network,
                               cases[i].lit, e);
    }
}

static void TestResolveIPAddr(TestingT *t) {
    struct {
        const char *network;
        const char *lit;
        const char *ip;
        const char *zone;
        const char *err;
    } cases[] = {
        {"ip", "127.0.0.1", "127.0.0.1", "", NULL},
        {"ip4", "127.0.0.1", "127.0.0.1", "", NULL},
        {"ip4:icmp", "127.0.0.1", "127.0.0.1", "", NULL},
        {"ip", "::1", "::1", "", NULL},
        {"ip6", "::1", "::1", "", NULL},
        {"ip6:ipv6-icmp", "::1", "::1", "", NULL},
        {"ip6:IPv6-ICMP", "::1", "::1", "", NULL},
        {"ip", "::1%en0", "::1", "en0", NULL},
        {"ip6", "::1%911", "::1", "911", NULL},
        {"", "127.0.0.1", "127.0.0.1", "", NULL},
        {"", "::1", "::1", "", NULL},
        {"ip4:icmp", "", NULL, "", NULL},
        {"l2tp", "127.0.0.1", NULL, "", "unknown network l2tp"},
        {"l2tp:gre", "127.0.0.1", NULL, "", "unknown network l2tp:gre"},
        {"tcp", "1.2.3.4:123", NULL, "", "unknown network tcp"},
        {"ip4", "2001:db8::1", NULL, "",
         "address 2001:db8::1: no suitable address found"},
        {"ip4:icmp", "2001:db8::1", NULL, "",
         "address 2001:db8::1: no suitable address found"},
        {"ip6", "127.0.0.1", NULL, "", "address 127.0.0.1: no suitable address found"},
        {"ip6", "::ffff:127.0.0.1", NULL, "",
         "address ::ffff:127.0.0.1: no suitable address found"},
        {"ip6:ipv6-icmp", "127.0.0.1", NULL, "",
         "address 127.0.0.1: no suitable address found"},
        {"ip6:ipv6-icmp", "::ffff:127.0.0.1", NULL, "",
         "address ::ffff:127.0.0.1: no suitable address found"},
        {"ip", "::", "::", "", NULL},
    };
    char buf[128];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Error e = BURROW_NO_ERROR;
        NetIPAddr *got = net_resolve_ip_addr(a, str_from_cstr(cases[i].network),
                                             str_from_cstr(cases[i].lit), &e);
        if (cases[i].err != NULL) {
            if (got != NULL || BURROW_OK(e))
                testing_t_errorf_v(t, "ResolveIPAddr(%s, %s) did not fail",
                                   cases[i].network, cases[i].lit);
            else
                CHECK_STR_EQ(c_text(error_text(e), buf, sizeof buf), cases[i].err);
            continue;
        }
        NetIP want = ip_of(cases[i].ip);
        for (int round = 0; round < 2 && got != NULL; round++) {
            if (!net_ip_equal(got->ip, want) || got->ip.len != want.len ||
                !str_eq(got->zone, str_from_cstr(cases[i].zone)))
                testing_t_errorf_v(t, "ResolveIPAddr(%s, %s) #%d = %s",
                                   cases[i].network, cases[i].lit, round,
                                   net_ip_addr_string(got, a));
            got = net_resolve_ip_addr(a, net_ip_addr_network(got),
                                      net_ip_addr_string(got, a), &e);
        }
        if (got == NULL || BURROW_FAILED(e))
            testing_t_errorf_v(t, "ResolveIPAddr(%s, %s): %v", cases[i].network,
                               cases[i].lit, e);
    }
}

#define TESTS(X)                                                                       \
    X(TestDialLocal)                                                                   \
    X(TestDialerPartialDeadline)                                                       \
    X(TestDialerLocalAddr)                                                             \
    X(TestCancelAfterDial)                                                             \
    X(TestDialListenerAddr)                                                            \
    X(TestDialerControl)                                                               \
    X(TestDialerControlContext)                                                        \
    X(TestDialContext)                                                                 \
    X(TestListenConfigControl)                                                         \
    X(TestADialErrorIsAnOpError)                                                       \
    X(TestDialTimeoutGivesUp)                                                          \
    X(TestResolveTCPAddr)                                                              \
    X(TestResolveUDPAddr)                                                              \
    X(TestResolveIPAddr)

static int net_dial_main(TestingM *m) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    int r = testing_m_run(m);
    arena_free(&ar);
    return r;
}

TESTING_MAIN_WITH(net_dial_main, TESTS)
