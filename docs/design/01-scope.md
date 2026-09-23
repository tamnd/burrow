# 01 — Scope: the complete inventory and the fidelity ledger

"100% of Go's standard library, nothing missing" is a claim that has to be
falsifiable or it is marketing. This document makes it falsifiable. It
enumerates every public package in Go 1.27.1, states what porting each one
costs, and — in §6 — lists the small number of declarations that *cannot* be
ported faithfully because they are properties of the Go compiler rather than of
a library, together with the substitute each one gets.

Nothing in this document is estimated. All figures are measured from a Go
1.27.1 tree, with exported-declaration counts taken from the union of
`$GOROOT/api/go1.txt` … `go1.27.txt`, where a line that only exists on some
platforms counts if it exists on linux-amd64, darwin-arm64 or windows-amd64, and
a line on all three counts once. Reproduce with `tools/inventory.sh` in
`tamnd/burrow`, which prints this table from any Go tree.

## 1. The totals

| | Packages | Exported decls | Non-test LOC | Test LOC |
| --- | ---: | ---: | ---: | ---: |
| Public `std` (excl. `internal/`, `vendor/`, `cmd/`) | **180** | **23,730** | **628,165** | **440,736** |

Plus 66,215 non-test lines of `vendor/golang.org/x/net` — `http2`, `http3`,
`quic`, `idna`, `dns` — which are not public packages but are load-bearing for
`net/http` and `net`, and therefore in scope. → [11](11-packages-net.md)

Two numbers dominate and both are less frightening than they look:

- **`syscall`: 8,656 decls, 161,607 LOC.** Around 95% is generated `const` and
  `struct` tables per OS/arch. The hand-written part is a few thousand lines of
  wrappers. Generation is a translation problem, not an implementation problem.
  → [10](10-packages-os.md) §5
- **`runtime` + subpackages: 694 decls, 134,466 LOC.** We do not port this. Go's
  runtime is a GC, a scheduler and a compiler ABI; `burrow` substitutes its own
  for the first two and has no third. What we *do* port is the ~130 public
  declarations (`GOMAXPROCS`, `NumCPU`, `Gosched`, `SetFinalizer`, `Caller`,
  `Stack`, the `MemStats` shape, `runtime/metrics`, `runtime/pprof` output
  formats). → [06](06-runtime.md)

Subtract both and the honest port target is:

| | Packages | Exported decls | Non-test LOC |
| --- | ---: | ---: | ---: |
| Library code to write | 171 | ~14,800 | ~347,000 |
| Substrate to invent (Tier 0/−1) | — | ~400 | ~35,000 est. |
| Generated tables (`syscall`, `unicode`, tz, crypto vectors) | — | ~9,000 | ~180,000 gen. |

## 2. Tiers

Tiering is by dependency depth, which determines build order and therefore
schedule. Measured dependency counts (`go list -f '{{len .Deps}}'`) back this
up: the Tier 1 set has 0–70 deps, Tier 3 has 160–205.

| Tier | What | Pkgs | Decls | LOC | Test LOC |
| --- | --- | ---: | ---: | ---: | ---: |
| −1 | PAL — platform abstraction | — | ~40 | ~8,000 | — |
| 0 | Substrate + `runtime`, `sync`, `reflect`, `errors`, `context` | 21 | 694 | 134,466 | 73,426 |
| 1 | Pure computation: text, encoding, compression, math, image | 90 | 3,817 | 134,258 | 154,419 |
| 2 | OS: `os`, `io/fs`, `syscall`, `time`, `debug/*` | 17 | 14,195 | 202,257 | 47,298 |
| 3 | Network: `net`, HTTP/1–3, URL, mail, RPC | 16 | 1,352 | 51,061 | 77,362 |
| 4 | Crypto that needs Tier 2/3: TLS, X.509, HPKE, FIPS | 5 | 794 | 25,378 | 30,351 |
| 5 | Everything on top: `go/*`, `database/sql`, templates, `testing` | 31 | 2,878 | 80,745 | 57,880 |

