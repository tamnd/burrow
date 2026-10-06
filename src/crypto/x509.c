/* crypto/x509: OIDs, the key formats and encrypted PEM blocks, from Go's
 * oid.go, pkcs1.go, sec1.go, pkcs8.go, pem_decrypt.go, x509_string.go and the
 * parts of x509.go and parser.go that read and write public keys.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/x509.h"

#include "burrow/atomic.h"
#include "burrow/bytes.h"
#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/aes.h"
#include "burrow/crypto/cipher.h"
#include "burrow/crypto/des.h"
#include "burrow/crypto/dsa.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/md5.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/rsa.h"
#include "burrow/crypto/x509/pkix.h"
#include "burrow/declare.h"
#include "burrow/encoding/asn1.h"
#include "burrow/encoding/hex.h"
#include "burrow/encoding/pem.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include "cryptobyte.h"
#include "x509_internal.h"

#include <stdint.h>
#include <string.h>

/* ---------------------------------------------------------------- helpers */

static void x509_fail(Error *err, const char *msg) {
    BURROW_OUT(err, errors_new(error_allocator(), str_from_cstr(msg)));
}

static Slice x509_bytes(const void *p, Int n) {
    return slice_from((void *)(uintptr_t)p, n, n, TYPE_BYTE);
}

/* A copy of s in a, the nil slice for an empty one. */
static Slice x509_clone(Alloc *a, Slice s) {
    if (s.len == 0)
        return slice_nil(TYPE_BYTE);
    Slice c = slice_make(a, TYPE_BYTE, s.len, s.len);
    if (c.p != NULL)
        memcpy(c.p, s.p, (size_t)s.len);
    return c;
}

static Int x509_bit_len64(uint64_t n) {
    Int l = 0;
    while (n != 0) {
        l++;
        n >>= 1;
    }
    return l;
}

/* An OID kept as a static array of arcs. */
#define X509_OID_ARCS(name, ...) static const Int name[] = {__VA_ARGS__}
#define X509_OID(arcs)                                                                 \
    ((Asn1ObjectIdentifier){(void *)(uintptr_t)(arcs),                                 \
                            (Int)(sizeof(arcs) / sizeof((arcs)[0])),                   \
                            (Int)(sizeof(arcs) / sizeof((arcs)[0])), TYPE_INT})

/* ------------------------------------------------------------------ GODEBUG */

enum {
    X509_DEBUG_KNOWN = 1 << 0,
    X509_DEBUG_RSACRT_OFF = 1 << 1, /* x509rsacrt=0 */
};

static uint32_t x509_debug_flags;

/* Whether GODEBUG in env has key=value, the last one counting when key is
 * there twice. */
static bool x509_godebug_is(const char *env, const char *key, const char *value) {
    size_t kl = strlen(key), vl = strlen(value);
    bool is = false;
    const char *p = env;
    while (*p != '\0') {
        const char *end = strchr(p, ',');
        if (end == NULL)
            end = p + strlen(p);
        if ((size_t)(end - p) > kl && memcmp(p, key, kl) == 0 && p[kl] == '=')
            is = (size_t)(end - p) == kl + 1 + vl && memcmp(p + kl + 1, value, vl) == 0;
        p = *end == ',' ? end + 1 : end;
    }
    return is;
}

static uint32_t x509_debug_load(void) {
    uint32_t f = burrow__atomic_load_relaxed_u32(&x509_debug_flags);
    if ((f & X509_DEBUG_KNOWN) != 0)
        return f;
    f = X509_DEBUG_KNOWN;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            if (x509_godebug_is(*env + 8, "x509rsacrt", "0"))
                f |= X509_DEBUG_RSACRT_OFF;
            break;
        }
    }
    burrow__atomic_store_relaxed_u32(&x509_debug_flags, f);
    return f;
}

void burrow__x509_godebug_set(const char *value) {
    uint32_t f = 0;
    if (value != NULL) {
        f = X509_DEBUG_KNOWN;
        if (x509_godebug_is(value, "x509rsacrt", "0"))
            f |= X509_DEBUG_RSACRT_OFF;
    }
    burrow__atomic_store_relaxed_u32(&x509_debug_flags, f);
}

/* ------------------------------------------------------------- descriptors */

BURROW_PTR_TYPE(X509BigIntPtr, BigInt);

#define X509_OID_FIELDS(F, T) F(T, Bytes, der, "")
BURROW_STRUCT_DEFINE(X509OID, X509_OID_FIELDS);

/* pkcs1AdditionalRSAPrime */
#define X509_PKCS1_ADDITIONAL_PRIME_FIELDS(F, T)                                       \
    F(T, X509BigIntPtr, prime, Prime, "")                                              \
    F(T, X509BigIntPtr, exp, Exp, "")                                                  \
    F(T, X509BigIntPtr, coeff, Coeff, "")
BURROW_STRUCT_AS(X509Pkcs1AdditionalPrime, X509_PKCS1_ADDITIONAL_PRIME_FIELDS);
BURROW_SLICE_TYPE(X509Pkcs1AdditionalPrimes, X509Pkcs1AdditionalPrime);

/* pkcs1PrivateKey */
#define X509_PKCS1_PRIVATE_KEY_FIELDS(F, T)                                            \
    F(T, Int, version, Version, "")                                                    \
    F(T, X509BigIntPtr, n, N, "")                                                      \
    F(T, Int, e, E, "")                                                                \
    F(T, X509BigIntPtr, d, D, "")                                                      \
    F(T, X509BigIntPtr, p, P, "")                                                      \
    F(T, X509BigIntPtr, q, Q, "")                                                      \
    F(T, X509BigIntPtr, dp, Dp, "asn1:\"optional\"")                                   \
    F(T, X509BigIntPtr, dq, Dq, "asn1:\"optional\"")                                   \
    F(T, X509BigIntPtr, qinv, Qinv, "asn1:\"optional\"")                               \
    F(T, X509Pkcs1AdditionalPrimes, additional_primes, AdditionalPrimes,               \
      "asn1:\"optional,omitempty\"")
BURROW_STRUCT_AS(X509Pkcs1PrivateKey, X509_PKCS1_PRIVATE_KEY_FIELDS);

/* pkcs1PublicKey */
#define X509_PKCS1_PUBLIC_KEY_FIELDS(F, T)                                             \
    F(T, X509BigIntPtr, n, N, "")                                                      \
    F(T, Int, e, E, "")
BURROW_STRUCT_AS(X509Pkcs1PublicKey, X509_PKCS1_PUBLIC_KEY_FIELDS);

/* ecPrivateKey */
#define X509_EC_PRIVATE_KEY_FIELDS(F, T)                                               \
    F(T, Int, version, Version, "")                                                    \
    F(T, Bytes, private_key, PrivateKey, "")                                           \
    F(T, Asn1ObjectIdentifier, named_curve_oid, NamedCurveOID,                         \
      "asn1:\"optional,explicit,tag:0\"")                                              \
    F(T, Asn1BitString, public_key, PublicKey, "asn1:\"optional,explicit,tag:1\"")
BURROW_STRUCT_AS(X509EcPrivateKey, X509_EC_PRIVATE_KEY_FIELDS);

/* pkcs8 */
#define X509_PKCS8_FIELDS(F, T)                                                        \
    F(T, Int, version, Version, "")                                                    \
    F(T, PkixAlgorithmIdentifier, algo, Algo, "")                                      \
    F(T, Bytes, private_key, PrivateKey, "")
BURROW_STRUCT_AS(X509Pkcs8, X509_PKCS8_FIELDS);

/* publicKeyInfo */
#define X509_PUBLIC_KEY_INFO_FIELDS(F, T)                                              \
    F(T, Asn1RawContent, raw, Raw, "")                                                 \
    F(T, PkixAlgorithmIdentifier, algorithm, Algorithm, "")                            \
    F(T, Asn1BitString, public_key, PublicKey, "")
BURROW_STRUCT_AS(X509PublicKeyInfo, X509_PUBLIC_KEY_INFO_FIELDS);

/* pkixPublicKey */
#define X509_PKIX_PUBLIC_KEY_FIELDS(F, T)                                              \
    F(T, PkixAlgorithmIdentifier, algo, Algo, "")                                      \
    F(T, Asn1BitString, bit_string, BitString, "")
BURROW_STRUCT_AS(X509PkixPublicKey, X509_PKIX_PUBLIC_KEY_FIELDS);

/* Whether der holds one of t, the asn1.Unmarshal(der, &T{}) the parse
 * functions use to tell a key in the wrong format from a broken one. */
static bool x509_der_is(Slice der, const Type *t, size_t size) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *ta = arena_allocator(&ar);
    bool ok = false;
    void *v = mem_alloc(ta, size, 16);
    if (v != NULL) {
        Error e = BURROW_NO_ERROR;
        (void)asn1_unmarshal(ta, der, BURROW_ANY(t, v), &e);
        ok = !BURROW_FAILED(e);
    }
    arena_free(&ar);
    return ok;
}

#define X509_DER_IS(der, T) x509_der_is((der), TYPE_OF(T), sizeof(T))

static Error x509_trailing_data(void) {
    return asn1_syntax_error_as_error((Asn1SyntaxError){BURROW_S("trailing data")},
                                      error_allocator());
}

/* -------------------------------------------------------------------- OIDs */

X509_OID_ARCS(x509_oid_public_key_rsa, 1, 2, 840, 113549, 1, 1, 1);
X509_OID_ARCS(x509_oid_public_key_dsa, 1, 2, 840, 10040, 4, 1);
X509_OID_ARCS(x509_oid_public_key_ecdsa, 1, 2, 840, 10045, 2, 1);
X509_OID_ARCS(x509_oid_public_key_x25519, 1, 3, 101, 110);
X509_OID_ARCS(x509_oid_public_key_ed25519, 1, 3, 101, 112);
X509_OID_ARCS(x509_oid_public_key_mldsa44, 2, 16, 840, 1, 101, 3, 4, 3, 17);
X509_OID_ARCS(x509_oid_public_key_mldsa65, 2, 16, 840, 1, 101, 3, 4, 3, 18);
X509_OID_ARCS(x509_oid_public_key_mldsa87, 2, 16, 840, 1, 101, 3, 4, 3, 19);

X509_OID_ARCS(x509_oid_named_curve_p224, 1, 3, 132, 0, 33);
X509_OID_ARCS(x509_oid_named_curve_p256, 1, 2, 840, 10045, 3, 1, 7);
X509_OID_ARCS(x509_oid_named_curve_p384, 1, 3, 132, 0, 34);
X509_OID_ARCS(x509_oid_named_curve_p521, 1, 3, 132, 0, 35);

