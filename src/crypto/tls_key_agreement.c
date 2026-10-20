/* crypto/tls: key_agreement.go. The TLS 1.0–1.2 key exchanges: RSA, where the
 * client encrypts the pre-master secret to the server's key, and ECDHE, where
 * the server signs an ephemeral key.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/md5.h"
#include "burrow/crypto/rsa.h"
#include "burrow/crypto/sha1.h"
#include "burrow/crypto/x509.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/hash.h"
#include "burrow/io.h"
#include "burrow/mem.h"
#include "burrow/slice.h"

#include "tls_internal.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* md5.Size and sha1.Size. */
#define TKA_MD5_SIZE 16
#define TKA_SHA1_SIZE 20

BURROW_SENTINEL_ERROR(tka_err_client_key_exchange,
                      "tls: invalid ClientKeyExchange message");
BURROW_SENTINEL_ERROR(tka_err_server_key_exchange,
                      "tls: invalid ServerKeyExchange message");
BURROW_SENTINEL_ERROR(tka_err_not_decrypter, "tls: certificate private key does not "
                                             "implement crypto.Decrypter");
BURROW_SENTINEL_ERROR(tka_err_unexpected_skx, "tls: unexpected ServerKeyExchange");
BURROW_SENTINEL_ERROR(tka_err_incorrect_key_type,
                      "tls: server certificate contains incorrect key type for "
                      "selected ciphersuite");
BURROW_SENTINEL_ERROR(tka_err_no_curves, "tls: no supported elliptic curves offered");
BURROW_SENTINEL_ERROR(tka_err_curve_accepted,
                      "tls: internal error: supportsCurve accepted unimplemented "
                      "curve");
BURROW_SENTINEL_ERROR(tka_err_cert_suite,
                      "tls: certificate cannot be used with the selected cipher "
                      "suite");
BURROW_SENTINEL_ERROR(tka_err_unsupported_curve,
                      "tls: server selected unsupported curve");
BURROW_SENTINEL_ERROR(tka_err_mldsa,
                      "tls: server selected ML-DSA with TLS version < 1.3");
BURROW_SENTINEL_ERROR(tka_err_unoffered_curve, "tls: server selected unoffered curve");
BURROW_SENTINEL_ERROR(tka_err_invalid_sig_alg,
                      "tls: certificate used with invalid signature algorithm");
BURROW_SENTINEL_ERROR(tka_err_missing_skx, "tls: missing ServerKeyExchange message");
BURROW_SENTINEL_ERROR(tka_err_internal_curve, "tls: internal error: unsupported curve");

void burrow__tls_key_agreement_init(TlsKeyAgreement *ka, const TlsSuite *suite,
                                    uint16_t version) {
    memset(ka, 0, sizeof *ka);
    switch (suite->ka) {
    case TLS_KA_RSA:
        break;
    case TLS_KA_ECDHE_RSA:
        ka->is_ecdhe = true;
        ka->is_rsa = true;
        ka->version = version;
        break;
    case TLS_KA_ECDHE_ECDSA:
        ka->is_ecdhe = true;
        ka->version = version;
        break;
    default:
        break;
    }
}

void burrow__tls_key_agreement_free(TlsKeyAgreement *ka) {
    ecdh_private_key_free(ka->key);
    ka->key = NULL;
}

/* ------------------------------------------------------------------- RSA */

/* rsaKeyAgreement implements the standard TLS key agreement where the client
 * encrypts the pre-master secret to the server's public key. */

