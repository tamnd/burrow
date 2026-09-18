# 09 — Tier 1: pure computation

Ninety packages, 3,817 exported declarations, 134,258 lines, 154,419 lines of
tests, and not one of them touches an OS. This is where the project earns
credibility: every package here is independently portable, independently
testable, differentially fuzzable against Go, and useful on its own. It is also
where roughly a third of the total work is, and where the work is most
parallelisable.

The general method for a Tier 1 package is the same every time, and it is worth
stating once because it is applied ninety times:

1. Translate the Go source function by function, preserving structure,
   variable names and comments. Not a rewrite — a transliteration. Divergence
   from Go's structure is a defect, because it breaks the ability to re-apply
   upstream changes later.
2. Apply the mapping rules ([08](08-naming-abi.md) §1) to the signatures and
   the allocator rule ([05](05-memory.md) §2) to anything that allocates.
3. Translate the `_test.go` files, which are overwhelmingly table-driven and
   therefore mechanically translatable. → [14](14-conformance.md) §3
4. Copy `testdata/` verbatim.
5. Wire up differential fuzzing for anything that parses.
6. Run the coverage gate; the package goes green only at 100%.

What follows is the per-area detail that this generic recipe does not cover —
the places where a naive transliteration produces something that compiles and
is wrong.

## 1. Strings, bytes, and the `str`/`slice` duality

`strings` (84 decls) and `bytes` (102 decls) are near-duplicates of each other
in Go, differing only in `string` vs `[]byte`. In C they differ in `Str` vs
`Slice`, and the temptation is to implement one and alias the other. Resist:
`bytes` functions mutate in place where `strings` cannot, and `bytes.Buffer`'s
behaviour differs from `strings.Builder`'s in ways tests check. Implement the
shared core once internally (`burrow__strings_index` over a `{ptr,len}` pair),
expose both surfaces.

The interesting details:

- **`strings.Index`** is not a naive loop. Go uses a Rabin-Karp fallback with a
  bounded brute-force prefix and, on amd64/arm64, SIMD-accelerated
  `IndexByte`. Port the algorithm including the cutover constants, because the
  tests include adversarial inputs sized around them. SIMD goes in the PAL as
  an optional acceleration with a portable fallback and a differential test
  between the two.
- **Case folding.** `strings.ToUpper`/`ToLower`/`ToTitle`/`EqualFold` are
  Unicode-correct, using `unicode.SpecialCase` and the simple-fold orbit. The
  ASCII fast path exists in Go and must exist here, and the *boundary* between
  fast and slow paths is where bugs live.
- **`strings.Builder`'s copy check.** Go panics if a `Builder` is copied after
  first use, detected by an internal self-pointer. Reproduce it; it catches a
  real user error and it is tested.
- **Iterator functions** (Go 1.24+): `strings.Lines`, `SplitSeq`,
  `FieldsSeq`, `SplitAfterSeq`. These return `iter.Seq[string]`, which is the
  range-over-func construct. → [04](04-core-types.md) §1,
  [01](01-scope.md) §6

`bufio` (80 decls) is straightforward except for `Scanner`'s buffer growth and
`ErrTooLong` behaviour, and `Reader.Peek`'s exact invalidation rules — both
test-pinned.

## 2. Numbers: `math`, `math/big`, `strconv`

**`math` is not a libm wrapper**, and assuming it is will fail tests on at
least one platform. Go's `math` contains its own implementations — largely
FDLIBM-derived — of `Sin`, `Cos`, `Tan`, `Asin`, `Atan`, `Atan2`, `Exp`,
`Exp2`, `Expm1`, `Log`, `Log1p`, `Log10`, `Log2`, `Pow`, `Sqrt`, `Cbrt`,
`Hypot`, `Gamma`, `Lgamma`, `Erf`, `Erfc`, `ErfInv`, `J0`/`J1`/`Jn`,
`Y0`/`Y1`/`Yn`, and its tests check results to the last bit against a table of
expected values. Platform libm implementations differ in the last bits, and
glibc, musl, Apple's libm and MSVCRT all differ from each other.

So: port Go's implementations, compile with `-ffp-contract=off`, never accept
`-ffast-math`, and treat any bit-level divergence as a bug. The only
delegations are the ones Go itself makes to hardware instructions (`Sqrt` →
`sqrtsd`/`fsqrt`, `Abs` → bit masking), and those are exact.

Special-value behaviour — NaN, ±Inf, ±0 — is specified per function in Go's
documentation and is separately tested. It is the most common source of
divergence in a hand port.

