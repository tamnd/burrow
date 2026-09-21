/* Arithmetic that answers what Go answers.
 *
 * Go and C agree about numbers right up until something goes wrong, and then
 * they part company completely. Go says a signed integer wraps, C says signed
 * overflow is undefined and lets the optimiser delete the code around it. Go
 * says a shift by 64 is zero, C says it is undefined. Go says dividing by zero
 * panics with a message, C says undefined again, and on x86 it is a hardware
 * fault with no message at all.
 *
 * None of that is theoretical. A ported hash function wraps on every round. A
 * ported parser shifts by a variable it got from the input. strconv, math/big,
 * hash/crc32, time and encoding/binary all depend on the wrapping answer, and
 * Go's own tests check it. So the choice is either to write the operations out
 * here once, or to write a subtly different bug into every package that needs
 * one.
 *
 * That is the whole rule for what lives in this header:
 *
 *     the operations where C is undefined, or where C is defined and disagrees
 *     with Go.
 *
 * Everything else stays as the operator you already know. Unsigned addition
 * wraps in C exactly as it does in Go, converting int64 to int8 truncates on
 * every compiler burrow supports, and adding two floats is IEEE 754 in both
 * languages, so write a + b and (int8_t)x for those. A function for them would
 * be noise with a cost in readability and nothing bought back.
 *
 * The names follow the type, the way str_at follows Str: int64_add, uint32_shl,
 * int_div. Int and Uint get their own set, since Go's int is its own type and
 * is 64 bits on a 64 bit platform and 32 on a 32 bit one.
 *
 * Nothing here allocates and nothing here calls out of line. Every one is a
 * static inline that folds: int64_add is a single add instruction on gcc and on
 * clang from -O1 up, and the wrapping is free because it was already what the
 * hardware did. The checks cost a compare and a perfectly predicted branch on
 * the division and shift paths, which is what the Go compiler emits there too.
 *
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_NUM_H
#define BURROW_NUM_H

#include "burrow/core.h"
#include "burrow/runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------- the families
 *
 * Ten integer types with the same nine operations each is ninety functions, and
 * writing them out by hand would be ninety chances to type 32 where 64 belongs.
 * So there are two macros, one for signed and one for unsigned, and every type
 * is one line further down. A bug in the macro is a bug in every width at once,
 * which the tests catch on the first run, and a bug in the eleventh hand
 * written copy is one that waits for the one caller who uses int16.
 *
 * The comments that would normally sit on each function are here instead,
 * because a comment inside a macro has to carry a backslash at the end of every
 * line and the result is unreadable and fights the formatter.
 *
 * What the signed macro generates, for a type called Name:
 *
 *     Name_add, Name_sub, Name_mul, Name_neg
 *         The operator, wrapping on overflow the way Go's does rather than
 *         being undefined the way C's is. The smallest value negates to itself.
 *
 *     Name_div, Name_mod
 *         Truncating division, which C99 and Go already agree about, plus the
 *         two cases they do not. A zero divisor stops the program, because the
 *         bare instruction faults on x86 and arrives as a signal with nothing
 *         in it. The smallest value divided by -1 does not fit in the type, so
 *         it wraps back to itself and the remainder is zero, which is what Go
 *         prints and what the Go compiler emits its own check to produce.
 *
 *     Name_shl, Name_shr
 *         Go shifts by any count and gives the answer the arithmetic implies,
 *         so a left shift past the width is zero because every bit has gone,
 *         and a right shift past the width leaves -1 for a negative value and 0
 *         for the rest. C calls all of that undefined. A negative count is a
 *         bug in the caller rather than a shift the other way, and Go stops for
 *         it, so this does too.
 *
 *     Name_from_float64
 *         Go's T(f), including the part Go leaves to the platform. See the note
 *         under the type list.
 *
 * The unsigned macro generates the same nine names. add, sub, mul and neg are
 * the plain operator there, since C already wraps unsigned arithmetic the way
 * Go does, and they exist so that the family is complete and so that the
 * generic macros at the bottom work on every type.
 *
 * The parameters:
 *
 *     Name   the prefix on the generated names, int32, uint8, int
 *     T      the C type
 *     U      the unsigned type of the same width, where the wrapping happens
 *     A      the type to do the arithmetic in, see below
 *     MINV   the smallest value of T
 *     MAXV   the largest value of T
 *     BITS   the width, which is what a shift count gets compared against
 *     FHI    the lowest double that does not fit in T, going up
 *     FLO    the highest double that does not fit in T, going down
 *
 * A is the one that is not obvious. C promotes anything narrower than int to
 * int before it does arithmetic, so (uint16_t)a * (uint16_t)b is a signed
 * multiply of two values that reach 65535, and that overflows a 32 bit int and
 * lands straight back in the undefined behaviour this header exists to avoid.
 * For the narrow types A is unsigned int, which is wide enough to hold every
 * product and does not promote to anything signed. For the wide ones the type
 * is already its own promotion, so A is U.
 *
 * burrow__Name_wrap is the internal one: an unsigned value back to signed,
 * without the implementation defined conversion. Every compiler in the world
 * gives the two's complement answer for that conversion and it still costs
 * nothing to write the defined version, since both compilers fold the whole
 * function into the add or the multiply that produced its argument. */

