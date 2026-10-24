/* log/syslog tests.
 *
 * Derived from Go's src/log/syslog/syslog_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/log/syslog.h"

#include "burrow/bufio.h"
#include "burrow/chan.h"
#include "burrow/fmt.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/path/filepath.h"
#include "burrow/platform.h"
#include "burrow/proc.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/testing.h"
#include "burrow/time.h"

#include <stdint.h>
#include <string.h>

/* Not in Go: Unix sockets and TCP here need the readiness poll FD, and
 * Windows has stream Unix sockets only. */
#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_NET 1
#endif

#if defined(HAVE_NET) && !defined(BURROW_OS_WINDOWS)
#define HAVE_UNIXGRAM 1
#endif

/* The strings the servers send on done are on the heap, and whoever takes one
 * off gives it back with this. */
static void free_str(Str s) {
    if (s.len > 0)
        mem_free(heap_allocator(), (void *)(uintptr_t)s.p, (size_t)s.len, 1);
}

/* Not in Go: makes the done channel, and frees it after the test with
 * whatever is still in it. Made before the servers that send on it, so their
 * cleanups run first. */
static void done_free(void *env) {
    Chan *done = (Chan *)env;
    Str s;
    bool ok = false;
    while (chan_try_recv(done, &s, &ok) && ok)
        free_str(s);
    chan_free(done);
}

static Chan *make_done(TestingT *t, Int cap) {
    Chan *done = chan_make(heap_allocator(), TYPE_STRING, cap);
    if (done == NULL)
        testing_t_fatalf_v(t, "no channel");
    testing_t_cleanup(t, BURROW_FN(Func, done_free, done));
    return done;
}

/* What startServer gives back: addr, sock and wg, and what C needs to free
 * them. */
typedef struct Server {
    TestingT *t;
    Str addr;
    bool packet;
    NetPacketConn pc;
    NetListener l;
    SyncWaitGroup wg;
    Chan *done;
    Str dir; /* the temp directory to remove, if there is one */
    bool stopped;
    Arena ar;
} Server;

static void append_bytes(Byte **p, Int *len, const Byte *b, Int n) {
    if (n <= 0)
        return;
    Byte *q = mem_alloc(heap_allocator(), (size_t)(*len + n), 1);
    if (q == NULL)
        return;
    if (*len > 0) {
        memcpy(q, *p, (size_t)*len);
        mem_free(heap_allocator(), *p, (size_t)*len, 1);
    }
    memcpy(q + *len, b, (size_t)n);
    *p = q;
    *len += n;
}

static void run_pkt_syslog(void *env) {
    Server *sv = (Server *)env;
    NetPacketConn c = sv->pc;
    Byte buf[4096];
    Byte *rcvd = NULL;
    Int rlen = 0;
    int ct = 0;
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    for (;;) {
        Error err = BURROW_NO_ERROR;
        NetAddr from = {NULL, NULL};

        (void)c.vt->set_read_deadline(c.data,
                                      time_add(time_now(), 100 * TIME_MILLISECOND));
        Int n = c.vt->read_from(c.data, slice_from(buf, 0, sizeof buf, TYPE_BYTE),
                                arena_allocator(&ar), &from, &err);
        append_bytes(&rcvd, &rlen, buf, n);
        if (BURROW_FAILED(err)) {
            if (err.vt->self_type == TYPE_NET_OP_ERROR) {
                const NetOpError *oe = (const NetOpError *)err.data;
                if (ct < 3 && net_op_error_temporary(oe)) {
                    ct++;
                    continue;
                }
            }
            break;
        }
    }
    (void)c.vt->closer.close(c.data);
    arena_free(&ar);
    Str s = {rcvd, rlen};
    chan_send(sv->done, &s);
    sync_wait_group_done(&sv->wg);
}

static bool crashy = false;

