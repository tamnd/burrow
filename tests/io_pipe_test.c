/* Derived from Go's src/io/pipe_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/io.h"

#include "burrow/burrow.h"
#include "burrow/chan.h"
#include "burrow/mem/heap.h"
#include "burrow/sync.h"
#include "burrow/time.h"

#include "check.h"

#include <stdint.h>
#include <string.h>

#define S BURROW_S
#define LEN(a) ((Int)(sizeof(a) / sizeof((a)[0])))

static Alloc *heap(void) {
    return heap_allocator();
}

static void send_int(Chan *c, Int v) {
    chan_send(c, &v);
}

static Int recv_int(Chan *c) {
    Int v = 0;
    chan_recv(c, &v);
    return v;
}

/* Each goroutine gets its arguments in one of these, since a Func carries one
 * pointer. */
typedef struct Job {
    TestingT *t;
    IoPipeReader *r;
    IoPipeWriter *w;
    Slice data;
    Chan *c;
    Int n;
    Error err;
} Job;

/* Test a single read/write pair. */
static void check_write(void *env) {
    Job *j = (Job *)env;
    TestingT *t = j->t;
    Error err = BURROW_NO_ERROR;
    Int n = io_pipe_writer_write(j->w, j->data, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "write: %v", err);
    if (n != j->data.len)
        testing_t_errorf_v(t, "short write: %d != %d", n, j->data.len);
    send_int(j->c, 0);
}

static void TestPipe1(TestingT *t) {
    Chan *c = chan_make(heap(), TYPE_INT, 0);
    IoPipeReader *r;
    IoPipeWriter *w;
    io_pipe(heap(), &r, &w);
    Byte buf[64];
    SyncWaitGroup wg = {0};
    Job j = {t, r, w, BURROW_B("hello, world"), c, 0, BURROW_NO_ERROR};
    sync_wait_group_go(&wg, BURROW_FN(Func, check_write, &j));
    Error err = BURROW_NO_ERROR;
    Int n = io_pipe_reader_read(r, slice_from(buf, 64, 64, TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "read: %v", err);
    else if (n != 12 || memcmp(buf, "hello, world", 12) != 0)
        testing_t_errorf_v(t, "bad read: got %q", str_from_bytes(buf, n));
    recv_int(c);
    io_pipe_reader_close(r);
    io_pipe_writer_close(w);
    sync_wait_group_wait(&wg);
    io_pipe_free(r);
    chan_free(c);
}

/* Test a sequence of read/write pairs. */
static void reader(void *env) {
    Job *j = (Job *)env;
    TestingT *t = j->t;
    Byte buf[64];
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Int n = io_pipe_reader_read(j->r, slice_from(buf, 64, 64, TYPE_BYTE), &err);
        if (errors_is(err, io_eof)) {
            send_int(j->c, 0);
            break;
        }
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "read: %v", err);
        send_int(j->c, n);
    }
}

static void TestPipe2(TestingT *t) {
    Chan *c = chan_make(heap(), TYPE_INT, 0);
    IoPipeReader *r;
    IoPipeWriter *w;
    io_pipe(heap(), &r, &w);
    SyncWaitGroup wg = {0};
    Job j = {t, r, w, {0}, c, 0, BURROW_NO_ERROR};
    sync_wait_group_go(&wg, BURROW_FN(Func, reader, &j));
    Byte buf[64] = {0};
    for (Int i = 0; i < 5; i++) {
        Slice p = slice_from(buf, 5 + i * 10, 5 + i * 10, TYPE_BYTE);
        Error err = BURROW_NO_ERROR;
        Int n = io_pipe_writer_write(w, p, &err);
        if (n != p.len)
            testing_t_errorf_v(t, "wrote %d, got %d", p.len, n);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "write: %v", err);
        Int nn = recv_int(c);
        if (nn != n)
            testing_t_errorf_v(t, "wrote %d, read got %d", n, nn);
    }
    io_pipe_writer_close(w);
    Int nn = recv_int(c);
    if (nn != 0)
        testing_t_errorf_v(t, "final read got %d", nn);
    sync_wait_group_wait(&wg);
    io_pipe_free(r);
    chan_free(c);
}

/* Test a large write that requires multiple reads to satisfy. */
static void writer(void *env) {
    Job *j = (Job *)env;
    j->n = io_pipe_writer_write(j->w, j->data, &j->err);
    io_pipe_writer_close(j->w);
    send_int(j->c, 0);
}

