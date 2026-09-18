# Allocators

This is the one page in the documentation you should not skip. Everything else in burrow is a port of something you can already look up in Go's documentation. This part has no equivalent in Go, because in Go the garbage collector does it for you, and here you do it yourself.

It is shorter than you are expecting.

## The rule

Every function in burrow that can allocate takes an allocator as its first parameter. There is one exception, described below, there is no hidden global, and there is no per object free function to remember.

```c
#include "burrow/burrow.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Str body = os_read_file(a, S("go.mod"));
    Slice lines = strings_split(a, body, S("\n"));

    arena_free(&ar);
    return 0;
}
```

Two allocations happened in there and possibly a few hundred underneath them, and one call cleaned up all of it. You do not free `body`. You do not free `lines`. You do not free the strings inside `lines`. You free the arena.

That is the whole model. The rest of this page is about the cases where the default is not what you want.

## The exception, which is Map

`map_make` takes an allocator and the map keeps it, so `map_set` does not take one and can still allocate:

```c
Map *m = map_make(a, TYPE_STRING, TYPE_INT, 0);
map_set(m, &key, &val);      /* this can grow the table */
```

An insert can grow the table, and the alternative is an allocator parameter on the hottest call a hash table has, plus a caller who passes a different one on the second insert and ends up with a table half from one place and half from another. Storing it once keeps the part that matters, which is that everything a map allocated came from the allocator you handed to `map_make`.

`map_free` exists for the same reason and it is the only per object free in the library. `Str` and `Slice` hand you the pointer and the size, so `mem_free` takes them directly. A `Map` is opaque and moves its own memory around, so nothing outside the map can name the pointer to give back. If you are using an arena, keep ignoring it.

Nothing else in burrow does this, and if something else ever needs to, it will be a growable container for the same reason and it will say so here.

## Why it works this way

Go's API hands back heap objects everywhere, because a collector cleans up afterwards. That design does not survive the trip to C, and there were three ways to deal with it.

Bolt a tracing collector onto the library. That forces a runtime on every consumer, breaks every foreign function interface that wants to call burrow from Python or Rust or Lua, and makes the library something you cannot drop into a program that already has its own ideas about memory.

Write a matching free function for every type. Go's standard library has 23,730 exported declarations. That means documenting ownership 23,730 times and being wrong some of those times, and it means every caller reading the documentation before every call.

Or make allocation explicit and uniform. One rule, stated once, that covers the whole surface. Zig got here first and it was right.

The cost is one extra parameter on a lot of functions. The benefit is that you can read any burrow call and know exactly where the memory came from and when it goes away, without looking anything up.

## The five allocators

