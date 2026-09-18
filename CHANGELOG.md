# Changelog

Every release gets a section here and the release workflow refuses to publish a tag that does not have one, so this file cannot fall behind.

Versions are `0.MINOR.PATCH` until 1.0. The minor number goes up when a milestone finishes and the patch number goes up for everything in between. Nothing before 1.0 is a stable API and everything before 1.0 is published as a prerelease, because none of it has been through a security review.

## Unreleased

### Errors

- `Error`: an interface value, a vtable pointer and a data pointer, returned by value, and a zeroed one means nothing went wrong. Succeeding allocates nothing and leaves nothing to free.
- `errors_new`, `errors_is`, `errors_as`, `errors_unwrap`, `errors_join` and `errors_join_v`, ported walk for walk from Go's, including the `Unwrap() []error` trees that `errors.Join` builds. `errors_as` returns the pointer rather than taking one and returning a bool, because in C the pointer is the bool.
- Sentinel errors are static `const` objects in read only memory, so comparing against one is two pointer loads and declaring one is a line the linker resolves.
- `docs/guides/errors.md` and `docs/guides/failure.md`.

### Maps

- `Map`: a Swiss table, which is what Go's map has been since 1.24. Eight slots to a group with eight control bytes in front of them, so asking which of the eight might hold a key is a few instructions on one 64 bit word with no branches and no key comparisons.
- `map_make`, `map_get`, `map_get2`, `map_set`, `map_del`, `map_clear`, `map_len`, `map_free`, `map_iter` and `map_next`, plus `MAP_SET`, `MAP_GET`, `MAP_HAS` and `MAP_DEL` for the call sites where the types are known.
- Iteration order is randomised per iterator, deliberately, because a program that depends on map order is already broken and breaking it on the first run beats breaking it the day somebody adds a key.
- Go's awkward corners come across because they are observable: a nil map reads as empty and writing to one stops the program, a negative zero and a positive zero are one key, and every NaN key is a different key that can never be found again.
- Float and complex descriptors got their own equality and hash, which is what those two rules are made of. Complex compares and hashes componentwise.
- Tombstones are reclaimed by rebuilding at the same size when the live entries would fit, so filling and emptying a map forever, which is what a cache does, reaches a steady state instead of growing without bound.
- `map_set` returns false when the table needed to grow and the allocator refused, and the map is unchanged. Go stops the world instead, and a library that takes the caller's allocator has to hand that decision back.
- One deviation from Go, and it is entry fifteen in `docs/ledger.md`. An insert that grows the table while an iterator is live moves every entry, so `map_next` stops the program rather than producing an arbitrary answer. Go keeps the displaced table alive for the iterator and needs a collector to know when it dies.
- `map_make` takes an allocator and the map keeps it, which is the third carve-out in the allocator rule and the reason `map_free` exists. Both are argued in `docs/design/05-memory.md` §2.
- `docs/guides/maps.md`.

### Runtime

- `runtime_rand64`, xoshiro256++ seeded from the OS, per thread. Every map gets its own hash seed from it, so two maps holding the same keys have different layouts and a program cannot be fed keys that all land in one group.

## v0.0.2 (2026-09-18)

Type descriptors and `Slice`, which are the two things `Map`, `Error`, `fmt` and `encoding/json` were all waiting on. Still nothing you can use as a Go standard library.

### Types

- The type descriptor. A `const Type` in read only memory, one per type, shared by everything that mentions it, so pointing at one costs a word and initialising one costs nothing because the linker did it. `Kind` is numbered exactly the way `reflect.Kind` numbers it, since those numbers are observable through `fmt`.
- `TypeOps`, optional, for the types whose equality is not a `memcmp` of the struct. `Str` is the reason it exists: two `Str` values pointing at different buffers holding the same bytes are one value in Go.
- `type_equal`, `type_hash`, `type_size`, `type_align`, `kind_name`, and the descriptors for every builtin.
- `docs/guides/types.md`.

### Slices

- `Slice`: pointer, length, capacity, and the element's descriptor. Four words where Go has three, and the fourth is what buys one `append` that works for every element type without templates or a macro per type.
- `append` matching Go exactly, including the part people rely on without being able to state it. When the capacity is already there the elements go into the existing backing array and every other slice over that array sees them.
- The growth progression is a port of Go's `nextslicecap`, so a program tuned against Go's allocation count gets the same one here. Go's `roundupsize` step is deliberately not ported, because size classes are a property of Go's allocator and burrow's allocator is whichever one you passed in. That is the one place the capacity numbers differ from Go's, and the table of where is in the guide.
- Nil and empty kept apart, because Go keeps them apart and the difference is visible in JSON output.
- `slice_sub` and `slice_sub3`, bounds checked against `cap` and not `len`, which is Go's rule and is how you get at the spare capacity on purpose.
- `slice_copy`, which handles overlap, because `copy(s, s[1:])` is how you delete an element and Go promises it works.
- Zero sized elements never allocate, so `[]struct{}` costs no memory and grows without calling the allocator.
- `docs/guides/slices.md`.

