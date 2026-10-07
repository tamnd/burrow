/* FileConn, FileListener, FilePacketConn and the File methods, from Go's
 * src/net/file_test.go and file_unix_test.go.
 *
 * Go compares addresses with reflect.DeepEqual, and these compare their type,
 * network and text, which is the same thing for the addresses here. The local
 * server is one that reads a byte from each connection it accepts, which is
 * the handler Go's tests give theirs.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/netpoll.h"
#include "burrow/os.h"
#include "burrow/pal.h"
#include "burrow/platform.h"
#include "burrow/proc.h"
#include "burrow/sync.h"
#include "burrow/syscall.h"
#include "burrow/testing.h"

#include "../src/net/internal.h"
#include "check.h"

#include <stddef.h>
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

/* ---------------------------------------------------------------- helpers */

/* testableNetwork, without the ones that need privileges. */
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
    return true;
#endif
}

/* A fresh name for a Unix socket, short enough for macOS, in a directory the
 * caller removes. */
static Str unix_path(TestingT *t, Str *dir) {
    Error e = BURROW_NO_ERROR;
    *dir = os_mkdir_temp(a, S(""), S("bxf"), &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "MkdirTemp: %v", e);
    return fmt_sprintf_v(a, "%s/s", *dir);
}

/* newLocalListener */
static NetListener local_listener(TestingT *t, Str network, Str *dir) {
    *dir = BURROW_STR_EMPTY;
    Str address = S("127.0.0.1:0");
    if (str_eq(network, S("unix")) || str_eq(network, S("unixpacket")))
        address = unix_path(t, dir);
    Error e = BURROW_NO_ERROR;
    NetListener l = net_listen(heap_allocator(), network, address, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "Listen(%s, %s): %v", network, address, e);
    return l;
}

