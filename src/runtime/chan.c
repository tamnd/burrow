/* Channels. Go's runtime/chan.go, and the same algorithm.
 *
 * One lock per channel and nothing clever on top of it. That is worth saying
 * first because it looks like the wrong answer and is not. A channel operation
 * is a handful of pointer writes and a copy of one element, and the lock is
 * held for exactly that, so the contended case is short and the uncontended one
 * is an uncontended atomic. Go has tried the lock free version of this more
 * than once and has this in the tree.
 *
 * There are three ways a send can go and the order they are tried in is the
 * whole design:
 *
 *   1. A receiver is already waiting. Copy the value straight into the
 *      receiver's variable and make it runnable. The value never touches the
 *      channel, which is why an unbuffered channel with no buffer works and why
 *      a buffered channel that is empty still avoids the buffer.
 *   2. There is room in the buffer. Copy it in and carry on.
 *   3. Neither. Park the sender with a pointer to its value, and let whoever
 *      receives next take it from there.
 *
 * Receive is the mirror image with one wrinkle in case three, which is written
 * out where it happens.
 *
 * The waiter is a stack local in the goroutine that blocks, which is Go's
 * sudog without the pool, because a parked goroutine's stack is not going
 * anywhere and the one thing a sudog pool buys is not needed when the waiter is
 * already free. It is reachable from the channel's queue for exactly as long as
 * the goroutine is parked, and whoever takes it off the queue owns it until it
 * readies that goroutine and must not touch it afterwards, because by then the
 * frame it lives in may be gone.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/chan.h"

#include "burrow/atomic.h"
#include "burrow/lock.h"
#include "burrow/note.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------- the waiter
 *
 * A goroutine, or a thread, blocked on a channel.
 *
 * Both, because burrow is a library inside somebody else's program and that
 * program's own threads are allowed to use a channel. A goroutine parks and
 * costs no thread, which is the whole point of the scheduler. A thread that is
 * not running a goroutine has nothing to park, so it sleeps on a note instead
 * and costs itself. Go cannot be in the second situation and therefore has only
 * the first half of this.
 *
 * `sendp` and `recvp` are one field in Go and two here, because a sender offers
 * a const pointer and a receiver hands out a mutable one, and casting the const
 * away to store them in one field is the sort of thing that is fine until
 * somebody writes through the wrong one. Exactly one of the two is set. */
typedef struct Waiter Waiter;
struct Waiter {
    /* The goroutine, or NULL for a thread that is not running one. */
    Goroutine *g;

    /* The gate a thread waits on, used only when g is NULL. */
    burrow__Note note;
    bool has_note;

    /* Where the value comes from, for a sender. */
    const void *sendp;

    /* Where the value goes, for a receiver. NULL for a receive that does not
     * want the value, which is Go's `<-c` with nothing on the left. */
    void *recvp;

    /* Written by whoever completes the operation, read by the waiter once it
     * wakes. True means a value really moved. False means the channel was
     * closed underneath, which is an answer for a receiver and the end of the
     * program for a sender. */
    bool success;

    Waiter *next;
    Waiter *prev;
};

/* A queue of them, in arrival order, because Go wakes waiters first in first
 * out and programs are written expecting that. Doubly linked because select
 * has to take a waiter out of the middle when another of its cases wins, and
 * that is one PR away.
 *
 * `n` is the length, kept because the non-blocking paths want to know whether
 * anybody is waiting without taking the lock. Go reads `q.first` there instead
 * and gets away with it because its race detector does not look at the runtime.
 * Ours is an ordinary library and a thread sanitizer looks at all of it, so the
 * unlocked question is asked of a word that is written atomically. It costs one
 * relaxed store on a path that already holds a lock. */
typedef struct Waitq {
    Waiter *first;
    Waiter *last;
    uint32_t n;
} Waitq;

/* True if anybody is waiting. Safe to call without the lock, and then it is a
 * snapshot, which is all the caller wanted. */
static bool waitq_any(const Waitq *q) {
    return burrow__atomic_load_acquire_u32(&q->n) != 0;
}

