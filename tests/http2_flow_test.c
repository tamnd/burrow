/* Derived from net/http/internal/http2's flow_test.go, databuffer_test.go and
 * pipe_test.go in Go 1.27.1.
 *
 * Go's dataBuffer tests compare the chunks with reflect.DeepEqual, and these
 * compare them as the strings Go prints them as when they differ, which is
 * the same test for chunks made of runs of one byte, as these all are.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http2.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include <stdint.h>
#include <string.h>

/* n KiB, as an Int. */
#define KB(n) ((Int)(n) * 1024)

#define FATALF(...)                                                                    \
    do {                                                                               \
        testing_t_fatalf_v(t, __VA_ARGS__);                                            \
        return;                                                                        \
    } while (0)

/* ------------------------------------------------------------------- flow */

static void TestInFlowTake(TestingT *t) {
    Http2Inflow f = {0, 0};
    burrow__http2_inflow_init(&f, 100);
    if (!burrow__http2_inflow_take(&f, 40))
        FATALF("f.take(40) from 100: got false, want true");
    if (!burrow__http2_inflow_take(&f, 40))
        FATALF("f.take(40) from 60: got false, want true");
    if (burrow__http2_inflow_take(&f, 40))
        FATALF("f.take(40) from 20: got true, want false");
    if (!burrow__http2_inflow_take(&f, 20))
        FATALF("f.take(20) from 20: got false, want true");
}

static void TestInflowAddSmall(TestingT *t) {
    Http2Inflow f = {0, 0};
    burrow__http2_inflow_init(&f, 0);
    /* Adding even a small amount when there is no flow causes an immediate
     * send. */
    int32_t got = burrow__http2_inflow_add(&f, 1);
    if (got != 1)
        FATALF("f.add(1) to 1 = %d, want %d", got, 1);
}

static void TestInflowAdd(TestingT *t) {
    Http2Inflow f = {0, 0};
    burrow__http2_inflow_init(&f, 10 * HTTP2_INFLOW_MIN_REFRESH);
    int32_t got = burrow__http2_inflow_add(&f, HTTP2_INFLOW_MIN_REFRESH - 1);
    if (got != 0)
        FATALF("f.add(minRefresh - 1) = %d, want %d", got, 0);
    got = burrow__http2_inflow_add(&f, 1);
    if (got != HTTP2_INFLOW_MIN_REFRESH)
        FATALF("f.add(minRefresh) = %d, want %d", got, HTTP2_INFLOW_MIN_REFRESH);
}

static void TestTakeInflows(TestingT *t) {
    Http2Inflow a = {0, 0};
    Http2Inflow b = {0, 0};
    burrow__http2_inflow_init(&a, 10);
    burrow__http2_inflow_init(&b, 20);
    if (!burrow__http2_take_inflows(&a, &b, 5))
        FATALF("takeInflows(a, b, 5) from 10, 20: got false, want true");
    if (burrow__http2_take_inflows(&a, &b, 6))
        FATALF("takeInflows(a, b, 6) from 5, 15: got true, want false");
    if (!burrow__http2_take_inflows(&a, &b, 5))
        FATALF("takeInflows(a, b, 5) from 5, 15: got false, want true");
}

static void TestOutFlow(TestingT *t) {
    Http2Outflow st = {0, NULL};
    Http2Outflow conn = {0, NULL};
    burrow__http2_outflow_add(&st, 3);
    burrow__http2_outflow_add(&conn, 2);

    int32_t got = burrow__http2_outflow_available(&st);
    if (got != 3)
        testing_t_errorf_v(t, "available = %d; want %d", got, 3);
    burrow__http2_outflow_set_conn_flow(&st, &conn);
    got = burrow__http2_outflow_available(&st);
    if (got != 2)
        testing_t_errorf_v(t, "after parent setup, available = %d; want %d", got, 2);

    burrow__http2_outflow_take(&st, 2);
    got = burrow__http2_outflow_available(&conn);
    if (got != 0)
        testing_t_errorf_v(t, "after taking 2, conn = %d; want %d", got, 0);
    got = burrow__http2_outflow_available(&st);
    if (got != 0)
        testing_t_errorf_v(t, "after taking 2, stream = %d; want %d", got, 0);
}

