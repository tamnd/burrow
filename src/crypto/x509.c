/* crypto/x509: OIDs, the key formats, parsing and creating certificates, CSRs
 * and CRLs, signature checks and encrypted PEM blocks, from Go's oid.go,
 * pkcs1.go, sec1.go, pkcs8.go, parser.go, pem_decrypt.go, x509_string.go and
 * x509.go.
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
#include "burrow/crypto/sha1.h"
#include "burrow/crypto/sha256.h"
#include "burrow/crypto/x509/pkix.h"
#include "burrow/declare.h"
#include "burrow/encoding/asn1.h"
#include "burrow/encoding/hex.h"
#include "burrow/encoding/pem.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/map.h"
#include "burrow/math/big.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/net.h"
#include "burrow/net/url.h"
#include "burrow/pal.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/time.h"
#include "burrow/type.h"
#include "burrow/utf8.h"

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
    X509_DEBUG_RSACRT_OFF = 1 << 1,      /* x509rsacrt=0 */
    X509_DEBUG_NEGATIVE_SERIAL = 1 << 2, /* x509negativeserial=1 */
    X509_DEBUG_POLICY_IDS = 1 << 3,      /* x509usepolicies=0 */
    X509_DEBUG_SHA1_SKID = 1 << 4,       /* x509sha256skid=0 */
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

/* The flags a GODEBUG value sets. */
static uint32_t x509_debug_parse(const char *value) {
    uint32_t f = X509_DEBUG_KNOWN;
    if (x509_godebug_is(value, "x509rsacrt", "0"))
        f |= X509_DEBUG_RSACRT_OFF;
    if (x509_godebug_is(value, "x509negativeserial", "1"))
        f |= X509_DEBUG_NEGATIVE_SERIAL;
    if (x509_godebug_is(value, "x509usepolicies", "0"))
        f |= X509_DEBUG_POLICY_IDS;
    if (x509_godebug_is(value, "x509sha256skid", "0"))
        f |= X509_DEBUG_SHA1_SKID;
    return f;
}

static uint32_t x509_debug_load(void) {
    uint32_t f = burrow__atomic_load_relaxed_u32(&x509_debug_flags);
    if ((f & X509_DEBUG_KNOWN) != 0)
        return f;
    f = X509_DEBUG_KNOWN;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            f = x509_debug_parse(*env + 8);
            break;
        }
    }
    burrow__atomic_store_relaxed_u32(&x509_debug_flags, f);
    return f;
}

void burrow__x509_godebug_set(const char *value) {
    burrow__atomic_store_relaxed_u32(&x509_debug_flags,
                                     value != NULL ? x509_debug_parse(value) : 0);
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
    const Byte *pss;
    X509PublicKeyAlgorithm pub_key_algo;
    CryptoHash hash;
    int params;
    bool is_rsa_pss;
} X509SignatureAlgorithmDetails;

#define X509_SIG(al, nm, o, pr, ps, pub, h, ip)                                        \
    {.algo = (al),                                                                     \
     .name = (nm),                                                                     \
     .oid = (o),                                                                       \
     .oid_len = (Int)(sizeof(o) / sizeof((o)[0])),                                     \
     .pss = (ps),                                                                      \
     .pub_key_algo = (pub),                                                            \
     .hash = (h),                                                                      \
     .params = (pr),                                                                   \
     .is_rsa_pss = (ip)}

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

bool burrow__x509_signature_details(Int i, CryptoHash *hash, bool *is_rsa_pss,
                                    Slice *params) {
    if (i < 0 || i >= X509_NSIG)
        return false;
    const X509SignatureAlgorithmDetails *d = &x509_signature_algorithm_details[i];
    *hash = d->hash;
    *is_rsa_pss = d->is_rsa_pss;
    *params = slice_nil(TYPE_BYTE);
    if (d->pss != NULL)
        *params = (Slice){(void *)(uintptr_t)d->pss, (Int)sizeof x509_pss_params_sha256,
                          (Int)sizeof x509_pss_params_sha256, TYPE_BYTE};
    return true;
}

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
    if (n < 2 || (u == NULL && s == NULL) || X509_ARC(0) > 2 ||
        (X509_ARC(0) < 2 && X509_ARC(1) >= 40)) {
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
    /* Go drops this error too: the structure always marshals. */
    Error ignored = BURROW_NO_ERROR;
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(X509Pkcs1PrivateKey), &priv), &ignored);
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
    /* Go drops this error too: the structure always marshals. */
    Error ignored = BURROW_NO_ERROR;
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(X509Pkcs1PublicKey), &pub), &ignored);
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
        /* NOLINTNEXTLINE(clang-analyzer-core.NullDereference) */
        if (((const Byte *)pk.p)[0] != 0) {
            x509_fail(err, "x509: invalid private key length");
            return NULL;
        }
        pk = slice_sub(pk, 1, pk.len);
    }
    Byte buf[66];
    memset(buf, 0, sizeof buf);
    if (pk.len > 0)
        /* NOLINTNEXTLINE(clang-analyzer-unix.cstring.NullArg) */
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
    (void)a;
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
    /* Go drops this error too: the structure always marshals. */
    Error ignored = BURROW_NO_ERROR;
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(X509PkixPublicKey), &pkix), &ignored);
}

/* ------------------------------------------------------------ certificates */

X509_OID_ARCS(x509_oid_extension_subject_alt_name, 2, 5, 29, 17);
X509_OID_ARCS(x509_oid_extension_crl_number, 2, 5, 29, 20);
X509_OID_ARCS(x509_oid_extension_reason_code, 2, 5, 29, 21);
X509_OID_ARCS(x509_oid_extension_authority_key_id, 2, 5, 29, 35);
X509_OID_ARCS(x509_oid_extension_authority_info_access, 1, 3, 6, 1, 5, 5, 7, 1, 1);
X509_OID_ARCS(x509_oid_authority_info_access_ocsp, 1, 3, 6, 1, 5, 5, 7, 48, 1);
X509_OID_ARCS(x509_oid_authority_info_access_issuers, 1, 3, 6, 1, 5, 5, 7, 48, 2);
X509_OID_ARCS(x509_oid_extension_request, 1, 2, 840, 113549, 1, 9, 14);
X509_OID_ARCS(x509_oid_mgf1, 1, 2, 840, 113549, 1, 1, 8);
X509_OID_ARCS(x509_oid_sha256, 2, 16, 840, 1, 101, 3, 4, 2, 1);
X509_OID_ARCS(x509_oid_sha384, 2, 16, 840, 1, 101, 3, 4, 2, 2);
X509_OID_ARCS(x509_oid_sha512, 2, 16, 840, 1, 101, 3, 4, 2, 3);

/* The GeneralName tags of RFC 5280 section 4.2.1.6 the parser reads. */
enum {
    X509_NAME_TYPE_EMAIL = 1,
    X509_NAME_TYPE_DNS = 2,
    X509_NAME_TYPE_URI = 6,
    X509_NAME_TYPE_IP = 7,
};

/* The ASN.1 string tags cryptobyte has no names for. */
#define X509_TAG_NUMERIC_STRING ((CryptobyteAsn1Tag)18)
#define X509_TAG_BMP_STRING ((CryptobyteAsn1Tag)30)

#define X509_CTX(n) cryptobyte_asn1_tag_context_specific((CryptobyteAsn1Tag)(n))
#define X509_CTX_CONS(n)                                                               \
    cryptobyte_asn1_tag_context_specific(                                              \
        cryptobyte_asn1_tag_constructed((CryptobyteAsn1Tag)(n)))

/* A type whose descriptor says only its name, for the structs whose fields
 * have no descriptors of their own. */
#define X509_OPAQUE_TYPE(T, name)                                                      \
    const Type burrow_type_##T = {                                                     \
        {(const Byte *)(name), (Int)(sizeof(name) - 1)},                               \
        {(const Byte *)"crypto/x509", 11},                                             \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

/* A pointer type whose typedef is in the header already. */
#define X509_PTR_TYPE(Name, T)                                                         \
    const Type burrow_type_##Name = {                                                  \
        {NULL, 0},                                                                     \
        {NULL, 0},                                                                     \
        KIND_POINTER,                                                                  \
        (uint32_t)sizeof(void *),                                                      \
        (uint16_t)_Alignof(void *),                                                    \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        TYPE_OF(T),                                                                    \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    }

X509_PTR_TYPE(X509URLPtr, Url);
X509_PTR_TYPE(X509IPNetPtr, NetIPNet);

#define X509_POLICY_MAPPING_FIELDS(F, T)                                               \
    F(T, X509OID, issuer_domain_policy, IssuerDomainPolicy, "")                        \
    F(T, X509OID, subject_domain_policy, SubjectDomainPolicy, "")
BURROW_STRUCT_AS_DEFINE(X509PolicyMapping, X509_POLICY_MAPPING_FIELDS);

BURROW_SLICE_TYPE(X509Extensions, PkixExtension);
BURROW_SLICE_TYPE(X509RawValues, Asn1RawValue);

#define X509_REVOCATION_LIST_ENTRY_FIELDS(F, T)                                        \
    F(T, Bytes, raw, Raw, "")                                                          \
    F(T, X509BigIntPtr, serial_number, SerialNumber, "")                               \
    F(T, Time, revocation_time, RevocationTime, "")                                    \
    F(T, Int, reason_code, ReasonCode, "")                                             \
    F(T, X509Extensions, extensions, Extensions, "")                                   \
    F(T, X509Extensions, extra_extensions, ExtraExtensions, "")
BURROW_STRUCT_AS_DEFINE(X509RevocationListEntry, X509_REVOCATION_LIST_ENTRY_FIELDS);

X509_OPAQUE_TYPE(X509Certificate, "Certificate");
X509_OPAQUE_TYPE(X509CertificateRequest, "CertificateRequest");
X509_OPAQUE_TYPE(X509RevocationList, "RevocationList");
X509_PTR_TYPE(X509CertificatePtr, X509Certificate);

/* pssParameters */
#define X509_PSS_PARAMETERS_FIELDS(F, T)                                               \
    F(T, PkixAlgorithmIdentifier, hash, Hash, "asn1:\"explicit,tag:0\"")               \
    F(T, PkixAlgorithmIdentifier, mgf, MGF, "asn1:\"explicit,tag:1\"")                 \
    F(T, Int, salt_length, SaltLength, "asn1:\"explicit,tag:2\"")                      \
    F(T, Int, trailer_field, TrailerField, "asn1:\"optional,explicit,tag:3,default:1\"")
BURROW_STRUCT_AS(X509PssParameters, X509_PSS_PARAMETERS_FIELDS);

/* tbsCertificateRequest */
#define X509_TBS_CSR_FIELDS(F, T)                                                      \
    F(T, Asn1RawContent, raw, Raw, "")                                                 \
    F(T, Int, version, Version, "")                                                    \
    F(T, Asn1RawValue, subject, Subject, "")                                           \
    F(T, X509PublicKeyInfo, public_key, PublicKey, "")                                 \
    F(T, X509RawValues, raw_attributes, RawAttributes, "asn1:\"tag:0\"")
BURROW_STRUCT_AS(X509TbsCsr, X509_TBS_CSR_FIELDS);

/* The SignatureAlgorithm of certificateRequest. */
#define X509_CSR_ALGORITHM_FIELDS(F, T)                                                \
    F(T, Asn1RawContent, raw, Raw, "")                                                 \
    F(T, Asn1ObjectIdentifier, algorithm, Algorithm, "")                               \
    F(T, Asn1RawValue, parameters, Parameters, "asn1:\"optional\"")
BURROW_STRUCT_AS(X509CsrAlgorithm, X509_CSR_ALGORITHM_FIELDS);

/* certificateRequest */
#define X509_CSR_FIELDS(F, T)                                                          \
    F(T, Asn1RawContent, raw, Raw, "")                                                 \
    F(T, X509TbsCsr, tbs_csr, TBSCSR, "")                                              \
    F(T, X509CsrAlgorithm, signature_algorithm, SignatureAlgorithm, "")                \
    F(T, Asn1BitString, signature_value, SignatureValue, "")
BURROW_STRUCT_AS(X509Csr, X509_CSR_FIELDS);

/* pkcs10Attribute */
#define X509_PKCS10_ATTRIBUTE_FIELDS(F, T)                                             \
    F(T, Asn1ObjectIdentifier, id, Id, "")                                             \
    F(T, X509RawValues, values, Values, "asn1:\"set\"")
BURROW_STRUCT_AS(X509Pkcs10Attribute, X509_PKCS10_ATTRIBUTE_FIELDS);

static Error x509_err(const char *msg) {
    return errors_new(error_allocator(), str_from_cstr(msg));
}

/* append(*s, *v) for a slice of t, which may still be the zero Slice. */
static bool x509_push(Alloc *a, Slice *s, const Type *t, const void *v) {
    if (s->elem == NULL)
        *s = slice_nil(t);
    Slice grown = slice_append(a, *s, v, 1);
    if (grown.len != s->len + 1)
        return false;
    *s = grown;
    return true;
}

static bool x509_push_str(Alloc *a, Slice *s, Slice b) {
    Str v = str_from_bytes(b.p, b.len);
    return x509_push(a, s, TYPE_STRING, &v);
}

/* *v copied into a and held in an Any of t. */
static bool x509_box(Alloc *a, Any *out, const Type *t, const void *v, size_t size,
                     size_t align) {
    void *p = mem_alloc(a, size, align);
    if (p == NULL)
        return false;
    memcpy(p, v, size);
    *out = BURROW_ANY(t, p);
    return true;
}

#define X509_BOX(a, out, T, t, v) x509_box((a), (out), (t), (v), sizeof(T), _Alignof(T))

static Asn1ObjectIdentifier x509_details_oid(const X509SignatureAlgorithmDetails *d) {
    return (Asn1ObjectIdentifier){(void *)(uintptr_t)d->oid, d->oid_len, d->oid_len,
                                  TYPE_INT};
}

/* isIA5String, as a bool. Go ranges over the runes, and a byte that is not
 * ASCII is either part of a rune past it or a RuneError, so any such byte
 * fails. */
static bool x509_is_ia5(Str s) {
    for (Int i = 0; i < s.len; i++)
        if (s.p[i] >= 0x80)
            return false;
    return true;
}

/* isPrintable, which lets '*' and '&' through as well, since certificates use
 * them. */
static bool x509_is_printable(Byte b) {
    return ('a' <= b && b <= 'z') || ('A' <= b && b <= 'Z') || ('0' <= b && b <= '9') ||
           ('\'' <= b && b <= ')') || ('+' <= b && b <= '/') || b == ' ' || b == ':' ||
           b == '=' || b == '?' || b == '*' || b == '&';
}

/* The UTF-8 of r, which is below 0x10000, at p. The count. */
static Int x509_put_bmp_rune(Byte *p, unsigned r) {
    if (r < 0x80) {
        p[0] = (Byte)r;
        return 1;
    }
    if (r < 0x800) {
        p[0] = (Byte)(0xc0U | (r >> 6));
        p[1] = (Byte)(0x80U | (r & 0x3fU));
        return 2;
    }
    p[0] = (Byte)(0xe0U | (r >> 12));
    p[1] = (Byte)(0x80U | ((r >> 6) & 0x3fU));
    p[2] = (Byte)(0x80U | (r & 0x3fU));
    return 3;
}

/* parseASN1String */
static Error x509_parse_asn1_string(Alloc *a, CryptobyteAsn1Tag tag, Slice value,
                                    Str *out) {
    const Byte *v = value.p;
    Int n = value.len;
    *out = BURROW_STR_EMPTY;
    switch (tag) {
    case CRYPTOBYTE_ASN1_T61_STRING: {
        /* Taken as Latin-1, as most of the world does. */
        if (n == 0)
            return BURROW_NO_ERROR;
        Byte *p = mem_alloc_nozero(a, (size_t)n * 2, 1);
        if (p == NULL)
            return burrow_err_out_of_memory;
        Int l = 0;
        for (Int i = 0; i < n; i++)
            l += x509_put_bmp_rune(p + l, v[i]);
        *out = str_from_bytes(p, l);
        return BURROW_NO_ERROR;
    }
    case CRYPTOBYTE_ASN1_PRINTABLE_STRING:
        for (Int i = 0; i < n; i++)
            if (!x509_is_printable(v[i]))
                return x509_err("invalid PrintableString");
        *out = str_from_bytes(v, n);
        return BURROW_NO_ERROR;
    case CRYPTOBYTE_ASN1_UTF8_STRING:
        if (!utf8_valid(value))
            return x509_err("invalid UTF-8 string");
        *out = str_from_bytes(v, n);
        return BURROW_NO_ERROR;
    case X509_TAG_BMP_STRING: {
        /* UCS-2, read as UTF-16 without the surrogates. */
        if (n % 2 != 0)
            return x509_err("invalid BMPString");
        if (n >= 2 && v[n - 1] == 0 && v[n - 2] == 0)
            n -= 2;
        if (n == 0)
            return BURROW_NO_ERROR;
        Byte *p = mem_alloc_nozero(a, (size_t)n / 2 * 3, 1);
        if (p == NULL)
            return burrow_err_out_of_memory;
        Int l = 0;
        for (Int i = 0; i < n; i += 2) {
            unsigned point = (unsigned)v[i] << 8 | v[i + 1];
            if (point == 0xfffeU || point == 0xffffU ||
                (point >= 0xfdd0U && point <= 0xfdefU) ||
                (point >= 0xd800U && point <= 0xdfffU))
                return x509_err("invalid BMPString");
            l += x509_put_bmp_rune(p + l, point);
        }
        *out = str_from_bytes(p, l);
        return BURROW_NO_ERROR;
    }
    case CRYPTOBYTE_ASN1_IA5_STRING:
        if (!x509_is_ia5(str_from_bytes(v, n)))
            return x509_err("invalid IA5String");
        *out = str_from_bytes(v, n);
        return BURROW_NO_ERROR;
    case X509_TAG_NUMERIC_STRING:
        for (Int i = 0; i < n; i++)
            if (!(('0' <= v[i] && v[i] <= '9') || v[i] == ' '))
                return x509_err("invalid NumericString");
        *out = str_from_bytes(v, n);
        return BURROW_NO_ERROR;
    default:
        return fmt_errorf_v("unsupported string type: %v", (int)tag);
    }
}

Error burrow__x509_parse_asn1_string(Alloc *a, uint8_t tag, Slice value, Str *out) {
    return x509_parse_asn1_string(a, (CryptobyteAsn1Tag)tag, value, out);
}

/* readASN1Time */
static Error x509_read_asn1_time(Alloc *a, CryptobyteString *der, Time *t) {
    *t = (Time){0};
    if (cryptobyte_string_peek_asn1_tag(*der, CRYPTOBYTE_ASN1_UTC_TIME)) {
        if (!cryptobyte_string_read_asn1_utc_time(der, a, t))
            return x509_err("x509: malformed UTCTime");
    } else if (cryptobyte_string_peek_asn1_tag(*der,
                                               CRYPTOBYTE_ASN1_GENERALIZED_TIME)) {
        if (!cryptobyte_string_read_asn1_generalized_time(der, a, t))
            return x509_err("x509: malformed GeneralizedTime");
    } else {
        return x509_err("x509: unsupported time format");
    }
    return BURROW_NO_ERROR;
}

