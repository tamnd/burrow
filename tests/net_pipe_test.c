/* Derived from Go's src/net/pipe_test.go and the conformance tests it runs,
 * src/vendor/golang.org/x/net/nettest/conntest.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/mem/heap.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include "check.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S

#define CHECK_TEXT(got, want)                                                          \
    do {                                                                               \
        Str g_ = (got), w_ = str_from_cstr(want);                                      \
        if (!str_eq(g_, w_))                                                           \
            testing_t_errorf_v(t, "got %q, want %q", g_, w_);                          \
    } while (0)

static Alloc *heap(void) {
    return heap_allocator();
}

static bool same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

static Int conn_read(NetConn c, Slice b, Error *err) {
    return c.vt->reader.read(c.data, b, err);
}

static Int conn_write(NetConn c, Slice b, Error *err) {
    return c.vt->writer.write(c.data, b, err);
}

static Error conn_close(NetConn c) {
    return c.vt->closer.close(c.data);
}

/* A zeroed buffer of n bytes from the heap, which the test fails on and
 * returns an empty slice from if there is no room. */
static Slice buf_new(TestingT *t, Int n) {
    Byte *p = (Byte *)mem_alloc(heap(), (size_t)n, 1);
    if (p == NULL) {
        testing_t_errorf_v(t, "out of memory for %d bytes", n);
        return slice_from(NULL, 0, 0, TYPE_BYTE);
    }
    memset(p, 0, (size_t)n);
    return slice_from(p, n, n, TYPE_BYTE);
}

static void buf_free(Slice b) {
    if (b.p != NULL)
        mem_free(heap(), b.p, (size_t)b.cap, 1);
}

/* ------------------------------------------------------------ the readers */

/* An endless stream of bytes from xorshift64, standing in for Go's
 * rand.New(rand.NewSource(0)). The tests only need bytes that are not all the
 * same, not Go's exact stream. */
typedef struct RandReader {
    uint64_t s;
} RandReader;

static Byte rand_byte(RandReader *r) {
    r->s ^= r->s << 13;
    r->s ^= r->s >> 7;
    r->s ^= r->s << 17;
    return (Byte)(r->s >> 32);
}

static Int rand_read(void *self, Slice p, Error *err) {
    RandReader *r = (RandReader *)self;
    for (Int i = 0; i < p.len; i++)
        ((Byte *)p.p)[i] = rand_byte(r);
    BURROW_OUT(err, BURROW_NO_ERROR);
    return p.len;
}

static const Type rand_reader_desc = {
    BURROW_S_INIT("randReader"),
    BURROW_S_INIT("nettest"),
    KIND_STRUCT,
    (uint32_t)sizeof(RandReader),
    (uint16_t)_Alignof(RandReader),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e747272U, /* "ntrr" */
    NULL,
};

static const IoReaderVT rand_reader_vt = {&rand_reader_desc, rand_read};

static IoReader rand_reader(RandReader *r) {
    r->s = 0x9e3779b97f4a7c15ULL;
    IoReader ir = {&rand_reader_vt, r};
    return ir;
}

/* chunkedCopy: copies from r to w in fixed-width chunks, the way io.CopyBuffer
 * does with neither side's shortcuts. */
static Error chunked_copy(IoWriter w, IoReader r) {
    Byte b[1024];
    for (;;) {
        Error rerr = BURROW_NO_ERROR;
        Int nr = r.vt->read(r.data, slice_from(b, 1024, 1024, TYPE_BYTE), &rerr);
        if (nr > 0) {
            Error werr = BURROW_NO_ERROR;
            Int nw = w.vt->write(w.data, slice_from(b, nr, nr, TYPE_BYTE), &werr);
            if (BURROW_FAILED(werr))
                return werr;
            if (nw != nr)
                return io_err_short_write;
        }
        if (BURROW_FAILED(rerr))
            return same(rerr, io_eof) ? BURROW_NO_ERROR : rerr;
    }
}

/* ------------------------------------------------------------- the harness */

typedef struct Pair Pair;

/* One copy running in the background, as the go chunkedCopy(...) lines start
 * and never wait for. */
typedef struct CopyJob {
    IoWriter w;
    IoReader r;
    RandReader rand;
} CopyJob;