static void TestOutFlowAdd(TestingT *t) {
    Http2Outflow f = {0, NULL};
    if (!burrow__http2_outflow_add(&f, 1))
        FATALF("failed to add 1");
    if (!burrow__http2_outflow_add(&f, -1))
        FATALF("failed to add -1");
    int32_t got = burrow__http2_outflow_available(&f);
    if (got != 0)
        FATALF("size = %d; want %d", got, 0);
    if (!burrow__http2_outflow_add(&f, INT32_MAX))
        FATALF("failed to add 2^31-1");
    got = burrow__http2_outflow_available(&f);
    if (got != INT32_MAX)
        FATALF("size = %d; want %d", got, INT32_MAX);
    if (burrow__http2_outflow_add(&f, 1))
        FATALF("adding 1 to max shouldn't be allowed");
}

static void TestOutFlowAddOverflow(TestingT *t) {
    Http2Outflow f = {0, NULL};
    static const int32_t adds[] = {0, -1, 0, 1, 1, 0, -3};
    for (size_t i = 0; i < sizeof adds / sizeof adds[0]; i++)
        if (!burrow__http2_outflow_add(&f, adds[i]))
            FATALF("failed to add %d", adds[i]);
    int32_t got = burrow__http2_outflow_available(&f);
    if (got != -2)
        FATALF("size = %d; want %d", got, -2);
    if (!burrow__http2_outflow_add(&f, INT32_MAX))
        FATALF("failed to add 2^31-1");
    got = burrow__http2_outflow_available(&f);
    if (got != 1 + -3 + INT32_MAX)
        FATALF("size = %d; want %d", got, 1 + -3 + INT32_MAX);
}

/* ------------------------------------------------------------- dataBuffer */

/* A run of n copies of c, which is all the tests write. */
typedef struct FlRun {
    char c;
    Int n;
} FlRun;

static Slice fl_runs(Alloc *a, const FlRun *runs, size_t nruns) {
    Int total = 0;
    for (size_t i = 0; i < nruns; i++)
        total += runs[i].n;
    Byte *p = mem_alloc(a, (size_t)total + 1, 1);
    Int off = 0;
    for (size_t i = 0; i < nruns; i++) {
        memset(p + off, runs[i].c, (size_t)runs[i].n);
        off += runs[i].n;
    }
    return slice_from(p, total, total, TYPE_BYTE);
}

/* fmtDataChunk. */
static void fl_fmt_data_chunk(BytesBuffer *out, Slice chunk) {
    IoWriter w = bytes_buffer_as_io_writer(out);
    const Byte *p = chunk.p;
    Byte last = 0;
    Int count = 0;
    for (Int i = 0; i < chunk.len; i++) {
        Byte c = p[i];
        if (c != last) {
            if (count > 0) {
                fmt_fprintf_v(w, " x %d ", count);
                count = 0;
            }
            Byte one[1] = {c};
            bytes_buffer_write(out, slice_from(one, 1, 1, TYPE_BYTE), NULL);
            last = c;
        }
        count++;
    }
    if (count > 0)
        fmt_fprintf_v(w, " x %d", count);
}

/* fmtDataChunks. */
static Str fl_fmt_data_chunks(Alloc *a, const Slice *chunks, Int n) {
    BytesBuffer out = BYTES_BUFFER(a);
    for (Int i = 0; i < n; i++) {
        BytesBuffer one = BYTES_BUFFER(a);
        fl_fmt_data_chunk(&one, chunks[i]);
        fmt_fprintf_v(bytes_buffer_as_io_writer(&out), "{%q}",
                      bytes_buffer_string(&one, a));
    }
    return bytes_buffer_string(&out, a);
}

static Str fl_fmt_one(Alloc *a, Slice chunk) {
    BytesBuffer out = BYTES_BUFFER(a);
    fl_fmt_data_chunk(&out, chunk);
    return bytes_buffer_string(&out, a);
}

typedef bool (*FlSetup)(TestingT *t, Alloc *a, Http2DataBuffer *b);

typedef struct FlCase {
    Slice want;
    FlSetup setup;
    Int read_size;
} FlCase;

