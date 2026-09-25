# Numbers

Go's numeric types are C's numeric types, so most of this guide is a table and then a short list of the places where the two languages give different answers.

Those places are all failure cases. Overflow, divide by zero, a shift wider than the type, a float that does not fit in the integer you are converting it to. Go defines every one of them. C leaves four of them undefined, which means the optimiser is allowed to assume they never happen and to delete the code you wrote around them.

That is what `burrow/num.h` is for, and it is all it is for.

## The types

| Go | burrow | Notes |
| --- | --- | --- |
| `int8` ... `int64` | `int8_t` ... `int64_t` | Plain C, from `stdint.h` |
| `uint8` ... `uint64` | `uint8_t` ... `uint64_t` | Plain C |
| `int` | `Int` | 64 bits on a 64 bit platform, 32 on a 32 bit one, exactly as Go's is |
| `uint` | `Uint` | Same |
| `uintptr` | `Uintptr` | |
| `byte` | `Byte` | Go's `byte` is `uint8` and this is `uint8_t` |
| `rune` | `Rune` | Go's `rune` is `int32`, and it is signed because decoders return a negative length next to it |
| `float32`, `float64` | `float`, `double` | IEEE 754 in both languages |
| `complex64`, `complex128` | `Complex64`, `Complex128` | Structs rather than C's `_Complex`, because MSVC has no `_Complex` in C mode |

## Use the operator when the operator is right

Nothing in this guide asks you to stop writing `+`. Unsigned arithmetic wraps in C exactly the way it wraps in Go. Float arithmetic is IEEE 754 in both. Converting a wide integer to a narrow one truncates on every compiler burrow supports. Comparisons, bitwise and, or, xor and not all mean the same thing in both languages.

<!-- example: ../examples/numbers/num.c#operators -->
```c
h = (h * 16777619u) ^ b;        /* fine, wraps the same in both */
double avg = total / (double)n; /* fine */
int8_t low = (int8_t)wide;      /* fine, truncates */
```

Reach for `burrow/num.h` for the five cases below and leave everything else alone.

## When it wraps

Signed overflow is defined in Go and undefined in C.

<!-- example: ../examples/numbers/num.c#wrap -->
```c
Int n = int_add(a, b); /* wraps, like Go */
Int m = a + b;         /* undefined behaviour if it overflows */
```

The undefined version is not merely a different answer. A compiler that sees `a + 1 > a` is entitled to fold it to `true`, and an overflow check written that way disappears from your binary without a warning. Sanitiser builds trap on it, which is how you find out, and burrow's CI runs those builds for exactly this reason.

There is a function for each width and each signedness:

```
int64_add   int64_sub   int64_mul   int64_neg
int32_add   int32_sub   int32_mul   int32_neg
int_add     int_sub     int_mul     int_neg
```

and the same for `int8`, `int16`, `uint8`, `uint16`, `uint32`, `uint64` and `uint`. The unsigned ones are the plain operator with the promotion pinned down, and they exist so that generic code does not have to care which half of the table it is in.

The smallest value of a type negates to itself, in Go and here. `int64_neg(INT64_MIN)` is `INT64_MIN`, because the positive version does not exist.

## Dividing

<!-- example: ../examples/numbers/num.c#divide -->
```c
Int q = int_div(a, b);
Int r = int_mod(a, b);
```

Two things happen here that `/` and `%` do not do.

A zero divisor panics with `runtime error: integer divide by zero`, which is the line Go prints. Without the check, the x86 divide instruction raises a hardware exception, and that arrives as a signal with no message in it and a stack trace pointing at an instruction rather than at your bug.

The smallest value divided by `-1` gives the smallest value back, and the remainder is zero. The true answer is one larger than the type can hold, so it wraps, and that is what Go prints. The same x86 instruction faults on this case too. The Go compiler emits its own check here for the same reason this does.

Rounding is the same in both languages and always has been: division truncates towards zero, and the remainder takes the sign of the dividend. `-7 / 2` is `-3` and `-7 % 2` is `-1`.

## Shifting

<!-- example: ../examples/numbers/num.c#shift -->
```c
Uint h = uint_shl(hash, 13);
Int s = int_shr(value, n);
```

Go answers a shift of any size. Shift left past the width of the type and every bit has gone, so the answer is zero. Shift right past the width and a negative value leaves `-1`, because the sign bit keeps arriving, while everything else leaves zero.

C calls all of that undefined, and on x86 the hardware quietly uses the low six bits of the count, so `1 << 64` comes out as `1`. That is the kind of difference that survives every test you wrote and then breaks on the one input with a large shift in it.

A negative count panics with `runtime error: negative shift amount`. Go's shift count is a count and not a direction, so a negative one is a bug in the caller rather than a shift the other way.

## From a float

<!-- example: ../examples/numbers/num.c#float -->
```c
Int n = int_from_float64(f);
```

This is the one place where the same Go program gives two different answers on two different machines, and Go's specification says it is allowed to.

