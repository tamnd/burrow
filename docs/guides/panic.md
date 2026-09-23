# panic and recover

<!-- example: ../examples/panic/parse.c#try -->
```c
BURROW_TRY {
    parse(input);
}
BURROW_CATCH(p) {
    log_bad_input(panic_text(p));
    err = errors_new(a, BURROW_S("bad input"));
}
BURROW_TRY_END;
```

`panic` stops what you are doing and unwinds until something catches it. On the way out every deferred call of every scope between the panic and the catch runs, innermost scope first and last in first out inside each scope. That last part is the reason this is worth having over returning early: a panic closes the files.

All of it is in `burrow/panic.h`.

If nothing catches it, the value is printed and the process ends with status 2, the same as an unrecovered panic in Go.

## When to reach for it

The same line Go draws, in the same place. A panic is for the condition the caller cannot have wanted to hear about through a return value, because the call was already wrong: an index past the end, a nil map written to, a state machine in a state it has no case for. Everything that can fail in the ordinary course of business still returns an `Error`, and there is a page on which is which: [failure.md](failure.md).

The rule of thumb that holds up is about who the audience is. An `Error` is for the person running the program. A panic is for the person who wrote it.

## The block

Three macros, and the shape is the one `BURROW_SCOPE` has for the same reason. The bookkeeping has to be declared before your first statement, a macro cannot reach inside braces that come after it, so `BURROW_TRY` opens a block of its own and `BURROW_TRY_END` closes it. Forgetting the end is a compile error rather than something subtler.

`BURROW_CATCH` names the value that was panicked with. It runs only if the block above it panicked. The name is scoped to the catch block, so you can call it `p` every time.

A `BURROW_TRY` is not a scope. If you want deferred calls inside it, open a `BURROW_SCOPE` inside it, the same as anywhere else. The two are kept apart because most scopes never want a recovery point and a recovery point costs more than a scope does.

## Where this differs from Go

This is the one deviation in the feature and it is worth reading before you write any of it.

Go recovers inside a deferred function, and the function that deferred it then returns normally:

```go
func parse(b []byte) (err error) {
    defer func() {
        if r := recover(); r != nil { err = fmt.Errorf("%v", r) }
    }()
    ...
}
```

burrow recovers in a catch block, and there is no `recover` function to call from a deferred call. The reason is C and not taste. Resuming in the frame that recovered means that frame has to have saved somewhere to come back to, which is a `setjmp`, and a `setjmp` is a couple of hundred bytes of `jmp_buf` in the frame plus a call plus a compiler that stops keeping your locals in registers across it. Go's rule needs one of those in every function that has a `defer` in it. Ours needs one only in the functions that actually recover, which in a program of any size is a handful of them, and everybody else keeps a `defer` that is two stores and a call.

What you lose is the shape above, where the recovery sits next to the thing being guarded rather than around it. What you get is a catch block, which is where a C programmer looks for it anyway, and a defer cheap enough to use everywhere.

Everything else is Go's, including the corners:

| Behaviour | burrow | Go |
| --- | --- | --- |
| Deferred calls run on the way out, innermost first | Yes | Yes |
| A panic inside a deferred call chains onto the one unwinding | Yes | Yes |
| The rest of that scope's deferred calls still run afterwards | Yes | Yes |
| An unrecovered panic prints and exits with status 2 | Yes | Yes |
| A panic does not cross a goroutine | Yes | Yes |
| `panic` with nil gets you a value that says so | Yes | Yes, since 1.21 |
| Recovery happens in a deferred function | No, in a catch block | Yes |

The chaining row is the one people forget exists. If a deferred call panics while a panic is already unwinding, the new one takes over and the old one is remembered, so an unrecovered pair prints both. The deferred calls beside the one that panicked still run, which is what keeps a failure in one cleanup from skipping the rest of them.

## The value

`panic` takes an `Any`, which is a type descriptor and a pointer, and that is Go's `any`, which is what `recover` gives you there.

<!-- not compiled: three alternatives, and only the first would ever run -->
```c
panic(BURROW_ANY_VAL(TYPE_INT, Int, 42));
panic(BURROW_ANY(TYPE_ERROR, &err));
panic_str(BURROW_S("no case for this state"));
```

`panic_str` is there because `panic("some text")` is what most panics in Go are, and going through `Any` for a string literal reads badly.

The pointer is the part to be careful with, because the frame it points into is gone by the time the catch block runs. burrow handles the common half of that for you: a value of thirty two bytes or less is copied into the catching frame before the jump, which covers `Str`, `Error`, `Any`, every number and every small struct. Something larger keeps pointing where it pointed, so it has to live somewhere that outlives the jump.

Copying the value is not copying what the value points at in turn, and that is the part worth reading twice. A panicked `Str` arrives with its pointer and length intact and the bytes are still wherever they were, so a message formatted into a buffer in the panicking frame is a message the catch block cannot read. Panic with a literal, or copy the bytes somewhere that outlives the jump first. It is the same rule `Str` follows everywhere else in burrow, but the jump makes it easier to forget.

`panic_text` turns a value into something printable without allocating. It knows nil, strings, errors, bools, the integer and float kinds, and for anything else it gives you the type's name, which is what the default printer uses too.

`panic_value` tells a deferred call whether it is running because of a panic. It returns the value being unwound, or a nil `Any` when the scope is closing normally, which is the question Go's deferred functions ask `recover` and burrow has no `recover` to ask.

## The runtime's own

An index past the end, a slice expression past the capacity, a divide by zero, a write to a nil map, a send on a closed channel. These panic with an `Error` whose concrete type is `RuntimeError`, which is Go's `runtime.Error`, and asking is one call:

<!-- example: ../examples/panic/runtime.c#catch -->
```c
BURROW_CATCH(p) {
    const RuntimeError *re = runtime_error_from(p);
    if (re == NULL)
        panic(p);
    log_crash(re->message);
}
```

`runtime_error_from` gives back `NULL` for every other panicked value, including an ordinary `Error`, so the check separates a bug in the code from a panic the program raised on purpose. Re-panicking from inside a catch block, as above, goes outward to the next one rather than back into the block that is running, which is what makes that pattern work.

The value is a plain `Error` underneath, so `errors_as(err, TYPE_RUNTIME_ERROR)` finds it through a wrapper and `error_message` prints it. The message is Go's text byte for byte, because those strings are what somebody pastes into a search box.

The message borrows. It lives in a fixed slot on the goroutine, `BURROW_RUNTIME_ERROR_MAX` bytes of it, and the next runtime error on the same goroutine writes over it, so copy the bytes if they need to outlive the catch block. The slot is there because this path cannot allocate: running out of memory is one of the things that arrives on it.

`runtime_panic` raises one with a message of your own, which is what a container of your own writes for its own bounds check. It copies the bytes, so the message may point into your frame.

## When nothing catches it

A panic with no catch block above it prints what it was and where it was, and exits with status 2:

```
panic: runtime error: index out of range [5] with length 3

goroutine 1 [running]:
	0x100e09408
	0x100e08750
	0x18fc02b98
```

The frames are innermost first, and the first one is the call that went wrong rather than anything inside burrow. A panic the runtime raised for you gets the same treatment as one you raised yourself, so an index panic starts at the function doing the indexing and not at the bounds check.

They are addresses rather than names because burrow does not have a symbol table yet. Turning one into a file and a line is one command. On Linux, `addr2line -e ./yourprogram -f -C 0x100e09408`. On macOS, `atos -o ./yourprogram -l <load address> 0x100e09408`, where the load address is what `vmmap` reports for the binary or what `_dyld_get_image_vmaddr_slide(0)` plus `0x100000000` comes to. Build with `-g` if you want line numbers, and keep `-fno-omit-frame-pointer` on, which is the flag that makes the frames visible at all.

Sixty four frames is as far as it goes. Deeper than that and the ones you want are in there anyway, since they are at the top.

You can take the same walk yourself:

<!-- example: ../examples/panic/callers.c#walk -->
```c
Uintptr pcs[32];
Int n = runtime_callers(0, slice_from(pcs, 32, 32, TYPE_UINTPTR));
```

This is Go's `runtime.Callers` with Go's numbering, where frame 0 is `runtime_callers` itself and frame 1 is whoever called it, so a logger that wants its own caller passes 2. It writes nothing but addresses, it allocates nothing, and it answers zero on an architecture burrow has no frame layout for, which today is anything that is not amd64, arm64 or 386. On Windows it asks the operating system instead, so it works under MSVC where there is no frame pointer to follow.

## setjmp's rule

A local of the function containing the `BURROW_TRY`, modified inside the try block and read in the catch block or after it, has an indeterminate value unless it is `volatile`. That is C's rule for `setjmp` and not something burrow can paper over. In practice it bites the accumulator pattern and nothing else:

<!-- example: ../examples/panic/volatile.c#body -->
```c
volatile Int done = 0;
BURROW_TRY {
    for (Int i = 0; i < n; i++) {
        step(i);
        done++;
    }
}
BURROW_CATCH(p) {
    Str why = panic_text(p);
    printf("stopped after %lld: %.*s\n", (long long)done, (int)why.len,
           (const char *)why.p);
}
BURROW_TRY_END;
```

gcc warns about some of these under `-Wextra` as `-Wclobbered`, and it warns about plenty it should not, so read what it says rather than obeying it.

## What it costs

A `BURROW_TRY` is a `jmp_buf` in the frame and a call to `setjmp`, and a `jmp_buf` is a couple of hundred bytes on glibc and rather less elsewhere. That is the whole reason this is a separate construct rather than something every scope carries.

Code with no `BURROW_TRY` in it pays nothing at all. `panic` is a call nobody makes, and the defer chain is walked by the panic rather than by the scopes, so a scope does not get more expensive because panic exists.

Panicking costs a walk of the open scopes, the deferred calls in them, and a `longjmp`. There is no unwinder, no allocation, and no table lookup, which means it is fast enough that a program could use it as control flow. Do not. It is fast so that the cases that should panic are not also slow.

## How it is built

No C++ exceptions, no `_Unwind_RaiseException`, and no SEH `RaiseException` on Windows. It is `setjmp` and `longjmp` everywhere, and the unwinding is done by hand: the panic walks the defer chain and closes each scope itself, then jumps.

Doing it by hand is what makes the three platforms behave identically rather than nearly identically, and it is also why there is nothing to configure. The chain of open scopes was already on the goroutine so that a goroutine parking mid scope keeps its defers, and the panic state sits right next to it for the same reason.

On GCC and Clang the block's bookkeeping is torn down by a `cleanup` attribute. MSVC has no such attribute, so there the macros use `__try` and `__finally`, which is the same idea spelled differently. A `longjmp` on MSVC runs the `__finally` blocks of the frames it jumps over, including scopes the panic has already closed by hand, so closing a scope twice has to be harmless, and it is.

`tools/check-banned.sh` refuses a `setjmp` anywhere else in the tree. A jump that is not a panic skips deferred calls, and this is the one jump that does not.
