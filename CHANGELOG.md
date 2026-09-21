# Changelog

Every release gets a section here and the release workflow refuses to publish a tag that does not have one, so this file cannot fall behind.

Versions are `0.MINOR.PATCH` until 1.0. The minor number goes up when a milestone finishes and the patch number goes up for everything in between. Nothing before 1.0 is a stable API and everything before 1.0 is published as a prerelease, because none of it has been through a security review.

## Unreleased

### Runtime

- `burrow/defer.h` is Go's `defer`, as a block: `BURROW_SCOPE`, your braces, `BURROW_SCOPE_END`, and `BURROW_DEFER(fn, arg)` inside. The deferred calls run when control leaves the block whichever way it leaves, including a `return` from the middle, and they run last in first out with the argument read at the line the defer was written on. `BURROW_DEFER_FUNC` takes a `Func` you already have.
- The block is required and a defer outside one does not compile. The design document had planned two forms, a bare `BURROW_DEFER` on the GCC and Clang `cleanup` attribute and a portable block for everything else, and the problem with offering both is that the bare one compiles on MSVC and does nothing at all, because there is no attribute there to reject it. A feature that is silently missing on one of three supported platforms is worse than one that costs a line on all three.
- Underneath it is the `cleanup` attribute on GCC and Clang and `__try`/`__finally` on MSVC. A scope is one struct in the frame that opened it and the deferred calls live in it, eight of them with nothing allocated, which is the number Go's compiler open-codes into a frame. A scope that goes past eight takes one allocation that doubles as it fills and is freed before the scope returns, which is the trade Go makes for the defers it cannot open-code into a frame. It held four until burrow-bench priced the first call past the end at about a hundred nanoseconds, since paying means a malloc and a free, against sixty four bytes of frame for the four slots that closed the gap and no time at all, since the array is never initialised.
- The chain of open scopes is a field of the goroutine rather than a thread local, because a goroutine that parks inside a scope can wake up on another thread, and a chain left behind on the first thread is a chain of calls that never run. A thread that is not a goroutine gets a thread local instead, so the macros work before `runtime_main` is called and after it returns.
- The unit is a scope and not a function, which is the one deliberate difference from Go. Wrap a whole function body and Go's rule is back. Leave the scope inside a loop and every turn runs its own cleanup, which is the thing people writing `defer` in a Go `for` body wanted and did not get, and is a mistake common enough that `go vet` has a check for its shape.
- The calls live in the scope rather than one record per defer in the frame that wrote it, which is the cheaper layout and is wrong. Those records sit between the caller's braces, the calls run after that block has ended, and a C object's lifetime ends with the block that declares it, so the scope reads storage the compiler may have reused by then. The address sanitizer calls that a stack use after scope and it found this before the code was a day old.
- A defer in a loop whose scope is outside the loop piles up and runs at the end of the scope, which is Go's behaviour for a defer in a loop, and is almost never what anybody wanted. `BURROW_DEFER` is an ordinary statement too, so a defer as the unbraced body of an `if` or a `for` is fine.
- `runtime_goexit` runs the deferred calls of every scope the goroutine is inside, innermost first, before the goroutine ends. That is Go's rule for `Goexit`, and the chain is re-read after each call so a deferred call may defer things of its own.

### Docs

- `docs/guides/defer.md` is the page, including what a scope costs and why the block is not optional.
- `docs/design/06-runtime.md` section 6 describes the `defer` that exists rather than the two that were planned, and keeps `panic` and `recover` as a plan.
- The README has a defer section, and the goroutines and failure pages say what changed for them.

## v0.0.15 (2026-09-21)

`select`, which is the last piece of the channel work and the thing that makes a goroutine able to wait on more than one conversation at once. `chan_select` because POSIX has owned the short name since 1983.

The other half of this release is a thread sanitizer that works. The `tsan` job in CI had been red since before there was a runtime for it to have an opinion about, and it was not reporting races, it was dying inside the sanitizer, because a goroutine that parks on one thread and resumes on another leaves the call stack the sanitizer keeps per thread in a state it has no way to understand. Telling it about the switch is a few lines. Working out that it had to be told, and which of two compiler attributes each compiler actually honours, was most of a day. What it bought is real: the two bugs below it found afterwards are a use after free in shutdown and a test that had been lying about a race it did not have.

### Runtime

- `chan_select` is Go's `select`. A `SelectCase` per arm, built with `BURROW_RECV`, `BURROW_RECV_OK`, `BURROW_SEND` or `BURROW_DEFAULT`, and the index of the arm that ran comes back. No default arm means it waits, a default arm means it never waits, and when more than one arm is ready the one that runs is picked uniformly at random, so a busy channel cannot starve a quiet one. An arm on a NULL channel never fires, which is how a loop turns an arm off when that channel closes.
- Up to sixteen arms need nothing but the caller's stack frame. Past that there is one allocation, from the allocator of the first channel in the list, and it is freed before the call returns whichever way the call goes.
- The locks are taken in channel address order and repeated channels are locked once, so two selects listing the same channels in different orders cannot deadlock. The arms are sorted by the address of their channel to get that order, with a heap sort, which is Go's choice and is made here for Go's reason: the case list belongs to the caller and a caller is allowed sixty five thousand arms, so a sort that goes quadratic on a list already in the wrong order is a sort that can be handed one.
- One compare and swap arbitrates a select. Every waiting select is one record with a claim flag and one queue entry per arm, and a sender, a receiver or a close that wants one of those entries has to take the flag first. Whoever takes it records which arm won. That is Go's design and it is the reason a select cannot be woken twice.
- A parked select does not hand its stack over until it says so. Go's park callback walks a list of pooled records, but burrow's lives in the parking goroutine's own frame, and the scheduler marks a goroutine waiting before it runs that callback. So the moment the callback unlocked the first channel, a claim could start the goroutine on another thread while the callback was still reading the case list out from under it. A state word swapped by both sides closes that window: whoever finds it untouched is the one that has to wait. A thread sanitizer found this, on Linux, under load, and it is written up in `src/runtime/chan.c`.
- A thread sanitizer build now gets told that a goroutine switched, which it did not before, and the whole test suite passes under one for the first time. The sanitizer adds a push to the front of every function it compiles and a matching pop to the end, and it counts them per thread, so a goroutine that parks on one thread and carries on from another pushes on the first and pops on the second. A few thousand parks of that and one side walks off the end of a fixed size buffer and the process dies inside the sanitizer, which is what `chan`, `select`, `context`, `stack`, `sched` and `time` had all been doing. The switch points already existed for the address sanitizer, so the fix is the fiber API at the same two places.
- The fibers are pooled, and that is the interesting half. The sanitizer's identifier space is one way: destroying a fiber does not give the slot back, and the eight thousand one hundred and ninety third one a process asks for takes the process down with a SEGV that points at nothing. A pool turns the number that matters into the most goroutines alive at once rather than the number ever started, and a reuse cap stops the frames a dead goroutine leaves behind from piling up on a fiber that outlives it. There is a budget too, so a build that really does want that many at once says so in English rather than crashing.
- `BURROW_NO_TSAN` in `burrow/platform.h` leaves one function out of that sanitizer, the one that does the switch, because a function whose push is counted against the old goroutine and whose pop is counted against the new one is the exact bookkeeping error being fixed. It has to be spelled differently for each compiler: gcc takes `no_sanitize("thread")` and clang ignores it for this purpose and wants `disable_sanitizer_instrumentation`, which gcc in turn rejects outright. That was measured by counting the calls in the object file, not assumed.
- `BURROW_TSAN` in `burrow/platform.h` answers whether this is a thread sanitizer build, in the two spellings the compilers disagree on, next to the `BURROW_ASAN` that already did the same job.
- The channel and select tests run the same number of rounds under a sanitizer as they do without one, which they could not before. Twenty thousand ping pong rounds under a thread sanitizer costs five seconds.
- Shutting the runtime down waits for any thread from outside that is part way through a call into it. `sched_ready` from a thread that is not a goroutine is a supported thing to do, it is what a callback from a C library needs, and the goroutine it readies may be the one `runtime_main` is waiting for. So the run can end while that call is still going: main returns, every thread is joined, and the gates they were sleeping on are freed, while the thread that set all of it off is still a few instructions from opening one. A count of the calls in flight, and a wait for it to reach zero before anything is freed, closes that. A thread sanitizer found it in the sysmon test, about one run in sixty.
- The two crossed selects test waits for the goroutine it started instead of assuming it was given a turn. The last round of that test hands the value over and leaves the other goroutine runnable, and the goroutine that got the value has three instructions left before it returns and brings the runtime down with it, so about one run in a hundred on Linux read the child's answer before the child had written it. Nothing was wrong with select. The test now ends with a send the parent receives, which is the way a Go program would have said it in the first place.
- Two clang-tidy complaints in the channel code, both cosmetic: a cast to the type the argument already had, and a nested `if` that reads better as one condition and a comment.
- Select is faster than Go's on every row burrow-bench has for it, which it was not when it landed. The first version took the locks by walking the case list for the lowest address above the last one it locked, which is quadratic, and shuffled the poll order with a modulo, which is a hardware divide per arm. On an EPYC, pinned, minimum of nine runs: a select over eight arms went from 544 nanoseconds to 323 against Go's 416, a select with a default and nothing ready went from 113 to 91 against Go's 78, and the two arm ping pong went from 642 to 575 against Go's 990. Sorting the arms once and using the sorted list for both the lock and the unlock is most of that, and the multiply and shift that replaced the modulo is the rest.

## v0.0.14 (2026-09-19)

Channels. The thing goroutines exist in order to talk to, and the reason the scheduler had `sched_park` and `sched_ready` in it before there was anything to use them.

It is Go's algorithm, which is more specific than a queue with blocking. An unbuffered send is a rendezvous with a direct handoff, so the value goes from the sender's variable into the receiver's without passing through the channel, and by the time `chan_send` returns somebody else has it. A buffered send takes the buffer only when nobody is waiting on it. A sender that has to block holds out a pointer to its value rather than a copy, so a large struct crossing a full channel is still copied exactly once. Closing is a broadcast. The rules that stop the program are Go's exactly, because each of them means two parts of a program disagree about who owns the channel and there is no return value that would fix that.

