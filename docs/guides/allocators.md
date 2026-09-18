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

Four of these exist today. `gc` is described here because the interface it plugs into is finished and it changes nothing about how you write your code, but it is still an open item on [P0](https://github.com/tamnd/burrow/issues/1).

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

Wraps another allocator and remembers every block it handed out, so that at the end it can tell you what you did wrong.

```c
Track tr;
track_init(&tr, heap_allocator());
Alloc *a = track_allocator(&tr);

/* run the thing under test, passing a */

if (track_check(&tr) != 0)
    /* something is wrong and track_check has already said what */;
track_free(&tr);
```

`track_check` returns the number of faults. Zero means the code under it allocated and freed correctly, and that is usually the whole of what a test needs. When you want the detail rather than the number, register a callback with `track_on_fault` and each fault arrives as a `TrackEvent` with the pointer, the size and alignment it was allocated with, the size and alignment the caller claimed when giving it back, which allocation it was counting from one, and a file and line if the call site asked for one.

Six things get caught. A block still live at `track_check` is a leak. Freeing a pointer that is not live is a double free. Freeing a pointer this allocator never handed out is a wild free, which in real code is usually two allocators in one function and the wrong one at the end. Passing a size or an alignment to free that does not match the allocation is a mismatch, and it matters more here than it does under `malloc`, because an arena moves its bump pointer back by exactly that number and believes you. Writing to a block after freeing it is a write after free.

That last one is why freed memory is not handed straight back. A freed block is filled with a poison byte and held, and only when the quarantine fills up does the oldest one get checked against the poison and really released. So a write after free is found some time after it happened rather than at the moment it happened, and a read after free is not found at all, because catching a read needs the page tables and this needs to work everywhere. AddressSanitizer catches the reads. This catches the ownership mistakes AddressSanitizer cannot see, because it knows what the sizes were supposed to be and ASan does not. Run both.

`track_set_quarantine` decides how much freed memory to hold, one mebibyte by default. Zero turns it off, which turns off write after free detection and gives the memory straight back, which is what you want if you are leaving this allocator in a long running program.

For a leak report you can act on, name the call site:

```c
Byte *p = mem_alloc(TRACK_HERE(a), n, 1);
```

`TRACK_HERE` returns the allocator it was given and does nothing at all unless that allocator is a `Track`, so it is safe to leave in code that also runs against a plain arena, and code you have to edit before you can check it does not get checked.

Two things are worth knowing before you read a report. Realloc here is always a fresh block and a copy, never a grow in place, even when the allocator underneath could have grown it, because growing in place leaves the old pointer valid and hides the bug where somebody kept it. And a reset counts as giving everything back rather than as a thousand leaks, which is what reset means, so a `Track` over an arena stays quiet when you reset the arena.

This is a debugging allocator. It is slow, it holds on to memory, and it does not belong in a program you ship.

### fixed

An allocator over a buffer you already have. It never calls anything underneath, because there is nothing underneath.

```c
unsigned char buf[4096];
Fixed fx;
fixed_init(&fx, buf, sizeof(buf));
Alloc *a = fixed_allocator(&fx);
```

Two reasons to want this. One is a target with no allocator at all, where a firmware image gets a region at link time and that is the whole story. The other is a test that proves a function stays inside a budget, which it does by handing it exactly that many bytes and watching it either fit or fail.

## Ownership, and the four annotations

Even with one allocator convention, one question is left per function. Does the `Str` coming back point into the input, or is it fresh memory?

Go's collector makes that invisible and it does not matter. In C it decides whether you can free the input while still holding the output, so every declaration in burrow says which it is:

```c
BURROW_OWNS(ret)        Str strings_to_upper(Alloc *a, Str s);
BURROW_BORROWS(ret, s)  Str strings_trim_space(Str s);
BURROW_STATIC(ret)      const char *burrow_version(void);
```

`BURROW_OWNS(ret)` means the return value is fresh memory from `a` and does not depend on the input. `BURROW_BORROWS(ret, s)` means the return value points into `s` and dies when `s` does. `BURROW_RETAINS` means the function kept a reference to an argument past the call, which is what you need to know before reusing a buffer you passed in.

`BURROW_STATIC(ret)` is the fourth and it is not a weaker borrow. It means the result has static storage duration or is nil, so there is nothing to free and nothing it can outlive. You can hold a `burrow_version()` or a `kind_name()` result for the life of the program and never think about it again. A borrow has to name what it came from, and these have nothing to name, so writing `BURROW_BORROWS(ret)` with the source left off would have looked exactly like somebody forgetting to fill it in.

Notice that `strings_trim_space` has no allocator parameter at all. That is the tell. A function that cannot allocate cannot give you fresh memory, so anything it returns must point into what you gave it.

Two of them can appear on one declaration, and on append they do:

```c
BURROW_OWNS(ret) BURROW_BORROWS(ret, s)
Slice slice_append(Alloc *a, Slice s, const void *elems, Int n);
```

That is not hedging. Append writes into the array it was given when there is spare capacity and allocates a bigger one when there is not, and from the outside you cannot tell which happened. So both are true and you have to act on both: keep `s` alive, because the result may be pointing at it, and free the result, because it may be memory of its own. If you are using an arena, which you probably are, this is one more thing you get to not think about.

The annotations expand to nothing, and they are not decoration. The documentation generator turns them into the lifetime sentence on every reference page, so nobody writes those by hand and nobody gets them wrong in prose. The conformance suite generates an AddressSanitizer test per annotated function that frees the input and touches the output, and requires a report exactly when `BURROW_BORROWS` says the two alias, so a wrong annotation is a failing test rather than a comment somebody believed. And there is a clang plugin that reads them if you want the check in your own code.

Inside burrow they are checked twice, because being present and being true are different problems. `tools/check-annotations.sh` runs in `make check` and refuses a declaration that returns a pointer and says nothing about it, or an annotation naming a parameter that does not exist. `tests/lifetime_test.c` runs the annotated functions under the tracking allocator and checks the claim itself: `BURROW_OWNS` has to make the live block count go up, `BURROW_BORROWS` and `BURROW_STATIC` have to leave it alone, and a borrow into a buffer has to land inside that buffer.

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
- `include/burrow/own.h` for the four annotations and what each one promises
- `docs/design/05-memory.md` for why each decision went the way it did
- `docs/guides/errors.md` for what happens to a failed allocation on its way back to you
- `docs/guides/maps.md` for the one type that stores an allocator, and why
