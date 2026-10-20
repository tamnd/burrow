/* crypto/tls: tls.go, Server, Client and the loading of a certificate and its
 * key from PEM.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/tls.h"

#include "burrow/core.h"
#include "burrow/crypto.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/rsa.h"
#include "burrow/crypto/x509.h"
#include "burrow/encoding/pem.h"
#include "burrow/error.h"
#include "burrow/fmt.h"
#include "burrow/os.h"
#include "burrow/slice.h"
#include "burrow/strings.h"
#include "burrow/type.h"

#include "tls_internal.h"

#include <stdbool.h>

TlsConn *tls_server(Alloc *a, NetConn conn, TlsConfig *config) {
    return burrow__tls_conn_new(a, conn, config, TLS_ROLE_SERVER,
                                burrow__tls_server_handshake);
}

TlsConn *tls_client(Alloc *a, NetConn conn, TlsConfig *config) {
    return burrow__tls_conn_new(a, conn, config, TLS_ROLE_CLIENT,
                                burrow__tls_client_handshake);
}

static Error tls_key_pair_error(const char *msg) {
    return errors_new(error_allocator(), str_from_cstr(msg));
}

static TlsCertificate tls_key_pair_fail(Error *err, Error e) {
    BURROW_OUT(err, e);
    return (TlsCertificate){0};
}

TlsCertificate tls_load_x509_key_pair(Alloc *a, Str cert_file, Str key_file,
                                      Error *err) {
    Error e = BURROW_NO_ERROR;
    Slice cert_pem_block = os_read_file(a, cert_file, &e);
    if (BURROW_FAILED(e))
        return tls_key_pair_fail(err, e);
    Slice key_pem_block = os_read_file(a, key_file, &e);
    if (BURROW_FAILED(e))
        return tls_key_pair_fail(err, e);
    return tls_x509_key_pair(a, cert_pem_block, key_pem_block, err);
}

/* Attempt to parse the given private key DER block. OpenSSL 0.9.8 generates
 * PKCS #1 private keys by default, while OpenSSL 1.0.0 generates PKCS #8 keys.
 * OpenSSL ecparam generates SEC1 EC private keys for ECDSA. We try all three. */
static CryptoPrivateKey tls_parse_private_key(Alloc *a, Slice der, Error *err) {
    Error pkcs8_err = BURROW_NO_ERROR; /* Return the PKCS#8 error if all parsing
                                        * attempts fail. */
    Any key = x509_parse_pkcs8_private_key(a, der, &pkcs8_err);
    if (BURROW_FAILED(pkcs8_err)) {
        Error e = BURROW_NO_ERROR;
        RsaPrivateKey *rsa = x509_parse_pkcs1_private_key(a, der, &e);
        if (!BURROW_FAILED(e)) {
            key = BURROW_ANY(TYPE_RSA_PRIVATE_KEY, rsa);
        } else {
            e = BURROW_NO_ERROR;
            EcdsaPrivateKey *ec = x509_parse_ec_private_key(a, der, &e);
            if (BURROW_FAILED(e)) {
                BURROW_OUT(err, fmt_errorf_v("tls: failed to parse private key: %w",
                                             pkcs8_err));
                return (Any){NULL, NULL};
            }
            key = BURROW_ANY(TYPE_ECDSA_PRIVATE_KEY, ec);
        }
    }
    if (key.t == TYPE_RSA_PRIVATE_KEY || key.t == TYPE_ECDSA_PRIVATE_KEY ||
        key.t == TYPE_ED25519_PRIVATE_KEY || key.t == TYPE_MLDSA_PRIVATE_KEY) {
        BURROW_OUT(err, BURROW_NO_ERROR);
        return key;
    }
    BURROW_OUT(err, tls_key_pair_error("tls: found unknown private key type in "
                                       "PKCS#8 wrapping"));
    return (Any){NULL, NULL};
}

/* Whether priv, which the switch in X509KeyPair has found to be the key type
 * that goes with pub, is the private half of pub. */
static bool tls_key_pair_matches(Alloc *a, CryptoPrivateKey priv, CryptoPublicKey pub) {
    if (pub.t == TYPE_RSA_PUBLIC_KEY)
        return rsa_public_key_equal(&((const RsaPrivateKey *)priv.data)->public_key,
                                    pub);
    if (pub.t == TYPE_ECDSA_PUBLIC_KEY)
        return ecdsa_public_key_equal(&((const EcdsaPrivateKey *)priv.data)->public_key,
                                      pub);
    if (pub.t == TYPE_ED25519_PUBLIC_KEY) {
        Ed25519PublicKey p =
            ed25519_private_key_public(*(const Ed25519PrivateKey *)priv.data, a);
        return ed25519_public_key_equal(p, pub);
    }
    return mldsa_public_key_equal(mldsa_private_key_public_key(priv.data), pub);
}

