/* Derived from Go's src/crypto/tls/conn_test.go.
 * Go source: go1.27.1.
 *
 * The hex certificates are in tls_test_gen.h, and the configurations, localPipe
 * and testHandshake are in tls_handshake_test.h.
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
#include "tls_handshake_test.h"
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

/* Not in Go: Go tests the record layer only through whole handshakes. This
 * sends three records through a halfConn with every suite and reads them back
 * through another one with the same keys, then checks that a record with one
 * byte changed is refused. */

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

/* Not in Go: one case of TestBrokenCertificateSkipped. */
typedef struct BrokenCertificateCase {
    const char *name;
    bool build_index;
} BrokenCertificateCase;

static const BrokenCertificateCase broken_certificate_cases[] = {
    {"BuildNameToCertificate", true},
    {"SupportsCertificate", false},
};

static void broken_certificate_skipped(void *env, TestingT *t) {
    const BrokenCertificateCase *test = (const BrokenCertificateCase *)env;
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const char broken[] = "not a valid X.509 certificate";
    Int n = (Int)sizeof broken - 1;
    TlsCertificate broken_cert;
    memset(&broken_cert, 0, sizeof broken_cert);
    broken_cert.certificate = slice_make(a, TYPE_BYTES, 1, 1);
    BURROW_AT(Slice, broken_cert.certificate, 0) =
        slice_from((Byte *)(uintptr_t)broken, n, n, TYPE_BYTE);

    TlsConfig server_config = test_config_server(a);
    Slice certs = slice_make(a, TYPE_TLS_CERTIFICATE, 2, 2);
    TlsCertificate *c = certs.p;
    c[0] = broken_cert;
    c[1] = parse_test_cert(a, test_ecdsap256_cert_pem, test_ecdsap256_key_pem);
    server_config.certificates = certs;
    if (test->build_index) {
        Error err = tls_config_build_name_to_certificate(&server_config, a);
        if (BURROW_FAILED(err))
            testing_t_fatal_v(t, err);
    }
    TlsConfig client_config = test_config_client(a);
    Handshake h;
    test_handshake(t, &client_config, &server_config, &h);
    if (BURROW_FAILED(h.err))
        testing_t_fatalf_v(t, "handshake failed: %v", h.err);
    /* Not in Go: Go would panic on an empty PeerCertificates. */
    if (h.client_state.peer_certificates.len == 0)
        testing_t_fatalf_v(t, "handshake succeeded but no certificate was sent");
    const X509Certificate *peer =
        *(X509Certificate *const *)h.client_state.peer_certificates.p;
    if (!x509_certificate_equal(peer, c[1].leaf))
        testing_t_fatalf_v(t, "handshake succeeded but wrong certificate was used");
    handshake_free(&h);
    tls_config_release(&client_config);
    tls_config_release(&server_config);
    arena_free(&ar);
}

/* TestBrokenCertificateSkipped checks that a Certificate in Config.Certificates
 * whose leaf doesn't parse as X.509 doesn't prevent the next, valid certificate
 * from being selected. It exercises both the legacy BuildNameToCertificate path
 * and the SupportsCertificate-based selection. */
static void TestBrokenCertificateSkipped(TestingT *t) {
    Int n = (Int)(sizeof broken_certificate_cases / sizeof broken_certificate_cases[0]);
    for (Int i = 0; i < n; i++) {
        const BrokenCertificateCase *test = &broken_certificate_cases[i];
        testing_t_run(t, str_from_cstr(test->name),
                      BURROW_FN(TestingTFunc, broken_certificate_skipped,
                                (void *)(uintptr_t)test));
    }
}

/* Not in Go: the goroutine of runDynamicRecordSizingTest. It performs a TLS
 * handshake over c and then reads TLS records until EOF, and keeps the size of
 * each one in record_sizes, and done says it got to the end, which Go says
 * with a nil slice. The sizes are made in ar. */
typedef struct RecordSizingClient {
    TestingT *t;
    NetConn c;
    TlsConfig *config;
    TlsConn *tls_conn;
    Chan *handshake_done;
    Slice record_sizes;
    bool done;
    Arena ar;
} RecordSizingClient;