static bool testable_network(Str network) {
    if (str_eq(network, BURROW_S("unix")) || str_eq(network, BURROW_S("unixgram"))) {
        if (strcmp(burrow_os_name(), "ios") == 0 ||
            strcmp(burrow_os_name(), "android") == 0)
            return false;
    }
    /* Not in Go: what this build has. */
#if !defined(HAVE_NET)
    return false;
#elif !defined(HAVE_UNIXGRAM)
    if (str_eq(network, BURROW_S("unixgram")))
        return false;
#endif
    return true;
}

typedef struct StreamConn {
    NetConn c;
    Chan *done;
    SyncWaitGroup *wg;
} StreamConn;

static void stream_conn(void *env) {
    StreamConn *sc = (StreamConn *)env;
    NetConn c = sc->c;
    SyncWaitGroup *wg = sc->wg;
    (void)c.vt->set_read_deadline(c.data, time_add(time_now(), 5 * TIME_SECOND));
    BufioReader *b = bufio_new_reader(heap_allocator(), net_conn_as_io_reader(c));
    for (int ct = 1; b != NULL && (!crashy || (ct & 7) != 0); ct++) {
        Error err = BURROW_NO_ERROR;
        Str s = bufio_reader_read_string(b, heap_allocator(), '\n', &err);
        if (BURROW_FAILED(err)) {
            free_str(s);
            break;
        }
        chan_send(sc->done, &s);
    }
    bufio_reader_free(b);
    net_conn_free(c);
    mem_free(heap_allocator(), sc, sizeof *sc, _Alignof(StreamConn));
    sync_wait_group_done(wg);
}

static void run_stream_syslog(void *env) {
    Server *sv = (Server *)env;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        NetConn c = sv->l.vt->accept(sv->l.data, &err);
        if (BURROW_FAILED(err))
            break;
        StreamConn *sc = mem_alloc(heap_allocator(), sizeof *sc, _Alignof(StreamConn));
        if (sc == NULL) {
            net_conn_free(c);
            continue;
        }
        sc->c = c;
        sc->done = sv->done;
        sc->wg = &sv->wg;
        sync_wait_group_add(&sv->wg, 1);
        if (!go(BURROW_FN(Func, stream_conn, sc))) {
            net_conn_free(c);
            mem_free(heap_allocator(), sc, sizeof *sc, _Alignof(StreamConn));
            sync_wait_group_done(&sv->wg);
        }
    }
    sync_wait_group_done(&sv->wg);
}

/* Not in Go, which has defer sock.Close() and defer srvWG.Wait() in each test:
 * closes the socket, waits for the server, and gives it back. A second call
 * does nothing. */
static void server_stop(Server *sv) {
    if (sv->stopped)
        return;
    sv->stopped = true;
    if (sv->packet) {
        (void)sv->pc.vt->closer.close(sv->pc.data);
        sync_wait_group_wait(&sv->wg);
        net_packet_conn_free(sv->pc);
    } else {
        (void)sv->l.vt->closer.close(sv->l.data);
        sync_wait_group_wait(&sv->wg);
        net_listener_free(sv->l);
    }
}

static void server_cleanup(void *env) {
    Server *sv = (Server *)env;
    server_stop(sv);
    if (!str_is_empty(sv->dir)) {
        Error err = os_remove_all(sv->dir);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(sv->t, "failed to remove socket temp directory: %v",
                               err);
    }
    arena_free(&sv->ar);
    mem_free(heap_allocator(), sv, sizeof *sv, _Alignof(Server));
}