static Slice tka_rsa_process_client_key_exchange(Alloc *a, const TlsConfig *config,
                                                 const TlsCertificate *cert,
                                                 const TlsClientKeyExchangeMsg *ckx,
                                                 Error *err) {
    const Byte *ct = ckx->ciphertext.p;
    if (ckx->ciphertext.len < 2) {
        BURROW_OUT(err, tka_err_client_key_exchange);
        return slice_nil(TYPE_BYTE);
    }
    Int ciphertext_len = (Int)ct[0] << 8 | (Int)ct[1];
    if (ciphertext_len != ckx->ciphertext.len - 2) {
        BURROW_OUT(err, tka_err_client_key_exchange);
        return slice_nil(TYPE_BYTE);
    }
    Slice ciphertext = slice_sub(ckx->ciphertext, 2, ckx->ciphertext.len);

    /* Not in Go: of the keys a Certificate can hold, only an RSA key is a
     * crypto.Decrypter. */
    if (cert->private_key.t != TYPE_RSA_PRIVATE_KEY || cert->private_key.data == NULL) {
        BURROW_OUT(err, tka_err_not_decrypter);
        return slice_nil(TYPE_BYTE);
    }
    const RsaPrivateKey *priv = cert->private_key.data;
    /* Perform constant time RSA PKCS #1 v1.5 decryption */
    RsaPKCS1v15DecryptOptions opts = {48};
    Error e = BURROW_NO_ERROR;
    Slice pre_master_secret = rsa_private_key_decrypt(
        priv, a, burrow__tls_config_rand(config), ciphertext,
        rsa_pkcs1_v15_decrypt_options_as_decrypter_opts(&opts), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }
    /* We don't check the version number in the premaster secret. For one,
     * by checking it, we would leak information about the validity of the
     * encrypted pre-master secret. Secondly, it provides only a small
     * benefit against a downgrade attack and some implementations send the
     * wrong version anyway. See the discussion at the end of section
     * 7.4.7.1 of RFC 4346. */
    return pre_master_secret;
}

