/* math/bits, counting and arithmetic on the bits of unsigned integers.
 *
 * Most of these are one instruction on the machine you are using, and the Go
 * compiler knows that and emits the instruction. This header does the same by
 * way of the compiler builtins where there is one, and Go's own portable code
 * where there is not, so the answer is the same either way and only the speed
 * changes. Everything is static inline for that reason: a call to
 * bits_len64 out of line would cost more than the instruction it wraps.
 *
 * The results follow the usual rule: the first comes back, and the second is
 * an out parameter at the end that may be NULL. So Go's
 *
 *     hi, lo := bits.Mul64(x, y)
 *     sum, carry := bits.Add64(x, y, 0)
 *
 * is
 *
 *     uint64_t lo;
 *     uint64_t hi = bits_mul64(x, y, &lo);
 *     uint64_t carry;
 *     uint64_t sum = bits_add64(x, y, 0, &carry);
 *
 * Div and Rem panic where Go's do, with Go's messages: a zero divisor is
 * "integer divide by zero", and a quotient that does not fit is "integer
 * overflow".
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_MATH_BITS_H
#define BURROW_MATH_BITS_H

#include "burrow/core.h"
#include "burrow/runtime.h"

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* The size of a Uint in bits, 32 or 64. */
#define BITS_UINT_SIZE BURROW_PTR_BITS

/* The builtins are used when the compiler has them. Defining
 * BURROW__BITS_PORTABLE turns them off, and the tests build a second time with
 * it defined so that the portable code, which is what an unknown compiler
 * gets, is checked on every run and not only on the day somebody brings one. */
#if !defined(BURROW__BITS_PORTABLE) && (defined(__GNUC__) || defined(__clang__))
#define BURROW__BITS_BUILTINS 1
#else
#define BURROW__BITS_BUILTINS 0
#endif

#if !defined(BURROW__BITS_PORTABLE) && defined(__SIZEOF_INT128__)
#define BURROW__BITS_INT128 1
#else
#define BURROW__BITS_INT128 0
#endif

/* ------------------------------------------------------------------ Len
 *
 * The number of bits needed to write x down, which is 0 for 0. Everything to
 * do with leading zeros is written in terms of these. */

static inline Int bits_len64(uint64_t x) {
#if BURROW__BITS_BUILTINS
    return x == 0 ? 0 : 64 - __builtin_clzll(x);
#else
    static const uint8_t len8tab[256] = {
        0, 1, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
        5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    };
    Int n = 0;
    if (x >= (uint64_t)1 << 32) {
        x >>= 32;
        n = 32;
    }
    if (x >= (uint64_t)1 << 16) {
        x >>= 16;
        n += 16;
    }
    if (x >= (uint64_t)1 << 8) {
        x >>= 8;
        n += 8;
    }
    return n + (Int)len8tab[x];
#endif
}

static inline Int bits_len32(uint32_t x) {
    return bits_len64(x);
}

static inline Int bits_len16(uint16_t x) {
    return bits_len64(x);
}

static inline Int bits_len8(uint8_t x) {
    return bits_len64(x);
}

static inline Int bits_len(Uint x) {
    return bits_len64(x);
}

/* --------------------------------------------------------- LeadingZeros
 *
 * The number of zero bits above the highest one, so the full width for 0. */

static inline Int bits_leading_zeros64(uint64_t x) {
    return 64 - bits_len64(x);
}

static inline Int bits_leading_zeros32(uint32_t x) {
    return 32 - bits_len32(x);
}

static inline Int bits_leading_zeros16(uint16_t x) {
    return 16 - bits_len16(x);
}

static inline Int bits_leading_zeros8(uint8_t x) {
    return 8 - bits_len8(x);
}

static inline Int bits_leading_zeros(Uint x) {
    return BITS_UINT_SIZE - bits_len(x);
}

/* -------------------------------------------------------- TrailingZeros
 *
 * The number of zero bits below the lowest one, so the full width for 0. The
 * portable version is Go's: isolate the lowest set bit and look it up with a
 * de Bruijn multiply. */

static inline Int bits_trailing_zeros64(uint64_t x) {
    if (x == 0)
        return 64;
#if BURROW__BITS_BUILTINS
    return __builtin_ctzll(x);
#else
    static const uint8_t debruijn64tab[64] = {
        0,  1,  56, 2,  57, 49, 28, 3,  61, 58, 42, 50, 38, 29, 17, 4,
        62, 47, 59, 36, 45, 43, 51, 22, 53, 39, 33, 30, 24, 18, 12, 5,
        63, 55, 48, 27, 60, 41, 37, 16, 46, 35, 44, 21, 52, 32, 23, 11,
        54, 26, 40, 15, 34, 20, 31, 10, 25, 14, 19, 9,  13, 8,  7,  6,
    };
    return debruijn64tab[((x & (0 - x)) * 0x03f79d71b4ca8b09ull) >> (64 - 6)];
#endif
}

