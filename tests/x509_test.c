/* crypto/x509 tests for the key formats, OIDs and encrypted PEM blocks: Go's
 * sec1_test.go, pkcs8_test.go, pem_decrypt_test.go and oid_test.go, and the
 * tests of x509_test.go that only touch keys: TestParsePKCS1PrivateKey,
 * TestPKCS1MismatchPublicKeyFormat, TestMarshalInvalidPublicKey,
 * TestParsePKIXPublicKey, TestPKIXMismatchPublicKeyFormat,
 * TestMarshalRSAPrivateKey, TestMarshalRSAPrivateKeyInvalid,
 * TestMarshalRSAPublicKey, TestEKUOIDS and the key half of TestMLDSA.
 *
 * TestOID checks toASN1OID, which is not exported, through
 * x509_oid_equal_asn1_oid instead. TestMarshalRSAPublicKey leaves out the part
 * that runs encoding/asn1 straight on an rsa.PublicKey, which works in Go by
 * accident of reflection and has no type descriptor here.
 *
 * Copyright 2009 The Go Authors. All rights reserved.
 * Copyright 2026 The burrow Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style licence that can be found
 * in the LICENSE file. */

#include "burrow/crypto/x509.h"

#include "burrow/bytes.h"
#include "burrow/crypto/ecdh.h"
#include "burrow/crypto/ecdsa.h"
#include "burrow/crypto/ed25519.h"
#include "burrow/crypto/elliptic.h"
#include "burrow/crypto/mldsa.h"
#include "burrow/crypto/rand.h"
#include "burrow/crypto/rsa.h"
#include "burrow/declare.h"
#include "burrow/encoding/asn1.h"
#include "burrow/encoding/base64.h"
#include "burrow/encoding/hex.h"
#include "burrow/encoding/pem.h"
#include "burrow/error.h"
#include "burrow/map.h"
#include "burrow/math/big.h"
#include "burrow/mem/arena.h"
#include "burrow/strings.h"
#include "burrow/testing.h"

#include "../src/crypto/x509_internal.h"
#include "check.h"
#include "x509_test_gen.h"

#include <stdint.h>
#include <string.h>

static Slice hexb(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b = hex_decode_string(a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_test: bad hex"));
    return b;
}

static Slice cbytes(const char *s) {
    Int n = (Int)strlen(s);
    return (Slice){(void *)(uintptr_t)s, n, n, TYPE_BYTE};
}

/* pemDecode: the bytes of the first block in s. */
static Slice pem_bytes(Alloc *a, const char *s) {
    Slice rest;
    PemBlock *b = pem_decode(a, cbytes(s), &rest);
    if (b == NULL)
        panic_str(BURROW_S("x509_test: no PEM block"));
    return b->bytes;
}

static BigInt *dec(Alloc *a, const char *s) {
    bool ok = false;
    BigInt *z = big_int_set_string(big_new_int(a, 0), str_from_cstr(s), 10, &ok);
    if (!ok)
        panic_str(BURROW_S("x509_test: bad decimal"));
    return z;
}

static bool contains(Error err, const char *want) {
    return BURROW_FAILED(err) && strings_contains(error_text(err), str_from_cstr(want));
}

static const char *cz(Alloc *a, Str s) {
    char *p = mem_alloc_nozero(a, (size_t)s.len + 1, 1);
    if (s.len > 0)
        memcpy(p, s.p, (size_t)s.len);
    p[s.len] = 0;
    return p;
}

#define ERRTEXT(a, e) cz((a), error_text(e))

/* ---------------------------------------------------------------- sec1 */

static void TestParseECPrivateKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const char *const tests[] = {ec_key_test_0, ec_key_test_1, ec_key_test_2};
    for (Int i = 0; i < 3; i++) {
        Slice der = hexb(a, tests[i]);
        Error err = BURROW_NO_ERROR;
        EcdsaPrivateKey *key = x509_parse_ec_private_key(a, der, &err);
        if (key == NULL) {
            testing_t_fatalf_v(t, "#%d: failed to decode EC private key: %s", i,
                               ERRTEXT(a, err));
            break;
        }
        Slice serialized = x509_marshal_ec_private_key(a, key, &err);
        if (BURROW_FAILED(err)) {
            testing_t_fatalf_v(t, "#%d: failed to encode EC private key: %s", i,
                               ERRTEXT(a, err));
            break;
        }
        bool matches = bytes_equal(serialized, der);
        if (matches != ec_key_test_reserialize[i])
            testing_t_fatalf_v(t,
                               "#%d: when serializing key: matches=%t, should match=%t",
                               i, matches, ec_key_test_reserialize[i]);
    }
    arena_free(&ar);
}

static void TestECMismatchKeyFormat(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const struct {
        const char *hex_key, *error_contains;
    } tests[] = {
        {hex_ec_test_pkcs8_key, "use ParsePKCS8PrivateKey instead"},
        {hex_ec_test_pkcs1_key, "use ParsePKCS1PrivateKey instead"},
    };
    for (Int i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        EcdsaPrivateKey *key =
            x509_parse_ec_private_key(a, hexb(a, tests[i].hex_key), &err);
        CHECK(key == NULL);
        if (!contains(err, tests[i].error_contains))
            testing_t_errorf_v(t, "#%d: expected error containing %q, got %s", i,
                               tests[i].error_contains, ERRTEXT(a, err));
    }
    arena_free(&ar);
}

/* --------------------------------------------------------------- pkcs8 */

