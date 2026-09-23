# 14 — Conformance

Go's standard library ships **440,736 lines of tests**: 10,021 `Test`
functions, 1,967 `Benchmark`s, 1,017 `Example`s, 61 `Fuzz` targets, and 18.0 MB
of `testdata` across 4,183 files in 109 directories.

That corpus is the most valuable thing this project inherits, and it is worth
more than the source it tests. Anyone can write a C string library. Nobody can
write one that has survived 10,021 tests, fifteen years of production
deployment, and a documented CVE history — except by porting the one that did.

Everything in this document follows from one decision: **Go's tests are part of
the port, not an afterthought to it.** A package is not "ported" when the code
compiles. It is ported when its tests pass.

## 1. The five mechanisms

| # | Mechanism | Catches | Cost |
| --- | --- | --- | --- |
| 1 | Translated Go tests | behavioural divergence on inputs Go's authors thought of | high (the bulk of the work) |
| 2 | Differential fuzzing vs Go | divergence on inputs nobody thought of | low, after harness |
| 3 | External conformance suites | spec violations Go itself may share | very low |
| 4 | Sanitisers | memory bugs the original language made impossible | very low |
| 5 | The coverage gate | missing API | already built ([08](08-naming-abi.md) §2) |

Mechanisms 2–4 are cheap and should be built *first*, before most of the
translation work, because they turn every subsequent line of ported code into
verified code automatically. Building the differential harness in week 3
instead of month 9 is the highest-leverage scheduling decision in the plan.

## 2. Translating Go's tests

Go's tests are overwhelmingly table-driven, which is what makes this tractable:

```go
var unquoteTests = []unQuoteTest{
    {`"a"`, "a"},
    {`"\a"`, "\a"},
    {`"\xFF"`, "\xFF"},
    ...
}
```

becomes

```c
static const struct { const char *in; const char *want; } unquote_tests[] = {
    { "\"a\"",    "a"    },
    { "\"\\a\"",  "\a"   },
    { "\"\\xFF\"","\xFF" },
};
```

**This is mechanised, not hand-typed.** `burrow-gen tests` parses a Go
`_test.go` file with `go/ast`, recognises the table-driven shape (a slice-of-
struct literal consumed by a `range` loop with `t.Errorf` inside), and emits the
C equivalent plus the loop. Measured against a sample of Go's test files, the
recognisable-shape fraction is high enough to matter — call it half to two
thirds of test *lines*, concentrated in exactly the mechanical packages
(`strconv`, `strings`, `time`, `net/url`, `path`, `fmt`) where hand-translation
would be most tedious. The rest is hand-ported.

Rules:

- **`testdata` is used verbatim, never regenerated.** It lives in
  `tamnd/burrow-testdata` as a git submodule tracking Go's tree, with a script
  that re-syncs it on each Go release and reports diffs. 18 MB is nothing, and
  those files encode real-world malformed inputs nobody would reinvent.
- **Test names are preserved exactly.** `TestUnquote` becomes
  `test_strconv_TestUnquote`. When a `burrow` test fails, you can read
  Go's test to understand what it was checking, and search Go's issue tracker
  for its history.
- **A test that is skipped is recorded, not deleted.** `burrow`'s test runner
  has a `SKIP(reason)` macro; the reason is a fidelity-ledger reference or an
  issue number, and the count of skips per package is reported in CI and
  tracked on the status page. A silently dropped test is the only way this
  project can lie to itself.
- **`testing`'s API is real.** `t.Run` subtests, `t.Parallel`, `t.Cleanup`,
  `t.TempDir`, `t.Helper`, `t.Fatal` (which requires the `panic`/`recover`
  machinery from [06](06-runtime.md) §6 to unwind correctly), `testing.TB`,
  `testing/quick`, `testing/iotest`, `testing/fstest`, `testing/slogtest`,
  `testing/synctest`. Ported early — nothing else can be tested without it.
- **`Example*` functions compile and their output is compared**, which makes
  them doctests and keeps the generated documentation honest.
  → [08](08-naming-abi.md) §9

## 3. The differential oracle

The most valuable mechanism per unit of effort, and the one thing this project
has that a from-scratch C library never could: **a reference implementation
that is known correct, freely available, and mechanically interrogable.**

```
   ┌───────────────┐        same bytes         ┌───────────────┐
   │ burrow (C)    │◀────── fuzz corpus ──────▶│ Go (oracle)   │
   │ strconv_   │                           │ strconv.      │
   │ ParseFloat    │                           │ ParseFloat    │
   └───────┬───────┘                           └───────┬───────┘
           │            ┌─────────────┐                │
           └───────────▶│   compare   │◀───────────────┘
                        │ value+error │
                        └──────┬──────┘
                               │ differ → crash → libFuzzer minimises
                               ▼                  → test case committed
```

