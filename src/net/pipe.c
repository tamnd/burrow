/* Derived from Go's src/net/pipe.go.
 * Go source: go1.27.1.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/net.h"

#include "burrow/atomic.h"
#include "burrow/chan.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/os.h"
#include "burrow/runtime.h"
#include "burrow/slice.h"
#include "burrow/sync.h"
#include "burrow/time.h"
#include "burrow/timer.h"
#include "burrow/type.h"

#include <stdint.h>
#include <string.h>

/* A synchronous, in-memory, full duplex connection, ported with Go's channels.
 * A Write offers its slice to the other end's Read, which copies what fits and
 * says how much on a second channel. Each end has a done channel that its
 * Close closes, and each deadline is a channel that closes when the deadline
 * passes, so every blocking call is one select over the data channel, both
 * done channels and the deadline.
 *
 * Go leaves the rest to the collector, and three things here stand in for it.
 *
 * A deadline's channel is replaced when the deadline is set again after it
 * has passed, and a call may have picked up the old one and not reached its
 * select yet. So each one is counted, with a reference for the deadline that
 * holds it, one for each call that is about to wait on it, and one for an
 * armed timer that is going to close it, and the last one frees it.
 *
 * The timer's callback runs on a goroutine of its own and touches the pair, so
 * the pair is counted too, with one reference for its owner and one for each
 * armed timer. A stop that says true puts the timer's reference down there and
 * then, and otherwise the callback puts it down when it is done.
 *
 * And the owner gives the pair back with net_pipe_free, once nothing is using
 * either end. */

typedef struct NpPair NpPair;

typedef struct NpCancel {
    Chan *c; /* bool, never sent on, only closed */
    uint32_t refs;
} NpCancel;

/* Go's pipeDeadline. */
typedef struct NpDeadline {
    SyncMutex mu; /* guards timer and cancel */
    TimeTimer *timer;
    NpCancel *cancel; /* never NULL once the pipe is made */
    NpPair *pair;
} NpDeadline;

/* Go's pipe, one end. */
typedef struct NpEnd {
    SyncMutex wr_mu; /* serializes Write operations */

    /* Used by a local Read to interact with a remote Write. A successful
     * receive on rd_rx is always followed by a send on rd_tx. */
    Chan *rd_rx; /* Slice */
    Chan *rd_tx; /* Int */

    /* Used by a local Write to interact with a remote Read. A successful send
     * on wr_tx is always followed by a receive on wr_rx. */
    Chan *wr_tx; /* Slice */
    Chan *wr_rx; /* Int */

    uint32_t closed; /* protects closing local_done */
    Chan *local_done;
    Chan *remote_done;

    NpDeadline read_deadline;
    NpDeadline write_deadline;

    NpPair *pair;
} NpEnd;

struct NpPair {
    NpEnd end[2];
    Chan *chans[6];
    uint32_t refs;
    Alloc *a;
};

static bool np_is_closed(Chan *c) {
    bool ok = true;
    return chan_try_recv(c, NULL, &ok) && !ok;
}

static bool np_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

/* ------------------------------------------------------------- the counts */

static NpCancel *np_cancel_new(Alloc *a) {
    NpCancel *k = (NpCancel *)mem_alloc(a, sizeof(NpCancel), _Alignof(NpCancel));
    if (k == NULL)
        return NULL;
    k->c = chan_make(a, TYPE_BOOL, 0);
    if (k->c == NULL) {
        mem_free(a, k, sizeof(NpCancel), _Alignof(NpCancel));
        return NULL;
    }
    k->refs = 1;
    return k;
}

static void np_cancel_release(Alloc *a, NpCancel *k) {
    if (k == NULL || burrow__atomic_add_u32(&k->refs, (uint32_t)-1) != 1)
        return;
    chan_free(k->c);
    mem_free(a, k, sizeof(NpCancel), _Alignof(NpCancel));
}

/* Everything net_pipe made, whichever parts of it it got to. */
static void np_pair_destroy(NpPair *pair) {
    Alloc *a = pair->a;
    for (int i = 0; i < 2; i++) {
        np_cancel_release(a, pair->end[i].read_deadline.cancel);
        np_cancel_release(a, pair->end[i].write_deadline.cancel);
    }
    for (int i = 0; i < 6; i++) {
        if (pair->chans[i] != NULL)
            chan_free(pair->chans[i]);
    }
    mem_free(a, pair, sizeof(NpPair), _Alignof(NpPair));
}

static void np_pair_release(NpPair *pair) {
    if (burrow__atomic_add_u32(&pair->refs, (uint32_t)-1) == 1)
        np_pair_destroy(pair);
}

