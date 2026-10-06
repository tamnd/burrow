/* Derived from Go's src/crypto/elliptic/elliptic_test.go, p224_test.go and
 * p256_test.go.
 * Go source: go1.27.1.
 *
 * Go runs the subtests of testAllCurves in parallel, and these run them one
 * after another. TestPanics, TestGenerateKeyError and TestNilY are burrow's
 * own: Go has no test for the panics, and the other two are about the C shape.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/rand.h"
#include "burrow/encoding/hex.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"

#include <stdint.h>
#include <string.h>

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("invalid hex"));
    return b;
}

static BigInt *big(Alloc *a, const char *s, Int base) {
    BigInt *z = big_new_int(a, 0);
    bool ok = false;
    big_int_set_string(z, str_from_cstr(s), base, &ok);
    if (!ok)
        panic_str(BURROW_S("invalid number"));
    return z;
}

static Str hexof(Alloc *a, const BigInt *x) {
    return big_int_text(x, a, 16);
}

static bool same_curve(EllipticCurve x, EllipticCurve y) {
    return x.vt == y.vt && x.data == y.data;
}

static bool is_infinity(const BigInt *x, const BigInt *y) {
    return big_int_sign(x) == 0 && big_int_sign(y) == 0;
}

static Int byte_len(EllipticCurve curve) {
    return (elliptic_curve_params(curve)->bit_size + 7) / 8;
}

static Slice sub(Slice s, Int from, Int to) {
    return slice_from((uint8_t *)s.p + from, to - from, to - from, TYPE_BYTE);
}

/* curve.Params().polynomial(x), which the tests reach in Go and is not
 * exported: x³ - 3x + b mod p. */
static BigInt *polynomial(Alloc *a, EllipticCurve curve, const BigInt *x) {
    const EllipticCurveParams *params = elliptic_curve_params(curve);
    BigInt *x3 = big_new_int(a, 0);
    BigInt *three_x = big_new_int(a, 0);
    big_int_mul(x3, x, x);
    big_int_mul(x3, x3, x);
    big_int_lsh(three_x, x, 1);
    big_int_add(three_x, three_x, x);
    big_int_sub(x3, x3, three_x);
    big_int_add(x3, x3, params->b);
    big_int_mod(x3, x3, params->p);
    return x3;
}

/* --------------------------------------------------------- testAllCurves */

typedef struct NamedCurve {
    const char *name;
    EllipticCurve curve;
} NamedCurve;

static EllipticCurveParams generic[4];
static NamedCurve all_curves[8];

/* genericParamsForCurve: a copy of the params, which no longer match the
 * curve's own and so get the generic implementation. */
static EllipticCurve generic_params_for_curve(EllipticCurveParams *d, EllipticCurve c) {
    *d = *elliptic_curve_params(c);
    return elliptic_curve_params_as_elliptic_curve(d);
}

static void setup_curves(void) {
    EllipticCurve cs[4] = {elliptic_p256(), elliptic_p224(), elliptic_p384(),
                           elliptic_p521()};
    static const char *names[8] = {"P256", "P256/Params", "P224", "P224/Params",
                                   "P384", "P384/Params", "P521", "P521/Params"};
    for (int i = 0; i < 4; i++) {
        all_curves[2 * i] = (NamedCurve){names[2 * i], cs[i]};
        all_curves[2 * i + 1] = (NamedCurve){
            names[2 * i + 1], generic_params_for_curve(&generic[i], cs[i])};
    }
}

typedef void (*CurveTest)(TestingT *t, EllipticCurve curve, Alloc *a);

typedef struct CurveRun {
    CurveTest f;
    EllipticCurve curve;
} CurveRun;

static void curve_run(void *env, TestingT *t) {
    CurveRun *r = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    r->f(t, r->curve, arena_allocator(&ar));
    arena_free(&ar);
}

static void test_all_curves(TestingT *t, CurveTest f) {
    setup_curves();
    int n = testing_short() ? 1 : 8;
    for (int i = 0; i < n; i++) {
        CurveRun r = {f, all_curves[i].curve};
        testing_t_run(t, str_from_cstr(all_curves[i].name),
                      BURROW_FN(TestingTFunc, curve_run, &r));
    }
}

