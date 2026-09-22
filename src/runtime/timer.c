/* The timers. burrow/timer.h says what they are, this is how they work.
 *
 * Every algorithm here is Go's, from runtime/time.go. The heap is a plain four
 * way heap and there is nothing subtle in it. Everything subtle in this file is
 * about the fact that the thread stopping a timer is almost never the thread
 * holding the heap it is in, and the three rules that come out of that are worth
 * having in one place:
 *
 *   Nobody but the owning P takes a timer out of a heap. A stop marks the timer
 *   ZOMBIE and leaves it there. The owner drops it on its next pass, and until
 *   then it is a timer in a heap that is not going to fire.
 *
 *   The heap's copy of `when` is allowed to be stale, and the MODIFIED bit is
 *   what says so. A thread moving a timer writes t->when under the timer's own
 *   lock and sets the bit, and the heap gets put back in order later by whoever
 *   holds it. Between those two moments the heap is not a heap, and every reader
 *   handles that by looking at the bits before trusting the order.
 *
 *   The two published minimums, min_when_heap and min_when_modified, are read
 *   without any lock by threads deciding how long to sleep. Together they are
 *   allowed to be earlier than the truth and never later. Early costs a thread
 *   waking up to find nothing to do. Late is a timer that does not fire.
 *
 * The lock order is the set and then the timer, never the other way round. The
 * scan in adjust holds the set and takes each timer in turn, so a thread holding
 * a timer and reaching for the set it is in would be the other half of a
 * deadlock. This is why maybe_add drops the timer's lock before taking the
 * set's, even though it has just finished with it.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/timer.h"

#include "burrow/atomic.h"
#include "burrow/clock.h"
#include "burrow/core.h"
#include "burrow/lock.h"
#include "burrow/mem.h"
#include "burrow/mem/heap.h"
#include "burrow/platform.h"
#include "burrow/runtime.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Children per node. Go's number. Two is a binary heap and is a third deeper,
 * four puts a node's children in one or two cache lines, and eight starts
 * costing more comparisons than it saves in depth. */
#define TIMER_HEAP_N 4U

/* The first allocation, in entries. Sixteen bytes each on a 64 bit machine, so
 * a P that uses timers at all pays 128 bytes for the privilege and a P that
 * never does pays nothing, because the array is only allocated on the first
 * add. */
#define HEAP_MIN_CAP 8U

/* The largest `when` there is, which is what a repeating timer's next firing is
 * clamped to when the addition would overflow. Go's maxWhen. */
#define MAX_WHEN INT64_MAX

/* ----------------------------------------------------------- the small stuff */

/* Every `when` is a burrow__nanotime reading and is positive, so the published
 * minimums can be unsigned, which is the width burrow has atomics for, and zero
 * is free to mean that there is nothing. */
static uint64_t pack_when(int64_t when) {
    return (uint64_t)when;
}

static int64_t unpack_when(uint64_t when) {
    return (int64_t)when;
}

static void timers_lock(burrow__Timers *ts) {
    burrow__lock(&ts->mu);
}

/* Publishes the length on the way out rather than at every change, so that a
 * pass which takes the only timer out and puts it straight back reads as one
 * throughout. A reader that saw the zero would conclude the P has no timers and
 * has no reason to look again. */
static void timers_unlock(burrow__Timers *ts) {
    burrow__atomic_store_u32(&ts->alen, ts->len);
    burrow__unlock(&ts->mu);
}

static void timer_lock(burrow__Timer *t) {
    burrow__lock(&t->mu);
}

/* Publishes the state bits on the way out, which is what lets a pass over a heap
 * decide that a timer needs nothing without taking its lock. */
static void timer_unlock(burrow__Timer *t) {
    burrow__atomic_store_u32(&t->astate, t->state);
    burrow__unlock(&t->mu);
}

static uint32_t timer_astate(burrow__Timer *t) {
    return burrow__atomic_load_u32(&t->astate);
}