struct Pair {
    TestingT *t;
    NetConn c1, c2;
    SyncWaitGroup bg; /* the copies, waited for once both ends are closed */
    CopyJob jobs[2];
    Int njobs;
    Chan *dog_done;
};

static void copy_job(void *env) {
    CopyJob *j = (CopyJob *)env;
    (void)chunked_copy(j->w, j->r);
}

static void go_copy(Pair *p, IoWriter w, IoReader r) {
    CopyJob *j = &p->jobs[p->njobs++];
    j->w = w;
    j->r = r;
    sync_wait_group_go(&p->bg, BURROW_FN(Func, copy_job, j));
}

static void go_copy_rand(Pair *p, IoWriter w) {
    CopyJob *j = &p->jobs[p->njobs];
    go_copy(p, w, rand_reader(&j->rand));
}

static IoReader rd(NetConn c) {
    return net_conn_as_io_reader(c);
}

static IoWriter wr(NetConn c) {
    return net_conn_as_io_writer(c);
}

/* The stop function TestPipe hands nettest. */
static void stop(Pair *p) {
    (void)conn_close(p->c1);
    (void)conn_close(p->c2);
}

static void watchdog(void *env) {
    Pair *p = (Pair *)env;
    testing_t_errorf_v(p->t, "test timed out; terminating pipe");
    stop(p);
    chan_close(p->dog_done);
}

typedef void (*ConnTester)(TestingT *t, Pair *p);

typedef struct ConnTest {
    ConnTester f;
} ConnTest;

/* timeoutWrapper, with a wait for the background copies and the free that Go
 * leaves to the collector. */
static void timeout_wrapper(void *env, TestingT *t) {
    const ConnTest *ct = (const ConnTest *)env;
    Pair p = {0};
    p.t = t;
    net_pipe(heap(), &p.c1, &p.c2);
    p.dog_done = chan_make(heap(), TYPE_BOOL, 0);
    if (p.c1.vt == NULL || p.dog_done == NULL) {
        testing_t_errorf_v(t, "unable to make pipe");
        net_pipe_free(p.c1);
        if (p.dog_done != NULL)
            chan_free(p.dog_done);
        return;
    }
    TimeTimer *timer =
        time_after_func(heap(), TIME_MINUTE, BURROW_FN(Func, watchdog, &p));
    ct->f(t, &p);
    if (timer != NULL) {
        if (!time_timer_stop(timer))
            chan_recv(p.dog_done, NULL);
        time_timer_free(timer);
    }
    stop(&p);
    sync_wait_group_wait(&p.bg);
    net_pipe_free(p.c1);
    chan_free(p.dog_done);
}

/* aLongTimeAgo and neverTimeout from nettest.go. */
static Time a_long_time_ago(void) {
    return time_from_unix(233431200, 0);
}

static const Time never_timeout = {0};

/* checkForTimeoutError: err has to be a net.Error whose Timeout says true. */
static void check_for_timeout_error(TestingT *t, Error err) {
    if (!net_is_error(err))
        testing_t_errorf_v(t, "got %v, want net.Error", err);
    else if (!net_error_timeout(err))
        testing_t_errorf_v(t, "got error: %v, want err.Timeout() = true", err);
}

/* testRoundtrip writes something into c and reads it back. It assumes that
 * everything written into c is echoed back to itself. */
static void test_roundtrip(TestingT *t, NetConn c) {
    Error err = c.vt->set_deadline(c.data, never_timeout);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "roundtrip SetDeadline error: %v", err);

    Byte buf[13];
    memcpy(buf, "Hello, world!", 13);
    Slice b = slice_from(buf, 13, 13, TYPE_BYTE);
    (void)conn_write(c, b, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "roundtrip Write error: %v", err);
    (void)io_read_full(rd(c), b, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "roundtrip Read error: %v", err);
    if (memcmp(buf, "Hello, world!", 13) != 0)
        testing_t_errorf_v(t, "roundtrip data mismatch: got %q, want %q",
                           str_from_bytes(buf, 13), S("Hello, world!"));
}

typedef struct ResyncJob {
    NetConn c;
    Error err;
} ResyncJob;

static void resync_write(void *env) {
    ResyncJob *j = (ResyncJob *)env;
    Byte ff = 0xff;
    (void)conn_write(j->c, slice_from(&ff, 1, 1, TYPE_BYTE), &j->err);
}