/* -------------------------------------------------------------- deadlines */

/* What the timer runs, on a goroutine of its own. set does not change the
 * channel under an armed timer without first stopping the timer or waiting
 * for this to close it, so reading it here without the lock is safe. */
static void np_deadline_fire(void *env) {
    NpDeadline *d = (NpDeadline *)env;
    NpPair *pair = d->pair;
    NpCancel *k = d->cancel;
    chan_close(k->c);
    np_cancel_release(pair->a, k);
    np_pair_release(pair);
}

/* Disarms the timer, if there is one, with d->mu held. A stop that says true
 * means the callback will never run, and its two references are put down
 * here. Otherwise it has run or is about to, and closing the channel is the
 * one thing left for it to do that matters, so this waits for that. */
static void np_deadline_disarm(NpDeadline *d) {
    if (d->timer == NULL)
        return;
    if (time_timer_stop(d->timer)) {
        np_cancel_release(d->pair->a, d->cancel);
        np_pair_release(d->pair);
    } else {
        chan_recv(d->cancel->c, NULL);
    }
    time_timer_free(d->timer);
    d->timer = NULL;
}

/* A fresh channel in place of one that has closed. */
static bool np_deadline_renew(NpDeadline *d) {
    NpCancel *k = np_cancel_new(d->pair->a);
    if (k == NULL)
        return false;
    np_cancel_release(d->pair->a, d->cancel);
    d->cancel = k;
    return true;
}

/* Go's pipeDeadline.set: the point in time when the deadline times out, which
 * is signalled by closing the channel np_deadline_wait gives. Once it has
 * timed out it can be refreshed with a t in the future, and the zero t means
 * no deadline. The one way it fails is a failed allocation, and then the
 * deadline is left as it was before or, if it had passed, still passed. */
static Error np_deadline_set(NpDeadline *d, Time t) {
    /* Even a time that needs no timer, so the call always works or always
     * throws, the way context_with_deadline does. */
    if (burrow__timers_local() == NULL)
        runtime_throw(
            BURROW_S("net: a pipe deadline needs a goroutine to put the timer on"));

    sync_mutex_lock(&d->mu);
    np_deadline_disarm(d);

    bool closed = np_is_closed(d->cancel->c);
    Error err = BURROW_NO_ERROR;

    /* The zero time means there is no deadline. */
    if (time_is_zero(t)) {
        if (closed && !np_deadline_renew(d))
            err = burrow_err_out_of_memory;
        sync_mutex_unlock(&d->mu);
        return err;
    }

    /* A time in the future, so set up a timer to close the channel then. */
    Duration dur = time_until(t);
    if (dur > 0) {
        if (closed && !np_deadline_renew(d)) {
            sync_mutex_unlock(&d->mu);
            return burrow_err_out_of_memory;
        }
        NpPair *pair = d->pair;
        (void)burrow__atomic_add_u32(&d->cancel->refs, 1);
        (void)burrow__atomic_add_u32(&pair->refs, 1);
        d->timer = time_after_func(pair->a, dur, BURROW_FN(Func, np_deadline_fire, d));
        if (d->timer == NULL) {
            (void)burrow__atomic_add_u32(&d->cancel->refs, (uint32_t)-1);
            (void)burrow__atomic_add_u32(&pair->refs, (uint32_t)-1);
            err = burrow_err_out_of_memory;
        }
        sync_mutex_unlock(&d->mu);
        return err;
    }

    /* A time in the past, so close it now. */
    if (!closed)
        chan_close(d->cancel->c);
    sync_mutex_unlock(&d->mu);
    return err;
}

/* Go's pipeDeadline.wait, with a reference the caller puts down once it has
 * finished waiting. */
static NpCancel *np_deadline_wait(NpDeadline *d) {
    sync_mutex_lock(&d->mu);
    NpCancel *k = d->cancel;
    (void)burrow__atomic_add_u32(&k->refs, 1);
    sync_mutex_unlock(&d->mu);
    return k;
}

/* ------------------------------------------------------------------ the end */

