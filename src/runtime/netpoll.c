/* The half of the netpoller that is the same on every system.
 *
 * burrow/netpoll.h says what this is for. This file is the two words per
 * descriptor that a goroutine parks on, the table those descriptors live in,
 * and the rules that decide when a wait ends. Everything that knows the name of
 * a system call is in netpoll_epoll.c or netpoll_kqueue.c, and the whole of
 * what those two have to do is defined by the six functions at the bottom of
 * the header.
 *
 * The algorithms are Go's, from runtime/netpoll.go, including the exact order
 * of the compare and swaps in netpollblock and netpollunblock, which is the
 * part of this that took Go years to get right and is not worth improvising on.
 * Read those two next to each other: between them they handle a notification
 * that arrives before the waiter parks, one that arrives while it is parking,
 * one that arrives after, and a close that races all three.
 *
 * Two things here are burrow's rather than Go's.
 *
 * The first is how a kernel event finds its descriptor. Go packs the pointer
 * and a generation count into one word using the bits a 64 bit address does not
 * use, and on a 32 bit machine it gives up and has no generation count at all.
 * This keeps a table instead and packs an index and a generation, which costs
 * one more load per event and works the same way on every machine. The load is
 * nothing next to the system call that produced the event, and a poller that is
 * only safe on 64 bit is a poller with a bug on the other machines rather than
 * a simpler one.
 *
 * The second is burrow__netpoll_drop_waiters, which exists because runtime_main
 * can return and Go's runtime cannot. See the header.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/netpoll.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/core.h"
#include "burrow/lock.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------- the two words
 *
 * What rg and wg hold. Anything larger than PD_WAIT is a goroutine.
 *
 * PD_READY is a notification nobody has taken. A waiter takes it by putting
 * PD_NIL back, which is what makes the notification worth exactly one wait and
 * not two. PD_WAIT is a goroutine that has decided to park and has not finished
 * parking, which is the state that lets a notification arriving in that moment
 * cancel the park rather than being lost behind it. */
#define PD_NIL ((uintptr_t)0)
#define PD_READY ((uintptr_t)1)
#define PD_WAIT ((uintptr_t)2)

/* The bits in pd->info, which is what a wait reads before it parks.
 *
 * They are out here in a word of their own, rather than being read from under
 * pd->mu, because every wait reads them and taking a lock per wait would put a
 * lock on the path this whole file exists to keep clear. The writes are under
 * the lock and each one is followed by a publish, so the reader sees a state
 * that was true at some moment and never a mixture of two. */
#define INFO_CLOSING 1U
#define INFO_FAILED 2U
#define INFO_RD_EXPIRED 4U
#define INFO_WD_EXPIRED 8U

/* ---------------------------------------------------------------- the table
 *
 * Where descriptors live, and why a kernel event can always be believed.
 *
 * An event can arrive for a descriptor that was closed while the event was
 * sitting in the kernel's ready list. Whatever that event points at has to
 * still be readable, so a descriptor is never given back to the allocator: it
 * goes on a free list and is used again. That much is Go's pollCache.
 *
 * Being readable is not the same as being the right one, and an event for the
 * closed connection must not wake the goroutine that is now reading the
 * connection which reused the slot. So the slot carries a generation that is
 * raised every time it goes back on the free list, and an event carries the
 * generation the descriptor was on when it was registered. A mismatch is an
 * event about a connection that no longer exists, and it is dropped.
 *
 * The table is two levels. The top is a fixed array of block pointers in BSS,
 * which costs address space and not memory until a block is made, and the
 * bottom is blocks of descriptors from the heap. It is fixed because a table
 * that grows has to be read by a thread draining kernel events at the same
 * moment it is written by a thread opening a connection, and the cost of that
 * is a great deal more than the 128 kilobytes of untouched BSS it saves. The
 * same trade as the P array in the scheduler.
 *
 * A million descriptors is the ceiling that comes out of it, which is above
 * what any system will give a process file descriptors for.
 *
 * The last block of the table is never made, which is worth the one line it
 * costs. It leaves every handle with at least one index bit clear, so a word of
 * all ones is a word no descriptor will ever be given, and a backend that needs
 * a value meaning something other than a descriptor has one. Both of them do:
 * that is how the wakeup the scheduler uses to cut a poll short is told apart
 * from a connection when both come out of the same call. */