Implementation: a Go program compiled with `-buildmode=c-archive` exposing
one `oracle_<pkg>` entry point per package, linked into a libFuzzer harness
alongside `burrow`. It lives in `fuzz/`, and `fuzz/README.md` says how to run
it and how to add a target. Input is the fuzzer's byte string, decoded identically by both sides;
output is a canonical serialisation of the result *and the error*, compared
byte for byte. Each side walks the whole package over the one input and writes
a line of text per call, which is simple enough that neither side needs a
formatter the other lacks. Any divergence is a crash, which libFuzzer minimises into a
one-line reproducer that becomes a committed regression test.

Targets, in priority order — these are where "nearly right" is most likely and
most damaging:

1. `strconv` float and integer parse/format, both directions, all bases,
   all bit sizes. Eisel–Lemire and Ryū are where a hand-port goes subtly wrong.
2. `encoding/json` (v1 and v2), `encoding/gob`, `encoding/asn1`,
   `encoding/xml`, `encoding/csv`, `encoding/pem` — round-trip and parse.
3. `regexp`: same pattern, same input, same submatch indices.
4. `time`: `Parse`/`Format` across every layout and every zone in tzdata;
   arithmetic across DST transitions.
5. `compress/flate`, `gzip`, `zlib`, `bzip2`, `lzw`: decompress-equality always,
   and **compress-byte-equality** at every level, because Go's encoder output
   is a de facto format. → [09](09-packages-pure.md) §6
6. `net/url`, `net/textproto`, `net/http` request/response parsing.
7. `path`, `path/filepath` (per-platform), `io/fs` globbing.
8. `math` and `math/big`: every function, every rounding mode, denormals,
   NaN payloads, signed zero.
9. `unicode`, `strings`, `bytes`: normalisation, case folding, iteration.
10. `crypto/x509` DER parsing; `crypto` primitives against Go *and* against
    Wycheproof. → [12](12-packages-crypto.md) §5

`burrow`'s own 61 `Fuzz` targets — inherited from Go — run alongside, with
Go's seed corpora.

Requiring Go to build the *oracle* does not violate the no-Go-toolchain
promise: the oracle is a CI artefact, not a dependency of `burrow`. Users
building `burrow` never see it. → [03](03-c-dialect.md) §8

## 4. External suites

Free correctness, and in several places *better* than Go's own coverage,
because these suites test the specification rather than the implementation.
Collected from the tier documents:

| Suite | Component | Ref |
| --- | --- | --- |
| `testing/fstest.TestFS` | `os`, `embed`, `archive/zip`, `io/fs` | [10](10-packages-os.md) §10 |
| `testing/slogtest` | `log/slog` handlers | [13](13-packages-go.md) §6 |
| `gofmt` over `$GOROOT/src` | `go/printer`, `go/parser`, `text/tabwriter` | [13](13-packages-go.md) §6 |
| `h2spec` | HTTP/2, client and server | [11](11-packages-net.md) §6 |
| `quic-interop-runner` | QUIC, HTTP/3 | [11](11-packages-net.md) §6 |
| BoringSSL `runner` | TLS 1.2/1.3 | [12](12-packages-crypto.md) §5 |
| `tlsfuzzer` | TLS malformed handshakes | [12](12-packages-crypto.md) §5 |
| Wycheproof | every crypto primitive | [12](12-packages-crypto.md) §5 |
| ACVP/CAVP vectors | FIPS module algorithms | [12](12-packages-crypto.md) §8 |
| BetterTLS | `crypto/x509` path building, name constraints | [12](12-packages-crypto.md) §7 |
| Unicode UCD + `NormalizationTest.txt` | `unicode`, `x/text` | [09](09-packages-pure.md) §5 |
| IANA tzdata release tests | `time` | [10](10-packages-os.md) §6 |
| `zlib`'s and `libdeflate`'s corpora | `compress/*` | [09](09-packages-pure.md) §6 |
| Historical Go CVE reproducers | `net/http`, `net/url`, `path/filepath`, `archive/*`, `crypto/*` | [11](11-packages-net.md) §7 |

The last row deserves emphasis. Go's vulnerability database enumerates every
security bug ever found in the standard library, with a reproducer and a fix.
It is a finite list of the exact mistakes a reimplementation is most likely to
repeat. Every entry becomes a `burrow` test **before** the corresponding
package is written, so the port starts with its predecessor's scars already
healed.

## 5. Sanitisers and memory discipline