### Performance

`AT` and `APPEND` are no longer wrappers around `slice_at` and `slice_append`. They expand to an inline fast path that is handed the element size at the call site, which makes the copy a single store and keeps the four word header in registers instead of writing it to the stack on the way into a call and reading it back through a return buffer on the way out.

That is worth fourteen times on x86-64 and not quite twice on arm64, for the same change. A four word struct is over the register passing limit on both ABIs, but on the AMD core tested the callee's four eight byte stores cannot be forwarded to the caller's two sixteen byte loads, so it waited for cache twice per append. Apple's core forwards it.

- 1024 appends into a slice with the capacity in hand: 6779 ns to 3700 ns on an M1, 19044 ns to 1360 ns on an EPYC.
- 64 bounds checked reads: 57.4 ns to 36.0 ns on an M1, 211 ns to 82 ns on an EPYC.

The element size is still checked against the descriptor at runtime, and a wrong size, a nil pointer, a full slice or an index out of range all fall through to the general path, so the bounds checks and the growth arithmetic are written exactly once and passing a `T` of the wrong size is a performance mistake rather than a correctness one.

Go is still ahead on both. 738 ns for the same appends and 17.6 ns for the same reads, because its compiler can often prove the bounds check away and burrow's cannot.

### Headers

`Slice` is in `burrow/slice.h` rather than `burrow/core.h`. A `Slice` points at a `Type` and a `Type` contains a `Str`, so the three have to be declared in that order. `burrow/burrow.h` includes them in the right order and nobody else has to care.

### Fixed

- CI was red on gcc and on the formatter and both failures were invisible on macOS. gcc rejects comparing two arrays with `!=`, and the formatter gate had no pinned version so it failed on a correctly formatted file. clang-format is now pinned to 20.1.7 in CI and named in `CONTRIBUTING.md`.

## v0.0.1 (2026-09-18)

The first tag. Nothing in here is usable as a Go standard library yet, and the point of releasing it is to find out whether the release machinery works while the mistakes are still cheap.

What exists is the bottom of the stack.

### Memory

Every function in burrow that can allocate takes an `Alloc *a` as its first parameter. That rule is the whole memory design and it is settled now rather than later.

- `Alloc`, one vtable, four operations, with zeroing handled at the interface so that no backend author can forget Go's zero value rule and no backend that already knows its memory is clean pays for it twice.
- `arena`, the default. Nesting, and a last allocation unwind that makes append in a loop reallocate in place instead of copying.
- `heap`, `malloc` and `free` behind the same interface. The only file in the tree allowed to call them, which a check enforces.
- `fixed`, over a buffer you hand it, for targets that have no allocator at all.
- `docs/guides/allocators.md`.

`gc` and `track` are not written.

### Core types

- `Str`, a pointer and a length, passed by value, never NUL terminated. Go strings can contain NUL bytes and every `char *` version of this is a truncation waiting for the one input somebody sent on purpose.
- `str_at`, which stops when the index is out of range, because Go's `s[i]` does.
- The numeric types. `Int` follows the pointer width, since Go's `int` does and the overflow behaviour is visible in Go's own tests.
- `docs/guides/strings.md`.

`Slice`, `Map`, `Error` and interfaces are not written.

### Platform

- `platform.h` answers every question a configure script would ask, using macros the compiler already sets. There is no configure step in burrow and there never will be.
- The OS and architecture names are Go's GOOS and GOARCH strings, so a bug report saying linux/arm64 means the same thing in both projects. An unknown platform is a compile error naming the one file that has to change, rather than a guess.

### Runtime

- `runtime_throw` and the two bounds check messages, carrying Go's text byte for byte. These are fatal errors rather than panics, because `recover` needs `defer` and `defer` needs the scheduler. The mechanism changes later, the text does not.
- `runtime_set_fatal_handler`, for the places that have no standard error to print to.
- `docs/guides/failure.md`.

### Build

- Four compilers, seven platforms, five sanitisers, all at `-Werror`.
- `tools/check-banned.sh` and `tools/check-headers.sh`.
- Sixteen CI jobs, green.

The API is not stable and will not be until 1.0. Published as a prerelease, because nothing here has been through a security review.