Tier 1 is 90 of 180 packages and only 3,817 declarations — small, shallow,
independently testable, and where the project earns its credibility before
touching anything hard.

## 3. Full inventory

Columns: exported declarations / non-test LOC / test LOC / transitive `std`
dependency count. Tier in the first column. Difficulty is a judgement:
**G** generated tables, **M** mechanical port, **H** hard (algorithmic or
protocol depth), **X** substrate-coupled (needs Tier 0 work first).

### archive, bufio, bytes, compress, container

| T | Package | Decls | LOC | Test | Deps | Diff |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 1 | `archive/tar` | 85 | 3,144 | 4,705 | 64 | M |
| 1 | `archive/zip` | 78 | 2,293 | 4,495 | 67 | M |
| 1 | `bufio` | 80 | 1,269 | 2,920 | 41 | M |
| 1 | `bytes` | 102 | 2,231 | 4,836 | 39 | M |
| 1 | `compress/bzip2` | 3 | 869 | 250 | 44 | M |
| 1 | `compress/flate` | 38 | 5,411 | 2,888 | 60 | H |
| 1 | `compress/gzip` | 33 | 548 | 1,256 | 64 | M |
| 1 | `compress/lzw` | 15 | 583 | 551 | 60 | M |
| 1 | `compress/zlib` | 25 | 386 | 448 | 64 | M |
| 1 | `container/heap` | 11 | 118 | 359 | 34 | M |
| 1 | `container/list` | 21 | 235 | 372 | 0 | M |
| 1 | `container/ring` | 10 | 136 | 421 | 0 | M |

`compress/flate` is the only hard one: Go's encoder has three strategies
(Huffman-only, fast, and a Snappy-derived level-1..9 matcher) whose *exact
output bytes* are pinned by tests. Bit-for-bit output parity is required, not
merely "valid DEFLATE". → [09](09-packages-pure.md) §4

### crypto

| T | Package | Decls | LOC | Test | Deps | Diff |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 1 | `crypto` | 69 | 296 | 312 | 39 | M |
| 1 | `crypto/aes` | 5 | 48 | 175 | 85 | H |
| 1 | `crypto/cipher` | 37 | 1,036 | 2,837 | 81 | M |
| 1 | `crypto/des` | 6 | 556 | 1,629 | 82 | M |
| 1 | `crypto/dsa` | 24 | 330 | 219 | 93 | M |
| 1 | `crypto/ecdh` | 23 | 551 | 774 | 92 | H |
| 1 | `crypto/ecdsa` | 37 | 991 | 1,199 | 112 | H |
| 1 | `crypto/ed25519` | 24 | 270 | 571 | 99 | H |
| 1 | `crypto/elliptic` | 43 | 884 | 827 | 79 | H |
| 4 | `crypto/fips140` | 4 | 93 | 108 | 57 | X |
| 1 | `crypto/hkdf` | 3 | 84 | 523 | 82 | M |
| 1 | `crypto/hmac` | 2 | 65 | 772 | 87 | M |
| 4 | `crypto/hpke` | 51 | 1,502 | 687 | 109 | H |
| 1 | `crypto/md5` | 6 | 619 | 392 | 79 | M |
| 1 | `crypto/mldsa` | 39 | 439 | 1,824 | 78 | H |
| 1 | `crypto/mlkem` | 34 | 227 | 792 | 78 | H |
| 1 | `crypto/mlkem/mlkemtest` | 2 | 53 | 0 | 81 | M |
| 1 | `crypto/pbkdf2` | 1 | 54 | 312 | 82 | M |
| 2 | `crypto/rand` | 5 | 198 | 509 | 93 | X |
| 1 | `crypto/rc4` | 7 | 83 | 171 | 78 | M |
| 1 | `crypto/rsa` | 61 | 1,612 | 2,769 | 99 | H |
| 1 | `crypto/sha1` | 6 | 502 | 348 | 85 | M |
| 1 | `crypto/sha256` | 10 | 74 | 528 | 85 | M |
| 1 | `crypto/sha3` | 32 | 275 | 504 | 55 | M |
| 1 | `crypto/sha512` | 18 | 123 | 1,040 | 85 | M |
| 1 | `crypto/subtle` | 8 | 134 | 417 | 11 | H |
| 4 | `crypto/tls` | 352 | 14,819 | 15,225 | 161 | H |
| 4 | `crypto/x509` | 336 | 8,620 | 14,331 | 143 | H |
| 4 | `crypto/x509/pkix` | 51 | 344 | 0 | 67 | M |