`chan_select` is not in this release. It needs a way to wait on several channels at once, a uniform random choice among the ready cases, and Go's lock ordering by channel address, and it is the next thing. In the meantime `chan_try_send` and `chan_try_recv` are the select with a default arm that most code actually writes.

### Runtime

- `burrow/chan.h` is Go's channel: `chan_make`, `chan_send`, `chan_recv`, `chan_try_send`, `chan_try_recv`, `chan_close`, `chan_len`, `chan_cap`, `chan_elem`, and `chan_free` because there is no collector.
- One lock per channel, which Go has tried to improve on more than once and has not. The critical section is a handful of pointer writes and one element copy, and waiters are made runnable after the lock is released, which is what `burrow/lock.h` requires of anything that holds it.
- The record of a blocked goroutine lives on that goroutine's stack, which is Go's `sudog` without the pool. A parked goroutine's stack is not going anywhere and the one thing a pool buys is not needed when the record is already free.
- A thread that is not running a goroutine can block on a channel. Go cannot be in that situation and so its waiter only ever holds a `g`; ours holds a goroutine or a note and the wakeup path picks. It costs the thread, which a goroutine parking does not, and that is the only difference a caller can see.
- Values are copied through the type descriptor rather than with `memcpy`, so a channel of `Str` copies a `Str` the way that type says to, and a close zeroes a waiting receiver's variable the same way.
- The wait queues carry an atomic length so that the non-blocking paths can ask whether anybody is waiting without taking the lock. Go reads the queue head there unsynchronised and gets away with it because its race detector does not look at the runtime, and ours is an ordinary library that a thread sanitizer looks at all of.
- `chan_send` returns nothing and `chan_try_send` has no `ok` out-parameter, which is a change from the sketch in the design document. A send has one failure and it is a panic, so a `bool` would only have offered a caller the chance to ignore it.
- `burrow__note_init_transient` is new, and it is there because putting a waiter on the stack found a real bug in notes. A wake opens the gate first, since opening it last would let a sleeper arrive in between and never be seen, and then it has a few instructions left to run on a note whose sleeper is already awake. For a note inside an M that is nothing. For a note in the frame the sleeper just returned from, the wake is reading memory that is gone, and on the backend with a mutex in the note it is about to lock one that has been destroyed. A transient note counts the wakes inside it and a free waits for that count to reach nought.
- Only transient notes count, because counting is not free. Two atomic additions on an otherwise untouched cache line took `note_cycle` from 11 to 23 nanoseconds pinned on an EPYC, which is not a price the scheduler should pay on every park, so the note says at init time which kind it is.
- A thread on its way to sleep now takes its last look for work after joining the idle list rather than before it, which fixes a deadlock. The old order left a gap: a goroutine readied in it went onto the queue, the wakeup that followed found an idle P but an empty idle list, and was not allowed to make a new thread because there were already as many threads as there are Ps, so it put the P back and left, and then the thread that was going to sleep went to sleep. Every thread idle, one runnable goroutine, nothing to start it. Go never reaches this because Go makes another thread rather than giving up. Taking the look under the scheduler lock with the thread already on the list means the sleeper and the waker cannot both miss each other.
- A host thread and a goroutine handing a value back and forth twenty thousand times is a test now. The old channel tests did the same thing fifty times, which found this deadlock roughly once in a working day on one machine and never on the others. Twenty thousand rounds found it nine runs out of thirty on a laptop.

### Docs

- `docs/guides/channels.md` is the page, including which of buffered and unbuffered you want and why that is a design question rather than a performance one.
- `docs/design/06-runtime.md` section 5 now describes the channels that exist instead of the ones that were planned, and keeps the `select` half as a plan.
- The README has a channels section.

## v0.0.13 (2026-09-19)

The monitor thread, which Go calls sysmon. One thread that holds no P and therefore gets to look around while every other thread is busy, which is the only reason it exists.

Go gives it four jobs and burrow can do one of them today. Retaking a P from a thread stuck in a syscall needs syscalls that release a P and there are none yet. Preemption is its own milestone. A forced collection needs a collector. What is left is timers, and the honest description of that job is a backstop: a thread going idle already sleeps with the earliest deadline anywhere as its own deadline, and a thread out of work already runs a victim's due timers on the last stealing pass, so the monitor is what notices when neither of those has happened. All it does about it is call `wakep`, which is the call a goroutine becoming runnable makes, so being slightly too eager costs a thread a wakeup and cannot cost anybody a wrong answer.

An idle program stays idle. The interval starts at 20 microseconds and doubles after fifty quiet passes up to 10 milliseconds, and below the bottom of that there is a state with every P idle and no timer anywhere where the monitor sleeps with no deadline at all, because nothing inside a runtime in that state can change it.

### Runtime

- `sysmon` starts in `runtime_main` once the world is up and is woken and joined on the way down. A run where the thread cannot be started carries on without it, because every job it does today is a backstop for a path that already works.
- The wake out of the indefinite sleep lives in `pidle_get`, since a P leaving the idle list is the only event that makes the world worth watching again.
- That decision and that wake both happen under the scheduler lock. The monitor reads the idle count and then says it is asleep, `pidle_get` changes the idle count and then reads whether the monitor is asleep, and without a lock across both pairs those stores can sit in store buffers long enough for each side to see the old value and for the wake to be lost.
- No deadlock detector, deliberately. Go's `checkdead` runs in this spot and throws when there is a runnable goroutine and nothing to run it. burrow is a library inside somebody else's program and that program's own threads are allowed to ready a parked goroutine, so the same check would throw on a program that is working. If it arrives it arrives as something the program asks for.

### Docs

- `docs/design/06-runtime.md` has a section on the monitor, saying which of Go's four jobs are done and which are waiting on parts that do not exist yet.
- `docs/guides/goroutines.md` mentions the extra thread, because it is one more than `runtime_gomaxprocs` would have you count and somebody is going to see it in a debugger.

## v0.0.12 (2026-09-19)

Timers, both halves. The machine underneath is a set per P with a four way heap in each, which is Go's design down to the number of children per node, and on top of it sit the three calls a program actually makes: `time_sleep`, `time_after_func`, and the stop and reset that go with a timer once you have one.

A stop marks the timer rather than pulling it out of the heap, which sounds like a shortcut and is the whole point. A connection that moves its read deadline on every read touches the heap once and never again, and the benchmark says so: resetting and stopping a timer costs the same whether there is one other timer in the heap or a thousand.

Writing the tests for this found a scheduler hang that had nothing to do with timers and everything to do with deadlines existing at all. A thread going idle put itself on the idle list and then waited on its note, and a note is allowed to be open with nobody waiting on it, so a wake could land before the sleeper reached the wait and be lost. Go never sees this because in Go a thread is only ever woken while being handed a P. The idle list is the state now and the note is the nudge.

### Time

- `burrow/time.h` is the half of Go's `time` package that needs a scheduler under it: `Duration` with its units, `time_sleep`, and `time_after_func` with the stop and reset that go with it. The calendar half, `Time` and formatting and timezones, does not need the runtime and is a separate job.
- `time_sleep` parks the goroutine and arms a timer, so the thread goes and finds other work. A program with a hundred thousand goroutines on a hundred thousand deadlines is asleep in the kernel using nothing.
- The timer is armed from inside the park callback rather than before the park, because arming it first lets it fire on another thread and ready a goroutine that is still running on this one, which is two threads on one stack.
- `time_after_func` runs the callback on a goroutine of its own, which is Go's rule. The callback gets a fresh stack and may block, take locks or sleep again without holding up the thread that noticed the timer was due.
- `time_timer_free` is the half Go does not have, because Go has a collector. It stops the timer and takes it out of whatever heap it is sitting in before freeing it, since a stopped timer stays in its P's heap until that P throws it out and freeing underneath that leaves the scheduler pointing at a hole. Arena users can ignore it.
- `time_after_func` and `time_timer_reset` answer failure rather than panicking when the P's heap will not grow. Go throws there and a C library does not get to make that choice for the program using it.

### Runtime

- A scheduler hang, found by the first test that made threads park with a deadline on them. `stopm` only took the thread off the idle list when the deadline had passed, and a thread woken any other way stayed on a list that is a promise to be asleep. It then put itself on again, the list pointed at itself, shutdown never finished and an idle thread was handed two Ps. The same path threw its deadline away and slept forever while a timer sat overdue on an idle P.
- The cause is that a note is allowed to be open with nobody waiting on it. A thread whose deadline runs out returns from the sleep, a wake meant for it lands before the clear, and the next park ends the moment it starts. Go never sees this because in Go a thread is only ever woken while being handed a P, so the note and the handover cannot disagree. Deadlines are what make them able to. `stopm` now reads the idle lists under the lock and treats the note as a hint about when to stop sleeping and nothing else.
- `burrow/timer.h` and `src/runtime/timer.c` are the timers, which is Go's `runtime/time.go`. A set per P, a four way heap inside each one, and a call the scheduler makes to run whatever is due. Nothing user facing sits on top of them yet, so this is the machine underneath `time.Sleep` and `time.AfterFunc` rather than either of those.
- The set is per P because the alternative is every goroutine that sets a deadline taking one lock, and a server that sets and clears a read deadline per request does nothing else all day. Go moved off a single global heap in 1.9 for the same reason.
- The heap has four children per node rather than two. Same logarithm, a third fewer levels, and the four entries a node compares against sit in one or two cache lines. Go's number.
- Stopping a timer does not take it out of the heap, because the heap belongs to a P that another thread may be holding. A stop marks the timer and the owning P throws it out later, which means a timer stopped and started again before the owner looks never leaves the heap at all. That is the case a read deadline on a busy connection hits every time.
- Two published minimums per set, one for the head of the heap and one for the timers whose recorded time has gone stale, so a thread working out how long to sleep reads two words rather than taking every P's lock. Together they are allowed to be earlier than the truth and never later, because early costs a wakeup and late is a timer that does not fire.
- Arming a timer answers false when the heap needed to grow and the allocator said no. Go cannot fail here because Go throws instead. burrow does not have that option and says so rather than losing the timer quietly.
- The scheduler pays for all this in three places: this P's timers at the top of find runnable, a victim's on the last stealing pass, and a deadline on the sleep a thread takes when it has run out of places to look. The deadline comes from a scan of every P's wake time, and the scan happens after the thread has stopped counting itself as a searcher, which is the same ordering argument as the note's sleeper count.
- The runtime's own lock moved out of `burrow/sched.h` into `burrow/lock.h`. The timers need it and do not otherwise need the scheduler, and a header that includes the scheduler to get a lock is a cycle waiting for the first file that goes the other way.

