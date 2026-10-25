# Porting a package

This page is for people who want to add a package to burrow, or finish one. [CONTRIBUTING.md](../../CONTRIBUTING.md) has the recipe as six short steps. This is the long version, with `hash/adler32` as the example all the way through, since it's small enough to read in one sitting and has every part a bigger package has: a header, a source file, Go's tests, a method set and two errors.

## 1. Claim it and measure it

Say on the package's milestone issue that you're taking it, so two people don't port the same thing. Then see what you're signing up for:

<!-- not compiled: a shell command -->
```sh
tools/inventory.sh hash/adler32
```

It counts the package's exported declarations in `$GOROOT/api`, the lines of Go in it and in its tests, and how many standard packages it depends on. `-l` lists the declarations themselves. A package can only go in once the packages it imports are in, so the dependency count is a fair guide to how soon you can start.

## 2. Keep Go's shape

A port is reviewed by reading it next to Go's file, so it keeps Go's files, Go's order and Go's names. `src/hash/adler32/adler32.go` becomes `src/hash/adler32.c`, and the public part goes in `include/burrow/hash/adler32.h`.

The header starts with what the package is for, then names the package's import path:

<!-- not compiled: from include/burrow/hash/adler32.h -->
```c
/* burrow:package hash/adler32 */
```

That line is how the amalgamation generator knows which files make up the package and what `BURROW_OMIT_HASH_ADLER32` should drop, so every `.c` file of the package includes this header first. The umbrella header `include/burrow/burrow.h` gets an `#include` for it too.

A file that comes from Go says so in its first lines, with the Go release it was read from and both copyright lines. `tools/check-headers.sh` checks this:

<!-- not compiled: from src/hash/adler32.c -->
```c
/* Derived from Go's src/hash/adler32/adler32.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */
```

Then translate, function by function, keeping Go's comments. Here's the inner loop of Go's `update`:

<!-- not compiled: Go, from src/hash/adler32/adler32.go -->
```go
for len(p) >= 4 {
	s1 += uint32(p[0])
	s2 += s1
	s1 += uint32(p[1])
	s2 += s1
	s1 += uint32(p[2])
	s2 += s1
	s1 += uint32(p[3])
	s2 += s1
	p = p[4:]
}
```

And the same loop in C:

<!-- not compiled: from src/hash/adler32.c -->
```c
for (; m >= 4; m -= 4) {
    s1 += p[0];
    s2 += s1;
    s1 += p[1];
    s2 += s1;
    s1 += p[2];
    s2 += s1;
    s1 += p[3];
    s2 += s1;
    p += 4;
}
```

Don't improve it while you're there. If Go's code looks slow or odd, port it as it is and open an issue. Somebody may depend on exactly what it does, and a port that is faithful only in places is one nobody can check.

## 3. Name things the way the tools expect

Go's exported names become the package name, an underscore and the name in snake case. `adler32.Checksum` is `adler32_checksum`, `adler32.New` is `adler32_new` and the constant `adler32.Size` is `ADLER32_SIZE`. `tools/burrow-coverage` uses these rules to find each of Go's declarations in the headers, so a name it can't find is a declaration it counts as missing. [Coming from Go](from-go.md) has the full set of rules, for methods and types as well as functions.

Anything Go doesn't export is `static` in the `.c` file. If another file of burrow needs it, it gets the `burrow__` prefix and goes in a private header under `src/`. `update` is used by `compress/zlib` too, so it's `burrow__adler32_update` in `src/hash/internal.h`.

Where Go would allocate, the C function takes an `Alloc *` as its first parameter. `New` allocates the digest, so it's `adler32_new(Alloc *a)`. `Checksum` doesn't, so it takes only the data. Results follow the same pattern everywhere: the first result is the return value, the others are out parameters, and an `Error` goes last.

Errors keep Go's text exactly, since people match on it:

<!-- not compiled: from src/hash/adler32.c -->
```c
BURROW_SENTINEL_ERROR(adler32_err_identifier,
                      "hash/adler32: invalid hash state identifier");
BURROW_SENTINEL_ERROR(adler32_err_size, "hash/adler32: invalid hash state size");
```

A Go type with methods gets its method set declared with `BURROW_METHODS_DEFINE`, so that a type assertion on it gives the answer Go's would. Go's `*digest` has `AppendBinary`, `Clone`, `MarshalBinary` and the rest, and the C digest lists the same ten methods. [interfaces](interfaces.md) explains how that works.

## 4. Bring the tests with it

