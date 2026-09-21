# 06 — The runtime: goroutines, scheduler, channels, panic

Nothing above Tier 0 works until this does. `net/http` is a goroutine per
connection. `os/exec` waits on pipes. `time.After` is a channel. `sync.WaitGroup`
parks. `database/sql` has a connection pool with waiters. `context` cancellation
is a closed channel. Roughly sixty stdlib packages are unimplementable without
a working scheduler, and this is the largest piece of genuinely new engineering
in the project — everything else is a port.

The target is Go's semantics, not a simplification of them: M:N scheduling
across `GOMAXPROCS` cores, work stealing, asynchronous preemption, a netpoller
integrated into the scheduler's idle path, and channels with Go's exact
happens-before guarantees.

## 1. The model

Go's G-M-P, kept verbatim because deviating from it changes observable
behaviour:

- **G** — a goroutine: its stack, its state, its defer chain, its error scope,
  its arena.
- **M** — an OS thread.
- **P** — a scheduling context with a local run queue. `GOMAXPROCS` of them.

```
   ┌──────┐ ┌──────┐ ┌──────┐         global run queue
   │  P0  │ │  P1  │ │  P2  │         ┌───────────────┐
   │ runq │ │ runq │ │ runq │◀───────▶│  G G G G ...  │
   └───┬──┘ └───┬──┘ └───┬──┘         └───────────────┘
       │        │        │  ▲
     ┌─▼─┐    ┌─▼─┐    ┌─▼─┐ └── work stealing
     │ M │    │ M │    │ M │
     └─┬─┘    └─┬─┘    └─┬─┘
       └────────┴────────┴──── netpoller (epoll / kqueue / IOCP / io_uring)
                               sysmon (preemption, retake, timers)
```

`findRunnable` follows Go's search order, which is not arbitrary — it is tuned
for locality and for avoiding a centralised bottleneck: local run queue, then
global run queue, then a non-blocking netpoll, then steal from other Ps' local
queues, then re-check GC workers (n/a) and the global queue if spinning. The
elegant part we copy exactly: **the thread that runs out of work becomes the
thread that sleeps inside `epoll_wait`.** A negative timeout blocks until an
event, zero returns immediately, positive blocks for a duration — so waiting
for goroutines and waiting for I/O are the same wait, with no dedicated poller
thread and no wakeup latency.

One step at the end of that search is not Go's, and it is there because burrow
holds a thread count Go does not. A thread with nothing left to do gives up its
P, takes one more look for work, and parks. Go can afford to take that look
before joining the list of parked threads, because when something readies a
goroutine and finds no parked thread to hand the idle P to, Go simply makes
another thread. burrow will not: there are as many threads as there are Ps and
no more, so the waker has nothing to do but put the P back and leave, and it
does that on the understanding that whichever thread is on its way to sleep will
see the work first. A look taken before joining the list breaks that
understanding, since work can arrive in the gap between the two. So the look is
taken after, under the scheduler lock, with the thread already visible on the
list, which leaves no order in which the sleeper and the waker miss each other.
Getting it wrong is a program where every thread is asleep and one goroutine is
runnable, and it takes tens of thousands of handoffs to see once.

`sysmon` is a dedicated thread that wakes on a timer and looks for the things
no running thread is in a position to notice: a P held by a thread that is
stuck in a syscall, a goroutine that has been on a processor too long, a
netpoller nobody has polled recently, and a timer that has come due on a P with
nothing on it. Go degrades badly without it and so would we. The section on it
below says which of those four burrow does today and why the other three are
waiting on parts that do not exist yet.

An M is an OS thread and `burrow/thread.h` is where one comes from: start,
join, detach, yield, an identity for the calling thread, and a processor count.
Pthreads everywhere and `_beginthreadex` on Windows, which is the one that
gives the new thread its own CRT state where `CreateThread` does not. The
handle carries the entry function and the argument, because a thread entry
point has room for one pointer and nothing in this library allocates behind the
caller's back to make room for two, so the handle has to outlive the thread.

There is no mutex and no condition variable in that header, which is the point
of it. A goroutine that blocks has to park the goroutine and free the thread,
so a `pthread_mutex_t` is the wrong tool at every level above this one, and the
one place the runtime genuinely has to put a thread to sleep gets a futex-style
primitive of its own rather than a general purpose lock. The processor count is
the number of processors that exist, which under a cpuset or a container CPU
limit is not the number this process may use; reconciling those two is
`GOMAXPROCS`'s job and is where a caller can override the answer anyway.

That primitive is `burrow/note.h`, and it keeps Go's name for it. A note is a
one shot gate with six operations: init, free, clear, wake, sleep, and a sleep
with a timeout on it. It starts closed, a wake opens it and releases everybody waiting, and a sleep on an
open one returns straight away. That last part is what makes it usable without a
lock wrapped round it, because the waker never has to know whether the sleeper
has arrived yet. Linux gets a futex, so a note is one 32 bit word of the
caller's own memory and an uncontended note costs no system call at all. Windows
gets a manual reset event, which is a one shot gate under a different name and
is what Go uses there. Everything else gets a mutex and a condition variable,
which is what Go uses on darwin.

All three keep a count of the threads that are about to sleep in a word next to
the open flag, and a wake with nobody in that count does not go near the kernel.
This is one of the few places burrow deliberately does more than Go's runtime
does, and the reason is a measurement rather than a preference: opening a gate
nobody was standing at cost 352 nanoseconds and the `sync.WaitGroup` it was
measured against, which keeps its own waiter count in the word next to its
counter, did the same thing in 17. It costs 11 now, against 15 for the
WaitGroup. A scheduler parks and unparks threads all day, so that is not a
rounding error. Nothing is given up for it. A sleeper joins the count and then
looks at the flag, a waker sets the flag and then looks at the count, and all
four of those are sequentially consistent, so they happen in one order that
every thread agrees on and in that order the sleeper cannot pass the waker. The
count is a second word rather than spare bits in the flag because the flag is
what a futex compares against, and a flag that changed every time somebody
arrived would wake everybody already asleep for nothing.

That ordering has a consequence that took a thread sanitizer to find. The waker
has to set the flag before it reads the count, and setting the flag is what
releases the sleeper, so every instruction of a wake after that store is running
against a sleeper that is already awake. For a note inside an M, which lives as
long as the thread does, that is nothing to think about. For a note in the stack
frame of the thread that just woke up, the frame is gone, and on the backend
with a mutex in the note the wake is about to lock one that has been destroyed.
So a note says at init time which kind it is. `burrow__note_init_transient`
makes one that counts the wakes inside it and whose free waits for that count to
reach nought, and that is what anything blocking a host thread on a stack local
asks for. The default does not count, because two atomic additions on an
otherwise untouched cache line doubled the cost of a wake and a walk through the
gate on an EPYC, and the scheduler's own notes cannot have the problem.

