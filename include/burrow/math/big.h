/* math/big, Go's arbitrary precision arithmetic.
 *
 *     BigInt x = {0}, y = {0};
 *     big_int_set_string(&x, BURROW_S("123456789012345678901234567890"), 10, NULL);
 *     big_int_mul(&y, &x, &x);
 *     big_int_exp(&y, &y, big_new_int(a, 3), NULL);
 *     Str s = big_int_string(&y, a);
 *     big_int_free(&x);
 *     big_int_free(&y);
 *
 * The API is Go's, method for method. A method on Int is big_int_ and the
 * method's name, the receiver comes first, and a method that sets its receiver
 * returns it, so calls chain the way they do in Go. Arguments may alias each
 * other and the receiver freely: big_int_add(&x, &x, &x) doubles x.
 *
 * Memory. A BigInt keeps its words in memory from the allocator in its a
 * field, and NULL there means the heap. {0} is a valid zero on the heap and
 * BIG_INT(a) is a zero on a. A result goes into the receiver's own buffer,
 * which grows when it has to and is never shared with another BigInt, so an
 * Int is a plain value you can keep as long as its allocator lives.
 * big_int_free gives the words back. With an arena there is nothing to free.
 *
 * The working memory a computation needs on the way comes from a scratch
 * arena the calling thread keeps, and is empty again by the time the function
 * returns. Running out of memory panics, as it does in Go, because none of
 * these functions has an error to report it through.
 *
 * A BigInt is not safe to change from two threads at once, and is safe to
 * read from any number of them, as in Go.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

/* burrow:package math/big */

#ifndef BURROW_MATH_BIG_H
#define BURROW_MATH_BIG_H

#include "burrow/core.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/math/rand.h"
#include "burrow/mem.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* big.Word: one digit of a multi-precision number, the machine's word. */
typedef Uint BigWord;

/* big.MaxBase: the largest base big_int_text and big_int_set_string accept. */
#define BIG_MAX_BASE 62

/* big.Accuracy: how a rounded result compares with the exact one. */
typedef int8_t BigAccuracy;
#define BIG_BELOW ((BigAccuracy) - 1)
#define BIG_EXACT ((BigAccuracy)0)
#define BIG_ABOVE ((BigAccuracy)1)

/* Accuracy.String: "Below", "Exact" or "Above", and "Accuracy(n)" for a
 * value that is none of them. */
BURROW_OWNS(ret) Str big_accuracy_string(BigAccuracy i, Alloc *a);

/* big.RoundingMode: how a result is rounded to fit. */
typedef uint8_t BigRoundingMode;
#define BIG_TO_NEAREST_EVEN ((BigRoundingMode)0) /* == IEEE 754-2008 roundTiesToEven */
#define BIG_TO_NEAREST_AWAY ((BigRoundingMode)1) /* == IEEE 754-2008 roundTiesToAway */
#define BIG_TO_ZERO ((BigRoundingMode)2)         /* == IEEE 754-2008 roundTowardZero */
#define BIG_AWAY_FROM_ZERO ((BigRoundingMode)3)  /* no IEEE 754-2008 equivalent */
#define BIG_TO_NEGATIVE_INF                                                            \
    ((BigRoundingMode)4) /* == IEEE 754-2008 roundTowardNegative */
#define BIG_TO_POSITIVE_INF                                                            \
    ((BigRoundingMode)5) /* == IEEE 754-2008 roundTowardPositive */

/* RoundingMode.String: "ToNearestEven" and so on, and
 * "RoundingMode(n)" for a value that is none of them. */
BURROW_OWNS(ret) Str big_rounding_mode_string(BigRoundingMode i, Alloc *a);

/* The modes big_int_divide takes. */
#define BIG_TRUNC BIG_TO_ZERO         /* T-division, the same as Go's / and % */
#define BIG_FLOOR BIG_TO_NEGATIVE_INF /* F-division */
#define BIG_ROUND BIG_TO_NEAREST_EVEN /* R-division */
#define BIG_CEIL BIG_TO_POSITIVE_INF  /* C-division */

