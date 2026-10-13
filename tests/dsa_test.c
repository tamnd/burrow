/* Derived from Go's src/crypto/dsa/dsa_test.go and dsa_wycheproof_test.go.
 * Go source: go1.27.1.
 *
 * The Wycheproof vectors Go fetches as a module are in dsa_test_gen.h, which
 * tools/gen-dsa-tests.sh writes, with each public key already out of its DER
 * as Go's x509 would have taken it, since there is no x509 here yet.
 *
 * Go has no benchmarks for this package. BenchmarkSign and BenchmarkVerify are
 * burrow's, as are the tests after TestDSAWycheproof, which check the errors
 * and the allocator.
 *
 * Copyright 2011 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/burrow.h"
#include "burrow/crypto.h"
#include "burrow/crypto/dsa.h"
#include "burrow/crypto/rand.h"
#include "burrow/encoding/hex.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/mem/heap.h"
#include "burrow/slices.h"

#include "../src/crypto/cryptobyte.h"

#include "dsa_test_gen.h"

#include <stdint.h>
#include <string.h>

static Slice cs(const char *s) {
    Int n = (Int)strlen(s);
    return slice_from((void *)(uintptr_t)s, n, n, TYPE_BYTE);
}

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("invalid hex"));
    return b;
}

/* fromHex. */
static BigInt *from_hex(Alloc *a, const char *s) {
    BigInt *r = big_new_int(a, 0);
    bool ok = false;
    big_int_set_string(r, str_from_cstr(s), 16, &ok);
    if (!ok)
        panic_str(BURROW_S("bad hex"));
    return r;
}

static void test_sign_and_verify(TestingT *t, int i, const DsaPrivateKey *priv) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice hashed = cs("testing");
    Error err = BURROW_NO_ERROR;
    BigInt *s = NULL;
    BigInt *r = dsa_sign(a, crypto_rand_reader, priv, hashed, &s, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%d: error signing: %s", i, error_text(err));
        arena_free(&ar);
        return;
    }

    if (!dsa_verify(&priv->public_key, hashed, r, s))
        testing_t_errorf_v(t, "%d: Verify failed", i);
    arena_free(&ar);
}

static void test_parameter_generation(TestingT *t, DsaParameterSizes sizes, Int l,
                                      Int n) {
    testing_t_helper(t);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DsaPrivateKey priv = {0};
    DsaParameters *params = &priv.public_key.parameters;

    Error err = dsa_generate_parameters(params, a, crypto_rand_reader, sizes);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%d: %s", (int)sizes, error_text(err));
        arena_free(&ar);
        return;
    }

    if (big_int_bit_len(params->p) != l)
        testing_t_errorf_v(t, "%d: params.BitLen got:%d want:%d", (int)sizes,
                           big_int_bit_len(params->p), l);

    if (big_int_bit_len(params->q) != n)
        testing_t_errorf_v(t, "%d: q.BitLen got:%d want:%d", (int)sizes,
                           big_int_bit_len(params->q), l);

    BigInt *one = big_new_int(a, 1);
    BigInt *pm1 = big_int_sub(big_new_int(a, 0), params->p, one);
    BigInt *rem = big_new_int(a, 0);
    BigInt *quo = big_int_div_mod(big_new_int(a, 0), pm1, params->q, rem);
    if (big_int_sign(rem) != 0)
        testing_t_errorf_v(t, "%d: p-1 mod q != 0", (int)sizes);
    BigInt *x = big_int_exp(big_new_int(a, 0), params->g, quo, params->p);
    if (big_int_cmp(x, one) == 0)
        testing_t_errorf_v(t, "%d: invalid generator", (int)sizes);

    err = dsa_generate_key(&priv, a, crypto_rand_reader);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "error generating key: %s", error_text(err));
        arena_free(&ar);
        return;
    }

    test_sign_and_verify(t, (int)sizes, &priv);
    arena_free(&ar);
}

static void TestParameterGeneration(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping parameter generation test in short mode");

    test_parameter_generation(t, DSA_L1024N160, 1024, 160);
    test_parameter_generation(t, DSA_L2048N224, 2048, 224);
    test_parameter_generation(t, DSA_L2048N256, 2048, 256);
    test_parameter_generation(t, DSA_L3072N256, 3072, 256);
}

#define TEST_P                                                                         \
    "A9B5B793FB4785793D246BAE77E8FF63CA52F442DA763C440259919FE1BC1D6065A9350637A04F75" \
    "A2F039401D49F08E066C4D275A5A65DA5684BC563C14289D7AB8A67163BFBF79D85972619AD2CFF5" \
    "5AB0EE77A9002B0EF96293BDD0F42685EBB2C66C327079F6C98000FBCB79AACDE1BC6F9D5C7B1A97" \
    "E3D9D54ED7951FEF"
