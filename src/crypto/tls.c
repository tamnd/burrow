/* crypto/tls: the names of versions, key exchanges, signature schemes and
 * alerts, the GODEBUG settings, and the defaults that do not depend on a
 * Config.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/atomic.h"
#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/declare.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/mem.h"
#include "burrow/pal.h"
#include "burrow/type.h"

#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------- the protocol */

const Byte burrow__tls_hello_retry_request_random[32] = {
    0xCF, 0x21, 0xAD, 0x74, 0xE5, 0x9A, 0x61, 0x11, 0xBE, 0x1D, 0x8C,
    0x02, 0x1E, 0x65, 0xB8, 0x91, 0xC2, 0xA2, 0x11, 0x16, 0x7A, 0xBB,
    0x8C, 0x5E, 0x07, 0x9E, 0x09, 0xE2, 0xC8, 0xA8, 0x33, 0x9C,
};

bool burrow__tls_is_tls13_only_key_exchange(TlsCurveID curve) {
    switch (curve) {
    case TLS_X25519MLKEM768:
    case TLS_SEC_P256_R1_MLKEM768:
    case TLS_SEC_P384_R1_MLKEM1024:
    case TLS_MLKEM1024:
        return true;
    default:
        return false;
    }
}

bool burrow__tls_is_pq_key_exchange(TlsCurveID curve) {
    switch (curve) {
    case TLS_X25519MLKEM768:
    case TLS_SEC_P256_R1_MLKEM768:
    case TLS_SEC_P384_R1_MLKEM1024:
    case TLS_MLKEM1024:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------- names */

/* prefix, v in decimal and suffix, from a, or an empty string when a is out
 * of memory. */
static Str tls_number_string(Alloc *a, const char *prefix, int64_t v,
                             const char *suffix) {
    char num[24];
    Int nl = 0;
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    do {
        num[sizeof num - 1 - (size_t)nl++] = (char)('0' + (int)(u % 10));
        u /= 10;
    } while (u != 0);
    if (v < 0)
        num[sizeof num - 1 - (size_t)nl++] = '-';
    size_t pl = strlen(prefix), sl = strlen(suffix);
    size_t len = pl + (size_t)nl + sl;
    Byte *p = (Byte *)mem_alloc_nozero(a, len, 1);
    if (p == NULL)
        return str_from_cstr("");
    memcpy(p, prefix, pl);
    memcpy(p + pl, num + sizeof num - (size_t)nl, (size_t)nl);
    memcpy(p + pl + (size_t)nl, suffix, sl);
    return str_from_bytes(p, (Int)len);
}

/* fmt.Sprintf("0x%04X", v), from a. */
static Str tls_hex4_string(Alloc *a, uint16_t v) {
    static const char digits[] = "0123456789ABCDEF";
    Byte *p = (Byte *)mem_alloc_nozero(a, 6, 1);
    if (p == NULL)
        return str_from_cstr("");
    p[0] = '0';
    p[1] = 'x';
    for (int i = 0; i < 4; i++)
        p[2 + i] = (Byte)digits[(v >> (12 - 4 * i)) & 0xF];
    return str_from_bytes(p, 6);
}

Str burrow__tls_hex4_string(Alloc *a, uint16_t v) {
    return tls_hex4_string(a, v);
}

Str tls_version_name(Alloc *a, uint16_t version) {
    switch (version) {
    case TLS_VERSION_SSL30:
        return BURROW_S("SSLv3");
    case TLS_VERSION_TLS10:
        return BURROW_S("TLS 1.0");
    case TLS_VERSION_TLS11:
        return BURROW_S("TLS 1.1");
    case TLS_VERSION_TLS12:
        return BURROW_S("TLS 1.2");
    case TLS_VERSION_TLS13:
        return BURROW_S("TLS 1.3");
    default:
        return tls_hex4_string(a, version);
    }
}

Str tls_curve_id_string(TlsCurveID i, Alloc *a) {
    switch (i) {
    case TLS_CURVE_P256:
        return BURROW_S("CurveP256");
    case TLS_CURVE_P384:
        return BURROW_S("CurveP384");
    case TLS_CURVE_P521:
        return BURROW_S("CurveP521");
    case TLS_X25519:
        return BURROW_S("X25519");
    case TLS_MLKEM1024:
        return BURROW_S("MLKEM1024");
    case TLS_SEC_P256_R1_MLKEM768:
        return BURROW_S("SecP256r1MLKEM768");
    case TLS_X25519MLKEM768:
        return BURROW_S("X25519MLKEM768");
    case TLS_SEC_P384_R1_MLKEM1024:
        return BURROW_S("SecP384r1MLKEM1024");
    default:
        return tls_number_string(a, "CurveID(", i, ")");
    }
}

Str tls_signature_scheme_string(TlsSignatureScheme i, Alloc *a) {
    switch (i) {
    case TLS_PKCS1_WITH_SHA1:
        return BURROW_S("PKCS1WithSHA1");
    case TLS_ECDSA_WITH_SHA1:
        return BURROW_S("ECDSAWithSHA1");
    case TLS_PKCS1_WITH_SHA256:
        return BURROW_S("PKCS1WithSHA256");
    case TLS_ECDSA_WITH_P256_AND_SHA256:
        return BURROW_S("ECDSAWithP256AndSHA256");
    case TLS_PKCS1_WITH_SHA384:
        return BURROW_S("PKCS1WithSHA384");
    case TLS_ECDSA_WITH_P384_AND_SHA384:
        return BURROW_S("ECDSAWithP384AndSHA384");
    case TLS_PKCS1_WITH_SHA512:
        return BURROW_S("PKCS1WithSHA512");
    case TLS_ECDSA_WITH_P521_AND_SHA512:
        return BURROW_S("ECDSAWithP521AndSHA512");
    case TLS_PSS_WITH_SHA256:
        return BURROW_S("PSSWithSHA256");
    case TLS_PSS_WITH_SHA384:
        return BURROW_S("PSSWithSHA384");
    case TLS_PSS_WITH_SHA512:
        return BURROW_S("PSSWithSHA512");
    case TLS_ED25519:
        return BURROW_S("Ed25519");
    case TLS_MLDSA44:
        return BURROW_S("MLDSA44");
    case TLS_MLDSA65:
        return BURROW_S("MLDSA65");
    case TLS_MLDSA87:
        return BURROW_S("MLDSA87");
    default:
        return tls_number_string(a, "SignatureScheme(", i, ")");
    }
}

Str tls_client_auth_type_string(TlsClientAuthType i, Alloc *a) {
    switch (i) {
    case TLS_NO_CLIENT_CERT:
        return BURROW_S("NoClientCert");
    case TLS_REQUEST_CLIENT_CERT:
        return BURROW_S("RequestClientCert");
    case TLS_REQUIRE_ANY_CLIENT_CERT:
        return BURROW_S("RequireAnyClientCert");
    case TLS_VERIFY_CLIENT_CERT_IF_GIVEN:
        return BURROW_S("VerifyClientCertIfGiven");
    case TLS_REQUIRE_AND_VERIFY_CLIENT_CERT:
        return BURROW_S("RequireAndVerifyClientCert");
    default:
        return tls_number_string(a, "ClientAuthType(", i, ")");
    }
}

/* ------------------------------------------------------------------ alerts */

Str burrow__tls_alert_text(uint8_t e) {
    switch (e) {
    case TLS_ALERT_CLOSE_NOTIFY:
        return BURROW_S("close notify");
    case TLS_ALERT_UNEXPECTED_MESSAGE:
        return BURROW_S("unexpected message");
    case TLS_ALERT_BAD_RECORD_MAC:
        return BURROW_S("bad record MAC");
    case TLS_ALERT_DECRYPTION_FAILED:
        return BURROW_S("decryption failed");
    case TLS_ALERT_RECORD_OVERFLOW:
        return BURROW_S("record overflow");
    case TLS_ALERT_DECOMPRESSION_FAILURE:
        return BURROW_S("decompression failure");
    case TLS_ALERT_HANDSHAKE_FAILURE:
        return BURROW_S("handshake failure");
    case TLS_ALERT_BAD_CERTIFICATE:
        return BURROW_S("bad certificate");
    case TLS_ALERT_UNSUPPORTED_CERTIFICATE:
        return BURROW_S("unsupported certificate");
    case TLS_ALERT_CERTIFICATE_REVOKED:
        return BURROW_S("revoked certificate");
    case TLS_ALERT_CERTIFICATE_EXPIRED:
        return BURROW_S("expired certificate");
    case TLS_ALERT_CERTIFICATE_UNKNOWN:
        return BURROW_S("unknown certificate");
    case TLS_ALERT_ILLEGAL_PARAMETER:
        return BURROW_S("illegal parameter");
    case TLS_ALERT_UNKNOWN_CA:
        return BURROW_S("unknown certificate authority");
    case TLS_ALERT_ACCESS_DENIED:
        return BURROW_S("access denied");
    case TLS_ALERT_DECODE_ERROR:
        return BURROW_S("error decoding message");
    case TLS_ALERT_DECRYPT_ERROR:
        return BURROW_S("error decrypting message");
    case TLS_ALERT_EXPORT_RESTRICTION:
        return BURROW_S("export restriction");
    case TLS_ALERT_PROTOCOL_VERSION:
        return BURROW_S("protocol version not supported");
    case TLS_ALERT_INSUFFICIENT_SECURITY:
        return BURROW_S("insufficient security level");
    case TLS_ALERT_INTERNAL_ERROR:
        return BURROW_S("internal error");
    case TLS_ALERT_INAPPROPRIATE_FALLBACK:
        return BURROW_S("inappropriate fallback");
    case TLS_ALERT_USER_CANCELED:
        return BURROW_S("user canceled");
    case TLS_ALERT_NO_RENEGOTIATION:
        return BURROW_S("no renegotiation");
    case TLS_ALERT_MISSING_EXTENSION:
        return BURROW_S("missing extension");
    case TLS_ALERT_UNSUPPORTED_EXTENSION:
        return BURROW_S("unsupported extension");
    case TLS_ALERT_CERTIFICATE_UNOBTAINABLE:
        return BURROW_S("certificate unobtainable");
    case TLS_ALERT_UNRECOGNIZED_NAME:
        return BURROW_S("unrecognized name");
    case TLS_ALERT_BAD_CERTIFICATE_STATUS_RESPONSE:
        return BURROW_S("bad certificate status response");
    case TLS_ALERT_BAD_CERTIFICATE_HASH_VALUE:
        return BURROW_S("bad certificate hash value");
    case TLS_ALERT_UNKNOWN_PSK_IDENTITY:
        return BURROW_S("unknown PSK identity");
    case TLS_ALERT_CERTIFICATE_REQUIRED:
        return BURROW_S("certificate required");
    case TLS_ALERT_NO_APPLICATION_PROTOCOL:
        return BURROW_S("no application protocol");
    case TLS_ALERT_ECH_REQUIRED:
        return BURROW_S("encrypted client hello required");
    default:
        return BURROW_S("");
    }
}

/* alert.String: "tls: " and the text, or "tls: alert(N)". Writes it to p, or
 * only counts it when p is NULL, and returns its length. */
static Int tls_alert_message(Byte *p, uint8_t e) {
    static const char prefix[] = "tls: ";
    Str text = burrow__tls_alert_text(e);
    char num[3];
    Int nl = 0;
    if (text.len == 0) {
        unsigned u = e;
        do {
            num[2 - nl++] = (char)('0' + (int)(u % 10));
            u /= 10;
        } while (u != 0);
    }
    Int len = (Int)sizeof prefix - 1 + (text.len > 0 ? text.len : 7 + nl);
    if (p == NULL)
        return len;
    memcpy(p, prefix, sizeof prefix - 1);
    p += sizeof prefix - 1;
    if (text.len > 0) {
        memcpy(p, text.p, (size_t)text.len);
    } else {
        memcpy(p, "alert(", 6);
        memcpy(p + 6, num + 3 - nl, (size_t)nl);
        p[6 + nl] = ')';
    }
    return len;
}

/* The alert first, so the data pointer of the Error is a pointer to a
 * TlsAlertError and errors_as hands it straight back. */
typedef struct TlsAlertBox {
    TlsAlertError e;
    Str message;
} TlsAlertBox;

static const Type tls_alert_error_desc = {
    {(const Byte *)"AlertError", 10},
    {(const Byte *)"crypto/tls", 10},
    KIND_UINT8,
    (uint32_t)sizeof(TlsAlertError),
    (uint16_t)_Alignof(TlsAlertError),
    0,
    0,
    NULL,
    NULL,
    NULL,
    NULL,
    0,
    0x746c6165U, /* "tlae" */
    NULL,
};

const Type *const TYPE_TLS_ALERT_ERROR = &tls_alert_error_desc;

static Str tls_alert_error_text(const void *self) {
    return ((const TlsAlertBox *)self)->message;
}

static Error tls_alert_error_clone(const void *self, Alloc *a) {
    return tls_alert_error_as_error(((const TlsAlertBox *)self)->e, a);
}

static const ErrorVT tls_alert_error_vt = {
    .self_type = &tls_alert_error_desc,
    .message = tls_alert_error_text,
    .clone = tls_alert_error_clone,
};

Error tls_alert_error_as_error(TlsAlertError e, Alloc *a) {
    Int mlen = tls_alert_message(NULL, e);
    TlsAlertBox *b = (TlsAlertBox *)mem_alloc_nozero(
        a, sizeof(TlsAlertBox) + (size_t)mlen, _Alignof(TlsAlertBox));
    if (b == NULL)
        return burrow_err_out_of_memory;
    Byte *p = (Byte *)(b + 1);
    tls_alert_message(p, e);
    b->e = e;
    b->message = str_from_bytes(p, mlen);
    return (Error){&tls_alert_error_vt, b};
}

Str tls_alert_error_error(TlsAlertError e, Alloc *a) {
    Int mlen = tls_alert_message(NULL, e);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)mlen, 1);
    if (p == NULL)
        return BURROW_STR_EMPTY;
    tls_alert_message(p, e);
    return str_from_bytes(p, mlen);
}

