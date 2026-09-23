# 08 — Naming, headers, generics, ABI

The mechanical name mapping is not cosmetic. It is the mechanism by which
"100% of the Go standard library" becomes a checkable claim rather than an
aspiration: if every Go symbol has exactly one derivable C name, then coverage
is a `diff` against Go's own API manifest, and a missing symbol is a build
failure rather than a discovery.

## 0. Why there is no library prefix

Almost every C library prefixes every symbol: `sqlite3_open`,
`git_repository_open`, `curl_easy_setopt`, `stbi_load`. They do it because C
has one flat global namespace and a library that exports `open` will fight
libc. The prefix is a namespace stapled onto the front of each identifier.

**`burrow` already has a namespace: Go's package name.** `strings_contains`,
`http_listen_and_serve`, `json_marshal`, `filepath_join`, `sha256_sum256`,
`tls_dial`. That segment is not decoration — it is doing exactly the work a
library prefix does, and it is doing it better, because it distinguishes
`strings_index` from `bytes_index` as well as distinguishing both from
everything outside the library. Adding `bw_` on top would be a namespace on a
namespace, six characters of noise on every call site of a library with 23,730
of them, for no disambiguation that the package segment does not already
provide.

So there is no library prefix. Not `go_`, not `bw_`, not `burrow_`. The C name
is the Go name with a dot turned into an underscore and the case fixed:

```c
strings.Contains(s, "x")      →   strings_contains(s, S("x"))
json.Marshal(v)               →   json_marshal(a, v, &err)
http.ListenAndServe(":8080")  →   http_listen_and_serve(S(":8080"), h)
```

This is the raylib/GTK position rather than the SQLite position, and it is the
right one *for this library specifically* — because the package segment exists
and is mandatory, which is not true of raylib or GTK.

### Where a namespace is still required

Being honest about the three places the argument does not hold, because
pretending it holds everywhere is how a library becomes unlinkable:

**1. Language-level types have no package.** `string`, `int`, `error`, `map`,
`chan`, `any` are Go *builtins*; there is no package segment to inherit. And
`str`, `map`, `error`, `type` as lowercase global typedefs are exactly the
identifiers most likely to collide with user code. The answer is the GTK
convention — **types are `CamelCase`, functions are `snake_case`** — which
gives them a namespace of a different kind:

```c
Str    Slice   Map   Chan   Any   Error   Int   Byte   Rune   Type   Alloc
```

Capitalised single words are far safer in C than lowercase ones, because
lowercase is where every other library and every local variable lives. It also
means you can tell a type from a function at a glance, which plain
`snake_case`-everywhere does not give you. Package types follow the same rule:
`StringsBuilder`, `IoReader`, `HttpRequest`, `OsFile`, `Time`, `Duration`.

**2. Macros ignore every scoping mechanism C has.** A header that defines
`DEFER`, `ASSERT`, `STRUCT`, `TRY`, `ZERO` or `S` in the global preprocessor
namespace will break real programs, and no `extern "C"` or `static` saves you.
So **macros keep the `BURROW_` prefix** — `BURROW_DEFER`, `BURROW_STRUCT`,
`BURROW_S` — and `#define BURROW_SHORT` before the include enables the
unprefixed spellings for programs that want them:

```c
#define BURROW_SHORT 1
#include <burrow/net/http.h>
...
DEFER(file_close, f);
GO(worker(x, y));
Str addr = S(":8080");
```

The examples in `docs/` use the short forms with the `#define` shown at the
top, because that is what the code people actually write looks like.
Package-qualified *constants* are not in this category — `TIME_SECOND`,
`HTTP_STATUS_NOT_FOUND`, `KIND_STRUCT` carry their package and follow the
function rule.