/* The heap is in a state it cannot get into on its own, which in practice means
 * two goroutines used one timer at once without saying so. Go throws here rather
 * than letting an index run off the end, because the message from the throw
 * names the problem and the message from the crash names an unrelated line in
 * the scheduler. */
static BURROW_NORETURN void bad_timer(void) {
    runtime_throw(BURROW_S("timer data corruption"));
}

/* -------------------------------------------------------------- the two mins */

static void update_min_when_heap(burrow__Timers *ts) {
    if (ts->len == 0)
        burrow__atomic_store_u64(&ts->min_when_heap, 0);
    else
        burrow__atomic_store_u64(&ts->min_when_heap, pack_when(ts->heap[0].when));
}

/* Lowers min_when_modified to `when` if it is not already at least that low.
 * Called without the set's lock, which is the whole reason this field exists. */
static void update_min_when_modified(burrow__Timers *ts, int64_t when) {
    uint64_t w = pack_when(when);

    for (;;) {
        uint64_t old = burrow__atomic_load_u64(&ts->min_when_modified);
        if (old != 0 && old < w)
            return;
        if (burrow__atomic_cas_u64(&ts->min_when_modified, &old, w))
            return;
    }
}

int64_t burrow__timers_wake_time(burrow__Timers *ts) {
    /* The order of these two loads matters and is the other half of the order in
     * adjust. adjust lowers min_when_heap to cover everything it is about to
     * clear out of min_when_modified, and then clears it. Reading modified first
     * means that a zero read here was written after the heap minimum already
     * covered what it stood for, so the heap minimum read second is never
     * missing anything. Reading them the other way round can see the old heap
     * minimum and the new zero, which is a wake time that is too late, and too
     * late is a timer that does not fire. */
    uint64_t modified = burrow__atomic_load_u64(&ts->min_when_modified);
    uint64_t when = burrow__atomic_load_u64(&ts->min_when_heap);

    if (when == 0 || (modified != 0 && modified < when))
        when = modified;
    return unpack_when(when);
}

uint32_t burrow__timers_len(burrow__Timers *ts) {
    return burrow__atomic_load_u32(&ts->alen);
}

/* ------------------------------------------------------------------ the heap
 *
 * All of these want the set's lock held. They index the array by hand and check
 * the indices by hand, because an out of range index here means the structure
 * has been corrupted by racy use and the useful thing to do is say so. */

static bool tw_less(burrow__TimerWhen a, burrow__TimerWhen b) {
    return a.when < b.when;
}

static void sift_up(burrow__Timers *ts, uint32_t i) {
    burrow__TimerWhen *heap = ts->heap;

    if (i >= ts->len)
        bad_timer();

    burrow__TimerWhen tw = heap[i];
    if (tw.when <= 0)
        bad_timer();

    while (i > 0) {
        uint32_t parent = (i - 1U) / TIMER_HEAP_N;
        if (!tw_less(tw, heap[parent]))
            break;
        heap[i] = heap[parent];
        i = parent;
    }
    if (heap[i].t != tw.t)
        heap[i] = tw;
}

static void sift_down(burrow__Timers *ts, uint32_t i) {
    burrow__TimerWhen *heap = ts->heap;
    uint32_t n = ts->len;

    if (i >= n)
        bad_timer();
    if (i * TIMER_HEAP_N + 1U >= n)
        return;

    burrow__TimerWhen tw = heap[i];
    if (tw.when <= 0)
        bad_timer();

    for (;;) {
        uint32_t first = i * TIMER_HEAP_N + 1U;
        if (first >= n)
            break;

        uint32_t last = first + TIMER_HEAP_N;
        if (last > n)
            last = n;

        /* The smallest of the four children, and only if it is smaller than the
         * one being moved down. Comparing all four and then comparing once
         * against the parent is a comparison cheaper than comparing against the
         * parent each time. */
        burrow__TimerWhen smallest = tw;
        uint32_t at = n;
        for (uint32_t j = first; j < last; j++) {
            if (tw_less(heap[j], smallest)) {
                smallest = heap[j];
                at = j;
            }
        }
        if (at == n)
            break;

        heap[i] = heap[at];
        i = at;
    }
    if (heap[i].t != tw.t)
        heap[i] = tw;
}

