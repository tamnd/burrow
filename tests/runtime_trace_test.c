/* Derived from Go's src/runtime/trace: trace_test.go, annotation_test.go,
 * flightrecorder_test.go and subscribe_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/context.h"
#include "burrow/io.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"
#include "burrow/runtime.h"
#include "burrow/runtime/trace.h"
#include "burrow/sync.h"
#include "burrow/sync/atomic.h"
#include "burrow/time.h"

#include <string.h>

#define S(lit) BURROW_S(lit)

/* ------------------------------------------------------- annotation_test.go */

static void TestStartRegionLongString(TestingT *t) {
    (void)t;
    /* Regression test: a region name longer than the trace region allocator's
     * block size (~64KB) used to crash with "traceRegion: alloc too large"
     * because traceStringTable.put inserted the full string into the trace map
     * before truncation. */
    Error err = trace_start(io_discard);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to start tracing: %v", err);

    enum { BIG = 70000 };
    Alloc *a = heap_allocator();
    Byte *p = mem_alloc_nozero(a, BIG, 1);
    if (p == NULL) {
        trace_stop();
        testing_t_fatal_v(t, "out of memory");
    }
    memset(p, 'x', BIG);
    Str big = {p, BIG};
    trace_region_end(trace_start_region(context_background(), big));

    trace_stop();
    mem_free(a, p, BIG, 1);
}

static void start_region_parallel(void *env, TestingPB *pb) {
    Context ctx = *(Context *)env;
    while (testing_pb_next(pb))
        trace_region_end(trace_start_region(ctx, S("region")));
}

static void BenchmarkStartRegion(TestingB *b) {
    testing_b_report_allocs(b);
    Arena ar;
    arena_init(&ar, NULL, 0);
    TraceTask *task;
    Context ctx = trace_new_task(arena_allocator(&ar), context_background(),
                                 S("benchmark"), &task);

    testing_b_run_parallel(b, BURROW_FN(TestingPBFunc, start_region_parallel, &ctx));
    trace_task_end(task);
    arena_free(&ar);
}

/* Go leaves each task to the collector. Here every goroutine has an arena that
 * starts again every so often instead. */
static void new_task_parallel(void *env, TestingPB *pb) {
    Context pctx = *(Context *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    for (Int i = 1; testing_pb_next(pb); i++) {
        TraceTask *task;
        (void)trace_new_task(arena_allocator(&ar), pctx, S("task"), &task);
        trace_task_end(task);
        if (i % 1024 == 0)
            arena_reset(&ar);
    }
    arena_free(&ar);
}

static void BenchmarkNewTask(TestingB *b) {
    testing_b_report_allocs(b);
    Arena ar;
    arena_init(&ar, NULL, 0);
    TraceTask *task;
    Context pctx = trace_new_task(arena_allocator(&ar), context_background(),
                                  S("benchmark"), &task);

    testing_b_run_parallel(b, BURROW_FN(TestingPBFunc, new_task_parallel, &pctx));
    trace_task_end(task);
    arena_free(&ar);
}

static void BenchmarkLog(TestingB *b) {
    testing_b_report_allocs(b);

    if (BURROW_FAILED(trace_start(io_discard)))
        return;

    Context ctx = context_background();
    while (testing_b_loop(b))
        trace_log(ctx, S(""), S(""));
    trace_stop();
}

/* ------------------------------------------------------------ trace_test.go */

static void TestTraceStartStop(TestingT *t) {
    if (trace_is_enabled())
        testing_t_skip_v(t, "skipping because -test.trace is set");
    Arena ar;
    arena_init(&ar, NULL, 0);
    BytesBuffer buf = BYTES_BUFFER(arena_allocator(&ar));
    Error err = trace_start(bytes_buffer_as_io_writer(&buf));
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "failed to start tracing: %v", err);
    }
    trace_stop();
    Int size = bytes_buffer_len(&buf);
    if (size == 0) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "trace is empty");
    }
    time_sleep(100 * TIME_MILLISECOND);
    Int after = bytes_buffer_len(&buf);
    arena_free(&ar);
    if (size != after)
        testing_t_fatalf_v(t, "trace writes after stop: %v -> %v", size, after);
}

