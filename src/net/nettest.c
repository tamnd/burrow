/* Derived from Go's src/internal/nettest/conn.go, listener.go and queue.go.
 * Go source: go1.27.3.
 *
 * Copyright 2026 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "nettest.h"

#include "../sync/gate.h"

#include "burrow/atomic.h"
#include "burrow/bytes.h"
#include "burrow/chan.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/func.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/net.h"
#include "burrow/net/netip.h"
#include "burrow/os.h"
#include "burrow/time.h"
#include "burrow/type.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Go leaves the pair to the collector, and a deadline's timer can fire after
 * both ends are done with, so the pair is counted: one reference for each end,
 * which burrow__nettest_conn_free puts down, and one for each armed timer. A
 * timer's callback has its own small block, nett_arm, that says which deadline
 * it is for, in place of Go's comparison of d.timer with the timer that fired.
 * A stop that says true frees the block there and then, and otherwise the
 * callback frees it when it finds it is no longer the deadline's. */

typedef struct NettPair NettPair;
typedef struct NettHalf NettHalf;
typedef struct NettArm NettArm;

/* Go's connDeadline. */
typedef struct NettDeadline {
    NettArm *arm;
    bool expired;
} NettDeadline;

/* connHalf is one direction data flow in a Conn.
 * The connHalf contains a buffer.
 * Writes to the connHalf push to the buffer and reads pull from it. */
struct NettHalf {
    NetAddr addr;

    /* A half can be readable and/or writable.
     *
     * These four channels act as a lock,
     * and allow waiting for readability/writability.
     * When the half is unlocked, exactly one channel contains a value.
     * When the half is locked, all channels are empty. */
    Chan *lockr;  /* readable */
    Chan *lockw;  /* writable */
    Chan *lockrw; /* readable and writable */
    Chan *lockc;  /* neither readable nor writable */

    /* Read and write timeouts. */
    NettDeadline read_deadline, write_deadline;

    Int buf_max; /* maximum buffer size */
    BytesBuffer buf;

    bool read_closed, write_closed;
    Error read_err, write_err; /* errors returned by reads/writes */
    Error close_err; /* error returned by closing the conn reading from this half */

    NettPair *pair;
};

/* A Conn consists of read and write halves.
 * A Conn's peer has the same halves, only swapped.
 *
 * A Conn reads from r and writes to w. */
struct NettestConn {
    NettHalf *r, *w;

    /* peer is the other endpoint. */
    NettestConn *peer;
};

struct NettPair {
    NettHalf half[2];
    NettestConn conn[2];
    NetTCPAddr *owned[2]; /* the addresses the pair made itself */
    uint32_t refs;
    Alloc *a;
};

struct NettArm {
    NettHalf *h;
    NettDeadline *d;
    TimeTimer *timer;
};

static bool nett_same(Error a, Error b) {
    return a.vt == b.vt && a.data == b.data;
}

BURROW_SENTINEL_ERROR(burrow__nettest_err_closed_by_peer, "connection closed by peer");

/* ---------------------------------------------------------------- the pair */

static void nett_pair_destroy(NettPair *pair) {
    Alloc *a = pair->a;
    for (int i = 0; i < 2; i++) {
        NettHalf *h = &pair->half[i];
        chan_free(h->lockr);
        chan_free(h->lockw);
        chan_free(h->lockrw);
        chan_free(h->lockc);
        bytes_buffer_free(&h->buf);
        net_tcp_addr_free(a, pair->owned[i]);
    }
    mem_free(a, pair, sizeof(NettPair), _Alignof(NettPair));
}

static void nett_pair_release(NettPair *pair) {
    if (burrow__atomic_add_u32(&pair->refs, (uint32_t)-1) == 1)
        nett_pair_destroy(pair);
}

/* ------------------------------------------------------------- the halves */

/* Go's struct{}{}, which only has to be there. */
static const bool nett_token = true;

static bool nett_can_read_locked(NettHalf *h) {
    return BURROW_FAILED(h->read_err) || h->read_deadline.expired ||
           bytes_buffer_len(&h->buf) > 0 || h->read_closed || h->write_closed;
}

static bool nett_can_write_locked(NettHalf *h) {
    return BURROW_FAILED(h->write_err) || h->write_deadline.expired ||
           h->buf_max > bytes_buffer_len(&h->buf) || h->read_closed || h->write_closed;
}

