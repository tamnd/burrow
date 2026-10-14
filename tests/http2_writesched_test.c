/* Derived from net/http/internal/http2's writesched_test.go,
 * writesched_roundrobin_test.go and writesched_priority_rfc9218_test.go in
 * Go 1.27.1.
 *
 * Go's checkConsume compares requests with reflect.DeepEqual, and here that
 * is the same writer kind, stream, done channel and, for DATA, the same bytes
 * and endStream. TestWriteResHeaders is not Go's. Go tests write.go through
 * the server, which is not here yet, and this one writes a response header
 * block and reads it back.
 *
 * Copyright 2016 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/net/http2.h"

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/mem/heap.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FATALF(...)                                                                    \
    do {                                                                               \
        testing_t_fatalf_v(t, __VA_ARGS__);                                            \
        return;                                                                        \
    } while (0)

/* What make([]byte, n) gives every DATA frame here, for n up to its size. */
static const Byte wt_zeros[128];

static Slice wt_bytes(Int n) {
    return slice_from((void *)(uintptr_t)wt_zeros, n, n, TYPE_BYTE);
}

static Http2FrameWriteRequest wt_request(Http2WriteFramer w, Http2Stream *st,
                                         Chan *done) {
    Http2FrameWriteRequest wr;
    memset(&wr, 0, sizeof wr);
    wr.write = w;
    wr.stream = st;
    wr.done = done;
    return wr;
}

static Http2Stream wt_stream(uint32_t id, Http2ServerConn *sc) {
    Http2Stream st;
    memset(&st, 0, sizeof st);
    st.id = id;
    st.sc = sc;
    return st;
}

static Http2ServerConn wt_conn(int32_t max_frame_size) {
    Http2ServerConn sc;
    memset(&sc, 0, sizeof sc);
    sc.max_frame_size = max_frame_size;
    return sc;
}

static Http2FrameWriteRequest make_write_non_stream_request(void) {
    return wt_request(burrow__http2_write_settings_ack(), NULL, NULL);
}

static Http2FrameWriteRequest make_write_rst_stream(uint32_t stream_id) {
    Http2StreamError se;
    memset(&se, 0, sizeof se);
    se.stream_id = stream_id;
    se.code = HTTP2_ERR_CODE_INTERNAL;
    return wt_request(burrow__http2_write_stream_error(se), NULL, NULL);
}

/* reflect.DeepEqual for two requests. */
static bool wt_equal(const Http2FrameWriteRequest *a, const Http2FrameWriteRequest *b) {
    if (a->write.kind != b->write.kind || a->stream != b->stream || a->done != b->done)
        return false;
    if (a->write.kind == HTTP2_WRITE_STREAM_ERROR) {
        const Http2StreamError *x = &a->write.u.stream_error;
        const Http2StreamError *y = &b->write.u.stream_error;
        return x->stream_id == y->stream_id && x->code == y->code;
    }
    /* The other kinds here carry nothing. */
    if (a->write.kind != HTTP2_WRITE_DATA)
        return true;
    const Http2WriteData *x = &a->write.u.data;
    const Http2WriteData *y = &b->write.u.data;
    return x->stream_id == y->stream_id && x->end_stream == y->end_stream &&
           x->p.len == y->p.len &&
           (x->p.len == 0 || memcmp(x->p.p, y->p.p, (size_t)x->p.len) == 0);
}

static Str wt_fmt(Alloc *a, const Http2FrameWriteRequest *wr) {
    if (wr->write.kind == HTTP2_WRITE_DATA)
        return fmt_sprintf_v(a,
                             "[FrameWriteRequest stream=%d, ch=%t, "
                             "writer=writeData(stream=%d, p=%d, endStream=%t)]",
                             burrow__http2_frame_write_request_stream_id(wr),
                             wr->done != NULL, wr->write.u.data.stream_id,
                             wr->write.u.data.p.len, wr->write.u.data.end_stream);
    return fmt_sprintf_v(a, "[FrameWriteRequest stream=%d, ch=%t, writer=kind %d]",
                         burrow__http2_frame_write_request_stream_id(wr),
                         wr->done != NULL, (int)wr->write.kind);
}