**`strconv`** (44 decls, 1,910 lines) is the hardest small package in the
library, entirely because of float conversion:

- `ParseFloat` uses Eisel–Lemire for the common case with an exact big-decimal
  fallback, and must be correctly rounded for every input in the double range.
- `FormatFloat` with `-1` precision must produce the *shortest* representation
  that round-trips, via a Ryū-style algorithm. Not "a representation that
  round-trips" — the shortest one, which tests check exhaustively.
- `AppendFloat`, `FormatFloat` with `'b'`/`'x'`/`'p'` verbs, and
  `ParseFloat`'s hex-float support are all required.

Budget real time here, port the algorithm faithfully rather than reaching for
`strtod`/`snprintf` (which are locale-dependent and not shortest-round-trip),
and fuzz differentially over the full bit pattern space of `double` — this is
one of the packages where exhaustive-ish fuzzing is actually feasible and worth
running for CPU-weeks.

`ParseInt`/`FormatInt` are easy but have precise overflow error behaviour
(`*NumError` with `ErrRange`/`ErrSyntax`, and the returned clamped value) that
matters.

**`math/big`** (190 decls, 10,219 lines) is `Int`, `Rat` and `Float` over a
`nat` limb layer. Straightforward to port, laborious, and performance-sensitive
because `crypto/rsa`, `crypto/dsa` and `crypto/elliptic` sit on it. Karatsuba
cutovers, Montgomery reduction, and the assembly-accelerated `addVV`/`mulAddVWW`
inner loops all need C equivalents with optional per-arch assembly. `big.Float`'s
rounding modes and `Rat`'s exactness guarantees are the fidelity-critical parts.

**`math/bits`** is a direct map onto compiler intrinsics
(`__builtin_clz`/`popcount`/`_BitScanReverse`), with portable fallbacks. Cheap,
and it accelerates everything above it.

**`math/rand` and `math/rand/v2`** must reproduce Go's exact generator output
for a given seed — the ALFG for v1, PCG and ChaCha8 for v2 — because seeded
reproducibility is the point of a seeded PRNG and tests check specific
sequences. `rand/v2`'s `Uint64N` uses Lemire's bounded rejection; the exact
rejection behaviour is observable.

## 3. Unicode

`unicode` is 10,816 lines of which 10,249 is `tables.go`, generated from the
Unicode Character Database. The correct approach is obvious and must not be
deviated from: **regenerate from the same UCD input with a C emitter.**
`maketables.go` becomes `maketables_c.go`, run once per Unicode version, output
checked in.

This gives bit-identical `RangeTable`s for all 344 exported declarations
(`unicode.Letter`, `Digit`, `Han`, `Arabic`, the 160-odd scripts and
properties), identical `SimpleFold` orbits, and identical `CaseRanges` — for
free. Hand-transcribing any of it would be a guaranteed source of subtle bugs.

The Unicode *version* is pinned per `burrow` release to match the Go release it
mirrors, and is exposed as `UNICODE_VERSION`. Tests that check specific
codepoint properties then pass unchanged.

`unicode/utf8` (23 decls) and `unicode/utf16` (7 decls) are small and
delightful and should be the first two packages anyone completes: zero
dependencies, complete test suites, immediately useful, and they validate the
whole `Str` design. `utf8.DecodeRune`'s handling of invalid sequences —
returning `RuneError, 1` under specific conditions — is subtle and tested.

**What shipped.** `unicode/utf8` is done, all sixteen functions and four
constants, ported from go1.27.1. Go's two copies of every function, one for
`string` and one for `[]byte`, are one internal function over a pointer and a
length with both public spellings on top, because a `Str` and a byte `Slice`
are the same two words here and Go only has the duplication because generics
arrived after the package was written. The second return value becomes a
nullable out-parameter per [04](04-core-types.md) §9.

The prediction above held: the invalid-sequence behaviour was the whole job.
Go's test tables came across unchanged, including the thirty-six four-byte
sequences that exercise one branch each of the accept table, and the port was
then checked against Go directly with 200,000 random byte strings through both
implementations, comparing every function's output and the full range-loop
transcript. They agree byte for byte.

One optimisation is deliberately absent. Go's `Valid` skips runs of ASCII a
machine word at a time; that needs the unaligned load and the endianness
question settled in the platform layer, and it belongs there rather than
copied into this file. The behaviour is identical without it.

`unicode/utf16` is next and is the same size of job.