#define POLL_INDEX_BITS 20
#define POLL_MAX_DESCS (((uint32_t)1) << POLL_INDEX_BITS)
#define POLL_INDEX_MASK (POLL_MAX_DESCS - 1U)
#define POLL_PER_BLOCK 64U
#define POLL_MAX_BLOCKS ((POLL_MAX_DESCS / POLL_PER_BLOCK) - 1U)

/* What is left of a word once the index has taken the bottom of it. Forty four
 * bits on a 64 bit machine and twelve on a 32 bit one. Twelve is enough: it
 * takes four thousand and ninety six opens and closes of one slot for a
 * generation to come round again, and the event that would then be believed has
 * to have been sitting in the kernel for all of them. */
#define POLL_GEN_MASK (((uintptr_t)-1) >> POLL_INDEX_BITS)

static burrow__Lock cache_lock;
static burrow__PollDesc *cache_first;
static void *cache_blocks[POLL_MAX_BLOCKS];
static uint32_t cache_ndescs;

/* Started once, on the first descriptor anybody opens. */
static burrow__Lock init_lock;
static uint32_t init_done;

/* How many goroutines are parked on a descriptor right now. */
static uint32_t netpoll_nwaiters;

static uintptr_t handle_of(uint32_t index, uint32_t gen) {
    return (((uintptr_t)gen & POLL_GEN_MASK) << POLL_INDEX_BITS) | (uintptr_t)index;
}

/* The descriptor at `index`, which the caller has to know exists. */
static burrow__PollDesc *desc_at(uint32_t index) {
    burrow__PollDesc *block = (burrow__PollDesc *)burrow__atomic_load_acquire_ptr(
        &cache_blocks[index / POLL_PER_BLOCK]);
    if (block == NULL)
        return NULL;
    return &block[index % POLL_PER_BLOCK];
}

/* The descriptor an event is about, or NULL if that event is about a use of the
 * slot which has since ended. */
static burrow__PollDesc *desc_of_handle(uintptr_t handle) {
    uint32_t index = (uint32_t)(handle & POLL_INDEX_MASK);
    if (index >= burrow__atomic_load_acquire_u32(&cache_ndescs))
        return NULL;

    burrow__PollDesc *pd = desc_at(index);
    if (pd == NULL)
        return NULL;

    uintptr_t gen = handle >> POLL_INDEX_BITS;
    if ((((uintptr_t)burrow__atomic_load_acquire_u32(&pd->gen)) & POLL_GEN_MASK) != gen)
        return NULL;
    return pd;
}

/* One more block of descriptors, with the cache lock held. False means the
 * allocator said no or the table is full, and both of those come back to the
 * caller as a failure to open rather than as a stopped program: a server that
 * cannot take one more connection can still serve the ones it has. */
static bool cache_grow(void) {
    uint32_t nblocks = cache_ndescs / POLL_PER_BLOCK;
    if (nblocks >= POLL_MAX_BLOCKS)
        return false;

    burrow__PollDesc *block =
        BURROW_NEW_N(heap_allocator(), burrow__PollDesc, POLL_PER_BLOCK);
    if (block == NULL)
        return false;

    for (uint32_t i = 0; i < POLL_PER_BLOCK; i++) {
        burrow__PollDesc *pd = &block[i];
        pd->index = nblocks * POLL_PER_BLOCK + i;
        pd->link = cache_first;
        cache_first = pd;

        /* Once for the life of the slot, and not again when it is reused. A
         * timer that fired and has not been thrown out of its P's heap yet is
         * still in that heap, and arming it again from there is the cheap path
         * rather than something to undo. */
        burrow__timer_init(&pd->rt, NULL, NULL);
        burrow__timer_init(&pd->wt, NULL, NULL);
    }

    /* The block goes up before the count does, and the count is what a reader
     * checks an index against, so a reader that sees an index in range is
     * looking at a block that is already there. */
    burrow__atomic_store_release_ptr(&cache_blocks[nblocks], block);
    burrow__atomic_store_release_u32(&cache_ndescs, cache_ndescs + POLL_PER_BLOCK);
    return true;
}