/* checkConsume. Says what was wrong, in a, or "" when nothing was. */
static Str check_consume(Alloc *a, Http2FrameWriteRequest wr, int32_t nbytes,
                         const Http2FrameWriteRequest *want, Int nwant) {
    Http2FrameWriteRequest consumed;
    Http2FrameWriteRequest rest;
    Int n = burrow__http2_frame_write_request_consume(&wr, nbytes, &consumed, &rest);
    Http2FrameWriteRequest want_consumed;
    Http2FrameWriteRequest want_rest;
    memset(&want_consumed, 0, sizeof want_consumed);
    memset(&want_rest, 0, sizeof want_rest);
    if (nwant >= 1)
        want_consumed = want[0];
    if (nwant == 2)
        want_rest = want[1];
    if (!wt_equal(&consumed, &want_consumed) || !wt_equal(&rest, &want_rest) ||
        n != nwant)
        return fmt_sprintf_v(a, "got %s, %s, %d\nwant %s, %s, %d", wt_fmt(a, &consumed),
                             wt_fmt(a, &rest), n, wt_fmt(a, &want_consumed),
                             wt_fmt(a, &want_rest), nwant);
    return BURROW_STR_EMPTY;
}

static void TestFrameWriteRequestNonData(TestingT *t) {
    Arena ar;
    memset(&ar, 0, sizeof ar);
    Alloc *a = arena_allocator(&ar);
    Http2FrameWriteRequest wr = make_write_non_stream_request();
    Int got = burrow__http2_frame_write_request_data_size(&wr);
    if (got != 0)
        testing_t_errorf_v(t, "DataSize: got %v, want %v", got, 0);

    /* Frames other than DATA are always taken whole. */
    Str e = check_consume(a, wr, 0, &wr, 1);
    if (e.len != 0)
        testing_t_errorf_v(t, "Consume:\n%s", e);

    wr = make_write_rst_stream(123);
    got = burrow__http2_frame_write_request_data_size(&wr);
    if (got != 0)
        testing_t_errorf_v(t, "DataSize: got %v, want %v", got, 0);

    /* RST_STREAM frames are always taken whole. */
    e = check_consume(a, wr, 0, &wr, 1);
    if (e.len != 0)
        testing_t_errorf_v(t, "Consume:\n%s", e);
    arena_free(&ar);
}

/* #49741 RST_STREAM and control frames should come before DATA frames, so
 * that a client that cannot drain the queue does not hold the streams up. */
static void TestFrameWriteRequestWithData(TestingT *t) {
    Arena ar;
    memset(&ar, 0, sizeof ar);
    Alloc *a = arena_allocator(&ar);
    Http2ServerConn sc = wt_conn(16);
    Http2Stream st = wt_stream(1, &sc);
    enum { size = 32 };
    Chan *done = chan_make(heap_allocator(), TYPE_ERROR, 0);
    Http2FrameWriteRequest wr =
        wt_request(burrow__http2_write_data(st.id, wt_bytes(size), true), &st, done);
    Int got = burrow__http2_frame_write_request_data_size(&wr);
    if (got != size)
        testing_t_errorf_v(t, "DataSize: got %v, want %v", got, (Int)size);

    /* No flow control bytes, so nothing can be taken. */
    Str e = check_consume(a, wr, INT32_MAX, NULL, 0);
    if (e.len != 0)
        testing_t_errorf_v(t, "Consume(limited by flow control):\n%s", e);

    wr = make_write_non_stream_request();
    got = burrow__http2_frame_write_request_data_size(&wr);
    if (got != 0)
        testing_t_errorf_v(t, "DataSize: got %v, want %v", got, 0);

    /* Frames other than DATA are always taken whole. */
    e = check_consume(a, wr, 0, &wr, 1);
    if (e.len != 0)
        testing_t_errorf_v(t, "Consume:\n%s", e);

    wr = make_write_rst_stream(1);
    got = burrow__http2_frame_write_request_data_size(&wr);
    if (got != 0)
        testing_t_errorf_v(t, "DataSize: got %v, want %v", got, 0);

    /* RST_STREAM frames are always taken whole. */
    e = check_consume(a, wr, 0, &wr, 1);
    if (e.len != 0)
        testing_t_errorf_v(t, "Consume:\n%s", e);
    chan_free(done);
    arena_free(&ar);
}