`golang.org/x/text`-style normalisation is not in the stdlib and is out of
scope, except for `vendor/golang.org/x/net/idna`, which `net/url` and
`net/http` need. → [11](11-packages-net.md) §2

## 4. Compression

| Package | Note |
| --- | --- |
| `compress/flate` | The hard one. §below |
| `compress/gzip`, `zlib` | Thin framing over flate; CRC/Adler checks; multistream handling |
| `compress/bzip2` | Decompress only in Go, as here. Huffman + BWT inverse + MTF |
| `compress/lzw` | Both LSB and MSB orderings; used by `image/gif` and PDF |

`compress/flate` deserves its own paragraph because the fidelity bar is higher
than it first appears: **Go's tests pin the exact output bytes** for each
compression level, not merely that the output decompresses correctly. Go's
encoder has three distinct paths — `HuffmanOnly`, a fast Snappy-derived matcher
for levels 1–3, and a lazy-matching encoder for 4–9 — each with its own
hash-table sizes, match-length limits and cutover heuristics, plus a dynamic
Huffman block-splitting heuristic. Substituting zlib or libdeflate produces
valid DEFLATE with different bytes, which fails the tests and therefore
forfeits the verification strategy. → [02](02-landscape.md) §4

Port all three paths. The decoder is comparatively easy but its error messages
(`flate.CorruptInputError` with a specific offset) are checked.

## 5. Hashing

`hash` (32), `hash/adler32`, `crc32`, `crc64`, `fnv`, `maphash`. All small, all
mechanical, with two notes:

- `crc32`'s three table strategies (simple, slicing-by-8, and the
  hardware-accelerated `SSE4.2`/`ARMv8 CRC` paths) all need to be present; the
  hardware paths go in the PAL with a differential test against the table
  implementation.
- `hash/maphash` is substrate-coupled: it must use the same process seed and
  the same hash function as `Map`, and `maphash.Comparable`/`WriteComparable`
  (Go 1.24+) need type descriptors. → [07](07-reflect.md)

## 6. Encoding

Fourteen packages, 34,524 lines, 49,663 lines of tests. Split by whether they
need reflection.

**Reflection-free, mechanical:** `hex`, `base32`, `base64`, `ascii85`, `pem`,
`csv`, `quotedprintable`, `encoding/json/jsontext`. Do these early — they are
easy wins that exercise the `Str`/`Slice`/`io.Reader` designs. `base64`'s
five predefined encodings, strict/lax padding modes and streaming
encoder/decoder invariants are the only subtlety. `pem`'s tolerance of
malformed input is precisely specified by tests.

**Reflection-dependent:** `json`, `json/v2`, `xml`, `gob`, `asn1`, `binary`.

- `encoding/json` + `json/v2` + `jsontext` is 17,714 lines with 31,933 lines of
  tests — the largest reflection consumer and the best proof that
  [07](07-reflect.md) works. Do `jsontext` (streaming tokens, reflection-free)
  first, then `json/v2` (the new options-based API), then `encoding/json` v1 as
  a compatibility layer over v2 — which is exactly how Go 1.27 now implements
  it, so following that structure is both less work and more faithful.
  `omitempty`/`omitzero`, `string` tags, embedded-struct flattening, `Marshaler`
  /`Unmarshaler`/`TextMarshaler` interface dispatch, HTML escaping,
  number-precision handling, and the exact error types are all test-pinned.
- `encoding/gob` (6,109 lines) transmits type descriptions on the wire, so it
  needs the type-name registry, not just descriptors. `gob.Register` maps onto
  `BURROW_REGISTER_TYPE`. → [07](07-reflect.md) §5
- `encoding/xml` (4,388 lines) needs namespace handling, `,any`, `,chardata`,
  `,cdata`, `,attr`, `,innerxml` tag modes, and a token-level API. Laborious,
  mechanical.
- `encoding/asn1` is small and dense, and `crypto/x509` depends on it
  absolutely — every DER parsing subtlety here becomes a certificate-parsing
  bug there. It also needs the reflection write side for `Unmarshal` into
  structs. → [12](12-packages-crypto.md) §4
- `encoding/binary` needs `Read`/`Write` over reflected structs plus the
  varint functions plus `NativeEndian`. The `Append*` and `binary.Encode`/
  `Decode` (Go 1.23+) forms too.

## 7. Archives, images, text

`archive/tar` (85 decls) and `archive/zip` (78) are format-detail exercises:
tar's GNU/PAX/USTAR/sparse variants and header round-tripping; zip's
data-descriptor handling, Zip64, the `fs.FS` adapter, and its deliberate
tolerance of real-world malformed archives. Both have large `testdata`
corpora, which is exactly what makes them safe to port.

