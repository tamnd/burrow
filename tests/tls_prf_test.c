/* Derived from Go's src/crypto/tls/prf_test.go.
 * Go source: go1.27.1.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "check.h"

#include "../src/crypto/tls_internal.h"
#include "burrow/burrow.h"
#include "burrow/crypto/tls.h"
#include "burrow/encoding/hex.h"

#include <stdint.h>

#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

typedef struct TestSplitPreMasterSecretTest {
    const char *in, *out1, *out2;
} TestSplitPreMasterSecretTest;

static const TestSplitPreMasterSecretTest test_split_pre_master_secret_tests[] = {
    {"", "", ""},
    {"00", "00", "00"},
    {"0011", "00", "11"},
    {"001122", "0011", "1122"},
    {"00112233", "0011", "2233"},
};

static Slice unhex(Alloc *a, const char *s) {
    return hex_decode_string(a, str_from_cstr(s), NULL);
}

static void TestSplitPreMasterSecret(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < NELEM(test_split_pre_master_secret_tests); i++) {
        const TestSplitPreMasterSecretTest *test =
            &test_split_pre_master_secret_tests[i];
        Slice in = unhex(a, test->in);
        Slice out1, out2;
        burrow__tls_split_pre_master_secret(in, &out1, &out2);
        Str s1 = hex_encode_to_string(a, out1);
        Str s2 = hex_encode_to_string(a, out2);
        Str want1 = str_from_cstr(test->out1);
        Str want2 = str_from_cstr(test->out2);
        if (!str_eq(s1, want1) || !str_eq(s2, want2))
            testing_t_errorf_v(t, "#%d: got: (%s, %s) want: (%s, %s)", (Int)i, s1, s2,
                               want1, want2);
    }
    arena_free(&ar);
}

typedef struct TestKeysFromTest {
    uint16_t version;
    uint16_t suite;
    const char *pre_master_secret;
    const char *client_random, *server_random;
    const char *master_secret;
    const char *client_mac, *server_mac;
    const char *client_key, *server_key;
    Int mac_len, key_len;
    const char *context_keying_material, *no_context_keying_material;
} TestKeysFromTest;

/* These test vectors were generated from GnuTLS using `gnutls-cli --insecure -d 9 ` */
static const TestKeysFromTest test_keys_from_tests[] = {
    {
        TLS_VERSION_TLS10,
        TLS_TLS_RSA_WITH_RC4_128_SHA,
        "0302cac83ad4b1db3b9ab49ad05957de2a504a634a386fc600889321e1a971f574794668"
        "30ac3e6f468e87f5385fa0c5",
        "4ae66303755184a3917fcb44880605fcc53baa01912b22ed94473fc69cebd558",
        "4ae663020ec16e6bb5130be918cfcafd4d765979a3136a5d50c593446e4e44db",
        "3d851bab6e5556e959a16bc36d66cfae32f672bfa9ecdef6096cbb1b23472df1da63dbbd"
        "9827606413221d149ed08ceb",
        "805aaa19b3d2c0a0759a4b6c9959890e08480119",
        "2d22f9fe519c075c16448305ceee209fc24ad109",
        "d50b5771244f850cd8117a9ccafe2cf1",
        "e076e33206b30507a85c32855acd0919",
        20,
        16,
        "4d1bb6fc278c37d27aa6e2a13c2e079095d143272c2aa939da33d88c1c0cec22",
        "93fba89599b6321ae538e27c6548ceb8b46821864318f5190d64a375e5d69d41",
    },
    {
        TLS_VERSION_TLS10,
        TLS_TLS_RSA_WITH_RC4_128_SHA,
        "03023f7527316bc12cbcd69e4b9e8275d62c028f27e65c745cfcddc7ce01bd3570a11137"
        "8b63848127f1c36e5f9e4890",
        "4ae66364b5ea56b20ce4e25555aed2d7e67f42788dd03f3fee4adae0459ab106",
        "4ae66363ab815cbf6a248b87d6b556184e945e9b97fbdf247858b0bdafacfa1c",
        "7d64be7c80c59b740200b4b9c26d0baaa1c5ae56705acbcf2307fe62beb4728c19392c83"
        "f20483801cce022c77645460",
        "97742ed60a0554ca13f04f97ee193177b971e3b0",
        "37068751700400e03a8477a5c7eec0813ab9e0dc",
        "207cddbc600d2a200abac6502053ee5c",
        "df3f94f6e1eacc753b815fe16055cd43",
        20,
        16,
        "2c9f8961a72b97cbe76553b5f954caf8294fc6360ef995ac1256fe9516d0ce7f",
        "274f19c10291d188857ad8878e2119f5aa437d4da556601cf1337aff23154016",
    },
    {
        TLS_VERSION_TLS10,
        TLS_TLS_RSA_WITH_RC4_128_SHA,
        "832d515f1d61eebb2be56ba0ef79879efb9b527504abb386fb4310ed5d0e3b1f220d3bb6"
        "b455033a2773e6d8bdf951d278a187482b400d45deb88a5d5a6bb7d6a7a1decc04eb9ef0"
        "642876cd4a82d374d3b6ff35f0351dc5d411104de431375355addc39bfb1f6329fb163b0"
        "bc298d658338930d07d313cd980a7e3d9196cac1",
        "4ae663b2ee389c0de147c509d8f18f5052afc4aaf9699efe8cb05ece883d3a5e",
        "4ae664d503fd4cff50cfc1fb8fc606580f87b0fcdac9554ba0e01d785bdf278e",
        "1aff2e7a2c4279d0126f57a65a77a8d9d0087cf2733366699bec27eb53d5740705a8574b"
        "b1acc2abbe90e44f0dd28d6c",
        "3c7647c93c1379a31a609542aa44e7f117a70085",
        "0d73102994be74a575a3ead8532590ca32a526d4",
        "ac7581b0b6c10d85bbd905ffbf36c65e",
        "ff07edde49682b45466bd2e39464b306",
        20,
        16,
        "678b0d43f607de35241dc7e9d1a7388a52c35033a1a0336d4d740060a6638fe2",
        "f3b4ac743f015ef21d79978297a53da3e579ee047133f38c234d829c0f907dab",
    },
};

