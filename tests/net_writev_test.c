/* Buffers and writev, from Go's writev_test.go, with TCPConn's ReadFrom and
 * WriteTo, the listeners' Accept, and the small error types Go has in net.go.
 *
 * TestBuffers_WriteTo counts the writev calls through the hook poll has for
 * it, as Go's does, and wants at least one for every 1024 buffers on the
 * systems with writev and exactly one on Windows, where WSASend takes them
 * all.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/os.h"
#include "burrow/sync.h"

#include "../src/net/internal.h"
#include "check.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(BURROW_NETPOLL_READINESS) && !defined(BURROW_OS_WASI)
#define HAVE_TCP 1
#endif

#define S(lit) BURROW_S(lit)

static Arena ar;
static Alloc *a;

static void need_tcp(TestingT *t) {
#if !defined(HAVE_TCP)
    testing_t_skip_v(t, "TCP here needs the readiness poll FD");
#else
    (void)t;
#endif
}

static Byte loop4_bytes[4] = {127, 0, 0, 1};

static NetTCPListener *listen_loop4(TestingT *t) {
    NetTCPAddr la = {slice_from(loop4_bytes, 4, 4, TYPE_BYTE), 0, BURROW_STR_EMPTY};
    Error e = BURROW_NO_ERROR;
    NetTCPListener *l = net_listen_tcp(heap_allocator(), S("tcp"), &la, &e);
    if (l == NULL)
        testing_t_errorf_v(t, "listen: %v", e);
    return l;
}

static NetTCPConn *dial_to(NetTCPListener *l, Error *err) {
    NetAddr la = net_tcp_listener_addr(l);
    NetTCPAddr ra = {slice_from(loop4_bytes, 4, 4, TYPE_BYTE),
                     ((const NetTCPAddr *)la.data)->port, BURROW_STR_EMPTY};
    return net_dial_tcp(heap_allocator(), S("tcp"), NULL, &ra, err);
}

static IoWriter writer_of(NetTCPConn *c) {
    return net_conn_as_io_writer(net_tcp_conn_as_conn(c));
}

static IoReader reader_of(NetTCPConn *c) {
    return net_conn_as_io_reader(net_tcp_conn_as_conn(c));
}

/* n buffers of one byte each, the i-th being want[i], from the heap, as the
 * arena is not for goroutines. */
static NetBuffers one_byte_buffers(Byte *want, Int n) {
    if (n == 0)
        return slice_nil(TYPE_BYTES);
    Slice *b = (Slice *)mem_alloc(heap_allocator(), (size_t)n * sizeof(Slice),
                                  _Alignof(Slice));
    if (b == NULL)
        return slice_nil(TYPE_BYTES);
    for (Int i = 0; i < n; i++)
        b[i] = slice_from(want + i, 1, 1, TYPE_BYTE);
    return slice_from(b, n, n, TYPE_BYTES);
}

/* -------------------------------------------------------------- Buffers */

static void TestBuffers_read(TestingT *t) {
    static const char story[] = "once upon a time in Gopherland ... ";
    static const char *const parts[] = {"once ", "upon ", "a ",
                                        "time ", "in ",   "Gopherland ... "};
    Slice b[6];
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++)
        b[i] = slice_from((void *)(uintptr_t)parts[i], (Int)strlen(parts[i]),
                          (Int)strlen(parts[i]), TYPE_BYTE);
    NetBuffers v = slice_from(b, 6, 6, TYPE_BYTES);
    Error e = BURROW_NO_ERROR;
    Slice got = io_read_all(a, net_buffers_as_io_reader(&v), &e);
    if (BURROW_FAILED(e)) {
        testing_t_fatalf_v(t, "%v", e);
        return;
    }
    if (!str_eq(str_from_bytes(got.p, got.len), str_from_cstr(story)))
        testing_t_errorf_v(t, "read %q; want %q", str_from_bytes(got.p, got.len),
                           str_from_cstr(story));
    if (v.len != 0)
        testing_t_errorf_v(t, "len(buffers) = %d; want 0", v.len);
}