#define BURROW__NUM_SIGNED(Name, T, U, A, MINV, MAXV, BITS, FHI, FLO)                  \
    static inline T burrow__##Name##_wrap(U u) {                                       \
        return u <= (U)(MAXV) ? (T)u : (T)((T)(u - (U)(MAXV) - (U)1) + (MINV));        \
    }                                                                                  \
    static inline T Name##_add(T a, T b) {                                             \
        return burrow__##Name##_wrap((U)((A)a + (A)b));                                \
    }                                                                                  \
    static inline T Name##_sub(T a, T b) {                                             \
        return burrow__##Name##_wrap((U)((A)a - (A)b));                                \
    }                                                                                  \
    static inline T Name##_mul(T a, T b) {                                             \
        return burrow__##Name##_wrap((U)((A)a * (A)b));                                \
    }                                                                                  \
    static inline T Name##_neg(T a) {                                                  \
        return burrow__##Name##_wrap((U)((A)0 - (A)a));                                \
    }                                                                                  \
    static inline T Name##_div(T a, T b) {                                             \
        if (b == 0)                                                                    \
            runtime_integer_divide_by_zero();                                          \
        if (b == (T)(-1))                                                              \
            return Name##_neg(a);                                                      \
        return (T)(a / b);                                                             \
    }                                                                                  \
    static inline T Name##_mod(T a, T b) {                                             \
        if (b == 0)                                                                    \
            runtime_integer_divide_by_zero();                                          \
        if (b == (T)(-1))                                                              \
            return (T)0;                                                               \
        return (T)(a % b);                                                             \
    }                                                                                  \
    static inline T Name##_shl(T x, Int n) {                                           \
        if (n < 0)                                                                     \
            runtime_negative_shift();                                                  \
        if (n >= (Int)(BITS))                                                          \
            return (T)0;                                                               \
        return burrow__##Name##_wrap((U)((A)(U)x << (A)n));                            \
    }                                                                                  \
    static inline T Name##_shr(T x, Int n) {                                           \
        if (n < 0)                                                                     \
            runtime_negative_shift();                                                  \
        if (n >= (Int)(BITS))                                                          \
            return x < 0 ? (T)(-1) : (T)0;                                             \
        return (T)(x >> n);                                                            \
    }                                                                                  \
    static inline T Name##_from_float64(double f) {                                    \
        if (f != f)                                                                    \
            return (T)0;                                                               \
        if (f >= (FHI))                                                                \
            return (MAXV);                                                             \
        if (f <= (FLO))                                                                \
            return (MINV);                                                             \
        return (T)f;                                                                   \
    }                                                                                  \
    struct burrow__##Name##_num_end

/* The unsigned half. from_float64 is written as !(f > -1.0) rather than
 * f <= -1.0 so that a NaN takes that branch as well, since every comparison
 * against a NaN is false. A right shift has no sign bit to keep here, so
 * everything shifted out leaves zero. */