static void TestPKCS8(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const struct {
        const char *name, *key_hex;
        const Type *key_type;
        int curve; /* 0 for none, else 224, 256, 384 or 521 */
    } tests[] = {
        {"RSA private key", pkcs8_rsa_private_key_hex, TYPE_RSA_PRIVATE_KEY, 0},
        {"P-224 private key", pkcs8_p224_private_key_hex, TYPE_ECDSA_PRIVATE_KEY, 224},
        {"P-256 private key", pkcs8_p256_private_key_hex, TYPE_ECDSA_PRIVATE_KEY, 256},
        {"P-384 private key", pkcs8_p384_private_key_hex, TYPE_ECDSA_PRIVATE_KEY, 384},
        {"P-521 private key", pkcs8_p521_private_key_hex, TYPE_ECDSA_PRIVATE_KEY, 521},
        {"Ed25519 private key", pkcs8_ed25519_private_key_hex, TYPE_ED25519_PRIVATE_KEY,
         0},
        {"X25519 private key", pkcs8_x25519_private_key_hex, TYPE_ECDH_PRIVATE_KEY, 0},
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        Slice der = hexb(a, tests[i].key_hex);
        Error err = BURROW_NO_ERROR;
        Any key = x509_parse_pkcs8_private_key(a, der, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: failed to decode PKCS#8: %s", tests[i].name,
                               ERRTEXT(a, err));
            continue;
        }
        if (key.t != tests[i].key_type) {
            testing_t_errorf_v(t, "%s: decoded PKCS#8 returned unexpected key type",
                               tests[i].name);
            continue;
        }
        EllipticCurve want = {0};
        switch (tests[i].curve) {
        case 224:
            want = elliptic_p224();
            break;
        case 256:
            want = elliptic_p256();
            break;
        case 384:
            want = elliptic_p384();
            break;
        case 521:
            want = elliptic_p521();
            break;
        default:
            break;
        }
        if (key.t == TYPE_ECDSA_PRIVATE_KEY) {
            const EcdsaPrivateKey *k = key.data;
            if (k->public_key.curve.vt != want.vt ||
                k->public_key.curve.data != want.data) {
                testing_t_errorf_v(t, "%s: decoded PKCS#8 returned unexpected curve",
                                   tests[i].name);
                continue;
            }
        }
        Slice reserialised = x509_marshal_pkcs8_private_key(a, key, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "%s: failed to marshal into PKCS#8: %s",
                               tests[i].name, ERRTEXT(a, err));
            continue;
        }
        if (!bytes_equal(der, reserialised)) {
            testing_t_errorf_v(
                t, "%s: marshaled PKCS#8 didn't match original: got %x, want %x",
                tests[i].name, reserialised, der);
            continue;
        }

        if (key.t == TYPE_ECDSA_PRIVATE_KEY) {
            EcdhPrivateKey *ecdh_key = ecdsa_private_key_ecdh(key.data, a, &err);
            if (ecdh_key == NULL) {
                if (tests[i].curve != 224)
                    testing_t_errorf_v(t, "%s: failed to convert to ecdh: %s",
                                       tests[i].name, ERRTEXT(a, err));
                continue;
            }
            reserialised = x509_marshal_pkcs8_private_key(
                a, BURROW_ANY(TYPE_ECDH_PRIVATE_KEY, ecdh_key), &err);
            if (BURROW_FAILED(err)) {
                testing_t_errorf_v(t, "%s: failed to marshal into PKCS#8: %s",
                                   tests[i].name, ERRTEXT(a, err));
                continue;
            }
            if (!bytes_equal(der, reserialised))
                testing_t_errorf_v(
                    t,
                    "%s: marshaled PKCS#8 didn't match original: got %x, "
                    "want %x",
                    tests[i].name, reserialised, der);
        }
    }
    arena_free(&ar);
}

static void TestPKCS8MismatchKeyFormat(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const struct {
        const char *hex_key, *error_contains;
    } tests[] = {
        {hex_pkcs8_test_ec_key, "use ParseECPrivateKey instead"},
        {hex_pkcs8_test_pkcs1_key, "use ParsePKCS1PrivateKey instead"},
    };
    for (Int i = 0; i < 2; i++) {
        Error err = BURROW_NO_ERROR;
        Any key = x509_parse_pkcs8_private_key(a, hexb(a, tests[i].hex_key), &err);
        CHECK(key.t == NULL && key.data == NULL);
        if (!contains(err, tests[i].error_contains))
            testing_t_errorf_v(t, "#%d: expected error containing %q, got %s", i,
                               tests[i].error_contains, ERRTEXT(a, err));
    }
    arena_free(&ar);
}

/* --------------------------------------------------------- pem_decrypt */

static Slice b64(Alloc *a, const char *s) {
    Error err = BURROW_NO_ERROR;
    Slice b =
        base64_encoding_decode_string(base64_std_encoding, a, str_from_cstr(s), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_test: bad base64"));
    return b;
}

static void TestDecrypt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof pem_test_data / sizeof pem_test_data[0]; i++) {
        Slice rest;
        PemBlock *block = pem_decode(a, cbytes(pem_test_data[i].pem_data), &rest);
        CHECK(block != NULL);
        if (block == NULL)
            continue;
        if (rest.len > 0)
            testing_t_errorf_v(t, "extra data");
        Error err = BURROW_NO_ERROR;
        Slice der =
            x509_decrypt_pem_block(a, block, cbytes(pem_test_data[i].password), &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test %d: decrypt failed: %s", (int)i,
                               ERRTEXT(a, err));
            continue;
        }
        if (x509_parse_pkcs1_private_key(a, der, &err) == NULL)
            testing_t_errorf_v(t, "test %d: invalid private key: %s", (int)i,
                               ERRTEXT(a, err));
        if (!bytes_equal(der, b64(a, pem_test_data[i].plain_der)))
            testing_t_errorf_v(t, "test %d: data mismatch", (int)i);
    }
    arena_free(&ar);
}

static void TestEncrypt(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (size_t i = 0; i < sizeof pem_test_data / sizeof pem_test_data[0]; i++) {
        Slice plain_der = b64(a, pem_test_data[i].plain_der);
        Slice password = cbytes("kremvax1");
        Error err = BURROW_NO_ERROR;
        PemBlock *block =
            x509_encrypt_pem_block(a, crypto_rand_reader, BURROW_S("RSA PRIVATE KEY"),
                                   plain_der, password, pem_test_data[i].kind, &err);
        if (block == NULL) {
            testing_t_errorf_v(t, "test %d: encrypt: %s", (int)i, ERRTEXT(a, err));
            continue;
        }
        if (!x509_is_encrypted_pem_block(block))
            testing_t_errorf_v(t, "PEM block does not appear to be encrypted");
        CHECK_STR_EQ(cz(a, block->type), "RSA PRIVATE KEY");
        Str key = BURROW_S("Proc-Type"), val = {0};
        if (!map_get2(block->headers, &key, &val) ||
            !str_eq(val, BURROW_S("4,ENCRYPTED")))
            testing_t_errorf_v(t, "block does not have correct Proc-Type header");
        Slice der = x509_decrypt_pem_block(a, block, password, &err);
        if (BURROW_FAILED(err)) {
            testing_t_errorf_v(t, "test %d: decrypt: %s", (int)i, ERRTEXT(a, err));
            continue;
        }
        if (!bytes_equal(der, plain_der))
            testing_t_errorf_v(t, "test %d: data mismatch", (int)i);
    }

    /* A nil reader is crypto_rand_reader. */
    Error err = BURROW_NO_ERROR;
    PemBlock *block =
        x509_encrypt_pem_block(a, (IoReader){0}, BURROW_S("X"), cbytes("data"),
                               cbytes("pw"), X509_PEM_CIPHER_AES256, &err);
    CHECK(block != NULL && BURROW_OK(err));
    if (block != NULL) {
        Slice der = x509_decrypt_pem_block(a, block, cbytes("pw"), &err);
        CHECK(BURROW_OK(err) && bytes_equal(der, cbytes("data")));
    }
    arena_free(&ar);
}