/* readASN1Any, for the types PkixAttributeTypeAndValue lists. */
static Error x509_read_asn1_any(Alloc *a, CryptobyteString *der, Any *out) {
    *out = (Any){NULL, NULL};
    CryptobyteString full;
    CryptobyteAsn1Tag tag = 0;
    if (!cryptobyte_string_read_any_asn1_element(der, &full, &tag))
        return x509_err("invalid ASN.1 element");
    bool ok = true;
    switch (tag) {
    case CRYPTOBYTE_ASN1_T61_STRING:
    case CRYPTOBYTE_ASN1_PRINTABLE_STRING:
    case CRYPTOBYTE_ASN1_UTF8_STRING:
    case X509_TAG_BMP_STRING:
    case CRYPTOBYTE_ASN1_IA5_STRING:
    case X509_TAG_NUMERIC_STRING: {
        CryptobyteString raw;
        if (!cryptobyte_string_read_asn1(&full, &raw, tag))
            return x509_err("invalid ASN.1 element");
        Str s;
        Error e = x509_parse_asn1_string(a, tag, raw, &s);
        if (BURROW_FAILED(e))
            return e;
        ok = X509_BOX(a, out, Str, TYPE_STRING, &s);
        break;
    }
    case CRYPTOBYTE_ASN1_INTEGER: {
        int64_t i = 0;
        if (!cryptobyte_string_read_asn1_integer_int64(&full, &i))
            return x509_err("invalid ASN.1 integer");
        ok = X509_BOX(a, out, int64_t, TYPE_INT64, &i);
        break;
    }
    case CRYPTOBYTE_ASN1_BIT_STRING: {
        Asn1BitString bs = {0};
        if (!cryptobyte_string_read_asn1_bit_string(&full, &bs))
            return x509_err("invalid ASN.1 BIT STRING");
        ok = X509_BOX(a, out, Asn1BitString, TYPE_ASN1_BIT_STRING, &bs);
        break;
    }
    case CRYPTOBYTE_ASN1_OCTET_STRING: {
        CryptobyteString s;
        if (!cryptobyte_string_read_asn1(&full, &s, CRYPTOBYTE_ASN1_OCTET_STRING))
            return x509_err("invalid ASN.1 OCTET STRING");
        ok = X509_BOX(a, out, Slice, TYPE_BYTES, &s);
        break;
    }
    case CRYPTOBYTE_ASN1_OBJECT_IDENTIFIER: {
        Asn1ObjectIdentifier oid = {0};
        if (!cryptobyte_string_read_asn1_object_identifier(&full, a, &oid))
            return x509_err("invalid ASN.1 OBJECT IDENTIFIER");
        ok = X509_BOX(a, out, Asn1ObjectIdentifier, TYPE_ASN1_OBJECT_IDENTIFIER, &oid);
        break;
    }
    case CRYPTOBYTE_ASN1_UTC_TIME:
    case CRYPTOBYTE_ASN1_GENERALIZED_TIME: {
        Time t;
        Error e = x509_read_asn1_time(a, &full, &t);
        if (BURROW_FAILED(e))
            return e;
        ok = X509_BOX(a, out, Time, TYPE_TIME, &t);
        break;
    }
    case CRYPTOBYTE_ASN1_BOOLEAN: {
        bool b = false;
        if (!cryptobyte_string_read_asn1_boolean(&full, &b))
            return x509_err("invalid ASN.1 BOOLEAN");
        ok = X509_BOX(a, out, bool, TYPE_BOOL, &b);
        break;
    }
    case CRYPTOBYTE_ASN1_NULL:
        return BURROW_NO_ERROR;
    default: {
        Asn1RawValue rv;
        memset(&rv, 0, sizeof rv);
        rv.cls = (Int)(tag >> 6);
        rv.is_compound = (tag & 0x20) == 0x20;
        rv.tag = (Int)(tag & 0x1f);
        rv.full_bytes = full;
        if (!cryptobyte_string_read_any_asn1(&full, &rv.bytes, &tag))
            return x509_err("invalid ASN.1 element");
        ok = X509_BOX(a, out, Asn1RawValue, TYPE_ASN1_RAW_VALUE, &rv);
        break;
    }
    }
    return ok ? BURROW_NO_ERROR : burrow_err_out_of_memory;
}

