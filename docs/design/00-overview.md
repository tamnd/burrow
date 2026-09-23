# 2143 — burrow: the Go standard library, in C, complete

Take the Go standard library — all 180 public packages, all 23,730 exported
declarations — and reimplement it in C with no omissions, no "subset for
embedded", no "the interesting 20%". Ship it the way SQLite ships: one `.c`,
one `.h`, no build system required, works on every platform you care about,
and you pick which packages come along.

The bet is not that C needs another utility library. It is that **Go's standard
library is the best-designed general-purpose library in existence, and it is
currently unavailable to the largest body of software ever written.** Every
embedded target, every kernel-adjacent daemon, every codebase that cannot take
a Go runtime, every language whose FFI speaks only C — all of them are locked
out of `net/http`, `crypto/tls`, `encoding/json`, `time`, `regexp`, `os`.
Not because those designs are Go-specific, but because nobody did the work.

## The name

`tamnd/burrow`. Gophers dig burrows; a burrow is a network of tunnels that all
connect.

Runners-up considered and rejected: `gostd` (unsearchable, trademark-adjacent),
`plinth`/`bedrock`/`substrate` (generic), `libgo` (collides with gccgo's runtime
library), `warren` (second choice, still good). The brand avoids the Go
trademark deliberately. → [18](18-legal.md) §3

## The naming: no prefix at all

There is no `go_`, no `bw_`, no `burrow_` on anything you call. Most C
libraries prefix every symbol because C has one flat namespace — but **`burrow`
already has a namespace: Go's package name.** `strings_contains` is
disambiguated from `bytes_contains` and from everything outside the library by
the segment that was going to be there anyway. A library prefix on top of that
is a namespace on a namespace, six characters of noise repeated 23,730 times.

The C name is the Go name with the dot turned into an underscore and the case
fixed:

| Go | C | Rule |
| --- | --- | --- |
| `strings.Contains` | `strings_contains` | functions `snake_case`, no prefix |
| `net/http.ListenAndServe` | `http_listen_and_serve` | the *package name*, not the import path |
| `encoding/json.Marshal` | `json_marshal` | same |
| `(*strings.Builder).WriteString` | `strings_builder_write_string` | receiver first, `noun_verb` order |
| `strings.Builder` | `StringsBuilder` | types `CamelCase` — GTK convention |
| `io.Reader` | `IoReader` + `IoReaderVT` | |
| `time.Time` | `Time` | no stuttering |
| `(time.Time).Add` | `time_add` | |
| `string`, `[]T`, `error`, `any` | `Str`, `Slice`, `Error`, `Any` | builtins have no package |
| `io.EOF`, `os.ErrNotExist` | `io_eof`, `os_err_not_exist` | sentinels are values |
| `time.Second` | `TIME_SECOND` | constants SCREAM, package first |
| `http.StatusNotFound` | `HTTP_STATUS_NOT_FOUND` | |

Types are `CamelCase` and functions `snake_case` — GTK's convention — which
gives Go's builtins (`Str`, `Map`, `Error`) a namespace they otherwise lack,
keeps the dangerous lowercase words out of the global namespace, and lets you
tell a type from a call at a glance.

Two carve-outs, because pretending otherwise would produce an unlinkable
library. **Macros keep `BURROW_`** — a header that defines `DEFER`, `ASSERT` or
`S` globally breaks real programs, and no scoping mechanism in C prevents it;
`#define BURROW_SHORT` enables the bare spellings for programs that want them.
And **three names collide with libc** and are renamed by an enumerated table:
`chan_select`, `sync_atomic_*`, `sched_park`/`sched_ready`.

If you do hit a collision, `#define BURROW_PREFIX bw` — or
`burrow-gen amalgamate --prefix bw_` — puts a prefix back on everything. You
compile `burrow` from source anyway, so that is one line rather than a
repackaging exercise. Pretty by default, safe on demand.

Twelve ordered rules, no lookup table, 23,730 symbols — and they invert, so
`go→c→go` round-trips as a CI test. A Go programmer reads `burrow` code without
a manual; a C programmer reads it without wincing; and — critically —
**coverage becomes measurable by `diff`** against `$GOROOT/api/go1.*.txt`.
A CI job also links a probe binary against glibc, musl, the platform SDKs and
the fifty most common C libraries, and fails on any duplicate symbol — so if
the unprefixed design ever stops working, we find out, not a user.
→ [08](08-naming-abi.md) §0–§1

