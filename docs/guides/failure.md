# How burrow reports failure

Three ways, and which one you get is not a style choice. It follows from whether the caller can do anything about it.

| What happened | What you get | Can you carry on |
| --- | --- | --- |
| The operation did not work | An `Error` return | Yes, that is the point |
| The program's belief about itself is wrong | A panic | Only with `recover` |
| The runtime cannot continue at all | A fatal error | No |

Go draws the same three lines in the same places, and burrow follows it case for case, because the line a Go programmer already knows is the line worth keeping.

Today the middle row does not exist yet, and it is now one step away rather than several. `recover` needs `defer`, `defer` needs the chain of open scopes on the goroutine, and that chain is built and tested: it is [defer.md](defer.md). What is left is the jump itself, which is the next thing. Until it lands, the things that will panic take the fatal path instead. The message text is already the final one, so only the mechanism changes.

## Errors

A file that is not there, a connection that was refused, JSON that does not parse. None of these are bugs. They are the ordinary outcomes of asking the world a question, they happen to correct programs, and the calling code is the only thing that knows what to do about them.

```c
Error err;
Slice data = os_read_file(a, BURROW_S("config.json"), &err);
if (BURROW_FAILED(err))
    return err;
```

An `Error` is a struct rather than a pointer, so the test is `BURROW_FAILED` and not a comparison against `NULL` or against `BURROW_NO_ERROR`. A function whose only result is the error returns it directly, and one with something else to hand back takes an `Error *` last, which is the closest C has to Go's second return value.

This is most of the library. If you are wondering which of the three a given function uses, it is this one. The whole convention, including sentinels, wrapping, `errors_is` and `errors_as`, is one page: [errors.md](errors.md).

## Panics

Indexing past the end of a string, writing to a nil map, dividing by zero, a type assertion that does not hold. The common thread is that none of them can happen in a program that is doing what its author thinks it is doing.

Go panics on these rather than returning an error, and so does burrow, for a reason worth being explicit about: returning an error here would mean every index expression has a failure branch, so nobody would check any of them, and the failure would be discovered later at a place that has nothing to do with the cause. A bug that stops the program at the line that caused it is worth more than one that is reported politely three subsystems away.

## Fatal errors

Not recoverable and not catchable. Go calls these fatal errors too, and prints them the same way:

```
fatal error: runtime error: index out of range [5] with length 3
```

Then the process ends with status 2, which is what an unrecovered Go panic exits with.

The function is `runtime_throw`, it lives in `burrow/runtime.h`, and you can call it. The bar for doing so is the same as the bar the library holds itself to: you have established that the program's state is not what the program believes it to be, and there is nothing sensible to return to. If you are reporting a bad argument, that is not this, return an `Error`.

## Where the message goes

The default puts it on standard error. That is right for a program and wrong for several places burrow is meant to run. A kernel module has no standard error. A wasm host would rather have the string than a trap. An embedded target often has exactly one place to leave a last message and it is not a file descriptor.

So the destination is yours to pick:

```c
static void to_the_log(Str msg) {
    my_log_write(msg.p, msg.len);
    my_reboot();
}

int main(void) {
    runtime_set_fatal_handler(to_the_log);
    ...
}
```

Two things about handlers, both of which matter.

A handler must not return. If yours does, burrow ends the process anyway. This is not burrow being stubborn, it is that returning would resume code which has already been told its assumptions do not hold, and the next thing that code does might be to write a file. Installing a handler is how you choose where the note is left, not a way to make a fatal error survivable. Leaving sideways is fine, so a handler that ends in `longjmp` or in a host trap is doing the right thing.

Install it once during startup, before there is a second thread. The hook is a plain pointer with no synchronisation around it, since the atomics layer does not exist yet.

## What the fatal path does not do

It does not allocate. Running out of memory is one of the conditions that will eventually arrive here, and a reporting path that needs an allocator is a reporting path that stops working exactly when it is needed. The messages are formatted into a fixed stack buffer and truncated if they somehow do not fit.

It does not print a stack trace yet. That arrives with the stack walker, near the end of the runtime work, and it is the single thing that will make these messages properly useful. A panic without a traceback is a support problem, and we know it.
