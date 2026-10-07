/* Derived from Go's src/crypto/internal/fips140/bigmod/nat_test.go.
 * Go source: go1.27.1.
 *
 * Go checks the first three properties with testing/quick, which draws its
 * inputs from a math/rand source seeded with the time, and Nat's Generate
 * gives it 50 words each time. Here the same generators run off a fixed seed,
 * so that a failure can be run again, 100 times each as quick does.
 * TestInverse reads testdata/mod_inv_tests.txt in Go, and the file is in
 * mod_inv_tests below as it is there. TestBurrow and TestPanics are burrow's
 * own: Go tests GCDVarTime, DivShortVarTime and the others through
 * crypto/rsa, which is not here yet, and has no test for the panics.
 *
 * Copyright 2021 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/rand.h"
#include "burrow/encoding/hex.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/strings.h"

#include "../src/crypto/bigmod.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/* How many times quick.Check calls the function. */
enum { QUICK_COUNT = 100 };

/* The size testing/quick passes to Generate. */
enum { QUICK_SIZE = 50 };

static Slice bs(const void *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

/* A Nat of the words in w, which it copies, from the heap. */
static BigmodNat nat_of(const Uint *w, Int n) {
    BigmodNat x;
    bigmod_nat_init(&x, NULL);
    bigmod_nat_reset(&x, n);
    if (n > 0)
        memcpy(x.limbs, w, (size_t)n * sizeof(Uint));
    return x;
}

static BigmodNat nat1(Uint w) {
    return nat_of(&w, 1);
}

/* Generate: an even Nat of size words, every word with its low bit clear. */
static BigmodNat generate(Rng *r, Int size) {
    BigmodNat x;
    bigmod_nat_init(&x, NULL);
    bigmod_nat_reset(&x, size);
    for (Int i = 0; i < size; i++)
        x.limbs[i] = (Uint)rng_u64(r) & ~(Uint)1;
    return x;
}

/* setBig: x = n, as many words as n has. */
static BigmodNat *set_big(BigmodNat *x, const BigInt *n) {
    Slice limbs = big_int_bits(n);
    bigmod_nat_reset(x, limbs.len);
    for (Int i = 0; i < limbs.len; i++)
        x->limbs[i] = (Uint)BURROW_AT(BigWord, limbs, i);
    return x;
}

/* asBig: n as a BigInt from a. */
static BigInt *as_big(Alloc *a, const BigmodNat *n) {
    BigInt *z = big_new_int(a, 0);
    return big_int_set_bits(z, slice_from(n->limbs, n->len, n->len, TYPE_UINT));
}

/* String: the words of n in hex, the top one first, from a. */
static Str nat_string(Alloc *a, const BigmodNat *n) {
    Int size = 2 + n->len * (BIGMOD_S * 2 + 1);
    char *p = mem_alloc(a, (size_t)size + 1, 1);
    Int k = 0;
    p[k++] = '{';
    for (Int i = 0; i < n->len; i++) {
        if (i > 0)
            p[k++] = ' ';
        k += snprintf(p + k, (size_t)(size + 1 - k), "%0*llX", BIGMOD_S * 2,
                      (unsigned long long)n->limbs[n->len - 1 - i]);
    }
    p[k++] = '}';
    return (Str){(const Byte *)p, k};
}

/* maxModulus: the biggest modulus that fits in n words. */
static BigmodModulus *max_modulus(Int n) {
    uint8_t b[512];
    memset(b, 0xff, (size_t)(n * BIGMOD_S));
    return bigmod_new_modulus(NULL, bs(b, n * BIGMOD_S), NULL);
}

/* natBytes: n's bytes, padded to its words. */
static Slice nat_bytes(Alloc *a, const BigmodNat *n) {
    BigmodModulus *m = max_modulus(n->len);
    Slice b = bigmod_nat_bytes(n, a, m);
    bigmod_modulus_free(m);
    return b;
}

/* natFromBytes: the Nat of b, with as many words as its value needs. */
static BigmodNat nat_from_bytes(Slice b) {
    BigInt bb = BIG_INT(NULL);
    big_int_set_bytes(&bb, b);
    BigmodNat x;
    bigmod_nat_init(&x, NULL);
    set_big(&x, &bb);
    big_int_free(&bb);
    return x;
}

static BigmodModulus *modulus_from_bytes(Slice b) {
    return bigmod_new_modulus(NULL, b, NULL);
}

/* ------------------------------------------------------------- the tests */

static bool test_mod_add_commutative(BigmodNat *a, BigmodNat *b) {
    BigmodModulus *m = max_modulus(a->len);
    BigmodNat a_plus_b, b_plus_a;
    bigmod_nat_init(&a_plus_b, NULL);
    bigmod_nat_init(&b_plus_a, NULL);
    bigmod_nat_set(&a_plus_b, a);
    bigmod_nat_add(&a_plus_b, b, m);
    bigmod_nat_set(&b_plus_a, b);
    bigmod_nat_add(&b_plus_a, a, m);
    bool ok = bigmod_nat_equal(&a_plus_b, &b_plus_a) == 1;
    bigmod_nat_free(&a_plus_b);
    bigmod_nat_free(&b_plus_a);
    bigmod_modulus_free(m);
    return ok;
}

static void TestModAddCommutative(TestingT *t) {
    Rng r = {1};
    for (int i = 0; i < QUICK_COUNT; i++) {
        BigmodNat a = generate(&r, QUICK_SIZE), b = generate(&r, QUICK_SIZE);
        if (!test_mod_add_commutative(&a, &b))
            testing_t_errorf_v(t, "#%d: failed on input", i);
        bigmod_nat_free(&a);
        bigmod_nat_free(&b);
    }
}

static bool test_mod_sub_then_add_identity(BigmodNat *a, BigmodNat *b) {
    BigmodModulus *m = max_modulus(a->len);
    BigmodNat original;
    bigmod_nat_init(&original, NULL);
    bigmod_nat_set(&original, a);
    bigmod_nat_sub(a, b, m);
    bigmod_nat_add(a, b, m);
    bool ok = bigmod_nat_equal(a, &original) == 1;
    bigmod_nat_free(&original);
    bigmod_modulus_free(m);
    return ok;
}

static void TestModSubThenAddIdentity(TestingT *t) {
    Rng r = {2};
    for (int i = 0; i < QUICK_COUNT; i++) {
        BigmodNat a = generate(&r, QUICK_SIZE), b = generate(&r, QUICK_SIZE);
        if (!test_mod_sub_then_add_identity(&a, &b))
            testing_t_errorf_v(t, "#%d: failed on input", i);
        bigmod_nat_free(&a);
        bigmod_nat_free(&b);
    }
}

static void TestMontgomeryRoundtrip(TestingT *t) {
    Rng r = {3};
    for (int i = 0; i < QUICK_COUNT; i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *al = arena_allocator(&ar);
        BigmodNat a = generate(&r, QUICK_SIZE);
        BigmodNat one;
        bigmod_nat_init(&one, NULL);
        bigmod_nat_reset(&one, a.len);
        one.limbs[0] = 1;
        BigInt *a_plus_one = big_new_int(al, 0);
        big_int_set_bytes(a_plus_one, nat_bytes(al, &a));
        big_int_add(a_plus_one, a_plus_one, big_new_int(al, 1));
        BigmodModulus *m =
            bigmod_new_modulus(NULL, big_int_bytes(a_plus_one, al), NULL);
        BigmodNat monty, a_again;
        bigmod_nat_init(&monty, NULL);
        bigmod_nat_init(&a_again, NULL);
        bigmod_nat_set(&monty, &a);
        bigmod_nat_montgomery_representation(&monty, m);
        bigmod_nat_set(&a_again, &monty);
        bigmod_nat_montgomery_mul(&a_again, &monty, &one, m);
        if (bigmod_nat_equal(&a, &a_again) != 1)
            testing_t_errorf_v(t, "%s != %s", nat_string(al, &a),
                               nat_string(al, &a_again));
        bigmod_nat_free(&a);
        bigmod_nat_free(&one);
        bigmod_nat_free(&monty);
        bigmod_nat_free(&a_again);
        bigmod_modulus_free(m);
        arena_free(&ar);
    }
}

typedef struct ShiftInExample {
    uint8_t m[9], x[9], expected[9];
    Int mlen, xlen, elen;
    uint64_t y;
} ShiftInExample;

static void TestShiftIn(TestingT *t) {
    if (BIGMOD_W != 64) {
        testing_t_skip_v(t, "examples are only valid in 64 bit");
        return;
    }
    static const ShiftInExample examples[] = {
        {{13}, {0}, {2}, 1, 1, 1, UINT64_C(0xFFFFFFFFFFFFFFFF)},
        {{13}, {7}, {10}, 1, 1, 1, UINT64_C(0xFFFFFFFFFFFFFFFF)},
        {{0x06, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d},
         {0},
         {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
         9,
         9,
         9,
         UINT64_C(0xFFFFFFFFFFFFFFFF)},
        {{0x06, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d},
         {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
         {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06},
         9,
         9,
         9,
         0},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < (int)(sizeof examples / sizeof examples[0]); i++) {
        const ShiftInExample *tt = &examples[i];
        BigmodModulus *m = modulus_from_bytes(bs(tt->m, tt->mlen));
        BigmodNat got = nat_from_bytes(bs(tt->x, tt->xlen));
        bigmod_nat_shift_in(bigmod_nat_expand_for(&got, m), (Uint)tt->y, m);
        BigmodNat exp = nat_from_bytes(bs(tt->expected, tt->elen));
        bigmod_nat_expand_for(&exp, m);
        if (bigmod_nat_equal(&got, &exp) != 1)
            testing_t_errorf_v(t, "%d: got %s, expected %s", i, nat_string(a, &got),
                               nat_string(a, &exp));
        bigmod_nat_free(&got);
        bigmod_nat_free(&exp);
        bigmod_modulus_free(m);
    }
    arena_free(&ar);
}

static void TestModulusAndNatSizes(TestingT *t) {
    (void)t;
    /* These are 126 bit (2 * _W on 64-bit architectures) values, serialized
     * as 128 bits worth of bytes. If leading zeroes are stripped, they fit in
     * two limbs, if they are not, they fit in three. This can be a problem
     * because modulus strips leading zeroes and nat does not. */
    static const uint8_t mb[] = {0x3f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    static const uint8_t xb[] = {0x3f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe};
    BigmodModulus *m = modulus_from_bytes(bs(mb, sizeof mb));
    BigmodNat x = nat_from_bytes(bs(xb, sizeof xb));
    bigmod_nat_expand_for(&x, m); /* must not panic for shrinking */
    BigmodNat y;
    bigmod_nat_init(&y, NULL);
    bigmod_nat_set_bytes(&y, bs(xb, sizeof xb), m, NULL);
    bigmod_nat_free(&x);
    bigmod_nat_free(&y);
    bigmod_modulus_free(m);
}

typedef struct SetBytesTest {
    uint8_t m[9], b[9];
    Int mlen, blen;
    bool fail;
} SetBytesTest;

static void TestSetBytes(TestingT *t) {
    static const SetBytesTest tests[] = {
        {{0xff, 0xff}, {0x00, 0x01}, 2, 2, false},
        {{0xff, 0xff}, {0xff, 0xff}, 2, 2, true},
        {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
         {0x00, 0x01},
         9,
         2,
         false},
        {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
         {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe},
         9,
         9,
         false},
        {{0xff, 0xff},
         {0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
         2,
         9,
         true},
        {{0xff, 0xff},
         {0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
         2,
         9,
         true},
        {{0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
         {0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe},
         8,
         8,
         false},
        {{0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
         {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe},
         8,
         8,
         true},
        {{0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
         {0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
         8,
         8,
         true},
        {{0x7f, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
         {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe},
         9,
         9,
         true},
        {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfd},
         {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff},
         9,
         9,
         true},
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < (int)(sizeof tests / sizeof tests[0]); i++) {
        const SetBytesTest *tt = &tests[i];
        BigmodModulus *m = modulus_from_bytes(bs(tt->m, tt->mlen));
        BigmodNat x;
        bigmod_nat_init(&x, NULL);
        Error err = BURROW_NO_ERROR;
        BigmodNat *got = bigmod_nat_set_bytes(&x, bs(tt->b, tt->blen), m, &err);
        if (BURROW_FAILED(err)) {
            if (!tt->fail)
                testing_t_errorf_v(t, "%d: unexpected error: %s", i, error_text(err));
        } else if (tt->fail) {
            testing_t_errorf_v(t, "%d: unexpected success", i);
        } else {
            BigmodNat expected = nat_from_bytes(bs(tt->b, tt->blen));
            bigmod_nat_expand_for(&expected, m);
            if (bigmod_nat_equal(got, &expected) != 1)
                testing_t_errorf_v(t, "%d: got %s, expected %s", i, nat_string(a, got),
                                   nat_string(a, &expected));
            bigmod_nat_free(&expected);
        }
        bigmod_nat_free(&x);
        bigmod_modulus_free(m);
    }

    Rng r = {4};
    for (int i = 0; i < QUICK_COUNT; i++) {
        uint8_t x_bytes[QUICK_SIZE];
        Int n = (Int)(rng_u64(&r) % QUICK_SIZE);
        for (Int j = 0; j < n; j++)
            x_bytes[j] = (uint8_t)rng_u64(&r);
        BigmodModulus *m = max_modulus(n * 8 / BIGMOD_W + 1);
        BigmodNat x;
        bigmod_nat_init(&x, NULL);
        Error err = BURROW_NO_ERROR;
        BigmodNat *got = bigmod_nat_set_bytes(&x, bs(x_bytes, n), m, &err);
        BigmodNat want = nat_from_bytes(bs(x_bytes, n));
        bigmod_nat_expand_for(&want, m);
        if (got == NULL || bigmod_nat_equal(got, &want) != 1)
            testing_t_errorf_v(t, "#%d: failed on input %x", i, bs(x_bytes, n));
        bigmod_nat_free(&x);
        bigmod_nat_free(&want);
        bigmod_modulus_free(m);
    }
    arena_free(&ar);
}

static void TestExpand(TestingT *t) {
    Uint sliced[] = {1, 2, 3, 4};
    Uint one_two[] = {1, 2};
    static const Uint out4[] = {1, 2, 0, 0};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < 3; i++) {
        BigmodNat in;
        Int n = i == 2 ? 2 : 4;
        const Uint *out = i == 2 ? one_two : out4;
        if (i == 1) {
            bigmod_nat_init_buf(&in, sliced, 4);
            in.len = 2;
        } else {
            in = nat_of(one_two, 2);
        }
        BigmodNat *got = bigmod_nat_expand(&in, n);
        BigmodNat want = nat_of(out, n);
        if (got->len != n || bigmod_nat_equal(got, &want) != 1)
            testing_t_errorf_v(t, "%d: got %s, expected %s", i, nat_string(a, got),
                               nat_string(a, &want));
        bigmod_nat_free(&in);
        bigmod_nat_free(&want);
    }
    arena_free(&ar);
}

static void TestMod(TestingT *t) {
    static const uint8_t mb[] = {0x06, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0d};
    static const uint8_t xb[] = {0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    static const uint8_t eb[] = {0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BigmodModulus *m = modulus_from_bytes(bs(mb, sizeof mb));
    BigmodNat x = nat_from_bytes(bs(xb, sizeof xb));
    BigmodNat out;
    bigmod_nat_init(&out, NULL);
    bigmod_nat_mod(&out, &x, m);
    BigmodNat expected = nat_from_bytes(bs(eb, sizeof eb));
    if (bigmod_nat_equal(&out, &expected) != 1)
        testing_t_errorf_v(t, "%s != %s", nat_string(a, &out),
                           nat_string(a, &expected));
    bigmod_nat_free(&x);
    bigmod_nat_free(&out);
    bigmod_nat_free(&expected);
    bigmod_modulus_free(m);
    arena_free(&ar);
}

/* x op= y mod 13, twice, against the two results Go's tests want. */
static void mod13(TestingT *t,
                  BigmodNat *(*op)(BigmodNat *, const BigmodNat *,
                                   const BigmodModulus *),
                  Uint first, Uint second) {
    static const uint8_t thirteen[] = {13};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BigmodModulus *m = modulus_from_bytes(bs(thirteen, 1));
    BigmodNat x = nat1(6), y = nat1(7);
    op(&x, &y, m);
    BigmodNat expected = nat1(first);
    if (bigmod_nat_equal(&x, &expected) != 1)
        testing_t_errorf_v(t, "%s != %s", nat_string(a, &x), nat_string(a, &expected));
    op(&x, &y, m);
    expected.limbs[0] = second;
    if (bigmod_nat_equal(&x, &expected) != 1)
        testing_t_errorf_v(t, "%s != %s", nat_string(a, &x), nat_string(a, &expected));
    bigmod_nat_free(&x);
    bigmod_nat_free(&y);
    bigmod_nat_free(&expected);
    bigmod_modulus_free(m);
    arena_free(&ar);
}

static void TestModSub(TestingT *t) {
    mod13(t, bigmod_nat_sub, 12, 5);
}

static void TestModAdd(TestingT *t) {
    mod13(t, bigmod_nat_add, 0, 7);
}

static void TestExp(TestingT *t) {
    static const uint8_t thirteen[] = {13}, twelve[] = {12};
    BigmodModulus *m = modulus_from_bytes(bs(thirteen, 1));
    BigmodNat x = nat1(3), out = nat1(0);
    bigmod_nat_exp(&out, &x, bs(twelve, 1), m);
    BigmodNat expected = nat1(1);
    if (bigmod_nat_equal(&out, &expected) != 1)
        testing_t_errorf_v(t, "%d != %d", (int)out.limbs[0], 1);
    bigmod_nat_free(&x);
    bigmod_nat_free(&out);
    bigmod_nat_free(&expected);
    bigmod_modulus_free(m);
}

static void TestExpShort(TestingT *t) {
    static const uint8_t thirteen[] = {13};
    BigmodModulus *m = modulus_from_bytes(bs(thirteen, 1));
    BigmodNat x = nat1(3), out = nat1(0);
    bigmod_nat_exp_short_var_time(&out, &x, 12, m);
    BigmodNat expected = nat1(1);
    if (bigmod_nat_equal(&out, &expected) != 1)
        testing_t_errorf_v(t, "%d != %d", (int)out.limbs[0], 1);
    bigmod_nat_free(&x);
    bigmod_nat_free(&out);
    bigmod_nat_free(&expected);
    bigmod_modulus_free(m);
}

/* TestMulReductions tests that Mul reduces results equal or slightly greater
 * than the modulus. Some Montgomery algorithms don't and need extra care to
 * return correct results. See https://go.dev/issue/13907. */
static void TestMulReductions(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *al = arena_allocator(&ar);
    /* Two short but multi-limb primes. */
    BigInt *a = big_new_int(al, 0), *b = big_new_int(al, 0);
    bool ok;
    big_int_set_string(
        a, BURROW_S("773608962677651230850240281261679752031633236267106044359907"), 10,
        &ok);
    big_int_set_string(
        b, BURROW_S("180692823610368451951102211649591374573781973061758082626801"), 10,
        &ok);
    BigInt *n = big_new_int(al, 0);
    big_int_mul(n, a, b);

    BigmodModulus *N = bigmod_new_modulus(NULL, big_int_bytes(n, al), NULL);
    BigmodNat A, B;
    bigmod_nat_init(&A, NULL);
    bigmod_nat_init(&B, NULL);
    bigmod_nat_expand_for(set_big(&A, a), N);
    bigmod_nat_expand_for(set_big(&B, b), N);

    if (bigmod_nat_is_zero(bigmod_nat_mul(&A, &B, N)) != 1)
        testing_t_error_v(t, "a * b mod (a * b) != 0");
    bigmod_modulus_free(N);

    BigInt *i = big_new_int(al, 0);
    big_int_mod_inverse(i, a, b);
    N = bigmod_new_modulus(NULL, big_int_bytes(b, al), NULL);
    BigmodNat I, one;
    bigmod_nat_init(&I, NULL);
    bigmod_nat_init(&one, NULL);
    bigmod_nat_expand_for(set_big(&A, a), N);
    bigmod_nat_expand_for(set_big(&I, i), N);
    bigmod_nat_expand_for(set_big(&one, big_new_int(al, 1)), N);

    if (bigmod_nat_equal(bigmod_nat_mul(&A, &I, N), &one) != 1)
        testing_t_error_v(t, "a * inv(a) mod b != 1");
    bigmod_nat_free(&A);
    bigmod_nat_free(&B);
    bigmod_nat_free(&I);
    bigmod_nat_free(&one);
    bigmod_modulus_free(N);
    arena_free(&ar);
}

static void test_mul(void *env, TestingT *t) {
    Int n = (Int)(intptr_t)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *al = arena_allocator(&ar);
    uint8_t *a = mem_alloc(al, (size_t)n, 1), *b = mem_alloc(al, (size_t)n, 1),
            *m = mem_alloc(al, (size_t)n, 1);
    Error err = BURROW_NO_ERROR;
    crypto_rand_read(bs(a, n), &err);
    crypto_rand_read(bs(b, n), &err);
    crypto_rand_read(bs(m, n), &err);

    /* Pick the highest as the modulus. */
    if (bytes_compare(bs(a, n), bs(m, n)) > 0) {
        uint8_t *s = a;
        a = m;
        m = s;
    }
    if (bytes_compare(bs(b, n), bs(m, n)) > 0) {
        uint8_t *s = b;
        b = m;
        m = s;
    }

    BigmodModulus *M = bigmod_new_modulus(NULL, bs(m, n), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    BigmodNat A, B;
    bigmod_nat_init(&A, NULL);
    bigmod_nat_init(&B, NULL);
    if (bigmod_nat_set_bytes(&A, bs(a, n), M, &err) == NULL ||
        bigmod_nat_set_bytes(&B, bs(b, n), M, &err) == NULL)
        testing_t_fatalf_v(t, "%s", error_text(err));

    bigmod_nat_mul(&A, &B, M);
    Slice a_bytes = bigmod_nat_bytes(&A, al, M);

    BigInt *m_big = big_new_int(al, 0), *a_big = big_new_int(al, 0),
           *b_big = big_new_int(al, 0), *n_big = big_new_int(al, 0);
    big_int_set_bytes(m_big, bs(m, n));
    big_int_set_bytes(a_big, bs(a, n));
    big_int_set_bytes(b_big, bs(b, n));
    big_int_mul(n_big, a_big, b_big);
    big_int_mod(n_big, n_big, m_big);
    Slice n_big_bytes = slice_make(al, TYPE_BYTE, a_bytes.len, a_bytes.len);
    big_int_fill_bytes(n_big, n_big_bytes);

    if (!bytes_equal(a_bytes, n_big_bytes))
        testing_t_errorf_v(t, "got %x, want %x", a_bytes, n_big_bytes);
    bigmod_nat_free(&A);
    bigmod_nat_free(&B);
    bigmod_modulus_free(M);
    arena_free(&ar);
}

static void TestMul(TestingT *t) {
    testing_t_run(t, BURROW_S("small"),
                  BURROW_FN(TestingTFunc, test_mul, (void *)(intptr_t)(760 / 8)));
    testing_t_run(t, BURROW_S("1024"),
                  BURROW_FN(TestingTFunc, test_mul, (void *)(intptr_t)(1024 / 8)));
    testing_t_run(t, BURROW_S("1536"),
                  BURROW_FN(TestingTFunc, test_mul, (void *)(intptr_t)(1536 / 8)));
    testing_t_run(t, BURROW_S("2048"),
                  BURROW_FN(TestingTFunc, test_mul, (void *)(intptr_t)(2048 / 8)));
}

static void check_yes(TestingT *t, BigmodChoice c, const char *err) {
    if (c != 1)
        testing_t_error_v(t, str_from_cstr(err));
}

static void check_not(TestingT *t, BigmodChoice c, const char *err) {
    if (c != 0)
        testing_t_error_v(t, str_from_cstr(err));
}

static void TestIs(TestingT *t) {
    static const uint8_t four[] = {4}, three[] = {3}, one_b[] = {0x01};
    BigmodModulus *m_four = modulus_from_bytes(bs(four, 1));
    BigmodNat n;
    bigmod_nat_init(&n, NULL);
    Error err = BURROW_NO_ERROR;
    if (bigmod_nat_set_bytes(&n, bs(three, 1), m_four, &err) == NULL)
        testing_t_fatalf_v(t, "%s", error_text(err));
    check_yes(t, bigmod_nat_is_minus_one(&n, m_four), "3 is not -1 mod 4");
    check_not(t, bigmod_nat_is_zero(&n), "3 is zero");
    check_not(t, bigmod_nat_is_one(&n), "3 is one");
    check_yes(t, bigmod_nat_is_odd(&n), "3 is not odd");
    bigmod_nat_sub_one(&n, m_four);
    check_not(t, bigmod_nat_is_minus_one(&n, m_four), "2 is -1 mod 4");
    check_not(t, bigmod_nat_is_zero(&n), "2 is zero");
    check_not(t, bigmod_nat_is_one(&n), "2 is one");
    check_not(t, bigmod_nat_is_odd(&n), "2 is odd");
    bigmod_nat_sub_one(&n, m_four);
    check_not(t, bigmod_nat_is_minus_one(&n, m_four), "1 is -1 mod 4");
    check_not(t, bigmod_nat_is_zero(&n), "1 is zero");
    check_yes(t, bigmod_nat_is_one(&n), "1 is not one");
    check_yes(t, bigmod_nat_is_odd(&n), "1 is not odd");
    bigmod_nat_sub_one(&n, m_four);
    check_not(t, bigmod_nat_is_minus_one(&n, m_four), "0 is -1 mod 4");
    check_yes(t, bigmod_nat_is_zero(&n), "0 is not zero");
    check_not(t, bigmod_nat_is_one(&n), "0 is one");
    check_not(t, bigmod_nat_is_odd(&n), "0 is odd");
    bigmod_nat_sub_one(&n, m_four);
    check_yes(t, bigmod_nat_is_minus_one(&n, m_four), "-1 is not -1 mod 4");
    check_not(t, bigmod_nat_is_zero(&n), "-1 is zero");
    check_not(t, bigmod_nat_is_one(&n), "-1 is one");
    check_yes(t, bigmod_nat_is_odd(&n), "-1 mod 4 is not odd");

    BigmodModulus *m_two_limbs = max_modulus(2);
    if (bigmod_nat_set_bytes(&n, bs(one_b, 1), m_two_limbs, &err) == NULL)
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (bigmod_nat_is_one(&n) != 1)
        testing_t_errorf_v(t, "1 is not one");
    bigmod_nat_free(&n);
    bigmod_modulus_free(m_four);
    bigmod_modulus_free(m_two_limbs);
}

static void TestTrailingZeroBits(TestingT *t) {
    static const uint8_t b[] = {0xff, 0xff, 0xff, 0xff, 0xff,
                                0xff, 0xff, 0xff, 0xff, 0x7e};
    BigInt nb = BIG_INT(NULL);
    big_int_set_bytes(&nb, bs(b, sizeof b));
    big_int_lsh(&nb, &nb, 128);
    int expected = 129;
    BigmodNat n;
    bigmod_nat_init(&n, NULL);
    while (expected >= 0) {
        set_big(&n, &nb);
        if (bigmod_nat_trailing_zero_bits_var_time(&n) != (Uint)expected)
            testing_t_errorf_v(t, "%d != %d",
                               (int)bigmod_nat_trailing_zero_bits_var_time(&n),
                               expected);
        big_int_rsh(&nb, &nb, 1);
        expected--;
    }
    bigmod_nat_free(&n);
    big_int_free(&nb);
}

typedef struct ShiftCase {
    const BigInt *nb;
    Uint shift;
} ShiftCase;

static void test_shift(void *env, TestingT *t) {
    const ShiftCase *c = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BigmodNat n;
    bigmod_nat_init(&n, NULL);
    set_big(&n, c->nb);
    Int old_len = n.len;
    bigmod_nat_shift_right_var_time(&n, c->shift);
    if (n.len != old_len)
        testing_t_errorf_v(t, "len(n.limbs) = %d, want %d", n.len, old_len);
    BigInt *exp = big_new_int(a, 0);
    big_int_rsh(exp, c->nb, c->shift);
    BigInt *got = as_big(a, &n);
    if (big_int_cmp(got, exp) != 0)
        testing_t_errorf_v(t, "%s != %s", big_int_text(got, a, 10),
                           big_int_text(exp, a, 10));
    bigmod_nat_free(&n);
    arena_free(&ar);
}

static void TestRightShift(TestingT *t) {
    uint8_t b[128];
    Error err = BURROW_NO_ERROR;
    crypto_rand_read(bs(b, sizeof b), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    BigInt nb = BIG_INT(NULL);
    big_int_set_bytes(&nb, bs(b, sizeof b));
    static const Uint shifts[] = {1,          32,        64,        128,
                                  1024 - 128, 1024 - 64, 1024 - 32, 1024 - 1};
    for (size_t i = 0; i < sizeof shifts / sizeof shifts[0]; i++) {
        for (Uint d = 0; d < 3; d++) {
            ShiftCase c = {&nb, shifts[i] - 1 + d};
            char name[24];
            snprintf(name, sizeof name, "%u", (unsigned)c.shift);
            testing_t_run(t, str_from_cstr(name),
                          BURROW_FN(TestingTFunc, test_shift, &c));
        }
    }
    big_int_free(&nb);
}

static void TestNewModulus(TestingT *t) {
    static const uint8_t zero24[24] = {0};
    static const uint8_t one24[24] = {[23] = 1};
    static const uint8_t zero[] = {0}, one[] = {1};
    struct {
        const char *name;
        Slice b;
    } cases[] = {
        {"0", bs(zero, 0)}, {"0", bs(zero, 1)},   {"0", bs(zero24, 24)},
        {"1", bs(one, 1)},  {"1", bs(one24, 24)},
    };
    Str expected = BURROW_S("modulus must be > 1");
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Error err = BURROW_NO_ERROR;
        BigmodModulus *m = bigmod_new_modulus(NULL, cases[i].b, &err);
        if (m != NULL || !BURROW_FAILED(err) || !str_eq(error_text(err), expected))
            testing_t_errorf_v(
                t, "NewModulus(%s) got %q, want %q", str_from_cstr(cases[i].name),
                BURROW_FAILED(err) ? error_text(err) : BURROW_S("<nil>"), expected);
        if (m != NULL)
            bigmod_modulus_free(m);
    }
}

static void test_add_mul_vvw_sized(void *env, TestingT *t) {
    int bits = (int)(intptr_t)env;
    Int n = bits / BIGMOD_W;
    Uint x[2048 / 32], z[2048 / 32], z2[2048 / 32];
    for (Int i = 0; i < n; i++)
        x[i] = z[i] = z2[i] = ~(Uint)0;
    Uint y = ~(Uint)0;
    Uint c = bigmod_add_mul_vvw(z, x, y, n);
    Uint c2 = bits == 1024   ? bigmod_add_mul_vvw1024(z2, x, y)
              : bits == 1536 ? bigmod_add_mul_vvw1536(z2, x, y)
                             : bigmod_add_mul_vvw2048(z2, x, y);
    if (memcmp(z, z2, (size_t)n * sizeof(Uint)) != 0 || c != c2)
        testing_t_errorf_v(t, "the sized addMulVVW for %d bits differs", bits);
}

static void TestAddMulVVWSized(TestingT *t) {
    /* Sized addMulVVW have architecture-specific implementations on a number
     * of architectures. Test that they match the generic implementation. */
    testing_t_run(
        t, BURROW_S("1024"),
        BURROW_FN(TestingTFunc, test_add_mul_vvw_sized, (void *)(intptr_t)1024));
    testing_t_run(
        t, BURROW_S("1536"),
        BURROW_FN(TestingTFunc, test_add_mul_vvw_sized, (void *)(intptr_t)1536));
    testing_t_run(
        t, BURROW_S("2048"),
        BURROW_FN(TestingTFunc, test_add_mul_vvw_sized, (void *)(intptr_t)2048));
}

/* testdata/mod_inv_tests.txt. */
static const char *const mod_inv_tests[] = {
    "# ModInv tests.",
    "#",
    "# These test vectors satisfy ModInv * A = 1 (mod M) and 0 <= ModInv < M.",
    "",
    "ModInv = 00",
    "A = 00",
    "M = 01",
    "",
    "ModInv = 00",
    "A = 01",
    "M = 01",
    "",
    "ModInv = 00",
    "A = 02",
    "M = 01",
    "",
    "ModInv = 00",
    "A = 03",
    "M = 01",
    "",
    "ModInv = 64",
    "A = 54",
    "M = e3",
    "",
    "ModInv = 13",
    "A = 2b",
    "M = 30",
    "",
    "ModInv = 2f",
    "A = 30",
    "M = 37",
    "",
    "ModInv = 4",
    "A = 13",
    "M = 4b",
    "",
    "ModInv = 1c47",
    "A = cd4",
    "M = 6a21",
    "",
    "ModInv = 2b97",
    "A = 8e7",
    "M = 49c0",
    "",
    "ModInv = 29b9",
    "A = fcb",
    "M = 3092",
    "",
    "ModInv = a83",
    "A = 14bf",
    "M = 41ae",
    "",
    "ModInv = 18f15fe1",
    "A = 11b5d53e",
    "M = 322e92a1",
    "",
    "ModInv = 32f9453b",
    "A = 8af6df6",
    "M = 33d45eb7",
    "",
    "ModInv = d696369",
    "A = c5f89dd5",
    "M = fc09c17c",
    "",
    "ModInv = 622839d8",
    "A = 60c2526",
    "M = 74200493",
    "",
    "ModInv = fb5a8aee7bbc4ef",
    "A = 24ebd835a70be4e2",
    "M = 9c7256574e0c5e93",
    "",
    "ModInv = 846bc225402419c",
    "A = 23026003ab1fbdb",
    "M = 1683cbe32779c59b",
    "",
    "ModInv = 5ff84f63a78982f9",
    "A = 4a2420dc733e1a0f",
    "M = a73c6bfabefa09e6",
    "",
    "ModInv = 133e74d28ef42b43",
    "A = 2e9511ae29cdd41",
    "M = 15234df99f19fcda",
    "",
    "ModInv = 46ae1fabe9521e4b99b198fc8439609023aa69be2247c0d1e27c2a0ea332f9c5",
    "A = 6331fec5f01014046788c919ed50dc86ac7a80c085f1b6f645dd179c0f0dc9cd",
    "M = 8ef409de82318259a8655a39293b1e762fa2cc7e0aeb4c59713a1e1fff6af640",
    "",
    "ModInv = 444ccea3a7b21677dd294d34de53cc8a5b51e69b37782310a00fc6bcc975709b",
    "A = 679280bd880994c08322143a4ea8a0825d0466fda1bb6b3eb86fc8e90747512b",
    "M = e4fecab84b365c63a0dab4244ce3f921a9c87ec64d69a2031939f55782e99a2e",
    "",
    "ModInv = 1ac7d7a03ceec5f690f567c9d61bf3469c078285bcc5cf00ac944596e887ca17",
    "A = 1593ef32d9c784f5091bdff952f5c5f592a3aed6ba8ea865efa6d7df87be1805",
    "M = 1e276882f90c95e0c1976eb079f97af075445b1361c02018d6bd7191162e67b2",
    "",
    "ModInv = 639108b90dfe946f498be21303058413bbb0e59d0bd6a6115788705abd0666d6",
    "A = 9258d6238e4923d120b2d1033573ffcac691526ad0842a3b174dccdbb79887bd",
    "M = ce62909c39371d463aaba3d4b72ea6da49cb9b529e39e1972ef3ccd9a66fe08f",
    "",
    "ModInv = "
    "aebde7654cb17833a106231c4b9e2f519140e85faee1bfb4192830f03f385e773c0f4767e93e874ffd"
    "c3b7a6b7e6a710e5619901c739ee8760a26128e8c91ef8cf761d0e505d8b28ae078d17e6071c372893"
    "bb7b72538e518ebc57efa70b7615e406756c49729b7c6e74f84aed7a316b6fa748ff4b9f143129d29d"
    "ad1bff98bb",
    "A = "
    "a29dacaf5487d354280fdd2745b9ace4cd50f2bde41d0ee529bf26a1913244f708085452ff32feab19"
    "a7418897990da46a0633f7c8375d583367319091bbbe069b0052c5e48a7daac9fb650db5af768cd250"
    "8ec3e2cda7456d4b9ce1c39459627a8b77e038b826cd7e326d0685b0cd0cb50f026f18300dae9f5fd4"
    "2aa150ee8b",
    "M = "
    "d686f9b86697313251685e995c09b9f1e337ddfaa050bd2df15bf4ca1dc46c5565021314765299c434"
    "ea1a6ec42bf92a29a7d1ffff599f4e50b79a82243fb24813060580c770d4c1140aeb2ab2685007e948"
    "b6f1f62e8001a0545619477d498132c907774479f6d95899e6251e7136f79ab6d3b7c82e4aca421e7d"
    "22fe7db19c",
    "",
    "ModInv = "
    "1ec872f4f20439e203597ca4de9d1296743f95781b2fe85d5def808558bbadef02a46b8955f47c83e1"
    "625f8bb40228eab09cad2a35c9ad62ab77a30e3932872959c5898674162da244a0ec1f68c0ed89f4b0"
    "f3572bfdc658ad15bf1b1c6e1176b0784c9935bd3ff1f49bb43753eacee1d8ca1c0b652d39ec727da8"
    "3984fe3a0f",
    "A = "
    "2e527b0a1dc32460b2dd94ec446c692989f7b3c7451a5cbeebf69fc0ea9c4871fbe78682d5dc5b6668"
    "9f7ed889b52161cd9830b589a93d21ab26dbede6c33959f5a0f0d107169e2daaac78bac8cf2d41a1eb"
    "1369cb6dc9e865e73bb2e51b886f4e896082db199175e3dde0c4ed826468f238a77bd894245d0918ef"
    "c9ca84f945",
    "M = "
    "b13133a9ebe0645f987d170c077eea2aa44e85c9ab10386d02867419a590cb182d9826a882306c212d"
    "be75225adde23f80f5b37ca75ed09df20fc277cc7fbbfac8d9ef37a50f6b68ea158f5447283618e64e"
    "1426406d26ea85232afb22bf546c75018c1c55cb84c374d58d9d44c0a13ba88ac2e387765cb4c3269e"
    "3a983250fa",
    "",
    "ModInv = "
    "30ffa1876313a69de1e4e6ee132ea1d3a3da32f3b56f5cfb11402b0ad517dce605cf8e91d69fa375dd"
    "887fa8507bd8a28b2d5ce745799126e86f416047709f93f07fbd88918a047f13100ea71b1d48f6fc6d"
    "12e5c917646df3041b302187af641eaedf4908abc36f12c204e1526a7d80e96e302fb0779c28d7da60"
    "7243732f26",
    "A = "
    "31157208bde6b85ebecaa63735947b3b36fa351b5c47e9e1c40c947339b78bf96066e5dbe21bb42629"
    "e6fcdb81f5f88db590bfdd5f4c0a6a0c3fc6377e5c1fd8235e46e291c688b6d6ecfb36604891c2a7c9"
    "cbcc58c26e44b43beecb9c5044b58bb58e35de3cf1128f3c116534fe4e421a33f83603c3df1ae36ec8"
    "8092f67f2a",
    "M = "
    "53408b23d6cb733e6c9bc3d1e2ea2286a5c83cc4e3e7470f8af3a1d9f28727f5b1f8ae348c1678f5d1"
    "105dc3edf2de64e65b9c99545c47e64b770b17c8b4ef5cf194b43a0538053e87a6b95ade1439cebf3d"
    "34c6aa72a11c1497f58f76011e16c5be087936d88aba7a740113120e939e27bd3ddcb6580c2841aa40"
    "6566e33c35",
    "",
    "ModInv = "
    "87355002f305c81ba0dc97ca2234a2bc02528cefde38b94ac5bd95efc7bf4c140899107fff47f0df9e"
    "3c6aa70017ebc90610a750f112cd4f475b9c76b204a953444b4e7196ccf17e93fdaed160b7345ca9b3"
    "97eddf9446e8ea8ee3676102ce70eaafbe9038a34639789e6f2f1e3f352638f2e8a8f5fc56aaea7ec7"
    "05ee068dd5",
    "A = "
    "42a25d0bc96f71750f5ac8a51a1605a41b506cca51c9a7ecf80cad713e56f70f1b4b6fa51cbb101f55"
    "fd74f318adefb3af04e0c8a7e281055d5a40dd40913c0e1211767c5be915972c73886106dc49325df6"
    "c2df49e9eea4536f0343a8e7d332c6159e4f5bdb20d89f90e67597c4a2a632c31b2ef2534080a9ac61"
    "f52303990d",
    "M = "
    "d3d3f95d50570351528a76ab1e806bae1968bd420899bdb3d87c823fac439a4354c31f6c888c939784"
    "f18fe10a95e6d203b1901caa18937ba6f8be033af10c35fc869cf3d16bef479f280f53b3499e645d03"
    "87554623207ca4989e5de00bfeaa5e9ab56474fc60dd4967b100e0832eaaf2fcb2ef82a181567057b8"
    "80b3afef62",
};

static Slice decode_hex(TestingT *t, Alloc *a, Str s) {
    if (s.len % 2 != 0) {
        Byte *p = mem_alloc(a, (size_t)s.len + 1, 1);
        p[0] = '0';
        memcpy(p + 1, s.p, (size_t)s.len);
        s = (Str){p, s.len + 1};
    }
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, s, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to decode hex %q: %s", s, error_text(err));
    return b;
}

typedef struct InverseCase {
    Str mod_inv, a, m;
} InverseCase;

static void inverse_case(void *env, TestingT *t) {
    const InverseCase *c = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *al = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    BigmodModulus *m = bigmod_new_modulus(NULL, decode_hex(t, al, c->m), &err);
    if (m == NULL) {
        arena_free(&ar);
        testing_t_skip_v(t, "modulus <= 1");
        return;
    }
    BigmodNat a, got, exp;
    bigmod_nat_init(&a, NULL);
    bigmod_nat_init(&got, NULL);
    bigmod_nat_init(&exp, NULL);
    if (bigmod_nat_set_bytes(&a, decode_hex(t, al, c->a), m, &err) == NULL)
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!bigmod_nat_inverse_var_time(&got, &a, m))
        testing_t_fatal_v(t, "not invertible");
    if (bigmod_nat_set_bytes(&exp, decode_hex(t, al, c->mod_inv), m, &err) == NULL)
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (bigmod_nat_equal(&got, &exp) != 1)
        testing_t_errorf_v(t, "%s != %s", nat_string(al, &got), nat_string(al, &exp));
    bigmod_nat_free(&a);
    bigmod_nat_free(&got);
    bigmod_nat_free(&exp);
    bigmod_modulus_free(m);
    arena_free(&ar);
}

static void TestInverse(TestingT *t) {
    InverseCase c = {{0}, {0}, {0}};
    int n = (int)(sizeof mod_inv_tests / sizeof mod_inv_tests[0]);
    for (int i = 0; i < n; i++) {
        int line_num = i + 1;
        Str line = str_from_cstr(mod_inv_tests[i]);
        if (line.len == 0 || line.p[0] == '#')
            continue;
        Str v;
        Str k = strings_cut(line, BURROW_S(" = "), &v, NULL);
        if (str_eq(k, BURROW_S("ModInv"))) {
            c.mod_inv = v;
        } else if (str_eq(k, BURROW_S("A"))) {
            c.a = v;
        } else if (str_eq(k, BURROW_S("M"))) {
            c.m = v;
            char name[32];
            snprintf(name, sizeof name, "line %d", line_num);
            testing_t_run(t, str_from_cstr(name),
                          BURROW_FN(TestingTFunc, inverse_case, &c));
        } else {
            testing_t_fatalf_v(t, "unknown key %q on line %d", k, line_num);
        }
    }
}

/* ---------------------------------------------------------- burrow's own */

static void TestBurrow(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *al = arena_allocator(&ar);

    /* DivShortVarTime: 1000003 / 7. */
    BigmodNat x = nat1(1000003);
    Uint r = bigmod_nat_div_short_var_time(&x, 7);
    if (x.limbs[0] != 142857 || r != 4)
        testing_t_errorf_v(t, "1000003 / 7 = %d rem %d", (int)x.limbs[0], (int)r);
    bigmod_nat_free(&x);

    /* GCDVarTime: gcd(84, 45) = 3, and two even numbers are an error. */
    BigmodNat a = nat1(84), b = nat1(45), g;
    bigmod_nat_init(&g, NULL);
    if (bigmod_nat_gcd_var_time(&g, &a, &b, NULL) == NULL || g.limbs[0] != 3)
        testing_t_errorf_v(t, "gcd(84, 45) = %d", (int)g.limbs[0]);
    b.limbs[0] = 46;
    Error err = BURROW_NO_ERROR;
    if (bigmod_nat_gcd_var_time(&g, &a, &b, &err) != NULL ||
        !str_eq(error_text(err), BURROW_S("extendedGCD: both a and m are even")))
        testing_t_errorf_v(t, "gcd(84, 46) gave %s", error_text(err));
    bigmod_nat_free(&a);
    bigmod_nat_free(&b);
    bigmod_nat_free(&g);

    /* NewModulusProduct is NewModulus of the product. */
    BigInt *p = big_new_int(al, 0), *q = big_new_int(al, 0), *pq = big_new_int(al, 0);
    bool ok;
    big_int_set_string(
        p, BURROW_S("773608962677651230850240281261679752031633236267106044359907"), 10,
        &ok);
    big_int_set_string(
        q, BURROW_S("180692823610368451951102211649591374573781973061758082626801"), 10,
        &ok);
    big_int_mul(pq, p, q);
    BigmodModulus *m1 = bigmod_new_modulus_product(NULL, big_int_bytes(p, al),
                                                   big_int_bytes(q, al), NULL);
    BigmodModulus *m2 = bigmod_new_modulus(NULL, big_int_bytes(pq, al), NULL);
    if (m1 == NULL || m1->nat.len != m2->nat.len ||
        bigmod_nat_equal(&m1->nat, &m2->nat) != 1 ||
        bigmod_nat_equal(&m1->rr, &m2->rr) != 1 || m1->m0inv != m2->m0inv)
        testing_t_error_v(t, "NewModulusProduct(p, q) is not NewModulus(p*q)");

    /* SetOverflowingBytes reduces what has no more bits than the modulus, and
     * Exp agrees with math/big. */
    BigmodNat y;
    bigmod_nat_init(&y, NULL);
    Slice all_ones =
        slice_make(al, TYPE_BYTE, bigmod_modulus_size(m2), bigmod_modulus_size(m2));
    memset(all_ones.p, 0xff, (size_t)all_ones.len);
    big_int_set_bytes(pq, all_ones);
    if (big_int_bit_len(pq) == bigmod_modulus_bit_len(m2)) {
        if (bigmod_nat_set_overflowing_bytes(&y, all_ones, m2, &err) == NULL)
            testing_t_fatalf_v(t, "SetOverflowingBytes: %s", error_text(err));
        BigInt *want = big_new_int(al, 0), *mod = big_new_int(al, 0);
        big_int_set_bytes(mod, bigmod_nat_bytes(&m2->nat, al, m2));
        big_int_mod(want, pq, mod);
        if (big_int_cmp(as_big(al, &y), want) != 0)
            testing_t_error_v(t, "SetOverflowingBytes did not reduce");

        static const uint8_t e[] = {0x01, 0x00, 0x01};
        BigmodNat out;
        bigmod_nat_init(&out, NULL);
        bigmod_nat_exp(&out, &y, bs(e, 3), m2);
        big_int_exp(want, want, big_new_int(al, 65537), mod);
        if (big_int_cmp(as_big(al, &out), want) != 0)
            testing_t_error_v(t, "Exp differs from math/big");
        bigmod_nat_exp_short_var_time(&out, &y, 65537, m2);
        if (big_int_cmp(as_big(al, &out), want) != 0)
            testing_t_error_v(t, "ExpShortVarTime differs from math/big");
        bigmod_nat_free(&out);
    }
    uint8_t too_big[64];
    memset(too_big, 0xff, sizeof too_big);
    if (bigmod_nat_set_overflowing_bytes(&y, bs(too_big, sizeof too_big), m2, &err) !=
        NULL)
        testing_t_error_v(t, "SetOverflowingBytes took more bits than the modulus has");
    bigmod_nat_free(&y);
    bigmod_modulus_free(m1);
    bigmod_modulus_free(m2);
    arena_free(&ar);
}

static BigmodModulus *panic_m;
static BigmodNat panic_x;
/* What do_bytes and do_shrink work on, set up before they panic so that
 * TestPanics can free them. */
static BigmodNat panic_big;
static BigmodNat panic_two;
static Alloc *panic_a;

static void do_exp(void) {
    static const uint8_t e[] = {3};
    BigmodNat out;
    bigmod_nat_init(&out, NULL);
    bigmod_nat_exp(&out, &panic_x, bs(e, 1), panic_m);
}

static void do_exp_short(void) {
    BigmodNat out;
    bigmod_nat_init(&out, NULL);
    bigmod_nat_exp_short_var_time(&out, &panic_x, 3, panic_m);
}

static void do_div(void) {
    bigmod_nat_div_short_var_time(&panic_x, 0);
}

static void do_bytes(void) {
    bigmod_nat_bytes(&panic_big, panic_a, panic_m);
}

static void do_shrink(void) {
    bigmod_nat_expand(&panic_two, 1);
}

static Str panic_of(Alloc *a, void (*f)(void)) {
    Str volatile msg = {0};
    BURROW_TRY {
        f();
    }
    BURROW_CATCH(r) {
        Str s = panic_text(r);
        Byte *p = mem_alloc(a, (size_t)s.len, 1);
        memcpy(p, s.p, (size_t)s.len);
        msg = (Str){p, s.len};
    }
    BURROW_TRY_END;
    return msg;
}

static void TestPanics(TestingT *t) {
    static const uint8_t ten[] = {10};
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    panic_m = modulus_from_bytes(bs(ten, 1));
    panic_x = nat1(3);
    panic_big = nat1(~(Uint)0);
    panic_two = nat_of((const Uint[]){1, 1}, 2);
    panic_a = a;
    struct {
        void (*f)(void);
        const char *want;
    } cases[] = {
        {do_exp, "bigmod: modulus for Exp must be odd"},
        {do_exp_short, "bigmod: modulus for ExpShortVarTime must be odd"},
        {do_div, "bigmod: division by zero"},
        {do_bytes, "bigmod: modulus is smaller than nat"},
        {do_shrink, "bigmod: internal error: shrinking nat"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Str got = panic_of(a, cases[i].f);
        if (!str_eq(got, str_from_cstr(cases[i].want)))
            testing_t_errorf_v(t, "panicked with %q, want %q", got,
                               str_from_cstr(cases[i].want));
    }
    bigmod_nat_free(&panic_x);
    bigmod_nat_free(&panic_big);
    bigmod_nat_free(&panic_two);
    bigmod_modulus_free(panic_m);
    arena_free(&ar);
}

/* ------------------------------------------------------------ benchmarks */

static BigmodModulus *make_benchmark_modulus(void) {
    return max_modulus(32);
}

static BigmodNat make_benchmark_value(void) {
    BigmodNat x;
    bigmod_nat_init(&x, NULL);
    bigmod_nat_reset(&x, 32);
    for (int i = 0; i < 32; i++)
        x.limbs[i]--;
    return x;
}

static Slice make_benchmark_exponent(uint8_t e[256]) {
    memset(e, 0, 256);
    memset(e, 0xff, 32);
    return bs(e, 256);
}

static void BenchmarkModAdd(TestingB *b) {
    BigmodNat x = make_benchmark_value(), y = make_benchmark_value();
    BigmodModulus *m = make_benchmark_modulus();
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        bigmod_nat_add(&x, &y, m);
    bigmod_nat_free(&x);
    bigmod_nat_free(&y);
    bigmod_modulus_free(m);
}

static void BenchmarkModSub(TestingB *b) {
    BigmodNat x = make_benchmark_value(), y = make_benchmark_value();
    BigmodModulus *m = make_benchmark_modulus();
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        bigmod_nat_sub(&x, &y, m);
    bigmod_nat_free(&x);
    bigmod_nat_free(&y);
    bigmod_modulus_free(m);
}

static void BenchmarkMontgomeryRepr(TestingB *b) {
    BigmodNat x = make_benchmark_value();
    BigmodModulus *m = make_benchmark_modulus();
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        bigmod_nat_montgomery_representation(&x, m);
    bigmod_nat_free(&x);
    bigmod_modulus_free(m);
}

static void BenchmarkMontgomeryMul(TestingB *b) {
    BigmodNat x = make_benchmark_value(), y = make_benchmark_value(),
              out = make_benchmark_value();
    BigmodModulus *m = make_benchmark_modulus();
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        bigmod_nat_montgomery_mul(&out, &x, &y, m);
    bigmod_nat_free(&x);
    bigmod_nat_free(&y);
    bigmod_nat_free(&out);
    bigmod_modulus_free(m);
}

static void BenchmarkModMul(TestingB *b) {
    BigmodNat x = make_benchmark_value(), y = make_benchmark_value();
    BigmodModulus *m = make_benchmark_modulus();
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        bigmod_nat_mul(&x, &y, m);
    bigmod_nat_free(&x);
    bigmod_nat_free(&y);
    bigmod_modulus_free(m);
}

static void BenchmarkExpBig(TestingB *b) {
    uint8_t eb[256];
    Slice exponent_bytes = make_benchmark_exponent(eb);
    BigInt out = BIG_INT(NULL), x = BIG_INT(NULL), e = BIG_INT(NULL), n = BIG_INT(NULL),
           one = BIG_INT(NULL);
    big_int_set_bytes(&x, exponent_bytes);
    big_int_set_bytes(&e, exponent_bytes);
    big_int_set_bytes(&n, exponent_bytes);
    big_int_set_uint64(&one, 1);
    big_int_add(&n, &n, &one);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        big_int_exp(&out, &x, &e, &n);
    big_int_free(&out);
    big_int_free(&x);
    big_int_free(&e);
    big_int_free(&n);
    big_int_free(&one);
}

static void BenchmarkExp(TestingB *b) {
    uint8_t eb[256];
    BigmodNat x = make_benchmark_value(), out = make_benchmark_value();
    Slice e = make_benchmark_exponent(eb);
    BigmodModulus *m = make_benchmark_modulus();
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++)
        bigmod_nat_exp(&out, &x, e, m);
    bigmod_nat_free(&x);
    bigmod_nat_free(&out);
    bigmod_modulus_free(m);
}

#define TESTS(X)                                                                       \
    X(TestModAddCommutative)                                                           \
    X(TestModSubThenAddIdentity)                                                       \
    X(TestMontgomeryRoundtrip)                                                         \
    X(TestShiftIn)                                                                     \
    X(TestModulusAndNatSizes)                                                          \
    X(TestSetBytes)                                                                    \
    X(TestExpand)                                                                      \
    X(TestMod)                                                                         \
    X(TestModSub)                                                                      \
    X(TestModAdd)                                                                      \
    X(TestExp)                                                                         \
    X(TestExpShort)                                                                    \
    X(TestMulReductions)                                                               \
    X(TestMul)                                                                         \
    X(TestIs)                                                                          \
    X(TestTrailingZeroBits)                                                            \
    X(TestRightShift)                                                                  \
    X(TestNewModulus)                                                                  \
    X(TestAddMulVVWSized)                                                              \
    X(TestInverse)                                                                     \
    X(TestBurrow)                                                                      \
    X(TestPanics)                                                                      \
    X(BenchmarkModAdd)                                                                 \
    X(BenchmarkModSub)                                                                 \
    X(BenchmarkMontgomeryRepr)                                                         \
    X(BenchmarkMontgomeryMul)                                                          \
    X(BenchmarkModMul)                                                                 \
    X(BenchmarkExpBig)                                                                 \
    X(BenchmarkExp)

TESTING_MAIN(TESTS)