/* resyncConn resynchronizes the connection into a sane state. It assumes that
 * everything written into c is echoed back to itself, and that 0xff is not
 * currently on the wire or in the read buffer. */
static void resync_conn(TestingT *t, NetConn c) {
    (void)c.vt->set_deadline(c.data, never_timeout);
    ResyncJob j = {c, BURROW_NO_ERROR};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, resync_write, &j));
    Byte buf[1024];
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Int n = conn_read(c, slice_from(buf, 1024, 1024, TYPE_BYTE), &err);
        if (n > 0) {
            Byte *ff = memchr(buf, 0xff, (size_t)n);
            if (ff != NULL && ff - buf == n - 1)
                break;
        }
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "unexpected Read error: %v", err);
            break;
        }
    }
    sync_wait_group_wait(&wg);
    if (BURROW_FAILED(j.err))
        testing_t_errorf_v(t, "unexpected Write error: %v", j.err);
}

/* ---------------------------------------------------------------- BasicIO */

typedef struct BasicJob {
    TestingT *t;
    NetConn c;
    Slice want;
    BytesBuffer got;
} BasicJob;

static void basic_send(void *env) {
    BasicJob *j = (BasicJob *)env;
    BytesReader r;
    bytes_reader_reset(&r, j->want);
    Error err = chunked_copy(wr(j->c), bytes_reader_as_io_reader(&r));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(j->t, "unexpected c1.Write error: %v", err);
    err = conn_close(j->c);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(j->t, "unexpected c1.Close error: %v", err);
}

static void basic_receive(void *env) {
    BasicJob *j = (BasicJob *)env;
    Error err = chunked_copy(bytes_buffer_as_io_writer(&j->got), rd(j->c));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(j->t, "unexpected c2.Read error: %v", err);
    err = conn_close(j->c);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(j->t, "unexpected c2.Close error: %v", err);
}

/* testBasicIO tests that the data sent on c1 is properly received on c2. */
static void test_basic_io(TestingT *t, Pair *p) {
    Slice want = buf_new(t, 1 << 20);
    if (want.p == NULL)
        return;
    RandReader rr;
    IoReader src = rand_reader(&rr);
    (void)src.vt->read(src.data, want, NULL);

    BasicJob send = {t, p->c1, want, BYTES_BUFFER(NULL)};
    BasicJob receive = {t, p->c2, want, BYTES_BUFFER(heap())};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, basic_send, &send));
    sync_wait_group_go(&wg, BURROW_FN(Func, basic_receive, &receive));
    sync_wait_group_wait(&wg);

    Slice got = bytes_buffer_bytes(&receive.got);
    if (got.len != want.len || memcmp(got.p, want.p, (size_t)want.len) != 0)
        testing_t_errorf_v(t, "transmitted data differs");
    bytes_buffer_free(&receive.got);
    buf_free(want);
}

/* --------------------------------------------------------------- PingPong */

typedef struct PingJob {
    TestingT *t;
    NetConn c;
} PingJob;

static uint64_t get_le64(const Byte *b) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = v << 8 | b[i];
    return v;
}

static void put_le64(Byte *b, uint64_t v) {
    for (int i = 0; i < 8; i++)
        b[i] = (Byte)(v >> (8 * i));
}

static void ping_ponger(void *env) {
    PingJob *j = (PingJob *)env;
    TestingT *t = j->t;
    Byte buf[8];
    Slice b = slice_from(buf, 8, 8, TYPE_BYTE);
    uint64_t prev = 0;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        (void)io_read_full(rd(j->c), b, &err);
        if (BURROW_FAILED(err)) {
            if (same(err, io_eof))
                break;
            testing_t_errorf_v(t, "unexpected Read error: %v", err);
        }

        uint64_t v = get_le64(buf);
        put_le64(buf, v + 1);
        if (prev != 0 && prev + 2 != v)
            testing_t_errorf_v(t, "mismatching value: got %d, want %d", (int64_t)v,
                               (int64_t)(prev + 2));
        prev = v;
        if (v == 1000)
            break;

        (void)conn_write(j->c, b, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "unexpected Write error: %v", err);
            break;
        }
    }
    Error err = conn_close(j->c);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected Close error: %v", err);
}