/* Puts the whole array back in heap order in one pass, which is linear in the
 * number of timers rather than the n log n of adding them one at a time. What
 * wants it is adjust, which has just moved an unknown number of entries. */
static void init_heap(burrow__Timers *ts) {
    if (ts->len <= 1)
        return;

    /* The parent of the last entry is the last one that can have children. */
    uint32_t i = (ts->len - 2U) / TIMER_HEAP_N;
    for (;;) {
        sift_down(ts, i);
        if (i == 0)
            break;
        i--;
    }
}

/* Makes room for one more. False means the allocator said no, and the caller has
 * to pass that on rather than lose the timer. */
static bool heap_grow(burrow__Timers *ts) {
    if (ts->len < ts->cap)
        return true;

    uint32_t cap = ts->cap == 0 ? HEAP_MIN_CAP : ts->cap * 2U;
    if (cap <= ts->cap)
        return false;

    size_t each = sizeof(burrow__TimerWhen);
    if ((size_t)cap > SIZE_MAX / each)
        return false;

    Alloc *a = heap_allocator();
    burrow__TimerWhen *heap =
        mem_realloc(a, ts->heap, (size_t)ts->cap * each, (size_t)cap * each,
                    _Alignof(burrow__TimerWhen));
    if (heap == NULL)
        return false;

    ts->heap = heap;
    ts->cap = cap;
    return true;
}

/* Puts a timer in the heap. The caller has checked that it belongs there. */
static bool add_heap(burrow__Timers *ts, burrow__Timer *t) {
    if (t->ts != NULL)
        bad_timer();
    if (!heap_grow(ts))
        return false;

    t->ts = ts;
    ts->heap[ts->len].t = t;
    ts->heap[ts->len].when = t->when;
    ts->len++;
    sift_up(ts, ts->len - 1U);
    if (ts->heap[0].t == t)
        update_min_when_heap(ts);
    return true;
}

/* Takes the earliest timer out. */
static void delete_min(burrow__Timers *ts) {
    burrow__Timer *t = ts->heap[0].t;

    if (t->ts != ts)
        bad_timer();
    t->ts = NULL;

    uint32_t last = ts->len - 1U;
    if (last > 0)
        ts->heap[0] = ts->heap[last];
    ts->heap[last] = (burrow__TimerWhen){0};
    ts->len = last;
    if (last > 0)
        sift_down(ts, 0);
    update_min_when_heap(ts);

    /* An empty heap has no modified timers in it, whatever the published lower
     * bound happens to say, and leaving a stale one there would keep waking a
     * thread up for a timer that is not there. */
    if (last == 0)
        burrow__atomic_store_u64(&ts->min_when_modified, 0);
}

/* Takes the timer at `i` out, wherever in the heap it is.
 *
 * Only burrow__timer_drop wants this. Every other path here works on the head,
 * which is what a heap is for, and this one exists because memory that is about
 * to be given back has to stop being reachable first.
 *
 * The last entry fills the hole and then goes whichever way it has to. It cannot
 * need both directions, so the comparison against its new parent decides which
 * one to do. */
static void delete_at(burrow__Timers *ts, uint32_t i) {
    burrow__Timer *t = ts->heap[i].t;

    if (t->ts != ts || i >= ts->len)
        bad_timer();
    t->ts = NULL;

    uint32_t last = ts->len - 1U;
    if (i != last)
        ts->heap[i] = ts->heap[last];
    ts->heap[last] = (burrow__TimerWhen){0};
    ts->len = last;

    if (i < last) {
        if (i > 0 && tw_less(ts->heap[i], ts->heap[(i - 1U) / TIMER_HEAP_N]))
            sift_up(ts, i);
        else
            sift_down(ts, i);
    }
    update_min_when_heap(ts);

    if (ts->len == 0)
        burrow__atomic_store_u64(&ts->min_when_modified, 0);
}

