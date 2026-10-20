/* The execution tracer. See burrow/exectrace.h for how the scheduler calls it.
 *
 * The format is Go's, byte for byte, so that go tool trace reads what this
 * writes. A trace is a header and then a run of batches. A batch is the events
 * one thread wrote into one buffer, each with the time since the one before it,
 * and every batch says which generation it is in. A generation ends about once
 * a second: the status of every goroutine that had no event in it is written
 * down, along with the stacks and strings its events referred to by number, so
 * that a reader can start at any generation and know where everything was.
 *
 * Go stops the world at the start and for a moment at the end of each
 * generation. burrow has no way to stop the world, so it does without, and the
 * difference is one window, which is the same one Go's comment on traceAcquire
 * describes for a thread that has not yet seen the trace start. A thread that
 * read that tracing was off, and then stalled for longer than the whole first
 * generation before making its change, makes that change without an event, and
 * a reader may then find a status in the first generation that the second one
 * disagrees with. It takes a stall of most of a second at exactly that point.
 *
 * Also unlike Go, nothing here wakes a reader from inside the scheduler. The
 * reader polls, and it is the reader that ends a generation when the second is
 * up, which also means a generation never ends before the reader is ready for
 * the next one, which is what Go uses a semaphore between the two for.
 *
 * Derived from Go's src/runtime/trace.go, tracebuf.go, traceevent.go,
 * traceruntime.go, tracestack.go, tracestatus.go, tracestring.go and
 * tracetime.go.
 * Go source: go1.27.1.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/exectrace.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/lock.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/pal.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"
#include "burrow/time.h"
#include "burrow/trace.h"
#include "burrow/waitreason.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- the format */

/* Go's tracev2.EventType, the ones written here. */
enum {
    EV_EVENT_BATCH = 1,
    EV_STACKS = 2,
    EV_STACK = 3,
    EV_STRINGS = 4,
    EV_STRING = 5,
    EV_FREQUENCY = 8,
    EV_PROCS_CHANGE = 9,
    EV_PROC_START = 10,
    EV_PROC_STOP = 11,
    EV_PROC_STATUS = 13,
    EV_GO_CREATE = 14,
    EV_GO_START = 16,
    EV_GO_DESTROY = 17,
    EV_GO_STOP = 19,
    EV_GO_BLOCK = 20,
    EV_GO_UNBLOCK = 21,
    EV_GO_STATUS = 25,
    EV_USER_TASK_BEGIN = 40,
    EV_USER_TASK_END = 41,
    EV_USER_REGION_BEGIN = 42,
    EV_USER_REGION_END = 43,
    EV_USER_LOG = 44,
    EV_GO_SWITCH = 45,
    EV_GO_CREATE_BLOCKED = 47,
    EV_SYNC = 50,
    EV_CLOCK_SNAPSHOT = 51,
    EV_END_OF_GENERATION = 52
};

/* Go's tracev2.GoStatus and tracev2.ProcStatus. */
enum { GO_RUNNABLE = 1, GO_RUNNING = 2, GO_WAITING = 4 };
enum { PROC_RUNNING = 1, PROC_IDLE = 2 };

/* Go's traceBytesPerNumber, tracev2.MaxBatchSize and
 * tracev2.MaxEventTrailerDataSize. */
#define BYTES_PER_NUMBER 10
#define MAX_BATCH_SIZE (64 << 10)
#define MAX_STRING_SIZE (1 << 10)

/* Go's traceTimeDiv. Time stamps are nanoseconds over 64, which is enough
 * resolution for a scheduler and makes most deltas fit in a byte or two. */
#define TIME_DIV 64

/* The thread a batch belongs to when it belongs to none, Go's ^uint64(0). */
#define NO_THREAD UINT64_MAX

/* How long a generation lasts and how often a reader with nothing to read
 * looks again. Go's advancer uses the same second. */
#define GEN_NS ((int64_t)1000000000)
#define POLL TIME_MILLISECOND

static const uint8_t trace_header[16] = {'g', 'o', ' ', '1', '.', '2', '6', ' ',
                                         't', 'r', 'a', 'c', 'e', 0,   0,   0};
static const uint8_t trace_end_of_generation[1] = {EV_END_OF_GENERATION};

/* Go's traceBlockReason strings, the ones a burrow wait can turn into. */
enum {
    BLOCK_UNSPECIFIED,
    BLOCK_FOREVER,
    BLOCK_NET,
    BLOCK_SELECT,
    BLOCK_COND_WAIT,
    BLOCK_SYNC,
    BLOCK_CHAN_SEND,
    BLOCK_CHAN_RECV,
    BLOCK_SLEEP,
    BLOCK_SYNCTEST,
    NBLOCK
};

static const char *const block_reason_strings[NBLOCK] = {
    "unspecified", "forever",   "network",      "select", "sync.(*Cond).Wait",
    "sync",        "chan send", "chan receive", "sleep",  "synctest",
};

/* Go's traceGoStopReason strings. */
enum { STOP_UNSPECIFIED, STOP_GOSCHED, STOP_PREEMPTED, NSTOP };

static const char *const stop_reason_strings[NSTOP] = {
    "unspecified",
    "runtime.Gosched",
    "preempted",
};

/* ----------------------------------------------------------------- the state */

/* Go's traceBuf. The batch header is written at the front when a buffer is
 * taken, with room left for the length, which is filled in when it is full. */
typedef struct TraceBuf {
    struct TraceBuf *link;
    uint64_t last_time;
    Int pos;
    Int len_pos;
    uint8_t arr[MAX_BATCH_SIZE - 64];
} TraceBuf;

/* Go's mTraceState, kept apart from the M because the buffers have to outlive
 * the thread that wrote them until the end of the generation. Never freed. A
 * scheduler thread that stops gives its own back for the next one to use. */
struct burrow__TraceThread {
    /* Held from an acquire to its release, and by the end of a generation
     * while it takes this thread's buffer. Go uses a flag the end of the
     * generation spins on instead, which is the same thing. */
    burrow__Lock lock;
    uint32_t depth; /* acquires not yet released, read only by this thread */
    uint64_t gen;   /* the generation the outermost acquire saw */
    uint64_t id;    /* the thread id the batches carry */
    TraceBuf *buf[2];

    /* What burrow__trace_capture saw, for the stop that follows it. */
    int why;
    Int ncap;
    Uintptr cap[BURROW_PROFILE_DEPTH];

