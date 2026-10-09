/* Derived from Go's src/crypto/rsa/rsa_test.go, pkcs1v15_test.go,
 * pss_test.go, equal_test.go and rsa_wycheproof_test.go, and the tests of
 * src/crypto/internal/fips140/rsa: keygen_test.go, pkcs1v15_test.go and
 * pkcs1v22_test.go. Go source: go1.27.1.
 *
 * The keys, the tables, the files in testdata and the Wycheproof vectors Go
 * fetches as a module are in rsa_test_gen.h, which tools/gen-rsa-tests.sh
 * writes, with every key already out of its PEM or DER as Go's x509 takes it
 * apart. There is no x509 here yet, so the round trips through PKCS #8 and
 * PKIX that testEverything and TestEqual make go through pkcs8_marshal,
 * pkcs8_parse, pkix_marshal and pkix_parse below, which write and read what
 * Go's x509 does for a key of two primes. TestKeyGenerationVectors compares
 * the PKCS #8 of the key it makes with that of the key in the vector, where Go
 * has the vector's DER itself.
 *
 * Go's tests set GODEBUG=rsa1024min=0 with t.Setenv, and here the tests that
 * need it set it with burrow__rsa_godebug_set and go back to the environment's
 * at the end. TestEncryptPKCS1v15 makes its random messages itself, since
 * there is no testing/quick, as many as quick.Check would. TestAllocations
 * counts Go's heap allocations and has no counterpart.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "burrow/bufio.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/compress/bzip2.h"
#include "burrow/crypto.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/rsa.h"
#include "burrow/crypto/sha1.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/sha512.h"
#include "burrow/encoding/base64.h"
#include "burrow/encoding/hex.h"
#include "burrow/io.h"
#include "burrow/math/big.h"
#include "burrow/slices.h"
#include "burrow/strings.h"

#include "../src/crypto/bigmod.h"
#include "../src/crypto/cryptobyte.h"
#include "../src/crypto/ecdsa_internal.h"
#include "../src/crypto/rand_internal.h"
#include "../src/crypto/rsa_internal.h"

#include "rsa_test_gen.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Slice bs(const void *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

static Slice cs(const char *s) {
    return bs(s, (Int)strlen(s));
}

static Slice unhex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("invalid hex"));
    return b;
}

static Str hexs(Alloc *a, Slice b) {
    return hex_encode_to_string(a, b);
}

/* bigFromHex. */
static BigInt *from_hex(Alloc *a, const char *s) {
    BigInt *r = big_new_int(a, 0);
    bool ok = false;
    big_int_set_string(r, str_from_cstr(s), 16, &ok);
    if (!ok)
        panic_str(BURROW_S("bad hex"));
    return r;
}

/* decodeBase64, which is nil for input that is not base64. */
static Slice decode_base64(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b =
        base64_encoding_decode_string(base64_std_encoding, a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        return (Slice){0};
    return b;
}

static Slice digest(Alloc *a, CryptoHash h, Slice msg) {
    Hash hh = crypto_hash_new(h, a);
    hash_write(hh, msg, NULL);
    return hash_sum(a, hh, (Slice){0});
}

/* The Wycheproof ciphertexts and signatures, which come in up to three
 * pieces C99 can take as literals. */
static Slice unhex_parts(Alloc *a, const char *const parts[3]) {
    Slice b[3];
    Int n = 0;
    while (n < 3 && parts[n] != NULL) {
        b[n] = unhex(a, parts[n]);
        n++;
    }
    return slices_concat(a, b, n);
}

static uint8_t *bytes_of(Slice s) {
    return (uint8_t *)s.p;
}

/* t.Setenv("GODEBUG", "rsa1024min=0"), until min1024_default. */
static void min1024_off(void) {
    burrow__rsa_godebug_set("rsa1024min=0");
}

static void min1024_default(void) {
    burrow__rsa_godebug_set(NULL);
}

static const Type test_type = {
    {(const Byte *)"keyGenTestReader", 16},
    {(const Byte *)"rsa_test", 8},
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
    0x72736174U,
    NULL,
};

/* ------------------------------------------------------------------- keys */

static const GenKey *gen_key_named(const char *name) {
    for (size_t i = 0; i < sizeof gen_keys / sizeof gen_keys[0]; i++)
        if (strcmp(gen_keys[i].name, name) == 0)
            return &gen_keys[i];
    panic_str(BURROW_S("no such key"));
}

/* The public half of k, from a. */
static RsaPublicKey *gen_public(Alloc *a, const GenKey *k) {
    RsaPublicKey *pub = BURROW_NEW(a, RsaPublicKey);
    pub->n = from_hex(a, k->n);
    pub->e = k->e;
    return pub;
}

/* k as x509.ParsePKCS1PrivateKey leaves it, with the precomputed values from
 * the key and then Precompute, from a. */
static RsaPrivateKey *gen_private(Alloc *a, const GenKey *k) {
    RsaPrivateKey *priv = BURROW_NEW(a, RsaPrivateKey);
    memset(priv, 0, sizeof *priv);
    priv->public_key.n = from_hex(a, k->n);
    priv->public_key.e = k->e;
    priv->d = from_hex(a, k->d);
    priv->primes = BURROW_NEW_N(a, BigInt *, 2);
    priv->primes[0] = from_hex(a, k->p);
    priv->primes[1] = from_hex(a, k->q);
    priv->primes_len = 2;
    priv->precomputed.dp = from_hex(a, k->dp);
    priv->precomputed.dq = from_hex(a, k->dq);
    priv->precomputed.qinv = from_hex(a, k->qinv);
    rsa_private_key_precompute(priv, a);
    return priv;
}

static RsaPrivateKey *key_named(Alloc *a, const char *name) {
    return gen_private(a, gen_key_named(name));
}

static CryptoPublicKey any_public(const RsaPublicKey *pub) {
    return BURROW_ANY(TYPE_RSA_PUBLIC_KEY, (uintptr_t)pub);
}

static CryptoPrivateKey any_private(const RsaPrivateKey *priv) {
    return BURROW_ANY(TYPE_RSA_PRIVATE_KEY, (uintptr_t)priv);
}

/* ------------------------------------------------------- PKCS #8 and PKIX */

/* The AlgorithmIdentifier of rsaEncryption, with its NULL parameters. */
static const uint8_t rsa_algorithm[] = {0x30, 0x0d, 0x06, 0x09, 0x2a, 0x86, 0x48, 0x86,
                                        0xf7, 0x0d, 0x01, 0x01, 0x01, 0x05, 0x00};

static void pkcs1_private_body(void *env, CryptobyteBuilder *b) {
    const RsaPrivateKey *k = env;
    cryptobyte_builder_add_asn1_int64(b, 0);
    cryptobyte_builder_add_asn1_big_int(b, k->public_key.n);
    cryptobyte_builder_add_asn1_int64(b, k->public_key.e);
    cryptobyte_builder_add_asn1_big_int(b, k->d);
    cryptobyte_builder_add_asn1_big_int(b, k->primes[0]);
    cryptobyte_builder_add_asn1_big_int(b, k->primes[1]);
    cryptobyte_builder_add_asn1_big_int(b, k->precomputed.dp);
    cryptobyte_builder_add_asn1_big_int(b, k->precomputed.dq);
    cryptobyte_builder_add_asn1_big_int(b, k->precomputed.qinv);
}

static void pkcs1_private(void *env, CryptobyteBuilder *b) {
    cryptobyte_builder_add_asn1(
        b, CRYPTOBYTE_ASN1_SEQUENCE,
        BURROW_FN(CryptobyteBuilderContinuation, pkcs1_private_body, env));
}

static void pkcs8_body(void *env, CryptobyteBuilder *b) {
    cryptobyte_builder_add_asn1_int64(b, 0);
    cryptobyte_builder_add_bytes(b, bs(rsa_algorithm, sizeof rsa_algorithm));
    cryptobyte_builder_add_asn1(
        b, CRYPTOBYTE_ASN1_OCTET_STRING,
        BURROW_FN(CryptobyteBuilderContinuation, pkcs1_private, env));
}

/* x509.MarshalPKCS8PrivateKey for an RSA key of two primes, from a, which
 * precomputes and validates the key first, as Go's does. */
static Slice pkcs8_marshal(Alloc *a, RsaPrivateKey *k, Error *err) {
    if (k->primes_len != 2) {
        *err = errors_new(error_allocator(),
                          BURROW_S("pkcs8_marshal: only keys of two primes"));
        return (Slice){0};
    }
    rsa_private_key_precompute(k, a);
    *err = rsa_private_key_validate(k);
    if (BURROW_FAILED(*err))
        return (Slice){0};
    CryptobyteBuilder b = cryptobyte_new_builder(a, (Slice){0});
    cryptobyte_builder_add_asn1(
        &b, CRYPTOBYTE_ASN1_SEQUENCE,
        BURROW_FN(CryptobyteBuilderContinuation, pkcs8_body, k));
    return cryptobyte_builder_bytes(&b, err);
}

/* x509.ParsePKCS8PrivateKey for an RSA key of two primes, from a. */
static RsaPrivateKey *pkcs8_parse(Alloc *a, Slice der, Error *err) {
    CryptobyteString input = der, info, algo, pkcs1_der, pkcs1;
    BigInt *v[9];
    for (int i = 0; i < 9; i++)
        v[i] = big_new_int(a, 0);
    bool ok =
        cryptobyte_string_read_asn1(&input, &info, CRYPTOBYTE_ASN1_SEQUENCE) &&
        cryptobyte_string_empty(input) &&
        cryptobyte_string_skip_asn1(&info, CRYPTOBYTE_ASN1_INTEGER) &&
        cryptobyte_string_read_asn1_element(&info, &algo, CRYPTOBYTE_ASN1_SEQUENCE) &&
        bytes_equal(algo, bs(rsa_algorithm, sizeof rsa_algorithm)) &&
        cryptobyte_string_read_asn1(&info, &pkcs1_der, CRYPTOBYTE_ASN1_OCTET_STRING) &&
        cryptobyte_string_read_asn1(&pkcs1_der, &pkcs1, CRYPTOBYTE_ASN1_SEQUENCE) &&
        cryptobyte_string_empty(pkcs1_der);
    for (int i = 0; ok && i < 9; i++)
        ok = cryptobyte_string_read_asn1_integer_big(&pkcs1, v[i]);
    if (!ok || !cryptobyte_string_empty(pkcs1)) {
        *err = errors_new(error_allocator(), BURROW_S("x509: malformed PKCS#8"));
        return NULL;
    }
    if (big_int_sign(v[0]) != 0) {
        *err = errors_new(error_allocator(),
                          BURROW_S("x509: unsupported private key version"));
        return NULL;
    }
    for (int i = 1; i < 9; i++) {
        if (big_int_sign(v[i]) <= 0) {
            *err = errors_new(
                error_allocator(),
                BURROW_S("x509: private key contains zero or negative value"));
            return NULL;
        }
    }
    RsaPrivateKey *k = BURROW_NEW(a, RsaPrivateKey);
    memset(k, 0, sizeof *k);
    k->public_key.n = v[1];
    k->public_key.e = (Int)big_int_int64(v[2]);
    k->d = v[3];
    k->primes = BURROW_NEW_N(a, BigInt *, 2);
    k->primes[0] = v[4];
    k->primes[1] = v[5];
    k->primes_len = 2;
    k->precomputed.dp = v[6];
    k->precomputed.dq = v[7];
    k->precomputed.qinv = v[8];
    rsa_private_key_precompute(k, a);
    *err = rsa_private_key_validate(k);
    if (BURROW_FAILED(*err))
        return NULL;
    return k;
}

static void pkcs1_public_body(void *env, CryptobyteBuilder *b) {
    const RsaPublicKey *k = env;
    cryptobyte_builder_add_asn1_big_int(b, k->n);
    cryptobyte_builder_add_asn1_int64(b, k->e);
}

typedef struct PkixEnv {
    Slice pkcs1;
} PkixEnv;

static void pkix_body(void *env, CryptobyteBuilder *b) {
    const PkixEnv *e = env;
    cryptobyte_builder_add_bytes(b, bs(rsa_algorithm, sizeof rsa_algorithm));
    cryptobyte_builder_add_asn1_bit_string(b, e->pkcs1);
}

/* x509.MarshalPKIXPublicKey for an RSA key, from a. */
static Slice pkix_marshal(Alloc *a, const RsaPublicKey *k, Error *err) {
    CryptobyteBuilder inner = cryptobyte_new_builder(a, (Slice){0});
    cryptobyte_builder_add_asn1(&inner, CRYPTOBYTE_ASN1_SEQUENCE,
                                BURROW_FN(CryptobyteBuilderContinuation,
                                          pkcs1_public_body, (void *)(uintptr_t)k));
    PkixEnv e = {cryptobyte_builder_bytes(&inner, err)};
    if (BURROW_FAILED(*err))
        return (Slice){0};
    CryptobyteBuilder b = cryptobyte_new_builder(a, (Slice){0});
    cryptobyte_builder_add_asn1(
        &b, CRYPTOBYTE_ASN1_SEQUENCE,
        BURROW_FN(CryptobyteBuilderContinuation, pkix_body, &e));
    return cryptobyte_builder_bytes(&b, err);
}

/* x509.ParsePKIXPublicKey for an RSA key, from a. */
static RsaPublicKey *pkix_parse(Alloc *a, Slice der, Error *err) {
    CryptobyteString input = der, spki, algo, pkcs1_der, pkcs1;
    BigInt *n = big_new_int(a, 0), *e = big_new_int(a, 0);
    if (!cryptobyte_string_read_asn1(&input, &spki, CRYPTOBYTE_ASN1_SEQUENCE) ||
        !cryptobyte_string_empty(input) ||
        !cryptobyte_string_read_asn1_element(&spki, &algo, CRYPTOBYTE_ASN1_SEQUENCE) ||
        !bytes_equal(algo, bs(rsa_algorithm, sizeof rsa_algorithm)) ||
        !cryptobyte_string_read_asn1_bit_string_as_bytes(&spki, &pkcs1_der) ||
        !cryptobyte_string_empty(spki) ||
        !cryptobyte_string_read_asn1(&pkcs1_der, &pkcs1, CRYPTOBYTE_ASN1_SEQUENCE) ||
        !cryptobyte_string_empty(pkcs1_der) ||
        !cryptobyte_string_read_asn1_integer_big(&pkcs1, n) ||
        !cryptobyte_string_read_asn1_integer_big(&pkcs1, e) ||
        !cryptobyte_string_empty(pkcs1)) {
        *err =
            errors_new(error_allocator(), BURROW_S("x509: malformed PKIX public key"));
        return NULL;
    }
    if (big_int_sign(n) <= 0) {
        *err = errors_new(error_allocator(),
                          BURROW_S("x509: RSA modulus is not a positive number"));
        return NULL;
    }
    if (big_int_sign(e) <= 0) {
        *err =
            errors_new(error_allocator(),
                       BURROW_S("x509: RSA public exponent is not a positive number"));
        return NULL;
    }
    RsaPublicKey *k = BURROW_NEW(a, RsaPublicKey);
    k->n = n;
    k->e = (Int)big_int_int64(e);
    return k;
}

/* ------------------------------------------------------- key generation */

static void key_basics(TestingT *t, Alloc *a, const RsaPrivateKey *priv) {
    Error err = rsa_private_key_validate(priv);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Validate() failed: %s", error_text(err));
    if (big_int_cmp(priv->d, priv->public_key.n) > 0)
        testing_t_errorf_v(t, "private exponent too large");

    Slice msg = cs("hi!");
    err = BURROW_NO_ERROR;
    Slice enc =
        rsa_encrypt_pkcs1_v15(a, crypto_rand_reader, &priv->public_key, msg, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "EncryptPKCS1v15: %s", error_text(err));
        return;
    }
    Slice dec = rsa_decrypt_pkcs1_v15(a, (IoReader){0}, priv, enc, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "DecryptPKCS1v15: %s", error_text(err));
        return;
    }
    if (!bytes_equal(dec, msg))
        testing_t_errorf_v(t, "got:%s want:%s", hexs(a, dec), hexs(a, msg));
}

