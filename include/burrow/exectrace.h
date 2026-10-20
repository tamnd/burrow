/* The execution tracer, the runtime half of runtime/trace.
 *
 * While a trace is on, the scheduler writes an event for every change it makes
 * to a goroutine or a P: created, started, stopped, blocked, woken, ended, and
 * a P taken or given back. The events go into a buffer per thread, the buffers
 * are cut into generations of about a second each, and runtime_read_trace hands
 * them out in the format Go 1.26 writes, which is the one go tool trace reads.
 *
 * Every hook in the scheduler has the same shape, which is Go's:
 *
 *     burrow__TraceLocker tl;
 *     bool traced = burrow__trace_acquire(&tl);
 *     ...change the goroutine's status...
 *     if (traced) {
 *         burrow__trace_go_unpark(&tl, gp);
 *         burrow__trace_release(&tl);
 *     }
 *
 * The acquire comes before the status changes, not after, because the end of a
 * generation writes down the status of every goroutine that had no event in it,
 * and a change that it misses has to be one that the next generation sees. A
 * thread between an acquire and its release holds nothing but its own buffer,
 * so it may not take the scheduler lock or park, and nothing it calls may.
 *
 * Internal, for the scheduler, runtime/trace and the tests.
 *
 * Derived from Go's src/runtime/trace.go, traceruntime.go and tracestatus.go.
 * Go source: go1.27.1.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_EXECTRACE_H
#define BURROW_EXECTRACE_H

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/platform.h"
#include "burrow/slice.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct burrow__G burrow__G;
typedef struct burrow__P burrow__P;
typedef struct burrow__M burrow__M;
typedef struct burrow__TraceThread burrow__TraceThread;

/* What a goroutine carries for the tracer. Go's gTraceState. `seq` and
 * `pending` start again whenever a G is handed out for a new goroutine, and
 * `traced` does not, as in Go, since the end of each generation keeps it right
 * for every G there is.
 *
 * `traced` says whether the goroutine's status has been written in a
 * generation, indexed by the generation mod 3, and `seq` counts the events in
 * a generation that other threads have to put in order, mod 2. `pending` is
 * not in Go: it marks a goroutine that was made by a thread with no P while a
 * trace was on. Such a thread has nowhere to write the creation, so the thread
 * that first runs the goroutine writes it instead, just before the start. */
typedef struct burrow__TraceG {
    uint32_t traced[3];
    uint64_t seq[2];
    uint32_t pending;
} burrow__TraceG;

/* The current generation, or zero when no trace is on. The only thing a hook
 * reads when tracing is off. */
extern uint64_t burrow__trace_gen;

typedef struct burrow__TraceLocker {
    burrow__TraceThread *tt;
    burrow__M *m; /* NULL on a thread that is not one of the scheduler's */
    uint64_t gen;
} burrow__TraceLocker;

bool burrow__trace_acquire_slow(burrow__TraceLocker *tl);

/* Go's traceAcquire. Answers false, quickly, when there is no trace. */
static inline bool burrow__trace_acquire(burrow__TraceLocker *tl) {
    if (BURROW_LIKELY(burrow__atomic_load_relaxed_u64(&burrow__trace_gen) == 0))
        return false;
    return burrow__trace_acquire_slow(tl);
}

/* Go's traceRelease. */
void burrow__trace_release(burrow__TraceLocker *tl);

/* Whether a trace is on. Go's traceEnabled. */
static inline bool burrow__trace_enabled(void) {
    return burrow__atomic_load_relaxed_u64(&burrow__trace_gen) != 0;
}

/* The events. `gp` is the goroutine the event is about, passed in because most
 * of these are written on the scheduler's own stack after it has stopped
 * running it. Each one first writes the statuses the reader has not been told
 * about in this generation, the way Go's eventWriter does. */
void burrow__trace_go_create(burrow__TraceLocker *tl, burrow__G *newg, bool blocked);
void burrow__trace_go_start(burrow__TraceLocker *tl, burrow__G *gp);
void burrow__trace_go_destroy(burrow__TraceLocker *tl, burrow__G *gp);
void burrow__trace_go_stop(burrow__TraceLocker *tl, burrow__G *gp);
void burrow__trace_go_park(burrow__TraceLocker *tl, burrow__G *gp);
void burrow__trace_go_unpark(burrow__TraceLocker *tl, burrow__G *gp);
void burrow__trace_go_switch(burrow__TraceLocker *tl, burrow__G *gp, burrow__G *next);
void burrow__trace_proc_start(burrow__TraceLocker *tl);
void burrow__trace_proc_stop(burrow__TraceLocker *tl, burrow__P *p);

/* Why the next stop on this thread happens, with the stack that led to it.
 * Called on the goroutine's own stack just before it hands over to the
 * scheduler, since by the time the event is written the stack that matters is
 * not the one being run. Does nothing when no trace is on. */
enum { BURROW_TRACE_PARK = 1, BURROW_TRACE_GOSCHED, BURROW_TRACE_PREEMPTED };
void burrow__trace_capture(int why);

/* The goroutine has just been handed out for a new goroutine. Clears what the
 * tracer knew about the last one, and marks it pending when a trace is on and
 * `has_p` says the caller cannot write its creation. */
void burrow__trace_g_reset(burrow__G *g, bool has_p);

/* A scheduler thread is about to stop for good. Writes the end of whatever P it
 * still has and gives its buffer back for the next thread. */
void burrow__trace_m_exit(burrow__M *m);

/* runtime_main is starting and ending. Goroutine ids start again at one in
 * every run, so the tracer adds how many earlier runs used to keep them apart,
 * and a trace started inside a run cannot outlive it. */
void burrow__trace_run_start(void);
void burrow__trace_run_end(uint64_t ngoid);

/* For the advance, in sched.c: calls fn on every goroutine there has been in
 * this run, with the scheduler lock held. fn may not take a lock but its own. */
void burrow__sched_walk_gs(void (*fn)(burrow__G *g, void *arg), void *arg);

/* Not in Go: makes the current generation end now rather than at the next
 * second. Go's runtime/trace has this as runtime_traceAdvance. */
void burrow__trace_advance(bool stop);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_EXECTRACE_H */