static void generate(TestingT *t, EllipticCurve curve, Alloc *a, BigInt **x,
                     BigInt **y) {
    Error err = BURROW_NO_ERROR;
    (void)elliptic_generate_key(a, curve, crypto_rand_reader, x, y, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
}

/* ------------------------------------------------------------- the tests */

static void on_curve(TestingT *t, EllipticCurve curve, Alloc *a) {
    (void)a;
    const EllipticCurveParams *params = elliptic_curve_params(curve);
    if (!elliptic_curve_is_on_curve(curve, params->gx, params->gy))
        testing_t_error_v(t, "basepoint is not on the curve");
}

static void TestOnCurve(TestingT *t) {
    test_all_curves(t, on_curve);
}

static void off_curve(TestingT *t, EllipticCurve curve, Alloc *a) {
    BigInt *x = big_new_int(a, 1), *y = big_new_int(a, 1);
    if (elliptic_curve_is_on_curve(curve, x, y))
        testing_t_errorf_v(t, "point off curve is claimed to be on the curve");

    Int n = byte_len(curve);
    Slice b = slice_make(a, TYPE_BYTE, 1 + 2 * n, 1 + 2 * n);
    ((uint8_t *)b.p)[0] = 4; /* uncompressed point */
    big_int_fill_bytes(x, sub(b, 1, 1 + n));
    big_int_fill_bytes(y, sub(b, 1 + n, 1 + 2 * n));

    BigInt *y1 = NULL;
    BigInt *x1 = elliptic_unmarshal(a, curve, b, &y1);
    if (x1 != NULL || y1 != NULL)
        testing_t_errorf_v(t, "unmarshaling a point not on the curve succeeded");
}

static void TestOffCurve(TestingT *t) {
    test_all_curves(t, off_curve);
}

static void infinity(TestingT *t, EllipticCurve curve, Alloc *a) {
    const EllipticCurveParams *params = elliptic_curve_params(curve);
    BigInt *x0 = big_new_int(a, 0), *y0 = big_new_int(a, 0);
    const BigInt *xg = params->gx, *yg = params->gy;
    BigInt *x, *y;
    uint8_t zero[] = {0}, k123[] = {1, 2, 3};
    Slice s0 = slice_from(zero, 1, 1, TYPE_BYTE);
    Slice s123 = slice_from(k123, 3, 3, TYPE_BYTE);

    x = elliptic_curve_scalar_mult(curve, a, xg, yg, big_int_bytes(params->n, a), &y);
    if (!is_infinity(x, y))
        testing_t_errorf_v(t, "x^q != ∞");
    x = elliptic_curve_scalar_mult(curve, a, xg, yg, s0, &y);
    if (!is_infinity(x, y))
        testing_t_errorf_v(t, "x^0 != ∞");

    x = elliptic_curve_scalar_mult(curve, a, x0, y0, s123, &y);
    if (!is_infinity(x, y))
        testing_t_errorf_v(t, "∞^k != ∞");
    x = elliptic_curve_scalar_mult(curve, a, x0, y0, s0, &y);
    if (!is_infinity(x, y))
        testing_t_errorf_v(t, "∞^0 != ∞");

    x = elliptic_curve_scalar_base_mult(curve, a, big_int_bytes(params->n, a), &y);
    if (!is_infinity(x, y))
        testing_t_errorf_v(t, "b^q != ∞");
    x = elliptic_curve_scalar_base_mult(curve, a, s0, &y);
    if (!is_infinity(x, y))
        testing_t_errorf_v(t, "b^0 != ∞");

    x = elliptic_curve_double(curve, a, x0, y0, &y);
    if (!is_infinity(x, y))
        testing_t_errorf_v(t, "2∞ != ∞");
    /* There is no other point of order two on the NIST curves. */

    BigInt *n_minus_one = big_new_int(a, 0);
    big_int_sub(n_minus_one, params->n, big_new_int(a, 1));
    x = elliptic_curve_scalar_mult(curve, a, xg, yg, big_int_bytes(n_minus_one, a), &y);
    x = elliptic_curve_add(curve, a, x, y, xg, yg, &y);
    if (!is_infinity(x, y))
        testing_t_errorf_v(t, "x^(q-1) + x != ∞");
    x = elliptic_curve_add(curve, a, xg, yg, x0, y0, &y);
    if (big_int_cmp(x, xg) != 0 || big_int_cmp(y, yg) != 0)
        testing_t_errorf_v(t, "x+∞ != x");
    x = elliptic_curve_add(curve, a, x0, y0, xg, yg, &y);
    if (big_int_cmp(x, xg) != 0 || big_int_cmp(y, yg) != 0)
        testing_t_errorf_v(t, "∞+x != x");

    if (elliptic_curve_is_on_curve(curve, x0, y0))
        testing_t_errorf_v(t, "IsOnCurve(∞) == true");

    BigInt *xx, *yy;
    xx = elliptic_unmarshal(a, curve, elliptic_marshal(a, curve, x0, y0), &yy);
    if (xx != NULL || yy != NULL)
        testing_t_errorf_v(t, "Unmarshal(Marshal(∞)) did not return an error");
    /* We don't test UnmarshalCompressed(MarshalCompressed(∞)) because there are
     * two valid points with x = 0. */
    xx = elliptic_unmarshal(a, curve, s0, &yy);
    if (xx != NULL || yy != NULL)
        testing_t_errorf_v(t, "Unmarshal(∞) did not return an error");
    Int n = byte_len(curve);
    Slice buf = slice_make(a, TYPE_BYTE, n * 2 + 1, n * 2 + 1);
    ((uint8_t *)buf.p)[0] = 4; /* Uncompressed format. */
    xx = elliptic_unmarshal(a, curve, buf, &yy);
    if (xx != NULL || yy != NULL)
        testing_t_errorf_v(t, "Unmarshal((0,0)) did not return an error");
}

static void TestInfinity(TestingT *t) {
    test_all_curves(t, infinity);
}

static void marshal(TestingT *t, EllipticCurve curve, Alloc *a) {
    BigInt *x, *y;
    generate(t, curve, a, &x, &y);
    Slice serialized = elliptic_marshal(a, curve, x, y);
    BigInt *yy;
    BigInt *xx = elliptic_unmarshal(a, curve, serialized, &yy);
    if (xx == NULL)
        testing_t_fatal_v(t, "failed to unmarshal");
    if (big_int_cmp(xx, x) != 0 || big_int_cmp(yy, y) != 0)
        testing_t_fatal_v(t, "unmarshal returned different values");
}

static void TestMarshal(TestingT *t) {
    test_all_curves(t, marshal);
}

static void unmarshal_to_large_coordinates(TestingT *t, EllipticCurve curve, Alloc *a) {
    /* See https://golang.org/issues/20482. */
    const BigInt *p = elliptic_curve_params(curve)->p;
    Int n = (big_int_bit_len(p) + 7) / 8;

    /* Set x to be greater than curve's parameter P -- specifically, to P+5.
     * Set y to mod_sqrt(x^3 - 3x + B)) so that (x mod P = 5 , y) is on the
     * curve. */
    BigInt *x = big_new_int(a, 0);
    big_int_add(x, p, big_new_int(a, 5));
    BigInt *y = polynomial(a, curve, x);
    big_int_mod_sqrt(y, y, p);

    Slice invalid = slice_make(a, TYPE_BYTE, n * 2 + 1, n * 2 + 1);
    ((uint8_t *)invalid.p)[0] = 4; /* uncompressed encoding */
    big_int_fill_bytes(x, sub(invalid, 1, 1 + n));
    big_int_fill_bytes(y, sub(invalid, 1 + n, invalid.len));

    BigInt *yy;
    BigInt *xx = elliptic_unmarshal(a, curve, invalid, &yy);
    if (xx != NULL || yy != NULL)
        testing_t_errorf_v(t, "Unmarshal accepts invalid X coordinate");

    if (same_curve(curve, elliptic_p256())) {
        /* This is a point on the curve with a small y value, small enough that
         * we can add p and still be within 32 bytes. */
        x = big(a,
                "3193192753515796370767856815220407298451758146722606822176186291"
                "5403492091210",
                10);
        y = big(a,
                "5208467867388784005506817585327037698770365050895731383201516607147",
                10);
        big_int_add(y, y, p);

        if (big_int_cmp(p, y) > 0 || big_int_bit_len(y) != 256)
            testing_t_fatal_v(t, "y not within expected range");

        /* marshal */
        big_int_fill_bytes(x, sub(invalid, 1, 1 + n));
        big_int_fill_bytes(y, sub(invalid, 1 + n, invalid.len));

        xx = elliptic_unmarshal(a, curve, invalid, &yy);
        if (xx != NULL || yy != NULL)
            testing_t_errorf_v(t, "Unmarshal accepts invalid Y coordinate");
    }
}

static void TestUnmarshalToLargeCoordinates(TestingT *t) {
    test_all_curves(t, unmarshal_to_large_coordinates);
}

static void check_is_on_curve_false(TestingT *t, EllipticCurve curve, const char *name,
                                    const BigInt *x, const BigInt *y) {
    if (elliptic_curve_is_on_curve(curve, x, y))
        testing_t_errorf_v(t, "IsOnCurve(%s) unexpectedly returned true", name);
}

/* testInvalidCoordinates tests big.Int values that are not valid field
 * elements (negative or bigger than P). They are expected to return false from
 * IsOnCurve, all other behavior is undefined. */
static void invalid_coordinates(TestingT *t, EllipticCurve curve, Alloc *a) {
    const EllipticCurveParams *params = elliptic_curve_params(curve);
    const BigInt *p = params->p;
    BigInt *x, *y;
    generate(t, curve, a, &x, &y);
    BigInt *xx = big_new_int(a, 0), *yy = big_new_int(a, 0);

    big_int_neg(xx, x);
    check_is_on_curve_false(t, curve, "-x, y", xx, y);
    big_int_neg(yy, y);
    check_is_on_curve_false(t, curve, "x, -y", x, yy);

    big_int_sub(xx, x, p);
    check_is_on_curve_false(t, curve, "x-P, y", xx, y);
    big_int_sub(yy, y, p);
    check_is_on_curve_false(t, curve, "x, y-P", x, yy);

    big_int_add(xx, x, p);
    check_is_on_curve_false(t, curve, "x+P, y", xx, y);
    big_int_add(yy, y, p);
    check_is_on_curve_false(t, curve, "x, y+P", x, yy);

    /* Check if the implementation is doing anything clever with the bit
     * size. */
    BigInt *big535 = big_new_int(a, 0);
    big_int_lsh(big535, big_new_int(a, 1), 535);
    big_int_add(xx, x, big535);
    check_is_on_curve_false(t, curve, "x+2⁵³⁵, y", xx, y);
    big_int_add(yy, y, big535);
    check_is_on_curve_false(t, curve, "x, y+2⁵³⁵", x, yy);

    /* Check that the implementation rejects x = P when (0, y) is on the
     * curve. */
    BigInt *sq = big_new_int(a, 0);
    if (big_int_mod_sqrt(sq, params->b, p) != NULL) {
        if (!elliptic_curve_is_on_curve(curve, big_new_int(a, 0), sq))
            testing_t_fatal_v(t, "(0, mod_sqrt(B)) is not on the curve?");
        check_is_on_curve_false(t, curve, "P, y", p, sq);
    }
}

static void TestInvalidCoordinates(TestingT *t) {
    test_all_curves(t, invalid_coordinates);
}

static void test_marshal_compressed(TestingT *t, EllipticCurve curve, Alloc *a,
                                    const BigInt *x, const BigInt *y, Slice want) {
    if (!elliptic_curve_is_on_curve(curve, x, y))
        testing_t_fatal_v(t, "invalid test point");
    Slice got = elliptic_marshal_compressed(a, curve, x, y);
    if (want.p != NULL && !bytes_equal(got, want))
        testing_t_errorf_v(
            t, "got unexpected MarshalCompressed result: got %x, want %x", got, want);

    BigInt *yy;
    BigInt *xx = elliptic_unmarshal_compressed(a, curve, got, &yy);
    if (xx == NULL || yy == NULL)
        testing_t_fatalf_v(t, "UnmarshalCompressed failed unexpectedly");

    if (!elliptic_curve_is_on_curve(curve, xx, yy))
        testing_t_error_v(t, "UnmarshalCompressed returned a point not on the curve");
    if (big_int_cmp(xx, x) != 0 || big_int_cmp(yy, y) != 0)
        testing_t_errorf_v(
            t, "point did not round-trip correctly: got (%s, %s), want (%s, %s)",
            hexof(a, xx), hexof(a, yy), hexof(a, x), hexof(a, y));
}

typedef struct CompressedVector {
    const char *data, *x, *y;
} CompressedVector;

static const CompressedVector compressed_vectors[] = {
    {"031e3987d9f9ea9d7dd7155a56a86b2009e1e0ab332f962d10d8beb6406ab1ad79",
     "13671033352574878777044637384712060483119675368076128232297328793087057702265",
     "66200849279091436748794323380043701364391950689352563629885086590854940586447"},
    {"021e3987d9f9ea9d7dd7155a56a86b2009e1e0ab332f962d10d8beb6406ab1ad79",
     "13671033352574878777044637384712060483119675368076128232297328793087057702265",
     "49591239931264812013903123569363872165694192725937750565648544718012157267504"},
};

static void compressed_vector(void *env, TestingT *t) {
    const CompressedVector *v = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    test_marshal_compressed(t, elliptic_p256(), a, big(a, v->x, 10), big(a, v->y, 10),
                            unhex(a, v->data));
    arena_free(&ar);
}

static void compressed_invalid(void *env, TestingT *t) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    /* x = 0x7fd4bf61763b46581fd9174d623516cf3c81edd40e29ffa2777fb6cb0ae3ce535,
     * which has no square root mod p. */
    Slice data =
        unhex(a, "02fd4bf61763b46581fd9174d623516cf3c81edd40e29ffa2777fb6cb0ae3ce535");
    BigInt *y;
    BigInt *x = elliptic_unmarshal_compressed(a, elliptic_p256(), data, &y);
    if (x != NULL || y != NULL)
        testing_t_error_v(t, "expected an error for invalid encoding");
    arena_free(&ar);
}