static void TestFrameWriteRequestData(TestingT *t) {
    Arena ar;
    memset(&ar, 0, sizeof ar);
    Alloc *a = arena_allocator(&ar);
    Http2ServerConn sc = wt_conn(16);
    Http2Stream st = wt_stream(1, &sc);
    enum { size = 32 };
    Chan *done = chan_make(heap_allocator(), TYPE_ERROR, 0);
    Http2FrameWriteRequest wr =
        wt_request(burrow__http2_write_data(st.id, wt_bytes(size), true), &st, done);
    Int got = burrow__http2_frame_write_request_data_size(&wr);
    if (got != size)
        testing_t_errorf_v(t, "DataSize: got %v, want %v", got, (Int)size);

    /* No flow control bytes, so nothing can be taken. */
    Str e = check_consume(a, wr, INT32_MAX, NULL, 0);
    if (e.len != 0)
        testing_t_errorf_v(t, "Consume(limited by flow control):\n%s", e);

    /* Enough flow control bytes for the whole frame, but now
     * st.sc.maxFrameSize is the limit. */
    (void)burrow__http2_outflow_add(&st.flow, size);
    Int mfs = sc.max_frame_size;
    Http2FrameWriteRequest want[2];
    want[0] =
        wt_request(burrow__http2_write_data(st.id, wt_bytes(mfs), false), &st, NULL);
    want[1] = wt_request(burrow__http2_write_data(st.id, wt_bytes(size - mfs), true),
                         &st, wr.done);
    e = check_consume(a, wr, INT32_MAX, want, 2);
    if (e.len != 0)
        testing_t_errorf_v(t, "Consume(limited by maxFrameSize):\n%s", e);
    Http2FrameWriteRequest rest = want[1];

    /* 8 bytes from what is left. */
    want[0] =
        wt_request(burrow__http2_write_data(st.id, wt_bytes(8), false), &st, NULL);
    want[1] = wt_request(
        burrow__http2_write_data(st.id, wt_bytes(size - mfs - 8), true), &st, wr.done);
    e = check_consume(a, rest, 8, want, 2);
    if (e.len != 0)
        testing_t_errorf_v(t, "Consume(8):\n%s", e);
    rest = want[1];

    /* All the rest. */
    want[0] = wt_request(
        burrow__http2_write_data(st.id, wt_bytes(size - mfs - 8), true), &st, wr.done);
    e = check_consume(a, rest, INT32_MAX, want, 1);
    if (e.len != 0)
        testing_t_errorf_v(t, "Consume(remainder):\n%s", e);
    chan_free(done);
    arena_free(&ar);
}

static void TestFrameWriteRequest_StreamID(TestingT *t) {
    enum { stream_id = 123 };
    Http2StreamError se;
    memset(&se, 0, sizeof se);
    se.stream_id = stream_id;
    se.code = HTTP2_ERR_CODE_NO;
    Http2FrameWriteRequest wr =
        wt_request(burrow__http2_write_stream_error(se), NULL, NULL);
    uint32_t got = burrow__http2_frame_write_request_stream_id(&wr);
    if (got != stream_id)
        testing_t_errorf_v(t, "FrameWriteRequest(StreamError) = %v; want %v", got,
                           (uint32_t)stream_id);
}

/* ------------------------------------------------------------- schedulers */

enum { WT_MAX_FRAME_SIZE = 16, WT_CONTROL_FRAMES = 2, WT_MAX_STREAMS = 6 };

typedef struct WtSched {
    Http2ServerConn sc;
    Http2Stream streams[WT_MAX_STREAMS];
    Http2WriteScheduler ws;
} WtSched;

static Http2PriorityParam wt_prio(uint8_t urgency, uint8_t incremental) {
    Http2PriorityParam p;
    memset(&p, 0, sizeof p);
    p.urgency = urgency;
    p.incremental = incremental;
    return p;
}

static Http2OpenStreamOptions wt_opts(Http2PriorityParam p) {
    Http2OpenStreamOptions o;
    memset(&o, 0, sizeof o);
    o.priority = p;
    return o;
}

/* Opens stream id as w->streams[i], with a large window, and queues frames
 * frames' worth of DATA on it. */
static bool wt_open_and_push(WtSched *w, Int i, uint32_t id, Http2PriorityParam p,
                             Int frames) {
    w->streams[i] = wt_stream(id, &w->sc);
    (void)burrow__http2_outflow_add(&w->streams[i].flow, 1 << 20); /* plenty */
    if (!burrow__http2_write_scheduler_open_stream(w->ws, id, wt_opts(p)))
        return false;
    Http2WriteFramer d =
        burrow__http2_write_data(id, wt_bytes(WT_MAX_FRAME_SIZE * frames), false);
    return burrow__http2_write_scheduler_push(w->ws,
                                              wt_request(d, &w->streams[i], NULL));
}