    burrow__TraceThread *all;  /* every one there has been, under tt_lock */
    burrow__TraceThread *free; /* the ones nobody is using, under tt_lock */
};

/* Go's traceStringTable, one per generation. Each new string is written out
 * straight away, into a batch of its own that belongs to no thread. */
typedef struct TraceStrEnt {
    uint64_t hash;
    uint64_t id;
    Int len;
    uint8_t *bytes;
} TraceStrEnt;

typedef struct TraceStrings {
    burrow__Lock lock;
    TraceBuf *buf;
    uint64_t last;
    TraceStrEnt *tab;
    Int cap;
    Int n;
} TraceStrings;

/* Go's traceStackTable, one per generation, written out when it ends. */
typedef struct TraceStackEnt {
    uint64_t hash;
    uint64_t id;
    Int n;
    Uintptr *pcs;
} TraceStackEnt;

typedef struct TraceStacks {
    burrow__Lock lock;
    uint64_t last;
    TraceStackEnt *tab;
    Int cap;
    Int n;
} TraceStacks;

/* Whether a trace is on, as start and stop see it. */
enum { STATE_OFF, STATE_ON, STATE_STOPPING };

uint64_t burrow__trace_gen;

typedef struct Trace {
    /* Taken by start and stop, around the state. */
    burrow__Lock ctl;
    int state;
    bool in_run; /* started inside runtime_main, so it ends with it */

    /* Taken by anything that ends a generation, and by the ends of a run. The
     * end of a generation never parks, so a thread lock is enough. */
    burrow__Lock adv_lock;
    uint64_t last_gen; /* Go's lastNonZeroGen */
    int64_t gen_start; /* when the current generation began, by nanotime */
    bool run_active;   /* runtime_main is running, so there are goroutines */
    uint64_t run_base; /* added to every goroutine id in this run */
    uint64_t next_base;

    /* Go's trace.lock, and what it covers. */
    burrow__Lock lock;
    TraceBuf *full_head[2];
    TraceBuf *full_tail[2];
    TraceBuf *empty;
    TraceBuf *reading;
    uint64_t reader_gen;
    uint64_t flushed_gen;
    bool header_written;
    bool eog_written;
    bool shutdown;
    bool session; /* a reader has something to read, the header if nothing else */

    /* The threads. A thread with no M shares `foreign`, whose lock is held for
     * as long as its acquire is, since two such threads cannot be told apart. */
    burrow__Lock tt_lock;
    burrow__TraceThread *tts;
    burrow__TraceThread *tt_free;
    uint64_t next_mid;
    burrow__TraceThread foreign;

    TraceStrings strings[2];
    TraceStacks stacks[2];
    uint64_t block_ids[2][NBLOCK];
    uint64_t stop_ids[2][NSTOP];

    /* Go's pTraceState, for each P by its id. */
    uint32_t ptraced[BURROW_MAXPROCS][3];
    uint64_t pseq[BURROW_MAXPROCS][2];
} Trace;

static Trace trace;

/* ------------------------------------------------------------ status and seq */

/* Go's traceSchedResourceState, spread over a goroutine's or a P's fields. */

static bool status_traced(uint32_t *traced, uint64_t gen) {
    return burrow__atomic_load_acquire_u32(&traced[gen % 3]) != 0;
}

static void ready_next_gen(uint32_t *traced, uint64_t *seq, uint64_t gen) {
    uint64_t next = gen + 1;
    burrow__atomic_store_relaxed_u64(&seq[next % 2], 0);
    burrow__atomic_store_release_u32(&traced[next % 3], 0);
}

static bool acquire_status(uint32_t *traced, uint64_t *seq, uint64_t gen) {
    if (status_traced(traced, gen))
        return false;
    uint32_t zero = 0;
    if (!burrow__atomic_cas_u32(&traced[gen % 3], &zero, 1))
        return false;
    ready_next_gen(traced, seq, gen);
    return true;
}

static uint64_t next_seq(uint64_t *seq, uint64_t gen) {
    uint64_t s = burrow__atomic_load_relaxed_u64(&seq[gen % 2]) + 1;
    burrow__atomic_store_relaxed_u64(&seq[gen % 2], s);
    return s;
}

static uint64_t goid(burrow__G *g) {
    return trace.run_base + burrow__atomic_load_u64(&g->id);
}

/* ------------------------------------------------------------------- buffers */

static uint64_t trace_now(void) {
    return (uint64_t)burrow__nanotime() / TIME_DIV;
}

/* Go's traceWriter: a buffer slot, and what to write in the header of the next
 * buffer that goes in it. */
typedef struct Writer {
    TraceBuf **slot;
    uint64_t gen;
    uint64_t mid;
} Writer;

static void buf_byte(TraceBuf *b, uint8_t v) {
    b->arr[b->pos++] = v;
}

static void buf_varint(TraceBuf *b, uint64_t v) {
    while (v >= 0x80) {
        b->arr[b->pos++] = (uint8_t)(0x80 | (v & 0x7f));
        v >>= 7;
    }
    b->arr[b->pos++] = (uint8_t)v;
}

/* Go's varintAt: a number in exactly BYTES_PER_NUMBER bytes, for the length
 * that is only known once the batch is done. */
static void buf_varint_at(TraceBuf *b, Int pos, uint64_t v) {
    for (int i = 0; i < BYTES_PER_NUMBER; i++) {
        b->arr[pos++] =
            i < BYTES_PER_NUMBER - 1 ? (uint8_t)(0x80 | (v & 0x7f)) : (uint8_t)v;
        v >>= 7;
    }
}

/* Go's traceBufFlush. trace.lock held. */
static void buf_flush(TraceBuf *b, uint64_t gen) {
    buf_varint_at(b, b->len_pos, (uint64_t)(b->pos - (b->len_pos + BYTES_PER_NUMBER)));
    b->link = NULL;
    if (trace.full_tail[gen % 2] != NULL)
        trace.full_tail[gen % 2]->link = b;
    else
        trace.full_head[gen % 2] = b;
    trace.full_tail[gen % 2] = b;
}

/* Go's traceWriter.refill: hands in the slot's buffer if it has one and puts a
 * new one in its place, with the batch header written. */