This is the piece that section 6 below calls `sched_park` and `sched_ready`.
Those two are the G-level operations and they park a goroutine, which needs Gs
and a scheduler to park them in. A note is the M-level half underneath, and it
is what `sched_park` will block on once there is nothing left for a thread to
run. It lands first because everything above it needs a thread that can sleep.

The timed sleep is `burrow__note_sleep_timeout`, which is Go's `notetsleep`, and
what wants it is a thread with no work to do and a timer due in a millisecond.
It needed a monotonic clock first, because a timeout measured against the system
time is a timeout that fires twice or never on the day the machine syncs with
ntp, and C11 has no such clock: `clock` counts processor time, `time` has a
resolution of a second, and `timespec_get(TIME_UTC)` is the wall clock. So there
is `burrow/clock.h`, one function, `burrow__nanotime`, and it is `nanotime` from
Go's runtime down to the four system calls underneath it. `CLOCK_MONOTONIC` on
Linux and the BSDs, `mach_absolute_time` on macOS, and `QueryPerformanceCounter`
on Windows. Only the difference between two readings means anything, which is
all a timer or a timeout ever asks for.

The timed sleep is three implementations and one of them is two. Linux passes a
relative timespec to `FUTEX_WAIT`, which the kernel already measures on
`CLOCK_MONOTONIC`. Windows passes a millisecond count to `WaitForSingleObject`.
The portable backend has to ask, because a condition variable measures an
absolute deadline on the wall clock unless it is told otherwise: every POSIX
system since 2001 is told otherwise with `pthread_condattr_setclock`, and macOS
is the exception that never implemented that call and offers
`pthread_cond_timedwait_relative_np` instead. Go makes the same split for the
same reason. All three loop and recompute what is left of the timeout from the
clock each time round, so a sleep that is interrupted nine times still waits the
length it was asked for rather than nine times it.

### Timers

`burrow/timer.h` is Go's `runtime/time.go`, the layer underneath `time.Sleep`,
`time.After`, `time.AfterFunc`, `time.Ticker`, context deadlines and every
network read timeout. None of those exist yet. What exists is the thing they
are all built out of: a set of timers per P, ordered so that the earliest one
is cheap to find, and a call the scheduler makes to run whatever is due.

Per P rather than one global set, because the alternative is every goroutine
that sets a deadline taking one lock, and a program that does nothing but set
and clear deadlines is a normal kind of server. Go started with a single heap
under a single lock and moved to this in 1.9 for exactly that reason. The heap
has four children per node rather than two, which is still logarithmic but a
third shallower, and the four entries a node compares against sit next to each
other in one or two cache lines. Go's number and Go's reason.

The part that shapes the rest is that a timer cannot be taken out of the heap
by whoever stops it. Stopping happens on whatever thread the goroutine was
running on, and the heap belongs to a P that some other thread may be holding,
so a stop marks the timer and walks away. Three bits do the marking: `HEAPED`
says the timer is in some P's heap, `MODIFIED` says `when` has changed since
the heap recorded it and the heap order is a guess until somebody fixes it, and
`ZOMBIE` says it was stopped and is waiting for the owning P to throw it out. A
timer that is stopped and started again before the owner gets round to it never
leaves the heap at all, and that case is a read deadline on a busy connection.

Two published minimums come out of the same fact. A thread deciding how long to
sleep needs to know when the earliest timer anywhere is due, and it cannot take
every P's lock to find out. So the set keeps `min_when_heap`, which is the head
of the heap, and `min_when_modified`, which is a lower bound over the timers
whose recorded time is stale. Whoever moves a timer earlier lowers the second
one without holding the set's lock at all. Together they are allowed to be
earlier than the truth and never later, which is the direction that costs a
thread waking up to find nothing to do rather than a timer that does not fire.

What this costs the scheduler is three things. A check of this P's own timers
at the top of `findrunnable`, which is two atomic loads for a P with no timers.
A check of a victim's on the last stealing pass, for the case where the whole
program is asleep on a timer belonging to a P that has no thread on it. And a
deadline on the sleep a thread takes when it has run out of places to look,
worked out by scanning every P's wake time after the thread has stopped
counting itself as a searcher. That order is the same argument as the note's
sleeper count: both sides are sequentially consistent, so there is no order in
which a timer being set misses a searching thread and the searching thread
misses the timer.

One difference from Go worth naming. Go's heap cannot fail to grow, because Go
throws if the allocation fails. burrow does not have that option and does not
pretend to, so arming a timer answers false when the heap needed to grow and
the allocator said no, and the caller is told rather than left with a timer
that will never fire. What Go has here that this does not, yet: timer channels
and the sequence numbers that go with them, both of which need channels, and
fake time for `testing/synctest`.

Arming a timer takes the earliest time it could fire and hands it to the
scheduler, which wakes a thread on a note and, if a thread is asleep inside the
poller with a later deadline than that, breaks the poll as well. The comparison
matters. A thread polling with a deadline of a second away has to be brought
out for a timer due in a millisecond and must not be brought out for one due in
two seconds, because the second case is a wakeup that finds nothing to do and
goes straight back to sleep, on every timer anybody arms.

### The monitor thread

`sysmon` is one thread started when the runtime comes up and joined when it
goes down, and it holds no P while it runs, which is the whole reason it can do
its job. Everything else in the scheduler only gets to look around while it is
between goroutines. The monitor looks around when nobody else can.

Go gives it five jobs and burrow can do two of them today, which is worth
stating plainly rather than shipping a thread that appears to do five. Retaking
a P from a thread stuck in a syscall needs syscalls that release a P, and there
are none, because there is no file I/O yet. Preemption needs something to
preempt with, and that is its own milestone further down this document. A
forced garbage collection needs a garbage collector. What is left is timers and
the netpoller backstop, and both of those are real now.

The netpoller job is the same shape as the timer one. If nobody has polled for
ten milliseconds and there are goroutines waiting on descriptors, the monitor
takes a look itself and readies whatever is ready. The ordinary path is a
thread that goes idle and sleeps inside the poller, so this only ever fires
when every thread is busy running goroutines and none of them has been round
the idle path recently. Without it a program that is compute bound on every P
would not notice a connection becoming readable until one of those goroutines
finished.

The timer job is a backstop and is written as one. The ordinary path already
covers the common case twice over: a thread about to go idle works out the
earliest wake time across every P and sleeps with that as its deadline, and a
thread that has run out of work runs a victim's due timers on the last stealing
pass. So the monitor is not how timers normally fire. It is what notices that
one of those two has not happened, and all it does about it is call `wakep`,
which is the same call a goroutine becoming runnable makes. A monitor that is
slightly too eager costs a thread a wakeup and cannot cost anybody a wrong
answer, and that asymmetry is what makes a backstop worth having at all.

