/* Every answer in here was taken from a Go program, not from a C standard.
 *
 * The file that produced them ran on darwin/arm64 with Go 1.27.1 and on
 * linux/amd64 with Go 1.26.5, and where the two disagreed the disagreement is
 * written into the test as a comment rather than smoothed over. There is
 * exactly one place they disagree and it is float to int conversion out of
 * range, which Go's specification calls implementation dependent.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "fatal.h"
#include "harness.h"

#include "burrow/num.h"

/* A zero and an infinity the compiler is not allowed to fold, since 0.0 / 0.0
 * written out is a diagnostic on some compilers and a constant on the rest. */
static volatile double zero_v = 0.0;
static volatile double big_v = 1e300;

static double nan_value(void) {
    return zero_v / zero_v;
}

/* ------------------------------------------------------------------ wrapping */

TEST(signed_overflow_wraps_the_way_go_says) {
    /* The one that matters. C calls this undefined and deletes the code that
     * depends on it, Go calls it the answer, and hash/fnv wraps on every byte
     * it hashes. */
    CHECK(int64_add(INT64_MAX, 1) == INT64_MIN);
    CHECK(int64_sub(INT64_MIN, 1) == INT64_MAX);
    CHECK(int64_mul(INT64_MAX, 2) == -2);
    CHECK(int64_neg(INT64_MIN) == INT64_MIN);

    /* The ordinary cases still have to be ordinary. */
    CHECK_INT_EQ(int64_add(2, 3), 5);
    CHECK_INT_EQ(int64_sub(2, 3), -1);
    CHECK_INT_EQ(int64_mul(-4, 3), -12);
    CHECK_INT_EQ(int64_neg(7), -7);
}

TEST(the_narrow_widths_wrap_at_their_own_width) {
    /* This is what the promotion rules get wrong if you write a + b and cast
     * the result. int8 arithmetic in C happens in int, so the overflow has to
     * be put back by hand. */
    CHECK_INT_EQ(int8_add(INT8_MAX, 1), INT8_MIN);
    CHECK_INT_EQ(int8_sub(INT8_MIN, 1), INT8_MAX);
    CHECK_INT_EQ(int8_mul(100, 100), 16); /* 10000 mod 256 is 16 */
    CHECK_INT_EQ(int8_neg(INT8_MIN), INT8_MIN);

    CHECK_INT_EQ(int16_add(INT16_MAX, 1), INT16_MIN);
    CHECK_INT_EQ(int16_mul(1000, 100), -31072); /* 100000 mod 65536, signed */
    CHECK_INT_EQ(int32_add(INT32_MAX, 1), INT32_MIN);
    CHECK_INT_EQ(int32_mul(65536, 65536), 0);

    /* The unsigned ones wrap in C already. They are here so that the family is
     * complete and so that a generic caller gets the same answer. */
    CHECK_INT_EQ(uint8_add(255, 1), 0);
    CHECK_INT_EQ(uint8_sub(0, 1), 255);
    CHECK_INT_EQ(uint8_mul(16, 16), 0);
    CHECK_INT_EQ(uint8_neg(1), 255);
    CHECK_INT_EQ(uint16_mul(300, 300), 24464);
    CHECK(uint64_add(UINT64_MAX, 1) == 0);
    CHECK(uint64_neg(1) == UINT64_MAX);
}

TEST(int_is_its_own_type_and_follows_the_platform) {
    CHECK(int_add(BURROW_INT_MAX, 1) == BURROW_INT_MIN);
    CHECK(int_sub(BURROW_INT_MIN, 1) == BURROW_INT_MAX);
    CHECK(int_neg(BURROW_INT_MIN) == BURROW_INT_MIN);
    CHECK_INT_EQ(int_mul(6, 7), 42);

    CHECK(uint_add((Uint)BURROW_INT_MAX, (Uint)BURROW_INT_MAX) == (Uint)-2);
    CHECK_INT_EQ(uint_div(100, 7), 14);

    /* Int is 64 bits on a 64 bit platform and 32 on a 32 bit one, which is the
     * whole reason it is not int64_t. */
#if BURROW_PTR_BITS == 64
    CHECK_INT_EQ(sizeof(Int), 8);
#else
    CHECK_INT_EQ(sizeof(Int), 4);
#endif
}

