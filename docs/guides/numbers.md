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

```c
uint32_t h = (h * 16777619u) ^ b;   /* fine, wraps the same in both */
double avg = total / (double)n;     /* fine */
int8_t low = (int8_t)wide;          /* fine, truncates */
```

Reach for `burrow/num.h` for the five cases below and leave everything else alone.

## When it wraps

Signed overflow is defined in Go and undefined in C.

```c
Int n = int_add(a, b);        /* wraps, like Go */
Int m = a + b;                /* undefined behaviour if it overflows */
```

The undefined version is not merely a different answer. A compiler that sees `a + 1 > a` is entitled to fold it to `true`, and an overflow check written that way disappears from your binary without a warning. Sanitiser builds trap on it, which is how you find out, and burrow's CI runs those builds for exactly this reason.

There is a function for each width and each signedness:

```c
int64_add   int64_sub   int64_mul   int64_neg
int32_add   int32_sub   int32_mul   int32_neg
int_add     int_sub     int_mul     int_neg
```

and the same for `int8`, `int16`, `uint8`, `uint16`, `uint32`, `uint64` and `uint`. The unsigned ones are the plain operator with the promotion pinned down, and they exist so that generic code does not have to care which half of the table it is in.

The smallest value of a type negates to itself, in Go and here. `int64_neg(INT64_MIN)` is `INT64_MIN`, because the positive version does not exist.

## Dividing

```c
Int q = int_div(a, b);
Int r = int_mod(a, b);
```

Two things happen here that `/` and `%` do not do.

A zero divisor stops the program with `runtime error: integer divide by zero`, which is the line Go prints. Without the check, the x86 divide instruction raises a hardware exception, and that arrives as a signal with no message in it and a stack trace pointing at an instruction rather than at your bug.

The smallest value divided by `-1` gives the smallest value back, and the remainder is zero. The true answer is one larger than the type can hold, so it wraps, and that is what Go prints. The same x86 instruction faults on this case too. The Go compiler emits its own check here for the same reason this does.

Rounding is the same in both languages and always has been: division truncates towards zero, and the remainder takes the sign of the dividend. `-7 / 2` is `-3` and `-7 % 2` is `-1`.

## Shifting

```c
Uint h = uint_shl(hash, 13);
Int  s = int_shr(value, n);
```

Go answers a shift of any size. Shift left past the width of the type and every bit has gone, so the answer is zero. Shift right past the width and a negative value leaves `-1`, because the sign bit keeps arriving, while everything else leaves zero.

C calls all of that undefined, and on x86 the hardware quietly uses the low six bits of the count, so `1 << 64` comes out as `1`. That is the kind of difference that survives every test you wrote and then breaks on the one input with a large shift in it.

A negative count stops the program with `runtime error: negative shift amount`. Go's shift count is a count and not a direction, so a negative one is a bug in the caller rather than a shift the other way.

## From a float

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

```c
Int total = BURROW_ADD(subtotal, tax);
uint32_t h = BURROW_SHL(hash, 5);
```

`BURROW_ADD`, `BURROW_SUB`, `BURROW_MUL`, `BURROW_NEG`, `BURROW_DIV`, `BURROW_MOD`, `BURROW_SHL` and `BURROW_SHR` pick the function by the type of the first argument, so the second argument has to be the same type. That is Go's rule rather than a limitation of the macro: Go has no mixed type arithmetic either.

The argument has to be exactly one of the eight fixed width types. `Int` and `Uint` arrive as whichever of those they are, so they work. A plain `char` is not `int8_t` and a `long` is not `int64_t` on a platform where `int64_t` is a `long long`, and both of those get a compile error naming the type rather than a silent widening.

None of these are in the `BURROW_SHORT` set. `ADD` and `SHL` are names other people's headers already use.

## What it costs

Nothing, on the arithmetic, and a compare on the rest.

Every function in the header is a `static inline`, and the wrapping is free because two's complement wrapping is what the hardware was going to do anyway. Built with `-O2`, `int_add` is a single `add` instruction and `int_mul` is a single `mul` on both gcc and clang. The generated code was checked rather than assumed, and there is a benchmark in [burrow-bench](https://github.com/tamnd/burrow-bench) that measures it against both the raw operator and Go.

`int_div` adds two compares in front of a divide that already costs twenty to forty cycles, so it does not show up. `int_shl` adds a compare and a conditional move, and no branch at all on the in range path.

## See also

- [zero values and multiple results](conventions.md), the two rules that shape every signature in the library
- `include/burrow/num.h`, which carries the reasoning next to the code
- `tests/num_test.c`, where every answer came from running a Go program on two architectures rather than from reading the C standard