The interval starts at 20 microseconds and doubles after fifty passes that find
nothing, up to 10 milliseconds. Go's numbers, Go's shape. The point of the
ramp is that a program with work in it gets checked often and a program sitting
idle does not get woken two hundred times a second to be told nothing has
changed.

Below the bottom of that ramp is a state the monitor sleeps in with no deadline
at all, which is every P on the idle list and not one timer anywhere. Nothing
inside a runtime in that state can change it, so a deadline would only be a
promise to wake up and find the same thing. The way out is `pidle_get`, which
is the one place a P leaves the idle list, and it opens the gate on the way
past. That decision and that wake both happen under the scheduler lock, which
is not incidental: the monitor reads the idle count and then says it is asleep,
`pidle_get` changes the idle count and then reads whether the monitor is
asleep, and without the lock those two stores can sit in store buffers long
enough for both sides to see the old value and for the wake to be lost.

A Go program cannot reach that state and still be alive. A burrow program can,
because burrow is a library inside somebody else's program and that program's
own threads are allowed to call `sched_ready` on a parked goroutine. The same
fact is why there is no deadlock detector here yet. Go's `checkdead` runs in
this exact spot and throws, and the same check in burrow would throw on a
program that is working correctly and waiting on a thread the runtime has never
heard of. If it arrives it has to arrive as something the program asks for.

## 2. Context switching

This is where the POSIX-only prior art ([02](02-landscape.md) §3) is
insufficient. `swapcontext` does not exist on Windows, it makes a `sigprocmask`
syscall per switch where it does, and musl does not implement it at all, which
is worth knowing before anybody plans around it: the functions are not hidden
behind a feature macro on musl, they are absent, and a build that reaches for
them fails at the link line. The answer, as in `minicoro`, `libaco` and
Boost.Context: **hand-written per-ABI assembly**, about 40 instructions each.

`burrow/context.h` is the interface and it is deliberately small: attach a
thread, make a context on a stack the caller owns, switch, free. Nothing in it
allocates. Three backends, picked by the machine rather than by a configure
step: assembly on amd64 and arm64 away from Windows, Fibers on Windows, and
ucontext for anything else. `-DBURROW_PORTABLE_CONTEXT=1` forces the fallback,
which is how you find out whether a bug is in the assembly or above it, and CI
builds and runs the suite both ways on every pull request.

`burrow__context_start` is the one piece all three share. Each backend's entry
stub gets as far as having the new stack live and then calls it, so running the
entry function, going to the link when that returns, and failing loudly when
there is no link are written once rather than three times in three languages.
The ucontext backend passes the context through a thread local rather than
through `makecontext`'s pair of ints, because splitting a pointer into two ints
and putting it back is undefined behaviour with a long tradition behind it.

| ABI | Registers to save | State |
| --- | --- | --- |
| SysV AMD64 | rbx, rbp, r12–r15, rsp, rip, mxcsr, x87 CW | done, `context_amd64.S` |
| AArch64 AAPCS | x19–x30, sp, d8–d15, fpcr | done, `context_arm64.S`. PAC and BTI are not used yet and belong with the hardening pass |
| Win64 | above + rsi, rdi, xmm6–xmm15, TIB stack limits | Fibers for now. Must update `NT_TIB` stack bounds or Windows guard pages fire |
| RISC-V | s0–s11, sp, ra, fs0–fs11 | ucontext |
| PPC64 ELFv2, s390x | per ABI | ucontext |
| wasm | — | no switching; see §9 |

Two details that are easy to get wrong and expensive to discover late:

- **Windows requires updating the TIB's `StackBase`/`StackLimit`** on every
  switch. Omit it and Windows' stack guard page mechanism misfires on the
  second goroutine, usually as a mysterious access violation deep in a callee.
  This is most of why Windows is on Fibers rather than assembly: a fiber is the
  operating system doing that bookkeeping for us.
- **Unwinding metadata.** Each goroutine stack needs correct CFI/SEH so that
  debuggers, profilers and `runtime.Stack()` can walk it. Both `.S` files carry
  `.cfi_*` directives through every push and pop, and the entry stub declares
  the return address undefined so a walk stops at the bottom of the goroutine
  stack rather than reading whatever the buffer held before. Win64 also needs a
  registered dynamic function table, which comes with the Win64 assembly.

- **Telling the sanitizers.** A sanitizer that is not told about a stack switch
  believes the thread is still on the stack it was on before, and from then on
  everything it says is about the wrong memory. ASan gets
  `__sanitizer_start_switch_fiber` on the way out and its matching finish on the
  way in, and the context carries the bounds of its stack so the call has
  something to pass. TSan gets a fiber per context, created on the way in and
  switched to on the way out, which is what gives each goroutine its own call
  stack in the sanitizer's eyes instead of one per thread with goroutines
  interleaved on it. That is not a nicety. Interleaved, the push the sanitizer
  adds to the front of every function it compiles lands on one goroutine and the
  matching pop lands on another, and a few thousand parks later one of them
  walks off the end of its buffer and the process dies inside the sanitizer.

  The fibers are pooled, because the identifier space is one way: destroying a
  fiber does not give its slot back, and the eight thousand one hundred and
  ninety third one a process asks for takes it down. Pooling makes the number
  that matters the most goroutines alive at once rather than the number ever
  started. `include/burrow/context.h` has the arithmetic and
  `src/runtime/context.c` the code.

Guard pages were on this list and are now in `burrow/stack.h`, which §4
describes.

The performance target is libmill's, which sets the bar for whether Go-style
code feels natural in C: **≥10 M goroutine launches/sec and ≥20 M context
switches/sec per core**. libmill hits ~20 M and ~50 M respectively but is
single-threaded and cooperative; we accept a constant-factor loss for M:N and
preemption and must stay within one binary order of magnitude.

## 3. Goroutines

```c
void       go(void (*fn)(void *), void *arg);        /* fire and forget    */
Goroutine *go_handle(void (*fn)(void *), void *arg); /* with a join handle */
Goroutine *go_with_stack_size(size_t n, void (*fn)(void *), void *arg);

#define BURROW_GO(call)  /* call sugar: BURROW_GO(worker(x, y)) */
```

Semantics are Go's, deliberately, including the parts libdill objects to:
goroutines are **unstructured**, there is no join, a goroutine outliving its
creator is legal, and cancellation is by `context`, by convention, not by
force. → [02](02-landscape.md) §3

Two additions that cost nothing and address libdill's real point:

```c
Bundle *b = bundle_new(a);
bundle_go(b, worker, arg);
Error err = bundle_wait(b, timeout);   /* joins, propagates panics */
bundle_cancel(b);                          /* cancels the group's contexts */
```

