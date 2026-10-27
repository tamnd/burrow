/* Derived from Go's src/internal/nettest/nettest_test.go, conn_test.go and
 * listener_test.go. Go source: go1.27.3.
 *
 * Go leaves the Conns and Listeners to its collector. Here each test frees
 * the ones it made at the end, after a synctest_wait so that no goroutine it
 * started is still using them, and those lines are marked "Not in Go". A flag
 * a goroutine sets and the test reads after synctest_wait is an atomic.
 *
 * Copyright 2026 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "../src/net/nettest.h"

#include "burrow/atomic.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/error.h"
#include "burrow/io.h"
#include "burrow/mem/heap.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/os.h"
#include "burrow/synctest.h"
#include "burrow/time.h"

#include "check.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define S BURROW_S

static Alloc *heap(void) {
    return heap_allocator();
}

static bool same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* A slice over a string literal, which a Write only reads. */
static Slice lit(const char *s) {
    Int n = (Int)strlen(s);
    return slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static Str text(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

static bool done_load(uint32_t *done) {
    return burrow__atomic_load_acquire_u32(done) != 0;
}

static void done_store(uint32_t *done) {
    burrow__atomic_store_release_u32(done, 1);
}

static void free_pair(NettestConn *c) {
    burrow__nettest_conn_free(burrow__nettest_conn_peer(c));
    burrow__nettest_conn_free(c);
}

/* -------------------------------------------------------- nettest_test.go */

typedef void (*BodyFunc)(TestingT *t, void *env);

typedef struct Sub {
    BodyFunc f;
    void *env;
} Sub;

static void sub_bubble(void *env, TestingT *t) {
    Sub *s = env;
    s->f(t, s->env);
}

static void sub_run(void *env, TestingT *t) {
    synctest_test(t, BURROW_FN(TestingTFunc, sub_bubble, env));
}

static void synctest_subtest(TestingT *t, const char *name, BodyFunc f, void *env) {
    Sub s = {f, env};
    testing_t_run(t, str_from_cstr(name), BURROW_FN(TestingTFunc, sub_run, &s));
}

static void synctest_top(TestingT *t, void (*f)(void *env, TestingT *t)) {
    synctest_test(t, BURROW_FN(TestingTFunc, f, NULL));
}

/* A deadlineTest describes an operation which blocks until a deadline,
 * and a separate operation which unblocks it. */
typedef struct DeadlineTest DeadlineTest;
struct DeadlineTest {
    const char *what;                /* name of the blocking op; e.g., "Read" */
    Error (*block)(DeadlineTest *d); /* blocking op; e.g., reading from a conn */
    void (*unblock)(
        DeadlineTest *d); /* unblocking op; e.g. writing to the other side */
    void (*set_deadline)(DeadlineTest *d,
                         Duration dur); /* deadline func; e.g., SetReadDeadline */
    NettestConn *rconn, *wconn;
    int unblock_kind, set_kind;
};

/* What testDeadline's setup closes over: which of the tables' entries the
 * test is for. */
typedef struct DeadlineCase {
    void (*setup)(const struct DeadlineCase *c, DeadlineTest *d);
    int unblock_kind, set_kind;
} DeadlineCase;

typedef struct BlockJob {
    TestingT *t;
    DeadlineTest *test;
    Time start;
    Duration delay;
    uint32_t done;
} BlockJob;

static void block_not_deadline(void *env) {
    BlockJob *j = env;
    Error err = j->test->block(j->test);
    if (errors_is(err, os_err_deadline_exceeded))
        testing_t_errorf_v(j->t, "%s: %v, want not deadline exceeded", j->test->what,
                           err);
    done_store(&j->done);
}

static void block_deadline_expires(void *env) {
    BlockJob *j = env;
    Error err = j->test->block(j->test);
    if (!errors_is(err, os_err_deadline_exceeded))
        testing_t_errorf_v(j->t, "%s: %v, want os.ErrDeadlineExceeded", j->test->what,
                           err);
    Duration got = time_since(j->start), want = j->delay;
    if (got != want)
        testing_t_errorf_v(j->t, "%s: returned after %v, want %v", j->test->what,
                           duration_string(got, error_allocator()),
                           duration_string(want, error_allocator()));
    done_store(&j->done);
}

static void block_deadline_exceeded(void *env) {
    BlockJob *j = env;
    Error err = j->test->block(j->test);
    if (!errors_is(err, os_err_deadline_exceeded))
        testing_t_errorf_v(j->t, "%s: %v, want os.ErrDeadlineExceeded", j->test->what,
                           err);
    done_store(&j->done);
}

/* Not in Go: waits for the goroutines the test started and frees its Conns. */
static void deadline_test_free(DeadlineTest *d) {
    synctest_wait();
    burrow__nettest_conn_free(d->rconn);
    burrow__nettest_conn_free(d->wconn);
}

static void dl_no_deadline(TestingT *t, void *env) {
    const DeadlineCase *c = env;
    DeadlineTest test;
    c->setup(c, &test);
    test.unblock(&test);
    synctest_wait();
    Error err = test.block(&test);
    if (errors_is(err, os_err_deadline_exceeded))
        testing_t_errorf_v(t, "%s: %v, want not deadline exceeded", test.what, err);
    deadline_test_free(&test);
}

static void dl_unblock_before_setdeadline(TestingT *t, void *env) {
    const DeadlineCase *c = env;
    DeadlineTest test;
    c->setup(c, &test);
    test.unblock(&test);
    synctest_wait();
    test.set_deadline(&test, 5 * TIME_SECOND);
    Error err = test.block(&test);
    if (errors_is(err, os_err_deadline_exceeded))
        testing_t_errorf_v(t, "%s: %v, want not deadline exceeded", test.what, err);
    deadline_test_free(&test);
}

static void dl_unblock_after_blocking(TestingT *t, void *env) {
    const DeadlineCase *c = env;
    DeadlineTest test;
    c->setup(c, &test);
    test.set_deadline(&test, 5 * TIME_SECOND);
    BlockJob j = {t, &test, {0}, 0, 0};
    go(BURROW_FN(Func, block_not_deadline, &j));
    synctest_wait();
    if (done_load(&j.done))
        testing_t_fatalf_v(t, "%s: unexpectedly returned before unblocking", test.what);
    test.unblock(&test);
    synctest_wait();
    if (!done_load(&j.done))
        testing_t_fatalf_v(t, "%s: did not return after unblocking", test.what);
    deadline_test_free(&test);
}

static void dl_deadline_expires(TestingT *t, void *env) {
    const DeadlineCase *c = env;
    DeadlineTest test;
    c->setup(c, &test);
    Time start = time_now();
    const Duration delay = 5 * TIME_SECOND;
    test.set_deadline(&test, delay);
    BlockJob j = {t, &test, start, delay, 0};
    go(BURROW_FN(Func, block_deadline_expires, &j));
    synctest_wait();
    if (done_load(&j.done))
        testing_t_fatalf_v(t, "%s: unexpectedly returned before unblocking", test.what);
    time_sleep(delay);
    synctest_wait();
    if (!done_load(&j.done))
        testing_t_fatalf_v(t, "%s: did not return after deadline", test.what);
    deadline_test_free(&test);
}

static void dl_deadline_already_expired(TestingT *t, void *env) {
    const DeadlineCase *c = env;
    DeadlineTest test;
    c->setup(c, &test);
    test.set_deadline(&test, -1 * TIME_SECOND);
    test.unblock(&test);
    synctest_wait();
    Error err = test.block(&test);
    if (!errors_is(err, os_err_deadline_exceeded))
        testing_t_errorf_v(t, "%s: %v, want os.ErrDeadlineExceeded", test.what, err);
    deadline_test_free(&test);
}

static void dl_reduce_deadline_after_blocking(TestingT *t, void *env) {
    const DeadlineCase *c = env;
    DeadlineTest test;
    c->setup(c, &test);
    test.set_deadline(&test, 5 * TIME_SECOND);
    BlockJob j = {t, &test, {0}, 0, 0};
    go(BURROW_FN(Func, block_deadline_exceeded, &j));
    synctest_wait();
    if (done_load(&j.done))
        testing_t_fatalf_v(t, "%s: unexpectedly returned before reducing deadline",
                           test.what);
    test.set_deadline(&test, -1 * TIME_SECOND);
    synctest_wait();
    if (!done_load(&j.done))
        testing_t_fatalf_v(t, "%s: did not return after deadline", test.what);
    deadline_test_free(&test);
}

/* testDeadline tests a variety of scenarios involving deadlines. */
static void test_deadline(TestingT *t, DeadlineCase *c) {
    synctest_subtest(t, "no deadline", dl_no_deadline, c);
    synctest_subtest(t, "unblock before setdeadline", dl_unblock_before_setdeadline, c);
    synctest_subtest(t, "unblock after blocking", dl_unblock_after_blocking, c);
    synctest_subtest(t, "deadline expires", dl_deadline_expires, c);
    synctest_subtest(t, "deadline already expired", dl_deadline_already_expired, c);
    synctest_subtest(t, "reduce deadline after blocking",
                     dl_reduce_deadline_after_blocking, c);
}

/* ---------------------------------------------------------- conn_test.go */

static Int conn_read(NettestConn *c, Slice b, Error *err) {
    return burrow__nettest_conn_read(c, b, err);
}

static Int conn_write(NettestConn *c, Slice b, Error *err) {
    return burrow__nettest_conn_write(c, b, err);
}

static IoReader conn_reader(NettestConn *c) {
    return net_conn_as_io_reader(burrow__nettest_conn_as_net_conn(c));
}

static void slice_free(Slice b) {
    mem_free(heap(), b.p, (size_t)b.cap, 1);
}

static void test_conn_read_write(void *env, TestingT *t) {
    (void)env;
    NettestConn *cli_conn, *srv_conn;
    burrow__nettest_new_conn_pair(heap(), &cli_conn, &srv_conn);

    Slice cli_data = lit("hello");
    Slice srv_data = lit("HELLO");
    Error err;
    Int n = conn_write(cli_conn, cli_data, &err);
    if (n != cli_data.len || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "cliConn.Write(%q) = %v, %v; want %v, nil",
                           text(cli_data), n, err, cli_data.len);
    err = burrow__nettest_conn_close_write(cli_conn);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "cliConn.CloseWrite() = %v, want nil", err);
    n = conn_write(srv_conn, srv_data, &err);
    if (n != srv_data.len || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "srvConn.Write(%q) = %v, %v; want %v, nil",
                           text(srv_data), n, err, srv_data.len);
    err = burrow__nettest_conn_close_write(srv_conn);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "cliConn.CloseWrite() = %v, want nil", err);
    Slice got_cli = io_read_all(heap(), conn_reader(cli_conn), &err);
    if (!bytes_equal(got_cli, srv_data) || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "io.ReadAll(cliConn) = %q, %v; want %v, nil",
                           text(got_cli), err, text(srv_data));
    Slice got_srv = io_read_all(heap(), conn_reader(srv_conn), &err);
    if (!bytes_equal(got_srv, cli_data) || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "io.ReadAll(srvConn) = %q, %v; want %v, nil",
                           text(got_srv), err, text(cli_data));
    slice_free(got_cli); /* Not in Go. */
    slice_free(got_srv);
    free_pair(cli_conn);
}

