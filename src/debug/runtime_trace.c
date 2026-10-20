/* runtime/trace: the tracer's public face, over the runtime's
 * runtime_start_trace, runtime_stop_trace and runtime_read_trace.
 *
 * Derived from Go's src/runtime/trace: trace.go, annotation.go, subscribe.go,
 * flightrecorder.go, recorder.go, batch.go and encoding.go.
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
#include "burrow/time.h"
#include "burrow/trace.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------- the recorder
 *
 * Go's recorder and rawGeneration. A generation is shared between the ring and
 * any trace_flight_recorder_write_to that is writing it out, where Go has the
 * collector keep it, so it counts who holds it. */

typedef struct FrBatch {
    Byte *p;
    Int len;
} FrBatch;

typedef struct FrGeneration {
    uint64_t gen;
    Int size;
    int64_t min_time; /* eventTime, in nanoseconds */
    FrBatch *batches;
    Int n, cap;
    int refs; /* ring_mu held */
} FrGeneration;

struct TraceFlightRecorder {
    Alloc *a;

    /* State specific to the recorder. */
    Byte header[16];
    bool header_received;
    FrGeneration *active;
    SyncMutex ring_mu;
    FrGeneration **ring;
    Int ring_len;
    double freq; /* timestamp conversion factor, from the runtime */

    /* Externally-set options. */
    uint64_t target_size;
    Duration target_period;

    uint32_t enabled;  /* whether the flight recorder is enabled, atomic */
    SyncMutex writing; /* protects concurrent calls to WriteTo */

    /* The values of target_size and target_period we've committed to since
     * the last Start. */
    uint64_t want_size;
    Duration want_dur;
};

static FrGeneration *fr_generation_new(void) {
    FrGeneration *g =
        mem_alloc(heap_allocator(), sizeof(FrGeneration), _Alignof(FrGeneration));
    if (g == NULL)
        runtime_throw(BURROW_S("runtime/trace: out of memory"));
    g->refs = 1;
    return g;
}

static void fr_generation_release(FrGeneration *g) {
    if (g == NULL || --g->refs > 0)
        return;
    Alloc *h = heap_allocator();
    for (Int i = 0; i < g->n; i++)
        mem_free(h, g->batches[i].p, (size_t)g->batches[i].len, 1);
    if (g->batches != NULL)
        mem_free(h, g->batches, (size_t)g->cap * sizeof(FrBatch), _Alignof(FrBatch));
    mem_free(h, g, sizeof(FrGeneration), _Alignof(FrGeneration));
}

/* Lets go of the ring and the active generation. Nothing may be recording. */
static void fr_drop(TraceFlightRecorder *r) {
    sync_mutex_lock(&r->ring_mu);
    for (Int i = 0; i < r->ring_len; i++)
        fr_generation_release(r->ring[i]);
    if (r->ring != NULL)
        mem_free(heap_allocator(), r->ring,
                 (size_t)r->ring_len * sizeof(FrGeneration *),
                 _Alignof(FrGeneration *));
    r->ring = NULL;
    r->ring_len = 0;
    fr_generation_release(r->active);
    r->active = NULL;
    sync_mutex_unlock(&r->ring_mu);
}

/* readUvarint. */
static bool fr_read_uvarint(const Byte *b, Int len, Int *at, uint64_t *v) {
    uint64_t x = 0;
    unsigned s = 0;
    for (Int i = 0; i < 10 && *at + i < len; i++) {
        Byte byt = b[*at + i];
        if (byt < 0x80) {
            if (i == 9 && byt > 1)
                return false;
            *v = x | (uint64_t)byt << s;
            *at += i + 1;
            return true;
        }
        x |= (uint64_t)(byt & 0x7f) << s;
        s += 7;
    }
    return false;
}

enum {
    FR_EV_EVENT_BATCH = 1,         /* tracev2.EvEventBatch */
    FR_EV_EXPERIMENTAL_BATCH = 51, /* tracev2.EvExperimentalBatch */
    FR_EV_END_OF_GENERATION = 52,  /* tracev2.EvEndOfGeneration */
    FR_MAX_BATCH_SIZE = 64 << 10   /* tracev2.MaxBatchSize */
};

