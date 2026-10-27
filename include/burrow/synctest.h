/* Deterministic tests for concurrent code.
 *
 * A test for something concurrent usually ends up written one of two ways. It
 * sleeps for long enough that the thing under test has probably finished, which
 * is slow and is flaky in proportion to how loaded the machine is, or it grows a
 * pile of channels and wait groups that exist only so the test can tell when to
 * look, which means it now tests a different program from the one that ships.
 *
 * A bubble is the third way. Every goroutine started inside one belongs to it,
 * and the bubble knows at every moment whether any of them can still make
 * progress on its own. When none of them can, that is not a guess and not a
 * timeout, it is a fact the scheduler already had, and it is the moment a test
 * wants to look at the result.
 *
 *     static void body(void *env) {
 *         Chan *c = chan_make(heap_allocator(), TYPE_INT, 0);
 *
 *         go(BURROW_FN(Func, worker, c));
 *         synctest_wait();
 *
 *         Int v; // The worker is sitting on the send. Nothing slept.
 *         chan_recv(c, &v);
 *     }
 *
 *     synctest_run(BURROW_FN(Func, body, NULL));
 *
 * A bubble also has a clock of its own, and it is the same idea again. The
 * clock only moves when nothing in the bubble can move, and then it jumps
 * straight to whatever timer is due next. So a sleep of an hour inside a bubble
 * takes no time at all and still happens after a sleep of a minute that started
 * with it. What that is worth is a test for a timeout that runs in microseconds
 * and never has to pick a number small enough to be quick and large enough not
 * to be flaky. See time_sleep in burrow/time.h.
 *
 * Derived from Go's src/testing/synctest/synctest.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_SYNCTEST_H
#define BURROW_SYNCTEST_H

#include "burrow/core.h"
#include "burrow/func.h"
#include "burrow/testing.h"
#include "burrow/time.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Runs f in a new bubble and waits for every goroutine in it to finish.
 *
 * f gets a goroutine of its own, so a bubble is never the goroutine that asked
 * for one. Everything f starts, and everything those start, is in the bubble
 * too, and the bubble is not over when f returns: it is over when the last
 * goroutine that was ever in it has exited. That is Go's rule and it is the one
 * that makes a leaked goroutine a test failure rather than something the next
 * test finds out about.
 *
 * Answers false if the goroutine could not be started, which is the same and
 * only reason go answers false and means the same thing. True means the bubble
 * ran and is finished.
 *
 * Stops the program if every goroutine in the bubble is durably blocked, none
 * of them is in synctest_wait, and no timer is left to fire, because that is a
 * deadlock and there is nothing left that could end it. A bubble whose f has
 * returned and which still has goroutines blocked gets a different message,
 * because that one is a leak rather than a deadlock: the clock stops when f
 * returns, so a goroutine waiting on a timer in there waits forever.
 *
 * Stops the program if called from outside a goroutine, and if called from
 * inside a bubble, since bubbles do not nest.
 *
 * This is Go 1.24's Run. Go 1.25 replaced it with Test, which is synctest_test
 * below and is what a test should call. This stays because it needs no T, and
 * the tests of the bubble itself run without one. */
bool synctest_run(Func f);

/* Runs f in a new bubble, with a T of its own, and waits for every goroutine in
 * the bubble to exit before returning. Go's synctest.Test.
 *
 * The T that f gets has the same name as t and writes its output to t. Its
 * cleanups run inside the bubble, just before synctest_test returns, and its
 * context belongs to the bubble, so waiting on context_done of it is durably
 * blocked. testing_t_run, testing_t_parallel and testing_t_deadline must not be
 * called on it, and panic if they are.
 *
 * If f fails, t fails too, and synctest_test ends t the way testing_t_fail_now
 * does. That happens outside the bubble, so t's duration is real time.
 *
 * Go fails the test when the goroutines in the bubble deadlock. Here a deadlock
 * stops the program, the same as it does for synctest_run.
 *
 * Must not be called from inside a bubble. */
void synctest_test(TestingT *t, TestingTFunc f);

/* Blocks until every other goroutine in the bubble is durably blocked.
 *
 * This is the call that replaces the sleep. After it returns, every other
 * goroutine in the bubble has got as far as it can without help, so whatever
 * they were going to do to the state this goroutine is about to look at, they
 * have done.
 *
 * A goroutine is durably blocked when the only thing that can wake it up is
 * another goroutine in the same bubble. Waiting to receive on a channel that
 * was made inside the bubble is durable, because the only sender that can reach
 * that channel is in here. Waiting on a socket is not, because the network can
 * deliver a packet at any time and the bubble has no say in it. Waiting on a
 * channel that was made outside the bubble is not either, for the same reason:
 * somebody out there is holding the other end.
 *
 * That distinction is why this is worth having. Something that merely watched
 * the run queues would have to treat every blocked goroutine alike, and a test
 * that waited for a socket read to be "blocked" would be a test that waited for
 * the network. Here the question is answered at the park itself, by the code
 * that knows what is being waited for.
 *
 * A goroutine that has been woken but has not run yet counts as running, so
 * this does not return early on a goroutine that is on its way back. That is
 * the case a hand-rolled version of this always gets wrong.
 *
 * Stops the program if called from outside a bubble, and if another goroutine
 * in the same bubble is already inside this call, because two goroutines
 * waiting for each other to block is a test that cannot be read. */
void synctest_wait(void);

/* Sleeps for d on the bubble's clock and then waits, which is exactly
 *
 *     time_sleep(d);
 *     synctest_wait();
 *
 * and is usually what a test wants instead of the sleep alone. If the test and
 * the code under test both sleep for the same minute, which of them runs first
 * when the minute is up is anybody's guess. This lets the other one finish
 * whatever it does at the minute before the test looks.
 *
 * Stops the program in the same cases synctest_wait does. */
void synctest_sleep(Duration d);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SYNCTEST_H */
