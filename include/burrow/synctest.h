/* Deterministic tests for concurrent code.
 *
 * A test for something concurrent usually ends up written one of two ways.
 * Either it sleeps for long enough that the thing under test has probably
 * finished, which makes the suite slow and flaky in proportion to how loaded
 * the machine is, or it grows a pile of channels and wait groups that exist
 * only so the test can tell when to look, which means the test is now testing a
 * different program from the one that ships.
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
 *         // Every goroutine in the bubble is blocked now, so the worker is
 *         // sitting on the send. Nothing raced to get here and nothing slept.
 *         Int v;
 *         chan_recv(c, &v);
 *     }
 *
 *     synctest_run(BURROW_FN(Func, body, NULL));
 *
 * Two calls, and the whole of the idea is in what "blocked" means, which is
 * written out at synctest_wait below.
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
 * Stops the program if every goroutine in the bubble is durably blocked and
 * none of them is in synctest_wait, because that is a deadlock and there is
 * nothing left that could end it. Stops the program if called from outside a
 * goroutine, and if called from inside a bubble, since bubbles do not nest.
 *
 * Go 1.25 replaced Run with Test, which takes the *testing.T so that a deadlock
 * fails the test rather than the process. burrow has no testing package yet, so
 * the shape here is Go 1.24's Run, and synctest_test arrives with testing. */
bool synctest_run(Func f);

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

#ifdef __cplusplus
}
#endif

#endif /* BURROW_SYNCTEST_H */
