# burrow

[![CI](https://github.com/tamnd/burrow/actions/workflows/ci.yml/badge.svg)](https://github.com/tamnd/burrow/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/license-BSD--3--Clause-blue.svg)](LICENSE)

The Go standard library, reimplemented in C. All 180 packages, all 23,730 exported declarations, no subset and no "the interesting 20%".

It ships the way SQLite ships. You download one `.c` and one `.h`, drop them in your tree, compile them with the compiler you already have, and pick which packages come along. No build system, no dependency manager, no Go toolchain, no code generator on your machine.

The name is for gophers. They dig burrows, and a burrow is a network of tunnels that all connect.

## Why

Go's standard library is the best designed general purpose library anyone has shipped, and almost nobody can use it. Every embedded target, every kernel adjacent daemon, every codebase that cannot take a Go runtime, and every language whose FFI speaks only C is locked out of `net/http`, `crypto/tls`, `encoding/json`, `time`, `regexp` and `os`. Not because those designs are Go specific, but because nobody did the work.

C programmers hand roll a worse version of `strings` in every project. They reach for a different HTTP library every time and each one has a different opinion about ownership. They get TLS from OpenSSL and spend a week on the API. Meanwhile there is a library sitting right there that solved all of it, with twenty years of design review and 440,000 lines of tests, and the only thing standing between the two is a port that nobody has attempted.

## What it looks like

```c
#define BURROW_SHORT 1
#include "burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DEFER(arena_free, &ar);

    Str body = S("the quick brown fox");
    Slice words = strings_fields(a, body);

    for (int i = 0; i < words.len; i++)
        fmt_println(slice_at(Str, words, i));

    return 0;
}
```

There is no prefix on anything you call. Most C libraries put one on every symbol because C has a single flat namespace, but burrow already has a namespace, which is Go's package name. `strings_contains` is separated from `bytes_contains` and from everything outside the library by a segment that was going to be there anyway, so a library prefix on top would be a namespace on a namespace repeated 23,730 times.

The C name is the Go name with the dot turned into an underscore and the case fixed.

| Go | burrow |
| --- | --- |
| `strings.Contains` | `strings_contains` |
| `net/http.ListenAndServe` | `http_listen_and_serve` |
| `encoding/json.Marshal` | `json_marshal` |
| `(*strings.Builder).WriteString` | `strings_builder_write_string` |
| `strings.Builder` | `StringsBuilder` |
| `io.Reader` | `IoReader` |
| `time.Time` | `Time` |
| `io.EOF` | `io_eof` |
| `time.Second` | `TIME_SECOND` |
| `http.StatusNotFound` | `HTTP_STATUS_NOT_FOUND` |

Types are CamelCase and functions are snake_case, which is GTK's convention. It gives Go's builtins a namespace they otherwise lack, keeps the dangerous lowercase words like `string` and `map` out of the global namespace, and lets you tell a type from a call without thinking about it.

Macros are the one exception and keep a `BURROW_` prefix, because a header that defines `DEFER` or `S` globally will break somebody's program and C has no scoping mechanism that prevents it. `#define BURROW_SHORT 1` turns on the bare spellings for programs that want them. If you ever hit a collision anyway, `#define BURROW_PREFIX bw` puts a prefix back on everything, and since you compile burrow from source that is one line rather than a repackaging exercise.

## Memory

Every function that can allocate takes an allocator as its first parameter. Uniformly, mechanically, with no exceptions.

```c
Str upper = strings_to_upper(a, name);
Slice parts = strings_split(a, path, S("/"));
```

This is Zig's convention and it is the only answer that works here. Go's API hands back heap objects everywhere because a garbage collector cleans up afterwards. Bolting a tracing GC onto a C library would force a runtime on every consumer and break every FFI host. A `_free` for every constructor means documenting ownership 23,730 times and getting it wrong some of those times. One rule that covers the entire surface beats both.

Arena is the default and it is what makes the ergonomics work. You make an arena, you pass it down, you free it once, and in between you write the code you would have written in Go. A Boehm GC backend is available for people who want Go's exact ergonomics, a tracking allocator finds leaks and use after free in CI, `heap` is `malloc` and `free` behind the same interface for people who want to manage it themselves, and `fixed` runs the whole library out of a buffer you hand it.

The full story is one page, and it is the one page worth reading before you write any burrow: [docs/guides/allocators.md](docs/guides/allocators.md).

## Status

Early. Nothing is usable yet.

The design is finished and written down in [docs/design](docs/design), twenty documents covering the scope measurement, the C dialect, the core types, the memory model, the scheduler, the reflection layer, the naming rules, every package tier, the conformance strategy, the build and deployment story, the phasing, and the legal position on porting a BSD licensed library.

The implementation plan is tracked in milestone issues:

- [P0: substrate, the part everything else stands on](https://github.com/tamnd/burrow/issues/1)
- [P1: the pure packages, strings through regexp](https://github.com/tamnd/burrow/issues/3)
- [P2: os, io/fs, time, and the platform layer](https://github.com/tamnd/burrow/issues/4)
- [P3: crypto](https://github.com/tamnd/burrow/issues/5)
- [P4: net and net/http](https://github.com/tamnd/burrow/issues/6)
- [P5: go/\*, templates, database/sql, log/slog](https://github.com/tamnd/burrow/issues/7)
- [P6: hardening and 1.0](https://github.com/tamnd/burrow/issues/8)

## Completeness is measured, not claimed

Go ships a machine readable manifest of its own API at `$GOROOT/api/go1.*.txt`, one line per exported declaration across every version. Because the naming rules are mechanical and reversible, a C symbol maps back to a Go declaration, so `tools/coverage` turns burrow's exported symbols into Go declarations and diffs them against that manifest. Coverage is a number a script prints, not an adjective in a README.

Correctness is measured the same way. Go's tests come across with the code, 440,000 lines of them, and a differential fuzzer runs burrow and Go against the same input and compares the output byte for byte. A package is not done until its coverage gate passes, its translated tests pass, its examples produce Go's exact output, it survives a day of fuzzing without diverging, it is clean under five sanitizers, its benchmarks are within the published ratios, it is green on every supported platform, and its documentation exists. Eight conditions, all of them checkable by a machine, none of them a matter of opinion.

## Repositories

| Repo | What |
| --- | --- |
| [burrow](https://github.com/tamnd/burrow) | The library, the amalgamation generator, the headers |
| [burrow-conformance](https://github.com/tamnd/burrow-conformance) | Translated Go tests, the differential oracle, fuzzing corpora |
| [burrow-testdata](https://github.com/tamnd/burrow-testdata) | Go's testdata files and generated vectors |
| [burrow-examples](https://github.com/tamnd/burrow-examples) | Go programs transliterated to C |

## Licence

BSD-3-Clause, matching Go, because a port of Go's source is a derivative work of Go and matching the upstream licence is the clean answer. Go's `PATENTS` grant travels with the code and is reproduced here. See [NOTICE](NOTICE) for attribution and [docs/design/18-legal.md](docs/design/18-legal.md) for the full position.

This project is not affiliated with, endorsed by, or sponsored by Google or the Go project. Go is a trademark of Google LLC.