static Slice tka_rsa_generate_client_key_exchange(Alloc *a, const TlsConfig *config,
                                                  const TlsClientHelloMsg *client_hello,
                                                  const X509Certificate *cert,
                                                  TlsClientKeyExchangeMsg **out,
                                                  Error *err) {
    Slice pre_master_secret = slice_make(a, TYPE_BYTE, 48, 48);
    if (pre_master_secret.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    Byte *pms = pre_master_secret.p;
    pms[0] = (Byte)(client_hello->vers >> 8);
    pms[1] = (Byte)client_hello->vers;
    Error e = BURROW_NO_ERROR;
    (void)io_read_full(burrow__tls_config_rand(config),
                       slice_sub(pre_master_secret, 2, 48), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }

    if (cert->public_key.t != TYPE_RSA_PUBLIC_KEY || cert->public_key.data == NULL) {
        BURROW_OUT(err, tka_err_incorrect_key_type);
        return slice_nil(TYPE_BYTE);
    }
    const RsaPublicKey *rsa_key = cert->public_key.data;
    Slice encrypted = rsa_encrypt_pkcs1_v15(a, burrow__tls_config_rand(config), rsa_key,
                                            pre_master_secret, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return slice_nil(TYPE_BYTE);
    }
    TlsClientKeyExchangeMsg *ckx = BURROW_NEW(a, TlsClientKeyExchangeMsg);
    Slice ciphertext = slice_make(a, TYPE_BYTE, encrypted.len + 2, encrypted.len + 2);
    if (ckx == NULL || ciphertext.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return slice_nil(TYPE_BYTE);
    }
    Byte *c = ciphertext.p;
    c[0] = (Byte)(encrypted.len >> 8);
    c[1] = (Byte)encrypted.len;
    memcpy(c + 2, encrypted.p, (size_t)encrypted.len);
    ckx->ciphertext = ciphertext;
    *out = ckx;
    return pre_master_secret;
}

/* ------------------------------------------------------------------ hashes */

/* The digest of h over the three slices, into out. It fits in the buffer, so a
 * is not used. */
static void tka_hash_into(Alloc *a, Hash h, Byte *out, Int size, Slice s1, Slice s2,
                          Slice s3) {
    Error e = BURROW_NO_ERROR;
    hash_write(h, s1, &e);
    hash_write(h, s2, &e);
    hash_write(h, s3, &e);
    Byte buf[TKA_SHA1_SIZE];
    Slice sum = hash_sum(a, h, slice_from(buf, 0, sizeof buf, TYPE_BYTE));
    memcpy(out, sum.p, (size_t)size);
}

/* hashForServerKeyExchange hashes the given slices and returns their digest
 * using a hash based on the sigType. It can only be used for TLS 1.0 and 1.1.
 *
 * Not in Go: sha1Hash and md5SHA1Hash are folded in here, the three slices are
 * the ones every caller passes, and the digest is from a. */
static Slice tka_hash_for_server_key_exchange(Alloc *a, uint8_t sig_type, Slice s1,
                                              Slice s2, Slice s3) {
    bool ecdsa = sig_type == TLS_SIGNATURE_ECDSA;
    Int n = ecdsa ? TKA_SHA1_SIZE : TKA_MD5_SIZE + TKA_SHA1_SIZE;
    Slice out = slice_make(a, TYPE_BYTE, n, n);
    if (out.p == NULL)
        return out;
    Byte *p = out.p;
    if (!ecdsa) {
        Hash hmd5 = md5_new(a);
        if (hmd5.vt == NULL)
            return slice_nil(TYPE_BYTE);
        tka_hash_into(a, hmd5, p, TKA_MD5_SIZE, s1, s2, s3);
        p += TKA_MD5_SIZE;
    }
    Hash hsha1 = sha1_new(a);
    if (hsha1.vt == NULL)
        return slice_nil(TYPE_BYTE);
    tka_hash_into(a, hsha1, p, TKA_SHA1_SIZE, s1, s2, s3);
    return out;
}

/* slices.Concat of three byte slices, from a. */
static Slice tka_concat(Alloc *a, Slice s1, Slice s2, Slice s3) {
    Int n = s1.len + s2.len + s3.len;
    Slice out = slice_make(a, TYPE_BYTE, n, n);
    if (out.p == NULL)
        return out;
    Byte *p = out.p;
    if (s1.len > 0)
        memcpy(p, s1.p, (size_t)s1.len);
    if (s2.len > 0)
        memcpy(p + s1.len, s2.p, (size_t)s2.len);
    if (s3.len > 0)
        memcpy(p + s1.len + s2.len, s3.p, (size_t)s3.len);
    return out;
}

/* ------------------------------------------------------------------- ECDHE */

/* ecdheKeyAgreement implements a TLS key agreement where the server
 * generates an ephemeral EC public/private key pair and signs it. The
 * pre-master secret is then calculated using ECDH. The signature may
 * be ECDSA, Ed25519 or RSA. */

static TlsServerKeyExchangeMsg *tka_ecdhe_generate_server_key_exchange(
    TlsKeyAgreement *ka, Alloc *a, const TlsConfig *config, const TlsCertificate *cert,
    const TlsClientHelloMsg *client_hello, const TlsServerHelloMsg *hello, Error *err) {
    const TlsCurveID *curves = client_hello->supported_curves.p;
    for (Int i = 0; i < client_hello->supported_curves.len; i++) {
        if (burrow__tls_config_supports_curve(config, ka->version, curves[i])) {
            ka->curve_id = curves[i];
            break;
        }
    }

    if (ka->curve_id == 0) {
        BURROW_OUT(err, tka_err_no_curves);
        return NULL;
    }
    if (burrow__tls_curve_for_curve_id(ka->curve_id) == NULL) {
        BURROW_OUT(err, tka_err_curve_accepted);
        return NULL;
    }

    Error e = BURROW_NO_ERROR;
    EcdhPrivateKey *key = burrow__tls_generate_ecdhe_key(
        a, burrow__tls_config_rand(config), ka->curve_id, &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, e);
        return NULL;
    }
    ecdh_private_key_free(ka->key);
    ka->key = key;

    /* See RFC 4492, Section 5.4. */
    Slice ecdhe_public = ecdh_public_key_bytes(ecdh_private_key_public_key(key), a);
    Int params_len = 1 + 2 + 1 + ecdhe_public.len;
    Slice server_ecdhe_params = slice_make(a, TYPE_BYTE, params_len, params_len);
    if (ecdhe_public.p == NULL || server_ecdhe_params.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    Byte *params = server_ecdhe_params.p;
    params[0] = 3; /* named curve */
    params[1] = (Byte)(ka->curve_id >> 8);
    params[2] = (Byte)ka->curve_id;
    params[3] = (Byte)ecdhe_public.len;
    memcpy(params + 4, ecdhe_public.p, (size_t)ecdhe_public.len);

    Ed25519Signer es;
    CryptoSigner priv;
    if (!burrow__tls_private_key_signer(cert->private_key, &es, &priv)) {
        BURROW_OUT(err, fmt_errorf_v("tls: certificate private key of type %T does not "
                                     "implement crypto.Signer",
                                     cert->private_key));
        return NULL;
    }

    Slice sig;
    if (ka->version >= TLS_VERSION_TLS12) {
        ka->signature_algorithm = burrow__tls_select_signature_scheme(
            ka->version, cert, client_hello->supported_signature_algorithms, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err, e);
            return NULL;
        }
        uint8_t sig_type;
        CryptoHash sig_hash;
        if (!burrow__tls_type_and_hash_from_signature_scheme(
                ka->signature_algorithm, &sig_type, &sig_hash, &e)) {
            BURROW_OUT(err, e);
            return NULL;
        }
        /* Go counts the tlssha1 GODEBUG setting here when sigHash is SHA-1. */
        Slice signed_ =
            tka_concat(a, client_hello->random, hello->random, server_ecdhe_params);
        if (signed_.p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        if ((sig_type == TLS_SIGNATURE_PKCS1V15 || sig_type == TLS_SIGNATURE_RSAPSS) !=
            ka->is_rsa) {
            BURROW_OUT(err, tka_err_cert_suite);
            return NULL;
        }
        CryptoSignerOpts sign_opts = crypto_hash_as_signer_opts(&sig_hash);
        RsaPSSOptions pss = {RSA_PSS_SALT_LENGTH_EQUALS_HASH, sig_hash};
        if (sig_type == TLS_SIGNATURE_RSAPSS)
            sign_opts = rsa_pss_options_as_signer_opts(&pss);
        sig = crypto_sign_message(a, priv, burrow__tls_config_rand(config), signed_,
                                  sign_opts, &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err,
                       fmt_errorf_v("tls: failed to sign ECDHE parameters: %v", e));
            return NULL;
        }
    } else {
        uint8_t sig_type;
        CryptoHash sig_hash;
        if (!burrow__tls_legacy_type_and_hash_from_public_key(
                crypto_signer_public(priv), &sig_type, &sig_hash, &e)) {
            BURROW_OUT(err, e);
            return NULL;
        }
        Slice signed_ = tka_hash_for_server_key_exchange(
            a, sig_type, client_hello->random, hello->random, server_ecdhe_params);
        if (signed_.p == NULL) {
            BURROW_OUT(err, burrow_err_out_of_memory);
            return NULL;
        }
        if ((sig_type == TLS_SIGNATURE_PKCS1V15) != ka->is_rsa) {
            BURROW_OUT(err, tka_err_cert_suite);
            return NULL;
        }
        sig = crypto_signer_sign(priv, a, burrow__tls_config_rand(config), signed_,
                                 crypto_hash_as_signer_opts(&sig_hash), &e);
        if (BURROW_FAILED(e)) {
            BURROW_OUT(err,
                       fmt_errorf_v("tls: failed to sign ECDHE parameters: %v", e));
            return NULL;
        }
    }

    TlsServerKeyExchangeMsg *skx = BURROW_NEW(a, TlsServerKeyExchangeMsg);
    Int sig_and_hash_len = 0;
    if (ka->version >= TLS_VERSION_TLS12)
        sig_and_hash_len = 2;
    Int key_len = server_ecdhe_params.len + sig_and_hash_len + 2 + sig.len;
    Slice skx_key = slice_make(a, TYPE_BYTE, key_len, key_len);
    if (skx == NULL || skx_key.p == NULL) {
        BURROW_OUT(err, burrow_err_out_of_memory);
        return NULL;
    }
    memcpy(skx_key.p, params, (size_t)server_ecdhe_params.len);
    Byte *k = (Byte *)skx_key.p + server_ecdhe_params.len;
    if (ka->version >= TLS_VERSION_TLS12) {
        k[0] = (Byte)(ka->signature_algorithm >> 8);
        k[1] = (Byte)ka->signature_algorithm;
        k += 2;
    }
    k[0] = (Byte)(sig.len >> 8);
    k[1] = (Byte)sig.len;
    if (sig.len > 0)
        memcpy(k + 2, sig.p, (size_t)sig.len);
    skx->key = skx_key;
    return skx;
}

