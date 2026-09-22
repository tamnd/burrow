# 10 — Tier 2: the OS layer

Seventeen packages, 202,257 lines, 14,195 exported declarations — and almost
all of the declaration count is generated constant tables. The real content is
`os` (11,765 lines), `time` (6,593), `os/exec` (1,937), `os/user` (1,722),
`path/filepath` (1,348) and `os/signal` (723), sitting on a platform
abstraction layer that we design from scratch.

The governing decision here is architectural: **Go exposes `syscall` publicly
with 4,149 platform-specific declarations, and we must reproduce that for
fidelity — but no `burrow` code above Tier −1 may call it.** Our own internals
route exclusively through a small PAL. Go does the same thing internally
(`internal/poll`, `internal/syscall/unix`) and it is the only structure that
keeps a 47-platform matrix maintainable.

## 1. The two-layer structure

```
   os  os/exec  os/signal  os/user  io/fs  path/filepath  time  net  ...
                              │
                    ┌─────────▼─────────┐
                    │   PAL  (Tier −1)  │   ~40 entry points
                    │   pal_*       │   pure C, no Go concepts
                    └─────────┬─────────┘
       ┌──────────┬───────────┼───────────┬──────────┬─────────┐
    linux      darwin      windows      bsd       wasi      cosmo
   (+io_uring) (kqueue)    (IOCP)      (kqueue)  (poll)    (portable)

   syscall  ── a *public, generated, faithful* package that nothing depends on
```

`syscall` becomes a leaf, not a foundation. It exists because Go's API includes
it and because users' ported code will call it; it is generated, it is tested
against Go's own `syscall` on each platform, and removing it from `burrow`'s
internal dependency graph means a bug in `syscall` cannot break `os`.

## 2. The PAL

Seventy six entry points, grouped. Each returns `int64_t` or a `bool` plus a
`PalErrno`, which the caller maps to an `Error`. No Go types cross this
boundary, no `Str` and no `Slice`, because the PAL has to be trivially
auditable one platform at a time.

The boundary is declared in [`include/burrow/pal.h`](../../include/burrow/pal.h)
and the table below is what it declares. `tools/check-pal.sh` is what keeps the
first sentence of that header true: it fails a file outside `src/pal/` that
includes a system header or defines a feature test macro, and it fails an entry
in `tools/pal-exceptions.txt` that has stopped needing to be there.

| Group | Entry points |
| --- | --- |
| Files | `open`, `close`, `read`, `pread`, `write`, `pwrite`, `seek`, `fsync`, `ftruncate`, `stat`, `lstat`, `fstat`, `unlink`, `rename`, `mkdir`, `rmdir`, `readdir`, `link`, `symlink`, `readlink`, `chmod`, `chown`, `utimes`, `dup`, `pipe`, `mmap`, `munmap` |
| Process | `spawn`, `wait`, `kill`, `getpid`, `exit`, `environ`, `chdir`, `getcwd`, `exec_lookup` |
| Threads | `thread_create`, `thread_join`, `thread_self`, `futex_wait`, `futex_wake`, `cpu_count` |
| Time | `clock_realtime`, `clock_monotonic`, `nanosleep`, `tz_load` |
| Net | `socket`, `bind`, `listen`, `accept`, `connect`, `sendto`, `recvfrom`, `getsockopt`, `setsockopt`, `shutdown`, `getaddrinfo`, `if_enumerate` |
| Poll | `poll_create`, `poll_add`, `poll_del`, `poll_wait`, `poll_break` |
| Memory | `vm_reserve`, `vm_commit`, `vm_decommit`, `vm_release`, `vm_guard` |
| Misc | `random_bytes`, `signal_install`, `signal_mask`, `dl_open`, `dl_sym`, `page_size`, `hostname`, `user_lookup` |

That is 27 files, 9 process, 6 threads, 4 time, 12 net, 5 poll, 5 memory and 8
misc. An earlier draft of this section said forty and then said sixty eight, both
of which were counts from before the table had finished growing. Seventy six is
what is written above and it is still small enough that a new platform is a week
of work rather than a port.