static void TestIncompleteBlock(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Slice rest;
    PemBlock *block = pem_decode(a, cbytes(incomplete_block_pem), &rest);
    CHECK(block != NULL);
    if (block != NULL) {
        Error err = BURROW_NO_ERROR;
        (void)x509_decrypt_pem_block(a, block, cbytes("foo"), &err);
        if (!BURROW_FAILED(err))
            testing_t_fatalf_v(t, "Bad PEM data decrypted successfully");
        else if (!contains(err, "block size"))
            testing_t_fatalf_v(t, "Expected error containing %q but got: %q",
                               "block size", ERRTEXT(a, err));
    }
    arena_free(&ar);
}

/* ----------------------------------------------------------------- oid */

typedef struct OidTest {
    Byte raw[16];
    Int raw_len;
    bool valid;
    const char *str;
    uint64_t ints[4];
    Int ints_len; /* 0 for nil */
} OidTest;

static const OidTest oid_tests[] = {
    {{0}, 0, false, "", {0}, 0},
    {{0x80, 0x01}, 2, false, "", {0}, 0},
    {{0x01, 0x80, 0x01}, 3, false, "", {0}, 0},

    {{1, 2, 3}, 3, true, "0.1.2.3", {0, 1, 2, 3}, 4},
    {{41, 2, 3}, 3, true, "1.1.2.3", {1, 1, 2, 3}, 4},
    {{86, 2, 3}, 3, true, "2.6.2.3", {2, 6, 2, 3}, 4},

    {{41, 255, 255, 255, 127}, 5, true, "1.1.268435455", {1, 1, 268435455}, 3},
    {{41, 0x87, 255, 255, 255, 127}, 6, true, "1.1.2147483647", {1, 1, 2147483647}, 3},
    {{41, 255, 255, 255, 255, 127}, 6, true, "1.1.34359738367", {1, 1, 34359738367}, 3},
    {{42, 255, 255, 255, 255, 255, 255, 255, 255, 127},
     10,
     true,
     "1.2.9223372036854775807",
     {1, 2, 9223372036854775807U},
     3},
    {{43, 0x81, 255, 255, 255, 255, 255, 255, 255, 255, 127},
     11,
     true,
     "1.3.18446744073709551615",
     {1, 3, 18446744073709551615U},
     3},
    {{44, 0x83, 255, 255, 255, 255, 255, 255, 255, 255, 127},
     11,
     true,
     "1.4.36893488147419103231",
     {0},
     0},
    {{85, 255, 255, 255, 255, 255, 255, 255, 255, 255, 127},
     11,
     true,
     "2.5.1180591620717411303423",
     {0},
     0},
    {{85, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 127},
     13,
     true,
     "2.5.19342813113834066795298815",
     {0},
     0},

    {{255, 255, 255, 127}, 4, true, "2.268435375", {2, 268435375}, 2},
    {{0x87, 255, 255, 255, 127}, 5, true, "2.2147483567", {2, 2147483567}, 2},
    {{255, 127}, 2, true, "2.16303", {2, 16303}, 2},
    {{255, 255, 255, 255, 127}, 5, true, "2.34359738287", {2, 34359738287}, 2},
    {{255, 255, 255, 255, 255, 255, 255, 255, 127},
     9,
     true,
     "2.9223372036854775727",
     {2, 9223372036854775727U},
     2},
    {{0x81, 255, 255, 255, 255, 255, 255, 255, 255, 127},
     10,
     true,
     "2.18446744073709551535",
     {2, 18446744073709551535U},
     2},
    {{0x83, 255, 255, 255, 255, 255, 255, 255, 255, 127},
     10,
     true,
     "2.36893488147419103151",
     {0},
     0},
    {{255, 255, 255, 255, 255, 255, 255, 255, 255, 127},
     10,
     true,
     "2.1180591620717411303343",
     {0},
     0},
    {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 127},
     12,
     true,
     "2.19342813113834066795298735",
     {0},
     0},

    {{41, 0x80 | 66, 0x80 | 44, 0x80 | 11, 33},
     5,
     true,
     "1.1.139134369",
     {1, 1, 139134369},
     3},
    {{0x80 | 66, 0x80 | 44, 0x80 | 11, 33}, 4, true, "2.139134289", {2, 139134289}, 2},
};

#define NOID_TESTS ((Int)(sizeof oid_tests / sizeof oid_tests[0]))

static Slice raw_of(const OidTest *v) {
    return (Slice){(void *)(uintptr_t)v->raw, v->raw_len, v->raw_len, TYPE_BYTE};
}

static Slice u64s(const uint64_t *v, Int n) {
    return (Slice){(void *)(uintptr_t)v, n, n, TYPE_UINT64};
}

static X509OID must_oid(Alloc *a, const uint64_t *v, Int n) {
    Error err = BURROW_NO_ERROR;
    X509OID oid = x509_oid_from_ints(a, u64s(v, n), &err);
    if (BURROW_FAILED(err))
        panic_str(BURROW_S("x509_test: bad OID"));
    return oid;
}

#define MUST_OID(a, ...)                                                               \
    must_oid((a), (const uint64_t[]){__VA_ARGS__},                                     \
             (Int)(sizeof((const uint64_t[]){__VA_ARGS__}) / sizeof(uint64_t)))