### Tests

- `tests/timer_test.c`. Most of it drives a set of timers with no scheduler near it and a clock the test makes up, so a timer due at 5000 not firing at 1000 is a check rather than a hope, and the checks can look at the heap directly and confirm that a stop left the timer in it. Two tests go through the real scheduler with the real clock, because arithmetic being right is not the same as a sleeping thread waking up.
- One of those two found a real bug before it was ever committed. Find runnable took its reading of the clock once and then looped, so a thread that slept until a timer was due woke up, compared the timer against the time from before it went to sleep, decided nothing was due and went round again, forever. The reading is taken once per pass now, which is where Go takes it.
- `tests/time_test.c` is the user facing calls on the real scheduler and the real clock. It is what found the idle list bug above, and it found it by hanging rather than by failing, which took a watchdog and a dump of every thread's state to get to the bottom of.
- One test in it started out asserting the order three sleepers came back in, and that is scheduling order rather than timer order, so it failed on a four core virtual machine and deserved to. It measures each sleeper's own wait now. The heap's ordering is tested in `tests/timer_test.c` on a clock the test drives, where the answer does not depend on anything else the machine is doing.
- Both contended run queue tests now wait for every thief thread to be in its steal loop before the owner starts. Starting a thread is a request, and on a machine with as many busy threads as cores it can take longer to be granted than the owner's whole loop takes to run, which left the check that says at least one steal succeeded failing for a thread that was never given a core rather than for a broken steal.
- The check itself was also wrong, which the barrier only made rarer. A thief backs off on purpose before it touches a running victim's `runnext`, because the goroutine in that slot is the one the victim is about to run, so on a round where the owner does nothing but yield the owner is supposed to win. Asking afterwards whether any steal succeeded was asking the scheduler to be worse at its job, and it failed four times in about 186 runs on a four core box under a load average of sixty.
- It counts the rounds where the owner's own get came back empty instead, which on this queue can only mean a thief took the goroutine, and it keeps going for up to two seconds until it sees one. That is a race free count read from the owner's side while the test is running, rather than a plain word read after the join when it is too late to wait. Five hundred runs on an idle six core machine, three hundred on the loaded four core one and two hundred on Windows, with nothing red.

### Docs

- `docs/design/06-runtime.md` has a section on timers: why the sets are per P, what the three state bits are for, why there are two published minimums, what it costs the scheduler, and what Go has here that burrow does not have yet.
- `docs/guides/time.md` is the guide for the user facing calls: durations and their units, sleeping, running a function later, stopping and moving a timer, why the free exists and when it can be skipped, and what it all costs.
- `docs/guides/goroutines.md` no longer lists timers among the things that are missing, and the README has a section on sleeping and timers.

## v0.0.11 (2026-09-19)

The monotonic clock and the timed sleep that goes on top of it, which is the first half of timers. Both are the ones Go has, `nanotime` and `notetsleep`, and the second one waits on the first rather than on the wall clock, so nothing anybody does to the system time can make a timeout fire early or late.

The note itself got faster while that was going in. A wake used to go into the kernel every time, including when nobody was waiting, which is the common case in a scheduler. Notes count their sleepers now and a wake with nobody in the count stays in user space, which took the uncontended cycle from 352 nanoseconds to 11 on an EPYC, against 15 for the `sync.WaitGroup` it is measured against.

v0.0.10 has a tag and no release. Its publish step failed on a Windows warning that was already there, and by the time that was fixed this release was the one worth cutting.

### Runtime

- `burrow/clock.h` is the monotonic clock, one function, `burrow__nanotime`. It is `nanotime` from Go's runtime and it is the same four system calls underneath: `CLOCK_MONOTONIC` on Linux and the BSDs, `mach_absolute_time` on macOS, and `QueryPerformanceCounter` on Windows. C has nothing that does this. `clock` counts processor time, `time` has a resolution of a second, and `timespec_get(TIME_UTC)` is the wall clock, which moves when somebody sets the date and moves backwards when ntp corrects a drift, and a timer built on a clock that can go backwards fires twice or never.
- Two of the three platforms need a conversion factor the machine fixes at boot, and both cache it in 32 bit halves published through a release store rather than in one 64 bit word. A 64 bit atomic on a 32 bit machine goes through burrow's spin lock table, and taking a lock to read a clock would be a strange thing to do to the call every timer is about to be built on.
- `burrow__note_sleep_timeout` is the timed sleep the note header has been promising since notes landed. It waits on the monotonic clock, so nothing anybody does to the system time makes it return early or late, and a timeout of zero or less is a poll. This is Go's `notetsleep`, and what wants it is a thread with no work and a timer due in a millisecond.
- The portable backend of that sleep is two backends, because a condition variable measures its deadline on the wall clock unless it is told otherwise. Every POSIX system since 2001 is told otherwise with `pthread_condattr_setclock`, and macOS is the one that never implemented that call and has `pthread_cond_timedwait_relative_np` instead. Go splits this the same way for the same reason.
- The Linux backend writes out the timespec that goes with the futex system call number it picked rather than using the one from `<time.h>`. `SYS_futex` wants the kernel's old timespec, whose fields are both a `long`, and a 32 bit build with `_TIME_BITS=64` has a libc timespec with an eight byte seconds field. Handing the second to the first is a struct the kernel reads the wrong way, on the one platform nobody builds for.
- All three backends recompute what is left of the timeout each time round rather than waiting the full amount again, so a sleep interrupted nine times still waits the length it was asked for. Each individual wait is capped at a thousand seconds, which costs one extra system call every quarter of an hour and makes every conversion out of nanoseconds something that cannot overflow.
- The timed sleep reads the note before it reads the clock. The caller is a thread that has just failed to find work and is parking with a deadline on it, and by then the wake it was racing has usually already landed, so the common path returns without asking what time it is and without touching the mutex on the portable backend. The Windows backend gets there differently: a wait on an open event returns straight away, so the first wait is the check and there is nothing to add in front of it.
- A note counts the threads that are about to sleep on it, in a word next to the open flag, and a wake with nobody in that count stays in user space. On Linux that is a futex call not made, on Windows a `SetEvent` not made, and on the portable backend a mutex not taken and a broadcast not shouted into an empty room. Pinned on an EPYC, opening a gate nobody was standing at and walking through it went from 352 nanoseconds to 11, against 15 for the `sync.WaitGroup` that burrow-bench measures it against. The timed version of the same thing went from 297 to 10. Go's runtime does not do this for its own notes and Go's `sync.WaitGroup` does exactly this, and a scheduler parks and unparks threads often enough that it matters.
- The Linux backend of `burrow__note_init` set the gate and forgot the count next to it, so every wake on a fresh note read memory nobody had written. Caught by the memory sanitiser after the change had been through macOS, Windows, four compilers, both other sanitiser sets, musl, i386, s390x and aarch64 without a word. The behaviour was right by accident on all of them, because stack memory is nearly always zero and a count of rubbish is still a count that is not zero.
- Nothing is given up for that count. A sleeper joins the count and then reads the flag, a waker sets the flag and then reads the count, and all four of those are sequentially consistent, so every thread agrees on one order for them and in any such order the two writes cannot both come after the two reads. One of the two always sees the other, and the sleeper that loses the race finds the gate open and does not wait at all.
- The count is a second word rather than spare bits in the flag. A futex compares the word it is given against the value the caller expected and returns rather than sleeping when they differ, so a shared word would mean every thread arriving at the gate woke every thread already asleep there, and a crowd of sixty four would wake each other a few thousand times on the way in for nothing.
- `burrow__note_is_open` on Windows no longer asks the kernel. It was a wait of zero milliseconds on the event, which is the documented way to read an event's state and is also a system call to read one bit. It reads the word now.

### Build and CI

- MSVC counts `_Alignas` on a member as warning C4324 and `/WX` turns that into an error, which had the Windows job red. The padding it is warning about is deliberate: `P` puts its run queue head and tail on separate cache lines so a thief and an owner do not fight over one. gcc and clang do not mention it.
- clang-tidy counts `_POSIX_C_SOURCE` as a reserved identifier, which it is, the same way `_XOPEN_SOURCE` already named in that list is. Being reserved is the point of a feature macro and there is no other spelling.

### Tests

- The four thread test for the clock checks cross thread agreement by reading the highest published reading before taking its own, rather than by comparing one thread's first reading against another's last. The second version assumes the threads overlap, and on Linux, where the clock reads through the vdso, twenty thousand readings are done before the fourth thread exists.

## v0.0.10 (2026-09-19)

The scheduler. G, M and P, the three run queues, work stealing, and the outside of it all in `burrow/proc.h`, so a program can call `go` and mean it. There is no timer, no netpoller and no preemption yet, so a goroutine that neither blocks nor yields still holds its thread, which is the same bug Go had before asynchronous preemption and is the next few changes here. AddressSanitizer also learned that a thread can change stacks, which it had been quietly wrong about since the context switch landed.

### Scheduler