static burrow__PollDesc *cache_alloc(void) {
    burrow__lock(&cache_lock);
    if (cache_first == NULL && !cache_grow()) {
        burrow__unlock(&cache_lock);
        return NULL;
    }

    burrow__PollDesc *pd = cache_first;
    cache_first = pd->link;
    pd->link = NULL;
    burrow__unlock(&cache_lock);
    return pd;
}

static void cache_free(burrow__PollDesc *pd) {
    /* The generation goes up first. Any event still in the kernel that names
     * this slot names the generation it was registered on, so raising it here
     * is what turns every one of those into something to drop. */
    burrow__atomic_store_u32(&pd->gen, burrow__atomic_load_relaxed_u32(&pd->gen) + 1U);

    burrow__lock(&cache_lock);
    pd->link = cache_first;
    cache_first = pd;
    burrow__unlock(&cache_lock);
}

/* ------------------------------------------------------------- the waiters
 *
 * The count the scheduler reads to decide whether sleeping inside the poller is
 * worth doing. A delta rather than a set of increments because a thread
 * draining kernel events takes several goroutines off in one pass and the count
 * only has to be right at the end of it. */
static void adjust_waiters(int32_t delta) {
    if (delta != 0)
        burrow__atomic_add_u32(&netpoll_nwaiters, (uint32_t)delta);
}

uint32_t burrow__netpoll_waiters(void) {
    return burrow__atomic_load_acquire_u32(&netpoll_nwaiters);
}

/* -------------------------------------------------------------- the errors */

/* Copies what is under the lock into the word a wait reads without it. Called
 * with pd->mu held, after anything that changes what the answer would be. */
static void publish_info(burrow__PollDesc *pd) {
    uint32_t info = 0;
    if (pd->closing)
        info |= INFO_CLOSING;
    if (pd->rd < 0)
        info |= INFO_RD_EXPIRED;
    if (pd->wd < 0)
        info |= INFO_WD_EXPIRED;

    /* The failure bit is not under the lock and is not owned by this. It is set
     * by whichever thread drained the event that said the descriptor is in a
     * bad way, so it is carried through rather than recomputed. */
    for (;;) {
        uint32_t old = burrow__atomic_load_acquire_u32(&pd->info);
        uint32_t want = (old & INFO_FAILED) | info;
        if (old == want || burrow__atomic_cas_u32(&pd->info, &old, want))
            return;
    }
}

static void set_failed(burrow__PollDesc *pd, bool failed) {
    for (;;) {
        uint32_t old = burrow__atomic_load_acquire_u32(&pd->info);
        uint32_t want = failed ? (old | INFO_FAILED) : (old & ~INFO_FAILED);
        if (old == want || burrow__atomic_cas_u32(&pd->info, &old, want))
            return;
    }
}

static burrow__PollStatus check_err(burrow__PollDesc *pd, uint32_t mode) {
    uint32_t info = burrow__atomic_load_acquire_u32(&pd->info);

    if ((info & INFO_CLOSING) != 0)
        return BURROW_POLL_CLOSED;

    /* The deadline that has passed is the one for this direction and not the
     * other, which is why the two are separate bits rather than one. A
     * connection with a read deadline that has gone by is still writable. */
    uint32_t expired = (mode == BURROW_POLL_READ) ? (uint32_t)INFO_RD_EXPIRED
                                                  : (uint32_t)INFO_WD_EXPIRED;
    if ((info & expired) != 0)
        return BURROW_POLL_TIMEOUT;

    /* Only the reader hears about it, which is Go's rule. A write that is going
     * to fail is about to say why in its own error, and that answer is more
     * specific than this one. */
    if (mode == BURROW_POLL_READ && (info & INFO_FAILED) != 0)
        return BURROW_POLL_UNPOLLABLE;

    return BURROW_POLL_READY;
}