static void TestBuffers_consume(TestingT *t) {
    static struct {
        const char *in[4];
        Int nin;
        int64_t consume;
        const char *want[2];
        Int nwant;
    } tests[] = {
        {{"foo", "bar"}, 2, 0, {"foo", "bar"}, 2},
        {{"foo", "bar"}, 2, 2, {"o", "bar"}, 2},
        {{"foo", "bar"}, 2, 3, {"bar"}, 1},
        {{"foo", "bar"}, 2, 4, {"ar"}, 1},
        {{NULL, NULL, NULL, "bar"}, 4, 1, {"ar"}, 1},
        {{NULL, NULL, NULL, "foo"}, 4, 0, {"foo"}, 1},
        {{NULL, NULL, NULL}, 3, 0, {NULL}, 0},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Slice b[4];
        for (Int j = 0; j < tests[i].nin; j++) {
            const char *s = tests[i].in[j];
            b[j] = s == NULL ? slice_nil(TYPE_BYTE)
                             : slice_from((void *)(uintptr_t)s, (Int)strlen(s),
                                          (Int)strlen(s), TYPE_BYTE);
        }
        NetBuffers v = slice_from(b, tests[i].nin, tests[i].nin, TYPE_BYTES);
        burrow__net_buffers_consume(&v, tests[i].consume);
        bool ok = v.len == tests[i].nwant;
        const Slice *got = (const Slice *)v.p;
        for (Int j = 0; ok && j < v.len; j++)
            ok = str_eq(str_from_bytes(got[j].p, got[j].len),
                        str_from_cstr(tests[i].want[j]));
        /* What went is nil, so that the buffers can be collected. */
        for (Int j = 0; ok && j < tests[i].nin - v.len; j++)
            ok = b[j].p == NULL && b[j].len == 0;
        if (!ok)
            testing_t_errorf_v(t, "%d. after consume(%d) is not what it should be",
                               (Int)i, tests[i].consume);
    }
}

/* ---------------------------------------------------- Buffers.WriteTo */

static SyncMutex wl_mu;
static Int wl_calls;
static int64_t wl_sum;

static void did_writev(Int n) {
    sync_mutex_lock(&wl_mu);
    wl_calls++;
    wl_sum += n;
    sync_mutex_unlock(&wl_mu);
}

typedef struct PairJob {
    NetTCPListener *l;
    Byte *want;
    Int chunks;
    bool use_copy;
    Error err;
    const char *msg;
    int64_t n;
    Slice all;
} PairJob;

/* The first peer: writes the buffers to the connection it accepts. */
static void write_peer(void *env) {
    PairJob *j = env;
    NetTCPConn *c = net_tcp_listener_accept_tcp(j->l, &j->err);
    if (c == NULL)
        return;
    NetBuffers v = one_byte_buffers(j->want, j->chunks);
    Slice *held = (Slice *)v.p;
    if (j->use_copy)
        j->n = io_copy(heap_allocator(), writer_of(c), net_buffers_as_io_reader(&v),
                       &j->err);
    else
        j->n = net_buffers_write_to(&v, writer_of(c), &j->err);
    if (BURROW_OK(j->err) && v.len != 0)
        j->msg = "len(buffers) != 0";
    if (held != NULL)
        mem_free(heap_allocator(), held, (size_t)j->chunks * sizeof(Slice),
                 _Alignof(Slice));
    net_tcp_conn_free(c);
}

/* The second: reads everything from the connection it dials. */
static void read_peer(void *env) {
    PairJob *j = env;
    NetTCPConn *c = dial_to(j->l, &j->err);
    if (c == NULL)
        return;
    j->all = io_read_all(heap_allocator(), reader_of(c), &j->err);
    net_tcp_conn_free(c);
}