static Slice tka_ecdhe_process_client_key_exchange(TlsKeyAgreement *ka, Alloc *a,
                                                   const TlsClientKeyExchangeMsg *ckx,
                                                   Error *err) {
    const Byte *ct = ckx->ciphertext.p;
    if (ckx->ciphertext.len == 0 || (Int)ct[0] != ckx->ciphertext.len - 1) {
        BURROW_OUT(err, tka_err_client_key_exchange);
        return slice_nil(TYPE_BYTE);
    }
    /* Not in Go: generateServerKeyExchange has to have set the key first. */
    if (ka->key == NULL) {
        BURROW_OUT(err, tka_err_client_key_exchange);
        return slice_nil(TYPE_BYTE);
    }

    Error e = BURROW_NO_ERROR;
    EcdhPublicKey *peer_key = ecdh_curve_new_public_key(
        ecdh_private_key_curve(ka->key), a,
        slice_sub(ckx->ciphertext, 1, ckx->ciphertext.len), &e);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, tka_err_client_key_exchange);
        return slice_nil(TYPE_BYTE);
    }
    Slice pre_master_secret = ecdh_private_key_ecdh(ka->key, a, peer_key, &e);
    ecdh_public_key_free(peer_key);
    if (BURROW_FAILED(e)) {
        BURROW_OUT(err, tka_err_client_key_exchange);
        return slice_nil(TYPE_BYTE);
    }

    return pre_master_secret;
}

