# Goroutines

A goroutine is a function that runs on a stack of its own, scheduled onto operating system threads by the runtime rather than by the kernel. Starting one costs a few hundred nanoseconds and a page of memory, so a program can have a hundred thousand of them, which is the whole reason Go programs are written the way they are.

Everything here is `burrow/proc.h`. The header next to it, `burrow/sched.h`, is the inside of the scheduler and a program that is not itself part of the runtime does not want it.

## Where the world begins

Go's runtime starts before your `main` because the Go toolchain arranges it. There is no toolchain here, so something has to say where the goroutine world begins and ends, and that is `runtime_main`.

```c
#include "burrow/proc.h"

static void run(void *env) {
    (void)env;
    /* This is func main. */
}

int main(void) {
    runtime_main(BURROW_FN(Func, run, NULL));
    return 0;
}
```

`runtime_main` creates the Ps, starts one thread for each of them, puts `run` on a run queue as the first goroutine, and goes to sleep. When `run` returns, every thread the scheduler started is stopped and joined before `runtime_main` returns, so by the time your `main` gets control back nothing of burrow's is still running.

That is the one call in this header that Go does not make you write. Everything inside it is Go.

## Starting one

```c
static void worker(void *env) {
    Job *j = env;
    ...
}

go(BURROW_FN(Func, worker, job));
```

`go` is Go's `go f()`. It returns as soon as the goroutine exists, which is before the goroutine has run, and there is no handle and no way to wait for it. Waiting is what a channel or a wait group is for, exactly as in Go.

It answers `bool`, which Go's `go` does not, because there is one way for it to fail: a stack could not be mapped. Go's answer to that is to end the program. A library can do better, since a server that cannot start one more connection handler can still serve the connections it has. Check it or ignore it, but know that ignoring it is a decision.

The function value is an ordinary `Func`, which is `burrow/func.h`'s name for `func()`. A target takes `void *env` as its only parameter and the `BURROW_FN` macro pairs it with whatever it needs to remember. [guides/functions.md](functions.md) has the rest of it, including where the environment lives when the value outlives the frame that made it, which for a goroutine is almost always.

## How many run at once

`runtime_gomaxprocs` is `runtime.GOMAXPROCS`. It is the number of Ps, so it bounds how many goroutines are running at the same instant rather than how many threads exist, and a program with ten thousand goroutines blocked on a channel is unaffected by it.

```c
runtime_gomaxprocs(4);        /* before runtime_main */
runtime_main(BURROW_FN(Func, run, NULL));
```

A zero or negative argument only asks. The default is the number of processors the machine has, which under a container CPU limit or a cpuset is not the number this process may use. Go reconciles those and burrow does not yet, so a program in a container with a fraction of a core should set this itself.

One limitation, and it is temporary. Changing the number while the scheduler is running means taking Ps away from threads that are using them, which is stopping the world, which needs preemption, which is deliberately the last piece of the runtime to be built. So a call with a positive argument while `runtime_main` is running changes nothing and returns the current value. Call it before and it does what Go does.

## Yielding

```c
runtime_gosched();
```

`runtime.Gosched`. It puts this goroutine back on the run queue and gives the thread to something else, and it comes straight back if there is nothing else to run.

Until preemption lands this is the only thing that lets a compute loop share a thread. A loop with no call in it that blocks and no call to this holds its thread until it finishes, and if every thread is held that way, nothing else in the program runs. That is the same bug that used to hang a Go program before Go 1.14, and it has the same fix, which is on the list.

## Ending one

A goroutine ends when its function returns. `runtime_goexit` ends it where it stands.

```c
runtime_goexit();
```

Go runs the goroutine's deferred calls on the way out and this will too once `defer` exists. Today it stops the goroutine and nothing else, which is the same thing for a goroutine with no defers and is the only case that can arise yet.

Go says calling it from the main goroutine ends that goroutine and leaves the program running, and then crashes when there is nothing left to run. burrow does the first part: the main goroutine ends and `runtime_main` returns. That difference exists because burrow's main goroutine returns to a caller and Go's does not have one.

## Building something that blocks

Channels, mutexes, wait groups, timers and the netpoller are all the same shape underneath. Remember which goroutine is waiting, park it, and ready it again when the thing it was waiting for happens. `sched_park` and `sched_ready` are that shape, and they are the reason a goroutine waiting on a channel costs a stack and not a thread.

```c
static bool unlock_chan(Goroutine *g, void *arg) {
    (void)g;
    unlock(&((Chan *)arg)->lock);
    return true;
}

/* The waiting side, with the lock held. */
w.g = sched_current();
w.next = c->waiters;
c->waiters = &w;
sched_park(unlock_chan, c);
/* Woken, and whoever woke us left the answer in w. */
```