/* ------------------------------------------------------- parking and waking */

/* The last thing the goroutine does before it is out of reach on this thread,
 * handed to sched_park so that it happens after the scheduler can no longer be
 * beaten to the wakeup.
 *
 * False means do not park after all, and it is the answer whenever the word is
 * no longer PD_WAIT, which is a notification that arrived in the moment between
 * deciding to park and getting there. */
static bool park_commit(Goroutine *g, void *word) {
    uintptr_t *gpp = (uintptr_t *)word;
    uintptr_t wait = PD_WAIT;

    if (!burrow__atomic_cas_uptr(gpp, &wait, (uintptr_t)g))
        return false;

    adjust_waiters(1);
    return true;
}

/* Waits for one notification on one of the two words. True means the descriptor
 * is ready, false means the wait was ended by something else and the caller
 * should look at why.
 *
 * The loop at the top is the whole of the handshake with the other side. Taking
 * a notification that is already there ends the wait without parking at all,
 * which is the common case on a busy connection. Anything else at all in the
 * word is a second goroutine waiting in the same direction, which is a bug in
 * the program above and is worth stopping for rather than working around.
 *
 * `waitio` says park whatever the error bits say, which is a wait that wants the
 * notification itself rather than an answer about the connection. There is one
 * caller and it is burrow__poll_wait_canceled: a closed or timed out descriptor
 * is exactly the state that wait is called in, so the usual check would send it
 * straight back without the notification it came for. */
static bool netpoll_block(burrow__PollDesc *pd, uint32_t mode, bool waitio) {
    uintptr_t *gpp = (mode == BURROW_POLL_READ) ? &pd->rg : &pd->wg;

    for (;;) {
        uintptr_t ready = PD_READY;
        if (burrow__atomic_cas_uptr(gpp, &ready, PD_NIL))
            return true;

        uintptr_t nil = PD_NIL;
        if (burrow__atomic_cas_uptr(gpp, &nil, PD_WAIT))
            break;

        uintptr_t got = burrow__atomic_load_acquire_uptr(gpp);
        if (got != PD_READY && got != PD_NIL)
            runtime_throw(BURROW_S(
                "netpoll: two goroutines waiting on one descriptor in one direction"));
    }

    /* The second look at the error bits, and it has to be here rather than only
     * at the top. A close does the opposite order of what this does: it sets
     * closing, publishes it, and then looks at this word. So a close that
     * happened while this was getting as far as PD_WAIT is one whose look
     * found nothing, and this is the look that catches it. */
    if (waitio || check_err(pd, mode) == BURROW_POLL_READY)
        sched_park(park_commit, gpp);

    uintptr_t old = burrow__atomic_swap_uptr(gpp, PD_NIL);
    if (old > PD_WAIT)
        runtime_throw(
            BURROW_S("netpoll: a descriptor woke up with a goroutine still on it"));

    return old == PD_READY;
}

/* Takes whoever is waiting in one direction off the descriptor and answers
 * them, or NULL if there is nobody.
 *
 * `ioready` says this is the kernel reporting readiness, which is the case that
 * leaves PD_READY behind for a waiter that has not arrived yet. A close or a
 * deadline leaves PD_NIL instead, because those are not a reason for the next
 * wait to return at once. */
static burrow__G *netpoll_unblock(burrow__PollDesc *pd, uint32_t mode, bool ioready,
                                  int32_t *delta) {
    uintptr_t *gpp = (mode == BURROW_POLL_READ) ? &pd->rg : &pd->wg;

    for (;;) {
        uintptr_t old = burrow__atomic_load_acquire_uptr(gpp);
        if (old == PD_READY)
            return NULL;
        if (old == PD_NIL && !ioready)
            return NULL;

        uintptr_t want = ioready ? PD_READY : PD_NIL;
        if (burrow__atomic_cas_uptr(gpp, &old, want)) {
            if (old == PD_NIL || old == PD_WAIT)
                return NULL;
            *delta -= 1;
            return (burrow__G *)old;
        }
    }
}