/* readBatch: the batch header in b, which is a whole batch or the end of a
 * generation. Go's says what was wrong, and the copier drops it; this only
 * says whether anything was. */
static bool fr_read_batch(const Byte *b, Int len, uint64_t *gen, uint64_t *ts) {
    *gen = 0;
    *ts = 0;
    if (len == 0)
        return false;
    if (b[0] == FR_EV_END_OF_GENERATION)
        return len == 1;
    if (b[0] != FR_EV_EVENT_BATCH && b[0] != FR_EV_EXPERIMENTAL_BATCH)
        return false;
    Int at = 1;
    uint64_t m, size;
    if (!fr_read_uvarint(b, len, &at, gen) || !fr_read_uvarint(b, len, &at, &m) ||
        !fr_read_uvarint(b, len, &at, ts) || !fr_read_uvarint(b, len, &at, &size))
        return false;
    return size <= FR_MAX_BATCH_SIZE && (uint64_t)at + size == (uint64_t)len;
}

/* frequency.mul: an unprocessed timestamp in nanoseconds. */
static int64_t fr_mul(double freq, uint64_t t) {
    return (int64_t)((double)t * freq);
}

/* recorder.Write. */
static void fr_write(TraceFlightRecorder *r, Bytes data) {
    const Byte *b = data.p;
    Int n = 0;
    if (!r->header_received) {
        if (data.len < (Int)sizeof r->header)
            return;
        memcpy(r->header, b, sizeof r->header);
        n += 16;
        r->header_received = true;
    }
    if (data.len == n)
        return;
    /* Every write from the runtime is guaranteed to be a complete batch. */
    uint64_t gen, ts;
    if (!fr_read_batch(b + n, data.len - n, &gen, &ts))
        return;
    Alloc *h = heap_allocator();
    Int len = data.len - n;
    Byte *copy = mem_alloc_nozero(h, (size_t)len, 1);
    if (copy == NULL)
        runtime_throw(BURROW_S("runtime/trace: out of memory"));
    memcpy(copy, b + n, (size_t)len);

    /* Append the batch to the current generation. */
    FrGeneration *g = r->active;
    if (gen != 0 && g->gen == 0)
        g->gen = gen;
    if (ts != 0 && (g->min_time == 0 || g->min_time > fr_mul(r->freq, ts)))
        g->min_time = fr_mul(r->freq, ts);
    g->size += len;
    if (g->n == g->cap) {
        Int cap = g->cap == 0 ? 16 : g->cap * 2;
        FrBatch *nb = mem_alloc(h, (size_t)cap * sizeof(FrBatch), _Alignof(FrBatch));
        if (nb == NULL)
            runtime_throw(BURROW_S("runtime/trace: out of memory"));
        if (g->n > 0)
            memcpy(nb, g->batches, (size_t)g->n * sizeof(FrBatch));
        if (g->batches != NULL)
            mem_free(h, g->batches, (size_t)g->cap * sizeof(FrBatch),
                     _Alignof(FrBatch));
        g->batches = nb;
        g->cap = cap;
    }
    g->batches[g->n++] = (FrBatch){copy, len};
}

