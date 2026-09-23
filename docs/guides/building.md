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

## make

From a checkout, `make` builds `build/libburrow.a`, and your program adds `-Iinclude` and links the archive. `make test` runs the suite and `make check` runs the source gates first. `make amalgamation` writes the two files into `build/amalgamation`, and `make AMALGAMATION=1 test` builds the library out of them and runs every test against exactly what ships.

## CMake

`cmake -S . -B build && cmake --build build` builds the same archive, and after `cmake --install` a project can use `find_package(burrow)` and link `burrow::burrow`. MSVC builds go this way.

## Not yet

Choosing a subset of packages or a single target platform when generating is designed in [docs/design/15-build-deploy.md](../design/15-build-deploy.md) section 2 and not implemented. Today the pair holds all of burrow for every platform, and the parts for the platforms you are not on compile to nothing.
