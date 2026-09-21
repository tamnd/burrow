# panic and recover

```c
#include "burrow/panic.h"

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

## setjmp's rule

A local of the function containing the `BURROW_TRY`, modified inside the try block and read in the catch block or after it, has an indeterminate value unless it is `volatile`. That is C's rule for `setjmp` and not something burrow can paper over. In practice it bites the accumulator pattern and nothing else:

```c
volatile Int done = 0;
BURROW_TRY {
    for (Int i = 0; i < n; i++) { step(i); done++; }
}
BURROW_CATCH(p) {
    printf("stopped after %lld\n", (long long)done);
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