/* testPingPong tests that the two endpoints can synchronously send data to
 * each other in a typical request-response pattern. */
static void test_ping_pong(TestingT *t, Pair *p) {
    SyncWaitGroup wg = {0};
    PingJob j1 = {t, p->c1}, j2 = {t, p->c2};
    sync_wait_group_go(&wg, BURROW_FN(Func, ping_ponger, &j1));
    sync_wait_group_go(&wg, BURROW_FN(Func, ping_ponger, &j2));

    /* Start off the chain reaction. */
    Byte zero[8] = {0};
    Error err = BURROW_NO_ERROR;
    (void)conn_write(p->c1, slice_from(zero, 8, 8, TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected c1.Write error: %v", err);
    sync_wait_group_wait(&wg);
}

/* ------------------------------------------------------ RacyRead and Write */

typedef struct RacyJob {
    TestingT *t;
    NetConn c;
} RacyJob;

static void racy_reader(void *env) {
    RacyJob *j = (RacyJob *)env;
    Slice b1 = buf_new(j->t, 1024), b2 = buf_new(j->t, 1024);
    if (b1.p != NULL && b2.p != NULL) {
        for (int k = 0; k < 100; k++) {
            Error err = BURROW_NO_ERROR;
            (void)conn_read(j->c, b1, &err);
            memcpy(b1.p, b2.p, 1024); /* Mutate b1 to trigger potential race */
            if (BURROW_FAILED(err)) {
                check_for_timeout_error(j->t, err);
                (void)j->c.vt->set_read_deadline(
                    j->c.data, time_add(time_now(), TIME_MILLISECOND));
            }
        }
    }
    buf_free(b1);
    buf_free(b2);
}

/* testRacyRead tests that it is safe to mutate the input Read buffer
 * immediately after cancellation has occurred. */
static void test_racy_read(TestingT *t, Pair *p) {
    go_copy_rand(p, wr(p->c2));

    SyncWaitGroup wg = {0};
    (void)p->c1.vt->set_read_deadline(p->c1.data,
                                      time_add(time_now(), TIME_MILLISECOND));
    RacyJob j = {t, p->c1};
    for (int i = 0; i < 10; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, racy_reader, &j));
    sync_wait_group_wait(&wg);
}

static void racy_writer(void *env) {
    RacyJob *j = (RacyJob *)env;
    Slice b1 = buf_new(j->t, 1024), b2 = buf_new(j->t, 1024);
    if (b1.p != NULL && b2.p != NULL) {
        for (int k = 0; k < 100; k++) {
            Error err = BURROW_NO_ERROR;
            (void)conn_write(j->c, b1, &err);
            memcpy(b1.p, b2.p, 1024); /* Mutate b1 to trigger potential race */
            if (BURROW_FAILED(err)) {
                check_for_timeout_error(j->t, err);
                (void)j->c.vt->set_write_deadline(
                    j->c.data, time_add(time_now(), TIME_MILLISECOND));
            }
        }
    }
    buf_free(b1);
    buf_free(b2);
}

/* testRacyWrite tests that it is safe to mutate the input Write buffer
 * immediately after cancellation has occurred. */
static void test_racy_write(TestingT *t, Pair *p) {
    go_copy(p, io_discard, rd(p->c2));

    SyncWaitGroup wg = {0};
    (void)p->c1.vt->set_write_deadline(p->c1.data,
                                       time_add(time_now(), TIME_MILLISECOND));
    RacyJob j = {t, p->c1};
    for (int i = 0; i < 10; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, racy_writer, &j));
    sync_wait_group_wait(&wg);
}

/* ------------------------------------------------------ the fixed timeouts */