/* newLocalPacketListener */
static NetPacketConn local_packet_listener(TestingT *t, Str network, Str *dir) {
    *dir = BURROW_STR_EMPTY;
    Str address = S("127.0.0.1:0");
    if (str_eq(network, S("unixgram")))
        address = unix_path(t, dir);
    Error e = BURROW_NO_ERROR;
    NetPacketConn c = net_listen_packet(heap_allocator(), network, address, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(t, "ListenPacket(%s, %s): %v", network, address, e);
    return c;
}

static void remove_dir(Str dir) {
    if (dir.len > 0)
        (void)os_remove_all(dir);
}

/* An address as what DeepEqual looks at: its type, network and text. */
typedef struct Addr {
    const Type *type;
    Str network;
    Str text;
} Addr;

static Addr addr_of(NetAddr addr) {
    Addr r = {NULL, S("<nil>"), S("<nil>")};
    if (addr.vt != NULL) {
        r.type = addr.vt->self_type;
        r.network = addr.vt->network(addr.data);
        r.text = addr.vt->string(addr.data, a);
    }
    return r;
}

static void check_addr(TestingT *t, NetAddr got, Addr want) {
    Addr g = addr_of(got);
    if (g.type != want.type || !str_eq(g.network, want.network) ||
        !str_eq(g.text, want.text))
        testing_t_errorf_v(t, "got %s %s; want %s %s", g.network, g.text, want.network,
                           want.text);
}

/* A localServer whose handler reads a byte from each connection it accepts
 * and closes it, until the listener is closed. */
typedef struct Server {
    NetListener l;
    Str dir;
    SyncWaitGroup wg;
} Server;

static void server_job(void *env) {
    Server *s = env;
    for (;;) {
        Error e = BURROW_NO_ERROR;
        NetConn c = s->l.vt->accept(s->l.data, &e);
        if (BURROW_FAILED(e))
            break;
        char b[1];
        Error re = BURROW_NO_ERROR;
        (void)c.vt->reader.read(c.data, slice_from(b, 1, 1, TYPE_BYTE), &re);
        net_conn_free(c);
    }
    sync_wait_group_done(&s->wg);
}

static bool server_start(TestingT *t, Server *s, Str network) {
    memset(s, 0, sizeof *s);
    s->l = local_listener(t, network, &s->dir);
    if (s->l.vt == NULL)
        return false;
    sync_wait_group_add(&s->wg, 1);
    if (!go(BURROW_FN(Func, server_job, s))) {
        sync_wait_group_done(&s->wg);
        testing_t_error_v(t, S("go: out of memory"));
        return false;
    }
    return true;
}

static void server_stop(Server *s) {
    if (s->l.vt == NULL)
        return;
    (void)s->l.vt->closer.close(s->l.data);
    sync_wait_group_wait(&s->wg);
    net_listener_free(s->l);
    remove_dir(s->dir);
}

/* c1.File(), for whichever conn c1 is. */
static OsFile *conn_file(NetConn c, Error *err) {
    NetTCPConn *tc = net_conn_as_tcp_conn(c);
    if (tc != NULL)
        return net_tcp_conn_file(tc, heap_allocator(), err);
    NetUDPConn *uc = net_conn_as_udp_conn(c);
    if (uc != NULL)
        return net_udp_conn_file(uc, heap_allocator(), err);
    NetUnixConn *xc = net_conn_as_unix_conn(c);
    if (xc != NULL)
        return net_unix_conn_file(xc, heap_allocator(), err);
    *err = errors_new(error_allocator(), S("not a TCPConn, UDPConn or UnixConn"));
    return NULL;
}

/* f.Close(), and the OsFile freed. */
static void close_file(TestingT *t, OsFile *f) {
    Error e = os_file_close(f);
    if (BURROW_FAILED(e))
        testing_t_error_v(t, e);
    os_file_free(f);
}

/* -------------------------------------------------------------- TestFileConn */

static void file_conn(void *env, TestingT *t) {
    Str network = *(const Str *)env;
    if (!testable(network)) {
        testing_t_skipf_v(t, "skipping %s test", network);
        return;
    }
    Server ls;
    memset(&ls, 0, sizeof ls);
    NetPacketConn pc = {NULL, NULL};
    Str pdir = BURROW_STR_EMPTY;
    Str net;
    Str address;
    if (str_eq(network, S("udp"))) {
        pc = local_packet_listener(t, network, &pdir);
        if (pc.vt == NULL)
            return;
        Addr la = addr_of(pc.vt->local_addr(pc.data));
        net = la.network;
        address = la.text;
    } else {
        if (!server_start(t, &ls, network)) {
            server_stop(&ls);
            return;
        }
        Addr la = addr_of(ls.l.vt->addr(ls.l.data));
        net = la.network;
        address = la.text;
    }

    Error e = BURROW_NO_ERROR;
    NetConn c1 = net_dial(heap_allocator(), net, address, &e);
    OsFile *f = NULL;
    NetConn c2 = {NULL, NULL};
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "Dial: %v", e);
    } else {
        Addr addr = addr_of(c1.vt->local_addr(c1.data));
        f = conn_file(c1, &e);
        Error ce = c1.vt->closer.close(c1.data);
        if (BURROW_FAILED(ce))
            testing_t_error_v(t, ce);
        net_conn_free(c1);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "File: %v", e);
        } else {
            c2 = net_file_conn(heap_allocator(), f, &e);
            close_file(t, f);
            if (BURROW_FAILED(e)) {
                testing_t_errorf_v(t, "FileConn: %v", e);
            } else {
                char msg[] = "FILECONN TEST";
                Error we = BURROW_NO_ERROR;
                (void)c2.vt->writer.write(
                    c2.data,
                    slice_from(msg, (Int)strlen(msg), (Int)strlen(msg), TYPE_BYTE),
                    &we);
                if (BURROW_FAILED(we))
                    testing_t_errorf_v(t, "Write: %v", we);
                check_addr(t, c2.vt->local_addr(c2.data), addr);
            }
        }
    }
    if (c2.vt != NULL)
        net_conn_free(c2);
    if (pc.vt != NULL)
        net_packet_conn_free(pc);
    remove_dir(pdir);
    server_stop(&ls);
}

static void TestFileConn(TestingT *t) {
    static const Str nets[] = {BURROW_S_INIT("tcp"), BURROW_S_INIT("udp"),
                               BURROW_S_INIT("unix"), BURROW_S_INIT("unixpacket")};
    for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++)
        testing_t_run(t, nets[i],
                      BURROW_FN(TestingTFunc, file_conn, (void *)(uintptr_t)&nets[i]));
}

/* ---------------------------------------------------------- TestFileListener */

typedef struct DialJob {
    TestingT *t;
    Addr addr;
    SyncWaitGroup wg;
} DialJob;

