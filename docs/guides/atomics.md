# Atomics

Go's `sync/atomic`, in `burrow/sync/atomic.h`. Counters that two threads can add to, flags that two threads can flip, pointers that can be swapped out from under a reader, and a box that holds any one value and hands it back whole.

The prefix is `sync_atomic_` rather than `atomic_`, and that is the only difference from Go's names. `atomic_` followed by a lowercase letter belongs to `<stdatomic.h>`, which reserves it for functions the C committee has not written yet. Taking it would mean a name collision on some future compiler and a library that stops building for reasons its users cannot fix. The import path with the slash turned into an underscore is the mapping the rest of the library uses, so this needs no special case in [design/08-naming-abi.md](../design/08-naming-abi.md) either.

## The two halves

<!-- example: ../examples/atomics/funcs.c#add -->
```c
#include "burrow/sync/atomic.h"

static int64_t hits;

void serve(void) {
    sync_atomic_add_int64(&hits, 1);
}
```

The plain functions take the address of an ordinary object. Your counter is an `int64_t`, it lives in your own struct, and code that prints it at shutdown when nothing else is running can read it directly. This is the half to use when the thing being shared is a field you already had.

<!-- example: ../examples/atomics/typed.c#typed -->
```c
static SyncAtomicInt64 hits;

void serve(void) {
    sync_atomic_int64_add(&hits, 1);
}
```

The types are the same operations bound to a value that can only be reached through them. The zero value is ready, there is nothing to initialise, and one of these costs exactly what the integer costs, so putting one in a struct of your own changes neither its size nor its alignment. This is the half to use when you want the next person to have to go out of their way to read the field without an atomic.

Out of their way, not stopped. The field is called `v` and C has no way to hide it. Go hides it by not exporting it, and the nearest C offers is that `hits.v` inside a type called `SyncAtomicInt64` is not something anybody writes by accident, which is most of the value and all that is available.

## What is here

Five integer widths, `int32`, `int64`, `uint32`, `uint64` and `uintptr`, and for each of them `add`, `and`, `or`, `load`, `store`, `swap` and `compare_and_swap`. Pointers get `load`, `store`, `swap` and `compare_and_swap`, with no arithmetic, which is Go's set. That is the whole plain surface, thirty nine functions, and every one of them is Go's function with Go's argument order and Go's return value.

The types are `SyncAtomicBool`, `SyncAtomicInt32`, `SyncAtomicInt64`, `SyncAtomicUint32`, `SyncAtomicUint64`, `SyncAtomicUintptr`, `SyncAtomicPointer` and `SyncAtomicValue`. The integer ones carry all seven operations, `Bool` and `Pointer` carry the four that make sense for them, and `Value` is its own thing and has a section below.

## Add returns the new value

<!-- example: ../examples/atomics/funcs.c#now -->
```c
int64_t now = sync_atomic_add_int64(&hits, 1);
```

`now` is the total after your increment, not before it. That is Go's choice and it is worth knowing because it is the opposite of what the hardware does: every machine's fetch and add instruction hands back the old value, and this puts the delta back on afterwards. Go made that call because a counter that returns its new total is the one people want to print, and anybody who wanted the previous value can subtract the delta they just passed in.

Subtraction is addition of a negative. On the signed functions that is what it looks like. On the unsigned ones it means adding the two's complement, the same as in Go:

<!-- example: ../examples/atomics/funcs.c#sub -->
```c
sync_atomic_add_uint64(&n, ~(uint64_t)0); /* n = n - 1 */
sync_atomic_add_uint64(&n, -(uint64_t)k); /* n = n - k */
```

Signed overflow is undefined behaviour in C, so the signed functions do their arithmetic in the unsigned type underneath and convert back through the wrapping conversion the rest of burrow uses. A counter that wraps past `INT64_MAX` wraps to `INT64_MIN` here, which is defined, is what Go does, and is what the instruction was going to do anyway.

## And and or return the old value

<!-- example: ../examples/atomics/funcs.c#or -->
```c
uint32_t before = sync_atomic_or_uint32(&flags, WRITABLE);
if ((before & WRITABLE) == 0)
    open_for_writing(); /* we are the one who set it */
```

The opposite of add, and this is Go's rule too. For a bitmask it is the useful answer: the bit you just set is a bit you already know about, and the question you were actually asking was whether somebody had got there first.

## Compare and swap

<!-- example: ../examples/atomics/funcs.c#cas -->
```c
for (;;) {
    int64_t old = sync_atomic_load_int64(&n);

    if (sync_atomic_compare_and_swap_int64(&n, old, old * 2))
        break;
}
```

Stores the third argument if the value is the second one, and says whether it did. A failure tells you no and nothing else, so the loop reloads for itself. C11's own compare and swap writes the value it saw back through a pointer, which saves the reload, and this throws that away to keep Go's shape. The loop reads the same either way and the reload is a cache hit on a line you already own.

There is no weak form. Go has none, and on the platforms where the distinction exists the strong one costs a retry in a loop that was already a loop.

## Pointers

<!-- example: ../examples/atomics/funcs.c#push -->
```c
static void *head;

static void push(Node *n) {
    n->next = sync_atomic_load_pointer(&head);
    while (!sync_atomic_compare_and_swap_pointer(&head, n->next, n))
        n->next = sync_atomic_load_pointer(&head);
}
```