/* recorder.endGeneration. */
static void fr_end_generation(TraceFlightRecorder *r) {
    Alloc *h = heap_allocator();

    /* Check if we're entering a new generation. */
    sync_mutex_lock(&r->ring_mu);

    /* Get the current trace clock time. */
    int64_t now = fr_mul(r->freq, burrow__trace_clock_now());

    /* Add the current generation to the ring. Make sure we always have at
     * least one complete generation by putting the active generation onto the
     * new list, regardless of whatever our settings are.
     *
     * N.B. Let's completely replace the ring here, so that WriteTo can just
     * make a copy and not worry about aliasing. */
    FrGeneration **ring =
        mem_alloc(h, (size_t)(r->ring_len + 1) * sizeof(FrGeneration *),
                  _Alignof(FrGeneration *));
    if (ring == NULL)
        runtime_throw(BURROW_S("runtime/trace: out of memory"));
    ring[0] = r->active;
    Int n = 1;
    Int size = r->active->size;
    Int i = r->ring_len - 1;
    for (; i >= 0; i--) {
        /* Stop adding older generations if the new ring already exceeds the
         * thresholds. This ensures we keep generations that cross a
         * threshold, but not any that lie entirely outside it. */
        if ((uint64_t)size > r->want_size || now - ring[n - 1]->min_time > r->want_dur)
            break;
        size += r->ring[i]->size;
        ring[n++] = r->ring[i];
    }
    for (; i >= 0; i--)
        fr_generation_release(r->ring[i]);
    for (Int lo = 0, hi = n - 1; lo < hi; lo++, hi--) {
        FrGeneration *t = ring[lo];
        ring[lo] = ring[hi];
        ring[hi] = t;
    }
    if (r->ring != NULL)
        mem_free(h, r->ring, (size_t)r->ring_len * sizeof(FrGeneration *),
                 _Alignof(FrGeneration *));
    r->ring = ring;
    r->ring_len = n;
    sync_mutex_unlock(&r->ring_mu);

    /* Start a new active generation. */
    r->active = fr_generation_new();
}

/* ------------------------------------------------------------- the multiplexer
 *
 * Go's traceMultiplexer, with its two subscribers: the writer trace_start was
 * given and the flight recorder. */

typedef struct TraceMultiplexer {
    SyncMutex mu;
    uint32_t enabled; /* atomic */
    int subscribers;

    SyncMutex subscribers_mu;
    bool has_writer;
    IoWriter start_writer;
    TraceFlightRecorder *flight_recorder;

    /* The copier: the subscribers it was started with, whether it is a thread
     * rather than a goroutine, and the gate it waits at until the trace is
     * on. */
    bool copier_has_writer;
    IoWriter copier_writer;
    TraceFlightRecorder *copier_flight_recorder;
    bool on_thread;
    burrow__Thread thread;
    SyncMutex gate;
    bool cancelled;
} TraceMultiplexer;

static TraceMultiplexer tracing;

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

    TraceFlightRecorder *fr = tracing.copier_flight_recorder;
    bool has_writer = tracing.copier_has_writer;
    IoWriter w = tracing.copier_writer;

    Bytes header = runtime_read_trace();
    if (has_writer)
        rtr_write(w, header);
    if (fr != NULL)
        fr_write(fr, header);
    for (;;) {
        Bytes data = runtime_read_trace();
        if (data.p == NULL)
            break;
        if (has_writer)
            rtr_write(w, data);
        if (fr != NULL)
            fr_write(fr, data);
        if (data.len == 1 && ((const uint8_t *)data.p)[0] == FR_EV_END_OF_GENERATION) {
            if (fr != NULL)
                fr_end_generation(fr);
            /* Pick up any changes. */
            sync_mutex_lock(&tracing.subscribers_mu);
            bool fr_is_new =
                fr != tracing.flight_recorder && tracing.flight_recorder != NULL;
            bool tr_is_new = tracing.has_writer &&
                             (!has_writer || tracing.start_writer.data != w.data ||
                              tracing.start_writer.vt != w.vt);
            fr = tracing.flight_recorder;
            has_writer = tracing.has_writer;
            w = tracing.start_writer;
            sync_mutex_unlock(&tracing.subscribers_mu);
            if (tr_is_new)
                rtr_write(w, header);
            if (fr_is_new)
                fr_write(fr, header);
        }
    }
}

/* The copier on a thread of its own, whose errors go with it. */
static void rtr_copy_thread(void *env) {
    rtr_copy(env);
    burrow__error_thread_exit();
}

