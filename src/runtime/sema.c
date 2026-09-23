/* The semaphore under sync. See burrow/sema.h for what it is for.
 *
 * The counter is the caller's. Everything else is here, in a table of 251 wait
 * queues indexed by the counter's address, so that a mutex nobody is waiting on
 * costs four bytes and no queue at all. Go does this for the same reason, and
 * the number is Go's: a prime, so that addresses that differ by a power of two
 * do not all land in the same bucket, and large enough that two live mutexes
 * colliding is unusual on any real program.
 *
 * Inside a bucket the waiters are a treap rather than a list, which is the part
 * that looks like too much machinery until you count. Two addresses that
 * collide share a lock, and a release has to find the waiters for its own
 * address among everybody else's. A list makes that a scan, and a program with
 * ten thousand blocked goroutines has forty per bucket to walk on every single
 * unlock. The treap is keyed on the address and balanced on a random priority,
 * so the search is logarithmic without anybody having to rebalance anything.
 * Waiters on the same address hang off one treap node as a list, since they are
 * all going to be found together anyway.
 *
 * Goroutines and threads both, which is the one thing here Go does not have to
 * do. A waiter carries either a goroutine to ready or a note to open, the queue
 * does not care which, and the difference shows up in exactly two functions at
 * the bottom of this file.
 *
 * Derived from Go's src/runtime/sema.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sema.h"

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

/* ------------------------------------------------------------- the spinning */

/* How many times a waiter spins before it gives up and sleeps, and how many
 * pause instructions one of those turns is worth. Both are Go's numbers, from
 * runtime/proc.go, where they are active_spin and active_spin_cnt. */
#define SPIN_TRIES 4
#define SPIN_PAUSES 30

bool burrow__sync_can_spin(int32_t iter) {
    if (iter >= SPIN_TRIES)
        return false;
    return burrow__sched_spin_ok();
}

void burrow__sync_do_spin(void) {
    for (int i = 0; i < SPIN_PAUSES; i++)
        burrow__atomic_spin_hint();
}

/* -------------------------------------------------------------- the waiter */

/* One goroutine, or one thread, waiting. On an address if it came in through
 * the semaphore, on a notify list if it came in through a Cond.
 *
 * It is a local in the frame of whoever is blocking, which is Go's sudog
 * without the pool for the reason channels do the same thing: a waiter that is
 * parked is a waiter whose stack is not going anywhere, so the free list a
 * sudog pool amounts to is already there.
 *
 * Whoever takes it off the queue owns it until the moment it wakes the waiter,
 * and must not touch it afterwards, because by then the frame may be gone. */
struct burrow__SemaWaiter {
    /* The counter being waited on, and the treap key. Unused by a waiter on a
     * notify list, which is queued on the list itself rather than an address. */
    uint32_t *addr;

    /* The treap. `prev` holds lower addresses and `next` higher ones, and
     * `parent` is what makes a rotation something that can be done from the
     * node rather than from the root. */
    struct burrow__SemaWaiter *parent;
    struct burrow__SemaWaiter *prev;
    struct burrow__SemaWaiter *next;

    /* Everybody else waiting on this same address, oldest first, hanging off
     * the one of them that is in the treap. The tail is kept so that arriving
     * at the back of the queue is a pointer write rather than a walk, and it is
     * only meaningful on the node that is actually in the treap. A waiter on a
     * notify list uses `waitlink` as the link in that list and leaves the tail
     * alone, since the list keeps its own. */
    struct burrow__SemaWaiter *waitlink;
    struct burrow__SemaWaiter *waittail;

    /* Three jobs, never two at the same time. While this waiter is in the treap
     * it is the random priority that keeps the tree balanced, and it always has
     * its low bit set so that it cannot be zero. Once the waiter has been taken
     * out it is cleared, and a release doing a handoff then sets it to one to
     * say that the wakeup has already been paid for and the waiter should not go
     * looking for another. On a notify list it is the ticket, and none of the
     * treap's rules apply because a waiter there is never in a treap. */
    uint32_t ticket;

    /* The goroutine to ready, or NULL for a thread that is not running one. */
    Goroutine *g;

    /* The gate a thread waits on, used only when g is NULL. */
    burrow__Note note;
    bool has_note;
};

typedef struct burrow__SemaWaiter SemaWaiter;

/* ---------------------------------------------------------------- the table */