static void key_generation(void *env, TestingT *t) {
    Int size = (Int)(intptr_t)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    min1024_default();
    Error err = BURROW_NO_ERROR;
    if (size < 1024) {
        (void)rsa_generate_key(a, crypto_rand_reader, size, &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "GenerateKey(%d) succeeded without GODEBUG", size);
        min1024_off();
    }
    err = BURROW_NO_ERROR;
    RsaPrivateKey *priv = rsa_generate_key(a, crypto_rand_reader, size, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "GenerateKey(%d): %s", size, error_text(err));
    Int bits = big_int_bit_len(priv->public_key.n);
    if (bits != size)
        testing_t_errorf_v(t, "key too short (%d vs %d)", bits, size);
    key_basics(t, a, priv);

    min1024_default();
    arena_free(&ar);
}

static void TestKeyGeneration(TestingT *t) {
    static const Int sizes[] = {128, 512, 1024, 2048, 3072, 4096};
    size_t n = testing_short() ? 2 : sizeof sizes / sizeof sizes[0];
    for (size_t i = 0; i < n; i++) {
        char name[16];
        snprintf(name, sizeof name, "%d", (int)sizes[i]);
        testing_t_run(
            t, str_from_cstr(name),
            BURROW_FN(TestingTFunc, key_generation, (void *)(intptr_t)sizes[i]));
    }
}

static void multi_prime_key_generation(TestingT *t, Int nprimes) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Int size = 1024;
    if (testing_short()) {
        min1024_off();
        size = 256;
    }
    Error err = BURROW_NO_ERROR;
    RsaPrivateKey *priv =
        rsa_generate_multi_prime_key(a, crypto_rand_reader, nprimes, size, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to generate key");
    key_basics(t, a, priv);

    min1024_default();
    arena_free(&ar);
}

static void Test3PrimeKeyGeneration(TestingT *t) {
    multi_prime_key_generation(t, 3);
}

static void Test4PrimeKeyGeneration(TestingT *t) {
    multi_prime_key_generation(t, 4);
}

static void TestNPrimeKeyGeneration(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    min1024_off();
    Int prime_size = 64, max_n = 24;
    if (testing_short()) {
        prime_size = 16;
        max_n = 16;
    }
    /* Test that generation of N-prime keys works for N > 4. */
    for (Int n = 5; n < max_n; n++) {
        Error err = BURROW_NO_ERROR;
        RsaPrivateKey *priv = rsa_generate_multi_prime_key(a, crypto_rand_reader, n,
                                                           64 + n * prime_size, &err);
        if (!BURROW_FAILED(err))
            key_basics(t, a, priv);
        else
            testing_t_errorf_v(t, "failed to generate %d-prime key", n);
        arena_reset(&ar);
    }

    min1024_default();
    arena_free(&ar);
}

/* This test ensures that trying to generate or validate toy RSA keys doesn't
 * enter an infinite loop or panic. */
static void TestImpossibleKeyGeneration(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    min1024_off();
    for (Int i = 0; i < 32; i++) {
        Error err = BURROW_NO_ERROR;
        (void)rsa_generate_key(a, crypto_rand_reader, i, &err);
        err = BURROW_NO_ERROR;
        (void)rsa_generate_multi_prime_key(a, crypto_rand_reader, 3, i, &err);
        err = BURROW_NO_ERROR;
        (void)rsa_generate_multi_prime_key(a, crypto_rand_reader, 4, i, &err);
        err = BURROW_NO_ERROR;
        (void)rsa_generate_multi_prime_key(a, crypto_rand_reader, 5, i, &err);
        arena_reset(&ar);
    }

    min1024_default();
    arena_free(&ar);
}

/* Toy-sized keys can randomly hit hard failures in GenerateKey. */
static void TestTinyKeyGeneration(TestingT *t) {
    if (testing_short())
        testing_t_skip_v(t, "skipping in short mode");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    min1024_off();
    for (int i = 0; i < 10000; i++) {
        Error err = BURROW_NO_ERROR;
        RsaPrivateKey *k = rsa_generate_key(a, crypto_rand_reader, 32, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "GenerateKey(32): %s", error_text(err));
        err = rsa_private_key_validate(k);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "Validate(32): %s", error_text(err));
        arena_reset(&ar);
    }

    min1024_default();
    arena_free(&ar);
}

/* keyGenTestReader: a read of one byte, which is randutil.MaybeReadByte, gets
 * nothing out of the DRBG, and every other read is filled from it. */
static Int key_gen_read(void *self, Slice p, Error *err) {
    (void)err;
    if (p.len == 1)
        return 1;
    burrow__ecdsa_drbg_generate(self, p);
    return p.len;
}

static const IoReaderVT key_gen_vt = {&test_type, key_gen_read};

static void key_generation_vector(void *env, TestingT *t) {
    const GenKeyGen *v = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    uint8_t pers[32];
    size_t pl = strlen("det RSA key gen");
    memcpy(pers, "det RSA key gen", pl);
    pers[pl] = (uint8_t)(v->bits >> 8);
    pers[pl + 1] = (uint8_t)v->bits;
    burrow__crypto_rand_godebug_set("cryptocustomrand=1");
    EcdsaDrbg *drbg = burrow__ecdsa_new_drbg(a, sha256_new, unhex(a, v->seed),
                                             (Slice){0}, bs(pers, (Int)pl + 2));
    IoReader rng = {&key_gen_vt, drbg};
    Error err = BURROW_NO_ERROR;
    RsaPrivateKey *priv = rsa_generate_key(a, rng, v->bits, &err);
    burrow__crypto_rand_godebug_set(NULL);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "GenerateKey: %s", error_text(err));
    key_basics(t, a, priv);
    Slice der = pkcs8_marshal(a, priv, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "MarshalPKCS8PrivateKey: %s", error_text(err));
    Slice want = pkcs8_marshal(a, gen_private(a, &v->key), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "MarshalPKCS8PrivateKey of the vector: %s",
                           error_text(err));
    if (!bytes_equal(der, want))
        testing_t_errorf_v(t, "PKCS8 mismatch:\n%s\nvs\n\n%s", hexs(a, der),
                           hexs(a, want));

    arena_free(&ar);
}

/* TestKeyGenerationVectors tests RSA key generation against the
 * c2sp.org/det-keygen test vectors. */
static void TestKeyGenerationVectors(TestingT *t) {
    for (size_t i = 0; i < sizeof gen_keygen / sizeof gen_keygen[0]; i++) {
        char name[16];
        snprintf(name, sizeof name, "%d", (int)i);
        testing_t_run(t, str_from_cstr(name),
                      BURROW_FN(TestingTFunc, key_generation_vector,
                                (void *)(uintptr_t)&gen_keygen[i]));
    }
}

/* This is a key generated by `certtool --generate-privkey --bits 128`. It's
 * such that de ≢ 1 mod φ(n), but is congruent mod the order of the group. */
static void TestGnuTLSKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_off();
    key_basics(t, a, key_named(a, "TestGnuTLSKey"));
    min1024_default();
    arena_free(&ar);
}

/* ------------------------------------------------------------ Everything */

static bool too_long(Error err) {
    return errors_is(err, rsa_err_message_too_long);
}

/* The PSS round trip of testEverything, with opts. */
static void everything_pss(TestingT *t, Alloc *a, const RsaPrivateKey *priv,
                           uint8_t *hash, const RsaPSSOptions *opts, const char *what) {
    Slice h = bs(hash, 32);
    Error err = BURROW_NO_ERROR;
    Slice sig = rsa_sign_pss(a, crypto_rand_reader, priv, CRYPTO_SHA256, h, opts, &err);
    if (too_long(err)) {
        testing_t_logf_v(t, "key too small for SignPSS with %s", what);
        return;
    }
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "SignPSS: %s", error_text(err));
        return;
    }
    err = rsa_verify_pss(&priv->public_key, CRYPTO_SHA256, h, sig, opts);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "VerifyPSS: %s", error_text(err));
    bytes_of(sig)[1] ^= 0x80;
    if (!BURROW_FAILED(rsa_verify_pss(&priv->public_key, CRYPTO_SHA256, h, sig, opts)))
        testing_t_errorf_v(t, "VerifyPSS success for tampered signature");
    bytes_of(sig)[1] ^= 0x80;
    hash[1] ^= 0x80;
    if (!BURROW_FAILED(rsa_verify_pss(&priv->public_key, CRYPTO_SHA256, h, sig, opts)))
        testing_t_errorf_v(t, "VerifyPSS success for tampered message");
    hash[1] ^= 0x80;
}