static Server *start_server(TestingT *t, Str n, Str la, Chan *done) {
    Server *sv = mem_alloc(heap_allocator(), sizeof *sv, _Alignof(Server));
    if (sv == NULL)
        testing_t_fatalf_v(t, "no memory for the server");
    sv->t = t;
    sv->done = done;
    sv->stopped = true; /* until there is something to stop */
    arena_init(&sv->ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&sv->ar);
    testing_t_cleanup(t, BURROW_FN(Func, server_cleanup, sv));

    if (str_eq(n, BURROW_S("udp")) || str_eq(n, BURROW_S("tcp"))) {
        la = BURROW_S("127.0.0.1:0");
    } else {
        /* unix and unixgram: choose an address if none given. */
        if (str_is_empty(la)) {
            /* The address must be short to fit in the sun_path field of the
             * sockaddr_un passed to the underlying system calls, so we use
             * os.MkdirTemp instead of t.TempDir: t.TempDir generally includes all
             * or part of the test name in the directory, which can be much more
             * verbose and risks running up against the limit. */
            Error err = BURROW_NO_ERROR;
            Str dir = os_mkdir_temp(a, BURROW_S(""), BURROW_S(""), &err);
            if (BURROW_FAILED(err))
                testing_t_fatalf_v(t, "%v", err);
            sv->dir = dir;
            Str parts[] = {dir, BURROW_S("sock")};
            la = filepath_join(a, slice_from(parts, 2, 2, TYPE_STRING));
        }
    }

    Error e = BURROW_NO_ERROR;
    if (str_eq(n, BURROW_S("udp")) || str_eq(n, BURROW_S("unixgram"))) {
        NetPacketConn l = net_listen_packet(heap_allocator(), n, la, &e);
        if (BURROW_FAILED(e)) {
            testing_t_helper(t);
            testing_t_fatalf_v(t, "startServer failed: %v", e);
        }
        NetAddr addr = l.vt->local_addr(l.data);
        sv->addr = addr.vt->string(addr.data, a);
        sv->packet = true;
        sv->pc = l;
        sv->stopped = false;
        sync_wait_group_add(&sv->wg, 1);
        if (!go(BURROW_FN(Func, run_pkt_syslog, sv))) {
            sync_wait_group_done(&sv->wg);
            testing_t_fatalf_v(t, "no goroutine for the server");
        }
    } else {
        NetListener l = net_listen(heap_allocator(), n, la, &e);
        if (BURROW_FAILED(e)) {
            testing_t_helper(t);
            testing_t_fatalf_v(t, "startServer failed: %v", e);
        }
        NetAddr addr = l.vt->addr(l.data);
        sv->addr = addr.vt->string(addr.data, a);
        sv->l = l;
        sv->stopped = false;
        sync_wait_group_add(&sv->wg, 1);
        if (!go(BURROW_FN(Func, run_stream_syslog, sv))) {
            sync_wait_group_done(&sv->wg);
            testing_t_fatalf_v(t, "no goroutine for the server");
        }
    }
    return sv;
}

static bool is_unix(Str transport) {
    return str_eq(transport, BURROW_S("unixgram")) ||
           str_eq(transport, BURROW_S("unix"));
}

static void check(TestingT *t, Str in, Str out, Str transport) {
    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Str hostname = os_hostname(a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Error retrieving hostname: %v", err);
        arena_free(&ar);
        return;
    }

    Int pri = SYSLOG_LOG_USER + SYSLOG_LOG_INFO;
    if (is_unix(transport)) {
        Str month = {NULL, 0}, date = {NULL, 0}, ts = {NULL, 0};
        Int pid = 0;
        Str tmpl = fmt_sprintf_v(a, "<%d>%%s %%s %%s syslog_test[%%d]: %s\n", pri, in);
        Int n = fmt_sscanf_v(a, &err, out, tmpl, &month, &date, &ts, &pid);
        if (n != 4 || BURROW_FAILED(err))
            testing_t_errorf_v(t, "Got %q, does not match template %q (%d %s)", out,
                               tmpl, n, err);
        arena_free(&ar);
        return;
    }

    /* Non-UNIX domain transports. */
    Str parsed_hostname = {NULL, 0}, timestamp = {NULL, 0};
    Int pid = 0;
    Str tmpl = fmt_sprintf_v(a, "<%d>%%s %%s syslog_test[%%d]: %s\n", pri, in);
    Int n = fmt_sscanf_v(a, &err, out, tmpl, &timestamp, &parsed_hostname, &pid);
    if (n != 3 || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Got %q, does not match template %q (%d %s)", out, tmpl,
                           n, err);
    if (!str_eq(hostname, parsed_hostname))
        testing_t_errorf_v(t, "Hostname got %q want %q in %q", parsed_hostname,
                           hostname, out);
    arena_free(&ar);
}

