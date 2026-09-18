# Changelog

Every release gets a section here and the release workflow refuses to publish a tag that does not have one, so this file cannot fall behind.

Versions are `0.MINOR.PATCH` until 1.0. The minor number goes up when a milestone finishes and the patch number goes up for everything in between. Nothing before 1.0 is a stable API and everything before 1.0 is published as a prerelease, because none of it has been through a security review.

## Unreleased

Nothing yet.

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
