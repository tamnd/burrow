/* runtime/trace: the execution tracer, and the annotations a program adds to
 * what it writes.
 *
 * A trace is a record of what the scheduler did while it was on: every
 * goroutine created, started, blocked, woken and ended, and every P taken and
 * given back, each with the time and the stack it happened at. It is written in
 * the format Go 1.26 writes, so go tool trace reads it:
 *
 *     Error err = trace_start(w);
 *     ...
 *     trace_stop();
 *
 * and then, with the trace in trace.out,
 *
 *     go tool trace trace.out
 *
 * A program can say what it was doing too. A task is a piece of work the
 * program cares about, such as one request, that may run on more than one
 * goroutine and that go tool trace measures from start to end. A region is a
 * stretch of one goroutine's time inside a task, and a log is a message at a
 * moment, filed under a category:
 *
 *     TraceTask *task;
 *     Context ctx = trace_new_task(a, parent, BURROW_S("makeCappuccino"), &task);
 *     TraceRegion r = trace_start_region(ctx, BURROW_S("steamMilk"));
 *     steam_milk();
 *     trace_region_end(r);
 *     trace_log(ctx, BURROW_S("orderID"), order_id);
 *     trace_task_end(task);
 *
 * Derived from Go's src/runtime/trace: trace.go, annotation.go and
 * subscribe.go.
 * Go source: go1.27.1.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package runtime/trace */

#ifndef BURROW_RUNTIME_TRACE_H
#define BURROW_RUNTIME_TRACE_H

#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where it differs from Go.
 *
 * Frames in a trace have function names and no files or line numbers, the same
 * as in a profile. burrow/runtime.h says why.
 *
 * Go stops the world for a moment when a trace starts. burrow cannot, so a
 * goroutine whose state changes at exactly the moment the trace starts, on a
 * thread that stalls for most of a second in the middle of the change, can be
 * missing that change in the first second of the trace.
 *
 * An event has to belong to a goroutine, and in Go there always is one. Here a
 * thread of the program's own can call these functions too, and from such a
 * thread the annotations write nothing.
 *
 * A trace started inside runtime_main ends when runtime_main returns, since the
 * goroutine writing it out ends with it. Stop it first. A trace that ended
 * that way still needs trace_stop before the next trace_start.
 *
 * The flight recorder is not here yet. */

/* Start enables tracing for the current program. While tracing, the trace will
 * be buffered and written to w. Start returns an error if tracing is already
 * enabled.
 *
 * The writes happen on a goroutine of their own, or a thread of their own when
 * runtime_main is not running, so w has to be safe to use from there. */
BURROW_STATIC(ret) Error trace_start(IoWriter w);

/* Stop stops the current tracing, if any. Stop only returns after all the
 * writes for the trace have completed. */
void trace_stop(void);

/* IsEnabled reports whether tracing is enabled. The information is advisory
 * only. The tracing status may have changed by the time this function
 * returns. */
bool trace_is_enabled(void);

/* Task is a data type for tracing a user-defined, logical operation. */
typedef struct TraceTask {
    uint64_t id;
} TraceTask;

/* NewTask creates a task instance with the type task_type and returns it,
 * through task, along with a Context that carries the task. If the input
 * context contains a task, the new task is its subtask.
 *
 * The task_type is used to classify task instances. Analysis tools like the Go
 * execution tracer may assume there are only a bounded number of unique task
 * types in the system.
 *
 * The returned Task's trace_task_end is used to mark the task's end. The trace
 * tool measures task latency as the time between task creation and when the
 * end is marked, and provides the latency distribution per task type. If the
 * end is marked multiple times, only the first call is used in the latency
 * measurement.
 *
 * The task and the context come from a and are never given back, the same as
 * the labels of pprof_with_labels, because other goroutines can go on using
 * the task after the context is gone. Use an arena that outlives the task, or
 * gc_allocator in a build with the collector. Returns the nil context, with *task NULL, when a says no. */
BURROW_OWNS(ret) Context trace_new_task(Alloc *a, Context pctx, Str task_type,
                                        TraceTask **task);

/* End marks the end of the operation represented by the Task. */
void trace_task_end(TraceTask *t);

/* Log emits a one-off event with the given category and message. Category can
 * be empty and the API assumes there are only a handful of unique categories
 * in the system. */
void trace_log(Context ctx, Str category, Str message);

/* Logf is like trace_log, but the value is formatted using the specified
 * format spec, as fmt_sprintf does, and only when a trace is on. */
void trace_logf(Context ctx, Str category, Str format, Slice args);

/* trace_logf with its arguments written out, as fmt_sprintf_v takes them. */
#define trace_logf_v(ctx, category, ...)                                               \
    trace_logf((ctx), (category), BURROW__FMT_FARGS(BURROW_ANY_OF, __VA_ARGS__))

/* WithRegion starts a region associated with its calling goroutine, runs fn,
 * and then ends the region. If the context carries a task, the region is
 * associated with the task. Otherwise, the region is attached to the
 * background task.
 *
 * The region_type is used to classify regions, so there should be only a
 * handful of unique region types. */
void trace_with_region(Context ctx, Str region_type, Func fn);

/* Region is a region of code whose execution time interval is traced. */
typedef struct TraceRegion {
    uint64_t id;
    Str region_type; /* borrowed from the caller of trace_start_region */
    bool active;     /* false for Go's noopRegion */
} TraceRegion;

/* StartRegion starts a region and returns it. The returned Region's
 * trace_region_end must be called from the same goroutine where the region
 * was started. Within each goroutine, regions must nest. That is, regions
 * started after this region must be ended before this region can be ended.
 *
 * region_type is kept in the region, so it has to last until the end, which a
 * string literal does. */
TraceRegion trace_start_region(Context ctx, Str region_type);

/* End marks the end of the traced code region. */
void trace_region_end(TraceRegion r);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_RUNTIME_TRACE_H */