static void record_sizing_client(void *env) {
    RecordSizingClient *j = (RecordSizingClient *)env;
    Alloc *a = arena_allocator(&j->ar);
    j->tls_conn = tls_client(heap_allocator(), j->c, j->config);
    if (j->tls_conn == NULL) {
        testing_t_errorf_v(j->t, "Error from client handshake: %v",
                           burrow_err_out_of_memory);
        (void)j->c.vt->closer.close(j->c.data);
        return;
    }
    Error err = tls_conn_handshake(j->tls_conn);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(j->t, "Error from client handshake: %v", err);
        (void)j->c.vt->closer.close(j->c.data);
        return;
    }
    chan_close(j->handshake_done);

    Byte record_header[TLS_RECORD_HEADER_LEN];
    Slice record = {0};
    Slice record_sizes = slice_make(a, TYPE_INT, 0, 0);
    IoReader r = net_conn_as_io_reader(j->c);

    for (;;) {
        Int n = io_read_full(r,
                             slice_from(record_header, TLS_RECORD_HEADER_LEN,
                                        TLS_RECORD_HEADER_LEN, TYPE_BYTE),
                             &err);
        if (err.vt == io_eof.vt && err.data == io_eof.data)
            break;
        if (BURROW_FAILED(err) || n != TLS_RECORD_HEADER_LEN) {
            testing_t_errorf_v(j->t, "io.ReadFull = %d, %v", n, err);
            (void)j->c.vt->closer.close(j->c.data);
            return;
        }

        Int length = (Int)record_header[3] << 8 | (Int)record_header[4];
        if (record.len < length)
            record = slice_make(a, TYPE_BYTE, length, length);

        n = io_read_full(r, slice_sub(record, 0, length), &err);
        if (BURROW_FAILED(err) || n != length) {
            testing_t_errorf_v(j->t, "io.ReadFull = %d, %v", n, err);
            (void)j->c.vt->closer.close(j->c.data);
            return;
        }

        Int size = TLS_RECORD_HEADER_LEN + length;
        record_sizes = slice_append(a, record_sizes, &size, 1);
    }

    j->record_sizes = record_sizes;
    j->done = true;
    (void)j->c.vt->closer.close(j->c.data);
}

/* Run with multiple crypto configs to test the logic for computing TLS record
 * overheads.
 *
 * Not in Go: the config is the server's, and it is changed in place where Go
 * changes a clone of it. */
static void run_dynamic_record_sizing_test(TestingT *t, TlsConfig *server_config,
                                           Alloc *a) {
    NetConn client_conn, server_conn;
    local_pipe(a, &client_conn, &server_conn);

    server_config->dynamic_record_sizing_disabled = false;
    TlsConn *tls_conn = tls_server(heap_allocator(), server_conn, server_config);
    if (tls_conn == NULL)
        testing_t_fatal_v(t, "out of memory");

    TlsConfig client_config = test_config_client(a);
    client_config.min_version = server_config->min_version;
    client_config.max_version = server_config->max_version;
    client_config.cipher_suites = server_config->cipher_suites;

    RecordSizingClient j;
    memset(&j, 0, sizeof j);
    j.t = t;
    j.c = client_conn;
    j.config = &client_config;
    j.handshake_done = chan_make(heap_allocator(), TYPE_BOOL, 0);
    if (j.handshake_done == NULL)
        testing_t_fatal_v(t, "out of memory");
    arena_init(&j.ar, NULL, 0);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, record_sizing_client, &j));

    bool failed = false;
    Error err = tls_conn_handshake(tls_conn);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Error from server handshake: %s", err);
        failed = true;
        (void)server_conn.vt->closer.close(server_conn.data);
    } else {
        (void)chan_recv(j.handshake_done, NULL);

        /* The server writes these plaintexts in order. */
        Int n =
            TLS_RECORD_SIZE_BOOST_THRESHOLD + TLS_MAX_PLAINTEXT * 2 + TLS_MAX_PLAINTEXT;
        Slice plaintext = slice_make(a, TYPE_BYTE, n, n);
        Byte *p = plaintext.p;
        memset(p, 'x', (size_t)TLS_RECORD_SIZE_BOOST_THRESHOLD);
        memset(p + TLS_RECORD_SIZE_BOOST_THRESHOLD, 'y', (size_t)TLS_MAX_PLAINTEXT * 2);
        memset(p + TLS_RECORD_SIZE_BOOST_THRESHOLD + TLS_MAX_PLAINTEXT * 2, 'z',
               (size_t)TLS_MAX_PLAINTEXT);

        (void)tls_conn_write(tls_conn, plaintext, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "Error from server write: %s", err);
            failed = true;
            (void)server_conn.vt->closer.close(server_conn.data);
        } else if (BURROW_FAILED(err = tls_conn_close(tls_conn))) {
            testing_t_errorf_v(t, "Error from server close: %s", err);
            failed = true;
        }
    }

    /* Not in Go: wait for the goroutine here, which Go defers. */
    sync_wait_group_wait(&wg);
    Slice record_sizes = j.record_sizes;
    if (!failed && !j.done) {
        testing_t_errorf_v(t, "Client encountered an error");
        failed = true;
    }

    if (!failed) {
        /* Drop the size of the second to last record, which is likely to be
         * truncated, and the last record, which is a close_notify alert. */
        record_sizes = slice_sub(record_sizes, 0, record_sizes.len - 2);

        /* recordSizes should contain a series of records smaller than
         * tcpMSSEstimate followed by some larger than maxPlaintext. */
        bool seen_large_record = false;
        const Int *sizes = record_sizes.p;
        for (Int i = 0; i < record_sizes.len && !failed; i++) {
            Int size = sizes[i];
            if (!seen_large_record) {
                if (size > (i + 1) * TLS_TCP_MSS_ESTIMATE) {
                    testing_t_errorf_v(t,
                                       "Record #%d has size %d, which is too large "
                                       "too soon",
                                       i, size);
                    failed = true;
                }
                if (size >= TLS_MAX_PLAINTEXT)
                    seen_large_record = true;
            } else if (size <= TLS_MAX_PLAINTEXT) {
                testing_t_errorf_v(t, "Record #%d has size %d but should be full sized",
                                   i, size);
                failed = true;
            }
        }

        if (!failed && !seen_large_record)
            testing_t_errorf_v(t, "No large records observed");
    }

    tls_conn_free(j.tls_conn);
    tls_conn_free(tls_conn);
    chan_free(j.handshake_done);
    arena_free(&j.ar);
    local_pipe_free(client_conn);
    tls_config_release(&client_config);
    if (failed)
        testing_t_fail_now(t);
}