/* lock locks h. */
static void nett_lock(NettHalf *h) {
    SelectCase cases[4] = {
        BURROW_RECV(h->lockw, NULL),  /* writable */
        BURROW_RECV(h->lockr, NULL),  /* readable */
        BURROW_RECV(h->lockrw, NULL), /* readable and writable */
        BURROW_RECV(h->lockc, NULL),  /* neither readable nor writable */
    };
    (void)chan_select(cases, 4);
}

/* unlock unlocks h. */
static void nett_unlock(NettHalf *h) {
    bool can_read = nett_can_read_locked(h);
    bool can_write = nett_can_write_locked(h);
    if (can_read && can_write)
        chan_send(h->lockrw, &nett_token); /* readable and writable */
    else if (can_read)
        chan_send(h->lockr, &nett_token); /* readable */
    else if (can_write)
        chan_send(h->lockw, &nett_token); /* writable */
    else
        chan_send(h->lockc, &nett_token); /* neither readable nor writable */
}

static bool nett_half_init(NettHalf *h, NettPair *pair, NetAddr addr) {
    Alloc *a = pair->a;
    h->addr = addr;
    h->lockw = chan_make(a, TYPE_BOOL, 1);
    h->lockr = chan_make(a, TYPE_BOOL, 1);
    h->lockrw = chan_make(a, TYPE_BOOL, 1);
    h->lockc = chan_make(a, TYPE_BOOL, 1);
    h->buf_max = BURROW_INT_MAX; /* unlimited */
    h->buf = BYTES_BUFFER(a);
    h->pair = pair;
    if (h->lockw == NULL || h->lockr == NULL || h->lockrw == NULL || h->lockc == NULL)
        return false;
    nett_unlock(h);
    return true;
}

static bool nett_half_can_read(NettHalf *h) {
    nett_lock(h);
    bool ok = nett_can_read_locked(h);
    nett_unlock(h);
    return ok;
}

/* waitAndLockForRead waits until h is readable and locks it. */
static void nett_wait_and_lock_for_read(NettHalf *h) {
    SelectCase cases[2] = {
        BURROW_RECV(h->lockr, NULL),  /* readable */
        BURROW_RECV(h->lockrw, NULL), /* readable and writable */
    };
    (void)chan_select(cases, 2);
}

/* waitAndLockForWrite waits until h is writable and locks it. */
static void nett_wait_and_lock_for_write(NettHalf *h) {
    SelectCase cases[2] = {
        BURROW_RECV(h->lockw, NULL),  /* writable */
        BURROW_RECV(h->lockrw, NULL), /* readable and writable */
    };
    (void)chan_select(cases, 2);
}

static Int nett_half_read(NettHalf *h, Slice b, Error *err) {
    nett_wait_and_lock_for_read(h);
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    if (h->read_closed)
        e = net_err_closed;
    else if (h->read_deadline.expired)
        e = os_err_deadline_exceeded;
    else if (bytes_buffer_len(&h->buf) > 0)
        n = bytes_buffer_read(&h->buf, b, &e);
    else if (h->write_closed)
        e = io_eof;
    else
        e = h->read_err;
    nett_unlock(h);
    *err = e;
    return n;
}

static void nett_half_set_buffer_size(NettHalf *h, Int size) {
    nett_lock(h);
    h->buf_max = size;
    nett_unlock(h);
}

static Int nett_half_write_partial(NettHalf *h, Slice b, Error *err) {
    nett_wait_and_lock_for_write(h);
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    if (h->write_closed) {
        e = net_err_closed;
    } else if (h->write_deadline.expired) {
        e = os_err_deadline_exceeded;
    } else if (h->read_closed) {
        e = burrow__nettest_err_closed_by_peer;
    } else if (BURROW_FAILED(h->write_err)) {
        e = h->write_err;
    } else {
        Int write_max = h->buf_max - bytes_buffer_len(&h->buf);
        if (write_max < b.len)
            b.len = write_max;
        n = bytes_buffer_write(&h->buf, b, &e);
    }
    nett_unlock(h);
    *err = e;
    return n;
}