X509_OID_ARCS(x509_oid_signature_md5_with_rsa, 1, 2, 840, 113549, 1, 1, 4);
X509_OID_ARCS(x509_oid_signature_sha1_with_rsa, 1, 2, 840, 113549, 1, 1, 5);
X509_OID_ARCS(x509_oid_signature_sha256_with_rsa, 1, 2, 840, 113549, 1, 1, 11);
X509_OID_ARCS(x509_oid_signature_sha384_with_rsa, 1, 2, 840, 113549, 1, 1, 12);
X509_OID_ARCS(x509_oid_signature_sha512_with_rsa, 1, 2, 840, 113549, 1, 1, 13);
X509_OID_ARCS(x509_oid_signature_rsapss, 1, 2, 840, 113549, 1, 1, 10);
X509_OID_ARCS(x509_oid_signature_dsa_with_sha1, 1, 2, 840, 10040, 4, 3);
X509_OID_ARCS(x509_oid_signature_dsa_with_sha256, 2, 16, 840, 1, 101, 3, 4, 3, 2);
X509_OID_ARCS(x509_oid_signature_ecdsa_with_sha1, 1, 2, 840, 10045, 4, 1);
X509_OID_ARCS(x509_oid_signature_ecdsa_with_sha256, 1, 2, 840, 10045, 4, 3, 2);
X509_OID_ARCS(x509_oid_signature_ecdsa_with_sha384, 1, 2, 840, 10045, 4, 3, 3);
X509_OID_ARCS(x509_oid_signature_ecdsa_with_sha512, 1, 2, 840, 10045, 4, 3, 4);
X509_OID_ARCS(x509_oid_iso_signature_sha1_with_rsa, 1, 3, 14, 3, 2, 29);

/* The parameters of the three PSS algorithms Go writes, with the MGF1 hash
 * the same as the message hash and the salt as long as the hash. */
static const Byte x509_pss_params_sha256[] = {
    48, 52,  160, 15,  48, 13, 6,   9,  96,  134, 72,  1,   101, 3,   4, 2, 1,  5,
    0,  161, 28,  48,  26, 6,  9,   42, 134, 72,  134, 247, 13,  1,   1, 8, 48, 13,
    6,  9,   96,  134, 72, 1,  101, 3,  4,   2,   1,   5,   0,   162, 3, 2, 1,  32};
static const Byte x509_pss_params_sha384[] = {
    48, 52,  160, 15,  48, 13, 6,   9,  96,  134, 72,  1,   101, 3,   4, 2, 2,  5,
    0,  161, 28,  48,  26, 6,  9,   42, 134, 72,  134, 247, 13,  1,   1, 8, 48, 13,
    6,  9,   96,  134, 72, 1,  101, 3,  4,   2,   2,   5,   0,   162, 3, 2, 1,  48};
static const Byte x509_pss_params_sha512[] = {
    48, 52,  160, 15,  48, 13, 6,   9,  96,  134, 72,  1,   101, 3,   4, 2, 3,  5,
    0,  161, 28,  48,  26, 6,  9,   42, 134, 72,  134, 247, 13,  1,   1, 8, 48, 13,
    6,  9,   96,  134, 72, 1,  101, 3,  4,   2,   3,   5,   0,   162, 3, 2, 1,  64};

/* What the parameters of a signature algorithm are when it is written. */
enum {
    X509_PARAMS_EMPTY, /* left out */
    X509_PARAMS_NULL,  /* asn1.NullRawValue */
    X509_PARAMS_PSS,   /* pss, below */
};

/* signatureAlgorithmDetails */
typedef struct X509SignatureAlgorithmDetails {
    X509SignatureAlgorithm algo;
    const char *name;
    const Int *oid;
    Int oid_len;
    int params;
    const Byte *pss;
    X509PublicKeyAlgorithm pub_key_algo;
    CryptoHash hash;
    bool is_rsa_pss;
} X509SignatureAlgorithmDetails;

#define X509_SIG(algo, name, oid, params, pss, pub, hash, is_pss)                      \
    {algo, name, oid,   (Int)(sizeof(oid) / sizeof((oid)[0])), params, pss,            \
     pub,  hash, is_pss}

static const X509SignatureAlgorithmDetails x509_signature_algorithm_details[] = {
    X509_SIG(X509_MD5_WITH_RSA, "MD5-RSA", x509_oid_signature_md5_with_rsa,
             X509_PARAMS_NULL, NULL, X509_RSA, CRYPTO_MD5, false),
    X509_SIG(X509_SHA1_WITH_RSA, "SHA1-RSA", x509_oid_signature_sha1_with_rsa,
             X509_PARAMS_NULL, NULL, X509_RSA, CRYPTO_SHA1, false),
    X509_SIG(X509_SHA1_WITH_RSA, "SHA1-RSA", x509_oid_iso_signature_sha1_with_rsa,
             X509_PARAMS_NULL, NULL, X509_RSA, CRYPTO_SHA1, false),
    X509_SIG(X509_SHA256_WITH_RSA, "SHA256-RSA", x509_oid_signature_sha256_with_rsa,
             X509_PARAMS_NULL, NULL, X509_RSA, CRYPTO_SHA256, false),
    X509_SIG(X509_SHA384_WITH_RSA, "SHA384-RSA", x509_oid_signature_sha384_with_rsa,
             X509_PARAMS_NULL, NULL, X509_RSA, CRYPTO_SHA384, false),
    X509_SIG(X509_SHA512_WITH_RSA, "SHA512-RSA", x509_oid_signature_sha512_with_rsa,
             X509_PARAMS_NULL, NULL, X509_RSA, CRYPTO_SHA512, false),
    X509_SIG(X509_SHA256_WITH_RSAPSS, "SHA256-RSAPSS", x509_oid_signature_rsapss,
             X509_PARAMS_PSS, x509_pss_params_sha256, X509_RSA, CRYPTO_SHA256, true),
    X509_SIG(X509_SHA384_WITH_RSAPSS, "SHA384-RSAPSS", x509_oid_signature_rsapss,
             X509_PARAMS_PSS, x509_pss_params_sha384, X509_RSA, CRYPTO_SHA384, true),
    X509_SIG(X509_SHA512_WITH_RSAPSS, "SHA512-RSAPSS", x509_oid_signature_rsapss,
             X509_PARAMS_PSS, x509_pss_params_sha512, X509_RSA, CRYPTO_SHA512, true),
    X509_SIG(X509_DSA_WITH_SHA1, "DSA-SHA1", x509_oid_signature_dsa_with_sha1,
             X509_PARAMS_EMPTY, NULL, X509_DSA, CRYPTO_SHA1, false),
    X509_SIG(X509_DSA_WITH_SHA256, "DSA-SHA256", x509_oid_signature_dsa_with_sha256,
             X509_PARAMS_EMPTY, NULL, X509_DSA, CRYPTO_SHA256, false),
    X509_SIG(X509_ECDSA_WITH_SHA1, "ECDSA-SHA1", x509_oid_signature_ecdsa_with_sha1,
             X509_PARAMS_EMPTY, NULL, X509_ECDSA, CRYPTO_SHA1, false),
    X509_SIG(X509_ECDSA_WITH_SHA256, "ECDSA-SHA256",
             x509_oid_signature_ecdsa_with_sha256, X509_PARAMS_EMPTY, NULL, X509_ECDSA,
             CRYPTO_SHA256, false),
    X509_SIG(X509_ECDSA_WITH_SHA384, "ECDSA-SHA384",
             x509_oid_signature_ecdsa_with_sha384, X509_PARAMS_EMPTY, NULL, X509_ECDSA,
             CRYPTO_SHA384, false),
    X509_SIG(X509_ECDSA_WITH_SHA512, "ECDSA-SHA512",
             x509_oid_signature_ecdsa_with_sha512, X509_PARAMS_EMPTY, NULL, X509_ECDSA,
             CRYPTO_SHA512, false),
    X509_SIG(X509_PURE_ED25519, "Ed25519", x509_oid_public_key_ed25519,
             X509_PARAMS_EMPTY, NULL, X509_ED25519, 0, false),
    X509_SIG(X509_MLDSA44, "ML-DSA-44", x509_oid_public_key_mldsa44, X509_PARAMS_EMPTY,
             NULL, X509_MLDSA, 0, false),
    X509_SIG(X509_MLDSA65, "ML-DSA-65", x509_oid_public_key_mldsa65, X509_PARAMS_EMPTY,
             NULL, X509_MLDSA, 0, false),
    X509_SIG(X509_MLDSA87, "ML-DSA-87", x509_oid_public_key_mldsa87, X509_PARAMS_EMPTY,
             NULL, X509_MLDSA, 0, false),
};

#define X509_NSIG                                                                      \
    ((Int)(sizeof x509_signature_algorithm_details /                                   \
           sizeof x509_signature_algorithm_details[0]))

/* oidExtKeyUsage*, in ExtKeyUsage order. */
X509_OID_ARCS(x509_oid_eku_any, 2, 5, 29, 37, 0);
X509_OID_ARCS(x509_oid_eku_server_auth, 1, 3, 6, 1, 5, 5, 7, 3, 1);
X509_OID_ARCS(x509_oid_eku_client_auth, 1, 3, 6, 1, 5, 5, 7, 3, 2);
X509_OID_ARCS(x509_oid_eku_code_signing, 1, 3, 6, 1, 5, 5, 7, 3, 3);
X509_OID_ARCS(x509_oid_eku_email_protection, 1, 3, 6, 1, 5, 5, 7, 3, 4);
X509_OID_ARCS(x509_oid_eku_ipsec_end_system, 1, 3, 6, 1, 5, 5, 7, 3, 5);
X509_OID_ARCS(x509_oid_eku_ipsec_tunnel, 1, 3, 6, 1, 5, 5, 7, 3, 6);
X509_OID_ARCS(x509_oid_eku_ipsec_user, 1, 3, 6, 1, 5, 5, 7, 3, 7);
X509_OID_ARCS(x509_oid_eku_time_stamping, 1, 3, 6, 1, 5, 5, 7, 3, 8);
X509_OID_ARCS(x509_oid_eku_ocsp_signing, 1, 3, 6, 1, 5, 5, 7, 3, 9);
X509_OID_ARCS(x509_oid_eku_ms_server_gated_crypto, 1, 3, 6, 1, 4, 1, 311, 10, 3, 3);
X509_OID_ARCS(x509_oid_eku_ns_server_gated_crypto, 2, 16, 840, 1, 113730, 4, 1);
X509_OID_ARCS(x509_oid_eku_ms_commercial_code_signing, 1, 3, 6, 1, 4, 1, 311, 2, 1, 22);
X509_OID_ARCS(x509_oid_eku_ms_kernel_code_signing, 1, 3, 6, 1, 4, 1, 311, 61, 1, 1);

typedef struct X509ExtKeyUsageOID {
    const Int *oid;
    Int len;
} X509ExtKeyUsageOID;

#define X509_EKU(oid) {oid, (Int)(sizeof(oid) / sizeof((oid)[0]))}