static void TestOID(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NOID_TESTS; i++) {
        const OidTest *v = &oid_tests[i];
        X509OID oid = {{0}};
        Error err = x509_oid_unmarshal_binary(&oid, a, raw_of(v));
        bool ok = BURROW_OK(err);
        if (ok != v->valid) {
            testing_t_errorf_v(t, "#%d: newOIDFromDER = %t; want %t", i, ok, v->valid);
            continue;
        }
        if (!ok)
            continue;
        CHECK_STR_EQ(cz(a, x509_oid_string(oid, a)), v->str);

        /* toASN1OID gives the arcs when they all fit an int32. */
        Int arcs[4];
        bool fits = v->ints_len > 0;
        for (Int j = 0; j < v->ints_len; j++) {
            if (v->ints[j] > INT32_MAX)
                fits = false;
            arcs[j] = (Int)v->ints[j];
        }
        if (fits) {
            Asn1ObjectIdentifier want = {arcs, v->ints_len, v->ints_len, TYPE_INT};
            if (!x509_oid_equal_asn1_oid(oid, want))
                testing_t_errorf_v(t, "#%d: (%s).EqualASN1OID = false, want true", i,
                                   v->str);
        }

        if (v->ints_len > 0) {
            X509OID oid2 = x509_oid_from_ints(a, u64s(v->ints, v->ints_len), &err);
            if (BURROW_FAILED(err))
                testing_t_errorf_v(t, "#%d: OIDFromInts: %s", i, ERRTEXT(a, err));
            else if (!x509_oid_equal(oid2, oid))
                testing_t_errorf_v(t, "#%d: OIDFromInts gave %s, want %s", i,
                                   cz(a, x509_oid_string(oid2, a)), v->str);
        }
    }
    arena_free(&ar);
}

static void TestInvalidOID(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const struct {
        const char *str;
        uint64_t ints[3];
        Int n;
    } cases[] = {
        {"", {0}, 0},         {"1", {1}, 1},
        {"3", {3}, 1},        {"3.100.200", {3, 100, 200}, 3},
        {"1.81", {1, 81}, 2}, {"1.81.200", {1, 81, 200}, 3},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Error err = BURROW_NO_ERROR;
        (void)x509_oid_from_ints(a, u64s(cases[i].ints, cases[i].n), &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "OIDFromInts(%s) succeeded", cases[i].str);
        err = BURROW_NO_ERROR;
        (void)x509_parse_oid(a, str_from_cstr(cases[i].str), &err);
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "ParseOID(%q) succeeded", cases[i].str);
        X509OID oid3 = {{0}};
        err = x509_oid_unmarshal_text(&oid3, a, cbytes(cases[i].str));
        if (!BURROW_FAILED(err))
            testing_t_errorf_v(t, "UnmarshalText(%q) succeeded", cases[i].str);
    }
    arena_free(&ar);
}

static void TestOIDEqual(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const X509OID zero = {{0}};
    const struct {
        X509OID oid, oid2;
        bool eq;
    } cases[] = {
        {MUST_OID(a, 1, 2, 3), MUST_OID(a, 1, 2, 3), true},
        {MUST_OID(a, 1, 2, 3), MUST_OID(a, 1, 2, 4), false},
        {MUST_OID(a, 1, 2, 3), MUST_OID(a, 1, 2, 3, 4), false},
        {MUST_OID(a, 2, 33, 22), MUST_OID(a, 2, 33, 23), false},
        {zero, zero, true},
        {zero, MUST_OID(a, 2, 33, 23), false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        if (x509_oid_equal(cases[i].oid, cases[i].oid2) != cases[i].eq)
            testing_t_errorf_v(t, "#%d: Equal = %t, want %t", (int)i, !cases[i].eq,
                               cases[i].eq);
    arena_free(&ar);
}

static bool is_invalid_oid(Error err) {
    return err.vt == burrow__x509_err_invalid_oid.vt &&
           err.data == burrow__x509_err_invalid_oid.data;
}

/* One case of TestOIDMarshal: in parses to out, or fails with errInvalidOID
 * when out is NULL. */
static void oid_marshal_case(TestingT *t, Alloc *a, const char *in,
                             const X509OID *out) {
    Error err = BURROW_NO_ERROR;
    X509OID o = x509_parse_oid(a, str_from_cstr(in), &err);
    if (out == NULL ? !is_invalid_oid(err) : BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "ParseOID(%q) = %v; want %s", in, err,
                           out == NULL ? "invalid oid" : "nil");
        return;
    }
    X509OID o2 = {{0}};
    err = x509_oid_unmarshal_text(&o2, a, cbytes(in));
    if (out == NULL ? !is_invalid_oid(err) : BURROW_FAILED(err)) {
        testing_t_errorf_v(t, "UnmarshalText(%q) = %v; want %s", in, err,
                           out == NULL ? "invalid oid" : "nil");
        return;
    }
    if (out == NULL)
        return;
    if (!x509_oid_equal(o, *out)) {
        testing_t_errorf_v(t, "ParseOID(%q) = %s", in, cz(a, x509_oid_string(o, a)));
        return;
    }
    if (!x509_oid_equal(o2, *out)) {
        testing_t_errorf_v(t, "UnmarshalText(%q) = %s", in,
                           cz(a, x509_oid_string(o2, a)));
        return;
    }

    Slice marshalled = x509_oid_marshal_text(o, a, &err);
    if (BURROW_FAILED(err) || !bytes_equal(marshalled, cbytes(in))) {
        testing_t_errorf_v(t, "MarshalText(%q) = %q", in, marshalled);
        return;
    }
    Slice text_append = slice_make(a, TYPE_BYTE, 4, 4);
    text_append = x509_oid_append_text(o, a, text_append, &err);
    text_append = slice_sub(text_append, 4, text_append.len);
    if (BURROW_FAILED(err) || !bytes_equal(text_append, cbytes(in))) {
        testing_t_errorf_v(t, "AppendText(%q) = %q", in, text_append);
        return;
    }

    Slice binary = x509_oid_marshal_binary(o, a, &err);
    CHECK(BURROW_OK(err));
    X509OID o3 = {{0}};
    err = x509_oid_unmarshal_binary(&o3, a, binary);
    CHECK(BURROW_OK(err));
    if (!x509_oid_equal(o3, *out)) {
        testing_t_errorf_v(t, "UnmarshalBinary(MarshalBinary(%q)) differs", in);
        return;
    }
    Slice binary_append = slice_make(a, TYPE_BYTE, 4, 4);
    binary_append = x509_oid_append_binary(o, a, binary_append, &err);
    binary_append = slice_sub(binary_append, 4, binary_append.len);
    CHECK(BURROW_OK(err));
    X509OID o4 = {{0}};
    err = x509_oid_unmarshal_binary(&o4, a, binary_append);
    CHECK(BURROW_OK(err));
    if (!x509_oid_equal(o4, *out))
        testing_t_errorf_v(t, "UnmarshalBinary(AppendBinary(%q)) differs", in);
}

static void TestOIDMarshal(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const char *const invalid[] = {
        "",         "0",         "1",     ".1",   ".1.",  "1.",   "1..",      "1.2.",
        "1.2.333.", "1.2.333..", "1.2..", "+1.2", "-1.2", "1.-2", "1.2.+333",
    };
    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++)
        oid_marshal_case(t, a, invalid[i], NULL);
    for (Int i = 0; i < NOID_TESTS; i++) {
        X509OID oid = {{0}};
        if (BURROW_FAILED(x509_oid_unmarshal_binary(&oid, a, raw_of(&oid_tests[i]))))
            continue;
        oid_marshal_case(t, a, oid_tests[i].str, &oid);
    }
    arena_free(&ar);
}