static Int np_read_raw(NpEnd *p, Slice b, Error *err) {
    if (np_is_closed(p->local_done)) {
        *err = io_err_closed_pipe;
        return 0;
    }
    if (np_is_closed(p->remote_done)) {
        *err = io_eof;
        return 0;
    }
    NpCancel *k = np_deadline_wait(&p->read_deadline);
    if (np_is_closed(k->c)) {
        np_cancel_release(p->pair->a, k);
        *err = os_err_deadline_exceeded;
        return 0;
    }

    Slice bw;
    SelectCase cases[4] = {
        BURROW_RECV(p->rd_rx, &bw),
        BURROW_RECV(p->local_done, NULL),
        BURROW_RECV(p->remote_done, NULL),
        BURROW_RECV(k->c, NULL),
    };
    Int i = chan_select(cases, 4);
    np_cancel_release(p->pair->a, k);

    switch (i) {
    case 0: {
        Int nr = bw.len < b.len ? bw.len : b.len;
        if (nr > 0)
            memmove(b.p, bw.p, (size_t)nr);
        chan_send(p->rd_tx, &nr);
        *err = BURROW_NO_ERROR;
        return nr;
    }
    case 1:
        *err = io_err_closed_pipe;
        return 0;
    case 2:
        *err = io_eof;
        return 0;
    default:
        *err = os_err_deadline_exceeded;
        return 0;
    }
}

static Int np_write_raw(NpEnd *p, Slice b, Error *err) {
    if (np_is_closed(p->local_done) || np_is_closed(p->remote_done)) {
        *err = io_err_closed_pipe;
        return 0;
    }
    NpCancel *k = np_deadline_wait(&p->write_deadline);
    bool passed = np_is_closed(k->c);
    np_cancel_release(p->pair->a, k);
    if (passed) {
        *err = os_err_deadline_exceeded;
        return 0;
    }

    sync_mutex_lock(&p->wr_mu); /* ensure the whole of b is written together */
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    for (bool once = true; once || b.len > 0; once = false) {
        k = np_deadline_wait(&p->write_deadline);
        SelectCase cases[4] = {
            BURROW_SEND(p->wr_tx, &b),
            BURROW_RECV(p->local_done, NULL),
            BURROW_RECV(p->remote_done, NULL),
            BURROW_RECV(k->c, NULL),
        };
        Int i = chan_select(cases, 4);
        np_cancel_release(p->pair->a, k);
        if (i == 0) {
            Int nw = 0;
            chan_recv(p->wr_rx, &nw);
            b = slice_sub(b, nw, b.len);
            n += nw;
            continue;
        }
        e = i == 3 ? os_err_deadline_exceeded : io_err_closed_pipe;
        break;
    }
    sync_mutex_unlock(&p->wr_mu);
    *err = e;
    return n;
}

/* The error a Read or a Write gives, which is err itself for the ones io
 * defines and otherwise an OpError saying which it was, in the calling
 * goroutine's error arena. */
static Error np_op_error(Str op, Error err) {
    NetOpError oe = {op, BURROW_S("pipe"), {NULL, NULL}, {NULL, NULL}, err};
    return net_op_error_as_error(&oe, error_allocator());
}

static Int np_read(void *self, Slice b, Error *err) {
    Error e;
    Int n = np_read_raw((NpEnd *)self, b, &e);
    if (BURROW_FAILED(e) && !np_same(e, io_eof) && !np_same(e, io_err_closed_pipe))
        e = np_op_error(BURROW_S("read"), e);
    BURROW_OUT(err, e);
    return n;
}

static Int np_write(void *self, Slice b, Error *err) {
    Error e;
    Int n = np_write_raw((NpEnd *)self, b, &e);
    if (BURROW_FAILED(e) && !np_same(e, io_err_closed_pipe))
        e = np_op_error(BURROW_S("write"), e);
    BURROW_OUT(err, e);
    return n;
}

static Error np_close(void *self) {
    NpEnd *p = (NpEnd *)self;
    uint32_t open = 0;
    if (burrow__atomic_cas_u32(&p->closed, &open, 1))
        chan_close(p->local_done);
    return BURROW_NO_ERROR;
}

/* Go's pipeAddr, whose network and text are both "pipe". */
typedef struct NpAddr {
    Byte unused;
} NpAddr;

static Str np_addr_network(void *self) {
    (void)self;
    return BURROW_S("pipe");
}

static Str np_addr_string(void *self, Alloc *a) {
    (void)self;
    (void)a;
    return BURROW_S("pipe");
}

static const Type np_addr_desc = {
    BURROW_S_INIT("pipeAddr"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NpAddr),
    (uint16_t)_Alignof(NpAddr),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e706164U, /* "npad" */
    NULL,
};

static const NetAddrVT np_addr_vt = {&np_addr_desc, np_addr_network, np_addr_string};

static const NpAddr np_addr_value = {0};

static NetAddr np_addr(void *self) {
    (void)self;
    NetAddr addr = {&np_addr_vt, (void *)(uintptr_t)&np_addr_value};
    return addr;
}

static bool np_either_closed(NpEnd *p) {
    return np_is_closed(p->local_done) || np_is_closed(p->remote_done);
}

