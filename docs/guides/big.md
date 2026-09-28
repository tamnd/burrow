# Big numbers

`burrow/math/big.h` is Go's `math/big`. So far it has `Int`, a signed integer of any size, and `Rat`, an exact fraction of two of them, each with every method Go gives it. `Float` is next.

The names follow the usual rule: `Int.Add` is `big_int_add`, `Int.ProbablyPrime` is `big_int_probably_prime` and `big.NewInt` is `big_new_int`. As in Go, the receiver comes first and is where the result goes, and a function that sets its receiver returns it, so calls can be chained. The operands and the receiver may be the same Int in any combination, so `big_int_add(&x, &x, &x)` doubles x.

## A first program

A zero `BigInt`, written `{0}`, is the number 0 and ready to use:

<!-- example: ../examples/big/int.c#basics -->
```c
BigInt x = {0}, y = {0}, z = {0};
big_int_set_string(&x, BURROW_S("123456789012345678901234567890"), 10, NULL);
big_int_set_int64(&y, 1000000007);
big_int_mul(&z, &x, &x);
show("x*x", big_int_string(&z, a));
big_int_mod(&z, &z, &y);
show("x*x mod p", big_int_string(&z, a));
big_int_exp(&z, &x, &y, &y);
show("x^p mod p", big_int_string(&z, a));
```

It prints:

```
x*x: 15241578753238836750495351562536198787501905199875019052100
x*x mod p: 562701352
x^p mod p: 197434842
```

The last argument to `big_int_set_string` is a `bool *` that says whether the text was a number, and NULL is fine when you know it is. `big_int_exp` takes the modulus as its last argument, and NULL there means no modulus, the way nil does in Go.

## Memory

An Int keeps its digits in memory from the allocator in its `a` field. `{0}` has NULL there, which means the heap, and gives the memory back when you call `big_int_free`:

<!-- example: ../examples/big/int.c#free -->
```c
big_int_free(&x);
big_int_free(&y);
big_int_free(&z);
```

`BIG_INT(a)` is a zero Int whose digits come from `a`. On an arena there is nothing to free, because the arena takes it all back at once:

<!-- example: ../examples/big/int.c#arena -->
```c
BigInt f = BIG_INT(a);
big_int_mul_range(&f, 1, 50);
show("50!", big_int_string(&f, a));
printf("bits: %d, hex: %.*s\n", (int)big_int_bit_len(&f),
       (int)big_int_text(&f, a, 16).len, (const char *)big_int_text(&f, a, 16).p);
```

A result always goes into the receiver's own memory, which grows when it has to and is never shared with another Int. That means an Int stays valid for as long as its allocator does, whatever else you compute afterwards, and there is no step where you copy a result out.

What a computation needs along the way, such as the partial products of a long multiplication or the quotient digits of a division, comes from a scratch arena that each thread keeps for itself. It is empty again by the time the function returns, so the only memory that outlives a call is the result. Two threads can do big arithmetic at once without waiting on each other, as long as they do not change the same Int. Reading an Int from several threads at once is fine, as it is in Go.

Running out of memory panics, as it does in Go, because none of these functions has an error to report it through.

## Division

Go has three kinds of integer division, and so does burrow. `big_int_quo` and `big_int_rem` truncate towards zero, the way C's `/` and `%` do. `big_int_div` and `big_int_mod` are Euclidean, so the remainder is never negative. `big_int_quo_rem` and `big_int_div_mod` give both at once:

<!-- example: ../examples/big/int.c#divide -->
```c
BigInt n = BIG_INT(a), d = BIG_INT(a), q = BIG_INT(a), r = BIG_INT(a);
big_int_set_int64(&n, -7);
big_int_set_int64(&d, 2);
big_int_quo_rem(&q, &n, &d, &r);
show("QuoRem", fmt_sprintf_v(a, "%v %v", BURROW_ANY(TYPE_BIG_INT, &q),
                             BURROW_ANY(TYPE_BIG_INT, &r)));
big_int_div_mod(&q, &n, &d, &r);
show("DivMod", fmt_sprintf_v(a, "%v %v", BURROW_ANY(TYPE_BIG_INT, &q),
                             BURROW_ANY(TYPE_BIG_INT, &r)));
```

`big_int_divide` is the newest of them. It takes a rounding mode, `BIG_TRUNC`, `BIG_FLOOR`, `BIG_ROUND` or `BIG_CEIL`, and sets a quotient and a remainder that fit that mode. Either of the two may be NULL if you only want the other. Dividing by zero panics with "division by zero", as it does in Go.

## Printing and parsing