static void fl_read_case(void *env, TestingT *t) {
    const FlCase *c = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Http2DataBuffer b;
    memset(&b, 0, sizeof b);
    if (c->setup(t, a, &b)) {
        Slice buf = slice_from(mem_alloc(a, (size_t)c->read_size, 1), c->read_size,
                               c->read_size, TYPE_BYTE);
        BytesBuffer got = BYTES_BUFFER(a);
        bool ok = true;
        for (;;) {
            Error err = BURROW_NO_ERROR;
            Int n = burrow__http2_data_buffer_read(&b, buf, &err);
            bytes_buffer_write(&got, slice_sub(buf, 0, n), NULL);
            if (errors_is(err, burrow__http2_err_read_empty))
                break;
            if (BURROW_FAILED(err)) {
                testing_t_fatalf_v(t, "error after %d bytes: %v",
                                   bytes_buffer_len(&got), err);
                ok = false;
                break;
            }
        }
        Slice g = bytes_buffer_bytes(&got);
        if (ok && !bytes_equal(g, c->want))
            testing_t_errorf_v(t, "FinalRead=%q, want %q", fl_fmt_one(a, g),
                               fl_fmt_one(a, c->want));
    }
    burrow__http2_data_buffer_free(&b);
    arena_free(&ar);
}

/* testDataBuffer: runs setup, then reads what is left with reads of a few
 * sizes, for the corner cases in Read. */
static void fl_test_data_buffer(TestingT *t, Slice want, FlSetup setup) {
    static const Int sizes[] = {1, 2, KB(1), KB(32)};
    for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        FlCase c = {want, setup, sizes[i]};
        char name[32];
        snprintf(name, sizeof name, "ReadSize=%d", (int)sizes[i]);
        testing_t_run(t, str_from_cstr(name),
                      BURROW_FN(TestingTFunc, fl_read_case, &c));
    }
}

static bool fl_write_runs(TestingT *t, Alloc *a, Http2DataBuffer *b, const FlRun *runs,
                          size_t nruns) {
    for (size_t i = 0; i < nruns; i++) {
        Slice p = fl_runs(a, &runs[i], 1);
        Error err = BURROW_NO_ERROR;
        Int n = burrow__http2_data_buffer_write(b, p, &err);
        if (n != p.len || BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "Write(\"%c\" x %d)=%d,%v want %d,nil", runs[i].c,
                               p.len, n, err, p.len);
            return false;
        }
    }
    return true;
}

/* The chunks b holds against want, each chunk of which is given as runs. */
static void fl_check_chunks(TestingT *t, Alloc *a, const Http2DataBuffer *b,
                            const FlRun *const *want, const size_t *nwant, Int n) {
    Slice *ws = mem_alloc_array(a, (size_t)n, sizeof(Slice), _Alignof(Slice));
    for (Int i = 0; i < n; i++)
        ws[i] = fl_runs(a, want[i], nwant[i]);
    Str got = fl_fmt_data_chunks(a, b->chunks, b->nchunks);
    Str wants = fl_fmt_data_chunks(a, ws, n);
    if (!str_eq(got, wants))
        testing_t_errorf_v(t, "dataBuffer.chunks\ngot:  %s\nwant: %s", got, wants);
}

static const FlRun fl_alloc_writes[] = {
    {'a', KB(1) - 1},  {'a', 1}, {'b', KB(4) - 1}, {'b', 1}, {'c', KB(8) - 1}, {'c', 1},
    {'d', KB(16) - 1}, {'d', 1}, {'e', KB(32)},
};

static bool fl_alloc_setup(TestingT *t, Alloc *a, Http2DataBuffer *b) {
    if (!fl_write_runs(t, a, b, fl_alloc_writes,
                       sizeof fl_alloc_writes / sizeof fl_alloc_writes[0]))
        return false;
    static const FlRun c0[] = {{'a', KB(1)}};
    static const FlRun c1[] = {{'b', KB(4)}};
    static const FlRun c2[] = {{'c', KB(8)}};
    static const FlRun c3[] = {{'d', KB(16)}};
    static const FlRun c4[] = {{'e', KB(16)}};
    static const FlRun *const want[] = {c0, c1, c2, c3, c4, c4};
    static const size_t nwant[] = {1, 1, 1, 1, 1, 1};
    fl_check_chunks(t, a, b, want, nwant, 6);
    return true;
}

static void TestDataBufferAllocation(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Slice want = fl_runs(arena_allocator(&ar), fl_alloc_writes,
                         sizeof fl_alloc_writes / sizeof fl_alloc_writes[0]);
    fl_test_data_buffer(t, want, fl_alloc_setup);
    arena_free(&ar);
}

/* The a allocates 16KB, the c allocates 16KB more, and the e overflows the 32KB
 * expectation and allocates just 1KB. */
static const FlRun fl_expected_writes[] = {
    {'a', KB(1)}, {'b', KB(14)}, {'c', KB(15)}, {'d', KB(2)}, {'e', KB(1)},
};