typedef struct Root {
    burrow__Lock lock;

    /* The treap of unique addresses. Under the lock. */
    SemaWaiter *treap;

    /* How many waiters are on this root. Atomic and read without the lock,
     * because it is what lets an uncontended release stay out of the lock
     * entirely, which is the common case by a wide margin. */
    uint32_t nwait;
} Root;

/* A prime, so that addresses a power of two apart do not collide, and 251
 * rather than something larger because the table is padded out to a cache line
 * an entry and this is already sixteen kilobytes of BSS. Go's number. */
#define SEMA_TAB_SIZE 251

/* Padded, because two unrelated mutexes that happen to land in adjacent
 * buckets should not have their locks in the same cache line. The alignment
 * does the padding, so there is no array of filler bytes to keep in step with
 * the struct. */
typedef struct PaddedRoot {
    _Alignas(BURROW_CACHELINE) Root root;
} PaddedRoot;

static PaddedRoot semtable[SEMA_TAB_SIZE];

/* Which queue an address belongs to. The shift throws away the three bits that
 * are always zero on a four byte aligned object, which would otherwise make two
 * thirds of the table unreachable. */
static Root *root_for(const uint32_t *addr) {
    uintptr_t key = (uintptr_t)addr >> 3;
    return &semtable[key % SEMA_TAB_SIZE].root;
}

/* ---------------------------------------------------------------- the treap
 *
 * Two rotations and the two operations built out of them, all of it Go's, and
 * all of it under the root's lock.
 *
 * A treap is a binary search tree on the key and a heap on the priority at the
 * same time. Inserting means putting the node where the search tree says it
 * goes and then rotating it up until the heap is satisfied, and because the
 * priority is random the result is balanced on average without anybody storing
 * a colour or a height. */

/* Turns (x a (y b c)) into (y (x a b) c), where x is the node passed in. */
static void rotate_left(Root *root, SemaWaiter *x) {
    SemaWaiter *p = x->parent;
    SemaWaiter *y = x->next;
    SemaWaiter *b = y->prev;

    y->prev = x;
    x->parent = y;
    x->next = b;
    if (b != NULL)
        b->parent = x;

    y->parent = p;
    if (p == NULL)
        root->treap = y;
    else if (p->prev == x)
        p->prev = y;
    else if (p->next == x)
        p->next = y;
    else
        runtime_throw(
            BURROW_S("sema: rotate left on a node that is not its parent's child"));
}

/* Turns (y (x a b) c) into (x a (y b c)), where y is the node passed in. */
static void rotate_right(Root *root, SemaWaiter *y) {
    SemaWaiter *p = y->parent;
    SemaWaiter *x = y->prev;
    SemaWaiter *b = x->next;

    x->next = y;
    y->parent = x;
    y->prev = b;
    if (b != NULL)
        b->parent = y;

    x->parent = p;
    if (p == NULL)
        root->treap = x;
    else if (p->prev == y)
        p->prev = x;
    else if (p->next == y)
        p->next = x;
    else
        runtime_throw(
            BURROW_S("sema: rotate right on a node that is not its parent's child"));
}

/* Puts `w` on the queue for `addr`. The root's lock is held. */
static void root_queue(Root *root, uint32_t *addr, SemaWaiter *w, bool lifo) {
    w->addr = addr;
    w->prev = NULL;
    w->next = NULL;
    w->waitlink = NULL;
    w->waittail = NULL;

    SemaWaiter *last = NULL;
    SemaWaiter **pt = &root->treap;

    for (SemaWaiter *t = *pt; t != NULL; t = *pt) {
        if (t->addr == addr) {
            if (lifo) {
                /* Take t's place in the treap, keeping its priority so that the
                 * tree does not have to be rebalanced, and put t at the front of
                 * the list hanging off the new node. */
                *pt = w;
                w->ticket = t->ticket;
                w->parent = t->parent;
                w->prev = t->prev;
                w->next = t->next;
                if (w->prev != NULL)
                    w->prev->parent = w;
                if (w->next != NULL)
                    w->next->parent = w;

                w->waitlink = t;
                w->waittail = t->waittail != NULL ? t->waittail : t;

                t->parent = NULL;
                t->prev = NULL;
                t->next = NULL;
                t->waittail = NULL;
            } else {
                /* On the end of t's list, which leaves the treap alone. */
                if (t->waittail == NULL)
                    t->waitlink = w;
                else
                    t->waittail->waitlink = w;
                t->waittail = w;
            }
            return;
        }

        last = t;
        pt = (uintptr_t)addr < (uintptr_t)t->addr ? &t->prev : &t->next;
    }

    /* A new leaf, and then up until the priorities are a heap again. The low
     * bit is forced on so the priority cannot be zero, which is the value that
     * means "not in the treap" everywhere else in this file. */
    w->ticket = (uint32_t)runtime_rand64() | 1U;
    w->parent = last;
    *pt = w;

    while (w->parent != NULL && w->parent->ticket > w->ticket) {
        if (w->parent->prev == w)
            rotate_right(root, w->parent);
        else
            rotate_left(root, w->parent);
    }
}

