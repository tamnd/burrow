# Function values

A Go `func` is two things: the code, and the variables the code captured. C has the first and nothing else, so a function value here is a pair.

<!-- not compiled: what BURROW_FUNC(Filter, bool, Str s) expands to -->
```c
typedef struct Filter {
    bool (*f)(void *env, Str s);
    void *env;
} Filter;
```

That is the same two words an interface is, and for the same reason. A function value is an interface with one method and no name for it.

## Declaring a type

Every function type in a Go signature gets a named type here, declared with `BURROW_FUNC`.

<!-- example: ../examples/functions/filter.c#declare -->
```c
BURROW_FUNC(Filter, bool, Str s);
BURROW_FUNC(ReadFn, Int, Slice p, Error *err);
```

The arguments after the return type are the parameters the caller writes. The macro puts the `void *env` in front of them, so you never write it in the declaration and you always write it in the target.

A function type that takes nothing needs the other macro, `BURROW_FUNC0`, because C99 has no way to write a variadic macro that accepts an empty argument list.

<!-- not compiled: already declared in burrow/func.h, repeating it would be a second typedef -->
```c
BURROW_FUNC0(Func, void);
```

That one is already declared for you. `Func` is Go's `func()`, the plainest function value there is, and it is spelled without a package because it has none. It is what `sync.Once.Do` runs, what a goroutine starts with, what `time.AfterFunc` fires, and what a deferred call is.

## Writing a target

A target is an ordinary static function whose first parameter is the environment. Write the parameter even when you have nothing to remember, and ignore it.

<!-- example: ../examples/functions/filter.c#target -->
```c
static bool is_empty(void *env, Str s) {
    (void)env;
    return s.len == 0;
}
```

This is not a style rule. Calling a function through a pointer of a different type is undefined behaviour in C, and on the targets that check, which is wasm and anything built with control flow integrity, it is a trap rather than a theoretical problem. Declare the parameter. Do not cast the pointer.

## Building one

`BURROW_FN` takes the type, the function and the environment. The type comes first because C needs it to know what the compound literal is.

<!-- example: ../examples/functions/filter.c#build -->
```c
Filter f = BURROW_FN(Filter, is_empty, NULL);
Func done = BURROW_FN(Func, close_it, file);
```

Pass `NULL` for the environment when the target has nothing to remember.

## Calling one

`BURROW_CALLF`, or `BURROW_CALLF0` when the value takes no arguments. Two macros for the same reason `BURROW_CALL` and `BURROW_CALL0` come in a pair.

<!-- example: ../examples/functions/filter.c#call -->
```c
bool keep = BURROW_CALLF(f, line);
BURROW_CALLF0(done);
```

With `BURROW_SHORT` those are `FN`, `CALLF` and `CALLF0`.

## nil

A zeroed function value is nil, and `BURROW_FUNC_IS_NIL` asks.

<!-- example: ../examples/functions/filter.c#nil -->
```c
Filter f = {NULL, NULL};
if (BURROW_FUNC_IS_NIL(f)) {
    printf("f is nil\n");
}
```

The function pointer is the first member so that this works without anybody writing a line to make it work. A struct that came out of an allocator is zeroed, so a function value in it starts out nil, the same way a Go struct's func fields do.

Calling a nil function value is a nil dereference here and a panic in Go. Neither language checks for you.

Nil is also the only comparison there is. Go allows `f == nil` and nothing else, because two closures made from the same function are not usefully the same value, and the rule here is the same.

## The environment is the captured variable

Go moves a captured variable to the heap when it sees a closure outlive the frame it was made in, and it does that silently. Here you write the struct and you decide where it lives.

<!-- example: ../examples/functions/filter.c#env -->
```c
typedef struct PrefixEnv {
    Str prefix;
} PrefixEnv;

static bool has_prefix(void *env, Str s) {
    PrefixEnv *e = (PrefixEnv *)env;
    if (s.len < e->prefix.len)
        return false;
    return str_eq(str_from_bytes(s.p, e->prefix.len), e->prefix);
}
```

The value is then built over one of those:

<!-- example: ../examples/functions/filter.c#value -->
```c
PrefixEnv e = {BURROW_S("go")};
Filter f = BURROW_FN(Filter, has_prefix, &e);
```

Capture by reference comes out of this for free, because the environment is a pointer. Writing to it from inside the target is visible outside afterwards, and two values built with the same environment see each other's writes. That is a Go closure capturing a variable rather than a copy of it, and it is what you want for a counter or an accumulator.

## Lifetime

Whatever `env` points at has to outlive the function value. Nothing in C can see that for you, so this is the one thing about function values that needs thinking about, and it comes down to a single question: does the value outlive the frame that made it?

A value passed to something that finishes before the call returns can keep its environment on the stack. A sort comparator, a filter, a walk callback. The environment is a local, you take its address, and the callee is gone before the function is.

<!-- example: ../examples/functions/filter.c#stack -->
```c
PrefixEnv e = {BURROW_S("go")};
Int n = count_if(lines, BURROW_FN(Filter, has_prefix, &e));
```

A value stored in a struct, handed to a goroutine or registered as a callback needs an environment that lives at least as long as the thing holding it, which in practice means the same allocator.

<!-- example: ../examples/functions/filter.c#heap -->
```c
PrefixEnv *e = BURROW_NEW(a, PrefixEnv);
e->prefix = BURROW_S("go");
h->keep = BURROW_FN(Filter, has_prefix, e);
```

If the two ever disagree you get a use after free, and it will look like a function value that used to work. When in doubt, put the environment in the allocator that owns the thing holding the value.

## Why the environment word is there at all

`qsort` takes a bare function pointer. Every caller who needs state for the comparison has to reach for a global, and then two callers cannot sort at once. Every platform has since grown a `qsort_r` to undo it, and no two of them agree on the argument order.

One word per function value buys out of that permanently. A function value fits in two registers, passing one costs two moves, and calling one is an indirect call the branch predictor gets right. Nothing in burrow takes a bare function pointer.

## Cost

Two words, and the function first.

<!-- example: ../examples/functions/filter.c#cost -->
```c
_Static_assert(sizeof(Filter) == 2 * sizeof(void *), "two words");
```

A call through a function value is a load and an indirect call, which is what a Go closure call is. The compiler cannot inline through it unless it can see which function it is, which is also true in Go.

## See also

`include/burrow/func.h` has the rules written next to the code, and `tests/func_test.c` has a worked example of every shape described here. [The interfaces guide](interfaces.md) covers the other two word pair, and the two are worth reading together, because the rules are the same three rules both times.