static bool fl_expected_setup(TestingT *t, Alloc *a, Http2DataBuffer *b) {
    b->expected = KB(32);
    if (!fl_write_runs(t, a, b, fl_expected_writes,
                       sizeof fl_expected_writes / sizeof fl_expected_writes[0]))
        return false;
    static const FlRun c0[] = {{'a', KB(1)}, {'b', KB(14)}, {'c', KB(1)}};
    static const FlRun c1[] = {{'c', KB(14)}, {'d', KB(2)}};
    static const FlRun c2[] = {{'e', KB(1)}};
    static const FlRun *const want[] = {c0, c1, c2};
    static const size_t nwant[] = {3, 2, 1};
    fl_check_chunks(t, a, b, want, nwant, 3);
    return true;
}

static void TestDataBufferAllocationWithExpected(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Slice want = fl_runs(arena_allocator(&ar), fl_expected_writes,
                         sizeof fl_expected_writes / sizeof fl_expected_writes[0]);
    fl_test_data_buffer(t, want, fl_expected_setup);
    arena_free(&ar);
}

static Slice fl_bytes(const char *s) {
    return slice_from((void *)(uintptr_t)s, (Int)strlen(s), (Int)strlen(s), TYPE_BYTE);
}

static bool fl_partial_setup(TestingT *t, Alloc *a, Http2DataBuffer *b) {
    (void)a;
    Error err = BURROW_NO_ERROR;
    Int n = burrow__http2_data_buffer_write(b, fl_bytes("abcd"), &err);
    if (n != 4 || BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "Write(\"abcd\")=%d,%v want 4,nil", n, err);
        return false;
    }
    Byte buf[2] = {0, 0};
    Slice p = slice_from(buf, 2, 2, TYPE_BYTE);
    n = burrow__http2_data_buffer_read(b, p, &err);
    if (n != 2 || BURROW_FAILED(err) || !bytes_equal(p, fl_bytes("ab"))) {
        testing_t_fatalf_v(t, "Read()=%q,%d,%v want \"ab\",2,nil", p, n, err);
        return false;
    }
    n = burrow__http2_data_buffer_write(b, fl_bytes("xyz"), &err);
    if (n != 3 || BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "Write(\"xyz\")=%d,%v want 3,nil", n, err);
        return false;
    }
    return true;
}

static void TestDataBufferWriteAfterPartialRead(TestingT *t) {
    fl_test_data_buffer(t, fl_bytes("cdxyz"), fl_partial_setup);
}

/* ------------------------------------------------------------------- pipe */

/* A pipe over a bytes.Buffer, both freed at the end of the test. */
typedef struct FlPipe {
    Http2Pipe p;
    BytesBuffer b;
} FlPipe;

static void fl_pipe_init(FlPipe *fp, bool with_buffer) {
    memset(fp, 0, sizeof *fp);
    fp->b = BYTES_BUFFER(heap_allocator());
    if (with_buffer)
        fp->p.b = burrow__http2_bytes_buffer_as_pipe_buffer(&fp->b);
}

static void fl_pipe_free(FlPipe *fp) {
    burrow__http2_pipe_free(&fp->p);
    bytes_buffer_free(&fp->b);
}

static bool fl_closed(Chan *c) {
    bool ok = true;
    return chan_try_recv(c, NULL, &ok) && !ok;
}

static void TestPipeClose(TestingT *t) {
    FlPipe fp;
    fl_pipe_init(&fp, true);
    Error a = errors_new(error_allocator(), BURROW_S("a"));
    Error b = errors_new(error_allocator(), BURROW_S("b"));
    burrow__http2_pipe_close_with_error(&fp.p, a);
    burrow__http2_pipe_close_with_error(&fp.p, b);
    Byte one[1] = {0};
    Error err = BURROW_NO_ERROR;
    burrow__http2_pipe_read(&fp.p, slice_from(one, 1, 1, TYPE_BYTE), &err);
    if (!errors_is(err, a))
        testing_t_errorf_v(t, "err = %v want %v", err, a);
    fl_pipe_free(&fp);
}

static void TestPipeDoneChan(TestingT *t) {
    FlPipe fp;
    fl_pipe_init(&fp, false);
    Chan *done = burrow__http2_pipe_done(&fp.p);
    if (fl_closed(done)) {
        fl_pipe_free(&fp);
        FATALF("done too soon");
    }
    burrow__http2_pipe_close_with_error(&fp.p, io_eof);
    if (!fl_closed(done))
        testing_t_fatalf_v(t, "should be done");
    fl_pipe_free(&fp);
}