static void TestPipe3(TestingT *t) {
    Chan *c = chan_make(heap(), TYPE_INT, 0);
    IoPipeReader *r;
    IoPipeWriter *w;
    io_pipe(heap(), &r, &w);
    Byte wdat[128];
    for (Int i = 0; i < LEN(wdat); i++)
        wdat[i] = (Byte)i;
    SyncWaitGroup wg = {0};
    Job j = {t, r, w, slice_from(wdat, 128, 128, TYPE_BYTE), c, 0, BURROW_NO_ERROR};
    sync_wait_group_go(&wg, BURROW_FN(Func, writer, &j));
    Byte rdat[1024];
    Int tot = 0;
    for (Int n = 1; n <= 256; n *= 2) {
        Error err = BURROW_NO_ERROR;
        Int nn = io_pipe_reader_read(r, slice_from(rdat + tot, n, n, TYPE_BYTE), &err);
        if (BURROW_FAILED(err) && !errors_is(err, io_eof)) {
            testing_t_fatalf_v(t, "read: %v", err);
            return;
        }
        /* only final two reads should be short - 1 byte, then 0 */
        Int expect = n;
        if (n == 128) {
            expect = 1;
        } else if (n == 256) {
            expect = 0;
            if (!errors_is(err, io_eof)) {
                testing_t_fatalf_v(t, "read at end: %v", err);
                return;
            }
        }
        if (nn != expect) {
            testing_t_fatalf_v(t, "read %d, expected %d, got %d", n, expect, nn);
            return;
        }
        tot += nn;
    }
    recv_int(c);
    sync_wait_group_wait(&wg);
    if (j.n != 128 || BURROW_FAILED(j.err))
        testing_t_fatalf_v(t, "write 128: %d, %v", j.n, j.err);
    if (tot != 128)
        testing_t_fatalf_v(t, "total read %d != 128", tot);
    for (Int i = 0; i < 128; i++)
        if (rdat[i] != (Byte)i)
            testing_t_fatalf_v(t, "rdat[%d] = %d", i, rdat[i]);
    io_pipe_free(r);
    chan_free(c);
}

/* Test read after/before writer close. */

typedef struct PipeTest {
    Error err;
    bool async;
    bool close_with_error;
} PipeTest;

typedef struct CloseJob {
    TestingT *t;
    IoPipeReader *r; /* close this end, */
    IoPipeWriter *w; /* or this one */
    Chan *ch;
    PipeTest tt;
} CloseJob;

static void delay_close(void *env) {
    CloseJob *j = (CloseJob *)env;
    TestingT *t = j->t;
    time_sleep(1 * TIME_MILLISECOND);
    Error err;
    if (j->r != NULL)
        err = j->tt.close_with_error ? io_pipe_reader_close_with_error(j->r, j->tt.err)
                                     : io_pipe_reader_close(j->r);
    else
        err = j->tt.close_with_error ? io_pipe_writer_close_with_error(j->w, j->tt.err)
                                     : io_pipe_writer_close(j->w);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "delayClose: %v", err);
    send_int(j->ch, 0);
}

static const PipeTest pipe_tests[] = {
    {{NULL, NULL}, true, false},  {{NULL, NULL}, true, true},
    {{NULL, NULL}, true, true}, /* err filled in with io_err_short_write */
    {{NULL, NULL}, false, false}, {{NULL, NULL}, false, true},
    {{NULL, NULL}, false, true}, /* likewise */
};

static PipeTest pipe_test(Int i) {
    PipeTest tt = pipe_tests[i];
    if (i == 2 || i == 5)
        tt.err = io_err_short_write;
    return tt;
}

static void TestPipeReadClose(TestingT *t) {
    for (Int i = 0; i < LEN(pipe_tests); i++) {
        PipeTest tt = pipe_test(i);
        Chan *c = chan_make(heap(), TYPE_INT, 1);
        IoPipeReader *r;
        IoPipeWriter *w;
        io_pipe(heap(), &r, &w);
        SyncWaitGroup wg = {0};
        CloseJob j = {t, NULL, w, c, tt};
        if (tt.async)
            sync_wait_group_go(&wg, BURROW_FN(Func, delay_close, &j));
        else
            delay_close(&j);
        Byte buf[64];
        Error err = BURROW_NO_ERROR;
        Int n = io_pipe_reader_read(r, slice_from(buf, 64, 64, TYPE_BYTE), &err);
        recv_int(c);
        Error want = BURROW_FAILED(tt.err) ? tt.err : io_eof;
        if (!errors_is(err, want))
            testing_t_errorf_v(t, "read from closed pipe: %v want %v", err, want);
        if (n != 0)
            testing_t_errorf_v(t, "read on closed pipe returned %d", n);
        err = io_pipe_reader_close(r);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "r.Close: %v", err);
        sync_wait_group_wait(&wg);
        io_pipe_free(r);
        chan_free(c);
    }
}