/* Does whatever the state bits of the earliest timer say needs doing, and
 * answers whether anything changed. The timer has to be heap[0], and both its
 * lock and the set's have to be held.
 *
 * This is the one place a timer leaves a heap. */
static bool update_heap(burrow__Timer *t) {
    burrow__Timers *ts = t->ts;

    if (ts == NULL || ts->len == 0 || ts->heap[0].t != t)
        bad_timer();

    if ((t->state & BURROW__TIMER_ZOMBIE) != 0) {
        t->state &=
            ~(BURROW__TIMER_HEAPED | BURROW__TIMER_ZOMBIE | BURROW__TIMER_MODIFIED);
        burrow__atomic_add_u32(&ts->zombies, 0U - 1U);
        delete_min(ts);
        return true;
    }

    if ((t->state & BURROW__TIMER_MODIFIED) != 0) {
        t->state &= ~BURROW__TIMER_MODIFIED;
        ts->heap[0].when = t->when;
        sift_down(ts, 0);
        update_min_when_heap(ts);
        return true;
    }

    return false;
}

/* Clears whatever rubbish has collected at the top of the heap, so that the next
 * question about when the earliest timer is due gets a straight answer.
 *
 * Zombies go from the end first, because taking the last entry out needs no
 * sifting at all, and because doing it improves the odds that the entry which
 * replaces a zombie at the top is a live timer rather than another zombie. */
static void clean_head(burrow__Timers *ts) {
    for (;;) {
        if (ts->len == 0)
            return;

        burrow__Timer *last = ts->heap[ts->len - 1U].t;
        if ((timer_astate(last) & BURROW__TIMER_ZOMBIE) != 0) {
            timer_lock(last);
            if ((last->state & BURROW__TIMER_ZOMBIE) != 0) {
                last->state &= ~(BURROW__TIMER_HEAPED | BURROW__TIMER_ZOMBIE |
                                 BURROW__TIMER_MODIFIED);
                last->ts = NULL;
                burrow__atomic_add_u32(&ts->zombies, 0U - 1U);
                ts->heap[ts->len - 1U] = (burrow__TimerWhen){0};
                ts->len--;
                if (ts->len == 0)
                    update_min_when_heap(ts);
            }
            timer_unlock(last);
            continue;
        }

        burrow__Timer *t = ts->heap[0].t;
        if (t->ts != ts)
            bad_timer();

        if ((timer_astate(t) & (BURROW__TIMER_MODIFIED | BURROW__TIMER_ZOMBIE)) == 0)
            return;

        timer_lock(t);
        bool changed = update_heap(t);
        timer_unlock(t);
        if (!changed)
            return;
    }
}

/* Walks the whole heap, applies every pending change, and puts it back in order.
 *
 * `force` is for the case where the heap is mostly stopped timers and the point
 * of the walk is to throw them out rather than to find one that is due. Without
 * it this returns immediately unless something modified is actually due, which
 * is what makes a program that sets and clears deadlines all day cheap: the
 * modified bits pile up and are dealt with in one pass rather than one at a
 * time. */
