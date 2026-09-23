# Errors

An error is two words, passed by value, and a zeroed one means nothing went wrong.

<!-- not compiled: the definition in burrow/core.h, shown for reference -->
```c
typedef struct Error {
    const ErrorVT *vt;
    const void *data;
} Error;
```

That is the same shape an interface value has anywhere else in the library, because in Go that is exactly what `error` is: an interface with one method. The vtable says what kind of error it is and the data points at the error's own state.

Anything that can fail returns one, and the caller asks:

<!-- example: ../examples/errors/errors.c#check -->
```c
Error err = save_config(path, data);
if (BURROW_FAILED(err))
    printf("nope: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
```

Succeeding costs nothing. There is no allocation on the happy path, no object to free, and a struct with an `Error` field in it starts out holding no error without anybody writing a line to say so.

## Asking whether it failed

Write `BURROW_FAILED` or `BURROW_OK`. Do not compare against `BURROW_NO_ERROR`, because C has no `==` on structs and the comparison people write by hand compares both words, which gives the wrong answer for any error whose data pointer happens to be `NULL`. Only `vt` decides.

<!-- example: ../examples/errors/errors.c#failed -->
```c
if (BURROW_FAILED(err)) {
    printf("failed\n");
}
if (BURROW_OK(err)) {
    printf("saved\n");
}
```

With `BURROW_SHORT` those are `FAILED` and `OK`, and `NO_ERROR` is the zero value.

## Getting the message

<!-- example: ../examples/errors/errors.c#message -->
```c
Str msg = error_text(err);
```

This is Go's `err.Error()`. It borrows, so the result points into the error and lives as long as the error does. No error gives you the empty string rather than stopping the program, because this gets called from log lines and a logging call that can take the process down is worse than a blank message.

The name is `error_text` and not `error_error`. Go's method is called `Error` on a type called `error`, and the mechanical rule would produce a function whose name says the same word twice. This is the only place in the library where a Go name is not carried across letter for letter, and it exists because that one collision reads badly several thousand times.

The message is built when the error is constructed, not when somebody prints it. Go builds it in `Error()` on demand, which saves an allocation for an error nobody prints. Here the printing path takes no allocator, so it cannot allocate and therefore cannot fail. On an error path that trade is the right way round: the case you care about is the one where memory is already tight and you still want to know what happened.

## Making one

<!-- example: ../examples/errors/errors.c#new -->
```c
Error err = errors_new(a, BURROW_S("record not found"));
```

The text is copied, so the result does not depend on the buffer it came from. Two calls with the same text give two different errors that are not `errors_is` each other, which is Go's behaviour and is exactly why sentinels are package level variables rather than something you build where you need it.

## Where errors live

`errors_new` takes an allocator, and most of the library does not have one to give it. `strconv_atoi` takes a string and returns a number, and in Go it makes a fresh `*NumError` when the string is bad and lets the garbage collector deal with it. burrow keeps an arena for exactly that, one per goroutine, and `error_allocator()` hands it out. The library uses it for the errors it makes, and you can too.

An error from it lives until the goroutine that made it ends. A goroutine that runs for the life of the program and fails often, a server loop for instance, marks the arena before each piece of work and releases it after, and whatever errors that piece of work made go with it:

<!-- example: ../examples/errors/errors.c#scope -->
```c
for (Int i = 0; i < 100000; i++) {
    ArenaMark m = error_mark();
    Error err = handle(i);
    if (BURROW_FAILED(err))
        last = error_retain(a, err); /* kept past the release */
    error_release(m);
}
```

`error_retain` copies an error out into an allocator you choose. Use it for an error that has to outlive its mark or its goroutine, and above all for one you send to another goroutine, since the goroutine that made it may be gone by the time anyone reads it. A sentinel comes back as it is. An error with a `clone` slot in its vtable is copied by that. Anything else keeps its message and the sentinels in its chain, so `errors_is` still works, but not its type, so `errors_as` does not.

A release that never happens, because of an early return or a panic, is not a bug in the dangerous sense. The memory is held until an outer release or the end of the goroutine, and nothing ends up pointing at memory that was freed early.

## Sentinels

Most errors in Go's library are not constructed, they are compared. `io.EOF`, `os.ErrNotExist`, `sql.ErrNoRows`, and several hundred more. Those are static here:

<!-- example: ../examples/errors/errors.c#sentinel -->
```c
/* in the .c file */
BURROW_SENTINEL_ERROR(err_not_found, "record not found");

/* in the header */
extern const Error err_not_found;
```

That is how the library declares `io_eof` and the rest of its own. The expansion is a `const` struct the linker fills in, so a sentinel costs a `Str` and two words of read only memory, no code at all, and nothing to clean up. All the sentinels in the library share one vtable. Comparing against one is two pointer loads and a branch.

<!-- example: ../examples/errors/errors.c#is -->
```c
if (errors_is(err, io_eof))
    break;
```

Every sentinel is distinct from every other one without anybody assigning numbers, because a sentinel's data points at its own static message and identity is both words matching.

Only ever hand the macro a string literal, the same rule as `BURROW_S`. Note that the macro spells the `Str` out in braces instead of using `BURROW_S`, because `BURROW_S` is a compound literal and C11 does not accept one of those as the initialiser for an object with static storage. If you write a static `Str` of your own, spell it out the same way.

## Wrapping, and walking what you get

Two vtable slots carry the wrapping. `unwrap` is Go's `Unwrap() error`, the chain form, and it is what almost every wrapping error uses. `unwrap_multi` is Go's `Unwrap() []error`, the tree form, and it is what `errors.Join` produces.

