/* Channels: the thing goroutines exist in order to talk to.
 *
 * A channel is a typed queue with a rendezvous built into it. One goroutine
 * sends, another receives, and whichever of them arrives first waits for the
 * other. That is the whole idea, and everything below is the detail Go's
 * version is specific about and a naive queue with a condition variable on it
 * gets wrong.
 *
 *     Chan *c = chan_make(a, TYPE_INT, 0);
 *
 *     go(BURROW_FN(Func, producer, c));
 *
 *     int v;
 *     while (chan_recv(c, &v))
 *         use(v);
 *
 * The loop ends when the producer calls chan_close. That is Go's `for v :=
 * range c` and it is the shape most channel code has.
 *
 * Three things are worth knowing before you use one.
 *
 * A channel made with a capacity of zero is unbuffered, and an unbuffered send
 * does not complete until a receiver has taken the value. It is a handoff and
 * not a queue of one. The value goes straight from the sender's variable into
 * the receiver's, with no copy through the channel in between, which is
 * observable: by the time chan_send returns, the receiver has the value.
 *
 * A channel with a capacity holds that many values and a send only blocks once
 * it is full. Those values are copied into the channel and out again, so the
 * sender is free as soon as there is room.
 *
 * Closing is a broadcast and it is one way. Every receiver blocked on a closed
 * channel wakes up, every receive after the buffer drains answers false
 * immediately, and there is no reopening. Closing is how a sender says there
 * will be no more, which is why closing is the sender's job and never the
 * receiver's.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CHAN_H
#define BURROW_CHAN_H

#include "burrow/core.h"
#include "burrow/mem.h"
#include "burrow/type.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The rules, which are Go's exactly, including the ones that stop the program,
 * because those are the ones that turn a race into a crash at the place the
 * mistake was made rather than a wrong answer somewhere else later.
 *
 *   send on a closed channel      stops the program
 *   close of a closed channel     stops the program
 *   close of a NULL channel       stops the program
 *   receive on a closed channel   the zero value and false, once drained
 *   send or receive on NULL       blocks forever
 *
 * Stopping the program is runtime_throw today and will be a panic once defer
 * and recover land, at which point every one of these becomes recoverable and
 * nothing here changes. See burrow/runtime.h for why the mechanism is temporary
 * and the messages are not.
 *
 * Blocking forever on a NULL channel is Go's behaviour for a nil one and it is
 * useful rather than a trap: a select with a case on a channel variable that is
 * NULL is a case that can never fire, which is how a loop turns one of its arms
 * off. Go's runtime notices when every goroutine in the program is blocked that
 * way and says so. burrow does not, because a burrow program has threads the
 * runtime knows nothing about and any one of them may be about to wake
 * somebody. docs/design/06-runtime.md has the argument.
 *
 * The memory guarantees are Go's too. A send happens before the receive of that
 * value completes, and a close happens before a receive that returns false.
 * They are the reason a channel is a synchronisation primitive and not merely a
 * queue: everything the sending goroutine did before the send is visible to the
 * receiving goroutine after the receive. */

/* Opaque and allocated, the way Go's channel is. A Go channel variable is a
 * pointer to a header the runtime owns, which is why passing a channel to a
 * function lets that function send on it, and why two copies of a channel
 * variable are the same channel. Chan * behaves the same way for the same
 * reason. */
typedef struct Chan Chan;

/* make(chan T, cap).
 *
 * `cap` of zero is an unbuffered channel, which is the rendezvous. A positive
 * cap is a buffer of that many elements, allocated here along with the header
 * so that a channel is one allocation rather than two.
 *
 * Returns NULL if the allocation fails, which Go does not have to do because Go
 * throws there. A caller that would rather stop than check can, and the check
 * is one branch on a call that happens once per channel.
 *
 * Stops the program if cap is negative, which is Go's `makechan: size out of
 * range`, and if elem is NULL, because a channel with no element type cannot
 * copy anything.
 *
 * The channel remembers the allocator, for the same reason a Map does: sends
 * and receives must not need one, and a buffer freed to a different allocator
 * than it came from is a bug nobody sees until much later. */
BURROW_OWNS(ret) Chan *chan_make(Alloc *a, const Type *elem, Int cap);

