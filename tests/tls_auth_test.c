/* Derived from Go's src/crypto/tls/auth_test.go.
 * Go source: go1.27.1.
 *
 * The cases that set GODEBUG run one after another rather than the others in
 * parallel. There is no FIPS 140-3 module here to be v1.0.0, so
 * MustMinimumFIPS140ModuleVersion never skips. The certificates come from
 * tls_test_gen.h, which is generated from Go's certificates_test.go.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/rsa.h"
#include "burrow/crypto/tls.h"
#include "burrow/strconv.h"
#include "tls_test_gen.h"

#include <stdint.h>

#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

/* The certificates the tests here use. */
enum {
    CERT_RSA2048,
    CERT_ECDSA_P256,
    CERT_ED25519,
    CERT_MLDSA44,
    CERT_MLDSA65,
    CERT_MLDSA87,
    CERT_RSA1024,
    CERT_PKCS1,  /* pkcs1Cert */
    CERT_BROKEN, /* brokenCert */
};

typedef struct AuthCerts {
    TlsCertificate c[9];
} AuthCerts;

static const TlsSignatureScheme pkcs1_cert_algs[] = {TLS_PKCS1_WITH_SHA1,
                                                     TLS_PKCS1_WITH_SHA256};
static const TlsSignatureScheme broken_cert_algs[] = {TLS_ED25519};

static Slice schemes(const TlsSignatureScheme *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_UINT16);
}

static void auth_certs(Alloc *a, AuthCerts *certs) {
    certs->c[CERT_RSA2048] =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    certs->c[CERT_ECDSA_P256] =
        parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem);
    certs->c[CERT_ED25519] =
        parse_test_cert(a, test_ed25519_cert_pem, test_ed25519_key_pem);
    certs->c[CERT_MLDSA44] =
        parse_test_cert(a, test_mldsa44_cert_pem, test_mldsa44_key_pem);
    certs->c[CERT_MLDSA65] =
        parse_test_cert(a, test_mldsa65_cert_pem, test_mldsa65_key_pem);
    certs->c[CERT_MLDSA87] =
        parse_test_cert(a, test_mldsa87_cert_pem, test_mldsa87_key_pem);
    certs->c[CERT_RSA1024] =
        parse_test_cert(a, test_rsa1024_cert_pem, test_rsa1024_key_pem);
    certs->c[CERT_PKCS1] = certs->c[CERT_RSA2048];
    certs->c[CERT_PKCS1].supported_signature_algorithms =
        schemes(pkcs1_cert_algs, NELEM(pkcs1_cert_algs));
    certs->c[CERT_BROKEN] = certs->c[CERT_RSA2048];
    certs->c[CERT_BROKEN].supported_signature_algorithms =
        schemes(broken_cert_algs, NELEM(broken_cert_algs));
}

/* peerSigAlgs, up to two of them, n being -1 for nil. */
typedef struct SelectionTest {
    const char *godebug;
    CryptoHash expected_hash;
    TlsSignatureScheme peer_sig_algs[2];
    uint16_t tls_version;
    TlsSignatureScheme expected_sig_alg;
    int n;
    uint8_t cert;
    uint8_t expected_sig_type;
} SelectionTest;

#define SHA1 CRYPTO_SHA1
#define SHA256 CRYPTO_SHA256
#define SHA384 CRYPTO_SHA384
#define SHA512 CRYPTO_SHA512
#define DIRECT TLS_DIRECT_SIGNING
#define TLS12 TLS_VERSION_TLS12
#define TLS13 TLS_VERSION_TLS13
#define PKCS1V15 TLS_SIGNATURE_PKCS1V15
#define RSAPSS TLS_SIGNATURE_RSAPSS
#define ECDSA TLS_SIGNATURE_ECDSA
#define ED25519 TLS_SIGNATURE_ED25519
#define MLDSA TLS_SIGNATURE_MLDSA

#define SEL(cert, a1, a2, n, vers, godebug, alg, type, hash)                           \
    {godebug, hash, {a1, a2}, vers, alg, n, cert, type}