static void everything(TestingT *t, Alloc *a, RsaPrivateKey *priv) {
    Error validate_err = rsa_private_key_validate(priv);
    if (BURROW_FAILED(validate_err) && priv->primes_len >= 2)
        testing_t_errorf_v(t, "Validate() failed: %s", error_text(validate_err));

    Slice msg = cs("test");
    Error err = BURROW_NO_ERROR;
    Slice enc =
        rsa_encrypt_pkcs1_v15(a, crypto_rand_reader, &priv->public_key, msg, &err);
    if (too_long(err))
        testing_t_logf_v(t, "key too small for EncryptPKCS1v15");
    else if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "EncryptPKCS1v15: %s", error_text(err));
    if (!BURROW_FAILED(err)) {
        Slice dec = rsa_decrypt_pkcs1_v15(a, (IoReader){0}, priv, enc, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "DecryptPKCS1v15: %s", error_text(err));
        uint8_t key[4] = {0};
        err = rsa_decrypt_pkcs1_v15_session_key((IoReader){0}, priv, enc, bs(key, 4));
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "DecryptPKCS1v15SessionKey: %s", error_text(err));
        if (!bytes_equal(dec, msg))
            testing_t_errorf_v(t, "got:%s want:%s", hexs(a, dec), hexs(a, msg));
    }

    Slice label = cs("label");
    err = BURROW_NO_ERROR;
    enc = rsa_encrypt_oaep(a, sha256_new(a), crypto_rand_reader, &priv->public_key, msg,
                           label, &err);
    if (too_long(err))
        testing_t_logf_v(t, "key too small for EncryptOAEP");
    else if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "EncryptOAEP: %s", error_text(err));
    if (!BURROW_FAILED(err)) {
        Slice dec =
            rsa_decrypt_oaep(a, sha256_new(a), (IoReader){0}, priv, enc, label, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "DecryptOAEP: %s", error_text(err));
        if (!bytes_equal(dec, msg))
            testing_t_errorf_v(t, "got:%s want:%s", hexs(a, dec), hexs(a, msg));
    }

    const char *hash_msg = "crypto/rsa: input must be hashed message";
    err = BURROW_NO_ERROR;
    (void)rsa_sign_pkcs1_v15(a, (IoReader){0}, priv, CRYPTO_SHA256, msg, &err);
    if (!BURROW_FAILED(err) || !str_eq(error_text(err), str_from_cstr(hash_msg)))
        testing_t_errorf_v(t, "SignPKCS1v15 with bad hash: err = %q, want %q",
                           error_text(err), hash_msg);

    uint8_t hash[32];
    Slice h = digest(a, CRYPTO_SHA256, msg);
    memcpy(hash, h.p, 32);
    h = bs(hash, 32);
    err = BURROW_NO_ERROR;
    Slice sig = rsa_sign_pkcs1_v15(a, (IoReader){0}, priv, CRYPTO_SHA256, h, &err);
    if (too_long(err))
        testing_t_logf_v(t, "key too small for SignPKCS1v15");
    else if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "SignPKCS1v15: %s", error_text(err));
    if (!BURROW_FAILED(err)) {
        err = rsa_verify_pkcs1_v15(&priv->public_key, CRYPTO_SHA256, h, sig);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "VerifyPKCS1v15: %s", error_text(err));
        bytes_of(sig)[1] ^= 0x80;
        if (!BURROW_FAILED(
                rsa_verify_pkcs1_v15(&priv->public_key, CRYPTO_SHA256, h, sig)))
            testing_t_errorf_v(t, "VerifyPKCS1v15 success for tampered signature");
        bytes_of(sig)[1] ^= 0x80;
        hash[1] ^= 0x80;
        if (!BURROW_FAILED(
                rsa_verify_pkcs1_v15(&priv->public_key, CRYPTO_SHA256, h, sig)))
            testing_t_errorf_v(t, "VerifyPKCS1v15 success for tampered message");
        hash[1] ^= 0x80;
    }

    RsaPSSOptions opts = {RSA_PSS_SALT_LENGTH_AUTO, 0};
    everything_pss(t, a, priv, hash, &opts, "PSSSaltLengthAuto");
    opts.salt_length = RSA_PSS_SALT_LENGTH_EQUALS_HASH;
    everything_pss(t, a, priv, hash, &opts, "PSSSaltLengthEqualsHash");

    /* Check that an input bigger than the modulus is handled correctly,
     * whether it is longer than the byte size of the modulus or not. */
    Int size = rsa_public_key_size(&priv->public_key);
    Slice c = slice_make(a, TYPE_BYTE, size + 1, size + 1);
    memset(c.p, 0xff, (size_t)(size + 1));
    c.len = size;
    if (!BURROW_FAILED(rsa_verify_pss(&priv->public_key, CRYPTO_SHA256, h, c, &opts)))
        testing_t_errorf_v(t, "VerifyPSS accepted a large signature");
    err = BURROW_NO_ERROR;
    (void)rsa_decrypt_pkcs1_v15(a, (IoReader){0}, priv, c, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "DecryptPKCS1v15 accepted a large ciphertext");
    c.len = size + 1;
    if (!BURROW_FAILED(rsa_verify_pss(&priv->public_key, CRYPTO_SHA256, h, c, &opts)))
        testing_t_errorf_v(t, "VerifyPSS accepted a long signature");
    err = BURROW_NO_ERROR;
    (void)rsa_decrypt_pkcs1_v15(a, (IoReader){0}, priv, c, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "DecryptPKCS1v15 accepted a long ciphertext");

    if (!BURROW_FAILED(validate_err)) {
        err = BURROW_NO_ERROR;
        Slice der = pkcs8_marshal(a, priv, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "MarshalPKCS8PrivateKey: %s", error_text(err));
        } else {
            RsaPrivateKey *key = pkcs8_parse(a, der, &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "ParsePKCS8PrivateKey: %s", error_text(err));
            else if (!rsa_private_key_equal(key, any_private(priv)))
                testing_t_errorf_v(t, "private key mismatch");
        }
    }

    err = BURROW_NO_ERROR;
    Slice der = pkix_marshal(a, &priv->public_key, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "MarshalPKIXPublicKey: %s", error_text(err));
        return;
    }
    RsaPublicKey *pub = pkix_parse(a, der, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "ParsePKIXPublicKey: %s", error_text(err));
    else if (!rsa_public_key_equal(pub, any_public(&priv->public_key)))
        testing_t_errorf_v(t, "public key mismatch");
}

static void everything_named(void *env, TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    everything(t, a, key_named(a, env));
    arena_free(&ar);
}

static void everything_size(void *env, TestingT *t) {
    Int size = (Int)(intptr_t)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    RsaPrivateKey *priv = rsa_generate_key(a, crypto_rand_reader, size, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "GenerateKey(%d): %s", size, error_text(err));
    Int bits = big_int_bit_len(priv->public_key.n);
    if (bits != size)
        testing_t_errorf_v(t, "key too short (%d vs %d)", bits, size);
    everything(t, a, priv);
    arena_free(&ar);
}

static void TestEverything(TestingT *t) {
    if (testing_short()) {
        /* Skip key generation, but still test real sizes. */
        testing_t_run(t, BURROW_S("1024"),
                      BURROW_FN(TestingTFunc, everything_named,
                                (void *)(uintptr_t)"test1024Key"));
        testing_t_run(t, BURROW_S("2048"),
                      BURROW_FN(TestingTFunc, everything_named,
                                (void *)(uintptr_t)"test2048Key"));
        return;
    }

    min1024_off();
    /* any smaller than 560 and not all tests will run */
    for (Int size = 32; size <= 560; size++) {
        char name[16];
        snprintf(name, sizeof name, "%d", (int)size);
        testing_t_run(t, str_from_cstr(name),
                      BURROW_FN(TestingTFunc, everything_size, (void *)(intptr_t)size));
    }
    min1024_default();
}

static void check_insecure(TestingT *t, Error err, const char *what) {
    if (!BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: expected error", what);
        return;
    }
    if (!strings_contains(error_text(err), BURROW_S("insecure")))
        testing_t_errorf_v(t, "%s: unexpected error: %s", what, error_text(err));
}

static void TestKeyTooSmall(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_off();
    RsaPrivateKey *k = key_named(a, "test512Key");
    min1024_default();

    uint8_t b[512 / 8] = {0};
    Slice buf = bs(b, sizeof b);
    CryptoHash sha512 = CRYPTO_SHA512;
    RsaPSSOptions pss = {RSA_PSS_SALT_LENGTH_EQUALS_HASH, 0};
    RsaPKCS1v15DecryptOptions pkcs = {0};
    RsaOAEPOptions oaep = {CRYPTO_SHA512, 0, {0}};
    Error err = BURROW_NO_ERROR;

    (void)rsa_private_key_sign(k, a, crypto_rand_reader, buf,
                               crypto_hash_as_signer_opts(&sha512), &err);
    check_insecure(t, err, "Sign");
    err = BURROW_NO_ERROR;
    (void)rsa_private_key_sign(k, a, crypto_rand_reader, buf,
                               rsa_pss_options_as_signer_opts(&pss), &err);
    check_insecure(t, err, "Sign with PSSOptions");
    err = BURROW_NO_ERROR;
    (void)rsa_private_key_decrypt(
        k, a, crypto_rand_reader, buf,
        rsa_pkcs1_v15_decrypt_options_as_decrypter_opts(&pkcs), &err);
    check_insecure(t, err, "Decrypt with PKCS1v15DecryptOptions");
    err = BURROW_NO_ERROR;
    (void)rsa_private_key_decrypt(k, a, crypto_rand_reader, buf,
                                  rsa_oaep_options_as_decrypter_opts(&oaep), &err);
    check_insecure(t, err, "Decrypt with OAEPOptions");
    check_insecure(t, rsa_verify_pkcs1_v15(&k->public_key, CRYPTO_SHA512, buf, buf),
                   "VerifyPKCS1v15");
    check_insecure(t, rsa_verify_pss(&k->public_key, CRYPTO_SHA512, buf, buf, &pss),
                   "VerifyPSS");
    err = BURROW_NO_ERROR;
    (void)rsa_sign_pkcs1_v15(a, crypto_rand_reader, k, CRYPTO_SHA512, buf, &err);
    check_insecure(t, err, "SignPKCS1v15");
    err = BURROW_NO_ERROR;
    (void)rsa_sign_pss(a, crypto_rand_reader, k, CRYPTO_SHA512, buf, &pss, &err);
    check_insecure(t, err, "SignPSS");
    err = BURROW_NO_ERROR;
    (void)rsa_encrypt_pkcs1_v15(a, crypto_rand_reader, &k->public_key, buf, &err);
    check_insecure(t, err, "EncryptPKCS1v15");
    err = BURROW_NO_ERROR;
    (void)rsa_encrypt_oaep(a, sha512_new(a), crypto_rand_reader, &k->public_key, buf,
                           (Slice){0}, &err);
    check_insecure(t, err, "EncryptOAEP");
    err = BURROW_NO_ERROR;
    (void)rsa_decrypt_pkcs1_v15(a, (IoReader){0}, k, buf, &err);
    check_insecure(t, err, "DecryptPKCS1v15");
    err = BURROW_NO_ERROR;
    (void)rsa_decrypt_oaep(a, sha512_new(a), (IoReader){0}, k, buf, (Slice){0}, &err);
    check_insecure(t, err, "DecryptOAEP");
    check_insecure(t, rsa_decrypt_pkcs1_v15_session_key((IoReader){0}, k, buf, buf),
                   "DecryptPKCS1v15SessionKey");

    arena_free(&ar);
}

/* ------------------------------------------------------------------ OAEP */

static RsaPublicKey oaep_public(Alloc *a, const GenOaepKey *k) {
    RsaPublicKey pub = {from_hex(a, k->modulus), k->e};
    return pub;
}

/* A key of testEncryptOAEPData as the tests make it: N, E and D only. */
static RsaPrivateKey oaep_private(Alloc *a, const GenOaepKey *k) {
    RsaPrivateKey priv;
    memset(&priv, 0, sizeof priv);
    priv.public_key = oaep_public(a, k);
    priv.d = from_hex(a, k->d);
    return priv;
}

static void TestEncryptOAEP(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();
    Hash sha1 = sha1_new(a);
    for (size_t i = 0; i < sizeof gen_oaep_keys / sizeof gen_oaep_keys[0]; i++) {
        const GenOaepKey *test = &gen_oaep_keys[i];
        RsaPublicKey pub = oaep_public(a, test);
        for (int j = 0; j < test->nmsgs; j++) {
            const GenOaepMessage *m = &gen_oaep_messages[test->msgs + j];
            BytesReader seed;
            bytes_reader_reset(&seed, unhex(a, m->seed));
            Error err = BURROW_NO_ERROR;
            Slice out = rsa_encrypt_oaep(a, sha1, bytes_reader_as_io_reader(&seed),
                                         &pub, unhex(a, m->in), (Slice){0}, &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "#%d,%d error: %s", (int)i, j, error_text(err));
            Slice want = unhex(a, m->out);
            if (!bytes_equal(out, want))
                testing_t_errorf_v(t, "#%d,%d bad result: %s (want %s)", (int)i, j,
                                   hexs(a, out), hexs(a, want));
        }
    }
    arena_free(&ar);
}

static void TestDecryptOAEP(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();
    Hash sha1 = sha1_new(a);
    for (size_t i = 0; i < sizeof gen_oaep_keys / sizeof gen_oaep_keys[0]; i++) {
        const GenOaepKey *test = &gen_oaep_keys[i];
        RsaPrivateKey priv = oaep_private(a, test);
        for (int j = 0; j < test->nmsgs; j++) {
            const GenOaepMessage *m = &gen_oaep_messages[test->msgs + j];
            Slice in = unhex(a, m->in), ct = unhex(a, m->out);
            Error err = BURROW_NO_ERROR;
            Slice out =
                rsa_decrypt_oaep(a, sha1, (IoReader){0}, &priv, ct, (Slice){0}, &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "#%d,%d error: %s", (int)i, j, error_text(err));
            else if (!bytes_equal(out, in))
                testing_t_errorf_v(t, "#%d,%d bad result: %s (want %s)", (int)i, j,
                                   hexs(a, out), hexs(a, in));

            /* Decrypt with blinding. */
            out = rsa_decrypt_oaep(a, sha1, crypto_rand_reader, &priv, ct, (Slice){0},
                                   &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "#%d,%d (blind) error: %s", (int)i, j,
                                   error_text(err));
            else if (!bytes_equal(out, in))
                testing_t_errorf_v(t, "#%d,%d (blind) bad result: %s (want %s)", (int)i,
                                   j, hexs(a, out), hexs(a, in));
        }
        if (testing_short())
            break;
    }
    arena_free(&ar);
}