static void writer_refill(Writer *w) {
    burrow__lock(&trace.lock);
    if (*w->slot != NULL)
        buf_flush(*w->slot, w->gen);
    TraceBuf *b = trace.empty;
    if (b != NULL)
        trace.empty = b->link;
    burrow__unlock(&trace.lock);

    if (b == NULL) {
        b = mem_alloc_nozero(heap_allocator(), sizeof(TraceBuf), _Alignof(TraceBuf));
        if (b == NULL)
            runtime_throw(BURROW_S("trace: out of memory"));
        b->last_time = 0;
    }

    uint64_t ts = trace_now();
    if (ts <= b->last_time)
        ts = b->last_time + 1;
    b->last_time = ts;
    b->link = NULL;
    b->pos = 0;
    buf_byte(b, EV_EVENT_BATCH);
    buf_varint(b, w->gen);
    buf_varint(b, w->mid);
    buf_varint(b, ts);
    b->len_pos = b->pos;
    b->pos += BYTES_PER_NUMBER;
    *w->slot = b;
}

/* Go's traceWriter.ensure. Answers whether it had to start a new buffer. */
static bool writer_ensure(Writer *w, Int n) {
    TraceBuf *b = *w->slot;
    if (b != NULL && (Int)sizeof b->arr - b->pos >= n)
        return false;
    writer_refill(w);
    return true;
}

/* Go's traceWriter.event. */
static void writer_event(Writer *w, uint8_t ev, const uint64_t *args, int nargs) {
    (void)writer_ensure(w, 1 + (Int)(nargs + 1) * BYTES_PER_NUMBER);
    TraceBuf *b = *w->slot;
    uint64_t ts = trace_now();
    if (ts <= b->last_time)
        ts = b->last_time + 1;
    uint64_t diff = ts - b->last_time;
    b->last_time = ts;
    buf_byte(b, ev);
    buf_varint(b, diff);
    for (int i = 0; i < nargs; i++)
        buf_varint(b, args[i]);
}

#define EVENT(w, ev, ...)                                                              \
    writer_event((w), (ev), (const uint64_t[]){__VA_ARGS__},                           \
                 (int)(sizeof((const uint64_t[]){__VA_ARGS__}) / sizeof(uint64_t)))

/* Hands a buffer that belongs to no thread in. */
static void writer_flush(Writer *w) {
    if (*w->slot == NULL)
        return;
    burrow__lock(&trace.lock);
    buf_flush(*w->slot, w->gen);
    burrow__unlock(&trace.lock);
    *w->slot = NULL;
}

/* ------------------------------------------------------------------- strings */

static uint64_t hash_bytes(const void *p, size_t n) {
    return runtime_memhash(p, n, 0x9e3779b97f4a7c15u);
}

static bool strings_grow(TraceStrings *t) {
    Int ncap = t->cap == 0 ? 64 : t->cap * 2;
    TraceStrEnt *tab = mem_alloc_array(heap_allocator(), (size_t)ncap,
                                       sizeof(TraceStrEnt), _Alignof(TraceStrEnt));
    if (tab == NULL)
        return false;
    for (Int i = 0; i < t->cap; i++) {
        if (t->tab[i].id == 0)
            continue;
        Int j = (Int)(t->tab[i].hash & (uint64_t)(ncap - 1));
        while (tab[j].id != 0)
            j = (j + 1) & (ncap - 1);
        tab[j] = t->tab[i];
    }
    if (t->tab != NULL)
        mem_free(heap_allocator(), t->tab, (size_t)t->cap * sizeof(TraceStrEnt),
                 _Alignof(TraceStrEnt));
    t->tab = tab;
    t->cap = ncap;
    return true;
}

/* Go's traceStringTable.put: the id of s in this generation, writing it out
 * the first time. Zero, which means the empty string, when s is empty or there
 * is no memory for it. */
static uint64_t string_put(uint64_t gen, const void *s, Int n) {
    if (n <= 0)
        return 0;
    if (n > MAX_STRING_SIZE)
        n = MAX_STRING_SIZE;
    TraceStrings *t = &trace.strings[gen % 2];
    uint64_t h = hash_bytes(s, (size_t)n);

    burrow__lock(&t->lock);
    if (t->n * 2 >= t->cap && !strings_grow(t)) {
        burrow__unlock(&t->lock);
        return 0;
    }
    Int j = (Int)(h & (uint64_t)(t->cap - 1));
    for (; t->tab[j].id != 0; j = (j + 1) & (t->cap - 1)) {
        TraceStrEnt *e = &t->tab[j];
        if (e->hash == h && e->len == n && memcmp(e->bytes, s, (size_t)n) == 0) {
            uint64_t id = e->id;
            burrow__unlock(&t->lock);
            return id;
        }
    }
    uint8_t *bytes = mem_alloc_nozero(heap_allocator(), (size_t)n, 1);
    if (bytes == NULL) {
        burrow__unlock(&t->lock);
        return 0;
    }
    memcpy(bytes, s, (size_t)n);
    uint64_t id = ++t->last;
    t->tab[j] = (TraceStrEnt){h, id, n, bytes};
    t->n++;

    Writer w = {&t->buf, gen, NO_THREAD};
    if (writer_ensure(&w, 1 + 2 * BYTES_PER_NUMBER + n + 1))
        buf_byte(t->buf, EV_STRINGS);
    buf_byte(t->buf, EV_STRING);
    buf_varint(t->buf, id);
    buf_varint(t->buf, (uint64_t)n);
    memcpy(&t->buf->arr[t->buf->pos], s, (size_t)n);
    t->buf->pos += n;
    burrow__unlock(&t->lock);
    return id;
}

static uint64_t string_put_c(uint64_t gen, const char *s) {
    return string_put(gen, s, (Int)strlen(s));
}

/* Go's traceStringTable.reset: the generation is over, so its strings go out
 * and its ids start again. */
static void strings_reset(uint64_t gen) {
    TraceStrings *t = &trace.strings[gen % 2];
    burrow__lock(&t->lock);
    Writer w = {&t->buf, gen, NO_THREAD};
    writer_flush(&w);
    for (Int i = 0; i < t->cap; i++)
        if (t->tab[i].id != 0)
            mem_free(heap_allocator(), t->tab[i].bytes, (size_t)t->tab[i].len, 1);
    if (t->tab != NULL)
        mem_free(heap_allocator(), t->tab, (size_t)t->cap * sizeof(TraceStrEnt),
                 _Alignof(TraceStrEnt));
    t->tab = NULL;
    t->cap = 0;
    t->n = 0;
    t->last = 0;
    burrow__unlock(&t->lock);
}