`image` (281 decls) + `color` (78) + `draw` + `gif` + `jpeg` + `png` is 10,308
lines. Mostly mechanical. Two notes: `image/jpeg`'s IDCT and its progressive
decoder are the algorithmic content, and the *encoder's* output bytes are
pinned; `image/draw`'s `Draw`/`DrawMask` fast paths per source/destination type
pair are where the complexity is, and Go's `drawFallback` must produce
identical pixels to every fast path (which its tests check, and which becomes
our differential test for free).

`text/template` + `text/template/parse` + `html/template` is 10,331 lines and
sits at Tier 5 because it needs reflection, method calls and
`html/template`'s contextual auto-escaping — which is security-critical and has
a large, precise test suite. `text/scanner` and `text/tabwriter` are small and
independent.

`regexp` + `regexp/syntax` is 6,601 lines and must be a faithful port for the
reasons in [02](02-landscape.md) §4: the parser to a syntax tree, simplification,
compilation to a program, then the matcher triad — `onepass` for patterns that
admit it, `backtrack` for small inputs, and the Pike VM with a lazy-DFA
(`onePassProg`/`machine`) for the general case — plus leftmost-first semantics,
submatch capture, and the `RE2` syntax surface including named captures,
character classes, Unicode classes and flags. Russ Cox's
[regexp series](https://swtch.com/~rsc/regexp/) is the theory reference; Go's
source is the specification.

`index/suffixarray` is 3,110 lines of SA-IS construction plus the FM-index-ish
lookup API. Self-contained, mechanical, well-tested.

## 8. Crypto primitives (the Tier 1 subset)

Everything under `crypto/` except `tls`, `x509`, `hpke`, `fips140` and `rand`
is pure computation and belongs here. The full subtree including
`crypto/internal/fips140` is 97,376 lines, of which 12,648 is generated field
arithmetic (`nistec/fiat`) that is regenerated, not transcribed.

The Tier 1 discipline for crypto is different from everywhere else and is
covered separately in [12](12-packages-crypto.md): constant-time coding rules,
the FIPS 140-3 module boundary, test-vector sourcing, and the rule that
`burrow`'s crypto is not to be used in anger until it has had an external
review.

## 9. The rest

`sort` (40) — Go's `pdqsort` plus the stable variant, with the exact algorithm
because `sort.Slice`'s instability pattern is observable. `sort.Interface` is
the canonical interface-as-vtable example.

`slices` (40) and `maps` (10) — generic, per [08](08-naming-abi.md) §5. The
place where the generics mechanism is proved.

`container/heap`, `list`, `ring` — 42 declarations, zero dependencies, one
afternoon each, and `container/list`'s Go 1.26 generic form.

`cmp`, `structs`, `encoding` (the interface-only package), `unique`, `uuid`,
`iter`, `weak` — tiny, some substrate-coupled.

`flag` (93) — mechanical, and a good early demonstration of the library being
usable: a CLI in `burrow` with `flag` and `fmt` is a compelling demo.

`io` (107) and `io/fs` (105) — interface definitions plus a handful of
implementations (`MultiReader`, `TeeReader`, `LimitReader`, `SectionReader`,
`Pipe`, `Copy`/`CopyBuffer`/`CopyN`). Small in code, enormous in consequence:
these are the interfaces every other package composes, so the vtable convention
gets decided here and is expensive to change later. `io.Pipe` needs the
scheduler. `io/fs`'s `WalkDir`, `Glob`, `Sub` and the `ReadDirFS`/`StatFS`/
`ReadFileFS` extension-interface pattern must all work, since `archive/zip`,
`os`, `embed` and `testing/fstest` all implement them.

`mime` + `mime/multipart` + `mime/quotedprintable` — 2,767 lines. `multipart`'s
streaming reader and its `Form` file-spilling behaviour need Tier 2, so it
lands with `net/http`.

## 10. Parallelism of the work

Tier 1's defining property is that it parallelises almost perfectly. Ninety
packages, a mean of 1,490 lines each, most with fewer than 70 dependencies and
a self-contained test suite. After `io`, `errors`, `fmt` and the substrate
exist, the remaining ~85 packages can be worked independently, in any order, by
any number of workers, with the coverage gate and the differential fuzzer as
the merge criteria.

This is the phase of the project that is genuinely amenable to massive
parallel execution, and the phasing in [16](16-milestones.md) is built around
that fact.