static void Test2DecryptOAEP(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();
    Slice msg = unhex(a, gen_2decrypt_oaep_msg), in = unhex(a, gen_2decrypt_oaep_in);
    RsaPrivateKey priv = oaep_private(a, &gen_oaep_keys[0]);
    RsaOAEPOptions opts = {CRYPTO_SHA256, CRYPTO_SHA1, {0}};
    Error err = BURROW_NO_ERROR;
    Slice out =
        rsa_private_key_decrypt(&priv, a, crypto_rand_reader, in,
                                rsa_oaep_options_as_decrypter_opts(&opts), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "error: %s", error_text(err));
    else if (!bytes_equal(out, msg))
        testing_t_errorf_v(t, "bad result %s (want %s)", hexs(a, out), hexs(a, msg));
    arena_free(&ar);
}

static void TestEncryptDecryptOAEP(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();
    Hash sha256 = sha256_new(a);
    for (size_t i = 0; i < sizeof gen_oaep_keys / sizeof gen_oaep_keys[0]; i++) {
        const GenOaepKey *test = &gen_oaep_keys[i];
        RsaPrivateKey priv = oaep_private(a, test);
        for (int j = 0; j < test->nmsgs; j++) {
            const GenOaepMessage *m = &gen_oaep_messages[test->msgs + j];
            Slice in = unhex(a, m->in);
            char lb[16];
            snprintf(lb, sizeof lb, "hi#%d", j);
            Slice label = cs(lb);
            Error err = BURROW_NO_ERROR;
            Slice enc = rsa_encrypt_oaep(a, sha256, crypto_rand_reader,
                                         &priv.public_key, in, label, &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "#%d,%d: EncryptOAEP: %s", (int)i, j,
                                   error_text(err));
                continue;
            }
            Slice dec = rsa_decrypt_oaep(a, sha256, crypto_rand_reader, &priv, enc,
                                         label, &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "#%d,%d: DecryptOAEP: %s", (int)i, j,
                                   error_text(err));
                continue;
            }
            if (!bytes_equal(dec, in))
                testing_t_errorf_v(t, "#%d,%d: round trip %s -> %s", (int)i, j,
                                   hexs(a, in), hexs(a, dec));

            /* Using different hash for MGF. */
            RsaOAEPOptions opts = {CRYPTO_SHA256, CRYPTO_SHA1, label};
            enc = rsa_encrypt_oaep_with_options(a, crypto_rand_reader, &priv.public_key,
                                                in, &opts, &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "#%d,%d: EncryptOAEP with different MGFHash: %s",
                                   (int)i, j, error_text(err));
                continue;
            }
            dec = rsa_private_key_decrypt(&priv, a, crypto_rand_reader, enc,
                                          rsa_oaep_options_as_decrypter_opts(&opts),
                                          &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "#%d,%d: DecryptOAEP with different MGFHash: %s",
                                   (int)i, j, error_text(err));
                continue;
            }
            if (!bytes_equal(dec, in))
                testing_t_errorf_v(t,
                                   "#%d,%d: round trip with different MGFHash %s -> %s",
                                   (int)i, j, hexs(a, in), hexs(a, dec));

            /* Using a zero MGFHash. */
            RsaOAEPOptions zero = {CRYPTO_SHA256, 0, label};
            enc = rsa_encrypt_oaep_with_options(a, crypto_rand_reader, &priv.public_key,
                                                in, &zero, &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "#%d,%d: EncryptOAEP with zero MGFHash: %s",
                                   (int)i, j, error_text(err));
                continue;
            }
            dec = rsa_decrypt_oaep(a, sha256, crypto_rand_reader, &priv, enc, label,
                                   &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "#%d,%d: DecryptOAEP with zero MGFHash: %s",
                                   (int)i, j, error_text(err));
                continue;
            }
            if (!bytes_equal(dec, in))
                testing_t_errorf_v(t, "#%d,%d: round trip with zero MGFHash %s -> %s",
                                   (int)i, j, hexs(a, in), hexs(a, dec));
        }
    }
    arena_free(&ar);
}

/* This key has a 256-bit P and a 257-bit Q. */
static void TestPSmallerThanQ(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_off();
    everything(t, a, key_named(a, "TestPSmallerThanQ"));
    min1024_default();
    arena_free(&ar);
}

/* k1 has a 768-bit P and a 256-bit Q, and k2 a 256-bit P and a 768-bit Q. */
static void TestLargeSizeDifference(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();
    everything(t, a, key_named(a, "TestLargeSizeDifference/k1"));
    everything(t, a, key_named(a, "TestLargeSizeDifference/k2"));
    arena_free(&ar);
}

/* test2048KeyOnlyD, test2048KeyWithoutPrecomputed and
 * test2048KeyWithPrecomputed, which has only the public precomputed values,
 * and not the ones underneath, from test2048Key. */
typedef enum { ONLY_D, WITHOUT_PRECOMPUTED, WITH_PRECOMPUTED } Test2048Kind;

static RsaPrivateKey test2048_variant(Alloc *a, Test2048Kind kind) {
    RsaPrivateKey *k = key_named(a, "test2048Key");
    RsaPrivateKey v;
    memset(&v, 0, sizeof v);
    v.public_key = k->public_key;
    v.d = k->d;
    if (kind != ONLY_D) {
        v.primes = k->primes;
        v.primes_len = k->primes_len;
    }
    if (kind == WITH_PRECOMPUTED) {
        v.precomputed.dp = k->precomputed.dp;
        v.precomputed.dq = k->precomputed.dq;
        v.precomputed.qinv = k->precomputed.qinv;
    }
    return v;
}

static void not_precomputed(void *env, TestingT *t) {
    Test2048Kind kind = (Test2048Kind)(intptr_t)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    RsaPrivateKey key = test2048_variant(a, kind);
    everything(t, a, &key);
    RsaPrivateKey k =
        test2048_variant(a, kind == ONLY_D ? ONLY_D : WITHOUT_PRECOMPUTED);
    rsa_private_key_precompute(&k, a);
    if (kind != ONLY_D && (k.precomputed.dp == NULL || k.precomputed.dq == NULL ||
                           k.precomputed.qinv == NULL))
        testing_t_errorf_v(t,
                           "Precomputed values should not be nil after Precompute()");
    everything(t, a, &k);

    arena_free(&ar);
}

static void TestNotPrecomputed(TestingT *t) {
    min1024_default();
    testing_t_run(t, BURROW_S("OnlyD"),
                  BURROW_FN(TestingTFunc, not_precomputed, (void *)(intptr_t)ONLY_D));
    testing_t_run(t, BURROW_S("Primes"),
                  BURROW_FN(TestingTFunc, not_precomputed,
                            (void *)(intptr_t)WITHOUT_PRECOMPUTED));
    testing_t_run(
        t, BURROW_S("AllValues"),
        BURROW_FN(TestingTFunc, not_precomputed, (void *)(intptr_t)WITH_PRECOMPUTED));
}

typedef enum {
    MOD_PUBLIC_KEY,
    MOD_PRECOMPUTED,
    MOD_D_PLUS_2,
    MOD_D_ZERO,
    MOD_D_NIL,
    MOD_N_PLUS_2,
    MOD_N_ZERO,
    MOD_N_NIL,
    MOD_P_PLUS_2,
    MOD_P_ZERO,
    MOD_P_NIL,
    MOD_Q_PLUS_2,
    MOD_Q_ZERO,
    MOD_Q_NIL,
    MOD_E_PLUS_2,
    MOD_E_ZERO,
} Modification;

static const struct {
    const char *name;
    Modification m;
} modifications[] = {
    {"PublicKey mismatch", MOD_PUBLIC_KEY},
    {"Precomputed mismatch", MOD_PRECOMPUTED},
    {"D+2", MOD_D_PLUS_2},
    {"D=0", MOD_D_ZERO},
    {"D is nil", MOD_D_NIL},
    {"N+2", MOD_N_PLUS_2},
    {"N=0", MOD_N_ZERO},
    {"N is nil", MOD_N_NIL},
    {"P+2", MOD_P_PLUS_2},
    {"P=0", MOD_P_ZERO},
    {"P is nil", MOD_P_NIL},
    {"Q+2", MOD_Q_PLUS_2},
    {"Q=0", MOD_Q_ZERO},
    {"Q is nil", MOD_Q_NIL},
    {"E+2", MOD_E_PLUS_2},
    {"E=0", MOD_E_ZERO},
};

static BigInt *plus2(Alloc *a, const BigInt *x) {
    BigInt *r = big_new_int(a, 0);
    return big_int_add(r, x, big_new_int(a, 2));
}

static void modified_private_key(void *env, TestingT *t) {
    Modification m = (Modification)(intptr_t)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    RsaPrivateKey *base = key_named(a, "test512Key");
    RsaPrivateKey k = *base;
    k.primes = BURROW_NEW_N(a, BigInt *, 2);
    k.primes[0] = base->primes[0];
    k.primes[1] = base->primes[1];
    switch (m) {
    case MOD_PUBLIC_KEY:
        k.public_key = key_named(a, "test512KeyTwo")->public_key;
        break;
    case MOD_PRECOMPUTED:
        k.precomputed = key_named(a, "test512KeyTwo")->precomputed;
        break;
    case MOD_D_PLUS_2:
        k.d = plus2(a, k.d);
        break;
    case MOD_D_ZERO:
        k.d = big_new_int(a, 0);
        break;
    case MOD_D_NIL:
        k.d = NULL;
        break;
    case MOD_N_PLUS_2:
        k.public_key.n = plus2(a, k.public_key.n);
        break;
    case MOD_N_ZERO:
        k.public_key.n = big_new_int(a, 0);
        break;
    case MOD_N_NIL:
        k.public_key.n = NULL;
        break;
    case MOD_P_PLUS_2:
        k.primes[0] = plus2(a, k.primes[0]);
        break;
    case MOD_P_ZERO:
        k.primes[0] = big_new_int(a, 0);
        break;
    case MOD_P_NIL:
        k.primes[0] = NULL;
        break;
    case MOD_Q_PLUS_2:
        k.primes[1] = plus2(a, k.primes[1]);
        break;
    case MOD_Q_ZERO:
        k.primes[1] = big_new_int(a, 0);
        break;
    case MOD_Q_NIL:
        k.primes[1] = NULL;
        break;
    case MOD_E_PLUS_2:
        k.public_key.e += 2;
        break;
    case MOD_E_ZERO:
        k.public_key.e = 0;
        break;
    default:
        break;
    }
    if (!BURROW_FAILED(rsa_private_key_validate(&k)))
        testing_t_errorf_v(t, "Validate should have failed");
    rsa_private_key_precompute(&k, a);
    if (!BURROW_FAILED(rsa_private_key_validate(&k)))
        testing_t_errorf_v(t, "Validate should have failed after Precompute()");

    arena_free(&ar);
}

static void TestModifiedPrivateKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();
    if (BURROW_FAILED(rsa_private_key_validate(key_named(a, "test512Key"))))
        testing_t_fatalf_v(t, "test512Key should be valid");
    arena_free(&ar);

    for (size_t i = 0; i < sizeof modifications / sizeof modifications[0]; i++)
        testing_t_run(t, str_from_cstr(modifications[i].name),
                      BURROW_FN(TestingTFunc, modified_private_key,
                                (void *)(intptr_t)modifications[i].m));
}

/* ----------------------------------------------------------- PKCS #1 v1.5 */

static void TestDecryptPKCS1v15(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_off();
    RsaPrivateKey *k = key_named(a, "test512Key");

    for (int f = 0; f < 2; f++) {
        for (size_t i = 0;
             i < sizeof gen_decryptPKCS1v15Tests / sizeof gen_decryptPKCS1v15Tests[0];
             i++) {
            const GenInOut *test = &gen_decryptPKCS1v15Tests[i];
            Slice ct = decode_base64(a, test->in);
            Error err = BURROW_NO_ERROR;
            Slice out = f == 0
                            ? rsa_decrypt_pkcs1_v15(a, (IoReader){0}, k, ct, &err)
                            : rsa_private_key_decrypt(k, a, (IoReader){0}, ct,
                                                      (CryptoDecrypterOpts){0}, &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "#%d error decrypting: %s", (int)i,
                                   error_text(err));
            Slice want = cs(test->out);
            if (!bytes_equal(out, want))
                testing_t_errorf_v(t, "#%d got:%q want:%q", (int)i,
                                   str_from_bytes(out.p, out.len), test->out);
        }
    }

    min1024_default();
    arena_free(&ar);
}

/* quick.Check of tryEncryptDecrypt, with its 100 random messages of up to 49
 * bytes, or 10 in short mode, each decrypted with or without blinding. */
static void TestEncryptPKCS1v15(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();
    RsaPrivateKey *priv = key_named(a, "test1024Key");
    Int k = (big_int_bit_len(priv->public_key.n) + 7) / 8;

    int count = testing_short() ? 10 : 100;
    for (int i = 0; i < count; i++) {
        uint8_t r[2], buf[64];
        crypto_rand_read(bs(r, 2), NULL);
        Int n = r[0] % 50;
        bool blind = (r[1] & 1) != 0;
        crypto_rand_read(bs(buf, n), NULL);
        Slice in = bs(buf, n);
        if (in.len > k - 11)
            in.len = k - 11;

        Error err = BURROW_NO_ERROR;
        Slice ct =
            rsa_encrypt_pkcs1_v15(a, crypto_rand_reader, &priv->public_key, in, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "error encrypting: %s", error_text(err));
            break;
        }
        Slice pt = rsa_decrypt_pkcs1_v15(a, blind ? crypto_rand_reader : (IoReader){0},
                                         priv, ct, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "error decrypting: %s", error_text(err));
            break;
        }
        if (!bytes_equal(pt, in)) {
            testing_t_errorf_v(t, "output mismatch: %s %s", hexs(a, pt), hexs(a, in));
            break;
        }
        arena_reset(&ar);
        priv = key_named(a, "test1024Key");
    }
    arena_free(&ar);
}

