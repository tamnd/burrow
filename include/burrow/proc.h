/* Goroutines: starting them, stopping them, and the two calls everything that
 * blocks is built on.
 *
 * This is the outside of the scheduler. burrow/sched.h is the inside, and a
 * program that is not itself part of the runtime wants this file and not that
 * one.
 *
 *     static void worker(void *env) {
 *         Counter *c = env;
 *         ...
 *     }
 *
 *     static void run(void *env) {
 *         (void)env;
 *         go(BURROW_FN(Func, worker, &counter));
 *         go(BURROW_FN(Func, worker, &counter));
 *         ...
 *     }
 *
 *     int main(void) {
 *         runtime_main(BURROW_FN(Func, run, NULL));
 *         return 0;
 *     }
 *
 * The one thing here that Go does not make you write is runtime_main. Go's
 * runtime starts before your main does, because the Go toolchain arranges it.
 * There is no toolchain here, so something has to say where the goroutine world
 * begins and ends, and this is it. Everything inside it is Go: go starts a
 * goroutine, the scheduler runs it on whichever thread is free, and when the
 * function you handed runtime_main returns, the program is over.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_PROC_H
#define BURROW_PROC_H

#include "burrow/func.h"
#include "burrow/own.h"
#include "burrow/platform.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A goroutine, from the outside. Opaque: the only things to do with one are
 * hand it to sched_ready and compare it against another.
 *
 * You get one from sched_current, and the reason to want one is that you are
 * writing something that blocks. A channel, a mutex, a wait group and a
 * netpoller all do the same thing: remember which goroutine is waiting, park
 * it, and ready it again when whatever it was waiting for happens. */
typedef struct burrow__G Goroutine;

/* ----------------------------------------------------------------- starting
 *
 * How big a goroutine's stack is, in bytes, before anything asks for a
 * different one.
 *
 * Go starts a goroutine on eight kilobytes and grows the stack by copying it
 * somewhere bigger when it runs out. Copying a stack means finding every
 * pointer into it and moving it, which Go's compiler can do because it emits a
 * map of where the pointers are, and which nothing can do for C. So a burrow
 * stack is decided once and never moves.
 *
 * A quarter of a megabyte is the answer to that, and it is not as expensive as
 * it sounds. The mapping is lazy on every system burrow targets, so a goroutine
 * that uses one page costs one page and the rest is address space, which 64 bit
 * machines have a great deal of. It is deep enough for ordinary C recursion,
 * and a goroutine that wants more says so with go_stack.
 *
 * Define it yourself to change it for a whole program. A server holding a
 * million connections may want less, and a recursive descent parser may want
 * considerably more. */
#ifndef BURROW_GOROUTINE_STACK
#define BURROW_GOROUTINE_STACK ((size_t)256 * 1024)
#endif

/* Starts fn as a new goroutine and answers whether it started.
 *
 * This is Go's `go f()`. It returns as soon as the goroutine exists, which is
 * before the goroutine has run, and there is no handle and no way to wait for
 * it, exactly as in Go. Waiting is what a channel or a WaitGroup is for.
 *
 * False means a stack could not be mapped, which is an address space or a
 * mapping count limit. Go has no way to report that because Go's answer is to
 * end the program, and returning it is the better answer for a library: a
 * server that cannot start one more connection handler can still serve the
 * connections it has. Check it or ignore it, but know that ignoring it is a
 * decision.
 *
 * Callable from a goroutine and from the thread sitting inside runtime_main.
 * Not callable before runtime_main has started or after it has returned, since
 * there is nothing to put the goroutine on. */
bool go(Func fn);

/* The same, with a stack size in bytes.
 *
 * Rounded up to a whole page and raised to the platform minimum, so the
 * goroutine gets at least what was asked for. Zero means BURROW_GOROUTINE_STACK.
 *
 * The size is a ceiling and not a reservation of memory: pages are committed as
 * they are touched. Asking for a megabyte because one goroutine in a thousand
 * recurses deeply is cheap. Asking for it for every goroutine in a program that
 * has a million of them is a terabyte of address space, which 64 bit machines
 * will give you and 32 bit machines will not. */
bool go_stack(Func fn, size_t stack_bytes);