static const SelectionTest selection_tests[] = {
    SEL(CERT_RSA2048, TLS_PKCS1_WITH_SHA1, TLS_PKCS1_WITH_SHA256, 2, TLS12, NULL,
        TLS_PKCS1_WITH_SHA256, PKCS1V15, SHA256),
    SEL(CERT_RSA2048, TLS_PKCS1_WITH_SHA1, TLS_PKCS1_WITH_SHA256, 2, TLS12, "tlssha1=1",
        TLS_PKCS1_WITH_SHA1, PKCS1V15, SHA1),
    SEL(CERT_RSA2048, TLS_PKCS1_WITH_SHA512, TLS_PKCS1_WITH_SHA1, 2, TLS12, NULL,
        TLS_PKCS1_WITH_SHA512, PKCS1V15, SHA512),
    SEL(CERT_RSA2048, TLS_PSS_WITH_SHA256, TLS_PKCS1_WITH_SHA256, 2, TLS12, NULL,
        TLS_PSS_WITH_SHA256, RSAPSS, SHA256),
    SEL(CERT_PKCS1, TLS_PSS_WITH_SHA256, TLS_PKCS1_WITH_SHA256, 2, TLS12, NULL,
        TLS_PKCS1_WITH_SHA256, PKCS1V15, SHA256),
    SEL(CERT_RSA2048, TLS_PSS_WITH_SHA384, TLS_PKCS1_WITH_SHA1, 2, TLS13, NULL,
        TLS_PSS_WITH_SHA384, RSAPSS, SHA384),
    SEL(CERT_RSA2048, TLS_PKCS1_WITH_SHA1, TLS_PSS_WITH_SHA384, 2, TLS13, NULL,
        TLS_PSS_WITH_SHA384, RSAPSS, SHA384),
    SEL(CERT_ECDSA_P256, TLS_ECDSA_WITH_SHA1, TLS_ECDSA_WITH_P256_AND_SHA256, 2, TLS12,
        NULL, TLS_ECDSA_WITH_P256_AND_SHA256, ECDSA, SHA256),
    SEL(CERT_ECDSA_P256, TLS_ECDSA_WITH_SHA1, 0, 1, TLS12, "tlssha1=1",
        TLS_ECDSA_WITH_SHA1, ECDSA, SHA1),
    SEL(CERT_ECDSA_P256, TLS_ECDSA_WITH_P256_AND_SHA256, 0, 1, TLS12, NULL,
        TLS_ECDSA_WITH_P256_AND_SHA256, ECDSA, SHA256),
    SEL(CERT_ECDSA_P256, TLS_ECDSA_WITH_P256_AND_SHA256, 0, 1, TLS13, NULL,
        TLS_ECDSA_WITH_P256_AND_SHA256, ECDSA, SHA256),
    SEL(CERT_ED25519, TLS_ED25519, 0, 1, TLS12, NULL, TLS_ED25519, ED25519, DIRECT),
    SEL(CERT_ED25519, TLS_ED25519, 0, 1, TLS13, NULL, TLS_ED25519, ED25519, DIRECT),
    SEL(CERT_MLDSA44, TLS_MLDSA44, 0, 1, TLS13, NULL, TLS_MLDSA44, MLDSA, DIRECT),
    SEL(CERT_MLDSA65, TLS_MLDSA65, 0, 1, TLS13, NULL, TLS_MLDSA65, MLDSA, DIRECT),
    SEL(CERT_MLDSA87, TLS_MLDSA87, 0, 1, TLS13, NULL, TLS_MLDSA87, MLDSA, DIRECT),

    /* TLS 1.2 without signature_algorithms extension */
    SEL(CERT_RSA2048, 0, 0, -1, TLS12, "tlssha1=1", TLS_PKCS1_WITH_SHA1, PKCS1V15,
        SHA1),
    SEL(CERT_ECDSA_P256, 0, 0, -1, TLS12, "tlssha1=1", TLS_ECDSA_WITH_SHA1, ECDSA,
        SHA1),

    /* TLS 1.2 does not restrict the ECDSA curve */
    SEL(CERT_ECDSA_P256, TLS_ECDSA_WITH_P384_AND_SHA384, 0, 1, TLS12, NULL,
        TLS_ECDSA_WITH_P384_AND_SHA384, ECDSA, SHA384),
};