void burrow__netpoll_ready(burrow__GQueue *out, uintptr_t handle, uint32_t mode,
                           bool failed) {
    burrow__PollDesc *pd = desc_of_handle(handle);
    if (pd == NULL)
        return;

    set_failed(pd, failed);

    int32_t delta = 0;
    burrow__G *rg = NULL;
    burrow__G *wg = NULL;
    if ((mode & BURROW_POLL_READ) != 0)
        rg = netpoll_unblock(pd, BURROW_POLL_READ, true, &delta);
    if ((mode & BURROW_POLL_WRITE) != 0)
        wg = netpoll_unblock(pd, BURROW_POLL_WRITE, true, &delta);

    if (rg != NULL)
        burrow__gqueue_push(out, rg);
    if (wg != NULL)
        burrow__gqueue_push(out, wg);

    adjust_waiters(delta);
}

/* ------------------------------------------------------------- the deadlines
 *
 * A deadline is a timer that marks one direction as expired and wakes whoever
 * is waiting in it. Everything a wait does with that is already written: the
 * mark is a bit in pd->info and check_err reads it, so the read and write paths
 * cost nothing at all for a descriptor that has no deadline on it and one
 * predictable branch for a descriptor that has.
 *
 * Go carries a sequence number per direction, bumped on every change, copied
 * into the timer when it is armed and compared when it fires, so that a timer
 * which went off and has not had its turn yet cannot act on a deadline that has
 * since moved. This does not have one, and the reason is worth stating because
 * the absence looks like an oversight.
 *
 * What the sequence number is really asking is whether the deadline this timer
 * is about is still the one that is due, and that question can be answered from
 * the deadline itself: a timer only acts when the deadline it finds under the
 * lock is a real one and has actually passed. A deadline moved later is not
 * past, so the late timer does nothing and the new arming fires in its own
 * time. A deadline cleared is not a deadline, so it does nothing. A deadline
 * moved earlier is past, and acting on it is right rather than stale, because
 * being past is the whole of what makes a deadline worth acting on. Even a slot
 * closed and reused underneath the timer lands on the same answer, since the
 * new connection's deadline either has passed, in which case it was due, or has
 * not, in which case nothing happens. One clock read against a counter and the
 * word of state that goes with it. */

static void deadline_fired(burrow__PollDesc *pd, bool read, bool write) {
    burrow__lock(&pd->mu);

    int64_t now = burrow__nanotime();
    bool rexp = read && pd->rd > 0 && pd->rd <= now;
    bool wexp = write && pd->wd > 0 && pd->wd <= now;

    if (!rexp && !wexp) {
        burrow__unlock(&pd->mu);
        return;
    }

    if (rexp)
        pd->rd = -1;
    if (wexp)
        pd->wd = -1;
    publish_info(pd);

    /* False, not true, because a deadline is not the kernel saying there is
     * something to be had. Leaving PD_READY behind would make the next wait in
     * that direction come straight back claiming readiness that nobody was
     * told about. */
    int32_t delta = 0;
    burrow__G *rg = rexp ? netpoll_unblock(pd, BURROW_POLL_READ, false, &delta) : NULL;
    burrow__G *wg = wexp ? netpoll_unblock(pd, BURROW_POLL_WRITE, false, &delta) : NULL;
    burrow__unlock(&pd->mu);

    if (rg != NULL)
        sched_ready(rg);
    if (wg != NULL)
        sched_ready(wg);
    adjust_waiters(delta);
}

static void read_deadline(void *arg, int64_t delay) {
    (void)delay;
    deadline_fired((burrow__PollDesc *)arg, true, false);
}

static void write_deadline(void *arg, int64_t delay) {
    (void)delay;
    deadline_fired((burrow__PollDesc *)arg, false, true);
}

/* Both at once, which is what SetDeadline asks for and is the reason the two
 * timers are not simply always both armed. */
static void both_deadlines(void *arg, int64_t delay) {
    (void)delay;
    deadline_fired((burrow__PollDesc *)arg, true, true);
}