static void waitq_push(Waitq *q, Waiter *w) {
    w->next = NULL;
    w->prev = q->last;
    if (q->last != NULL)
        q->last->next = w;
    else
        q->first = w;
    q->last = w;
    burrow__atomic_store_u32(&q->n, q->n + 1);
}

static Waiter *waitq_pop(Waitq *q) {
    Waiter *w = q->first;
    if (w == NULL)
        return NULL;

    q->first = w->next;
    if (q->first != NULL)
        q->first->prev = NULL;
    else
        q->last = NULL;
    burrow__atomic_store_u32(&q->n, q->n - 1);

    w->next = NULL;
    w->prev = NULL;
    return w;
}

/* ------------------------------------------------------------- the channel */

struct Chan {
    burrow__Lock lock;

    /* Where the header and the buffer came from, and how much of it there is,
     * because mem_free wants the size back and nothing outside this file can
     * work it out from an opaque pointer. */
    Alloc *a;
    size_t alloc_size;
    size_t alloc_align;

    const Type *elem;
    uint32_t elemsize;

    /* The ring. `buf` is NULL for an unbuffered channel. `sendx` and `recvx`
     * are indices into it and `qcount` is how many values are in it, which is
     * what makes empty and full tell each other apart without a spare slot. */
    uint8_t *buf;
    uint32_t dataqsiz;
    uint32_t qcount;
    uint32_t sendx;
    uint32_t recvx;

    /* Atomic, because the non-blocking paths read it once without the lock to
     * decide whether the slow path is worth entering at all. Every read that
     * decides anything is taken again under the lock. */
    uint32_t closed;

    Waitq recvq;
    Waitq sendq;
};

static uint8_t *slot(Chan *c, uint32_t i) {
    return c->buf + (size_t)i * (size_t)c->elemsize;
}

/* ------------------------------------------------------------- blocking
 *
 * The two ways to stop, and the two ways to be started again.
 *
 * A goroutine goes through sched_park, which takes the channel lock away once
 * the goroutine is off its thread. That callback is the whole reason park takes
 * a function rather than the caller unlocking first: between an unlock and a
 * park there is a window where the other side can see the waiter on the queue
 * and ready a goroutine that has not stopped running yet.
 *
 * A thread has no such window to close, because the gate it sleeps on is
 * allowed to be opened before anybody is standing at it. So it unlocks, then
 * sleeps, and a wake that lands in between leaves the gate open and the sleep
 * returns at once. */

static bool unlock_chan(Goroutine *g, void *p) {
    (void)g;
    burrow__unlock(&((Chan *)p)->lock);
    return true;
}

/* Sets `w` up to block, with the channel lock held.
 *
 * False means this thread cannot block here, which happens when it is not
 * running a goroutine and a note could not be allocated. That is an out of
 * memory condition on a path with nothing useful to do about it, so the caller
 * stops the program rather than returning a channel operation that silently
 * did not happen. */
static bool waiter_init(Waiter *w) {
    memset(w, 0, sizeof(*w));

    w->g = sched_current();
    if (w->g != NULL)
        return true;

    /* Transient, because this note is a local in the frame of whoever is about
     * to block and it is freed the moment the sleep returns. See the waker count
     * in src/runtime/note.c for what that costs and why it is asked for. */
    w->has_note = burrow__note_init_transient(&w->note);
    return w->has_note;
}

/* Blocks until somebody completes the operation. The channel lock is held on
 * the way in and is not held on the way out. */
static void waiter_block(Waiter *w, Chan *c) {
    if (w->g != NULL) {
        sched_park(unlock_chan, c);
        return;
    }

    burrow__unlock(&c->lock);
    burrow__note_sleep(&w->note);
    burrow__note_free(&w->note);
}

/* Starts `w` again. The channel lock must already be released, and `w` must not
 * be touched after this: the goroutine it belongs to may be running by the time
 * this returns, and the stack frame the waiter lives in may be gone. */
