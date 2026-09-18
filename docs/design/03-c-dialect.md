# 03 — The C dialect: baseline, compilers, portability rules

A 400,000-line C library that must compile everywhere has to decide, once and
in writing, exactly which C it is written in. Get this wrong and the cost is
not a compile error — it is a slow bleed of `#ifdef` across 180 packages.

## 1. The baseline: C11, with a C23 fast path

**`burrow` is written in C11 and compiles with `-std=c11` / `/std:c11`.**
C23 features are used only behind feature detection, and every one of them has
a C11 fallback in the same header.

The reason is MSVC and it is not close. As of 2026 the support picture is:

| Feature | GCC | Clang | MSVC |
| --- | --- | --- | --- |
| C11 core | ✅ | ✅ | ✅ (since VS2019 16.8) |
| `_Generic` | ✅ | ✅ | ✅ |
| `_Atomic` | ✅ | ✅ | ⚠️ partial; use intrinsics |
| `_Thread_local` | ✅ | ✅ | ✅ (`__declspec(thread)`) |
| `typeof` | ✅ (C23 / GNU ext) | ✅ | ❌ (`__typeof__` unavailable in C mode) |
| `nullptr` / `nullptr_t` | ✅ (GCC 13+) | ✅ (18+) | ❌ |
| `constexpr` (objects) | ✅ (GCC 13+) | ✅ (18+) | ❌ |
| `_BitInt(N)` | ✅ | ✅ (first to ship) | ❌ |
| `#embed` | ✅ (GCC 15+) | ✅ (19/20+) | ❌ (not even in C++) |
| C23 as default dialect | ✅ (GCC 15) | — | ❌ no published roadmap |

GCC leads — GCC 15 made C23 the default and added `#embed`. Clang 18+ is solid.
MSVC has **no published C23 roadmap** even in Visual Studio 2026, offers only
partial support via `/std:clatest`, and is widely observed not to fully
implement C99. Since Windows is a required platform and MSVC is what Windows
shops use, C11 is the floor.

Three C23 features are worth the conditional plumbing:

- **`#embed`** — makes `embed.FS` a two-line implementation instead of a
  code-generation step. Guarded by `__has_embed`; falls back to
  `burrow-gen embed` emitting a `static const unsigned char[]`.
  → [10](10-packages-os.md) §8
- **`typeof`** — makes the generic macros in `slices`/`maps`/`sync/atomic`
  dramatically cleaner. Available as `__typeof__` on GCC/Clang regardless of
  standard, so the C11 path already has it everywhere except MSVC; the MSVC
  path uses explicit type arguments. → [08](08-naming-abi.md) §5
- **`_BitInt(N)`** — would simplify `math/big` limb arithmetic and
  `crypto/internal/fips140` field arithmetic. Not available on MSVC, so it
  stays an optimisation, never a requirement.

`nullptr` and `constexpr` are declined outright: the benefit is stylistic and
the cost is a third code path.

## 2. Required compilers and the CI matrix

A configuration is **supported** only if it is in CI. Everything else is
"probably works, report bugs."

| Tier | Toolchain | Platforms | Gate |
| --- | --- | --- | --- |
| A | GCC 13+ | linux/amd64, linux/arm64 | every PR |
| A | Clang 18+ | darwin/arm64, linux/amd64 | every PR |
| A | MSVC 19.40+ (VS2022/2026) | windows/amd64 | every PR |
| B | Clang 18+ | darwin/amd64, windows/arm64, freebsd/amd64 | nightly |
| B | GCC 13+ | linux/riscv64, linux/ppc64le, linux/s390x (big-endian!) | nightly |
| B | `cosmocc` | APE (x86-64 + aarch64) | nightly |
| C | `clang --target=wasm32-wasi` | wasip1/wasm | nightly, reduced suite |
| C | GCC 13+ | openbsd, netbsd, illumos, aix/ppc64 | weekly |
| C | TCC, cproc, chibicc | linux/amd64, C11 subset | weekly, best-effort |

`linux/s390x` earns a Tier B slot on its own merit: it is the only realistic
big-endian target available, and it catches the entire class of byte-order bugs
that `encoding/binary`, `debug/elf`, `crypto` and every wire format can hide.
Go supports it; so do we.

