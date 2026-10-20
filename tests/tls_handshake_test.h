/* crypto/tls: the test configurations of handshake_test.go, which the tests
 * of more than one file run on.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#ifndef BURROW_TESTS_TLS_HANDSHAKE_TEST_H
#define BURROW_TESTS_TLS_HANDSHAKE_TEST_H

#include "burrow/burrow.h"
#include "burrow/crypto/tls.h"
#include "burrow/crypto/x509.h"
#include "tls_test_gen.h"

#include <string.h>

static inline Time test_time(void *env) {
    (void)env;
    return time_from_unix(1476984729, 0);
}

/* Not in Go: testConfigServer, made in a. tls_config_release gives back the
 * session ticket keys it comes to hold. */
static inline TlsConfig test_config_server(Alloc *a) {
    TlsConfig config;
    memset(&config, 0, sizeof config);
    config.time = (TlsTimeFunc){test_time, NULL};
    Slice certs = slice_make(a, TYPE_TLS_CERTIFICATE, 7, 7);
    TlsCertificate *c = certs.p;
    c[0] = parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem);
    c[1] = parse_test_cert(a, test_rsa2048_cert_pem, test_rsa2048_key_pem);
    c[2] = parse_test_cert(a, test_ed25519_cert_pem, test_ed25519_key_pem);
    c[3] = parse_test_cert(a, test_sni_cert_pem, test_sni_key_pem);
    c[4] = parse_test_cert(a, test_mldsa44_cert_pem, test_mldsa44_key_pem);
    c[5] = parse_test_cert(a, test_mldsa65_cert_pem, test_mldsa65_key_pem);
    c[6] = parse_test_cert(a, test_mldsa87_cert_pem, test_mldsa87_key_pem);
    config.certificates = certs;
    config.client_cas = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(
        config.client_cas,
        parse_test_cert(a, test_client_root_cert_pem, test_client_root_key_pem).leaf);
    return config;
}

/* Not in Go: testConfigClient, made in a. */
static inline TlsConfig test_config_client(Alloc *a) {
    TlsConfig config;
    memset(&config, 0, sizeof config);
    config.time = (TlsTimeFunc){test_time, NULL};
    Slice certs = slice_make(a, TYPE_TLS_CERTIFICATE, 6, 6);
    TlsCertificate *c = certs.p;
    c[0] = parse_test_cert(a, test_client_ecdsap256_cert_pem,
                           test_client_ecdsap256_key_pem);
    c[1] =
        parse_test_cert(a, test_client_rsa2048_cert_pem, test_client_rsa2048_key_pem);
    c[2] =
        parse_test_cert(a, test_client_ed25519_cert_pem, test_client_ed25519_key_pem);
    c[3] =
        parse_test_cert(a, test_client_mldsa44_cert_pem, test_client_mldsa44_key_pem);
    c[4] =
        parse_test_cert(a, test_client_mldsa65_cert_pem, test_client_mldsa65_key_pem);
    c[5] =
        parse_test_cert(a, test_client_mldsa87_cert_pem, test_client_mldsa87_key_pem);
    config.certificates = certs;
    config.root_cas = x509_new_cert_pool(a);
    x509_cert_pool_add_cert(
        config.root_cas,
        parse_test_cert(a, test_root_cert_pem, test_root_key_pem).leaf);
    config.server_name = BURROW_S("test.golang.example");
    return config;
}

#endif /* BURROW_TESTS_TLS_HANDSHAKE_TEST_H */