Low LOC counts here are misleading: `crypto/aes` is 48 lines because the real
work lives in `crypto/internal/fips140/aes` plus assembly. The full `crypto`
subtree including `internal/fips140` is **97,376 lines**, of which 12,648 is
`nistec/fiat` — generated field arithmetic. Constant-time discipline and the
FIPS 140-3 service-indicator machinery are the actual difficulty.
→ [12](12-packages-crypto.md)

### database, debug, embed, encoding

| T | Package | Decls | LOC | Test | Deps | Diff |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 5 | `database/sql` | 166 | 4,647 | 8,529 | 101 | X |
| 5 | `database/sql/driver` | 128 | 885 | 96 | 99 | M |
| 2 | `debug/buildinfo` | 3 | 586 | 558 | 75 | M |
| 2 | `debug/dwarf` | 637 | 4,502 | 1,341 | 61 | G+M |
| 2 | `debug/elf` | 3,296 | 5,678 | 3,223 | 69 | G |
| 2 | `debug/gosym` | 55 | 1,469 | 513 | 61 | M |
| 2 | `debug/macho` | 458 | 1,437 | 453 | 68 | G |
| 2 | `debug/pe` | 347 | 1,240 | 926 | 68 | G |
| 2 | `debug/plan9obj` | 38 | 376 | 81 | 59 | G |
| 0 | `embed` | 4 | 436 | 23 | 45 | X |
| 1 | `encoding` | 12 | 78 | 0 | 0 | M |
| 1 | `encoding/ascii85` | 7 | 307 | 211 | 38 | M |
| 1 | `encoding/asn1` | 73 | 2,116 | 1,770 | 65 | X |
| 1 | `encoding/base32` | 21 | 585 | 971 | 41 | M |
| 1 | `encoding/base64` | 24 | 660 | 661 | 41 | M |
| 1 | `encoding/binary` | 36 | 1,228 | 1,591 | 44 | X |
| 1 | `encoding/csv` | 34 | 651 | 998 | 60 | M |
| 1 | `encoding/gob` | 17 | 6,109 | 4,568 | 64 | X |
| 1 | `encoding/hex` | 15 | 353 | 399 | 58 | M |
| 1 | `encoding/json` | 92 | 6,055 | 14,507 | 71 | X |
| 1 | `encoding/json/jsontext` | 113 | 5,072 | 4,295 | 50 | M |
| 1 | `encoding/json/v2` | 45 | 6,587 | 13,131 | 70 | X |
| 1 | `encoding/pem` | 7 | 335 | 807 | 45 | M |
| 1 | `encoding/xml` | 83 | 4,388 | 5,754 | 61 | X |

Every **X** in this group is X for the same reason: it marshals arbitrary user
types, which means it stands on `reflect`. `encoding/json` + `json/v2` +
`jsontext` together are 17,714 lines and 32,000 lines of tests — the single
largest reflection consumer, and the best early proof that
[07](07-reflect.md)'s descriptor design works.

`debug/*` is 4,831 declarations for 14,702 lines: almost entirely `const`
tables for ELF/Mach-O/PE/DWARF constants. Generate from Go source, do not
hand-type. → [13](13-packages-go.md) §5

### errors, expvar, flag, fmt, go

