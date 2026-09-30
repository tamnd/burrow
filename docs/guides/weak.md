# Weak pointers

`burrow/weak.h` is Go's `weak` package. A `WeakPointer` remembers a pointer without keeping the memory behind it alive: `weak_pointer_value` gives the pointer back while the memory is still there, and `NULL` once it has gone. It is what you want for a cache that should not be the reason something stays around.

## Making one and reading it back

`weak_make` takes any pointer, including one into the middle of an object, and `weak_pointer_value` gives it back. Once the memory is freed it reads `NULL`:

<!-- example: ../examples/weak/weak.c#basic -->
```c
Blob *b = BURROW_NEW(a, Blob);
WeakPointer w = weak_make(b);
printf("%d\n", weak_pointer_value(w) == b); /* 1 */
mem_free(a, b, sizeof(Blob), _Alignof(Blob));
printf("%d\n", weak_pointer_value(w) == NULL); /* 1 */
```

In Go the collector decides when the memory goes. burrow has several allocators, and for all but one of them memory goes when you give it back, so that is when a `WeakPointer` to it starts reading `NULL`:

| Allocator | The memory goes at |
|---|---|
| heap, or any allocator you wrote | `mem_free`, or `mem_realloc`, even when the block grows in place |
| arena | the same, plus `arena_reset`, `arena_release` for everything after the mark, and `arena_free` |
| fixed | the same, plus `fixed_reset` |
| gc | when the collector finds nothing else reaching it, as in Go. `mem_free` does nothing here and changes nothing |

Resetting an arena drops every `WeakPointer` into it at once:

<!-- example: ../examples/weak/weak.c#arena -->
```c
Arena ar;
arena_init(&ar, NULL, 0);
Blob *b = BURROW_NEW(arena_allocator(&ar), Blob);
WeakPointer w = weak_make(b);
arena_reset(&ar);
printf("%d\n", weak_pointer_value(w) == NULL); /* 1 */
arena_free(&ar);
```

## Equality

Two `WeakPointer`s made from the same pointer are equal, and `weak_pointer_eq` compares them. As in Go, pointers to two fields of one struct make different ones:

<!-- example: ../examples/weak/weak.c#eq -->
```c
Blob *b = BURROW_NEW(a, Blob);
WeakPointer w1 = weak_make(b);
WeakPointer w2 = weak_make(b);
WeakPointer wid = weak_make(&b->id);
WeakPointer wsize = weak_make(&b->size);
printf("%d %d\n", weak_pointer_eq(w1, w2),
       weak_pointer_eq(wid, wsize)); /* 1 0 */
mem_free(a, b, sizeof(Blob), _Alignof(Blob));
```

Equality also survives the memory going: after a free, the old `WeakPointer`s are still equal to each other. When the allocator later hands the same address out again, `weak_make` on it gives a new `WeakPointer`, not equal to the old ones, and the old ones keep reading `NULL`. An address being reused never brings a dead `WeakPointer` back.

## Memory no allocator owns

A pointer to a global, a string literal or anything else no allocator handed out never goes away, so its `WeakPointer` never reads `NULL`. Go treats pointers to its own immortal data the same way:

<!-- example: ../examples/weak/weak.c#global -->
```c
WeakPointer w = weak_make(&config);
Blob *c = weak_pointer_value(w);
printf("%d\n", (int)c->size); /* 64 */
```

## What it costs

The table behind the `WeakPointer`s is one per process, behind a lock, and it is only there once something calls `weak_make`. Until then a free costs one extra load. After that, every free and reset looks its range up in the table, which is a binary search. Making a `WeakPointer` and reading one back are both a few nanoseconds.

`WeakPointer` is eight bytes, the same as Go's, and every function here is safe to call from any number of threads.

## Differences from Go

Go stops when it runs out of memory, and `weak_make` returns the zero `WeakPointer` instead, whose value is always `NULL`. Go has no `mem_free`, so the rules for the allocators other than gc are burrow's own, and they follow from what freeing means: once memory is given back, nothing is left for a `WeakPointer` to point at.