static void TestPipeDoneChan_ErrFirst(TestingT *t) {
    FlPipe fp;
    fl_pipe_init(&fp, false);
    burrow__http2_pipe_close_with_error(&fp.p, io_eof);
    Chan *done = burrow__http2_pipe_done(&fp.p);
    if (!fl_closed(done))
        testing_t_fatalf_v(t, "should be done");
    fl_pipe_free(&fp);
}

static void TestPipeDoneChan_Break(TestingT *t) {
    FlPipe fp;
    fl_pipe_init(&fp, false);
    Chan *done = burrow__http2_pipe_done(&fp.p);
    if (fl_closed(done)) {
        fl_pipe_free(&fp);
        FATALF("done too soon");
    }
    burrow__http2_pipe_break_with_error(&fp.p, io_eof);
    if (!fl_closed(done))
        testing_t_fatalf_v(t, "should be done");
    fl_pipe_free(&fp);
}

static void TestPipeDoneChan_Break_ErrFirst(TestingT *t) {
    FlPipe fp;
    fl_pipe_init(&fp, false);
    burrow__http2_pipe_break_with_error(&fp.p, io_eof);
    Chan *done = burrow__http2_pipe_done(&fp.p);
    if (!fl_closed(done))
        testing_t_fatalf_v(t, "should be done");
    fl_pipe_free(&fp);
}

static void TestPipeCloseWithError(TestingT *t) {
    FlPipe fp;
    fl_pipe_init(&fp, true);
    Http2Pipe *p = &fp.p;
    Alloc *heap = heap_allocator();
    io_write_string(burrow__http2_pipe_as_io_writer(p), BURROW_S("foo"), NULL);
    Error a = errors_new(error_allocator(), BURROW_S("test error"));
    burrow__http2_pipe_close_with_error(p, a);
    Error err = BURROW_NO_ERROR;
    Slice all = io_read_all(heap, burrow__http2_pipe_as_io_reader(p), &err);
    if (!bytes_equal(all, fl_bytes("foo")))
        testing_t_errorf_v(t, "read bytes = %q; want %q", all, BURROW_S("foo"));
    if (!errors_is(err, a))
        testing_t_logf_v(t, "read error = %v, %v", err, a);
    mem_free(heap, all.p, (size_t)all.cap, 1);
    if (burrow__http2_pipe_len(p) != 0)
        testing_t_errorf_v(t, "pipe should have 0 unread bytes");
    /* Read and Write should fail. */
    Int n = burrow__http2_pipe_write(p, fl_bytes("abc"), &err);
    if (!errors_is(err, burrow__http2_err_closed_pipe_write) || n != 0)
        testing_t_errorf_v(t, "Write(abc) after close\ngot %d, %v\nwant 0, %v", n, err,
                           burrow__http2_err_closed_pipe_write);
    Byte one[1] = {0};
    n = burrow__http2_pipe_read(p, slice_from(one, 1, 1, TYPE_BYTE), &err);
    if (BURROW_OK(err) || n != 0)
        testing_t_errorf_v(t, "Read() after close\ngot %d, nil\nwant 0, %v", n,
                           burrow__http2_err_closed_pipe_write);
    if (burrow__http2_pipe_len(p) != 0)
        testing_t_errorf_v(t, "pipe should have 0 unread bytes");
    fl_pipe_free(&fp);
}

static void TestPipeBreakWithError(TestingT *t) {
    FlPipe fp;
    fl_pipe_init(&fp, true);
    Http2Pipe *p = &fp.p;
    Alloc *heap = heap_allocator();
    io_write_string(burrow__http2_pipe_as_io_writer(p), BURROW_S("foo"), NULL);
    Error a = errors_new(error_allocator(), BURROW_S("test err"));
    burrow__http2_pipe_break_with_error(p, a);
    Error err = BURROW_NO_ERROR;
    Slice all = io_read_all(heap, burrow__http2_pipe_as_io_reader(p), &err);
    if (all.len != 0)
        testing_t_errorf_v(t, "read bytes = %q; want empty string", all);
    if (!errors_is(err, a))
        testing_t_logf_v(t, "read error = %v, %v", err, a);
    if (all.p != NULL)
        mem_free(heap, all.p, (size_t)all.cap, 1);
    if (p->b.vt != NULL)
        testing_t_errorf_v(t, "buffer should be nil after BreakWithError");
    if (burrow__http2_pipe_len(p) != 3)
        testing_t_errorf_v(t, "pipe should have 3 unread bytes");
    /* Write should fail. */
    Int n = burrow__http2_pipe_write(p, fl_bytes("abc"), &err);
    if (!errors_is(err, burrow__http2_err_closed_pipe_write) || n != 0)
        testing_t_errorf_v(
            t, "Write(abc) after break\ngot %d, %v\nwant 0, errClosedPipeWrite", n,
            err);
    if (p->b.vt != NULL)
        testing_t_errorf_v(t, "buffer should be nil after Write");
    if (burrow__http2_pipe_len(p) != 3)
        testing_t_errorf_v(t, "pipe should have 6 unread bytes");
    /* Read should fail. */
    Byte one[1] = {0};
    n = burrow__http2_pipe_read(p, slice_from(one, 1, 1, TYPE_BYTE), &err);
    if (BURROW_OK(err) || n != 0)
        testing_t_errorf_v(t, "Read() after close\ngot %d, nil\nwant 0, not nil", n);
    fl_pipe_free(&fp);
}