static Int nett_half_write(NettHalf *h, Slice b, Error *err) {
    Int n = 0;
    while (n < b.len) {
        Error e;
        Int nn = nett_half_write_partial(h, slice_sub(b, n, b.len), &e);
        n += nn;
        if (BURROW_FAILED(e)) {
            *err = e;
            return n;
        }
    }
    *err = BURROW_NO_ERROR;
    return n;
}

/* -------------------------------------------------------------- deadlines */

/* What the timer runs, on a goroutine of its own. */
static void nett_deadline_fire(void *env) {
    NettArm *arm = (NettArm *)env;
    NettHalf *h = arm->h;
    nett_lock(h);
    if (arm->d->arm == arm) {
        arm->d->arm = NULL;
        arm->d->expired = true;
    }
    TimeTimer *timer = arm->timer;
    nett_unlock(h);
    NettPair *pair = h->pair;
    time_timer_free(timer);
    mem_free(pair->a, arm, sizeof(NettArm), _Alignof(NettArm));
    nett_pair_release(pair);
}

/* Disarms d's timer, if it has one, with h locked. */
static void nett_deadline_disarm(NettDeadline *d, NettHalf *h) {
    NettArm *arm = d->arm;
    if (arm == NULL)
        return;
    d->arm = NULL;
    if (time_timer_stop(arm->timer)) {
        NettPair *pair = h->pair;
        time_timer_free(arm->timer);
        mem_free(pair->a, arm, sizeof(NettArm), _Alignof(NettArm));
        nett_pair_release(pair);
    }
}

/* Go's connDeadline.setDeadline, with h as the locker. The one way it fails is
 * a failed allocation for a time in the future, and then there is no
 * deadline. Not in Go, which always succeeds. */
static Error nett_deadline_set(NettDeadline *d, NettHalf *h, Time t) {
    nett_lock(h);
    nett_deadline_disarm(d, h);
    if (time_is_zero(t)) {
        /* No deadline. */
        d->expired = false;
        nett_unlock(h);
        return BURROW_NO_ERROR;
    }
    Duration expiry = time_until(t);
    if (expiry <= 0) {
        /* Deadline has already passed. */
        d->expired = true;
        nett_unlock(h);
        return BURROW_NO_ERROR;
    }
    /* Deadline is in the future. */
    d->expired = false;
    Error err = BURROW_NO_ERROR;
    NettPair *pair = h->pair;
    NettArm *arm = (NettArm *)mem_alloc(pair->a, sizeof(NettArm), _Alignof(NettArm));
    if (arm != NULL) {
        arm->h = h;
        arm->d = d;
        (void)burrow__atomic_add_u32(&pair->refs, 1);
        arm->timer =
            time_after_func(pair->a, expiry, BURROW_FN(Func, nett_deadline_fire, arm));
        if (arm->timer == NULL) {
            (void)burrow__atomic_add_u32(&pair->refs, (uint32_t)-1);
            mem_free(pair->a, arm, sizeof(NettArm), _Alignof(NettArm));
            arm = NULL;
        }
    }
    if (arm == NULL)
        err = burrow_err_out_of_memory;
    d->arm = arm;
    nett_unlock(h);
    return err;
}

/* -------------------------------------------------------------------- Conn */

/* The error a Read, Write or Close gives, as an OpError in the calling
 * goroutine's error arena. */
static Error nett_op_error(Str op, NetAddr source, NetAddr addr, Error err) {
    NetOpError oe = {op, BURROW_S("tcp"), source, addr, err};
    return net_op_error_as_error(&oe, error_allocator());
}

/* newConnPair, with the address of each end either made from an AddrPort,
 * when ap is not NULL, or borrowed. */