#define BURROW__NUM_UNSIGNED(Name, T, A, MAXV, BITS, FHI)                              \
    static inline T Name##_add(T a, T b) {                                             \
        return (T)((A)a + (A)b);                                                       \
    }                                                                                  \
    static inline T Name##_sub(T a, T b) {                                             \
        return (T)((A)a - (A)b);                                                       \
    }                                                                                  \
    static inline T Name##_mul(T a, T b) {                                             \
        return (T)((A)a * (A)b);                                                       \
    }                                                                                  \
    static inline T Name##_neg(T a) {                                                  \
        return (T)((A)0 - (A)a);                                                       \
    }                                                                                  \
    static inline T Name##_div(T a, T b) {                                             \
        if (b == 0)                                                                    \
            runtime_integer_divide_by_zero();                                          \
        return (T)(a / b);                                                             \
    }                                                                                  \
    static inline T Name##_mod(T a, T b) {                                             \
        if (b == 0)                                                                    \
            runtime_integer_divide_by_zero();                                          \
        return (T)(a % b);                                                             \
    }                                                                                  \
    static inline T Name##_shl(T x, Int n) {                                           \
        if (n < 0)                                                                     \
            runtime_negative_shift();                                                  \
        if (n >= (Int)(BITS))                                                          \
            return (T)0;                                                               \
        return (T)((A)x << (A)n);                                                      \
    }                                                                                  \
    static inline T Name##_shr(T x, Int n) {                                           \
        if (n < 0)                                                                     \
            runtime_negative_shift();                                                  \
        if (n >= (Int)(BITS))                                                          \
            return (T)0;                                                               \
        return (T)((A)x >> (A)n);                                                      \
    }                                                                                  \
    static inline T Name##_from_float64(double f) {                                    \
        if (!(f > -1.0))                                                               \
            return (T)0;                                                               \
        if (f >= (FHI))                                                                \
            return (MAXV);                                                             \
        return (T)f;                                                                   \
    }                                                                                  \
    struct burrow__##Name##_num_end

/* ----------------------------------------------------------------- the types
 *
 * The float bounds are the first value that does not fit rather than the last
 * one that does, because the conversion truncates towards zero before it goes
 * out of range. 127.9 is a perfectly good int8 and 128.0 is not, so the bound
 * going up is 128.0 and the bound going down is -129.0.
 *
 * For int64 the bound going down is written as -9223372036854775808.0, which is
 * the smallest int64 itself rather than one below it. There is no double
 * between that value and the next one down, and a value exactly on the bound
 * converts to the same answer either way, so the simpler constant is also the
 * correct one. */

BURROW__NUM_SIGNED(int8, int8_t, uint8_t, unsigned int, INT8_MIN, INT8_MAX, 8, 128.0,
                   -129.0);
BURROW__NUM_SIGNED(int16, int16_t, uint16_t, unsigned int, INT16_MIN, INT16_MAX, 16,
                   32768.0, -32769.0);
BURROW__NUM_SIGNED(int32, int32_t, uint32_t, uint32_t, INT32_MIN, INT32_MAX, 32,
                   2147483648.0, -2147483649.0);
BURROW__NUM_SIGNED(int64, int64_t, uint64_t, uint64_t, INT64_MIN, INT64_MAX, 64,
                   9223372036854775808.0, -9223372036854775808.0);

BURROW__NUM_UNSIGNED(uint8, uint8_t, unsigned int, UINT8_MAX, 8, 256.0);
BURROW__NUM_UNSIGNED(uint16, uint16_t, unsigned int, UINT16_MAX, 16, 65536.0);
BURROW__NUM_UNSIGNED(uint32, uint32_t, uint32_t, UINT32_MAX, 32, 4294967296.0);
BURROW__NUM_UNSIGNED(uint64, uint64_t, uint64_t, UINT64_MAX, 64,
                     18446744073709551616.0);

/* Go's int and uint. They are the same width as one of the pairs above and they
 * are still their own type, the same way they are in Go, so int_add is a real
 * function rather than a spelling of int64_add that stops working on a 32 bit
 * build. The compiler inlines both to the same instruction. */
#if BURROW_PTR_BITS == 64
BURROW__NUM_SIGNED(int, Int, Uint, Uint, BURROW_INT_MIN, BURROW_INT_MAX, 64,
                   9223372036854775808.0, -9223372036854775808.0);
