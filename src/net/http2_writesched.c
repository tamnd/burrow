/* net/http/internal/http2's writesched.go, writesched_roundrobin.go and
 * writesched_priority_rfc9218.go: the order the server writes frames in.
 *
 * Copyright 2014 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "http2.h"

#include "burrow/chan.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/map.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/panic.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------- FrameWriteRequest */

uint32_t burrow__http2_frame_write_request_stream_id(const Http2FrameWriteRequest *wr) {
    if (wr->stream == NULL) {
        /* resetStream has no stream to give, as there may not be one, so a
         * StreamError says which. */
        if (wr->write.kind == HTTP2_WRITE_STREAM_ERROR)
            return wr->write.u.stream_error.stream_id;
        return 0;
    }
    return wr->stream->id;
}

bool burrow__http2_frame_write_request_is_control(const Http2FrameWriteRequest *wr) {
    return wr->stream == NULL;
}

Int burrow__http2_frame_write_request_data_size(const Http2FrameWriteRequest *wr) {
    if (wr->write.kind == HTTP2_WRITE_DATA)
        return wr->write.u.data.p.len;
    return 0;
}

Int burrow__http2_frame_write_request_consume(const Http2FrameWriteRequest *wr,
                                              int32_t n,
                                              Http2FrameWriteRequest *consumed,
                                              Http2FrameWriteRequest *rest) {
    memset(consumed, 0, sizeof *consumed);
    memset(rest, 0, sizeof *rest);

    /* Frames other than DATA are always taken whole. */
    const Http2WriteData *wd = &wr->write.u.data;
    if (wr->write.kind != HTTP2_WRITE_DATA || wd->p.len == 0) {
        *consumed = *wr;
        return 1;
    }

    /* It may need splitting to keep to the limits. */
    int32_t allowed = burrow__http2_outflow_available(&wr->stream->flow);
    if (n < allowed)
        allowed = n;
    if (wr->stream->sc->max_frame_size < allowed)
        allowed = wr->stream->sc->max_frame_size;
    if (allowed <= 0)
        return 0;
    if (wd->p.len > (Int)allowed) {
        burrow__http2_outflow_take(&wr->stream->flow, allowed);
        /* There are bytes left over, so this part does not end the stream,
         * and the writer waits for the last part, not this one. */
        consumed->write = burrow__http2_write_data(wd->stream_id,
                                                   slice_sub(wd->p, 0, allowed), false);
        consumed->stream = wr->stream;
        consumed->done = NULL;
        rest->write = burrow__http2_write_data(
            wd->stream_id, slice_sub(wd->p, allowed, wd->p.len), wd->end_stream);
        rest->stream = wr->stream;
        rest->done = wr->done;
        return 2;
    }

    /* The whole frame fits. allowed is at most INT32_MAX, so this fits. */
    burrow__http2_outflow_take(&wr->stream->flow, (int32_t)wd->p.len);
    *consumed = *wr;
    return 1;
}

/* What Go's %T says for each writer. */
static const char *const hws_type_names[] = {
    "<nil>",
    "http2.flushFrameWriter",
    "http2.writeSettings",
    "*http2.writeGoAway",
    "*http2.writeData",
    "http2.handlerPanicRST",
    "http2.StreamError",
    "http2.writePing",
    "http2.writePingAck",
    "http2.writeSettingsAck",
    "*http2.writeResHeaders",
    "*http2.writePushPromise",
    "http2.write100ContinueHeadersFrame",
    "http2.writeWindowUpdate",
};

void burrow__http2_frame_write_request_reply_to_writer(Http2FrameWriteRequest *wr,
                                                       Error err) {
    if (wr->done == NULL)
        return;
    if (!chan_try_send(wr->done, &err)) {
        const char *t = "?";
        if ((size_t)wr->write.kind < sizeof hws_type_names / sizeof hws_type_names[0])
            t = hws_type_names[wr->write.kind];
        panic_str(fmt_sprintf_v(error_allocator(),
                                "unbuffered done channel passed in for type %s", t));
    }
    /* Not to be used again, now that done has been told. */
    memset(&wr->write, 0, sizeof wr->write);
}