static void waiter_wake(Waiter *w) {
    if (w->g != NULL) {
        sched_ready(w->g);
        return;
    }
    burrow__note_wake(&w->note);
}

/* A send or a receive on a NULL channel, which Go says blocks forever.
 *
 * It is not as useless as it sounds. A select case on a channel variable that
 * is NULL is a case that can never fire, which is how a loop turns one of its
 * arms off, and that idiom only works if a bare send or receive agrees.
 *
 * Go's runtime notices when every goroutine in a program is stuck like this and
 * says so. burrow does not, for the reason in docs/design/06-runtime.md: a host
 * thread may be about to wake somebody and the runtime cannot see it. */
BURROW_NORETURN static void block_forever(void) {
    if (sched_current() != NULL) {
        for (;;)
            sched_park(NULL, NULL);
    }

    burrow__Note n;
    if (!burrow__note_init(&n))
        runtime_throw(BURROW_S("chan: out of memory blocking on a nil channel"));

    for (;;)
        burrow__note_sleep(&n);
}

/* ------------------------------------------------------------------ make */

Chan *chan_make(Alloc *a, const Type *elem, Int cap) {
    if (a == NULL)
        runtime_throw(BURROW_S("chan_make: no allocator"));
    if (elem == NULL)
        runtime_throw(BURROW_S("chan_make: nil element type"));
    if (cap < 0)
        runtime_throw(BURROW_S("makechan: size out of range"));

    size_t n = (size_t)cap;
    size_t each = elem->size;

    /* Go's overflow check, which matters here for the same reason: cap is a
     * number the program computed and elemsize is a number the type carries, so
     * the product is not bounded by anything the caller checked. */
    if (each != 0 && n > (SIZE_MAX / 2) / each)
        runtime_throw(BURROW_S("makechan: size out of range"));

    /* One allocation for the header and the buffer, which is what Go does. The
     * alignment is the stricter of the two, and the buffer starts at the first
     * offset past the header that satisfies it. */
    size_t align = _Alignof(Chan);
    if (elem->align > align)
        align = elem->align;

    size_t off = (sizeof(Chan) + align - 1) & ~(align - 1);
    size_t total = off + n * each;

    Chan *c = (Chan *)mem_alloc(a, total, align);
    if (c == NULL)
        return NULL;

    c->a = a;
    c->alloc_size = total;
    c->alloc_align = align;
    c->elem = elem;
    c->elemsize = (uint32_t)each;
    c->dataqsiz = (uint32_t)n;

    /* A zero sized element type has a buffer with nothing in it, and pointing
     * at the header rather than past the end of the allocation keeps every copy
     * a copy between two real addresses. Go points its buffer at the channel
     * for the same reason. */
    if (n == 0)
        c->buf = NULL;
    else if (each == 0)
        c->buf = (uint8_t *)c;
    else
        c->buf = (uint8_t *)c + off;

    return c;
}

void chan_free(Chan *c) {
    if (c == NULL)
        return;

    burrow__lock(&c->lock);
    bool busy = c->recvq.first != NULL || c->sendq.first != NULL;
    burrow__unlock(&c->lock);

    if (busy)
        runtime_throw(BURROW_S("chan_free: goroutines are still blocked on it"));

    mem_free(c->a, c, c->alloc_size, c->alloc_align);
}

/* -------------------------------------------------------------------- send */