static void TestDynamicRecordSizingWithStreamCipher(TestingT *t) {
    skip_fips(t); /* No RC4 in FIPS mode. */

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static uint16_t suites[] = {TLS_TLS_RSA_WITH_RC4_128_SHA};
    TlsConfig config = test_config_server(a);
    config.max_version = TLS_VERSION_TLS12;
    config.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    run_dynamic_record_sizing_test(t, &config, a);
    tls_config_release(&config);
    arena_free(&ar);
}

static void TestDynamicRecordSizingWithCBC(TestingT *t) {
    skip_fips(t); /* No CBC cipher suites in defaultCipherSuitesFIPS. */

    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static uint16_t suites[] = {TLS_TLS_RSA_WITH_AES_256_CBC_SHA};
    TlsConfig config = test_config_server(a);
    config.max_version = TLS_VERSION_TLS12;
    config.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    run_dynamic_record_sizing_test(t, &config, a);
    tls_config_release(&config);
    arena_free(&ar);
}

static void TestDynamicRecordSizingWithAEAD(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static uint16_t suites[] = {TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256};
    TlsConfig config = test_config_server(a);
    config.max_version = TLS_VERSION_TLS12;
    config.cipher_suites = slice_from(suites, 1, 1, TYPE_UINT16);
    run_dynamic_record_sizing_test(t, &config, a);
    tls_config_release(&config);
    arena_free(&ar);
}

static void TestDynamicRecordSizingWithTLSv13(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    TlsConfig config = test_config_server(a);
    run_dynamic_record_sizing_test(t, &config, a);
    tls_config_release(&config);
    arena_free(&ar);
}

/* ------------------------------------------------------------------- Conn */

/* hairpinConn is a net.Conn that makes a “hairpin” call when closed, back into
 * the tls.Conn which is calling it. */
typedef struct HairpinConn {
    NetConn conn;
    TlsConn *tls_conn;
} HairpinConn;

static Int hairpin_read(void *self, Slice b, Error *err) {
    HairpinConn *h = (HairpinConn *)self;
    return h->conn.vt->reader.read(h->conn.data, b, err);
}

static Int hairpin_write(void *self, Slice b, Error *err) {
    HairpinConn *h = (HairpinConn *)self;
    return h->conn.vt->writer.write(h->conn.data, b, err);
}

static Error hairpin_close(void *self) {
    HairpinConn *h = (HairpinConn *)self;
    (void)tls_conn_connection_state(h->tls_conn);
    return BURROW_NO_ERROR;
}

static const NetConnVT hairpin_vt = {
    {NULL, hairpin_read},
    {NULL, hairpin_write},
    {NULL, hairpin_close},
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
};

static TlsCertificate *unreachable_get_certificate(void *env, TlsClientHelloInfo *chi,
                                                   Error *err) {
    (void)env;
    (void)chi;
    (void)err;
    panic_str(BURROW_S("unreachable"));
}

/* Not in Go: localPipe is a net.Pipe here. */
static void TestHairpinInClose(TestingT *t) {
    /* This tests that the underlying net.Conn can call back into the
     * tls.Conn when being closed without deadlocking. */
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn client, server;
    net_pipe(a, &client, &server);

    TlsConfig config;
    memset(&config, 0, sizeof config);
    HairpinConn conn = {client, NULL};
    config.get_certificate = (TlsGetCertificateFunc){unreachable_get_certificate, NULL};
    TlsConn *tls_conn = tls_server(a, (NetConn){&hairpin_vt, &conn}, &config);
    if (tls_conn == NULL)
        testing_t_fatal_v(t, "out of memory");
    conn.tls_conn = tls_conn;

    /* This call should not deadlock. */
    (void)tls_conn_close(tls_conn);

    (void)server.vt->closer.close(server.data);
    (void)client.vt->closer.close(client.data);
    tls_conn_free(tls_conn);
    net_pipe_free(client);
    arena_free(&ar);
}

