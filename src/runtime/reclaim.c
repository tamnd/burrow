/* Epoch based reclamation. The design and the rules are in
 * include/burrow/reclaim.h and are not repeated here.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/reclaim.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/lock.h"
#include "burrow/platform.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"
#include "burrow/thread.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Two halves, and which half a thread gets is the only thing that differs
 * between a goroutine and a thread that walked in from outside.
 *
 * The lower half is indexed by M, so a goroutine picks its slot with an array
 * index and never competes for one. The upper half is a pool that a foreign
 * thread takes from on the way into a pin and gives back on the way out, which
 * costs a compare and swap on each side and is the price of not knowing how
 * many such threads there will ever be. Handing them a permanent slot instead
 * would be faster and would also leak a slot per thread that exits, which for a
 * program that makes threads in a loop is a program that eventually stops
 * working. */
#define MSLOTS BURROW_MAXPROCS
#define FSLOTS BURROW_MAXPROCS
#define NSLOTS (MSLOTS + FSLOTS)

/* How many objects one thread holds on to before it pushes them to the shared
 * list and has a go at moving the epoch on.
 *
 * The whole cost of a collection is one walk of the participant slots, so this
 * number is how far that walk is amortised. Too small and every delete pays for
 * a walk. Too large and memory sits in a thread's pocket. Go's own sweeper
 * works in spans of a few dozen for the same reason. */
#define BATCH 64

typedef struct Slot {
    /* Zero when this slot is not pinned. Otherwise the epoch it was pinned at,
     * shifted up one, with the low bit set. One word so that the walk reads one
     * thing per participant, and on its own cache line so that the walk does
     * not fight the pins it is reading. */
    _Alignas(BURROW_CACHELINE) uint32_t state;
} Slot;

static Slot slots[NSLOTS];

/* The epoch. Moves forward by one, and only when every pinned slot is already
 * at it. It wraps, and wrapping is fine, because nothing compares two epochs
 * for order: the only test anywhere is equality against this value. */
static uint32_t cur_epoch;

/* How far into each half anybody has ever been. The walk stops there rather
 * than reading all five hundred and twelve slots, almost all of which are
 * untouched pages on almost every program.
 *
 * Both only go up, and both are raised before the slot they describe is
 * published, which is what stops a walk from reading a stale limit and missing
 * a pin that matters. */
static uint32_t mhigh;
static uint32_t fhigh;

/* The three lists, one per epoch modulo three, and the lock over them.
 *
 * A list is filled during one epoch and freed three epochs later. Three rather
 * than two, which is what the rule needs, because the list being filled has to
 * be a different one from the list being freed and from the one in between. */
static burrow__Lock bag_lock;
static burrow__Retired *bag[3];

/* How many objects have arrived on the shared list since the last walk of the
 * slots. Under bag_lock, and it is what spreads the cost of a walk over a
 * batch no matter who the objects came from. */
static uint32_t bag_since;

/* How many objects are waiting, counting the ones still in somebody's pocket.
 * Nothing reads this to make a decision. */
static uint32_t pending;

/* What one M has retired and not yet pushed.
 *
 * Per M rather than per goroutine, because a goroutine moves between Ms and a
 * pocket that moved with it would need a lock to be safe. Per M rather than
 * thread local for the opposite reason: a thread local pocket cannot be found
 * by anybody else, so the objects in it at the end of the program have nowhere
 * to go and look exactly like a leak. These can be walked, which is what
 * burrow__reclaim_drain does.
 *
 * Only the thread running that M touches its pocket, so there is nothing
 * atomic here. Padded so that two Ms retiring at the same time are not writing
 * to the same cache line. */
typedef struct Pocket {
    _Alignas(BURROW_CACHELINE) burrow__Retired *head;
    uint32_t n;
} Pocket;

static Pocket pockets[MSLOTS];

/* This thread's slot and how many pins deep it is.
 *
 * Thread local rather than per goroutine on purpose. A pin is a promise about
 * what a thread is doing right now, and a goroutine that parks in the middle of
 * one has broken the promise, which is why the header says not to. */