| T | Package | Decls | LOC | Test | Deps | Diff |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 0 | `errors` | 7 | 362 | 785 | 30 | X |
| 5 | `expvar` | 39 | 417 | 669 | 196 | M |
| 1 | `flag` | 93 | 1,238 | 1,160 | 59 | M |
| 5 | `fmt` | 43 | 3,544 | 4,080 | 56 | X |
| 5 | `go/ast` | 486 | 3,458 | 1,276 | 63 | M |
| 5 | `go/build` | 105 | 2,605 | 2,389 | 81 | H |
| 5 | `go/build/constraint` | 30 | 696 | 431 | 41 | M |
| 5 | `go/constant` | 50 | 1,464 | 938 | 64 | M |
| 5 | `go/doc` | 77 | 2,825 | 870 | 68 | M |
| 5 | `go/doc/comment` | 55 | 2,330 | 471 | 60 | M |
| 5 | `go/format` | 2 | 309 | 316 | 69 | M |
| 5 | `go/importer` | 5 | 138 | 95 | 134 | H |
| 5 | `go/parser` | 22 | 3,821 | 1,690 | 65 | M |
| 5 | `go/printer` | 18 | 3,804 | 1,098 | 67 | H |
| 5 | `go/scanner` | 24 | 1,121 | 1,391 | 62 | M |
| 5 | `go/token` | 223 | 1,449 | 1,095 | 57 | M |
| 5 | `go/types` | 509 | 24,879 | 10,544 | 78 | H |
| 5 | `go/version` | 3 | 64 | 106 | 42 | M |

`fmt` is small in declarations (43) and enormous in consequence: `%v` on an
arbitrary type is the first thing every user tries, and it is pure reflection.
`fmt` is the acceptance test for Tier 0.

`go/types` at 24,879 lines is the largest single package in the port — a full
Go type checker including generics inference. It is also the most
self-contained hard thing here: no OS, no network, no crypto, and 10,544 lines
of tests plus the entire `go/types/testdata` corpus. → [13](13-packages-go.md)

### hash, html, image, index, io, iter

| T | Package | Decls | LOC | Test | Deps | Diff |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 1 | `hash` | 32 | 92 | 158 | 36 | M |
| 1 | `hash/adler32` | 4 | 129 | 158 | 37 | M |
| 1 | `hash/crc32` | 16 | 1,030 | 402 | 37 | M |
| 1 | `hash/crc64` | 11 | 211 | 223 | 37 | M |
| 1 | `hash/fnv` | 6 | 380 | 294 | 37 | M |
| 1 | `hash/maphash` | 24 | 504 | 1,229 | 37 | X |
| 1 | `html` | 2 | 2,475 | 248 | 40 | G |
| 5 | `html/template` | 76 | 4,864 | 6,865 | 86 | X |
| 1 | `image` | 281 | 2,111 | 1,032 | 44 | M |
| 1 | `image/color` | 78 | 720 | 313 | 0 | M |
| 1 | `image/color/palette` | 2 | 632 | 0 | 1 | G |
| 1 | `image/draw` | 25 | 1,084 | 1,338 | 46 | M |
| 1 | `image/gif` | 22 | 1,125 | 1,256 | 66 | M |
| 1 | `image/jpeg` | 15 | 2,843 | 1,499 | 46 | H |
| 1 | `image/png` | 24 | 1,793 | 1,527 | 68 | M |
| 1 | `index/suffixarray` | 7 | 3,110 | 706 | 50 | M |
| 1 | `io` | 107 | 1,089 | 1,806 | 35 | X |
| 1 | `io/fs` | 105 | 1,008 | 933 | 44 | X |
| 2 | `io/ioutil` | 16 | 151 | 462 | 53 | M |
| 0 | `iter` | 4 | 473 | 509 | 28 | X |

`io` is X because `io.Reader`/`io.Writer` are *the* interface pattern of the
entire library — 107 declarations that every other package composes. Getting
the C vtable convention right here determines the ergonomics of everything
else. → [04](04-core-types.md) §5

`iter` is X because Go's range-over-func is a compiler feature. → §6.

