/* Derived from Go's src/crypto/tls/key_schedule_test.go.
 * Go source: go1.27.1.
 *
 * Go reads the exporter secrets with tls13.TestingOnlyExporterSecret; here
 * the secret field is right there to read.
 *
 * Copyright 2018 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/bytes.h"
#include "burrow/crypto/sha256.h"
#include "burrow/encoding/hex.h"
#include "burrow/panic.h"
#include "burrow/strings.h"
#include "burrow/unicode.h"

static Slice from_hex(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("from_hex: bad hex"));
    return b;
}

static Str as_str(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

static void TestACVPVectors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;

    /* https://github.com/usnistgov/ACVP-Server/blob/3a7333f63/gen-val/json-files/TLS-v1.3-KDF-RFC8446/prompt.json#L428-L436 */
    Slice psk =
        from_hex(a, "56288B726C73829F7A3E47B103837C8139ACF552E7530C7A710B35ED41191698");
    Slice dhe =
        from_hex(a, "EFFE9EC26AA29FD750DFA6A10B944D74071595B27EE88887D5E11C84590B5CC3");
    Slice hello_client_random =
        from_hex(a, "E9137679E582BA7C1DB41CF725F86C6D09C8C05F297BAD9A65B552EAF524FDE4");
    Slice hello_server_random =
        from_hex(a, "23ECCFD030790748C8F8D8A656FD98D717F1B62AF3712F97211D2070B499F98A");
    Slice finished_client_random =
        from_hex(a, "62A62FA75563ED4FDCAA0BC16567B314871C304ACF06B0FFC3F08C1797594D43");
    Slice finished_server_random =
        from_hex(a, "C750EDA6696CD101B142BD79E00E6AC8C5F2C0ABC78DD64F4D991326659E9299");

    /* https://github.com/usnistgov/ACVP-Server/blob/3a7333f63/gen-val/json-files/TLS-v1.3-KDF-RFC8446/expectedResults.json#L571-L581 */
    Slice client_early_traffic_secret =
        from_hex(a, "3272189698C3594D18F58EFA3F12B638A249515099BE7A2FA9836BABE74F0111");
    Slice early_exporter_master_secret =
        from_hex(a, "88E078F562CDC930219F6A5E98A1CE8C6E5F3DAC5AC516459A96F2EF8F114C66");
    Slice client_handshake_traffic_secret =
        from_hex(a, "B32306C3CE9932C460A1FE6C0F060593974842036B96FA45049B7352E71C2AD2");
    Slice server_handshake_traffic_secret =
        from_hex(a, "22787F8CA269D34BC549AC8BA19F2040938A3AA370D7CC9D60F720882B88D01B");
    Slice client_application_traffic_secret =
        from_hex(a, "47D7EA08397B5871154B0FE85584BCC30A87C69E84D69B56007C5B21F76493BA");
    Slice server_application_traffic_secret =
        from_hex(a, "EFBDB0C873C0480DA57307083839A8984BE25B9A8545E4FCA029940FE2800565");
    Slice exporter_master_secret =
        from_hex(a, "8A43D787EE3804EAD4A2A5B32972F9896B696295645D7222E1FD081DDD939834");
    Slice resumption_master_secret =
        from_hex(a, "5F4C961329C91044011ACBECB0B289282E0E3FED045CB3EA924DFFE5FE654B3D");

    /* The "Random" values are undocumented, but they are meant to be written to
     * the hash in sequence to develop the transcript. */
    Hash transcript = sha256_new(a);

    Tls13EarlySecret es = burrow__tls13_new_early_secret(a, sha256_new, psk);

    hash_write(transcript, hello_client_random, &err);

    Slice got =
        burrow__tls13_early_secret_client_early_traffic_secret(&es, a, transcript);
    if (!bytes_equal(got, client_early_traffic_secret))
        testing_t_errorf_v(t, "clientEarlyTrafficSecret = %x, want %x", as_str(got),
                           as_str(client_early_traffic_secret));
    got = burrow__tls13_early_secret_early_exporter_master_secret(&es, a, transcript)
              .secret;
    if (!bytes_equal(got, early_exporter_master_secret))
        testing_t_errorf_v(t, "earlyExporterMasterSecret = %x, want %x", as_str(got),
                           as_str(early_exporter_master_secret));

    Tls13HandshakeSecret hs = burrow__tls13_early_secret_handshake_secret(&es, a, dhe);

    hash_write(transcript, hello_server_random, &err);

    got = burrow__tls13_handshake_secret_client_handshake_traffic_secret(&hs, a,
                                                                         transcript);
    if (!bytes_equal(got, client_handshake_traffic_secret))
        testing_t_errorf_v(t, "clientHandshakeTrafficSecret = %x, want %x", as_str(got),
                           as_str(client_handshake_traffic_secret));
    got = burrow__tls13_handshake_secret_server_handshake_traffic_secret(&hs, a,
                                                                         transcript);
    if (!bytes_equal(got, server_handshake_traffic_secret))
        testing_t_errorf_v(t, "serverHandshakeTrafficSecret = %x, want %x", as_str(got),
                           as_str(server_handshake_traffic_secret));

    Tls13MasterSecret ms = burrow__tls13_handshake_secret_master_secret(&hs, a);

    hash_write(transcript, finished_server_random, &err);

    got = burrow__tls13_master_secret_client_application_traffic_secret(&ms, a,
                                                                        transcript);
    if (!bytes_equal(got, client_application_traffic_secret))
        testing_t_errorf_v(t, "clientApplicationTrafficSecret = %x, want %x",
                           as_str(got), as_str(client_application_traffic_secret));
    got = burrow__tls13_master_secret_server_application_traffic_secret(&ms, a,
                                                                        transcript);
    if (!bytes_equal(got, server_application_traffic_secret))
        testing_t_errorf_v(t, "serverApplicationTrafficSecret = %x, want %x",
                           as_str(got), as_str(server_application_traffic_secret));
    got = burrow__tls13_master_secret_exporter_master_secret(&ms, a, transcript).secret;
    if (!bytes_equal(got, exporter_master_secret))
        testing_t_errorf_v(t, "exporterMasterSecret = %x, want %x", as_str(got),
                           as_str(exporter_master_secret));

    hash_write(transcript, finished_client_random, &err);

    got = burrow__tls13_master_secret_resumption_master_secret(&ms, a, transcript);
    if (!bytes_equal(got, resumption_master_secret))
        testing_t_errorf_v(t, "resumptionMasterSecret = %x, want %x", as_str(got),
                           as_str(resumption_master_secret));
    arena_free(&ar);
}

