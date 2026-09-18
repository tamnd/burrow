# Errors

An error is two words, passed by value, and a zeroed one means nothing went wrong.

```c
typedef struct Error {
    const ErrorVT *vt;
    const void *data;
} Error;
```

That is the same shape an interface value has anywhere else in the library, because in Go that is exactly what `error` is: an interface with one method. The vtable says what kind of error it is and the data points at the error's own state.

```c
Error err = os_write_file(path, data, 0644);
if (BURROW_FAILED(err))
    printf("nope: " BURROW_STR_FMT "\n", BURROW_STR_ARG(error_message(err)));
```

Succeeding costs nothing. There is no allocation on the happy path, no object to free, and a struct with an `Error` field in it starts out holding no error without anybody writing a line to say so.

## Asking whether it failed

Write `BURROW_FAILED` or `BURROW_OK`. Do not compare against `BURROW_NO_ERROR`, because C has no `==` on structs and the comparison people write by hand compares both words, which gives the wrong answer for any error whose data pointer happens to be `NULL`. Only `vt` decides.

```c
if (BURROW_FAILED(err)) { ... }
if (BURROW_OK(err))     { ... }
```

With `BURROW_SHORT` those are `FAILED` and `OK`, and `NO_ERROR` is the zero value.

## Getting the message

```c
Str msg = error_message(err);
```

This is Go's `err.Error()`. It borrows, so the result points into the error and lives as long as the error does. No error gives you the empty string rather than stopping the program, because this gets called from log lines and a logging call that can take the process down is worse than a blank message.

The name is `error_message` and not `error_error`. Go's method is called `Error` on a type called `error`, and the mechanical rule would produce a function whose name says the same word twice. This is the only place in the library where a Go name is not carried across letter for letter, and it exists because that one collision reads badly several thousand times.

The message is built when the error is constructed, not when somebody prints it. Go builds it in `Error()` on demand, which saves an allocation for an error nobody prints. Here the printing path takes no allocator, so it cannot allocate and therefore cannot fail. On an error path that trade is the right way round: the case you care about is the one where memory is already tight and you still want to know what happened.

## Making one

```c
Error err = errors_new(a, BURROW_S("record not found"));
```

The text is copied, so the result does not depend on the buffer it came from. Two calls with the same text give two different errors that are not `errors_is` each other, which is Go's behaviour and is exactly why sentinels are package level variables rather than something you build where you need it.

## Sentinels

Most errors in Go's library are not constructed, they are compared. `io.EOF`, `os.ErrNotExist`, `sql.ErrNoRows`, and several hundred more. Those are static here:

```c
/* in the .c file */
BURROW_SENTINEL_ERROR(io_eof, "EOF");

/* in the header */
extern const Error io_eof;
```

The expansion is a `const` struct the linker fills in, so a sentinel costs a `Str` and two words of read only memory, no code at all, and nothing to clean up. All the sentinels in the library share one vtable. Comparing against one is two pointer loads and a branch.

```c
if (errors_is(err, io_eof))
    break;
```

Every sentinel is distinct from every other one without anybody assigning numbers, because a sentinel's data points at its own static message and identity is both words matching.

Only ever hand the macro a string literal, the same rule as `BURROW_S`. Note that the macro spells the `Str` out in braces instead of using `BURROW_S`, because `BURROW_S` is a compound literal and C11 does not accept one of those as the initialiser for an object with static storage. If you write a static `Str` of your own, spell it out the same way.

## Wrapping, and walking what you get

Two vtable slots carry the wrapping. `unwrap` is Go's `Unwrap() error`, the chain form, and it is what almost every wrapping error uses. `unwrap_multi` is Go's `Unwrap() []error`, the tree form, and it is what `errors.Join` produces.

```c
Error errors_unwrap(Error err);
bool errors_is(Error err, Error target);
const void *errors_as(Error err, const Type *target);
```

`errors_unwrap` peels one layer. An error that does not wrap, and no error at all, both give you no error, which is Go returning nil in both cases. A multi error unwraps to nothing, also exactly as in Go, because `errors.Unwrap` is defined over the chain form and says nothing about trees.