`int64(1e300)` prints `-9223372036854775808` on amd64 and `9223372036854775807` on arm64. `int64(math.NaN())` prints the smallest int64 on amd64 and `0` on arm64. Both are correct Go, because the specification calls the out of range result implementation dependent.

A library cannot pass that decision on to its users, so burrow picks one answer and gives it everywhere:

- A value too large in either direction gives the nearest value that fits.
- NaN gives zero.
- Anything that fits truncates towards zero, which is what Go does everywhere.

That is arm64's answer, and wasm's, and Rust's, and it is the one that keeps an index or a checksum inside its own type instead of flipping its sign. If you ported code from a Go program that ran on amd64 and depended on the other answer, this is the line in this guide to remember.

The result cannot tell you it was out of range, since `int64_from_float64(1e300)` and `int64_from_float64(9223372036854775807.0)` give the same number. Check the value before you convert it if you need to know, which is what you would have had to do in Go as well.

There is no `_from_float32` set. A `float` widens to a `double` without losing anything, so pass it to the `float64` version.

## `int` is not `int64`

`Int` is its own type with its own set of functions, even though it is the same width as `int64_t` on the machine you are almost certainly reading this on. That is deliberate and it matches Go, where `int` and `int64` are also distinct types of the same width.

The reason it matters is that the other half of the world is 32 bit. `len()` returns `int`, `strings_index` returns `int`, and Go's own tests for `bytes.Repeat` and `slices.Grow` check the overflow behaviour of a 32 bit `int`. A port that used `int64_t` everywhere would pass tests that the original fails, which is a strange way to claim compatibility.

Write `int_add` for an `Int` and `int64_add` for an `int64_t`. The compiler inlines both to the same instruction on a 64 bit build.

## One spelling for every width

For code that does not know which type it has, there is a generic form of each operation.

<!-- example: ../examples/numbers/num.c#generic -->
```c
Int total = BURROW_ADD(subtotal, tax);
uint32_t h = BURROW_SHL(hash, 5);
```

`BURROW_ADD`, `BURROW_SUB`, `BURROW_MUL`, `BURROW_NEG`, `BURROW_DIV`, `BURROW_MOD`, `BURROW_SHL` and `BURROW_SHR` pick the function by the type of the first argument, so the second argument has to be the same type. That is Go's rule rather than a limitation of the macro: Go has no mixed type arithmetic either.

The argument has to be exactly one of the eight fixed width types. `Int` and `Uint` arrive as whichever of those they are, so they work. A plain `char` is not `int8_t` and a `long` is not `int64_t` on a platform where `int64_t` is a `long long`, and both of those get a compile error naming the type rather than a silent widening.

None of these are in the `BURROW_SHORT` set. `ADD` and `SHL` are names other people's headers already use.

## Bits

`burrow/math/bits.h` is Go's `math/bits`, and the names follow the header path the same way: `bits.Len64` is `bits_len64`, `bits.OnesCount` is `bits_ones_count`, and `bits.UintSize` is `BITS_UINT_SIZE`.

<!-- example: ../examples/numbers/num.c#bits -->
```c
Int width = bits_len64(x);               /* 0 for 0, like Go */
Int set = bits_ones_count64(x);          /* how many bits are 1 */
uint64_t lo, hi = bits_mul64(x, y, &lo); /* the full 128 bit product */
```

The functions that return two results in Go, `Add`, `Sub`, `Mul` and `Div`, return the first one and write the second through a pointer, which can be `NULL` when you only want the first. `bits_div64` panics with Go's messages, `integer divide by zero` for a zero divisor and `integer overflow` when the quotient would not fit.

Everything in the header is a `static inline` over the compiler's builtin where there is one, so `bits_len64` is a `clz` and `bits_mul64` is one widening multiply. Where there is no builtin the header falls back to Go's own portable code, and the tests run a second time against that fallback so that it is not left untested until someone brings an unusual compiler.

## Floating point

`burrow/math.h` is Go's `math` package. `math.Hypot` is `math_hypot`, `math.RoundToEven` is `math_round_to_even`, and the constants are macros with the package in front, so `math.Pi` is `MATH_PI` and `math.MaxInt64` is `MATH_MAX_INT64`.

<!-- example: ../examples/numbers/math.c#basic -->
```c
double h = math_hypot(3, 4);        /* 5 */
double r = math_round(-2.5);        /* -3, half away from zero */
double e = math_round_to_even(2.5); /* 2 */
double p = math_pow(2, 0.5);        /* the square root of 2 */
```

The functions that give back two results in Go return the first and write the second through a pointer at the end, and the pointer can be `NULL`. `Frexp`, `Modf`, `Sincos` and `Lgamma` are the four:

<!-- example: ../examples/numbers/math.c#two -->
```c
Int exp;
double frac = math_frexp(48, &exp); /* 0.75 and 6, since 48 is 0.75 * 2^6 */
double c;
double s = math_sincos(MATH_PI / 6, &c);
double whole = math_modf(-3.25, NULL); /* -3, the fraction is not wanted */
```

The special cases are Go's too. A square root of a negative number is NaN, the log of zero is minus infinity, and `math_is_nan` and `math_is_inf` are how you ask:

<!-- example: ../examples/numbers/math.c#special -->
```c
double nan = math_sqrt(-1);
bool isnan = math_is_nan(nan);
bool inf = math_is_inf(math_log(0), -1); /* log(0) is -Inf */
```

None of this calls the C library. Go carries its own implementations, most of them from FDLIBM, and the answers they give differ in the last bit from glibc's, musl's, Apple's and Microsoft's, which also differ from each other. Every function here is Go's code ported, so `math_sin(x)` gives the bits Go's `math.Sin(x)` gives, on every platform, and the library does not link against libm. The tests check that directly: besides Go's own tests, every function runs over sixteen thousand inputs and the bits are compared with what Go returned for the same inputs.

That only holds if the compiler rounds every multiply and every add the way Go does. Some compilers fuse `a * b + c` into one instruction that rounds once, which changes the last bit, so the source turns that off for itself with the pragmas GCC, Clang and MSVC understand. `math_sqrt` and `math_fma` are the two where the hardware gives the one correctly rounded answer Go's code also gives, so they use it where every machine of the kind has it: the square root instruction on x86-64 and arm64, and the fused multiply and add on arm64. Everywhere else they run Go's software versions.

## Complex numbers

A Go `complex128` is a `Complex128` here, a struct with `re` and `im` fields, and `burrow/math/cmplx.h` is Go's `math/cmplx` over it. The names follow the same rule, so `cmplx.Sqrt` is `cmplx_sqrt` and `cmplx.IsNaN` is `cmplx_is_nan`.

<!-- example: ../examples/numbers/cmplx.c#basic -->
```c
Complex128 z = {3, 4};
double r = cmplx_abs(z);                            /* 5 */
Complex128 w = cmplx_sqrt((Complex128){-4, 0});     /* 0+2i */
Complex128 e = cmplx_exp((Complex128){0, MATH_PI}); /* -1, nearly */
```

C has no operators for a struct, so the ones Go's language gives `complex128` are functions: `cmplx_add`, `cmplx_sub`, `cmplx_mul`, `cmplx_div`, `cmplx_neg` and `cmplx_eq`. Multiplication is the textbook formula, with each product rounded on its own as Go does. Division is Go's runtime routine, which scales to avoid overflow and follows C99's annex G when an infinity or a zero is involved, so dividing by zero gives an infinity and never traps:

<!-- example: ../examples/numbers/cmplx.c#ops -->
```c
Complex128 a = {1, 2}, b = {3, 4};
Complex128 p = cmplx_mul(a, b);                    /* -5+10i */
Complex128 q = cmplx_div(a, b);                    /* 0.44+0.08i */
Complex128 inf = cmplx_div(a, (Complex128){0, 0}); /* +Inf+Inf i, not a crash */
```

`Polar` is the one function with two results, and the angle goes through a pointer at the end like the others in this guide:

<!-- example: ../examples/numbers/cmplx.c#polar -->
```c
double theta;
double m = cmplx_polar((Complex128){0, 2}, &theta); /* 2 and Pi/2 */
Complex128 back = cmplx_rect(m, theta);
```

The tests are Go's `cmath_test.go`, with every special case from annex G, and the same bit for bit comparison against Go that `math` has, run over the operators as well as the functions.

## What it costs

Nothing, on the arithmetic, and a compare on the rest.

Every function in the header is a `static inline`, and the wrapping is free because two's complement wrapping is what the hardware was going to do anyway. Built with `-O2`, `int_add` is a single `add` instruction and `int_mul` is a single `mul` on both gcc and clang. The generated code was checked rather than assumed, and there is a benchmark in [burrow-bench](https://github.com/tamnd/burrow-bench) that measures it against both the raw operator and Go.

`int_div` adds two compares in front of a divide that already costs twenty to forty cycles, so it does not show up. `int_shl` adds a compare and a conditional move, and no branch at all on the in range path.

## See also

- [zero values and multiple results](conventions.md), the two rules that shape every signature in the library
- `include/burrow/num.h`, which carries the reasoning next to the code
- `include/burrow/math.h`, and `tests/math_test.c`, which is Go's `all_test.go` with the tables generated from Go's by `tools/gen-math-tests.sh`
- `include/burrow/math/cmplx.h`, and `tests/cmplx_test.c`, which is Go's `cmath_test.go` with the tables generated by `tools/gen-cmplx-tests.sh`
- `include/burrow/math/bits.h`, and `tests/bits_test.c`, which is Go's `bits_test.go` ported line for line
- `tests/num_test.c`, where every answer came from running a Go program on two architectures rather than from reading the C standard