static void dial_job(void *env) {
    DialJob *j = env;
    Error e = BURROW_NO_ERROR;
    NetConn c = net_dial(heap_allocator(), j->addr.network, j->addr.text, &e);
    if (BURROW_FAILED(e))
        testing_t_errorf_v(j->t, "Dial: %v", e);
    else
        net_conn_free(c);
    sync_wait_group_done(&j->wg);
}

static void file_listener(void *env, TestingT *t) {
    Str network = *(const Str *)env;
    if (!testable(network)) {
        testing_t_skipf_v(t, "skipping %s test", network);
        return;
    }
    bool is_unix = !str_eq(network, S("tcp"));
    Str dir;
    NetListener ln1 = local_listener(t, network, &dir);
    if (ln1.vt == NULL) {
        remove_dir(dir);
        return;
    }
    Addr addr = addr_of(ln1.vt->addr(ln1.data));

    Error e = BURROW_NO_ERROR;
    OsFile *f;
    if (is_unix)
        f = net_unix_listener_file(net_listener_as_unix_listener(ln1), heap_allocator(),
                                   &e);
    else
        f = net_tcp_listener_file(net_listener_as_tcp_listener(ln1), heap_allocator(),
                                  &e);
    /* UnixListener.Close removes the socket file, so that one waits. */
    if (!is_unix) {
        Error ce = ln1.vt->closer.close(ln1.data);
        if (BURROW_FAILED(ce))
            testing_t_error_v(t, ce);
    }

    NetListener ln2 = {NULL, NULL};
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "File: %v", e);
    } else {
        ln2 = net_file_listener(heap_allocator(), f, &e);
        close_file(t, f);
        if (BURROW_FAILED(e))
            testing_t_errorf_v(t, "FileListener: %v", e);
    }
    if (ln2.vt != NULL) {
        DialJob j;
        memset(&j, 0, sizeof j);
        j.t = t;
        j.addr = addr_of(ln2.vt->addr(ln2.data));
        sync_wait_group_add(&j.wg, 1);
        if (!go(BURROW_FN(Func, dial_job, &j)))
            sync_wait_group_done(&j.wg);
        NetConn c = ln2.vt->accept(ln2.data, &e);
        if (BURROW_FAILED(e))
            testing_t_errorf_v(t, "Accept: %v", e);
        else
            net_conn_free(c);
        sync_wait_group_wait(&j.wg);
        check_addr(t, ln2.vt->addr(ln2.data), addr);
        net_listener_free(ln2);
    }
    net_listener_free(ln1);
    remove_dir(dir);
}

static void TestFileListener(TestingT *t) {
    static const Str nets[] = {BURROW_S_INIT("tcp"), BURROW_S_INIT("unix"),
                               BURROW_S_INIT("unixpacket")};
    for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++)
        testing_t_run(
            t, nets[i],
            BURROW_FN(TestingTFunc, file_listener, (void *)(uintptr_t)&nets[i]));
}

/* -------------------------------------------------------- TestFilePacketConn */

static void file_packet_conn(void *env, TestingT *t) {
    Str network = *(const Str *)env;
    if (!testable(network)) {
        testing_t_skipf_v(t, "skipping %s test", network);
        return;
    }
    Str dir;
    NetPacketConn c1 = local_packet_listener(t, network, &dir);
    if (c1.vt == NULL) {
        remove_dir(dir);
        return;
    }
    Addr addr = addr_of(c1.vt->local_addr(c1.data));

    Error e = BURROW_NO_ERROR;
    OsFile *f;
    NetUDPConn *uc = net_packet_conn_as_udp_conn(c1);
    if (uc != NULL)
        f = net_udp_conn_file(uc, heap_allocator(), &e);
    else
        f = net_unix_conn_file(net_packet_conn_as_unix_conn(c1), heap_allocator(), &e);
    Error ce = c1.vt->closer.close(c1.data);
    if (BURROW_FAILED(ce))
        testing_t_error_v(t, ce);

    NetPacketConn c2 = {NULL, NULL};
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "File: %v", e);
    } else {
        c2 = net_file_packet_conn(heap_allocator(), f, &e);
        close_file(t, f);
        if (BURROW_FAILED(e))
            testing_t_errorf_v(t, "FilePacketConn: %v", e);
    }
    if (c2.vt != NULL) {
        /* Go writes to c1's address, which is c2's too, and c1's own NetAddr
         * went with c1, so this writes to c2's. */
        char msg[] = "FILEPACKETCONN TEST";
        Error we = BURROW_NO_ERROR;
        (void)c2.vt->write_to(
            c2.data, slice_from(msg, (Int)strlen(msg), (Int)strlen(msg), TYPE_BYTE),
            c2.vt->local_addr(c2.data), &we);
        if (BURROW_FAILED(we))
            testing_t_errorf_v(t, "WriteTo: %v", we);
        check_addr(t, c2.vt->local_addr(c2.data), addr);
        net_packet_conn_free(c2);
    }
    net_packet_conn_free(c1);
    remove_dir(dir);
}