Bundles are what `burrow`'s own internals use (`net/http`'s per-connection
goroutines, `os/exec`'s pipe pumps, `database/sql`'s pool maintenance), so
leaks in our code are structurally prevented while user code keeps Go's
semantics. And the arena design gives a second, orthogonal answer: a leaked
goroutine leaks its arena, which is bounded and attributable, rather than an
unbounded object graph.

**Goroutine state**, per G: stack, run state, defer chain head, panic chain
head, error scope stack, arena, `context` pointer for diagnostics, profiling
labels, and a `synctest` bubble pointer. All of it in one cache-line-aligned
struct; creation is one arena allocation plus a stack.

## 4. Stacks

Go uses contiguous, copying, growable stacks starting at 2 KB (8 KB min in
recent versions), with the compiler emitting a prologue check at every
function entry. **C has no such prologue, so stack growth by copying is not
available to us** — we cannot find and rewrite the pointers into a moved stack.

This is the second real fidelity compromise in the project (the first being
memory safety, [03](03-c-dialect.md) §4), and the resolution is:

- **Default: fixed-size stacks with a guard page.** 256 KB of reserved address
  space per goroutine, committed lazily by the OS, with an inaccessible guard
  page below. Virtual address space is free on 64-bit; actual RSS is the few
  pages the goroutine touches. Overflow hits the guard page and is converted,
  via a `SIGSEGV`/`SIGBUS` handler on an alternate signal stack (or a Windows
  vectored exception handler), into a `panic("stack overflow")` with a usable
  traceback — which is much better than a silent corruption and is roughly what
  Go reports.
- **Configurable per goroutine**, because `go/parser`, `encoding/json` and
  `regexp/syntax` recursion depths vary: `GoOptions(fn, arg, &(GoOptions){
  .stack_size = 1<<20 })`.
- **Segmented-stack option** (`-DBURROW_SEGMENTED_STACKS=1`) using a split-stack
  runtime where the toolchain supports `-fsplit-stack` (GCC on Linux). Reduces
  per-goroutine reservation to a few KB at a small call-overhead cost. Off by
  default because it is not portable.
- **32-bit platforms** cannot afford 256 KB of reservation per goroutine at
  scale; there the default drops to 64 KB and the documentation states the
  practical goroutine ceiling.

Consequence: **a million idle goroutines costs ~256 GB of reserved address
space and a few hundred MB of RSS on 64-bit.** Reserved address space is not
memory, so this works, but it is a real difference from Go and it is documented
prominently rather than discovered.

All library code obeys the no-VLA, no-`alloca`, bounded-recursion rules from
[03](03-c-dialect.md) §3 precisely so that the default 256 KB is comfortably
sufficient for everything `burrow` itself does.

### What exists

`burrow/stack.h` is the first half of the above: the mapping, the guard and the
conversion of a fault on the guard into `fatal error: stack overflow`. The
per-goroutine size, the segmented option and the 32-bit default arrive with the
scheduler, since there is nothing yet to configure them on.

A stack is one anonymous mapping of the guard and the usable part together, so
they are next to each other by construction rather than by luck, with the guard
`mprotect`ed to `PROT_NONE` on POSIX and left reserved but uncommitted on
Windows. Reserved and uncommitted is the same fault for no commit charge, which
is the cheaper of the two ways to spell it there. The struct is `lo`, `hi` and
the guard size, nothing is derived from anything else, and all zeroes is both a
stack that was never allocated and one that has been freed, which is what makes
a double free do nothing.

The handler is a SIGSEGV and SIGBUS handler with `SA_ONSTACK` and `SA_SIGINFO`,
or a vectored exception handler on Windows. It claims a fault only when the
address is inside the guard of the stack the faulting thread said it was on,
which a thread says through `burrow__stack_set_current` and the scheduler will
say on every switch. Anything else is chained to whatever handler was installed
before, or has the old disposition put back and is allowed to happen again, so
burrow linked into a program with its own handler takes nothing away from it and
an ordinary segmentation fault still produces a core file.

Two things it does not catch, both in the header next to the function so nobody
has to come here to find them. A single frame larger than the guard can step
over it, which is exactly what Go's per-function prologue check prevents and
what a library has no way to reach for. `-fstack-clash-protection` closes it for
code built with that flag. And on Windows the vectored handler runs on the stack
that faulted, which is by definition the one with no room left, so it is best
effort there in a way it is not elsewhere. The Win64 assembly fixes that, because
that is what lets the TIB describe the goroutine stack and lets Windows' own
guard machinery do this job.

## 5. Channels and `select`

Both halves are built. What is still a plan is the `BURROW_SELECT` block at the
end of this section, which is sugar over what exists.

```c
Chan *chan_make(Alloc *a, const Type *elem, Int cap);
void  chan_free(Chan *c);
void  chan_send(Chan *c, const void *v);
bool  chan_recv(Chan *c, void *out);             /* false if closed+drained */
bool  chan_try_send(Chan *c, const void *v);
bool  chan_try_recv(Chan *c, void *out, bool *ok);
void  chan_close(Chan *c);
Int   chan_len(const Chan *c);
Int   chan_cap(const Chan *c);
const Type *chan_elem(const Chan *c);
Int   chan_select(SelectCase *cases, Int n);     /* index of the arm that ran */
```

Two things moved between this list and the sketch it replaces, and both are the
same decision. `chan_send` returns nothing, because a send on a closed channel
is a panic in Go and a `bool` return would have offered a caller the choice of
ignoring it. `chan_try_send` loses its `ok` out-parameter for the same reason:
there is only one thing it can report, which is whether it sent. The `ok` that
matters is on the receiving side, where closed and empty is a real answer and
has to be distinguishable from nothing happened.

Go's exact semantics, which are more specific than "a queue with blocking" and
all of which are test-observable:

- Unbuffered send/receive is a **rendezvous** with a direct hand-off — the
  sender writes into the receiver's destination, no buffer copy. This is
  observable through timing and through `synctest`.
- Buffered channels are a ring buffer with separate sender and receiver wait
  queues, FIFO in wakeup order.
- Send on closed **panics**; receive on closed yields the zero value with
  `ok == false`; close of closed panics; close of nil panics; send/receive on
  nil blocks forever.
- Happens-before: a send happens before the corresponding receive completes; a
  close happens before a receive that returns zero. These are the guarantees
  the Go memory model makes and they dictate the memory ordering on the
  fast paths (acquire/release, not relaxed).

Two places where the implementation differs from Go's, both forced by burrow
being a library rather than a whole program.

A channel is freed. `chan_free` hands the header and the buffer back, which is
one call because they are one allocation, and freeing a channel that goroutines
are still blocked on stops the program rather than leaving a parked goroutine
with a pointer into returned memory.

