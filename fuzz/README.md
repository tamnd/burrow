# Differential fuzzing

burrow is a port, so there is already a correct answer to every question it can be asked: the one Go gives. This directory asks both. Each target hands the same bytes to burrow and to Go's own standard library, has each side write a report about what it saw, and fails on the first byte where the two reports differ. libFuzzer then shrinks the input to the smallest one that still disagrees.

The design is in [docs/design/14-conformance.md](../docs/design/14-conformance.md) section 3. This file is how to run it and how to add to it.

## Running it

You need Go and a clang that has libFuzzer. Linux distributions ship one with clang. On a Mac the clang that comes with Xcode does not, and Homebrew's llvm does.

```sh
make fuzz                                    # every target, 60 seconds each
make fuzz FUZZ=utf8 FUZZTIME=600             # one target, ten minutes
make fuzz FUZZ_CC=$(brew --prefix llvm)/bin/clang
```

New inputs that reach new code go into `build-fuzz/corpus/<target>`, so a run never writes into the tree. A disagreement stops the run, prints the first line where the reports differ followed by both reports, and saves the input as `build-fuzz/crash-<target>-<hash>`.

`make fuzz-replay` needs no libFuzzer. It builds the same comparison with your ordinary compiler and runs every file in `fuzz/corpus/<target>` through it once. CI runs it on every pull request, which is what keeps a fixed disagreement fixed.

## When it finds something

Decide which side is wrong first. Nearly always it is burrow, and the fix goes in burrow. Sometimes it is the target itself, when the two report functions do not quite ask the same question, and then the fix goes in the target. Either way the input is copied into `fuzz/corpus/<target>/` under a name that says what it is, so that `make fuzz-replay` runs it from then on.

A disagreement is never fixed by making burrow match a report that the target got wrong, and never by teaching the report to skip the case.

## Adding a target

A target is a pair of files with the same name.

`fuzz/oracle/<name>.go` builds the report from Go, and `fuzz/oracle/oracle.go` gets an exported `oracle_<name>` that calls it. The oracle is one Go archive with one entry point per package, rather than one per function, because a report that walks a whole package over one input finds more than a dozen reports that each look at one call.

`fuzz/<name>.c` builds the same report from burrow and defines `fuzz_target` with the name and both functions. The Makefile finds it by its file name.

The report is plain text, one call per line, integers in decimal and bytes in lowercase hex. It is deliberately simple enough that both sides can write it with nothing more than printf, since the point is to compare the two libraries and not two formatters. Write the two report functions side by side and keep them in the same order, line for line, so that the line number in a failure points at the same call in both.

Put a few seeds in `fuzz/corpus/<name>/`: the empty input, something ordinary, and the edges you already know about. libFuzzer finds the rest faster when it starts near them.

## What the oracle costs

The Go archive carries the Go runtime, which starts its own threads and installs its own signal handlers when the program starts. That is harmless for a target like utf8 that never starts burrow's runtime. A target that does start it, anything with goroutines or timers, shares the process with two schedulers and two sets of signal handlers, and will need care that the targets so far have not.

Go is needed to build the oracle and nothing else. Building burrow never needs it.
