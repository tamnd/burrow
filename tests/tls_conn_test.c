/* Derived from Go's src/crypto/tls/conn_test.go.
 * Go source: go1.27.1.
 *
 * TestRoundUp, TestRemovePadding and TestCertificateSelection for now. The
 * rest of the file needs a Conn and a handshake, which come in later. The hex
 * certificates are in tls_test_gen.h.
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

static void TestRoundUp(TestingT *t) {
    if (burrow__tls_round_up(0, 16) != 0 || burrow__tls_round_up(1, 16) != 16 ||
        burrow__tls_round_up(15, 16) != 16 || burrow__tls_round_up(16, 16) != 16 ||
        burrow__tls_round_up(17, 16) != 32) {
        testing_t_error_v(t, "roundUp broken");
    }
}

/* will be initialized with {0, 255, 255, ..., 255} */
static Byte padding255_bad[256];

/* will be initialized with {255, 255, 255, ..., 255} */
static Byte padding255_good[256] = {255};

typedef struct PaddingTest {
    Byte in[6];
    Int len;
    bool good;
    Int expected_len;
} PaddingTest;

static const PaddingTest padding_tests[] = {
    {{1, 2, 3, 4, 0}, 5, true, 4},       {{1, 2, 3, 4, 0, 1}, 6, false, 0},
    {{1, 2, 3, 4, 99, 99}, 6, false, 0}, {{1, 2, 3, 4, 1, 1}, 6, true, 4},
    {{1, 2, 3, 2, 2, 2}, 6, true, 3},    {{1, 2, 3, 3, 3, 3}, 6, true, 2},
    {{1, 2, 3, 4, 3, 3}, 6, false, 0},   {{1, 4, 4, 4, 4, 4}, 6, true, 1},
    {{5, 5, 5, 5, 5, 5}, 6, true, 0},    {{6, 6, 6, 6, 6, 6}, 6, false, 0},
};

static void TestRemovePadding(TestingT *t) {
    for (int i = 1; i < 256; i++) {
        padding255_bad[i] = 255;
        padding255_good[i] = 255;
    }
    Int ntests = (Int)(sizeof padding_tests / sizeof padding_tests[0]) + 2;
    for (Int i = 0; i < ntests; i++) {
        Slice in;
        bool test_good;
        Int expected_len = 0;
        if (i < ntests - 2) {
            const PaddingTest *test = &padding_tests[i];
            in = slice_from((void *)(uintptr_t)test->in, test->len, test->len,
                            TYPE_BYTE);
            test_good = test->good;
            expected_len = test->expected_len;
        } else if (i == ntests - 2) {
            in = slice_from(padding255_bad, 256, 256, TYPE_BYTE);
            test_good = false;
        } else {
            in = slice_from(padding255_good, 256, 256, TYPE_BYTE);
            test_good = true;
        }
        Byte good;
        Int padding_len = burrow__tls_extract_padding(in, &good);
        Byte expected_good = 255;
        if (!test_good)
            expected_good = 0;
        if (good != expected_good)
            testing_t_errorf_v(t, "#%d: wrong validity, want:%d got:%d", i,
                               (int)expected_good, (int)good);
        if (good == 255 && in.len - padding_len != expected_len)
            testing_t_errorf_v(t, "#%d: got %d, want %d", i, in.len - padding_len,
                               expected_len);
    }
}

/* Not in Go: Go tests the record layer through whole handshakes, which are not
 * here yet. This sends three records through a halfConn with every suite and
 * reads them back through another one with the same keys, then checks that a
 * record with one byte changed is refused. */

/* Not in Go. Key material of n bytes that differs for each seed. */
static Slice key_bytes(Alloc *a, Int n, Byte seed) {
    Slice b = slice_make(a, TYPE_BYTE, n, n);
    for (Int i = 0; i < n; i++)
        ((Byte *)b.p)[i] = (Byte)(seed + 7 * i);
    return b;
}

/* Not in Go. Sets hc up with suite's keys, for reading or for writing. */
static void prepare_suite(TestingT *t, TlsHalfConn *hc, const TlsSuite *suite,
                          uint16_t version, bool is_read) {
    Alloc *a = burrow__tls_half_conn_next_alloc(hc);
    if (a == NULL)
        testing_t_fatal_v(t, "out of memory");
    Slice key = key_bytes(a, suite->key_len, 1);
    Slice iv = key_bytes(a, suite->iv_len, 2);
    TlsCipher c;
    Hash mac = {0};
    if (suite->aead != NULL) {
        c = suite->aead(a, key, iv);
    } else {
        c = suite->cipher(a, key, iv, is_read);
        mac = suite->mac(a, key_bytes(a, suite->mac_len, 3));
    }
    burrow__tls_half_conn_prepare_cipher_spec(hc, version, c, mac);
    Error err = burrow__tls_half_conn_change_cipher_spec(hc);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, "changeCipherSpec: %v", err);
}