static void adjust(burrow__Timers *ts, int64_t now, bool force) {
    if (!force) {
        int64_t first = unpack_when(burrow__atomic_load_u64(&ts->min_when_modified));
        if (first == 0 || first > now)
            return;
    }

    /* The two stores below are in this order for the reader in wake_time, and
     * getting them the wrong way round is a wake that comes too late.
     *
     * The walk is about to clear every MODIFIED bit, which is what makes
     * min_when_modified zero afterwards. But a reader that sees the zero before
     * the walk has finished has to still get a correct answer, and the only
     * field left for it to read is min_when_heap, which this holds the lock for
     * and can therefore set to whatever keeps the answer right. So: lower the
     * heap minimum to cover what modified stood for, then clear modified, then
     * walk, then correct the heap minimum to the truth. */
    burrow__atomic_store_u64(&ts->min_when_heap,
                             pack_when(burrow__timers_wake_time(ts)));
    burrow__atomic_store_u64(&ts->min_when_modified, 0);

    bool changed = false;
    for (uint32_t i = 0; i < ts->len; i++) {
        burrow__Timer *t = ts->heap[i].t;
        if (t->ts != ts)
            bad_timer();

        if ((timer_astate(t) & (BURROW__TIMER_MODIFIED | BURROW__TIMER_ZOMBIE)) == 0)
            continue;

        timer_lock(t);
        if ((t->state & BURROW__TIMER_HEAPED) == 0)
            bad_timer();

        if ((t->state & BURROW__TIMER_ZOMBIE) != 0) {
            burrow__atomic_add_u32(&ts->zombies, 0U - 1U);
            t->state &=
                ~(BURROW__TIMER_HEAPED | BURROW__TIMER_ZOMBIE | BURROW__TIMER_MODIFIED);
            t->ts = NULL;
            ts->heap[i] = ts->heap[ts->len - 1U];
            ts->heap[ts->len - 1U] = (burrow__TimerWhen){0};
            ts->len--;
            /* Look at this index again, because what is in it now is whatever
             * came off the end. When i is 0 this wraps and the loop's increment
             * wraps it back, which is what unsigned arithmetic is defined to
             * do. */
            i--;
            changed = true;
        } else if ((t->state & BURROW__TIMER_MODIFIED) != 0) {
            ts->heap[i].when = t->when;
            t->state &= ~BURROW__TIMER_MODIFIED;
            changed = true;
        }
        timer_unlock(t);
    }

    if (changed)
        init_heap(ts);
    update_min_when_heap(ts);
}

/* ----------------------------------------------------------------- one timer */

void burrow__timer_init(burrow__Timer *t, burrow__TimerFn f, void *arg) {
    *t = (burrow__Timer){0};
    t->f = f;
    t->arg = arg;
}

/* Whether this timer wants to be in a heap and is not. The timer's lock is
 * held. */
static bool needs_add(burrow__Timer *t) {
    return (t->state & BURROW__TIMER_HEAPED) == 0 && t->when > 0;
}

/* Puts a timer in a set if it is not in one already.
 *
 * No lock is held on the way in. It cannot be: the order is the set and then the
 * timer, and this is called by somebody who has just put the timer's lock down
 * for that reason. So the timer may have been started by somebody else in
 * between, and needs_add is the second look that catches it. */
static bool maybe_add(burrow__Timers *ts, burrow__Timer *t) {
    bool ok = true;
    bool wake = false;
    int64_t when = 0;

    timers_lock(ts);
    clean_head(ts);
    timer_lock(t);
    if (needs_add(t)) {
        int64_t next = burrow__timers_wake_time(ts);
        when = t->when;

        t->state |= BURROW__TIMER_HEAPED;
        ok = add_heap(ts, t);
        if (!ok)
            t->state &= ~BURROW__TIMER_HEAPED;
        else
            wake = !ts->fake && (next == 0 || when < next);
    }
    timer_unlock(t);
    timers_unlock(ts);

    /* Outside both locks, because what this ends up doing is starting a thread
     * and a thread start is not something to do with a scheduler lock held. */
    if (wake)
        burrow__timers_wake(when);
    return ok;
}

