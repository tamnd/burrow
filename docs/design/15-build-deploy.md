# 15 — Build and deployment

[02](02-landscape.md) §1 established that `burrow` has no incumbent to displace
and therefore no organic adoption: nobody is looking for it, because nobody
believes it exists. Adoption has to be bought with the deployment model.

So this is the document where the project either earns its users or doesn't.
The bar is the one the user set: **SQLite**. Download one `.c` and one `.h`,
drop them in your tree, compile them with your existing build, choose which
parts you want with `#define`s, ship one binary. No `configure`, no CMake, no
`pkg-config`, no submodule, no Go toolchain, no code generator, no version
skew, no transitive dependencies. SQLite has been distributed that way for
twenty years and it is in every phone on earth.

## 1. What the user downloads

Three artefacts, per release, from a single GitHub release page:

| Artefact | Size (est.) | Contents |
| --- | --- | --- |
| `burrow-amalgamation-1.27.0.zip` | ~12 MB | `burrow.c`, `burrow.h`, `burrow-crypto.c`, `LICENSE` |
| `burrow-src-1.27.0.tar.gz` | ~25 MB | the real tree: per-package sources, headers, tests, tools |
| `burrow-testdata-1.27.0.tar.gz` | ~18 MB | Go's `testdata`, needed only to run the test suite |

And the promise, which is the headline of the README and the thing the whole
design serves:

```sh
curl -LO https://github.com/tamnd/burrow/releases/latest/download/burrow-amalgamation.zip
unzip burrow-amalgamation.zip
cc -std=c11 -O2 -c burrow.c
cc -std=c11 -O2 myapp.c burrow.o -o myapp
```

Four lines, no third line that says "first install…". On Windows:

```bat
cl /std:c11 /O2 /c burrow.c
cl /std:c11 /O2 myapp.c burrow.obj
```

That is the entire contract. Everything below exists to keep it true.

## 2. `burrow-gen amalgamate`

The amalgamation is **generated, never curated** — SQLite's rule, and the one
that keeps the real source tree navigable while the shipped artefact stays a
single file. → [02](02-landscape.md) §2

The real tree is ordinary: one directory per Go package, one `.c` per source
file, one header per package at the path the import path implies. Nobody edits
`burrow.c`; it does not exist in git.

```
burrow-gen amalgamate \
    --packages net/http,encoding/json,log/slog \
    --target linux-amd64,darwin-arm64,windows-amd64 \
    --prefix bw_ \
    --split 6 \
    --out dist/
```

What it does, in order:

1. **Resolve the package closure.** `net/http` pulls 186 transitive
   dependencies; the generator computes the closure from the same dependency
   data that drives the build order in [01](01-scope.md) §5. Asking for
   `net/http` gets you everything it needs and nothing else.
2. **Topologically order** the translation units so that the concatenation
   compiles as one TU: types before uses, no forward-declaration games.
3. **Concatenate the headers** into `burrow.h`, stripping include guards,
   deduplicating system includes to a single block at the top, and hoisting
   all `typedef`s ahead of all function declarations. This is the same
   machinery that produces the per-package headers' ordering.
   → [03](03-c-dialect.md) §6
4. **Concatenate the sources** into `burrow.c`, marking every file boundary
   with `/* ==== begin strings/strings.c ==== */` so a stack trace or a
   compiler diagnostic can be traced back to the real file, and emitting
   `#line` directives so debuggers point at the *original* paths. SQLite does
   not do the `#line` part; we should, because `burrow.c` is four times the
   size and a raw line number in it is useless.
5. **Select the platform code.** The PAL files for the requested targets are
   included; the rest are dropped. A `--target linux-amd64` amalgamation
   contains no Win32 code at all, which is both smaller and easier to audit.
   With no `--target`, all platforms are included and selected by `#if` — the
   default, and what the published artefact uses.
6. **Bake the prefix**, if asked. `--prefix bw_` rewrites every public symbol
   at generation time rather than via an aliasing header, which is the right
   choice for someone vendoring `burrow` inside a product that may link another
   copy. → [08](08-naming-abi.md) §6
7. **Emit a manifest**: `burrow-manifest.json` recording the version, the
   package list, the targets, the prefix, the generator's own git SHA, and a
   SHA-256 of every input file. Reproducible builds are a requirement, not a
   nicety: two runs of the generator on the same inputs produce byte-identical
   output, and CI checks it.

### Compile-time selection

For users who take the full amalgamation and want to trim it, the same
selection is available as `#define`s, SQLite-style:

```c
#define BURROW_OMIT_CRYPTO      1
#define BURROW_OMIT_NET         1
#define BURROW_OMIT_REFLECT     1
#define BURROW_OMIT_GOROUTINES  1   /* single-threaded; no scheduler, no stacks */
#define BURROW_ENABLE_ASSERT    1
#include "burrow.h"
```

`--packages` is better — it produces a smaller file that compiles faster — but
`BURROW_OMIT_*` lets a single vendored copy serve several consumers. Both are
supported, both are tested, and the CI matrix builds a representative set of
`BURROW_OMIT_*` combinations to make sure the `#if` skeleton has not rotted.
The generator itself emits the omission guards, so they cannot drift out of
sync with the package graph.