static const X509ExtKeyUsageOID x509_ext_key_usage_oids[] = {
    X509_EKU(x509_oid_eku_any),
    X509_EKU(x509_oid_eku_server_auth),
    X509_EKU(x509_oid_eku_client_auth),
    X509_EKU(x509_oid_eku_code_signing),
    X509_EKU(x509_oid_eku_email_protection),
    X509_EKU(x509_oid_eku_ipsec_end_system),
    X509_EKU(x509_oid_eku_ipsec_tunnel),
    X509_EKU(x509_oid_eku_ipsec_user),
    X509_EKU(x509_oid_eku_time_stamping),
    X509_EKU(x509_oid_eku_ocsp_signing),
    X509_EKU(x509_oid_eku_ms_server_gated_crypto),
    X509_EKU(x509_oid_eku_ns_server_gated_crypto),
    X509_EKU(x509_oid_eku_ms_commercial_code_signing),
    X509_EKU(x509_oid_eku_ms_kernel_code_signing),
};

#define X509_NEKU                                                                      \
    ((Int)(sizeof x509_ext_key_usage_oids / sizeof x509_ext_key_usage_oids[0]))

static bool x509_oid_is(Asn1ObjectIdentifier oid, Asn1ObjectIdentifier want) {
    return asn1_object_identifier_equal(oid, want);
}

static bool x509_is_mldsa_oid(Asn1ObjectIdentifier oid) {
    return x509_oid_is(oid, X509_OID(x509_oid_public_key_mldsa44)) ||
           x509_oid_is(oid, X509_OID(x509_oid_public_key_mldsa65)) ||
           x509_oid_is(oid, X509_OID(x509_oid_public_key_mldsa87));
}

static bool x509_curve_is(EllipticCurve c, EllipticCurve want) {
    return c.vt == want.vt && c.data == want.data;
}

/* namedCurveFromOID, false for none. */
static bool x509_named_curve_from_oid(Asn1ObjectIdentifier oid, EllipticCurve *c) {
    if (x509_oid_is(oid, X509_OID(x509_oid_named_curve_p224)))
        *c = elliptic_p224();
    else if (x509_oid_is(oid, X509_OID(x509_oid_named_curve_p256)))
        *c = elliptic_p256();
    else if (x509_oid_is(oid, X509_OID(x509_oid_named_curve_p384)))
        *c = elliptic_p384();
    else if (x509_oid_is(oid, X509_OID(x509_oid_named_curve_p521)))
        *c = elliptic_p521();
    else
        return false;
    return true;
}

/* oidFromNamedCurve */
static bool x509_oid_from_named_curve(EllipticCurve c, Asn1ObjectIdentifier *oid) {
    if (c.vt == NULL)
        return false;
    if (x509_curve_is(c, elliptic_p224()))
        *oid = X509_OID(x509_oid_named_curve_p224);
    else if (x509_curve_is(c, elliptic_p256()))
        *oid = X509_OID(x509_oid_named_curve_p256);
    else if (x509_curve_is(c, elliptic_p384()))
        *oid = X509_OID(x509_oid_named_curve_p384);
    else if (x509_curve_is(c, elliptic_p521()))
        *oid = X509_OID(x509_oid_named_curve_p521);
    else
        return false;
    return true;
}

/* oidFromECDHCurve */
static bool x509_oid_from_ecdh_curve(const EcdhCurve *c, Asn1ObjectIdentifier *oid) {
    if (c == ecdh_x25519())
        *oid = X509_OID(x509_oid_public_key_x25519);
    else if (c == ecdh_p256())
        *oid = X509_OID(x509_oid_named_curve_p256);
    else if (c == ecdh_p384())
        *oid = X509_OID(x509_oid_named_curve_p384);
    else if (c == ecdh_p521())
        *oid = X509_OID(x509_oid_named_curve_p521);
    else
        return false;
    return true;
}

/* mldsaParametersFromOID, NULL for none. */
static const MldsaParameters *x509_mldsa_parameters_from_oid(Asn1ObjectIdentifier oid) {
    if (x509_oid_is(oid, X509_OID(x509_oid_public_key_mldsa44)))
        return mldsa_mldsa44();
    if (x509_oid_is(oid, X509_OID(x509_oid_public_key_mldsa65)))
        return mldsa_mldsa65();
    if (x509_oid_is(oid, X509_OID(x509_oid_public_key_mldsa87)))
        return mldsa_mldsa87();
    return NULL;
}

/* oidFromMLDSAParameters */
static bool x509_oid_from_mldsa_parameters(const MldsaParameters *params,
                                           Asn1ObjectIdentifier *oid) {
    if (params == mldsa_mldsa44())
        *oid = X509_OID(x509_oid_public_key_mldsa44);
    else if (params == mldsa_mldsa65())
        *oid = X509_OID(x509_oid_public_key_mldsa65);
    else if (params == mldsa_mldsa87())
        *oid = X509_OID(x509_oid_public_key_mldsa87);
    else
        return false;
    return true;
}

/* ------------------------------------------------------------------- enums */