`poll_break` is the newest of them and how it got there is worth a sentence. The
table had four poll entry points and no way to end a wait early, which is a
thing every one of the three backends had been doing since the netpoller was
written. Nobody noticed until the netpoller actually moved down here, which is
the argument for moving working code behind a boundary rather than declaring the
boundary and calling it done: the declarations were checked against a design and
the design was short a call.

Not all seventy six have a backend today. All of them are declared, because the
shape of the boundary is worth deciding once rather than discovering package by
package, and because a call to one that is missing is a link error that names it.
Time, memory, random, the machine queries and poll are implemented and in use.
Files, process, threads and net land with the packages that need them, where
there is a real caller to design against.

Poll is the one group whose two shapes are not hidden. A readiness backend says
a descriptor is worth trying and a completion backend says an operation has
finished, and no amount of wrapping makes those the same deal for a caller, so
the header says which one the build got and the runtime has a file for each.
`PalOverlapped` is there for the same reason: it is the completion's own
structure, declared in `pal.h` so that the code above the boundary can lay out
an operation without including `windows.h`, with a `_Static_assert` on each side
tying it to the real thing.

Three PAL rules:

1. **No feature detection at build time.** Optional kernel facilities —
   `io_uring`, `statx`, `getrandom`, `pidfd_open`, `copy_file_range`,
   `openat2`, `clone3` — are probed once at runtime and cached, exactly as Go
   does. This is what lets one binary run on a 4.x and a 6.x kernel and is why
   [03](03-c-dialect.md) §8 can promise no `configure`.
2. **No libc dependency where a syscall will do**, on Linux. Direct syscalls
   (via `syscall(2)` or inline asm) make static musl builds, Alpine
   containers, and `vfork`-based `spawn` all behave. On macOS, libSystem is
   mandatory and we use it. On Windows, Win32 only — never the CRT's POSIX
   emulation layer, which has the wrong semantics for almost everything.
3. **One file per platform per group**, never `#ifdef` forests inside a
   function. `src/pal/file_linux.c`, `src/pal/file_windows.c`, and so on. Where
   two platforms want the same code the file is shared and named for the family
   rather than copied, which is what `src/pal/vm_posix.c` is. Either way the
   whole file sits under one `#if` and a reader can tell what runs on their
   machine by looking at the top of it.

## 3. Platform support commitments

Go supports 47 GOOS/GOARCH pairs, 8 first-class. `burrow`'s commitment ladder:

| Level | Platforms | Promise |
| --- | --- | --- |
| **Primary** | linux/{amd64,arm64}, darwin/arm64, windows/amd64 | Full test suite green in CI on every PR. Release blocker. |
| **Secondary** | darwin/amd64, windows/arm64, linux/{386,riscv64,ppc64le,s390x}, freebsd/amd64 | Full suite nightly. Release blocker for the suite, not for benchmarks. |
| **Tertiary** | openbsd, netbsd, dragonfly, illumos, solaris, aix, android, ios | Weekly; community-maintained; documented gaps allowed. |
| **Special** | wasip1/wasm, Cosmopolitan APE | Reduced suite (§9). |
| **Not supported** | js/wasm, plan9 | Out of scope. → [01](01-scope.md) §7 |

`linux/s390x` is on the Secondary list specifically because it is big-endian
and catches an entire bug class the other platforms cannot. → [03](03-c-dialect.md) §2

## 4. `os` (285 decls, 11,765 lines)

Go's `os` is split across 165 files, 52 of them platform-specific, which is a
good indication of where the difficulty lies. Notes on the parts that a naive
port gets wrong:

- **`os.File` is not a file descriptor.** It carries a finaliser, a
  `poll.FD` with a reference count and a close-once guard, and a directory
  read-state. `burrow`'s `OsFile` carries the same plus its allocator. The
  refcounting matters: a `Read` racing a `Close` must not use a reused fd, and
  Go's `poll.FD` exists precisely to prevent that. Port the mechanism, not just
  the API.