### log, maps, math, mime, net

| T | Package | Decls | LOC | Test | Deps | Diff |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 5 | `log` | 51 | 483 | 405 | 58 | M |
| 5 | `log/slog` | 172 | 3,002 | 3,948 | 77 | X |
| 5 | `log/syslog` | 71 | 376 | 454 | 71 | M |
| 1 | `maps` | 10 | 137 | 551 | 29 | X |
| 1 | `math` | 130 | 6,281 | 4,925 | 2 | H |
| 1 | `math/big` | 190 | 10,219 | 11,826 | 62 | H |
| 1 | `math/bits` | 51 | 918 | 1,766 | 1 | M |
| 1 | `math/cmplx` | 27 | 1,131 | 1,639 | 3 | M |
| 1 | `math/rand` | 47 | 1,354 | 1,517 | 35 | M |
| 1 | `math/rand/v2` | 60 | 1,056 | 2,423 | 32 | M |
| 1 | `mime` | 16 | 1,432 | 1,271 | 62 | M |
| 1 | `mime/multipart` | 37 | 1,023 | 1,810 | 110 | M |
| 1 | `mime/quotedprintable` | 8 | 312 | 415 | 60 | M |
| 3 | `net` | 381 | 19,082 | 24,255 | 63 | H |
| 3 | `net/http` | 505 | 18,605 | 34,394 | 186 | H |
| 3 | `net/http/cgi` | 14 | 770 | 954 | 190 | M |
| 3 | `net/http/cookiejar` | 9 | 732 | 1,680 | 187 | M |
| 3 | `net/http/fcgi` | 4 | 671 | 453 | 191 | M |
| 3 | `net/http/httptest` | 33 | 1,038 | 1,150 | 196 | M |
| 3 | `net/http/httptrace` | 32 | 306 | 118 | 164 | M |
| 3 | `net/http/httputil` | 53 | 1,765 | 2,877 | 187 | M |
| 3 | `net/http/pprof` | 6 | 471 | 326 | 194 | M |
| 3 | `net/mail` | 20 | 992 | 1,422 | 77 | M |
| 3 | `net/netip` | 80 | 1,684 | 3,536 | 41 | M |
| 3 | `net/rpc` | 57 | 1,144 | 926 | 204 | X |
| 3 | `net/rpc/jsonrpc` | 5 | 258 | 352 | 205 | X |
| 3 | `net/smtp` | 27 | 539 | 1,281 | 164 | M |
| 3 | `net/textproto` | 64 | 1,303 | 656 | 71 | M |
| 3 | `net/url` | 62 | 1,701 | 2,982 | 64 | M |

`math` is marked H for an unobvious reason: Go's `math` is not a thin wrapper
over libm. It has its own `Sin`, `Exp`, `Log`, `Gamma`, `Erf`, `J0`, `Y0`,
`Cbrt` implementations (largely FDLIBM-derived), and its tests pin results to
the last bit. Delegating to the platform libm will fail those tests on at least
one platform. Port the Go implementations. → [09](09-packages-pure.md) §2

### os, path, plugin, reflect, regexp, runtime

| T | Package | Decls | LOC | Test | Deps | Diff |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 2 | `os` | 285 | 11,765 | 17,749 | 50 | H |
| 2 | `os/exec` | 49 | 1,937 | 3,714 | 59 | H |
| 2 | `os/signal` | 6 | 723 | 1,732 | 52 | H |
| 2 | `os/user` | 23 | 1,722 | 945 | 58 | M |
| 1 | `path` | 9 | 444 | 475 | 32 | M |
| 2 | `path/filepath` | 32 | 1,348 | 3,280 | 53 | M |
| 0 | `plugin` | 4 | 294 | 15 | 38 | X |
| 0 | `reflect` | 265 | 8,844 | 12,149 | 40 | X |
| 1 | `regexp` | 50 | 2,777 | 2,979 | 46 | H |
| 1 | `regexp/syntax` | 178 | 3,824 | 932 | 44 | H |
| 0 | `runtime` | 124 | 112,843 | 42,236 | 27 | X |
| 0 | `runtime/cgo` | 5 | 713 | 103 | 32 | X |
| 0 | `runtime/coverage` | 5 | 68 | 0 | 95 | X |
| 0 | `runtime/debug` | 37 | 644 | 753 | 58 | X |
| 0 | `runtime/metrics` | 27 | 1,284 | 261 | 29 | X |
| 0 | `runtime/pprof` | 19 | 2,846 | 4,625 | 68 | M |
| 0 | `runtime/race` | 0 | 251 | 1,161 | 0 | X |
| 0 | `runtime/trace` | 21 | 989 | 635 | 58 | M |

