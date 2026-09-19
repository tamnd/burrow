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

/* ------------------------------------------------------------------ select
 *
 * Waiting on several channels at once, and taking whichever is ready first.
 *
 * Go spells this `select` and it is a statement. C has no statement to hook, so
 * here it is an array of cases and a call that answers which one of them ran.
 * The answer is an index into the array, which lines up with a switch:
 *
 *     Int job;
 *     SelectCase cases[] = {
 *         BURROW_RECV(work, &job),
 *         BURROW_RECV(quit, NULL),
 *         BURROW_SEND(results, &answer),
 *     };
 *
 *     switch (chan_select(cases, 3)) {
 *     case 0:
 *         do_the_job(job);
 *         break;
 *     case 1:
 *         return;
 *     case 2:
 *         answer = next_answer();
 *         break;
 *     default:
 *         break;
 *     }
 *
 * That is the whole API. The rest of this is what it promises.
 *
 * With no BURROW_DEFAULT case the call blocks until one of the cases can run.
 * With one it never blocks: if nothing is ready the default's index comes back
 * and no channel was touched. That is Go's `default:` arm and it is what
 * chan_try_send and chan_try_recv are, for the single channel shape that does
 * not need an array.
 *
 * When more than one case is ready, the one that runs is chosen uniformly at
 * random. This is not a detail to be improved on later. Go does it, Go programs
 * are written on top of it, and a select that preferred the first ready case
 * would starve the last one in every loop that has a fast channel and a slow
 * one in it.
 *
 * A case on a NULL channel is a case that can never fire, which is the idiom
 * for turning an arm off. Set the channel variable to NULL when that arm is not
 * wanted and set it back when it is, and the case list stays the same shape.
 * A select where every case is on a NULL channel, and a select with no cases at
 * all, blocks forever, exactly as Go's does.
 *
 * The same channel may appear in as many cases as you like, including once to
 * send and once to receive.
 *
 * Everything else behaves the way the single channel calls above do. A receive
 * on a closed channel is ready at once and reports false through its ok
 * pointer. A send on a closed channel stops the program, and because the cases
 * are visited in a random order, a select with one ready case and one send on a
 * closed channel may or may not reach the second one. That is Go's behaviour
 * and it is worth knowing rather than relying on. */

typedef enum SelectOp {
    /* v, ok := <-c */
    SELECT_RECV = 0,
    /* c <- v */
    SELECT_SEND = 1,
    /* The default arm. Its channel and pointers are ignored. */
    SELECT_DEFAULT = 2,
} SelectOp;

/* One arm of a select. Build these with the macros below rather than by hand,
 * which keeps the unused halves zero and makes the case list read like Go's.
 *
 * `send` and `recv` are two fields rather than one union because a send offers
 * a const pointer and a receive is handed a mutable one, and a union would need
 * the const cast away at every use.
 *
 * Nothing here is copied. The pointers are read and written where they point,
 * during the call and not after it, so pointing them at locals is right. */
typedef struct SelectCase {
    SelectOp op;

    /* The channel. NULL is a case that never fires. Ignored for a default. */
    Chan *c;

    /* Where a send's value comes from. */
    const void *send;

    /* Where a receive's value goes, or NULL for a receive whose value is not
     * wanted. The channel is still read from. */
    void *recv;

    /* Where a receive's second answer goes, or NULL. True means a real value,
     * false means the channel was closed and drained, which is exactly what
     * chan_recv returns. Only written when this is the case that ran. */
    bool *ok;
} SelectCase;

/* The case builders.
 *
 *   BURROW_RECV(c, &v)           v := <-c
 *   BURROW_RECV_OK(c, &v, &ok)   v, ok := <-c
 *   BURROW_SEND(c, &v)           c <- v
 *   BURROW_DEFAULT               default:
 *
 * BURROW_RECV with NULL for the value is Go's `<-c` with nothing on the left.
 *
 * These are not in the BURROW_SHORT set. SEND, RECV and above all DEFAULT are
 * names other people's headers use, and a library that takes DEFAULT away from
 * its users has overstepped. */
#define BURROW_RECV(ch, out) ((SelectCase){.op = SELECT_RECV, .c = (ch), .recv = (out)})
#define BURROW_RECV_OK(ch, out, okp)                                                   \
    ((SelectCase){.op = SELECT_RECV, .c = (ch), .recv = (out), .ok = (okp)})
#define BURROW_SEND(ch, v) ((SelectCase){.op = SELECT_SEND, .c = (ch), .send = (v)})
#define BURROW_DEFAULT ((SelectCase){.op = SELECT_DEFAULT})

/* Runs exactly one of the cases and answers which one, as an index into
 * `cases`. Never answers anything else: a select that cannot run a case blocks
 * until it can, and a select with a default always has one it can run.
 *
 * `n` is how many cases there are. Zero blocks forever.
 *
 * Callable from a goroutine, which parks and costs no thread, and from a thread
 * of the host program's own, which sleeps and costs itself. Both are supported
 * for the same reason chan_send and chan_recv support both.
 *
 * A select with up to sixteen cases needs nothing but its own stack frame.
 * Above that it makes one allocation, from the allocator of the first channel
 * in the list, and gives it back before returning. Sixteen is a great many more
 * arms than a select in real code has, so this is written down for the person
 * reading an allocation profile rather than as something to design around. */
Int chan_select(SelectCase *cases, Int n);

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CHAN_H */
