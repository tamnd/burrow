/* sync.Pool. See burrow/sync.h.
 *
 * Two pieces, the same two Go has. Underneath is a ring buffer with one
 * producer and many consumers: the P that owns it pushes and pops at the head
 * and anybody at all may steal from the tail, which is what makes a Put and a
 * Get that stay on one P touch nothing another core is looking at. On top of
 * that is a chain of those rings, each twice the size of the one before, so a
 * queue that fills grows instead of dropping work.
 *
 * What is not Go's is the giving back. Go empties a pool by dropping the array
 * of per P slots and letting the collector take the whole thing, rings and
 * pooled objects together. Here the pooled objects go to the free function the
 * caller supplied, and the rings are emptied and kept rather than freed, which
 * is the one decision in this file worth reading twice.
 *
 * Keeping them is what makes the sweep safe to run with the program going. A
 * sweep that freed the rings would be freeing memory another P might be halfway
 * through stealing from, and the fix for that is either stopping the world,
 * which burrow cannot do, or a reclamation pin on the steal path, which would
 * cost every Get a sequentially consistent store. Emptying a ring is a pop from
 * the tail, which is the operation a thief already performs, so it needs
 * nothing at all. The price is that the rings stay at their high water mark
 * until sync_pool_free, and a ring is two words and its slots, so a pool that
 * once held a thousand objects keeps about sixteen kilobytes and not the
 * thousand objects.
 *
 * Derived from Go's src/sync/pool.go and src/sync/poolqueue.go.
 * Go source: go1.27.1.
 *
 * Copyright 2013 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/sync.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/iface.h"
#include "burrow/lock.h"
#include "burrow/mem.h"
#include "burrow/sched.h"
#include "burrow/sync/atomic.h"
#include "burrow/thread.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Head and tail live in one word so that a steal can move the tail and see the
 * head in the same compare and swap. Go packs them the same way and for the
 * same reason. Both halves are free running and masked, so a ring is empty when
 * they are equal and full when they are a length apart, and neither of those
 * needs a count of its own.
 *
 * The limit is a quarter of what the half word holds. Go's number, and the
 * reason is that head and tail have to stay less than half the range apart for
 * the wrap arithmetic to keep saying which came first, and a quarter leaves
 * room to spare. */
#define HT_BITS 32
#define HT_MASK 0xffffffffU
#define RING_LIMIT (1U << 30)
#define RING_INIT 8

/* An Any in a ring slot, with the descriptor kept as a word.
 *
 * It is a word because the descriptor pointer is the field a thief clears to
 * say that it has finished with the slot, so it has to be written atomically,
 * and Any holds its descriptor as a const pointer. Taking the const off to get
 * at the pointer atomics is a cast that every warning worth having complains
 * about and should. Going through a word instead is the same machine code with
 * nothing to apologise for, and the const goes back on as the value leaves. */
typedef struct PoolSlot {
    uintptr_t t;
    void *data;
} PoolSlot;

static PoolSlot slot_of_any(Any v) {
    return (PoolSlot){(uintptr_t)v.t, v.data};
}

static Any any_of_slot(PoolSlot s) {
    return (Any){(const Type *)s.t, s.data};
}

/* One ring, and its place in the chain.
 *
 * `next` and `prev` are read by threads other than the owner and so are atomic.
 * `allnext` is not: it is the list every ring ever made stays on so that
 * sync_pool_free can find them, it is only ever appended to by the owner, and
 * the only thing that reads it is sync_pool_free, which runs when nothing else
 * is touching the pool. The reason it has to exist at all is that a chain drops
 * rings off its own tail as they drain and nils the link back to them, so by
 * the end the chain no longer knows about most of what it allocated. */
typedef struct Ring Ring;
struct Ring {
    uint64_t headtail;
    Ring *next;
    Ring *prev;
    Ring *allnext;
    uint32_t n;
    PoolSlot vals[];
};

/* A chain of rings, newest at the head.
 *
 * `head` and `first` are the owner's and nobody else reads them. `tail` is read
 * by thieves and by the sweep, so it is atomic.
 *
 * `first` is not Go's. Go has nothing to free and so keeps no such pointer, and
 * a chain on its own forgets the rings it has drained, because draining one is
 * what nils the link back to it. */
typedef struct Chain {
    Ring *head;
    Ring *tail;
    Ring *first;
} Chain;