/* testReadTimeout tests that Read timeouts do not affect Write. */
static void test_read_timeout(TestingT *t, Pair *p) {
    go_copy(p, io_discard, rd(p->c2));

    Byte buf[1024] = {0};
    Slice b = slice_from(buf, 1024, 1024, TYPE_BYTE);
    (void)p->c1.vt->set_read_deadline(p->c1.data, a_long_time_ago());
    Error err = BURROW_NO_ERROR;
    (void)conn_read(p->c1, b, &err);
    check_for_timeout_error(t, err);
    (void)conn_write(p->c1, b, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected Write error: %v", err);
}

/* testWriteTimeout tests that Write timeouts do not affect Read. */
static void test_write_timeout(TestingT *t, Pair *p) {
    go_copy_rand(p, wr(p->c2));

    Byte buf[1024] = {0};
    Slice b = slice_from(buf, 1024, 1024, TYPE_BYTE);
    (void)p->c1.vt->set_write_deadline(p->c1.data, a_long_time_ago());
    Error err = BURROW_NO_ERROR;
    (void)conn_write(p->c1, b, &err);
    check_for_timeout_error(t, err);
    (void)conn_read(p->c1, b, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected Read error: %v", err);
}

/* testPastTimeout tests that a deadline set in the past immediately times out
 * Read and Write requests. */
static void test_past_timeout(TestingT *t, Pair *p) {
    go_copy(p, wr(p->c2), rd(p->c2));

    test_roundtrip(t, p->c1);

    Byte buf[1024] = {0};
    Slice b = slice_from(buf, 1024, 1024, TYPE_BYTE);
    (void)p->c1.vt->set_deadline(p->c1.data, a_long_time_ago());
    Error err = BURROW_NO_ERROR;
    Int n = conn_write(p->c1, b, &err);
    if (n != 0)
        testing_t_errorf_v(t, "unexpected Write count: got %d, want 0", n);
    check_for_timeout_error(t, err);
    n = conn_read(p->c1, b, &err);
    if (n != 0)
        testing_t_errorf_v(t, "unexpected Read count: got %d, want 0", n);
    check_for_timeout_error(t, err);

    test_roundtrip(t, p->c1);
}

/* --------------------------------------------------------- PresentTimeout */

typedef struct PresentJob {
    TestingT *t;
    NetConn c;
    Chan *deadline_set; /* bool, with room for one */
} PresentJob;

static void present_set(void *env) {
    PresentJob *j = (PresentJob *)env;
    time_sleep(100 * TIME_MILLISECOND);
    bool yes = true;
    chan_send(j->deadline_set, &yes);
    (void)j->c.vt->set_read_deadline(j->c.data, a_long_time_ago());
    (void)j->c.vt->set_write_deadline(j->c.data, a_long_time_ago());
}

static void present_read(void *env) {
    PresentJob *j = (PresentJob *)env;
    Byte buf[1024];
    Error err = BURROW_NO_ERROR;
    Int n = conn_read(j->c, slice_from(buf, 1024, 1024, TYPE_BYTE), &err);
    if (n != 0)
        testing_t_errorf_v(j->t, "unexpected Read count: got %d, want 0", n);
    check_for_timeout_error(j->t, err);
    if (chan_len(j->deadline_set) == 0)
        testing_t_errorf_v(j->t, "Read timed out before deadline is set");
}

static void present_write(void *env) {
    PresentJob *j = (PresentJob *)env;
    Byte buf[1024] = {0};
    Error err = BURROW_NO_ERROR;
    while (!BURROW_FAILED(err))
        (void)conn_write(j->c, slice_from(buf, 1024, 1024, TYPE_BYTE), &err);
    check_for_timeout_error(j->t, err);
    if (chan_len(j->deadline_set) == 0)
        testing_t_errorf_v(j->t, "Write timed out before deadline is set");
}

/* testPresentTimeout tests that a past deadline set while there are pending
 * Read and Write operations immediately times out those operations. */
static void test_present_timeout(TestingT *t, Pair *p) {
    PresentJob j = {t, p->c1, chan_make(heap(), TYPE_BOOL, 1)};
    if (j.deadline_set == NULL) {
        testing_t_errorf_v(t, "out of memory");
        return;
    }
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, present_set, &j));
    sync_wait_group_go(&wg, BURROW_FN(Func, present_read, &j));
    sync_wait_group_go(&wg, BURROW_FN(Func, present_write, &j));
    sync_wait_group_wait(&wg);
    chan_free(j.deadline_set);
}

/* ---------------------------------------------------------- FutureTimeout */

static void future_read(void *env) {
    RacyJob *j = (RacyJob *)env;
    Byte buf[1024];
    Error err = BURROW_NO_ERROR;
    (void)conn_read(j->c, slice_from(buf, 1024, 1024, TYPE_BYTE), &err);
    check_for_timeout_error(j->t, err);
}