<!-- example: ../examples/errors/errors.c#walk -->
```c
Error errors_unwrap(Error err);
bool errors_is(Error err, Error target);
const void *errors_as(Error err, const Type *target);
```

`errors_unwrap` peels one layer. An error that does not wrap, and no error at all, both give you no error, which is Go returning nil in both cases. A multi error unwraps to nothing, also exactly as in Go, because `errors.Unwrap` is defined over the chain form and says nothing about trees.

`errors_is` walks. At each error it asks whether the error is identical to the target, then asks the error's own `is` slot if it has one, then follows what the error wraps. A tree turns the walk into a depth first search, so a target buried in the second branch of a join two levels down is still found. The walk is one directional: an error that wraps `io_eof` is `errors_is` `io_eof`, and `io_eof` is not `errors_is` the error that wraps it.

`errors_is(err, NO_ERROR)` is true only when `err` is also no error, matching `errors.Is(nil, nil)`.

`errors_as` is Go's `errors.As` with the C spelling. Go takes a pointer to a variable and returns a bool, because Go needs somewhere to put the answer. Here the answer is the return value:

<!-- example: ../examples/errors/errors.c#as -->
```c
const ParseError *pe = errors_as(err, TYPE_OF(ParseError));
if (pe != NULL)
    printf("failed on line %lld\n", (long long)pe->line);
```

`ParseError` there is a struct declared with `BURROW_STRUCT`, so `TYPE_OF(ParseError)` is its descriptor, and its vtable names that descriptor in the first slot. When `os` lands, `OsPathError` works the same way.

`void *` converts to any object pointer type in C, so there is no cast at the call site and no way to ask for one type and be handed another. It also drops both of Go's panics, since there is no nil target and no non pointer target left to complain about. The outermost matching error wins, the same as Go, and a miss gives `NULL` rather than a guess.

Both walks respect a custom slot. An error that wants to match something it is not identical to fills in `is`, and an error that wants to be extracted as a type it is not fills in `as`. Those are Go's optional `Is(error) bool` and `As(any) bool`, promoted into the vtable instead of being discovered by type assertion on every call.

There is no public constructor for a wrapping error yet, because Go does not have one either. Go wraps with `fmt.Errorf` and `%w`, so wrapping arrives with `fmt`. Until then, filling in the vtable by hand is what the library does internally and there is nothing stopping you:

<!-- example: ../examples/errors/errors.c#custom -->
```c
typedef struct MyError {
    Str text;
    Error cause;
} MyError;

static Str my_message(const void *self) {
    return ((const MyError *)self)->text;
}
static Error my_unwrap(const void *self) {
    return ((const MyError *)self)->cause;
}

static const ErrorVT my_vt = {
    NULL, my_message, my_unwrap, NULL, NULL, NULL, NULL,
};
```

The first slot is the type `errors_as` matches on. `NULL` there means the error does not want to be extracted, which is what an unexported type gets you in Go, and it is fine for an error that only exists to carry a message and a cause. Give it a descriptor, as `ParseError` did above, when callers need to get at the fields.

Fill in one of `unwrap` and `unwrap_multi`, not both. A Go type cannot satisfy both, since it has one method of that name, so there is no Go behaviour to copy if you do it anyway. What you get is the chain form, because that is the first case in Go's type switch.

## Joining

<!-- example: ../examples/errors/errors.c#join -->
```c
Error err = errors_join_v(a, 2, close_err, flush_err);
```

`errs` that are no error are dropped. If every one of them is, the result is no error. One survivor is still wrapped in a tree of one, because Go does that and because `errors_is` has to give the same answer whether or not the caller filtered first. The message is the surviving messages joined with a single newline and none trailing.

There is a `Slice` form too, for when you already have the errors in one:

<!-- example: ../examples/errors/errors.c#join-slice -->
```c
Slice errs = slice_make(a, TYPE_ERROR, 0, 4);
errs = BURROW_APPEND(Error, a, errs, err1);
Error all = errors_join(a, errs);
```

`TYPE_ERROR` is the descriptor for Go's `error`. It is declared in `burrow/error.h` rather than with the other builtins in `burrow/type.h`, because it needs `sizeof(Error)` and the type header comes first in the include order.

## When the allocator says no

`errors_new` and `errors_join` are the only functions here that allocate, and both of them return `burrow_err_out_of_memory` when they cannot. That is a sentinel, so it is always there. The failure mode of an error constructor cannot itself be a failure to construct an error, and returning a success by accident would be worse than either.

<!-- example: ../examples/errors/errors.c#oom -->
```c
Error err = errors_new(a, BURROW_S("record not found"));
if (errors_is(err, burrow_err_out_of_memory)) {
    printf("could not even build the error\n");
}
```

The name says `burrow` rather than `errors` because it is not a Go symbol, and keeping it out of Go's namespace is what lets the coverage tool map every `errors_` symbol back to a real declaration in Go's API manifest.

The plan in [docs/design/05-memory.md](../design/05-memory.md) is for every allocator to hold a small reserved block so that this is unreachable in practice. That reserve is not built yet, so today a caller under memory pressure can see it.

## What this does not have yet

`fmt_errorf`, and with it `%w`, which is how Go wraps in practice. It arrives with `fmt`.

Panic and `recover`, which are a separate mechanism and not this one. See [failure.md](failure.md) for which of the three failure modes applies to what.