static void TestEncryptPKCS1v15SessionKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_off();
    RsaPrivateKey *k = key_named(a, "test512Key");
    for (size_t i = 0; i < sizeof gen_decryptPKCS1v15SessionKeyTests /
                               sizeof gen_decryptPKCS1v15SessionKeyTests[0];
         i++) {
        const GenInOut *test = &gen_decryptPKCS1v15SessionKeyTests[i];
        uint8_t key[4] = {'F', 'A', 'I', 'L'};
        Error err = rsa_decrypt_pkcs1_v15_session_key(
            (IoReader){0}, k, decode_base64(a, test->in), bs(key, 4));
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d error decrypting", (int)i);
        if (!bytes_equal(bs(key, 4), cs(test->out)))
            testing_t_errorf_v(t, "#%d got:%q want:%q", (int)i, str_from_bytes(key, 4),
                               test->out);
    }
    min1024_default();
    arena_free(&ar);
}

static void TestEncryptPKCS1v15DecrypterSessionKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_off();
    RsaPrivateKey *k = key_named(a, "test512Key");
    RsaPKCS1v15DecryptOptions opts = {4};
    for (size_t i = 0; i < sizeof gen_decryptPKCS1v15SessionKeyTests /
                               sizeof gen_decryptPKCS1v15SessionKeyTests[0];
         i++) {
        const GenInOut *test = &gen_decryptPKCS1v15SessionKeyTests[i];
        Error err = BURROW_NO_ERROR;
        Slice pt = rsa_private_key_decrypt(
            k, a, crypto_rand_reader, decode_base64(a, test->in),
            rsa_pkcs1_v15_decrypt_options_as_decrypter_opts(&opts), &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "#%d: error decrypting: %s", (int)i, error_text(err));
        if (pt.len != 4)
            testing_t_fatalf_v(t, "#%d: incorrect length plaintext: got %d, want 4",
                               (int)i, pt.len);
        if (strcmp(test->out, "FAIL") != 0 && !bytes_equal(pt, cs(test->out)))
            testing_t_errorf_v(t, "#%d: incorrect plaintext: got %s, want %s", (int)i,
                               hexs(a, pt), hexs(a, cs(test->out)));
    }
    min1024_default();
    arena_free(&ar);
}

static void TestNonZeroRandomBytes(TestingT *t) {
    uint8_t b[512];
    Error err = burrow__rsa_non_zero_random_bytes(bs(b, sizeof b), crypto_rand_reader);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "returned error: %s", error_text(err));
    for (size_t i = 0; i < sizeof b; i++) {
        if (b[i] == 0) {
            testing_t_errorf_v(t, "Zero octet found");
            return;
        }
    }
}

static void TestSignPKCS1v15(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_off();
    RsaPrivateKey *k = key_named(a, "test512Key");
    for (size_t i = 0;
         i < sizeof gen_signPKCS1v15Tests / sizeof gen_signPKCS1v15Tests[0]; i++) {
        const GenInOut *test = &gen_signPKCS1v15Tests[i];
        Slice d = digest(a, CRYPTO_SHA1, cs(test->in));
        Error err = BURROW_NO_ERROR;
        Slice s = rsa_sign_pkcs1_v15(a, (IoReader){0}, k, CRYPTO_SHA1, d, &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d %s", (int)i, error_text(err));
        Slice expected = unhex(a, test->out);
        if (!bytes_equal(s, expected))
            testing_t_errorf_v(t, "#%d got: %s want: %s", (int)i, hexs(a, s),
                               hexs(a, expected));
    }
    min1024_default();
    arena_free(&ar);
}

static void TestVerifyPKCS1v15(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_off();
    RsaPrivateKey *k = key_named(a, "test512Key");
    for (size_t i = 0;
         i < sizeof gen_signPKCS1v15Tests / sizeof gen_signPKCS1v15Tests[0]; i++) {
        const GenInOut *test = &gen_signPKCS1v15Tests[i];
        Slice d = digest(a, CRYPTO_SHA1, cs(test->in));
        Error err =
            rsa_verify_pkcs1_v15(&k->public_key, CRYPTO_SHA1, d, unhex(a, test->out));
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d %s", (int)i, error_text(err));
    }
    min1024_default();
    arena_free(&ar);
}

static void TestOverlongMessagePKCS1v15(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_off();
    RsaPrivateKey *k = key_named(a, "test512Key");
    Slice ct = decode_base64(a, gen_overlong_ciphertext);
    Error err = BURROW_NO_ERROR;
    (void)rsa_decrypt_pkcs1_v15(a, (IoReader){0}, k, ct, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "RSA decrypted a message that was too long.");
    min1024_default();
    arena_free(&ar);
}

static void TestUnpaddedSignature(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_off();
    RsaPrivateKey *k = key_named(a, "test512Key");

    Slice msg = cs(gen_unpadded_msg);
    /* This base64 value was generated with:
     * % echo Thu Dec 19 18:06:16 EST 2013 > /tmp/msg
     * % openssl rsautl -sign -inkey key -out /tmp/sig -in /tmp/msg
     *
     * Where "key" contains the RSA private key test512Key. */
    Slice expected = decode_base64(a, gen_unpadded_sig);
    Error err = BURROW_NO_ERROR;
    Slice sig = rsa_sign_pkcs1_v15(a, (IoReader){0}, k, 0, msg, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SignPKCS1v15 failed: %s", error_text(err));
    if (!bytes_equal(sig, expected))
        testing_t_fatalf_v(t, "signature is not expected value: got %s, want %s",
                           hexs(a, sig), hexs(a, expected));
    err = rsa_verify_pkcs1_v15(&k->public_key, 0, msg, sig);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "signature failed to verify: %s", error_text(err));

    min1024_default();
    arena_free(&ar);
}

/* This tests that attempting to decrypt a session key where the ciphertext is
 * too small doesn't run outside the array bounds. */
static void TestShortSessionKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();
    RsaPrivateKey *k = key_named(a, "test1024Key");
    uint8_t one = 1;
    Error err = BURROW_NO_ERROR;
    Slice ct =
        rsa_encrypt_pkcs1_v15(a, crypto_rand_reader, &k->public_key, bs(&one, 1), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Failed to encrypt short message: %s", error_text(err));

    uint8_t key[32] = {0};
    err = rsa_decrypt_pkcs1_v15_session_key((IoReader){0}, k, ct, bs(key, sizeof key));
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "Failed to decrypt short message: %s", error_text(err));
    for (size_t i = 0; i < sizeof key; i++)
        if (key[i] != 0)
            testing_t_fatalf_v(t, "key was modified when ciphertext was invalid");
    arena_free(&ar);
}

static void TestShortPKCS1v15Signature(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();
    RsaPublicKey *pub = gen_public(a, gen_key_named("TestShortPKCS1v15Signature"));
    Slice sig = unhex(a, gen_short_pkcs1v15_sig);
    Slice h = digest(a, CRYPTO_SHA256, cs("hello"));
    if (!BURROW_FAILED(rsa_verify_pkcs1_v15(pub, CRYPTO_SHA256, h, sig)))
        testing_t_fatalf_v(t, "VerifyPKCS1v15 accepted a truncated signature");
    arena_free(&ar);
}

/* ------------------------------------------------------------------- PSS */

typedef enum { PSS_IDLE, PSS_N, PSS_E, PSS_SKIP, PSS_MSG, PSS_SALT, PSS_SIG } PssState;

typedef struct PssGolden {
    TestingT *t;
    Alloc *a;
    PssState state;
    int skip;
    RsaPublicKey key;
    Slice msg;
} PssGolden;

static const char new_key_marker[] = "START NEW KEY";
static const char new_signature_marker[] = "START NEW SIGNATURE";

/* What the loop over values in TestPSSGolden does with the next one, a value
 * or a marker. */
static void pss_golden_value(PssGolden *g, const char *v) {
    if (strcmp(v, new_key_marker) == 0) {
        g->state = PSS_N;
        return;
    }
    if (strcmp(v, new_signature_marker) == 0) {
        g->state = PSS_MSG;
        return;
    }
    RsaPSSOptions opts = {RSA_PSS_SALT_LENGTH_EQUALS_HASH, 0};
    switch (g->state) {
    case PSS_IDLE:
        testing_t_fatalf_v(g->t, "unknown marker: %s", v);
        return;
    case PSS_N:
        g->key.n = from_hex(g->a, v);
        g->state = PSS_E;
        break;
    case PSS_E:
        g->key.e = (Int)strtol(v, NULL, 16);
        /* We don't care for d, p, q, dP, dQ or qInv. */
        g->skip = 6;
        g->state = PSS_SKIP;
        break;
    case PSS_SKIP:
        if (--g->skip == 0)
            g->state = PSS_IDLE;
        break;
    case PSS_MSG:
        g->msg = unhex(g->a, v);
        g->state = PSS_SALT;
        break;
    case PSS_SALT:
        g->state = PSS_SIG;
        break;
    case PSS_SIG: {
        Slice hashed = digest(g->a, CRYPTO_SHA1, g->msg);
        Error err = rsa_verify_pss(&g->key, CRYPTO_SHA1, hashed, unhex(g->a, v), &opts);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(g->t, "%s", error_text(err));
        g->state = PSS_IDLE;
        break;
    }
    default:
        break;
    }
}

/* TestPSSGolden tests all the test vectors in pss-vect.txt from
 * ftp://ftp.rsasecurity.com/pub/pkcs/pkcs-1/pkcs-1v2-1-vec.zip */
static void TestPSSGolden(TestingT *t) {
    Arena ar, line_arena;
    arena_init(&ar, NULL, 0);
    arena_init(&line_arena, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();

    BytesReader f;
    bytes_reader_reset(&f, bs(gen_pss_vect_bz2, sizeof gen_pss_vect_bz2));
    BufioReader *scanner =
        bufio_new_reader(a, bzip2_new_reader(a, bytes_reader_as_io_reader(&f)));

    /* The file contains RSA keys and then a series of signatures. The lines
     * are merged into values here, with the spaces in the hex taken out, and
     * the starts of new keys and signature blocks marked. */
    PssGolden g = {t, a, PSS_IDLE, 0, {NULL, 0}, {0}};
    static char partial[8192];
    Int plen = 0;
    bool last_was_value = true;
    for (;;) {
        Error err = BURROW_NO_ERROR;
        Str line =
            bufio_reader_read_string(scanner, arena_allocator(&line_arena), '\n', &err);
        if (line.len == 0 && BURROW_FAILED(err)) {
            if (errors_is(err, io_eof))
                break;
            testing_t_fatalf_v(t, "%s", error_text(err));
        }
        if (line.len > 0 && line.p[line.len - 1] == '\n')
            line.len--;
        if (line.len > 0 && line.p[line.len - 1] == '\r')
            line.len--;

        if (line.len == 0) {
            if (plen > 0) {
                Int n = 0;
                for (Int i = 0; i < plen; i++)
                    if (partial[i] != ' ')
                        partial[n++] = partial[i];
                partial[n] = '\0';
                plen = 0;
                pss_golden_value(&g, partial);
                last_was_value = true;
            }
        } else if (strings_has_prefix(line, BURROW_S("# ======")) && last_was_value) {
            pss_golden_value(&g, new_key_marker);
            last_was_value = false;
        } else if (strings_has_prefix(line, BURROW_S("# ------")) && last_was_value) {
            pss_golden_value(&g, new_signature_marker);
            last_was_value = false;
        } else if (line.p[0] != '#') {
            if (plen + line.len >= (Int)sizeof partial)
                testing_t_fatalf_v(t, "value too long");
            memcpy(partial + plen, line.p, (size_t)line.len);
            plen += line.len;
        }
        arena_reset(&line_arena);
        if (BURROW_FAILED(err))
            break;
    }

    arena_free(&line_arena);
    arena_free(&ar);
}

/* TestPSSOpenSSL ensures that we can verify a PSS signature from OpenSSL with
 * the default options. OpenSSL sets the salt length to be maximal. */
static void TestPSSOpenSSL(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_off();
    RsaPrivateKey *k = key_named(a, "test512Key");
    Slice hashed = digest(a, CRYPTO_SHA256, cs("testing"));
    /* Generated with `echo -n testing | openssl dgst -sign key.pem -sigopt
     * rsa_padding_mode:pss -sha256 > sig` */
    Slice sig = unhex(a, gen_pss_openssl_sig);
    Error err = rsa_verify_pss(&k->public_key, CRYPTO_SHA256, hashed, sig, NULL);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s", error_text(err));
    min1024_default();
    arena_free(&ar);
}

static void TestPSSNilOpts(TestingT *t) {
    (void)t;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();
    RsaPrivateKey *k = key_named(a, "test1024Key");
    Slice hashed = digest(a, CRYPTO_SHA256, cs("testing"));
    Error err = BURROW_NO_ERROR;
    (void)rsa_sign_pss(a, crypto_rand_reader, k, CRYPTO_SHA256, hashed, NULL, &err);
    arena_free(&ar);
}

static void TestPSSSigning(TestingT *t) {
    /* The good column is the one Go has outside FIPS mode, where
     * PSSSaltLengthAuto is not capped at PSSSaltLengthEqualsHash. */
    static const struct {
        Int sign_salt_length, verify_salt_length;
        bool good;
    } combinations[] = {
        {RSA_PSS_SALT_LENGTH_AUTO, RSA_PSS_SALT_LENGTH_AUTO, true},
        {RSA_PSS_SALT_LENGTH_EQUALS_HASH, RSA_PSS_SALT_LENGTH_AUTO, true},
        {RSA_PSS_SALT_LENGTH_EQUALS_HASH, RSA_PSS_SALT_LENGTH_EQUALS_HASH, true},
        {RSA_PSS_SALT_LENGTH_EQUALS_HASH, 8, false},
        {8, 8, true},
        {8, RSA_PSS_SALT_LENGTH_AUTO, true},
        {42, RSA_PSS_SALT_LENGTH_AUTO, true},
        {RSA_PSS_SALT_LENGTH_AUTO, RSA_PSS_SALT_LENGTH_EQUALS_HASH, false},
        {RSA_PSS_SALT_LENGTH_AUTO, 106, true},
        {RSA_PSS_SALT_LENGTH_AUTO, 20, false},
        {RSA_PSS_SALT_LENGTH_AUTO, -2, false},
    };
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();
    RsaPrivateKey *k = key_named(a, "test1024Key");
    Slice hashed = digest(a, CRYPTO_SHA1, cs("testing"));
    RsaPSSOptions opts = {0, 0};

    for (size_t i = 0; i < sizeof combinations / sizeof combinations[0]; i++) {
        opts.salt_length = combinations[i].sign_salt_length;
        Error err = BURROW_NO_ERROR;
        Slice sig =
            rsa_sign_pss(a, crypto_rand_reader, k, CRYPTO_SHA1, hashed, &opts, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d: error while signing: %s", (int)i,
                               error_text(err));
            continue;
        }
        opts.salt_length = combinations[i].verify_salt_length;
        err = rsa_verify_pss(&k->public_key, CRYPTO_SHA1, hashed, sig, &opts);
        if (!BURROW_FAILED(err) != combinations[i].good)
            testing_t_errorf_v(t, "#%d: bad result, wanted: %t, got: %s", (int)i,
                               combinations[i].good, error_text(err));
    }
    arena_free(&ar);
}