/* Starts the scheduler, runs fn as the main goroutine, and returns when fn
 * returns.
 *
 * This is func main. Everything Go's runtime does before your main is called
 * happens here: the Ps are created, the threads that run them are started, and
 * fn goes on a run queue as the first goroutine.
 *
 * When fn returns, the program is over in the sense Go means. No new goroutine
 * is scheduled and every thread the scheduler started is stopped and joined
 * before this returns, so that by the time your main gets control back, nothing
 * of burrow's is still running. A goroutine that is parked forever does not
 * hold anything up, because a parked goroutine is not holding a thread. A
 * goroutine spinning in a loop that calls nothing in burrow does hold one up,
 * because that loop passes no safe point and so is never preempted. See
 * runtime_preempt_point below for the one line that fixes it.
 *
 * May be called again after it returns, which Go cannot do and which the tests
 * here need. Not reentrant: calling it from inside a goroutine is a bug and
 * stops the program. */
void runtime_main(Func fn);

/* ------------------------------------------------------------------ yielding
 *
 * Gives up the processor and puts this goroutine back on the run queue, so
 * something else gets a turn. Comes straight back if there is nothing else to
 * run.
 *
 * This is runtime.Gosched. A loop with no call in it that blocks, no call to
 * anything in burrow, and no call to this or to runtime_preempt_point holds its
 * thread until it finishes. */
void runtime_gosched(void);

/* Gives up the processor, but only if the scheduler has asked for it.
 *
 * The cheap one. runtime_gosched above always goes through the scheduler, which
 * is a stack switch and a trip through the global run queue, and a loop that
 * calls it every turn spends most of its time in the scheduler. This reads one
 * field and returns, and only does the expensive thing on the turn where
 * something is actually waiting, which is at most once every ten milliseconds.
 *
 * Go has no equivalent because Go does not need one: its compiler puts a safe
 * point in your loop for you and its signal handler can move a goroutine
 * wherever it stands. Neither is available to a C library. So burrow's
 * preemption is a request that a goroutine honours at the next place it is safe
 * to stop. Those places are the ones a loop that never blocks actually passes
 * through: a send, a receive, a select, and starting a goroutine. Everything
 * else in burrow that a loop might call either goes through one of those or
 * ends up waiting, and waiting gives the processor up anyway. A loop that does
 * none of it has no safe point in it, and this is how to put one there.
 *
 *     while (still_going(&work)) {
 *         crunch(&work);
 *         runtime_preempt_point();
 *     }
 *
 * Costs a relaxed load and a branch when nobody is waiting, so it is cheap
 * enough for the inside of a loop that does real work per turn and not cheap
 * enough for the inside of one that does a single arithmetic operation. In the
 * second case put it in the outer loop.
 *
 * Safe to call from a plain thread, where it does nothing, so a function that
 * is sometimes called from a goroutine and sometimes not does not need to ask
 * which. */
void runtime_preempt_point(void);

/* Ends the calling goroutine. Does not return.
 *
 * This is runtime.Goexit. Go runs the goroutine's deferred calls on the way
 * out, and this will too once defer exists, which is the next change but one.
 * Today it stops the goroutine and nothing else, which is the same thing for a
 * goroutine with no defers and is the only case that can arise yet.
 *
 * Go says calling it from the main goroutine ends that goroutine and leaves the
 * program running, and then crashes when there is nothing left to run. burrow
 * does the first part: the main goroutine ends, runtime_main returns, and the
 * other goroutines are stopped the way they are for any other return from main.
 * That difference exists because burrow's main goroutine returns to a caller
 * and Go's does not have one. */
BURROW_NORETURN void runtime_goexit(void);

/* ---------------------------------------------------------------- the counts
 *
 * How many goroutines may run at once, and how to change it.
 *
 * runtime.GOMAXPROCS. A positive n sets it and returns what it was before. A
 * zero or negative n only asks. It is the number of Ps, so it bounds the number
 * of goroutines running at the same instant rather than the number of threads,
 * and a program with ten thousand goroutines blocked on I/O is unaffected by
 * it.
 *
 * The default is the number of processors the machine has, which under a
 * container cpu limit or a cpuset is not the number this process may use. Go
 * reconciles those. burrow does not yet, so a program in a container with a
 * fraction of a core should set this itself.
 *
 * One limitation. Changing the number while the scheduler is running means
 * taking Ps away from threads that are using them, which is stopping the world.
 * burrow can ask a goroutine to give way but it cannot make one that is between
 * safe points do it, so there is no bound on how long stopping the world would
 * take and no honest way to offer it. A call with a positive n while
 * runtime_main is running therefore changes nothing and returns the current
 * value. Call it before runtime_main and it does what Go does. */