static void TestConnReadWrite(TestingT *t) {
    synctest_top(t, test_conn_read_write);
}

typedef struct ZeroBuffer {
    NettestConn *rconn, *wconn;
    uint32_t read_done, write_done;
} ZeroBuffer;

static void zero_buffer_read(void *env) {
    ZeroBuffer *z = env;
    Byte buf[100];
    Error err;
    (void)conn_read(z->rconn, slice_from(buf, 100, 100, TYPE_BYTE), &err);
    done_store(&z->read_done);
}

static void zero_buffer_write(void *env) {
    ZeroBuffer *z = env;
    Error err;
    (void)conn_write(z->wconn, lit("a"), &err);
    done_store(&z->write_done);
}

static void test_conn_zero_buffer(void *env, TestingT *t) {
    (void)env;
    ZeroBuffer z = {NULL, NULL, 0, 0};
    burrow__nettest_new_conn_pair(heap(), &z.rconn, &z.wconn);
    burrow__nettest_conn_set_read_buffer_size(z.rconn, 0);
    go(BURROW_FN(Func, zero_buffer_read, &z));
    go(BURROW_FN(Func, zero_buffer_write, &z));
    synctest_wait();
    bool read_done = done_load(&z.read_done), write_done = done_load(&z.write_done);
    if (read_done || write_done)
        testing_t_errorf_v(t,
                           "before unblocking: readDone=%v, writeDone=%v; want false",
                           read_done, write_done);
    burrow__nettest_conn_close(z.wconn);
    synctest_wait();
    read_done = done_load(&z.read_done);
    write_done = done_load(&z.write_done);
    if (!read_done || !write_done)
        testing_t_errorf_v(t, "after unblocking: readDone=%v, writeDone=%v; want true",
                           read_done, write_done);
    free_pair(z.rconn); /* Not in Go. */
}