static NettestConn *nett_new_conn_pair(Alloc *a, const NetipAddrPort *ap1,
                                       NetAddr addr1, const NetipAddrPort *ap2,
                                       NetAddr addr2) {
    NettPair *pair = (NettPair *)mem_alloc(a, sizeof(NettPair), _Alignof(NettPair));
    if (pair == NULL)
        return NULL;
    memset(pair, 0, sizeof(NettPair));
    pair->a = a;
    pair->refs = 2;
    bool ok = true;
    if (ap1 != NULL) {
        pair->owned[0] = net_tcp_addr_from_addr_port(a, *ap1);
        addr1 = net_tcp_addr_as_addr(pair->owned[0]);
        ok = pair->owned[0] != NULL;
    }
    if (ap2 != NULL) {
        pair->owned[1] = net_tcp_addr_from_addr_port(a, *ap2);
        addr2 = net_tcp_addr_as_addr(pair->owned[1]);
        ok = ok && pair->owned[1] != NULL;
    }
    ok = nett_half_init(&pair->half[0], pair, addr1) && ok;
    ok = nett_half_init(&pair->half[1], pair, addr2) && ok;
    if (!ok) {
        nett_pair_destroy(pair);
        return NULL;
    }
    NettHalf *h1 = &pair->half[0], *h2 = &pair->half[1];
    NettestConn *c1 = &pair->conn[0], *c2 = &pair->conn[1];
    c1->r = h1;
    c1->w = h2;
    c2->r = h2;
    c2->w = h1;
    c1->peer = c2;
    c2->peer = c1;
    burrow__nettest_conn_set_read_buffer_size(c1, -1);
    burrow__nettest_conn_set_read_buffer_size(c2, -1);
    return c1;
}

void burrow__nettest_new_conn_pair(Alloc *a, NettestConn **c1, NettestConn **c2) {
    NetipAddrPort ap1 = netip_must_parse_addr_port(BURROW_S("192.0.2.1:10000"));
    NetipAddrPort ap2 = netip_must_parse_addr_port(BURROW_S("192.0.2.1:10001"));
    NetAddr none = {NULL, NULL};
    NettestConn *c = nett_new_conn_pair(a, &ap1, none, &ap2, none);
    *c1 = c;
    *c2 = c == NULL ? NULL : c->peer;
}

void burrow__nettest_conn_free(NettestConn *c) {
    if (c == NULL)
        return;
    nett_lock(c->r);
    nett_deadline_disarm(&c->r->read_deadline, c->r);
    nett_unlock(c->r);
    nett_lock(c->w);
    nett_deadline_disarm(&c->w->write_deadline, c->w);
    nett_unlock(c->w);
    nett_pair_release(c->r->pair);
}

NettestConn *burrow__nettest_conn_peer(NettestConn *c) {
    return c->peer;
}

Int burrow__nettest_conn_read(NettestConn *c, Slice b, Error *err) {
    Error e;
    Int n = nett_half_read(c->r, b, &e);
    if (BURROW_FAILED(e) && !nett_same(e, io_eof))
        e = nett_op_error(BURROW_S("read"), burrow__nettest_conn_remote_addr(c),
                          burrow__nettest_conn_local_addr(c), e);
    BURROW_OUT(err, e);
    return n;
}

bool burrow__nettest_conn_can_read(NettestConn *c) {
    return nett_half_can_read(c->r);
}

Int burrow__nettest_conn_write(NettestConn *c, Slice b, Error *err) {
    Error e;
    Int n = nett_half_write(c->w, b, &e);
    if (BURROW_FAILED(e))
        e = nett_op_error(BURROW_S("write"), burrow__nettest_conn_local_addr(c),
                          burrow__nettest_conn_remote_addr(c), e);
    BURROW_OUT(err, e);
    return n;
}

bool burrow__nettest_conn_is_closed(NettestConn *c) {
    nett_lock(c->r);
    bool read_closed = c->r->read_closed;
    nett_unlock(c->r);
    nett_lock(c->w);
    bool write_closed = c->w->write_closed;
    nett_unlock(c->w);
    return read_closed && write_closed;
}

Error burrow__nettest_conn_close_read(NettestConn *c) {
    nett_lock(c->r);
    bytes_buffer_reset(&c->r->buf); /* discard unread data */
    c->r->read_closed = true;
    nett_unlock(c->r);
    return BURROW_NO_ERROR;
}

Error burrow__nettest_conn_close_write(NettestConn *c) {
    nett_lock(c->w);
    c->w->write_closed = true;
    nett_unlock(c->w);
    return BURROW_NO_ERROR;
}

Error burrow__nettest_conn_close(NettestConn *c) {
    nett_lock(c->r);
    bool read_closed = c->r->read_closed;
    bytes_buffer_reset(&c->r->buf); /* discard unread data */
    c->r->read_closed = true;
    Error err = c->r->close_err;
    nett_unlock(c->r);

    nett_lock(c->w);
    bool write_closed = c->w->write_closed;
    c->w->write_closed = true;
    nett_unlock(c->w);

    if (read_closed && write_closed)
        err = net_err_closed;
    if (BURROW_FAILED(err)) {
        NetAddr none = {NULL, NULL};
        err = nett_op_error(BURROW_S("close"), none, burrow__nettest_conn_local_addr(c),
                            err);
    }
    return err;
}