- **`os.FileMode`** is Go's own portable permission/type abstraction, not the
  platform's `mode_t`. `ModeDir`, `ModeSymlink`, `ModeNamedPipe`, `ModeSocket`,
  `ModeSetuid`, `ModeSticky`, `ModeIrregular` and the Windows mapping (where
  only the read-only bit is real) are all specified and tested.
- **`os.FileInfo`/`fs.FileInfo`** must expose `ModTime` with the platform's
  actual resolution and `Sys()` returning the platform stat struct — which is
  one of the few places a `syscall` type legitimately crosses into `os`.
- **`os.ReadDir`/`File.ReadDir`** returns `DirEntry`, which lazily stats. The
  lazy behaviour is observable (a `DirEntry` for a deleted file can still
  report its name) and `getdents`-based implementations must supply the type
  byte where the platform provides it (`d_type`) and fall back to `lstat` where
  it does not.
- **`os.Stdin`/`Stdout`/`Stderr`** are package-level `*File` vars, initialised
  before `main`. In C that is a constructor-attribute or first-use
  initialisation; we use first-use with an atomic guard, so there is no static
  initialisation order problem and no dependency on constructor support.
- **`os.Getenv`/`Setenv`/`Environ`** must be consistent with what a spawned
  child sees, and on Windows the environment is UTF-16 and case-insensitive.
  `burrow` maintains its own environment copy (as Go does) with a mutex,
  syncing to the platform on `Setenv`.
- **Atomicity and error mapping.** `os.ErrNotExist`, `ErrExist`,
  `ErrPermission`, `ErrClosed`, `ErrDeadlineExceeded` and
  `errors.Is(err, fs.ErrNotExist)` working across the whole library depend on a
  single, correct errno→sentinel table per platform. This table is where
  `os`'s 17,749 lines of tests spend most of their effort, and it is the single
  highest-value thing to get right in Tier 2.

## 5. `syscall` (8,656 decls, 161,607 lines) — generated

Not hand-written. A generator, `burrow-gen syscall`, reads Go's own
`zerrors_*.go`, `ztypes_*.go`, `zsyscall_*.go` and `zsysnum_*.go` files —
which Go itself generates from system headers via `mkerrors.sh` and `cgo -godefs`
— and emits C headers with identical constant values, identical struct layouts
and wrapper functions.

This is a translation task on a well-structured input, and its correctness is
directly verifiable: a generated test program prints every constant from Go and
from C and diffs them, and a second compares `sizeof`/`offsetof` for every
struct. On a platform where those two tests pass, `syscall` is correct by
construction.

Scope discipline: generated for the Primary and Secondary platforms, on
request for Tertiary. The `syscall.Syscall`/`RawSyscall` entry points are real
on Unix; on Windows, Go's `syscall` is a different shape (`LazyDLL`,
`NewProc`, `Syscall` over `stdcall`) and is ported to match.

## 6. `time` (189 decls, 6,593 lines)

Deceptively hard, and every hard part is test-pinned.

- **`time.Time` is 24 bytes with a hidden monotonic reading.** A `Time` from
  `time.Now()` carries both wall-clock and monotonic values; subtraction uses
  the monotonic one, formatting uses the wall one, and `Round`/`Truncate`/
  serialisation strip it. Reproduce the representation exactly, including the
  `wall`/`ext`/`loc` field layout semantics, or `Sub` gives wrong answers
  across a clock adjustment.
- **The reference-layout format language.** `"2006-01-02 15:04:05.000 MST"`.
  `Format`/`Parse`/`ParseInLocation`/`AppendFormat` plus all 17 predefined
  layouts, the fractional-second handling, `-0700` vs `Z07:00` variants, and
  `Parse`'s error messages. Purely mechanical, voluminous, and 7,011 lines of
  tests.