/* What one P gets. The private slot is the fast path and the chain is the
 * overflow, which is exactly Go's split: a Get that finds something in private
 * is a load and a store and no atomic anywhere.
 *
 * Padded, because two Ps whose slots share a cache line are two Ps writing to
 * the same line on every Put, and the whole point of a slot per P is that they
 * do not. */
typedef struct Local {
    _Alignas(BURROW_CACHELINE) Any priv;
    uint32_t epoch;
    Chain shared;
} Local;

/* One generation of per P slots. Two of these per pool, and a sweep swaps which
 * is which rather than allocating, so nothing in the steady state allocates and
 * nothing has to be freed while other threads can still see it.
 *
 * `n` is gomaxprocs plus one. The extra slot on the end is for a thread that
 * walked in from outside the scheduler and so has no P to be indexed by. Those
 * threads take the pool's mutex and share the slot, which serialises them
 * against each other and leaves the real Ps alone. Go has no such case, because
 * in Go there is nothing that is not a goroutine. */
typedef struct Shard {
    uint32_t n;

    /* Bumped every time this shard is emptied, and compared against the copy in
     * each Local. It is how the private slots get emptied without the sweep
     * having to touch them.
     *
     * A private slot is the fast path and it is the one place in here that has
     * no atomic in it at all, which is only true because exactly one thread
     * ever reads or writes one. The sweep is a second thread, so the sweep does
     * not go near them: it empties the rings, moves this on by one, and leaves
     * the private slots to be noticed by whoever owns them, on their next Get
     * or Put, which is a load and a compare against a line every P has in
     * shared state already.
     *
     * The cost of doing it this way is that a P which stops using a pool keeps
     * whatever was in its private slot. That is at most one object per P per
     * shard, so two per P for the pool, which is the same order as the pool
     * being warm at all, and sync_pool_free takes them. */
    uint32_t epoch;

    Local locals[];
} Shard;

/* The registry, and the lock over it.
 *
 * Every pool that has been used is on this list until sync_pool_free takes it
 * off, and the sweep walks it. The lock is the runtime's rather than a
 * SyncMutex because the sweep runs on the system monitor's thread, which is not
 * an M and has no P, so it has nothing to park. */
static burrow__Lock pools_lock;
static SyncPool *allpools;

/* ------------------------------------------------------------------ the ring
 *
 * Go's poolDequeue, with its comments left where they were earned. */

static uint64_t ht_pack(uint32_t head, uint32_t tail) {
    return ((uint64_t)head << HT_BITS) | (uint64_t)tail;
}

static uint32_t ht_head(uint64_t ht) {
    return (uint32_t)((ht >> HT_BITS) & HT_MASK);
}

static uint32_t ht_tail(uint64_t ht) {
    return (uint32_t)(ht & HT_MASK);
}

static Ring *ring_new(Alloc *a, uint32_t n) {
    Ring *r = (Ring *)mem_alloc(a, sizeof(Ring) + (size_t)n * sizeof(PoolSlot),
                                _Alignof(Ring));
    if (r == NULL)
        return NULL;
    r->n = n;
    return r;
}

/* Push at the head. Only the owner calls this.
 *
 * The two ways this says no are worth telling apart. The first is a ring whose
 * head has caught up with its tail, which is a ring that is honestly full. The
 * second is a slot whose type word is still set, which means a thief took the
 * value out of it and has not yet said so, and for a moment longer the ring is
 * still full. Go checks both and so does this. */
static bool ring_push(Ring *r, Any v) {
    uint64_t ht = burrow__atomic_load_u64(&r->headtail);
    uint32_t head = ht_head(ht);
    uint32_t tail = ht_tail(ht);

    if (((tail + r->n) & HT_MASK) == head)
        return false;

    PoolSlot *slot = &r->vals[head & (r->n - 1)];
    if (burrow__atomic_load_acquire_uptr(&slot->t) != 0)
        return false;

    /* Written plainly, published by the add below. A consumer reads headtail
     * before it reads any slot, so the add is what orders this against it. */
    PoolSlot w = slot_of_any(v);
    slot->data = w.data;
    burrow__atomic_store_release_uptr(&slot->t, w.t);

    (void)burrow__atomic_add_u64(&r->headtail, (uint64_t)1 << HT_BITS);
    return true;
}