/* Queues the control frames, and then pops everything, which should be the
 * control frames and then frames of WT_MAX_FRAME_SIZE bytes, whose streams go
 * in got. */
static bool wt_drain(TestingT *t, WtSched *w, uint32_t *got, Int cap, Int *ngot) {
    *ngot = 0;
    for (int i = 0; i < WT_CONTROL_FRAMES; i++) {
        if (!burrow__http2_write_scheduler_push(w->ws,
                                                make_write_non_stream_request())) {
            testing_t_errorf_v(t, "Push: out of memory");
            return false;
        }
    }
    /* The control frames come first. */
    Http2FrameWriteRequest wr;
    for (int i = 0; i < WT_CONTROL_FRAMES; i++) {
        bool ok = burrow__http2_write_scheduler_pop(w->ws, &wr);
        uint32_t id = burrow__http2_frame_write_request_stream_id(&wr);
        if (!ok || id != 0) {
            testing_t_fatalf_v(t, "wr.Pop() = stream %v, %v; want 0, true", id, ok);
            return false;
        }
    }
    /* Each stream writes WT_MAX_FRAME_SIZE bytes at a time until it has
     * nothing left. */
    while (burrow__http2_write_scheduler_pop(w->ws, &wr)) {
        Int n = burrow__http2_frame_write_request_data_size(&wr);
        if (n != WT_MAX_FRAME_SIZE) {
            testing_t_fatalf_v(t, "wr.Pop() = %v data bytes, want %v", n,
                               (Int)WT_MAX_FRAME_SIZE);
            return false;
        }
        if (*ngot == cap) {
            testing_t_fatalf_v(t, "more than %d frames popped", cap);
            return false;
        }
        got[(*ngot)++] = burrow__http2_frame_write_request_stream_id(&wr);
    }
    return true;
}

/* A []uint32 as Go prints one, such as [1 2 3]. */
static Str wt_fmt_ids(char *buf, size_t size, const uint32_t *ids, Int n) {
    size_t off = 0;
    buf[off++] = '[';
    for (Int i = 0; i < n && off + 12 < size; i++)
        off += (size_t)snprintf(buf + off, size - off, i == 0 ? "%u" : " %u",
                                (unsigned)ids[i]);
    buf[off++] = ']';
    return str_from_bytes(buf, (Int)off);
}

static void wt_check_ids(TestingT *t, const uint32_t *got, Int ngot,
                         const uint32_t *want, Int nwant) {
    bool same = ngot == nwant;
    for (Int i = 0; same && i < ngot; i++)
        same = got[i] == want[i];
    if (same)
        return;
    char gb[256];
    char wb[256];
    testing_t_fatalf_v(t, "popped streams %s, want %s",
                       wt_fmt_ids(gb, sizeof gb, got, ngot),
                       wt_fmt_ids(wb, sizeof wb, want, nwant));
}

static bool wt_init(TestingT *t, WtSched *w, bool rfc9218) {
    memset(w, 0, sizeof *w);
    w->sc = wt_conn(WT_MAX_FRAME_SIZE);
    w->ws = rfc9218
                ? burrow__http2_new_priority_write_scheduler_rfc9218(heap_allocator())
                : burrow__http2_new_round_robin_write_scheduler(heap_allocator());
    if (w->ws.vt == NULL) {
        testing_t_fatalf_v(t, "making the scheduler: out of memory");
        return false;
    }
    return true;
}

static void TestRoundRobinScheduler(TestingT *t) {
    WtSched w;
    if (!wt_init(t, &w, false))
        return;
    for (Int i = 0; i < 4; i++) {
        uint32_t id = (uint32_t)i + 1;
        if (!wt_open_and_push(&w, i, id, wt_prio(0, 0), i + 1))
            FATALF("opening stream %d: out of memory", id);
    }
    /* Stream 1 has one frame of data, 2 has two, and so on. */
    static const uint32_t want[] = {1, 2, 3, 4, 2, 3, 4, 3, 4, 4};
    uint32_t got[32];
    Int ngot = 0;
    if (wt_drain(t, &w, got, sizeof got / sizeof got[0], &ngot))
        wt_check_ids(t, got, ngot, want, sizeof want / sizeof want[0]);
    burrow__http2_write_scheduler_free(w.ws);
}