/* Takes the longest waiting waiter for `addr` off the queue, or NULL if there
 * is none. The root's lock is held. */
static SemaWaiter *root_dequeue(Root *root, const uint32_t *addr) {
    SemaWaiter **ps = &root->treap;
    SemaWaiter *w = *ps;

    while (w != NULL && w->addr != addr) {
        ps = (uintptr_t)addr < (uintptr_t)w->addr ? &w->prev : &w->next;
        w = *ps;
    }
    if (w == NULL)
        return NULL;

    SemaWaiter *t = w->waitlink;
    if (t != NULL) {
        /* Somebody else is waiting on this address, so they take the treap node
         * and its priority and the tree is untouched. */
        *ps = t;
        t->ticket = w->ticket;
        t->parent = w->parent;
        t->prev = w->prev;
        t->next = w->next;
        if (t->prev != NULL)
            t->prev->parent = t;
        if (t->next != NULL)
            t->next->parent = t;
        t->waittail = t->waitlink != NULL ? w->waittail : NULL;
    } else {
        /* Nobody else. Rotate it down to a leaf, taking the lower priority
         * child up each time so the heap survives, and then unhook it. */
        while (w->next != NULL || w->prev != NULL) {
            if (w->next == NULL ||
                (w->prev != NULL && w->prev->ticket < w->next->ticket))
                rotate_right(root, w);
            else
                rotate_left(root, w);
        }
        if (w->parent == NULL)
            root->treap = NULL;
        else if (w->parent->prev == w)
            w->parent->prev = NULL;
        else
            w->parent->next = NULL;
    }

    w->parent = NULL;
    w->prev = NULL;
    w->next = NULL;
    w->waitlink = NULL;
    w->waittail = NULL;
    w->ticket = 0;
    return w;
}

/* --------------------------------------------------------- stopping and going
 *
 * The two functions where a goroutine and a thread differ, and the reason the
 * rest of this file does not have to know which it is holding. */

static bool unlock_lock(Goroutine *g, void *p) {
    (void)g;
    burrow__unlock((burrow__Lock *)p);
    return true;
}

/* Gets a waiter ready to block. False means this thread cannot block at all,
 * which is a note that could not be allocated and so is an out of memory
 * condition on a path with nothing useful to do about it. */
static bool waiter_init(SemaWaiter *w) {
    memset(w, 0, sizeof(*w));

    w->g = sched_current();
    if (w->g != NULL)
        return true;

    /* Transient, because this note lives in the frame of the thread that is
     * about to block and is freed the moment the wait is over. */
    w->has_note = burrow__note_init_transient(&w->note);
    return w->has_note;
}

/* Blocks until somebody takes this waiter off whatever queue it is on and wakes
 * it. `lk` is the lock guarding that queue, held on the way in and not held on
 * the way out. `durable` is the caller's answer to the synctest question, and a
 * thread has no opinion about it: a bubble is a set of goroutines, so a thread
 * blocking here is not in one and the flag has nowhere to go. */
static void waiter_sleep(SemaWaiter *w, burrow__Lock *lk, bool durable) {
    if (w->g != NULL) {
        burrow__park(unlock_lock, lk, durable);
        return;
    }

    /* A thread has no window to close between the unlock and the sleep, because
     * a note is allowed to be opened before anybody is standing at it. So it
     * unlocks out in the open and a wake that lands in between leaves the gate
     * open and the sleep returns at once. */
    burrow__unlock(lk);
    burrow__note_sleep(&w->note);
}

/* Gets the waiter ready to be queued a second time, which happens when the
 * wakeup it was given got taken by somebody who never queued at all.
 *
 * Clearing is safe here and nowhere else: the waiter is off the queue, so
 * nobody can be about to wake it, and the wake it is recovering from has
 * already been seen. */
static void waiter_reset(SemaWaiter *w) {
    if (w->has_note)
        burrow__note_clear(&w->note);
}

