/* Sleeping, and running a function later on.
 *
 * This is the half of Go's time package that needs a scheduler underneath it:
 * Duration and its constants, time.Sleep, and time.AfterFunc with the Stop and
 * Reset that go with it. The calendar half, the Time type with its wall clock
 * and its formatting and its timezones, is a separate job and is not here yet.
 *
 * A goroutine that sleeps here costs a timer and no thread. The thread it was
 * running on goes and finds other work, and one of the scheduler's threads wakes
 * it again when the time is up, which is why a program can have a hundred
 * thousand goroutines waiting on a hundred thousand deadlines and still be
 * asleep in the kernel using no processor at all. That is the whole reason the
 * per P timer heaps in burrow/timer.h exist, and this header is the part of it
 * people actually call.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package time */

#ifndef BURROW_TIME_H
#define BURROW_TIME_H

#include "burrow/func.h"
#include "burrow/mem.h"
#include "burrow/own.h"
#include "burrow/platform.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A length of time in nanoseconds, which is Go's time.Duration exactly.
 *
 * Signed, so a difference between two readings can be negative, and 64 bits
 * wide, which puts the largest duration at about 292 years. Anything that needs
 * longer than that is a date and not a duration.
 *
 * It is a plain integer and not a struct on purpose. Go's is a plain int64 too,
 * so d / 2, d * 3 and d1 < d2 all mean what they look like, and a Duration can
 * be printed with the format string for a long long. */
typedef int64_t Duration;

/* The units, which are Go's constants and are used the same way.
 *
 *     time_sleep(500 * TIME_MILLISECOND);
 *     time_sleep(2 * TIME_SECOND + 300 * TIME_MILLISECOND);
 *
 * Multiplying by the unit rather than writing the nanoseconds out is not a
 * style preference. 500000000 and 5000000000 look the same at a glance and one
 * of them is ten times the other, and that is a real bug that has shipped in
 * real programs more than once. */
#define TIME_NANOSECOND ((Duration)1)
#define TIME_MICROSECOND (1000 * TIME_NANOSECOND)
#define TIME_MILLISECOND (1000 * TIME_MICROSECOND)
#define TIME_SECOND (1000 * TIME_MILLISECOND)
#define TIME_MINUTE (60 * TIME_SECOND)
#define TIME_HOUR (60 * TIME_MINUTE)

/* Nanoseconds on a clock that only goes forwards, measured from an arbitrary
 * point that means nothing on its own.
 *
 *     int64_t start = burrow_nanotime();
 *     work();
 *     Duration took = burrow_nanotime() - start;
 *
 * It says burrow rather than time because Go has no such function. Go's
 * time.Now carries a monotonic reading around inside it and time.Since pulls it
 * back out, so a Go program never names the clock directly. burrow has no Time
 * yet, and the parts that need a deadline need it now, so the reading is
 * exposed on its own: burrow/context.h measures a deadline on this clock, and
 * so does anything that has to know how long something took.
 *
 * Never goes backwards and never jumps, which is the point. The wall clock does
 * both whenever somebody sets the date or ntp corrects a drift, and a timeout
 * measured on it waits for an hour or fires twice.
 *
 * Callable from any thread, including one the runtime knows nothing about.
 * Costs a few nanoseconds everywhere, because every platform answers this out
 * of the vdso or its equivalent rather than from a system call.
 *
 * Inside a synctest bubble this is the bubble's clock and not the machine's, so
 * a deadline set in a bubble and a sleep in a bubble agree with each other.
 * Which also means the difference between two readings taken in a bubble is how
 * long the code under test thinks it took, not how long it really took, and the
 * second number is not available in there. See burrow/synctest.h.
 *
 * A bubble starts its clock at midnight UTC on 2000-01-01, which comes to
 * 946684800000000000 nanoseconds after the unix epoch. Go's number, and worth
 * knowing because it makes a test that prints an elapsed time print the same
 * thing on every machine and every run. */
int64_t burrow_nanotime(void);

/* Stops the calling goroutine for at least d.
 *
 * At least, and never exactly. The goroutine becomes runnable when the time is
 * up and then has to wait for a thread to pick it up, so a busy program hands it
 * back late. Every sleep in every language works this way. What burrow promises
 * is the same thing Go promises: not early, and not measured on a clock that
 * somebody can set backwards.
 *
 * A duration of zero or less returns straight away without giving up the
 * thread, which is Go's rule as well. It is not a yield, and if a yield is what
 * you want, that is runtime_gosched.
 *
 * Callable from a thread that is not one of the scheduler's, and then it sleeps
 * the thread rather than parking a goroutine, because there is no goroutine to
 * park. That is a courtesy for setup code and tests rather than a thing to build
 * on.
 *
 * Inside a synctest bubble this returns as soon as every other goroutine in the
 * bubble is blocked too, because that is when the bubble's clock jumps to the
 * next timer that is due. A sleep of an hour in there costs microseconds. The
 * order still holds: the goroutine that asked for a minute comes back before
 * the one that asked for an hour. See burrow/synctest.h. */