The bugs `burrow` can have that Go cannot are the ones worth the most CI time.
→ [03](03-c-dialect.md) §4

| Config | Scope | Frequency |
| --- | --- | --- |
| ASan + UBSan | full suite, Linux + macOS, GCC and Clang | every PR |
| TSan | `sync`, `runtime`, `net`, `net/http`, `database/sql` | every PR |
| MSan | full suite, Clang only | nightly |
| Valgrind memcheck | full suite | nightly |
| ctgrind (secrets as undefined) | `crypto/*` | every PR |
| `dudect` timing statistics | `crypto/subtle`, `aes`, `rsa`, `ecdsa`, `mlkem` | every PR |
| ASan + `--allocator=track` | leak and ownership-annotation validation | every PR |
| `-fstack-protector-all`, `_FORTIFY_SOURCE=3`, CFI | full suite | nightly |
| Debug allocator: poison-on-free, guard pages, red zones | full suite | every PR |

The `track` row is the one specific to this project's design: the allocator
backend that records every allocation against its `BURROW_OWNS`/`BURROW_BORROWS`
annotation, so a function that returns borrowed memory the caller then frees is
caught mechanically rather than by review. → [05](05-memory.md) §4

TSan on `runtime` requires care: the hand-written context-switch assembly is
invisible to TSan, so a run that is not told about a switch reports goroutines
against whichever thread happened to be carrying them. The mitigation is TSan's
fiber API at the switch points, which every backend goes through, so the
assembly path is tested rather than bypassed.
→ [06](06-runtime.md) §3

## 6. Benchmarks

1,967 inherited `Benchmark` functions, translated alongside the tests, reported
as ratios against Go on identical hardware. The published target is
[05](05-memory.md) §9's: parity on pure computation, within 2× on
allocation-heavy paths, and no worse than 1.5× on `net/http` throughput.
Regressions above 5% on any benchmark block a release.
→ [16](16-milestones.md) §5

Benchmarks are not a vanity metric here. They are the second line of defence
against a port that is *correct but useless* — a `Map` that passes every
test while being 10× slower than Go's is a failed port, and only the benchmarks
will say so.

## 7. CI topology

The full matrix is too expensive to run per-commit, so it is staged:

- **Per-PR, ~15 min:** Tier A compilers (GCC 13+, Clang 18+, MSVC 19.40+) on
  linux/amd64, darwin/arm64, windows/amd64; full test suite; ASan+UBSan; TSan
  on the concurrency packages; the coverage gate; the amalgamation build
  (`cc -std=c11 -O2 burrow.c`); differential fuzzing for 60 s per target.
- **Per-merge, ~2 h:** Tier B platforms (linux/arm64, linux/386,
  linux/riscv64, linux/s390x for big-endian, freebsd/amd64, darwin/amd64,
  windows/arm64, wasip1); MSan; benchmarks; `h2spec`; BoringSSL `runner`;
  Wycheproof.
- **Nightly:** Valgrind; Tier C compilers (TCC, cproc, chibiccc) build-only;
  `cosmocc`; extended fuzzing (8 h per target, corpus persisted); external
  interop matrices; `gofmt`-over-`$GOROOT` and `go/types`-over-`$GOROOT`.
- **Per-release:** the whole thing, plus `quic-interop-runner`, plus a
  from-clean amalgamation build on every documented platform, plus the binary
  size table in [15](15-build-deploy.md) §7 regenerated and diffed.

Big-endian coverage is non-negotiable and s390x is the only practical way to
get it. A byte-order bug in `encoding/binary` or `crypto` that only appears on
big-endian hardware is precisely the class of bug this project will produce.
→ [03](03-c-dialect.md) §3

## 8. What "done" means, per package

A package is green when all of the following hold, and the CI status page shows
these eight columns for all 180 rows:

1. Coverage gate: every symbol in `$GOROOT/api` present or ledger-waived.
2. All translated tests pass; skip count is zero or fully ledger-referenced.
3. All `Example*` functions compile and produce Go's exact output.
4. Differential fuzzing: 24 h cumulative, no divergence, corpus committed.
5. Clean under ASan, UBSan, MSan, TSan and `track`.
6. Benchmarks within the published ratios.
7. Green on all Tier A and Tier B platforms.
8. Docs gate: a package page exists, every exported symbol has a reference
   entry with a lifetime/allocator sentence, and every code block in that
   package's docs compiles and runs in CI. → [19](19-docs.md) §4

Nothing about this is subjective, which is the point. "100% of Go's standard
library, nothing missing" is a claim that can be checked by a script, and the
script is the deliverable that makes the claim worth making.