static void future_write(void *env) {
    RacyJob *j = (RacyJob *)env;
    Byte buf[1024] = {0};
    Error err = BURROW_NO_ERROR;
    while (!BURROW_FAILED(err))
        (void)conn_write(j->c, slice_from(buf, 1024, 1024, TYPE_BYTE), &err);
    check_for_timeout_error(j->t, err);
}

/* testFutureTimeout tests that a future deadline will eventually time out
 * Read and Write operations. */
static void test_future_timeout(TestingT *t, Pair *p) {
    SyncWaitGroup wg = {0};
    RacyJob j = {t, p->c1};

    (void)p->c1.vt->set_deadline(p->c1.data,
                                 time_add(time_now(), 100 * TIME_MILLISECOND));
    sync_wait_group_go(&wg, BURROW_FN(Func, future_read, &j));
    sync_wait_group_go(&wg, BURROW_FN(Func, future_write, &j));
    sync_wait_group_wait(&wg);

    go_copy(p, wr(p->c2), rd(p->c2));
    resync_conn(t, p->c1);
    test_roundtrip(t, p->c1);
}

/* ----------------------------------------------------------- CloseTimeout */

static void close_later(void *env) {
    RacyJob *j = (RacyJob *)env;
    time_sleep(100 * TIME_MILLISECOND);
    (void)conn_close(j->c);
}

static void read_until_error(void *env) {
    RacyJob *j = (RacyJob *)env;
    Byte buf[1024];
    Error err = BURROW_NO_ERROR;
    while (!BURROW_FAILED(err))
        (void)conn_read(j->c, slice_from(buf, 1024, 1024, TYPE_BYTE), &err);
}

static void write_until_error(void *env) {
    RacyJob *j = (RacyJob *)env;
    Byte buf[1024] = {0};
    Error err = BURROW_NO_ERROR;
    while (!BURROW_FAILED(err))
        (void)conn_write(j->c, slice_from(buf, 1024, 1024, TYPE_BYTE), &err);
}

/* testCloseTimeout tests that calling Close immediately times out pending
 * Read and Write operations. */
static void test_close_timeout(TestingT *t, Pair *p) {
    go_copy(p, wr(p->c2), rd(p->c2));

    SyncWaitGroup wg = {0};
    RacyJob j = {t, p->c1};

    /* Test for cancellation upon connection closure. */
    (void)p->c1.vt->set_deadline(p->c1.data, never_timeout);
    sync_wait_group_go(&wg, BURROW_FN(Func, close_later, &j));
    sync_wait_group_go(&wg, BURROW_FN(Func, read_until_error, &j));
    sync_wait_group_go(&wg, BURROW_FN(Func, write_until_error, &j));
    sync_wait_group_wait(&wg);
}

/* ------------------------------------------------------ ConcurrentMethods */

static void cm_read(void *env) {
    NetConn *c = (NetConn *)env;
    Byte buf[1024];
    (void)conn_read(*c, slice_from(buf, 1024, 1024, TYPE_BYTE), NULL);
}

static void cm_write(void *env) {
    NetConn *c = (NetConn *)env;
    Byte buf[1024] = {0};
    (void)conn_write(*c, slice_from(buf, 1024, 1024, TYPE_BYTE), NULL);
}

static void cm_set_deadline(void *env) {
    NetConn *c = (NetConn *)env;
    (void)c->vt->set_deadline(c->data, time_add(time_now(), 10 * TIME_MILLISECOND));
}

static void cm_set_read_deadline(void *env) {
    NetConn *c = (NetConn *)env;
    (void)c->vt->set_read_deadline(c->data, a_long_time_ago());
}

static void cm_set_write_deadline(void *env) {
    NetConn *c = (NetConn *)env;
    (void)c->vt->set_write_deadline(c->data, a_long_time_ago());
}

static void cm_local_addr(void *env) {
    NetConn *c = (NetConn *)env;
    (void)c->vt->local_addr(c->data);
}

static void cm_remote_addr(void *env) {
    NetConn *c = (NetConn *)env;
    (void)c->vt->remote_addr(c->data);
}