/* Takes one string off done, for check, and gives it back after. */
static void check_next(TestingT *t, Str in, Chan *done, Str transport) {
    Str out = {NULL, 0};
    (void)chan_recv(done, &out);
    check(t, in, out, transport);
    free_str(out);
}

static void with_simulated(void *env, TestingT *t) {
    Str tr = *(const Str *)env;
    Str msg = BURROW_S("Test 123");

    testing_t_parallel(t);

    Chan *done = make_done(t, 0);
    Server *sv = start_server(t, tr, BURROW_S(""), done);
    Error err = BURROW_NO_ERROR;
    SyslogWriter *s =
        syslog_dial(heap_allocator(), tr, sv->addr, SYSLOG_LOG_INFO | SYSLOG_LOG_USER,
                    BURROW_S("syslog_test"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Dial() failed: %v", err);
    err = syslog_writer_info(s, msg);
    if (BURROW_FAILED(err)) {
        syslog_writer_free(s);
        testing_t_fatalf_v(t, "log failed: %v", err);
    }
    check_next(t, msg, done, tr);
    (void)syslog_writer_close(s);
    syslog_writer_free(s);
    server_stop(sv);
    if (is_unix(tr))
        (void)os_remove(sv->addr);
}

static void TestWithSimulated(TestingT *t) {
    static const Str nets[] = {BURROW_S_INIT("unix"), BURROW_S_INIT("unixgram"),
                               BURROW_S_INIT("udp"), BURROW_S_INIT("tcp")};

    testing_t_parallel(t);

    for (size_t i = 0; i < sizeof nets / sizeof nets[0]; i++) {
        if (!testable_network(nets[i]))
            continue;

        (void)testing_t_run(
            t, nets[i],
            BURROW_FN(TestingTFunc, with_simulated, (void *)(uintptr_t)&nets[i]));
    }
}

static void TestFlap(TestingT *t) {
    Str net = BURROW_S("unix");
    if (!testable_network(net)) {
        const char *goos = burrow_os_name();
        const char *goarch = burrow_arch_name();
        testing_t_skipf_v(t, "skipping on %s/%s; 'unix' is not supported", goos,
                          goarch);
    }

    Chan *done = make_done(t, 0);
    Server *sv = start_server(t, net, BURROW_S(""), done);
    Str addr = sv->addr;

    Error err = BURROW_NO_ERROR;
    SyslogWriter *s =
        syslog_dial(heap_allocator(), net, addr, SYSLOG_LOG_INFO | SYSLOG_LOG_USER,
                    BURROW_S("syslog_test"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Dial() failed: %v", err);
    Str msg = BURROW_S("Moo 2");
    err = syslog_writer_info(s, msg);
    if (BURROW_FAILED(err)) {
        syslog_writer_free(s);
        testing_t_fatalf_v(t, "log failed: %v", err);
    }
    check_next(t, msg, done, net);

    /* restart the server */
    err = os_remove(addr);
    if (BURROW_FAILED(err)) {
        syslog_writer_free(s);
        testing_t_fatalf_v(t, "%v", err);
    }
    Server *sv2 = start_server(t, net, addr, done);

    /* and try retransmitting */
    msg = BURROW_S("Moo 3");
    err = syslog_writer_info(s, msg);
    if (BURROW_FAILED(err)) {
        syslog_writer_free(s);
        testing_t_fatalf_v(t, "log failed: %v", err);
    }
    check_next(t, msg, done, net);

    (void)syslog_writer_close(s);
    syslog_writer_free(s);
    server_stop(sv2);
    server_stop(sv);
    (void)os_remove(addr);
}

static bool syslogd_missing(Error err) {
    return str_eq(error_text(err), BURROW_S("Unix syslog delivery error"));
}

static void TestNew(TestingT *t) {
    if (SYSLOG_LOG_LOCAL7 != 23 << 3)
        testing_t_fatalf_v(t, "LOG_LOCAL7 has wrong value");
    if (testing_short()) {
        /* Depends on syslog daemon running, and sometimes it's not. */
        testing_t_skip_v(t, "skipping syslog test during -short");
    }

    Error err = BURROW_NO_ERROR;
    SyslogWriter *s = syslog_new(heap_allocator(), SYSLOG_LOG_INFO | SYSLOG_LOG_USER,
                                 BURROW_S("the_tag"), &err);
    if (BURROW_FAILED(err)) {
        if (syslogd_missing(err))
            testing_t_skip_v(t, "skipping: syslogd not running");
        testing_t_fatalf_v(t, "New() failed: %s", err);
    }
    /* Don't send any messages. */
    (void)syslog_writer_close(s);
    syslog_writer_free(s);
}

static void TestNewLogger(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping syslog test during -short");
    Error err = BURROW_NO_ERROR;
    LogLogger *f =
        syslog_new_logger(heap_allocator(), SYSLOG_LOG_USER | SYSLOG_LOG_INFO, 0, &err);
    if (f == NULL) {
        if (syslogd_missing(err))
            testing_t_skip_v(t, "skipping: syslogd not running");
        testing_t_error_v(t, err);
    }
    syslog_logger_free(heap_allocator(), f);
}

static void TestDial(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping syslog test during -short");
    Error err = BURROW_NO_ERROR;
    SyslogWriter *f = syslog_dial(heap_allocator(), BURROW_S(""), BURROW_S(""),
                                  (SYSLOG_LOG_LOCAL7 | SYSLOG_LOG_DEBUG) + 1,
                                  BURROW_S("syslog_test"), &err);
    if (f != NULL)
        testing_t_fatalf_v(t, "Should have trapped bad priority");
    f = syslog_dial(heap_allocator(), BURROW_S(""), BURROW_S(""), -1,
                    BURROW_S("syslog_test"), &err);
    if (f != NULL)
        testing_t_fatalf_v(t, "Should have trapped bad priority");
    SyslogWriter *l =
        syslog_dial(heap_allocator(), BURROW_S(""), BURROW_S(""),
                    SYSLOG_LOG_USER | SYSLOG_LOG_ERR, BURROW_S("syslog_test"), &err);
    if (BURROW_FAILED(err)) {
        if (syslogd_missing(err))
            testing_t_skip_v(t, "skipping: syslogd not running");
        testing_t_fatalf_v(t, "Dial() failed: %s", err);
    }
    (void)syslog_writer_close(l);
    syslog_writer_free(l);
}

static void TestWrite(TestingT *t) {
    static const struct {
        SyslogPriority pri;
        const char *pre;
        const char *msg;
        const char *exp;
    } tests[] = {
        {SYSLOG_LOG_USER | SYSLOG_LOG_ERR, "syslog_test", "",
         "%s %s syslog_test[%d]: \n"},
        {SYSLOG_LOG_USER | SYSLOG_LOG_ERR, "syslog_test", "write test",
         "%s %s syslog_test[%d]: write test\n"},
        /* Write should not add \n if there already is one */
        {SYSLOG_LOG_USER | SYSLOG_LOG_ERR, "syslog_test", "write test 2\n",
         "%s %s syslog_test[%d]: write test 2\n"},
    };

    testing_t_parallel(t);

    Arena ar;
    arena_init(&ar, heap_allocator(), 0);
    Alloc *a = arena_allocator(&ar);
    Error herr = BURROW_NO_ERROR;
    Str hostname = os_hostname(a, &herr);
    if (BURROW_FAILED(herr)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "Error retrieving hostname");
    }
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Chan *done = make_done(t, 0);
        Server *sv = start_server(t, BURROW_S("udp"), BURROW_S(""), done);
        Error err = BURROW_NO_ERROR;
        SyslogWriter *l = syslog_dial(heap_allocator(), BURROW_S("udp"), sv->addr,
                                      tests[i].pri, str_from_cstr(tests[i].pre), &err);
        if (BURROW_FAILED(err)) {
            arena_free(&ar);
            testing_t_fatalf_v(t, "syslog.Dial() failed: %v", err);
        }
        (void)io_write_string(syslog_writer_as_io_writer(l),
                              str_from_cstr(tests[i].msg), &err);
        if (BURROW_FAILED(err)) {
            syslog_writer_free(l);
            arena_free(&ar);
            testing_t_fatalf_v(t, "WriteString() failed: %v", err);
        }
        Str rcvd = {NULL, 0};
        (void)chan_recv(done, &rcvd);
        Int pri = tests[i].pri;
        const char *exp_tail = tests[i].exp;
        Str exp = fmt_sprintf_v(a, "<%d>%s", pri, exp_tail);
        Str parsed_hostname = {NULL, 0}, timestamp = {NULL, 0};
        Int pid = 0;
        Int n = fmt_sscanf_v(a, &err, rcvd, exp, &timestamp, &parsed_hostname, &pid);
        if (n != 3 || BURROW_FAILED(err) || !str_eq(hostname, parsed_hostname))
            testing_t_errorf_v(t, "s.Info() = '%q', didn't match '%q' (%d %s)", rcvd,
                               exp, n, err);
        free_str(rcvd);
        (void)syslog_writer_close(l);
        syslog_writer_free(l);
    }
    arena_free(&ar);
}

typedef struct ConcurrentWrite {
    TestingT *t;
    SyslogWriter *w;
    SyncWaitGroup wg;
} ConcurrentWrite;

static void concurrent_write(void *env) {
    ConcurrentWrite *cw = (ConcurrentWrite *)env;
    Error err = syslog_writer_info(cw->w, BURROW_S("test"));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(cw->t, "Info() failed: %v", err);
    sync_wait_group_done(&cw->wg);
}

static void TestConcurrentWrite(TestingT *t) {
    Server *sv = start_server(t, BURROW_S("udp"), BURROW_S(""), make_done(t, 1));
    Error err = BURROW_NO_ERROR;
    ConcurrentWrite cw;
    memset(&cw, 0, sizeof cw);
    cw.t = t;
    cw.w = syslog_dial(heap_allocator(), BURROW_S("udp"), sv->addr,
                       SYSLOG_LOG_USER | SYSLOG_LOG_ERR, BURROW_S("how's it going?"),
                       &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "syslog.Dial() failed: %v", err);
    for (int i = 0; i < 10; i++) {
        sync_wait_group_add(&cw.wg, 1);
        if (!go(BURROW_FN(Func, concurrent_write, &cw))) {
            sync_wait_group_done(&cw.wg);
            testing_t_errorf_v(t, "no goroutine");
        }
    }
    sync_wait_group_wait(&cw.wg);
    syslog_writer_free(cw.w);
}

enum { N = 10, M = 100 };

typedef struct Reconnect {
    TestingT *t;
    Str net;
    Str addr;
    SyncWaitGroup wg;
} Reconnect;

static void reconnect_one(void *env) {
    Reconnect *r = (Reconnect *)env;
    Error err = BURROW_NO_ERROR;
    SyslogWriter *w =
        syslog_dial(heap_allocator(), r->net, r->addr, SYSLOG_LOG_USER | SYSLOG_LOG_ERR,
                    BURROW_S("tag"), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(r->t, "syslog.Dial() failed: %v", err);
        sync_wait_group_done(&r->wg);
        return;
    }
    for (int i = 0; i < M; i++) {
        err = syslog_writer_info(w, BURROW_S("test"));
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(r->t, "Info() failed: %v", err);
            break;
        }
    }
    (void)syslog_writer_close(w);
    syslog_writer_free(w);
    sync_wait_group_done(&r->wg);
}

typedef struct Counter {
    Chan *done;
    Chan *count;
    SyncWaitGroup wg; /* Not in Go: so the channels outlive it */
} Counter;

/* count all the messages arriving */
static void count_messages(void *env) {
    Counter *c = (Counter *)env;
    Int ct = 0;
    Str s;
    while (chan_recv(c->done, &s)) {
        free_str(s);
        ct++;
        /* we are looking for 500 out of 1000 events
         * here because lots of log messages are lost
         * in buffers (kernel and/or bufio) */
        if (ct > N * M / 2)
            break;
    }
    chan_send(c->count, &ct);
    sync_wait_group_done(&c->wg);
}

static void crashy_off(void *env) {
    (void)env;
    crashy = false;
}

static void count_free(void *env) {
    Counter *c = (Counter *)env;
    sync_wait_group_wait(&c->wg);
    chan_free(c->count);
    mem_free(heap_allocator(), c, sizeof *c, _Alignof(Counter));
}

static void TestConcurrentReconnect(TestingT *t) {
    crashy = true;
    testing_t_cleanup(t, BURROW_FN(Func, crashy_off, NULL));

    Str net = BURROW_S("unix");
    if (!testable_network(net)) {
        net = BURROW_S("tcp");
        if (!testable_network(net)) {
            const char *goos = burrow_os_name();
            const char *goarch = burrow_arch_name();
            testing_t_skipf_v(t,
                              "skipping on %s/%s; neither 'unix' or 'tcp' is supported",
                              goos, goarch);
        }
    }
    Chan *done = make_done(t, N * M);
    Server *sv = start_server(t, net, BURROW_S(""), done);

    Counter *c = mem_alloc(heap_allocator(), sizeof *c, _Alignof(Counter));
    if (c == NULL)
        testing_t_fatalf_v(t, "no memory for the counter");
    c->done = done;
    c->count = chan_make(heap_allocator(), TYPE_INT, 1);
    testing_t_cleanup(t, BURROW_FN(Func, count_free, c));
    if (c->count == NULL)
        testing_t_fatalf_v(t, "no channel");
    sync_wait_group_add(&c->wg, 1);
    if (!go(BURROW_FN(Func, count_messages, c))) {
        sync_wait_group_done(&c->wg);
        testing_t_fatalf_v(t, "no goroutine");
    }

    Reconnect r;
    memset(&r, 0, sizeof r);
    r.t = t;
    r.net = net;
    r.addr = sv->addr;
    sync_wait_group_add(&r.wg, N);
    for (int i = 0; i < N; i++) {
        if (!go(BURROW_FN(Func, reconnect_one, &r))) {
            sync_wait_group_done(&r.wg);
            testing_t_errorf_v(t, "no goroutine");
        }
    }
    sync_wait_group_wait(&r.wg);
    server_stop(sv);
    chan_close(done);

    Int ct = 0;
    Chan *after = time_after_chan(heap_allocator(), 100 * TIME_MILLISECOND);
    SelectCase cases[] = {BURROW_RECV(c->count, &ct), BURROW_RECV(after, NULL)};
    if (chan_select(cases, 2) == 1)
        testing_t_errorf_v(t, "timeout in concurrent reconnect");
    chan_free(after);
    if (str_eq(net, BURROW_S("unix")))
        (void)os_remove(sv->addr);
}

#define TESTS(X)                                                                       \
    X(TestWithSimulated)                                                               \
    X(TestFlap)                                                                        \
    X(TestNew)                                                                         \
    X(TestNewLogger)                                                                   \
    X(TestDial)                                                                        \
    X(TestWrite)                                                                       \
    X(TestConcurrentWrite)                                                             \
    X(TestConcurrentReconnect)

TESTING_MAIN(TESTS)