- `burrow/sched.h` has the three structures Go's scheduler is made of. G is a goroutine, M is an operating system thread, P is a scheduling context that owns a run queue. The names are Go's and are kept letter for letter, because anybody who has read `runtime/proc.go` should be able to read this and anybody who has not should be able to go and read that.
- `src/runtime/runq.c` has the queues. A runnable goroutine is in exactly one of three places: the `runnext` slot, which holds the one goroutine that was just made runnable and is about to be handed over to, the per P ring of 256, or the global queue for everything that overflows. The order is what keeps a channel handoff on one core and in one cache.
- The ring is lock free, single producer at the tail and multiple consumer at the head. Adding work to your own queue is a load and a store with no atomic read modify write anywhere in it, which is the whole reason for the shape. The release store to the tail is what publishes a slot and the acquire load of it is what makes the contents visible.
- The ring slots are relaxed atomics rather than plain pointers, which is the one place this differs from Go. The indices guarantee that the slot the owner writes is never the slot a thief reads at that instant, but a thief can be descheduled between working out which slots it wants and reading them, and the owner can go all the way round the ring in that time. The thief's compare and swap then fails and the value is thrown away, so nothing goes wrong, but the accesses really do overlap and in C that is undefined behaviour rather than a stale read. ThreadSanitizer found it. Relaxed costs nothing on any architecture burrow targets, since all the ordering lives on the tail and the head.
- Work stealing takes half of a victim's queue, rounded up so that a queue holding one goroutine gives that one up instead of nothing. The goroutine the thief returns is the last of the batch and never becomes visible in the thief's own ring, so a steal does not push and pop anything for no reason.
- A thief takes the victim's `runnext` only on the last pass of a search that has already failed everywhere else, because that goroutine is the one the victim is about to run and taking it is exactly the cache miss the slot exists to avoid. Go sleeps three microseconds before doing it and this yields instead, since there is no monotonic sleep here yet. That becomes the sleep when timers land.
- `burrow__runq_put` hands back the goroutine that overflowed rather than leaving the caller to work it out, because when the put asked for the `runnext` slot the one that overflows is whatever came out of that slot and not the one that went in. Getting that backwards loses a goroutine and runs another one twice.
- The tests run the goroutines standing still, which is the only way to find out whether stealing takes the half it says it takes. Two of them run three thieves against an owner and check that every goroutine came out exactly once, which a queue that duplicates one and drops another passes on counting alone and fails here.
- Checked on macOS arm64, on Linux glibc and musl on both amd64 and arm64, on 32 bit x86, on eight real x86 cores with gcc 13 and clang 18, under AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer and MemorySanitizer, and on Windows with mingw gcc 16.

### Goroutines

- `burrow/proc.h` is the outside of the scheduler and the first header in burrow that a program uses rather than reads about. `go` starts a goroutine, `runtime_gosched` gives the processor away, `runtime_goexit` ends a goroutine, `runtime_gomaxprocs` says how many run at once, and `sched_park` with `sched_ready` are what everything that blocks will be built on. There is a guide at `docs/guides/goroutines.md`.
- `runtime_main` is the one call here that Go does not make you write. Go's runtime starts before your `main` because the toolchain arranges it, there is no toolchain here, so something has to say where the goroutine world begins and ends. It creates the Ps, starts a thread for each, runs the function you hand it as the first goroutine, and joins every thread before it returns.
- `src/runtime/sched.c` is Go's scheduler from `runtime/proc.go`, with the same search order in `findrunnable`, the same spinning thread accounting, the same random start and random coprime stride when stealing, and the same rule that a victim's `runnext` is only taken on the last pass. A thread that finds nothing gives its P up and then looks again, which is the order that stops a wakeup from being lost, and the reason is written out where it happens.
- `mcall` is the one thing spelled differently from Go. Go's version tail calls into `execute` from the goroutine's own stack and never comes back. C has no tail call, so the call on the scheduler stack returns the goroutine to run next instead, or NULL for go and find one. That also gives `park0` somewhere to put the case where the unlock callback answers false and the same goroutine has to run again, which a recursive `execute` would turn into unbounded stack growth on the scheduler's stack.
- Goroutine stacks are a fixed size decided once and never moved. Go grows a stack by copying it somewhere bigger, which needs a map of where every pointer into it lives, which Go's compiler emits and nothing can do for C. The default is a quarter of a megabyte, the mapping is lazy everywhere burrow runs, so a goroutine that touches one page costs one page, and `go_stack` takes a size for the ones that recurse.
- `go` answers whether the goroutine started, which Go's does not. There is exactly one way for it to fail, which is that a stack could not be mapped, and Go's answer to that is to end the program. Returning it is the better answer for a library, because a server that cannot start one more connection handler can still serve the connections it has.
- Dead goroutines are kept rather than freed, on a free list per P with a central overflow, which is Go's `gfget` and `gfput`. A goroutine whose stack is big enough is handed out again with that stack still mapped, so a program that starts and stops goroutines in a loop maps memory once rather than every time.
- `burrow__lock` is the thing Go calls a mutex in `runtime2.go`, which is not what a program means by one. It spins and then yields, and it becomes a futex when `sync` lands. It is what the scheduler's own lock is made of and it is not for anybody else.
- The Ps and Ms are fixed arrays sized by `BURROW_MAXPROCS`, which defaults to 256, so the scheduler has no allocator underneath it and cannot fail to start. Redefine the macro for a machine with more cores than that.
- Two fields in the scheduler are mirrored into atomics because they are written under the lock and read without it. The length of the global queue and the number of idle Ps are both looked at on the fast path of `findrunnable`, and reading them unsynchronised is a data race in C whatever the hardware does about it. ThreadSanitizer agreed.
- `tests/sched_test.c` runs the goroutines rather than standing them still, which is the opposite of `tests/runq_test.c` and the reason both exist. The single threaded ones pin exact behaviour, including that the newest goroutine runs first because `go` puts it in the `runnext` slot, and that a dead goroutine's structure is handed out again. The threaded ones check that four goroutines that never yield really do end up on four threads, and that sixteen goroutines parking and readying each other two hundred times each lose nothing.
- A P's status is written with an atomic store because `runq.c` reads it with an acquire load, when a thief is deciding whether the victim is running and so whether its `runnext` slot is worth reaching for. The write is under the scheduler lock and the read is not, so a plain store there is a data race in C whatever the hardware does with it. ThreadSanitizer on x86 found it and the arm64 run did not, which is the usual shape of this.
- Checked on macOS arm64, on Linux glibc and musl on both amd64 and arm64, on 32 bit x86, on eight real x86 cores with gcc 13 and clang 18, under AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer and MemorySanitizer, with the portable ucontext backend forced on, and on Windows with mingw gcc 16 where all 26 test binaries pass on Fibers.

### Sanitizers and the stack switch

- AddressSanitizer is now told when a thread changes stacks. Every switch calls `__sanitizer_start_switch_fiber` on the way out and the matching finish on the way in, so the sanitizer's idea of where the thread is follows the thread instead of staying on the stack it started on. What this fixed in practice is a warning on Linux that said `ASan is ignoring requested __asan_handle_no_return` with a stack size of minus four terabytes, which was ASan measuring the thread's real stack against a goroutine's stack pointer.
- A context now carries the bounds of its stack in a sanitizer build, because the call has to be handed them before the switch rather than after. A thread's own context is the one case where nobody knows those bounds, and asking the operating system is a different question on every platform, so instead the first switch away from a thread fills them in: the finish call on the far side answers with the bounds of the stack that was just left.
- ThreadSanitizer is deliberately not told, and there is a paragraph in `burrow/context.h` arguing for it. Its fiber API allocates a whole thread state per fiber, with a shadow stack and a trace buffer each, and the implementation holds a few hundred of those before it starts reclaiming slots by stopping the world. A test program that starts about five hundred goroutines slows to nothing and then stops answering. Until that gets cheaper, ThreadSanitizer sees one shadow stack per thread with goroutines interleaved on it, which makes some traces odd to read and has not stopped it finding real races.
- `burrow__context_switch` is now a small inline wrapper in the header around `burrow__context_switch_raw`, which is the assembly, or the swapcontext, or the fiber switch. The annotations go in the wrapper. With no sanitizer on it is the same one call into the assembly that it was before.
- Checked on macOS arm64 with both the assembly and the ucontext backends, on Linux glibc and musl on amd64 and arm64, on 32 bit x86, under AddressSanitizer with fake stacks turned on, UndefinedBehaviorSanitizer, ThreadSanitizer and MemorySanitizer, and on Windows with mingw gcc 16, where the annotations compile out and the Fibers backend is unchanged.

## v0.0.9 (2026-09-19)

Where a goroutine stack comes from. One mapping with an unreadable page underneath it, and a handler that turns a landing on that page into `fatal error: stack overflow` instead of a segmentation fault with nothing to go on. It is the last piece the scheduler needs before there is something to schedule.

### Stacks