/* Go's traceRegisterLabelsAndReasons, the reasons part. */
static void register_reasons(uint64_t gen) {
    for (int i = 0; i < NBLOCK; i++)
        trace.block_ids[gen % 2][i] = string_put_c(gen, block_reason_strings[i]);
    for (int i = 0; i < NSTOP; i++)
        trace.stop_ids[gen % 2][i] = string_put_c(gen, stop_reason_strings[i]);
}

/* -------------------------------------------------------------------- stacks */

static bool stacks_grow(TraceStacks *t) {
    Int ncap = t->cap == 0 ? 64 : t->cap * 2;
    TraceStackEnt *tab = mem_alloc_array(
        heap_allocator(), (size_t)ncap, sizeof(TraceStackEnt), _Alignof(TraceStackEnt));
    if (tab == NULL)
        return false;
    for (Int i = 0; i < t->cap; i++) {
        if (t->tab[i].id == 0)
            continue;
        Int j = (Int)(t->tab[i].hash & (uint64_t)(ncap - 1));
        while (tab[j].id != 0)
            j = (j + 1) & (ncap - 1);
        tab[j] = t->tab[i];
    }
    if (t->tab != NULL)
        mem_free(heap_allocator(), t->tab, (size_t)t->cap * sizeof(TraceStackEnt),
                 _Alignof(TraceStackEnt));
    t->tab = tab;
    t->cap = ncap;
    return true;
}

/* Go's traceStackTable.put: the id of a stack of return addresses in this
 * generation. Zero, the empty stack, when there are none. */
static uint64_t stack_put(uint64_t gen, const Uintptr *pcs, Int n) {
    if (n <= 0)
        return 0;
    TraceStacks *t = &trace.stacks[gen % 2];
    size_t bytes = (size_t)n * sizeof(Uintptr);
    uint64_t h = hash_bytes(pcs, bytes);

    burrow__lock(&t->lock);
    if (t->n * 2 >= t->cap && !stacks_grow(t)) {
        burrow__unlock(&t->lock);
        return 0;
    }
    Int j = (Int)(h & (uint64_t)(t->cap - 1));
    for (; t->tab[j].id != 0; j = (j + 1) & (t->cap - 1)) {
        TraceStackEnt *e = &t->tab[j];
        if (e->hash == h && e->n == n && memcmp(e->pcs, pcs, bytes) == 0) {
            uint64_t id = e->id;
            burrow__unlock(&t->lock);
            return id;
        }
    }
    Uintptr *copy = mem_alloc_nozero(heap_allocator(), bytes, _Alignof(Uintptr));
    if (copy == NULL) {
        burrow__unlock(&t->lock);
        return 0;
    }
    memcpy(copy, pcs, bytes);
    uint64_t id = ++t->last;
    t->tab[j] = (TraceStackEnt){h, id, n, copy};
    t->n++;
    burrow__unlock(&t->lock);
    return id;
}

/* Go's traceStackTable.dump: writes every stack of the generation out and
 * forgets them. Each frame is a pc and the name of the function it is in.
 * burrow's symbol table has no file names or line numbers, so those are left
 * empty, which the format allows. */
static void stacks_dump(uint64_t gen) {
    TraceStacks *t = &trace.stacks[gen % 2];
    TraceBuf *buf = NULL;
    Writer w = {&buf, gen, NO_THREAD};

    burrow__lock(&t->lock);
    for (Int i = 0; i < t->cap; i++) {
        TraceStackEnt *e = &t->tab[i];
        if (e->id == 0)
            continue;
        uint64_t funcs[BURROW_PROFILE_DEPTH];
        Int n = e->n < BURROW_PROFILE_DEPTH ? e->n : BURROW_PROFILE_DEPTH;
        for (Int k = 0; k < n; k++) {
            RuntimeFunc fn;
            funcs[k] = e->pcs[k] != 0 && runtime_func_for_pc(e->pcs[k] - 1, &fn)
                           ? string_put(gen, fn.name.p, fn.name.len)
                           : 0;
        }
        if (writer_ensure(&w, 1 + 2 * BYTES_PER_NUMBER + n * 4 * BYTES_PER_NUMBER + 1))
            buf_byte(buf, EV_STACKS);
        buf_byte(buf, EV_STACK);
        buf_varint(buf, e->id);
        buf_varint(buf, (uint64_t)n);
        for (Int k = 0; k < n; k++) {
            buf_varint(buf, (uint64_t)(e->pcs[k] - 1));
            buf_varint(buf, funcs[k]);
            buf_varint(buf, 0);
            buf_varint(buf, 0);
        }
        mem_free(heap_allocator(), e->pcs, (size_t)e->n * sizeof(Uintptr),
                 _Alignof(Uintptr));
    }
    if (t->tab != NULL)
        mem_free(heap_allocator(), t->tab, (size_t)t->cap * sizeof(TraceStackEnt),
                 _Alignof(TraceStackEnt));
    t->tab = NULL;
    t->cap = 0;
    t->n = 0;
    t->last = 0;
    burrow__unlock(&t->lock);
    writer_flush(&w);
}

/* ------------------------------------------------------------------- threads */

static burrow__TraceThread *tt_get(void) {
    burrow__lock(&trace.tt_lock);
    burrow__TraceThread *tt = trace.tt_free;
    if (tt != NULL)
        trace.tt_free = tt->free;
    burrow__unlock(&trace.tt_lock);
    if (tt != NULL) {
        tt->free = NULL;
        return tt;
    }

    tt = mem_alloc(heap_allocator(), sizeof *tt, _Alignof(burrow__TraceThread));
    if (tt == NULL)
        return NULL;
    burrow__lock(&trace.tt_lock);
    tt->id = ++trace.next_mid;
    tt->all = trace.tts;
    trace.tts = tt;
    burrow__unlock(&trace.tt_lock);
    return tt;
}

bool burrow__trace_acquire_slow(burrow__TraceLocker *tl) {
    burrow__M *m = burrow__curm();
    burrow__TraceThread *tt;
    if (m != NULL) {
        tt = m->trace;
        if (tt == NULL) {
            tt = tt_get();
            if (tt == NULL)
                return false;
            m->trace = tt;
        }
        if (tt->depth++ == 0) {
            burrow__lock(&tt->lock);
            tt->gen = burrow__atomic_load_acquire_u64(&burrow__trace_gen);
        }
    } else {
        tt = &trace.foreign;
        burrow__lock(&tt->lock);
        tt->depth = 1;
        tt->gen = burrow__atomic_load_acquire_u64(&burrow__trace_gen);
    }

    tl->tt = tt;
    tl->m = m;
    tl->gen = tt->gen;
    if (tl->gen == 0) {
        burrow__trace_release(tl);
        return false;
    }
    return true;
}

