/* crypto/tls: auth.go, the signatures of the handshake, made and checked with
 * the certificate keys, and the choice of the scheme to make them with.
 *
 * A Go crypto.Signer is any type with the methods. Here a certificate's
 * private key is one of the four key types below, or a TYPE_CRYPTO_SIGNER for
 * any other signer, and burrow__tls_private_key_signer turns it into the
 * CryptoSigner the handshake signs with. The two cases in
 * unsupportedCertificateError for an RSA or ECDSA key that is not a pointer,
 * and an Ed25519 key that is one, have no counterpart, since the key types
 * here are always held the one way.
 *
 * Copyright 2017 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/rsa.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/hash.h"
#include "burrow/mem.h"
#include "burrow/mem/arena.h"
#include "burrow/panic.h"
#include "burrow/slice.h"
#include "burrow/type.h"

#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* What pubkey points at when it is of type t, NULL when it is not. */
static void *tls_key_as(Any key, const Type *t) {
    return key.t == t ? key.data : NULL;
}

static Error tls_auth_error(const char *msg) {
    return errors_new(error_allocator(), str_from_cstr(msg));
}

/* The body of verifyHandshakeSignature once signed is hashed, with a for the
 * hash. */
static Error tls_verify_handshake_signature_in(Alloc *a, uint8_t sig_type,
                                               CryptoPublicKey pubkey,
                                               CryptoHash hash_func, Slice signed_,
                                               Slice sig) {
    if (hash_func != TLS_DIRECT_SIGNING) {
        if (!crypto_hash_available(hash_func))
            return fmt_errorf_v("hash function unavailable: %s",
                                crypto_hash_string(hash_func, error_allocator()));
        Hash h = crypto_hash_new(hash_func, a);
        Error werr = BURROW_NO_ERROR;
        hash_write(h, signed_, &werr);
        signed_ = hash_sum(a, h, slice_nil(TYPE_BYTE));
    }
    switch (sig_type) {
    case TLS_SIGNATURE_ECDSA: {
        const EcdsaPublicKey *pub = tls_key_as(pubkey, TYPE_ECDSA_PUBLIC_KEY);
        if (pub == NULL)
            return fmt_errorf_v("expected an ECDSA public key, got %T", pubkey);
        if (!ecdsa_verify_asn1(pub, signed_, sig))
            return tls_auth_error("ECDSA verification failure");
        break;
    }
    case TLS_SIGNATURE_ED25519: {
        const Ed25519PublicKey *pub = tls_key_as(pubkey, TYPE_ED25519_PUBLIC_KEY);
        if (pub == NULL)
            return fmt_errorf_v("expected an Ed25519 public key, got %T", pubkey);
        if (!ed25519_verify(*pub, signed_, sig))
            return tls_auth_error("Ed25519 verification failure");
        break;
    }
    case TLS_SIGNATURE_MLDSA: {
        const MldsaPublicKey *pub = tls_key_as(pubkey, TYPE_MLDSA_PUBLIC_KEY);
        if (pub == NULL)
            return fmt_errorf_v("expected an ML-DSA public key, got %T", pubkey);
        Error e = mldsa_verify(pub, signed_, sig, NULL);
        if (BURROW_FAILED(e))
            return fmt_errorf_v("ML-DSA verification failure: %w", e);
        break;
    }
    case TLS_SIGNATURE_PKCS1V15: {
        const RsaPublicKey *pub = tls_key_as(pubkey, TYPE_RSA_PUBLIC_KEY);
        if (pub == NULL)
            return fmt_errorf_v("expected an RSA public key, got %T", pubkey);
        Error e = rsa_verify_pkcs1_v15(pub, hash_func, signed_, sig);
        if (BURROW_FAILED(e))
            return e;
        break;
    }
    case TLS_SIGNATURE_RSAPSS: {
        const RsaPublicKey *pub = tls_key_as(pubkey, TYPE_RSA_PUBLIC_KEY);
        if (pub == NULL)
            return fmt_errorf_v("expected an RSA public key, got %T", pubkey);
        RsaPSSOptions sign_opts = {RSA_PSS_SALT_LENGTH_EQUALS_HASH, 0};
        Error e = rsa_verify_pss(pub, hash_func, signed_, sig, &sign_opts);
        if (BURROW_FAILED(e))
            return e;
        break;
    }
    default:
        return tls_auth_error("internal error: unknown signature type");
    }
    return BURROW_NO_ERROR;
}