static BURROW_THREAD_LOCAL Slot *my_slot;
static BURROW_THREAD_LOCAL uint32_t my_depth;

/* Raises a high water mark to at least `want`. Ordinary compare and swap loop,
 * and it gives up the moment somebody else has already raised it far enough. */
static void raise_high(uint32_t *high, uint32_t want) {
    uint32_t have = burrow__atomic_load_u32(high);
    while (have < want) {
        if (burrow__atomic_cas_u32(high, &have, want))
            return;
    }
}

/* Takes a slot out of the upper half, waiting if every one of them is busy.
 *
 * The compare and swap that claims the slot is also the one that publishes the
 * epoch, because for a foreign thread those two events are the same event: it
 * has no slot when it is not pinned, so a slot that is claimed is a slot that
 * is pinned.
 *
 * Waiting cannot deadlock. A pin is not allowed to park, so every thread
 * holding one of these is running and on its way out of it. */
static Slot *foreign_acquire(void) {
    for (;;) {
        for (uint32_t i = 0; i < FSLOTS; i++) {
            Slot *s = &slots[MSLOTS + i];
            uint32_t free_state = 0;
            uint32_t e = burrow__atomic_load_u32(&cur_epoch);
            raise_high(&fhigh, i + 1);
            if (burrow__atomic_cas_u32(&s->state, &free_state, (e << 1) | 1U))
                return s;
        }
        burrow__thread_yield();
    }
}

void burrow__pin(void) {
    if (my_depth++ > 0)
        return;

    burrow__M *m = burrow__curm();
    if (m == NULL) {
        my_slot = foreign_acquire();
        return;
    }

    Slot *s = &slots[m->id];
    my_slot = s;

    /* Before the publish below, so that a walk which reads the limit and then
     * reads the slots cannot read a limit from before this M existed and then
     * miss the pin it is about to take. */
    raise_high(&mhigh, (uint32_t)m->id + 1);

    /* Sequentially consistent, and this is the one instruction in the whole
     * file that has to be. Everything this thread is about to read has to be
     * ordered after this store, because a load that the processor hoists above
     * it is a load of a pointer nobody knew was being read. */
    uint32_t e = burrow__atomic_load_u32(&cur_epoch);
    burrow__atomic_store_u32(&s->state, (e << 1) | 1U);
}

/* Frees a list, outside any lock, and takes the pending count down as it goes. */
static void free_list(burrow__Retired *r) {
    while (r != NULL) {
        burrow__Retired *next = r->next;
        r->free(r->obj);
        burrow__atomic_add_u32(&pending, (uint32_t)-1);
        r = next;
    }
}

/* True when every pinned participant is already at `e`, which is the whole
 * condition for moving the epoch on.
 *
 * The loads are sequentially consistent for the same reason the store in
 * burrow__pin is. A slot this reads as unpinned belongs to a thread whose next
 * read of the structure is ordered after this read of the slot, and therefore
 * after whatever unlinking has already happened, so that thread cannot reach
 * the objects this walk is about to release. */
static bool everybody_is_at(uint32_t e) {
    uint32_t mn = burrow__atomic_load_u32(&mhigh);
    for (uint32_t i = 0; i < mn; i++) {
        uint32_t s = burrow__atomic_load_u32(&slots[i].state);
        if (s != 0 && (s >> 1) != e)
            return false;
    }
    uint32_t fn = burrow__atomic_load_u32(&fhigh);
    for (uint32_t i = 0; i < fn; i++) {
        uint32_t s = burrow__atomic_load_u32(&slots[MSLOTS + i].state);
        if (s != 0 && (s >> 1) != e)
            return false;
    }
    return true;
}

/* Empties a pocket and answers what was in it. */
static burrow__Retired *take(Pocket *p) {
    burrow__Retired *r = p->head;
    p->head = NULL;
    p->n = 0;
    return r;
}

/* Pushes a list onto the shared one and has a go at the epoch.
 *
 * The list goes into the bag for whatever the epoch is now, which for the
 * objects retired earliest in the batch is later than it strictly had to be.
 * That is deliberate: tagging each object as it arrives would mean reading the
 * epoch on every retire, and being one epoch late frees memory one round trip
 * later while being one epoch early frees it while somebody is reading it. */