/* ------------------------------------------------------------------ division */

TEST(division_truncates_towards_zero_like_go) {
    /* C99 and Go already agree here. The test is here because an earlier C
     * rounded towards minus infinity and somebody reading this will want to
     * know which one they are getting. */
    CHECK_INT_EQ(int64_div(7, 2), 3);
    CHECK_INT_EQ(int64_div(-7, 2), -3);
    CHECK_INT_EQ(int64_div(7, -2), -3);
    CHECK_INT_EQ(int64_div(-7, -2), 3);

    /* The remainder takes the sign of the dividend. */
    CHECK_INT_EQ(int64_mod(7, 2), 1);
    CHECK_INT_EQ(int64_mod(-7, 2), -1);
    CHECK_INT_EQ(int64_mod(7, -2), 1);
    CHECK_INT_EQ(int64_mod(-7, -2), -1);

    CHECK_INT_EQ(uint64_div(7, 2), 3);
    CHECK_INT_EQ(uint64_mod(7, 2), 1);
    CHECK_INT_EQ(int8_div(-100, 3), -33);
    CHECK_INT_EQ(int8_mod(-100, 3), -1);
}

TEST(the_smallest_value_divided_by_minus_one) {
    /* The answer does not fit in the type, so it wraps, which is what Go
     * prints. The x86 division instruction faults on this one, so the check is
     * not optional even on the platform where the divisor test looks free. */
    CHECK(int64_div(INT64_MIN, -1) == INT64_MIN);
    CHECK(int64_mod(INT64_MIN, -1) == 0);
    CHECK_INT_EQ(int8_div(INT8_MIN, -1), INT8_MIN);
    CHECK_INT_EQ(int8_mod(INT8_MIN, -1), 0);
    CHECK(int_div(BURROW_INT_MIN, -1) == BURROW_INT_MIN);

    /* Minus one is not otherwise special. */
    CHECK_INT_EQ(int64_div(42, -1), -42);
    CHECK_INT_EQ(int64_mod(42, -1), 0);
}

TEST(dividing_by_zero_stops_with_gos_message) {
    CHECK_FATAL(int64_div(1, 0), "runtime error: integer divide by zero");
    CHECK_FATAL(int64_mod(1, 0), "runtime error: integer divide by zero");
    CHECK_FATAL(uint64_div(1, 0), "runtime error: integer divide by zero");
    CHECK_FATAL(uint8_mod(1, 0), "runtime error: integer divide by zero");
    CHECK_FATAL(int_div(1, 0), "runtime error: integer divide by zero");

    /* Zero divided by zero is the same failure, which is worth stating because
     * the float version of it is not. */
    CHECK_FATAL(int64_div(0, 0), "runtime error: integer divide by zero");
}

/* -------------------------------------------------------------------- shifts */

TEST(shifting_past_the_width_is_defined) {
    /* Go gives the answer the arithmetic implies once every bit has gone. C
     * calls the same expression undefined, and on x86 the hardware quietly uses
     * the low six bits of the count, so 1 << 64 comes out as 1. */
    CHECK(int64_shl(1, 64) == 0);
    CHECK(int64_shl(1, 100) == 0);
    CHECK(uint64_shl(UINT64_MAX, 64) == 0);
    CHECK_INT_EQ(int8_shl(1, 8), 0);

    /* A signed right shift keeps the sign bit, so a negative value never
     * becomes zero no matter how far it goes. */
    CHECK(int64_shr(-1, 100) == -1);
    CHECK(int64_shr(-1, 63) == -1);
    CHECK(int64_shr(INT64_MIN, 100) == -1);
    CHECK(int64_shr(1, 100) == 0);
    CHECK_INT_EQ(int8_shr(-1, 8), -1);
    CHECK_INT_EQ(int8_shr(127, 8), 0);

    /* Unsigned has no sign bit to keep. */
    CHECK(uint64_shr(UINT64_MAX, 64) == 0);
    CHECK_INT_EQ(uint8_shr(255, 8), 0);
}