static void waiter_free(SemaWaiter *w) {
    if (w->has_note)
        burrow__note_free(&w->note);
}

/* Starts a waiter again. The root's lock must not be held, and the waiter must
 * not be touched afterwards. */
static void waiter_wake(SemaWaiter *w) {
    if (w->g != NULL) {
        sched_ready(w->g);
        return;
    }
    burrow__note_wake(&w->note);
}

/* ------------------------------------------------------------- the operations */

/* Takes one wakeup off the counter if there is one there. */
static bool can_acquire(uint32_t *addr) {
    for (;;) {
        uint32_t v = burrow__atomic_load_u32(addr);
        if (v == 0)
            return false;
        if (burrow__atomic_cas_u32(addr, &v, v - 1))
            return true;
    }
}

void burrow__sema_acquire(uint32_t *addr, bool lifo, bool durable) {
    /* The whole of the uncontended case. No lock, no queue, one compare and
     * swap on a word the caller already owns. */
    if (can_acquire(addr))
        return;

    SemaWaiter w;
    if (!waiter_init(&w))
        runtime_throw(BURROW_S("sema: out of memory blocking on a semaphore"));

    Root *root = root_for(addr);
    bool first = true;

    for (;;) {
        if (!first)
            waiter_reset(&w);
        first = false;

        burrow__lock(&root->lock);

        /* Count this waiter before looking again, so that a release landing
         * between the look and the queueing sees that somebody is here and
         * takes the slow path rather than leaving the wakeup on the counter for
         * a waiter that is about to go to sleep. */
        burrow__atomic_add_u32(&root->nwait, 1);

        if (can_acquire(addr)) {
            burrow__atomic_add_u32(&root->nwait, (uint32_t)-1);
            burrow__unlock(&root->lock);
            break;
        }

        root_queue(root, addr, &w, lifo);
        waiter_sleep(&w, &root->lock, durable);

        /* A ticket means the release handed the wakeup straight over and there
         * is nothing left to take. Otherwise the wakeup went back on the
         * counter and this is a race with everybody else for it, which is
         * exactly what Go's non handoff release means. */
        if (w.ticket != 0 || can_acquire(addr))
            break;
    }

    /* The waiter is on this frame and it was on a list in the global semtable a
     * moment ago, which is what the analyser is objecting to. It is off that
     * list on both ways out of the loop: the first break never queued on this
     * turn, and the second one is reached only after waiter_sleep returned,
     * which happens when a release has already dequeued this waiter to wake it.
     * The analyser cannot follow that, because the dequeue is in another
     * function and the wakeup that orders the two is a note. */
    /* NOLINTNEXTLINE(clang-analyzer-core.StackAddressEscape) */
    waiter_free(&w);
}

void burrow__sema_release(uint32_t *addr, bool handoff) {
    Root *root = root_for(addr);

    burrow__atomic_add_u32(addr, 1);

    /* The whole of the uncontended case again. Reading the count without the
     * lock is what makes an unlock nobody is waiting for two atomics and no
     * more, and it is safe because a waiter counts itself before it looks at
     * the counter, so a waiter this misses is a waiter who has not yet made the
     * decision that this would have changed. */
    if (burrow__atomic_load_u32(&root->nwait) == 0)
        return;

    burrow__lock(&root->lock);
    if (burrow__atomic_load_u32(&root->nwait) == 0) {
        /* Somebody else got there in between and has taken the waiter this was
         * going to wake, which means the wakeup this call added is already
         * accounted for. */
        burrow__unlock(&root->lock);
        return;
    }

    SemaWaiter *w = root_dequeue(root, addr);
    if (w != NULL)
        burrow__atomic_add_u32(&root->nwait, (uint32_t)-1);
    burrow__unlock(&root->lock);

    if (w == NULL)
        return;

    /* Read out of the waiter before waking it, because after the wake the frame
     * it lives in belongs to somebody who may already be running in it. */
    bool given = false;
    if (handoff && can_acquire(addr)) {
        w->ticket = 1;
        given = true;
    }
    waiter_wake(w);

    /* A handoff that does not give up the processor is not a handoff. The
     * waiter has the lock and this goroutine is still running, so without this
     * the two of them race and the one that was woken loses about as often as
     * it wins, which is the behaviour the handoff exists to stop. */
    if (given && sched_current() != NULL)
        runtime_gosched();
}

