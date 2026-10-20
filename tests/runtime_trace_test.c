/* Derived from Go's src/runtime/trace: trace_test.go and annotation_test.go.
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

#define TESTS(X)                                                                       \
    X(TestStartRegionLongString)                                                       \
    X(BenchmarkStartRegion)                                                            \
    X(BenchmarkNewTask)                                                                \
    X(BenchmarkLog)                                                                    \
    X(TestTraceStartStop)                                                              \
    X(TestTraceDoubleStart)                                                            \
    X(TestTraceAnnotationsHeader)

TESTING_MAIN(TESTS)