static void TestFilePacketConn(TestingT *t) {
    static const Str nets[] = {BURROW_S_INIT("udp"), BURROW_S_INIT("unixgram")};
    for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++)
        testing_t_run(
            t, nets[i],
            BURROW_FN(TestingTFunc, file_packet_conn, (void *)(uintptr_t)&nets[i]));
}

/* --------------------------------------------------------- TestFileCloseRace */

typedef struct RaceJob {
    NetTCPConn *tc;
    SyncWaitGroup wg;
} RaceJob;

static void race_file(void *env) {
    RaceJob *j = env;
    Error e = BURROW_NO_ERROR;
    OsFile *f = net_tcp_conn_file(j->tc, heap_allocator(), &e);
    if (BURROW_OK(e)) {
        (void)os_file_close(f);
        os_file_free(f);
    }
    sync_wait_group_done(&j->wg);
}

static void race_close(void *env) {
    RaceJob *j = env;
    (void)net_tcp_conn_close(j->tc);
    sync_wait_group_done(&j->wg);
}

static void TestFileCloseRace(TestingT *t) {
    if (!testable(S("tcp"))) {
        testing_t_skip_v(t, "tcp not supported");
        return;
    }
    Server ls;
    if (!server_start(t, &ls, S("tcp"))) {
        server_stop(&ls);
        return;
    }
    Addr la = addr_of(ls.l.vt->addr(ls.l.data));
    for (int i = 0; i < 100; i++) {
        Error e = BURROW_NO_ERROR;
        NetConn c1 = net_dial(heap_allocator(), la.network, la.text, &e);
        if (BURROW_FAILED(e)) {
            testing_t_errorf_v(t, "Dial: %v", e);
            break;
        }
        RaceJob j;
        memset(&j, 0, sizeof j);
        j.tc = net_conn_as_tcp_conn(c1);
        sync_wait_group_add(&j.wg, 2);
        if (!go(BURROW_FN(Func, race_file, &j)))
            race_file(&j);
        if (!go(BURROW_FN(Func, race_close, &j)))
            race_close(&j);
        sync_wait_group_wait(&j.wg);
        net_conn_free(c1);
    }
    server_stop(&ls);
}

/* ---------------------------------------------------------- TestFileFdBlocks */

#if defined(HAVE_SOCKETS) && !defined(BURROW_OS_WINDOWS)
typedef struct Mode {
    TestingT *t;
    const char *what;
} Mode;

/* unix.IsNonblock on the descriptor, which has to say yes. */
static void want_nonblock(void *env, Uintptr fd) {
    Mode *m = env;
    bool on = false;
    PalErrno pe = PAL_OK;
    if (!pal_nonblock((int64_t)fd, &on, &pe))
        testing_t_errorf_v(m->t, "IsNonblock: %v", syscall_errno_from_pal(pe));
    else if (!on)
        testing_t_errorf_v(m->t, "%s is in blocking mode", m->what);
}

static void check_nonblock(TestingT *t, SyscallRawConn rc, const char *what) {
    Mode m = {t, what};
    Error e = rc.vt->control(rc.data, BURROW_FN(SyscallFdFunc, want_nonblock, &m));
    if (BURROW_FAILED(e))
        testing_t_error_v(t, e);
}
#endif

/* For backward compatibility, opening a net.Conn, turning it into an os.File,
 * and calling the Fd method should return a blocking descriptor. */