/* See Issue 42741, and separately, RFC 8017: "Note that the octet length of EM
 * will be one less than k if modBits - 1 is divisible by 8 and equal to k
 * otherwise, where k is the length in octets of the RSA modulus n." */
static void TestPSS513(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_off();
    Error err = BURROW_NO_ERROR;
    RsaPrivateKey *key = rsa_generate_key(a, crypto_rand_reader, 513, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    Slice d = digest(a, CRYPTO_SHA256, cs("message"));
    RsaPSSOptions opts = {RSA_PSS_SALT_LENGTH_AUTO, CRYPTO_SHA256};
    Slice signature = rsa_private_key_sign(key, a, crypto_rand_reader, d,
                                           rsa_pss_options_as_signer_opts(&opts), &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    err = rsa_verify_pss(&key->public_key, CRYPTO_SHA256, d, signature, NULL);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "%s", error_text(err));
    min1024_default();
    arena_free(&ar);
}

static void TestInvalidPSSSaltLength(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_off();
    Error err = BURROW_NO_ERROR;
    RsaPrivateKey *key = rsa_generate_key(a, crypto_rand_reader, 245, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));

    Slice d = digest(a, CRYPTO_SHA256, cs("message"));
    RsaPSSOptions opts = {-2, CRYPTO_SHA256};
    (void)rsa_sign_pss(a, crypto_rand_reader, key, CRYPTO_SHA256, d, &opts, &err);
    const char *want = "crypto/rsa: invalid PSS salt length";
    if (!str_eq(error_text(err), str_from_cstr(want)))
        testing_t_fatalf_v(t, "SignPSS unexpected error: got %s, want %s",
                           error_text(err), want);

    /* Only that there is an error, since Go's crypto/rsa and
     * crypto/internal/boring give different ones. */
    static const uint8_t three[] = {1, 2, 3};
    uint8_t sig[31] = {0};
    RsaPSSOptions verify = {-2, 0};
    if (!BURROW_FAILED(rsa_verify_pss(&key->public_key, CRYPTO_SHA256, bs(three, 3),
                                      bs(sig, sizeof sig), &verify)))
        testing_t_fatalf_v(t, "VerifyPSS unexpected success");
    min1024_default();
    arena_free(&ar);
}

static void TestHashOverride(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();
    RsaPrivateKey *k = key_named(a, "test2048Key");
    Slice d = digest(a, CRYPTO_SHA512, cs("message"));
    /* opts.Hash overrides the passed hash argument. */
    RsaPSSOptions sign = {0, CRYPTO_SHA512};
    Error err = BURROW_NO_ERROR;
    Slice sig = rsa_sign_pss(a, crypto_rand_reader, k, CRYPTO_SHA256, d, &sign, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "SignPSS unexpected error: got %s, want nil",
                           error_text(err));

    /* VerifyPSS has the inverse behavior, opts.Hash is always ignored, check
     * this is true. */
    RsaPSSOptions verify = {0, CRYPTO_SHA256};
    err = rsa_verify_pss(&k->public_key, CRYPTO_SHA512, d, sig, &verify);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "VerifyPSS unexpected error: got %s, want nil",
                           error_text(err));
    arena_free(&ar);
}

/* ----------------------------------------------------------------- Equal */

static void TestEqual(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_off();

    RsaPrivateKey *priv = key_named(a, "test512Key");
    const RsaPublicKey *pub = &priv->public_key;

    if (!rsa_public_key_equal(pub, any_public(pub)))
        testing_t_errorf_v(t, "public key is not equal to itself");
    CryptoSigner signer = rsa_private_key_signer(priv);
    if (!rsa_public_key_equal(pub, crypto_signer_public(signer)))
        testing_t_errorf_v(t, "private.Public() is not Equal to public");
    if (!rsa_private_key_equal(priv, any_private(priv)))
        testing_t_errorf_v(t, "private key is not equal to itself");

    Error err = BURROW_NO_ERROR;
    Slice enc = pkcs8_marshal(a, priv, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    RsaPrivateKey *decoded = pkcs8_parse(a, enc, &err);
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "%s", error_text(err));
    if (!rsa_public_key_equal(pub, rsa_private_key_public(decoded)))
        testing_t_errorf_v(t, "public key is not equal to itself after decoding");
    if (!rsa_private_key_equal(priv, any_private(decoded)))
        testing_t_errorf_v(t, "private key is not equal to itself after decoding");

    RsaPrivateKey *other = key_named(a, "test512KeyTwo");
    if (rsa_public_key_equal(pub, rsa_private_key_public(other)))
        testing_t_errorf_v(t, "different public keys are Equal");
    if (rsa_private_key_equal(priv, any_private(other)))
        testing_t_errorf_v(t, "different private keys are Equal");

    RsaPrivateKey no_precomp = *priv;
    memset(&no_precomp.precomputed, 0, sizeof no_precomp.precomputed);
    if (!rsa_private_key_equal(priv, any_private(&no_precomp)))
        testing_t_errorf_v(t,
                           "private key with no precomputation is not equal to itself");

    min1024_default();
    arena_free(&ar);
}

/* ------------------------------------------------------------ Wycheproof */

static void wycheproof_name(char *buf, size_t n, const char *file,
                            const GenWycheproof *tv) {
    if (tv->comment[0] != '\0')
        snprintf(buf, n, "%s #%d %s", file, tv->tc_id, tv->comment);
    else
        snprintf(buf, n, "%s #%d", file, tv->tc_id);
}

static void TestRSAOAEPDecryptWycheproof(TestingT *t) {
    Arena ar, keys;
    arena_init(&ar, NULL, 0);
    arena_init(&keys, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();

    int group = -1;
    RsaPrivateKey *priv = NULL;
    for (size_t i = 0; i < sizeof gen_wycheproof_oaep / sizeof gen_wycheproof_oaep[0];
         i++) {
        const GenWycheproof *tv = &gen_wycheproof_oaep[i];
        const GenOaepGroup *tg = &gen_wycheproof_oaep_groups[tv->group];
        if (tv->group != group) {
            arena_reset(&keys);
            priv = gen_private(arena_allocator(&keys), &tg->key);
            group = tv->group;
        }
        char name[512];
        wycheproof_name(name, sizeof name, tg->file, tv);

        RsaOAEPOptions opts = {tg->hash, tg->mgf_hash, unhex(a, tv->label)};
        Error err = BURROW_NO_ERROR;
        Slice plaintext =
            rsa_private_key_decrypt(priv, a, (IoReader){0}, unhex_parts(a, tv->data),
                                    rsa_oaep_options_as_decrypter_opts(&opts), &err);
        if (tv->want) {
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "%s: expected success: %s", name,
                                   error_text(err));
            else if (!bytes_equal(plaintext, unhex(a, tv->msg)))
                testing_t_errorf_v(t, "%s: unexpected plaintext: got %s, want %s", name,
                                   hexs(a, plaintext), tv->msg);
        } else if (!BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: expected failure", name);
        }
        arena_reset(&ar);
    }

    arena_free(&keys);
    arena_free(&ar);
}

