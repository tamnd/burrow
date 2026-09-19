/* The scheduler. burrow/sched.h has the structures and the queues, this has the
 * loop that drives them, and burrow/proc.h is what a program outside the runtime
 * sees of it.
 *
 * Every algorithm here is Go's from runtime/proc.go. The search order in
 * findrunnable, the rule about how many threads may be looking for work at once,
 * the decision to drop the P and look again before parking, and the order those
 * three happen in are all load bearing. A port that reorders them has changed
 * how programs schedule, usually into a shape where a wakeup goes missing about
 * once an hour on a machine with enough cores.
 *
 * The one thing written differently from Go is mcall. Go switches to the
 * scheduler's stack and calls a function there, in assembly, per architecture.
 * Here the goroutine leaves a function pointer in its M and switches back to
 * g0, and the scheduler loop makes the call on the other side of the switch.
 * Same effect, no assembly, one predictable branch per schedule. Why it has to
 * happen on the scheduler's stack at all: parking a goroutine means publishing
 * it somewhere another thread can find it, and doing that while still standing
 * on its stack is a race with that thread picking it up and running it.
 *
 * Two things Go has that are not here yet, both of them further down the list in
 * docs/design/06-runtime.md section 12. There is no netpoll step in
 * findrunnable, because there is no netpoller, and where it goes is marked. And
 * nothing preempts a goroutine, so a loop that never blocks holds its thread
 * until it finishes.
 *
 * Timers are here. What that costs this file is three things: a check of this
 * P's own timers at the top of findrunnable, a check of a victim's on the last
 * stealing pass, and a deadline on the sleep a thread takes when it has run out
 * of places to look. The last one is where Go calls the netpoller with a
 * timeout, and swapping this for that is what the netpoller change does.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/proc.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/context.h"
#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/note.h"
#include "burrow/platform.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"
#include "burrow/stack.h"
#include "burrow/thread.h"
#include "burrow/timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A fiber brings its own stack and there is no call that takes one you already
 * have, so on Windows a goroutine that also mapped a stack would have mapped it
 * for nothing. Worse than nothing: the Windows path commits what it maps, so
 * every goroutine would cost its whole stack in real memory rather than in the
 * pages it touches. So the mapping is skipped there and the size is handed to
 * the fiber instead. */
#if defined(BURROW_CONTEXT_FIBERS)
#define GOROUTINE_MAPS_ITS_STACK 0
#else
#define GOROUTINE_MAPS_ITS_STACK 1
#endif

/* How many dead goroutines a P keeps to itself before half of them go to the
 * central list. Go's number, and the reason for a limit at all is that a P which
 * once spawned a million goroutines should not be holding a million stacks an
 * hour later. */
#define GFREE_MAX 64

/* How many passes findrunnable makes over the other Ps before it gives up.
 * Go's number. The last one is the only one allowed to take a victim's runnext,
 * which is why there is more than one. */
#define STEAL_PASSES 4

/* ---------------------------------------------------------------- the world
 *
 * One of these per process, and the comment on each field says what makes it
 * safe to touch. Three kinds: under `lock`, atomic, or fixed while the
 * scheduler is running. Nothing here is any other kind, and a field that wanted
 * to be would be a field that needs a different design. */
typedef struct Sched {
    /* Covers everything below marked as being under it. */
    burrow__Lock lock;

    /* Under the lock. Everything that overflowed a P's ring, plus everything
     * readied by a thread that was not holding a P at the time.
     *
     * `runqsize` is the queue's length again, written atomically every time the
     * queue changes and read by threads that have not got the lock. A thread
     * about to go hunting wants to know whether this is worth a trip through the
     * lock, and making it take the lock to find that out is exactly the
     * bottleneck the local queues exist to avoid. */
    burrow__GQueue runq;
    uint32_t runqsize;

    /* Under the lock. Ms with no work, each asleep on its own note. */
    burrow__M *midle;
    int32_t nmidle;

    /* Under the lock. How many Ms have been created, which is also the next free
     * index into allm. */
    int32_t nmcreated;

    /* Under the lock. Ps with no M holding them.
     *
     * The count is atomic as well as being under the lock, because it is one of
     * the two numbers a thread deciding whether to go looking for work reads
     * without taking anything. Writing it atomically under the lock costs
     * nothing and is what makes that read legal rather than merely usually
     * right. */
    burrow__P *pidle;
    uint32_t npidle;

    /* Under the lock. Every goroutine that exists, so that shutting down can
     * give back the stack of one that parked and was never woken. */
    burrow__G *allg;

    /* Under the lock. Dead goroutines that overflowed a P's own free list. */
    burrow__G *gfree;
    int32_t gfree_count;

    /* Atomic. How many Ms are looking for work without holding one. Capped at
     * half of GOMAXPROCS, which is what stops every idle thread waking up to
     * search for the same one goroutine. */
    uint32_t nmspinning;

    /* Atomic. Live goroutines, for NumGoroutine, and the next id to hand out. */
    uint32_t ngoroutine;
    uint64_t nextgoid;

    /* Atomic. Set once the world is up, and once again when it is coming down.
     * Every M checks the second one at the top of its search and before it
     * parks, which are the only two places it can safely stop. */
    uint32_t running;
    uint32_t stopping;

    /* Fixed while the scheduler is running. */
    int32_t gomaxprocs;

    /* The goroutine runtime_main is waiting for, and the gate it waits on. */
    burrow__G *maing;
    burrow__Note mainnote;
} Sched;

static Sched sched;

/* The Ps and the Ms. Static because a scheduler that can fail to start is a
 * scheduler with an error path through every caller of `go`, and because BSS
 * costs address space rather than memory until it is touched. See
 * BURROW_MAXPROCS in burrow/sched.h.
 *
 * One M per P is all this needs today. Go has more, because an M blocked in a
 * system call has handed its P to somebody else and is still an M, and that is
 * what grows this array when system calls learn to give the P up. */
static burrow__P allp[BURROW_MAXPROCS];
static burrow__M allm[BURROW_MAXPROCS];