`big_int_string` gives the number in decimal and `big_int_text` in any base from 2 to 62. `big_int_append` adds the text to the end of a byte slice. An Int also works with fmt through `TYPE_BIG_INT`, and takes the verbs and flags Go's does: `%b`, `%o`, `%O`, `%d`, `%x` and `%X`, and `%v` for decimal, with width, precision, `+`, `#`, `-`, `0` and space:

<!-- example: ../examples/big/int.c#fmt -->
```c
BigInt p = BIG_INT(a);
big_int_lsh(&p, big_int_set_int64(&p, 1), 127);
big_int_sub(&p, &p, big_new_int(a, 1));
show("fmt",
     fmt_sprintf_v(a, "%d %#x %+.40d", BURROW_ANY(TYPE_BIG_INT, &p),
                   BURROW_ANY(TYPE_BIG_INT, &p), BURROW_ANY(TYPE_BIG_INT, &p)));
printf("prime: %s\n", big_int_probably_prime(&p, 20) ? "true" : "false");
```

`big_int_set_string` reads a number in a given base, or with base 0 works the base out from a prefix such as `0x` and allows underscores between digits, the way a Go literal does. fmt's scanning functions read an Int the same way, with `%d`, `%x` and the other verbs.

## Number theory

`big_int_gcd` gives the greatest common divisor and, if you ask for them, the x and y that make `a*x + b*y` equal to it. Pass NULL for either one you do not need:

<!-- example: ../examples/big/int.c#gcd -->
```c
BigInt g = BIG_INT(a), s = BIG_INT(a), t = BIG_INT(a);
big_int_set_int64(&n, 240);
big_int_set_int64(&d, 46);
big_int_gcd(&g, &s, &t, &n, &d);
show("gcd",
     fmt_sprintf_v(a, "%v = 240*%v + 46*%v", BURROW_ANY(TYPE_BIG_INT, &g),
                   BURROW_ANY(TYPE_BIG_INT, &s), BURROW_ANY(TYPE_BIG_INT, &t)));
```

There is also `big_int_mod_inverse`, `big_int_mod_sqrt`, `big_jacobi`, `big_int_sqrt`, `big_int_binomial` and `big_int_mul_range`. `big_int_probably_prime(x, n)` runs n rounds of Miller-Rabin with random bases and then a Baillie-PSW test, the same as Go's, so it is right for every number below 2⁶⁴ and no number above that is known to fool it.

## Encoding

An Int can be written as JSON, as text and in the gob encoding, and read back from each. The JSON form is a bare number, not a string, and reading it back accepts `null` and leaves the Int as it was:

<!-- example: ../examples/big/int.c#json -->
```c
Slice j = big_int_marshal_json(&p, a, NULL);
printf("json: %.*s\n", (int)j.len, (const char *)j.p);
Error err = big_int_unmarshal_json(&g, slice_from_str(a, BURROW_S("\"12x\"")));
if (BURROW_FAILED(err))
    show("error", error_text(err));
```

`big_int_bytes` and `big_int_set_bytes` give the absolute value as big-endian bytes and read it back, and `big_int_fill_bytes` writes it into a buffer you already have, padded with zeros at the front.

## Fractions

A `BigRat` is a fraction kept in lowest terms, with a numerator and a denominator that are both `BigInt`s. `{0}` is 0 on the heap and `BIG_RAT(a)` is 0 on an allocator, the same as for an Int, and `big_rat_free` gives the heap memory back. The functions are named the same way, so `Rat.SetFrac64` is `big_rat_set_frac64` and `big.NewRat` is `big_new_rat`:

<!-- example: ../examples/big/rat.c#basics -->
```c
BigRat x = BIG_RAT(a), y = BIG_RAT(a), z = BIG_RAT(a);
big_rat_set_frac64(&x, 1, 3);
big_rat_set_string(&y, BURROW_S("0.25"), NULL);
big_rat_add(&z, &x, &y);
show("1/3 + 0.25", big_rat_string(&z, a));
big_rat_mul(&z, &z, &z);
show("squared", big_rat_string(&z, a));
show("to 10 places", big_rat_float_string(&z, a, 10));
```

The result of an addition or a multiplication is reduced by the GCD every time, so a long sum stays as small as it can be. Here is the 20th harmonic number:

<!-- example: ../examples/big/rat.c#harmonic -->
```c
BigRat h = BIG_RAT(a), term = BIG_RAT(a);
for (int64_t k = 1; k <= 20; k++)
    big_rat_add(&h, &h, big_rat_set_frac64(&term, 1, k));
show("H(20)", big_rat_string(&h, a));
printf("num bits: %d, is int: %s\n", (int)big_int_bit_len(big_rat_num(&h)),
       big_rat_is_int(&h) ? "true" : "false");
```