Error burrow__tls_verify_handshake_signature(uint8_t sig_type, CryptoPublicKey pubkey,
                                             CryptoHash hash_func, Slice signed_,
                                             Slice sig) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Error e = tls_verify_handshake_signature_in(arena_allocator(&ar), sig_type, pubkey,
                                                hash_func, signed_, sig);
    arena_free(&ar);
    return e;
}

Error burrow__tls_verify_legacy_handshake_signature(uint8_t sig_type,
                                                    CryptoPublicKey pubkey,
                                                    CryptoHash hash_func, Slice hashed,
                                                    Slice sig) {
    switch (sig_type) {
    case TLS_SIGNATURE_ECDSA: {
        const EcdsaPublicKey *pub = tls_key_as(pubkey, TYPE_ECDSA_PUBLIC_KEY);
        if (pub == NULL)
            return fmt_errorf_v("expected an ECDSA public key, got %T", pubkey);
        if (!ecdsa_verify_asn1(pub, hashed, sig))
            return tls_auth_error("ECDSA verification failure");
        break;
    }
    case TLS_SIGNATURE_PKCS1V15: {
        const RsaPublicKey *pub = tls_key_as(pubkey, TYPE_RSA_PUBLIC_KEY);
        if (pub == NULL)
            return fmt_errorf_v("expected an RSA public key, got %T", pubkey);
        Error e = rsa_verify_pkcs1_v15(pub, hash_func, hashed, sig);
        if (BURROW_FAILED(e))
            return e;
        break;
    }
    default:
        return tls_auth_error("internal error: unknown signature type");
    }
    return BURROW_NO_ERROR;
}

/* signaturePadding, 64 spaces. */
#define TLS_SIGNATURE_PADDING_LEN 64

Slice burrow__tls_signed_message(Alloc *a, Str context, Hash transcript) {
    /* 64 for signaturePadding, the context and 64 for SHA-512. */
    Int max_size = TLS_SIGNATURE_PADDING_LEN +
                   (Int)(sizeof(TLS_SERVER_SIGNATURE_CONTEXT) - 1) + 512 / 8;
    Slice b = slice_make(a, TYPE_BYTE, 0, max_size);
    if (b.p == NULL)
        return b;
    Byte pad[TLS_SIGNATURE_PADDING_LEN];
    memset(pad, 0x20, sizeof(pad));
    b = slice_append(a, b, pad, TLS_SIGNATURE_PADDING_LEN);
    b = slice_append(a, b, context.p, context.len);
    return hash_sum(a, transcript, b);
}

bool burrow__tls_legacy_type_and_hash_from_public_key(CryptoPublicKey pub,
                                                      uint8_t *sig_type,
                                                      CryptoHash *hash, Error *err) {
    BURROW_OUT(sig_type, 0);
    BURROW_OUT(hash, 0);
    if (pub.t == TYPE_RSA_PUBLIC_KEY) {
        BURROW_OUT(sig_type, TLS_SIGNATURE_PKCS1V15);
        BURROW_OUT(hash, CRYPTO_MD5SHA1);
        return true;
    }
    if (pub.t == TYPE_ECDSA_PUBLIC_KEY) {
        BURROW_OUT(sig_type, TLS_SIGNATURE_ECDSA);
        BURROW_OUT(hash, CRYPTO_SHA1);
        return true;
    }
    if (pub.t == TYPE_ED25519_PUBLIC_KEY) {
        /* RFC 8422 specifies support for Ed25519 in TLS 1.0 and 1.1, but it
         * requires holding on to a handshake transcript to do a full
         * signature, and not even OpenSSL bothers with the complexity, so we
         * can't even test it properly. */
        BURROW_OUT(err,
                   tls_auth_error("tls: Ed25519 public keys are not supported before "
                                  "TLS 1.2"));
        return false;
    }
    if (pub.t == TYPE_MLDSA_PUBLIC_KEY) {
        BURROW_OUT(err, tls_auth_error("tls: ML-DSA public keys are not supported "
                                       "before TLS 1.3"));
        return false;
    }
    BURROW_OUT(err, fmt_errorf_v("tls: unsupported public key: %T", pub));
    return false;
}