/* Exercise the case where one side of the conn is blocked writing and the
 * other side is blocked reading.
 * This can only happen when the read buffer has been set to 0, blocking all writes. */
static void TestConnZeroBuffer(TestingT *t) {
    synctest_top(t, test_conn_zero_buffer);
}

enum { READ_SIZE = 5 };

typedef struct PartialWrite {
    TestingT *t;
    NettestConn *rconn;
    Slice data;
} PartialWrite;

static void partial_write_read(void *env) {
    PartialWrite *p = env;
    TestingT *t = p->t;
    Byte got[READ_SIZE];
    Error err;
    Int n = io_read_full(conn_reader(p->rconn),
                         slice_from(got, READ_SIZE, READ_SIZE, TYPE_BYTE), &err);
    if (n != READ_SIZE || BURROW_FAILED(err))
        testing_t_errorf_v(t, "io.ReadFull() = %v, %v; want %v, nil", n, err,
                           READ_SIZE);
    Slice want = slice_sub(p->data, 0, READ_SIZE);
    if (!bytes_equal(slice_from(got, READ_SIZE, READ_SIZE, TYPE_BYTE), want))
        testing_t_errorf_v(t, "read %q, want %q", str_from_bytes(got, READ_SIZE),
                           text(want));
    synctest_wait();
    burrow__nettest_conn_close(p->rconn);
}

static void test_conn_partial_write(void *env, TestingT *t) {
    (void)env;
    Slice data = lit("0123456789");
    NettestConn *rconn, *wconn;
    burrow__nettest_new_conn_pair(heap(), &rconn, &wconn);
    burrow__nettest_conn_set_read_buffer_size(rconn, 1);
    PartialWrite p = {t, rconn, data};
    go(BURROW_FN(Func, partial_write_read, &p));
    Error err;
    Int n = conn_write(wconn, data, &err);
    if (n != READ_SIZE + 1 || !BURROW_FAILED(err))
        testing_t_errorf_v(t, "Write() = %v, %v; want %v, error", n, err,
                           READ_SIZE + 1);
    synctest_wait(); /* Not in Go. */
    free_pair(rconn);
}

/* A blocking write to a conn successfully writes some, but not all data. */
static void TestConnPartialWrite(TestingT *t) {
    synctest_top(t, test_conn_partial_write);
}

static const char *const read_unblock_names[] = {"Write", "Close", "CloseWrite"};
static const char *const read_set_names[] = {"SetDeadline", "SetReadDeadline"};

static Error read_block(DeadlineTest *d) {
    Byte b[1];
    Error err;
    (void)conn_read(d->rconn, slice_from(b, 1, 1, TYPE_BYTE), &err);
    return err;
}

static void read_unblock(DeadlineTest *d) {
    Error err;
    switch (d->unblock_kind) {
    case 0:
        (void)conn_write(d->wconn, lit("x"), &err);
        break;
    case 1:
        (void)burrow__nettest_conn_close(d->wconn);
        break;
    default:
        (void)burrow__nettest_conn_close_write(d->wconn);
        break;
    }
}

static void read_set_deadline(DeadlineTest *d, Duration dur) {
    Time when = time_add(time_now(), dur);
    if (d->set_kind == 0)
        (void)burrow__nettest_conn_set_deadline(d->rconn, when);
    else
        (void)burrow__nettest_conn_set_read_deadline(d->rconn, when);
}

static void read_setup(const DeadlineCase *c, DeadlineTest *d) {
    memset(d, 0, sizeof *d);
    burrow__nettest_new_conn_pair(heap(), &d->rconn, &d->wconn);
    d->what = "Read()";
    d->block = read_block;
    d->unblock = read_unblock;
    d->set_deadline = read_set_deadline;
    d->unblock_kind = c->unblock_kind;
    d->set_kind = c->set_kind;
}

static void run_deadline_case(void *env, TestingT *t) {
    test_deadline(t, env);
}