/* Which M the calling thread is, or NULL on a thread the scheduler did not
 * start. Thread local, written once when the thread becomes an M and once when
 * it stops being one. */
static BURROW_THREAD_LOCAL burrow__M *curm;

/* ------------------------------------------------------------- the accessors */

burrow__M *burrow__curm(void) {
    return curm;
}

burrow__G *burrow__curg(void) {
    return curm != NULL ? curm->curg : NULL;
}

burrow__P *burrow__allp(int32_t i) {
    if (i < 0 || i >= sched.gomaxprocs)
        return NULL;
    return &allp[i];
}

int32_t burrow__gomaxprocs(void) {
    return sched.gomaxprocs;
}

burrow__Timers *burrow__timers_local(void) {
    if (curm == NULL || curm->p == NULL)
        return NULL;
    return &curm->p->timers;
}

Goroutine *sched_current(void) {
    return burrow__curg();
}

int runtime_numcpu(void) {
    return burrow__thread_ncpu();
}

int runtime_numgoroutine(void) {
    return (int)burrow__atomic_load_u32(&sched.ngoroutine);
}

static void set_status(burrow__G *g, burrow__GStatus s) {
    burrow__atomic_store_u32(&g->status, (uint32_t)s);
}

/* ------------------------------------------------------ the global run queue
 *
 * All three want the lock held. They are spelled as functions anyway rather
 * than inlined into their callers, because the lock discipline is easier to
 * check when the only things that touch sched.runq have the word globrunq in
 * the name. */

static void globrunq_publish(void) {
    burrow__atomic_store_release_u32(&sched.runqsize, (uint32_t)sched.runq.len);
}

static void globrunq_put(BURROW_RETAINS(1) burrow__G *g) {
    burrow__gqueue_push(&sched.runq, g);
    globrunq_publish();
}

static void globrunq_put_batch(burrow__GQueue *batch) {
    burrow__gqueue_push_all(&sched.runq, batch);
    globrunq_publish();
}

/* Takes this P's share of the global queue, answers the first of them and puts
 * the rest on the P's own ring.
 *
 * The share is Go's: length over GOMAXPROCS, plus one so a queue shorter than
 * GOMAXPROCS still hands something out. Taking one at a time means coming back
 * through the lock for every goroutine, and taking the lot means one P runs
 * everything while the others steal it back one at a time. */
static burrow__G *globrunq_get(burrow__P *p) {
    if (sched.runq.len == 0)
        return NULL;

    int32_t n = sched.runq.len / sched.gomaxprocs + 1;
    if (n > sched.runq.len)
        n = sched.runq.len;
    if (n > BURROW_RUNQ_SIZE / 2)
        n = BURROW_RUNQ_SIZE / 2;

    burrow__G *gp = burrow__gqueue_pop(&sched.runq);
    for (n--; n > 0; n--) {
        burrow__G *g1 = burrow__gqueue_pop(&sched.runq);
        if (g1 == NULL)
            break;

        /* Cannot fail: n was capped at half the ring and the ring was this P's
         * own, which is to say empty enough to have come looking here. The check
         * is here because "cannot fail" and "does not fail" are different claims
         * and only one of them is testable. */
        burrow__G *overflow = NULL;
        if (!burrow__runq_put(p, g1, false, &overflow)) {
            burrow__gqueue_push_head(&sched.runq, overflow);
            break;
        }
    }
    globrunq_publish();
    return gp;
}

/* Puts a goroutine on a P's ring, and deals with the ring being full.
 *
 * The full case moves half of the ring plus the newcomer to the global queue in
 * one batch, which is one trip through the lock for 129 goroutines rather than
 * 129 trips. The loop is for the case where the ring stopped being full while
 * that was being arranged, which is a thief having just taken from it. */
static void runq_put(burrow__P *p, BURROW_RETAINS(2) burrow__G *g, bool next) {
    for (;;) {
        burrow__G *overflow = NULL;
        if (burrow__runq_put(p, g, next, &overflow))
            return;

        burrow__GQueue batch = {NULL, NULL, 0};
        if (burrow__runq_put_slow(p, overflow, &batch)) {
            burrow__lock(&sched.lock);
            globrunq_put_batch(&batch);
            burrow__unlock(&sched.lock);
            return;
        }

        /* The ring had room after all. Try the one that did not fit, and not the
         * one that came in, because a put with `next` set has already swapped
         * into the runnext slot and the two are no longer the same goroutine. */
        g = overflow;
        next = false;
    }
}

/* ------------------------------------------------------------- the idle lists
 *
 * Both want the lock. A P is on the idle list exactly when no M holds it, and
 * an M is on the idle list exactly when it is asleep on its note, and those two
 * facts are what the whole wakeup protocol rests on. */

/* A P's status is written under the scheduler lock and read without it, by a
 * thief deciding whether the victim is running and so whether its runnext slot
 * is worth reaching for. A plain store against an atomic load is a data race in
 * C whatever the hardware does with it, so the store is atomic too. */
static void set_pstatus(burrow__P *p, burrow__PStatus s) {
    burrow__atomic_store_u32(&p->status, (uint32_t)s);
}

static void pidle_put(burrow__P *p) {
    set_pstatus(p, BURROW_PIDLE);
    p->m = NULL;
    p->link = sched.pidle;
    sched.pidle = p;
    burrow__atomic_store_release_u32(
        &sched.npidle, burrow__atomic_load_relaxed_u32(&sched.npidle) + 1U);
}

static burrow__P *pidle_get(void) {
    burrow__P *p = sched.pidle;
    if (p == NULL)
        return NULL;

    sched.pidle = p->link;
    p->link = NULL;
    burrow__atomic_store_release_u32(
        &sched.npidle, burrow__atomic_load_relaxed_u32(&sched.npidle) - 1U);
    return p;
}

/* Hands a P to an M. Three writes that always go together, and a P with an M and
 * a status that disagree is a P two threads think they own. */
