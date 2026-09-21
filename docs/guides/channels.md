# Channels

A channel is a typed queue with a rendezvous built into it. One goroutine sends, another receives, and whichever arrives first waits for the other.

That sentence is the whole idea, and it is why Go programs share memory by communicating rather than the other way round. Everything below is `burrow/chan.h`.

```c
#include "burrow/chan.h"

static void producer(void *env) {
    Chan *c = env;
    for (Int i = 0; i < 10; i++)
        chan_send(c, &i);
    chan_close(c);
}

static void run(void *env) {
    (void)env;

    Chan *c = chan_make(heap_allocator(), TYPE_INT, 0);
    go(BURROW_FN(Func, producer, c));

    Int v;
    while (chan_recv(c, &v))
        printf("%lld\n", (long long)v);

    chan_free(c);
}
```

That loop is Go's `for v := range c`. It ends when the producer closes the channel, and it is the shape most channel code has.

## Values go by pointer

`chan_send` takes a `const void *` and `chan_recv` takes a `void *`, for the same reason `map_set` does: C has no generics and a channel has to work for any element type.

```c
Int v = 42;
chan_send(c, &v);       /* copies sizeof(Int) bytes out of v */

Int got;
chan_recv(c, &got);     /* copies them into got */
```

The channel copies `elem->size` bytes and keeps nothing. A pointer you send is not held after the call returns, and a value you receive is yours. So sending a stack local is fine, and this

```c
for (Int i = 0; i < 10; i++)
    chan_send(c, &i);   /* fine: the value is copied, not the address */
```

does what it looks like it does, which is not true of a queue of `void *`.

The copy goes through the type descriptor rather than through `memcpy`, so a channel of `Str` or of a struct with a `Str` in it copies each field the way that type says to.

`chan_recv` will take `NULL` for a value you do not want, which is Go's `<-c` with nothing on the left. The value still comes off the channel.

## Buffered and unbuffered

`chan_make(a, elem, 0)` is unbuffered and it is the rendezvous. A send does not complete until a receiver has taken the value.

```c
chan_send(c, &v);
/* by the time this line runs, some other goroutine has v */
```

The value goes straight from your variable into the receiver's, with no copy through the channel in between. That is not an optimisation detail, it is the observable behaviour: an unbuffered channel is a handoff and not a queue of one.

`chan_make(a, elem, 4)` holds four values. A send only blocks once the buffer is full, and a receive only blocks once it is empty, so the sender can get four ahead before the receiver has to keep up.

Which one you want is a design question and not a performance question. Unbuffered means the two sides run in lockstep, which is what you want when the send is a handoff of responsibility. Buffered means the sender can run ahead, which is what you want when the work arrives in bursts and you would rather absorb one than stall.

`chan_len` is how many values are sitting in the buffer and `chan_cap` is how many it holds. Both are zero for an unbuffered channel, because a value in flight across a rendezvous is in neither goroutine's hands and in no buffer. Both are a snapshot: on a channel somebody else is using, the answer can be wrong before it is returned, so this is for reporting and for tests, and a program that branches on it has a race in it.

## Closing

Closing says there will be no more values. It is a broadcast and it is one way.

```c
chan_close(c);
```

Every receiver blocked on the channel wakes up. Every receive after the buffer drains answers false immediately and writes the element type's zero value, for ever. There is no reopening.

That is what ends the `while (chan_recv(c, &v))` loop, and it is why closing is the sender's job. A receiver that closes has told the sender something the sender did not agree to, and the sender's next send panics. Where there are several senders, something above all of them has to close, once they have all finished.

Closing is not idempotent and that is on purpose. A double close panics because it means two things both believe they own the channel, and that is worth finding at the second close rather than much later.

## The rules that panic

These are Go's, exactly.

| what | what happens |
| --- | --- |
| send on a closed channel | panics |
| close of a closed channel | panics |
| close of a `NULL` channel | panics |
| receive on a closed channel | the zero value and false, once drained |
| send or receive on `NULL` | blocks forever |

They look harsh for a C library. They are the right ones anyway, because each of them is a statement that two parts of the program disagree about who owns the channel, and there is no value any of these calls could return that would make such a program correct. A send on a closed channel that quietly did nothing would turn a race into lost data somewhere else later, which is worse in every way than a crash at the line that made the mistake.

The panic is the ordinary kind. A `BURROW_TRY` around the call catches it, the value is a `RuntimeError` carrying Go's text, and the channel's lock is released before the panic goes up, so the channel is still usable from another goroutine afterwards. A server that does not want one confused handler to take the whole process down can survive this, which is the same latitude Go gives.