bool burrow__timer_stop(burrow__Timer *t) {
    timer_lock(t);

    if ((t->state & BURROW__TIMER_HEAPED) != 0) {
        /* Cannot be taken out of somebody else's heap, so it is marked instead.
         * MODIFIED as well as ZOMBIE because t->when is about to stop matching
         * the copy in the heap. */
        t->state |= BURROW__TIMER_MODIFIED;
        if ((t->state & BURROW__TIMER_ZOMBIE) == 0) {
            t->state |= BURROW__TIMER_ZOMBIE;
            burrow__atomic_add_u32(&t->ts->zombies, 1);
        }
    }

    bool pending = t->when > 0;
    t->when = 0;
    timer_unlock(t);
    return pending;
}

void burrow__timer_drop(burrow__Timer *t) {
    for (;;) {
        timer_lock(t);
        if ((t->state & BURROW__TIMER_HEAPED) == 0) {
            /* In no heap, so there is nothing to take it out of and the whole
             * job is forgetting what it was going to do. */
            t->state = 0;
            t->when = 0;
            t->period = 0;
            t->ts = NULL;
            timer_unlock(t);
            return;
        }

        /* In a heap, and the heap's lock comes before the timer's, so the
         * timer's has to go down first. The set can be handed to another P in
         * between, which is what the second look under both locks is for. */
        burrow__Timers *ts = t->ts;
        if (ts == NULL)
            bad_timer();
        timer_unlock(t);

        timers_lock(ts);
        timer_lock(t);
        if ((t->state & BURROW__TIMER_HEAPED) == 0 || t->ts != ts) {
            timer_unlock(t);
            timers_unlock(ts);
            continue;
        }

        uint32_t at = ts->len;
        for (uint32_t i = 0; i < ts->len; i++) {
            if (ts->heap[i].t == t) {
                at = i;
                break;
            }
        }
        if (at == ts->len)
            bad_timer();

        if ((t->state & BURROW__TIMER_ZOMBIE) != 0)
            burrow__atomic_add_u32(&ts->zombies, 0U - 1U);
        t->state = 0;
        t->when = 0;
        t->period = 0;

        delete_at(ts, at);
        timer_unlock(t);
        timers_unlock(ts);
        return;
    }
}

bool burrow__timer_reset_on(burrow__Timers *ts, burrow__Timer *t, int64_t when,
                            int64_t period, burrow__TimerFn f, void *arg,
                            bool *pending) {
    if (when <= 0)
        runtime_throw(BURROW_S("burrow__timer_reset: when must be positive"));
    if (period < 0)
        runtime_throw(BURROW_S("burrow__timer_reset: period cannot be negative"));

    timer_lock(t);

    bool was_pending = t->when > 0;
    bool wake = false;
    t->when = when;
    t->period = period;
    if (f != NULL) {
        t->f = f;
        t->arg = arg;
    }

    if ((t->state & BURROW__TIMER_HEAPED) != 0) {
        /* Already in a heap, which may be another thread's. Move it by writing
         * the new time and saying that the heap's copy is out of date. */
        t->state |= BURROW__TIMER_MODIFIED;
        if ((t->state & BURROW__TIMER_ZOMBIE) != 0) {
            /* It was stopped and had not been thrown out yet, so starting it
             * again is a matter of taking the mark off. This is the path a read
             * deadline on a busy connection takes, and it touches no heap at
             * all. */
            t->state &= ~BURROW__TIMER_ZOMBIE;
            burrow__atomic_add_u32(&t->ts->zombies, 0U - 1U);
        }

        burrow__Timers *in = t->ts;
        int64_t next = burrow__timers_wake_time(in);
        wake = !in->fake && (next == 0 || when < next);

        /* The bit has to be out where the owning P can see it before the
         * published minimum moves, or the P's next pass can clear a lower bound
         * that was about to be lowered again and lose this change with it. The
         * unlock is what publishes the bit. */
        timer_unlock(t);
        update_min_when_modified(in, when);

        if (pending != NULL)
            *pending = was_pending;
        if (wake)
            burrow__timers_wake(when);
        return true;
    }

    bool add = needs_add(t);
    timer_unlock(t);

    if (pending != NULL)
        *pending = was_pending;
    if (!add)
        return true;
    return maybe_add(ts, t);
}

