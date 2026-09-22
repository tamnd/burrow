# 17 — Open questions

Everything decided is decided in documents 01–16, and most of it is decided
firmly. This file is the opposite: the things that are genuinely undecided,
where I do not have enough information to commit, and where committing early
would be worse than leaving the question open.

Each entry states the question, the options actually on the table, what would
settle it, and — where I have one — a leaning. A leaning is not a decision. The
distinction matters, because a spec that pretends to have resolved everything
is a spec whose real uncertainties surface as surprises during implementation.

Fourteen questions, roughly in order of how much they would change the project.

## 1. Is the explicit-allocator ergonomic actually acceptable?

[05](05-memory.md) commits to Zig-style explicit allocator passing: every
function that allocates takes `Alloc *a` first, no exceptions beyond the two
documented carve-outs. This is the single most consequential API decision in
the project, it touches every signature, and it is the one I am least
confident about.

The case for: it is the only honest answer in C, it makes arena-per-request
possible, it avoids a mandatory GC, and Zig has demonstrated that users tolerate
it.

The case against: Go's appeal is partly that you do not think about this, and
`strings_split(a, s, ",")` is uglier than `strings.Split(s, ",")` in a way
that compounds over a whole program. The `BURROW_IMPLICIT_ALLOC` opt-in layer
([08](08-naming-abi.md) §4) exists precisely because I suspect this, but an
opt-in layer with nine documented collisions is a hedge, not an answer.

**What settles it:** shipping 0.1 and watching people write code with it.
[16](16-milestones.md) §3 makes this the explicit purpose of 0.1 and freezes
the substrate API only at 0.5. If the feedback is that the allocator parameter
is intolerable, the fallback is making `BURROW_IMPLICIT_ALLOC` the documented
default with a thread-local allocator, and treating explicit passing as the
advanced mode. That is a big change and it is cheaper at 0.1 than at 0.9.

**Leaning:** ship explicit, promote implicit if users demand it.

## 2. `simd` and `simd/archsimd`

New in Go 1.27, architecture-gated, and a moving target upstream — the API is
explicitly marked unstable and will change. It is 1,200-odd declarations of
what are, in Go, compiler intrinsics with no library implementation at all.

The problem is not difficulty; it is that the C analogue already exists and is
better. A `burrow` user who wants SIMD writes `<immintrin.h>` or
`<arm_neon.h>`, which is what Go's `archsimd` is a wrapper *for*. Reproducing
Go's spelling of intrinsics in C, where the real intrinsics are one include
away, is work that produces nothing.

Options: (a) deferred, tracked as a gap — the current position in
[01](01-scope.md) §6; (b) a thin macro layer mapping Go's names onto the
platform intrinsics, which is mechanical and mostly generated; (c) never,
declared permanently out of scope and removed from the coverage-gate
denominator.

**What settles it:** whether Go's API stabilises, and whether anyone asks. If
`archsimd` settles and a user wants portability of Go SIMD code, (b) is a
weekend of code generation. If nobody asks, (a) and (c) are the same thing with
different honesty.

**Leaning:** (b), late, generated — but only after the API stops moving.

## 3. Is fixed-size stack the final answer?

[06](06-runtime.md) §4 commits to 256 KB guard-paged stacks because C cannot
relocate a stack: there is no way to find and rewrite interior pointers, and a
C compiler will happily keep a pointer to a local in a register.

This is the second of three acknowledged compromises and the one with the most
practical bite: a million goroutines needs 256 GB of address space, which is
fine on 64-bit and impossible on 32-bit.

Options not yet exhausted: split stacks via a compiler flag (`-fsplit-stack` on
GCC, incomplete and effectively abandoned); segmented stacks with a guard-page
trap handler that allocates a new discontiguous segment and rewrites the frame
pointer, which is how Go worked before 1.3 and which has known hot-split
pathologies; a smaller default (64 KB) with a documented recursion budget;
per-goroutine stack sizing via `go_with_stack_size`.