static void TestTraceDoubleStart(TestingT *t) {
    if (trace_is_enabled())
        testing_t_skip_v(t, "skipping because -test.trace is set");
    trace_stop();
    Arena ar;
    arena_init(&ar, NULL, 0);
    BytesBuffer buf = BYTES_BUFFER(arena_allocator(&ar));
    Error err = trace_start(bytes_buffer_as_io_writer(&buf));
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        testing_t_fatalf_v(t, "failed to start tracing: %v", err);
    }
    err = trace_start(bytes_buffer_as_io_writer(&buf));
    bool started = BURROW_OK(err);
    trace_stop();
    trace_stop();
    arena_free(&ar);
    if (started)
        testing_t_fatalf_v(t, "succeed to start tracing second time");
}

typedef struct Annotate {
    TestingT *t;
    Alloc *a;
    Context ctx;
    const TraceTask *parent;
} Annotate;

/* A subtask with a region and a log in it. */
static void annotate(void *env) {
    Annotate *an = (Annotate *)env;
    TraceTask *task;
    Context tctx = trace_new_task(an->a, an->ctx, S("sub"), &task);
    if (task == NULL)
        testing_t_fatal_v(an->t, "trace_new_task gave no task");
    if (task->id <= an->parent->id)
        testing_t_errorf_v(an->t, "subtask id %d is not after its parent's, %d",
                           task->id, an->parent->id);
    TraceRegion r = trace_start_region(tctx, S("work"));
    trace_logf_v(tctx, S("cat"), S("value %d"), 42);
    trace_region_end(r);
    trace_task_end(task);
}

/* Not in Go. Go's internal/trace tests read the traces back, and burrow has no
 * reader of its own, so this checks what a reader looks at first, the header,
 * and that a trace with tasks, regions and logs in it starts again cleanly. */
static void TestTraceAnnotationsHeader(TestingT *t) {
    if (trace_is_enabled())
        testing_t_skip_v(t, "skipping because -test.trace is set");
    for (int round = 0; round < 2; round++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        BytesBuffer buf = BYTES_BUFFER(arena_allocator(&ar));
        Error err = trace_start(bytes_buffer_as_io_writer(&buf));
        if (BURROW_FAILED(err)) {
            arena_free(&ar);
            testing_t_fatalf_v(t, "failed to start tracing: %v", err);
        }
        if (!trace_is_enabled())
            testing_t_errorf_v(t, "trace_is_enabled() = false while tracing");
        /* Not the buffer's arena, which the goroutine writing the trace out
         * grows at the same time. */
        Arena tasks;
        arena_init(&tasks, NULL, 0);
        TraceTask *task;
        Context ctx = trace_new_task(arena_allocator(&tasks), context_background(),
                                     S("top"), &task);
        if (task == NULL) {
            trace_stop();
            arena_free(&tasks);
            arena_free(&ar);
            testing_t_fatal_v(t, "trace_new_task gave no task");
        }
        Annotate an = {t, arena_allocator(&tasks), ctx, task};
        for (int i = 0; i < 4; i++)
            trace_with_region(ctx, S("region"), BURROW_FN(Func, annotate, &an));
        trace_log(ctx, S("cat"), S("message"));
        trace_task_end(task);
        trace_stop();
        arena_free(&tasks);
        if (trace_is_enabled())
            testing_t_errorf_v(t, "trace_is_enabled() = true after trace_stop");

        static const Byte header[16] = {'g', 'o', ' ', '1', '.', '2', '6', ' ',
                                        't', 'r', 'a', 'c', 'e', 0,   0,   0};
        Bytes got = bytes_buffer_bytes(&buf);
        bool ok = got.len > 16 && memcmp(got.p, header, 16) == 0;
        Int len = got.len;
        arena_free(&ar);
        if (!ok)
            testing_t_fatalf_v(t,
                               "round %d: trace of %d bytes does not start with the "
                               "Go 1.26 header",
                               round, len);
    }
}