/* ------------------------------------------------------------------- Int */

/* The words of a number, least significant first. Not for use outside the
 * library. */
typedef struct burrow__BigNat {
    BigWord *p;
    Int len;
    Int cap;
} burrow__BigNat;

/* big.Int: a signed integer of any size. The fields are here so that one can
 * live on the stack. Read the value through the functions below and leave the
 * fields alone, except a, which you may set while the Int is zero. */
typedef struct BigInt {
    Alloc *a; /* where the words live, NULL for the heap */
    bool neg;
    burrow__BigNat abs;
} BigInt;

/* A zero BigInt whose words will come from a. */
#define BIG_INT(alloc) ((BigInt){.a = (alloc), .neg = false, .abs = {NULL, 0, 0}})

/* Gives the words back to the allocator and leaves x zero, still on the same
 * allocator. NULL is fine. */
void big_int_free(BigInt *x);

/* big.NewInt: a new Int set to x, the struct and its words both from a. */
BURROW_OWNS(ret) BigInt *big_new_int(Alloc *a, int64_t x);

/* Int.Sign: -1, 0 or +1. */
Int big_int_sign(const BigInt *x);

/* Int.SetInt64, SetUint64 and Set: z = x. */
BigInt *big_int_set_int64(BigInt *z, int64_t x);
BigInt *big_int_set_uint64(BigInt *z, uint64_t x);
BURROW_BORROWS(ret, z) BigInt *big_int_set(BigInt *z, const BigInt *x);

/* Int.Bits: the words of |x|, least significant first, as a Slice of
 * BigWord. It is a view of x and changes when x does. */
BURROW_BORROWS(ret, x) Slice big_int_bits(const BigInt *x);

/* Int.SetBits: z = the words in abs, a Slice of BigWord, as an unsigned
 * number. Go shares the slice with z. This copies it, because z owns its
 * words. */
BURROW_BORROWS(ret, z) BigInt *big_int_set_bits(BigInt *z, Slice abs);

/* Int.Abs and Neg: z = |x| and z = -x. */
BURROW_BORROWS(ret, z) BigInt *big_int_abs(BigInt *z, const BigInt *x);
BURROW_BORROWS(ret, z) BigInt *big_int_neg(BigInt *z, const BigInt *x);

/* Int.Add, Sub and Mul. */
BURROW_BORROWS(ret, z) BigInt *big_int_add(BigInt *z, const BigInt *x, const BigInt *y);
BURROW_BORROWS(ret, z) BigInt *big_int_sub(BigInt *z, const BigInt *x, const BigInt *y);
BURROW_BORROWS(ret, z) BigInt *big_int_mul(BigInt *z, const BigInt *x, const BigInt *y);

/* Int.MulRange: z = the product of the integers in [a, b], 1 when the range
 * is empty. */
BURROW_BORROWS(ret, z) BigInt *big_int_mul_range(BigInt *z, int64_t a, int64_t b);

/* Int.Binomial: z = the binomial coefficient C(n, k). */
BURROW_BORROWS(ret, z) BigInt *big_int_binomial(BigInt *z, int64_t n, int64_t k);

/* Int.Quo, Rem and QuoRem: truncated division, the kind Go's / and % do.
 * Division by zero panics. QuoRem sets z to the quotient and r to the
 * remainder, and returns z. */
BURROW_BORROWS(ret, z) BigInt *big_int_quo(BigInt *z, const BigInt *x, const BigInt *y);
BURROW_BORROWS(ret, z) BigInt *big_int_rem(BigInt *z, const BigInt *x, const BigInt *y);
BURROW_BORROWS(ret, z) BigInt *big_int_quo_rem(BigInt *z, const BigInt *x,
                                               const BigInt *y, BigInt *r);

/* Int.Div, Mod and DivMod: Euclidean division, where the remainder is never
 * negative. DivMod sets z to the quotient and m to the modulus, and returns
 * z. */