`sync_atomic_load_pointer` takes a `void *const *`, so a field you were only lent can be read without casting the const away. The other three take `void **`.

`SyncAtomicPointer` is one type rather than Go's `Pointer[T]`. Go needs the generic because Go has no implicit conversion to and from a pointer to nothing in particular, and C does, both ways and without a cast. What is lost is the compiler noticing that you stored a `Foo` and loaded a `Bar`. What is gained is that a lock free list whose nodes point at each other needs no instantiation, no macro and no second type.

This library has no garbage collector, so a pointer swapped out is a pointer somebody still has to free, and the hard part of a lock free structure in C is not the compare and swap, it is knowing when the last reader of the old node has gone. That problem is not solved here and no atomic solves it.

## Bool

<!-- example: ../examples/atomics/typed.c#bool -->
```c
static SyncAtomicBool stopping;

static void stop(void) {
    if (sync_atomic_bool_compare_and_swap(&stopping, false, true))
        begin_shutdown(); /* only the first caller gets in */
}
```

One 32 bit word underneath, because there is no byte wide operation in the layer below and a flag is never the thing that made your struct too big. `load` and `swap` give back a `bool`, and any non zero word reads as true, so a `SyncAtomicBool` written through some other route still behaves.

## Value

<!-- example: ../examples/atomics/typed.c#value -->
```c
static SyncAtomicValue config;

void reload(Config *next) {
    sync_atomic_value_store(&config, BURROW_ANY(TYPE_CONFIG, next));
}

Config *current(void) {
    return any_assert(sync_atomic_value_load(&config), TYPE_CONFIG);
}
```

`SyncAtomicValue` is Go's `Value`: any one value of any one type, stored and loaded whole. It is the answer for a configuration struct that is replaced wholesale, read on every request and written once an hour.

An `Any` points at its value rather than holding it, so what you store has to outlive the `Value` and every reader that might still be looking at it. A struct in static storage or one that came out of an arena is fine. A compound literal in the function that stored it is not, and that is the mistake to watch for here, the same one [interfaces.md](interfaces.md) describes for `Any` everywhere else.

The rules are Go's. The zero value loads as a nil `Any`. The first store decides the type. Every later store has to use the same type, and one that does not panics rather than quietly putting two shapes in the same box. Storing nil panics as well, because a store of nothing has no meaning here and the value that made it nil is almost always the bug.

`sync_atomic_value_swap` stores and gives back what was there, or a nil `Any` if nothing was. `sync_atomic_value_compare_and_swap` compares through the type descriptor, which means Go's `==` and not an address comparison: two `Str` values with the same bytes in different places are equal here. A nil `old` matches only a `Value` nothing has been stored to, which is how you claim one exactly once.

An `Any` is two words, so publishing one is two stores and a reader could land between them. The type descriptor is written last and read first, so a reader either sees no type and answers nil or sees a type and knows the data beside it was already there. The first store is the one that changes both, and it is serialised: the type word goes to a sentinel that is not a type, then the data, then the real type. A reader seeing the sentinel answers nil, which is the answer it would have given a moment earlier. A second storer seeing it waits, because it does not yet know what type this `Value` is going to be. The wait is a spin, since the thread ahead has two stores left, with a yield to the system after sixty four turns so that one core and two threads cannot livelock.

This is Go's algorithm, from `src/sync/atomic/value.go`, and the whole of it is in `src/sync/value.c`.

## Memory ordering

Everything here is sequentially consistent. That is the only ordering Go's package offers, this is a port of it, and a port that quietly gave you a weaker one would be a port of something else.

If you want an acquire load or a release store, that is `burrow/atomic.h`, which is the layer this is written on and which spells the order into every name. It is internal, its names carry the `burrow__` prefix, and the reason it is not the public face is that the order is the part people get wrong. Reach for it when you have a reason, and then write down what the reason was.

## Alignment, and 32 bit machines

A 64 bit operation on a 32 bit machine is not a single instruction, so it goes through a table of spin locks keyed on the address, the way Go's runtime does. It is slower and it is correct, and nothing about the calls changes. `int64_t` and `uint64_t` need to be eight byte aligned for this, which your compiler already does for a plain object and for a struct field, and `SyncAtomicInt64` and `SyncAtomicUint64` carry the alignment for you, which is why Go's own documentation tells 32 bit users to reach for the types.

The path is tested on every machine, not just the ones that need it: the whole test file is compiled a second time with `BURROW_ATOMIC_FORCE_LOCK64` set. A fallback that nothing builds is a fallback that is already broken.

## What it costs

Nothing above the instruction. Every function except the four `Value` calls is `static inline`, so `sync_atomic_add_int64(&hits, 1)` compiles to one `lock xadd` on x86 and one `ldaddal` on arm64, with the signed conversion folding into the instruction that produced it. The typed methods are the plain functions with the address of the field, which is the same instruction again.

The contention, not the instruction, is what costs. A counter every core adds to is a cache line moving between cores on every increment, and it will be slower than the same counter per core added up at the end whatever library you call it through. That is a fact about the machine rather than about this package.
