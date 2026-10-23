/* Tests for the CPU profile's log.
 *
 * Go's tests, with the slices spelled out as arrays and a count. The blocking
 * ones are skipped on one P, because a reader asleep in the buffer holds on to
 * its P the way Go's notetsleepg holds on to its M, and then nothing else would
 * run to wake it.
 *
 * Derived from Go's src/runtime/profbuf_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/profbuf.h"

#include "burrow/atomic.h"
#include "burrow/chan.h"
#include "burrow/func.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/time.h"

#include "check.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* An array and its length, for the arguments below that are a slice in Go. */
#define WORDS(...)                                                                     \
    (const uint64_t[]){__VA_ARGS__},                                                   \
        (Int)(sizeof((const uint64_t[]){__VA_ARGS__}) / sizeof(uint64_t))
#define PCS(...)                                                                       \
    (const uintptr_t[]){__VA_ARGS__},                                                  \
        (Int)(sizeof((const uintptr_t[]){__VA_ARGS__}) / sizeof(uintptr_t))
#define PTRS(...)                                                                      \
    (void *const[]){__VA_ARGS__},                                                      \
        (Int)(sizeof((void *const[]){__VA_ARGS__}) / sizeof(void *))
#define NONE NULL, 0

static char my_tags[100];

#define TAG(i) ((void *)&my_tags[(i)])

static void write_rec(burrow__ProfBuf *b, void *tag, int64_t now, const uint64_t *hdr,
                      Int nhdr, const uintptr_t *stk, Int nstk) {
    burrow__prof_buf_write(b, &tag, now, hdr, nhdr, stk, nstk);
}

static bool same_read(burrow__ProfBufRead got, const uint64_t *data, Int ndata,
                      void *const *tags, Int ntags) {
    if (got.ndata != ndata || got.ntags != ntags)
        return false;
    for (Int i = 0; i < ndata; i++)
        if (got.data[i] != data[i])
            return false;
    for (Int i = 0; i < ntags; i++)
        if (got.tags[i] != tags[i])
            return false;
    return true;
}

static void read_rec(TestingT *t, burrow__ProfBuf *b, const uint64_t *data, Int ndata,
                     void *const *tags, Int ntags) {
    burrow__ProfBufRead got = burrow__prof_buf_read(b, BURROW_PROF_BUF_NON_BLOCKING);
    if (!same_read(got, data, ndata, tags, ntags))
        testing_t_fatalf_v(t,
                           "unexpected profile read:\nhave %d data words and %d tags\n"
                           "want %d data words and %d tags",
                           got.ndata, got.ntags, ndata, ntags);
    if (got.eof)
        testing_t_fatalf_v(t, "unexpected eof");
}

/* --------------------------------------------------------------- TestProfBuf */

static void prof_buf_basic_write_read(void *env, TestingT *t) {
    (void)env;
    burrow__ProfBuf *b = burrow__prof_buf_new(2, 11, 1);
    write_rec(b, TAG(0), 1, WORDS(2, 3), PCS(4, 5, 6, 7, 8, 9));
    read_rec(t, b, WORDS(10, 1, 2, 3, 4, 5, 6, 7, 8, 9), PTRS(TAG(0)));
    read_rec(t, b, NONE, NONE); /* release data returned by previous read */
    write_rec(b, TAG(2), 99, WORDS(101, 102), PCS(201, 202, 203, 204));
    read_rec(t, b, WORDS(8, 99, 101, 102, 201, 202, 203, 204), PTRS(TAG(2)));
    burrow__prof_buf_free(b);
}

static void prof_buf_read_many(void *env, TestingT *t) {
    (void)env;
    burrow__ProfBuf *b = burrow__prof_buf_new(2, 50, 50);
    write_rec(b, TAG(0), 1, WORDS(2, 3), PCS(4, 5, 6, 7, 8, 9));
    write_rec(b, TAG(2), 99, WORDS(101, 102), PCS(201, 202, 203, 204));
    write_rec(b, TAG(1), 500, WORDS(502, 504), PCS(506));
    read_rec(t, b,
             WORDS(10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 8, 99, 101, 102, 201, 202, 203, 204,
                   5, 500, 502, 504, 506),
             PTRS(TAG(0), TAG(2), TAG(1)));
    burrow__prof_buf_free(b);
}

