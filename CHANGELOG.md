# Changelog

Every release gets a section here and the release workflow refuses to publish a tag that does not have one, so this file cannot fall behind.

Versions are `0.MINOR.PATCH` until 1.0. The minor number goes up when a milestone finishes and the patch number goes up for everything in between. Nothing before 1.0 is a stable API and everything before 1.0 is published as a prerelease, because none of it has been through a security review.

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