## 3. The split form, and why crypto is separate

Two reasons the shipped artefact is not literally one `.c` file:

**Line numbers.** `burrow.c` at full scope is roughly 400,000 lines. Some
debuggers and older toolchains mishandle line numbers above 32,768, and MSVC's
compile time on a single 400k-line TU is unpleasant. So `--split N` emits
`burrow-1.c` … `burrow-N.c`, each a valid TU, sharing `burrow.h`. **The split
form is the default for the full build**; the single-file form is the default
for a trimmed `--packages` selection where it stays under ~50k lines.
→ [02](02-landscape.md) §2

**Compiler flags.** `crypto/subtle` and the constant-time primitives must be
compiled with optimisation barriers that differ per compiler, and on MSVC that
requires `#pragma optimize("", off)` around a whole translation unit. So
`burrow-crypto.c` is always a separate file, always documented as such, and
carries its own required-flags comment at the top:

```c
/* burrow-crypto.c — compile with -O2, WITHOUT -ffast-math, WITHOUT PGO.
 * Constant-time guarantees depend on the barriers in this file.
 * Verified by ctgrind and dudect in CI; see Spec/2143/12 §3.  */
```

This is the one place the single-file promise bends, and it bends for a good
reason. → [12](12-packages-crypto.md) §3

## 4. Build systems

None is required. All are provided, because "drop it in your existing build" is
the point and people's existing builds vary:

| File | For |
| --- | --- |
| `Makefile` | POSIX make, the canonical development build |
| `CMakeLists.txt` | CMake ≥ 3.16, `FetchContent`-friendly, exports `burrow::burrow` |
| `meson.build` | Meson, with `subproject()` support |
| `build.zig` | Zig as a C toolchain — cross-compiles to everything out of the box |
| `BUILD.bazel` | Bazel, for monorepos |
| `burrow.pc.in` | pkg-config, for the system-package path |
| `vcpkg.json`, `conanfile.py`, `Formula/burrow.rb`, `PKGBUILD` | package managers |

All of these are thin wrappers that compile the same sources with the same
flags. **There is no `configure` script and no generated `config.h`**, because
all feature detection is runtime, not build-time — probe once, cache, branch.
That decision costs a branch and buys the entire deployment story: one
amalgamation runs on an old kernel and a new one, and cross-compiling requires
no target-machine introspection. → [03](03-c-dialect.md) §8

`build.zig` deserves a specific mention: `zig cc` bundles headers and libc for
every major target, so `zig build -Dtarget=aarch64-windows` cross-compiles
`burrow` from a Mac with no SDK installation. That makes it the best
cross-compilation story available and it should be documented as the
recommended path.

## 5. What you don't pay for

The corollary of "choose which parts to include" is that unused parts must cost
nothing, and in C that means being careful about what forces linkage.

- **No global constructors on the fidelity path.** Type descriptors are placed
  in linker sections and read lazily, not registered by `__attribute__
  ((constructor))`, so a descriptor for an unused type is dropped by
  `--gc-sections`. → [07](07-reflect.md) §5
- **Everything builds with `-ffunction-sections -fdata-sections`** and links
  with `--gc-sections` / `/OPT:REF`, which is documented in the README as the
  recommended flag set.
- **No hidden `reflect` dependency in `fmt`.** `BURROW_PRINT_NO_REFLECT` gives a
  printf-alike with no descriptor machinery at all (~6 KB), for firmware users
  who want `strings`, `time` and `encoding/hex` and nothing else.
  → [07](07-reflect.md) §10
- **No scheduler unless you use goroutines.** `BURROW_OMIT_GOROUTINES`
  compiles out the M-P-G machinery, the context-switch assembly, the stacks and
  the netpoller; `SyncMutex` degrades to a no-op, channels are unavailable,
  and blocking I/O is genuinely blocking. This is the mode for embedded and for
  single-threaded CLI tools, and it is a supported, tested configuration.
- **The global state list is seven items** and each has a documented
  initialisation cost; nothing else runs before `main`.
  → [03](03-c-dialect.md) §5

## 6. Single-binary deployment

Three targets worth calling out, in increasing order of how good a demo they
make.

**Static linking** is the baseline and it just works: `burrow` has no
dependencies, so `cc -static myapp.c burrow.o` on Linux with musl produces a
fully static binary. On glibc, `getaddrinfo`'s NSS modules are the usual static
linking problem, and `burrow` has the same answer Go does — set
`GODEBUG=netdns=go` and the pure resolver is used instead.
→ [11](11-packages-net.md) §1

**`cosmocc`** is the one that makes people stop and look. Cosmopolitan libc is
one more PAL backend ([10](10-packages-os.md) §9), not a port, so:

```sh
cosmocc -std=c11 -O2 -o server burrow.c server.c
```