/* This file contains tests derived from draft-ietf-tls-tls13-vectors-07. */

static Slice parse_vector(Alloc *a, const char *vs) {
    Str v = str_from_cstr(vs);
    Byte *p = (Byte *)mem_alloc_nozero(a, (size_t)v.len + 1, 1);
    if (p == NULL)
        panic_str(BURROW_S("parse_vector: out of memory"));
    Int n = 0;
    for (Int i = 0; i < v.len; i++) {
        if (!unicode_is_space((Rune)(Byte)v.p[i]))
            p[n++] = (Byte)v.p[i];
    }
    v = str_from_bytes(p, n);
    Int colon = strings_last_index(v, BURROW_S(":"));
    v = str_from_bytes(v.p + colon + 1, v.len - colon - 1);
    Error err = BURROW_NO_ERROR;
    Slice res = hex_decode_string(a, v, &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("parse_vector: bad hex"));
    return res;
}

static void TestTrafficKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice traffic_secret = parse_vector(
        a, "PRK (32 octets):  b6 7b 7d 69 0c c1 6c 4e 75 e5 42 13 cb 2d 37 b4\n"
           "\t\te9 c9 12 bc de d9 10 5d 42 be fd 59 d3 91 ad 38");
    Slice want_key = parse_vector(
        a, "key expanded (16 octets):  3f ce 51 60 09 c2 17 27 d0 f2 e4 e8 6e\n"
           "\t\te4 03 bc");
    Slice want_iv = parse_vector(
        a, "iv expanded (12 octets):  5d 31 3e b2 67 12 76 ee 13 00 0b 30");

    const TlsSuiteTLS13 *c = &burrow__tls_cipher_suites_tls13[0];
    Slice got_key, got_iv;
    CHECK(burrow__tls13_traffic_key(c, a, traffic_secret, &got_key, &got_iv));
    if (!bytes_equal(got_key, want_key))
        testing_t_errorf_v(t, "cipherSuiteTLS13.trafficKey() gotKey = % x, want % x",
                           as_str(got_key), as_str(want_key));
    if (!bytes_equal(got_iv, want_iv))
        testing_t_errorf_v(t, "cipherSuiteTLS13.trafficKey() gotIV = % x, want % x",
                           as_str(got_iv), as_str(want_iv));
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestACVPVectors)                                                                 \
    X(TestTrafficKey)

TESTING_MAIN(TESTS)