/* Pop at the head, which is the most recently pushed value and so the one most
 * likely to still be in this core's cache. Only the owner calls this.
 *
 * The slot is cleared outright rather than released the way ring_steal does,
 * because the owner is the only one who can push into it and the owner is here.
 * Go makes the same distinction between its popHead and its popTail. */
static bool ring_pop(Ring *r, Any *out) {
    uint64_t ht = burrow__atomic_load_u64(&r->headtail);

    for (;;) {
        uint32_t head = ht_head(ht);
        uint32_t tail = ht_tail(ht);
        if (head == tail)
            return false;

        head--;
        if (burrow__atomic_cas_u64(&r->headtail, &ht, ht_pack(head, tail))) {
            PoolSlot *slot = &r->vals[head & (r->n - 1)];
            *out = any_of_slot(*slot);
            slot->t = 0;
            slot->data = NULL;
            return true;
        }
    }
}

/* Pop at the tail, which is the oldest value. Anybody may call this, including
 * several threads at once and including the owner.
 *
 * Clearing the type word last and with a release store is what hands the slot
 * back to the owner, and it has to come after the value has been copied out.
 * Doing it the other way round lets the owner push a new value into a slot this
 * thread has not finished reading. */
static bool ring_steal(Ring *r, Any *out) {
    uint64_t ht = burrow__atomic_load_u64(&r->headtail);

    for (;;) {
        uint32_t head = ht_head(ht);
        uint32_t tail = ht_tail(ht);
        if (head == tail)
            return false;

        if (burrow__atomic_cas_u64(&r->headtail, &ht, ht_pack(head, tail + 1))) {
            PoolSlot *slot = &r->vals[tail & (r->n - 1)];
            *out = any_of_slot(*slot);
            slot->data = NULL;
            burrow__atomic_store_release_uptr(&slot->t, 0);
            return true;
        }
    }
}

/* ----------------------------------------------------------------- the chain
 *
 * Go's poolChain. A ring that fills gets a bigger one in front of it rather
 * than dropping the value, and a ring that drains falls off the back. */

static void chain_push(Chain *c, Alloc *a, Any v, SyncPoolFreeFunc free_fn) {
    Ring *d = c->head;

    if (d == NULL) {
        d = ring_new(a, RING_INIT);
        if (d == NULL) {
            if (free_fn.f != NULL)
                BURROW_CALLF(free_fn, v);
            return;
        }
        c->head = d;
        c->first = d;
        burrow__atomic_store_release_ptr((void **)&c->tail, d);
    }

    if (ring_push(d, v))
        return;

    uint32_t n = d->n * 2;
    if (n >= RING_LIMIT)
        n = RING_LIMIT;

    Ring *d2 = ring_new(a, n);
    if (d2 == NULL) {
        /* Out of memory is not a reason to lose an object the caller is trying
         * to give back, and there is nowhere left to put it, so it goes where
         * it would have gone at the next sweep. Go cannot get here. */
        if (free_fn.f != NULL)
            BURROW_CALLF(free_fn, v);
        return;
    }

    d2->prev = d;
    d2->allnext = c->first;
    c->first = d2;
    c->head = d2;
    burrow__atomic_store_release_ptr((void **)&d->next, d2);
    (void)ring_push(d2, v);
}

static bool chain_pop(Chain *c, Any *out) {
    for (Ring *d = c->head; d != NULL;
         d = (Ring *)burrow__atomic_load_acquire_ptr((void *const *)&d->prev)) {
        if (ring_pop(d, out))
            return true;
    }
    return false;
}

static bool chain_steal(Chain *c, Any *out) {
    Ring *d = (Ring *)burrow__atomic_load_acquire_ptr((void *const *)&c->tail);
    if (d == NULL)
        return false;

    for (;;) {
        /* Read the next ring before trying this one. The other order is a race:
         * a steal that fails because this ring just emptied would then read a
         * next that is still nil and give up, with the value it wanted sitting
         * in the ring that was about to be linked on. Go's comment says the
         * same thing in fewer words. */
        Ring *d2 = (Ring *)burrow__atomic_load_acquire_ptr((void *const *)&d->next);

        if (ring_steal(d, out))
            return true;
        if (d2 == NULL)
            return false;

        /* This ring is drained and there is another behind it, so move the tail
         * along. Losing the race is fine: whoever won moved it to the same
         * place. Nilling the back link is what stops chain_pop walking over
         * rings that will never hold anything again. */
        Ring *expect = d;
        if (burrow__atomic_cas_release_ptr((void **)&c->tail, (void **)&expect, d2))
            burrow__atomic_store_release_ptr((void **)&d2->prev, NULL);

        d = d2;
    }
}