Three of these exist today. `gc` and `track` are described here because the interface they plug into is finished and they change nothing about how you write your code, but they are still open items on [P0](https://github.com/tamnd/burrow/issues/1).

### arena

The default, and the one to use unless you have a reason not to.

An arena hands out memory by moving a pointer forward through a large chunk, and gives all of it back at once. Allocation is a bounds check and an add. There is no per object bookkeeping, so there is no per object cost and no way to leak a single object.

```c
Arena ar;
arena_init(&ar, NULL, 0);
Alloc *a = arena_allocator(&ar);
/* ... */
arena_free(&ar);
```

It fits the shape of most programs better than people expect. A request handler, a parse, a build step, a compiler pass and a test all have a moment where everything the work produced stops being interesting at the same time. That moment is your `arena_reset` or your `arena_free`.

For a server, the pattern is one arena per request, reset rather than freed:

```c
Arena ar;
arena_init(&ar, NULL, 0);

for (;;) {
    Request *req = accept_one();
    handle(arena_allocator(&ar), req);
    arena_reset(&ar);
}
```

A reset keeps the chunks instead of returning them, so the second request and every request after it allocate nothing from the operating system at all. This is the reason the pattern is fast, not just tidy.

Arenas nest. Pass another arena's allocator as the parent and the child's chunks come from it:

```c
Arena conn;
arena_init(&conn, NULL, 0);

Arena req;
arena_init(&req, arena_allocator(&conn), 0);
```

An `Arena` is not thread safe and is not meant to be. Give each goroutine its own, which costs one chunk, or put a lock in front of one if you really want sharing. Making the fast path atomic would charge every single threaded program for something most of them never do.

### heap

`malloc` and `free` behind the same interface.

```c
Alloc *a = heap_allocator();
```

Never NULL, never needs initialising, no state to carry. Reach for it when individual objects need to go away at individual times, which is the workload an arena is bad at. A cache with per entry eviction is the clearest example. It is also what you want when you are handing memory to something outside burrow that is going to call `free` on it.

### gc

A conservative collector, optional, off unless you ask for it at build time.

This exists for people porting Go code that genuinely depends on a collector, and for the shape of program where object lifetimes form a graph rather than a tree. It is the escape hatch, not the default, and choosing it means taking on a dependency the rest of the library does not have.

### track

Wraps another allocator and remembers every block it handed out.

```c
Track tr;
track_init(&tr, heap_allocator());
Alloc *a = track_allocator(&tr);

/* run the thing under test */

track_report(&tr, stderr);   /* every block still outstanding, with a backtrace */
```

This is what tests use. It is how burrow's own test suite proves a function allocates what it says it allocates and nothing more, and it is how you find the one place in your program that is holding on to something.

### fixed

An allocator over a buffer you already have. It never calls anything underneath, because there is nothing underneath.

```c
unsigned char buf[4096];
Fixed fx;
fixed_init(&fx, buf, sizeof(buf));
Alloc *a = fixed_allocator(&fx);
```

Two reasons to want this. One is a target with no allocator at all, where a firmware image gets a region at link time and that is the whole story. The other is a test that proves a function stays inside a budget, which it does by handing it exactly that many bytes and watching it either fit or fail.

## Ownership, and the three annotations

Even with one allocator convention, one question is left per function. Does the `Str` coming back point into the input, or is it fresh memory?

Go's collector makes that invisible and it does not matter. In C it decides whether you can free the input while still holding the output, so every declaration in burrow says which it is:

```c
BURROW_OWNS(ret)        Str strings_to_upper(Alloc *a, Str s);
BURROW_BORROWS(ret, s)  Str strings_trim_space(Str s);
```

`BURROW_OWNS(ret)` means the return value is fresh memory from `a` and does not depend on the input. `BURROW_BORROWS(ret, s)` means the return value points into `s` and dies when `s` does. `BURROW_RETAINS` means the function kept a reference to an argument past the call.

Notice that `strings_trim_space` has no allocator parameter at all. That is the tell. A function that cannot allocate cannot give you fresh memory, so anything it returns must point into what you gave it.

The annotations expand to nothing, and they are not decoration. The documentation generator turns them into the lifetime sentence on every reference page, so nobody writes those by hand and nobody gets them wrong in prose. The conformance suite generates an AddressSanitizer test per annotated function that frees the input and touches the output, and requires a report exactly when `BURROW_BORROWS` says the two alias, so a wrong annotation is a failing test rather than a comment somebody believed. And there is a clang plugin that reads them if you want the check in your own code.

## Allocation failure

`mem_alloc` returns NULL when it cannot satisfy a request. It does not abort, it does not longjmp, and it does not call an out of memory handler you did not install, because a library does not get to end the host's process.

Inside burrow, a failed allocation becomes an `Error` that travels back up through the normal error return, the same as any other failure. Callers check errors and this is one more reason an error might be there.

If you would rather crash than check, which is a reasonable choice for a command line tool, write six lines of wrapper around the allocator you are using and crash in its `alloc`. That is your decision to make and it stays in your code, which is exactly where it belongs.

## Writing your own

An `Alloc` is a vtable and a receiver, which is the same shape as a Go interface value, and that is not an accident. Fill in the six slots, point `self` at your state, and hand it to anything in burrow:

```c
static const AllocVT my_vt = {
    my_alloc, NULL, my_realloc, my_free, NULL, NULL,
};

Alloc my_allocator = {&my_vt, &my_state};
```

Three of the six are optional. `alloc_zeroed` is how a backend says it can skip the memset, which an arena handing out untouched memory can and `malloc` cannot. `reset` is NULL for allocators that cannot give everything back at once. `stats` is NULL when you do not count.

You do not have to zero memory in `alloc`. `mem_alloc` does it for you, which means a backend author cannot forget Go's zero value rule, and the `alloc_zeroed` hook means the backends that already know their memory is clean do not pay for it twice.

## What this costs

An arena allocation is a comparison, an add and a store, and it is inlined. That is faster than `malloc`, not slower, and it is the reason the arena is the default rather than a specialist tool.

The extra parameter costs one register on every platform burrow supports. It does not show up in a profile.

What it costs you is thinking about lifetimes, once, at the point where you decide which arena a piece of work belongs to. Go charged you the same thing in a different currency, as collector pauses and as allocation rate tuning, and you did not get to see the bill.

## See also

- `include/burrow/mem.h` for the interface itself
- `docs/design/05-memory.md` for why each decision went the way it did
- `docs/guides/errors.md` for what happens to a failed allocation on its way back to you
- `docs/guides/maps.md` for the one type that stores an allocator, and why