typedef struct BadSelectionTest {
    TlsSignatureScheme peer_sig_algs[2];
    uint16_t tls_version;
    int8_t n;
    uint8_t cert;
} BadSelectionTest;

#define BAD(cert, a1, a2, n, vers) {{a1, a2}, vers, n, cert}

static const BadSelectionTest bad_selection_tests[] = {
    BAD(CERT_RSA2048, TLS_ECDSA_WITH_P256_AND_SHA256, TLS_ECDSA_WITH_SHA1, 2, TLS12),
    BAD(CERT_ECDSA_P256, TLS_PKCS1_WITH_SHA256, TLS_PKCS1_WITH_SHA1, 2, TLS12),
    BAD(CERT_RSA2048, 0, 0, 1, TLS12),
    BAD(CERT_ED25519, TLS_ECDSA_WITH_P256_AND_SHA256, TLS_ECDSA_WITH_SHA1, 2, TLS12),
    BAD(CERT_ECDSA_P256, TLS_ED25519, 0, 1, TLS12),
    BAD(CERT_BROKEN, TLS_ED25519, 0, 1, TLS12),
    BAD(CERT_BROKEN, TLS_PKCS1_WITH_SHA256, 0, 1, TLS12),
    /* RFC 5246, Section 7.4.1.4.1, says to only consider {sha1,ecdsa} as
     * default when the extension is missing, and RFC 8422 does not update it.
     * Anyway, if a stack supports Ed25519 it better support sigalgs. */
    BAD(CERT_ED25519, 0, 0, -1, TLS12),
    /* TLS 1.3 has no default signature_algorithms. */
    BAD(CERT_RSA2048, 0, 0, -1, TLS13),
    BAD(CERT_ECDSA_P256, 0, 0, -1, TLS13),
    BAD(CERT_ED25519, 0, 0, -1, TLS13),
    /* Wrong curve, which TLS 1.3 checks */
    BAD(CERT_ECDSA_P256, TLS_ECDSA_WITH_P384_AND_SHA384, 0, 1, TLS13),
    /* TLS 1.3 does not support PKCS1v1.5 or SHA-1. */
    BAD(CERT_RSA2048, TLS_PKCS1_WITH_SHA256, 0, 1, TLS13),
    BAD(CERT_PKCS1, TLS_PSS_WITH_SHA256, TLS_PKCS1_WITH_SHA256, 2, TLS13),
    BAD(CERT_ECDSA_P256, TLS_ECDSA_WITH_SHA1, 0, 1, TLS13),
    /* The key can be too small for the hash. */
    BAD(CERT_RSA1024, TLS_PSS_WITH_SHA512, 0, 1, TLS12),
    /* SHA-1 requires tlssha1=1 */
    BAD(CERT_RSA2048, TLS_PKCS1_WITH_SHA1, 0, 1, TLS12),
    BAD(CERT_ECDSA_P256, TLS_ECDSA_WITH_SHA1, 0, 1, TLS12),
    BAD(CERT_RSA2048, 0, 0, -1, TLS12),
    BAD(CERT_ECDSA_P256, 0, 0, -1, TLS12),
    /* ML-DSA requires TLS 1.3. */
    BAD(CERT_MLDSA44, TLS_MLDSA44, 0, 1, TLS12),
    BAD(CERT_MLDSA65, TLS_MLDSA65, 0, 1, TLS12),
    BAD(CERT_MLDSA87, TLS_MLDSA87, 0, 1, TLS12),
    /* ML-DSA parameter sets don't cross-match. */
    BAD(CERT_MLDSA44, TLS_MLDSA65, 0, 1, TLS13),
    BAD(CERT_MLDSA65, TLS_MLDSA87, 0, 1, TLS13),
    BAD(CERT_MLDSA87, TLS_MLDSA44, 0, 1, TLS13),
    /* ML-DSA cert with non-ML-DSA peer sig algs and vice versa. */
    BAD(CERT_MLDSA44, TLS_ED25519, 0, 1, TLS13),
    BAD(CERT_RSA2048, TLS_MLDSA44, 0, 1, TLS13),
    BAD(CERT_ECDSA_P256, TLS_MLDSA44, 0, 1, TLS13),
};