typedef struct TlsRsaSignatureScheme {
    TlsSignatureScheme scheme;
    Int min_modulus_bytes;
} TlsRsaSignatureScheme;

static const TlsRsaSignatureScheme tls_rsa_signature_schemes[] = {
    /* RSA-PSS is used with PSSSaltLengthEqualsHash, and requires
     *    emLen >= hLen + sLen + 2 */
    {TLS_PSS_WITH_SHA256, 32 * 2 + 2},
    {TLS_PSS_WITH_SHA384, 48 * 2 + 2},
    {TLS_PSS_WITH_SHA512, 64 * 2 + 2},
    /* PKCS #1 v1.5 uses prefixes from hashPrefixes in crypto/rsa, and requires
     *    emLen >= len(prefix) + hLen + 11 */
    {TLS_PKCS1_WITH_SHA256, 19 + 32 + 11},
    {TLS_PKCS1_WITH_SHA384, 19 + 48 + 11},
    {TLS_PKCS1_WITH_SHA512, 19 + 64 + 11},
    {TLS_PKCS1_WITH_SHA1, 15 + 20 + 11},
};

#define TLS_N_RSA_SIGNATURE_SCHEMES                                                    \
    ((Int)(sizeof(tls_rsa_signature_schemes) / sizeof(tls_rsa_signature_schemes[0])))

static bool tls_curve_is(EllipticCurve c, EllipticCurve want) {
    return c.vt == want.vt && c.data == want.data;
}

/* The schemes at schemes, n of them, as a []SignatureScheme from a. */
static Slice tls_schemes(Alloc *a, const TlsSignatureScheme *schemes, Int n) {
    Slice out = slice_make(a, TYPE_UINT16, 0, n);
    if (out.p == NULL)
        return out;
    return slice_append(a, out, schemes, n);
}