void burrow__trace_release(burrow__TraceLocker *tl) {
    burrow__TraceThread *tt = tl->tt;
    if (--tt->depth == 0)
        burrow__unlock(&tt->lock);
}

BURROW_NOINLINE void burrow__trace_capture(int why) {
    if (!burrow__trace_enabled())
        return;
    burrow__M *m = burrow__curm();
    if (m == NULL)
        return;
    if (m->trace == NULL && (m->trace = tt_get()) == NULL)
        return;
    burrow__TraceThread *tt = m->trace;
    tt->why = why;
    tt->ncap = burrow__callers(BURROW_WALK_FROM, 1, tt->cap, BURROW_PROFILE_DEPTH);
}

/* The stack of whatever is calling into the scheduler, when that is a
 * goroutine, from the caller of the scheduler function that wrote the event.
 * On the scheduler's own stack there is nothing worth showing. */
static BURROW_NOINLINE uint64_t here_stack(burrow__TraceLocker *tl) {
    if (tl->m == NULL || tl->m->curg == NULL)
        return 0;
    Uintptr pcs[BURROW_PROFILE_DEPTH];
    Int n = burrow__callers(BURROW_WALK_FROM, 2, pcs, BURROW_PROFILE_DEPTH);
    return stack_put(tl->gen, pcs, n);
}

/* The stack burrow__trace_capture saw, and why, used once. */
static uint64_t captured_stack(burrow__TraceLocker *tl, int *why) {
    burrow__TraceThread *tt = tl->tt;
    *why = tt->why;
    uint64_t id = stack_put(tl->gen, tt->cap, tt->ncap);
    tt->why = 0;
    tt->ncap = 0;
    return id;
}

/* -------------------------------------------------------------------- events */

static Writer thread_writer(burrow__TraceLocker *tl) {
    return (Writer){&tl->tt->buf[tl->gen % 2], tl->gen, tl->tt->id};
}

/* Go's traceLocker.eventWriter: before an event, the status of this thread's P
 * and of the goroutine the event is about, if this generation has not had
 * them. */
static Writer event_writer(burrow__TraceLocker *tl, burrow__G *gp, int gostatus,
                           int procstatus) {
    Writer w = thread_writer(tl);
    burrow__P *p = tl->m != NULL ? tl->m->p : NULL;
    if (p != NULL && acquire_status(trace.ptraced[p->id], trace.pseq[p->id], tl->gen))
        EVENT(&w, EV_PROC_STATUS, (uint64_t)p->id, (uint64_t)procstatus);
    if (gp != NULL && acquire_status(gp->trace.traced, gp->trace.seq, tl->gen))
        EVENT(&w, EV_GO_STATUS, goid(gp), tl->tt->id, (uint64_t)gostatus);
    return w;
}

/* Go's traceLocker.emitUnblockStatus. */
static void unblock_status(burrow__TraceLocker *tl, burrow__G *gp) {
    if (acquire_status(gp->trace.traced, gp->trace.seq, tl->gen)) {
        Writer w = thread_writer(tl);
        EVENT(&w, EV_GO_STATUS, goid(gp), NO_THREAD, (uint64_t)GO_WAITING);
    }
}

static uint64_t start_pc_stack(burrow__TraceLocker *tl, burrow__G *g) {
    Uintptr pc = (Uintptr)burrow__atomic_load_acquire_ptr(&g->startpc);
    if (pc == 0)
        return 0;
    pc++;
    return stack_put(tl->gen, &pc, 1);
}

static void go_create(burrow__TraceLocker *tl, burrow__G *creator, burrow__G *newg,
                      bool blocked, uint64_t stack) {
    burrow__atomic_store_release_u32(&newg->trace.traced[tl->gen % 3], 1);
    Writer w = event_writer(tl, creator, GO_RUNNING, PROC_RUNNING);
    EVENT(&w, blocked ? EV_GO_CREATE_BLOCKED : EV_GO_CREATE, goid(newg),
          start_pc_stack(tl, newg), stack);
}

void burrow__trace_go_create(burrow__TraceLocker *tl, burrow__G *newg, bool blocked) {
    go_create(tl, tl->m != NULL ? tl->m->curg : NULL, newg, blocked, here_stack(tl));
}

void burrow__trace_go_start(burrow__TraceLocker *tl, burrow__G *gp) {
    if (burrow__atomic_load_acquire_u32(&gp->trace.pending) != 0) {
        burrow__atomic_store_release_u32(&gp->trace.pending, 0);
        go_create(tl, NULL, gp, false, 0);
    }
    Writer w = event_writer(tl, gp, GO_RUNNABLE, PROC_RUNNING);
    EVENT(&w, EV_GO_START, goid(gp), next_seq(gp->trace.seq, tl->gen));
}

void burrow__trace_go_destroy(burrow__TraceLocker *tl, burrow__G *gp) {
    Writer w = event_writer(tl, gp, GO_RUNNING, PROC_RUNNING);
    writer_event(&w, EV_GO_DESTROY, NULL, 0);
}

void burrow__trace_go_stop(burrow__TraceLocker *tl, burrow__G *gp) {
    int why;
    uint64_t stack = captured_stack(tl, &why);
    int reason = why == BURROW_TRACE_GOSCHED     ? STOP_GOSCHED
                 : why == BURROW_TRACE_PREEMPTED ? STOP_PREEMPTED
                                                 : STOP_UNSPECIFIED;
    Writer w = event_writer(tl, gp, GO_RUNNING, PROC_RUNNING);
    EVENT(&w, EV_GO_STOP, trace.stop_ids[tl->gen % 2][reason], stack);
}

/* Go's traceBlockReason for each of burrow's wait reasons, which are the
 * reasons Go's callers of gopark pass with them. */