BURROW__NUM_UNSIGNED(uint, Uint, Uint, UINT64_MAX, 64, 18446744073709551616.0);
#else
BURROW__NUM_SIGNED(int, Int, Uint, Uint, BURROW_INT_MIN, BURROW_INT_MAX, 32,
                   2147483648.0, -2147483649.0);
BURROW__NUM_UNSIGNED(uint, Uint, Uint, UINT32_MAX, 32, 4294967296.0);
#endif

/* ------------------------------------------------------------- float to int
 *
 * Go's specification says the result is implementation dependent when the value
 * does not fit, and Go means it. The same program gives -9223372036854775808
 * for int64(1e300) on amd64 and 9223372036854775807 on arm64, and both are
 * correct Go.
 *
 * A library cannot hand that on. Code ported to burrow has to give one answer,
 * so the functions above pick one:
 *
 *     too big, either direction, gives the nearest value that fits
 *     NaN gives zero
 *     anything that fits truncates towards zero, which is what Go does
 *
 * That is arm64's answer, and wasm's, and Rust's, and it is the one that keeps
 * a checksum or an index inside its own type instead of flipping its sign. The
 * cost is two compares on a path that already has a conversion instruction on
 * it. Code that needs to know it was out of range should check before it calls,
 * because the result cannot tell you: int64_from_float64(1e300) and
 * int64_from_float64(9223372036854775807.0) both give the same number, which is
 * exactly the ambiguity Go has as well.
 *
 * There is no _from_float32 set. A float widens to a double without losing
 * anything, so int64_from_float64(f) is the right answer for a float too, and a
 * second set of ten functions would only be a place for the two to disagree. */

/* -------------------------------------------------------------- generic forms
 *
 * One spelling that works for any width, for the places where a macro or a
 * ported generic function does not know which type it has:
 *
 *     Int total = BURROW_ADD(subtotal, tax);
 *     uint32_t h = BURROW_SHL(hash, 5);
 *
 * They select a function by the type of the first argument and then call it, so
 * the second argument has to be the same type. That is not an accident of the
 * implementation, it is Go's rule: Go has no mixed type arithmetic either, and
 * a warning here is the conversion Go would have refused to compile.
 *
 * The argument has to be exactly one of the eight fixed width types. Int and
 * Uint arrive as whichever of those they are, so they work without an entry of
 * their own. A plain char is not int8_t, and a long is not int64_t on a
 * platform where int64_t is a long long, so those get a compile error naming
 * the type rather than a silent widening.
 *
 * Nothing here is in the BURROW_SHORT set. ADD and SHL are names other people's
 * headers already use, and a macro called ADD arriving from a library you
 * included is a bad afternoon. */

#define BURROW__NUM_FN(op, x)                                                          \
    _Generic((x),                                                                      \
        int8_t: int8_##op,                                                             \
        int16_t: int16_##op,                                                           \
        int32_t: int32_##op,                                                           \
        int64_t: int64_##op,                                                           \
        uint8_t: uint8_##op,                                                           \
        uint16_t: uint16_##op,                                                         \
        uint32_t: uint32_##op,                                                         \
        uint64_t: uint64_##op)

#define BURROW_ADD(a, b) (BURROW__NUM_FN(add, a)((a), (b)))
#define BURROW_SUB(a, b) (BURROW__NUM_FN(sub, a)((a), (b)))
#define BURROW_MUL(a, b) (BURROW__NUM_FN(mul, a)((a), (b)))
#define BURROW_NEG(a) (BURROW__NUM_FN(neg, a)((a)))
#define BURROW_DIV(a, b) (BURROW__NUM_FN(div, a)((a), (b)))
#define BURROW_MOD(a, b) (BURROW__NUM_FN(mod, a)((a), (b)))
#define BURROW_SHL(x, n) (BURROW__NUM_FN(shl, x)((x), (n)))
#define BURROW_SHR(x, n) (BURROW__NUM_FN(shr, x)((x), (n)))

#ifdef __cplusplus
}
#endif

#endif /* BURROW_NUM_H */