static void prof_buf_read_many_short_data(void *env, TestingT *t) {
    (void)env;
    burrow__ProfBuf *b = burrow__prof_buf_new(2, 50, 50);
    write_rec(b, TAG(0), 1, WORDS(2, 3), PCS(4, 5, 6, 7, 8, 9));
    write_rec(b, TAG(2), 99, WORDS(101, 102), PCS(201, 202, 203, 204));
    read_rec(t, b,
             WORDS(10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 8, 99, 101, 102, 201, 202, 203, 204),
             PTRS(TAG(0), TAG(2)));
    burrow__prof_buf_free(b);
}

static void prof_buf_read_many_short_tags(void *env, TestingT *t) {
    (void)env;
    burrow__ProfBuf *b = burrow__prof_buf_new(2, 50, 50);
    write_rec(b, TAG(0), 1, WORDS(2, 3), PCS(4, 5, 6, 7, 8, 9));
    write_rec(b, TAG(2), 99, WORDS(101, 102), PCS(201, 202, 203, 204));
    read_rec(t, b,
             WORDS(10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 8, 99, 101, 102, 201, 202, 203, 204),
             PTRS(TAG(0), TAG(2)));
    burrow__prof_buf_free(b);
}

static void prof_buf_read_after_overflow1(void *env, TestingT *t) {
    (void)env;
    /* overflow record synthesized by write */
    burrow__ProfBuf *b = burrow__prof_buf_new(2, 16, 5);
    write_rec(b, TAG(0), 1, WORDS(2, 3), PCS(4, 5, 6, 7, 8, 9)); /* uses 10 */
    /* reads 10 but still in use until next read */
    read_rec(t, b, WORDS(10, 1, 2, 3, 4, 5, 6, 7, 8, 9), PTRS(TAG(0)));
    write_rec(b, TAG(0), 1, WORDS(2, 3), PCS(4, 5)); /* uses 6 */
    /* reads 6 but still in use until next read */
    read_rec(t, b, WORDS(6, 1, 2, 3, 4, 5), PTRS(TAG(0)));
    /* now 10 available */
    write_rec(b, TAG(2), 99, WORDS(101, 102),
              PCS(201, 202, 203, 204, 205, 206, 207, 208, 209)); /* no room */
    for (int i = 0; i < 299; i++) {
        /* no room for overflow+this record */
        write_rec(b, TAG(3), (int64_t)(100 + i), WORDS(101, 102),
                  PCS(201, 202, 203, 204));
    }
    /* room for overflow+this record */
    write_rec(b, TAG(1), 500, WORDS(502, 504), PCS(506));
    read_rec(t, b, WORDS(5, 99, 0, 0, 300, 5, 500, 502, 504, 506), PTRS(NULL, TAG(1)));
    burrow__prof_buf_free(b);
}

static void prof_buf_read_after_overflow2(void *env, TestingT *t) {
    (void)env;
    /* overflow record synthesized by read */
    burrow__ProfBuf *b = burrow__prof_buf_new(2, 16, 5);
    write_rec(b, TAG(0), 1, WORDS(2, 3), PCS(4, 5, 6, 7, 8, 9));
    write_rec(b, TAG(2), 99, WORDS(101, 102),
              PCS(201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213));
    for (int i = 0; i < 299; i++)
        write_rec(b, TAG(3), 100, WORDS(101, 102), PCS(201, 202, 203, 204));
    /* reads 10 but still in use until next read */
    read_rec(t, b, WORDS(10, 1, 2, 3, 4, 5, 6, 7, 8, 9), PTRS(TAG(0)));
    write_rec(b, TAG(1), 500, WORDS(502, 504), NONE); /* still overflow */
    /* overflow synthesized by read */
    read_rec(t, b, WORDS(5, 99, 0, 0, 301), PTRS(NULL));
    write_rec(b, TAG(1), 500, WORDS(502, 505), PCS(506)); /* written */
    read_rec(t, b, WORDS(5, 500, 502, 505, 506), PTRS(TAG(1)));
    burrow__prof_buf_free(b);
}