/* Test close on Read side during Read. */
static void TestPipeReadClose2(TestingT *t) {
    Chan *c = chan_make(heap(), TYPE_INT, 1);
    IoPipeReader *r;
    io_pipe(heap(), &r, NULL);
    SyncWaitGroup wg = {0};
    CloseJob j = {t, r, NULL, c, {{NULL, NULL}, false, false}};
    sync_wait_group_go(&wg, BURROW_FN(Func, delay_close, &j));
    Byte buf[64];
    Error err = BURROW_NO_ERROR;
    Int n = io_pipe_reader_read(r, slice_from(buf, 64, 64, TYPE_BYTE), &err);
    recv_int(c);
    if (n != 0 || !errors_is(err, io_err_closed_pipe))
        testing_t_errorf_v(t, "read from closed pipe: %v, %v want %v, %v", n, err, 0,
                           io_err_closed_pipe);
    sync_wait_group_wait(&wg);
    io_pipe_free(r);
    chan_free(c);
}

/* Test write after/before reader close. */
static void TestPipeWriteClose(TestingT *t) {
    for (Int i = 0; i < LEN(pipe_tests); i++) {
        PipeTest tt = pipe_test(i);
        Chan *c = chan_make(heap(), TYPE_INT, 1);
        IoPipeReader *r;
        IoPipeWriter *w;
        io_pipe(heap(), &r, &w);
        SyncWaitGroup wg = {0};
        CloseJob j = {t, r, NULL, c, tt};
        if (tt.async)
            sync_wait_group_go(&wg, BURROW_FN(Func, delay_close, &j));
        else
            delay_close(&j);
        Error err = BURROW_NO_ERROR;
        Int n =
            io_write_string(io_pipe_writer_as_io_writer(w), S("hello, world"), &err);
        recv_int(c);
        Error expect = BURROW_FAILED(tt.err) ? tt.err : io_err_closed_pipe;
        if (!errors_is(err, expect))
            testing_t_errorf_v(t, "write on closed pipe: %v want %v", err, expect);
        if (n != 0)
            testing_t_errorf_v(t, "write on closed pipe returned %d", n);
        err = io_pipe_writer_close(w);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "w.Close: %v", err);
        sync_wait_group_wait(&wg);
        io_pipe_free(r);
        chan_free(c);
    }
}

/* Test close on Write side during Write. */
static void TestPipeWriteClose2(TestingT *t) {
    Chan *c = chan_make(heap(), TYPE_INT, 1);
    IoPipeReader *r;
    IoPipeWriter *w;
    io_pipe(heap(), &r, &w);
    SyncWaitGroup wg = {0};
    CloseJob j = {t, NULL, w, c, {{NULL, NULL}, false, false}};
    sync_wait_group_go(&wg, BURROW_FN(Func, delay_close, &j));
    Byte buf[64] = {0};
    Error err = BURROW_NO_ERROR;
    Int n = io_pipe_writer_write(w, slice_from(buf, 64, 64, TYPE_BYTE), &err);
    recv_int(c);
    if (n != 0 || !errors_is(err, io_err_closed_pipe))
        testing_t_errorf_v(t, "write to closed pipe: %v, %v want %v, %v", n, err, 0,
                           io_err_closed_pipe);
    sync_wait_group_wait(&wg);
    io_pipe_free(r);
    chan_free(c);
}

static void write_empty(void *env) {
    Job *j = (Job *)env;
    io_pipe_writer_write(j->w, j->data, NULL);
    io_pipe_writer_close(j->w);
}

static void write_then_read_two(TestingT *t, Slice data) {
    (void)t;
    IoPipeReader *r;
    IoPipeWriter *w;
    io_pipe(heap(), &r, &w);
    SyncWaitGroup wg = {0};
    Job j = {t, r, w, data, NULL, 0, BURROW_NO_ERROR};
    sync_wait_group_go(&wg, BURROW_FN(Func, write_empty, &j));
    Byte b[2];
    io_read_full(io_pipe_reader_as_io_reader(r), slice_from(b, 2, 2, TYPE_BYTE), NULL);
    io_pipe_reader_close(r);
    sync_wait_group_wait(&wg);
    io_pipe_free(r);
}

static void TestWriteEmpty(TestingT *t) {
    static Byte none[1];
    write_then_read_two(t, slice_from(none, 0, 0, TYPE_BYTE));
}

