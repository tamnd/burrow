# Building burrow into your program

There are three ways to build with burrow. They produce the same library, and they differ only in how much of the repository you have to carry around.

## Two files

This is the way most people should use it, and it is how SQLite ships. Every release has a `burrow-VERSION-amalgamation` archive with three files in it that matter: `burrow.c`, `burrow.h` and `burrow-manifest.json`. Copy the first two into your project and compile the `.c` like any other file.

```sh
cc -std=c11 -O2 -c burrow.c
cc -std=c11 -O2 main.c burrow.o -pthread -o main
```

On Windows with MinGW, link `-lws2_32` instead of `-pthread`. With MSVC, add `burrow.c` to the project and link `ws2_32.lib`.

Your code includes one header:

<!-- not compiled: needs the generated burrow.h, and tests/amalgamation/hello.c is the copy CI builds against a fresh pair -->
```c
#include "burrow.h"

#include <stdio.h>

static SyncWaitGroup wg;

static void hello(void *env) {
    printf("hello from goroutine %d\n", (int)(intptr_t)env);
}

static void body(void *env) {
    (void)env;
    for (int i = 0; i < 3; i++)
        sync_wait_group_go(&wg, BURROW_FN(Func, hello, (void *)(intptr_t)i));
    sync_wait_group_wait(&wg);
}

int main(void) {
    runtime_main(BURROW_FN(Func, body, NULL));
}
```

`tests/amalgamation/hello.c` is a slightly longer version of this, and CI builds it this way against a freshly generated pair on every push.

A few things are worth knowing about the two files.

They are generated, not written. `burrow.c` is every private header and every source file in the tree pasted together in a fixed order, and `burrow.h` is the public headers. Each piece starts with a marker and a `#line` pointing at the file it came from, so a compiler warning, a debugger or a sanitizer report names `src/runtime/chan.c` and a line in it rather than line 40,000 of `burrow.c`.

They compile clean under the same warning set the tree does, `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` and the rest, with gcc, clang and MinGW.

Tracebacks print addresses and not names. The name table a panic prints from is built by reading the compiled objects, which a single `.c` cannot do for itself, so the copy inside `burrow.c` is empty. If you want names, build with `-DBURROW_EXTERNAL_SYMTAB` and generate the table with `tools/burrow-symtab`, which is what the Makefile does. Everything else behaves the same.

`BURROW_SOURCE_ID` is set to the release tag. It is what `burrow_sourceid()` returns, and you can define it yourself when compiling if you want your own build's commit there instead.

`burrow-manifest.json` lists the SHA-256 of every file the pair was made from and of the pair itself. Generating from the same tree always gives the same bytes, so you can check a download against the source tarball:

```sh
tools/burrow-gen amalgamate --out check --source-id v0.0.31
cmp check/burrow.c burrow.c
```

## Leaving packages out

A package you don't use costs compile time and nothing else, because the linker drops what nothing calls. If the compile time matters, there are two ways to cut it down.

The first is to generate a smaller pair. `--packages` takes a comma separated list of import paths and keeps those packages and whatever they need, and nothing else:

```sh
tools/burrow-gen amalgamate --packages context --out dist
```

What else came along is listed in `burrow-manifest.json`, and it is always at least sync, sync/atomic, time and unicode/utf8, because the runtime itself uses them. A name that isn't a package is an error that lists the ones that are.

The second works on the full pair you already have. Every package that can be dropped is wrapped in a `BURROW_OMIT_` macro named after its import path, in capitals with the slashes turned into underscores, so `BURROW_OMIT_CONTEXT` or `BURROW_OMIT_UNICODE_UTF8`. Define it for `burrow.c` and for your own files, which in practice means on the compiler command line:

```sh
cc -std=c11 -O2 -DBURROW_OMIT_CONTEXT -DBURROW_OMIT_IO -c burrow.c
cc -std=c11 -O2 -DBURROW_OMIT_CONTEXT -DBURROW_OMIT_IO main.c burrow.o -pthread -o main
```

The list of packages is at the top of `burrow.h`, and so are the checks. Leaving out a package the runtime needs, or one that a package you kept needs, stops the compile with an `#error` that names both, rather than failing at link time with a missing symbol. `make check` builds every combination the macros allow and runs a program against each one, so none of them can quietly stop compiling.

`--packages` is the better of the two when you control the build, since the smaller file compiles faster. The macros are for one vendored copy that several programs with different needs share.

## One binary for every system

[Cosmopolitan](https://github.com/jart/cosmopolitan) builds a program that runs as it is on Linux, macOS, Windows and the BSDs, on x86-64 and arm64, out of one file. burrow builds with it unchanged, so the two files are all it takes:

```sh
cosmocc -std=c11 -O2 -o hello burrow.c hello.c
./hello
```

CI builds `tests/amalgamation/hello.c` this way on Linux and runs the same binary, without rebuilding it, on macOS and Windows. It is about a megabyte.

Some things to know. The netpoller has no Cosmopolitan backend yet, so nothing under cosmocc waits on a descriptor through it, which matters once there is a net package to use it. The stack protector is left out of a cosmocc build, because a binary built with it crashes on its first check. And `make test` works with the x86-64 compiler, `x86_64-unknown-cosmo-cc` with `x86_64-unknown-cosmo-ar`, but not with `cosmocc` itself, whose archive tool keeps only one architecture's objects. The single file build has no archive and so has no such problem.

## make

From a checkout, `make` builds `build/libburrow.a`, and your program adds `-Iinclude` and links the archive. `make test` runs the suite and `make check` runs the source gates first. `make amalgamation` writes the two files into `build/amalgamation`, and `make AMALGAMATION=1 test` builds the library out of them and runs every test against exactly what ships.

## CMake

`cmake -S . -B build && cmake --build build` builds the same archive, and after `cmake --install` a project can use `find_package(burrow)` and link `burrow::burrow`. MSVC builds go this way.

## Not yet

Choosing a single target platform, baking in a prefix for the private names, and splitting `burrow.c` into several files are designed in [docs/design/15-build-deploy.md](../design/15-build-deploy.md) section 2 and not implemented. Today the pair holds the code for every platform, and the parts for the platforms you are not on compile to nothing.
