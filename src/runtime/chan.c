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
#include "burrow/sched.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------- the sleeper
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
 * This is one struct and the entry on a channel's queue is another, which looks
 * like one struct too many until select turns up. A select is one sleeper with
 * a queue entry on every channel it is waiting on, and only one of those
 * entries may complete it. Keeping who is asleep apart from what is queued is
 * what makes that one decision rather than several. */
typedef struct Parked {
    /* The goroutine, or NULL for a thread that is not running one. */
    Goroutine *g;

    /* The gate a thread waits on, used only when g is NULL. */
    burrow__Note note;
    bool has_note;

    /* Select only, and the reason select needs anything beyond a queue entry.
     *
     * `claimed` goes from zero to one exactly once, by a compare and swap taken
     * by whoever pops one of this select's entries with that channel's lock
     * held. Winning it is what owning the select means. Losing it means another
     * channel got there first, so the entry that was just popped is a leftover
     * and the search moves on to the next one.
     *
     * It cannot be a per channel decision, because the whole point is that the
     * decision is shared across every channel the select is on, and the locks
     * are per channel. Go keeps the same flag on the g and uses it the same
     * way. */
    bool is_select;
    uint32_t claimed;

    /* Select only again, and the other half of what select needs.
     *
     * A plain send or receive hands park one channel to unlock and park
     * dereferences nothing else. A select hands it a list, and that list is a
     * local in the frame of the goroutine that is parking. Park runs the unlock
     * on the scheduler's own stack, after the goroutine is marked waiting, and
     * the moment the first channel in that list comes unlocked somebody may
     * claim this select and start the goroutine running again. Then two threads
     * are reading the same frame: one finishing the unlock and one running the
     * goroutine that owns it.
     *
     * So the wake is held back until the unlock says it is finished. Each side
     * swaps in its own answer and reads back what the other one left, which
     * means exactly one of them finds PARKING and that one is the loser of the
     * race:
     *
     *   PARKING  nobody has decided yet, which is where it starts
     *   PARKED   the unlock finished first, so a claim wakes the sleeper
     *   WOKEN    a claim landed first, so the sleeper does not park at all and
     *            carries straight on
     *
     * A swap and not a compare and swap, because this word is what hands the
     * frame from one thread to the other and a swap is a read and a write
     * whichever way it goes. A compare and swap that fails is only a read, and
     * a read that fails to be a release leaves the unlock's own reads with
     * nothing ordering them against whatever the woken goroutine does next.
     * That is a real race and a thread sanitizer says so.
     *
     * Go has no version of this because a Go sudog comes from a pool rather
     * than a stack, so the list Go's unlock walks stays valid whatever the
     * goroutine does next. Trading an allocation per wait for a compare and
     * swap per wait is the right way round. */
    uint32_t state;

    /* Which case the winner completed, written under the claim and read by the
     * sleeper once it wakes. */
    Int won;
} Parked;

#define PARK_PARKING 0u
#define PARK_PARKED 1u
#define PARK_WOKEN 2u

/* ------------------------------------------------------------- the waiter
 *
 * One entry on one channel's queue. A plain send or receive has one of these
 * and a select has one per case, and they all point at the same sleeper.
 *
 * `sendp` and `recvp` are one field in Go and two here, because a sender offers
 * a const pointer and a receiver hands out a mutable one, and casting the const
 * away to store them in one field is the sort of thing that is fine until
 * somebody writes through the wrong one. Exactly one of the two is set. */
typedef struct Waiter Waiter;
struct Waiter {
    /* Who to start again, and never NULL. */
    Parked *p;