BURROW_BORROWS(ret, z) BigInt *big_int_div(BigInt *z, const BigInt *x, const BigInt *y);
BURROW_BORROWS(ret, z) BigInt *big_int_mod(BigInt *z, const BigInt *x, const BigInt *y);
BURROW_BORROWS(ret, z) BigInt *big_int_div_mod(BigInt *z, const BigInt *x,
                                               const BigInt *y, BigInt *m);

/* Int.Divide: division rounded the way mode says, BIG_TRUNC, BIG_FLOOR,
 * BIG_ROUND or BIG_CEIL. Sets z to the quotient and r to the remainder,
 * either of which may be NULL, and returns z. Any other mode panics. */
BURROW_BORROWS(ret, z) BigInt *big_int_divide(BigInt *z, const BigInt *x,
                                              const BigInt *y, BigInt *r,
                                              BigRoundingMode mode);

/* Int.Cmp and CmpAbs: -1, 0 or +1 as x is less than, equal to or greater
 * than y, comparing the absolute values for CmpAbs. */
Int big_int_cmp(const BigInt *x, const BigInt *y);
Int big_int_cmp_abs(const BigInt *x, const BigInt *y);

/* Int.Int64 and Uint64: the low 64 bits of x, which is x itself when
 * IsInt64 or IsUint64 says it fits. */
int64_t big_int_int64(const BigInt *x);
uint64_t big_int_uint64(const BigInt *x);
bool big_int_is_int64(const BigInt *x);
bool big_int_is_uint64(const BigInt *x);

/* Int.Float64: the float64 nearest x, ties to even, and in *acc how that
 * compares with x. A value too big is an infinity. acc may be NULL. */
double big_int_float64(const BigInt *x, BigAccuracy *acc);

/* Int.SetString: z = the number in s, in base, which is 0 or 2 through
 * BIG_MAX_BASE. Base 0 reads a prefix: 0b, 0o or 0 for octal, 0x, or none for
 * decimal, and allows underscores between digits. Returns z, or NULL when s is
 * not all a number, and says the same in *ok, which may be NULL. */
BURROW_BORROWS(ret, z) BigInt *big_int_set_string(BigInt *z, Str s, Int base, bool *ok);

/* Int.SetBytes: z = buf read as a big endian unsigned number. */
BURROW_BORROWS(ret, z) BigInt *big_int_set_bytes(BigInt *z, Slice buf);

/* Int.Bytes: |x| as big endian bytes, as few as it takes. */
BURROW_OWNS(ret) Slice big_int_bytes(const BigInt *x, Alloc *a);

/* Int.FillBytes: |x| as big endian bytes, filling buf and zero padded on the
 * left. Panics when x does not fit. Returns buf. */
BURROW_BORROWS(ret, buf) Slice big_int_fill_bytes(const BigInt *x, Slice buf);

/* Int.BitLen and TrailingZeroBits, of |x|. */
Int big_int_bit_len(const BigInt *x);
Uint big_int_trailing_zero_bits(const BigInt *x);

/* Int.Exp: z = x**y mod |m|, or x**y when m is NULL or zero. A negative y
 * needs m, and uses the inverse of x mod m, and when there is none the result
 * is NULL and z is left alone. For y <= 0 without a modulus the result is 1. */
BURROW_BORROWS(ret, z) BigInt *big_int_exp(BigInt *z, const BigInt *x, const BigInt *y,
                                           const BigInt *m);

/* Int.GCD: z = the greatest common divisor of a and b, and when x and y are
 * not NULL, the x and y with z = a*x + b*y. */
BURROW_BORROWS(ret, z) BigInt *big_int_gcd(BigInt *z, BigInt *x, BigInt *y,
                                           const BigInt *a, const BigInt *b);

/* Int.Rand: z = a pseudo random number in [0, n), drawn from rnd. Not for
 * cryptography. */
BURROW_BORROWS(ret, z) BigInt *big_int_rand(BigInt *z, MathRandRand *rnd,
                                            const BigInt *n);

/* Int.ModInverse: z = the inverse of g in the ring of integers mod n. NULL,
 * with z left alone, when g and n are not relatively prime. */
BURROW_BORROWS(ret, z) BigInt *big_int_mod_inverse(BigInt *z, const BigInt *g,
                                                   const BigInt *n);

