/* Constant time modular arithmetic on numbers of any size, which crypto/rsa
 * and crypto/ecdsa are built on. Go keeps this in
 * crypto/internal/fips140/bigmod, where nothing outside the standard library
 * can import it, so here it is a private header.
 *
 * A BigmodNat is a natural number in words of BITS_UINT_SIZE bits, little
 * endian, with an announced length, the number of words it has. The
 * operations can leak that length but nothing about the values in the words.
 * A BigmodModulus is a number of at least two, stored with no zero words on
 * top, and with the constants for Montgomery multiplication when it is odd.
 *
 * Go makes a new Nat whenever it wants one and lets the collector have it. A
 * BigmodNat here keeps the allocator its words come from, NULL for the heap,
 * and grows from it when an operation needs more words. bigmod_nat_free gives
 * the words back. The scratch numbers an operation needs for itself live on
 * the stack while they are no bigger than a 2048 bit number, as they do in Go,
 * and come from the heap and go back to it when they are bigger.
 *
 * The functions take the receiver first, as Go's methods do, and return it.
 * Where Go returns an error, they take an Error * and return NULL. A choice is
 * a Uint that is 1 or 0. The names in Go and here:
 *
 *     NewNat               bigmod_nat_init, bigmod_new_nat
 *     (*Nat).SetBytes      bigmod_nat_set_bytes
 *     (*Nat).Mul           bigmod_nat_mul
 *     NewModulus           bigmod_new_modulus
 *     (*Modulus).Nat       bigmod_modulus_nat
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_CRYPTO_BIGMOD_H
#define BURROW_CRYPTO_BIGMOD_H

/* crypto.h comes first so that the amalgamation files this header with
 * package crypto, which the packages that use it all import. */
#include "burrow/crypto.h"

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/math/bits.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include <stdbool.h>

/* _W and _S: the size of a word in bits and in bytes. */
#define BIGMOD_W BITS_UINT_SIZE
#define BIGMOD_S (BITS_UINT_SIZE / 8)

/* preallocLimbs: the words of a 2048 bit number, the most common RSA size. */
#define BIGMOD_PREALLOC_LIMBS ((2048 + BIGMOD_W - 1) / BIGMOD_W)

/* choice: 1 or 0, as a word, so that it can become a mask. */
typedef Uint BigmodChoice;

typedef struct BigmodNat {
    Uint *limbs; /* len words in use, cap there */
    Int len;
    Int cap;
    Alloc *a;      /* where the words come from, NULL for the heap */
    bool borrowed; /* the words are someone else's, not to be freed */
} BigmodNat;

typedef struct BigmodModulus {
    BigmodNat nat; /* the number, with no zero words on top */
    bool odd;      /* the fields below are only set when it is */
    Uint m0inv;    /* -nat.limbs[0]⁻¹ mod 2^_W */
    BigmodNat rr;  /* R*R mod m, for the Montgomery form */
} BigmodModulus;

/* NewNat: x as zero words long, growing from a. */
void bigmod_nat_init(BigmodNat *x, Alloc *a);

/* NewNat, with the struct from a too. Panics when a has no memory. */
BURROW_OWNS(ret) BigmodNat *bigmod_new_nat(Alloc *a);

/* x as zero words long, in the cap words at buf, which stay the caller's. When
 * x needs more, it gets them from the heap. */
void bigmod_nat_init_buf(BigmodNat *x, Uint *buf, Int cap);

/* The words of x back to where they came from, leaving it zero words long.
 * The struct itself is the caller's, even from bigmod_new_nat. */
void bigmod_nat_free(BigmodNat *x);

/* expand: x as n words, with the same value. Panics if x has more. */
BigmodNat *bigmod_nat_expand(BigmodNat *x, Int n);

/* reset: x as zero, n words long. */
BigmodNat *bigmod_nat_reset(BigmodNat *x, Int n);

/* resetToBytes: x = b, big endian, with as many words as the value needs. */
BigmodNat *bigmod_nat_reset_to_bytes(BigmodNat *x, Slice b);