/* Not Go's: a Read blocked on an empty pipe is woken by a Write from another
 * goroutine, then by CloseWithError, and the code given to
 * closeWithErrorAndCode runs once, before the error. */
typedef struct FlWaiter {
    Http2Pipe *p;
    Byte buf[8];
    Int n;
    bool failed;
    bool eof;
} FlWaiter;

static void fl_wait_read(void *env) {
    FlWaiter *w = env;
    Error err = BURROW_NO_ERROR;
    w->n = burrow__http2_pipe_read(w->p, slice_from(w->buf, 8, 8, TYPE_BYTE), &err);
    w->failed = BURROW_FAILED(err);
    w->eof = errors_is(err, io_eof);
}

static void fl_count(void *env) {
    (*(int *)env)++;
}

static void TestPipeReadWaits(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    FlPipe fp;
    fl_pipe_init(&fp, true);
    FlWaiter w;
    memset(&w, 0, sizeof w);
    w.p = &fp.p;
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, fl_wait_read, &w));
    time_sleep(10 * TIME_MILLISECOND);
    burrow__http2_pipe_write(&fp.p, fl_bytes("hi"), NULL);
    sync_wait_group_wait(&wg);
    if (w.n != 2 || w.failed || memcmp(w.buf, "hi", 2) != 0)
        testing_t_errorf_v(t, "Read = %d, failed %t; want 2, nil", w.n, w.failed);

    int calls = 0;
    memset(&w, 0, sizeof w);
    w.p = &fp.p;
    sync_wait_group_go(&wg, BURROW_FN(Func, fl_wait_read, &w));
    time_sleep(10 * TIME_MILLISECOND);
    burrow__http2_pipe_close_with_error_and_code(&fp.p, io_eof,
                                                 BURROW_FN(Func, fl_count, &calls));
    sync_wait_group_wait(&wg);
    if (w.n != 0 || !w.eof)
        testing_t_errorf_v(t, "Read after close = %d, EOF %t; want 0, EOF", w.n, w.eof);
    Error err = BURROW_NO_ERROR;
    Byte one[1] = {0};
    burrow__http2_pipe_read(&fp.p, slice_from(one, 1, 1, TYPE_BYTE), &err);
    if (!errors_is(err, io_eof))
        testing_t_errorf_v(t, "second Read error = %v; want EOF", err);
    if (calls != 1)
        testing_t_errorf_v(t, "readFn ran %d times; want 1", calls);
    fl_pipe_free(&fp);
}

#define TESTS(X)                                                                       \
    X(TestInFlowTake)                                                                  \
    X(TestInflowAddSmall)                                                              \
    X(TestInflowAdd)                                                                   \
    X(TestTakeInflows)                                                                 \
    X(TestOutFlow)                                                                     \
    X(TestOutFlowAdd)                                                                  \
    X(TestOutFlowAddOverflow)                                                          \
    X(TestDataBufferAllocation)                                                        \
    X(TestDataBufferAllocationWithExpected)                                            \
    X(TestDataBufferWriteAfterPartialRead)                                             \
    X(TestPipeClose)                                                                   \
    X(TestPipeDoneChan)                                                                \
    X(TestPipeDoneChan_ErrFirst)                                                       \
    X(TestPipeDoneChan_Break)                                                          \
    X(TestPipeDoneChan_Break_ErrFirst)                                                 \
    X(TestPipeCloseWithError)                                                          \
    X(TestPipeBreakWithError)                                                          \
    X(TestPipeReadWaits)
TESTING_MAIN(TESTS)