/* -------------------------------------------------------------- writeQueue
 *
 * The frames for one stream, in a queue made of two stages, curr[curr_pos:]
 * and next, like the two lists of Okasaki's functional queue without having
 * to reverse one. prev and next_q link the queues into a ring. */

typedef struct HwsQueue {
    Http2FrameWriteRequest *curr;
    Int curr_len;
    Int curr_cap;
    Int curr_pos;
    Http2FrameWriteRequest *nextq;
    Int next_len;
    Int next_cap;
    struct HwsQueue *prev;
    struct HwsQueue *next;
    /* The RFC 9218 scheduler's streamMetadata.priority. */
    Http2PriorityParam priority;
} HwsQueue;

static bool hws_queue_empty(const HwsQueue *q) {
    return q->curr_len - q->curr_pos + q->next_len == 0;
}

static bool hws_queue_push(Alloc *a, HwsQueue *q, Http2FrameWriteRequest wr) {
    if (q->next_len == q->next_cap) {
        Int ncap = q->next_cap == 0 ? 4 : q->next_cap * 2;
        Http2FrameWriteRequest *p =
            mem_realloc(a, q->nextq, (size_t)q->next_cap * sizeof *p,
                        (size_t)ncap * sizeof *p, _Alignof(Http2FrameWriteRequest));
        if (p == NULL)
            return false;
        q->nextq = p;
        q->next_cap = ncap;
    }
    q->nextq[q->next_len++] = wr;
    return true;
}

static Http2FrameWriteRequest hws_queue_shift(HwsQueue *q) {
    if (hws_queue_empty(q))
        panic_str(BURROW_S("invalid use of queue"));
    if (q->curr_pos >= q->curr_len) {
        Http2FrameWriteRequest *old = q->curr;
        Int old_cap = q->curr_cap;
        q->curr = q->nextq;
        q->curr_len = q->next_len;
        q->curr_cap = q->next_cap;
        q->curr_pos = 0;
        q->nextq = old;
        q->next_len = 0;
        q->next_cap = old_cap;
    }
    Http2FrameWriteRequest wr = q->curr[q->curr_pos];
    memset(&q->curr[q->curr_pos], 0, sizeof wr);
    q->curr_pos++;
    return wr;
}

static Http2FrameWriteRequest *hws_queue_peek(HwsQueue *q) {
    if (q->curr_pos < q->curr_len)
        return &q->curr[q->curr_pos];
    if (q->next_len > 0)
        return &q->nextq[0];
    return NULL;
}

/* consume takes up to n bytes from the first frame, which comes off the
 * queue when it is used up and stays with the rest when it is not. False
 * when no bytes were taken. */
static bool hws_queue_consume(HwsQueue *q, int32_t n, Http2FrameWriteRequest *out) {
    memset(out, 0, sizeof *out);
    if (hws_queue_empty(q))
        return false;
    Http2FrameWriteRequest *first = hws_queue_peek(q);
    Http2FrameWriteRequest consumed;
    Http2FrameWriteRequest rest;
    switch (burrow__http2_frame_write_request_consume(first, n, &consumed, &rest)) {
    case 0:
        return false;
    case 1:
        (void)hws_queue_shift(q);
        break;
    default:
        *first = rest;
        break;
    }
    *out = consumed;
    return true;
}

static void hws_queue_free_arrays(Alloc *a, HwsQueue *q) {
    mem_free(a, q->curr, (size_t)q->curr_cap * sizeof *q->curr,
             _Alignof(Http2FrameWriteRequest));
    mem_free(a, q->nextq, (size_t)q->next_cap * sizeof *q->nextq,
             _Alignof(Http2FrameWriteRequest));
    q->curr = NULL;
    q->curr_cap = 0;
    q->nextq = NULL;
    q->next_cap = 0;
}

