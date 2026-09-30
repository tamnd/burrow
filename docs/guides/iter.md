# Iterators

`burrow/iter.h` is Go's `iter` package. It gives sequences a common shape, so that a function which produces values one at a time and a loop which consumes them can be written without knowing about each other. Go added this in 1.23 along with range over func. C has no range over func, so this package also carries `BURROW_RANGE`, a loop macro that does the same job.

Read [Function values](functions.md) first if `BURROW_FN` and `BURROW_CALLF` are new to you. Everything here is built on them.

## Writing a sequence

A sequence is an `IterSeq`, a function value that takes a yield callback. It calls yield once for each value and stops as soon as yield returns false. Here is one that counts from 1 to n:

<!-- example: ../examples/iter/iter.c#seq -->
```c
/* count yields the Ints from 1 to *env, which is the documented contract: each
 * v points at an Int. */
static void count(void *env, IterYield yield) {
    Int n = *(const Int *)env;
    for (Int i = 1; i <= n; i++) {
        if (!BURROW_CALLF(yield, &i))
            return;
    }
}
```

Go's `iter.Seq[V]` is generic and C is not, so yield gets a pointer to the value rather than the value. What type that pointer points at is part of the sequence's contract, the same way the element type of a `Slice` is, and it should be written down next to the function. The pointer is only good until yield returns. Pointing at a local, as `count` does with `i`, is fine.

Checking the result of yield is not optional. A loop that breaks early makes yield return false, and a sequence that keeps going after that is a bug. `iter_pull_next` will panic if it happens under Pull.

## Pushing values through

The cheapest way to consume a sequence is to call it with a yield of your own. This is what Go's compiler turns a range over func loop into, and it costs nothing beyond the calls:

<!-- example: ../examples/iter/iter.c#push -->
```c
static bool print_int(void *env, const void *v) {
    (void)env;
    printf(" %lld", (long long)*(const Int *)v);
    return true;
}
```

<!-- example: ../examples/iter/iter.c#push-call -->
```c
Int n = 5;
IterSeq seq = BURROW_FN(IterSeq, count, &n);
printf("count:");
BURROW_CALLF(seq, BURROW_FN(IterYield, print_int, NULL));
printf("\n");
```

The yield gets the same `env` pointer that went into `BURROW_FN`, so state that the loop body needs, a running total or an output buffer, goes there.

## Range loops

Splitting a loop body into a separate function gets tiresome, and `break` and `continue` do not cross a function boundary. `BURROW_RANGE` reads like Go's `for v := range seq`:

<!-- example: ../examples/iter/iter.c#range -->
```c
Int n = 1000000;
Int sum = 0;
BURROW_RANGE(Int, i, BURROW_FN(IterSeq, count, &n)) {
    if (i % 2 == 0)
        continue;
    if (i > 9)
        break;
    sum += i;
}
printf("sum of odd numbers up to 9: %lld\n", (long long)sum);
```

The first argument is the value type, and the loop variable is a copy, so it is yours to keep after the iteration ends. `break` and `continue` do what they do in Go. Here `count` would go to a million, but the loop stops it at 11 and the sequence sees yield return false.

Two things are different from Go. The loop runs the sequence on a coroutine, so it has to run on a goroutine, which means somewhere under `runtime_main` or `go`. And leaving the loop with `return` or `goto` skips the cleanup and leaks the coroutine. Set a flag, `break`, and act on the flag after the loop.

Loops nest, and each one gets its own coroutine. Keep them on separate lines, since the hidden state is named after the line.

Sequences of pairs are `IterSeq2`, with a yield that takes a key and a value, and `BURROW_RANGE2` loops over them:

<!-- example: ../examples/iter/iter.c#seq2 -->
```c
static void enumerate(void *env, IterYield2 yield) {
    const char *const *words = env;
    for (Int i = 0; words[i] != NULL; i++) {
        if (!BURROW_CALLF(yield, &i, &words[i]))
            return;
    }
}
```

<!-- example: ../examples/iter/iter.c#range2 -->
```c
static const char *const words[] = {"alpha", "beta", "gamma", NULL};
BURROW_RANGE2(Int, i, const char *, w,
              BURROW_FN(IterSeq2, enumerate, (void *)words)) {
    printf("%lld %s\n", (long long)i, w);
}
```

## Pulling values out

Sometimes the loop is the wrong shape. Walking two sequences side by side is the usual case: neither can be the outer loop, because each has to advance one step at a time. `iter_pull` turns a sequence into something you call for the next value, the same as Go's `iter.Pull`:

<!-- example: ../examples/iter/iter.c#pull -->
```c
Int a = 3, b = 5;
IterPull pa, pb;
if (!iter_pull(&pa, BURROW_FN(IterSeq, count, &a)))
    return;
if (!iter_pull(&pb, BURROW_FN(IterSeq, count, &b))) {
    iter_pull_stop(&pa);
    return;
}
printf("zip:");
for (;;) {
    const void *x;
    const void *y;
    bool okx = iter_pull_next(&pa, &x);
    bool oky = iter_pull_next(&pb, &y);
    if (!okx || !oky)
        break;
    printf(" (%lld, %lld)", (long long)*(const Int *)x, (long long)*(const Int *)y);
}
printf("\n");
iter_pull_stop(&pa);
iter_pull_stop(&pb);
```

`iter_pull` answers false only if there was no memory for the coroutine. After that, `iter_pull_next` gives out values until the sequence ends, and then answers false every time. `iter_pull_stop` ends the sequence early, which makes the yield it is waiting in return false. Call it whenever you stop before `iter_pull_next` has said there is nothing left. Calling it again, or after the end, does nothing, so calling it unconditionally is simplest.

The `IterPull` must not move while the sequence runs, because the coroutine keeps a pointer to it. A local that stays in scope until the stop is the normal way to hold one.

If the sequence panics, the panic comes out of the `iter_pull_next` or `iter_pull_stop` that was running it, and a `runtime_goexit` inside the sequence ends the goroutine that called next. Both match Go, and `BURROW_RANGE` inherits them because it is built on Pull.

## What it costs

Pushing through a yield is a plain indirect call per value. Pull switches to the coroutine and back for each value, with no trip through the scheduler. On a loaded M1 Mac that is about 96 ns a value against Go's 56 ns. Starting a pull, which takes a goroutine from the free list, is about 116 ns against Go's 180 to 210 ns. A range loop pays the Pull price, so in a hot loop calling the sequence with your own yield is still the faster choice.