/* big.Jacobi: the Jacobi symbol (x/y), -1, 0 or +1. y has to be odd. */
Int big_jacobi(const BigInt *x, const BigInt *y);

/* Int.ModSqrt: z = a square root of x mod the prime p, or NULL, with z left
 * alone, when x is not a square mod p. p not prime gives an answer that means
 * nothing, or panics. */
BURROW_BORROWS(ret, z) BigInt *big_int_mod_sqrt(BigInt *z, const BigInt *x,
                                                const BigInt *p);

/* Int.Lsh and Rsh: z = x << n and z = x >> n. Rsh on a negative x rounds
 * toward minus infinity, as an arithmetic shift does. */
BURROW_BORROWS(ret, z) BigInt *big_int_lsh(BigInt *z, const BigInt *x, Uint n);
BURROW_BORROWS(ret, z) BigInt *big_int_rsh(BigInt *z, const BigInt *x, Uint n);

/* Int.Bit and SetBit: bit i of x, in two's complement for a negative x. i
 * must not be negative. */
Uint big_int_bit(const BigInt *x, Int i);
BURROW_BORROWS(ret, z) BigInt *big_int_set_bit(BigInt *z, const BigInt *x, Int i,
                                               Uint b);

/* Int.And, AndNot, Or, Xor and Not: bitwise, in two's complement. */
BURROW_BORROWS(ret, z) BigInt *big_int_and(BigInt *z, const BigInt *x, const BigInt *y);
BURROW_BORROWS(ret, z) BigInt *big_int_and_not(BigInt *z, const BigInt *x,
                                               const BigInt *y);
BURROW_BORROWS(ret, z) BigInt *big_int_or(BigInt *z, const BigInt *x, const BigInt *y);
BURROW_BORROWS(ret, z) BigInt *big_int_xor(BigInt *z, const BigInt *x, const BigInt *y);
BURROW_BORROWS(ret, z) BigInt *big_int_not(BigInt *z, const BigInt *x);

/* Int.Sqrt: z = the floor of the square root of x. Panics for a negative x. */
BURROW_BORROWS(ret, z) BigInt *big_int_sqrt(BigInt *z, const BigInt *x);

/* Int.ProbablyPrime: whether x is probably prime, by n rounds of Miller
 * Rabin with pseudo random bases and a Baillie-PSW test. 100% accurate below
 * 2**64. Not for inputs an adversary may have chosen when n is 0. */
bool big_int_probably_prime(const BigInt *x, Int n);

/* Int.Text and String: x in base, 2 through BIG_MAX_BASE, with lower case
 * letters for 10 to 35 and upper case above. "<nil>" for a NULL x. */
BURROW_OWNS(ret) Str big_int_text(const BigInt *x, Alloc *a, Int base);
BURROW_OWNS(ret) Str big_int_string(const BigInt *x, Alloc *a);

/* Int.Append: buf with x in base on the end. */
BURROW_OWNS(ret) Slice big_int_append(const BigInt *x, Alloc *a, Slice buf, Int base);

/* Int.Format: what fmt calls for %b, %o, %O, %d, %x, %X, %s and %v, with
 * Go's flags, width and precision. fmt finds it through TYPE_BIG_INT, so
 *
 *     fmt_printf_v("%08x\n", BURROW_ANY(TYPE_BIG_INT, &x));
 *
 * works, and so does %v inside a struct. */
void big_int_format(const BigInt *x, FmtState s, Rune ch);

/* Int.Scan: what fmt's scanning calls to read a number, for the verbs b, o,
 * d, x, X, s and v. */
BURROW_BORROWS(ret) Error big_int_scan(BigInt *z, FmtScanState s, Rune ch);

/* Int.AppendText, MarshalText and UnmarshalText: the decimal form. Unmarshal
 * reads a prefix the way SetString with base 0 does. */
BURROW_OWNS(ret) Slice big_int_append_text(const BigInt *x, Alloc *a, Slice b,
                                           Error *err);