/* Stops both timers, with pd->mu held. */
static void deadlines_stop(burrow__PollDesc *pd) {
    if (pd->rt_armed) {
        (void)burrow__timer_stop(&pd->rt);
        pd->rt_armed = false;
    }
    if (pd->wt_armed) {
        (void)burrow__timer_stop(&pd->wt);
        pd->wt_armed = false;
    }
}

/* ------------------------------------------------------------- starting up */

bool burrow__netpoll_inited(void) {
    return burrow__atomic_load_acquire_u32(&init_done) != 0;
}

/* Makes sure there is a poller, and answers whether this build has one at all.
 *
 * Lazy, so that a program which never opens a descriptor never makes an epoll,
 * a kqueue or a completion port, which is most programs that link a library. Go
 * does the same thing from the same place, in poll_runtime_pollServerInit. */
static bool netpoll_start(void) {
#if defined(BURROW_NETPOLL_NONE)
    return false;
#else
    if (burrow__netpoll_inited())
        return true;

    burrow__lock(&init_lock);
    if (burrow__atomic_load_relaxed_u32(&init_done) == 0) {
        burrow__netpoll_backend_init();
        burrow__atomic_store_release_u32(&init_done, 1);
    }
    burrow__unlock(&init_lock);
    return true;
#endif
}

void burrow__netpoll(int64_t delay, burrow__GQueue *out) {
    if (!burrow__netpoll_inited())
        return;
    burrow__netpoll_backend_wait(delay, out);
}

void burrow__netpoll_break(void) {
    if (!burrow__netpoll_inited())
        return;
    burrow__netpoll_backend_break();
}

/* ------------------------------------------------------------- the four calls
 *
 * What a connection is built out of. */

int burrow__poll_open(burrow__PollFd fd, burrow__PollDesc **out) {
    *out = NULL;

    if (!netpoll_start())
        return ENOSYS;

    burrow__PollDesc *pd = cache_alloc();
    if (pd == NULL)
        return ENOMEM;

    /* The slot came off the free list, so nobody else has it and nothing here
     * needs the lock. The check is that it was given back in the state it is
     * supposed to be given back in, which is a check on burrow rather than on
     * the caller, and it is here because getting it wrong shows up as a
     * goroutine woken by a connection it has never heard of. */
    if (burrow__atomic_load_relaxed_uptr(&pd->rg) > PD_READY ||
        burrow__atomic_load_relaxed_uptr(&pd->wg) > PD_READY)
        runtime_throw(
            BURROW_S("netpoll: a free descriptor with a goroutine parked on it"));

    pd->fd = fd;
    pd->closing = false;
    pd->rd = 0;
    pd->wd = 0;
    pd->rt_armed = false;
    pd->wt_armed = false;
    pd->handle = handle_of(pd->index, burrow__atomic_load_relaxed_u32(&pd->gen));
    burrow__atomic_store_relaxed_uptr(&pd->rg, PD_NIL);
    burrow__atomic_store_relaxed_uptr(&pd->wg, PD_NIL);
    burrow__atomic_store_u32(&pd->info, 0);

    int err = burrow__netpoll_backend_open(fd, pd->handle);
    if (err != 0) {
        cache_free(pd);
        return err;
    }

    *out = pd;
    return 0;
}

#if defined(BURROW_NETPOLL_COMPLETION)

void burrow__poll_op_init(burrow__PollOp *op, burrow__PollDesc *pd, uint32_t mode) {
    if (mode != BURROW_POLL_READ && mode != BURROW_POLL_WRITE)
        runtime_throw(
            BURROW_S("netpoll: an operation for neither reading nor writing"));

    *op = (burrow__PollOp){0};
    op->desc = pd->handle;
    op->mode = mode;
}

#endif /* BURROW_NETPOLL_COMPLETION */

void burrow__poll_close(burrow__PollDesc *pd) {
    if (!pd->closing)
        runtime_throw(
            BURROW_S("netpoll: a descriptor closed without being unblocked first"));
    if (burrow__atomic_load_acquire_uptr(&pd->rg) > PD_READY ||
        burrow__atomic_load_acquire_uptr(&pd->wg) > PD_READY)
        runtime_throw(
            BURROW_S("netpoll: a descriptor closed with a goroutine parked on it"));

    (void)burrow__netpoll_backend_close(pd->fd);
    cache_free(pd);
}