`big_rat_num` and `big_rat_denom` give the two halves as `BigInt`s you can read and change. Go's `Denom` gives a new Int holding 1 for a zero Rat that has never been set, and burrow's writes the 1 into the Rat instead and returns its own denominator, because a C function cannot hand back a new Int without an allocator to put it in. The value of the Rat is the same either way.

A Rat holds any float64 exactly, and the conversion back rounds to the nearest float. Go returns a second result that says whether that was exact, and here it is a `bool *` at the end, which may be NULL:

<!-- example: ../examples/big/rat.c#float -->
```c
BigRat t = BIG_RAT(a);
big_rat_set_float64(&t, 0.1);
show("0.1 exactly", big_rat_string(&t, a));
big_rat_set_string(&t, BURROW_S("1/10"), NULL);
bool exact;
double f = big_rat_float64(&t, &exact);
printf("1/10 as float64: %g, exact: %s\n", f, exact ? "true" : "false");
```

`big_rat_set_float64` returns NULL for an infinity or a NaN, as Go returns nil. `big_rat_float_prec` tells you how many digits after the point a decimal needs to show a fraction exactly, and whether it can at all:

<!-- example: ../examples/big/rat.c#prec -->
```c
bool finite;
big_rat_set_string(&t, BURROW_S("7/40"), NULL);
Int n = big_rat_float_prec(&t, &finite);
printf("7/40 needs %d digits, finite: %s\n", (int)n, finite ? "true" : "false");
big_rat_set_frac64(&t, 1, 7);
n = big_rat_float_prec(&t, &finite);
printf("1/7 needs %d digits, finite: %s\n", (int)n, finite ? "true" : "false");
```

`big_rat_string` always writes the denominator and `big_rat_rat_string` leaves it out when it is 1. fmt prints a Rat through `TYPE_BIG_RAT` with `%v` or `%s`, and scans one the same way:

<!-- example: ../examples/big/rat.c#fmt -->
```c
big_rat_set_frac64(&t, -6, 4);
show("fmt", fmt_sprintf_v(a, "%v %s", BURROW_ANY(TYPE_BIG_RAT, &t),
                          BURROW_ANY(TYPE_BIG_RAT, &t)));
show("RatString", big_rat_rat_string(&t, a));
big_rat_set_int64(&t, 4);
show("String of 4", big_rat_string(&t, a));
show("RatString of 4", big_rat_rat_string(&t, a));
```

`big_rat_set_string` takes a fraction written as `a/b`, or a number with a decimal point, an exponent or a `0x`, `0b` or `0o` prefix, as Go's does. The `bool *` says whether the text was a number:

<!-- example: ../examples/big/rat.c#parse -->
```c
const char *inputs[] = {"3/-6", "1e-3", "0x1p-2", "1_000.5", "1/0", "abc"};
for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
    bool ok;
    big_rat_set_string(&t, str_from_cstr(inputs[i]), &ok);
    if (ok)
        show(inputs[i], big_rat_string(&t, a));
    else
        printf("%s: not a number\n", inputs[i]);
}
```

A Rat also has the text and gob encodings, through `big_rat_marshal_text`, `big_rat_unmarshal_text`, `big_rat_gob_encode` and `big_rat_gob_decode`. Dividing by zero panics, from `big_rat_quo`, `big_rat_inv`, `big_rat_set_frac` and `big_rat_set_frac64` alike.

## How close it is to Go

The tests run a little over 8000 cases generated by Go's own `math/big`, from tools/gen-math-big-tests.sh, on random numbers of every size where the code changes strategy: one word, a few words, sizes past the Karatsuba threshold for multiplication and past the recursive threshold for division. Every case checks every result against Go's, and each one that sets a receiver is run again with the receiver being one of its own operands and again with it on an arena. The operations cover arithmetic, the three kinds of division, bit operations on negative numbers, square roots, exponentiation with and without a modulus, GCD with both cofactors, modular inverses and square roots, primality, conversion to and from text in every base, fmt printing and scanning, and every encoding.

The Rat tests do the same with about 10,000 cases from tools/gen-math-big-rat-tests.sh. They run every string literal in Go's rat tests through SetString, with and without a sign and an exponent, and take each number that parses through the float conversions and every text form. Random fractions go through the arithmetic, and floats near the edges of the denormal and overflow ranges go through the conversion both ways.

## See also

- [Numbers](numbers.md) for the fixed size integer and floating point types.
- [fmt](fmt.md) for the verbs and flags.
- [Allocators](allocators.md) for arenas.