Slice burrow__tls_signature_schemes_for_public_key(Alloc *a, uint16_t version,
                                                   CryptoPublicKey pub) {
    if (pub.t == TYPE_ECDSA_PUBLIC_KEY && pub.data != NULL) {
        const EcdsaPublicKey *k = pub.data;
        if (version < TLS_VERSION_TLS13) {
            /* In TLS 1.2 and earlier, ECDSA algorithms are not constrained to
             * a single curve. */
            static const TlsSignatureScheme any_curve[] = {
                TLS_ECDSA_WITH_P256_AND_SHA256,
                TLS_ECDSA_WITH_P384_AND_SHA384,
                TLS_ECDSA_WITH_P521_AND_SHA512,
                TLS_ECDSA_WITH_SHA1,
            };
            return tls_schemes(a, any_curve, 4);
        }
        TlsSignatureScheme s;
        if (tls_curve_is(k->curve, elliptic_p256()))
            s = TLS_ECDSA_WITH_P256_AND_SHA256;
        else if (tls_curve_is(k->curve, elliptic_p384()))
            s = TLS_ECDSA_WITH_P384_AND_SHA384;
        else if (tls_curve_is(k->curve, elliptic_p521()))
            s = TLS_ECDSA_WITH_P521_AND_SHA512;
        else
            return slice_nil(TYPE_UINT16);
        return tls_schemes(a, &s, 1);
    }
    if (pub.t == TYPE_RSA_PUBLIC_KEY && pub.data != NULL) {
        Int size = rsa_public_key_size(pub.data);
        Slice sig_algs = slice_make(a, TYPE_UINT16, 0, TLS_N_RSA_SIGNATURE_SCHEMES);
        if (sig_algs.p == NULL)
            return sig_algs;
        for (Int i = 0; i < TLS_N_RSA_SIGNATURE_SCHEMES; i++) {
            const TlsRsaSignatureScheme *candidate = &tls_rsa_signature_schemes[i];
            if (size >= candidate->min_modulus_bytes)
                sig_algs = slice_append(a, sig_algs, &candidate->scheme, 1);
        }
        return sig_algs;
    }
    if (pub.t == TYPE_ED25519_PUBLIC_KEY) {
        TlsSignatureScheme s = TLS_ED25519;
        return tls_schemes(a, &s, 1);
    }
    if (pub.t == TYPE_MLDSA_PUBLIC_KEY && pub.data != NULL) {
        const MldsaParameters *params = mldsa_public_key_parameters(pub.data);
        TlsSignatureScheme s;
        if (params == mldsa_mldsa44())
            s = TLS_MLDSA44;
        else if (params == mldsa_mldsa65())
            s = TLS_MLDSA65;
        else if (params == mldsa_mldsa87())
            s = TLS_MLDSA87;
        else
            panic_str(BURROW_S("tls: internal error: unknown ML-DSA parameter set"));
        return tls_schemes(a, &s, 1);
    }
    return slice_nil(TYPE_UINT16);
}

bool burrow__tls_private_key_signer(CryptoPrivateKey key, Ed25519Signer *es,
                                    CryptoSigner *out) {
    if (key.data == NULL)
        return false;
    if (key.t == TYPE_RSA_PRIVATE_KEY)
        *out = rsa_private_key_signer(key.data);
    else if (key.t == TYPE_ECDSA_PRIVATE_KEY)
        *out = ecdsa_private_key_signer(key.data);
    else if (key.t == TYPE_ED25519_PRIVATE_KEY)
        *out = ed25519_private_key_signer(*(const Ed25519PrivateKey *)key.data, es);
    else if (key.t == TYPE_MLDSA_PRIVATE_KEY)
        *out = mldsa_private_key_signer(key.data);
    else if (key.t == TYPE_CRYPTO_SIGNER)
        *out = *(const CryptoSigner *)key.data;
    else
        return false;
    return true;
}

/* Takes out of algs, a []SignatureScheme, the schemes keep says false of, the
 * way slices.DeleteFunc does with the opposite. */
static Slice tls_keep_schemes(Slice algs,
                              bool (*keep)(TlsSignatureScheme, const void *),
                              const void *env) {
    TlsSignatureScheme *p = algs.p;
    Int n = 0;
    for (Int i = 0; i < algs.len; i++) {
        if (keep(p[i], env))
            p[n++] = p[i];
    }
    algs.len = n;
    return algs;
}

static bool tls_in_cert_algs(TlsSignatureScheme sig_alg, const void *env) {
    return burrow__tls_is_supported_signature_algorithm(sig_alg, *(const Slice *)env);
}

static bool tls_enabled_at(TlsSignatureScheme sig_alg, const void *env) {
    return !burrow__tls_is_disabled_signature_algorithm(*(const uint16_t *)env, sig_alg,
                                                        false);
}