static void TestConnReadDeadline(TestingT *t) {
    for (int u = 0; u < 3; u++) {
        for (int s = 0; s < 2; s++) {
            DeadlineCase c = {read_setup, u, s};
            char name[64];
            snprintf(name, sizeof name, "%s/%s", read_unblock_names[u],
                     read_set_names[s]);
            testing_t_run(t, str_from_cstr(name),
                          BURROW_FN(TestingTFunc, run_deadline_case, &c));
        }
    }
}

static const char *const write_unblock_names[] = {"Read", "Close", "CloseRead"};
static const char *const write_set_names[] = {"SetDeadline", "SetWriteDeadline"};

static Error write_block(DeadlineTest *d) {
    Error err;
    (void)conn_write(d->wconn, lit("1234"), &err);
    burrow__nettest_conn_close(d->wconn);
    return err;
}

static void write_unblock_job(void *env) {
    DeadlineTest *d = env;
    Error err;
    switch (d->unblock_kind) {
    case 0:
        (void)io_copy(heap(), io_discard, conn_reader(d->rconn), &err);
        break;
    case 1:
        (void)burrow__nettest_conn_close(d->rconn);
        break;
    default:
        (void)burrow__nettest_conn_close_read(d->rconn);
        break;
    }
}

static void write_unblock(DeadlineTest *d) {
    go(BURROW_FN(Func, write_unblock_job, d));
}

static void write_set_deadline(DeadlineTest *d, Duration dur) {
    Time when = time_add(time_now(), dur);
    if (d->set_kind == 0)
        (void)burrow__nettest_conn_set_deadline(d->wconn, when);
    else
        (void)burrow__nettest_conn_set_write_deadline(d->wconn, when);
}

static void write_setup(const DeadlineCase *c, DeadlineTest *d) {
    memset(d, 0, sizeof *d);
    burrow__nettest_new_conn_pair(heap(), &d->rconn, &d->wconn);
    burrow__nettest_conn_set_read_buffer_size(d->rconn, 1);
    d->what = "Write()";
    d->block = write_block;
    d->unblock = write_unblock;
    d->set_deadline = write_set_deadline;
    d->unblock_kind = c->unblock_kind;
    d->set_kind = c->set_kind;
}

static void TestConnWriteDeadline(TestingT *t) {
    for (int u = 0; u < 3; u++) {
        for (int s = 0; s < 2; s++) {
            DeadlineCase c = {write_setup, u, s};
            char name[64];
            snprintf(name, sizeof name, "%s/%s", write_unblock_names[u],
                     write_set_names[s]);
            testing_t_run(t, str_from_cstr(name),
                          BURROW_FN(TestingTFunc, run_deadline_case, &c));
        }
    }
}

static void test_conn_can_read(void *env, TestingT *t) {
    (void)env;
    NettestConn *rconn, *wconn;
    burrow__nettest_new_conn_pair(heap(), &rconn, &wconn);
    bool got = burrow__nettest_conn_can_read(rconn), want = false;
    if (got != want)
        testing_t_fatalf_v(t, "before writing data: rconn.CanRead() = %v, want %v", got,
                           want);
    Error err;
    (void)conn_write(wconn, lit("a"), &err);
    got = burrow__nettest_conn_can_read(rconn);
    want = true;
    if (got != want)
        testing_t_fatalf_v(t, "after writing data: rconn.CanRead() = %v, want %v", got,
                           want);
    Byte b[1];
    (void)conn_read(rconn, slice_from(b, 1, 1, TYPE_BYTE), &err);
    got = burrow__nettest_conn_can_read(rconn);
    want = false;
    if (got != want)
        testing_t_fatalf_v(t, "after reading data: rconn.CanRead() = %v, want %v", got,
                           want);
    burrow__nettest_conn_close(wconn);
    got = burrow__nettest_conn_can_read(rconn);
    want = true;
    if (got != want)
        testing_t_fatalf_v(t, "after closing: rconn.CanRead() = %v, want %v", got,
                           want);
    free_pair(rconn); /* Not in Go. */
}

static void TestConnCanRead(TestingT *t) {
    synctest_top(t, test_conn_can_read);
}

typedef struct IsClosedCase {
    const char *name;
    NettestConn *(*f)(void);
    bool want;
} IsClosedCase;

static NettestConn *new_conn(void) {
    NettestConn *conn, *peer;
    burrow__nettest_new_conn_pair(heap(), &conn, &peer);
    return conn;
}

static NettestConn *unclosed(void) {
    return new_conn();
}

static NettestConn *closed(void) {
    NettestConn *conn = new_conn();
    burrow__nettest_conn_close(conn);
    return conn;
}

static NettestConn *read_closed(void) {
    NettestConn *conn = new_conn();
    burrow__nettest_conn_close_read(conn);
    return conn;
}

static NettestConn *write_closed(void) {
    NettestConn *conn = new_conn();
    burrow__nettest_conn_close_write(conn);
    return conn;
}

static NettestConn *read_write_closed(void) {
    NettestConn *conn = new_conn();
    burrow__nettest_conn_close_read(conn);
    burrow__nettest_conn_close_write(conn);
    return conn;
}

static void is_closed_body(TestingT *t, void *env) {
    const IsClosedCase *test = env;
    NettestConn *conn = test->f();
    bool got = burrow__nettest_conn_is_closed(conn), want = test->want;
    if (got != want)
        testing_t_fatalf_v(t, "conn.IsClosed() = %v, want %v", got, want);
    got = burrow__nettest_conn_is_closed(burrow__nettest_conn_peer(conn));
    want = false;
    if (got != want)
        testing_t_fatalf_v(t, "conn.Peer().IsClosed() = %v, want %v", got, want);
    free_pair(conn); /* Not in Go. */
}