static inline Int bits_trailing_zeros32(uint32_t x) {
    return x == 0 ? 32 : bits_trailing_zeros64(x);
}

static inline Int bits_trailing_zeros16(uint16_t x) {
    return x == 0 ? 16 : bits_trailing_zeros64(x);
}

static inline Int bits_trailing_zeros8(uint8_t x) {
    return x == 0 ? 8 : bits_trailing_zeros64(x);
}

static inline Int bits_trailing_zeros(Uint x) {
    return x == 0 ? BITS_UINT_SIZE : bits_trailing_zeros64(x);
}

/* ------------------------------------------------------------ OnesCount
 *
 * The number of one bits, the population count. */

static inline Int bits_ones_count64(uint64_t x) {
#if BURROW__BITS_BUILTINS
    return __builtin_popcountll(x);
#else
    /* Go's: add adjacent pairs, then nibbles, then bytes, then sum the bytes.
     * Each step can only grow a field by one bit, so no field overflows into
     * the next and the masks can be applied late. */
    const uint64_t m0 = 0x5555555555555555ull;
    const uint64_t m1 = 0x3333333333333333ull;
    const uint64_t m2 = 0x0f0f0f0f0f0f0f0full;
    x = ((x >> 1) & m0) + (x & m0);
    x = ((x >> 2) & m1) + (x & m1);
    x = ((x >> 4) + x) & m2;
    x += x >> 8;
    x += x >> 16;
    x += x >> 32;
    return (Int)(x & 127);
#endif
}

static inline Int bits_ones_count32(uint32_t x) {
    return bits_ones_count64(x);
}

static inline Int bits_ones_count16(uint16_t x) {
    return bits_ones_count64(x);
}

static inline Int bits_ones_count8(uint8_t x) {
    return bits_ones_count64(x);
}

static inline Int bits_ones_count(Uint x) {
    return bits_ones_count64(x);
}

/* ----------------------------------------------------------- RotateLeft
 *
 * x rotated left by k bits, or right by -k when k is negative. Any k works,
 * since only k modulo the width matters. The shift the other way is masked as
 * well, because a rotate by 0 would otherwise shift by the full width, which C
 * leaves undefined and Go does not. */

static inline uint64_t bits_rotate_left64(uint64_t x, Int k) {
    unsigned s = (unsigned)k & 63;
    return x << s | x >> ((64 - s) & 63);
}

static inline uint32_t bits_rotate_left32(uint32_t x, Int k) {
    unsigned s = (unsigned)k & 31;
    return x << s | x >> ((32 - s) & 31);
}

static inline uint16_t bits_rotate_left16(uint16_t x, Int k) {
    unsigned s = (unsigned)k & 15;
    return (uint16_t)(x << s | x >> ((16 - s) & 15));
}

static inline uint8_t bits_rotate_left8(uint8_t x, Int k) {
    unsigned s = (unsigned)k & 7;
    return (uint8_t)(x << s | x >> ((8 - s) & 7));
}

static inline Uint bits_rotate_left(Uint x, Int k) {
#if BURROW_PTR_BITS == 64
    return bits_rotate_left64(x, k);
#else
    return bits_rotate_left32(x, k);
#endif
}

/* -------------------------------------------------------- ReverseBytes */

static inline uint16_t bits_reverse_bytes16(uint16_t x) {
    return (uint16_t)(x >> 8 | x << 8);
}

static inline uint32_t bits_reverse_bytes32(uint32_t x) {
#if BURROW__BITS_BUILTINS
    return __builtin_bswap32(x);
#else
    const uint32_t m = 0x00ff00ffu;
    x = ((x >> 8) & m) | ((x & m) << 8);
    return x >> 16 | x << 16;
#endif
}

static inline uint64_t bits_reverse_bytes64(uint64_t x) {
#if BURROW__BITS_BUILTINS
    return __builtin_bswap64(x);
#else
    const uint64_t m3 = 0x00ff00ff00ff00ffull;
    const uint64_t m4 = 0x0000ffff0000ffffull;
    x = ((x >> 8) & m3) | ((x & m3) << 8);
    x = ((x >> 16) & m4) | ((x & m4) << 16);
    return x >> 32 | x << 32;
#endif
}

static inline Uint bits_reverse_bytes(Uint x) {
#if BURROW_PTR_BITS == 64
    return bits_reverse_bytes64(x);
#else
    return bits_reverse_bytes32(x);
#endif
}