A thread that is not running a goroutine can block on a channel. Go cannot be
in that situation and so its `sudog` only ever holds a `g`; burrow's waiter
holds either a goroutine or a note, and the wakeup path picks. A goroutine that
blocks costs no thread, a host thread that blocks costs itself, and that is the
only difference a caller can see. The note in that waiter is the reason
`burrow__note_init_transient` exists, and section 1 above says why.

One place where the implementation differs from Go's for a reason that is
ours rather than Go's: the wait queues carry an atomic length. The non-blocking
paths want to know whether anybody is waiting without taking the lock, Go reads
`q.first` there unsynchronised and gets away with it because its race detector
does not instrument the runtime, and ours is an ordinary library that a thread
sanitizer looks at all of. One relaxed store on a path that already holds the
lock buys a clean TSan run.

**`select`** is the interesting one because Go's `select` is a statement and C
has no statement to hook. The implementation is a case array plus a driver, and
the ergonomics are recovered with a macro:

```c
SelectCase cases[] = {
    BURROW_RECV(ch1, &v1),
    BURROW_RECV(ch2, &v2),
    BURROW_SEND(ch3, &v3),
    BURROW_DEFAULT,
};
switch (chan_select(cases, 4)) {
case 0: use(v1); break;
case 1: use(v2); break;
case 2: sent(); break;
case 3: nothing_ready(); break;
}
```

With sugar, for the common shape:

```c
BURROW_SELECT {
    BURROW_CASE_RECV(ch1, Str, msg)  { handle(msg); }
    BURROW_CASE_RECV(done, Unit, _)  { return; }
    BURROW_CASE_AFTER(5 * TIME_SECOND) { timeout(); }
}
```

`BURROW_SELECT` expands to the array-plus-switch form; there is no hidden
allocation and no `setjmp`. It is the one part of this section that is still a
plan, and it is waiting on `time_after`, because a select sugar without a
timeout arm is sugar nobody reaches for.

Fairness matters and is copied exactly: when multiple cases are ready, one is
chosen **uniformly at random**, using the per-P fast RNG, because Go does this
and Go programs (and tests) depend on the absence of starvation. The poll order
and lock order across the case set follow Go's channel-address ordering to
avoid deadlock. The arms are sorted by channel address to get that order, with
a heap sort, and both the locking and the unlocking walk the sorted list and
step over a channel that appears in more than one arm. That is Go's answer and
the heap sort is there for Go's reason, which is that the case list comes from
the caller and the caller is allowed sixty five thousand arms.

An earlier version walked the case list for the lowest address above the last
one it locked, which needed no storage and read better, and was quadratic. That
looked defensible for the three arms a real select has and burrow-bench
disagreed: at eight arms a quarter of the whole call was in the unlock. The
array is sixteen arms before it allocates.

Two deviations from Go are worth naming, and both come from the same place:
burrow keeps the per-arm wait records in the calling goroutine's stack frame,
as it does for a plain send or receive, while Go pools them.

The first is that `chan_select` is where the runtime's only stack-frame
handover lives. `sched_park` marks the goroutine as waiting and then calls the
unlock callback from the scheduler's stack, and that callback walks the case
list, which is in the goroutine's frame. The instant it unlocks the first
channel, a claim on any arm can make the goroutine runnable and another thread
can be executing that frame while the callback is still reading it. So the
select and its claimer swap values into one state word, and whichever of them
finds the word untouched is the one that waits: a claim that lands mid-unlock
records itself and returns without a wakeup, and the unlock finds the record
and skips the sleep. Go needs none of this because a pooled record outlives the
frame. One atomic swap per wait against one allocation per wait is the right
side of that trade.

The second is smaller. Whether a waiter is a goroutine or a host thread is
already a per-waiter question here, and a select adds nothing to it: one record
per select, one queue entry per arm, all pointing at the same sleeper, and the
sleeper knows which kind it is.

## 6. `defer`, `panic`, `recover`

**`defer`** is one implementation and no user choice between them, which is a
change from the plan this section used to hold.

The plan was two: a cheap `BURROW_DEFER` on its own, built on the GCC and Clang
`cleanup` attribute, and a portable `BURROW_SCOPE` block for MSVC and for the
code panic has to be able to unwind. The problem with offering both is that the
cheap one compiles on MSVC and does nothing, because there is no attribute
there to reject it, so the same source builds clean on three platforms and
leaks on one of them. A feature that is silently absent on a supported platform
is worse than a feature that costs a line, so there is one form and it is the
block:

```c
BURROW_SCOPE {
    OsFile *f = os_open(a, path, &err);
    if (BURROW_FAILED(err))
        return err;
    BURROW_DEFER(os_file_close, f);
    ...
}
BURROW_SCOPE_END;
```

`BURROW_DEFER` names the scope variable, so a defer written outside a scope does
not compile, on any of the three compilers rather than on two of them.
Underneath, `BURROW_SCOPE` is the `cleanup` attribute on GCC and Clang and
`__try`/`__finally` on MSVC, and both of those handle a `return` out of the
middle, which is the only property the block needs from the compiler.

The deferred calls live in the scope itself: eight of them in the struct and the
rest in one allocation that doubles as it fills and is freed before the scope
returns, which is the trade Go makes for the defers it cannot open-code into a
frame. Eight is the number Go's compiler stops open-coding at, and it was four
here until burrow-bench priced the fifth call at a hundred nanoseconds against
sixty four bytes of never initialised frame for the four slots that fill it.
The obvious cheaper layout is one record per defer, declared where the defer is
written, and it is wrong for a reason that takes a sanitizer to notice. Those
records sit between the caller's braces, the calls run after that block has
ended, and an object's lifetime in C ends with the block that declares
it, so the scope would be reading storage the compiler is entitled to have
reused. The address sanitizer calls it a stack use after scope and it was right.

The chain of open scopes is a field of the goroutine rather than a thread local,
because a goroutine that parks inside a scope can wake up on another thread and
a chain left behind on the first one is a chain of calls that never run. A
thread that is not a goroutine gets a thread local instead, so the macros work
in a program that uses burrow's allocators and never starts the runtime.

LIFO order and argument evaluation at `defer` time are Go's and are kept. The
scope rather than the function as the unit is not Go's, and it is the better
rule: a `defer` inside a Go `for` body does not run until the function returns,
which is a bug people write often enough that `go vet` has a check for its
shape, and a `BURROW_DEFER` inside a scope inside a loop runs every turn.
Modifying a named result does not arise, because C has no named results, and a
deferred call that wants to change what the function returns takes a pointer to
the variable like any other call.

A defer in a loop whose scope is outside the loop piles up and runs at the end
of the scope, which is exactly Go's behaviour for a defer in a loop, and is what
the storage above buys: a defer is a value the scope appends rather than a
record in the loop body, so the same source line can be reached any number of
times. That also makes `BURROW_DEFER` an ordinary statement, which is worth more
than it sounds, because a defer as the unbraced body of an `if` or a `for` is a
thing people write.