static int block_reason(uint8_t wr) {
    switch ((burrow__WaitReason)wr) {
    case BURROW_WAIT_IO:
        return BLOCK_NET;
    case BURROW_WAIT_CHAN_RECEIVE_NIL_CHAN:
    case BURROW_WAIT_CHAN_SEND_NIL_CHAN:
    case BURROW_WAIT_SELECT_NO_CASES:
        return BLOCK_FOREVER;
    case BURROW_WAIT_SELECT:
    case BURROW_WAIT_SYNCTEST_SELECT:
        return BLOCK_SELECT;
    case BURROW_WAIT_CHAN_RECEIVE:
    case BURROW_WAIT_SYNCTEST_CHAN_RECEIVE:
        return BLOCK_CHAN_RECV;
    case BURROW_WAIT_CHAN_SEND:
    case BURROW_WAIT_SYNCTEST_CHAN_SEND:
        return BLOCK_CHAN_SEND;
    case BURROW_WAIT_SEMACQUIRE:
    case BURROW_WAIT_SYNC_MUTEX_LOCK:
    case BURROW_WAIT_SYNC_RW_MUTEX_R_LOCK:
    case BURROW_WAIT_SYNC_RW_MUTEX_LOCK:
    case BURROW_WAIT_SYNC_WAIT_GROUP_WAIT:
    case BURROW_WAIT_SYNCTEST_WAIT_GROUP_WAIT:
        return BLOCK_SYNC;
    case BURROW_WAIT_SYNC_COND_WAIT:
        return BLOCK_COND_WAIT;
    case BURROW_WAIT_SLEEP:
        return BLOCK_SLEEP;
    case BURROW_WAIT_SYNCTEST_RUN:
    case BURROW_WAIT_SYNCTEST_WAIT:
        return BLOCK_SYNCTEST;
    case BURROW_WAIT_ZERO:
    case BURROW_WAIT_COROUTINE:
    default:
        return BLOCK_UNSPECIFIED;
    }
}

void burrow__trace_go_park(burrow__TraceLocker *tl, burrow__G *gp) {
    int why;
    uint64_t stack = captured_stack(tl, &why);
    Writer w = event_writer(tl, gp, GO_RUNNING, PROC_RUNNING);
    EVENT(&w, EV_GO_BLOCK, trace.block_ids[tl->gen % 2][block_reason(gp->waitreason)],
          stack);
}

void burrow__trace_go_unpark(burrow__TraceLocker *tl, burrow__G *gp) {
    uint64_t stack = here_stack(tl);
    unblock_status(tl, gp);
    Writer w =
        event_writer(tl, tl->m != NULL ? tl->m->curg : NULL, GO_RUNNING, PROC_RUNNING);
    EVENT(&w, EV_GO_UNBLOCK, goid(gp), next_seq(gp->trace.seq, tl->gen), stack);
}

void burrow__trace_go_switch(burrow__TraceLocker *tl, burrow__G *gp, burrow__G *next) {
    unblock_status(tl, next);
    Writer w = event_writer(tl, gp, GO_RUNNING, PROC_RUNNING);
    EVENT(&w, EV_GO_SWITCH, goid(next), next_seq(next->trace.seq, tl->gen));
}

void burrow__trace_proc_start(burrow__TraceLocker *tl) {
    burrow__P *p = tl->m->p;
    Writer w = event_writer(tl, NULL, 0, PROC_IDLE);
    EVENT(&w, EV_PROC_START, (uint64_t)p->id, next_seq(trace.pseq[p->id], tl->gen));
}

void burrow__trace_proc_stop(burrow__TraceLocker *tl, burrow__P *p) {
    (void)p;
    Writer w = event_writer(tl, NULL, 0, PROC_RUNNING);
    writer_event(&w, EV_PROC_STOP, NULL, 0);
}

/* Go's traceLocker.Gomaxprocs. Its event needs a goroutine to belong to, so a
 * thread that is not running one leaves it out. */
static void procs_change(void) {
    burrow__M *m = burrow__curm();
    if (m == NULL || m->curg == NULL || m->p == NULL)
        return;
    burrow__TraceLocker tl;
    if (!burrow__trace_acquire(&tl))
        return;
    uint64_t stack = here_stack(&tl);
    Writer w = event_writer(&tl, m->curg, GO_RUNNING, PROC_RUNNING);
    EVENT(&w, EV_PROCS_CHANGE, (uint64_t)burrow__gomaxprocs(), stack);
    burrow__trace_release(&tl);
}

void burrow__trace_g_reset(burrow__G *g, bool has_p) {
    burrow__atomic_store_relaxed_u64(&g->trace.seq[0], 0);
    burrow__atomic_store_relaxed_u64(&g->trace.seq[1], 0);
    burrow__atomic_store_release_u32(&g->trace.pending,
                                     !has_p && burrow__trace_enabled());
}

void burrow__trace_m_exit(burrow__M *m) {
    if (m->p != NULL) {
        burrow__TraceLocker tl;
        if (burrow__trace_acquire(&tl)) {
            burrow__trace_proc_stop(&tl, m->p);
            burrow__trace_release(&tl);
        }
    }
    burrow__TraceThread *tt = m->trace;
    if (tt == NULL)
        return;
    m->trace = NULL;
    burrow__lock(&trace.tt_lock);
    tt->free = trace.tt_free;
    trace.tt_free = tt;
    burrow__unlock(&trace.tt_lock);
}

/* --------------------------------------------------------------- generations */

/* Go's traceSyncBatch: the first batch of every generation, which says how
 * fast the clock runs and what time it was. */
static void sync_batch(uint64_t gen) {
    TraceBuf *buf = NULL;
    Writer w = {&buf, gen, NO_THREAD};
    (void)writer_ensure(&w, 3 + 5 * BYTES_PER_NUMBER);
    buf_byte(buf, EV_SYNC);
    buf_byte(buf, EV_FREQUENCY);
    buf_varint(buf, (uint64_t)(1000000000 / TIME_DIV));

    int64_t mono = burrow__nanotime();
    int64_t real = pal_clock_realtime();
    uint64_t ts = trace_now();
    if (ts <= buf->last_time)
        ts = buf->last_time + 1;
    uint64_t diff = ts - buf->last_time;
    buf->last_time = ts;
    buf_byte(buf, EV_CLOCK_SNAPSHOT);
    buf_varint(buf, diff);
    buf_varint(buf, (uint64_t)mono);
    buf_varint(buf, (uint64_t)(real / 1000000000));
    buf_varint(buf, (uint64_t)(real % 1000000000));
    writer_flush(&w);
}

typedef struct Untraced {
    burrow__G *g;
    uint64_t goid;
    uint32_t status;
} Untraced;

typedef struct UntracedList {
    uint64_t gen;
    Untraced *v;
    Int n;
    Int cap;
} UntracedList;