static bool tka_contains_curve(Slice curves, TlsCurveID id) {
    const TlsCurveID *p = curves.p;
    for (Int i = 0; i < curves.len; i++) {
        if (p[i] == id)
            return true;
    }
    return false;
}

static Error tka_invalid_signature(Error e) {
    return fmt_errorf_v("tls: invalid signature by the server certificate: %v", e);
}

static Error tka_ecdhe_process_server_key_exchange(
    TlsKeyAgreement *ka, Alloc *a, const TlsConfig *config,
    const TlsClientHelloMsg *client_hello, const TlsServerHelloMsg *server_hello,
    const X509Certificate *cert, const TlsServerKeyExchangeMsg *skx) {
    const Byte *key = skx->key.p;
    if (skx->key.len < 4)
        return tka_err_server_key_exchange;
    if (key[0] != 3) /* named curve */
        return tka_err_unsupported_curve;
    ka->curve_id = (TlsCurveID)(key[1] << 8 | key[2]);

    Int public_len = (Int)key[3];
    if (public_len + 4 > skx->key.len)
        return tka_err_server_key_exchange;
    Slice server_ecdhe_params = slice_sub(skx->key, 0, 4 + public_len);
    Slice public_key = slice_sub(server_ecdhe_params, 4, server_ecdhe_params.len);

    Slice sig = slice_sub(skx->key, 4 + public_len, skx->key.len);
    if (sig.len < 2)
        return tka_err_server_key_exchange;
    if (ka->version >= TLS_VERSION_TLS12) {
        const Byte *s = sig.p;
        ka->signature_algorithm = (TlsSignatureScheme)(s[0] << 8 | s[1]);
        sig = slice_sub(sig, 2, sig.len);
        if (sig.len < 2)
            return tka_err_server_key_exchange;
        switch (ka->signature_algorithm) {
        case TLS_MLDSA44:
        case TLS_MLDSA65:
        case TLS_MLDSA87:
            return tka_err_mldsa;
        default:
            break;
        }
    }
    const Byte *s = sig.p;
    Int sig_len = (Int)s[0] << 8 | (Int)s[1];
    if (sig_len + 2 != sig.len)
        return tka_err_server_key_exchange;
    sig = slice_sub(sig, 2, sig.len);

    if (!tka_contains_curve(client_hello->supported_curves, ka->curve_id))
        return tka_err_unoffered_curve;

    if (burrow__tls_curve_for_curve_id(ka->curve_id) == NULL)
        return tka_err_unsupported_curve;

    Error e = BURROW_NO_ERROR;
    EcdhPrivateKey *priv = burrow__tls_generate_ecdhe_key(
        a, burrow__tls_config_rand(config), ka->curve_id, &e);
    if (BURROW_FAILED(e))
        return e;
    ecdh_private_key_free(ka->key);
    ka->key = priv;

    EcdhPublicKey *peer_key =
        ecdh_curve_new_public_key(ecdh_private_key_curve(priv), a, public_key, &e);
    if (BURROW_FAILED(e))
        return tka_err_server_key_exchange;
    ka->pre_master_secret = ecdh_private_key_ecdh(priv, a, peer_key, &e);
    ecdh_public_key_free(peer_key);
    if (BURROW_FAILED(e))
        return tka_err_server_key_exchange;

    Slice our_public_key = ecdh_public_key_bytes(ecdh_private_key_public_key(priv), a);
    TlsClientKeyExchangeMsg *ckx = BURROW_NEW(a, TlsClientKeyExchangeMsg);
    Slice ciphertext =
        slice_make(a, TYPE_BYTE, 1 + our_public_key.len, 1 + our_public_key.len);
    if (our_public_key.p == NULL || ckx == NULL || ciphertext.p == NULL)
        return burrow_err_out_of_memory;
    Byte *c = ciphertext.p;
    c[0] = (Byte)our_public_key.len;
    memcpy(c + 1, our_public_key.p, (size_t)our_public_key.len);
    ckx->ciphertext = ciphertext;
    ka->ckx = ckx;

    uint8_t sig_type;
    CryptoHash sig_hash;
    if (ka->version >= TLS_VERSION_TLS12) {
        if (!burrow__tls_is_supported_signature_algorithm(
                ka->signature_algorithm, client_hello->supported_signature_algorithms))
            return tka_err_invalid_sig_alg;
        if (!burrow__tls_type_and_hash_from_signature_scheme(ka->signature_algorithm,
                                                             &sig_type, &sig_hash, &e))
            return e;
        /* Go counts the tlssha1 GODEBUG setting here when sigHash is SHA-1. */
        if ((sig_type == TLS_SIGNATURE_PKCS1V15 || sig_type == TLS_SIGNATURE_RSAPSS) !=
            ka->is_rsa)
            return tka_err_server_key_exchange;
        Slice signed_ = tka_concat(a, client_hello->random, server_hello->random,
                                   server_ecdhe_params);
        if (signed_.p == NULL)
            return burrow_err_out_of_memory;
        e = burrow__tls_verify_handshake_signature(sig_type, cert->public_key, sig_hash,
                                                   signed_, sig);
        if (BURROW_FAILED(e))
            return tka_invalid_signature(e);
    } else {
        if (!burrow__tls_legacy_type_and_hash_from_public_key(cert->public_key,
                                                              &sig_type, &sig_hash, &e))
            return e;
        if ((sig_type == TLS_SIGNATURE_PKCS1V15) != ka->is_rsa)
            return tka_err_server_key_exchange;
        Slice signed_ =
            tka_hash_for_server_key_exchange(a, sig_type, client_hello->random,
                                             server_hello->random, server_ecdhe_params);
        if (signed_.p == NULL)
            return burrow_err_out_of_memory;
        e = burrow__tls_verify_legacy_handshake_signature(sig_type, cert->public_key,
                                                          sig_hash, signed_, sig);
        if (BURROW_FAILED(e))
            return tka_invalid_signature(e);
    }

    return BURROW_NO_ERROR;
}

