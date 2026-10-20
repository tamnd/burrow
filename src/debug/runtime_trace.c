/* runtime/trace: the tracer's public face, over the runtime's
 * runtime_start_trace, runtime_stop_trace and runtime_read_trace.
 *
 * Derived from Go's src/runtime/trace: trace.go, annotation.go and
 * subscribe.go.
 * Go source: go1.27.1.
 *
 * Copyright 2015 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/runtime/trace.h"

#include "burrow/atomic.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/exectrace.h"
#include "burrow/fmt.h"
#include "burrow/func.h"
#include "burrow/iface.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"
#include "burrow/sync.h"
#include "burrow/thread.h"
#include "burrow/trace.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------- the multiplexer
 *
 * Go's traceMultiplexer, with the one subscriber there is so far: the writer
 * trace_start was given. The flight recorder will be the other. */

typedef struct TraceMultiplexer {
    SyncMutex mu;
    uint32_t enabled; /* atomic */
    int subscribers;

    SyncMutex subscribers_mu;
    bool has_writer;
    IoWriter start_writer;

    /* The copier: the writer it was started with, whether it is a thread rather
     * than a goroutine, and the gate it waits at until the trace is on. */
    bool copier_has_writer;
    IoWriter copier_writer;
    bool on_thread;
    burrow__Thread thread;
    SyncMutex gate;
    bool cancelled;
} TraceMultiplexer;

static TraceMultiplexer tracing;

/* tracev2.EvEndOfGeneration, the one-byte chunk the reader gets at the end of
 * each generation. */
enum { RTR_EV_END_OF_GENERATION = 52 };

BURROW_SENTINEL_ERROR(rtr_err_enabled, "execution tracer already enabled");

/* Not in Go: a goroutine to copy the trace out could not be made. */
BURROW_SENTINEL_ERROR(rtr_err_no_copier,
                      "runtime/trace: cannot start the trace copier");

static void rtr_write(IoWriter w, Bytes b) {
    Error err = BURROW_NO_ERROR;
    (void)w.vt->write(w.data, b, &err);
}

/* The goroutine in Go's startLocked that copies the trace out. */
static void rtr_copy(void *env) {
    (void)env;
    sync_mutex_lock(&tracing.gate);
    bool cancelled = tracing.cancelled;
    sync_mutex_unlock(&tracing.gate);
    if (cancelled)
        return;

    bool has_writer = tracing.copier_has_writer;
    IoWriter w = tracing.copier_writer;

    Bytes header = runtime_read_trace();
    if (has_writer)
        rtr_write(w, header);
    for (;;) {
        Bytes data = runtime_read_trace();
        if (data.p == NULL)
            break;
        if (has_writer)
            rtr_write(w, data);
        if (data.len == 1 && ((const uint8_t *)data.p)[0] == RTR_EV_END_OF_GENERATION) {
            /* Pick up any changes. */
            sync_mutex_lock(&tracing.subscribers_mu);
            bool is_new = tracing.has_writer &&
                          (!has_writer || tracing.start_writer.data != w.data ||
                           tracing.start_writer.vt != w.vt);
            has_writer = tracing.has_writer;
            w = tracing.start_writer;
            sync_mutex_unlock(&tracing.subscribers_mu);
            if (is_new)
                rtr_write(w, header);
        }
    }
}

static Error rtr_start_locked(void) {
    /* Go starts the trace and then the goroutine that reads it, which cannot
     * fail. Here making the copier can, and a trace with no reader can never
     * be stopped, so the copier comes first and waits at the gate. */
    sync_mutex_lock(&tracing.gate);
    tracing.cancelled = false;

    /* Grab the trace reader goroutine's subscribers. */
    sync_mutex_lock(&tracing.subscribers_mu);
    tracing.copier_has_writer = tracing.has_writer;
    tracing.copier_writer = tracing.start_writer;
    sync_mutex_unlock(&tracing.subscribers_mu);

    /* A goroutine when there is a scheduler to run one, as in Go, and a thread
     * of its own when the program is tracing from outside runtime_main. */
    tracing.on_thread = !burrow__sched_running();
    bool started = tracing.on_thread
                       ? burrow__thread_start(&tracing.thread, rtr_copy, NULL, 0)
                       : go(BURROW_FN(Func, rtr_copy, NULL));
    if (!started) {
        tracing.on_thread = false;
        sync_mutex_unlock(&tracing.gate);
        return rtr_err_no_copier;
    }

    Error err = runtime_start_trace();
    if (BURROW_FAILED(err)) {
        tracing.cancelled = true;
        sync_mutex_unlock(&tracing.gate);
        if (tracing.on_thread) {
            (void)burrow__thread_join(&tracing.thread);
            tracing.on_thread = false;
        }
        return err;
    }
    sync_mutex_unlock(&tracing.gate);
    burrow__atomic_store_release_u32(&tracing.enabled, 1);
    return BURROW_NO_ERROR;
}