/* ------------------------------------------------------------------ the pool
 */

static Shard *shard_new(Alloc *a, uint32_t n) {
    Shard *s = (Shard *)mem_alloc(a, sizeof(Shard) + (size_t)n * sizeof(Local),
                                  _Alignof(Shard));
    if (s == NULL)
        return NULL;
    s->n = n;
    return s;
}

/* Build the two generations, once, the first time anybody uses the pool.
 *
 * The registration at the end is what puts this pool in front of the sweep, and
 * the burrow__set_sweep beside it is what starts the sweep happening at all. It
 * is set every time rather than once because setting it is a store of a pointer
 * that is always the same pointer, which is cheaper than the flag it would take
 * to skip it. */
static bool pool_init(SyncPool *p) {
    sync_mutex_lock(&p->mu);

    if (sync_atomic_uint32_load(&p->inited) != 0) {
        sync_mutex_unlock(&p->mu);
        return true;
    }

    /* Big enough for every P there is going to be. Inside a goroutine, which is
     * where a pool is first touched in any normal program, gomaxprocs is
     * already final and this is exactly it. Outside one it is zero, because the
     * scheduler has not started and has not decided yet, so the processor count
     * stands in for it, which is what the scheduler will pick unless the
     * program says otherwise.
     *
     * An id past the end is not a correctness problem, because slot_of sends a
     * P it cannot index to the shared slot the same way it sends a thread with
     * no P at all. It is a slow pool, so it is worth not getting wrong. */
    int32_t procs = burrow__gomaxprocs();
    int32_t ncpu = burrow__thread_ncpu();
    if (procs < ncpu)
        procs = ncpu;
    if (procs < 1)
        procs = 1;
    uint32_t n = (uint32_t)procs + 1;

    Shard *a = shard_new(p->a, n);
    Shard *b = shard_new(p->a, n);
    if (a == NULL || b == NULL) {
        if (a != NULL)
            mem_free(p->a, a, sizeof(Shard) + (size_t)n * sizeof(Local),
                     _Alignof(Shard));
        if (b != NULL)
            mem_free(p->a, b, sizeof(Shard) + (size_t)n * sizeof(Local),
                     _Alignof(Shard));
        sync_mutex_unlock(&p->mu);
        return false;
    }

    p->shard[0] = a;
    p->shard[1] = b;
    burrow__atomic_store_release_ptr(&p->live, a);
    burrow__atomic_store_release_ptr(&p->victim, b);

    burrow__lock(&pools_lock);
    p->allnext = allpools;
    allpools = p;
    burrow__unlock(&pools_lock);

    burrow__set_sweep(burrow__pool_sweep);

    sync_atomic_uint32_store(&p->inited, 1);
    sync_mutex_unlock(&p->mu);
    return true;
}

/* Empty this slot's private object if a sweep has been past since it was put
 * there, which is the deferred half of what the sweep would otherwise have to
 * do itself. Owner only, like everything else that touches a private slot.
 *
 * The load of the shard's counter is the whole cost of this on the fast path.
 * It is read by every P and written once a second, so it sits in shared state
 * in every cache and the compare falls out of the load. */
static void catch_up(SyncPool *p, Shard *s, Local *l) {
    uint32_t e = burrow__atomic_load_acquire_u32(&s->epoch);
    if (l->epoch == e)
        return;

    Any v = l->priv;
    l->priv = (Any){NULL, NULL};
    l->epoch = e;

    if (!BURROW_ANY_IS_NIL(v) && p->free_fn.f != NULL)
        BURROW_CALLF(p->free_fn, v);
}