static Error rtr_start_locked(void) {
    /* Go starts the trace and then the goroutine that reads it, which cannot
     * fail. Here making the copier can, and a trace with no reader can never
     * be stopped, so the copier comes first and waits at the gate. */
    sync_mutex_lock(&tracing.gate);
    tracing.cancelled = false;

    /* Grab the trace reader goroutine's subscribers. */
    sync_mutex_lock(&tracing.subscribers_mu);
    tracing.copier_flight_recorder = tracing.flight_recorder;
    tracing.copier_has_writer = tracing.has_writer;
    tracing.copier_writer = tracing.start_writer;
    sync_mutex_unlock(&tracing.subscribers_mu);

    /* A goroutine when there is a scheduler to run one, as in Go, and a thread
     * of its own when the program is tracing from outside runtime_main. */
    tracing.on_thread = !burrow__sched_running();
    bool started = tracing.on_thread
                       ? burrow__thread_start(&tracing.thread, rtr_copy_thread, NULL, 0)
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

/* ----------------------------------------------------------- flight recorder */

BURROW_SENTINEL_ERROR(rtr_err_fr_enabled, "flight recorder already enabled");
BURROW_SENTINEL_ERROR(rtr_err_fr_start, "cannot enable a enabled flight recorder");
BURROW_SENTINEL_ERROR(rtr_err_fr_disabled,
                      "cannot snapshot a disabled flight recorder");
BURROW_SENTINEL_ERROR(rtr_err_fr_busy,
                      "call to WriteTo for trace.FlightRecorder already in progress");

static Error rtr_subscribe_flight_recorder(TraceFlightRecorder *r) {
    sync_mutex_lock(&tracing.mu);
    sync_mutex_lock(&tracing.subscribers_mu);
    if (tracing.flight_recorder != NULL) {
        sync_mutex_unlock(&tracing.subscribers_mu);
        sync_mutex_unlock(&tracing.mu);
        return rtr_err_fr_enabled;
    }
    tracing.flight_recorder = r;
    sync_mutex_unlock(&tracing.subscribers_mu);

    Error err = rtr_added_subscriber();
    if (BURROW_FAILED(err)) {
        sync_mutex_lock(&tracing.subscribers_mu);
        tracing.flight_recorder = NULL;
        sync_mutex_unlock(&tracing.subscribers_mu);
    }
    sync_mutex_unlock(&tracing.mu);
    return err;
}

static void rtr_unsubscribe_flight_recorder(void) {
    sync_mutex_lock(&tracing.mu);
    rtr_removing_subscriber();
    sync_mutex_lock(&tracing.subscribers_mu);
    if (tracing.flight_recorder == NULL) {
        sync_mutex_unlock(&tracing.subscribers_mu);
        sync_mutex_unlock(&tracing.mu);
        return;
    }
    tracing.flight_recorder = NULL;
    sync_mutex_unlock(&tracing.subscribers_mu);
    rtr_removed_subscriber();
    sync_mutex_unlock(&tracing.mu);
}

TraceFlightRecorder *trace_new_flight_recorder(Alloc *a,
                                               TraceFlightRecorderConfig cfg) {
    TraceFlightRecorder *fr =
        mem_alloc(a, sizeof(TraceFlightRecorder), _Alignof(TraceFlightRecorder));
    if (fr == NULL)
        return NULL;
    fr->a = a;
    if (cfg.max_bytes != 0)
        fr->target_size = cfg.max_bytes;
    else
        fr->target_size = 10 << 20; /* 10 MiB. */
    if (cfg.min_age != 0)
        fr->target_period = cfg.min_age;
    else
        fr->target_period = 10 * TIME_SECOND;
    return fr;
}

void trace_flight_recorder_free(TraceFlightRecorder *fr) {
    if (fr == NULL)
        return;
    trace_flight_recorder_stop(fr);
    mem_free(fr->a, fr, sizeof(TraceFlightRecorder), _Alignof(TraceFlightRecorder));
}

Error trace_flight_recorder_start(TraceFlightRecorder *fr) {
    if (burrow__atomic_load_acquire_u32(&fr->enabled) != 0)
        return rtr_err_fr_start;
    fr->want_size = fr->target_size;
    fr->want_dur = fr->target_period;
    fr->freq = 1.0 / ((double)burrow__trace_clock_units_per_second() / 1e9);
    fr->header_received = false;
    fr->active = fr_generation_new();

    /* Start tracing, data is sent to a recorder which forwards it to our own
     * storage. */
    Error err = rtr_subscribe_flight_recorder(fr);
    if (BURROW_FAILED(err)) {
        fr_drop(fr);
        return err;
    }
    burrow__atomic_store_release_u32(&fr->enabled, 1);
    return BURROW_NO_ERROR;
}

void trace_flight_recorder_stop(TraceFlightRecorder *fr) {
    if (burrow__atomic_load_acquire_u32(&fr->enabled) == 0)
        return;
    burrow__atomic_store_release_u32(&fr->enabled, 0);
    rtr_unsubscribe_flight_recorder();

    /* Reset all state. Go's needs no lock because the reader has already
     * exited, and leaves a WriteTo still going to the collector. Here that
     * WriteTo holds its own references to what it writes, and this waits for
     * it, as the documentation says. */
    sync_mutex_lock(&fr->writing);
    fr_drop(fr);
    sync_mutex_unlock(&fr->writing);
}

bool trace_flight_recorder_enabled(const TraceFlightRecorder *fr) {
    return burrow__atomic_load_acquire_u32(&fr->enabled) != 0;
}

int64_t trace_flight_recorder_write_to(TraceFlightRecorder *fr, IoWriter w,
                                       Error *err) {
    *err = BURROW_NO_ERROR;
    if (burrow__atomic_load_acquire_u32(&fr->enabled) == 0) {
        *err = rtr_err_fr_disabled;
        return 0;
    }
    if (!sync_mutex_try_lock(&fr->writing)) {
        /* Indicates that a call to WriteTo was made while one was already in
         * progress. If the caller of WriteTo sees this error, they should use
         * the result from the other call to WriteTo. */
        *err = rtr_err_fr_busy;
        return 0;
    }

    /* Force a global buffer flush. */
    burrow__trace_advance(false);

    /* Now that everything has been flushed and written, grab whatever we
     * have.
     *
     * N.B. burrow__trace_advance blocks until the tracer goroutine has
     * actually written everything out, which means the generation we just
     * flushed must have been already been observed by the recorder
     * goroutine. */
    Alloc *h = heap_allocator();
    sync_mutex_lock(&fr->ring_mu);
    Int ngens = fr->ring_len;
    FrGeneration **gens = NULL;
    if (ngens > 0) {
        gens = mem_alloc(h, (size_t)ngens * sizeof(FrGeneration *),
                         _Alignof(FrGeneration *));
        if (gens == NULL)
            runtime_throw(BURROW_S("runtime/trace: out of memory"));
        for (Int i = 0; i < ngens; i++) {
            gens[i] = fr->ring[i];
            gens[i]->refs++;
        }
    }
    sync_mutex_unlock(&fr->ring_mu);

    /* Write the header. */
    int64_t n = 0;
    Int nw = w.vt->write(w.data, (Bytes){fr->header, 16, 16, TYPE_BYTE}, err);
    n += nw;

    /* Write all the data. */
    for (Int i = 0; i < ngens && BURROW_OK(*err); i++) {
        for (Int j = 0; j < gens[i]->n && BURROW_OK(*err); j++) {
            FrBatch d = gens[i]->batches[j];
            nw = w.vt->write(w.data, (Bytes){d.p, d.len, d.len, TYPE_BYTE}, err);
            n += nw;
        }
    }

    sync_mutex_lock(&fr->ring_mu);
    for (Int i = 0; i < ngens; i++)
        fr_generation_release(gens[i]);
    sync_mutex_unlock(&fr->ring_mu);
    if (gens != NULL)
        mem_free(h, gens, (size_t)ngens * sizeof(FrGeneration *),
                 _Alignof(FrGeneration *));
    sync_mutex_unlock(&fr->writing);
    return n;
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
