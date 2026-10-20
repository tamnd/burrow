/* Derived from Go's src/crypto/tls/conn_test.go.
 * Go source: go1.27.1.
 *
 * Only TestCertificateSelection for now. The rest of the file needs the record
 * layer, which comes in later. The hex certificates are in tls_test_gen.h.
 *
 * Copyright 2010 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto/tls.h"
#include "burrow/encoding/hex.h"
#include "tls_test_gen.h"

#include <stdint.h>
#include <string.h>

/* fromHex, from a. */
static Slice from_hex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("fromHex: bad hex"));
    return b;
}

typedef struct CertificateSelection {
    TestingT *t;
    TlsConfig *config;
} CertificateSelection;

static Int pointer_to_index(const CertificateSelection *s, const TlsCertificate *c) {
    const TlsCertificate *certs = (const TlsCertificate *)s->config->certificates.p;
    for (Int i = 0; i < s->config->certificates.len; i++) {
        if (c == &certs[i])
            return i;
    }
    return -1;
}

static TlsCertificate *certificate_for_name(const CertificateSelection *s, Str name) {
    TlsClientHelloInfo client_hello = {0};
    client_hello.server_name = name;
    Error err = BURROW_NO_ERROR;
    TlsCertificate *cert =
        burrow__tls_config_get_certificate(s->config, &client_hello, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(s->t, "unable to get certificate for name '%s': %s", name,
                           err);
        return NULL;
    }
    return cert;
}

static void TestCertificateSelection(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    const char *const hex[3] = {cert_example_com_hex, cert_wildcard_example_com_hex,
                                cert_foo_example_com_hex};
    TlsCertificate certs[3];
    memset(certs, 0, sizeof(certs));
    for (int i = 0; i < 3; i++) {
        certs[i].certificate = slice_make(a, TYPE_BYTES, 1, 1);
        BURROW_AT(Slice, certs[i].certificate, 0) = from_hex(a, hex[i]);
    }
    TlsConfig config = {0};
    config.certificates = slice_from(certs, 3, 3, TYPE_TLS_CERTIFICATE);

    Error err = tls_config_build_name_to_certificate(&config, a);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, err);

    CertificateSelection s = {t, &config};
    Int n = pointer_to_index(&s, certificate_for_name(&s, BURROW_S("example.com")));
    if (n != 0)
        testing_t_errorf_v(t, "example.com returned certificate %d, not 0", n);
    n = pointer_to_index(&s, certificate_for_name(&s, BURROW_S("bar.example.com")));
    if (n != 1)
        testing_t_errorf_v(t, "bar.example.com returned certificate %d, not 1", n);
    n = pointer_to_index(&s, certificate_for_name(&s, BURROW_S("foo.example.com")));
    if (n != 2)
        testing_t_errorf_v(t, "foo.example.com returned certificate %d, not 2", n);
    n = pointer_to_index(&s, certificate_for_name(&s, BURROW_S("foo.bar.example.com")));
    if (n != 0)
        testing_t_errorf_v(t, "foo.bar.example.com returned certificate %d, not 0", n);
    arena_free(&ar);
}

#define TESTS(X) X(TestCertificateSelection)

TESTING_MAIN(TESTS)