/* Not in Go: the goroutine of TestRecordBadVersionTLS13. */
typedef struct BadVersionClient {
    TestingT *t;
    NetConn c;
    TlsConfig *config;
    TlsConn *tls_conn;
} BadVersionClient;

static void bad_version_client(void *env) {
    BadVersionClient *j = (BadVersionClient *)env;
    j->tls_conn = tls_client(heap_allocator(), j->c, j->config);
    if (j->tls_conn == NULL) {
        testing_t_errorf_v(j->t, "Error from client handshake: %v",
                           burrow_err_out_of_memory);
        return;
    }
    Error err = tls_conn_handshake(j->tls_conn);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(j->t, "Error from client handshake: %v", err);
        return;
    }
    j->tls_conn->vers = 0x1111;
    Byte one[1] = {1};
    (void)tls_conn_write(j->tls_conn, slice_from(one, 1, 1, TYPE_BYTE), &err);
}

static void TestRecordBadVersionTLS13(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn client, server;
    local_pipe(a, &client, &server);

    TlsConfig client_config = test_config_client(a);
    client_config.min_version = TLS_VERSION_TLS13;
    client_config.max_version = TLS_VERSION_TLS13;
    TlsConfig server_config = test_config_server(a);
    server_config.min_version = TLS_VERSION_TLS13;
    server_config.max_version = TLS_VERSION_TLS13;

    BadVersionClient j = {t, client, &client_config, NULL};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, bad_version_client, &j));

    TlsConn *tls_conn = tls_server(heap_allocator(), server, &server_config);
    if (tls_conn == NULL)
        testing_t_fatal_v(t, "out of memory");
    Error err = tls_conn_handshake(tls_conn);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "Error from client handshake: %v", err);
    } else {
        Str expected_err = BURROW_S(
            "tls: received record with version 1111 when expecting version 303");

        Byte buf[10];
        (void)tls_conn_read(tls_conn, slice_from(buf, 10, 10, TYPE_BYTE), &err);
        /* Not in Go: Go would panic on a nil error. */
        if (!BURROW_FAILED(err) || !str_eq(error_text(err), expected_err))
            testing_t_errorf_v(t, "unexpected error: got %q, want %q", err,
                               expected_err);
    }

    (void)server.vt->closer.close(server.data);
    (void)client.vt->closer.close(client.data);
    sync_wait_group_wait(&wg);
    tls_conn_free(tls_conn);
    tls_conn_free(j.tls_conn);
    local_pipe_free(client);
    tls_config_release(&client_config);
    tls_config_release(&server_config);
    arena_free(&ar);
}

/* Not in Go: the goroutine of TestKeyUpdateSpamPostHandshakeTLS13. The traffic
 * secrets are made in ar. */
typedef struct KeyUpdateSpamClient {
    TestingT *t;
    NetConn c;
    TlsConfig *config;
    TlsConn *tls_conn;
    Arena ar;
} KeyUpdateSpamClient;

static void key_update_spam_client(void *env) {
    KeyUpdateSpamClient *j = (KeyUpdateSpamClient *)env;
    Alloc *a = arena_allocator(&j->ar);
    TlsConn *c = tls_client(heap_allocator(), j->c, j->config);
    j->tls_conn = c;
    if (c == NULL) {
        testing_t_errorf_v(j->t, "%v", burrow_err_out_of_memory);
        return;
    }
    Error err = tls_conn_handshake(c);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(j->t, "%v", err);
        return;
    }
    TlsKeyUpdateMsg msg = {false};
    Slice ku = burrow__tls_key_update_msg_marshal(&msg, a, &err);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(j->t, "%v", err);
        return;
    }
    const TlsSuiteTLS13 *cs = burrow__tls_cipher_suite_tls13_by_id(c->cipher_suite);
    for (Int i = 0; i <= TLS_MAX_USELESS_RECORDS; i++) {
        (void)burrow__tls_conn_write_record_locked(c, TLS_RECORD_TYPE_HANDSHAKE, ku,
                                                   &err);
        (void)burrow__tls_conn_set_write_traffic_secret(
            c, cs, TLS_QUIC_ENCRYPTION_LEVEL_INITIAL,
            burrow__tls13_next_traffic_secret(cs, a, c->out.traffic_secret));
    }
}