static void prof_buf_read_at_end_after_overflow(void *env, TestingT *t) {
    (void)env;
    burrow__ProfBuf *b = burrow__prof_buf_new(2, 12, 5);
    write_rec(b, TAG(0), 1, WORDS(2, 3), PCS(4, 5, 6, 7, 8, 9));
    write_rec(b, TAG(2), 99, WORDS(101, 102), PCS(201, 202, 203, 204));
    for (int i = 0; i < 299; i++)
        write_rec(b, TAG(3), 100, WORDS(101, 102), PCS(201, 202, 203, 204));
    read_rec(t, b, WORDS(10, 1, 2, 3, 4, 5, 6, 7, 8, 9), PTRS(TAG(0)));
    read_rec(t, b, WORDS(5, 99, 0, 0, 300), PTRS(NULL));
    write_rec(b, TAG(1), 500, WORDS(502, 504), PCS(506));
    read_rec(t, b, WORDS(5, 500, 502, 504, 506), PTRS(TAG(1)));
    burrow__prof_buf_free(b);
}

/* Go's readBlock, which starts a goroutine that makes one blocking read and
 * checks it, and its wait. A goroutine has no t to fail, so it counts. */
typedef struct BlockRead {
    burrow__ProfBuf *b;
    const uint64_t *data;
    Int ndata;
    void *const *tags;
    Int ntags;
    Chan *c;
} BlockRead;

static uint32_t block_bad;

static void block_reader(void *arg) {
    BlockRead *r = arg;
    bool eof = r->data == NULL;
    burrow__ProfBufRead got = burrow__prof_buf_read(r->b, BURROW_PROF_BUF_BLOCKING);
    if (!same_read(got, r->data, r->ndata, r->tags, r->ntags) || got.eof != eof)
        (void)burrow__atomic_add_u32(&block_bad, 1);
    Int one = 1;
    chan_send(r->c, &one);
}

static void read_block(BlockRead *r, burrow__ProfBuf *b, const uint64_t *data,
                       Int ndata, void *const *tags, Int ntags) {
    *r = (BlockRead){b,    data,  ndata,
                     tags, ntags, chan_make(heap_allocator(), TYPE_INT, 0)};
    if (r->c == NULL || !go(BURROW_FN(Func, block_reader, r))) {
        (void)burrow__atomic_add_u32(&block_bad, 1);
        return;
    }
    time_sleep(10 * TIME_MILLISECOND); /* let goroutine run and block */
}

static void wait_block(BlockRead *r) {
    if (r->c == NULL)
        return;
    Int v;
    (void)chan_recv(r->c, &v);
    chan_free(r->c);
    r->c = NULL;
}

/* Go's readEOF, counting into the same place. */
static void read_eof(burrow__ProfBuf *b) {
    burrow__ProfBufRead got = burrow__prof_buf_read(b, BURROW_PROF_BUF_BLOCKING);
    if (got.data != NULL || got.tags != NULL || !got.eof)
        (void)burrow__atomic_add_u32(&block_bad, 1);
    got = burrow__prof_buf_read(b, BURROW_PROF_BUF_NON_BLOCKING);
    if (got.data != NULL || got.tags != NULL || !got.eof)
        (void)burrow__atomic_add_u32(&block_bad, 1);
}

/* The reader sleeps holding its P, so the writer needs another. */
static void skip_on_one_p(TestingT *t) {
    if (runtime_gomaxprocs(0) < 2)
        testing_t_skip_v(t, "needs a second P while the reader sleeps on one");
}

static burrow__ProfBuf *blocking_buf;

static void blocking_write_read_body(void *arg) {
    (void)arg;
    burrow__ProfBuf *b = blocking_buf;
    BlockRead r;
    read_block(&r, b, WORDS(10, 1, 2, 3, 4, 5, 6, 7, 8, 9), PTRS(TAG(0)));
    write_rec(b, TAG(0), 1, WORDS(2, 3), PCS(4, 5, 6, 7, 8, 9));
    wait_block(&r);
    read_block(&r, b, WORDS(8, 99, 101, 102, 201, 202, 203, 204), PTRS(TAG(2)));
    time_sleep(10 * TIME_MILLISECOND);
    write_rec(b, TAG(2), 99, WORDS(101, 102), PCS(201, 202, 203, 204));
    wait_block(&r);
    read_block(&r, b, NONE, NONE);
    burrow__prof_buf_close(b);
    wait_block(&r);
    read_block(&r, b, NONE, NONE);
    wait_block(&r);
    read_eof(b);
}