**What settles it:** measurement. The question is what fraction of real
goroutines ever exceed 64 KB, and whether lazy commit makes the address-space
cost irrelevant in practice. A profiling build that records high-water marks
across the test suite answers it cheaply.

**Leaning:** keep fixed, add `go_with_stack_size`, lower the default if the data
supports it. Segmented stacks are a trap.

## 4. Preemption: is cooperative-at-safe-points good enough?

[06](06-runtime.md) §9 commits to cooperative preemption at function-call safe
points, with async signals used to *request* a yield rather than to force one.
Go, since 1.14, can preempt a tight loop with no calls in it; `burrow` cannot,
because a signal handler cannot safely rewrite a C frame's state to inject a
yield.

Consequence: a goroutine running `for {}` with no calls blocks its P forever,
and a program that relies on Go's preemption guarantees will hang. `sysmon` can
detect it and report it, but not fix it.

Options: accept and document (current); a `BURROW_PREEMPT_CHECK()` macro that users
put in hot loops, which is honest but is a manual safety requirement; compiler
assistance via `-finstrument-functions` (too slow) or a `-fplugin` (not
portable); one OS thread per goroutine above a threshold so the OS preempts,
which trades the problem for a different one.

**What settles it:** whether real ported Go code trips over it. Go's own test
suite includes tests that specifically require async preemption; those tests
will fail and their failure is the measurement.

**Leaning:** accept, document loudly, add `BURROW_PREEMPT_CHECK()`, and have
`sysmon` print a diagnostic naming the stuck goroutine. A clear error beats a
silent hang.

## 5. How far should `burrow-gen --from-go` go?

Today: translate Go type and function *declarations* into `burrow` type
descriptors and headers. That is well-defined, useful, and small.

The tempting extension is translating *bodies* — a Go-to-C transpiler — which
would turn the whole porting effort from hand work into tool work. It is also
a substantially different and much larger project: Go's semantics include
garbage collection, closures capturing by reference, `defer` with loop
variables, goroutines as a language construct, interface satisfaction inferred
structurally, and generics with type inference. A transpiler that handles 90%
of Go is a tool that produces subtly wrong code 10% of the time, which is worse
than no tool.

Options: declarations only (current); declarations plus straight-line
expression bodies as a *porting aid* whose output a human reviews and owns;
a real transpiler as a separate project.

**What settles it:** trying the middle option on a Tier 1 package and measuring
how much of the resulting diff a human has to rewrite. If it is 20%, it is a
huge accelerant; if it is 60%, reading the Go and writing the C is faster.

**Leaning:** try the middle option early — during P1, on `strings` — because
the answer changes the effort model in [16](16-milestones.md) §1 by a large
factor in either direction.

## 6. C11 or C23?

[03](03-c-dialect.md) commits to C11 because MSVC is the constraint. C23 would
give `#embed` (which would make `embed` and tzdata trivial), `typeof`,
`nullptr`, `constexpr`, `_BitInt`, and standardised attributes — all of which
this project wants.

**What settles it:** MSVC's C23 support, which is partial and improving, and
whether the Tier C compilers (TCC, cproc, chibicc) ever get there. A C23
baseline that drops MSVC is not acceptable; a C11 baseline with C23 features
used behind `__STDC_VERSION__` guards is what we do now and is unsatisfying but
correct.

**Leaning:** hold C11 for 1.0, re-evaluate for 2.0. Use `#embed` when available
and the generated-byte-array fallback otherwise, since that particular
divergence is invisible to users.

## 7. Should `syscall` be public at all?

[10](10-packages-os.md) commits to reproducing `syscall`'s 4,149
platform-specific declarations for fidelity while forbidding any `burrow` code
above the PAL from calling it. It is generated, it is large, it is
platform-specific, and it is *deprecated in Go* in favour of
`golang.org/x/sys/unix`.

So we are generating 180,000 lines of a deprecated API to satisfy a coverage
gate. The counter-argument is that the coverage gate is the project's central
claim and carving out the inconvenient 17% of it is exactly how such claims
rot.

