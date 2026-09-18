# Interfaces

An interface value is two words: a pointer to a vtable and a pointer to the data. That is what Go's is too, and it is why a Go interface costs nothing to pass around and neither does this one.

```c
typedef struct IoReader {
    const IoReaderVT *vt;
    void *data;
} IoReader;
```

There are two kinds, because Go has two kinds. An interface with methods, such as `io.Reader` or `fmt.Stringer`, carries a vtable. The empty interface, Go's `any`, carries a type descriptor instead, because nothing is ever called through it. This guide covers both, and `burrow/iface.h` is where the rules are written down next to the code.

## Using one

Call a method with `BURROW_CALL`, or `BURROW_CALL0` when the method takes no arguments.

```c
Error err = BURROW_NO_ERROR;
Int n = BURROW_CALL(r, read, buf, &err);
```

The receiver goes in first. Go does that too, it just does not make you write it. There are two macros rather than one because C99 needs at least one argument after the format in a variadic macro, and the trick that gets around it is C23, and a method with no arguments is far too common to make people write `BURROW_CALL(v, close, )`.

With `BURROW_SHORT` those are `CALL` and `CALL0`.

## nil

A zeroed interface value is nil, and `BURROW_IFACE_IS_NIL` says so.

```c
IoReader r = {NULL, NULL};
if (BURROW_IFACE_IS_NIL(r)) { ... }
```

This is why the vtable pointer is the first member. A struct that came out of an allocator is zeroed, so an interface field in it is nil before anybody writes a line to say so, exactly as a Go struct's interface fields start out nil.

Calling a method on a nil interface is a nil dereference, in C and in Go both. Neither language checks for you, and burrow does not either, because a check on every call in the library would turn one crash into a different crash at the cost of a branch in every dispatch.

## Implementing one

Two things: a static vtable, and a constructor that hands back the pair. Both go next to your type, and you write them once.

```c
typedef struct Counter {
    Int n;
} Counter;

static Int counter_read(void *self, Slice p, Error *err) {
    Counter *c = (Counter *)self;
    ...
}

static const IoReaderVT counter_reader_vt = {&counter_type, counter_read};

IoReader counter_as_io_reader(Counter *c) {
    IoReader r = {&counter_reader_vt, c};
    return r;
}
```

The vtable is `const` and `static`, so it lives in read only memory and there is one of them per type rather than one per value. The constructor compiles to two moves. Satisfying an interface costs a few words of rodata and nothing at runtime.

The name of the constructor is a rule and not a preference: an adapter from a concrete type to an interface is `<type>_as_<interface>`, which is R10 in [the naming rules](../design/08-naming-abi.md). So `os_file_as_io_reader`, `bytes_buffer_as_io_writer`, `counter_as_io_reader`. When you are looking for the way to pass your thing to something that wants an `io.Reader`, that is the name to grep for.

Note the first member of the vtable. Every vtable in the library starts with a `const Type *self_type`, which is what makes a type assertion possible later. Fill it in with your type's descriptor. Filling it in with `NULL` is allowed and means your type declines to be asserted to, which is the same thing an unexported type gets you in Go.

## Embedding

`io.ReadWriter` is `io.Reader` plus `io.Writer`. In C the outer vtable holds the inner vtables as named members.

```c
typedef struct IoReadWriterVT {
    IoReaderVT reader;
    IoWriterVT writer;
} IoReadWriterVT;
```

A type that is both fills in both halves of one object.

```c
static const IoReadWriterVT pipe_read_writer_vt = {
    {&pipe_type, pipe_read},
    {&pipe_type, pipe_write},
};
```

Narrowing is then the address of a member, which the library does for you.

```c
IoReader r = io_read_writer_as_io_reader(rw);
IoWriter w = io_read_writer_as_io_writer(rw);
```

Those exist because Go does this conversion silently when you pass an `io.ReadWriter` to something that wants an `io.Reader`, and C has no such step. Each one is a member address and a pointer copy, and a nil value in gives a nil value out.

The design document originally called for the wide vtable to be laid out as a prefix compatible superset, so that narrowing would be a cast of the vtable pointer. That works for whichever interface happens to be first and quietly does not for the second, since the second one's function pointers are at an offset the cast knows nothing about. Taking the address of a member is the same instruction, is checked by the compiler, and cannot be got wrong by adding a method. Nothing about the change is visible from outside the library, so it is not a fidelity question and there is no ledger entry for it, but [the design document](../design/04-core-types.md) says what was decided and why.

