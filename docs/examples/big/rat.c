#include <stdbool.h>
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
    BigRat x = BIG_RAT(a), y = BIG_RAT(a), z = BIG_RAT(a);
    big_rat_set_frac64(&x, 1, 3);
    big_rat_set_string(&y, BURROW_S("0.25"), NULL);
    big_rat_add(&z, &x, &y);
    show("1/3 + 0.25", big_rat_string(&z, a));
    big_rat_mul(&z, &z, &z);
    show("squared", big_rat_string(&z, a));
    show("to 10 places", big_rat_float_string(&z, a, 10));
    // doc: end

    // doc: harmonic
    BigRat h = BIG_RAT(a), term = BIG_RAT(a);
    for (int64_t k = 1; k <= 20; k++)
        big_rat_add(&h, &h, big_rat_set_frac64(&term, 1, k));
    show("H(20)", big_rat_string(&h, a));
    printf("num bits: %d, is int: %s\n", (int)big_int_bit_len(big_rat_num(&h)),
           big_rat_is_int(&h) ? "true" : "false");
    // doc: end

    // doc: float
    BigRat t = BIG_RAT(a);
    big_rat_set_float64(&t, 0.1);
    show("0.1 exactly", big_rat_string(&t, a));
    big_rat_set_string(&t, BURROW_S("1/10"), NULL);
    bool exact;
    double f = big_rat_float64(&t, &exact);
    printf("1/10 as float64: %g, exact: %s\n", f, exact ? "true" : "false");
    // doc: end

    // doc: prec
    bool finite;
    big_rat_set_string(&t, BURROW_S("7/40"), NULL);
    Int n = big_rat_float_prec(&t, &finite);
    printf("7/40 needs %d digits, finite: %s\n", (int)n, finite ? "true" : "false");
    big_rat_set_frac64(&t, 1, 7);
    n = big_rat_float_prec(&t, &finite);
    printf("1/7 needs %d digits, finite: %s\n", (int)n, finite ? "true" : "false");
    // doc: end

    // doc: fmt
    big_rat_set_frac64(&t, -6, 4);
    show("fmt", fmt_sprintf_v(a, "%v %s", BURROW_ANY(TYPE_BIG_RAT, &t),
                              BURROW_ANY(TYPE_BIG_RAT, &t)));
    show("RatString", big_rat_rat_string(&t, a));
    big_rat_set_int64(&t, 4);
    show("String of 4", big_rat_string(&t, a));
    show("RatString of 4", big_rat_rat_string(&t, a));
    // doc: end

    // doc: parse
    const char *inputs[] = {"3/-6", "1e-3", "0x1p-2", "1_000.5", "1/0", "abc"};
    for (size_t i = 0; i < sizeof inputs / sizeof inputs[0]; i++) {
        bool ok;
        big_rat_set_string(&t, str_from_cstr(inputs[i]), &ok);
        if (ok)
            show(inputs[i], big_rat_string(&t, a));
        else
            printf("%s: not a number\n", inputs[i]);
    }
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
1/3 + 0.25: 7/12
squared: 49/144
to 10 places: 0.3402777778
H(20): 55835135/15519504
num bits: 26, is int: false
0.1 exactly: 3602879701896397/36028797018963968
1/10 as float64: 0.1, exact: false
7/40 needs 3 digits, finite: true
1/7 needs 0 digits, finite: false
fmt: -3/2 -3/2
RatString: -3/2
String of 4: 4/1
RatString of 4: 4
3/-6: not a number
1e-3: 1/1000
0x1p-2: 1/4
1_000.5: 2001/2
1/0: not a number
abc: not a number
*/