static void prof_buf_blocking_write_read(void *env, TestingT *t) {
    (void)env;
    SKIP_WITHOUT_THREADS(t);
    skip_on_one_p(t);
    burrow__atomic_store_u32(&block_bad, 0);
    blocking_buf = burrow__prof_buf_new(2, 11, 1);
    blocking_write_read_body(NULL);
    CHECK_INT_EQ(burrow__atomic_load_u32(&block_bad), 0);
    burrow__prof_buf_free(blocking_buf);
    blocking_buf = NULL;
}

static void prof_buf_data_wraparound(void *env, TestingT *t) {
    (void)env;
    burrow__ProfBuf *b = burrow__prof_buf_new(2, 16, 1024);
    for (int i = 0; i < 10; i++) {
        write_rec(b, TAG(0), 1, WORDS(2, 3), PCS(4, 5, 6, 7, 8, 9));
        read_rec(t, b, WORDS(10, 1, 2, 3, 4, 5, 6, 7, 8, 9), PTRS(TAG(0)));
        read_rec(t, b, NONE, NONE); /* release data returned by previous read */
    }
    burrow__prof_buf_free(b);
}

static void prof_buf_tag_wraparound(void *env, TestingT *t) {
    (void)env;
    burrow__ProfBuf *b = burrow__prof_buf_new(2, 1024, 2);
    for (int i = 0; i < 10; i++) {
        write_rec(b, TAG(0), 1, WORDS(2, 3), PCS(4, 5, 6, 7, 8, 9));
        read_rec(t, b, WORDS(10, 1, 2, 3, 4, 5, 6, 7, 8, 9), PTRS(TAG(0)));
        read_rec(t, b, NONE, NONE); /* release data returned by previous read */
    }
    burrow__prof_buf_free(b);
}

static void prof_buf_both_wraparound(void *env, TestingT *t) {
    (void)env;
    burrow__ProfBuf *b = burrow__prof_buf_new(2, 16, 2);
    for (int i = 0; i < 10; i++) {
        write_rec(b, TAG(0), 1, WORDS(2, 3), PCS(4, 5, 6, 7, 8, 9));
        read_rec(t, b, WORDS(10, 1, 2, 3, 4, 5, 6, 7, 8, 9), PTRS(TAG(0)));
        read_rec(t, b, NONE, NONE); /* release data returned by previous read */
    }
    burrow__prof_buf_free(b);
}

static void TestProfBuf(TestingT *t) {
    static const struct {
        Str name;
        void (*fn)(void *, TestingT *);
    } subtests[] = {
        {BURROW_S_INIT("BasicWriteRead"), prof_buf_basic_write_read},
        {BURROW_S_INIT("ReadMany"), prof_buf_read_many},
        {BURROW_S_INIT("ReadManyShortData"), prof_buf_read_many_short_data},
        {BURROW_S_INIT("ReadManyShortTags"), prof_buf_read_many_short_tags},
        {BURROW_S_INIT("ReadAfterOverflow1"), prof_buf_read_after_overflow1},
        {BURROW_S_INIT("ReadAfterOverflow2"), prof_buf_read_after_overflow2},
        {BURROW_S_INIT("ReadAtEndAfterOverflow"), prof_buf_read_at_end_after_overflow},
        {BURROW_S_INIT("BlockingWriteRead"), prof_buf_blocking_write_read},
        {BURROW_S_INIT("DataWraparound"), prof_buf_data_wraparound},
        {BURROW_S_INIT("TagWraparound"), prof_buf_tag_wraparound},
        {BURROW_S_INIT("BothWraparound"), prof_buf_both_wraparound},
    };
    for (size_t i = 0; i < sizeof subtests / sizeof subtests[0]; i++)
        testing_t_run(t, subtests[i].name, (TestingTFunc){subtests[i].fn, NULL});
}

/* ------------------------------------------------------ the wakeup tests */

static burrow__ProfBuf *wake_buf;

