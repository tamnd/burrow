/* Derived from Go's src/crypto/internal/fips140/edwards25519/edwards25519_test.go,
 * scalar_test.go, scalar_alias_test.go, scalarmult_test.go and tables_test.go,
 * and from field/fe_test.go, fe_alias_test.go and fe_bench_test.go.
 * Go source: go1.27.1.
 *
 * Go checks most of these properties with testing/quick, which draws its
 * inputs from a math/rand source seeded with the time. Here the same
 * generators run off a fixed seed, so a failure can be run again, for the
 * count quick would use: 100 in -test.short, and that times the scale Go
 * passes otherwise. TestFeMul compares the assembly multiply with the generic
 * one in Go and the two names for the generic one here, since there is no
 * assembly. TestComparable is about Go's == on a Point, which C does not have.
 *
 * TestBasepointTableGeneration and TestBasepointNafTableGeneration compare the
 * tables built with the C arithmetic against the constants in
 * edwards25519_table.c, which Go's code printed, so they also check that the C
 * gives the same limbs as Go and not just the same numbers.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto/rand.h"
#include "burrow/math/big.h"

#include "../src/crypto/edwards25519.h"

#include <stdint.h>
#include <string.h>

typedef Edwards25519Element Fe;
typedef Edwards25519Scalar Sc;
typedef Edwards25519Point Ge;

/* ------------------------------------------------------------- helpers */

/* splitmix64, the source the generators below draw from. */
typedef struct Rng {
    uint64_t s;
} Rng;