static void buffer_write_to(void *env, TestingT *t) {
    const PairJob *tt = env;
    need_tcp(t);
    Int chunks = tt->chunks;
    Byte *want = (Byte *)mem_alloc(a, (size_t)chunks + 1, 1);
    if (want == NULL) {
        testing_t_fatalf_v(t, "out of memory");
        return;
    }
    for (Int i = 0; i < chunks; i++)
        want[i] = (Byte)i;
    NetTCPListener *l = listen_loop4(t);
    if (l == NULL)
        return;
    wl_calls = 0;
    wl_sum = 0;
    burrow__pfd_set_writev_hook(did_writev);

    PairJob w = {.l = l, .want = want, .chunks = chunks, .use_copy = tt->use_copy};
    PairJob r = {.l = l};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, write_peer, &w));
    sync_wait_group_go(&wg, BURROW_FN(Func, read_peer, &r));
    sync_wait_group_wait(&wg);
    burrow__pfd_set_writev_hook(NULL);
    net_tcp_listener_free(l);

    if (BURROW_FAILED(w.err))
        testing_t_errorf_v(t, "%v", w.err);
    else if (w.msg != NULL)
        testing_t_errorf_v(t, "%s", str_from_cstr(w.msg));
    else if (w.n != (int64_t)chunks)
        testing_t_errorf_v(t, "Buffers.WriteTo returned %d; want %d", w.n, chunks);
    if (BURROW_FAILED(r.err) || r.all.len != chunks ||
        (chunks > 0 && memcmp(r.all.p, want, (size_t)chunks) != 0))
        testing_t_errorf_v(t, "client read %d bytes, %v; want %d, nil", r.all.len,
                           r.err, chunks);
    if (r.all.p != NULL)
        mem_free(heap_allocator(), r.all.p, (size_t)r.all.cap, 1);
#if defined(BURROW_OS_WINDOWS)
    Int want_calls = chunks > 0 ? 1 : 0;
    if (wl_calls != want_calls)
        testing_t_errorf_v(t, "write calls = %d; want %d", wl_calls, want_calls);
#else
    Int want_min_calls = (chunks + 1023) / 1024;
    if (wl_calls < want_min_calls)
        testing_t_errorf_v(t, "write calls = %d < wanted min %d", wl_calls,
                           want_min_calls);
#endif
    if (wl_sum != (int64_t)chunks)
        testing_t_errorf_v(t, "writev call sum  = %d; want %d", wl_sum, chunks);
}

static void TestBuffers_WriteTo(TestingT *t) {
    static const char *const names[] = {"WriteTo", "Copy"};
    static const Int sizes[] = {0, 10, 1023, 1024, 1025};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
            char name[32];
            (void)snprintf(name, sizeof name, "%s/%d", names[i], (int)sizes[k]);
            PairJob tt = {.chunks = sizes[k], .use_copy = i == 1};
            testing_t_run(t, str_from_cstr(name),
                          BURROW_FN(TestingTFunc, buffer_write_to, &tt));
        }
    }
}

typedef struct AcceptJob {
    NetTCPListener *l;
    NetConn c;
    Error err;
} AcceptJob;

static void accept_job(void *env) {
    AcceptJob *j = env;
    j->c = net_tcp_listener_accept(j->l, &j->err);
}

#if !defined(BURROW_OS_WINDOWS)
static void writev_error(TestingT *t) {
    need_tcp(t);
    NetTCPListener *l = listen_loop4(t);
    if (l == NULL)
        return;
    AcceptJob j = {.l = l};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, accept_job, &j));
    Error e = BURROW_NO_ERROR;
    NetTCPConn *c1 = dial_to(l, &e);
    sync_wait_group_wait(&wg);
    net_tcp_listener_free(l);
    if (c1 == NULL) {
        testing_t_errorf_v(t, "%v", e);
        net_conn_free(j.c);
        return;
    }
    if (j.c.vt == NULL) {
        testing_t_errorf_v(t, "no server side connection: %v", j.err);
        net_tcp_conn_free(c1);
        return;
    }
    net_conn_free(j.c);

    /* 1 GB of data should be enough to notice the connection is gone, and
     * the same 1 MB buffer a thousand times over is all it takes. */
    Byte *buf = (Byte *)mem_alloc(a, (size_t)1 << 20, 1);
    Slice *bs = (Slice *)mem_alloc(a, (size_t)1024 * sizeof(Slice), _Alignof(Slice));
    if (buf == NULL || bs == NULL) {
        testing_t_errorf_v(t, "out of memory");
        net_tcp_conn_free(c1);
        return;
    }
    for (Int i = 0; i < 1024; i++)
        bs[i] = slice_from(buf, (Int)1 << 20, (Int)1 << 20, TYPE_BYTE);
    NetBuffers v = slice_from(bs, 1024, 1024, TYPE_BYTES);
    (void)net_buffers_write_to(&v, writer_of(c1), &e);
    if (BURROW_OK(e))
        testing_t_errorf_v(t, "Buffers.WriteTo(closed conn) succeeded, want error");
    else if (errors_as(e, TYPE_NET_OP_ERROR) == NULL)
        testing_t_errorf_v(t, "got %v; want an OpError", e);
    net_tcp_conn_free(c1);
}
#endif