void burrow__nettest_conn_set_close_error(NettestConn *c, Error err) {
    nett_lock(c->r);
    c->r->close_err = err;
    nett_unlock(c->r);
}

NetAddr burrow__nettest_conn_local_addr(NettestConn *c) {
    nett_lock(c->r);
    NetAddr addr = c->r->addr;
    nett_unlock(c->r);
    return addr;
}

void burrow__nettest_conn_set_local_addr(NettestConn *c, NetAddr addr) {
    nett_lock(c->r);
    c->r->addr = addr;
    nett_unlock(c->r);
}

/* Go takes c.r's lock here and reads c.w.addr, which the peer's SetLocalAddr
 * writes under c.w's lock. This takes c.w's, so that the two do not race. */
NetAddr burrow__nettest_conn_remote_addr(NettestConn *c) {
    nett_lock(c->w);
    NetAddr addr = c->w->addr;
    nett_unlock(c->w);
    return addr;
}

Error burrow__nettest_conn_set_deadline(NettestConn *c, Time t) {
    Error err = burrow__nettest_conn_set_read_deadline(c, t);
    Error werr = burrow__nettest_conn_set_write_deadline(c, t);
    return BURROW_FAILED(err) ? err : werr;
}

Error burrow__nettest_conn_set_read_deadline(NettestConn *c, Time t) {
    return nett_deadline_set(&c->r->read_deadline, c->r, t);
}

Error burrow__nettest_conn_set_write_deadline(NettestConn *c, Time t) {
    return nett_deadline_set(&c->w->write_deadline, c->w, t);
}

void burrow__nettest_conn_set_read_buffer_size(NettestConn *c, Int size) {
    if (size < 0)
        size = BURROW_INT_MAX;
    nett_half_set_buffer_size(c->r, size);
}

void burrow__nettest_conn_set_read_error(NettestConn *c, Error err) {
    nett_lock(c->r);
    c->r->read_err = err;
    nett_unlock(c->r);
}

void burrow__nettest_conn_set_write_error(NettestConn *c, Error err) {
    nett_lock(c->w);
    c->w->write_err = err;
    nett_unlock(c->w);
}

/* ---------------------------------------------------------- Conn as net.Conn */

static Int nett_vt_read(void *self, Slice b, Error *err) {
    return burrow__nettest_conn_read((NettestConn *)self, b, err);
}

static Int nett_vt_write(void *self, Slice b, Error *err) {
    return burrow__nettest_conn_write((NettestConn *)self, b, err);
}

static Error nett_vt_close(void *self) {
    return burrow__nettest_conn_close((NettestConn *)self);
}

static NetAddr nett_vt_local_addr(void *self) {
    return burrow__nettest_conn_local_addr((NettestConn *)self);
}

static NetAddr nett_vt_remote_addr(void *self) {
    return burrow__nettest_conn_remote_addr((NettestConn *)self);
}

static Error nett_vt_set_deadline(void *self, Time t) {
    return burrow__nettest_conn_set_deadline((NettestConn *)self, t);
}

static Error nett_vt_set_read_deadline(void *self, Time t) {
    return burrow__nettest_conn_set_read_deadline((NettestConn *)self, t);
}

static Error nett_vt_set_write_deadline(void *self, Time t) {
    return burrow__nettest_conn_set_write_deadline((NettestConn *)self, t);
}

static const Type nett_conn_desc = {
    BURROW_S_INIT("Conn"),
    BURROW_S_INIT("internal/nettest"),
    KIND_STRUCT,
    (uint32_t)sizeof(NettestConn),
    (uint16_t)_Alignof(NettestConn),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e74636eU, /* "ntcn" */
    NULL,
};

static const NetConnVT nett_conn_vt = {
    {&nett_conn_desc, nett_vt_read},
    {&nett_conn_desc, nett_vt_write},
    {&nett_conn_desc, nett_vt_close},
    nett_vt_local_addr,
    nett_vt_remote_addr,
    nett_vt_set_deadline,
    nett_vt_set_read_deadline,
    nett_vt_set_write_deadline,
};