- `burrow/stack.h` is where a goroutine stack comes from. Map one of a given size with unreadable pages underneath it, hand `lo` and the size to `burrow__context_make`, free it when nothing is running on it. `burrow/context.h` has always taken a buffer from the caller and never cared where it came from, and this is the caller it was waiting for.
- The guard and the usable part are one mapping, so they are next to each other by construction rather than because two mappings happened to land that way. On POSIX the guard is `mprotect`ed to `PROT_NONE` and on Windows it is reserved and never committed, which is the same fault for no commit charge.
- The struct is `lo`, `hi` and the guard size. Nothing in it is derived from anything else, so there is nothing that can disagree with itself, and all zeroes means both a stack that was never allocated and one that has been freed. That is what makes freeing twice do nothing rather than unmap somebody else's memory.
- Sizes are rounded up to a whole page and raised to 16 kilobytes if they are below it, which is the same floor `burrow/context.h` has. The page size is asked for at runtime because it is 4096 on most things, 16384 on arm64 macOS and 65536 on some arm64 Linux kernels, and a guessed constant would waste most of a stack on one and fail to align on another.
- A fault on a guard page becomes `fatal error: stack overflow`, which is what Go prints. That takes a SIGSEGV and SIGBUS handler on an alternate signal stack, or a vectored exception handler on Windows, because a handler for a stack overflow cannot run on the stack that overflowed.
- The handler claims a fault only when the address is inside the guard of the stack the faulting thread said it was on, which a thread says by calling `burrow__stack_set_current` and the scheduler will say on every context switch. Everything else is passed on to whatever handler was there before, or has the old disposition put back so the fault happens again, so burrow linked into a program with its own SIGSEGV handler takes nothing away from it and an ordinary segmentation fault still produces a core file.
- Two things the guard does not catch and both are in the header rather than left to be discovered. A single stack frame larger than a page can step over the guard, which Go avoids with a bounds check in every function prologue and which a library has no way to reach for, though `-fstack-clash-protection` closes it for code built with that flag. And on Windows the handler runs on the stack that faulted, which is the one with no room left, so it is best effort there until the Win64 assembly lands and lets Windows' own guard machinery do the job.
- Arming is a three state compare and swap rather than a flag, because a second thread that arrives while the first is halfway through installing the handler has to wait for it rather than be told yes. Being told yes and then overflowing would mean no message, which is the thing this all exists to prevent.
- The signal stack size comes from `sysconf(_SC_SIGSTKSZ)` with a 64 kilobyte floor under it. `MINSIGSTKSZ` is 5120 on glibc, 6144 on musl and 32768 on arm64 macOS, on glibc since 2.34 it is not a constant at all but a call into the dynamic loader, and none of those numbers account for a handler that formats a message and writes it.
- Three feature macros before the first include, which is two more than it should take. `_XOPEN_SOURCE` is what makes `sigaction`, `siginfo_t`, `sigaltstack` and `SA_ONSTACK` visible to a strict C11 build at all. The other two are there because `MAP_ANONYMOUS` is not POSIX and asking a libc for POSIX is also telling it that is all you want, so `_DEFAULT_SOURCE` puts the flag back on glibc and `_DARWIN_C_SOURCE` puts it back on macOS. glibc 2.36 needs that and glibc 2.41 does not, which is the kind of difference that turns into a build failure on whichever machine is a release behind.
- The tests allocate at both ends of the size range and write every byte between `lo` and `hi` in both directions, which is how an off by one page in the mapping would show up. Two of them cause a real fault and come back from it: one pokes the byte below `lo`, the other runs a context that recurses until it runs off the bottom. Both run on a thread of their own, so leaving a handler by `siglongjmp` never leaves the main thread marked as still on its signal stack, and both are skipped under the sanitizers, which install handlers of their own and are entitled to.
- The overflow test is also skipped where a context is a fiber, which today means Windows. A fiber brings its own stack, so the recursion runs off the end of that one and hits the operating system's guard page rather than this library's, and the handler correctly decides the fault is none of its business. There is nothing to test there until Win64 assembly puts a goroutine on a stack from this file. The guard test still runs and covers the vectored handler itself.
- The recursion in that test reads a local after the recursive call rather than adding to the result before it. gcc at `-O2` on arm64 turns `something + f(n + 1)` into a loop with an accumulator and no frames in it, which then never reaches the guard and never returns either, so the first version of the test did not overflow a stack, it hung. There is also a depth limit far past what the stack has room for, so a build where this stops faulting fails the test instead of spinning until somebody kills it.
- Checked on macOS arm64, on Linux glibc 2.36 and 2.41 and musl on both amd64 and arm64, on 32 bit x86, under AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer and MemorySanitizer, with the portable ucontext backend forced on, and on Windows with mingw gcc 16.

## v0.0.8 (2026-09-19)

The bottom of the scheduler. A thread to run work on, a way to put one to sleep and wake it again, and a way to put one stack down and pick another one up. None of it is something a user calls and all of it is what a goroutine is made of, so the next release is the first one where the word means anything.

### Threads

- `burrow/thread.h` starts an OS thread. Start, join, detach, yield, an identity for the calling thread and a processor count, which is all the scheduler needs from the operating system and is deliberately where the file stops. Pthreads everywhere and `_beginthreadex` on Windows.
- There is no mutex and no condition variable in it. A goroutine that blocks has to park the goroutine and free the thread, so a pthread mutex is the wrong tool everywhere above this, and the one place the runtime does have to sleep a thread gets a futex primitive of its own. That is the next piece.
- The handle carries the function and the argument, because a thread entry point has room for one pointer and the library does not allocate behind the caller's back to make room for two. So the handle has to outlive the thread, which is written in the header and is what the tests do.
- A stack size below the platform's minimum is raised to it rather than rejected, since the minimum is a different number on every system and a caller asking for 16 kilobytes means small rather than exactly that. The minimum comes from `sysconf(_SC_THREAD_STACK_MIN)` rather than from `PTHREAD_STACK_MIN`, because glibc hides that macro unless a feature macro is set and a strict C11 build does not set one, so the constant it was falling back to was right on amd64 and 8 times too small on arm64, where every thread with an explicit stack size failed to start. The size is also rounded up to a whole page, which macOS requires and nothing else minds.
- `burrow__thread_ncpu` is the number of processors that exist, which under a cpuset or a container cpu limit is not the number this process may use. Getting that right belongs with GOMAXPROCS, which is where a caller can override it anyway.
- `tests/atomic_concurrent_test.c` is the half of the atomics tests that could not be written before there were threads. Eight threads against counters with a known total, a bitmask where each thread owns one bit, compare and swap loops in both the strong and the weak form, and a published pointer that is checked for tearing. Every answer is known in advance, so a lost update fails it rather than merely being unlikely.
- That file is compiled twice, the second time with the lock table forced on, which is the only way the 64 bit spin locks are run under real contention on a 64 bit machine.
- Checked under ThreadSanitizer, AddressSanitizer, UndefinedBehaviorSanitizer and MemorySanitizer on Linux, on 32 bit x86 in Docker where the lock table is the real path rather than a forced one, and on Windows with mingw gcc 16.

### Notes

- `burrow/note.h` is the one place in burrow where a thread genuinely blocks in the kernel. Everything above it parks a goroutine and hands the thread to somebody else, and this is what is underneath when there is nobody left to hand it to.
- A note is a one shot gate. It starts closed, a wake opens it and releases everybody waiting, and a sleep on one that is already open returns straight away. That last part is the whole reason it works without a lock around it, because the waker never has to know whether the sleeper got there first.
- The name is Go's. `runtime.note` is the same object with the same operations, so somebody reading Go's scheduler next to this one finds the same word for the same thing.
- Linux gets a futex, which is one 32 bit word of the caller's own memory and no kernel object at all until a thread actually has to sleep. The constants are written out rather than taken from a kernel header, because they are part of the system call interface and cannot change, and `syscall` is declared in the file because both glibc and musl hide it behind `_GNU_SOURCE` and burrow is built as strict C11.
- Windows gets a manual reset event, which is a one shot gate under another name and is what Go uses there. `WaitOnAddress` would be the closer match to a futex and is not used, because it lives in `synchronization.lib` and burrow still links nothing.
- Everything else gets a mutex and a condition variable, broadcast rather than signalled, since a note releases every sleeper and not one of them.
- The open flag is read and written atomically even on the path where every write already happens under a mutex. That is what lets `burrow__note_is_open` answer without taking the lock, which is what a caller that must not block needs.
- No timed sleep yet. A timeout wants a monotonic clock that does not move when the system time is set, C11 has none, and the runtime has to grow one for timers anyway. The timed version arrives with it.
- The tests are written so that a sleep which does not sleep fails them: the sleeper sets a flag going in and another coming out, and the checker looks at the second one while it still has to be clear. There is also a wake that lands before the thread exists, eight sleepers released by one wake, sixteen rounds of clear and reuse, and a thousand volleys of two threads passing a turn back and forth, which is what the scheduler will actually do with these.
- Checked on macOS arm64, on Linux x86_64 with gcc and clang, under ThreadSanitizer and AddressSanitizer, on 32 bit x86 in Docker where the futex is the time32 one, and on Windows with mingw gcc 16.

### Context switching

- `burrow/context.h` puts one stack down and picks another one up, which is the piece every goroutine in this library eventually sits on. Attach a thread, make a context on a stack, switch, free. Nothing in it allocates: the caller owns the stack, passes it in, and keeps it alive until the context is done with.
- Hand written assembly for SysV amd64 and for AArch64. Each one saves exactly what its ABI says survives a call, which is six registers plus mxcsr and the x87 control word on amd64, and ten plus the frame pointer, the link register, eight double registers and fpcr on arm64. None of it goes in the context struct, it goes on the stack being switched away from, and the struct holds the one stack pointer that finds it again.
- The frame layout lives entirely inside each `.S` file, because the code that builds a fresh frame and the code that pops it are the two halves that must agree, and they can only disagree if somebody edits one of them and not the other. The C side knows that `sp` is at offset zero and nothing else.
- Both files carry `.cfi_*` directives through every push and pop, so a debugger or a profiler can walk a goroutine stack. The entry stub says the return address is undefined, which is how a walk stops at the bottom of a stack that was built by hand rather than reading whatever the buffer held before.
- Windows is on Fibers. Win64 is a different ABI with more registers to save and a thread information block whose stack bounds have to be updated on every switch or the guard page machinery misfires, and a fiber is the operating system doing that for us. One difference comes with it and is written in the header: a fiber allocates its own stack, so the buffer you pass is not the memory it runs on. A real buffer is still required, so the contract is one sentence everywhere.
- Everything else gets ucontext. That is 32 bit x86, RISC-V, PPC64 and anything not yet written, and `-DBURROW_PORTABLE_CONTEXT=1` forces it on a machine that would have picked assembly, which is how you find out whether a bug is in the assembly or above it.
- musl has no ucontext at all. Not behind a feature macro the way `getentropy` was, the four functions are simply absent, so the portable path on musl is a `#error` that says what is wrong instead of five undefined references at the link line. This is also why the assembly had to land in the same change as the fallback rather than after it.
- The ucontext backend passes its context through a thread local instead of chopping a pointer into the two ints `makecontext` gives you, which is undefined behaviour with a long tradition behind it. All three backends take the link through one shared function rather than one of them using `uc_link` and the other two not.
- CI runs the suite both ways on Linux and macOS, and checks that the default build really has the assembly in it by looking for a symbol only the assembly defines. A guard that is subtly wrong produces an empty object file, a library that quietly falls back, and a suite that passes.
- The tests pass under AddressSanitizer with fake stacks on and under ThreadSanitizer, though neither is told about the switch yet. Both have calls for that and they are not called, so this is a thing that is missing rather than a thing that is broken, and it is written down in the header so the day it starts mattering nobody has to guess.
- Checked on macOS arm64 with both backends, on Linux amd64 with gcc and clang and both backends, on Alpine musl on amd64 and arm64, on 32 bit x86 in Docker where ucontext is the automatic choice rather than a forced one, and on Windows with mingw where Fibers are.

### Portability