TEST(shifting_within_the_width_is_the_operator) {
    CHECK(int64_shl(1, 63) == INT64_MIN);
    CHECK_INT_EQ(int64_shl(1, 10), 1024);
    CHECK_INT_EQ(int64_shr(1024, 10), 1);
    CHECK_INT_EQ(int64_shr(-1024, 10), -1);
    CHECK_INT_EQ(int64_shl(0, 0), 0);
    CHECK_INT_EQ(int64_shr(-7, 1), -4); /* rounds towards minus infinity */

    CHECK_INT_EQ(int8_shl(1, 7), INT8_MIN);
    CHECK_INT_EQ(int8_shl(-1, 1), -2);
    CHECK_INT_EQ(uint8_shl(1, 7), 128);
    CHECK_INT_EQ(uint16_shl(1, 15), 32768);
    CHECK(uint32_shl(1, 31) == 2147483648u);
    CHECK(uint64_shr(UINT64_MAX, 32) == 4294967295u);
    CHECK(int_shl(1, 4) == 16);
    CHECK(uint_shr(256, 4) == 16);
}

TEST(a_negative_shift_count_stops) {
    /* Go's count is a count and not a direction, so a negative one is a bug in
     * the caller. The message carries no number because Go's does not. */
    CHECK_FATAL(int64_shl(1, -1), "runtime error: negative shift amount");
    CHECK_FATAL(int64_shr(1, -1), "runtime error: negative shift amount");
    CHECK_FATAL(uint64_shl(1, -64), "runtime error: negative shift amount");
    CHECK_FATAL(uint8_shr(1, -1), "runtime error: negative shift amount");
    CHECK_FATAL(int_shl(1, -1), "runtime error: negative shift amount");
}

/* ------------------------------------------------------------- float to int */

TEST(a_float_that_fits_truncates_towards_zero) {
    CHECK_INT_EQ(int64_from_float64(2.7), 2);
    CHECK_INT_EQ(int64_from_float64(-2.7), -2);
    CHECK_INT_EQ(int64_from_float64(0.9), 0);
    CHECK_INT_EQ(int64_from_float64(-0.9), 0);
    CHECK_INT_EQ(uint64_from_float64(2.7), 2);

    /* Truncation happens before the range test, so the largest value that fits
     * is the one whose whole part fits rather than the one below the bound. */
    CHECK_INT_EQ(int8_from_float64(127.9), 127);
    CHECK_INT_EQ(int8_from_float64(-128.9), -128);
    CHECK_INT_EQ(uint8_from_float64(255.9), 255);
    CHECK_INT_EQ(uint8_from_float64(-0.9), 0);
}

TEST(a_float_that_does_not_fit_saturates) {
    /* Go leaves this to the platform and the platforms differ: the same
     * program prints -9223372036854775808 on amd64 and 9223372036854775807 on
     * arm64. burrow picks the nearest value that fits, everywhere, which is
     * arm64's answer and wasm's and Rust's.
     *
     * The value cannot tell you it was out of range. Check before you call if
     * you need to know. */
    CHECK(int64_from_float64(big_v) == INT64_MAX);
    CHECK(int64_from_float64(-big_v) == INT64_MIN);
    CHECK(uint64_from_float64(big_v) == UINT64_MAX);
    CHECK(uint64_from_float64(-big_v) == 0);

    CHECK_INT_EQ(int8_from_float64(1000.0), INT8_MAX);
    CHECK_INT_EQ(int8_from_float64(-1000.0), INT8_MIN);
    CHECK_INT_EQ(int16_from_float64(1e9), INT16_MAX);
    CHECK_INT_EQ(int32_from_float64(1e18), INT32_MAX);
    CHECK_INT_EQ(int32_from_float64(-1e18), INT32_MIN);
    CHECK_INT_EQ(uint8_from_float64(1000.0), UINT8_MAX);
    CHECK(uint32_from_float64(1e18) == UINT32_MAX);

    /* Exactly on the bound. 2 to the 63 is one past the largest int64 and is
     * the first double up there, so it saturates. */
    CHECK(int64_from_float64(9223372036854775808.0) == INT64_MAX);
    CHECK(int64_from_float64(-9223372036854775808.0) == INT64_MIN);
    CHECK_INT_EQ(int8_from_float64(128.0), INT8_MAX);
    CHECK_INT_EQ(int8_from_float64(-129.0), INT8_MIN);
}

