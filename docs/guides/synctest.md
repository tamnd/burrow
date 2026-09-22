# Deterministic tests for concurrent code

A test for something concurrent usually ends up written one of two ways, and both of them are bad.

The first sleeps. Start the worker, sleep ten milliseconds, check the result. It passes on your laptop and fails on a loaded CI machine, so somebody raises it to fifty milliseconds, and now the suite takes four minutes and still fails once a week. The number is a guess about a machine you have never seen.

The second instruments. Add a channel the worker sends on when it reaches the interesting point, add a wait group so the test knows when everything has finished, add a flag so the worker knows it is under test. Now the test is reliable and the thing being tested is not the thing that ships.

A bubble is the third way. Everything in `burrow/synctest.h`, and it is two functions.

## The two calls

```c
#include "burrow/synctest.h"

static void body(void *env) {
    (void)env;

    Chan *c = chan_make(heap_allocator(), TYPE_INT, 0);
    go(BURROW_FN(Func, worker, c));

    synctest_wait();

    /* Every other goroutine in the bubble is blocked now. */
    Int v;
    chan_recv(c, &v);
    chan_free(c);
}

static void top(void *env) {
    (void)env;
    synctest_run(BURROW_FN(Func, body, NULL));
}

int main(void) {
    runtime_main(BURROW_FN(Func, top, NULL));
    return 0;
}
```

`synctest_run` takes a function, gives it a goroutine of its own, and waits. Every goroutine that function starts, and everything those start, is in the bubble too. The run is not over when the function returns, it is over when the last goroutine that was ever in the bubble has exited, so a goroutine a test forgot about is a test that does not finish rather than a surprise two tests later.

`synctest_wait` blocks until every other goroutine in the bubble is durably blocked. That is the call that replaces the sleep, and the whole of the idea is in what durably blocked means.

## What durably blocked means

A goroutine is durably blocked when the only thing that can wake it up is another goroutine in the same bubble.

Waiting to receive on a channel that was made inside the bubble is durable. The only code that can reach that channel is code in the bubble, so if nothing in the bubble is running, nothing is going to arrive.

Waiting on a socket is not durable. The network can deliver a packet at any moment and the bubble has no say in it.

Waiting on a channel that was made outside the bubble is not durable either, for the same reason. Somebody out there is holding the other end.

That distinction is the point. Something that merely watched the run queues would have to treat every blocked goroutine alike, and a test that waited for a socket read to count as blocked would be a test that waited for the network. Here the question is answered at the park itself, by the code that knows what is being waited for.

A goroutine that has been woken but has not run yet counts as running, so `synctest_wait` does not return early on a goroutine that is on its way back. That is the case a hand rolled version of this always gets wrong.

Here is every wait in the library and the answer it gives.

| Waiting on | Durable | Why |
| --- | --- | --- |
| A channel made inside the bubble | Yes | Nothing outside can reach it. |
| A channel made outside the bubble | No | Somebody out there holds the other end. |
| `chan_select` where every channel was made inside | Yes | The same argument, for all the cases at once. |
| `chan_select` mixing inside and outside channels | No | One outside case is enough to complete the whole select. |
| A receive or send on a nil channel, or a select with no cases | Yes | Nothing can ever complete it, which is durable in the strictest sense. |
| `sync_cond_wait` | Yes | Only a Signal or a Broadcast ends it, and a bubble where everybody is here has nobody left to send one. |
| `sync_wait_group_wait`, when every Add came from inside the bubble | Yes | Then only a Done from inside can take the counter to zero. |
| `sync_wait_group_wait`, on a group used from outside | No | Anybody can call Done. |
| `sync_mutex_lock`, `sync_rw_mutex_lock`, `sync_rw_mutex_r_lock` | No | A goroutine waiting for a lock is waiting for the goroutine holding it, and that one is running. |
| `time_sleep` | No, for now | See the last section. |
| Anything built on `sched_park` outside the runtime | No | The runtime cannot tell what will end it, so the honest answer is that something might. |