## Documentation is written with the code, not after it

A library nobody can learn is a library nobody uses, and "we'll write the docs
before 1.0" is how projects end up with none. So `docs/` is a deliverable of
every package, gated in CI alongside the tests, from the first commit:

- Every public symbol has a reference page with the C signature, Go's original
  doc comment verbatim, an explicit lifetime/allocator sentence, and **a
  compiling example**.
- Every example in `docs/` is extracted, compiled and run by CI. A doc example
  that does not compile fails the build, so the documentation cannot drift.
- Go's 1,017 `Example*` functions come across as doctests with their output
  compared, which seeds the corpus on day one.
- A package is not "done" until its docs are done — item 8 of the per-package
  gate. → [19](19-docs.md), [14](14-conformance.md) §8

## What the research established

Seven findings, each of which changed the design.

**1. The scope is 347k lines, not a million.** Headline figure for `std` is
899k non-test lines, but that includes `syscall` (161k, 95% of it generated
constant tables) and `runtime` (113k, most of which is a GC and scheduler we
replace rather than port). The portable library — the part that is actually
*algorithms and protocols* — is **347k lines across 171 packages**. That is
large but it is a known, bounded, countable quantity, and roughly a third of it
is generated tables. → [01](01-scope.md)

**2. Nobody has done this.** Not partially, not for a subset. `c_std`
reimplements *C++*'s library in C. `libmill`/`libdill` bring Go's *concurrency*
to C and stop there. TinyGo and gccgo reduce or recompile Go but still require
Go source. There is no prior art to catch up to and no incumbent to displace —
which also means no existing user base, and the adoption story has to be
carried entirely by the deployment model. → [02](02-landscape.md)

**3. The memory model is the whole design, and Zig already solved it.** Go's
API returns heap objects everywhere, freely, because a GC cleans up. Bolting a
tracing GC onto a C *library* forces a runtime on every consumer and breaks
every FFI host. Per-object `_free` means documenting ownership 23,730 times and
getting it wrong. The answer is the Zig convention: **every function that can
allocate takes an allocator as its first parameter**, uniformly, mechanically,
with no exceptions. `strings_split(Alloc *a, Str s, Str sep)`. One
sentence of rule, and it covers the entire surface. Arena is the default
allocator, Boehm GC is a drop-in for people who want Go's exact ergonomics, and
a tracking allocator is available for leak-hunting. → [05](05-memory.md)

**4. `reflect` is the one genuinely hard wall, and it is climbable.** `fmt`
`%v`, `encoding/json`, `gob`, `xml`, `text/template` and `database/sql` all
stand on runtime type information that C does not have. The answer is a type
descriptor registry plus an X-macro declaration DSL that emits the struct *and*
its descriptor from one declaration, with no external tool and no build step —
amalgamation-compatible by construction. An optional libclang-based generator
handles plain annotated structs for people who want natural syntax. `reflect`
over registered types is complete; `reflect` over an arbitrary unregistered C
struct is impossible and we say so plainly. → [07](07-reflect.md)

**5. Vendoring third-party C libraries is a trap.** Tempting to reach for
zlib, PCRE2, mbedTLS. All three are wrong. Go's `regexp` is an RE2-family
engine with linear-time guarantees and RE2 syntax — PCRE2 is a backtracker with
different syntax and different semantics, so using it forfeits fidelity
outright, and no C-native RE2 exists (RE2 is C++; CRE2 is a wrapper).
`compress/flate` has specific, test-visible output. `crypto/tls` has specific
handshake behaviour, cipher preferences and error strings. **Fidelity requires
porting Go's own implementations.** This is more work up front and vastly less
work forever after, because Go's tests then apply. → [02](02-landscape.md) §4

**6. Go's own tests are the deliverable that makes the claim credible.**
440,736 lines of test code and ~36 MB of testdata, containing decades of
accumulated edge cases, RFC vectors and regression pins. A port that does not
pass them is a rumour. Three mechanisms: mechanical translation of
table-driven tests (the dominant Go idiom, and mechanically tractable), a
differential oracle harness that drives Go and C side by side on shared
corpora, and direct reuse of `testdata` byte-for-byte. → [14](14-conformance.md)