Blocking forever on a `NULL` channel is Go's behaviour for a nil one, and it is useful rather than a trap. A select case on a channel variable that is `NULL` is a case that can never fire, which is how a loop turns one of its arms off, and that idiom only works if a bare send or receive agrees. `chan_try_send` and `chan_try_recv` both answer false on `NULL` without blocking, for the same reason.

Go's runtime notices when every goroutine in a program is blocked like that and says so. burrow does not, because a burrow program has threads the runtime knows nothing about and any one of them may be about to wake somebody up. [docs/design/06-runtime.md](../design/06-runtime.md) has the argument.

## Not waiting

`chan_try_send` and `chan_try_recv` are a select with a default arm, which is half the uses of select in real code.

```c
if (!chan_try_send(work, &job)) {
    /* the queue is full, shed the load rather than blocking on it */
    drop(job);
}
```

`chan_try_send` answers whether it sent. `chan_try_recv` answers whether anything was there to be had, and sets `*ok` the way `chan_recv`'s return value works:

| returned | `*ok` | what it means |
| --- | --- | --- |
| true | true | a real value, in `out` |
| true | false | the channel is closed and drained, `out` is zeroed |
| false | untouched | nothing happened, the channel is open and not ready |

So a false return is the case the default arm exists for. `ok` may be `NULL` if you do not need to tell the first two apart.

`chan_try_send` panics on a closed channel, exactly as a send does. A select does not make a send on a closed channel legal.

## Waiting on several at once

`chan_select` is Go's `select`. The name has the extra word because POSIX got to `select` first and a header that takes it away from you is a header nobody can include.

You describe the arms and it tells you which one ran.

```c
Int job;
Str msg;

SelectCase cases[] = {
    BURROW_RECV(work, &job),
    BURROW_RECV(control, &msg),
};

switch (chan_select(cases, 2)) {
case 0:
    do_the_job(job);
    break;
case 1:
    obey(msg);
    break;
default:
    break;
}
```

The return value is the index of the arm that ran, so the `switch` is on the same numbers the array is in. There are four ways to build an arm.

| arm | what it does |
| --- | --- |
| `BURROW_RECV(c, out)` | receive into `out`, which may be `NULL` if you only want to know it happened |
| `BURROW_RECV_OK(c, out, okp)` | the same with `*okp` set the way `chan_recv`'s return value works |
| `BURROW_SEND(c, v)` | send the value at `v` |
| `BURROW_DEFAULT` | run when nothing else is ready |

With no default arm, `chan_select` waits until one of the channels is ready. With a default arm it never waits: if nothing else can run at the moment it looks, the default does. There is no third behaviour, which is Go, and it means a select with a default in a tight loop is a spin and should be a select without one.

When more than one arm is ready, the one that runs is chosen uniformly at random. That is not a detail to work around. It is what keeps a channel that is always ready from starving one that is only sometimes ready, and code that depends on arm order is code that will starve something.

### Turning an arm off

An arm on a `NULL` channel never fires, which is how a loop stops listening to a channel that has closed without rewriting the array.

```c
Chan *a = ..., *b = ...;

while (a != NULL || b != NULL) {
    Int v;
    bool ok;
    SelectCase cases[] = {
        BURROW_RECV_OK(a, &v, &ok),
        BURROW_RECV_OK(b, &v, &ok),
    };

    Int arm = chan_select(cases, 2);
    if (!ok) {
        /* that one is closed and drained, so stop listening to it */
        if (arm == 0)
            a = NULL;
        else
            b = NULL;
        continue;
    }
    use(v);
}
```

A closed channel is ready forever, so an arm left pointing at one would spin the loop. Setting the variable to `NULL` is what Go code writes here and it works for the same reason.

A select where every arm is a `NULL` channel and there is no default blocks forever, which is the same answer a receive on a `NULL` channel gives.

### The rules

A send arm on a closed channel panics, exactly as a bare send does. Because the choice among ready arms is random, a select that has such an arm may panic on one run and not on the next, which is worth knowing when you are reading a crash report.

The same channel may appear in several arms. It is locked once.

`chan_select` with no arms at all blocks forever. It is not an error, it is the empty `select {}`.

### What it costs

Up to sixteen arms need nothing but your own stack frame. Past that there is one allocation, taken from the allocator of the first channel in the list, and it is given back before the call returns whichever way the call goes. Sixteen is well past what real code uses and a goroutine stack is a quarter of a megabyte, so the inline case is the case.

Two selects listing the same channels in different orders cannot deadlock, because the locks are taken in channel address order rather than in the order you wrote the arms.

## The allocator