static void TestOIDEqualASN1OID(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    const X509OID zero = {{0}};
    const Int max = INT32_MAX;
    const Int max_plus_one = (Int)INT32_MAX + 1;
    const struct {
        X509OID oid;
        Int asn1[3];
        Int n;
        bool eq;
    } cases[] = {
        {MUST_OID(a, 1, 2, 3), {1, 2, 3}, 3, true},
        {MUST_OID(a, 1, 2, 3), {1, 2, 4}, 3, false},
        {MUST_OID(a, 1, 2, 3), {1, 2, 3}, 2, false}, /* {1, 2, 3, 4} below */
        {MUST_OID(a, 1, 33, 22), {1, 33, 23}, 3, false},
        {MUST_OID(a, 1, 33, 23), {1, 33, 22}, 3, false},
        {MUST_OID(a, 1, 33, 127), {1, 33, 127}, 3, true},
        {MUST_OID(a, 1, 33, 128), {1, 33, 127}, 3, false},
        {MUST_OID(a, 1, 33, 128), {1, 33, 128}, 3, true},
        {MUST_OID(a, 1, 33, 129), {1, 33, 129}, 3, true},
        {MUST_OID(a, 1, 33, 128), {1, 33, 129}, 3, false},
        {MUST_OID(a, 1, 33, 129), {1, 33, 128}, 3, false},
        {MUST_OID(a, 1, 33, 255), {1, 33, 255}, 3, true},
        {MUST_OID(a, 1, 33, 256), {1, 33, 256}, 3, true},
        {MUST_OID(a, 2, 33, 257), {2, 33, 256}, 3, false},
        {MUST_OID(a, 2, 33, 256), {2, 33, 257}, 3, false},

        {MUST_OID(a, 1, 33), {1, 33, max}, 3, false},
        {MUST_OID(a, 1, 33, INT32_MAX), {1, 33}, 2, false},
        {MUST_OID(a, 1, 33, INT32_MAX), {1, 33, max}, 3, true},
        {MUST_OID(a, 1, 33, (uint64_t)INT32_MAX + 1), {1, 33, max_plus_one}, 3, false},

        {MUST_OID(a, 1, 33, 256), {0}, 0, false},
        {zero, {1, 33, 256}, 3, false},
        {zero, {0}, 0, false},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        Asn1ObjectIdentifier o = {(void *)(uintptr_t)cases[i].asn1, cases[i].n,
                                  cases[i].n, TYPE_INT};
        if (i == 2) {
            static const Int four[] = {1, 2, 3, 4};
            o = (Asn1ObjectIdentifier){(void *)(uintptr_t)four, 4, 4, TYPE_INT};
        }
        if (x509_oid_equal_asn1_oid(cases[i].oid, o) != cases[i].eq)
            testing_t_errorf_v(t, "#%d: EqualASN1OID = %t, want %t", (int)i,
                               !cases[i].eq, cases[i].eq);
    }
    arena_free(&ar);
}

static void TestOIDUnmarshalBinary(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    for (Int i = 0; i < NOID_TESTS; i++) {
        X509OID o = {{0}};
        Error err = x509_oid_unmarshal_binary(&o, a, raw_of(&oid_tests[i]));
        bool want_err = !oid_tests[i].valid;
        if (want_err ? !is_invalid_oid(err) : BURROW_FAILED(err))
            testing_t_errorf_v(t, "#%d: UnmarshalBinary = %v", i, err);
    }
    arena_free(&ar);
}

static void TestOIDFromASN1OID(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const Int negative[] = {-1};
    Error err = BURROW_NO_ERROR;
    (void)x509_oid_from_asn1_oid(
        a, (Asn1ObjectIdentifier){(void *)(uintptr_t)negative, 1, 1, TYPE_INT}, &err);
    if (!BURROW_FAILED(err))
        testing_t_fatalf_v(t, "OIDFromASN1OID({-1}) succeeded");
    else
        CHECK_STR_EQ(ERRTEXT(a, err), "x509: OID components must be non-negative");

    static const Int short_oid[] = {1};
    static const Int first[] = {255, 1};
    static const Int second[] = {1, 255};
    const Asn1ObjectIdentifier bad[] = {
        {(void *)(uintptr_t)short_oid, 1, 1, TYPE_INT},
        {(void *)(uintptr_t)first, 2, 2, TYPE_INT},
        {(void *)(uintptr_t)second, 2, 2, TYPE_INT},
    };
    for (Int i = 0; i < 3; i++) {
        err = BURROW_NO_ERROR;
        (void)x509_oid_from_asn1_oid(a, bad[i], &err);
        if (!is_invalid_oid(err))
            testing_t_errorf_v(t, "#%d: OIDFromASN1OID = %v; want invalid oid", i, err);
    }
    arena_free(&ar);
}

static void TestEKUOIDS(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    static const char *const want[] = {
        "2.5.29.37.0",
        "1.3.6.1.5.5.7.3.1",
        "1.3.6.1.5.5.7.3.2",
        "1.3.6.1.5.5.7.3.3",
        "1.3.6.1.5.5.7.3.4",
        "1.3.6.1.5.5.7.3.5",
        "1.3.6.1.5.5.7.3.6",
        "1.3.6.1.5.5.7.3.7",
        "1.3.6.1.5.5.7.3.8",
        "1.3.6.1.5.5.7.3.9",
        "1.3.6.1.4.1.311.10.3.3",
        "2.16.840.1.113730.4.1",
        "1.3.6.1.4.1.311.2.1.22",
        "1.3.6.1.4.1.311.61.1.1",
    };
    for (Int i = 0; i < (Int)(sizeof want / sizeof want[0]); i++) {
        X509OID oid = x509_ext_key_usage_oid((X509ExtKeyUsage)i, a);
        CHECK_STR_EQ(cz(a, x509_oid_string(oid, a)), want[i]);
    }
    arena_free(&ar);
}