/* writeQueuePool: queues to use again. */
typedef struct HwsPool {
    HwsQueue **p;
    Int len;
    Int cap;
} HwsPool;

static void hws_queue_destroy(Alloc *a, HwsQueue *q) {
    hws_queue_free_arrays(a, q);
    mem_free(a, q, sizeof *q, _Alignof(HwsQueue));
}

static void hws_pool_put(Alloc *a, HwsPool *pool, HwsQueue *q) {
    if (q->curr_len > 0)
        memset(q->curr, 0, (size_t)q->curr_len * sizeof *q->curr);
    if (q->next_len > 0)
        memset(q->nextq, 0, (size_t)q->next_len * sizeof *q->nextq);
    q->curr_len = 0;
    q->next_len = 0;
    q->curr_pos = 0;
    if (pool->len == pool->cap) {
        Int ncap = pool->cap == 0 ? 4 : pool->cap * 2;
        HwsQueue **p = mem_realloc(a, pool->p, (size_t)pool->cap * sizeof *p,
                                   (size_t)ncap * sizeof *p, _Alignof(HwsQueue *));
        if (p == NULL) {
            /* Not kept, then. */
            hws_queue_destroy(a, q);
            return;
        }
        pool->p = p;
        pool->cap = ncap;
    }
    pool->p[pool->len++] = q;
}

static HwsQueue *hws_pool_get(Alloc *a, HwsPool *pool) {
    if (pool->len == 0)
        return mem_alloc(a, sizeof(HwsQueue), _Alignof(HwsQueue));
    HwsQueue *q = pool->p[--pool->len];
    pool->p[pool->len] = NULL;
    return q;
}

static void hws_pool_free(Alloc *a, HwsPool *pool) {
    for (Int i = 0; i < pool->len; i++)
        hws_queue_destroy(a, pool->p[i]);
    mem_free(a, pool->p, (size_t)pool->cap * sizeof *pool->p, _Alignof(HwsQueue *));
    memset(pool, 0, sizeof *pool);
}

/* ------------------------------------------------- what the two share */

static HwsQueue *hws_lookup(Map *streams, uint32_t id) {
    HwsQueue **q = (HwsQueue **)map_get(streams, &id);
    return q == NULL ? NULL : *q;
}

static void hws_panic_opened(uint32_t id) {
    panic_str(fmt_sprintf_v(error_allocator(), "stream %d already opened", id));
}

/* Puts q at the end of the ring at *head, which is just before *head. */
static void hws_ring_insert(HwsQueue **head, HwsQueue *q) {
    if (*head == NULL) {
        *head = q;
        q->next = q;
        q->prev = q;
        return;
    }
    q->prev = (*head)->prev;
    q->next = *head;
    q->prev->next = q;
    q->next->prev = q;
}

static void hws_ring_remove(HwsQueue **head, HwsQueue *q) {
    if (q->next == q) {
        /* The only one there was. */
        *head = NULL;
        return;
    }
    q->prev->next = q->next;
    q->next->prev = q->prev;
    if (*head == q)
        *head = q->next;
}

/* Opens id with a queue from the pool, or says false and changes nothing. */
static HwsQueue *hws_open(Alloc *a, Map *streams, HwsPool *pool, uint32_t id) {
    if (hws_lookup(streams, id) != NULL)
        hws_panic_opened(id);
    HwsQueue *q = hws_pool_get(a, pool);
    if (q == NULL)
        return NULL;
    if (!map_set(streams, &id, &q)) {
        hws_pool_put(a, pool, q);
        return NULL;
    }
    return q;
}

/* Push for both. A frame on a stream that is not open goes with the control
 * frames, and has to be no HEADERS or DATA. */
static bool hws_push(Alloc *a, HwsQueue *control, Map *streams,
                     Http2FrameWriteRequest wr) {
    if (burrow__http2_frame_write_request_is_control(&wr))
        return hws_queue_push(a, control, wr);
    HwsQueue *q = hws_lookup(streams, burrow__http2_frame_write_request_stream_id(&wr));
    if (q == NULL) {
        if (burrow__http2_frame_write_request_data_size(&wr) > 0)
            panic_str(BURROW_S("add DATA on non-open stream"));
        return hws_queue_push(a, control, wr);
    }
    return hws_queue_push(a, q, wr);
}

