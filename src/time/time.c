/* Sleeping and running a function later, on top of the runtime's timers.
 *
 * Go's time.Sleep and time.AfterFunc are about forty lines between them, and
 * that is because everything hard about them lives in runtime/time.go, which
 * here is src/runtime/timer.c. This file is the same forty lines: work out a
 * deadline, arm a timer, and say what the answer means.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/time.h"

#include "burrow/clock.h"
#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/note.h"
#include "burrow/proc.h"
#include "burrow/runtime.h"
#include "burrow/sched.h"
#include "burrow/thread.h"
#include "burrow/timer.h"

#include <stdint.h>

/* When a duration that starts now is up, on the runtime's clock.
 *
 * Timers are armed with a deadline rather than a duration because the heap sorts
 * on it and because the answer has to stay right across however many times the
 * timer is looked at before it fires. The clamps are for the two ends nobody
 * ever reaches: a duration long enough to run off the end of the clock, and a
 * reading so early in the life of the process that adding nothing to it leaves a
 * deadline the timer code will not take. */
static int64_t deadline(Duration d) {
    int64_t now = burrow__nanotime();

    if (d > 0 && d > INT64_MAX - now)
        return INT64_MAX;

    int64_t when = d > 0 ? now + d : now;
    return when > 0 ? when : 1;
}

/* Waits on the thread rather than on a goroutine.
 *
 * For a caller that is not on one of the scheduler's threads, and for the
 * goroutine case where the timer could not be armed. The note is never opened by
 * anybody, so this is a plain timed wait that always runs out, which is the
 * cheapest sleep the platform has underneath it.
 *
 * If even the note cannot be made, which on Linux cannot happen at all and
 * elsewhere means the kernel is out of handles, this spins and gives the
 * processor away while it waits. That burns a core for the duration and is still
 * better than returning early from something whose whole job is not to. */
static void sleep_thread(int64_t ns) {
    burrow__Note n;

    if (!burrow__note_init(&n)) {
        int64_t end = deadline(ns);
        while (burrow__nanotime() < end)
            burrow__thread_yield();
        return;
    }

    (void)burrow__note_sleep_timeout(&n, ns);
    burrow__note_free(&n);
}

/* ------------------------------------------------------------------- sleeping */

/* What the timer does when a sleep is up. Runs on whichever thread noticed,
 * which is not the thread the sleeper was on and usually not even the P it was
 * on. */
static void wake_sleeper(void *arg, int64_t delay) {
    (void)delay;
    sched_ready((Goroutine *)arg);
}

/* Everything the park needs, on the sleeping goroutine's own stack.
 *
 * Go keeps the deadline in the g instead, because a Go stack moves when it grows
 * and a pointer into one cannot be handed to another thread. burrow's stacks do
 * not move, and the goroutine is parked rather than gone, so its frame is as
 * good a place to keep this as any. */
typedef struct Sleeper {
    burrow__Timer *t;
    int64_t when;
    bool armed;
} Sleeper;

/* Arms the timer once the goroutine is off its thread and can no longer be
 * reached from it.
 *
 * The order is the whole point, and it is why this is a park callback rather
 * than two lines before the park. Arming first would let the timer fire on
 * another thread and ready a goroutine that is still running on this one, which
 * is two threads on one stack. By the time this runs, the goroutine is in the
 * waiting state and its context is saved, so a timer that goes off in the middle
 * of this call is simply the shortest sleep there has ever been.
 *
 * Answering false means the heap would not grow, and the goroutine carries on
 * without parking. Its caller sees `armed` and sleeps the thread instead. */
static bool arm_sleep(Goroutine *g, void *lock) {
    Sleeper *s = (Sleeper *)lock;

    s->armed = burrow__timer_reset(s->t, s->when, 0, wake_sleeper, g, NULL);
    return s->armed;
}

/* One line, and it is here rather than being the same symbol as the runtime's
 * clock because the two make different promises. burrow__nanotime is the
 * runtime's own and carries the double underscore that says it may change:
 * there is already a plan for a cached reading on the platforms where the
 * syscall is dear enough to be worth it. This one is supported API and will go
 * on meaning exactly what burrow/time.h says it means. */
int64_t burrow_nanotime(void) {
    return burrow__nanotime();
}

void time_sleep(Duration d) {
    if (d <= 0)
        return;

    Goroutine *g = sched_current();
    burrow__Timer *t = g != NULL ? burrow__sleep_timer() : NULL;

    /* Not on a goroutine, or on one that cannot have a timer because the
     * allocator is empty. Either way the thread waits. In the second case that
     * holds up the P as well, which is the honest price of being out of memory
     * and is still a sleep that lasts as long as it was asked to. */
    if (t == NULL) {
        sleep_thread(d);
        return;
    }

    Sleeper s = {t, deadline(d), false};
    sched_park(arm_sleep, &s);

    if (!s.armed)
        sleep_thread(d);
}

/* ----------------------------------------------------------------- after func */

struct TimeTimer {
    burrow__Timer t;

    /* What to run, and where the memory came from. The allocator is kept for the
     * same reason Map keeps one: this is an opaque type, so nothing outside this
     * file can name the pointer or the size to hand back. */
    Func f;
    Alloc *a;
};

/* What the runtime calls when an AfterFunc timer is due.
 *
 * Starts a goroutine and gets out of the way, because this runs on a scheduler
 * thread that is in the middle of looking for work, and anything that blocks
 * here blocks a P. Go does the same thing with the same two lines.
 *
 * A goroutine that cannot be started is a program that is out of memory, and
 * there is nothing useful to do about it from in here. The timer has fired
 * either way, so the caller sees a callback that never ran rather than one that
 * ran late. */
static void run_after(void *arg, int64_t delay) {
    (void)delay;
    TimeTimer *tt = (TimeTimer *)arg;

    (void)go(tt->f);
}

TimeTimer *time_after_func(Alloc *a, Duration d, Func f) {
    if (a == NULL)
        runtime_throw(BURROW_S("time_after_func: no allocator"));
    if (BURROW_FUNC_IS_NIL(f))
        runtime_throw(BURROW_S("time_after_func: nil function"));
    if (burrow__timers_local() == NULL)
        runtime_throw(BURROW_S("time_after_func: not on a goroutine"));

    TimeTimer *tt = BURROW_NEW(a, TimeTimer);
    if (tt == NULL)
        return NULL;

    tt->f = f;
    tt->a = a;
    burrow__timer_init(&tt->t, run_after, tt);

    if (!burrow__timer_reset(&tt->t, deadline(d), 0, NULL, NULL, NULL)) {
        mem_free(a, tt, sizeof(TimeTimer), _Alignof(TimeTimer));
        return NULL;
    }
    return tt;
}

bool time_timer_stop(TimeTimer *t) {
    if (t == NULL)
        runtime_throw(BURROW_S("time_timer_stop: nil timer"));

    return burrow__timer_stop(&t->t);
}

bool time_timer_reset(TimeTimer *t, Duration d, bool *pending) {
    if (t == NULL)
        runtime_throw(BURROW_S("time_timer_reset: nil timer"));
    if (burrow__timers_local() == NULL)
        runtime_throw(BURROW_S("time_timer_reset: not on a goroutine"));

    return burrow__timer_reset(&t->t, deadline(d), 0, NULL, NULL, pending);
}

void time_timer_free(TimeTimer *t) {
    if (t == NULL)
        return;

    Alloc *a = t->a;

    burrow__timer_drop(&t->t);
    mem_free(a, t, sizeof(TimeTimer), _Alignof(TimeTimer));
}