/* Which slot this thread gets, and whether it had to take the mutex to get it.
 *
 * A goroutine gets its P's slot and nothing more happens, where Go has to pin
 * the P with procPin and unpin it afterwards. burrow does not, and the reason
 * is the shape of its preemption rather than the absence of it: a goroutine
 * here gives way only at a safe point, and there is no safe point between this
 * returning and the end of a Get or a Put. The path is atomics and at most one
 * uncontended mutex, none of which asks. If burrow ever gets preemption that
 * can move a goroutine anywhere, as Go's signal based version can, this becomes
 * procPin on the same day.
 *
 * One hole, and it is older than preemption. catch_up below calls the pool's
 * free function, which is somebody else's code, and if that code blocks then
 * the goroutine can come back on a different P with this slot already handed to
 * another one. A free function that blocks was never safe here. See issue 101.
 *
 * A thread with no P gets the slot on the end under the pool's mutex. That slot
 * behaves exactly like a P's, it just has more than one owner taking turns. */
static Local *slot_of(SyncPool *p, Shard *s, bool *locked) {
    burrow__M *m = burrow__curm();
    burrow__P *proc = m != NULL ? m->p : NULL;
    Local *l;

    if (proc != NULL && (uint32_t)proc->id < s->n - 1) {
        *locked = false;
        l = &s->locals[proc->id];
    } else {
        sync_mutex_lock(&p->mu);
        *locked = true;
        l = &s->locals[s->n - 1];
    }

    catch_up(p, s, l);
    return l;
}

void sync_pool_put(SyncPool *p, Any v) {
    if (BURROW_ANY_IS_NIL(v))
        return;

    if (sync_atomic_uint32_load(&p->inited) == 0 && !pool_init(p)) {
        if (p->free_fn.f != NULL)
            BURROW_CALLF(p->free_fn, v);
        return;
    }

    Shard *s = (Shard *)burrow__atomic_load_acquire_ptr(&p->live);
    bool locked = false;
    Local *l = slot_of(p, s, &locked);

    if (BURROW_ANY_IS_NIL(l->priv))
        l->priv = v;
    else
        chain_push(&l->shared, p->a, v, p->free_fn);

    if (locked)
        sync_mutex_unlock(&p->mu);
}

/* The slow half of a Get: this P's own queue was empty, so try everybody else's
 * and then the generation behind.
 *
 * Stealing goes round the other Ps in order starting from the one after this
 * one, which is Go's order and spreads the first steal of a burst out instead
 * of sending every P at P zero. The private slots of the other Ps are left
 * alone, because they belong to whoever is running on them.
 *
 * The victim is read last and read whole, this P's private slot included, which
 * is safe for the reason the private slot is always safe: the only thread that
 * ever touches the slot at this index is this P. */
static bool get_slow(SyncPool *p, Shard *s, uint32_t self, Any *out) {
    for (uint32_t i = 1; i < s->n; i++) {
        Local *l = &s->locals[(self + i) % s->n];
        if (chain_steal(&l->shared, out))
            return true;
    }

    Shard *v = (Shard *)burrow__atomic_load_acquire_ptr(&p->victim);
    if (v == NULL || v == s)
        return false;

    if (self < v->n) {
        Local *l = &v->locals[self];
        catch_up(p, v, l);
        if (!BURROW_ANY_IS_NIL(l->priv)) {
            *out = l->priv;
            l->priv = (Any){NULL, NULL};
            return true;
        }
    }

    for (uint32_t i = 0; i < v->n; i++) {
        if (chain_steal(&v->locals[i].shared, out))
            return true;
    }
    return false;
}

Any sync_pool_get(SyncPool *p) {
    Any v = {NULL, NULL};

    if (sync_atomic_uint32_load(&p->inited) == 0 && !pool_init(p))
        return p->new_fn.f != NULL ? BURROW_CALLF0(p->new_fn) : v;

    Shard *s = (Shard *)burrow__atomic_load_acquire_ptr(&p->live);
    bool locked = false;
    Local *l = slot_of(p, s, &locked);
    uint32_t self = (uint32_t)(l - s->locals);

    if (!BURROW_ANY_IS_NIL(l->priv)) {
        v = l->priv;
        l->priv = (Any){NULL, NULL};
    } else if (!chain_pop(&l->shared, &v)) {
        (void)get_slow(p, s, self, &v);
    }

    if (locked)
        sync_mutex_unlock(&p->mu);

    if (BURROW_ANY_IS_NIL(v) && p->new_fn.f != NULL)
        v = BURROW_CALLF0(p->new_fn);
    return v;
}