/* The digits of v in buf, which has room for 21 bytes, with a sign. */
static Int x509_itoa(Int v, char *buf) {
    char tmp[24];
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    Int n = 0;
    do {
        tmp[n++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    Int k = 0;
    if (v < 0)
        buf[k++] = '-';
    while (n > 0)
        buf[k++] = tmp[--n];
    return k;
}

/* prefix, v and suffix, in a. */
static Str x509_number_string(Alloc *a, const char *prefix, Int v, const char *suffix) {
    char num[24];
    Int nl = x509_itoa(v, num);
    size_t pl = strlen(prefix), sl = strlen(suffix);
    size_t len = pl + (size_t)nl + sl;
    Byte *p = (Byte *)mem_alloc_nozero(a, len, 1);
    if (p == NULL)
        return str_from_cstr("");
    memcpy(p, prefix, pl);
    memcpy(p + pl, num, (size_t)nl);
    memcpy(p + pl + (size_t)nl, suffix, sl);
    return str_from_bytes(p, (Int)len);
}

Str x509_signature_algorithm_string(X509SignatureAlgorithm algo, Alloc *a) {
    for (Int i = 0; i < X509_NSIG; i++)
        if (x509_signature_algorithm_details[i].algo == algo)
            return str_from_cstr(x509_signature_algorithm_details[i].name);
    return x509_number_string(a, "", algo, "");
}

Str x509_public_key_algorithm_string(X509PublicKeyAlgorithm algo, Alloc *a) {
    static const char *const names[] = {NULL,    "RSA",     "DSA",
                                        "ECDSA", "Ed25519", "ML-DSA"};
    if (algo > 0 && algo < (Int)(sizeof names / sizeof names[0]))
        return str_from_cstr(names[algo]);
    return x509_number_string(a, "", algo, "");
}

Str x509_key_usage_string(X509KeyUsage i, Alloc *a) {
    switch (i) {
    case X509_KEY_USAGE_DIGITAL_SIGNATURE:
        return BURROW_S("digitalSignature");
    case X509_KEY_USAGE_CONTENT_COMMITMENT:
        return BURROW_S("contentCommitment");
    case X509_KEY_USAGE_KEY_ENCIPHERMENT:
        return BURROW_S("keyEncipherment");
    case X509_KEY_USAGE_DATA_ENCIPHERMENT:
        return BURROW_S("dataEncipherment");
    case X509_KEY_USAGE_KEY_AGREEMENT:
        return BURROW_S("keyAgreement");
    case X509_KEY_USAGE_CERT_SIGN:
        return BURROW_S("keyCertSign");
    case X509_KEY_USAGE_CRL_SIGN:
        return BURROW_S("cRLSign");
    case X509_KEY_USAGE_ENCIPHER_ONLY:
        return BURROW_S("encipherOnly");
    case X509_KEY_USAGE_DECIPHER_ONLY:
        return BURROW_S("decipherOnly");
    default:
        return x509_number_string(a, "KeyUsage(", i, ")");
    }
}

Str x509_ext_key_usage_string(X509ExtKeyUsage i, Alloc *a) {
    static const char *const names[] = {
        "anyExtendedKeyUsage", "serverAuth",     "clientAuth",  "codeSigning",
        "emailProtection",     "ipsecEndSystem", "ipsecTunnel", "ipsecUser",
        "timeStamping",        "OCSPSigning",    "msSGC",       "nsSGC",
        "msCodeCom",           "msKernelCode",
    };
    if (i >= 0 && i < (Int)(sizeof names / sizeof names[0]))
        return str_from_cstr(names[i]);
    return x509_number_string(a, "ExtKeyUsage(", i, ")");
}

/* --------------------------------------------------------------------- OID */

BURROW_SENTINEL_ERROR(burrow__x509_err_invalid_oid, "invalid oid");

/* newOIDFromDER: whether der is the contents of an OID, every arc ending and
 * none starting with a padding byte. */
static bool x509_oid_der_valid(Slice der) {
    const Byte *p = der.p;
    if (der.len == 0 || (p[der.len - 1] & 0x80) != 0)
        return false;
    Int start = 0;
    for (Int i = 0; i < der.len; i++) {
        if (i == start && p[i] == 0x80)
            return false;
        if ((p[i] & 0x80) == 0)
            start = i + 1;
    }
    return true;
}

static Int x509_base128_len(uint64_t n) {
    if (n == 0)
        return 1;
    return (x509_bit_len64(n) + 6) / 7;
}

static Int x509_append_base128(Byte *dst, uint64_t n) {
    Int l = x509_base128_len(n);
    for (Int i = l - 1; i >= 0; i--) {
        Byte o = (Byte)((n >> (unsigned)(i * 7)) & 0x7f);
        if (i != 0)
            o |= 0x80;
        *dst++ = o;
    }
    return l;
}

/* OIDFromInts on the arcs in u or, when u is NULL, in s. */
static X509OID x509_oid_from_arcs(Alloc *a, const uint64_t *u, const Int *s, Int n,
                                  Error *err) {
#define X509_ARC(i) (u != NULL ? u[i] : (uint64_t)s[i])
    if (n < 2 || X509_ARC(0) > 2 || (X509_ARC(0) < 2 && X509_ARC(1) >= 40)) {
        BURROW_OUT(err, burrow__x509_err_invalid_oid);
        return (X509OID){{0}};
    }
    uint64_t first = X509_ARC(0) * 40 + X509_ARC(1);
    Int length = x509_base128_len(first);
    for (Int i = 2; i < n; i++)
        length += x509_base128_len(X509_ARC(i));
    Slice der = slice_make(a, TYPE_BYTE, length, length);
    if (der.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (X509OID){{0}};
    }
    Byte *p = der.p;
    p += x509_append_base128(p, first);
    for (Int i = 2; i < n; i++)
        p += x509_append_base128(p, X509_ARC(i));
#undef X509_ARC
    BURROW_OUT(err, BURROW_NO_ERROR);
    return (X509OID){der};
}

X509OID x509_oid_from_ints(Alloc *a, Slice oid, Error *err) {
    return x509_oid_from_arcs(a, (const uint64_t *)oid.p, NULL, oid.len, err);
}

X509OID x509_oid_from_asn1_oid(Alloc *a, Asn1ObjectIdentifier asn1_oid, Error *err) {
    const Int *s = asn1_oid.p;
    for (Int i = 0; i < asn1_oid.len; i++) {
        if (s[i] < 0) {
            x509_fail(err, "x509: OID components must be non-negative");
            return (X509OID){{0}};
        }
    }
    return x509_oid_from_arcs(a, NULL, s, asn1_oid.len, err);
}

/* The decimal digits in s as little endian base 128 limbs in l, which has
 * room for s.len + 1, without the zero limbs at the top. The count. */
static Int x509_decimal_to_base128(Str s, Byte *l) {
    Int n = 0;
    for (Int i = 0; i < s.len; i++) {
        unsigned carry = (unsigned)(s.p[i] - '0');
        for (Int j = 0; j < n; j++) {
            unsigned v = (unsigned)l[j] * 10U + carry;
            l[j] = (Byte)(v & 0x7fU);
            carry = v >> 7;
        }
        while (carry != 0) {
            l[n++] = (Byte)(carry & 0x7fU);
            carry >>= 7;
        }
    }
    while (n > 0 && l[n - 1] == 0)
        n--;
    return n;
}

/* appendBase128BigInt, for the limbs from x509_decimal_to_base128. */
static Int x509_append_limbs(Byte *dst, const Byte *l, Int n) {
    if (n == 0) {
        dst[0] = 0;
        return 1;
    }
    for (Int j = n - 1; j >= 0; j--)
        *dst++ = (Byte)(l[j] | (j != 0 ? 0x80 : 0));
    return n;
}

/* unmarshalOIDText, the der in a. */
static Error x509_unmarshal_oid_text(Alloc *a, Str oid, X509OID *out) {
    for (Int i = 0; i < oid.len; i++) {
        Byte c = oid.p[i];
        if (!(c >= '0' && c <= '9') && c != '.')
            return burrow__x509_err_invalid_oid;
    }
    bool more;
    Str rest;
    Str first_num = strings_cut(oid, BURROW_S("."), &rest, &more);
    if (!more)
        return burrow__x509_err_invalid_oid;
    Str second_num = strings_cut(rest, BURROW_S("."), &rest, &more);
    if (first_num.len == 0 || second_num.len == 0)
        return burrow__x509_err_invalid_oid;

    /* Every arc takes at most as many bytes as it has digits, and the first
     * two together at most as many as the two of them. */
    Byte *l = (Byte *)mem_alloc_nozero(a, (size_t)oid.len + 2, 1);
    Slice der = slice_make(a, TYPE_BYTE, 0, oid.len);
    if (l == NULL || der.p == NULL)
        return burrow_err_out_of_memory;
    Byte *p = der.p;
    Error e = burrow__x509_err_invalid_oid;

    Int fl = x509_decimal_to_base128(first_num, l);
    unsigned first = fl == 0 ? 0U : l[0];
    if (fl > 1 || first > 2)
        goto out;
    Int sl = x509_decimal_to_base128(second_num, l);
    if (first < 2 && (sl > 1 || (sl == 1 && l[0] >= 40)))
        goto out;
    unsigned carry = first * 40U;
    for (Int j = 0; carry != 0; j++) {
        if (j == sl)
            l[sl++] = 0;
        unsigned v = l[j] + carry;
        l[j] = (Byte)(v & 0x7fU);
        carry = v >> 7;
    }
    der.len += x509_append_limbs(p, l, sl);

    while (more) {
        Str num = strings_cut(rest, BURROW_S("."), &rest, &more);
        if (num.len == 0)
            goto out;
        Int n = x509_decimal_to_base128(num, l);
        der.len += x509_append_limbs(p + der.len, l, n);
    }
    out->der = der;
    e = BURROW_NO_ERROR;
out:
    mem_free(a, l, (size_t)oid.len + 2, 1);
    return e;
}

X509OID x509_parse_oid(Alloc *a, Str oid, Error *err) {
    X509OID o = {{0}};
    Error e = x509_unmarshal_oid_text(a, oid, &o);
    BURROW_OUT(err, e);
    return o;
}

bool x509_oid_equal(X509OID oid, X509OID other) {
    return bytes_equal(oid.der, other.der);
}

/* parseBase128Int. False when it fails. */
static bool x509_parse_base128_int(Slice b, Int *offset, Int *ret) {
    const Byte *p = b.p;
    int64_t ret64 = 0;
    for (Int shifted = 0; *offset < b.len; shifted++) {
        if (shifted == 5)
            return false;
        ret64 <<= 7;
        Byte c = p[*offset];
        if (shifted == 0 && c == 0x80)
            return false;
        ret64 |= (int64_t)(c & 0x7f);
        (*offset)++;
        if ((c & 0x80) == 0) {
            *ret = (Int)ret64;
            return ret64 <= INT32_MAX;
        }
    }
    return false;
}

bool x509_oid_equal_asn1_oid(X509OID oid, Asn1ObjectIdentifier other) {
    const Int *o = other.p;
    if (other.len < 2)
        return false;
    Int offset = 0, v;
    if (!x509_parse_base128_int(oid.der, &offset, &v))
        return false;
    if (v < 80) {
        if (o[0] != v / 40 || o[1] != v % 40)
            return false;
    } else {
        if (o[0] != 2 || o[1] != v - 80)
            return false;
    }
    Int i = 2;
    for (; offset < oid.der.len; i++) {
        if (!x509_parse_base128_int(oid.der, &offset, &v))
            return false;
        if (i >= other.len || v != o[i])
            return false;
    }
    return i == other.len;
}

/* The arc in the 7-bit groups g[0..n) in decimal at out, less 80 when sub80,
 * for an arc too big for a uint64_t. d is scratch for 3n + 1 digits. The
 * count. */
static Int x509_big_arc(const Byte *g, Int n, bool sub80, Byte *d, Byte *out) {
    Int m = 0;
    for (Int i = 0; i < n; i++) {
        unsigned carry = g[i] & 0x7fU;
        for (Int j = 0; j < m; j++) {
            unsigned v = (unsigned)d[j] * 128U + carry;
            d[j] = (Byte)(v % 10U);
            carry = v / 10U;
        }
        while (carry != 0) {
            d[m++] = (Byte)(carry % 10U);
            carry /= 10U;
        }
    }
    if (sub80) {
        /* The arc is past 2^64 here, so taking 80 off cannot go below zero. */
        unsigned borrow = 0;
        for (Int j = 0; j < m; j++) {
            int s = (int)d[j] - (int)borrow - (j == 0 ? 0 : j == 1 ? 8 : 0);
            borrow = s < 0;
            d[j] = (Byte)(s < 0 ? s + 10 : s);
        }
        while (m > 1 && d[m - 1] == 0)
            m--;
    }
    for (Int j = 0; j < m; j++)
        out[j] = (Byte)('0' + d[m - 1 - j]);
    return m;
}

static Int x509_utoa(uint64_t u, Byte *out) {
    Byte tmp[20];
    Int n = 0;
    do {
        tmp[n++] = (Byte)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    for (Int i = 0; i < n; i++)
        out[i] = tmp[n - 1 - i];
    return n;
}

Str x509_oid_string(X509OID oid, Alloc *a) {
    const Byte *der = oid.der.p;
    Int n = oid.der.len;
    if (n == 0)
        return BURROW_S("");
    /* Seven bits are never more than three digits, and every arc adds a dot,
     * and the first one "2." as well. */
    size_t cap = (size_t)n * 4 + 4;
    Byte *buf = (Byte *)mem_alloc_nozero(a, cap, 1);
    Byte *d = (Byte *)mem_alloc_nozero(a, (size_t)n * 3 + 2, 1);
    if (buf == NULL || d == NULL)
        return BURROW_S("");
    Int k = 0, start = 0;
    for (Int i = 0; i < n; i++) {
        if ((der[i] & 0x80) != 0)
            continue;
        /* der[start..i] is one arc. */
        Int groups = i - start + 1;
        Int bits = 7 * (groups - 1) + x509_bit_len64(der[start] & 0x7fU);
        if (start != 0)
            buf[k++] = '.';
        if (bits <= 64) {
            uint64_t val = 0;
            for (Int j = start; j <= i; j++)
                val = val << 7 | (uint64_t)(der[j] & 0x7f);
            if (start == 0) {
                if (val < 80) {
                    k += x509_utoa(val / 40, buf + k);
                    buf[k++] = '.';
                    k += x509_utoa(val % 40, buf + k);
                } else {
                    buf[k++] = '2';
                    buf[k++] = '.';
                    k += x509_utoa(val - 80, buf + k);
                }
            } else {
                k += x509_utoa(val, buf + k);
            }
        } else {
            if (start == 0) {
                buf[k++] = '2';
                buf[k++] = '.';
            }
            k += x509_big_arc(der + start, groups, start == 0, d, buf + k);
        }
        start = i + 1;
    }
    mem_free(a, d, (size_t)n * 3 + 2, 1);
    return str_from_bytes(buf, k);
}

Slice x509_oid_append_text(X509OID o, Alloc *a, Slice b, Error *err) {
    Str s = x509_oid_string(o, a);
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    return slice_append_slice(a, b, x509_bytes(s.p, s.len));
}

Slice x509_oid_marshal_text(X509OID o, Alloc *a, Error *err) {
    return x509_oid_append_text(o, a, slice_nil(TYPE_BYTE), err);
}

Error x509_oid_unmarshal_text(X509OID *o, Alloc *a, Slice text) {
    return x509_unmarshal_oid_text(a, str_from_bytes(text.p, text.len), o);
}

Slice x509_oid_append_binary(X509OID o, Alloc *a, Slice b, Error *err) {
    BURROW_OUT(err, BURROW_NO_ERROR);
    if (b.elem == NULL)
        b = slice_nil(TYPE_BYTE);
    return slice_append_slice(a, b, o.der);
}

Slice x509_oid_marshal_binary(X509OID o, Alloc *a, Error *err) {
    return x509_oid_append_binary(o, a, slice_nil(TYPE_BYTE), err);
}

Error x509_oid_unmarshal_binary(X509OID *o, Alloc *a, Slice b) {
    if (!x509_oid_der_valid(b))
        return burrow__x509_err_invalid_oid;
    Slice der = x509_clone(a, b);
    if (der.p == NULL)
        return burrow_err_out_of_memory;
    o->der = der;
    return BURROW_NO_ERROR;
}

X509OID x509_ext_key_usage_oid(X509ExtKeyUsage eku, Alloc *a) {
    if (eku < 0 || eku >= X509_NEKU)
        panic_str(BURROW_S("x509: internal error: known ExtKeyUsage has no OID"));
    const X509ExtKeyUsageOID *e = &x509_ext_key_usage_oids[eku];
    Error err = BURROW_NO_ERROR;
    X509OID oid = x509_oid_from_arcs(a, NULL, e->oid, e->len, &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509: internal error: known ExtKeyUsage has invalid OID"));
    return oid;
}

/* ------------------------------------------------------------------ PKCS #1 */

/* The x509rsacrt=0 way out: the key again, with the CRT values worked out
 * rather than taken from the encoding. */
static bool x509_rsa_retry_without_crt(RsaPrivateKey *key, Alloc *a) {
    if ((x509_debug_load() & X509_DEBUG_RSACRT_OFF) == 0)
        return false;
    key->precomputed.dp = NULL;
    key->precomputed.dq = NULL;
    key->precomputed.qinv = NULL;
    rsa_private_key_precompute(key, a);
    return !BURROW_FAILED(rsa_private_key_validate(key));
}

RsaPrivateKey *x509_parse_pkcs1_private_key(Alloc *a, Slice der, Error *err) {
    X509Pkcs1PrivateKey priv;
    memset(&priv, 0, sizeof priv);
    Error e = BURROW_NO_ERROR;
    Slice rest =
        asn1_unmarshal(a, der, BURROW_ANY(TYPE_OF(X509Pkcs1PrivateKey), &priv), &e);
    if (rest.len > 0) {
        BURROW_OUT(err, x509_trailing_data());
        return NULL;
    }
    if (BURROW_FAILED(e)) {
        if (X509_DER_IS(der, X509EcPrivateKey)) {
            x509_fail(err, "x509: failed to parse private key (use ParseECPrivateKey "
                           "instead for this key format)");
            return NULL;
        }
        if (X509_DER_IS(der, X509Pkcs8)) {
            x509_fail(err,
                      "x509: failed to parse private key (use ParsePKCS8PrivateKey "
                      "instead for this key format)");
            return NULL;
        }
        BURROW_OUT(err, e);
        return NULL;
    }

    if (priv.version > 1) {
        x509_fail(err, "x509: unsupported private key version");
        return NULL;
    }
    if (big_int_sign(priv.n) <= 0 || big_int_sign(priv.d) <= 0 ||
        big_int_sign(priv.p) <= 0 || big_int_sign(priv.q) <= 0 ||
        (priv.dp != NULL && big_int_sign(priv.dp) <= 0) ||
        (priv.dq != NULL && big_int_sign(priv.dq) <= 0) ||
        (priv.qinv != NULL && big_int_sign(priv.qinv) <= 0)) {
        x509_fail(err, "x509: private key contains zero or negative value");
        return NULL;
    }

    Int np = 2 + priv.additional_primes.len;
    RsaPrivateKey *key = BURROW_NEW(a, RsaPrivateKey);
    BigInt **primes = BURROW_NEW_N(a, BigInt *, (size_t)np);
    if (key == NULL || primes == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    key->public_key.e = priv.e;
    key->public_key.n = priv.n;
    key->d = priv.d;
    key->primes = primes;
    key->primes_len = np;
    primes[0] = priv.p;
    primes[1] = priv.q;
    key->precomputed.dp = priv.dp;
    key->precomputed.dq = priv.dq;
    key->precomputed.qinv = priv.qinv;
    const X509Pkcs1AdditionalPrime *ap = priv.additional_primes.p;
    for (Int i = 0; i < priv.additional_primes.len; i++) {
        if (big_int_sign(ap[i].prime) <= 0) {
            x509_fail(err, "x509: private key contains zero or negative prime");
            return NULL;
        }
        primes[i + 2] = ap[i].prime;
    }

    rsa_private_key_precompute(key, a);
    Error ve = rsa_private_key_validate(key);
    if (BURROW_FAILED(ve)) {
        if (x509_rsa_retry_without_crt(key, a)) {
            BURROW_OUT(err, BURROW_NO_ERROR);
            return key;
        }
        BURROW_OUT(err, ve);
        return NULL;
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return key;
}

Slice x509_marshal_pkcs1_private_key(Alloc *a, RsaPrivateKey *key) {
    rsa_private_key_precompute(key, a);
    if (key->primes_len < 2)
        panic_str(BURROW_S("runtime error: index out of range [1] with length 1"));

    X509Pkcs1PrivateKey priv;
    memset(&priv, 0, sizeof priv);
    priv.version = key->primes_len > 2 ? 1 : 0;
    priv.n = key->public_key.n;
    priv.e = key->public_key.e;
    priv.d = key->d;
    priv.p = key->primes[0];
    priv.q = key->primes[1];
    priv.dp = key->precomputed.dp;
    priv.dq = key->precomputed.dq;
    priv.qinv = key->precomputed.qinv;

    Int ncrt = key->precomputed.crt_values_len;
    priv.additional_primes =
        slice_make(a, TYPE_OF(X509Pkcs1AdditionalPrime), ncrt, ncrt);
    X509Pkcs1AdditionalPrime *ap = priv.additional_primes.p;
    for (Int i = 0; i < ncrt; i++) {
        ap[i].prime = key->primes[2 + i];
        ap[i].exp = key->precomputed.crt_values[i].exp;
        ap[i].coeff = key->precomputed.crt_values[i].coeff;
    }
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(X509Pkcs1PrivateKey), &priv), NULL);
}

RsaPublicKey *x509_parse_pkcs1_public_key(Alloc *a, Slice der, Error *err) {
    X509Pkcs1PublicKey pub;
    memset(&pub, 0, sizeof pub);
    Error e = BURROW_NO_ERROR;
    Slice rest =
        asn1_unmarshal(a, der, BURROW_ANY(TYPE_OF(X509Pkcs1PublicKey), &pub), &e);
    if (BURROW_FAILED(e)) {
        if (X509_DER_IS(der, X509PublicKeyInfo)) {
            x509_fail(err, "x509: failed to parse public key (use ParsePKIXPublicKey "
                           "instead for this key format)");
            return NULL;
        }
        BURROW_OUT(err, e);
        return NULL;
    }
    if (rest.len > 0) {
        BURROW_OUT(err, x509_trailing_data());
        return NULL;
    }
    if (big_int_sign(pub.n) <= 0 || pub.e <= 0) {
        x509_fail(err, "x509: public key contains zero or negative value");
        return NULL;
    }
    if (pub.e > INT32_MAX) {
        x509_fail(err, "x509: public key contains large public exponent");
        return NULL;
    }
    RsaPublicKey *key = BURROW_NEW(a, RsaPublicKey);
    if (key == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    key->e = pub.e;
    key->n = pub.n;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return key;
}

Slice x509_marshal_pkcs1_public_key(Alloc *a, const RsaPublicKey *key) {
    X509Pkcs1PublicKey pub = {key->n, key->e};
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(X509Pkcs1PublicKey), &pub), NULL);
}

/* -------------------------------------------------------------------- SEC 1 */

/* marshalECPrivateKeyWithOID, with no curve written for a nil oid. */
static Slice x509_marshal_ec_private_key_with_oid(Alloc *a, const EcdsaPrivateKey *key,
                                                  Asn1ObjectIdentifier oid,
                                                  Error *err) {
    Error e = BURROW_NO_ERROR;
    Slice private_key = ecdsa_private_key_bytes(key, a, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }
    Slice public_key = ecdsa_public_key_bytes(&key->public_key, a, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }
    X509EcPrivateKey k;
    memset(&k, 0, sizeof k);
    k.version = 1;
    k.private_key = private_key;
    k.named_curve_oid = oid;
    k.public_key.bytes = public_key;
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(X509EcPrivateKey), &k), err);
}

/* marshalECDHPrivateKey */
static Slice x509_marshal_ecdh_private_key(Alloc *a, const EcdhPrivateKey *key,
                                           Error *err) {
    X509EcPrivateKey k;
    memset(&k, 0, sizeof k);
    k.version = 1;
    k.private_key = ecdh_private_key_bytes(key, a);
    k.public_key.bytes = ecdh_public_key_bytes(ecdh_private_key_public_key(key), a);
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(X509EcPrivateKey), &k), err);
}