BURROW_OWNS(ret) Slice big_int_marshal_text(const BigInt *x, Alloc *a, Error *err);
BURROW_BORROWS(ret) Error big_int_unmarshal_text(BigInt *z, Slice text);

/* Int.MarshalJSON and UnmarshalJSON: the decimal form as a JSON number,
 * null for a NULL x, and null read as leaving z alone. */
BURROW_OWNS(ret) Slice big_int_marshal_json(const BigInt *x, Alloc *a, Error *err);
BURROW_BORROWS(ret) Error big_int_unmarshal_json(BigInt *z, Slice text);

/* Int.GobEncode and GobDecode: Go's gob form, a version and sign byte and
 * then the big endian bytes. */
BURROW_OWNS(ret) Slice big_int_gob_encode(const BigInt *x, Alloc *a, Error *err);
BURROW_BORROWS(ret) Error big_int_gob_decode(BigInt *z, Slice buf);

/* The type descriptor for *Int, with the Format, Scan, String and marshaling
 * methods on it, for BURROW_ANY. */
extern const Type burrow_type_BigInt;
extern const Type *const TYPE_BIG_INT;

/* ------------------------------------------------------------------- Rat */

/* big.Rat: a quotient a/b of any precision, kept in lowest terms with b > 0.
 * A zero BigRat is 0 and ready to use. Its memory works the way an Int's does:
 * the numerator and the denominator keep their words in memory from their own
 * a fields, NULL for the heap, and BIG_RAT(a) is a zero Rat on a. Read the
 * value through the functions below. A denominator with no words means 1. */
typedef struct BigRat {
    BigInt a; /* the numerator, which carries the sign */
    BigInt b; /* the denominator, never negative */
} BigRat;

/* A zero BigRat whose words will come from a. */
#define BIG_RAT(alloc) ((BigRat){BIG_INT(alloc), BIG_INT(alloc)})

/* Gives the words of both halves back and leaves x zero. NULL is fine. */
void big_rat_free(BigRat *x);

/* big.NewRat: a new Rat set to num/den, the struct and its words both from a.
 * A zero den panics. */
BURROW_OWNS(ret) BigRat *big_new_rat(Alloc *a, int64_t num, int64_t den);

/* Rat.SetFloat64: z = f exactly. NULL, with z left alone, when f is not
 * finite. */
BURROW_BORROWS(ret, z) BigRat *big_rat_set_float64(BigRat *z, double f);

/* Rat.Float32 and Float64: the nearest float to x, ties to even, and in
 * *exact whether it is x itself. A value too big is an infinity. exact may be
 * NULL. */
float big_rat_float32(const BigRat *x, bool *exact);
double big_rat_float64(const BigRat *x, bool *exact);

/* Rat.SetFrac and SetFrac64: z = a/b, reduced. A zero b panics. */
BURROW_BORROWS(ret, z) BigRat *big_rat_set_frac(BigRat *z, const BigInt *a,
                                                const BigInt *b);
BURROW_BORROWS(ret, z) BigRat *big_rat_set_frac64(BigRat *z, int64_t a, int64_t b);

/* Rat.SetInt, SetInt64, SetUint64 and Set: z = x. */
BURROW_BORROWS(ret, z) BigRat *big_rat_set_int(BigRat *z, const BigInt *x);
BURROW_BORROWS(ret, z) BigRat *big_rat_set_int64(BigRat *z, int64_t x);
BURROW_BORROWS(ret, z) BigRat *big_rat_set_uint64(BigRat *z, uint64_t x);
BURROW_BORROWS(ret, z) BigRat *big_rat_set(BigRat *z, const BigRat *x);

/* Rat.Abs, Neg and Inv: z = |x|, -x and 1/x. Inv of zero panics. */
BURROW_BORROWS(ret, z) BigRat *big_rat_abs(BigRat *z, const BigRat *x);
BURROW_BORROWS(ret, z) BigRat *big_rat_neg(BigRat *z, const BigRat *x);
BURROW_BORROWS(ret, z) BigRat *big_rat_inv(BigRat *z, const BigRat *x);