static void TestConnIsClosed(TestingT *t) {
    static const IsClosedCase tests[] = {
        {"unclosed", unclosed, false},
        {"closed", closed, true},
        {"read-closed", read_closed, false},
        {"write-closed", write_closed, false},
        {"read-write-closed", read_write_closed, true},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++)
        synctest_subtest(t, tests[i].name, is_closed_body,
                         (void *)(uintptr_t)&tests[i]);
}

/* anyError is passed to isOpError to match any error */
BURROW_SENTINEL_ERROR(any_error, "any");

static bool is_op_error(Error err, Error want) {
    if (err.vt == NULL || err.vt->self_type != TYPE_NET_OP_ERROR)
        return false;
    const NetOpError *oe = errors_as(err, TYPE_NET_OP_ERROR);
    return oe != NULL && (same(oe->err, want) || same(want, any_error));
}

static void want_conn_read_bytes(TestingT *t, NettestConn *c, const char *s) {
    testing_t_helper(t);
    Slice want = lit(s);
    Byte got[16];
    Slice g = slice_from(got, want.len, want.len, TYPE_BYTE);
    Error err;
    Int n = io_read_full(conn_reader(c), g, &err);
    if (n < want.len || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "io.ReadFull = %v, %v; want %v, nil", n, err, want.len);

    if (!bytes_equal(g, want))
        testing_t_fatalf_v(t, "io.ReadFull read %q, want %q", text(g), text(want));
}

static void want_conn_read_err(TestingT *t, NettestConn *c, Error want) {
    testing_t_helper(t);
    Byte b[1];
    Error err;
    Int n = conn_read(c, slice_from(b, 1, 1, TYPE_BYTE), &err);
    if (same(want, io_eof)) {
        if (n != 0 || !same(err, io_eof))
            testing_t_fatalf_v(t, "c.Read() = %v, %v; want 0, io.EOF", n, err);
    } else {
        if (n != 0 || !is_op_error(err, want))
            testing_t_fatalf_v(t, "c.Read() = %v, %v; want 0, OpError{Err: %q}", n, err,
                               error_text(want));
    }
}

typedef struct ReadBlocked {
    TestingT *t;
    NettestConn *c;
    uint32_t done;
} ReadBlocked;

static void read_blocked_job(void *env) {
    ReadBlocked *r = env;
    Byte b[1];
    Error err;
    Int n = conn_read(r->c, slice_from(b, 1, 1, TYPE_BYTE), &err);
    if (n != 0 || !errors_is(err, os_err_deadline_exceeded))
        testing_t_errorf_v(r->t, "c.Read() = %v, %v; want 0, ErrDeadlineExceeded", n,
                           err);
    done_store(&r->done);
}

static void want_conn_read_blocked(TestingT *t, NettestConn *c) {
    ReadBlocked r = {t, c, 0};
    go(BURROW_FN(Func, read_blocked_job, &r));
    synctest_wait();
    if (done_load(&r.done))
        testing_t_fatalf_v(t, "Read unexpectedly returned before setting deadline");
    burrow__nettest_conn_set_read_deadline(c, time_add(time_now(), -1 * TIME_SECOND));
    synctest_wait();
    Time zero = {0};
    burrow__nettest_conn_set_read_deadline(c, zero);
    if (!done_load(&r.done))
        testing_t_fatalf_v(t,
                           "Read unexpectedly did not return after setting deadline");
}

static void test_conn_set_read_error(void *env, TestingT *t) {
    (void)env;
    Error want_err = errors_new(error_allocator(), S("error"));
    NettestConn *rconn, *wconn;
    burrow__nettest_new_conn_pair(heap(), &rconn, &wconn);
    burrow__nettest_conn_set_read_error(rconn, want_err);
    Error err;

    /* Consume buffer before returning error. */
    (void)conn_write(wconn, lit("one"), &err);
    want_conn_read_bytes(t, rconn, "one");
    want_conn_read_err(t, rconn, want_err);

    /* Write more to the buffer, suppressing error until buffer drains again. */
    (void)conn_write(wconn, lit("two"), &err);
    want_conn_read_bytes(t, rconn, "two");
    want_conn_read_err(t, rconn, want_err);

    /* Error may be cleared. */
    burrow__nettest_conn_set_read_error(rconn, BURROW_NO_ERROR);
    want_conn_read_blocked(t, rconn);

    /* Close overrides read error. */
    burrow__nettest_conn_set_read_error(rconn, want_err);
    (void)conn_write(wconn, lit("three"), &err);
    burrow__nettest_conn_close(wconn);
    want_conn_read_bytes(t, rconn, "three");
    want_conn_read_err(t, rconn, io_eof);

    /* Setting another read error does not override Close. */
    burrow__nettest_conn_set_read_error(rconn, BURROW_NO_ERROR);
    want_conn_read_err(t, rconn, io_eof);
    burrow__nettest_conn_set_read_error(rconn, want_err);
    want_conn_read_err(t, rconn, io_eof);

    /* ErrClosed takes precedence over read error. */
    burrow__nettest_conn_close(rconn);
    want_conn_read_err(t, rconn, net_err_closed);
    free_pair(rconn); /* Not in Go. */
}

static void TestConnSetReadError(TestingT *t) {
    synctest_top(t, test_conn_set_read_error);
}

static void want_conn_write_bytes(TestingT *t, NettestConn *c, const char *s) {
    testing_t_helper(t);
    Slice b = lit(s);
    Error err;
    Int n = conn_write(c, b, &err);
    if (n != b.len || BURROW_FAILED(err))
        testing_t_fatalf_v(t, "c.Write() = %v, %v; want %v, nil", n, err, b.len);
}