static void TestPrioritySchedulerUrgency(TestingT *t) {
    WtSched w;
    if (!wt_init(t, &w, true))
        return;
    for (Int i = 0; i < 5; i++) {
        uint32_t id = (uint32_t)i + 1;
        if (!wt_open_and_push(&w, i, id, wt_prio(7, 0), i + 1))
            FATALF("opening stream %d: out of memory", id);
    }
    /* The even numbered streams get more urgent. */
    for (uint32_t id = 2; id <= 5; id += 2)
        burrow__http2_write_scheduler_adjust_stream(w.ws, id, wt_prio(0, 0));

    /* The more urgent even numbered streams come first. */
    static const uint32_t want[] = {2, 2, 4, 4, 4, 4, 1, 3, 3, 3, 5, 5, 5, 5, 5};
    uint32_t got[32];
    Int ngot = 0;
    if (wt_drain(t, &w, got, sizeof got / sizeof got[0], &ngot))
        wt_check_ids(t, got, ngot, want, sizeof want / sizeof want[0]);
    burrow__http2_write_scheduler_free(w.ws);
}

static void TestPrioritySchedulerIncremental(TestingT *t) {
    WtSched w;
    if (!wt_init(t, &w, true))
        return;
    for (Int i = 0; i < 5; i++) {
        uint32_t id = (uint32_t)i + 1;
        if (!wt_open_and_push(&w, i, id, wt_prio(7, 0), i + 1))
            FATALF("opening stream %d: out of memory", id);
    }
    /* The even numbered streams become incremental. */
    for (uint32_t id = 2; id <= 5; id += 2)
        burrow__http2_write_scheduler_adjust_stream(w.ws, id, wt_prio(7, 1));

    /* The writes should go:
     * - in turn between the even and odd numbered streams, which have the
     *   same u and a different i,
     * - in turn among the even numbered streams, which are incremental,
     * - and one stream at a time among the odd numbered ones, which are not. */
    static const uint32_t want[] = {2, 1, 4, 3, 2, 3, 4, 3, 4, 5, 4, 5, 5, 5, 5};
    uint32_t got[32];
    Int ngot = 0;
    if (wt_drain(t, &w, got, sizeof got / sizeof got[0], &ngot))
        wt_check_ids(t, got, ngot, want, sizeof want / sizeof want[0]);
    burrow__http2_write_scheduler_free(w.ws);
}

static void TestPrioritySchedulerUrgencyAndIncremental(TestingT *t) {
    WtSched w;
    if (!wt_init(t, &w, true))
        return;
    for (Int i = 0; i < 6; i++) {
        uint32_t id = (uint32_t)i + 1;
        if (!wt_open_and_push(&w, i, id, wt_prio(7, 0), i + 1))
            FATALF("opening stream %d: out of memory", id);
    }
    /* The even numbered streams become incremental and more urgent. */
    for (uint32_t id = 2; id <= 6; id += 2)
        burrow__http2_write_scheduler_adjust_stream(w.ws, id, wt_prio(0, 1));
    /* Close streams 1 and 4. */
    burrow__http2_write_scheduler_close_stream(w.ws, 1);
    burrow__http2_write_scheduler_close_stream(w.ws, 4);

    /* The writes should be:
     * - the even numbered streams first, in turn, as they are more urgent
     *   and incremental,
     * - then the odd numbered ones, one at a time to the end, as they are
     *   less urgent and not incremental,
     * - and none for streams 1 and 4, which are closed. */
    static const uint32_t want[] = {2, 6, 2, 6, 6, 6, 6, 6, 3, 3, 3, 5, 5, 5, 5, 5};
    uint32_t got[32];
    Int ngot = 0;
    if (wt_drain(t, &w, got, sizeof got / sizeof got[0], &ngot))
        wt_check_ids(t, got, ngot, want, sizeof want / sizeof want[0]);
    burrow__http2_write_scheduler_free(w.ws);
}