static Error rtr_added_subscriber(void) {
    if (burrow__atomic_load_acquire_u32(&tracing.enabled) != 0) {
        /* This is necessary for the trace reader goroutine to pick up on the
         * new subscriber. */
        burrow__trace_advance(false);
    } else {
        Error err = rtr_start_locked();
        if (BURROW_FAILED(err))
            return err;
    }
    tracing.subscribers++;
    return BURROW_NO_ERROR;
}

static void rtr_removing_subscriber(void) {
    if (tracing.subscribers == 0)
        return;
    tracing.subscribers--;
    if (tracing.subscribers == 0) {
        runtime_stop_trace();
        burrow__atomic_store_release_u32(&tracing.enabled, 0);
        if (tracing.on_thread) {
            (void)burrow__thread_join(&tracing.thread);
            tracing.on_thread = false;
        }
    } else {
        /* This is necessary to avoid missing trace data when the system is
         * under high load. */
        burrow__trace_advance(false);
    }
}

static void rtr_removed_subscriber(void) {
    if (tracing.subscribers > 0) {
        /* This is necessary for the trace reader goroutine to pick up on the
         * new subscriber. */
        burrow__trace_advance(false);
    }
}

Error trace_start(IoWriter w) {
    sync_mutex_lock(&tracing.mu);
    sync_mutex_lock(&tracing.subscribers_mu);
    if (tracing.has_writer) {
        sync_mutex_unlock(&tracing.subscribers_mu);
        sync_mutex_unlock(&tracing.mu);
        return rtr_err_enabled;
    }
    tracing.has_writer = true;
    tracing.start_writer = w;
    sync_mutex_unlock(&tracing.subscribers_mu);

    Error err = rtr_added_subscriber();
    if (BURROW_FAILED(err)) {
        sync_mutex_lock(&tracing.subscribers_mu);
        tracing.has_writer = false;
        tracing.start_writer = (IoWriter){0};
        sync_mutex_unlock(&tracing.subscribers_mu);
    }
    sync_mutex_unlock(&tracing.mu);
    return err;
}

void trace_stop(void) {
    sync_mutex_lock(&tracing.mu);
    rtr_removing_subscriber();
    sync_mutex_lock(&tracing.subscribers_mu);
    if (!tracing.has_writer) {
        sync_mutex_unlock(&tracing.subscribers_mu);
        sync_mutex_unlock(&tracing.mu);
        return;
    }
    tracing.has_writer = false;
    tracing.start_writer = (IoWriter){0};
    sync_mutex_unlock(&tracing.subscribers_mu);
    rtr_removed_subscriber();
    sync_mutex_unlock(&tracing.mu);
}

bool trace_is_enabled(void) {
    return burrow__atomic_load_acquire_u32(&tracing.enabled) != 0;
}

/* ----------------------------------------------------------------- annotation */

/* traceContextKey. */
static const Type rtr_context_key_desc = {
    {(const Byte *)"traceContextKey", 15},
    {(const Byte *)"runtime/trace", 13},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x7274636bU, /* "rtck" */
    NULL,
};

static const uint8_t rtr_context_key_value = 0;

static const Any rtr_context_key = {&rtr_context_key_desc,
                                    (void *)(uintptr_t)&rtr_context_key_value};

