#include "burrow/burrow.h"

static void arith(Alloc *a) {
    // doc: arith
    // (2.3 + 5i) * 11, worked out exactly.
    ConstantValue ar = constant_make_float64(a, 2.3);
    ConstantValue ai = constant_make_imag(a, constant_make_int64(5));
    ConstantValue x = constant_binary_op(a, ar, TOKEN_ADD, ai);
    ConstantValue c = constant_binary_op(a, x, TOKEN_MUL, constant_make_uint64(a, 11));

    bool exact;
    double re = constant_float64_val(constant_real(c), &exact);
    if (!exact)
        fmt_printf_v("real part %s is not exact as a double\n",
                     constant_value_string(constant_real(c), a));
    double im = constant_float64_val(constant_imag(c), &exact);
    fmt_println_v("go/constant", BURROW_ANY(TYPE_CONSTANT_VALUE, &c));
    fmt_println_v("double", re, im, exact);

    // 11 / 0.5
    ConstantValue q = constant_binary_op(a, constant_make_uint64(a, 11), TOKEN_QUO,
                                         constant_make_float64(a, 0.5));
    fmt_println_v(constant_value_string(q, a));
    // doc: end
}

static void unary(Alloc *a) {
    // doc: unary
    ConstantValue vs[] = {
        constant_make_bool(true),
        constant_make_float64(a, 2.7),
        constant_make_uint64(a, 42),
    };
    for (int i = 0; i < 3; i++) {
        switch (constant_value_kind(vs[i])) {
        case CONSTANT_BOOL:
            vs[i] = constant_unary_op(a, TOKEN_NOT, vs[i], 0);
            break;
        case CONSTANT_FLOAT:
            vs[i] = constant_unary_op(a, TOKEN_SUB, vs[i], 0);
            break;
        case CONSTANT_INT:
            // 16 bits of precision, the same as ^uint16(v).
            vs[i] = constant_unary_op(a, TOKEN_XOR, vs[i], 16);
            break;
        default:
            break;
        }
    }
    for (int i = 0; i < 3; i++)
        fmt_println_v(constant_value_string(vs[i], a));
    // doc: end
}

// doc: compare
static int by_value(void *env, const void *x, const void *y) {
    (void)env;
    const ConstantValue *p = x, *q = y;
    if (constant_compare(*p, TOKEN_LSS, *q))
        return -1;
    if (constant_compare(*p, TOKEN_GTR, *q))
        return +1;
    return 0;
}

static void compare(Alloc *a) {
    ConstantValue vs[] = {
        constant_make_string(a, BURROW_S("Z")),
        constant_make_string(a, BURROW_S("bacon")),
        constant_make_string(a, BURROW_S("go")),
        constant_make_string(a, BURROW_S("Frame")),
        constant_make_string(a, BURROW_S("defer")),
        constant_make_from_literal(a, BURROW_S("\"a\""), TOKEN_STRING, 0),
    };
    Slice s = slice_from(vs, 6, 6, TYPE_CONSTANT_VALUE);
    slices_sort_func(s, BURROW_FN(SlicesCmpFunc, by_value, NULL));
    for (int i = 0; i < 6; i++)
        fmt_println_v(constant_string_val(vs[i]));
}
// doc: end

// doc: sign
static ConstantValue mk_complex(Alloc *a, ConstantValue re, ConstantValue im) {
    return constant_binary_op(a, re, TOKEN_ADD, constant_make_imag(a, im));
}

static void sign(Alloc *a) {
    ConstantValue zero = constant_make_int64(0);
    ConstantValue one = constant_make_int64(1);
    ConstantValue neg_one = constant_make_int64(-1);
    ConstantValue vs[] = {
        neg_one,
        mk_complex(a, zero, neg_one),
        mk_complex(a, one, neg_one),
        mk_complex(a, neg_one, one),
        mk_complex(a, neg_one, neg_one),
        zero,
        mk_complex(a, zero, zero),
        one,
        mk_complex(a, zero, one),
        mk_complex(a, one, one),
    };
    for (int i = 0; i < 10; i++)
        fmt_printf_v("% d %s\n", constant_sign(vs[i]), constant_value_string(vs[i], a));
}
// doc: end

static void val(Alloc *a) {
    // doc: val
    ConstantValue vs[] = {
        constant_make_int64(INT64_MAX),
        constant_make_float64(a, MATH_E),
        constant_make_bool(true),
        constant_make(a, BURROW_ANY_VAL(TYPE_BOOL, bool, false)),
    };
    for (int i = 0; i < 4; i++)
        fmt_printf_v("%v\n", constant_val(a, vs[i]));
    // doc: end
}

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    arith(a);
    unary(a);
    compare(a);
    sign(a);
    val(a);
    arena_free(&ar);
    return 0;
}

/* Output:
real part 25.3 is not exact as a double
go/constant (25.3 + 55i)
double 25.299999999999997 55 true
22
false
-2.7
65493
Frame
Z
a
bacon
defer
go
-1 -1
-1 (0 + -1i)
-1 (1 + -1i)
-1 (-1 + 1i)
-1 (-1 + -1i)
 0 0
 0 (0 + 0i)
 1 1
 1 (0 + 1i)
 1 (1 + 1i)
9223372036854775807
6121026514868073/2251799813685248
true
false
*/