static void marshal_compressed(TestingT *t, EllipticCurve curve, Alloc *a) {
    BigInt *x, *y;
    generate(t, curve, a, &x, &y);
    test_marshal_compressed(t, curve, a, x, y, slice_nil(TYPE_BYTE));
}

static void TestMarshalCompressed(TestingT *t) {
    testing_t_run(t, BURROW_S("P-256/03"),
                  BURROW_FN(TestingTFunc, compressed_vector,
                            (void *)(uintptr_t)&compressed_vectors[0]));
    testing_t_run(t, BURROW_S("P-256/02"),
                  BURROW_FN(TestingTFunc, compressed_vector,
                            (void *)(uintptr_t)&compressed_vectors[1]));
    testing_t_run(t, BURROW_S("Invalid"),
                  BURROW_FN(TestingTFunc, compressed_invalid, NULL));
    if (testing_short())
        testing_t_skip_v(t, "skipping other curves on short test");
    test_all_curves(t, marshal_compressed);
}

static void large_is_on_curve(TestingT *t, EllipticCurve curve, Alloc *a) {
    BigInt *large = big_new_int(a, 1);
    big_int_lsh(large, large, 1000);
    if (elliptic_curve_is_on_curve(curve, large, large))
        testing_t_errorf_v(t, "(2^1000, 2^1000) is reported on the curve");
}