`runtime_goexit` runs every open scope on the goroutine before the goroutine
ends, which is Go's rule for `Goexit`, and re-reads the chain after each call so
that a deferred call is free to defer things of its own.

**`panic`/`recover`** is the other half of this section and it is built. It is
`setjmp`/`longjmp` over the chain of scopes above, scoped to a single
goroutine:

```c
BURROW_NORETURN void panic(Any v);
BURROW_NORETURN void panic_str(Str s);
Any panic_value(void);
Str panic_text(Any v);

BURROW_TRY {
    risky();
}
BURROW_CATCH(p) {
    log_panic(panic_text(p));
}
BURROW_TRY_END;
```

There is no `recover` function, which is the one deviation from Go in the whole
feature and the one thing in this section that is a judgement call rather than
a translation. Go recovers inside a deferred function and then returns normally
from the function that deferred it. Resuming in the frame that recovered means
that frame has somewhere to come back to, which is a `setjmp`, which is a
couple of hundred bytes of `jmp_buf` in the frame, a call, and a compiler that
stops keeping locals in registers across it. Go's rule needs one of those in
every function that has a `defer` in it, and a `defer` that costs a `setjmp` is
a `defer` people stop using. The catch block puts the `setjmp` only where
somebody actually catches something, which in a program of any size is a
handful of places, and leaves `BURROW_SCOPE` at two stores and a call. It also
reads the way a C programmer expects recovery to read.

The rest is Go's:

- A panic runs the deferred calls of every scope between it and the catch
  block, innermost scope first and LIFO inside each scope.
- An unrecovered panic prints the value and ends the process with status 2,
  which is Go's exit status for the same thing. Ending the process is what
  crosses goroutines here. The unwinding does not: a goroutine that panics
  walks its own scopes and finds its own catch blocks, the same as Go, which
  is why the panic state is a field of the `G` next to the defer chain and for
  the same reason.
- A panic inside a deferred call chains onto the one already unwinding rather
  than replacing it, both are printed if neither is caught, and the deferred
  calls beside the one that panicked still run. That last part is why
  `burrow__scope_close` takes each call off the scope before making it and
  leaves the scope on the chain until its calls are done: a panic from inside
  one comes back around, finds the scope still there, and finishes it.
- `panic(nil)` gets a value that says nil was panicked with, which is Go's
  behaviour since 1.21 and for Go's reason, that a catch block receiving nil
  cannot tell it caught anything.
- `panic_value` is what a deferred call asks instead of calling `recover` to
  find out whether it is running because of a panic. It returns the value being
  unwound, or nil during an ordinary scope close.

The value is an `Any`, so it is a type descriptor and a pointer into a frame
the jump is about to leave. A recovery point carries thirty two bytes of
storage and `panic` copies the pointee into it before jumping, which covers
`Str`, `Error`, `Any`, every number and every small struct. Anything larger
keeps pointing where it pointed and has to outlive the jump on its own.
Copying the value is not copying what the value points at in turn, so a
panicked `Str` still borrows its bytes, which is the rule everywhere else in
burrow and is the one thing about this feature that has already caught us out
in a test.

`longjmp` rather than SEH on Windows, on all three compilers and both Windows
toolchains. The by-hand walk over the defer chain is doing the work that an
unwinder would otherwise do, which leaves `RaiseException` with nothing to add
except a second code path to keep in step. On MSVC a `longjmp` runs the
`__finally` blocks of the frames it jumps over, including scopes the panic has
already closed by hand, so closing a scope twice has to be harmless, and it is
for the same reason re-entering one is.

Nothing on the panic path allocates, which matters because running out of
memory is one of the things that will eventually arrive here.

The [03](03-c-dialect.md) §3 restriction, no `longjmp` across allocator or lock
boundaries, holds because the only `longjmp` in the tree is the panic and it
closes the scopes it passes rather than skipping them.
`tools/check-banned.sh` refuses `setjmp`, `longjmp` and their signal variants
anywhere outside `burrow/panic.h`, `src/runtime/panic.c` and one signal handler
test that needs `sigsetjmp` to restore the signal mask, which a panic cannot
do.

The runtime's own checks panic rather than throwing, and what they panic with
is an `Error` whose concrete type is `RuntimeError`, which is Go's
`runtime.Error`. `runtime_error_from` is how a catch block asks, and it gives
back nothing for any other panicked value, so a caller can tell a bug in the
code from a panic the program raised on purpose. `runtime_panic` raises one
with a message of the caller's own, for somebody writing their own container's
bounds check.

Which conditions those are is a lookup and not a judgement. If Go's version of
a condition is a recoverable panic then burrow panics, and if Go throws, or if
the condition only exists because burrow is written in C, burrow throws. That
puts the index and slice bounds checks, the divide by zero, the nil map write,
the send on a closed channel, the double close and the impossible make sizes on
the panic side, and leaves the map that grew under an iterator, the invalid map
key type, running out of memory and every scheduler invariant on the throw
side.

The message is the thing that made this need thought. It is formatted into a
frame the jump lands past, so it cannot live there, and the path cannot
allocate, so it cannot live on the heap either. It lives in a fixed
`BURROW_RUNTIME_ERROR_MAX` slot on the goroutine beside the panic state, which
means it borrows and the next runtime error on the same goroutine writes over
it. The header says so and the tests hold it to that. The `Error` value itself
is sixteen bytes, so the existing copy into the recovery point's storage
carries it.

Still to come in this half: the stack trace the printer should carry waits on
the stack walker in §4.

## 7. `sync`, `sync/atomic`, `context`

**`sync/atomic`** (94 declarations) is a thin sequentially consistent layer over
`burrow/atomic.h`, which is burrow's own and is written first because the
scheduler needs it long before there is a package to expose. That header has
three backends: the `__atomic` builtins on GCC and Clang, `Interlocked*` plus
`__iso_volatile_*` on MSVC, and `<stdatomic.h>` through a cast for anything
else. The builtins rather than `<stdatomic.h>` where both exist, because they
operate on ordinary objects, and that is what allows `sync_atomic_add_int64` to
take an `Int64 *` the way Go's takes an `*int64` instead of demanding a wrapper
type at every call site. Go's `atomic.Int64`, `atomic.Pointer[T]` and
`atomic.Value` still become structs, with the same zero-value-is-usable
property, and `atomic.Value`'s consistent-type requirement is enforced with the
stored type descriptor.

64-bit atomics on a 32-bit machine go through a table of spin locks keyed on the
address, as Go does. The choice is made on pointer width, so it is the same on
32-bit ARM and 32-bit x86, and `-DBURROW_ATOMIC_FORCE_LOCK64=1` takes that path
on a 64-bit machine so the fallback is run by the test suite everywhere rather
than only on hardware nobody has on their desk.