#define TEST_Q "E1D3391245933D68A0714ED34BBCB7A1F422B9C1"
#define TEST_G                                                                         \
    "634364FC25248933D01D1993ECABD0657CC0CB2CEED7ED2E3E8AECDFCDC4A25C3B15E9E3B163ACA2" \
    "984B5539181F3EFF1A5E8903D71D5B95DA4F27202B77D2C44B430BB53741A8D59A8F86887525C9F2" \
    "A6A5980A195EAA7F2FF910064301DEF89D3AA213E1FAC7768D89365318E370AF54A112EFBA9246D9" \
    "158386BA1B4EEFDA"
#define TEST_Y                                                                         \
    "32969E5780CFE1C849A1C276D7AEB4F38A23B591739AA2FE197349AEEBD31366AEE5EB7E6C6DDB7C" \
    "57D02432B30DB5AA66D9884299FAA72568944E4EEDC92EA3FBC6F39F53412FBCC563208F7C15B737" \
    "AC8910DBC2D9C9B8C001E72FDC40EB694AB1F06A5A2DBD18D9E36C66F31F566742F11EC0A52E9F7B" \
    "89355C02FB5D32D2"
#define TEST_X "5078D4D29795CBE76D3AACFE48C9AF0BCDBEE91A"

/* The key of TestSignAndVerify, with its integers from a. */
static DsaPrivateKey test_key(Alloc *a) {
    DsaPrivateKey priv = {0};
    priv.public_key.parameters.p = from_hex(a, TEST_P);
    priv.public_key.parameters.q = from_hex(a, TEST_Q);
    priv.public_key.parameters.g = from_hex(a, TEST_G);
    priv.public_key.y = from_hex(a, TEST_Y);
    priv.x = from_hex(a, TEST_X);
    return priv;
}

static void TestSignAndVerify(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    DsaPrivateKey priv = test_key(arena_allocator(&ar));

    test_sign_and_verify(t, 0, &priv);
    arena_free(&ar);
}

static void TestSignAndVerifyWithBadPublicKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DsaPublicKey pub = {0};
    pub.parameters.p = from_hex(a, TEST_P);
    pub.parameters.q = from_hex(a, "FA");
    pub.parameters.g = from_hex(a, TEST_G);
    pub.y = from_hex(a, TEST_Y);

    if (dsa_verify(&pub, cs("testing"), from_hex(a, "2"), from_hex(a, "4")))
        testing_t_errorf_v(
            t, "Verify unexpected success with non-existent mod inverse of Q");
    arena_free(&ar);
}

static void TestSigningWithDegenerateKeys(TestingT *t) {
    /* Signing with degenerate private keys should not cause an infinite
     * loop. */
    static const struct {
        const char *p, *q, *g, *y, *x;
    } bad_keys[] = {
        {"00", "01", "00", "00", "00"},
        {"01", "ff", "00", "00", "00"},
    };

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof bad_keys / sizeof bad_keys[0]; i++) {
        DsaPrivateKey priv = {0};
        priv.public_key.parameters.p = from_hex(a, bad_keys[i].p);
        priv.public_key.parameters.q = from_hex(a, bad_keys[i].q);
        priv.public_key.parameters.g = from_hex(a, bad_keys[i].g);
        priv.public_key.y = from_hex(a, bad_keys[i].y);
        priv.x = from_hex(a, bad_keys[i].x);

        Slice hashed = cs("testing");
        Error err = BURROW_NO_ERROR;
        BigInt *s = NULL;
        (void)dsa_sign(a, crypto_rand_reader, &priv, hashed, &s, &err);
        if (BURROW_OK(err))
            testing_t_errorf_v(t, "#%d: unexpected success", (int)i);
    }
    arena_free(&ar);
}

/* ----------------------------------------------------------- Wycheproof */

/* A message or signature of a Wycheproof test, which the generator has cut
 * into pieces C99 can take as literals. */
static Slice unhex_parts(Alloc *a, const char *const parts[3]) {
    Slice b[3];
    Int n = 0;
    while (n < 3 && parts[n] != NULL) {
        b[n] = unhex(a, parts[n]);
        n++;
    }
    return slices_concat(a, b, n);
}