static void TestPrioritySchedulerIdempotentUpdate(TestingT *t) {
    WtSched w;
    if (!wt_init(t, &w, true))
        return;
    for (Int i = 0; i < 6; i++) {
        uint32_t id = (uint32_t)i + 1;
        if (!wt_open_and_push(&w, i, id, wt_prio(7, 0), i + 1))
            FATALF("opening stream %d: out of memory", id);
    }
    /* The even numbered streams become incremental and more urgent. */
    for (uint32_t id = 2; id <= 6; id += 2)
        burrow__http2_write_scheduler_adjust_stream(w.ws, id, wt_prio(0, 1));
    burrow__http2_write_scheduler_close_stream(w.ws, 1);
    /* The same update again, which should change nothing. */
    for (uint32_t id = 2; id <= 6; id += 2)
        burrow__http2_write_scheduler_adjust_stream(w.ws, id, wt_prio(0, 1));
    burrow__http2_write_scheduler_close_stream(w.ws, 2);

    /* The writes should be:
     * - the even numbered streams first, in turn, as they are more urgent
     *   and incremental,
     * - then the odd numbered ones, one at a time to the end, as they are
     *   less urgent and not incremental,
     * - and none for streams 1 and 2, which are closed. */
    static const uint32_t want[] = {4, 6, 4, 6, 4, 6, 4, 6, 6,
                                    6, 3, 3, 3, 5, 5, 5, 5, 5};
    uint32_t got[32];
    Int ngot = 0;
    if (wt_drain(t, &w, got, sizeof got / sizeof got[0], &ngot))
        wt_check_ids(t, got, ngot, want, sizeof want / sizeof want[0]);
    burrow__http2_write_scheduler_free(w.ws);
}

static void TestPrioritySchedulerBuffersPriorityUpdate(TestingT *t) {
    WtSched w;
    if (!wt_init(t, &w, true))
        return;

    /* Priorities change for streams that are not open yet. */
    burrow__http2_write_scheduler_adjust_stream(w.ws, 1, wt_prio(0, 0));
    burrow__http2_write_scheduler_adjust_stream(w.ws, 5, wt_prio(0, 0));
    static const uint32_t ids[] = {1, 3, 5};
    for (Int i = 0; i < 3; i++) {
        if (!wt_open_and_push(&w, i, ids[i], wt_prio(7, 1), 3))
            FATALF("opening stream %d: out of memory", ids[i]);
    }

    /* Only the latest update is kept and used, and the older ones are
     * forgotten. */
    static const uint32_t want[] = {5, 5, 5, 1, 3, 1, 3, 1, 3};
    uint32_t got[32];
    Int ngot = 0;
    if (wt_drain(t, &w, got, sizeof got / sizeof got[0], &ngot))
        wt_check_ids(t, got, ngot, want, sizeof want / sizeof want[0]);
    burrow__http2_write_scheduler_free(w.ws);
}

/* ------------------------------------------------------------- write.go */

/* A writeContext over a Framer writing to buf, with its own encoder. */
typedef struct WtCtx {
    BytesBuffer buf;
    BytesBuffer hbuf;
    Http2Framer *fr;
    HpackEncoder *enc;
} WtCtx;

static Http2Framer *wt_ctx_framer(void *self) {
    return ((WtCtx *)self)->fr;
}