/* ------------------------------------------------- flightrecorder_test.go */

static TraceFlightRecorder *new_flight_recorder(TestingT *t,
                                                TraceFlightRecorderConfig cfg) {
    TraceFlightRecorder *fr = trace_new_flight_recorder(heap_allocator(), cfg);
    if (fr == NULL)
        testing_t_fatal_v(t, "out of memory");
    return fr;
}

static void TestFlightRecorderDoubleStart(TestingT *t) {
    TraceFlightRecorder *fr = new_flight_recorder(t, (TraceFlightRecorderConfig){0});
    Error err = trace_flight_recorder_start(fr);
    if (BURROW_FAILED(err)) {
        trace_flight_recorder_free(fr);
        testing_t_fatalf_v(t, "unexpected error on Start: %v", err);
    }
    err = trace_flight_recorder_start(fr);
    trace_flight_recorder_free(fr);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "expected error from double Start: %v", err);
}

static void TestFlightRecorderEnabled(TestingT *t) {
    TraceFlightRecorder *fr = new_flight_recorder(t, (TraceFlightRecorderConfig){0});
    if (trace_flight_recorder_enabled(fr)) {
        trace_flight_recorder_free(fr);
        testing_t_fatal_v(t, "flight recorder is enabled, but never started");
    }
    Error err = trace_flight_recorder_start(fr);
    if (BURROW_FAILED(err)) {
        trace_flight_recorder_free(fr);
        testing_t_fatalf_v(t, "unexpected error on Start: %v", err);
    }
    if (!trace_flight_recorder_enabled(fr)) {
        trace_flight_recorder_free(fr);
        testing_t_fatal_v(t, "flight recorder is not enabled, but started");
    }
    trace_flight_recorder_stop(fr);
    bool enabled = trace_flight_recorder_enabled(fr);
    trace_flight_recorder_free(fr);
    if (enabled)
        testing_t_fatal_v(t, "flight recorder is enabled, but stopped");
}

static void TestFlightRecorderWriteToDisabled(TestingT *t) {
    BytesBuffer buf = BYTES_BUFFER(heap_allocator());
    TraceFlightRecorder *fr = new_flight_recorder(t, (TraceFlightRecorderConfig){0});
    Error err;
    int64_t n =
        trace_flight_recorder_write_to(fr, bytes_buffer_as_io_writer(&buf), &err);
    if (BURROW_OK(err)) {
        trace_flight_recorder_free(fr);
        bytes_buffer_free(&buf);
        testing_t_fatalf_v(
            t, "successfully wrote %d bytes from disabled flight recorder", n);
    }
    err = trace_flight_recorder_start(fr);
    if (BURROW_FAILED(err)) {
        trace_flight_recorder_free(fr);
        bytes_buffer_free(&buf);
        testing_t_fatalf_v(t, "unexpected error on Start: %v", err);
    }
    trace_flight_recorder_stop(fr);
    n = trace_flight_recorder_write_to(fr, bytes_buffer_as_io_writer(&buf), &err);
    trace_flight_recorder_free(fr);
    bytes_buffer_free(&buf);
    if (BURROW_OK(err))
        testing_t_fatalf_v(
            t, "successfully wrote %d bytes from disabled flight recorder", n);
}

/* Go's testReader parses the trace with internal/trace, which burrow does not
 * have. This walks it the way the parser starts to: the header, then batches
 * that are each whole, each ending generation marked. It answers how many
 * generations there were, or -1. */