/* What the context holds under the key: a *Task. */
static const Type rtr_task_desc = {
    {(const Byte *)"Task", 4},
    {(const Byte *)"runtime/trace", 13},
    KIND_STRUCT,
    (uint32_t)sizeof(TraceTask),
    (uint16_t)_Alignof(TraceTask),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

/* task id issued last time */
static uint64_t rtr_last_task_id;

static uint64_t rtr_new_id(void) {
    return burrow__atomic_add_u64(&rtr_last_task_id, 1) + 1;
}

static uint64_t rtr_from_context(Context ctx) {
    if (ctx.vt == NULL)
        return 0;
    Any v = context_value(ctx, rtr_context_key);
    if (v.t != &rtr_task_desc || v.data == NULL)
        return 0;
    return ((const TraceTask *)v.data)->id;
}

/* The stack of the caller of the function this is written in, for the event
 * that function writes. Only walked when a trace is on. */
#define RTR_STACK(pcs, n)                                                              \
    Uintptr pcs[BURROW_PROFILE_DEPTH];                                                 \
    Int n = burrow__trace_enabled()                                                    \
                ? burrow__callers(BURROW_WALK_FROM, 0, pcs, BURROW_PROFILE_DEPTH)      \
                : 0

BURROW_NOINLINE Context trace_new_task(Alloc *a, Context pctx, Str task_type,
                                       TraceTask **task) {
    *task = NULL;
    uint64_t pid = rtr_from_context(pctx);
    uint64_t id = rtr_new_id();
    /* We allocate a new task even when the tracing is disabled because the
     * context and task can be used across trace enable/disable boundaries,
     * which complicates the problem. */
    TraceTask *s = mem_alloc(a, sizeof(TraceTask), _Alignof(TraceTask));
    if (s == NULL)
        return (Context){0};
    s->id = id;
    Context ctx =
        context_with_value(a, pctx, rtr_context_key, BURROW_ANY(&rtr_task_desc, s));
    if (ctx.vt == NULL) {
        mem_free(a, s, sizeof(TraceTask), _Alignof(TraceTask));
        return ctx;
    }
    RTR_STACK(pcs, n);
    burrow__trace_user_task_create(id, pid, task_type, pcs, n);
    *task = s;
    return ctx;
}

BURROW_NOINLINE void trace_task_end(TraceTask *t) {
    if (t == NULL)
        return;
    RTR_STACK(pcs, n);
    burrow__trace_user_task_end(t->id, pcs, n);
}

BURROW_NOINLINE void trace_log(Context ctx, Str category, Str message) {
    uint64_t id = rtr_from_context(ctx);
    RTR_STACK(pcs, n);
    burrow__trace_user_log(id, category, message, pcs, n);
}

BURROW_NOINLINE void trace_logf(Context ctx, Str category, Str format, Slice args) {
    if (!trace_is_enabled())
        return;
    uint64_t id = rtr_from_context(ctx);
    Alloc *a = heap_allocator();
    Str message = fmt_sprintf(a, format, args);
    RTR_STACK(pcs, n);
    burrow__trace_user_log(id, category, message, pcs, n);
    if (message.len > 0)
        mem_free(a, (void *)(uintptr_t)message.p, (size_t)message.len, 1);
}

enum { REGION_START_CODE = 0, REGION_END_CODE = 1 };

BURROW_NOINLINE void trace_with_region(Context ctx, Str region_type, Func fn) {
    uint64_t id = rtr_from_context(ctx);
    RTR_STACK(pcs, n);
    burrow__trace_user_region(id, REGION_START_CODE, region_type, pcs, n);
    fn.f(fn.env);
    burrow__trace_user_region(id, REGION_END_CODE, region_type, pcs, n);
}

BURROW_NOINLINE TraceRegion trace_start_region(Context ctx, Str region_type) {
    if (!trace_is_enabled())
        return (TraceRegion){0, {NULL, 0}, false};
    uint64_t id = rtr_from_context(ctx);
    RTR_STACK(pcs, n);
    burrow__trace_user_region(id, REGION_START_CODE, region_type, pcs, n);
    return (TraceRegion){id, region_type, true};
}

BURROW_NOINLINE void trace_region_end(TraceRegion r) {
    if (!r.active)
        return;
    RTR_STACK(pcs, n);
    burrow__trace_user_region(r.id, REGION_END_CODE, r.region_type, pcs, n);
}