static bool chan_send_impl(Chan *c, const void *v, bool block) {
    /* Go's unlocked early reject, and only for the non-blocking form. A send
     * that is not going to block has to answer now, and a full channel that is
     * open cannot become sendable by the time the lock is taken without some
     * other goroutine having run, which means the default arm was a correct
     * answer at the moment it was taken. Taking the lock to find that out would
     * put every select with a default through the contended path. */
    if (!block && burrow__atomic_load_acquire_u32(&c->closed) == 0 &&
        ((c->dataqsiz == 0 && !waitq_any(&c->recvq)) ||
         (c->dataqsiz > 0 &&
          burrow__atomic_load_acquire_u32(&c->qcount) == c->dataqsiz)))
        return false;

    burrow__lock(&c->lock);

    if (c->closed != 0) {
        burrow__unlock(&c->lock);
        runtime_throw(BURROW_S("send on closed channel"));
    }

    /* 1. Somebody is waiting for exactly this. Straight into their variable,
     *    which is what makes an unbuffered channel a handoff rather than a
     *    queue of one, and what keeps a buffered channel that is empty from
     *    touching its buffer. */
    Waiter *w = waitq_pop(&c->recvq);
    if (w != NULL) {
        if (w->recvp != NULL)
            type_copy(c->elem, w->recvp, v);
        w->success = true;

        burrow__unlock(&c->lock);
        waiter_wake(w);
        return true;
    }

    /* 2. Room in the buffer. */
    if (c->qcount < c->dataqsiz) {
        type_copy(c->elem, slot(c, c->sendx), v);
        c->sendx++;
        if (c->sendx == c->dataqsiz)
            c->sendx = 0;
        burrow__atomic_store_u32(&c->qcount, c->qcount + 1);

        burrow__unlock(&c->lock);
        return true;
    }

    if (!block) {
        burrow__unlock(&c->lock);
        return false;
    }

    /* 3. Wait, holding out a pointer to the value rather than a copy of it. The
     *    receiver that eventually arrives copies from there, so a send of a
     *    large struct across a full channel still copies it exactly once. */
    Waiter w2;
    if (!waiter_init(&w2)) {
        burrow__unlock(&c->lock);
        runtime_throw(BURROW_S("chan_send: out of memory"));
    }
    w2.sendp = v;
    waitq_push(&c->sendq, &w2);

    waiter_block(&w2, c);

    /* Woken. Either the value went somewhere or the channel closed under us,
     * and the second one is a program that is already wrong. */
    if (!w2.success)
        runtime_throw(BURROW_S("send on closed channel"));

    return true;
}

void chan_send(Chan *c, const void *v) {
    if (c == NULL)
        block_forever();

    (void)chan_send_impl(c, v, true);
}

bool chan_try_send(Chan *c, const void *v) {
    if (c == NULL)
        return false;

    return chan_send_impl(c, v, false);
}

/* ----------------------------------------------------------------- receive */

static bool chan_recv_impl(Chan *c, void *out, bool *ok, bool block) {
    /* The mirror of the send side's early reject, with the extra clause Go has:
     * an empty channel that is not closed cannot produce a value without
     * somebody else running, but an empty channel that is closed can produce an
     * answer, so the closed check comes after the emptiness check and not
     * before. Reading them the other way round would let a receive on a channel
     * that was closed while this ran report that nothing was ready. */
    if (!block) {
        const bool empty =
            (c->dataqsiz == 0 && !waitq_any(&c->sendq)) ||
            (c->dataqsiz > 0 && burrow__atomic_load_acquire_u32(&c->qcount) == 0);

        /* Empty and closed is not an answer on its own, which is why only the
         * open case answers here. A value may have arrived since the read above,
         * and a closed channel with a value in it still hands it over, so that
         * one falls through and gets settled under the lock. */
        if (empty && burrow__atomic_load_acquire_u32(&c->closed) == 0)
            return false;
    }

    burrow__lock(&c->lock);

    /* 1. A sender is waiting, which on an unbuffered channel means it is
     *    holding the value and on a buffered one means the buffer is full. */
    Waiter *w = waitq_pop(&c->sendq);
    if (w != NULL) {
        if (c->dataqsiz == 0) {
            if (out != NULL)
                type_copy(c->elem, out, w->sendp);
        } else {
            /* The wrinkle. The buffer is full, so the value that comes out is
             * the one at the head and not the one the sender is holding, and
             * the sender's goes in at the tail. Head and tail are the same slot
             * when the buffer is full, so this is one slot read and then
             * written, and the two indices move together. */
            uint8_t *qp = slot(c, c->recvx);
            if (out != NULL)
                type_copy(c->elem, out, qp);
            type_copy(c->elem, qp, w->sendp);

            c->recvx++;
            if (c->recvx == c->dataqsiz)
                c->recvx = 0;
            c->sendx = c->recvx;
        }
        w->success = true;

        burrow__unlock(&c->lock);
        waiter_wake(w);

        if (ok != NULL)
            *ok = true;
        return true;
    }

    /* 2. Something in the buffer. */
    if (c->qcount > 0) {
        uint8_t *qp = slot(c, c->recvx);
        if (out != NULL)
            type_copy(c->elem, out, qp);
        type_zero(c->elem, qp);

        c->recvx++;
        if (c->recvx == c->dataqsiz)
            c->recvx = 0;
        burrow__atomic_store_u32(&c->qcount, c->qcount - 1);

        burrow__unlock(&c->lock);
        if (ok != NULL)
            *ok = true;
        return true;
    }

    /* 3. Nothing to be had. Closed means there never will be, which is an
     *    answer rather than a wait. */
    if (c->closed != 0) {
        burrow__unlock(&c->lock);
        if (out != NULL)
            type_zero(c->elem, out);
        if (ok != NULL)
            *ok = false;
        return true;
    }

    if (!block) {
        burrow__unlock(&c->lock);
        return false;
    }

    Waiter w2;
    if (!waiter_init(&w2)) {
        burrow__unlock(&c->lock);
        runtime_throw(BURROW_S("chan_recv: out of memory"));
    }
    w2.recvp = out;
    waitq_push(&c->recvq, &w2);

    waiter_block(&w2, c);

    /* Woken. A sender filled `out` directly, or close zeroed it. */
    if (ok != NULL)
        *ok = w2.success;
    return true;
}