/* -------------------------------------------------------------- Reverse
 *
 * The bits in the opposite order. Swap adjacent bits, then pairs, then
 * nibbles, and the bytes are left in the right order within themselves, so
 * reversing the bytes finishes the job. */

static inline uint64_t bits_reverse64(uint64_t x) {
    const uint64_t m0 = 0x5555555555555555ull;
    const uint64_t m1 = 0x3333333333333333ull;
    const uint64_t m2 = 0x0f0f0f0f0f0f0f0full;
    x = ((x >> 1) & m0) | ((x & m0) << 1);
    x = ((x >> 2) & m1) | ((x & m1) << 2);
    x = ((x >> 4) & m2) | ((x & m2) << 4);
    return bits_reverse_bytes64(x);
}

static inline uint32_t bits_reverse32(uint32_t x) {
    return (uint32_t)(bits_reverse64(x) >> 32);
}

static inline uint16_t bits_reverse16(uint16_t x) {
    return (uint16_t)(bits_reverse64(x) >> 48);
}

static inline uint8_t bits_reverse8(uint8_t x) {
    return (uint8_t)(bits_reverse64(x) >> 56);
}

static inline Uint bits_reverse(Uint x) {
#if BURROW_PTR_BITS == 64
    return bits_reverse64(x);
#else
    return bits_reverse32(x);
#endif
}

/* ------------------------------------------------------------- Add, Sub
 *
 * The sum or difference with a carry or borrow in and out. The carry in must
 * be 0 or 1 and the carry out always is. The formulas are Go's, which compile
 * to an add with carry on every compiler worth the name without a builtin. */

static inline uint64_t bits_add64(uint64_t x, uint64_t y, uint64_t carry,
                                  uint64_t *carry_out) {
    uint64_t sum = x + y + carry;
    if (carry_out)
        *carry_out = ((x & y) | ((x | y) & ~sum)) >> 63;
    return sum;
}

static inline uint32_t bits_add32(uint32_t x, uint32_t y, uint32_t carry,
                                  uint32_t *carry_out) {
    uint64_t sum64 = (uint64_t)x + (uint64_t)y + (uint64_t)carry;
    if (carry_out)
        *carry_out = (uint32_t)(sum64 >> 32);
    return (uint32_t)sum64;
}

static inline Uint bits_add(Uint x, Uint y, Uint carry, Uint *carry_out) {
#if BURROW_PTR_BITS == 64
    return bits_add64(x, y, carry, carry_out);
#else
    return bits_add32(x, y, carry, carry_out);
#endif
}

static inline uint64_t bits_sub64(uint64_t x, uint64_t y, uint64_t borrow,
                                  uint64_t *borrow_out) {
    uint64_t diff = x - y - borrow;
    if (borrow_out)
        *borrow_out = ((~x & y) | (~(x ^ y) & diff)) >> 63;
    return diff;
}

static inline uint32_t bits_sub32(uint32_t x, uint32_t y, uint32_t borrow,
                                  uint32_t *borrow_out) {
    uint32_t diff = x - y - borrow;
    if (borrow_out)
        *borrow_out = ((~x & y) | (~(x ^ y) & diff)) >> 31;
    return diff;
}

static inline Uint bits_sub(Uint x, Uint y, Uint borrow, Uint *borrow_out) {
#if BURROW_PTR_BITS == 64
    return bits_sub64(x, y, borrow, borrow_out);
#else
    return bits_sub32(x, y, borrow, borrow_out);
#endif
}

/* ------------------------------------------------------------------ Mul
 *
 * The full product of x and y. The high half comes back and the low half goes
 * to *lo. */

static inline uint64_t bits_mul64(uint64_t x, uint64_t y, uint64_t *lo) {
#if BURROW__BITS_INT128
    /* ISO C has no 128 bit type, and __extension__ on the typedef is what
     * keeps gcc's -Wpedantic from saying so every time the name is used. */
    __extension__ typedef unsigned __int128 U128;
    U128 p = (U128)x * y;
    if (lo)
        *lo = (uint64_t)p;
    return (uint64_t)(p >> 64);
#elif defined(_MSC_VER) && !defined(__clang__) && defined(_M_X64) &&                   \
    !defined(BURROW__BITS_PORTABLE)
    uint64_t hi;
    uint64_t l = _umul128(x, y, &hi);
    if (lo)
        *lo = l;
    return hi;
#elif defined(_MSC_VER) && !defined(__clang__) && defined(_M_ARM64) &&                 \
    !defined(BURROW__BITS_PORTABLE)
    if (lo)
        *lo = x * y;
    return __umulh(x, y);
#else
    const uint64_t mask32 = 0xffffffffull;
    uint64_t x0 = x & mask32, x1 = x >> 32;
    uint64_t y0 = y & mask32, y1 = y >> 32;
    uint64_t w0 = x0 * y0;
    uint64_t t = x1 * y0 + (w0 >> 32);
    uint64_t w1 = t & mask32, w2 = t >> 32;
    w1 += x0 * y1;
    if (lo)
        *lo = x * y;
    return x1 * y1 + w2 + (w1 >> 32);
#endif
}