A WaitGroup that is added to from inside a bubble belongs to that bubble until its counter reaches zero, and adding to it from outside in the meantime stops the program. That is the same rule channels have and it is there for the same reason: without it, a Wait that counted as durable could be ended by a Done from somewhere the test cannot see. The association goes away on its own as soon as the counter is back to zero, so the same group can be used again by a later bubble or by nothing in particular.

## What the test gets to say

Because the bubble knows the difference between blocked and finished, a test can assert on a program in the middle of its work rather than only at the end of it.

```c
static void worker(void *env) {
    Chan *c = env;

    Int v = 1;
    chan_send(c, &v);
}

static void body(void *env) {
    (void)env;

    Chan *c = chan_make(heap_allocator(), TYPE_INT, 0);
    go(BURROW_FN(Func, worker, c));
    synctest_wait();

    /* The worker is sitting on the send. Not probably, not usually. */
    Int v;
    chan_recv(c, &v);
    chan_free(c);
}
```

The `synctest_wait` above is worth reading twice. Without it, the `chan_recv` would work anyway, because an unbuffered receive waits for a sender. With it, the test has established that the worker got as far as the send and no further, which is a claim about the program rather than about this run of it.

## Channels belong to the bubble that made them

A channel made inside a bubble is a channel of that bubble's, and using it from outside stops the program.

That sounds strict and it is the rule that makes the rest work. If code outside the bubble could send on a bubbled channel, then a goroutine parked on that channel would not be durably blocked after all, and `synctest_wait` would be answering a question it could not actually answer. The alternative would be to give up on the whole idea and treat every channel as if the outside world might touch it.

A channel made outside the bubble and used inside it is fine, and waits on it are not durable. That is the escape hatch for a test that genuinely does want to talk to something outside, and the bubble stays honest about it.

## Deadlock

When every goroutine in a bubble is durably blocked and none of them is sitting in `synctest_wait`, nothing in there is ever going to run again. The program stops and says so.

That is a better answer than hanging, and it is available here for a reason that does not hold for a program at large: the bubble knows the complete set of goroutines involved and knows that none of them can be woken from outside. Go's runtime does the same thing for the same reason.

## What it costs

Nothing, for a program that never makes a bubble.

Every goroutine carries a bubble pointer, which is `NULL` for all of them outside a test and is inherited from whoever called `go`. Every channel carries one too, set when it is made. A send or a receive loads the channel's pointer and branches on it being `NULL`, and that is the whole of the fast path. There is no thread local lookup and nothing to switch on.

Inside a bubble a park costs a few atomic adds and one uncontended lock, a wake costs one atomic swap, and all of it lands on a single cache line shared by the goroutines in the bubble. A bubble with a thousand goroutines in it contends on that line, and a bubble with a thousand goroutines in it is a test that has other problems.

## What Go has that this does not, yet

Go 1.24 shipped this as `synctest.Run`. Go 1.25 replaced it with `synctest.Test`, which takes the `*testing.T` so that a deadlock fails the test rather than the process. burrow has no `testing` package yet, so the shape here is `Run`, and `synctest_test` arrives with `testing`.

Fake time is the other half and is not here yet. In Go, a bubble has a clock of its own that starts at midnight UTC on 1 January 2000 and moves forward only when every goroutine in the bubble is durably blocked, so a test that sleeps for an hour takes no time at all. Until that lands, `time_sleep` is not durable: a sleeping goroutine keeps counting as running, which means `synctest_wait` waits for it and a bubble with a timer in it does not report a false deadlock. The behaviour is honest, it is just slow, and the cure is a separate change.

## See also

- [guides/goroutines.md](goroutines.md) for `runtime_main`, `go` and what a park is
- [guides/channels.md](channels.md) for `chan_make`, `chan_select` and what a closed channel does
- [guides/sync.md](sync.md) for `WaitGroup`, `Cond` and the locks
- [guides/time.md](time.md) for `time_sleep` and the timer heap fake time will sit on