static Int test_reader(TestingT *t, Bytes tb) {
    static const Byte header[16] = {'g', 'o', ' ', '1', '.', '2', '6', ' ',
                                    't', 'r', 'a', 'c', 'e', 0,   0,   0};
    const Byte *b = tb.p;
    if (tb.len < 16 || memcmp(b, header, 16) != 0) {
        testing_t_errorf_v(
            t, "trace of %d bytes does not start with the Go 1.26 header", tb.len);
        return -1;
    }
    Int gens = 0;
    Int at = 16;
    while (at < tb.len) {
        if (b[at] == 52) { /* EvEndOfGeneration */
            gens++;
            at++;
            continue;
        }
        if (b[at] != 1 && b[at] != 51) {
            testing_t_errorf_v(t, "expected batch event at offset %d, got event %d", at,
                               b[at]);
            return -1;
        }
        at++;
        uint64_t size = 0;
        for (int field = 0; field < 4; field++) {
            uint64_t x = 0;
            unsigned shift = 0;
            while (at < tb.len && (b[at] & 0x80) != 0) {
                x |= (uint64_t)(b[at++] & 0x7f) << shift;
                shift += 7;
            }
            if (at == tb.len) {
                testing_t_errorf_v(t, "batch header cut short at offset %d", at);
                return -1;
            }
            size = x | (uint64_t)b[at++] << shift;
        }
        if (size > (uint64_t)(tb.len - at)) {
            testing_t_errorf_v(t, "batch of %d bytes at offset %d runs past the end",
                               (Int)size, at);
            return -1;
        }
        at += (Int)size;
    }
    if (gens == 0 || b[tb.len - 1] != 52) {
        testing_t_errorf_v(t, "trace does not end a generation");
        return -1;
    }
    return gens;
}

typedef struct ConcurrentWriteTo {
    TestingT *t;
    TraceFlightRecorder *fr;
    BytesBuffer *buf;
    int i;
    SyncAtomicUint32 *successes;
    SyncWaitGroup *wg;
} ConcurrentWriteTo;

static void concurrent_write_to(void *env) {
    ConcurrentWriteTo *c = env;
    TestingT *t = c->t;
    Error err;
    int64_t n =
        trace_flight_recorder_write_to(c->fr, bytes_buffer_as_io_writer(c->buf), &err);
    if (BURROW_FAILED(err) &&
        str_eq(error_text(err),
               S("call to WriteTo for trace.FlightRecorder already in progress"))) {
        if (n != 0)
            testing_t_errorf_v(t,
                               "(goroutine %d) WriteTo bytes written is non-zero for "
                               "early bail out: %d",
                               c->i, n);
        sync_wait_group_done(c->wg);
        return;
    }
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t,
                           "(goroutine %d) failed to write snapshot for unexpected "
                           "reason: %v",
                           c->i, err);
    sync_atomic_uint32_add(c->successes, 1);
    if (n == 0)
        testing_t_errorf_v(
            t, "(goroutine %d) wrote invalid trace of zero bytes in size", c->i);
    if (n != (int64_t)bytes_buffer_len(c->buf))
        testing_t_errorf_v(t,
                           "(goroutine %d) trace length doesn't match WriteTo result: "
                           "got %d, want %d",
                           c->i, n, (int64_t)bytes_buffer_len(c->buf));
    sync_wait_group_done(c->wg);
}

static void TestFlightRecorderConcurrentWriteTo(TestingT *t) {
    TraceFlightRecorder *fr = new_flight_recorder(t, (TraceFlightRecorderConfig){0});
    Error err = trace_flight_recorder_start(fr);
    if (BURROW_FAILED(err)) {
        trace_flight_recorder_free(fr);
        testing_t_fatalf_v(t, "unexpected error on Start: %v", err);
    }

    /* Start two goroutines to write snapshots.
     *
     * Most of the time one will fail and one will succeed, but we don't require
     * this. Due to a variety of factors, it's definitely possible for them both
     * to succeed. However, at least one should succeed. */
    BytesBuffer bufs[2] = {BYTES_BUFFER(heap_allocator()),
                           BYTES_BUFFER(heap_allocator())};
    SyncWaitGroup wg = {0};
    SyncAtomicUint32 successes = {0};
    ConcurrentWriteTo c[2];
    for (int i = 0; i < 2; i++) {
        c[i] = (ConcurrentWriteTo){t, fr, &bufs[i], i, &successes, &wg};
        sync_wait_group_add(&wg, 1);
        if (!go(BURROW_FN(Func, concurrent_write_to, &c[i])))
            concurrent_write_to(&c[i]);
    }
    sync_wait_group_wait(&wg);

    /* Stop tracing. */
    trace_flight_recorder_free(fr);

    /* Make sure at least one succeeded to write. */
    if (sync_atomic_uint32_load(&successes) == 0) {
        bytes_buffer_free(&bufs[0]);
        bytes_buffer_free(&bufs[1]);
        testing_t_fatal_v(
            t, "expected at least one success to write a snapshot, got zero");
    }

    /* Validate the traces that came out. */
    for (int i = 0; i < 2; i++) {
        if (bytes_buffer_len(&bufs[i]) != 0)
            (void)test_reader(t, bytes_buffer_bytes(&bufs[i]));
        bytes_buffer_free(&bufs[i]);
    }
}