/* parseECPrivateKey, with the curve from named_curve_oid when it is not
 * NULL. */
static EcdsaPrivateKey *
x509_parse_ec_private_key_oid(Alloc *a, const Asn1ObjectIdentifier *named_curve_oid,
                              Slice der, Error *err) {
    X509EcPrivateKey priv;
    memset(&priv, 0, sizeof priv);
    Error e = BURROW_NO_ERROR;
    (void)asn1_unmarshal(a, der, BURROW_ANY(TYPE_OF(X509EcPrivateKey), &priv), &e);
    if (BURROW_FAILED(e)) {
        if (X509_DER_IS(der, X509Pkcs8)) {
            x509_fail(err,
                      "x509: failed to parse private key (use ParsePKCS8PrivateKey "
                      "instead for this key format)");
            return NULL;
        }
        if (X509_DER_IS(der, X509Pkcs1PrivateKey)) {
            x509_fail(err,
                      "x509: failed to parse private key (use ParsePKCS1PrivateKey "
                      "instead for this key format)");
            return NULL;
        }
        BURROW_OUT(err, fmt_errorf_v("x509: failed to parse EC private key: %v", e));
        return NULL;
    }
    if (priv.version != 1) {
        BURROW_OUT(
            err, fmt_errorf_v("x509: unknown EC private key version %d", priv.version));
        return NULL;
    }

    EllipticCurve curve;
    if (!x509_named_curve_from_oid(named_curve_oid != NULL ? *named_curve_oid
                                                           : priv.named_curve_oid,
                                   &curve)) {
        x509_fail(err, "x509: unknown elliptic curve");
        return NULL;
    }

    Int size = (big_int_bit_len(elliptic_curve_params(curve)->n) + 7) / 8;
    Slice pk = priv.private_key;
    while (pk.len > size) {
        if (((const Byte *)pk.p)[0] != 0) {
            x509_fail(err, "x509: invalid private key length");
            return NULL;
        }
        pk = slice_sub(pk, 1, pk.len);
    }
    Byte buf[66];
    memset(buf, 0, sizeof buf);
    if (pk.len > 0)
        memcpy(buf + (size - pk.len), pk.p, (size_t)pk.len);
    EcdsaPrivateKey *key =
        ecdsa_parse_raw_private_key(a, curve, x509_bytes(buf, size), err);
    memset(buf, 0, sizeof buf);
    return key;
}

EcdsaPrivateKey *x509_parse_ec_private_key(Alloc *a, Slice der, Error *err) {
    return x509_parse_ec_private_key_oid(a, NULL, der, err);
}

Slice x509_marshal_ec_private_key(Alloc *a, const EcdsaPrivateKey *key, Error *err) {
    Asn1ObjectIdentifier oid;
    if (!x509_oid_from_named_curve(key->public_key.curve, &oid)) {
        x509_fail(err, "x509: unknown elliptic curve");
        return slice_nil(TYPE_BYTE);
    }
    return x509_marshal_ec_private_key_with_oid(a, key, oid, err);
}

/* ------------------------------------------------------------------ PKCS #8 */

/* key in an Any made from a. */
static Any x509_any(Alloc *a, const Type *t, void *key, Error *err) {
    if (key == NULL)
        return (Any){NULL, NULL};
    BURROW_OUT(err, BURROW_NO_ERROR);
    return BURROW_ANY(t, key);
}