- The runtime seeds itself with `getrandom` on Linux now instead of `getentropy`, which fixes the build on musl. musl declares `getentropy` only under `_BSD_SOURCE` or `_GNU_SOURCE`, neither of which is set in a strict C11 build, and its `<sys/random.h>` does not declare it at all, so the library did not compile on Alpine. `getrandom` is declared with no feature macro by both musl and glibc. macOS and the BSDs keep `getentropy`, which is the portable one there.
- `getrandom` can be interrupted and can return fewer bytes than asked for, which `getentropy` cannot, so the call site grew a retry loop with a bounded attempt count. It also needs a Linux 3.17 kernel, which is 2014.
- CI has a musl job now, because a platform nothing builds on is a platform that breaks again a week later. It is Alpine in a container and it checks that it really is a musl toolchain before it builds, the same way the big endian job checks its byte order.

## v0.0.7 (2026-09-18)

Groundwork. Nothing in this release is a package a user calls, and all of it is what the next ones stand on: the atomics the scheduler needs, an allocator that catches the mistakes the ownership annotations describe, and a check that the annotations and the list of global state are true rather than merely written down.

### Atomics

- `burrow/atomic.h` is the layer everything concurrent in burrow will be built on. Four widths, `u32`, `u64`, `uptr` and `ptr`, and load, store, add, and, or, swap, compare and swap, the weak compare and swap, three fences and a spin hint over each of them. The memory order is part of every name, because the only caller is code that has already thought about which order it wants.
- This is not `sync/atomic`. That is a Go package with a public surface and sequentially consistent semantics and it will be written on top of this one, which is why the names here carry the internal prefix.
- Three backends. The `__atomic` builtins on GCC and Clang, `Interlocked*` and `__iso_volatile_*` on MSVC, and `<stdatomic.h>` through a cast for anything else. The builtins rather than `<stdatomic.h>` where both exist, because they work on ordinary objects, and that is what lets a caller pass a plain `uint32_t *` instead of wrapping every field in a struct.
- MSVC gets the full barrier form of every read modify write, because the acquire and release variants only exist on ARM and a fast path that compiles on one of the two Windows architectures is a bug waiting for a machine to turn up on. The relaxed loads and stores, which are the ones a spin loop runs, still get the cheap intrinsic.
- Compare and swap takes the expected value by pointer and writes back what it actually saw, which is what C11 does and what a retry loop needs, since the loop has to compute its next attempt from the current value.
- 64-bit operations on a 32-bit machine go through a table of spin locks keyed on the address, the way Go does it. The decision is made on pointer width rather than on what the compiler says about compare and swap, because otherwise a 64-bit load on 32-bit x86 compiles into a call into `libatomic` and the library acquires a link-time dependency that only bites on one platform.
- The lock table is compiled everywhere, not only where it is used. It is four kilobytes of bss a 64-bit build never touches, and in exchange `-DBURROW_ATOMIC_FORCE_LOCK64=1` lets the test suite run it on the machines we own.
- `tests/atomic_test.c` is compiled three times: once as itself, once with the lock table forced on, and once with the C11 backend forced on. So the two paths a normal build never takes are still run by every job in the matrix, apart from the C11 one on MSVC, which keeps C11 atomics behind `/experimental:c11atomics` and makes `<stdatomic.h>` a hard error without it. That build is skipped there rather than turning an experimental switch on for a backend MSVC never selects. The tests are single threaded, since there is no thread abstraction yet, and they use full width values with the top bit set because a cast that truncates or sign extends shows up immediately on one thread with the right value in it.
- The design docs said `<stdatomic.h>` on GCC and Clang and a lock table on 32-bit ARM. Both are now corrected to what the code does.

### Memory

- `burrow/mem/track.h` is the tracking allocator. It wraps any other allocator, passes everything through, and remembers what it handed out so that `track_check` can say what you did wrong at the end. Leaks, double frees, wild frees, size mismatches, alignment mismatches and writes after free.
- Faults arrive through a callback as a `TrackEvent` rather than as text, so a test asserts on a field. The event carries the pointer, the size and alignment it was allocated with, the size and alignment the caller claimed, which allocation it was, and a file and line when the call site used `TRACK_HERE`.
- A free poisons the block and holds it rather than passing the free down, which is what makes a write after free detectable without the page tables. `track_set_quarantine` sets how much to hold, one mebibyte by default, and zero turns it off.
- Realloc is always a new block and a copy, never a grow in place, so code that kept the old pointer gets caught the same way any other write after free does.
- A `Track` over an arena reports that it can be reset and treats a reset as giving everything back rather than as a leak per block. A `Track` over the heap reports that it cannot, rather than offering a reset that would quietly do nothing.
- Bookkeeping comes from a separate allocator, the heap by default and `track_set_meta` otherwise, so records outlive a reset of the thing being tracked and do not show up in its statistics.
- Checked under AddressSanitizer, UndefinedBehaviorSanitizer, MemorySanitizer and ThreadSanitizer on Linux, on 32 bit x86 in Docker, and on Windows with mingw gcc 16.
- `docs/guides/allocators.md` has the real API and the reasoning behind the quarantine.

### Ownership annotations

- `burrow/own.h` is where `BURROW_OWNS`, `BURROW_BORROWS`, `BURROW_RETAINS` and the new `BURROW_STATIC` live. It has no dependencies, because `platform.h` and `version.h` have functions to annotate and no business depending on the allocator interface to do it.
- `BURROW_STATIC(ret)` says the result has static storage duration or is nil, so there is nothing to free and nothing it can outlive. That is what `burrow_version`, `kind_name`, `heap_allocator`, `map_key_type` and `slice_nil` actually do, and writing `BURROW_BORROWS(ret)` with the source left off would have been indistinguishable from somebody forgetting to fill it in.
- `tools/check-annotations.sh` runs in `make check`. A declaration whose return type carries a pointer needs an annotation naming `ret`, and every name inside an annotation has to be `ret` or a parameter of that same declaration, which is what catches the annotation that was right until somebody renamed the parameter.
- That check found 26 declarations saying nothing about their result, including `slice_nil`, `slice_at_fast`, the whole `mem_alloc` family, all four allocator accessors and every function in `version.h`. All of them are annotated now.
- `slice_append`, `slice_append_slice`, `slice_append_fast` and `utf8_append_rune` were annotated `BURROW_OWNS(ret)` alone, which was wrong. Append writes into the array it was given when there is capacity and allocates when there is not, so both `BURROW_OWNS(ret)` and `BURROW_BORROWS(ret, s)` are true of the declaration and the caller has to act on both.
- `tests/lifetime_test.c` checks the annotations are true rather than merely present, which needs the memory to exist and so could not be done before the tracking allocator landed. `BURROW_OWNS` has to make the live block count go up, `BURROW_BORROWS` and `BURROW_STATIC` have to leave it alone, and a borrow into a buffer has to land inside that buffer.
- The four macros are `AttributeMacros` in `.clang-format`, so the formatter stops breaking a declaration in the middle of its return type.

### Out of memory

- `mem_set_oom` installs a handler on one allocator, which is told the size and alignment that was refused and answers whether the allocation is worth trying again. True retries it once, false lets the NULL through as before, and a handler is also free not to return at all, by `longjmp` or by ending the process. Once rather than in a loop, because a handler that says true without having freed anything would otherwise spin forever.
- The handler lives on `Alloc` rather than on `AllocVT`, since a vtable is shared by every allocator using it and this is a decision about one allocator. That makes `Alloc` four words instead of two and means the hook works on an allocator somebody else wrote, without that allocator knowing the hook exists. Write `Alloc a = {.vt = &my_vt, .self = &my_state}` and name the fields, or `-Wextra` will point out the two you meant to leave zero.
- The handler fires for a request an allocator refused and for nothing else. A zero sized request is not a failure and neither is an element count that overflows when multiplied by the element size, since no amount of free memory would have satisfied that one.
- `tools/check-alloc.sh` reads `src/` and fails the build on an allocation whose result is dropped, never tested against NULL, or dereferenced before the test. It runs in `make check` and in CI, as does `tools/check-annotations.sh`, which was only in `make check` before this.
- `docs/design/05-memory.md` section 7 and `docs/guides/allocators.md` describe the policy as it now is rather than as it was planned. The one part still outstanding is raising a panic from functions with no error return, which waits on the runtime because there is no panic to raise yet.

### The gc backend

- `burrow/mem/gc.h` is the collecting allocator, which is the Boehm collector behind the same `Alloc` interface as everything else. You get an allocator, you pass it down, and you never free anything, which is Go's ergonomics back for the people who want them.
- It is off unless you ask. `make BOEHM=1` or `cmake -DBURROW_ENABLE_BOEHM=ON` links `-lgc` and turns it on. Every function is present in either build, so code that uses it compiles everywhere and is only skipped at run time.
- Without the collector, `gc_available` answers false and `gc_allocator` returns NULL. It does not fall back to the heap, because that fallback is a program that allocates in a loop, frees nothing, believes something is cleaning up behind it and grows until the machine stops.
- `mem_free` through this allocator does nothing and `mem_can_reset` is false. That is the collector's answer rather than a missing piece: memory goes away when nothing can reach it, and there is no moment at which everything is known to be dead.
- Alignment stricter than a pointer goes through `GC_memalign`, which may hand back a pointer into the middle of the object the collector actually allocated. Those blocks are grown with a fresh allocation and a copy rather than through `GC_realloc`, which is also why nothing here is ever handed to `GC_free`.
- `mem_stats` reports the collector's own numbers. `bytes_live` is the heap minus what it knows to be free, so it includes its own per object overhead, and `blocks` is the collection count. `allocs`, `frees` and `bytes_peak` stay at zero, because a plausible number would be worse than an honest gap.
- `gc_collect` is `runtime.GC`, and like `runtime.GC` it is almost always the wrong thing to call.
- Nothing in burrow uses this backend and no test needs it, so the library still has no required dependency beyond libc. The tests for it run in both builds and were checked against Boehm 8.2 on Debian.

### Global state