static void acquirep(burrow__M *m, burrow__P *p) {
    m->p = p;
    p->m = m;
    set_pstatus(p, BURROW_PRUNNING);
}

static void midle_put(burrow__M *m) {
    m->next = sched.midle;
    sched.midle = m;
    sched.nmidle++;
}

/* Takes an M off the idle list if it is still on it, and answers whether it was.
 *
 * A thread that parked with a deadline and reached it has to do this before it
 * goes back to looking for work, because being on that list is a promise to be
 * asleep. Whoever finds it there next would hand it a P and wake a thread that
 * is already awake and holding a different one.
 *
 * A walk rather than a doubly linked list. The list is at most one entry per P
 * and this runs only when a sleep runs out, which is once per timer rather than
 * once per goroutine. */
static bool midle_remove(burrow__M *m) {
    burrow__M **at = &sched.midle;

    while (*at != NULL) {
        if (*at == m) {
            *at = m->next;
            m->next = NULL;
            sched.nmidle--;
            return true;
        }
        at = &(*at)->next;
    }
    return false;
}

static burrow__M *midle_get(void) {
    burrow__M *m = sched.midle;
    if (m == NULL)
        return NULL;

    sched.midle = m->next;
    m->next = NULL;
    sched.nmidle--;
    return m;
}

/* -------------------------------------------------------- goroutines and stacks
 *
 * A goroutine costs a struct and a stack, and the stack is the expensive half
 * by three orders of magnitude: a mapping with a guard page is a system call and
 * a fresh page table entry, and it measures around fifteen microseconds on a
 * server. So dead goroutines are kept with their stacks attached and handed to
 * the next one, per P so that taking one needs no atomic at all. That cache is
 * the difference between a goroutine costing fifteen microseconds and costing a
 * few hundred nanoseconds. */

#if GOROUTINE_MAPS_ITS_STACK

static size_t goroutine_stack_bytes(const burrow__G *g) {
    return (size_t)((char *)g->stack.hi - (char *)g->stack.lo);
}

static bool stack_take(burrow__G *g, size_t bytes) {
    return burrow__stack_alloc(&g->stack, bytes);
}

static void stack_give_back(burrow__G *g) {
    burrow__stack_free(&g->stack);
}

/* Whether the stack this goroutine already has is big enough for what is being
 * asked for now. */
static bool stack_big_enough(const burrow__G *g, size_t bytes) {
    return g->stack.lo != NULL && goroutine_stack_bytes(g) >= bytes;
}

/* Whether this goroutine still needs a mapping before it can run. */
static bool stack_missing(const burrow__G *g) {
    return g->stack.lo == NULL;
}

#else

/* The fiber owns the stack, so there is nothing here to allocate, nothing to
 * free, nothing to measure and nothing that can be the wrong size: the number
 * goes to CreateFiber on every make and the mapping is the operating system's
 * business. These four exist so that the code that calls them does not have to
 * know which platform it is on. */

static bool stack_take(burrow__G *g, size_t bytes) {
    (void)g;
    (void)bytes;
    return true;
}

static void stack_give_back(burrow__G *g) {
    (void)g;
}

static bool stack_big_enough(const burrow__G *g, size_t bytes) {
    (void)g;
    (void)bytes;
    return true;
}

static bool stack_missing(const burrow__G *g) {
    (void)g;
    return false;
}

#endif

/* Takes a dead goroutine from this P's free list, or the central one, or makes a
 * new one. NULL means the system would not give out a stack.
 *
 * `p` may be NULL, which is `go` being called from a thread that is not one of
 * the scheduler's. Then there is no local list to look at and the central one is
 * the whole answer. */
static burrow__G *gfget(burrow__P *p, size_t bytes) {
    burrow__G *g = NULL;

    if (p != NULL && p->gfree != NULL) {
        g = p->gfree;
        p->gfree = g->next;
        g->next = NULL;
        p->gfree_count--;
    }

    if (g == NULL) {
        burrow__lock(&sched.lock);
        if (sched.gfree != NULL) {
            g = sched.gfree;
            sched.gfree = g->next;
            g->next = NULL;
            sched.gfree_count--;
        }
        burrow__unlock(&sched.lock);
    }

    if (g == NULL) {
        Alloc *a = heap_allocator();
        g = mem_alloc(a, sizeof(burrow__G), _Alignof(burrow__G));
        if (g == NULL)
            return NULL;

        g->id = burrow__atomic_add_u64(&sched.nextgoid, 1);

        burrow__lock(&sched.lock);
        g->allnext = sched.allg;
        sched.allg = g;
        burrow__unlock(&sched.lock);
    }

    if (!stack_big_enough(g, bytes)) {
        /* The one it came with is too small for this request. Give it back and
         * ask for the right one. This is rare: it takes a program that mixes
         * go and go_stack, and the common case is every goroutine asking for the
         * same size and every reuse matching on the first try. */
        stack_give_back(g);
    }

    if (stack_missing(g) && !stack_take(g, bytes)) {
        /* No stack to be had. The G itself stays on allg, because it is already
         * on it and taking it off needs a walk. It goes back on a free list with
         * no stack, which the next gfget handles by asking for one again. */
        burrow__lock(&sched.lock);
        g->next = sched.gfree;
        sched.gfree = g;
        sched.gfree_count++;
        burrow__unlock(&sched.lock);
        return NULL;
    }
    return g;
}

/* Puts a dead goroutine on a free list, and moves half of an overfull local list
 * to the central one. The context goes now rather than at reuse, because on
 * Windows it is a fiber and holding on to it holds a stack with it. */