static void push_and_collect(burrow__Retired *mine, bool force) {
    burrow__Retired *dead = NULL;

    burrow__lock(&bag_lock);
    uint32_t e = burrow__atomic_load_u32(&cur_epoch);
    if (mine != NULL) {
        burrow__Retired *tail = mine;
        uint32_t n = 1;
        while (tail->next != NULL) {
            tail = tail->next;
            n++;
        }
        tail->next = bag[e % 3];
        bag[e % 3] = mine;
        bag_since += n;
    }

    /* The walk of the slots is the whole cost of a collection, so it happens
     * once per batch of arrivals rather than once per arrival. An M brings its
     * batch in one go and crosses this on its own, and a thread without a
     * pocket brings one object at a time and crosses it with the help of
     * everybody else, which is what stops a program whose only user of this is
     * a foreign thread from paying for a walk on every delete. */
    if (!force && bag_since < BATCH) {
        burrow__unlock(&bag_lock);
        return;
    }
    bag_since = 0;

    if (everybody_is_at(e)) {
        /* Moving to e plus one makes the bag we are about to step into safe.
         * It was last emptied when the epoch was e minus two, so everything in
         * it was retired during that epoch, and an object retired during e
         * minus two needs the epoch to reach e, which it has. */
        uint32_t next = e + 1;
        burrow__atomic_store_u32(&cur_epoch, next);
        dead = bag[next % 3];
        bag[next % 3] = NULL;
    }
    burrow__unlock(&bag_lock);

    free_list(dead);
}

void burrow__unpin(void) {
    if (my_depth == 0)
        runtime_throw(BURROW_S("reclaim: unpin without a pin"));
    if (--my_depth > 0)
        return;

    Slot *s = my_slot;
    my_slot = NULL;

    /* Release, so that everything this thread read while pinned stays below it.
     * The slot going to zero is also what gives a foreign slot back, which is
     * why there is nothing else to undo. */
    burrow__atomic_store_release_u32(&s->state, 0);
}

void burrow__retire(burrow__Retired *r, void (*free)(void *obj), void *obj) {
    r->free = free;
    r->obj = obj;
    r->next = NULL;
    burrow__atomic_add_u32(&pending, 1);

    /* A thread the scheduler did not start has no pocket, because a pocket is
     * only findable through the M it belongs to and there is no M. So it pays
     * for the lock on every retire, which is the same deal it gets everywhere
     * else here and is the reason the fast path can stay this short. */
    burrow__M *m = burrow__curm();
    if (m == NULL) {
        push_and_collect(r, false);
        return;
    }

    Pocket *p = &pockets[m->id];
    r->next = p->head;
    p->head = r;
    p->n++;

    if (p->n >= BATCH)
        push_and_collect(take(p), false);
}

void burrow__reclaim_flush(void) {
    burrow__M *m = burrow__curm();
    push_and_collect(m != NULL ? take(&pockets[m->id]) : NULL, true);
}

void burrow__reclaim_drain(void) {
    burrow__Retired *mine = NULL;
    for (uint32_t i = 0; i < MSLOTS; i++) {
        burrow__Retired *r = pockets[i].head;
        pockets[i].head = NULL;
        pockets[i].n = 0;
        while (r != NULL) {
            burrow__Retired *next = r->next;
            r->next = mine;
            mine = r;
            r = next;
        }
    }

    burrow__lock(&bag_lock);
    burrow__Retired *dead = NULL;
    for (int i = 0; i < 3; i++) {
        burrow__Retired *r = bag[i];
        bag[i] = NULL;
        while (r != NULL) {
            burrow__Retired *next = r->next;
            r->next = dead;
            dead = r;
            r = next;
        }
    }
    burrow__unlock(&bag_lock);

    free_list(mine);
    free_list(dead);
}

Int burrow__reclaim_pending(void) {
    return (Int)burrow__atomic_load_u32(&pending);
}