/* The peer's list of a test, nil when n is -1. */
static Slice peer_algs(const TlsSignatureScheme *algs, int n) {
    if (n < 0)
        return slice_nil(TYPE_UINT16);
    return schemes(algs, n);
}

typedef struct SelectionEnv {
    const SelectionTest *test;
    const AuthCerts *certs;
} SelectionEnv;

static void signature_selection(void *envp, TestingT *t) {
    const SelectionEnv *env = (const SelectionEnv *)envp;
    const SelectionTest *test = env->test;
    if (burrow__tls_fips140_required() && test->expected_hash == CRYPTO_SHA1)
        testing_t_skip_v(t, "skipping test not compatible with TLS FIPS mode");
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    if (test->godebug != NULL)
        burrow__tls_godebug_set(test->godebug);

    Error err = BURROW_NO_ERROR;
    TlsSignatureScheme sig_alg = burrow__tls_select_signature_scheme(
        test->tls_version, &env->certs->c[test->cert],
        peer_algs(test->peer_sig_algs, test->n), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected selectSignatureScheme error: %v", err);
    if (test->expected_sig_alg != sig_alg)
        testing_t_errorf_v(t, "expected signature scheme %s, got %s",
                           tls_signature_scheme_string(test->expected_sig_alg, a),
                           tls_signature_scheme_string(sig_alg, a));
    uint8_t sig_type;
    CryptoHash hash_func;
    err = BURROW_NO_ERROR;
    burrow__tls_type_and_hash_from_signature_scheme(sig_alg, &sig_type, &hash_func,
                                                    &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "unexpected typeAndHashFromSignatureScheme error: %v",
                           err);
    if (test->expected_sig_type != sig_type)
        testing_t_errorf_v(t, "expected signature algorithm %#x, got %#x",
                           (Int)test->expected_sig_type, (Int)sig_type);
    if (test->expected_hash != hash_func)
        testing_t_errorf_v(t, "expected hash function %#x, got %#x",
                           (Int)test->expected_hash, (Int)hash_func);

    if (test->godebug != NULL)
        burrow__tls_godebug_set(NULL);
    arena_free(&ar);
}

static void TestSignatureSelection(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    AuthCerts certs;
    auth_certs(a, &certs);

    for (size_t test_no = 0; test_no < NELEM(selection_tests); test_no++) {
        SelectionEnv env = {&selection_tests[test_no], &certs};
        testing_t_run(t, strconv_itoa(a, (Int)test_no),
                      BURROW_FN(TestingTFunc, signature_selection, &env));
    }

    for (size_t test_no = 0; test_no < NELEM(bad_selection_tests); test_no++) {
        const BadSelectionTest *test = &bad_selection_tests[test_no];
        Error err = BURROW_NO_ERROR;
        TlsSignatureScheme sig_alg = burrow__tls_select_signature_scheme(
            test->tls_version, &certs.c[test->cert],
            peer_algs(test->peer_sig_algs, test->n), &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "test[%d]: unexpected success, got %s", (Int)test_no,
                               tls_signature_scheme_string(sig_alg, a));
    }
    arena_free(&ar);
}

static void TestLegacyTypeAndHash(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsCertificate rsa_cert =
        parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    TlsCertificate ecdsa_cert =
        parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem);
    TlsCertificate ed25519_cert =
        parse_test_cert(a, test_ed25519_cert_pem, test_ed25519_key_pem);

    uint8_t sig_type;
    CryptoHash hash_func;
    Error err = BURROW_NO_ERROR;
    burrow__tls_legacy_type_and_hash_from_public_key(
        rsa_private_key_public(any_assert(rsa_cert.private_key, TYPE_RSA_PRIVATE_KEY)),
        &sig_type, &hash_func, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "RSA: unexpected error: %v", err);
    if (TLS_SIGNATURE_PKCS1V15 != sig_type)
        testing_t_errorf_v(t, "RSA: expected signature type %#x, got %#x",
                           (Int)TLS_SIGNATURE_PKCS1V15, (Int)sig_type);
    if (CRYPTO_MD5SHA1 != hash_func)
        testing_t_errorf_v(t, "RSA: expected hash %#x, got %#x", (Int)CRYPTO_MD5SHA1,
                           (Int)hash_func);

    err = BURROW_NO_ERROR;
    burrow__tls_legacy_type_and_hash_from_public_key(
        ecdsa_private_key_public(
            any_assert(ecdsa_cert.private_key, TYPE_ECDSA_PRIVATE_KEY)),
        &sig_type, &hash_func, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "ECDSA: unexpected error: %v", err);
    if (TLS_SIGNATURE_ECDSA != sig_type)
        testing_t_errorf_v(t, "ECDSA: expected signature type %#x, got %#x",
                           (Int)TLS_SIGNATURE_ECDSA, (Int)sig_type);
    if (CRYPTO_SHA1 != hash_func)
        testing_t_errorf_v(t, "ECDSA: expected hash %#x, got %#x", (Int)CRYPTO_SHA1,
                           (Int)hash_func);

    /* Ed25519 is not supported by TLS 1.0 and 1.1. */
    Ed25519PublicKey ed25519_pub = ed25519_private_key_public(
        *(const Ed25519PrivateKey *)any_assert(ed25519_cert.private_key,
                                               TYPE_ED25519_PRIVATE_KEY),
        a);
    err = BURROW_NO_ERROR;
    burrow__tls_legacy_type_and_hash_from_public_key(
        BURROW_ANY(TYPE_ED25519_PUBLIC_KEY, &ed25519_pub), NULL, NULL, &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "Ed25519: unexpected success");

    /* ML-DSA is not supported by TLS 1.0 and 1.1. */
    const char *const *mldsa_pems[3][2] = {
        {test_mldsa44_cert_pem, test_mldsa44_key_pem},
        {test_mldsa65_cert_pem, test_mldsa65_key_pem},
        {test_mldsa87_cert_pem, test_mldsa87_key_pem},
    };
    for (size_t i = 0; i < NELEM(mldsa_pems); i++) {
        TlsCertificate c = parse_test_cert(a, mldsa_pems[i][0], mldsa_pems[i][1]);
        const MldsaPrivateKey *key = any_assert(c.private_key, TYPE_MLDSA_PRIVATE_KEY);
        err = BURROW_NO_ERROR;
        if (burrow__tls_legacy_type_and_hash_from_public_key(
                mldsa_private_key_public(key), NULL, NULL, &err))
            testing_t_errorf_v(t, "%s: unexpected success",
                               mldsa_parameters_string(mldsa_public_key_parameters(
                                   mldsa_private_key_public_key(key))));
    }
    arena_free(&ar);
}

