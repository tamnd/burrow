/* Derived from Go's src/crypto/internal/fips140test/nistec_ordinv_test.go and
 * nistec_ordinv_fips140v1.28_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2022 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/math/big.h"

#include "../src/crypto/nistec.h"

#include <stdint.h>
#include <string.h>

static uint64_t be_uint64(const Byte *b) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = v << 8 | b[i];
    return v;
}

static void be_put_uint64(Byte *b, uint64_t v) {
    for (int i = 7; i >= 0; i--) {
        b[i] = (Byte)v;
        v >>= 8;
    }
}

static void bytesToLimbs(uint64_t l[4], const Byte b[32]) {
    l[0] = be_uint64(b + 24);
    l[1] = be_uint64(b + 16);
    l[2] = be_uint64(b + 8);
    l[3] = be_uint64(b);
}

static void limbsToBytes(Byte b[32], const uint64_t l[4]) {
    be_put_uint64(b + 24, l[0]);
    be_put_uint64(b + 16, l[1]);
    be_put_uint64(b + 8, l[2]);
    be_put_uint64(b, l[3]);
}

static void p256OrdInverse(TestingT *t, uint64_t k[4]) {
    (void)t;
    burrow__p256_ord_inverse(k);
}

static bool limbs_equal(const uint64_t k[4], const Byte want[32]) {
    Byte got[32];
    limbsToBytes(got, k);
    return memcmp(got, want, 32) == 0;
}

static void TestP256OrdInverse(TestingT *t) {
    const BigInt *N = elliptic_curve_params(elliptic_p256())->n;

    /* inv(0) is expected to be 0. */
    Byte zero[32] = {0};
    uint64_t k[4];
    bytesToLimbs(k, zero);
    p256OrdInverse(t, k);
    if (!limbs_equal(k, zero))
        testing_t_error_v(t, "unexpected output for inv(0)");

    /* inv(N) is also 0 mod N. */
    Byte input[32];
    Slice in = slice_from(input, 32, 32, TYPE_BYTE);
    big_int_fill_bytes(N, in);
    bytesToLimbs(k, input);
    p256OrdInverse(t, k);
    if (!limbs_equal(k, zero))
        testing_t_error_v(t, "unexpected output for inv(N)");

    /* Check inv(1) and inv(N+1) against math/big */
    BigInt v = {0}, inv = {0};
    Byte exp[32];
    big_int_mod_inverse(&inv, big_int_set_int64(&v, 1), N);
    big_int_fill_bytes(&inv, slice_from(exp, 32, 32, TYPE_BYTE));
    big_int_fill_bytes(big_int_set_int64(&v, 1), in);
    bytesToLimbs(k, input);
    p256OrdInverse(t, k);
    if (!limbs_equal(k, exp))
        testing_t_error_v(t, "unexpected output for inv(1)");

    big_int_fill_bytes(big_int_add(&v, N, big_int_set_int64(&v, 1)), in);
    bytesToLimbs(k, input);
    p256OrdInverse(t, k);
    if (!limbs_equal(k, exp))
        testing_t_error_v(t, "unexpected output for inv(N+1)");

    /* Check inv(20) and inv(N+20) against math/big */
    big_int_mod_inverse(&inv, big_int_set_int64(&v, 20), N);
    big_int_fill_bytes(&inv, slice_from(exp, 32, 32, TYPE_BYTE));
    big_int_fill_bytes(big_int_set_int64(&v, 20), in);
    bytesToLimbs(k, input);
    p256OrdInverse(t, k);
    if (!limbs_equal(k, exp))
        testing_t_error_v(t, "unexpected output for inv(20)");

    big_int_fill_bytes(big_int_add(&v, N, big_int_set_int64(&v, 20)), in);
    bytesToLimbs(k, input);
    p256OrdInverse(t, k);
    if (!limbs_equal(k, exp))
        testing_t_error_v(t, "unexpected output for inv(N+20)");

    /* Check inv(2^256-1) against math/big */
    BigInt bigInput = {0}, one = {0};
    big_int_set_int64(&one, 1);
    big_int_lsh(&bigInput, &one, 256);
    big_int_sub(&bigInput, &bigInput, &one);
    big_int_mod_inverse(&inv, &bigInput, N);
    big_int_fill_bytes(&inv, slice_from(exp, 32, 32, TYPE_BYTE));
    big_int_fill_bytes(&bigInput, in);
    bytesToLimbs(k, input);
    p256OrdInverse(t, k);
    if (!limbs_equal(k, exp))
        testing_t_error_v(t, "unexpected output for inv(2^256-1)");
    big_int_free(&bigInput);
    big_int_free(&one);
    big_int_free(&v);
    big_int_free(&inv);
}

#define TESTS(X) X(TestP256OrdInverse)

TESTING_MAIN(TESTS)