**`sync`** primitives are built on the scheduler, not on pthreads, because a
goroutine blocking on a mutex must park the *goroutine* and free the *thread* —
using `pthread_mutex_t` would block the M and defeat the whole design. So:

| Type | Implementation |
| --- | --- |
| `Mutex` | Futex-style atomic word; spin, then park on the scheduler. Zero value valid. Go's starvation-mode hand-off after 1 ms is reproduced, because it is observable under contention. |
| `RWMutex` | Reader count + writer intent, writer-preferring, as Go's |
| `WaitGroup` | Atomic counter + parked waiter list; `Go` method (Go 1.25+) included |
| `Once` | Atomic state + park; `OnceFunc`/`OnceValue`/`OnceValues` |
| `Cond` | Ticket list on the scheduler |
| `Map` | Go's read-mostly design: atomic read map + dirty map + misses counter. Behaviour under concurrent range is matched. |
| `Pool` | Per-P private/shared slots with victim cache. Without a GC there is no natural drain point, so `burrow`'s `Pool` drains on a `sysmon` tick with Go's two-generation victim policy — same observable effect. |

Park/unpark is a single primitive (`sched_park`, `sched_ready`) which everything else
is built on, and which is where all the memory ordering subtlety lives. One
place to get right, audited under TSan.

**`context`** is ordinary library code once channels exist — `WithCancel`,
`WithTimeout`, `WithDeadline`, `WithValue`, `AfterFunc`, `WithoutCancel`,
`Cause` — and is passed explicitly as `Context *`, exactly as in Go. No
goroutine-local storage, no ambient context; the explicitness that Go's API
already forces is the C-friendly choice.

## 8. The netpoller

A goroutine blocked in `Read` must not hold an OS thread. This is what lets Go
scale to a million connections without a thread pool, and it is the reason
`net/http` is architecturally possible at all.

Two families, and the difference is structural:

- **Readiness** — `epoll` (Linux), `kqueue` (BSD/macOS), `poll` (fallback).
  The kernel says "this fd is readable"; you then choose when and how to read.
- **Completion** — IOCP (Windows), `io_uring` (Linux). You hand the kernel a
  buffer up front and it tells you when the operation finished.

Go's internal poller abstracts over both, and so must ours, with one `pollDesc`
per fd coordinating suspension and resumption. The awkwardness is that
readiness and completion want different buffer ownership: completion requires
committing a buffer before the goroutine is parked, and that buffer must stay
valid — which interacts with arenas. Resolution: the completion backend
allocates from a **pinned per-P buffer pool**, not from the caller's arena, and
copies on completion. One extra copy on Windows, correctness guaranteed, and it
is what Go effectively does too.

| Platform | Primary | Fallback |
| --- | --- | --- |
| Linux | `epoll` + `eventfd`; `io_uring` opt-in at runtime | `poll` |
| macOS, *BSD | `kqueue` | `poll` |
| Windows | IOCP | — |
| illumos/Solaris | event ports | `poll` |
| AIX | `pollset` | `poll` |
| wasip1 | `poll_oneoff` | — |
| Cosmopolitan | `poll` (portable) | — |

`io_uring` is runtime-detected and opt-in, not build-time, consistent with
[03](03-c-dialect.md) §8.

**Regular file I/O** is the honest caveat, and Go has it too: blocking file
syscalls generally occupy an M, with `sysmon` handing the P to another thread
so the program keeps making progress. `burrow` matches this, and additionally
routes file I/O through `io_uring` when it is available and enabled, which is
better than Go.

### What exists

`burrow/netpoll.h` is the interface and it is internal: no program calls it,
`net` will. Underneath it `src/runtime/netpoll.c` is everything that does not
depend on which kernel this is, and one file per backend is everything that
does. `netpoll_epoll.c` covers Linux and `netpoll_kqueue.c` covers macOS and
the BSDs. Windows selects neither, the header answers that the poller is not
initialised, and every call the scheduler would make into it is skipped, so
the runtime on Windows behaves exactly as it did before this landed until the
IOCP backend arrives.

Four calls are the whole surface. `burrow__poll_open` takes a descriptor and
gives back a `PollDesc`, `burrow__poll_wait` parks the calling goroutine until
the descriptor is ready in the direction asked for, `burrow__poll_unblock`
wakes every goroutine on a descriptor and leaves it woken so that a close is
not a race, and `burrow__poll_close` gives the descriptor back. A wait answers
ready, closed, timed out or unpollable, and the third of those is unreachable
until deadlines land.

Registration is edge triggered and happens once, for both directions together,
for as long as the descriptor is open. That is Go's choice and it is why a busy
connection costs no system calls beyond the reads and writes themselves. What
it asks for in exchange is the discipline every edge triggered poller asks for,
which is that a caller reads or writes until the answer is that it would block,
because a caller that stops early has consumed an edge that will not come
again.

A descriptor is two words, one per direction, each of which is empty, ready,
claimed, or a pointer to the goroutine parked on it. That is Go's `pollDesc`
and the reason it works is that the park and the wake meet at a compare and
swap on that word rather than at a lock. Descriptors come from a cache that
never returns memory to the allocator, because a kernel event can name a
descriptor that was closed a moment ago and the layer that receives it has to
be able to look at the memory to find that out. Go calls this type stability.
Here it is blocks of sixty four, and a handle packing a block index and a
generation, so a stale event fails the generation check and is dropped.

The one thing here that is not a port is the wakeup, and it is worth saying why
because both backends are shaped by it. Go can afford to lose a wakeup: a
thread that stays asleep when it should not have is replaced by a new one, and
the program carries on with one thread more than it needed. burrow runs at most
one thread per P and `startm` gives a P back rather than creating a thread, so
a thread asleep in the kernel that nobody can reach is a program that stops. So
every path that would have woken a thread and found nobody to wake breaks the
poll instead, and the break must survive being picked up by a thread it was not
meant for. That rules out the edge triggered wakeup primitives, `EVFILT_USER`
on kqueue in particular, which the kernel forgets the moment anybody receives
it. Both backends use a level triggered wakeup instead, an `eventfd` on Linux
and a self pipe on kqueue, so a poll that was only looking sees the wakeup,
leaves it where it is, and the thread it was for still finds it. The cost is
one spurious return for a thread that was going to sleep anyway.

The other half of that is the idle path. A thread with nothing left to do gives
its P up, then polls with the deadline of the earliest timer, then takes a P
back and goes round the loop again rather than assuming the poll answered its
question. Giving the P up first is what makes a lost wakeup impossible, since
a goroutine readied by anybody at that point finds a P on the idle list and a
thread to hand it to. Going round again rather than parking is what makes a
poll that returned with nothing harmless, and a poll returns with nothing
routinely: a wakeup meant for another thread, a signal, a deadline another
thread got to first. Only when there is no P left to take does the thread fall
through to the ordinary note.