/* TestSupportedSignatureAlgorithms checks that all supportedSignatureAlgorithms
 * have valid type and hash information. */
static void TestSupportedSignatureAlgorithms(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice algs = burrow__tls_supported_signature_algorithms(a, TLS_VERSION_TLS12,
                                                            TLS_VERSION_TLS13);
    for (Int i = 0; i < algs.len; i++) {
        TlsSignatureScheme sig_alg = BURROW_AT(TlsSignatureScheme, algs, i);
        Str name = tls_signature_scheme_string(sig_alg, a);
        uint8_t sig_type;
        CryptoHash hash;
        Error err = BURROW_NO_ERROR;
        burrow__tls_type_and_hash_from_signature_scheme(sig_alg, &sig_type, &hash,
                                                        &err);
        if (BURROW_FAILED(err))
            testing_t_errorf_v(t, "%s: unexpected error: %v", name, err);
        if (sig_type == 0)
            testing_t_errorf_v(t, "%s: missing signature type", name);
        if (hash == 0 && sig_alg != TLS_ED25519 && sig_alg != TLS_MLDSA44 &&
            sig_alg != TLS_MLDSA65 && sig_alg != TLS_MLDSA87)
            testing_t_errorf_v(t, "%s: missing hash", name);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestSignatureSelection)                                                          \
    X(TestLegacyTypeAndHash)                                                           \
    X(TestSupportedSignatureAlgorithms)

TESTING_MAIN(TESTS)