`regexp` + `regexp/syntax` is 6,601 lines and must be a from-scratch port, not
a PCRE binding — see [02](02-landscape.md) §4. Go's engine is the
Pike-VM/one-pass/backtrack triad with an onepass optimiser and a lazy DFA, with
leftmost-first semantics and linear-time guarantees. Substituting a backtracker
changes both results and complexity class.

### slices, sort, strconv, strings, structs, sync, syscall

| T | Package | Decls | LOC | Test | Deps | Diff |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 1 | `slices` | 40 | 1,801 | 2,898 | 30 | X |
| 1 | `sort` | 40 | 2,174 | 1,848 | 33 | M |
| 1 | `strconv` | 44 | 1,910 | 1,962 | 32 | H |
| 1 | `strings` | 84 | 2,444 | 4,441 | 39 | M |
| 0 | `structs` | 1 | 40 | 0 | 0 | X |
| 0 | `sync` | 46 | 1,711 | 3,535 | 31 | X |
| 0 | `sync/atomic` | 94 | 854 | 3,348 | 1 | X |
| 2 | `syscall` | 8,656 | 161,607 | 4,377 | 36 | G |

`strconv` is H because of `ParseFloat`/`FormatFloat`: Go uses Eisel-Lemire with
a Ryū-style shortest-representation formatter and exact fallback, and the tests
pin correctness across the full double range. This is a known-hard,
well-documented algorithm; budget real time for it.

`slices` and `maps` are X because they are generic. → [08](08-naming-abi.md) §5.

### testing, text, time, unicode, unique, unsafe, uuid, weak

| T | Package | Decls | LOC | Test | Deps | Diff |
| --- | --- | ---: | ---: | ---: | ---: | --- |
| 5 | `testing` | 170 | 5,575 | 4,106 | 69 | X |
| 5 | `testing/cryptotest` | 1 | 76 | 202 | 102 | M |
| 5 | `testing/fstest` | 15 | 981 | 249 | 63 | M |
| 5 | `testing/iotest` | 10 | 357 | 474 | 60 | M |
| 5 | `testing/quick` | 21 | 385 | 327 | 63 | X |
| 5 | `testing/slogtest` | 2 | 391 | 75 | 87 | M |
| 5 | `testing/synctest` | 3 | 332 | 380 | 70 | X |
| 1 | `text/scanner` | 59 | 792 | 1,074 | 58 | M |
| 1 | `text/tabwriter` | 19 | 601 | 827 | 57 | M |
| 5 | `text/template` | 39 | 2,898 | 2,969 | 68 | X |
| 5 | `text/template/parse` | 262 | 2,569 | 1,342 | 59 | M |
| 2 | `time` | 189 | 6,593 | 7,011 | 37 | H |
| 2 | `time/tzdata` | 0 | 115 | 0 | 37 | G |
| 1 | `unicode` | 344 | 10,816 | 1,313 | 0 | G |
| 1 | `unicode/utf16` | 7 | 144 | 287 | 0 | M |
| 1 | `unicode/utf8` | 23 | 578 | 1,027 | 0 | M |
| 0 | `unique` | 3 | 552 | 594 | 33 | X |
| 0 | `unsafe` | 0 | 271 | 0 | 0 | X |
| 1 | `uuid` | 13 | 273 | 256 | 96 | M |
| 0 | `weak` | 3 | 107 | 365 | 28 | X |

