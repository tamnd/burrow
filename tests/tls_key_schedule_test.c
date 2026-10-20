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

/* Not in Go. Go tests the key exchanges through its handshakes, which are not
 * here yet. This runs keyShares, serverSharedSecret and clientSharedSecret for
 * each group and checks the sizes against RFC 8446, FIPS 203 and
 * draft-ietf-tls-ecdhe-mlkem-02. */
static void TestKeyExchanges(TestingT *t) {
    struct {
        TlsCurveID id, fallback;
        Int client_share, server_share, shared_key, fallback_share;
    } tests[] = {
        {TLS_X25519, 0, 32, 32, 32, 0},
        {TLS_CURVE_P256, 0, 65, 65, 32, 0},
        {TLS_CURVE_P384, 0, 97, 97, 48, 0},
        {TLS_CURVE_P521, 0, 133, 133, 66, 0},
        {TLS_X25519MLKEM768, TLS_X25519, 1184 + 32, 1088 + 32, 32 + 32, 32},
        {TLS_SEC_P256_R1_MLKEM768, TLS_CURVE_P256, 65 + 1184, 65 + 1088, 32 + 32, 65},
        {TLS_SEC_P384_R1_MLKEM1024, TLS_CURVE_P384, 97 + 1568, 97 + 1568, 48 + 32, 97},
        {TLS_MLKEM1024, 0, 1568, 1568, 32, 0},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        Arena ar;
        arena_init(&ar, NULL, 0);
        Alloc *a = arena_allocator(&ar);
        Str name = tls_curve_id_string(tests[i].id, a);
        Error err = BURROW_NO_ERROR;
        const TlsKeyExchange *ke =
            burrow__tls_key_exchange_for_curve_id(tests[i].id, &err);
        if (ke == NULL) {
            testing_t_errorf_v(t, "%s: keyExchangeForCurveID: %v", name, err);
            arena_free(&ar);
            continue;
        }

        TlsKeySharePrivateKeys priv;
        Slice shares;
        if (!burrow__tls_key_exchange_key_shares(ke, a, (IoReader){0}, &priv, &shares,
                                                 &err)) {
            testing_t_errorf_v(t, "%s: keyShares: %v", name, err);
            arena_free(&ar);
            continue;
        }
        Int want_shares = tests[i].fallback != 0 ? 2 : 1;
        if (shares.len != want_shares)
            testing_t_fatalf_v(t, "%s: got %d key shares, want %d", name, shares.len,
                               want_shares);
        TlsKeyShare share = BURROW_AT(TlsKeyShare, shares, 0);
        if (share.group != tests[i].id || share.data.len != tests[i].client_share)
            testing_t_errorf_v(t, "%s: key share is group %d with %d bytes, want %d",
                               name, (Int)share.group, share.data.len,
                               tests[i].client_share);
        if (want_shares == 2) {
            TlsKeyShare fallback = BURROW_AT(TlsKeyShare, shares, 1);
            if (fallback.group != tests[i].fallback ||
                fallback.data.len != tests[i].fallback_share)
                testing_t_errorf_v(t, "%s: fallback share is group %d with %d bytes",
                                   name, (Int)fallback.group, fallback.data.len);
            /* The ML-KEM key comes first only for X25519MLKEM768. */
            Int at = tests[i].id == TLS_X25519MLKEM768
                         ? share.data.len - fallback.data.len
                         : 0;
            if (fallback.data.len == tests[i].fallback_share &&
                !bytes_equal(slice_sub(share.data, at, at + fallback.data.len),
                             fallback.data))
                testing_t_errorf_v(
                    t, "%s: fallback share is not at %d in the hybrid one", name, at);
        }

        TlsKeyShare ks = {0};
        Slice server_key = burrow__tls_key_exchange_server_shared_secret(
            ke, a, (IoReader){0}, share.data, &ks, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: serverSharedSecret: %v", name, err);
        } else {
            if (ks.group != tests[i].id || ks.data.len != tests[i].server_share)
                testing_t_errorf_v(t,
                                   "%s: server key share is group %d with %d bytes, "
                                   "want %d",
                                   name, (Int)ks.group, ks.data.len,
                                   tests[i].server_share);
            Slice client_key = burrow__tls_key_exchange_client_shared_secret(
                ke, a, &priv, ks.data, &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "%s: clientSharedSecret: %v", name, err);
            else if (server_key.len != tests[i].shared_key ||
                     !bytes_equal(server_key, client_key))
                testing_t_errorf_v(t, "%s: shared keys of %d and %d bytes differ", name,
                                   server_key.len, client_key.len);
        }
        burrow__tls_key_share_private_keys_free(&priv);
        arena_free(&ar);
    }
}

/* Not in Go. The errors the key exchanges give for groups they do not know
 * and for shares of the wrong size. */
static void TestKeyExchangeErrors(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    if (burrow__tls_key_exchange_for_curve_id(0x0a0a, &err) != NULL ||
        !str_eq(error_text(err), BURROW_S("tls: unsupported key exchange")))
        testing_t_errorf_v(t, "keyExchangeForCurveID(0x0a0a): got %v", err);

    struct {
        TlsCurveID id;
        bool client;
        Int len;
        const char *want;
    } tests[] = {
        {TLS_X25519MLKEM768, false, 1184 + 31,
         "tls: invalid client key share length for hybrid key exchange"},
        {TLS_X25519MLKEM768, true, 1088 + 33,
         "tls: invalid server key share length for hybrid key exchange"},
        {TLS_SEC_P384_R1_MLKEM1024, false, 0,
         "tls: invalid client key share length for hybrid key exchange"},
        {TLS_SEC_P256_R1_MLKEM768, true, 65 + 1184,
         "tls: invalid server key share length for hybrid key exchange"},
        {TLS_MLKEM1024, false, 1567, "mlkem: invalid encapsulation key length"},
        {TLS_CURVE_P256, false, 64, "crypto/ecdh: invalid public key"},
        {TLS_CURVE_P384, true, 65, "crypto/ecdh: invalid public key"},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        err = BURROW_NO_ERROR;
        const TlsKeyExchange *ke =
            burrow__tls_key_exchange_for_curve_id(tests[i].id, &err);
        CHECK(ke != NULL);
        Slice share = slice_make(a, TYPE_BYTE, tests[i].len, tests[i].len);
        CHECK(tests[i].len == 0 || share.p != NULL);
        Slice key;
        if (tests[i].client) {
            TlsKeySharePrivateKeys priv;
            Slice shares;
            CHECK(burrow__tls_key_exchange_key_shares(ke, a, (IoReader){0}, &priv,
                                                      &shares, &err));
            key = burrow__tls_key_exchange_client_shared_secret(ke, a, &priv, share,
                                                                &err);
            burrow__tls_key_share_private_keys_free(&priv);
        } else {
            TlsKeyShare ks = {0};
            key = burrow__tls_key_exchange_server_shared_secret(ke, a, (IoReader){0},
                                                                share, &ks, &err);
        }
        if (key.p != NULL || !BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "#%d: got a shared key, want %q", (Int)i,
                               str_from_cstr(tests[i].want));
            continue;
        }
        if (!str_eq(error_text(err), str_from_cstr(tests[i].want)))
            testing_t_errorf_v(t, "#%d: got %v, want %q", (Int)i, err,
                               str_from_cstr(tests[i].want));
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestACVPVectors)                                                                 \
    X(TestTrafficKey)                                                                  \
    X(TestKeyExchanges)                                                                \
    X(TestKeyExchangeErrors)

TESTING_MAIN(TESTS)