/* A slice key such as Ed25519's in an Any, the Slice made from a. */
static Any x509_slice_any(Alloc *a, const Type *t, Slice key, Error *err) {
    Slice *p = BURROW_NEW(a, Slice);
    if (p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return (Any){NULL, NULL};
    }
    *p = key;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return BURROW_ANY(t, p);
}

static Any x509_parse_pkcs8_mldsa(Alloc *a, const X509Pkcs8 *k, Error *err) {
    if (k->algo.parameters.full_bytes.len != 0) {
        x509_fail(err, "x509: invalid ML-DSA private key parameters");
        return (Any){NULL, NULL};
    }
    const Byte *p = k->private_key.p;
    Int l = k->private_key.len;
    if (l == 0) {
        BURROW_OUT(err, fmt_errorf_v("x509: invalid ML-DSA private key length: %d", l));
        return (Any){NULL, NULL};
    }
    switch (p[0]) {
    case 0x80: /* IMPLICIT [0] OCTET STRING, the seed */
        break;
    case 0x04: /* OCTET STRING, the expanded key */
        x509_fail(err, "x509: semi-expanded ML-DSA private keys without seed are not "
                       "supported");
        return (Any){NULL, NULL};
    case 0x30: /* SEQUENCE, both */
        x509_fail(err, "x509: ML-DSA private keys with both seed and expanded key are "
                       "not supported, use e.g. \"openssl pkey -provparam "
                       "ml-dsa.output_formats=seed-only\" to convert to a seed-only "
                       "key");
        return (Any){NULL, NULL};
    default:
        BURROW_OUT(err, fmt_errorf_v("x509: invalid ML-DSA private key: invalid ASN.1 "
                                     "tag %02x",
                                     (int)p[0]));
        return (Any){NULL, NULL};
    }
    if (l != 2 + MLDSA_PRIVATE_KEY_SIZE) {
        BURROW_OUT(err, fmt_errorf_v("x509: invalid ML-DSA private key length: %d", l));
        return (Any){NULL, NULL};
    }
    if (p[1] != MLDSA_PRIVATE_KEY_SIZE) {
        x509_fail(err, "x509: invalid ML-DSA private key ASN.1 encoding");
        return (Any){NULL, NULL};
    }
    const MldsaParameters *params = x509_mldsa_parameters_from_oid(k->algo.algorithm);
    if (params == NULL) {
        x509_fail(err, "x509: unknown ML-DSA parameters");
        return (Any){NULL, NULL};
    }
    MldsaPrivateKey *key = mldsa_new_private_key(
        params, a, slice_sub(k->private_key, 2, k->private_key.len), err);
    return x509_any(a, TYPE_MLDSA_PRIVATE_KEY, key, err);
}

Any x509_parse_pkcs8_private_key(Alloc *a, Slice der, Error *err) {
    X509Pkcs8 priv;
    memset(&priv, 0, sizeof priv);
    Error e = BURROW_NO_ERROR;
    (void)asn1_unmarshal(a, der, BURROW_ANY(TYPE_OF(X509Pkcs8), &priv), &e);
    if (BURROW_FAILED(e)) {
        if (X509_DER_IS(der, X509EcPrivateKey)) {
            x509_fail(err, "x509: failed to parse private key (use ParseECPrivateKey "
                           "instead for this key format)");
            return (Any){NULL, NULL};
        }
        if (X509_DER_IS(der, X509Pkcs1PrivateKey)) {
            x509_fail(err,
                      "x509: failed to parse private key (use ParsePKCS1PrivateKey "
                      "instead for this key format)");
            return (Any){NULL, NULL};
        }
        BURROW_OUT(err, e);
        return (Any){NULL, NULL};
    }

    Asn1ObjectIdentifier algo = priv.algo.algorithm;
    if (x509_oid_is(algo, X509_OID(x509_oid_public_key_rsa))) {
        RsaPrivateKey *key = x509_parse_pkcs1_private_key(a, priv.private_key, &e);
        if (key == NULL) {
            BURROW_OUT(err, fmt_errorf_v("x509: failed to parse RSA private key "
                                         "embedded in PKCS#8: %v",
                                         e));
            return (Any){NULL, NULL};
        }
        return x509_any(a, TYPE_RSA_PRIVATE_KEY, key, err);
    }

    if (x509_oid_is(algo, X509_OID(x509_oid_public_key_ecdsa))) {
        Asn1ObjectIdentifier named = {0};
        Error oe = BURROW_NO_ERROR;
        (void)asn1_unmarshal(a, priv.algo.parameters.full_bytes,
                             BURROW_ANY(TYPE_ASN1_OBJECT_IDENTIFIER, &named), &oe);
        EcdsaPrivateKey *key = x509_parse_ec_private_key_oid(
            a, BURROW_FAILED(oe) ? NULL : &named, priv.private_key, &e);
        if (key == NULL) {
            BURROW_OUT(err, fmt_errorf_v("x509: failed to parse EC private key "
                                         "embedded in PKCS#8: %v",
                                         e));
            return (Any){NULL, NULL};
        }
        return x509_any(a, TYPE_ECDSA_PRIVATE_KEY, key, err);
    }

    if (x509_oid_is(algo, X509_OID(x509_oid_public_key_ed25519))) {
        if (priv.algo.parameters.full_bytes.len != 0) {
            x509_fail(err, "x509: invalid Ed25519 private key parameters");
            return (Any){NULL, NULL};
        }
        Slice seed = {0};
        (void)asn1_unmarshal(a, priv.private_key, BURROW_ANY(TYPE_BYTES, &seed), &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, fmt_errorf_v("x509: invalid Ed25519 private key: %v", e));
            return (Any){NULL, NULL};
        }
        if (seed.len != ED25519_SEED_SIZE) {
            BURROW_OUT(err, fmt_errorf_v("x509: invalid Ed25519 private key length: %d",
                                         seed.len));
            return (Any){NULL, NULL};
        }
        Ed25519PrivateKey key = ed25519_new_key_from_seed(a, seed);
        if (key.p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return (Any){NULL, NULL};
        }
        return x509_slice_any(a, TYPE_ED25519_PRIVATE_KEY, key, err);
    }

    if (x509_is_mldsa_oid(algo))
        return x509_parse_pkcs8_mldsa(a, &priv, err);

    if (x509_oid_is(algo, X509_OID(x509_oid_public_key_x25519))) {
        if (priv.algo.parameters.full_bytes.len != 0) {
            x509_fail(err, "x509: invalid X25519 private key parameters");
            return (Any){NULL, NULL};
        }
        Slice k = {0};
        (void)asn1_unmarshal(a, priv.private_key, BURROW_ANY(TYPE_BYTES, &k), &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, fmt_errorf_v("x509: invalid X25519 private key: %v", e));
            return (Any){NULL, NULL};
        }
        EcdhPrivateKey *key = ecdh_curve_new_private_key(ecdh_x25519(), a, k, err);
        return x509_any(a, TYPE_ECDH_PRIVATE_KEY, key, err);
    }

    BURROW_OUT(err,
               fmt_errorf_v("x509: PKCS#8 wrapping contained private key with "
                            "unknown algorithm: %s",
                            asn1_object_identifier_string(algo, error_allocator())));
    return (Any){NULL, NULL};
}

/* The parameters of an EC key: the DER of its curve's OID. */
static bool x509_curve_params(Alloc *a, Asn1ObjectIdentifier oid, Asn1RawValue *params,
                              Error *err) {
    Error e = BURROW_NO_ERROR;
    Slice b = asn1_marshal(a, BURROW_ANY(TYPE_ASN1_OBJECT_IDENTIFIER, &oid), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, fmt_errorf_v("x509: failed to marshal curve OID: %v", e));
        return false;
    }
    memset(params, 0, sizeof *params);
    params->full_bytes = b;
    return true;
}

Slice x509_marshal_pkcs8_private_key(Alloc *a, Any key, Error *err) {
    X509Pkcs8 priv;
    memset(&priv, 0, sizeof priv);
    Error e = BURROW_NO_ERROR;
    const Slice none = slice_nil(TYPE_BYTE);

    if (key.t == TYPE_RSA_PRIVATE_KEY && key.data != NULL) {
        RsaPrivateKey *k = key.data;
        priv.algo.algorithm = X509_OID(x509_oid_public_key_rsa);
        priv.algo.parameters = asn1_null_raw_value;
        rsa_private_key_precompute(k, a);
        e = rsa_private_key_validate(k);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return none;
        }
        priv.private_key = x509_marshal_pkcs1_private_key(a, k);
    } else if (key.t == TYPE_ECDSA_PRIVATE_KEY && key.data != NULL) {
        const EcdsaPrivateKey *k = key.data;
        Asn1ObjectIdentifier oid;
        if (!x509_oid_from_named_curve(k->public_key.curve, &oid)) {
            x509_fail(err, "x509: unknown curve while marshaling to PKCS#8");
            return none;
        }
        priv.algo.algorithm = X509_OID(x509_oid_public_key_ecdsa);
        if (!x509_curve_params(a, oid, &priv.algo.parameters, err))
            return none;
        priv.private_key =
            x509_marshal_ec_private_key_with_oid(a, k, (Asn1ObjectIdentifier){0}, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, fmt_errorf_v("x509: failed to marshal EC private key while "
                                         "building PKCS#8: %v",
                                         e));
            return none;
        }
    } else if (key.t == TYPE_ED25519_PRIVATE_KEY && key.data != NULL) {
        priv.algo.algorithm = X509_OID(x509_oid_public_key_ed25519);
        Slice seed = ed25519_private_key_seed(*(const Ed25519PrivateKey *)key.data, a);
        priv.private_key = asn1_marshal(a, BURROW_ANY(TYPE_BYTES, &seed), &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, fmt_errorf_v("x509: failed to marshal private key: %v", e));
            return none;
        }
    } else if (key.t == TYPE_MLDSA_PRIVATE_KEY && key.data != NULL) {
        const MldsaPrivateKey *k = key.data;
        Asn1ObjectIdentifier oid;
        if (!x509_oid_from_mldsa_parameters(
                mldsa_public_key_parameters(mldsa_private_key_public_key(k)), &oid)) {
            x509_fail(err,
                      "x509: unknown ML-DSA parameters while marshaling to PKCS#8");
            return none;
        }
        priv.algo.algorithm = oid;
        Slice seed = mldsa_private_key_bytes(k, a);
        Slice b = slice_make(a, TYPE_BYTE, 2 + seed.len, 2 + seed.len);
        if (b.p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
        ((Byte *)b.p)[0] = 0x80;
        ((Byte *)b.p)[1] = MLDSA_PRIVATE_KEY_SIZE;
        memcpy((Byte *)b.p + 2, seed.p, (size_t)seed.len);
        priv.private_key = b;
    } else if (key.t == TYPE_ECDH_PRIVATE_KEY && key.data != NULL) {
        const EcdhPrivateKey *k = key.data;
        const EcdhCurve *c = ecdh_private_key_curve(k);
        if (c == ecdh_x25519()) {
            priv.algo.algorithm = X509_OID(x509_oid_public_key_x25519);
            Slice kb = ecdh_private_key_bytes(k, a);
            priv.private_key = asn1_marshal(a, BURROW_ANY(TYPE_BYTES, &kb), &e);
            if (BURROW_FAILED(e)) {
                BURROW_OUT(err,
                           fmt_errorf_v("x509: failed to marshal private key: %v", e));
                return none;
            }
        } else {
            Asn1ObjectIdentifier oid;
            if (!x509_oid_from_ecdh_curve(c, &oid)) {
                x509_fail(err, "x509: unknown curve while marshaling to PKCS#8");
                return none;
            }
            priv.algo.algorithm = X509_OID(x509_oid_public_key_ecdsa);
            if (!x509_curve_params(a, oid, &priv.algo.parameters, err))
                return none;
            priv.private_key = x509_marshal_ecdh_private_key(a, k, &e);
            if (BURROW_FAILED(e)) {
                BURROW_OUT(err, fmt_errorf_v("x509: failed to marshal EC private key "
                                             "while building PKCS#8: %v",
                                             e));
                return none;
            }
        }
    } else {
        BURROW_OUT(err, fmt_errorf_v(
                            "x509: unknown key type while marshaling PKCS#8: %T", key));
        return none;
    }
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(X509Pkcs8), &priv), err);
}

