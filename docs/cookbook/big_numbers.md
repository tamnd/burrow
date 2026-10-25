# Do arithmetic on very large integers

A `BigInt` holds an integer of any size, taking its memory from the allocator it was made with. Each function writes its result to its first argument and returns it, so calls can nest. `big_int_exp` with a modulus works out a power without making the whole number, and `big_int_set_string` reads a number in any base from 2 to 62.

<!-- example: ../examples/cookbook/big_numbers.c#main -->
```c
#define BURROW_SHORT 1
#include "burrow/burrow.h"
#include "burrow/math/big.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    /* A BigInt grows as it needs to, taking its words from the allocator it
     * was made with. Most functions write their result to the first argument
     * and return it. */
    BigInt f = BIG_INT(a);
    big_int_mul_range(&f, 1, 30);
    Str digits = big_int_string(&f, a);
    fmt_printf_v("30! = %s, %d digits\n", digits, digits.len);

    BigInt two = BIG_INT(a), n = BIG_INT(a), p = BIG_INT(a);
    big_int_set_int64(&two, 2);
    big_int_set_int64(&n, 100);
    big_int_exp(&p, &two, &n, NULL);
    fmt_println_v("2^100 =", big_int_string(&p, a));

    /* With a modulus, exp keeps every step small, so this never makes the
     * whole of 2^100. */
    BigInt m = BIG_INT(a), r = BIG_INT(a);
    big_int_set_int64(&m, 1000);
    big_int_exp(&r, &two, &n, &m);
    fmt_println_v("2^100 mod 1000 =", big_int_string(&r, a));

    /* probably_prime runs 20 rounds of Miller-Rabin and a Baillie-PSW test.
     * 2^127 - 1 is a prime and 2^128 + 1 is not. */
    BigInt one = BIG_INT(a), mp = BIG_INT(a), fp = BIG_INT(a);
    big_int_set_int64(&one, 1);
    big_int_sub(&mp, big_int_lsh(&mp, &one, 127), &one);
    big_int_add(&fp, big_int_lsh(&fp, &one, 128), &one);
    bool mp_prime = big_int_probably_prime(&mp, 20);
    bool fp_prime = big_int_probably_prime(&fp, 20);
    fmt_printf_v("2^127-1 prime: %t\n2^128+1 prime: %t\n", mp_prime, fp_prime);

    bool ok = false;
    BigInt big = BIG_INT(a);
    big_int_set_string(&big, S("deadbeefdeadbeefdeadbeef"), 16, &ok);
    fmt_println_v("0xdeadbeefdeadbeefdeadbeef =", big_int_string(&big, a));
    arena_free(&ar);
}
```

It prints:

```text
30! = 265252859812191058636308480000000, 33 digits
2^100 = 1267650600228229401496703205376
2^100 mod 1000 = 376
2^127-1 prime: true
2^128+1 prime: false
0xdeadbeefdeadbeefdeadbeef = 68915718021581205938132336367
```

The program is [docs/examples/cookbook/big_numbers.c](../examples/cookbook/big_numbers.c). CI builds and runs it and checks that it prints this.
