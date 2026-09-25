/* cmp, comparing ordered values.
 *
 * Go's cmp. Go writes these once as generic functions over its Ordered
 * constraint. C has no generics, so each is a function per type and a macro
 * that picks one by the type of its first argument:
 *
 *     int c = cmp_compare(a, b);
 *     if (cmp_less(x, 1.5)) ...
 *     Str name = cmp_or(flag_name, env_name, BURROW_S("guest"));
 *
 * The second argument is converted to the first one's type, the way an untyped
 * constant is in Go. The typed functions are there to call directly when the
 * type is known, or when a compiler has no _Generic.
 *
 * Floats follow Go: a NaN sorts before every other value and equals another
 * NaN, and -0.0 equals 0.0.
 *
 * Copyright 2023 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package cmp */

#ifndef BURROW_CMP_H
#define BURROW_CMP_H

#include "burrow/core.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Go's Ordered constraint, as a list: the C types that have an order, each
 * with the suffix its functions use. Pass it a macro X(T, suffix) to write
 * something out once per ordered type:
 *
 *     #define PRINT_NAME(T, suffix) puts(#suffix);
 *     CmpOrdered(PRINT_NAME)
 *
 * Int, Uint and Uintptr are not listed on their own because each is one of
 * the fixed width types. */
#define CmpOrdered(X)                                                                  \
    X(int8_t, int8)                                                                    \
    X(int16_t, int16)                                                                  \
    X(int32_t, int32)                                                                  \
    X(int64_t, int64)                                                                  \
    X(uint8_t, uint8)                                                                  \
    X(uint16_t, uint16)                                                                \
    X(uint32_t, uint32)                                                                \
    X(uint64_t, uint64)                                                                \
    X(float, float32)                                                                  \
    X(double, float64)                                                                 \
    X(Str, str)

/* -1 if x is less than y, 0 if they are equal and +1 if x is greater. */
int cmp_compare_int8(int8_t x, int8_t y);
int cmp_compare_int16(int16_t x, int16_t y);
int cmp_compare_int32(int32_t x, int32_t y);
int cmp_compare_int64(int64_t x, int64_t y);
int cmp_compare_uint8(uint8_t x, uint8_t y);
int cmp_compare_uint16(uint16_t x, uint16_t y);
int cmp_compare_uint32(uint32_t x, uint32_t y);
int cmp_compare_uint64(uint64_t x, uint64_t y);
int cmp_compare_float32(float x, float y);
int cmp_compare_float64(double x, double y);
int cmp_compare_str(Str x, Str y);

/* Whether x is less than y. */
bool cmp_less_int8(int8_t x, int8_t y);
bool cmp_less_int16(int16_t x, int16_t y);
bool cmp_less_int32(int32_t x, int32_t y);
bool cmp_less_int64(int64_t x, int64_t y);
bool cmp_less_uint8(uint8_t x, uint8_t y);
bool cmp_less_uint16(uint16_t x, uint16_t y);
bool cmp_less_uint32(uint32_t x, uint32_t y);
bool cmp_less_uint64(uint64_t x, uint64_t y);
bool cmp_less_float32(float x, float y);
bool cmp_less_float64(double x, double y);
bool cmp_less_str(Str x, Str y);

/* Go's Or two at a time: x if it is not the zero value, y otherwise. cmp_or
 * chains these so that it answers the first argument that is not zero. For a
 * float, -0.0 is zero and a NaN is not. */
int8_t cmp_or_int8(int8_t x, int8_t y);
int16_t cmp_or_int16(int16_t x, int16_t y);
int32_t cmp_or_int32(int32_t x, int32_t y);
int64_t cmp_or_int64(int64_t x, int64_t y);
uint8_t cmp_or_uint8(uint8_t x, uint8_t y);
uint16_t cmp_or_uint16(uint16_t x, uint16_t y);
uint32_t cmp_or_uint32(uint32_t x, uint32_t y);
uint64_t cmp_or_uint64(uint64_t x, uint64_t y);
float cmp_or_float32(float x, float y);
double cmp_or_float64(double x, double y);
BURROW_BORROWS(ret, x) BURROW_BORROWS(ret, y) Str cmp_or_str(Str x, Str y);
bool cmp_or_bool(bool x, bool y);

/* The function for the type of x. In steps, because a _Generic may not name
 * the same type twice, and long, long long and the fixed width types overlap
 * differently on every platform. A long or long long that is not one of the
 * fixed width types goes to the 64 bit function, which holds it whatever its
 * width, and so does anything else, so that a pointer or a struct is a
 * compile error about converting it to an integer. */