In practice s390x runs on every pull request rather than nightly, which is a
deviation from the table above and one worth taking. Under QEMU the whole suite
takes about three minutes, and the first time it ran it found a test that read
the first byte of an `Int` and expected the low one. That is exactly the bug the
slot exists for, and finding it the next morning in a nightly against a tree
that had moved on would have been strictly worse. The rest of Tier B stays
nightly, because the rest of Tier B is about toolchains and platforms rather
than about a property every line of code in the tree has to have.

Tier C's tiny compilers (TCC, cproc, chibicc) are not vanity. They enforce that
we have not accidentally become GCC-dependent, and TCC in particular makes
`burrow` usable as a scripting substrate. They compile the C11 core only, with
generics macros in explicit-type mode.

Go supports **47 GOOS/GOARCH combinations**, 8 of them first-class. `burrow`'s
Tier A+B is 11 configurations; the remaining Go ports are reachable because the
PAL is small, and are accepted as community-maintained.
→ [10](10-packages-os.md) §3

## 3. Language rules

Enforced by a clang-tidy config plus a custom checker in CI, not by convention.

**Mandatory**

- `-std=c11` with no GNU extensions in portable code. GNU extensions permitted
  only inside `pal/` and `runtime/` behind `#if BURROW_HAS_GNU`.
- Every translation unit compiles clean under
  `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wstrict-prototypes
  -Wold-style-definition -Wmissing-prototypes -Wvla -Werror`, and MSVC `/W4 /WX`.
- No VLAs, no `alloca`. Stack budgets matter because goroutine stacks are
  bounded. → [06](06-runtime.md) §4
- No recursion without a documented depth bound. `encoding/json`,
  `encoding/xml`, `go/parser` and `regexp/syntax` all recurse on untrusted
  input and all get explicit depth limits matching Go's.
- Fixed-width integers everywhere (`int32_t`, `uint64_t`). The sole exceptions
  are `Int`/`Uint`, which are typedefs matching Go's `int`/`uint`
  (64-bit on 64-bit platforms, 32-bit on 32-bit). → [04](04-core-types.md) §2
- No `char*` string arguments in public API except in documented C-interop
  helpers (`str_from_cstr`, `str_to_cstr`). Go strings are not
  NUL-terminated and pretending otherwise is the most likely source of
  correctness bugs. → [04](04-core-types.md) §3
- All public functions declared `BURROW_API`, which expands to the right
  visibility/dllexport attribute per platform.
- Every `static` function and file-scope variable in a package carries the
  package's internal prefix, so that concatenation into one translation unit
  cannot collide. Enforced mechanically — this is exactly SQLite's discipline.

**Forbidden**

- C++ anywhere in the tree, including "just for tests." A C++ toolchain
  requirement would break `cc burrow.c`, wasm builds, and TCC.
- Global mutable state outside an explicitly documented registry list
  (§5 below). No `errno`-style thread-local error slot for the public API;
  errors are returned. → [04](04-core-types.md) §6
- `setjmp`/`longjmp` across allocator or lock boundaries. `panic`/`recover` is
  built on it but only within a goroutine's own frame chain, with unwinding
  through registered defer records. → [06](06-runtime.md) §6
- `assert()` in shipped code. Internal invariants use `BURROW_ASSERT`, compiled out
  in release, and `BURROW_CHECK` for conditions that must hold in release.
- Signed integer overflow, ever, including in hash functions. Compute in
  unsigned and convert. CI runs UBSan with `-fno-sanitize-recover`.
- Strict-aliasing violations. Type punning goes through `memcpy` or a union.
  `burrow` must survive `-O3 -fstrict-aliasing`, which is the default.

## 4. Undefined behaviour posture

Go is a memory-safe language and its library is written as if memory safety is
free. A C port inherits every buffer, every index and every pointer without
that guarantee, and the consequence is that `burrow`'s bug class is strictly
worse than Go's. This is the one place where the port is genuinely inferior to
the original, and pretending otherwise would be dishonest.

Mitigations, all mandatory:

- **Bounds-checked by default.** `Slice` access goes through
  `slice_at(s, i)`, which panics on out-of-range exactly as Go does. An
  unchecked `BURROW_SLICE_AT_UNSAFE` exists for hot loops and is auditable by grep.
  The cost is real and measured; see [16](16-milestones.md) §5.