/* parseName */
static Error x509_parse_name(Alloc *a, CryptobyteString raw, PkixRDNSequence *out) {
    *out = slice_nil(TYPE_PKIX_RELATIVE_DISTINGUISHED_NAME_SET);
    if (!cryptobyte_string_read_asn1(&raw, &raw, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: invalid RDNSequence");
    while (!cryptobyte_string_empty(raw)) {
        PkixRelativeDistinguishedNameSET rdn =
            slice_nil(TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE);
        CryptobyteString set;
        if (!cryptobyte_string_read_asn1(&raw, &set, CRYPTOBYTE_ASN1_SET))
            return x509_err("x509: invalid RDNSequence");
        while (!cryptobyte_string_empty(set)) {
            CryptobyteString atv;
            if (!cryptobyte_string_read_asn1(&set, &atv, CRYPTOBYTE_ASN1_SEQUENCE))
                return x509_err("x509: invalid RDNSequence: invalid attribute");
            PkixAttributeTypeAndValue attr;
            memset(&attr, 0, sizeof attr);
            if (!cryptobyte_string_read_asn1_object_identifier(&atv, a, &attr.type))
                return x509_err("x509: invalid RDNSequence: invalid attribute type");
            Error e = x509_read_asn1_any(a, &atv, &attr.value);
            if (BURROW_FAILED(e)) {
                if (errors_is(e, burrow_err_out_of_memory))
                    return e;
                return fmt_errorf_v(
                    "x509: invalid RDNSequence: invalid attribute value: %s",
                    error_text(e));
            }
            if (!x509_push(a, &rdn, TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE, &attr))
                return burrow_err_out_of_memory;
        }
        if (!x509_push(a, out, TYPE_PKIX_RELATIVE_DISTINGUISHED_NAME_SET, &rdn))
            return burrow_err_out_of_memory;
    }
    return BURROW_NO_ERROR;
}

/* parseName and FillFromRDNSequence. */
static Error x509_parse_name_into(Alloc *a, CryptobyteString raw, PkixName *name) {
    PkixRDNSequence rdns;
    Error e = x509_parse_name(a, raw, &rdns);
    if (BURROW_FAILED(e))
        return e;
    if (!pkix_name_fill_from_rdn_sequence(name, a, &rdns))
        return burrow_err_out_of_memory;
    return BURROW_NO_ERROR;
}

/* parseAI */
static Error x509_parse_ai(Alloc *a, CryptobyteString der,
                           PkixAlgorithmIdentifier *ai) {
    memset(ai, 0, sizeof *ai);
    if (!cryptobyte_string_read_asn1_object_identifier(&der, a, &ai->algorithm))
        return x509_err("x509: malformed OID");
    if (cryptobyte_string_empty(der))
        return BURROW_NO_ERROR;
    CryptobyteString params;
    CryptobyteAsn1Tag tag = 0;
    if (!cryptobyte_string_read_any_asn1_element(&der, &params, &tag))
        return x509_err("x509: malformed parameters");
    ai->parameters.tag = (Int)tag;
    ai->parameters.full_bytes = params;
    return BURROW_NO_ERROR;
}

/* parseExtension */
static Error x509_parse_extension(Alloc *a, CryptobyteString der, PkixExtension *ext) {
    memset(ext, 0, sizeof *ext);
    if (!cryptobyte_string_read_asn1_object_identifier(&der, a, &ext->id))
        return x509_err("x509: malformed extension OID field");
    if (cryptobyte_string_peek_asn1_tag(der, CRYPTOBYTE_ASN1_BOOLEAN)) {
        if (!cryptobyte_string_read_asn1_boolean(&der, &ext->critical))
            return x509_err("x509: malformed extension critical field");
    }
    CryptobyteString val;
    if (!cryptobyte_string_read_asn1(&der, &val, CRYPTOBYTE_ASN1_OCTET_STRING))
        return x509_err("x509: malformed extension value field");
    ext->value = val;
    return BURROW_NO_ERROR;
}

/* parseKeyUsageExtension */
static Error x509_parse_key_usage(CryptobyteString der, X509KeyUsage *out) {
    Asn1BitString bits = {0};
    if (!cryptobyte_string_read_asn1_bit_string(&der, &bits))
        return x509_err("x509: invalid key usage");
    Int usage = 0;
    for (Int i = 0; i < 9; i++)
        if (asn1_bit_string_at(bits, i) != 0)
            usage |= (Int)1 << i;
    *out = usage;
    return BURROW_NO_ERROR;
}

/* parseBasicConstraintsExtension */
static Error x509_parse_basic_constraints(CryptobyteString der, bool *is_ca,
                                          Int *max_path_len) {
    *is_ca = false;
    if (!cryptobyte_string_read_asn1(&der, &der, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: invalid basic constraints");
    if (cryptobyte_string_peek_asn1_tag(der, CRYPTOBYTE_ASN1_BOOLEAN)) {
        if (!cryptobyte_string_read_asn1_boolean(&der, is_ca))
            return x509_err("x509: invalid basic constraints");
    }
    *max_path_len = -1;
    if (cryptobyte_string_peek_asn1_tag(der, CRYPTOBYTE_ASN1_INTEGER)) {
        uint64_t mpl = 0;
        if (!cryptobyte_string_read_asn1_integer_uint64(&der, &mpl) ||
            mpl > (uint64_t)BURROW_INT_MAX)
            return x509_err("x509: invalid basic constraints");
        *max_path_len = (Int)mpl;
    }
    return BURROW_NO_ERROR;
}

/* forEachSAN: fn with the tag and contents of each GeneralName in der. */
typedef Error (*X509SANFunc)(void *ctx, int tag, Slice data);

static Error x509_for_each_san(CryptobyteString der, X509SANFunc fn, void *ctx) {
    if (!cryptobyte_string_read_asn1(&der, &der, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: invalid subject alternative names");
    while (!cryptobyte_string_empty(der)) {
        CryptobyteString san;
        CryptobyteAsn1Tag tag = 0;
        if (!cryptobyte_string_read_any_asn1(&der, &san, &tag))
            return x509_err("x509: invalid subject alternative name");
        Error e = fn(ctx, (int)(tag ^ 0x80), san);
        if (BURROW_FAILED(e))
            return e;
    }
    return BURROW_NO_ERROR;
}

/* The names a subjectAltName extension holds. */
typedef struct X509SANs {
    Alloc *a;
    Slice dns_names, email_addresses, ip_addresses, uris;
} X509SANs;

/* domainNameValid, the checks domainToReverseLabels makes without the
 * allocation. Go leaves out some RFC 1034 checks for now since turning them on
 * broke certificates people use, and so does this. */
static bool x509_domain_name_valid(Str s, bool constraint) {
    if (s.len == 0)
        return true;
    /* No trailing period. */
    if (s.p[s.len - 1] == '.')
        return false;
    Int last_dot = -1;
    const Byte *p = s.p;
    Int n = s.len;
    if (constraint && p[0] == '.') {
        p++;
        n--;
    }
    for (Int i = 0; i <= n; i++) {
        if (i < n && (p[i] < 33 || p[i] > 126))
            return false;
        if (i == n || p[i] == '.') {
            Int label_len = i;
            if (last_dot >= 0)
                label_len -= last_dot + 1;
            if (label_len == 0)
                return false;
            last_dot = i;
        }
    }
    return true;
}

bool burrow__x509_domain_name_valid(Str s, bool constraint) {
    return x509_domain_name_valid(s, constraint);
}

static Error x509_san_one(void *ctx, int tag, Slice data) {
    X509SANs *out = ctx;
    Alloc *a = out->a;
    Str s = str_from_bytes(data.p, data.len);
    switch (tag) {
    case X509_NAME_TYPE_EMAIL:
        if (!x509_is_ia5(s))
            return x509_err("x509: SAN rfc822Name is malformed");
        if (!x509_push(a, &out->email_addresses, TYPE_STRING, &s))
            return burrow_err_out_of_memory;
        break;
    case X509_NAME_TYPE_DNS:
        if (!x509_is_ia5(s))
            return x509_err("x509: SAN dNSName is malformed");
        if (!x509_push(a, &out->dns_names, TYPE_STRING, &s))
            return burrow_err_out_of_memory;
        break;
    case X509_NAME_TYPE_URI: {
        if (!x509_is_ia5(s))
            return x509_err("x509: SAN uniformResourceIdentifier is malformed");
        Error e = BURROW_NO_ERROR;
        Url *uri = url_parse(a, s, &e);
        if (BURROW_FAILED(e))
            return fmt_errorf_v("x509: cannot parse URI %q: %s", s, error_text(e));
        if (uri->host.len > 0 && !x509_domain_name_valid(uri->host, false))
            return fmt_errorf_v("x509: cannot parse URI %q: invalid domain", s);
        if (!x509_push(a, &out->uris, TYPE_X509_URL_PTR, &uri))
            return burrow_err_out_of_memory;
        break;
    }
    case X509_NAME_TYPE_IP:
        if (data.len == NET_IPV6_LEN) {
            if (net_ip_to4(data).len != 0)
                return x509_err(
                    "x509: SAN iPAddress contains IPv4-mapped IPv6 address");
        } else if (data.len != NET_IPV4_LEN) {
            return fmt_errorf_v("x509: cannot parse IP address of length %d", data.len);
        }
        if (!x509_push(a, &out->ip_addresses, TYPE_NET_IP, &data))
            return burrow_err_out_of_memory;
        break;
    default:
        break;
    }
    return BURROW_NO_ERROR;
}

/* parseSANExtension */
static Error x509_parse_san(Alloc *a, CryptobyteString der, X509SANs *out) {
    memset(out, 0, sizeof *out);
    out->a = a;
    return x509_for_each_san(der, x509_san_one, out);
}

/* parseAuthorityKeyIdentifier */
static Error x509_parse_authority_key_id(const PkixExtension *e, Slice *out) {
    *out = slice_nil(TYPE_BYTE);
    if (e->critical)
        return x509_err("x509: authority key identifier incorrectly marked critical");
    CryptobyteString val = e->value, akid;
    if (!cryptobyte_string_read_asn1(&val, &akid, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: invalid authority key identifier");
    if (cryptobyte_string_peek_asn1_tag(akid, X509_CTX(0))) {
        if (!cryptobyte_string_read_asn1(&akid, &akid, X509_CTX(0)))
            return x509_err("x509: invalid authority key identifier");
        *out = akid;
    }
    return BURROW_NO_ERROR;
}

/* extKeyUsageFromOID */
static bool x509_ext_key_usage_from_oid(Asn1ObjectIdentifier oid,
                                        X509ExtKeyUsage *eku) {
    for (Int i = 0; i < X509_NEKU; i++) {
        const X509ExtKeyUsageOID *e = &x509_ext_key_usage_oids[i];
        Asn1ObjectIdentifier want = {(void *)(uintptr_t)e->oid, e->len, e->len,
                                     TYPE_INT};
        if (x509_oid_is(oid, want)) {
            *eku = (X509ExtKeyUsage)i;
            return true;
        }
    }
    return false;
}

/* parseExtKeyUsageExtension */
static Error x509_parse_ext_key_usage(Alloc *a, CryptobyteString der, Slice *ekus,
                                      Slice *unknown) {
    if (!cryptobyte_string_read_asn1(&der, &der, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: invalid extended key usages");
    while (!cryptobyte_string_empty(der)) {
        Asn1ObjectIdentifier oid = {0};
        if (!cryptobyte_string_read_asn1_object_identifier(&der, a, &oid))
            return x509_err("x509: invalid extended key usages");
        X509ExtKeyUsage eku = 0;
        bool ok = x509_ext_key_usage_from_oid(oid, &eku)
                      ? x509_push(a, ekus, TYPE_INT, &eku)
                      : x509_push(a, unknown, TYPE_ASN1_OBJECT_IDENTIFIER, &oid);
        if (!ok)
            return burrow_err_out_of_memory;
    }
    return BURROW_NO_ERROR;
}

/* parseCertificatePoliciesExtension */
static Error x509_parse_certificate_policies(Alloc *a, CryptobyteString der,
                                             Slice *oids) {
    if (!cryptobyte_string_read_asn1(&der, &der, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: invalid certificate policies");
    while (!cryptobyte_string_empty(der)) {
        CryptobyteString cp, oid_bytes;
        if (!cryptobyte_string_read_asn1(&der, &cp, CRYPTOBYTE_ASN1_SEQUENCE) ||
            !cryptobyte_string_read_asn1(&cp, &oid_bytes,
                                         CRYPTOBYTE_ASN1_OBJECT_IDENTIFIER))
            return x509_err("x509: invalid certificate policies");
        const X509OID *seen = oids->p;
        for (Int i = 0; i < oids->len; i++)
            if (bytes_equal(seen[i].der, oid_bytes))
                return x509_err("x509: invalid certificate policies");
        if (!x509_oid_der_valid(oid_bytes))
            return x509_err("x509: invalid certificate policies");
        X509OID oid = {oid_bytes};
        if (!x509_push(a, oids, TYPE_OF(X509OID), &oid))
            return burrow_err_out_of_memory;
    }
    return BURROW_NO_ERROR;
}

/* OID.toASN1OID, false for an arc past 31 bits. */
static bool x509_oid_to_asn1_oid(X509OID oid, Alloc *a, Asn1ObjectIdentifier *out) {
    Slice arcs = slice_make(a, TYPE_INT, 0, oid.der.len + 1);
    if (arcs.p == NULL)
        return false;
    Int *o = arcs.p;
    Int n = 0;
    Int val = 0;
    const Int max_safe_shift = ((Int)1 << (31 - 7)) - 1;
    const Byte *d = oid.der.p;
    for (Int i = 0; i < oid.der.len; i++) {
        if (val > max_safe_shift)
            return false;
        val <<= 7;
        val |= (Int)(d[i] & 0x7f);
        if ((d[i] & 0x80) == 0) {
            if (n == 0) {
                if (val < 80) {
                    o[n++] = val / 40;
                    o[n++] = val % 40;
                } else {
                    o[n++] = 2;
                    o[n++] = val - 80;
                }
            } else {
                o[n++] = val;
            }
            val = 0;
        }
    }
    arcs.len = n;
    *out = arcs;
    return true;
}

/* isValidIPMask: some 1 bits followed by 0 bits only. */
static bool x509_is_valid_ip_mask(Slice mask) {
    bool seen_zero = false;
    const Byte *m = mask.p;
    for (Int i = 0; i < mask.len; i++) {
        if (seen_zero) {
            if (m[i] != 0)
                return false;
            continue;
        }
        /* NOLINTNEXTLINE(clang-analyzer-core.NullDereference) */
        switch (m[i]) {
        case 0x00:
        case 0x80:
        case 0xc0:
        case 0xe0:
        case 0xf0:
        case 0xf8:
        case 0xfc:
        case 0xfe:
            seen_zero = true;
            break;
        case 0xff:
            break;
        default:
            return false;
        }
    }
    return true;
}

/* parseRFC2821Mailbox: whether in is an RFC 2821 mailbox, with its local part
 * unquoted into a and its domain pointing into in. local and domain may be
 * NULL when only the answer is wanted. */
static bool x509_parse_rfc2821_mailbox(Alloc *a, Str in, Str *local, Str *domain) {
    if (in.len == 0)
        return false;
    Byte *buf = NULL;
    if (local != NULL) {
        buf = mem_alloc_nozero(a, (size_t)in.len, 1);
        if (buf == NULL)
            return false;
    }
    const Byte *p = in.p;
    Int n = in.len;
    Int lp = 0;
    Byte first = 0, last = 0;
    bool two_dots = false;
#define X509_LP_ADD(c)                                                                 \
    do {                                                                               \
        Byte c_ = (c);                                                                 \
        if (lp == 0)                                                                   \
            first = c_;                                                                \
        else if (last == '.' && c_ == '.')                                             \
            two_dots = true;                                                           \
        if (buf != NULL)                                                               \
            buf[lp] = c_;                                                              \
        lp++;                                                                          \
        last = c_;                                                                     \
    } while (0)

    if (p[0] == '"') {
        /* Quoted-string = DQUOTE *qcontent DQUOTE, without the obsolete
         * syntax of RFC 2822. */
        p++;
        n--;
        for (;;) {
            if (n == 0)
                return false;
            Byte c = *p++;
            n--;
            if (c == '"')
                break;
            if (c == '\\') {
                /* quoted-pair */
                if (n == 0)
                    return false;
                if (p[0] == 11 || p[0] == 12 || (1 <= p[0] && p[0] <= 9) ||
                    (14 <= p[0] && p[0] <= 127)) {
                    X509_LP_ADD(p[0]);
                    p++;
                    n--;
                } else {
                    return false;
                }
            } else if (c == 11 || c == 12 ||
                       /* Space is not in the grammar, but RFC 3696 has an
                        * example that uses it, so it is let through. */
                       c == 32 || c == 33 || c == 127 || (1 <= c && c <= 8) ||
                       (14 <= c && c <= 31) || (35 <= c && c <= 91) ||
                       (93 <= c && c <= 126)) {
                /* qtext */
                X509_LP_ADD(c);
            } else {
                return false;
            }
        }
    } else {
        /* Atom ("." Atom)* */
        while (n > 0) {
            /* atext from RFC 2822 section 3.2.4 */
            Byte c = p[0];
            if (c == '\\') {
                /* RFC 3696 has escaped characters outside quotes in its
                 * examples, and they are let through. */
                p++;
                n--;
                if (n == 0)
                    return false;
            } else if (!(('0' <= c && c <= '9') || ('a' <= c && c <= 'z') ||
                         ('A' <= c && c <= 'Z') || c == '!' || c == '#' || c == '$' ||
                         c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' ||
                         c == '-' || c == '/' || c == '=' || c == '?' || c == '^' ||
                         c == '_' || c == '`' || c == '{' || c == '|' || c == '}' ||
                         c == '~' || c == '.')) {
                break;
            }
            X509_LP_ADD(p[0]);
            p++;
            n--;
        }
        if (lp == 0)
            return false;
        /* RFC 3696 section 3: a period may not start or end the local part,
         * and two may not be next to each other. */
        if (first == '.' || last == '.' || two_dots)
            return false;
    }
#undef X509_LP_ADD

    if (n == 0 || p[0] != '@')
        return false;
    p++;
    n--;

    /* Anything after the @ is taken as the domain, since the format the RFC
     * gives is not what people use. */
    Str d = str_from_bytes(p, n);
    if (!x509_domain_name_valid(d, false))
        return false;
    for (Int i = 0; i < n; i++)
        if (p[i] == '@')
            return false;
    if (local != NULL)
        *local = str_from_bytes(buf, lp);
    if (domain != NULL)
        *domain = d;
    return true;
}

bool burrow__x509_parse_rfc2821_mailbox(Alloc *a, Str in, Str *local, Str *domain) {
    return x509_parse_rfc2821_mailbox(a, in, local, domain);
}

/* The four lists a GeneralSubtrees of a NameConstraints fills. */
typedef struct X509Subtrees {
    Slice *dns, *ips, *emails, *uris;
} X509Subtrees;

static Error x509_constraint_ia5_error(Str s) {
    return fmt_errorf_v(
        "x509: invalid constraint value: x509: %q cannot be encoded as an IA5String",
        s);
}

/* getValues in parseNameConstraintsExtension. */
static Error x509_constraint_values(Alloc *a, CryptobyteString subtrees,
                                    const X509Subtrees *out, bool *unhandled) {
    const CryptobyteAsn1Tag dns_tag = X509_CTX(2);
    const CryptobyteAsn1Tag email_tag = X509_CTX(1);
    const CryptobyteAsn1Tag ip_tag = X509_CTX(7);
    const CryptobyteAsn1Tag uri_tag = X509_CTX(6);
    while (!cryptobyte_string_empty(subtrees)) {
        CryptobyteString seq, value;
        CryptobyteAsn1Tag tag = 0;
        if (!cryptobyte_string_read_asn1(&subtrees, &seq, CRYPTOBYTE_ASN1_SEQUENCE) ||
            !cryptobyte_string_read_any_asn1(&seq, &value, &tag))
            return x509_err("x509: invalid NameConstraints extension");
        Str s = str_from_bytes(value.p, value.len);
        if (tag == dns_tag) {
            if (!x509_is_ia5(s))
                return x509_constraint_ia5_error(s);
            if (!x509_domain_name_valid(s, true))
                return fmt_errorf_v("x509: failed to parse dnsName constraint %q", s);
            if (!x509_push(a, out->dns, TYPE_STRING, &s))
                return burrow_err_out_of_memory;
        } else if (tag == ip_tag) {
            Int half;
            if (value.len == 8)
                half = 4;
            else if (value.len == 32)
                half = 16;
            else
                return fmt_errorf_v("x509: IP constraint contained value of length %d",
                                    value.len);
            Slice ip = slice_sub(value, 0, half);
            Slice mask = slice_sub(value, half, value.len);
            if (!x509_is_valid_ip_mask(mask)) {
                Str hex = hex_encode_to_string(error_allocator(), mask);
                return fmt_errorf_v("x509: IP constraint contained invalid mask %s",
                                    hex);
            }
            if (ip.len == NET_IPV6_LEN && net_ip_to4(ip).len != 0)
                return x509_err(
                    "x509: IP constraint contained IPv4-mapped IPv6 address");
            NetIPNet *ipnet = BURROW_NEW(a, NetIPNet);
            if (ipnet == NULL)
                return burrow_err_out_of_memory;
            ipnet->ip = ip;
            ipnet->mask = mask;
            if (!x509_push(a, out->ips, TYPE_X509_IP_NET_PTR, &ipnet))
                return burrow_err_out_of_memory;
        } else if (tag == email_tag) {
            if (!x509_is_ia5(s))
                return x509_constraint_ia5_error(s);
            /* With an @ in it, the constraint is one exact mailbox. */
            bool has_at = memchr(s.p, '@', (size_t)s.len) != NULL;
            bool ok = has_at ? x509_parse_rfc2821_mailbox(a, s, NULL, NULL)
                             : x509_domain_name_valid(s, true);
            if (!ok)
                return fmt_errorf_v("x509: failed to parse rfc822Name constraint %q",
                                    s);
            if (!x509_push(a, out->emails, TYPE_STRING, &s))
                return burrow_err_out_of_memory;
        } else if (tag == uri_tag) {
            if (!x509_is_ia5(s))
                return x509_constraint_ia5_error(s);
            if (net_parse_ip(a, s).len != 0)
                return fmt_errorf_v(
                    "x509: failed to parse URI constraint %q: cannot be IP address", s);
            if (!x509_domain_name_valid(s, true))
                return fmt_errorf_v("x509: failed to parse URI constraint %q", s);
            if (!x509_push(a, out->uris, TYPE_STRING, &s))
                return burrow_err_out_of_memory;
        } else {
            *unhandled = true;
        }
    }
    return BURROW_NO_ERROR;
}

/* parseNameConstraintsExtension */
static Error x509_parse_name_constraints(Alloc *a, X509Certificate *out,
                                         const PkixExtension *e, bool *unhandled) {
    *unhandled = false;
    CryptobyteString outer = e->value, toplevel, permitted = {0}, excluded = {0};
    bool have_permitted = false, have_excluded = false;
    if (!cryptobyte_string_read_asn1(&outer, &toplevel, CRYPTOBYTE_ASN1_SEQUENCE) ||
        !cryptobyte_string_empty(outer) ||
        !cryptobyte_string_read_optional_asn1(&toplevel, &permitted, &have_permitted,
                                              X509_CTX_CONS(0)) ||
        !cryptobyte_string_read_optional_asn1(&toplevel, &excluded, &have_excluded,
                                              X509_CTX_CONS(1)) ||
        !cryptobyte_string_empty(toplevel))
        return x509_err("x509: invalid NameConstraints extension");
    if ((!have_permitted && !have_excluded) ||
        (permitted.len == 0 && excluded.len == 0))
        /* RFC 5280 section 4.2.1.10: either the permittedSubtrees field or
         * the excludedSubtrees MUST be present. */
        return x509_err("x509: empty name constraints extension");

    /* Go assigns each list whole, so a second NameConstraints replaces what
     * the first one gave. */
    out->permitted_dns_domains = slice_nil(TYPE_STRING);
    out->permitted_ip_ranges = slice_nil(TYPE_X509_IP_NET_PTR);
    out->permitted_email_addresses = slice_nil(TYPE_STRING);
    out->permitted_uri_domains = slice_nil(TYPE_STRING);
    out->excluded_dns_domains = slice_nil(TYPE_STRING);
    out->excluded_ip_ranges = slice_nil(TYPE_X509_IP_NET_PTR);
    out->excluded_email_addresses = slice_nil(TYPE_STRING);
    out->excluded_uri_domains = slice_nil(TYPE_STRING);
    const X509Subtrees p = {&out->permitted_dns_domains, &out->permitted_ip_ranges,
                            &out->permitted_email_addresses,
                            &out->permitted_uri_domains};
    const X509Subtrees x = {&out->excluded_dns_domains, &out->excluded_ip_ranges,
                            &out->excluded_email_addresses, &out->excluded_uri_domains};
    Error err = x509_constraint_values(a, permitted, &p, unhandled);
    if (BURROW_FAILED(err))
        return err;
    err = x509_constraint_values(a, excluded, &x, unhandled);
    if (BURROW_FAILED(err))
        return err;
    out->permitted_dns_domains_critical = e->critical;
    return BURROW_NO_ERROR;
}

/* The CRL distribution points extension, case 31 of processExtensions. */
static Error x509_parse_crl_distribution_points(Alloc *a, CryptobyteString val,
                                                Slice *out) {
    if (!cryptobyte_string_read_asn1(&val, &val, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: invalid CRL distribution points");
    while (!cryptobyte_string_empty(val)) {
        CryptobyteString dp, name = {0};
        bool present = false;
        if (!cryptobyte_string_read_asn1(&val, &dp, CRYPTOBYTE_ASN1_SEQUENCE))
            return x509_err("x509: invalid CRL distribution point");
        if (!cryptobyte_string_read_optional_asn1(&dp, &name, &present,
                                                  X509_CTX_CONS(0)))
            return x509_err("x509: invalid CRL distribution point");
        if (!present)
            continue;
        if (!cryptobyte_string_read_asn1(&name, &name, X509_CTX_CONS(0)))
            return x509_err("x509: invalid CRL distribution point");
        while (!cryptobyte_string_empty(name)) {
            if (!cryptobyte_string_peek_asn1_tag(name, X509_CTX(6)))
                break;
            CryptobyteString uri;
            if (!cryptobyte_string_read_asn1(&name, &uri, X509_CTX(6)))
                return x509_err("x509: invalid CRL distribution point");
            if (!x509_push_str(a, out, uri))
                return burrow_err_out_of_memory;
        }
    }
    return BURROW_NO_ERROR;
}

/* The policy constraints extension, case 36 of processExtensions. */
static Error x509_parse_policy_constraints(CryptobyteString val, X509Certificate *out) {
    if (!cryptobyte_string_read_asn1(&val, &val, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: invalid policy constraints extension");
    if (cryptobyte_string_peek_asn1_tag(val, X509_CTX(0))) {
        int64_t v = 0;
        if (!cryptobyte_string_read_asn1_int64_with_tag(&val, &v, X509_CTX(0)))
            return x509_err("x509: invalid policy constraints extension");
        out->require_explicit_policy = (Int)v;
        if ((int64_t)out->require_explicit_policy != v)
            return x509_err(
                "x509: policy constraints requireExplicitPolicy field overflows int");
        out->require_explicit_policy_zero = out->require_explicit_policy == 0;
    }
    if (cryptobyte_string_peek_asn1_tag(val, X509_CTX(1))) {
        int64_t v = 0;
        if (!cryptobyte_string_read_asn1_int64_with_tag(&val, &v, X509_CTX(1)))
            return x509_err("x509: invalid policy constraints extension");
        out->inhibit_policy_mapping = (Int)v;
        if ((int64_t)out->inhibit_policy_mapping != v)
            return x509_err(
                "x509: policy constraints inhibitPolicyMapping field overflows int");
        out->inhibit_policy_mapping_zero = out->inhibit_policy_mapping == 0;
    }
    return BURROW_NO_ERROR;
}

/* The policy mappings extension, case 33 of processExtensions. */
static Error x509_parse_policy_mappings(Alloc *a, CryptobyteString val, Slice *out) {
    if (!cryptobyte_string_read_asn1(&val, &val, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: invalid policy mappings extension");
    while (!cryptobyte_string_empty(val)) {
        CryptobyteString s, issuer, subject;
        if (!cryptobyte_string_read_asn1(&val, &s, CRYPTOBYTE_ASN1_SEQUENCE) ||
            !cryptobyte_string_read_asn1(&s, &issuer,
                                         CRYPTOBYTE_ASN1_OBJECT_IDENTIFIER) ||
            !cryptobyte_string_read_asn1(&s, &subject,
                                         CRYPTOBYTE_ASN1_OBJECT_IDENTIFIER))
            return x509_err("x509: invalid policy mappings extension");
        X509PolicyMapping m = {{issuer}, {subject}};
        if (!x509_push(a, out, TYPE_X509_POLICY_MAPPING, &m))
            return burrow_err_out_of_memory;
    }
    return BURROW_NO_ERROR;
}

/* The authority information access extension of RFC 5280 section 4.2.2.1. */
static Error x509_parse_authority_info_access(Alloc *a, const PkixExtension *e,
                                              X509Certificate *out) {
    if (e->critical)
        return x509_err("x509: authority info access incorrectly marked critical");
    CryptobyteString val = e->value;
    if (!cryptobyte_string_read_asn1(&val, &val, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: invalid authority info access");
    while (!cryptobyte_string_empty(val)) {
        CryptobyteString aia;
        if (!cryptobyte_string_read_asn1(&val, &aia, CRYPTOBYTE_ASN1_SEQUENCE))
            return x509_err("x509: invalid authority info access");
        Asn1ObjectIdentifier method = {0};
        if (!cryptobyte_string_read_asn1_object_identifier(&aia, a, &method))
            return x509_err("x509: invalid authority info access");
        if (!cryptobyte_string_peek_asn1_tag(aia, X509_CTX(6)))
            continue;
        if (!cryptobyte_string_read_asn1(&aia, &aia, X509_CTX(6)))
            return x509_err("x509: invalid authority info access");
        bool ok = true;
        if (x509_oid_is(method, X509_OID(x509_oid_authority_info_access_ocsp)))
            ok = x509_push_str(a, &out->ocsp_server, aia);
        else if (x509_oid_is(method, X509_OID(x509_oid_authority_info_access_issuers)))
            ok = x509_push_str(a, &out->issuing_certificate_url, aia);
        if (!ok)
            return burrow_err_out_of_memory;
    }
    return BURROW_NO_ERROR;
}

/* One extension with an id of 2.5.29.n, a switch case of processExtensions. */
static Error x509_process_id_ce(Alloc *a, X509Certificate *out, const PkixExtension *e,
                                Int n, bool *unhandled) {
    Error err = BURROW_NO_ERROR;
    switch (n) {
    case 15:
        return x509_parse_key_usage(e->value, &out->key_usage);
    case 19:
        err = x509_parse_basic_constraints(e->value, &out->is_ca, &out->max_path_len);
        if (BURROW_FAILED(err))
            return err;
        out->basic_constraints_valid = true;
        out->max_path_len_zero = out->max_path_len == 0;
        return BURROW_NO_ERROR;
    case 17: {
        X509SANs sans;
        err = x509_parse_san(a, e->value, &sans);
        if (BURROW_FAILED(err))
            return err;
        out->dns_names = sans.dns_names;
        out->email_addresses = sans.email_addresses;
        out->ip_addresses = sans.ip_addresses;
        out->uris = sans.uris;
        /* An empty one gets the critical check below. */
        if (sans.dns_names.len == 0 && sans.email_addresses.len == 0 &&
            sans.ip_addresses.len == 0 && sans.uris.len == 0)
            *unhandled = true;
        return BURROW_NO_ERROR;
    }
    case 30:
        return x509_parse_name_constraints(a, out, e, unhandled);
    case 31:
        return x509_parse_crl_distribution_points(a, e->value,
                                                  &out->crl_distribution_points);
    case 35:
        return x509_parse_authority_key_id(e, &out->authority_key_id);
    case 36:
        return x509_parse_policy_constraints(e->value, out);
    case 37:
        out->ext_key_usage = slice_nil(TYPE_INT);
        out->unknown_ext_key_usage = slice_nil(TYPE_ASN1_OBJECT_IDENTIFIER);
        return x509_parse_ext_key_usage(a, e->value, &out->ext_key_usage,
                                        &out->unknown_ext_key_usage);
    case 14: {
        /* RFC 5280 section 4.2.1.2. Conforming CAs mark it non-critical. */
        if (e->critical)
            return x509_err("x509: subject key identifier incorrectly marked critical");
        CryptobyteString val = e->value, skid;
        if (!cryptobyte_string_read_asn1(&val, &skid, CRYPTOBYTE_ASN1_OCTET_STRING))
            return x509_err("x509: invalid subject key identifier");
        out->subject_key_id = skid;
        return BURROW_NO_ERROR;
    }
    case 32: {
        out->policies = slice_nil(TYPE_OF(X509OID));
        err = x509_parse_certificate_policies(a, e->value, &out->policies);
        if (BURROW_FAILED(err))
            return err;
        Slice ids = slice_make(a, TYPE_ASN1_OBJECT_IDENTIFIER, 0, out->policies.len);
        if (ids.p == NULL && out->policies.len > 0)
            return burrow_err_out_of_memory;
        const X509OID *oids = out->policies.p;
        for (Int i = 0; i < out->policies.len; i++) {
            Asn1ObjectIdentifier oid;
            if (x509_oid_to_asn1_oid(oids[i], a, &oid) &&
                !x509_push(a, &ids, TYPE_ASN1_OBJECT_IDENTIFIER, &oid))
                return burrow_err_out_of_memory;
        }
        out->policy_identifiers = ids;
        return BURROW_NO_ERROR;
    }
    case 33:
        return x509_parse_policy_mappings(a, e->value, &out->policy_mappings);
    case 54: {
        CryptobyteString val = e->value;
        if (!cryptobyte_string_read_asn1_integer_int(&val, &out->inhibit_any_policy))
            return x509_err("x509: invalid inhibit any policy extension");
        out->inhibit_any_policy_zero = out->inhibit_any_policy == 0;
        return BURROW_NO_ERROR;
    }
    default:
        /* Unknown extensions are recorded when critical. */
        *unhandled = true;
        return BURROW_NO_ERROR;
    }
}

/* processExtensions */
static Error x509_process_extensions(Alloc *a, X509Certificate *out) {
    const PkixExtension *exts = out->extensions.p;
    for (Int i = 0; i < out->extensions.len; i++) {
        const PkixExtension *e = &exts[i];
        const Int *id = e->id.p;
        bool unhandled = false;
        Error err = BURROW_NO_ERROR;
        if (e->id.len == 4 && id[0] == 2 && id[1] == 5 && id[2] == 29)
            err = x509_process_id_ce(a, out, e, id[3], &unhandled);
        else if (x509_oid_is(e->id, X509_OID(x509_oid_extension_authority_info_access)))
            err = x509_parse_authority_info_access(a, e, out);
        else
            unhandled = true;
        if (BURROW_FAILED(err))
            return err;
        if (e->critical && unhandled &&
            !x509_push(a, &out->unhandled_critical_extensions,
                       TYPE_ASN1_OBJECT_IDENTIFIER, &e->id))
            return burrow_err_out_of_memory;
    }
    return BURROW_NO_ERROR;
}

/* getSignatureAlgorithmFromAI */
static X509SignatureAlgorithm
x509_signature_algorithm_from_ai(const PkixAlgorithmIdentifier *ai) {
    Asn1ObjectIdentifier oid = ai->algorithm;
    /* RFC 8410 section 3 and RFC 9881 section 2: the parameters MUST be
     * absent. */
    if ((x509_oid_is(oid, X509_OID(x509_oid_public_key_ed25519)) ||
         x509_is_mldsa_oid(oid)) &&
        ai->parameters.full_bytes.len != 0)
        return X509_UNKNOWN_SIGNATURE_ALGORITHM;

    if (!x509_oid_is(oid, X509_OID(x509_oid_signature_rsapss))) {
        for (Int i = 0; i < X509_NSIG; i++) {
            const X509SignatureAlgorithmDetails *d =
                &x509_signature_algorithm_details[i];
            if (x509_oid_is(oid, x509_details_oid(d)))
                return d->algo;
        }
        return X509_UNKNOWN_SIGNATURE_ALGORITHM;
    }

    /* RSA PSS keeps what matters in the parameters, and only three settings
     * of them are taken: the MGF1 hash the same as the message hash, the
     * salt as long as the hash, and the default trailer. */
    X509SignatureAlgorithm algo = X509_UNKNOWN_SIGNATURE_ALGORITHM;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *ta = arena_allocator(&ar);
    X509PssParameters params;
    PkixAlgorithmIdentifier mgf1;
    memset(&params, 0, sizeof params);
    memset(&mgf1, 0, sizeof mgf1);
    Error e = BURROW_NO_ERROR;
    (void)asn1_unmarshal(ta, ai->parameters.full_bytes,
                         BURROW_ANY(TYPE_OF(X509PssParameters), &params), &e);
    if (BURROW_FAILED(e))
        goto done;
    (void)asn1_unmarshal(ta, params.mgf.parameters.full_bytes,
                         BURROW_ANY(TYPE_PKIX_ALGORITHM_IDENTIFIER, &mgf1), &e);
    if (BURROW_FAILED(e))
        goto done;
    Slice hp = params.hash.parameters.full_bytes;
    Slice mp = mgf1.parameters.full_bytes;
    if ((hp.len != 0 && !bytes_equal(hp, asn1_null_bytes)) ||
        !x509_oid_is(params.mgf.algorithm, X509_OID(x509_oid_mgf1)) ||
        !x509_oid_is(mgf1.algorithm, params.hash.algorithm) ||
        (mp.len != 0 && !bytes_equal(mp, asn1_null_bytes)) || params.trailer_field != 1)
        goto done;
    if (x509_oid_is(params.hash.algorithm, X509_OID(x509_oid_sha256)) &&
        params.salt_length == 32)
        algo = X509_SHA256_WITH_RSAPSS;
    else if (x509_oid_is(params.hash.algorithm, X509_OID(x509_oid_sha384)) &&
             params.salt_length == 48)
        algo = X509_SHA384_WITH_RSAPSS;
    else if (x509_oid_is(params.hash.algorithm, X509_OID(x509_oid_sha512)) &&
             params.salt_length == 64)
        algo = X509_SHA512_WITH_RSAPSS;
done:
    arena_free(&ar);
    return algo;
}

/* getPublicKeyAlgorithmFromOID */
static X509PublicKeyAlgorithm
x509_public_key_algorithm_from_oid(Asn1ObjectIdentifier oid) {
    if (x509_oid_is(oid, X509_OID(x509_oid_public_key_rsa)))
        return X509_RSA;
    if (x509_oid_is(oid, X509_OID(x509_oid_public_key_dsa)))
        return X509_DSA;
    if (x509_oid_is(oid, X509_OID(x509_oid_public_key_ecdsa)))
        return X509_ECDSA;
    if (x509_oid_is(oid, X509_OID(x509_oid_public_key_ed25519)))
        return X509_ED25519;
    if (x509_is_mldsa_oid(oid))
        return X509_MLDSA;
    return X509_UNKNOWN_PUBLIC_KEY_ALGORITHM;
}

/* Reads one Extension SEQUENCE from *exts into a. */
static Error x509_read_extension(Alloc *a, CryptobyteString *exts, PkixExtension *ext) {
    CryptobyteString one;
    if (!cryptobyte_string_read_asn1(exts, &one, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed extension");
    return x509_parse_extension(a, one, ext);
}

/* The slices of a certificate with their element types and no elements. */
static void x509_certificate_init(X509Certificate *c) {
    memset(c, 0, sizeof *c);
    c->raw = c->raw_tbs_certificate = c->raw_subject_public_key_info =
        slice_nil(TYPE_BYTE);
    c->raw_subject = c->raw_issuer = c->raw_signature_algorithm = slice_nil(TYPE_BYTE);
    c->signature = c->subject_key_id = c->authority_key_id = slice_nil(TYPE_BYTE);
    c->extensions = c->extra_extensions = slice_nil(TYPE_PKIX_EXTENSION);
    c->unhandled_critical_extensions = slice_nil(TYPE_ASN1_OBJECT_IDENTIFIER);
    c->ext_key_usage = slice_nil(TYPE_INT);
    c->unknown_ext_key_usage = slice_nil(TYPE_ASN1_OBJECT_IDENTIFIER);
    c->ocsp_server = c->issuing_certificate_url = slice_nil(TYPE_STRING);
    c->dns_names = c->email_addresses = slice_nil(TYPE_STRING);
    c->ip_addresses = slice_nil(TYPE_NET_IP);
    c->uris = slice_nil(TYPE_X509_URL_PTR);
    c->permitted_dns_domains = c->excluded_dns_domains = slice_nil(TYPE_STRING);
    c->permitted_ip_ranges = c->excluded_ip_ranges = slice_nil(TYPE_X509_IP_NET_PTR);
    c->permitted_email_addresses = c->excluded_email_addresses = slice_nil(TYPE_STRING);
    c->permitted_uri_domains = c->excluded_uri_domains = slice_nil(TYPE_STRING);
    c->crl_distribution_points = slice_nil(TYPE_STRING);
    c->policy_identifiers = slice_nil(TYPE_ASN1_OBJECT_IDENTIFIER);
    c->policies = slice_nil(TYPE_OF(X509OID));
    c->policy_mappings = slice_nil(TYPE_X509_POLICY_MAPPING);
}

/* The extensions of a version 3 certificate, [3] EXPLICIT. */
static Error x509_parse_certificate_extensions(Alloc *a, CryptobyteString *tbs,
                                               X509Certificate *cert) {
    CryptobyteString exts = {0};
    bool present = false;
    if (!cryptobyte_string_read_optional_asn1(tbs, &exts, &present, X509_CTX_CONS(3)))
        return x509_err("x509: malformed extensions");
    if (!present)
        return BURROW_NO_ERROR;
    if (!cryptobyte_string_read_asn1(&exts, &exts, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed extensions");
    while (!cryptobyte_string_empty(exts)) {
        PkixExtension ext = {0};
        Error err = x509_read_extension(a, &exts, &ext);
        if (BURROW_FAILED(err))
            return err;
        const PkixExtension *seen = cert->extensions.p;
        for (Int i = 0; i < cert->extensions.len; i++)
            if (x509_oid_is(seen[i].id, ext.id))
                return fmt_errorf_v(
                    "x509: certificate contains duplicate extension with OID %q",
                    asn1_object_identifier_string(ext.id, error_allocator()));
        if (!x509_push(a, &cert->extensions, TYPE_PKIX_EXTENSION, &ext))
            return burrow_err_out_of_memory;
    }
    return x509_process_extensions(a, cert);
}

/* The AlgorithmIdentifier SEQUENCE at the front of tbs, checked against the
 * one after the TBS in input. The element goes to *raw. */
static Error x509_read_signature_ai(Alloc *a, CryptobyteString *tbs,
                                    CryptobyteString *input, Slice *raw,
                                    X509SignatureAlgorithm *algo) {
    CryptobyteString sig_ai, outer;
    if (!cryptobyte_string_read_asn1_element(tbs, &sig_ai, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed signature algorithm identifier");
    *raw = sig_ai;
    if (!cryptobyte_string_read_asn1(&sig_ai, &sig_ai, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed signature algorithm identifier");
    if (!cryptobyte_string_read_asn1(input, &outer, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed algorithm identifier");
    if (!bytes_equal(outer, sig_ai))
        return x509_err(
            "x509: inner and outer signature algorithm identifiers don't match");
    PkixAlgorithmIdentifier ai;
    Error err = x509_parse_ai(a, sig_ai, &ai);
    if (BURROW_FAILED(err))
        return err;
    *algo = x509_signature_algorithm_from_ai(&ai);
    return BURROW_NO_ERROR;
}

/* The SubjectPublicKeyInfo of a certificate. */
static Error x509_read_spki(Alloc *a, CryptobyteString *tbs, X509Certificate *cert) {
    CryptobyteString spki, pk_ai_seq;
    if (!cryptobyte_string_read_asn1_element(tbs, &spki, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed spki");
    cert->raw_subject_public_key_info = spki;
    if (!cryptobyte_string_read_asn1(&spki, &spki, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed spki");
    if (!cryptobyte_string_read_asn1(&spki, &pk_ai_seq, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed public key algorithm identifier");
    X509PublicKeyInfo ki;
    memset(&ki, 0, sizeof ki);
    Error err = x509_parse_ai(a, pk_ai_seq, &ki.algorithm);
    if (BURROW_FAILED(err))
        return err;
    cert->public_key_algorithm =
        x509_public_key_algorithm_from_oid(ki.algorithm.algorithm);
    if (!cryptobyte_string_read_asn1_bit_string(&spki, &ki.public_key))
        return x509_err("x509: malformed subjectPublicKey");
    if (cert->public_key_algorithm != X509_UNKNOWN_PUBLIC_KEY_ALGORITHM) {
        err = BURROW_NO_ERROR;
        cert->public_key = x509_parse_public_key(a, &ki, &err);
        if (BURROW_FAILED(err))
            return err;
    }
    return BURROW_NO_ERROR;
}

/* parseCertificate */
static Error x509_parse_certificate_der(Alloc *a, Slice der, X509Certificate *cert) {
    x509_certificate_init(cert);
    CryptobyteString input = der;
    /* The SEQUENCE with its header first, for raw, then its contents. */
    if (!cryptobyte_string_read_asn1_element(&input, &input, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed certificate");
    cert->raw = input;
    if (!cryptobyte_string_read_asn1(&input, &input, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed certificate");

    CryptobyteString tbs;
    if (!cryptobyte_string_read_asn1_element(&input, &tbs, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed tbs certificate");
    cert->raw_tbs_certificate = tbs;
    if (!cryptobyte_string_read_asn1(&tbs, &tbs, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed tbs certificate");

    if (!cryptobyte_string_read_optional_asn1_integer_int(&tbs, &cert->version,
                                                          X509_CTX_CONS(0), 0))
        return x509_err("x509: malformed version");
    if (cert->version < 0)
        return x509_err("x509: malformed version");
    /* One-indexed, unlike the encoding of RFC 5280, as Go has always had it. */
    cert->version++;
    if (cert->version > 3)
        return x509_err("x509: invalid version");

    BigInt *serial = big_new_int(a, 0);
    if (serial == NULL)
        return burrow_err_out_of_memory;
    if (!cryptobyte_string_read_asn1_integer_big(&tbs, serial))
        return x509_err("x509: malformed serial number");
    if (big_int_sign(serial) == -1 &&
        (x509_debug_load() & X509_DEBUG_NEGATIVE_SERIAL) == 0)
        return x509_err("x509: negative serial number");
    cert->serial_number = serial;

    Error err = x509_read_signature_ai(a, &tbs, &input, &cert->raw_signature_algorithm,
                                       &cert->signature_algorithm);
    if (BURROW_FAILED(err))
        return err;

    CryptobyteString issuer;
    if (!cryptobyte_string_read_asn1_element(&tbs, &issuer, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed issuer");
    cert->raw_issuer = issuer;
    err = x509_parse_name_into(a, issuer, &cert->issuer);
    if (BURROW_FAILED(err))
        return err;

    CryptobyteString validity;
    if (!cryptobyte_string_read_asn1(&tbs, &validity, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed validity");
    err = x509_read_asn1_time(a, &validity, &cert->not_before);
    if (BURROW_FAILED(err))
        return err;
    err = x509_read_asn1_time(a, &validity, &cert->not_after);
    if (BURROW_FAILED(err))
        return err;

    CryptobyteString subject;
    if (!cryptobyte_string_read_asn1_element(&tbs, &subject, CRYPTOBYTE_ASN1_SEQUENCE))
        /* Go says issuer here too. */
        return x509_err("x509: malformed issuer");
    cert->raw_subject = subject;
    err = x509_parse_name_into(a, subject, &cert->subject);
    if (BURROW_FAILED(err))
        return err;

    err = x509_read_spki(a, &tbs, cert);
    if (BURROW_FAILED(err))
        return err;

    if (cert->version > 1) {
        if (!cryptobyte_string_skip_optional_asn1(&tbs, X509_CTX(1)))
            return x509_err("x509: malformed issuerUniqueID");
        if (!cryptobyte_string_skip_optional_asn1(&tbs, X509_CTX(2)))
            return x509_err("x509: malformed subjectUniqueID");
        if (cert->version == 3) {
            err = x509_parse_certificate_extensions(a, &tbs, cert);
            if (BURROW_FAILED(err))
                return err;
        }
    }

    Asn1BitString signature = {0};
    if (!cryptobyte_string_read_asn1_bit_string(&input, &signature))
        return x509_err("x509: malformed signature");
    cert->signature = asn1_bit_string_right_align(signature, a);
    return BURROW_NO_ERROR;
}

X509Certificate *x509_parse_certificate(Alloc *a, Slice der, Error *err) {
    X509Certificate *cert = BURROW_NEW(a, X509Certificate);
    if (cert == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    Error e = x509_parse_certificate_der(a, der, cert);
    if (!BURROW_FAILED(e) && der.len != cert->raw.len)
        e = x509_err("x509: trailing data");
    BURROW_OUT(err, e);
    return BURROW_FAILED(e) ? NULL : cert;
}

Slice x509_parse_certificates(Alloc *a, Slice der, Error *err) {
    Slice certs = slice_nil(TYPE_X509_CERTIFICATE_PTR);
    while (der.len > 0) {
        X509Certificate *cert = BURROW_NEW(a, X509Certificate);
        Error e = cert == NULL ? burrow_err_out_of_memory
                               : x509_parse_certificate_der(a, der, cert);
        if (!BURROW_FAILED(e) &&
            !x509_push(a, &certs, TYPE_X509_CERTIFICATE_PTR, &cert))
            e = burrow_err_out_of_memory;
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return slice_nil(TYPE_X509_CERTIFICATE_PTR);
        }
        der = slice_sub(der, cert->raw.len, der.len);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return certs;
}

bool x509_certificate_equal(const X509Certificate *c, const X509Certificate *other) {
    if (c == NULL || other == NULL)
        return c == other;
    return bytes_equal(c->raw, other->raw);
}

/* ------------------------------------------------------------- signatures */

BURROW_SENTINEL_ERROR(x509_err_unsupported_algorithm,
                      "x509: cannot verify signature: algorithm unimplemented");

/* InsecureAlgorithmError with its message, the algorithm first so errors_as
 * hands back a pointer to it. */
typedef struct X509InsecureBox {
    X509InsecureAlgorithmError algo;
    Str message;
} X509InsecureBox;

static const Type x509_insecure_desc = {
    {(const Byte *)"InsecureAlgorithmError", 22},
    {(const Byte *)"crypto/x509", 11},
    KIND_INT,
    (uint32_t)sizeof(X509InsecureAlgorithmError),
    (uint16_t)_Alignof(X509InsecureAlgorithmError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0,
    NULL,
};

const Type *const TYPE_X509_INSECURE_ALGORITHM_ERROR = &x509_insecure_desc;

static const char x509_insecure_prefix[] =
    "x509: cannot verify signature: insecure algorithm ";

#define X509_INSECURE_PREFIX_LEN ((Int)sizeof(x509_insecure_prefix) - 1)

static Str x509_insecure_text(const void *self) {
    return ((const X509InsecureBox *)self)->message;
}

static Error x509_insecure_clone(const void *self, Alloc *a) {
    return x509_insecure_algorithm_error_as_error(((const X509InsecureBox *)self)->algo,
                                                  a);
}

static const ErrorVT x509_insecure_vt = {
    .self_type = &x509_insecure_desc,
    .message = x509_insecure_text,
    .clone = x509_insecure_clone,
};

/* SignatureAlgorithm.String of e, with num as the room for a number. */
static Str x509_insecure_name(X509InsecureAlgorithmError e, char num[24]) {
    for (Int i = 0; i < X509_NSIG; i++)
        if (x509_signature_algorithm_details[i].algo == e)
            return str_from_cstr(x509_signature_algorithm_details[i].name);
    return str_from_bytes((const Byte *)num, x509_itoa(e, num));
}

/* Writes the message for name to p, which has room for it. */
static Str x509_insecure_write(Byte *p, Str name) {
    memcpy(p, x509_insecure_prefix, (size_t)X509_INSECURE_PREFIX_LEN);
    memcpy(p + X509_INSECURE_PREFIX_LEN, name.p, (size_t)name.len);
    return str_from_bytes(p, X509_INSECURE_PREFIX_LEN + name.len);
}

Error x509_insecure_algorithm_error_as_error(X509InsecureAlgorithmError e, Alloc *a) {
    char num[24];
    Str name = x509_insecure_name(e, num);
    X509InsecureBox *b = (X509InsecureBox *)mem_alloc_nozero(
        a, sizeof(X509InsecureBox) + (size_t)(X509_INSECURE_PREFIX_LEN + name.len),
        _Alignof(X509InsecureBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    b->algo = e;
    b->message = x509_insecure_write((Byte *)(b + 1), name);
    return (Error){&x509_insecure_vt, b};
}

Str x509_insecure_algorithm_error_error(X509InsecureAlgorithmError e, Alloc *a) {
    char num[24];
    Str name = x509_insecure_name(e, num);
    Byte *p =
        (Byte *)mem_alloc_nozero(a, (size_t)(X509_INSECURE_PREFIX_LEN + name.len), 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    return x509_insecure_write(p, name);
}

/* ConstraintViolationError and UnhandledCriticalExtension, which have
 * nothing in them and so are one constant each. */
/* NOLINTBEGIN(bugprone-macro-parentheses) */
#define X509_EMPTY_ERROR(T, desc, name, gotype, text)                                  \
    static const Type desc = {                                                         \
        {(const Byte *)(gotype), (Int)(sizeof(gotype) - 1)},                           \
        {(const Byte *)"crypto/x509", 11},                                             \
        KIND_STRUCT,                                                                   \
        (uint32_t)sizeof(T),                                                           \
        (uint16_t)_Alignof(T),                                                         \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        NULL,                                                                          \
        0,                                                                             \
        0,                                                                             \
        NULL,                                                                          \
    };                                                                                 \
    static Str desc##_message(const void *self) {                                      \
        (void)self;                                                                    \
        return (Str){(const Byte *)(text), (Int)(sizeof(text) - 1)};                   \
    }                                                                                  \
    static const ErrorVT desc##_vt = {                                                 \
        &desc, desc##_message, NULL, NULL, NULL, NULL, NULL,                           \
    };                                                                                 \
    static const T desc##_value = {0};                                                 \
    const Error name = {&desc##_vt, &desc##_value}
/* NOLINTEND(bugprone-macro-parentheses) */

X509_EMPTY_ERROR(X509ConstraintViolationError, x509_constraint_violation_desc,
                 x509_constraint_violation_error, "ConstraintViolationError",
                 "x509: invalid signature: parent certificate cannot sign this kind of "
                 "certificate");
const Type *const TYPE_X509_CONSTRAINT_VIOLATION_ERROR =
    &x509_constraint_violation_desc;

Str x509_constraint_violation_error_error(X509ConstraintViolationError e) {
    return x509_constraint_violation_desc_message(&e);
}

X509_EMPTY_ERROR(X509UnhandledCriticalExtension, x509_unhandled_critical_desc,
                 x509_unhandled_critical_extension, "UnhandledCriticalExtension",
                 "x509: unhandled critical extension");
const Type *const TYPE_X509_UNHANDLED_CRITICAL_EXTENSION =
    &x509_unhandled_critical_desc;

Str x509_unhandled_critical_extension_error(X509UnhandledCriticalExtension e) {
    return x509_unhandled_critical_desc_message(&e);
}

/* signaturePublicKeyAlgoMismatchError, type_name being what %T says of the
 * key in Go. */
static Error x509_key_mismatch_error(X509PublicKeyAlgorithm want, const char *type_name,
                                     Alloc *ta) {
    return fmt_errorf_v(
        "x509: signature algorithm specifies an %s public key, but have "
        "public key of type %s",
        x509_public_key_algorithm_string(want, ta), type_name);
}

/* checkSignature, with signed hashed in ta first when the algorithm hashes. */
static Error x509_check_signature_in(Alloc *ta, X509SignatureAlgorithm algo,
                                     Slice signed_data, Slice signature, Any public_key,
                                     bool allow_sha1) {
    CryptoHash hash_type = 0;
    X509PublicKeyAlgorithm pub_key_algo = X509_UNKNOWN_PUBLIC_KEY_ALGORITHM;
    bool is_pss = false;
    for (Int i = 0; i < X509_NSIG; i++) {
        const X509SignatureAlgorithmDetails *d = &x509_signature_algorithm_details[i];
        if (d->algo == algo) {
            hash_type = d->hash;
            pub_key_algo = d->pub_key_algo;
            is_pss = d->is_rsa_pss;
            break;
        }
    }

    if (hash_type == 0) {
        if (pub_key_algo != X509_ED25519 && pub_key_algo != X509_MLDSA)
            return x509_err_unsupported_algorithm;
    } else {
        if (hash_type == CRYPTO_MD5 || (hash_type == CRYPTO_SHA1 && !allow_sha1))
            /* SHA-1 is only taken for CRLs and CSRs. */
            return x509_insecure_algorithm_error_as_error(algo, error_allocator());
        if (!crypto_hash_available(hash_type))
            return x509_err_unsupported_algorithm;
        Hash h = crypto_hash_new(hash_type, ta);
        if (h.vt == NULL)
            return burrow_err_out_of_memory;
        hash_write(h, signed_data, NULL);
        signed_data = hash_sum(ta, h, slice_nil(TYPE_BYTE));
        if (signed_data.len == 0)
            return burrow_err_out_of_memory;
    }

    const Type *t = public_key.t;
    if (t == NULL || public_key.data == NULL)
        return x509_err_unsupported_algorithm;
    if (t == TYPE_RSA_PUBLIC_KEY) {
        if (pub_key_algo != X509_RSA)
            return x509_key_mismatch_error(pub_key_algo, "*rsa.PublicKey", ta);
        const RsaPublicKey *pub = public_key.data;
        if (is_pss) {
            RsaPSSOptions opts = {RSA_PSS_SALT_LENGTH_EQUALS_HASH, 0};
            return rsa_verify_pss(pub, hash_type, signed_data, signature, &opts);
        }
        return rsa_verify_pkcs1_v15(pub, hash_type, signed_data, signature);
    }
    if (t == TYPE_ECDSA_PUBLIC_KEY) {
        if (pub_key_algo != X509_ECDSA)
            return x509_key_mismatch_error(pub_key_algo, "*ecdsa.PublicKey", ta);
        if (!ecdsa_verify_asn1(public_key.data, signed_data, signature))
            return x509_err("x509: ECDSA verification failure");
        return BURROW_NO_ERROR;
    }
    if (t == TYPE_ED25519_PUBLIC_KEY) {
        if (pub_key_algo != X509_ED25519)
            return x509_key_mismatch_error(pub_key_algo, "ed25519.PublicKey", ta);
        if (!ed25519_verify(*(const Slice *)public_key.data, signed_data, signature))
            return x509_err("x509: Ed25519 verification failure");
        return BURROW_NO_ERROR;
    }
    if (t == TYPE_MLDSA_PUBLIC_KEY) {
        if (pub_key_algo != X509_MLDSA)
            return x509_key_mismatch_error(pub_key_algo, "*mldsa.PublicKey", ta);
        const MldsaPublicKey *pub = public_key.data;
        const MldsaParameters *params = mldsa_public_key_parameters(pub);
        X509SignatureAlgorithm want;
        if (params == mldsa_mldsa44())
            want = X509_MLDSA44;
        else if (params == mldsa_mldsa65())
            want = X509_MLDSA65;
        else if (params == mldsa_mldsa87())
            want = X509_MLDSA87;
        else
            return fmt_errorf_v("x509: unknown ML-DSA parameters: %s",
                                mldsa_parameters_string(params));
        if (algo != want)
            return fmt_errorf_v(
                "x509: signature algorithm specifies an ML-DSA public key with %s "
                "parameters, but have a public key with %s parameters",
                x509_signature_algorithm_string(algo, ta),
                mldsa_parameters_string(params));
        Error e = mldsa_verify(pub, signed_data, signature, NULL);
        if (BURROW_FAILED(e))
            return fmt_errorf_v("x509: ML-DSA verification failure: %w", e);
        return BURROW_NO_ERROR;
    }
    return x509_err_unsupported_algorithm;
}

static Error x509_check_signature(X509SignatureAlgorithm algo, Slice signed_data,
                                  Slice signature, Any public_key, bool allow_sha1) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error e = x509_check_signature_in(arena_allocator(&ar), algo, signed_data,
                                      signature, public_key, allow_sha1);
    arena_free(&ar);
    return e;
}

/* Whether parent may sign with usage, the checks CheckSignatureFrom of a
 * certificate and of a revocation list share. */
static Error x509_check_parent(const X509Certificate *parent, X509KeyUsage usage) {
    /* RFC 5280 section 4.2.1.9: without basic constraints in a version 3
     * certificate, or with them and no cA, the key MUST NOT check
     * certificate signatures. */
    if ((parent->version == 3 && !parent->basic_constraints_valid) ||
        (parent->basic_constraints_valid && !parent->is_ca))
        return x509_constraint_violation_error;
    if (parent->key_usage != 0 && (parent->key_usage & usage) == 0)
        return x509_constraint_violation_error;
    if (parent->public_key_algorithm == X509_UNKNOWN_PUBLIC_KEY_ALGORITHM)
        return x509_err_unsupported_algorithm;
    return BURROW_NO_ERROR;
}

Error x509_certificate_check_signature_from(const X509Certificate *c,
                                            const X509Certificate *parent) {
    Error e = x509_check_parent(parent, X509_KEY_USAGE_CERT_SIGN);
    if (BURROW_FAILED(e))
        return e;
    return x509_check_signature(c->signature_algorithm, c->raw_tbs_certificate,
                                c->signature, parent->public_key, false);
}

Error x509_certificate_check_signature(const X509Certificate *c,
                                       X509SignatureAlgorithm algo, Slice signed_data,
                                       Slice signature) {
    return x509_check_signature(algo, signed_data, signature, c->public_key, true);
}

Error x509_certificate_check_crl_signature(const X509Certificate *c,
                                           const PkixCertificateList *crl) {
    X509SignatureAlgorithm algo =
        x509_signature_algorithm_from_ai(&crl->signature_algorithm);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *ta = arena_allocator(&ar);
    Slice sig = asn1_bit_string_right_align(crl->signature_value, ta);
    Error e =
        x509_check_signature(algo, crl->tbs_cert_list.raw, sig, c->public_key, true);
    arena_free(&ar);
    return e;
}

/* ---------------------------------------------------- certificate requests */

/* parseRawAttributes: the attributes that are AttributeTypeAndValueSETs,
 * leaving out the ones such as challengePassword that are not. */
static Slice x509_parse_raw_attributes(Alloc *a, Slice raw_attributes) {
    Slice out = slice_nil(TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE_SET);
    const Asn1RawValue *raw = raw_attributes.p;
    for (Int i = 0; i < raw_attributes.len; i++) {
        PkixAttributeTypeAndValueSET attr;
        memset(&attr, 0, sizeof attr);
        Error e = BURROW_NO_ERROR;
        Slice rest = asn1_unmarshal(
            a, raw[i].full_bytes,
            BURROW_ANY(TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE_SET, &attr), &e);
        if (!BURROW_FAILED(e) && rest.len == 0)
            (void)x509_push(a, &out, TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE_SET, &attr);
    }
    return out;
}

/* parseCSRExtensions: the extensions in the extensionRequest attributes. */
static Error x509_parse_csr_extensions(Alloc *a, Slice raw_attributes, Slice *out) {
    *out = slice_nil(TYPE_PKIX_EXTENSION);
    const Asn1RawValue *raw = raw_attributes.p;
    for (Int i = 0; i < raw_attributes.len; i++) {
        X509Pkcs10Attribute attr;
        memset(&attr, 0, sizeof attr);
        Error e = BURROW_NO_ERROR;
        Slice rest = asn1_unmarshal(
            a, raw[i].full_bytes, BURROW_ANY(TYPE_OF(X509Pkcs10Attribute), &attr), &e);
        /* The ones that do not parse are skipped. */
        if (BURROW_FAILED(e) || rest.len != 0 || attr.values.len == 0)
            continue;
        if (!x509_oid_is(attr.id, X509_OID(x509_oid_extension_request)))
            continue;
        Slice exts = slice_nil(TYPE_PKIX_EXTENSION);
        const Asn1RawValue *values = attr.values.p;
        (void)asn1_unmarshal(a, values[0].full_bytes,
                             BURROW_ANY(TYPE_OF(X509Extensions), &exts), &e);
        if (BURROW_FAILED(e))
            return e;
        const PkixExtension *ext = exts.p;
        for (Int j = 0; j < exts.len; j++) {
            const PkixExtension *seen = out->p;
            for (Int k = 0; k < out->len; k++)
                if (x509_oid_is(seen[k].id, ext[j].id))
                    return x509_err(
                        "x509: certificate request contains duplicate requested "
                        "extensions");
            if (!x509_push(a, out, TYPE_PKIX_EXTENSION, &ext[j]))
                return burrow_err_out_of_memory;
        }
    }
    return BURROW_NO_ERROR;
}

/* parseCertificateRequest */
static Error x509_parse_csr(Alloc *a, const X509Csr *in, X509CertificateRequest *out) {
    memset(out, 0, sizeof *out);
    out->raw = in->raw;
    out->raw_tbs_certificate_request = in->tbs_csr.raw;
    out->raw_subject_public_key_info = in->tbs_csr.public_key.raw;
    out->raw_subject = in->tbs_csr.subject.full_bytes;
    out->raw_signature_algorithm = in->signature_algorithm.raw;
    out->signature = asn1_bit_string_right_align(in->signature_value, a);
    PkixAlgorithmIdentifier ai = {in->signature_algorithm.algorithm,
                                  in->signature_algorithm.parameters};
    out->signature_algorithm = x509_signature_algorithm_from_ai(&ai);
    out->public_key_algorithm =
        x509_public_key_algorithm_from_oid(in->tbs_csr.public_key.algorithm.algorithm);
    out->version = in->tbs_csr.version;
    out->attributes = x509_parse_raw_attributes(a, in->tbs_csr.raw_attributes);
    out->extra_extensions = slice_nil(TYPE_PKIX_EXTENSION);
    out->dns_names = out->email_addresses = slice_nil(TYPE_STRING);
    out->ip_addresses = slice_nil(TYPE_NET_IP);
    out->uris = slice_nil(TYPE_X509_URL_PTR);

    Error err = BURROW_NO_ERROR;
    if (out->public_key_algorithm != X509_UNKNOWN_PUBLIC_KEY_ALGORITHM) {
        out->public_key = x509_parse_public_key(a, &in->tbs_csr.public_key, &err);
        if (BURROW_FAILED(err))
            return err;
    }
    err = x509_parse_name_into(a, in->tbs_csr.subject.full_bytes, &out->subject);
    if (BURROW_FAILED(err))
        return err;
    err = x509_parse_csr_extensions(a, in->tbs_csr.raw_attributes, &out->extensions);
    if (BURROW_FAILED(err))
        return err;
    const PkixExtension *ext = out->extensions.p;
    for (Int i = 0; i < out->extensions.len; i++) {
        if (!x509_oid_is(ext[i].id, X509_OID(x509_oid_extension_subject_alt_name)))
            continue;
        X509SANs sans;
        err = x509_parse_san(a, ext[i].value, &sans);
        if (BURROW_FAILED(err))
            return err;
        out->dns_names = sans.dns_names;
        out->email_addresses = sans.email_addresses;
        out->ip_addresses = sans.ip_addresses;
        out->uris = sans.uris;
    }
    return BURROW_NO_ERROR;
}

X509CertificateRequest *x509_parse_certificate_request(Alloc *a, Slice der,
                                                       Error *err) {
    X509Csr csr;
    memset(&csr, 0, sizeof csr);
    Error e = BURROW_NO_ERROR;
    Slice rest = asn1_unmarshal(a, der, BURROW_ANY(TYPE_OF(X509Csr), &csr), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    if (rest.len != 0) {
        BURROW_OUT(err, x509_trailing_data());
        return NULL;
    }
    X509CertificateRequest *out = BURROW_NEW(a, X509CertificateRequest);
    e = out == NULL ? burrow_err_out_of_memory : x509_parse_csr(a, &csr, out);
    BURROW_OUT(err, e);
    return BURROW_FAILED(e) ? NULL : out;
}

Error x509_certificate_request_check_signature(const X509CertificateRequest *c) {
    return x509_check_signature(c->signature_algorithm, c->raw_tbs_certificate_request,
                                c->signature, c->public_key, true);
}

/* -------------------------------------------------------- revocation lists */

/* One entry of revokedCertificates. */
static Error x509_parse_revoked_entry(Alloc *a, CryptobyteString *revoked,
                                      X509RevocationList *rl) {
    X509RevocationListEntry rce;
    memset(&rce, 0, sizeof rce);
    rce.extensions = rce.extra_extensions = slice_nil(TYPE_PKIX_EXTENSION);
    CryptobyteString cert;
    if (!cryptobyte_string_read_asn1_element(revoked, &cert, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed crl");
    rce.raw = cert;
    if (!cryptobyte_string_read_asn1(&cert, &cert, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed crl");
    rce.serial_number = big_new_int(a, 0);
    if (rce.serial_number == NULL)
        return burrow_err_out_of_memory;
    if (!cryptobyte_string_read_asn1_integer_big(&cert, rce.serial_number))
        return x509_err("x509: malformed serial number");
    Error err = x509_read_asn1_time(a, &cert, &rce.revocation_time);
    if (BURROW_FAILED(err))
        return err;
    CryptobyteString exts = {0};
    bool present = false;
    if (!cryptobyte_string_read_optional_asn1(&cert, &exts, &present,
                                              CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed extensions");
    while (present && !cryptobyte_string_empty(exts)) {
        PkixExtension ext = {0};
        err = x509_read_extension(a, &exts, &ext);
        if (BURROW_FAILED(err))
            return err;
        if (x509_oid_is(ext.id, X509_OID(x509_oid_extension_reason_code))) {
            CryptobyteString val = ext.value;
            if (!cryptobyte_string_read_asn1_enum(&val, &rce.reason_code))
                return x509_err("x509: malformed reasonCode extension");
        }
        if (!x509_push(a, &rce.extensions, TYPE_PKIX_EXTENSION, &ext))
            return burrow_err_out_of_memory;
    }
    PkixRevokedCertificate rc = {rce.serial_number, rce.revocation_time,
                                 rce.extensions};
    if (!x509_push(a, &rl->revoked_certificate_entries, TYPE_X509_REVOCATION_LIST_ENTRY,
                   &rce) ||
        !x509_push(a, &rl->revoked_certificates, TYPE_PKIX_REVOKED_CERTIFICATE, &rc))
        return burrow_err_out_of_memory;
    return BURROW_NO_ERROR;
}

/* The crlExtensions of a list, [0] EXPLICIT. */
static Error x509_parse_crl_extensions(Alloc *a, CryptobyteString *tbs,
                                       X509RevocationList *rl) {
    CryptobyteString exts = {0};
    bool present = false;
    if (!cryptobyte_string_read_optional_asn1(tbs, &exts, &present, X509_CTX_CONS(0)))
        return x509_err("x509: malformed extensions");
    if (!present)
        return BURROW_NO_ERROR;
    if (!cryptobyte_string_read_asn1(&exts, &exts, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed extensions");
    while (!cryptobyte_string_empty(exts)) {
        PkixExtension ext = {0};
        Error err = x509_read_extension(a, &exts, &ext);
        if (BURROW_FAILED(err))
            return err;
        if (x509_oid_is(ext.id, X509_OID(x509_oid_extension_authority_key_id))) {
            err = x509_parse_authority_key_id(&ext, &rl->authority_key_id);
            if (BURROW_FAILED(err))
                return err;
        } else if (x509_oid_is(ext.id, X509_OID(x509_oid_extension_crl_number))) {
            CryptobyteString value = ext.value;
            rl->number = big_new_int(a, 0);
            if (rl->number == NULL)
                return burrow_err_out_of_memory;
            if (!cryptobyte_string_read_asn1_integer_big(&value, rl->number))
                return x509_err("x509: malformed crl number");
        }
        if (!x509_push(a, &rl->extensions, TYPE_PKIX_EXTENSION, &ext))
            return burrow_err_out_of_memory;
    }
    return BURROW_NO_ERROR;
}

/* ParseRevocationList */
static Error x509_parse_revocation_list_der(Alloc *a, Slice der,
                                            X509RevocationList *rl) {
    memset(rl, 0, sizeof *rl);
    rl->authority_key_id = slice_nil(TYPE_BYTE);
    rl->revoked_certificate_entries = slice_nil(TYPE_X509_REVOCATION_LIST_ENTRY);
    rl->revoked_certificates = slice_nil(TYPE_PKIX_REVOKED_CERTIFICATE);
    rl->extensions = rl->extra_extensions = slice_nil(TYPE_PKIX_EXTENSION);

    CryptobyteString input = der;
    if (!cryptobyte_string_read_asn1_element(&input, &input, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed crl");
    rl->raw = input;
    if (!cryptobyte_string_read_asn1(&input, &input, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed crl");

    CryptobyteString tbs;
    if (!cryptobyte_string_read_asn1_element(&input, &tbs, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed tbs crl");
    rl->raw_tbs_revocation_list = tbs;
    if (!cryptobyte_string_read_asn1(&tbs, &tbs, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed tbs crl");

    Int version = 0;
    if (!cryptobyte_string_peek_asn1_tag(tbs, CRYPTOBYTE_ASN1_INTEGER))
        return x509_err("x509: unsupported crl version");
    if (!cryptobyte_string_read_asn1_integer_int(&tbs, &version))
        return x509_err("x509: malformed crl");
    /* X.509 v2 is encoded as 1. */
    if (version != 1)
        return fmt_errorf_v("x509: unsupported crl version: %d", version);

    Error err = x509_read_signature_ai(a, &tbs, &input, &rl->raw_signature_algorithm,
                                       &rl->signature_algorithm);
    if (BURROW_FAILED(err))
        return err;

    Asn1BitString signature = {0};
    if (!cryptobyte_string_read_asn1_bit_string(&input, &signature))
        return x509_err("x509: malformed signature");
    rl->signature = asn1_bit_string_right_align(signature, a);

    CryptobyteString issuer;
    if (!cryptobyte_string_read_asn1_element(&tbs, &issuer, CRYPTOBYTE_ASN1_SEQUENCE))
        return x509_err("x509: malformed issuer");
    rl->raw_issuer = issuer;
    err = x509_parse_name_into(a, issuer, &rl->issuer);
    if (BURROW_FAILED(err))
        return err;

    err = x509_read_asn1_time(a, &tbs, &rl->this_update);
    if (BURROW_FAILED(err))
        return err;
    if (cryptobyte_string_peek_asn1_tag(tbs, CRYPTOBYTE_ASN1_GENERALIZED_TIME) ||
        cryptobyte_string_peek_asn1_tag(tbs, CRYPTOBYTE_ASN1_UTC_TIME)) {
        err = x509_read_asn1_time(a, &tbs, &rl->next_update);
        if (BURROW_FAILED(err))
            return err;
    }

    if (cryptobyte_string_peek_asn1_tag(tbs, CRYPTOBYTE_ASN1_SEQUENCE)) {
        CryptobyteString revoked;
        if (!cryptobyte_string_read_asn1(&tbs, &revoked, CRYPTOBYTE_ASN1_SEQUENCE))
            return x509_err("x509: malformed crl");
        while (!cryptobyte_string_empty(revoked)) {
            err = x509_parse_revoked_entry(a, &revoked, rl);
            if (BURROW_FAILED(err))
                return err;
        }
    }
    return x509_parse_crl_extensions(a, &tbs, rl);
}

X509RevocationList *x509_parse_revocation_list(Alloc *a, Slice der, Error *err) {
    X509RevocationList *rl = BURROW_NEW(a, X509RevocationList);
    Error e = rl == NULL ? burrow_err_out_of_memory
                         : x509_parse_revocation_list_der(a, der, rl);
    BURROW_OUT(err, e);
    return BURROW_FAILED(e) ? NULL : rl;
}

Error x509_revocation_list_check_signature_from(const X509RevocationList *rl,
                                                const X509Certificate *parent) {
    Error e = x509_check_parent(parent, X509_KEY_USAGE_CRL_SIGN);
    if (BURROW_FAILED(e))
        return e;
    return x509_certificate_check_signature(parent, rl->signature_algorithm,
                                            rl->raw_tbs_revocation_list, rl->signature);
}

PkixCertificateList *x509_parse_crl(Alloc *a, Slice crl_bytes, Error *err) {
    /* A PEM CRL where a DER one should be is common, so one with nothing in
     * front of it is decoded first. */
    static const Byte prefix[] = "-----BEGIN X509 CRL";
    if (bytes_has_prefix(crl_bytes, x509_bytes(prefix, (Int)sizeof prefix - 1))) {
        Slice rest;
        PemBlock *block = pem_decode(a, crl_bytes, &rest);
        if (block != NULL && str_eq(block->type, BURROW_S("X509 CRL")))
            crl_bytes = block->bytes;
    }
    return x509_parse_dercrl(a, crl_bytes, err);
}

PkixCertificateList *x509_parse_dercrl(Alloc *a, Slice der, Error *err) {
    PkixCertificateList *cl = BURROW_NEW(a, PkixCertificateList);
    if (cl == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    Error e = BURROW_NO_ERROR;
    Slice rest = asn1_unmarshal(a, der, BURROW_ANY(TYPE_PKIX_CERTIFICATE_LIST, cl), &e);
    if (!BURROW_FAILED(e) && rest.len != 0)
        e = x509_err("x509: trailing data after CRL");
    BURROW_OUT(err, e);
    return BURROW_FAILED(e) ? NULL : cl;
}

/* -------------------------------------------------------------- creating */

X509_OID_ARCS(x509_oid_extension_subject_key_id, 2, 5, 29, 14);
X509_OID_ARCS(x509_oid_extension_key_usage, 2, 5, 29, 15);
X509_OID_ARCS(x509_oid_extension_basic_constraints, 2, 5, 29, 19);
X509_OID_ARCS(x509_oid_extension_name_constraints, 2, 5, 29, 30);
X509_OID_ARCS(x509_oid_extension_crl_distribution_points, 2, 5, 29, 31);
X509_OID_ARCS(x509_oid_extension_certificate_policies, 2, 5, 29, 32);
X509_OID_ARCS(x509_oid_extension_extended_key_usage, 2, 5, 29, 37);

/* validity */
#define X509_VALIDITY_FIELDS(F, T)                                                     \
    F(T, Time, not_before, NotBefore, "")                                              \
    F(T, Time, not_after, NotAfter, "")
BURROW_STRUCT_AS(X509Validity, X509_VALIDITY_FIELDS);

/* tbsCertificate */
#define X509_TBS_CERTIFICATE_FIELDS(F, T)                                              \
    F(T, Asn1RawContent, raw, Raw, "")                                                 \
    F(T, Int, version, Version, "asn1:\"optional,explicit,default:0,tag:0\"")          \
    F(T, X509BigIntPtr, serial_number, SerialNumber, "")                               \
    F(T, PkixAlgorithmIdentifier, signature_algorithm, SignatureAlgorithm, "")         \
    F(T, Asn1RawValue, issuer, Issuer, "")                                             \
    F(T, X509Validity, validity, Validity, "")                                         \
    F(T, Asn1RawValue, subject, Subject, "")                                           \
    F(T, X509PublicKeyInfo, public_key, PublicKey, "")                                 \
    F(T, Asn1BitString, unique_id, UniqueId, "asn1:\"optional,tag:1\"")                \
    F(T, Asn1BitString, subject_unique_id, SubjectUniqueId, "asn1:\"optional,tag:2\"") \
    F(T, X509Extensions, extensions, Extensions,                                       \
      "asn1:\"omitempty,optional,explicit,tag:3\"")
BURROW_STRUCT_AS(X509TbsCertificate, X509_TBS_CERTIFICATE_FIELDS);

/* certificate */
#define X509_CERT_FIELDS(F, T)                                                         \
    F(T, X509TbsCertificate, tbs_certificate, TBSCertificate, "")                      \
    F(T, PkixAlgorithmIdentifier, signature_algorithm, SignatureAlgorithm, "")         \
    F(T, Asn1BitString, signature_value, SignatureValue, "")
BURROW_STRUCT_AS(X509Cert, X509_CERT_FIELDS);

/* authKeyId */
#define X509_AUTH_KEY_ID_FIELDS(F, T) F(T, Bytes, id, Id, "asn1:\"optional,tag:0\"")
BURROW_STRUCT_AS(X509AuthKeyId, X509_AUTH_KEY_ID_FIELDS);

/* basicConstraints */
#define X509_BASIC_CONSTRAINTS_FIELDS(F, T)                                            \
    F(T, bool, is_ca, IsCA, "asn1:\"optional\"")                                       \
    F(T, Int, max_path_len, MaxPathLen, "asn1:\"optional,default:-1\"")
BURROW_STRUCT_AS(X509BasicConstraints, X509_BASIC_CONSTRAINTS_FIELDS);

/* authorityInfoAccess */
#define X509_AUTHORITY_INFO_ACCESS_FIELDS(F, T)                                        \
    F(T, Asn1ObjectIdentifier, method, Method, "")                                     \
    F(T, Asn1RawValue, location, Location, "")
BURROW_STRUCT_AS(X509AuthorityInfoAccess, X509_AUTHORITY_INFO_ACCESS_FIELDS);
BURROW_SLICE_TYPE(X509AuthorityInfoAccesses, X509AuthorityInfoAccess);

/* distributionPointName */
#define X509_DISTRIBUTION_POINT_NAME_FIELDS(F, T)                                      \
    F(T, X509RawValues, full_name, FullName, "asn1:\"optional,tag:0\"")                \
    F(T, PkixRDNSequence, relative_name, RelativeName, "asn1:\"optional,tag:1\"")
BURROW_STRUCT_AS(X509DistributionPointName, X509_DISTRIBUTION_POINT_NAME_FIELDS);

/* distributionPoint */
#define X509_DISTRIBUTION_POINT_FIELDS(F, T)                                           \
    F(T, X509DistributionPointName, distribution_point, DistributionPoint,             \
      "asn1:\"optional,tag:0\"")                                                       \
    F(T, Asn1BitString, reason, Reason, "asn1:\"optional,tag:1\"")                     \
    F(T, Asn1RawValue, crl_issuer, Cerror, "asn1:\"optional,tag:2\"")
BURROW_STRUCT_AS(X509DistributionPoint, X509_DISTRIBUTION_POINT_FIELDS);
BURROW_SLICE_TYPE(X509DistributionPoints, X509DistributionPoint);

BURROW_SLICE_TYPE(X509ObjectIdentifiers, Asn1ObjectIdentifier);
BURROW_SLICE_TYPE(X509RevokedCertificates, PkixRevokedCertificate);
BURROW_SLICE_TYPE(X509AttributeSets, PkixAttributeTypeAndValueSET);
BURROW_SLICE_TYPE(X509ExtensionLists, X509Extensions);

/* The extensionRequest attribute CreateCertificateRequest adds. */
#define X509_EXTENSION_REQUEST_FIELDS(F, T)                                            \
    F(T, Asn1ObjectIdentifier, type, Type, "")                                         \
    F(T, X509ExtensionLists, value, Value, "asn1:\"set\"")
BURROW_STRUCT_AS(X509ExtensionRequest, X509_EXTENSION_REQUEST_FIELDS);

/* tbsCertificateList */
#define X509_TBS_CERTIFICATE_LIST_FIELDS(F, T)                                         \
    F(T, Asn1RawContent, raw, Raw, "")                                                 \
    F(T, Int, version, Version, "asn1:\"optional,default:0\"")                         \
    F(T, PkixAlgorithmIdentifier, signature, Signature, "")                            \
    F(T, Asn1RawValue, issuer, Issuer, "")                                             \
    F(T, Time, this_update, ThisUpdate, "")                                            \
    F(T, Time, next_update, NextUpdate, "asn1:\"optional\"")                           \
    F(T, X509RevokedCertificates, revoked_certificates, RevokedCertificates,           \
      "asn1:\"optional\"")                                                             \
    F(T, X509Extensions, extensions, Extensions, "asn1:\"tag:0,optional,explicit\"")
BURROW_STRUCT_AS(X509TbsCertificateList, X509_TBS_CERTIFICATE_LIST_FIELDS);

/* certificateList */
#define X509_CERTIFICATE_LIST_FIELDS(F, T)                                             \
    F(T, X509TbsCertificateList, tbs_cert_list, TBSCertList, "")                       \
    F(T, PkixAlgorithmIdentifier, signature_algorithm, SignatureAlgorithm, "")         \
    F(T, Asn1BitString, signature_value, SignatureValue, "")
BURROW_STRUCT_AS(X509CertificateList, X509_CERTIFICATE_LIST_FIELDS);

/* emptyASN1Subject, the DER of an empty Subject. */
static const Byte x509_empty_asn1_subject[] = {0x30, 0};

/* reverseBitsInAByte */
static Byte x509_reverse_bits_in_a_byte(Byte in) {
    Byte b1 = (Byte)(in >> 4 | in << 4);
    Byte b2 = (Byte)((b1 >> 2 & 0x33) | (b1 << 2 & 0xcc));
    return (Byte)((b2 >> 1 & 0x55) | (b2 << 1 & 0xaa));
}

/* asn1BitLength: the length of bit_string up to its last set bit, counting
 * the top bit of a byte as its first, which is ASN.1's order. */
static Int x509_asn1_bit_length(Slice bit_string) {
    Int bit_len = bit_string.len * 8;
    const Byte *p = bit_string.p;
    for (Int i = 0; i < bit_string.len; i++) {
        Byte b = p[bit_string.len - i - 1];
        for (unsigned bit = 0; bit < 8; bit++) {
            if ((b >> bit & 1) == 1)
                return bit_len;
            bit_len--;
        }
    }
    return 0;
}

Int burrow__x509_asn1_bit_length(Slice bit_string) {
    return x509_asn1_bit_length(bit_string);
}

/* oidInExtensions */
static bool x509_oid_in_extensions(Asn1ObjectIdentifier oid, Slice extensions) {
    const PkixExtension *e = extensions.p;
    for (Int i = 0; i < extensions.len; i++)
        if (x509_oid_is(e[i].id, oid))
            return true;
    return false;
}

/* isIA5String, as the error. */
static Error x509_ia5_error(Str s) {
    if (x509_is_ia5(s))
        return BURROW_NO_ERROR;
    return fmt_errorf_v("x509: %q cannot be encoded as an IA5String", s);
}

static Slice x509_str_bytes(Str s) {
    return x509_bytes(s.p, s.len);
}

/* A GeneralName of the given tag holding b. */
static Asn1RawValue x509_general_name(Int tag, Slice b) {
    Asn1RawValue v;
    memset(&v, 0, sizeof v);
    v.cls = ASN1_CLASS_CONTEXT_SPECIFIC;
    v.tag = tag;
    v.bytes = b;
    return v;
}

/* marshalSANs: the contents of a subjectAltName extension. */
Slice burrow__x509_marshal_sans(Alloc *a, Slice dns_names, Slice email_addresses,
                                Slice ip_addresses, Slice uris, Error *err) {
    Slice raw = slice_nil(TYPE_ASN1_RAW_VALUE);
    Error e = BURROW_NO_ERROR;
    const Str *dns = dns_names.p;
    for (Int i = 0; i < dns_names.len; i++) {
        e = x509_ia5_error(dns[i]);
        if (BURROW_FAILED(e))
            goto fail;
        Asn1RawValue v = x509_general_name(X509_NAME_TYPE_DNS, x509_str_bytes(dns[i]));
        if (!x509_push(a, &raw, TYPE_ASN1_RAW_VALUE, &v))
            goto oom;
    }
    const Str *email = email_addresses.p;
    for (Int i = 0; i < email_addresses.len; i++) {
        e = x509_ia5_error(email[i]);
        if (BURROW_FAILED(e))
            goto fail;
        Asn1RawValue v =
            x509_general_name(X509_NAME_TYPE_EMAIL, x509_str_bytes(email[i]));
        if (!x509_push(a, &raw, TYPE_ASN1_RAW_VALUE, &v))
            goto oom;
    }
    const NetIP *ips = ip_addresses.p;
    for (Int i = 0; i < ip_addresses.len; i++) {
        /* An IPv4 address always goes in as 4 bytes when it can. */
        NetIP ip = net_ip_to4(ips[i]);
        if (ip.p == NULL)
            ip = ips[i];
        Asn1RawValue v = x509_general_name(X509_NAME_TYPE_IP, ip);
        if (!x509_push(a, &raw, TYPE_ASN1_RAW_VALUE, &v))
            goto oom;
    }
    Url *const *uri = uris.p;
    for (Int i = 0; i < uris.len; i++) {
        Str s = url_string(uri[i], a);
        e = x509_ia5_error(s);
        if (BURROW_FAILED(e))
            goto fail;
        Asn1RawValue v = x509_general_name(X509_NAME_TYPE_URI, x509_str_bytes(s));
        if (!x509_push(a, &raw, TYPE_ASN1_RAW_VALUE, &v))
            goto oom;
    }
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(X509RawValues), &raw), err);
oom:
    e = burrow_err_out_of_memory;
fail:
    BURROW_OUT(err, e);
    return slice_nil(TYPE_BYTE);
}

/* An extension with id and the DER of v as its value. */
static Error x509_marshal_extension(Alloc *a, PkixExtension *ext,
                                    Asn1ObjectIdentifier id, bool critical, Any v) {
    Error e = BURROW_NO_ERROR;
    ext->id = id;
    ext->critical = critical;
    ext->value = asn1_marshal(a, v, &e);
    return e;
}

/* marshalKeyUsage */
static Error x509_marshal_key_usage(Alloc *a, X509KeyUsage ku, PkixExtension *ext) {
    Byte b[2];
    b[0] = x509_reverse_bits_in_a_byte((Byte)ku);
    b[1] = x509_reverse_bits_in_a_byte((Byte)(ku >> 8));
    Slice bits = x509_bytes(b, b[1] != 0 ? 2 : 1);
    Asn1BitString bs = {bits, x509_asn1_bit_length(bits)};
    return x509_marshal_extension(a, ext, X509_OID(x509_oid_extension_key_usage), true,
                                  BURROW_ANY(TYPE_ASN1_BIT_STRING, &bs));
}

/* marshalExtKeyUsage */
static Error x509_marshal_ext_key_usage(Alloc *a, Slice ext_usages,
                                        Slice unknown_usages, PkixExtension *ext) {
    ext->id = X509_OID(x509_oid_extension_extended_key_usage);
    ext->critical = false;
    Int n = ext_usages.len + unknown_usages.len;
    Slice oids = slice_make(a, TYPE_ASN1_OBJECT_IDENTIFIER, n, n);
    if (oids.p == NULL && n != 0)
        return burrow_err_out_of_memory;
    Asn1ObjectIdentifier *o = oids.p;
    const X509ExtKeyUsage *u = ext_usages.p;
    for (Int i = 0; i < ext_usages.len; i++) {
        if (u[i] < 0 || u[i] >= X509_NEKU)
            return x509_err("x509: unknown extended key usage");
        const X509ExtKeyUsageOID *eku = &x509_ext_key_usage_oids[u[i]];
        /* NOLINTNEXTLINE(clang-analyzer-core.NullDereference) */
        o[i] = (Asn1ObjectIdentifier){(void *)(uintptr_t)eku->oid, eku->len, eku->len,
                                      TYPE_INT};
    }
    if (unknown_usages.len > 0)
        /* NOLINTNEXTLINE(clang-analyzer-unix.cstring.NullArg) */
        memcpy(o + ext_usages.len, unknown_usages.p,
               (size_t)unknown_usages.len * sizeof *o);
    return x509_marshal_extension(a, ext, ext->id, false,
                                  BURROW_ANY(TYPE_OF(X509ObjectIdentifiers), &oids));
}

/* marshalBasicConstraints. A max_path_len of zero means none unless
 * max_path_len_zero says otherwise, and -1 leaves it out. */
static Error x509_marshal_basic_constraints(Alloc *a, bool is_ca, Int max_path_len,
                                            bool max_path_len_zero,
                                            PkixExtension *ext) {
    if (max_path_len == 0 && !max_path_len_zero)
        max_path_len = -1;
    X509BasicConstraints bc = {is_ca, max_path_len};
    return x509_marshal_extension(a, ext,
                                  X509_OID(x509_oid_extension_basic_constraints), true,
                                  BURROW_ANY(TYPE_OF(X509BasicConstraints), &bc));
}

/* The certificatePolicies extension, from policies or, with GODEBUG
 * x509usepolicies=0, from policy_identifiers. */
typedef struct X509Policies {
    Slice policies;
    Slice policy_identifiers;
    bool use_policies;
} X509Policies;

static void x509_policy_oid_body(void *env, CryptobyteBuilder *child) {
    const X509OID *v = env;
    if (v->der.len == 0) {
        cryptobyte_builder_set_error(child,
                                     x509_err("invalid policy object identifier"));
        return;
    }
    cryptobyte_builder_add_bytes(child, v->der);
}

static void x509_policy_body(void *env, CryptobyteBuilder *child) {
    cryptobyte_builder_add_asn1(
        child, CRYPTOBYTE_ASN1_OBJECT_IDENTIFIER,
        BURROW_FN(CryptobyteBuilderContinuation, x509_policy_oid_body, env));
}

static void x509_policy_identifier_body(void *env, CryptobyteBuilder *child) {
    cryptobyte_builder_add_asn1_object_identifier(child,
                                                  *(const Asn1ObjectIdentifier *)env);
}

static void x509_policies_body(void *env, CryptobyteBuilder *child) {
    X509Policies *p = env;
    if (p->use_policies) {
        X509OID *v = p->policies.p;
        for (Int i = 0; i < p->policies.len; i++)
            cryptobyte_builder_add_asn1(
                child, CRYPTOBYTE_ASN1_SEQUENCE,
                BURROW_FN(CryptobyteBuilderContinuation, x509_policy_body, &v[i]));
    } else {
        Asn1ObjectIdentifier *v = p->policy_identifiers.p;
        for (Int i = 0; i < p->policy_identifiers.len; i++)
            cryptobyte_builder_add_asn1(child, CRYPTOBYTE_ASN1_SEQUENCE,
                                        BURROW_FN(CryptobyteBuilderContinuation,
                                                  x509_policy_identifier_body, &v[i]));
    }
}

/* marshalCertificatePolicies */
static Error x509_marshal_certificate_policies(Alloc *a, Slice policies,
                                               Slice policy_identifiers,
                                               PkixExtension *ext) {
    ext->id = X509_OID(x509_oid_extension_certificate_policies);
    ext->critical = false;
    X509Policies p = {policies, policy_identifiers,
                      (x509_debug_load() & X509_DEBUG_POLICY_IDS) == 0};
    CryptobyteBuilder b = cryptobyte_new_builder(a, slice_make(a, TYPE_BYTE, 0, 128));
    cryptobyte_builder_add_asn1(
        &b, CRYPTOBYTE_ASN1_SEQUENCE,
        BURROW_FN(CryptobyteBuilderContinuation, x509_policies_body, &p));
    Error e = BURROW_NO_ERROR;
    ext->value = cryptobyte_builder_bytes(&b, &e);
    return e;
}

/* The bytes of a name constraint and the tag they go under. */
typedef struct X509Tagged {
    CryptobyteAsn1Tag tag;
    Slice bytes;
} X509Tagged;

static void x509_bytes_body(void *env, CryptobyteBuilder *child) {
    cryptobyte_builder_add_bytes(child, *(const Slice *)env);
}

static void x509_tagged_body(void *env, CryptobyteBuilder *child) {
    X509Tagged *t = env;
    cryptobyte_builder_add_asn1(
        child, t->tag,
        BURROW_FN(CryptobyteBuilderContinuation, x509_bytes_body, &t->bytes));
}

/* A GeneralSubtree, which is a SEQUENCE of the name with tag. */
static void x509_add_subtree(CryptobyteBuilder *b, Int tag, Slice bytes) {
    X509Tagged t = {X509_CTX(tag), bytes};
    cryptobyte_builder_add_asn1(
        b, CRYPTOBYTE_ASN1_SEQUENCE,
        BURROW_FN(CryptobyteBuilderContinuation, x509_tagged_body, &t));
}

/* ipAndMask: the network of ip_net masked, then the mask. */
static Error x509_ip_and_mask(Alloc *a, const NetIPNet *ip_net, Slice *out) {
    NetIP masked = net_ip_mask(ip_net->ip, a, ip_net->mask);
    /* Not likely to happen, but it is better to stop someone doing it. */
    if (masked.len == 16 && net_ip_to4(masked).p != NULL)
        return x509_err("x509: IP constraint contained IPv4-mapped IPv6 address with a "
                        "IPv6 mask");
    Int n = masked.len + ip_net->mask.len;
    *out = slice_make(a, TYPE_BYTE, n, n);
    if (out->p == NULL && n != 0)
        return burrow_err_out_of_memory;
    if (masked.len > 0)
        /* NOLINTNEXTLINE(clang-analyzer-unix.cstring.NullArg) */
        memcpy(out->p, masked.p, (size_t)masked.len);
    if (ip_net->mask.len > 0)
        /* NOLINTNEXTLINE(clang-analyzer-unix.cstring.NullArg) */
        memcpy((Byte *)out->p + masked.len, ip_net->mask.p, (size_t)ip_net->mask.len);
    return BURROW_NO_ERROR;
}

/* serialiseConstraints: one GeneralSubtrees of a NameConstraints, without
 * its own tag. */
static Slice x509_serialise_constraints(Alloc *a, Slice dns, Slice ips, Slice emails,
                                        Slice uri_domains, Error *err) {
    CryptobyteBuilder b = cryptobyte_new_builder(a, slice_nil(TYPE_BYTE));
    Error e = BURROW_NO_ERROR;
    const Str *s = dns.p;
    for (Int i = 0; i < dns.len; i++) {
        e = x509_ia5_error(s[i]);
        if (BURROW_FAILED(e))
            goto fail;
        x509_add_subtree(&b, X509_NAME_TYPE_DNS, x509_str_bytes(s[i]));
    }
    NetIPNet *const *ip = ips.p;
    for (Int i = 0; i < ips.len; i++) {
        Slice encoded;
        e = x509_ip_and_mask(a, ip[i], &encoded);
        if (BURROW_FAILED(e))
            goto fail;
        x509_add_subtree(&b, X509_NAME_TYPE_IP, encoded);
    }
    s = emails.p;
    for (Int i = 0; i < emails.len; i++) {
        e = x509_ia5_error(s[i]);
        if (BURROW_FAILED(e))
            goto fail;
        x509_add_subtree(&b, X509_NAME_TYPE_EMAIL, x509_str_bytes(s[i]));
    }
    s = uri_domains.p;
    for (Int i = 0; i < uri_domains.len; i++) {
        e = x509_ia5_error(s[i]);
        if (BURROW_FAILED(e))
            goto fail;
        x509_add_subtree(&b, X509_NAME_TYPE_URI, x509_str_bytes(s[i]));
    }
    return cryptobyte_builder_bytes(&b, err);
fail:
    BURROW_OUT(err, e);
    return slice_nil(TYPE_BYTE);
}

/* The two halves of a NameConstraints. */
typedef struct X509Constraints {
    X509Tagged permitted, excluded;
} X509Constraints;

static void x509_constraints_body(void *env, CryptobyteBuilder *child) {
    X509Constraints *c = env;
    if (c->permitted.bytes.len > 0)
        x509_tagged_body(&c->permitted, child);
    if (c->excluded.bytes.len > 0)
        x509_tagged_body(&c->excluded, child);
}

/* The nameConstraints extension of template. */
static Error x509_marshal_name_constraints(Alloc *a, const X509Certificate *template_,
                                           PkixExtension *ext) {
    ext->id = X509_OID(x509_oid_extension_name_constraints);
    ext->critical = template_->permitted_dns_domains_critical;
    Error e = BURROW_NO_ERROR;
    X509Constraints c;
    c.permitted.tag = X509_CTX_CONS(0);
    c.permitted.bytes = x509_serialise_constraints(
        a, template_->permitted_dns_domains, template_->permitted_ip_ranges,
        template_->permitted_email_addresses, template_->permitted_uri_domains, &e);
    if (BURROW_FAILED(e))
        return e;
    c.excluded.tag = X509_CTX_CONS(1);
    c.excluded.bytes = x509_serialise_constraints(
        a, template_->excluded_dns_domains, template_->excluded_ip_ranges,
        template_->excluded_email_addresses, template_->excluded_uri_domains, &e);
    if (BURROW_FAILED(e))
        return e;
    CryptobyteBuilder b = cryptobyte_new_builder(a, slice_nil(TYPE_BYTE));
    cryptobyte_builder_add_asn1(
        &b, CRYPTOBYTE_ASN1_SEQUENCE,
        BURROW_FN(CryptobyteBuilderContinuation, x509_constraints_body, &c));
    ext->value = cryptobyte_builder_bytes(&b, &e);
    return e;
}

/* The authorityInfoAccess extension, with the OCSP servers and then the
 * issuer URLs. */
static Error x509_marshal_authority_info_access(Alloc *a, Slice ocsp_server,
                                                Slice issuing_certificate_url,
                                                PkixExtension *ext) {
    Slice aia = slice_nil(TYPE_OF(X509AuthorityInfoAccess));
    const Str *s = ocsp_server.p;
    for (Int i = 0; i < ocsp_server.len; i++) {
        X509AuthorityInfoAccess v = {
            X509_OID(x509_oid_authority_info_access_ocsp),
            x509_general_name(X509_NAME_TYPE_URI, x509_str_bytes(s[i]))};
        if (!x509_push(a, &aia, TYPE_OF(X509AuthorityInfoAccess), &v))
            return burrow_err_out_of_memory;
    }
    s = issuing_certificate_url.p;
    for (Int i = 0; i < issuing_certificate_url.len; i++) {
        X509AuthorityInfoAccess v = {
            X509_OID(x509_oid_authority_info_access_issuers),
            x509_general_name(X509_NAME_TYPE_URI, x509_str_bytes(s[i]))};
        if (!x509_push(a, &aia, TYPE_OF(X509AuthorityInfoAccess), &v))
            return burrow_err_out_of_memory;
    }
    return x509_marshal_extension(
        a, ext, X509_OID(x509_oid_extension_authority_info_access), false,
        BURROW_ANY(TYPE_OF(X509AuthorityInfoAccesses), &aia));
}

/* The cRLDistributionPoints extension, one point with a full name for each
 * URL. */
static Error x509_marshal_crl_distribution_points(Alloc *a, Slice points,
                                                  PkixExtension *ext) {
    Int n = points.len;
    Slice dps = slice_make(a, TYPE_OF(X509DistributionPoint), n, n);
    Asn1RawValue *names = BURROW_NEW_N(a, Asn1RawValue, (size_t)n);
    if (dps.p == NULL || names == NULL)
        return burrow_err_out_of_memory;
    X509DistributionPoint *dp = dps.p;
    const Str *s = points.p;
    for (Int i = 0; i < n; i++) {
        memset(&dp[i], 0, sizeof dp[i]);
        names[i] = x509_general_name(X509_NAME_TYPE_URI, x509_str_bytes(s[i]));
        dp[i].distribution_point.full_name =
            slice_from(&names[i], 1, 1, TYPE_ASN1_RAW_VALUE);
    }
    return x509_marshal_extension(
        a, ext, X509_OID(x509_oid_extension_crl_distribution_points), false,
        BURROW_ANY(TYPE_OF(X509DistributionPoints), &dps));
}

/* buildCertExtensions: the extensions template asks for, less the ones
 * extra_extensions has, followed by extra_extensions. */
static Error x509_build_cert_extensions(Alloc *a, const X509Certificate *template_,
                                        bool subject_is_empty, Slice authority_key_id,
                                        Slice subject_key_id, Slice *out) {
    /* The most there can be. */
    PkixExtension ret[10];
    Int n = 0;
    Error e = BURROW_NO_ERROR;
    Slice extra = template_->extra_extensions;
    memset(ret, 0, sizeof ret);

    if (template_->key_usage != 0 &&
        !x509_oid_in_extensions(X509_OID(x509_oid_extension_key_usage), extra)) {
        e = x509_marshal_key_usage(a, template_->key_usage, &ret[n]);
        if (BURROW_FAILED(e))
            return e;
        n++;
    }

    if ((template_->ext_key_usage.len > 0 ||
         template_->unknown_ext_key_usage.len > 0) &&
        !x509_oid_in_extensions(X509_OID(x509_oid_extension_extended_key_usage),
                                extra)) {
        e = x509_marshal_ext_key_usage(a, template_->ext_key_usage,
                                       template_->unknown_ext_key_usage, &ret[n]);
        if (BURROW_FAILED(e))
            return e;
        n++;
    }

    if (template_->basic_constraints_valid &&
        !x509_oid_in_extensions(X509_OID(x509_oid_extension_basic_constraints),
                                extra)) {
        e = x509_marshal_basic_constraints(a, template_->is_ca, template_->max_path_len,
                                           template_->max_path_len_zero, &ret[n]);
        if (BURROW_FAILED(e))
            return e;
        n++;
    }

    if (subject_key_id.len > 0 &&
        !x509_oid_in_extensions(X509_OID(x509_oid_extension_subject_key_id), extra)) {
        e = x509_marshal_extension(a, &ret[n],
                                   X509_OID(x509_oid_extension_subject_key_id), false,
                                   BURROW_ANY(TYPE_BYTES, &subject_key_id));
        if (BURROW_FAILED(e))
            return e;
        n++;
    }

    if (authority_key_id.len > 0 &&
        !x509_oid_in_extensions(X509_OID(x509_oid_extension_authority_key_id), extra)) {
        X509AuthKeyId aki = {authority_key_id};
        e = x509_marshal_extension(a, &ret[n],
                                   X509_OID(x509_oid_extension_authority_key_id), false,
                                   BURROW_ANY(TYPE_OF(X509AuthKeyId), &aki));
        if (BURROW_FAILED(e))
            return e;
        n++;
    }

    if ((template_->ocsp_server.len > 0 ||
         template_->issuing_certificate_url.len > 0) &&
        !x509_oid_in_extensions(X509_OID(x509_oid_extension_authority_info_access),
                                extra)) {
        e = x509_marshal_authority_info_access(
            a, template_->ocsp_server, template_->issuing_certificate_url, &ret[n]);
        if (BURROW_FAILED(e))
            return e;
        n++;
    }

    if ((template_->dns_names.len > 0 || template_->email_addresses.len > 0 ||
         template_->ip_addresses.len > 0 || template_->uris.len > 0) &&
        !x509_oid_in_extensions(X509_OID(x509_oid_extension_subject_alt_name), extra)) {
        ret[n].id = X509_OID(x509_oid_extension_subject_alt_name);
        /* RFC 5280 section 4.2.1.6: with an empty subject the subjectAltName
         * is critical. */
        ret[n].critical = subject_is_empty;
        ret[n].value = burrow__x509_marshal_sans(
            a, template_->dns_names, template_->email_addresses,
            template_->ip_addresses, template_->uris, &e);
        if (BURROW_FAILED(e))
            return e;
        n++;
    }

    bool use_policies = (x509_debug_load() & X509_DEBUG_POLICY_IDS) == 0;
    if (((!use_policies && template_->policy_identifiers.len > 0) ||
         (use_policies && template_->policies.len > 0)) &&
        !x509_oid_in_extensions(X509_OID(x509_oid_extension_certificate_policies),
                                extra)) {
        e = x509_marshal_certificate_policies(a, template_->policies,
                                              template_->policy_identifiers, &ret[n]);
        if (BURROW_FAILED(e))
            return e;
        n++;
    }

    if ((template_->permitted_dns_domains.len > 0 ||
         template_->excluded_dns_domains.len > 0 ||
         template_->permitted_ip_ranges.len > 0 ||
         template_->excluded_ip_ranges.len > 0 ||
         template_->permitted_email_addresses.len > 0 ||
         template_->excluded_email_addresses.len > 0 ||
         template_->permitted_uri_domains.len > 0 ||
         template_->excluded_uri_domains.len > 0) &&
        !x509_oid_in_extensions(X509_OID(x509_oid_extension_name_constraints), extra)) {
        e = x509_marshal_name_constraints(a, template_, &ret[n]);
        if (BURROW_FAILED(e))
            return e;
        n++;
    }

    if (template_->crl_distribution_points.len > 0 &&
        !x509_oid_in_extensions(X509_OID(x509_oid_extension_crl_distribution_points),
                                extra)) {
        e = x509_marshal_crl_distribution_points(a, template_->crl_distribution_points,
                                                 &ret[n]);
        if (BURROW_FAILED(e))
            return e;
        n++;
    }

    /* Another extension here needs ret to grow, and CreateCertificate's list
     * of the template fields it reads in the header. */

    *out = slice_make(a, TYPE_PKIX_EXTENSION, 0, n + extra.len);
    *out = slice_append(a, *out, ret, n);
    *out = slice_append(a, *out, extra.p, extra.len);
    if (out->len != n + extra.len)
        return burrow_err_out_of_memory;
    return BURROW_NO_ERROR;
}

/* buildCSRExtensions */
static Error x509_build_csr_extensions(Alloc *a,
                                       const X509CertificateRequest *template_,
                                       Slice *out) {
    *out = slice_nil(TYPE_PKIX_EXTENSION);
    if ((template_->dns_names.len > 0 || template_->email_addresses.len > 0 ||
         template_->ip_addresses.len > 0 || template_->uris.len > 0) &&
        !x509_oid_in_extensions(X509_OID(x509_oid_extension_subject_alt_name),
                                template_->extra_extensions)) {
        Error e = BURROW_NO_ERROR;
        PkixExtension ext;
        memset(&ext, 0, sizeof ext);
        ext.id = X509_OID(x509_oid_extension_subject_alt_name);
        ext.value = burrow__x509_marshal_sans(
            a, template_->dns_names, template_->email_addresses,
            template_->ip_addresses, template_->uris, &e);
        if (BURROW_FAILED(e))
            return e;
        if (!x509_push(a, out, TYPE_PKIX_EXTENSION, &ext))
            return burrow_err_out_of_memory;
    }
    Int n = out->len + template_->extra_extensions.len;
    *out = slice_append(a, *out, template_->extra_extensions.p,
                        template_->extra_extensions.len);
    return out->len == n ? BURROW_NO_ERROR : burrow_err_out_of_memory;
}

/* The DER of name, raw when there is one. */
static Slice x509_name_bytes(Alloc *a, Slice raw, PkixName name, Error *err) {
    if (raw.len > 0) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return raw;
    }
    PkixRDNSequence rdns = pkix_name_to_rdn_sequence(name, a);
    return asn1_marshal(a, BURROW_ANY(TYPE_PKIX_RDN_SEQUENCE, &rdns), err);
}

/* subjectBytes */
static Slice x509_subject_bytes(Alloc *a, const X509Certificate *cert, Error *err) {
    return x509_name_bytes(a, cert->raw_subject, cert->subject, err);
}

/* The parameters of the AlgorithmIdentifier d is written with. */
static Asn1RawValue x509_details_params(const X509SignatureAlgorithmDetails *d) {
    Asn1RawValue v;
    memset(&v, 0, sizeof v);
    if (d->params == X509_PARAMS_NULL)
        return asn1_null_raw_value;
    if (d->params == X509_PARAMS_PSS)
        v.full_bytes = x509_bytes(d->pss, (Int)sizeof x509_pss_params_sha256);
    return v;
}

/* signingParamsForKey: the algorithm to sign with key, which is sig_algo
 * unless that is zero, and its AlgorithmIdentifier. */
static Error x509_signing_params_for_key(CryptoSigner key,
                                         X509SignatureAlgorithm sig_algo,
                                         X509SignatureAlgorithm *out,
                                         PkixAlgorithmIdentifier *ai) {
    memset(ai, 0, sizeof *ai);
    *out = X509_UNKNOWN_SIGNATURE_ALGORITHM;
    X509PublicKeyAlgorithm pub_type;
    X509SignatureAlgorithm default_algo;
    CryptoPublicKey pub = crypto_signer_public(key);

    if (pub.t == TYPE_RSA_PUBLIC_KEY) {
        pub_type = X509_RSA;
        default_algo = X509_SHA256_WITH_RSA;
    } else if (pub.t == TYPE_ECDSA_PUBLIC_KEY) {
        pub_type = X509_ECDSA;
        EllipticCurve c = ((const EcdsaPublicKey *)pub.data)->curve;
        if (x509_curve_is(c, elliptic_p224()) || x509_curve_is(c, elliptic_p256()))
            default_algo = X509_ECDSA_WITH_SHA256;
        else if (x509_curve_is(c, elliptic_p384()))
            default_algo = X509_ECDSA_WITH_SHA384;
        else if (x509_curve_is(c, elliptic_p521()))
            default_algo = X509_ECDSA_WITH_SHA512;
        else
            return x509_err("x509: unsupported elliptic curve");
    } else if (pub.t == TYPE_ED25519_PUBLIC_KEY) {
        pub_type = X509_ED25519;
        default_algo = X509_PURE_ED25519;
    } else if (pub.t == TYPE_MLDSA_PUBLIC_KEY) {
        pub_type = X509_MLDSA;
        const MldsaParameters *params = mldsa_public_key_parameters(pub.data);
        if (params == mldsa_mldsa44())
            default_algo = X509_MLDSA44;
        else if (params == mldsa_mldsa65())
            default_algo = X509_MLDSA65;
        else if (params == mldsa_mldsa87())
            default_algo = X509_MLDSA87;
        else
            return fmt_errorf_v("x509: unsupported ML-DSA parameters: %s",
                                mldsa_parameters_string(params));
    } else {
        return x509_err("x509: only RSA, ECDSA, ML-DSA and Ed25519 keys supported");
    }

    if (sig_algo == 0)
        sig_algo = default_algo;

    for (Int i = 0; i < X509_NSIG; i++) {
        const X509SignatureAlgorithmDetails *d = &x509_signature_algorithm_details[i];
        if (d->algo != sig_algo)
            continue;
        if (d->pub_key_algo != pub_type)
            return x509_err(
                "x509: requested SignatureAlgorithm does not match private key type");
        if (pub_type == X509_MLDSA && sig_algo != default_algo)
            return x509_err(
                "x509: requested SignatureAlgorithm does not match ML-DSA parameters");
        if (d->hash == CRYPTO_MD5)
            return x509_err("x509: signing with MD5 is not supported");
        *out = sig_algo;
        ai->algorithm = x509_details_oid(d);
        ai->parameters = x509_details_params(d);
        return BURROW_NO_ERROR;
    }
    return x509_err("x509: unknown SignatureAlgorithm");
}

/* signTBS: tbs signed with key, and the signature checked, which catches a
 * signer that does not do what it says. */
static Slice x509_sign_tbs(Alloc *a, Slice tbs, CryptoSigner key,
                           X509SignatureAlgorithm sig_alg, IoReader rand, Error *err) {
    CryptoHash hash_func = 0;
    bool is_pss = false;
    for (Int i = 0; i < X509_NSIG; i++) {
        if (x509_signature_algorithm_details[i].algo == sig_alg) {
            hash_func = x509_signature_algorithm_details[i].hash;
            is_pss = x509_signature_algorithm_details[i].is_rsa_pss;
            break;
        }
    }
    CryptoSignerOpts opts = crypto_hash_as_signer_opts(&hash_func);
    RsaPSSOptions pss = {RSA_PSS_SALT_LENGTH_EQUALS_HASH, hash_func};
    if (is_pss)
        opts = rsa_pss_options_as_signer_opts(&pss);

    Error e = BURROW_NO_ERROR;
    Slice signature = crypto_sign_message(a, key, rand, tbs, opts, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }
    e = x509_check_signature(sig_alg, tbs, signature, crypto_signer_public(key), true);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(
            err, fmt_errorf_v("x509: signature returned by signer is invalid: %w", e));
        return slice_nil(TYPE_BYTE);
    }
    BURROW_OUT(err, BURROW_NO_ERROR);
    return signature;
}

/* key.Equal(x) for the public key types, -1 for a type with no Equal. */
static int x509_public_key_equal(CryptoPublicKey key, CryptoPublicKey x) {
    if (key.t == TYPE_RSA_PUBLIC_KEY)
        return rsa_public_key_equal(key.data, x);
    if (key.t == TYPE_ECDSA_PUBLIC_KEY)
        return ecdsa_public_key_equal(key.data, x);
    if (key.t == TYPE_ED25519_PUBLIC_KEY)
        return ed25519_public_key_equal(*(const Ed25519PublicKey *)key.data, x);
    if (key.t == TYPE_MLDSA_PUBLIC_KEY)
        return mldsa_public_key_equal(key.data, x);
    if (key.t == TYPE_ECDH_PUBLIC_KEY)
        return ecdh_public_key_equal(key.data, x);
    return -1;
}

static Slice x509_fail_nil(Error *err, Error e) {
    BURROW_OUT(err, e);
    return slice_nil(TYPE_BYTE);
}

static Error x509_not_a_signer(void) {
    return x509_err("x509: certificate private key does not implement crypto.Signer");
}

/* A signature as the BIT STRING it is written as. */
static Asn1BitString x509_signature_bits(Slice signature) {
    Asn1BitString bs = {signature, signature.len * 8};
    return bs;
}

Slice x509_create_certificate(Alloc *a, IoReader rand, const X509Certificate *template_,
                              const X509Certificate *parent, Any pub, CryptoSigner priv,
                              Error *err) {
    if (priv.vt == NULL)
        return x509_fail_nil(err, x509_not_a_signer());
    if (rand.vt == NULL)
        rand = crypto_rand_reader;
    Error e = BURROW_NO_ERROR;

    BigInt *serial_number = template_->serial_number;
    if (serial_number == NULL) {
        /* RFC 5280 section 4.1.2.2: positive and at most 20 octets once
         * encoded. With the top bit set the encoding would need a zero byte in
         * front, which makes 21, so it is cleared. */
        Byte serial_bytes[20];
        io_read_full(rand, x509_bytes(serial_bytes, 20), &e);
        if (BURROW_FAILED(e))
            return x509_fail_nil(err, e);
        serial_bytes[0] &= 0x7f;
        serial_number = big_new_int(a, 0);
        if (serial_number == NULL)
            return x509_fail_nil(err, burrow_err_out_of_memory);
        big_int_set_bytes(serial_number, x509_bytes(serial_bytes, 20));
    }

    /* RFC 5280 section 4.1.2.2 wants it positive. It also wants at most 20
     * octets, which a lot of people get wrong, so a longer one is let
     * through. */
    if (big_int_sign(serial_number) == -1)
        return x509_fail_nil(err, x509_err("x509: serial number must be positive"));

    if (template_->basic_constraints_valid && template_->max_path_len < -1)
        return x509_fail_nil(
            err, x509_err("x509: invalid MaxPathLen, must be greater or equal to -1"));

    if (template_->basic_constraints_valid && !template_->is_ca &&
        template_->max_path_len != -1 &&
        (template_->max_path_len != 0 || template_->max_path_len_zero))
        return x509_fail_nil(
            err, x509_err("x509: only CAs are allowed to specify MaxPathLen"));

    X509SignatureAlgorithm signature_algorithm;
    PkixAlgorithmIdentifier algorithm_identifier;
    e = x509_signing_params_for_key(priv, template_->signature_algorithm,
                                    &signature_algorithm, &algorithm_identifier);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    Slice public_key_bytes;
    PkixAlgorithmIdentifier public_key_algorithm;
    if (!x509_marshal_public_key(a, pub, &public_key_bytes, &public_key_algorithm, err))
        return slice_nil(TYPE_BYTE);
    if (x509_public_key_algorithm_from_oid(public_key_algorithm.algorithm) ==
        X509_UNKNOWN_PUBLIC_KEY_ALGORITHM)
        return x509_fail_nil(
            err, fmt_errorf_v("x509: unsupported public key type: %T", pub));

    Slice asn1_issuer = x509_subject_bytes(a, parent, &e);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);
    Slice asn1_subject = x509_subject_bytes(a, template_, &e);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    Slice authority_key_id = template_->authority_key_id;
    if (!bytes_equal(asn1_issuer, asn1_subject) && parent->subject_key_id.len > 0)
        authority_key_id = parent->subject_key_id;

    Slice subject_key_id = template_->subject_key_id;
    if (subject_key_id.len == 0 && template_->is_ca) {
        Byte *id = BURROW_NEW_N(a, Byte, 20);
        if (id == NULL)
            return x509_fail_nil(err, burrow_err_out_of_memory);
        if ((x509_debug_load() & X509_DEBUG_SHA1_SKID) != 0) {
            /* Method 1 of RFC 5280 section 4.2.1.2: the SHA-1 of the bits of
             * subjectPublicKey, without the tag, length and unused bits. */
            Sha1SumRet h = sha1_sum(public_key_bytes);
            memcpy(id, h.a, 20);
        } else {
            /* Method 1 of RFC 7093 section 2: the leftmost 160 bits of the
             * SHA-256 of the same. */
            Sha256Sum256Ret h = sha256_sum256(public_key_bytes);
            memcpy(id, h.a, 20);
        }
        subject_key_id = x509_bytes(id, 20);
    }

    /* The signer's public key has to be the parent's, when the parent has
     * one. */
    CryptoPublicKey priv_pub = crypto_signer_public(priv);
    int eq = x509_public_key_equal(priv_pub, parent->public_key);
    if (eq < 0)
        return x509_fail_nil(
            err,
            x509_err(
                "x509: internal error: supported public key does not implement Equal"));
    if (parent->public_key.t != NULL && eq == 0)
        return x509_fail_nil(
            err,
            x509_err("x509: provided PrivateKey doesn't match parent's PublicKey"));

    Slice extensions;
    e = x509_build_cert_extensions(
        a, template_, bytes_equal(asn1_subject, x509_bytes(x509_empty_asn1_subject, 2)),
        authority_key_id, subject_key_id, &extensions);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    X509Cert cert;
    memset(&cert, 0, sizeof cert);
    X509TbsCertificate *c = &cert.tbs_certificate;
    c->version = 2;
    c->serial_number = serial_number;
    c->signature_algorithm = algorithm_identifier;
    c->issuer.full_bytes = asn1_issuer;
    c->validity.not_before = time_utc(template_->not_before);
    c->validity.not_after = time_utc(template_->not_after);
    c->subject.full_bytes = asn1_subject;
    c->public_key.algorithm = public_key_algorithm;
    c->public_key.public_key = x509_signature_bits(public_key_bytes);
    c->extensions = extensions;

    Slice tbs = asn1_marshal(a, BURROW_ANY(TYPE_OF(X509TbsCertificate), c), &e);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);
    c->raw = tbs;

    Slice signature = x509_sign_tbs(a, tbs, priv, signature_algorithm, rand, &e);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    cert.signature_algorithm = algorithm_identifier;
    cert.signature_value = x509_signature_bits(signature);
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(X509Cert), &cert), err);
}

/* The revocation times of revoked made UTC, which RFC 5280 wants, in a copy
 * from a. */
static Slice x509_revoked_utc(Alloc *a, Slice revoked) {
    Slice out = slice_make(a, TYPE_PKIX_REVOKED_CERTIFICATE, revoked.len, revoked.len);
    if (out.p == NULL && revoked.len != 0)
        return out;
    PkixRevokedCertificate *rc = out.p;
    const PkixRevokedCertificate *in = revoked.p;
    for (Int i = 0; i < revoked.len; i++) {
        rc[i] = in[i];
        rc[i].revocation_time = time_utc(in[i].revocation_time);
    }
    return out;
}

/* The authorityKeyIdentifier extension with id. */
static Error x509_authority_key_id_extension(Alloc *a, Slice id, PkixExtension *ext) {
    X509AuthKeyId aki = {id};
    return x509_marshal_extension(a, ext, X509_OID(x509_oid_extension_authority_key_id),
                                  false, BURROW_ANY(TYPE_OF(X509AuthKeyId), &aki));
}

Slice x509_certificate_create_crl(const X509Certificate *c, Alloc *a, IoReader rand,
                                  CryptoSigner priv, Slice revoked_certs, Time now,
                                  Time expiry, Error *err) {
    if (priv.vt == NULL)
        return x509_fail_nil(err, x509_not_a_signer());
    if (rand.vt == NULL)
        rand = crypto_rand_reader;

    X509SignatureAlgorithm signature_algorithm;
    PkixAlgorithmIdentifier algorithm_identifier;
    Error e = x509_signing_params_for_key(priv, 0, &signature_algorithm,
                                          &algorithm_identifier);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    PkixCertificateList cl;
    memset(&cl, 0, sizeof cl);
    PkixTBSCertificateList *tbs_cert_list = &cl.tbs_cert_list;
    tbs_cert_list->version = 1;
    tbs_cert_list->signature = algorithm_identifier;
    tbs_cert_list->issuer = pkix_name_to_rdn_sequence(c->subject, a);
    tbs_cert_list->this_update = time_utc(now);
    tbs_cert_list->next_update = time_utc(expiry);
    tbs_cert_list->revoked_certificates = x509_revoked_utc(a, revoked_certs);
    if (tbs_cert_list->revoked_certificates.p == NULL && revoked_certs.len != 0)
        return x509_fail_nil(err, burrow_err_out_of_memory);
    tbs_cert_list->extensions = slice_nil(TYPE_PKIX_EXTENSION);

    if (c->subject_key_id.len > 0) {
        PkixExtension aki;
        e = x509_authority_key_id_extension(a, c->subject_key_id, &aki);
        if (BURROW_FAILED(e))
            return x509_fail_nil(err, e);
        if (!x509_push(a, &tbs_cert_list->extensions, TYPE_PKIX_EXTENSION, &aki))
            return x509_fail_nil(err, burrow_err_out_of_memory);
    }

    Slice tbs =
        asn1_marshal(a, BURROW_ANY(TYPE_PKIX_TBS_CERTIFICATE_LIST, tbs_cert_list), &e);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);
    tbs_cert_list->raw = tbs;

    Slice signature = x509_sign_tbs(a, tbs, priv, signature_algorithm, rand, &e);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    cl.signature_algorithm = algorithm_identifier;
    cl.signature_value = x509_signature_bits(signature);
    return asn1_marshal(a, BURROW_ANY(TYPE_PKIX_CERTIFICATE_LIST, &cl), err);
}

/* newRawAttributes: attributes as the RawValues of a request. */
static Error x509_new_raw_attributes(Alloc *a, Slice attributes, Slice *out) {
    *out = slice_nil(TYPE_ASN1_RAW_VALUE);
    Error e = BURROW_NO_ERROR;
    Slice b = asn1_marshal(a, BURROW_ANY(TYPE_OF(X509AttributeSets), &attributes), &e);
    if (BURROW_FAILED(e))
        return e;
    Slice rest = asn1_unmarshal(a, b, BURROW_ANY(TYPE_OF(X509RawValues), out), &e);
    if (BURROW_FAILED(e))
        return e;
    if (rest.len != 0)
        return x509_err("x509: failed to unmarshal raw CSR Attributes");
    return BURROW_NO_ERROR;
}

/* Whether the extensionRequest values in set already have an extension
 * with id. */
static bool x509_attribute_has(const PkixAttributeTypeAndValueSET *set,
                               Asn1ObjectIdentifier id) {
    const Slice *atvs = set->value.p;
    for (Int i = 0; i < set->value.len; i++) {
        const PkixAttributeTypeAndValue *atv = atvs[i].p;
        for (Int j = 0; j < atvs[i].len; j++)
            if (x509_oid_is(atv[j].type, id))
                return true;
    }
    return false;
}

/* The extensions added to the first value of the first extensionRequest in
 * attributes that has one, leaving out the ones it names already. False in
 * *appended when there is no such attribute. */
static Error x509_append_extensions(Alloc *a, Slice attributes, Slice extensions,
                                    bool *appended) {
    *appended = false;
    PkixAttributeTypeAndValueSET *set = attributes.p;
    for (Int i = 0; i < attributes.len; i++) {
        if (!x509_oid_is(set[i].type, X509_OID(x509_oid_extension_request)) ||
            set[i].value.len == 0)
            continue;
        Slice *values = set[i].value.p;
        Slice first = values[0];
        Slice nv = slice_make(a, TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE, 0,
                              first.len + extensions.len);
        nv = slice_append(a, nv, first.p, first.len);
        if (nv.len != first.len)
            return burrow_err_out_of_memory;
        const PkixExtension *ext = extensions.p;
        for (Int j = 0; j < extensions.len; j++) {
            /* What the attributes say for an extension wins. */
            if (x509_attribute_has(&set[i], ext[j].id))
                continue;
            /* An AttributeTypeAndValue has nowhere to put critical. */
            PkixAttributeTypeAndValue atv;
            atv.type = ext[j].id;
            if (!X509_BOX(a, &atv.value, Slice, TYPE_BYTES, &ext[j].value) ||
                !x509_push(a, &nv, TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE, &atv))
                return burrow_err_out_of_memory;
        }
        values[0] = nv;
        *appended = true;
        break;
    }
    return BURROW_NO_ERROR;
}

Slice x509_create_certificate_request(Alloc *a, IoReader rand,
                                      const X509CertificateRequest *template_,
                                      CryptoSigner priv, Error *err) {
    if (priv.vt == NULL)
        return x509_fail_nil(err, x509_not_a_signer());
    if (rand.vt == NULL)
        rand = crypto_rand_reader;

    X509SignatureAlgorithm signature_algorithm;
    PkixAlgorithmIdentifier algorithm_identifier;
    Error e = x509_signing_params_for_key(priv, template_->signature_algorithm,
                                          &signature_algorithm, &algorithm_identifier);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    Slice public_key_bytes;
    PkixAlgorithmIdentifier public_key_algorithm;
    if (!x509_marshal_public_key(a, crypto_signer_public(priv), &public_key_bytes,
                                 &public_key_algorithm, err))
        return slice_nil(TYPE_BYTE);

    Slice extensions;
    e = x509_build_csr_extensions(a, template_, &extensions);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    /* A copy of the attributes, with their lists of values copied as well,
     * since the extensions may go into one of those. */
    Int n = template_->attributes.len;
    Slice attributes = slice_make(a, TYPE_PKIX_ATTRIBUTE_TYPE_AND_VALUE_SET, n, n);
    if (attributes.p == NULL && n != 0)
        return x509_fail_nil(err, burrow_err_out_of_memory);
    PkixAttributeTypeAndValueSET *attr = attributes.p;
    const PkixAttributeTypeAndValueSET *in = template_->attributes.p;
    for (Int i = 0; i < n; i++) {
        attr[i].type = in[i].type;
        Int m = in[i].value.len;
        attr[i].value = slice_make(a, in[i].value.elem, m, m);
        if (attr[i].value.p == NULL && m != 0)
            return x509_fail_nil(err, burrow_err_out_of_memory);
        if (m > 0)
            memcpy(attr[i].value.p, in[i].value.p, (size_t)m * sizeof(Slice));
    }

    bool extensions_appended = false;
    if (extensions.len > 0) {
        e = x509_append_extensions(a, attributes, extensions, &extensions_appended);
        if (BURROW_FAILED(e))
            return x509_fail_nil(err, e);
    }

    Slice raw_attributes;
    e = x509_new_raw_attributes(a, attributes, &raw_attributes);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    /* Without an attribute to put them in, the extensions get one of their
     * own. */
    if (extensions.len > 0 && !extensions_appended) {
        X509ExtensionRequest req;
        req.type = X509_OID(x509_oid_extension_request);
        req.value = slice_from(&extensions, 1, 1, TYPE_OF(X509Extensions));
        Slice b = asn1_marshal(a, BURROW_ANY(TYPE_OF(X509ExtensionRequest), &req), &e);
        if (BURROW_FAILED(e))
            return x509_fail_nil(
                err,
                fmt_errorf_v("x509: failed to serialise extensions attribute: %v", e));
        Asn1RawValue raw_value;
        memset(&raw_value, 0, sizeof raw_value);
        (void)asn1_unmarshal(a, b, BURROW_ANY(TYPE_ASN1_RAW_VALUE, &raw_value), &e);
        if (BURROW_FAILED(e))
            return x509_fail_nil(err, e);
        if (!x509_push(a, &raw_attributes, TYPE_ASN1_RAW_VALUE, &raw_value))
            return x509_fail_nil(err, burrow_err_out_of_memory);
    }

    Slice asn1_subject =
        x509_name_bytes(a, template_->raw_subject, template_->subject, &e);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    X509Csr cr;
    memset(&cr, 0, sizeof cr);
    X509TbsCsr *tbs_csr = &cr.tbs_csr;
    tbs_csr->version = 0; /* PKCS #10, RFC 2986 */
    tbs_csr->subject.full_bytes = asn1_subject;
    tbs_csr->public_key.algorithm = public_key_algorithm;
    tbs_csr->public_key.public_key = x509_signature_bits(public_key_bytes);
    tbs_csr->raw_attributes = raw_attributes;

    Slice tbs = asn1_marshal(a, BURROW_ANY(TYPE_OF(X509TbsCsr), tbs_csr), &e);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);
    tbs_csr->raw = tbs;

    Slice signature = x509_sign_tbs(a, tbs, priv, signature_algorithm, rand, &e);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    cr.signature_algorithm.algorithm = algorithm_identifier.algorithm;
    cr.signature_algorithm.parameters = algorithm_identifier.parameters;
    cr.signature_value = x509_signature_bits(signature);
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(X509Csr), &cr), err);
}

/* The revokedCertificates of a CRL from the entries of template, with a
 * reasonCode extension for each reason_code that is not zero. */
static Error x509_revoked_from_entries(Alloc *a, Slice entries, Slice *out) {
    *out = slice_make(a, TYPE_PKIX_REVOKED_CERTIFICATE, entries.len, entries.len);
    if (out->p == NULL && entries.len != 0)
        return burrow_err_out_of_memory;
    PkixRevokedCertificate *rc = out->p;
    const X509RevocationListEntry *rce = entries.p;
    for (Int i = 0; i < entries.len; i++) {
        if (rce[i].serial_number == NULL)
            return x509_err(
                "x509: template contains entry with nil SerialNumber field");
        if (time_is_zero(rce[i].revocation_time))
            return x509_err(
                "x509: template contains entry with zero RevocationTime field");
        memset(&rc[i], 0, sizeof rc[i]);
        rc[i].serial_number = rce[i].serial_number;
        rc[i].revocation_time = time_utc(rce[i].revocation_time);

        /* The extra extensions, but not a reasonCode, which is made here so
         * that it is right. */
        Slice exts = slice_make(a, TYPE_PKIX_EXTENSION, 0, rce[i].extra_extensions.len);
        const PkixExtension *ext = rce[i].extra_extensions.p;
        for (Int j = 0; j < rce[i].extra_extensions.len; j++) {
            if (x509_oid_is(ext[j].id, X509_OID(x509_oid_extension_reason_code)))
                return x509_err("x509: template contains entry with ReasonCode "
                                "ExtraExtension; use ReasonCode field instead");
            if (!x509_push(a, &exts, TYPE_PKIX_EXTENSION, &ext[j]))
                return burrow_err_out_of_memory;
        }

        /* RFC 5280 section 5.3.1: only a reason that is not zero is
         * written. */
        if (rce[i].reason_code != 0) {
            Asn1Enumerated reason = rce[i].reason_code;
            PkixExtension r;
            Error e = x509_marshal_extension(
                a, &r, X509_OID(x509_oid_extension_reason_code), false,
                BURROW_ANY(TYPE_ASN1_ENUMERATED, &reason));
            if (BURROW_FAILED(e))
                return e;
            if (!x509_push(a, &exts, TYPE_PKIX_EXTENSION, &r))
                return burrow_err_out_of_memory;
        }

        if (exts.len > 0)
            rc[i].extensions = exts;
    }
    return BURROW_NO_ERROR;
}

Slice x509_create_revocation_list(Alloc *a, IoReader rand,
                                  const X509RevocationList *template_,
                                  const X509Certificate *issuer, CryptoSigner priv,
                                  Error *err) {
    if (template_ == NULL)
        return x509_fail_nil(err, x509_err("x509: template can not be nil"));
    if (issuer == NULL)
        return x509_fail_nil(err, x509_err("x509: issuer can not be nil"));
    if ((issuer->key_usage & X509_KEY_USAGE_CRL_SIGN) == 0)
        return x509_fail_nil(
            err, x509_err("x509: issuer must have the crlSign key usage bit set"));
    if (issuer->subject_key_id.len == 0)
        return x509_fail_nil(
            err,
            x509_err(
                "x509: issuer certificate doesn't contain a subject key identifier"));
    if (time_before(template_->next_update, template_->this_update))
        return x509_fail_nil(
            err, x509_err("x509: template.ThisUpdate is after template.NextUpdate"));
    if (template_->number == NULL)
        return x509_fail_nil(err, x509_err("x509: template contains nil Number field"));
    if (rand.vt == NULL)
        rand = crypto_rand_reader;

    X509SignatureAlgorithm signature_algorithm;
    PkixAlgorithmIdentifier algorithm_identifier;
    Error e = x509_signing_params_for_key(priv, template_->signature_algorithm,
                                          &signature_algorithm, &algorithm_identifier);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    /* The deprecated revoked_certificates are only used when there are no
     * entries. */
    Slice revoked_certs;
    if (template_->revoked_certificates.len > 0 &&
        template_->revoked_certificate_entries.len == 0) {
        revoked_certs = x509_revoked_utc(a, template_->revoked_certificates);
        if (revoked_certs.p == NULL)
            return x509_fail_nil(err, burrow_err_out_of_memory);
    } else {
        e = x509_revoked_from_entries(a, template_->revoked_certificate_entries,
                                      &revoked_certs);
        if (BURROW_FAILED(e))
            return x509_fail_nil(err, e);
    }

    PkixExtension exts[2];
    memset(exts, 0, sizeof exts);
    e = x509_authority_key_id_extension(a, issuer->subject_key_id, &exts[0]);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    Slice num_bytes = big_int_bytes(template_->number, a);
    if (num_bytes.len > 20 ||
        (num_bytes.len == 20 && (((const Byte *)num_bytes.p)[0] & 0x80) != 0))
        return x509_fail_nil(err, x509_err("x509: CRL number exceeds 20 octets"));
    BigInt *number = template_->number;
    e = x509_marshal_extension(a, &exts[1], X509_OID(x509_oid_extension_crl_number),
                               false, BURROW_ANY(TYPE_OF(X509BigIntPtr), &number));
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    /* The issuer's own subject DER, when it has one. */
    Slice issuer_subject = x509_subject_bytes(a, issuer, &e);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    X509CertificateList cl;
    memset(&cl, 0, sizeof cl);
    X509TbsCertificateList *tbs_cert_list = &cl.tbs_cert_list;
    tbs_cert_list->version = 1; /* v2 */
    tbs_cert_list->signature = algorithm_identifier;
    tbs_cert_list->issuer.full_bytes = issuer_subject;
    tbs_cert_list->this_update = time_utc(template_->this_update);
    tbs_cert_list->next_update = time_utc(template_->next_update);
    Int n = 2 + template_->extra_extensions.len;
    tbs_cert_list->extensions = slice_make(a, TYPE_PKIX_EXTENSION, 0, n);
    tbs_cert_list->extensions = slice_append(a, tbs_cert_list->extensions, exts, 2);
    tbs_cert_list->extensions =
        slice_append(a, tbs_cert_list->extensions, template_->extra_extensions.p,
                     template_->extra_extensions.len);
    if (tbs_cert_list->extensions.len != n)
        return x509_fail_nil(err, burrow_err_out_of_memory);
    if (revoked_certs.len > 0)
        tbs_cert_list->revoked_certificates = revoked_certs;

    Slice tbs =
        asn1_marshal(a, BURROW_ANY(TYPE_OF(X509TbsCertificateList), tbs_cert_list), &e);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);
    /* Set so the list is only marshalled once, here and in cl below. */
    tbs_cert_list->raw = tbs;

    Slice signature = x509_sign_tbs(a, tbs, priv, signature_algorithm, rand, &e);
    if (BURROW_FAILED(e))
        return x509_fail_nil(err, e);

    cl.signature_algorithm = algorithm_identifier;
    cl.signature_value = x509_signature_bits(signature);
    return asn1_marshal(a, BURROW_ANY(TYPE_OF(X509CertificateList), &cl), err);
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