TEST(nan_converts_to_zero) {
    /* The other half of what Go leaves to the platform. amd64 gives the
     * smallest int64 and arm64 gives zero. Zero is the one that does not look
     * like a real number further down the program. */
    double nan = nan_value();

    CHECK(nan != nan);
    CHECK(int64_from_float64(nan) == 0);
    CHECK(uint64_from_float64(nan) == 0);
    CHECK_INT_EQ(int8_from_float64(nan), 0);
    CHECK_INT_EQ(uint8_from_float64(nan), 0);
    CHECK(int_from_float64(nan) == 0);
}

TEST(a_float_widens_to_a_double_without_losing_anything) {
    /* Which is why there is no _from_float32 set to keep in step with this
     * one. */
    float f = 2.7f;

    CHECK_INT_EQ(int64_from_float64((double)f), 2);
    CHECK_INT_EQ(int64_from_float64((double)-f), -2);
}

/* ------------------------------------------------------------ generic forms */

TEST(the_generic_macros_pick_by_the_type_of_the_first_argument) {
    int8_t a8 = INT8_MAX;
    int32_t a32 = INT32_MAX;
    uint16_t u16 = 65535;
    int64_t a64 = INT64_MAX;
    Int ai = 10;

    CHECK_INT_EQ(BURROW_ADD(a8, (int8_t)1), INT8_MIN);
    CHECK_INT_EQ(BURROW_ADD(a32, 1), INT32_MIN);
    CHECK_INT_EQ(BURROW_ADD(u16, (uint16_t)1), 0);
    CHECK(BURROW_ADD(a64, 1) == INT64_MIN);

    CHECK_INT_EQ(BURROW_SUB(a32, 1), INT32_MAX - 1);
    CHECK_INT_EQ(BURROW_MUL(a8, (int8_t)2), -2);
    CHECK_INT_EQ(BURROW_NEG(a32), -INT32_MAX);
    CHECK_INT_EQ(BURROW_DIV(a32, 2), INT32_MAX / 2);
    CHECK_INT_EQ(BURROW_MOD(a32, 2), 1);
    CHECK_INT_EQ(BURROW_SHL(u16, 15), 32768);
    CHECK_INT_EQ(BURROW_SHR(u16, 15), 1);

    /* Int arrives as whichever fixed width type it is, so the generic form
     * works on it without an entry of its own. */
    CHECK(BURROW_MUL(ai, (Int)10) == 100);

    /* The failure modes are still the real ones. */
    CHECK_FATAL(BURROW_DIV(a32, 0), "runtime error: integer divide by zero");
    CHECK_FATAL(BURROW_SHL(a32, -1), "runtime error: negative shift amount");
}

/* ----------------------------------------------------------------- the shape
 *
 * Not a test of an answer, a test that the things this header promises about
 * itself are true. */

TEST(nothing_here_reads_or_writes_anything) {
    /* Every operation is a pure function of its arguments, so a call with the
     * same arguments in a loop gives the same value and none of them can be
     * holding state between calls. A compiler that folded one of these away
     * would pass this test as well, which is fine: the point is that folding it
     * away is allowed. */
    Int i;
    Int total = 0;

    for (i = 0; i < 1000; i++)
        total = int_add(total, int_mod(int_mul(i, 3), 7));

    CHECK_INT_EQ(total, 2999);
}

int main(void) {
    RUN(signed_overflow_wraps_the_way_go_says);
    RUN(the_narrow_widths_wrap_at_their_own_width);
    RUN(int_is_its_own_type_and_follows_the_platform);
    RUN(division_truncates_towards_zero_like_go);
    RUN(the_smallest_value_divided_by_minus_one);
    RUN(dividing_by_zero_stops_with_gos_message);
    RUN(shifting_past_the_width_is_defined);
    RUN(shifting_within_the_width_is_the_operator);
    RUN(a_negative_shift_count_stops);
    RUN(a_float_that_fits_truncates_towards_zero);
    RUN(a_float_that_does_not_fit_saturates);
    RUN(nan_converts_to_zero);
    RUN(a_float_widens_to_a_double_without_losing_anything);
    RUN(the_generic_macros_pick_by_the_type_of_the_first_argument);
    RUN(nothing_here_reads_or_writes_anything);
    return harness_report("num");
}