- **Sanitiser matrix in CI.** Every Tier A configuration runs the full suite
  under ASan+UBSan, and under TSan for anything touching Tier 0. This is also
  how `runtime/race` is implemented, so the work is dual-purpose.
  → [01](01-scope.md) §6
- **Continuous fuzzing.** Every parser, decoder and protocol implementation is
  fuzzed with libFuzzer, seeded from Go's `testdata`, in differential mode
  against Go. → [14](14-conformance.md) §5
- **`-D_FORTIFY_SOURCE=3`, `-fstack-protector-strong`, `-fstack-clash-protection`,
  CFI and `-ftrivial-auto-var-init=zero`** in the recommended release flags,
  published in the README so users inherit them.
- **No unchecked arithmetic on lengths.** All length/capacity arithmetic goes
  through `burrow__size_add`/`burrow__size_mul` which detect overflow and panic. This
  closes the classic integer-overflow-to-heap-overflow path that every
  `decode` function in the library is otherwise exposed to.

## 5. The global state list

Reentrancy is a requirement, so global mutable state is enumerated,
justified, and individually synchronised. These are the categories, and
nothing outside them is allowed:

| State | Why unavoidable | Synchronisation |
| --- | --- | --- |
| The scheduler (Ps, Ms, run queues) | It is the runtime | Per-P locks + atomics, work-stealing protocol |
| The netpoller (epoll/kqueue/IOCP handle) | One per process is correct | Internal lock; multiple instances supported for tests |
| Type descriptor registry | `reflect` needs process-wide type identity | Write-once at init, then read-only; RW lock for dynamic registration |
| Allocator singletons | `malloc` is a singleton and so is a collector | Written only by `mem_set_oom`, before the second thread |
| `time` zone cache | `LoadLocation` caching, matches Go | Mutex-guarded map |
| Signal disposition table | The OS has one per process | Matches `os/signal`'s design exactly |
| `flag.CommandLine`, `os.Args`, `os.Stdout`… | Go exposes these as package-level vars | Public, documented, same caveats as Go |
| Markers (`zerobase`, the tombstone) | An address has to come from somewhere | None needed; never written, never read through |
| The fatal handler | Something has to run when the runtime gives up | Written once at startup, atomic load once `sync/atomic` exists |
| The random generator | Map iteration order and hash seeding | Thread-local, which is what makes it lock free |
| The 64-bit atomic lock table | A 32-bit machine cannot load eight bytes atomically | It is the synchronisation. One spin lock per address hash, held for one operation |

Anything else that wants to be global is a design error. The test harness
constructs fresh instances of the first four to run tests in parallel.

This list is not prose. `tools/globals.txt` has one line per global with its
category and how it is synchronised, and `tools/check-globals.sh` fails the
build when the tree and that file disagree in either direction — a global that
is not listed, or a line whose global no longer exists. The second direction is
the one that matters, because a list that can rot is a list nobody reads.

## 6. Header discipline

- **Public headers include only public headers**, plus `<stddef.h>`,
  `<stdint.h>`, `<stdbool.h>`. Never `<stdio.h>`, never `<windows.h>`, never
  `<sys/*.h>`. A user including `go/strings.h` must not inherit the Windows
  API.
- **No macro leakage.** Every public macro is `BURROW_`-prefixed. `min`, `max`,
  `bool` and friends are not defined. `windows.h`'s `min`/`max`/`ERROR` are
  actively defended against.
- **Self-contained and idempotent.** Every header compiles alone, twice, in C
  and under a C++ compiler (`extern "C"` guards — C++ *consumers* are
  supported, C++ *source* is not).
- **One header per Go package**, at the path the Go import path implies:
  `#include <burrow/net/http.h>`. Plus `<burrow/all.h>` for the impatient.
  → [08](08-naming-abi.md) §3
- **`burrow.h` is generated** by concatenating the selected packages' headers
  in dependency order, with include guards stripped. Same machinery as the
  amalgamation. → [15](15-build-deploy.md) §2

## 7. Portability rules that bite

Learned the hard way in every C port; written down so we do not relearn them.