static void TestWritevError(TestingT *t) {
#if defined(BURROW_OS_WINDOWS)
    testing_t_skip_v(t,
                     "skipping the test: windows does not have problem sending large "
                     "chunks of data");
#else
    writev_error(t);
#endif
}

/* A writer that is not a connection gets the buffers one Write at a time. */
static void TestBuffersWriteToAnythingElse(TestingT *t) {
    static const char *const parts[] = {"one", "", "two", "three"};
    Slice b[4];
    for (size_t i = 0; i < 4; i++)
        b[i] = slice_from((void *)(uintptr_t)parts[i], (Int)strlen(parts[i]),
                          (Int)strlen(parts[i]), TYPE_BYTE);
    NetBuffers v = slice_from(b, 4, 4, TYPE_BYTES);
    BytesBuffer out = BYTES_BUFFER(a);
    Error e = BURROW_NO_ERROR;
    int64_t n = net_buffers_write_to(&v, bytes_buffer_as_io_writer(&out), &e);
    CHECK(BURROW_OK(e));
    CHECK_INT_EQ(n, 11);
    CHECK_INT_EQ(v.len, 0);
    Slice got = bytes_buffer_bytes(&out);
    CHECK(str_eq(str_from_bytes(got.p, got.len), S("onetwothree")));
    bytes_buffer_free(&out);
}

/* ------------------------------------------------ ReadFrom and WriteTo */

typedef struct CopyJob {
    NetTCPListener *l;
    Slice data;
    Error err;
    int64_t n;
} CopyJob;

/* Accepts one connection and writes data to it with WriteTo, from the
 * accepted end's point of view a ReadFrom. */
static void read_from_job(void *env) {
    CopyJob *j = env;
    NetTCPConn *c = net_tcp_listener_accept_tcp(j->l, &j->err);
    if (c == NULL)
        return;
    /* A limited reader has no WriteTo, so io_copy asks the conn for ReadFrom. */
    BytesReader br;
    bytes_reader_reset(&br, j->data);
    IoLimitedReader lr = io_limit_reader(bytes_reader_as_io_reader(&br), j->data.len);
    j->n = io_copy(heap_allocator(), writer_of(c), io_limited_reader_as_io_reader(&lr),
                   &j->err);
    net_tcp_conn_free(c);
}

static void TestTCPConnReadFromAndWriteTo(TestingT *t) {
    need_tcp(t);
    NetTCPListener *l = listen_loop4(t);
    if (l == NULL)
        return;
    Int size = 100000;
    Byte *data = (Byte *)mem_alloc(a, (size_t)size, 1);
    if (data == NULL) {
        testing_t_fatalf_v(t, "out of memory");
        return;
    }
    for (Int i = 0; i < size; i++)
        data[i] = (Byte)(i * 7);
    CopyJob j = {.l = l, .data = slice_from(data, size, size, TYPE_BYTE)};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, read_from_job, &j));
    Error e = BURROW_NO_ERROR;
    NetTCPConn *c = dial_to(l, &e);
    BytesBuffer out = BYTES_BUFFER(a);
    int64_t n = 0;
    if (c != NULL) {
        /* The conn is the reader, and its WriteTo comes first. */
        n = io_copy(heap_allocator(), bytes_buffer_as_io_writer(&out), reader_of(c),
                    &e);
        net_tcp_conn_free(c);
    }
    sync_wait_group_wait(&wg);
    net_tcp_listener_free(l);
    if (BURROW_FAILED(j.err) || j.n != size)
        testing_t_errorf_v(t, "ReadFrom: %d, %v; want %d, nil", j.n, j.err, size);
    if (BURROW_FAILED(e) || n != size)
        testing_t_errorf_v(t, "WriteTo: %d, %v; want %d, nil", n, e, size);
    Slice got = bytes_buffer_bytes(&out);
    CHECK(got.len == size && memcmp(got.p, data, (size_t)size) == 0);
    bytes_buffer_free(&out);
}