static void hws_free_streams(Alloc *a, Map *streams) {
    MapIter it = map_iter(streams);
    const void *k;
    void *v;
    while (map_next(&it, &k, &v))
        hws_queue_destroy(a, *(HwsQueue **)v);
    map_free(streams);
}

static Map *hws_make_streams(Alloc *a) {
    return map_make(a, TYPE_UINT32, TYPE_UNSAFE_POINTER, 0);
}

/* ------------------------------------------------------------ round robin */

typedef struct HwsRoundRobin {
    Alloc *a;
    /* SETTINGS, PING and the like. */
    HwsQueue control;
    /* Stream ID to queue. */
    Map *streams;
    /* The ring of stream queues, at the next one to write, or NULL when no
     * stream is open. */
    HwsQueue *head;
    HwsPool pool;
} HwsRoundRobin;

static bool hws_rr_open_stream(void *self, uint32_t id, Http2OpenStreamOptions opts) {
    (void)opts;
    HwsRoundRobin *ws = (HwsRoundRobin *)self;
    HwsQueue *q = hws_open(ws->a, ws->streams, &ws->pool, id);
    if (q == NULL)
        return false;
    hws_ring_insert(&ws->head, q);
    return true;
}

static void hws_rr_close_stream(void *self, uint32_t id) {
    HwsRoundRobin *ws = (HwsRoundRobin *)self;
    HwsQueue *q = hws_lookup(ws->streams, id);
    if (q == NULL)
        return;
    hws_ring_remove(&ws->head, q);
    map_del(ws->streams, &id);
    hws_pool_put(ws->a, &ws->pool, q);
}

static void hws_rr_adjust_stream(void *self, uint32_t id, Http2PriorityParam p) {
    (void)self;
    (void)id;
    (void)p;
}

static bool hws_rr_push(void *self, Http2FrameWriteRequest wr) {
    HwsRoundRobin *ws = (HwsRoundRobin *)self;
    return hws_push(ws->a, &ws->control, ws->streams, wr);
}

static bool hws_rr_pop(void *self, Http2FrameWriteRequest *wr) {
    HwsRoundRobin *ws = (HwsRoundRobin *)self;
    /* Control and RST_STREAM frames first. */
    if (!hws_queue_empty(&ws->control)) {
        *wr = hws_queue_shift(&ws->control);
        return true;
    }
    memset(wr, 0, sizeof *wr);
    if (ws->head == NULL)
        return false;
    HwsQueue *q = ws->head;
    for (;;) {
        if (hws_queue_consume(q, INT32_MAX, wr)) {
            ws->head = q->next;
            return true;
        }
        q = q->next;
        if (q == ws->head)
            break;
    }
    return false;
}

static void hws_rr_free(void *self) {
    HwsRoundRobin *ws = (HwsRoundRobin *)self;
    Alloc *a = ws->a;
    hws_free_streams(a, ws->streams);
    hws_pool_free(a, &ws->pool);
    hws_queue_free_arrays(a, &ws->control);
    mem_free(a, ws, sizeof *ws, _Alignof(HwsRoundRobin));
}

static const Http2WriteSchedulerVT hws_rr_vt = {
    hws_rr_open_stream, hws_rr_close_stream, hws_rr_adjust_stream,
    hws_rr_push,        hws_rr_pop,          hws_rr_free,
};

Http2WriteScheduler burrow__http2_new_round_robin_write_scheduler(Alloc *a) {
    Http2WriteScheduler none = {NULL, NULL};
    if (a == NULL)
        a = heap_allocator();
    HwsRoundRobin *ws = mem_alloc(a, sizeof *ws, _Alignof(HwsRoundRobin));
    if (ws == NULL)
        return none;
    ws->a = a;
    ws->streams = hws_make_streams(a);
    if (ws->streams == NULL) {
        mem_free(a, ws, sizeof *ws, _Alignof(HwsRoundRobin));
        return none;
    }
    Http2WriteScheduler s = {&hws_rr_vt, ws};
    return s;
}