/* ----------------------------------------------------------------- GODEBUG */

enum {
    TLS_DEBUG_KNOWN = 1 << 0,
    TLS_DEBUG_MLKEM_OFF = 1 << 1,      /* tlsmlkem=0 */
    TLS_DEBUG_SECP_MLKEM_OFF = 1 << 2, /* tlssecpmlkem=0 */
    TLS_DEBUG_SHA1_ON = 1 << 3,        /* tlssha1=1 */
};

static uint32_t tls_debug_flags;

/* Whether GODEBUG in env has key=value, the last one counting when key is
 * there twice. */
static bool tls_godebug_is(const char *env, const char *key, const char *value) {
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
static uint32_t tls_debug_parse(const char *value) {
    uint32_t f = TLS_DEBUG_KNOWN;
    if (tls_godebug_is(value, "tlsmlkem", "0"))
        f |= TLS_DEBUG_MLKEM_OFF;
    if (tls_godebug_is(value, "tlssecpmlkem", "0"))
        f |= TLS_DEBUG_SECP_MLKEM_OFF;
    if (tls_godebug_is(value, "tlssha1", "1"))
        f |= TLS_DEBUG_SHA1_ON;
    return f;
}

static uint32_t tls_debug_load(void) {
    uint32_t f = burrow__atomic_load_relaxed_u32(&tls_debug_flags);
    if ((f & TLS_DEBUG_KNOWN) != 0)
        return f;
    f = TLS_DEBUG_KNOWN;
    for (const char *const *env = pal_environ(); env != NULL && *env != NULL; env++) {
        if (strncmp(*env, "GODEBUG=", 8) == 0) {
            f = tls_debug_parse(*env + 8);
            break;
        }
    }
    burrow__atomic_store_relaxed_u32(&tls_debug_flags, f);
    return f;
}

bool burrow__tls_godebug_sha1(void) {
    return (tls_debug_load() & TLS_DEBUG_SHA1_ON) != 0;
}

void burrow__tls_godebug_set(const char *value) {
    burrow__atomic_store_relaxed_u32(&tls_debug_flags,
                                     value != NULL ? tls_debug_parse(value) : 0);
}

/* ---------------------------------------------------------------- defaults */

bool burrow__tls_default_curve_enabled(TlsCurveID c) {
    switch (c) {
    case TLS_X25519:
    case TLS_CURVE_P256:
    case TLS_CURVE_P384:
    case TLS_CURVE_P521:
        return true;
    case TLS_X25519MLKEM768:
        return (tls_debug_load() & TLS_DEBUG_MLKEM_OFF) == 0;
    case TLS_SEC_P256_R1_MLKEM768:
    case TLS_SEC_P384_R1_MLKEM1024:
        return (tls_debug_load() & (TLS_DEBUG_MLKEM_OFF | TLS_DEBUG_SECP_MLKEM_OFF)) ==
               0;
    default:
        return false;
    }
}

const TlsCurveID burrow__tls_curve_preference_order[8] = {
    TLS_X25519MLKEM768,
    TLS_SEC_P256_R1_MLKEM768,
    TLS_SEC_P384_R1_MLKEM1024,
    TLS_MLKEM1024,
    TLS_X25519,
    TLS_CURVE_P256,
    TLS_CURVE_P384,
    TLS_CURVE_P521,
};

const TlsSignatureScheme burrow__tls_default_supported_signature_algorithms[15] = {
    TLS_MLDSA44,
    TLS_MLDSA65,
    TLS_MLDSA87,
    TLS_PSS_WITH_SHA256,
    TLS_ECDSA_WITH_P256_AND_SHA256,
    TLS_ED25519,
    TLS_PSS_WITH_SHA384,
    TLS_PSS_WITH_SHA512,
    TLS_PKCS1_WITH_SHA256,
    TLS_PKCS1_WITH_SHA384,
    TLS_PKCS1_WITH_SHA512,
    TLS_ECDSA_WITH_P384_AND_SHA384,
    TLS_ECDSA_WITH_P521_AND_SHA512,
    TLS_PKCS1_WITH_SHA1,
    TLS_ECDSA_WITH_SHA1,
};

/* -------------------------------------------------------- signature schemes */

bool burrow__tls_type_and_hash_from_signature_scheme(TlsSignatureScheme s,
                                                     uint8_t *sig_type,
                                                     CryptoHash *hash, Error *err) {
    uint8_t t;
    CryptoHash h;
    switch (s) {
    case TLS_PKCS1_WITH_SHA1:
    case TLS_PKCS1_WITH_SHA256:
    case TLS_PKCS1_WITH_SHA384:
    case TLS_PKCS1_WITH_SHA512:
        t = TLS_SIGNATURE_PKCS1V15;
        break;
    case TLS_PSS_WITH_SHA256:
    case TLS_PSS_WITH_SHA384:
    case TLS_PSS_WITH_SHA512:
        t = TLS_SIGNATURE_RSAPSS;
        break;
    case TLS_ECDSA_WITH_SHA1:
    case TLS_ECDSA_WITH_P256_AND_SHA256:
    case TLS_ECDSA_WITH_P384_AND_SHA384:
    case TLS_ECDSA_WITH_P521_AND_SHA512:
        t = TLS_SIGNATURE_ECDSA;
        break;
    case TLS_ED25519:
        t = TLS_SIGNATURE_ED25519;
        break;
    case TLS_MLDSA44:
    case TLS_MLDSA65:
    case TLS_MLDSA87:
        t = TLS_SIGNATURE_MLDSA;
        break;
    default:
        goto unsupported;
    }
    switch (s) {
    case TLS_PKCS1_WITH_SHA1:
    case TLS_ECDSA_WITH_SHA1:
        h = CRYPTO_SHA1;
        break;
    case TLS_PKCS1_WITH_SHA256:
    case TLS_PSS_WITH_SHA256:
    case TLS_ECDSA_WITH_P256_AND_SHA256:
        h = CRYPTO_SHA256;
        break;
    case TLS_PKCS1_WITH_SHA384:
    case TLS_PSS_WITH_SHA384:
    case TLS_ECDSA_WITH_P384_AND_SHA384:
        h = CRYPTO_SHA384;
        break;
    case TLS_PKCS1_WITH_SHA512:
    case TLS_PSS_WITH_SHA512:
    case TLS_ECDSA_WITH_P521_AND_SHA512:
        h = CRYPTO_SHA512;
        break;
    default:
        h = TLS_DIRECT_SIGNING;
        break;
    }
    BURROW_OUT(sig_type, t);
    BURROW_OUT(hash, h);
    return true;

unsupported:
    BURROW_OUT(sig_type, 0);
    BURROW_OUT(hash, 0);
    if (err != NULL)
        *err = fmt_errorf_v("unsupported signature algorithm: %s",
                            tls_signature_scheme_string(s, error_allocator()));
    return false;
}

bool burrow__tls_is_disabled_signature_algorithm(uint16_t version, TlsSignatureScheme s,
                                                 bool is_cert) {
    switch (s) {
    case TLS_MLDSA44:
    case TLS_MLDSA65:
    case TLS_MLDSA87:
        /* ML-DSA codepoints are only defined for TLS 1.3. */
        if (version < TLS_VERSION_TLS13)
            return true;
        break;
    default:
        break;
    }

    /* For the _cert extension we include all algorithms, including SHA-1 and
     * PKCS#1 v1.5, because it's more likely that something on our side will be
     * willing to accept a *-with-SHA1 certificate (e.g. with a custom
     * VerifyConnection or by a direct match with the CertPool), than that the
     * peer would have a better certificate but is just choosing not to send it.
     * crypto/x509 will refuse to verify important SHA-1 signatures anyway. */
    if (is_cert)
        return false;

    /* TLS 1.3 removed support for PKCS#1 v1.5 and SHA-1 signatures, and Go 1.25
     * removed support for SHA-1 signatures in TLS 1.2. */
    uint8_t sig_type;
    CryptoHash sig_hash;
    (void)burrow__tls_type_and_hash_from_signature_scheme(s, &sig_type, &sig_hash,
                                                          NULL);
    if (version > TLS_VERSION_TLS12)
        return sig_type == TLS_SIGNATURE_PKCS1V15 || sig_hash == CRYPTO_SHA1;
    if ((tls_debug_load() & TLS_DEBUG_SHA1_ON) == 0)
        return sig_hash == CRYPTO_SHA1;
    return false;
}

/* The default signature algorithms with the ones that are disabled at every
 * version from min_vers to max_vers taken out, from a. */
static Slice tls_signature_algorithms_for(Alloc *a, uint16_t min_vers,
                                          uint16_t max_vers, bool is_cert) {
    Int n = (Int)(sizeof(burrow__tls_default_supported_signature_algorithms) /
                  sizeof(burrow__tls_default_supported_signature_algorithms[0]));
    Slice out = slice_make(a, TYPE_UINT16, 0, n);
    if (out.p == NULL)
        return out;
    TlsSignatureScheme *p = (TlsSignatureScheme *)out.p;
    for (Int i = 0; i < n; i++) {
        TlsSignatureScheme s = burrow__tls_default_supported_signature_algorithms[i];
        for (uint32_t v = min_vers; v <= max_vers; v++) {
            if (!burrow__tls_is_disabled_signature_algorithm((uint16_t)v, s, is_cert)) {
                p[out.len++] = s;
                break;
            }
        }
    }
    return out;
}

Slice burrow__tls_supported_signature_algorithms(Alloc *a, uint16_t min_vers,
                                                 uint16_t max_vers) {
    return tls_signature_algorithms_for(a, min_vers, max_vers, false);
}

Slice burrow__tls_supported_signature_algorithms_cert(Alloc *a, uint16_t min_vers,
                                                      uint16_t max_vers) {
    return tls_signature_algorithms_for(a, min_vers, max_vers, true);
}

bool burrow__tls_is_supported_signature_algorithm(TlsSignatureScheme sig_alg,
                                                  Slice supported) {
    const TlsSignatureScheme *p = (const TlsSignatureScheme *)supported.p;
    for (Int i = 0; i < supported.len; i++) {
        if (p[i] == sig_alg)
            return true;
    }
    return false;
}

/* ------------------------------------------------------------- descriptors */

const Type burrow_type_TlsCipherSuite = {
    {(const Byte *)"CipherSuite", 11},
    {(const Byte *)"crypto/tls", 10},
    KIND_STRUCT,
    (uint32_t)sizeof(TlsCipherSuite),
    (uint16_t)_Alignof(TlsCipherSuite),
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

BURROW_PTR_TYPE_DEFINE(TlsCipherSuitePtr, TlsCipherSuite);