```c
/* The other side, also with the lock held. */
c->waiters = w->next;
w->result = value;
sched_ready(w->g);
```

The callback is the part that is easy to get wrong. Dropping the lock and then calling `sched_park` leaves a window where somebody else can see the goroutine on the list and ready it, and readying a goroutine that has not finished parking is how a wakeup gets lost. So the unlock happens inside the park, once the goroutine can no longer be reached on this thread.

Answering `false` from the callback says do not park after all, and the goroutine carries on as if `sched_park` had not been called. That is for the case where one more look under the lock turned up the thing the goroutine was about to wait for.

`sched_current` answers `NULL` on a thread that is not one of the scheduler's, which is how a library can tell whether it may park at all.

These two are spelled `sched_` rather than `runtime_` because Go's are called `gopark` and `goready`, which are not exported names, and because `park` and `ready` on their own in a library with no prefix is asking for a collision. [design/08-naming-abi.md](../design/08-naming-abi.md) section 3 lists this as one of exactly three renames in the whole library.

## Stacks

This is the one place a burrow goroutine is not a Go goroutine.

Go starts a goroutine on eight kilobytes and grows the stack by copying it somewhere bigger when it runs out. Copying a stack means finding every pointer into it and moving it, which Go's compiler can do because it emits a map of where the pointers are, and which nothing can do for C. A pointer to a local is an ordinary word in memory and there is no way to find it, so a burrow stack is decided once and never moves.

The default is a quarter of a megabyte, and it is not as expensive as it sounds. The mapping is lazy on every system burrow targets, so a goroutine that uses one page costs one page and the rest is address space, which 64 bit machines have a great deal of. A million goroutines is a quarter of a terabyte of address space and as much real memory as they actually touch.

```c
go_stack(BURROW_FN(Func, parse, input), 4 * 1024 * 1024);
```

`go_stack` takes a size in bytes, rounded up to a whole page and raised to the platform minimum. Zero means the default. Ask for more when a goroutine recurses, and define `BURROW_GOROUTINE_STACK` to change the default for a whole program: a server holding a million connections may want less, a recursive descent parser may want considerably more.

Running off the end is not silent. Every stack is mapped with unreadable pages underneath it, and landing on one produces `fatal error: stack overflow` rather than a segmentation fault with nothing to go on. [guides/failure.md](failure.md) has the details, including the two cases the guard cannot catch.

## Counting

```c
runtime_numcpu();        /* processors on the machine */
runtime_numgoroutine();  /* goroutines that exist right now */
```

`runtime_numgoroutine` counts running, runnable and parked goroutines, including the one asking. It is a snapshot, and on a busy program it is out of date before you read it, which is true of Go's as well. It is for reporting and for tests, and a program that branches on it is a program with a race in it.

Both answer zero outside `runtime_main`.

## What the scheduler does

The algorithms are Go's, from `runtime/proc.go`, and the names are kept letter for letter so that anybody who has read that file can read this one.

A runnable goroutine is in one of three places. The `runnext` slot of a P holds the one goroutine that was just made runnable and is about to be handed over to, which is what keeps a channel handoff on one core and in one cache. Behind it is a lock free ring of 256 per P. Everything that overflows goes on a global queue under a lock.

A thread with nothing to run looks in its own ring, then the global queue, then steals half of another P's ring, picking the Ps to try in a random order with a random stride so that two thieves do not walk the same path. It takes a victim's `runnext` only on the last pass of a search that has already failed everywhere else, because that goroutine is the one the victim is about to run.

A thread that finds nothing gives its P up and looks again before parking. Doing it in that order is what stops a wakeup from being lost: a thread that checked the queues and then gave up its P would leave a window where somebody can add work, look for an idle P, not find one because this thread still has it, and go away again.

`runtime_gosched` from a goroutine puts it on the global queue rather than the local one, which is Go's behaviour and matters for fairness. `sched_ready` puts the woken goroutine in `runnext`, which is also Go's, for the cache.

## What is not here yet

Four things, all of them on the P0 milestone.

Timers, which is what makes `sched_park` able to come back on its own after a while. The netpoller, which is what makes a blocked read give its thread up instead of holding it. Channels and `select`, which are the interface most of this exists to support. And preemption, which is what stops a goroutine that never yields from holding a thread forever.

Until preemption lands, a goroutine that runs a long computation should call `runtime_gosched` from time to time. It is one function call and it costs about two hundred nanoseconds.

## See also

- [guides/functions.md](functions.md) for `Func`, `BURROW_FN` and where the environment lives
- [guides/failure.md](failure.md) for what a stack overflow prints and what happens next
- [design/06-runtime.md](../design/06-runtime.md) for why the scheduler is shaped this way