void time_sleep(Duration d);

/* A timer that runs a function once its time is up. Go's time.Timer.
 *
 * The name is what the mapping in docs/design/08-naming-abi.md gives for
 * time.Timer, package and type stuck together, the same rule that turns
 * strings.Builder into StringsBuilder. It stutters, and the alternative is a
 * mapping with exceptions in it, which costs more than the stutter does.
 *
 * Opaque, because the only thing the Go type has that you can reach is its
 * channel, and there are no channels yet. When there are, the channel arrives
 * as an accessor rather than a field, and nothing written against this header
 * has to change. */
typedef struct TimeTimer TimeTimer;

/* Runs f in its own goroutine once d has gone by, and hands back the timer so
 * that it can be stopped or moved. Go's time.AfterFunc.
 *
 * In its own goroutine, which is Go's rule and matters more than it sounds. The
 * function runs on a fresh goroutine with a fresh stack, so it may block, take
 * locks, sleep again or talk to the network without holding up the thread that
 * noticed the timer was due. The cost is that there is no ordering between two
 * callbacks that come due at the same moment, exactly as in Go.
 *
 * The timer's memory comes from a, and it has to be given back with
 * time_timer_free. Go leaves that to the collector. This is the paired
 * constructor and destructor that docs/design/05-memory.md asks for whenever Go
 * itself has a Stop or a Close, and an arena user can ignore it because
 * arena_free already covers everything.
 *
 * NULL means the allocator or the timer heap would not give out memory, and
 * then nothing has been armed and nothing will run. A duration of zero or less
 * means the callback is due immediately and runs as soon as a thread looks.
 *
 * Has to be called from a goroutine, because the timer goes in the heap of the
 * P the caller is running on. */
BURROW_OWNS(ret) TimeTimer *time_after_func(Alloc *a, Duration d, Func f);

/* Stops a timer, and answers whether it was still waiting to run.
 *
 * False means the timer had already fired or had already been stopped. It does
 * not mean the callback has finished, and it does not mean the callback has even
 * started, because the callback runs on its own goroutine and a stop that loses
 * the race by a nanosecond still says false while the goroutine is still being
 * put together. Go has exactly this, and the answer there and here is the same:
 * a timer stop does not synchronise with the callback, so anything the callback
 * touches needs its own lock.
 *
 * Costs nothing but the timer's own lock. A stopped timer is left where it is
 * and marked, and the P that owns the heap throws it out the next time it walks
 * one, so a program that sets and clears a deadline on every read never touches
 * a heap at all. */
bool time_timer_stop(TimeTimer *t);

/* Arms a timer again for d from now, whether or not it was running, and answers
 * whether the call worked.
 *
 * `pending` may be NULL, and when it is not it is set to whether the timer was
 * still waiting to run, which is what Go's Reset returns. It is out here rather
 * than in the return value because arming a timer that had already fired can
 * need the P's heap to grow, and a heap that cannot grow is a failure a C
 * library has to report rather than panic on. burrow/timer.h says more about
 * that. False means the timer is not armed and will not run.
 *
 * Go's advice about Reset applies here unchanged: resetting a timer whose
 * callback is already running does not unrun it, and a program that needs to
 * know which of the two happened needs to say so itself, with a flag under a
 * lock the callback takes as well. */
bool time_timer_reset(TimeTimer *t, Duration d, bool *pending);

/* Hands the timer's memory back to the allocator it came from, and leaves the
 * TimeTimer * dangling, so it is the last thing you do with one.
 *
 * Stops the timer first, and takes it out of whatever heap it is sitting in, so
 * this is safe on a timer that never fired. That second part is the reason this
 * cannot be a plain mem_free: a timer that has been stopped is still in a P's
 * heap until that P gets round to throwing it out, and freeing the memory under
 * it would leave a pointer to a hole in the scheduler. Taking it out costs a
 * walk of the heap it is in, which is a few hundred nanoseconds on a P with a
 * thousand timers on it and nothing at all on one that has already fired.
 *
 * What it does not do is wait for a callback that is already running, for the
 * reason in time_timer_stop. A program that frees a timer whose callback is
 * still going is fine as far as the timer is concerned, since the callback has
 * its own goroutine and does not touch the timer, but it is on its own for
 * anything else that goroutine is holding.
 *
 * NULL is fine and does nothing. */
void time_timer_free(TimeTimer *t);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_TIME_H */