burrow__PollStatus burrow__poll_wait(burrow__PollDesc *pd, uint32_t mode) {
    if (mode != BURROW_POLL_READ && mode != BURROW_POLL_WRITE)
        runtime_throw(BURROW_S("netpoll: a wait for neither reading nor writing"));
    if (sched_current() == NULL)
        runtime_throw(BURROW_S("netpoll: a wait on a thread that is not a goroutine"));

    burrow__PollStatus st = check_err(pd, mode);
    if (st != BURROW_POLL_READY)
        return st;

    /* The loop is for a wait that ended without a notification and without a
     * reason. That happens when a deadline fires and is moved again before the
     * goroutine gets a turn, so the state it woke up to complain about is no
     * longer there. There is nothing to report, so it waits again. */
    while (!netpoll_block(pd, mode, false)) {
        st = check_err(pd, mode);
        if (st != BURROW_POLL_READY)
            return st;
    }
    return BURROW_POLL_READY;
}

#if defined(BURROW_NETPOLL_COMPLETION)

void burrow__poll_wait_canceled(burrow__PollDesc *pd, uint32_t mode) {
    if (mode != BURROW_POLL_READ && mode != BURROW_POLL_WRITE)
        runtime_throw(
            BURROW_S("netpoll: a cancelled wait for neither reading nor writing"));

    /* No answer and no way out other than the notification, which is the point.
     * The descriptor is closed or its deadline has gone by, so a wait that
     * looked at the error bits would come straight back and the caller would go
     * on to free an operation the kernel is still holding. */
    while (!netpoll_block(pd, mode, true)) {
    }
}

#endif /* BURROW_NETPOLL_COMPLETION */

void burrow__poll_unblock(burrow__PollDesc *pd) {
    burrow__lock(&pd->mu);
    if (pd->closing) {
        burrow__unlock(&pd->mu);
        runtime_throw(BURROW_S("netpoll: a descriptor unblocked twice"));
    }
    pd->closing = true;

    /* Published before the two words are looked at, and that order is the other
     * half of the one in netpoll_block. Between them there is no way for a
     * goroutine to be parked here with nobody having seen it. */
    publish_info(pd);

    int32_t delta = 0;
    burrow__G *rg = netpoll_unblock(pd, BURROW_POLL_READ, false, &delta);
    burrow__G *wg = netpoll_unblock(pd, BURROW_POLL_WRITE, false, &delta);

    /* The timers come off here rather than in burrow__poll_close, because this
     * is the call that has the lock and is the one that happens first. A timer
     * left armed on a descriptor that is going back on the free list is a timer
     * that fires on somebody else's connection. */
    deadlines_stop(pd);
    burrow__unlock(&pd->mu);

    if (rg != NULL)
        sched_ready(rg);
    if (wg != NULL)
        sched_ready(wg);
    adjust_waiters(delta);
}