**7. The deployment model is the product.** SQLite's amalgamation succeeded
because it aligned the distribution format with the dominant use case, and got
a 5–10% speed bonus from single-translation-unit compilation as a side effect.
`burrow` goes one step further: the amalgamation is **generated per-selection**.
`burrow-gen --pkgs=net/http,encoding/json` resolves the dependency closure and
emits exactly those packages plus their substrate as `burrow.c` + `burrow.h`.
`cosmocc` is a supported toolchain, so "one binary, every OS" is available to
anyone who wants it. → [15](15-build-deploy.md)

## The numbers

Measured against Go 1.27.1 (`darwin/arm64`, `$GOROOT/api/go1.*.txt` union,
platform variants collapsed to a single target):

| | Packages | Exported decls | Non-test LOC | Test LOC |
| --- | ---: | ---: | ---: | ---: |
| Public `std` | 180 | 23,730 | 628,165 | 440,736 |
| — less `syscall` | 179 | 15,074 | 466,558 | 436,359 |
| — less `runtime/*` | 171 | 14,836 | 346,920 | 386,585 |
| Vendored `x/net` (h2, h3, QUIC, IDNA) | 5 | — | 66,215 | — |
| **Port target** | **171 + substrate** | **~14,800** | **~413,000** | **~387,000** |

Biggest single items, which is where the schedule lives:

| Package | Decls | LOC | Test LOC |
| --- | ---: | ---: | ---: |
| `go/types` | 509 | 24,879 | 10,544 |
| `net` | 381 | 19,082 | 24,255 |
| `net/http` | 505 | 18,605 | 34,394 |
| `crypto/tls` | 352 | 14,819 | 15,225 |
| `os` | 285 | 11,765 | 17,749 |
| `unicode` | 344 | 10,816 | 1,313 |
| `math/big` | 190 | 10,219 | 11,826 |
| `reflect` | 265 | 8,844 | 12,149 |
| `crypto/x509` | 336 | 8,620 | 14,331 |

`debug/elf` (3,296 decls) and `syscall` (8,656) look enormous by declaration
count and are nearly free: both are overwhelmingly `const` tables, generated.

## The shape of it

```
                       ┌───────────────────────────────────┐
  your program  ───▶   │  #include "burrow.h"              │
                       │  cc burrow.c app.c                │
                       └────────────────┬──────────────────┘
                                        │
   ┌────────────────────────────────────────────────────────────────┐
   │  Tier 5   go/*   database/sql   text/template   testing        │
   ├────────────────────────────────────────────────────────────────┤
   │  Tier 4   crypto/tls   crypto/x509   crypto/hpke   fips140     │
   ├────────────────────────────────────────────────────────────────┤
   │  Tier 3   net   net/http (h1,h2,h3)   net/url   net/smtp       │
   ├────────────────────────────────────────────────────────────────┤
   │  Tier 2   os   io/fs   path/filepath   os/exec   time          │
   ├────────────────────────────────────────────────────────────────┤
   │  Tier 1   strings bytes strconv slices maps unicode math       │
   │           encoding/* compress/* hash/* regexp image text/*     │
   ├────────────────────────────────────────────────────────────────┤
   │  Tier 0   SUBSTRATE                                            │
   │    Str  Slice  Map  Chan  Iface  Error       │
   │    allocators · goroutines · scheduler · netpoll · select      │
   │    defer/panic/recover · type descriptors · atomics            │
   ├────────────────────────────────────────────────────────────────┤
   │  Tier -1  PAL — platform abstraction (≈40 calls)               │
   │    linux · darwin · windows · freebsd · openbsd · netbsd       │
   │    illumos · wasi · cosmopolitan                               │
   └────────────────────────────────────────────────────────────────┘
```

Tier −1 is the only code that knows what an OS is, and it is deliberately tiny
— around forty entry points. Go gets this wrong for our purposes by exposing
`syscall` as a public package with 4,149 platform-specific declarations; we
keep that surface for fidelity but route *our own* internals exclusively
through the PAL. → [10](10-packages-os.md) §2

## Repositories