static void want_conn_write_err(TestingT *t, NettestConn *c, Error want) {
    testing_t_helper(t);
    Byte b[1] = {0};
    Error err;
    Int n = conn_write(c, slice_from(b, 1, 1, TYPE_BYTE), &err);
    if (n != 0 || !is_op_error(err, want))
        testing_t_fatalf_v(t, "c.Write() = %v, %v; want 0, OpError{Err: %q}", n, err,
                           error_text(want));
}

static void test_conn_set_write_error(void *env, TestingT *t) {
    (void)env;
    Error want_err = errors_new(error_allocator(), S("error"));
    NettestConn *rconn, *wconn;
    burrow__nettest_new_conn_pair(heap(), &rconn, &wconn);
    burrow__nettest_conn_set_write_error(wconn, want_err);

    /* Error blocks writes. */
    want_conn_write_err(t, wconn, want_err);
    want_conn_read_blocked(t, rconn);

    /* Error may be cleared. */
    burrow__nettest_conn_set_write_error(wconn, BURROW_NO_ERROR);
    want_conn_write_bytes(t, wconn, "one");

    /* Restoring error does not prevent reading buffered data. */
    burrow__nettest_conn_set_write_error(wconn, want_err);
    want_conn_write_err(t, wconn, want_err);
    want_conn_read_bytes(t, rconn, "one");

    /* Error does not interfere with closing the conn. */
    burrow__nettest_conn_close(wconn);
    want_conn_read_err(t, rconn, io_eof);
    free_pair(rconn); /* Not in Go. */
}

static void TestConnSetWriteError(TestingT *t) {
    synctest_top(t, test_conn_set_write_error);
}

static void test_conn_set_close_error(void *env, TestingT *t) {
    (void)env;
    Error want_err = errors_new(error_allocator(), S("error"));
    NettestConn *rconn, *wconn;
    burrow__nettest_new_conn_pair(heap(), &rconn, &wconn);

    burrow__nettest_conn_set_close_error(wconn, want_err);
    Error err;
    (void)conn_write(wconn, lit("one"), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "wconn.Write = %v, want success", err);
    err = burrow__nettest_conn_close(wconn);
    if (!is_op_error(err, want_err))
        testing_t_fatalf_v(t, "wconn.Close = %v, want OpError{Err: %v}", err, want_err);
    err = burrow__nettest_conn_close(wconn);
    if (!is_op_error(err, net_err_closed))
        testing_t_fatalf_v(t, "wconn.Close = %v, want OpError{Err: net.ErrClosed}",
                           err);
    want_conn_read_bytes(t, rconn, "one");
    want_conn_read_err(t, rconn, io_eof);
    free_pair(rconn); /* Not in Go. */
}

static void TestConnSetCloseError(TestingT *t) {
    synctest_top(t, test_conn_set_close_error);
}

static void test_conn_close_read_write_error(void *env, TestingT *t) {
    (void)env;
    NettestConn *conn = new_conn();
    burrow__nettest_conn_set_close_error(conn,
                                         errors_new(error_allocator(), S("error")));
    Error err = burrow__nettest_conn_close_read(conn);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "conn.CloseRead = %v, want nil", err);
    err = burrow__nettest_conn_close_write(conn);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "conn.CloseRead = %v, want nil", err);
    free_pair(conn); /* Not in Go. */
}

static void TestConnCloseReadWriteError(TestingT *t) {
    synctest_top(t, test_conn_close_read_write_error);
}

/* ------------------------------------------------------ listener_test.go */

enum { NCONNS = 3 };

typedef struct NewConnTest {
    TestingT *t;
    NettestListener *li;
    const char *want[NCONNS];
    Slice got[NCONNS];
    NettestConn *cli[NCONNS]; /* Not in Go: kept to be freed */
    NettestConn *srv[NCONNS];
} NewConnTest;

typedef struct NewConnJob {
    NewConnTest *test;
    int i;
} NewConnJob;

static void new_conn_job(void *env) {
    NewConnJob *j = env;
    NewConnTest *test = j->test;
    int i = j->i;
    NettestConn *conn = burrow__nettest_listener_new_conn(test->li);
    test->cli[i] = conn;
    Slice b = lit(test->want[i]);
    Error err;
    Int n = conn_write(conn, b, &err);
    if (n != b.len || BURROW_FAILED(err))
        testing_t_errorf_v(test->t, "conn%v.Write() = %v, %v; want %v, nil", i, n, err,
                           b.len);
    burrow__nettest_conn_close(conn);
}