/* trim: x without the zero words on top. */
BigmodNat *bigmod_nat_trim(BigmodNat *x);

/* set: x = y, as long as y. */
BigmodNat *bigmod_nat_set(BigmodNat *x, const BigmodNat *y);

/* Bits: the words of x, a Slice of Uint that shares them. */
BURROW_BORROWS(ret, x) Slice bigmod_nat_bits(BigmodNat *x);

/* SetBits: x = the words in y, a Slice of Uint, as long as y. */
BigmodNat *bigmod_nat_set_bits(BigmodNat *x, Slice y);

/* Bytes: x big endian, padded to the size of m, from a. x must be as long as
 * m and no bigger than it. Panics "bigmod: modulus is smaller than nat" when x
 * does not fit. */
BURROW_OWNS(ret) Slice bigmod_nat_bytes(const BigmodNat *x, Alloc *a,
                                        const BigmodModulus *m);

/* SetBytes: x = b, big endian, as long as m. An error when b >= m. */
BigmodNat *bigmod_nat_set_bytes(BigmodNat *x, Slice b, const BigmodModulus *m,
                                Error *err);

/* SetOverflowingBytes: x = b mod m, as long as m, for b with no more bits than
 * m. An error when it has more. */
BigmodNat *bigmod_nat_set_overflowing_bytes(BigmodNat *x, Slice b,
                                            const BigmodModulus *m, Error *err);

/* SetUint: x = y, one word long. */
BigmodNat *bigmod_nat_set_uint(BigmodNat *x, Uint y);

/* Equal: whether x == y, for x and y of the same length. */
BigmodChoice bigmod_nat_equal(const BigmodNat *x, const BigmodNat *y);

/* IsZero, IsOne and IsOdd. */
BigmodChoice bigmod_nat_is_zero(const BigmodNat *x);
BigmodChoice bigmod_nat_is_one(const BigmodNat *x);
BigmodChoice bigmod_nat_is_odd(const BigmodNat *x);

/* IsMinusOne: whether x == -1 mod m, for x reduced and as long as m. */
BigmodChoice bigmod_nat_is_minus_one(const BigmodNat *x, const BigmodModulus *m);

/* TrailingZeroBitsVarTime: the zero bits at the bottom of x. */
Uint bigmod_nat_trailing_zero_bits_var_time(const BigmodNat *x);

/* ShiftRightVarTime: x = x >> n, with the same length. */
BigmodNat *bigmod_nat_shift_right_var_time(BigmodNat *x, Uint n);

/* ShiftRightByOne: x = x >> 1, with the same length. */
BigmodNat *bigmod_nat_shift_right_by_one(BigmodNat *x);

/* BitLenVarTime: the bits x needs, which leaks through the timing. */
Int bigmod_nat_bit_len_var_time(const BigmodNat *x);

/* DivShortVarTime: x = x / y, returning the remainder. Panics
 * "bigmod: division by zero" when y is zero. */
Uint bigmod_nat_div_short_var_time(BigmodNat *x, Uint y);

/* NewModulus: the modulus b, big endian, from a. An error, "modulus must be >
 * 1", for zero and one. How many bits it has and whether it is even leak
 * through the timing. */
BURROW_OWNS(ret) BigmodModulus *bigmod_new_modulus(Alloc *a, Slice b, Error *err);

/* NewModulusProduct: the modulus x*y, both big endian, from a. */
BURROW_OWNS(ret) BigmodModulus *bigmod_new_modulus_product(Alloc *a, Slice x, Slice y,
                                                           Error *err);

/* m's words and the struct back to the allocator it came from. */
void bigmod_modulus_free(BigmodModulus *m);

/* Size and BitLen: the bytes and bits of m. */
Int bigmod_modulus_size(const BigmodModulus *m);
Int bigmod_modulus_bit_len(const BigmodModulus *m);

/* Nat: a copy of m as a new Nat from a. */
BURROW_OWNS(ret) BigmodNat *bigmod_modulus_nat(const BigmodModulus *m, Alloc *a);

