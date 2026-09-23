# defer

Run this when you leave, whichever way you leave. It lives in `burrow/defer.h`.

<!-- example: ../examples/defer/handle.c#handle -->
```c
static Error count_lines(const char *path, Int *lines) {
    BURROW_SCOPE {
        FILE *f = open_file(path);
        if (f == NULL)
            return err_cannot_open;
        BURROW_DEFER(close_file, f);

        for (int c; (c = fgetc(f)) != EOF;)
            if (c == '\n')
                ++*lines;
    }
    BURROW_SCOPE_END;

    return BURROW_NO_ERROR;
}
```

burrow's `os` package is not ported yet, so the examples on this page open files with stdio, and `close_file` is a two line wrapper around `fclose` in the shape a deferred call takes. Nothing about the pattern changes when `os_open` arrives.

The close runs when control leaves the block, and it does not matter how control leaves: falling off the end, `return`, `break`, `continue`, a `goto` out, or a panic once panic lands.

That is the whole feature, and it is worth having in C for the same reason it is worth having in Go. The line that opens a thing and the line that closes it are next to each other, so a reader can see both at once, and the early return somebody adds six months from now cannot forget the second one. The usual C answer is a `goto done` at the bottom, which works and which stops working the moment there are two things to unwind and one of them is conditional.

## The block is not optional

`BURROW_SCOPE` and `BURROW_SCOPE_END` are two halves of one thing and both are needed. `BURROW_SCOPE` opens a block of its own, your braces open another inside it, and `BURROW_SCOPE_END` closes the outer one. The outer one exists because the bookkeeping has to be declared before your first statement, and a macro cannot reach inside braces that come after it.

A `BURROW_DEFER` outside a scope does not compile. That is deliberate. The alternative was a bare `BURROW_DEFER` with no block around it, which is possible on GCC and Clang, because they have the `cleanup` attribute, and impossible on MSVC, which does not. A macro that works on two platforms and silently leaks on the third is worse than one that asks for an extra line on all three, so the extra line is the deal.

Inside the scope, a defer is an ordinary statement and goes anywhere a statement goes, including the body of an `if` or a `for` with no braces on it.

## The rules

Deferred calls run last in first out.

<!-- example: ../examples/defer/order.c#lifo -->
```c
BURROW_SCOPE {
    BURROW_DEFER(say, "a");
    BURROW_DEFER(say, "b");
    BURROW_DEFER(say, "c");
}
BURROW_SCOPE_END;
/* prints c b a */
```

The argument is read when you write the defer and not when it runs, so this prints the word `word` held on the `BURROW_DEFER` line, whatever it holds by the time the scope ends:

<!-- example: ../examples/defer/order.c#argument -->
```c
BURROW_SCOPE {
    const char *word = "first";
    BURROW_DEFER(say, word);
    word = "second";
}
BURROW_SCOPE_END;
/* prints first */
```

There is no way to undo a defer, in this or in Go.

Scopes nest, and the inner one finishes first:

<!-- example: ../examples/defer/order.c#nested -->
```c
BURROW_SCOPE {
    BURROW_DEFER(say, "outer");
    BURROW_SCOPE {
        BURROW_DEFER(say, "inner");
    }
    BURROW_SCOPE_END;
    say("between");
}
BURROW_SCOPE_END;
/* prints inner, between, outer */
```

A deferred call may open scopes and defer things of its own, and those run before the call returns.

## What you can defer

A `Func`, which is a function taking `void *` and the pointer it was made with, so the thing being cleaned up is usually the argument.

<!-- example: ../examples/defer/kinds.c#kinds -->
```c
BURROW_DEFER(close_file, f); /* the common case */
BURROW_DEFER(free, buf);     /* a C function that fits already */
BURROW_DEFER_FUNC(cleanup);  /* a Func you already have */
```

A function that returns something other than `void` does not fit and does not compile. Wrap it in a small static function that ignores the result. If the result is an `Error`, that wrapper is the place where a reader can see you decided to ignore it, which is where that decision belongs.

## Where this differs from Go

Go's `defer` runs at function return. This one runs at scope exit, and there is no way to have Go's rule in C without wrapping every function body in something. If you want it, put the scope around the whole body, which is what the example at the top does.

The difference shows up in a loop, and when it does, this rule is the one people wanted:

<!-- example: ../examples/defer/loop.c#loop -->
```c
for (Int i = 0; i < n; i++) {
    BURROW_SCOPE {
        FILE *f = open_file(paths[i]);
        if (f == NULL)
            continue;
        BURROW_DEFER(close_file, f);

        if (fgets(line, sizeof line, f) != NULL)
            printf("%s says %s\n", paths[i], line);
    }
    BURROW_SCOPE_END;
}
```

Every turn closes its own file. The same loop in Go holds every one of them open until the function returns, which is how a Go program ends up out of file descriptors, and is a mistake written often enough that `go vet` has a check for the shape of it.

Put the scope outside the loop instead and you get Go's behaviour back: the calls pile up and all of them run at the end of the scope. That is legitimate when you meant it, which is the case where every file has to stay open until the whole batch is done, and it is almost never what somebody writing a cleanup in a loop meant.

Everything else is Go's.

## What it costs

A scope is one struct in your stack frame, and the deferred calls live in it. Eight of them fit there. The ninth and everything after it go in one allocation from the heap allocator, which doubles as it fills and is freed before the scope returns, and that is the same trade Go makes for the defers it cannot open-code into the frame.

Eight is the number for two reasons. It is where Go's compiler gives up on open-coding as well, so a scope that allocates here is a function that would allocate there. And it was four first, on the argument that a scope with five cleanups in it is rare enough to pay for the fifth itself, which burrow-bench answered: the fifth call cost about a hundred nanoseconds, because paying means a malloc and a free, and the four empty slots cost sixty four bytes of a frame and no time, because the array is never initialised. It is not a knob, since the number is part of the shape of the struct.

The calls live in the scope rather than one local variable each because the last of them runs after your closing brace, and a variable declared between your braces is dead by then. That is a rule C has always had, an address sanitizer will tell you about it, and it is the kind of thing that works in every test you write and then does not.

## Goroutines

The chain of open scopes belongs to the goroutine and not to the thread, so a goroutine that parks in the middle of a scope and wakes up on another thread still has its deferred calls. That is the reason it is a field of the goroutine, and it costs one load of the current goroutine per scope rather than per deferred call.

`runtime_goexit` runs every scope the goroutine is inside, innermost first, before the goroutine ends. This is Go's rule for `runtime.Goexit` and it is the difference between ending a goroutine and abandoning it.

A thread that is not a goroutine has a chain of its own, which is what makes these macros usable in code that has not started the runtime and in code that runs after `runtime_main` has returned.

## Panic

A panic unwinds the chain this page describes. It runs the deferred calls of every scope between the panic and the catch block, innermost scope first and last in first out inside each scope, which is Go's rule and is most of the reason defer is worth having: a panic closes the files.

That includes the panics the runtime raises itself, an index past the end and the rest of them, so a scope that opened between the bad index and the `BURROW_TRY` above it still gets closed on the way past. [panic.md](panic.md) is the page about the catch block, and [failure.md](failure.md) is the page about which failures panic and which end the process.