static void gfput(burrow__P *p, BURROW_RETAINS(2) burrow__G *g) {
    burrow__context_free(&g->ctx);
    g->ctx = (burrow__Context){0};

    if (p == NULL) {
        burrow__lock(&sched.lock);
        g->next = sched.gfree;
        sched.gfree = g;
        sched.gfree_count++;
        burrow__unlock(&sched.lock);
        return;
    }

    g->next = p->gfree;
    p->gfree = g;
    p->gfree_count++;
    if (p->gfree_count < GFREE_MAX)
        return;

    burrow__G *move = NULL;
    int32_t moved = 0;
    while (p->gfree_count > GFREE_MAX / 2) {
        burrow__G *one = p->gfree;
        p->gfree = one->next;
        p->gfree_count--;
        one->next = move;
        move = one;
        moved++;
    }

    burrow__lock(&sched.lock);
    while (move != NULL) {
        burrow__G *one = move;
        move = one->next;
        one->next = sched.gfree;
        sched.gfree = one;
    }
    sched.gfree_count += moved;
    burrow__unlock(&sched.lock);
}

/* --------------------------------------------------------------- running one
 *
 * The other end of every goroutine. Runs what it was asked to run and then ends
 * the goroutine, which never returns, which is why the context was made with no
 * link to return to. */
static void goroutine_start(void *arg) {
    burrow__G *g = (burrow__G *)arg;
    Func fn = {g->entry, g->arg};

    BURROW_CALLF0(fn);
    runtime_goexit();
}

/* Points a goroutine's context at its stack and at goroutine_start, which is
 * what makes it runnable. Answers false if the system would not give out
 * whatever the backend needs, which on Windows is a fiber and everywhere else is
 * nothing at all.
 *
 * `asked` is the size the caller wanted, which matters only where the fiber owns
 * the stack. Everywhere else the mapping has already happened and the size is
 * whatever rounding up to a page produced. */
static bool make_context(burrow__G *g, size_t asked) {
#if GOROUTINE_MAPS_ITS_STACK
    (void)asked;
    return burrow__context_make(&g->ctx, g->stack.lo, goroutine_stack_bytes(g),
                                goroutine_start, g, NULL);
#else
    /* The fiber backend makes its own stack and ignores the one it is handed,
     * but it checks the argument anyway so that a NULL is a bug on every
     * platform rather than on three out of four. So it gets an address that
     * exists, belongs to this goroutine, and is never read through here. */
    return burrow__context_make(&g->ctx, (void *)g, asked, goroutine_start, g, NULL);
#endif
}

/* Leaves a call for the scheduler to make on g0 and switches to it. Comes back
 * when somebody schedules this goroutine again, and for goexit it never comes
 * back at all. */
static void mcall(burrow__G *(*fn)(burrow__M *m, burrow__G *g)) {
    burrow__M *m = curm;
    burrow__G *gp = m->curg;

    m->mcall = fn;
    m->mcallg = gp;
    burrow__context_switch(&gp->ctx, &m->g0.ctx);
}

/* Runs one goroutine until it stops, makes the call it left behind, and answers
 * whatever that call says to run next. NULL means go and look for work.
 *
 * Everything between the two switches is on the goroutine's own stack and
 * everything outside them is on the thread's. */
static burrow__G *execute(burrow__M *m, burrow__G *gp) {
    m->curg = gp;
    set_status(gp, BURROW_GRUNNING);
    burrow__stack_set_current(&gp->stack);

    burrow__context_switch(&m->g0.ctx, &gp->ctx);

    /* Back on the thread's own stack, so the goroutine's bounds are no longer
     * what a stack overflow should be measured against. */
    burrow__stack_set_current(NULL);
    m->curg = NULL;

    burrow__G *(*fn)(burrow__M *, burrow__G *) = m->mcall;
    burrow__G *arg = m->mcallg;
    m->mcall = NULL;
    m->mcallg = NULL;

    if (fn == NULL)
        runtime_throw(BURROW_S("schedule: goroutine stopped without an mcall"));
    return fn(m, arg);
}

/* --------------------------------------------------- the three ways to stop */

static burrow__G *goexit0(burrow__M *m, burrow__G *gp) {
    set_status(gp, BURROW_GDEAD);
    gp->entry = NULL;
    gp->arg = NULL;
    burrow__atomic_add_u32(&sched.ngoroutine, (uint32_t)-1);

    bool was_main = gp == sched.maing;
    gfput(m->p, gp);

    /* Last, so that by the time runtime_main wakes up, the goroutine it was
     * waiting for is on a free list and not half way there. */
    if (was_main)
        burrow__note_wake(&sched.mainnote);
    return NULL;
}

static burrow__G *gosched0(burrow__M *m, burrow__G *gp) {
    (void)m;
    set_status(gp, BURROW_GRUNNABLE);

    /* The global queue and not this P's own ring, because a goroutine that asked
     * to give way and then went straight back to the front of its own queue has
     * not given way to anything. Go does the same. */
    burrow__lock(&sched.lock);
    globrunq_put(gp);
    burrow__unlock(&sched.lock);
    return NULL;
}

static burrow__G *park0(burrow__M *m, burrow__G *gp) {
    set_status(gp, BURROW_GWAITING);

    SchedUnlockFn unlockf = m->parkunlock;
    void *lock = m->parklock;
    m->parkunlock = NULL;
    m->parklock = NULL;

    if (unlockf != NULL && !unlockf(gp, lock)) {
        /* One more look under the lock found the thing this goroutine was about
         * to wait for. Nobody else has seen it in the waiting state, because
         * nobody else could have taken the lock, so it can simply carry on. */
        set_status(gp, BURROW_GRUNNABLE);
        return gp;
    }
    return NULL;
}

/* ---------------------------------------------------------------- waking up */

static void newm(burrow__P *p, bool spinning);

/* Finds a thread for `p` and wakes it. With p NULL it takes an idle P first and
 * does nothing if there is not one.
 *
 * `spinning` says the thread being started has already been counted in
 * nmspinning, so if this gives up it has to put that count back. Getting that
 * wrong leaves the count permanently too high, which stops other threads from
 * ever going to look for work, which is a program that goes quiet under load
 * and looks like a deadlock. */