/* The String methods, from x509_string.go and x509.go. */
static void TestEnumStrings(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    CHECK_STR_EQ(cz(a, x509_signature_algorithm_string(X509_SHA256_WITH_RSAPSS, a)),
                 "SHA256-RSAPSS");
    CHECK_STR_EQ(cz(a, x509_signature_algorithm_string(X509_PURE_ED25519, a)),
                 "Ed25519");
    CHECK_STR_EQ(cz(a, x509_signature_algorithm_string(X509_MLDSA87, a)), "ML-DSA-87");
    CHECK_STR_EQ(
        cz(a, x509_signature_algorithm_string(X509_UNKNOWN_SIGNATURE_ALGORITHM, a)),
        "0");
    CHECK_STR_EQ(cz(a, x509_signature_algorithm_string(99, a)), "99");
    CHECK_STR_EQ(cz(a, x509_public_key_algorithm_string(X509_ECDSA, a)), "ECDSA");
    CHECK_STR_EQ(cz(a, x509_public_key_algorithm_string(X509_MLDSA, a)), "ML-DSA");
    CHECK_STR_EQ(cz(a, x509_public_key_algorithm_string(0, a)), "0");
    CHECK_STR_EQ(cz(a, x509_key_usage_string(X509_KEY_USAGE_CRL_SIGN, a)), "cRLSign");
    CHECK_STR_EQ(cz(a, x509_key_usage_string(3, a)), "KeyUsage(3)");
    CHECK_STR_EQ(cz(a, x509_ext_key_usage_string(X509_EXT_KEY_USAGE_OCSP_SIGNING, a)),
                 "OCSPSigning");
    CHECK_STR_EQ(cz(a, x509_ext_key_usage_string(-1, a)), "ExtKeyUsage(-1)");
    arena_free(&ar);
}

/* ---------------------------------------------------------- x509_test.go */

BURROW_PTR_TYPE(X509TestBigIntPtr, BigInt);

/* The anonymous struct TestParsePKCS1PrivateKey marshals: pkcs1PrivateKey
 * without the CRT values. */
#define X509_TEST_PARTIAL_KEY_FIELDS(F, T)                                             \
    F(T, Int, version, Version, "")                                                    \
    F(T, X509TestBigIntPtr, n, N, "")                                                  \
    F(T, Int, e, E, "")                                                                \
    F(T, X509TestBigIntPtr, d, D, "")                                                  \
    F(T, X509TestBigIntPtr, p, P, "")                                                  \
    F(T, X509TestBigIntPtr, q, Q, "")
BURROW_STRUCT_AS(X509TestPartialKey, X509_TEST_PARTIAL_KEY_FIELDS);

static void TestParsePKCS1PrivateKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    RsaPrivateKey *priv =
        x509_parse_pkcs1_private_key(a, pem_bytes(a, pem_private_key), &err);
    if (priv == NULL) {
        testing_t_errorf_v(t, "Failed to parse private key: %s", ERRTEXT(a, err));
        arena_free(&ar);
        return;
    }
    if (big_int_cmp(priv->public_key.n, dec(a, rsa_private_key_n)) != 0 ||
        priv->public_key.e != 65537 ||
        big_int_cmp(priv->d, dec(a, rsa_private_key_d)) != 0 ||
        big_int_cmp(priv->primes[0], dec(a, rsa_private_key_p)) != 0 ||
        big_int_cmp(priv->primes[1], dec(a, rsa_private_key_q)) != 0)
        testing_t_errorf_v(t, "parsed key differs from rsaPrivateKey");

    /* This private key includes an invalid prime that rsa.PrivateKey.Validate
     * should reject. */
    static const Byte data[] = "0\x16\x02\x00\x02\x02\x7f\x00\x02\x02"
                               "00\x02\x02"
                               "00\x02\x02\x00\x01\x02\x02\x7f\x00";
    if (x509_parse_pkcs1_private_key(a,
                                     (Slice){(void *)(uintptr_t)data, sizeof data - 1,
                                             sizeof data - 1, TYPE_BYTE},
                                     &err) != NULL)
        testing_t_errorf_v(t, "parsing invalid private key did not result in an error");

    /* A partial key without CRT values should still parse. */
    err = BURROW_NO_ERROR;
    X509TestPartialKey partial = {0,       priv->public_key.n, priv->public_key.e,
                                  priv->d, priv->primes[0],    priv->primes[1]};
    Slice b = asn1_marshal(a, BURROW_ANY(TYPE_OF(X509TestPartialKey), &partial), &err);
    CHECK(BURROW_OK(err));
    RsaPrivateKey *p2 = x509_parse_pkcs1_private_key(a, b, &err);
    if (p2 == NULL) {
        testing_t_fatalf_v(t, "parsing partial private key resulted in an error: %v",
                           err);
        arena_free(&ar);
        return;
    }
    if (!rsa_private_key_equal(p2, BURROW_ANY(TYPE_RSA_PRIVATE_KEY, priv)))
        testing_t_errorf_v(t, "partial private key did not match original key");
    if (p2->precomputed.dp == NULL || p2->precomputed.dq == NULL ||
        p2->precomputed.qinv == NULL)
        testing_t_errorf_v(t, "precomputed values not recomputed");
    arena_free(&ar);
}

static void TestPKCS1MismatchPublicKeyFormat(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    RsaPublicKey *pub =
        x509_parse_pkcs1_public_key(a, hexb(a, pkix_public_key_hex), &err);
    CHECK(pub == NULL);
    if (!contains(err, "use ParsePKIXPublicKey instead"))
        testing_t_errorf_v(t, "expected error containing %q, got %s",
                           "use ParsePKIXPublicKey instead", ERRTEXT(a, err));
    arena_free(&ar);
}

static void TestMarshalInvalidPublicKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    EcdsaPublicKey empty = {0};
    Error err = BURROW_NO_ERROR;
    (void)x509_marshal_pkix_public_key(a, BURROW_ANY(TYPE_ECDSA_PUBLIC_KEY, &empty),
                                       &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "expected error, got MarshalPKIXPublicKey success");
    EcdsaPublicKey bad = {elliptic_p256(), big_new_int(a, 1), big_new_int(a, 2)};
    err = BURROW_NO_ERROR;
    (void)x509_marshal_pkix_public_key(a, BURROW_ANY(TYPE_ECDSA_PUBLIC_KEY, &bad),
                                       &err);
    if (!BURROW_FAILED(err))
        testing_t_errorf_v(t, "expected error, got MarshalPKIXPublicKey success");
    arena_free(&ar);
}