/* Rat.Sign: -1, 0 or +1. */
Int big_rat_sign(const BigRat *x);

/* Rat.IsInt: whether the denominator is 1. */
bool big_rat_is_int(const BigRat *x);

/* Rat.Num and Denom: the numerator, which may be negative, and the
 * denominator, which is always positive. Both are x's own Ints, so changing
 * one changes x, and setting x changes them. Go's Denom hands back a new Int
 * for a Rat whose denominator has no words yet. This stores a 1 in x instead,
 * so on a zero Rat it is a write. */
BURROW_BORROWS(ret, x) BigInt *big_rat_num(BigRat *x);
BURROW_BORROWS(ret, x) BigInt *big_rat_denom(BigRat *x);

/* Rat.Cmp: -1, 0 or +1 as x is less than, equal to or greater than y. */
Int big_rat_cmp(const BigRat *x, const BigRat *y);

/* Rat.Add, Sub, Mul and Quo. Quo by zero panics. */
BURROW_BORROWS(ret, z) BigRat *big_rat_add(BigRat *z, const BigRat *x, const BigRat *y);
BURROW_BORROWS(ret, z) BigRat *big_rat_sub(BigRat *z, const BigRat *x, const BigRat *y);
BURROW_BORROWS(ret, z) BigRat *big_rat_mul(BigRat *z, const BigRat *x, const BigRat *y);
BURROW_BORROWS(ret, z) BigRat *big_rat_quo(BigRat *z, const BigRat *x, const BigRat *y);

/* Rat.SetString: z = the number in s, a fraction "a/b" or a floating point
 * number with an optional exponent. A fraction's parts, and a number without
 * a fraction or an exponent, may have a base prefix. Returns z, or NULL when s
 * is not all a number, and says the same in *ok, which may be NULL. */
BURROW_BORROWS(ret, z) BigRat *big_rat_set_string(BigRat *z, Str s, bool *ok);

/* Rat.String: "a/b", with the /1 kept. RatString leaves it off when x is an
 * integer. */
BURROW_OWNS(ret) Str big_rat_string(const BigRat *x, Alloc *a);
BURROW_OWNS(ret) Str big_rat_rat_string(const BigRat *x, Alloc *a);

/* Rat.FloatString: x in decimal with prec digits after the point, the last
 * one rounded half away from zero. */
BURROW_OWNS(ret) Str big_rat_float_string(const BigRat *x, Alloc *a, Int prec);

/* Rat.FloatPrec: how many digits after the point x takes in decimal, and in
 * *exact whether that many is enough, which is when the denominator has no
 * prime factors but 2 and 5. exact may be NULL. */
Int big_rat_float_prec(const BigRat *x, bool *exact);

/* Rat.Scan: what fmt's scanning calls to read a Rat, for the verbs e, E, f,
 * F, g, G and v. */
BURROW_BORROWS(ret) Error big_rat_scan(BigRat *z, FmtScanState s, Rune ch);

/* Rat.AppendText, MarshalText and UnmarshalText: the RatString form. */
BURROW_OWNS(ret) Slice big_rat_append_text(const BigRat *x, Alloc *a, Slice b,
                                           Error *err);
BURROW_OWNS(ret) Slice big_rat_marshal_text(const BigRat *x, Alloc *a, Error *err);
BURROW_BORROWS(ret) Error big_rat_unmarshal_text(BigRat *z, Slice text);

/* Rat.GobEncode and GobDecode: Go's gob form, a version and sign byte, the
 * numerator's length in four bytes, and both halves as big endian bytes. */
BURROW_OWNS(ret) Slice big_rat_gob_encode(const BigRat *x, Alloc *a, Error *err);
BURROW_BORROWS(ret) Error big_rat_gob_decode(BigRat *z, Slice buf);

/* The type descriptor for *Rat, with the Scan, String and marshaling methods
 * on it, for BURROW_ANY. */
extern const Type burrow_type_BigRat;
extern const Type *const TYPE_BIG_RAT;

#ifdef __cplusplus
}
#endif

#endif /* BURROW_MATH_BIG_H */
