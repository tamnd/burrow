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
    BigFloat x = BIG_FLOAT(a), y = BIG_FLOAT(a), z = BIG_FLOAT(a);
    big_float_set_prec(&z, 200);
    big_float_set_int64(&x, 1);
    big_float_set_int64(&y, 3);
    big_float_quo(&z, &x, &y);
    show("1/3 to 200 bits", big_float_text(&z, a, 'g', 50));
    show("accuracy", big_accuracy_string(big_float_acc(&z), a));
    printf("as float64: %.17g\n", big_float_float64(&z, NULL));
    // doc: end

    // doc: sqrt2
    BigFloat two = BIG_FLOAT(a), r = BIG_FLOAT(a);
    big_float_set_prec(&r, 1000);
    big_float_set_int64(&two, 2);
    big_float_sqrt(&r, &two);
    show("sqrt(2)", big_float_text(&r, a, 'f', 60));
    // doc: end

    // doc: prec
    BigFloat f = BIG_FLOAT(a);
    big_float_set_float64(&f, 0.1);
    printf("prec of 0.1: %d, min prec: %d\n", (int)big_float_prec(&f),
           (int)big_float_min_prec(&f));
    show("0.1 exactly", big_float_text(&f, a, 'f', 55));
    big_float_set_prec(&f, 8);
    show("0.1 in 8 bits", big_float_text(&f, a, 'f', 20));
    show("accuracy", big_accuracy_string(big_float_acc(&f), a));
    // doc: end

    // doc: mode
    BigRoundingMode modes[] = {BIG_TO_NEAREST_EVEN, BIG_TO_ZERO, BIG_TO_POSITIVE_INF};
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        BigFloat g = BIG_FLOAT(a);
        big_float_set_mode(big_float_set_prec(&g, 2), modes[i]);
        big_float_set_float64(&g, 2.5);
        Str name = big_rounding_mode_string(modes[i], a);
        printf("%.*s: %g\n", (int)name.len, (const char *)name.p,
               big_float_float64(&g, NULL));
    }
    // doc: end

    // doc: parse
    bool ok;
    big_float_set_string(&f, BURROW_S("1.5e1000"), &ok);
    show("1.5e1000", big_float_text(&f, a, 'e', 10));
    big_float_set_string(&f, BURROW_S("0x1.8p-3"), &ok);
    show("0x1.8p-3", big_float_string(&f, a));
    show("fmt", fmt_sprintf_v(a, "%.20f", BURROW_ANY(TYPE_BIG_FLOAT, &f)));
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
1/3 to 200 bits: 0.33333333333333333333333333333333333333333333333333
accuracy: Above
as float64: 0.33333333333333331
sqrt(2): 1.414213562373095048801688724209698078569671875376948073176680
prec of 0.1: 53, min prec: 52
0.1 exactly: 0.1000000000000000055511151231257827021181583404541015625
0.1 in 8 bits: 0.10009765625000000000
accuracy: Above
ToNearestEven: 2
ToZero: 2
ToPositiveInf: 3
1.5e1000: 1.5027499280e+1000
0x1.8p-3: 0.1875
fmt: 0.18750000000000000000
*/