/* Not in Go. Writes three records from out and reads them from in. */
static void round_trip(TestingT *t, Alloc *a, Str name, TlsHalfConn *out,
                       TlsHalfConn *in, uint8_t typ) {
    static const char *const payloads[] = {"hello", "",
                                           "a longer record than the others"};
    for (int i = 0; i < 3; i++) {
        /* writeRecordLocked puts the plaintext length in the header before
         * encrypt, and up to TLS 1.2 the MAC covers it. */
        Str want = str_from_cstr(payloads[i]);
        Byte header[TLS_RECORD_HEADER_LEN] = {typ, 3, 3, (Byte)(want.len >> 8),
                                              (Byte)want.len};
        Slice record = slice_make(a, TYPE_BYTE, 0, 512);
        record = slice_append(a, record, header, TLS_RECORD_HEADER_LEN);
        Slice payload =
            slice_from((void *)(uintptr_t)want.p, want.len, want.len, TYPE_BYTE);
        Error err = BURROW_NO_ERROR;
        record = burrow__tls_half_conn_encrypt(out, a, record, payload,
                                               crypto_rand_reader, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: encrypt: %v", name, err);
            return;
        }
        uint8_t got_typ = 0;
        Slice got = burrow__tls_half_conn_decrypt(in, record, &got_typ, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: record %d: decrypt: %v", name, i, err);
            return;
        }
        if (got_typ != typ)
            testing_t_errorf_v(t, "%s: record %d: type %d, want %d", name, i,
                               (int)got_typ, (int)typ);
        if (got.len != want.len ||
            (want.len > 0 && memcmp(got.p, want.p, (size_t)want.len) != 0))
            testing_t_errorf_v(t, "%s: record %d: got %q, want %q", name, i,
                               str_from_bytes(got.p, got.len), want);
    }

    Byte header[TLS_RECORD_HEADER_LEN] = {typ, 3, 3, 0, 8};
    Slice record = slice_make(a, TYPE_BYTE, 0, 512);
    record = slice_append(a, record, header, TLS_RECORD_HEADER_LEN);
    Error err = BURROW_NO_ERROR;
    static Byte tampered[] = "tampered";
    record = burrow__tls_half_conn_encrypt(out, a, record,
                                           slice_from(tampered, 8, 8, TYPE_BYTE),
                                           crypto_rand_reader, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%s: encrypt: %v", name, err);
        return;
    }
    ((Byte *)record.p)[record.len - 1] ^= 1;
    uint8_t got_typ = 0;
    uint8_t alert = 0;
    burrow__tls_half_conn_decrypt(in, record, &got_typ, &err);
    if (!burrow__tls_is_alert(err, &alert) || alert != TLS_ALERT_BAD_RECORD_MAC)
        testing_t_errorf_v(t, "%s: changed record: got %v, want %v", name, err,
                           burrow__tls_alert(a, TLS_ALERT_BAD_RECORD_MAC));
}

/* Not in Go. */
static void TestHalfConnRoundTrip(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const uint16_t versions[] = {TLS_VERSION_TLS10, TLS_VERSION_TLS11,
                                        TLS_VERSION_TLS12};
    for (Int i = 0; i < burrow__tls_ncipher_suites; i++) {
        const TlsSuite *suite = &burrow__tls_cipher_suites[i];
        for (int v = 0; v < 3; v++) {
            if ((suite->flags & TLS_SUITE_TLS12) != 0 &&
                versions[v] != TLS_VERSION_TLS12)
                continue;
            Str name = tls_cipher_suite_name(a, suite->id);
            TlsHalfConn out, in;
            burrow__tls_half_conn_init(&out, NULL);
            burrow__tls_half_conn_init(&in, NULL);
            prepare_suite(t, &out, suite, versions[v], false);
            prepare_suite(t, &in, suite, versions[v], true);
            round_trip(t, a, name, &out, &in, TLS_RECORD_TYPE_APPLICATION_DATA);
            burrow__tls_half_conn_free(&out);
            burrow__tls_half_conn_free(&in);
        }
    }
    for (Int i = 0; i < burrow__tls_ncipher_suites_tls13; i++) {
        const TlsSuiteTLS13 *suite = &burrow__tls_cipher_suites_tls13[i];
        Str name = tls_cipher_suite_name(a, suite->id);
        Slice secret = key_bytes(a, crypto_hash_size(suite->hash), 4);
        TlsHalfConn out, in;
        burrow__tls_half_conn_init(&out, NULL);
        burrow__tls_half_conn_init(&in, NULL);
        out.version = TLS_VERSION_TLS13;
        in.version = TLS_VERSION_TLS13;
        Error err = burrow__tls_half_conn_set_traffic_secret(
            &out, suite, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION, secret);
        if (BURROW_OK(err))
            err = burrow__tls_half_conn_set_traffic_secret(
                &in, suite, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION, secret);
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, "setTrafficSecret: %v", err);
        round_trip(t, a, name, &out, &in, TLS_RECORD_TYPE_HANDSHAKE);
        burrow__tls_half_conn_free(&out);
        burrow__tls_half_conn_free(&in);
    }
    arena_free(&ar);
}

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

#define TESTS(X)                                                                       \
    X(TestRoundUp)                                                                     \
    X(TestRemovePadding)                                                               \
    X(TestHalfConnRoundTrip)                                                           \
    X(TestCertificateSelection)

TESTING_MAIN(TESTS)