static void TestWriteNil(TestingT *t) {
    write_then_read_two(t, slice_nil(TYPE_BYTE));
}

typedef struct AfterCloseJob {
    TestingT *t;
    IoPipeWriter *w;
    Error write_err;
} AfterCloseJob;

static void write_after_close(void *env) {
    AfterCloseJob *j = (AfterCloseJob *)env;
    TestingT *t = j->t;
    Error err = BURROW_NO_ERROR;
    io_pipe_writer_write(j->w, BURROW_B("hello"), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "got error: %q; expected none", err);
    io_pipe_writer_close(j->w);
    io_pipe_writer_write(j->w, BURROW_B("world"), &j->write_err);
}

static void TestWriteAfterWriterClose(TestingT *t) {
    IoPipeReader *r;
    IoPipeWriter *w;
    io_pipe(heap(), &r, &w);
    SyncWaitGroup wg = {0};
    AfterCloseJob j = {t, w, BURROW_NO_ERROR};
    sync_wait_group_go(&wg, BURROW_FN(Func, write_after_close, &j));

    Byte buf[100];
    Error err = BURROW_NO_ERROR;
    Int n = io_read_full(io_pipe_reader_as_io_reader(r),
                         slice_from(buf, 100, 100, TYPE_BYTE), &err);
    if (BURROW_FAILED(err) && !errors_is(err, io_err_unexpected_eof))
        testing_t_fatalf_v(t, "got: %q; want: %q", err, io_err_unexpected_eof);
    Str result = str_from_bytes(buf, n);
    sync_wait_group_wait(&wg);
    if (!str_eq(result, S("hello")))
        testing_t_errorf_v(t, "got: %q; want: %q", result, S("hello"));
    if (!errors_is(j.write_err, io_err_closed_pipe))
        testing_t_errorf_v(t, "got: %q; want: %q", j.write_err, io_err_closed_pipe);
    io_pipe_reader_close(r);
    io_pipe_free(r);
}

static const Str test_error1_text = {(const Byte *)"testError1", 10};
static const Str test_error2_text = {(const Byte *)"testError2", 10};
static const Error test_error1 = {&burrow_sentinel_error_vt, &test_error1_text};
static const Error test_error2 = {&burrow_sentinel_error_vt, &test_error2_text};

static void TestPipeCloseError(TestingT *t) {
    IoPipeReader *r;
    IoPipeWriter *w;
    Error err;

    io_pipe(heap(), &r, &w);
    io_pipe_reader_close_with_error(r, test_error1);
    err = BURROW_NO_ERROR;
    io_pipe_writer_write(w, slice_nil(TYPE_BYTE), &err);
    if (!errors_is(err, test_error1))
        testing_t_errorf_v(t, "Write error: got %v, want testError1", err);
    io_pipe_reader_close_with_error(r, test_error2);
    err = BURROW_NO_ERROR;
    io_pipe_writer_write(w, slice_nil(TYPE_BYTE), &err);
    if (!errors_is(err, test_error1))
        testing_t_errorf_v(t, "Write error: got %v, want testError1", err);
    io_pipe_free(r);

    io_pipe(heap(), &r, &w);
    io_pipe_writer_close_with_error(w, test_error1);
    err = BURROW_NO_ERROR;
    io_pipe_reader_read(r, slice_nil(TYPE_BYTE), &err);
    if (!errors_is(err, test_error1))
        testing_t_errorf_v(t, "Read error: got %v, want testError1", err);
    io_pipe_writer_close_with_error(w, test_error2);
    err = BURROW_NO_ERROR;
    io_pipe_reader_read(r, slice_nil(TYPE_BYTE), &err);
    if (!errors_is(err, test_error1))
        testing_t_errorf_v(t, "Read error: got %v, want testError1", err);
    io_pipe_free(r);
}

#define INPUT "0123456789abcdef"
#define INPUT_LEN 16
#define COUNT 8
#define READ_SIZE 2