/* ExpandFor: x as long as m, with the same value, which must fit. */
BigmodNat *bigmod_nat_expand_for(BigmodNat *x, const BigmodModulus *m);

/* resetFor: x as zero, as long as m. */
BigmodNat *bigmod_nat_reset_for(BigmodNat *x, const BigmodModulus *m);

/* Mod: out = x mod m, as long as m, for x of any size. out and x must not be
 * the same Nat. */
BigmodNat *bigmod_nat_mod(BigmodNat *out, const BigmodNat *x, const BigmodModulus *m);

/* shiftIn: x = x << _W + y mod m, for x reduced. */
BigmodNat *bigmod_nat_shift_in(BigmodNat *x, Uint y, const BigmodModulus *m);

/* maybeSubtractModulus: x -= m if x >= m or always is 1. */
void bigmod_nat_maybe_subtract_modulus(BigmodNat *x, BigmodChoice always,
                                       const BigmodModulus *m);

/* Sub, SubOne, Add and Mul: x = x - y, x - 1, x + y and x * y mod m, for
 * operands reduced and as long as m. */
BigmodNat *bigmod_nat_sub(BigmodNat *x, const BigmodNat *y, const BigmodModulus *m);
BigmodNat *bigmod_nat_sub_one(BigmodNat *x, const BigmodModulus *m);
BigmodNat *bigmod_nat_add(BigmodNat *x, const BigmodNat *y, const BigmodModulus *m);
BigmodNat *bigmod_nat_mul(BigmodNat *x, const BigmodNat *y, const BigmodModulus *m);

/* montgomeryRepresentation and montgomeryReduction: x = x * R and x / R mod
 * m, with R = 2^(_W * n) for m n words long. m must be odd. */
BigmodNat *bigmod_nat_montgomery_representation(BigmodNat *x, const BigmodModulus *m);
BigmodNat *bigmod_nat_montgomery_reduction(BigmodNat *x, const BigmodModulus *m);

/* montgomeryMul: x = a * b / R mod m, for a and b reduced and as long as m. x
 * may be a or b. */
BigmodNat *bigmod_nat_montgomery_mul(BigmodNat *x, const BigmodNat *a,
                                     const BigmodNat *b, const BigmodModulus *m);

/* Exp: out = x^e mod m, for e big endian and x reduced. Panics "bigmod:
 * modulus for Exp must be odd" when m is even. out may be x. */
BigmodNat *bigmod_nat_exp(BigmodNat *out, const BigmodNat *x, Slice e,
                          const BigmodModulus *m);

/* ExpShortVarTime: out = x^e mod m, leaking e through the timing. Panics
 * "bigmod: modulus for ExpShortVarTime must be odd" when m is even. out may
 * be x. An e of zero gives x, as in Go. */
BigmodNat *bigmod_nat_exp_short_var_time(BigmodNat *out, const BigmodNat *x, Uint e,
                                         const BigmodModulus *m);

/* InverseVarTime: x = a⁻¹ mod m, for a reduced, and whether there is one.
 * When there is not, x is as it was. */
bool bigmod_nat_inverse_var_time(BigmodNat *x, const BigmodNat *a,
                                 const BigmodModulus *m);

/* GCDVarTime: x = gcd(a, b), for a and b not zero and not both even, which are
 * errors that leave x as it was. x is as long as the longer of the two. */
BigmodNat *bigmod_nat_gcd_var_time(BigmodNat *x, const BigmodNat *a, const BigmodNat *b,
                                   Error *err);

/* addMulVVW: z += x * y over len words, returning the carry. The sized ones
 * are for 1024, 1536 and 2048 bits, which Go has in assembly. */
Uint bigmod_add_mul_vvw(Uint *z, const Uint *x, Uint y, Int len);
Uint bigmod_add_mul_vvw1024(Uint *z, const Uint *x, Uint y);
Uint bigmod_add_mul_vvw1536(Uint *z, const Uint *x, Uint y);
Uint bigmod_add_mul_vvw2048(Uint *z, const Uint *x, Uint y);

#endif /* BURROW_CRYPTO_BIGMOD_H */