Deadlines are the next piece and they are a timer per descriptor per direction
on top of this, which is what `net.Conn.SetDeadline` needs. After that, IOCP.

## 9. Preemption

Cooperative-only scheduling is what makes libmill and libdill unsuitable as a
Go-fidelity runtime: a compute-bound coroutine holds the CPU forever. Go solved
this in 1.14 with asynchronous preemption — `sysmon` signals a thread whose G
has run for more than 10 ms, and the signal handler parks the goroutine at a
safe point.

`burrow` does the same, with one constraint Go does not have: Go's compiler
guarantees the signal lands at an async-safe point with a known stack map. We
have no stack maps. So:

- `SIGURG` (as Go uses) on POSIX; `SuspendThread` + context inspection on
  Windows.
- The handler cannot park mid-instruction safely without a stack map, so it
  sets a **preemption request flag** on the G, and the goroutine yields at the
  next safe point. Safe points are: every channel operation, every `sync`
  operation, every allocation, every syscall boundary, every function in
  `burrow` marked `BURROW_PREEMPTIBLE`, and — for user code that has none of those
  — an explicit `gosched()`.
- A goroutine that reaches no safe point within 10 ms is reported by `sysmon`
  to the `GODEBUG=schedtrace`-equivalent diagnostic and, under
  `BURROW_PREEMPT_HARD=1`, is preempted by stack-scanning heuristics that are
  documented as best-effort.

So preemption is **cooperative-at-safe-points with asynchronous requesting** —
strictly better than libmill/libdill, not quite Go. A pure compute loop in user
C code with no `burrow` calls will not be preempted, and the documentation says
so and points at `gosched()`. This is the third and final acknowledged
runtime compromise.

`GOMAXPROCS`, `NumCPU`, `NumGoroutine`, `Gosched`, `LockOSThread`/
`UnlockOSThread` (needed by `os/signal` and by GUI interop) all port faithfully.

## 10. `testing/synctest`

Go 1.25 added `synctest` — run a "bubble" of goroutines with a fake clock that
advances only when every goroutine in the bubble is durably blocked, making
concurrent tests deterministic. It is listed as substrate-coupled in
[01](01-scope.md) §6 and it is the one place where owning the scheduler makes
us *better off* than a port normally is:

```c
synctest_test(t, bubble_fn, arg);
synctest_wait();
```

Implementation is direct: the bubble is a P-like scheduling domain with its own
virtual clock; `sched_park` records whether a block is durable; when the bubble's
runnable count hits zero, the clock jumps to the next timer. We control every
park point, so "durably blocked" is exactly knowable rather than inferred.

This matters beyond fidelity: **`burrow`'s own concurrency tests run inside
`synctest`**, which is how a hand-written scheduler gets tested deterministically
instead of by hoping race conditions show up in CI. → [14](14-conformance.md) §4

## 11. `runtime`'s public surface

Of `runtime`'s 124 exported declarations, we implement all of them; roughly 30
are meaningful, the rest are GC knobs that become documented no-ops.

| Real | `GOMAXPROCS`, `NumCPU`, `NumGoroutine`, `Gosched`, `Goexit`, `LockOSThread`, `UnlockOSThread`, `Caller`, `Callers`, `CallersFrames`, `FuncForPC`, `Stack`, `Version`, `GOOS`, `GOARCH`, `SetFinalizer`*, `AddCleanup`*, `KeepAlive`, `Error` types, `MemProfileRate`, `SetBlockProfileRate`, `SetMutexProfileFraction`, `ReadMemStats`, `ReadTrace` |
| No-op / degraded | `GC`, `SetGCPercent`, `SetMemoryLimit`, `NumCgoCall`, `MemStats` GC fields |

\* Real under `gc`; arena-teardown callbacks otherwise
([05](05-memory.md) §8).

`Caller`/`Callers`/`Stack`/`FuncForPC` need symbolisation, which in C means
either a platform unwinder (`libunwind`, `_Unwind_Backtrace`, `dbghelp`) or
frame-pointer walking plus a generated symbol table. Decision: frame-pointer
walking (compile `burrow` with `-fno-omit-frame-pointer`, which costs
essentially nothing on 64-bit and which distributions increasingly default to)
plus an embedded symbol table generated at amalgamation time. This gives
readable panic tracebacks with no dependency, which is the actual requirement —
a panic without a traceback is a support nightmare.

`runtime/pprof` and `runtime/trace` then emit Go's exact wire formats
(pprof protobuf, the Go execution-trace format), so `go tool pprof` and
`go tool trace` work on `burrow` output unmodified. That is a small amount of
work for a disproportionate payoff in usability, and it is the kind of detail
that decides whether people adopt this.

## 12. Order of construction

The build order within Tier 0, because it is unusually constrained:

1. Atomics abstraction, `sched_park`/`sched_ready` primitive, platform threads.
2. Context-switch assembly for amd64 + arm64; portable fallback for the rest.
3. Stack allocation with guard pages; overflow → panic conversion.
4. P/M/G structures, local and global run queues, `findRunnable`, work stealing.
5. Timers (four-heap, per-P, as Go's), `sysmon`.
6. Channels, then `select`.
7. `defer`/`panic`/`recover`, then stack walking and symbolisation.
8. `sync` package on top of park/unpark.
9. Netpoller: `epoll` first, then `kqueue`, then deadlines, then IOCP.
10. `context`. Then `synctest`, and immediately retro-test 4–9 inside it.
11. Preemption. Last, because it is the hardest to debug and everything else
    must be stable first.

Step 10 before step 11 is deliberate: `synctest` gives deterministic tests for
the scheduler, and adding preemption to an untested scheduler produces bugs
that cannot be reproduced.

## Sources

- [Scheduling In Go, Part II — Ardan Labs](https://www.ardanlabs.com/blog/2018/08/scheduling-in-go-part2.html)
- [Go Scheduler — nghiant3223](https://nghiant3223.github.io/2025/04/15/go-scheduler.html)
- [How Go Handles Networking — goperf.dev](https://goperf.dev/02-networking/networking-internals/)
- [Understanding the Go Runtime: The Network Poller](https://internals-for-interns.com/posts/go-netpoller/)
- [Go Netpoller and Runtime Behavior](https://alexanderobregon.substack.com/p/go-netpoller-and-runtime-behavior)
- [libmill](https://github.com/sustrik/libmill) ·
  [libdill structured concurrency](https://sustrik.github.io/libdill/structured-concurrency.html) ·
  [c-coroutine](https://github.com/zelang-dev/c-coroutine)