/* ------------------------------------------------------------- PKIX keys */

/* parsePublicKey */
static Any x509_parse_public_key(Alloc *a, const X509PublicKeyInfo *ki, Error *err) {
    const Any none = {NULL, NULL};
    Asn1ObjectIdentifier oid = ki->algorithm.algorithm;
    Slice params = ki->algorithm.parameters.full_bytes;
    Slice data = asn1_bit_string_right_align(ki->public_key, a);

    if (x509_oid_is(oid, X509_OID(x509_oid_public_key_rsa))) {
        if (!bytes_equal(params, asn1_null_bytes)) {
            x509_fail(err, "x509: RSA key missing NULL parameters");
            return none;
        }
        CryptobyteString der = data, seq;
        RsaPublicKey *pub = BURROW_NEW(a, RsaPublicKey);
        BigInt *n = big_new_int(a, 0);
        if (pub == NULL || n == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
        if (!cryptobyte_string_read_asn1(&der, &seq, CRYPTOBYTE_ASN1_SEQUENCE)) {
            x509_fail(err, "x509: invalid RSA public key");
            return none;
        }
        if (!cryptobyte_string_read_asn1_integer_big(&seq, n)) {
            x509_fail(err, "x509: invalid RSA modulus");
            return none;
        }
        Int e = 0;
        if (!cryptobyte_string_read_asn1_integer_int(&seq, &e)) {
            x509_fail(err, "x509: invalid RSA public exponent");
            return none;
        }
        if (big_int_sign(n) <= 0) {
            x509_fail(err, "x509: RSA modulus is not a positive number");
            return none;
        }
        if (e <= 0) {
            x509_fail(err, "x509: RSA public exponent is not a positive number");
            return none;
        }
        pub->n = n;
        pub->e = e;
        return x509_any(a, TYPE_RSA_PUBLIC_KEY, pub, err);
    }

    if (x509_oid_is(oid, X509_OID(x509_oid_public_key_ecdsa))) {
        CryptobyteString pd = params;
        Asn1ObjectIdentifier named = {0};
        if (!cryptobyte_string_read_asn1_object_identifier(&pd, a, &named)) {
            x509_fail(err, "x509: invalid ECDSA parameters");
            return none;
        }
        EllipticCurve curve;
        if (!x509_named_curve_from_oid(named, &curve)) {
            x509_fail(err, "x509: unsupported elliptic curve");
            return none;
        }
        EcdsaPublicKey *pub = ecdsa_parse_uncompressed_public_key(a, curve, data, err);
        return x509_any(a, TYPE_ECDSA_PUBLIC_KEY, pub, err);
    }

    if (x509_oid_is(oid, X509_OID(x509_oid_public_key_ed25519))) {
        if (params.len != 0) {
            x509_fail(err, "x509: Ed25519 key encoded with illegal parameters");
            return none;
        }
        if (data.len != ED25519_PUBLIC_KEY_SIZE) {
            x509_fail(err, "x509: wrong Ed25519 public key size");
            return none;
        }
        Slice key = x509_clone(a, data);
        if (key.p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
        return x509_slice_any(a, TYPE_ED25519_PUBLIC_KEY, key, err);
    }

    if (x509_is_mldsa_oid(oid)) {
        if (params.len != 0) {
            x509_fail(err, "x509: ML-DSA key encoded with illegal parameters");
            return none;
        }
        const MldsaParameters *mp = x509_mldsa_parameters_from_oid(oid);
        if (mp == NULL) {
            x509_fail(err, "x509: unsupported ML-DSA parameters");
            return none;
        }
        MldsaPublicKey *pub = mldsa_new_public_key(mp, a, data, err);
        return x509_any(a, TYPE_MLDSA_PUBLIC_KEY, pub, err);
    }

    if (x509_oid_is(oid, X509_OID(x509_oid_public_key_x25519))) {
        if (params.len != 0) {
            x509_fail(err, "x509: X25519 key encoded with illegal parameters");
            return none;
        }
        EcdhPublicKey *pub = ecdh_curve_new_public_key(ecdh_x25519(), a, data, err);
        return x509_any(a, TYPE_ECDH_PUBLIC_KEY, pub, err);
    }

    if (x509_oid_is(oid, X509_OID(x509_oid_public_key_dsa))) {
        CryptobyteString der = data;
        DsaPublicKey *pub = BURROW_NEW(a, DsaPublicKey);
        if (pub == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
        pub->y = big_new_int(a, 0);
        pub->parameters.p = big_new_int(a, 0);
        pub->parameters.q = big_new_int(a, 0);
        pub->parameters.g = big_new_int(a, 0);
        if (pub->y == NULL || pub->parameters.p == NULL || pub->parameters.q == NULL ||
            pub->parameters.g == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return none;
        }
        if (!cryptobyte_string_read_asn1_integer_big(&der, pub->y)) {
            x509_fail(err, "x509: invalid DSA public key");
            return none;
        }
        CryptobyteString pd = params, seq;
        if (!cryptobyte_string_read_asn1(&pd, &seq, CRYPTOBYTE_ASN1_SEQUENCE) ||
            !cryptobyte_string_read_asn1_integer_big(&seq, pub->parameters.p) ||
            !cryptobyte_string_read_asn1_integer_big(&seq, pub->parameters.q) ||
            !cryptobyte_string_read_asn1_integer_big(&seq, pub->parameters.g)) {
            x509_fail(err, "x509: invalid DSA parameters");
            return none;
        }
        if (big_int_sign(pub->y) <= 0 || big_int_sign(pub->parameters.p) <= 0 ||
            big_int_sign(pub->parameters.q) <= 0 ||
            big_int_sign(pub->parameters.g) <= 0) {
            x509_fail(err, "x509: zero or negative DSA parameter");
            return none;
        }
        return x509_any(a, TYPE_DSA_PUBLIC_KEY, pub, err);
    }

    x509_fail(err, "x509: unknown public key algorithm");
    return none;
}

Any x509_parse_pkix_public_key(Alloc *a, Slice der, Error *err) {
    X509PublicKeyInfo pki;
    memset(&pki, 0, sizeof pki);
    Error e = BURROW_NO_ERROR;
    Slice rest =
        asn1_unmarshal(a, der, BURROW_ANY(TYPE_OF(X509PublicKeyInfo), &pki), &e);
    if (BURROW_FAILED(e)) {
        if (X509_DER_IS(der, X509Pkcs1PublicKey)) {
            x509_fail(err, "x509: failed to parse public key (use ParsePKCS1PublicKey "
                           "instead for this key format)");
            return (Any){NULL, NULL};
        }
        BURROW_OUT(err, e);
        return (Any){NULL, NULL};
    }
    if (rest.len != 0) {
        x509_fail(err, "x509: trailing data after ASN.1 of public-key");
        return (Any){NULL, NULL};
    }
    return x509_parse_public_key(a, &pki, err);
}

/* marshalPublicKey: the bytes that go in the bit string, and the algorithm. */
static bool x509_marshal_public_key(Alloc *a, Any pub, Slice *bytes,
                                    PkixAlgorithmIdentifier *algo, Error *err) {
    memset(algo, 0, sizeof *algo);
    Error e = BURROW_NO_ERROR;
    if (pub.t == TYPE_RSA_PUBLIC_KEY && pub.data != NULL) {
        const RsaPublicKey *k = pub.data;
        X509Pkcs1PublicKey p1 = {k->n, k->e};
        *bytes = asn1_marshal(a, BURROW_ANY(TYPE_OF(X509Pkcs1PublicKey), &p1), &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return false;
        }
        algo->algorithm = X509_OID(x509_oid_public_key_rsa);
        algo->parameters = asn1_null_raw_value;
    } else if (pub.t == TYPE_ECDSA_PUBLIC_KEY && pub.data != NULL) {
        const EcdsaPublicKey *k = pub.data;
        Asn1ObjectIdentifier oid;
        if (!x509_oid_from_named_curve(k->curve, &oid)) {
            x509_fail(err, "x509: unsupported elliptic curve");
            return false;
        }
        *bytes = ecdsa_public_key_bytes(k, a, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return false;
        }
        algo->algorithm = X509_OID(x509_oid_public_key_ecdsa);
        Slice pb = asn1_marshal(a, BURROW_ANY(TYPE_ASN1_OBJECT_IDENTIFIER, &oid), &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return false;
        }
        algo->parameters.full_bytes = pb;
    } else if (pub.t == TYPE_ED25519_PUBLIC_KEY && pub.data != NULL) {
        *bytes = *(const Ed25519PublicKey *)pub.data;
        algo->algorithm = X509_OID(x509_oid_public_key_ed25519);
    } else if (pub.t == TYPE_MLDSA_PUBLIC_KEY && pub.data != NULL) {
        const MldsaPublicKey *k = pub.data;
        Asn1ObjectIdentifier oid;
        if (!x509_oid_from_mldsa_parameters(mldsa_public_key_parameters(k), &oid)) {
            x509_fail(err, "x509: unsupported ML-DSA parameters");
            return false;
        }
        *bytes = mldsa_public_key_bytes(k, a);
        algo->algorithm = oid;
    } else if (pub.t == TYPE_ECDH_PUBLIC_KEY && pub.data != NULL) {
        const EcdhPublicKey *k = pub.data;
        const EcdhCurve *c = ecdh_public_key_curve(k);
        *bytes = ecdh_public_key_bytes(k, a);
        if (c == ecdh_x25519()) {
            algo->algorithm = X509_OID(x509_oid_public_key_x25519);
        } else {
            Asn1ObjectIdentifier oid;
            if (!x509_oid_from_ecdh_curve(c, &oid)) {
                x509_fail(err, "x509: unsupported elliptic curve");
                return false;
            }
            algo->algorithm = X509_OID(x509_oid_public_key_ecdsa);
            Slice pb =
                asn1_marshal(a, BURROW_ANY(TYPE_ASN1_OBJECT_IDENTIFIER, &oid), &e);
            if (BURROW_FAILED(e)) {
                BURROW_OUT(err, e);
                return false;
            }
            algo->parameters.full_bytes = pb;
        }
    } else {
        BURROW_OUT(err, fmt_errorf_v("x509: unsupported public key type: %T", pub));
        return false;
    }
    return true;
}

Slice x509_marshal_pkix_public_key(Alloc *a, Any pub, Error *err) {
    Slice bytes;
    X509PkixPublicKey pkix;
    if (!x509_marshal_public_key(a, pub, &bytes, &pkix.algo, err))
        return slice_nil(TYPE_BYTE);
    pkix.bit_string.bytes = bytes;
    pkix.bit_string.bit_length = 8 * bytes.len;
    BURROW_OUT(err, BURROW_NO_ERROR);
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(X509PkixPublicKey), &pkix), NULL);
}

/* ----------------------------------------------------------- encrypted PEM */

BURROW_SENTINEL_ERROR(x509_incorrect_password_error,
                      "x509: decryption password incorrect");

/* rfc1423Algo */
typedef struct X509Rfc1423Algo {
    X509PEMCipher cipher;
    const char *name;
    bool des;
    Int key_size;
    Int block_size;
} X509Rfc1423Algo;

static const X509Rfc1423Algo x509_rfc1423_algos[] = {
    {X509_PEM_CIPHER_DES, "DES-CBC", true, 8, DES_BLOCK_SIZE},
    {X509_PEM_CIPHER3_DES, "DES-EDE3-CBC", true, 24, DES_BLOCK_SIZE},
    {X509_PEM_CIPHER_AES128, "AES-128-CBC", false, 16, AES_BLOCK_SIZE},
    {X509_PEM_CIPHER_AES192, "AES-192-CBC", false, 24, AES_BLOCK_SIZE},
    {X509_PEM_CIPHER_AES256, "AES-256-CBC", false, 32, AES_BLOCK_SIZE},
};

#define X509_NALGOS ((Int)(sizeof x509_rfc1423_algos / sizeof x509_rfc1423_algos[0]))

static const X509Rfc1423Algo *x509_cipher_by_name(Str name) {
    for (Int i = 0; i < X509_NALGOS; i++)
        if (str_eq(name, str_from_cstr(x509_rfc1423_algos[i].name)))
            return &x509_rfc1423_algos[i];
    return NULL;
}

static const X509Rfc1423Algo *x509_cipher_by_key(X509PEMCipher key) {
    for (Int i = 0; i < X509_NALGOS; i++)
        if (x509_rfc1423_algos[i].cipher == key)
            return &x509_rfc1423_algos[i];
    return NULL;
}

/* deriveKey: OpenSSL's EVP_BytesToKey with MD5 and one round, into out, which
 * has room for 32 bytes. */
static bool x509_derive_key(const X509Rfc1423Algo *c, Alloc *a, Slice password,
                            const Byte *salt, Byte *out) {
    size_t n = MD5_SIZE + (size_t)password.len + 8;
    Byte *buf = (Byte *)mem_alloc_nozero(a, n, 1);
    if (buf == NULL)
        return false;
    Int dl = 0;
    for (Int i = 0; i < c->key_size; i += MD5_SIZE) {
        /* hash(digest || password || salt), the digest empty the first time. */
        if (password.len > 0)
            memcpy(buf + dl, password.p, (size_t)password.len);
        memcpy(buf + dl + password.len, salt, 8);
        Md5SumRet d = md5_sum(x509_bytes(buf, dl + password.len + 8));
        Int take = c->key_size - i < MD5_SIZE ? c->key_size - i : MD5_SIZE;
        memcpy(out + i, d.a, (size_t)take);
        memcpy(buf, d.a, MD5_SIZE);
        dl = MD5_SIZE;
    }
    memset(buf, 0, n);
    mem_free(a, buf, n, 1);
    return true;
}

static CipherBlock x509_new_block(const X509Rfc1423Algo *c, Alloc *a, Slice key,
                                  Error *err) {
    if (!c->des)
        return aes_new_cipher(a, key, err);
    if (c->key_size == 8)
        return des_new_cipher(a, key, err);
    return des_new_triple_des_cipher(a, key, err);
}

bool x509_is_encrypted_pem_block(const PemBlock *b) {
    if (b->headers == NULL)
        return false;
    Str key = BURROW_S("DEK-Info");
    Str val;
    return map_get2(b->headers, &key, &val);
}

Slice x509_decrypt_pem_block(Alloc *a, const PemBlock *b, Slice password, Error *err) {
    const Slice none = slice_nil(TYPE_BYTE);
    Str key = BURROW_S("DEK-Info");
    Str dek;
    if (b->headers == NULL || !map_get2(b->headers, &key, &dek)) {
        x509_fail(err, "x509: no DEK-Info header in block");
        return none;
    }
    bool ok;
    Str hex_iv;
    Str mode = strings_cut(dek, BURROW_S(","), &hex_iv, &ok);
    if (!ok) {
        x509_fail(err, "x509: malformed DEK-Info header");
        return none;
    }
    const X509Rfc1423Algo *ciph = x509_cipher_by_name(mode);
    if (ciph == NULL) {
        x509_fail(err, "x509: unknown encryption mode");
        return none;
    }

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *ta = arena_allocator(&ar);
    Slice data = none;
    Error e = BURROW_NO_ERROR;
    Byte k[32];
    memset(k, 0, sizeof k);

    Slice iv = hex_decode_string(ta, hex_iv, &e);
    if (BURROW_FAILED(e))
        goto out;
    if (iv.len != ciph->block_size) {
        e = errors_new(error_allocator(), BURROW_S("x509: incorrect IV size"));
        goto out;
    }
    if (!x509_derive_key(ciph, ta, password, iv.p, k)) {
        e = burrow_err_out_of_memory;
        goto out;
    }
    CipherBlock block = x509_new_block(ciph, ta, x509_bytes(k, ciph->key_size), &e);
    if (BURROW_FAILED(e))
        goto out;
    if (b->bytes.len % cipher_block_block_size(block) != 0) {
        e = errors_new(error_allocator(),
                       BURROW_S("x509: encrypted PEM data is not a multiple of the "
                                "block size"));
        goto out;
    }

    Int dlen = b->bytes.len;
    data = slice_make(a, TYPE_BYTE, dlen, dlen);
    if (dlen > 0 && data.p == NULL) {
        e = burrow_err_out_of_memory;
        goto out;
    }
    CipherBlockMode dec = cipher_new_cbc_decrypter(ta, block, iv);
    cipher_block_mode_crypt_blocks(dec, data, b->bytes);

    if (dlen == 0 || dlen % ciph->block_size != 0) {
        e = errors_new(error_allocator(), BURROW_S("x509: invalid padding"));
        goto out;
    }
    const Byte *d = data.p;
    Int last = d[dlen - 1];
    if (dlen < last || last == 0 || last > ciph->block_size) {
        e = x509_incorrect_password_error;
        goto out;
    }
    for (Int i = dlen - last; i < dlen; i++) {
        if (d[i] != last) {
            e = x509_incorrect_password_error;
            goto out;
        }
    }
    data = slice_sub(data, 0, dlen - last);
out:
    memset(k, 0, sizeof k);
    arena_free(&ar);
    BURROW_OUT(err, e);
    return BURROW_FAILED(e) ? none : data;
}

PemBlock *x509_encrypt_pem_block(Alloc *a, IoReader rand, Str block_type, Slice data,
                                 Slice password, X509PEMCipher alg, Error *err) {
    const X509Rfc1423Algo *ciph = x509_cipher_by_key(alg);
    if (ciph == NULL) {
        x509_fail(err, "x509: unknown encryption mode");
        return NULL;
    }
    if (rand.vt == NULL)
        rand = crypto_rand_reader;

    Byte iv[AES_BLOCK_SIZE];
    Slice ivs = x509_bytes(iv, ciph->block_size);
    Error e = BURROW_NO_ERROR;
    (void)io_read_full(rand, ivs, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, fmt_errorf_v("x509: cannot generate IV: %v", e));
        return NULL;
    }

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *ta = arena_allocator(&ar);
    PemBlock *out = NULL;
    Byte k[32];
    memset(k, 0, sizeof k);
    if (!x509_derive_key(ciph, ta, password, iv, k)) {
        e = burrow_err_out_of_memory;
        goto done;
    }
    CipherBlock block = x509_new_block(ciph, ta, x509_bytes(k, ciph->key_size), &e);
    if (BURROW_FAILED(e))
        goto done;
    CipherBlockMode enc = cipher_new_cbc_encrypter(ta, block, ivs);

    Int pad = ciph->block_size - data.len % ciph->block_size;
    Slice encrypted = slice_make(a, TYPE_BYTE, data.len + pad, data.len + pad);
    out = BURROW_NEW(a, PemBlock);
    Map *headers = map_make(a, TYPE_STRING, TYPE_STRING, 2);
    Str hex = hex_encode_to_string(ta, ivs);
    size_t nl = strlen(ciph->name);
    size_t dl = nl + 1 + (size_t)hex.len;
    Byte *dp = (Byte *)mem_alloc_nozero(a, dl, 1);
    if (encrypted.p == NULL || out == NULL || headers == NULL || dp == NULL ||
        (hex.len > 0 && hex.p == NULL)) {
        e = burrow_err_out_of_memory;
        out = NULL;
        goto done;
    }
    if (data.len > 0)
        memcpy(encrypted.p, data.p, (size_t)data.len);
    memset((Byte *)encrypted.p + data.len, (int)pad, (size_t)pad);
    cipher_block_mode_crypt_blocks(enc, encrypted, encrypted);

    memcpy(dp, ciph->name, nl);
    dp[nl] = ',';
    memcpy(dp + nl + 1, hex.p, (size_t)hex.len);
    Str proc_key = BURROW_S("Proc-Type"), proc_val = BURROW_S("4,ENCRYPTED");
    Str dek_key = BURROW_S("DEK-Info"), dek_val = str_from_bytes(dp, (Int)dl);
    if (!map_set(headers, &proc_key, &proc_val) ||
        !map_set(headers, &dek_key, &dek_val)) {
        e = burrow_err_out_of_memory;
        out = NULL;
        goto done;
    }
    out->type = block_type;
    out->headers = headers;
    out->bytes = encrypted;
done:
    memset(k, 0, sizeof k);
    arena_free(&ar);
    BURROW_OUT(err, e);
    return out;
}