`chan_make` takes an allocator and the channel remembers it, which is the same deal a `Map` gets and for the same reason: a send must not need one, and a buffer freed to a different allocator than it came from is a bug nobody sees until much later.

`chan_free` gives the channel and its buffer back. It is one allocation, header and buffer together, so it is one free.

Freeing a channel that goroutines are still blocked on stops the program, because the alternative is a parked goroutine holding a pointer into memory that has been handed back. Close the channel, let the receivers drain it, and free it after. A closed channel with values still in its buffer is fine to free, and the values are dropped, which is what happens to them in Go when the last reference goes.

Arena users can ignore all of this and free the arena.

## What it is underneath

One lock per channel and nothing clever on top of it. That looks like the wrong answer and is not: a channel operation is a handful of pointer writes and one element copy, the lock is held for exactly that, and Go has tried the lock free version more than once and has this in the tree.

A send tries three things in order, and the order is the design.

1. **A receiver is already waiting.** Copy the value straight into the receiver's variable and make it runnable. The value never touches the channel, which is why an unbuffered channel with no buffer works, and why a buffered channel that happens to be empty still skips the buffer.
2. **There is room in the buffer.** Copy it in and carry on.
3. **Neither.** Park, holding out a pointer to the value rather than a copy of it, and let whoever receives next take it from there. A send of a large struct across a full channel still copies it exactly once.

A receive is the mirror of that, with one wrinkle in the third case. If the buffer is full and a sender is parked behind it, the value that comes out is the one at the head of the buffer and not the one the sender is holding, and the sender's goes in at the tail. Head and tail are the same slot when the buffer is full, so it is one slot read and then written. Getting that backwards gives a channel that reorders values only when it is under pressure.

The record of a blocked goroutine lives on that goroutine's stack, which is Go's `sudog` without the pool. A parked goroutine's stack is not going anywhere, and the one thing a pool buys is not needed when the record is already free.

### One thing Go has no equivalent of

A thread that is not running a goroutine can block on a channel.

burrow is a library inside somebody else's program, and that program has its own threads. If one of them sends on a channel that a goroutine is going to receive from, that works.

The difference is what it costs. A goroutine that blocks costs no thread, which is the entire point of the scheduler. A host thread that blocks costs itself, because there is nothing to park and it has to sleep. So a worker pool of host threads talking to goroutines over a channel is correct but is not the cheap thing, and the cheap thing is to be a goroutine.

It is also the part that was hardest to get right, which is worth a paragraph because it is the part most likely to be wrong somewhere else. A blocked host thread sleeps on a gate that lives in its own stack frame, and that frame goes away the moment the sleep returns, while the thread that opened the gate still has a few instructions to run on it. Getting that wrong is invisible in ordinary running and obvious under a thread sanitizer.

The other half of it is the runtime thread on the receiving end. It has nothing to run while it waits, so it goes to sleep, and the host thread sending to it is the one that has to wake it up. That handshake had a gap in it, and a value sent into the gap left the runtime asleep with a goroutine ready to run. Fifty handoffs almost never land in the gap and twenty thousand do, which is why there is now a test that does twenty thousand. `docs/design/06-runtime.md` has both of these in full.

### How a select waits

A select that cannot run anything puts an entry on every one of its channels and goes to sleep. Any one of those entries being taken has to wake it exactly once, and the arm that took it is the arm that ran, so the whole thing turns on one flag: a sender, a receiver or a close that wants a waiting select has to win a compare and swap on that flag first, and the winner writes down which arm it was. Losers put the entry back and look for another one. That is Go's design.

The part that is not Go's is what happens next. Go keeps those entries in a pool, so the list the scheduler walks while parking the goroutine stays valid no matter what the goroutine does afterwards. burrow keeps them in the parking goroutine's own stack frame, which is the same choice the rest of this header makes and is free. The cost is a window: the scheduler marks a goroutine as waiting before it unlocks the channels, so the moment the first channel comes unlocked somebody may claim the select and start that goroutine on another thread, while the first thread is still reading the case list out of the frame it is standing on.

So the frame is handed over deliberately. Both sides swap a value into one word, the one that finds it untouched is the one that has to wait, and a claim that arrives before the unlock finishes leaves a note instead of a wakeup. `src/runtime/chan.c` has it written out. It is the kind of thing that is invisible in ordinary running and that a thread sanitizer finds in about a minute, which is the argument for running one.

## What is not here yet

`BURROW_SELECT` as a block, with `BURROW_CASE_RECV` and friends reading like Go's statement rather than like an array. The array is the honest version and stays whatever else arrives, and the sugar is worth having once there is a `time_after` to write `BURROW_CASE_AFTER` against, which needs timers to hand out channels and they do not yet.