static Error wt_ctx_flush(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static Error wt_ctx_close_conn(void *self) {
    (void)self;
    return BURROW_NO_ERROR;
}

static HpackEncoder *wt_ctx_header_encoder(void *self, BytesBuffer **buf) {
    WtCtx *c = (WtCtx *)self;
    *buf = &c->hbuf;
    return c->enc;
}

static const Http2WriteContextVT wt_ctx_vt = {wt_ctx_framer, wt_ctx_flush,
                                              wt_ctx_close_conn, wt_ctx_header_encoder};

/* A header block bigger than a frame, so that it needs a CONTINUATION. */
enum { WT_BIG = 20000 };

static void TestWriteResHeaders(TestingT *t) {
    Arena ar;
    memset(&ar, 0, sizeof ar);
    Alloc *a = arena_allocator(&ar);
    WtCtx c;
    memset(&c, 0, sizeof c);
    c.buf = BYTES_BUFFER(a);
    c.hbuf = BYTES_BUFFER(a);
    c.fr = burrow__http2_new_framer(a, bytes_buffer_as_io_writer(&c.buf),
                                    bytes_buffer_as_io_reader(&c.buf));
    c.enc = burrow__hpack_new_encoder(a, bytes_buffer_as_io_writer(&c.hbuf));
    HpackDecoder *dec =
        burrow__hpack_new_decoder(a, HTTP2_INITIAL_HEADER_TABLE_SIZE, NULL, NULL);
    HttpHeader h = http_header_make(a);
    Byte *big = mem_alloc(a, WT_BIG, 1);
    if (c.fr == NULL || c.enc == NULL || dec == NULL || h == NULL || big == NULL) {
        arena_free(&ar);
        FATALF("out of memory");
    }
    memset(big, 'x', WT_BIG);
    c.fr->read_meta_headers = dec;
    http_header_set(h, BURROW_S("Foo"), BURROW_S("bar"));
    http_header_add(h, BURROW_S("Foo"), BURROW_S("baz"));
    http_header_set(h, BURROW_S("Big"), str_from_bytes(big, WT_BIG));
    /* Only "trailers" is let through for Transfer-Encoding. */
    http_header_set(h, BURROW_S("Transfer-Encoding"), BURROW_S("chunked"));
    /* Bad values are left out. */
    http_header_set(h, BURROW_S("Bad"), BURROW_S("a\nb"));

    Http2WriteResHeaders rh;
    memset(&rh, 0, sizeof rh);
    rh.stream_id = 1;
    rh.http_res_code = 200;
    rh.h = h;
    rh.end_stream = true;
    rh.content_length = BURROW_S("0");
    Http2WriteFramer w = burrow__http2_write_res_headers(&rh);
    if (burrow__http2_write_stays_within_buffer(&w, 1 << 20))
        testing_t_errorf_v(t, "staysWithinBuffer = true; want false");
    if (!burrow__http2_write_ends_stream(&w))
        testing_t_errorf_v(t, "writeEndsStream = false; want true");
    Http2WriteContext ctx = {&wt_ctx_vt, &c};
    Error err = burrow__http2_write_frame(&w, ctx);
    if (BURROW_FAILED(err)) {
        arena_free(&ar);
        FATALF("writeFrame: %v", err);
    }

    Http2Frame *f = burrow__http2_framer_read_frame(c.fr, &err);
    if (BURROW_FAILED(err) || f == NULL) {
        arena_free(&ar);
        FATALF("ReadFrame: %v", err);
    }
    static const char *const want[][2] = {
        {":status", "200"}, {"big", NULL},           {"foo", "bar"},
        {"foo", "baz"},     {"content-length", "0"},
    };
    const HpackHeaderFields *fs = &f->u.meta_headers.fields;
    Int nwant = (Int)(sizeof want / sizeof want[0]);
    if (fs->len != nwant)
        testing_t_errorf_v(t, "got %d fields; want %d", fs->len, nwant);
    for (Int i = 0; i < fs->len && i < nwant; i++) {
        Str name = fs->p[i].name;
        Str value = fs->p[i].value;
        Str want_value = want[i][1] == NULL ? str_from_bytes(big, WT_BIG)
                                            : str_from_cstr(want[i][1]);
        if (!str_eq(name, str_from_cstr(want[i][0])) || !str_eq(value, want_value))
            testing_t_errorf_v(t, "field %d = %q (%d bytes); want %q (%d bytes)", i,
                               name, value.len, str_from_cstr(want[i][0]),
                               want_value.len);
    }
    if (!burrow__http2_headers_frame_stream_ended(f))
        testing_t_errorf_v(t, "END_STREAM not set");
    burrow__http2_frame_free(f);
    burrow__hpack_decoder_free(dec);
    burrow__hpack_encoder_free(c.enc);
    burrow__http2_framer_free(c.fr);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestFrameWriteRequestNonData)                                                    \
    X(TestFrameWriteRequestWithData)                                                   \
    X(TestFrameWriteRequestData)                                                       \
    X(TestFrameWriteRequest_StreamID)                                                  \
    X(TestRoundRobinScheduler)                                                         \
    X(TestPrioritySchedulerUrgency)                                                    \
    X(TestPrioritySchedulerIncremental)                                                \
    X(TestPrioritySchedulerUrgencyAndIncremental)                                      \
    X(TestPrioritySchedulerIdempotentUpdate)                                           \
    X(TestPrioritySchedulerBuffersPriorityUpdate)                                      \
    X(TestWriteResHeaders)
TESTING_MAIN(TESTS)