NetConn burrow__nettest_conn_as_net_conn(NettestConn *c) {
    NetConn nc = {NULL, NULL};
    if (c != NULL) {
        nc.vt = &nett_conn_vt;
        nc.data = c;
    }
    return nc;
}

NettestConn *burrow__nettest_conn_from_net_conn(NetConn c) {
    return c.vt == &nett_conn_vt ? (NettestConn *)c.data : NULL;
}

/* ---------------------------------------------------------------- Listener */

struct NettestListener {
    Gate gate;

    /* Go's queue[*Conn], as one array whose live part is
     * queue[head:head+len]. */
    NettestConn **queue;
    Int head, len, cap;

    bool closed;
    Error accept_err;
    Error close_err;

    NetAddr addr;
    NetipAddrPort nextaddr;

    NetTCPAddr *own_addr; /* the address NewListener made */
    Alloc *a;
};

static bool nett_queue_push(NettestListener *li, NettestConn *c) {
    if (li->head + li->len == li->cap) {
        if (li->head > 0) {
            memmove(li->queue, li->queue + li->head,
                    (size_t)li->len * sizeof(NettestConn *));
            li->head = 0;
        } else {
            Int ncap = li->cap == 0 ? 4 : li->cap * 2;
            NettestConn **q = (NettestConn **)mem_alloc(
                li->a, (size_t)ncap * sizeof(NettestConn *), _Alignof(NettestConn *));
            if (q == NULL)
                return false;
            if (li->len > 0)
                memcpy(q, li->queue, (size_t)li->len * sizeof(NettestConn *));
            mem_free(li->a, li->queue, (size_t)li->cap * sizeof(NettestConn *),
                     _Alignof(NettestConn *));
            li->queue = q;
            li->cap = ncap;
        }
    }
    li->queue[li->head + li->len] = c;
    li->len++;
    return true;
}

static NettestConn *nett_queue_pop(NettestListener *li) {
    if (li->len == 0)
        return NULL;
    NettestConn *c = li->queue[li->head];
    li->queue[li->head] = NULL;
    li->head++;
    li->len--;
    if (li->len == 0)
        li->head = 0;
    return c;
}

static void nett_listener_lock(NettestListener *li) {
    (void)burrow__gate_lock(&li->gate);
}

static void nett_listener_unlock(NettestListener *li) {
    burrow__gate_unlock(&li->gate, BURROW_FAILED(li->accept_err) || li->len > 0);
}

NettestListener *burrow__nettest_new_listener(Alloc *a) {
    NettestListener *li = (NettestListener *)mem_alloc(a, sizeof(NettestListener),
                                                       _Alignof(NettestListener));
    if (li == NULL)
        return NULL;
    memset(li, 0, sizeof(NettestListener));
    li->a = a;
    li->own_addr = net_tcp_addr_from_addr_port(
        a, netip_must_parse_addr_port(BURROW_S("192.0.2.1:1000")));
    if (li->own_addr == NULL || !burrow__gate_init(&li->gate, a, false)) {
        net_tcp_addr_free(a, li->own_addr);
        mem_free(a, li, sizeof(NettestListener), _Alignof(NettestListener));
        return NULL;
    }
    li->addr = net_tcp_addr_as_addr(li->own_addr);
    li->nextaddr = netip_must_parse_addr_port(BURROW_S("192.0.2.1:10001"));
    return li;
}

void burrow__nettest_listener_free(NettestListener *li) {
    if (li == NULL)
        return;
    for (NettestConn *c; (c = nett_queue_pop(li)) != NULL;)
        burrow__nettest_conn_free(c);
    Alloc *a = li->a;
    mem_free(a, li->queue, (size_t)li->cap * sizeof(NettestConn *),
             _Alignof(NettestConn *));
    burrow__gate_destroy(&li->gate);
    net_tcp_addr_free(a, li->own_addr);
    mem_free(a, li, sizeof(NettestListener), _Alignof(NettestListener));
}