produces a single `server` binary that runs on Linux, macOS, Windows, FreeBSD,
OpenBSD and NetBSD, on both amd64 and arm64. An HTTP server with TLS, in one
file, that runs everywhere, with no runtime. That is a demo worth building
early, well before the library is complete, because it is the thing that makes
the project legible to someone who has thirty seconds.

**WebAssembly.** `wasip1` via `clang --target=wasm32-wasi`, which needs the
`wasip1` PAL backend and `BURROW_OMIT_GOROUTINES` (or WASI threads where
available). `js/wasm` is out of scope. → [01](01-scope.md) §7

And the composite demo, `tamnd/burrow-examples`, which is where the deployment
story becomes a claim someone can check:

- `hello-http` — an HTTP/2 server with TLS, ~40 lines.
- `sqlite-api` — `database/sql` over a 500-line SQLite driver, plus
  `sqlite3.c`, plus `burrow.c`: a JSON REST API over a real database in three
  source files and one binary. → [13](13-packages-go.md) §4
- `reverse-proxy` — `httputil.ReverseProxy`, ~20 lines.
- `cosmo-everything` — the above, built with `cosmocc`, one binary, six
  operating systems.
- `esp32-embedded` — `strings` + `time` + `encoding/json` on a microcontroller,
  proving the omission flags work.

## 7. Size

Go's floor is high because the runtime, the GC, the scheduler and type metadata
for everything reachable are always linked. `burrow` has no GC, no
compiler-emitted metadata, and per-package selection, so it should be
substantially smaller. That is the one axis where a C port has a structural
advantage rather than a structural compromise.

Measured with `tools/size.sh` on linux/amd64, in bytes. Each program is built
the way a program using the amalgamation would be, with `-O2
-ffunction-sections -fdata-sections`, linked with `--gc-sections` and
stripped. It is built twice, on the full `burrow.c` and on one made with
`--packages` and the packages listed, which bring in what they need. The Go
column is the same program in `tools/size` built with Go 1.27.3 and
`-ldflags="-s -w"`.

| Program | `--packages` | gcc 13.3 full | gcc 13.3 picked | clang 18.1 full | clang 18.1 picked | Go |
| --- | --- | --- | --- | --- | --- | --- |
| `floor`: strings, strconv, time, no fmt, no goroutines | `strings,strconv,time` | 363,056 | 182,832 | 498,600 | 191,400 | 1,388,704 |
| `hello`: `fmt.Println("hi")` | `fmt` | 330,096 | 149,872 | 469,752 | 154,360 | 1,507,488 |
| `worker_pool` from the cookbook | `fmt,sync` | 358,768 | 174,448 | 486,184 | 170,792 | - |
| `json_config` from the cookbook | `encoding/json,fmt,os` | 1,564,608 | 539,248 | 818,160 | 543,728 | - |
| `slog_logs` from the cookbook | `log/slog,os` | 1,609,920 | 592,744 | 863,440 | 589,008 | - |
| `http`: a server with one handler | `net/http,fmt` | 2,097,088 | 1,978,304 | 2,323,016 | 2,134,600 | 5,882,119 |
| `https`: the same over TLS, with HTTP/2 | `net/http,fmt` | 2,424,768 | 2,305,984 | 2,724,456 | 2,531,944 | 6,373,639 |

The whole library, the text, data and bss of `burrow.o` as size(1) prints
them, is 43,361,117 bytes with gcc and 44,283,760 with clang.

A program built on the full file is bigger because the linker can't drop
everything nothing calls. gcc puts most string constants into a few shared
sections, and the linker keeps or drops each of those whole, so one string in
use keeps the strings of every package. For `floor` that is about 180 KB of
`.rodata`, while its `.text` is the same both ways. clang leaves more than that
in the small programs. A program that uses more of the library has fewer
strings to gain, and `http` is about 2 MB either way. `net/http` needs
`crypto/tls`, so `http` and `https` start from the same packages.

These replace the projections this section had before, which put the first
row at about 120 KB, an HTTP/1 server at about 1.3 MB and a TLS server with
HTTP/2 at about 2.6 MB. Running `tools/size.sh` regenerates the table.
→ [14](14-conformance.md) §7

The `floor` row is the interesting one. It is the configuration that does not
exist in Go at all. Go cannot give you `time.Parse` and `strconv.ParseFloat`
without its whole runtime, 1.4 MB here, and `burrow` does it in 183 KB. That is
a real reason for someone to choose `burrow` over Go rather than merely
tolerate it.

## 8. Versioning and release

`burrow 1.27.x` tracks Go 1.27's API; the third component is `burrow`'s own
patch level. A new Go minor release produces a new `burrow` minor release whose
first job is to re-run the coverage gate against the new
`$GOROOT/api/go1.28.txt` and enumerate what arrived.
→ [08](08-naming-abi.md) §7

Every release ships: the three artefacts, the manifest, SHA-256 sums, a Sigstore
signature, SBOMs in SPDX and CycloneDX, the regenerated size table, the
benchmark ratios against Go, the per-package status table from
[14](14-conformance.md) §8, and the fidelity ledger with any changes marked.
Reproducibility is verified in CI by regenerating the amalgamation on a
different machine and diffing.