typedef void (*FlightRecorderTestFunc)(TraceFlightRecorder *fr, TestingT *t,
                                       BytesBuffer *buf);

/* The snapshot callback of Go's testFlightRecorder. */
static void snapshot(TraceFlightRecorder *fr, TestingT *t, BytesBuffer *buf) {
    Error err;
    int64_t n =
        trace_flight_recorder_write_to(fr, bytes_buffer_as_io_writer(buf), &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "unexpected failure during flight recording: %v", err);
        return;
    }
    if (n < 16)
        testing_t_errorf_v(t, "expected a trace size of at least 16 bytes, got %d", n);
    if (n != (int64_t)bytes_buffer_len(buf))
        testing_t_errorf_v(t,
                           "WriteTo result doesn't match trace size: got %d, want %d",
                           n, (int64_t)bytes_buffer_len(buf));
}

/* Go's testFlightRecorder. The trace is left in buf, and the count of its
 * generations, from test_reader, is the result. */
static Int test_flight_recorder(TestingT *t, TraceFlightRecorder *fr,
                                FlightRecorderTestFunc f, BytesBuffer *buf) {
    if (trace_is_enabled())
        testing_t_skip_v(t, "cannot run flight recorder tests when tracing is enabled");

    /* Start the flight recorder. */
    Error err = trace_flight_recorder_start(fr);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "unexpected error on Start: %v", err);
        return -1;
    }

    /* Call the test function. */
    f(fr, t, buf);

    /* Stop the flight recorder. */
    trace_flight_recorder_stop(fr);

    /* Parse the trace to make sure it's not broken. */
    return test_reader(t, bytes_buffer_bytes(buf));
}

static void TestFlightRecorder(TestingT *t) {
    TraceFlightRecorder *fr = new_flight_recorder(t, (TraceFlightRecorderConfig){0});
    BytesBuffer buf = BYTES_BUFFER(heap_allocator());
    (void)test_flight_recorder(t, fr, snapshot, &buf);
    bytes_buffer_free(&buf);
    trace_flight_recorder_free(fr);
}

static void TestFlightRecorderStartStop(TestingT *t) {
    TraceFlightRecorder *fr = new_flight_recorder(t, (TraceFlightRecorderConfig){0});
    for (int i = 0; i < 5; i++) {
        BytesBuffer buf = BYTES_BUFFER(heap_allocator());
        (void)test_flight_recorder(t, fr, snapshot, &buf);
        bytes_buffer_free(&buf);
    }
    trace_flight_recorder_free(fr);
}

static void log_and_snapshot(TraceFlightRecorder *fr, TestingT *t, BytesBuffer *buf) {
    trace_log(context_background(), S("message"), S("hello"));
    snapshot(fr, t, buf);
}

static void TestFlightRecorderLog(TestingT *t) {
    TraceFlightRecorder *fr = new_flight_recorder(t, (TraceFlightRecorderConfig){0});
    BytesBuffer buf = BYTES_BUFFER(heap_allocator());
    Int gens = test_flight_recorder(t, fr, log_and_snapshot, &buf);

    /* Find the log message in the trace. Go reads the events back to find it;
     * with no reader here, the category and the message are looked for in the
     * strings the trace carries. */
    Bytes tr = bytes_buffer_bytes(&buf);
    bool found =
        gens > 0 &&
        bytes_contains(tr, (Bytes){(void *)(uintptr_t)"message", 7, 7, TYPE_BYTE}) &&
        bytes_contains(tr, (Bytes){(void *)(uintptr_t)"hello", 5, 5, TYPE_BYTE});
    bytes_buffer_free(&buf);
    trace_flight_recorder_free(fr);
    if (!found)
        testing_t_errorf_v(t,
                           "failed to find expected log message (%q, %q) in snapshot",
                           S("message"), S("hello"));
}

