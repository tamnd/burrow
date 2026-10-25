# Coming from C

You know C and you want Go's standard library in it. burrow is that library, ported from Go's sources package by package, with Go's tests. It is still C: you compile it with your compiler, link it like any other library and call it from your own `main`. What changes is a handful of habits that Go's library assumes, and this page is about those.

Each section is a few lines from one program, [fromc.c](../examples/fromc/fromc.c), which CI compiles and runs. It prints this:

<!-- not compiled: the output of ../examples/fromc/fromc.c -->
```text
name is gopher, 6 bytes
gopher
loading the config: open no/such/file: no such file or directory
true
caught: runtime error: index out of range [5] with length 3
10000 goroutines slept
```

## A string has a length

A `Str` is a pointer and a length. It isn't NUL-terminated, and burrow's functions don't need it to be:

<!-- not compiled: the definition in burrow/core.h, shown for reference -->
```c
typedef struct Str {
    const Byte *p;
    Int len;
} Str;
```

Taking a piece of a string doesn't copy it, because the piece is just another pointer and length into the same bytes. `strings_cut` here hands back two strings that both point into `line`, and `printf` prints them with `%.*s`, which is what `STR_FMT` and `STR_ARG` spell:

<!-- example: ../examples/fromc/fromc.c#str -->
```c
Str line = S("name=gopher");
Str value;
Str key = strings_cut(line, S("="), &value, NULL);
printf(STR_FMT " is " STR_FMT ", %d bytes\n", STR_ARG(key), STR_ARG(value),
       (int)value.len);
char *c = str_to_cstr(a, value);
puts(c);
```

`S` only takes a string literal, because it uses `sizeof` to get the length. Give it a `char *` and you get the size of a pointer. For a C string you already have, `str_from_cstr` borrows it, measuring it once and copying nothing.

When you need a C string again, to hand to `fopen` or some other library, `str_to_cstr` copies the bytes and adds the NUL. A `Str` can hold a zero byte in the middle, though, and then the C string ends early. If the bytes came from outside your program, check with `str_has_nul` first. A file name that is one thing to burrow and a shorter thing to the C library is how path checks get bypassed.

## Memory comes from an allocator

Every function that allocates takes an `Alloc *` first, the `a` in `str_to_cstr(a, value)` above. That is how burrow manages without a garbage collector. You choose where the memory comes from and when it goes back. The program here makes one arena at the top, allocates from it everywhere and frees it once at the end, and most programs can do the same. There's no `free` for each string and no ownership to track through the calls. [allocators](allocators.md) is the guide to read before any other, because the rest of the library assumes you know it.

## An error is a value, not errno

A call that can fail gives you an `Error`, either as its return value or, when it has something else to return, through a last parameter. `NULL` is allowed there if you don't care. An `Error` is two words. A zeroed one means success, and a failure carries its own message, so nothing is left in a global for the next call to overwrite, and the message says what failed as well as why:

<!-- example: ../examples/fromc/fromc.c#errors -->
```c
Error err = NO_ERROR;
OsFile *f = os_open(a, S("no/such/file"), &err);
if (FAILED(err)) {
    Error wrapped = fmt_errorf_v("loading the config: %w", err);
    bool missing = errors_is(wrapped, os_err_not_exist);
    fmt_println_v(wrapped);
    fmt_println_v(missing);
} else {
    os_file_close(f);
}
```

`%w` wraps one error in another, so the message reads from the outside in, `loading the config: open no/such/file: no such file or directory`. The cause is still there underneath. `errors_is` looks through the wrapping, so a caller three levels up can still ask whether the file was missing without parsing the text. [errors](errors.md) has the rest, including how to declare errors of your own.

## Out of bounds is a panic, not undefined behaviour

C lets you read past the end of an array and gives you whatever was there. burrow checks. `AT` reads an element of a slice, and an index outside it is a panic with Go's message:

<!-- example: ../examples/fromc/fromc.c#bounds -->
```c
BURROW_TRY {
    Int v = AT(Int, s, i);
    printf("%d\n", (int)v);
}
BURROW_CATCH(p) {
    Str text = panic_text(p);
    printf("caught: " STR_FMT "\n", STR_ARG(text));
}
BURROW_TRY_END;
```

A panic unwinds to the nearest `BURROW_TRY`, running deferred calls on the way out. Nothing has to catch it, though. Most code doesn't, and then the message is printed and the process exits with status 2, as an unrecovered panic does in Go. A program that stops and tells you where is easier to deal with than one that keeps going on bad memory. Panics are for mistakes in the program, like a bad index or a write to a nil map. Things that can go wrong in the normal run of a program, like a missing file, are an `Error`. [panic](panic.md) has the details.

## Goroutines are threads you can have thousands of

`runtime_main` starts the scheduler and runs your function as the first goroutine. `go` starts another. A goroutine has its own stack and runs on whichever of the scheduler's threads is free, so it is a thread as far as your code can tell, but starting one costs a few hundred nanoseconds and a page of memory. There are no callbacks. A goroutine that waits, on a channel, a timer or a socket, is parked and its thread goes off to run something else:

<!-- example: ../examples/fromc/fromc.c#goroutines -->
```c
done = chan_make(a, TYPE_INT, 0);
for (int i = 0; i < 10000; i++)
    go(FN(Func, nap, NULL));
Int total = 0, one;
for (int i = 0; i < 10000; i++) {
    chan_recv(done, &one);
    total += one;
}
fmt_println_v(total, "goroutines slept");
```

Each `nap` sleeps for ten milliseconds and then sends on the channel. All ten thousand can be asleep at once on a few threads, because a sleeping goroutine holds its stack and no thread. Sockets work the same way, so a server can have a goroutine for each connection and write plain blocking code in it, the way a Go server does.

One thing is still different from Go. Go can stop a goroutine anywhere to give its thread to another one. burrow can only ask, and the goroutine gives way at its next send, receive, select or `go`, or when it waits for something. A loop that does none of those keeps its thread until it ends. A call to `runtime_preempt_point` in the loop gives it a place to stop. [goroutines](goroutines.md) explains this and the rest of the scheduler, and [channels](channels.md) covers what goroutines use to talk to each other.

## Where next

If you also know Go, [Coming from Go](from-go.md) covers how Go's names, results and interfaces map to C. Otherwise, start with the [tour](../tour.md) and then [allocators](allocators.md).
