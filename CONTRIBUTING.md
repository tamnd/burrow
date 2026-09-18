# Contributing

The work is large and it parallelises well. Most of it is packages, and a package is a self contained unit with an inherited test suite and a merge criterion that a machine checks, so you can pick one up without talking to anybody first.

## Before you start

Read [docs/design/00-overview.md](docs/design/00-overview.md). It is twenty minutes and it will save you from writing something that has to be thrown away.

Then read the two documents that constrain every line of code in the tree:

- [docs/design/05-memory.md](docs/design/05-memory.md) for why every allocating function takes an allocator first.
- [docs/design/08-naming-abi.md](docs/design/08-naming-abi.md) for how a Go name becomes a C name. The rules are mechanical and there is no lookup table, so if you find yourself inventing a name you have probably found a bug in the rules and should open an issue about it rather than picking something that reads nicely.

## Porting a package

The recipe is the same every time.

1. Claim the package by commenting on its milestone issue, so two people do not do the same one.
2. Run `tools/inventory.sh <pkg>` to get the symbol list and the line counts you are signing up for.
3. Translate the Go source file by file, keeping the file names. `strings.go` becomes `strings.c`, `strings_test.go` becomes `strings_test.c`. Keeping the shape means a reviewer can diff against upstream, which is the only practical way to review a port of this size.
4. Bring the tests across with the code, in the same commit. `tools/burrow-gen tests` does the mechanical part of the translation for table driven tests, which is most of them.
5. Run the differential fuzzer against the package. It builds Go's version as a C archive and asks both implementations the same question until they disagree.
6. Write the docs. A package is not done without them, and the docs job in CI compiles and runs every example, so a broken example is a broken build.

## The rules that CI enforces

- Every file derived from Go carries a header naming the upstream file, the Go release, and the commit it was read at. The check is in `tools/check-headers`.
- No compiler warnings. The build is `-Wall -Wextra -Werror` plus about thirty more, on four compilers.
- No allocation without an allocator parameter. `malloc`, `calloc`, `realloc` and `free` appear in exactly one file, which is the malloc backend.
- No libc string functions. `strcpy`, `strcat`, `sprintf` and the rest of that family are banned outright, and the banned list is in `tools/check-banned`.
- Clean under AddressSanitizer, UndefinedBehaviorSanitizer, MemorySanitizer, ThreadSanitizer and the tracking allocator.
- Every public symbol maps back to a Go declaration in `$GOROOT/api`, checked by `tools/coverage`.
- Formatted by clang-format 20.1.7, which is the version CI runs and the version `make fmt` should be run with. Get it with `pipx install clang-format==20.1.7` if the one on your machine is a different release. The versions disagree with each other about a few constructs, so a tree formatted by a newer one fails the gate on a correctly formatted file, and the only fix for that is for everybody to run the same one.
- Clean under clang-tidy 20.1.0, which is `make tidy` locally and a job in CI. Pinned for the same reason the formatter is, and available the same way with `pipx install clang-tidy==20.1.0`. The configuration is `.clang-tidy` in the root, every check that is switched off has a paragraph next to it saying why, and if one of them is in your way then the thing to argue with is that paragraph. On macOS the pinned build ships no default sysroot, so pass one: `make tidy CLANG_TIDY="clang-tidy --extra-arg=-isysroot --extra-arg=$(xcrun --show-sdk-path)"`.
- Passes on a big endian machine. CI builds and runs the suite on s390x under emulation on every pull request, and it takes about three minutes. Nothing in the library is allowed to care about byte order, and the way that rule gets broken is almost never a cast that looks wrong. It is a test that reads the first byte of an integer, and it passes on every machine any of us owns.

None of this is negotiable per PR, but all of it is negotiable in an issue. If a rule is wrong it should be changed everywhere rather than waived once.

## Commits and pull requests

Write the commit message as a sentence that says what changed and why, in the present tense, addressed to somebody reading `git log` in two years. Prefix it with the package when there is one.

```
strings: the Rabin-Karp path Index takes on long needles
runtime: stop a parked goroutine outliving the channel it parked on
```

Keep a pull request to one thing. A pull request that ports a package and also changes the allocator interface is two pull requests, and the second one is the interesting one.

## Fidelity, and when to differ from Go

The default is that burrow does what Go does, including the parts that look like bugs, because somebody's code depends on them and because a port that improves things selectively is a port nobody can reason about.

When burrow genuinely cannot match Go, it goes in the fidelity ledger with what Go does, what burrow does, why, and whether it is permanent. The ledger is in `docs/ledger.md` and it is linked from every symbol it touches. There are fourteen entries today and every one of them was argued about. Adding the fifteenth is allowed and it needs the same argument.

## Developer Certificate of Origin

Every commit needs a `Signed-off-by` line, which `git commit -s` adds. It says you wrote the patch or have the right to submit it under the project's licence. That matters more than usual here, because the project is a derivative work of somebody else's BSD licensed code and the provenance chain has to be clean.

## Security

Do not open an issue for a vulnerability. Mail the address in [SECURITY.md](SECURITY.md).
