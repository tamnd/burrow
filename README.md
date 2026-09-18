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

## Strings

A string is a pointer and a length, passed by value, and it is not NUL terminated.

```c
Str name = S("burrow");
Str arg  = str_from_cstr(argv[1]);   /* borrows, does not copy */
printf(STR_FMT "\n", STR_ARG(name));
```

That is the same shape Go uses, and it is the only shape that works, because a Go string can contain a NUL byte and a `char *` cannot. `os.ReadFile` on a JPEG returns a perfectly legal Go string. Every substring is also free, since the result just points into the middle of the original, which is why `strings_split` can hand back a thousand pieces without a thousand allocations.

Details, including how to get a real C string back when you need one: [docs/guides/strings.md](docs/guides/strings.md).

## Types

Go's library leans on its type system far more than it looks like it does. `fmt` prints anything because it can ask the value what it is, `encoding/json` walks a struct nobody wrote code for, `sort` works on a slice of anything. None of that is possible in C unless the types describe themselves, so in burrow they do.

```c
const Type *t = TYPE_INT;
printf("%u bytes\n", t->size);

if (type_equal(TYPE_STRING, &a, &b)) { ... }
```

A descriptor is a `const Type` in read only memory, one per type, shared by everything that mentions it. Pointing at one costs a word and initialising one costs nothing, because the linker did it. Kinds are numbered exactly the way `reflect.Kind` numbers them, since those numbers are observable through `fmt`.

Details: [docs/guides/types.md](docs/guides/types.md).

## Slices

A pointer, a length, a capacity, and the element's type descriptor. Go's header is three words and this one is four, and the fourth is what buys you one `append` that works for every element type without templates.

```c
Slice xs = slice_make(a, TYPE_INT, 0, 16);
xs = APPEND(Int, a, xs, 42);
Int v = AT(Int, xs, 0);
```

Append matches Go exactly, including the part people rely on without being able to state it: when the capacity is already there the elements go into the existing backing array and every other slice over that array sees them. The growth progression is Go's `nextslicecap`, so a program tuned against Go's allocation count gets the same one here.

`AT` and `APPEND` are not wrappers. They expand to an inline fast path that knows the element size at the call site, which makes the copy a single store and keeps the slice header out of memory, and they fall through to the general version for anything the fast path cannot do. On x86-64 that is fourteen times faster than the call it replaced.

Details, including what it costs against Go and the one place the capacity numbers differ from Go's and why: [docs/guides/slices.md](docs/guides/slices.md).

## Interfaces

Two words, a vtable pointer and a data pointer, which is what Go's interface value is too. Implementing one is a static `const` vtable and a constructor, both written once next to your type, so satisfying an interface costs a few words of read only memory and a call through it is a load and an indirect call.

```c
static Int counter_read(void *self, Slice p, Error *err) { ... }
static const IoReaderVT counter_reader_vt = {&counter_type, counter_read};

IoReader counter_as_io_reader(Counter *c) {
    IoReader r = {&counter_reader_vt, c};
    return r;
}

Int n = CALL(r, read, buf, &err);
```

A zeroed interface value is nil, so an interface field in a struct that came out of an allocator starts out nil without anybody writing a line to say so. Every vtable starts with a `const Type *self_type`, which is what makes `iface_assert`, Go's `v.(T)`, possible on a value that has already forgotten its concrete type.

Embedding is by member, so an `IoReadWriter`'s vtable holds an `IoReaderVT` and an `IoWriterVT` and narrowing to either one is the address of a member rather than a pointer cast that would be right for the first and wrong for the second.

Go's empty interface is `Any`, a type descriptor and a pointer, and it is what `fmt`'s arguments and `json_marshal`'s parameter become.

```c
Any v = ANY_VAL(TYPE_INT, Int, 42);
```

Details, including what `Any` costs you that Go's `any` does not, which is that the pointee has to outlive it: [docs/guides/interfaces.md](docs/guides/interfaces.md).

## Function values

