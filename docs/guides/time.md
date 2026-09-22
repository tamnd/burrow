# Time

Sleeping, and running a function later on. This is `burrow/time.h`, and it is the half of Go's `time` package that needs a scheduler underneath it: `Duration` and its units, `time.Sleep`, and `time.AfterFunc` with the `Stop` and `Reset` that go with it.

The calendar half, the `Time` type with its wall clock and its formatting and its timezones, is a separate job and is not here yet. It does not need the runtime, so it can be built alongside anything.

## Durations

```c
Duration d = 500 * TIME_MILLISECOND;
Duration total = 2 * TIME_SECOND + 300 * TIME_MILLISECOND;
```

`Duration` is a count of nanoseconds and it is a plain `int64_t`, which is what Go's is. So `d / 2`, `d * 3` and `d1 < d2` all mean what they look like, and printing one takes the format for a long long.

Signed, because the difference between two readings of a clock can be negative, and 64 bits wide, which puts the longest duration at about 292 years. Anything longer than that is a date rather than a duration.

The units are `TIME_NANOSECOND`, `TIME_MICROSECOND`, `TIME_MILLISECOND`, `TIME_SECOND`, `TIME_MINUTE` and `TIME_HOUR`, and they are Go's constants with the names the mapping in [design/08-naming-abi.md](../design/08-naming-abi.md) gives them. Write the multiplication rather than the nanoseconds. `500000000` and `5000000000` look alike at a glance, one of them is ten times the other, and that is a bug that has shipped in real programs more than once.

## Reading the clock

```c
int64_t start = burrow_nanotime();
work();
Duration took = burrow_nanotime() - start;
```

`burrow_nanotime` is nanoseconds on a clock that only goes forwards, measured from an arbitrary point that means nothing on its own. Subtract two readings and the answer is a `Duration`.

It says `burrow` rather than `time` because Go has no such function, which is what rule R11a in [design/08-naming-abi.md](../design/08-naming-abi.md) asks for. Go's `time.Now` carries a monotonic reading around inside it and `time.Since` pulls it back out, so a Go program never names the clock directly. burrow has no `Time` yet and the parts that need a deadline need one now, so the reading is exposed on its own. `burrow/context.h` measures a deadline on this clock, and so does anything that has to know how long something took.

Never backwards and never a jump, which is the point of it. The wall clock does both whenever somebody sets the date or ntp corrects a drift, and a timeout measured on the wall clock either waits an hour or fires twice.

Callable from any thread, including one the runtime knows nothing about, and it costs a few nanoseconds everywhere, because every platform answers this out of the vdso or its equivalent rather than from a system call.

Inside a synctest bubble this reads the bubble's clock instead. See the section on fake time below.

## Sleeping

```c
#include "burrow/time.h"

static void poll(void *env) {
    (void)env;
    for (;;) {
        check_the_thing();
        time_sleep(30 * TIME_SECOND);
    }
}
```

`time_sleep` is `time.Sleep`. The goroutine stops for at least `d` and the thread it was running on goes and finds something else to do, which is the difference between this and every sleep C has. A thousand goroutines sleeping is a thousand stacks and no threads, and while they are all waiting the process is asleep in the kernel using no processor at all.

At least `d`, and never exactly. The goroutine becomes runnable when the time is up and then waits for a thread to pick it up, so a busy program hands it back late. That is true of every sleep in every language. What this promises is what Go promises: not early, and measured on the monotonic clock, so nothing anybody does to the system date can make it come back sooner or later.

Zero or less returns immediately without giving up the thread, which is Go's rule too. It is not a yield. If a yield is what you want, that is `runtime_gosched`.

Calling it off a scheduler thread, from your own `main` or from a test, sleeps the thread instead. There is no goroutine to park, so that is the only thing it can do, and it is a courtesy for setup code rather than something to build on.

## Running a function later

```c
static void give_up(void *env) {
    conn_close(env);
}

TimeTimer *t = time_after_func(a, 5 * TIME_SECOND, BURROW_FN(Func, give_up, conn));
if (t == NULL)
    return err_no_memory;

...

time_timer_stop(t);
time_timer_free(t);
```

`time_after_func` is `time.AfterFunc`. The function runs in a goroutine of its own once the duration is up, which is Go's rule and matters more than it sounds: the callback has a fresh stack, so it may block, take locks, sleep again or talk to the network without holding up the thread that noticed the timer was due. The price is that two callbacks due at the same instant have no order between them, exactly as in Go.

It answers `NULL` when the allocator or the timer heap would not give out memory, and then nothing has been armed and nothing will run. Go cannot fail here because Go throws instead, and a library does not get to make that choice for the program using it.

It has to be called from a goroutine, because the timer goes into the heap of the P the caller is on.

## Stopping and moving one

```c
bool was_waiting = time_timer_stop(t);
```

`Stop`. True means the timer was still waiting and now is not. False means it had already fired, or had already been stopped.