int runtime_gomaxprocs(int n);

/* How many processors the machine has. runtime.NumCPU, and the same number
 * burrow/thread.h answers with. */
int runtime_numcpu(void);

/* How many goroutines exist right now, running, runnable or parked.
 *
 * runtime.NumGoroutine. A snapshot, and on a busy program it is out of date
 * before you read it, which is true of Go's as well. It is for reporting and
 * for tests, and a program that branches on it is a program with a race in it.
 *
 * The goroutine asking is counted. Zero means the scheduler is not running. */
int runtime_numgoroutine(void);

/* ---------------------------------------------------------- park and ready
 *
 * The two calls everything that blocks is built on, and the reason a goroutine
 * waiting on a channel does not cost a thread.
 *
 * They are spelled sched_ rather than runtime_ because Go's are called gopark
 * and goready, which are not exported names and would leave these as runtime_
 * park and runtime_ready, and because park and ready on their own in a library
 * with no prefix is asking for a collision. docs/design/08-naming-abi.md
 * section 3 lists this as one of exactly three renames in the whole library.
 *
 * The shape to copy, from every user of these there will ever be:
 *
 *     lock(&c->lock);
 *     ...
 *     w.g = sched_current();
 *     w.next = c->waiters;
 *     c->waiters = &w;
 *     sched_park(unlock_chan, c);
 *     ... woken, and whoever woke us left the answer in w
 *
 * and on the other side, with the lock held:
 *
 *     c->waiters = w->next;
 *     w->result = value;
 *     sched_ready(w->g);
 */

/* Called by sched_park once the goroutine can no longer be reached on this
 * thread, with the goroutine and the pointer that was handed to sched_park.
 *
 * Its job is to release whatever lock was protecting the list the goroutine was
 * just put on. It has to be a callback rather than something the caller does
 * first, because between dropping the lock and parking there is a window where
 * somebody else can see the goroutine on the list and ready it, and readying a
 * goroutine that has not parked yet is how a wakeup gets lost.
 *
 * Answering false says do not park after all, and the goroutine carries on as
 * if sched_park had not been called. That is for the case where taking one more
 * look under the lock turned up the thing the goroutine was about to wait for. */
typedef bool (*SchedUnlockFn)(Goroutine *g, void *lock);

/* The goroutine this call is running on.
 *
 * Never NULL inside a goroutine. NULL on a thread that is not one of the
 * scheduler's, which is how a library can tell whether it may park. */
BURROW_BORROWS(ret) Goroutine *sched_current(void);

/* Stops the calling goroutine and hands its thread to the scheduler.
 *
 * Comes back when somebody calls sched_ready on it, and not before. A goroutine
 * that nobody readies is parked forever, which costs its stack and no thread,
 * and which is what a deadlocked Go program looks like too.
 *
 * unlockf may be NULL, and then lock is ignored and the park is unconditional.
 * That is the right call for a goroutine parking on something that is already
 * safely published, and the wrong one for anything with a lock in it, for the
 * reason written above SchedUnlockFn. */
void sched_park(SchedUnlockFn unlockf, void *lock);

/* Makes a parked goroutine runnable again.
 *
 * Puts it in the runnext slot of the P this call is running on, because the
 * goroutine being readied is usually the one that wants the value this
 * goroutine just produced, and running it next on this core is what keeps that
 * value in this core's cache.
 *
 * Callable from a goroutine and from a thread that is not one, which is what a
 * signal handler or a callback from a C library needs. It is not safe from an
 * actual signal handler yet, because it may take the scheduler lock.
 *
 * A call from a thread that is not a goroutine holds the runtime open until it
 * returns. That matters because the goroutine being readied may be the one
 * runtime_main is waiting for, so the run can end part way through this call,
 * and without the promise the memory this is standing on would be freed
 * underneath it. runtime_main waits instead.
 *
 * Readying a goroutine that is not parked is a bug and stops the program, since
 * the alternative is two threads running one stack. */
void sched_ready(Goroutine *g);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_PROC_H */