static void TestFileFdBlocks(TestingT *t) {
#if !defined(HAVE_SOCKETS) || defined(BURROW_OS_WINDOWS)
    testing_t_skip_v(t, "skipping: unix sockets not supported");
#else
    Server ls;
    if (!server_start(t, &ls, S("unix"))) {
        server_stop(&ls);
        return;
    }
    Addr la = addr_of(ls.l.vt->addr(ls.l.data));
    Error e = BURROW_NO_ERROR;
    NetConn client = net_dial(heap_allocator(), la.network, la.text, &e);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "Dial: %v", e);
        server_stop(&ls);
        return;
    }
    NetUnixConn *uc = net_conn_as_unix_conn(client);

    /* The socket should be non-blocking. */
    SyscallRawConn rc = net_unix_conn_syscall_conn(uc, &e);
    if (BURROW_FAILED(e))
        testing_t_error_v(t, e);
    else
        check_nonblock(t, rc, "unix socket");

    OsFile *file = net_unix_conn_file(uc, heap_allocator(), &e);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "File: %v", e);
    } else {
        /* At this point the descriptor should still be non-blocking. */
        rc = os_file_syscall_conn(file, &e);
        if (BURROW_FAILED(e))
            testing_t_error_v(t, e);
        else
            check_nonblock(t, rc, "unix socket as os.File");

        Uintptr fd = os_file_fd(file);

        /* Calling Fd should have put the descriptor into blocking mode. */
        bool on = true;
        PalErrno pe = PAL_OK;
        if (!pal_nonblock((int64_t)fd, &on, &pe))
            testing_t_errorf_v(t, "IsNonblock: %v", syscall_errno_from_pal(pe));
        else if (on)
            testing_t_error_v(t, S("unix socket through os.File.Fd is non-blocking"));
        close_file(t, file);
    }
    net_conn_free(client);
    server_stop(&ls);
#endif
}

/* ------------------------------------------------------------------ errors */

/* What FileConn and the others give for a file that is not a socket, which
 * Go has as an OpError naming the file. */
static void TestFileConnNotSocket(TestingT *t) {
#if !defined(HAVE_SOCKETS) || defined(BURROW_OS_WINDOWS)
    testing_t_skip_v(t, "Windows says something else for a file that is not a socket");
#else
    Str dir;
    Error e = BURROW_NO_ERROR;
    dir = os_mkdir_temp(a, S(""), S("bxf"), &e);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "MkdirTemp: %v", e);
        return;
    }
    Str name = fmt_sprintf_v(a, "%s/f", dir);
    OsFile *f = os_create(heap_allocator(), name, &e);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "Create: %v", e);
        remove_dir(dir);
        return;
    }
    NetConn c = net_file_conn(heap_allocator(), f, &e);
    if (c.vt != NULL) {
        testing_t_error_v(t, S("FileConn made a conn from a regular file"));
        net_conn_free(c);
    }
    Str want = fmt_sprintf_v(
        a, "file file+net %s: getsockopt: socket operation on non-socket", name);
    if (!str_eq(error_text(e), want))
        testing_t_errorf_v(t, "got %s; want %s", error_text(e), want);
    if (errors_as(e, TYPE_NET_OP_ERROR) == NULL)
        testing_t_errorf_v(t, "%v is not an OpError", e);
    close_file(t, f);
    remove_dir(dir);
#endif
}

/* The File methods on NULL, which are EINVAL, as on a nil conn in Go. */
static void TestFileNil(TestingT *t) {
    Error e = BURROW_NO_ERROR;
    OsFile *f = net_tcp_conn_file(NULL, heap_allocator(), &e);
    if (f != NULL || !errors_is(e, burrow__net_einval()))
        testing_t_errorf_v(t, "TCPConn.File on nil: %v", e);
    e = BURROW_NO_ERROR;
    f = net_unix_listener_file(NULL, heap_allocator(), &e);
    if (f != NULL || !errors_is(e, burrow__net_einval()))
        testing_t_errorf_v(t, "UnixListener.File on nil: %v", e);
}

#define TESTS(X)                                                                       \
    X(TestFileConn)                                                                    \
    X(TestFileListener)                                                                \
    X(TestFilePacketConn)                                                              \
    X(TestFileCloseRace)                                                               \
    X(TestFileFdBlocks)                                                                \
    X(TestFileConnNotSocket)                                                           \
    X(TestFileNil)

static int net_file_main(TestingM *m) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    int code = testing_m_run(m);
    arena_free(&ar);
    return code;
}

TESTING_MAIN_WITH(net_file_main, TESTS)