bool burrow__timer_reset(burrow__Timer *t, int64_t when, int64_t period,
                         burrow__TimerFn f, void *arg, bool *pending) {
    burrow__Timers *ts = burrow__timers_local();
    if (ts == NULL)
        runtime_throw(BURROW_S("burrow__timer_reset: not on a goroutine"));
    return burrow__timer_reset_on(ts, t, when, period, f, arg, pending);
}

/* --------------------------------------------------------------- running them */

/* Runs the timer, which is holding its own lock and is the head of a set that is
 * also locked. Comes back with the timer unlocked and the set locked again.
 *
 * The locks come off around the call because the function is allowed to do
 * anything a scheduler call can do, including readying a goroutine, which takes
 * the scheduler's lock. Holding a P's timer lock across that would put the two
 * locks in an order nothing else uses. */
static void unlock_and_run(burrow__Timer *t, int64_t now) {
    if ((t->state & (BURROW__TIMER_MODIFIED | BURROW__TIMER_ZOMBIE)) != 0)
        bad_timer();

    burrow__TimerFn f = t->f;
    void *arg = t->arg;
    int64_t delay = now - t->when;
    int64_t next = 0;

    if (t->period > 0) {
        /* Stays in the heap with the next firing worked out from when it was
         * due rather than from now, so that a ticker whose firings are late does
         * not drift. The multiplication is what skips the firings that were
         * missed entirely, which is Go's rule: a ticker that nobody serviced for
         * a minute ticks once on the way back, not three thousand times. */
        next = t->when + t->period * (1 + delay / t->period);
        if (next < 0)
            next = MAX_WHEN;
    }

    burrow__Timers *ts = t->ts;
    t->when = next;
    if ((t->state & BURROW__TIMER_HEAPED) != 0) {
        t->state |= BURROW__TIMER_MODIFIED;
        if (next == 0) {
            t->state |= BURROW__TIMER_ZOMBIE;
            burrow__atomic_add_u32(&ts->zombies, 1);
        }
        (void)update_heap(t);
    }

    timer_unlock(t);
    timers_unlock(ts);

    if (f != NULL)
        f(arg, delay);

    timers_lock(ts);
}

/* Looks at the earliest timer and runs it if it is due. Answers 0 if it ran one,
 * -1 if the set is empty, and otherwise when the earliest one is due. The set is
 * locked, and stays locked, though not continuously. */
static int64_t run_one(burrow__Timers *ts, int64_t now) {
    for (;;) {
        if (ts->len == 0)
            return -1;

        burrow__TimerWhen tw = ts->heap[0];
        burrow__Timer *t = tw.t;
        if (t->ts != ts)
            bad_timer();

        if ((timer_astate(t) & (BURROW__TIMER_MODIFIED | BURROW__TIMER_ZOMBIE)) == 0 &&
            tw.when > now)
            return tw.when;

        timer_lock(t);
        if (update_heap(t)) {
            /* The head has changed under us, so whatever is there now is a
             * different question. */
            timer_unlock(t);
            continue;
        }

        if ((t->state & BURROW__TIMER_HEAPED) == 0 ||
            (t->state & BURROW__TIMER_MODIFIED) != 0)
            bad_timer();

        if (t->when > now) {
            timer_unlock(t);
            return t->when;
        }

        unlock_and_run(t, now);
        return 0;
    }
}