static void startm(burrow__P *p, bool spinning) {
    burrow__lock(&sched.lock);

    if (p == NULL) {
        p = pidle_get();
        if (p == NULL) {
            burrow__unlock(&sched.lock);
            if (spinning)
                burrow__atomic_add_u32(&sched.nmspinning, (uint32_t)-1);
            return;
        }
    }

    burrow__M *m = midle_get();
    if (m == NULL) {
        if (sched.nmcreated < sched.gomaxprocs) {
            burrow__unlock(&sched.lock);
            newm(p, spinning);
            return;
        }

        /* Every thread is busy. Put the P back and leave: whichever thread
         * finishes first will look at the idle list before it parks, which is
         * the step in findrunnable that exists for exactly this. */
        pidle_put(p);
        burrow__unlock(&sched.lock);
        if (spinning)
            burrow__atomic_add_u32(&sched.nmspinning, (uint32_t)-1);
        return;
    }

    m->nextp = p;
    m->spinning = spinning ? 1U : 0U;
    burrow__unlock(&sched.lock);

    burrow__note_wake(&m->park);
}

/* Tries to get a thread started on some idle P, because there is new work.
 *
 * The compare and swap is the whole point. Only one thread at a time may be in
 * the spinning state on behalf of a wakeup, so a goroutine readying ten others
 * in a row wakes one thread and not ten, and the one it wakes finds the rest by
 * stealing. */
static void wakep(void) {
    if (burrow__atomic_load_relaxed_u32(&sched.stopping) != 0)
        return;

    uint32_t none = 0;
    if (!burrow__atomic_cas_u32(&sched.nmspinning, &none, 1))
        return;

    startm(NULL, true);
}

/* A timer has been set for earlier than whoever is asleep was told. Go pokes the
 * netpoller here, since in Go the thread with nothing to do is sitting in
 * epoll_wait with a deadline. burrow has no netpoller, so that thread is asleep
 * on its own note with a deadline on it, and the way to cut that short is the
 * same way new work cuts it short.
 *
 * The thread this has to reach has already given its P up, which is what makes
 * wakep the right call rather than a second mechanism: a P with a timer due and
 * nobody holding it is a P on the idle list, and that is exactly what wakep goes
 * looking for.
 *
 * wakep does nothing when a thread is already out searching, and that is safe
 * for the same reason it is safe for a goroutine being readied. A searching
 * thread stops counting itself as searching and then takes one more look before
 * it parks, and that last look reads every P's wake time. Both halves are
 * sequentially consistent, so in any order the threads agree on, either the
 * searcher sees this timer or this sees a searcher that has not finished. */
void burrow__timers_wake(void) {
    wakep();
}

/* Parks this thread until somebody hands it a P. Comes back with m->p set, or
 * with m->p NULL when the world is coming down.
 *
 * `until` is a burrow__nanotime reading to wake at whether or not anybody hands
 * this thread anything, or 0 to sleep until somebody does. That is how a timer
 * gets run when every thread has run out of work: the earliest timer anybody has
 * becomes the deadline on this sleep. In Go the same deadline goes to the
 * netpoller, because in Go this thread is the one sitting in epoll_wait.
 *
 * The loop is because a deadline that passes is not on its own a reason to stop
 * being idle. The thread takes itself off the idle list, looks for a P to do the
 * work with, and if there is not one to be had it goes back to sleep, this time
 * with no deadline: every P is busy, so every P has a thread that will get to its
 * own timers. */
static void stopm(burrow__M *m, int64_t until) {
    for (;;) {
        burrow__lock(&sched.lock);
        if (burrow__atomic_load_relaxed_u32(&sched.stopping) != 0) {
            burrow__unlock(&sched.lock);
            return;
        }
        midle_put(m);
        burrow__unlock(&sched.lock);

        bool woken = true;
        if (until == 0) {
            burrow__note_sleep(&m->park);
        } else {
            woken = burrow__note_sleep_timeout(&m->park, until - burrow__nanotime());
        }
        burrow__note_clear(&m->park);

        burrow__lock(&sched.lock);
        if (!woken) {
            /* The deadline passed. Off the list, and if it turns out somebody
             * took this thread off it already then that somebody has left a P in
             * nextp and the sleep was going to end anyway. */
            (void)midle_remove(m);
        }
        burrow__P *p = m->nextp;
        m->nextp = NULL;
        if (p == NULL && !woken)
            p = pidle_get();
        burrow__unlock(&sched.lock);

        if (p != NULL) {
            acquirep(m, p);
            return;
        }
        if (burrow__atomic_load_acquire_u32(&sched.stopping) != 0)
            return;

        /* Whatever the deadline was for, this thread could not get a P to do it
         * with. Sleep until somebody has something to hand over. */
        until = 0;
    }
}

/* ------------------------------------------------------------- finding work */