| Repo | What | Licence |
| --- | --- | --- |
| `tamnd/burrow` | The library, the amalgamation generator, the headers | BSD-3-Clause |
| `tamnd/burrow-conformance` | Translated Go tests, oracle harness, corpora | BSD-3-Clause |
| `tamnd/burrow-testdata` | Go's `testdata` (36 MB) + generated vectors | as upstream |
| `tamnd/burrow-examples` | Ported Go programs that prove the API is usable | BSD-3-Clause |

BSD-3-Clause throughout and not by preference: a port of Go's source is a
derivative work of Go, which is BSD-3-Clause plus a patent grant. Matching the
upstream licence is the clean answer, and it happens to be the right licence for
something meant to be embedded everywhere. → [18](18-legal.md)

## Non-goals, stated once

- **Not a Go compiler.** No Go source is consumed at build time. `go/parser`
  and `go/types` are ported because they are library packages people want, not
  because `burrow` runs Go.
- **Not source-compatible with Go.** You cannot compile Go with this. You can
  transliterate Go to C almost mechanically, and the examples repo demonstrates
  that, but it is a human (or agent) activity.
- **Not faster than Go.** Parity is the goal; single-TU compilation and arena
  allocation should make most of it modestly faster, and the scheduler will
  initially be worse. Benchmarks are a gate, not a selling point.
- **Not a C++ library, and no C++ in the tree.** → [03](03-c-dialect.md)

## Document map

| | |
| --- | --- |
| [01-scope.md](01-scope.md) | The full 180-package inventory; tiering; the fidelity ledger |
| [02-landscape.md](02-landscape.md) | Prior art, what we borrow, what we refuse |
| [03-c-dialect.md](03-c-dialect.md) | C11/C23 baseline, compiler matrix, portability rules |
| [04-core-types.md](04-core-types.md) | `Str`, `Slice`, `Map`, interfaces, errors |
| [05-memory.md](05-memory.md) | Allocator passing, arenas, GC backend, ownership rules |
| [06-runtime.md](06-runtime.md) | Goroutines, scheduler, channels, select, defer/panic |
| [07-reflect.md](07-reflect.md) | Type descriptors, the declaration DSL, codegen |
| [08-naming-abi.md](08-naming-abi.md) | Name mapping rules, headers, generics, ABI, versioning |
| [09-packages-pure.md](09-packages-pure.md) | Tier 1: computation, encoding, compression, text |
| [10-packages-os.md](10-packages-os.md) | Tier 2: `os`, filesystems, processes, signals, the PAL |
| [11-packages-net.md](11-packages-net.md) | Tier 3: `net`, HTTP/1–3, URL, mail, RPC |
| [12-packages-crypto.md](12-packages-crypto.md) | Tier 4: primitives, TLS 1.3, X.509, FIPS, post-quantum |
| [13-packages-go.md](13-packages-go.md) | Tier 5: `go/*`, `debug/*`, `database/sql`, templates |
| [14-conformance.md](14-conformance.md) | How we prove 100%: translation, oracles, fuzzing, CI |
| [15-build-deploy.md](15-build-deploy.md) | Amalgamation generator, package selection, toolchains |
| [16-milestones.md](16-milestones.md) | Phasing, effort model, what ships when |
| [17-open-questions.md](17-open-questions.md) | What is genuinely undecided |
| [18-legal.md](18-legal.md) | Derivative-work obligations, patents, trademark |
| [19-docs.md](19-docs.md) | `docs/` as a deliverable: reference, guides, compiled examples |
| [20-windows.md](20-windows.md) | Windows paths, files, modes, links and errors, as Go does them |

## The one-paragraph version

`tamnd/burrow` is the Go standard library rewritten in C11, symbol-for-symbol,
with an unprefixed, mechanical `<pkg>_<snake_case>` / `PkgType` mapping that
reads like a native C library and makes completeness auditable against Go's own
API manifests. Memory is handled by Zig-style explicit allocator passing with
arena, GC and tracking backends. Goroutines, channels
and `select` are real, on an M:N scheduler with a readiness/completion-hybrid
netpoller. `reflect` works via a type descriptor registry fed by a
zero-dependency declaration macro. Correctness is established by porting Go's
440k lines of tests and by differential testing against Go itself. It ships as
a generated single-file amalgamation containing exactly the packages you asked
for, builds with `cc burrow.c`, and runs on nine platform families including
Cosmopolitan's build-once-run-anywhere target.