/* Gives the channel and its buffer back to the allocator it came from.
 *
 * Go has no such call, because Go has a collector. The rule here is the one
 * every other owning type in burrow has: whoever made it frees it, and an arena
 * user can ignore this entirely and free the arena.
 *
 * Freeing a channel that goroutines are still blocked on stops the program,
 * because the alternative is a parked goroutine holding a pointer into memory
 * that has been handed back. Close the channel, let the receivers drain it, and
 * free it after. Freeing NULL does nothing.
 *
 * A closed channel with values still in its buffer is fine to free. The values
 * are dropped, which is what happens to them in Go when the last reference
 * goes. */
void chan_free(Chan *c);

/* c <- v.
 *
 * Blocks until a receiver takes the value on an unbuffered channel, or until
 * there is room on a buffered one. Copies elem->size bytes from v, so v points
 * at a value of the channel's element type and is not kept after this returns.
 *
 * Stops the program on a closed channel. That is Go's panic and it is the right
 * one: a send on a closed channel means the sender and the closer disagree
 * about who owns the channel, and there is no value this call could return that
 * would make the program correct.
 *
 * Blocks forever on a NULL channel. */
void chan_send(Chan *c, const void *v);

/* v, ok := <-c.
 *
 * Blocks until there is a value, then copies it to out and answers true. False
 * means the channel is closed and empty and there will never be another value,
 * and out is set to the element type's zero value so that a caller who ignores
 * the answer gets Go's zero rather than whatever was on the stack.
 *
 * out may be NULL for a receive whose value is not wanted, which is Go's
 * `<-c`. The value is still taken off the channel.
 *
 * Blocks forever on a NULL channel. */
bool chan_recv(Chan *c, void *out);

/* The two non-blocking halves, which are `select` with a default arm.
 *
 * chan_try_send answers whether it sent. chan_try_recv answers whether a value
 * or a close was there to be had, and sets *ok the way chan_recv's return value
 * works: true for a real value, false for a receive from a drained closed
 * channel. So a false return means the channel was empty and open and nothing
 * happened at all, which is the case a default arm exists for.
 *
 * ok may be NULL if the caller does not care to tell those apart, and out may
 * be NULL the way chan_recv's may.
 *
 * Both answer false on a NULL channel, without blocking, because a case on a
 * nil channel is a case that never fires. chan_try_send stops the program on a
 * closed channel, exactly as a send does, since select does not make a send on
 * a closed channel legal.
 *
 * These are here rather than being left to chan_select because they are half
 * the uses of select in real code and because a select with one case and a
 * default should not need an array and a switch. */
bool chan_try_send(Chan *c, const void *v);
bool chan_try_recv(Chan *c, void *out, bool *ok);

/* close(c).
 *
 * Wakes every goroutine blocked on the channel. Receivers waiting get the zero
 * value and false, senders waiting stop the program, since they were about to
 * send on a channel that is now closed.
 *
 * Stops the program on a closed channel and on NULL, both of which are Go's
 * panics. Closing is not idempotent on purpose: a program that closes twice has
 * two things that both believe they own the channel, and that is worth finding.
 *
 * Only the sending side may close, and where there are several senders that
 * means something above them has to. A channel is not a way to tell a sender to
 * stop, it is a way for a sender to say it has. */
void chan_close(Chan *c);

/* len(c) and cap(c). The number of values sitting in the buffer, and how many
 * it can hold. Both are zero for an unbuffered channel, since a value in flight
 * across a rendezvous is in neither goroutine's hands and in no buffer.
 *
 * Both answer zero for NULL, the way len and cap of a nil channel do in Go.
 *
 * A snapshot. On a channel anybody else is using it can be wrong before it is
 * returned, which is true of Go's as well, so this is for reporting and for
 * tests and a program that branches on it has a race in it. */
Int chan_len(const Chan *c);
Int chan_cap(const Chan *c);

/* What the channel carries, for code that was handed one and has to ask. This
 * is what fmt and a future reflect will use to print or walk one. NULL for a
 * NULL channel. */
BURROW_STATIC(ret) const Type *chan_elem(const Chan *c);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CHAN_H */