/* ---------------------------------------------------------------- dispatch */

TlsServerKeyExchangeMsg *burrow__tls_key_agreement_generate_server_key_exchange(
    TlsKeyAgreement *ka, Alloc *a, const TlsConfig *config, const TlsCertificate *cert,
    const TlsClientHelloMsg *client_hello, const TlsServerHelloMsg *hello, Error *err) {
    if (!ka->is_ecdhe)
        return NULL;
    return tka_ecdhe_generate_server_key_exchange(ka, a, config, cert, client_hello,
                                                  hello, err);
}

Slice burrow__tls_key_agreement_process_client_key_exchange(
    TlsKeyAgreement *ka, Alloc *a, const TlsConfig *config, const TlsCertificate *cert,
    const TlsClientKeyExchangeMsg *ckx, uint16_t version, Error *err) {
    (void)version;
    if (!ka->is_ecdhe)
        return tka_rsa_process_client_key_exchange(a, config, cert, ckx, err);
    return tka_ecdhe_process_client_key_exchange(ka, a, ckx, err);
}

Error burrow__tls_key_agreement_process_server_key_exchange(
    TlsKeyAgreement *ka, Alloc *a, const TlsConfig *config,
    const TlsClientHelloMsg *client_hello, const TlsServerHelloMsg *server_hello,
    const struct X509Certificate *cert, const TlsServerKeyExchangeMsg *skx) {
    if (!ka->is_ecdhe)
        return tka_err_unexpected_skx;
    return tka_ecdhe_process_server_key_exchange(ka, a, config, client_hello,
                                                 server_hello, cert, skx);
}