static Any parse_pkix_round_trip(TestingT *t, Alloc *a, const char *pem) {
    Slice der = pem_bytes(a, pem);
    Error err = BURROW_NO_ERROR;
    Any pub = x509_parse_pkix_public_key(a, der, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "Failed to parse public key: %s", ERRTEXT(a, err));
        return pub;
    }
    Slice again = x509_marshal_pkix_public_key(a, pub, &err);
    if (BURROW_FAILED(err))
        testing_t_errorf_v(t, "Failed to marshal public key for the second time: %s",
                           ERRTEXT(a, err));
    else if (!bytes_equal(again, der))
        testing_t_errorf_v(
            t, "Reserialization of public key didn't match. got %x, want %x", again,
            der);
    return pub;
}

static void TestParsePKIXPublicKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Any pub = parse_pkix_round_trip(t, a, pem_public_key);
    if (pub.t != TYPE_RSA_PUBLIC_KEY)
        testing_t_errorf_v(
            t, "Value returned from ParsePKIXPublicKey was not an RSA public "
               "key");
    pub = parse_pkix_round_trip(t, a, pem_ed25519_key);
    if (pub.t != TYPE_ED25519_PUBLIC_KEY)
        testing_t_errorf_v(t,
                           "Value returned from ParsePKIXPublicKey was not an Ed25519 "
                           "public key");
    pub = parse_pkix_round_trip(t, a, pem_x25519_key);
    if (pub.t != TYPE_ECDH_PUBLIC_KEY ||
        ecdh_public_key_curve(pub.data) != ecdh_x25519())
        testing_t_errorf_v(t,
                           "Value returned from ParsePKIXPublicKey was not an X25519 "
                           "public key");
    arena_free(&ar);
}

static void TestPKIXMismatchPublicKeyFormat(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    Any pub = x509_parse_pkix_public_key(a, hexb(a, pkcs1_public_key_hex), &err);
    CHECK(pub.t == NULL);
    if (!contains(err, "use ParsePKCS1PublicKey instead"))
        testing_t_errorf_v(t, "expected error containing %q, got %s",
                           "use ParsePKCS1PublicKey instead", ERRTEXT(a, err));
    arena_free(&ar);
}

static void TestMarshalRSAPrivateKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    BigInt *primes[3] = {dec(a, marshal_rsa_p), dec(a, marshal_rsa_q),
                         dec(a, marshal_rsa_r)};
    RsaPrivateKey priv = {0};
    priv.public_key.n = dec(a, marshal_rsa_n);
    priv.public_key.e = 3;
    priv.d = dec(a, marshal_rsa_d);
    priv.primes = primes;
    priv.primes_len = 3;

    Slice der = x509_marshal_pkcs1_private_key(a, &priv);
    Error err = BURROW_NO_ERROR;
    RsaPrivateKey *priv2 = x509_parse_pkcs1_private_key(a, der, &err);
    if (priv2 == NULL) {
        testing_t_errorf_v(t, "error parsing serialized key: %s", ERRTEXT(a, err));
        arena_free(&ar);
        return;
    }
    if (big_int_cmp(priv.public_key.n, priv2->public_key.n) != 0 ||
        priv.public_key.e != priv2->public_key.e ||
        big_int_cmp(priv.d, priv2->d) != 0 || priv2->primes_len != 3 ||
        big_int_cmp(primes[0], priv2->primes[0]) != 0 ||
        big_int_cmp(primes[1], priv2->primes[1]) != 0 ||
        big_int_cmp(primes[2], priv2->primes[2]) != 0)
        testing_t_errorf_v(t, "wrong priv after a round trip");
    if (priv.precomputed.dp == NULL)
        testing_t_fatalf_v(t, "Precomputed.Dp is nil");
    arena_free(&ar);
}

static void TestMarshalRSAPrivateKeyInvalid(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    Error err = BURROW_NO_ERROR;
    RsaPrivateKey *test_rsa2048 =
        x509_parse_pkcs1_private_key(a, pem_bytes(a, rsa2048_pem), &err);
    if (test_rsa2048 == NULL) {
        testing_t_fatalf_v(t, "parsing the test key: %s", ERRTEXT(a, err));
        arena_free(&ar);
        return;
    }
    RsaPrivateKey broken = *test_rsa2048;
    broken.precomputed.dp = big_int_set_uint64(big_new_int(a, 0), 42);

    RsaPrivateKey *parsed = x509_parse_pkcs1_private_key(
        a, x509_marshal_pkcs1_private_key(a, &broken), &err);
    if (parsed != NULL)
        testing_t_errorf_v(t, "expected error, got success");

    burrow__x509_godebug_set("x509rsacrt=0");
    err = BURROW_NO_ERROR;
    parsed = x509_parse_pkcs1_private_key(a, x509_marshal_pkcs1_private_key(a, &broken),
                                          &err);
    burrow__x509_godebug_set(NULL);
    if (parsed == NULL) {
        testing_t_fatalf_v(t, "expected success, got error: %s", ERRTEXT(a, err));
    } else if (big_int_cmp(parsed->precomputed.dp, test_rsa2048->precomputed.dp) != 0) {
        testing_t_errorf_v(t, "Dp recomputation failed");
    }
    arena_free(&ar);
}