/* --------------------------------------------------------------- RFC 9218 */

enum { HWS_URGENCIES = 8 };

typedef struct HwsRFC9218 {
    Alloc *a;
    /* SETTINGS, PING and the like. */
    HwsQueue control;
    /* The rings of streams, by urgency from u=0 to u=7, and then by
     * incremental, false and true. RFC 9218 section 4. */
    HwsQueue *heads[HWS_URGENCIES][2];
    /* Stream ID to queue, and the queue has the stream's priority. */
    Map *streams;
    HwsPool pool;
    /* Whether incremental streams go first among those of the same urgency
     * in this Pop. */
    bool prioritize_incremental;
    /* The latest PRIORITY_UPDATE for a stream that is not open yet. A
     * stream_id of 0 is none, as a PRIORITY_UPDATE for stream 0 is a
     * PROTOCOL_ERROR. RFC 9218 section 7. */
    uint32_t update_stream_id;
    Http2PriorityParam update_priority;
} HwsRFC9218;

/* The ring for p, which Go finds by indexing, and so panics as Go would for a
 * priority out of range. */
static HwsQueue **hws_pr_head(HwsRFC9218 *ws, Http2PriorityParam p) {
    if (p.urgency >= HWS_URGENCIES || p.incremental > 1)
        panic_str(BURROW_S("runtime error: index out of range"));
    return &ws->heads[p.urgency][p.incremental];
}

static bool hws_pr_open_stream(void *self, uint32_t id, Http2OpenStreamOptions opts) {
    HwsRFC9218 *ws = (HwsRFC9218 *)self;
    if (hws_lookup(ws->streams, id) != NULL)
        hws_panic_opened(id);
    Http2PriorityParam priority = opts.priority;
    bool buffered = id == ws->update_stream_id;
    if (buffered)
        priority = ws->update_priority;
    HwsQueue **head = hws_pr_head(ws, priority);
    HwsQueue *q = hws_open(ws->a, ws->streams, &ws->pool, id);
    if (q == NULL)
        return false;
    if (buffered)
        ws->update_stream_id = 0;
    q->priority = priority;
    hws_ring_insert(head, q);
    return true;
}

static void hws_pr_close_stream(void *self, uint32_t id) {
    HwsRFC9218 *ws = (HwsRFC9218 *)self;
    HwsQueue *q = hws_lookup(ws->streams, id);
    if (q == NULL)
        return;
    hws_ring_remove(hws_pr_head(ws, q->priority), q);
    map_del(ws->streams, &id);
    hws_pool_put(ws->a, &ws->pool, q);
}

static void hws_pr_adjust_stream(void *self, uint32_t id, Http2PriorityParam p) {
    HwsRFC9218 *ws = (HwsRFC9218 *)self;
    HwsQueue *q = hws_lookup(ws->streams, id);
    if (q == NULL) {
        ws->update_stream_id = id;
        ws->update_priority = p;
        return;
    }
    HwsQueue **to = hws_pr_head(ws, p);
    /* Out of the ring it was in and into the one for p. */
    hws_ring_remove(hws_pr_head(ws, q->priority), q);
    hws_ring_insert(to, q);
    q->priority = p;
}

static bool hws_pr_push(void *self, Http2FrameWriteRequest wr) {
    HwsRFC9218 *ws = (HwsRFC9218 *)self;
    return hws_push(ws->a, &ws->control, ws->streams, wr);
}