static void TestRSAPKCS1DecryptWycheproof(TestingT *t) {
    Arena ar, keys;
    arena_init(&ar, NULL, 0);
    arena_init(&keys, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();

    int group = -1;
    RsaPrivateKey *priv = NULL;
    for (size_t i = 0; i < sizeof gen_wycheproof_pkcs1_decrypt /
                               sizeof gen_wycheproof_pkcs1_decrypt[0];
         i++) {
        const GenWycheproof *tv = &gen_wycheproof_pkcs1_decrypt[i];
        const GenDecryptGroup *tg = &gen_wycheproof_pkcs1_decrypt_groups[tv->group];
        if (tv->group != group) {
            arena_reset(&keys);
            priv = gen_private(arena_allocator(&keys), &tg->key);
            group = tv->group;
        }
        char name[512];
        wycheproof_name(name, sizeof name, tg->file, tv);

        Error err = BURROW_NO_ERROR;
        Slice plaintext = rsa_decrypt_pkcs1_v15(a, (IoReader){0}, priv,
                                                unhex_parts(a, tv->data), &err);
        if (BURROW_FAILED(err)) {
            if (tv->want)
                testing_t_errorf_v(t, "%s: DecryptPKCS1v15: %s", name, error_text(err));
        } else if (!tv->want) {
            testing_t_errorf_v(t, "%s: DecryptPKCS1v15 unexpectedly succeeded", name);
        } else if (!bytes_equal(plaintext, unhex(a, tv->msg))) {
            testing_t_errorf_v(t, "%s: plaintext mismatch: got %s, want %s", name,
                               hexs(a, plaintext), tv->msg);
        }
        arena_reset(&ar);
    }

    arena_free(&keys);
    arena_free(&ar);
}

static void TestRSAPKCS1SignaturesWycheproof(TestingT *t) {
    Arena ar, keys;
    arena_init(&ar, NULL, 0);
    arena_init(&keys, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();

    int group = -1;
    RsaPublicKey *pub = NULL;
    for (size_t i = 0;
         i < sizeof gen_wycheproof_pkcs1_sig / sizeof gen_wycheproof_pkcs1_sig[0];
         i++) {
        const GenWycheproof *tv = &gen_wycheproof_pkcs1_sig[i];
        const GenVerifyGroup *tg = &gen_wycheproof_pkcs1_sig_groups[tv->group];
        if (tv->group != group) {
            arena_reset(&keys);
            pub = gen_public(arena_allocator(&keys), &tg->key);
            group = tv->group;
        }
        Slice hashed = digest(a, tg->hash, unhex(a, tv->msg));
        Error err =
            rsa_verify_pkcs1_v15(pub, tg->hash, hashed, unhex_parts(a, tv->data));
        if (!BURROW_FAILED(err) != tv->want) {
            char name[512];
            wycheproof_name(name, sizeof name, tg->file, tv);
            testing_t_errorf_v(t, "%s: wanted success: %t err: %s", name, tv->want,
                               error_text(err));
        }
        arena_reset(&ar);
    }

    arena_free(&keys);
    arena_free(&ar);
}

/* Go runs each group twice, first with PSSSaltLengthAuto and then, in name,
 * with the group's salt length, but it makes opts again for the second run, so
 * that both verify with PSSSaltLengthAuto, and want is what the vectors give
 * after Go's overrides for that. Here both runs are the same too. */
static void TestRSAPSSSignaturesWycheproof(TestingT *t) {
    Arena ar, keys;
    arena_init(&ar, NULL, 0);
    arena_init(&keys, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    min1024_default();

    size_t n = sizeof gen_wycheproof_pss / sizeof gen_wycheproof_pss[0];
    size_t start = 0;
    while (start < n) {
        int group = gen_wycheproof_pss[start].group;
        const GenVerifyGroup *tg = &gen_wycheproof_pss_groups[group];
        size_t end = start;
        while (end < n && gen_wycheproof_pss[end].group == group)
            end++;
        arena_reset(&keys);
        RsaPublicKey *pub = gen_public(arena_allocator(&keys), &tg->key);
        RsaPSSOptions opts = {RSA_PSS_SALT_LENGTH_AUTO, tg->hash};
        for (int run = 0; run < 2; run++) {
            const char *salt_label = run == 0 ? "autoSalt" : "vecSalt";
            for (size_t i = start; i < end; i++) {
                const GenWycheproof *tv = &gen_wycheproof_pss[i];
                Slice hashed = digest(a, tg->hash, unhex(a, tv->msg));
                Error err = rsa_verify_pss(pub, tg->hash, hashed,
                                           unhex_parts(a, tv->data), &opts);
                if (!BURROW_FAILED(err) != tv->want) {
                    char name[512];
                    wycheproof_name(name, sizeof name, tg->file, tv);
                    testing_t_errorf_v(t, "%s %s: wanted success: %t err: %s", name,
                                       salt_label, tv->want, error_text(err));
                }
                arena_reset(&ar);
            }
        }
        start = end;
    }

    arena_free(&keys);
    arena_free(&ar);
}

/* ----------------------------------------------- the FIPS module's own */

/* decodeHex, which takes an odd number of digits too. */
static Slice decode_hex(Alloc *a, const char *s) {
    size_t n = strlen(s);
    if (n % 2 == 0)
        return unhex(a, s);
    char *p = mem_alloc(a, n + 2, 1);
    p[0] = '0';
    memcpy(p + 1, s, n + 1);
    return unhex(a, p);
}

/* s with zeros in front to make it n digits long. */
static const char *pad_hex(Alloc *a, const char *s, size_t n) {
    size_t l = strlen(s);
    if (l >= n)
        return s;
    char *p = mem_alloc(a, n + 1, 1);
    memset(p, '0', n - l);
    memcpy(p + n - l, s, l + 1);
    return p;
}

static void miller_rabin(void *env, TestingT *t) {
    const GenMillerRabin *v = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    const char *w_hex = v->w;
    if (strlen(w_hex) % 2 != 0)
        w_hex = pad_hex(a, w_hex, strlen(w_hex) + 1);
    const char *b_hex = pad_hex(a, v->b, strlen(w_hex));

    Slice w = decode_hex(a, w_hex);
    if (w.len == 0 || bytes_of(w)[w.len - 1] % 4 != 3) {
        arena_free(&ar);
        testing_t_skip_v(t, "skipping test with W not congruent to 3 mod 4");
    }

    Error err = BURROW_NO_ERROR;
    RsaMillerRabin *mr = burrow__rsa_miller_rabin_setup(a, w, &err);
    if (BURROW_FAILED(err)) {
        testing_t_logf_v(t, "W = %s", w_hex);
        testing_t_logf_v(t, "B = %s", b_hex);
        testing_t_fatalf_v(t, "failed to set up Miller-Rabin test: %s",
                           error_text(err));
    }
    bool result = false;
    err = burrow__rsa_miller_rabin_iteration(mr, decode_hex(a, b_hex), &result);
    if (BURROW_FAILED(err)) {
        testing_t_logf_v(t, "W = %s", w_hex);
        testing_t_logf_v(t, "B = %s", b_hex);
        testing_t_fatalf_v(t, "failed to run Miller-Rabin test: %s", error_text(err));
    }
    if (result != v->possibly_prime) {
        testing_t_logf_v(t, "W = %s", w_hex);
        testing_t_logf_v(t, "B = %s", b_hex);
        testing_t_fatalf_v(t, "unexpected result: got %t, want %t", result,
                           v->possibly_prime);
    }
    arena_free(&ar);
}

static void TestMillerRabin(TestingT *t) {
    for (size_t i = 0; i < sizeof gen_miller_rabin / sizeof gen_miller_rabin[0]; i++) {
        char name[32];
        snprintf(name, sizeof name, "line %d", gen_miller_rabin[i].line);
        testing_t_run(t, str_from_cstr(name),
                      BURROW_FN(TestingTFunc, miller_rabin,
                                (void *)(uintptr_t)&gen_miller_rabin[i]));
    }
}

/* addOne. */
static Slice add_one(Alloc *a, Slice b) {
    BigInt *x = big_new_int(a, 0);
    big_int_set_bytes(x, b);
    big_int_add(x, x, big_new_int(a, 1));
    return big_int_bytes(x, a);
}

/* hex.EncodeToString(b) with the zeros in front trimmed. */
static Str trimmed_hex(Alloc *a, Slice b) {
    Str s = hexs(a, b);
    while (s.len > 0 && s.p[0] == '0') {
        s.p++;
        s.len--;
    }
    return s;
}

static void totient(void *env, TestingT *t) {
    const GenGcdLcm *v = env;
    if (strcmp(v->a, "0") == 0 || strcmp(v->b, "0") == 0)
        testing_t_skip_v(t, "skipping test with zero input");
    if (strcmp(v->lcm, "1") == 0)
        testing_t_skip_v(t, "skipping test with LCM=1");

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    Slice ab = decode_hex(a, v->a), bb = decode_hex(a, v->b);
    Slice pb = add_one(a, ab), qb = add_one(a, bb);
    Error err = BURROW_NO_ERROR;
    BigmodModulus *p = bigmod_new_modulus(a, pb, &err);
    BigmodNat *an = bigmod_nat_set_bytes(bigmod_new_nat(a), ab, p, &err);
    err = BURROW_NO_ERROR;
    BigmodModulus *q = bigmod_new_modulus(a, qb, &err);
    BigmodNat *bn = bigmod_nat_set_bytes(bigmod_new_nat(a), bb, q, &err);

    err = BURROW_NO_ERROR;
    BigmodNat *gcd = bigmod_nat_gcd_var_time(bigmod_new_nat(a), an, bn, &err);
    if (!BURROW_FAILED(err)) {
        Str got = trimmed_hex(a, bigmod_nat_bytes(gcd, a, p));
        if (!str_eq(got, str_from_cstr(v->gcd)))
            testing_t_fatalf_v(t, "unexpected GCD: got %s, want %s", got, v->gcd);
    }

    if (((const Uint *)bigmod_nat_bits(an).p)[0] % 4 != 2 ||
        ((const Uint *)bigmod_nat_bits(bn).p)[0] % 4 != 2) {
        arena_free(&ar);
        testing_t_skip_v(t, "skipping test with invalid input for totient");
    }

    err = BURROW_NO_ERROR;
    Slice lcm = burrow__rsa_totient(a, pb, qb, &err);
    if (big_int_bit_len(from_hex(a, v->gcd)) > 32) {
        if (!errors_is(err, burrow__rsa_err_divisor_too_large))
            testing_t_fatalf_v(t, "expected divisor too large error, got %s",
                               error_text(err));
        arena_free(&ar);
        testing_t_skip_v(t, "GCD too large");
    }
    if (BURROW_FAILED(err))
        testing_t_fatalf_v(t, "failed to calculate totient: %s", error_text(err));
    Str got = trimmed_hex(a, lcm);
    if (!str_eq(got, str_from_cstr(v->lcm)))
        testing_t_fatalf_v(t, "unexpected LCM: got %s, want %s", got, v->lcm);
    arena_free(&ar);
}

static void TestTotient(TestingT *t) {
    for (size_t i = 0; i < sizeof gen_gcd_lcm / sizeof gen_gcd_lcm[0]; i++) {
        char name[32];
        snprintf(name, sizeof name, "line %d", gen_gcd_lcm[i].line);
        testing_t_run(
            t, str_from_cstr(name),
            BURROW_FN(TestingTFunc, totient, (void *)(uintptr_t)&gen_gcd_lcm[i]));
    }
}

static void TestHashPrefixes(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof gen_prefixes / sizeof gen_prefixes[0]; i++) {
        Str name = crypto_hash_string(gen_prefixes[i].hash, a);
        Slice got = {0};
        (void)burrow__rsa_hash_prefix(name, &got);
        Slice want = unhex(a, gen_prefixes[i].prefix);
        if (!bytes_equal(got, want))
            testing_t_errorf_v(t, "%s: got %s, want %s", name, hexs(a, got),
                               hexs(a, want));
    }
    arena_free(&ar);
}

/* Test vector in file pss-int.txt from
 * ftp://ftp.rsasecurity.com/pub/pkcs/pkcs-1/pkcs-1v2-1-vec.zip */
static void TestEMSAPSS(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice msg = unhex(a, gen_emsa_pss_msg), salt = unhex(a, gen_emsa_pss_salt);
    Slice expected = unhex(a, gen_emsa_pss_expected);
    Slice hashed = digest(a, CRYPTO_SHA1, msg);

    Error err = BURROW_NO_ERROR;
    Slice encoded =
        burrow__rsa_emsa_pss_encode(a, hashed, 1023, salt, sha1_new(a), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Error from emsaPSSEncode: %s", error_text(err));
    if (!bytes_equal(encoded, expected))
        testing_t_errorf_v(t, "Bad encoding. got %s, want %s", hexs(a, encoded),
                           hexs(a, expected));
    err = burrow__rsa_emsa_pss_verify(hashed, encoded, 1023, salt.len, sha1_new(a));
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Bad verification: %s", error_text(err));
    arena_free(&ar);
}

/* ------------------------------------------------------------ benchmarks */

static IoReader buffered_rand(Alloc *a) {
    return bufio_reader_as_io_reader(
        bufio_new_reader_size(a, crypto_rand_reader, 1 << 15));
}

static void bench_decrypt_pkcs1v15(void *env, TestingB *b) {
    Arena keys, ar;
    arena_init(&keys, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *ka = arena_allocator(&keys), *a = arena_allocator(&ar);
    RsaPrivateKey *k = key_named(ka, env);
    IoReader r = buffered_rand(ka);

    Slice m = cs("Hello Gophers");
    Error err = BURROW_NO_ERROR;
    Slice c = rsa_encrypt_pkcs1_v15(ka, r, &k->public_key, m, &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "%s", error_text(err));

    testing_b_reset_timer(b);
    uint8_t sink = 0;
    for (Int i = 0; i < testing_b_n(b); i++) {
        Slice p = rsa_decrypt_pkcs1_v15(a, r, k, c, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        if (!bytes_equal(p, m))
            testing_b_fatalf_v(b, "unexpected output: %s", hexs(a, p));
        sink ^= bytes_of(p)[0];
        arena_reset(&ar);
    }
    (void)sink;
    arena_free(&ar);
    arena_free(&keys);
}

static void BenchmarkDecryptPKCS1v15(TestingB *b) {
    testing_b_run(b, BURROW_S("2048"),
                  BURROW_FN(TestingBFunc, bench_decrypt_pkcs1v15,
                            (void *)(uintptr_t)"test2048Key"));
    testing_b_run(b, BURROW_S("3072"),
                  BURROW_FN(TestingBFunc, bench_decrypt_pkcs1v15,
                            (void *)(uintptr_t)"test3072Key"));
    testing_b_run(b, BURROW_S("4096"),
                  BURROW_FN(TestingBFunc, bench_decrypt_pkcs1v15,
                            (void *)(uintptr_t)"test4096Key"));
}

static void bench_encrypt_pkcs1v15(void *env, TestingB *b) {
    (void)env;
    Arena keys, ar;
    arena_init(&keys, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *ka = arena_allocator(&keys), *a = arena_allocator(&ar);
    RsaPrivateKey *k = key_named(ka, "test2048Key");
    IoReader r = buffered_rand(ka);
    Slice m = cs("Hello Gophers");

    uint8_t sink = 0;
    for (Int i = 0; i < testing_b_n(b); i++) {
        Error err = BURROW_NO_ERROR;
        Slice c = rsa_encrypt_pkcs1_v15(a, r, &k->public_key, m, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        sink ^= bytes_of(c)[0];
        arena_reset(&ar);
    }
    (void)sink;
    arena_free(&ar);
    arena_free(&keys);
}

static void BenchmarkEncryptPKCS1v15(TestingB *b) {
    testing_b_run(b, BURROW_S("2048"),
                  BURROW_FN(TestingBFunc, bench_encrypt_pkcs1v15, NULL));
}

static void bench_decrypt_oaep(void *env, TestingB *b) {
    (void)env;
    Arena keys, ar;
    arena_init(&keys, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *ka = arena_allocator(&keys), *a = arena_allocator(&ar);
    RsaPrivateKey *k = key_named(ka, "test2048Key");
    IoReader r = buffered_rand(ka);

    Slice m = cs("Hello Gophers");
    Error err = BURROW_NO_ERROR;
    Slice c =
        rsa_encrypt_oaep(ka, sha256_new(ka), r, &k->public_key, m, (Slice){0}, &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "%s", error_text(err));

    testing_b_reset_timer(b);
    uint8_t sink = 0;
    for (Int i = 0; i < testing_b_n(b); i++) {
        Slice p = rsa_decrypt_oaep(a, sha256_new(a), r, k, c, (Slice){0}, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        if (!bytes_equal(p, m))
            testing_b_fatalf_v(b, "unexpected output: %s", hexs(a, p));
        sink ^= bytes_of(p)[0];
        arena_reset(&ar);
    }
    (void)sink;
    arena_free(&ar);
    arena_free(&keys);
}

static void BenchmarkDecryptOAEP(TestingB *b) {
    testing_b_run(b, BURROW_S("2048"),
                  BURROW_FN(TestingBFunc, bench_decrypt_oaep, NULL));
}

static void bench_encrypt_oaep(void *env, TestingB *b) {
    (void)env;
    Arena keys, ar;
    arena_init(&keys, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *ka = arena_allocator(&keys), *a = arena_allocator(&ar);
    RsaPrivateKey *k = key_named(ka, "test2048Key");
    IoReader r = buffered_rand(ka);
    Slice m = cs("Hello Gophers");

    uint8_t sink = 0;
    for (Int i = 0; i < testing_b_n(b); i++) {
        Error err = BURROW_NO_ERROR;
        Slice c =
            rsa_encrypt_oaep(a, sha256_new(a), r, &k->public_key, m, (Slice){0}, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        sink ^= bytes_of(c)[0];
        arena_reset(&ar);
    }
    (void)sink;
    arena_free(&ar);
    arena_free(&keys);
}

static void BenchmarkEncryptOAEP(TestingB *b) {
    testing_b_run(b, BURROW_S("2048"),
                  BURROW_FN(TestingBFunc, bench_encrypt_oaep, NULL));
}

typedef enum { SIGN_KEY, SIGN_ONLY_D, SIGN_PRIMES, SIGN_ALL_VALUES } SignKey;

static void bench_sign_pkcs1v15(void *env, TestingB *b) {
    SignKey which = (SignKey)(intptr_t)env;
    Arena keys, ar;
    arena_init(&keys, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *ka = arena_allocator(&keys), *a = arena_allocator(&ar);
    RsaPrivateKey k;
    if (which == SIGN_KEY)
        k = *key_named(ka, "test2048Key");
    else
        k = test2048_variant(ka, which == SIGN_ONLY_D   ? ONLY_D
                                 : which == SIGN_PRIMES ? WITHOUT_PRECOMPUTED
                                                        : WITH_PRECOMPUTED);
    Slice hashed = digest(ka, CRYPTO_SHA256, cs("testing"));

    uint8_t sink = 0;
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Error err = BURROW_NO_ERROR;
        Slice s =
            rsa_sign_pkcs1_v15(a, crypto_rand_reader, &k, CRYPTO_SHA256, hashed, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        sink ^= bytes_of(s)[0];
        arena_reset(&ar);
    }
    (void)sink;
    arena_free(&ar);
    arena_free(&keys);
}

static void BenchmarkSignPKCS1v15(TestingB *b) {
    testing_b_run(
        b, BURROW_S("2048"),
        BURROW_FN(TestingBFunc, bench_sign_pkcs1v15, (void *)(intptr_t)SIGN_KEY));
    testing_b_run(
        b, BURROW_S("2048/noprecomp/OnlyD"),
        BURROW_FN(TestingBFunc, bench_sign_pkcs1v15, (void *)(intptr_t)SIGN_ONLY_D));
    testing_b_run(
        b, BURROW_S("2048/noprecomp/Primes"),
        BURROW_FN(TestingBFunc, bench_sign_pkcs1v15, (void *)(intptr_t)SIGN_PRIMES));
    /* This is different from "2048" because it's only the public precomputed
     * values, and not the ones underneath. */
    testing_b_run(b, BURROW_S("2048/noprecomp/AllValues"),
                  BURROW_FN(TestingBFunc, bench_sign_pkcs1v15,
                            (void *)(intptr_t)SIGN_ALL_VALUES));
}

static void bench_verify_pkcs1v15(void *env, TestingB *b) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    RsaPrivateKey *k = key_named(a, "test2048Key");
    Slice hashed = digest(a, CRYPTO_SHA256, cs("testing"));
    Error err = BURROW_NO_ERROR;
    Slice s = rsa_sign_pkcs1_v15(a, crypto_rand_reader, k, CRYPTO_SHA256, hashed, &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "%s", error_text(err));

    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        err = rsa_verify_pkcs1_v15(&k->public_key, CRYPTO_SHA256, hashed, s);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
    }
    arena_free(&ar);
}

static void BenchmarkVerifyPKCS1v15(TestingB *b) {
    testing_b_run(b, BURROW_S("2048"),
                  BURROW_FN(TestingBFunc, bench_verify_pkcs1v15, NULL));
}

static void bench_sign_pss(void *env, TestingB *b) {
    (void)env;
    Arena keys, ar;
    arena_init(&keys, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *ka = arena_allocator(&keys), *a = arena_allocator(&ar);
    RsaPrivateKey *k = key_named(ka, "test2048Key");
    Slice hashed = digest(ka, CRYPTO_SHA256, cs("testing"));

    uint8_t sink = 0;
    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        Error err = BURROW_NO_ERROR;
        Slice s =
            rsa_sign_pss(a, crypto_rand_reader, k, CRYPTO_SHA256, hashed, NULL, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        sink ^= bytes_of(s)[0];
        arena_reset(&ar);
    }
    (void)sink;
    arena_free(&ar);
    arena_free(&keys);
}

static void BenchmarkSignPSS(TestingB *b) {
    testing_b_run(b, BURROW_S("2048"), BURROW_FN(TestingBFunc, bench_sign_pss, NULL));
}

static void bench_verify_pss(void *env, TestingB *b) {
    (void)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    RsaPrivateKey *k = key_named(a, "test2048Key");
    Slice hashed = digest(a, CRYPTO_SHA256, cs("testing"));
    Error err = BURROW_NO_ERROR;
    Slice s = rsa_sign_pss(a, crypto_rand_reader, k, CRYPTO_SHA256, hashed, NULL, &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "%s", error_text(err));

    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        err = rsa_verify_pss(&k->public_key, CRYPTO_SHA256, hashed, s, NULL);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
    }
    arena_free(&ar);
}

static void BenchmarkVerifyPSS(TestingB *b) {
    testing_b_run(b, BURROW_S("2048"), BURROW_FN(TestingBFunc, bench_verify_pss, NULL));
}

/* Go parses the DER in test2048KeyPEM, and this the PKCS #8 pkcs8_marshal
 * writes for the same key. */
static void bench_parse_pkcs8(void *env, TestingB *b) {
    (void)env;
    Arena keys, ar;
    arena_init(&keys, NULL, 0);
    arena_init(&ar, NULL, 0);
    Alloc *ka = arena_allocator(&keys), *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Slice der = pkcs8_marshal(ka, key_named(ka, "test2048Key"), &err);
    if (BURROW_FAILED(err))
        testing_b_fatalf_v(b, "%s", error_text(err));

    testing_b_reset_timer(b);
    for (Int i = 0; i < testing_b_n(b); i++) {
        (void)pkcs8_parse(a, der, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        arena_reset(&ar);
    }
    arena_free(&ar);
    arena_free(&keys);
}

static void BenchmarkParsePKCS8PrivateKey(TestingB *b) {
    testing_b_run(b, BURROW_S("2048"),
                  BURROW_FN(TestingBFunc, bench_parse_pkcs8, NULL));
}

/* benchmarkPrimeReader feeds prime candidates from the lines of a keygen file,
 * one per line in hex, to GenerateKey. */
typedef struct PrimeReader {
    const char *const *lines;
    Int n, next;
} PrimeReader;

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static Int prime_read(void *self, Slice p, Error *err) {
    PrimeReader *r = self;
    /* Neutralize randutil.MaybeReadByte. */
    if (p.len == 1)
        return 1;
    if (r->next >= r->n) {
        *err = io_eof;
        return 0;
    }
    const char *line = r->lines[r->next++];
    size_t l = strlen(line);
    if ((Int)(l / 2) != p.len) {
        *err = errors_new(error_allocator(), BURROW_S("unexpected read length"));
        return 0;
    }
    for (Int i = 0; i < p.len; i++) {
        int hi = hex_nibble(line[2 * i]), lo = hex_nibble(line[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            *err = errors_new(error_allocator(), BURROW_S("invalid hex"));
            return 0;
        }
        bytes_of(p)[i] = (uint8_t)(hi << 4 | lo);
    }
    return p.len;
}

static const IoReaderVT prime_vt = {&test_type, prime_read};

typedef struct PrimeBench {
    Int bits;
    const char *const *lines;
    Int n;
} PrimeBench;

static void bench_generate_key(void *env, TestingB *b) {
    const PrimeBench *pb = env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    burrow__crypto_rand_godebug_set("cryptocustomrand=1");
    while (testing_b_loop(b)) {
        PrimeReader pr = {pb->lines, pb->n, 0};
        IoReader r = {&prime_vt, &pr};
        Error err = BURROW_NO_ERROR;
        (void)rsa_generate_key(a, r, pb->bits, &err);
        if (BURROW_FAILED(err))
            testing_b_fatalf_v(b, "%s", error_text(err));
        arena_reset(&ar);
    }
    burrow__crypto_rand_godebug_set(NULL);
    arena_free(&ar);
}

static void BenchmarkGenerateKey(TestingB *b) {
    static const PrimeBench benches[] = {
        {2048, gen_keygen2048, sizeof gen_keygen2048 / sizeof gen_keygen2048[0]},
        {3072, gen_keygen3072, sizeof gen_keygen3072 / sizeof gen_keygen3072[0]},
        {4096, gen_keygen4096, sizeof gen_keygen4096 / sizeof gen_keygen4096[0]},
    };
    for (size_t i = 0; i < sizeof benches / sizeof benches[0]; i++) {
        char name[16];
        snprintf(name, sizeof name, "%d", (int)benches[i].bits);
        testing_b_run(b, str_from_cstr(name),
                      BURROW_FN(TestingBFunc, bench_generate_key,
                                (void *)(uintptr_t)&benches[i]));
    }
}

#define TESTS(X)                                                                       \
    X(TestKeyGeneration)                                                               \
    X(Test3PrimeKeyGeneration)                                                         \
    X(Test4PrimeKeyGeneration)                                                         \
    X(TestNPrimeKeyGeneration)                                                         \
    X(TestImpossibleKeyGeneration)                                                     \
    X(TestTinyKeyGeneration)                                                           \
    X(TestKeyGenerationVectors)                                                        \
    X(TestGnuTLSKey)                                                                   \
    X(TestEverything)                                                                  \
    X(TestKeyTooSmall)                                                                 \
    X(TestEncryptOAEP)                                                                 \
    X(TestDecryptOAEP)                                                                 \
    X(Test2DecryptOAEP)                                                                \
    X(TestEncryptDecryptOAEP)                                                          \
    X(TestPSmallerThanQ)                                                               \
    X(TestLargeSizeDifference)                                                         \
    X(TestNotPrecomputed)                                                              \
    X(TestModifiedPrivateKey)                                                          \
    X(TestDecryptPKCS1v15)                                                             \
    X(TestEncryptPKCS1v15)                                                             \
    X(TestEncryptPKCS1v15SessionKey)                                                   \
    X(TestEncryptPKCS1v15DecrypterSessionKey)                                          \
    X(TestNonZeroRandomBytes)                                                          \
    X(TestSignPKCS1v15)                                                                \
    X(TestVerifyPKCS1v15)                                                              \
    X(TestOverlongMessagePKCS1v15)                                                     \
    X(TestUnpaddedSignature)                                                           \
    X(TestShortSessionKey)                                                             \
    X(TestShortPKCS1v15Signature)                                                      \
    X(TestPSSGolden)                                                                   \
    X(TestPSSOpenSSL)                                                                  \
    X(TestPSSNilOpts)                                                                  \
    X(TestPSSSigning)                                                                  \
    X(TestPSS513)                                                                      \
    X(TestInvalidPSSSaltLength)                                                        \
    X(TestHashOverride)                                                                \
    X(TestEqual)                                                                       \
    X(TestRSAOAEPDecryptWycheproof)                                                    \
    X(TestRSAPKCS1DecryptWycheproof)                                                   \
    X(TestRSAPKCS1SignaturesWycheproof)                                                \
    X(TestRSAPSSSignaturesWycheproof)                                                  \
    X(TestMillerRabin)                                                                 \
    X(TestTotient)                                                                     \
    X(TestHashPrefixes)                                                                \
    X(TestEMSAPSS)                                                                     \
    X(BenchmarkDecryptPKCS1v15)                                                        \
    X(BenchmarkEncryptPKCS1v15)                                                        \
    X(BenchmarkDecryptOAEP)                                                            \
    X(BenchmarkEncryptOAEP)                                                            \
    X(BenchmarkSignPKCS1v15)                                                           \
    X(BenchmarkVerifyPKCS1v15)                                                         \
    X(BenchmarkSignPSS)                                                                \
    X(BenchmarkVerifyPSS)                                                              \
    X(BenchmarkParsePKCS8PrivateKey)                                                   \
    X(BenchmarkGenerateKey)

TESTING_MAIN(TESTS)