False does not mean the callback has finished. It does not even mean the callback has started, because a stop that loses the race by a nanosecond still says false while the goroutine is still being put together. Go has exactly this and the answer is the same in both: a stop does not synchronise with the callback, so anything the callback touches needs a lock of its own.

```c
bool pending;
if (!time_timer_reset(t, 5 * TIME_SECOND, &pending))
    return err_no_memory;
```

`Reset`, and it arms the timer for `d` from now whether or not it was running. `pending` may be `NULL`, and when it is not it is set to whether the timer was still waiting, which is what Go's `Reset` returns. That answer is out here rather than in the return value because arming a timer that had already fired can need the P's heap to grow, and a heap that will not grow is a failure this has to report. A false return means the timer is not armed and will not run.

Go's advice about `Reset` applies unchanged. Resetting a timer whose callback is already running does not unrun it, and a program that needs to know which of the two happened has to say so itself, with a flag under a lock the callback takes as well.

A stop costs the timer's own lock and nothing else. The timer is left where it is and marked, and the P that owns the heap throws it out next time it walks one, so a connection that sets and clears a read deadline on every read never touches a heap at all. That is the case this design is for.

## Giving the memory back

```c
time_timer_free(t);
```

Go hands a `*Timer` to the collector and forgets about it. There is no collector here, so a timer is one of the objects [design/05-memory.md](../design/05-memory.md) calls out as having a lifetime, and it gets the constructor and destructor pair that go with one.

`time_timer_free` stops the timer and takes it out of whatever heap it is in before it frees the memory. That second part is why this cannot be a plain `mem_free`: a stopped timer is still sitting in a P's heap until that P gets round to throwing it out, and freeing the memory under it would leave the scheduler pointing at a hole. Taking it out costs a walk of that one heap, which is nothing on a timer that has already fired and a few hundred nanoseconds on a P holding a thousand of them.

`NULL` is fine and does nothing, so the usual C cleanup shape works.

What it does not do is wait for a callback that is already running, for the reason in the section above. The callback has its own goroutine and does not touch the timer, so the timer is fine, but anything else that goroutine is holding is your problem.

An arena user can skip all of this. `arena_free` takes the whole region at once and the timers in it go with everything else. [guides/allocators.md](allocators.md) has the rest.

## Fake time

Inside a [synctest](synctest.md) bubble, everything on this page runs on the bubble's clock rather than the machine's.

That clock starts at midnight UTC on 1 January 2000 and moves only when every goroutine in the bubble is durably blocked, and then it jumps straight to the next timer that is due. So a sleep of an hour in a bubble costs microseconds, an `AfterFunc` armed for a day fires on the next line, and a context deadline thirty seconds out is something a test can wait for rather than something it has to work around.

```c
static void body(void *env) {
    int64_t start = burrow_nanotime();

    time_sleep(TIME_HOUR);

    // An hour later on the bubble's clock, and no time at all on yours.
    assert(burrow_nanotime() - start == TIME_HOUR);
}

synctest_run(BURROW_FN(Func, body, NULL));
```

Nothing on this page had to be told about bubbles for that to work, and neither did `burrow/context.h`. Every timer in the program is armed through one function in the runtime that picks the caller's timer set, and inside a bubble that is the bubble's own set. [guides/synctest.md](synctest.md) has the rules, including the two that catch people out.

## What it costs

A sleeping goroutine is a stack and a timer. The timer is 64 bytes and lives in the heap of the P that armed it, so arming one is a push onto a four way heap under that P's own lock and not a global one. That is the reason this scales: a server that sets a deadline per request has every core pushing onto a different heap, and Go moved off a single global heap in 1.9 after measuring the version that did not.

A thread with nothing to run reads two published words per P to work out how long it can sleep, rather than taking every P's lock, and then sleeps on the monotonic clock until the earliest of them. So an idle program with a timer due in an hour is an idle program, not a program waking up to check.

[design/06-runtime.md](../design/06-runtime.md) has the rest of it, including the three state bits a timer carries and why there are two published minimums rather than one.

## What Go has that this does not, yet

`time.After`, `time.Tick`, `time.NewTimer` and `time.NewTicker` all hand back a channel, and channels are the next thing being built. When they land these arrive with them, and `TimeTimer` grows an accessor for its channel rather than changing shape, so nothing written against this header has to be rewritten.

Repeating timers are in the runtime underneath already, since `burrow__timer_reset` takes a period. Nothing up here uses it until `Ticker` exists.

The calendar half is the larger piece and none of it is here: `Time`, `time.Now`, parsing, formatting, `Location` and the timezone database.

## See also

- [guides/goroutines.md](goroutines.md) for `runtime_main`, `go` and the scheduler these sit on
- [guides/functions.md](functions.md) for `Func`, `BURROW_FN` and where a callback's environment lives
- [guides/allocators.md](allocators.md) for what to pass as `Alloc *` and when the free can be skipped
- [guides/synctest.md](synctest.md) for the bubble's clock and what a test gets out of it
- [design/06-runtime.md](../design/06-runtime.md) for the timer heaps themselves