/* verifyASN1. */
static bool verify_asn1(const DsaPublicKey *pub, Slice hash, Slice sig) {
    BigInt r = BIG_INT(NULL), s = BIG_INT(NULL);
    CryptobyteString inner = {0};
    CryptobyteString input = sig;
    bool ok = false;
    if (cryptobyte_string_read_asn1(&input, &inner, CRYPTOBYTE_ASN1_SEQUENCE) &&
        cryptobyte_string_empty(input) &&
        cryptobyte_string_read_asn1_integer_big(&inner, &r) &&
        cryptobyte_string_read_asn1_integer_big(&inner, &s) &&
        cryptobyte_string_empty(inner))
        ok = dsa_verify(pub, hash, &r, &s);
    big_int_free(&r);
    big_int_free(&s);
    return ok;
}

static void TestDSAWycheproof(TestingT *t) {
    Arena ar, keys;
    arena_init(&ar, NULL, 0);
    arena_init(&keys, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    int group = -1;
    DsaPublicKey pub = {0};
    for (size_t i = 0; i < sizeof gen_wycheproof / sizeof gen_wycheproof[0]; i++) {
        const GenWycheproof *tv = &gen_wycheproof[i];
        const GenWycheproofGroup *tg = &gen_wycheproof_groups[tv->group];
        if (tv->group != group) {
            arena_reset(&keys);
            Alloc *ka = arena_allocator(&keys);
            pub.parameters.p = from_hex(ka, tg->p);
            pub.parameters.q = from_hex(ka, tg->q);
            pub.parameters.g = from_hex(ka, tg->g);
            pub.y = from_hex(ka, tg->y);
            group = tv->group;
        }

        Hash h = crypto_hash_new(tg->hash, a);
        hash_write(h, unhex_parts(a, tv->msg), NULL);
        Slice hashed = hash_sum(a, h, (Slice){0});
        /* Truncate to the byte-length of the subgroup (Q). */
        hashed = slice_sub(hashed, 0, big_int_bit_len(pub.parameters.q) / 8);
        bool got = verify_asn1(&pub, hashed, unhex_parts(a, tv->sig));
        if (got != tv->want)
            testing_t_errorf_v(t, "%s #%d %s: wanted success: %t", tg->file, tv->tc_id,
                               tv->comment, tv->want);
        arena_reset(&ar);
    }

    arena_free(&keys);
    arena_free(&ar);
}

/* ------------------------------------------------------- burrow's own */

static void TestInvalidParameterSizes(TestingT *t) {
    DsaParameters params = {0};
    Error err = dsa_generate_parameters(&params, NULL, crypto_rand_reader, 4);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "GenerateParameters with sizes 4 succeeded");
    if (!str_eq(error_text(err), BURROW_S("crypto/dsa: invalid ParameterSizes")))
        testing_t_errorf_v(t, "error = %q", error_text(err));
    if (params.p != NULL || params.q != NULL || params.g != NULL)
        testing_t_errorf_v(t, "the parameters were set");
}

static void TestGenerateKeyWithoutParameters(TestingT *t) {
    DsaPrivateKey priv = {0};
    Error err = dsa_generate_key(&priv, NULL, crypto_rand_reader);
    if (BURROW_OK(err))
        testing_t_fatalf_v(t, "GenerateKey without parameters succeeded");
    if (!str_eq(error_text(err),
                BURROW_S("crypto/dsa: parameters not set up before generating key")))
        testing_t_errorf_v(t, "error = %q", error_text(err));
    if (priv.x != NULL || priv.public_key.y != NULL)
        testing_t_errorf_v(t, "the key was set");
}

static void TestInvalidPublicKeyError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DsaPrivateKey priv = test_key(a);
    /* A q whose length in bits is not a multiple of 8. */
    priv.public_key.parameters.q = from_hex(a, "7F");
    Error err = BURROW_NO_ERROR;
    BigInt *s = big_new_int(a, 1);
    BigInt *r = dsa_sign(a, crypto_rand_reader, &priv, cs("testing"), &s, &err);
    if (!errors_is(err, dsa_err_invalid_public_key))
        testing_t_errorf_v(t, "error = %v, want ErrInvalidPublicKey", err);
    if (!str_eq(error_text(dsa_err_invalid_public_key),
                BURROW_S("crypto/dsa: invalid public key")))
        testing_t_errorf_v(t, "ErrInvalidPublicKey = %q",
                           error_text(dsa_err_invalid_public_key));
    if (r != NULL || s != NULL)
        testing_t_errorf_v(t, "Sign gave a signature with an error");
    arena_free(&ar);
}

static const Type test_type = {
    {(const Byte *)"errReader", 9},
    {(const Byte *)"dsa_test", 8},
    KIND_STRUCT,
    0,
    1,
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x64736174U,
    NULL,
};

BURROW_SENTINEL_ERROR(test_err_read, "test: read failed");