static TlsSignatureScheme tls_select_signature_scheme_in(Alloc *a, uint16_t vers,
                                                         const TlsCertificate *c,
                                                         Slice peer_algs, Error *err) {
    Ed25519Signer es;
    CryptoSigner priv;
    if (!burrow__tls_private_key_signer(c->private_key, &es, &priv)) {
        BURROW_OUT(err, burrow__tls_unsupported_certificate_error(c));
        return 0;
    }
    Slice supported_algs = burrow__tls_signature_schemes_for_public_key(
        a, vers, crypto_signer_public(priv));
    if (!slice_is_nil(c->supported_signature_algorithms))
        supported_algs = tls_keep_schemes(supported_algs, tls_in_cert_algs,
                                          &c->supported_signature_algorithms);
    /* Filter out any unsupported signature algorithms, for example due to FIPS
     * 140-3 policy, tlssha1=0, or protocol version. */
    supported_algs = tls_keep_schemes(supported_algs, tls_enabled_at, &vers);
    if (supported_algs.len == 0) {
        BURROW_OUT(err, burrow__tls_unsupported_certificate_error(c));
        return 0;
    }
    if (peer_algs.len == 0 && vers == TLS_VERSION_TLS12) {
        /* For TLS 1.2, if the client didn't send signature_algorithms then we
         * can assume that it supports SHA1. See RFC 5246, Section 7.4.1.4.1.
         * RFC 9155 made signature_algorithms mandatory in TLS 1.2, and we
         * gated it behind the tlssha1 GODEBUG setting. */
        if (!burrow__tls_godebug_sha1()) {
            BURROW_OUT(err, tls_auth_error("tls: missing signature_algorithms from TLS "
                                           "1.2 peer"));
            return 0;
        }
        static const TlsSignatureScheme sha1_algs[] = {TLS_PKCS1_WITH_SHA1,
                                                       TLS_ECDSA_WITH_SHA1};
        peer_algs = tls_schemes(a, sha1_algs, 2);
    }
    /* Pick signature scheme in the peer's preference order, as our preference
     * order is not configurable. */
    const TlsSignatureScheme *p = peer_algs.p;
    for (Int i = 0; i < peer_algs.len; i++) {
        if (burrow__tls_is_supported_signature_algorithm(p[i], supported_algs))
            return p[i];
    }
    BURROW_OUT(err, tls_auth_error("tls: peer doesn't support any of the certificate's "
                                   "signature algorithms"));
    return 0;
}

TlsSignatureScheme burrow__tls_select_signature_scheme(uint16_t vers,
                                                       const TlsCertificate *c,
                                                       Slice peer_algs, Error *err) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    TlsSignatureScheme s =
        tls_select_signature_scheme_in(arena_allocator(&ar), vers, c, peer_algs, err);
    arena_free(&ar);
    return s;
}

Error burrow__tls_unsupported_certificate_error(const TlsCertificate *cert) {
    Ed25519Signer es;
    CryptoSigner signer;
    if (!burrow__tls_private_key_signer(cert->private_key, &es, &signer))
        return fmt_errorf_v(
            "tls: certificate private key (%T) does not implement crypto.Signer",
            cert->private_key);

    CryptoPublicKey pub = crypto_signer_public(signer);
    if (pub.t == TYPE_ECDSA_PUBLIC_KEY && pub.data != NULL) {
        EllipticCurve curve = ((const EcdsaPublicKey *)pub.data)->curve;
        if (!tls_curve_is(curve, elliptic_p256()) &&
            !tls_curve_is(curve, elliptic_p384()) &&
            !tls_curve_is(curve, elliptic_p521()))
            return fmt_errorf_v("tls: unsupported certificate curve (%s)",
                                elliptic_curve_params(curve)->name);
    } else if (pub.t == TYPE_RSA_PUBLIC_KEY) {
        return tls_auth_error(
            "tls: certificate RSA key size too small for supported signature "
            "algorithms");
    } else if (pub.t == TYPE_ED25519_PUBLIC_KEY) {
    } else if (pub.t == TYPE_MLDSA_PUBLIC_KEY) {
        return tls_auth_error("tls: ML-DSA certificates require TLS 1.3");
    } else {
        return fmt_errorf_v("tls: unsupported certificate key (%T)", pub);
    }

    if (!slice_is_nil(cert->supported_signature_algorithms))
        return tls_auth_error(
            "tls: peer doesn't support the certificate custom signature algorithms");

    return fmt_errorf_v("tls: internal error: unsupported key (%T)", cert->private_key);
}