static uint32_t gcd_u32(uint32_t a, uint32_t b) {
    while (b != 0) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

/* Walks the other Ps in a different order on every thread and every pass.
 *
 * A fixed order is what makes every idle thread arrive at P0 at the same moment
 * and fight over it. Go solves that with a random start and a random stride
 * chosen from the strides that are coprime with the number of Ps, since a stride
 * that shares a factor visits only some of them. This picks the stride the same
 * way, by walking up from a random point until the greatest common divisor is
 * one, which always terminates because one is coprime with everything. */
static burrow__G *steal_work(burrow__M *m, int64_t *now, bool *ran_timer) {
    burrow__P *p = m->p;
    uint32_t n = (uint32_t)sched.gomaxprocs;
    if (n < 2)
        return NULL;

    for (int pass = 0; pass < STEAL_PASSES; pass++) {
        uint64_t r = runtime_rand64();
        uint32_t at = (uint32_t)(r % n);
        uint32_t step = (uint32_t)((r >> 32U) % n) + 1U;
        while (gcd_u32(step, n) != 1U)
            step = step % n + 1U;

        for (uint32_t k = 0; k < n; k++) {
            burrow__P *victim = &allp[at];
            at = (at + step) % n;
            if (victim == p)
                continue;

            /* The last pass also runs the victim's timers, for the case where
             * the whole program is asleep on one. Nobody is going to come and
             * run them: the P they belong to has no thread on it, which is why
             * there was nothing to steal from it either. Doing it on the last
             * pass rather than the first keeps it off the path of a thread that
             * is about to find real work. */
            if (pass == STEAL_PASSES - 1) {
                bool ran = false;
                *now = burrow__timers_check(&victim->timers, *now, NULL, &ran);
                if (ran) {
                    /* A timer that readied a goroutine put it on this thread's
                     * own queue, not on the victim's, because readying happens
                     * wherever the thread doing it is standing. */
                    burrow__G *own = burrow__runq_get(p);
                    if (own != NULL)
                        return own;
                    *ran_timer = true;
                }
            }

            /* Only the last pass may take the victim's runnext, and only then
             * because every other way of finding work has already failed. That
             * goroutine is the one the victim is about to run, and taking it is
             * exactly the cache miss the runnext slot exists to avoid. */
            burrow__G *gp = burrow__runq_steal(p, victim, pass == STEAL_PASSES - 1);
            if (gp != NULL)
                return gp;
        }
    }
    return NULL;
}

/* Whether this thread should start looking for work rather than going to sleep.
 *
 * At most half the Ps' worth of threads may be searching at once. Without a cap,
 * one goroutine becoming runnable wakes every idle thread, all of them scan
 * every run queue, one of them wins, and the rest go back to sleep having spent
 * a cache line transfer each. Go's rule, and the constant is Go's constant. */
static bool may_spin(void) {
    uint32_t spinning = burrow__atomic_load_acquire_u32(&sched.nmspinning);
    uint32_t idle = burrow__atomic_load_acquire_u32(&sched.npidle);
    uint32_t procs = (uint32_t)sched.gomaxprocs;

    /* Ps that have an M on them, which is the number of threads that could
     * plausibly still produce work. The subtraction cannot go negative, since
     * the idle list is a subset of the Ps, and it is written this way round
     * because both are unsigned. */
    if (idle >= procs)
        return false;
    return 2U * spinning < procs - idle;
}

/* Answers a runnable goroutine, blocking until there is one. NULL means the
 * world is coming down and this thread should stop.
 *
 * The order is Go's and it is the part of this file to change least casually:
 * this P's own queue, the global queue, then the other Ps, then give the P up
 * and look one more time before parking. Locality first, the shared thing
 * second, somebody else's work third. Where the netpoller goes is marked. */
static burrow__G *findrunnable(burrow__M *m) {
    for (;;) {
        if (burrow__atomic_load_acquire_u32(&sched.stopping) != 0 || m->p == NULL)
            return NULL;

        burrow__P *p = m->p;

        /* One reading of the clock per pass, shared by this P's timers and by
         * every other P's in the stealing loop below, because a thread looking
         * at eight Ps does not need eight readings a few nanoseconds apart. It
         * has to be taken again on the next pass though, and hoisting it out of
         * this loop is a thread that sleeps until a timer is due, wakes up,
         * compares the timer against the time before it went to sleep, decides
         * nothing is due yet and goes round again forever. */
        int64_t now = 0;

        /* Timers before anything else, because a timer that is due readies a
         * goroutine onto this P's own queue and the next thing this does is look
         * there. A P with no timers pays two atomic loads for this. */
        now = burrow__timers_check(&p->timers, now, NULL, NULL);

        burrow__G *gp = burrow__runq_get(p);
        if (gp != NULL)
            return gp;

        /* An unlocked look first. It can be wrong in both directions and both
         * are harmless: a false empty means this pass goes on to stealing and
         * the give up path checks again under the lock, and a false non empty
         * means one wasted trip through an uncontended lock. */
        if (burrow__atomic_load_acquire_u32(&sched.runqsize) != 0) {
            burrow__lock(&sched.lock);
            gp = globrunq_get(p);
            burrow__unlock(&sched.lock);
            if (gp != NULL)
                return gp;
        }

        /* The netpoller goes here, as a poll with no timeout, so that a thread
         * with nothing to run finds a socket that became readable before it
         * starts taking work off other threads. */

        if (m->spinning == 0 && may_spin()) {
            m->spinning = 1;
            burrow__atomic_add_u32(&sched.nmspinning, 1);
        }
        if (m->spinning != 0) {
            bool ran_timer = false;
            gp = steal_work(m, &now, &ran_timer);
            if (gp != NULL)
                return gp;
            if (ran_timer)
                continue;
        }

        /* Nothing anywhere. Give the P up, and then look again.
         *
         * The order of those two is the whole trick, and reversing it is the
         * bug that loses a wakeup. A thread that checks the queues and then
         * gives up its P leaves a window in between where somebody can add work
         * and call wakep, find no idle P because this thread still has one, and
         * go away again, and then this thread parks with the work sitting there.
         * Dropping the P first means that wakep either sees the P on the idle
         * list and wakes somebody, or has not run yet, in which case the second
         * look below finds what it added. */
        burrow__lock(&sched.lock);
        if (burrow__atomic_load_relaxed_u32(&sched.stopping) != 0) {
            burrow__unlock(&sched.lock);
            return NULL;
        }
        gp = globrunq_get(p);
        if (gp != NULL) {
            burrow__unlock(&sched.lock);
            return gp;
        }
        m->p = NULL;
        pidle_put(p);
        burrow__unlock(&sched.lock);

        bool was_spinning = m->spinning != 0;
        if (was_spinning) {
            m->spinning = 0;
            burrow__atomic_add_u32(&sched.nmspinning, (uint32_t)-1);
        }

        /* The second look. Only a thread that was spinning does it, because a
         * thread that was not spinning was never the one responsible for finding
         * this work, and Go makes the same distinction for the same reason. */
        if (was_spinning) {
            bool found = false;
            for (int32_t i = 0; i < sched.gomaxprocs; i++) {
                if (burrow__runq_len(&allp[i]) != 0) {
                    found = true;
                    break;
                }
            }
            if (!found && burrow__atomic_load_acquire_u32(&sched.runqsize) != 0)
                found = true;

            if (found) {
                burrow__lock(&sched.lock);
                burrow__P *again = pidle_get();
                burrow__unlock(&sched.lock);
                if (again != NULL) {
                    acquirep(m, again);
                    m->spinning = 1;
                    burrow__atomic_add_u32(&sched.nmspinning, 1);
                    continue;
                }
            }
        }

        /* How long to sleep for: the earliest timer anybody has, or forever.
         *
         * This scan comes after the thread has stopped counting itself as a
         * searcher, and that order is the same trick as dropping the P before
         * taking the last look at the run queues. A timer set just now either
         * publishes its time before this reads it, or arrives to find a thread
         * that has not yet stopped searching and wakes it. Both sides are
         * sequentially consistent, so there is no order in which each misses the
         * other. Reading it before the transition would leave the window where
         * both do, which is a program that goes to sleep with a timer due and
         * wakes up when something else happens to it. */
        int64_t until = 0;
        for (int32_t i = 0; i < sched.gomaxprocs; i++) {
            int64_t w = burrow__timers_wake_time(&allp[i].timers);
            if (w != 0 && (until == 0 || w < until))
                until = w;
        }

        stopm(m, until);
    }
}

/* ----------------------------------------------------------------- the loop */

static void schedule(burrow__M *m) {
    for (;;) {
        burrow__G *gp = findrunnable(m);
        if (gp == NULL)
            return;

        if (m->spinning != 0) {
            /* This thread has found work, so it is no longer one of the threads
             * looking for some, and somebody else should take over looking. That
             * second part matters: without it, a burst of goroutines readied
             * while every thread was busy would leave all of them asleep. */
            m->spinning = 0;
            burrow__atomic_add_u32(&sched.nmspinning, (uint32_t)-1);
            wakep();
        }

        while (gp != NULL)
            gp = execute(m, gp);
    }
}

static void mstart(void *arg) {
    burrow__M *m = (burrow__M *)arg;

    curm = m;
    if (!burrow__context_attach(&m->g0.ctx))
        runtime_throw(BURROW_S("mstart: this thread cannot be made switchable"));

    /* Per thread alternate signal stack, so that a goroutine's stack overflow
     * has somewhere to report itself from. A thread that cannot have one still
     * runs goroutines, and an overflow on it arrives as a plain fault rather
     * than as a message, which is worse but is not worth refusing to start a
     * thread over. */
    (void)burrow__stack_guard_arm_thread();
    burrow__stack_set_current(NULL);

    burrow__P *p = m->nextp;
    m->nextp = NULL;
    if (p != NULL)
        acquirep(m, p);

    schedule(m);

    if (m->spinning != 0) {
        m->spinning = 0;
        burrow__atomic_add_u32(&sched.nmspinning, (uint32_t)-1);
    }

    burrow__stack_guard_disarm_thread();
    burrow__context_detach(&m->g0.ctx);
    curm = NULL;
}

/* Makes a thread and starts it on `p`. The M comes out of the static array, so
 * the only thing here that can fail is the thread itself. */
static void newm(burrow__P *p, bool spinning) {
    burrow__lock(&sched.lock);
    if (sched.nmcreated >= sched.gomaxprocs) {
        pidle_put(p);
        burrow__unlock(&sched.lock);
        if (spinning)
            burrow__atomic_add_u32(&sched.nmspinning, (uint32_t)-1);
        return;
    }

    burrow__M *m = &allm[sched.nmcreated];
    m->id = sched.nmcreated;
    sched.nmcreated++;
    burrow__unlock(&sched.lock);

    if (!burrow__note_init(&m->park))
        runtime_throw(BURROW_S("newm: cannot make a gate for a new thread"));

    m->nextp = p;
    m->spinning = spinning ? 1U : 0U;

    /* The thread runs on its own stack and g0 is that stack, so the size here is
     * the scheduler's own needs and not a goroutine's. Zero asks for the
     * platform default, which is megabytes and is more than this will ever
     * use. */
    if (!burrow__thread_start(&m->thread, mstart, m, 0))
        runtime_throw(BURROW_S("newm: cannot start a thread"));
}

/* ------------------------------------------------------------------ starting */

bool go_stack(Func fn, size_t stack_bytes) {
    if (fn.f == NULL)
        runtime_throw(BURROW_S("go of a nil function"));
    if (burrow__atomic_load_acquire_u32(&sched.running) == 0)
        runtime_throw(BURROW_S("go outside runtime_main"));

    if (stack_bytes == 0)
        stack_bytes = BURROW_GOROUTINE_STACK;

    burrow__M *m = curm;
    burrow__P *p = m != NULL ? m->p : NULL;

    burrow__G *newg = gfget(p, stack_bytes);
    if (newg == NULL)
        return false;

    newg->entry = fn.f;
    newg->arg = fn.env;
    newg->next = NULL;

    if (!make_context(newg, stack_bytes)) {
        gfput(p, newg);
        return false;
    }

    set_status(newg, BURROW_GRUNNABLE);
    burrow__atomic_add_u32(&sched.ngoroutine, 1);

    if (p != NULL) {
        /* The runnext slot, because the overwhelmingly common reason to start a
         * goroutine is that this one is about to wait for it. */
        runq_put(p, newg, true);
    } else {
        burrow__lock(&sched.lock);
        globrunq_put(newg);
        burrow__unlock(&sched.lock);
    }

    wakep();
    return true;
}

bool go(Func fn) {
    return go_stack(fn, 0);
}

/* ------------------------------------------------------------ park and ready */

void sched_park(SchedUnlockFn unlockf, void *lock) {
    burrow__M *m = curm;
    if (m == NULL || m->curg == NULL)
        runtime_throw(BURROW_S("sched_park: not on a goroutine"));

    m->parkunlock = unlockf;
    m->parklock = lock;
    mcall(park0);
}

void sched_ready(Goroutine *g) {
    if (g == NULL)
        runtime_throw(BURROW_S("sched_ready of a nil goroutine"));

    uint32_t waiting = (uint32_t)BURROW_GWAITING;
    if (!burrow__atomic_cas_u32(&g->status, &waiting, (uint32_t)BURROW_GRUNNABLE))
        runtime_throw(BURROW_S("sched_ready: goroutine is not parked"));

    burrow__M *m = curm;
    if (m != NULL && m->p != NULL) {
        runq_put(m->p, g, true);
    } else {
        burrow__lock(&sched.lock);
        globrunq_put(g);
        burrow__unlock(&sched.lock);
    }

    wakep();
}

void runtime_gosched(void) {
    if (curm == NULL || curm->curg == NULL)
        runtime_throw(BURROW_S("runtime_gosched: not on a goroutine"));
    mcall(gosched0);
}

void runtime_goexit(void) {
    if (curm == NULL || curm->curg == NULL)
        runtime_throw(BURROW_S("runtime_goexit: not on a goroutine"));

    /* Deferred calls run here once defer exists. */
    mcall(goexit0);
    runtime_throw(BURROW_S("runtime_goexit: a dead goroutine came back"));
}

/* ---------------------------------------------------------------- GOMAXPROCS */

int runtime_gomaxprocs(int n) {
    int was = (int)sched.gomaxprocs;
    if (was == 0)
        was = burrow__thread_ncpu();

    if (n <= 0)
        return was;
    if (burrow__atomic_load_acquire_u32(&sched.running) != 0)
        return was;

    if (n > BURROW_MAXPROCS)
        n = BURROW_MAXPROCS;
    sched.gomaxprocs = n;
    return was;
}

/* ------------------------------------------------------- starting and stopping */

static void schedinit(void) {
    if (sched.gomaxprocs <= 0) {
        int n = burrow__thread_ncpu();
        if (n > BURROW_MAXPROCS)
            n = BURROW_MAXPROCS;
        sched.gomaxprocs = n;
    }

    /* The process wide handler that turns a guard page fault into a message. A
     * runtime that cannot install it still runs and reports an overflow as a
     * fault with no explanation, which is what a C program without burrow does
     * and is not a reason to refuse to start. */
    (void)burrow__stack_guard_arm();

    for (int32_t i = sched.gomaxprocs - 1; i >= 0; i--) {
        burrow__P *p = &allp[i];
        p->id = i;
        burrow__timers_init(&p->timers);
        pidle_put(p);
    }
}

/* Gives back everything the run took: the threads, the stacks, the goroutine
 * structs and the gates. Runs on runtime_main's thread with every M joined, so
 * nothing here needs the lock and everything here is allowed to assume it is
 * alone. */
static void teardown(void) {
    Alloc *a = heap_allocator();

    burrow__G *g = sched.allg;
    while (g != NULL) {
        burrow__G *next = g->allnext;
        burrow__context_free(&g->ctx);
        stack_give_back(g);
        mem_free(a, g, sizeof(burrow__G), _Alignof(burrow__G));
        g = next;
    }

    for (int32_t i = 0; i < sched.nmcreated; i++)
        burrow__note_free(&allm[i].park);
    burrow__note_free(&sched.mainnote);

    for (int32_t i = 0; i < sched.gomaxprocs; i++) {
        burrow__timers_free(&allp[i].timers);
        allp[i] = (burrow__P){0};
    }
    for (int32_t i = 0; i < sched.nmcreated; i++)
        allm[i] = (burrow__M){0};

    sched = (Sched){0};
}

void runtime_main(Func fn) {
    if (fn.f == NULL)
        runtime_throw(BURROW_S("runtime_main of a nil function"));
    if (curm != NULL)
        runtime_throw(BURROW_S("runtime_main: called from inside a goroutine"));
    if (burrow__atomic_load_acquire_u32(&sched.running) != 0)
        runtime_throw(BURROW_S("runtime_main: already running"));

    schedinit();

    if (!burrow__note_init(&sched.mainnote))
        runtime_throw(BURROW_S("runtime_main: cannot make the gate main waits on"));

    /* The main goroutine, built by hand because `go` refuses to run before the
     * world is up and the world is not up until this exists. */
    burrow__G *mg = gfget(NULL, BURROW_GOROUTINE_STACK);
    if (mg == NULL)
        runtime_throw(BURROW_S("runtime_main: cannot make the main goroutine"));

    mg->entry = fn.f;
    mg->arg = fn.env;

    if (!make_context(mg, BURROW_GOROUTINE_STACK))
        runtime_throw(BURROW_S("runtime_main: cannot make the main goroutine"));

    set_status(mg, BURROW_GRUNNABLE);
    sched.maing = mg;
    globrunq_put(mg);
    burrow__atomic_add_u32(&sched.ngoroutine, 1);

    burrow__atomic_store_release_u32(&sched.running, 1);

    /* One thread per P, each handed its P directly rather than made to go and
     * look for one. All but the one that picks up the main goroutine will find
     * nothing and park, which is a few microseconds of startup and is what makes
     * the first `go` in the program a wakeup rather than a thread creation. */
    int32_t want = sched.gomaxprocs;
    for (int32_t i = 0; i < want; i++) {
        burrow__lock(&sched.lock);
        burrow__P *p = pidle_get();
        burrow__unlock(&sched.lock);
        if (p == NULL)
            break;
        newm(p, false);
    }

    burrow__note_sleep(&sched.mainnote);

    /* The main goroutine has returned, so the program is over in the sense Go
     * means. Setting the flag before taking the lock is what makes this safe
     * against a thread that is on its way to sleep: either it takes the lock
     * first and is on the idle list by the time this walks it, or this takes the
     * lock first and the thread sees the flag and does not park at all. */
    burrow__atomic_store_u32(&sched.stopping, 1);

    burrow__lock(&sched.lock);
    for (burrow__M *m = sched.midle; m != NULL; m = m->next)
        burrow__note_wake(&m->park);
    burrow__unlock(&sched.lock);

    for (int32_t i = 0; i < sched.nmcreated; i++)
        (void)burrow__thread_join(&allm[i].thread);

    teardown();
}