/* A reader that fails every read. */
static Int err_read(void *self, Slice p, Error *err) {
    (void)self;
    (void)p;
    BURROW_OUT(err, test_err_read);
    return 0;
}

static const IoReaderVT err_vt = {&test_type, err_read};

static void TestReadErrors(TestingT *t) {
    IoReader bad = {&err_vt, NULL};
    DsaParameters params = {0};
    Error err = dsa_generate_parameters(&params, NULL, bad, DSA_L1024N160);
    if (!errors_is(err, test_err_read))
        testing_t_errorf_v(t, "GenerateParameters: error = %v", err);
    if (params.p != NULL || params.q != NULL || params.g != NULL)
        testing_t_errorf_v(t, "GenerateParameters: the parameters were set");

    Arena ar;
    arena_init(&ar, NULL, 0);
    DsaPrivateKey priv = test_key(arena_allocator(&ar));
    priv.x = NULL;
    priv.public_key.y = NULL;
    err = dsa_generate_key(&priv, NULL, bad);
    if (!errors_is(err, test_err_read))
        testing_t_errorf_v(t, "GenerateKey: error = %v", err);
    if (priv.x != NULL || priv.public_key.y != NULL)
        testing_t_errorf_v(t, "GenerateKey: the key was set");
    arena_free(&ar);
}

static void free_int(Alloc *a, BigInt *x) {
    big_int_free(x);
    mem_free(a, x, sizeof *x, _Alignof(BigInt));
}

/* Everything from the heap and given back, which the sanitizer checks. */
static void TestHeapKeys(TestingT *t) {
    Alloc *a = heap_allocator();
    DsaPrivateKey priv = {0};
    DsaParameters *params = &priv.public_key.parameters;
    params->p = from_hex(a, TEST_P);
    params->q = from_hex(a, TEST_Q);
    params->g = from_hex(a, TEST_G);
    Error err = dsa_generate_key(&priv, a, (IoReader){NULL, NULL});
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    Slice hashed = cs("testing");
    BigInt *s = NULL;
    BigInt *r = dsa_sign(NULL, (IoReader){NULL, NULL}, &priv, hashed, &s, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!dsa_verify(&priv.public_key, hashed, r, s))
        testing_t_errorf_v(t, "signature does not verify");
    big_int_add(r, r, s);
    if (dsa_verify(&priv.public_key, hashed, r, s))
        testing_t_errorf_v(t, "changed signature verifies");

    free_int(a, r);
    free_int(a, s);
    free_int(a, priv.x);
    free_int(a, priv.public_key.y);
    free_int(a, params->p);
    free_int(a, params->q);
    free_int(a, params->g);
}

/* ----------------------------------------------------------- benchmarks */

static void BenchmarkSign(TestingB *b) {
    Arena keys, ar;
    arena_init(&keys, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    DsaPrivateKey priv = test_key(arena_allocator(&keys));
    uint8_t hashed[] = "testing";

    testing_b_report_allocs(b);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Error err = BURROW_NO_ERROR;
        BigInt *s = NULL;
        BigInt *r = dsa_sign(a, crypto_rand_reader, &priv,
                             slice_from(hashed, 7, 7, TYPE_BYTE), &s, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        hashed[0] = (uint8_t)big_int_bit_len(r);
        arena_reset(&ar);
    }
    arena_free(&ar);
    arena_free(&keys);
}

static void BenchmarkVerify(TestingB *b) {
    Arena keys;
    arena_init(&keys, NULL, 0);
    Alloc *ka = arena_allocator(&keys);
    DsaPrivateKey priv = test_key(ka);
    Slice hashed = cs("testing");
    Error err = BURROW_NO_ERROR;
    BigInt *s = NULL;
    BigInt *r = dsa_sign(ka, crypto_rand_reader, &priv, hashed, &s, &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "%s", error_text(err));

    testing_b_report_allocs(b);
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        if (!dsa_verify(&priv.public_key, hashed, r, s))
            testing_b_fatalf_v(b, "verify failed");
    }
    arena_free(&keys);
}

#define TESTS(X)                                                                       \
    X(TestParameterGeneration)                                                         \
    X(TestSignAndVerify)                                                               \
    X(TestSignAndVerifyWithBadPublicKey)                                               \
    X(TestSigningWithDegenerateKeys)                                                   \
    X(TestDSAWycheproof)                                                               \
    X(TestInvalidParameterSizes)                                                       \
    X(TestGenerateKeyWithoutParameters)                                                \
    X(TestInvalidPublicKeyError)                                                       \
    X(TestReadErrors)                                                                  \
    X(TestHeapKeys)                                                                    \
    X(BenchmarkSign)                                                                   \
    X(BenchmarkVerify)

TESTING_MAIN(TESTS)