    /* Which case this entry is, for a select. Ignored otherwise. */
    Int caseidx;

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
 * out and programs are written expecting that. Doubly linked because a select
 * has to take its losing entries out of the middle of queues when one of its
 * cases wins.
 *
 * `n` is the length, kept because the non-blocking paths want to know whether
 * anybody is waiting without taking the lock. Go reads `q.first` there instead
 * and gets away with it because its race detector does not look at the runtime.
 * Ours is an ordinary library and a thread sanitizer looks at all of it, so the
 * unlocked question is asked of a word that is written atomically. It costs one
 * relaxed store on a path that already holds a lock.
 *
 * With select in the picture `n` can count entries that are leftovers, so a yes
 * from it is now only a reason to go and look properly under the lock. That was
 * already all anybody did with it. */
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

static void waitq_unlink(Waitq *q, Waiter *w) {
    if (w->prev != NULL)
        w->prev->next = w->next;
    else
        q->first = w->next;

    if (w->next != NULL)
        w->next->prev = w->prev;
    else
        q->last = w->prev;

    w->next = NULL;
    w->prev = NULL;
    burrow__atomic_store_u32(&q->n, q->n - 1);
}

/* Takes the first entry that is still there to be taken, with the lock held.
 *
 * Coming off the queue is not the same as being owned once select exists, so
 * the compare and swap is the real answer and the unlink is bookkeeping. Losing
 * the swap means some other channel completed that select already and this
 * entry is a leftover, so it stays off the queue and the search carries on.
 * Dropping it here rather than leaving it is deliberate: the select will come
 * along and unlink its own entries when it wakes, and whichever of the two gets
 * there first, the other finds it already gone.
 *
 * The winner writes down which case it is completing, here rather than in each
 * of the five callers, because here is where the claim is and the two belong
 * together. A plain send or receive skips the atomic entirely, which is what
 * the flag is for. */
static Waiter *waitq_pop(Waitq *q) {
    for (;;) {
        Waiter *w = q->first;
        if (w == NULL)
            return NULL;

        waitq_unlink(q, w);

        if (!w->p->is_select)
            return w;

        uint32_t unclaimed = 0;
        if (burrow__atomic_cas_u32(&w->p->claimed, &unclaimed, 1)) {
            w->p->won = w->caseidx;
            return w;
        }
    }
}

/* Takes one particular entry off, if it is still on, with the lock held.
 *
 * The first test is the one that needs explaining. An entry with no neighbours
 * is either the only thing on the queue or something a pop already dropped, and
 * those two look identical from the entry. The queue's own head is what tells
 * them apart. */
static void waitq_remove(Waitq *q, Waiter *w) {
    if (w->prev == NULL && w->next == NULL && q->first != w)
        return;

    waitq_unlink(q, w);
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

    /* The synctest bubble this channel was made in, or NULL, which is what
     * every channel outside a test is. Set once and never written again.
     *
     * A channel carries this rather than the goroutines deciding for
     * themselves, because the question a bubble asks about a blocked receive is
     * whether anybody outside could still send, and that is a property of the
     * channel and not of whoever is standing at it. A channel made in the
     * bubble can only be reached from inside it, so a wait on one is a wait on
     * another goroutine in the bubble and nothing else. */
    burrow__Bubble *bubble;
};

/* Whether a wait on this channel is one a bubble should count as durable, and
 * the place the misuse Go makes fatal is caught.
 *
 * The cost to a program with no bubbles anywhere is one load and one branch,
 * which is why the channel is asked before the goroutine is. A channel made
 * outside a bubble can be used from anywhere and never makes a wait durable,
 * and that is the answer on every send and receive in every program that is not
 * a test. */
static bool bubbled(const Chan *c) {
    if (c->bubble == NULL)
        return false;

    const Goroutine *g = sched_current();
    if (g == NULL || g->bubble != c->bubble)
        runtime_throw(
            BURROW_S("chan: a channel made inside a synctest bubble was used from "
                     "outside it"));
    return true;
}

static uint8_t *slot(Chan *c, uint32_t i) {
    return c->buf + (size_t)i * (size_t)c->elemsize;
}

/* ------------------------------------------------------- moving one value
 *
 * The four ways a value crosses a channel, each with the lock held and none of
 * them deciding anything. The deciding is done by the callers, and there are
 * three of those now that select is one of them, which is why these are
 * functions rather than the straight line they used to be.
 *
 * The two that take a waiter mark it done. None of them wake anybody, because
 * waking happens with the lock down and these are called with it up. */

/* Straight into a waiting receiver's variable. This is what makes an unbuffered
 * channel a handoff rather than a queue of one, and what keeps a buffered
 * channel that is empty from touching its buffer at all. */
static void give_to_receiver(Chan *c, Waiter *w, const void *v) {
    if (w->recvp != NULL)
        type_copy(c->elem, w->recvp, v);
    w->success = true;
}

/* Out of a waiting sender.
 *
 * On an unbuffered channel that is the sender's own variable going straight
 * into the receiver's. On a buffered one the buffer is full, which is the only
 * way a sender is waiting on one, so the value that comes out is the one at the
 * head and the sender's goes in at the tail. Head and tail are the same slot
 * when the buffer is full, so that is one slot read and then written, and the
 * two indices move together. */
static void take_from_sender(Chan *c, Waiter *w, void *out) {
    if (c->dataqsiz == 0) {
        if (out != NULL)
            type_copy(c->elem, out, w->sendp);
    } else {
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
}

/* Into the tail of the buffer, which the caller has checked has room. */
static void put_in_buffer(Chan *c, const void *v) {
    type_copy(c->elem, slot(c, c->sendx), v);
    c->sendx++;
    if (c->sendx == c->dataqsiz)
        c->sendx = 0;
    burrow__atomic_store_u32(&c->qcount, c->qcount + 1);
}

/* Out of the head of the buffer, which the caller has checked is not empty. */
static void take_from_buffer(Chan *c, void *out) {
    uint8_t *qp = slot(c, c->recvx);
    if (out != NULL)
        type_copy(c->elem, out, qp);
    type_zero(c->elem, qp);

    c->recvx++;
    if (c->recvx == c->dataqsiz)
        c->recvx = 0;
    burrow__atomic_store_u32(&c->qcount, c->qcount - 1);
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

/* Sets a sleeper up, with the channel lock held.
 *
 * False means this thread cannot block here, which happens when it is not
 * running a goroutine and a note could not be allocated. That is an out of
 * memory condition on a path with nothing useful to do about it, so the caller
 * stops the program rather than returning a channel operation that silently
 * did not happen. */
static bool parked_init(Parked *p) {
    memset(p, 0, sizeof(*p));

    p->g = sched_current();
    if (p->g != NULL)
        return true;

    /* Transient, because this note is a local in the frame of whoever is about
     * to block and it is freed the moment the sleep returns. See the waker count
     * in src/runtime/note.c for what that costs and why it is asked for. */
    p->has_note = burrow__note_init_transient(&p->note);
    return p->has_note;
}

/* Says the unlock is finished and nobody is reading the sleeper's frame any
 * more, so a claim may start it again. Answers false if a claim already landed,
 * which means do not go to sleep at all.
 *
 * Called from inside the unlock callback, which is the last thing that touches
 * the frame before the sleeper stops being the only one who can. */
static bool parked_commit(Parked *p) {
    if (!p->is_select)
        return true;

    return burrow__atomic_swap_u32(&p->state, PARK_PARKED) == PARK_PARKING;
}

/* Blocks until somebody completes the operation. Whatever locks are held on the
 * way in are dropped by `unlockf` and are not held on the way out.
 *
 * `durable` is for a synctest bubble and says that the only thing which can end
 * this wait is another goroutine in the same bubble, which for a channel means
 * the channel was made inside it. A thread waiting on a note is never durable:
 * it is not a goroutine, so it is not in anybody's bubble, and the park below
 * is not the one being asked about. */
static void parked_sleep(Parked *p, SchedUnlockFn unlockf, void *arg, bool durable) {
    if (p->g != NULL) {
        burrow__park(unlockf, arg, durable);
        return;
    }

    /* A thread has no window to close, because the gate it sleeps on is allowed
     * to be opened before anybody is standing at it. So it unlocks out in the
     * open, and a wake that lands in between leaves the gate open and the sleep
     * returns at once. The callback is the same one a goroutine would hand to
     * park, and it gets a NULL goroutine because there is not one.
     *
     * A false from it means a claim landed while the unlock was still going, in
     * which case nobody opened the gate and nobody is going to, so there is
     * nothing here to wait for. */
    if (!unlockf(NULL, arg))
        return;

    burrow__note_sleep(&p->note);
}

/* Gives back whatever the sleep needed. Call it once the sleeper can no longer
 * be reached from any queue, which for a plain send or receive is the moment
 * the sleep returns and for a select is after it has unlinked its entries. */
static void parked_done(Parked *p) {
    if (p->has_note)
        burrow__note_free(&p->note);
}

/* Starts a sleeper again. No channel lock may be held, and the sleeper must not
 * be touched after this: the goroutine it belongs to may be running by the time
 * this returns, and the stack frame it lives in may be gone. */
static void parked_wake(Parked *p) {
    /* A select that has not committed yet is still using its own frame to
     * unlock the channels it locked, so it cannot be started here. The claim is
     * already recorded, so leaving a note that says so is enough: the commit
     * fails, the sleeper skips the sleep and reads the claim out for itself. */
    if (p->is_select && burrow__atomic_swap_u32(&p->state, PARK_WOKEN) == PARK_PARKING)
        return;

    if (p->g != NULL) {
        sched_ready(p->g);
        return;
    }
    burrow__note_wake(&p->note);
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
        /* Durable, which sounds odd for a wait that never ends and is exactly
         * right. A bubble asks whether anything outside it could end this wait,
         * and here the answer is that nothing anywhere could, so a bubble whose
         * goroutines have all ended up here has deadlocked and should say so.
         * Go counts a nil channel the same way. */
        for (;;)
            burrow__park(NULL, NULL, true);
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
        runtime_panic(BURROW_S("makechan: size out of range"));

    size_t n = (size_t)cap;
    size_t each = elem->size;

    /* Go's overflow check, which matters here for the same reason: cap is a
     * number the program computed and elemsize is a number the type carries, so
     * the product is not bounded by anything the caller checked. */
    if (each != 0 && n > (SIZE_MAX / 2) / each)
        runtime_panic(BURROW_S("makechan: size out of range"));

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

    /* Whoever is making it decides, once, and that is the whole of how a
     * channel comes to belong to a bubble. There is no call to put one in and
     * none to take one out, the same as Go. */
    const Goroutine *g = sched_current();
    c->bubble = g != NULL ? g->bubble : NULL;

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
    /* A safe point, here at the top where no lock is held. A send that ends up
     * waiting gives the processor up anyway, so the one this matters for is the
     * send that finds a receiver or finds room and returns straight away: a
     * producer feeding a buffered channel faster than anybody drains it never
     * blocks and would otherwise hold its thread until it ran out of things to
     * send. See src/runtime/sched.c under preemption for why the request cannot
     * simply move the goroutine where it stands. */
    burrow__preempt_point();

    /* At the top rather than at the park, because the question of whether this
     * goroutine is allowed to touch this channel at all does not depend on
     * whether it ends up waiting, and a program that gets it wrong should hear
     * about it on the send that happened to find a receiver too. */
    bool durable = bubbled(c);

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
        runtime_panic(BURROW_S("send on closed channel"));
    }

    /* 1. Somebody is waiting for exactly this. */
    Waiter *w = waitq_pop(&c->recvq);
    if (w != NULL) {
        give_to_receiver(c, w, v);
        Parked *p = w->p;

        burrow__unlock(&c->lock);
        parked_wake(p);
        return true;
    }

    /* 2. Room in the buffer. */
    if (c->qcount < c->dataqsiz) {
        put_in_buffer(c, v);
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
    Parked p;
    if (!parked_init(&p)) {
        burrow__unlock(&c->lock);
        runtime_throw(BURROW_S("chan_send: out of memory"));
    }

    Waiter w2;
    memset(&w2, 0, sizeof(w2));
    w2.p = &p;
    w2.sendp = v;
    waitq_push(&c->sendq, &w2);

    parked_sleep(&p, unlock_chan, c, durable);
    parked_done(&p);

    /* Woken. Either the value went somewhere or the channel closed under us,
     * and the second one is a program that is already wrong. */
    if (!w2.success)
        runtime_panic(BURROW_S("send on closed channel"));

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
    /* The send side's safe point, mirrored, and for the consumer that is always
     * a value behind rather than the producer that is always ahead. */
    burrow__preempt_point();

    bool durable = bubbled(c);

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
        take_from_sender(c, w, out);
        Parked *p = w->p;

        burrow__unlock(&c->lock);
        parked_wake(p);

        if (ok != NULL)
            *ok = true;
        return true;
    }

    /* 2. Something in the buffer. */
    if (c->qcount > 0) {
        take_from_buffer(c, out);

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

    Parked p;
    if (!parked_init(&p)) {
        burrow__unlock(&c->lock);
        runtime_throw(BURROW_S("chan_recv: out of memory"));
    }

    Waiter w2;
    memset(&w2, 0, sizeof(w2));
    w2.p = &p;
    w2.recvp = out;
    waitq_push(&c->recvq, &w2);

    parked_sleep(&p, unlock_chan, c, durable);
    parked_done(&p);

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
        runtime_panic(BURROW_S("close of nil channel"));

    burrow__lock(&c->lock);

    if (c->closed != 0) {
        burrow__unlock(&c->lock);
        runtime_panic(BURROW_S("close of closed channel"));
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
        /* Read before the wake and not after, because by the time the wake
         * returns the frame this entry lives in may be gone. */
        Waiter *next = woken->next;
        parked_wake(woken->p);
        woken = next;
    }
}

/* ------------------------------------------------------------------ select
 *
 * Go's runtime/select.go, in three passes and with the same order of events.
 *
 *   1. Lock every channel in the case list. Walk the cases in a random order
 *      looking for one that can run now, and run the first one found.
 *   2. Nothing could run, and there is no default. Put an entry on every
 *      channel's queue and go to sleep on all of them at once.
 *   3. Woken. Exactly one case completed. Take the other entries off the queues
 *      they are still on and report which case it was.
 *
 * Two things here are not obvious and both are load bearing.
 *
 * The order the cases are visited in is random, and it has to be a shuffled
 * order rather than a count of what is ready followed by a pick. Finding out
 * whether a case can run means taking a waiter off a queue and claiming it, and
 * a claim cannot be taken back. So the choice is made before the looking, by
 * shuffling, and then the first case that can run is the one that runs. That
 * comes out uniform over whichever cases turned out to be ready, which is what
 * Go promises and what stops a loop with a fast channel and a slow one in it
 * from starving the slow one.
 *
 * The order the channels are locked in is increasing address. It cannot be the
 * case order, because two selects listing the same two channels the other way
 * round would take one each and wait for the other forever. Address order is
 * something every select in the program agrees on without any of them having to
 * coordinate. So the cases are sorted by the address of their channel, once,
 * and the locking and the unlocking both walk that.
 *
 * This used to find the next channel to lock by scanning the whole case list
 * for the lowest address above the last one, which needed no list and no sort
 * and read well. It was also quadratic, and burrow-bench says what that was
 * worth: a select over eight arms spent a quarter of its time in the unlock
 * alone, because locking and unlocking eight channels once meant a hundred and
 * forty four passes over the case list. Sorting costs one pass and a heap. */

/* How many cases fit in the frame of the call. See the note on chan_select in
 * burrow/chan.h for why there is a number here at all. */
#define SELECT_SMALL 16

/* Go's limit, and the same one, because the case index has to fit somewhere and
 * a select with more arms than this is a program that wanted a different
 * shape. */
#define SELECT_MAX 65536

typedef struct Select {
    SelectCase *cases;
    Int n;

    /* A random permutation of 0 through n, the order to visit the cases in. */
    const uint32_t *order;

    /* The cases that have a channel, in increasing address order, with the
     * duplicates left in. nlock is how many, which is the case count less the
     * defaults and the nil channels. */
    const uint32_t *lock_order;
    Int nlock;

    /* One entry per case, used only if the select has to wait. Indexed by case
     * number, including the cases that never get one, because an index that
     * lines up with the case list is worth more than the few unused structs. */
    Waiter *waiters;

    /* The sleeper every one of those entries points at. */
    Parked p;

    /* Carried out of the first pass rather than acted on where it happens.
     *
     * A waiter whose operation this select completed has to be started again
     * with no channel lock held, and a send that found its channel closed has
     * to bring the locks down before it brings the program down. Both of those
     * are decided under the locks and done above them. */
    Parked *wake;
    bool send_on_closed;
} Select;

/* The address of the channel case i is on, as an integer.
 *
 * Addresses go through uintptr_t because comparing two pointers that are not
 * into the same object with < is not something C defines, and comparing the
 * integers is. */
static uintptr_t chan_at(const SelectCase *cases, uint32_t i) {
    return (uintptr_t)cases[i].c;
}

/* One step of the heap, moving the entry at root down until the subtree below
 * it has no larger address in it. */
static void sift(uint32_t *order, const SelectCase *cases, Int root, Int len) {
    for (;;) {
        Int child = 2 * root + 1;
        if (child >= len)
            return;

        if (child + 1 < len &&
            chan_at(cases, order[child]) < chan_at(cases, order[child + 1]))
            child++;

        if (chan_at(cases, order[root]) >= chan_at(cases, order[child]))
            return;

        uint32_t t = order[root];
        order[root] = order[child];
        order[child] = t;
        root = child;
    }
}

/* Fills order with the cases that have a channel, in increasing address order,
 * and answers how many there are. Duplicates stay in, and the walks below step
 * over them, because a channel listed twice has to be locked once.
 *
 * A heap sort rather than the insertion sort that would be faster on the three
 * or four cases a real select has, and it is the same choice Go makes for the
 * same reason. This runs on a list the caller built, the caller is allowed
 * sixty five thousand of them, and an insertion sort handed that many in
 * descending address order would sit there for the rest of the afternoon. */
static Int lock_order(uint32_t *order, const SelectCase *cases, Int n) {
    Int len = 0;

    for (Int i = 0; i < n; i++) {
        if (cases[i].op == SELECT_DEFAULT || cases[i].c == NULL)
            continue;
        order[len++] = (uint32_t)i;
    }

    for (Int i = len / 2 - 1; i >= 0; i--)
        sift(order, cases, i, len);

    for (Int i = len - 1; i > 0; i--) {
        uint32_t t = order[0];
        order[0] = order[i];
        order[i] = t;
        sift(order, cases, 0, i);
    }

    return len;
}

static void sel_lock(const Select *s) {
    Chan *last = NULL;

    for (Int k = 0; k < s->nlock; k++) {
        Chan *c = s->cases[s->lock_order[k]].c;
        if (c == last)
            continue;

        burrow__lock(&c->lock);
        last = c;
    }
}

static void sel_unlock(const Select *s) {
    Chan *last = NULL;

    for (Int k = 0; k < s->nlock; k++) {
        Chan *c = s->cases[s->lock_order[k]].c;
        if (c == last)
            continue;

        burrow__unlock(&c->lock);
        last = c;
    }
}

static bool unlock_select(Goroutine *g, void *arg) {
    Select *s = (Select *)arg;
    (void)g;
    sel_unlock(s);

    /* Nothing below this line may read the select, and nothing above it may be
     * skipped, because the commit is what hands the frame over. */
    return parked_commit(&s->p);
}

/* Fisher and Yates, out of the runtime's generator.
 *
 * The number below the bound comes from a multiply and a shift rather than a
 * modulo, which is Lemire's, and it is what Go's cheaprandn does. Take a random
 * number in 0 to 2^32, multiply it by the bound, and the top half of the 64 bit
 * product is a number in 0 to the bound. The reason to bother is that the
 * modulo it replaces is a hardware divide, and on the machine in burrow-bench's
 * results a select over eight arms was spending a quarter of its time in this
 * function waiting for seven of them.
 *
 * Both are biased, by a part in two to the thirty second here against a part in
 * two to the forty eighth before, over a case list that is almost always three
 * long. Neither is a number anybody can measure, and the unbiased answer is a
 * rejection loop on a path that runs on every select. */
static void shuffle(uint32_t *order, Int n) {
    for (Int i = 0; i < n; i++)
        order[i] = (uint32_t)i;

    for (Int i = n - 1; i > 0; i--) {
        uint64_t r = runtime_rand64() >> 32U;
        Int j = (Int)((r * (uint64_t)(i + 1)) >> 32U);
        uint32_t t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
}

/* One pass over the cases in poll order, with every channel locked, looking for
 * one that can run now and running it. Answers its index, or -1 for none.
 *
 * The checks per case are the same three the plain send and receive do and in
 * the same order, because a select arm that behaved differently from the bare
 * operation would be a trap. */
static Int sel_try(Select *s) {
    for (Int k = 0; k < s->n; k++) {
        SelectCase *sc = &s->cases[s->order[k]];
        Chan *c = sc->c;

        if (sc->op == SELECT_DEFAULT || c == NULL)
            continue;

        if (sc->op == SELECT_SEND) {
            /* Checked before anything else, the way Go checks it, which is why
             * a select with a ready case and a send on a closed channel may or
             * may not stop the program. The random order decides. */
            if (c->closed != 0) {
                s->send_on_closed = true;
                return (Int)s->order[k];
            }

            Waiter *w = waitq_pop(&c->recvq);
            if (w != NULL) {
                give_to_receiver(c, w, sc->send);
                s->wake = w->p;
                return (Int)s->order[k];
            }

            if (c->qcount < c->dataqsiz) {
                put_in_buffer(c, sc->send);
                return (Int)s->order[k];
            }

            continue;
        }

        Waiter *w = waitq_pop(&c->sendq);
        if (w != NULL) {
            take_from_sender(c, w, sc->recv);
            s->wake = w->p;
            if (sc->ok != NULL)
                *sc->ok = true;
            return (Int)s->order[k];
        }

        if (c->qcount > 0) {
            take_from_buffer(c, sc->recv);
            if (sc->ok != NULL)
                *sc->ok = true;
            return (Int)s->order[k];
        }

        if (c->closed != 0) {
            if (sc->recv != NULL)
                type_zero(c->elem, sc->recv);
            if (sc->ok != NULL)
                *sc->ok = false;
            return (Int)s->order[k];
        }
    }

    return -1;
}

/* Puts an entry on every channel in the list, with every channel locked. */
static void sel_enqueue(Select *s) {
    for (Int i = 0; i < s->n; i++) {
        SelectCase *sc = &s->cases[i];
        if (sc->op == SELECT_DEFAULT || sc->c == NULL)
            continue;

        Waiter *w = &s->waiters[i];
        memset(w, 0, sizeof(*w));
        w->p = &s->p;
        w->caseidx = i;

        if (sc->op == SELECT_SEND) {
            w->sendp = sc->send;
            waitq_push(&sc->c->sendq, w);
        } else {
            w->recvp = sc->recv;
            waitq_push(&sc->c->recvq, w);
        }
    }
}

/* Takes them all off again, with every channel locked. The winning entry is in
 * here too and is already off, which waitq_remove works out for itself. */
static void sel_dequeue(Select *s) {
    for (Int i = 0; i < s->n; i++) {
        SelectCase *sc = &s->cases[i];
        if (sc->op == SELECT_DEFAULT || sc->c == NULL)
            continue;

        if (sc->op == SELECT_SEND)
            waitq_remove(&sc->c->sendq, &s->waiters[i]);
        else
            waitq_remove(&sc->c->recvq, &s->waiters[i]);
    }
}

Int chan_select(SelectCase *cases, Int n) {
    if (n < 0)
        runtime_throw(BURROW_S("chan_select: negative case count"));
    if (n > SELECT_MAX)
        runtime_throw(BURROW_S("select case count too large"));

    /* A safe point for the same reason the two above are, and this one earns it
     * more than either: a select with a default arm in a loop is the one shape
     * in the language that is built to go round for ever without blocking. */
    burrow__preempt_point();

    /* The first default in the list, and the first channel, in one pass.
     *
     * Go's compiler rejects a second default. This is an array built at run
     * time and nothing can reject anything, so the first one wins and the rest
     * are unreachable, which is what a second default means anyway. */
    Int dflt = -1;
    Alloc *home = NULL;
    Int nchan = 0;
    Int nbubbled = 0;

    for (Int i = 0; i < n; i++) {
        switch (cases[i].op) {
        case SELECT_DEFAULT:
            if (dflt < 0)
                dflt = i;
            break;
        case SELECT_SEND:
        case SELECT_RECV:
            if (cases[i].c == NULL)
                break;
            if (home == NULL)
                home = cases[i].c->a;
            nchan++;
            if (bubbled(cases[i].c))
                nbubbled++;
            break;
        default:
            runtime_throw(BURROW_S("chan_select: bad case op"));
        }
    }

    /* Every channel or none, which is stricter than asking whether any of them
     * is in the bubble and is the only answer that is safe.
     *
     * A select waiting on one channel from inside the bubble and one from
     * outside it can be completed by a goroutine the bubble knows nothing
     * about, so it is not durably blocked however bubbled the other case is.
     * Calling it durable would let a test decide everybody had stopped while
     * one of them was waiting on the outside world.
     *
     * A case on a NULL channel is not counted either way, because it can never
     * fire and so cannot be the thing that ends the wait. A select made
     * entirely of them has nchan zero, which is not durable here and does not
     * need to be: it goes to block_forever below, which has its own answer. */
    bool durable = nchan > 0 && nbubbled == nchan;

    /* No channel anywhere means nothing can ever become ready, so the answer is
     * the default if there is one and a wait that never ends if there is not.
     * Taken here because it is also the one shape that would want scratch space
     * with nowhere to get it from: no channel means no allocator to ask. */
    if (home == NULL) {
        if (dflt >= 0)
            return dflt;
        block_forever();
    }

    uint32_t small_order[SELECT_SMALL];
    uint32_t small_locks[SELECT_SMALL];
    Waiter small_waiters[SELECT_SMALL];

    Select s;
    memset(&s, 0, sizeof(s));
    s.cases = cases;
    s.n = n;

    uint32_t *order = small_order;
    uint32_t *locks = small_locks;
    void *scratch = NULL;
    size_t scratch_size = 0;
    const size_t scratch_align = _Alignof(Waiter);

    if (n > SELECT_SMALL) {
        /* One block, entries first, because a Waiter is the stricter of the two
         * alignments and putting it first means no padding to compute and no
         * pointer that has to be nudged into place. */
        size_t count = (size_t)n;

        scratch_size = count * (sizeof(Waiter) + 2 * sizeof(uint32_t));
        scratch = mem_alloc(home, scratch_size, scratch_align);
        if (scratch == NULL)
            runtime_throw(BURROW_S("chan_select: out of memory"));

        s.waiters = (Waiter *)scratch;
        order = (uint32_t *)(s.waiters + count);
        locks = order + count;
    } else {
        s.waiters = small_waiters;
    }

    shuffle(order, n);
    s.order = order;

    s.nlock = lock_order(locks, cases, n);
    s.lock_order = locks;

    sel_lock(&s);
    Int won = sel_try(&s);

    if (won < 0 && dflt >= 0)
        won = dflt;

    if (won >= 0) {
        sel_unlock(&s);

        if (s.wake != NULL)
            parked_wake(s.wake);

        if (scratch != NULL)
            mem_free(home, scratch, scratch_size, scratch_align);

        if (s.send_on_closed)
            runtime_panic(BURROW_S("send on closed channel"));

        return won;
    }

    /* Nothing was ready and there is no default, so wait on all of them. */
    if (!parked_init(&s.p)) {
        sel_unlock(&s);
        if (scratch != NULL)
            mem_free(home, scratch, scratch_size, scratch_align);
        runtime_throw(BURROW_S("chan_select: out of memory"));
    }
    s.p.is_select = true;
    s.p.won = -1;

    sel_enqueue(&s);
    parked_sleep(&s.p, unlock_select, &s, durable);

    /* Woken, which means somebody claimed this select and wrote down which case
     * they completed. The other entries are still sitting on their queues, or
     * have been dropped by a pop that could not claim them, and either way they
     * have to be gone before this frame is. */
    sel_lock(&s);
    sel_dequeue(&s);
    sel_unlock(&s);

    parked_done(&s.p);

    won = s.p.won;
    SelectCase *sc = &cases[won];
    bool success = s.waiters[won].success;

    if (scratch != NULL)
        mem_free(home, scratch, scratch_size, scratch_align);

    /* A send that woke up unsuccessful was in flight across a close, which is
     * the same mistake as a send after one. */
    if (sc->op == SELECT_SEND && !success)
        runtime_panic(BURROW_S("send on closed channel"));

    if (sc->op == SELECT_RECV && sc->ok != NULL)
        *sc->ok = success;

    return won;
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