`errors_is` walks. At each error it asks whether the error is identical to the target, then asks the error's own `is` slot if it has one, then follows what the error wraps. A tree turns the walk into a depth first search, so a target buried in the second branch of a join two levels down is still found. The walk is one directional: an error that wraps `io_eof` is `errors_is` `io_eof`, and `io_eof` is not `errors_is` the error that wraps it.

`errors_is(err, NO_ERROR)` is true only when `err` is also no error, matching `errors.Is(nil, nil)`.

`errors_as` is Go's `errors.As` with the C spelling. Go takes a pointer to a variable and returns a bool, because Go needs somewhere to put the answer. Here the answer is the return value:

```c
const OsPathError *pe = errors_as(err, TYPE_OS_PATH_ERROR);
if (pe != NULL)
    printf("failed on " BURROW_STR_FMT "\n", BURROW_STR_ARG(pe->path));
```

`void *` converts to any object pointer type in C, so there is no cast at the call site and no way to ask for one type and be handed another. It also drops both of Go's panics, since there is no nil target and no non pointer target left to complain about. The outermost matching error wins, the same as Go, and a miss gives `NULL` rather than a guess.

Both walks respect a custom slot. An error that wants to match something it is not identical to fills in `is`, and an error that wants to be extracted as a type it is not fills in `as`. Those are Go's optional `Is(error) bool` and `As(any) bool`, promoted into the vtable instead of being discovered by type assertion on every call.

There is no public constructor for a wrapping error yet, because Go does not have one either. Go wraps with `fmt.Errorf` and `%w`, so wrapping arrives with `fmt`. Until then, filling in the vtable by hand is what the library does internally and there is nothing stopping you:

```c
typedef struct MyError {
    Str text;
    Error cause;
} MyError;

static Str my_message(const void *self)  { return ((const MyError *)self)->text; }
static Error my_unwrap(const void *self) { return ((const MyError *)self)->cause; }

static const ErrorVT my_vt = {
    &my_error_type, my_message, my_unwrap, NULL, NULL, NULL,
};
```

Fill in one of `unwrap` and `unwrap_multi`, not both. A Go type cannot satisfy both, since it has one method of that name, so there is no Go behaviour to copy if you do it anyway. What you get is the chain form, because that is the first case in Go's type switch.

## Joining

```c
Error err = errors_join_v(a, 2, close_err, flush_err);
```

`errs` that are no error are dropped. If every one of them is, the result is no error. One survivor is still wrapped in a tree of one, because Go does that and because `errors_is` has to give the same answer whether or not the caller filtered first. The message is the surviving messages joined with a single newline and none trailing.

There is a `Slice` form too, for when you already have the errors in one:

```c
Slice errs = slice_make(a, TYPE_ERROR, 0, 4);
errs = BURROW_APPEND(Error, a, errs, err1);
Error all = errors_join(a, errs);
```

`TYPE_ERROR` is the descriptor for Go's `error`. It is declared in `burrow/error.h` rather than with the other builtins in `burrow/type.h`, because it needs `sizeof(Error)` and the type header comes first in the include order.

## When the allocator says no

`errors_new` and `errors_join` are the only functions here that allocate, and both of them return `burrow_err_out_of_memory` when they cannot. That is a sentinel, so it is always there. The failure mode of an error constructor cannot itself be a failure to construct an error, and returning a success by accident would be worse than either.

```c
Error err = errors_new(a, BURROW_S("..."));
if (errors_is(err, burrow_err_out_of_memory)) { ... }
```

The name says `burrow` rather than `errors` because it is not a Go symbol, and keeping it out of Go's namespace is what lets the coverage tool map every `errors_` symbol back to a real declaration in Go's API manifest.

The plan in [docs/design/05-memory.md](../design/05-memory.md) is for every allocator to hold a small reserved block so that this is unreachable in practice. That reserve is not built yet, so today a caller under memory pressure can see it.

## What this does not have yet

`fmt_errorf`, and with it `%w`, which is how Go wraps in practice. It arrives with `fmt`.

Panic and `recover`, which are a separate mechanism and not this one. See [failure.md](failure.md) for which of the three failure modes applies to what.