/* For each goroutine, with the scheduler lock held: gets it ready for the next
 * generation, and notes its status if this one has not had it. */
static void walk_one(burrow__G *g, void *arg) {
    UntracedList *l = (UntracedList *)arg;
    ready_next_gen(g->trace.traced, g->trace.seq, l->gen);

    uint32_t st = burrow__atomic_load_acquire_u32(&g->status) & ~(uint32_t)BURROW_GSCAN;
    if (st == (uint32_t)BURROW_GIDLE || st == (uint32_t)BURROW_GDEAD)
        return;
    if (burrow__atomic_load_acquire_u32(&g->trace.pending) != 0)
        return;
    if (status_traced(g->trace.traced, l->gen))
        return;

    uint32_t status = st == (uint32_t)BURROW_GRUNNABLE  ? GO_RUNNABLE
                      : st == (uint32_t)BURROW_GWAITING ? GO_WAITING
                                                        : GO_RUNNING;
    if (l->n == l->cap) {
        Int ncap = l->cap == 0 ? 64 : l->cap * 2;
        Untraced *v = mem_alloc_array(heap_allocator(), (size_t)ncap, sizeof(Untraced),
                                      _Alignof(Untraced));
        if (v == NULL)
            return;
        if (l->v != NULL) {
            memcpy(v, l->v, (size_t)l->n * sizeof(Untraced));
            mem_free(heap_allocator(), l->v, (size_t)l->cap * sizeof(Untraced),
                     _Alignof(Untraced));
        }
        l->v = v;
        l->cap = ncap;
    }
    l->v[l->n++] = (Untraced){g, goid(g), status};
}

/* Every buffer every thread has for gen, handed in. Taking each thread's lock
 * is what waits for a thread that is in the middle of writing to it. */
static void flush_threads(uint64_t gen) {
    burrow__lock(&trace.tt_lock);
    burrow__TraceThread *head = trace.tts;
    burrow__unlock(&trace.tt_lock);

    for (burrow__TraceThread *tt = head;; tt = tt->all) {
        if (tt == NULL)
            tt = &trace.foreign;
        burrow__lock(&tt->lock);
        TraceBuf *b = tt->buf[gen % 2];
        tt->buf[gen % 2] = NULL;
        burrow__unlock(&tt->lock);
        if (b != NULL) {
            burrow__lock(&trace.lock);
            buf_flush(b, gen);
            burrow__unlock(&trace.lock);
        }
        if (tt == &trace.foreign)
            break;
    }
}

/* Go's traceAdvance. adv_lock held. */
static void advance_locked(bool stop) {
    uint64_t gen = burrow__atomic_load_acquire_u64(&burrow__trace_gen);
    if (gen == 0)
        return;

    UntracedList untraced = {gen, NULL, 0, 0};
    if (trace.run_active)
        burrow__sched_walk_gs(walk_one, &untraced);

    if (!stop)
        register_reasons(gen + 1);

    for (int i = 0; i < BURROW_MAXPROCS; i++)
        ready_next_gen(trace.ptraced[i], trace.pseq[i], gen);

    trace.last_gen = gen;
    if (stop) {
        burrow__lock(&trace.lock);
        trace.shutdown = true;
        burrow__atomic_store_release_u64(&burrow__trace_gen, 0);
        burrow__unlock(&trace.lock);
    } else {
        burrow__atomic_store_release_u64(&burrow__trace_gen, gen + 1);
        trace.gen_start = burrow__nanotime();
        sync_batch(gen + 1);
        procs_change();
    }

    flush_threads(gen);

    TraceBuf *buf = NULL;
    Writer w = {&buf, gen, NO_THREAD};
    for (Int i = 0; i < untraced.n; i++) {
        Untraced *u = &untraced.v[i];
        if (status_traced(u->g->trace.traced, gen))
            continue;
        EVENT(&w, EV_GO_STATUS, u->goid, NO_THREAD, (uint64_t)u->status);
    }
    writer_flush(&w);
    if (untraced.v != NULL)
        mem_free(heap_allocator(), untraced.v, (size_t)untraced.cap * sizeof(Untraced),
                 _Alignof(Untraced));

    stacks_dump(gen);
    strings_reset(gen);

    burrow__lock(&trace.lock);
    trace.flushed_gen = gen;
    burrow__unlock(&trace.lock);
}

void burrow__trace_advance(bool stop) {
    burrow__lock(&trace.adv_lock);
    advance_locked(stop);
    burrow__unlock(&trace.adv_lock);
}

static void free_list(TraceBuf *b) {
    while (b != NULL) {
        TraceBuf *next = b->link;
        mem_free(heap_allocator(), b, sizeof(TraceBuf), _Alignof(TraceBuf));
        b = next;
    }
}

/* Lets go of every buffer. Nothing may be tracing. */
static void drop_buffers(void) {
    burrow__lock(&trace.tt_lock);
    burrow__TraceThread *head = trace.tts;
    burrow__unlock(&trace.tt_lock);
    for (burrow__TraceThread *tt = head;; tt = tt->all) {
        if (tt == NULL)
            tt = &trace.foreign;
        burrow__lock(&tt->lock);
        for (int i = 0; i < 2; i++) {
            free_list(tt->buf[i]);
            tt->buf[i] = NULL;
        }
        burrow__unlock(&tt->lock);
        if (tt == &trace.foreign)
            break;
    }
    burrow__lock(&trace.lock);
    for (int i = 0; i < 2; i++) {
        free_list(trace.full_head[i]);
        trace.full_head[i] = NULL;
        trace.full_tail[i] = NULL;
    }
    free_list(trace.reading);
    trace.reading = NULL;
    free_list(trace.empty);
    trace.empty = NULL;
    burrow__unlock(&trace.lock);
}

/* A trace started inside a run that is now over. Its reader and whoever would
 * have stopped it were goroutines of that run, so there is nobody left to hand
 * the rest to. ctl and adv_lock held. */
static void abandon_locked(void) {
    uint64_t gen = burrow__atomic_load_acquire_u64(&burrow__trace_gen);
    if (gen != 0) {
        for (int i = 0; i < BURROW_MAXPROCS; i++)
            ready_next_gen(trace.ptraced[i], trace.pseq[i], gen);
        trace.last_gen = gen;
        burrow__atomic_store_release_u64(&burrow__trace_gen, 0);
    }
    /* Only the current generation's tables have anything in them, since the
     * last advance emptied the one before. Writing the stacks out is the
     * simplest way to let go of them, and the buffers go with the rest. */
    stacks_dump(gen);
    strings_reset(gen);
    drop_buffers();

    burrow__lock(&trace.lock);
    trace.session = false;
    trace.shutdown = true;
    burrow__unlock(&trace.lock);
    trace.state = STATE_OFF;
}