A Go `func` is code plus the variables it captured, so a function value here is a pair of the same two words an interface is. Every function type in a Go signature gets a named type, and `BURROW_FUNC` declares one.

```c
BURROW_FUNC(Filter, bool, Str s);

static bool has_prefix(void *env, Str s) { ... }

PrefixEnv e = {S("go")};
Int n = count_if(lines, FN(Filter, has_prefix, &e));
```

The environment word is not optional. A library that takes a bare function pointer forces every caller who needs state to reach for a global, and then two callers cannot use it at once. `qsort` is that mistake and every platform has since grown a `qsort_r` to undo it. Nothing in burrow takes a bare function pointer.

The function pointer is first, so a zeroed value is nil the same way a zeroed interface is, and the environment goes in first at the call, so the target declares it and ignores it rather than anybody casting a function pointer to a different signature, which is undefined behaviour and a trap under wasm or control flow integrity.

Details, including the one thing that needs thinking about, which is where the environment lives when the value outlives the frame that made it: [docs/guides/functions.md](docs/guides/functions.md).

## io

`io.Reader` and `io.Writer`, the two interfaces everything that moves bytes speaks, plus `Closer`, `Seeker` and the combinations, the sentinel errors with Go's messages, and the four functions that need nothing but an interface to run.

```c
int64_t n = io_copy(a, dst, src, &err);
```

`io_read_full` and `io_read_at_least` are ported line for line from Go's, including the distinction that makes them worth having: an input that ends before anything arrives is `io_eof` and an input that ends halfway through is `io_err_unexpected_eof`, which is the difference between there was no next record and the file is truncated.

The implementations that fill these in live where they live in Go, which is `os` and `bytes`, and neither of those is written yet.

## Errors

Go's `error` is an interface with one method, so burrow's is an interface value: a vtable pointer and a data pointer, returned by value, and a zeroed one means nothing went wrong.

```c
Error err = os_write_file(path, data, 0644);
if (FAILED(err))
    return err;

if (errors_is(err, os_err_not_exist)) { ... }
```

Succeeding costs nothing, since there is no allocation on the happy path and nothing to free. The several hundred sentinels in Go's library, `io.EOF` and friends, are static `const` objects in read only memory here, so comparing against one is two pointer loads and creating one is a line the linker resolves. `errors_is` and `errors_as` are ports of Go's, walk for walk, including the `Unwrap() []error` trees that `errors.Join` builds.

`errors_as` returns the pointer instead of taking one and returning a bool, because in C the pointer is the bool:

```c
const OsPathError *pe = errors_as(err, TYPE_OS_PATH_ERROR);
```

Details, including how to write your own error type and what happens when the allocator says no: [docs/guides/errors.md](docs/guides/errors.md).

## Maps

A Swiss table, which is what Go's map has been since 1.24. Keys and values are copied in and compared by value, so a map keyed by string is keyed by the bytes.

```c
Map *counts = map_make(a, TYPE_STRING, TYPE_INT, 0);
MAP_SET(Str, Int, counts, S("the"), 1);

Int *n = MAP_GET(Str, Int, counts, S("the"));
if (n != NULL)
    (*n)++;
```

Eight slots to a group and eight control bytes in front of them, so asking which of the eight might hold your key is a few instructions on one 64 bit word, no branches and no key comparisons. A lookup that misses usually touches one cache line and compares nothing.

Iteration order is randomised per iterator, and that is on purpose. A program that depends on map order is already broken, and breaking it on the first run beats breaking it the day somebody adds a key.

Go's awkward corners come along unchanged because they are observable: a nil map reads as empty and writing to one stops the program, `-0.0` and `0.0` are one key, and every NaN key is a different key that can never be found again.

```c
for (MapIter it = map_iter(m); map_next(&it, &k, &v); ) { ... }
```

Details, including the one deviation from Go, which is that an insert that grows the table invalidates a live iterator and says so: [docs/guides/maps.md](docs/guides/maps.md).

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