TlsCertificate tls_x509_key_pair(Alloc *a, Slice cert_pem_block, Slice key_pem_block,
                                 Error *err) {
    TlsCertificate cert = {0};
    cert.certificate = slice_nil(TYPE_BYTES);
    Slice skipped_block_types = slice_nil(TYPE_STRING);
    for (;;) {
        PemBlock *cert_der_block = pem_decode(a, cert_pem_block, &cert_pem_block);
        if (cert_der_block == NULL)
            break;
        if (str_eq(cert_der_block->type, BURROW_S("CERTIFICATE")))
            cert.certificate =
                slice_append(a, cert.certificate, &cert_der_block->bytes, 1);
        else
            skipped_block_types =
                slice_append(a, skipped_block_types, &cert_der_block->type, 1);
    }

    if (cert.certificate.len == 0) {
        if (skipped_block_types.len == 0)
            return tls_key_pair_fail(err, tls_key_pair_error("tls: failed to find any "
                                                             "PEM data in certificate "
                                                             "input"));
        if (skipped_block_types.len == 1 &&
            strings_has_suffix(BURROW_AT(Str, skipped_block_types, 0),
                               BURROW_S("PRIVATE KEY")))
            return tls_key_pair_fail(
                err, tls_key_pair_error("tls: failed to find certificate PEM data in "
                                        "certificate input, but did find a private "
                                        "key; PEM inputs may have been switched"));
        return tls_key_pair_fail(
            err, fmt_errorf_v("tls: failed to find \"CERTIFICATE\" PEM block in "
                              "certificate input after skipping PEM blocks of the "
                              "following types: %v",
                              skipped_block_types));
    }

    skipped_block_types.len = 0;
    PemBlock *key_der_block;
    for (;;) {
        key_der_block = pem_decode(a, key_pem_block, &key_pem_block);
        if (key_der_block == NULL) {
            if (skipped_block_types.len == 0)
                return tls_key_pair_fail(err, tls_key_pair_error("tls: failed to find "
                                                                 "any PEM data in key "
                                                                 "input"));
            if (skipped_block_types.len == 1 &&
                str_eq(BURROW_AT(Str, skipped_block_types, 0), BURROW_S("CERTIFICATE")))
                return tls_key_pair_fail(
                    err,
                    tls_key_pair_error("tls: found a certificate rather than a key "
                                       "in the PEM for the private key"));
            return tls_key_pair_fail(
                err, fmt_errorf_v("tls: failed to find PEM block with type ending in "
                                  "\"PRIVATE KEY\" in key input after skipping PEM "
                                  "blocks of the following types: %v",
                                  skipped_block_types));
        }
        if (str_eq(key_der_block->type, BURROW_S("PRIVATE KEY")) ||
            strings_has_suffix(key_der_block->type, BURROW_S(" PRIVATE KEY")))
            break;
        skipped_block_types =
            slice_append(a, skipped_block_types, &key_der_block->type, 1);
    }

    /* We don't need to parse the public key for TLS, but we so do anyway
     * to check that it looks sane and matches the private key. */
    Error e = BURROW_NO_ERROR;
    X509Certificate *x509_cert =
        x509_parse_certificate(a, BURROW_AT(Slice, cert.certificate, 0), &e);
    if (BURROW_FAILED(e))
        return tls_key_pair_fail(err, e);
    cert.leaf = x509_cert;

    cert.private_key = tls_parse_private_key(a, key_der_block->bytes, &e);
    if (BURROW_FAILED(e))
        return tls_key_pair_fail(err, e);

    CryptoPublicKey pub = x509_cert->public_key;
    const Type *want;
    if (pub.t == TYPE_RSA_PUBLIC_KEY)
        want = TYPE_RSA_PRIVATE_KEY;
    else if (pub.t == TYPE_ECDSA_PUBLIC_KEY)
        want = TYPE_ECDSA_PRIVATE_KEY;
    else if (pub.t == TYPE_ED25519_PUBLIC_KEY)
        want = TYPE_ED25519_PRIVATE_KEY;
    else if (pub.t == TYPE_MLDSA_PUBLIC_KEY)
        want = TYPE_MLDSA_PRIVATE_KEY;
    else
        return tls_key_pair_fail(
            err, tls_key_pair_error("tls: unknown public key algorithm"));
    if (cert.private_key.t != want)
        return tls_key_pair_fail(err, tls_key_pair_error("tls: private key type does "
                                                         "not match public key type"));
    if (!tls_key_pair_matches(a, cert.private_key, pub))
        return tls_key_pair_fail(err, tls_key_pair_error("tls: private key does not "
                                                         "match public key"));

    BURROW_OUT(err, BURROW_NO_ERROR);
    return cert;
}