- **Timezones.** `LoadLocation` searches, in order, `$ZONEINFO`, the platform
  zoneinfo directory, and the embedded database if `time/tzdata` is linked.
  Windows has no zoneinfo, so Go carries a registry-name→IANA-name mapping
  table, which we must too. The `tzdata` embedded database is **1,375,382 bytes
  of Go source** wrapping a 400 KB zip; ours is a `#embed` of the same
  `zoneinfo.zip`, or a generated C array on C11 toolchains, and it is optional
  exactly as Go's is. → [03](03-c-dialect.md) §1
- **Timers.** `Timer`, `Ticker`, `After`, `AfterFunc`, `Tick`, `NewTimer`,
  `Stop`/`Reset` semantics — including the Go 1.23 change making unstopped
  timers immediately eligible for collection and `Reset` on an unstopped timer
  well-defined. Implemented on the scheduler's per-P timer heaps.
  → [06](06-runtime.md) §12
- **Leap seconds, DST transitions, and the pre-1900 era** all have specific
  test cases.

## 7. `path/filepath` (32 decls) and Windows paths

`path` (Tier 1) is always `/` and is trivial. `path/filepath` is
platform-aware and Windows is where all the complexity lives:

- Drive letters, UNC paths (`\\server\share`), device paths (`\\.\`,
  `\\?\`), the `\\?\` long-path prefix, and `MAX_PATH` behaviour.
- Reserved device names (`CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`,
  `LPT1`–`LPT9`), which Go's `filepath.IsLocal` and Go 1.20+'s path-safety
  changes specifically address — security-relevant, and heavily tested after
  several CVEs.
- Case-insensitive but case-preserving comparison, and `EvalSymlinks` over
  junctions and reparse points.
- `filepath.Clean`'s exact behaviour on `.`/`..`/`//`, which differs from
  `path.Clean` on Windows.
- `filepath.Walk` vs `WalkDir` (the latter avoids a stat per entry), `Glob`,
  `Match`, `Rel`, `Abs`, `VolumeName`, `Localize` (Go 1.23+).

All Windows path handling lives in the PAL plus one `filepath_windows.c`, and
the security-relevant functions get their own fuzz target seeded with the
corpora from Go's path CVEs.

## 8. Processes, signals, users, embed

**`os/exec`** (49 decls, 1,937 lines, 3,714 test lines). The hard parts: fork
safety in a multithreaded program (use `posix_spawn` where available,
`vfork`+`execve` on Linux with the careful signal/fd handling Go does,
`CreateProcess` with correct command-line *quoting* on Windows — which is its
own minefield and has had CVEs); `PATH` lookup semantics per platform;
pipe plumbing with goroutines pumping (`StdinPipe`/`StdoutPipe`/`Output`/
`CombinedOutput`); `Cmd.Cancel` and `WaitDelay` (Go 1.20+); `ExitError` and
`ProcessState`; and Go 1.19+'s refusal to resolve a relative `PATH` entry,
which was a security fix and must be reproduced.

**`os/signal`** (6 decls, 723 lines, 1,732 test lines). Six declarations and a
genuinely difficult implementation: signals arrive on an arbitrary thread, must
be forwarded to a channel without allocating in a signal handler, and
`Notify`/`Stop`/`Reset`/`Ignore`/`NotifyContext` must compose. The design is
Go's: a signal handler writes to a self-pipe (or sets an atomic and wakes the
netpoller), and a dedicated goroutine fans out to registered channels. On
Windows, console control handlers stand in for `SIGINT`/`SIGTERM`.
`LockOSThread` interaction and `SIGURG` reservation for preemption
([06](06-runtime.md) §9) must be coordinated.

**`os/user`** (23 decls). `getpwnam_r`/`getgrnam_r` with the cgo-free fallback
of parsing `/etc/passwd` and `/etc/group` — Go maintains both paths, and
`burrow` keeps only the parsing path plus the platform API where it is
non-optional (macOS Open Directory, Windows `NetUserGetInfo`/`LookupAccountName`).
Notably simpler for us than for Go, since there is no cgo/non-cgo split.