bool burrow__poll_set_deadline(burrow__PollDesc *pd, int64_t when, uint32_t mode) {
    if (mode == 0 || (mode & ~(BURROW_POLL_READ | BURROW_POLL_WRITE)) != 0)
        runtime_throw(BURROW_S("netpoll: a deadline for neither reading nor writing"));
    if (sched_current() == NULL)
        runtime_throw(
            BURROW_S("netpoll: a deadline set on a thread that is not a goroutine"));

    /* Everything already past becomes the one value that means past, so that
     * nothing below this line has to ask twice whether a reading from the clock
     * has been overtaken by the clock. */
    if (when != 0 && when <= burrow__nanotime())
        when = -1;

    burrow__lock(&pd->mu);

    /* A deadline on a descriptor that is being closed is not an error and is
     * not worth arming anything for. Every wait on it answers closed already,
     * which is a better answer than a timeout. */
    if (pd->closing) {
        burrow__unlock(&pd->mu);
        return true;
    }

    int64_t rd0 = pd->rd;
    int64_t wd0 = pd->wd;
    bool combo0 = rd0 > 0 && rd0 == wd0;

    if ((mode & BURROW_POLL_READ) != 0)
        pd->rd = when;
    if ((mode & BURROW_POLL_WRITE) != 0)
        pd->wd = when;
    publish_info(pd);

    /* One timer when both deadlines are the same instant, which is what the two
     * together mode always produces and is therefore the usual case. The read
     * timer is the one that carries both. */
    bool combo = pd->rd > 0 && pd->rd == pd->wd;
    burrow__TimerFn rf = combo ? both_deadlines : read_deadline;
    bool ok = true;

    if (!pd->rt_armed || pd->rd != rd0 || combo != combo0) {
        if (pd->rd > 0) {
            pd->rt_armed = burrow__timer_reset(&pd->rt, pd->rd, 0, rf, pd, NULL);
            ok = ok && pd->rt_armed;
        } else if (pd->rt_armed) {
            (void)burrow__timer_stop(&pd->rt);
            pd->rt_armed = false;
        }
    }

    if (!pd->wt_armed || pd->wd != wd0 || combo != combo0) {
        if (pd->wd > 0 && !combo) {
            pd->wt_armed =
                burrow__timer_reset(&pd->wt, pd->wd, 0, write_deadline, pd, NULL);
            ok = ok && pd->wt_armed;
        } else if (pd->wt_armed) {
            (void)burrow__timer_stop(&pd->wt);
            pd->wt_armed = false;
        }
    }

    /* A deadline that was already past when it was set. Nothing is going to
     * fire, so whoever is waiting is woken here instead. */
    int32_t delta = 0;
    burrow__G *rg =
        pd->rd < 0 ? netpoll_unblock(pd, BURROW_POLL_READ, false, &delta) : NULL;
    burrow__G *wg =
        pd->wd < 0 ? netpoll_unblock(pd, BURROW_POLL_WRITE, false, &delta) : NULL;
    burrow__unlock(&pd->mu);

    if (rg != NULL)
        sched_ready(rg);
    if (wg != NULL)
        sched_ready(wg);
    adjust_waiters(delta);
    return ok;
}

void burrow__netpoll_drop_waiters(void) {
    uint32_t n = burrow__atomic_load_acquire_u32(&cache_ndescs);

    for (uint32_t i = 0; i < n; i++) {
        burrow__PollDesc *pd = desc_at(i);
        if (pd == NULL)
            continue;
        (void)burrow__atomic_swap_uptr(&pd->rg, PD_NIL);
        (void)burrow__atomic_swap_uptr(&pd->wg, PD_NIL);
    }

    burrow__atomic_store_u32(&netpoll_nwaiters, 0);
}

/* ----------------------------------------------------------- with no backend
 *
 * The two web targets, until each of them has one. Nothing above
 * ever reaches these, because netpoll_start answers false there and every entry
 * point either asks it first or asks whether the poller has started. They are
 * written out anyway so that the library links, and they throw rather than
 * returning something plausible, because a call that gets here is a bug in the
 * scheduler and not a platform that cannot do I/O. */
#if defined(BURROW_NETPOLL_NONE)

void burrow__netpoll_backend_init(void) {
    runtime_throw(BURROW_S("netpoll: this platform has no poller yet"));
}

int burrow__netpoll_backend_open(burrow__PollFd fd, uintptr_t handle) {
    (void)fd;
    (void)handle;
    return ENOSYS;
}

int burrow__netpoll_backend_close(burrow__PollFd fd) {
    (void)fd;
    return 0;
}

void burrow__netpoll_backend_wait(int64_t delay, burrow__GQueue *out) {
    (void)delay;
    (void)out;
    runtime_throw(BURROW_S("netpoll: a poll on a platform with no poller"));
}

void burrow__netpoll_backend_break(void) {
    runtime_throw(BURROW_S("netpoll: a wakeup on a platform with no poller"));
}

#endif /* BURROW_NETPOLL_NONE */