There is no conversion from one combination to another, which is to say no `io_read_write_closer_as_io_read_writer`. A `ReadWriter` value is two words with nowhere to keep a vtable, and a function cannot return a pointer to one it made on its stack, so producing that value would mean assuming the wide vtable's first two members line up with the narrow one's, which is the assumption this whole design exists to avoid. Go back to the concrete type and ask it for the interface you want. That is what the `_as_` adapters are for and every ported type has the full set.

## Type assertions

Go's `v.(T)` is `iface_assert`, which gives you the pointer when the dynamic type matches and `NULL` when it does not.

```c
OsFile *f = iface_assert(BURROW_IFACE(r), TYPE_OS_FILE);
if (f != NULL) {
    /* it really is a file, so the fast path is available */
}
```

`BURROW_IFACE` is what converts your specific interface value into the generic one the helper takes. It is the one cast in the design, and it is defined rather than clever: C guarantees that a pointer to a struct points at its first member, and every vtable's first member is the `self_type` slot.

There is no comma ok form, because in C the pointer already answers both questions. There is also no version that stops the program on failure, which Go's one result assertion does. A library that ends the process over a failed conversion is not one people can build on, and the caller has the pointer in hand and can see for itself.

The match is pointer identity on the descriptor, not a comparison of names. Two types called `Label` from two packages are two types, which is the mistake Go's `PkgPath` exists to prevent.

`iface_type` gives the descriptor back on its own, which is the beginning of a type switch: get the type, then switch on it.

## any

Go's empty interface is `Any`, and it is a descriptor and a pointer.

```c
typedef struct Any {
    const Type *t;
    void *data;
} Any;
```

This is what `fmt`'s arguments, `json_marshal`'s parameter, a `sync.Map`'s values and `context.WithValue`'s value all become. Build one from a pointer you already have, or from a value:

```c
Int n = 42;
Any a1 = BURROW_ANY(TYPE_INT, &n);
Any a2 = BURROW_ANY_VAL(TYPE_INT, Int, 42);
```

`BURROW_ANY_VAL` puts the value in a compound literal, which lives until the end of the enclosing block. That is exactly long enough for an argument to a print, which is where nearly every `Any` in a program comes from.

It is not long enough for anything you keep. Go hides this problem by copying small values into the interface word and larger ones onto the heap, invisibly. `Any` always points at something, so that something has to outlive the `Any`. When it needs to escape the block, box it:

```c
Any kept = any_box(a, a1);
```

That copies the value into the allocator through the descriptor, so a type with its own copy operation gets it. Nothing here is deep: boxing an `Any` that holds a `Slice` copies the three word header and not the elements, which is what assigning a slice in Go does too. A boxed value is owned by the allocator you passed, so it dies when the arena does.

Getting the value back out is `any_assert`, which is `iface_assert` with a descriptor instead of a vtable.

```c
Int *got = (Int *)any_assert(v, TYPE_INT);
```

## Comparing

`any_equal` is Go's `==` on two interface values. Equal means the same dynamic type and equal values, and the comparison goes through the descriptor, so two `Str` values with different pointers and the same bytes are equal.

```c
Int i = 3;
int64_t j = 3;
any_equal(BURROW_ANY(TYPE_INT, &i), BURROW_ANY(TYPE_INT64, &j));  /* false */
```

The same number and the same bytes, and Go says they are different, because the dynamic types differ. That is the rule that keeps `int(3)` and `int64(3)` apart as keys in a `map[any]int`, and `TYPE_ANY` hashes the dynamic type along with the value so the map agrees with the comparison.

Comparing two values of a type that cannot be compared, a slice or a map or a function, stops the program with Go's message:

```
runtime error: comparing uncomparable type []int
```

Go decides that at run time as well, and has to, because the static type on both sides is `any` and neither compiler can see what is inside.

## What this costs

A call through an interface is a load and an indirect call, the same as Go, with no lookup and no allocation. Converting to an interface is two moves. Narrowing to an embedded interface is a member address and a move. A type assertion is one load and one pointer comparison.

The one thing that is not free is what Go's compiler does that C's cannot: Go will sometimes prove the dynamic type at a call site and inline through it. burrow has no such step, so an interface call here is always an indirect call. Where that matters, the answer is the same as it is in Go when the inliner does not fire, which is to hold the concrete type and call the function directly.

## See also

- [`burrow/iface.h`](../../include/burrow/iface.h) for the three rules, next to the code they are about.
- [`burrow/io.h`](../../include/burrow/io.h) for the first interfaces built on this, and for a worked example of embedding.
- [Errors](errors.md), which is an interface with one method and is the one you will meet first.
- [Types](types.md) for the descriptors that make assertions and `Any` possible.