void burrow__trace_run_start(void) {
    burrow__lock(&trace.adv_lock);
    trace.run_base = trace.next_base;
    trace.run_active = true;
    burrow__unlock(&trace.adv_lock);
}

void burrow__trace_run_end(uint64_t ngoid) {
    burrow__lock(&trace.ctl);
    burrow__lock(&trace.adv_lock);
    trace.run_active = false;
    trace.next_base = trace.run_base + ngoid;
    if (trace.state != STATE_OFF && trace.in_run)
        abandon_locked();
    burrow__unlock(&trace.adv_lock);
    burrow__unlock(&trace.ctl);
}

/* ---------------------------------------------------------------- start/stop */

BURROW_SENTINEL_ERROR(trace_err_enabled, "tracing is already enabled");

/* Not in Go, where there is always a runtime to trace. */
BURROW_SENTINEL_ERROR(trace_err_no_runtime,
                      "tracing needs runtime_main on a platform with one thread");

Error runtime_start_trace(void) {
    burrow__lock(&trace.ctl);
    if (trace.state != STATE_OFF) {
        burrow__unlock(&trace.ctl);
        return trace_err_enabled;
    }
    bool in_run = burrow__sched_running();
#if defined(__wasi__)
    if (!in_run) {
        burrow__unlock(&trace.ctl);
        return trace_err_no_runtime;
    }
#else
    (void)trace_err_no_runtime;
#endif

    burrow__lock(&trace.adv_lock);
    uint64_t first = trace.last_gen + 1;

    burrow__lock(&trace.tt_lock);
    if (trace.foreign.id == 0)
        trace.foreign.id = ++trace.next_mid;
    burrow__unlock(&trace.tt_lock);

    burrow__lock(&trace.lock);
    trace.header_written = false;
    trace.eog_written = false;
    trace.shutdown = false;
    trace.reader_gen = first;
    trace.flushed_gen = 0;
    trace.session = true;
    burrow__unlock(&trace.lock);

    register_reasons(first);
    trace.in_run = in_run;
    trace.gen_start = burrow__nanotime();
    burrow__atomic_store_release_u64(&burrow__trace_gen, first);
    sync_batch(first);
    procs_change();
    burrow__unlock(&trace.adv_lock);

    trace.state = STATE_ON;
    burrow__unlock(&trace.ctl);
    return BURROW_NO_ERROR;
}

void runtime_stop_trace(void) {
    burrow__lock(&trace.ctl);
    if (trace.state != STATE_ON) {
        burrow__unlock(&trace.ctl);
        return;
    }
    trace.state = STATE_STOPPING;
    burrow__unlock(&trace.ctl);

    burrow__trace_advance(true);

    /* Go's StopTrace returns once the reader has had everything, and so does
     * this, by watching for the reader to get to the end. */
    for (;;) {
        burrow__lock(&trace.lock);
        bool done = !trace.session;
        burrow__unlock(&trace.lock);
        if (done)
            break;
        time_sleep(POLL);
    }

    burrow__lock(&trace.lock);
    free_list(trace.empty);
    trace.empty = NULL;
    burrow__unlock(&trace.lock);

    burrow__lock(&trace.ctl);
    if (trace.state == STATE_STOPPING)
        trace.state = STATE_OFF;
    burrow__unlock(&trace.ctl);
}

/* ------------------------------------------------------------------- reading */

/* Go's readTrace0. trace.lock held, and given up before it answers. Sets
 * *park when there is nothing to read yet. */
static Bytes read_locked(bool *park) {
    *park = false;
    if (trace.reading != NULL) {
        trace.reading->link = trace.empty;
        trace.empty = trace.reading;
        trace.reading = NULL;
    }
    if (!trace.session) {
        burrow__unlock(&trace.lock);
        return (Bytes){NULL, 0, 0, TYPE_BYTE};
    }
    if (!trace.header_written) {
        trace.header_written = true;
        burrow__unlock(&trace.lock);
        return (Bytes){(void *)(uintptr_t)trace_header, 16, 16, TYPE_BYTE};
    }

    for (;;) {
        uint64_t gen = trace.reader_gen;
        TraceBuf *b = trace.full_head[gen % 2];
        if (b != NULL) {
            trace.full_head[gen % 2] = b->link;
            if (b->link == NULL)
                trace.full_tail[gen % 2] = NULL;
            b->link = NULL;
            trace.reading = b;
            burrow__unlock(&trace.lock);
            return (Bytes){b->arr, b->pos, b->pos, TYPE_BYTE};
        }
        if (trace.flushed_gen == gen) {
            if (!trace.eog_written) {
                trace.eog_written = true;
                burrow__unlock(&trace.lock);
                return (Bytes){(void *)(uintptr_t)trace_end_of_generation, 1, 1,
                               TYPE_BYTE};
            }
            trace.eog_written = false;
            if (trace.shutdown) {
                trace.session = false;
                burrow__unlock(&trace.lock);
                return (Bytes){NULL, 0, 0, TYPE_BYTE};
            }
            trace.reader_gen = burrow__atomic_load_acquire_u64(&burrow__trace_gen);
            continue;
        }
        *park = true;
        burrow__unlock(&trace.lock);
        return (Bytes){NULL, 0, 0, TYPE_BYTE};
    }
}

Bytes runtime_read_trace(void) {
    for (;;) {
        /* The reader ends each generation once its second is up, and only
         * once it has read everything before it, so that the buffers the next
         * generation fills are ones it is finished with. */
        uint64_t gen = burrow__atomic_load_acquire_u64(&burrow__trace_gen);
        if (gen != 0 && burrow__trylock(&trace.adv_lock)) {
            burrow__lock(&trace.lock);
            bool caught_up = trace.reader_gen == gen;
            burrow__unlock(&trace.lock);
            if (caught_up && burrow__nanotime() - trace.gen_start >= GEN_NS)
                advance_locked(false);
            burrow__unlock(&trace.adv_lock);
        }

        burrow__lock(&trace.lock);
        bool park;
        Bytes b = read_locked(&park);
        if (!park)
            return b;
        time_sleep(POLL);
    }
}