static bool hws_pr_pop(void *self, Http2FrameWriteRequest *wr) {
    HwsRFC9218 *ws = (HwsRFC9218 *)self;
    /* Control and RST_STREAM frames first. */
    if (!hws_queue_empty(&ws->control)) {
        *wr = hws_queue_shift(&ws->control);
        return true;
    }
    memset(wr, 0, sizeof *wr);

    /* Turn about between incremental and not among streams of the same
     * urgency, so that the incremental ones get half the bandwidth between
     * them and the first one that is not gets the other half, as streams that
     * are not incremental are not written in turn. */
    ws->prioritize_incremental = !ws->prioritize_incremental;

    /* The lowest u, which is the most urgent, first. */
    for (int u = 0; u < HWS_URGENCIES; u++) {
        for (int k = 0; k < 2; k++) {
            int i = ws->prioritize_incremental ? (k + 1) % 2 : k;
            HwsQueue *q = ws->heads[u][i];
            if (q == NULL)
                continue;
            for (;;) {
                if (hws_queue_consume(q, INT32_MAX, wr)) {
                    /* Incremental streams are written in turn, as each can
                     * use a part straight away. One that is not is written
                     * to the end before the next, but head still moves to
                     * it, so that a stream with nothing to write does not
                     * hold up the ones behind it next time. */
                    ws->heads[u][i] = i == 1 ? q->next : q;
                    return true;
                }
                q = q->next;
                if (q == ws->heads[u][i])
                    break;
            }
        }
    }
    return false;
}

static void hws_pr_free(void *self) {
    HwsRFC9218 *ws = (HwsRFC9218 *)self;
    Alloc *a = ws->a;
    hws_free_streams(a, ws->streams);
    hws_pool_free(a, &ws->pool);
    hws_queue_free_arrays(a, &ws->control);
    mem_free(a, ws, sizeof *ws, _Alignof(HwsRFC9218));
}

static const Http2WriteSchedulerVT hws_pr_vt = {
    hws_pr_open_stream, hws_pr_close_stream, hws_pr_adjust_stream,
    hws_pr_push,        hws_pr_pop,          hws_pr_free,
};

Http2WriteScheduler burrow__http2_new_priority_write_scheduler_rfc9218(Alloc *a) {
    Http2WriteScheduler none = {NULL, NULL};
    if (a == NULL)
        a = heap_allocator();
    HwsRFC9218 *ws = mem_alloc(a, sizeof *ws, _Alignof(HwsRFC9218));
    if (ws == NULL)
        return none;
    ws->a = a;
    ws->streams = hws_make_streams(a);
    if (ws->streams == NULL) {
        mem_free(a, ws, sizeof *ws, _Alignof(HwsRFC9218));
        return none;
    }
    Http2WriteScheduler s = {&hws_pr_vt, ws};
    return s;
}

bool burrow__http2_write_scheduler_is_rfc9218(Http2WriteScheduler ws) {
    return ws.vt == &hws_pr_vt;
}

/* --------------------------------------------------------------- methods */

bool burrow__http2_write_scheduler_open_stream(Http2WriteScheduler ws,
                                               uint32_t stream_id,
                                               Http2OpenStreamOptions opts) {
    return ws.vt->open_stream(ws.self, stream_id, opts);
}

void burrow__http2_write_scheduler_close_stream(Http2WriteScheduler ws,
                                                uint32_t stream_id) {
    ws.vt->close_stream(ws.self, stream_id);
}

void burrow__http2_write_scheduler_adjust_stream(Http2WriteScheduler ws,
                                                 uint32_t stream_id,
                                                 Http2PriorityParam p) {
    ws.vt->adjust_stream(ws.self, stream_id, p);
}

bool burrow__http2_write_scheduler_push(Http2WriteScheduler ws,
                                        Http2FrameWriteRequest wr) {
    return ws.vt->push(ws.self, wr);
}

bool burrow__http2_write_scheduler_pop(Http2WriteScheduler ws,
                                       Http2FrameWriteRequest *wr) {
    return ws.vt->pop(ws.self, wr);
}

void burrow__http2_write_scheduler_free(Http2WriteScheduler ws) {
    if (ws.vt != NULL)
        ws.vt->free(ws.self);
}