/* Not in Go: the writer says here that it is done with wake_buf. The reader
 * sees eof as soon as the close has marked the buffer, which is before the
 * close has woken it and returned, so the buffer can only be freed after this.
 * Go's garbage collector keeps b for as long as the writer has it. */
static Chan *double_wakeup_done;

static void double_wakeup_writer(void *arg) {
    (void)arg;
    for (int i = 0; i < 1000; i++)
        burrow__prof_buf_write(wake_buf, NULL, 1, WORDS(5, 6), PCS(7, 8));
    burrow__prof_buf_close(wake_buf);
    Int one = 1;
    chan_send(double_wakeup_done, &one);
}

static void double_wakeup_body(void *arg) {
    (void)arg;
    double_wakeup_done = chan_make(heap_allocator(), TYPE_INT, 1);
    if (double_wakeup_done == NULL)
        return;
    if (!go(BURROW_FN(Func, double_wakeup_writer, NULL))) {
        chan_free(double_wakeup_done);
        double_wakeup_done = NULL;
        return;
    }
    for (;;) {
        burrow__ProfBufRead got =
            burrow__prof_buf_read(wake_buf, BURROW_PROF_BUF_BLOCKING);
        if (got.eof)
            break;
    }
    Int v;
    (void)chan_recv(double_wakeup_done, &v);
    chan_free(double_wakeup_done);
    double_wakeup_done = NULL;
}

static void TestProfBufDoubleWakeup(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    skip_on_one_p(t);
    wake_buf = burrow__prof_buf_new(2, 16, 2);
    double_wakeup_body(NULL);
    burrow__prof_buf_free(wake_buf);
    wake_buf = NULL;
}

static Int wakeup_first;
static Chan *wakeup_done;

static void wakeup_reader(void *arg) {
    (void)arg;
    Int read = 0;
    for (;;) {
        burrow__ProfBufRead got =
            burrow__prof_buf_read(wake_buf, BURROW_PROF_BUF_BLOCKING);
        if (read == 0)
            wakeup_first = got.ndata;
        read += got.ndata;
        if (got.eof)
            break;
    }
    Int one = 1;
    chan_send(wakeup_done, &one);
}

/* Go's awaitBlockedGoroutine looks for the reader in a goroutine dump with the
 * status syscall. burrow's reader sleeps without letting go of its P, so its
 * status stays running, and the buffer is asked instead. */
static void await_blocked_reader(void) {
    for (;;) {
        runtime_gosched();
        if (burrow__prof_buf_reader_sleeping(wake_buf))
            return;
    }
}

static void wakeup_body(void *arg) {
    (void)arg;
    wakeup_done = chan_make(heap_allocator(), TYPE_INT, 0);
    if (wakeup_done == NULL || !go(BURROW_FN(Func, wakeup_reader, NULL)))
        return;

    /* Ensure that the reader is blocked */
    await_blocked_reader();
    /* NB: this writes 6 words not 4. Fine for the test.
     * The reader shouldn't wake up for this */
    burrow__prof_buf_write(wake_buf, NULL, 1, WORDS(1, 2), PCS(3, 4));

    /* The reader should still be blocked. */
    await_blocked_reader();
    burrow__prof_buf_write(wake_buf, NULL, 1, WORDS(5, 6), PCS(7, 8));
    burrow__prof_buf_close(wake_buf);

    /* Wait here so we can be sure the reader got the data */
    Int v;
    (void)chan_recv(wakeup_done, &v);
    chan_free(wakeup_done);
    wakeup_done = NULL;
}

static void TestProfBufWakeup(TestingT *t) {
    SKIP_WITHOUT_THREADS(t);
    skip_on_one_p(t);
    wake_buf = burrow__prof_buf_new(2, 16, 2);
    wakeup_first = -1;
    wakeup_body(NULL);
    if (wakeup_first < 8)
        testing_t_errorf_v(t, "first wake up at less than half full, got %d words",
                           wakeup_first);
    burrow__prof_buf_free(wake_buf);
    wake_buf = NULL;
}

#define TESTS(X)                                                                       \
    X(TestProfBuf)                                                                     \
    X(TestProfBufDoubleWakeup)                                                         \
    X(TestProfBufWakeup)

TESTING_MAIN(TESTS)