static inline uint32_t bits_mul32(uint32_t x, uint32_t y, uint32_t *lo) {
    uint64_t p = (uint64_t)x * y;
    if (lo)
        *lo = (uint32_t)p;
    return (uint32_t)(p >> 32);
}

static inline Uint bits_mul(Uint x, Uint y, Uint *lo) {
#if BURROW_PTR_BITS == 64
    return bits_mul64(x, y, lo);
#else
    return bits_mul32(x, y, lo);
#endif
}

/* -------------------------------------------------------------- Div, Rem
 *
 * (hi, lo) divided by y, as one number twice the width. The quotient comes
 * back and the remainder goes to *rem. Div panics on a zero y and on a y no
 * bigger than hi, since then the quotient does not fit. Rem panics only on the
 * zero, because a remainder always fits.
 *
 * The 64 bit division is Go's, which is Knuth's algorithm D specialised to two
 * digits, from Hacker's Delight. */

static inline uint64_t bits_div64(uint64_t hi, uint64_t lo, uint64_t y, uint64_t *rem) {
    if (y == 0)
        runtime_integer_divide_by_zero();
    if (y <= hi)
        runtime_integer_overflow();

    /* With hi 0 the answer is an ordinary division, and that is the common
     * case by a long way. */
    if (hi == 0) {
        if (rem)
            *rem = lo % y;
        return lo / y;
    }

    Int s = bits_leading_zeros64(y);
    y <<= s;

    const uint64_t two32 = (uint64_t)1 << 32;
    const uint64_t mask32 = two32 - 1;
    uint64_t yn1 = y >> 32;
    uint64_t yn0 = y & mask32;
    uint64_t un32 = hi << s | (s == 0 ? 0 : lo >> (64 - s));
    uint64_t un10 = lo << s;
    uint64_t un1 = un10 >> 32;
    uint64_t un0 = un10 & mask32;
    uint64_t q1 = un32 / yn1;
    uint64_t rhat = un32 - q1 * yn1;

    while (q1 >= two32 || q1 * yn0 > two32 * rhat + un1) {
        q1--;
        rhat += yn1;
        if (rhat >= two32)
            break;
    }

    uint64_t un21 = un32 * two32 + un1 - q1 * y;
    uint64_t q0 = un21 / yn1;
    rhat = un21 - q0 * yn1;

    while (q0 >= two32 || q0 * yn0 > two32 * rhat + un0) {
        q0--;
        rhat += yn1;
        if (rhat >= two32)
            break;
    }

    if (rem)
        *rem = (un21 * two32 + un0 - q0 * y) >> s;
    return q1 * two32 + q0;
}

static inline uint32_t bits_div32(uint32_t hi, uint32_t lo, uint32_t y, uint32_t *rem) {
    if (y != 0 && y <= hi)
        runtime_integer_overflow();
    if (y == 0)
        runtime_integer_divide_by_zero();
    uint64_t z = (uint64_t)hi << 32 | lo;
    if (rem)
        *rem = (uint32_t)(z % y);
    return (uint32_t)(z / y);
}

static inline Uint bits_div(Uint hi, Uint lo, Uint y, Uint *rem) {
#if BURROW_PTR_BITS == 64
    return bits_div64(hi, lo, y, rem);
#else
    return bits_div32(hi, lo, y, rem);
#endif
}

static inline uint64_t bits_rem64(uint64_t hi, uint64_t lo, uint64_t y) {
    /* hi % y first, so that the division below cannot overflow. That is also
     * where a zero y panics. */
    uint64_t r;
    if (y == 0)
        runtime_integer_divide_by_zero();
    bits_div64(hi % y, lo, y, &r);
    return r;
}

static inline uint32_t bits_rem32(uint32_t hi, uint32_t lo, uint32_t y) {
    if (y == 0)
        runtime_integer_divide_by_zero();
    return (uint32_t)((((uint64_t)hi << 32) | lo) % y);
}

static inline Uint bits_rem(Uint hi, Uint lo, Uint y) {
#if BURROW_PTR_BITS == 64
    return bits_rem64(hi, lo, y);
#else
    return bits_rem32(hi, lo, y);
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MATH_BITS_H */