static bool has_prefix(Error e, const char *p) {
    Str s = error_text(e);
    size_t n = strlen(p);
    return (size_t)s.len >= n && memcmp(s.p, p, n) == 0;
}

static bool contains(Error e, const char *p) {
    Str s = error_text(e);
    size_t n = strlen(p);
    for (Int i = 0; i + (Int)n <= s.len; i++)
        if (memcmp(s.p + i, p, n) == 0)
            return true;
    return false;
}

/* What goes wrong in the copy is in an OpError named for the method, around
 * the Read or Write's own. */
static void TestTCPConnCopyErrorsSayWhichWay(TestingT *t) {
    need_tcp(t);
    NetTCPListener *l = listen_loop4(t);
    if (l == NULL)
        return;
    AcceptJob j = {.l = l};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, accept_job, &j));
    Error e = BURROW_NO_ERROR;
    NetTCPConn *c = dial_to(l, &e);
    sync_wait_group_wait(&wg);
    net_tcp_listener_free(l);
    net_conn_free(j.c);
    if (c == NULL) {
        testing_t_errorf_v(t, "%v", e);
        return;
    }
    CHECK(BURROW_OK(net_tcp_conn_close(c)));

    BytesReader br;
    bytes_reader_reset(&br, slice_from(loop4_bytes, 4, 4, TYPE_BYTE));
    (void)net_tcp_conn_read_from(c, bytes_reader_as_io_reader(&br), &e);
    if (!has_prefix(e, "readfrom tcp 127.0.0.1:") ||
        !contains(e, ": write tcp 127.0.0.1:") ||
        !contains(e, "use of closed network connection"))
        testing_t_errorf_v(t, "ReadFrom: %v", e);
    const NetOpError *oe = errors_as(e, TYPE_NET_OP_ERROR);
    CHECK(oe != NULL && str_eq(oe->op, S("readfrom")));
    CHECK(errors_is(e, net_err_closed));

    BytesBuffer out = BYTES_BUFFER(a);
    (void)net_tcp_conn_write_to(c, bytes_buffer_as_io_writer(&out), &e);
    if (!has_prefix(e, "writeto tcp 127.0.0.1:") ||
        !contains(e, ": read tcp 127.0.0.1:"))
        testing_t_errorf_v(t, "WriteTo: %v", e);
    CHECK(errors_is(e, net_err_closed));
    bytes_buffer_free(&out);

    CHECK(net_tcp_conn_read_from(NULL, bytes_reader_as_io_reader(&br), &e) == 0);
    CHECK(BURROW_FAILED(e));
    net_tcp_conn_free(c);
}

/* -------------------------------------------------------------- Accept */

typedef struct UnixAcceptJob {
    NetUnixListener *l;
    NetConn c;
    Error err;
} UnixAcceptJob;

static void unix_accept_job(void *env) {
    UnixAcceptJob *j = env;
    j->c = net_unix_listener_accept(j->l, &j->err);
}