- `tools/globals.txt` lists every mutable global in burrow's own source, one line each, with its category and how it is synchronised. `tools/check-globals.sh` runs in `make check` and in CI and fails when the tree and that list disagree.
- It fails in both directions. A global that is not listed fails, which is the case everybody expects, and a line whose global no longer exists fails too, which is the one that matters, because a list that can rot is a list nobody reads.
- What counts as mutable is the C rule rather than a guess. A const object is not mutable, a pointer to const is because the pointer can be repointed, and a pointer that is itself const is not.
- Seven globals in the tree today: `zerobase` and the tracking allocator's tombstone, which are addresses that exist to be unique and are never written; the `heap` and `gc` allocator singletons and the collector's started flag; the runtime's fatal handler; and the thread local generator behind map iteration order.
- Three of those categories were not in `docs/design/03-c-dialect.md` section 5, which said there were exactly seven categories and named a different set. The table now has the markers, the fatal handler and the generator in it, which is the checker doing its job on the first day it existed.

### Corrections

- The note in v0.0.6 about how `utf8_valid_string` reads a machine word said it was built from separate byte loads and shifts. What shipped uses `memcpy`, for the reason now recorded in `docs/design/09-packages-pure.md`.

## v0.0.6 (2026-09-18)

Text. A Go string holds bytes, and this is the release where reading those bytes as UTF-8 arrives, along with the range loop that is how almost everybody does it.

### Runes and UTF-8

- `burrow/utf8.h` is `unicode/utf8`, ported whole from go1.27.1. All sixteen functions, both spellings of each one where Go has two, and the four constants. The size that Go returns as a second value is an out parameter here that may be `NULL`, which is the rule the rest of the library follows.
- Invalid input behaves exactly as it does in Go: `UTF8_RUNE_ERROR` with a width of one byte, so a loop over a corrupt string terminates and never skips a byte that could have started something valid. Overlong encodings, surrogate halves and runes above U+10FFFF are all rejected.
- `str_runes` and `str_next_rune` in `burrow/core.h` are `for i, r := range s`. The index is the byte offset the rune started at, both out parameters are optional, and the iterator's zero value iterates zero times.
- `tests/utf8_test.c` carries Go's tables across, including the thirty six invalid four byte sequences, one per branch of the accept table, and Go's sequencing test that checks the forward loop, the one shot decoder and the backwards decoder all visit the same runes.
- Checked against Go directly as well: 200,000 random byte strings through both implementations produce identical output for every function and for the whole range loop transcript.
- `str_runes` and `str_next_rune` are inline, with only the multi byte path out of line, because Go's range loop is generated by the compiler and a call per rune is the obvious way for a C port to lose. It was losing: summing the runes of a line of accented text measured 270 nanoseconds as a real call against Go's 117, and 80 once it was inline.
- `utf8_valid_string` and `utf8_valid` skip a machine word at a time through a run of ASCII, which is Go's optimisation and most of what the function costs on real input. The word is read with a fixed size `memcpy`, so there is no unaligned load and no dependence on byte order. Go builds the same word out of separate byte loads and shifts instead, and ported literally that is five times slower here, because gcc folds those shifts back into one load only while there is a single word in the expression and gives up as soon as two of them are combined.
- `docs/guides/runes.md` is new.

## v0.0.5 (2026-09-18)

Two of Go's rules that everything else stands on. A function value, which is what a Go closure becomes here, and the arithmetic where Go's answer and C's answer are not the same answer.

### Numbers

- `burrow/num.h`, which covers the operations where C is undefined or where C is defined and disagrees with Go, and nothing else. Unsigned wrapping, float arithmetic, truncating conversions and the bitwise operators are all the same in both languages and stay as the plain operator.
- `int_add`, `int_sub`, `int_mul` and `int_neg` wrap the way Go says signed overflow wraps, for all nine integer families: `int8` to `int64`, `uint8` to `uint64`, and `Int` and `Uint`. Signed overflow is undefined in C, which means an overflow check written as `a + 1 > a` can be folded to `true` and deleted from the binary, so the wrap has to be spelled out.
- `int_div` and `int_mod` stop the program with `runtime error: integer divide by zero` rather than taking a hardware fault with no message in it, and the smallest value divided by `-1` gives the smallest value back with a remainder of zero, which is what Go prints and what the divide instruction faults on.
- `int_shl` and `int_shr` answer a shift of any width. Past the width of the type a left shift is zero and a right shift is `-1` or `0` depending on the sign, where C is undefined and x86 quietly uses the low six bits of the count. A negative count stops the program with `runtime error: negative shift amount`.
- `int_from_float64` and the rest of the `_from_float64` set saturate to the nearest value that fits and give zero for NaN. Go's specification calls the out of range result implementation dependent and the two big architectures really do disagree, so a library has to pick, and this is arm64's answer, wasm's and Rust's.
- `BURROW_ADD`, `BURROW_SUB`, `BURROW_MUL`, `BURROW_NEG`, `BURROW_DIV`, `BURROW_MOD`, `BURROW_SHL` and `BURROW_SHR` select by the type of the first argument for code that does not know which width it has. They stay out of `BURROW_SHORT`, since `ADD` and `SHL` are names other headers already use.
- Every answer in `tests/num_test.c` came from running the equivalent Go program on darwin/arm64 and linux/amd64 rather than from reading the C standard.
- `docs/guides/numbers.md` is new.

### Zero values and multiple results

- `BURROW_ZERO(T)` for the zero value of a type where you need a value rather than an initialiser, and `BURROW_OUT(p, v)` for writing through an out parameter that the caller is allowed to pass as `NULL`.
- The zero value rule is now checked rather than stated. `tests/core_test.c` takes the zero value of every public type and uses it, and it grows by a few lines every time a type lands. C cannot evaluate `str_is_empty` at compile time, so a test is what stands in for a static assertion here.
- `docs/guides/conventions.md` is new, and it is the page that explains both rules once so that no other header has to.

### Function values

- `BURROW_FUNC` and `BURROW_FUNC0` declare a function value type, which is a function pointer and the environment it was made with. Every Go `func` in a signature becomes one of these, and the environment word is what makes a callback with state possible without a global.
- `BURROW_FN` builds one, `BURROW_CALLF` and `BURROW_CALLF0` call one, and `BURROW_FUNC_IS_NIL` is the nil check. With `BURROW_SHORT` those are `FN`, `CALLF` and `CALLF0`.
- `Func` is Go's `func()`, already declared, and it is what `sync.Once.Do`, `time.AfterFunc`, a goroutine and a deferred call will all take.
- The function pointer is the first member, so a zeroed value is nil and a function value in a struct out of an allocator starts out nil the way a Go struct's func fields do.
- The environment goes in first at the call and the target declares the parameter even when it ignores it. Calling a function through a pointer of a different signature is undefined behaviour and traps under wasm or control flow integrity, so nothing here casts the pointer to avoid writing the parameter.
- `docs/guides/functions.md` is new, and section 7 of `docs/design/04-core-types.md` now describes what shipped rather than what was sketched.

## v0.0.4 (2026-09-18)

Interfaces, and the first two things built on them. `io.Reader` and `io.Writer` exist now, with the four functions that need nothing else, so there is a real shape for every reader and writer the rest of the library will grow.

### Interfaces

- The interface value: a vtable pointer and a data pointer, two words, passed by value. Every vtable is a static `const` object per implementing type, so satisfying an interface costs a few words of read only memory and a constructor that is two moves, and calling through one is a load and an indirect call with no lookup.
- `BURROW_CALL` and `BURROW_CALL0` for the call, `BURROW_IFACE_IS_NIL` for the nil check. Two call macros rather than one because C99 needs at least one argument for the ellipsis and the thing that fixes it is C23, and a method with no arguments is far too common to write around.
- Every vtable starts with a `const Type *self_type`, which is what makes `iface_assert`, Go's `v.(T)`, work on a value that has already forgotten its concrete type. A `NULL` there means the type declines to be asserted to, which is what an unexported type gets you in Go.
- A zeroed interface value is nil, so an interface field in a struct out of an allocator starts out nil with nobody writing a line to say so.
- Embedding is by named member rather than by a prefix compatible cast. That is a change from `docs/design/04-core-types.md`, and the reason is in there now: a cast is right for whichever interface is embedded first and quietly wrong for the second, while the address of a member is the same instruction and the compiler checks it.
- `Any`, Go's empty interface, with `BURROW_ANY`, `BURROW_ANY_VAL`, `any_assert`, `any_box` and `any_equal`. Boxing copies through the descriptor and no deeper, which is what assignment does in Go.
- `any_equal` is Go's `==` on two interface values, including the part where comparing two uncomparable values stops the program at run time with Go's message, because the static type on both sides is `any` and neither compiler can see inside.
- `TYPE_ANY` hashes the dynamic type along with the value, so an `Any` holding an `Int` 1 and an `Any` holding an `Int8` 1 are different keys, which is what makes `map[any]T` behave as Go's does.
- `docs/guides/interfaces.md`.

### io

- `IoReader`, `IoWriter`, `IoCloser`, `IoSeeker` and the combinations, which are the first interfaces built on the machinery above and the reason it exists.
- `io_read_full`, `io_read_at_least`, `io_copy` and `io_copy_buffer`, ported line for line from Go's, including the two parts that look wrong and are not. A read that gets everything it asked for is a success even when the reader reported the end in the same call, and a copy that ran to the end of its source is a success rather than a failure carrying `io_eof`.
- The distinction those functions exist for: an input that ends before anything arrives is `io_eof`, and an input that ends halfway through is `io_err_unexpected_eof`. The first means there was no next record and the second means the file is truncated.
- A wrapped `io_eof` is a failure and not a clean end, which is Go comparing with `==` rather than `errors.Is` in both of those loops. A reader that wraps the end has dressed up the one value meaning nothing went wrong, and believing it would turn a truncated download into a complete one.
- `io_copy` returns an `int64_t` rather than an `Int`, which is Go's choice and the right one, because a copy is the one operation here whose result does not fit in a word on a 32 bit machine.
- The sentinels with Go's messages: `io_eof`, `io_err_unexpected_eof`, `io_err_short_write`, `io_err_short_buffer` and `io_err_no_progress`.
- The down conversions, `io_read_writer_as_io_reader` and the rest, since C has no implicit conversion step. There is deliberately no combination to combination conversion, and `include/burrow/io.h` says why.

## v0.0.3 (2026-09-18)

`Error` and `Map`, which are the last two core types that everything else was waiting on, and a hash worth having under the map. Still nothing you can use as a Go standard library.

### Errors