static void sleep_and_snapshot(TraceFlightRecorder *fr, TestingT *t, BytesBuffer *buf) {
    /* Sleep to let a few generations pass. */
    time_sleep(3 * TIME_SECOND);
    snapshot(fr, t, buf);
}

static void generation_count(TestingT *t, TraceFlightRecorderConfig cfg) {
    TraceFlightRecorder *fr = new_flight_recorder(t, cfg);
    BytesBuffer buf = BYTES_BUFFER(heap_allocator());
    Int gens = test_flight_recorder(t, fr, sleep_and_snapshot, &buf);
    bytes_buffer_free(&buf);
    trace_flight_recorder_free(fr);

    /* Go's parser puts a Sync event at the start of every generation and one
     * more at the end, and Go wants at most 3 of them, which is at most 2
     * generations. */
    enum { WANT_MAX_SYNCS = 3 };
    if (gens >= 0 && gens + 1 > WANT_MAX_SYNCS)
        testing_t_errorf_v(t, "expected at most %d sync events, found %d",
                           WANT_MAX_SYNCS, gens + 1);
}

static void generation_count_min_age(void *env, TestingT *t) {
    (void)env;
    generation_count(t, (TraceFlightRecorderConfig){.min_age = TIME_MILLISECOND});
}

static void generation_count_max_bytes(void *env, TestingT *t) {
    (void)env;
    generation_count(t, (TraceFlightRecorderConfig){.max_bytes = 16});
}

static void TestFlightRecorderGenerationCount(TestingT *t) {
    testing_t_run(t, S("MinAge"),
                  BURROW_FN(TestingTFunc, generation_count_min_age, NULL));
    testing_t_run(t, S("MaxBytes"),
                  BURROW_FN(TestingTFunc, generation_count_max_bytes, NULL));
}

static void TestTraceAndFlightRecorder(TestingT *t) {
    BytesBuffer t_buf = BYTES_BUFFER(heap_allocator());
    BytesBuffer fr_buf = BYTES_BUFFER(heap_allocator());
    Error err = trace_start(bytes_buffer_as_io_writer(&t_buf));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unable to start execution tracer: %v", err);

    TraceFlightRecorder *fr =
        new_flight_recorder(t, (TraceFlightRecorderConfig){.max_bytes = 16});
    (void)trace_flight_recorder_start(fr);
    (void)trace_flight_recorder_write_to(fr, bytes_buffer_as_io_writer(&fr_buf), &err);
    trace_flight_recorder_stop(fr);
    trace_stop();

    Int t_len = bytes_buffer_len(&t_buf);
    Int fr_len = bytes_buffer_len(&fr_buf);
    bytes_buffer_free(&t_buf);
    bytes_buffer_free(&fr_buf);
    trace_flight_recorder_free(fr);
    if (t_len == 0 || fr_len == 0)
        testing_t_errorf_v(t, "None of these should be equal to zero: %d %d", t_len,
                           fr_len);
    if (t_len <= fr_len)
        testing_t_errorf_v(t,
                           "trace should be longer than the flight recorder: trace=%d "
                           "flight record=%d",
                           t_len, fr_len);
}

/* ------------------------------------------------------- subscribe_test.go */

/* Go's validate, with test_reader for the parser and the validator. */
static void validate_traces(TestingT *t, BytesBuffer *trace, BytesBuffer *fr_trace) {
    testing_t_log_v(t, "validating", S("tracer"));
    (void)test_reader(t, bytes_buffer_bytes(trace));
    testing_t_log_v(t, "validating", S("flightRecorder"));
    (void)test_reader(t, bytes_buffer_bytes(fr_trace));
}

