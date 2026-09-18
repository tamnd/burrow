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

`sysmon` is a dedicated thread waking roughly every 10 ms to retake Ps from
Ms blocked in syscalls, deliver preemption signals, poll the netpoller if
nothing else has recently, and fire timers. Go degrades badly without it; so
would we.

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

## 2. Context switching

This is where the POSIX-only prior art ([02](02-landscape.md) §3) is
insufficient. `swapcontext` does not exist on Windows and makes a
`sigprocmask` syscall per switch where it does. The answer, as in `minicoro`,
`libaco` and Boost.Context: **hand-written per-ABI assembly**, about 40
instructions each.

| ABI | Registers to save | Notes |
| --- | --- | --- |
| SysV AMD64 | rbx, rbp, r12–r15, rsp, rip, mxcsr, x87 CW | ~30 instrs |
| Win64 | above + rsi, rdi, xmm6–xmm15, TIB stack limits | must update `NT_TIB` stack bounds or Windows guard pages fire |
| AArch64 AAPCS | x19–x30, sp, d8–d15 | + PAC/BTI landing pads on Apple and hardened Linux |
| RISC-V | s0–s11, sp, ra, fs0–fs11 | |
| PPC64 ELFv2, s390x | per ABI | |
| wasm | — | no switching; see §9 |

Two details that are easy to get wrong and expensive to discover late:

- **Windows requires updating the TIB's `StackBase`/`StackLimit`** on every
  switch. Omit it and Windows' stack guard page mechanism misfires on the
  second goroutine, usually as a mysterious access violation deep in a callee.
- **Unwinding metadata.** Each goroutine stack needs correct CFI/SEH so that
  debuggers, profilers and `runtime.Stack()` can walk it. Hand-written
  `.cfi_*` directives per switch stub, and a registered dynamic function table
  on Win64.

A portable fallback exists (`ucontext` on POSIX, Fibers on Windows) selected by
`-DBURROW_PORTABLE_CONTEXT=1`, for architectures we have not written assembly
for. It is correct and ~10× slower per switch.

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

## 5. Channels and `select`

```c
Chan *chan_make(Alloc *a, const Type *elem, Int cap);
bool     chan_send(Chan *c, const void *v);          /* false if closed */
bool     chan_recv(Chan *c, void *out);              /* false if closed+drained */
bool     chan_try_send(Chan *c, const void *v, bool *ok);
void     chan_close(Chan *c);
Int   chan_len(Chan *c);
Int   chan_cap(Chan *c);
```

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
allocation and no `setjmp`. Fairness matters and is copied exactly: when
multiple cases are ready, one is chosen **uniformly at random**, using the
per-P fast RNG, because Go does this and Go programs (and tests) depend on the
absence of starvation. The poll order and lock order across the case set follow
Go's channel-address ordering to avoid deadlock.

## 6. `defer`, `panic`, `recover`

**`defer`** has two implementations and a user choice between them.

The cheap one uses the GCC/Clang `cleanup` attribute, which runs a function on
scope exit. It is zero-cost, it composes with early `return`, and it is not
available on MSVC:

```c
#define BURROW_DEFER(fn, arg) \
    __attribute__((cleanup(burrow__defer_run))) burrow__defer_rec _gd##__LINE__ = {fn, arg}
```

The portable one pushes a record onto the goroutine's defer chain and requires
an explicit scope block, which is the form used inside `burrow` and the form
that works on MSVC and that panic-unwinding can see:

```c
BURROW_SCOPE {
    OsFile *f = os_open(a, path, &err);
    BURROW_DEFER_CALL(os_file_close, f);
    ...
}   /* defers run here, LIFO, even on panic */
```

LIFO order, argument evaluation at `defer` time, and the ability to modify
named results are all preserved; the last of these requires the deferred
function to receive a pointer to the result slot, which `BURROW_DEFER_RET` provides.

**`panic`/`recover`** is `setjmp`/`longjmp` over the defer chain, scoped to a
single goroutine:

```c
void      panic(Any v);
Any    recover(void);        /* valid only inside a deferred call */

BURROW_TRY {
    risky();
} BURROW_CATCH(p) {
    log_panic(p);
}
```

Rules, matching Go:

- A panic runs the deferred functions of each frame as it unwinds, in LIFO
  order, until one of them calls `recover`.
- An unrecovered panic terminates the *program*, printing the panicking
  goroutine's stack and the goroutine that created it — Go's behaviour, and
  important: a panic in one goroutine is not contained to it.
- `recover` outside a deferred call returns nil.
- Re-panicking inside a defer chains the panics and prints both.
- Runtime errors that Go panics on — nil map write, index out of range, nil
  dereference through an interface, integer divide by zero, slice bounds,
  type-assertion failure — panic with the corresponding `runtime.Error` type,
  with the same message text, because those strings appear in tests.

The [03](03-c-dialect.md) §3 restriction — no `longjmp` across allocator or
lock boundaries — is enforced by having every `burrow` lock and every arena
scope register a defer record, so unwinding releases them in order. A checker
in CI flags any `setjmp` use outside the three sanctioned macros.

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
9. Netpoller: `epoll` first, then `kqueue`, then IOCP.
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