**3. A few names collide with libc, the standard headers or libraries that
ship with every system** and are renamed, once, by an enumerated table rather
than a rule: `select` (POSIX `select(2)`) → `chan_select`; `sync/atomic`'s
package segment (`<stdatomic.h>`'s `atomic_*`) → `sync_atomic_`; `runtime`'s
park/ready primitives → `sched_park`/`sched_ready`; and Go 1.27's `uuid.Parse`
and `UUID.Compare`, which libuuid and macOS's libc already define with
different signatures, → `uuid_parse_str` and `uuid_cmp`. `burrow-gen
collisions` (below) is what finds these, and the table only grows when it
does.

### The escape hatch, which is what makes this safe

`burrow` is compiled from source by the person using it — that is the whole
deployment model — so restoring a prefix is one line, not a repackaging
exercise:

```c
#define BURROW_PREFIX bw     /* bw_strings_contains, BwStr, BW_TIME_SECOND */
```

or, at generation time, `burrow-gen amalgamate --prefix bw_`, which bakes it in
permanently and is the right choice for anyone vendoring `burrow` inside a
product that might link a second copy. → [15](15-build-deploy.md) §2

Unprefixed by default, prefixed when you need it. The pretty spelling is the
one almost everyone gets; the safe spelling is available to the few who hit a
real collision, and they will know who they are.

## 1. The mapping rules

Complete, ordered, and applied without discretion. A reference implementation of
R1–R12 is ~200 lines and is the same code the coverage gate uses, so the
mapping cannot drift from its enforcement.

**R1 — Package segment: the Go *package name*, not the import path.**
`encoding/json` → `json`, `net/http` → `http`, `path/filepath` → `filepath`,
`crypto/sha256` → `sha256`. This is the name Go source itself uses at every
call site, so `json_marshal` is what a Go programmer already reads
`json.Marshal` as.

On collision — the same package name at two import paths — prepend parent
segments until unique, and do it for *all* colliding members so no one of them
is privileged. The complete collision list for Go 1.27 is short and fixed:

| Colliding name | Resolves to |
| --- | --- |
| `rand` | `math_rand_*`, `mathrand2_*`, `crypto_rand_*` |
| `template` | `text_template_*`, `html_template_*` |
| `pprof` | `runtime_pprof_*`, `http_pprof_*` |
| `scanner` | `go_scanner_*`, `text_scanner_*` |
| `json` (v1/v2) | `json_*`, `jsonv2_*` |
| `atomic` (vs `<stdatomic.h>`) | `sync_atomic_*` |

Because the list is closed and checked in, R1 stays a pure function of the
import path.

**R2 — Identifier: CamelCase → `snake_case`** for functions, values and
methods. `ListenAndServe` → `listen_and_serve`, `ParseFloat` → `parse_float`,
`ReadFile` → `read_file`. Acronyms lower as a unit: `URL` → `url`,
`HTTPClient` → `http_client`, `ServeHTTP` → `serve_http`, `ParseIP` →
`parse_ip`. The acronym table is checked in (`URL HTTP HTTPS TLS TCP UDP IP DNS
ID API CPU IO EOF ASN1 DER PEM JSON XML UTF8 UTF16 RSA ECDSA GCM CBC SHA MD5
CRC FS DB SQL RPC MIME SMTP URI UUID PKCS OID SAN CA OCSP SCT ALPN SNI QUIC
HPACK GZIP ZIP RW`), because acronym splitting is the one part of case
conversion that cannot be inferred. `RW` is in the list for `sync.RWMutex`,
which is `sync_rw_mutex_*` and not `sync_r_w_mutex_*`.

**R3 — Functions.** `pkg.Func` → `<pkg>_<func>`.
`http.ListenAndServe` → `http_listen_and_serve`.

**R4 — Types are `CamelCase`**, package and type concatenated:
`strings.Builder` → `StringsBuilder`, `http.Request` → `HttpRequest`,
`os.File` → `OsFile`, `bufio.Reader` → `BufioReader`. **Eponym collapse:** when
the type name equals the package name case-insensitively, the duplicate is
dropped — `time.Time` → `Time`, `url.URL` → `Url`, `list.List` → `List`,
`fs.FS` → `Fs`, `ring.Ring` → `Ring`. This is the anti-stutter rule and the
only non-obvious one; it fires 23 times in the whole library and those 23 are
enumerated in the generator's table.

**R5 — Language-level types drop the package entirely**, because they have
none — they are Go's builtins and `burrow`'s substrate: `Str`, `Slice`, `Map`,
`Chan`, `Any`, `Error`, `Int`, `Uint`, `Byte`, `Rune`, `Type`, `Alloc`,
`Goroutine`. → [04](04-core-types.md)

**R6 — Methods.** `(T).M` and `(*T).M` → `<snake_case type>_<method>`, with the
receiver as the first parameter — value for value receivers, pointer for
pointer receivers. The type name snake-cases under R2, so
`(*strings.Builder).WriteString` → `strings_builder_write_string` and
`(time.Time).Add` → `time_add`. Go forbids a type having both `T.M` and `*T.M`,
so there is no collision.

The rule has exactly one exception in the whole library. `error.Error()` would
become `error_error`, a name that says the same word twice at every one of the
call sites where a message gets printed, so it is `error_text`, since
`error_message` belongs to com_err. The
generator's table carries it as a special case in both directions so the
round-trip test still closes. → [04](04-core-types.md) §6

**R7 — Consts and enum members SCREAM**, package first:
`time.Second` → `TIME_SECOND`, `os.O_RDONLY` → `OS_O_RDONLY`,
`http.StatusNotFound` → `HTTP_STATUS_NOT_FOUND`, `reflect.Struct` →
`KIND_STRUCT`.

**R8 — Package-level vars** follow R3's lowercase form and are `extern`.
**Sentinel errors** are vars, and the `Err` prefix becomes `err_`:
`io.EOF` → `io_eof`, `os.ErrNotExist` → `os_err_not_exist`,
`fs.ErrPermission` → `fs_err_permission`.

**R9 — Interfaces.** `pkg.I` → the value type `<Pkg><I>` plus the vtable type
`<Pkg><I>VT`. Vtable members are the methods under R2.
`io.Reader` → `IoReader` / `IoReaderVT`.

**R10 — Derived and generated names**, all marked so they sort beside their
origin and never collide with a Go symbol:

| Kind | Form | Example |
| --- | --- | --- |
| Result struct (3+ results) | `…Ret` | `StringsCutRet` |
| Interface adapter | `…_as_…` | `os_file_as_io_reader` |
| Variadic convenience | `…_v` | `errors_join_v` |
| Generic instantiation | `…_<T>` | `slices_sort_int64` |
| Nested/anonymous type | `…Ctx`, `…Opt` | `HttpTransportDialCtx` |

Adapters are generated for every (type, interface) pair the library relies on —
412 pairs. Nested anonymous types number 31 and are enumerated rather than
derived.

**R11 — Internal symbols** get `burrow__` (double underscore) and are `static`
wherever possible. The double underscore is the grep-able marker for "not API,
may change".

**R11a, public symbols with no Go original**, get a single `burrow_`:
`burrow_version()`, `burrow_err_out_of_memory`, `burrow_sentinel_error_vt`,
`burrow_nanotime()`.
There are few of them and they are supported API, they just have nothing in
Go's manifest to map back to. Naming them after a Go package instead would put
a symbol in that package's namespace that the coverage round trip then has to
carry an exception for, so the prefix is doing real work: it says "ours" and it
keeps `errors_`, `os_` and the rest exactly as wide as Go's.

**R12 — Generic functions** get the dispatching macro at the base name plus an
explicit-type variant per instantiation: `slices_sort(x)` (the `_Generic`
macro) and `slices_sort_int64(...)`. §5.

Worked examples across the awkward cases:

| Go | C |
| --- | --- |
| `strings.Contains(s, substr string) bool` | `bool strings_contains(Str s, Str substr)` |
| `strings.Builder` | `typedef struct StringsBuilder StringsBuilder;` |
| `(*strings.Builder).WriteString` | `Int strings_builder_write_string(StringsBuilder *, Str, Error *)` |
| `io.Reader` | `IoReader` + `IoReaderVT` |
| `io.EOF` | `extern const Error io_eof;` |
| `time.Time` | `typedef struct { … } Time;` (eponym collapse, R4) |
| `time.Duration` | `typedef int64_t Duration;` |
| `time.Second` | `#define TIME_SECOND ((Duration)1000000000)` |
| `(time.Time).Add` | `Time time_add(Time, Duration)` |
| `net/http.ListenAndServe` | `Error http_listen_and_serve(Str addr, HttpHandler h)` |
| `http.HandlerFunc` | `typedef struct {…} HttpHandlerFunc;` |
| `(http.ResponseWriter).WriteHeader` | vtable member `write_header` |
| `http.StatusNotFound` | `#define HTTP_STATUS_NOT_FOUND 404` |
| `encoding/json.Marshal` | `Slice json_marshal(Alloc *a, Any v, Error *err)` |
| `crypto/sha256.Sum256` | `Sha256Sum256Ret sha256_sum256(Slice data)` |
| `path/filepath.Join` | `Str filepath_join(Alloc *a, Slice elems)` |
| `os.FileMode` | `typedef uint32_t OsFileMode;` |
| `sync.Map` | `typedef struct SyncMap SyncMap;` |
| `sync/atomic.AddInt64` | `Int64 sync_atomic_add_int64(Int64 *, Int64)` (R1 collision) |
| `slices.Sort[S ~[]E, E cmp.Ordered](x S)` | `slices_sort(x)` macro + `slices_sort_int64(Slice)` etc. |
| `errors.Join(errs ...error) error` | `Error errors_join(Alloc *a, Slice errs)` + `errors_join_v(Alloc *a, int n, …)` |
| `error.Error() string` | `Str error_text(Error)` (R6's one exception) |

**Round-tripping is a test.** The generator implements the mapping in both
directions and CI checks that `go→c→go` is the identity over all 23,730
declarations. A rule that is not invertible is a rule that will produce a
collision eventually, and the round-trip test finds it at the moment the rule is
written rather than three packages later.

**Collision auditing is also a test.** `burrow-gen collisions` reads the symbol
tables of glibc, musl, the macOS SDK, MinGW's import libraries for the Windows
SDK and the sixty or so most common C libraries, and reports any `burrow` public
symbol that is also defined there, along with the C name of every Go
declaration not yet written, so a clash is found before the package that would
cause it. Reading symbol tables rather than linking a probe catches the case a
link does not: a static definition in the program silently answering a shared
library's own calls. It runs in CI, and `tools/collision-waivers.txt` holds the
few clashes that cannot hurt anyone, each with its reason. If the unprefixed design ever becomes untenable, that
report is how we will find out — not a user's build log.
## 2. The coverage gate

The mapping's whole purpose. `tools/burrow-coverage` in `tamnd/burrow`:

1. Read `$GOROOT/api/go1.txt` … `go1.27.txt` and take the union, keeping a
   platform specific line if it exists on linux-amd64, darwin-arm64 or
   windows-amd64. → 23,730 declarations, the same count as `tools/inventory.sh`.
2. Apply R1–R12 to each, producing the expected C name. The few places where a
   header spells a name differently on purpose are a table in the tool, each
   entry pointing at the argument for it.
3. Read the public headers under `include/burrow`, which are what `burrow.h`
   is made of.
4. Report, per package: present / missing / waived. Signatures are not compared
   yet.

```
$ tools/burrow-coverage sync sync/atomic unicode/utf8 context
sync            46/46     100.0%  done
sync/atomic     94/94     100.0%  done
unicode/utf8    23/23     100.0%  done
context         19/21      90.5%  partial
------------------------------------------
total          182/184    98.91%  4 packages, 0 waived
```

The gate is per package. `tools/coverage-done.txt` lists the packages that are
finished, and one of those missing a declaration fails CI. A package joins the
list in the pull request that finishes it, so it cannot lose a declaration
afterwards without somebody noticing. The rest are reported, which keeps the
total a trend rather than a surprise.

**Any unexplained missing symbol in a finished package fails CI.** Waivers live
in `tools/coverage-waivers.txt` with a reason and a link to the ledger entry in
[01](01-scope.md) §6; adding one requires review. That is the entire enforcement
mechanism for the project's headline claim, and it is about 500 lines of Python.

## 3. Header layout

Headers mirror Go's import paths exactly, so translation is textual:

```c
#include <burrow/strings.h>          // import "strings"
#include <burrow/net/http.h>         // import "net/http"
#include <burrow/encoding/json.h>    // import "encoding/json"
```

```
include/burrow/
  platform.h          detected platform macros
  core.h              Str, Any, Error, Int
  mem.h               Alloc, and mem/arena.h mem/heap.h … per backend
  type.h              Type, BURROW_STRUCT and friends
  slice.h             Slice, BURROW_AT, BURROW_APPEND
  runtime.h           go, Chan, chan_select, BURROW_DEFER
  all.h               everything, for the impatient
  strings.h  bytes.h  strconv.h  …                        (one per package)
  net/http.h  net/url.h  …
  encoding/json.h  …
```

`core.h` + `mem.h` + `type.h` + `slice.h` + `runtime.h` are the substrate and
are always present. They go in that order, because a Slice points at a Type and
a Type holds a Str, and a header cannot come before the one it needs. Every
package header includes exactly what it needs and nothing more.

Rules from [03](03-c-dialect.md) §6 apply: self-contained, idempotent, no
platform headers leaked, no unprefixed macros, `extern "C"` guarded.

## 4. `BURROW_SHORT`: the bare macro spellings

Functions and types are already unprefixed (§0). `BURROW_SHORT` is about the
one category that is not — macros:

```c
#define BURROW_SHORT 1
#include <burrow/net/http.h>

Str addr = S(":8080");                    /* BURROW_S            */
DEFER(os_file_close, f);                  /* BURROW_DEFER        */
GO(worker(x, y));                         /* BURROW_GO           */
SELECT { CASE_RECV(ch, v): ... }          /* BURROW_SELECT       */
STRUCT(Point, POINT_FIELDS)               /* BURROW_STRUCT       */
```

The short names are defined as additional `#define`s alongside the long ones,
never instead of them, so a header that was written against `BURROW_DEFER`
keeps working in a translation unit that turns shorts on. Names that would
shadow something common are refused: `ASSERT`, `TRY`, `CATCH`, `MIN`, `MAX`,
`ZERO`, `RANGE`, `CHECK`, `SEND` keep their prefix unconditionally, with a
comment in the header naming each one and why. There are 9 such refusals.

`BURROW_SHORT` is never defined inside `burrow` and is off by default. It is
the right choice for an application and the wrong choice for a library header,
and the documentation says so in those words. `docs/` examples use it, with the
`#define` shown at the top of every one.

A third spelling exists for tests and scripts: `BURROW_IMPLICIT_ALLOC` installs a
thread-local default allocator and defines `_a`-suffixed wrappers that omit the
allocator argument. Useful, dangerous, loudly documented, and forbidden in
library code. → [05](05-memory.md) §2

## 5. Generics

Go 1.18+ generics appear in 12 stdlib packages: `slices`, `maps`, `cmp`,
`sync/atomic` (`Pointer[T]`), `sync` (`OnceValue`), `math/rand/v2`, `iter`,
`unique`, `weak`, `container/list` (Go 1.26+), `encoding/json/v2`, `testing`.
Three mechanisms, chosen per case:

**(a) Descriptor-driven, for anything reflection can do.** `slices.Sort` on a
`Slice` whose `elem` descriptor has a `compare` op needs no templates at
all:

```c
void slices_sort_any(Slice s);           /* uses elem->ops->compare */
```

Correct, general, and slower than a monomorphised sort by roughly 2–3× because
of the indirect comparison.

**(b) `_Generic` dispatch to monomorphised instances**, for the hot cases:

```c
#define slices_sort(s) _Generic((s),                  \
    SliceInt:     slices_sort_int,                \
    SliceInt64:   slices_sort_int64,              \
    SliceFloat64: slices_sort_float64,            \
    SliceStr:     slices_sort_str,                \
    default:          slices_sort_any)(s)
```

`burrow` pre-instantiates 14 element types (the integer widths, the two floats,
`Str`, `Byte`, `Rune`) for the 40 `slices` and 10 `maps` functions.
That is 700 small generated functions, roughly 30 KB of text, all
`--gc-sections`-droppable.

**(c) A user instantiation macro**, for your own types:

```c
BURROW_SLICES_INSTANTIATE(MyItem, my_item_cmp)
/* defines SliceMyItem, slices_sort_my_item, _BinarySearch_MyItem, … */
```

The header-template pattern (a `.h` included repeatedly with `BURROW_T` defined),
which is the standard C answer and works on every compiler including MSVC.

Typed slice wrappers make (b) possible and are worth the extra types:

```c
typedef struct { Slice s; } SliceInt64;    /* distinct for _Generic */
```

A one-word struct wrapper, zero runtime cost, and the reason the ergonomics of
`slices_sort(x)` work at all. `BURROW_SLICE_OF(T)` builds them.

`cmp.Ordered` and the other constraints become documented requirements on the
instantiation macro (a comparison function must exist), checked with
`_Static_assert` where possible.

## 6. Putting a prefix back on

§0 argues the unprefixed design is right for almost everyone. "Almost" is doing
real work: a program that links `burrow` alongside another library exporting
`Error`, `Map` or `time_now` has a genuine problem, and the answer must be
better than "rename your other library".

```c
#define BURROW_PREFIX bw          /* before including anything */
```

Renames every public symbol: `bw_strings_contains`, `BwStr`, `BwStringsBuilder`,
`BW_TIME_SECOND`. Implemented by a generated aliasing header rather than by
macro-mangling the definitions, so debuggers and profilers still show real
names.

```sh
burrow-gen amalgamate --prefix bw_ ...
```

Bakes it into the generated sources permanently, which is the right choice for
someone vendoring `burrow` inside a product that might link a second copy —
there, an aliasing header is not enough, because the *definitions* must differ.
→ [15](15-build-deploy.md) §2, [18](18-legal.md) §3

Both modes are built and tested in CI, so the prefixed path cannot rot from
disuse. And `burrow-gen collisions` (§0) runs every night against glibc, musl,
the platform SDKs and the fifty most common C libraries, so the question "is
unprefixed still tenable?" has a standing, monitored answer rather than an
opinion.

## 7. ABI and versioning

**Version scheme.** `burrow` versions track the Go release they mirror:
`burrow 1.27.x` implements Go 1.27's standard library. The third component is
`burrow`'s own patch number. This makes "which Go are we compatible with" a
non-question, and it sets the maintenance cadence — a Go release every six
months, each adding on the order of 100–400 declarations.

**Go 1 compatibility inheritance.** Go's own compatibility promise means the
API only grows. `burrow` inherits that: no public symbol is ever removed or
changed incompatibly within a major line. Where Go deprecates (`io/ioutil`,
`math/rand`'s global functions), we keep the symbol and mirror Go's
deprecation notice.

**ABI stability.** Two modes, because the two use cases have opposite needs:

- **Source-embedded (default).** No ABI promise at all. You compile
  `burrow.c` with your program; structs are transparent, functions inline,
  `Str` is passed in registers. This is the fast, recommended mode, and it
  is why `Str` and `Slice` are by-value structs rather than opaque
  handles.
- **Shared library** (`libburrow.so`/`.dylib`/`.dll`). Public structs that are
  not by-value value types become opaque, a version-checked `burrow_abi_version()`
  is exported, symbol versioning maps are provided for ELF, and the by-value
  types (`Str`, `Slice`, `Any`, `Error`, interface pairs) are frozen
  — they are part of the ABI and will never change size or layout. An ABI
  conformance test using `abi-compliance-checker` runs per release.

**Struct transparency policy.** Which structs users may see the fields of is a
real decision, and the rule is: **transparent if Go's is, opaque if Go's is
not.** `http.Request`'s fields are public in Go, so they are public in C;
`strings.Builder`'s are not, so it is opaque. This keeps the mapping mechanical
and gives users exactly the access Go gives them. Transparent structs get
generated `_Static_assert`s on their size so accidental layout changes break the
build.

**Symbol visibility.** `-fvisibility=hidden` plus explicit `BURROW_API` on public
symbols. In the amalgamation, everything not public is `static`, so a single
`burrow.c` exports exactly the public surface and nothing else — which also
means the linker can discard aggressively.

## 8. C++ and other-language consumers

Not a goal, cheap to support, and the FFI case is a meaningful fraction of the
audience.

- **C++**: `extern "C"` guards everywhere; headers compile under C++17.
  Optionally, `burrow.hpp` supplies `std::string_view` ↔ `Str` conversions,
  RAII arena guards and a `std::error_code` bridge — a thin, separate,
  optional header that is not part of the tree's build.
- **Rust**: a `bindgen`-friendly header set (no macros in the API surface that
  bindgen cannot see through — this is why `BURROW_SHORT` aliases are `static
  inline` functions and why `BURROW_CALL` has a function equivalent). A
  `burrow-sys` crate is a plausible follow-on.
- **Python/Node/Ruby/Zig/Lua**: nothing needed beyond a clean C ABI in
  shared-library mode. The by-value two-word structs are the only mild
  friction; `str_ptr`/`str_len` accessors exist for FFIs that cannot
  handle struct returns.
- **Go**: `burrow` is callable from Go via cgo, which is either absurd or
  useful depending on whether you want Go's `net/http` inside a
  Cosmopolitan binary.

## 9. Documentation, generated

Go's documentation is one of the reasons its library is good, and a port that
loses it loses much of the value. What follows is the naming-and-ABI half of
it — the symbol-level mapping. The documentation system as a whole — layout,
the per-symbol contract, the compile-every-example rule and the per-package
gate — is [19](19-docs.md).

`burrow-gen doc` produces, for every symbol: the mapped C signature, the
**original Go doc comment verbatim** (with a clear attribution header per
[18](18-legal.md) §2), the lifetime sentence derived from
`BURROW_OWNS`/`BURROW_BORROWS` ([05](05-memory.md) §4), and the Go source location it
was ported from. Output as man pages, a single HTML file, and a
`compile_commands`-adjacent JSON for editor tooling.

Every example in Go's documentation (`Example*` functions, 1,017 of them) is
translated to C and **compiled and run as part of CI**, so documentation
examples cannot rot. This doubles as a substantial part of the conformance
suite. → [14](14-conformance.md) §3