static Str as_str(Slice b) {
    return str_from_bytes((const Byte *)b.p, b.len);
}

static void TestKeysFromPreMasterSecret(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < NELEM(test_keys_from_tests); i++) {
        const TestKeysFromTest *test = &test_keys_from_tests[i];
        const TlsSuite *suite = burrow__tls_cipher_suite_by_id(test->suite);
        Slice in = unhex(a, test->pre_master_secret);
        Slice client_random = unhex(a, test->client_random);
        Slice server_random = unhex(a, test->server_random);

        Slice master_secret = burrow__tls_master_from_pre_master_secret(
            a, test->version, suite, in, client_random, server_random);
        Str s = hex_encode_to_string(a, master_secret);
        if (!str_eq(s, str_from_cstr(test->master_secret))) {
            testing_t_errorf_v(t, "#%d: bad master secret %s, want %s", (Int)i, s,
                               test->master_secret);
            continue;
        }

        TlsKeys keys;
        CHECK(burrow__tls_keys_from_master_secret(
            a, test->version, suite, master_secret, client_random, server_random,
            test->mac_len, test->key_len, 0, &keys));
        Str client_mac_string = hex_encode_to_string(a, keys.client_mac);
        Str server_mac_string = hex_encode_to_string(a, keys.server_mac);
        Str client_key_string = hex_encode_to_string(a, keys.client_key);
        Str server_key_string = hex_encode_to_string(a, keys.server_key);
        if (!str_eq(client_mac_string, str_from_cstr(test->client_mac)) ||
            !str_eq(server_mac_string, str_from_cstr(test->server_mac)) ||
            !str_eq(client_key_string, str_from_cstr(test->client_key)) ||
            !str_eq(server_key_string, str_from_cstr(test->server_key)))
            testing_t_errorf_v(t, "#%d: got: (%s, %s, %s, %s) want: (%s, %s, %s, %s)",
                               (Int)i, client_mac_string, server_mac_string,
                               client_key_string, server_key_string, test->client_mac,
                               test->server_mac, test->client_key, test->server_key);

        TlsEkmMaster ekm = {test->version, suite, master_secret, client_random,
                            server_random};
        Error err = BURROW_NO_ERROR;
        static Byte context[] = "context";
        Slice context_keying_material = burrow__tls_ekm_from_master_secret(
            &ekm, a, BURROW_S("label"), slice_from(context, 7, 7, TYPE_BYTE), 32, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "ekmFromMasterSecret failed: %v", err);

        Slice no_context_keying_material = burrow__tls_ekm_from_master_secret(
            &ekm, a, BURROW_S("label"), slice_nil(TYPE_BYTE), 32, &err);
        if (BURROW_FAILED(err))
            testing_t_fatalf_v(t, "ekmFromMasterSecret failed: %v", err);

        if (!str_eq(hex_encode_to_string(a, context_keying_material),
                    str_from_cstr(test->context_keying_material)) ||
            !str_eq(hex_encode_to_string(a, no_context_keying_material),
                    str_from_cstr(test->no_context_keying_material)))
            testing_t_errorf_v(
                t, "#%d: got keying material: (%s, %s) want: (%s, %s)", (Int)i,
                as_str(context_keying_material), as_str(no_context_keying_material),
                test->context_keying_material, test->no_context_keying_material);
    }
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestSplitPreMasterSecret)                                                        \
    X(TestKeysFromPreMasterSecret)

TESTING_MAIN(TESTS)