/* testConcurrentMethods tests that the methods of net.Conn can safely be
 * called concurrently. */
static void test_concurrent_methods(TestingT *t, Pair *p) {
    go_copy(p, wr(p->c2), rd(p->c2));

    /* The results of the calls may be nonsensical, but this should not
     * trigger a race detector warning. */
    SyncWaitGroup wg = {0};
    NetConn c1 = p->c1;
    for (int i = 0; i < 100; i++) {
        sync_wait_group_go(&wg, BURROW_FN(Func, cm_read, &c1));
        sync_wait_group_go(&wg, BURROW_FN(Func, cm_write, &c1));
        sync_wait_group_go(&wg, BURROW_FN(Func, cm_set_deadline, &c1));
        sync_wait_group_go(&wg, BURROW_FN(Func, cm_set_read_deadline, &c1));
        sync_wait_group_go(&wg, BURROW_FN(Func, cm_set_write_deadline, &c1));
        sync_wait_group_go(&wg, BURROW_FN(Func, cm_local_addr, &c1));
        sync_wait_group_go(&wg, BURROW_FN(Func, cm_remote_addr, &c1));
    }
    sync_wait_group_wait(&wg); /* At worst, the deadline is set 10ms into the future */

    resync_conn(t, p->c1);
    test_roundtrip(t, p->c1);
}

/* ---------------------------------------------------------------- the tests */

static void TestPipe(TestingT *t) {
    struct {
        const char *name;
        ConnTest test;
    } tests[] = {
        {"BasicIO", {test_basic_io}},
        {"PingPong", {test_ping_pong}},
        {"RacyRead", {test_racy_read}},
        {"RacyWrite", {test_racy_write}},
        {"ReadTimeout", {test_read_timeout}},
        {"WriteTimeout", {test_write_timeout}},
        {"PastTimeout", {test_past_timeout}},
        {"PresentTimeout", {test_present_timeout}},
        {"FutureTimeout", {test_future_timeout}},
        {"CloseTimeout", {test_close_timeout}},
        {"ConcurrentMethods", {test_concurrent_methods}},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Str name = str_from_cstr(tests[i].name);
        testing_t_run(t, name,
                      BURROW_FN(TestingTFunc, timeout_wrapper, &tests[i].test));
    }
}

static void TestPipeCloseError(TestingT *t) {
    NetConn c1, c2;
    net_pipe(heap(), &c1, &c2);
    if (c1.vt == NULL) {
        testing_t_fatalf_v(t, "net_pipe: out of memory");
        return;
    }
    (void)conn_close(c1);

    Slice nil = slice_from(NULL, 0, 0, TYPE_BYTE);
    Error err = BURROW_NO_ERROR;
    (void)conn_read(c1, nil, &err);
    if (!same(err, io_err_closed_pipe))
        testing_t_errorf_v(t, "c1.Read() = %v, want io.ErrClosedPipe", err);
    (void)conn_write(c1, nil, &err);
    if (!same(err, io_err_closed_pipe))
        testing_t_errorf_v(t, "c1.Write() = %v, want io.ErrClosedPipe", err);
    err = c1.vt->set_deadline(c1.data, never_timeout);
    if (!same(err, io_err_closed_pipe))
        testing_t_errorf_v(t, "c1.SetDeadline() = %v, want io.ErrClosedPipe", err);
    (void)conn_read(c2, nil, &err);
    if (!same(err, io_eof))
        testing_t_errorf_v(t, "c2.Read() = %v, want io.EOF", err);
    (void)conn_write(c2, nil, &err);
    if (!same(err, io_err_closed_pipe))
        testing_t_errorf_v(t, "c2.Write() = %v, want io.ErrClosedPipe", err);
    err = c2.vt->set_deadline(c2.data, never_timeout);
    if (!same(err, io_err_closed_pipe))
        testing_t_errorf_v(t, "c2.SetDeadline() = %v, want io.ErrClosedPipe", err);
    net_pipe_free(c2);
}

/* What follows is burrow's own. */

/* The error a timed-out Read gives is the OpError Go's would be, with the
 * text, the wrapped error and the net.Error methods to match. */