/* TestKeyUpdateSpamPostHandshakeTLS13 tests that TLS 1.3 KeyUpdate messages do
 * not count as a state-advancing message after a handshake has been completed. */
static void TestKeyUpdateSpamPostHandshakeTLS13(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn client, server;
    local_pipe(a, &client, &server);

    TlsConfig client_config = test_config_client(a);
    KeyUpdateSpamClient j;
    memset(&j, 0, sizeof j);
    j.t = t;
    j.c = client;
    j.config = &client_config;
    arena_init(&j.ar, NULL, 0);
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, key_update_spam_client, &j));

    TlsConfig server_config = test_config_server(a);
    TlsConn *s = tls_server(heap_allocator(), server, &server_config);
    if (s == NULL)
        testing_t_fatal_v(t, "out of memory");
    Error err = tls_conn_handshake(s);
    if (BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "%v", err);
    } else {
        Str expected_err = BURROW_S("tls: too many non-advancing records");
        Byte buf[1];
        (void)tls_conn_read(s, slice_from(buf, 1, 1, TYPE_BYTE), &err);
        if (!BURROW_FAILED(err) || !str_eq(error_text(err), expected_err))
            testing_t_errorf_v(t, "unexpected error: got %v, want %q", err,
                               expected_err);
    }

    (void)server.vt->closer.close(server.data);
    (void)client.vt->closer.close(client.data);
    sync_wait_group_wait(&wg);
    tls_conn_free(s);
    tls_conn_free(j.tls_conn);
    arena_free(&j.ar);
    local_pipe_free(client);
    tls_config_release(&client_config);
    tls_config_release(&server_config);
    arena_free(&ar);
}

/* Not in Go. Go tests the Conn only through real handshakes. These run the Conn
 * over a net.Pipe with a handshake that only sets the keys: up to TLS 1.2 it
 * prepares both halves and swaps ChangeCipherSpec records, and in TLS 1.3 it
 * sets the traffic secrets. The suite and version are set on the Conn before
 * the handshake. */
static Error fake_prepare(TlsHalfConn *hc, const TlsSuite *suite, uint16_t version,
                          bool is_read, Byte seed) {
    Alloc *a = burrow__tls_half_conn_next_alloc(hc);
    if (a == NULL)
        return burrow_err_out_of_memory;
    Slice key = key_bytes(a, suite->key_len, seed);
    Slice iv = key_bytes(a, suite->iv_len, (Byte)(seed + 1));
    TlsCipher c;
    Hash mac = {0};
    if (suite->aead != NULL) {
        c = suite->aead(a, key, iv);
    } else {
        c = suite->cipher(a, key, iv, is_read);
        mac = suite->mac(a, key_bytes(a, suite->mac_len, (Byte)(seed + 2)));
    }
    burrow__tls_half_conn_prepare_cipher_spec(hc, version, c, mac);
    return BURROW_NO_ERROR;
}

/* Not in Go. The client writes with seed 1 and the server with seed 11. */
static Error fake_handshake(TlsConn *c, Context ctx) {
    (void)ctx;
    c->have_vers = true;
    c->in.version = c->vers;
    c->out.version = c->vers;
    Byte write_seed = c->is_client ? 1 : 11;
    Byte read_seed = c->is_client ? 11 : 1;

    if (c->vers == TLS_VERSION_TLS13) {
        const TlsSuiteTLS13 *suite =
            burrow__tls_cipher_suite_tls13_by_id(c->cipher_suite);
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Int n = crypto_hash_size(suite->hash);
        Error err = burrow__tls_conn_set_write_traffic_secret(
            c, suite, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION,
            key_bytes(a, n, write_seed));
        if (BURROW_OK(err))
            err = burrow__tls_conn_set_read_traffic_secret(
                c, suite, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION,
                key_bytes(a, n, read_seed), true);
        arena_free(&ar);
        if (BURROW_FAILED(err))
            return err;
        sync_atomic_bool_store(&c->is_handshake_complete, true);
        return BURROW_NO_ERROR;
    }

    const TlsSuite *suite = burrow__tls_cipher_suite_by_id(c->cipher_suite);
    Error err = fake_prepare(&c->out, suite, c->vers, false, write_seed);
    if (BURROW_OK(err))
        err = fake_prepare(&c->in, suite, c->vers, true, read_seed);
    if (BURROW_FAILED(err))
        return err;
    if (c->is_client) {
        err = burrow__tls_conn_write_change_cipher_record(c);
        if (BURROW_OK(err))
            err = burrow__tls_conn_read_change_cipher_spec(c);
    } else {
        err = burrow__tls_conn_read_change_cipher_spec(c);
        if (BURROW_OK(err))
            err = burrow__tls_conn_write_change_cipher_record(c);
    }
    if (BURROW_FAILED(err))
        return err;
    sync_atomic_bool_store(&c->is_handshake_complete, true);
    return BURROW_NO_ERROR;
}