static void TestLargeIsOnCurve(TestingT *t) {
    test_all_curves(t, large_is_on_curve);
}

/* ------------------------------------------------------------------ P-224 */

typedef struct BaseMultTest {
    const char *k;
    const char *x, *y;
} BaseMultTest;

static const BaseMultTest p224_base_mult_tests[] = {
    {"1", "b70e0cbd6bb4bf7f321390b94a03c1d356c21122343280d6115c1d21",
     "bd376388b5f723fb4c22dfe6cd4375a05a07476444d5819985007e34"},
    {"2", "706a46dc76dcb76798e60e6d89474788d16dc18032d268fd1a704fa6",
     "1c2b76a7bc25e7702a704fa986892849fca629487acf3709d2e4e8bb"},
    {"3", "df1b1d66a551d0d31eff822558b9d2cc75c2180279fe0d08fd896d04",
     "a3f7f03cadd0be444c0aa56830130ddf77d317344e1af3591981a925"},
    {"4", "ae99feebb5d26945b54892092a8aee02912930fa41cd114e40447301",
     "482580a0ec5bc47e88bc8c378632cd196cb3fa058a7114eb03054c9"},
    {"5", "31c49ae75bce7807cdff22055d94ee9021fedbb5ab51c57526f011aa",
     "27e8bff1745635ec5ba0c9f1c2ede15414c6507d29ffe37e790a079b"},
    {"6", "1f2483f82572251fca975fea40db821df8ad82a3c002ee6c57112408",
     "89faf0ccb750d99b553c574fad7ecfb0438586eb3952af5b4b153c7e"},
    {"7", "db2f6be630e246a5cf7d99b85194b123d487e2d466b94b24a03c3e28",
     "f3a30085497f2f611ee2517b163ef8c53b715d18bb4e4808d02b963"},
    {"8", "858e6f9cc6c12c31f5df124aa77767b05c8bc021bd683d2b55571550",
     "46dcd3ea5c43898c5c5fc4fdac7db39c2f02ebee4e3541d1e78047a"},
    {"9", "2fdcccfee720a77ef6cb3bfbb447f9383117e3daa4a07e36ed15f78d",
     "371732e4f41bf4f7883035e6a79fcedc0e196eb07b48171697517463"},
    {"10", "aea9e17a306517eb89152aa7096d2c381ec813c51aa880e7bee2c0fd",
     "39bb30eab337e0a521b6cba1abe4b2b3a3e524c14a3fe3eb116b655f"},
    {"11", "ef53b6294aca431f0f3c22dc82eb9050324f1d88d377e716448e507c",
     "20b510004092e96636cfb7e32efded8265c266dfb754fa6d6491a6da"},
    {"12", "6e31ee1dc137f81b056752e4deab1443a481033e9b4c93a3044f4f7a",
     "207dddf0385bfdeab6e9acda8da06b3bbef224a93ab1e9e036109d13"},
    {"13", "34e8e17a430e43289793c383fac9774247b40e9ebd3366981fcfaeca",
     "252819f71c7fb7fbcb159be337d37d3336d7feb963724fdfb0ecb767"},
    {"14", "a53640c83dc208603ded83e4ecf758f24c357d7cf48088b2ce01e9fa",
     "d5814cd724199c4a5b974a43685fbf5b8bac69459c9469bc8f23ccaf"},
    {"15", "baa4d8635511a7d288aebeedd12ce529ff102c91f97f867e21916bf9",
     "979a5f4759f80f4fb4ec2e34f5566d595680a11735e7b61046127989"},
    {"16", "b6ec4fe1777382404ef679997ba8d1cc5cd8e85349259f590c4c66d",
     "3399d464345906b11b00e363ef429221f2ec720d2f665d7dead5b482"},
    {"17", "b8357c3a6ceef288310e17b8bfeff9200846ca8c1942497c484403bc",
     "ff149efa6606a6bd20ef7d1b06bd92f6904639dce5174db6cc554a26"},
    {"18", "c9ff61b040874c0568479216824a15eab1a838a797d189746226e4cc",
     "ea98d60e5ffc9b8fcf999fab1df7e7ef7084f20ddb61bb045a6ce002"},
    {"19", "a1e81c04f30ce201c7c9ace785ed44cc33b455a022f2acdbc6cae83c",
     "dcf1f6c3db09c70acc25391d492fe25b4a180babd6cea356c04719cd"},
    {"20", "fcc7f2b45df1cd5a3c0c0731ca47a8af75cfb0347e8354eefe782455",
     "d5d7110274cba7cdee90e1a8b0d394c376a5573db6be0bf2747f530"},
    {"112233445566778899", "61f077c6f62ed802dad7c2f38f5c67f2cc453601e61bd076bb46179e",
     "2272f9e9f5933e70388ee652513443b5e289dd135dcc0d0299b225e4"},
    {"112233445566778899112233445566778899",
     "29895f0af496bfc62b6ef8d8a65c88c613949b03668aab4f0429e35",
     "3ea6e53f9a841f2019ec24bde1a75677aa9b5902e61081c01064de93"},
    {"6950511619965839450988900688150712778015737983940691968051900319680",
     "ab689930bcae4a4aa5f5cb085e823e8ae30fd365eb1da4aba9cf0379",
     "3345a121bbd233548af0d210654eb40bab788a03666419be6fbd34e7"},
    {"13479972933410060327035789020509431695094902435494295338570602119423",
     "bdb6a8817c1f89da1c2f3dd8e97feb4494f2ed302a4ce2bc7f5f4025",
     "4c7020d57c00411889462d77a5438bb4e97d177700bf7243a07f1680"},
    {"13479971751745682581351455311314208093898607229429740618390390702079",
     "d58b61aa41c32dd5eba462647dba75c5d67c83606c0af2bd928446a9",
     "d24ba6a837be0460dd107ae77725696d211446c5609b4595976b16bd"},
    {"13479972931865328106486971546324465392952975980343228160962702868479",
     "dc9fa77978a005510980e929a1485f63716df695d7a0c18bb518df03",
     "ede2b016f2ddffc2a8c015b134928275ce09e5661b7ab14ce0d1d403"},
    {"11795773708834916026404142434151065506931607341523388140225443265536",
     "499d8b2829cfb879c901f7d85d357045edab55028824d0f05ba279ba",
     "bf929537b06e4015919639d94f57838fa33fc3d952598dcdbb44d638"},
    {"784254593043826236572847595991346435467177662189391577090",
     "8246c999137186632c5f9eddf3b1b0e1764c5e8bd0e0d8a554b9cb77",
     "e80ed8660bc1cb17ac7d845be40a7a022d3306f116ae9f81fea65947"},
    {"13479767645505654746623887797783387853576174193480695826442858012671",
     "6670c20afcceaea672c97f75e2e9dd5c8460e54bb38538ebb4bd30eb",
     "f280d8008d07a4caf54271f993527d46ff3ff46fd1190a3f1faa4f74"},
    {"205688069665150753842126177372015544874550518966168735589597183",
     "eca934247425cfd949b795cb5ce1eff401550386e28d1a4c5a8eb",
     "d4c01040dba19628931bc8855370317c722cbd9ca6156985f1c2e9ce"},
    {"13479966930919337728895168462090683249159702977113823384618282123295",
     "ef353bf5c73cd551b96d596fbc9a67f16d61dd9fe56af19de1fba9cd",
     "21771b9cdce3e8430c09b3838be70b48c21e15bc09ee1f2d7945b91f"},
    {"50210731791415612487756441341851895584393717453129007497216",
     "4036052a3091eb481046ad3289c95d3ac905ca0023de2c03ecd451cf",
     "d768165a38a2b96f812586a9d59d4136035d9c853a5bf2e1c86a4993"},
    {"26959946667150639794667015087019625940457807714424391721682722368041",
     "fcc7f2b45df1cd5a3c0c0731ca47a8af75cfb0347e8354eefe782455",
     "f2a28eefd8b345832116f1e574f2c6b2c895aa8c24941f40d8b80ad1"},
    {"26959946667150639794667015087019625940457807714424391721682722368042",
     "a1e81c04f30ce201c7c9ace785ed44cc33b455a022f2acdbc6cae83c",
     "230e093c24f638f533dac6e2b6d01da3b5e7f45429315ca93fb8e634"},
    {"26959946667150639794667015087019625940457807714424391721682722368043",
     "c9ff61b040874c0568479216824a15eab1a838a797d189746226e4cc",
     "156729f1a003647030666054e208180f8f7b0df2249e44fba5931fff"},
    {"26959946667150639794667015087019625940457807714424391721682722368044",
     "b8357c3a6ceef288310e17b8bfeff9200846ca8c1942497c484403bc",
     "eb610599f95942df1082e4f9426d086fb9c6231ae8b24933aab5db"},
    {"26959946667150639794667015087019625940457807714424391721682722368045",
     "b6ec4fe1777382404ef679997ba8d1cc5cd8e85349259f590c4c66d",
     "cc662b9bcba6f94ee4ff1c9c10bd6ddd0d138df2d099a282152a4b7f"},
    {"26959946667150639794667015087019625940457807714424391721682722368046",
     "baa4d8635511a7d288aebeedd12ce529ff102c91f97f867e21916bf9",
     "6865a0b8a607f0b04b13d1cb0aa992a5a97f5ee8ca1849efb9ed8678"},
    {"26959946667150639794667015087019625940457807714424391721682722368047",
     "a53640c83dc208603ded83e4ecf758f24c357d7cf48088b2ce01e9fa",
     "2a7eb328dbe663b5a468b5bc97a040a3745396ba636b964370dc3352"},
    {"26959946667150639794667015087019625940457807714424391721682722368048",
     "34e8e17a430e43289793c383fac9774247b40e9ebd3366981fcfaeca",
     "dad7e608e380480434ea641cc82c82cbc92801469c8db0204f13489a"},
    {"26959946667150639794667015087019625940457807714424391721682722368049",
     "6e31ee1dc137f81b056752e4deab1443a481033e9b4c93a3044f4f7a",
     "df82220fc7a4021549165325725f94c3410ddb56c54e161fc9ef62ee"},
    {"26959946667150639794667015087019625940457807714424391721682722368050",
     "ef53b6294aca431f0f3c22dc82eb9050324f1d88d377e716448e507c",
     "df4aefffbf6d1699c930481cd102127c9a3d992048ab05929b6e5927"},
    {"26959946667150639794667015087019625940457807714424391721682722368051",
     "aea9e17a306517eb89152aa7096d2c381ec813c51aa880e7bee2c0fd",
     "c644cf154cc81f5ade49345e541b4d4b5c1adb3eb5c01c14ee949aa2"},
    {"26959946667150639794667015087019625940457807714424391721682722368052",
     "2fdcccfee720a77ef6cb3bfbb447f9383117e3daa4a07e36ed15f78d",
     "c8e8cd1b0be40b0877cfca1958603122f1e6914f84b7e8e968ae8b9e"},
    {"26959946667150639794667015087019625940457807714424391721682722368053",
     "858e6f9cc6c12c31f5df124aa77767b05c8bc021bd683d2b55571550",
     "fb9232c15a3bc7673a3a03b0253824c53d0fd1411b1cabe2e187fb87"},
    {"26959946667150639794667015087019625940457807714424391721682722368054",
     "db2f6be630e246a5cf7d99b85194b123d487e2d466b94b24a03c3e28",
     "f0c5cff7ab680d09ee11dae84e9c1072ac48ea2e744b1b7f72fd469e"},
    {"26959946667150639794667015087019625940457807714424391721682722368055",
     "1f2483f82572251fca975fea40db821df8ad82a3c002ee6c57112408",
     "76050f3348af2664aac3a8b05281304ebc7a7914c6ad50a4b4eac383"},
    {"26959946667150639794667015087019625940457807714424391721682722368056",
     "31c49ae75bce7807cdff22055d94ee9021fedbb5ab51c57526f011aa",
     "d817400e8ba9ca13a45f360e3d121eaaeb39af82d6001c8186f5f866"},
    {"26959946667150639794667015087019625940457807714424391721682722368057",
     "ae99feebb5d26945b54892092a8aee02912930fa41cd114e40447301",
     "fb7da7f5f13a43b81774373c879cd32d6934c05fa758eeb14fcfab38"},
    {"26959946667150639794667015087019625940457807714424391721682722368058",
     "df1b1d66a551d0d31eff822558b9d2cc75c2180279fe0d08fd896d04",
     "5c080fc3522f41bbb3f55a97cfecf21f882ce8cbb1e50ca6e67e56dc"},
    {"26959946667150639794667015087019625940457807714424391721682722368059",
     "706a46dc76dcb76798e60e6d89474788d16dc18032d268fd1a704fa6",
     "e3d4895843da188fd58fb0567976d7b50359d6b78530c8f62d1b1746"},
    {"26959946667150639794667015087019625940457807714424391721682722368060",
     "b70e0cbd6bb4bf7f321390b94a03c1d356c21122343280d6115c1d21",
     "42c89c774a08dc04b3dd201932bc8a5ea5f8b89bbb2a7e667aff81cd"},
};