/* --------------------------------------------------------- the notify list
 *
 * The queue a sync.Cond waits on. See the long comment in burrow/sema.h for
 * why it is not the semaphore above.
 *
 * Derived from Go's notifyList in src/runtime/sema.go. */

/* Ticket comparison, which has to allow for the counters wrapping. Tickets are
 * handed out forever and a program that takes four billion of them starts again
 * at zero, so the question is never "is a smaller than b" but "is a behind b",
 * and the signed difference answers that for any two tickets less than two
 * billion apart. Go's `less`. */
static bool ticket_before(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

uint32_t burrow__notify_list_add(burrow__NotifyList *l) {
    /* The add returns the value before it, which is this caller's ticket. No
     * lock: taking a number is the only thing that happens here, and the order
     * the numbers come out in is the order the waiters will be woken in. */
    return burrow__atomic_add_u32(&l->wait, 1);
}

void burrow__notify_list_wait(burrow__NotifyList *l, uint32_t t, bool durable) {
    burrow__lock(&l->lock);

    /* Already notified, between the ticket being taken and this lock. This is
     * the case the ticket exists for, and getting it wrong is the lost wakeup
     * that makes a Cond hang. */
    if (ticket_before(t, burrow__atomic_load_u32(&l->notify))) {
        burrow__unlock(&l->lock);
        return;
    }

    SemaWaiter w;
    if (!waiter_init(&w))
        runtime_throw(BURROW_S("sema: out of memory waiting on a condition"));
    w.ticket = t;

    /* On the end, which is where the tickets say this one belongs. Two waiters
     * can take their numbers and then reach this lock in the other order, so
     * the list is very nearly sorted rather than sorted, which is a thing the
     * notify of a single ticket below has to know about. */
    if (l->tail == NULL)
        l->head = &w;
    else
        l->tail->waitlink = &w;
    l->tail = &w;

    waiter_sleep(&w, &l->lock, durable);
    waiter_free(&w);
}

void burrow__notify_list_notify_all(burrow__NotifyList *l) {
    /* Nobody has taken a ticket since the last notify, so there is nobody to
     * wake and no reason to touch the lock. This is a Broadcast on an idle Cond
     * and it costs two loads. */
    if (burrow__atomic_load_u32(&l->wait) == burrow__atomic_load_u32(&l->notify))
        return;

    burrow__lock(&l->lock);
    SemaWaiter *w = l->head;
    l->head = NULL;
    l->tail = NULL;

    /* Everybody up to the current `wait` counts as notified. The ones already
     * on the list are about to be woken, and the ones that have a ticket but
     * have not reached the lock yet will see this and not sleep at all. */
    burrow__atomic_store_u32(&l->notify, burrow__atomic_load_u32(&l->wait));
    burrow__unlock(&l->lock);

    /* Outside the lock, because waking a goroutine can run the scheduler and
     * holding a runtime lock across that is how a deadlock is built. */
    while (w != NULL) {
        SemaWaiter *next = w->waitlink;
        w->waitlink = NULL;
        waiter_wake(w);
        w = next;
    }
}

void burrow__notify_list_notify_one(burrow__NotifyList *l) {
    if (burrow__atomic_load_u32(&l->wait) == burrow__atomic_load_u32(&l->notify))
        return;

    burrow__lock(&l->lock);

    uint32_t t = burrow__atomic_load_u32(&l->notify);
    if (t == burrow__atomic_load_u32(&l->wait)) {
        burrow__unlock(&l->lock);
        return;
    }
    burrow__atomic_store_u32(&l->notify, t + 1);

    /* Find the waiter holding that ticket. It may not be on the list yet, in
     * which case there is nothing to do here: it is between taking its number
     * and reaching the lock, and the store above means it will find itself
     * already notified and go straight through.
     *
     * The scan looks linear and is not, in practice. A waiter is only behind
     * others in the list because it lost the race between taking a number and
     * queueing, so the one being looked for is at or near the front. */
    SemaWaiter *prev = NULL;
    for (SemaWaiter *w = l->head; w != NULL; prev = w, w = w->waitlink) {
        if (w->ticket != t)
            continue;

        SemaWaiter *next = w->waitlink;
        if (prev != NULL)
            prev->waitlink = next;
        else
            l->head = next;
        if (next == NULL)
            l->tail = prev;

        burrow__unlock(&l->lock);
        w->waitlink = NULL;
        waiter_wake(w);
        return;
    }

    burrow__unlock(&l->lock);
}