Go's tests come across in the same pull request as the code, in a file named after Go's. `adler32_test.go` becomes `tests/adler32_test.c`. Keep the test names, the tables and the failure messages exactly as Go has them, even the typos, so that a failure here can be compared with a failure there:

<!-- not compiled: from tests/adler32_test.c -->
```c
if (got != g->out) {
    testing_t_errorf_v(t,
                       "simple implementation: checksum(%q) = 0x%x want 0x%x",
                       in, got, g->out);
    continue;
}
```

The file ends with the list of tests and the runner:

<!-- not compiled: from tests/adler32_test.c -->
```c
#define TESTS(X)                                                                       \
    X(TestHashInterface)                                                               \
    X(TestGolden)                                                                      \
    X(TestGoldenMarshal)                                                               \
    X(BenchmarkAdler32KB)

TESTING_MAIN(TESTS)
```

`tools/burrow-gen tests` does the mechanical part. Give it one of Go's `_test.go` files and it writes the structs behind the test tables, the tables themselves and every `Test` function, with the loop over a table written out and the loop's body left as Go in a comment for you to translate. Go's names in it come out as their C names, using the same rules as `tools/burrow-coverage`. It needs Go on your `PATH`.

Sometimes a table has to change shape. Go's adler32 table has rows like `strings.Repeat("\xff", 5548) + "8"`, and C has no way to write that as a literal, so the C table stores the piece, the count and the tail, and a comment above the struct says why. That's fine. Changing what a test checks is not. If you add a test or a check that Go doesn't have, mark it with a comment that starts `Not in Go`, so a reviewer knows not to look for it upstream.

Every test file is its own program. `make build/tests/adler32_test` builds one, and it takes the same `-test.run` and `-test.v` flags a Go test binary does. [testing](testing.md) has more.

## 5. Check it against the gate

`tools/burrow-coverage` says how much of a package's API is declared. Run it with the package's import path. With Go on the machine it reads `$GOROOT/api`:

<!-- not compiled: the output of tools/burrow-coverage hash/adler32 -->
```text
hash/adler32     4/4      100.0%  done
------------------------------------------
total            4/4     100.00%  1 packages, 0 waived, 0 with the wrong signature
```

`-v` lists each declaration with the C name it found. When a package gets to 100 percent, add its import path to `tools/coverage-done.txt` in the same pull request. From then on CI fails if it ever loses a declaration.

Then run the source checks, which are what CI runs first:

<!-- not compiled: a shell command -->
```sh
make check
```

They format-check every file with clang-format 20.1.7, look for banned libc calls, check the file headers, the allocator rule and the list of mutable globals, and a few more. The rules and the reasons for them are in [CONTRIBUTING.md](../../CONTRIBUTING.md). Then `make test`, which builds every test with warnings as errors.

## 6. Fuzz it against Go

A port has a correct answer to every question already, which is Go's. The differential fuzzer in `fuzz/` hands the same bytes to burrow and to Go's own package and fails on the first place the two disagree. A target is two files, `fuzz/NAME.c` for burrow's side and `fuzz/oracle/NAME.go` for Go's, and [fuzz/README.md](../../fuzz/README.md) shows how to write them and run them. When the fuzzer finds something, the input goes in `fuzz/corpus/NAME/` so CI replays it from then on.

## 7. Write the docs

A package isn't done without them. The package gets a guide in `docs/guides/`, `hash/adler32` shares [hashing](hashing.md) with the other checksums, and the code in it comes from programs under `docs/examples/` that CI compiles and runs. A block of C in a guide points at the program it came from with a comment just before it, like `<!-- example: ../examples/hash/hash.c#oneshot -->`, and the program marks the lines with `// doc: oneshot` and `// doc: end`. A program ends with an `/* Output: ... */` comment, and CI fails if the program prints anything else. `tools/burrow-gen doc --check` checks that every block is what its program says, and `--sync` rewrites the blocks from their programs. A guide whose blocks all come from programs goes in `docs/examples/checked.txt`, so a new block without a program fails the check.

## When burrow can't do what Go does

Sometimes C can't match Go. There's no garbage collector to keep a value alive, a platform doesn't have a system call, or Go relies on something its runtime does for it. When that happens, don't paper over it. Write it in [the ledger](../ledger.md): what Go does, what burrow does, why, and whether it will change. A waived declaration goes in `tools/coverage-waivers.txt` with a pointer to its ledger entry, and needs review like anything else.

## The pull request

One package per pull request, with its tests and its docs, and nothing else in it. If you had to change something shared on the way, like the allocator interface or a header other packages use, that's a separate pull request, and it's the one reviewers will want to read most closely. Say in the description what you left out and why, and which of Go's tests are skipped, if any.