#define NP224 ((int)(sizeof p224_base_mult_tests / sizeof p224_base_mult_tests[0]))

static void p224_base_mult(TestingT *t, EllipticCurve p224) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (int i = 0; i < NP224; i++) {
        const BaseMultTest *e = &p224_base_mult_tests[i];
        BigInt *k = big_new_int(a, 0);
        bool ok = false;
        big_int_set_string(k, str_from_cstr(e->k), 10, &ok);
        if (!ok)
            testing_t_errorf_v(t, "%d: bad value for k: %s", i, str_from_cstr(e->k));
        BigInt *y;
        BigInt *x = elliptic_curve_scalar_base_mult(p224, a, big_int_bytes(k, a), &y);
        Str gx = hexof(a, x), gy = hexof(a, y);
        if (!str_eq(gx, str_from_cstr(e->x)) || !str_eq(gy, str_from_cstr(e->y)))
            testing_t_errorf_v(
                t, "%d: bad output for k=%s: got (%s, %s), want (%s, %s)", i,
                str_from_cstr(e->k), gx, gy, str_from_cstr(e->x), str_from_cstr(e->y));
        if (testing_short() && i > 5)
            break;
    }
    arena_free(&ar);
}

static void TestP224BaseMult(TestingT *t) {
    p224_base_mult(t, elliptic_p224());
}