/* Empty one generation, handing everything in it to the free function.
 *
 * Every ring is emptied through the same pop from the tail a thief uses, which
 * is what makes this safe to run while the program is going: a ring already
 * allows any number of consumers at once and this is one more of them.
 *
 * The private slots are not touched at all. They belong to one P each and are
 * the one thing in here read and written with no atomic, so the sweep leaves
 * them and moves the shard's counter on instead, and their owners empty them
 * the next time they call in. See catch_up.
 *
 * Nothing is freed. The rings stay where they are, empty, for the next
 * generation that lands on this shard to reuse. */
static void shard_drain(Shard *s, SyncPoolFreeFunc free_fn) {
    if (s == NULL)
        return;

    for (uint32_t i = 0; i < s->n; i++) {
        Any v;
        while (chain_steal(&s->locals[i].shared, &v)) {
            if (free_fn.f != NULL)
                BURROW_CALLF(free_fn, v);
        }
    }

    /* Last, so that a P which reads this and then looks at its private slot
     * cannot find a slot that is about to be emptied underneath it. There is
     * nothing to synchronise with on the other side beyond the release here and
     * the acquire in catch_up, because the slot itself is that P's alone. */
    burrow__atomic_store_u32(&s->epoch, burrow__atomic_load_acquire_u32(&s->epoch) + 1);
}

/* One pool's sweep. Go's two generations, kept exactly.
 *
 * The victim is what was live one sweep ago and has not been asked for since,
 * so it goes. What is live now becomes the victim, and the shard the old victim
 * was using, which is empty and still has its rings, becomes the new live one.
 *
 * The two stores are not one atomic step and do not need to be. A Get that
 * reads the old live pointer and then the new victim pointer reads the same
 * shard twice, which get_slow notices and skips. A Get that reads the new live
 * pointer finds an empty shard and goes to the victim, which is where
 * everything it wanted now is. Neither loses an object and neither hands one
 * out twice. */
static void pool_sweep(SyncPool *p) {
    Shard *live = (Shard *)burrow__atomic_load_acquire_ptr(&p->live);
    Shard *victim = (Shard *)burrow__atomic_load_acquire_ptr(&p->victim);

    shard_drain(victim, p->free_fn);
    burrow__atomic_store_release_ptr(&p->victim, live);
    burrow__atomic_store_release_ptr(&p->live, victim);
}

void burrow__pool_sweep(void) {
    burrow__lock(&pools_lock);
    for (SyncPool *p = allpools; p != NULL; p = p->allnext)
        pool_sweep(p);
    burrow__unlock(&pools_lock);
}

static void shard_free(Alloc *a, Shard *s, SyncPoolFreeFunc free_fn) {
    if (s == NULL)
        return;

    shard_drain(s, free_fn);

    /* The private slots go here and not in shard_drain. A sweep leaves them to
     * their owners because it runs while the program does, but this does not:
     * sync_pool_free is the caller saying it is finished with the pool, so
     * there is nobody left to come back and empty them lazily. */
    for (uint32_t i = 0; i < s->n; i++) {
        Any v = s->locals[i].priv;
        s->locals[i].priv = (Any){NULL, NULL};

        if (!BURROW_ANY_IS_NIL(v) && free_fn.f != NULL)
            BURROW_CALLF(free_fn, v);
    }

    for (uint32_t i = 0; i < s->n; i++) {
        Ring *r = s->locals[i].shared.first;
        while (r != NULL) {
            Ring *next = r->allnext;
            mem_free(a, r, sizeof(Ring) + (size_t)r->n * sizeof(PoolSlot),
                     _Alignof(Ring));
            r = next;
        }
    }

    mem_free(a, s, sizeof(Shard) + (size_t)s->n * sizeof(Local), _Alignof(Shard));
}

void sync_pool_free(SyncPool *p) {
    if (sync_atomic_uint32_load(&p->inited) == 0)
        return;

    burrow__lock(&pools_lock);
    SyncPool **link = &allpools;
    while (*link != NULL) {
        if (*link == p) {
            *link = p->allnext;
            break;
        }
        link = &(*link)->allnext;
    }
    burrow__unlock(&pools_lock);

    shard_free(p->a, (Shard *)p->shard[0], p->free_fn);
    shard_free(p->a, (Shard *)p->shard[1], p->free_fn);

    p->shard[0] = NULL;
    p->shard[1] = NULL;
    p->live = NULL;
    p->victim = NULL;
    p->allnext = NULL;
    sync_atomic_uint32_store(&p->inited, 0);
}