/* Not in Go. A client and a server Conn over a net.Pipe. */
typedef struct ConnPair {
    Arena ar;
    TlsConfig config;
    NetConn c1, c2;
    TlsConn *client, *server;
} ConnPair;

static void conn_pair_init(TestingT *t, ConnPair *p, uint16_t vers, uint16_t suite) {
    arena_init(&p->ar, NULL, 0);
    Alloc *a = arena_allocator(&p->ar);
    memset(&p->config, 0, sizeof p->config);
    net_pipe(a, &p->c1, &p->c2);
    /* The Conns allocate from the goroutines that use them, which an arena does
     * not allow, so they get the heap. */
    Alloc *heap = heap_allocator();
    p->client = burrow__tls_conn_new(heap, p->c1, &p->config, true, fake_handshake);
    p->server = burrow__tls_conn_new(heap, p->c2, &p->config, false, fake_handshake);
    if (p->client == NULL || p->server == NULL)
        testing_t_fatal_v(t, "out of memory");
    p->client->vers = vers;
    p->client->cipher_suite = suite;
    p->server->vers = vers;
    p->server->cipher_suite = suite;
}

static void conn_pair_free(ConnPair *p) {
    tls_conn_free(p->client);
    tls_conn_free(p->server);
    net_pipe_free(p->c1);
    arena_free(&p->ar);
}

/* Not in Go. What a goroutine does to one Conn, and what it got. */
typedef struct ConnJob {
    TlsConn *c;
    Slice data;
    Int n;
    Error err;
    Error close_err;
} ConnJob;

/* Writes "hello", then data, then closes. */
static void write_close_job(void *env) {
    ConnJob *j = (ConnJob *)env;
    static Byte hello[] = "hello";
    j->n = tls_conn_write(j->c, slice_from(hello, 5, 5, TYPE_BYTE), &j->err);
    if (BURROW_OK(j->err))
        j->n += tls_conn_write(j->c, j->data, &j->err);
    j->close_err = tls_conn_close(j->c);
}

/* Not in Go. */
static void TestConnRoundTrip(TestingT *t) {
    static const struct {
        uint16_t vers;
        uint16_t suite;
    } tests[] = {
        {TLS_VERSION_TLS10, TLS_TLS_RSA_WITH_AES_128_CBC_SHA},
        {TLS_VERSION_TLS11, TLS_TLS_ECDHE_RSA_WITH_AES_256_CBC_SHA},
        {TLS_VERSION_TLS12, TLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256},
        {TLS_VERSION_TLS12, TLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256},
        {TLS_VERSION_TLS12, TLS_TLS_RSA_WITH_AES_128_CBC_SHA256},
        {TLS_VERSION_TLS13, TLS_TLS_AES_128_GCM_SHA256},
        {TLS_VERSION_TLS13, TLS_TLS_CHACHA20_POLY1305_SHA256},
    };
    enum { big = 40000 };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        ConnPair p;
        conn_pair_init(t, &p, tests[i].vers, tests[i].suite);
        Alloc *a = arena_allocator(&p.ar);
        Str name = tls_cipher_suite_name(a, tests[i].suite);

        Slice data = slice_make(a, TYPE_BYTE, big, big);
        if (data.p == NULL)
            testing_t_fatal_v(t, "out of memory");
        for (Int k = 0; k < big; k++)
            ((Byte *)data.p)[k] = (Byte)(k * 31 + 7);

        ConnJob client = {p.client, data, 0, BURROW_NO_ERROR, BURROW_NO_ERROR};
        SyncWaitGroup wg = {0};
        sync_wait_group_go(&wg, BURROW_FN(Func, write_close_job, &client));

        Slice got = slice_make(a, TYPE_BYTE, 0, 5 + big);
        if (got.p == NULL)
            testing_t_fatal_v(t, "out of memory");
        Byte buf[4096];
        Error err = BURROW_NO_ERROR;
        while (BURROW_OK(err)) {
            Int n =
                tls_conn_read(p.server, slice_from(buf, 4096, 4096, TYPE_BYTE), &err);
            got = slice_append(a, got, buf, n);
        }
        sync_wait_group_wait(&wg);

        if (!errors_is(err, io_eof))
            testing_t_errorf_v(t, "%s: Read error: %v, want EOF", name, err);
        if (BURROW_FAILED(client.err))
            testing_t_errorf_v(t, "%s: Write error: %v", name, client.err);
        if (BURROW_FAILED(client.close_err))
            testing_t_errorf_v(t, "%s: Close error: %v", name, client.close_err);
        if (client.n != 5 + big)
            testing_t_errorf_v(t, "%s: wrote %d bytes, want %d", name, client.n,
                               5 + big);
        if (got.len != 5 + big || memcmp(got.p, "hello", 5) != 0 ||
            memcmp((Byte *)got.p + 5, data.p, big) != 0)
            testing_t_errorf_v(t, "%s: read %d bytes that do not match", name, got.len);

        TlsConnectionState state = tls_conn_connection_state(p.server);
        if (!state.handshake_complete || state.version != tests[i].vers ||
            state.cipher_suite != tests[i].suite)
            testing_t_errorf_v(t, "%s: state %v %x %x", name, state.handshake_complete,
                               state.version, state.cipher_suite);
        conn_pair_free(&p);
    }
}