static void TestP224GenericBaseMult(TestingT *t) {
    /* We use the P224 CurveParams directly in order to test the generic
     * implementation. */
    EllipticCurveParams d;
    p224_base_mult(t, generic_params_for_curve(&d, elliptic_p224()));
}

static void TestP224Overflow(TestingT *t) {
    /* This tests for a specific bug in the P224 implementation. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    EllipticCurve p224 = elliptic_p224();
    Slice point_data = unhex(a, "049B535B45FB0A2072398A6831834624C7E32CCFD5A4B933BCEAF7"
                                "7F1DD945E08BBE5178F5EDF5E733388F196D2A631D2E075BB16CBF"
                                "EEA15B");
    BigInt *y;
    BigInt *x = elliptic_unmarshal(a, p224, point_data, &y);
    if (x == NULL || !elliptic_curve_is_on_curve(p224, x, y))
        testing_t_error_v(t, "P224 failed to validate a correct point");
    arena_free(&ar);
}

/* ------------------------------------------------------------------ P-256 */

typedef struct ScalarMultTest {
    const char *k;
    const char *x_in, *y_in;
    const char *x_out, *y_out;
} ScalarMultTest;

static const ScalarMultTest p256_mult_tests[] = {
    {
        "2a265f8bcbdcaf94d58519141e578124cb40d64a501fba9c11847b28965bc737",
        "023819813ac969847059028ea88a1f30dfbcde03fc791d3a252c6b41211882ea",
        "f93e4ae433cc12cf2a43fc0ef26400c0e125508224cdb649380f25479148a4ad",
        "4d4de80f1534850d261075997e3049321a0864082d24a917863366c0724f5ae3",
        "a22d2b7f7818a3563e0f7a76c9bf0921ac55e06e2e4d11795b233824b1db8cc0",
    },
    {
        "313f72ff9fe811bf573176231b286a3bdb6f1b14e05c40146590727a71c3bccd",
        "cc11887b2d66cbae8f4d306627192522932146b42f01d3c6f92bd5c8ba739b06",
        "a2f08a029cd06b46183085bae9248b0ed15b70280c7ef13a457f5af382426031",
        "831c3f6b5f762d2f461901577af41354ac5f228c2591f84f8a6e51e2e3f17991",
        "93f90934cd0ef2c698cc471c60a93524e87ab31ca2412252337f364513e43684",
    },
};

static void TestP256BaseMult(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    EllipticCurve p256 = elliptic_p256();
    EllipticCurveParams d;
    EllipticCurve p256_generic = generic_params_for_curve(&d, p256);

    BigInt *scalars[NP224 + 1];
    for (int i = 0; i < NP224; i++)
        scalars[i] = big(a, p224_base_mult_tests[i].k, 10);
    BigInt *k = big_new_int(a, 1);
    big_int_lsh(k, k, 500);
    scalars[NP224] = k;

    for (int i = 0; i <= NP224; i++) {
        Slice kb = big_int_bytes(scalars[i], a);
        BigInt *y, *y2;
        BigInt *x = elliptic_curve_scalar_base_mult(p256, a, kb, &y);
        BigInt *x2 = elliptic_curve_scalar_base_mult(p256_generic, a, kb, &y2);
        if (big_int_cmp(x, x2) != 0 || big_int_cmp(y, y2) != 0)
            testing_t_errorf_v(t, "#%d: got (%s, %s), want (%s, %s)", i, hexof(a, x),
                               hexof(a, y), hexof(a, x2), hexof(a, y2));
        if (testing_short() && i > 5)
            break;
    }
    arena_free(&ar);
}

static void TestP256Mult(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    EllipticCurve p256 = elliptic_p256();
    for (int i = 0; i < (int)(sizeof p256_mult_tests / sizeof p256_mult_tests[0]);
         i++) {
        const ScalarMultTest *e = &p256_mult_tests[i];
        BigInt *x = big(a, e->x_in, 16);
        BigInt *y = big(a, e->y_in, 16);
        BigInt *k = big(a, e->k, 16);
        BigInt *expected_x = big(a, e->x_out, 16);
        BigInt *expected_y = big(a, e->y_out, 16);
        BigInt *yy;
        BigInt *xx =
            elliptic_curve_scalar_mult(p256, a, x, y, big_int_bytes(k, a), &yy);
        if (big_int_cmp(xx, expected_x) != 0 || big_int_cmp(yy, expected_y) != 0)
            testing_t_errorf_v(t, "#%d: got (%s, %s), want (%s, %s)", i, hexof(a, xx),
                               hexof(a, yy), hexof(a, expected_x),
                               hexof(a, expected_y));
    }
    arena_free(&ar);
}

static void TestIssue52075(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    EllipticCurve p256 = elliptic_p256();
    const BigInt *gx = elliptic_curve_params(p256)->gx;
    const BigInt *gy = elliptic_curve_params(p256)->gy;
    Slice scalar = slice_make(a, TYPE_BYTE, 33, 33);
    ((uint8_t *)scalar.p)[32] = 1;
    BigInt *y;
    BigInt *x = elliptic_curve_scalar_base_mult(p256, a, scalar, &y);
    if (big_int_cmp(x, gx) != 0 || big_int_cmp(y, gy) != 0)
        testing_t_errorf_v(t, "unexpected output (%s,%s)", hexof(a, x), hexof(a, y));
    x = elliptic_curve_scalar_mult(p256, a, gx, gy, scalar, &y);
    if (big_int_cmp(x, gx) != 0 || big_int_cmp(y, gy) != 0)
        testing_t_errorf_v(t, "unexpected output (%s,%s)", hexof(a, x), hexof(a, y));
    arena_free(&ar);
}

/* ---------------------------------------------------------- burrow's own */

static EllipticCurve panic_curve;
static Alloc *panic_alloc;
static BigInt *panic_bad;

static void do_marshal(void) {
    (void)elliptic_marshal(panic_alloc, panic_curve, panic_bad, panic_bad);
}

static void do_marshal_compressed(void) {
    (void)elliptic_marshal_compressed(panic_alloc, panic_curve, panic_bad, panic_bad);
}

static void do_add(void) {
    const EllipticCurveParams *params = elliptic_curve_params(panic_curve);
    (void)elliptic_curve_add(panic_curve, panic_alloc, params->gx, params->gy,
                             panic_bad, panic_bad, NULL);
}

static void do_double(void) {
    (void)elliptic_curve_double(panic_curve, panic_alloc, panic_bad, panic_bad, NULL);
}

static void do_scalar_mult(void) {
    uint8_t one[] = {1};
    (void)elliptic_curve_scalar_mult(panic_curve, panic_alloc, panic_bad, panic_bad,
                                     slice_from(one, 1, 1, TYPE_BYTE), NULL);
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

/* The point (1, 1) is on none of the curves, and each operation panics on it
 * with Go's message, the NIST curves with their own and the generic code with
 * panicIfNotOnCurve's. */
static void TestPanics(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    setup_curves();
    panic_alloc = a;
    panic_bad = big_new_int(a, 1);
    const char *invalid = "crypto/elliptic: attempted operation on invalid point";
    for (int i = 0; i < 8; i++) {
        panic_curve = all_curves[i].curve;
        bool nist = i % 2 == 0;
        struct {
            const char *op;
            void (*f)(void);
            const char *want;
        } cases[] = {
            {"Marshal", do_marshal, invalid},
            {"MarshalCompressed", do_marshal_compressed, invalid},
            {"Add", do_add,
             nist ? "crypto/elliptic: Add was called on an invalid point" : invalid},
            {"Double", do_double,
             nist ? "crypto/elliptic: Double was called on an invalid point" : invalid},
            {"ScalarMult", do_scalar_mult,
             nist ? "crypto/elliptic: ScalarMult was called on an invalid point"
                  : invalid},
        };
        for (size_t j = 0; j < sizeof cases / sizeof cases[0]; j++) {
            Str got = panic_of(a, cases[j].f);
            if (!str_eq(got, str_from_cstr(cases[j].want)))
                testing_t_errorf_v(t, "%s: %s panicked with %q, want %q",
                                   str_from_cstr(all_curves[i].name),
                                   str_from_cstr(cases[j].op), got,
                                   str_from_cstr(cases[j].want));
        }
    }
    arena_free(&ar);
}

static Int failing_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    *err = errors_new(error_allocator(), BURROW_S("no randomness here"));
    return 0;
}

static const IoReaderVT failing_reader_vt = {NULL, failing_read};

/* A reader that fails gives its error back, with no key. */
static void TestGenerateKeyError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    IoReader r = {&failing_reader_vt, NULL};
    BigInt *x = big_new_int(a, 7), *y = x;
    Error err = BURROW_NO_ERROR;
    Slice priv = elliptic_generate_key(a, elliptic_p256(), r, &x, &y, &err);
    if (!BURROW_FAILED(err) || !str_eq(error_text(err), BURROW_S("no randomness here")))
        testing_t_errorf_v(t, "err = %s, want no randomness here", error_text(err));
    if (priv.p != NULL || x != NULL || y != NULL)
        testing_t_error_v(t, "GenerateKey gave a key with its error");

    /* And a nil reader is crypto/rand's. */
    priv = elliptic_generate_key(a, elliptic_p256(), (IoReader){0}, &x, &y, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (priv.len != 32 || !elliptic_curve_is_on_curve(elliptic_p256(), x, y))
        testing_t_errorf_v(t, "GenerateKey(nil) gave a %d byte key", priv.len);
    arena_free(&ar);
}

/* y may be NULL anywhere a point comes back, and the heap works as the
 * allocator. */
static void TestNilY(TestingT *t) {
    setup_curves();
    uint8_t two[] = {2};
    Slice k = slice_from(two, 1, 1, TYPE_BYTE);
    for (int i = 0; i < 8; i++) {
        EllipticCurve c = all_curves[i].curve;
        BigInt *y;
        BigInt *want = elliptic_curve_scalar_base_mult(c, NULL, k, &y);
        const EllipticCurveParams *params = elliptic_curve_params(c);
        BigInt *x = elliptic_curve_double(c, NULL, params->gx, params->gy, NULL);
        if (big_int_cmp(x, want) != 0)
            testing_t_errorf_v(t, "%s: Double(G) and ScalarBaseMult(2) differ",
                               str_from_cstr(all_curves[i].name));
        Slice enc = elliptic_marshal(NULL, c, want, y);
        BigInt *ux = elliptic_unmarshal(NULL, c, enc, NULL);
        if (ux == NULL || big_int_cmp(ux, want) != 0)
            testing_t_errorf_v(t, "%s: Unmarshal with no y failed",
                               str_from_cstr(all_curves[i].name));
        BigInt *tmp[] = {want, y, x, ux};
        for (size_t j = 0; j < sizeof tmp / sizeof tmp[0]; j++) {
            if (tmp[j] == NULL)
                continue;
            big_int_free(tmp[j]);
            mem_free(heap_allocator(), tmp[j], sizeof(BigInt), _Alignof(BigInt));
        }
        mem_free(heap_allocator(), enc.p, (size_t)enc.cap, 1);
    }
}

/* ------------------------------------------------------------ benchmarks */

typedef void (*CurveBench)(TestingB *b, EllipticCurve curve, Alloc *a);

typedef struct BenchRun {
    CurveBench f;
    EllipticCurve curve;
} BenchRun;

static void bench_run(void *env, TestingB *b) {
    BenchRun *r = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    r->f(b, r->curve, arena_allocator(&ar));
    arena_free(&ar);
}

static void benchmark_all_curves(TestingB *b, CurveBench f) {
    static const char *names[] = {"P256", "P224", "P384", "P521"};
    EllipticCurve cs[] = {elliptic_p256(), elliptic_p224(), elliptic_p384(),
                          elliptic_p521()};
    for (int i = 0; i < 4; i++) {
        BenchRun r = {f, cs[i]};
        testing_b_run(b, str_from_cstr(names[i]),
                      BURROW_FN(TestingBFunc, bench_run, &r));
    }
}

static void scalar_base_mult(TestingB *b, EllipticCurve curve, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    Slice priv = elliptic_generate_key(a, curve, crypto_rand_reader, NULL, NULL, &err);
    Arena loop;
    arena_init(&loop, NULL, 0);
    Alloc *la = arena_allocator(&loop);
    testing_b_report_allocs(b);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        BigInt *x = elliptic_curve_scalar_base_mult(curve, la, priv, NULL);
        ((uint8_t *)priv.p)[0] ^= (uint8_t)BURROW_AT(BigWord, big_int_bits(x), 0);
        arena_reset(&loop);
    }
    arena_free(&loop);
}