static void concurrent_write(void *env) {
    Job *j = (Job *)env;
    TestingT *t = j->t;
    time_sleep(TIME_MILLISECOND); /* Increase probability of race */
    Error err = BURROW_NO_ERROR;
    Int n = io_pipe_writer_write(j->w, BURROW_B(INPUT), &err);
    if (n != INPUT_LEN || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Write() = (%d, %v); want (%d, nil)", n, err, INPUT_LEN);
}

static void TestPipeConcurrentWrite(TestingT *t) {
    IoPipeReader *r;
    IoPipeWriter *w;
    io_pipe(heap(), &r, &w);
    SyncWaitGroup wg = {0};
    Job j = {t, r, w, {0}, NULL, 0, BURROW_NO_ERROR};
    for (Int i = 0; i < COUNT; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, concurrent_write, &j));

    Byte buf[COUNT * INPUT_LEN];
    for (Int i = 0; i < LEN(buf); i += READ_SIZE) {
        Error err = BURROW_NO_ERROR;
        Int n = io_pipe_reader_read(
            r, slice_from(buf + i, READ_SIZE, READ_SIZE, TYPE_BYTE), &err);
        if (n != READ_SIZE || BURROW_FAILED(err))
            testing_t_errorf_v(t, "Read() = (%d, %v); want (%d, nil)", n, err,
                               READ_SIZE);
    }
    sync_wait_group_wait(&wg);

    /* Since each Write is fully gated, if multiple Read calls were needed, the
     * contents of Write should still appear together in the output. */
    for (Int i = 0; i < COUNT; i++)
        CHECK(memcmp(buf + i * INPUT_LEN, INPUT, INPUT_LEN) == 0);
    io_pipe_free(r);
}

typedef struct ReadJob {
    TestingT *t;
    IoPipeReader *r;
    Chan *c;
} ReadJob;

static void concurrent_read(void *env) {
    ReadJob *j = (ReadJob *)env;
    TestingT *t = j->t;
    time_sleep(TIME_MILLISECOND); /* Increase probability of race */
    Byte buf[READ_SIZE];
    Error err = BURROW_NO_ERROR;
    Int n = io_pipe_reader_read(j->r, slice_from(buf, READ_SIZE, READ_SIZE, TYPE_BYTE),
                                &err);
    if (n != READ_SIZE || BURROW_FAILED(err))
        testing_t_errorf_v(t, "Read() = (%d, %v); want (%d, nil)", n, err, READ_SIZE);
    Int v = (Int)buf[0] << 8 | buf[1];
    chan_send(j->c, &v);
}

/* sortBytesInGroups, with each group already packed into one number. */
static void sort_groups(Int *v, Int n) {
    for (Int i = 1; i < n; i++)
        for (Int k = i; k > 0 && v[k - 1] > v[k]; k--) {
            Int x = v[k];
            v[k] = v[k - 1];
            v[k - 1] = x;
        }
}

static void TestPipeConcurrentRead(TestingT *t) {
    IoPipeReader *r;
    IoPipeWriter *w;
    io_pipe(heap(), &r, &w);
    enum { READERS = COUNT * INPUT_LEN / READ_SIZE };
    Chan *c = chan_make(heap(), TYPE_INT, READERS);
    SyncWaitGroup wg = {0};
    ReadJob j = {t, r, c};
    for (Int i = 0; i < READERS; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, concurrent_read, &j));

    for (Int i = 0; i < COUNT; i++) {
        Error err = BURROW_NO_ERROR;
        Int n = io_pipe_writer_write(w, BURROW_B(INPUT), &err);
        if (n != INPUT_LEN || BURROW_FAILED(err))
            testing_t_errorf_v(t, "Write() = (%d, %v); want (%d, nil)", n, err,
                               INPUT_LEN);
    }
    sync_wait_group_wait(&wg);

    /* Since each read is independent, the only guarantee about the output is
     * that it is a permutation of the input in readSized groups. */
    Int got[READERS], want[READERS];
    for (Int i = 0; i < READERS; i++)
        got[i] = recv_int(c);
    for (Int i = 0; i < READERS; i++) {
        Int k = i * READ_SIZE % INPUT_LEN;
        want[i] = (Int)(Byte)INPUT[k] << 8 | (Byte)INPUT[k + 1];
    }
    sort_groups(got, READERS);
    sort_groups(want, READERS);
    CHECK(memcmp(got, want, sizeof got) == 0);
    io_pipe_free(r);
    chan_free(c);
}

#define TESTS(X)                                                                       \
    X(TestPipe1)                                                                       \
    X(TestPipe2)                                                                       \
    X(TestPipe3)                                                                       \
    X(TestPipeReadClose)                                                               \
    X(TestPipeReadClose2)                                                              \
    X(TestPipeWriteClose)                                                              \
    X(TestPipeWriteClose2)                                                             \
    X(TestWriteEmpty)                                                                  \
    X(TestWriteNil)                                                                    \
    X(TestWriteAfterWriterClose)                                                       \
    X(TestPipeCloseError)                                                              \
    X(TestPipeConcurrentWrite)                                                         \
    X(TestPipeConcurrentRead)

TESTING_MAIN(TESTS)
