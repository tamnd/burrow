#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"

static void show(const char *label, Str s) {
    printf("%s: %.*s\n", label, (int)s.len, (const char *)s.p);
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: basics
    BigInt x = {0}, y = {0}, z = {0};
    big_int_set_string(&x, BURROW_S("123456789012345678901234567890"), 10, NULL);
    big_int_set_int64(&y, 1000000007);
    big_int_mul(&z, &x, &x);
    show("x*x", big_int_string(&z, a));
    big_int_mod(&z, &z, &y);
    show("x*x mod p", big_int_string(&z, a));
    big_int_exp(&z, &x, &y, &y);
    show("x^p mod p", big_int_string(&z, a));
    // doc: end

    // doc: free
    big_int_free(&x);
    big_int_free(&y);
    big_int_free(&z);
    // doc: end

    // doc: arena
    BigInt f = BIG_INT(a);
    big_int_mul_range(&f, 1, 50);
    show("50!", big_int_string(&f, a));
    printf("bits: %d, hex: %.*s\n", (int)big_int_bit_len(&f),
           (int)big_int_text(&f, a, 16).len, (const char *)big_int_text(&f, a, 16).p);
    // doc: end

    // doc: divide
    BigInt n = BIG_INT(a), d = BIG_INT(a), q = BIG_INT(a), r = BIG_INT(a);
    big_int_set_int64(&n, -7);
    big_int_set_int64(&d, 2);
    big_int_quo_rem(&q, &n, &d, &r);
    show("QuoRem", fmt_sprintf_v(a, "%v %v", BURROW_ANY(TYPE_BIG_INT, &q),
                                 BURROW_ANY(TYPE_BIG_INT, &r)));
    big_int_div_mod(&q, &n, &d, &r);
    show("DivMod", fmt_sprintf_v(a, "%v %v", BURROW_ANY(TYPE_BIG_INT, &q),
                                 BURROW_ANY(TYPE_BIG_INT, &r)));
    // doc: end

    // doc: fmt
    BigInt p = BIG_INT(a);
    big_int_lsh(&p, big_int_set_int64(&p, 1), 127);
    big_int_sub(&p, &p, big_new_int(a, 1));
    show("fmt",
         fmt_sprintf_v(a, "%d %#x %+.40d", BURROW_ANY(TYPE_BIG_INT, &p),
                       BURROW_ANY(TYPE_BIG_INT, &p), BURROW_ANY(TYPE_BIG_INT, &p)));
    printf("prime: %s\n", big_int_probably_prime(&p, 20) ? "true" : "false");
    // doc: end

    // doc: gcd
    BigInt g = BIG_INT(a), s = BIG_INT(a), t = BIG_INT(a);
    big_int_set_int64(&n, 240);
    big_int_set_int64(&d, 46);
    big_int_gcd(&g, &s, &t, &n, &d);
    show("gcd",
         fmt_sprintf_v(a, "%v = 240*%v + 46*%v", BURROW_ANY(TYPE_BIG_INT, &g),
                       BURROW_ANY(TYPE_BIG_INT, &s), BURROW_ANY(TYPE_BIG_INT, &t)));
    // doc: end

    // doc: json
    Slice j = big_int_marshal_json(&p, a, NULL);
    printf("json: %.*s\n", (int)j.len, (const char *)j.p);
    Error err = big_int_unmarshal_json(&g, slice_from_str(a, BURROW_S("\"12x\"")));
    if (BURROW_FAILED(err))
        show("error", error_text(err));
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
x*x: 15241578753238836750495351562536198787501905199875019052100
x*x mod p: 562701352
x^p mod p: 197434842
50!: 30414093201713378043612608166064768844377641568960512000000000000
bits: 215, hex: 49eebc961ed279b02b1ef4f28d19a84f5973a1d2c7800000000000
QuoRem: -3 -1
DivMod: -4 1
fmt: 170141183460469231731687303715884105727 0x7fffffffffffffffffffffffffffffff +0170141183460469231731687303715884105727
prime: true
gcd: 2 = 240*-9 + 46*47
json: 170141183460469231731687303715884105727
error: math/big: cannot unmarshal "\"12x\"" into a *big.Int
*/