static void BenchmarkScalarBaseMult(TestingB *b) {
    benchmark_all_curves(b, scalar_base_mult);
}

static void scalar_mult(TestingB *b, EllipticCurve curve, Alloc *a) {
    Error err = BURROW_NO_ERROR;
    BigInt *x, *y;
    (void)elliptic_generate_key(a, curve, crypto_rand_reader, &x, &y, &err);
    Slice priv = elliptic_generate_key(a, curve, crypto_rand_reader, NULL, NULL, &err);
    BigInt *px = big_new_int(a, 0), *py = big_new_int(a, 0);
    big_int_set(px, x);
    big_int_set(py, y);
    Arena loop;
    arena_init(&loop, NULL, 0);
    Alloc *la = arena_allocator(&loop);
    testing_b_report_allocs(b);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        BigInt *ny;
        BigInt *nx = elliptic_curve_scalar_mult(curve, la, px, py, priv, &ny);
        big_int_set(px, nx);
        big_int_set(py, ny);
        arena_reset(&loop);
    }
    arena_free(&loop);
}

static void BenchmarkScalarMult(TestingB *b) {
    benchmark_all_curves(b, scalar_mult);
}

#define TESTS(X)                                                                       \
    X(TestOnCurve)                                                                     \
    X(TestOffCurve)                                                                    \
    X(TestInfinity)                                                                    \
    X(TestMarshal)                                                                     \
    X(TestUnmarshalToLargeCoordinates)                                                 \
    X(TestInvalidCoordinates)                                                          \
    X(TestMarshalCompressed)                                                           \
    X(TestLargeIsOnCurve)                                                              \
    X(TestP224BaseMult)                                                                \
    X(TestP224GenericBaseMult)                                                         \
    X(TestP224Overflow)                                                                \
    X(TestP256BaseMult)                                                                \
    X(TestP256Mult)                                                                    \
    X(TestIssue52075)                                                                  \
    X(TestPanics)                                                                      \
    X(TestGenerateKeyError)                                                            \
    X(TestNilY)                                                                        \
    X(BenchmarkScalarBaseMult)                                                         \
    X(BenchmarkScalarMult)

TESTING_MAIN(TESTS)