static TraceFlightRecorder *start_flight_recorder(TestingT *t) {
    TraceFlightRecorder *fr = new_flight_recorder(t, (TraceFlightRecorderConfig){0});
    Error err = trace_flight_recorder_start(fr);
    if (BURROW_FAILED(err)) {
        trace_flight_recorder_free(fr);
        testing_t_fatalf_v(t, "unexpected error creating flight recorder: %v", err);
    }
    return fr;
}

static bool start_trace(TestingT *t, BytesBuffer *w) {
    Error err = trace_start(bytes_buffer_as_io_writer(w));
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "unexpected error starting flight recorder: %v", err);
        return false;
    }
    return true;
}

static void stop_flight_recorder(TestingT *t, TraceFlightRecorder *fr, BytesBuffer *w) {
    Error err;
    (void)trace_flight_recorder_write_to(fr, bytes_buffer_as_io_writer(w), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected error writing trace from flight recorder: %v",
                           err);
    trace_flight_recorder_stop(fr);
}

/* The order the subtest starts and stops the two in: 'F' and 'T' start the
 * flight recorder and the tracer, 'f' and 't' stop them. */
static void subscribers(void *env, TestingT *t) {
    const char *order = env;
    if (trace_is_enabled())
        testing_t_skip_v(t, "skipping because trace is already enabled");
    BytesBuffer fr_buf = BYTES_BUFFER(heap_allocator());
    BytesBuffer t_buf = BYTES_BUFFER(heap_allocator());
    TraceFlightRecorder *fr = NULL;
    bool ok = true;
    for (const char *o = order; *o != 0 && ok; o++) {
        switch (*o) {
        case 'F':
            fr = start_flight_recorder(t);
            break;
        case 'T':
            ok = start_trace(t, &t_buf);
            break;
        case 'f':
            stop_flight_recorder(t, fr, &fr_buf);
            break;
        default:
            trace_stop();
            break;
        }
    }
    trace_flight_recorder_free(fr);
    trace_stop();
    if (ok)
        validate_traces(t, &t_buf, &fr_buf);
    bytes_buffer_free(&fr_buf);
    bytes_buffer_free(&t_buf);
}

static void TestSubscribers(TestingT *t) {
    static const struct {
        const char *name;
        const char *order;
    } cases[] = {
        {"start(flight)_start(trace)_stop(trace)_stop(flight)", "FTtf"},
        {"start(trace)_start(flight)_stop(trace)_stop(flight)", "TFtf"},
        {"start(flight)_stop(flight)_start(trace)_stop(trace)", "FfTt"},
        {"start(flight)_stop(flight)_start(trace)_stop(trace)", "FfTt"},
        {"start(flight)_start(trace)_stop(flight)_stop(trace)", "FTft"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        testing_t_run(
            t, (Str){(const Byte *)cases[i].name, (Int)strlen(cases[i].name)},
            BURROW_FN(TestingTFunc, subscribers, (void *)(uintptr_t)cases[i].order));
}

#define TESTS(X)                                                                       \
    X(TestStartRegionLongString)                                                       \
    X(BenchmarkStartRegion)                                                            \
    X(BenchmarkNewTask)                                                                \
    X(BenchmarkLog)                                                                    \
    X(TestTraceStartStop)                                                              \
    X(TestTraceDoubleStart)                                                            \
    X(TestTraceAnnotationsHeader)                                                      \
    X(TestFlightRecorderDoubleStart)                                                   \
    X(TestFlightRecorderEnabled)                                                       \
    X(TestFlightRecorderWriteToDisabled)                                               \
    X(TestFlightRecorderConcurrentWriteTo)                                             \
    X(TestFlightRecorder)                                                              \
    X(TestFlightRecorderStartStop)                                                     \
    X(TestFlightRecorderLog)                                                           \
    X(TestFlightRecorderGenerationCount)                                               \
    X(TestTraceAndFlightRecorder)                                                      \
    X(TestSubscribers)

TESTING_MAIN(TESTS)