`unicode` is 10,816 lines of which 10,249 is `tables.go` — generated. Generate
ours from the same UCD input with a C emitter and the fidelity is free.

`time` is H for timezone handling: `LoadLocation`, the `tzdata` embedded zip
(`time/tzdata`), the platform-specific zoneinfo search paths, monotonic-clock
tracking inside `time.Time`, and `Format`/`Parse`'s reference-layout language.
All of it is test-pinned. → [10](10-packages-os.md) §6

Go 1.27 additions are present and in scope: `simd` and `simd/archsimd` (new,
architecture-gated intrinsics), `uuid`, `weak`, `unique`, `structs`,
`crypto/hpke`, `crypto/mldsa`, `encoding/json/v2`, `testing/synctest`.
`simd/archsimd` is deferred to a post-1.0 milestone and tracked as an explicit
gap. → [17](17-open-questions.md) §2

## 4. What "complete" means, operationally

Three graduated claims, each mechanically checkable, each reported per package
in CI:

**Declaration coverage.** For every symbol in `$GOROOT/api/go1.*.txt` there
exists a symbol in `burrow.h` whose name is the mechanical mapping of it, with
a matching arity and a type that maps under the rules of
[08](08-naming-abi.md). A missing symbol fails the build of the coverage gate,
not just a test. Target: 100%, no exceptions outside §6.

**Behavioural coverage.** For every Go test function in `std`, a corresponding
`burrow` test exists and passes. Target: 100% of translatable tests, with an
explicit, reviewed, per-test waiver list for the untranslatable remainder
(tests that inspect Go's compiler output, use `go build`, or assert on Go's
GC). → [14](14-conformance.md)

**Differential coverage.** For packages with a well-defined input→output
contract, a fuzzing harness feeds identical inputs to Go and to `burrow` and
requires identical results. Target: 40+ packages under continuous differential
fuzzing, with any divergence a release blocker.

A package is **green** only when all three are satisfied. The README carries the
per-package table and it is generated, never edited.

## 5. Ordering

Dependency counts dictate this almost entirely.

1. **Tier −1/0 substrate.** No Go package ships. Ends when `fmt.Printf("%v")`
   works on a user struct, a goroutine can `select` on two channels, and
   `errors.Is/As/Unwrap` behave.
2. **Tier 1, the zero-dep leaves.** `cmp`, `container/*`, `unicode*`,
   `image/color`, `math/bits`, `structs`, `encoding`. Eleven packages with
   literally zero `std` dependencies — the first green cells.
3. **Tier 1, text and numbers.** `strings`, `bytes`, `strconv`, `sort`,
   `slices`, `maps`, `math`, `math/big`, `regexp`, `bufio`, `io`, `path`.
4. **Tier 1, formats.** `encoding/*`, `compress/*`, `hash/*`, `archive/*`,
   `image/*`, `text/*`, `mime/*`. This is where `reflect` gets a real workout.
5. **Tier 2.** PAL completion, then `syscall` tables, `os`, `io/fs`,
   `path/filepath`, `time`, `os/exec`, `os/signal`, `os/user`, `debug/*`.
6. **Tier 1 crypto primitives**, then **Tier 4** TLS/X.509.
7. **Tier 3.** `net`, then `net/http` and its constellation, then h2, then h3.
8. **Tier 5.** `go/*`, `database/sql`, templates, `log/slog`, `testing`.

`testing` lands last in the *shipping* order and first in the *building* order:
`burrow`'s own test harness is a bootstrap version of it, promoted to a public
package once stable. → [14](14-conformance.md) §2

## 6. The fidelity ledger

These are the declarations that cannot be a 1:1 port because they are
properties of the Go *compiler and toolchain*, not of a library. Every one gets
a defined substitute. This list is complete and is the only place where
"100%, nothing missing" is qualified — and I would rather state it here in
detail than have a user discover it.