Error burrow__nettest_listener_close(NettestListener *li) {
    nett_listener_lock(li);
    Error err = li->close_err;
    li->closed = true;
    li->accept_err = net_err_closed;
    li->close_err = net_err_closed;
    NetAddr addr = li->addr;
    nett_listener_unlock(li);
    if (BURROW_FAILED(err)) {
        NetAddr none = {NULL, NULL};
        return nett_op_error(BURROW_S("close"), none, addr, err);
    }
    return err;
}

NetAddr burrow__nettest_listener_addr(NettestListener *li) {
    nett_listener_lock(li);
    NetAddr addr = li->addr;
    nett_listener_unlock(li);
    return addr;
}

void burrow__nettest_listener_set_addr(NettestListener *li, NetAddr addr) {
    nett_listener_lock(li);
    li->addr = addr;
    nett_listener_unlock(li);
}

static void nett_no_config(void *env, NettestConn *conn) {
    (void)env;
    (void)conn;
}

NettestConn *burrow__nettest_listener_new_conn(NettestListener *li) {
    return burrow__nettest_listener_new_conn_config(
        li, BURROW_FN(NettestConnFunc, nett_no_config, NULL));
}

NettestConn *burrow__nettest_listener_new_conn_config(NettestListener *li,
                                                      NettestConnFunc f) {
    nett_listener_lock(li);
    NetAddr none = {NULL, NULL};
    NettestConn *cli = nett_new_conn_pair(li->a, &li->nextaddr, none, NULL, li->addr);
    if (cli != NULL) {
        NettestConn *srv = cli->peer;
        if (!nett_queue_push(li, srv)) {
            burrow__nettest_conn_free(srv);
            burrow__nettest_conn_free(cli);
            cli = NULL;
        }
    }
    if (cli != NULL) {
        li->nextaddr =
            netip_addr_port_from(netip_addr_port_addr(li->nextaddr),
                                 (uint16_t)(netip_addr_port_port(li->nextaddr) + 1));
        /* Not in Go: the server end is queued before f runs, so that a full
         * queue is found out before f has been called. Nobody can accept it
         * until the lock is let go, after f returns. */
        BURROW_CALLF(f, cli);
    }
    nett_listener_unlock(li);
    return cli;
}

NettestConn *burrow__nettest_listener_accept(NettestListener *li, Error *err) {
    (void)burrow__gate_wait_and_lock(&li->gate, context_background());
    NettestConn *c = NULL;
    Error e = BURROW_NO_ERROR;
    if (BURROW_FAILED(li->accept_err) && li->len == 0) {
        NetAddr none = {NULL, NULL};
        e = nett_op_error(BURROW_S("accept"), none, li->addr, li->accept_err);
    } else {
        c = nett_queue_pop(li);
    }
    nett_listener_unlock(li);
    BURROW_OUT(err, e);
    return c;
}

void burrow__nettest_listener_set_accept_error(NettestListener *li, Error err) {
    nett_listener_lock(li);
    if (!li->closed)
        li->accept_err = err;
    nett_listener_unlock(li);
}

void burrow__nettest_listener_set_close_error(NettestListener *li, Error err) {
    nett_listener_lock(li);
    if (!li->closed)
        li->close_err = err;
    nett_listener_unlock(li);
}

/* ------------------------------------------------- Listener as net.Listener */

static Error nett_li_vt_close(void *self) {
    return burrow__nettest_listener_close((NettestListener *)self);
}

static NetConn nett_li_vt_accept(void *self, Error *err) {
    return burrow__nettest_conn_as_net_conn(
        burrow__nettest_listener_accept((NettestListener *)self, err));
}

static NetAddr nett_li_vt_addr(void *self) {
    return burrow__nettest_listener_addr((NettestListener *)self);
}

static const Type nett_listener_desc = {
    BURROW_S_INIT("Listener"),
    BURROW_S_INIT("internal/nettest"),
    KIND_STRUCT,
    (uint32_t)sizeof(NettestListener),
    (uint16_t)_Alignof(NettestListener),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x6e746c69U, /* "ntli" */
    NULL,
};

static const NetListenerVT nett_listener_vt = {
    {&nett_listener_desc, nett_li_vt_close},
    nett_li_vt_accept,
    nett_li_vt_addr,
};

NetListener burrow__nettest_listener_as_net_listener(NettestListener *li) {
    NetListener l = {NULL, NULL};
    if (li != NULL) {
        l.vt = &nett_listener_vt;
        l.data = li;
    }
    return l;
}