static void TestPipeTimeoutError(TestingT *t) {
    NetConn c1, c2;
    net_pipe(heap(), &c1, &c2);
    if (c1.vt == NULL) {
        testing_t_fatalf_v(t, "net_pipe: out of memory");
        return;
    }
    (void)c1.vt->set_read_deadline(c1.data, a_long_time_ago());
    Byte buf[8];
    Error err = BURROW_NO_ERROR;
    (void)conn_read(c1, slice_from(buf, 8, 8, TYPE_BYTE), &err);
    CHECK(err.vt != NULL && err.vt->self_type == TYPE_NET_OP_ERROR);
    CHECK_TEXT(error_text(err), "read pipe: i/o timeout");
    CHECK(errors_is(err, os_err_deadline_exceeded));
    CHECK(net_is_error(err));
    CHECK(net_error_timeout(err));
    CHECK(!net_error_temporary(err));

    (void)c2.vt->set_write_deadline(c2.data, a_long_time_ago());
    (void)conn_write(c2, slice_from(buf, 8, 8, TYPE_BYTE), &err);
    CHECK_TEXT(error_text(err), "write pipe: i/o timeout");
    CHECK(net_error_timeout(err));

    NetAddr addr = c1.vt->local_addr(c1.data);
    CHECK_TEXT(addr.vt->network(addr.data), "pipe");
    CHECK_TEXT(addr.vt->string(addr.data, heap()), "pipe");
    net_pipe_free(c1);
}

/* OpError's text, put together the way Go's Error method does. */
static void TestOpErrorString(TestingT *t) {
    NetConn c1, c2;
    net_pipe(heap(), &c1, &c2);
    if (c1.vt == NULL) {
        testing_t_fatalf_v(t, "net_pipe: out of memory");
        return;
    }
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetAddr addr = c1.vt->local_addr(c1.data);
    NetAddr none = {NULL, NULL};
    static const struct {
        const char *op, *net;
        bool source, addr;
        const char *want;
    } tests[] = {
        {"read", "pipe", false, false, "read pipe: use of closed network connection"},
        {"dial", "pipe", false, true,
         "dial pipe pipe: use of closed network connection"},
        {"dial", "pipe", true, true,
         "dial pipe pipe->pipe: use of closed network connection"},
        {"accept", "", false, false, "accept: use of closed network connection"},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        NetOpError e = {str_from_cstr(tests[i].op), str_from_cstr(tests[i].net),
                        tests[i].source ? addr : none, tests[i].addr ? addr : none,
                        net_err_closed};
        CHECK_TEXT(net_op_error_error(&e, a), tests[i].want);

        Error err = net_op_error_as_error(&e, a);
        CHECK_TEXT(error_text(err), tests[i].want);
        CHECK(same(net_op_error_unwrap(&e), net_err_closed));
        CHECK(errors_is(err, net_err_closed));
        CHECK(!net_error_timeout(err));
    }
    CHECK(net_is_error(net_err_closed));
    CHECK(!net_error_timeout(net_err_closed));
    CHECK(!net_error_temporary(net_err_closed));
    CHECK(!net_is_error(io_eof));
    arena_free(&ar);
    net_pipe_free(c1);
}

/* A pipe freed with a deadline still to come stops its timer, so nothing
 * fires into freed memory. */
static void TestPipeFreeArmed(TestingT *t) {
    for (int i = 0; i < 20; i++) {
        NetConn c1, c2;
        net_pipe(heap(), &c1, &c2);
        if (c1.vt == NULL) {
            testing_t_fatalf_v(t, "net_pipe: out of memory");
            return;
        }
        Duration d = i % 2 == 0 ? TIME_HOUR : (Duration)i * TIME_MICROSECOND;
        CHECK(!BURROW_FAILED(c1.vt->set_deadline(c1.data, time_add(time_now(), d))));
        CHECK(
            !BURROW_FAILED(c2.vt->set_read_deadline(c2.data, time_add(time_now(), d))));
        net_pipe_free(i % 2 == 0 ? c1 : c2);
    }
    time_sleep(2 * TIME_MILLISECOND);
}

#define TESTS(X)                                                                       \
    X(TestPipe)                                                                        \
    X(TestPipeCloseError)                                                              \
    X(TestPipeTimeoutError)                                                            \
    X(TestOpErrorString)                                                               \
    X(TestPipeFreeArmed)

TESTING_MAIN(TESTS)
