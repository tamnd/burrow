#include <stdint.h>

#include "burrow/burrow.h"

// doc: checkequal
/* Two ways to take the absolute value of an int32, which should agree. */
BURROW_FUNC(AbsFn, int64_t, int32_t x);
#define ABS_SIG(IN, OUT) IN(0, int32_t) OUT(int64_t)
BURROW_FUNC_TYPE_DEFINE(AbsFn, ABS_SIG);

static int64_t abs_branch(void *env, int32_t x) {
    (void)env;
    return x < 0 ? -(int64_t)x : (int64_t)x;
}

static int64_t abs_bits(void *env, int32_t x) {
    (void)env;
    int64_t v = x;
    int64_t m = v >> 63;
    return (v ^ m) - m;
}

static void TestAbs(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    AbsFn f = {abs_branch, NULL};
    AbsFn g = {abs_bits, NULL};
    Error err = quick_check_equal(a, BURROW_ANY(TYPE_OF(AbsFn), &f),
                                  BURROW_ANY(TYPE_OF(AbsFn), &g), NULL);
    fmt_printf_v("abs: %v\n", err);
    if (BURROW_FAILED(err))
        testing_t_error_v(t, err);
    arena_free(&ar);
}
// doc: end

// doc: values
/* Doubling and halving an int8 should give it back, which it does not once
 * the doubling overflows. */
BURROW_FUNC(HalfFn, bool, int8_t x);
#define HALF_SIG(IN, OUT) IN(0, int8_t) OUT(bool)
BURROW_FUNC_TYPE_DEFINE(HalfFn, HALF_SIG);

static bool double_then_half(void *env, int8_t x) {
    (void)env;
    int8_t d = (int8_t)(x * 2);
    return d / 2 == x;
}

/* The inputs, in order, instead of random ones. */
static int8_t inputs[] = {1, 50, 100, 120};
static Int next_input;

static void fixed_values(void *env, Slice values, MathRandRand *r) {
    (void)env;
    (void)r;
    BURROW_AT(ReflectValue, values, 0) =
        reflect_value_of(BURROW_ANY(TYPE_OF(int8_t), &inputs[next_input++]));
}

static void TestDoubleThenHalf(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    HalfFn f = {double_then_half, NULL};
    QuickConfig cfg = {4, 0, NULL, BURROW_FN(QuickValuesFunc, fixed_values, NULL)};
    Error err =
        quick_check(arena_allocator(&ar), BURROW_ANY(TYPE_OF(HalfFn), &f), &cfg);
    fmt_println_v(err);
    arena_free(&ar);
}
// doc: end

#define TESTS(X) X(TestAbs) X(TestDoubleThenHalf)
TESTING_MAIN(TESTS)

/* Output:
abs: <nil>
#3: failed on input 100
PASS
*/