static Error np_set_deadline(void *self, Time t) {
    NpEnd *p = (NpEnd *)self;
    if (np_either_closed(p))
        return io_err_closed_pipe;
    Error err = np_deadline_set(&p->read_deadline, t);
    if (BURROW_FAILED(err))
        return err;
    return np_deadline_set(&p->write_deadline, t);
}

static Error np_set_read_deadline(void *self, Time t) {
    NpEnd *p = (NpEnd *)self;
    if (np_either_closed(p))
        return io_err_closed_pipe;
    return np_deadline_set(&p->read_deadline, t);
}

static Error np_set_write_deadline(void *self, Time t) {
    NpEnd *p = (NpEnd *)self;
    if (np_either_closed(p))
        return io_err_closed_pipe;
    return np_deadline_set(&p->write_deadline, t);
}

static const Type np_end_desc = {
    BURROW_S_INIT("pipe"),
    BURROW_S_INIT("net"),
    KIND_STRUCT,
    (uint32_t)sizeof(NpEnd),
    (uint16_t)_Alignof(NpEnd),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e706970U, /* "npip" */
    NULL,
};

static const NetConnVT np_conn_vt = {
    {&np_end_desc, np_read},
    {&np_end_desc, np_write},
    {&np_end_desc, np_close},
    np_addr,
    np_addr,
    np_set_deadline,
    np_set_read_deadline,
    np_set_write_deadline,
};

/* --------------------------------------------------------------- the pair */

static void np_end_init(NpEnd *p, NpPair *pair, Chan *rd_rx, Chan *rd_tx, Chan *wr_tx,
                        Chan *wr_rx, Chan *local_done, Chan *remote_done) {
    p->rd_rx = rd_rx;
    p->rd_tx = rd_tx;
    p->wr_tx = wr_tx;
    p->wr_rx = wr_rx;
    p->local_done = local_done;
    p->remote_done = remote_done;
    p->read_deadline.pair = pair;
    p->write_deadline.pair = pair;
    p->pair = pair;
}

void net_pipe(Alloc *a, NetConn *c1, NetConn *c2) {
    NetConn none = {NULL, NULL};
    BURROW_OUT(c1, none);
    BURROW_OUT(c2, none);

    NpPair *pair = (NpPair *)mem_alloc(a, sizeof(NpPair), _Alignof(NpPair));
    if (pair == NULL)
        return;
    pair->a = a;
    pair->refs = 1;

    static const Type *const elems[6] = {TYPE_BYTES, TYPE_BYTES, TYPE_INT,
                                         TYPE_INT,   TYPE_BOOL,  TYPE_BOOL};
    bool ok = true;
    for (int i = 0; i < 6; i++) {
        pair->chans[i] = chan_make(a, elems[i], 0);
        ok = ok && pair->chans[i] != NULL;
    }
    for (int i = 0; i < 2; i++) {
        pair->end[i].read_deadline.cancel = np_cancel_new(a);
        pair->end[i].write_deadline.cancel = np_cancel_new(a);
        ok = ok && pair->end[i].read_deadline.cancel != NULL &&
             pair->end[i].write_deadline.cancel != NULL;
    }
    if (!ok) {
        np_pair_destroy(pair);
        return;
    }

    Chan *cb1 = pair->chans[0], *cb2 = pair->chans[1];
    Chan *cn1 = pair->chans[2], *cn2 = pair->chans[3];
    Chan *done1 = pair->chans[4], *done2 = pair->chans[5];
    np_end_init(&pair->end[0], pair, cb1, cn1, cb2, cn2, done1, done2);
    np_end_init(&pair->end[1], pair, cb2, cn2, cb1, cn1, done2, done1);

    NetConn p1 = {&np_conn_vt, &pair->end[0]};
    NetConn p2 = {&np_conn_vt, &pair->end[1]};
    BURROW_OUT(c1, p1);
    BURROW_OUT(c2, p2);
}

void net_pipe_free(NetConn c) {
    if (c.data == NULL)
        return;
    if (c.vt != &np_conn_vt)
        panic_str(BURROW_S("net: net_pipe_free of a connection net_pipe did not make"));
    NpPair *pair = ((NpEnd *)c.data)->pair;
    for (int i = 0; i < 2; i++) {
        NpDeadline *ds[2] = {&pair->end[i].read_deadline, &pair->end[i].write_deadline};
        for (int j = 0; j < 2; j++) {
            sync_mutex_lock(&ds[j]->mu);
            np_deadline_disarm(ds[j]);
            sync_mutex_unlock(&ds[j]->mu);
        }
    }
    np_pair_release(pair);
}