int64_t burrow__timers_check(burrow__Timers *ts, int64_t now, int64_t *next,
                             bool *ran) {
    int64_t until = 0;
    bool did = false;

    if (next != NULL)
        *next = 0;
    if (ran != NULL)
        *ran = false;

    int64_t first = burrow__timers_wake_time(ts);
    if (first == 0)
        return now;

    if (now == 0)
        now = burrow__nanotime();

    /* A set that is mostly stopped timers is worth a pass even when nothing is
     * due, because every operation on it is paying for the dead weight. Only the
     * P that owns the set does that, so that a thread looking around at other Ps
     * before it parks does not take their locks to do housekeeping for them. */
    bool mine = ts == burrow__timers_local();
    bool force = mine && burrow__atomic_load_u32(&ts->zombies) >
                             burrow__atomic_load_u32(&ts->alen) / 4U;

    if (now < first && !force) {
        if (next != NULL)
            *next = first;
        return now;
    }

    timers_lock(ts);
    if (ts->len > 0) {
        adjust(ts, now, false);
        for (;;) {
            if (ts->len == 0)
                break;
            int64_t when = run_one(ts, now);
            if (when != 0) {
                if (when > 0)
                    until = when;
                break;
            }
            did = true;
        }

        /* The forced pass happens after the running rather than instead of it.
         * Go found this measurably faster under contention and says it does not
         * fully understand why, which is worth repeating rather than quietly
         * tidying away. */
        force = mine && burrow__atomic_load_u32(&ts->zombies) > ts->len / 4U;
        if (force)
            adjust(ts, now, true);
    } else {
        /* The published minimum said there was something due and the heap is
         * empty, which happens when the last timer in it was thrown out by a
         * path that had no reason to touch the minimum. Left alone it is a time
         * in the past that never moves, and a thread working out how long to
         * sleep from it would not sleep at all and would come straight back
         * here. */
        update_min_when_heap(ts);
        burrow__atomic_store_u64(&ts->min_when_modified, 0);
    }
    timers_unlock(ts);

    if (next != NULL)
        *next = until;
    if (ran != NULL)
        *ran = did;
    return now;
}

/* ---------------------------------------------------------------- a whole set */

void burrow__timers_init(burrow__Timers *ts) {
    *ts = (burrow__Timers){0};
}

void burrow__timers_free(burrow__Timers *ts) {
    for (uint32_t i = 0; i < ts->len; i++) {
        burrow__Timer *t = ts->heap[i].t;
        timer_lock(t);
        t->state &=
            ~(BURROW__TIMER_HEAPED | BURROW__TIMER_ZOMBIE | BURROW__TIMER_MODIFIED);
        t->ts = NULL;
        timer_unlock(t);
    }

    if (ts->heap != NULL) {
        Alloc *a = heap_allocator();
        mem_free(a, ts->heap, (size_t)ts->cap * sizeof(burrow__TimerWhen),
                 _Alignof(burrow__TimerWhen));
    }
    *ts = (burrow__Timers){0};
}

bool burrow__timers_take(burrow__Timers *ts, burrow__Timers *src) {
    if (src->len == 0)
        return true;

    /* Room for all of them before any of them move, so that a failure leaves
     * both sets as they were rather than half of one in the other. */
    while (ts->len + src->len > ts->cap) {
        if (!heap_grow(ts))
            return false;
    }

    for (uint32_t i = 0; i < src->len; i++) {
        burrow__Timer *t = src->heap[i].t;
        t->ts = NULL;
        if ((t->state & BURROW__TIMER_ZOMBIE) != 0) {
            t->state &=
                ~(BURROW__TIMER_HEAPED | BURROW__TIMER_ZOMBIE | BURROW__TIMER_MODIFIED);
        } else {
            t->state &= ~BURROW__TIMER_MODIFIED;
            if (!add_heap(ts, t))
                bad_timer();
        }
        burrow__atomic_store_u32(&t->astate, t->state);
    }

    if (src->heap != NULL) {
        Alloc *a = heap_allocator();
        mem_free(a, src->heap, (size_t)src->cap * sizeof(burrow__TimerWhen),
                 _Alignof(burrow__TimerWhen));
    }
    *src = (burrow__Timers){0};
    burrow__atomic_store_u32(&ts->alen, ts->len);
    return true;
}