**`embed`** (4 decls). C23 `#embed` where available:

```c
BURROW_EMBED_FILE(index_html, "static/index.html");
BURROW_EMBED_DIR(static_files, "static");       /* generator-backed */
Fs fs = embed_fs_as_fs(&static_files);
```

`BURROW_EMBED_FILE` is `#embed` on GCC 15+/Clang 19+; on MSVC and older
toolchains, `burrow-gen embed` emits a `static const unsigned char[]` plus the
directory index. The result satisfies `io/fs.FS`, `ReadDirFS` and `ReadFileFS`,
so `http.FileServerFS` and `template.ParseFS` work on it — which is the actual
point. → [01](01-scope.md) §6

**`debug/*`** (7 packages, 4,834 decls, 15,288 lines). Overwhelmingly generated
constant tables for ELF, Mach-O, PE, plan9obj and DWARF. The hand-written
content is the readers, which are mechanical, plus `debug/gosym`'s Go symbol
table and pclntab parsing and `debug/buildinfo`'s module extraction. Useful in
their own right (an ELF reader in portable C with no dependencies is a thing
people want) and they have large `testdata` corpora. Fuzz them: they parse
untrusted binaries.

`plugin` maps to `dlopen`/`LoadLibrary` with descriptor-hash type checking in
place of Go's type identity. → [01](01-scope.md) §6

## 9. wasip1 and Cosmopolitan

**wasip1/wasm** gets a reduced but honest subset. WASI preview 1 has no
threads, no `fork`, no sockets in the base spec (only preopened listeners), and
no signals. So:

- Single-threaded scheduler: `GOMAXPROCS` is 1, goroutines are cooperative
  coroutines over the single wasm stack via the Asyncify or stack-switching
  proposal, or — the simpler default — a single-threaded event loop with
  `poll_oneoff`. Channels and `select` work; true parallelism does not.
- `os/exec`, `os/signal`, `os/user`, `plugin` return
  `err_not_supported`, exactly as Go's wasip1 port does.
- `net` supports only preopened listeners and outbound where the host allows.
- The reduced suite in CI is precisely the set Go itself runs on wasip1, so
  "supported subset" has an upstream definition rather than one we invent.

**Cosmopolitan** is one more PAL backend, not a port: `pal/*_cosmo.c` uses
Cosmopolitan libc's portable surface, `poll` rather than
epoll/kqueue/IOCP, and defers OS detection to Cosmopolitan's runtime. The
payoff is `cc burrow.c app.c` with `cosmocc` producing a single binary that
runs on Linux, macOS, Windows, FreeBSD, OpenBSD and NetBSD, which is the most
compelling demo the project has. → [15](15-build-deploy.md) §6

## 10. The Tier 2 gate

Tier 2 is green when, on all four Primary platforms:

1. `os_read_file`/`WriteFile`/`Open`/`Create`/`Remove`/`Rename`/`MkdirAll`/
   `ReadDir` round-trip, with correct `os.ErrNotExist` etc. via `errors.Is`.
2. `exec` runs a child, captures both streams, and reports the exit code.
3. `Ctrl-C` produces a channel receive.
4. `time.Now()`, `Format`, `Parse`, `LoadLocation("America/New_York")` and a
   DST-boundary arithmetic case all match Go.
5. `filepath.Walk` over a tree with symlinks matches Go's traversal exactly.
6. The generated `syscall` constant/struct diff against Go is empty.
7. `testing/fstest.TestFS` passes on `os.DirFS`, `embed.FS` and `zip.Reader`.

Item 7 is the cheapest high-value check in the project: Go ships a conformance
test for the `fs.FS` interface, and passing it on three independent
implementations validates `io/fs`, `os`, `embed` and `archive/zip` at once.