| Hazard | Rule |
| --- | --- |
| `char` signedness (signed on x86, unsigned on ARM/PPC) | Never use bare `char` for data. `uint8_t` for bytes, `Byte` typedef. |
| Byte order | All wire formats go through explicit `burrow__le32`/`burrow__be64` helpers. CI includes s390x. |
| `long` is 32-bit on Windows, 64-bit elsewhere | `long` is banned. |
| `time_t` size and epoch | Never crosses an API boundary. `Time` is internally int64 seconds + int32 nanos + monotonic + location, as Go's is. |
| Path separators and case | `path` is always `/`; `path/filepath` is platform-aware. Windows `\\?\` long-path and UNC handling, and Windows' reserved device names, are in the PAL. → [10](10-packages-os.md) §7 |
| `int` width for syscall returns | PAL normalises to `int64_t` + `Error`. |
| Alignment on strict-alignment targets | No unaligned loads outside `#if BURROW_UNALIGNED_OK`. |
| 64-bit atomics on 32-bit machines | They fall back to a table of spin locks keyed on the address, as Go does. The decision is made on pointer width rather than on what the compiler claims, because the builtins will otherwise compile a 64-bit load into a call into `libatomic` and give the library a link-time dependency that only bites on one platform. `-DBURROW_ATOMIC_FORCE_LOCK64=1` takes that path anywhere, which is how it gets tested. |
| Denormals, FMA contraction, `-ffast-math` | `burrow` compiles with `-ffp-contract=off` and rejects `-ffast-math`; `math` and `strconv` tests are bit-exact. |
| Stack size for the main thread | Deep-recursion packages get explicit limits; the scheduler allocates its own goroutine stacks. |
| TLS model on Android/musl/static binaries | `_Thread_local` only; no `__thread` tricks, no `pthread_key_t` in hot paths. |
| MSVC's lack of `_Atomic` | `burrow/atomic.h` has three backends behind one set of `burrow__atomic_*` functions: the `__atomic` builtins on GCC and Clang, `Interlocked*` plus `__iso_volatile_*` on MSVC, and `<stdatomic.h>` through a cast as a last resort. The builtins rather than `<stdatomic.h>` on the two compilers that have both, because they work on ordinary objects, which is what lets a caller pass a plain `uint32_t *` instead of a wrapper struct. The public `sync_atomic_*` set is written on top. |
| `%zu`, `%lld` portability in MSVC's printf | `burrow` never uses the platform printf in library code; `fmt` is our own. |

## 8. Build without a build system

The load-bearing promise:

```sh
cc -std=c11 -O2 -c burrow.c
cc -std=c11 -O2 app.c burrow.o -o app          # plus -lm -lpthread on Unix
```

No `configure`. No feature probing at build time. Platform detection is
entirely preprocessor-based, in one header (`go/platform.h`, ~400 lines), keyed
off the standard predefined macros (`__linux__`, `__APPLE__`, `_WIN32`,
`__FreeBSD__`, `__wasi__`, `__COSMOPOLITAN__`, `__x86_64__`, `__aarch64__`, …).

Optional capabilities that cannot be detected from macros — `io_uring`,
`statx`, `getrandom`, `pidfd`, `copy_file_range` — are resolved at **runtime**,
not build time, with a probe-once-and-cache pattern. This is also how Go does
it, and it is what lets one binary run on an old kernel and a new one. It costs
a branch and it is the reason there is no `configure`.

A `CMakeLists.txt`, a `Makefile`, a `meson.build` and a `build.zig` are all
provided for people who want them. None is required, and none is the canonical
build. → [15](15-build-deploy.md) §4

## Sources

- [Compiler support for C23 — cppreference](https://en.cppreference.com/c/compiler_support/23) ·
  [C23 — cppreference](https://cppreference.com/c/23)
- [State of C 2026](https://devnewsletter.com/p/state-of-c-2026/)
- [C23 Features Explained (2026)](https://netalith.com/blogs/c-tutorial/modern-c23-standard-features-syntax-changes-2026)
- [cppstat — C/C++ compiler support tracker](https://cppstat.dev/)
- [C++23 Support in MSVC Build Tools 14.51](https://devblogs.microsoft.com/cppblog/c23-support-in-msvc-build-tools-14-51/)