Slice burrow__tls_key_agreement_generate_client_key_exchange(
    TlsKeyAgreement *ka, Alloc *a, const TlsConfig *config,
    const TlsClientHelloMsg *client_hello, const struct X509Certificate *cert,
    TlsClientKeyExchangeMsg **ckx, Error *err) {
    *ckx = NULL;
    if (!ka->is_ecdhe)
        return tka_rsa_generate_client_key_exchange(a, config, client_hello, cert, ckx,
                                                    err);
    if (ka->ckx == NULL) {
        BURROW_OUT(err, tka_err_missing_skx);
        return slice_nil(TYPE_BYTE);
    }

    *ckx = ka->ckx;
    return ka->pre_master_secret;
}

/* ------------------------------------------------------------------ curves */

EcdhPrivateKey *burrow__tls_generate_ecdhe_key(Alloc *a, IoReader rand,
                                               TlsCurveID curve_id, Error *err) {
    const EcdhCurve *curve = burrow__tls_curve_for_curve_id(curve_id);
    if (curve == NULL) {
        BURROW_OUT(err, tka_err_internal_curve);
        return NULL;
    }

    return ecdh_curve_generate_key(curve, a, rand, err);
}

const EcdhCurve *burrow__tls_curve_for_curve_id(TlsCurveID id) {
    switch (id) {
    case TLS_X25519:
        return ecdh_x25519();
    case TLS_CURVE_P256:
        return ecdh_p256();
    case TLS_CURVE_P384:
        return ecdh_p384();
    case TLS_CURVE_P521:
        return ecdh_p521();
    default:
        return NULL;
    }
}