#define BURROW__CMP_FN3(op, x)                                                         \
    _Generic((x),                                                                      \
        long: cmp_##op##_int64,                                                        \
        unsigned long: cmp_##op##_uint64,                                              \
        long long: cmp_##op##_int64,                                                   \
        unsigned long long: cmp_##op##_uint64,                                         \
        default: cmp_##op##_int64)

#define BURROW__CMP_FN2(op, x)                                                         \
    _Generic((x),                                                                      \
        int8_t: cmp_##op##_int8,                                                       \
        int16_t: cmp_##op##_int16,                                                     \
        int32_t: cmp_##op##_int32,                                                     \
        int64_t: cmp_##op##_int64,                                                     \
        uint8_t: cmp_##op##_uint8,                                                     \
        uint16_t: cmp_##op##_uint16,                                                   \
        uint32_t: cmp_##op##_uint32,                                                   \
        uint64_t: cmp_##op##_uint64,                                                   \
        default: BURROW__CMP_FN3(op, x))

#define BURROW__CMP_FN(op, x)                                                          \
    _Generic((x),                                                                      \
        float: cmp_##op##_float32,                                                     \
        double: cmp_##op##_float64,                                                    \
        Str: cmp_##op##_str,                                                           \
        default: BURROW__CMP_FN2(op, x))

#define BURROW__CMP_OR_FN(x)                                                           \
    _Generic((x), bool: cmp_or_bool, default: BURROW__CMP_FN(or, x))

/* cmp.Compare and cmp.Less, for any ordered x. */
#define cmp_compare(x, y) (BURROW__CMP_FN(compare, x)((x), (y)))
#define cmp_less(x, y) (BURROW__CMP_FN(less, x)((x), (y)))

/* cmp.Or, for one to sixteen values of an ordered type or bool: the first
 * that is not the zero value, or the zero value if they all are. Every
 * argument is evaluated, as it is in Go. */
#define cmp_or(...)                                                                    \
    BURROW__CMP_CAT(BURROW__CMP_OR, BURROW__CMP_NARGS(__VA_ARGS__))(__VA_ARGS__)

#define BURROW__CMP_CAT(a, b) BURROW__CMP_CAT_(a, b)
#define BURROW__CMP_CAT_(a, b) a##b
#define BURROW__CMP_NARGS(...)                                                         \
    BURROW__CMP_NARGS_(__VA_ARGS__, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3,   \
                       2, 1, 0)
#define BURROW__CMP_NARGS_(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13,     \
                           _14, _15, _16, n, ...)                                      \
    n
#define BURROW__CMP_OR1(a) (a)
#define BURROW__CMP_OR2(a, b) (BURROW__CMP_OR_FN(a)((a), (b)))
#define BURROW__CMP_OR3(a, ...) BURROW__CMP_OR2(a, BURROW__CMP_OR2(__VA_ARGS__))
#define BURROW__CMP_OR4(a, ...) BURROW__CMP_OR2(a, BURROW__CMP_OR3(__VA_ARGS__))
#define BURROW__CMP_OR5(a, ...) BURROW__CMP_OR2(a, BURROW__CMP_OR4(__VA_ARGS__))
#define BURROW__CMP_OR6(a, ...) BURROW__CMP_OR2(a, BURROW__CMP_OR5(__VA_ARGS__))
#define BURROW__CMP_OR7(a, ...) BURROW__CMP_OR2(a, BURROW__CMP_OR6(__VA_ARGS__))
#define BURROW__CMP_OR8(a, ...) BURROW__CMP_OR2(a, BURROW__CMP_OR7(__VA_ARGS__))
#define BURROW__CMP_OR9(a, ...) BURROW__CMP_OR2(a, BURROW__CMP_OR8(__VA_ARGS__))
#define BURROW__CMP_OR10(a, ...) BURROW__CMP_OR2(a, BURROW__CMP_OR9(__VA_ARGS__))
#define BURROW__CMP_OR11(a, ...) BURROW__CMP_OR2(a, BURROW__CMP_OR10(__VA_ARGS__))
#define BURROW__CMP_OR12(a, ...) BURROW__CMP_OR2(a, BURROW__CMP_OR11(__VA_ARGS__))
#define BURROW__CMP_OR13(a, ...) BURROW__CMP_OR2(a, BURROW__CMP_OR12(__VA_ARGS__))
#define BURROW__CMP_OR14(a, ...) BURROW__CMP_OR2(a, BURROW__CMP_OR13(__VA_ARGS__))
#define BURROW__CMP_OR15(a, ...) BURROW__CMP_OR2(a, BURROW__CMP_OR14(__VA_ARGS__))
#define BURROW__CMP_OR16(a, ...) BURROW__CMP_OR2(a, BURROW__CMP_OR15(__VA_ARGS__))

#ifdef __cplusplus
}
#endif

#endif /* BURROW_CMP_H */