Options: full generation (current); generate only the constants and types,
omitting the wrapper functions, and ledger the difference; omit entirely and
provide `x/sys`-style access instead.

**What settles it:** whether anyone uses it. In C, calling `syscall(2)`
directly is normal and needs no wrapper library, which is a real argument that
`syscall_*` is dead weight.

**Leaning:** full generation, because it is generated and therefore cheap, and
because a mechanical claim with one large exception is not a mechanical claim.
But this is the entry on this list I would most readily trade away.

## 8. What is the concurrency story for `BURROW_OMIT_GOROUTINES` users?

[15](15-build-deploy.md) §5 offers a build with no scheduler for embedded and
single-threaded use. In that mode channels are unavailable and `SyncMutex`
is a no-op — which means a large fraction of the library's *own* internals,
which use `sync` freely, need to work with a degenerate implementation.

That is fine for mutexes. It is not obviously fine for anything that assumes a
second goroutine can make progress: `io.Pipe`, `net/http`'s transport,
`database/sql`'s pool, `context`'s cancellation propagation, `sync.WaitGroup`
waiting on work that will never run.

**What settles it:** deciding which packages are available in that mode, and
testing it as a real configuration rather than a documented aspiration. The
honest answer may be that `BURROW_OMIT_GOROUTINES` restricts you to Tier 1 plus
a synchronous subset of `os`, and that this is stated as a supported
*profile* with an enumerated package list — not as a flag that happens to
compile.

**Leaning:** define it as a named profile with an explicit package list, and
put that list in the coverage gate as a separate column.

## 9. Error allocation: is the per-goroutine arena right?

[05](05-memory.md) §2 carves out errors from the explicit-allocator rule: they
come from a per-goroutine error arena with `BURROW_ERROR_SCOPE` and
`error_retain` to promote one out of scope. This is the compromise that
keeps `if (BURROW_FAILED(err))` from requiring an allocator at every call site.

It has a failure mode: an error retained across a scope boundary without
`error_retain` is a dangling pointer, and the whole point of the design was
to avoid that class of bug. The `track` allocator catches it in tests
([14](14-conformance.md) §5) but only on paths the tests exercise.

Options: current; reference-counted errors (a small, bounded use of refcounting
that does not contaminate the rest of the design); errors as values copied into
caller-provided storage, which is uglier but has no lifetime question at all;
immortal errors — never free error allocations, accepting a slow leak in
long-running error-heavy loops.

**What settles it:** how often the annotation is got wrong in practice, and
whether a static check (a clang-tidy plugin over the `BURROW_OWNS`/`BURROW_BORROWS`
annotations) can catch it at compile time rather than test time.

**Leaning:** current, plus the static check, plus refcounting as the fallback
if the check proves insufficient. Errors are the one place where a little
refcounting is affordable.

## 10. Reflection: X-macros or the generator?

[07](07-reflect.md) offers both: an X-macro DSL requiring no build step, and
`burrow-gen reflect` using libclang. The DSL is primary because it preserves
the `cc burrow.c` promise, which is the whole deployment story.

But the DSL is genuinely unpleasant for large structs, and the promise it
preserves is about building *`burrow`*, not about building the user's program —
a user who already runs CMake would not notice a generator step.

**What settles it:** user reports. If everyone runs the generator anyway, the
DSL is a maintenance burden for a promise nobody is cashing; if people
appreciate the zero-dependency path, it stays primary.

**Leaning:** keep both, keep the DSL primary and hand-writable, and make the
generator's output *be* DSL invocations rather than raw descriptors — so the
two paths converge and there is only one thing to test.

## 11. Should the amalgamation include the tests?

SQLite ships its tests separately and nobody minds. But a user vendoring
`burrow.c` into a product has no way to verify their compiler and platform
produce a correct build, which for a library this size is a real gap — and
"my compiler miscompiled it" is a failure mode this project should expect.

Options: separate, as now; a `burrow-test.c` companion amalgamation containing
the translated tests for the selected packages, runnable as one binary; a small
built-in smoke test (`burrow_selftest()`) covering the substrate and the highest-
risk packages, in the main amalgamation, a few hundred KB.

