# Zero values and multiple results

Two of Go's rules turn up in every header in this library. Neither is hard, both are worth reading once, and after that they are invisible.

## Every zero value is a useful one

Go promises that the zero value of a type is a working value of that type. `var buf bytes.Buffer` is an empty buffer you can write to. A nil map reads as empty. A nil slice appends. A `sync.Mutex` is unlocked. A great deal of Go code is written on that promise and never says so.

C gives you the same bit pattern for free.

<!-- example: ../examples/conventions/zero.c#zero -->
```c
Str s = {0};
Slice parts = {0};
Error err = {0};
```

`BURROW_ZERO(T)` is the same thing where you need a value rather than an initialiser.

<!-- example: ../examples/conventions/zero.c#zero-return -->
```c
static Slice nothing(void) {
    return BURROW_ZERO(Slice);
}

static bool unnamed(Str name) {
    return str_eq(name, BURROW_ZERO(Str));
}
```

It expands to a compound literal, so it cannot initialise something with static storage. Write `= {0}` by hand there, which is the same bits and is a constant expression.

What the rule costs is a constraint on the design of every type in the library: **no type in burrow may require non zero initialisation.** That is why `SyncMutex` is a futex style atomic word rather than a `pthread_mutex_t`, whose initialiser is not portably all zero, and why `BytesBuffer` allocates lazily on its first write rather than in a constructor. If a type here ever needs an init call, the rule has been broken and the type is wrong.

Allocators are the one exception and they are not really an exception. An `Arena` holds memory rather than describing it, so it has `arena_init` for the same reason a file has `open`. Everything that is a value rather than a resource follows the rule.

The rule is checked rather than asserted. C cannot evaluate `str_is_empty` at compile time, so there is no static assertion available, and what stands in for one is a test in `tests/core_test.c` that takes the zero value of every public type and exercises it. It grows by a few lines every time a type lands.

One nuance worth knowing. A zeroed `Slice` is the nil slice, with no length, no capacity and nothing behind it, and it also has no element type. That is the one zero value that cannot do everything its non zero form can, since `slice_append` needs to know how big an element is. Use `slice_nil(TYPE_INT)` when you want a nil slice that can be appended to.

## More than one result

Go functions return two things constantly and C functions return one. The rule:

> The first result comes back. Everything after it is an out parameter at the end of the parameter list, in Go's order. `error` is always last. Any out parameter may be `NULL` to throw that result away.

```go
func Atoi(s string) (int, error)
func ReadFile(name string) ([]byte, error)
```

<!-- example: ../examples/conventions/results.c#declarations -->
```c
Int strconv_atoi(Str s, Error *err);
Slice os_read_file(Alloc *a, Str name, Error *err);
```

Which reads at the call site as:

<!-- example: ../examples/conventions/results.c#call -->
```c
Error err = BURROW_NO_ERROR;
Int n = strconv_atoi(s, &err);
if (BURROW_FAILED(err))
    return err;
```

or, when you do not care why it failed:

<!-- example: ../examples/conventions/results.c#call-null -->
```c
Int n = strconv_atoi(s, NULL);
```

`NULL` being allowed everywhere is the part that has to hold without exception. A caller who wants only the first result should not have to declare a variable to throw away, and a rule with holes in it is one you have to look up every time.

On the writing side that is `BURROW_OUT`, here in a cut down `strconv_atoi` that only knows digits:

<!-- example: ../examples/conventions/results.c#out -->
```c
static Int strconv_atoi(Str s, Error *err) {
    Int n = 0;
    if (s.len == 0) {
        BURROW_OUT(err, strconv_err_syntax);
        return 0;
    }
    for (Int i = 0; i < s.len; i++) {
        if (s.p[i] < '0' || s.p[i] > '9') {
            BURROW_OUT(err, strconv_err_syntax);
            return 0;
        }
        n = n * 10 + (s.p[i] - '0');
    }
    return n;
}
```

It writes through the pointer if there is one and does nothing if there is not. The pointer appears twice in the expansion, so hand it a pointer variable rather than a call with a side effect in it.

### When the first result has no room for failure

Some functions have nothing useful to return, and those return the `Error` directly, which makes the common check read the way it should.

<!-- not compiled: os is not ported yet, so os_write_file does not exist -->
```c
Error err = os_write_file(path, data, 0644);
if (BURROW_FAILED(err))
    return err;
```

Some return a value with no spare bit pattern to signal with. `str_clone` returns a `Str`, and a `Str` has no value that means failure, so a failed allocation gives you the empty string. `slice_make` gives you the nil slice. Both say so on their declaration and both tell you what to compare if you need to tell that apart from an empty result that succeeded. The alternative was an out parameter on the two most common calls in the library, which would have cost every caller something to buy a check that almost nobody writes.

And some have no result in Go at all, but can fail to allocate here, where in Go they can't. Those return `bool`, which is `true` when the work got done. `sync_map_store` and `sync_wait_group_go` are two of them. A `false` leaves everything the way it was.

### Three or more

Go returns three meaningful values rarely, and where it does the port gets a named struct rather than a third pointer.

<!-- example: ../examples/conventions/zero.c#cut -->
```c
typedef struct StringsCutRet {
    Str before;
    Str after;
    bool found;
} StringsCutRet;
```

Slightly verbose, and better than four out parameters in a row where transposing two of them still compiles.

## See also

- [errors](errors.md), for what an `Error` is and how to make one
- [failure](failure.md), for which failures are an `Error`, which panic, and which stop the program
- `include/burrow/core.h`, where both macros live with the reasoning next to them