bool chan_recv(Chan *c, void *out) {
    if (c == NULL)
        block_forever();

    bool ok = false;
    (void)chan_recv_impl(c, out, &ok, true);
    return ok;
}

bool chan_try_recv(Chan *c, void *out, bool *ok) {
    if (c == NULL)
        return false;

    return chan_recv_impl(c, out, ok, false);
}

/* ------------------------------------------------------------------- close */

void chan_close(Chan *c) {
    if (c == NULL)
        runtime_throw(BURROW_S("close of nil channel"));

    burrow__lock(&c->lock);

    if (c->closed != 0) {
        burrow__unlock(&c->lock);
        runtime_throw(BURROW_S("close of closed channel"));
    }

    burrow__atomic_store_u32(&c->closed, 1);

    /* Everybody waiting comes off both queues and onto one local list, and
     * nobody is started again until the lock is back. That is Go's shape and
     * the reason for it is burrow/lock.h's rule: this lock blocks a thread, so
     * a critical section under it does not call out into the scheduler.
     *
     * Receivers get the zero value and a false, which is `v, ok := <-c` on a
     * drained closed channel. Senders get a false as well and turn it into the
     * end of the program when they wake, because a send that was in flight
     * across a close is the same mistake as a send after one. */
    Waiter *woken = NULL;
    Waiter *w;

    while ((w = waitq_pop(&c->recvq)) != NULL) {
        if (w->recvp != NULL)
            type_zero(c->elem, w->recvp);
        w->success = false;
        w->next = woken;
        woken = w;
    }
    while ((w = waitq_pop(&c->sendq)) != NULL) {
        w->success = false;
        w->next = woken;
        woken = w;
    }

    burrow__unlock(&c->lock);

    while (woken != NULL) {
        Waiter *next = woken->next;
        waiter_wake(woken);
        woken = next;
    }
}

/* ------------------------------------------------------------------ asking */

Int chan_len(const Chan *c) {
    if (c == NULL)
        return 0;

    /* Deliberately without the lock, because this is a snapshot either way and
     * a lock would only make it a snapshot that cost more. Go reads the field
     * unsynchronised here too. */
    return (Int)burrow__atomic_load_acquire_u32(&c->qcount);
}

Int chan_cap(const Chan *c) {
    if (c == NULL)
        return 0;

    return (Int)c->dataqsiz;
}

const Type *chan_elem(const Chan *c) {
    if (c == NULL)
        return NULL;

    return c->elem;
}