**Leaning:** all three, in that order of priority. `burrow_selftest()` is cheap and
answers "did my compiler break it" in one call, which is worth a lot for a
project whose distribution model invites unusual compilers.

## 12. Timer and netpoller design under `synctest`

[06](06-runtime.md) §10 promises a real `testing/synctest` — fake time,
deterministic scheduling, idle detection — and argues it is cleaner than Go's
because we own the scheduler.

The timer half is settled and shipped. A bubble owns a set of timers and a
reading of its own, `burrow__timers_local` hands a goroutine inside a bubble
the bubble's set rather than its P's, and the goroutine in `synctest_run` runs
what is due and then winds the reading forward to the next deadline.
Everything built on timers followed for free, including the `context`
deadlines, which needed no change at all. [06](06-runtime.md) §10 has the
shape and [guides/synctest.md](../guides/synctest.md) has the rules.

The unresolved part is the interaction with the netpoller: `synctest` requires
that all goroutines in a bubble be durably blocked before time advances, and a
goroutine blocked on a real socket is not durably blocked in any way the
scheduler can distinguish from one that is about to be woken. Go handles this
by making network I/O outside a bubble an error. Whether that is sufficient
for testing `net/http`, which is where deterministic tests would be most
valuable, is unclear.

There is a second half to that which is now concrete rather than hypothetical.
A deadline armed on a socket inside a bubble becomes a timer on the bubble's
clock, and the clock cannot move while the socket read is keeping the bubble
from going idle, so the deadline never fires. Go has the same shape. It is
fine as long as the rule is that real network IO does not go in a bubble, and
it is the thing to revisit when `net` lands.

**What settles it:** porting Go's own `synctest` tests and then trying to write
a deterministic `net/http` timeout test. If it works, `burrow` has something
genuinely better than most C runtimes offer.

## 13. Which Go version to track, and how to handle drift?

[08](08-naming-abi.md) §7 says `burrow 1.27.x` tracks Go 1.27. But Go releases
every six months, each adding API, and `burrow` will not keep pace during
development — by the time 1.0 ships, Go may be at 1.31.

Options: pin to one version and catch up after 1.0, so the coverage gate stays
meaningful against a fixed denominator; track HEAD continuously and accept a
moving target; track the newest *stable* at each `burrow` minor release.

**Leaning:** pin 1.27 through 1.0, then track stable with a one-release lag.
A moving denominator makes the coverage gate unfalsifiable, and the gate is the
project's spine.

## 14. Is there a licensing obstacle nobody has hit yet?

[18](18-legal.md) concludes that a C port is a derivative work of Go under
BSD-3-Clause, that this is fine with attribution, and that the PATENTS grant
carries over. I believe that is right and it is the standard analysis.

What I cannot do is give legal advice, and this project's whole existence rests
on that conclusion. The specific things I would want a lawyer to confirm:
whether doc comments copied verbatim into `burrow` headers are covered by the
same licence (they are part of the same distribution, so almost certainly yes);
whether the PATENTS grant's "this implementation of Go" language extends to a
reimplementation; and whether the trademark position in
[18](18-legal.md) §3 is sufficient.

**What settles it:** an actual lawyer, once, early. It is a few hours of
someone's time and it de-risks the entire project. Do it before 0.1, not before
1.0.

---

## The shape of this list

Questions 1, 3 and 4 are the ones that could change the project's character,
and all three are the same question in different clothes: *how much of Go's
implicitness can C's explicitness carry before users stop caring?* None of them
can be answered by more analysis. They are answered by shipping 0.1 and finding
out, which is the actual argument for [16](16-milestones.md) §3's early release
schedule.

Questions 5 and 14 are the cheapest to resolve and have the largest expected
value: one is a week of experiment that could halve the project's effort, and
the other is a few hours of legal review that protects all of it. Both should
happen before the substrate is finished.

Everything else on this list is a detail that will resolve itself under
contact with the code, and is listed here only so that resolving it counts as
progress rather than as discovering a problem.