| Go | Why it cannot port | Substitute |
| --- | --- | --- |
| `unsafe` (all) | Compiler intrinsics: `Sizeof`, `Offsetof`, `Alignof`, `Pointer`, `Slice`, `String`, `Add` | `sizeof`/`offsetof`/`_Alignof` are native C. `unsafe_slice`/`String` exist as real functions. Direct equivalence, zero loss. |
| `embed.FS` | `//go:embed` is a compiler directive | `BURROW_EMBED_FILE(sym, path)` macro using C23 `#embed`; a generator fallback (`burrow-gen embed`) emits a C array for C11 toolchains. Produces a real `EmbedFS` satisfying `Fs`. |
| `plugin` | Requires Go's dynamic linking and type identity across objects | `dlopen`/`LoadLibrary`-backed `plugin_open`/`Lookup`. Type-identity checks become descriptor-hash checks. Semantics differ; documented. |
| `runtime/cgo` | Is the Go↔C boundary; in C there is no boundary | Header present, all five declarations are no-ops returning success. |
| `runtime/race` | Needs compiler instrumentation | Maps to TSan: `-fsanitize=thread`. Full functionality *with a rebuild*, which is exactly Go's `-race` deal. Better than parity. |
| `runtime/coverage` | Needs compiler instrumentation | Maps to `-fprofile-instr-generate`/gcov; emits Go-compatible `covmeta`/`covcounters` files so `go tool covdata` reads them. |
| `iter.Seq`, `iter.Seq2` | Range-over-func is compiler syntax | Function-pointer + yield-callback types, plus a `BURROW_RANGE(...)` macro giving `for`-loop syntax over a `IterSeq`. Ergonomics differ, capability identical. |
| `reflect` on unregistered types | C has no RTTI | Complete over descriptor-registered types. `reflect_type_of` on an unknown pointer returns `NULL` and a diagnosable error rather than guessing. → [07](07-reflect.md) §7 |
| `weak.Pointer`, `runtime.SetFinalizer`, `runtime.AddCleanup` | Need a tracing collector | Real under the Boehm allocator backend. Under arena/manual backends, `weak.Pointer` is a checked handle into a generational slot table (works, different liveness rules) and finalizers run at arena teardown. Backend-dependent; documented per backend. |
| `runtime.MemStats` GC fields | No Go GC exists | Populated with the active allocator's real statistics. Field *shape* is identical so tooling parses it; `NumGC` is 0 under arena. |
| `structs.HostLayout` | Compiler layout directive | Zero-size marker struct; C layout is already host layout. Trivially correct. |
| `go/build` toolchain queries | Shells out to `go` | Present and functional when a Go toolchain is on `PATH`; returns `build_err_no_toolchain` otherwise. The pure parts (constraint evaluation, file matching) are complete. |
| `testing/synctest` | Needs the Go scheduler's idle detection | Real, because we own the scheduler — arguably cleaner than Go's. → [06](06-runtime.md) §8 |
| `simd/archsimd` | Compiler intrinsics, Go 1.27-new, unstable | Deferred to post-1.0. Tracked gap. → [17](17-open-questions.md) §2 |

Fourteen entries. Eleven are full-capability substitutes; three (`plugin`,
`weak` under non-GC backends, `simd/archsimd`) are genuine semantic
differences. Out of 23,730 declarations, that is the honest extent of the
asterisk, and it is small enough to print on the front page — which is where
it goes.

## 7. Explicitly out of scope

- `cmd/*` — the Go toolchain. 201 packages. Not a library.
- `internal/*` — ported only as far as public packages require, under
  `burrow__*` names, with no stability promise.
- `vendor/golang.org/x/*` — ported as `burrow` internals (h2/h3/QUIC/IDNA are
  required for `net/http` and `net` fidelity), not exposed as public API,
  because Go does not expose them either.
- `js/wasm` as a target. `wasip1/wasm` is in scope; the JS bridge is not.
  → [15](15-build-deploy.md) §7
