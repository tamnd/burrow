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
3. Translate the Go source file by file, keeping the file names. `strings.go` becomes `strings.c`, `strings_test.go` becomes `strings_test.c`. Keeping the shape means a reviewer can diff against upstream, which is the only practical way to review a port of this size. The package's header gets a `/* burrow:package strings */` line with its import path, and each of its `.c` files includes that header first. That is how the amalgamation generator knows which files are the package, what it needs, and what `--packages` and `BURROW_OMIT_STRINGS` should drop.
4. Bring the tests across with the code, in the same commit. `tools/burrow-gen tests` does the mechanical part of the translation for table driven tests, which is most of them.
5. Run the differential fuzzer against the package. It builds Go's version as a C archive and asks both implementations the same question until they disagree.
6. Write the docs. A package is not done without them, and the docs job in CI compiles and runs every example, so a broken example is a broken build.

## The rules that CI enforces

- Every file derived from Go carries a header naming the upstream file, the Go release, and the commit it was read at. The check is in `tools/check-headers`.
- No compiler warnings. The build is `-Wall -Wextra -Werror` plus about thirty more, on four compilers.
- No allocation without an allocator parameter. `malloc`, `calloc`, `realloc` and `free` appear in exactly one file, which is the malloc backend.
- No libc string functions. `strcpy`, `strcat`, `sprintf` and the rest of that family are banned outright, and the banned list is in `tools/check-banned`.
- No mutable global that is not in `tools/globals.txt`, with its category and a line saying what happens when two threads reach it. The check is `tools/check-globals`, and it fails in both directions, so a line whose global has gone fails too. Adding a global is allowed. Adding one without a reviewer seeing it is not.
- Clean under AddressSanitizer, UndefinedBehaviorSanitizer, MemorySanitizer, ThreadSanitizer and the tracking allocator.
- Every public symbol maps back to a Go declaration in `$GOROOT/api`, checked by `tools/coverage`.
- No public symbol is one that a common C library already defines. `make collisions` compares libburrow.a, and the C name of everything in Go's API that isn't written yet, against every library on your machine, and CI runs it against the 65 packages in `tools/collision-packages.txt` on Linux and the SDK on macOS. A clash that can hurt someone is fixed by renaming the burrow side in the table in `tools/burrow-coverage`. One that can't, like a symbol private to a library nothing else links, goes in `tools/collision-waivers.txt` with the reason.
- Formatted by clang-format 20.1.7, which is the version CI runs and the version `make fmt` should be run with. Get it with `pipx install clang-format==20.1.7` if the one on your machine is a different release. The versions disagree with each other about a few constructs, so a tree formatted by a newer one fails the gate on a correctly formatted file, and the only fix for that is for everybody to run the same one.
- Clean under clang-tidy 20.1.0, which is `make tidy` locally and a job in CI. Pinned for the same reason the formatter is, and available the same way with `pipx install clang-tidy==20.1.0`. The configuration is `.clang-tidy` in the root, every check that is switched off has a paragraph next to it saying why, and if one of them is in your way then the thing to argue with is that paragraph. On macOS the pinned build ships no default sysroot, so pass one: `make tidy CLANG_TIDY="clang-tidy --extra-arg=-isysroot --extra-arg=$(xcrun --show-sdk-path)"`.
- Passes on a big endian machine. CI builds and runs the suite on s390x under emulation on every pull request, and it takes about three minutes. Nothing in the library is allowed to care about byte order, and the way that rule gets broken is almost never a cast that looks wrong. It is a test that reads the first byte of an integer, and it passes on every machine any of us owns.

None of this is negotiable per PR, but all of it is negotiable in an issue. If a rule is wrong it should be changed everywhere rather than waived once.

## Crypto code

The hard part of crypto in C is not the algorithms, it is keeping the compiler from helping. Go's crypto code is written so that the time it takes does not depend on secrets, and a port has to keep that property through a compiler that is free to turn a mask back into a branch. The reasoning behind these rules is in [docs/design/12-packages-crypto.md](docs/design/12-packages-crypto.md) section 3.

- Anything that depends on a secret goes through `crypto/subtle`: `subtle_constant_time_compare`, `subtle_constant_time_select`, `subtle_constant_time_byte_eq`, `subtle_constant_time_eq`, `subtle_constant_time_less_or_eq` and `subtle_constant_time_copy`, or through a package's own helpers that Go has too, like `ct_mask` and `ct_eq` in `src/crypto/bigmod.c`. No `if`, no `?:`, no `memcmp`, no array index and no division on secret data. Go's code already follows this, so a port that keeps the shape of the Go function keeps it too. The risk is in the places where C offers a shortcut Go didn't have.
- The helpers in `src/crypto/subtle.c` pass their values through `ct_hide`, an empty `asm` statement that stops GCC and Clang from reasoning about the value. MSVC has no such statement, so on MSVC `ct_hide` reads the value back through a `volatile`, and that file turns MSVC's optimiser off with `#pragma optimize("", off)` as well. `ct_mask` in `src/crypto/bigmod.c` uses the same barrier. A new helper of that kind belongs in `subtle.c` with the barrier, not copied into another file without it.
- No profile guided optimisation. Nothing in the build uses `-fprofile-use` or anything like it, and that has to stay true, because PGO puts branches where the profile says they pay off and that is exactly what a secret must not decide.
- Nothing zeroes secrets yet. Go's crypto packages don't wipe key material either, and the `crypto_wipe` in the design doc does not exist. A plain `memset` on a buffer about to be freed is not a fix, because the compiler can drop it.
- `make ctgrind` checks part of this. It runs `tools/ctgrind.c` under valgrind with the secrets marked as undefined memory, so memcheck reports any branch or memory address that depends on one. It needs valgrind and runs in CI on Linux. It covers `crypto/subtle`, the SHA-2 hashes and HMAC, AES with both the portable code and the AES instructions, GCM, ChaCha20-Poly1305, Ed25519, ECDH on all four curves, ECDSA signing on the four NIST curves, ML-KEM, RSA signing and decryption, and bigmod's `Exp`. If you add a constant-time path, add a check for it there. Each place Go branches on a secret on purpose, to report that the secret is invalid, is listed in `tools/ctgrind.supp` with the reason. A new report is a bug to fix, not another line for that file. `dudect` and an assembly diff on `subtle.c` are still planned. Until they land, and for the code ctgrind doesn't reach, the check is a reviewer reading the diff. Say in the pull request when a change touches code that handles secrets, so that somebody reads it with this list in mind.

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