static uint64_t rng_u64(Rng *r) {
    uint64_t z = (r->s += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

static int rng_intn(Rng *r, int n) {
    return (int)(rng_u64(r) % (uint64_t)n);
}

static void rng_read(Rng *r, uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)rng_u64(r);
}

/* How many times quick.Check calls the function with quickCheckConfig(scale). */
static int quick_count(int scale) {
    return testing_short() ? 100 : 100 * scale;
}

static Slice bs(const void *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

static int unhex_digit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    return c - 'a' + 10;
}

/* Go's decodeHex, for the strings of these tests, which are all lower case. */
static Int unhex(const char *s, uint8_t *out) {
    Int n = (Int)strlen(s) / 2;
    for (Int i = 0; i < n; i++)
        out[i] = (uint8_t)(unhex_digit(s[2 * i]) << 4 | unhex_digit(s[2 * i + 1]));
    return n;
}

static void tohex(const uint8_t *p, Int n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (Int i = 0; i < n; i++) {
        out[2 * i] = digits[p[i] >> 4];
        out[2 * i + 1] = digits[p[i] & 15];
    }
    out[2 * n] = 0;
}

/* -------------------------------------------------------------- field */

static Fe generate_field_element(Rng *r) {
    const uint64_t mask_low_52_bits = (UINT64_C(1) << 52) - 1;
    Fe v;
    v.l0 = rng_u64(r) & mask_low_52_bits;
    v.l1 = rng_u64(r) & mask_low_52_bits;
    v.l2 = rng_u64(r) & mask_low_52_bits;
    v.l3 = rng_u64(r) & mask_low_52_bits;
    v.l4 = rng_u64(r) & mask_low_52_bits;
    return v;
}

/* Limbs that combine into edge cases. 0 and -1 come up more often, as they
 * combine well. */
static const uint64_t weird_limbs_51[] = {
    0,
    0,
    0,
    0,
    1,
    19 - 1,
    19,
    UINT64_C(0x2aaaaaaaaaaaa),
    UINT64_C(0x5555555555555),
    (UINT64_C(1) << 51) - 20,
    (UINT64_C(1) << 51) - 19,
    (UINT64_C(1) << 51) - 1,
    (UINT64_C(1) << 51) - 1,
    (UINT64_C(1) << 51) - 1,
    (UINT64_C(1) << 51) - 1,
};

static const uint64_t weird_limbs_52[] = {
    0,
    0,
    0,
    0,
    0,
    0,
    1,
    19 - 1,
    19,
    UINT64_C(0x2aaaaaaaaaaaa),
    UINT64_C(0x5555555555555),
    (UINT64_C(1) << 51) - 20,
    (UINT64_C(1) << 51) - 19,
    (UINT64_C(1) << 51) - 1,
    (UINT64_C(1) << 51) - 1,
    (UINT64_C(1) << 51) - 1,
    (UINT64_C(1) << 51) - 1,
    (UINT64_C(1) << 51) - 1,
    (UINT64_C(1) << 51) - 1,
    UINT64_C(1) << 51,
    (UINT64_C(1) << 51) + 1,
    (UINT64_C(1) << 52) - 19,
    (UINT64_C(1) << 52) - 1,
};

#define NELEM(x) ((int)(sizeof(x) / sizeof((x)[0])))

static Fe generate_weird_field_element(Rng *r) {
    Fe v;
    v.l0 = weird_limbs_52[rng_intn(r, NELEM(weird_limbs_52))];
    v.l1 = weird_limbs_51[rng_intn(r, NELEM(weird_limbs_51))];
    v.l2 = weird_limbs_51[rng_intn(r, NELEM(weird_limbs_51))];
    v.l3 = weird_limbs_51[rng_intn(r, NELEM(weird_limbs_51))];
    v.l4 = weird_limbs_51[rng_intn(r, NELEM(weird_limbs_51))];
    return v;
}

/* Element.Generate. */
static Fe gen_fe(Rng *r) {
    if (rng_intn(r, 2) == 0)
        return generate_weird_field_element(r);
    return generate_field_element(r);
}

static int bit_len64(uint64_t x) {
    int n = 0;
    while (x != 0) {
        n++;
        x >>= 1;
    }
    return n;
}

/* Whether every limb is within the bounds a light reduction leaves. */
static bool is_in_bounds(const Fe *x) {
    return bit_len64(x->l0) <= 52 && bit_len64(x->l1) <= 52 && bit_len64(x->l2) <= 52 &&
           bit_len64(x->l3) <= 52 && bit_len64(x->l4) <= 52;
}

static bool fe_same(const Fe *a, const Fe *b) {
    return memcmp(a, b, sizeof *a) == 0;
}

static Fe fe_from_bytes(const uint8_t b[32]) {
    Fe v = {0, 0, 0, 0, 0};
    burrow__fe_set_bytes(&v, bs(b, 32), NULL);
    return v;
}

static void TestMultiplyDistributesOverAdd(TestingT *t) {
    Rng r = {1};
    for (int i = 0, n = quick_count(1024); i < n; i++) {
        Fe x = gen_fe(&r), y = gen_fe(&r), z = gen_fe(&r);
        /* t1 = (x+y)*z */
        Fe t1, t2, t3;
        burrow__fe_add(&t1, &x, &y);
        burrow__fe_multiply(&t1, &t1, &z);
        /* t2 = x*z + y*z */
        burrow__fe_multiply(&t2, &x, &z);
        burrow__fe_multiply(&t3, &y, &z);
        burrow__fe_add(&t2, &t2, &t3);
        if (burrow__fe_equal(&t1, &t2) != 1 || !is_in_bounds(&t1) ||
            !is_in_bounds(&t2)) {
            testing_t_errorf_v(t, "#%d: failed", i);
            return;
        }
    }
}

static void TestMul64to128(TestingT *t) {
    uint64_t a = 5;
    uint64_t b = 5;
    Edwards25519Uint128 r = burrow__fe_mul64(a, b);
    if (r.lo != 0x19 || r.hi != 0)
        testing_t_errorf_v(t, "lo-range wide mult failed, got %d + %d*(2**64)", r.lo,
                           r.hi);

    a = UINT64_C(18014398509481983); /* 2^54 - 1 */
    b = UINT64_C(18014398509481983); /* 2^54 - 1 */
    r = burrow__fe_mul64(a, b);
    if (r.lo != UINT64_C(0xff80000000000001) || r.hi != UINT64_C(0xfffffffffff))
        testing_t_errorf_v(t, "hi-range wide mult failed, got %d + %d*(2**64)", r.lo,
                           r.hi);

    a = UINT64_C(1125899906842661);
    b = UINT64_C(2097155);
    r = burrow__fe_mul64(a, b);
    r = burrow__fe_add_mul64(r, a, b);
    r = burrow__fe_add_mul64(r, a, b);
    r = burrow__fe_add_mul64(r, a, b);
    r = burrow__fe_add_mul64(r, a, b);
    if (r.lo != UINT64_C(16888498990613035) || r.hi != 640)
        testing_t_errorf_v(t, "wrong answer: %d + %d*(2**64)", r.lo, r.hi);
}

static void TestSetBytesRoundTrip(TestingT *t) {
    Rng r = {2};
    for (int i = 0, n = quick_count(1); i < n; i++) {
        uint8_t in[32], out[32];
        rng_read(&r, in, sizeof in);
        Fe fe = gen_fe(&r);
        burrow__fe_set_bytes(&fe, bs(in, 32), NULL);

        /* Mask the most significant bit as it's ignored by SetBytes. (Now
         * instead of earlier so we check the masking in SetBytes is working.) */
        in[31] &= (1 << 7) - 1;

        burrow__fe_bytes(&fe, out);
        if (memcmp(in, out, 32) != 0 || !is_in_bounds(&fe)) {
            testing_t_errorf_v(t, "failed bytes->FE->bytes round-trip: #%d", i);
            break;
        }
    }

    for (int i = 0, n = quick_count(1); i < n; i++) {
        Fe fe = gen_fe(&r), rr = gen_fe(&r);
        uint8_t b[32];
        burrow__fe_bytes(&fe, b);
        burrow__fe_set_bytes(&rr, bs(b, 32), NULL);

        /* Intentionally not using Equal not to go through Bytes again.
         * Calling reduce because both Generate and SetBytes can produce
         * non-canonical representations. */
        burrow__fe_reduce(&fe);
        burrow__fe_reduce(&rr);
        if (!fe_same(&fe, &rr)) {
            testing_t_errorf_v(t, "failed FE->bytes->FE round-trip: #%d", i);
            break;
        }
    }

    /* Check some fixed vectors from dalek. */
    static const struct {
        Fe fe;
        uint8_t b[32];
    } tests[] = {
        {{358744748052810, 1691584618240980, 977650209285361, 1429865912637724,
          560044844278676},
         {74,  209, 69,  197, 70, 70,  161, 222, 56, 226, 229, 19, 112, 60,  25,  92,
          187, 74,  222, 56,  50, 153, 51,  233, 40, 74,  57,  6,  160, 185, 213, 31}},
        {{84926274344903, 473620666599931, 365590438845504, 1028470286882429,
          2146499180330972},
         {199, 23, 106, 112, 61, 77, 216, 79,  186, 60,  11,  118, 13,  16,  103, 15,
          42,  32, 83,  250, 44, 57, 204, 198, 78,  199, 253, 119, 146, 172, 3,   122}},
    };
    for (int i = 0; i < NELEM(tests); i++) {
        uint8_t b[32];
        burrow__fe_bytes(&tests[i].fe, b);
        Fe fe = fe_from_bytes(tests[i].b);
        if (memcmp(b, tests[i].b, 32) != 0 || burrow__fe_equal(&fe, &tests[i].fe) != 1)
            testing_t_errorf_v(t, "Failed fixed roundtrip: #%d", i);
    }
}

static void swap_endianness(uint8_t *buf, Int n) {
    for (Int i = 0; i < n / 2; i++) {
        uint8_t c = buf[i];
        buf[i] = buf[n - i - 1];
        buf[n - i - 1] = c;
    }
}

/* fromBig: v = n. The bit length of n must not exceed 256. */
static Fe fe_from_big(const BigInt *n) {
    if (big_int_bit_len(n) > 32 * 8)
        panic_str(BURROW_S("edwards25519: invalid field element input size"));
    uint8_t buf[32];
    big_int_fill_bytes(n, bs(buf, 32));
    swap_endianness(buf, 32);
    return fe_from_bytes(buf);
}

/* toBig. */
static void fe_to_big(BigInt *z, const Fe *v) {
    uint8_t buf[32];
    burrow__fe_bytes(v, buf);
    swap_endianness(buf, 32);
    big_int_set_bytes(z, bs(buf, 32));
}

static Fe fe_from_decimal(const char *s) {
    BigInt n = {0};
    bool ok = false;
    big_int_set_string(&n, str_from_cstr(s), 10, &ok);
    if (!ok)
        panic_str(BURROW_S("not a valid decimal"));
    Fe v = fe_from_big(&n);
    big_int_free(&n);
    return v;
}

static void TestBytesBigEquivalence(TestingT *t) {
    Rng r = {3};
    BigInt b = {0}, b1 = {0};
    for (int i = 0, n = quick_count(1); i < n; i++) {
        uint8_t in[32], buf[32], feb[32];
        rng_read(&r, in, sizeof in);
        Fe fe = gen_fe(&r);
        burrow__fe_set_bytes(&fe, bs(in, 32), NULL);

        in[31] &= (1 << 7) - 1; /* mask the most significant bit */
        swap_endianness(in, 32);
        big_int_set_bytes(&b, bs(in, 32));
        Fe fe1 = fe_from_big(&b);

        bool ok = fe_same(&fe, &fe1);
        if (ok) {
            fe_to_big(&b1, &fe1);
            memset(buf, 0, sizeof buf);
            big_int_fill_bytes(&b1, bs(buf, 32));
            swap_endianness(buf, 32);
            burrow__fe_bytes(&fe, feb);
            ok = memcmp(feb, buf, 32) == 0 && is_in_bounds(&fe) && is_in_bounds(&fe1);
        }
        if (!ok) {
            testing_t_errorf_v(t, "#%d: failed", i);
            break;
        }
    }
    big_int_free(&b);
    big_int_free(&b1);
}

static void TestDecimalConstants(TestingT *t) {
    Fe exp = fe_from_decimal("196811613767075059568070793049885420154460665159238901627"
                             "44021073123829784752");
    if (burrow__fe_equal(&burrow__fe_sqrt_m1, &exp) != 1)
        testing_t_errorf_v(t, "sqrtM1 is wrong");
    /* Go leaves this one commented out, because d is in the parent package and
     * the field test cannot see it. Here it can. */
    exp = fe_from_decimal("370957059346694393431380835087545651895421138798432190163887"
                          "85533085940283555");
    if (burrow__fe_equal(&burrow__ge_d, &exp) != 1)
        testing_t_errorf_v(t, "d is wrong");
}

/* Self-consistency between Multiply and Square. */
static void TestConsistency(TestingT *t) {
    Fe x = {1, 1, 1, 1, 1};
    Fe x2, x2sq;

    burrow__fe_multiply(&x2, &x, &x);
    burrow__fe_square(&x2sq, &x);
    if (!fe_same(&x2, &x2sq))
        testing_t_fatalf_v(t, "all ones failed");

    uint8_t bytes[32];
    Error err = BURROW_NO_ERROR;
    crypto_rand_read(bs(bytes, 32), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    x = fe_from_bytes(bytes);

    burrow__fe_multiply(&x2, &x, &x);
    burrow__fe_square(&x2sq, &x);
    if (!fe_same(&x2, &x2sq))
        testing_t_fatalf_v(t, "all ones failed");
}

static void TestEqual(TestingT *t) {
    Fe x = {1, 1, 1, 1, 1};
    Fe y = {5, 4, 3, 2, 1};

    if (burrow__fe_equal(&x, &x) != 1)
        testing_t_errorf_v(t, "wrong about equality");
    if (burrow__fe_equal(&x, &y) != 0)
        testing_t_errorf_v(t, "wrong about inequality");
}

static void TestInvert(TestingT *t) {
    Fe x = {1, 1, 1, 1, 1};
    Fe one = {1, 0, 0, 0, 0};
    Fe xinv, r;

    burrow__fe_invert(&xinv, &x);
    burrow__fe_multiply(&r, &x, &xinv);
    burrow__fe_reduce(&r);
    if (!fe_same(&one, &r))
        testing_t_errorf_v(t, "inversion identity failed");

    uint8_t bytes[32];
    Error err = BURROW_NO_ERROR;
    crypto_rand_read(bs(bytes, 32), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    x = fe_from_bytes(bytes);

    burrow__fe_invert(&xinv, &x);
    burrow__fe_multiply(&r, &x, &xinv);
    burrow__fe_reduce(&r);
    if (!fe_same(&one, &r))
        testing_t_errorf_v(t, "random inversion identity failed");

    Fe zero = {0, 0, 0, 0, 0};
    burrow__fe_set(&x, &zero);
    if (burrow__fe_invert(&xinv, &x) != &xinv)
        testing_t_errorf_v(t, "inverting zero did not return the receiver");
    else if (burrow__fe_equal(&xinv, &zero) != 1)
        testing_t_errorf_v(t, "inverting zero did not return zero");
}

static void TestSelectSwap(TestingT *t) {
    Fe a = {358744748052810, 1691584618240980, 977650209285361, 1429865912637724,
            560044844278676};
    Fe b = {84926274344903, 473620666599931, 365590438845504, 1028470286882429,
            2146499180330972};
    Fe c, d;

    burrow__fe_select(&c, &a, &b, 1);
    burrow__fe_select(&d, &a, &b, 0);
    if (burrow__fe_equal(&c, &a) != 1 || burrow__fe_equal(&d, &b) != 1)
        testing_t_errorf_v(t, "Select failed");

    burrow__fe_swap(&c, &d, 0);
    if (burrow__fe_equal(&c, &a) != 1 || burrow__fe_equal(&d, &b) != 1)
        testing_t_errorf_v(t, "Swap failed");

    burrow__fe_swap(&c, &d, 1);
    if (burrow__fe_equal(&c, &b) != 1 || burrow__fe_equal(&d, &a) != 1)
        testing_t_errorf_v(t, "Swap failed");
}

static void TestMult32(TestingT *t) {
    Rng r = {4};
    for (int i = 0, n = quick_count(1024); i < n; i++) {
        Fe x = gen_fe(&r);
        uint32_t y = (uint32_t)rng_u64(&r);
        Fe t1, t2, ty = {0, 0, 0, 0, 0};
        for (int j = 0; j < 100; j++)
            burrow__fe_mult32(&t1, &x, y);
        ty.l0 = y;
        for (int j = 0; j < 100; j++)
            burrow__fe_multiply(&t2, &x, &ty);
        if (burrow__fe_equal(&t1, &t2) != 1 || !is_in_bounds(&t1) ||
            !is_in_bounds(&t2)) {
            testing_t_errorf_v(t, "#%d: failed", i);
            return;
        }
    }
}

static void TestSqrtRatio(TestingT *t) {
    /* From draft-irtf-cfrg-ristretto255-decaf448-00, Appendix A.4. */
    static const struct {
        const char *u, *v;
        int was_square;
        const char *r;
    } tests[] = {
        /* If u is 0, the function is defined to return (0, TRUE), even if v
         * is zero. Note that where used in this package, the denominator v
         * is never zero. */
        {"0000000000000000000000000000000000000000000000000000000000000000",
         "0000000000000000000000000000000000000000000000000000000000000000", 1,
         "0000000000000000000000000000000000000000000000000000000000000000"},
        /* 0/1 == 0² */
        {"0000000000000000000000000000000000000000000000000000000000000000",
         "0100000000000000000000000000000000000000000000000000000000000000", 1,
         "0000000000000000000000000000000000000000000000000000000000000000"},
        /* If u is non-zero and v is zero, defined to return (0, FALSE). */
        {"0100000000000000000000000000000000000000000000000000000000000000",
         "0000000000000000000000000000000000000000000000000000000000000000", 0,
         "0000000000000000000000000000000000000000000000000000000000000000"},
        /* 2/1 is not square in this field. */
        {"0200000000000000000000000000000000000000000000000000000000000000",
         "0100000000000000000000000000000000000000000000000000000000000000", 0,
         "3c5ff1b5d8e4113b871bd052f9e7bcd0582804c266ffb2d4f4203eb07fdb7c54"},
        /* 4/1 == 2² */
        {"0400000000000000000000000000000000000000000000000000000000000000",
         "0100000000000000000000000000000000000000000000000000000000000000", 1,
         "0200000000000000000000000000000000000000000000000000000000000000"},
        /* 1/4 == (2⁻¹)² == (2^(p-2))² per Euler's theorem */
        {"0100000000000000000000000000000000000000000000000000000000000000",
         "0400000000000000000000000000000000000000000000000000000000000000", 1,
         "f6ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff3f"},
    };
    for (int i = 0; i < NELEM(tests); i++) {
        uint8_t b[32];
        unhex(tests[i].u, b);
        Fe u = fe_from_bytes(b);
        unhex(tests[i].v, b);
        Fe v = fe_from_bytes(b);
        unhex(tests[i].r, b);
        Fe want = fe_from_bytes(b);
        Fe got;
        int was_square = -1;
        burrow__fe_sqrt_ratio(&got, &u, &v, &was_square);
        if (burrow__fe_equal(&got, &want) == 0 || was_square != tests[i].was_square)
            testing_t_errorf_v(t, "%d: got wasSquare %d, want %d", i, was_square,
                               tests[i].was_square);
    }
}

static void TestSquareN(TestingT *t) {
    static const int ns[] = {1, 2, 5, 10, 15, 50, 120};
    Rng r = {5};
    for (int i = 0, n = quick_count(1024); i < n; i++) {
        Fe x = gen_fe(&r);
        for (int k = 0; k < NELEM(ns); k++) {
            Fe got, want;
            burrow__fe_square_n(&got, &x, ns[k]);
            burrow__fe_set(&want, &x);
            for (int j = 0; j < ns[k]; j++)
                burrow__fe_square(&want, &want);
            if (burrow__fe_equal(&got, &want) != 1) {
                testing_t_errorf_v(t, "SquareN(%d) mismatch", ns[k]);
                return;
            }
            if (!is_in_bounds(&got)) {
                testing_t_errorf_v(t, "SquareN(%d) out of bounds", ns[k]);
                return;
            }
        }
    }
}

static void TestFeMul(TestingT *t) {
    Rng r = {6};
    for (int i = 0, n = quick_count(1024); i < n; i++) {
        Fe a = gen_fe(&r), b = gen_fe(&r);
        Fe a1 = a, a2 = a, b1 = b, b2 = b;

        burrow__fe_mul_generic(&a1, &a1, &b1);
        burrow__fe_multiply(&a2, &a2, &b2);

        if (!fe_same(&a1, &a2) || !is_in_bounds(&a2) || !fe_same(&b1, &b2) ||
            !is_in_bounds(&b2)) {
            testing_t_errorf_v(t, "#%d: failed", i);
            return;
        }
    }
}

typedef Fe *(*FeOneArg)(Fe *v, const Fe *x);
typedef Fe *(*FeTwoArgs)(Fe *v, const Fe *x, const Fe *y);

static bool fe_check_aliasing_one_arg(FeOneArg f, Fe v, Fe x) {
    Fe x1 = x, v1 = x;

    /* Calculate a reference f(x) without aliasing. Go's check here is
     * out != &v && isInBounds(out), which only fails for the wrong pointer. */
    Fe *out = f(&v, &x);
    if (out != &v && is_in_bounds(out))
        return false;

    /* Test aliasing the argument and the receiver. */
    if (f(&v1, &v1) != &v1 || !fe_same(&v1, &v))
        return false;

    /* Ensure the arguments was not modified. */
    return fe_same(&x, &x1);
}

static bool fe_check_aliasing_two_args(FeTwoArgs f, Fe v, Fe x, Fe y) {
    Fe x1 = x, y1 = y, v1;

    /* Calculate a reference f(x, y) without aliasing. */
    Fe *out = f(&v, &x, &y);
    if (out != &v && is_in_bounds(out))
        return false;

    /* Test aliasing the first argument and the receiver. */
    v1 = x;
    if (f(&v1, &v1, &y) != &v1 || !fe_same(&v1, &v))
        return false;
    /* Test aliasing the second argument and the receiver. */
    v1 = y;
    if (f(&v1, &x, &v1) != &v1 || !fe_same(&v1, &v))
        return false;

    /* Calculate a reference f(x, x) without aliasing. */
    if (f(&v, &x, &x) != &v)
        return false;

    /* Test aliasing the first argument and the receiver. */
    v1 = x;
    if (f(&v1, &v1, &x) != &v1 || !fe_same(&v1, &v))
        return false;
    /* Test aliasing the second argument and the receiver. */
    v1 = x;
    if (f(&v1, &x, &v1) != &v1 || !fe_same(&v1, &v))
        return false;
    /* Test aliasing both arguments and the receiver. */
    v1 = x;
    if (f(&v1, &v1, &v1) != &v1 || !fe_same(&v1, &v))
        return false;

    /* Ensure the arguments were not modified. */
    return fe_same(&x, &x1) && fe_same(&y, &y1);
}

static Fe *fe_square_n10(Fe *v, const Fe *x) {
    return burrow__fe_square_n(v, x, 10);
}

static Fe *fe_mult32_max(Fe *v, const Fe *x) {
    return burrow__fe_mult32(v, x, 0xffffffff);
}

static Fe *fe_sqrt_ratio_r(Fe *v, const Fe *x, const Fe *y) {
    int was_square;
    return burrow__fe_sqrt_ratio(v, x, y, &was_square);
}

static Fe *fe_select0(Fe *v, const Fe *x, const Fe *y) {
    return burrow__fe_select(v, x, y, 0);
}

static Fe *fe_select1(Fe *v, const Fe *x, const Fe *y) {
    return burrow__fe_select(v, x, y, 1);
}

/* TestAliasing checks that receivers and arguments can alias each other
 * without leading to incorrect results. That is, it ensures that it's safe to
 * write burrow__fe_invert(v, v) or burrow__fe_add(v, v, v) without any of the
 * inputs getting clobbered by the output being written. */
static void TestAliasing(TestingT *t) {
    static const struct {
        const char *name;
        FeOneArg one_arg;
        FeTwoArgs two_args;
    } targets[] = {
        {"Absolute", burrow__fe_absolute, NULL}, {"Invert", burrow__fe_invert, NULL},
        {"Negate", burrow__fe_negate, NULL},     {"Set", burrow__fe_set, NULL},
        {"Square", burrow__fe_square, NULL},     {"SquareN", fe_square_n10, NULL},
        {"Pow22523", burrow__fe_pow22523, NULL}, {"Mult32", fe_mult32_max, NULL},
        {"Multiply", NULL, burrow__fe_multiply}, {"Add", NULL, burrow__fe_add},
        {"Subtract", NULL, burrow__fe_subtract}, {"SqrtRatio", NULL, fe_sqrt_ratio_r},
        {"Select0", NULL, fe_select0},           {"Select1", NULL, fe_select1},
    };
    Rng r = {7};
    for (int k = 0; k < NELEM(targets); k++) {
        for (int i = 0, n = quick_count(256); i < n; i++) {
            bool ok;
            if (targets[k].one_arg != NULL) {
                Fe v = gen_fe(&r), x = gen_fe(&r);
                ok = fe_check_aliasing_one_arg(targets[k].one_arg, v, x);
            } else {
                Fe v = gen_fe(&r), x = gen_fe(&r), y = gen_fe(&r);
                ok = fe_check_aliasing_two_args(targets[k].two_args, v, x, y);
            }
            if (!ok) {
                testing_t_errorf_v(t, "%s: #%d: failed", targets[k].name, i);
                break;
            }
        }
    }
}

/* ------------------------------------------------------------- scalar */

static const uint8_t sc_one_bytes[32] = {1};

static Sc sc_one(void) {
    Sc s;
    burrow__sc_set_canonical_bytes(&s, bs(sc_one_bytes, 32), NULL);
    return s;
}

static Sc sc_minus_one(void) {
    Sc zero = {{0, 0, 0, 0}}, one = sc_one(), s;
    burrow__sc_subtract(&s, &zero, &one);
    return s;
}

static bool sc_same(const Sc *a, const Sc *b) {
    return memcmp(a, b, sizeof *a) == 0;
}

static bool sc_is_reduced(const Sc *s) {
    uint8_t b[32];
    burrow__sc_bytes(s, b);
    return burrow__sc_is_reduced(bs(b, 32));
}

/* Scalar.Generate: a valid scalar, reduced modulo l, weighted towards high,
 * low and edge values. Go puts the bytes into the Montgomery domain itself;
 * SetCanonicalBytes does the same, and every value here is below l. */
static Sc gen_sc(Rng *r) {
    uint8_t s[32];
    memset(s, 0, sizeof s);
    int dice_roll = rng_intn(r, 100);
    if (dice_roll == 0) {
    } else if (dice_roll == 1) {
        memcpy(s, sc_one_bytes, 32);
    } else if (dice_roll == 2) {
        Sc m = sc_minus_one();
        burrow__sc_bytes(&m, s);
    } else if (dice_roll < 5) {
        /* A low scalar in [0, 2^125). */
        rng_read(r, s, 16);
        s[15] &= (1 << 5) - 1;
    } else if (dice_roll < 10) {
        /* A high scalar in [2^252, 2^252 + 2^124). */
        s[31] = 1 << 4;
        rng_read(r, s, 16);
        s[15] &= (1 << 4) - 1;
    } else {
        /* A valid scalar in [0, l) by returning [0, 2^252) which has a
         * negligibly different distribution (the former has a 2^-127.6 chance
         * of being out of the latter range). */
        rng_read(r, s, 32);
        s[31] &= (1 << 4) - 1;
    }
    Sc v;
    if (burrow__sc_set_canonical_bytes(&v, bs(s, 32), NULL) == NULL)
        panic_str(BURROW_S("generated a non-canonical scalar"));
    return v;
}

static void TestScalarGenerate(TestingT *t) {
    Rng r = {8};
    for (int i = 0, n = quick_count(1024); i < n; i++) {
        Sc sc = gen_sc(&r);
        if (!sc_is_reduced(&sc)) {
            testing_t_errorf_v(t, "generated unreduced scalar: #%d", i);
            return;
        }
    }
}

static void expect_reject(TestingT *t, const uint8_t b[32]) {
    Sc one = sc_one(), s = one;
    Error err = BURROW_NO_ERROR;
    Sc *out = burrow__sc_set_canonical_bytes(&s, bs(b, 32), &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "SetCanonicalBytes worked on a non-canonical value");
    else if (!sc_same(&s, &one))
        testing_t_errorf_v(t, "SetCanonicalBytes modified its receiver");
    else if (out != NULL)
        testing_t_errorf_v(t, "SetCanonicalBytes did not return nil with an error");
}

static void TestScalarSetCanonicalBytes(TestingT *t) {
    Rng r = {9};
    for (int i = 0, n = quick_count(1024); i < n; i++) {
        uint8_t in[32], repr[32];
        rng_read(&r, in, sizeof in);
        Sc sc = gen_sc(&r);
        /* Mask out top 4 bits to guarantee value falls in [0, l). */
        in[31] &= (1 << 4) - 1;
        Error err = BURROW_NO_ERROR;
        burrow__sc_set_canonical_bytes(&sc, bs(in, 32), &err);
        bool ok = BURROW_OK(err);
        if (ok) {
            burrow__sc_bytes(&sc, repr);
            ok = memcmp(in, repr, 32) == 0 && burrow__sc_is_reduced(bs(repr, 32));
        }
        if (!ok) {
            testing_t_errorf_v(t, "failed bytes->scalar->bytes round-trip: #%d", i);
            break;
        }
    }

    for (int i = 0, n = quick_count(1024); i < n; i++) {
        Sc sc1 = gen_sc(&r), sc2 = gen_sc(&r);
        uint8_t b[32];
        burrow__sc_bytes(&sc1, b);
        Error err = BURROW_NO_ERROR;
        burrow__sc_set_canonical_bytes(&sc2, bs(b, 32), &err);
        if (BURROW_FAILED(err) || !sc_same(&sc1, &sc2)) {
            testing_t_errorf_v(t, "failed scalar->bytes->scalar round-trip: #%d", i);
            break;
        }
    }

    Sc m = sc_minus_one();
    uint8_t b[32];
    burrow__sc_bytes(&m, b);
    b[0] += 1;
    expect_reject(t, b);

    burrow__sc_bytes(&m, b);
    b[31] += 1;
    expect_reject(t, b);

    burrow__sc_bytes(&m, b);
    b[31] |= 0x80;
    expect_reject(t, b);
}

static void big_from_le(BigInt *z, const uint8_t *b, Int n) {
    uint8_t bb[64];
    for (Int i = 0; i < n; i++)
        bb[i] = b[n - i - 1];
    big_int_set_bytes(z, bs(bb, n));
}

static void TestScalarSetUniformBytes(TestingT *t) {
    BigInt mod = {0}, one = {0}, sc_big = {0}, in_big = {0};
    big_int_set_string(&mod, BURROW_S("27742317777372353535851937790883648493"), 10,
                       NULL);
    big_int_set_int64(&one, 1);
    big_int_add(&mod, &mod, big_int_lsh(&one, &one, 252));
    Rng r = {10};
    for (int i = 0, n = quick_count(1024); i < n; i++) {
        uint8_t in[64], repr[32];
        rng_read(&r, in, sizeof in);
        Sc sc = gen_sc(&r);
        burrow__sc_set_uniform_bytes(&sc, bs(in, 64), NULL);
        burrow__sc_bytes(&sc, repr);
        bool ok = burrow__sc_is_reduced(bs(repr, 32));
        if (ok) {
            big_from_le(&sc_big, repr, 32);
            big_from_le(&in_big, in, 64);
            ok = big_int_cmp(big_int_mod(&in_big, &in_big, &mod), &sc_big) == 0;
        }
        if (!ok) {
            testing_t_errorf_v(t, "#%d: failed", i);
            break;
        }
    }
    big_int_free(&mod);
    big_int_free(&one);
    big_int_free(&sc_big);
    big_int_free(&in_big);
}

static void TestScalarSetBytesWithClamping(TestingT *t) {
    /* Generated with libsodium.js 1.0.18 crypto_scalarmult_ed25519_base. */
    static const struct {
        const char *name, *in, *want;
    } tests[] = {
        {"random", "633d368491364dc9cd4c1bf891b1d59460face1644813240a313e61f2c88216e",
         "1d87a9026fd0126a5736fe1628c95dd419172b5b618457e041c9c861b2494a94"},
        {"zero", "0000000000000000000000000000000000000000000000000000000000000000",
         "693e47972caf527c7883ad1b39822f026f47db2ab0e1919955b8993aa04411d1"},
        {"one", "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
         "12e9a68b73fd5aacdbcaf3e88c46fea6ebedb1aa84eed1842f07f8edab65e3a7"},
    };
    for (int i = 0; i < NELEM(tests); i++) {
        uint8_t in[32], out[32];
        char got[65];
        unhex(tests[i].in, in);
        Sc s;
        burrow__sc_set_bytes_with_clamping(&s, bs(in, 32), NULL);
        Ge p;
        burrow__ge_scalar_base_mult(&p, &s);
        burrow__ge_bytes(&p, out);
        tohex(out, 32, got);
        if (strcmp(got, tests[i].want) != 0)
            testing_t_errorf_v(t, "%s: got %q, want %q", tests[i].name,
                               (const char *)got, tests[i].want);
    }
}

static void TestScalarMultiplyDistributesOverAdd(TestingT *t) {
    Rng r = {11};
    for (int i = 0, n = quick_count(1024); i < n; i++) {
        Sc x = gen_sc(&r), y = gen_sc(&r), z = gen_sc(&r);
        /* t1 = (x+y)*z */
        Sc t1, t2, t3;
        burrow__sc_add(&t1, &x, &y);
        burrow__sc_multiply(&t1, &t1, &z);
        /* t2 = x*z + y*z */
        burrow__sc_multiply(&t2, &x, &z);
        burrow__sc_multiply(&t3, &y, &z);
        burrow__sc_add(&t2, &t2, &t3);
        if (!sc_same(&t1, &t2) || !sc_is_reduced(&t1) || !sc_is_reduced(&t2)) {
            testing_t_errorf_v(t, "#%d: failed", i);
            return;
        }
    }
}

static void TestScalarAddLikeSubNeg(TestingT *t) {
    Rng r = {12};
    for (int i = 0, n = quick_count(1024); i < n; i++) {
        Sc x = gen_sc(&r), y = gen_sc(&r);
        /* t1 = x - y */
        Sc t1, t2;
        burrow__sc_subtract(&t1, &x, &y);
        /* t2 = -y + x */
        burrow__sc_negate(&t2, &y);
        burrow__sc_add(&t2, &t2, &x);
        if (!sc_same(&t1, &t2) || !sc_is_reduced(&t1)) {
            testing_t_errorf_v(t, "#%d: failed", i);
            return;
        }
    }
}

static void TestScalarNonAdjacentForm(TestingT *t) {
    static const uint8_t in[32] = {
        0x1a, 0x0e, 0x97, 0x8a, 0x90, 0xf6, 0x62, 0x2d, 0x37, 0x47, 0x02,
        0x3f, 0x8a, 0xd8, 0x26, 0x4d, 0xa7, 0x58, 0xaa, 0x1b, 0x88, 0xe0,
        0x40, 0xd1, 0x58, 0x9e, 0x7b, 0x7f, 0x23, 0x76, 0xef, 0x09,
    };
    static const int8_t expected_naf[256] = {
        0,  13,  0, 0,   0,  0,  0,  0,  0,  7,   0,  0,   0, 0,  0, 0,  -9, 0, 0,  0,
        0,  -11, 0, 0,   0,  0,  3,  0,  0,  0,   0,  1,   0, 0,  0, 0,  9,  0, 0,  0,
        0,  -5,  0, 0,   0,  0,  0,  0,  3,  0,   0,  0,   0, 11, 0, 0,  0,  0, 11, 0,
        0,  0,   0, 0,   -9, 0,  0,  0,  0,  0,   -3, 0,   0, 0,  0, 9,  0,  0, 0,  0,
        0,  1,   0, 0,   0,  0,  0,  0,  -1, 0,   0,  0,   0, 0,  9, 0,  0,  0, 0,  -15,
        0,  0,   0, 0,   -7, 0,  0,  0,  0,  -9,  0,  0,   0, 0,  0, 5,  0,  0, 0,  0,
        13, 0,   0, 0,   0,  0,  -3, 0,  0,  0,   0,  -11, 0, 0,  0, 0,  -7, 0, 0,  0,
        0,  -13, 0, 0,   0,  0,  11, 0,  0,  0,   0,  -9,  0, 0,  0, 0,  0,  1, 0,  0,
        0,  0,   0, -15, 0,  0,  0,  0,  1,  0,   0,  0,   0, 7,  0, 0,  0,  0, 0,  0,
        0,  0,   5, 0,   0,  0,  0,  0,  13, 0,   0,  0,   0, 0,  0, 11, 0,  0, 0,  0,
        0,  15,  0, 0,   0,  0,  0,  -9, 0,  0,   0,  0,   0, 0,  0, -1, 0,  0, 0,  0,
        0,  0,   0, 7,   0,  0,  0,  0,  0,  -15, 0,  0,   0, 0,  0, 15, 0,  0, 0,  0,
        15, 0,   0, 0,   0,  15, 0,  0,  0,  0,   0,  1,   0, 0,  0, 0,
    };
    Sc s;
    burrow__sc_set_canonical_bytes(&s, bs(in, 32), NULL);
    int8_t s_naf[256];
    burrow__sc_non_adjacent_form(&s, 5, s_naf);
    for (int i = 0; i < 256; i++) {
        if (expected_naf[i] != s_naf[i])
            testing_t_errorf_v(t, "Wrong digit at position %d, got %d, expected %d", i,
                               s_naf[i], expected_naf[i]);
    }
}

static void TestScalarEqual(TestingT *t) {
    Sc one = sc_one(), minus_one = sc_minus_one();
    if (burrow__sc_equal(&one, &minus_one) == 1)
        testing_t_errorf_v(t, "scOne.Equal(&scMinusOne) is true");
    if (burrow__sc_equal(&minus_one, &minus_one) == 0)
        testing_t_errorf_v(t, "scMinusOne.Equal(&scMinusOne) is false");
}

typedef Sc *(*ScOneArg)(Sc *v, const Sc *x);
typedef Sc *(*ScTwoArgs)(Sc *v, const Sc *x, const Sc *y);

static bool sc_check_aliasing_one_arg(ScOneArg f, Sc v, Sc x) {
    Sc x1 = x, v1 = x;

    /* Calculate a reference f(x) without aliasing. */
    Sc *out = f(&v, &x);
    if (out != &v || !sc_is_reduced(out))
        return false;

    /* Test aliasing the argument and the receiver. */
    out = f(&v1, &v1);
    if (out != &v1 || !sc_same(&v1, &v) || !sc_is_reduced(out))
        return false;

    /* Ensure the arguments was not modified. */
    return sc_same(&x, &x1);
}

static bool sc_check_aliasing_two_args(ScTwoArgs f, Sc v, Sc x, Sc y) {
    Sc x1 = x, y1 = y, v1;
    Sc *out;

    /* Calculate a reference f(x, y) without aliasing. */
    out = f(&v, &x, &y);
    if (out != &v || !sc_is_reduced(out))
        return false;

    /* Test aliasing the first argument and the receiver. */
    v1 = x;
    out = f(&v1, &v1, &y);
    if (out != &v1 || !sc_same(&v1, &v) || !sc_is_reduced(out))
        return false;
    /* Test aliasing the second argument and the receiver. */
    v1 = y;
    out = f(&v1, &x, &v1);
    if (out != &v1 || !sc_same(&v1, &v) || !sc_is_reduced(out))
        return false;

    /* Calculate a reference f(x, x) without aliasing. */
    out = f(&v, &x, &x);
    if (out != &v || !sc_is_reduced(out))
        return false;

    /* Test aliasing the first argument and the receiver. */
    v1 = x;
    out = f(&v1, &v1, &x);
    if (out != &v1 || !sc_same(&v1, &v) || !sc_is_reduced(out))
        return false;
    /* Test aliasing the second argument and the receiver. */
    v1 = x;
    out = f(&v1, &x, &v1);
    if (out != &v1 || !sc_same(&v1, &v) || !sc_is_reduced(out))
        return false;
    /* Test aliasing both arguments and the receiver. */
    v1 = x;
    out = f(&v1, &v1, &v1);
    if (out != &v1 || !sc_same(&v1, &v) || !sc_is_reduced(out))
        return false;

    /* Ensure the arguments were not modified. */
    return sc_same(&x, &x1) && sc_same(&y, &y1);
}

/* The fixed argument of the three MultiplyAdd cases. A function pointer has
 * nowhere else to carry it. */
typedef struct ScFixed {
    Sc fixed;
} ScFixed;

static ScFixed *sc_fixed_slot(void) {
    static BURROW_THREAD_LOCAL ScFixed slot;
    return &slot;
}

static Sc *sc_multiply_add1(Sc *v, const Sc *x, const Sc *y) {
    return burrow__sc_multiply_add(v, &sc_fixed_slot()->fixed, x, y);
}

static Sc *sc_multiply_add2(Sc *v, const Sc *x, const Sc *y) {
    return burrow__sc_multiply_add(v, x, &sc_fixed_slot()->fixed, y);
}

static Sc *sc_multiply_add3(Sc *v, const Sc *x, const Sc *y) {
    return burrow__sc_multiply_add(v, x, y, &sc_fixed_slot()->fixed);
}

static void TestScalarAliasing(TestingT *t) {
    static const struct {
        const char *name;
        ScOneArg one_arg;
        ScTwoArgs two_args;
        bool fixed;
    } targets[] = {
        {"Negate", burrow__sc_negate, NULL, false},
        {"Multiply", NULL, burrow__sc_multiply, false},
        {"Add", NULL, burrow__sc_add, false},
        {"Subtract", NULL, burrow__sc_subtract, false},
        {"MultiplyAdd1", NULL, sc_multiply_add1, true},
        {"MultiplyAdd2", NULL, sc_multiply_add2, true},
        {"MultiplyAdd3", NULL, sc_multiply_add3, true},
    };
    Rng r = {13};
    for (int k = 0; k < NELEM(targets); k++) {
        for (int i = 0, n = quick_count(32); i < n; i++) {
            bool ok;
            if (targets[k].one_arg != NULL) {
                Sc v = gen_sc(&r), x = gen_sc(&r);
                ok = sc_check_aliasing_one_arg(targets[k].one_arg, v, x);
            } else {
                Sc v = gen_sc(&r), x = gen_sc(&r), y = gen_sc(&r);
                if (targets[k].fixed)
                    sc_fixed_slot()->fixed = gen_sc(&r);
                ok = sc_check_aliasing_two_args(targets[k].two_args, v, x, y);
            }
            if (!ok) {
                testing_t_errorf_v(t, "%s: #%d: failed", targets[k].name, i);
                break;
            }
        }
    }
}

/* -------------------------------------------------------------- point */

static void check_on_curve(TestingT *t, const Ge *p) {
    Fe xx, yy, zz, zzzz;
    burrow__fe_square(&xx, &p->x);
    burrow__fe_square(&yy, &p->y);
    burrow__fe_square(&zz, &p->z);
    burrow__fe_square(&zzzz, &zz);
    /* -x² + y² = 1 + dx²y²
     * -(X/Z)² + (Y/Z)² = 1 + d(X/Z)²(Y/Z)²
     * (-X² + Y²)/Z² = 1 + (dX²Y²)/Z⁴
     * (-X² + Y²)*Z² = Z⁴ + dX²Y² */
    Fe lhs, rhs;
    burrow__fe_multiply(&lhs, burrow__fe_subtract(&lhs, &yy, &xx), &zz);
    burrow__fe_add(
        &rhs,
        burrow__fe_multiply(&rhs, burrow__fe_multiply(&rhs, &burrow__ge_d, &xx), &yy),
        &zzzz);
    if (burrow__fe_equal(&lhs, &rhs) != 1)
        testing_t_errorf_v(t, "X, Y, and Z do not specify a point on the curve");
    /* xy = T/Z */
    burrow__fe_multiply(&lhs, &p->x, &p->y);
    burrow__fe_multiply(&rhs, &p->z, &p->t);
    if (burrow__fe_equal(&lhs, &rhs) != 1)
        testing_t_errorf_v(t, "point is not valid");
}

static Ge gen_b(void) {
    Ge b;
    burrow__ge_generator(&b);
    return b;
}

static Ge gen_i(void) {
    Ge i;
    burrow__ge_identity(&i);
    return i;
}

static void TestGenerator(TestingT *t) {
    /* These are the coordinates of B from RFC 8032, Section 5.1, converted to
     * little endian hex. */
    const char *x = "1ad5258f602d56c9b2a7259560c72c695cdcd6fd31e2a4c0fe536ecdd3366921";
    const char *y = "5866666666666666666666666666666666666666666666666666666666666666";
    Ge b = gen_b();
    uint8_t buf[32];
    char got[65];
    burrow__fe_bytes(&b.x, buf);
    tohex(buf, 32, got);
    if (strcmp(got, x) != 0)
        testing_t_errorf_v(t, "wrong B.x: got %s, expected %s", (const char *)got, x);
    burrow__fe_bytes(&b.y, buf);
    tohex(buf, 32, got);
    if (strcmp(got, y) != 0)
        testing_t_errorf_v(t, "wrong B.y: got %s, expected %s", (const char *)got, y);
    Fe one;
    burrow__fe_one(&one);
    if (burrow__fe_equal(&b.z, &one) != 1)
        testing_t_errorf_v(t, "wrong B.z, expected 1");
    /* Check that t is correct. */
    check_on_curve(t, &b);
}

static void TestAddSubNegOnBasePoint(TestingT *t) {
    Ge b = gen_b(), i = gen_i();
    Ge check_lhs, check_rhs;

    burrow__ge_add(&check_lhs, &b, &b);
    Edwards25519ProjP2 tmp_p2;
    Edwards25519ProjP1xP1 tmp_p1xp1;
    burrow__ge_p2_from_p3(&tmp_p2, &b);
    burrow__ge_p1xp1_double(&tmp_p1xp1, &tmp_p2);
    burrow__ge_from_p1xp1(&check_rhs, &tmp_p1xp1);
    if (burrow__ge_equal(&check_lhs, &check_rhs) != 1)
        testing_t_errorf_v(t, "B + B != [2]B");
    check_on_curve(t, &check_lhs);
    check_on_curve(t, &check_rhs);

    burrow__ge_subtract(&check_lhs, &b, &b);
    Ge bneg;
    burrow__ge_negate(&bneg, &b);
    burrow__ge_add(&check_rhs, &b, &bneg);
    if (burrow__ge_equal(&check_lhs, &check_rhs) != 1)
        testing_t_errorf_v(t, "B - B != B + (-B)");
    if (burrow__ge_equal(&i, &check_lhs) != 1)
        testing_t_errorf_v(t, "B - B != 0");
    if (burrow__ge_equal(&i, &check_rhs) != 1)
        testing_t_errorf_v(t, "B + (-B) != 0");
    check_on_curve(t, &check_lhs);
    check_on_curve(t, &check_rhs);
    check_on_curve(t, &bneg);
}

static void TestInvalidEncodings(TestingT *t) {
    /* An invalid point, that also happens to have y > p. */
    const char *invalid =
        "efffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f";
    uint8_t buf[32];
    unhex(invalid, buf);
    Ge p = gen_b(), b = gen_b();
    Error err = BURROW_NO_ERROR;
    Ge *out = burrow__ge_set_bytes(&p, bs(buf, 32), &err);
    if (BURROW_OK(err))
        testing_t_errorf_v(t, "expected error for invalid point");
    else if (out != NULL)
        testing_t_errorf_v(t, "SetBytes did not return nil on an invalid encoding");
    else if (burrow__ge_equal(&p, &b) != 1)
        testing_t_errorf_v(t,
                           "the Point was modified while decoding an invalid encoding");
    check_on_curve(t, &p);
}

typedef struct NonCanonicalTest {
    const char *name, *encoding, *canonical;
} NonCanonicalTest;

static const NonCanonicalTest non_canonical_tests[] = {
    /* Points with x = 0 and the sign bit set. With x = 0 the curve equation
     * gives y² = 1, so y = ±1. 1 has two valid encodings. */
    {"y=1,sign-", "0100000000000000000000000000000000000000000000000000000000000080",
     "0100000000000000000000000000000000000000000000000000000000000000"},
    {"y=p+1,sign-", "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "0100000000000000000000000000000000000000000000000000000000000000"},
    {"y=p-1,sign-", "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f"},

    /* Non-canonical y encodings with values 2²⁵⁵-19 (p) to 2²⁵⁵-1 (p+18). */
    {"y=p,sign+", "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
     "0000000000000000000000000000000000000000000000000000000000000000"},
    {"y=p,sign-", "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "0000000000000000000000000000000000000000000000000000000000000080"},
    {"y=p+1,sign+", "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
     "0100000000000000000000000000000000000000000000000000000000000000"},
    /* "y=p+1,sign-" is already tested above. */
    /* p+2 is not a valid y-coordinate. */
    {"y=p+3,sign+", "f0ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
     "0300000000000000000000000000000000000000000000000000000000000000"},
    {"y=p+3,sign-", "f0ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "0300000000000000000000000000000000000000000000000000000000000080"},
    {"y=p+4,sign+", "f1ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
     "0400000000000000000000000000000000000000000000000000000000000000"},
    {"y=p+4,sign-", "f1ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "0400000000000000000000000000000000000000000000000000000000000080"},
    {"y=p+5,sign+", "f2ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
     "0500000000000000000000000000000000000000000000000000000000000000"},
    {"y=p+5,sign-", "f2ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "0500000000000000000000000000000000000000000000000000000000000080"},
    {"y=p+6,sign+", "f3ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
     "0600000000000000000000000000000000000000000000000000000000000000"},
    {"y=p+6,sign-", "f3ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "0600000000000000000000000000000000000000000000000000000000000080"},
    /* p+7 is not a valid y-coordinate. */
    /* p+8 is not a valid y-coordinate. */
    {"y=p+9,sign+", "f6ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
     "0900000000000000000000000000000000000000000000000000000000000000"},
    {"y=p+9,sign-", "f6ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "0900000000000000000000000000000000000000000000000000000000000080"},
    {"y=p+10,sign+", "f7ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
     "0a00000000000000000000000000000000000000000000000000000000000000"},
    {"y=p+10,sign-", "f7ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "0a00000000000000000000000000000000000000000000000000000000000080"},
    /* p+11 is not a valid y-coordinate. */
    /* p+12 is not a valid y-coordinate. */
    /* p+13 is not a valid y-coordinate. */
    {"y=p+14,sign+", "fbffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
     "0e00000000000000000000000000000000000000000000000000000000000000"},
    {"y=p+14,sign-", "fbffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "0e00000000000000000000000000000000000000000000000000000000000080"},
    {"y=p+15,sign+", "fcffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
     "0f00000000000000000000000000000000000000000000000000000000000000"},
    {"y=p+15,sign-", "fcffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "0f00000000000000000000000000000000000000000000000000000000000080"},
    {"y=p+16,sign+", "fdffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
     "1000000000000000000000000000000000000000000000000000000000000000"},
    {"y=p+16,sign-", "fdffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "1000000000000000000000000000000000000000000000000000000000000080"},
    /* p+17 is not a valid y-coordinate. */
    {"y=p+18,sign+", "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
     "1200000000000000000000000000000000000000000000000000000000000000"},
    {"y=p+18,sign-", "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
     "1200000000000000000000000000000000000000000000000000000000000080"},
};

static void non_canonical_run(void *env, TestingT *t) {
    const NonCanonicalTest *tt = env;
    uint8_t buf[32];
    char got[65];
    Error err = BURROW_NO_ERROR;
    Ge p1, p2;
    unhex(tt->encoding, buf);
    burrow__ge_set_bytes(&p1, bs(buf, 32), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error decoding non-canonical point: %s",
                           error_text(err));
    unhex(tt->canonical, buf);
    burrow__ge_set_bytes(&p2, bs(buf, 32), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "error decoding canonical point: %s", error_text(err));
    if (burrow__ge_equal(&p1, &p2) != 1)
        testing_t_errorf_v(t, "equivalent points are not equal");
    burrow__ge_bytes(&p1, buf);
    tohex(buf, 32, got);
    if (strcmp(got, tt->canonical) != 0)
        testing_t_errorf_v(t,
                           "re-encoding does not match canonical; got %q, expected %q",
                           (const char *)got, tt->canonical);
    check_on_curve(t, &p1);
    check_on_curve(t, &p2);
}

static void TestNonCanonicalPoints(TestingT *t) {
    for (int i = 0; i < NELEM(non_canonical_tests); i++) {
        testing_t_run(t, str_from_cstr(non_canonical_tests[i].name),
                      BURROW_FN(TestingTFunc, non_canonical_run,
                                (uintptr_t)&non_canonical_tests[i]));
    }
}

/* -------------------------------------------------------- scalar mult */

/* A random scalar generated using dalek. */
static const uint8_t dalek_scalar_bytes[32] = {
    219, 106, 114, 9,   174, 249, 155, 89,  69,  203, 201, 93,  92,  116, 234, 187,
    78,  115, 103, 172, 182, 98,  62,  103, 187, 136, 13,  100, 248, 110, 12,  4};

/* The above, times the edwards25519 basepoint. */
static const uint8_t dalek_scalar_basepoint_bytes[32] = {
    0xf4, 0xef, 0x7c, 0xa,  0x34, 0x55, 0x7b, 0x9f, 0x72, 0x3b, 0xb6,
    0x1e, 0xf9, 0x46, 0x9,  0x91, 0x1c, 0xb9, 0xc0, 0x6c, 0x17, 0x28,
    0x2d, 0x8b, 0x43, 0x2b, 0x5,  0x18, 0x6a, 0x54, 0x3e, 0x48};

static Sc dalek_scalar(void) {
    Sc s;
    burrow__sc_set_canonical_bytes(&s, bs(dalek_scalar_bytes, 32), NULL);
    return s;
}

static Ge dalek_scalar_basepoint(void) {
    Ge p;
    burrow__ge_set_bytes(&p, bs(dalek_scalar_basepoint_bytes, 32), NULL);
    return p;
}

static void TestScalarMultSmallScalars(TestingT *t) {
    Sc z = {{0, 0, 0, 0}};
    Ge b = gen_b(), i = gen_i(), p;
    burrow__ge_scalar_mult(&p, &z, &b);
    if (burrow__ge_equal(&i, &p) != 1)
        testing_t_errorf_v(t, "0*B != 0");
    check_on_curve(t, &p);

    Sc sc_eight = sc_one();
    burrow__ge_scalar_mult(&p, &sc_eight, &b);
    if (burrow__ge_equal(&b, &p) != 1)
        testing_t_errorf_v(t, "1*B != 1");
    check_on_curve(t, &p);
}

static void TestScalarMultVsDalek(TestingT *t) {
    Sc s = dalek_scalar();
    Ge b = gen_b(), want = dalek_scalar_basepoint(), p;
    burrow__ge_scalar_mult(&p, &s, &b);
    if (burrow__ge_equal(&want, &p) != 1)
        testing_t_errorf_v(t, "Scalar mul does not match dalek");
    check_on_curve(t, &p);
}

static void TestBaseMultVsDalek(TestingT *t) {
    Sc s = dalek_scalar();
    Ge want = dalek_scalar_basepoint(), p;
    burrow__ge_scalar_base_mult(&p, &s);
    if (burrow__ge_equal(&want, &p) != 1)
        testing_t_errorf_v(t, "Scalar mul does not match dalek");
    check_on_curve(t, &p);
}

static void TestVarTimeDoubleBaseMultVsDalek(TestingT *t) {
    Sc s = dalek_scalar(), z = {{0, 0, 0, 0}};
    Ge b = gen_b(), want = dalek_scalar_basepoint(), p;
    burrow__ge_var_time_double_scalar_base_mult(&p, &s, &b, &z);
    if (burrow__ge_equal(&want, &p) != 1)
        testing_t_errorf_v(t, "VarTimeDoubleScalarBaseMult fails with b=0");
    check_on_curve(t, &p);
    burrow__ge_var_time_double_scalar_base_mult(&p, &z, &b, &s);
    if (burrow__ge_equal(&want, &p) != 1)
        testing_t_errorf_v(t, "VarTimeDoubleScalarBaseMult fails with a=0");
    check_on_curve(t, &p);
}

static void TestScalarMultDistributesOverAdd(TestingT *t) {
    Rng r = {14};
    Ge b = gen_b();
    for (int i = 0, n = quick_count(32); i < n; i++) {
        Sc x = gen_sc(&r), y = gen_sc(&r), z;
        burrow__sc_add(&z, &x, &y);
        Ge p, q, rr, check;
        burrow__ge_scalar_mult(&p, &x, &b);
        burrow__ge_scalar_mult(&q, &y, &b);
        burrow__ge_scalar_mult(&rr, &z, &b);
        burrow__ge_add(&check, &p, &q);
        check_on_curve(t, &p);
        check_on_curve(t, &q);
        check_on_curve(t, &rr);
        check_on_curve(t, &check);
        if (burrow__ge_equal(&check, &rr) != 1) {
            testing_t_errorf_v(t, "#%d: failed", i);
            return;
        }
    }
}

/* Whether p.ScalarMult and q.ScalarBaseMult give the same, when p and q are
 * originally set to the base point. */
static void TestScalarMultNonIdentityPoint(TestingT *t) {
    Rng r = {15};
    Ge b = gen_b();
    for (int i = 0, n = quick_count(32); i < n; i++) {
        Sc x = gen_sc(&r);
        Ge p, q;
        burrow__ge_set(&p, &b);
        burrow__ge_set(&q, &b);

        burrow__ge_scalar_mult(&p, &x, &b);
        burrow__ge_scalar_base_mult(&q, &x);

        check_on_curve(t, &p);
        check_on_curve(t, &q);
        if (burrow__ge_equal(&p, &q) != 1) {
            testing_t_errorf_v(t, "#%d: failed", i);
            return;
        }
    }
}

static void TestBasepointTableGeneration(TestingT *t) {
    /* The basepoint table is 32 affineLookupTables, corresponding to
     * (16^2i)*B for table i. */
    Edwards25519ProjP1xP1 tmp1;
    Edwards25519ProjP2 tmp2;
    Ge tmp3 = gen_b();
    for (int i = 0; i < 32; i++) {
        /* Build the table. */
        Edwards25519AffineLookupTable table;
        burrow__ge_affine_table_from_p3(&table, &tmp3);
        /* Assert equality with the hardcoded one. */
        if (memcmp(&table, &burrow__ge_basepoint_table[i], sizeof table) != 0)
            testing_t_errorf_v(t, "Basepoint table %d does not match", i);

        /* Set p = (16^2)*p = 256*p = 2^8*p. */
        burrow__ge_p2_from_p3(&tmp2, &tmp3);
        for (int j = 0; j < 7; j++) {
            burrow__ge_p1xp1_double(&tmp1, &tmp2);
            burrow__ge_p2_from_p1xp1(&tmp2, &tmp1);
        }
        burrow__ge_p1xp1_double(&tmp1, &tmp2);
        burrow__ge_from_p1xp1(&tmp3, &tmp1);
        check_on_curve(t, &tmp3);
    }
}

static void TestScalarMultMatchesBaseMult(TestingT *t) {
    Rng r = {16};
    Ge b = gen_b();
    for (int i = 0, n = quick_count(32); i < n; i++) {
        Sc x = gen_sc(&r);
        Ge p, q;
        burrow__ge_scalar_mult(&p, &x, &b);
        burrow__ge_scalar_base_mult(&q, &x);
        check_on_curve(t, &p);
        check_on_curve(t, &q);
        if (burrow__ge_equal(&p, &q) != 1) {
            testing_t_errorf_v(t, "#%d: failed", i);
            return;
        }
    }
}

static void TestBasepointNafTableGeneration(TestingT *t) {
    Edwards25519NafLookupTable8 table;
    Ge b = gen_b();
    burrow__ge_naf8_table_from_p3(&table, &b);
    if (memcmp(&table, &burrow__ge_basepoint_naf_table, sizeof table) != 0)
        testing_t_errorf_v(t, "BasepointNafTable does not match");
}

static void TestVarTimeDoubleBaseMultMatchesBaseMult(TestingT *t) {
    Rng r = {17};
    Ge b = gen_b();
    for (int i = 0, n = quick_count(32); i < n; i++) {
        Sc x = gen_sc(&r), y = gen_sc(&r);
        Ge p, q1, q2, check;

        burrow__ge_var_time_double_scalar_base_mult(&p, &x, &b, &y);

        burrow__ge_scalar_base_mult(&q1, &x);
        burrow__ge_scalar_base_mult(&q2, &y);
        burrow__ge_add(&check, &q1, &q2);

        check_on_curve(t, &p);
        check_on_curve(t, &check);
        check_on_curve(t, &q1);
        check_on_curve(t, &q2);
        if (burrow__ge_equal(&p, &check) != 1) {
            testing_t_errorf_v(t, "#%d: failed", i);
            return;
        }
    }
}

/* ------------------------------------------------------------- tables */

static void TestProjLookupTable(TestingT *t) {
    Edwards25519ProjLookupTable table;
    Ge b = gen_b(), i = gen_i();
    burrow__ge_proj_table_from_p3(&table, &b);

    Edwards25519ProjCached tmp1, tmp2, tmp3;
    burrow__ge_proj_table_select(&table, &tmp1, 6);
    burrow__ge_proj_table_select(&table, &tmp2, -2);
    burrow__ge_proj_table_select(&table, &tmp3, -4);
    /* Expect T1 + T2 + T3 = identity. */

    Edwards25519ProjP1xP1 acc_p1xp1;
    Ge acc_p3 = gen_i();

    burrow__ge_p1xp1_add(&acc_p1xp1, &acc_p3, &tmp1);
    burrow__ge_from_p1xp1(&acc_p3, &acc_p1xp1);
    burrow__ge_p1xp1_add(&acc_p1xp1, &acc_p3, &tmp2);
    burrow__ge_from_p1xp1(&acc_p3, &acc_p1xp1);
    burrow__ge_p1xp1_add(&acc_p1xp1, &acc_p3, &tmp3);
    burrow__ge_from_p1xp1(&acc_p3, &acc_p1xp1);

    if (burrow__ge_equal(&acc_p3, &i) != 1)
        testing_t_errorf_v(t,
                           "Consistency check on ProjLookupTable.SelectInto failed!");
}

static void TestAffineLookupTable(TestingT *t) {
    Edwards25519AffineLookupTable table;
    Ge b = gen_b(), i = gen_i();
    burrow__ge_affine_table_from_p3(&table, &b);

    Edwards25519AffineCached tmp1, tmp2, tmp3;
    burrow__ge_affine_table_select(&table, &tmp1, 3);
    burrow__ge_affine_table_select(&table, &tmp2, -7);
    burrow__ge_affine_table_select(&table, &tmp3, 4);
    /* Expect T1 + T2 + T3 = identity. */

    Edwards25519ProjP1xP1 acc_p1xp1;
    Ge acc_p3 = gen_i();

    burrow__ge_p1xp1_add_affine(&acc_p1xp1, &acc_p3, &tmp1);
    burrow__ge_from_p1xp1(&acc_p3, &acc_p1xp1);
    burrow__ge_p1xp1_add_affine(&acc_p1xp1, &acc_p3, &tmp2);
    burrow__ge_from_p1xp1(&acc_p3, &acc_p1xp1);
    burrow__ge_p1xp1_add_affine(&acc_p1xp1, &acc_p3, &tmp3);
    burrow__ge_from_p1xp1(&acc_p3, &acc_p1xp1);

    if (burrow__ge_equal(&acc_p3, &i) != 1)
        testing_t_errorf_v(t,
                           "Consistency check on ProjLookupTable.SelectInto failed!");
}

static void TestNafLookupTable5(TestingT *t) {
    Edwards25519NafLookupTable5 table;
    Ge b = gen_b();
    burrow__ge_naf5_table_from_p3(&table, &b);

    Edwards25519ProjCached tmp1, tmp2, tmp3, tmp4;
    burrow__ge_naf5_table_select(&table, &tmp1, 9);
    burrow__ge_naf5_table_select(&table, &tmp2, 11);
    burrow__ge_naf5_table_select(&table, &tmp3, 7);
    burrow__ge_naf5_table_select(&table, &tmp4, 13);
    /* Expect T1 + T2 = T3 + T4. */

    Edwards25519ProjP1xP1 acc_p1xp1;
    Ge lhs = gen_i(), rhs = gen_i();

    burrow__ge_p1xp1_add(&acc_p1xp1, &lhs, &tmp1);
    burrow__ge_from_p1xp1(&lhs, &acc_p1xp1);
    burrow__ge_p1xp1_add(&acc_p1xp1, &lhs, &tmp2);
    burrow__ge_from_p1xp1(&lhs, &acc_p1xp1);

    burrow__ge_p1xp1_add(&acc_p1xp1, &rhs, &tmp3);
    burrow__ge_from_p1xp1(&rhs, &acc_p1xp1);
    burrow__ge_p1xp1_add(&acc_p1xp1, &rhs, &tmp4);
    burrow__ge_from_p1xp1(&rhs, &acc_p1xp1);

    if (burrow__ge_equal(&lhs, &rhs) != 1)
        testing_t_errorf_v(t, "Consistency check on nafLookupTable5 failed");
}

static void TestNafLookupTable8(TestingT *t) {
    Edwards25519NafLookupTable8 table;
    Ge b = gen_b();
    burrow__ge_naf8_table_from_p3(&table, &b);

    Edwards25519AffineCached tmp1, tmp2, tmp3, tmp4;
    burrow__ge_naf8_table_select(&table, &tmp1, 49);
    burrow__ge_naf8_table_select(&table, &tmp2, 11);
    burrow__ge_naf8_table_select(&table, &tmp3, 35);
    burrow__ge_naf8_table_select(&table, &tmp4, 25);
    /* Expect T1 + T2 = T3 + T4. */

    Edwards25519ProjP1xP1 acc_p1xp1;
    Ge lhs = gen_i(), rhs = gen_i();

    burrow__ge_p1xp1_add_affine(&acc_p1xp1, &lhs, &tmp1);
    burrow__ge_from_p1xp1(&lhs, &acc_p1xp1);
    burrow__ge_p1xp1_add_affine(&acc_p1xp1, &lhs, &tmp2);
    burrow__ge_from_p1xp1(&lhs, &acc_p1xp1);

    burrow__ge_p1xp1_add_affine(&acc_p1xp1, &rhs, &tmp3);
    burrow__ge_from_p1xp1(&rhs, &acc_p1xp1);
    burrow__ge_p1xp1_add_affine(&acc_p1xp1, &rhs, &tmp4);
    burrow__ge_from_p1xp1(&rhs, &acc_p1xp1);

    if (burrow__ge_equal(&lhs, &rhs) != 1)
        testing_t_errorf_v(t, "Consistency check on nafLookupTable8 failed");
}

/* A zero Point is not a point. Go's methods panic on one, and so do these. */
static void TestUninitializedPoint(TestingT *t) {
    Ge zero, b = gen_b(), out;
    memset(&zero, 0, sizeof zero);
    volatile bool panicked = false;
    BURROW_TRY {
        burrow__ge_add(&out, &zero, &b);
    }
    BURROW_CATCH(r) {
        panicked =
            str_eq(panic_text(r), BURROW_S("edwards25519: use of uninitialized Point"));
    }
    BURROW_TRY_END;
    if (!panicked)
        testing_t_errorf_v(t, "adding a zero Point did not panic");
}

/* --------------------------------------------------------- benchmarks */

static void BenchmarkEncodingDecoding(TestingB *b) {
    Ge p = dalek_scalar_basepoint();
    uint8_t buf[32];
    for (Int i = 0; i < testing_b_n(b); i++) {
        burrow__ge_bytes(&p, buf);
        Error err = BURROW_NO_ERROR;
        burrow__ge_set_bytes(&p, bs(buf, 32), &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
    }
}

static void BenchmarkScalarBaseMult(TestingB *b) {
    Sc s = dalek_scalar();
    Ge p;
    for (Int i = 0; i < testing_b_n(b); i++)
        burrow__ge_scalar_base_mult(&p, &s);
}

static void BenchmarkScalarMult(TestingB *b) {
    Sc s = dalek_scalar();
    Ge bp = gen_b(), p;
    for (Int i = 0; i < testing_b_n(b); i++)
        burrow__ge_scalar_mult(&p, &s, &bp);
}

static void BenchmarkVarTimeDoubleScalarBaseMult(TestingB *b) {
    Sc s = dalek_scalar();
    Ge bp = gen_b(), p;
    for (Int i = 0; i < testing_b_n(b); i++)
        burrow__ge_var_time_double_scalar_base_mult(&p, &s, &bp, &s);
}

static void BenchmarkAdd(TestingB *b) {
    Fe x, y;
    burrow__fe_one(&x);
    burrow__fe_add(&y, &x, &x);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        burrow__fe_add(&x, &x, &y);
}

static void BenchmarkMultiply(TestingB *b) {
    Fe x, y;
    burrow__fe_one(&x);
    burrow__fe_add(&y, &x, &x);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        burrow__fe_multiply(&x, &x, &y);
}

static void BenchmarkSquare(TestingB *b) {
    Fe one, x;
    burrow__fe_one(&one);
    burrow__fe_add(&x, &one, &one);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        burrow__fe_square(&x, &x);
}

static void BenchmarkInvert(TestingB *b) {
    Fe one, x;
    burrow__fe_one(&one);
    burrow__fe_add(&x, &one, &one);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        burrow__fe_invert(&x, &x);
}

static void BenchmarkMult32(TestingB *b) {
    Fe x;
    burrow__fe_one(&x);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        burrow__fe_mult32(&x, &x, 0xaa42aa42);
}

static void BenchmarkBytes(TestingB *b) {
    Fe x;
    uint8_t buf[32];
    burrow__fe_one(&x);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        burrow__fe_bytes(&x, buf);
}

#define TESTS(X)                                                                       \
    X(TestMultiplyDistributesOverAdd)                                                  \
    X(TestMul64to128)                                                                  \
    X(TestSetBytesRoundTrip)                                                           \
    X(TestBytesBigEquivalence)                                                         \
    X(TestDecimalConstants)                                                            \
    X(TestConsistency)                                                                 \
    X(TestEqual)                                                                       \
    X(TestInvert)                                                                      \
    X(TestSelectSwap)                                                                  \
    X(TestMult32)                                                                      \
    X(TestSqrtRatio)                                                                   \
    X(TestSquareN)                                                                     \
    X(TestFeMul)                                                                       \
    X(TestAliasing)                                                                    \
    X(TestScalarGenerate)                                                              \
    X(TestScalarSetCanonicalBytes)                                                     \
    X(TestScalarSetUniformBytes)                                                       \
    X(TestScalarSetBytesWithClamping)                                                  \
    X(TestScalarMultiplyDistributesOverAdd)                                            \
    X(TestScalarAddLikeSubNeg)                                                         \
    X(TestScalarNonAdjacentForm)                                                       \
    X(TestScalarEqual)                                                                 \
    X(TestScalarAliasing)                                                              \
    X(TestGenerator)                                                                   \
    X(TestAddSubNegOnBasePoint)                                                        \
    X(TestInvalidEncodings)                                                            \
    X(TestNonCanonicalPoints)                                                          \
    X(TestScalarMultSmallScalars)                                                      \
    X(TestScalarMultVsDalek)                                                           \
    X(TestBaseMultVsDalek)                                                             \
    X(TestVarTimeDoubleBaseMultVsDalek)                                                \
    X(TestScalarMultDistributesOverAdd)                                                \
    X(TestScalarMultNonIdentityPoint)                                                  \
    X(TestBasepointTableGeneration)                                                    \
    X(TestScalarMultMatchesBaseMult)                                                   \
    X(TestBasepointNafTableGeneration)                                                 \
    X(TestVarTimeDoubleBaseMultMatchesBaseMult)                                        \
    X(TestProjLookupTable)                                                             \
    X(TestAffineLookupTable)                                                           \
    X(TestNafLookupTable5)                                                             \
    X(TestNafLookupTable8)                                                             \
    X(TestUninitializedPoint)                                                          \
    X(BenchmarkEncodingDecoding)                                                       \
    X(BenchmarkScalarBaseMult)                                                         \
    X(BenchmarkScalarMult)                                                             \
    X(BenchmarkVarTimeDoubleScalarBaseMult)                                            \
    X(BenchmarkAdd)                                                                    \
    X(BenchmarkMultiply)                                                               \
    X(BenchmarkSquare)                                                                 \
    X(BenchmarkInvert)                                                                 \
    X(BenchmarkMult32)                                                                 \
    X(BenchmarkBytes)

TESTING_MAIN(TESTS)