static void accept_job(void *env) {
    NewConnJob *j = env;
    NewConnTest *test = j->test;
    int i = j->i;
    Error err;
    NettestConn *conn = burrow__nettest_listener_accept(test->li, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(test->t, "li.Accept() = %v", err);
    test->srv[i] = conn;
    Slice b = io_read_all(heap(), conn_reader(conn), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(test->t, "io.ReadAll(conn%v) = %v", i, err);
    test->got[i] = b;
}

static void sort_strs(Str *s, int n) {
    for (int i = 1; i < n; i++)
        for (int k = i; k > 0 && str_cmp(s[k - 1], s[k]) > 0; k--) {
            Str x = s[k];
            s[k] = s[k - 1];
            s[k - 1] = x;
        }
}

static void test_listener_new_conn(void *env, TestingT *t) {
    (void)env;
    NewConnTest test = {
        t, burrow__nettest_new_listener(heap()), {"a", "b", "c"}, {{0}}, {0}, {0}};

    /* Create several connections in parallel. */
    NewConnJob jobs[NCONNS];
    for (int i = 0; i < NCONNS; i++) {
        jobs[i] = (NewConnJob){&test, i};
        go(BURROW_FN(Func, new_conn_job, &jobs[i]));
    }

    /* Accept the connections in parallel as well. */
    for (int i = 0; i < NCONNS; i++)
        go(BURROW_FN(Func, accept_job, &jobs[i]));

    synctest_wait();
    Str got[NCONNS], want[NCONNS];
    for (int i = 0; i < NCONNS; i++) {
        got[i] = text(test.got[i]);
        want[i] = str_from_cstr(test.want[i]);
    }
    sort_strs(got, NCONNS);
    sort_strs(want, NCONNS);
    for (int i = 0; i < NCONNS; i++) {
        if (!str_eq(got[i], want[i])) {
            testing_t_errorf_v(t, "connections read [%s %s %s]; want [%q %q %q]",
                               got[0], got[1], got[2], want[0], want[1], want[2]);
            break;
        }
    }
    for (int i = 0; i < NCONNS; i++) { /* Not in Go. */
        slice_free(test.got[i]);
        burrow__nettest_conn_free(test.cli[i]);
        burrow__nettest_conn_free(test.srv[i]);
    }
    burrow__nettest_listener_close(test.li);
    burrow__nettest_listener_free(test.li);
}

static void TestListenerNewConn(TestingT *t) {
    synctest_top(t, test_listener_new_conn);
}

/* Not in Go: the error Accept gave goes when the goroutine does, so the
 * goroutine checks it and keeps its text for the messages. */
typedef struct InterruptAccept {
    NettestListener *li;
    bool closed;          /* errors.Is(acceptErr, net.ErrClosed) */
    char accept_err[256]; /* the text of acceptErr */
    uint32_t returned;    /* says the two above have been set */
} InterruptAccept;

static void interrupt_accept_job(void *env) {
    InterruptAccept *a = env;
    Error err;
    NettestConn *c = burrow__nettest_listener_accept(a->li, &err);
    burrow__nettest_conn_free(c);
    a->closed = errors_is(err, net_err_closed);
    Str msg = BURROW_FAILED(err) ? error_text(err) : S("<nil>");
    snprintf(a->accept_err, sizeof a->accept_err, "%.*s", (int)msg.len, msg.p);
    done_store(&a->returned);
}

static void test_listener_interrupt_accept(void *env, TestingT *t) {
    (void)env;
    InterruptAccept a = {burrow__nettest_new_listener(heap()), false, "", 0};

    go(BURROW_FN(Func, interrupt_accept_job, &a));

    synctest_wait();
    if (done_load(&a.returned))
        testing_t_fatalf_v(t, "li.Accept() = %v, want still running before close",
                           str_from_cstr(a.accept_err));

    burrow__nettest_listener_close(a.li);
    synctest_wait();
    if (!done_load(&a.returned) || !a.closed)
        testing_t_fatalf_v(t, "li.Accept() = %v, want ErrClosed",
                           str_from_cstr(a.accept_err));
    burrow__nettest_listener_free(a.li); /* Not in Go. */
}

static void TestListenerInterruptAccept(TestingT *t) {
    synctest_top(t, test_listener_interrupt_accept);
}

static NetipAddrPort tcp_addr_port(NetAddr addr) {
    return net_tcp_addr_addr_port((const NetTCPAddr *)addr.data);
}

static void set_cli_addr(void *env, NettestConn *conn) {
    burrow__nettest_conn_set_local_addr(conn, net_tcp_addr_as_addr(env));
}

static void addr_mismatch(TestingT *t, const char *what, NetipAddrPort got,
                          NetipAddrPort want) {
    testing_t_errorf_v(t, "%s = %s, want %s", what, netip_addr_port_string(got, heap()),
                       netip_addr_port_string(want, heap()));
}

static void test_listener_addresses(void *env, TestingT *t) {
    (void)env;
    NetipAddrPort srvaddr = netip_must_parse_addr_port(S("10.0.0.1:80"));
    NetipAddrPort cliaddr = netip_must_parse_addr_port(S("10.0.0.2:1234"));
    NetTCPAddr *srvtcp = net_tcp_addr_from_addr_port(heap(), srvaddr);
    NetTCPAddr *clitcp = net_tcp_addr_from_addr_port(heap(), cliaddr);

    NettestListener *li = burrow__nettest_new_listener(heap());

    burrow__nettest_listener_set_addr(li, net_tcp_addr_as_addr(srvtcp));
    NetipAddrPort got = tcp_addr_port(burrow__nettest_listener_addr(li));
    if (!netip_addr_port_eq(got, srvaddr))
        addr_mismatch(t, "li.Addr()", got, srvaddr);

    NettestConn *cli = burrow__nettest_listener_new_conn_config(
        li, BURROW_FN(NettestConnFunc, set_cli_addr, clitcp));
    Error err;
    NetConn srvc =
        burrow__nettest_conn_as_net_conn(burrow__nettest_listener_accept(li, &err));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "li.Accept() = %v", err);
    NettestConn *srv = burrow__nettest_conn_from_net_conn(srvc);

    if (burrow__nettest_conn_peer(cli) != srv)
        testing_t_errorf_v(t, "cli.Peer() != srv; should be the same");
    if (burrow__nettest_conn_peer(srv) != cli)
        testing_t_errorf_v(t, "cli.Peer() != srv; should be the same");

    got = tcp_addr_port(burrow__nettest_conn_local_addr(cli));
    if (!netip_addr_port_eq(got, cliaddr))
        addr_mismatch(t, "cli.LocalAddr()", got, cliaddr);
    got = tcp_addr_port(burrow__nettest_conn_remote_addr(cli));
    if (!netip_addr_port_eq(got, srvaddr))
        addr_mismatch(t, "cli.LocalAddr()", got, srvaddr);
    got = tcp_addr_port(burrow__nettest_conn_local_addr(srv));
    if (!netip_addr_port_eq(got, srvaddr))
        addr_mismatch(t, "srv.LocalAddr()", got, srvaddr);
    got = tcp_addr_port(burrow__nettest_conn_remote_addr(srv));
    if (!netip_addr_port_eq(got, cliaddr))
        addr_mismatch(t, "cli.LocalAddr()", got, cliaddr);

    free_pair(cli); /* Not in Go. */
    burrow__nettest_listener_close(li);
    burrow__nettest_listener_free(li);
    net_tcp_addr_free(heap(), srvtcp);
    net_tcp_addr_free(heap(), clitcp);
}

static void TestListenerAddresses(TestingT *t) {
    synctest_top(t, test_listener_addresses);
}

static void want_listener_accept(TestingT *t, NettestListener *li, NettestConn *want) {
    testing_t_helper(t);
    Error err;
    NettestConn *got = burrow__nettest_listener_accept(li, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "li.Accept() = %v, want conn", err);
    if (got != want)
        testing_t_fatalf_v(t, "li.Accept() returned unexpected conn");
}

static void want_listener_accept_err(TestingT *t, NettestListener *li, Error want) {
    testing_t_helper(t);
    Error err;
    NettestConn *got = burrow__nettest_listener_accept(li, &err);
    if (got != NULL || !is_op_error(err, want))
        testing_t_fatalf_v(t, "li.Accept() = %p, %v; want nil, OpError{Err: %q}",
                           (void *)got, err, error_text(want));
}

typedef struct AcceptBlocked {
    TestingT *t;
    NettestListener *li;
    Error cancel_err;
    uint32_t done;
} AcceptBlocked;

static void accept_blocked_job(void *env) {
    AcceptBlocked *a = env;
    Error err;
    NettestConn *got = burrow__nettest_listener_accept(a->li, &err);
    if (got != NULL || !errors_is(err, a->cancel_err))
        testing_t_errorf_v(a->t, "li.Accept = %p, %v; want nil, cancelErr", (void *)got,
                           err);
    done_store(&a->done);
}

static void want_listener_accept_blocked(TestingT *t, NettestListener *li) {
    AcceptBlocked a = {t, li, errors_new(error_allocator(), S("cancel")), 0};
    go(BURROW_FN(Func, accept_blocked_job, &a));
    synctest_wait();
    if (done_load(&a.done))
        testing_t_fatalf_v(t, "Accept unexpectedly returned before canceling");
    burrow__nettest_listener_set_accept_error(li, a.cancel_err);
    synctest_wait();
    burrow__nettest_listener_set_accept_error(li, BURROW_NO_ERROR);
    if (!done_load(&a.done))
        testing_t_fatalf_v(t, "Accept unexpectedly did not return after canceling");
}

static void test_listener_set_accept_error(void *env, TestingT *t) {
    (void)env;
    Error accept_err = errors_new(error_allocator(), S("accept error"));
    NettestListener *li = burrow__nettest_new_listener(heap());
    burrow__nettest_listener_set_accept_error(li, accept_err);

    /* Accept conns from queue before returning error. */
    NettestConn *c1 = burrow__nettest_listener_new_conn(li);
    want_listener_accept(t, li, burrow__nettest_conn_peer(c1));
    want_listener_accept_err(t, li, accept_err);

    /* Add a new conn, suppressing error until the queue is empty. */
    NettestConn *c2 = burrow__nettest_listener_new_conn(li);
    want_listener_accept(t, li, burrow__nettest_conn_peer(c2));
    want_listener_accept_err(t, li, accept_err);

    /* Error may be cleared. */
    burrow__nettest_listener_set_accept_error(li, BURROW_NO_ERROR);
    want_listener_accept_blocked(t, li);

    /* ErrClosed takes precedence over accept error. */
    burrow__nettest_listener_set_accept_error(li, accept_err);
    burrow__nettest_listener_close(li);
    want_listener_accept_err(t, li, net_err_closed);
    free_pair(c1); /* Not in Go. */
    free_pair(c2);
    burrow__nettest_listener_free(li);
}

static void TestListenerSetAcceptError(TestingT *t) {
    synctest_top(t, test_listener_set_accept_error);
}

static void test_listener_set_close_error(void *env, TestingT *t) {
    (void)env;
    NettestListener *li = burrow__nettest_new_listener(heap());
    Error close_err = errors_new(error_allocator(), S("close error"));
    burrow__nettest_listener_set_close_error(li, close_err);

    /* First close uses the user-provided error. */
    Error err = burrow__nettest_listener_close(li);
    if (!is_op_error(err, close_err))
        testing_t_fatalf_v(t, "li.Close() = %v; want OpError wrapping accept error",
                           err);

    /* Repeated closes return ErrClosed. */
    err = burrow__nettest_listener_close(li);
    if (!is_op_error(err, net_err_closed))
        testing_t_fatalf_v(t, "li.Close() = %v; want OpError wrapping net.ErrClosed",
                           err);
    burrow__nettest_listener_free(li); /* Not in Go. */
}

static void TestListenerSetCloseError(TestingT *t) {
    synctest_top(t, test_listener_set_close_error);
}

#define TESTS(X)                                                                       \
    X(TestConnReadWrite)                                                               \
    X(TestConnZeroBuffer)                                                              \
    X(TestConnPartialWrite)                                                            \
    X(TestConnReadDeadline)                                                            \
    X(TestConnWriteDeadline)                                                           \
    X(TestConnCanRead)                                                                 \
    X(TestConnIsClosed)                                                                \
    X(TestConnSetReadError)                                                            \
    X(TestConnSetWriteError)                                                           \
    X(TestConnSetCloseError)                                                           \
    X(TestConnCloseReadWriteError)                                                     \
    X(TestListenerNewConn)                                                             \
    X(TestListenerInterruptAccept)                                                     \
    X(TestListenerAddresses)                                                           \
    X(TestListenerSetAcceptError)                                                      \
    X(TestListenerSetCloseError)

TESTING_MAIN(TESTS)