static void TestMarshalRSAPublicKey(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    RsaPublicKey pub = {dec(a, marshal_rsa_public_n), 3};
    Slice der = x509_marshal_pkcs1_public_key(a, &pub);
    Error err = BURROW_NO_ERROR;
    RsaPublicKey *pub2 = x509_parse_pkcs1_public_key(a, der, &err);
    if (pub2 == NULL)
        testing_t_errorf_v(t, "ParsePKCS1PublicKey: %s", ERRTEXT(a, err));
    else if (big_int_cmp(pub.n, pub2->n) != 0 || pub.e != pub2->e)
        testing_t_errorf_v(t, "ParsePKCS1PublicKey round trip differs");

    static const struct {
        Byte der[12];
        Int len;
        const char *expected_err_substr;
    } public_keys[] = {
        {{0x30, 6, 0x02, 1, 17, 0x02, 1, 3}, 8, NULL},
        {{0x30, 6, 0x02, 1, 0xff, 0x02, 1, 3}, 8, "zero or negative"},
        {{0x30, 6, 0x02, 1, 17, 0x02, 1, 0xff}, 8, "zero or negative"},
        {{0x30, 6, 0x02, 1, 17, 0x02, 1, 3, 1}, 9, "trailing data"},
        {{0x30, 9, 0x02, 1, 17, 0x02, 4, 0x7f, 0xff, 0xff, 0xff}, 11, NULL},
        {{0x30, 10, 0x02, 1, 17, 0x02, 5, 0x00, 0x80, 0x00, 0x00, 0x00}, 12, "large"},
    };
    for (Int i = 0; i < (Int)(sizeof public_keys / sizeof public_keys[0]); i++) {
        Slice d = {(void *)(uintptr_t)public_keys[i].der, public_keys[i].len,
                   public_keys[i].len, TYPE_BYTE};
        err = BURROW_NO_ERROR;
        RsaPublicKey *p = x509_parse_pkcs1_public_key(a, d, &err);
        if (public_keys[i].expected_err_substr != NULL) {
            if (p != NULL)
                testing_t_errorf_v(t, "#%d: unexpected success", i);
            else if (!contains(err, public_keys[i].expected_err_substr))
                testing_t_errorf_v(t, "#%d: expected error containing %q, got %s", i,
                                   public_keys[i].expected_err_substr, ERRTEXT(a, err));
        } else {
            if (p == NULL) {
                testing_t_errorf_v(t, "#%d: unexpected failure: %s", i,
                                   ERRTEXT(a, err));
                continue;
            }
            Slice re = x509_marshal_pkcs1_public_key(a, p);
            if (!bytes_equal(re, d))
                testing_t_errorf_v(t, "#%d: failed to reserialize: got %x, expected %x",
                                   i, re, d);
        }
    }
    arena_free(&ar);
}

static void test_mldsa(TestingT *t, Alloc *a, const char *private_pem,
                       const char *public_pem) {
    Error err = BURROW_NO_ERROR;
    Slice priv_der = pem_bytes(a, private_pem);
    Any priv = x509_parse_pkcs8_private_key(a, priv_der, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "ParsePKCS8PrivateKey failed: %s", ERRTEXT(a, err));
        return;
    }
    if (priv.t != TYPE_MLDSA_PRIVATE_KEY) {
        testing_t_fatalf_v(t, "ParsePKCS8PrivateKey returned wrong type");
        return;
    }
    const MldsaPrivateKey *sk = priv.data;
    CHECK_STR_EQ(cz(a, hex_encode_to_string(a, mldsa_private_key_bytes(sk, a))),
                 "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f");

    Slice got = x509_marshal_pkcs8_private_key(a, priv, &err);
    if (BURROW_FAILED(err) || !bytes_equal(got, priv_der))
        testing_t_fatalf_v(t,
                           "MarshalPKCS8PrivateKey did not return original DER bytes");

    Slice pub_der = pem_bytes(a, public_pem);
    Any pub = x509_parse_pkix_public_key(a, pub_der, &err);
    if (BURROW_FAILED(err)) {
        testing_t_fatalf_v(t, "ParsePKIXPublicKey failed: %s", ERRTEXT(a, err));
        return;
    }
    if (pub.t != TYPE_MLDSA_PUBLIC_KEY) {
        testing_t_fatalf_v(t, "ParsePKIXPublicKey returned wrong type");
        return;
    }
    const MldsaPublicKey *pk = mldsa_private_key_public_key(sk);
    if (!mldsa_public_key_equal(pk, pub))
        testing_t_fatalf_v(t,
                           "ParsePKIXPublicKey returned public key that does not match "
                           "private key");
    got = x509_marshal_pkix_public_key(a, pub, &err);
    if (BURROW_FAILED(err) || !bytes_equal(got, pub_der))
        testing_t_fatalf_v(t, "MarshalPKIXPublicKey did not return original DER bytes");
}

static void TestMLDSA(TestingT *t) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);
    test_mldsa(t, a, mldsa44_private_key_pem, mldsa44_public_key_pem);
    test_mldsa(t, a, mldsa65_private_key_pem, mldsa65_public_key_pem);
    test_mldsa(t, a, mldsa87_private_key_pem, mldsa87_public_key_pem);

    Error err = BURROW_NO_ERROR;
    Any key = x509_parse_pkcs8_private_key(
        a, pem_bytes(a, mldsa44_private_key_expanded_pem), &err);
    if (key.t != NULL || !contains(err, "supported"))
        testing_t_fatalf_v(t, "parsing an expanded ML-DSA-44 key: %v", err);
    err = BURROW_NO_ERROR;
    key = x509_parse_pkcs8_private_key(a, pem_bytes(a, mldsa44_private_key_both_pem),
                                       &err);
    if (key.t != NULL || !contains(err, "openssl"))
        testing_t_fatalf_v(t, "parsing an ML-DSA-44 key with both forms: %v", err);
    arena_free(&ar);
}

#define TESTS(X)                                                                       \
    X(TestParseECPrivateKey)                                                           \
    X(TestECMismatchKeyFormat)                                                         \
    X(TestPKCS8)                                                                       \
    X(TestPKCS8MismatchKeyFormat)                                                      \
    X(TestDecrypt)                                                                     \
    X(TestEncrypt)                                                                     \
    X(TestIncompleteBlock)                                                             \
    X(TestOID)                                                                         \
    X(TestInvalidOID)                                                                  \
    X(TestOIDEqual)                                                                    \
    X(TestOIDMarshal)                                                                  \
    X(TestOIDEqualASN1OID)                                                             \
    X(TestOIDUnmarshalBinary)                                                          \
    X(TestOIDFromASN1OID)                                                              \
    X(TestEKUOIDS)                                                                     \
    X(TestEnumStrings)                                                                 \
    X(TestParsePKCS1PrivateKey)                                                        \
    X(TestPKCS1MismatchPublicKeyFormat)                                                \
    X(TestMarshalInvalidPublicKey)                                                     \
    X(TestParsePKIXPublicKey)                                                          \
    X(TestPKIXMismatchPublicKeyFormat)                                                 \
    X(TestMarshalRSAPrivateKey)                                                        \
    X(TestMarshalRSAPrivateKeyInvalid)                                                 \
    X(TestMarshalRSAPublicKey)                                                         \
    X(TestMLDSA)

TESTING_MAIN(TESTS)