/* Not in Go. The client sends a KeyUpdate that asks for one back, then
 * "after" under its next key. */
static void key_update_write_job(void *env) {
    ConnJob *j = (ConnJob *)env;
    TlsConn *c = j->c;
    TlsHandshakeMsg msg;
    burrow__tls_handshake_msg_init(&msg, TLS_MSG_KEY_UPDATE);
    msg.key_update.update_requested = true;
    IoWriter none = {NULL, NULL};
    (void)burrow__tls_conn_write_handshake_record(c, &msg, none, &j->err);
    if (BURROW_FAILED(j->err))
        return;

    const TlsSuiteTLS13 *suite = burrow__tls_cipher_suite_tls13_by_id(c->cipher_suite);
    Arena ar;
    arena_init(&ar, NULL, 0);
    Slice next = burrow__tls13_next_traffic_secret(suite, arena_allocator(&ar),
                                                   c->out.traffic_secret);
    j->err = burrow__tls_conn_set_write_traffic_secret(
        c, suite, TLS_QUIC_ENCRYPTION_LEVEL_APPLICATION, next);
    arena_free(&ar);
    if (BURROW_FAILED(j->err))
        return;

    static Byte after[] = "after";
    j->n = tls_conn_write(c, slice_from(after, 5, 5, TYPE_BYTE), &j->err);
}

/* Reads into data. */
static void read_job(void *env) {
    ConnJob *j = (ConnJob *)env;
    j->n = tls_conn_read(j->c, j->data, &j->err);
}

/* Not in Go. */
static void TestConnKeyUpdate(TestingT *t) {
    ConnPair p;
    conn_pair_init(t, &p, TLS_VERSION_TLS13, TLS_TLS_AES_128_GCM_SHA256);
    Error err = tls_conn_handshake(p.client);
    if (BURROW_OK(err))
        err = tls_conn_handshake(p.server);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, "handshake: %v", err);

    Byte rbuf[16];
    ConnJob writer = {p.client, {0}, 0, BURROW_NO_ERROR, BURROW_NO_ERROR};
    ConnJob reader = {p.client, slice_from(rbuf, 16, 16, TYPE_BYTE), 0, BURROW_NO_ERROR,
                      BURROW_NO_ERROR};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, key_update_write_job, &writer));
    sync_wait_group_go(&wg, BURROW_FN(Func, read_job, &reader));

    /* The server answers the KeyUpdate while it reads, then reads "after". */
    Byte buf[16];
    Int n = tls_conn_read(p.server, slice_from(buf, 16, 16, TYPE_BYTE), &err);
    if (BURROW_FAILED(err) || n != 5 || memcmp(buf, "after", 5) != 0)
        testing_t_errorf_v(t, "server Read: %q, %v; want \"after\"",
                           str_from_bytes(buf, n), err);
    static Byte pong[] = "pong";
    (void)tls_conn_write(p.server, slice_from(pong, 4, 4, TYPE_BYTE), &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "server Write: %v", err);
    sync_wait_group_wait(&wg);

    if (BURROW_FAILED(writer.err))
        testing_t_errorf_v(t, "client Write: %v", writer.err);
    if (BURROW_FAILED(reader.err) || reader.n != 4 || memcmp(rbuf, "pong", 4) != 0)
        testing_t_errorf_v(t, "client Read: %q, %v; want \"pong\"",
                           str_from_bytes(rbuf, reader.n), reader.err);
    conn_pair_free(&p);
}

static void bad_certificate_job(void *env) {
    ConnJob *j = (ConnJob *)env;
    j->err = burrow__tls_conn_send_alert(j->c, TLS_ALERT_BAD_CERTIFICATE);
}