- `Error`: an interface value, a vtable pointer and a data pointer, returned by value, and a zeroed one means nothing went wrong. Succeeding allocates nothing and leaves nothing to free.
- `errors_new`, `errors_is`, `errors_as`, `errors_unwrap`, `errors_join` and `errors_join_v`, ported walk for walk from Go's, including the `Unwrap() []error` trees that `errors.Join` builds. `errors_as` returns the pointer rather than taking one and returning a bool, because in C the pointer is the bool.
- Sentinel errors are static `const` objects in read only memory, so comparing against one is two pointer loads and declaring one is a line the linker resolves.
- `docs/guides/errors.md` and `docs/guides/failure.md`.

### Maps

- `Map`: a Swiss table, which is what Go's map has been since 1.24. Eight slots to a group with eight control bytes in front of them, so asking which of the eight might hold a key is a few instructions on one 64 bit word with no branches and no key comparisons.
- `map_make`, `map_get`, `map_get2`, `map_set`, `map_del`, `map_clear`, `map_len`, `map_free`, `map_iter` and `map_next`, plus `MAP_SET`, `MAP_GET`, `MAP_HAS` and `MAP_DEL` for the call sites where the types are known.
- Iteration order is randomised per iterator, deliberately, because a program that depends on map order is already broken and breaking it on the first run beats breaking it the day somebody adds a key.
- Go's awkward corners come across because they are observable: a nil map reads as empty and writing to one stops the program, a negative zero and a positive zero are one key, and every NaN key is a different key that can never be found again.
- Float and complex descriptors got their own equality and hash, which is what those two rules are made of. Complex compares and hashes componentwise.
- Tombstones are reclaimed by rebuilding at the same size when the live entries would fit, so filling and emptying a map forever, which is what a cache does, reaches a steady state instead of growing without bound.
- `map_set` returns false when the table needed to grow and the allocator refused, and the map is unchanged. Go stops the world instead, and a library that takes the caller's allocator has to hand that decision back.
- One deviation from Go, and it is entry fifteen in `docs/ledger.md`. An insert that grows the table while an iterator is live moves every entry, so `map_next` stops the program rather than producing an arbitrary answer. Go keeps the displaced table alive for the iterator and needs a collector to know when it dies.
- `map_make` takes an allocator and the map keeps it, which is the third carve-out in the allocator rule and the reason `map_free` exists. Both are argued in `docs/design/05-memory.md` §2.
- `docs/guides/maps.md`.

### Runtime

- `runtime_rand64`, xoshiro256++ seeded from the OS, per thread. Every map gets its own hash seed from it, so two maps holding the same keys have different layouts and a program cannot be fed keys that all land in one group.

### Performance

- `type_hash` is a multiply and fold hash now instead of FNV-1a. FNV is one multiply per byte and each multiply waits for the one before it, so an eight byte key was eight multiplies the chip could not overlap, measured at 8.6 ns out of a 36 ns map insert. The replacement is two multiplies for anything up to sixteen bytes and one more per sixteen bytes after that, and the multiplies in the loop are independent. Map operations came down by thirteen to forty two percent, measured pinned with the minimum of thirty runs on Linux x86-64, and the hash on its own went from 5.16 to 2.55 ns for an int key and from 1315 to 96 ns for a kilobyte.
- Nothing about this is observable. The hash is not part of any API, its values were never stable between runs because every map seeds its own, and byte order is deliberately not corrected on big endian for the same reason.
- The quality is tested and not assumed. `type_test` checks that flipping any one bit of a key flips each bit of the hash between a quarter and three quarters of the time, that a thousand int keys and a thousand string keys spread across the groups and the control bytes a map slices them into without a pile up, and that every length from zero to thirty nine hashes to its own number. That last test earned its keep immediately by catching a bug where the length was folded in with an exclusive or and `"ab"` and `"abc"` cancelled it against their last byte and hashed identically.
- New benchmarks in burrow-bench for the hash on its own, five of them, against `hash/maphash`. The map benchmarks are what found the FNV problem but could never say how much of their own time was the hash, so now that question has its own file.

### Build and CI

- The clang-tidy job is green again. It had failed on every branch since it was added, and it went unnoticed because the check doing it, `clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling`, fires against glibc headers and not against the macOS SDK. It asks for the C11 Annex K functions, which glibc has never implemented and says it will not, so it is off with the reason written next to it like every other disabled check.

## v0.0.2 (2026-09-18)

Type descriptors and `Slice`, which are the two things `Map`, `Error`, `fmt` and `encoding/json` were all waiting on. Still nothing you can use as a Go standard library.

### Types

- The type descriptor. A `const Type` in read only memory, one per type, shared by everything that mentions it, so pointing at one costs a word and initialising one costs nothing because the linker did it. `Kind` is numbered exactly the way `reflect.Kind` numbers it, since those numbers are observable through `fmt`.
- `TypeOps`, optional, for the types whose equality is not a `memcmp` of the struct. `Str` is the reason it exists: two `Str` values pointing at different buffers holding the same bytes are one value in Go.
- `type_equal`, `type_hash`, `type_size`, `type_align`, `kind_name`, and the descriptors for every builtin.
- `docs/guides/types.md`.

### Slices

- `Slice`: pointer, length, capacity, and the element's descriptor. Four words where Go has three, and the fourth is what buys one `append` that works for every element type without templates or a macro per type.
- `append` matching Go exactly, including the part people rely on without being able to state it. When the capacity is already there the elements go into the existing backing array and every other slice over that array sees them.
- The growth progression is a port of Go's `nextslicecap`, so a program tuned against Go's allocation count gets the same one here. Go's `roundupsize` step is deliberately not ported, because size classes are a property of Go's allocator and burrow's allocator is whichever one you passed in. That is the one place the capacity numbers differ from Go's, and the table of where is in the guide.
- Nil and empty kept apart, because Go keeps them apart and the difference is visible in JSON output.
- `slice_sub` and `slice_sub3`, bounds checked against `cap` and not `len`, which is Go's rule and is how you get at the spare capacity on purpose.
- `slice_copy`, which handles overlap, because `copy(s, s[1:])` is how you delete an element and Go promises it works.
- Zero sized elements never allocate, so `[]struct{}` costs no memory and grows without calling the allocator.
- `docs/guides/slices.md`.

### Performance

`AT` and `APPEND` are no longer wrappers around `slice_at` and `slice_append`. They expand to an inline fast path that is handed the element size at the call site, which makes the copy a single store and keeps the four word header in registers instead of writing it to the stack on the way into a call and reading it back through a return buffer on the way out.

That is worth fourteen times on x86-64 and not quite twice on arm64, for the same change. A four word struct is over the register passing limit on both ABIs, but on the AMD core tested the callee's four eight byte stores cannot be forwarded to the caller's two sixteen byte loads, so it waited for cache twice per append. Apple's core forwards it.

- 1024 appends into a slice with the capacity in hand: 6779 ns to 3700 ns on an M1, 19044 ns to 1360 ns on an EPYC.
- 64 bounds checked reads: 57.4 ns to 36.0 ns on an M1, 211 ns to 82 ns on an EPYC.

The element size is still checked against the descriptor at runtime, and a wrong size, a nil pointer, a full slice or an index out of range all fall through to the general path, so the bounds checks and the growth arithmetic are written exactly once and passing a `T` of the wrong size is a performance mistake rather than a correctness one.

Go is still ahead on both. 738 ns for the same appends and 17.6 ns for the same reads, because its compiler can often prove the bounds check away and burrow's cannot.

### Headers

`Slice` is in `burrow/slice.h` rather than `burrow/core.h`. A `Slice` points at a `Type` and a `Type` contains a `Str`, so the three have to be declared in that order. `burrow/burrow.h` includes them in the right order and nobody else has to care.

### Fixed

- CI was red on gcc and on the formatter and both failures were invisible on macOS. gcc rejects comparing two arrays with `!=`, and the formatter gate had no pinned version so it failed on a correctly formatted file. clang-format is now pinned to 20.1.7 in CI and named in `CONTRIBUTING.md`.

## v0.0.1 (2026-09-18)

The first tag. Nothing in here is usable as a Go standard library yet, and the point of releasing it is to find out whether the release machinery works while the mistakes are still cheap.

What exists is the bottom of the stack.

### Memory

Every function in burrow that can allocate takes an `Alloc *a` as its first parameter. That rule is the whole memory design and it is settled now rather than later.

- `Alloc`, one vtable, four operations, with zeroing handled at the interface so that no backend author can forget Go's zero value rule and no backend that already knows its memory is clean pays for it twice.
- `arena`, the default. Nesting, and a last allocation unwind that makes append in a loop reallocate in place instead of copying.
- `heap`, `malloc` and `free` behind the same interface. The only file in the tree allowed to call them, which a check enforces.
- `fixed`, over a buffer you hand it, for targets that have no allocator at all.
- `docs/guides/allocators.md`.

`gc` and `track` are not written.

### Core types

- `Str`, a pointer and a length, passed by value, never NUL terminated. Go strings can contain NUL bytes and every `char *` version of this is a truncation waiting for the one input somebody sent on purpose.
- `str_at`, which stops when the index is out of range, because Go's `s[i]` does.
- The numeric types. `Int` follows the pointer width, since Go's `int` does and the overflow behaviour is visible in Go's own tests.
- `docs/guides/strings.md`.

`Slice`, `Map`, `Error` and interfaces are not written.

### Platform

- `platform.h` answers every question a configure script would ask, using macros the compiler already sets. There is no configure step in burrow and there never will be.
- The OS and architecture names are Go's GOOS and GOARCH strings, so a bug report saying linux/arm64 means the same thing in both projects. An unknown platform is a compile error naming the one file that has to change, rather than a guess.

### Runtime

- `runtime_throw` and the two bounds check messages, carrying Go's text byte for byte. These are fatal errors rather than panics, because `recover` needs `defer` and `defer` needs the scheduler. The mechanism changes later, the text does not.
- `runtime_set_fatal_handler`, for the places that have no standard error to print to.
- `docs/guides/failure.md`.

### Build

- Four compilers, seven platforms, five sanitisers, all at `-Werror`.
- `tools/check-banned.sh` and `tools/check-headers.sh`.
- Sixteen CI jobs, green.

The API is not stable and will not be until 1.0. Published as a prerelease, because nothing here has been through a security review.