static void TestUnixListenerAcceptIsAConn(TestingT *t) {
    need_tcp(t);
    Error e = BURROW_NO_ERROR;
    Str dir = os_mkdir_temp(a, S(""), S("bxw"), &e);
    if (BURROW_FAILED(e)) {
        testing_t_errorf_v(t, "MkdirTemp: %v", e);
        return;
    }
    Byte *p = (Byte *)mem_alloc(a, (size_t)dir.len + 2, 1);
    if (p == NULL) {
        (void)os_remove_all(dir);
        return;
    }
    memcpy(p, dir.p, (size_t)dir.len);
    p[dir.len] = '/';
    p[dir.len + 1] = 's';
    NetUnixAddr addr = {str_from_bytes(p, dir.len + 2), S("unix")};
    NetUnixListener *l = net_listen_unix(heap_allocator(), S("unix"), &addr, &e);
    if (l == NULL) {
        testing_t_errorf_v(t, "listen: %v", e);
        (void)os_remove_all(dir);
        return;
    }
    UnixAcceptJob j = {.l = l};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, unix_accept_job, &j));
    NetUnixConn *c = net_dial_unix(heap_allocator(), S("unix"), NULL, &addr, &e);
    sync_wait_group_wait(&wg);
    if (c == NULL || j.c.vt == NULL) {
        testing_t_errorf_v(t, "dial: %v; accept: %v", e, j.err);
    } else {
        CHECK(net_conn_as_unix_conn(j.c) != NULL);
        char hi[] = "hi";
        Slice b = slice_from(hi, 2, 2, TYPE_BYTE);
        CHECK_INT_EQ(net_unix_conn_write(c, b, &e), 2);
        char got[2] = {0, 0};
        Slice g = slice_from(got, 2, 2, TYPE_BYTE);
        CHECK_INT_EQ(io_read_full(net_conn_as_io_reader(j.c), g, &e), 2);
        CHECK(memcmp(got, "hi", 2) == 0);
    }
    net_conn_free(j.c);
    net_unix_conn_free(c);
    net_unix_listener_free(l);
    (void)os_remove_all(dir);
}

/* ---------------------------------------------------------- the errors */

static void TestTheSmallErrorTypes(TestingT *t) {
    Str s = net_unknown_network_error_error(S("foo"), a);
    CHECK(str_eq(s, S("unknown network foo")));
    CHECK(!net_unknown_network_error_timeout(S("foo")));
    CHECK(!net_unknown_network_error_temporary(S("foo")));

    Error e = net_invalid_addr_error(error_allocator(), S("bad address"));
    CHECK(str_eq(error_text(e), S("bad address")));
    CHECK(str_eq(net_error_error(e), S("bad address")));
    CHECK(!net_error_timeout(e) && !net_error_temporary(e));
    const Str *ia = errors_as(e, TYPE_NET_INVALID_ADDR_ERROR);
    CHECK(ia != NULL && str_eq(*ia, S("bad address")));
    CHECK(str_eq(net_invalid_addr_error_error(S("x")), S("x")));
    CHECK(!net_invalid_addr_error_timeout(S("x")));
    CHECK(!net_invalid_addr_error_temporary(S("x")));

    Error inner = errors_new(error_allocator(), S("open /etc/resolv.conf: denied"));
    NetDNSConfigError de = {inner};
    s = net_dns_config_error_error(&de, a);
    CHECK(str_eq(s, S("error reading DNS config: open /etc/resolv.conf: denied")));
    CHECK(errors_is(net_dns_config_error_unwrap(&de), inner));
    CHECK(!net_dns_config_error_timeout(&de) && !net_dns_config_error_temporary(&de));
    Error w = net_dns_config_error_as_error(&de, error_allocator());
    CHECK(str_eq(error_text(w), s));
    CHECK(errors_is(w, inner));
    CHECK(errors_is(errors_unwrap(w), inner));
    const NetDNSConfigError *dp = errors_as(w, TYPE_NET_DNS_CONFIG_ERROR);
    CHECK(dp != NULL && errors_is(dp->err, inner));
    CHECK(!net_error_timeout(w) && !net_error_temporary(w));
}

#define TESTS(X)                                                                       \
    X(TestBuffers_read)                                                                \
    X(TestBuffers_consume)                                                             \
    X(TestBuffers_WriteTo)                                                             \
    X(TestWritevError)                                                                 \
    X(TestBuffersWriteToAnythingElse)                                                  \
    X(TestTCPConnReadFromAndWriteTo)                                                   \
    X(TestTCPConnCopyErrorsSayWhichWay)                                                \
    X(TestUnixListenerAcceptIsAConn)                                                   \
    X(TestTheSmallErrorTypes)

static int net_writev_main(TestingM *m) {
    arena_init(&ar, NULL, 0);
    a = arena_allocator(&ar);
    int code = testing_m_run(m);
    arena_free(&ar);
    return code;
}

TESTING_MAIN_WITH(net_writev_main, TESTS)