/* Not in Go. */
static void TestConnAlert(TestingT *t) {
    ConnPair p;
    conn_pair_init(t, &p, TLS_VERSION_TLS13, TLS_TLS_AES_128_GCM_SHA256);
    Error err = tls_conn_handshake(p.client);
    if (BURROW_OK(err))
        err = tls_conn_handshake(p.server);
    if (BURROW_FAILED(err))
        testing_t_fatal_v(t, "handshake: %v", err);

    ConnJob client = {p.client, {0}, 0, BURROW_NO_ERROR, BURROW_NO_ERROR};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, bad_certificate_job, &client));
    Byte buf[16];
    for (int i = 0; i < 2; i++) {
        err = BURROW_NO_ERROR;
        (void)tls_conn_read(p.server, slice_from(buf, 16, 16, TYPE_BYTE), &err);
        if (!str_eq(error_text(err), BURROW_S("remote error: tls: bad certificate")))
            testing_t_errorf_v(t, "Read %d: %v, want remote error", i, err);
    }
    sync_wait_group_wait(&wg);
    if (!str_eq(error_text(client.err), BURROW_S("local error: tls: bad certificate")))
        testing_t_errorf_v(t, "sendAlert: %v, want local error", client.err);
    conn_pair_free(&p);
}

static void raw_write_job(void *env) {
    ConnJob *j = (ConnJob *)env;
    NetConn c = j->c->conn;
    j->n = c.vt->writer.write(c.data, j->data, &j->err);
}

/* Not in Go. Reads the first handshake message. */
static Error read_hello_handshake(TlsConn *c, Context ctx) {
    (void)ctx;
    Arena ar;
    arena_init(&ar, NULL, 0);
    TlsHandshakeMsg msg;
    IoWriter none = {NULL, NULL};
    Error err = burrow__tls_conn_read_handshake(c, none, arena_allocator(&ar), &msg);
    arena_free(&ar);
    return err;
}

/* Not in Go. */
static void TestConnRecordHeaderError(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    NetConn c1, c2;
    net_pipe(a, &c1, &c2);
    TlsConfig config;
    memset(&config, 0, sizeof config);
    Alloc *heap = heap_allocator();
    TlsConn *client = burrow__tls_conn_new(heap, c1, &config, true, fake_handshake);
    TlsConn *server =
        burrow__tls_conn_new(heap, c2, &config, false, read_hello_handshake);
    if (client == NULL || server == NULL)
        testing_t_fatal_v(t, "out of memory");

    static Byte get[] = "GET / HTTP/1.1\r\n\r\n";
    ConnJob job = {client, slice_from(get, 18, 18, TYPE_BYTE), 0, BURROW_NO_ERROR,
                   BURROW_NO_ERROR};
    SyncWaitGroup wg = {0};
    sync_wait_group_go(&wg, BURROW_FN(Func, raw_write_job, &job));

    Error err = tls_conn_handshake(server);
    (void)c2.vt->closer.close(c2.data);
    sync_wait_group_wait(&wg);

    const TlsRecordHeaderError *e = errors_as(err, TYPE_TLS_RECORD_HEADER_ERROR);
    if (e == NULL) {
        testing_t_errorf_v(t, "Handshake: %v, want a RecordHeaderError", err);
    } else {
        if (!str_eq(e->msg,
                    BURROW_S("first record does not look like a TLS handshake")))
            testing_t_errorf_v(t, "Msg = %q", e->msg);
        if (memcmp(e->record_header, "GET /", 5) != 0)
            testing_t_errorf_v(t, "RecordHeader = %q",
                               str_from_bytes(e->record_header, 5));
        if (e->conn.data != c2.data)
            testing_t_errorf_v(t, "Conn is not the server's net.Conn");
    }
    if (!str_eq(error_text(err),
                BURROW_S("tls: first record does not look like a TLS handshake")))
        testing_t_errorf_v(t, "Error() = %q", error_text(err));

    /* The handshake error is kept. */
    Error again = tls_conn_handshake(server);
    if (!str_eq(error_text(again), error_text(err)))
        testing_t_errorf_v(t, "second Handshake: %v, want %v", again, err);

    tls_conn_free(client);
    tls_conn_free(server);
    net_pipe_free(c1);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestRoundUp)                                                                     \
    X(TestRemovePadding)                                                               \
    X(TestHalfConnRoundTrip)                                                           \
    X(TestCertificateSelection)                                                        \
    X(TestBrokenCertificateSkipped)                                                    \
    X(TestDynamicRecordSizingWithStreamCipher)                                         \
    X(TestDynamicRecordSizingWithCBC)                                                  \
    X(TestDynamicRecordSizingWithAEAD)                                                 \
    X(TestDynamicRecordSizingWithTLSv13)                                               \
    X(TestHairpinInClose)                                                              \
    X(TestRecordBadVersionTLS13)                                                       \
    X(TestKeyUpdateSpamPostHandshakeTLS13)                                             \
    X(TestConnRoundTrip)                                                               \
    X(TestConnKeyUpdate)                                                               \
    X(TestConnAlert)                                                                   \
    X(TestConnRecordHeaderError)

TESTING_MAIN(TESTS)
